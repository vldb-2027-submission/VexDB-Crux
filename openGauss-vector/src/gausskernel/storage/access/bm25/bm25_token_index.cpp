/**
 * Copyright ...
 */

#include <vtl/hashtable>
#include <vtl/lrucache.hpp>

#include "access/bm25/bm25_token_index.h"
#include "access/bm25/bm25_inverted_list.h"
#include "access/bm25/bm25_internal.h"
#include "access/bm25/tokenizer/token_pool.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/halfutils.h"
#include "access/annvector/module/timer.h"
#include "access/annvector/ann_utils.h"
#include "catalog/pg_partition_fn.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"

using namespace bm25;
using namespace bm25_tokenizer;

ShortToken::ShortToken(const char *s)
{
    size_t i = 0;
    char *tok_ptr = tok;
    for (; i < short_word_len; ++i) {
        *tok_ptr = *s;
        if (*s == '\0') {
            ++i;
            break;
        }
        ++s;
        ++tok_ptr;
    }
    for (; i < short_word_len; ++i) {
        *tok_ptr = '\0';
        ++tok_ptr;
    }
}

MidToken::MidToken(const char *s)
{
    size_t i = mid_word_len;
    char *tok_ptr = tok;
    for (; i != 0; --i) {
        *tok_ptr = *s;
        if (*s == '\0') {
            --i;
            break;
        }
        ++s;
        ++tok_ptr;
    }
    errno_t rc = memset_s(tok_ptr, i, 0, i);
    securec_check_c(rc, "\0", "\0");
}

LongToken::LongToken(const char *s)
{
    size_t i = long_word_len;
    char *tok_ptr = tok;
    for (; i != 0; --i) {
        *tok_ptr = *s;
        if (*s == '\0') {
            --i;
            break;
        }
        ++s;
        ++tok_ptr;
    }
    errno_t rc = memset_s(tok_ptr, i, 0, i);
    securec_check_c(rc, "\0", "\0");
}

FullToken::FullToken(const char *s)
{
    size_t i = max_token_length;
    char *tok_ptr = tok;
    for (; i != 0; --i) {
        *tok_ptr = *s;
        if (*s == '\0') {
            --i;
            break;
        }
        ++s;
        ++tok_ptr;
    }
    errno_t rc = memset_s(tok_ptr, i, 0, i);
    securec_check_c(rc, "\0", "\0");
}

void TokenIndex::create_token_index(Relation rel, uint32 sparse_bits, TokenIndexMeta &meta, bool need_wal)
{
    const int nattr = RelationGetDescr(rel)->natts;
    for (int i = 0; i < nattr; ++i) {
        if (sparse_bits & (1 << i)) {
            meta.attr_meta[i].short_store_blkno = dim_store::get_disk_hashtable(rel, need_wal);
            meta.attr_meta[i].mid_store_blkno = dim_store::get_disk_hashtable(rel, need_wal);
            continue;
        }
        meta.attr_meta[i].short_store_blkno = short_store::get_disk_hashtable(rel, need_wal);
        meta.attr_meta[i].mid_store_blkno = mid_store::get_disk_hashtable(rel, need_wal);
        meta.attr_meta[i].long_store_blkno = long_store::get_disk_hashtable(rel, need_wal);
        meta.attr_meta[i].full_store_blkno = full_store::get_disk_hashtable(rel, need_wal);
    }
}

bool TokenIndex::get(const Token &token, TokenIndexEntry &out)
{
    return helper(token, [&out](const auto &tok, auto &store) -> bool {
        return store.cvisit(tok, [&out](const auto &kv) { out = kv.v; });
    });
}

bool TokenIndex::get(const SparseElement &sd, SparseDimIndexEntry &out)
{
    return helper2(sd, [&out](const auto &d, auto &store) -> bool {
        return store.cvisit(d, [&out](const auto &kv) { out = kv.v; });
    });
}

void TokenIndex::insert(const Token &token, uint64 doc_id, uint32 freq, const void *il_meta)
{
    BlockNumber start_blkno, insert_blkno;
    uint32 ndoc;
    bool updated = false;
    bool rewrite;
retry:
    rewrite = false;
retry2:
    bool res = helper(token, [&](const auto &tok, auto &store) -> bool {
        return store.try_emplace_or_visit(tok, [&](auto &kv) -> bool {
            if (!updated) {
                ndoc = ++kv.v.stats.ndoc;
            }
            if (rewrite) {
                kv.v.start_blkno = start_blkno;
                kv.v.insert_blkno = insert_blkno;
            } else {
                start_blkno = kv.v.start_blkno;
                insert_blkno = kv.v.insert_blkno;
            }
            return updated || !rewrite;
        }, _rel, il_meta, doc_id, freq, need_wal());
    });
    if (res) {  /* emplace constructor called, no need to call further insertion process */
        return;
    }
    updated = true;
    InvertedList il(_rel, start_blkno, insert_blkno, (const InvertedListMeta)il_meta, _need_wal);
    if (il.upon_threshold(ndoc)) {
        if (!il.try_upgrade(ndoc, start_blkno, insert_blkno, (const InvertedListMeta)il_meta)) {
            il.destroy();
            CHECK_FOR_INTERRUPTS();
            goto retry;
        }
        il.destroy();
        rewrite = true;
        goto retry2;
    }

    BlockNumber res_blkno = il.insert<false>(doc_id, freq);
    il.destroy();
    if (!BlockNumberIsValid(res_blkno)) {
        CHECK_FOR_INTERRUPTS();
        goto retry;
    }

    if (insert_blkno != res_blkno) {  /* only set insert_blkno if needed */
        helper(token, [res_blkno](const auto &tok, auto &store) -> bool {
            store.visit(tok, [res_blkno](auto &kv) -> bool {
                if (kv.v.insert_blkno != res_blkno) {
                    kv.v.insert_blkno = res_blkno;
                    return true;
                }
                return false;
            });
            return true;    /* returned bool not used */
        });
    }
}

void TokenIndex::insert(const SparseElement &sd, uint64 doc_id, const void *il_meta)
{
    BlockNumber start_blkno, insert_blkno;
    uint32 ndoc;
    bool updated = false;
    bool rewrite;
retry:
    rewrite = false;
retry2:
    bool res = helper2(sd, [&](const auto &d, auto &store) -> bool {
        return store.try_emplace_or_visit(d, [&](auto &kv) -> bool {
            if (!updated) {
                ndoc = ++kv.v.stats.ndoc;
                if (half_to_float_unsigned(kv.v.stats.max_score) < half_to_float_unsigned(sd.v)) {
                    kv.v.stats.max_score = sd.v;
                }
            }
            if (rewrite) {
                kv.v.start_blkno = start_blkno;
                kv.v.insert_blkno = insert_blkno;
            } else {
                start_blkno = kv.v.start_blkno;
                insert_blkno = kv.v.insert_blkno;
            }
            return updated || !rewrite;
        }, _rel, il_meta, doc_id, sd.v, need_wal());
    });
    if (res) {  /* emplace constructor called, no need to call further insertion process */
        return;
    }
    updated = true;
    InvertedList il(_rel, start_blkno, insert_blkno, (const InvertedListMeta)il_meta, _need_wal);
    if (il.upon_threshold(ndoc)) {
        if (!il.try_upgrade(ndoc, start_blkno, insert_blkno, (const InvertedListMeta)il_meta)) {
            il.destroy();
            CHECK_FOR_INTERRUPTS();
            goto retry;
        }
        il.destroy();
        rewrite = true;
        goto retry2;
    }

    BlockNumber res_blkno = il.insert<true>(doc_id, sd.v);
    il.destroy();
    if (!BlockNumberIsValid(res_blkno)) {
        CHECK_FOR_INTERRUPTS();
        goto retry;
    }

    if (insert_blkno != res_blkno) {    /* only set insert_blkno if needed */
        helper2(sd, [res_blkno](const auto &d, auto &store) -> bool {
            store.visit(d, [res_blkno](auto &kv) -> bool {
                if (kv.v.insert_blkno != res_blkno) {
                    kv.v.insert_blkno = res_blkno;
                    return true;
                }
                return false;
            });
            return true;    /* returned bool not used */
        });
    }
}

void TokenIndex::vacuum(doc_id_track &id_track, IndexBulkDeleteResult *stats, const void *meta)
{
    const auto is_sparse_vector = [&](int attrno) -> bool {
        return ((const BM25MetaPage)meta)->is_sparse_vector & 1 << attrno;
    };
    const InvertedListMeta il_meta = &((const BM25MetaPage)meta)->il_meta;

    char index_name[NAMEDATALEN + 1];
    char part_name[NAMEDATALEN + 1];
    populate_index_partition_name(_rel, index_name, part_name);
    constexpr size_t nlist_per_report = 10'000ul;
    ann_helper::Timer timer(0, nlist_per_report, index_name, part_name);
    TokenPool pool;
    struct SpResult {
        uint32 nremoved;
        uint16 max_score;

        SpResult(uint32 _nremoved, uint16 _max_score)
            : nremoved(_nremoved),
              max_score(_max_score) {}
    };
    UnorderedMap<CharString,
                 uint32,
                 impl::DefaultHasher<CharString>,
                 impl::DefaultEqual<CharString>,
                 CONTEXT_ALLOCATOR<Pair<CharString, uint32>>> vacuum_result;
    UnorderedMap<uint32,
                 SpResult,
                 impl::DefaultHasher<uint32>,
                 impl::DefaultEqual<uint32>,
                 CONTEXT_ALLOCATOR<Pair<uint32, SpResult>>> vacuum_dim_result;
    const auto reset_container = [&pool, &vacuum_result, &timer]() -> void {
        pool.reset();
        vacuum_result.clear();
        timer.reset_step(nlist_per_report);
        timer.set_nloop(0);
    };
    const auto reset_dim_container = [&vacuum_dim_result, &timer]() -> void {
        vacuum_dim_result.clear();
        timer.reset_step(nlist_per_report);
        timer.set_nloop(0);
    };

    struct ILKey {
        Relation rel;
        Relation parent;
        Partition part;
    };
    const auto get_il =
        [&](ILKey &k, BlockNumber start_blkno, BlockNumber insert_blkno) -> InvertedList {
        if (!IsBgWorkerProcess()) {
            k.rel = _rel;
        } else if (RelationIsPartition(_rel)) {
            Oid parent_id = GetBaseRelOidOfParition(_rel);
            k.parent = index_open(parent_id, NoLock);
            k.part = partitionOpen(k.parent, RelationGetRelid(_rel), NoLock);
            k.rel = partitionGetRelation(k.parent, k.part);
        } else {
            k.rel = index_open(RelationGetRelid(_rel), NoLock);
        }
        return InvertedList(k.rel, start_blkno, insert_blkno, il_meta, need_wal());
    };
    const auto remove_il = [&](ILKey &&k, InvertedList &&il) -> void {
        il.destroy();
        if (!IsBgWorkerProcess()) {
            /* do nothing */
        } else if (RelationIsPartition(_rel)) {
            releaseDummyRelation(&k.rel);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
            partitionClose(k.parent, k.part, NoLock);
            index_close(k.parent, NoLock);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized, gcc false reporting */
        } else {
            index_close(k.rel, NoLock);
        }
    };

    slock_t mutex;
    SpinLockInit(&mutex);
    const auto do_vacuum_list_tok = [&](const char *str, const TokenIndexEntry &entry) {
        ILKey k;
        InvertedList il = get_il(k, entry.start_blkno, entry.insert_blkno);
        uint16 unused;
        uint32 s = il.vacuum<false>(id_track, stats, unused);
        remove_il(std::move(k), std::move(il));
        if (s > 0) {
            SpinLockAcquire(&mutex);
            /* no need to check uniqueness, as str is the key to the outer hashtable */
            vacuum_result.emplace(pool.get_token(str), s);
            SpinLockRelease(&mutex);
        }
        timer.inc_loop_count_forground_report("Vacuum token inverted lists");
    };
    const auto do_vacuum_list_sparse = [&](uint32 dim, const SparseDimIndexEntry &entry) {
        ILKey k;
        InvertedList il = get_il(k, entry.start_blkno, entry.insert_blkno);
        uint16 max_freq;
        uint32 s = il.vacuum<true>(id_track, stats, max_freq);
        remove_il(std::move(k), std::move(il));
        SpinLockAcquire(&mutex);
        vacuum_dim_result.emplace(dim, s, max_freq);
        SpinLockRelease(&mutex);
        timer.inc_loop_count_forground_report("Vacuum sparsevector inverted lists");
    };

    INIT_TASK_RUNNER();
    uint32 parallel_workers = get_bm25_parallel_workers(_rel);
    if (parallel_workers > 0) {
        LAUNCH_CONSUMER(parallel_workers);
        pg_usleep(1'000l * parallel_workers);    /* wait for bgworkers to launch */
    }
    START_TASK_POOL();
    const auto do_vacuum_tok = [&](const auto &kv) {
        RUN_TASK(do_vacuum_list_tok, kv.k.tok, kv.v);
    };
    const auto do_vacuum_sparse = [&](const auto &kv) {
        RUN_TASK(do_vacuum_list_sparse, kv.k, kv.v);
    };
    const auto clear_tok = [&](auto &kv, bool &modified) -> bool {
        auto it = vacuum_result.cfind(kv.k.tok);
        if (it == vacuum_result.cend() || it->second == 0) {
            modified = false;
            return false;
        }
        modified = true;
        kv.v.stats.ndoc -= it->second;
        timer.report_loop("Clean up stats");
        /* not implemented */
        // InvertedList il(_rel, kv.v.start_blkno, kv.v.insert_blkno, il_meta, need_wal());
        // if (il.below_threshold(kv.v.stats.ndoc)) {
        //     il.try_downgrade(kv.v.stats.ndoc, kv.v.start_blkno, kv.v.insert_blkno, il_meta);
        // }
        // il.destroy();
        return kv.v.stats.ndoc == 0;
    };
    const auto clear_sparse = [&](auto &kv, bool &modified) -> bool {
        auto it = vacuum_dim_result.cfind(kv.k);
        if (it == vacuum_dim_result.cend() || it->second.nremoved == 0) {
            modified = false;
            return false;
        }
        modified = true;
        kv.v.stats.ndoc -= it->second.nremoved;
        kv.v.stats.max_score = it->second.max_score;
        timer.report_loop("Clean up stats");
        /* not implemented */
        // InvertedList il(_rel, kv.v.start_blkno, kv.v.insert_blkno, il_meta, need_wal());
        // if (il.below_threshold(kv.v.stats.ndoc)) {
        //     il.try_downgrade(kv.v.stats.ndoc, kv.v.start_blkno, kv.v.insert_blkno, il_meta);
        // }
        // il.destroy();
        return kv.v.stats.ndoc == 0;
    };

    int nattr = RelationGetDescr(_rel)->natts;
    for (int i = 0; i < nattr; ++i) {
        if (is_sparse_vector(i)) {
            timer.report("Start to vacuum sparse vector attrbite %d", i + 1);
            const auto do_task = [&](BlockNumber blkno) -> void {
                dim_store store(_rel, blkno, true);
                store.cvisit(do_vacuum_sparse);
                WAIT_TASK();
                timer.set_nloop(timer._nloop_count);
                timer.reset_step(nlist_per_report);
                timer.report("Start to clean up stats");
                store.erase_if(clear_sparse);
                store.destroy();
                reset_dim_container();
            };
            timer.report("Start to vacuum positive values");
            do_task(_meta.attr_meta[i].short_store_blkno);
            timer.report("Start to vacuum negative values");
            do_task(_meta.attr_meta[i].mid_store_blkno);
        } else {
            timer.report("Start to vacuum text attrbite %d", i + 1);
            const auto do_task = [&](auto &&store) -> void {
                store.cvisit(do_vacuum_tok);
                WAIT_TASK();
                timer.set_nloop(timer._nloop_count);
                timer.reset_step(nlist_per_report);
                timer.report("Start to clean up stats");
                store.erase_if(clear_tok);
                store.destroy();
                reset_container();
            };
            timer.report("Start to vacuum short tokens");
            do_task(short_store(_rel, _meta.attr_meta[i].short_store_blkno, true));
            timer.report("Start to vacuum mid tokens");
            do_task(mid_store(_rel, _meta.attr_meta[i].mid_store_blkno, true));
            timer.report("Start to vacuum long tokens");
            do_task(long_store(_rel, _meta.attr_meta[i].long_store_blkno, true));
            timer.report("Start to vacuum full-length tokens");
            do_task(full_store(_rel, _meta.attr_meta[i].full_store_blkno, true));
        }
    }
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();
    SpinLockFree(&mutex);
    pool.destroy();
    timer.destroy();
    ann_helper::optional_destroy(vacuum_result);
    ann_helper::optional_destroy(vacuum_dim_result);
}

void TokenIndex::verify(const void *meta)
{
    const auto is_sparse_vector = [&](int attrno) -> bool {
        return ((const BM25MetaPage)meta)->is_sparse_vector & 1 << attrno;
    };
    const InvertedListMeta il_meta = &((const BM25MetaPage)meta)->il_meta;

    struct ILKey {
        Relation rel;
        Relation parent;
        Partition part;
    };
    const auto get_il =
        [&](ILKey &k, BlockNumber start_blkno, BlockNumber insert_blkno) -> InvertedList {
        if (!IsBgWorkerProcess()) {
            k.rel = _rel;
        } else if (RelationIsPartition(_rel)) {
            Oid parent_id = GetBaseRelOidOfParition(_rel);
            k.parent = index_open(parent_id, NoLock);
            k.part = partitionOpen(k.parent, RelationGetRelid(_rel), NoLock);
            k.rel = partitionGetRelation(k.parent, k.part);
        } else {
            k.rel = index_open(RelationGetRelid(_rel), NoLock);
        }
        return InvertedList(k.rel, start_blkno, insert_blkno, il_meta, need_wal());
    };
    const auto remove_il = [&](ILKey &&k, InvertedList &&il) -> void {
        il.destroy();
        if (!IsBgWorkerProcess()) {
            /* do nothing */
        } else if (RelationIsPartition(_rel)) {
            releaseDummyRelation(&k.rel);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
            partitionClose(k.parent, k.part, NoLock);
            index_close(k.parent, NoLock);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized, gcc false reporting */
        } else {
            index_close(k.rel, NoLock);
        }
    };

    const auto do_verify_tok = [&](const TokenIndexEntry &entry) {
        ILKey k;
        InvertedList il = get_il(k, entry.start_blkno, entry.insert_blkno);
        il.verify<false>(entry.stats);
        remove_il(std::move(k), std::move(il));
    };
    const auto do_verify_sparse = [&](const SparseDimIndexEntry entry) {
        ILKey k;
        InvertedList il = get_il(k, entry.start_blkno, entry.insert_blkno);
        il.verify<true>(entry.stats);
        remove_il(std::move(k), std::move(il));
    };

    INIT_TASK_RUNNER();
    uint32 parallel_workers = get_bm25_parallel_workers(_rel);
    LAUNCH_CONSUMER(parallel_workers);
    START_TASK_POOL();
    int nattr = RelationGetDescr(_rel)->natts;
    for (int i = 0; i < nattr; ++i) {
        if (is_sparse_vector(i)) {
            const auto do_task = [&](BlockNumber blkno) -> void {
                dim_store store(_rel, blkno, true);
                store.cvisit([&](const auto &kv) { RUN_TASK(do_verify_sparse, kv.v); });
                WAIT_TASK();
                store.destroy();
            };
            do_task(_meta.attr_meta[i].short_store_blkno);
            do_task(_meta.attr_meta[i].mid_store_blkno);
        } else {
            const auto do_task = [&](auto &&store) -> void {
                store.cvisit([&](const auto &kv) { do_verify_tok(kv.v); });
                WAIT_TASK();
                store.destroy();
            };
            do_task(short_store(_rel, _meta.attr_meta[i].short_store_blkno, true));
            do_task(mid_store(_rel, _meta.attr_meta[i].mid_store_blkno, true));
            do_task(long_store(_rel, _meta.attr_meta[i].long_store_blkno, true));
            do_task(full_store(_rel, _meta.attr_meta[i].full_store_blkno, true));
        }
    }
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();
}
