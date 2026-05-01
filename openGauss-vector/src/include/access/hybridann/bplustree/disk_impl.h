/**
 * Copyright ...
 * PG interface for B+ tree implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_DISK_IMPL_H
#define DISKANN_CONTAINER_BPLUSTREE_DISK_IMPL_H

#include "access/annvector/macro.h"
#include "access/annvector/module/leak_checker.h"
#include "access/annvector/xlog/log_manager.h"
#include "access/hybridann/bplustree/base.h"
#include "access/hybridann/bplustree/data_impl.h"
#include "access/diskann/vector_bt.h"
#include "access/itup.h"
#include "storage/off.h"
#include "storage/lmgr.h"
#include "storage/buf/buf.h"
#include "storage/buf/bufmgr.h"
#include "storage/buf/bufpage.h"
#include "storage/buf/block.h"
#include "utils/relcache.h"

namespace disk_container {
struct DiskBTOpaqueData {
    uint16 page_id;
    uint16 cycle_id;
    uint32 info;
    BlockNumber prev;
    BlockNumber next;
    TransactionId xact; /* next transaction ID, if deleted */

    constexpr static uint32 LEAF_MASK = 0x00000100u;
    constexpr static uint32 LEVEL_MASK = 0x000000FFu;
    constexpr static uint32 SIZE_MASK = 0x001FFE00u;
    constexpr static uint32 ROOT_MASK = 0x00200000u;
    constexpr static uint32 VACUUM_DELETING_MASK = 0x00400000u;
    constexpr static uint32 DELETE_MASK = 0x00800000u;
    constexpr static uint32 HALF_DEAD_MASK = 0x01000000u;
    constexpr static uint32 SPLIT_END_MASK = 0x02000000u;
    constexpr static uint32 HAS_GARBAGE_MASK = 0x04000000u;
    constexpr static uint32 INCOMPLETE_SPLIT_MASK = 0x08000000u;
    constexpr static uint32 TEMPORARILY_UNLINKABLE = 0x10000000u;
    static_assert(__builtin_popcount(SIZE_MASK) >= 12, "SIZE_MASK should have at least 12 bits");
    bool is_leaf() const { return info & LEAF_MASK; }
    uint16 level() const { return info & LEVEL_MASK; }
    OffsetNumber size() const { return (info & SIZE_MASK) >> 9u; }
    void set_size(uint16 size) { info = (info & ~SIZE_MASK) | (size << 9u); }
    void inc_size() { info += (1u << 9u); }
    void dec_size() { Assert(size() > 0); info -= (1u << 9u); }
    void init_flag() { info = 0u; }
};
typedef DiskBTOpaqueData *DiskBTOpaque;

struct DiskBTInternalOpaqueData : public DiskBTOpaqueData {
    BlockNumber vec_index_meta_blkno[max_index_magnitude_size];
    BlockNumber new_vec_index_meta_blkno[max_index_magnitude_size];
    void init_vec_meta()
    {
        for (size_t i = 0; i < max_index_magnitude_size; ++i) {
            vec_index_meta_blkno[i] = InvalidBlockNumber;
            new_vec_index_meta_blkno[i] = InvalidBlockNumber;
        }
    }
};
typedef DiskBTInternalOpaqueData *DiskBTInternalOpaque;

struct DiskBTMetaData {
    uint32 magic;
    uint32 version;
    BlockNumber root_ptr;
    BlockNumber unlinked_pages_metablkno;/* record the unlinked pages from btree but not yet recycle to fsm */
};
typedef DiskBTMetaData *DiskBTMeta;
struct DiskNodeImpl : public ann_helper::LeakChecker {
    using Data = BTTupleData;
    using ScanKeyImpl = BTTupleScanKey;
    using leak_checker = ann_helper::LeakChecker;

    DiskNodeImpl() {}
    DiskNodeImpl(Relation rel, BlockNumber blkno, BufferAccessStrategy strategy = NULL)
        : _buf(ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, strategy)),
          _page(BufferGetPage(_buf)) {}
    explicit DiskNodeImpl(Buffer buf) : _buf(buf), _page(BufferGetPage(_buf)) {}
    explicit DiskNodeImpl(Page page) : _page(page) {}
    ~DiskNodeImpl()
    {
        if (!valid()) {
            leak_checker::destroy();
        }
    }
    void destroy()
    {
        if (BufferIsValid(_buf)) {
            ReleaseBuffer(_buf);
            _buf = InvalidBuffer;
        }
        leak_checker::destroy();
    }
    void mark_dirty() const { MarkBufferDirty(_buf); }

    DiskBTOpaque opaque() const { return reinterpret_cast<DiskBTOpaque>(PageGetSpecialPointer(_page)); }
    DiskBTInternalOpaque internal_opaque() const
        { return reinterpret_cast<DiskBTInternalOpaque>(PageGetSpecialPointer(_page)); }
    static constexpr Size opaque_size() { return sizeof(DiskBTInternalOpaqueData); }
    bool is_leaf() const { return opaque()->is_leaf(); }
    OffsetNumber size() const { return PageGetMaxOffsetNumber(_page); }
    bool is_vacuum_deleting() const { return opaque()->info & opaque()->VACUUM_DELETING_MASK; }
    void set_vacuum_deleting(bool deleting)
    { 
        opaque()->info = deleting ?
                         opaque()->info | opaque()->VACUUM_DELETING_MASK :
                         opaque()->info & ~opaque()->VACUUM_DELETING_MASK;
    }
    bool is_deleted() const { return opaque()->info & opaque()->DELETE_MASK; }
    void set_deleted(bool deleted)
    {
        opaque()->info = deleted ?
                         opaque()->info | opaque()->DELETE_MASK :
                         opaque()->info & ~opaque()->DELETE_MASK;
    }
    bool is_half_dead() const { return opaque()->info & opaque()->HALF_DEAD_MASK; }
    void set_half_dead(bool half_dead)
    {
        opaque()->info = half_dead ?
                         opaque()->info | opaque()->HALF_DEAD_MASK :
                         opaque()->info & ~opaque()->HALF_DEAD_MASK;
    }

    bool is_temporarily_unlinkable() const { return opaque()->info & opaque()->TEMPORARILY_UNLINKABLE; }
    void set_temporarily_unlinkable(bool flag)
    {
        opaque()->info = flag ?
                         opaque()->info | opaque()->TEMPORARILY_UNLINKABLE :
                         opaque()->info & ~opaque()->TEMPORARILY_UNLINKABLE;
    }
    bool is_split_end() const { return opaque()->info & opaque()->SPLIT_END_MASK; }
    void set_split_end(bool split_end)
    {
        opaque()->info = split_end ?
                         opaque()->info | opaque()->SPLIT_END_MASK :
                         opaque()->info & ~opaque()->SPLIT_END_MASK;
    }
    bool has_garbage() const { return opaque()->info & opaque()->HAS_GARBAGE_MASK; }
    void set_has_garbage(bool has_garbage)
    {
        opaque()->info = has_garbage ?
                         opaque()->info | opaque()->HAS_GARBAGE_MASK :
                         opaque()->info & ~opaque()->HAS_GARBAGE_MASK;
    }
    bool is_incomplete_split() const { return opaque()->info & opaque()->INCOMPLETE_SPLIT_MASK; }
    void set_incomplete_split(bool incomplete_split)
    {
        opaque()->info = incomplete_split ?
                         opaque()->info | opaque()->INCOMPLETE_SPLIT_MASK :
                         opaque()->info & ~opaque()->INCOMPLETE_SPLIT_MASK;
    }
    bool is_root() const { return opaque()->info & opaque()->ROOT_MASK; }
    void set_root(bool root)
    {
        opaque()->info = root ?
                         opaque()->info | opaque()->ROOT_MASK :
                         opaque()->info & ~opaque()->ROOT_MASK;
    }
    void set_leaf(bool leaf)
        { opaque()->info = leaf ? opaque()->info | opaque()->LEAF_MASK : opaque()->info & ~opaque()->LEAF_MASK; }
    void set_level(uint16 level) { opaque()->info = (opaque()->info & ~opaque()->LEVEL_MASK) | level; }
    bool is_internal() const { return !is_leaf(); }
    bool can_ignore() const { return is_deleted() || is_half_dead(); }
    bool can_insert(const Data *x) const { return PageGetFreeSpace(_page) >= MAXALIGN(x->size()); }

    bool ignore() { return opaque()->info & (opaque()->DELETE_MASK | opaque()->HALF_DEAD_MASK); }
    void set_xact(TransactionId id) { opaque()->xact = id; }

    /* return the first element offset that is greater or equal to the key */
    OffsetNumber item_index_of(const ScanKeyImpl &key) const
    {
        OffsetNumber high = size();
        OffsetNumber low = p_firstdatakey(*this);
        /*
         * If there are no keys on the page, return the first available slot. Note
         * this covers two cases: the page is really empty (no keys), or it
         * contains only a high key.  The latter case is possible after vacuuming.
         * This can never happen on an internal page, however, since they are
         * never empty (an internal page must have children).
         */
        if (low > high) {
            return low;
        }
        const bool isleaf = is_leaf();
        /* skip the first item for internal nodes as it's supposed to be minimum */
        if (!isleaf) {
            low = OffsetNumberNext(low);
        }
        ++high; /* establish the loop invariant for high */
        OffsetNumber mid;
        while (low < high) {
            mid = low + (high - low) / 2; /* no possible overflow */
            if (key < *get_data(mid)) {
                high = mid;
            } else {
                low = mid + 1;
            }
        }
        Assert(low > 0 && (isleaf || low > p_firstdatakey(*this)));
        return isleaf ? low : OffsetNumberPrev(low);
    }
    /* return the last element offset that is less or equal to the key */
    OffsetNumber right_item_index_of(const ScanKeyImpl &key) const
    {
        OffsetNumber high = size();
        OffsetNumber low = p_firstdatakey(*this);
        if (low > high) {
            return low;
        }
        const bool isleaf = is_leaf();
        /* skip the first item for internal nodes as it's supposed to be minimum */
        if (!isleaf) {
            low = OffsetNumberNext(low);
        }
        ++high; /* establish the loop invariant for high */
        OffsetNumber mid;
        while (low < high) {
            mid = low + (high - low) / 2; /* no possible overflow */
            if (key > *get_data(mid)) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return OffsetNumberPrev(low);
    }
    OffsetNumber split_loc(OffsetNumber newitemoff, const Data *data) const
    {
        const size_t avail_size = _capacity();
        const size_t diff_threshold = avail_size / 16;
        OffsetNumber best_offset = InvalidOffsetNumber;
        size_t left_size = 0u, left_high_size;
        size_t right_size = get_page_used_size();
        size_t right_high_size = is_right_most() ? 0 : get_data(p_hikey)->size();
        size_t best_diff = SIZE_MAX;
        for (OffsetNumber i = p_firstdatakey(*this); i <= size(); i = OffsetNumberNext(i)) {
            left_high_size = get_data(i)->size();
            Assert(right_size >= left_high_size);
            left_size += left_high_size;
            right_size -= left_high_size;
            const size_t ll = left_size + left_high_size;
            const size_t rr = right_size + right_high_size;
            const size_t diff = ll > rr ? ll - rr : rr - ll;
            if (diff < diff_threshold) {
                best_offset = i;
                break;
            }
            if (diff < best_diff) {
                best_diff = diff;
                best_offset = i;
            }
        }
        Assert(OffsetNumberIsValid(best_offset));
        return best_offset;
    }

    void insert(OffsetNumber i, const Data *k)
    {
        if (PageAddItem(_page, (Item)k, k->size(), i, false, false) == InvalidOffsetNumber) {
            ereport(ERROR, (errmodule(MOD_INDEX),
                errcode(ERRCODE_INDEX_CORRUPTED),
                errmsg("failed to add item to the index page"),
                errdetail("item size: %lu, Tid: block %u, offset %u,",
                          (unsigned long)k->size(),
                          BlockIdGetBlockNumber(&(k->tuple()->t_tid.ip_blkid)),
                          k->tuple()->t_tid.ip_posid),
                errcause("System error."),
                erraction("Check WARNINGS for the details.")));
        }
    }

    void r_lock() { LockBuffer(_buf, BUFFER_LOCK_SHARE); }
    void w_lock() { LockBuffer(_buf, BUFFER_LOCK_EXCLUSIVE); }
    bool r_trylock() { return TryLockBuffer(_buf, BUFFER_LOCK_SHARE, false); }
    bool w_trylock() { return TryLockBuffer(_buf, BUFFER_LOCK_EXCLUSIVE, false); }
    void r_unlock() { unlock(); }
    void w_unlock() { unlock(); }
    void unlock() { LockBuffer(_buf, BUFFER_LOCK_UNLOCK); }
    void r_unlock_destroy() { r_unlock(); destroy(); }
    void w_unlock_destroy() { w_unlock(); destroy(); }
    void unlock_destroy() { unlock(); destroy(); }

    Data *get_data(OffsetNumber i) const
    {
        return reinterpret_cast<Data *>(PageGetItem(_page, PageGetItemId(_page, i)));
    }
    BlockNumber ptr() const { return BufferGetBlockNumber(_buf); }
    BlockNumber &next() const { return opaque()->next; }
    BlockNumber &prev() const { return opaque()->prev; }
    bool is_left_most() const { return !BlockNumberIsValid(prev()); }
    bool is_right_most() const { return !BlockNumberIsValid(next()); }
    bool valid() const { return _page != NULL; }
    uint16 level() const { return opaque()->level(); }
    uint16 &cycle_id() { return opaque()->cycle_id; }

    BlockNumber get_ptr(OffsetNumber i) const { return get_data(i)->get_ptr(); }
    void insert(OffsetNumber i, Data *k, BlockNumber ptr)
    {
        if (ptr != InvalidBlockNumber) {
            Assert(this->is_internal());
            k->set_ptr(ptr);
        }
        insert(i, k);
    }
    BlockNumber *index_ptr() const { return internal_opaque()->vec_index_meta_blkno; }
    BlockNumber *new_index_ptr() const { return internal_opaque()->new_vec_index_meta_blkno; }
    BlockNumber get_index_ptr(size_t slot) const { return index_ptr()[slot]; }
    BlockNumber get_new_index_ptr(size_t slot) const { return new_index_ptr()[slot]; }
    void set_index_ptrs(DiskNodeImpl &node, const size_t index_magnitude_size)
    {
        for (size_t i = 0; i < index_magnitude_size; ++i) {
            index_ptr()[i] = node.index_ptr()[i];
            new_index_ptr()[i] = node.new_index_ptr()[i];
        }
    }
    void set_index_ptr(size_t slot, BlockNumber ptr) { index_ptr()[slot] = ptr; }

    void split(DiskNodeImpl &new_left, DiskNodeImpl &new_right, OffsetNumber firstright,
               OffsetNumber newitemoff, Data *new_data)
    {
        Assert(is_leaf() == new_left.is_leaf());
        Assert(is_leaf() == new_right.is_leaf());
        /* set the high key */
        OffsetNumber right_cur = p_hikey;
        if (!is_right_most()) {
            new_right.insert(p_hikey, get_data(p_hikey));
            right_cur = p_firstkey;
        }
        new_left.insert(p_hikey, get_data(firstright));
        auto copy = [this](OffsetNumber from, DiskNodeImpl &target, OffsetNumber to, OffsetNumber count) {
            if (count == 0) {
                return;
            }
            for (OffsetNumber i = 0; i < count; ++i) {
                const Data *k = get_data(from + i);
                PageAddItem(target._page, (Item)k, k->size(), to + i, false, false);
            }
        };
        if (newitemoff >= firstright) {
            if (!is_right_most()) {
                copy(p_firstkey, new_left, p_firstkey, firstright - 2);
                copy(firstright, new_right, p_firstkey, newitemoff - firstright);
                new_right.insert(newitemoff - firstright + 2, new_data);
                copy(newitemoff, new_right, newitemoff - firstright + 3, size() - newitemoff + 1);
            } else {
                copy(p_hikey, new_left, p_firstkey, firstright - 1);
                copy(firstright, new_right, p_hikey, newitemoff - firstright);
                new_right.insert(newitemoff - firstright + 1, new_data);
                copy(newitemoff, new_right, newitemoff - firstright + 2, size() - newitemoff + 1);
            }
        } else {
            if (!is_right_most()) {
                copy(p_firstkey, new_left, p_firstkey, newitemoff - 2);
                new_left.insert(newitemoff, new_data);
                copy(newitemoff, new_left, newitemoff + 1, firstright - newitemoff);
                copy(firstright, new_right, p_firstkey, size() - firstright + 1);
            } else {
                copy(p_hikey, new_left, p_firstkey, newitemoff - 1);
                new_left.insert(newitemoff + 1, new_data);
                copy(newitemoff, new_left, newitemoff + 2, firstright - newitemoff);
                copy(firstright, new_right, p_hikey, size() - firstright + 1);
            }
        }
    }

    /*
     * Add an item to a page being built.
     *
     * The main difference between this routine and a bare PageAddItem call
     * is that this code knows that the leftmost data item on a non-leaf
     * btree page doesn't need to have a key.  Therefore, it strips such
     * items down to just the item header.
     *
     * This is almost like nbtinsert.c's _bt_pgaddtup(), but we can't use
     * that because it assumes that P_RIGHTMOST() will return the correct
     * answer for the page.  Here, we don't know yet if the page will be
     * rightmost.  Offset p_firstkey is always the first data key.
     */
    void sortadd(const Data *data, OffsetNumber off)
    {
        if (is_leaf() || off != p_firstkey) {
            insert(off, data);
            return;
        }
        Data trunctuple;
        trunctuple.t_tid = data->t_tid;
        trunctuple.t_info = sizeof(Data);
        Assert(sizeof(IndexTupleData) == trunctuple.size());
        insert(off, &trunctuple);
    }

    /**
     * @brief: slide an array of ItemIds back one slot (from p_firstkey to p_hikey,
     * overwriting p_hikey). we need to do this when we discover that we have built
     * an ItemId array in what has turned out to be a P_RIGHTMOST page.
     */
    void slideleft()
    {
        if (PageIsEmpty(_page)) {
            return;
        }
        ItemId previi = PageGetItemId(_page, p_hikey);
        OffsetNumber maxoff = PageGetMaxOffsetNumber(_page);
        for (OffsetNumber off = p_firstkey; off <= maxoff; off = OffsetNumberNext(off)) {
            ItemId thisii = PageGetItemId(_page, off);
            *previi = *thisii;
            previi = thisii;
        }
        ((PageHeader)_page)->pd_lower -= sizeof(ItemIdData);
    }

    void delete_tuple(OffsetNumber offset)
    {
        PageIndexTupleDelete(_page, offset);
    }

    /*
    *	page_recyclable() -- Is an existing page recyclable?
    *
    * This exists to make sure vacuumscan have the same
    * policy about whether a page is safe to re-use.
    */
    bool page_recyclable()
    {
        /*
        * It's possible to find an all-zeroes page in an index --- for example, a
        * backend might successfully extend the relation one page and then crash
        * before it is able to make a WAL entry for adding the page. If we find a
        * zeroed page then reclaim it.
        */
        if (PageIsNew(_page))
            return true;

        /*
        * Otherwise, recycle if deleted and too old to have any processes
        * interested in it.
        */
        if (is_deleted() &&
            TransactionIdPrecedes(opaque()->xact, u_sess->utils_cxt.RecentGlobalXmin)) {
            return true;
        }
        return false;
    }


    /*
    * Delete item(s) from a btree page during VACUUM.
    *
    * This must only be used for deleting leaf items.	Deleting an item on a
    * non-leaf page has to be done as part of an atomic action that includes
    * deleting the page it points to.
    *
    * This routine assumes that the caller has pinned and locked the buffer.
    * Also, the given itemnos *must* appear in increasing order in the array.
    *
    * We record VACUUMs and b-tree deletes differently in WAL. InHotStandby
    * we need to be able to pin all of the blocks in the btree in physical
    * order when replaying the effects of a VACUUM, just as we do for the
    * original VACUUM itself. lastBlockVacuumed allows us to tell whether an
    * intermediate range of blocks has had no changes at all by VACUUM,
    * and so must be scanned anyway during replay. We always write a WAL record
    * for the last block in the index, whether or not it contained any items
    * to be removed. This allows us to scan right up to end of index to
    * ensure correct locking.
    */
    void delitems_vacuum(OffsetNumber *deletable, int num_deletable, BlockNumber last_block_vacuumed)
    {
        START_CRIT_SECTION();

        if (num_deletable > 0) {
            PageIndexMultiDelete(_page, deletable, num_deletable);
        }

        cycle_id() = 0;
        set_has_garbage(false);

        mark_dirty();

        XLogBeginInsert();
        XLogRegisterBuffer(0, _buf, REGBUF_STANDARD);
        XLogRegisterData((char *)&num_deletable, sizeof(int));
        if (num_deletable > 0) {
            XLogRegisterBufData(0, (char *)deletable, num_deletable * sizeof(OffsetNumber));
        }
        XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_VACUUM);
        PageSetLSN(_page, recptr);
        END_CRIT_SECTION();
    }

    void checkpage(Relation index)
    {
        /*
        * ReadBuffer verifies that every newly-read page passes
        * PageHeaderIsValid, which means it either contains a reasonably sane
        * page header or is all-zero.	We have to defend against the all-zero
        * case, however.
        */
        if (PageIsNew(_page)) {
            PageHeader phdr = (PageHeader)_page;
            //exrto_dump_btree_info(rel, BufferGetBlockNumber(buf), par_blkno);
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("index \"%s\" oid: %u contains unexpected zero page at block %u, pd_upper %d pd_lower %d",
                            RelationGetRelationName(index), index->rd_id, ptr(), phdr->pd_upper,
                            phdr->pd_lower),
                    errhint("Please REINDEX it.")));
        }

        /*
        * Additionally check that the special area looks sane.
        */
        if (PageGetSpecialSize(_page) != opaque_size())
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("index \"%s\" contains corrupted page at block %u", RelationGetRelationName(index),
                                ptr()),
                            errhint("Please REINDEX it.")));
    }

    void lock(bool exclusive)
    {
        if (exclusive) {
            w_lock();
        } else {
            r_lock();
        }
    }
    void trylock(bool exclusive)
    {
        if (exclusive) {
            w_trylock();
        } else {
            r_trylock();
        }
    }
    void unlock(bool exclusive)
    {
        if (exclusive) {
            w_unlock();
        } else {
            r_unlock();
        }
    }
    void unlock_destroy(bool exclusive) { unlock(exclusive); destroy(); }

    double get_page_used_ratio() { return (get_page_used_size() / (_capacity() * 1.0)); }

    Buffer _buf{InvalidBuffer};
    Page _page{NULL};
private:
    static constexpr Size _capacity() { return BLCKSZ - SizeOfPageHeaderData - opaque_size(); }
    Size get_page_used_size() const { return _capacity() - PageGetFreeSpace(_page); }
};

struct DiskBTNodeContext : public ann_helper::LeakChecker {
    using DataImpl = BTTupleData;
    using ScanKeyType = BTTupleScanKey;
    using Node = DiskNodeImpl;
    using leak_checker = ann_helper::LeakChecker;
public:
    DiskBTNodeContext(Relation rel, Relation heap, BlockNumber meta_blkno, bool wal) : _rel(rel), _heap(heap), _wal(wal)
    {
        _meta_buf = ReadBuffer(rel, meta_blkno);
        _meta = reinterpret_cast<DiskBTMeta>(PageGetContents(BufferGetPage(_meta_buf)));
        Assert(_meta->magic == DISK_BT_META_MAGIC);
        Assert(_meta->version == 1u);
        _tupDesc = buildHybridTupDesc(rel);

        Buffer buf = ReadBuffer(rel, HYBRIDANN_METAPAGE_BLKNO);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        HybridAnnMetaPage *meta = reinterpret_cast<HybridAnnMetaPage *>(PageGetContents(BufferGetPage(buf)));
        _index_magnitude = NEW IndexMagnitude(meta->indexMagnitudes, meta->sizeIndexMagnitudes, meta->graphMagnitudeThreshold);
        UnlockReleaseBuffer(buf);
    }
    void destroy() { 
        ReleaseBuffer(_meta_buf);
        pfree(_tupDesc);
        _index_magnitude->destroy();
        delete _index_magnitude;
        leak_checker::destroy();
    }

    constexpr BlockNumber invalid_ptr() const { return InvalidBlockNumber; }
    BlockNumber get_root_ptr() const { return _meta->root_ptr; }
    Buffer get_meta_buffer() const { return _meta_buf; }
    BlockNumber get_unlinkedPages_metablkno() const { return _meta->unlinked_pages_metablkno; }
    void set_root_ptr(BlockNumber root) { _meta->root_ptr = root; }
    TupleDesc get_tupDesc() { return _tupDesc; }
    /* generate a right node */
    Node get_new_node(Node &node, BlockNumber prevPtr = InvalidBlockNumber)
    {
        LockRelationForExtension(_rel, ExclusiveLock);
        Buffer new_buf = ReadBuffer(_rel, P_NEW);
        UnlockRelationForExtension(_rel, ExclusiveLock);
        /* no need to lock since it's not accessible */
        Node res(new_buf);
        PageInit(res._page, BLCKSZ, node.opaque_size());
        res.opaque()->init_flag();
        res.opaque()->page_id = DISK_BT_DATA_ID;
        res.set_level(node.level());
        res.set_leaf(node.is_leaf());
        res.opaque()->set_size(0);
        res.next() = node.next();
        res.prev() = BlockNumberIsValid(prevPtr) ? prevPtr : node.ptr();
        node.next() = res.ptr();
        res.set_index_ptrs(node, _index_magnitude->size());
        return res;
    }

    Node get_new_node(const uint16 level)
    {
        LockRelationForExtension(_rel, ExclusiveLock);
        Buffer new_buf = ReadBuffer(_rel, P_NEW);
        UnlockRelationForExtension(_rel, ExclusiveLock);
        /* no need to lock since it's not accessible */
        Node res(new_buf);
        PageInit(res._page, BLCKSZ, sizeof(DiskBTInternalOpaqueData));
        res.opaque()->page_id = DISK_BT_DATA_ID;
        res.opaque()->set_size(0);
        res.next() = res.prev() = InvalidBlockNumber;
        res.opaque()->init_flag();
        res.set_level(level);
        res.set_leaf(level == 0);
        res.internal_opaque()->init_vec_meta();
        return res;
    }

    /* generate a copy of a node */
    Node get_temp_node(Node &node)
    {
        Assert(PageSizeIsValid(PageGetPageSize(node._page)));
        Page page = (Page)palloc(BLCKSZ);
        Size opaque_size = node.opaque_size();
        PageInit(page, BLCKSZ, opaque_size);
        PageSetLSN(page, PageGetLSN(node._page));
        Node res(page);
        errno_t rc = memcpy_s(res.opaque(), opaque_size, node.opaque(), opaque_size);
        securec_check_c(rc, "\0", "\0");
        res.opaque()->set_size(0);
        return res;
    }
    void restore_destroy_temp_node(Node &&temp_node, Node &target)
    {
        memcpy(target._page, temp_node._page, BLCKSZ);
        pfree(temp_node._page);
        temp_node._page = NULL;
    }

    void r_lock() { LockBuffer(_meta_buf, BUFFER_LOCK_SHARE); }
    void w_lock() { LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE); }
    bool r_trylock() { return TryLockBuffer(_meta_buf, BUFFER_LOCK_SHARE, false); }
    bool w_trylock() { return TryLockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE, false); }
    void r_unlock() { LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK); }
    void w_unlock() { LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK); }

    size_t estimate_total_capacity(Node &node)
    {
        return 300ul * node.level() * node.level();
    }

    size_t estimate_root_index_capacity(Node &node)
    {
        Assert(node.is_root() && node.is_internal());
        return 300ul * (node.level() + 1ul) * (node.level() + 1ul) / 5ul;
    }

    void mark_dirty() const { MarkBufferDirty(_meta_buf); }
    Node get_node(BlockNumber ptr) const { return Node(_rel, ptr); }
    bool need_wal() const { return _wal && RelationNeedsWAL(_rel); }
    ScanKeyType get_scan_key(const DataImpl *data)
    {
        Assert(RelationGetDescr(_rel)->natts >= 1);
        const uint32 nproc = uint32(RelationGetDescr(_rel)->natts);
        FmgrInfo *proc[nproc];
        for (uint32 i = 1; i < nproc; ++i) {
            proc[i] = index_getprocinfo(_rel, i + 1, HYBRID_BTORDER_PROC);
        }
        return ScanKeyType(*data, _tupDesc, _rel->rd_indcollation, proc);
    }
    Relation get_index() { return _rel; }
    Relation get_heap() { return _heap; }
    IndexMagnitude* index_magnitude() { return _index_magnitude; }
protected:
    Relation _rel;
    Relation _heap;
    Buffer _meta_buf;
    DiskBTMeta _meta;
    bool _wal;
    TupleDesc _tupDesc;
    IndexMagnitude *_index_magnitude;
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_DISK_IMPL_H */
