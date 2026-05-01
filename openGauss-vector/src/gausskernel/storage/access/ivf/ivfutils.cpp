#include "postgres.h"

#include <algorithm> 
#include "access/annvector/ivf.h"
#include "storage/buf/bufmgr.h"
#include "access/annvector/floatvector.h"
#include "storage/indexfsm.h"
#include "postmaster/bgworker.h"
#include "utils/fmgroids.h"
#include "access/annvector/ann_utils.h"

/*
 * Get the number of lists in the index
 */
int
IvfGetLists(Relation index)
{
	IvfOptions *opts = (IvfOptions *) index->rd_options;

	if (opts)
		return opts->lists;

	return IVF_DEFAULT_LISTS;
}

bool 
IvfGetEnableToast(Relation index)
{
	IvfOptions *opts = (IvfOptions *) index->rd_options;

	if (opts)
		return opts->enable_toast;

	return true;
}

/*
 * Get the metapage info
 */
IvfMetaPage
IvfGetMetaPageData(Relation index, BlockNumber metablkno, bool ispq)
{
	Buffer		buf;
	Page		page;
	IvfMetaPage metap;
	IvfMetaPage metaData = NULL;

	buf = ReadBuffer(index, metablkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = IvfPageGetMeta(page);
	
	if (unlikely(metap->magicNumber != IVF_MAGIC_NUMBER))
		elog(ERROR, "ivfpq index magic number is not valid");
	

	if (ispq) {
		metaData = (IvfMetaPage)palloc0(sizeof(IvfPQMetaPageData));
		error_t rc = memcpy_s(metaData, sizeof(IvfPQMetaPageData), metap, sizeof(IvfPQMetaPageData));
		securec_check(rc, "\0", "\0");
	} else {
		metaData = (IvfMetaPage)palloc0(sizeof(IvfflatMetaPageData));
		error_t rc = memcpy_s(metaData, sizeof(IvfflatMetaPageData), metap, sizeof(IvfflatMetaPageData));
		securec_check(rc, "\0", "\0");
	}

	UnlockReleaseBuffer(buf);
	return metaData;
}

int
IvfGetlists(Relation index, BlockNumber metablkno)
{
	Buffer		buf;
	Page		page;
	IvfMetaPage metap;
	int lists;

	buf = ReadBuffer(index, metablkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = IvfPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVF_MAGIC_NUMBER))
		elog(ERROR, "ivf index magic number is not valid");
	

	lists = metap->lists;

	UnlockReleaseBuffer(buf);

	return lists;
}

uint32
IvfGetMetaVersion(Relation index, BlockNumber metablkno)
{
	Buffer		buf;
	Page		page;
	IvfMetaPage metap;
	uint32 version;

	buf = ReadBuffer(index, metablkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = IvfPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVF_MAGIC_NUMBER))
		elog(ERROR, "ivf index magic number is not valid");
	

	version = metap->version;

	UnlockReleaseBuffer(buf);

	return version;
}

size_t IvfPQGetNumSubquantizers(Relation index)
{
	IvfPQOptions *opts = (IvfPQOptions *) index->rd_options;

	if (opts)
		return opts->num_subquantizers;

	return IVFPQ_DEFAULT_NUM_SUBQUANTIZERS;
}

size_t IvfPQGetNbits(Relation index)
{
	IvfPQOptions *opts = (IvfPQOptions *) index->rd_options;

	if (opts)
		return opts->nbits;

	return IVFPQ_DEFAULT_NBITS;
}

bool IvfPQPerformanceModeEnabled(Relation index)
{
	return false;
}

bool IvfPQByResidual(Relation index)
{
	IvfPQOptions *opts = (IvfPQOptions *) index->rd_options;

	if (opts)
		return opts->by_residual;

	return true;
}

int IvfGetParallelWorkers(Relation index)
{
	IvfOptions *opts = (IvfOptions *) index->rd_options;

	if (opts)
		return opts->parallel_workers;

	return 0;
}


BlockNumber IvfGetListFirstPage(IvfMetaPage meta, bool ispq)
{
	if (meta->version == IVFFLAT_VERSION_OLD) {
		return IVF_HEAD_BLKNO;
	}

	if (ispq) {
		return ((IvfPQMetaPage)meta)->listFirstPage;
	} else {
		return ((IvfflatMetaPage)meta)->listFirstPage;
	}
}

bool IvfMetaGetEnableToast(IvfMetaPage meta, Relation index)
{
	if (meta->version == IVFFLAT_VERSION_OLD) {
		return IvfGetEnableToast(index);
	}

	if (meta->version == IVFPQ_VERSION_NEW) {
		return ((IvfPQMetaPage)meta)->enableToast;
	} else {
		return ((IvfflatMetaPage)meta)->enableToast;
	}
}

bool IvfMetaGetPQPerfMode(IvfMetaPage meta, Relation index)
{
	if (meta->version == IVFPQ_VERSION_OLD) {
		return IvfPQPerformanceModeEnabled(index);
	}

	return (IvfPQMetaPage(meta))->perfEnabled;
}

/*
 * Get proc
 */
FmgrInfo *
IvfOptionalProcInfo(Relation rel, uint16 procnum)
{
	if (!OidIsValid(index_getprocid(rel, 1, procnum)))
		return NULL;

	return index_getprocinfo(rel, 1, procnum);
}

/*
 * New buffer
 */
Buffer
IvfNewBuffer(Relation index, ForkNumber forkNum)
{
	if (isHybridIndex(index)) {
		LockRelationForExtension(index, ExclusiveLock);
	}
	
	Buffer		buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);

	if (isHybridIndex(index)) {
		UnlockRelationForExtension(index, ExclusiveLock);
	}

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	return buf;
}

/*
 * Init page
 */
void
IvfInitPage(Buffer buf, Page page)
{
	PageInit(page, BufferGetPageSize(buf), sizeof(IvfPageOpaqueData));
	IvfPageGetOpaque(page)->nextblkno = InvalidBlockNumber;
	IvfPageGetOpaque(page)->page_id = IVF_PAGE_ID;
}

/*
 * Init and register page
 */
void
IvfInitRegisterPage(Relation index, Buffer *buf, Page *page)
{
	*page = BufferGetPage(*buf);
	IvfInitPage(*buf, *page);
}

/*
 * Commit buffer
 */
void
IvftCommitBuffer(Buffer buf)
{
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

/*
 * Add a new page
 *
 * The order is very important!!
 */
void
IvfAppendPage(Relation index, Buffer *buf, Page *page, ForkNumber forkNum)
{
	/* Get new buffer */
	Buffer		newbuf = IvfNewBuffer(index, forkNum);
	Page		newpage = BufferGetPage(newbuf);

	/* Update the previous buffer */
	IvfPageGetOpaque(*page)->nextblkno = BufferGetBlockNumber(newbuf);

	/* Init new page */
	IvfInitPage(newbuf, newpage);

	/* Commit */
	MarkBufferDirty(*buf);
	MarkBufferDirty(newbuf);

	/* Unlock */
	UnlockReleaseBuffer(*buf);

	*page = BufferGetPage(newbuf);
	*buf = newbuf;
}

void InvalidListUpdateData(IvfListUpdate listUpdate)
{
	listUpdate->startPage = InvalidBlockNumber;
	listUpdate->insertPage = InvalidBlockNumber;
	listUpdate->secondaryStartPage = InvalidBlockNumber;
	listUpdate->secondaryInsertPage = InvalidBlockNumber;
}

/*
 * Update the start or insert page of a list
 */
void
IvfUpdateList(Relation index, ListInfo listInfo, IvfListUpdate listUpdateInfo,
				ForkNumber forkNum, bool isWal)
{
	Buffer		buf;
	Page		page;
	IvfList list;
	bool		changed = false;

	buf = ReadBufferExtended(index, forkNum, listInfo.blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	list = (IvfList) PageGetItem(page, PageGetItemId(page, listInfo.offno));

	if (BlockNumberIsValid(listUpdateInfo->startPage) && listUpdateInfo->startPage != list->startPage)
	{
		list->startPage = listUpdateInfo->startPage;
		changed = true;
	}

	if (BlockNumberIsValid(listUpdateInfo->insertPage) && listUpdateInfo->insertPage != list->insertPage)
	{
		list->insertPage = listUpdateInfo->insertPage;
		changed = true;	
	}

	if (BlockNumberIsValid(listUpdateInfo->secondaryStartPage) && listUpdateInfo->secondaryStartPage != list->secondaryStartPage)
	{
		list->secondaryStartPage = listUpdateInfo->secondaryStartPage;
		changed = true;
	}

	if (BlockNumberIsValid(listUpdateInfo->secondaryInsertPage) && listUpdateInfo->secondaryInsertPage != list->secondaryInsertPage)
	{
		list->secondaryInsertPage = listUpdateInfo->secondaryInsertPage;
		changed = true;	
	}

	if (changed) {
		MarkBufferDirty(buf);
	}

	/* Only commit if changed */
	if (isWal && changed && (RelationNeedsWAL(index)))
	{
		IvfXLogUpdateList(listInfo.offno, list, buf, page);
	}

	UnlockReleaseBuffer(buf);
}

void IvfPQComputeCodes(float *vec, float *center, ProductQuantizer *pq, bool by_residual,
						 IvfPQIndexTupleBase indexTuple, bool perfMode) 
{
	uint8_t *codes;
	if (by_residual) {
		floatvector_sub_inplace(vec, center, (int)pq->d);
	}

	if (perfMode) {
		codes = ((IvfPQIndexTuplePerf)indexTuple)->codes;
	} else {
		codes = ((IvfPQIndexTuple)indexTuple)->codes;
	}
	
	pq->compute_code(vec, codes);
}

void PopulatePQCodeBookFromPages(Relation index, BlockNumber blockno, ProductQuantizer *pq)
{
	Buffer		buf;
	Page		page;
	
	size_t leftSize = 0;
	size_t totalSize = 0;
	size_t offset = 0;
	BlockNumber searchPage = blockno;
	char* dest = (char*)pq->centroids;
	while (BlockNumberIsValid(searchPage) ) {
		buf = ReadBuffer(index, searchPage);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		char* pageContent = PageGetContents(page);
		PageHeader phdr = (PageHeader)page;
		Size maxSpace =  page + (int)phdr->pd_lower - pageContent;
		if (searchPage == blockno) {
			//read the total size firstly
			totalSize = *((size_t*)pageContent);
			Assert(totalSize == (pq->get_centroids_size() * sizeof(float)));
			maxSpace -= sizeof(size_t);
			pageContent += sizeof(size_t);
			leftSize = totalSize;
		}

		Size minSize = std::min(maxSpace, leftSize);
		error_t rc = memcpy_s(dest + offset, minSize, pageContent , minSize);
		securec_check(rc, "\0", "\0");
		leftSize -= minSize;
		offset += minSize;

		searchPage = IvfPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
	}

	Assert(offset == totalSize);
}

size_t GetIvfPQTupleSize(size_t code_size, bool perfMode)
{
	size_t size; 
	if (perfMode) {
		size = IVFPQINDEXTUPLEPERFDATA_SIZE(code_size);
	} else {
		size = IVFPQINDEXTUPLEDATA_SIZE(code_size);
	}
	return size;
}


IvfPQIndexTupleBase InitIvfPQIndexTuple(size_t code_size, bool perfMode)
{
	IvfPQIndexTupleBase	 tup;
	size_t	size = GetIvfPQTupleSize(code_size, perfMode);
	tup = (IvfPQIndexTupleBase) palloc0(size);
	return tup;
}

void IvfXLogUpdateList(OffsetNumber offsetNumber, IvfList list, Buffer buf, Page page)
{
	XLogRecPtr	recptr;

	XLogBeginInsert();
	START_CRIT_SECTION();
	XLogRegisterData((char *)&offsetNumber, sizeof(OffsetNumber));
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) &list->startPage, 4 * sizeof(BlockNumber));
	recptr = XLogInsert(RM_IVF_ID, XLOG_IVF_UPDATE_LIST);
	PageSetLSN(page, recptr);
	END_CRIT_SECTION();
}

void ivf_collect_index_page_blknos(Relation index, BlockNumber metablkno, Vector<BlockNumber> &indexblknos)
{
	Buffer		cbuf;
	Page		cpage;
	IvfList list;
	BlockNumber startPages[MaxOffsetNumber];
	BlockNumber secStartPages[MaxOffsetNumber];
	OffsetNumber coffno;
	OffsetNumber cmaxoffno;
	
	uint32 metaversion = IvfGetMetaVersion(index, metablkno);
	const bool ispq = (metaversion == IVFPQ_VERSION_NEW);
	IvfMetaPage metap = IvfGetMetaPageData(index, metablkno, ispq);
	bool secodnaryListExist = ispq && IvfMetaGetPQPerfMode(metap, index);
	BlockNumber listFirstPage = IvfGetListFirstPage(metap, ispq);
	BlockNumber nextblkno = listFirstPage;
	pfree(metap);


	indexblknos.emplace_back(metablkno);

	const auto ivfListCollect = [&indexblknos](Relation index, const BlockNumber startSearchPage)
	{
		Buffer		buf;
		Page	    page;
		BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

		BlockNumber searchPage = startSearchPage;

		/* Iterate over entry pages */
		while (BlockNumberIsValid(searchPage))
		{
			indexblknos.emplace_back(searchPage);
			buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			searchPage = IvfPageGetOpaque(page)->nextblkno;
			UnlockReleaseBuffer(buf);
		}
		FreeAccessStrategy(bas);
	};


	/* Iterate over list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		indexblknos.emplace_back(nextblkno);
		cbuf = ReadBuffer(index, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		cmaxoffno = PageGetMaxOffsetNumber(cpage);

		/* Iterate over lists */
		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			list = (IvfList) PageGetItem(cpage, PageGetItemId(cpage, coffno));
			startPages[coffno - FirstOffsetNumber] = list->startPage;
			secStartPages[coffno - FirstOffsetNumber] = list->secondaryStartPage;
		}


		nextblkno = IvfPageGetOpaque(cpage)->nextblkno;
		UnlockReleaseBuffer(cbuf);

		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			BlockNumber searchPage = startPages[coffno - FirstOffsetNumber];
			ivfListCollect(index, searchPage);
			if (secodnaryListExist) {
				BlockNumber secSearchPage = secStartPages[coffno - FirstOffsetNumber];
				ivfListCollect(index, secSearchPage);
			}
		}
	}

	if (ispq) {
		Buffer		buf;
		Page		page;
		BlockNumber searchPage = ((IvfPQMetaPage)metap)->pq_codebook_start_page;
		while (BlockNumberIsValid(searchPage) ) {
			indexblknos.emplace_back(searchPage);
			buf = ReadBuffer(index, searchPage);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			searchPage = IvfPageGetOpaque(page)->nextblkno;
			UnlockReleaseBuffer(buf);
		}
	}
}

void ivf_recycle_to_fsm(Relation index, BlockNumber metablkno)
{
	Vector<BlockNumber> indexblknos;
	ivf_collect_index_page_blknos(index, metablkno, indexblknos);
	for(auto blkno : indexblknos){
		RecordFreeIndexPage(index, blkno);
	}
	
	ann_helper::optional_destroy(indexblknos);
}

Metric getIvfMetricType(Relation index, bool ispq)
{
	Metric metric;
	FmgrInfo   *procinfo = index_getprocinfo(index, 1, IVF_DISTANCE_PROC);
	FmgrInfo   *normprocinfo = IvfOptionalProcInfo(index,IVF_NORM_PROC);
	if (ispq) {
		if (procinfo->fn_oid == (Oid)F_FLOATVECTOR_L2_SQUARED_DISTANCE) {
			if (normprocinfo != NULL) {
				metric = Metric::COSINE;
			} else {
				metric = Metric::L2;
			}
		} else if (procinfo->fn_oid == (Oid)F_FLOATVECTOR_NEGATIVE_INNER_PRODUCT) {
			metric = Metric::INNER_PRODUCT;
		} else {
			elog(ERROR, "Unknown Distance Function Type");
		}
	} else {
		if (procinfo->fn_oid == (Oid)F_FLOATVECTOR_L2_SQUARED_DISTANCE) {
			metric = Metric::L2;
		} else if (procinfo->fn_oid == (Oid)F_FLOATVECTOR_NEGATIVE_INNER_PRODUCT) {
			if (normprocinfo != NULL) {
				metric = Metric::COSINE;
			} else {
				metric = Metric::INNER_PRODUCT;
			}
		} else {
			elog(ERROR, "Unknown Distance Function Type");
		}
	}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
	return metric;
#pragma GCC diagnostic pop
}

IvfprocInfos *getIvfprocInfo(Metric metric, bool ispq, int dimensions)
{
	IvfprocInfos *ivfprocs = (IvfprocInfos *)palloc0(sizeof(IvfprocInfos));
	if (ispq) {
		if (metric ==  Metric::L2 || metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
			ivfprocs->procinfo = ann_helper::get_general_distance_func(Metric::L2, dimensions);
			ivfprocs->kmeansprocinfo  = ann_helper::get_general_distance_func(Metric::L2_SQRT, dimensions);
			if (metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
				ivfprocs->normprocinfo = ann_helper::get_general_distance_func(Metric::L2_NORM, dimensions);
				ivfprocs->kmeansnormprocinfo = ann_helper::get_general_distance_func(Metric::L2_NORM, dimensions);
			} else {
				ivfprocs->normprocinfo = NULL;
				ivfprocs->kmeansnormprocinfo = NULL;
			}
			ivfprocs->pqkmeansprocinfo = NULL;
		} else if (metric ==  Metric::INNER_PRODUCT) {
			ivfprocs->procinfo = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT, dimensions);
			ivfprocs->kmeansprocinfo = ann_helper::get_general_distance_func(Metric::SPHERICAL, dimensions);
			ivfprocs->normprocinfo = NULL;
			ivfprocs->kmeansnormprocinfo = ann_helper::get_general_distance_func(Metric::L2_NORM, dimensions);
			ivfprocs->pqkmeansprocinfo = ann_helper::get_general_distance_func(Metric::L2_SQRT, dimensions);
		} else {
			elog(ERROR, "Unsupported distance metric:%u.", metric);
		}
	} else {
		if (metric == Metric::L2) {
			ivfprocs->procinfo = ann_helper::get_general_distance_func(Metric::L2, dimensions);
			ivfprocs->kmeansprocinfo = ann_helper::get_general_distance_func(Metric::L2_SQRT, dimensions);
			ivfprocs->normprocinfo = NULL;
			ivfprocs->kmeansnormprocinfo = NULL;
			ivfprocs->pqkmeansprocinfo = NULL;
		} else if (metric == Metric::COSINE || metric == Metric::FAST_COSINE || metric == Metric::INNER_PRODUCT) {
			ivfprocs->procinfo = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT, dimensions);
			ivfprocs->kmeansprocinfo = ann_helper::get_general_distance_func(Metric::SPHERICAL, dimensions);
			ivfprocs->kmeansnormprocinfo = ann_helper::get_general_distance_func(Metric::L2_NORM, dimensions);
			if (metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
				ivfprocs->normprocinfo = ann_helper::get_general_distance_func(Metric::L2_NORM, dimensions);
			} else {
				ivfprocs->normprocinfo = NULL;
			}
			ivfprocs->pqkmeansprocinfo = NULL;
		} else {
			elog(ERROR, "Unsupported distance metric:%u.", metric);
		}
	}

	return ivfprocs;
}
