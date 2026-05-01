#include "postgres.h"

#include <float.h>
#include <math.h>

#include "catalog/index.h"
#include "access/amapi.h"
#include "access/reloptions.h"
#include "commands/vacuum.h"
#include "access/hnsw/hnsw.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "securec.h"

Datum hnswbuild(PG_FUNCTION_ARGS);
Datum hnswbuildempty(PG_FUNCTION_ARGS);
Datum hnswinsert(PG_FUNCTION_ARGS);
Datum hnswbulkdelete(PG_FUNCTION_ARGS);
Datum hnswvacuumcleanup(PG_FUNCTION_ARGS);
Datum hnswcostestimate(PG_FUNCTION_ARGS);
Datum hnswoptions(PG_FUNCTION_ARGS);
Datum hnswvalidate(PG_FUNCTION_ARGS);
Datum hnswbeginscan(PG_FUNCTION_ARGS);
Datum hnswrescan(PG_FUNCTION_ARGS);
Datum hnswgettuple(PG_FUNCTION_ARGS);
Datum hnswendscan(PG_FUNCTION_ARGS);

/*
 * Estimate the cost of an index scan
 */
static void hnswcostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
    Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
    double *indexCorrelation)
{
    /* Never use index without order and limit */
    int nlimit_temp = root->limit_tuples;
    bool has_limit = nlimit_temp > 0;
    if (!has_limit) {
        PlannerInfo *cur = root;
        while (cur && cur->parse && !has_limit) {
            has_limit = cur->parse->limitCount;
            nlimit_temp = cur->limit_tuples;
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
    constexpr uint32 default_nlimit = 10;
    const uint32 nlimit = nlimit_temp <= 0 ? default_nlimit : nlimit_temp + 1;

    Relation index = index_open(path->indexinfo->indexoid, NoLock);
    Relation partRel = NULL;
    if (path->indexinfo->ispartitionedindex) {
        List *partitionIdList =  indexGetPartitionOidList(index);
        ListCell *cell = list_head(partitionIdList);
        if (cell) {
            Oid partitionOid = lfirst_oid(cell);
            Partition indexpart = partitionOpen(index, partitionOid, NoLock);
            partRel = partitionGetRelation(index, indexpart);
            partitionClose(index, indexpart, NoLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
        } else {
            index_close(index, NoLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
            elog(ERROR, "hnsw costestimate get partition oid faild by index oid(%d)", path->indexinfo->indexoid);
        }
    }
    int m = HnswGetM(partRel ? partRel : index);

    if (partRel) {
        releaseDummyRelation(&partRel);
    }
    index_close(index, NoLock);

    uint32 search_list_size = uint32(u_sess->attr.attr_storage.ef_search);
    if (search_list_size < nlimit) {
        search_list_size = nlimit;
    }
    /* the deafult opr cost is 5, but internal calc is a little bit faster */
    constexpr double opr_cost = 4;
    uint32 nnode = search_list_size * 1.1;
    uint32 nvec = HnswGetLayerM(m, 0) * search_list_size * 2;
    constexpr double scaling_factor = 0.55;
    constexpr double default_factor = 0.5;
    nvec *= path->indexinfo->tuples > 0 ?
        scaling_factor * log(path->indexinfo->tuples) / (log(m) * (1 + log(search_list_size))) :
        default_factor;
    double spc_seq_page_cost, spc_random_page_cost;
    get_tablespace_page_costs(path->indexinfo->reltablespace, &spc_random_page_cost,
                              &spc_seq_page_cost);
    double vec_read_cost = spc_seq_page_cost / 8;   /* vec buffer cost */
    vec_read_cost += spc_random_page_cost / 5;      /* disk read cost */
    constexpr Cost base_cost = 100;
    Cost startup_cost = base_cost + (nnode + 2) * spc_random_page_cost;
    startup_cost += nvec * (u_sess->attr.attr_sql.cpu_operator_cost * opr_cost + vec_read_cost);

    /* Adjust cost if needed since TOAST not included in seq scan cost */
    double ratio = path->indexinfo->tuples > 0 ? nvec / path->indexinfo->tuples : 1.0;
    if (ratio < 0.5) {
        double pages = nnode + 1;
        const double vec_rate = double(nvec) / path->indexinfo->tuples;
        constexpr double base_denoise_rate = 0.06;
        const double rate = Min(1, base_denoise_rate / vec_rate) + .4;
        pages += nvec * rate;
        if (pages > path->indexinfo->rel->pages) {
            double reduce_cost = (pages - path->indexinfo->rel->pages) * spc_seq_page_cost;
            startup_cost -= std::min(reduce_cost, startup_cost * 0.85);
            startup_cost = std::max(startup_cost, base_cost);
        }
    }
    startup_cost *= loop_count;
    Assert(path->path.rows > 0);
    while (search_list_size / nlimit / 1.25 < path->indexinfo->tuples / path->path.rows) {
        startup_cost -= base_cost;
        startup_cost *= 2.45;
        startup_cost += base_cost;
        search_list_size *= 2;
    }

    *indexStartupCost = startup_cost;
    *indexTotalCost = startup_cost + search_list_size * u_sess->attr.attr_sql.cpu_index_tuple_cost;
    *indexSelectivity = 1.0;
    *indexCorrelation = 0.0;

    if (path->indexinfo->ispartitionedindex) {
        double num_tuples = RELOPTINFO_LOCAL_FIELD(root, path->indexinfo->rel, tuples);
        num_tuples = std::max(1.0, num_tuples);
        *indexSelectivity = std::min(*indexTotalCost, search_list_size / num_tuples);
    }
}

static bytea *hnswoptions_internal(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"m", RELOPT_TYPE_INT, offsetof(HnswOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT, offsetof(HnswOptions, efConstruction)},
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(HnswOptions, parallel_workers)},
        {"quantizer", RELOPT_TYPE_STRING, offsetof(HnswOptions, qt_type_offset)},
        {"num_cluster", RELOPT_TYPE_INT64, offsetof(HnswOptions, num_cluster)},
        {"rabitq_keep_vecs", RELOPT_TYPE_BOOL, offsetof(HnswOptions, rabitq_keep_vecs)},
    };

    relopt_value *options = NULL;
    int           numoptions = 0;
    HnswOptions  *rdopts = NULL;

    options = parseRelOptions(reloptions, validate, RELOPT_KIND_HNSW, &numoptions);

    if (numoptions == 0) {
        return NULL;
    }

    rdopts = (HnswOptions *)allocateReloptStruct(sizeof(HnswOptions), options, numoptions);
    fillRelOptions((void *)rdopts, sizeof(HnswOptions), options, numoptions,
                   validate, tab, lengthof(tab));
    if (validate) {
        constexpr int64 min_ncluster = 100l;
        if (rdopts->num_cluster > 0 && rdopts->num_cluster < min_ncluster) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Invalid parameter value for \"num_cluster\": %ld, "
                                   "the value should be either 0 or not less than %ld",
                                   rdopts->num_cluster, min_ncluster)));
        }
    }
    return (bytea *)rdopts;

}

/*
 * Validate catalog entries for the specified operator class
 */
static bool hnswvalidate_internal(Oid opclassoid) { return true; }

Datum hnswbuild(PG_FUNCTION_ARGS)
{
    Relation heap = (Relation)PG_GETARG_POINTER(0);
    Relation index = (Relation)PG_GETARG_POINTER(1);
    if (pg_strcasecmp(NameStr(index->rd_am->amname), DEFAULT_HNSW_INDEX_TYPE) == 0) {
        ereport(NOTICE, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
                        errmsg("Index type %s is deprecated", DEFAULT_HNSW_INDEX_TYPE),
                        errhint("Please use GRAPH_INDEX instead")));
    }
    IndexInfo *indexinfo = (IndexInfo *)PG_GETARG_POINTER(2);
    IndexBuildResult *result = hnswbuild_internal(heap, index, indexinfo);
    PG_RETURN_POINTER(result);
}

Datum hnswbuildempty(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    if (pg_strcasecmp(NameStr(index->rd_am->amname), DEFAULT_HNSW_INDEX_TYPE) == 0) {
        ereport(NOTICE, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
                        errmsg("Index type %s is deprecated", DEFAULT_HNSW_INDEX_TYPE),
                        errhint("Please use GRAPH_INDEX instead")));
    }
    hnswbuildempty_internal(index);
    PG_RETURN_VOID();
}

Datum hnswinsert(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    Datum * values = (Datum *)PG_GETARG_POINTER(1);
    bool *isnull = (bool *)PG_GETARG_POINTER(2);
    ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
    Relation heaprel = (Relation)PG_GETARG_POINTER(4);
    bool result = hnswinsert_internal(rel, heaprel, values, isnull, ht_ctid, HNSW_METAPAGE_BLKNO, InvalidVectorIndex);
    PG_RETURN_BOOL(result);
}

Datum hnswbulkdelete(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
    void *callback_state = (void *)PG_GETARG_POINTER(3);
    int nparallel = HnswGetBuildParallel(info->index);
    stats = hnswbulkdelete_internal(info->index, stats, nparallel, callback, callback_state,
        HNSW_METAPAGE_BLKNO, NULL);

    PG_RETURN_POINTER(stats);
}

Datum hnswvacuumcleanup(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    stats = hnswvacuumcleanup_internal(info, stats);

    PG_RETURN_POINTER(stats);
}

Datum hnswcostestimate(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
    IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
    double loopcount = (double)PG_GETARG_FLOAT8(2);
    Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
    Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
    Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
    double *correlation = (double *)PG_GETARG_POINTER(6);
    hnswcostestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation);

    PG_RETURN_VOID();    
}

Datum hnswoptions(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    bytea *result = hnswoptions_internal(reloptions, validate);

    if (NULL != result)
        PG_RETURN_BYTEA_P(result);

    PG_RETURN_NULL();
}

Datum hnswvalidate(PG_FUNCTION_ARGS)
{
    Oid opclassoid = PG_GETARG_OID(0);
    bool result = hnswvalidate_internal(opclassoid);
    PG_RETURN_BOOL(result);
}

Datum hnswbeginscan(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    int nkeys = PG_GETARG_INT32(1);
    int norderbys = PG_GETARG_INT32(2);
    IndexScanDesc scan = hnswbeginscan_internal(rel, nkeys, norderbys);
    PG_RETURN_POINTER(scan);
}

Datum hnswrescan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
    int nkeys = PG_GETARG_INT32(2);
    ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
    int norderbys = PG_GETARG_INT32(4);
    hnswrescan_internal(scan, scankey, nkeys, orderbys, norderbys);
    PG_RETURN_VOID();
}


Datum hnswgettuple(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);

    if (NULL == scan) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid arguments for function hnswgettuple")));
    }

    /*
     * Index can be used to scan backward, but Postgres doesn't support
     * backward scan on operators
     */
    int ef = u_sess->attr.attr_storage.ef_search;
    if (scan->with_limit && ef < scan->limit_count) {
        ef = scan->limit_count;
    }
    bool result = hnswgettuple_internal(scan, scan->opaque, HNSW_METAPAGE_BLKNO, ef);
    PG_RETURN_BOOL(result);
}


Datum hnswendscan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    hnswendscan_internal(scan);
    PG_RETURN_VOID();
}
