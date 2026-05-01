#include <vtl/vector>
#include <vtl/hashtable>

#include "postgres.h"
#include "access/bm25/bm25_expr.h"
#include "access/annvector/sparsevector.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "optimizer/paths.h"
#include "optimizer/var.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "executor/executor.h"

static Expr *is_const_expr(Expr *expr)
{
    switch nodeTag(expr) {
        case T_Const:
            return expr;
        case T_RelabelType:
            return is_const_expr(((RelabelType *)expr)->arg);
        case T_Param: {
            Param *p = (Param *)expr;
            if (p->paramkind == PARAM_EXTERN) {
                if (p->paramtype == FLOAT4OID || p->paramtype == FLOAT8OID) {
                    return expr;
                }
            }
            /* we don't support sublink or internal case */
        } /* fall through */
        case T_FuncExpr:
        case T_OpExpr:
        default:
            return NULL;
    }
}

static double extract_const_value(Expr *expr, const EState *estate)
{
    if (unlikely(IsA(expr, RelabelType))) {
        expr = ((RelabelType *)expr)->arg;
    }
    switch nodeTag(expr) {
        case T_Const: {
            Const *c = (Const *)expr;
            if (c->consttype == FLOAT4OID) {
                return DatumGetFloat4(c->constvalue);
            } else if (c->consttype == FLOAT8OID) {
                return DatumGetFloat8(c->constvalue);
            } else {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("FULLTEXT index order by only supports float4/float8 constant")));
            }
        }
        case T_Param: {
            Param *p = (Param *)expr;
            if (p->paramkind == PARAM_EXTERN || p->paramkind == PARAM_EXEC) {
                const ParamExternData &prm = estate->es_param_list_info->params[p->paramid - 1];
                if (prm.isnull) {
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("FULLTEXT index order by does not support null constant")));
                }
                Datum value = prm.value;
                if (p->paramtype == FLOAT4OID) {
                    return DatumGetFloat4(value);
                }
                if (p->paramtype == FLOAT8OID) {
                    return DatumGetFloat8(value);
                }
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("FULLTEXT index order by only supports float4/float8 constant")));
            }
        }
        default:
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("FULLTEXT index order by found unsupported const type %d", nodeTag(expr))));
    }
    return 0.0; /* keep compiler happy */
}

/* reserved for best_field implementation */
static bool index_match_operand(Node *expr, int indexcol, IndexOptInfo *index)
{
    return match_index_to_operand(expr, indexcol, index);
}

/* has_bm25_score needs to be false when called */
static Expr *find_bm25_expr_helper(IndexOptInfo *index, Expr *expr, bool &has_bm25_score)
{
    switch nodeTag(expr) {
        case T_RelabelType:
            return find_bm25_expr_helper(index, ((RelabelType *)expr)->arg, has_bm25_score);
        case T_FuncExpr:
            if (((FuncExpr *)expr)->funcid == F_BM25_SCORE) {
                has_bm25_score = true;
                return expr;
            }
            return NULL;
        case T_OpExpr: {
            OpExpr *op = (OpExpr *)expr;
            switch (op->opfuncid) {
                case F_SPARSEVECTOR_COSINE_DISTANCE:
                case F_SPARSEVECTOR_NEGATIVE_INNER_PRODUCT: {
                    Assert(list_length(op->args) == 2);
                    Node *left = linitial_node(Node, op->args);
                    Node *right = llast_node(Node, op->args);
                    for (int indexcol = 0; indexcol < index->nkeycolumns; ++indexcol) {
                        if (!op_in_opfamily(op->opno, index->opfamily[indexcol])) {
                            continue;
                        }
                        if (index_match_operand(left, indexcol, index) &&
                            !contain_var_clause(right)) {
                            return expr;
                        }
                        if (index_match_operand(right, indexcol, index) &&
                            !contain_var_clause(left)) {
                            OpExpr *new_op = makeNode(OpExpr);
                            errno_t rc = memcpy_s(new_op, sizeof(OpExpr), op, sizeof(OpExpr));
                            securec_check(rc, "\0", "\0");
                            /* we do not need to commute sparsevector dist ops */
                            new_op->args = list_make2(right, left);
                            return expr;
                        }
                    }
                } break;
                case F_FLOAT4PL:
                case F_FLOAT8PL:
                case F_FLOAT48PL:
                case F_FLOAT84PL:
                case F_FLOAT4MI:
                case F_FLOAT8MI:
                case F_FLOAT48MI:
                case F_FLOAT84MI:
                    Assert(list_length(op->args) == 2);
                    foreach_cell (lc, op->args) {
                        if (!find_bm25_expr_helper(index, lfirst_node(Expr, lc), has_bm25_score) &&
                            !is_const_expr(lfirst_node(Expr, lc))) {
                            return NULL;
                        }
                    }
                    return expr;
                case F_FLOAT4MUL:
                case F_FLOAT8MUL:
                case F_FLOAT48MUL:
                case F_FLOAT84MUL: {
                    Assert(list_length(op->args) == 2);
                    bool has_const = false;
                    bool has_bm25 = false;
                    foreach_cell (lc, op->args) {
                        if (!has_const) {
                            if (is_const_expr(lfirst_node(Expr, lc))) {
                                has_const = true;
                                continue;
                            }
                        }
                        if (!has_bm25) {
                            if (find_bm25_expr_helper(index, lfirst_node(Expr, lc),
                                                      has_bm25_score)) {
                                has_bm25 = true;
                                continue;
                            }
                        }
                    }
                    if (has_const && has_bm25) {
                        return expr;
                    }
                    if (has_bm25 && has_bm25_score) {
                        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("BM25 score order by only supports multiplying by constant")));
                    }
                } break;
                case F_FLOAT4DIV:
                case F_FLOAT8DIV:
                case F_FLOAT48DIV:
                case F_FLOAT84DIV: {
                    Assert(list_length(op->args) == 2);
                    Expr *left = linitial_node(Expr, op->args);
                    Expr *right = llast_node(Expr, op->args);
                    if (find_bm25_expr_helper(index, left, has_bm25_score) &&
                        is_const_expr(right)) {
                        return expr;
                    } else if (find_bm25_expr_helper(index, right, has_bm25_score) &&
                               has_bm25_score) {
                        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("BM25 score order by only supports dividing by constant")));
                    }
                } break;
                case F_FLOAT4UM:
                case F_FLOAT8UM:
                    Assert(list_length(op->args) == 1);
                    if (find_bm25_expr_helper(index, linitial_node(Expr, op->args), has_bm25_score)) {
                        return expr;
                    };
                    break;
                default:
                    break;
            }
        } /* fall through */
        default:
            return NULL;
    }
}

Expr *find_bm25_expr(IndexOptInfo *index, Expr *expr)
{
    bool has_bm25_score = false;
    return find_bm25_expr_helper(index, expr, has_bm25_score);
}

static Node *fix_bm25_orderby_qual_mutator(Node *node, IndexOptInfo *index)
{
    if (!node) {
        return NULL;
    }
    if (IsA(node, RelabelType)) {
        node = (Node*)((RelabelType*)node)->arg;
    }
    if (IsA(node, Var) && ((Var*)node)->varno == index->rel->relid) {
        AttrNumber indexcol = -1;
        for (int i = 0; i < index->nkeycolumns; ++i) {
            if (((Var*)node)->varattno == index->indexkeys[i]) {
                indexcol = i;
                break;
            }
        }
        if (unlikely(indexcol < 0)) {
            ereport(ERROR, (errmodule(MOD_OPT),
                    errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                    errmsg("index key does not match expected index column")));
        }
        Var *result = (Var *)copyObject(node);
        result->varno = INDEX_VAR;
        result->varattno = indexcol + 1;
        return (Node *)result;
    }
    ListCell *indexpr_item = list_head(index->indexprs);
    for (int indexcol = 0; indexcol < index->ncolumns; ++indexcol) {
        if (index->indexkeys[indexcol] != 0) {
            continue;
        }
        if (unlikely(indexpr_item == NULL)) {
            ereport(ERROR, (errmodule(MOD_OPT),
                    errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("too few entries in indexprs list")));
        }

        Node *indexkey = lfirst_node(Node, indexpr_item);
        if (indexkey && IsA(indexkey, RelabelType)) {
            indexkey = (Node *)((RelabelType *)indexkey)->arg;
        }
        if (indexkey && IsA(indexkey, PrefixKey)) {
            indexkey = (Node *)((PrefixKey *)indexkey)->arg;
        }
        if (equal(node, indexkey)) {
            return (Node *)makeVar(INDEX_VAR, indexcol + 1,
                                   exprType(lfirst_node(Node, indexpr_item)), -1,
                                   exprCollation(lfirst_node(Node, indexpr_item)), 0);
        }
        indexpr_item = lnext(indexpr_item);
    }
    return expression_tree_mutator(node, (Node *(*)(Node *, void *))fix_bm25_orderby_qual_mutator,
                                   (void *)index, false);
}

Node *fix_bm25_orderby_qual(Node *node, IndexOptInfo *index)
    { return fix_bm25_orderby_qual_mutator(node, index); }

struct OrderByExploreKey {
    AttrNumber attno;
    int runtime_offset;
    SparseVector *sv;

    bool operator==(const OrderByExploreKey &other) const {
        if (runtime_offset > 0 || other.runtime_offset > 0) {
            return runtime_offset == other.runtime_offset && attno == other.attno;
        }
        if (!sv) {
            return other.sv == NULL && attno == other.attno;
        }
        if (!other.sv) {
            return false;
        }
        return attno == other.attno && sv->operator==(*other.sv);
    }
    struct Hasher {
        uint32 operator()(const OrderByExploreKey &key) const noexcept {
            if (!key.sv) {
                return (uint32)key.attno + key.runtime_offset;
            }
            uint32 hash = SparseVectorHasher()(key.sv);
            hash ^= (0x9e3779b9u + (hash << 6) + (hash >> 2) + (((uint32)key.attno) >> 4) +
                     (key.attno << 4));
            hash ^= key.runtime_offset;
            return hash;
        }
    };
};
struct OrderByExploreValue {
    Oid func_id;
    double weight;
    constexpr OrderByExploreValue(double _weight, Oid _func_id)
        : func_id(_func_id),
          weight(_weight) {}
};
using entry_set = UnorderedMap<OrderByExploreKey, OrderByExploreValue, OrderByExploreKey::Hasher>;

struct ExtractionHelperContext {
    EState *estate;
    PlanState *ps;
    IndexRuntimeKeyInfo *runtime_keys;
    int n_runtime_keys;
    int max_runtime_keys;
    entry_set explore_entries;

    void destroy() { ann_helper::optional_destroy(explore_entries); }
};

static void extract_bm25_orderby_helper(Expr *orderby_expr, double base, double coeff,
    ExtractionHelperContext &context)
{
    if (IsA(orderby_expr, FuncExpr)) {
        OrderByExploreKey entry_key{InvalidAttrNumber, -1, NULL};
        double weight = base + coeff;
        auto [it, inserted] = context.explore_entries.emplace(std::move(entry_key), weight, F_BM25_SCORE);
        if (!inserted) {
            it->second.weight += weight;
            Assert(it->second.func_id == F_BM25_SCORE);
        }
        return;
    }

    bool need_negate = false;
    OpExpr *op = (OpExpr *)orderby_expr;
    switch (op->opfuncid) {
        case F_SPARSEVECTOR_COSINE_DISTANCE:
        case F_SPARSEVECTOR_NEGATIVE_INNER_PRODUCT: {
            int runtime_key_offset = -1;
            Var *left = linitial_node(Var, op->args);
            if (IsA(left, RelabelType)) {
                left = (Var *)((RelabelType *)left)->arg;
            }
            Expr *right = llast_node(Expr, op->args);
            if (IsA(right, RelabelType)) {
                right = ((RelabelType *)right)->arg;
            }
            if (!IsA(left, Var)) {
                Assert(IsA(right, Var));
                Expr *temp = (Expr *)left;
                left = (Var *)right;
                right = temp;
            }
            SparseVector *sv;
            if (IsA(right, Const)) {
                Const *c = (Const *)right;
                sv = c->constisnull ? NULL : DatumGetSparseVector(c->constvalue);
            } else {
                auto *&runtime_keys = context.runtime_keys;
                for (int i = 0; i < context.n_runtime_keys; ++i) {
                    if (!runtime_keys[i].key_toastable) {
                        continue;
                    }
                    if (equal(right, runtime_keys[i].key_expr)) {
                        runtime_key_offset = i;
                        goto set_runtime_end;
                    }
                }
                if (context.n_runtime_keys >= context.max_runtime_keys) {
                    if (context.max_runtime_keys == 0) {
                        constexpr int init_max_key = 8;
                        runtime_keys = (IndexRuntimeKeyInfo *)palloc(
                            init_max_key * sizeof(IndexRuntimeKeyInfo));
                        context.max_runtime_keys = init_max_key;
                    } else {
                        int target = context.max_runtime_keys * 2;
                        runtime_keys = (IndexRuntimeKeyInfo *)repalloc(
                            runtime_keys, target * sizeof(IndexRuntimeKeyInfo));
                        context.max_runtime_keys = target;
                    }
                }
                runtime_keys[context.n_runtime_keys].key_expr = ExecInitExpr(right, context.ps);
                runtime_keys[context.n_runtime_keys].key_toastable = true;
                runtime_key_offset = context.n_runtime_keys;
                ++context.n_runtime_keys;
set_runtime_end:
                sv = NULL;
            }
            OrderByExploreKey entry_key{left->varattno, runtime_key_offset, sv};
            double weight = base + coeff;
            auto [it, inserted] =
                context.explore_entries.emplace(std::move(entry_key), weight, op->opfuncid);
            if (!inserted) {
                it->second.weight += weight;
                Assert(it->second.func_id == op->opfuncid);
            }
        } break;
        case F_FLOAT4MI:
        case F_FLOAT8MI:
        case F_FLOAT48MI:
        case F_FLOAT84MI:
            need_negate = true;
            /* fall through */
        case F_FLOAT4PL:
        case F_FLOAT8PL:
        case F_FLOAT48PL:
        case F_FLOAT84PL: {
            Assert(list_length(op->args) == 2);
            Expr *left = linitial_node(Expr, op->args);
            Expr *right = llast_node(Expr, op->args);
            bool left_c = is_const_expr(left);
            bool right_c = is_const_expr(right);
            if (left_c ^ right_c) {
                Expr *c = left_c ? left : right;
                Expr *other = right_c ? left : right;
                double new_coeff = extract_const_value(c, context.estate) * base;
                coeff += need_negate ? -new_coeff : new_coeff;
                extract_bm25_orderby_helper(other, base, coeff, context);
                return;
            }
            Assert(!left_c && !right_c);
            extract_bm25_orderby_helper(left, base, coeff, context);
            extract_bm25_orderby_helper(right, need_negate ? -base : base, coeff, context);
            return;
        }
        case F_FLOAT4MUL:
        case F_FLOAT8MUL:
        case F_FLOAT48MUL:
        case F_FLOAT84MUL: {
            Assert(list_length(op->args) == 2);
            Expr *c;
            Expr *other;
            if (is_const_expr(llast_node(Expr, op->args))) {
                c = llast_node(Expr, op->args);
                other = linitial_node(Expr, op->args);
            } else {
                c = linitial_node(Expr, op->args);
                Assert(is_const_expr(c));
                other = llast_node(Expr, op->args);
                if (need_negate) {
                    base = -base;
                }
            }
            Assert(!is_const_expr(other));
            base *= extract_const_value(c, context.estate);
            extract_bm25_orderby_helper(other, base, coeff, context);
            return;
        }
        case F_FLOAT4DIV:
        case F_FLOAT8DIV:
        case F_FLOAT48DIV:
        case F_FLOAT84DIV: {
            Assert(list_length(op->args) == 2);
            Expr *left = linitial_node(Expr, op->args);
            Expr *right = llast_node(Expr, op->args);
            Assert(is_const_expr(left));
            base /= extract_const_value(left, context.estate);
            extract_bm25_orderby_helper(right, base, coeff, context);
            return;
        }
        case F_FLOAT4UM:
        case F_FLOAT8UM:
            Assert(list_length(op->args) == 1);
            extract_bm25_orderby_helper(linitial_node(Expr, op->args), -base, coeff, context);
            return;
        default:
            break;
    }
}

static ScanKey extract_bm25_orderby_scankey(Expr *orderby_expr, EState *estate, PlanState *ps,
    int &num_orderby, IndexRuntimeKeyInfo *&runtime_keys, int &n_runtime_keys)
{
    ExtractionHelperContext context{estate, ps, runtime_keys, n_runtime_keys, n_runtime_keys};
    extract_bm25_orderby_helper(orderby_expr, 1.0, 0.0, context);
    runtime_keys = context.runtime_keys;
    n_runtime_keys = context.n_runtime_keys;
    if (context.explore_entries.empty()) {
        context.destroy();
        num_orderby = 0;
        return NULL;
    }
    Vector<int> invalidated_offset(context.explore_entries.size());
    ScanKey sk = (ScanKey)palloc(sizeof(ScanKeyData) * context.explore_entries.size());
    ScanKey current_sk = sk;
    for (const auto &[entry_key, entry_value] : context.explore_entries) {
        float weight = entry_value.weight;
        if (weight == 0) {
            if (entry_key.runtime_offset >= 0) {
                invalidated_offset.push_back(entry_key.runtime_offset);
            }
            continue;
        }
        if (isnanf(weight) || isinff(weight)) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("FULLTEXT index order by got overflow weight %f for attribute %hd",
                       entry_value.weight, entry_key.attno)));
        }
        if (weight < 0) {
            if (entry_value.func_id == F_BM25_SCORE) {
                weight = -weight;
            } else {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("FULLTEXT index order by got negative weight %f for attribute %hd",
                           weight, entry_key.attno)));
            }
        } else if (entry_value.func_id == F_BM25_SCORE && IsA(orderby_expr, FuncExpr)) {
            ereport(WARNING,
                    (errcode(ERRCODE_INVALID_OPERATION),
                        errmsg("Scoring function bm25_score() only support negative weight, "
                               "forcing reverse ordering")));
        }

        current_sk->sk_flags = SK_ORDER_BY;
        current_sk->sk_attno = entry_key.attno;
        current_sk->sk_strategy = InvalidStrategy;
        current_sk->sk_subtype = InvalidOid;
        current_sk->sk_weight = weight;
        current_sk->sk_func.fn_addr = NULL;
        current_sk->sk_func.fn_oid = entry_value.func_id;
        if (entry_key.runtime_offset >= 0) {
            current_sk->sk_argument = (Datum)0;
            runtime_keys[entry_key.runtime_offset].scan_key = current_sk;
        } else if (!entry_key.sv) {
            current_sk->sk_flags |= SK_ISNULL;
        } else {
            current_sk->sk_argument = PointerGetDatum(entry_key.sv);
        }
        ++current_sk;
    }
    context.destroy();
    if (!invalidated_offset.empty()) {
        std::sort(invalidated_offset.begin(), invalidated_offset.end(), std::greater<int>{});
        for (int i : invalidated_offset) {
            int nmoved = n_runtime_keys - i - 1;
            if (nmoved > 0) {
                errno_t rc = memmove_s(runtime_keys + i, (nmoved + 1) * sizeof(IndexRuntimeKeyInfo),
                    runtime_keys + i + 1, nmoved * sizeof(IndexRuntimeKeyInfo));
                securec_check(rc, "\0", "\0");
            }
            --n_runtime_keys;
        }
    }
    ann_helper::optional_destroy(invalidated_offset);
    num_orderby = current_sk - sk;
    if (num_orderby <= 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("FULLTEXT index order by got all clauses with zero weights")));
    }
    return sk;
}

ScanKey extract_bm25_orderby_scankey(List *indexorderby, EState *estate, PlanState *ps,
    int &num_orderby, IndexRuntimeKeyInfo *&runtimeKeys, int &numRuntimeKeys)
{
    Assert(list_length(indexorderby) == 1);
    return extract_bm25_orderby_scankey(linitial_node(Expr, indexorderby), estate, ps,
                                        num_orderby, runtimeKeys, numRuntimeKeys);
}
