/**
 * Copyright ...
 * BM25 expression processing.
 */

#ifndef BM25_EXPR_H
#define BM25_EXPR_H

#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "nodes/execnodes.h"

extern Expr *find_bm25_expr(IndexOptInfo *index, Expr *expr);
extern Node *fix_bm25_orderby_qual(Node *node, IndexOptInfo *index);
extern ScanKey extract_bm25_orderby_scankey(List *indexorderby, EState *estate, PlanState *ps,
    int &num_orderby, IndexRuntimeKeyInfo *&runtimeKeys, int &numRuntimeKeys);

#endif /* BM25_EXPR_H */
