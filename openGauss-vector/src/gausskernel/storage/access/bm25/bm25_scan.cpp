/**
 * Copyright ...
 */

#include <algorithm>    /* upper_bound, min */
#include <numeric>      /* accumulate */
#include <vtl/tuple>
#include <vtl/expr_helper>

#include "access/bm25/bm25_scan.h"
#include "access/bm25/bm25_internal.h"
#include "access/bm25/bm25_parse.h"
#include "access/bm25/tokenizer/tokenizer.h"
#include "access/bm25/tokenizer/cppjieba/jieba.h"
#include "tsearch/ts_locale.h"

using namespace bm25;
using namespace bm25_tokenizer;
using namespace ann_helper;

static void set_done_explore(QueryContext &context, uint16 idx)
{
    context.results[idx].done_explore = true;
    BM25LOG("Group %hu done explore", idx);
}

void QueryContextEntry::swap(QueryContextEntry &other)
{
    if (this == &other) {
        return;
    }
    std::swap(cur_offset, other.cur_offset);
    std::swap(max_offset, other.max_offset);
    std::swap(category, other.category);
    std::swap(attrno, other.attrno);
    std::swap(list_length, other.list_length);
    std::swap(wscore, other.wscore);
    std::swap(max_wscore, other.max_wscore);
    list.swap(other.list);
}

bool QueryContextEntry::locate_id(uint64 min_id, QueryStats &stats)
{
    for (;;) {
        if (cur_offset >= max_offset) {
            bool looped = false;
            while (list.has_nextl_info() && list.nextl_doc_id() <= min_id) {
                list.iter_nextl();
                stats.record_lskip();
                looped = true;
            }
            if (!looped) {
                if (!list.iter_next()) {
                    return false;
                }
                stats.record_qskip();
            }
            cur_offset = 0;
        }
        const auto *list_entries = list.get_doc_ids(max_offset);
        cur_offset = std::upper_bound(list_entries + cur_offset, list_entries + max_offset, min_id,
            [](uint64 a, const InvertedListEntry &b) {
                return a < b.doc_id;
            }) - list_entries;
        stats.record_query();
        if (cur_offset < max_offset) {
            break;
        }
    }
    return true;
}

static double estimate_taat_cost(QueryGroup &group) { return 1.0; }

static double estimate_daat_cost(Vector<QueryGroup> &queries) { return 0.0; }

bool QueryContext::init_process(uint32 topk)
{
    if (has_top_group()) {
        init_process_group(get_top_group());
        if (query_base.empty()) {
            return true;
        }
    }

    for (auto &g : query_base) {
        init_process_group(g);
        if (g.param.minimum_should_match <= 0) {
            continue; /* empty query */
        }
        if (g.query_tokens.index == uint16(-1) ||
            entries[g.query_tokens.index].size() < (uint32)g.param.minimum_should_match) {
            return false;
        }
        const uint16 cat = query_base.offset(g) + 1ul;
        match_requirements.push_back({cat, (uint16)g.param.minimum_should_match});
        auto &entry_list = entries[g.query_tokens.index];
        for (auto &e : entry_list) {
            e.category = cat;
        }
    }

    if (topk > max_daat_threshold || query_base.empty() ||
        !query_base.front().get_need_score_order() ||
        (has_top_group() && estimate_taat_cost(get_top_group()) <= estimate_daat_cost(query_base))) {
        uint16 idx = group_base.size();
        if (!has_top_group()) {
            BM25LOG("Try TAAT algorithm, rebind queries");
            group_base.swap(query_base);
            uint16 idx = group_base.size();
            QueryGroup temp{QueryToken()};
            temp.set_full_cover(true);
            temp.set_score(false, false);
            temp.set_type(QueryGroupType::AND);
            temp.idx = temp.nchild_group = idx;
            temp.child_group_idx = (uint16 *)palloc(sizeof(uint16) * idx);
            for (uint16 i = 0; i < idx; ++i) {
                group_base[i].idx = i;
                temp.child_group_idx[i] = i;
            }
            group_base.push_back(std::move(temp));
            top_group_at_start = false;
        } else if (get_top_group().get_type() == QueryGroupType::AND) {
            BM25LOG("Try TAAT algorithm, combine AND filter and queries");
            auto &top = get_top_group();
            top.child_group_idx = (uint16 *)(top.child_group_idx ?
                repalloc(top.child_group_idx, sizeof(uint16) * (top.nchild_group + query_base.size())) :
                palloc(sizeof(uint16) * (top.nchild_group + query_base.size())));
            for (auto &q : query_base) {
                q.idx = top.child_group_idx[top.nchild_group] = idx;
                group_base.push_back(std::move(q));
                ++top.nchild_group;
                ++idx;
            }
            query_base.clear_no_destroy();
        } else {
            BM25LOG("Try TAAT algorithm, combine OR filter and queries");
            QueryToken tok;
            tok.index = uint16(-1);
            QueryGroup top(std::move(tok));
            top.child_group_idx = (uint16 *)palloc(sizeof(uint16) * (query_base.size() + 1ul));
            top.set_type(QueryGroupType::AND);
            Assert(top_group_at_start);
            top.child_group_idx[top.nchild_group] = get_top_group().idx;
            ++top.nchild_group;
            for (auto &q : query_base) {
                q.idx = top.child_group_idx[top.nchild_group] = idx;
                group_base.push_back(std::move(q));
                ++top.nchild_group;
                ++idx;
            }
            query_base.clear_no_destroy();
            top.idx = group_base.size();
            group_base.push_back(std::move(top));
            top_group_at_start = false;
        }
        results.resize(group_base.size());
        return true;
    }

    if (!query_base.empty()) {
        bool should_keep = false;
        /* TD: judge whether we can use reverse scan order */
        if (should_keep) { /* if so, swap it back */
            return true;
        }
    }

    /* gather all entries into one */
    Vector<QueryContextEntry> res;
    size_t start_query_idx = entries.size();
    for (auto &qg : query_base) {
        if (qg.query_tokens.index < start_query_idx) {
            start_query_idx = qg.query_tokens.index;
        }
    }
    for (auto it = entries.at(start_query_idx); it != entries.end(); ++it) {
        auto &v = *it;
        for (auto &e : v) {
            res.push_back(std::move(e));
        }
        v.clear_no_destroy();
    }
    if (res.empty()) {
        return true;
    }

    std::sort(res.begin(), res.end(),
              [](const QueryContextEntry &a, const QueryContextEntry &b) {
        return a.wscore + a.max_wscore > b.wscore + b.max_wscore;
    });
    entries.push_back(std::move(res));
    entries.shrink_to_fit();
    for (auto &v : entries) {
        v.shrink_to_fit();
    }
    ordered_search_optimized = true;
    return true;
}

struct ScoreSet {
    ScoreSet(uint16 freq, uint16 cat, AttrNumber attr, float wscore)
        : freq(freq), cat(cat), attr(attr), wscore(wscore) {}
    uint16 freq;
    uint16 cat;
    AttrNumber attr;
    float wscore;
};
struct ScoreRes {
    float est_score{0.0};
    Vector<ScoreSet> score_set;
};
using score_res_map = UnorderedMap<uint64, ScoreRes>;

/* mixed DAAT and TAAT */
template <class id_F, class range_F>
static void perform_search(QueryContext &context, uint32 topk, BM25Store &store, id_F &&id_f,
                           range_F &&range_f)
{
    static_assert(IS_INVOCABLE(range_F, score_res_map &, uint64, uint64), "incorrect range_f type");
    static_assert(IS_INVOCABLE_R(id_F, bool, uint64, uint64), "incorrect id_f type");
    Assert(!context.entries.empty());
    Assert(topk >= 2u);
    auto &target_lists = context.entries.back();
    auto &top_explored = context.top_explored;
    uint32 explored_size = 0;
    float min_top_explored_score = 0.0f;
    constexpr size_t score_res_map_init_capacity = 500;
    score_res_map temp_res_score(score_res_map_init_capacity);

    float cur_max_score;
    float remain_possible_max_score;
    const auto insert_record = [&](const InvertedListEntry &ie, const QueryContextEntry &qe,
        float est_score) -> void {
        score_res_map::iterator rs_it;
        bool new_inserted = false;
        if (remain_possible_max_score + est_score > min_top_explored_score) {
            tie(rs_it, new_inserted) = temp_res_score.try_emplace(ie.doc_id);
        } else {
            rs_it = temp_res_score.find(ie.doc_id);
            if (rs_it == temp_res_score.end()) {
                context.record_skip();
                return;
            }
        }
        rs_it->second.est_score += est_score;
        if (rs_it->second.est_score + remain_possible_max_score <= min_top_explored_score) {
            BM25LOG("Remove doc_id %lu with max possible score %f",
                    ie.doc_id, rs_it->second.est_score + remain_possible_max_score);
            temp_res_score.erase(rs_it);
            context.record_skip();
            return;
        }
        if (rs_it->second.est_score > cur_max_score) {
            cur_max_score = rs_it->second.est_score;
        }
        BM25LOG("Entry(%f) insert doc_id %lu with score %f to %f",
                qe.wscore, ie.doc_id, est_score, rs_it->second.est_score);
        rs_it->second.score_set.emplace_back(ie.freq, qe.category, qe.attrno, qe.wscore);
        context.record_query();
    };

    const auto insert_score = [&](const score_res_map::data_type &p) -> void {
        uint64 id = p.first;
        const Vector<ScoreSet> &score_set = p.second.score_set;
        if (explored_size >= topk && min_top_explored_score >= p.second.est_score) {
            return;
        }
        DocumentStats stats[store.get_nattr()];
        ItemPointerData tid;
        context.record_doc();
        Oid part_id;
        if (!store.get_doc_info(id, stats, &tid, part_id)) {
            return;
        }
        
        bool has_bm25 = false;
        for (const auto &s : score_set) {
            if (!store.is_sparse_vector(s.attr)) {
                has_bm25 = true;
            }
        }
        float score = 0;
        float bm25_score = 0;
        if (has_bm25) {
            for (const auto &s : score_set) {
                const float temp =
                    s.wscore * context.scorers[s.attr - 1]->doc_score(stats[s.attr - 1], s.freq);
                score += temp;
                if (!store.is_sparse_vector(s.attr)) {
                    bm25_score += temp;
                }
            }
        } else {
            score = p.second.est_score;
        }
        if (explored_size >= topk) {
            if (min_top_explored_score >= score) {
                return;
            }
            Assert(top_explored.size() == explored_size);
            auto it = --top_explored.cend();
            Assert(it != top_explored.cbegin());
            --it;   /* topk is at least 2 so we don't check it is end */
            min_top_explored_score = std::min(it->first, score);
            ++it;
            top_explored.erase(it);
            BM25LOG("Min score update to %f", min_top_explored_score);
        } else {
            ++explored_size;
            if (explored_size == topk) {
                min_top_explored_score = std::min(top_explored.crbegin()->first, score);
                BM25LOG("Min score update to %f", min_top_explored_score);
            }
        }
        BM25LOG("Score %f inserted (id = %lu)", score, id);
        top_explored.emplace(score, QueryContext::QueryRes{tid, part_id, bm25_score});
    };

    const auto next_list = [&context](QueryContextEntry &entry) -> bool {
        do {
            if (!entry.list.iter_next()) {
                return false;
            }
            context.record_load();
            entry.list.get_doc_ids(entry.max_offset);
        } while (entry.max_offset == 0);
        entry.cur_offset = 0;
        return true;
    };

    float max_possible_score = 0;
    for (const auto &tl : target_lists) {
        max_possible_score += tl.max_wscore;
    }
    size_t list_size = target_lists.size();
    uint64 max_explored_id = 0ul; /* set as first min_explored_id - 1 */
    uint64 min_explored_id;
    while (list_size > 0) {
        CHECK_FOR_INTERRUPTS();

        cur_max_score = 0.0;
        QueryContextEntry &head_entry = target_lists.front();
        remain_possible_max_score = max_possible_score - head_entry.max_wscore;
restart:
        if (head_entry.max_offset <= head_entry.cur_offset && !next_list(head_entry)) {
            BM25LOG("Head entry(%f) has done exploration", head_entry.wscore);
            max_possible_score -= head_entry.max_wscore;
            target_lists.erase(target_lists.begin());
            --list_size;
            continue;
        }
        uint64 temp_max_explored_id = head_entry.list.next_doc_id();
        temp_max_explored_id = temp_max_explored_id == 0 ?
            head_entry.last_cur_id() :
            temp_max_explored_id - 1ul;
        if (max_explored_id >= temp_max_explored_id) {
            head_entry.cur_offset = head_entry.max_offset;
            goto restart;
        }
        min_explored_id = max_explored_id + 1ul;
        constexpr uint64 init_max_scan_range = 30000ul;
        constexpr uint64 next_max_scan_range = 120000ul;
        const uint64 max_scan_range =
            max_explored_id == 0 ? init_max_scan_range : next_max_scan_range;
        max_explored_id = std::min(max_explored_id + max_scan_range, temp_max_explored_id);
        if (!id_f(min_explored_id, max_explored_id)) {
            BM25LOG("Skip doc_id from %lu to %lu", min_explored_id, max_explored_id);
            continue;
        }

        BM25LOG("Explore head entry(%f) with min_top_score %f, remain_max %f",
                 head_entry.wscore, min_top_explored_score, remain_possible_max_score);
        const GlobalStats &head_gstat = context.global_stats[head_entry.attrno - 1];
        const DocumentStats best_head_doc_stat =
            {float(head_gstat.total_length) / head_gstat.total_doc};
        const auto &head_scorer = context.scorers[head_entry.attrno - 1];
        size_t i = 1ul;
        for (bool head_reach_end = false;;) {
            const InvertedListEntry *head_entries = head_entry.list.get_doc_ids();
            for (; head_entry.cur_offset < head_entry.max_offset; ++head_entry.cur_offset) {
                const auto &itered_entry = head_entries[head_entry.cur_offset];
                if (itered_entry.doc_id > max_explored_id) {
                    head_reach_end = true;
                    break;
                }
                if (!id_f(itered_entry.doc_id, 0) || itered_entry.doc_id < min_explored_id) {
                    continue;
                }
                float rs = head_entry.wscore *
                    head_scorer->doc_score(best_head_doc_stat, itered_entry.freq);
                insert_record(itered_entry, head_entry, rs);
            }
            if (head_reach_end) {
                break;
            }
            uint64 last_id = head_entry.last_cur_id();
            if (!next_list(head_entry)) {
                BM25LOG("Head entry(%f) has done exploration", head_entry.wscore);
                max_possible_score -= head_entry.max_wscore;
                target_lists.erase(target_lists.begin());
                --list_size;
                Assert(min_explored_id <= last_id);
                max_explored_id = last_id;
                i = 0;
                break;
            }
        }
        if (list_size > 0) {
            BM25LOG("Exploring doc_id from %lu to %lu", min_explored_id, max_explored_id);
        }

        for (; i < list_size; ++i) {
            CHECK_FOR_INTERRUPTS();
            QueryContextEntry &entry = target_lists[i];
            remain_possible_max_score -= entry.max_wscore;
            if (remain_possible_max_score < 0) {
                remain_possible_max_score = 0;
            }
            InvertedList &il = entry.list;
            const auto erase_il = [&]() {
                BM25LOG("Entry(%f) has done exploration", entry.wscore);
                max_possible_score -= entry.max_wscore;
                target_lists.erase(target_lists.of(entry));
                --i;
                --list_size;
            };
            if (entry.max_offset <= entry.cur_offset && !next_list(entry)) {
                erase_il();
                continue;
            }

            const GlobalStats &gstat = context.global_stats[entry.attrno - 1];
            const PostDocScorer *scorer = context.scorers[entry.attrno - 1];
            const DocumentStats best_doc_stat = DocumentStats{
                store.is_sparse_vector(entry.attrno) ?
                0 : float(gstat.total_length) / gstat.total_doc};
            BM25LOG("Explore entry(%f) with score base %f, min_top_score %f, remain_max %f",
                    entry.wscore, cur_max_score, min_top_explored_score, remain_possible_max_score);

            bool reach_end = false;
            const float ds_threshold =
                (min_top_explored_score - (cur_max_score + remain_possible_max_score)) /
                entry.wscore;
            while (max_explored_id > entry.last_cur_id()) {
                float max_ds = scorer->doc_score(best_doc_stat, il.next_max_freq());
                while (il.has_next() && max_explored_id > entry.last_cur_id() &&
                       max_ds <= ds_threshold) {
                    if (il.has_nextl_info() && il.nextl_doc_id() <= max_explored_id &&
                        scorer->doc_score(best_doc_stat, il.nextl_max_freq()) <= ds_threshold) {
                        il.iter_nextl();
                        context.record_lskip();
                        BM25LOG("Entry(%f) did a 5-block jump with ds %f and threshold %f",
                                entry.wscore, scorer->doc_score(best_doc_stat, il.nextl_max_freq()),
                                ds_threshold);
                        il.get_doc_ids(entry.max_offset);
                        if (entry.max_offset == 0 && !next_list(entry)) {
                            reach_end = true;
                            break;
                        }
                        entry.cur_offset = 0;
                    } else {
                        context.record_qskip();
                        if (!next_list(entry)) {
                            reach_end = true;
                            break;
                        }
                        BM25LOG("Entry(%f) did a 1-block jump with ds %f and threshold %f",
                                entry.wscore, max_ds, ds_threshold);
                    }
                    max_ds = scorer->doc_score(best_doc_stat, il.next_max_freq());
                }
                if (reach_end || max_explored_id <= entry.last_cur_id()) {
                    break;
                }
                BM25LOG("Explore entry(%f) with ds %f and threshold %f",
                        entry.wscore, max_ds, ds_threshold);
                const auto *entries = entry.list.get_doc_ids();
                for (; entry.cur_offset < entry.max_offset; ++entry.cur_offset) {
                    const auto &e = entries[entry.cur_offset];
                    if (!id_f(e.doc_id, 0) || e.doc_id < min_explored_id) {
                        continue;
                    }
                    const float rs = entry.wscore * scorer->doc_score(best_doc_stat, e.freq);
                    insert_record(e, entry, rs);
                }
                if (!next_list(entry)) {
                    reach_end = true;
                    break;
                }
            }
            if (reach_end) {
                erase_il();
                continue;
            }

            BM25LOG("Explore entry(%f)", entry.wscore);
            const auto *entries = entry.list.get_doc_ids();
            for (;; ++entry.cur_offset) {
                Assert(entry.cur_offset < entry.max_offset);
                const auto &e = entries[entry.cur_offset];
                if (e.doc_id > max_explored_id) {
                    break;
                }
                if (!id_f(e.doc_id, 0) || e.doc_id < min_explored_id) {
                    if (e.doc_id == max_explored_id) {
                        break;
                    }
                    continue;
                }
                const float rs = entry.wscore * scorer->doc_score(best_doc_stat, e.freq);
                insert_record(e, entry, rs);
                if (e.doc_id == max_explored_id) {
                    break;
                }
            }
        }

        BM25LOG("temp_res_score has size %lu and capacity %lu",
                temp_res_score.size(), temp_res_score.capacity());
        auto [it, end_it] = range_f(temp_res_score, min_explored_id, max_explored_id);
        CONSTEXPR_IF (std::is_same<decltype(it), score_res_map::const_iterator>::value) {
            temp_res_score.ctraverse(it, end_it, [&insert_score](const auto &p) {
                insert_score(p);
                optional_destroy(p);
            });
            temp_res_score.clear_no_destroy();
        } else {
            for (; it != end_it; ++it) {
                insert_score(*it);
            }
            temp_res_score.clear();
        }
    }
    optional_destroy(temp_res_score);
}

void QueryContext::process(uint32 topk, void *in_store)
{
    const auto default_id_f = [](uint64 from_or_id, uint64 to) _GLIBCXX17_CONSTEXPR -> bool {
        return true;
    };
    const auto default_range_f = [](score_res_map &map, uint64, uint64) {
        struct {
            typename score_res_map::const_iterator start;
            typename score_res_map::const_iterator end;
        } ret = {map.cbegin(), map.cend()};
        return ret;
    };
    const auto check_match = [this](score_res_map &map) {
        bool erased;
        for (auto it = map.cbegin(); it != map.cend();) {
            erased = false;
            for (auto &r : match_requirements) {
                uint16 match = r.minimum_should_match;
                for (const auto &s : it->second.score_set) {
                    if (s.cat == r.cat) {
                        if (--match == 0) {
                            break;
                        }
                    }
                }
                if (match > 0) {
                    BM25LOG("Doc_id %lu does not match requirement for %u", it->first, r.cat);
                    it = map.erase(it);
                    erased = true;
                    break;
                }
            }
            if (!erased) {
                ++it;
            }
        }
    };
    const auto default_range_check_f = [this, &check_match](score_res_map &map, uint64, uint64) {
        check_match(map);
        struct {
            typename score_res_map::const_iterator start;
            typename score_res_map::const_iterator end;
        } ret = {map.cbegin(), map.cend()};
        return ret;
    };
    BM25Store &store = *(BM25Store *)in_store;

    if (!has_top_group()) {
        if (!ordered_search_optimized) {
            return;
        }
        if (match_requirements.empty()) {
            perform_search(*this, topk, store, default_id_f, default_range_f);
        } else {
            perform_search(*this, topk, store, default_id_f, default_range_check_f);
        }
        return;
    }

    QueryGroup &top_group = get_top_group();
    if (!ordered_search_optimized) {
        DocumentStats stats[store.get_nattr()];
        ItemPointerData tid;
        Oid part_id;
        uint64 doc_id = next(top_group);
        while (doc_id != 0) {
            if (!store.get_doc_info(doc_id, stats, &tid, part_id)) {
                continue;
            }
            top_explored.emplace(get_score(doc_id, stats), QueryContext::QueryRes{tid, part_id});
            doc_id = next(top_group);
        }
        return;
    }

    RoaringBitMap<> bitmap;
    perform_search(*this, topk, store, default_id_f,
                   [this, &bitmap, &check_match](score_res_map &map, uint64 from, uint64 to) {
        if (!match_requirements.empty()) {
            check_match(map);
        }
        BM25LOG("Start filtering %lu candidates", map.size());
        bitmap.clear();
        map.ctraverse([&bitmap](const auto &p) {
            bitmap.set(p.first);
        });
        QueryGroup &top = get_top_group();
        Assert(from > 0);
        top.no_less_than = from - 1ul;
        auto it = bitmap.after(from);
        uint64 top_at = 0;
        while (it != bitmap.end()) {
            uint64 target_id = *it;
            if (target_id > to) {
                break;
            }
            if (top_at < target_id) {
                top.no_less_than = target_id - 1ul;
                top_at = next(top);
            }
            if (top_at != target_id) {
                BM25LOG("Filter out doc_id %lu", target_id);
                it.reset_to_next();
            } else {
                BM25LOG("Accept doc_id %lu", target_id);
                ++it;
            }
        }
        class InternalIterator {
            RoaringBitMap<>::iterator it;
            score_res_map &res_map;
        public:
            InternalIterator(RoaringBitMap<>::iterator it, score_res_map &map)
                : it(it), res_map(map) {}
            bool operator!=(const InternalIterator &other) const
                { return it != other.it; }
            score_res_map::data_type *operator->()
                { return res_map.find(*it).operator->(); }
            score_res_map::data_type &operator*()
                { return *res_map.find(*it); }
            InternalIterator &operator++()
                { ++it; return *this; }
        };
        struct {
            InternalIterator start;
            InternalIterator end;
        } res = {
            InternalIterator(bitmap.begin(), map),
            InternalIterator(bitmap.end(), map)
        };
        return res;
    });
    bitmap.desreoy();
}

/* The recursion is not likely to have a long depth.
 * I cannot think of any reasonable case with depth > 10 */
void QueryContext::init_process_group(const QueryGroup &group)
{
    check_stack_depth();
    for (uint16 i = 0; i < group.nchild_group; ++i) {
        init_process_group(group_base[group.child_group_idx[i]]);
    }
    if (group.get_type() == QueryGroupType::AND && !group.get_full_cover()) {
        set_done_explore(*this, group.idx);
    }
    if (group.query_tokens.index == uint16(-1)) {
        return;
    }
    auto &e = entries[group.query_tokens.index];
    for (auto it = e.begin(); it != e.end();) {
        QueryContextEntry &entry = *it;
        if (!entry.list.iter_next()) {
            if (group.get_type() == QueryGroupType::AND) {
                set_done_explore(*this, group.idx);
                break;
            }
            it = e.erase(it);
        } else {
            entry.list.get_doc_ids(entry.max_offset);
            this->record_load();
            ++it;
        }
    }
}

uint64 QueryContext::next(QueryGroup &group)
{
    /* we don't check stack depth since this should have the same depth with init_process_group() */
    if (results[group.idx].done_explore) { /* stop if no more data */
        return 0;
    }
    CHECK_FOR_INTERRUPTS();

    if (group.get_type() == QueryGroupType::AND) {
        /**
         * For an AND scan, it is possible for @~@ scoring op being rebinded under it.
         * And as they are optional, we have to deal with it with special logics.
         * In addition, we assume all @~@ ops are rebinded under one AND op during planning,
         * so that we can know whether at least one score op is met within this AND.
         */
        uint64 cur_id = group.no_less_than;
        bool cur_id_unset;
        bool has_concensus;
        bool has_optional;
        bool has_score;
retry_3:
        do {
            has_concensus = true;
            cur_id_unset = true;
            has_optional = false;
            has_score = false;
            for (uint16 i = 0; i < group.nchild_group; ++i) {
                QueryGroup &g = group_base[group.child_group_idx[i]];
                if (g.is_optional()) {
                    has_optional = true;
                    continue;
                }
                g.no_less_than = cur_id;
                uint64 res = next(g);
                if (res == 0) {
                    set_done_explore(*this, group.idx);
                    return 0;
                }
                if (cur_id_unset) {
                    cur_id = res - 1ul;
                    cur_id_unset = false;
                    BM25LOG("Group %hu set consensus to %lu by its child", group.idx, cur_id);
                } else if (res > cur_id + 1ul) {
                    cur_id = res - 1ul;
                    has_concensus = false;
                    BM25LOG("Group %hu got %lu on child %hu, reset consensus", group.idx, cur_id, group.child_group_idx[i]);
                    break;
                }
                if (g.get_need_score()) {
                    has_score = true;
                }
            }
            if (!has_concensus) {
                continue;
            }
            if (group.query_tokens.index == uint16(-1)) {
                break;
            }
            auto &il_arr = entries[group.query_tokens.index];
            for (auto &entry : il_arr) {
                if (!entry.locate_id(cur_id, *this)) {
                    set_done_explore(*this, group.idx);
                    return 0;
                }
                const uint64 res = entry.list.get_doc_ids()[entry.cur_offset].doc_id;
                if (cur_id_unset) {
                    cur_id = res - 1ul;
                    cur_id_unset = false;
                    BM25LOG("Group %hu set consensus to %lu by its token", group.idx, cur_id);
                } else if (res > cur_id + 1ul) {
                    cur_id = res - 1ul;
                    has_concensus = false;
                    BM25LOG("Group %hu got %lu on entry(%f), reset consensus", group.idx, cur_id, entry.wscore);
                    break;
                }
            }
        } while (!has_concensus);
        if (cur_id_unset) {
            if (!has_optional) {
                set_done_explore(*this, group.idx);
                return 0;
            }
            uint64 res = UINT64_MAX;
            for (uint16 i = 0; i < group.nchild_group; ++i) {
                QueryGroup &g = group_base[group.child_group_idx[i]];
                if (!g.is_optional()) {
                    break;
                }
                g.no_less_than = cur_id;
                uint64 temp = next(g);
                if (res > temp && temp > 0) {
                    res = temp;
                }
            }
            if (res == UINT64_MAX) {
                set_done_explore(*this, group.idx);
                return 0;
            }
            group.no_less_than = res;
            return res;
        }
        if (has_optional) {
            for (uint16 i = 0; i < group.nchild_group; ++i) {
                QueryGroup &g = group_base[group.child_group_idx[i]];
                if (g.is_optional()) {
                    g.no_less_than = cur_id;
                    uint64 res = next(g);
                    if (res == cur_id + 1ul) {
                        has_score = true;
                    }
                }
            }
            if (!has_score) {
                ++cur_id;
                goto retry_3;
            }
        }
        ++cur_id;
        group.no_less_than = cur_id;
        return cur_id;
    }
    if (group.get_type() == QueryGroupType::OR) {
        for (uint16 i = 0; i < group.nchild_group; ++i) {
            group_base[group.child_group_idx[i]].no_less_than = group.no_less_than;
            /* OR groups with children must not be gradable */
            uint64 res = next(group_base[group.child_group_idx[i]]);
            if (res == 0) {
                BM25LOG("Remove group %hu", group.child_group_idx[i]);
                errno_t rc = memmove_s(group.child_group_idx + i,
                                       sizeof(uint16) * (group.nchild_group - i),
                                       group.child_group_idx + i + 1,
                                       sizeof(uint16) * (group.nchild_group - i - 1));
                securec_check_c(rc, "\0", "\0");
                --i;
                --group.nchild_group;
                /* we cannot clean up group_base as all other pointer idx will be invalid */
                continue;
            }
            results[group.idx].explored.set(res);
        }
        auto it = results[group.idx].explored.after(group.no_less_than, false);
        const bool it_valid = group.param.minimum_should_match == 0 && !it.at_end();
        if (group.query_tokens.index == uint16(-1)) {
ret:
            if (!it_valid) {
                set_done_explore(*this, group.idx);
                return 0;
            }
            group.no_less_than = *it;
            return *it;
        }

        auto &il_arr = entries[group.query_tokens.index];
retry_1:
        if (il_arr.empty()) {
            BM25LOG("Group %hu set token list empty", group.idx);
            group.query_tokens.index = uint16(-1);
            goto ret;
        }
        const auto iter_or_list = [&](auto &&insert_id) {
            /* We don't skip anything,
             * but we still need to scan all entries simultaneously to avoid missing scores */
            for (auto entry_it = il_arr.begin(); entry_it != il_arr.end();) {
                auto &entry = *entry_it;
                if (!entry.locate_id(group.no_less_than, *this)) {
                    BM25LOG("Group %hu remove token entry(%f)", group.idx, entry_it->wscore);
                    entry_it = il_arr.erase(entry_it);
                    continue;
                }
                ++entry_it;
                insert_id(entry.list.get_doc_ids()[entry.cur_offset].doc_id);
            }
        };

        uint64 cur_id;
        if (group.param.minimum_should_match <= 1) {
            cur_id = group.no_less_than;
            iter_or_list([&](uint64 entry_id) {
                if (entry_id < cur_id || cur_id == group.no_less_than) {
                    cur_id = entry_id;
                }
            });
            if (cur_id == group.no_less_than) {
                goto retry_1;
            }
        } else {
            MultiSet<uint64> entry_ids;
retry_2:
            iter_or_list([&](uint64 entry_id) {
                entry_ids.insert(entry_id);
            });
            if (entry_ids.size() < size_t(group.param.minimum_should_match)) {
                optional_destroy(entry_ids);
                il_arr.clear();
                group.query_tokens.index = uint16(-1);
                goto ret;
            }
            auto eit = entry_ids.begin();
            cur_id = *eit;
            int n_not_matched = 0;
            int matched = 1;
            for (++eit; eit != entry_ids.end(); ++eit) {
                uint64 entry_id = *eit;
                if (entry_id == cur_id) {
                    ++matched;
                    if (matched >= group.param.minimum_should_match) {
                        break;
                    }
                    continue;
                } else {
                    n_not_matched += matched;
                    if (n_not_matched >= group.param.minimum_should_match) {
                        break;
                    }
                    matched = 1;
                    cur_id = entry_id;
                }
            }
            if (matched < group.param.minimum_should_match) {
                /* for ordered ids A B C D E F and minimum_should_match = 3,
                 * we cannot set no_less_than to be C as we don't know A and B cannot be C
                 * next time, but it will be wasteful to set it to B;
                 * the best choice is C - 1.
                 * cur_id >= 1 as we have at least one entry in entry_ids.
                 */
                group.no_less_than = std::max(group.no_less_than + 1ul, cur_id - 1ul);
                BM25LOG("Minimum should match is not satisfied, skip to %lu", group.no_less_than);
                entry_ids.clear();
                goto retry_2;
            }
            optional_destroy(entry_ids);
        }
        BM25LOG("Group %hu iterate to doc id %lu with lower bound %lu", group.idx, cur_id, group.no_less_than);

        if (it_valid) {
            const uint64 res = *it;
            if (res < cur_id) {
                cur_id = res;
                BM25LOG("Group %hu has child group satisfying doc id %lu", group.idx, res);
            }
        }
        group.no_less_than = cur_id;
        return cur_id;
    }

    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Unrecognized query group type %u", uint16(group.get_type())),
                    errhint("Internal error.")));
    return 0;   /* keep compiler quiet */
}

float QueryContext::get_score(uint64 doc_id, const DocumentStats *doc_stats, QueryGroup &group)
{
    float res = 0;
    for (uint16 i = 0; i < group.nchild_group; ++i) {
        res += get_score(doc_id, doc_stats, group_base[group.child_group_idx[i]]);
    }
    if (group.get_need_score() && !group.get_is_bm25() && group.query_tokens.index != uint16(-1)) {
        for (const auto &entry : entries[group.query_tokens.index]) {
            const auto &l_entry = entry.list.get_doc_ids()[entry.cur_offset];
            if (l_entry.doc_id == doc_id) {
                res += scorers[entry.attrno - 1]->doc_score(
                    doc_stats[entry.attrno - 1], l_entry.freq) * entry.wscore;
            }
        }
    }
    return res;
}

void QueryContext::reset_iter(QueryGroup &group)
{
    if (group.query_tokens.index != uint16(-1)) {
        for (auto &e : entries[group.query_tokens.index]) {
            e.cur_offset = 0;
            e.max_offset = 0;
            e.list.reset();
        }
    }
    for (uint16 i = 0; i < group.nchild_group; ++i) {
        reset_iter(group_base[group.child_group_idx[i]]);
    }
}

void QueryContext::collect_result(QueryGroup &group)
{
    if (group.query_tokens.toks.empty() || entries[group.query_tokens.index].empty()) {
        return;
    }
    /* reserved for parallel optimization */
}

static char *traverse_to_end(char *str)
{
    for (;;) {
        if (*str == '\0' || t_isspace(str) || (str[0] == '>' && str[1] == '@')) {
            return str;
        }
        str += pg_mblen(str);
    }
}

/* str has to be trimed */
static char *try_extract_param(char *str, QueryGroupParam &param, bool &min_should_match_percent)
{
    new (&param) QueryGroupParam();
    size_t l = strlen(str);
    if (l < 2ul || str[l - 1ul] != '@' || str[l - 2ul] != '>') {
        return NULL;
    }
    char *temp = strstr(str, "@<");
    char *start = NULL;
    while (temp) {
        start = temp + 2;
        temp = strstr(start, "@<");
    }
    if (!start) {
        return NULL;
    }

    bool res = false;
    l -= (size_t)(start - str) + 2ul;
    char *param_pos = start - 2;
    while (l > 0) {
        if (strncmp(start, "PARAM:", 6ul) == 0) {
            start += 6;
            if (strncmp(start, "MINIMUM_SHOULD_MATCH=", 21ul) == 0) {
                res = true;
                start += 21;
                bool is_neg = *start == '-';
                if (is_neg) {
                    ++start;
                }
                char *end = traverse_to_end(start);
                min_should_match_percent = end[-1] == '%';
                char end_c;
                if (min_should_match_percent) {
                    end[-1] = '\0';
                } else {
                    end_c = *end;
                    *end = '\0';
                }
                char *temp = start;
                uint32 min_should_match = strtoul(start, &temp, 10);
                if (is_neg) {
                    --start;
                }
                if (temp == start + (is_neg ? 1 : 0) || *temp != '\0' ||
                    temp != end - (min_should_match_percent ? 1 : 0)) {
                    if (min_should_match_percent) {
                        end[-1] = '%';
                        *end = '\0';
                    }
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                    errmsg("Invalid MINIMUM_SHOULD_MATCH value: %s", start)));
                }
                if (min_should_match_percent && min_should_match > 100u) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                    errmsg("MINIMUM_SHOULD_MATCH percent number must be "
                                           "between 0 and 100, got %u", min_should_match)));
                }
                param.minimum_should_match = (is_neg ? -1 : 1) * min_should_match;
                if (min_should_match_percent) {
                    end[-1] = '%';
                } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"  /* GCC bug for false reporting */
                    *end = end_c;
#pragma GCC diagnostic pop
                }
                l -= 27ul + (size_t)(end - start);
                start = end;
            } else if (strncmp(start, "BOOST=", 6) == 0) {
                res = true;
                start += 6;
                char *end = traverse_to_end(start);
                char end_c = *end;
                *end = '\0';
                char *temp = start;
                float boost = strtof(start, &temp);
                if (temp == start || *temp != '\0' || temp != end) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                    errmsg("Invalid BOOST value: %s", start)));
                }
                if (boost <= 0.0f || isinf(boost) || isnan(boost)) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                    errmsg("BOOST value must be greater than 0, got %f", boost)));
                }
                param.boost = boost;
                *end = end_c;
                l -= 12ul + (size_t)(end - start);
                start = end;
            } else if (strncmp(start, "DICT=", 5) == 0) {
                res = true;
                start += 5;
                char *end = traverse_to_end(start);
                char end_c = *end;
                *end = '\0';
                char *dict_name = start;
                Oid dict_id = get_dict_oid(dict_name);
                param.dict_id = dict_id;
                *end = end_c;
                l -= 11ul + (size_t)(end - start);
                start = end;
            } else if (strncmp(start, "EXTEND=", 7) == 0) {
                res = true;
                start += 7;
                char *end = traverse_to_end(start);
                char end_c = *end;
                *end = '\0';
                if (!parse_bool(start, &param.extend)) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("EXTEND must be boolean value, got \"%s\"", start)));
                }
                *end = end_c;
                l -= 13ul + (size_t)(end - start);
                start = end;
            } else if (strncmp(start, "SCORING=", 8) == 0) {
                res = true;
                start += 8;
                char *end = traverse_to_end(start);
                char end_c = *end;
                *end = '\0';
                if (strcasecmp(start, "plain") == 0 || strcasecmp(start, "default") == 0) {
                    param.norm = false;
                } else if (strcasecmp(start, "normalized") == 0 || strcasecmp(start, "norm") == 0) {
                    param.norm = true;
                } else {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("Unknown SCORING parameter at: %s", start),
                                errhint("Valid parameters are: "
                                        "plain/default and normalized/norm")));
                }
                *end = end_c;
                l -= 14ul + (size_t)(end - start);
                start = end;
            } else {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("Unknown query parameter at: %s", start),
                                errhint("Valid parameters are: MINIMUM_SHOULD_MATCH, BOOST, DICT, "
                                        "EXTEND, SCORING")));
            }
        } else {
            ereport(WARNING, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                              errmsg("Found @<>@ query setting but the "
                                     "format is incorrect at \"%s\", ignored", start),
                              errhint("Supported format: \"@<PARAM:param_name=value"
                                      "[ PARAM:param_name=value]>@\"")));
            break;
        }
        while (*start && t_isspace(start)) {
            const size_t sp_len = pg_mblen(start);
            start += sp_len;
            l -= sp_len;
        }
    }
    if (res) {
        *param_pos = '\0';
    }
    return param_pos;
}

static void set_minimum_should_match(size_t keywords_size, QueryGroupParam &param, bool is_percent)
{
    if (is_percent) {
        float percent = param.minimum_should_match / 100.0f;
        if (param.minimum_should_match >= 0) {
            param.minimum_should_match = floorf(percent * keywords_size);
        } else {
            size_t s = ceilf(-percent * keywords_size);
            param.minimum_should_match = keywords_size > s ?
                keywords_size - s : 0;
        }
    } else if (param.minimum_should_match < 0) {
        size_t minus_size = -param.minimum_should_match;
        param.minimum_should_match = keywords_size > minus_size ?
            keywords_size - minus_size : 0;
    } else if ((size_t)param.minimum_should_match > keywords_size) {
        param.minimum_should_match = keywords_size;
    }
}

static Vector<char *> extract_query(RowQuery &row_query, Oid dict_id, QueryGroupParam &param,
                                    bool &use_percent)
{
    if (OidIsValid(dict_id)) {
        Assert(row_query.query);
        char *q = trim(row_query.query);
        char *param_pos = try_extract_param(q, param, use_percent);
        if (param.dict_id == InvalidOid) {
            param.dict_id = dict_id;
        }
        void *dict = get_jieba(param.dict_id);
        Vector<char *> keywords = param.extend ?
            ((cppjieba::Jieba *)dict)->cut_query(q) :
            ((cppjieba::Jieba *)dict)->cut_mix(q);
        release_dict_resource(dict);
        if (param_pos && *param_pos == '\0') {
            *param_pos = '@';
        }
        return keywords;
    }

    Assert(row_query.queries);
    Vector<char *> keywords;
    char **cur = row_query.queries;
    while (*cur) {
        keywords.push_back(pstrdup(*cur));
        ++cur;
    }
    pfree_ext(row_query.queries);
    if (keywords.empty()) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Query clause must not be empty")));
    }
    char *param_str = keywords.back();
    if (param_str && param_str[0] == '@' && param_str[1] == '<' &&
        try_extract_param(param_str, param, use_percent)) {
        pfree(param_str);
        keywords.pop_back();
        if (keywords.empty()) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Query clause must not be empty")));
        }
    }
    return keywords;
}

static QueryGroup *pull_up_group(Vector<QueryGroup> &&child_group, QueryGroupType type,
                                 size_t pull_up_idx)
{
    QueryGroup base(std::move(child_group[pull_up_idx]));
    child_group.erase(child_group.at(pull_up_idx));
    for (auto &qg : base.child_group) {
        child_group.push_back(std::move(qg));
    }
    base.child_group.clear_no_destroy();

    QueryGroup *res = NEW QueryGroup(std::move(base.query_tokens), std::move(child_group), base.param);
    res->set_score(base.get_need_score(), false);
    res->set_type(type);
    res->set_bm25(base.get_is_bm25());
    base.destroy();
    return res;
}

static QueryGroup *extract_top_qg(Vector<QueryGroup> &&child_group)
{
    if (child_group.empty()) {
        optional_destroy(child_group);
        return NULL;
    }

    if (child_group.size() == 1ul) {
        QueryGroup *res = NEW QueryGroup(std::move(child_group.front()));
        child_group.clear_no_destroy();
        optional_destroy(child_group);
        return res;
    }

    /* all OR or all AND */
    QueryGroupType type = child_group[0].get_type();
    bool all_type_same = true;
    for (const auto &qg : child_group) {
        if (type != qg.get_type()) {
            all_type_same = false;
            break;
        }
    }
    if (!all_type_same) {
        size_t pull_up_idx = 0;
        /* there must be at least one AND */
        for (const auto &qg : child_group) {
            if (qg.get_type() == QueryGroupType::AND) {
                break;
            }
            ++pull_up_idx;
        }
        return pull_up_group(std::move(child_group), QueryGroupType::AND, pull_up_idx);
    }
    if (type == QueryGroupType::AND) {
        return pull_up_group(std::move(child_group), QueryGroupType::AND, child_group.size() - 1ul);
    }

    /* for multiple @-@ or @~@ with msm > 0, we need to combine it with AND */
    bool or_set[child_group.size()] = {false};
    uint32 count = 0;
    uint32 and_count = 0;
    for (const auto &qg : child_group) {
        if (!qg.get_need_score() || qg.param.minimum_should_match > 0) {
            ++and_count;
            or_set[count] = true;
        }
        ++count;
    }
    if (and_count == 0) {
        return pull_up_group(std::move(child_group), type, child_group.size() - 1ul);
    }

    QueryGroup *res = NEW QueryGroup(QueryToken());
    res->set_score(false, false);
    res->set_type(QueryGroupType::AND);
    for (uint32 i = 0; i < count; ++i) {
        if (or_set[i]) {
            res->child_group.push_back(std::move(child_group[i]));
        }
    }
    const size_t rem_size = count - and_count;
    if (rem_size > 1ul) {
        for (uint32 i = 1u; i <= count; ++i) {
            uint32 idx = count - i;
            if (or_set[idx]) {
                child_group.erase(child_group.at(idx));
            }
        }
        QueryGroup *temp = pull_up_group(std::move(child_group), QueryGroupType::OR, rem_size - 1ul);
        res->child_group.push_back(std::move(*temp));
        pfree(temp);
    } else if (rem_size == 1ul) {
        for (uint32 i = 0; i < count; ++i) {
            if (!or_set[i]) {
                res->child_group.push_back(std::move(child_group[i]));
                break;
            }
        }
        optional_destroy(child_group);
    }
    return res;
}

void BM25Scanner::extract_query_group(QueryGroup **filters, QueryGroup **queries, uint16 &nqueries,
                                      const Oid *dict_ids)
{
    Vector<QueryGroup> query_group;
    Vector<QueryGroup> child_group;
    QueryGroupParam param;
    for (auto &row_query : row_queries) {
        if (row_query.need_score()) {
            const Oid dict_id = dict_ids[row_query.attrno - 1];
            bool use_percent = false;
            Vector<char *> keywords = extract_query(row_query, dict_id, param, use_percent);
            UnorderedMap<CharString, uint32> tmp_map;
            for (char *keyword : keywords) {
                auto res = tmp_map.emplace(keyword, 1ul);
                if (!res.second) {
                    res.first->second += 1ul;
                    pfree(keyword);
                }
            }
            ann_helper::optional_destroy(keywords);

            QueryToken bm25_qry_tokens;
            bm25_qry_tokens.attrno = row_query.attrno;
            tmp_map.ctraverse([&bm25_qry_tokens](const auto &p) -> void {
                bm25_qry_tokens.toks.push_back(p.first.str());
                bm25_qry_tokens.frequencies.push_back(p.second);
            });
            set_minimum_should_match(tmp_map.size(), param, use_percent);
            ann_helper::optional_destroy(tmp_map);
            param.boost *= row_query.weight;

            if (require_order_by) {
                query_group.emplace_back(std::move(bm25_qry_tokens), param);
                auto &res = query_group.back();
                res.set_type(QueryGroupType::OR);
                res.set_score(true, true);
            } else {
                child_group.emplace_back(std::move(bm25_qry_tokens), param);
                auto &res = child_group.back();
                res.set_type(QueryGroupType::OR);
                res.set_score(true, false);
            }
        } else {
            if (!row_query.query) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("@-@ query clause only takes non-empty strings")));
            }
            QueryGroup *res = QueryGroup::parse_query_group(row_query.query, row_query.attrno);
            if (OidIsValid(dict_ids[row_query.attrno - 1])) {
                auto *dict = (cppjieba::Jieba *)get_jieba(dict_ids[row_query.attrno - 1]);
                Vector<char *> r = dict->post_processing(res->query_tokens.as_toks());
                release_dict_resource(dict);
                free_str_container(res->query_tokens.as_toks());
                res->query_tokens.as_toks() = r;
            }
            res->set_score(false, false);
            child_group.push_back(std::move(*res));
            delete res;
        }
    }
    for (const auto &row_vector : row_vectors) {
        QueryToken bm25_qry_tokens;
        bm25_qry_tokens.is_text = false;
        bm25_qry_tokens.attrno = row_vector.attrno;
        const uint32 *dims = row_vector.vec->indices;
        bm25_qry_tokens.toks.push_back(dims, dims + row_vector.vec->nnz);
        const uint16 *values = SPARSEVEC_VALUES(row_vector.vec);
        bm25_qry_tokens.frequencies.push_back(values, values + row_vector.vec->nnz);
        query_group.emplace_back(std::move(bm25_qry_tokens));
        auto &res = query_group.back();
        res.param.boost = row_vector.weight;
        res.set_type(QueryGroupType::OR);
        res.set_score(true, true);
        res.set_bm25(true);
    }

    QueryGroup *top_qg = extract_top_qg(std::move(child_group));
    if (top_qg) {
        top_qg->print();
    }
    *filters = top_qg;

    for (const auto &qg : query_group) {
        qg.print("Query clause:");
    }
    *queries = query_group.data();
    nqueries = query_group.size();
    if (nqueries == 0) {
        pfree_ext(*queries);
    }
}

uint32 BM25Scanner::get_topk() const
{
    if (!linfo.limit_set || !linfo.with_limit) {
        return max_daat_threshold + 1u;
    }
    uint64 res = linfo.limit_count + linfo.limit_offset;
    return res > __UINT32_MAX__ ? max_daat_threshold + 1u : std::max(3u, uint32(res));
}
