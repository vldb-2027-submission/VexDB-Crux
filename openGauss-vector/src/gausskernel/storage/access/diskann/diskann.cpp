#include "postgres.h"
#include "access/amapi.h"
#include "access/diskann/diskann.h"
#include "access/roar/roar.h"
#include "catalog/index.h"

Datum diskannbuild(PG_FUNCTION_ARGS)
{
    Relation heap = (Relation)PG_GETARG_POINTER(0);
    Relation index = (Relation)PG_GETARG_POINTER(1);
    // if (pg_strcasecmp(NameStr(index->rd_am->amname), DEFAULT_DISKANN_INDEX_TYPE) == 0) {
    //     ereport(NOTICE, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
    //                     errmsg("Index type %s is deprecated", DEFAULT_DISKANN_INDEX_TYPE),
    //                     errhint("Please use GRAPH_INDEX instead")));
    // }
    IndexInfo *indexInfo = (IndexInfo *)PG_GETARG_POINTER(2);
    // IndexBuildResult *result = diskannbuild_internal(heap, index, indexInfo);
    IndexBuildResult *result = roarbuild_internal(heap, index, indexInfo);
    PG_RETURN_POINTER(result);
}

Datum diskannbuildempty(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    if (pg_strcasecmp(NameStr(index->rd_am->amname), DEFAULT_DISKANN_INDEX_TYPE) == 0) {
        ereport(NOTICE, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
                        errmsg("Index type %s is deprecated", DEFAULT_DISKANN_INDEX_TYPE),
                        errhint("Please use GRAPH_INDEX instead")));
    }
    diskannbuildempty_internal(index);
    PG_RETURN_VOID();
}

Datum diskannbeginscan(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    int nkeys = PG_GETARG_INT32(1);
    int norderbys = PG_GETARG_INT32(2);

    IndexScanDesc scan = roarbeginscan_internal(rel, nkeys, norderbys);
    PG_RETURN_POINTER(scan);
}

Datum diskannrescan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
    int nkeys = PG_GETARG_INT32(2);
    ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
    int norderbys = PG_GETARG_INT32(4);

    roarrescan_internal(scan, scankey, nkeys, orderbys, norderbys);
    PG_RETURN_VOID();
}

Datum diskanngettuple(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanDirection direction = (ScanDirection)PG_GETARG_INT32(1);

    if (!scan) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("Invalid arguments for function diskanngettuple")));
    }

    bool result = roargettuple_internal(scan);
    PG_RETURN_BOOL(result);
}

Datum diskannendscan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    roarendscan_internal(scan);
    PG_RETURN_VOID();
}

Datum diskanninsert(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    Datum * values = (Datum *)PG_GETARG_POINTER(1);
    bool *isnull = (bool *)PG_GETARG_POINTER(2);
    ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
    Relation heaprel = (Relation)PG_GETARG_POINTER(4);
    IndexUniqueCheck checkunique = (IndexUniqueCheck)PG_GETARG_INT32(5);

    bool result = diskanninsert_internal(rel, values, isnull, ht_ctid, heaprel, checkunique); 

    PG_RETURN_BOOL(result);
}

Datum diskannbulkdelete(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
    void *callback_state = (void *)PG_GETARG_POINTER(3);
    stats = diskannbulkdelete_internal(info, stats, callback, callback_state);
    PG_RETURN_POINTER(stats);
}

Datum diskannvacuumcleanup(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    stats = diskannvacuumcleanup_internal(info, stats);
    PG_RETURN_POINTER(stats);
}

Datum diskanncostestimate(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
    IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
    double loopcount = (double)PG_GETARG_FLOAT8(2);
    Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
    Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
    Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
    double *correlation = (double *)PG_GETARG_POINTER(6);

    diskanncostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation);

    PG_RETURN_VOID();
}

Datum diskannoptions(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    bytea *result = diskannoptions_internal(reloptions, validate);

    if (result) {
        PG_RETURN_BYTEA_P(result);
    }
    PG_RETURN_NULL();
}
