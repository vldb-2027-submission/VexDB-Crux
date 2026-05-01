/**
 * Copyright ...
 * BM25 index internal definitions.
 */

#ifndef BM25_INTERNAL_H
#define BM25_INTERNAL_H

#include "access/bm25/bm25_inverted_list.h"
#include "access/bm25/bm25_token_index.h"
#include "access/bm25/bm25_doc_store.h"
#include "access/bm25/bm25_score.h"
#include "access/bm25/bm25_parse.h"
#include "access/bm25/bm25_scan.h"
#include "access/bm25/index_inspect.h"

#if OUTPUT_BM25_LOG
#define BM25LOG(fmt, ...) ereport(NOTICE, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define BM25LOG(fmt, ...)
#endif /* OUTPUT_BM25_LOG */

namespace bm25 {
struct BM25Options {
    int32 vl_len_;              /* varlena header (do not touch directly!) */
    int32 parallel_workers;
    int dicts_offset;
    int algorithms_offset;
    int coefficients_offset;
};

struct BM25MetaPageData {
    uint32 version;
    uint32 magic;
    uint32 nattr;
    TokenIndexMeta tok_idx_meta;
    DocumentStoreMeta doc_store_meta;
    InvertedListMetaData il_meta;
    Oid dict_ids[BM25_MAX_NATTR];
    Scorer scorer[BM25_MAX_NATTR];
    uint32 is_sparse_vector;
    static_assert(sizeof(is_sparse_vector) * CHAR_BIT >= BM25_MAX_NATTR);

    void init(Relation index);
};
using BM25MetaPage = BM25MetaPageData *;
#define GetBM25MetaPage(page) ((BM25MetaPage)((char *)(page) + SizeOfPageHeaderData))
static_assert(sizeof(BM25MetaPageData) <= BLCKSZ - SizeOfPageHeaderData);

class BM25Store : public ann_helper::LeakChecker {
public:
    BM25Store(Relation index, bool need_wal);
    void destroy();
    void insert(Documents &doc);
    void vacuum(IndexBulkDeleteCallback callback, void *callback_state,
                IndexBulkDeleteResult *stats);
    void inspect(IndexInspectResult &res);
    bool get_doc_info(uint64 doc_id, DocumentStats *stats, ItemPointer tid, Oid &part_id);
    /* parsed_top_query will be released after preprocess */
    void preprocess_query_context(QueryContext &query_context, QueryGroup *parsed_top_filter,
        QueryGroup *parsed_queries, uint16 nquery);
    Oid get_dict_id(AttrNumber attr) const { return _meta->dict_ids[attr - 1]; }
    const Oid *get_dict_id() const { return _meta->dict_ids; }
    uint16 get_nattr() const { return _meta->nattr; }
    bool is_sparse_vector(AttrNumber attrno) const
    {
        Assert(AttributeNumberIsValid(attrno));
        Assert(attrno <= BM25_MAX_NATTR);
        return _sparse_vec_bits & 1 << (attrno - 1);
    }
private:
    Buffer _meta_buf;
    BM25MetaPage _meta;
    TokenIndex _tok_index;
    DocumentStore _doc_store;
    uint32 _sparse_vec_bits;

    Vector<QueryContextEntry> generate_context(const QueryToken &query, GlobalStats *global_stats,
                                               bool need_score, const QueryGroupParam &param);
    uint64 insert_global_stats(Relation index, Documents &docs, const DocumentStats *stats,
                               bool need_wal);
};
}; /* namespace bm25 */

#endif /* BM25_INTERNAL_H */
