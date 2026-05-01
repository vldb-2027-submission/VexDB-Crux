/**
 * Copyright ...
 */

#include "access/annvector/ivf.h"
#include "access/hnsw/hnsw.h"
#include "access/diskann/diskann.h"
#include "access/bm25/index_inspect.h"
#include "access/bm25/bm25_internal.h"
#include "catalog/index.h"
#include "catalog/pg_partition_fn.h"
#include "commands/cluster.h"
#include "utils/fmgroids.h"
#include "utils/relcache.h"
#include "funcapi.h"

using namespace bm25;

constexpr int idx_num = 2;

void IndexInspectResult::destroy()
{
    for (size_t i = 0; i < nattr; ++i) {
        text *t = DatumGetTextP(attributes[i]);
        if (t) {
            pfree(t);
        }
        t = DatumGetTextP(contents[i]);
        if (t) {
            pfree(t);
        }
    }
    pfree_ext(attributes);
    pfree_ext(contents);
    nattr = 0;
}

void IndexInspectResult::append(IndexInspectResult &&other)
{
    size_t new_capacity = capacity;
    while (new_capacity < nattr + other.nattr) {
        new_capacity *= 2ul;
    }
    if (new_capacity > capacity) {
        capacity = new_capacity;
        attributes = (Datum *)repalloc(attributes, sizeof(Datum) * capacity);
        contents = (Datum *)repalloc(contents, sizeof(Datum) * capacity);
    }
    for (size_t i = 0; i < other.nattr; ++i) {
        attributes[nattr + i] = other.attributes[i];
        contents[nattr + i] = other.contents[i];
    }
    nattr += other.nattr;
    other.nattr = 0;
    other.destroy();
}

static IndexInspectResult *bm25_index_inspect(Relation index)
{
    IndexInspectResult *res = NEW IndexInspectResult();
    BM25Store store(index, false);
    store.inspect(*res);
    store.destroy();
    return res;
}

static IndexInspectResult *index_inspect(Relation index)
{
    if (index->rd_am->ambuild == F_BM25BUILD) {
        return bm25_index_inspect(index);
    }
    if (index->rd_am->ambuild == F_DISKANNBUILD &&
        strcmp(NameStr(index->rd_am->amname), DEFAULT_DISKANN_INDEX_TYPE) == 0) {
        return (IndexInspectResult *)diskann_inspect(index);
    }
    if (index->rd_am->ambuild == F_DISKANNBUILD &&
        strcmp(NameStr(index->rd_am->amname), DEFAULT_HYBRIDANN_INDEX_TYPE) == 0) {
        return (IndexInspectResult *)hybridann_inspect(index);
    }
    if (index->rd_am->ambuild == F_HNSWBUILD) {
        return (IndexInspectResult *)hnsw_inspect(index);
    }
    if (index->rd_am->ambuild == F_IVFPQBUILD) {
        return (IndexInspectResult *)ivfpq_inspect(index);
    }
    if (index->rd_am->ambuild == F_IVFFLATBUILD) {
        return (IndexInspectResult *)ivfflat_inspect(index);
    }
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index_inspect does not support %s", NameStr(index->rd_am->amname))));
    return NULL;
}

extern Datum index_inspect_oid(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        IndexInspectResult *res;
        Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
        Oid part_id = PG_GETARG_OID(1);
        if (OidIsValid(part_id)) {
            if (!RelationIsPartitioned(index)) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Index is not partitioned."),
                    errhint("Please input index name/oid only")));
            }
            Partition part = partitionOpen(index, part_id, AccessShareLock);
            Relation cindex = partitionGetRelation(index, part);
            res = index_inspect(cindex);
            releaseDummyRelation(&cindex);
            partitionClose(index, part, AccessShareLock);
        } else {
            if (!RelationIsPartitioned(index)) {
                res = index_inspect(index);
            } else {
                /* scan over all partition */
                List *l = indexGetPartitionList(index, AccessShareLock);
                res = NEW IndexInspectResult();
                foreach_cell(lc, l) {
                    Partition part = (Partition)lfirst(lc);
                    Relation cindex = partitionGetRelation(index, part);
                    res->append_attr("Partition OID");
                    res->fill_content("%u", part->pd_id);
                    const char *part_name = NameStr(part->pd_part->relname);
                    if (part_name && *part_name != '\0') {
                        res->append_attr("Partition Name");
                        res->fill_content("%s", part_name);
                    }
                    auto *temp = index_inspect(cindex);
                    res->append(std::move(*temp));
                    delete temp;
                    releaseDummyRelation(&cindex);
                }
                releasePartitionList(index, &l, AccessShareLock);
            }
        }
        index_close(index, AccessShareLock);

        auto tupdesc = CreateTemplateTupleDesc(idx_num, false);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "attribute", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "content", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = res ? res->nattr : 0;
        funcctx->user_fctx = res;
        MemoryContextSwitchTo(oldcontext);
        if (!funcctx->user_fctx) {
            SRF_RETURN_DONE(funcctx);
        }
    }

    funcctx = SRF_PERCALL_SETUP();
    if (funcctx->user_fctx && funcctx->call_cntr < funcctx->max_calls) {
        IndexInspectResult *res = (IndexInspectResult *)funcctx->user_fctx;
        Datum values[idx_num] = {res->attributes[funcctx->call_cntr], res->contents[funcctx->call_cntr]};
        bool nulls[idx_num] = {false};
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    pfree_ext(funcctx->user_fctx);
    SRF_RETURN_DONE(funcctx);
}

extern Datum index_inspect_name(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
        if (!RelationIsPartitioned(index)) {
            index_close(index, AccessShareLock);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Index is not partitioned."),
                    errhint("Please input index name/oid only")));
        }
        char *part_name = text_to_cstring(PG_GETARG_TEXT_P(1));
        Oid part_id = PartitionNameGetPartitionOid(PG_GETARG_OID(0), part_name,
            PART_OBJ_TYPE_INDEX_PARTITION, AccessShareLock, false, false, NULL, NULL, NoLock);
        pfree(part_name);
        Partition part = partitionOpen(index, part_id, AccessShareLock);
        Relation cindex = partitionGetRelation(index, part);
        IndexInspectResult *res = index_inspect(cindex);
        releaseDummyRelation(&cindex);
        partitionClose(index, part, AccessShareLock);
        index_close(index, AccessShareLock);
        auto tupdesc = CreateTemplateTupleDesc(idx_num, false);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "attribute", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "content", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = res ? res->nattr : 0;
        funcctx->user_fctx = res;
        MemoryContextSwitchTo(oldcontext);
        if (!funcctx->user_fctx) {
            SRF_RETURN_DONE(funcctx);
        }
    }

    funcctx = SRF_PERCALL_SETUP();
    if (funcctx->user_fctx && funcctx->call_cntr < funcctx->max_calls) {
        IndexInspectResult *res = (IndexInspectResult *)funcctx->user_fctx;
        Datum values[idx_num] = {res->attributes[funcctx->call_cntr], res->contents[funcctx->call_cntr]};
        bool nulls[idx_num] = {false};
        ++funcctx->call_cntr;
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    pfree_ext(funcctx->user_fctx);
    SRF_RETURN_DONE(funcctx);
}
