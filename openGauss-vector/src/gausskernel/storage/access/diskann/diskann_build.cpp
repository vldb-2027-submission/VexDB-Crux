#include <vtl/holder>
#include <vtl/disk_container/freespace.hpp>

#include "postgres.h"
#include "catalog/index.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/diskann/disk_pq.h"
#include "access/diskann/partition.h"
#include "access/annvector/xlog/log_manager.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/module/timer.h"
#include "access/annvector/ann_utils.h"
#include "utils/fmgroids.h"
#include "catalog/pg_operator.h"

using namespace ann_helper;
using disk_container::DiskVector;
using disk_container::PlainStore;
using disk_container::FreeSpace;

static void EstimateMemUsage(DiskAnnBuildState *buildState)
{
    /* Not known yet */
    buildState->numPoints = 0;
    buildState->hasLargeData = false;
    /* Estimate how many points can be loaded in memory */
    buildState->maxWorkMem =
        ((uint64) u_sess->attr.attr_memory.maintenance_work_mem) * kb_to_bytes * DISKANN_WORK_MEM_PERCENTAGE;
    buildState->maxNumPointsInMem = 
        buildState->maxWorkMem / (buildState->dimensions * sizeof(float) +
                                  sizeof(DiskAnnVamanaNode) + sizeof(AnnNeighbors));
}

/*
 * Initialize the build state
 */
static void InitBuildState(DiskAnnBuildState *buildState, Relation heap, Relation index,
                           IndexInfo *indexInfo, ForkNumber forkNum)
{
    buildState->heap = heap;
    buildState->index = index;
    buildState->indexInfo = indexInfo;
    buildState->forkNum = forkNum;

    buildState->relTuples = 0;
    buildState->idxTuples = 0;

    auto *opts = (DiskAnnOptions *)index->rd_options;
    if (opts != NULL) {
        buildState->numThreads = opts->parallel_workers;
        buildState->isPQEnabled = opts->enable_quantization;
        buildState->enableSubGraph = opts->enable_subgraph;
    } else {
        buildState->numThreads = 0;
        buildState->isPQEnabled = false;
        buildState->enableSubGraph = false;
    }

    if (DiskAnnUseBTree(index)) {
        ereport(ERROR, (errmsg("Old Hybridann index is not supported, please upgrade your database")));
    }
    if (RelationGetDescr(index)->natts > 1) {
        buildState->use_inplace_attr_filter = true;
    } else {
        buildState->use_inplace_attr_filter = false;
    }

    buildState->procInfo = DiskAnnOptionalProcInfo(index, DISKANN_DISTANCE_PROC);
    Assert(buildState->procInfo);
    buildState->metric = get_func_metric(buildState->procInfo->fn_oid);
    buildState->collation = index->rd_indcollation[0];

    int32 dim = TupleDescAttr(RelationGetDescr(index), 0)->atttypmod;
    /* Require column to have dimensions to be indexed */
    if (dim < 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Vector column does not have dimensions")));
    }
    if (dim > DISKANN_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Vector column cannot have more than %d dimensions for diskann index",
                               DISKANN_MAX_DIM)));
    }
    buildState->dimensions = (uint32)dim;

    EstimateMemUsage(buildState);

    buildState->attrMetaBlkNo = InvalidBlockNumber;

    buildState->numShards = 1;
    buildState->samplingRate = 1;
    buildState->numCenters = DISKANN_MAX_NUM_PQ_CENTROIDS;
    buildState->numPQChunks = buildState->dimensions;

    buildState->pqPivotsMetaBlkNo = InvalidBlockNumber;
    buildState->pqCompressedMetaBlkNo = InvalidBlockNumber;

    buildState->pqPivots = NULL;
    buildState->pqChunkOffsets = NULL;
}

static void clear_mem_data(DiskAnnBuildState *buildState)
{
    buildState->data.clear();
    buildState->graph.clear();
    buildState->nodes.clear();
}

static void FlushVector(DiskAnnBuildState *buildState)
{
    Relation index = buildState->index;
    constexpr size_t block_size = 76'800'000;
    const size_t size = buildState->data.size();
    Assert(size % buildState->dimensions == 0);
    const size_t start_size = buildState->num_inserted * buildState->dimensions;
    float *data = buildState->data.data();

    auto preprocessor = ann_helper::get_vector_preprocess_func(buildState->metric);
    if (preprocessor) {
        for (float *fdata = data; fdata < buildState->data.end(); fdata += buildState->dimensions) {
            preprocessor(fdata, buildState->dimensions, fdata);
        }
    }

    for (size_t i = 0; i < size; i += block_size) {
        size_t block = std::min(size - i, block_size);
        vec_write(index->rd_smgr, (i + start_size) * sizeof(float), block * sizeof(float),
                  (const char *)(data + i), false);
    }

    buildState->data.clear();
    buildState->num_inserted += size / buildState->dimensions;
}

static void FlushNodes(DiskAnnBuildState *buildState)
{
    constexpr size_t block_size = 600'000;
    const size_t size = buildState->nodes.size();
    DiskVector<DiskAnnVamanaNode> nodes(buildState->index, buildState->nodeMetaBlkNo, false);
    for (size_t i = 0; i < size; i += block_size) {
        size_t block = std::min(size - i, block_size);
        nodes.push_back_n(buildState->nodes.data() + i, block);
    }
    buildState->nodes.clear();
    nodes.destroy();
}

struct CallbackData {
    DiskAnnBuildState *buildState;
    Holder<PlainStore> store;
    ann_helper::Timer *timer;
};

template <bool use_inplace_attr_filter>
static void PopulateCallback(Relation index, HeapTuple htup, Datum *values,
                             const bool *isnull, bool tupleIsAlive, void *data)
{
    ((CallbackData *)data)->timer->report_loop("Scanning heap");
    if (isnull[0] || !tupleIsAlive) {
        return;
    }

    DiskAnnBuildState *buildState = reinterpret_cast<CallbackData *>(data)->buildState;
    uint32 dim = buildState->dimensions;
    Datum vec_datum = values[0];
    auto key = PlainStore::invalid_key();
    if (use_inplace_attr_filter) {
        const_cast<bool *>(isnull)[0] = true;
        IndexTuple tuple = index_form_tuple(RelationGetDescr(index), values, isnull);
        Size size = IndexTupleSize(tuple);
        auto &store = reinterpret_cast<CallbackData *>(data)->store;
        key = store->put(tuple, size);
        pfree(tuple);
    }

    FloatVector *value = DatumGetFloatVector(vec_datum);
    Assert(((uint32) value->dim) == dim);
    if (buildState->numPoints >= buildState->maxNumPointsInMem
        && buildState->numPoints % buildState->maxNumPointsInMem == 0) {
        FlushVector(buildState);
        FlushNodes(buildState);
        buildState->hasLargeData = true;
    }
    buildState->data.push_back(value->x, &value->x[dim]);
    buildState->nodes.emplace_back(htup->t_self, key, diskann_node_flag::init_flag);
    ++buildState->numPoints;

    if ((Pointer)value != DatumGetPointer(vec_datum)) {
        pfree(value);
    }
}

static void PopulateData(DiskAnnBuildState *buildState)
{
    if (!buildState->heap) {
        return;
    }
    double reltuples = get_relstats_reltuples(buildState->heap);
    char indexName[NAMEDATALEN + 1];
	char partIndexName[NAMEDATALEN + 1];
    populate_index_partition_name(buildState->index, indexName, partIndexName);
    ann_helper::Timer timer(reltuples, 250'000, indexName, partIndexName);
    timer.set_stage("Scan Table");
    CallbackData data = { buildState, {}, &timer };
    
    /* Insert an extra point as place holder in order to set medoid at the head later */
    for (uint32 i = 0; i < buildState->dimensions; ++i) {
        buildState->data.push_back(0);
    }
    ItemPointerData item_ptr = {{0, 0}, 0};
    buildState->nodes.emplace_back(item_ptr, PlainStore::invalid_key(), diskann_node_flag::init_flag);
    create_vec_data(buildState->index, true);
    buildState->num_inserted = 0;
    if (buildState->use_inplace_attr_filter) {
        data.store.emplace(buildState->index, buildState->attrMetaBlkNo, false);
        buildState->relTuples =
            IndexBuildHeapScan(buildState->heap, buildState->index, buildState->indexInfo,
                                true, PopulateCallback<true>, (void *)&data, NULL);
        data.store->destroy();
    } else {
        buildState->relTuples =
            IndexBuildHeapScan(buildState->heap, buildState->index, buildState->indexInfo,
                                true, PopulateCallback<false>, (void *)&data, NULL);
    }
    if (buildState->numPoints > buildState->maxNumPointsInMem) {
        FlushVector(buildState);
        FlushNodes(buildState);
    }

    if (buildState->hasLargeData) {
        buildState->required_size = buildState->numPoints / DISKANN_WORK_MEM_PERCENTAGE *
            (buildState->dimensions * sizeof(float) + sizeof(DiskAnnVamanaNode) + sizeof(AnnNeighbors));
        char buf[64];
        print_size(size_t(u_sess->attr.attr_memory.maintenance_work_mem) * kb_to_bytes, buf);
        print_size(buildState->required_size, buf + 32);
        ereport(WARNING, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
            errmsg("Current maintenance_work_mem (%s) is not enough to build "
                    "a DiskANN index in memory, required memory is %s", buf, buf + 32)));
    }
    if (buildState->hasLargeData) {
        buildState->data.destroy();
        new (&buildState->data) decltype(buildState->data);
    }
    timer.report("Heap scan finished (%lu)", timer._nloop_count);
    timer.destroy();

    size_t max_training_size = 1000 * 1024 * kb_to_bytes / (buildState->dimensions * sizeof(float));
    if (max_training_size > DISKANN_MAX_PQ_TRAINING_SET_SIZE) {
        max_training_size = DISKANN_MAX_PQ_TRAINING_SET_SIZE;
    }
    if (buildState->numPoints > max_training_size) {
        buildState->samplingRate = max_training_size / (double)buildState->numPoints;
    } else {
        buildState->samplingRate = 1.0;
    }
}

static void CreateMetaPage(DiskAnnBuildState *buildState)
{
    Buffer buf = AnnNewBuffer(buildState->index, buildState->forkNum);
    Page page = BufferGetPage(buf);
    DiskAnnInitMeta(buf, page);
    DiskAnnMetaPageBase *metaPageBase = DiskAnnPageGetMeta(page);
    
    metaPageBase->magicNumber = DISKANN_MAGIC_NUMBER;
    metaPageBase->metric = buildState->metric;
    metaPageBase->dimensions = buildState->dimensions;

    metaPageBase->nodeMetaBlkNo = DiskVector<DiskAnnVamanaNode>::get_disk_vector(buildState->index, false);
    buildState->nodeMetaBlkNo = metaPageBase->nodeMetaBlkNo;

    metaPageBase->freespaceMetaBlkNo = FreeSpace<size_t>::get_freespace_meta(buildState->index, false);

    DiskAnnMetaPage *metaPage = (DiskAnnMetaPage *)(metaPageBase);

    metaPage->version = DISKANN_VERSION_ONE;
    metaPage->numCenters = buildState->numCenters;
    metaPage->numPQChunks = buildState->numPQChunks;

    metaPage->graphMetaBlkNo = DiskVector<AnnNeighbors>::get_disk_vector(buildState->index, false);
    buildState->graphMetaBlkNo = metaPage->graphMetaBlkNo;

    if (buildState->use_inplace_attr_filter) {
        buildState->attrMetaBlkNo =
            PlainStore::get_plain_store(buildState->index, false, buildState->forkNum);
    }
    metaPage->attrMetaBlkNo = buildState->attrMetaBlkNo;

    ((PageHeader) page)->pd_lower = ((char *) metaPage + sizeof(DiskAnnMetaPage)) - (char *) page;

    AnnCommitBuffer(buf);
}

static uint32 get_recommanded_nchunk(uint32 dim)
{
    if (dim <= 256) {
        return dim;
    }
    if (dim <= 1024) {
        return dim / 2;
    }
    if (dim <= 2048) {
        return dim / 3;
    }
    if (dim <= 4096) {
        return dim / 4;
    }
    return dim / 5;
}

static void GeneratePQData(DiskAnnBuildState *buildState)
{
    if (buildState->numPoints <= 0) {
        buildState->isPQEnabled = false;
    }
    if (!buildState->isPQEnabled) {
        return;
    }

    float *sampleData;
    uint32 sampleSize;
    gen_random_slice(buildState, sampleData, &sampleSize);
    buildState->numPQChunks = get_recommanded_nchunk(buildState->dimensions);

    INIT_TASK_RUNNER();
    if (buildState->numThreads > 1) {
        LAUNCH_CONSUMER(buildState->numThreads);
    }
    size_t num_centers = min_ndistinct_pivots(sampleData, sampleSize, buildState->numPQChunks, buildState->dimensions);
    if (num_centers < buildState->numCenters) {
        if (num_centers < DISKANN_MIN_NUM_PQ_CENTROIDS) {
            ereport(WARNING, (errmsg("Fail to generate quantinization pivots during index construction"), errdetail("N/A"),
                              errcause("Not enough distinct points to generate quantinization pivots."),
                              erraction("Set enable_quantinization to off or build with more various data.")));
            buildState->isPQEnabled = false;
            DESTROY_TASK_RUNNER();
            return;
        }
        buildState->numCenters = num_centers;
    }
    generate_pq_pivots(buildState, sampleData, sampleSize, DISKANN_NUM_KMEANS_REPS_PQ);
    pfree(sampleData);
    generate_pq_data_from_pivots(buildState);
    DESTROY_TASK_RUNNER();

    pfree_ext(buildState->pqPivots);
    pfree_ext(buildState->pqChunkOffsets);
}

static void populate_data_for_subgraph_build(DiskAnnBuildState *buildState, uint32_t shardIndex)
{
    uint32 dim = buildState->dimensions;

    tmpvector<float> &data = buildState->shardData[shardIndex];
    tmpvector<DiskAnnVamanaNode> &nodes = buildState->shardNodes[shardIndex];

    size_t size = data.size() / dim;
    Assert(size > 0);

    size_t medoid = DiskANNIndex::calculate_entry_point(data, buildState->metric, dim);
    buildState->shardMedoids[shardIndex] = medoid;

    float *point = (float *) palloc(dim * sizeof(float));

    data.get_n(medoid * dim, dim, point);
    buildState->data.push_back(&point[0], &point[dim]);

    DiskAnnVamanaNode tmp_node;
    tmp_node.heapTid = nodes[medoid].heapTid;
    tmp_node.flag = diskann_node_flag::init_flag;
    diskann_node_flag::set_frozen(tmp_node.flag);
    buildState->nodes.push_back(tmp_node);

    for (size_t i = 0; i < size; i++) {
        data.get_n(i * dim, dim, point);
        buildState->data.push_back(&point[0], &point[dim]);
        buildState->nodes.push_back(nodes[i]);
    }

    ++size;
    AnnNeighbors ngh = {0, 0, };
    for (size_t i = 0; i < size; i++) {
        buildState->graph.push_back(ngh);
    }

    pfree(point);
}

static bool BuildAndMergeSubGraphs(DiskAnnBuildState *buildState)
{
    constexpr uint32 k_base = 2;
    /* Calculate num of shards and shard into clusters with IDs only */
    if (!partition_with_ram_budget(buildState, k_base)) {
        constexpr size_t recommanded_nshard = 5ul;
        char buf[64];
        print_size(size_t(u_sess->attr.attr_memory.maintenance_work_mem) * kb_to_bytes, buf);
        print_size(buildState->required_size * k_base / recommanded_nshard, buf + 32);
        ereport(WARNING, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
            errmsg("Too many points to build sub-graphs, disable sub-graph building. "
                   "Please increase maintenance_work_mem to (%s) at least and rebuild "
                   "if you need faster index construction (current maintenance_work_mem setting is %s)",
                   buf + 32, buf)));
        return false;
    }

    ereport(NOTICE, (errmsg("Building the index in %d shards", buildState->numShards)));

    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);

    for (uint32 i = 0; i < buildState->numShards; i++) {
        ereport(NOTICE, (errmsg("Building sub-index-%d", i)));
        /* Generate shards of data */
        retrieve_shard_data_from_ids(buildState, i);
        /* Populate data for memory build */
        populate_data_for_subgraph_build(buildState, i);
        /* Initialize an sub-index */
        DiskANNIndex index(buildState->index, buildState->data, buildState->nodes, buildState->graph, &mutex);
        /* Build sub-graphs */
        index.build(buildState->data, buildState->nodes, buildState->graph, &mutex, buildState->numThreads);
        /* Collect sub-graphs */
        buildState->shardGraphs[i].push_back_n(buildState->graph.data(), buildState->graph.size());
        /* Clear for next shard building */
        clear_mem_data(buildState);
        index.destroy();
    }

    pthread_mutex_destroy(&mutex);

    ereport(NOTICE, (errmsg("Merging all the sub-indexes into a single index")));

    /* Merge sub-graphs */
    merge_shards(buildState);

    return true;
}

static size_t populate_data_for_mem_build(DiskAnnBuildState *buildState)
{
    uint32 dim = buildState->dimensions;

    uint32 size = buildState->data.size() / dim;
    Assert(size > 0);

    size_t medoid = DiskANNIndex::calculate_entry_point(buildState->data, buildState->metric, dim);

    /* Reset the first point of data */
    for (uint32 i = 0; i < dim; i++) {
        buildState->data.set(i, buildState->data[(medoid + 1ul) * dim + i]);
    }

    /* Reset the first point of nodes*/
    DiskAnnVamanaNode tmp_node;
    tmp_node.heapTid = buildState->nodes[medoid + 1ul].heapTid;
    tmp_node.flag = diskann_node_flag::init_flag;
    diskann_node_flag::set_frozen(tmp_node.flag);
    buildState->nodes.set(0, tmp_node);

    /* Initialize the graph */
    AnnNeighbors ngh = {0, 0, };
    for (uint32 i = 0; i < size; i++) {
        buildState->graph.push_back(ngh);
    }

    return medoid;
}

static void BuildIndexInMemory(DiskAnnBuildState *buildState)
{
    /* The data and nodes are already in memory, just populate the medoid and the graph */
    size_t medoid = populate_data_for_mem_build(buildState);

    /* Build index in memory in one shot */
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);

    DiskANNIndex index(buildState->index, buildState->data, buildState->nodes, buildState->graph, &mutex);
    index.build(buildState->data, buildState->nodes, buildState->graph, &mutex, buildState->numThreads);

    pthread_mutex_destroy(&mutex);

    /* Flush the memory graph into disk */
    index.flush(std::move(buildState->data), std::move(buildState->nodes),
                std::move(buildState->graph), medoid);

    index.destroy();
}

static void BuildDiskAnnIndex(DiskAnnBuildState *buildState)
{
    if (buildState->numPoints <= 0) {
        return;
    }
    if (buildState->heap != NULL && buildState->heap->rd_rel->relpersistence == RELPERSISTENCE_GLOBAL_TEMP && buildState->numThreads > 1) {
        ereport(NOTICE, (errmsg("switch off parallel mode for global temp table")));
        buildState->numThreads = 0;
    }
    if (!buildState->hasLargeData) {
        BuildIndexInMemory(buildState);
        return;
    }
    if (buildState->enableSubGraph && BuildAndMergeSubGraphs(buildState)) {
        DiskANNIndex index(buildState->index, false, false);
        index.flush(buildState->numPoints);
        index.destroy();
        return;
    }
    DiskANNIndex index(buildState->index, false, false);
    index.build(buildState->numPoints, buildState->numThreads);
    index.destroy();
}

static void UpdateMetaPage(DiskAnnBuildState *buildState)
{
    Buffer buf = AnnLoadBufferExtended(buildState->index, buildState->forkNum, DISKANN_METAPAGE_BLKNO);
    DiskAnnMetaPageBase *metaPageBase = DiskAnnPageGetMeta(BufferGetPage(buf));

    auto *metaPage = (DiskAnnMetaPage *)(metaPageBase);

    metaPage->numCenters = buildState->numCenters;
    metaPage->numPQChunks = buildState->numPQChunks;

    metaPage->medoid = 0; /* diskann index search always starts from location 0 where medoid saves */

    metaPage->pqPivotsMetaBlkNo = buildState->pqPivotsMetaBlkNo;
    metaPage->pqCompressedMetaBlkNo = buildState->pqCompressedMetaBlkNo;

    AnnCommitBuffer(buf);
}

/*
 * Free resources
 */
static void FreeBuildState(DiskAnnBuildState *buildState)
{
    optional_destroy(buildState->data);
    optional_destroy(buildState->nodes);
    optional_destroy(buildState->graph);

    optional_destroy(buildState->shardIDs);
    optional_destroy(buildState->shardData);
    optional_destroy(buildState->shardNodes);
    optional_destroy(buildState->shardGraphs);
    optional_destroy(buildState->shardMedoids);
}
/*
 * Build DiskANN index
 */
static void BuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
                       DiskAnnBuildState *buildState, ForkNumber forkNum)
{
    InitBuildState(buildState, heap, index, indexInfo, forkNum);
    CreateMetaPage(buildState);
    PopulateData(buildState);

    GeneratePQData(buildState);
    UpdateMetaPage(buildState);
    BuildDiskAnnIndex(buildState);

    LogManager logmgr(index);
    logmgr.log_build_index(forkNum, false);
    logmgr.log_build_vector(heap, index, buildState->dimensions, buildState->numPoints, false);
    logmgr.destroy();

    FreeBuildState(buildState);
}

/*
 * Build the index for a logged table
 */
IndexBuildResult *diskannbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    DiskAnnBuildState buildState;

    check_ann_attributes(index);
    BuildIndex(heap, index, indexInfo, &buildState, MAIN_FORKNUM);

    result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

    result->heap_tuples = buildState.relTuples;
    result->index_tuples = buildState.idxTuples;

    return result;
}

/*
 * Build the index for an unlogged table
 */
void diskannbuildempty_internal(Relation index)
{
    IndexInfo  *indexInfo = BuildIndexInfo(index);
    DiskAnnBuildState buildState;

    BuildIndex(NULL, index, indexInfo, &buildState, INIT_FORKNUM);
}

/*
 * Parse and validate the reloptions
 */
bytea *diskannoptions_internal(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(DiskAnnOptions, parallel_workers)},
        {"enable_quantization", RELOPT_TYPE_BOOL, offsetof(DiskAnnOptions, enable_quantization)},
        {"enable_subgraph", RELOPT_TYPE_BOOL, offsetof(DiskAnnOptions, enable_subgraph)},
        {"occlusion_factor", RELOPT_TYPE_REAL, offsetof(DiskAnnOptions, occlusion_factor)},
        {"fillfactor", RELOPT_TYPE_INT, offsetof(DiskAnnOptions, fillfactor)},
        {"m", RELOPT_TYPE_INT, offsetof(DiskAnnOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT, offsetof(DiskAnnOptions, ef_construction)},
        {"vec_index_magnitudes", RELOPT_TYPE_STRING, offsetof(DiskAnnOptions, vec_index_magnitudes_offset)},
        {"adaptive_relaxation", RELOPT_TYPE_STRING, offsetof(DiskAnnOptions, adaptive_relaxation)},
        {"subgraph_max_vectors_factor", RELOPT_TYPE_STRING, offsetof(DiskAnnOptions, subgraph_max_vectors_factor)},
    };

    int numOptions = 0;
    relopt_value *options = NULL;
    DiskAnnOptions *rdopts = NULL;

    options = parseRelOptions(reloptions, validate, RELOPT_KIND_DISKANN, &numOptions);

    if (numOptions == 0) {
        return NULL;
    }

    rdopts = (DiskAnnOptions *)allocateReloptStruct(sizeof(DiskAnnOptions), options, numOptions);

    fillRelOptions((void *) rdopts, sizeof(DiskAnnOptions), options, numOptions, validate, tab, lengthof(tab));

    return (bytea *) rdopts;

}
