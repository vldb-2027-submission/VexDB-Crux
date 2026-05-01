/**
 * Copyright ...
 * B+ Tree vacuum implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_VACUUM_H
#define DISKANN_CONTAINER_BPLUSTREE_VACUUM_H

#include "access/hybridann/bplustree/bplustree.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "access/transam.h"
#include "commands/vacuum.h"
#include "utils/elog.h"
#include "access/annvector/xlog/log_manager.h"

namespace disk_container {

class VacuumBPlusTree : public virtual BPlusTreeImpl {
    using super = BPlusTreeImpl;
    /* Working state needed by btvacuumpage */
    struct BTVacState {
        IndexVacuumInfo *info;
        IndexBulkDeleteResult *stats;
        IndexBulkDeleteCallback callback;
        void *callback_state;
        BTCycleId cycleid;
        BlockNumber lastBlockVacuumed; /* highest blkno actually vacuumed */
        BlockNumber lastBlockLocked;   /* highest blkno we've cleanup-locked */
        BlockNumber totFreePages;      /* true total # of free pages */
        MemoryContext pagedelcontext;
    };

public:
    using super::BPlusTreeImpl; /* inherit constructor */

    IndexBulkDeleteResult* bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, const void *callback_state)
    {
        Relation rel = info->index;
        BTCycleId cycleid;

        /* Establish the vacuum cycle ID to use for this scan */
        /* The ENSURE stuff ensures we clean up shared memory on failure */
        PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
        {
            cycleid = _bt_start_vacuum(rel);

            vacuumscan(info, stats, callback, (void *)callback_state, cycleid);
        }
        PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
        _bt_end_vacuum(rel);

        return stats;
    }

private:

    void collect_all_nodes(List** blknolist, BlockNumber &maxBlkno)
    {
        Vector<BlockNumber> path;
        iterate_to_leaf_leftmost(path);
        for (size_t i = 0; i < path.size(); ++i) {
            BlockNumber blkno = path[i];
            while (BlockNumberIsValid(blkno)) {
                *blknolist = lappend_oid(*blknolist, blkno);
                if (blkno > maxBlkno) {
                    maxBlkno = blkno;
                }
                NodeImpl node = _ctx.get_node(blkno);
                node.r_lock();
                blkno = node.next();
                node.r_unlock_destroy();
            }
        }
    }

    /*
    * Subroutine to find the parent of the branch we're deleting.  This climbs
    * up the tree until it finds a page with more than one child, i.e. a page
    * that will not be totally emptied by the deletion.  The chain of pages below
    * it, with one downlink each, will form the branch that we need to delete.
    *
    * If we cannot remove the downlink from the parent, because it's the
    * rightmost entry, returns false.  On success, *topparent and *topoff are set
    * to the buffer holding the parent, and the offset of the downlink in it.
    * *topparent is write-locked, the caller is responsible for releasing it when
    * done.  *target is set to the topmost page in the branch to-be-deleted, i.e.
    * the page whose downlink *topparent / *topoff point to, and *rightsib to its
    * right sibling.
    *
    * "child" is the leaf page we wish to delete, and "stack" is a search stack
    * leading to it (it actually leads to the leftmost leaf page with a high key
    * matching that of the page to be deleted in !heapkeyspace indexes).  Note
    * that we will update the stack entry(s) to reflect current downlink
    * positions --- this is essentially the same as the corresponding step of
    * splitting, and is not expected to affect caller.  The caller should
    * initialize *target and *rightsib to the leaf page and its right sibling.
    *
    * Note: it's OK to release page locks on any internal pages between the leaf
    * and *topparent, because a safe deletion can't become unsafe due to
    * concurrent activity.  An internal page can only acquire an entry if the
    * child is split, but that cannot happen as long as we hold a lock on the
    * leaf.
    */
    bool lock_branch_parent(PathStack &stack, DiskNodeImpl &topparent,
                            OffsetNumber *topoff, BlockNumber *target, BlockNumber *rightsib)
    {
        BlockNumber parent;
        OffsetNumber poffset;
        OffsetNumber maxoff;
        BlockNumber leftsib;

        /*
        * Locate the downlink of "child" in the parent, updating the stack entry
        * if needed.  This is how !heapkeyspace indexes deal with having
        * non-unique high keys in leaf level pages.  Even heapkeyspace indexes
        * can have a stale stack due to insertions into the parent.
        */
        NodeImpl pnode = _get_parent(stack, *target);
        if (!pnode.valid()) {
            return false;
        }
        parent = stack.back().ptr;
        poffset = stack.back().offset;

        maxoff = pnode.size();
        /*
        * If the target is the rightmost child of its parent, then we can't
        * delete, unless it's also the only child.
        */
        if (poffset >= maxoff) {
            /* It's rightmost child... */
            if (poffset == p_firstdatakey(pnode)) {
                /*
                * It's only child, so safe if parent would itself be removable.
                * We have to check the parent itself, and then recurse to test
                * the conditions at the parent's parent.
                */
                if (pnode.is_right_most() || pnode.is_root() || pnode.is_incomplete_split()) {
                    pnode.w_unlock_destroy();
                    return false;
                }

                *target = parent;
                *rightsib = pnode.next();
                leftsib = pnode.prev();

                // _bt_relbuf(rel, pbuf);
                pnode.w_unlock_destroy();

                /*
                * Like in _bt_pagedel, check that the left sibling is not marked
                * with INCOMPLETE_SPLIT flag.  That would mean that there is no
                * downlink to the page to be deleted, and the page deletion
                * algorithm isn't prepared to handle that.
                */
                if (BlockNumberIsValid(leftsib)) {
                    DiskNodeImpl lnode(_ctx.get_index(), leftsib);
                    lnode.r_lock();
                    /*
                    * If the left sibling was concurrently split, so that its
                    * next-pointer doesn't point to the current page anymore, the
                    * split that created the current page must be completed. (We
                    * don't allow splitting an incompletely split page again
                    * until the previous split has been completed)
                    */
                    if (lnode.next() == parent && lnode.is_incomplete_split()) {
                        lnode.r_unlock_destroy();
                        return false;
                    }
                    lnode.r_unlock_destroy();
                }
                stack.pop_back();
                return lock_branch_parent(stack, topparent, topoff, target, rightsib);
            } else {
                /* Unsafe to delete */
                pnode.w_unlock_destroy();
                return false;
            }
        } else {
            /* Not rightmost child, so safe to delete */
            topparent = pnode;
            *topoff = poffset;
            return true;
        }
    }



    /*
    * First stage of page deletion.  Remove the downlink to the top of the
    * branch being deleted, and mark the leaf page as half-dead.
    */
    bool mark_page_halfdead(DiskNodeImpl node, PathStack &stack)
    {
        BlockNumber leafblkno;
        BlockNumber leafrightsib;
        BlockNumber target;
        BlockNumber rightsib;
        DiskNodeImpl topparent;
        OffsetNumber topoff;
        IndexTupleData trunctuple;
        errno_t rc;

        Assert(!node.is_right_most() && !node.is_root() && !node.is_deleted() && !node.is_half_dead() &&
            node.is_leaf() && p_firstdatakey(node) > node.size());

        /*
        * Save info about the leaf page.
        */
        leafblkno = node.ptr();
        leafrightsib = node.next();

        /*
        * Before attempting to lock the parent page, check that the right sibling
        * is not in half-dead state.  A half-dead right sibling would have no
        * downlink in the parent, which would be highly confusing later when we
        * delete the downlink that follows the current page's downlink. (I
        * believe the deletion would work correctly, but it would fail the
        * cross-check we make that the following downlink points to the right
        * sibling of the delete page.)
        */
        DiskNodeImpl rightNode(_ctx.get_index(), leafrightsib);
        rightNode.r_lock();
        if (rightNode.is_half_dead()) {
            ereport(DEBUG1, (errcode(ERRCODE_LOG),
                errmsg("could not delete page %u because its right sibling %u is half-dead",
                        leafblkno, leafrightsib)));
            rightNode.r_unlock_destroy();
            return false;
        }
        rightNode.r_unlock_destroy();

        /*
        * We cannot delete a page that is the rightmost child of its immediate
        * parent, unless it is the only child --- in which case the parent has to
        * be deleted too, and the same condition applies recursively to it. We
        * have to check this condition all the way up before trying to delete,
        * and lock the final parent of the to-be-deleted subtree.
        *
        * However, we won't need to repeat the above _bt_is_page_halfdead() check
        * for parent/ancestor pages because of the rightmost restriction. The
        * leaf check will apply to a right "cousin" leaf page rather than a
        * simple right sibling leaf page in cases where we actually go on to
        * perform internal page deletion. The right cousin leaf page is
        * representative of the left edge of the subtree to the right of the
        * to-be-deleted subtree as a whole.  (Besides, internal pages are never
        * marked half-dead, so it isn't even possible to directly assess if an
        * internal page is part of some other to-be-deleted subtree.)
        */
        rightsib = leafrightsib;
        target = leafblkno;
        if (!lock_branch_parent(stack, topparent, &topoff, &target, &rightsib)) {
            return false;
        }

        /*
        * Check that the parent-page index items we're about to delete/overwrite
        * contain what we expect.  This can fail if the index has become corrupt
        * for some reason.  We want to throw any error before entering the
        * critical section --- otherwise it'd be a PANIC.
        *
        * The test on the target item is just an Assert because
        * _bt_lock_branch_parent should have guaranteed it has the expected
        * contents.  The test on the next-child downlink is known to sometimes
        * fail in the field, though.
        */

    #ifdef USE_ASSERT_CHECKING
        Assert(topparent.get_ptr(topoff) == target);
    #endif

        if (topparent.get_ptr(OffsetNumberNext(topoff)) != rightsib) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                errmsg("right sibling %u of block %u is not next child of block %u in index \"%s\"",
                rightsib, target, topparent.ptr(), RelationGetRelationName(_ctx.get_index()))));
        }

        /*
        * Any insert which would have gone on the leaf block will now go to its
        * right sibling.
        */
        PredicateLockPageCombine(_ctx.get_index(), leafblkno, leafrightsib);

        /* No ereport(ERROR) until changes are logged */
        START_CRIT_SECTION();

        /*
        * Update parent.  The normal case is a tad tricky because we want to
        * delete the target's downlink and the *following* key.  Easiest way is
        * to copy the right sibling's downlink over the target downlink, and then
        * delete the following item.
        */
        topparent.get_data(topoff)->set_ptr(rightsib);


        topparent.delete_tuple(OffsetNumberNext(topoff));
        
        _merge_vector_index_node(topparent);

        /*
        * Mark the leaf page as half-dead, and stamp it with a pointer to the
        * highest internal page in the branch we're deleting.  We use the tid of
        * the high key to store it.
        */
        node.set_half_dead(true);

        node.delete_tuple(P_HIKEY);
        Assert(node.size() == 0);
        rc = memset_s(&trunctuple, sizeof(IndexTupleData), 0, sizeof(IndexTupleData));
        securec_check(rc, "\0", "\0");
        trunctuple.t_info = sizeof(IndexTupleData);
        ItemPointerSetBlockNumber(&trunctuple.t_tid, target != leafblkno ? target : InvalidBlockNumber);

        node.insert(P_HIKEY, (Data*)&trunctuple);

        /* Must mark buffers dirty before XLogInsert */
        topparent.mark_dirty();
        node.mark_dirty();


        /* XLOG stuff */
        if (RelationNeedsWAL(_ctx.get_index())) {
            xl_hybrid_mark_page_harfhead xl_rec;
            xl_rec.topoff = topoff;
            xl_rec.rightsib = rightsib;
            XLogBeginInsert();
            XLogRegisterBuffer(0, topparent._buf, REGBUF_STANDARD);
            XLogRegisterBufData(0, (char *)&xl_rec, sizeof(xl_hybrid_mark_page_harfhead));
            XLogRegisterBuffer(1, node._buf, REGBUF_STANDARD);
            XLogRegisterBufData(1, (char *)&trunctuple, sizeof(IndexTupleData));
            XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD);
            PageSetLSN(topparent._page, recptr);
            PageSetLSN(node._page, recptr);
        }

        END_CRIT_SECTION();

        topparent.w_unlock_destroy();
        return true;
    }

    /*
    * Unlink a page in a branch of half-dead pages from its siblings.
    *
    * If the leaf page still has a downlink pointing to it, unlinks the highest
    * parent in the to-be-deleted branch instead of the leaf page.  To get rid
    * of the whole branch, including the leaf page itself, iterate until the
    * leaf page is deleted.
    *
    * Returns 'false' if the page could not be unlinked (shouldn't happen).
    * If the (new) right sibling of the page is empty, *rightsib_empty is set
    * to true.
    *
    * Must hold pin and lock on leafbuf at entry (read or write doesn't matter).
    * On success exit, we'll be holding pin and write lock.  On failure exit,
    * we'll release both pin and lock before returning (we define it that way
    * to avoid having to reacquire a lock we already released).
    */
    bool unlink_halfdead_page(DiskNodeImpl leafnode, bool *rightsib_empty)
    {
        BlockNumber leafblkno = leafnode.ptr();
        BlockNumber leafleftsib;
        BlockNumber leafrightsib;
        BlockNumber target;
        BlockNumber leftsib;
        BlockNumber rightsib;
        DiskNodeImpl lnode;
        DiskNodeImpl rnode;
        DiskNodeImpl node;
        BlockNumber nextchild;

        Assert(leafnode.is_leaf() && leafnode.is_half_dead());

        /*
        * Remember some information about the leaf page.
        */
        target = leafnode.get_ptr(P_HIKEY);

        leafleftsib = leafnode.prev();
        leafrightsib = leafnode.next();

        leafnode.unlock();

        /*
        * Check here, as calling loops will have locks held, preventing
        * interrupts from being processed.
        */
        CHECK_FOR_INTERRUPTS();

        /*
        * If the leaf page still has a parent pointing to it (or a chain of
        * parents), we don't unlink the leaf page yet, but the topmost remaining
        * parent in the branch.  Set 'target' and 'buf' to reference the page
        * actually being unlinked.
        */
        if (target != InvalidBlockNumber) {
            Assert(target != leafblkno);

            /* fetch the block number of the topmost parent's left sibling */
            node = _ctx.get_node(target);
            node.r_lock();
            leftsib = node.prev();
            /*
            * To avoid deadlocks, we'd better drop the target page lock before
            * going further.
            */
            node.r_unlock();
        } else {
            target = leafblkno;
            node = leafnode;
            leftsib = leafleftsib;
        }

        /*
        * We have to lock the pages we need to modify in the standard order:
        * moving right, then up.  Else we will deadlock against other writers.
        *
        * So, first lock the leaf page, if it's not the target.  Then find and
        * write-lock the current left sibling of the target page.  The sibling
        * that was current a moment ago could have split, so we may have to move
        * right.  This search could fail if either the sibling or the target page
        * was deleted by someone else meanwhile; if so, give up.  (Right now,
        * that should never happen, since page deletion is only done in VACUUM
        * and there shouldn't be multiple VACUUMs concurrently on the same
        * table.)
        */
        if (target != leafblkno) {
            leafnode.w_lock();
        }
        if (BlockNumberIsValid(leftsib)) {
            lnode = _ctx.get_node(leftsib);
            lnode.w_lock();
            while (lnode.is_deleted() || lnode.next() != target) {
                /* step right one page */
                leftsib = lnode.next();
                lnode.w_unlock_destroy();

                /*
                * It'd be good to check for interrupts here, but it's not easy to
                * do so because a lock is always held. This block isn't
                * frequently reached, so hopefully the consequences of not
                * checking interrupts aren't too bad.
                */

                if (!BlockNumberIsValid(leftsib)) {
                    ereport(LOG, (
                        errmsg("no left sibling (concurrent deletion?) of block %u in \"%s\"",
                               target, RelationGetRelationName(_ctx.get_index()))));
                    if (target != leafblkno) {
                        /* we have only a pin on target, but pin+lock on leafbuf */
                        node.destroy();
                        leafnode.w_unlock_destroy();
                    } else {
                        /* we have only a pin on leafbuf */
                        leafnode.destroy();

                    }
                    return false;
                }
                lnode = _ctx.get_node(leftsib);
                lnode.w_lock();
            }
        }

        /*
        * Next write-lock the target page itself.  It should be okay to take just
        * a write lock not a superexclusive lock, since no scans would stop on an
        * empty page.
        */
        node.w_lock();
        /*
        * Check page is still empty etc, else abandon deletion.  This is just for
        * paranoia's sake; a half-dead page cannot resurrect because there can be
        * only one vacuum process running at a time.
        */
        if (node.is_right_most() || node.is_root() || node.is_deleted()) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("half-dead page changed status unexpectedly in block %u of index \"%s\"",
                           target, RelationGetRelationName(_ctx.get_index()))));
        }
        if (node.prev() != leftsib) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("left link changed unexpectedly in block %u of index \"%s\"",
                           target, RelationGetRelationName(_ctx.get_index()))));
        }

        if (target == leafblkno) {
            if (p_firstdatakey(node) <= node.size() || !node.is_leaf() || !node.is_half_dead()) {
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("half-dead page changed status unexpectedly in block %u of index \"%s\"",
                           target, RelationGetRelationName(_ctx.get_index()))));
            }
            nextchild = InvalidBlockNumber;
        } else {
            if (p_firstdatakey(node) != node.size() || node.is_leaf()) {
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("half-dead page changed status unexpectedly in block %u of index \"%s\"",
                           target, RelationGetRelationName(_ctx.get_index()))));
            }

            /* remember the next non-leaf child down in the branch. */
            nextchild = node.get_ptr(p_firstdatakey(node));
            if (nextchild == leafblkno) {
                nextchild = InvalidBlockNumber;
            }
        }

        /*
        * And next write-lock the (current) right sibling.
        */
        rightsib = node.next();
        rnode = _ctx.get_node(rightsib);
        rnode.w_lock();

        if (rnode.prev() != target) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                errmsg("right sibling's left-link doesn't match: "
                       "block %u links to %u instead of expected %u in index \"%s\"",
                       rightsib, rnode.prev(), target, RelationGetRelationName(_ctx.get_index()))));
        }
        *rightsib_empty = (p_firstdatakey(rnode) > rnode.size());

        /*
        * Here we begin doing the deletion.
        */

        /* No ereport(ERROR) until changes are logged */
        START_CRIT_SECTION();

        for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
            if (BlockNumberIsValid(node.index_ptr()[i])) {
                VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), node.index_ptr()[i]);
                index->remove_node_blkno(node.ptr());
                if (index->node_empty()) {
                    index->set_leftmost_node_blkno(InvalidBlockNumber);
                    if (lnode.valid()  && lnode.index_ptr()[i] != rnode.index_ptr()[i]) {
                        VectorIndex *lindex = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), lnode.index_ptr()[i]);
                        lindex->update_next_index_meta_blkno(rnode.index_ptr()[i], true);
                        delete_vector_index(lindex);
                    }
                    _recycle_vector_index(index->ptr(), i);
                } else {
                    if(node.ptr() == index->get_leftmost_node()) {
                        Assert(rnode.index_ptr()[i] == index->ptr());
                        index->set_leftmost_node_blkno(rightsib);
                    }
                }
                delete_vector_index(index);
            }
        }
        
        /*
        * Update siblings' side-links.  Note the target page's side-links will
        * continue to point to the siblings.  Asserts here are just rechecking
        * things we already verified above.
        */
        if (lnode.valid()) {
            Assert(lnode.next() == target);
            lnode.next() = rightsib;
        }

        Assert(rnode.prev() == target);
        rnode.prev() = leftsib;

        /*
        * If we deleted a parent of the targeted leaf page, instead of the leaf
        * itself, update the leaf to point to the next remaining child in the
        * branch.
        */
        if (target != leafblkno) {
            leafnode.get_data(P_HIKEY)->set_ptr(nextchild);
        }

        /*
        * Mark the page itself deleted.  It can be recycled when all current
        * transactions are gone.  Storing GetTopTransactionId() would work, but
        * we're in VACUUM and would not otherwise have an XID.  Having already
        * updated links to the target, ReadNewTransactionId() suffices as an
        * upper bound.  Any scan having retained a now-stale link is advertising
        * in its PGXACT an xmin less than or equal to the value we read here.  It
        * will continue to do so, holding back RecentGlobalXmin, for the duration
        * of that scan.
        */
        

        node.set_half_dead(false);
        node.set_deleted(true);
        node.set_xact(ReadNewTransactionId());

        FreeSpace<BlockNumber> unlinked_pages(_ctx.get_index(), _ctx.get_unlinkedPages_metablkno());
        unlinked_pages.insert(&target, 1);
        //elog(WARNING, "unlink half dead page, put blkno:%u to unlinked_pages store", node.ptr());
        unlinked_pages.destroy();
    

        /* Must mark buffers dirty before XLogInsert */
        rnode.mark_dirty();
        node.mark_dirty();
        if (lnode.valid()) {
            lnode.mark_dirty();
        }
        if (target != leafblkno) {
            leafnode.mark_dirty();
        }

        /* XLOG stuff */
        if (RelationNeedsWAL(_ctx.get_index())) {
            xl_hybrid_unlink_harfhead_page xl_rec;
            xl_rec.leftsib = leftsib;
            xl_rec.rightsib = rightsib;
            xl_rec.nextchild = nextchild;
            xl_rec.target = target;
            xl_rec.leafblkno = leafblkno;
            xl_rec.xid = node.opaque()->xact;

            XLogBeginInsert();
            XLogRegisterData((char *)&xl_rec, sizeof(xl_hybrid_unlink_harfhead_page));
            XLogRegisterBuffer(0, rnode._buf, REGBUF_STANDARD);
            XLogRegisterBuffer(1, node._buf, REGBUF_STANDARD);
            if (lnode.valid()) {
                XLogRegisterBuffer(2, lnode._buf, REGBUF_STANDARD);
            }
            if (target != leafblkno) {
                XLogRegisterBuffer(3, leafnode._buf, REGBUF_STANDARD);
            }
            XLogRecPtr recptr = XLogInsert(RM_HYBRIDBT_ID, XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE);
            PageSetLSN(rnode._page, recptr);
            PageSetLSN(node._page, recptr);
            if(lnode.valid()) {
                PageSetLSN(lnode._page, recptr);
            }
            if (target != leafblkno) {
                PageSetLSN(leafnode._page, recptr);
            }
        }

        END_CRIT_SECTION();

        /* release siblings */
        if (lnode.valid()) {
            lnode.w_unlock_destroy();
        }

        rnode.w_unlock_destroy();

        /*
        * Release the target, if it was not the leaf block.  The leaf is always
        * kept locked.
        */
        if (target != leafblkno) {
            node.w_unlock_destroy();
        }

        return true;
    }


    /*
    * pagedel() -- Delete a page from the b-tree, if legal to do so.
    *
    * This action unlinks the page from the b-tree structure, removing all
    * pointers leading to it --- but not touching its own left and right links.
    * The page cannot be physically reclaimed right away, since other processes
    * may currently be trying to follow links leading to the page; they have to
    * be allowed to use its right-link to recover.  See nbtree/README.
    *
    * On entry, the target buffer must be pinned and locked (either read or write
    * lock is OK).  This lock and pin will be dropped before exiting.
    *
    * Returns the number of pages successfully deleted (zero if page cannot
    * be deleted now; could be more than one if parent or sibling pages were
    * deleted too). 
    *
    * NOTE: this leaks memory.  Rather than trying to clean up everything
    * carefully, it's better to run it in a temp context that can be reset
    * frequently.
    */
    int pagedel(DiskNodeImpl node)
    {
        int ndeleted = 0;
        BlockNumber rightsib;
        bool rightsib_empty = false;

        /*
        * "stack" is a search stack leading (approximately) to the target page.
        * It is initially NULL, but when iterating, we keep it to avoid
        * duplicated search effort.
        *
        * Also, when "stack" is not NULL, we have already checked that the
        * current page is not the right half of an incomplete split, i.e. the
        * left sibling does not have its INCOMPLETE_SPLIT flag set.
        */
        PathStack stack;
        bool foundstack = false;
        for (;;) {
            if (node.is_temporarily_unlinkable()) {
                ereport(LOG, (errmsg("index \"%s\"'s page(%u) is temporarily unlinkable, skip pagedel", 
                        RelationGetRelationName(_ctx.get_index()), node.ptr())));
                node.unlock_destroy();
                ann_helper::optional_destroy(stack);
                return ndeleted;
            }
            /*
            * Internal pages are never deleted directly, only as part of deleting
            * the whole branch all the way down to leaf level.
            */
            if (!node.is_leaf()) {
                /*
                * Pre-9.4 page deletion only marked internal pages as half-dead,
                * but now we only use that flag on leaf pages. The old algorithm
                * was never supposed to leave half-dead pages in the tree, it was
                * just a transient state, but it was nevertheless possible in
                * error scenarios. We don't know how to deal with them here. They
                * are harmless as far as searches are considered, but inserts
                * into the deleted keyspace could add out-of-order downlinks in
                * the upper levels. Log a notice, hopefully the admin will notice
                * and reindex.
                */
                if (node.is_half_dead()) {
                    ereport(LOG,
                            (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("index \"%s\" contains a half-dead internal page", RelationGetRelationName(_ctx.get_index())),
                            errhint("This can be caused by an interrupted VACUUM in version 9.3 or older, before upgrade. "
                                    "Please REINDEX it.")));
                }
                /* must be r_lock since w_lock is only held for leaf pages */
                node.r_unlock_destroy();
                ann_helper::optional_destroy(stack);
                return ndeleted;
            }

            /*
            * We can never delete rightmost pages nor root pages.  While at it,
            * check that page is not already deleted and is empty.
            *
            * To keep the algorithm simple, we also never delete an incompletely
            * split page (they should be rare enough that this doesn't make any
            * meaningful difference to disk usage):
            *
            * The INCOMPLETE_SPLIT flag on the page tells us if the page is the
            * left half of an incomplete split, but ensuring that it's not the
            * right half is more complicated.  For that, we have to check that
            * the left sibling doesn't have its INCOMPLETE_SPLIT flag set.  On
            * the first iteration, we temporarily release the lock on the current
            * page, and check the left sibling and also construct a search stack
            * to.  On subsequent iterations, we know we stepped right from a page
            * that passed these tests, so it's OK.
            */
            if (node.is_right_most() || node.is_root() || node.is_deleted() ||
                p_firstdatakey(node) <= node.size() ||  node.is_incomplete_split()) {
                /* Should never fail to delete a half-dead page */
                Assert(!node.is_half_dead());
                /* must be w_lock since only leaf pages and half-dead pages can enter here */
                node.w_unlock_destroy();
                ann_helper::optional_destroy(stack);
                return ndeleted;
            }

            /*
            * First, remove downlink pointing to the page (or a parent of the
            * page, if we are going to delete a taller branch), and mark the page
            * as half-dead.
            */
            if (!node.is_half_dead()) {
                /*
                * We need an approximate pointer to the page's parent page.  We
                * use a variant of the standard search mechanism to search for
                * the page's high key; this will give us a link to either the
                * current parent or someplace to its left (if there are multiple
                * equal high keys, which is possible with !heapkeyspace indexes).
                *
                * Also check if this is the right-half of an incomplete split
                * (see comment above).
                */
                if (!foundstack) {
                    Data* targetkey;
                    BlockNumber leftsib;

                    targetkey = (Data*)CopyIndexTuple((IndexTuple)node.get_data(p_hikey));

                    leftsib = node.prev();

                    /*
                    * To avoid deadlocks, we'd better drop the leaf page lock
                    * before going further.
                    */
                    node.w_unlock();

                    /*
                    * Fetch the left sibling, to check that it's not marked with
                    * INCOMPLETE_SPLIT flag.  That would mean that the page
                    * to-be-deleted doesn't have a downlink, and the page
                    * deletion algorithm isn't prepared to handle that.
                    */
                    if (!node.is_left_most()) {
                        DiskNodeImpl lnode(_ctx.get_index(), leftsib);
                        lnode.r_lock();
                        /*
                        * If the left sibling is split again by another backend,
                        * after we released the lock, we know that the first
                        * split must have finished, because we don't allow an
                        * incompletely-split page to be split again.  So we don't
                        * need to walk right here.
                        */
                        if (lnode.next() == node.ptr() && lnode.is_incomplete_split()) {
                            node.destroy();
                            lnode.r_unlock_destroy();
                            ann_helper::optional_destroy(stack);
                            return ndeleted;
                        }
                        lnode.r_unlock_destroy();
                    }

                    /* we need an insertion scan key for the search, so build one */
                    auto scan_key = _ctx.get_scan_key(targetkey);
                    NodeImpl leaf = iterate_to_leaf(scan_key,
                        [this, &stack, &scan_key](NodeImpl &node) {
                            if (node.is_leaf()) {
                                node.r_lock();
                                _move_right(node, scan_key, stack, true, false);
                            } else {
                                node.r_lock();
                                _move_right(node, scan_key, stack, false, false);
                                stack.emplace_back(node.ptr(), 0);
                            }
                        },
                        [&stack](NodeImpl &node, OffsetNumber i) {
                            node.r_unlock();
                            stack.back().offset = i;
                    });
                    foundstack = true;
                    ann_helper::optional_destroy(scan_key);

                    /* leaf may not the same as the buf, so release it, later use stack and get_parent() to precisely locate buf's parent and grand parent*/
                    leaf.r_unlock_destroy();
                    
                    /*
                    * Re-lock the leaf page, and start over, to re-check that the
                    * page can still be deleted.
                    */
                    node.w_lock();
                    continue;
                }

                if (!mark_page_halfdead(node, stack)) {
                    node.w_unlock_destroy();
                    ann_helper::optional_destroy(stack);
                    return ndeleted;
                }
            }

            /*
            * Then unlink it from its siblings.  Each call to
            * _bt_unlink_halfdead_page unlinks the topmost page from the branch,
            * making it shallower.  Iterate until the leaf page is gone.
            */
            rightsib_empty = false;
            while (node.is_half_dead()) {
                /* will check for interrupts, once lock is released */
                if (!unlink_halfdead_page(node, &rightsib_empty)) {
                    /* _bt_unlink_halfdead_page already released buffer */
                    ann_helper::optional_destroy(stack);
                    return ndeleted;
                }
                ndeleted++;
            }

            rightsib = node.next();

            node.unlock_destroy();

            /*
            * Check here, as calling loops will have locks held, preventing
            * interrupts from being processed.
            */
            CHECK_FOR_INTERRUPTS();

            /*
            * The page has now been deleted. If its right sibling is completely
            * empty, it's possible that the reason we haven't deleted it earlier
            * is that it was the rightmost child of the parent. Now that we
            * removed the downlink for this page, the right sibling might now be
            * the only child of the parent, and could be removed. It would be
            * picked up by the next vacuum anyway, but might as well try to
            * remove it now, so loop back to process the right sibling.
            */
            if (!rightsib_empty) {
                break;
            }

            node = _ctx.get_node(rightsib);
            node.w_lock();
        }

        ann_helper::optional_destroy(stack);

        return ndeleted;
    }

    /*
    * vacuumpage --- VACUUM one page
    *
    * This processes a single page for vacuumscan().  In some cases we
    * must go back and re-examine previously-scanned pages; this routine
    * recurses when necessary to handle that case.
    *
    * blkno is the page to process.  orig_blkno is the highest block number
    * reached by the outer btvacuumscan loop (the same as blkno, unless we
    * are recursing to re-examine a previous page).
    */
    
    void vacuumpage(BTVacState *vstate, BlockNumber blkno, BlockNumber orig_blkno)
    {
        IndexVacuumInfo *info = vstate->info;
        IndexBulkDeleteResult *stats = vstate->stats;
        IndexBulkDeleteCallback callback = vstate->callback;
        void *callback_state = vstate->callback_state;
        Relation rel = info->index;
        bool delete_now = false;
        BlockNumber recurse_to;

    restart:
        delete_now = false;
        recurse_to = InvalidBlockNumber;

        /* call vacuum_delay_point while not holding any buffer lock */
        vacuum_delay_point();

        DiskNodeImpl node(rel, blkno, info->strategy);
        _bt_checkbuffer_valid(rel, node._buf);
        node.r_lock();
        if (!PageIsNew(node._page)) {
            node.checkpage(rel);
        }

        /*
        * If we are recursing, the only case we want to do anything with is a
        * live leaf page having the current vacuum cycle ID.  Any other state
        * implies we already saw the page (eg, deleted it as being empty).
        */
        if (blkno != orig_blkno) {
            if (node.page_recyclable() || node.ignore() || !node.is_leaf() ||
                node.cycle_id() != vstate->cycleid) {
                node.r_unlock_destroy();
                return;
            }
        }

        /* Page is valid, see what to do with it */
        if (node.page_recyclable()) {
            /* Okay to recycle this page */
            RecordFreeIndexPage(rel, blkno);
            vstate->totFreePages++;
            stats->pages_deleted++;
        } else if (node.is_deleted()) {
            /* Already deleted, but can't recycle yet */
            stats->pages_deleted++;
        } else if (node.is_half_dead()) {
            /* Half-dead, try to delete */
            delete_now = true;
        } else if (node.is_leaf()) {
            // Data* data = node.get_data(p_firstdatakey(node));
            // bool isnull;
            //Datum col2 = index_getattr((IndexTuple)data, 2, _ctx.get_tupDesc(), &isnull);
            //elog(WARNING, "vacuumpage, leaf page blkno:%u, left key id is:%ld", node.ptr(),DatumGetInt64(col2));
            OffsetNumber deletable[MaxIndexTuplesPerPage];
            int num_deletable = 0;
            int num_dead_heap_tids = 0;
            int num_live_heap_tids = 0;
            OffsetNumber offnum, minoff, maxoff;

            /*
            * Trade in the initial read lock for a super-exclusive write lock on
            * this page.  We must get such a lock on every leaf page over the
            * course of the vacuum scan, whether or not it actually contains any
            * deletable tuples --- see nbtree/README.
            */
            node.r_unlock();
            LockBufferForCleanup(node._buf);

            /*
            * Remember highest leaf page number we've taken cleanup lock on; see
            * notes in btvacuumscan
            */
            if (blkno > vstate->lastBlockLocked) {
                vstate->lastBlockLocked = blkno;
            }

            /*
            * Check whether we need to recurse back to earlier pages.	What we
            * are concerned about is a page split that happened since we started
            * the vacuum scan.  If the split moved some tuples to a lower page
            * then we might have missed 'em.  If so, set up for tail recursion.
            * (Must do this before possibly clearing btpo_cycleid below!)
            */
            if (vstate->cycleid != 0 &&  node.cycle_id() == vstate->cycleid && !(node.is_split_end()) &&
                !node.is_right_most() && node.next() < orig_blkno) {
                recurse_to = node.next();
            }

            /*
            * Scan over all items to see which ones need deleted according to the
            * callback function.
            */
            minoff = p_firstdatakey(node);
            maxoff = node.size();
            if (callback) {
                for (offnum = minoff; offnum <= maxoff; offnum = OffsetNumberNext(offnum)) {
                    IndexTuple itup = (IndexTuple)node.get_data(offnum);
                    ItemPointer htup = &(itup->t_tid);

                    /*
                    * During Hot Standby we currently assume that
                    * XLOG_BTREE_VACUUM records do not produce conflicts. That is
                    * only true as long as the callback function depends only
                    * upon whether the index tuple refers to heap tuples removed
                    * in the initial heap scan. When vacuum starts it derives a
                    * value of OldestXmin. Backends taking later snapshots could
                    * have a RecentGlobalXmin with a later xid than the vacuum's
                    * OldestXmin, so it is possible that row versions deleted
                    * after OldestXmin could be marked as killed by other
                    * backends. The callback function *could* look at the index
                    * tuple state in isolation and decide to delete the index
                    * tuple, though currently it does not. If it ever did, we
                    * would need to reconsider whether XLOG_BTREE_VACUUM records
                    * should cause conflicts. If they did cause conflicts they
                    * would be fairly harsh conflicts, since we haven't yet
                    * worked out a way to pass a useful value for
                    * latestRemovedXid on the XLOG_BTREE_VACUUM records. This
                    * applies to *any* type of index that marks index tuples as
                    * killed.
                    */
                    Oid partOid = InvalidOid;
                    int2 bktId = InvalidBktId;
                    if (RelationIsGlobalIndex(rel)) {
                        partOid = index_getattr_tableoid(rel, itup);
                    }
                    if (RelationIsCrossBucketIndex(rel)) {
                        bktId = index_getattr_bucketid(rel, itup);
                    }

                    if (callback(htup, callback_state, partOid, bktId)) {
                        deletable[num_deletable++] = offnum;
                        num_dead_heap_tids++;
                    } else {
                        num_live_heap_tids++;
                    }
                }
            }

            if (num_deletable > 0) {
                /*
                * Notice that the issued XLOG_BTREE_VACUUM WAL record includes an
                * instruction to the replay code to get cleanup lock on all pages
                * between the previous lastBlockVacuumed and this page.  This
                * ensures that WAL replay locks all leaf pages at some point.
                *
                * Since we can visit leaf pages out-of-order when recursing,
                * replay might end up locking such pages an extra time, but it
                * doesn't seem worth the amount of bookkeeping it'd take to avoid
                * that.
                */
                Assert(num_dead_heap_tids >= Max(num_deletable, 1));

                Assert(num_deletable > 0);
                node.delitems_vacuum(deletable, num_deletable, vstate->lastBlockVacuumed);

                /*
                * Remember highest leaf page number we've issued a
                * XLOG_BTREE_VACUUM WAL record for.
                */
                if (blkno > vstate->lastBlockVacuumed) {
                    vstate->lastBlockVacuumed = blkno;
                }

                stats->tuples_removed += num_dead_heap_tids;
                /* must recompute maxoff */
                maxoff = node.size();

            } else {
                Assert(num_dead_heap_tids == 0);
                /*
                * If the page has been split during this vacuum cycle, it seems
                * worth expending a write to clear btpo_cycleid even if we don't
                * have any deletions to do.  (If we do, _bt_delitems_vacuum takes
                * care of this.)  This ensures we won't process the page again.
                * We treat this like a hint-bit update because there's no need to
                * WAL-log it.
                */
                if (vstate->cycleid != 0 && node.cycle_id() == vstate->cycleid) {
                    node.cycle_id() = 0;
                    MarkBufferDirtyHint(node._buf, true);
                }
            }

            /*
            * If it's now empty, try to delete; else count the live tuples. We
            * don't delete when recursing, though, to avoid putting entries into
            * freePages out-of-order (doesn't seem worth any extra code to handle
            * the case).
            */
            if (minoff > maxoff) {
                delete_now = (blkno == orig_blkno);
            } else if (callback) {
                stats->num_index_tuples += num_live_heap_tids;
            } else {
                stats->num_index_tuples += maxoff - minoff + 1;
            }

            Assert(!delete_now || num_live_heap_tids == 0);
        }

        if (delete_now) {
            /* Run pagedel in a temp context to avoid memory leakage */
            MemoryContextReset(vstate->pagedelcontext);
            MemoryContext oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

            int ndel = pagedel(node);
            if (ndel) {
                /* count only this page, else may double-count parent */
                stats->pages_deleted++;
            }

            MemoryContextSwitchTo(oldcontext);
            /* pagedel released buffer, so we shouldn't */
        } else {
            node.unlock_destroy(); 
        }

        /*
        * This is really tail recursion, but if the compiler is too stupid to
        * optimize it as such, we'd eat an uncomfortably large amount of stack
        * space per recursion level (due to the deletable[] array). A failure is
        * improbable since the number of levels isn't likely to be large ... but
        * just in case, let's hand-optimize into a loop.
        */
        if (BlockNumberIsValid(recurse_to)) {
            blkno = recurse_to;
            goto restart;
        }
    }

    /*
    * btvacuumscan --- scan the index for VACUUMing purposes
    *
    * This combines the functions of looking for leaf tuples that are deletable
    * according to the vacuum callback, looking for empty pages that can be
    * deleted, and looking for old deleted pages that can be recycled.  Both
    * btbulkdelete and btvacuumcleanup invoke this (the latter only if no
    * btbulkdelete call occurred).
    *
    * The caller is responsible for initially allocating/zeroing a stats struct
    * and for obtaining a vacuum cycle ID if necessary.
    */
    void vacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
                            void *callback_state, BTCycleId cycleid)
    {
        //elog(WARNING, "enter vacuumscan");
        Relation rel = _ctx.get_index();
        BTVacState vstate;

        /*
        * Reset counts that will be incremented during the scan; needed in case
        * of multiple scans during a single VACUUM command
        */
        stats->estimated_count = false;
        stats->num_index_tuples = 0;
        stats->pages_deleted = 0;

        /* Set up info to pass down to btvacuumpage */
        vstate.info = info;
        vstate.stats = stats;
        vstate.callback = callback;
        vstate.callback_state = callback_state;
        vstate.cycleid = cycleid;
        vstate.lastBlockVacuumed = DISKANN_METAPAGE_BLKNO; /* Initialise at first block */
        vstate.lastBlockLocked = DISKANN_METAPAGE_BLKNO;
        vstate.totFreePages = 0;

        /* Create a temporary memory context to run _bt_pagedel in */
        vstate.pagedelcontext = AllocSetContextCreate(CurrentMemoryContext, "_bplustree_pagedel",
                                                      ALLOCSET_DEFAULT_SIZES);

        /*
        * The outer loop iterates over all index pages except the metapage, in
        * physical order (we hope the kernel will cooperate in providing
        * read-ahead for speed).  It is critical that we visit all leaf pages,
        * including ones added after we start the scan, else we might fail to
        * delete some deletable tuples.  Hence, we must repeatedly check the
        * relation length.  We must acquire the relation-extension lock while
        * doing so to avoid a race condition: if someone else is extending the
        * relation, there is a window where bufmgr/smgr have created a new
        * all-zero page but it hasn't yet been write-locked by _bt_getbuf(). If
        * we manage to scan such a page here, we'll improperly assume it can be
        * recycled.  Taking the lock synchronizes things enough to prevent a
        * problem: either num_pages won't include the new page, or _bt_getbuf
        * already has write lock on the buffer and it will be fully initialized
        * before we can examine it.  (See also vacuumlazy.c, which has the same
        * issue.)	Also, we need not worry if a page is added immediately after
        * we look; the page splitting code already has write-lock on the left
        * page before it adds a right page, so we must already have processed any
        * tuples due to be moved into such a page.
        *
        * We can skip locking for new or temp relations, however, since no one
        * else could be accessing them.
        */
        BlockNumber prevMaxBlkno = DISKANN_METAPAGE_BLKNO;
        BlockNumber num_pages;
        //elog(WARNING, "relation total pages for now:%u", RelationGetNumberOfBlocks(rel));
        for (;;) {
            List* blknolist = NIL;
            ListCell* cell = NULL;
            BlockNumber newMaxBlkno = DISKANN_METAPAGE_BLKNO;
#if OUTPUT_BTVECLOG
            log_all_nodes(DEBUG1);
            log_all_index(DEBUG1);
            validate_vector_index_santity();
#endif /* OUTPUT_BTVECLOG */            
            collect_all_nodes(&blknolist, newMaxBlkno);
            // foreach (cell, blknolist) {
            //     BlockNumber blkno = lfirst_oid(cell);
            //     elog(WARNING, "collectec page's blkno:%u", blkno);
            // }
            num_pages = (BlockNumber)list_length(blknolist);

            /* Quit if we've scanned the whole btree */
            if (prevMaxBlkno >= newMaxBlkno) {
                if (blknolist != NIL) {
                    list_free_ext(blknolist);
                }
                break;
            }
            /* Iterate over pages, then loop back to recheck length */
            foreach (cell, blknolist) {
                BlockNumber blkno = lfirst_oid(cell);
                if (blkno < prevMaxBlkno) {
                    continue;
                }
                //elog(WARNING, "vacuumpage blkno:%u", blkno);
                vacuumpage(&vstate, blkno, blkno);
            }
        
            if (blknolist != NIL) {
                list_free_ext(blknolist);
            }
            prevMaxBlkno = newMaxBlkno;
        }

        FreeSpace<BlockNumber> unlinked_pages(rel, _ctx.get_unlinkedPages_metablkno());
        Vector<BlockNumber> pendingBlknos;
        BlockNumber res;
        while(unlinked_pages.pop(res)) {
            DiskNodeImpl node(rel, res, vstate.info->strategy);
            node.r_lock();
            Assert(node.is_deleted());
            /* Page is valid, see what to do with it */
            if (node.page_recyclable()) {
                /* Okay to recycle this page */
                RecordFreeIndexPage(rel, res);
                //elog(WARNING, "record blkno:%u to fsm.......", res);
                vstate.totFreePages++;
                stats->pages_deleted++;
            } else {
                pendingBlknos.emplace_back(res);
            }
            node.r_unlock_destroy();
        }
        unlinked_pages.insert(pendingBlknos.begin(), pendingBlknos.size());
        ann_helper::optional_destroy(pendingBlknos);
        unlinked_pages.destroy();

        /*
        * If the WAL is replayed in hot standby, the replay process needs to get
        * cleanup locks on all index leaf pages, just as we've been doing here.
        * However, we won't issue any WAL records about pages that have no items
        * to be deleted.  For pages between pages we've vacuumed, the replay code
        * will take locks under the direction of the lastBlockVacuumed fields in
        * the XLOG_BTREE_VACUUM WAL records.  To cover pages after the last one
        * we vacuum, we need to issue a dummy XLOG_BTREE_VACUUM WAL record
        * against the last leaf page in the index, if that one wasn't vacuumed.
        */
        if (XLogStandbyInfoActive() && vstate.lastBlockVacuumed < vstate.lastBlockLocked) {
            /*
            * The page should be valid, but we can't use _bt_getbuf() because we
            * want to use a nondefault buffer access strategy.  Since we aren't
            * going to delete any items, getting cleanup lock again is probably
            * overkill, but for consistency do that anyway.
            */
            DiskNodeImpl node(rel,  vstate.lastBlockLocked, info->strategy);

            _bt_checkbuffer_valid(rel, node._buf);
            LockBufferForCleanup(node._buf);
            node.checkpage(rel);
            node.delitems_vacuum( NULL, 0, vstate.lastBlockVacuumed);
            node.w_unlock_destroy();
        }

        MemoryContextDelete(vstate.pagedelcontext);

        /* update statistics */
        stats->num_pages = num_pages;
        stats->pages_free = vstate.totFreePages;

        //elog(WARNING, "step out vacuumscan");
    }

    void validate_vector_index_santity()
    {
        Vector<BlockNumber> path;
        iterate_to_leaf_leftmost(path);
        path.pop_back();
        for (size_t k = 0 ; k < path.size(); ++k) {
            BlockNumber blkno = path[k];
            NodeImpl node = _ctx.get_node(blkno);
            node.r_lock();
            for (size_t i = 0; i < _ctx.index_magnitude()->size(); ++i) {
                if (node.index_ptr()[i] == InvalidBlockNumber) {
                    continue;
                }

                VectorIndex *index = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), node.index_ptr()[i]);
                index->r_lock();
                const VectorIndexType itype = index->type();
    
                if (itype == VectorIndexType::None) {
                    index->r_unlock();
                    delete_vector_index(index);
                    continue;
                }

                Assert(index->get_leftmost_node() == node.ptr());
                BlockNumber next_idx = index->next();
                index->r_unlock();
                delete_vector_index(index);
    
                BlockNumber next = node.next();
                BlockNumber currentIdx = node.index_ptr()[i];
                while(BlockNumberIsValid(next)) {
                    DiskNodeImpl nextnode(_ctx.get_index(), next);
                    nextnode.r_lock();
                    if (nextnode.index_ptr()[i] != currentIdx) {
                        VectorIndex *nindex = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), nextnode.index_ptr()[i]);
                        nindex->r_lock();
                        Assert(nindex->get_leftmost_node() == nextnode.ptr());
                        Assert(next_idx == nextnode.index_ptr()[i]);
                        currentIdx = nextnode.index_ptr()[i];
                        next_idx = nindex->next();
                        nindex->r_unlock();
                        delete_vector_index(nindex);
                    }
                    next = nextnode.next();
                    nextnode.r_unlock_destroy();
                }
            }
            node.r_unlock_destroy();
        }
        ann_helper::optional_destroy(path);
    }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_VACUUM_H */
