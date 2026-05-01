/**
 * Copyright ...
 * B+ Tree construction implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_BUILD_H
#define DISKANN_CONTAINER_BPLUSTREE_BUILD_H

#include "access/hybridann/bplustree/bplustree.h"

namespace disk_container {
class BuildBPlusTree : public virtual BPlusTreeImpl {
    using super = BPlusTreeImpl;
    /*
     * Status record for a btree page being built.	We have one of these
     * for each active tree level.
     *
     * The reason we need to store a copy of the minimum key is that we'll
     * need to propagate it to the parent node when this page is linked
     * into its parent.  However, if the page is not a leaf page, the first
     * entry on the page doesn't need to contain a key, so we will not have
     * stored the key itself on the page.  (You might think we could skip
     * copying the minimum key on leaf pages, but actually we must have a
     * writable copy anyway because we'll poke the page's address into it
     * before passing it up to the parent...)
     */
    struct DiskBTPageState {               
        DiskNodeImpl btps_nodeImpl; /* workspace for page building */
        BTTupleData *btps_minkey;   /* copy of minimum key (first item) on page */
        OffsetNumber btps_lastoff;  /* last item offset loaded */
        uint32 btps_level;          /* tree level (0 = leaf) */
        Size btps_full;             /* "full" if less than this much free space */
        Size btps_lastextra;
        DiskBTPageState *btps_next; /* link to parent level, if any */
    };
public:
    using super::BPlusTreeImpl; /* inherit constructor */
    void buildadd(Relation heap, DiskBTPageState *state, Data* data, Size truncextra)
        { _buildadd(heap, state, data, truncextra); }
    void uppershutdown(Relation heap, DiskBTPageState *state) { _uppershutdown(heap, state); }
    NodeImpl get_new_build_node(const uint16 level) { 
        NodeImpl res = _ctx.get_new_node(level);
        /* Make the p_hikey line pointer appear allocated for build index case */
        ((PageHeader)res._page)->pd_lower += sizeof(ItemIdData);
        res.mark_dirty();
        return res;
    }
    NodeImpl get_new_build_node(NodeImpl& node) { 
        NodeImpl res = _ctx.get_new_node(node);
        /* Make the p_hikey line pointer appear allocated for build index case */
        ((PageHeader)res._page)->pd_lower += sizeof(ItemIdData);
        res.mark_dirty();
        return res;
    }

    /*
     * allocate and initialize a new DiskBTPageState.  the returned structure
     * is suitable for immediate use by _bt_buildadd.
     */
    static DiskBTPageState *diskbt_pagestate(Relation index, uint32 level, BuildBPlusTree &btree)
    {
        DiskBTPageState *state = (DiskBTPageState *)palloc0(sizeof(DiskBTPageState));
        /* create initial page for level */
        state->btps_nodeImpl = btree.get_new_build_node(level);
        state->btps_minkey = NULL;
        /* initialize lastoff so first item goes into P_FIRSTKEY */
        state->btps_lastoff = p_hikey;
        state->btps_level = level;
        state->btps_lastextra = 0;
        /* set "full" threshold based on level.  See notes at head of file. */
        if (level > 0) {
            state->btps_full = (BLCKSZ * (100 - BTREE_NONLEAF_FILLFACTOR) / 100);
        } else {
            state->btps_full = (Size)RelationGetTargetPageFreeSpaceForHybridIndex(HybridAnnGetFillFactor(index));
        }
        /* no parent level, yet */
        state->btps_next = NULL;
        return state;
    }
private:
    /*
     * Add an item to a disk page from the sort output.
     * We must be careful to observe the page layout conventions of nbtsearch.c:
     * - rightmost pages start data items at P_HIKEY instead of at P_FIRSTKEY.
     * - on non-leaf pages, the key portion of the first item need not be
     *	 stored, we should store only the link.
     *
     * A leaf page being built looks like:
     * +----------------+---------------------------------+
     * | PageHeaderData | linp0 linp1 linp2 ...			  |
     * +-----------+----+---------------------------------+
     * | ... linpN |									  |
     * +-----------+--------------------------------------+
     * |	 ^ last										  |
     * |												  |
     * +-------------+------------------------------------+
     * |			 | itemN ...						  |
     * +-------------+------------------+-----------------+
     * |		  ... item3 item2 item1 | "special space" |
     * +--------------------------------+-----------------+
     *
     * Contrast this with the diagram in bufpage.h; note the mismatch
     * between linps and items.  This is because we reserve linp0 as a
     * placeholder for the pointer to the "high key" item; when we have
     * filled up the page, we will set linp0 to point to itemN and clear
     * linpN.  On the other hand, if we find this is the last (rightmost)
     * page, we leave the items alone and slide the linp array over.
     *
     * 'last' pointer indicates the last offset added to the page.
     */
    void _buildadd(Relation heap, DiskBTPageState *state, Data *data, Size truncextra)
    {
        CHECK_FOR_INTERRUPTS();
        NodeImpl nnodeimpl = state->btps_nodeImpl;
        OffsetNumber last_off = state->btps_lastoff;
        Size last_truncextra = state->btps_lastextra;
        state->btps_lastextra = truncextra;
        Size pgspc = PageGetFreeSpace(nnodeimpl._page);
        Size itupsz = MAXALIGN(data->size());
        const bool is_leaf = (state->btps_level == 0);

        /*
         * Check whether the item can fit on a btree page at all. (Eventually, we
         * ought to try to apply TOAST methods if not.) We actually need to be
         * able to fit three items on every page, so restrict any one item to 1/3
         * the per-page available space. Note that at this point, itupsz doesn't
         * include the ItemId.
         *
         * NOTE: similar code appears in _bt_insertonpg() to defend against
         * oversize items being inserted into an already-existing index. But
         * during creation of an index, we don't go through there.
         */
        if (unlikely(itupsz > (Size)BTREE_MAX_ITEM_SIZE(nnodeimpl._page))) {
            btree_check_third_page(_ctx.get_index(), heap, is_leaf, nnodeimpl._page, data->tuple());
        }

        /*
         * Check to see if page is "full".	It's definitely full if the item won't
         * fit.  Otherwise, compare to the target freespace derived from the
         * fillfactor.	However, we must put at least two items on each page, so
         * disregard fillfactor if we don't have that many.
         */
        if (pgspc < itupsz + (is_leaf ? MAXALIGN(sizeof(ItemPointerData)) : 0) ||
            (pgspc + last_truncextra < state->btps_full && last_off > p_firstkey)) {
            /* Finish off the page and write it out. */
            NodeImpl onodeimpl = nnodeimpl;
            nnodeimpl = get_new_build_node(onodeimpl);

            /*
             * We copy the last item on the page into the new page, and then
             * rearrange the old page so that the 'last item' becomes its high key
             * rather than a true data item.  There had better be at least two
             * items on the page already, else the page would be empty of useful
             * data.
             */
            Assert(last_off > p_firstkey);
            ItemId ii = PageGetItemId(onodeimpl._page, last_off);
            Data *odata = onodeimpl.get_data(last_off);
            nnodeimpl.sortadd(odata, p_firstkey);
            /* Move 'last' into the high key position on opage */
            ItemId hii = PageGetItemId(onodeimpl._page, P_HIKEY);
            *hii = *ii;
            ItemIdSetUnused(ii); /* redundant */
            ((PageHeader)onodeimpl._page)->pd_lower -= sizeof(ItemIdData);

            /*
             * Link the old page into its parent, using its minimum key. If we
             * don't have a parent, we have to create one; this adds a new btree
             * level.
             */
            if (state->btps_next == NULL) {
                state->btps_next = diskbt_pagestate(_ctx.get_index(), state->btps_level + 1, *this);
            }

            Assert(state->btps_minkey != NULL);
            state->btps_minkey->set_ptr(onodeimpl.ptr());
            _buildadd(heap, state->btps_next, state->btps_minkey, 0);
            pfree(state->btps_minkey);

            /*
             * Save a copy of the minimum key for the new page.  We have to copy
             * it off the old page, not the new one, in case we are not at leaf
             * level.  Despite oitup is already initialized, it's important to get
             * high key from the page, since we could have replaced it with
             * truncated copy.	See comment above.
             */
            state->btps_minkey = Data::create(odata);

            /*
             * Write out the old page.	We never need to touch it again, so we can
             * free the opage workspace too.
             */
            onodeimpl.mark_dirty();
            onodeimpl.destroy();
            
            /* Reset last_off to point to new page */
            last_off = p_firstkey;
        }

        /*
         * If the new item is the first for its page, stash a copy for later. Note
         * this will only happen for the first item on a level; on later pages,
         * the first item for a page is copied from the prior page in the code
         * above. Since the minimum key for an entire level is only used as a
         * minus infinity downlink, and never as a high key, there is no need to
         * truncate away non-key attributes at this point.
         */
        if (last_off == p_hikey) {
            Assert(state->btps_minkey == NULL);
            state->btps_minkey = Data::create(data);
        }

        /* Add the new item into the current page */
        last_off = OffsetNumberNext(last_off);
        nnodeimpl.sortadd(data, last_off);

        state->btps_nodeImpl = nnodeimpl;
        state->btps_lastoff = last_off;
    }

    /* Finish writing out the completed btree. */
    void _uppershutdown(Relation heap, DiskBTPageState *state)
    {
        BlockNumber rootblkno = P_NONE;
        /* Each iteration of this loop completes one more level of the tree. */
        for (DiskBTPageState *s = state; s != NULL; s = s->btps_next) {
            BlockNumber blkno = s->btps_nodeImpl.ptr();
            /*
             * We have to link the last page on this level to somewhere.
             *
             * If we're at the top, it's the root, so attach it to the metapage.
             * Otherwise, add an entry for it to its parent using its minimum key.
             * This may cause the last page of the parent level to split, but
             * that's not a problem -- we haven't gotten to it yet.
             */
            if (s->btps_next == NULL) {
                s->btps_nodeImpl.set_root(true);
                rootblkno = blkno;
            } else {
                Assert(s->btps_minkey != NULL);
                s->btps_minkey->set_ptr(blkno);
                _buildadd(heap, s->btps_next, s->btps_minkey, 0);
                pfree(s->btps_minkey);
                s->btps_minkey = NULL;
            }

            /*
             * This is the rightmost page, so the ItemId array needs to be slid
             * back one slot.  Then we can dump out the page.
             */
            s->btps_nodeImpl.slideleft();
            s->btps_nodeImpl.mark_dirty();
            s->btps_nodeImpl.destroy();
        }

        _ctx.w_lock();
        _ctx.set_root_ptr(rootblkno);
        _ctx.mark_dirty();
        _ctx.w_unlock();
    }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_BUILD_H */
