#include <float.h>
#include <algorithm>
#include <cstdlib>

#include "postgres.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/annvector/ivf.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/buf/bufmgr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "access/index_backend/taskpool.h"
#include "access/index_backend/index_backend.h"
#include "access/annvector/ann_utils.h"

/*
 * Compare list distances
 */
static int
CompareLists(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	if (((const IvfScanList *) a)->distance > ((const IvfScanList *) b)->distance) {
		return 1;
	}
	if (((const IvfScanList *) a)->distance < ((const IvfScanList *) b)->distance) {
		return -1;
	}
	return 0;
}

struct DistTid : SAFE_CONSTRUCTOR {
    float dist;
    ItemPointerData tid;
    DistTid(float d, ItemPointerData t) : dist(d), tid(t) {}

	static bool larger_tid(ItemPointerData a, ItemPointerData b)
    {
        BlockNumber a_blk = ((BlockNumber)a.ip_blkid.bi_hi << 16) | a.ip_blkid.bi_lo;
        BlockNumber b_blk = ((BlockNumber)b.ip_blkid.bi_hi << 16) | b.ip_blkid.bi_lo;
        if (a_blk == b_blk) {
            return a.ip_posid > b.ip_posid;
        }
        return a_blk > b_blk;
    }
    bool operator<(const DistTid &rhs) const
    	{ return dist < rhs.dist || (dist == rhs.dist && larger_tid(tid, rhs.tid)); }
};

struct IvfScanOpaqueData {
	TupleDesc	tupdesc;
	int			probes;
	int			dimensions;
	Buffer		buf;
	BlockNumber listFirstPage;
	Metric      metric;
	bool        vecBufMode;
	bool		first;
	
	/* pq */
	bool ispq;
	bool by_residual;
	bool pqPerfEnabled;
	ProductQuantizer *pq;

	/* Support functions */
	distance_func   procinfo;
	distance_func   normprocinfo;

	/* Sorting */
	DistTid *dist_tids;
	size_t cur_pos;
	size_t sorted_pos;
	size_t size;

	/* Lists */
	pairingheap *listQueue;
	IvfScanList lists[FLEXIBLE_ARRAY_MEMBER];	/* must come last */
};
typedef IvfScanOpaqueData *IvfScanOpaque;
static_assert(!ann_helper::constructor_need_ctx<DistTid>, "compiler error on DistTid");
using dist_tids_type = Vector<DistTid, HUGE_ALLOCATOR<DistTid>>;

/*
 * Get lists and sort by distance
 */
static void
GetScanLists(Relation index, IvfScanOpaque so, FloatVector *queryvec)
{
	Buffer		cbuf;
	Page		cpage;
	IvfList list;
	OffsetNumber offno;
	OffsetNumber maxoffno;
	int			listCount = 0;
	float		distance;
	IvfScanList *scanlist;
	float		maxDistance = FLT_MAX;
	BlockNumber nextblkno = so->listFirstPage;

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		cbuf = ReadBuffer(index, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			list = (IvfList) PageGetItem(cpage, PageGetItemId(cpage, offno));
			/* Use procinfo from the index instead of scan key for performance */
			distance = so->procinfo((float*)&list->center.x, queryvec->x, so->dimensions);

			if (listCount < so->probes)
			{
				scanlist = &so->lists[listCount];
				scanlist->startPage = list->startPage;
				scanlist->distance = distance;
				if (so->ispq) {
					scanlist->center = InitFloatVector(so->dimensions);
					FloatVectorSet(scanlist->center, &list->center);
				}
				listCount++;

				/* Add to heap */
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Calculate max distance */
				if (listCount == so->probes)
					maxDistance = ((IvfScanList *) pairingheap_first(so->listQueue))->distance;
			}
			else if (distance < maxDistance)
			{
				/* Remove */
				scanlist = (IvfScanList *) pairingheap_remove_first(so->listQueue);

				/* Reuse */
				scanlist->startPage = list->startPage;
				scanlist->distance = distance;
				if (so->ispq) {
					FloatVectorSet(scanlist->center, &list->center);
				}
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Update max distance */
				maxDistance = ((IvfScanList *) pairingheap_first(so->listQueue))->distance;
			}
		}

		nextblkno = IvfPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}
}

/*
 * Get items
 */
static dist_tids_type
GetScanItems(Relation index, IvfScanOpaque so,  FloatVector *queryvec, MemoryContext ctx)
{
	ann_helper::distance_func dist_func;
	bool alloced = false;
	float *queryFloat = nullptr;
	float flag = 1.0;

	if (so->ispq) {
		if(so->metric == Metric::INNER_PRODUCT) {
			flag = -1.0;
		}
	} else {
		queryFloat = queryvec->x;
		if (so->vecBufMode) {
			dist_func = ann_helper::get_aligned_distance_func(so->metric, so->dimensions);
			if (!is_aligned(queryFloat)) {
				size_t dim = so->dimensions;
				float *temp = alloc_floatvector(dim);
				errno_t rc = memcpy_s(temp, sizeof(float) * dim, queryFloat, sizeof(float) * dim);
				securec_check(rc, "\0", "\0");
				queryFloat = temp;
				alloced = true;
			}
		}
	}

	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

	Oid index_id;
	Oid index_part_id;
	if (RelationIsPartition(index)) {
		Assert(RelationIsPartition(index));
		index_id = GetBaseRelOidOfParition(index);
		index_part_id = RelationGetRelid(index);
	} else {
		Assert(!RelationIsPartition(index));
		index_id = RelationGetRelid(index);
		index_part_id = InvalidOid;
	}

	pthread_mutex_t lock;
	pthread_mutex_init(&lock, NULL);
	constexpr size_t est_ndata_per_cluster = 512ul;
	dist_tids_type dist_tids(so->probes * est_ndata_per_cluster, ctx);
	const bool use_parallel = USE_PARALLEL_QUERY;
	const auto task = [&](IvfScanList *scanlist) {
		/* Search all entry pages for list */
		Buffer		buf;
		Page		page;
		OffsetNumber offno;
		OffsetNumber maxoffno;
		Datum		datum;
		bool		isnull;
		uint8_t     *codes4[4];
		ItemPointerData tids4[4];
		float       distances4[4];
		BlockNumber searchPage = scanlist->startPage;
		float *     distTable;
		FloatVector *residualVector = NULL;
		Vector<DistTid> local_dist_tids(est_ndata_per_cluster);

		Relation index_base = NULL;
		Partition index_part = NULL;
		Relation _index;
		//TASKPOOLLOG("DEBUG: run task ivf");

		if (so->ispq) {
			distTable = (float *)palloc(so->pq->ksub * so->pq->M * sizeof(float));
			residualVector = InitFloatVector(so->dimensions);
			FloatVectorSet(residualVector, queryvec);
			if(so->by_residual) {
				floatvector_sub_inplace(residualVector->x, scanlist->center->x, so->dimensions);
			}
			so->pq->compute_distance_table(residualVector->x, distTable);
		}

		if (index_part_id == InvalidOid) {
			_index = index_open(index_id, NoLock);
		} else {
			index_base = index_open(index_id, NoLock);
			index_part = partitionOpen(index_base, index_part_id, NoLock);
			_index = partitionGetRelation(index_base, index_part);
		}
		while (BlockNumberIsValid(searchPage)) {
			buf = ReadBufferExtended(_index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			maxoffno = PageGetMaxOffsetNumber(page);
			if (so->ispq) {
				OffsetNumber mod = maxoffno % 4;
				for (offno = FirstOffsetNumber;  offno <= maxoffno - mod; offno = OffsetNumberNext(offno)) {
					IvfPQIndexTupleBase itup = (IvfPQIndexTupleBase) PageGetItem(page, PageGetItemId(page, offno));
					int idx = (offno % 4 == 0) ? 3 : (offno % 4) - 1;
					tids4[idx] = so->pqPerfEnabled ? ((IvfPQIndexTuplePerf)(itup))->itid : itup->htid;
					codes4[idx] = so->pqPerfEnabled ? ((IvfPQIndexTuplePerf)(itup))->codes : ((IvfPQIndexTuple)(itup))->codes;						
					if (offno % 4 == 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
						so->pq->distance_to_four_code(distTable, codes4[0], codes4[1], codes4[2], codes4[3],
													  distances4[0], distances4[1], distances4[2], distances4[3]);
#pragma GCC diagnostic pop
						if (use_parallel) {
							for (size_t i = 0; i < 4; ++i) {
								local_dist_tids.emplace_back(flag * distances4[i], tids4[i]);
							}
						} else {
							for (size_t i = 0; i < 4; ++i) {
								dist_tids.emplace_back(flag * distances4[i], tids4[i]);
							}
						}
					}
				}

				for (offno = maxoffno - mod + 1; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
					IvfPQIndexTupleBase itup = (IvfPQIndexTupleBase) PageGetItem(page, PageGetItemId(page, offno));
					ItemPointerData tid = so->pqPerfEnabled ? ((IvfPQIndexTuplePerf)(itup))->itid : itup->htid;
					uint8_t *codes = so->pqPerfEnabled ? ((IvfPQIndexTuplePerf)(itup))->codes : ((IvfPQIndexTuple)(itup))->codes;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
					float _dist = flag * so->pq->distance_to_code(codes, distTable);
#pragma GCC diagnostic pop
					if (use_parallel) {
						local_dist_tids.emplace_back(_dist, tid);
					} else {
						dist_tids.emplace_back(_dist, tid);
					}
				}
			} else {
				if (so->vecBufMode) {
					for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
						IvfflatVecBufTuple itup = (IvfflatVecBufTuple) PageGetItem(page, PageGetItemId(page, offno));
						VecBuffer buffer = vec_read_buffer(_index, itup->vectorId, so->dimensions * sizeof(float));
						float _dist = dist_func((float *)buffer.get_vecbuf(), queryFloat, so->dimensions);
						buffer.release();
						if (use_parallel) {
							local_dist_tids.emplace_back(_dist, itup->htid);
						} else {
							dist_tids.emplace_back(_dist, itup->htid);
						}
					}
				} else {
					for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno)) {
						IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offno));
						datum = index_getattr(itup, 1, so->tupdesc, &isnull);
						FloatVector *vd = DatumGetFloatVector(datum);
						float _dist = so->procinfo(vd->x, queryFloat, so->dimensions);
						if (use_parallel) {
							local_dist_tids.emplace_back(_dist, itup->t_tid);
						} else {
							dist_tids.emplace_back(_dist, itup->t_tid);
						}
						if (Pointer(vd) != DatumGetPointer(datum)) {
							pfree(vd);
						}
					}
				}
			}
			searchPage = IvfPageGetOpaque(page)->nextblkno;
			UnlockReleaseBuffer(buf);
		}
		if (use_parallel) {
			pthread_mutex_lock(&lock);
			dist_tids.push_back(local_dist_tids.cbegin(), local_dist_tids.cend());
			pthread_mutex_unlock(&lock);
		}
        ann_helper::optional_destroy(local_dist_tids);
		if (so->ispq) {
			pfree(distTable);
			pfree(residualVector);
		}
		if (index_part_id == InvalidOid) {
			index_close(_index, NoLock);
		} else {
			releaseDummyRelation(&_index);
			partitionClose(index_base, index_part, NoLock);
			index_close(index_base, NoLock);
		}
	};

	const bool isglobaltemp = index->rd_backend != InvalidBackendId;
	if (!isglobaltemp && use_parallel) {
		INIT_TASK_RUNNER();
		LOAD_CONSUMER();

		START_TASK_POOL();
		/* Search closest probes lists */
		while (!pairingheap_is_empty(so->listQueue)) {
			IvfScanList *scanlist = (IvfScanList *)pairingheap_remove_first(so->listQueue);
			RUN_TASK(task, scanlist);
		}
		RESIGN_PRODUCER();
		TASK_RUNNER->pure_consume();
		WAIT_AND_END_TASK_POOL();
		if (!so->vecBufMode) {
			DESTROY_TASK_RUNNER();
		}
	} else {
		/* Search closest probes lists */
		while (!pairingheap_is_empty(so->listQueue)) {
			IvfScanList *scanlist = (IvfScanList *)pairingheap_remove_first(so->listQueue);
			task(scanlist);
		}
	}
	
	pthread_mutex_destroy(&lock);

	if (alloced) {
		free_vector(queryFloat);
	}

	FreeAccessStrategy(bas);
	return dist_tids;
}


void *ivfcreatescanopaque(Relation index, Metric metric, bool ispq, BlockNumber metablkno,
	int probes_in, TupleDesc tupDesc)
{
	IvfScanOpaque so;
	int			lists;
	int			dimensions;
	
	IvfMetaPage metap = IvfGetMetaPageData(index, metablkno, ispq);
	lists = metap->lists;
	dimensions = metap->dimensions;

	if (probes_in > lists) {
		probes_in = lists;
	}
	
	so = (IvfScanOpaque) palloc(offsetof(IvfScanOpaqueData, lists) + probes_in * sizeof(IvfScanList));
	so->tupdesc = tupDesc;
	so->ispq = ispq;
	so->buf = InvalidBuffer;
	so->first = true;
	so->probes = probes_in;
	so->vecBufMode = false;
	so->dist_tids = NULL;

	so->metric = metric;
	IvfprocInfos *ivfprocs = getIvfprocInfo(so->metric, ispq, dimensions);
	so->procinfo = ivfprocs->procinfo;
	so->normprocinfo = ivfprocs->normprocinfo;
	pfree(ivfprocs);

	if (ispq) {
		IvfPQMetaPage metaData = (IvfPQMetaPage)metap;
		so->by_residual = metaData->by_residual;
		so->pqPerfEnabled = metaData->perfEnabled;
		so->pq = (ProductQuantizer *)palloc(sizeof(ProductQuantizer));
		so->pq->set_basic_values(metaData->dimensions, metaData->num_subquantizers, metaData->nbits);
		so->pq->set_fvec_ny_distance_func(so->metric == Metric::INNER_PRODUCT ? Metric::INNER_PRODUCT : Metric::L2);
		so->pq->set_dist_code_func();
		PopulatePQCodeBookFromPages(index, metaData->pq_codebook_start_page, so->pq);
		for(int i = 0; i < so->probes; ++i) {
			so->lists[i].center = nullptr;
		}
	} else {
		so->vecBufMode = ((IvfflatMetaPage)metap)->vecBufMode;
	}

	so->listFirstPage = IvfGetListFirstPage(metap, ispq);
	so->dimensions = dimensions;
	so->listQueue = pairingheap_allocate(CompareLists, NULL);

	pfree(metap);

	return so;
}


/*
 * Prepare for an index scan
 */
IndexScanDesc
ivfbeginscan_internal(Relation index, int nkeys, int norderbys, bool ispq, BlockNumber metablkno)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	int probes = u_sess->attr.attr_storage.ivf_probes;
	Metric metric = getIvfMetricType(index, ispq);
	scan->opaque = ivfcreatescanopaque(index, metric, ispq, metablkno, probes, RelationGetDescr(index));
	return scan;
}

/*
 * Start or restart an index scan
 */
void
ivfrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	IvfScanOpaque so = (IvfScanOpaque) scan->opaque;
	so->first = true;
	pfree_ext(so->dist_tids);
	pairingheap_reset(so->listQueue);

	if (keys && scan->numberOfKeys > 0) {
		errno_t rc = memmove_s(scan->keyData, scan->numberOfKeys * sizeof(ScanKeyData),keys, scan->numberOfKeys * sizeof(ScanKeyData));
		securec_check_c(rc, "\0", "\0");
    }

	if (orderbys && scan->numberOfOrderBys > 0) {
		errno_t rc = memmove_s(scan->orderByData, scan->numberOfOrderBys * sizeof(ScanKeyData), orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
		securec_check_c(rc, "\0", "\0");
	}	
}

static size_t ivfPQRefineDistanceSort(IndexScanDesc scan, IvfScanOpaque so, FloatVector *queryvec,
	int64 limitOffset, int64 limitCount, dist_tids_type &dist_tids)
{
	double kfactor = u_sess->attr.attr_storage.ivfpq_refine_k_factor;
	size_t klimit = std::min<size_t>(limitCount * kfactor + limitOffset, dist_tids.size());
	if (klimit == 0) {
		return 0;
	}
	std::nth_element(dist_tids.begin(), dist_tids.at(klimit), dist_tids.end());
	if (so->pqPerfEnabled) {
		std::sort(dist_tids.begin(), dist_tids.at(klimit),
			[](const DistTid &a, const DistTid &b) {
				BlockNumber b1 = ItemPointerGetBlockNumberNoCheck(&a.tid);
				BlockNumber b2 = ItemPointerGetBlockNumberNoCheck(&b.tid);
				return b1 < b2;
			});
		BlockNumber blknum = ItemPointerGetBlockNumberNoCheck(&dist_tids[0].tid);
		Buffer buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, blknum, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		Page page = BufferGetPage(buf);
		for (size_t i = 0; i < klimit; ++i) {
			BlockNumber curblknum = ItemPointerGetBlockNumber(&dist_tids[i].tid);
			OffsetNumber curoffsetnum = ItemPointerGetOffsetNumber(&dist_tids[i].tid);
			if (curblknum != blknum) {
				UnlockReleaseBuffer(buf);
				buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, curblknum, RBM_NORMAL, NULL);
				LockBuffer(buf, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buf);
				blknum = curblknum;
			}

			ItemId lp = PageGetItemId(page, curoffsetnum);
			if (ItemIdIsUnused(lp)) {
				continue;
			}
			IndexTuple itup = (IndexTuple)PageGetItem(page, lp);
			bool isnull;
			Datum datum = index_getattr(itup, 1, so->tupdesc, &isnull);
			if (isnull) {
				continue;
			}
			FloatVector *vec = DatumGetFloatVector(datum);
			dist_tids[i].dist = so->procinfo(vec->x , queryvec->x, so->dimensions);
			if ((Pointer)vec != DatumGetPointer(datum)) {
				pfree(vec);
			}
			UnlockReleaseBuffer(buf);
		}
	} else {
		EState *estate = CreateExecutorState();
		ExprContext *econtext = GetPerTupleExprContext(estate);
		TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(scan->heapRelation));
		/* Arrange for econtext's scan tuple to be the tuple under test */
		econtext->ecxt_scantuple = slot;
		IndexInfo *indexInfo = BuildIndexInfo(scan->indexRelation);

		Datum values[INDEX_MAX_KEYS];
		bool isnull[INDEX_MAX_KEYS];
		FloatVector *norm_vec = InitFloatVector(so->dimensions);

		for (size_t i = 0; i < klimit; ++i) {
			scan->xs_ctup.t_self = dist_tids[i].tid;
			HeapTuple htuple = (HeapTuple)IndexFetchTuple(scan);
			if (htuple != NULL) {
				ExecStoreTuple(htuple, slot, InvalidBuffer, false);
				FormIndexDatum(indexInfo, slot, estate, values, isnull);
				Datum value0 = values[0]; /* for hybrid index case, vector is also the first column, so it's ok*/
				if (so->normprocinfo != NULL) {
					if (!AnnNormValue(so->normprocinfo, &value0, norm_vec)) {
						elog(WARNING, "ivfpq refine distance sort, normalize heap vector failed, so skip it");
						continue;
					}	
				}
				dist_tids[i].dist = so->procinfo(DatumGetFloatVector(value0)->x, queryvec->x, so->dimensions);
			}
		}
		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		pfree(norm_vec);
	}
	return klimit;
}

/*
 * Fetch the next tuple in the given scan
 */
bool
ivfgettuple_internal(IndexScanDesc scan, void *so_in, float *dist_out)
{
	IvfScanOpaque so = (IvfScanOpaque)so_in;
	if (so->first) {
		/* Count index scan for stats */
		pgstat_count_index_scan(scan->indexRelation);

		/* Safety check */
		if (scan->orderByData == NULL) {
			elog(ERROR, "cannot scan ivf index without order");
		}

		/* No items will match if null */
		if (scan->orderByData->sk_flags & SK_ISNULL) {
			return false;
		}

		Datum value = scan->orderByData->sk_argument;
		if (scan->with_limit) {
			int64 limitCount = scan->limit_count;
			if (so->ispq) {
				double kfactor = u_sess->attr.attr_storage.ivfpq_refine_k_factor;
				limitCount = (int64)(limitCount * kfactor);
			}
	    } else {
			if (so->ispq && so->pqPerfEnabled) {
				/* when store_original_vector is enabled, in pq sort stage we use index tid, then use this index tid to refine sort
				so we can not do full sort in this case */
				elog(ERROR, "cannot scan ivfpq index(cosine or inner product distance) without limit in sql");
			}
		}

		/* Value should not be compressed or toasted */
		Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
		Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));

		FloatVector *queryvec = DatumGetFloatVector(value);
		if (queryvec->dim != so->dimensions) {
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Incorrect dimension of query vector")));
		}
		GetScanLists(scan->indexRelation, so, queryvec);
		MemoryContext ctx = !USE_PARALLEL_QUERY ? CurrentMemoryContext :
			AllocSetContextCreate(IndexerCtx, "ivf parallel query context",
								  ALLOCSET_DEFAULT_SIZES, SHARED_CONTEXT);
		dist_tids_type dist_tids = GetScanItems(scan->indexRelation, so, queryvec, ctx);
		size_t res_size = dist_tids.size();
		
		if (so->ispq && scan->with_limit) {
			res_size = ivfPQRefineDistanceSort(scan, so, queryvec,
				scan->limit_offset, scan->limit_count, dist_tids);
		}

		if (USE_PARALLEL_QUERY) {
			if (res_size > 0) {
				so->dist_tids = (DistTid *)palloc(res_size * sizeof(DistTid));
				errno_t rc = memcpy_s(so->dist_tids, res_size * sizeof(DistTid),
					dist_tids.data(), res_size * sizeof(DistTid));
				securec_check(rc, "\0", "\0");
			}
			MemoryContextDelete(ctx);
		} else {
			so->dist_tids = dist_tids.data();
		}
		so->cur_pos = 0;
		so->sorted_pos = 0;
		so->size = res_size;
		so->first = false;
	}

	if (so->cur_pos >= so->size) {
		return false;
	}
	if (so->cur_pos >= so->sorted_pos) {
		size_t limit_count = std::min(25ul, size_t(scan->limit_count * 1.2));
		size_t next_pos = std::min(so->size, so->sorted_pos + limit_count);
		std::partial_sort(so->dist_tids + so->sorted_pos,
						  so->dist_tids + next_pos,
						  so->dist_tids + so->size,
						  [](const DistTid &a, const DistTid &b) { return a.dist < b.dist; });
		so->sorted_pos = next_pos;
	}
	scan->xs_ctup.t_self = so->dist_tids[so->cur_pos].tid;
	if (dist_out) {
		*dist_out = so->dist_tids[so->cur_pos].dist;
	}
	++so->cur_pos;
	return true;
}

/*
 * End a scan and release resources
 */
void
ivfendscan_internal(void *in_so)
{
	IvfScanOpaque so = (IvfScanOpaque)in_so;
	/* Release pin */
	if (BufferIsValid(so->buf)) {
		ReleaseBuffer(so->buf);
	}

	pairingheap_free(so->listQueue);
	pfree_ext(so->dist_tids);

	if (so->ispq) {
		so->pq->free_resourses();
		pfree(so->pq);
		for(int i = 0; i < so->probes; ++i) {
			if (so->lists[i].center) {
				pfree(so->lists[i].center);
			}
		}
	}

	pfree(so);
}
