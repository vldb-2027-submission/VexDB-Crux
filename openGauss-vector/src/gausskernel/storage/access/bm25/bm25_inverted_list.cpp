/**
 * Copyright ...
 */

#include <algorithm>    /* upper_bound */
#include <boost/preprocessor/repetition/repeat.hpp>

#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/freespace.hpp>
#include <vtl/expr_helper>
#include <vtl/pair>
#include <vtl/tuple>

#include "access/bm25/bm25.h"
#include "access/bm25/bm25_inverted_list.h"
#include "access/annvector/halfutils.h"
#include "storage/indexfsm.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"

#define TestAssert(expr)                                                            \
    do {                                                                            \
        if (!(expr))                                                                \
            elog(ERROR, "Assertion failed: %s (%s:%d)", #expr, __FILE__, __LINE__); \
    } while (0)

using namespace bm25;
using namespace disk_container;

constexpr uint8 short_il_offset_version_len = 3u;
constexpr uint8 short_il_offset_version_mod = 1u << short_il_offset_version_len;
constexpr uint32 short_il_offset_shift = 32u - short_il_offset_version_len;
constexpr uint32 short_il_offset_mask =
    ((1u << short_il_offset_version_len) - 1u) << short_il_offset_shift;
constexpr uint32 short_il_offset_mask_inv = ~short_il_offset_mask;
static uint8 get_version(uint32 offset)
    { return (offset & short_il_offset_mask) >> short_il_offset_shift; }
static void inc_version(uint32 &offset)
{
    uint8 version = get_version(offset);
    version = (version + 1u) % short_il_offset_version_mod;
    offset = (offset & short_il_offset_mask_inv) | (version << short_il_offset_shift);
}
static void set_version(uint32 &offset, uint8 version)
{
    if (version >= short_il_offset_version_mod) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid version %u for inverted list offset.", version)));
    }
    offset = (offset & short_il_offset_mask_inv) | (version << short_il_offset_shift);
}
static size_t get_offset(uint32 offset) { return offset & short_il_offset_mask_inv; }

InvertedList::short_store::short_store(BlockNumber s, BlockNumber i, const InvertedListMeta meta)
    : version(get_version(i)),
      level(s),
      offset(get_offset(i)),
      vec_blkno(meta->vec_blknos[level]) {}

InvertedList::Store::Store(BlockNumber s, BlockNumber i, const InvertedListMeta meta)
{
    if (s < n_il_level) {
        new (&_short) short_store(s, i, meta);
    } else {
        new (&_long) long_store(s, i);
    }
}

InvertedList::InvertedList(Relation rel, BlockNumber start_blkno, BlockNumber insert_blkno,
                           const InvertedListMeta meta, bool need_wal)
    : _rel(rel),
      _is_short(start_blkno < n_il_level),
      _need_wal(need_wal),
      _store(start_blkno, insert_blkno, meta) {}

TokenIndexEntry::TokenIndexEntry(Relation rel, const void *il_meta, uint64 doc_id, uint16 freq,
                                 bool need_wal)
{
    InvertedListMetaData &meta = *(InvertedListMeta)il_meta;
    DiskVector<FixedInvertedList<il_threshold_levels[0]>> vec(rel, meta.vec_blknos[0], need_wal);
    uint32 offset;
    FreeSpace<uint32> free_space(rel, meta.free_space_blknos[0], need_wal);
    FixedInvertedList<il_threshold_levels[0]> il_entry;
    il_entry.entries[0].doc_id = doc_id;
    for (uint16 i = 1; i < il_threshold_levels[0]; ++i) {
        il_entry.entries[i].doc_id = 0;
    }
    il_entry.entries[0].freq = uint16(freq);
    if (!free_space.pop(offset)) {
        il_entry.version = 0;
        offset = vec.push_back(il_entry);
        if (short_il_offset_mask & offset) {
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                            errmsg("Inverted list too long."),
                            errhint("Please vacuum or rebuild the index.")));
        }
    } else {
        inc_version(offset);
        il_entry.version = get_version(offset);
        if (need_wal) {
            vec.template set<AccessorLockType::WriteLock>(get_offset(offset), il_entry);
        } else {
            vec.template set<AccessorLockType::NoLockWrite>(get_offset(offset), il_entry);
        }
    }
    free_space.destroy();
    vec.destroy();
    insert_blkno = offset;
    start_blkno = 0;
    stats.ndoc = 1u;
}

SparseDimIndexEntry::SparseDimIndexEntry(Relation rel, const void *il_meta, uint64 doc_id,
    uint16 score, bool need_wal)
{
    InvertedListMetaData &meta = *(InvertedListMeta)il_meta;
    DiskVector<FixedInvertedList<il_threshold_levels[0]>> vec(rel, meta.vec_blknos[0], need_wal);
    uint32 offset;
    FreeSpace<uint32> free_space(rel, meta.free_space_blknos[0], need_wal);
    FixedInvertedList<il_threshold_levels[0]> il_entry;
    il_entry.entries[0].doc_id = doc_id;
    for (uint16 i = 1; i < il_threshold_levels[0]; ++i) {
        il_entry.entries[i].doc_id = 0;
    }
    il_entry.entries[0].freq = score;
    if (!free_space.pop(offset)) {
        il_entry.version = 0;
        offset = vec.push_back(il_entry);
        if (short_il_offset_mask & offset) {
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                            errmsg("Inverted list too long."),
                            errhint("Please vacuum or rebuild the index.")));
        }
    } else {
        inc_version(offset);
        il_entry.version = get_version(offset);
        if (need_wal) {
            vec.template set<AccessorLockType::WriteLock>(get_offset(offset), il_entry);
        } else {
            vec.template set<AccessorLockType::NoLockWrite>(get_offset(offset), il_entry);
        }
    }
    free_space.destroy();
    vec.destroy();
    insert_blkno = offset;
    start_blkno = 0;
    stats.ndoc = 1u;
    stats.max_score = score;
}

void InvertedListMetaData::init(Relation rel)
{
#define INIT_BLKNOS(z, i, x)    \
    vec_blknos[i] = DiskVector<FixedInvertedList<il_threshold_levels[i]>>::get_disk_vector(rel, false); \
    free_space_blknos[i] = FreeSpace<uint32>::get_freespace_meta(rel, false);

    static_assert(n_il_level == 3u, "n_il_level must be 3");
    BOOST_PP_REPEAT(3, INIT_BLKNOS, x);
#undef INIT_BLKNOS
}

void InvertedListPageData::init()
{
    magic = BM25_INVERTED_LIST_MAGIC;
    version = BM25_INVERTED_LIST_VERSION;
    nentry = 0;
    skip_pointer.prev_blkno = InvalidBlockNumber;
    for (uint32 i = 0; i < InvertedListSkipPointers::nlevel; ++i) {
        get_pointer(i).init();
    }
}

bool InvertedListPageData::full() const { return nentry == max_il_page_nentry; }

static tuple<BlockNumber, Buffer, bool> get_new_page(Relation rel)
{
    BlockNumber blkno = GetFreeIndexPage(rel);
    Buffer buf;
    bool page_is_new;
    if (BlockNumberIsValid(blkno)) {
        buf = ReadBuffer(rel, blkno);
        page_is_new = false;
    } else {
        LockRelationForExtension(rel, ExclusiveLock);
        buf = ReadBuffer(rel, P_NEW);
        UnlockRelationForExtension(rel, ExclusiveLock);
        blkno = BufferGetBlockNumber(buf);
        page_is_new = true;
    }
    return {blkno, buf, page_is_new};
}

void InvertedList::destroy()
{
    ann_helper::LeakChecker::destroy();
    if (!_is_short && BufferIsValid(_store._long.cur_buf)) {
        UnlockReleaseBuffer(_store._long.cur_buf);
        _store._long.cur_buf = InvalidBuffer;
    }
    if (_is_short) {
        pfree_ext(_store._short.ptr);
    }
}

void InvertedList::reset()
{
    if (_is_short) {
        return;
    }
    if (BufferIsValid(_store._long.cur_buf)) {
        UnlockReleaseBuffer(_store._long.cur_buf);
        _store._long.cur_buf = InvalidBuffer;
    }
    _store._long.cur_blkno = InvalidBlockNumber;
}

void InvertedList::swap(InvertedList &other)
{
    if (this == &other) {
        return;
    }
    std::swap(_rel, other._rel);
    std::swap(_store, other._store);
    std::swap(_is_short, other._is_short);
    std::swap(_need_wal, other._need_wal);
}

template <uint16 l>
BlockNumber InvertedList::insert_helper(uint64 doc_id, uint16 freq)
{
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    DiskVector<IL_TYPE> vec(_rel, _store._short.vec_blkno, _need_wal);
    bool inserted = vec.template apply<AccessorLockType::WriteLock>([&](IL_TYPE &entry) -> bool {
        if (_store._short.version != entry.version) {
            return false;
        }
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            if (entry.entries[i].doc_id == 0) {
                entry.entries[i].doc_id = doc_id;
                entry.entries[i].freq = freq;
                return true;
            } else if (entry.entries[i].doc_id > doc_id) {
                if (entry.entries[IL_TYPE::n - 1u].doc_id != 0) {
                    return false;
                }
                uint16 nmoved = IL_TYPE::n - i - 1u;
                while (nmoved > 0 && entry.entries[i + nmoved - 1u].doc_id == 0) {
                    --nmoved;
                }
                if (nmoved > 0) {
                    errno_t rc = memmove_s(
                        entry.entries + i + 1u, sizeof(InvertedListEntry) * nmoved,
                        entry.entries + i, sizeof(InvertedListEntry) * nmoved);
                    securec_check_c(rc, "\0", "\0");
                }
                entry.entries[i].doc_id = doc_id;
                entry.entries[i].freq = freq;
                return true;
            }
            Assert(entry.entries[i].doc_id != doc_id);
        }
        return false;
    })(_store._short.offset);
    vec.destroy();
    if (!inserted) {
        /* we don't do upgrade if failed since we cannot handle concurrency here */
        return InvalidBlockNumber;
    }
    uint32 offset = _store._short.offset;
    set_version(offset, _store._short.version);
    return offset;
}

/* only set lprev with greater freq, we do allow false-positive but not in other way */
template <bool is_sparse, bool force_doc_id_set>
void InvertedList::set_lprev_pointer(uint16 target_freq, uint64 doc_id, uint32 order,
                                     BlockNumber prev_blkno) const
{
    if (!BlockNumberIsValid(prev_blkno) || order % InvertedListSkipPointers::slevel_len == 0) {
        return;
    }

    using freq_type = typename std::conditional<is_sparse, float, uint16>::type;
    static constexpr auto to_score = [](uint16 freq) -> freq_type {
        CONSTEXPR_IF (is_sparse) {
            return half_to_float_unsigned(freq);
        } else {
            return freq;
        }
    };
    const freq_type max_score = to_score(target_freq);

    uint32 step = order % InvertedListSkipPointers::slevel_len;
    doc_id = (force_doc_id_set || step == InvertedListSkipPointers::slevel_len - 1u) ? doc_id : 0;
    Buffer buf;
    InvertedListPage il_page;
    for (uint32 i = 1u;; ++i) {
        buf = ReadBuffer(_rel, prev_blkno);
        il_page = GetInvertedListPage(BufferGetPage(buf));
        if (i == step) {
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            break;
        }
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        CONSTEXPR_IF (!force_doc_id_set) {
            if (doc_id == 0 && max_score <= to_score(il_page->get_pointer(0).max_freq)) {
                UnlockReleaseBuffer(buf);
                return;
            }
        }
        prev_blkno = il_page->skip_pointer.prev_blkno;
        Assert(BlockNumberIsValid(prev_blkno));
        UnlockReleaseBuffer(buf);
    }
    if (!force_doc_id_set && !il_page->has_next_lpage() && doc_id == 0) {
        UnlockReleaseBuffer(buf);
        return;
    }

    bool pointer_set = false;
    if ((force_doc_id_set || doc_id != 0) && il_page->get_pointer(1).next_doc_id > doc_id) {
        il_page->get_pointer(1).next_doc_id = doc_id;
        pointer_set = true;
    }
    if (to_score(il_page->get_pointer(1).max_freq) < max_score) {
        il_page->get_pointer(1).max_freq = target_freq;
        pointer_set = true;
    }
    if (pointer_set) {
        MarkBufferDirty(buf);
        if (_need_wal) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_pointer_offset + sizeof(InvertedListPointer),
                sizeof(InvertedListPointer)
            });
        }
    }
    UnlockReleaseBuffer(buf);
}

/**
 * BUG: it still cannot pass verify() test when build with parallel.
 *      The long skip pointer doc_id is not set properly, but false-positive does not affect search.
 */
template <bool is_sparse>
BlockNumber InvertedList::insert(uint64 doc_id, uint16 freq)
{
    if (_is_short) {
        static_assert(n_il_level == 3u, "n_il_level must be 3");
        switch (_store._short.level) {
            case 0: return insert_helper<0>(doc_id, freq);
            case 1u: return insert_helper<1u>(doc_id, freq);
            case 2u: return insert_helper<2u>(doc_id, freq);
            default:
                __builtin_unreachable();
        }
    }

    using freq_type = typename std::conditional<is_sparse, float, uint16>::type;
    static constexpr auto to_score = [](uint16 freq) -> freq_type {
        CONSTEXPR_IF (is_sparse) {
            return half_to_float_unsigned(freq);
        } else {
            return freq;
        }
    };
    static constexpr auto to_freq = [](freq_type freq) -> uint16 {
        CONSTEXPR_IF (is_sparse) {
            return float_to_half(freq);
        } else {
            return freq;
        }
    };

    Assert(BlockNumberIsValid(_store._long.insert_blkno));
    Buffer buf = ReadBuffer(_rel, _store._long.insert_blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(buf));
    while ((il_page->nentry == 0 || il_page->entries[0].doc_id > doc_id) &&
           BufferGetBlockNumber(buf) != _store._long.start_blkno) {
        if (!BlockNumberIsValid(il_page->skip_pointer.prev_blkno)) {
            break;
        }
        _store._long.insert_blkno = il_page->skip_pointer.prev_blkno;
        UnlockReleaseBuffer(buf);
        buf = ReadBuffer(_rel, _store._long.insert_blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        /* prev_blkno is always valid as we do not recycle IL pages */
        il_page = GetInvertedListPage(BufferGetPage(buf));
    }
    while ((il_page->nentry == 0 && il_page->has_next_page()) ||
           (il_page->nentry >= max_il_page_nentry &&
            il_page->entries[max_il_page_nentry - 1u].doc_id < doc_id)) {
        if (il_page->has_next_page()) {
            _store._long.insert_blkno = il_page->get_pointer(0).blkno;
            Buffer temp_buf = ReadBuffer(_rel, _store._long.insert_blkno);
            LockBuffer(temp_buf, BUFFER_LOCK_EXCLUSIVE);
            UnlockReleaseBuffer(buf);
            buf = temp_buf;
            il_page = GetInvertedListPage(BufferGetPage(buf));
        } else {
            freq_type max_freq = 0;
            for (uint16 i = 0; i < max_il_page_nentry; ++i) {
                freq_type temp = to_score(il_page->entries[i].freq);
                max_freq = max_freq < temp ? temp : max_freq;
            }
            const uint32 cur_order = il_page->skip_pointer.order;
            BlockNumber prev_blkno = il_page->skip_pointer.prev_blkno;
            auto [new_blkno, new_buf, page_is_new] = get_new_page(_rel);
            LockBuffer(new_buf, BUFFER_LOCK_EXCLUSIVE);
            BlockNumber old_blkno = _store._long.insert_blkno;
            _store._long.insert_blkno = new_blkno;
            il_page->get_pointer(0) = {to_freq(max_freq), _store._long.insert_blkno, doc_id};
            MarkBufferDirty(buf);
            if (_need_wal) {
                Bm25XLogAddData(buf, BufferGetPage(buf), {
                    InvertedListPageMacro::page_pointer_offset,
                    sizeof(InvertedListPointer)
                });
            }
            UnlockReleaseBuffer(buf);
            buf = new_buf;
            Page page = BufferGetPage(buf);
            PageInit(page, BLCKSZ, 0);
            ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
            il_page = GetInvertedListPage(page);
            il_page->init();
            il_page->skip_pointer.order = cur_order + 1u;
            if (il_page->skip_pointer.order % InvertedListSkipPointers::slevel_len == 0) {
                for (uint32 i = 0;; ++i) {
                    const bool at_target = i == InvertedListSkipPointers::slevel_len - 2u;
                    Buffer old_buf = ReadBuffer(_rel, prev_blkno);
                    LockBuffer(old_buf, at_target ? BUFFER_LOCK_EXCLUSIVE : BUFFER_LOCK_SHARE);
                    auto *old_il_page = GetInvertedListPage(BufferGetPage(old_buf));
                    freq_type temp = to_score(old_il_page->get_pointer(0).max_freq);
                    if (max_freq < temp) {
                        max_freq = temp;
                    }
                    prev_blkno = old_il_page->skip_pointer.prev_blkno;
                    if (at_target) {
                        auto &ptr = old_il_page->get_pointer(1);
                        freq_type temp = to_score(ptr.max_freq);
                        if (max_freq > temp) {
                            ptr.max_freq = to_freq(max_freq);
                        }
                        ptr.blkno = _store._long.insert_blkno;
                        if (ptr.next_doc_id == 0 || ptr.next_doc_id > doc_id) {
                            ptr.next_doc_id = doc_id;
                        }
                        MarkBufferDirty(old_buf);
                        if (_need_wal) {
                            Bm25XLogAddData(old_buf, BufferGetPage(old_buf), {
                                InvertedListPageMacro::page_pointer_offset +
                                    sizeof(InvertedListPointer),
                                sizeof(InvertedListPointer)
                            });
                        }
                    }
                    UnlockReleaseBuffer(old_buf);
                    if (at_target) {
                        break;
                    }
                }
            }
            il_page->skip_pointer.prev_blkno = old_blkno;
            MarkBufferDirty(buf);
            if (_need_wal) {
                if (page_is_new) {
                    Bm25XLogAppendPage(buf, page);
                } else {
                    Bm25XLogInitPage(buf, page);
                }
            }
        }
    }
    InvertedListEntry *pos = std::upper_bound(il_page->entries, il_page->entries + il_page->nentry,
        doc_id, [](uint64 value, const InvertedListEntry &elem) -> bool {
            return value < elem.doc_id;
        });
    bool set_prev_docid = pos == il_page->entries && il_page->nentry > 0;
    bool pointer_set = false;
    bool pointer2_set = false;
    uint64 new_doc_id = 0;
    uint16 new_freq;
    if (il_page->nentry >= max_il_page_nentry) {
        new_doc_id = il_page->entries[max_il_page_nentry - 1u].doc_id;
        new_freq = il_page->entries[max_il_page_nentry - 1u].freq;
        if (il_page->get_pointer(0).next_doc_id > new_doc_id) {
            il_page->get_pointer(0).next_doc_id = new_doc_id;
            pointer_set = true;
        }
        freq_type temp_max = to_score(il_page->get_pointer(0).max_freq);
        if (temp_max <= to_score(new_freq)) {
            freq_type cur_max_freq = 0;
            for (uint16 i = 0; i < il_page->nentry - 1u; ++i) {
                freq_type temp = to_score(il_page->entries[i].freq);
                if (temp > cur_max_freq) {
                    cur_max_freq = temp;
                }
            }
            if (temp_max > cur_max_freq) {
                il_page->get_pointer(0).max_freq = to_freq(cur_max_freq);
                if (il_page->has_next_lpage() && to_score(il_page->get_pointer(1).max_freq) <
                                                 to_score(freq)) {
                    il_page->get_pointer(1).max_freq = freq;
                    pointer2_set = true;
                }
                pointer_set = true;
            }
        }

        size_t s = max_il_page_nentry - 1ul - (pos - il_page->entries);
        if (s > 0) {
            s *= sizeof(InvertedListEntry);
            errno_t rc = memmove_s(pos + 1, s, pos, s);
            securec_check_c(rc, "\0", "\0");
        }
        --il_page->nentry;
    } else if (pos != il_page->entries + il_page->nentry) {
        errno_t rc = memmove_s(pos + 1,
            sizeof(InvertedListEntry) * (max_il_page_nentry - 1u - (pos - il_page->entries)),
            pos, sizeof(InvertedListEntry) * (il_page->nentry - (pos - il_page->entries)));
        securec_check_c(rc, "\0", "\0");
    }
    new (pos) InvertedListEntry(doc_id, freq);
    ++il_page->nentry;
    if (il_page->has_next_page() && to_score(il_page->get_pointer(0).max_freq) < to_score(freq)) {
        il_page->get_pointer(0).max_freq = freq;
        if (il_page->has_next_lpage() && to_score(il_page->get_pointer(1).max_freq) <
                                         to_score(freq)) {
            il_page->get_pointer(1).max_freq = freq;
            pointer2_set = true;
        }
        pointer_set = true;
    }
    MarkBufferDirty(buf);
    if (_need_wal) {
        if (pointer_set) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_pointer_offset,
                sizeof(InvertedListPointer) + (pointer2_set ? sizeof(InvertedListPointer) : 0)
            });
        }
        if (il_page->nentry - (pos - il_page->entries) == 0) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_nentry_offset,
                sizeof(uint16)
            });
        } else {
            Bm25XLogInsertEntry(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_nth_entry_offset(pos - il_page->entries),
                sizeof(InvertedListEntry) * (il_page->nentry - (pos - il_page->entries)),
                il_page->nentry
            });
        }
    }
    if (new_doc_id != 0 && il_page->has_next_page()) {
        _store._long.insert_blkno = il_page->get_pointer(0).blkno;
    }
    uint16 target_freq = il_page->get_pointer(0).max_freq;
    uint32 order = il_page->skip_pointer.order;
    BlockNumber prev_blkno = il_page->skip_pointer.prev_blkno;
    uint64 new_id = il_page->get_pointer(0).next_doc_id;
    UnlockReleaseBuffer(buf);
    if (pointer_set) {
        set_lprev_pointer<is_sparse, false>(target_freq, new_id, order, prev_blkno);
    }
    if (set_prev_docid && BlockNumberIsValid(prev_blkno)) {
        buf = ReadBuffer(_rel, prev_blkno);
        il_page = GetInvertedListPage(BufferGetPage(buf));
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        if (il_page->get_pointer(0).next_doc_id <= doc_id) {
            UnlockReleaseBuffer(buf);
            goto my_break;
        }
        il_page->get_pointer(0).next_doc_id = doc_id;
        MarkBufferDirty(buf);
        if (_need_wal) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_pointer_offset,
                sizeof(InvertedListPointer)
            });
        }
        prev_blkno = il_page->skip_pointer.prev_blkno;
        order = il_page->skip_pointer.order;
        UnlockReleaseBuffer(buf);
        if ((order + 1u) % InvertedListSkipPointers::slevel_len == 0 && order > 0) {
            set_lprev_pointer<is_sparse, true>(0, doc_id, order, prev_blkno);
        }
    }
my_break:
    if (new_doc_id == 0) {
        return _store._long.insert_blkno;
    }
    return insert<is_sparse>(new_doc_id, new_freq);  /* hope it is tail recursion */
}

const InvertedListEntry *InvertedList::get_doc_ids() const
{
    if (_is_short) {
        return _store._short.ptr;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->entries;
}

const InvertedListEntry *InvertedList::get_doc_ids(uint16 &n) const
{
    if (_is_short) {
        const uint16 max_n = il_threshold_levels[_store._short.level];
        for (n = 0; n < max_n; ++n) {
            if (_store._short.ptr[n].doc_id == 0) {
                break;
            }
        }
        return _store._short.ptr;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    n = il_page->nentry;
    return il_page->entries;
}

bool InvertedList::has_next() const
{
    return !_is_short &&
        GetInvertedListPage(BufferGetPage(_store._long.cur_buf))->has_next_page();
}

uint16 InvertedList::next_max_freq() const
{
    if (_is_short) {
        return 0x7bffu; /* FLOAT16_MAX binary, which is large enough for freq too */
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->get_pointer(0).max_freq;
}

uint64 InvertedList::next_doc_id() const
{
    if (_is_short) {
        return 0;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->get_pointer(0).next_doc_id;
}

bool InvertedList::has_nextl_info() const
{
    if (_is_short) {
        return false;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->has_next_lpage();
}

uint16 InvertedList::nextl_max_freq() const
{
    if (_is_short) {
        return 0x7bffu;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->get_pointer(1).max_freq;
}

uint64 InvertedList::nextl_doc_id() const
{
    if (_is_short) {
        return 0;
    }
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    return il_page->get_pointer(1).next_doc_id;
}

template <uint16 l>
static void iter_load_ptr(Relation rel, BlockNumber v, uint8 version, uint32 offset,
                          InvertedListEntry *&out)
{
    Assert(out == NULL);
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    out = (InvertedListEntry *)palloc(sizeof(InvertedListEntry) * il_threshold_levels[l]);
    out[0].doc_id = 0;
    DiskVector<FixedInvertedList<il_threshold_levels[l]>> vec(rel, v, false);
    vec.template apply<AccessorLockType::ReadLock>([&](const IL_TYPE &entry) -> bool {
        if (entry.version == version) {
            for (uint16 i = 0; i < IL_TYPE::n; ++i) {
                out[i] = entry.entries[i];
                if (out[i].doc_id == 0) {
                    break;
                }
            }
        }
        return false;
    })(offset);
    if (out[0].doc_id == 0) {
        pfree(out);
        out = NULL;
    }
    vec.destroy();
}

bool InvertedList::iter_next()
{
    if (_is_short) {
        if (!_store._short.ptr) {
            static_assert(n_il_level == 3u, "n_il_level must be 3");
            const BlockNumber vec_blkno = _store._short.vec_blkno;
            const uint8 version = _store._short.version;
            const uint32 offset = _store._short.offset;
            switch (_store._short.level) {
                case 0:
                    iter_load_ptr<0>(_rel, vec_blkno, version, offset, _store._short.ptr);
                    break;
                case 1u:
                    iter_load_ptr<1u>(_rel, vec_blkno, version, offset, _store._short.ptr);
                    break;
                case 2u:
                    iter_load_ptr<2u>(_rel, vec_blkno, version, offset, _store._short.ptr);
                    break;
                default:
                    __builtin_unreachable();
            }
            return _store._short.ptr != NULL;
        }
        return false;
    }
    if (BufferIsValid(_store._long.cur_buf)) {
        InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
        _store._long.cur_blkno = il_page->get_pointer(0).blkno;
        UnlockReleaseBuffer(_store._long.cur_buf);
        _store._long.cur_buf = InvalidBuffer;
    } else {
        _store._long.cur_blkno = _store._long.start_blkno;
    }

    if (BlockNumberIsValid(_store._long.cur_blkno)) {
        Buffer new_buf = ReadBuffer(_rel, _store._long.cur_blkno);
        LockBuffer(new_buf, BUFFER_LOCK_SHARE);
        Assert(GetInvertedListPage(BufferGetPage(new_buf))->magic == BM25_INVERTED_LIST_MAGIC);
        _store._long.cur_buf = new_buf;
        return true;
    }
    return false;
}

void InvertedList::iter_nextl()
{
    Assert(!_is_short);
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(_store._long.cur_buf));
    _store._long.cur_blkno = il_page->get_pointer(1).blkno;
    Buffer new_buf = ReadBuffer(_rel, _store._long.cur_blkno);
    LockBuffer(new_buf, BUFFER_LOCK_SHARE);
    UnlockReleaseBuffer(_store._long.cur_buf);
    _store._long.cur_buf = new_buf;
}

template <uint16 l, bool is_sparse>
void InvertedList::verify_helper(const stats_type &s) const
{
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    DiskVector<IL_TYPE> vec(_rel, _store._short.vec_blkno, false);
    uint32 ndoc = 0;
    CONSTEXPR_IF (is_sparse) {
        float max_score = half_to_float_unsigned(s.sds.max_score);
        vec.template visit<AccessorLockType::NoLockUnsafe>([&](const IL_TYPE *e, size_t) -> void {
            const IL_TYPE &entry = *e;
            if (entry.version != _store._short.version) {
                return;
            }
            for (uint16 i = 0; i < IL_TYPE::n; ++i) {
                if (entry.entries[i].doc_id == 0) {
                    break;
                }
                if (i > 0) {
                    TestAssert(entry.entries[i].doc_id > entry.entries[i - 1u].doc_id);
                }
                ++ndoc;
                TestAssert(half_to_float_unsigned(entry.entries[i].freq) <= max_score);
            }
        })(_store._short.offset, 1ul);
    } else {
        vec.template visit<AccessorLockType::NoLockUnsafe>([&](const IL_TYPE *e, size_t) -> void {
            const IL_TYPE &entry = *e;
            if (entry.version != _store._short.version) {
                return;
            }
            for (uint16 i = 0; i < IL_TYPE::n; ++i) {
                if (entry.entries[i].doc_id == 0) {
                    break;
                }
                if (i > 0) {
                    TestAssert(entry.entries[i].doc_id > entry.entries[i - 1u].doc_id);
                }
                ++ndoc;
            }
        })(_store._short.offset, 1ul);
    }
    vec.destroy();
    CONSTEXPR_IF (is_sparse) {
        TestAssert(ndoc == s.sds.ndoc);
    } else {
        TestAssert(ndoc == s.ts.ndoc);
    }
}

template <bool is_sparse>
void InvertedList::verify(stats_type s) const
{
    if (_is_short) {
        static_assert(n_il_level == 3u, "n_il_level must be 3");
        switch (_store._short.level) {
            case 0:
                verify_helper<0, is_sparse>(s);
                break;
            case 1u:
                verify_helper<1u, is_sparse>(s);
                break;
            case 2u:
                verify_helper<2u, is_sparse>(s);
                break;
            default:
                __builtin_unreachable();
        }
        return;
    }

    using MAX_SCORE_TYPE = std::conditional_t<is_sparse, float, uint16>;
    uint32 ndoc = 0;
    BlockNumber cur_blkno = _store._long.start_blkno;
    static constexpr auto to_score = [](uint16 freq) -> MAX_SCORE_TYPE {
        CONSTEXPR_IF (is_sparse) {
            return half_to_float_unsigned(freq);
        } else {
            return freq;
        }
    };
    std::conditional_t<is_sparse, SparseDimStats, TokenStats> my_s = [&s]() {
        CONSTEXPR_IF (is_sparse) {
            return s.sds;
        } else {
            return s.ts;
        }
    }();

    MAX_SCORE_TYPE max_score;
    CONSTEXPR_IF (is_sparse) {
        max_score = to_score(my_s.max_score);
    } else {
        max_score = UINT16_MAX;
    }
    MAX_SCORE_TYPE last_max_score = HALF_MAX;
    MAX_SCORE_TYPE lastl_max_score = HALF_MAX;
    uint64 last_doc_id = 0;
    uint64 lastl_doc_id = 0;
    uint32 cur_order = 0;
    while (BlockNumberIsValid(cur_blkno)) {
        Buffer buf = ReadBuffer(_rel, cur_blkno);
        const InvertedListPage il_page = GetInvertedListPage(BufferGetPage(buf));
        TestAssert(cur_order == il_page->skip_pointer.order);
        ndoc += il_page->nentry;
        last_max_score = il_page->has_next_page() ?
            to_score(il_page->get_pointer(0).max_freq) : UINT16_MAX;
        if (il_page->skip_pointer.order % InvertedListSkipPointers::slevel_len == 0) {
            if (il_page->has_next_lpage()) {
                lastl_max_score = to_score(il_page->get_pointer(1).max_freq);
            } else {
                lastl_max_score = HALF_MAX;
            }
        }

        bool meet = false;
        for (uint16 i = 0; i < il_page->nentry; ++i) {
            MAX_SCORE_TYPE cur_score = to_score(il_page->entries[i].freq);
            TestAssert(cur_score <= max_score);
            TestAssert(cur_score <= last_max_score);
            TestAssert(cur_score <= lastl_max_score);
            if (cur_score >= last_max_score) {
                meet = true;
            }
            if (i > 0) {
                TestAssert(il_page->entries[i].doc_id > il_page->entries[i - 1u].doc_id);
            } else {
                if (cur_order > 0) {
                    TestAssert(il_page->entries[i].doc_id == last_doc_id);
                    if (cur_order % InvertedListSkipPointers::slevel_len == 0) {
                        /* loose condition to pass the test */
                        TestAssert(il_page->entries[i].doc_id + 100ul >= lastl_doc_id);
                        TestAssert(il_page->entries[i].doc_id <= lastl_doc_id);
                    }
                }
            }
        }
        last_doc_id = il_page->get_pointer(0).next_doc_id;
        TestAssert(meet || last_max_score >= HALF_MAX);
        if (il_page->skip_pointer.order % InvertedListSkipPointers::slevel_len == 0) {
            if (il_page->has_next_lpage()) {
                lastl_doc_id = il_page->get_pointer(1).next_doc_id;
            } else {
                lastl_doc_id = 0;
            }
        }

        cur_blkno = il_page->get_pointer(0).blkno;
        ReleaseBuffer(buf);
        ++cur_order;
    }
    TestAssert(ndoc == my_s.ndoc);
}

template <uint16 l, bool is_sparse>
size_t InvertedList::vacuum_helper(doc_id_track &id_track, IndexBulkDeleteResult *stats,
                                   uint16 &max_freq)
{
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    DiskVector<IL_TYPE> vec(_rel, _store._short.vec_blkno, _need_wal);
    size_t res = 0;
    [[maybe_unused]] float max_float = 0;
    vec.template apply<AccessorLockType::WriteLock>([&](IL_TYPE &entry) -> size_t {
        if (entry.version != _store._short.version) {
            return 0;
        }
        uint16 cur = 0;
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            if (entry.entries[i].doc_id == 0) {
                break;
            }
            if (id_track.contains(entry.entries[i].doc_id)) {
                ++res;
                entry.entries[i].doc_id = 0;
            } else {
                std::swap(entry.entries[cur], entry.entries[i]);
                CONSTEXPR_IF (is_sparse) {
                    float temp = half_to_float_unsigned(entry.entries[cur].freq);
                    if (temp > max_float) {
                        max_float = temp;
                    }
                }
                ++cur;
            }
        }
        return res > 0;
    })(_store._short.offset);
    vec.destroy();
    CONSTEXPR_IF (is_sparse) {
        max_freq = float_to_half(max_float);
    }
    return res;
}

/** 
 * Vacuum locks page from left to right same as look up, except that the latter only holds one lock.
 * During insert, it's possible for the operation locks up the inserted page and then go backward
 *  to update pointer infos, currently up to 5 pages. However, it's worth to dive deep into these
 *  operations and find out the actual potential conflicts:
 *  1. get_new_page/goto next page as the insert page is old:
 *      In this case, the left page is locked first, and then the right one is accessed, the order
 *      is still left to right.
 *  2. update prev 5-pointer:
 *      The operation holds current insert page lock, then iterate left pages with r/w locks.
 *      But the lock is held after the prev one is released while the insert page lock remains held.
 *  3. go backward to locate insert page to fit inserted doc_id:
 *      The case occurs when the doc_id is older than the inserted page, and the operation will
 *      release the current page and reset the insert page backward. The old page is likely to be
 *      full so it will launch a new insertion of the last element in the old page. This process
 *      also holds one lock at a time. Note that there are no ways 2 and 3 can happen together.
 *  Then it seems to become clear that as long as we do not touch the insert page with left to right
 *  operation, there will be no conflicts even though we holds two locks from left to right.
 *  Now, to reasoning about how to validate the insert page as there is no synchronic machanism on
 *  it, notice that if the memory commits in order (which is always true in all universe), then the
 *  insert page can be either old (on the left of the actual one) or updated (the actual one).
 *  If vacuum holds the old one, we just stop earlier and restart from the old insert page, but
 *  note that we cannot continue by holding two locks as there can be updated-page -> old-page lock
 *  order by insert operation.
 *  If vacuum holds the updated one and there are insertions holding old insert pages,
 *  these insertions will go to routine 1 as they should know their pages are old; blame lock
 *  implementation if they don't.
 * 
 * Since all entries have to be ordered, we move them between pages as long as possible,
 *  no operations can be saved without breaking the ordering (which is hard to achieve while
 *  maintaining valid back up readers).
 * 
 * Thus, we reach a straightforward solution:
 *  Perform vacuum from start page until insert page (exclusive), lock two pages at a time,
 *  do vacuum and packing. If the second page can be deleted after packing, according to insertion
 *  lookup dependency at line 71, we cannot recollect it instantly. A relatively good solution is to
 *  mark the page deleted first (nentry = 0), and recollect it after locking the third page.
 *  At the insert page, we cannot lock two pages at the same time, so it's impossible to do packing
 *  between pages. We just do page-level vacuum and packing and leave them there.
 * 
 * We don't think about adaptation to further small list optimizations during vacuum.
 *  It's token index's job during re-update.
 * 
 * The packing implementation is not trivial, at least it's not trivial to come out one.
 *  Think about the scenario below: we are packing page A and B,
 *  while the insertion at C is relocating to B, illustrated below:
 *          A(1,2,3)  B(4,5,7)  C(8,9,10) <- 6
 *  After packing on A and B, B is empty, so the insertion gets B and then aquire A
 *  which is still locked. The vacuum gets C and packing it to A.
 *          A(1,7)  B() <- 6 C(8,9,10)
 *  How to guarentee that 6 can be inserted to right position rather than a wrong starting page?
 *  Also, how to ensure 5-block pointer is set properly while removing blocks?
 * 
 *  We need some base assumptions to hold for insertions being valid:
 *  1. AB, AC, BC should always be valid (all elements in order, page for mat is valid)
 *     when any lock is released -- trivial.
 *  2. The insertion can find correct lower bound page:
 *      a. If A only has larger elements, according to 1, it should continue to the left.
 *      b. If B has no element larger than the inserted, the corrent page is either B or the page to
 *         its right, so B is always a good lower bound.
 *      c. If B is empty, then either A or C can be the inserted page, so there is no way to
 *         determine the direction. However, given the only reason for B to be empty is that B is
 *         merged into A, we always go to the left.
 *  3. If we find a correct lower bound page, we can always find a correct location by moving
 *     to the right -- trivial.
 *  4. B can only be deleted if there is no existing link to B, which includes the pointers from
 *     both direction and start/insert link, however, as some threads may hold an obsolete insert
 *     link, we delete them with a vacuum lock (lock until no contension, which has no performance
 *     cost if it is already invisible through pointers).
 * 
 * With all above elements considered, the framework of vacuum/insert should be simple as mentioned.
 * 
 * There is no log issues as long as all logs are applied with the write order.
 * 
 * TD mingwei: set up correct next id and max freq
 */
template <bool is_sparse>
size_t InvertedList::vacuum(doc_id_track &id_track, IndexBulkDeleteResult *stats, uint16 &max_freq)
{
    if (_is_short) {
        static_assert(n_il_level == 3u, "n_il_level must be 3");
        switch (_store._short.level) {
            case 0: return vacuum_helper<0, is_sparse>(id_track, stats, max_freq);
            case 1u: return vacuum_helper<1u, is_sparse>(id_track, stats, max_freq);
            case 2u: return vacuum_helper<2u, is_sparse>(id_track, stats, max_freq);
            default:
                __builtin_unreachable();
        }
    }

    if (!BlockNumberIsValid(_store._long.start_blkno)) {
        return 0;
    }

    using freq_type = typename std::conditional<is_sparse, float, uint16>::type;
    static constexpr auto to_score = [](uint16 freq) -> freq_type {
        CONSTEXPR_IF (is_sparse) {
            return half_to_float_unsigned(freq);
        } else {
            return freq;
        }
    };
    static constexpr auto to_freq = [](freq_type freq) -> uint16 {
        CONSTEXPR_IF (is_sparse) {
            return float_to_half(freq);
        } else {
            return freq;
        }
    };
    freq_type max_score = 0;
    const auto vacuum_page = [&](InvertedListPage il_page, Buffer buf) -> size_t {
        stats->num_index_tuples += il_page->nentry;
        UnorderedSet<uint16> delete_idx;
        for (uint16 i = 0; i < il_page->nentry; ++i) {
            if (!id_track.contains(il_page->entries[i].doc_id)) {
                continue;
            }
            delete_idx.insert(i);
        }
        if (delete_idx.empty()) {
            ann_helper::optional_destroy(delete_idx);
            freq_type temp = to_score(il_page->get_pointer(0).max_freq);
            if (max_score < temp) {
                max_score = temp;
            }
            return 0;
        }
        size_t res = delete_idx.size();
        stats->tuples_removed += res;
        if (il_page->nentry == res) {
            il_page->nentry = 0;
            il_page->get_pointer(0).max_freq = 0;
            MarkBufferDirty(buf);
            if (_need_wal) {
                Bm25XLogAddData(buf, BufferGetPage(buf), {
                    InvertedListPageMacro::page_nentry_offset,
                    InvertedListPageMacro::pointer_offset - InvertedListPageMacro::nentry_offset +
                        sizeof(InvertedListPointer)
                });
            }
            ann_helper::optional_destroy(delete_idx);
            return res;
        }
        /* it's not likely for a long array with little deleted data
         * as vacuum should be triggered with certain non-trivial threshold,
         * so we do rebuild instead of memmove */
        uint16 cur_idx = 0;
        for (uint16 i = 0; i < il_page->nentry; ++i) {
            if (delete_idx.contains(i)) {
                continue;
            }
            if (i != cur_idx) {
                il_page->entries[cur_idx] = il_page->entries[i];
            }
            freq_type temp = to_score(il_page->entries[i].freq);
            if (max_score < temp) {
                max_score = temp;
            }
            ++cur_idx;
        }
        il_page->nentry = cur_idx;
        MarkBufferDirty(buf);
        if (_need_wal) {
            if (unlikely(il_page->nentry == 0)) {
                Bm25XLogAddData(buf, BufferGetPage(buf), {
                    InvertedListPageMacro::page_nentry_offset,
                    sizeof(uint16)
                });
            } else {
                Bm25XLogUpdateInvertList(buf, BufferGetPage(buf), {
                    InvertedListPageMacro::page_entry_offset,
                    sizeof(InvertedListEntry) * il_page->nentry,
                    il_page->nentry
                });
            }
        }
        ann_helper::optional_destroy(delete_idx);
        return res;
    };
    const auto merge_page = [this](InvertedListPage l, Buffer lb, InvertedListPage r, Buffer rb) -> bool {
        uint16 nentry_can_move = max_il_page_nentry - l->nentry;
        if (nentry_can_move > r->nentry) {
            nentry_can_move = r->nentry;
        }
        if (nentry_can_move == 0) {
            return r->nentry == 0;
        }
        errno_t rc = memcpy_s(l->entries + l->nentry, sizeof(InvertedListEntry) * nentry_can_move,
                              r->entries, sizeof(InvertedListEntry) * nentry_can_move);
        securec_check_c(rc, "\0", "\0");
        uint16 l_nentry = l->nentry;
        l->nentry += nentry_can_move;
        MarkBufferDirty(lb);
        if (_need_wal) {
            Bm25XLogUpdateInvertList(lb, BufferGetPage(lb), {
                InvertedListPageMacro::page_nth_entry_offset(l_nentry),
                sizeof(InvertedListEntry) * nentry_can_move,
                l->nentry
            });
        }
        r->nentry -= nentry_can_move;
        if (r->nentry > 0) {
            rc = memmove_s(r->entries, sizeof(InvertedListEntry) * max_il_page_nentry,
                           r->entries + nentry_can_move, sizeof(InvertedListEntry) * r->nentry);
            securec_check_c(rc, "\0", "\0");
        }
        MarkBufferDirty(rb);
        if (_need_wal) {
            if (r->nentry > 0) {
                Bm25XLogUpdateInvertList(rb, BufferGetPage(rb), {
                    InvertedListPageMacro::page_entry_offset,
                    sizeof(InvertedListEntry) * r->nentry,
                    r->nentry
                });
            } else {
                Bm25XLogAddData(rb, BufferGetPage(rb), {
                    InvertedListPageMacro::page_nentry_offset,
                    sizeof(uint16)
                });
            }
        }
        return r->nentry == 0;
    };

    Buffer cur_buf = ReadBuffer(_rel, _store._long.start_blkno);
    BlockNumber cur_blkno = _store._long.start_blkno;
    InvertedListPage cur_il_page = GetInvertedListPage(BufferGetPage(cur_buf));
    LockBuffer(cur_buf, BUFFER_LOCK_EXCLUSIVE);
    size_t res = vacuum_page(cur_il_page, cur_buf);
    cur_blkno = cur_il_page->get_pointer(0).blkno;
    while (BlockNumberIsValid(cur_blkno) && cur_blkno != _store._long.insert_blkno) {
        CHECK_FOR_INTERRUPTS();
        Buffer next_buf = ReadBuffer(_rel, cur_blkno);
        InvertedListPage next_il_page = GetInvertedListPage(BufferGetPage(next_buf));
        LockBuffer(next_buf, BUFFER_LOCK_EXCLUSIVE);
        res += vacuum_page(next_il_page, next_buf);
        if (merge_page(cur_il_page, cur_buf, next_il_page, next_buf)) {
            cur_blkno = next_il_page->get_pointer(0).blkno;
            UnlockReleaseBuffer(next_buf);
        } else {
            cur_blkno = next_il_page->get_pointer(0).blkno;
            UnlockReleaseBuffer(cur_buf);
            cur_buf = next_buf;
            cur_il_page = next_il_page;
        }
    }
    cur_blkno = cur_il_page->get_pointer(0).blkno;
    UnlockReleaseBuffer(cur_buf);
    while (BlockNumberIsValid(cur_blkno)) {
        CHECK_FOR_INTERRUPTS();
        cur_buf = ReadBuffer(_rel, cur_blkno);
        LockBuffer(cur_buf, BUFFER_LOCK_EXCLUSIVE);
        cur_il_page = GetInvertedListPage(BufferGetPage(cur_buf));
        res += vacuum_page(cur_il_page, cur_buf);
        cur_blkno = cur_il_page->get_pointer(0).blkno;
        UnlockReleaseBuffer(cur_buf);
    }
    max_freq = to_freq(max_score);

    fix_stats_pages<is_sparse>();
    return res;
}

template <bool is_sparse>
void InvertedList::fix_stats_pages()
{
    if (_is_short || !BlockNumberIsValid(_store._long.start_blkno)) {
        return;
    }
    using freq_type = typename std::conditional<is_sparse, float, uint16>::type;
    static constexpr auto to_score = [](uint16 freq) -> freq_type {
        CONSTEXPR_IF (is_sparse) {
            return half_to_float_unsigned(freq);
        } else {
            return freq;
        }
    };
    static constexpr auto to_freq = [](freq_type freq) -> uint16 {
        CONSTEXPR_IF (is_sparse) {
            return float_to_half(freq);
        } else {
            return freq;
        }
    };
    const auto get_next_buf = [this](InvertedListPage il_page, uint32 cur_order)
            -> Pair<Buffer, InvertedListPage> {
        if (!BlockNumberIsValid(il_page->get_pointer(0).blkno)) {
            return ::vtl::make_pair(InvalidBuffer, (InvertedListPage)NULL);
        }
        Buffer buf = ReadBuffer(_rel, il_page->get_pointer(0).blkno);
        InvertedListPage page = GetInvertedListPage(BufferGetPage(buf));
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        bool writen = page->nentry == 0;
        while (page->nentry == 0) {
            if (!page->has_next_page()) {
                UnlockReleaseBuffer(buf);
                return ::vtl::make_pair(InvalidBuffer, (InvertedListPage)NULL);
            }
            BlockNumber next_blkno = page->get_pointer(0).blkno;
            Buffer next_buf = ReadBuffer(_rel, next_blkno);
            InvertedListPage next_page = GetInvertedListPage(BufferGetPage(next_buf));
            LockBuffer(next_buf, BUFFER_LOCK_EXCLUSIVE);
            next_page->skip_pointer.prev_blkno = page->skip_pointer.prev_blkno;
            next_page->skip_pointer.order = cur_order + 1u;
            /* pointer will be writen later, no need to log here */
            il_page->get_pointer(0).blkno = next_blkno;
            UnlockReleaseBuffer(buf);
            RecordFreeIndexPage(_rel, next_blkno);
            buf = next_buf;
            page = next_page;
        }
        if (!writen) {
            if (page->skip_pointer.order != cur_order + 1u) {
                page->skip_pointer.order = cur_order + 1u;
                writen = true;
            }
        }
        if (writen) {
            MarkBufferDirty(buf);
            if (_need_wal) {
                Bm25XLogAddData(buf, BufferGetPage(buf), {
                    InvertedListPageMacro::page_pre_offset,
                    sizeof(BlockNumber) + sizeof(uint32)
                });
            }
        }
        return ::vtl::make_pair(buf, page);
    };

    Assert(BlockNumberIsValid(_store._long.start_blkno));
    freq_type lmax_score = 0;
    freq_type max_score;
    Buffer buf = ReadBuffer(_rel, _store._long.start_blkno);
    Buffer lbuf = buf;
    InvertedListPage il_page = GetInvertedListPage(BufferGetPage(buf));
    InvertedListPage lpage = il_page;
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    for (; BufferIsValid(buf);) {
        CHECK_FOR_INTERRUPTS();
        max_score = 0;
        uint32 order = il_page->skip_pointer.order;
        auto [next_buf, next_page] = get_next_buf(il_page, order);
        for (uint16 i = 0; i < il_page->nentry; ++i) {
            max_score = std::max(max_score, to_score(il_page->entries[i].freq));
        }
        lmax_score = std::max(max_score, lmax_score);
        bool changed = false;
        auto &pointer = il_page->get_pointer(0);
        uint16 target_freq = to_freq(max_score);
        if (BufferIsValid(next_buf)) {
            pointer.max_freq = target_freq;
            pointer.next_doc_id = next_page->entries[0].doc_id;
        } else {
            pointer.init();
            pointer.max_freq = target_freq;
        }
        if (il_page->has_next_lpage() && order % InvertedListSkipPointers::slevel_len != 0) {
            il_page->get_pointer(1).init();
            changed = true;
        }
        MarkBufferDirty(buf);
        if (_need_wal) {
            Bm25XLogAddData(buf, BufferGetPage(buf), {
                InvertedListPageMacro::page_pointer_offset,
                sizeof(InvertedListPointer) + (changed ? sizeof(InvertedListPointer) : 0)
            });
        }
        if ((order + 1u) % InvertedListSkipPointers::slevel_len == 0) {
            Assert(BufferIsValid(lbuf));
            changed = false;
            auto &lpointer = lpage->get_pointer(1);
            LockBuffer(lbuf, BUFFER_LOCK_EXCLUSIVE);
            if (BufferIsValid(next_buf)) {
                if (lpointer.blkno != pointer.blkno) {
                    lpointer.blkno = pointer.blkno;
                    changed = true;
                }
                freq_type target_freq = to_freq(lmax_score);
                if (lpointer.max_freq != target_freq) {
                    lpointer.max_freq = target_freq;
                    changed = true;
                }
                if (lpointer.next_doc_id != next_page->entries[0].doc_id) {
                    lpointer.next_doc_id = next_page->entries[0].doc_id;
                    changed = true;
                }
            } else {
                lpointer.init();
                changed = true;
            }
            if (changed) {
                MarkBufferDirty(lbuf);
                if (_need_wal) {
                    Bm25XLogAddData(lbuf, BufferGetPage(lbuf), {
                        InvertedListPageMacro::page_pointer_offset + sizeof(InvertedListPointer),
                        sizeof(InvertedListPointer)
                    });
                }
            }
            UnlockReleaseBuffer(lbuf);
            lbuf = InvalidBuffer;
        }
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        if (order % InvertedListSkipPointers::slevel_len != 0) {
            ReleaseBuffer(buf);
        } else {
            lbuf = buf;
            lpage = il_page;
        }
        buf = next_buf;
        il_page = next_page;
    }
    if (BufferIsValid(lbuf)) {
        if (lpage->has_next_lpage()) {
            LockBuffer(lbuf, BUFFER_LOCK_EXCLUSIVE);
            lpage->get_pointer(1).init();
            MarkBufferDirty(lbuf);
            if (_need_wal) {
                Bm25XLogAddData(lbuf, BufferGetPage(lbuf), {
                    InvertedListPageMacro::page_pointer_offset + sizeof(InvertedListPointer),
                    sizeof(InvertedListPointer)
                });
            }
            LockBuffer(lbuf, BUFFER_LOCK_UNLOCK);
        }
        ReleaseBuffer(lbuf);
    }
}

bool InvertedList::try_downgrade(uint32 current_ndoc, BlockNumber &start_blkno,
                                 BlockNumber &insert_blkno, const InvertedListMeta meta)
{
    return false;
}

template <uint16 l>
bool InvertedList::upgrade_helper(BlockNumber &start_blkno, BlockNumber &insert_blkno,
                                  const InvertedListMeta meta)
{
    using IL_TYPE = FixedInvertedList<il_threshold_levels[l]>;
    using NEW_IL_TYPE = FixedInvertedList<il_threshold_levels[std::min(l + 1u, n_il_level - 1u)]>;

    DiskVector<IL_TYPE> vec(_rel, _store._short.vec_blkno, _need_wal);
    bool can_upgrade = false;
    vec.template apply<AccessorLockType::ReadLock>(
        [this, &can_upgrade](IL_TYPE &entry) -> bool {
        if (entry.version != _store._short.version) {
            return false;
        }
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            if (entry.entries[i].doc_id == 0) {
                return false;
            }
        }
        can_upgrade = true;
        return false;
    })(_store._short.offset);
    if (!can_upgrade) {
        vec.destroy();
        return false;
    }

    vec.wlock();
    can_upgrade = false;
    NEW_IL_TYPE il_entry;
    vec.template apply<AccessorLockType::WriteLock>(
        [this, &can_upgrade, &il_entry](IL_TYPE &entry) -> bool {
        if (entry.version != _store._short.version) {
            return false;
        }
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            if (entry.entries[i].doc_id == 0) {
                return false;
            }
        }
        can_upgrade = true;
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            il_entry.entries[i] = entry.entries[i];
        }
        for (uint16 i = 0; i < IL_TYPE::n; ++i) {
            entry.entries[i].doc_id = 0;
        }
        return true;
    })(_store._short.offset);
    vec.wunlock();
    vec.destroy();
    if (!can_upgrade) {
        return false;
    }

    CONSTEXPR_IF(l + 1u < n_il_level) {
        for (uint16 i = IL_TYPE::n; i < NEW_IL_TYPE::n; ++i) {
            il_entry.entries[i].doc_id = 0;
        }
        DiskVector<NEW_IL_TYPE> vec1(_rel, meta->vec_blknos[l + 1u], _need_wal);
        FreeSpace<uint32> free_space1(_rel, meta->free_space_blknos[l + 1u], _need_wal);
        uint32 offset;
        if (!free_space1.pop(offset)) {
            il_entry.version = 0;
            offset = vec1.push_back(il_entry);
            if (short_il_offset_mask & offset) {
                ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                errmsg("Inverted list too long."),
                                errhint("Please vacuum or rebuild the index.")));
            }
        } else {
            inc_version(offset);
            il_entry.version = get_version(offset);
            if (_need_wal) {
                vec1.template set<AccessorLockType::WriteLock>(get_offset(offset), il_entry);
            } else {
                vec1.template set<AccessorLockType::NoLockWrite>(get_offset(offset), il_entry);
            }
        }
        vec1.destroy();
        free_space1.destroy();
        FreeSpace<uint32> free_space(_rel, meta->free_space_blknos[l], _need_wal);
        free_space.insert(insert_blkno);
        free_space.destroy();
        insert_blkno = offset;
        start_blkno = l + 1u;
        return true;
    }

    auto [blkno, buf, page_is_new] = get_new_page(_rel);
    Page page = BufferGetPage(buf);
    PageInit(page, BLCKSZ, 0);
    ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
    auto il_page = GetInvertedListPage(page);
    il_page->init();
    for (uint16 i = 0; i < NEW_IL_TYPE::n; ++i) {
        il_page->entries[i] = il_entry.entries[i];
    }
    il_page->nentry = NEW_IL_TYPE::n;
    il_page->skip_pointer.order = 0;
    MarkBufferDirty(buf);
    if (_need_wal) {
        if (page_is_new) {
            Bm25XLogAppendPage(buf, page);
        } else {
            Bm25XLogInitPage(buf, page);
        }
    }
    ReleaseBuffer(buf);
    FreeSpace<uint32> free_space(_rel, meta->free_space_blknos[l], _need_wal);
    free_space.insert(insert_blkno);
    free_space.destroy();
    start_blkno = insert_blkno = blkno;
    return true;
}

bool InvertedList::try_upgrade(uint32 current_ndoc, BlockNumber &start_blkno,
                               BlockNumber &insert_blkno, const InvertedListMeta meta)
{
    if (!_is_short || current_ndoc < il_threshold_levels[_store._short.level]) {
        return false;
    }
    static_assert(n_il_level == 3u, "n_il_level must be 3");
    switch (_store._short.level) {
        case 0: return upgrade_helper<0>(start_blkno, insert_blkno, meta);
        case 1u: return upgrade_helper<1u>(start_blkno, insert_blkno, meta);
        case 2u: return upgrade_helper<2u>(start_blkno, insert_blkno, meta);
        default:
            __builtin_unreachable();
    }
}

bool InvertedList::upon_threshold(uint32 current_ndoc) const
{
    if (!_is_short) {
        return false;
    }
    return current_ndoc > il_threshold_levels[_store._short.level];
}

bool InvertedList::below_threshold(uint32 current_ndoc) const
{
    if (!_is_short || _store._short.level == 0) {
        return false;
    }
    return current_ndoc <= il_threshold_levels[_store._short.level - 1u];
}

template BlockNumber InvertedList::insert<false>(uint64, uint16);
template BlockNumber InvertedList::insert<true>(uint64, uint16);
template size_t InvertedList::vacuum<false>(doc_id_track &, IndexBulkDeleteResult *, uint16 &);
template size_t InvertedList::vacuum<true>(doc_id_track &, IndexBulkDeleteResult *, uint16 &);
template void InvertedList::verify<false>(stats_type) const;
template void InvertedList::verify<true>(stats_type) const;
