#include <math.h>
#include <random>
#include <thread>
#include <boost/lockfree/queue.hpp>

#include "postgres.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "lib/pairingheap.h"
#include "nodes/pg_list.h"
#include "storage/buf/bufmgr.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "knl/knl_guc.h"
#include "knl/knl_variable.h"
#include "storage/buf/bufpage.h"
#include "postmaster/bgworker.h"
#include "access/tableam.h"
#include "access/amapi.h"

#include "access/hnsw/hnsw.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/diskann/diskann_cache.h"
#include "access/diskann/math_utils.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/ivf.h"
#include "access/annvector/annkmeans.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/annkmeans.h"
#include "access/rabitq/rabitq.h"

constexpr size_t cqueue_capacity = 512ul;
using cqueue_ctx = CONTEXT_ALLOCATOR<HnswElement>;
using cqueue = boost::lockfree::queue<HnswElement,
    boost::lockfree::allocator<cqueue_ctx>,
    boost::lockfree::capacity<cqueue_capacity>>;

using namespace rabitq;

struct HnswLeader {
    int nparticipanttuplesorts;
    void *hnswshared;
};

struct HnswGraph {
    /* Graph state */
    slock_t lock;
    HnswElement head;
    double indtuples;

    /* Entry state */
    LWLock entryLock;
    LWLock entryWaitLock;
    HnswElement entryPoint;

    /* Allocations state */
    LWLock allocatorLock;
    long memoryUsed;
    long memoryTotal;

    /* Flushed state */
    LWLock flushLock;
    bool flushed;
    bool  warned;
    uint32 nworker;
    cqueue *elem_queue;

    /* cluster state, also used by PQ */
    bool cluster_pq;
    bool flushing_pq;
    bool graph_pq;
    bool pq_flushed;

    /* quantizer state */
    QuantizerType qt_type;
    ProductQuantizer pq;
};

struct HnswBuildState {
    /* Info */
    Relation heap;
    Relation index;
    IndexInfo *indexInfo;
    ForkNumber forkNum;
    BlockNumber metablkno;
    BlockNumber headblkno;

    /* Settings */
    QuantizerType qt_type;
    DistPrecisionType precision_type;
    int dimensions;
    int m;
    int efConstruction;
    int parallel_workers;
    int maintenance_work_mem;
    size_t vector_size;

    /* Cluster and PQ */
    int64 ncluster;
    bool cluster_pq;
    bool need_norm;
    HnswPQMetaInfo pq_metainfo;
    RaBitQParam rbq_param;
    bool concurrent_quant;
    ProductQuantizer *pq_quantizer;

    /* Statistics */
    double indtuples;
    double reltuples;

    /* Support functions */
    FmgrInfo *procinfo;
    Metric metric;
    Oid collation;
    ann_helper::distance_func func_ptr;
    ann_helper::vector_preprocess_func norm_func_ptr;

    /* Variables */
    HnswGraph graphData;
    HnswGraph *graph;
    double ml;
    int maxLevel;

    /* Memory */
    MemoryContext graphCtx;
    HnswAllocator allocator;
    ThreadId graphCtxcreator;
    bool graphCtxfreed;

    /* Parallel builds */
    HnswLeader *hnswleader;
    char *hnswarea;
    vector_pair_vector *vec_data;
    char indexName[NAMEDATALEN + 1];
    char partIndexName[NAMEDATALEN + 1];
};

struct BuildCallbackData {
    HnswBuildState *buildState;
    Relation heap;
    ann_helper::Timer *timer;
    char *bin_elem_buf;
};

struct HnswShared {
    /* Immutable state */
    Oid heaprelid;
    Oid indexrelid;
    Oid heappartid;
    Oid indexpartid;

    bool isconcurrent;
    ann_helper::Timer *timer;
    HnswBuildState *buildstate;

    /* Mutex for mutable state */
    slock_t mutex;

    /* Mutable state */
    int nparticipantsdone;
    double reltuples;

    /*
     * This variable-sized field must come last.
     */
    ParallelHeapScanDescData heapdesc;
};

static void init_pool(HnswBuildState *buildstate, Relation index)
{
    cqueue *temp = (cqueue *)MemoryContextAlloc(buildstate->graphCtx, sizeof(cqueue));
    new (temp) cqueue(cqueue_ctx(buildstate->graphCtx));
    HnswGraph *graph = buildstate->graph;
    HnswPQMetaInfo &pq_metainfo = buildstate->pq_metainfo;
    graph->elem_queue = temp;
    graph->pq.set_basic_values(buildstate->dimensions, pq_metainfo.m, pq_metainfo.nbits());
    graph->pq.set_fvec_L2sqr_ny_nearest_func();
    hnsw_read_pq_center(index, graph->pq);
    pg_memory_barrier();
    graph->flushing_pq = true;
}

static void join_pool(HnswBuildState *buildstate, Relation heap, Relation index)
{
    HnswGraph *graph = buildstate->graph;
    if (!graph->cluster_pq) {
        return;
    }
    SpinLockAcquire(&graph->lock);
    ++graph->nworker;
    SpinLockRelease(&graph->lock);
    while (!graph->flushing_pq) {
        CHECK_FOR_INTERRUPTS();
        constexpr long wait_time = 200'000l;    /* 0.2s */
        pg_usleep(wait_time);
    }
    Vector<ItemPointerData> tids;
    Vector<float *> vec_holder;
    disk_container::PlainStore ps(index, HNSW_PS_BLKNO, false);
    while (!graph->pq_flushed) {
        HnswElement e;
        if (graph->elem_queue->pop(e)) {
            e->transform_to_pq(heap, index, graph->pq, tids, vec_holder, ps);
        } else {
            CHECK_FOR_INTERRUPTS();
            constexpr long wait_time = 100'000l;    /* 0.1s */
            pg_usleep(wait_time);
        }
    }
    SpinLockAcquire(&graph->lock);
    --graph->nworker;
    SpinLockRelease(&graph->lock);
    ps.destroy();
    ann_helper::optional_destroy(tids);
    ann_helper::optional_destroy(vec_holder);
}

static bool insert_into_pool(HnswGraph *graph, HnswElement e)
    { return graph->elem_queue->push(e); }

static void end_pool(Relation heap, Relation index, HnswGraph *graph, Vector<ItemPointerData> &tids,
    Vector<float *> &vec_holder, disk_container::PlainStore &ps)
{
    HnswElement e;
    while (!graph->elem_queue->empty()) {
        if (graph->elem_queue->pop(e)) {
            e->transform_to_pq(heap, index, graph->pq, tids, vec_holder, ps);
        }
    }
    graph->pq_flushed = true;
    pg_memory_barrier();
    graph->flushing_pq = false;
    graph->pq.free_resourses();
    while (graph->nworker != 0) {
        CHECK_FOR_INTERRUPTS();
        constexpr long wait_time = 50'000l;    /* 0.05s */
        pg_usleep(wait_time);
    }
    /* we don't free elem_queue here,
     * there was unsolved bug for backend free pointers from graphCtx */
}

HnswMetaBlknos createHnswMetaPage(Relation index, ForkNumber forkNum, int m, int dimensions, 
    int efConstruction, Metric metric, QuantizerType qt_type, RaBitQParam *rbq_param,
    DistPrecisionType precision_type, bool needWal)
{
    Buffer buf;
    Page page;
    HnswMetaPage metap;
    HnswMetaBlknos metablknos;
    const bool is_hybrid = isHybridIndex(index);

    buf = HnswNewBuffer(index, forkNum, is_hybrid);
    metablknos.metablkno = BufferGetBlockNumber(buf);
    page = BufferGetPage(buf);
    HnswInitPage(buf, page);

    /* Set metapage data */
    metap = HnswPageGetMeta(page);
    metap->magicNumber = HNSW_MAGIC_NUMBER;
    metap->version = HNSW_VERSION;
    metap->dimensions = dimensions;
    metap->m = m;
    metap->efConstruction = efConstruction;
    metap->entryBlkno = InvalidBlockNumber;
    metap->entryOffno = InvalidOffsetNumber;
    metap->entryLevel = -1;
    metap->num_vectors = 0;
    metap->metric = metric;
    metap->cluster.cluster_pq = false;
    metap->precision_type = precision_type;
    ((PageHeader) page)->pd_lower =
        ((char *)metap + sizeof(HnswMetaPageData)) - (char *)page;
    metap->quantizer_metainfo.set_type(qt_type);
    metap->quantizer_metainfo.num_new_data = 0;
    metap->quantizer_metainfo.centroids_version = 0;
    metap->quantizer_metainfo.code_version = 0;
    if (qt_type == QuantizerType::NONE) {
        auto meta = metap->quantizer_metainfo.metainfo;
        errno_t rc = memset_s(&meta, sizeof(meta), 0, sizeof(meta));
        securec_check_c(rc, "\0", "\0");
    }
    else if (qt_type == QuantizerType::PQ) {
        HnswPQMetaInfo &pq_metainfo = metap->quantizer_metainfo.get_pq_metainfo();
        pq_metainfo.init(dimensions);
    } else if (qt_type == QuantizerType::RABITQ) {
        RaBitQMeta &rbq_meta = metap->quantizer_metainfo.get_rabitq_meta();
        rbq_meta.enabled = false;
        rbq_meta.keep_vecs = HnswGetRaBitQKeepVecs(index);
        size_t padded_dim = RABITQ_PADDED_DIM(dimensions);
        size_t cid_size = sizeof(uint16);
        size_t bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
        size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
        rbq_meta.quant_size = cid_size + bin_size + ext_size;
        rbq_meta.query_rescaling_factor = get_const_scaling_factors(dimensions, 3);
    }
    

    Buffer head_buf = HnswNewBuffer(index, forkNum, is_hybrid);
    Page head_page = BufferGetPage(head_buf);
    BlockNumber head_blkno = BufferGetBlockNumber(head_buf);
    HnswInitPage(head_buf, head_page);
    MarkBufferDirty(head_buf);
    if (needWal) {
        HnswXLogAppendPage(index, head_buf, head_page);
    }
    UnlockReleaseBuffer(head_buf);

    metap->insertPage = metap->head_blkno = head_blkno;
    metablknos.headblkno = head_blkno;

    MarkBufferDirty(buf);
    if (needWal) {
        HnswXLogAppendPage(index, buf, page);
    }
    UnlockReleaseBuffer(buf);

    if (HnswUseCluster(index)) {
        [[maybe_unused]]
        BlockNumber ps_blkno = disk_container::PlainStore::get_plain_store(index, false, forkNum);
        Assert(ps_blkno == HNSW_PS_BLKNO);
    }
    if (HnswUseCluster(index) || qt_type == QuantizerType::PQ) {
        buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
        Assert(BufferGetBlockNumber(buf) == HNSW_PQ_BLKNO(index));
        ReleaseBuffer(buf);
    }

    if (rbq_param != NULL && rbq_param->rbq_meta.enabled && qt_type == QuantizerType::RABITQ) {
        buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
        Assert(BufferGetBlockNumber(buf) == HNSW_RABITQ_BLKNO(index));
        ReleaseBuffer(buf);
    }

    return metablknos;
}

static void HnswBuildAppendPage(Relation index, Buffer *buf, Page *page, ForkNumber forkNum)
{
    /* Add a new page */
    Buffer newbuf = HnswNewBuffer(index, forkNum);

    /* Update previous page */
    HnswPageGetOpaque(*page)->nextblkno = BufferGetBlockNumber(newbuf);

    /* Commit */
    HnswCommitBuffer(*buf);

    /* Can take a while, so ensure we can interrupt */
    /* Needs to be called when no buffer locks are held */
    LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
    CHECK_FOR_INTERRUPTS();
    LockBuffer(newbuf, BUFFER_LOCK_EXCLUSIVE);

    /* Prepare new page */
    *buf = newbuf;
    *page = BufferGetPage(*buf);
    HnswInitPage(*buf, *page);
}

static void TransformPQ(HnswBuildState *buildstate, Relation heap, Relation index)
{
    HnswElement iter = buildstate->graph->head;
    ann_helper::Timer timer(0, 100'000ul, buildstate->indexName, buildstate->partIndexName);
    timer.set_stage("Flush PQ Codes");
    timer.report("Start to Flush PQ Codes");
    init_pool(buildstate, index);
    Vector<ItemPointerData> tids;
    Vector<float *> vec_holder;
    disk_container::PlainStore ps(index, HNSW_PS_BLKNO, false);
    HnswGraph *graph = buildstate->graph;
    while (!HnswPtrIsNull(iter)) {
        if (!insert_into_pool(graph, iter)) {
            iter->transform_to_pq(heap, index, graph->pq, tids, vec_holder, ps);
        }
        iter = iter->next;
        timer.report_loop("Flush PQ Codes");
    }
    end_pool(heap, index, buildstate->graph, tids, vec_holder, ps);
    ps.destroy();
    ann_helper::optional_destroy(tids);
    if (!vec_holder.empty()) {
        free_vector(vec_holder[0]);
    }
    ann_helper::optional_destroy(vec_holder);
}

FloatVectorArray quantizer_sample_data(Relation heap, Relation index, size_t dimensions,
    bool need_norm, DistPrecisionType precision_type, int parallel_workers, size_t k)
{
    int	numSamples = GetSampleNumbers(heap, index, k);
    FloatVectorArray samples = FloatVectorArrayInit(numSamples, dimensions);
    ann_sample_rows(samples, heap, index, dimensions, parallel_workers, need_norm, precision_type);
    return samples;
}

ProductQuantizer *do_kmeans(Relation index, FloatVectorArray samples, size_t dimensions,
    size_t m, size_t k, Metric metric, bool need_norm, int parallel_workers)
{
    ProductQuantizer *pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
    pq->set_basic_values(dimensions, m, log2(k));
    AnnKmeansState *kmeanstate = (AnnKmeansState *)palloc0(sizeof(AnnKmeansState));
    setupKmeansState(metric == Metric::INNER_PRODUCT && !need_norm ?
        Metric::INNER_PRODUCT : Metric::L2, index, kmeanstate, pq->dsub, true, true);
    pq->train(kmeanstate, samples, parallel_workers, u_sess->attr.attr_memory.maintenance_work_mem);
    FREE_ANNKEMANSTATE(kmeanstate);
    return pq;
}

static bool do_cluster(HnswBuildState *buildstate, QuantizerType type)
{
    constexpr auto get_pq_name = [](QuantizerType type) -> const char * {
        if (type == QuantizerType::PQ) {
            return "PQ";
        }
        return "RaBitQ";
    };
    ann_helper::Timer timer(0, 0, buildstate->indexName, buildstate->partIndexName);
    timer.set_stage("Setting up quantization");
    timer.set_nloop_count_unknown(true);
    timer.report("Start Setting up \"%s\"", get_pq_name(type));

    FloatVectorArray samples = quantizer_sample_data(buildstate->heap, buildstate->index,
        buildstate->dimensions, buildstate->need_norm, buildstate->precision_type,
        buildstate->parallel_workers, MAX_SAMPLE_VECTOR_NUM);
    int ndata = samples->length;
    if (ndata < HNSW_MIN_QT_SAMPLES_SIZE) {
        FloatVectorArrayFree(samples);
        timer.report("Finish Setting up \"%s\"", get_pq_name(type));
        timer.destroy();
        return false;
    }

    ProductQuantizer *&pq = buildstate->pq_quantizer;
    if (type == QuantizerType::PQ) {
        HnswPQMetaInfo &pq_metainfo = buildstate->pq_metainfo;
        int dim = buildstate->dimensions;
        uint16 m = pq_metainfo.m;
        uint16 k = pq_metainfo.k;
        pq = do_kmeans(buildstate->index, samples, dim, m, k,
            buildstate->metric, buildstate->need_norm, buildstate->parallel_workers);
        store_centroids(buildstate->index, pq->centroids, dim * k * sizeof(float), true, false, false);
    } else {
        pq = do_kmeans(buildstate->index, samples, buildstate->dimensions, buildstate->dimensions,
            HNSW_RABITQ_NUM_CLUSTERS, buildstate->metric, buildstate->need_norm, buildstate->parallel_workers);
        size_t centroids_size = HNSW_RABITQ_NUM_CLUSTERS * buildstate->dimensions * sizeof(float);
        RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(buildstate->rbq_param.quantizer);
        errno_t rc = memcpy_s(quantizer->get_centroids(), centroids_size, pq->centroids, centroids_size);
        securec_check(rc, "\0", "\0");
    }

    FloatVectorArrayFree(samples);
    timer.report("Finish Setting up \"%s\"", get_pq_name(type));
    timer.destroy();
    return true;
}

static void InitRaBitQ(HnswBuildState *buildstate)
{
    RaBitQParam &rbq_param = buildstate->rbq_param;
    rbq_param.quant_data = (char *)palloc0(rbq_param.rbq_meta.quant_size);
    RaBitQuantizer *quantizer =
        NEW RaBitQuantizer(rbq_param.dim, rbq_param.padded_dim, rbq_param.metric);
    rbq_param.quantizer = (void *)quantizer;
}

static bool TrainRaBitQ(HnswBuildState *buildstate, Relation index)
{
    RaBitQParam &rbq_param = buildstate->rbq_param;
    /* generate centroids by kmean clustering */
    if (do_cluster(buildstate, QuantizerType::RABITQ)) {
        /* generate random matrix and rotated the centroids */
        RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
        quantizer->train();
        return true;
    }
    rbq_param.rbq_meta.enabled = false;
    return false;
}

static void FreeBuildingRaBitQ(HnswBuildState *buildstate)
{
    RaBitQParam &rbq_param = buildstate->rbq_param;
    pfree_ext(rbq_param.quant_data);
    if (rbq_param.quantizer) {
        RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
        quantizer->destroy();
        delete quantizer;
        rbq_param.quantizer = NULL;
    }
}

static void CreateGraphPages(HnswBuildState *buildstate, Relation index)
{
    ForkNumber forkNum = buildstate->forkNum;
    Size maxSize;
    HnswTuple tuple;
    BlockNumber insertPage;
    HnswElement graphEntryPoint;
    Buffer buf;
    Page page;
    HnswElement iter = buildstate->graph->head;
    RaBitQParam &rbq_param = buildstate->rbq_param;

    ann_helper::Timer timer(buildstate->graph->indtuples, 1'000'000ul, buildstate->indexName,
                            buildstate->partIndexName);
    timer.set_stage("Flush Graph Pages");
    timer.report("Start Flush Graph Pages");

    /* Calculate sizes */
    maxSize = HNSW_MAX_SIZE;

    /* Allocate once */
    tuple = (HnswTuple)palloc0(HNSW_TUPLE_ALLOC_SIZE);

    /* Prepare first page */
    buf = HnswLoadBufferExtended(index, forkNum, buildstate->headblkno);
    page = BufferGetPage(buf);
    size_t num_vectors = 0;

    char **values;
    bool need_concurrent_quant = buildstate->concurrent_quant && !buildstate->vec_data &&
        (buildstate->graph->graph_pq || rbq_param.rbq_meta.enabled);
    
    if (need_concurrent_quant) {
        Size s = sizeof(char *) * buildstate->graph->indtuples;
        values = s >= MaxAllocSize ? (char **)palloc_huge(CurrentMemoryContext, s) : (char **)palloc(s);
    }

    float *half2float = NULL;
    if (buildstate->precision_type == DistPrecisionType::HALF && !need_concurrent_quant) {
        half2float = alloc_floatvector(buildstate->dimensions);
    }

    ProductQuantizer *pq_quantizer = buildstate->pq_quantizer;

    const bool half_need_quant = buildstate->precision_type == DistPrecisionType::HALF &&  ((buildstate->graph->graph_pq) ||
                                (rbq_param.quantizer && rbq_param.rbq_meta.enabled));

    Size tupleSize;
    while (!HnswPtrIsNull(iter)) {
        HnswElement element = (HnswElement)iter;
        /* Calculate sizes */
        tupleSize = HNSW_TUPLE_SIZE(element->level, buildstate->m);

        /* Initial size check */
        if (tupleSize > HNSW_TUPLE_ALLOC_SIZE) {
            elog(ERROR, "index tuple too large");
        }

        if (!buildstate->vec_data) {
            char *value = element->value;
            Assert(value);
            size_t floatVectorIndex = num_vectors++;
            if (need_concurrent_quant) {
                values[num_vectors - 1ul] = value;
            } else {
                if (half_need_quant) {
                    halfs_to_floats((half *)value, half2float, buildstate->dimensions);
                    value = (char *)half2float;
                }
                if (buildstate->graph->graph_pq) {
                    char *code = (char *)palloc(sizeof(char) * pq_quantizer->code_size);
                    pq_quantizer->compute_code((float *)value, (uint8 *)code);
                    HnswSetElementQTCode(index, floatVectorIndex, pq_quantizer->code_size, code, true, false);
                    pfree(code);
                } else if (rbq_param.quantizer && rbq_param.rbq_meta.enabled) {
                    QuantizeRaBitQ(rbq_param, (float *)value, rbq_param.quant_data);
                    if (rbq_param.rbq_meta.keep_vecs) {
                        HnswSetElementVector(index, floatVectorIndex, buildstate->vector_size, value, true, true);
                    }
                    HnswSetElementQTCode(index, floatVectorIndex, rbq_param.rbq_meta.quant_size,
                        rbq_param.quant_data, true, rbq_param.rbq_meta.keep_vecs);
                } else {
                    HnswSetElementVector(index, floatVectorIndex, buildstate->vector_size, value, true);
                }
            }
            element->floatVectorIndex = floatVectorIndex;
        }


        /* Check free space */
        if (PageGetFreeSpace(page) < tupleSize) {
            HnswBuildAppendPage(index, &buf, &page, forkNum);
        }

        /* Calculate offsets */
        element->blkno = BufferGetBlockNumber(buf);
        element->offno = OffsetNumberNext(PageGetMaxOffsetNumber(page));

        /* Zero memory for each element */
        MemSet(tuple, 0, HNSW_TUPLE_ALLOC_SIZE);
        HnswSetElementTuple(tuple, element);
        tuple->floatVectorIndex = element->floatVectorIndex;
        
        /* Add element */
        if (PageAddItem(page, (Item) tuple, tupleSize, InvalidOffsetNumber, false, false) != element->offno) {
            elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
        }
       /* Update iterator */  
       iter = element->next;

        timer.report_loop("Flush Graph Pages");
    }
    timer.report("Flush Graph Pages finished");

    if (half2float) {
        free_vector(half2float);
    }

    if (need_concurrent_quant) {
        const auto quant_task = [&](size_t batchIndex, size_t start, size_t end) {
            Relation rel;
            Relation parent;
            Partition part;
            if (!IsBgWorkerProcess()) {
                rel = index;
            } else if (RelationIsPartition(index)) {
                Oid parent_id = GetBaseRelOidOfParition(index);
                parent = index_open(parent_id, NoLock);
                part = partitionOpen(parent, RelationGetRelid(index), NoLock);
                rel = partitionGetRelation(parent, part);
                RelationOpenSmgr(rel);
            } else {
                rel = index_open(RelationGetRelid(index), NoLock);
                RelationOpenSmgr(rel);
            }

            float *half2float_local = NULL;
            if (buildstate->precision_type == DistPrecisionType::HALF) {
                half2float_local = alloc_floatvector(buildstate->dimensions);
            }

            char *quant_data = NULL;
            if (buildstate->graph->graph_pq) {
                quant_data = (char *)palloc(sizeof(char) * pq_quantizer->code_size);
            } else if (rbq_param.quantizer && rbq_param.rbq_meta.enabled) {
                quant_data = (char *)palloc(rbq_param.rbq_meta.quant_size);
            }

            for (size_t i = start; i < end; ++i) {
                char *local_value = values[i];
                if (half_need_quant) {
                    halfs_to_floats((half *)local_value, half2float_local, buildstate->dimensions);
                    local_value = (char *)half2float_local;
                }

                if (buildstate->graph->graph_pq) {
                    pq_quantizer->compute_code((float *)local_value, (uint8 *)quant_data);
                    HnswSetElementQTCode(rel, i, pq_quantizer->code_size, quant_data, true, false);
                } else if (rbq_param.quantizer && rbq_param.rbq_meta.enabled) {
                    QuantizeRaBitQ(rbq_param, (float *)local_value, quant_data);
                    if (rbq_param.rbq_meta.keep_vecs) {
                        HnswSetElementVector(rel, i, buildstate->vector_size, local_value, true, true);
                    }
                    HnswSetElementQTCode(rel, i, rbq_param.rbq_meta.quant_size, quant_data, true, rbq_param.rbq_meta.keep_vecs);
                }

                timer.inc_loop_count_forground_report("Quantizing Codes");
            }

            if (half2float_local) {
                free_vector(half2float_local);
            }

            if (quant_data) {
                pfree(quant_data);
            }

            if (!IsBgWorkerProcess()) {
                /* do nothing */
            } else if (RelationIsPartition(index)) {
                releaseDummyRelation(&rel);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
                partitionClose(parent, part, NoLock);
                index_close(parent, NoLock);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized, gcc false reporting */
            } else {
                index_close(rel, NoLock);
            }
        };

        timer.reset_step(500'000ul);
        if (rbq_param.rbq_meta.enabled) {
            timer.set_stage("RaBitQ Quantization");
            timer.report("Start RaBitQ Quantization");
        } else {
            timer.set_stage("Product Quantization");
            timer.report("Start Product Quantization");
        }

        INIT_TASK_RUNNER();
        if (buildstate->parallel_workers > 0) {
            LAUNCH_CONSUMER(buildstate->parallel_workers);
        }
        START_TASK_POOL();
        PARALLEL_BATCH_RUN_INIT();
        int total_parallel_workers = buildstate->parallel_workers + 1;
        PARALLEL_BATCH_RUN_TASK_WAIT(num_vectors, total_parallel_workers, quant_task);
        WAIT_AND_END_TASK_POOL();
        DESTROY_TASK_RUNNER();

        if (rbq_param.rbq_meta.enabled) {
            timer.report("RaBitQ Quantization Finished");
        } else {
            timer.report("Product Quantization Finished");
        }

        pfree(values);
    }

    insertPage = BufferGetBlockNumber(buf);
    /* Commit */
    HnswCommitBuffer(buf);

    pfree(tuple);
    timer.destroy();

    graphEntryPoint = (HnswElement)buildstate->graph->entryPoint;
    Buffer metaBuf = HnswLoadBufferExtended(index, forkNum, buildstate->metablkno);
    HnswMetaPage metap = HnswPageGetMeta(BufferGetPage(metaBuf));
    if (graphEntryPoint) {
        metap->entryBlkno = graphEntryPoint->blkno;
        metap->entryOffno = graphEntryPoint->offno;
        metap->entryLevel = graphEntryPoint->level;
        metap->insertPage = insertPage;
    }
    if (!buildstate->vec_data) {
        metap->num_vectors = num_vectors;
        metap->cluster.cluster_pq = buildstate->graph->cluster_pq;
        if (pq_quantizer && buildstate->qt_type == QuantizerType::PQ) {
            metap->quantizer_metainfo.get_pq_metainfo().graph_pq = buildstate->graph->graph_pq;
        } else if (rbq_param.quantizer && rbq_param.rbq_meta.enabled) {
            RaBitQMeta &rbq_meta = metap->quantizer_metainfo.get_rabitq_meta();
            rbq_meta.enabled = true;
        }
    }
    HnswCommitBuffer(metaBuf);
}

static void WriteNeighbors(HnswBuildState *buildstate, Relation index)
{
    int m = buildstate->m;
    HnswElement iter = buildstate->graph->head;
    ann_helper::Timer timer(buildstate->graph->indtuples, 1'000'000, buildstate->indexName, buildstate->partIndexName);
    timer.set_stage("Flush neighbors");
    timer.report("Start Flush neighbors");

    while (!HnswPtrIsNull(iter)) {
        HnswElement element = (HnswElement)iter;

        Buffer buf = ReadBuffer(index, element->blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
        ItemId item = PageGetItemId(page, element->offno);
        HnswTuple tuple = (HnswTuple) PageGetItem(page, item);

        /* Update iterator */
        iter = element->next;

        /* Can take a while, so ensure we can interrupt */
        /* Needs to be called when no buffer locks are held */
        CHECK_FOR_INTERRUPTS();

        HnswSetNeighborsData(tuple, element, m);

        /* Commit */
        HnswCommitBuffer(buf);
        timer.report_loop("Flush neighbors");   
    }
    timer.report("Flush neighbors finished");
    timer.destroy();
}

static void free_graph_ctx(HnswBuildState *buildstate)
{
    if (t_thrd.proc->sessMemorySessionid == buildstate->graphCtxcreator &&
        !buildstate->graphCtxfreed) {
        MemoryContextDelete(buildstate->graphCtx);
        buildstate->graphCtxfreed = true;
    }
}

static void FlushPages(HnswBuildState *buildstate, Relation index)
{
    if (!buildstate->vec_data) {
        create_vec_data(index, true);
    }

    CreateGraphPages(buildstate, index);
    WriteNeighbors(buildstate, index);

    buildstate->graph->flushed = true;
    free_graph_ctx(buildstate);
}

static bool AddDuplicateInMemory(Relation index, HnswElement element, HnswElement dup)
{
    bool unused;
    LWLockAcquire(&dup->lock, LW_EXCLUSIVE);
    bool res = dup->insert_tid(index, true, element->heaptids[0], unused);
    LWLockRelease(&dup->lock);
    return res;
}

static bool FindDuplicateInMemory(Relation index, HnswElement element, size_t vec_size)
{
    HnswNeighborArray *neighbors = HnswGetNeighbors(element, 0);
    Pointer value = (Pointer)element->value;

    for (int i = 0; i < neighbors->length; i++) {
        HnswCandidate *neighbor = &neighbors->items[i];
        HnswElement neighborElement = (HnswElement)neighbor->element;
        Pointer neighborValue = (Pointer)neighborElement->value;

        /* Exit early since ordered by distance */
        size_t old_idx = element->floatVectorIndex;
        if (memcmp(value, neighborValue, vec_size) == 0) {
            element->floatVectorIndex = InvalidVectorIndex;
        } else if (element->floatVectorIndex != NonCenterVectorIndex) {
            return false;
        }

        /* Check for space */
        if (AddDuplicateInMemory(index, element, neighborElement)) {
            return true;
        }
        element->floatVectorIndex = old_idx;
    }

    return false;
}

static void AddElementInMemory(HnswGraph * graph, HnswElement element)
{
    SpinLockAcquire(&graph->lock);
    element->next = graph->head;
    graph->head = element;
    SpinLockRelease(&graph->lock);
}

static void UpdateNeighborsInMemory(distance_func dist_func, HnswElement e, int m, uint32 dim)
{
    const uint8 level = e->level;
    for (uint8 ilc = 0; ilc <= level; ++ilc) {
        uint8 lc = level - ilc;
        int lm = HnswGetLayerM(m, lc);
        HnswNeighborArray *neighbors = HnswGetNeighbors(e, lc);
        for (int i = 0; i < neighbors->length; ++i) {
            HnswCandidate *hc = &neighbors->items[i];
            HnswElement neighborElement = (HnswElement)hc->element;

            /* Keep scan-build happy on Mac x86-64 */
            Assert(neighborElement);

            /* Use element for lock instead of hc since hc can be replaced */
            LWLockAcquire(&neighborElement->lock, LW_EXCLUSIVE);
            HnswUpdateConnection(e, hc, lm, lc, dist_func, dim, m);
            LWLockRelease(&neighborElement->lock);
        }
    }
}

static void UpdateGraphInMemory(Relation index, HnswElement element, HnswElement entryPoint,
    HnswBuildState *buildstate)
{
    if (FindDuplicateInMemory(index, element, buildstate->vector_size)) {
        return;
    }

    HnswGraph *graph = buildstate->graph;
    AddElementInMemory(graph, element);
    UpdateNeighborsInMemory(buildstate->func_ptr, element, buildstate->m, buildstate->dimensions);

    /* Update entry point if needed (already have lock) */
    if (entryPoint == NULL || element->level > entryPoint->level) {
        graph->entryPoint = element;
    }
}

static void InsertTupleInMemory(Relation index, HnswBuildState *buildstate, HnswElement element)
{
    distance_func dist_func = buildstate->func_ptr;
    HnswGraph   *graph = buildstate->graph;
    HnswElement entryPoint;
    LWLock      *entryLock = &graph->entryLock;
    LWLock      *entryWaitLock = &graph->entryWaitLock;
    int         m = buildstate->m;

    /* Wait if another process needs exclusive lock on entry lock */
    LWLockAcquire(entryWaitLock, LW_EXCLUSIVE);
    LWLockRelease(entryWaitLock);

    /* Get entry point */
    LWLockAcquire(entryLock, LW_SHARED);
    entryPoint = (HnswElement)graph->entryPoint;

    /* Prevent concurrent inserts when likely updating entry point */
    if (entryPoint == NULL || element->level > entryPoint->level) {
        /* Release shared lock */
        LWLockRelease(entryLock);

        /* Tell other processes to wait and get exclusive lock */
        LWLockAcquire(entryWaitLock, LW_EXCLUSIVE);
        LWLockAcquire(entryLock, LW_EXCLUSIVE);
        LWLockRelease(entryWaitLock);

        /* Get latest entry point after lock is acquired */
        entryPoint = (HnswElement)graph->entryPoint;
    }

    /* Find neighbors for element */
    HnswFindElementNeighbors(element, entryPoint, dist_func, m, buildstate->efConstruction,
                             buildstate->dimensions);

    /* Update graph in memory */
    UpdateGraphInMemory(index, element, entryPoint, buildstate);

    /* Release entry lock */
    LWLockRelease(entryLock);
}

static bool InsertTuple(Relation heap, Relation index, Datum *values, const bool *isnull,
    ItemPointer heaptid, HnswBuildState *buildstate, char **bin_elem_buf, BlockNumber metablkno)
{
    HnswGraph *graph = buildstate->graph;
    HnswElement element;
    HnswAllocator *allocator = &buildstate->allocator;
    void *valuePtr;
    LWLock *flushLock = &graph->flushLock;
    uint32 dim = buildstate->dimensions;

    /* Detoast once for all calls */
    size_t floatVectorIndex = buildstate->vec_data ? DatumGetUInt64(values[1]) : InvalidVectorIndex;

    /* Ensure graph not flushed when inserting */
    LWLockAcquire(flushLock, LW_SHARED);

    /* Are we in the on-disk phase? */
    if (graph->flushed) {
        LWLockRelease(flushLock);
        free_graph_ctx(buildstate);
        bool res = HnswInsertTupleOnDisk(index, heap, values, isnull, heaptid, true, floatVectorIndex, metablkno);
        return res;
    }

    /*
     * In a parallel build, the HnswElement is allocated from the shared
     * memory area, so we need to coordinate with other processes.
     */
    LWLockAcquire(&graph->allocatorLock, LW_EXCLUSIVE);

    /*
     * Check that we have enough memory available for the new element now that
     * we have the allocator lock, and flush pages if needed.
     */
    if (graph->memoryUsed >= graph->memoryTotal) {
        LWLockRelease(&graph->allocatorLock);

        LWLockRelease(flushLock);
        if (!graph->warned && !IsBgWorkerProcess()) {
            graph->warned = true;
            ereport(WARNING,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("hnsw graph no longer fits into maintenance_work_mem after "
                        INT64_FORMAT " tuples", (int64) graph->indtuples),
                 errdetail("Building will take significantly more time."),
                 errhint("Increase maintenance_work_mem to speed up builds.")));
        }
        if (!LWLockConditionalAcquire(flushLock, LW_EXCLUSIVE)) {
            if (!graph->flushed) {
                join_pool(buildstate, heap, index);
            }
            LWLockAcquire(flushLock, LW_EXCLUSIVE);
        }

        if (!graph->flushed) {
            buildstate->concurrent_quant = false;
            FlushPages(buildstate, index);
        }

        LWLockRelease(flushLock);

        bool res = HnswInsertTupleOnDisk(index, heap, values, isnull, heaptid, true, floatVectorIndex, metablkno);
        return res;
    }

    /* Ok, we can proceed to allocate the element */
    bool as_bin_elem = buildstate->ncluster > 0 && graph->indtuples > buildstate->ncluster;
    if (as_bin_elem) {
         size_t vec_size = ann_helper::vector_aligned_size + buildstate->vector_size;
        if (!*bin_elem_buf) {
            *bin_elem_buf = (char *)HnswAlignedAlloc(allocator,
                sizeof(HnswElementData) + vec_size, HnswElementDataAlignment);
            ((HnswElement)*bin_elem_buf)->neighbors = NULL;
        }
        element = (HnswElement)*bin_elem_buf;
        element = HnswInitElement(heaptid, buildstate->m, buildstate->ml, buildstate->maxLevel,
                                  element, allocator, NonCenterVectorIndex);
        valuePtr = element + 1;
        std::align(ann_helper::vector_aligned_size, buildstate->vector_size, valuePtr, vec_size);
    } else {
        element = HnswInitElement(heaptid, buildstate->m, buildstate->ml, buildstate->maxLevel,
                                   NULL, allocator, floatVectorIndex);
        valuePtr = HnswAlignedAlloc(allocator, buildstate->vector_size, ann_helper::vector_aligned_size);
    }

    /*
     * We have now allocated the space needed for the element, so we don't
     * need the allocator lock anymore. Release it and initialize the rest of
     * the element.
     */
    LWLockRelease(&graph->allocatorLock);

    Pointer vec_p = NULL;
    char *v = DatumGetVector(values[0], buildstate->precision_type, &vec_p);

    /* Copy the datum */
    memcpy(valuePtr, v, buildstate->vector_size);
    element->value = (Pointer)valuePtr;

    if (vec_p != DatumGetPointer(values[0])) {
        pfree(vec_p);
    }

    if (buildstate->norm_func_ptr) {
        Assert(buildstate->need_norm);
        buildstate->norm_func_ptr(element->value, dim, element->value);
    }

    /* Create a lock for the element */
    LWLockInitialize(&element->lock, LWTRANCHE_EXTEND);

    /* Insert tuple */
    InsertTupleInMemory(index, buildstate, element);

    /* Release flush lock */
    LWLockRelease(flushLock);

    return true;
}

/*
 * Callback for table_index_build_scan
 */
static void BuildCallback(Relation index, HeapTuple hup, Datum *values,
                          const bool *isnull, bool tupleIsAlive, void *state)
{
    BuildCallbackData* data = (BuildCallbackData *)state;
    HnswBuildState *buildstate = (HnswBuildState *)data->buildState;
    Relation heap = data->heap != NULL ? data->heap : buildstate->heap;
    data->timer->inc_loop_count_forground_report("Graph Build");
    HnswGraph *graph = buildstate->graph;
    MemoryContext oldCtx;

#if PG_VERSION_NUM < 130000
    ItemPointer tid = &hup->t_self;
#endif

    /* Skip nulls */
    if (isnull[0]) {
        return;
    }

    MemoryContext tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw build temporary context", ALLOCSET_DEFAULT_SIZES);
    /* Use memory context */
    oldCtx = MemoryContextSwitchTo(tmpCtx);

    /* Insert tuple */
    if (InsertTuple(heap, index, values, isnull, tid, buildstate, &data->bin_elem_buf,
                    buildstate->metablkno)) {
        /* Update progress */
        SpinLockAcquire(&graph->lock);
        ++graph->indtuples;
        SpinLockRelease(&graph->lock);
    }

    /* Reset memory context */
    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(tmpCtx);
}

static void InitGraph(HnswGraph * graph, long memoryTotal)
{
    graph->head = NULL;
    graph->entryPoint = NULL;
    graph->memoryUsed = 0;
    graph->memoryTotal = memoryTotal;
    graph->flushed = false;
    graph->warned = false;
    /* cluster and PQ setting */
    graph->cluster_pq = false;
    graph->graph_pq = false;
    graph->pq_flushed = false;
    graph->nworker = 0;
    graph->elem_queue = NULL;
    graph->indtuples = 0;
    SpinLockInit(&graph->lock);
    LWLockInitialize(&graph->entryLock, LWTRANCHE_EXTEND);
    LWLockInitialize(&graph->entryWaitLock, LWTRANCHE_EXTEND);
    LWLockInitialize(&graph->allocatorLock, LWTRANCHE_EXTEND);
    LWLockInitialize(&graph->flushLock, LWTRANCHE_EXTEND);
}

static void InitAllocator(HnswAllocator *allocator, void *(*alloc)(Size size, void *state),
                          void *(*align_alloc)(Size size, Size align, void *state), void *state)
{
    allocator->alloc = alloc;
    allocator->align_alloc = align_alloc;
    allocator->state = state;
}

static void *HnswSharedMemoryAlloc(Size size, void *state)
{
    HnswBuildState *buildstate = (HnswBuildState *)state;
    void       *chunk = buildstate->hnswarea + buildstate->graph->memoryUsed;
    buildstate->graph->memoryUsed += MAXALIGN(size);
    return chunk;
}

static void *HnswSharedMemoryAlignedAlloc(Size size, Size align, void *state)
{
    HnswBuildState *buildstate = (HnswBuildState *)state;
    void       *chunk = buildstate->hnswarea + buildstate->graph->memoryUsed;
    Size used = size + align;
    std::align(align, size, chunk, used);
    buildstate->graph->memoryUsed += size + size + align - used;
    return chunk;
}

static void InitBuildState(HnswBuildState *buildstate, Relation heap, Relation index,
    IndexInfo *indexInfo, ForkNumber forkNum, int m, int efConstruction, QuantizerType qt_type,
    int parallel_workers, int maintenance_work_mem, vector_pair_vector *vec_data = NULL)
{
    buildstate->heap = heap;
    buildstate->index = index;
    buildstate->indexInfo = indexInfo;
    buildstate->forkNum = forkNum;

    buildstate->m = m;
    buildstate->efConstruction = efConstruction;
    buildstate->parallel_workers = parallel_workers;
    buildstate->maintenance_work_mem = maintenance_work_mem;
    buildstate->vec_data = vec_data;
    buildstate->ncluster = HnswGetNumCluster(index);
    buildstate->qt_type = qt_type;
   
    buildstate->need_norm = HnswOptionalProcInfo(index, HNSW_NORM_PROC) != NULL;
    buildstate->dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;

    buildstate->precision_type = DistPrecisionType::FLOAT;
    if (TupleDescAttr(index->rd_att, 0)->atttypid == HALFVECTOROID) {
        buildstate->precision_type = DistPrecisionType::HALF;
    }

    buildstate->vector_size = buildstate->dimensions * VEC_ELEM_SIZE(buildstate->precision_type);

    if (buildstate->ncluster > 0) {
        elog(WARNING, "num_cluster is still an experimental feature");
    }

    /* Disallow varbit since require fixed dimensions */
    if (TupleDescAttr(index->rd_att, 0)->atttypid == VARBITOID) {
        elog(ERROR, "type not supported for hnsw index");
    }

    /* Require column to have dimensions to be indexed */
    if (buildstate->dimensions < 0) {
        elog(ERROR, "column does not have dimensions");
    }

    if (buildstate->dimensions > HNSW_MAX_DIM) {
        elog(ERROR, "column cannot have more than %d dimensions for hnsw index", HNSW_MAX_DIM);
    }

    if (buildstate->efConstruction < 2 * buildstate->m) {
        elog(ERROR, "ef_construction must be greater than or equal to 2 * m");
    }

    buildstate->reltuples = 0;
    buildstate->indtuples = 0;

    /* Get support functions */
    buildstate->procinfo = index_getprocinfo(index, 1, HNSW_DISTANCE_PROC);
    buildstate->metric = get_func_metric(buildstate->procinfo->fn_oid);
    buildstate->collation = index->rd_indcollation[0];
    buildstate->func_ptr = hnsw_get_aligned_distance_func(buildstate->index, buildstate->metric, 
        buildstate->dimensions, buildstate->precision_type);
    if (buildstate->need_norm) {
        buildstate->norm_func_ptr =
            get_vector_preprocess_func(Metric::FAST_COSINE, buildstate->precision_type);
    } else {
        buildstate->norm_func_ptr = NULL;
    }

    buildstate->ml = HnswGetMl(buildstate->m);
    buildstate->maxLevel = HnswGetMaxLevel(buildstate->m);

    /* Quantizer */
    buildstate->pq_quantizer = NULL;
    /* pq */
    if (qt_type == QuantizerType::PQ) {
        buildstate->pq_metainfo.init(buildstate->dimensions);
    }
    /* RaBitQ */
    RaBitQParam &rbq_param = buildstate->rbq_param;
    /* do not enable RaBitQ for graph cluster or hybridann for now */
    rbq_param.rbq_meta.enabled = qt_type == QuantizerType::RABITQ && buildstate->ncluster <= 0 && vec_data == NULL;
    if (rbq_param.rbq_meta.enabled) {
        rbq_param.rbq_meta.keep_vecs = HnswGetRaBitQKeepVecs(index);
        rbq_param.applied = false; /* not use RaBitQ distance estimation during index building */
        rbq_param.dim = buildstate->dimensions;
        rbq_param.padded_dim = RABITQ_PADDED_DIM(buildstate->dimensions);
        rbq_param.metric = buildstate->metric;
        rbq_param.cid_size = sizeof(uint16);
        rbq_param.bin_size = RABITQ_BIN_DATA_SIZE(rbq_param.padded_dim);
        rbq_param.ext_size = RABITQ_EXT_DATA_SIZE(rbq_param.padded_dim);
        rbq_param.rbq_meta.quant_size = rbq_param.cid_size + rbq_param.bin_size + rbq_param.ext_size;
        rbq_param.rbq_meta.query_rescaling_factor = -1.0f;
        rbq_param.quant_data = NULL;
        rbq_param.quantizer = NULL;
        rbq_param.estimator = NULL;
    }
    buildstate->concurrent_quant = true; /* for full in-memory build */
    /* end of RaBitQ */

    /* check if column expression is supported by quantization */
    if (index->rd_index->indkey.values[0] == 0 && (buildstate->ncluster > 0 ||
        buildstate->qt_type == QuantizerType::PQ || rbq_param.rbq_meta.enabled)) {
        if (!index->rd_indexprs) {
            RelationGetIndexExpressions(index);
        }
        bool supported = false;
        Expr *expr = (Expr *)linitial(index->rd_indexprs);
        if (IsA(expr, FuncExpr)) {
            Oid func_id = ((FuncExpr *)expr)->funcid;
            if (func_id == F_ARRAY_TO_FLOATVECTOR || func_id == F_FLOATVECTOR_TO_FLOATVECTOR
                || func_id == F_ARRAY_TO_HALFVECTOR || func_id == F_HALFVECTOR_TO_HALFVECTOR) {
                Expr *arg = (Expr *)linitial(((FuncExpr *)expr)->args);
                if (IsA(arg, Var)) {
                    supported = true;
                }
            } else if (func_id == F_SUBFLOATVECTOR || func_id == F_HALFVECTOR_SUBVECTOR) {
                Assert(list_length(((FuncExpr *)expr)->args) == 3);
                ListCell *lc = ((FuncExpr *)expr)->args->head;
                Expr *arg1 = (Expr *)lfirst(lc);
                lc = lnext(lc);
                Const *arg2 = (Const *)lfirst(lc);
                lc = lnext(lc);
                Const *arg3 = (Const *)lfirst(lc);
                if (IsA(arg1, Var) && IsA(arg2, Const) && arg2->consttype == INT4OID &&
                    IsA(arg3, Const) && arg3->consttype == INT4OID) {
                    supported = true;
                }
            }
        }
        if (!supported) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("GRAPH_INDEX with quantization does not support customized expression")));
        }
    }

    buildstate->graphCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Hnsw build graph context", ALLOCSET_DEFAULT_SIZES);

    buildstate->graphCtxcreator = t_thrd.proc->sessMemorySessionid;
    buildstate->graphCtxfreed = false; 

    Size esthnswarea = u_sess->attr.attr_memory.maintenance_work_mem * 1024L;
    Size estother = 3 * 1024 * 1024;
    if (esthnswarea > estother) {
        esthnswarea -= estother;
    }

    buildstate->hnswarea = (char *)palloc_huge(buildstate->graphCtx, esthnswarea);

    InitGraph(&buildstate->graphData, esthnswarea - 1024 * 1024);

    InitAllocator(&buildstate->allocator, &HnswSharedMemoryAlloc, &HnswSharedMemoryAlignedAlloc,
                  buildstate);

    buildstate->graph = &buildstate->graphData;
    buildstate->hnswleader = NULL;

    populate_index_partition_name(buildstate->index, buildstate->indexName, buildstate->partIndexName);
}

static void FreeBuildState(HnswBuildState *buildstate) { free_graph_ctx(buildstate); }

/*
 * Within leader, wait for end of heap scan
 */
static double ParallelHeapScan(HnswBuildState *buildstate)
{
    HnswShared *hnswshared = (HnswShared *)buildstate->hnswleader->hnswshared;
    BgworkerListWaitFinish(&buildstate->hnswleader->nparticipanttuplesorts);
    pg_memory_barrier();
    double reltuples = hnswshared->reltuples;
    return reltuples;
}

/*
 * Perform a worker's portion of a parallel insert
 */
static void HnswParallelScanAndInsert(Relation heapRel, Relation indexRel, HnswShared *hnswshared)
{
    TableScanDesc scan;
    double        reltuples;
    IndexInfo  *indexInfo;

    /* Join parallel scan */
    indexInfo = BuildIndexInfo(indexRel);
    indexInfo->ii_Concurrent = hnswshared->isconcurrent;
    BuildCallbackData data = { hnswshared->buildstate, heapRel, hnswshared->timer, NULL};
    scan = tableam_scan_begin_parallel(heapRel, &hnswshared->heapdesc);
    reltuples = IndexBuildHeapScan(heapRel, indexRel, indexInfo, true, BuildCallback,
                                   (void *)&data, scan);
    pfree(indexInfo);

    /* Record statistics */
    SpinLockAcquire(&hnswshared->mutex);
    hnswshared->nparticipantsdone++;
    hnswshared->reltuples += reltuples;
    SpinLockRelease(&hnswshared->mutex);
}

void HnswParallelBuildMain(const BgWorkerContext *bwc)
{
    HnswShared *hnswshared = (HnswShared *)bwc->bgshared;
    Relation  targetheap;
    Relation  targetindex;
    Partition heappart = NULL;
    Partition indexpart = NULL;
    
    Relation    heapRel;
    Relation    indexRel;
    LOCKMODE    heapLockmode = NoLock;
    LOCKMODE    indexLockmode = NoLock;

    /* Open relations within worker */
    heapRel = heap_open(hnswshared->heaprelid, heapLockmode);
    indexRel = index_open(hnswshared->indexrelid, indexLockmode);

    if (OidIsValid(hnswshared->heappartid)) {
        heappart = partitionOpen(heapRel, hnswshared->heappartid,  heapLockmode);
        indexpart = partitionOpen(indexRel, hnswshared->indexpartid, indexLockmode);
        targetheap = partitionGetRelation(heapRel, heappart);
        targetindex = partitionGetRelation(indexRel, indexpart);
    } else {
        targetheap = heapRel;
        targetindex = indexRel;
    }

    /* Perform inserts */
    HnswParallelScanAndInsert(targetheap, targetindex, hnswshared);
    if (!hnswshared->buildstate->graph->flushed) {
        join_pool(hnswshared->buildstate, targetheap, targetindex);
    }

    if (OidIsValid(hnswshared->heappartid)) {
        releaseDummyRelation(&targetheap);
        releaseDummyRelation(&targetindex);
        partitionClose(indexRel, indexpart, NoLock);
        partitionClose(heapRel, heappart, NoLock);
    }

    /* Close relations within worker */
    index_close(indexRel, indexLockmode);
    heap_close(heapRel, heapLockmode);
}

static void HnswParallelCleanUp(const BgWorkerContext *bwc) {}

static void HnswEndParallel() { BgworkerListSyncQuit(); }

static void BuildGraphfromVecdata(HnswBuildState *buildstate, ann_helper::Timer *timer)
{
    Oid indexrelid;
    Oid indexpartid;
    if (RelationIsPartition(buildstate->index)) {
        indexrelid = GetBaseRelOidOfParition(buildstate->index);
        indexpartid = RelationGetRelid(buildstate->index);
    } else {
        indexrelid = RelationGetRelid(buildstate->index);
        indexpartid = InvalidOid;
    }

    const auto buildtask = [&](int batchIndex, int start, int end) {
        FloatVector *vector = InitFloatVector(buildstate->dimensions);
        HeapTupleData hup;
        BuildCallbackData data = {buildstate, NULL, timer, NULL};
        Datum values[2];
        bool isnull[2] = {false};
        Relation indexRel = NULL;
        Partition indexpart = NULL;
        Relation targetindex;
        if (IsBgWorkerProcess()) {
            indexRel = index_open(indexrelid, NoLock);
            if (OidIsValid(indexpartid)) {
                indexpart = partitionOpen(indexRel, indexpartid, NoLock);
                targetindex = partitionGetRelation(indexRel, indexpart);
            } else {
                targetindex = indexRel;
            }
        } else {
            targetindex = buildstate->index;
        }
        VecStorageType st = VecStorageType::PureVec;
        if (buildstate->graph->graph_pq || buildstate->rbq_param.rbq_meta.enabled) {
            st = VecStorageType::VecWithCode;
        }
        for (int64 i = start; i < end; ++i) {
            VectorPair v = *buildstate->vec_data->at(i);
            VecBuffer buffer = vec_read_buffer(targetindex, v.vid, buildstate->vector_size, st);
            errno_t rc = memcpy_s(vector->x, buildstate->vector_size,
                buffer.get_vecbuf(), buildstate->vector_size);
            buffer.release();
            securec_check(rc, "", "");
            values[0] = PointerGetDatum(vector);
            values[1] = UInt64GetDatum(v.vid);
            hup.t_self = v.tid;
            BuildCallback(targetindex, &hup,  values, isnull, true, &data);
        }

        pfree(vector);

        if (IsBgWorkerProcess()) {
            /* Close relations within worker */
            if (OidIsValid(indexpartid)) {
                releaseDummyRelation(&targetindex);
                partitionClose(indexRel, indexpart, NoLock);
            }
            index_close(indexRel, NoLock);
        }
    };

    int parallel_workers = buildstate->parallel_workers;

    INIT_TASK_RUNNER();
    if (parallel_workers > 0) {
        LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(parallel_workers);
    }
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();

    int totalParaWorkers = parallel_workers + 1;
    PARALLEL_BATCH_RUN_TASK_WAIT(buildstate->vec_data->size(), totalParaWorkers, buildtask);

    WAIT_AND_END_TASK_POOL();
    DESTROY_TASK_RUNNER();
}

static void HnswBeginParallel(HnswBuildState *buildstate, bool isconcurrent, int request,
    ann_helper::Timer *timer)
{
    HnswShared *hnswshared;
    HnswLeader *hnswleader = (HnswLeader *) palloc0(sizeof(HnswLeader));
    Assert(request > 0);

    uint32 nparts = 0;
    if (RelationIsGlobalIndex(buildstate->index)) {
        if (RelationIsSubPartitioned(buildstate->heap)) {
            nparts = GetSubPartitionNumber(buildstate->heap);
        } else {
            nparts = getPartitionNumber(buildstate->heap->partMap);
        }
    }
    hnswshared = (HnswShared *)palloc0(sizeof(HnswShared) + sizeof(double) * nparts);

    /* Initialize immutable state */
    if (RelationIsPartition(buildstate->heap)) {
        hnswshared->heaprelid = GetBaseRelOidOfParition(buildstate->heap);
        hnswshared->indexrelid = GetBaseRelOidOfParition(buildstate->index);
        hnswshared->heappartid = RelationGetRelid(buildstate->heap);
        hnswshared->indexpartid = RelationGetRelid(buildstate->index);
    } else {
        hnswshared->heaprelid = RelationGetRelid(buildstate->heap);
        hnswshared->indexrelid = RelationGetRelid(buildstate->index);
        hnswshared->heappartid = InvalidOid;
        hnswshared->indexpartid = InvalidOid;
    }

    hnswshared->isconcurrent = isconcurrent;

    SpinLockInit(&hnswshared->mutex);
    hnswshared->nparticipantsdone = 0;
    hnswshared->reltuples = 0;
    HeapParallelscanInitialize(&hnswshared->heapdesc, buildstate->heap);

    hnswshared->timer = timer;
    hnswshared->buildstate = buildstate;

    hnswleader->nparticipanttuplesorts = LaunchBackgroundWorkers(request, hnswshared,
        HnswParallelBuildMain, HnswParallelCleanUp, buildstate->ncluster == 0);
    
    if (hnswleader->nparticipanttuplesorts == 0) {
        pfree_ext(hnswshared);
        pfree_ext(hnswleader);
        return;
    }
    hnswleader->hnswshared = hnswshared;
    buildstate->hnswleader = hnswleader;

    /* Join heap scan ourselves */
    HnswParallelScanAndInsert(buildstate->heap, buildstate->index, hnswshared);
}

static BlockNumber BuildGraph(HnswBuildState *buildstate, ForkNumber forkNum)
{
    size_t reltuples_stats = 0;
    if (buildstate->vec_data) {
        reltuples_stats = buildstate->vec_data->size();
    } else {
        reltuples_stats = get_relstats_reltuples(buildstate->heap);
    }

    HnswMetaBlknos metablknos = createHnswMetaPage(buildstate->index, buildstate->forkNum,
        buildstate->m, buildstate->dimensions, buildstate->efConstruction, buildstate->metric,
        buildstate->qt_type, &buildstate->rbq_param, buildstate->precision_type);

    buildstate->metablkno = metablknos.metablkno;
    buildstate->headblkno = metablknos.headblkno;

    if (buildstate->ncluster > 0 || buildstate->qt_type == QuantizerType::PQ) {
        ProductQuantizer *&pq_quantizer = buildstate->pq_quantizer;
        if (do_cluster(buildstate, QuantizerType::PQ)) {
            buildstate->graph->graph_pq = buildstate->qt_type == QuantizerType::PQ;
            buildstate->graph->cluster_pq = buildstate->ncluster > 0;
            pq_quantizer->set_fvec_L2sqr_ny_nearest_func();
        }
    } else if (buildstate->rbq_param.rbq_meta.enabled) {
        InitRaBitQ(buildstate);
        if (TrainRaBitQ(buildstate, buildstate->index)) {
            RaBitQParam &rbq_param = buildstate->rbq_param;
            RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
        
            size_t random_matrix_size = quantizer->get_random_matrix_size();
            size_t centroids_size = HNSW_RABITQ_NUM_CLUSTERS * rbq_param.dim * sizeof(float);
            size_t rotated_centroids_size = HNSW_RABITQ_NUM_CLUSTERS * rbq_param.padded_dim * sizeof(float);
            size_t total_size = random_matrix_size + centroids_size + rotated_centroids_size;
        
            char *rabitq_data = (char *)palloc(total_size);
            char *centroids = rabitq_data + random_matrix_size;
            char *rotated_centroids = centroids + centroids_size;

            errno_t rc = memcpy_s(rabitq_data, random_matrix_size, quantizer->get_random_matrix(), random_matrix_size);
            securec_check(rc, "\0", "\0");
            rc = memcpy_s(centroids, centroids_size, quantizer->get_centroids(), centroids_size);
            securec_check(rc, "\0", "\0");
            rc = memcpy_s(rotated_centroids, rotated_centroids_size, quantizer->get_rotated_centroids(), rotated_centroids_size);
            securec_check(rc, "\0", "\0");

            store_centroids(buildstate->index, (float *)rabitq_data, total_size, true, false, false);
            pfree(rabitq_data);
        } else {
            FreeBuildingRaBitQ(buildstate);
        }
    }

    ann_helper::Timer timer(reltuples_stats, 500'000, buildstate->indexName, buildstate->partIndexName);
    timer.set_stage("Graph Build");
    timer.report("Start Graph Build");

    if (buildstate->vec_data) {
        BuildGraphfromVecdata(buildstate, &timer);
    } else {
        /* Attempt to launch parallel worker scan when required */
        if (buildstate->heap != NULL) {
            if (buildstate->parallel_workers > 0) {
                if (buildstate->heap->rd_rel->relpersistence != RELPERSISTENCE_GLOBAL_TEMP) {
                    HnswBeginParallel(buildstate, buildstate->indexInfo->ii_Concurrent,
                        buildstate->parallel_workers, &timer);
                } else {
                    ereport(NOTICE, (errmsg("switch off parallel mode for global temp table")));
                }
            }
            if (buildstate->graph->cluster_pq && !buildstate->graph->flushed) {
                TransformPQ(buildstate, buildstate->heap, buildstate->index);
            }

            /* Add tuples to graph */
            if (buildstate->hnswleader) {
                buildstate->reltuples = ParallelHeapScan(buildstate);
            } else {
                BuildCallbackData data = { buildstate, NULL, &timer, NULL};
                buildstate->reltuples = IndexBuildHeapScan(buildstate->heap, buildstate->index,
                    buildstate->indexInfo, true, BuildCallback, (void *) &data, NULL);
            }
        }

        /* End parallel build */
        if (buildstate->hnswleader) {
            HnswEndParallel();
            pfree_ext(buildstate->hnswleader);
        }
    }

    buildstate->indtuples = buildstate->graph->indtuples;
    timer.report("Graph Build Finished");
    timer.destroy();

    /* Flush pages */
    if (!buildstate->graph->flushed) {
        FlushPages(buildstate, buildstate->index);
    }

    /* Free quantizer resources */
    if (buildstate->qt_type == QuantizerType::PQ && buildstate->pq_metainfo.graph_pq) {
        buildstate->pq_quantizer->free_resourses();
        pfree(buildstate->pq_quantizer);
    } else if (buildstate->qt_type == QuantizerType::RABITQ && buildstate->rbq_param.rbq_meta.enabled) {
        FreeBuildingRaBitQ(buildstate);
    }

    return buildstate->metablkno;
}

BlockNumber BuildHnswIndex(Relation heap, Relation index, IndexInfo *indexInfo,
    ForkNumber forkNum, int m, int efConstruction, QuantizerType qt_type, int parallel_workers,
    int maintenance_work_mem, vector_pair_vector *vec_data, double *reltuples, double *indtuples)
{
    HnswBuildState buildstate;
    InitBuildState(&buildstate, heap, index, indexInfo, forkNum, m, efConstruction,
                   qt_type, parallel_workers, maintenance_work_mem, vec_data);

    BlockNumber meta = BuildGraph(&buildstate, forkNum);

    if (!vec_data && (RelationNeedsWAL(index) || forkNum == INIT_FORKNUM)) {
        if (forkNum == MAIN_FORKNUM) {
            log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
                              true, RM_HNSW_ID, XLOG_HNSW_BUILD_INDEX, NULL, true);
        } else if (forkNum == INIT_FORKNUM) {
            log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
                              true, RM_HNSW_ID, XLOG_HNSW_UNLOG_BUILD_INDEX, NULL, true);
        }
        HnswXLogBuildAddVector(heap, index, buildstate.vector_size, forkNum, meta, buildstate.rbq_param);
    }

    if (reltuples) {
        *reltuples = buildstate.reltuples;
    }
    if (indtuples) {
        *indtuples = buildstate.indtuples;
    }

    FreeBuildState(&buildstate);

    return meta;
}

IndexBuildResult *hnswbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    BuildHnswIndex(heap, index, indexInfo, MAIN_FORKNUM, HnswGetM(index),
        HnswGetEfConstruction(index), HnswGetQuantizerType(index), HnswGetBuildParallel(index),
        u_sess->attr.attr_memory.maintenance_work_mem, NULL, &result->heap_tuples,
        &result->index_tuples);

    return result;
}

void hnswbuildempty_internal(Relation index)
{
    IndexInfo  *indexInfo = BuildIndexInfo(index);
    BuildHnswIndex(NULL, index, indexInfo, INIT_FORKNUM, HnswGetM(index),
        HnswGetEfConstruction(index), HnswGetQuantizerType(index), HnswGetBuildParallel(index),
        u_sess->attr.attr_memory.maintenance_work_mem);
}
