#include "access/relscan.h"
#include "pgstat.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/fmgroids.h"
#include "catalog/pg_cast.h"
#include "optimizer/cost.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/ann_utils.h"

using namespace ann_helper;

struct ScanOpaqueBase {
    Relation    index;
    ItemPointer tids;
    uint32      searchListSize;
    uint32      dimensions;
    int         currIndex;
    int         lastIndex;
    bool        first;
    bool        queryWithPQ;
};
using DiskAnnScanOpaque = ScanOpaqueBase;

/*
 * Prepare for an index scan
 */
IndexScanDesc diskannbeginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);

    Buffer buf = AnnLoadBuffer(index, DISKANN_METAPAGE_BLKNO);
    DiskAnnMetaPage *meta = (DiskAnnMetaPage *)(DiskAnnPageGetMeta(BufferGetPage(buf)));
    if (unlikely(meta->magicNumber != DISKANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("diskann index is not valid")));
    }

    const bool is_hybrid = meta->version == DISKANN_VERSION_TWO;
    if (is_hybrid) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Old Hybrid index data is not supported. Please upgrade database and reindex.")));
    }
    
    auto *so = (ScanOpaqueBase *)palloc(sizeof(ScanOpaqueBase));

    so->index = index;
    so->first = true;

    so->queryWithPQ = u_sess->attr.attr_storage.diskann_query_with_pq;
    so->searchListSize = u_sess->attr.attr_storage.ef_search;

    so->dimensions = meta->dimensions;

    so->tids = NULL;
    so->currIndex = so->lastIndex = 0;

    UnlockReleaseBuffer(buf);

    scan->opaque = so;
    return scan;
}

/*
 * Start or restart an index scan
 */
void diskannrescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    auto *so = (ScanOpaqueBase *)scan->opaque;
    so->first = true;
    so->searchListSize = u_sess->attr.attr_storage.ef_search;
    pfree_ext(so->tids);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }

    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
    }
}

static bool diskann_search(IndexScanDesc scan, ScanDirection dir)
{
    auto *so = (DiskAnnScanOpaque *) scan->opaque;

     /* Postgres doesn't support backward scan on operators */
    Assert(ScanDirectionIsForward(dir));

    if (so->first) {
        /* Count index scan for stats */
        pgstat_count_index_scan(scan->indexRelation);

        /* Safety check */
        if (scan->numberOfOrderBys <= 0 || scan->orderByData->sk_attno != 1) {
            ereport(ERROR, (errcode(ERRCODE_PLPGSQL_ERROR),
                            errmsg("cannot scan diskann index without vector order")));
        }
        if (scan->orderByData->sk_flags & SK_ISNULL) {
            return false;
        }

        Datum value = scan->orderByData->sk_argument;
        FloatVector *vector = DatumGetFloatVector(value);
        if (uint16(vector->dim) != so->dimensions) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Incorrect dimension of query vector")));
        }
        float *query = vector->x;
        if (scan->with_limit && so->searchListSize < (uint32)scan->limit_count + 1) {
            so->searchListSize = (uint32)scan->limit_count + 1;
        }
        so->tids = (ItemPointerData *) palloc(so->searchListSize * sizeof(ItemPointerData));
        so->currIndex = 0;

        DiskANNIndex index(so->index, so->queryWithPQ);
        so->lastIndex = index.size() == 0 ? -1 :
                        index.search(query, so->searchListSize, so->searchListSize,
                                     so->tids, nullptr, scan->numberOfKeys, scan->keyData);
        index.destroy();

        so->first = false;
        /* free if copy */
        if ((Pointer)vector != DatumGetPointer(value)) {
            pfree(vector);
        }
    }

    if (so->currIndex < so->lastIndex) {
        scan->xs_recheck = false;
        scan->xs_ctup.t_self = so->tids[so->currIndex];
        ++so->currIndex;
        return true;
    }

    if (so->lastIndex >= 0 && !RelationIsPartition(scan->indexRelation)) {
        ereport(WARNING, (errmsg("Run out of search list with potentially unfetched data. "
                                 "Set larger ef_search value to fetch potential data.")));
    }

    return false;
}

/*
 * Fetch the next tuple in the given scan
 */
bool diskanngettuple_internal(IndexScanDesc scan, ScanDirection dir)
{
    return diskann_search(scan, dir);
}

/*
 * End a scan and release resources
 */
void diskannendscan_internal(IndexScanDesc scan)
{
    ScanOpaqueBase *so = (ScanOpaqueBase *) scan->opaque;

    pfree_ext(so->tids);
    pfree(so);
    scan->opaque = NULL;
}

static size_t estimate_num_vec_access(double num_tuples, uint32 search_list_size)
{
    if (num_tuples <= 1000) {
        return Min(num_tuples, search_list_size * 3);
    }
    size_t res;
    if (num_tuples < 10'000) {
        res = 500 + (num_tuples - 1000) / 18;
    } else if (num_tuples < 100'000) {
        res = 1000 + (num_tuples - 10000) / 45;
    } else if (num_tuples < 1'000'000) {
        res =  3000 + (num_tuples - 100'000) / 450;
    } else if (num_tuples < 10'000'000) {
        res = 5000 + (num_tuples - 1'000'000) / 3000;
    } else {
        res = 8000 + (num_tuples - 10'000'000) / 10000;
    }
    return res * log2(search_list_size / 100.0 + 1);
}

static size_t estimate_vec_page_access(double num_tuples, uint32 search_list_size, uint32 vec_dim)
{
    const size_t nvec = estimate_num_vec_access(num_tuples, search_list_size);
    const double vec_rate = double(nvec) / num_tuples;
    constexpr double base_denoise_rate = 0.06;
    const double rate = Min(1, base_denoise_rate / vec_rate) + double(vec_dim) /
        disk_container::DiskVector<float>::n_data_per_block();
    return nvec * rate;
}

static size_t estimate_tuple_page_access(double num_tuples, uint32 search_list_size, bool has_indexquals)
{
    /* assume only 5% explored vector are actually get extanded */
    const size_t ntup = estimate_num_vec_access(num_tuples, search_list_size) / 20;
    const size_t num_node_page = num_tuples /
        disk_container::DiskVector<DiskAnnVamanaNode>::n_data_per_block() + 1;
    size_t node_access = has_indexquals ? ntup / 5 : search_list_size / 2;
    return ntup / 1.025 + Min(num_node_page, node_access); /* neighbor page + node pages */
}

static size_t estimate_meta_page_access(bool has_indexquals)
{
    return 4ul + has_indexquals ? 1 : 0; /* index, data, graph, node, and attr */
}


/*
 * Estimate the cost of an index scan
 */
void diskanncostestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
                                  Cost *indexStartupCost, Cost *indexTotalCost,
                                  Selectivity *indexSelectivity, double *indexCorrelation)
{
    *indexStartupCost = DBL_MAX;
    *indexTotalCost = DBL_MAX;
    *indexSelectivity = 0;
    *indexCorrelation = 0;
    /* Never use index without order and limit */
    bool has_limit = root->limit_tuples > 0;
    if (!has_limit) {
        PlannerInfo *cur = root;
        while (cur && cur->parse && !has_limit) {
            has_limit = cur->parse->limitCount;
            cur = cur->parent_root;
        }
    }

    Relation indexRel = index_open(path->indexinfo->indexoid, AccessShareLock);
    Relation partRel = NULL;
    if (path->indexinfo->ispartitionedindex) {
        List *partitionIdList = indexGetPartitionOidList(indexRel);
        ListCell *cell = list_head(partitionIdList);
        if (cell) {
            Oid partitionOid = lfirst_oid(cell);
            Partition indexpart = partitionOpen(indexRel, partitionOid, AccessShareLock);
            partRel = partitionGetRelation(indexRel, indexpart);
            partitionClose(indexRel, indexpart, AccessShareLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
        } else {
            index_close(indexRel, AccessShareLock);
            if (partitionIdList != NULL)  {
                releasePartitionOidList(&partitionIdList);
            }
            elog(ERROR, "diskann costestimate get partition oid faild by index oid(%d)",
                        path->indexinfo->indexoid);
        }
	}

	if (partRel) {
		releaseDummyRelation(&partRel);
	}
    
	index_close(indexRel, AccessShareLock);

    if (list_length(path->indexorderbys) <= 0) {
        return;
    }
    if (!has_limit && u_sess->attr.attr_sql.enable_seqscan) {
        return;
    }
    uint32 vec_dim = 768u;
    /* all vector operation cost is set to 5 */
    float4 opr_cost = 5;
    Node *node = (Node *)lfirst(list_head(path->indexorderbys));
    if (IsA(node, OpExpr)) {
        OpExpr *op = (OpExpr *)node;
        /* the first order by must be vector ops */
        if (list_length(op->args) != 2) {
            return;
        }
        set_opfuncid(op);
        if (op->opfuncid != F_COSINE_DISTANCE &&
            op->opfuncid != F_L2_DISTANCE &&
            op->opfuncid != F_FLOATVECTOR_NEGATIVE_INNER_PRODUCT) {
            return;
        }
        opr_cost = get_func_cost(op->opfuncid);
        Node *arg1 = (Node *)lfirst(list_head(op->args));
        Node *arg2 = (Node *)lfirst(list_tail(op->args));
        Var *var = NULL;
        if (IsA(arg1, Var)) {
            var = (Var *)arg1;
        } else if (IsA(arg2, Var)) {
            var = (Var *)arg2;
        }
        if (var) {
            vec_dim = var->vartypmod > 0 ? var->vartypmod : vec_dim;
        }
    } /* we don't deal with the order by expr being no-opexpr case, just assume it's a vec order by */

    double selec = 1.0;
    const bool has_indexquals = list_length(path->indexquals) > 0;
    if (has_indexquals) {
        List *quals = add_predicate_to_quals(path->indexinfo, path->indexquals, root);
        selec = clauselist_selectivity(root, quals, path->indexinfo->rel->relid, JOIN_INNER, NULL);
        /* potential mem leak on quals, but it's optimizer, we all know there are mem leak everywhere... */
    }
    uint32 search_list_size = size_t(u_sess->attr.attr_storage.ef_search);
    if (search_list_size < (uint32)root->limit_tuples + 1) {
        search_list_size = (uint32)root->limit_tuples + 1;
    }
    double num_tuples = RELOPTINFO_LOCAL_FIELD(root, path->indexinfo->rel, tuples);
    *indexSelectivity = path->indexinfo->ispartitionedindex ?
        std::min(selec, search_list_size / num_tuples) :
        selec;
    if (num_tuples * selec < search_list_size * 2) {
        return;
    }

    double seqcost, randomcost;
    get_tablespace_page_costs(path->indexinfo->reltablespace, &randomcost, &seqcost);
    size_t page_fetched = estimate_meta_page_access(has_indexquals) +
                          estimate_tuple_page_access(num_tuples, search_list_size, has_indexquals);
    page_fetched += estimate_vec_page_access(num_tuples, search_list_size, vec_dim) * Max(selec, 0.8);
    if (page_fetched > path->indexinfo->pages / 1.2) {
        page_fetched = path->indexinfo->pages / 1.2 + 1;
    }
    page_fetched *= loop_count;
    page_fetched = index_pages_fetched(page_fetched, path->indexinfo->pages, (double)path->indexinfo->pages,
                                       root, path->indexinfo->rel->isPartitionedTable);
    if (randomcost > seqcost) {
        constexpr size_t small_page_size = 15'000ul;
        constexpr double scale_factor = 0.0002;
        randomcost = LOGISTIC_FUNC(page_fetched, small_page_size, randomcost, seqcost, scale_factor);
    }

    Cost calc_cost =
        (u_sess->attr.attr_sql.cpu_operator_cost * opr_cost + u_sess->attr.attr_sql.cpu_tuple_cost) *
        estimate_num_vec_access(num_tuples, search_list_size);
    *indexStartupCost = page_fetched * randomcost + calc_cost;
    *indexTotalCost = *indexStartupCost + search_list_size * randomcost;
    *indexCorrelation = 0.0;    /* same with above */
}
