/**
 * Copyright ...
 * B+ Tree implementation, supposed to be decoupling with PG buffer interface.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_H
#define DISKANN_CONTAINER_BPLUSTREE_H

#include <vtl/vector>
#include <vtl/hashtable>

#include "access/hybridann/bplustree/disk_impl.h"
#include "access/hybridann/bplustree/data_impl.h"
#include "access/diskann/vector_bt_factory.h"
#include "access/index_backend/index_backend.h"
#include "access/annvector/xlog/log_manager.h"

namespace disk_container {
inline size_t get_vector_id(IndexTuple tuple, TupleDesc tupleDesc)
{
    bool is_null;
    size_t vec_id = DatumGetUInt64(index_getattr(tuple, 1, tupleDesc, &is_null));
    Assert(!is_null);
    return vec_id;
}

inline void delete_vector_index(VectorIndex *index)
{
    ann_helper::optional_destroy(*index);
    delete index;
}

/**
 * Lehman & Yao B+ tree implementation.
 */
class BPlusTreeImpl {
protected:
    using Data = BTTupleData;
    using ScanKeyImpl = BTTupleScanKey;
    using NodeImpl = DiskNodeImpl;
    using ContextImpl = DiskBTNodeContext;
    using NodePtr = decltype(((ContextImpl *)nullptr)->get_root_ptr());
    using ScanKeyType = decltype(((ContextImpl *)nullptr)->get_scan_key(new Data()));
    struct SearchEntry {
        NodePtr ptr;
        OffsetNumber offset;
        SearchEntry() {}
        SearchEntry(NodePtr ptr, OffsetNumber offset) : ptr(ptr), offset(offset) {}
    };
    using PathStack = Vector<SearchEntry>;
public:
    template <typename ...Args>
    explicit BPlusTreeImpl(Args &&...args) : _ctx(std::forward<Args>(args)...) {}
    void destroy() { _ctx.destroy(); }

    void log_all_nodes(int err_level)
    {
        if (err_level < client_min_messages && err_level < log_min_messages) {
            return;
        }
        _ctx.r_lock();
        NodePtr ptr = _ctx.get_root_ptr();
        _ctx.r_unlock();
        uint16 level;
        do {
            NodeImpl node = _ctx.get_node(ptr);
            node.r_lock();
            level = node.level();
            uint32 nnode = 0;
            uint32 nelement = 0;
            NodePtr next_ptr;
            for (;;) {
                ++nnode;
                nelement += node.size();
                next_ptr = node.next();
                node.r_unlock_destroy();
                if (BlockNumberIsValid(next_ptr)) {
                    node = _ctx.get_node(next_ptr);
                    node.r_lock();
                } else {
                    break;
                }
            }
            double avg_node_size = (double)nelement / nnode;
            double std_dev = 0;
            next_ptr = ptr;
            do {
                node = _ctx.get_node(next_ptr);
                node.r_lock();
                uint32 n = node.size();
                double diff = n - avg_node_size;
                std_dev += diff * diff;
                next_ptr = node.next();
                node.r_unlock_destroy();
            } while (BlockNumberIsValid(next_ptr));
            std_dev = sqrt(std_dev / nnode);
            ereport(err_level, (
                errmsg("B+ tree level %u: %u nodes, %u elements, avg %.2f, std dev %.2f",
                       level, nnode, nelement, avg_node_size, std_dev)));
            node = _ctx.get_node(ptr);
            node.r_lock();
            ptr = node.get_ptr(p_firstdatakey(node));
            node.r_unlock_destroy();
        } while (level > 0);
    }
    void log_all_index(int err_level)
    {
        if (err_level < client_min_messages && err_level < log_min_messages) {
            return;
        }
        _ctx.r_lock();
        NodePtr ptr = _ctx.get_root_ptr();
        _ctx.r_unlock();
        uint16 level;
        const size_t index_magnitude_size = _ctx.index_magnitude()->size();
        do {
            NodeImpl node = _ctx.get_node(ptr);
            node.r_lock();
            level = node.level();
            uint32 nnode[index_magnitude_size] = {0};
            uint32 nindex[index_magnitude_size] = {0};
            for (size_t i = 0; i < index_magnitude_size; ++i) {
                BlockNumber blkno = node.get_index_ptr(i);
                while (BlockNumberIsValid(blkno)) {
                    VectorIndex *idx = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), blkno);
                    idx->r_lock();
                    blkno = idx->next();
                    size_t n;
                    BlockNumber *blknos = idx->get_node_blkno(n);
                    pfree(blknos);
                    idx->r_unlock();
                    delete_vector_index(idx);
                    nnode[i] += n;
                    ++nindex[i];
                }
            }
            for (size_t i = index_magnitude_size - 1;; --i) {
                if (nnode[i] != 0) {
                    ereport(err_level,
                        (errmsg("B+ tree level %u index %zu: avg %lf nodes, %u indexes",
                                level, i, double(nnode[i]) / nindex[i], nindex[i])));
                }
                if (i == 0) {
                    break;
                }
            }
            ptr = node.get_ptr(p_firstdatakey(node));
            node.r_unlock_destroy();
        } while (level > 0);
    }

    NodeImpl search(const ScanKeyType &scan_data, OffsetNumber &offset)
    {
        PathStack path;
        NodeImpl res = iterate_to_leaf(scan_data,
            [this, &scan_data, &path](NodeImpl &node) {
                node.r_lock();
                _move_right(node, scan_data, path, false, false);
            },
            [](NodeImpl &node, OffsetNumber) {
                node.r_unlock();
            });
        ann_helper::optional_destroy(path);
        offset = res.item_index_of(scan_data);
        return res;
    }
    void insert(Data *data) { _doinsert(data); }
    TupleDesc get_tupDesc() { return _ctx.get_tupDesc(); }
protected:
    ContextImpl _ctx;

    virtual void _split_vector_index_root(NodeImpl &new_root, NodeImpl &lnode) = 0;
    virtual void _split_vector_index_node(NodeImpl &pnode) = 0;
    virtual void _insert_data_to_vector_index(Data *data, PathStack &path) = 0;
    virtual void _insert_node_to_vector_index(NodeImpl &node) = 0;
    virtual void _merge_vector_index_node(NodeImpl &pnode) = 0;
    virtual void _recycle_vector_index(const BlockNumber indexBlkno, const size_t magnitude_level) = 0;

    template <class F1, class F2, class F3>
    NodeImpl iterate_until(NodeImpl from_node, const ScanKeyType &data,
                           F1 &&on_access, F2 &&on_leave, F3 &&stop)
    {
        static_assert(IS_INVOCABLE_R(F3, bool, const NodeImpl &),
                      "F3 must be invocable with const NodeImpl & and return bool");
        static_assert(IS_INVOCABLE(F1, NodeImpl &),
                      "F1 must be invocable with NodeImpl &");
        static_assert(IS_INVOCABLE(F2, NodeImpl &, OffsetNumber),
                      "F2 must be invocable with NodeImpl & and OffsetNumber");
        OffsetNumber i;
        on_access(from_node);
        while (!stop(from_node)) {
            i = from_node.item_index_of(data);
            NodePtr ptr = from_node.get_ptr(i);
            on_leave(from_node, i);
            from_node.destroy();
            from_node = _ctx.get_node(ptr);
            on_access(from_node);
        }
        return from_node;
    }
    template <class F1, class F2, class F3>
    NodeImpl iterate_until(const ScanKeyType &data, F1 &&on_access, F2 &&on_leave, F3 &&stop)
    {
        _ctx.r_lock();
        NodePtr ptr = _ctx.get_root_ptr();
        NodeImpl node = _ctx.get_node(ptr);
        _ctx.r_unlock();
        return iterate_until(node, data, std::forward<F1>(on_access),
            std::forward<F2>(on_leave), std::forward<F3>(stop));
    }
    template <class F1, class F2>
    inline NodeImpl iterate_to_leaf(const ScanKeyType &data, F1 &&on_access, F2 &&on_leave)
    {
        return iterate_until(data, std::forward<F1>(on_access), std::forward<F2>(on_leave),
                             [](const NodeImpl &node) { return node.is_leaf(); });
    }

    void _split_root(NodeImpl &new_root, Data *data, NodePtr lptr, NodePtr rptr)
    {
        /**
         * split root node, below is lock reasoning by pg implementation:
         * We've just split the old root page and need to create a new one.
         * In order to do this, we add a new root page to the file, then lock
         * the metadata page and update it.  This is guaranteed to be deadlock-
         * free, because all readers release their locks on the metadata page
         * before trying to lock the root, and all writers lock the root before
         * trying to lock the metadata page.  We have a write lock on the old
         * root page, so we have not introduced any cycles into the waits-for graph.
         */
        _ctx.w_lock();
        START_CRIT_SECTION();
        new_root.set_root(true);
        new_root.insert(p_hikey, data, lptr);
        new_root.insert(p_firstkey, data, rptr);
        new_root.mark_dirty();
        _ctx.set_root_ptr(new_root.ptr());
        _ctx.mark_dirty();
        END_CRIT_SECTION();
    }

    /*
     * split a page in the btree.
     *
     *  On entry, orig_node is the node to split, and is write-locked.
     *  firstright is the item index of the first item to be moved to the
     *  new right page.  newitemoff etc. tell us about the new item that
     *  must be inserted along with the data from the old page.
     *
     *  When splitting a internal node, 'child_node' is the left-sibling of the
     *  node we're inserting the downlink for.  This function will clear its
     *  INCOMPLETE_SPLIT flag.
     *
     *  Returns the new right sibling of orig_node, and write-locked.
     *  The lock on orig_node are maintained.
     */
    NodeImpl _split(NodeImpl &orig_node, NodeImpl &&child_node, OffsetNumber firstright,
                    OffsetNumber newitemoff, Data *data)
    {
        const bool isleaf = orig_node.is_leaf();

        /*
         * origpage is the original page to be split.  leftpage is a temporary
         * buffer that receives the left-sibling data, which will be copied back
         * into origpage on success.  rightpage is the new page that receives the
         * right-sibling data.	If we fail before reaching the critical section,
         * origpage hasn't been modified and leftpage is only workspace. In
         * principle we shouldn't need to worry about rightpage either, because it
         * hasn't been linked into the btree page structure; but to avoid leaving
         * possibly-confusing junk behind, we are careful to rewrite rightpage as
         * zeroes before throwing any error.
         */
        NodeImpl ltempnode = _ctx.get_temp_node(orig_node);
        ltempnode.set_root(false);
        ltempnode.set_split_end(false);
        ltempnode.set_has_garbage(false);
        /* Acquire a new node to split into */
        NodeImpl rnode = _ctx.get_new_node(ltempnode, orig_node.ptr());
        ltempnode.set_incomplete_split(true);
        ltempnode.cycle_id() = rnode.cycle_id() = _vacuum_cycleid();

        orig_node.split(ltempnode, rnode, firstright, newitemoff, data);

        _insert_node_to_vector_index(rnode);

        /*
         * We have to grab the right sibling (if any) and fix the prev pointer
         * there. We are guaranteed that this is deadlock-free since no other
         * writer will be holding a lock on that page and trying to move left, and
         * all readers release locks on a page before trying to fetch its
         * neighbors.
         */
        NodeImpl right_node;
        Assert(!right_node.valid());
        if (!orig_node.is_right_most()) {
            NodePtr right_ptr = orig_node.next();
            right_node = _ctx.get_node(right_ptr);
            right_node.w_lock();

            /*
             * Check to see if we can set the SPLIT_END flag in the right-hand
             * split page; this can save some I/O for vacuum since it need not
             * proceed to the right sibling.  We can set the flag if the right
             * sibling has a different cycleid: that means it could not be part of
             * a group of pages that were all split off from the same ancestor
             * page.  If you're confused, imagine that page A splits to A B and
             * then again, yielding A C B, while vacuum is in progress.  Tuples
             * originally in A could now be in either B or C, hence vacuum must
             * examine both pages.	But if D, our right sibling, has a different
             * cycleid then it could not contain any tuples that were in A when
             * the vacuum started.
             */
            if (right_node.cycle_id() != rnode.cycle_id()) {
                rnode.set_split_end(true);
            }
        }
        rnode.w_lock();

        /*
         * Right sibling is locked, new siblings are prepared, but original page
         * is not updated yet.
         *
         * NO EREPORT(ERROR) till right sibling is updated.  We can get away with
         * not starting the critical section till here because we haven't been
         * scribbling on the original page yet; see comments above.
         */
        START_CRIT_SECTION();

        /*
         * By here, the original data page has been split into two new halves, and
         * these are correct.  The algorithm requires that the left page never
         * move during a split, so we copy the new left page back on top of the
         * original.  Note that this is not a waste of time, since we also require
         * (in the page management code) that the center of a page always be
         * clean, and the most efficient way to guarantee this is just to compact
         * the data by reinserting it into a new left page.  (XXX the latter
         * comment is probably obsolete; but in any case it's good to not scribble
         * on the original page until we enter the critical section.)
         *
         * We need to do this before writing the WAL record, so that XLogInsert
         * can WAL log an image of the page if necessary.
         */
        _ctx.restore_destroy_temp_node(std::move(ltempnode), orig_node);
        /* leftpage, lopaque must not be used below here */
        orig_node.mark_dirty();
        rnode.mark_dirty();
        if (right_node.valid()) {
            right_node.prev() = rnode.ptr();
            right_node.mark_dirty();
        }

        if (!isleaf) {
            child_node.set_incomplete_split(false);
            child_node.mark_dirty();
        }

        /* XLOG stuff */
        if (_ctx.need_wal()) {
            /* reference nbtinsert.cpp:1596 */
            xl_hybrid_split xl_rec;
            xl_rec.is_right_most = right_node.valid();
            xl_rec.isleaf = isleaf;
            XLogBeginInsert();
            XLogRegisterBuffer(0, orig_node._buf, REGBUF_STANDARD);
            XLogRegisterBufData(0, (char *)orig_node._page, BLCKSZ);
            XLogRegisterBuffer(1, rnode._buf, REGBUF_FORCE_IMAGE);
            BlockNumber r_blkno = rnode.ptr();
            if (xl_rec.is_right_most) {
                XLogRegisterBuffer(2, right_node._buf, REGBUF_STANDARD);
                XLogRegisterBufData(2, (char *)&r_blkno, sizeof(BlockNumber));
            }
            if (!xl_rec.isleaf) {
                XLogRegisterBuffer(3, child_node._buf, REGBUF_STANDARD);
            }
            XLogRegisterData((char *)&xl_rec, sizeof(xl_hybrid_split));
            XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_SPLIT);
            PageSetLSN(orig_node._page, recptr);
            PageSetLSN(rnode._page, recptr);
            if (xl_rec.is_right_most) {
                PageSetLSN(right_node._page, recptr);
            }
            if (!xl_rec.isleaf) {
                PageSetLSN(child_node._page, recptr);
            }
        }
        END_CRIT_SECTION();

        /* release the old right sibling */
        if (!rnode.is_right_most()) {
            right_node.w_unlock_destroy();
        }
        /* release the child */
        if (!isleaf && !child_node.is_leaf()) {
            child_node.w_unlock_destroy();
        }

        /* split's done */
        return rnode;
    }

    void _insert_parent(NodeImpl &&lnode, NodeImpl &&rnode, PathStack &stack, bool is_root)
    {
        /*
         * Here we have to do something Lehman and Yao don't talk about: deal with
         * a root split and construction of a new root.  If our stack is empty
         * then we have just split a node on what had been the root level when we
         * descended the tree. If it was still the root then we perform a
         * new-root construction.  If it *wasn't* the root anymore, search to find
         * the next higher level that someone constructed meanwhile, and find the
         * right place to insert as for the normal case.
         *
         * If we have to search for the parent level, we do so by re-descending
         * from the root.  This is not super-efficient, but it's rare enough not
         * to matter.  (This path is also taken when called from WAL recovery ---
         * we have no stack in that case.)
         */
        if (is_root) {
            Assert(stack.empty());
            NodeImpl new_root = _ctx.get_new_node(lnode.level() + 1);
            /* root is set at _split_root */
            Data *data = Data::create(rnode.get_data(p_hikey));
            _split_root(new_root, data, lnode.ptr(), rnode.ptr());
            lnode.set_root(false);
            _split_vector_index_root(new_root, lnode);
            lnode.set_incomplete_split(false);
            lnode.mark_dirty();

            XLogBeginInsert();
            START_CRIT_SECTION();
            XLogRegisterBuffer(0, new_root._buf, REGBUF_FORCE_IMAGE);
            XLogRegisterBuffer(1, lnode._buf, REGBUF_STANDARD);
            XLogRegisterBuffer(2, _ctx.get_meta_buffer(), REGBUF_STANDARD);
            BlockNumber new_root_blkno = new_root.ptr();
            XLogRegisterBufData(2, (char *)&new_root_blkno, sizeof(BlockNumber));
            XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_SPLIT_ROOT);
            PageSetLSN(new_root._page, recptr);
            PageSetLSN(lnode._page, recptr);
            PageSetLSN(BufferGetPage(_ctx.get_meta_buffer()), recptr);
            END_CRIT_SECTION();

            ann_helper::optional_destroy(*data);
            pfree(data);
            lnode.w_unlock_destroy();
            rnode.w_unlock_destroy();
            _ctx.w_unlock();
            new_root.destroy();
            return;
        }
        if (stack.empty()) {
            /* Find the leftmost page at the next level up */
            NodePtr p_ptr = _get_end_point(lnode.level() + 1, false);
            /* Set up a phony stack entry pointing there */
            stack.emplace_back(p_ptr, InvalidOffsetNumber);
        }

        /* get high key from left page == lower bound for new right page */
        const Data *lhikey = lnode.get_data(p_hikey);
        /* form an index tuple that points at the new right page
         * assure that memory is properly allocated, prevent from missing log of insert parent */
        Data *new_data = Data::create(lhikey);
        new_data->set_ptr(rnode.ptr());
        rnode.w_unlock_destroy();

        NodeImpl pnode = _get_parent(stack, lnode.ptr());
        Assert(pnode.valid());
        if (!pnode.valid()) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("failed to re-find parent key in index \"%s\" for split pages %u/%u",
                                   RelationGetRelationName(_ctx.get_index()), lnode.ptr(), rnode.ptr())));
        }

        SearchEntry last_entry = stack.back();
        stack.pop_back();

        _insertonpg(new_data, std::move(pnode), std::move(lnode), stack, last_entry.offset + 1);

        ann_helper::optional_destroy(*new_data);
        pfree(new_data);
        stack.push_back(last_entry);
    }

    void iterate_to_leaf_leftmost(Vector<BlockNumber> &path)
    {
        BlockNumber left_most = _ctx.get_root_ptr();
        while(BlockNumberIsValid(left_most)) {
            NodeImpl node = _ctx.get_node(left_most);
            node.r_lock();
            const bool isleaf = node.is_leaf();
            path.emplace_back(node.ptr());
            OffsetNumber left_idx = p_firstdatakey(node);
            left_most = node.size() >= left_idx ? node.get_ptr(left_idx) : InvalidBlockNumber;
            node.r_unlock_destroy();
            if (isleaf) {
                break;
            }
        }
    }

    /*
     * Insert a tuple on a particular page in the index.
     *
     * This recursive procedure does the following things:
     *
     * On entry, we must have the correct lnode in which to do the
     * insertion, and node must be write-locked.
     *
     * When inserting to a internal node, cnode is the left-sibling of the
     * page we're inserting the downlink for. This function will clear the
     * INCOMPLETE_SPLIT flag on it, and unlock, destroy the node.
     *
     * The locking interactions in this code are critical.  You should
     * grok Lehman and Yao's paper before making any changes.  In addition,
     * you need to understand how we disambiguate duplicate keys in this
     * implementation, in order to be able to find our location using
     * L&Y "move right" operations.  Since we may insert duplicate user
     * keys, and since these dups may propagate up the tree, we use the
     * 'afteritem' parameter to position ourselves correctly for the
     * insertion on internal pages.
     */
    void _insertonpg(Data *data, NodeImpl &&lnode, NodeImpl &&cnode, PathStack &stack,
                     OffsetNumber newitemoff)
    {
        OffsetNumber firstright = InvalidOffsetNumber;
        /* child buffer must be given iff inserting on an internal page */
        Assert(lnode.is_leaf() != (cnode.valid()));
        /* The caller should've finished any incomplete splits already. */
        if (lnode.is_incomplete_split()) {
            elog(ERROR, "cannot insert to incompletely split page at %u", lnode.ptr());
        }

        if (!lnode.can_insert(data)) {
            const bool is_root = lnode.is_root();
            /* Choose the split point */
            firstright = lnode.split_loc(newitemoff, data);
            /* split the buffer into left and right halves */
            NodeImpl rnode = _split(lnode, std::move(cnode), firstright, newitemoff, data);

            /* ----------
             * By here,
             *
             *  +  our target page has been split;
             *  +  the original tuple has been inserted;
             *  +  we have write locks on both the old (left half)
             *     and new (right half) buffers, after the split; and
             *  +  we know the key we want to insert into the parent
             *     (it's the "high key" on the left child page).
             *
             * We're ready to do the parent insertion.  We need to hold onto the
             * locks for the child pages until we locate the parent, but we can
             * release them before doing the actual insertion (see Lehman and Yao
             * for the reasoning).
             * ----------
             */
            _insert_parent(std::move(lnode), std::move(rnode), stack, is_root);
            if (cnode.valid() && cnode.is_leaf()) {
                cnode.w_unlock_destroy();
            }
            return;
        }

        /* Do the update.  No ereport(ERROR) until changes are logged */
        START_CRIT_SECTION();
        lnode.insert(newitemoff, data);
        lnode.mark_dirty();

        if (cnode.valid()) {
            cnode.set_incomplete_split(false);
            cnode.mark_dirty();
        }

        /* XLOG stuff */
        if (_ctx.need_wal()) {
            /* reference nbtinsert.cpp:1115 */
            XLogBeginInsert();
            XLogRegisterData((char *)&newitemoff, sizeof(OffsetNumber));
            XLogRegisterBuffer(0, lnode._buf, REGBUF_STANDARD);
            XLogRegisterBufData(0, (char *)data, data->size());
            if (cnode.valid()) {
                XLogRegisterBuffer(1, cnode._buf, REGBUF_STANDARD);
            }
            XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_INSERT_DATA);
            PageSetLSN(lnode._page, recptr);
            if (cnode.valid()) {
                PageSetLSN(cnode._page, recptr);
            }
        }
        END_CRIT_SECTION();

        _split_vector_index_node(lnode);
        lnode.w_unlock_destroy();

        if (cnode.valid()) {
            _split_vector_index_node(cnode);
            cnode.w_unlock_destroy();
        }
    }

    void _doinsert(Data *data)
    {
        PathStack path;
        auto scan_key = _ctx.get_scan_key(data);
        NodeImpl leaf = iterate_to_leaf(scan_key,
            [this, &path, &scan_key](NodeImpl &node) {
                if (node.is_leaf()) {
                    node.w_lock();
                    _move_right(node, scan_key, path, true, false);
                } else {
                    node.r_lock();
                    _move_right(node, scan_key, path, false, false);
                    path.emplace_back(node.ptr(), 0);
                }
            },
            [&path](NodeImpl &node, OffsetNumber i) {
                node.r_unlock();
                path.back().offset = i;
            });

        /* find the position to insert */
        OffsetNumber offset = leaf.item_index_of(scan_key);
        /* do the insertion */
        _insertonpg(data, std::move(leaf), NodeImpl(), path, offset);
        /* insert data into related vector indexes */
        _insert_data_to_vector_index(data, path);

        /* be tidy */
        ann_helper::optional_destroy(path);
        ann_helper::optional_destroy(scan_key);
    }

    inline uint16 _vacuum_cycleid() { return _bt_vacuum_cycleid(_ctx.get_index()); }

    void _finish_split(NodeImpl &&node, PathStack &path)
    {
        Assert(node.is_incomplete_split());
        Assert(!node.is_right_most());
        NodeImpl rnode = _ctx.get_node(node.next());
        rnode.w_lock();
        bool was_root = false;
        if (path.empty()) {
            _ctx.r_lock();
            was_root = node.ptr() == _ctx.get_root_ptr();
            _ctx.r_unlock();
        }
        _insert_parent(std::move(node), std::move(rnode), path, was_root);
    }

    /*
     *	move right in the btree if necessary.
     *
     * When we follow a pointer to reach a page, it is possible that
     * the page has changed in the meanwhile.  If this happens, we're
     * guaranteed that the page has "split right" -- that is, that any
     * data that appeared on the page originally is either on the page
     * or strictly to the right of it.
     *
     * This routine decides whether or not we need to move right in the
     * tree by examining the high key entry on the page.  If that entry
     * is strictly less than the scankey, or <= the scankey in the nextkey=true
     * case, then we followed the wrong link and we need to move right.
     *
     * The passed scankey must be an insertion-type scankey (see nbtree/README),
     * but it can omit the rightmost column(s) of the index.
     *
     * When nextkey is false (the usual case), we are looking for the first
     * item >= scankey.  When nextkey is true, we are looking for the first
     * item strictly greater than scankey.
     *
     * If forupdate is true, we will attempt to finish any incomplete splits
     * that we encounter.  This is required when locking a target page for an
     * insertion, because we don't allow inserting on a page before the split
     * is completed.  'stack' is only used if forupdate is true.
     *
     * On entry, we have the buffer pinned and a lock of the type specified by
     * 'access'.  If we move right, we release the buffer and lock and acquire
     * the same on the right sibling.  Return value is the buffer we stop at.
     */
    bool _move_right(NodeImpl &node, const ScanKeyType &key, PathStack &path, bool exclusive_lock,
                     bool force_update)
    {
        uint32 nmoved = 0;
        for (;;) {
            if (node.is_right_most() || key < *node.get_data(p_hikey)) {
                return false;
            }
            if (force_update && node.is_incomplete_split()) {
                if (!exclusive_lock) {
                    node.r_unlock();
                    node.w_lock();
                    if (!node.is_incomplete_split()) {
                        node.w_unlock();
                        node.r_lock();
                        continue;
                    }
                }
                NodePtr cur_ptr = node.ptr();
                _finish_split(std::move(node), path);
                node = _ctx.get_node(cur_ptr);
                node.lock(exclusive_lock);
                continue;
            }
            if (node.can_ignore() || !(key < *node.get_data(p_hikey))) {
                ++nmoved;
                NodePtr next_ptr = node.next();
                node.unlock_destroy(exclusive_lock);
                node = _ctx.get_node(next_ptr);
                node.lock(exclusive_lock);
            }
        }
        Assert(!node.can_ignore());
        path.back().offset += nmoved;
        return nmoved > 0;
    }

    /*
     *  return a write locked actual parent of the child node according to the path.
     *
     * path will get modified if the parent is not the immediate parent of the child.
     * parent should be always lockable since we only lock one level at a time.
     */
    NodeImpl _get_parent(PathStack &path, NodePtr child_ptr)
    {
        NodePtr cur_ptr = path.back().ptr;
        Offset start = path.back().offset;
        for (;;) {
            NodeImpl cur_node = _ctx.get_node(cur_ptr);
            cur_node.w_lock();
            if (cur_node.is_incomplete_split()) {
                _finish_split(std::move(cur_node), path);
                continue;
            }
            if (!cur_node.can_ignore()) {
                OffsetNumber offnum;
                OffsetNumber minoff = p_firstdatakey(cur_node);
                OffsetNumber maxoff = cur_node.size();
                if (start < minoff) {
                    start = minoff;
                } else if (start > maxoff) {
                    start = maxoff;
                }
                for (offnum = start; offnum <= maxoff; offnum = OffsetNumberNext(offnum)) {
                    if (cur_node.get_ptr(offnum) == child_ptr) {
                        path.back() = {cur_ptr, offnum};
                        /* TD: nbtinsert impl has f*king global split flag unset here */
                        return cur_node;
                    }
                }
                for (offnum = OffsetNumberPrev(start); offnum >= minoff; offnum = OffsetNumberPrev(offnum)) {
                    if (cur_node.get_ptr(offnum) == child_ptr) {
                        path.back() = {cur_ptr, offnum};
                        return cur_node;
                    }
                }
            }

            cur_ptr = cur_node.next();
            cur_node.w_unlock_destroy();
            if (cur_ptr == _ctx.invalid_ptr()) {
                return NodeImpl();
            }
        }
    }

    NodePtr _get_end_point(uint16 level, bool right_end)
    {
        _ctx.r_lock();
        NodePtr res = _ctx.get_root_ptr();
        _ctx.r_unlock();
        NodeImpl node;
        for (;;) {
            node = _ctx.get_node(res);
            node.r_lock();
            if (level >= node.level()) {
                node.r_unlock_destroy();
                return res;
            }
            while (right_end ? !node.is_right_most() : !node.is_left_most()) {
                res = right_end ? node.next() : node.prev();
                node.r_unlock_destroy();
                node = _ctx.get_node(res);
                node.r_lock();
            }
            res = right_end ? node.get_ptr(node.size()) : node.get_ptr(p_firstdatakey(node));
            node.r_unlock_destroy();
        }
    }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_H */
