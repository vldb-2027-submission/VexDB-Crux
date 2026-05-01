#include "knl/knl_instance.h"
#include "executor/executor.h"
#include "utils/elog.h"
#include "commands/cluster.h"
#include "funcapi.h"
#include "utils/fmgroids.h"
#include "catalog/index.h"
#include "access/hnsw/hnsw.h"
#include "access/annvector/store/bulkbuf_smgr.h"
#include "utils/resowner.h"
#include "access/annvector/ann_utils.h"
#include "access/bm25/index_inspect.h"

using namespace ann_helper;

static uint32 calculate_pow(uint32 elem_size) {
    constexpr uint32 target_size = 24 * 1024 * 1024u; /* 24MB */
    double target_count = static_cast<double>(target_size) / elem_size;
    double x_double = LOG2(target_count);
    int x = static_cast<int>(round(x_double));
    return x;
}

static char *allocate_aligned_memory(size_t alloc_bytes) {
    constexpr size_t extra = vector_aligned_size + sizeof(void *);
    char *original_ptr = (char *)palloc(sizeof(char) * (alloc_bytes + extra));
    uint64 raw_addr = (uint64)original_ptr + sizeof(void *);
    uint64 aligned_addr = (raw_addr + vector_aligned_size - 1) & ~(vector_aligned_size - 1);
    char *aligned_ptr = reinterpret_cast<char *>(aligned_addr);
    return aligned_ptr;
}

BulkBuffer::BulkBuffer(Relation r, MemoryContext ctx, size_t e_num, uint32 store_esize, VecStorageType type)
    : id(r->rd_smgr->smgr_rnode.node.relNode), ctx(ctx), store_elem_size(store_esize), vec_storage_type(type) {
    if (store_elem_size % vector_aligned_size == 0) {
        elem_size = store_elem_size;
    } else {
        elem_size = ((store_elem_size / vector_aligned_size) + 1) *
            vector_aligned_size;
    }
    total_elem_nums = e_num;
    pow_elem_nums_per_chunk = calculate_pow(elem_size);
    one_chunk_elem_nums = 1 << pow_elem_nums_per_chunk;
    chunk_size = elem_size * one_chunk_elem_nums;
    size_t chunk_nums = 0;
    chunk_nums = e_num > 0 ? (get_chunk_no(e_num) + 1) : 0;
    new (&vec) Vector<Chunk> (chunk_nums);
    for (size_t i = 0; i < chunk_nums; ++i) {
        char *aligned_ptr = allocate_aligned_memory(chunk_size);
        vec.emplace_back(aligned_ptr);
    }
    for (size_t i = 0; i < chunk_nums; ++i) {
        load_one_chunk(r, i);
    }
    SpinLockInit(&mutex);
    PthreadRwLockInit(&lock, NULL);
    visit_count = 0;
    load_time = GetCurrentTimestamp();
}

void BulkBuffer::load_one_chunk(Relation index, size_t chunk_no) {
    int tempbuf_size = 0;
    uint32 copy_elem_nums = 0;
    bool is_tail = unlikely(chunk_no == vec.size() - 1) ? true : false;
    if (is_tail) {
        uint32 tail_elem_nums = total_elem_nums - chunk_no * one_chunk_elem_nums;
        tempbuf_size = store_elem_size * tail_elem_nums;
        copy_elem_nums = tail_elem_nums;
    } else {
        tempbuf_size = store_elem_size * one_chunk_elem_nums;
        copy_elem_nums = one_chunk_elem_nums;
    }
    char *tempbuf = (char *)palloc0(sizeof(char) * tempbuf_size);
    off_t offset = chunk_no * store_elem_size * one_chunk_elem_nums;
    SMGR_READ_STATUS status = vec_read(index->rd_smgr, offset, tempbuf_size, tempbuf, vec_storage_type);
    if (unlikely(status != SMGR_RD_OK)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("could not read elem at loc %ldu in relation \"%s\"",
                            offset, index->rd_rel->relname.data)));
    }
    if (store_elem_size % vector_aligned_size == 0) {
        errno_t rc = memcpy_s(vec[chunk_no].buf, tempbuf_size, tempbuf, tempbuf_size);
        securec_check_c(rc, "\0", "\0");
    } else {
        for (uint32 i = 0; i < copy_elem_nums; ++i) {
            char *dest = vec[chunk_no].buf + i * elem_size;
            char *src = tempbuf + i * store_elem_size;
            errno_t rc = memcpy_s(dest, store_elem_size, src, store_elem_size);
            securec_check_c(rc, "\0", "\0");
        }
    }
    pfree(tempbuf);
}

uint32 BulkBuffer::get_chunk_no(size_t idx) {
    return idx >> pow_elem_nums_per_chunk;
}

uint32 BulkBuffer::get_chunk_offset(size_t idx) {
    return (idx & ((1 << pow_elem_nums_per_chunk) - 1)) * elem_size;
}

char *BulkBuffer::get(size_t idx) {
#if STATISTIC
    visit_count.fetch_add(1, std::memory_order_relaxed);
#endif
    uint32 chunk_no = get_chunk_no(idx);
    uint32 chunk_offset = get_chunk_offset(idx);
    return &vec[chunk_no].buf[chunk_offset];
}

void BulkBuffer::update(size_t idx, char *value) {
    uint32 chunk_no = get_chunk_no(idx);
    uint32 chunk_offset = get_chunk_offset(idx);

#if STATISTIC
    size_t current_total = total_elem_nums.load(std::memory_order_acquire);
    if (idx >= current_total) {
        size_t new_total = idx + 1;
        while (!total_elem_nums.compare_exchange_weak(current_total, new_total,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (idx < current_total) {
                break;
            }
            new_total = idx + 1;
        }
    }
#endif

    /* if no need to append, directly update */
    if (chunk_no < vec.size()) {
        char *dest = &vec[chunk_no].buf[chunk_offset];
        errno_t rc = memcpy_s(dest, store_elem_size, value, store_elem_size);
        securec_check_c(rc, "\0", "\0");
        return;
    }
    
    /* need append but no need realloc */
    auto old_ctx = MemoryContextSwitchTo(ctx);
    SpinLockAcquire(&mutex);
    if (chunk_no < vec.capacity()) {
        for (uint32 i = vec.size(); i < chunk_no + 1; ++i) {
            char *aligned_ptr = allocate_aligned_memory(chunk_size);
            vec.emplace_back(aligned_ptr);
        }
        SpinLockRelease(&mutex);
        char *dest = &vec[chunk_no].buf[chunk_offset];
        errno_t rc = memcpy_s(dest, store_elem_size, value, store_elem_size);
        securec_check_c(rc, "\0", "\0");
        MemoryContextSwitchTo(old_ctx);
        return;
    }
    SpinLockRelease(&mutex);

    /* need realloc */
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &lock);
    PthreadRWlockWrlock(LOCAL_SYSDB_RESOWNER, &lock);
    for (uint32 i = vec.size(); i < chunk_no + 1; ++i) {
        char *aligned_ptr = allocate_aligned_memory(chunk_size);
        vec.emplace_back(aligned_ptr);
    }
    char *dest = &vec[chunk_no].buf[chunk_offset];
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &lock);
    errno_t rc = memcpy_s(dest, store_elem_size, value, store_elem_size);
    securec_check_c(rc, "\0", "\0");
    MemoryContextSwitchTo(old_ctx);
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &lock);
}

void BulkBuffer::acquire() {
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &lock);
}

void BulkBuffer::release() {
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &lock);
}

void BulkBuffer::destroy() {
    MemoryContextDelete(ctx);
}

BulkBufferInspect BulkBuffer::get_inspect() {
    int64 used_space = 0;
    acquire();
    CalculateContextSize(ctx, &used_space);
    size_t count = visit_count.load(std::memory_order_acquire);
    char *name = pstrdup(ctx->name);
    size_t elem_nums = total_elem_nums.load(std::memory_order_acquire);
    auto sf_size = format_size(elem_size);
    release();
    auto sf_space = format_size(used_space);
    char *space = psprintf("%.2f %s", sf_space.n, sf_space.unit_str());
    char *size = psprintf("%.0f %s", sf_size.n, sf_size.unit_str());
    return BulkBufferInspect(name, space, size, elem_nums, count, load_time);
}

BulkBuffer *BulkBufferManager::get_bulkbuf(Relation index) {
    Oid rel_id = index->rd_id;
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    auto it = visit_map.find(rel_id);
    if (it != visit_map.end()) {
        BulkBuffer *bulkbuf = it->second;
        bulkbuf->acquire();
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        return bulkbuf;
    } else {
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        return NULL;
    }
}

Vector<BulkBufferInspect> BulkBufferManager::get_inspect() {
    Vector<BulkBufferInspect> res;
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    for (auto it : visit_map) {
        BulkBuffer *bulkbuf = it.second;
        res.push_back(bulkbuf->get_inspect());
    }
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    return res;
}

void BulkBufferManager::rename_ctx(Oid rel_id, const char *new_name) {
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    auto it = visit_map.find(rel_id);
    if (it != visit_map.end()) {
        MemoryContext ctx = it->second->ctx;
        auto old_ctx = MemoryContextSwitchTo(ctx);
        if (ctx->name != (char *)ctx->methods + sizeof(MemoryContextMethods)) {
            pfree(ctx->name);
        }
        ctx->name = pstrdup(new_name);
        MemoryContextSwitchTo(old_ctx);
    }
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
}

bool BulkBufferManager::get_array(Relation index, const char *ctx_name, size_t e_num,
    uint32 store_esize, VecStorageType vec_storage_type) {
    Oid rel_id = index->rd_id;
    bool insert = false;
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    auto it = visit_map.find(rel_id);
    if (it == visit_map.end()) {
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        PthreadRWlockWrlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        it = visit_map.find(rel_id);
        if (it == visit_map.end()) {
            auto ctx = AllocSetContextCreate(BULKBUF_CTX, ctx_name, 
                ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);
            MemoryContext old_ctx = MemoryContextSwitchTo(ctx);
            BulkBuffer *bulkbuf = NULL;
            PG_TRY(); {
                bulkbuf = NEW BulkBuffer(index, ctx, e_num, store_esize, vec_storage_type);
            };
            PG_CATCH(); {
                PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
                MemoryContextDelete(ctx);
                PG_RE_THROW();
            }
            PG_END_TRY();
            if (bulkbuf) {
                MemoryContextSwitchTo(BULKBUF_CTX);
                visit_map.emplace(rel_id, bulkbuf);
                insert = true;
            }
            MemoryContextSwitchTo(old_ctx);
        }
    }
    PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    return insert;
}

BulkBufferManager::BulkBufferManager() {
    new (&visit_map) UnorderedMap<Oid, BulkBuffer *>;
    PthreadRwLockInit(&visit_map_lock, NULL);
}

void BulkBufferManager::destroy() {
    for (auto it = visit_map.begin(); it != visit_map.end(); ++it) {
        it->destroy();
    }
    optional_destroy(visit_map);
}

bool BulkBufferManager::index_load(Relation index, const char *ctx_name) {
    bool insert = false;
    if (index->rd_am->ambuild == F_HNSWBUILD) {
        Buffer metabuf = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
        LockBuffer(metabuf, BUFFER_LOCK_SHARE);
        HnswMetaPage metap = HnswPageGetMeta(BufferGetPage(metabuf));
        uint32 dim = metap->dimensions;
        size_t num_vectors = metap->num_vectors;
        QuantizerMetaInfo qt_metainfo = metap->quantizer_metainfo;
        UnlockReleaseBuffer(metabuf);
        QuantizerType qt_type = qt_metainfo.get_type();
        if (qt_type == QuantizerType::PQ) {
            const HnswPQMetaInfo &pq_metainfo = qt_metainfo.get_pq_metainfo();
            uint32 code_len = pq_metainfo.code_size();
            insert = get_array(index, ctx_name, num_vectors, code_len, VecStorageType::PureCode);
        } else if (qt_type == QuantizerType::RABITQ) {
            RaBitQMeta &rbq_meta = qt_metainfo.get_rabitq_meta();
            VecStorageType st = rbq_meta.keep_vecs ? VecStorageType::CodeWithVec : VecStorageType::PureCode;
            insert = get_array(index, ctx_name, num_vectors, rbq_meta.quant_size, st);
        } else if (qt_type == QuantizerType::NONE) {
            const size_t vec_size = VEC_ELEM_SIZE(metap->precision_type) * dim;
            insert = get_array(index, ctx_name, num_vectors, vec_size, VecStorageType::PureVec);
        }
        return insert;
    }
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index_memory_load does not support \"%s\"", NameStr(index->rd_am->amname))));
    __builtin_unreachable();
}

bool BulkBufferManager::index_release(Relation index, bool need_notice) {
    Oid rel_id = index->rd_id;
    return index_release(rel_id, need_notice);
}

bool BulkBufferManager::index_release(Oid rel_id, bool need_notice) {
    PthreadRWlockRdlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
    auto it = visit_map.find(rel_id);
    if (it == visit_map.end()) {
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        return false;
    } else {
        BulkBuffer *bulkbuf = it->second;
        
        /* notice if needed */
        if (need_notice) {
            ereport(NOTICE, (errcode(ERRCODE_LOG),
                errmsg("index \"%s\" will be released from bulk buffer", bulkbuf->ctx->name)));
        }

        /* release read lock and acquire write lock */
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);
        PthreadRWlockWrlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);

        /* erase from map, other thread cannot find this bulkbuf anymore */
        visit_map.erase(rel_id);
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &visit_map_lock);

        /* waiting for other threads exit, they get this bulkbuf before erase and still using */
        PthreadRWlockWrlock(LOCAL_SYSDB_RESOWNER, &bulkbuf->lock);

        /* now only this thread holding this bulkbuf, unlock and destroy */
        PthreadRWlockUnlock(LOCAL_SYSDB_RESOWNER, &bulkbuf->lock);
        bulkbuf->destroy();
        
        return true;
    }
}

bool BulkBufferManager::auto_index_release(Relation index) {
    if (!RelationIsPartitioned(index) || RelationIsPartition(index)) {
        return index_release(index, true);
    }
    List *l = indexGetPartitionList(index, AccessShareLock);
    foreach_cell(lc, l) {
        Partition part = (Partition)lfirst(lc);
        Relation cindex = partitionGetRelation(index, part);
        index_release(cindex, true);
        releaseDummyRelation(&cindex);
    }
    releasePartitionList(index, &l, AccessShareLock);
    return true;
}

void BulkBufferManager::auto_partindex_release(Oid partindex_oid) {
    index_release(partindex_oid, true);
}

void BulkBufferManager::auto_partindex_release(Relation parttable_rel, Oid part_oid) {
    List *indexOidList = RelationGetIndexList(parttable_rel);
    ListCell *indList = NULL;
    foreach (indList, indexOidList) {
        Oid indexId = lfirst_oid(indList);
        Relation indexRel = index_open(indexId, AccessShareLock);
        if (!BULKBUF_SUPPORT(indexRel)) {
            index_close(indexRel, AccessShareLock);
            continue;
        } else {
            index_close(indexRel, AccessShareLock);
            Oid indexPartitionOid = indexIdAndPartitionIdGetIndexPartitionId(indexId, part_oid);
            index_release(indexPartitionOid, true);
        }
    }
    list_free_ext(indexOidList);
}

void init_bulkbuf_smgr() {
    auto old_ctx = MemoryContextSwitchTo(g_instance.diskann_cxt.bulk_buf_ctx);
    g_instance.diskann_cxt.bulkbuf_mgr = (BulkBufferManager *)palloc(sizeof(BulkBufferManager));
    new (g_instance.diskann_cxt.bulkbuf_mgr) BulkBufferManager();
    MemoryContextSwitchTo(old_ctx);
}

static void index_usable_check(Relation index) {
    if (!IndexIsUsable(index->rd_index)) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("Cannot load unusabled index \"%s\" to bulk buffer.",
            RelationGetRelationName(index))));
    }
}

static void index_usable_check(Partition part, char *name) {
    if (!part->pd_part->indisusable) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("Cannot load unusabled index \"%s\" to bulk buffer.", name)));
    } 
}

static void index_partitioned_check(Relation index) {
    if (!RelationIsPartitioned(index)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("Index is not partitioned."),
            errhint("Please input index name/oid only")));
    }
}

static bool index_memory_release(Relation index) {
    return BULKBUF_MGR->index_release(index);
}

void BulkBufferManager::index_load(Relation index, Oid part_oid) {
    if (OidIsValid(part_oid)) {
        index_partitioned_check(index);
        Partition part = partitionOpen(index, part_oid, ShareRowExclusiveLock);
        Relation cindex = partitionGetRelation(index, part);
        index_usable_check(part, RelationGetRelationName(cindex));
        BULKBUF_MGR->index_load(cindex, PartitionGetPartitionName(part));
        releaseDummyRelation(&cindex);
        partitionClose(index, part, ShareRowExclusiveLock);
    } else {
        if (!RelationIsPartitioned(index)) {
            index_usable_check(index);
            BULKBUF_MGR->index_load(index, RelationGetRelationName(index));
        } else {
            /* scan over all partition */
            List *l = indexGetPartitionList(index, ShareRowExclusiveLock);
            PG_TRY(); {
                foreach_cell(lc, l) {
                    Partition part = (Partition)lfirst(lc);
                    Relation cindex = partitionGetRelation(index, part);
                    index_usable_check(part, RelationGetRelationName(cindex));
                    BULKBUF_MGR->index_load(cindex, PartitionGetPartitionName(part));
                    releaseDummyRelation(&cindex);
                }
                releasePartitionList(index, &l, ShareRowExclusiveLock);
            }
            PG_CATCH(); {
                foreach_cell(lc, l) {
                    Partition part = (Partition)lfirst(lc);
                    Relation cindex = partitionGetRelation(index, part);
                    if (!part->pd_part->indisusable) {
                        break;
                    }
                    index_memory_release(cindex);
                }
                PG_RE_THROW();
            }
            PG_END_TRY();
        }
    }
}

Datum index_memory_load_oid(PG_FUNCTION_ARGS) {
    if (!superuser()) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("Only system admin can call function index_memory_load.")));
    }
    Relation index = index_open(PG_GETARG_OID(0), ShareRowExclusiveLock);
    Oid part_oid = PG_GETARG_OID(1);
    BULKBUF_MGR->index_load(index, part_oid);
    index_close(index, ShareRowExclusiveLock);
    PG_RETURN_BOOL(true);
}

Datum index_memory_load_name(PG_FUNCTION_ARGS) {
    if (!superuser()) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("Only system admin can call function index_memory_load.")));
    }
    Relation index = index_open(PG_GETARG_OID(0), ShareRowExclusiveLock);
    index_usable_check(index);
    index_partitioned_check(index);
    char *part_name = text_to_cstring(PG_GETARG_TEXT_P(1));
    Oid part_id = PartitionNameGetPartitionOid(PG_GETARG_OID(0), part_name,
        PART_OBJ_TYPE_INDEX_PARTITION, ShareRowExclusiveLock, false, false, NULL, NULL, NoLock);
    pfree(part_name);
    Partition part = partitionOpen(index, part_id, ShareRowExclusiveLock);
    Relation cindex = partitionGetRelation(index, part);
    index_usable_check(part, RelationGetRelationName(cindex));
    BULKBUF_MGR->index_load(cindex, PartitionGetPartitionName(part));
    releaseDummyRelation(&cindex);
    partitionClose(index, part, ShareRowExclusiveLock);
    index_close(index, ShareRowExclusiveLock);
    PG_RETURN_BOOL(true);
}

Datum index_memory_release_oid(PG_FUNCTION_ARGS) {
    if (!superuser()) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("Only system admin can call function index_memory_release.")));
    }
    bool success = false;
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    Oid part_id = PG_GETARG_OID(1);
    if (OidIsValid(part_id)) {
        index_partitioned_check(index);
        Partition part = partitionOpen(index, part_id, AccessShareLock);
        Relation cindex = partitionGetRelation(index, part);
        success = index_memory_release(cindex);
        releaseDummyRelation(&cindex);
        partitionClose(index, part, AccessShareLock);
        index_close(index, AccessShareLock);
    } else {
        if (!RelationIsPartitioned(index)) {
            success = index_memory_release(index);
        } else {
            /* scan over all partition */
            List *l = indexGetPartitionList(index, AccessShareLock);
            foreach_cell(lc, l) {
                Partition part = (Partition)lfirst(lc);
                Relation cindex = partitionGetRelation(index, part);
                success = index_memory_release(cindex) ? true : success;
                releaseDummyRelation(&cindex);
            }
            releasePartitionList(index, &l, AccessShareLock);
        }
        index_close(index, AccessShareLock);
    }
    PG_RETURN_BOOL(success);
}

Datum index_memory_release_name(PG_FUNCTION_ARGS) {
    if (!superuser()) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("Only system admin can call function index_memory_release.")));
    }
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    index_partitioned_check(index);
    char *part_name = text_to_cstring(PG_GETARG_TEXT_P(1));
    Oid part_id = PartitionNameGetPartitionOid(PG_GETARG_OID(0), part_name,
        PART_OBJ_TYPE_INDEX_PARTITION, AccessShareLock, false, false, NULL, NULL, NoLock);
    pfree(part_name);
    Partition part = partitionOpen(index, part_id, AccessShareLock);
    Relation cindex = partitionGetRelation(index, part);
    bool success = index_memory_release(cindex);
    releaseDummyRelation(&cindex);
    partitionClose(index, part, AccessShareLock);
    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(success);
}

Datum bulkbuffer_inspect(PG_FUNCTION_ARGS)
{
    /* name, used_space, elem_size, elem_nums, visit_count, load_time */
    FuncCallContext *funcctx;
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);        
        Vector<BulkBufferInspect> inspect_results = BULKBUF_MGR->get_inspect();
        TupleDesc tupdesc = CreateTemplateTupleDesc(6, false);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "name", CSTRINGOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "used_space", CSTRINGOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)3, "elem_size", CSTRINGOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)4, "elem_nums", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)5, "visit_count", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)6, "load_time", TIMESTAMPTZOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = inspect_results.size();
        funcctx->user_fctx = inspect_results.data();
        MemoryContextSwitchTo(oldcontext);
    }
    funcctx = SRF_PERCALL_SETUP();
    if (funcctx->user_fctx && funcctx->call_cntr < funcctx->max_calls) {
        BulkBufferInspect *inspect = (BulkBufferInspect *)funcctx->user_fctx + funcctx->call_cntr;
        Datum values[6];
        bool nulls[6] = {false};        
        values[0] = CStringGetDatum(inspect->name);
        values[1] = CStringGetDatum(inspect->used_space);
        values[2] = CStringGetDatum(inspect->elem_size);
        values[3] = Int64GetDatum(inspect->elem_nums);
        values[4] = Int64GetDatum(inspect->visit_count);
        values[5] = TimestampTzGetDatum(inspect->load_time);
        
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    SRF_RETURN_DONE(funcctx);
}
