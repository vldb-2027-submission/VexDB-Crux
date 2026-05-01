#include "postgres.h"

#include <float.h>
#include <algorithm>
#include <random>

#include <vtl/vector>

#include "catalog/index.h"
#include "access/annvector/ivf.h"
#include "access/annvector/annkmeans.h"
#include "miscadmin.h"
#include "storage/buf/bufmgr.h"
#include "utils/memutils.h"
#include "commands/vacuum.h"
#include "access/xloginsert.h"
#include "postgres.h"
#include "postmaster/bgworker.h"
#include "access/index_backend/taskpool.h"
#include "storage/indexfsm.h"
#include "access/annvector/ann_utils.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"

struct BuildCallbackState {
    IvfBuildState *buildState;
    ann_helper::Timer *timer;
};
/*
 * Add tuple to sort
 */
static void
AddTupleToSort(Relation index, ItemPointer tid, Datum *values, IvfBuildState * buildstate)
{
	float		distance;
	float		minDistance = FLT_MAX;
	int			closestCenter = 0;
	FloatVectorArray centers = buildstate->centers;
	TupleTableSlot *slot = buildstate->slot;
	int			i;

	/* Detoast once for all calls */
	Datum		value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Normalize if needed */
	if (buildstate->normprocinfo != NULL)
	{
		if (!AnnNormValue(buildstate->normprocinfo, &value, buildstate->normvec)) {
			elog(NOTICE, "Found zero norm vector inserting into vector index, skipping");
			return;
		}
	}


	FloatVector *vec = DatumGetFloatVector(value);
	/* Find the list that minimizes the distance */
	for (i = 0; i < centers->length; i++)
	{
		distance = buildstate->procinfo((float*)vec->x, FloatVectorArrayGet(centers, i), buildstate->dimensions);

		if (distance < minDistance)
		{
			minDistance = distance;
			closestCenter = i;
		}
	}

#ifdef IVF_KMEANS_DEBUG
	buildstate->inertia += minDistance;
	buildstate->listSums[closestCenter] += minDistance;
	buildstate->listCounts[closestCenter]++;
#endif

	/* Create a virtual tuple */
	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(closestCenter);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = PointerGetDatum(tid);
	slot->tts_isnull[1] = false;
	if (buildstate->vecBufMode) {
		slot->tts_values[2] = UInt64GetDatum(buildstate->curVecId);
		slot->tts_isnull[2] = false;
	} else {
		slot->tts_values[2] = value;
		slot->tts_isnull[2] = false;
	}

	ExecStoreVirtualTuple(slot);

	/*
	 * Add tuple to sort
	 *
	 * tuplesort_puttupleslot comment: Input data is always copied; the caller
	 * need not save itldin.
	 */
	tuplesort_puttupleslot(buildstate->sortstate, slot);

	buildstate->indtuples++;
}

/*
 * Callback for table_index_build_scan
 */
static void
BuildCallback(Relation index, HeapTuple hup, Datum *values,
			  const bool *isnull, bool tupleIsAlive, void *state)
{
	BuildCallbackState *callState = (BuildCallbackState *) state;
	IvfBuildState *buildstate = callState->buildState;
	ann_helper::Timer *timer = callState->timer;

	MemoryContext oldCtx;

#if PG_VERSION_NUM < 130000
	ItemPointer tid = &hup->t_self;
#endif

	/* Skip nulls */
	if (isnull[0])
		return;

	/* Use memory context since detoast can allocate */
	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* Add tuple to sort */
	AddTupleToSort(index, tid, values, buildstate);

	/* Reset memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
	
	if (timer) {
		timer->inc_loop_count_forground_report("Scan Table");
	}
}

/*
 * Get index tuple from sort state
 */
static inline IvfInsertTupleComposite
GetNextTuple(IvfBuildState *buildstate, Relation index, TupleDesc tupdesc, TupleTableSlot *slot, int *list, Size *itemsz, Size *secondaryItemsz)
{
	Datum		value;
	bool		isnull;
	IvfInsertTupleComposite  tupComp = (IvfInsertTupleComposite)palloc(sizeof(IvfInsertTupleCompositeData));
	tupComp->tuple = NULL;
	tupComp->secondaryTuple = NULL;
    if (buildstate->ispq) {
		IvfPQBuildState *pqbuildstate = (IvfPQBuildState *)buildstate;
		if (tuplesort_gettupleslot(pqbuildstate->sortstate, true, slot, NULL))
		{
			*list = DatumGetInt32(tableam_tslot_getattr(slot, 1, &isnull));
			ItemPointer itempointer = (ItemPointer) DatumGetPointer(tableam_tslot_getattr(slot, 2, &isnull));
			value = heap_slot_getattr(slot, 3, &isnull);
             
			/*do this first because pq compute code will modidy value */
			if (pqbuildstate->perfEnabled) {
				/* Form the index tuple */
				IndexTuple indexTuple = index_form_tuple(tupdesc, &value, &isnull, buildstate->enableToast);
			    indexTuple->t_tid = *itempointer;
				*secondaryItemsz = MAXALIGN(IndexTupleSize(indexTuple));
				tupComp->secondaryTuple = (Item)indexTuple;
			}

			IvfPQIndexTupleBase pqIndexTuple = InitIvfPQIndexTuple(pqbuildstate->pq->code_size, pqbuildstate->perfEnabled);
			IvfPQComputeCodes((float*)DatumGetFloatVector(value)->x, FloatVectorArrayGet(pqbuildstate->centers, *list), 
							 pqbuildstate->pq, pqbuildstate->by_residual, pqIndexTuple, pqbuildstate->perfEnabled);
			pqIndexTuple->htid = *itempointer;
			*itemsz = MAXALIGN(GetIvfPQTupleSize(pqbuildstate->pq->code_size, pqbuildstate->perfEnabled));
			tupComp->tuple = (Item)pqIndexTuple;
		}
		else {
			*list = -1;
		}
	} else {
		if (tuplesort_gettupleslot(buildstate->sortstate, true, slot, NULL))
		{
			*list = DatumGetInt32(tableam_tslot_getattr(slot, 1, &isnull));
			if (buildstate->vecBufMode) {
				value = DatumGetUInt64(tableam_tslot_getattr(slot, 3, &isnull));
				size_t size = sizeof(IvfflatVecBufTupleData);
				IvfflatVecBufTuple indexTuple = (IvfflatVecBufTuple)palloc0(size);
				indexTuple->vectorId = value;
				indexTuple->htid = *((ItemPointer) DatumGetPointer(tableam_tslot_getattr(slot, 2, &isnull)));
				*itemsz = MAXALIGN(size);
				tupComp->tuple = (Item)indexTuple;
			} else {
				value = heap_slot_getattr(slot, 3, &isnull);

				/* Form the index tuple */
				IndexTuple indexTuple = index_form_tuple(tupdesc, &value, &isnull, buildstate->enableToast);
				indexTuple->t_tid = *((ItemPointer) DatumGetPointer(tableam_tslot_getattr(slot, 2, &isnull)));
				*itemsz = MAXALIGN(IndexTupleSize(indexTuple));
				tupComp->tuple = (Item)indexTuple;
			}
		}
		else
			*list = -1;

	}

	return tupComp;
}

struct IvfAllocatePagesShared 
{
	Oid         indexrelid;
	Oid         indexpartid;
	Size        totalPages;
	Size        allocatedPages;
	BlockNumber firstBlkno;
	ForkNumber  forkNum;
	bool         usedUp;
};


void IvfAllocatePagesMain(const BgWorkerContext *bwc)
{
	IvfAllocatePagesShared *shared = (IvfAllocatePagesShared *)bwc->bgshared;

	/* Open relations within worker */
	Relation indexRel = index_open(shared->indexrelid, NoLock);
	Partition indexpart = NULL;
	Relation targetindex;
	if (OidIsValid(shared->indexpartid)) {
		indexpart = partitionOpen(indexRel, shared->indexpartid, NoLock);
		targetindex = partitionGetRelation(indexRel, indexpart);
	} else {
		targetindex = indexRel;
	}

	Buffer buf = ReadBuffer(targetindex, shared->firstBlkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	Page page = BufferGetPage(buf);
	for(Size i = 0; i < shared->totalPages; ++i) {
		IvfAppendPage(targetindex, &buf, &page, shared->forkNum);
		++shared->allocatedPages;
	}
	IvftCommitBuffer(buf);

	/* Close relations within worker */
	if (OidIsValid(shared->indexpartid)) {
		releaseDummyRelation(&targetindex);
		partitionClose(indexRel, indexpart, NoLock);
	}
	index_close(indexRel, NoLock);
}


BlockNumber getNextBlknoFromWorkerAllocated(Buffer buf, Page page, IvfAllocatePagesShared *shared)
{
	if (shared->usedUp) {
		return InvalidBlockNumber;
	}
	BlockNumber nextblkno = InvalidBlockNumber;
	while(!BlockNumberIsValid(nextblkno = IvfPageGetOpaque(page)->nextblkno))
	{
		CHECK_FOR_INTERRUPTS();

		if (shared->allocatedPages >= shared->totalPages) {
			break;
		}
		/*race conditions*/
		MarkBufferDirty(buf);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		pg_usleep(20);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	}

	if (!BlockNumberIsValid(nextblkno) && !shared->usedUp) {
		shared->usedUp = true;
	}

	return nextblkno;
}

void
FetchOrAppendNextPage(Relation index, Buffer *buf, Page *page, ForkNumber forkNum, IvfAllocatePagesShared *shared)
{
	BlockNumber nextblkno = getNextBlknoFromWorkerAllocated(*buf, *page, shared);
	if (BlockNumberIsValid(nextblkno)) {
		/*got from worker, read it*/
		IvftCommitBuffer(*buf);
		*buf = ReadBuffer(index, nextblkno);
		LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);
		*page = BufferGetPage(*buf);
	} else {
		/*append page*/
		IvfAppendPage(index, buf, page, forkNum);
	}
}

/*
 * Create initial entry pages
 */
static void
InsertTuples(Relation index, IvfBuildState * buildstate, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	Buffer		secbuf;
	Page		secpage = NULL;
	int			list;
	IvfInsertTupleComposite tupComp = NULL;

	IvfListUpdateData listUpdateInfo;

	Size		itemsz;
	Size        secondaryItemsz;
	int			i;
	BlockNumber availBlknoFromWorker = InvalidBlockNumber;

	TupleTableSlot *slot = MakeSingleTupleTableSlot(buildstate->tupdesc);

	ann_helper::Timer timer((size_t)buildstate->reltuples, 100'000, buildstate->indexName, buildstate->partIndexName);
	timer.set_stage("Build Index");

	tupComp = GetNextTuple(buildstate, index, buildstate->vectorTupdesc, slot, &list, &itemsz, &secondaryItemsz);
	int parallelWorkers = buildstate->parallelworkers; 
	bool serialMode = true;
	IvfAllocatePagesShared *shared = NULL;
	int requestWorker = 0;
	if (parallelWorkers > 0 && list != -1) {
		buf = IvfNewBuffer(index, forkNum);
		IvfInitRegisterPage(index, &buf, &page);
		availBlknoFromWorker = BufferGetBlockNumber(buf);

		Size freeSpace =  PageGetFreeSpace(page) + sizeof(ItemIdData);
		Size totalPageNeed =  ((Size)buildstate->reltuples)  / (freeSpace / (itemsz + sizeof(ItemIdData)));
		BlockNumber firstBlkno = availBlknoFromWorker;
		IvftCommitBuffer(buf);

		int requestWorker = 1;
		shared = (IvfAllocatePagesShared*)palloc(sizeof(IvfAllocatePagesShared));
		shared->totalPages = totalPageNeed;
		shared->firstBlkno = firstBlkno;
		shared->forkNum = forkNum;
		shared->allocatedPages = 0;
		shared->usedUp = false;
		if (RelationIsPartition(index)) {
			shared->indexrelid = GetBaseRelOidOfParition(buildstate->index);
			shared->indexpartid = RelationGetRelid(buildstate->index);
		} else {
			shared->indexrelid = RelationGetRelid(buildstate->index);
			shared->indexpartid = InvalidOid;
		}
		if(!LaunchBackgroundWorkers(requestWorker, shared, IvfAllocatePagesMain, nullptr, false)) {
			elog(ERROR, "Launch Allocate pages worker failed");
		}
		serialMode = false;
	}

	for (i = 0; i < buildstate->centers->length; i++)
	{
		if (!serialMode) {
			BgworkerListCheckStatus();
		}
		/* Can take a while, so ensure we can interrupt */
		/* Needs to be called when no buffer locks are held */
		CHECK_FOR_INTERRUPTS();
		InvalidListUpdateData(&listUpdateInfo);

		if (!serialMode && BlockNumberIsValid(availBlknoFromWorker)) {
			buf = ReadBuffer(index, availBlknoFromWorker);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
		} else {
			buf = IvfNewBuffer(index, forkNum);
			IvfInitRegisterPage(index, &buf, &page);
		}

		listUpdateInfo.startPage = BufferGetBlockNumber(buf);

		if (buildstate->ispq && ((IvfPQBuildState*)buildstate)->perfEnabled) {
			secbuf = IvfNewBuffer(index, forkNum);
			IvfInitRegisterPage(index, &secbuf, &secpage);
			listUpdateInfo.secondaryStartPage = BufferGetBlockNumber(secbuf);
		}

		/* Get all tuples for list */
		while (list == i)
		{
			
			if (tupComp->secondaryTuple) {
				/* Check for free space */
				if (PageGetFreeSpace(secpage) < secondaryItemsz)
					IvfAppendPage(index, &secbuf, &secpage, forkNum);

				/* Add the item */
				OffsetNumber offsetnum = PageAddItem(secpage, tupComp->secondaryTuple, secondaryItemsz, InvalidOffsetNumber, false, false);
				if (!OffsetNumberIsValid(offsetnum))
					elog(ERROR, "failed to add index  secondary item to \"%s\"", RelationGetRelationName(index));
				
				IvfPQIndexTuplePerf indextuple = (IvfPQIndexTuplePerf)(tupComp->tuple);
				ItemPointerSet(&indextuple->itid, BufferGetBlockNumber(secbuf), offsetnum);
				pfree(tupComp->secondaryTuple);
			}


			/* Check for free space */
			if (PageGetFreeSpace(page) < itemsz) {
				if (serialMode) {
					IvfAppendPage(index, &buf, &page, forkNum);
				} else {
					FetchOrAppendNextPage(index, &buf, &page, forkNum, shared);	
				}
			}
		
			/* Add the item */
			if (PageAddItem(page, tupComp->tuple, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

			pfree(tupComp->tuple);
			pfree(tupComp);

			timer.inc_loop_count_forground_report("Build Index");

			tupComp = GetNextTuple(buildstate, index, buildstate->vectorTupdesc, slot, &list, &itemsz, &secondaryItemsz);
		}

		listUpdateInfo.insertPage = BufferGetBlockNumber(buf);
		if (!serialMode) {
			BlockNumber nextblkno = getNextBlknoFromWorkerAllocated(buf, page, shared);
			IvfPageGetOpaque(page)->nextblkno = InvalidBlockNumber;
			availBlknoFromWorker = nextblkno;
		}
		IvftCommitBuffer(buf);

		if (secpage) {
			listUpdateInfo.secondaryInsertPage = BufferGetBlockNumber(secbuf);
			IvftCommitBuffer(secbuf);
		}

		/* Set the start and insert pages */
		IvfUpdateList(index, buildstate->listInfo[i], &listUpdateInfo, forkNum, false);
	}

	timer.report("Build Index finished");
	timer.destroy();

	if (!serialMode) {
		BgworkerListWaitFinish(&requestWorker);
		pg_memory_barrier();
		BgworkerListSyncQuit();
		
		while(BlockNumberIsValid(availBlknoFromWorker)) {
			RecordFreeIndexPage(index, availBlknoFromWorker);
			Buffer localbuf = ReadBuffer(index, availBlknoFromWorker);
			LockBuffer(localbuf, BUFFER_LOCK_EXCLUSIVE);
			Page localPage = BufferGetPage(localbuf);
			availBlknoFromWorker = IvfPageGetOpaque(localPage)->nextblkno;
			UnlockReleaseBuffer(localbuf);
		}
	}
}

static void setMetricType(IvfBuildState *buildstate, Relation index, bool ispq)
{
	buildstate->metric = getIvfMetricType(index, ispq);
}

/*
 * Initialize the build state
 */
static void
InitBuildState(IvfBuildState * buildstate, Relation heap, Relation index, IndexInfo *indexInfo, bool ispq,
	vector_pair_vector *vectorIds = NULL, int parallelWorkers = 0, int maintenanceWorkMem = 0)
{
	buildstate->ispq = ispq;
	buildstate->heap = heap;
	buildstate->index = index;
	buildstate->indexInfo = indexInfo;
	buildstate->vectorIds = vectorIds;
	buildstate->vecBufMode = isHybridIndex(index);

	buildstate->lists = !isHybridIndex(index) ? IvfGetLists(index) : (int)sqrt(buildstate->vectorIds->size());
	buildstate->dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;

	/* Require column to have dimensions to be indexed */
	if (buildstate->dimensions < 0)
		elog(ERROR, "column does not have dimensions");

	if (buildstate->dimensions > IVF_MAX_DIM)
		elog(ERROR, "column cannot have more than %d dimensions for ivf index", IVF_MAX_DIM);

	buildstate->reltuples = 0;
	buildstate->indtuples = 0;

	/* Get support functions */
	IvfprocInfos *ivfprocs = getIvfprocInfo(buildstate->metric, buildstate->ispq, buildstate->dimensions);
	buildstate->procinfo = ivfprocs->procinfo;
	buildstate->normprocinfo = ivfprocs->normprocinfo;
	buildstate->kmeansprocinfo = ivfprocs->kmeansprocinfo;
	buildstate->kmeansnormprocinfo = ivfprocs->kmeansnormprocinfo;
	buildstate->pqkmeansprocinfo = ivfprocs->pqkmeansprocinfo;
	pfree(ivfprocs);

	/* Require more than one dimension for spherical k-means */
	/* Lists check for backwards compatibility */
	/* TODO Remove lists check in 0.3.0 */
	if (buildstate->kmeansnormprocinfo != NULL && buildstate->dimensions == 1 && buildstate->lists > 1)
		elog(ERROR, "dimensions must be greater than one for this opclass");

	buildstate->vectorTupdesc = CreateTemplateTupleDesc(1, false);
	Size size =  sizeof(FormData_pg_attribute);
    errno_t rc = memcpy_s(&(buildstate->vectorTupdesc->attrs), size,  &(RelationGetDescr(index)->attrs), size);
    securec_check(rc, "\0", "\0");

	/* Create tuple description for sorting */
	buildstate->tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(buildstate->tupdesc, (AttrNumber) 1, "list", INT4OID, -1, 0);
	TupleDescInitEntry(buildstate->tupdesc, (AttrNumber) 2, "tid", TIDOID, -1, 0);
	if (buildstate->vecBufMode) {
		TupleDescInitEntry(buildstate->tupdesc, (AttrNumber) 3, "vectorId", INT8OID, -1, 0);
	} else {
		TupleDescInitEntry(buildstate->tupdesc, (AttrNumber) 3, "vector", buildstate->vectorTupdesc->attrs[0].atttypid, -1, 0);
	}

	buildstate->slot = MakeSingleTupleTableSlot(buildstate->tupdesc);
	buildstate->centers = FloatVectorArrayInit(buildstate->lists, buildstate->dimensions);
	buildstate->listInfo = (ListInfo*)palloc(sizeof(ListInfo) * buildstate->lists);

	/* Reuse for each tuple */
	buildstate->normvec = InitFloatVector(buildstate->dimensions);

	buildstate->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "Ivf build temporary context",
											   ALLOCSET_DEFAULT_SIZES);

#ifdef IVF_KMEANS_DEBUG
	buildstate->inertia = 0;
	buildstate->listSums = palloc0(sizeof(double) * buildstate->lists);
	buildstate->listCounts = palloc0(sizeof(int) * buildstate->lists);
#endif
	buildstate->ivfleader = NULL;
	buildstate->skipkmeansnormsample = false;
	buildstate->enableToast = isHybridIndex(index) ? false : IvfGetEnableToast(index);
	buildstate->parallelworkers = isHybridIndex(index) ? parallelWorkers : IvfGetParallelWorkers(index);
	buildstate->maintenanceWorkMem = isHybridIndex(index) ? maintenanceWorkMem : u_sess->attr.attr_memory.maintenance_work_mem;

	populate_index_partition_name(buildstate->index, buildstate->indexName, buildstate->partIndexName);
}

static void
InitPQBuildState(IvfBuildState * buildstate, Relation heap, Relation index, IndexInfo *indexInfo)
{
	setMetricType(buildstate, index, true);
	InitBuildState(buildstate, heap, index, indexInfo, true);
	IvfPQBuildState* ivfpqbuildstate  = (IvfPQBuildState*)buildstate;
	ivfpqbuildstate->num_subquantizers = IvfPQGetNumSubquantizers(index);
	ivfpqbuildstate->nbits = IvfPQGetNbits(index);
	ivfpqbuildstate->pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
	ivfpqbuildstate->pq->set_basic_values(ivfpqbuildstate->dimensions, ivfpqbuildstate->num_subquantizers, ivfpqbuildstate->nbits);
	ivfpqbuildstate->pq->set_fvec_L2sqr_ny_nearest_func();
	ivfpqbuildstate->by_residual = IvfPQByResidual(index);
	ivfpqbuildstate->perfEnabled = IvfPQPerformanceModeEnabled(index);
}
/*
 * Free resources
 */
static void
FreeBuildState(IvfBuildState * buildstate)
{
	FloatVectorArrayFree(buildstate->centers);
	pfree(buildstate->listInfo);
	pfree(buildstate->normvec);
	pfree(buildstate->vectorTupdesc);

#ifdef IVF_KMEANS_DEBUG
	pfree(buildstate->listSums);
	pfree(buildstate->listCounts);
#endif

	MemoryContextDelete(buildstate->tmpCtx);
}

static void 
FreeIvfPQBuildState(IvfPQBuildState * buildstate)
{
	FloatVectorArrayFree(buildstate->centers);
	pfree(buildstate->listInfo);
	pfree(buildstate->normvec);
	buildstate->pq->free_resourses();
	pfree(buildstate->pq);

#ifdef IVF_KMEANS_DEBUG
	pfree(buildstate->listSums);
	pfree(buildstate->listCounts);
#endif

	MemoryContextDelete(buildstate->tmpCtx);

}

static void PQTrain(IvfPQBuildState * buildstate);



static bool NeedResampleForPQTrain(IvfBuildState * buildstate)
{
	if (buildstate->ispq && buildstate->kmeansnormprocinfo != NULL 
		&& buildstate->metric == Metric::INNER_PRODUCT) {
		return true;
	} else {
		return false;
	}
}

void SampleRows(IvfBuildState *buildstate, int numSamples)
{
	/* Sample rows */
	/* TODO Ensure within maintenance_work_mem */
	buildstate->samples = FloatVectorArrayInit(numSamples, buildstate->dimensions);
	if (!isHybridIndex(buildstate->index)) {
		if (buildstate->heap != NULL) {
			const bool need_norm = buildstate->skipkmeansnormsample ? false : buildstate->kmeansnormprocinfo != NULL;
			IvfBench("ivf_sample_rows",
			ann_sample_rows(buildstate->samples, buildstate->heap, buildstate->index,
				buildstate->dimensions, buildstate->parallelworkers,
				need_norm));
		}
	} else {
		FloatVector *norm_vec = InitFloatVector(buildstate->dimensions);
		const auto populateSamples = [buildstate, norm_vec](const FloatVector* temp)
		{
			FloatVectorArray samples = buildstate->samples;

			Datum value = PointerGetDatum(temp);
			/*
			* Normalize with KMEANS_NORM_PROC since spherical distance function
			* expects unit vectors
			*/
			if (buildstate->kmeansnormprocinfo != NULL && !buildstate->skipkmeansnormsample) {
				if(!AnnNormValue(buildstate->kmeansnormprocinfo, &value, norm_vec)) {
					return;
				}
			}

			FloatVectorArraySet(samples,  samples->length, DatumGetFloatVector(value)->x);
			samples->length++;
		};
		
		size_t sample_target_num = (size_t)buildstate->samples->maxlen;
		if (buildstate->vectorIds->size() <= sample_target_num) {
			FloatVector *vector = InitFloatVector(buildstate->dimensions);
			for (auto v : *buildstate->vectorIds) {
				VecBuffer buffer = vec_read_buffer(buildstate->index, v.vid, buildstate->dimensions * sizeof(float));
				errno_t rc = memcpy_s(vector->x, sizeof(float) * buildstate->dimensions,
									  buffer.get_vecbuf(), sizeof(float) * buildstate->dimensions);
				buffer.release();
				securec_check(rc, "", "");
				populateSamples(vector);
			}
			pfree(vector);
		 } else {
			Vector<size_t> selected;
			std::random_device rd;
			auto x = rd();
			std::mt19937 generator(x);
			std::uniform_int_distribution<size_t> distribution(0, buildstate->vectorIds->size() - 1ul);
			size_t id;
			for (size_t j = 0; j < sample_target_num; ++j) {
				id = distribution(generator);
				if (std::find(selected.cbegin(), selected.cend(), id) != selected.cend()) {
					--j;
					continue;
				}
				selected.push_back(id);
			}

			FloatVector *vector = InitFloatVector(buildstate->dimensions);
			for (auto i : selected) {
				VecBuffer buffer = vec_read_buffer(buildstate->index,
					(*buildstate->vectorIds)[i].vid, buildstate->dimensions * sizeof(float));
				errno_t rc = memcpy_s(vector->x, sizeof(float) * buildstate->dimensions,
									  buffer.get_vecbuf(), sizeof(float) * buildstate->dimensions);
				buffer.release();
				securec_check(rc, "", "");
				populateSamples(vector);
			}
			pfree(vector);
		}
		pfree(norm_vec);
	}

	if (buildstate->samples->length < buildstate->lists)
	{
		ereport(NOTICE,
				(errmsg("ivf index created with little data"),
				errdetail("This will cause low recall."),
				errhint("Drop the index until the table has more data.")));
	}

}


/*
 * Compute centers
 */
static void
ComputeCenters(IvfBuildState * buildstate)
{
	int			numSamples = GetSampleNumbers(buildstate->heap, buildstate->index, buildstate->lists);
	ann_helper::Timer timer(0, 10, buildstate->indexName, buildstate->partIndexName);
	timer.set_stage("Kmeans");
	timer.set_nloop_count_unknown(true);

	SampleRows(buildstate, numSamples);

	/* Calculate centers */
	AnnKmeansState *kmeanstate = (AnnKmeansState*)palloc0(sizeof(AnnKmeansState));
	setupKmeansState(buildstate->metric, buildstate->index, kmeanstate, buildstate->dimensions, buildstate->ispq, false);
	INIT_TASK_RUNNER();
	int parallelWorkers = buildstate->parallelworkers;
	if (parallelWorkers > 0) {
		LAUNCH_CONSUMER(parallelWorkers);
	}
	IvfBench("k-means", AnnKmeans(kmeanstate, buildstate->samples, buildstate->centers, buildstate->maintenanceWorkMem, parallelWorkers));
	FREE_ANNKEMANSTATE(kmeanstate);
	DESTROY_TASK_RUNNER();
	timer.report("Kmeans finished");
	timer.destroy();
	
    if (buildstate->ispq) {
		if (buildstate->samples->length / (buildstate->lists * 1.0) <  3.0) {
			// if there is less sample data, expecially sample data is less than list number, in this case, residual will cause pq train's input data zero vector,
			// thus cause kmeans checking zero norm detected, so in this case, skip residual.
			((IvfPQBuildState*)buildstate)->by_residual = false;
		}
		if (NeedResampleForPQTrain(buildstate)) {
			FloatVectorArrayFree(buildstate->samples);
			buildstate->skipkmeansnormsample = true;
			SampleRows(buildstate, numSamples);
			buildstate->skipkmeansnormsample = false;
		}
		PQTrain((IvfPQBuildState *)buildstate);
	}
 
	/* Free samples before we allocate more memory */
	FloatVectorArrayFree(buildstate->samples);
}

/*
 * Create the metapage
 */
static BlockNumber
CreateMetaPage(Relation index, IvfBuildState *buildState, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	IvfflatMetaPage metap;

	buf = IvfNewBuffer(index, forkNum);
	IvfInitRegisterPage(index, &buf, &page);

	/* Set metapage data */
	metap = IvfflatPageGetMeta(page);
	metap->magicNumber = IVF_MAGIC_NUMBER;
	metap->version = IVFFLAT_VERSION_NEW;
	metap->dimensions = buildState->dimensions;
	metap->lists = buildState->lists;
	metap->listFirstPage = InvalidBlockNumber; //overwrite later
	metap->enableToast = buildState->enableToast;
	metap->vecBufMode = buildState->vecBufMode;

	((PageHeader) page)->pd_lower =
		((char *) metap + sizeof(IvfflatMetaPageData)) - (char *) page;

	BlockNumber blkno = BufferGetBlockNumber(buf);
	IvftCommitBuffer(buf);
	
	return blkno;
}


/*
 * Create the metapage
 */
static BlockNumber
CreatePQMetaPage(Relation index, IvfPQBuildState *build_state, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	IvfPQMetaPage metap;

	buf = IvfNewBuffer(index, forkNum);
	IvfInitRegisterPage(index, &buf, &page);

	/* Set metapage data */
	metap = IvfPQPageGetMeta(page);
	metap->magicNumber = IVF_MAGIC_NUMBER;
	metap->version = IVFPQ_VERSION_NEW;
	metap->dimensions = build_state->dimensions;
	metap->listFirstPage = InvalidBlockNumber; //overwrite later
	metap->lists = build_state->lists;
    metap->by_residual = build_state->by_residual;
	metap->num_subquantizers = build_state->num_subquantizers;
	metap->nbits = build_state->nbits;
	metap->pq_codebook_start_page = 0; //no important, will be rewrite later
	metap->perfEnabled = build_state->perfEnabled;
	metap->enableToast = build_state->enableToast;
	
	((PageHeader) page)->pd_lower =
		((char *) metap + sizeof(IvfPQMetaPageData)) - (char *) page;

	BlockNumber blkno = BufferGetBlockNumber(buf);

	IvftCommitBuffer(buf);

	return blkno;
}

/*
 * Create list pages
 * return the last block number 
 */
static void
CreateListPages(Relation index, FloatVectorArray centers, int dimensions,
				int lists, ForkNumber forkNum, ListInfo * *listInfo)
{
	int			i;
	Buffer		buf;
	Page		page;
	OffsetNumber offno;
	Size		itemsz;
	IvfList list;

	itemsz = MAXALIGN(IVF_LIST_SIZE(dimensions));
	list = (IvfList)palloc(itemsz);

	buf = IvfNewBuffer(index, forkNum);
	IvfInitRegisterPage(index, &buf, &page);
	for (i = 0; i < lists; i++)
	{
		/* Load list */
		list->startPage = InvalidBlockNumber;
		list->insertPage = InvalidBlockNumber;
		list->secondaryStartPage = InvalidBlockNumber;
		list->secondaryInsertPage = InvalidBlockNumber;
		list->center.dim = dimensions;
		errno_t rc = memcpy_s(&list->center.x, FLOATVECTOR_COMPACT_SIZE(dimensions), FloatVectorArrayGet(centers, i), FLOATVECTOR_COMPACT_SIZE(dimensions));
		securec_check(rc, "\0", "\0");

		/* Ensure free space */
		if (PageGetFreeSpace(page) < itemsz)
			IvfAppendPage(index, &buf, &page, forkNum);

		/* Add the item */
		offno = PageAddItem(page, (Item) list, itemsz, InvalidOffsetNumber, false, false);
		if (offno == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		/* Save location info */
		(*listInfo)[i].blkno = BufferGetBlockNumber(buf);
		(*listInfo)[i].offno = offno;
	}

	IvftCommitBuffer(buf);

	pfree(list);
}

/*
 * Print k-means metrics
 */
#ifdef IVF_KMEANS_DEBUG
static void
PrintKmeansMetrics(IvfBuildState * buildstate)
{
	elog(INFO, "inertia: %.3e", buildstate->inertia);

	/* Calculate Davies-Bouldin index */
	if (buildstate->lists > 1)
	{
		double		db = 0.0;

		/* Calculate average distance */
		for (int i = 0; i < buildstate->lists; i++)
		{
			if (buildstate->listCounts[i] > 0)
				buildstate->listSums[i] /= buildstate->listCounts[i];
		}

		for (int i = 0; i < buildstate->lists; i++)
		{
			double		max = 0.0;
			double		distance;

			for (int j = 0; j < buildstate->lists; j++)
			{
				if (j == i)
					continue;

				distance = DatumGetFloat4(FunctionCall2Coll(buildstate->procinfo, buildstate->collation, PointerGetDatum(FloatVectorArrayGet(buildstate->centers, i)), PointerGetDatum(FloatVectorArrayGet(buildstate->centers, j))));
				distance = (buildstate->listSums[i] + buildstate->listSums[j]) / distance;

				if (distance > max)
					max = distance;
			}
			db += max;
		}
		db /= buildstate->lists;
		elog(INFO, "davies-bouldin: %.3f", db);
	}
}
#endif

/*
 * Perform a worker's portion of a parallel sort
 */
static void
IvfParallelScanAndSort(IvfSpool * ivfspool, IvfShared * ivfshared, int sortmem, bool progress)
{
	SortCoordinate coordinate;
	IvfBuildState buildstate;
	TableScanDesc scan;
	double		reltuples;
	IndexInfo  *indexInfo;

	/* Sort options, which must match AssignTuples */
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {INT4LTOID};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};

	/* Initialize local tuplesort coordination state */
	coordinate = (SortCoordinate)palloc0(sizeof(SortCoordinateData));
	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = ivfshared->sharedsort;

	/* Join parallel scan */
	indexInfo = BuildIndexInfo(ivfspool->index);
	indexInfo->ii_Concurrent = ivfshared->isconcurrent;
	setMetricType(&buildstate, ivfspool->index, ivfshared->ispq);
	InitBuildState(&buildstate, ivfspool->heap, ivfspool->index, indexInfo, ivfshared->ispq);
	errno_t rc = memcpy_s(buildstate.centers->items,  FLOATVECTOR_COMPACT_SIZE(buildstate.centers->dim) * buildstate.centers->maxlen,
						  ivfshared->ivfcenters, FLOATVECTOR_COMPACT_SIZE(buildstate.centers->dim) * buildstate.centers->maxlen);
	securec_check(rc, "\0", "\0");					  
	buildstate.centers->length = buildstate.centers->maxlen;
	ivfspool->sortstate = tuplesort_begin_heap(buildstate.tupdesc, 1, attNums, sortOperators, sortCollations, nullsFirstFlags,
												sortmem, false, 0, 0, 1, coordinate);
	buildstate.sortstate = ivfspool->sortstate;
	scan = tableam_scan_begin_parallel(ivfspool->heap, &ivfshared->heapdesc);
	BuildCallbackState callState = { &buildstate, ivfshared->timer };
	reltuples = IndexBuildHeapScan(buildstate.heap, buildstate.index, buildstate.indexInfo, 
									true, BuildCallback, (void *) &callState, scan);

	/* Execute this worker's part of the sort */
	tuplesort_performsort(ivfspool->sortstate);

	/* Record statistics */
	SpinLockAcquire(&ivfshared->mutex);
	ivfshared->nparticipantsdone++;
	ivfshared->reltuples += reltuples;
	ivfshared->indtuples += buildstate.indtuples;
#ifdef IVFFLAT_KMEANS_DEBUG
	ivfshared->inertia += buildstate.inertia;
#endif
	SpinLockRelease(&ivfshared->mutex);

	/* Log statistics */
	if (progress)
		ereport(DEBUG1, (errmsg("leader processed " INT64_FORMAT " tuples", (int64) reltuples)));
	else
		ereport(DEBUG1, (errmsg("worker processed " INT64_FORMAT " tuples", (int64) reltuples)));

	/* We can end tuplesorts immediately */
	tuplesort_end(ivfspool->sortstate);
	pfree_ext(coordinate);

	FreeBuildState(&buildstate);
}



/*
 * Perform work within a launched parallel process
 */
static void
IvfParallelBuildMain(const BgWorkerContext *bwc)
{
	IvfSpool    *ivfspool;
	Relation	heapRel;
	Relation	indexRel;
	Relation    targetheap;
    Relation    targetindex;
	Partition   heappart = NULL;
    Partition   indexpart = NULL;
	LOCKMODE	heapLockmode = NoLock;
	LOCKMODE	indexLockmode = NoLock;
	int			sortmem;

	IvfShared *ivfshared = (IvfShared *)bwc->bgshared;

	/* Open relations within worker */
	heapRel = heap_open(ivfshared->heaprelid, heapLockmode);
	indexRel = index_open(ivfshared->indexrelid, indexLockmode);
	if (OidIsValid(ivfshared->heappartid)) {
		heappart = partitionOpen(heapRel, ivfshared->heappartid,  heapLockmode);
        indexpart = partitionOpen(indexRel, ivfshared->indexpartid, indexLockmode);
        targetheap = partitionGetRelation(heapRel, heappart);
        targetindex = partitionGetRelation(indexRel, indexpart);
	} else {
        targetheap = heapRel;
        targetindex = indexRel;
    }


	/* Initialize worker's own spool */
	ivfspool = (IvfSpool *) palloc0(sizeof(IvfSpool));
	ivfspool->heap = targetheap;
	ivfspool->index = targetindex;

	/* Perform sorting */
	sortmem = ivfshared->maintenanceWorkMem / ivfshared->scantuplesortstates;
	IvfParallelScanAndSort(ivfspool, ivfshared, sortmem, false);

	/* Close relations within worker */
	if (OidIsValid(ivfshared->heappartid)) {
        releaseDummyRelation(&targetheap);
        releaseDummyRelation(&targetindex);
        partitionClose(indexRel, indexpart, NoLock);
        partitionClose(heapRel, heappart, NoLock);
    }

	index_close(indexRel, indexLockmode);
	heap_close(heapRel, heapLockmode);
	pfree(ivfspool);
	
}

static void IvfParallelCleanUp(const BgWorkerContext *bwc)
{
	IvfShared * ivfshared = (IvfShared *)bwc->bgshared;

	/* delete shared fileset */
	Assert(ivfshared->sharedsort);
	SharedFileSetDeleteAll(&ivfshared->sharedsort->fileset);
	pfree_ext(ivfshared->ivfcenters);
	pfree_ext(ivfshared->sharedsort);
}

/*
 * Within leader, participate as a parallel worker
 */
static void
IvfLeaderParticipateAsWorker(IvfBuildState * buildstate)
{
	IvfLeader *ivfleader = buildstate->ivfleader;
	IvfSpool *leaderworker;
	int			sortmem;

	/* Allocate memory and initialize private spool */
	leaderworker = (IvfSpool *) palloc0(sizeof(IvfSpool));
	leaderworker->heap = buildstate->heap;
	leaderworker->index = buildstate->index;

	/* Perform work common to all participants */
	sortmem = buildstate->maintenanceWorkMem / ivfleader->nparticipanttuplesorts;
	IvfParallelScanAndSort(leaderworker, ivfleader->ivfshared, sortmem, true);
	pfree(leaderworker);
}

/*
 * Begin parallel build
 */
static void
IvfBeginParallel(IvfBuildState * buildstate, bool isconcurrent, int request, ann_helper::Timer *timer)
{
	int			scantuplesortstates;
	Size		estsort;
	Size		estcenters;
	IvfShared  *ivfshared;
	Sharedsort *sharedsort;
	char	   *ivfcenters;
	IvfLeader *ivfleader = (IvfLeader *) palloc0(sizeof(IvfLeader));
	bool		leaderparticipates = true;

#ifdef DISABLE_LEADER_PARTICIPATION
	leaderparticipates = false;
#endif

	Assert(request > 0);

	scantuplesortstates = leaderparticipates ? request + 1 : request;

 	/* Store shared build state, for which we reserved space */
	uint32 nparts = 0;
    if (RelationIsGlobalIndex(buildstate->index)) {
        if (RelationIsSubPartitioned(buildstate->heap)) {
            nparts = GetSubPartitionNumber(buildstate->heap);
        } else {
            nparts = getPartitionNumber(buildstate->heap->partMap);
        }
    }
	ivfshared = (IvfShared *)palloc0(sizeof(IvfShared) + sizeof(double) * nparts);
	/* Initialize immutable state */
	if (RelationIsPartition(buildstate->heap)) {
		ivfshared->heaprelid = GetBaseRelOidOfParition(buildstate->heap);
        ivfshared->indexrelid = GetBaseRelOidOfParition(buildstate->index);
        ivfshared->heappartid = RelationGetRelid(buildstate->heap);
        ivfshared->indexpartid = RelationGetRelid(buildstate->index);
	} else {
		ivfshared->heaprelid = RelationGetRelid(buildstate->heap);
		ivfshared->indexrelid = RelationGetRelid(buildstate->index);
		ivfshared->heappartid = InvalidOid;
        ivfshared->indexpartid = InvalidOid;
	}

	ivfshared->ispq = buildstate->ispq;
	ivfshared->maintenanceWorkMem  = buildstate->maintenanceWorkMem;
	ivfshared->isconcurrent = isconcurrent;
	ivfshared->scantuplesortstates = scantuplesortstates;
	SpinLockInit(&ivfshared->mutex);
	/* Initialize mutable state */
	ivfshared->nparticipantsdone = 0;
	ivfshared->reltuples = 0;
	ivfshared->indtuples = 0;
	ivfshared->timer = timer;
#ifdef IVFFLAT_KMEANS_DEBUG
	ivfshared->inertia = 0;
#endif

	HeapParallelscanInitialize(&ivfshared->heapdesc, buildstate->heap);

	/* Store shared tuplesort-private state, for which we reserved space */
	estsort = tuplesort_estimate_shared(scantuplesortstates);
	sharedsort = (Sharedsort *)palloc0(estsort);
	tuplesort_initialize_shared(sharedsort, scantuplesortstates);
	
	estcenters = FLOATVECTOR_COMPACT_SIZE(buildstate->centers->dim) * buildstate->centers->maxlen;
	ivfcenters = (char *)palloc0(estcenters);
	errno_t rc = memcpy_s(ivfcenters, estcenters, buildstate->centers->items, estcenters);
	securec_check(rc, "\0", "\0");
	
	ivfshared->sharedsort  = sharedsort;
	ivfshared->ivfcenters = ivfcenters;

	/* Launch workers, saving status for leader/caller */
	ivfleader->nparticipanttuplesorts = LaunchBackgroundWorkers(request, ivfshared,
																IvfParallelBuildMain, 
																IvfParallelCleanUp);

	ivfleader->ivfshared = ivfshared;

	/* If no workers were successfully launched, back out (do serial build) */
	if (ivfleader->nparticipanttuplesorts == 0)
	{
		pfree_ext(ivfshared);
		pfree_ext(ivfcenters);
		pfree_ext(sharedsort);
		pfree_ext(ivfleader);
		return;
	}

	/* Save leader state now that it's clear build will be parallel */
	buildstate->ivfleader = ivfleader;

	/* Join heap scan ourselves */
	if (leaderparticipates) {
		ivfleader->nparticipanttuplesorts++;
		IvfLeaderParticipateAsWorker(buildstate);
	}
}


/*
 * Within leader, wait for end of heap scan.
 *
 * When called, parallel heap scan started by _bt_begin_parallel() will
 * already be underway within worker processes (when leader participates
 * as a worker, we should end up here just as workers are finishing).
 *
 * Fills in fields needed for ambuild statistics, and lets caller set
 * field indicating that some worker encountered a broken HOT chain.
 *
 * Returns the total number of heap tuples scanned.
 */
static double IvfParallelHeapScan(IvfBuildState *buildstate)
{
	IvfShared *ivfshared = buildstate->ivfleader->ivfshared;
	double		reltuples;
	buildstate->ivfleader->nparticipanttuplesorts -= 1;
	BgworkerListWaitFinish(&buildstate->ivfleader->nparticipanttuplesorts);
	pg_memory_barrier();

	reltuples = ivfshared->reltuples;
	buildstate->indtuples = ivfshared->indtuples;
#ifdef IVFFLAT_KMEANS_DEBUG
			buildstate->inertia = ivfshared->inertia;
#endif	
	ivfshared->sharedsort->actualParticipants = buildstate->ivfleader->nparticipanttuplesorts + 1;
	
	return reltuples;
}

/*
 * End parallel build
 */
static void
IvfflatEndParallel()
{
	BgworkerListSyncQuit();
}
/*
 * Scan table for tuples to index
 */
static void
AssignTuples(IvfBuildState * buildstate)
{
	int			parallel_workers = 0;
	SortCoordinate coordinate = NULL;

	/* Sort options, which must match IvfParallelScanAndSort */
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {INT4LTOID};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};

	/* Get parallel workers */
	parallel_workers = buildstate->parallelworkers;
	size_t reltuples = get_relstats_reltuples(buildstate->heap);
	ann_helper::Timer timer(reltuples, 100'000, buildstate->indexName, buildstate->partIndexName);
	timer.set_stage("Scan Table");

	/* Attempt to launch parallel worker scan when required */
	if (buildstate->heap != NULL && parallel_workers > 0) {
		if (buildstate->heap->rd_rel->relpersistence == RELPERSISTENCE_GLOBAL_TEMP) {
            ereport(NOTICE, (errmsg("switch off parallel mode for global temp table")));
        } else {
			Assert(!buildstate->indexInfo->ii_Concurrent);
			IvfBeginParallel(buildstate, buildstate->indexInfo->ii_Concurrent, parallel_workers, &timer);
		}
	}

	/* Set up coordination state if at least one worker launched */
	if (buildstate->ivfleader)
	{
		coordinate = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
		coordinate->isWorker = false;
		coordinate->nParticipants = buildstate->ivfleader->nparticipanttuplesorts;
		coordinate->sharedsort = buildstate->ivfleader->ivfshared->sharedsort;
	}

	/* Begin serial/leader tuplesort */
	buildstate->sortstate = tuplesort_begin_heap(buildstate->tupdesc, 1, attNums, sortOperators, sortCollations, 
												nullsFirstFlags, buildstate->maintenanceWorkMem, 
												false, 0, 0, 1, coordinate);						

	/* Add tuples to sort */
	if (buildstate->heap != NULL)
	{
		if (buildstate->ivfleader) {
			buildstate->reltuples = IvfParallelHeapScan(buildstate);
			pfree_ext(coordinate);
		} else {
			BuildCallbackState callState = { buildstate, &timer};
			buildstate->reltuples = IndexBuildHeapScan(buildstate->heap, buildstate->index, buildstate->indexInfo,
											   true, BuildCallback, (void *)&callState, NULL);
		}

#ifdef IVFFLAT_KMEANS_DEBUG
		PrintKmeansMetrics(buildstate);
#endif
	}

	timer.report("Scan Table finished");
    timer.destroy();
}

/*
 * Scan vectorIds for tuples to index
 */
static void
AssignTuplesFromVectorIds(IvfBuildState * buildstate, bool withoutTuples)
{
	SortCoordinate coordinate = NULL;

	/* Sort options, which must match IvfParallelScanAndSort */
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {INT4LTOID};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};
	

	/* Begin serial/leader tuplesort */
	buildstate->sortstate = tuplesort_begin_heap(buildstate->tupdesc, 1, attNums, sortOperators, sortCollations, 
												nullsFirstFlags, buildstate->maintenanceWorkMem, 
												false, 0, 0, 1, coordinate);
	if (withoutTuples) {
		return;
	}

	/* Add tuples to sort */
	Assert(buildstate->vectorIds != NULL);
	FloatVector *vector = InitFloatVector(buildstate->dimensions);
	Datum values[1];
	bool isnull[1] = {false};
	HeapTupleData hup;
	ann_helper::Timer timer(buildstate->vectorIds->size(), 100'000, buildstate->indexName, buildstate->partIndexName);
	timer.set_stage("Populate TupleSort");

	BuildCallbackState callState = { buildstate, nullptr};
	for (auto v : *buildstate->vectorIds) {
		VecBuffer buffer = vec_read_buffer(buildstate->index, v.vid, buildstate->dimensions * sizeof(float));
		errno_t rc = memcpy_s(vector->x, sizeof(float) * buildstate->dimensions, buffer.get_vecbuf(), sizeof(float) * buildstate->dimensions);
		buffer.release();
		securec_check(rc, "", "");
		values[0] = PointerGetDatum(vector);
		hup.t_self = v.tid;
		buildstate->curVecId = v.vid;
		BuildCallback(buildstate->index, &hup,  values, isnull, true, &callState);
		timer.report_loop("Populate TupleSort");
    }
	pfree(vector);
	timer.report("Populate TupleSort finished");
	timer.destroy();
		
#ifdef IVFFLAT_KMEANS_DEBUG
		PrintKmeansMetrics(buildstate);
#endif
}

/*
 * Create entry pages
 */
static void
CreateEntryPages(IvfBuildState * buildstate, ForkNumber forkNum, bool withoutTuples = false)
{
	/*withoutTuples means just populate each list one empty start/insert page.*/
	if (!isHybridIndex(buildstate->index)) {
		IvfBench("assign tuples", AssignTuples(buildstate));
	} else {
		IvfBench("assign tuples", AssignTuplesFromVectorIds(buildstate, withoutTuples));
	}
	
	/* Sort */
	IvfBench("sort tuples", tuplesort_performsort(buildstate->sortstate));

	// /* End parallel build */
	if (buildstate->ivfleader) {
		IvfflatEndParallel();
		pfree_ext(buildstate->ivfleader);
	}


#ifdef IVF_KMEANS_DEBUG
	PrintKmeansMetrics(buildstate);
#endif

	/* Insert */
	IvfBench("load tuples", InsertTuples(buildstate->index, buildstate, forkNum));
	tuplesort_end(buildstate->sortstate);
}

void IvfUpdateListFirstPage(Relation index, BlockNumber firstPage, BlockNumber metablkno)
{
	Buffer		buf;
	Page		page;
	IvfflatMetaPage metap;

	buf = ReadBuffer(index, metablkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	metap = IvfflatPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVF_MAGIC_NUMBER))
		elog(ERROR, "ivf index magic is not valid");
	
	metap->listFirstPage = firstPage;
	
	IvftCommitBuffer(buf);
}


/*
 * Build the index to centers created
 */

BlockNumber
IvfflatComputeCenters(Relation heap, Relation index, IndexInfo *indexInfo,
		   IvfBuildState * buildstate, ForkNumber forkNum, vector_pair_vector *vectorIds,
		   int parallelWorkers, int maintenanceWorkMem)
{
	InitBuildState(buildstate, heap, index, indexInfo,  false, vectorIds, parallelWorkers, maintenanceWorkMem);

	ComputeCenters(buildstate);

	/* Create pages */
	BlockNumber metablkno = CreateMetaPage(index, buildstate, forkNum);
	CreateListPages(index, buildstate->centers, buildstate->dimensions, buildstate->lists, forkNum, &buildstate->listInfo);

	BlockNumber listFirstBlkno = buildstate->listInfo[0].blkno;
	IvfUpdateListFirstPage(index, listFirstBlkno, metablkno);
	CreateEntryPages(buildstate, forkNum, true);

	if ((RelationNeedsWAL(index) || forkNum == INIT_FORKNUM)) {
		Vector<BlockNumber> indexblknos;
		ivf_collect_index_page_blknos(index, metablkno, indexblknos);
		if (forkNum == MAIN_FORKNUM) {
			log_newpage_range(index, forkNum, 0, (BlockNumber)indexblknos.size(), true, RM_IVF_ID,
							  XLOG_IVF_BUILD_INDEX, indexblknos.begin(), true);
		} else if (forkNum == INIT_FORKNUM) {
			log_newpage_range(index, forkNum, 0, (BlockNumber)indexblknos.size(), true, RM_IVF_ID,
							  XLOG_IVF_UNLOG_BUILD_INDEX, indexblknos.begin(), true);
		}

		ann_helper::optional_destroy(indexblknos);
	}

	FreeBuildState(buildstate);
	
	return metablkno;
}

/*
 * Build the index
 */

BlockNumber
IvfflatBuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
		   IvfBuildState * buildstate, ForkNumber forkNum, vector_pair_vector *vectorIds,
		   int parallelWorkers, int maintenanceWorkMem)
{
	if (!vectorIds) {
		setMetricType(buildstate, index, false);
	}
	InitBuildState(buildstate, heap, index, indexInfo,  false, vectorIds, parallelWorkers, maintenanceWorkMem);

	ComputeCenters(buildstate);

	/* Create pages */
	BlockNumber metablkno = CreateMetaPage(index, buildstate, forkNum);
	CreateListPages(index, buildstate->centers, buildstate->dimensions, buildstate->lists, forkNum, &buildstate->listInfo);
	BlockNumber listFirstBlkno = buildstate->listInfo[0].blkno;
	IvfUpdateListFirstPage(index, listFirstBlkno, metablkno);
	CreateEntryPages(buildstate, forkNum);
	if (!isHybridIndex(index) && (RelationNeedsWAL(index) || forkNum == INIT_FORKNUM)) {
		if (forkNum == MAIN_FORKNUM) {
			log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
							  true, RM_IVF_ID, XLOG_IVF_BUILD_INDEX, NULL, true);
		} else if (forkNum == INIT_FORKNUM) {
			log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
							  true, RM_IVF_ID, XLOG_IVF_UNLOG_BUILD_INDEX, NULL, true);
		}
	}
	FreeBuildState(buildstate);
	
	return metablkno;
}

/*
 * Build the index for a logged table
 */
IndexBuildResult *
ivfflatbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	IvfBuildState buildstate;

	IvfflatBuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = buildstate.reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * Build the index for an unlogged table
 */
void
ivfflatbuildempty_internal(Relation index)
{
	IndexInfo  *indexInfo = BuildIndexInfo(index);
	IvfBuildState buildstate;

	IvfflatBuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}

/*
 * PQ train
 */
static void PQTrain(IvfPQBuildState * buildstate)
{
	int parallelWorkers = buildstate->parallelworkers;
	if (buildstate->by_residual) {
		const auto task = [buildstate] (size_t batchIndex, size_t start, size_t end) {
			for (size_t isamp = start; isamp < end; ++isamp) {
				float* vec = FloatVectorArrayGet(buildstate->samples, isamp);
				float	minDistance = FLT_MAX;
				int		closestCenter = 0;
				/* Find the list that minimizes the distance */
				for (int icenter = 0; icenter < buildstate->centers->length; ++icenter)
				{
					float distance = buildstate->procinfo(vec, FloatVectorArrayGet(buildstate->centers, icenter), buildstate->dimensions);

					if (distance < minDistance)
					{
						minDistance = distance;
						closestCenter = icenter;
					}
				}
				//calculate the residual
				floatvector_sub_inplace(vec, FloatVectorArrayGet(buildstate->centers, closestCenter), buildstate->centers->dim);
			}

		};

		INIT_TASK_RUNNER();
		if (parallelWorkers > 0) {
			LAUNCH_CONSUMER(parallelWorkers);
		}
		START_TASK_POOL();
		PARALLEL_BATCH_RUN_INIT();

		int totalParaWorkers = parallelWorkers + 1;
		PARALLEL_BATCH_RUN_TASK_WAIT(buildstate->samples->length, totalParaWorkers, task);
		WAIT_AND_END_TASK_POOL();
		DESTROY_TASK_RUNNER();
	}

	/* train pq */
	AnnKmeansState *kmeanstate = (AnnKmeansState*)palloc0(sizeof(AnnKmeansState));
	setupKmeansState(buildstate->metric, buildstate->index, kmeanstate, buildstate->pq->dsub, buildstate->ispq, true);
	IvfBench("pq-train", buildstate->pq->train(kmeanstate, buildstate->samples, parallelWorkers, buildstate->maintenanceWorkMem));
	FREE_ANNKEMANSTATE(kmeanstate);
}

void IvfPQUpdateMetaPage(Relation index, BlockNumber listfirstPage,  BlockNumber start_page, BlockNumber metablkno)
{
	Buffer		buf;
	Page		page;
	IvfPQMetaPage metap;

	buf = ReadBuffer(index, metablkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	metap = IvfPQPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVF_MAGIC_NUMBER))
		elog(ERROR, "ivfpq index magic is not valid");
	
	metap->pq_codebook_start_page = start_page;
	metap->listFirstPage = listfirstPage;
	
	IvftCommitBuffer(buf);
}


BlockNumber CreatePQCodeBookPages(Relation index, IvfPQBuildState * buildstate, ForkNumber forkNum) {
	Buffer		buf;
	Page		page;
	errno_t     rc;

	buf = IvfNewBuffer(index, forkNum);
	BlockNumber startBlkno = BufferGetBlockNumber(buf);
	IvfInitRegisterPage(index, &buf, &page);

	PageHeader phdr = (PageHeader)page;
	char* pageContent = PageGetContents(page);

	//first write the total size in the page
	/* copy the item's data onto the page */
	const size_t total_size = buildstate->pq->get_centroids_size()* sizeof(float);
	rc = memcpy_s(pageContent, sizeof(size_t), &total_size, sizeof(size_t));
	securec_check(rc, "", "");
	phdr->pd_lower +=  sizeof(size_t);

	size_t left_size = total_size;
	size_t offset = 0;
	char* src = (char *)buildstate->pq->centroids;
	while (left_size > 0) {
		Size freeSpace =  (int)phdr->pd_upper - (int)phdr->pd_lower;
		Size minSize = std::min(freeSpace, left_size);
		rc = memcpy_s(page + phdr->pd_lower, minSize, src + offset , minSize);
		securec_check(rc, "", "");
		phdr->pd_lower += minSize;
		offset += minSize;
		left_size -= minSize;

		if (left_size > 0) {
			//allocate new page
			Buffer newbuf = IvfNewBuffer(index, forkNum);
			Page newPage;
	    	IvfInitRegisterPage(index, &newbuf, &newPage);
			BlockNumber newBlkno = BufferGetBlockNumber(newbuf);
			IvfPageGetOpaque(page)->nextblkno = newBlkno;
			IvftCommitBuffer(buf);

			buf = newbuf;
			page = BufferGetPage(buf);
			phdr = (PageHeader)page;
		}
	}

	IvftCommitBuffer(buf);
	Assert(offset == total_size);

	return startBlkno;
}

/*
 * Build the index
 */
static void
IvfpqBuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
		   		IvfPQBuildState * buildstate, ForkNumber forkNum)
{
	InitPQBuildState(buildstate, heap, index, indexInfo);

	ComputeCenters(buildstate);

	/* Create pages */
	BlockNumber metablkno = CreatePQMetaPage(index, buildstate, forkNum);
	CreateListPages(index, buildstate->centers, buildstate->dimensions, buildstate->lists, forkNum, &buildstate->listInfo);
	BlockNumber listFirstBlkno = buildstate->listInfo[0].blkno;
	if ( !BlockNumberIsValid(listFirstBlkno)) {
		elog(ERROR, "Create list pages block number is invalid.");
	}

	BlockNumber startBlkno = CreatePQCodeBookPages(index, buildstate, forkNum);
	//overwrite the start page no for pq codebook in metapage
	IvfPQUpdateMetaPage(index, listFirstBlkno, startBlkno, metablkno);
	CreateEntryPages(buildstate, forkNum);
	if (RelationNeedsWAL(index) || forkNum == INIT_FORKNUM) {
		if (forkNum == MAIN_FORKNUM) {
			log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
							  true, RM_IVF_ID, XLOG_IVF_BUILD_INDEX, NULL, true);
		} else if (forkNum == INIT_FORKNUM) {
			log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum),
							  true, RM_IVF_ID, XLOG_IVF_UNLOG_BUILD_INDEX, NULL, true);
		}
	}
	FreeIvfPQBuildState(buildstate);
}


/*
 * Build the index for a logged table
 */
IndexBuildResult *
ivfpqbuild_internal(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	IvfPQBuildState buildstate;

	IvfpqBuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = buildstate.reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * Build the index for an unlogged table
 */
void
ivfpqbuildempty_internal(Relation index)
{
	IndexInfo  *indexInfo = BuildIndexInfo(index);
	IvfPQBuildState buildstate;

	IvfpqBuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}
