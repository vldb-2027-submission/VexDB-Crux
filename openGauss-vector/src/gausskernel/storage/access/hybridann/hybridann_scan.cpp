#include "access/relscan.h"
#include "pgstat.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/fmgroids.h"
#include "catalog/pg_cast.h"
#include "optimizer/cost.h"
#include "access/annvector/module/perf_usage.h"
#include "access/hybridann/bplustree/disk_bplustree.h"
#include "access/hybridann/hybridann.h"
#include "access/relscan.h"
#include "access/tableam.h"

using namespace ann_helper;
using namespace disk_container;

#define BTREE_STAT_LOG_LEVEL DEBUG1
#define OUTPUT_BTVEC_SEARCH_LOG false
#if OUTPUT_BTVEC_SEARCH_LOG && !defined(__OPTIMIZE__)
#define BTVEC_SEARCH_LOG(fmt, ...) ereport(NOTICE, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define BTVEC_SEARCH_LOG(fmt, ...)
#endif /* OUTPUT_BTVEC_SEARCH_LOG */

struct ArrayKey {
    uint32 nitems;
    uint32 cur_idx;
    uint32 key_idx;
    Datum *items;
};

struct BPTreeScanOpaque {
    Relation    index;
    ItemPointer tids;
    uint32      searchListSize;
    uint32      dimensions;
    int         currIndex;
    int         lastIndex;
    bool        first;
    bool no_more_data;
    bool can_iterate;
    bool require_recheck;

    BlockNumber btreeMetaBlkNo;
    BlockNumber nextBlkno;
    MemoryContext scan_cxt;

    int32 nkeys;
    ScanKey keyData;
    uint32 narrkeys;
    ArrayKey *arrkeys;

    TupleDesc tupleDesc;
    BTTupleScanKey *scankey;

    DiskNodeImpl node;
    OffsetNumber offset;
};

/*
 * Prepare for an index scan
 */
IndexScanDesc hybridannbeginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);

    Buffer buf = AnnLoadBuffer(index, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = (HybridAnnMetaPage *)(HybridAnnPageGetMeta(BufferGetPage(buf)));

    if (unlikely(meta->magicNumber == DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please REINDEX.")));
    }

    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybrid index is not valid")));
    }

    auto *so = (BPTreeScanOpaque *)palloc(sizeof(BPTreeScanOpaque));

    so->index = index;
    so->first = true;

    so->searchListSize = u_sess->attr.attr_storage.ef_search;

    so->dimensions = meta->dimensions;

    so->tids = NULL;
    so->currIndex = so->lastIndex = 0;

    so->scan_cxt = NULL;
    so->keyData = NULL;
    so->arrkeys = NULL;
    so->tupleDesc = NULL;
    so->scankey = NULL;
    so->btreeMetaBlkNo = meta->BTMetaBlkNo;
    so->nextBlkno = InvalidBlockNumber;

    UnlockReleaseBuffer(buf);

    scan->opaque = so;
    return scan;
}

void hybridannendscan_internal(IndexScanDesc scan)
{
    BPTreeScanOpaque *so = (BPTreeScanOpaque *) scan->opaque;
    pfree_ext(so->keyData);
    pfree_ext(so->arrkeys);
    if (so->scankey) {
        optional_destroy(*so->scankey);
        delete so->scankey;
        so->scankey = NULL;
    }

    if (so->scan_cxt) {
        MemoryContextDelete(so->scan_cxt);
    }

    pfree_ext(so->tupleDesc);

    pfree_ext(so->tids);
    pfree(so);
    scan->opaque = NULL;
}

/*
 * Start or restart an index scan
 */
void hybridannrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    auto *so = (BPTreeScanOpaque *)scan->opaque;
    so->first = true;
    so->searchListSize = u_sess->attr.attr_storage.ef_search;
    pfree_ext(so->keyData);
    pfree_ext(so->arrkeys);
    
    pfree_ext(so->tids);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }

    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
    }
}


static BTTupleData *btree_check_keys(BPTreeScanOpaque *btso, OffsetNumber offset, bool &no_more_data)
{
    BTTupleData *data = btso->node.get_data(offset);
    bool can_forward;
    if ((*btso->scankey)(*data, can_forward)) {
        return data;
    }
    no_more_data = !can_forward;
    return NULL;
}

static void btree_release_node(BPTreeScanOpaque *btso)
{
    btso->node.r_unlock();
    btso->node.destroy();
}

static void btree_read_node(BPTreeScanOpaque *btso)
{
    bool no_more_data = false;
    int32 index = 0;
    OffsetNumber maxOffset = btso->node.size();
    OffsetNumber offset = Max(btso->offset, p_firstdatakey(btso->node));
    while (offset <= maxOffset) {
        BTTupleData *data = btree_check_keys(btso, offset, no_more_data);
        offset = OffsetNumberNext(offset);
        if (data) {
            btso->tids[index++] = data->t_tid;
        } else if (no_more_data) {
            break;
        }
    }
    btso->nextBlkno = btso->node.next();
    btree_release_node(btso);
    btso->no_more_data = no_more_data;
    btso->lastIndex = index - 1;
    btso->currIndex = 0;
}

static void btree_next_node(BPTreeScanOpaque *so, BlockNumber nextblkno)
{
    so->node = DiskNodeImpl(so->index, nextblkno);
    so->node.r_lock();
    so->offset = p_firstdatakey(so->node);
}

static BTTupleScanKey *btree_scan_key(IndexScanDesc scan)
{
    auto *so = (BPTreeScanOpaque *) scan->opaque;
    Relation rel = so->index;
    Oid *collation = rel->rd_indcollation;

    int nattrs = RelationGetDescr(so->index)->natts;
    int nkeys = so->nkeys;

    MemoryContext oldcxt = MemoryContextSwitchTo(so->scan_cxt);
    Datum lower[nattrs];
    Datum upper[nattrs];
    bool lower_inclusive[nattrs];
    bool upper_inclusive[nattrs];
    bool lower_nulls[nattrs];
    bool upper_nulls[nattrs];
    bool equals[nattrs];
    FmgrInfo *proc[nattrs];
    std::fill_n(lower_nulls, nattrs, true);
    std::fill_n(upper_nulls, nattrs, true);
    std::fill_n(equals, nattrs, false);
    for (int i = 1; i < nattrs; ++i) {
        proc[i] = index_getprocinfo(so->index, i + 1, HYBRID_BTORDER_PROC);
    }
    for (int i = 0; i < nkeys; ++i) {
        ScanKey key = &so->keyData[i];
        int attno = key->sk_attno;
        Assert(attno > 0 && attno <= nattrs);
        if (attno == 1) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("Vector key not supported")));
        }
        --attno;
        if (key->sk_flags & SK_ISNULL) {
            MemoryContextSwitchTo(oldcxt);
            if (key->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("Hybridann index currently does not support NULL clause")));
            }
            return NULL;
        }

        const auto set_lower = [&](int attno, Datum value, bool inclusive) {
            if (lower_nulls[attno]) {
                lower[attno] = value;
                lower_inclusive[attno] = inclusive;
                lower_nulls[attno] = false;
            } else {
                int cmp = DatumGetInt32(FunctionCall2Coll(
                    proc[attno], collation[attno], lower[attno], value));
                if (cmp < 0) {
                    lower[attno] = value;
                    lower_inclusive[attno] = inclusive;
                } else if (cmp == 0) {
                    lower_inclusive[attno] &= inclusive;
                }
            }
        };
        const auto set_upper = [&](int attno, Datum value, bool inclusive) {
            if (upper_nulls[attno]) {
                upper[attno] = value;
                upper_inclusive[attno] = inclusive;
                upper_nulls[attno] = false;
            } else {
                int cmp = DatumGetInt32(FunctionCall2Coll(
                    proc[attno], collation[attno], upper[attno], value));
                if (cmp > 0) {
                    upper[attno] = value;
                    upper_inclusive[attno] = inclusive;
                } else if (cmp == 0) {
                    upper_inclusive[attno] &= inclusive;
                }
            }
        };

        Datum value = key->sk_argument;
        if (key->sk_subtype != rel->rd_opcintype[attno] && OidIsValid(key->sk_subtype)) {
            HeapTuple tuple = SearchSysCache2(CASTSOURCETARGET,
                ObjectIdGetDatum(key->sk_subtype), ObjectIdGetDatum(rel->rd_opcintype[attno]));
            if (!HeapTupleIsValid(tuple)) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Cache lookup failed for cast from %u to %u",
                                       key->sk_subtype, rel->rd_opcintype[attno])));
            }
            bool is_null;
            Datum value_cast = SysCacheGetAttr(CASTSOURCETARGET, tuple, Anum_pg_cast_castfunc, &is_null);
            Assert(!is_null);
            Oid funcoid = DatumGetObjectId(value_cast);
            ReleaseSysCache(tuple);
            if (OidIsValid(funcoid)) {
                FmgrInfo *cast_proc = (FmgrInfo *)palloc(sizeof(FmgrInfo));
                fmgr_info(funcoid, cast_proc);
                value = FunctionCall1Coll(cast_proc, key->sk_collation, value);
                pfree(cast_proc);
            } /* no need to cast datum if funcoid is not specified, they are basically alias types */
        }
        switch (key->sk_strategy) {
            case BTLessStrategyNumber:
            case BTLessEqualStrategyNumber:
                set_upper(attno, value, key->sk_strategy == BTLessEqualStrategyNumber);
                break;
            case BTEqualStrategyNumber:
                set_lower(attno, value, true);
                set_upper(attno, value, true);
                break;
            case BTGreaterEqualStrategyNumber:
            case BTGreaterStrategyNumber:
                set_lower(attno, value, key->sk_strategy == BTGreaterEqualStrategyNumber);
                break;
            default:
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Invalid comparison strategy number: %d", key->sk_strategy)));
        }
    }
    for (int i = 1; i < nattrs; ++i) {
        if (lower_nulls[i] || upper_nulls[i]) {
            continue;
        }
        int cmp = DatumGetInt32(FunctionCall2Coll(
            proc[i], collation[i], lower[i], upper[i]));
        equals[i] = cmp == 0;
    }
    auto *res = NEW BTTupleScanKey(so->tupleDesc, lower, upper, lower_inclusive, upper_inclusive,
                                   lower_nulls, upper_nulls, equals, collation, proc);
    MemoryContextSwitchTo(oldcxt);
    return res;
}

static void btree_find_node(IndexScanDesc scan)
{
    auto *so = (BPTreeScanOpaque *)scan->opaque;
    OffsetNumber offset = InvalidOffsetNumber;
    DiskBPlusTree btree(so->index, scan->heapRelation, so->btreeMetaBlkNo, false);
    if (so->tupleDesc == NULL) {
        so->tupleDesc = CreateTupleDescCopy(btree.get_tupDesc());
    }
    so->scankey = btree_scan_key(scan);
    DiskNodeImpl node = btree.search(*so->scankey, offset);
    btree.destroy();

    so->node = node;
    so->offset = offset;
}

static int find_extreme_value_idx(bool is_max, Oid opfamily, Oid elemtype, Oid collation,
    Datum *values, bool *is_null, int num_values)
{
    StrategyNumber strat = is_max ? BTGreaterStrategyNumber : BTLessStrategyNumber;
    Oid cmp_op = get_opfamily_member(opfamily, elemtype, elemtype, strat);
    if (!OidIsValid(cmp_op)) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
            errmsg("missing operator %d(%u,%u) in opfamily %u",
                   strat, elemtype, elemtype, opfamily)));
    }
    RegProcedure cmp_proc = get_opcode(cmp_op);
    if (!RegProcedureIsValid(cmp_proc)) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("missing oprcode for operator %u", cmp_op)));
    }
    FmgrInfo flinfo;
    fmgr_info(cmp_proc, &flinfo);
    int idx = -1;
    for (int i = 0; i < num_values; ++i) {
        if (is_null[i]) {
            continue;
        }
        if (idx == -1) {
            idx = i;
            continue;
        }
        Datum cmp_res = FunctionCall2Coll(&flinfo, collation, values[i], values[idx]);
        if (DatumGetBool(cmp_res)) {
            idx = i;
        }
    }
    return idx;
}

struct SortArrayContext {
    FmgrInfo flinfo;
    Oid collation;
    bool reverse;
};

static int _bt_compare_array_elements(const void *a, const void *b, void *arg)
{
    Datum da = *((const Datum *)a);
    Datum db = *((const Datum *)b);
    SortArrayContext *cxt = (SortArrayContext *)arg;
    int32 compare = DatumGetInt32(FunctionCall2Coll(&cxt->flinfo, cxt->collation, da, db));
    return cxt->reverse ? -compare : compare;
}

static uint32 sort_array_elements(Oid opfamily, Oid elemtype, Oid collation,
    bool reverse, Datum *elems, uint32 nelems)
{
    if (nelems <= 1) {
        return nelems; /* no work to do */
    }

    RegProcedure cmp_proc = get_opfamily_proc(opfamily, elemtype, elemtype, HYBRID_BTORDER_PROC);
    if (!RegProcedureIsValid(cmp_proc)) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("missing support function %d(%u,%u) in opfamily %u",
                               HYBRID_BTORDER_PROC, elemtype, elemtype, opfamily)));
    }

    FmgrInfo flinfo;
    fmgr_info(cmp_proc, &flinfo);
    SortArrayContext cxt = {flinfo, collation, reverse};
    qsort_arg((void *)elems, nelems, sizeof(Datum), _bt_compare_array_elements, (void *)&cxt);

    uint32 last_non_dup = 0;
    for (uint32 i = 1u; i < nelems; ++i) {
        int compare = DatumGetInt32(FunctionCall2Coll(&flinfo, collation, elems[last_non_dup], elems[i]));
        if (compare != 0) {
            elems[++last_non_dup] = elems[i];
        }
    }
    return last_non_dup + 1u;
}

static bool preprocess_scankey(IndexScanDesc scan, BPTreeScanOpaque *btso)
{
    if (btso->scan_cxt == NULL) {
        btso->scan_cxt = AllocSetContextCreate(CurrentMemoryContext, "HybridannScanContext",
            ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);
    } else {
        MemoryContextReset(btso->scan_cxt);
    }

    MemoryContext oldcxt = MemoryContextSwitchTo(btso->scan_cxt);
    btso->nkeys = scan->numberOfKeys;
    btso->narrkeys = 0;
    btso->keyData = btso->nkeys == 0 ?
        NULL :
        (ScanKey)palloc(uint32(scan->numberOfKeys) * sizeof(ScanKeyData));
    for (int i = 0; i < scan->numberOfKeys; ++i) {
        ScanKey key = &scan->keyData[i];
        Datum value = key->sk_argument;
        if (key->sk_flags & SK_SEARCHARRAY) {
            Relation rel = scan->indexRelation;
            Oid entry_type = OidIsValid(key->sk_subtype) ?
                key->sk_subtype :
                rel->rd_opcintype[key->sk_attno - 1];
            Oid opfamily = rel->rd_opfamily[key->sk_attno - 1];
            ArrayType *arrayval = DatumGetArrayTypeP(value);
            int16 elmlen;
            bool elmbyval;
            char elmalign;
            get_typlenbyvalalign(ARR_ELEMTYPE(arrayval), &elmlen, &elmbyval, &elmalign);
            Datum *elem_values;
            bool *elem_nulls;
            int num_elems;
            deconstruct_array(arrayval, ARR_ELEMTYPE(arrayval), elmlen, elmbyval, elmalign,
                &elem_values, &elem_nulls, &num_elems);
            if (key->sk_strategy == BTEqualStrategyNumber) {
                if (btso->narrkeys == 0) {
                    btso->arrkeys = (ArrayKey *)palloc(uint32(scan->numberOfKeys) * sizeof(ArrayKey));
                }
                ArrayKey &arrkey = btso->arrkeys[btso->narrkeys];
                arrkey.nitems = 0;
                arrkey.cur_idx = 0;
                arrkey.key_idx = i;
                for (int j = 0; j < num_elems; ++j) {
                    if (elem_nulls[j]) {
                        continue;
                    }
                    elem_values[arrkey.nitems] = elem_values[j];
                    ++arrkey.nitems;
                }
                arrkey.nitems = sort_array_elements(opfamily, entry_type, key->sk_collation, false,
                    elem_values, arrkey.nitems);
                if (arrkey.nitems == 0) {
                    return false;
                }
                arrkey.items = (Datum *)repalloc(elem_values, uint32(arrkey.nitems) * sizeof(Datum));
                ++btso->narrkeys;
                value = arrkey.items[0];
            } else {
                int pos = find_extreme_value_idx(
                    key->sk_strategy != BTGreaterStrategyNumber && key->sk_strategy != BTGreaterEqualStrategyNumber,
                    opfamily, entry_type, key->sk_collation, elem_values, elem_nulls, num_elems);
                if (pos < 0) {
                    return false;
                }
                value = elem_values[pos];
                pfree_ext(elem_values);
            }
            pfree_ext(elem_nulls);
        }
        ScanKey newkey = &btso->keyData[i];
        ScanKeyEntryInitialize(newkey, key->sk_flags, key->sk_attno, key->sk_strategy, key->sk_subtype,
            key->sk_collation, key->sk_func.fn_oid, value);
    }
    if (btso->arrkeys && btso->narrkeys != uint32(scan->numberOfKeys)) {
        btso->arrkeys = (ArrayKey *)repalloc(btso->arrkeys, btso->narrkeys * sizeof(ArrayKey));
    }
    MemoryContextSwitchTo(oldcxt);
    return true;
}

static void btree_init_search(IndexScanDesc scan, BPTreeScanOpaque *btso)
{
    constexpr size_t MAX_TIDS_PER_DISK_NODE =
        (BLCKSZ - sizeof(DiskBTOpaqueData) - SizeOfPageHeaderData) / sizeof(ItemPointerData);
    btso->tids = (ItemPointerData *)palloc(MAX_TIDS_PER_DISK_NODE * sizeof(ItemPointerData));
    for (int i = 0; i < btso->nkeys; ++i) {
        Assert(btso->keyData[i].sk_attno >= 2);
        if (btso->keyData[i].sk_flags & SK_ISNULL) {
            if (btso->keyData[i].sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("Hybridann index currently does not support NULL clause")));
            }
            btso->no_more_data = true;
            btso->lastIndex = -1;
            btso->currIndex = 0;
            return;
        }
    }
    btree_find_node(scan);
    btree_read_node(btso);
}

static bool btree_next_search(IndexScanDesc scan, BPTreeScanOpaque *so)
{
    if (so->narrkeys == 0) {
        return false;
    }
    Assert(so->arrkeys);
    for (ArrayKey *cur = so->arrkeys + so->narrkeys - 1u; cur >= so->arrkeys; --cur) {
        ++cur->cur_idx;
        if (cur->cur_idx < cur->nitems) {
            so->keyData[cur->key_idx].sk_argument = cur->items[cur->cur_idx];
            return true;
        }
        cur->cur_idx = 0;
        so->keyData[cur->key_idx].sk_argument = cur->items[0];
    }
    return false;
}

static void merge_vector_idx(Vector<DiskBPlusTree::ScanResult> &&src,
                             Vector<DiskBPlusTree::ScanResult> &dst)
{
    if (dst.empty()) {
        dst = std::move(src);
        return;
    }
    for (const auto &idx : src) {
        auto it = std::find_if(dst.cbegin(), dst.cend(), [&](const DiskBPlusTree::ScanResult &x) {
            return x.idx_ptr == idx.idx_ptr;
        });
        if (it == dst.cend()) {
            dst.push_back(idx);
        }
    }
    optional_destroy(src);
}

struct DistTid : SAFE_CONSTRUCTOR {
    float dist;
    ItemPointerData tid;
    DistTid(float d, ItemPointerData t) : dist(d), tid(t) {}
    static bool larger_tid(ItemPointerData a, ItemPointerData b)
    {
        BlockNumber a_blk = ((BlockNumber)a.ip_blkid.bi_hi << 16) | a.ip_blkid.bi_lo;
        BlockNumber b_blk = ((BlockNumber)b.ip_blkid.bi_hi << 16) | b.ip_blkid.bi_lo;
        if (a_blk == b_blk) {
            return a.ip_posid > b.ip_posid;
        }
        return a_blk > b_blk;
    }
    bool operator<(const DistTid &rhs) const
    {
        return dist < rhs.dist ||
            /* prefer newer data (with larger blkno) */
            (dist == rhs.dist && larger_tid(tid, rhs.tid));
    }
};

static uint32 get_unique_idx(const Vector<DistTid, CONTEXT_ALLOCATOR<DistTid>> &dist_tid, uint32 from, uint32 until,
                             Vector<ItemPointerData> &tids)
{
    ItemPointerData cur_tid;
    ItemPointerSetInvalid(&cur_tid);
    uint32 res = from;
    for (auto it = dist_tid.at(from); it != dist_tid.cend(); ++it, ++res) {
        if (cur_tid.ip_posid == it->tid.ip_posid &&
            ItemPointerGetBlockNumberNoCheck(&cur_tid) == ItemPointerGetBlockNumberNoCheck(&it->tid)) {
            continue;
        }
        if (res >= until) {
            break;
        }
        cur_tid = it->tid;
        tids.push_back(it->tid);
    }
    return res;
}

static void btree_vector_search(IndexScanDesc scan, BPTreeScanOpaque *so)
{
    if (scan->numberOfOrderBys != 1 || scan->orderByData->sk_attno != 1) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Hybridann index only support vector order")));
    }
    Datum value = scan->orderByData->sk_argument;
    FloatVector *vec = DatumGetFloatVector(value);
    if (uint16(vec->dim) != so->dimensions) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Incorrect dimension of query vector")));
    }
    DiskBPlusTree btree(so->index, scan->heapRelation, so->btreeMetaBlkNo, false);
    if (so->tupleDesc == NULL) {
        so->tupleDesc = CreateTupleDescCopy(btree.get_tupDesc());
    }
    Vector<BTTupleScanKey> scan_keys;
    do {
        BTTupleScanKey *key = btree_scan_key(scan);
        if (key) {
            scan_keys.push_back(std::move(*key));
            delete key;
        }
    } while (btree_next_search(scan, so));
    if (scan_keys.empty()) {
        optional_destroy(scan_keys);
        btree.destroy();
        so->lastIndex = -1;
        so->currIndex = 0;
        return;
    }
    if (scan->with_limit && so->searchListSize < (uint32)scan->limit_count) {
        so->searchListSize = (uint32)scan->limit_count;
    }
    Metric metric = get_func_metric(scan->orderByData->sk_func.fn_oid);
    Vector<DiskBPlusTree::ScanResult> idxs;

    /* should use SHARED_CONTEXT, because if parallel scan is enabled,
     * when backend worker execute dist_tids.push_back() alloc memory,
     * if use STANDARD_CONTEXT will cause the memory usage statistic on backend worker and
     * will remain as long as this worker alive, althouth the memory is pfreed finally.
     * this will cause the database faild to alloc new memrory */
    auto ctx = AllocSetContextCreate(IndexerCtx, "hybridann parallel query context",
        ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE, SHARED_CONTEXT);
    Vector<DistTid, CONTEXT_ALLOCATOR<DistTid>> dist_tids(ctx);

    float *query = vec->x;
    bool alloced = false;
    if (!is_aligned(query)) {
        size_t dim = so->dimensions;
        float *temp = alloc_floatvector(dim);
        errno_t rc = memcpy_s(temp, sizeof(float) * dim, query, sizeof(float) * dim);
        securec_check(rc, "\0", "\0");
        query = temp;
        alloced = true;
    }

    Oid heap_id;
    Oid heap_part_id;
    Oid index_id;
	Oid index_part_id;
    if (RelationIsPartition(so->index)) {
		Assert(RelationIsPartition(so->index));
		heap_id = GetBaseRelOidOfParition(scan->heapRelation);
		heap_part_id = RelationGetRelid(scan->heapRelation);
		index_id = GetBaseRelOidOfParition(so->index);
		index_part_id = RelationGetRelid(so->index);
	} else {
		Assert(!RelationIsPartition(so->index));
		heap_id = RelationGetRelid(scan->heapRelation);
		heap_part_id = InvalidOid;
		index_id = RelationGetRelid(so->index);
		index_part_id = InvalidOid;
	}

    constexpr int total_iter_steps = 3;
    const float cover_thresholds[total_iter_steps] = {0.5, 0.8, 1.0};
    int cur_iter_step = 0;
    bool need_retry = false;
    const auto func = get_aligned_distance_func(metric, uint32(vec->dim));

    slock_t lock;
    SpinLockInit(&lock);
    PerfUsage perf;

retry:
    bool do_parallel = false;
    uint32 nleaves = 0;
    size_t leaf_size = 0;
    for (const auto &k : scan_keys) {
        merge_vector_idx(btree.search_coverage(k, cover_thresholds[cur_iter_step]), idxs);
    }
    const auto search_node = [&](const DiskBPlusTree::ScanResult &idx) {
        SpinLockAcquire(&lock);
        if (need_retry) {
             SpinLockRelease(&lock);
             return;          
        }
        SpinLockRelease(&lock);

        Relation index_base = NULL;
        Partition index_part = NULL;
        Relation localindex;
        if (t_thrd.role == VECINDEX_WORKER) {
            if (index_part_id == InvalidOid) {
                localindex = index_open(index_id, NoLock);
            } else {
                index_base = index_open(index_id, NoLock);
                index_part = partitionOpen(index_base, index_part_id, NoLock);
                localindex = partitionGetRelation(index_base, index_part);
            }
        } else {
            localindex = so->index;
        }

        Vector<DistTid> localidxs;
        if (idx.level == btree.leaf_level) {
            TASKPOOLLOG("DEBUG: run task leaf");
            ++nleaves;
            struct OffsetTid { size_t offset; ItemPointerData tid; };
            Vector<OffsetTid> offset_tids;
            DiskNodeImpl node(localindex, idx.idx_ptr);
            node.r_lock();
            const OffsetNumber n = node.size();

            /* extract valid offset number */
            BitVector<> offset_search_idx(n + 1u);
            for (const auto &k : scan_keys) {
                const OffsetNumber start = std::max(p_firstdatakey(node), node.item_index_of(k));
                const OffsetNumber end = std::min(n, node.right_item_index_of(k));
                if (k.single_range()) {
                    for (OffsetNumber i = start; i <= end; i = OffsetNumberNext(i)) {
                        offset_search_idx.set(i, true);
                    }
                } else {
                    bool forward = true;
                    for (OffsetNumber i = start; i < end && forward; i = OffsetNumberNext(i)) {
                        if (k(*node.get_data(i), forward)) {
                            offset_search_idx.set(i, true);
                        }
                    }
                }
            }

            /* extract offset and tid given offsets */
            for (OffsetNumber i = p_firstdatakey(node); i <= n; ++i) {
                if (!offset_search_idx[i]) {
                    continue;
                }
                const auto *data = node.get_data(i);
                bool is_null;
                Datum vid = index_getattr(data->tuple(), 1, btree.get_tupDesc(), &is_null);
                Assert(!is_null);
                offset_tids.push_back({(size_t)DatumGetInt64(vid), data->t_tid});
            }
            optional_destroy(offset_search_idx);

            /* calculate their distances */
            leaf_size += offset_tids.size();
            PerfVecCmp(perf, offset_tids.size());

            if (do_parallel) {
                localidxs.reserve(offset_tids.size());
            }
            for (const auto &offset_tid : offset_tids) {
                VecBuffer buf = vec_read_buffer(localindex, offset_tid.offset, uint32(vec->dim) * sizeof(float));
                float dist = func(query, (float *)buf.get_vecbuf(), uint32(vec->dim));
                buf.release();

                if (do_parallel) {
                    localidxs.emplace_back(dist, offset_tid.tid);
                } else {
                    dist_tids.emplace_back(dist, offset_tid.tid);
                }
            }
            PerfStop(perf);

            node.r_unlock();
            node.destroy();
            optional_destroy(offset_tids);
        } else {
            TASKPOOLLOG("DEBUG: run task Vector Index");
            VectorIndex *index = VectorIndexFactory::create(localindex, scan->heapRelation, idx.idx_ptr);
            BTVEC_SEARCH_LOG("Searching level-%u %s index %u", idx.level, index->type_name(), idx.idx_ptr);
            uint32 topk = so->searchListSize;
            float effect_cover = std::max(cover_thresholds[cur_iter_step], idx.cover_ratio);
            if (scan->with_limit && topk < (uint32)std::ceil(scan->limit_count / effect_cover)) {
                topk = (uint32)std::ceil(scan->limit_count / effect_cover);
            }
            float *dists = alloc_floatvector(1ul, topk);
            ItemPointer tids = (ItemPointer)palloc(topk * sizeof(ItemPointerData));
            /* we don't lock index for search */
            VectorIndexNoneSearchParam param =
                {query, btree.get_tupDesc(), topk, uint32(vec->dim)};
            if (index->type() == VectorIndexType::GRAPH) {
                PerfPrune(perf);
            } else {
                PerfIvf(perf);
            }

            IndexScanDesc localscan = NULL;
            Relation heapRel = NULL;
            Relation    targetheap = NULL;
            Partition   heappart = NULL;
            if (t_thrd.role == VECINDEX_WORKER) {
                heapRel = heap_open(heap_id, NoLock);
                if (OidIsValid(heap_part_id)) {
                    heappart = partitionOpen(heapRel, heap_part_id,  NoLock);
                    targetheap = partitionGetRelation(heapRel, heappart);
                } else {
                    targetheap = heapRel;
                }
                localscan = RelationGetIndexScan(localindex, scan->numberOfKeys, scan->numberOfOrderBys);
                localscan->heapRelation = targetheap;
                errno_t rc;
                if (scan->numberOfKeys) {
                    rc = memcpy_s(localscan->keyData, sizeof(ScanKeyData), scan->keyData, sizeof(ScanKeyData));
                    securec_check_c(rc, "\0", "\0");
                }
                if (scan->numberOfOrderBys) {
                    rc = memcpy_s(localscan->orderByData, sizeof(ScanKeyData), scan->orderByData, sizeof(ScanKeyData));
                    securec_check_c(rc, "\0", "\0");
                }
                localscan->xs_snapshot = u_sess->utils_cxt.CurrentSnapshot;
                localscan->with_limit = scan->with_limit;
                localscan->limit_offset = scan->limit_offset;
                localscan->limit_count = scan->limit_count;
            } else {
                localscan = scan;
            }

            EState *estate = CreateExecutorState();
            ExprContext *econtext = GetPerTupleExprContext(estate);
            TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(localscan->heapRelation));
            /* Arrange for econtext's scan tuple to be the tuple under test */
            econtext->ecxt_scantuple = slot;
            IndexInfo *indexInfo = BuildIndexInfo(localindex);
            int nkeys = RelationGetDescr(localindex)->natts;
            Datum		values[nkeys];
            bool		isnull[nkeys];
            size_t filter_count = 0;

index_research:
            size_t nvec = index->search(localscan, dists, tids, &param);
            size_t filter_total = localscan->with_limit ? localscan->limit_count : nvec;
            size_t i = 0;
            while (i < nvec && filter_count < filter_total) {
                localscan->xs_ctup.t_self = tids[i];
                HeapTuple htuple = (HeapTuple)IndexFetchTuple(localscan);
                if (htuple != NULL) {
                    (void)ExecStoreTuple(htuple, slot, InvalidBuffer, false);
                    FormIndexDatum(indexInfo, slot, estate, values, isnull);
                    values[0] = 0;
                    BTTupleData * itup = (BTTupleData *)index_form_tuple(btree.get_tupDesc(), values, isnull);
                    for (const auto &k : scan_keys) {
                        if (k(*itup)) {
                            if (do_parallel) {
                                localidxs.emplace_back(dists[i], tids[i]);
                            } else {
                                dist_tids.emplace_back(dists[i], tids[i]);
                            }
                            ++filter_count;
                            break;
                        }  
                    }
                    pfree(itup);
                }
                ++i;
            }

            if (localscan->with_limit && filter_count < (size_t)localscan->limit_count &&
                idx.cover_ratio == 1 && nvec > 0) {
                goto index_research;
            }

            ExecDropSingleTupleTableSlot(slot);
            FreeExecutorState(estate);

            PerfStop(perf);
           
            if (localscan->with_limit && filter_count < (size_t)localscan->limit_count 
                && idx.cover_ratio < 1) {
                SpinLockAcquire(&lock);
                need_retry = true;
                SpinLockRelease(&lock);
            }
          
            index->destroy();
            delete index;

            if (t_thrd.role == VECINDEX_WORKER) {
                if (BufferIsValid(localscan->xs_cbuf)) {
                    ReleaseBuffer(localscan->xs_cbuf);
                }

                if (OidIsValid(heap_part_id)) {
                    releaseDummyRelation(&targetheap);
                    partitionClose(heapRel, heappart, NoLock);
                }
                heap_close(heapRel, NoLock);

                IndexScanEnd(localscan);
            }

            free_vector(dists);
            pfree(tids);
        }

        if (do_parallel) {
            SpinLockAcquire(&lock);
            if (!need_retry || (need_retry && cur_iter_step + 1 == total_iter_steps)) {
                dist_tids.push_back(localidxs.cbegin(), localidxs.cend());
            }
            SpinLockRelease(&lock);
        }
        optional_destroy(localidxs);
        if (t_thrd.role == VECINDEX_WORKER) {
            if (index_part_id == InvalidOid) {
                index_close(localindex, NoLock);
            } else {
                releaseDummyRelation(&localindex);
                partitionClose(index_base, index_part, NoLock);
                index_close(index_base, NoLock);
            }
        }
    };
    const bool isglobaltemp = so->index->rd_backend != InvalidBackendId;
    if (!isglobaltemp && USE_PARALLEL_QUERY && idxs.size() > 1) {
        do_parallel = true;
        INIT_TASK_RUNNER();
        LOAD_CONSUMER();
        START_TASK_POOL_NTASK(idxs.size());

        for (const auto &idx : idxs) {
            RUN_TASK(search_node, idx);
        }
        RESIGN_PRODUCER();
        TASK_RUNNER->pure_consume();
        WAIT_AND_END_TASK_POOL();
        DESTROY_TASK_RUNNER();
    } else {
        for (const auto &idx : idxs) {
            search_node(idx);
        }
    }

    SpinLockFree(&lock);

    BTVEC_SEARCH_LOG("Searched %u leaves, %lu vectors in total", nleaves, leaf_size);
    PerfReport(perf);

    cur_iter_step++;
    if (need_retry && cur_iter_step < total_iter_steps) {
        need_retry = false;
        idxs.clear();
        dist_tids.clear();
        goto retry;
    }


    optional_destroy(perf);
    if (alloced) {
        free_vector(query);
    }

    optional_destroy(idxs);
    optional_destroy(scan_keys);
    btree.destroy();
    if ((Pointer)vec != DatumGetPointer(value)) {
        pfree(vec);
    }
    uint32 top_k = std::min(uint32(so->searchListSize) * 2u, (uint32)dist_tids.size());
    uint32 unique_k = std::min(uint32(so->searchListSize), top_k);
    uint32 cur_k = 0;
    Vector<ItemPointerData> unique_tids(unique_k);
    const uint32 s = dist_tids.size();
    while (unique_tids.size() < unique_k && cur_k < s) {
        const uint32 idx = std::min(cur_k + top_k, s);
        std::partial_sort(dist_tids.at(cur_k), dist_tids.at(idx), dist_tids.end());
        cur_k = get_unique_idx(dist_tids, cur_k, idx, unique_tids);
    }
    MemoryContextDelete(ctx);
    so->tids = unique_tids.data();
    so->currIndex = 0;
    so->lastIndex = int(unique_tids.size()) - 1;
}

static bool btree_search(IndexScanDesc scan, ScanDirection dir)
{
    BPTreeScanOpaque *so = (BPTreeScanOpaque *)scan->opaque;

     /* Postgres doesn't support backward scan on operators */
    Assert(ScanDirectionIsForward(dir));

    if (so->first) {
        pgstat_count_index_scan(so->index);

        if (!preprocess_scankey(scan, so)) {
            return false;
        }

        if (scan->numberOfOrderBys > 0) {
            btree_vector_search(scan, so);
            so->can_iterate = false;
            so->require_recheck = false;
        } else {
            btree_init_search(scan, so);
            so->can_iterate = true;
            so->require_recheck = false;
        }
        so->first = false;
    }

retry:
    if (so->currIndex > so->lastIndex) {
        if (!so->can_iterate) {
            return false;
        }
        BlockNumber nextblkno = InvalidBlockNumber;
        for (;;) {
            nextblkno = so->no_more_data ? InvalidBlockNumber : so->nextBlkno;
            if (!BlockNumberIsValid(nextblkno)) {
                if (!btree_next_search(scan, so)) {
                    return false;
                }
                btree_find_node(scan);
                btree_read_node(so);
                goto retry;
                
            }
            btree_next_node(so, nextblkno);
            btree_read_node(so);
            if (so->currIndex <= so->lastIndex) {
                break;
            }
        }
    }

    scan->xs_recheck = so->require_recheck;
    scan->xs_ctup.t_self = so->tids[so->currIndex++];
    return true;
}


bool hybridanngettuple_internal(IndexScanDesc scan, ScanDirection dir)
{
    return btree_search(scan, dir);
}

static size_t estimate_num_vec_access(double num_tuples, uint32 search_list_size)
{
    if (num_tuples <= 1000) {
        return Min(num_tuples, search_list_size * 3);
    }
    size_t res;
    if (num_tuples < 10'000) {
        res = 500 + (num_tuples - 1000) / 18;
    } else if (num_tuples < 100'000) {
        res = 1000 + (num_tuples - 10000) / 45;
    } else if (num_tuples < 1'000'000) {
        res =  3000 + (num_tuples - 100'000) / 450;
    } else if (num_tuples < 10'000'000) {
        res = 5000 + (num_tuples - 1'000'000) / 3000;
    } else {
        res = 8000 + (num_tuples - 10'000'000) / 10000;
    }
    return res * log2(search_list_size / 1000.0 + 1);
}

static size_t estimate_vec_page_access(double num_tuples, uint32 search_list_size, uint32 vec_dim)
{
    const size_t nvec = estimate_num_vec_access(num_tuples, search_list_size);
    const double vec_rate = double(nvec) / num_tuples;
    constexpr double base_denoise_rate = 0.06;
    const double rate = Min(1, base_denoise_rate / vec_rate) + double(vec_dim) /
        DiskVector<float>::n_data_per_block();
    return nvec * rate;
}

static size_t estimate_tuple_page_access(double num_tuples, uint32 search_list_size, bool has_indexquals)
{
    /* assume only 5% explored vector are actually get extanded */
    const size_t ntup = estimate_num_vec_access(num_tuples, search_list_size) / 20;
    const size_t num_node_page = num_tuples /
        DiskVector<DiskAnnVamanaNode>::n_data_per_block() + 1ul;
    size_t node_access = has_indexquals ? ntup / 5 : search_list_size / 2;
    return ntup / 1.025 + Min(num_node_page, node_access); /* neighbor page + node pages */
}

static size_t estimate_meta_page_access(bool has_indexquals)
{
    return 4ul + has_indexquals ? 1 : 0; /* index, data, graph, node, and attr */
}

static void hybrid_vector_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
    uint32 vec_dim, float4 opr_cost, Cost *indexStartupCost, Cost *indexTotalCost,
    Selectivity *indexSelectivity, double *indexCorrelation, IndexMagnitude *index_magnitude)
{
    Assert(index_magnitude->size() > 0);
    *indexStartupCost = 0;
    double index_costs[index_magnitude->size()];
    for (auto &cost : index_costs) {
        cost = 0;   /* init it to zero, calculate it only if needed */
    }
    uint32 search_list_size = size_t(u_sess->attr.attr_storage.ef_search);
    if (search_list_size < (uint32)root->limit_tuples + 1) {
        search_list_size = (uint32)root->limit_tuples + 1;
    }
    double seqcost, randomcost;
    
    get_tablespace_page_costs(path->indexinfo->reltablespace, &randomcost, &seqcost);
    const auto index_cost = [&](size_t level) -> double {
        if (index_costs[level] > 0) {
            return index_costs[level];
        }
        const size_t num_tuples = index_magnitude->get(level);
        size_t page_fetched = estimate_meta_page_access(false) + 1ul +
            estimate_tuple_page_access(num_tuples, search_list_size, false) +
            estimate_vec_page_access(num_tuples, search_list_size, vec_dim);
        page_fetched *= loop_count;
        page_fetched = index_pages_fetched(page_fetched, path->indexinfo->pages,
            (double)path->indexinfo->pages, root, path->indexinfo->rel->isPartitionedTable);
        
        double cur_randomcost = randomcost;
        if (cur_randomcost > seqcost) {
            constexpr size_t small_page_size = 15'000ul;
            constexpr double scale_factor = 0.0002;
            cur_randomcost = LOGISTIC_FUNC(page_fetched, small_page_size, randomcost, seqcost, scale_factor);
        }
        const Cost calc_cost =
            (u_sess->attr.attr_sql.cpu_operator_cost * opr_cost + u_sess->attr.attr_sql.cpu_tuple_cost) *
            estimate_num_vec_access(num_tuples, search_list_size);
        double res = page_fetched * cur_randomcost + calc_cost + 0.0001;
        index_costs[level] = res;
        return res;
    };

    /* currently we treat everything as single range for efficiency */
    double selec = 1.0;
    if (list_length(path->indexquals) > 0) {
        List *quals = add_predicate_to_quals(path->indexinfo, path->indexquals, root);
        selec = clauselist_selectivity(root, quals, path->indexinfo->rel->relid, JOIN_INNER, NULL);
    }
    const double num_tuples = RELOPTINFO_LOCAL_FIELD(root, path->indexinfo->rel, tuples);
    *indexSelectivity = path->indexinfo->ispartitionedindex ?
        std::min(selec, search_list_size / num_tuples) :
        selec;
    size_t range_length = num_tuples * selec;
    uint16 cur_level = index_magnitude->size() - 1u;
    Vector<size_t> &index_magnitudes = index_magnitude->magnitudes();
    for (auto it = index_magnitudes.crbegin(); it != index_magnitudes.crend(); --it, --cur_level) {
        while (range_length >= *it) {
            *indexStartupCost += index_cost(index_magnitudes.offset(it));
            range_length -= *it;
        }
    }
    opr_cost *= u_sess->attr.attr_sql.cpu_operator_cost;
    *indexStartupCost += range_length * opr_cost * loop_count;
    *indexTotalCost = *indexStartupCost + search_list_size * (opr_cost + randomcost);
}

/*
 * Estimate the cost of an index scan
 */
void hybridanncostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
                                  Cost *indexStartupCost, Cost *indexTotalCost,
                                  Selectivity *indexSelectivity, double *indexCorrelation)
{
    *indexStartupCost = DBL_MAX;
    *indexTotalCost = DBL_MAX;
    *indexSelectivity = 0;
    *indexCorrelation = 0;
    /* Never use index without order and limit */
    bool has_limit = root->limit_tuples > 0;
    if (!has_limit) {
        PlannerInfo *cur = root;
        while (cur && cur->parse && !has_limit) {
            has_limit = cur->parse->limitCount;
            cur = cur->parent_root;
        }
    }

    Relation indexRel = index_open(path->indexinfo->indexoid, AccessShareLock);
    Relation partRel = NULL;
    if (path->indexinfo->ispartitionedindex) {
        List *partitionIdList = indexGetPartitionOidList(indexRel);
        ListCell *cell = list_head(partitionIdList);
        if (cell) {
            Oid partitionOid = lfirst_oid(cell);
            Partition indexpart = partitionOpen(indexRel, partitionOid, AccessShareLock);
            partRel = partitionGetRelation(indexRel, indexpart);
            partitionClose(indexRel, indexpart, AccessShareLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
        } else {
            index_close(indexRel, AccessShareLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
            elog(ERROR, "Hybridann costestimate get partition oid faild by index oid(%d)",
                        path->indexinfo->indexoid);
        }
	}

    Buffer buf = AnnLoadBuffer(partRel ? partRel : indexRel, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = (HybridAnnMetaPage *)HybridAnnPageGetMeta(BufferGetPage(buf));

    if (unlikely(meta->magicNumber == DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please REINDEX.")));
    }

    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybridann index is not valid")));
    }
    IndexMagnitude *index_magnitude = NEW IndexMagnitude(meta->indexMagnitudes, meta->sizeIndexMagnitudes, meta->graphMagnitudeThreshold);
    UnlockReleaseBuffer(buf);
    

	if (partRel) {
		releaseDummyRelation(&partRel);
	}
    
	index_close(indexRel, AccessShareLock);

    if (list_length(path->indexorderbys) <= 0) {
        btcostestimate_internal(root, path, loop_count,
            indexStartupCost, indexTotalCost, indexSelectivity, indexCorrelation);
        constexpr double hybrid_cost_factor = 1.2;
        *indexStartupCost *= hybrid_cost_factor;
        *indexTotalCost *= hybrid_cost_factor;
        return;
    }

    if (!has_limit && u_sess->attr.attr_sql.enable_seqscan) {
        return;
    }
    uint32 vec_dim = 768u;
    /* all vector operation cost is set to 5 */
    float4 opr_cost = 5;
    Node *node = (Node *)lfirst(list_head(path->indexorderbys));
    if (IsA(node, OpExpr)) {
        OpExpr *op = (OpExpr *)node;
        /* the first order by must be vector ops */
        if (list_length(op->args) != 2) {
            return;
        }
        set_opfuncid(op);
        if (op->opfuncid != F_COSINE_DISTANCE &&
            op->opfuncid != F_L2_DISTANCE &&
            op->opfuncid != F_FLOATVECTOR_NEGATIVE_INNER_PRODUCT) {
            return;
        }
        opr_cost = get_func_cost(op->opfuncid);
        Node *arg1 = (Node *)lfirst(list_head(op->args));
        Node *arg2 = (Node *)lfirst(list_tail(op->args));
        Var *var = NULL;
        if (IsA(arg1, Var)) {
            var = (Var *)arg1;
        } else if (IsA(arg2, Var)) {
            var = (Var *)arg2;
        }
        if (var) {
            vec_dim = var->vartypmod > 0 ? var->vartypmod : vec_dim;
        }
    } /* we don't deal with the order by expr being no-opexpr case, just assume it's a vec order by */

    hybrid_vector_costestimate(root, path, loop_count, vec_dim, opr_cost,
        indexStartupCost, indexTotalCost, indexSelectivity, indexCorrelation, index_magnitude);
    index_magnitude->destroy();
    delete index_magnitude;
}
