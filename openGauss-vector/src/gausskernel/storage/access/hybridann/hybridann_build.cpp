#include "postgres.h"
#include "catalog/index.h"
#include "access/reloptions.h"
#include "access/hybridann/hybridann.h"
#include "access/hybridann/bplustree/disk_bplustree.h"

using namespace disk_container;

struct CallbackData {
    HybridAnnBuildState *buildState;
    ann_helper::Timer *timer;
};

static void EstimateMemUsage(HybridAnnBuildState *buildState)
{
    buildState->numPoints = 0;
    /* Estimate how many points can be loaded in memory */
    buildState->maxWorkMem =
        ((uint64) u_sess->attr.attr_memory.maintenance_work_mem) * kb_to_bytes * HYBRIDANN_WORK_MEM_PERCENTAGE;
    buildState->maxNumPointsInMem = buildState->maxWorkMem / (buildState->dimensions * sizeof(float));
}

/*
 * Initialize the build state
 */
static void InitBuildState(HybridAnnBuildState *buildState, Relation heap, Relation index,
                           IndexInfo *indexInfo, ForkNumber forkNum)
{
    buildState->heap = heap;
    buildState->index = index;
    buildState->indexInfo = indexInfo;
    buildState->forkNum = forkNum;

    buildState->relTuples = 0;
    buildState->idxTuples = 0;

    buildState->numThreads = hybridAnnGetNumParallel(index);

    if (RelationGetDescr(index)->natts <= 1) {
        ereport(ERROR, (errmsg("hybrid ann index must contain both vector and scalar columns")));
    }

    const char *input_index_magnitudes = hybridAnnGetVecIndexMagnitudes(index);
    StringParamExtractor::validate(input_index_magnitudes, max_index_magnitude_size);
    StringParamExtractor spe(input_index_magnitudes);
    spe.extract(buildState->indexMagnitudes);
    spe.destroy();

    buildState->sortState = NULL;
    buildState->procInfo = hybridAnnOptionalProcInfo(index, HYBRIDANN_DISTANCE_PROC);
    Assert(buildState->procInfo);
    buildState->metric = get_func_metric(buildState->procInfo->fn_oid);
    buildState->collation = index->rd_indcollation[0];

    int32 dim = TupleDescAttr(RelationGetDescr(index), 0)->atttypmod;
    /* Require column to have dimensions to be indexed */
    if (dim < 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Vector column does not have dimensions")));
    }
    if (dim > FLOATVECTOR_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Vector column cannot have more than %d dimensions for diskann index",
                               FLOATVECTOR_MAX_DIM)));
    }
    buildState->dimensions = (uint32)dim;
    buildState->m = hybridannGetM(index);
    buildState->ef_construction = hybridannGetEfConstruction(index);

    EstimateMemUsage(buildState);

    buildState->btreeMetaBlkNo = InvalidBlockNumber;
}


static void CreateMetaPage(HybridAnnBuildState *buildState)
{
    Buffer buf = AnnNewBuffer(buildState->index, buildState->forkNum);
    Page page = BufferGetPage(buf);
    hybridAnnInitMeta(buf, page);
    HybridAnnMetaPage *metaPage = HybridAnnPageGetMeta(page);
    
    metaPage->magicNumber = HYBRIDANN_MAGIC_NUMBER;
    metaPage->metric = buildState->metric;
    metaPage->dimensions = buildState->dimensions;

    metaPage->m = buildState->m;
    metaPage->ef_construction = buildState->ef_construction;

    metaPage->freespaceMetaBlkNo =
        FreeSpace<size_t>::get_freespace_meta(buildState->index, false);

    metaPage->version = HYBRID_VERSION_ONE;
    ((PageHeader)page)->pd_lower = ((char *)metaPage + sizeof(HybridAnnMetaPage)) - (char *)page;


    int64 graphMagitudeThreshold = hybridAnnGetGraphMagnitudeThreshold(buildState->index);
    metaPage->graphMagnitudeThreshold = graphMagitudeThreshold;
    IndexMagnitude index_magnitude(buildState->indexMagnitudes, graphMagitudeThreshold);
    metaPage->sizeIndexMagnitudes = index_magnitude.size();
    for (size_t i = 0; i < index_magnitude.size(); ++i) {
        metaPage->indexMagnitudes[i] = index_magnitude.get(i);
    }
    index_magnitude.destroy();
    
    AnnCommitBuffer(buf);
}

static void FlushVector(HybridAnnBuildState *buildState)
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

static void PopulateCallback(Relation index, HeapTuple htup, Datum *values,
                             const bool *isnull, bool tupleIsAlive, void *data)
{
    ((CallbackData *)data)->timer->report_loop("Scanning heap");
    if (isnull[0] || !tupleIsAlive) {
        return;
    }

    HybridAnnBuildState *buildState = reinterpret_cast<CallbackData *>(data)->buildState;
    uint32 dim = buildState->dimensions;
    Datum vec_datum = values[0];
    values[0] = UInt64GetDatum(buildState->numPoints);
    tuplesort_putindextuplevalues(buildState->sortState, index, &htup->t_self, values, isnull);

    FloatVector *value = DatumGetFloatVector(vec_datum);
    Assert(((uint32)value->dim) == dim);
    if (buildState->numPoints >= buildState->maxNumPointsInMem &&
        buildState->numPoints % buildState->maxNumPointsInMem == 0) {
        FlushVector(buildState);
    }
    buildState->data.push_back(value->x, &value->x[dim]);
    ++buildState->numPoints;

    if ((Pointer)value != DatumGetPointer(vec_datum)) {
        pfree(value);
    }
    values[0] = vec_datum;
}

static void PopulateData(HybridAnnBuildState *buildState)
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
    CallbackData data = { buildState, &timer };
    buildState->tupdesc = buildHybridTupDesc(buildState->index);
    buildState->sortState = tuplesort_begin_index_btree(
        buildState->index, false, u_sess->attr.attr_memory.work_mem, NULL, false, 0, buildState->tupdesc, 2);
    create_vec_data(buildState->index, true);
    buildState->num_inserted = 0;
    buildState->relTuples = IndexBuildHeapScan(buildState->heap, buildState->index,
        buildState->indexInfo, true, PopulateCallback, (void *)&data, NULL);
    FlushVector(buildState);
    buildState->data.destroy();

    timer.report("Heap scan finished (%lu)", timer._nloop_count);
    timer.destroy();
}


static void BuildDiskBPlusTreeIndex(Relation heap, Relation index, HybridAnnBuildState *buildState)
{
    TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(index));

    tuplesort_performsort(buildState->sortState);

    buildState->btreeMetaBlkNo = create_disk_btree_metapage(buildState->index, false, false);

    DiskBPlusTree btree(index, heap, buildState->btreeMetaBlkNo, false);
    auto *state = DiskBPlusTree::diskbt_pagestate(buildState->index, 0, btree);
    IndexTuple itup;
    char indexName[NAMEDATALEN + 1];
	char partIndexName[NAMEDATALEN + 1];
    populate_index_partition_name(buildState->index, indexName, partIndexName);
    ann_helper::Timer timer(buildState->numPoints, 1'000'000, indexName, partIndexName);
    timer.set_stage("Build Btree Index");
    while ((itup = tuplesort_getindextuple(buildState->sortState, true)) != NULL) {
        btree.buildadd(buildState->heap, state, reinterpret_cast<BTTupleData *>(itup), 0);
        timer.report_loop("Build Btree Index");
    }
    btree.uppershutdown(buildState->heap, state);
    btree.destroy();
    timer.report("Build Btree Index finished");
    timer.destroy();

    ExecDropSingleTupleTableSlot(slot);
}

static void BuildVectorIndexes(Relation index, HybridAnnBuildState *buildState)
{
    DiskBPlusTree btree(index, buildState->heap, buildState->btreeMetaBlkNo, false);
    btree.buildVectorIndexes();
    btree.destroy();
}


static void UpdateMetaPage(HybridAnnBuildState *buildState)
{
    Buffer buf = AnnLoadBufferExtended(buildState->index, buildState->forkNum, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *metaPage = HybridAnnPageGetMeta(BufferGetPage(buf));

    metaPage->BTMetaBlkNo = buildState->btreeMetaBlkNo;

    Buffer data_buf = AnnNewBuffer(buildState->index, buildState->forkNum);
    Page data_page = BufferGetPage(data_buf);
    HybridAnnInitPage(data_buf, data_page);
    reinterpret_cast<HybridAnnDataMeta>(PageGetContents(data_page))->num_vectors = buildState->numPoints;
    metaPage->dataMetaBlkNo = BufferGetBlockNumber(data_buf);
    ((PageHeader)data_page)->pd_lower = (char *)PageGetContents(data_page) + sizeof(HybridAnnDataMetaPage) - (char *)data_page;
    AnnCommitBuffer(data_buf);

    AnnCommitBuffer(buf);  
}

/*
 * Free resources
 */
static void FreeBuildState(HybridAnnBuildState *buildState)
{
    optional_destroy(buildState->data);

    if (buildState->sortState) {
        tuplesort_end(buildState->sortState);
        pfree(buildState->tupdesc);
    }

    optional_destroy(buildState->indexMagnitudes);
}
/*
 * Build HybridANN index
 */
static void BuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
                       HybridAnnBuildState *buildState, ForkNumber forkNum)
{
    InitBuildState(buildState, heap, index, indexInfo, forkNum);
    CreateMetaPage(buildState);
    PopulateData(buildState);

    BuildDiskBPlusTreeIndex(heap, index, buildState);
    UpdateMetaPage(buildState);
    BuildVectorIndexes(index, buildState);

    LogManager logmgr(index);
    logmgr.log_build_index(forkNum, true);
    logmgr.log_build_vector(heap, index, buildState->dimensions, buildState->numPoints, true);
    logmgr.destroy();

    FreeBuildState(buildState);
}
/*
 * Build the index for a logged table
 */
IndexBuildResult *hybridannbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    HybridAnnBuildState buildState;

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
void hybridannbuildempty_internal(Relation index)
{
    // IndexInfo  *indexInfo = BuildIndexInfo(index);
    // HybridAnnBuildState buildState;
    // BuildIndex(NULL, index, indexInfo, &buildState, INIT_FORKNUM);
}

/*
 * Parse and validate the reloptions
 */
bytea *hybridannoptions_internal(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(HybridAnnOptions, parallel_workers)},
        {"fillfactor", RELOPT_TYPE_INT, offsetof(HybridAnnOptions, fillfactor)},
        {"m", RELOPT_TYPE_INT, offsetof(HybridAnnOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT, offsetof(HybridAnnOptions, ef_construction)},
        {"vec_index_magnitudes", RELOPT_TYPE_STRING, offsetof(HybridAnnOptions, vec_index_magnitudes_offset)},
        {"graph_magnitude_threshold", RELOPT_TYPE_INT64, offsetof(HybridAnnOptions, graph_magnitude_threshold)}
    };

    int numOptions = 0;
    relopt_value *options = NULL;
    HybridAnnOptions *rdopts = NULL;

    options = parseRelOptions(reloptions, validate, RELOPT_KIND_HYBRIDANN, &numOptions);

    if (numOptions == 0) {
        return NULL;
    }

    rdopts = (HybridAnnOptions *)allocateReloptStruct(sizeof(HybridAnnOptions), options, numOptions);

    fillRelOptions((void *) rdopts, sizeof(HybridAnnOptions), options, numOptions, validate, tab, lengthof(tab));

    return (bytea *) rdopts;

}
