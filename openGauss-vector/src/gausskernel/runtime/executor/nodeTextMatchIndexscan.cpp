/**
 * Copyright ...
 */

#include "executor/node/nodeTextmatchindexscan.h"
#include "executor/node/nodeIndexscan.h"
#include "executor/executor.h"
#include "access/hbindex_am.h"
#include "access/hbucket_am.h"
#include "access/tupdesc.h"
#include "access/bm25/bm25_scan.h"
#include "access/bm25/bm25_expr.h"
#include "gstrace/executer_gstrace.h"

static TupleTableSlot *TextMatchIndexNext(TextMatchIndexScanState *node)
{
    Assert(ScanDirectionIsForward(node->ss.ps.state->es_direction) ||
           (ScanDirectionIsBackward(((IndexScan *)node->ss.ps.plan)->indexorderdir) &&
            ScanDirectionIsBackward(node->ss.ps.state->es_direction)));
    IndexScanDesc scandesc = node->iss_ScanDesc;
    ExprContext *econtext = node->ss.ps.ps_ExprContext;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    for (;;) {
        CHECK_FOR_INTERRUPTS();
        HeapTuple tuple = scan_handler_idx_getnext(scandesc, ForwardScanDirection, InvalidOid,
            InvalidBktId, &node->ss.ps.state->have_current_xact_date);
        if (!HeapTupleIsValid(tuple)) {
            break;
        }
        if (((TextMatchIndexScan *)node->ss.ps.plan)->has_score_output) {
            auto desc = RelationGetDescr(node->ss.ss_currentRelation);
            heap_deform_tuple(tuple, desc, slot->tts_values, slot->tts_isnull);
            auto *scanner = reinterpret_cast<bm25::BM25Scanner *>(scandesc->opaque);
            node->score = scanner->score / scanner->bm25_weight;
            slot->tts_values[desc->natts] = Float4GetDatum(node->score);
            slot->tts_isnull[desc->natts] = false;
            HeapTuple new_tuple = heap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
            new_tuple->t_self = tuple->t_self;
            // new_tuple->t_tableOid = tuple->t_tableOid;
            // new_tuple->t_xid_base = tuple->t_xid_base;
            // new_tuple->t_multi_base = tuple->t_multi_base;
#if defined(ENABLE_MULTIPLE_NODES) && defined(PGXC)
            if (TTS_SHOULDFREE_ROW(slot)) {
                pfree_ext(slot->tts_dataRow);
            }
            slot->tts_dataRow = NULL;
            slot->tts_dataLen = -1;
#endif /* defined(ENABLE_MULTIPLE_NODES) && defined(PGXC) */
            if (slot->tts_tuple) {
                heap_freetuple((HeapTuple)slot->tts_tuple);
            }
            slot->tts_tuple = new_tuple;
            slot->tts_flags &= ~(TTS_FLAG_EMPTY | TTS_FLAG_SHOULDFREEMIN | TTS_FLAG_SHOULDFREE);
            slot->tts_mintuple = NULL;
            slot->tts_nvalid = 0;
        } else {
            ExecStoreTuple(tuple, slot, scandesc->xs_cbuf, false);
        }

        if (GetIndexScanDesc(scandesc)->xs_recheck) {
            econtext->ecxt_scantuple = slot;
            ResetExprContext(econtext);
            if (!ExecQual(node->indexqualorig, econtext, false)) {
                /* Fails recheck, so drop it and loop back for another */
                InstrCountFiltered2(node, 1);
                continue;
            }
        }
        return slot;
    }
    return ExecClearTuple(slot);
}

static bool TextMatchIndexRecheck(TextMatchIndexScanState *node, TupleTableSlot *slot)
{
    ExprContext *econtext = node->ss.ps.ps_ExprContext;
    /* Does the tuple meet the indexqual condition? */
    econtext->ecxt_scantuple = slot;
    ResetExprContext(econtext);
    return ExecQual(node->indexqualorig, econtext, false);
}

static TupleTableSlot *ExecTextMatchIndexScan(PlanState *state)
{
    TextMatchIndexScanState *node = castNode(TextMatchIndexScanState, state);
    auto *scanner = reinterpret_cast<bm25::BM25Scanner *>(node->iss_ScanDesc->opaque);
    scanner->linfo.set_limit(node->with_limit, node->limit_offset, node->limit_count);

    /* If we have runtime keys and they've not already been set up, do it now. */
    if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady) {
        /*
         * Set a flag for partitioned table, so we can deal with it specially
         * when we rescan the partitioned table
         */
        if (node->ss.isPartTbl) {
            if (PointerIsValid(node->ss.partitions)) {
                node->ss.ss_ReScan = true;
                ExecReScan((PlanState *)node);
            }
        } else {
            ExecReScan((PlanState *)node);
        }
    }
    return ExecScan(&node->ss, (ExecScanAccessMtd)TextMatchIndexNext,
                    (ExecScanRecheckMtd)TextMatchIndexRecheck);
}

TextMatchIndexScanState *ExecInitTextMatchIndexScan(TextMatchIndexScan *node, EState *estate, int eflags)
{
    gstrace_entry(GS_TRC_ID_ExecInitIndexScan);
    TextMatchIndexScanState *index_state = makeNode(TextMatchIndexScanState);
    index_state->ss.ps.plan = (Plan *)node;
    index_state->ss.ps.state = estate;
    index_state->ss.isPartTbl = node->scan.isPartTbl;
    index_state->ss.currentSlot = 0;
    index_state->ss.partScanDirection = node->indexorderdir;
    index_state->ss.ps.ExecProcNode = ExecTextMatchIndexScan;

    ExecAssignExprContext(estate, &index_state->ss.ps);
    index_state->ss.ps.ps_vec_TupFromTlist = false;

    /*
     * initialize child expressions
     *
     * Note: we don't initialize all of the indexqual expression, only the
     * sub-parts corresponding to runtime keys (see below).  Likewise for
     * indexorderby, if any.  But the indexqualorig expression is always
     * initialized even though it will only be used in some uncommon cases ---
     * would be nice to improve that.  (Problem is that any SubPlans present
     * in the expression must be found now...)
     */
    if (estate->es_is_flt_frame) {
        index_state->ss.ps.qual =
            (List *)ExecInitQualByFlatten(node->scan.plan.qual, (PlanState *)index_state);
        index_state->indexqualorig =
            (List *)ExecInitQualByFlatten(node->indexqualorig, (PlanState *)index_state);
    } else {
        index_state->ss.ps.targetlist =
            (List *)ExecInitExprByRecursion((Expr *)node->scan.plan.targetlist, (PlanState *)index_state);
        index_state->ss.ps.qual =
            (List *)ExecInitExprByRecursion((Expr *)node->scan.plan.qual, (PlanState *)index_state);
        index_state->indexqualorig =
            (List *)ExecInitExprByRecursion((Expr *)node->indexqualorig, (PlanState *)index_state);
    }

    /* open the base relation and acquire appropriate lock on it. */
    Relation current_relation = ExecOpenScanRelation(estate, node->scan.scanrelid);

    index_state->ss.ss_currentRelation = current_relation;
    index_state->ss.ss_currentScanDesc = NULL; /* no heap scan here */
    /* tuple table initialization */
    ExecInitResultTupleSlot(estate, &index_state->ss.ps, current_relation->rd_tam_ops);
    ExecInitScanTupleSlot(estate, &index_state->ss, current_relation->rd_tam_ops);

    TupleDesc tupdesc = RelationGetDescr(current_relation);
    if (node->has_score_output) {
        tupdesc = CreateTupleDescCopy(RelationGetDescr(current_relation));
        TargetEntry *tle = (TargetEntry *)llast(node->indextlist);
        Assert(!tle->resjunk && IsA(tle->expr, FuncExpr) && ((FuncExpr *)tle->expr)->funcid == F_BM25_SCORE);
        ++tupdesc->natts;
        tupdesc = (TupleDesc)repalloc(tupdesc,
            offsetof(struct tupleDesc, attrs) + tupdesc->natts * sizeof(FormData_pg_attribute));
        TupleDescInitEntry(tupdesc, tupdesc->natts, tle->resname, FLOAT4OID, -1, 0);
        TupleDescInitEntryCollation(tupdesc, tupdesc->natts, ((const OpExpr *)tle->expr)->opcollid);
    }
    ExecAssignScanType(&index_state->ss, tupdesc);
    ExecAssignResultTypeFromTL(&index_state->ss.ps,
        index_state->ss.ss_ScanTupleSlot->tts_tupleDescriptor->td_tam_ops);
    ExecAssignScanProjectionInfo(&index_state->ss);
    Assert(index_state->ss.ps.ps_ResultTupleSlot->tts_tupleDescriptor->td_tam_ops);

    /*
     * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
     * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
     * references to nonexistent indexes.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        gstrace_exit(GS_TRC_ID_ExecInitIndexScan);
        return index_state;
    }

    /*
     * Open the index relation.
     *
     * If the parent table is one of the target relations of the query, then
     * InitPlan already opened and write-locked the index, so we can avoid
     * taking another lock here.  Otherwise we need a normal reader's lock.
     */
    bool relis_target = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
    index_state->iss_RelationDesc = index_open(node->indexid, relis_target ? NoLock : AccessShareLock);
    if (!IndexIsUsable(index_state->iss_RelationDesc->rd_index)) {
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("can't initialize index scans using unusable index \"%s\"",
                               RelationGetRelationName(index_state->iss_RelationDesc))));
    }

    index_state->iss_RuntimeKeysReady = false;
    index_state->iss_RuntimeKeys = NULL;
    index_state->iss_NumRuntimeKeys = 0;

    ExecIndexBuildScanKeys((PlanState *)index_state,
        index_state->iss_RelationDesc,
        node->indexqual,
        false,
        &index_state->iss_ScanKeys,
        &index_state->iss_NumScanKeys,
        &index_state->iss_RuntimeKeys,
        &index_state->iss_NumRuntimeKeys,
        NULL, /* no ArrayKeys */
        NULL);
    if (list_length(node->indexorderby) > 0) {
        index_state->iss_OrderByKeys = extract_bm25_orderby_scankey(node->indexorderby, estate,
            (PlanState *)index_state, index_state->iss_NumOrderByKeys, index_state->iss_RuntimeKeys,
            index_state->iss_NumRuntimeKeys);
    }

    /*
     * If we have runtime keys, we need an ExprContext to evaluate them. The
     * node's standard context won't do because we want to reset that context
     * for every tuple.  So, build another context just like the other one...
     * -tgl 7/11/00
     */
    if (index_state->iss_NumRuntimeKeys != 0) {
        ExprContext* stdecontext = index_state->ss.ps.ps_ExprContext;
        ExecAssignExprContext(estate, &index_state->ss.ps);
        index_state->iss_RuntimeContext = index_state->ss.ps.ps_ExprContext;
        index_state->ss.ps.ps_ExprContext = stdecontext;
    } else {
        index_state->iss_RuntimeContext = NULL;
    }

    /* deal with partition info */
    ExecInitIndexRelation(index_state, estate, eflags);

    /*
     * If no run-time keys to calculate, go ahead and pass the scankeys to the
     * index AM.
     */
    if (index_state->iss_ScanDesc == NULL) {
        index_state->ss.ps.stubType = PST_Scan;
    } else if (index_state->iss_NumRuntimeKeys == 0) {
        scan_handler_idx_rescan_local(index_state->iss_ScanDesc,
            index_state->iss_ScanKeys,
            index_state->iss_NumScanKeys,
            index_state->iss_OrderByKeys,
            index_state->iss_NumOrderByKeys);
    }
    gstrace_exit(GS_TRC_ID_ExecInitIndexScan);
    return index_state;
}

void ExecEndTextMatchIndexScan(TextMatchIndexScanState* state) { ExecEndIndexScan(state); }
void ExecReScanTextMatchIndexScan(TextMatchIndexScanState* state) { ExecReScanIndexScan(state); }

/* not likely to change */
void ExecTextMatchIndexMarkPos(TextMatchIndexScanState* state) { ExecIndexMarkPos(state); }
void ExecTextMatchIndexRestrPos(TextMatchIndexScanState* state) { ExecIndexRestrPos(state); }
