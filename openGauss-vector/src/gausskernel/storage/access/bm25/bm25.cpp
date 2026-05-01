/**
 * Copyright ...
 * BM25 index access method definitions.
 */

#include <atomic>

#include <vtl/hashtable>
#include <vtl/btree>
#include <vtl/disk_container/disk_hashtable.hpp>

#include "access/bm25/bm25.h"
#include "access/bm25/bm25_utils.h"
#include "access/bm25/bm25_internal.h"
#include "access/bm25/bm25_scan.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "access/hash.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/reloptions.h"
#include "nodes/relation.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/module/timer.h"
#include "postmaster/bgworker.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_partition_fn.h"
#include "access/bm25/tokenizer/cppjieba/jieba.h"
#include "access/bm25/tokenizer/cppjieba/query_segment.h"
#include "access/bm25/tokenizer/cppjieba/full_segment.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "access/bm25/bm25_parse.h"
#include "access/annvector/ann_utils.h"

using namespace bm25;
using namespace disk_container;
using namespace bm25_tokenizer;
using namespace ann_helper;
using namespace cppjieba;

struct TestHasher {
    uint32 operator()(const uint32 &val) const noexcept
        { return DatumGetUInt32(hash_uint32(val)); }
};
#define TestAssert(expr)                                                            \
    do {                                                                            \
        if (!(expr))                                                                \
            elog(ERROR, "Assertion failed: %s (%s:%d)", #expr, __FILE__, __LINE__); \
    } while (0)
#define DO_HASHTABLE_TEST(index) // hashtable_test(index)

using test_table = DiskHashTable<uint32, uint32, TestHasher, std::equal_to<uint32>, 10ul>;
[[maybe_unused]] static void hashtable_test(Relation index)
{
    BlockNumber s = test_table::get_disk_hashtable(index, false);
    constexpr size_t N = 2'500'000ul;
    constexpr size_t aN = 1'000'000ul;
    constexpr size_t nparallel = 8ul;
    const auto insert_test = [index_oid = index->rd_id, s](uint32 i) {
        Relation rel = index_open(index_oid, NoLock);
        test_table ht(rel, s, false);
        size_t end = i * N + N;
        for (size_t j = i * N; j < end; ++j) {
            CHECK_FOR_INTERRUPTS();
            bool res = ht.insert(j, j);
            TestAssert(res);
        }
        ht.destroy();
        index_close(rel, NoLock);
    };
    const auto search_test = [index_oid = index->rd_id, s]() {
        Relation rel = index_open(index_oid, NoLock);
        test_table ht(rel, s, false);
        constexpr double hit_ratio = 1;
        for (size_t i = 0; i < aN; ++i) {
            CHECK_FOR_INTERRUPTS();
            uint32 q = random() % uint32((1.0 / hit_ratio) * N * nparallel);
            const auto check = [q](const test_table::kv_base &kv) { TestAssert(kv.v == q); };
            bool res = ht.cvisit(q, check);
            TestAssert(res == (q < N * nparallel));
        }
        ht.destroy();
        index_close(rel, NoLock);
    };
    INIT_TASK_RUNNER();
    if (nparallel > 1ul) {
        LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER(nparallel - 1ul);
    }
    pg_usleep(500'000l);
    START_TASK_POOL();
    Timer timer;
    for (size_t i = 0; i < nparallel; ++i) {
        RUN_TASK(insert_test, i);
    }
    WAIT_TASK();
    timer.report("DiskHashTable insert test passed.");
    for (size_t i = 0; i < nparallel; ++i) {
        RUN_TASK(search_test);
    }
    WAIT_AND_END_TASK_POOL();
    timer.report("DiskHashTable search test passed.");
    DESTROY_TASK_RUNNER();
    test_table tt(index, s, false);
    size_t odd_n = 0;
    size_t even_n = 0;
    size_t visited = tt.cvisit([&even_n, &odd_n](const auto &kv) {
        CHECK_FOR_INTERRUPTS();
        if (kv.v % 2 == 0) {
            ++even_n;
        } else {
            ++odd_n;
        }
    });
    TestAssert(visited == N * nparallel);
    TestAssert(odd_n + even_n == visited);
    TestAssert(odd_n >= visited / 2);
    TestAssert(even_n >= visited / 2);
    timer.report("Bulk cvisit test passed.");
    size_t nerased = tt.erase_cif([](const auto &kv) {
        CHECK_FOR_INTERRUPTS();
        return kv.v % 2 == 0;
    });
    TestAssert(even_n == nerased);
    timer.report("Bulk cerase done.");
    even_n = 0;
    visited = tt.cvisit([&even_n](const auto &kv) {
        CHECK_FOR_INTERRUPTS();
        if (kv.v % 2 == 0) {
            ++even_n;
        }
    });
    TestAssert(even_n == 0);
    TestAssert(visited == odd_n);
    timer.report("Bulk erase test passed.");
    visited = tt.visit([&even_n, &odd_n](auto &kv) -> bool {
        CHECK_FOR_INTERRUPTS();
        kv.v = (kv.v + 1) / 2;
        return true;
    });
    TestAssert(visited == odd_n);
    timer.report("Bulk visit test passed.");
    even_n = 0;
    odd_n = 0;
    nerased = tt.erase_if([&even_n, &odd_n](auto &kv, bool &modified) -> bool {
        CHECK_FOR_INTERRUPTS();
        if (kv.v % 2 == 0) {
            ++even_n;
            modified = kv.v % 4 == 0;
            return !modified;
        }
        ++odd_n;
        modified = false;
        return false;
    });
    TestAssert(visited == odd_n + even_n);
    TestAssert(visited / 2 <= odd_n);
    TestAssert(visited / 2 <= even_n);
    TestAssert(even_n / 2 <= nerased);
    timer.report("Bulk erase test passed.");
    timer.destroy();
    elog(ERROR, "Stop here.");
}

struct BM25BuildBase {
    Relation heap;
    Relation index;
    IndexInfo *index_info;
    AttrNumber nattr;
    BM25Store store;
    Jieba **dictionaries;
    Timer *timer;
    MemoryContext ctx;
    BM25BuildBase(Relation in_heap, Relation in_index, IndexInfo *in_index_info, Timer *timer)
        : heap(in_heap),
          index(in_index),
          index_info(in_index_info),
          nattr(in_index_info->ii_NumIndexAttrs),
          store(in_index, false),
          timer(timer),
          ctx(AllocSetContextCreate(CurrentMemoryContext, "bm25 build tokenizer context",
                                    ALLOCSET_DEFAULT_SIZES, STANDARD_CONTEXT,
                                    DEFAULT_MEMORY_CONTEXT_MAX_SIZE, true))
    {
        if (RelationIsGlobalIndex(index)) {
            --nattr;
        }
        dictionaries = (Jieba **)palloc(sizeof(Jieba *) * nattr);
        for (AttrNumber i = 1; i <= nattr; ++i) {
            Oid dict_id = store.get_dict_id(i);
            dictionaries[i - 1] = OidIsValid(dict_id) ? (Jieba *)get_jieba(dict_id) : NULL;
        }
    }
    void destroy() {
        if (dictionaries) {
            for (AttrNumber i = 0; i < nattr; ++i) {
                if (dictionaries[i]) {
                    release_dict_resource(dictionaries[i]);
                    dictionaries[i] = NULL;
                }
            }
            pfree(dictionaries);
            dictionaries = NULL;
        }
        store.destroy();
        MemoryContextDelete(ctx); 
    }
};

struct BM25BuildState : public BM25BuildBase {
    Oid heap_id;
    Oid index_id;
    Oid heap_part_id;
    Oid index_part_id;

    pthread_mutex_t lock;
    std::atomic<int> part_cnt{0};
    double total_tuples{0};
    ParallelHeapScanDescData parallel_data;

    BM25BuildState(Relation in_heap, Relation in_index, IndexInfo *in_index_info, Timer *timer)
        : BM25BuildBase(in_heap, in_index, in_index_info, timer)
    {
        Assert(RelationIsValid(heap));
        if (RelationIsPartition(heap)) {
            Assert(RelationIsPartition(index));
            heap_id = GetBaseRelOidOfParition(heap);
            index_id = GetBaseRelOidOfParition(index);
            heap_part_id = RelationGetRelid(heap);
            index_part_id = RelationGetRelid(index);
        } else {
            Assert(!RelationIsPartition(index));
            heap_id = RelationGetRelid(heap);
            index_id = RelationGetRelid(index);
            heap_part_id = InvalidOid;
            index_part_id = InvalidOid;
        }

        pthread_mutex_init(&lock, NULL);
    }
    void destroy()
    {
        BM25BuildBase::destroy();
        pthread_mutex_destroy(&lock);
    }
};


static void scan_and_insert(Relation index, HeapTuple hup, Datum *values, const bool *isnull,
                            bool tuple_alive, void *void_state)
{
    BM25BuildBase *state = (BM25BuildBase *)void_state;
    state->timer->inc_loop_count_forground_report("Building BM25 index");
    if (!tuple_alive) {
        return;
    }
    CHECK_FOR_INTERRUPTS();
    auto old_ctx = MemoryContextSwitchTo(state->ctx);
    const AttrNumber nattr = state->nattr;
    Documents docs(hup->t_self);
    if (RelationIsGlobalIndex(index)) {
        docs.part_id = DatumGetObjectId(values[nattr]);
    }
    for (AttrNumber i = 0; i < nattr; ++i) {
        if (isnull[i]) { /* we don't handle null attributes */
            continue;
        }
        Oid typid = RelationGetDescr(index)->attrs[i].atttypid;
        if (typid != SPARSEVECTOROID) {
            docs.docs.emplace_back(i + 1, values[i], typid, state->dictionaries[i]);
        } else {
            SparseVector *temp = DatumGetSparseVector(values[i]);
            if (index->rd_opfamily[i] == SPARSEVEC_COSINE_FULLTEXT_FAM_OID) {
                sparsevector_normalize(temp);
            }
            if (temp->nnz > 0) {
                docs.docs.emplace_back(i + 1, temp);
            }
        }
    }
    state->store.insert(docs);
    docs.destroy();
    MemoryContextSwitchTo(old_ctx);
    MemoryContextResetAndDeleteChildren(state->ctx);
}

static void parallel_build_main(const BgWorkerContext *bwc)
{
    BM25BuildState *state = (BM25BuildState *)bwc->bgshared;
    Relation heap_base = NULL;
    Relation index_base = NULL;
    Partition heap_part = NULL;
    Partition index_part = NULL;
    Relation heap;
    Relation index;
    if (state->heap_part_id == InvalidOid) {
        heap = heap_open(state->heap_id, NoLock);
        index = index_open(state->index_id, NoLock);
    } else {
        heap_base = heap_open(state->heap_id, NoLock);
        index_base = index_open(state->index_id, NoLock);
        heap_part = partitionOpen(heap_base, state->heap_part_id, NoLock);
        index_part = partitionOpen(index_base, state->index_part_id, NoLock);
        heap = partitionGetRelation(heap_base, heap_part);
        index = partitionGetRelation(index_base, index_part);
    }

    IndexInfo *index_info = BuildIndexInfo(index);
    BM25BuildBase base(heap, index, index_info, state->timer);
    TableScanDesc scan = tableam_scan_begin_parallel(heap, &state->parallel_data);
    double res = IndexBuildHeapScan(heap, index, index_info, true, scan_and_insert, &base, scan);
    base.destroy();
    pthread_mutex_lock(&state->lock);
    state->total_tuples += res;
    pthread_mutex_unlock(&state->lock);

    if (state->heap_part_id == InvalidOid) {
        heap_close(heap, NoLock);
        index_close(index, NoLock);
    } else {
        releaseDummyRelation(&heap);
        releaseDummyRelation(&index);
        partitionClose(heap_base, heap_part, NoLock);
        partitionClose(index_base, index_part, NoLock);
        heap_close(heap_base, NoLock);
        index_close(index_base, NoLock);
    }
}

static void build_global(Relation heap, Relation index, IndexInfo *index_info, BM25BuildState *state)
{
    List *heapparts = RelationIsSubPartitioned(heap) ?
        RelationGetSubPartitionList(heap, NoLock) : relationGetPartitionList(heap, NoLock);
    const int npart = list_length(heapparts);
    for (;;) {
        int ipart = state->part_cnt.fetch_add(1, std::memory_order::memory_order_relaxed);
        if (ipart >= npart) {
            break;
        }
        Partition heappart = (Partition)list_nth(heapparts, ipart);
        Relation targetheap = RelationIsSubPartitioned(heap) ?
            SubPartitionGetRelation(heap, heappart, NoLock) : partitionGetRelation(heap, heappart);
        BM25BuildBase base(targetheap, index, index_info, state->timer);
        double res = tableam_index_build_scan(targetheap, index, index_info, true, scan_and_insert,
                                              &base, NULL);
        base.destroy();
        releaseDummyRelation(&targetheap);
        pthread_mutex_lock(&state->lock);
        state->total_tuples += res;
        pthread_mutex_unlock(&state->lock);
    }
    releasePartitionList(heap, &heapparts, NoLock);
}

static void parallel_build_global_main(const BgWorkerContext *bwc)
{
    BM25BuildState *state = (BM25BuildState *)bwc->bgshared;
    Relation heap = heap_open(state->heap_id, NoLock);
    Relation index = index_open(state->index_id, NoLock);
    Assert(RelationIsGlobalIndex(index));
    IndexInfo *index_info = BuildIndexInfo(index);
    build_global(heap, index, index_info, state);
    index_close(index, NoLock);
    heap_close(heap, NoLock);
}

static void parallel_clean_main(const BgWorkerContext *bwc)
{
    BM25BuildState *state = (BM25BuildState *)bwc->bgshared;
    state->destroy();
}

static void init_meta_page(Relation index, ForkNumber fork_number)
{
    /* no thread contension here yet */
    Buffer buf = ReadBufferExtended(index, fork_number, P_NEW, RBM_NORMAL, NULL);
    Buffer buf2 = ReadBufferExtended(index, fork_number, P_NEW, RBM_NORMAL, NULL);
    Assert(BufferGetBlockNumber(buf) == BM25_META_BLKNO);
    Assert(BufferGetBlockNumber(buf2) == BM25_STATISTICS_BLKNO); /* make sure buf2 get blkno 1 */
    Page page = BufferGetPage(buf);
    PageInit(page, BLCKSZ, 0);
    GetBM25MetaPage(page)->init(index);
    ((PageHeader)page)->pd_lower =
        (PageGetContents(page) + sizeof(BM25MetaPageData)) - (char *)page;
    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    page = BufferGetPage(buf2);
    PageInit(page, BLCKSZ, 0);
    GetBM25StatisticsPage(page)->init();
    ((PageHeader) page)->pd_lower =
        (PageGetContents(page) + MAXALIGN(offsetof(BM25StatisticsPageData, stats))
        + sizeof(GlobalStats) * BM25_MAX_NATTR) - (char *)page;
    MarkBufferDirty(buf2);
    ReleaseBuffer(buf2);
}

Datum bm25build(PG_FUNCTION_ARGS)
{
    IndexBuildResult *result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    result->all_part_tuples = NULL;
    Relation heap = (Relation)PG_GETARG_POINTER(0);
    Relation index = (Relation)PG_GETARG_POINTER(1);
    DO_HASHTABLE_TEST(index);
    init_meta_page(index, MAIN_FORKNUM);
    if (!RelationIsValid(heap)) {
        result->heap_tuples = result->index_tuples = 0;
        PG_RETURN_POINTER(result);
    }
    IndexInfo *index_info = (IndexInfo *)PG_GETARG_POINTER(2);
    uint32 parallel_workers = get_bm25_parallel_workers(index);
    if (parallel_workers > 0 && heap->rd_rel->relpersistence == RELPERSISTENCE_GLOBAL_TEMP) {
        ereport(NOTICE, (errmsg("switch off parallel mode for global temp table")));
        parallel_workers = 0;
    }

    size_t reltuples = get_relstats_reltuples(heap);
    char indexName[NAMEDATALEN + 1];
	char partIndexName[NAMEDATALEN + 1];
    populate_index_partition_name(index, indexName, partIndexName);
    Timer timer(reltuples, 50'000ul, indexName, partIndexName);
    timer.set_stage("Building Index");
    double res;
    if (parallel_workers > 0) {
        uint32 nparts = 0;
        if (RelationIsGlobalIndex(index)) {
            if (RelationIsSubPartitioned(heap)) {
                nparts = GetSubPartitionNumber(heap);
            } else {
                nparts = getPartitionNumber(heap->partMap);
            }
        }
        BM25BuildState *state =
            (BM25BuildState *)palloc(sizeof(BM25BuildState) + sizeof(double) * nparts);
        new (state) BM25BuildState(heap, index, index_info, &timer);
        int nworker;
        if (!RelationIsGlobalIndex(index)) {
            HeapParallelscanInitialize(&state->parallel_data, heap);
            nworker = LaunchBackgroundWorkers(parallel_workers, state,
                parallel_build_main, parallel_clean_main, false);
            TableScanDesc scan = tableam_scan_begin_parallel(heap, &state->parallel_data);
            res = IndexBuildHeapScan(heap, index, index_info, true, scan_and_insert, state, scan);
        } else {
            nworker = LaunchBackgroundWorkers(parallel_workers, state, parallel_build_global_main,
                parallel_clean_main, false);
            build_global(heap, index, index_info, state);
            res = 0;
        }
        BgworkerListWaitFinish(&nworker);
        res += state->total_tuples;
        BgworkerListSyncQuit();
    } else {
        BM25BuildState state(heap, index, index_info, &timer);
        if (RelationIsGlobalIndex(index)) {
            List *heapparts = RelationIsSubPartitioned(heap) ?
                RelationGetSubPartitionList(heap, NoLock) : relationGetPartitionList(heap, NoLock);
            int npart = list_length(heapparts);
            releasePartitionList(heap, &heapparts, NoLock);
            res = 0;
            double *temp = GlobalIndexBuildHeapScan(heap, index, index_info, scan_and_insert, &state);
            for (int i = 0; i < npart; ++i) {
                res += temp[i];
            }
            pfree(temp);
        } else {
            res = IndexBuildHeapScan(heap, index, index_info, true, scan_and_insert, &state, NULL);
        }
        state.destroy();
    }

    timer.report("BM25 index built");
    timer.destroy();
    if (RelationNeedsWAL(index)) {
        log_newpage_range(index, MAIN_FORKNUM,
                          0, RelationGetNumberOfBlocksInFork(index, MAIN_FORKNUM), true,
                          RM_BM25_ID, XLOG_BM25_BUILD_INDEX, NULL, true);
    }

    result->heap_tuples = result->index_tuples = res;
    PG_RETURN_POINTER(result);
}

Datum bm25buildempty(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    init_meta_page(index, INIT_FORKNUM);
    if (RelationNeedsWAL(index)) {
        log_newpage_range(index, INIT_FORKNUM,
                          0, RelationGetNumberOfBlocksInFork(index, INIT_FORKNUM), true,
                          RM_BM25_ID, XLOG_BM25_BUILD_INDEX);
    }
    PG_RETURN_VOID();
}

Datum bm25beginscan(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    int nkeys = PG_GETARG_INT32(1);
    int norderbys = PG_GETARG_INT32(2);
    IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);

    scan->opaque = NEW BM25Scanner();
    scan->xs_recheck = false;
    scan->xs_ctup.t_self = {{0, 0}, 0};
    PG_RETURN_POINTER(scan);
}

Datum bm25rescan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
    int nkeys = PG_GETARG_INT32(2);
    ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
    int norderbys = PG_GETARG_INT32(4);

    if (scankey && nkeys > 0) {
        if (scan->numberOfKeys == nkeys) {
            /* do nothing */
        } else if (scan->numberOfKeys > 0) {
            scan->keyData = (ScanKey)repalloc(scan->keyData, sizeof(ScanKeyData) * nkeys);
        } else {
            scan->keyData = (ScanKey)palloc(sizeof(ScanKeyData) * nkeys);
        }
        scan->numberOfKeys = nkeys;
        errno_t rc = memmove_s(scan->keyData, nkeys * sizeof(ScanKeyData), scankey,
                               nkeys * sizeof(ScanKeyData));
        securec_check(rc, "", "");
    } else if (scan->numberOfKeys > 0) {
        pfree_ext(scan->keyData);
        scan->numberOfKeys = 0;
    }
    if (orderbys && norderbys > 0) {
        if (scan->numberOfOrderBys == norderbys) {
            /* do nothing */
        } else if (scan->numberOfOrderBys > 0) {
            scan->orderByData = (ScanKey)repalloc(scan->orderByData, sizeof(ScanKeyData) * norderbys);
        } else {
            scan->orderByData = (ScanKey)palloc(sizeof(ScanKeyData) * norderbys);
        }
        scan->numberOfOrderBys = norderbys;
        errno_t rc = memmove_s(scan->orderByData, norderbys * sizeof(ScanKeyData), orderbys,
                               norderbys * sizeof(ScanKeyData));
        securec_check(rc, "", "");
    } else if (scan->numberOfOrderBys > 0) {
        pfree_ext(scan->orderByData);
        scan->numberOfOrderBys = 0;
    }

    auto *scanner = reinterpret_cast<BM25Scanner *>(scan->opaque);
    LimitInfo linfo = scanner->linfo;
    scanner->destroy();
    new (scanner) BM25Scanner();
    scanner->linfo = linfo;
    scan->xs_recheck = false;
    scan->xs_ctup.t_self = {{0, 0}, 0};
    PG_RETURN_VOID();
}

Datum bm25gettuple(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    auto *scanner = reinterpret_cast<BM25Scanner *>(scan->opaque);
    if (!scanner->inited()) {
        for (int i = 0; i < scan->numberOfKeys; ++i) {
            ScanKey key = scan->keyData + i;
            /* should've been prevented from optimizer */
            if (unlikely(key->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL))) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("BM25 index does not support IS [NOT] NULL clause.")));
            }
            if (unlikely(key->sk_flags & SK_ISNULL)) {
                PG_RETURN_BOOL(false);
            }
            Oid in_type = InvalidOid;
            switch (key->sk_func.fn_oid) {
                case F_BM25_MATCH_TEXT:
                case F_BM25_RANK_MATCH_TEXT:
                case F_BM25_MATCH_TEXTARR:
                    in_type = TEXTOID;
                    break;
                case F_BM25_MATCH_VARCHAR:
                case F_BM25_RANK_MATCH_VARCHAR:
                case F_BM25_MATCH_VARCHARARR:
                    in_type = VARCHAROID;
                    break;
                case F_BM25_MATCH_CHAR:
                case F_BM25_RANK_MATCH_CHAR:
                case F_BM25_MATCH_CHARARR:
                    in_type = BPCHAROID;
                    break;
                case F_BM25_RANK_MATCH_TEXTARR:
                    in_type = TEXTARRAYOID;
                    break;
                case F_BM25_RANK_MATCH_VARCHARARR:
                    in_type = VARCHARARRAYOID;
                    break;
                case F_BM25_RANK_MATCH_CHARARR:
                    in_type = BPCHARARRAYOID;
                    break;
                default:
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("FULLTEXT index does not support function %s for search.",
                            format_procedure(key->sk_func.fn_oid))));
            }
            switch (in_type) {
                case TEXTOID:
                case VARCHAROID:
                case BPCHAROID: {
                    char *s = get_cstring(key->sk_argument, in_type);
                    scanner->row_queries.emplace_back(s, key->sk_attno, key->sk_strategy);
                } break;
                case TEXTARRAYOID:
                case VARCHARARRAYOID:
                case BPCHARARRAYOID: {
                    ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
                    const Oid etype = in_type == TEXTARRAYOID ? TEXTOID :
                                      in_type == VARCHARARRAYOID ? VARCHAROID : BPCHAROID;
                    uint32 arr_len = 0;
                    const_string_arr arr_cstr = get_arr_cstring(arr, etype, arr_len);
                    if (arr_len == 0) {
                        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("BM25 index does not support empty array.")));
                    }
                    arr_cstr = (char **)repalloc(arr_cstr, (arr_len + 1u) * sizeof(char *));
                    arr_cstr[arr_len] = NULL; /* make it a null terminated array */
                    scanner->row_queries.emplace_back(arr_cstr, key->sk_attno, key->sk_strategy);
                } break;
                default:
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("BM25 index does not support type %s for search.",
                            format_type_be(in_type))));
            }
            
        }
        scanner->require_order_by = scan->numberOfOrderBys > 0;
        for (int i = 0; i < scan->numberOfOrderBys; ++i) {
            ScanKey key = scan->orderByData + i;
            switch (key->sk_func.fn_oid) {
                case F_SPARSEVECTOR_COSINE_DISTANCE:
                case F_SPARSEVECTOR_NEGATIVE_INNER_PRODUCT: {
                    SparseVector *sv = (SparseVector *)DatumGetPointer(key->sk_argument);
                    if (!sv) {
                        PG_RETURN_BOOL(false);
                    }
                    bool need_norm = scan->indexRelation->rd_opfamily[key->sk_attno - 1] ==
                        SPARSEVEC_COSINE_FULLTEXT_FAM_OID;
                    scanner->row_vectors.emplace_back(sv, key->sk_weight, key->sk_attno, need_norm);
                } break;
                case F_BM25_SCORE:
                    for (auto &rq : scanner->row_queries) {
                        rq.weight = key->sk_weight;
                    }
                    scanner->bm25_weight = key->sk_weight;
                    break;
                default:
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("BM25 index does not support function %s in ORDER BY clause.",
                            format_procedure(key->sk_func.fn_oid))));
            }
        }

retry:
        auto &context = scanner->context;
        BM25Store store(scan->indexRelation, false);
        QueryGroup *filter;
        QueryGroup *queries;
        uint16 nquery;
        scanner->extract_query_group(&filter, &queries, nquery, store.get_dict_id());
        store.preprocess_query_context(context, filter, queries, nquery);
        if (context.empty()) {
            store.destroy();
            PG_RETURN_BOOL(false);
        }

        uint32 topk = scanner->get_topk();
        if (!context.init_process(topk)) {
            PG_RETURN_BOOL(false);
        }
        if (scanner->require_order_by) {
            context.process(topk, &store);
            const size_t s = context.top_explored.size();
            scanner->scores.reserve(s);
            scanner->tids.reserve(s);
            if (RelationIsGlobalIndex(scan->indexRelation)) {
                if (!scanner->part_ids) {
                    scanner->part_ids.emplace(s);
                }
                for (const auto &kv : context.top_explored) {
                    scanner->scores.push_back(kv.first);
                    scanner->tids.push_back(kv.second.tid);
                    scanner->part_ids->push_back(kv.second.part_id);
                }
            } else {
                for (const auto &kv : context.top_explored) {
                    scanner->scores.push_back(kv.first);
                    scanner->tids.push_back(kv.second.tid);
                }
            }
            ann_helper::optional_destroy(context.top_explored);
        }
        store.destroy();
        scanner->set_inited();
    }

    if (scanner->require_order_by) {
        while (scanner->cur_offset < scanner->tids.size()) {
            scan->xs_ctup.t_self = scanner->tids[scanner->cur_offset];
            Oid part_id = InvalidOid;
            if (scanner->part_ids) {
                part_id = scanner->part_ids.value()[scanner->cur_offset];
                if (scan->xs_want_ext_oid && GPIScanCheckPartOid(scan->xs_gpi_scan, part_id)) {
                    scan->xs_gpi_scan->currPartOid = part_id;
                }
            }
            scanner->score = scanner->scores[scanner->cur_offset];
            ++scanner->cur_offset;
            if (scanner->returned && scanner->returned->contains({scan->xs_ctup.t_self, part_id})) {
                continue;
            }
            PG_RETURN_BOOL(true);
        }
        if (!scanner->linfo.with_limit ||
            scanner->get_topk() > std::min<uint32>(max_daat_threshold, scanner->tids.size()) ||
            (RelationIsPartition(scan->heapRelation) && !RelationIsGlobalIndex(scan->indexRelation))) {
            PG_RETURN_BOOL(false);
        }
        scanner->linfo.limit_count *= 2;
        scanner->scores.clear();
        const size_t size = scanner->tids.size();
        if (!scanner->returned) {
            scanner->returned.emplace(size + scanner->linfo.limit_count);
        }
        if (scanner->part_ids) {
            for (size_t i = 0; i < size; ++i) {
                scanner->returned->insert({scanner->tids[i], (*scanner->part_ids)[i]});
            }
            scanner->part_ids->clear();
        } else {
            for (const auto &tid : scanner->tids) {
                scanner->returned->insert({tid, InvalidOid});
            }
        }
        scanner->tids.clear();
        scanner->context.reset();
        goto retry;
    }
    auto &context = scanner->context;
    uint64 doc_id = context.next();
    if (doc_id == 0) {
        PG_RETURN_BOOL(false);
    }
    bool has_tuple = false;
    BM25Store store(scan->indexRelation, false);
    for (; doc_id != 0; doc_id = context.next()) {
        BM25LOG("Accept doc_id %lu", doc_id);
        DocumentStats stats[store.get_nattr()];
        context.record_doc();
        Oid part_id;
        if (store.get_doc_info(doc_id, stats, &scan->xs_ctup.t_self, part_id)) {
            has_tuple = true;
            scanner->score = context.get_score(doc_id, stats);
            if (scan->xs_want_ext_oid && GPIScanCheckPartOid(scan->xs_gpi_scan, part_id)) {
                scan->xs_gpi_scan->currPartOid = part_id;
            }
            break;
        }
    }
    store.destroy();
    PG_RETURN_BOOL(has_tuple);
}

Datum bm25endscan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    reinterpret_cast<BM25Scanner *>(scan->opaque)->destroy();
    pfree(scan->opaque);
    PG_RETURN_VOID();
}

Datum bm25costestimate(PG_FUNCTION_ARGS)
{
    /* BM25 index is used merely rule based as all other paths are invalid,
     * we set the cost to the possible lowest value */
    IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
    Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
    Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
    Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
    double *correlation = (double *)PG_GETARG_POINTER(6);

    IndexOptInfo *index = path->indexinfo;

    *startupcost = 0.01 * index->ncolumns;  /* use the short one */
    *totalcost = 0.05 * index->ncolumns;
    *selectivity = index->isGlobal ? 0.005 : 0.8;   /* prefer global as much as possible */
    *correlation = 0;

    PG_RETURN_VOID();
}

Datum bm25insert(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    Datum *values = (Datum *)PG_GETARG_POINTER(1);
    bool *isnull = (bool *)PG_GETARG_POINTER(2);
    ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);

    BM25Store store(index, true);
    Documents docs(*ht_ctid);
    const uint16 nattr = store.get_nattr();
    for (uint16 i = 0; i < nattr; ++i) {
        if (isnull[i]) {
            continue;
        }
        const Oid dict_id = store.get_dict_id()[i];
        const Oid typid = RelationGetDescr(index)->attrs[i].atttypid;
        void *dict = NULL;
        if (OidIsValid(dict_id)) {
            dict = get_jieba(dict_id);
        } else if (typid == SPARSEVECTOROID) {
            SparseVector *temp = DatumGetSparseVector(values[i]);
            if (index->rd_opfamily[i] == SPARSEVEC_COSINE_FULLTEXT_FAM_OID) {
                sparsevector_normalize(temp);
            }
            if (temp->nnz > 0) {
                docs.docs.emplace_back(i + 1, temp);
            }
            continue;
        }
        docs.docs.emplace_back(i + 1, values[i], typid, dict);
        if (dict) {
            release_dict_resource(dict);
        }
    }
    if (RelationIsGlobalIndex(index)) {
        docs.part_id = DatumGetObjectId(values[nattr]);
    }
    store.insert(docs);
    docs.destroy();
    store.destroy();
    PG_RETURN_BOOL(true);
}

Datum bm25bulkdelete(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    if (!stats) {
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    }
    IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
    void *callback_state = (void *)PG_GETARG_POINTER(3);
    BM25Store store(info->index, true);
    store.vacuum(callback, callback_state, stats);
    store.destroy();
    PG_RETURN_POINTER(stats);
}

Datum bm25vacuumcleanup(PG_FUNCTION_ARGS)
{
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    PG_RETURN_POINTER(stats);
}

Datum bm25options(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    static const relopt_parse_elt tab[] = {
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(BM25Options, parallel_workers)},
        {"dicts", RELOPT_TYPE_STRING, offsetof(BM25Options, dicts_offset)},
        {"algorithms", RELOPT_TYPE_STRING, offsetof(BM25Options, algorithms_offset)},
        {"coefficients", RELOPT_TYPE_STRING, offsetof(BM25Options, coefficients_offset)},
    };
    int noption = 0;
    relopt_value *options = parseRelOptions(reloptions, validate, RELOPT_KIND_BM25, &noption);
    if (noption == 0) {
        PG_RETURN_NULL();
    }
    BM25Options *rdopts = (BM25Options *)allocateReloptStruct(sizeof(BM25Options), options, noption);
    fillRelOptions((void *)rdopts, sizeof(BM25Options), options, noption, validate, tab, lengthof(tab));
    PG_RETURN_BYTEA_P((bytea *)rdopts);
}

Datum bm25_match_text(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), TEXTOID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), TEXTOID);
    bool res = bm25_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_match_varchar(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), VARCHAROID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), VARCHAROID);
    bool res = bm25_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_match_char(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), BPCHAROID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), BPCHAROID);
    bool res = bm25_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_match_textarr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    uint32 arr1_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, TEXTOID, arr1_len);
    char *q = get_cstring(PG_GETARG_DATUM(1), TEXTOID);
    bool res = bm25_match_arr(arr1, arr1_len, q);
    free_string_arr(arr1, arr1_len);
    pfree(q);
    PG_RETURN_BOOL(res);
}

Datum bm25_match_varchararr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    uint32 arr1_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, VARCHAROID, arr1_len);
    char *q = get_cstring(PG_GETARG_DATUM(1), VARCHAROID);
    bool res = bm25_match_arr(arr1, arr1_len, q);
    free_string_arr(arr1, arr1_len);
    pfree(q);
    PG_RETURN_BOOL(res);
}

Datum bm25_match_chararr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    uint32 arr1_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, BPCHAROID, arr1_len);
    char *q = get_cstring(PG_GETARG_DATUM(1), BPCHAROID);
    bool res = bm25_match_arr(arr1, arr1_len, q);
    free_string_arr(arr1, arr1_len);
    pfree(q);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_text(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), TEXTOID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), TEXTOID);
    bool res = bm25_rank_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_varchar(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), VARCHAROID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), VARCHAROID);
    bool res = bm25_rank_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_char(PG_FUNCTION_ARGS)
{
    char *c1 = get_cstring(PG_GETARG_DATUM(0), BPCHAROID);
    char *c2 = get_cstring(PG_GETARG_DATUM(1), BPCHAROID);
    bool res = bm25_rank_match(c1, c2);
    pfree_ext(c1);
    pfree_ext(c2);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_textarr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType *a2 = PG_GETARG_ARRAYTYPE_P(1);
    uint32 arr1_len = 0;
    uint32 arr2_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, TEXTOID, arr1_len);
    const_string_arr arr2 = get_arr_cstring(a2, TEXTOID, arr2_len);
    bool res = bm25_rank_match_arr(arr1, arr1_len, arr2, arr2_len);
    free_string_arr(arr1, arr1_len);
    free_string_arr(arr2, arr2_len);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_varchararr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType *a2 = PG_GETARG_ARRAYTYPE_P(1);
    uint32 arr1_len = 0;
    uint32 arr2_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, VARCHAROID, arr1_len);
    const_string_arr arr2 = get_arr_cstring(a2, VARCHAROID, arr2_len);
    bool res = bm25_rank_match_arr(arr1, arr1_len, arr2, arr2_len);
    free_string_arr(arr1, arr1_len);
    free_string_arr(arr2, arr2_len);
    PG_RETURN_BOOL(res);
}

Datum bm25_rank_match_chararr(PG_FUNCTION_ARGS)
{
    ArrayType *a1 = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType *a2 = PG_GETARG_ARRAYTYPE_P(1);
    uint32 arr1_len = 0;
    uint32 arr2_len = 0;
    const_string_arr arr1 = get_arr_cstring(a1, BPCHAROID, arr1_len);
    const_string_arr arr2 = get_arr_cstring(a2, BPCHAROID, arr2_len);
    bool res = bm25_rank_match_arr(arr1, arr1_len, arr2, arr2_len);
    free_string_arr(arr1, arr1_len);
    free_string_arr(arr2, arr2_len);
    PG_RETURN_BOOL(res);
}

Datum bm25_score(PG_FUNCTION_ARGS)
{
    ereport(ERROR,
            (errcode(ERRCODE_OPERATE_NOT_SUPPORTED),
             errmsg("Function bm25_score can only be evaluated with "
                    "the @~@ operator under Text Match index scan."),
             errhint("Please make sure FULLTEXT index is built and can be used by the query.")));
    PG_RETURN_FLOAT4(0.0f); /* keep compiler quiet */
}

Datum bm25_tokenize(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(1);
    char *sentence_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    void *dict = get_jieba(dict_id);
    Vector<char *> keywords = ((Jieba *)dict)->cut_mix(sentence_str);
    release_dict_resource(dict);
    pfree(sentence_str);
    Datum *data = (Datum *)palloc(sizeof(Datum) * keywords.size());
    int s = 0;
    for (char *a : keywords) {
        data[s] = CStringGetTextDatum(a);
        ++s;
        pfree(a);
    }
    optional_destroy(keywords);
    ArrayType *res = construct_array(data, s, TEXTOID, -1, false, 'i');
    PG_RETURN_POINTER(res);
}

Datum bm25_query_tokenize(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(1);
    char *sentence_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    void *dict = get_jieba(dict_id);
    Vector<char *> keywords = ((Jieba *)dict)->cut_query(sentence_str);
    release_dict_resource(dict);
    pfree(sentence_str);
    Datum *data = (Datum *)palloc(sizeof(Datum) * keywords.size());
    int s = 0;
    for (char *a : keywords) {
        data[s] = CStringGetTextDatum(a);
        ++s;
        pfree(a);
    }
    optional_destroy(keywords);
    ArrayType *res = construct_array(data, s, TEXTOID, -1, false, 'i');
    PG_RETURN_POINTER(res);
}

Datum bm25_search_highlight(PG_FUNCTION_ARGS)
{
    Oid dict_id = PG_GETARG_OID(4);
    text *text_arg = PG_GETARG_TEXT_P(0);
    text *query_arg = PG_GETARG_TEXT_P(1);
    text *lchar_arg = PG_GETARG_TEXT_P(2);
    text *rchar_arg = PG_GETARG_TEXT_P(3);
    char *text = text_to_cstring(text_arg);
    char *query = text_to_cstring(query_arg);
    char *lchar = text_to_cstring(lchar_arg);
    char *rchar = text_to_cstring(rchar_arg);
    void *dict = get_jieba(dict_id);
    char *res = ((Jieba *)dict)->highlight(text, query, lchar, rchar);
    release_dict_resource(dict);
    pfree(text);
    pfree(query);
    pfree(lchar);
    pfree(rchar);
    PG_RETURN_TEXT_P(cstring_to_text(res));
}