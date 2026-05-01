#include "access/hnsw/hnsw_quantizer.h"
#include "access/hnsw/hnsw_struct.h"
#include "access/hnsw/hnsw_xlog.h"
#include "access/hnsw/hnsw.h"
#include "access/rabitq/rabitq.h"
#include "access/rabitq/rabitq_cache.h"
#include "access/rabitq/estimator.h"
#include "utils/snapmgr.h"
#include "access/annvector/ann_utils.h"
#include "access/index_backend/index_backend.h"

using namespace rabitq;

static void SetRaBitQParam(Relation index, HnswMetaPage metap, RaBitQParam &rbq_param)
{
    rbq_param.rbq_meta = metap->quantizer_metainfo.get_rabitq_meta();
    rbq_param.applied = rbq_param.rbq_meta.enabled;
    rbq_param.dim = metap->dimensions;
    rbq_param.padded_dim = RABITQ_PADDED_DIM(rbq_param.dim);
    rbq_param.metric = metap->metric;
    rbq_param.num_vectors = metap->num_vectors;
    rbq_param.cid_size = sizeof(uint16);
    rbq_param.bin_size = RABITQ_BIN_DATA_SIZE(rbq_param.padded_dim);
    rbq_param.ext_size = RABITQ_EXT_DATA_SIZE(rbq_param.padded_dim);
    rbq_param.rbq_meta.quant_size = rbq_param.cid_size + rbq_param.bin_size + rbq_param.ext_size;
    rbq_param.quant_data = NULL;
    rbq_param.quantizer = NULL;
    rbq_param.estimator = NULL;
}

void read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data)
{
    struct BufSize {
        Buffer buf;
        size_t s;
    };
    Vector<BufSize> bufs;

    Buffer buf = ReadBuffer(index, HNSW_RABITQ_BLKNO(index));
    Page page = BufferGetPage(buf);
    size_t tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
    bufs.push_back({buf, tmp_s});
    BlockNumber next_blk = HnswPageGetOpaque(page)->nextblkno;
    while (BlockNumberIsValid(next_blk)) {
        buf = ReadBuffer(index, next_blk);
        page = BufferGetPage(buf);
        tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
        bufs.push_back({buf, tmp_s});
        next_blk = HnswPageGetOpaque(page)->nextblkno;
    }

    size_t total_size = 0;
    for (const auto &bs : bufs) {
        total_size += bs.s;
    }

	Assert(total_size == rabitq_data_size);

    char *cur = rabitq_data;
    for (auto &bs : bufs) {
        errno_t rc = memcpy_s(cur, bs.s, PageGetContents(BufferGetPage(bs.buf)), bs.s);
        securec_check(rc, "\0", "\0");
        ReleaseBuffer(bs.buf);
        cur += bs.s;
    }
    optional_destroy(bufs);
}

static void LoadRaBitQDataFixed(Relation index, size_t fixed_data_size, RaBitQCache *cache)
{
    cache->fixed_data = (char *)RABITQ_CACHE_ALLOC(fixed_data_size);
    read_rabitq_data(index, fixed_data_size, cache->fixed_data);
}

static void LoadRaBitQuantizer(Relation index, RaBitQParam &rbq_param, const RaBitQCache *cache)
{
    RaBitQuantizer *quantizer =
		NEW RaBitQuantizer(rbq_param.dim, rbq_param.padded_dim, rbq_param.metric);
    quantizer->set_rescaling_factor(rbq_param.rbq_meta.query_rescaling_factor);

    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t centroids_size = HNSW_RABITQ_NUM_CLUSTERS * rbq_param.dim;
    size_t rotated_centroids_size = HNSW_RABITQ_NUM_CLUSTERS * rbq_param.padded_dim;
    size_t total_fixed_size = random_matrix_size + (centroids_size + rotated_centroids_size) * sizeof(float);

    if (index) {
        LoadRaBitQDataFixed(index, total_fixed_size, const_cast<RaBitQCache *>(cache));
    }

    char *random_matrix = cache->fixed_data;
    float *centroids = (float *)(random_matrix + random_matrix_size);
    float *rotated_centroids = centroids + centroids_size;

    quantizer->load(random_matrix, centroids, rotated_centroids);
    rbq_param.quantizer = (void *) quantizer;
}

static void LoadRaBitQCache(Relation index, RaBitQParam &rbq_param)
{
    Oid index_oid = RelationGetRelid(index);

    LWLockAcquire(RaBitQCacheLock, LW_SHARED);
    const RaBitQCache *cache = get_rabitq_cache(index_oid);
    if (cache) {
        LoadRaBitQuantizer(NULL, rbq_param, cache);
    }
    LWLockRelease(RaBitQCacheLock);
    if (cache) {
        return;
    }

    RaBitQCache new_cache;
    new_cache.oid = index_oid;
    LoadRaBitQuantizer(index, rbq_param, &new_cache);

    LWLockAcquire(RaBitQCacheLock, LW_EXCLUSIVE);
    if (!set_rabitq_cache(new_cache)) {
        new_cache.destroy();
    }
    LWLockRelease(RaBitQCacheLock);
}

void LoadRaBitQ(Relation index, void *metapage, RaBitQParam &rbq_param, char *query)
{
    HnswMetaPage metap = (HnswMetaPage)metapage;
    SetRaBitQParam(index, metap, rbq_param);
    if (query) {
        float *half2float = NULL;
        if (metap->precision_type == DistPrecisionType::HALF) {
            half2float = alloc_floatvector(metap->dimensions);
            halfs_to_floats((half *)query, half2float, metap->dimensions);
            query = (char *)half2float;
        }
        LoadRaBitQCache(index, rbq_param);
        RaBitQEstimator *estimator = NEW RaBitQEstimator(rbq_param.padded_dim, rbq_param.metric,
            rbq_param.rbq_meta.query_rescaling_factor);
        RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
        estimator->set_quantizer(quantizer);
        estimator->preprocess((float *)query);
        rbq_param.estimator = (void *)estimator;
        if (half2float){
            free_vector(half2float);
        }
    } else {
        rbq_param.applied = false;
        rbq_param.estimator = NULL;
    }
}

static void FreeRaBitQ(RaBitQParam &rbq_param)
{
    if (rbq_param.quantizer) {
        RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
        quantizer->destroy();
        delete quantizer;
        rbq_param.quantizer = NULL;
    }
    if (rbq_param.estimator) {
        RaBitQEstimator *estimator = reinterpret_cast<RaBitQEstimator *>(rbq_param.estimator);
        estimator->perf_report();
        estimator->destroy();
        delete estimator;
        rbq_param.estimator = NULL;
    }
}

void QuantizerParam::set_resource(Relation index, void *metapage, char *query, bool building, bool set_bulk) {
    HnswMetaPage metap = (HnswMetaPage)metapage;
    QuantizerMetaInfo qt_metainfo = metap->quantizer_metainfo;
    if (get_type() == QuantizerType::PQ) {
        HnswPQMetaInfo &pq_metainfo = qt_metainfo.get_pq_metainfo();
        PQParam &pq_param = get_pq_param();
        pq_param.pq_metainfo = pq_metainfo;
        ProductQuantizer *pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
        pq->set_fvec_L2sqr_ny_nearest_func();
        pq->set_basic_values(metap->dimensions, pq_metainfo.m, pq_metainfo.nbits());
        FmgrInfo *normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
        if (metap->metric == Metric::INNER_PRODUCT && normprocinfo == NULL) {
            pq->set_fvec_ny_distance_func(Metric::INNER_PRODUCT);
            pq_param.flag = -1.0;
        } else {
            pq->set_fvec_ny_distance_func(Metric::L2);
            pq_param.flag = 1.0;
        }
        pq->set_dist_code_func();
        hnsw_read_pq_center(index, *pq);
        pq_param.code_len = pq_metainfo.code_size();
        pq_param.dist_table = alloc_floatvector(pq_metainfo.k * pq_metainfo.m);
        pq_param.pq = pq;
    } else if (get_type() == QuantizerType::RABITQ) {
        RaBitQParam &rbq_param = get_rabitq_param();
        LoadRaBitQ(index, metapage, rbq_param, query);
        rbq_param.applied = rbq_param.applied && !building;
    }
    if (set_bulk) {
        set_bulkbuf(GET_BULKBUF(index));
    } else {
        bulkbuf = NULL;
    }
}

void QuantizerParam::release_resource() {
    if (get_type() == QuantizerType::PQ) {
        PQParam &pq_param = get_pq_param();
        pq_param.pq->free_resourses();
        pfree(pq_param.pq);
        free_vector(pq_param.dist_table);
    } else if (get_type() == QuantizerType::RABITQ) {
        FreeRaBitQ(get_rabitq_param());
    }
    release_bulkbuf();
}

/* valid only if index is graph_index/hnsw */
bool HnswGetRaBitQKeepVecs(Relation index)
{
    HnswOptions *opts = (HnswOptions *)index->rd_options;
    return opts != NULL ? opts->rabitq_keep_vecs : false;
}

void QuantizeRaBitQ(RaBitQParam &rbq_param, float *vec, char *quant_data)
{
    RaBitQuantizer *quantizer = reinterpret_cast<RaBitQuantizer *>(rbq_param.quantizer);
    memset(quant_data, 0, rbq_param.rbq_meta.quant_size);
    char *bin_data = quant_data + rbq_param.cid_size;
    char *ext_data = bin_data + rbq_param.bin_size;
    int cluster_id = quantizer->quantize(vec, bin_data, ext_data);
    errno_t rc = memcpy_s(quant_data, rbq_param.cid_size, &cluster_id, rbq_param.cid_size);
    securec_check(rc, "\0", "\0");
}

void hnsw_read_pq_center(Relation index, ProductQuantizer &pq)
{
    const size_t pq_size = pq.d * pq.ksub * sizeof(float);
    LWLockAcquire(VectorPQCacheLock, LW_SHARED);
    const DiskAnnPQCache *cache = diskann_get_pq_cache(RelationGetRelid(index));
    if (cache) {
        errno_t rc = memcpy_s(pq.centroids, pq_size, cache->pivots, pq_size);
        securec_check(rc, "\0", "\0");
    }
    LWLockRelease(VectorPQCacheLock);
    if (cache) {
        return;
    }

    struct BufSize {
        Buffer buf;
        size_t s;
    };
    Vector<BufSize> bufs;
    Buffer buf = ReadBuffer(index, HNSW_PQ_BLKNO(index));
    Page page = BufferGetPage(buf);
    size_t tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
    bufs.push_back({buf, tmp_s});
    BlockNumber next_blk = HnswPageGetOpaque(page)->nextblkno;
    while (BlockNumberIsValid(next_blk)) {
        buf = ReadBuffer(index, next_blk);
        page = BufferGetPage(buf);
        tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
        bufs.push_back({buf, tmp_s});
        next_blk = HnswPageGetOpaque(page)->nextblkno;
    }

    size_t total_size = 0;
    for (const auto &bs : bufs) {
        total_size += bs.s;
    }
    Assert(pq_size == total_size);
    char *cur = (char *)pq.centroids;
    for (auto &bs : bufs) {
        errno_t rc = memcpy_s(cur, bs.s, PageGetContents(BufferGetPage(bs.buf)), bs.s);
        securec_check(rc, "\0", "\0");
        ReleaseBuffer(bs.buf);
        cur += bs.s;
    }
    optional_destroy(bufs);

    float *res = (float *)CACHE_ALLOC(total_size);
    errno_t rc = memcpy_s(res, pq_size, pq.centroids, pq_size);
    securec_check(rc, "\0", "\0");
    LWLockAcquire(VectorPQCacheLock, LW_EXCLUSIVE);
    if (!diskann_set_pq_cache(RelationGetRelid(index), res)) {
        CACHE_FREE(res);
    }
    LWLockRelease(VectorPQCacheLock);
}

static void fetch_vec_via_slot(Relation index, Relation heap, HeapTuple tuple, char *vec,
                                uint32 dim, DistPrecisionType precision_type)
{
    Oid func_oid = InvalidOid;
    int attnum = index->rd_index->indkey.values[0];
    if (attnum == 0) { /* function expression */
        if (!index->rd_indexprs) {
            RelationGetIndexExpressions(index);
        }
        FuncExpr *func_expr = (FuncExpr *)linitial(index->rd_indexprs);
        func_oid = func_expr->funcid;
        attnum = ((Var *)linitial(func_expr->args))->varattno;
    }

    Assert(heap->rd_tam_ops == TableAmHeap);
    bool is_null;
    Datum value = heap_getattr(tuple, attnum, RelationGetDescr(heap), &is_null);
    Assert(!is_null);

    Datum d;
    if (func_oid == F_ARRAY_TO_FLOATVECTOR || func_oid == F_ARRAY_TO_HALFVECTOR) {
        PGFunction func = precision_type == DistPrecisionType::FLOAT ? array_to_floatvector : array_to_halfvector;
        d = DirectFunctionCall2(func, value, Int32GetDatum(dim));
    } else if (func_oid == F_SUBFLOATVECTOR || func_oid == F_HALFVECTOR_SUBVECTOR) {
        FuncExpr *func_expr = (FuncExpr *)linitial(index->rd_indexprs);
        Assert(list_length(func_expr->args) == 3);
        ListCell *lc = func_expr->args->head->next;
        Const *c = (Const *)lfirst(lc);
        Assert(IsA(c, Const) && c->consttype == INT4OID);
        Datum arg2 = c->constvalue;
        c = (Const *)lfirst(lnext(lc));
        Assert(IsA(c, Const) && c->consttype == INT4OID);
        Datum arg3 = c->constvalue;
        PGFunction func = precision_type == DistPrecisionType::FLOAT ? subfloatvector : halfvector_subvector;
        d = DirectFunctionCall3(func, value, arg2, arg3);
    } else {
        d = value;
    }

    FloatVector *data = DatumGetFloatVector(d);
    errno_t rc = memcpy_s(vec, dim * VEC_ELEM_SIZE(precision_type), data->x, dim * VEC_ELEM_SIZE(precision_type));
    securec_check(rc, "\0", "\0");

    if (PointerGetDatum(data) != value) {
        pfree(data);
    }
}

bool fetch_vec_from_heap(Relation index, Relation heap, ItemPointerData htid, char *vec, uint32 dim, DistPrecisionType precision_type)
{
    HeapTuple tuple = (HeapTupleData *)heaptup_alloc(BLCKSZ);
    tuple->t_data = (HeapTupleHeader)((char *)tuple + HEAPTUPLESIZE);
    tuple->t_self = htid;

    Assert(heap->rd_tam_ops == TableAmHeap);
    Buffer buf;
    if (heap_fetch(heap, SnapshotAny, tuple, &buf, false, NULL) ||
        heap_fetch(heap, SnapshotNow, tuple, &buf, false, NULL)) {
        fetch_vec_via_slot(index, heap, tuple, vec, dim, precision_type);
        ReleaseBuffer(buf);
    } else {
        heap_freetuple(tuple);
        return false;
    }

    FmgrInfo *normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
    if (normprocinfo != NULL) {
        auto norm_func = get_vector_preprocess_func(Metric::FAST_COSINE, precision_type);
        norm_func(vec, dim, vec);
    }

    heap_freetuple(tuple);
    return true;
}

ItemPointerData get_heap_tid(Relation index, ItemPointerData indexTid)
{
    Buffer buf = ReadBuffer(index, ItemPointerGetBlockNumber(&indexTid));
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buf);
    HnswTuple tuple = (HnswTuple)PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(&indexTid)));
    ItemPointerData heapTid = tuple->heaptids[0];
    UnlockReleaseBuffer(buf);
    return heapTid;
}

void store_centroids(Relation index, const float *center, size_t write_size,
    bool building, bool enabling, bool updating)
{
    char *cur = (char *)center;
    Buffer buf = ReadBuffer(index, HNSW_PQ_BLKNO(index));
    Buffer new_buf = InvalidBuffer;
    for (;;) {
        Page page = BufferGetPage(buf);
        PageHeader phdr = (PageHeader)page;
        size_t fs = 0;
        char *write_offset = 0;
        if (building || enabling) {
            HnswInitPage(buf, page);
            fs = phdr->pd_upper - phdr->pd_lower;
            write_offset = page + phdr->pd_lower;
        } else if (updating) {
            fs = 8160;
            write_offset = page + 24;
        }
        size_t ws = std::min(fs, write_size);
        errno_t rc = memcpy_s(write_offset, fs, cur, ws);
        securec_check(rc, "\0", "\0");
        if (building || enabling) {
            phdr->pd_lower += ws;
        }
        cur += ws;
        write_size -= ws;
        if (write_size > 0) {
            HnswPageOpaque opaque = HnswPageGetOpaque(page);
            if (building || enabling) {
                new_buf = ReadBuffer(index, P_NEW);
                opaque->nextblkno = BufferGetBlockNumber(new_buf);
            } else if (updating) {
                new_buf = ReadBuffer(index, opaque->nextblkno);
            }
        }
        MarkBufferDirty(buf);
        if (enabling || updating) {
            HnswXLogWriteCentroids(index, buf, page);
        }
        ReleaseBuffer(buf);
        if (write_size == 0) {
            break;
        }
        buf = new_buf;
    }
}

void qt_update_init(knl_g_annvec_context *cxt)
{
    MemoryContext qt_update_cxt = AllocSetContextCreate(cxt->ann_cxt, "Quantizer Update Context",
        ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);
    cxt->qt_update_cxt = qt_update_cxt;
    auto old_ctx = MemoryContextSwitchTo(qt_update_cxt);
    QtUpdateMgr *qt_update_mgr = (QtUpdateMgr *)palloc(sizeof(QtUpdateMgr));
    new (&qt_update_mgr->qt_record_map) UnorderedMap<Oid, QtUpdatingBuffer *>;
    SpinLockInit(&qt_update_mgr->qt_record_maplock);
    new (&qt_update_mgr->qt_updating_set) UnorderedSet<Oid>;
    SpinLockInit(&qt_update_mgr->qt_updating_setlock);
    new (&qt_update_mgr->qt_timering_map) UnorderedMap<Oid, TimeRing *>;
    SpinLockInit(&qt_update_mgr->qt_timering_maplock);
    cxt->qt_update_mgr = qt_update_mgr;
    MemoryContextSwitchTo(old_ctx);
}

bool QtUpdateMgr::insert_updating(Oid oid)
{
    bool success = false;
    SpinLockAcquire(&qt_updating_setlock);
    if (!qt_updating_set.contains(oid)) {
        qt_updating_set.emplace(oid);
        success = true;
    }
    SpinLockRelease(&qt_updating_setlock);
    return success;
}

void QtUpdateMgr::erase_updating(Oid oid)
{
    /* no parallel */
    SpinLockAcquire(&qt_updating_setlock);
    qt_updating_set.erase(oid);
    SpinLockRelease(&qt_updating_setlock);
}

bool QtUpdateMgr::contain_updating(Oid oid)
{
    SpinLockAcquire(&qt_updating_setlock);
    bool contain = qt_updating_set.contains(oid);
    SpinLockRelease(&qt_updating_setlock);
    return contain;
}

void QtUpdateMgr::insert_record(Oid oid)
{
    /* no parallel */
    SpinLockAcquire(&qt_record_maplock);
    QtUpdatingBuffer *buffer = (QtUpdatingBuffer *)palloc(sizeof(QtUpdatingBuffer));
    new (buffer) QtUpdatingBuffer();
    qt_record_map.emplace(oid, buffer);
    SpinLockRelease(&qt_record_maplock);
}

Vector<Slot> *QtUpdateMgr::erase_record(Oid oid)
{
    /* no parallel */
    SpinLockAcquire(&qt_record_maplock);
    auto it = qt_record_map.find(oid);
    QtUpdatingBuffer *buffer = it->second;
    qt_record_map.erase(it);
    SpinLockRelease(&qt_record_maplock);
    Vector<Slot> *record_vec = &buffer->record_vec;
    pfree(buffer);
    return record_vec;
}

QtUpdatingBuffer *QtUpdateMgr::find_record(Oid oid)
{
    bool find = true;
    SpinLockAcquire(&qt_record_maplock);
    auto it = qt_record_map.find(oid);
    find = it != qt_record_map.end();
    SpinLockRelease(&qt_record_maplock);
    return find ? it->second : NULL;
}

TimeRing *QtUpdateMgr::insert_timgring(Oid oid)
{
    SpinLockAcquire(&qt_timering_maplock);
    if (!qt_timering_map.contains(oid)) {
        auto old_ctx = MemoryContextSwitchTo(g_instance.diskann_cxt.vec_indexer_ctx);
        TimeRing *timering = (TimeRing *)palloc(sizeof(TimeRing));
        new (timering) TimeRing();
        qt_timering_map.emplace(oid, timering);
        MemoryContextSwitchTo(old_ctx);
        SpinLockRelease(&qt_timering_maplock);
        return timering;
    } else {
        SpinLockRelease(&qt_timering_maplock);
        return NULL;
    }
}

TimeRing *QtUpdateMgr::find_timering(Oid oid)
{
    bool find = true;
    SpinLockAcquire(&qt_timering_maplock);
    auto it = qt_timering_map.find(oid);
    find = it != qt_timering_map.end();
    SpinLockRelease(&qt_timering_maplock);
    return find ? it->second : NULL;
}

void QtUpdateMgr::erase_timering(Oid oid)
{
    SpinLockAcquire(&qt_timering_maplock);
    auto it = qt_timering_map.find(oid);
    if (it != qt_timering_map.end()) {
        TimeRing *timering = it->second;
        qt_timering_map.erase(it);
        pfree(timering);
    }
    SpinLockRelease(&qt_timering_maplock);
}

Datum index_qtupdate(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), ShareRowExclusiveLock);
    if (RelationIsPartitioned(index)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("cannot update partitioned table")));
    }
    Buffer metabuf = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    HnswMetaPage metap = HnswPageGetMeta(BufferGetPage(metabuf));
    size_t num_vectors = metap->num_vectors;
    QuantizerMetaInfo qt_metainfo = metap->quantizer_metainfo;
    UnlockReleaseBuffer(metabuf);
    if (num_vectors < HNSW_MIN_QT_SAMPLES_SIZE) {
        index_close(index, ShareRowExclusiveLock);
        PG_RETURN_BOOL(false);
    }
    QtUpdateMgr *qt_update_mgr = (QtUpdateMgr *)g_instance.annvec_cxt.qt_update_mgr;
    QuantizerUpdateParam param;
    param.qt_type = qt_metainfo.get_setting_type();
    param.enable = qt_metainfo.get_type() != QuantizerType::NONE;
    param.metablkno = 0;
    param.freq_10min = 0;
    param.force = true;
    add_quantizer_update_task(index, &param);
    qt_update_mgr->insert_updating(index->rd_id);
    index_close(index, ShareRowExclusiveLock);
    PG_RETURN_BOOL(true);
}
