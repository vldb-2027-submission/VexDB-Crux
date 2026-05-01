#include "postgres.h"
#include "access/amapi.h"
#include "access/hybridann/hybridann.h"
#include "catalog/index.h"

Datum hybridannbuild(PG_FUNCTION_ARGS)
{
    Relation heap = (Relation)PG_GETARG_POINTER(0);
    Relation index = (Relation)PG_GETARG_POINTER(1);
    IndexInfo *indexInfo = (IndexInfo *)PG_GETARG_POINTER(2);
    IndexBuildResult *result = hybridannbuild_internal(heap, index, indexInfo);
    PG_RETURN_POINTER(result);
}

Datum hybridannbuildempty(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    hybridannbuildempty_internal(index);
    PG_RETURN_VOID();
}

Datum hybridannbeginscan(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    int nkeys = PG_GETARG_INT32(1);
    int norderbys = PG_GETARG_INT32(2);

    IndexScanDesc scan = hybridannbeginscan_internal(rel, nkeys, norderbys);
    PG_RETURN_POINTER(scan);
}

Datum hybridannrescan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
    int nkeys = PG_GETARG_INT32(2);
    ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
    int norderbys = PG_GETARG_INT32(4);

    hybridannrescan_internal(scan, scankey, nkeys, orderbys, norderbys);
    PG_RETURN_VOID();
}

Datum hybridanngettuple(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanDirection direction = (ScanDirection)PG_GETARG_INT32(1);

    if (!scan) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("Invalid arguments for function hybridanngettuple")));
    }

    bool result = hybridanngettuple_internal(scan, direction);
    PG_RETURN_BOOL(result);
}

Datum hybridannendscan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    hybridannendscan_internal(scan);
    PG_RETURN_VOID();
}

Datum hybridanninsert(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    Datum * values = (Datum *)PG_GETARG_POINTER(1);
    bool *isnull = (bool *)PG_GETARG_POINTER(2);
    ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
    Relation heaprel = (Relation)PG_GETARG_POINTER(4);
    IndexUniqueCheck checkunique = (IndexUniqueCheck)PG_GETARG_INT32(5);

    bool result = hybridanninsert_internal(rel, values, isnull, ht_ctid, heaprel, checkunique); 

    PG_RETURN_BOOL(result);
}

Datum hybridannbulkdelete(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
    void *callback_state = (void *)PG_GETARG_POINTER(3);
    stats = hybridannbulkdelete_internal(info, stats, callback, callback_state);
    PG_RETURN_POINTER(stats);
}

Datum hybridannvacuumcleanup(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    stats = hybridannvacuumcleanup_internal(info, stats);
    PG_RETURN_POINTER(stats);
}

Datum hybridanncostestimate(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
    IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
    double loopcount = (double)PG_GETARG_FLOAT8(2);
    Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
    Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
    Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
    double *correlation = (double *)PG_GETARG_POINTER(6);

    hybridanncostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation);

    PG_RETURN_VOID();
}

Datum hybridannoptions(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    bytea *result = hybridannoptions_internal(reloptions, validate);

    if (result) {
        PG_RETURN_BYTEA_P(result);
    }
    PG_RETURN_NULL();
}
