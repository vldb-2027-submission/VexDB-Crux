/**
 * Copyright ...
 * BM25 index scan.
 */

#ifndef BM25_SCAN_H
#define BM25_SCAN_H

#include <vtl/vector>
#include <vtl/btree>
#include <vtl/roaring_bitmap>
#include <vtl/optional>

#include "c.h"
#include "access/skey.h"
#include "access/attnum.h"
#include "access/bm25/bm25_inverted_list.h"
#include "access/bm25/bm25_parse.h"
#include "access/bm25/bm25_score.h"

struct LimitInfo {
    bool limit_set{false};
    bool with_limit;
    uint64 limit_offset;
    uint64 limit_count;
    void set_limit(bool l, int64 offset, int64 count)
    {
        limit_set = true;
        if (l) {
            with_limit = true;
            limit_offset = uint64(offset);
            limit_count = uint64(count);
        } else {
            with_limit = false;
        }
    }
};

namespace bm25 {
constexpr uint32 max_daat_threshold = 15000u;

struct ReturnResult {
    ItemPointerData tid;
    Oid part_id;
    bool operator==(const ReturnResult &other) const
    {
        return ItemPointerEqualsNoCheck((ItemPointer)&tid, (ItemPointer)&other.tid) &&
            part_id == other.part_id;
    }
};
struct ReturnResultHasher {
    uint32 operator()(const ReturnResult &key) const noexcept
    {
        return impl::DefaultHasher<ItemPointerData>{}(key.tid) + key.part_id;
    }
};

struct QueryContextEntry {
    uint16 cur_offset{0};
    uint16 max_offset{0};
    uint16 category;
    AttrNumber attrno;
    uint32 list_length;
    float wscore;
    float max_wscore;
    InvertedList list;
    QueryContextEntry(float score, float max_wscore, AttrNumber attrno, uint32 list_length,
                      Relation index, BlockNumber start_blkno, BlockNumber insert_blkno,
                      const InvertedListMeta meta)
        : attrno(attrno),
          list_length(list_length),
          wscore(score),
          max_wscore(max_wscore),
          list(index, start_blkno, insert_blkno, meta, false) {}
    QueryContextEntry(QueryContextEntry &&) = default;
    QueryContextEntry &operator=(const QueryContextEntry &) = default;
    QueryContextEntry &operator=(QueryContextEntry &&) = default;
    bool locate_id(uint64 min_id, QueryStats &stats);
    void swap(QueryContextEntry &other);
    void destroy() { list.destroy(); }
    uint64 last_cur_id() const { return list.get_doc_ids()[max_offset - 1u].doc_id; }
};

struct QueryResult {
    RoaringBitMap<> explored{};
    uint64 cur_max_id{0};
    uint64 explored_id{0};
    bool done_explore{false};
    void destroy() { explored.desreoy(); }
};

struct MatchRequirement {
    uint16 cat;
    uint16 minimum_should_match;
};

struct QueryContext : public QueryStats {
    struct QueryRes {
        ItemPointerData tid;
        Oid part_id;
        float bm25_score;
    };

    Vector<Vector<QueryContextEntry>> entries;
    Vector<QueryResult> results;
    /* topologically ordered, with offset 0 being the top query */
    Vector<QueryGroup> group_base;
    Vector<QueryGroup> query_base;
    Vector<MatchRequirement> match_requirements{2ul};
    MultiMap<float, QueryRes, std::greater<float>,
        MapAllocator<HUGE_ALLOCATOR, float, QueryRes>> top_explored;
    static_assert(!ann_helper::constructor_need_ctx<Pair<const float, QueryRes>>,
                  "compiler error on top_explored");
    bool ordered_search_optimized{false};
    bool top_group_at_start{true};
    uint16 nattr;
    GlobalStats *global_stats{NULL};
    PostDocScorer **scorers{NULL};
    void destroy()
    {
        ann_helper::optional_destroy(entries);
        ann_helper::optional_destroy(results);
        ann_helper::optional_destroy(group_base);
        ann_helper::optional_destroy(query_base);
        ann_helper::optional_destroy(match_requirements);
        ann_helper::optional_destroy(top_explored);
        pfree_ext(global_stats);
        if (scorers) {
            for (uint16 i = 0; i < nattr; ++i) {
                if (scorers[i]) {
                    delete scorers[i];
                    scorers[i] = NULL;
                }
            }
        }
        QueryStats::report();
        QueryStats::destroy();
    }
    void reset()
    {
        destroy();
        new (this) QueryContext;
    }
    bool init_process(uint32 topk);
    bool empty() const { return query_base.empty() && group_base.empty(); }
    void process(uint32 topk, void *store);
    /* return 0 if reaching end */
    uint64 next() { return next(get_top_group()); }
    /* called after next */
    float get_score(uint64 doc_id, const DocumentStats *doc_stats)
        { return get_score(doc_id, doc_stats, get_top_group()); }
private:
    bool has_top_group() const { return !group_base.empty(); }
    QueryGroup &get_top_group()
        { return top_group_at_start ? group_base.front() : group_base.back(); }
    void init_process_group(const QueryGroup &group);
    uint64 next(QueryGroup &group);
    float get_score(uint64 doc_id, const DocumentStats *doc_stats, QueryGroup &group);
    void reset_iter(QueryGroup &group);
    void collect_result(QueryGroup &group);
};

constexpr StrategyNumber BM25_RANK_STRATEGY = 1u;
constexpr StrategyNumber BM25_MATCH_STRATEGY = 2u;
struct RowQuery {
    char *query{NULL};
    char **queries{NULL};
    float weight{1.0};
    AttrNumber attrno;
    StrategyNumber sop;

    RowQuery() = default;
    RowQuery(char *q, AttrNumber a, StrategyNumber s) : query(q), attrno(a), sop(s) {}
    RowQuery(char **q, AttrNumber a, StrategyNumber s) : queries(q), attrno(a), sop(s) {}
    void destroy()
    {
        pfree_ext(query);
        if (queries) {
            char **old_queries = queries;
            while (*queries) {
                pfree(*queries);
                ++queries;
            }
            pfree(old_queries);
            queries = NULL;
        }
    }
    bool need_score() const { return sop == BM25_RANK_STRATEGY; }
};
struct RowSparseVec {
    SparseVector *vec;
    float weight;
    AttrNumber attrno;
    RowSparseVec(SparseVector *_vec, float _weight, AttrNumber _attrno, bool need_normalize)
        : vec(COPY_SPARSEVEC(_vec)),
          weight(_weight),
          attrno(_attrno)
    {
        if (need_normalize) {
            sparsevector_normalize(vec);
        }
    }
    void destroy() { pfree(vec); }
};

struct BM25Scanner : public BaseObject {
    constexpr static size_t uninit_offset = size_t(-1);
    size_t cur_offset{uninit_offset};
    Vector<ItemPointerData> tids;
    Vector<float> scores;
    Optional<Vector<Oid>> part_ids;
    Optional<UnorderedSet<ReturnResult, ReturnResultHasher>> returned;
    float bm25_weight{1.0};
    float score;

    Vector<RowQuery> row_queries;
    Vector<RowSparseVec> row_vectors;
    bool require_order_by;

    LimitInfo linfo;
    QueryContext context;

    bool inited() const { return cur_offset != uninit_offset; }
    void set_inited() { cur_offset = 0; }
    void destroy()
    {
        ann_helper::optional_destroy(tids);
        ann_helper::optional_destroy(scores);
        ann_helper::optional_destroy(part_ids);
        ann_helper::optional_destroy(returned);
        ann_helper::optional_destroy(row_queries);
        ann_helper::optional_destroy(row_vectors);
        context.destroy();
    }

    void extract_query_group(QueryGroup **filters, QueryGroup **queries, uint16 &nqueries,
                             const Oid *dict_ids);
    uint32 get_topk() const;
};
}; /* namespace bm25 */

#endif /* BM25_SCAN_H */
