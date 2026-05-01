/**
 * Copyright ...
 */

#ifndef BM25_PARSE_H
#define BM25_PARSE_H

#include <vtl/vector>

#include "c.h"
#include "access/attnum.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_cache.h"
#include "access/bm25/tokenizer/cppjieba/jieba.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "access/annvector/sparsevector.h"

namespace bm25 {

enum class ASTTokenType : uint16 { AND = 0, OR = 1u, LPAREN, RPAREN, KEYWORD, END };
struct ASTToken {
    ASTTokenType type;
    const char *value;
    ASTToken(ASTTokenType t, const char *v = "") : type(t), value(v) {}
};

class ASTNode : public BaseObject {
public:
    const char *keyword;
    ASTTokenType op;
    ASTNode *left;
    ASTNode *right;

    /* 一个关键字节点，应当为叶节点 */
    ASTNode(const char *kw) : keyword(kw), op(ASTTokenType::KEYWORD), left(NULL), right(NULL) {} 
    /* 一个运算符节点，应当为内部节点 */
    ASTNode(ASTTokenType opType, ASTNode *l, ASTNode *r)
        : keyword(NULL), op(opType), left(l), right(r) {}
    void destroy()
    {
        pfree_ext(keyword);
        if (left) {
            left->destroy();
            delete left;
        }
        if (right) {
            right->destroy();
            delete right;
        }
    }
};

class Lexer {
    const char *input;
    size_t pos{0};
public:
    Lexer(const char *str) : input(str) {}
    Vector<ASTToken> tokenize();
};

class Parser {
    Vector<ASTToken> tokens;
    size_t current = 0;

    const ASTToken &consume() { return tokens[current++]; }
    const ASTToken &peek() const { return tokens[current]; }
    bool match(ASTTokenType type) const { return peek().type == type; }

    ASTNode *parse_expression();
    ASTNode *parse_term();
    ASTNode *parse_factor();
public:
    Parser(const Vector<ASTToken> &t) : tokens(t) {}
    ASTNode *parse() { return parse_expression(); }
};

struct QueryToken {
    union value {
        char *tok;
        uint32 dim;

        constexpr value(char *t) : tok(t) {}
        constexpr value(uint32 d) : dim(d) {}
    };
    bool is_text{true};
    uint16 index{uint16(-1)};   /* index to QueryContext entries */
    AttrNumber attrno{InvalidAttrNumber};
    Vector<uint16> frequencies{};
    Vector<value> toks{};

    Vector<char *> &as_toks() { return (Vector<char *> &)toks; }

    QueryToken() = default;
    QueryToken(AttrNumber a, std::initializer_list<uint16> &&f, std::initializer_list<value> &&t)
        : is_text(true),
          attrno(a),
          frequencies(f),
          toks(t) {}
    QueryToken(QueryToken &&other)
        : is_text(other.is_text),
          index(other.index),
          attrno(other.attrno),
          frequencies(std::move(other.frequencies)),
          toks(std::move(other.toks)) {}
    QueryToken(const QueryToken &other)
        : is_text(other.is_text),
          index(other.index),
          attrno(other.attrno),
          frequencies(other.frequencies),
          toks()
    {
        if (is_text) {
            for (const auto &s : other.toks) {
                toks.push_back(pstrdup(s.tok));
            }
        } else {
            for (const auto &s : other.toks) {
                toks.push_back(s.dim);
            }
        }
    }
    void destroy()
    {
        ann_helper::optional_destroy(frequencies);
        if (is_text) {
            for (auto &t : toks) {
                if (t.tok) {
                    pfree(t.tok);
                }
            }
        }
        ann_helper::optional_destroy(toks);
    }
    void swap(QueryToken &other)
    {
        std::swap(is_text, other.is_text);
        std::swap(index, other.index);
        std::swap(attrno, other.attrno);
        frequencies.swap(other.frequencies);
        toks.swap(other.toks);
    }
};

enum class QueryGroupType : uint16 {
    AND = 0,
    OR = 1u,
};

struct QueryGroupParam {
    int minimum_should_match{0}; /* minimum number of tokens to match */
    float boost{1.0f}; /* boost factor for this group */
    Oid dict_id{InvalidOid}; /* dict oid used when search */
    bool extend{true}; /* query if true, mix if false */
    bool norm{false}; /* whether the score is normalized */
};

class QueryGroup : public BaseObject {
public:
    QueryToken query_tokens;
    uint16 flag{0};
    Vector<QueryGroup> child_group{};   /* invalid after preprocessed */

    /* used for query */
    uint16 idx;
    uint16 nchild_group{0};
    uint16 *child_group_idx{NULL};    /* index to QueryContext group_base */
    uint64 no_less_than{0};
    QueryGroupParam param{};

    static constexpr uint16 TypeMask = 0x0001u;
    static constexpr uint16 CoverMask = 0x0002u;
    static constexpr uint16 ScoreMask = 0x8000u;
    static constexpr uint16 ScoreOrderMask = 0x4000u;
    static constexpr uint16 BM25Mask = 0x2000u;

    QueryGroup(const QueryGroup &qg)
        : query_tokens(qg.query_tokens),
          flag(qg.flag),
          child_group(qg.child_group),
          idx(qg.idx),
          nchild_group(qg.nchild_group),
          child_group_idx(qg.child_group_idx),
          no_less_than(qg.no_less_than),
          param(qg.param)
    {
        if (child_group_idx) {
            Assert(nchild_group > 0);
            uint16 *new_idx = (uint16 *)palloc(sizeof(uint16) * nchild_group);
            for (uint16 i = 0; i < nchild_group; ++i) {
                new_idx[i] = child_group_idx[i];
            }
            child_group_idx = new_idx;
        } else {
            Assert(nchild_group == 0);
        }
    }
    QueryGroup(QueryGroup &&qg)
        : query_tokens(std::move(qg.query_tokens)),
          flag(qg.flag),
          child_group(std::move(qg.child_group)),
          idx(qg.idx),
          nchild_group(qg.nchild_group),
          child_group_idx(qg.child_group_idx),
          no_less_than(qg.no_less_than),
          param(qg.param) { qg.child_group_idx = NULL; }
    QueryGroup(QueryToken &&qt) : query_tokens(std::move(qt)) {}
    QueryGroup(QueryToken &&qt, const QueryGroupParam &p) : query_tokens(std::move(qt)), param(p) {}
    QueryGroup(QueryToken &&qt, Vector<QueryGroup> &&child_group, const QueryGroupParam &p)
        : query_tokens(std::move(qt)),
          child_group(std::move(child_group)),
          param(p) {}
    QueryGroup(QueryToken &&qt, std::initializer_list<QueryGroup *> &&list)
        : query_tokens(std::move(qt)),
          child_group(list, [](QueryGroup *qg) -> QueryGroup && { return std::move(*qg); }) {}
    void destroy()
    {
        query_tokens.destroy();
        ann_helper::optional_destroy(child_group);
        pfree_ext(child_group_idx);
    }
    void swap(QueryGroup &other)
    {
        if (this == &other) {
            return;
        }
        query_tokens.swap(other.query_tokens);
        std::swap(flag, other.flag);
        child_group.swap(other.child_group);

        std::swap(idx, other.idx);
        std::swap(nchild_group, other.nchild_group);
        std::swap(child_group_idx, other.child_group_idx);
        std::swap(no_less_than, other.no_less_than);
        std::swap(param, other.param);
    }    

    QueryGroupType get_type() const { return QueryGroupType(flag & TypeMask); }
    void set_type(QueryGroupType type) { flag = (flag & ~TypeMask) | (uint16(type) & TypeMask); }
    bool get_full_cover() const { return flag & CoverMask; }
    void set_full_cover(bool full_cover)
    {
        if (full_cover) {
            flag |= CoverMask;
        } else {
            flag &= ~CoverMask;
        }
    }
    bool get_need_score() const { return flag & ScoreMask; }
    bool get_need_score_order() const { return flag & ScoreOrderMask; }
    bool get_is_bm25() const { return flag & BM25Mask; }
    void set_score(bool need_score, bool need_score_order)
    {
        if (!need_score && need_score_order) {
            ereport(ERROR,
                (errcode(ERRCODE_OPERATE_NOT_SUPPORTED),
                errmsg("Function bm25_score can only be evaluated with "
                        "the @~@ operator under bm25 Text Match index scan.")));
        }
        
        if (need_score) {
            flag |= ScoreMask;
        } else {
            flag &= ~ScoreMask;
        }
        if (need_score_order) {
            flag |= ScoreOrderMask;
        } else {
            flag &= ~ScoreOrderMask;
        }
    }
    void set_bm25(bool bm25)
    {
        if (bm25) {
            flag |= BM25Mask;
        } else {
            flag &= ~BM25Mask;
        }
    }
    bool is_optional() const
    {
        return get_type() == QueryGroupType::OR && get_need_score() &&
            param.minimum_should_match == 0;
    }

    static QueryGroup *compress_from_ast(ASTNode *root, AttrNumber attrno);
    static QueryGroup *parse_query_group(const char *query, AttrNumber attrno);
    void dfs_print(int indent, StringInfo buffer) const;
    void print(const char *header = NULL) const;
};
}; /* namespace bm25 */

#endif /* BM25_PARSE_H */
