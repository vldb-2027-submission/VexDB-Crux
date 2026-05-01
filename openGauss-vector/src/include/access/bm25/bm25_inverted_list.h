/**
 * Copyright ...
 * BM25 inverted list.
 */

#ifndef BM25_INVERTED_LIST_H
#define BM25_INVERTED_LIST_H

#include "c.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "knl/knl_session.h"
#include "storage/buf/block.h"
#include "access/bm25/bm25_struct.h"
#include "access/bm25/bm25_utils.h"
#include "access/annvector/module/leak_checker.h"
#include "access/genam.h"

namespace bm25 {
struct InvertedListPointer {
    uint16 max_freq;
    BlockNumber blkno;
    uint64 next_doc_id;

    void init()
    {
        max_freq = 0;
        blkno = InvalidBlockNumber;
        next_doc_id = 0;
    }
};

struct InvertedListSkipPointers {
    static constexpr uint32 nlevel = 2u;
    static constexpr uint32 slevel_len = 5u;
    InvertedListPointer pointers[nlevel];
    BlockNumber prev_blkno;
    uint32 order;
};

constexpr uint16 il_threshold_levels[] = {4u, 32u, 162u};
constexpr uint16 n_il_level = sizeof(il_threshold_levels) / sizeof(il_threshold_levels[0]);
static_assert(n_il_level > 0, "n_il_level must non-zero");

struct InvertedListMetaData {
    BlockNumber vec_blknos[n_il_level];
    BlockNumber free_space_blknos[n_il_level];
    void init(Relation rel);
};
using InvertedListMeta = InvertedListMetaData *;
#define GetInvertedListMeta(page) ((InvertedListMeta)((char *)(page) + SizeOfPageHeaderData))

struct InvertedListPageData {
    constexpr static uint16 DELETE_FLAG = 0x01u;
    uint32 magic;
    uint16 version;
    uint16 nentry;
    InvertedListSkipPointers skip_pointer;
    InvertedListEntry entries[FLEXIBLE_ARRAY_MEMBER];
    void init();
    bool full() const;
    const InvertedListPointer &get_pointer(uint32 i) const { return skip_pointer.pointers[i]; }
    InvertedListPointer &get_pointer(uint32 i) { return skip_pointer.pointers[i]; }
    bool has_next_page() const { return BlockNumberIsValid(get_pointer(0).blkno); }
    bool has_next_lpage() const { return BlockNumberIsValid(get_pointer(1).blkno); }
};
using InvertedListPage = InvertedListPageData *;
#define GetInvertedListPage(page) ((InvertedListPage)((char *)(page) + SizeOfPageHeaderData))
struct InvertedListPageMacro {
private:
    static constexpr uint32 page_offset = MAXALIGN(SizeOfPageHeaderData);
public:
    static constexpr uint32 nentry_offset = offsetof(InvertedListPageData, nentry);
    static constexpr uint32 pointer_offset =
        offsetof(InvertedListPageData, skip_pointer) + offsetof(InvertedListSkipPointers, pointers);
    static constexpr uint32 pre_offset = offsetof(InvertedListPageData, skip_pointer) +
        offsetof(InvertedListSkipPointers, prev_blkno);
    static constexpr uint32 entry_offset = offsetof(InvertedListPageData, entries);
    static constexpr uint32 nth_entry_offset(uint16 n)
        { return entry_offset + sizeof(InvertedListEntry) * n; }
    static constexpr uint32 page_nentry_offset = page_offset + nentry_offset;
    static constexpr uint32 page_pointer_offset = page_offset + pointer_offset;
    static constexpr uint32 page_pre_offset = page_offset + pre_offset;
    static constexpr uint32 page_entry_offset = page_offset + entry_offset;
    static constexpr uint32 page_nth_entry_offset(uint16 n)
        { return page_offset + nth_entry_offset(n); }
};
constexpr uint16 max_il_page_nentry =
    (BLCKSZ - InvertedListPageMacro::page_entry_offset) / sizeof(InvertedListEntry);

class InvertedList : public ann_helper::LeakChecker {
public:
    InvertedList(Relation rel, BlockNumber start_blkno, BlockNumber insert_blkno,
                 const InvertedListMeta meta, bool need_wal);
    InvertedList(InvertedList &&) = default;
    InvertedList &operator=(const InvertedList &) = default;
    InvertedList &operator=(InvertedList &&) = default;
    void destroy();
    void reset();
    void swap(InvertedList &other);

    template <bool is_sparse>
    BlockNumber insert(uint64 doc_id, uint16 freq);
    const InvertedListEntry *get_doc_ids(uint16 &n) const;
    const InvertedListEntry *get_doc_ids() const;
    bool has_next() const;
    uint16 next_max_freq() const;
    uint64 next_doc_id() const;
    bool has_nextl_info() const;
    uint16 nextl_max_freq() const;
    uint64 nextl_doc_id() const;
    bool iter_next();
    void iter_nextl();
    union stats_type {
        SparseDimStats sds;
        TokenStats ts;

        stats_type(SparseDimStats s) : sds(s) {}
        stats_type(TokenStats s) : ts(s) {}
    };
    template <bool is_sparse>
    void verify(stats_type s) const;
    template <bool is_sparse>
    void fix_stats_pages();

    template <bool is_sparse>
    size_t vacuum(doc_id_track &id_track, IndexBulkDeleteResult *stats, uint16 &max_freq);
    bool try_downgrade(uint32 current_ndoc, BlockNumber &start_blkno, BlockNumber &insert_blkno,
                       const InvertedListMeta meta);
    bool try_upgrade(uint32 current_ndoc, BlockNumber &start_blkno, BlockNumber &insert_blkno,
                     const InvertedListMeta meta);
    bool upon_threshold(uint32 current_ndoc) const;
    bool below_threshold(uint32 current_ndoc) const;

private:
    struct long_store {
        BlockNumber start_blkno;
        BlockNumber insert_blkno;
        BlockNumber cur_blkno{InvalidBlockNumber};
        Buffer cur_buf{InvalidBuffer};
        long_store(BlockNumber s, BlockNumber i) : start_blkno(s), insert_blkno(i) {}
    };
    struct short_store {
        uint8 version;
        uint8 level;
        uint32 offset;
        BlockNumber vec_blkno;
        InvertedListEntry *ptr{NULL};
        short_store(BlockNumber s, BlockNumber i, const InvertedListMeta meta);
    };
    Relation _rel;
    bool _is_short;
    bool _need_wal;
    union Store {
        long_store _long;
        short_store _short;
        Store(BlockNumber s, BlockNumber i, const InvertedListMeta meta);
    } _store;

    template <uint16 l>
    BlockNumber insert_helper(uint64 doc_id, uint16 freq);
    template <uint16 l>
    bool upgrade_helper(BlockNumber &start_blkno, BlockNumber &insert_blkno,
                        const InvertedListMeta meta);
    template <uint16 l, bool is_sparse>
    size_t vacuum_helper(doc_id_track &id_track, IndexBulkDeleteResult *stats, uint16 &max_freq);
    template <uint16 l, bool is_sparse>
    void verify_helper(const stats_type &s) const;
    template <bool is_sparse, bool force_doc_id_set>
    void set_lprev_pointer(uint16 target_freq, uint64 doc_id, uint32 order,
                           BlockNumber prev_blkno) const;
};
}; /* namespace bm25 */

#endif /* BM25_INVERTED_LIST_H */
