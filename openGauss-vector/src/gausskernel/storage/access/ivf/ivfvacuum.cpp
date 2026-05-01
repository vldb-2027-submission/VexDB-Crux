#include <vtl/vector>
#include <vtl/btree>

#include "postgres.h"
#include "commands/vacuum.h"
#include "access/annvector/ivf.h"
#include "storage/buf/bufmgr.h"
#include "storage/indexfsm.h"

using ann_helper::optional_destroy;

static void ivfListBulkDdelete(Relation index, IndexBulkDeleteCallback callback,
	const void *callback_state, IndexBulkDeleteResult *stats, BlockNumber startSearchPage,
	BlockNumber *insertPage, bool ispq, bool vecBufMode, bool secondaryList)
{
	Buffer		buf;
	Page	    page;
	OffsetNumber offno;
	OffsetNumber maxoffno;
	int			ndeletable;
	OffsetNumber deletable[MaxOffsetNumber];
	ItemPointer htup;
	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

	*insertPage = InvalidBlockNumber;
	BlockNumber searchPage = startSearchPage;

	Vector<Pair<BlockNumber, int>> invalid_blknos;
	Map<int, BlockNumber> valid_blk_orders;

	/* Iterate over entry pages */
	int order = 0;
	while (BlockNumberIsValid(searchPage)) {
		vacuum_delay_point();

		buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);
		/*
		 * ambulkdelete cannot delete entries from pages that are
		 * pinned by other backends
		 * https://www.postgresql.org/docs/current/index-locking.html
		 */
		LockBufferForCleanup(buf);
		page = BufferGetPage(buf);

		maxoffno = PageGetMaxOffsetNumber(page);
		ndeletable = 0;
		/* Find deleted tuples */
		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
			Item item = PageGetItem(page, PageGetItemId(page, offno));
			if (ispq && !secondaryList) {
				htup = &(((IvfPQIndexTupleBase)item)->htid);
			} else {
				if (vecBufMode) {
					htup = &(((IvfflatVecBufTuple)item)->htid);
				} else {
					htup = &(((IndexTuple)item)->t_tid);
				}
			}

			if (callback(htup, (void *)callback_state, InvalidOid, InvalidBktId)) {
				deletable[ndeletable++] = offno;
				if(!secondaryList && stats) {
					stats->tuples_removed++;
				}
			} else if(!secondaryList && stats) {
				stats->num_index_tuples++;
			}
		}
	
		/* Set to first free page */
		/* Must be set before searchPage is updated */
		if (!BlockNumberIsValid(*insertPage) && ndeletable > 0) {
			*insertPage = searchPage;
		}

		if (ndeletable > 0) {
			uint8 flag;
			if (secondaryList) {
				/* mark tuples unused */
				PageIndexMultiSetUnused(page, deletable, ndeletable);
				flag = DELETE_MARK_UNUSED;
			} else {
				/* Delete tuples */
				PageIndexMultiDelete(page, deletable, ndeletable);
				flag = DELETE_PHYSICAL;
			}

			MarkBufferDirty(buf);
			if (RelationNeedsWAL(index)) {
				IvfXLogDelete(ndeletable, deletable, buf, page, flag);
			}
			if (PageIsUnused(page) && order > 0) {
				invalid_blknos.emplace_back(searchPage, order);
			} else {
				valid_blk_orders.emplace(-order, searchPage);
			}
		} else {
			valid_blk_orders.emplace(-order, searchPage);
		}
		searchPage = IvfPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
		++order;
	}

	for (const auto &p : invalid_blknos) {
		const BlockNumber &del_blk = p.first;
		BlockNumber prev_blk = valid_blk_orders.upper_bound(-p.second)->second;
		Buffer prev_buf = ReadBufferExtended(index, MAIN_FORKNUM, prev_blk, RBM_NORMAL, NULL);
		LockBuffer(prev_buf, BUFFER_LOCK_EXCLUSIVE);
		Page prev_page = BufferGetPage(prev_buf);
		Buffer cur_buf = ReadBufferExtended(index, MAIN_FORKNUM, del_blk, RBM_NORMAL, NULL);
		LockBuffer(cur_buf, BUFFER_LOCK_EXCLUSIVE);
		Page cur_page = BufferGetPage(cur_buf);
		/* double check page status (unused or empty) */
		if (!PageIsUnused(cur_page)) {
			UnlockReleaseBuffer(cur_buf);
			UnlockReleaseBuffer(prev_buf);
			continue;
		}
		BlockNumber next_blkno = IvfPageGetOpaque(cur_page)->nextblkno;
		RecordFreeIndexPage(index, del_blk);
		IvfPageGetOpaque(prev_page)->nextblkno = next_blkno;
		MarkBufferDirty(prev_buf);
		IvfXLogNextBlkNo(index, prev_buf, IvfPageGetOpaque(prev_page)->nextblkno);

		if (PageIsEmpty(cur_page)) {
			IvfPageGetOpaque(cur_page)->nextblkno = InvalidBlockNumber;
			MarkBufferDirty(cur_buf);
			IvfXLogNextBlkNo(index, cur_buf, IvfPageGetOpaque(cur_page)->nextblkno);
		} else {
			IvfInitPage(cur_buf, cur_page);
			MarkBufferDirty(cur_buf);
			if (RelationNeedsWAL(index)) {
				XLogBeginInsert();
				START_CRIT_SECTION();
				XLogRegisterBuffer(0, cur_buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRecPtr recptr = XLogInsert(RM_IVF_ID, XLOG_IVF_EXTEND_NEWPAGES);
				PageSetLSN(cur_page, recptr);
				END_CRIT_SECTION();
			}
		}

		if (BlockNumberIsValid(*insertPage) && *insertPage == del_blk) {
			*insertPage = prev_blk;
		}
		UnlockReleaseBuffer(cur_buf);
		UnlockReleaseBuffer(prev_buf);
	}

	optional_destroy(invalid_blknos);
	optional_destroy(valid_blk_orders);
	FreeAccessStrategy(bas);
}

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
ivfbulkdelete_internal(Relation index, IndexBulkDeleteResult *stats,
	IndexBulkDeleteCallback callback, const void *callback_state, bool ispq, BlockNumber metablkno)
{
	Buffer		cbuf;
	Page		cpage;
	IvfList list;
	BlockNumber startPages[MaxOffsetNumber];
	BlockNumber secStartPages[MaxOffsetNumber];
	BlockNumber insertPage;
	BlockNumber secInsertPage;
	OffsetNumber coffno;
	OffsetNumber cmaxoffno;
	ListInfo	listInfo;
	

	IvfMetaPage metap = IvfGetMetaPageData(index, metablkno, ispq);
	bool secodnaryListExist = ispq && IvfMetaGetPQPerfMode(metap, index);
	BlockNumber listFirstPage = IvfGetListFirstPage(metap, ispq);
	BlockNumber nextblkno = listFirstPage;
	bool vecBufMode = !ispq && ((IvfflatMetaPage)metap)->vecBufMode;
	pfree(metap);

	if (stats == NULL && metablkno == IVF_METAPAGE_BLKNO) {
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	}

	/* Iterate over list pages */
	while (BlockNumberIsValid(nextblkno)) {
		cbuf = ReadBuffer(index, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		cmaxoffno = PageGetMaxOffsetNumber(cpage);

		/* Iterate over lists */
		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno)) {
			list = (IvfList) PageGetItem(cpage, PageGetItemId(cpage, coffno));
			startPages[coffno - FirstOffsetNumber] = list->startPage;
			secStartPages[coffno - FirstOffsetNumber] = list->secondaryStartPage;
		}

		listInfo.blkno = nextblkno;
		nextblkno = IvfPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);

		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno)) {
			BlockNumber searchPage = startPages[coffno - FirstOffsetNumber];
			ivfListBulkDdelete(index, callback, callback_state, stats, searchPage, &insertPage, ispq, vecBufMode,  false);
			if (secodnaryListExist) {
				BlockNumber secSearchPage = secStartPages[coffno - FirstOffsetNumber];
				ivfListBulkDdelete(index, callback, callback_state, stats, secSearchPage, &secInsertPage, ispq, vecBufMode, true);
			}

			/*
			 * Update after all tuples deleted.
			 *
			 * We don't add or delete items from lists pages, so offset won't
			 * change.
			 */
			if (BlockNumberIsValid(insertPage) || BlockNumberIsValid(secInsertPage)) {
				listInfo.offno = coffno;
				IvfListUpdateData listUpdateInfo;
				InvalidListUpdateData(&listUpdateInfo);
				listUpdateInfo.insertPage = insertPage; 
				listUpdateInfo.secondaryInsertPage = secInsertPage; 
				IvfUpdateList(index, listInfo, &listUpdateInfo, MAIN_FORKNUM, true);
			}
		}
	}

	return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
ivfvacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only) {
		return stats;
	}

	/* stats is NULL if ambulkdelete not called */
	/* OK to return NULL if index not changed */
	if (stats == NULL) {
		return NULL;
	}
    
	/* Finally, vacuum the FSM */
    IndexFreeSpaceMapVacuum(info->index);

	stats->num_pages = RelationGetNumberOfBlocks(info->index);

	return stats;
}

void IvfXLogDelete(int ndeletable, OffsetNumber *deletable, Buffer buf, Page page, uint8 flag)
{
	XLogRecPtr recptr;
	xl_ivf_vacuum xl_rec;

	xl_rec.flag = flag;
	xl_rec.ndeletable = ndeletable;

	XLogBeginInsert();
	START_CRIT_SECTION();
	XLogRegisterData((char *)&xl_rec, sizeof(xl_ivf_vacuum));
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *)deletable, ndeletable * sizeof(OffsetNumber));
	recptr = XLogInsert(RM_IVF_ID , XLOG_IVF_DELETE_INDEX);
	PageSetLSN(page, recptr);
	END_CRIT_SECTION();
}
