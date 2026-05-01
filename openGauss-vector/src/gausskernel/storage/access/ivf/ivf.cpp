#include "postgres.h"

#include <float.h>

#include "access/amapi.h"
#include "commands/vacuum.h"
#include "access/annvector/ivf.h"
#include "access/qasp/qasp.h"
#include "utils/guc.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "securec.h"

/*
 * Estimate the cost of an index scan
 */
static void
ivfcostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation, bool ispq)
{
	/* Never use index without order and limit */
    bool has_limit = root->limit_tuples > 0;
    if (!has_limit) {
        PlannerInfo *cur = root;
        while (cur && cur->parse && !has_limit) {
            has_limit = cur->parse->limitCount;
            cur = cur->parent_root;
        }
    }
    if (list_length(path->indexorderbys) <= 0 ||
        (!has_limit && u_sess->attr.attr_sql.enable_seqscan)) {
        *indexStartupCost = DBL_MAX;
        *indexTotalCost = DBL_MAX;
        *indexSelectivity = 0;
        *indexCorrelation = 0;
        return;
    }

	GenericCosts costs;
	int			lists;
	double		ratio;
	double		spc_seq_page_cost;
	double		spc_random_page_cost;
	double		sequentialRatio = 0.5;
	Relation	indexRel = index_open(path->indexinfo->indexoid, NoLock);
	Relation	partRel = NULL;
	MemSet(&costs, 0, sizeof(costs));

	if (path->indexinfo->ispartitionedindex) {
	    List *partitionIdList =  indexGetPartitionOidList(indexRel);
		ListCell *cell = list_head(partitionIdList);
		if (cell) {
			Oid partitionOid = lfirst_oid(cell);
       		Partition indexpart = partitionOpen(indexRel, partitionOid, NoLock);
			partRel = partitionGetRelation(indexRel, indexpart);
			partitionClose(indexRel, indexpart, NoLock);
			if (partitionIdList != NULL)  {
				releasePartitionOidList(&partitionIdList);
			}
		} else {
			index_close(indexRel, NoLock);
			if (partitionIdList != NULL)  {
				releasePartitionOidList(&partitionIdList);
			}
			elog(ERROR, "ivf costestimate get partition oid faild by index oid(%d)", path->indexinfo->indexoid);
		}
	}

	IvfMetaPage meta = IvfGetMetaPageData(partRel ? partRel : indexRel, IVF_METAPAGE_BLKNO, ispq);
	lists = meta->lists;
	pfree(meta);
	
	if (partRel) {
		releaseDummyRelation(&partRel);
	}

	index_close(indexRel, NoLock);

	/* Get the ratio of lists that we need to visit */
	ratio = ((double) u_sess->attr.attr_storage.ivf_probes) / lists;
	if (ratio > 1.0)
		ratio = 1.0;

	/*
	 * This gives us the subset of tuples to visit. This value is passed into
	 * the generic cost estimator to determine the number of pages to visit
	 * during the index scan.
	 */
	costs.numIndexTuples = path->indexinfo->tuples * ratio;
	costs.numIndexPages  = path->indexinfo->pages;

	genericcostestimate(root, path, loop_count, costs.numIndexTuples, &costs.indexStartupCost,
						&costs.indexTotalCost, &costs.indexSelectivity, &costs.indexCorrelation);

	get_tablespace_page_costs(path->indexinfo->reltablespace, &spc_random_page_cost, &spc_seq_page_cost);

	// /* Change some page cost from random to sequential */
	costs.indexTotalCost -= sequentialRatio * costs.numIndexPages * (spc_random_page_cost - spc_seq_page_cost);

	/* Startup cost is cost before returning the first row */
	costs.indexStartupCost = costs.indexTotalCost * ratio;

	/* Adjust cost if needed since TOAST not included in seq scan cost */
	double startupPages = costs.numIndexPages * ratio;
	if (startupPages > path->indexinfo->rel->pages)
	{
		// /* Change rest of page cost from random to sequential */
		costs.indexStartupCost -= (1 - sequentialRatio) * startupPages * (spc_random_page_cost - spc_seq_page_cost);

		/* Remove cost of extra pages */
		costs.indexStartupCost -= (startupPages - path->indexinfo->rel->pages) * spc_seq_page_cost;
	}
	/*
	 * If the list selectivity is lower than what is returned from the generic
	 * cost estimator, use that.
	 */
	if (ratio < costs.indexSelectivity)
		costs.indexSelectivity = ratio;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexStartupCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
}

/*
 * Parse and validate the reloptions
 */
static bytea *
ivfflatoptions_internal(Datum reloptions, bool validate)
{
	static const relopt_parse_elt ivf_parse_elems[] = {
		{"storage_type", RELOPT_TYPE_STRING, offsetof(IvfflatOptions, storage_type)},
        {"ivf_nlist", RELOPT_TYPE_INT, offsetof(IvfflatOptions, lists)}, // int类型
        {"ivf_nlist2", RELOPT_TYPE_INT, offsetof(IvfflatOptions, secondary_lists)}, // int类型
        {"compress", RELOPT_TYPE_BOOL, offsetof(IvfflatOptions, compress)},
        {"enable_toast", RELOPT_TYPE_BOOL, offsetof(IvfflatOptions, enable_toast)},
        {"parallel_workers", RELOPT_TYPE_BOOL, offsetof(IvfflatOptions, parallel_workers)},
	};

	relopt_value *options = NULL;
	int			options_num = 0;
	IvfflatOptions *rdopts = NULL;
	options = parseRelOptions(reloptions, validate, RELOPT_KIND_IVFFLAT, &options_num);

    if (options_num == 0) {
        return NULL;
    }

	rdopts = (IvfflatOptions *)allocateReloptStruct(sizeof(IvfflatOptions), options, options_num);
	fillRelOptions((void *) rdopts, sizeof(IvfflatOptions), options, options_num,
				   validate, ivf_parse_elems, lengthof(ivf_parse_elems));

	return (bytea *) rdopts;

}

/*
 * Parse and validate the reloptions
 */
static bytea *
ivfpqoptions_internal(Datum reloptions, bool validate)
{
	static const relopt_parse_elt ivf_parse_elems[] = {
		{"storage_type", RELOPT_TYPE_STRING, offsetof(IvfPQOptions, storage_type)},
        {"ivf_nlist", RELOPT_TYPE_INT, offsetof(IvfPQOptions, lists)}, // int类型
        {"ivf_nlist2", RELOPT_TYPE_INT, offsetof(IvfPQOptions, secondary_lists)}, // int类型
        {"compress", RELOPT_TYPE_BOOL, offsetof(IvfPQOptions, compress)},
        {"enable_toast", RELOPT_TYPE_BOOL, offsetof(IvfPQOptions, enable_toast)},
        {"parallel_workers", RELOPT_TYPE_BOOL, offsetof(IvfPQOptions, parallel_workers)},
        {"num_subquantizers", RELOPT_TYPE_INT, offsetof(IvfPQOptions, num_subquantizers)},
        {"nbits", RELOPT_TYPE_INT, offsetof(IvfPQOptions, nbits)},
        {"enable_performance_mode", RELOPT_TYPE_BOOL, offsetof(IvfPQOptions, enable_performance_mode)},
        {"by_residual", RELOPT_TYPE_BOOL, offsetof(IvfPQOptions, by_residual)},
	};

	relopt_value *options = NULL;
	int			options_num = 0;
	IvfPQOptions *rdopts = NULL;
	options = parseRelOptions(reloptions, validate, RELOPT_KIND_IVFPQ, &options_num);

    if (options_num == 0) {
        return NULL;
    }

	rdopts = (IvfPQOptions *)allocateReloptStruct(sizeof(IvfPQOptions), options, options_num);
	fillRelOptions((void *) rdopts, sizeof(IvfPQOptions), options, options_num,
				   validate, ivf_parse_elems, lengthof(ivf_parse_elems));

	return (bytea *) rdopts;

}

/*
 * Validate catalog entries for the specified operator class
 */
static bool
ivfvalidate_internal(Oid opclassoid)
{
	return true;
}


Datum ivfflatbuild(PG_FUNCTION_ARGS)
{
	Relation heap = (Relation)PG_GETARG_POINTER(0);
	Relation index = (Relation)PG_GETARG_POINTER(1);
	IndexInfo *indexinfo = (IndexInfo *)PG_GETARG_POINTER(2);
	IndexBuildResult *result = qaspbuild_internal(heap, index, indexinfo);
	PG_RETURN_POINTER(result);
}

Datum
ivfflatbuildempty(PG_FUNCTION_ARGS)
{
	Relation index = (Relation)PG_GETARG_POINTER(0);
	ivfflatbuildempty_internal(index);
	PG_RETURN_VOID();
}


Datum
ivfflatinsert(PG_FUNCTION_ARGS)
{
	Relation rel = (Relation)PG_GETARG_POINTER(0);
	Datum * values = (Datum *)PG_GETARG_POINTER(1);
	bool *isnull = (bool *)PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
	Metric metric = getIvfMetricType(rel, false);
	IvfInsertState * state = createIvfInsertState(rel, metric, false);
	bool result = ivfinsert_internal(rel, state, values, isnull, ht_ctid, false);
	pfree(state);
	PG_RETURN_BOOL(result);
}

Datum
ivfflatbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
	void *callback_state = (void *)PG_GETARG_POINTER(3);
	stats = ivfbulkdelete_internal(info->index, stats, callback, callback_state, false);
	PG_RETURN_POINTER(stats);
}

Datum
ivfflatvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
	stats = ivfvacuumcleanup_internal(info, stats);
	PG_RETURN_POINTER(stats);
}


Datum
ivfflatcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
	IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
	double loopcount = (double)PG_GETARG_FLOAT8(2);
	Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
	Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
	Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
	double *correlation = (double *)PG_GETARG_POINTER(6);
	// ivfcostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation, false);
	*startupcost = 0.1;
	*totalcost = 0.2;
	*selectivity = 0.3;
	*correlation = 0.4;
	// ivfcostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation, false);

	PG_RETURN_VOID();	
}


Datum
ivfflatoptions(PG_FUNCTION_ARGS)
{
	Datum reloptions = PG_GETARG_DATUM(0);
	bool validate = PG_GETARG_BOOL(1);
	bytea *result = ivfflatoptions_internal(reloptions, validate);

	if (NULL != result)
		PG_RETURN_BYTEA_P(result);

	PG_RETURN_NULL();
}


Datum
ivfflatvalidate(PG_FUNCTION_ARGS)
{
	Oid opclassoid = PG_GETARG_OID(0);
	bool result = ivfvalidate_internal(opclassoid);

	PG_RETURN_BOOL(result);
}


Datum
ivfflatbeginscan(PG_FUNCTION_ARGS)
{
	Relation rel = (Relation)PG_GETARG_POINTER(0);
	int nkeys = PG_GETARG_INT32(1);
	int norderbys = PG_GETARG_INT32(2);
	// IndexScanDesc scan = ivfbeginscan_internal(rel, nkeys, norderbys, false);
	IndexScanDesc scan = qaspbeginscan_internal(rel, nkeys, norderbys);
	PG_RETURN_POINTER(scan);
}


Datum
ivfflatrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
	int nkeys = PG_GETARG_INT32(2);
	ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
	int norderbys = PG_GETARG_INT32(4);
	qasprescan_internal(scan, scankey, nkeys, orderbys, norderbys);
	PG_RETURN_VOID();
}


Datum
ivfflatgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	if (NULL == scan)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Invalid arguments for function ivfflatgettuple")));
	
	// bool result = ivfgettuple_internal(scan, scan->opaque);
	bool result = qaspgettuple_internal(scan, Act_cluster_num);
	PG_RETURN_BOOL(result);
}


Datum
ivfflatendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	qaspendscan_internal(scan);
	scan->opaque = NULL;
	PG_RETURN_VOID();
}


Datum ivfpqbuild(PG_FUNCTION_ARGS)
{
	Relation heap = (Relation)PG_GETARG_POINTER(0);
	Relation index = (Relation)PG_GETARG_POINTER(1);
	IndexInfo *indexinfo = (IndexInfo *)PG_GETARG_POINTER(2);
	IndexBuildResult *result = ivfpqbuild_internal(heap, index, indexinfo);
	PG_RETURN_POINTER(result);
}

Datum
ivfpqbuildempty(PG_FUNCTION_ARGS)
{
	Relation index = (Relation)PG_GETARG_POINTER(0);
	ivfpqbuildempty_internal(index);
	PG_RETURN_VOID();
}

Datum
ivfpqvalidate(PG_FUNCTION_ARGS)
{
	Oid opclassoid = PG_GETARG_OID(0);
	bool result = ivfvalidate_internal(opclassoid);

	PG_RETURN_BOOL(result);
}


Datum
ivfpqbeginscan(PG_FUNCTION_ARGS)
{
	Relation rel = (Relation)PG_GETARG_POINTER(0);
	int nkeys = PG_GETARG_INT32(1);
	int norderbys = PG_GETARG_INT32(2);
	IndexScanDesc scan = ivfbeginscan_internal(rel, nkeys, norderbys, true);
	PG_RETURN_POINTER(scan);
}


Datum
ivfpqrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
	int nkeys = PG_GETARG_INT32(2);
	ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
	int norderbys = PG_GETARG_INT32(4);
	ivfrescan_internal(scan, scankey, nkeys, orderbys, norderbys);
	PG_RETURN_VOID();
}


Datum
ivfpqgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	if (NULL == scan) {
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 	errmsg("Invalid arguments for function ivfpqgettuple")));
	}
	
	bool result = ivfgettuple_internal(scan, scan->opaque);
	PG_RETURN_BOOL(result);
}


Datum
ivfpqendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
	ivfendscan_internal(scan->opaque);
	scan->opaque = NULL;
	PG_RETURN_VOID();
}

Datum
ivfpqinsert(PG_FUNCTION_ARGS)
{
	Relation rel = (Relation)PG_GETARG_POINTER(0);
	Datum * values = (Datum *)PG_GETARG_POINTER(1);
	bool *isnull = (bool *)PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
	Metric metric = getIvfMetricType(rel, true);
	IvfInsertState * state = createIvfInsertState(rel, metric, true);
	bool result = ivfinsert_internal(rel, state, values, isnull, ht_ctid, true);
	pfree(state);
	PG_RETURN_BOOL(result);
}

Datum
ivfpqbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
	void *callback_state = (void *)PG_GETARG_POINTER(3);
	stats = ivfbulkdelete_internal(info->index, stats, callback, callback_state, true);
	PG_RETURN_POINTER(stats);
}

Datum
ivfpqvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
	stats = ivfvacuumcleanup_internal(info, stats);
	PG_RETURN_POINTER(stats);
}


Datum
ivfpqcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
	IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
	double loopcount = (double)PG_GETARG_FLOAT8(2);
	Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
	Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
	Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
	double *correlation = (double *)PG_GETARG_POINTER(6);
	ivfcostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation, true);

	PG_RETURN_VOID();	
}


Datum
ivfpqoptions(PG_FUNCTION_ARGS)
{
	Datum reloptions = PG_GETARG_DATUM(0);
	bool validate = PG_GETARG_BOOL(1);
	bytea *result = ivfpqoptions_internal(reloptions, validate);

	if (NULL != result)
		PG_RETURN_BYTEA_P(result);

	PG_RETURN_NULL();
}