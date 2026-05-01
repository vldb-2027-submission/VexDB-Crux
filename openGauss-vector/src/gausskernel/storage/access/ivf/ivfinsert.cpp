#include "postgres.h"

#include <float.h>

#include "access/annvector/ivf.h"
#include "storage/buf/bufmgr.h"
#include "utils/memutils.h"
#include "utils/fmgroids.h"
#include "storage/lmgr.h"
#include "storage/indexfsm.h"
#include "access/annvector/ann_utils.h"

typedef struct IvfInsertClosestListData
{
	BlockNumber insertPage;
	BlockNumber secondaryInsertPage;
	ListInfo	listInfo;
	FloatVector	closestCenter;
}			IvfInsertClosestListData;

typedef IvfInsertClosestListData * IvfInsertClosestList;
#define IVFINSERTCLOESTLIST_SIZE(_dim)		(offsetof(IvfInsertClosestListData, closestCenter) + FLOATVECTOR_SIZE(_dim))


IvfInsertClosestList InitIvfInsertClosestList(int dim)
{
	IvfInsertClosestList ret;
	ret = (IvfInsertClosestList)palloc0(IVFINSERTCLOESTLIST_SIZE(dim));
	int size = FLOATVECTOR_SIZE(dim);
	SET_VARSIZE(&ret->closestCenter, size);
	ret->closestCenter.dim = dim;
	return ret;
}

/*
 * Find the list that minimizes the distance function
 */
static void
FindInsertPage(Relation rel, IvfInsertState *state, Datum value, IvfInsertClosestList *insertCloseList, BlockNumber listFirstPage)
{
	Buffer		cbuf;
	Page		cpage;
	IvfList list;
	float		distance;
	float		minDistance = FLT_MAX;
	BlockNumber nextblkno = listFirstPage;
	OffsetNumber offno;
	OffsetNumber maxoffno;
    
	/* Avoid compiler warning */
	(*insertCloseList)->insertPage = InvalidBlockNumber;
	(*insertCloseList)->secondaryInsertPage = InvalidBlockNumber;
	(*insertCloseList)->listInfo.blkno = nextblkno;
	(*insertCloseList)->listInfo.offno = FirstOffsetNumber;

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		cbuf = ReadBuffer(rel, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);
		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			list = (IvfList) PageGetItem(cpage, PageGetItemId(cpage, offno));
			FloatVector *vec = DatumGetFloatVector(value);
			distance = state->procinfo(vec->x, (float*)(&list->center.x), vec->dim);

			if (distance < minDistance || !BlockNumberIsValid((*insertCloseList)->insertPage))
			{
				(*insertCloseList)->insertPage = list->insertPage;
				(*insertCloseList)->secondaryInsertPage = list->secondaryInsertPage;
				(*insertCloseList)->listInfo.blkno = nextblkno;
				(*insertCloseList)->listInfo.offno = offno;
				FloatVectorSet(&((*insertCloseList)->closestCenter), &list->center);
				minDistance = distance;
			}
		}

		nextblkno = IvfPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}
}

/*
 * New buffer
 */
Buffer
IvfInsertNewBuffer(Relation index, ForkNumber forkNum)
{
	Buffer		buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	return buf;
}

/*
 * Batch allocate new buffer (either by recycling, or by extending the index file).
 *
 * The returned buffer is already pinned and exclusive-locked.
 * Caller is responsible for unlock and release the returned buffer.
 */
Buffer IvfInsertBatchNewBuffer(Relation index)
{
	const size_t alloc_page_batch = g_instance.attr.attr_storage.ivf_extend_file_block_batch_count;

	/* First, try to get a page from FSM */
	size_t fsm_allocated_num = 0;
	Buffer ret_buffer_from_fsm = InvalidBuffer;
	BlockNumber prev_blkno_fsm = InvalidBlockNumber;
	Buffer prev_buffer_fsm = InvalidBuffer;
	List* fsm_buf_release = NIL;
	LockRelationForExtension(index, ExclusiveLock);
	size_t newPageNum = 0;

	for (;;) {
		BlockNumber blkno = GetFreeIndexPage(index);
		if (!BlockNumberIsValid(blkno)) {
			break; /* nothing known to FSM */
		}

		if (BlockNumberIsValid(prev_blkno_fsm) && std::abs((int64)blkno - (int64)prev_blkno_fsm) != 1) {
			//the block number is not continus, so recycle back to FSM if current thread can lock the buffer
			RecordFreeIndexPage(index, blkno);
			break;
		}

		Buffer buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		Page page = BufferGetPage(buffer);
		if (isHybridIndex(index)) {
			IvfInitPage(buffer, page);
			MarkBufferDirty(buffer);
			if (RelationNeedsWAL(index)) {
				XLogBeginInsert();
				START_CRIT_SECTION();
				XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRecPtr recptr = XLogInsert(RM_IVF_ID, XLOG_IVF_EXTEND_NEWPAGES);
				PageSetLSN(page, recptr);
				END_CRIT_SECTION();
			}
		}

		if (!BufferIsValid(ret_buffer_from_fsm)) {
			ret_buffer_from_fsm = buffer;
		}

		if (BufferIsValid(prev_buffer_fsm)) {
			IvfPageGetOpaque(BufferGetPage(prev_buffer_fsm))->nextblkno = blkno;
			MarkBufferDirty(prev_buffer_fsm);
			IvfXLogNextBlkNo(index, prev_buffer_fsm, blkno);
		}

		if (buffer != ret_buffer_from_fsm) {
			fsm_buf_release = lcons_int(buffer, fsm_buf_release);
		}
		prev_blkno_fsm = blkno;
		prev_buffer_fsm = buffer;
		if (++fsm_allocated_num >= alloc_page_batch) {
			break;;
		}
	}

	if (BufferIsValid(ret_buffer_from_fsm)) {
		UnlockRelationForExtension(index, ExclusiveLock);
		//unlock and release page except the the first fsm block(ret_buffer_from_fsm)
		ListCell* cell = NULL;
		foreach (cell, fsm_buf_release) {
			UnlockReleaseBuffer(lfirst_int(cell));
		}
		if (fsm_buf_release != NIL) {
			list_free_ext(fsm_buf_release);
		}
		return ret_buffer_from_fsm;
	}

	/* Must extend the file */
	/* From ReadBufferExtended: Caller is responsible for ensuring
	* that only one backend tries to extend a relation at the same
	* time!
	*/
	/* Add a new page */
	Buffer new_buf_ret  = IvfInsertNewBuffer(index, MAIN_FORKNUM);
	Page new_page_ret = BufferGetPage(new_buf_ret);
	/* Init new page */
	IvfInitPage(new_buf_ret, new_page_ret);
	MarkBufferDirty(new_buf_ret);

	Size extend_count = 1;
	Buffer prevbuf = new_buf_ret ;
	List* buf_release  = NIL;
	newPageNum = 0;
	Buffer *buffs = (Buffer*)palloc(sizeof(Buffer) * alloc_page_batch);
	buffs[newPageNum++] = prevbuf;
	while (++extend_count <= alloc_page_batch) {
		Buffer curbuf = IvfInsertNewBuffer(index, MAIN_FORKNUM);
		Page curpage = BufferGetPage(curbuf);
		/* Init new page */
		IvfInitPage(curbuf, curpage);
		BlockNumber curBlkno = BufferGetBlockNumber(curbuf);
		Page prevPage = BufferGetPage(prevbuf);
		IvfPageGetOpaque(prevPage)->nextblkno = curBlkno;
		MarkBufferDirty(curbuf);
		buf_release = lcons_int(curbuf, buf_release);
		prevbuf = curbuf;
		buffs[newPageNum++] = prevbuf;
	}

	UnlockRelationForExtension(index, ExclusiveLock);

	size_t startNo = 0;
	XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID - 1, 0);
	while (startNo < newPageNum)
	{
		Buffer curBufs[XLR_MAX_BLOCK_ID];
		XLogBeginInsert();
		START_CRIT_SECTION();
		int nbufs = 0;
		while (nbufs < XLR_MAX_BLOCK_ID && startNo < newPageNum)
		{
			curBufs[nbufs] = buffs[startNo];
			XLogRegisterBuffer(nbufs, buffs[startNo], REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			nbufs++;
			startNo++;
		}
		if (nbufs == 0) {
			break;
		}
		XLogRecPtr recptr = XLogInsert(RM_IVF_ID, XLOG_IVF_EXTEND_NEWPAGES);
		for (int i = 0; i < nbufs; i++) {
			PageSetLSN(BufferGetPage(curBufs[i]), recptr);
		}
		END_CRIT_SECTION();
	}
	pfree(buffs);

	//unlock and release page except the the first extended block(new_buf_ret)
	ListCell* cell = NULL;
	foreach (cell, buf_release) {
		UnlockReleaseBuffer(lfirst_int(cell));
	}
	if (buf_release != NIL) {
		list_free_ext(buf_release);
	}

	return new_buf_ret;
}

static void InsertListTuple(Relation rel, Item itup, Size itemsz, BlockNumber *insertPage, OffsetNumber *offsetNumber, const bool appendMode)
{
	Buffer		buf;
	Page		page;
	OffsetNumber reuseOffno = InvalidOffsetNumber;

	Assert(itemsz <= BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(IvfPageOpaqueData)));

	/* Find a page to insert the item */
	for (;;) {
		buf = ReadBuffer(rel, *insertPage);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (appendMode) {
			if (PageGetFreeSpace(page) >= itemsz) {
				break;
			}
		} else {
			reuseOffno = PageFindUnsedlp(page);
			if (OffsetNumberIsValid(reuseOffno)) {
				break;
			} else {
				PageHeader phdr = (PageHeader)page;
				if (PageHasFreeLinePointers(phdr)) {
					/*skip wal log the below is acceptable, if crash recovery or standby switch to master, 
					* data correctness will not be affected, just performance issue
					*/
					PageClearHasFreeLinePointers(phdr);
				}
				if (PageGetFreeSpace(page) >= itemsz) {
					break;
				}
			}
		}

		*insertPage = IvfPageGetOpaque(page)->nextblkno;

		if (BlockNumberIsValid(*insertPage))
		{
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Buffer newbuf = IvfInsertBatchNewBuffer(rel);

			page = BufferGetPage(buf);
			/* Update insert page */
			*insertPage = BufferGetBlockNumber(newbuf);

			/* Update previous buffer */
			IvfPageGetOpaque(page)->nextblkno = *insertPage;

			MarkBufferDirty(newbuf);
			MarkBufferDirty(buf);
			/* Commit */
			IvfXLogNextBlkNo(rel, buf, IvfPageGetOpaque(page)->nextblkno);
			UnlockReleaseBuffer(buf);

			/* Prepare new buffer */
			buf = newbuf;
			break;
		}
	}

	page = BufferGetPage(buf);
	if (OffsetNumberIsValid(reuseOffno)) {
		if (!page_index_tuple_overwrite(page, reuseOffno, itup, itemsz)) {
			elog(ERROR, "failed to add index item(overwrite) to \"%s\"", RelationGetRelationName(rel));
		}
		*offsetNumber = reuseOffno;
	} else {
		/* Add to next offset */
		*offsetNumber = PageAddItem(page, itup, itemsz, InvalidOffsetNumber, false, false);
		if (*offsetNumber == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(rel));
	}

	MarkBufferDirty(buf);
	if (RelationNeedsWAL(rel)) {
		IvfXLogInsert(itemsz, *offsetNumber, reuseOffno, itup, buf, page);
	}

	IvftCommitBuffer(buf);
}

/*
 * Insert a tuple into the index
 */
static void
InsertTuple(Relation rel, IvfInsertState *state, Datum *values, const bool *isnull, ItemPointer heap_tid, bool ispq, BlockNumber metablkno, size_t vectorId)
{
	Datum		value;
	Size		itemsz;
	BlockNumber originalInsertPage;
	BlockNumber secOriginalInsertPage = InvalidBlockNumber;
	OffsetNumber offsetNumber;
	ProductQuantizer *pq;
	IvfInsertClosestList closestlist;

	/* Detoast once for all calls */
	value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	const auto getVectorTupDesc = [rel]() -> TupleDesc {
		if (isHybridIndex(rel)) {
			TupleDesc vectorTupdesc = CreateTemplateTupleDesc(1, false);
			Size size =  sizeof(FormData_pg_attribute);
			errno_t rc = memcpy_s(&(vectorTupdesc->attrs), size,  &(RelationGetDescr(rel)->attrs), size);
			securec_check(rc, "\0", "\0");
			return vectorTupdesc;
		} else {
			return RelationGetDescr(rel);
		}
	};

	const auto releaseVectorTupDesc = [rel](TupleDesc tupdesc) {
		if (isHybridIndex(rel)) {
			pfree(tupdesc);
		}
	};

	/* Normalize if needed */
	if (state->normprocinfo != NULL)
	{
		if (!AnnNormValue(state->normprocinfo, &value, NULL)) {
			elog(NOTICE, "Found zero norm vector inserting into vector index, skipping");
			return;
		}
	}
	IvfMetaPage metap = IvfGetMetaPageData(rel, metablkno, ispq);
	BlockNumber listFirstPage = IvfGetListFirstPage(metap, ispq);
	const bool enableToast = IvfMetaGetEnableToast(metap, rel);
	/* Form tuple */
	if (ispq) {
		IvfPQMetaPage metaData = (IvfPQMetaPage)metap;
		closestlist = InitIvfInsertClosestList(metaData->dimensions);
		/* Find the insert page - sets the page and list info */
		FindInsertPage(rel, state, value, &closestlist, listFirstPage);
		Assert(BlockNumberIsValid(closestlist->insertPage));
		originalInsertPage = closestlist->insertPage;
		const bool perfMode  = IvfMetaGetPQPerfMode(metap, rel);

		/* Should do this first, because pq compute codes will modify value */
		if (perfMode) {
			Assert(BlockNumberIsValid(closestlist->secondaryInsertPage));
			secOriginalInsertPage = closestlist->secondaryInsertPage;
			TupleDesc vectorTupdesc = getVectorTupDesc();
			IndexTuple secIndexTuple = index_form_tuple(vectorTupdesc, &value, isnull, enableToast);
			releaseVectorTupDesc(vectorTupdesc);
			secIndexTuple->t_tid = *heap_tid;
			/* Get tuple size */
			Size secitemsz = MAXALIGN(IndexTupleSize(secIndexTuple));
			InsertListTuple(rel, (Item)secIndexTuple, secitemsz, &(closestlist->secondaryInsertPage), &offsetNumber, false);
		}

		pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
		pq->set_basic_values(metaData->dimensions, metaData->num_subquantizers, metaData->nbits);
		pq->set_fvec_L2sqr_ny_nearest_func();
		PopulatePQCodeBookFromPages(rel, metaData->pq_codebook_start_page, pq);
		IvfPQIndexTupleBase indexTuple = InitIvfPQIndexTuple( pq->code_size, perfMode);
		IvfPQComputeCodes(DatumGetFloatVector(value)->x, (float*)&closestlist->closestCenter.x, pq, metaData->by_residual, indexTuple, perfMode);
		indexTuple->htid = *heap_tid;
		if (perfMode) {
			ItemPointerSet(&((IvfPQIndexTuplePerf)indexTuple)->itid, closestlist->secondaryInsertPage, offsetNumber);
		}
		itemsz = MAXALIGN(GetIvfPQTupleSize(pq->code_size, perfMode));
		InsertListTuple(rel, (Item)indexTuple, itemsz, &(closestlist->insertPage), &offsetNumber, true);

	} else {
		closestlist = InitIvfInsertClosestList(metap->dimensions);
		/* Find the insert page - sets the page and list info */
		FindInsertPage(rel, state, value, &closestlist, listFirstPage);
		Assert(BlockNumberIsValid(closestlist->insertPage));
		originalInsertPage = closestlist->insertPage;
		bool vecBufMode = ((IvfflatMetaPage)(metap))->vecBufMode;
		if (vecBufMode) {
			size_t size = sizeof(IvfflatVecBufTupleData);
			IvfflatVecBufTuple indexTuple = (IvfflatVecBufTuple)palloc0(size);
			indexTuple->vectorId = vectorId;
			indexTuple->htid = *heap_tid;
			itemsz = MAXALIGN(size);
			InsertListTuple(rel, (Item)indexTuple, itemsz, &(closestlist->insertPage), &offsetNumber, true);
		} else {
			TupleDesc vectorTupdesc = getVectorTupDesc();
			IndexTuple indexTuple = index_form_tuple(vectorTupdesc, &value, isnull, enableToast);
			releaseVectorTupDesc(vectorTupdesc);
			indexTuple->t_tid = *heap_tid;
			/* Get tuple size */
			itemsz = MAXALIGN(IndexTupleSize(indexTuple));
			InsertListTuple(rel, (Item)indexTuple, itemsz, &(closestlist->insertPage), &offsetNumber, true);
		}
	}

	/* Update the insert page */
	if (closestlist->insertPage != originalInsertPage || closestlist->secondaryInsertPage != secOriginalInsertPage) {
		IvfListUpdateData listUpdateInfo;
		InvalidListUpdateData(&listUpdateInfo);
		listUpdateInfo.insertPage = closestlist->insertPage != originalInsertPage ? closestlist->insertPage : InvalidBlockNumber;
		listUpdateInfo.secondaryInsertPage = closestlist->secondaryInsertPage != secOriginalInsertPage ? closestlist->secondaryInsertPage : InvalidBlockNumber;
		IvfUpdateList(rel, closestlist->listInfo, &listUpdateInfo, MAIN_FORKNUM, true);
	}

    pfree(closestlist);
	pfree(metap);
    if (ispq) {
		pq->free_resourses();
		pfree(pq);
	}
}

IvfInsertState *createIvfInsertState(Relation index, Metric metric, bool ispq, BlockNumber metablkno)
{
	IvfInsertState *state = (IvfInsertState *)palloc0(sizeof(IvfInsertState));

	IvfMetaPage metap = IvfGetMetaPageData(index, metablkno, ispq);
	int dimensions = metap->dimensions;
	pfree(metap);

	IvfprocInfos *ivfprocs = getIvfprocInfo(metric, ispq, dimensions);
	state->procinfo = ivfprocs->procinfo;
	state->normprocinfo = ivfprocs->normprocinfo;
	pfree(ivfprocs);

	return state;
}

/*
 * Insert a tuple into the index
 */
bool
ivfinsert_internal(Relation index, IvfInsertState *state, Datum *values, const bool *isnull, ItemPointer heap_tid, bool ispq, BlockNumber metablkno,  size_t vectorId)
{
	MemoryContext oldCtx;

	/* Skip nulls */
	if (isnull[0])
		return false;

	/*
	 * Use memory context since detoast, IvfNormValue, and
	 * index_form_tuple can allocate
	 */
	MemoryContext insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Ivf insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	/* Insert tuple */
	InsertTuple(index, state, values, isnull, heap_tid, ispq, metablkno, vectorId);

	/* Delete memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}

void IvfXLogNextBlkNo(Relation index, Buffer buffer, BlockNumber blkno)
{
	Page page = BufferGetPage(buffer);
    XLogRecPtr recptr;

	if (!RelationNeedsWAL(index)) {
		return;
	}

	XLogBeginInsert();
	START_CRIT_SECTION();
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) &blkno, sizeof(BlockNumber));
	recptr = XLogInsert(RM_IVF_ID, XLOG_IVF_UPDATE_PAGE_NEXTBLKNO);
	PageSetLSN(page, recptr);
	END_CRIT_SECTION();
}

void IvfXLogInsert(Size itemsz, OffsetNumber offsetNumber, OffsetNumber reuseOffno, Item itup, Buffer buf, Page page)
{
	XLogRecPtr	lsn;
	xl_ivf_insert xl_rec;

	xl_rec.offsetNumber = offsetNumber;
	xl_rec.reuseOffno = reuseOffno;
	xl_rec.itemsz = itemsz;

	XLogBeginInsert();
	START_CRIT_SECTION();
	XLogRegisterData((char *)&xl_rec, sizeof(xl_ivf_insert));
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *)itup, itemsz);
	lsn = XLogInsert(RM_IVF_ID, XLOG_IVF_INSERT_INDEX);
	PageSetLSN(page, lsn);
	END_CRIT_SECTION();
}