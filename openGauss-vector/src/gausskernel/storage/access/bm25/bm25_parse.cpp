/**
 * Copyright ...
 */

#include "access/bm25/bm25_parse.h"
#include "access/bm25/bm25.h"
#include "access/bm25/tokenizer/token.h"
#include "tsearch/ts_locale.h"

using namespace bm25;
using namespace bm25_tokenizer;

#define ESCAPE_CHAR 92 /* ASCII */
#define AND_LEN 3
#define OR_LEN 2
#define PAREN_LEN 1

inline int is_special_token(const char *input, const size_t &pos, size_t &len) {
    /*   ***AND***  */
    /*      ↑       */
    /*     pos      */
    if (pos + AND_LEN <= len && strncmp(input + pos , "AND", AND_LEN) == 0) {
        return 0;
    }
    if (pos + OR_LEN <= len && strncmp(input + pos , "OR", OR_LEN) == 0) {
        return 1;
    }
    if (pos + PAREN_LEN <= len && strncmp(input + pos , "(", PAREN_LEN) == 0) {
        return 2;
    }
    if (pos + PAREN_LEN <= len && strncmp(input + pos , ")", PAREN_LEN) == 0) {
        return 3;
    }
    return -1;
}

void insert_keyword(const char *input, size_t start, size_t end, Vector<ASTToken> &tokens)
{
    size_t len = strlen(input);
    while (start < len && isspace(input[start])) {
        ++start;
    }
    while (end > 0 && isspace(input[end])) {
        --end;
    }
    if (end >= start) {
        const size_t word_len = end - start + 1;
        char *word = (char *)palloc(sizeof(char) * (word_len + 1));
        size_t filteredIndex = 0;
        for (size_t i = start; i <= end; ++i) {
            if (input[i] != ESCAPE_CHAR) { 
                word[filteredIndex++] = input[i];
            } else if (is_special_token(input, i + 1, len) == -1){
                word[filteredIndex++] = input[i];
            }
        }
        word[filteredIndex] = '\0';
        tokens.emplace_back(ASTTokenType::KEYWORD, word);
    }
}
    

Vector<ASTToken> Lexer::tokenize()
{
    Vector<ASTToken> tokens;
    size_t pos = 1;
    size_t len = strlen(input);
    size_t start = 0;
    size_t end = 0;
    int status = is_special_token(input, 0, len);
    if (status == 2) {
        tokens.emplace_back(ASTTokenType::LPAREN, "(");
        start = 1;
    } else if (status != -1) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("illegal syntax query string")));
    }
    while (pos < len) {
        int status = is_special_token(input, pos, len);
        if (status != -1 && input[pos - 1] != ESCAPE_CHAR) {
            end = pos - 1;
            insert_keyword(input, start, end, tokens);
            switch (status) {
                case 0:
                    tokens.emplace_back(ASTTokenType::AND, "AND");
                    pos += AND_LEN;
                    break;
                case 1:
                    tokens.emplace_back(ASTTokenType::OR, "OR");
                    pos += OR_LEN;
                    break;
                case 2:
                    tokens.emplace_back(ASTTokenType::LPAREN, "(");
                    pos += PAREN_LEN;
                    break;
                case 3:
                    tokens.emplace_back(ASTTokenType::RPAREN, ")");
                    pos += PAREN_LEN;
                    break;
            }
            start = pos;
        } else {
            ++pos;
        }
    }
    end = pos - 1;
    insert_keyword(input, start, end, tokens);
    tokens.emplace_back(ASTTokenType::END);
    return tokens;
}


ASTNode *Parser::parse_expression() {
    ASTNode *left = parse_term();
    while (match(ASTTokenType::OR)) {
        consume();
        ASTNode *right = parse_term();
        ASTNode *new_node = NEW ASTNode(ASTTokenType::OR, left, right);
        left = new_node; 
    }
    return left;
}

ASTNode *Parser::parse_term() {
    ASTNode *left = parse_factor();
    while (match(ASTTokenType::AND)) {
        consume(); 
        ASTNode *right = parse_factor();
        ASTNode *new_node = NEW ASTNode(ASTTokenType::AND, left, right);
        left = new_node;
    }
    return left;
}

ASTNode *Parser::parse_factor() {
    if (match(ASTTokenType::LPAREN)) {
        consume();
        ASTNode *node = parse_expression();
        if (!match(ASTTokenType::RPAREN)) {
            node->destroy();
            delete node;
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("illegal syntax query string")));
        }
        consume();
        return node;
    }

    if (match(ASTTokenType::KEYWORD)) {
        ASTNode *node = NEW ASTNode(pstrdup(peek().value));
        consume();
        return node;
    }

    ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("illegal syntax query string")));
    return NULL; /* keep compiler quiet */
}


QueryGroup *QueryGroup::compress_from_ast(ASTNode *root, AttrNumber attrno)
{
    if (root->op == ASTTokenType::KEYWORD) {
        Assert(!root->left && !root->right);
        auto *res = NEW QueryGroup(QueryToken(attrno, {1u}, {pstrdup(root->keyword)}), {});
        res->set_type(QueryGroupType::AND);
        return res;
    }

    ASTNode *lc = root->left;
    ASTNode *rc = root->right;
    Assert(lc && rc);   /* 不应当抵达叶节点，因为所有运算符都为内部节点，需在内部处理完毕 */

    QueryGroupType root_op_type = static_cast<QueryGroupType>(root->op);
    /* 两个子节点都是关键字节点 */
    if (lc->op == ASTTokenType::KEYWORD && rc->op == ASTTokenType::KEYWORD) {
        auto *res = NEW QueryGroup(
            cmp_token(lc->keyword, rc->keyword) != 0 ?
                QueryToken(attrno, {1u, 1u}, {pstrdup(lc->keyword), pstrdup(rc->keyword)}) :
                QueryToken(attrno, {2u}, {pstrdup(lc->keyword)}),
            {});
        res->set_type(root_op_type);
        return res;
    }
    /* 一个子节点为关键字节点，另一个为运算符节点 */
    if ((lc->op == ASTTokenType::KEYWORD) ^ (rc->op == ASTTokenType::KEYWORD)) {
        ASTNode *keyword_c = lc->op == ASTTokenType::KEYWORD ? lc:rc;
        ASTNode *op_c = keyword_c == lc ? rc : lc;

        QueryGroup *op_qg = compress_from_ast(op_c, attrno);
        
        /* 父子运算符相同，可合并 */
        if (root_op_type == op_qg->get_type()) {
            Assert(op_qg->query_tokens.attrno == attrno);
            for (const auto &tok : op_qg->query_tokens.toks) {
                if (cmp_token(keyword_c->keyword, tok.tok) == 0) {
                    /* no need to set up frequencies, it won't be used */
                    return op_qg;
                }
            }
            op_qg->query_tokens.toks.emplace_back(pstrdup(keyword_c->keyword));
            op_qg->query_tokens.frequencies.emplace_back(1u);
            return op_qg;
        }
        /* 父子运算符不同，不合并 */
        QueryToken root_qt(attrno, {1u}, {pstrdup(keyword_c->keyword)});
        auto *res = NEW QueryGroup(std::move(root_qt), {op_qg});
        res->set_type(root_op_type);
        delete op_qg;
        return res;
    }
    /* 两个子节点都是运算符节点 */
    QueryGroup *lc_qg = compress_from_ast(lc, attrno);
    QueryGroup *rc_qg = compress_from_ast(rc, attrno);

    /* 两个子节点运算符相同 */
    if (lc_qg->get_type() == rc_qg->get_type()) {
        /* 父节点运算符也相同,三合一 */
        if (root_op_type == lc_qg->get_type()) {
            /* 填充QueryToken*/
            QueryToken root_qry_tok;
            root_qry_tok.attrno = attrno;
            root_qry_tok.toks.push_back(lc_qg->query_tokens.toks.cbegin(),
                                        lc_qg->query_tokens.toks.cend());
            root_qry_tok.toks.push_back(rc_qg->query_tokens.toks.cbegin(),
                                        rc_qg->query_tokens.toks.cend());
            ann_helper::optional_destroy(lc_qg->query_tokens.toks);
            ann_helper::optional_destroy(rc_qg->query_tokens.toks);
            QueryGroup *root_qg = NEW QueryGroup(std::move(root_qry_tok));

            /* 填充其他字段 */
            root_qg->set_type(root_op_type);
            for (auto &qg : lc_qg->child_group) {
                root_qg->child_group.emplace_back(std::move(qg));
            }
            for (auto &qg : rc_qg->child_group) {
                root_qg->child_group.emplace_back(std::move(qg));
            }
            lc_qg->destroy();
            delete lc_qg;
            rc_qg->destroy();
            delete rc_qg;
            return root_qg;
        }
        /* 父节点运算符不同 */
        auto *res = NEW QueryGroup(QueryToken(), {lc_qg, rc_qg});
        res->set_type(root_op_type);
        delete lc_qg;
        delete rc_qg;
        return res;
    }

    /* 两个子节点运算符不同 */
    QueryGroup *merge_qg = root_op_type == lc_qg->get_type() ? lc_qg : rc_qg;
    QueryGroup *not_merge_qg = merge_qg == lc_qg ? rc_qg : lc_qg;
    merge_qg->child_group.emplace_back(*not_merge_qg);
    /* 还有其他的没有填充 */
    return merge_qg;
}

QueryGroup *QueryGroup::parse_query_group(const char *query, AttrNumber attrno)
{
    Lexer lexer(query);
    Vector<ASTToken> tokens = lexer.tokenize();

    Parser parser(tokens);
    ASTNode *root = parser.parse();
    QueryGroup *res = compress_from_ast(root, attrno);
    root->destroy();
    delete root;
    return res;
}

void QueryGroup::dfs_print(int indent, StringInfo buffer) const
{
    appendStringInfoSpaces(buffer, indent);
    appendStringInfo(buffer, "GROUP: %s (%s score, %s order, boost %f, min_match %d)\n",
        get_type() == QueryGroupType::AND ? "AND" : "OR",
        get_need_score() ? "has" : "no",
        get_need_score_order() ? "has": "no",
        param.boost,
        param.minimum_should_match);

    indent += 4;
    if (!query_tokens.toks.empty()) {
        appendStringInfoSpaces(buffer, indent);
        for (const auto &tok : query_tokens.toks) {
            appendStringInfoChar(buffer, '\"');
            if (query_tokens.is_text) {
                appendStringInfoString(buffer, tok.tok);
            } else {
                appendStringInfo(buffer, "%u", tok.dim);
            }
            appendStringInfoString(buffer, "\", ");
        }
        appendStringInfoChar(buffer, '\n');
    }

    for (const auto &it : child_group) {
        it.dfs_print(indent, buffer);
    }
}

void QueryGroup::print(const char *header) const
{
#if OUTPUT_BM25_LOG
    StringInfoData buffer;
    initStringInfo(&buffer);
    if (header) {
        appendStringInfoString(&buffer, header);
        appendStringInfoChar(&buffer, '\n');
    }
    dfs_print(0, &buffer);
    ereport(NOTICE, (errmsg("%s", buffer.data)));
    FreeStringInfo(&buffer);
#endif /* OUTPUT_BM25_LOG */
}
