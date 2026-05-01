/**
 * Copyright ...
 * B+ Tree range search implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_SEARCH_H
#define DISKANN_CONTAINER_BPLUSTREE_SEARCH_H

#include "access/hybridann/bplustree/bplustree.h"

#define OUTPUT_BTVECLOG false

#include <algorithm>    /* min_element, sort */
#if OUTPUT_BTVECLOG
#include <sstream>
#endif /* OUTPUT_BTVECLOG */

namespace disk_container {
#if OUTPUT_BTVECLOG && !defined(__OPTIMIZE__)
#define BTVECLOG(fmt, ...) ereport(NOTICE, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define BTVECLOG(fmt, ...)
#endif /* OUTPUT_BTVECLOG */
class SearchBPlusTree : public virtual BPlusTreeImpl {
    using super = BPlusTreeImpl;
    struct SearchRange {
        NodePtr start;
        NodePtr end;
        uint16 level;
        SearchRange() {}
        SearchRange(NodePtr lptr, NodePtr rptr, uint16 level)
            : start(lptr), end(rptr), level(level) {}
    };
    using search_ranges = Vector<SearchRange>;
public:
    using super::BPlusTreeImpl; /* inherit constructor */
    struct ScanResult {
        NodePtr idx_ptr;
        uint16 level;
        float  cover_ratio;
        ScanResult(NodePtr ptr, uint16 level) : idx_ptr(ptr), level(level), cover_ratio(0) {}
        ScanResult(NodePtr ptr, uint16 level, float cover) : idx_ptr(ptr), level(level), cover_ratio(cover) {}
    };
    using scan_results = Vector<ScanResult>;
    static constexpr uint16 leaf_level = uint16(-1);
    scan_results search_coverage(const ScanKeyType &scan_data, const float cover_threshold)
    {
        if (!scan_data.single_range()) {
            base_range U;
            index_ranges S;
            auto leaf_res = _retrieve_coverage(scan_data, U, S, cover_threshold);
            auto res = _optimal_cover(U, S);
            ann_helper::optional_destroy(U);
            ann_helper::optional_destroy(S);
            merge_results(res, std::move(leaf_res));
            return res;
        }

        scan_results res;
        search_ranges ranges;
        _ctx.r_lock();
        NodePtr ptr = _ctx.get_root_ptr();
        _ctx.r_unlock();
        ranges.emplace_back(ptr, ptr, _ctx.index_magnitude()->size() - 1u);
        bool left_open = true;
        bool right_open = true;
        while (!ranges.empty()) {
            _search_coverage_recurse(scan_data, ranges, left_open, right_open, res, cover_threshold);
            CHECK_FOR_INTERRUPTS();
        }
        return res;
    }
private:
    void merge_results(scan_results &target, scan_results &&from)
    {
        target.push_back(from.cbegin(), from.cend());
        ann_helper::optional_destroy(from);
    }

    /* single range search algorithm */
    void _iter_leaf_range(NodeImpl &&node, NodePtr end, scan_results &res)
    {
        NodePtr n = node.ptr();
        res.emplace_back(n, leaf_level);
        if (n == end) {
            node.r_unlock_destroy();
            return;
        }
        n = node.next();
        node.r_unlock_destroy();
        for (; n != end;) {
            if (n == _ctx.invalid_ptr()) {
                ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("unexpected end of index \"%s\" while scanning leaf nodes",
                     RelationGetRelationName(_ctx.get_index()))));
            }
            res.emplace_back(n, leaf_level);
            NodeImpl next_node = _ctx.get_node(n);
            next_node.r_lock();
            n = next_node.next();
            next_node.r_unlock_destroy();
        }
        res.emplace_back(end, leaf_level);
    }
    float _index_cover_range(VectorIndex *idx, const ScanKeyType &scan_data)
    {
        constexpr uint32 max_sample = 20u;
        size_t nblock;
        BlockNumber *blocks = idx->get_node_blkno(nblock);
        BlockNumber *original_blocks = blocks; 
        Assert(nblock > 0);
        struct sample_block {
            BlockNumber blkno;
            size_t nitem;
            sample_block(BlockNumber blkno, size_t nitem) : blkno(blkno), nitem(nitem) {}
        };
        Vector<sample_block> samples(max_sample);
        auto generate_swap = [&nblock, &blocks](size_t start) -> BlockNumber {
            int res = random() % (nblock - start) + start;
            std::swap(blocks[start], blocks[res]);
            return blocks[start];
        };
        if (nblock > max_sample) {
            --nblock;
            samples.emplace_back(blocks[0], 1ul);
            samples.emplace_back(blocks[nblock], 1ul);
            ++blocks;
            for (uint32 i = 0; i < max_sample - 2u; ++i) {
                samples.emplace_back(generate_swap(i), 1ul);
            }
            --blocks;
            ++nblock;
        } else {
            size_t base = max_sample / nblock;
            size_t quotient = max_sample % nblock;
            for (size_t i = 0; i < quotient; ++i) {
                samples.emplace_back(generate_swap(i), base + 1ul);
            }
            for (size_t i = quotient; i < nblock; ++i) {
                samples.emplace_back(blocks[i], base);
            }
        }
        size_t nsample = max_sample;
        size_t ncover = 0;
        for (auto &sample : samples) {
            NodeImpl node = _ctx.get_node(sample.blkno);
            node.r_lock();
            OffsetNumber end = node.size();
            OffsetNumber s = node.is_right_most() ? end : end - 1u;
            if (node.is_internal()) {
                if (s <= 1u) {
                    node.r_unlock_destroy();
                    continue;
                }
                s -= 1;
            }
            if (sample.blkno == blocks[0]) {
                OffsetNumber offset = end - s + 1u;
                if (node.is_left_most()) {
                    ++offset;
                }
                if (end >= offset && scan_data(*node.get_data(offset))) {
                    ++ncover;
                }
                --sample.nitem;
            } else if (sample.blkno == blocks[nblock - 1ul]) {
                if (scan_data(*node.get_data(end))) {
                    ++ncover;
                }
                --sample.nitem;
            }
            if (sample.nitem == 0) {
                node.r_unlock_destroy();
                continue;
            }
            if (sample.nitem == 1ul) {
                /* VEC TD: add fastpath check by tracking hikey of each node */
            }
            OffsetNumber n, step;
            if (s < sample.nitem) {
                step = 1u;
                n = s;
                nsample -= sample.nitem - s;
            } else {
                step = s / sample.nitem;
                n = sample.nitem;
            }
            for (size_t i = 0; i < n; ++i) {
                OffsetNumber target = end - i * step;
                if (scan_data(*node.get_data(target))) {
                    ++ncover;
                }
            }
            node.r_unlock_destroy();
        }
        pfree(original_blocks);
        ann_helper::optional_destroy(samples);


        BTVECLOG("%s index %u has coverage: (%lu/%lu)",
                 idx->type_name(), idx->ptr(),  ncover, nsample);
        return  (float)ncover / nsample;
    }
    bool _extract_index(const ScanKeyType &scan_data, NodePtr start_idx_ptr, NodePtr end_node,
        NodePtr &l_uncover, NodePtr &r_uncover, uint16 level, scan_results &res, const float cover_threshold)
    {
        bool reach_end = false;
        bool at_least_one_idx = false;
        bool set_l_uncover = false;
        bool use_index;
        float cover_ratio = 1.0;
        for (NodePtr cur_ptr = start_idx_ptr; !reach_end;) {
            use_index = true;
            VectorIndex *cur_idx = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), cur_ptr);
            if (cur_idx->type() == VectorIndexType::None) {
                delete_vector_index(cur_idx);
                return false;
            }
            cur_idx->r_lock();
            if (set_l_uncover) {
                NodePtr l_ptr = cur_idx->leftmost_node();
                NodeImpl lnode = _ctx.get_node(l_ptr);
                /* no rlock is applied, lock does nothing in reading prev pointer here */
                l_uncover = lnode.prev();
                lnode.destroy();
                set_l_uncover = false;
                BTVECLOG("set l_uncover to %u", l_uncover);
            }
            if (cur_ptr == start_idx_ptr) {
                cover_ratio = _index_cover_range(cur_idx, scan_data);
                if (cover_ratio < cover_threshold) {
                    use_index = false;
                    set_l_uncover = true;
                }
            }
            size_t nnode;
            BlockNumber *nodes = cur_idx->get_node_blkno(nnode);
            for (size_t i = 0; i < nnode; ++i) {
                if (nodes[i] == end_node) {
                    reach_end = true;
                    break;
                }
            }
            pfree(nodes);
            if (reach_end && cur_ptr != start_idx_ptr) {
                cover_ratio = _index_cover_range(cur_idx, scan_data);
                if (cover_ratio < cover_threshold) {
                    r_uncover = cur_idx->leftmost_node();
                    use_index = false;
                    BTVECLOG("set r_uncover to %u", r_uncover);
                }
               
            }
            if (use_index) {
                at_least_one_idx = true;
                res.emplace_back(cur_ptr, level, cover_ratio);
            }
            cur_ptr = cur_idx->next();
            Assert(reach_end || cur_ptr != _ctx.invalid_ptr());
            cur_idx->r_unlock();
            delete_vector_index(cur_idx);
        }
        return at_least_one_idx;
    }
    void _search_coverage_recurse(const ScanKeyType &scan_data, search_ranges &ranges,
        bool &left_open, bool &right_open, scan_results &res, const float cover_threshold)
    {
        search_ranges new_range(ranges.size() * 2ul);
        size_t n = ranges.size();
        for (size_t i = 0; i < n; ++i) {
            const auto &range = ranges[i];
            uint16 target_level = range.level;
            NodeImpl lnode = _ctx.get_node(range.start);
            lnode.r_lock();
            BTVECLOG("probing range %u %u with ilevel %u nlevel %u",
                     range.start, range.end, target_level, lnode.level());

            const auto get_loffset = [&](NodeImpl &node) -> OffsetNumber {
                return i == 0 && left_open ? node.item_index_of(scan_data) : p_firstdatakey(lnode);
            };
            const auto get_roffset = [&](NodeImpl &node) -> OffsetNumber {
                return i == n - 1ul && right_open ? node.right_item_index_of(scan_data) : node.size();
            };
            const auto end_ptr = [&](NodeImpl &node, OffsetNumber &roffset) -> NodePtr {
                roffset = get_roffset(node);
                if (node.level() == 1u && roffset < node.size()) {
                    roffset = OffsetNumberNext(roffset);
                }
                return node.get_ptr(roffset);
            };

            NodePtr lidx = range.level >= _ctx.index_magnitude()->size() ?
                InvalidBlockNumber :
                lnode.get_index_ptr(range.level);
            if (lidx == InvalidBlockNumber) {
                if (lnode.is_leaf()) {
                    _iter_leaf_range(std::move(lnode), range.end, res);
                    continue;
                }
                OffsetNumber loffset = get_loffset(lnode);
                NodePtr ridx;
                OffsetNumber roffset;
                if (range.start != range.end) {
                    NodeImpl rnode = _ctx.get_node(range.end);
                    rnode.r_lock();
                    Assert(rnode.level() == lnode.level());
                    ridx = end_ptr(rnode, roffset);
                    rnode.r_unlock_destroy();
                } else {
                    ridx = end_ptr(lnode, roffset);
                }
                new_range.emplace_back(lnode.get_ptr(loffset), ridx, target_level);
                BTVECLOG("input offset %u %u from node %u,%u ilevel %u nlevel %u",
                         loffset, roffset, range.start, range.end, target_level, lnode.level() - 1u);
            } else {
                NodePtr l_uncover = _ctx.invalid_ptr();
                NodePtr r_uncover = _ctx.invalid_ptr();
                bool idx_splitted = _extract_index(scan_data, lidx, range.end,
                                                   l_uncover, r_uncover, target_level, res, cover_threshold);
                if (idx_splitted) {
                    if (target_level > 0) {
                        --target_level;
                    }
                    if (l_uncover != _ctx.invalid_ptr()) {
                        new_range.emplace_back(range.start, l_uncover, target_level);
                        BTVECLOG("input range %u %u with ilevel %u nlevel %u",
                                 range.start, l_uncover, target_level, lnode.level());
                    } else if (i == 0 && left_open) {
                        left_open = false;
                    }
                    if (r_uncover != _ctx.invalid_ptr()) {
                        new_range.emplace_back(r_uncover, range.end, target_level);
                        BTVECLOG("input range %u %u with ilevel %u nlevel %u",
                                 r_uncover, range.end, target_level, lnode.level());
                    } else if (i == n && right_open) {
                        right_open = false;
                    }
                } else if (target_level > 0) {
                    --target_level;
                    new_range.emplace_back(range.start, range.end, target_level);
                    BTVECLOG("input range %u %u with ilevel %u nlevel %u",
                             range.start, range.end, target_level, lnode.level());
                } else if (lnode.is_leaf()) {
                    _iter_leaf_range(std::move(lnode), range.end, res);
                    continue;
                } else {
                    OffsetNumber loffset = i == 0 && left_open ?
                        lnode.item_index_of(scan_data) :
                        p_firstdatakey(lnode);
                    NodePtr lptr = lnode.get_ptr(loffset);
                    NodePtr rptr;
                    OffsetNumber unused;
                    if (range.start != range.end) {
                        NodeImpl rnode = _ctx.get_node(range.end);
                        rnode.r_lock();
                        Assert(rnode.level() == lnode.level());
                        rptr = end_ptr(rnode, unused);
                        rnode.r_unlock_destroy();
                    } else {
                        rptr = end_ptr(lnode, unused);
                    }
                    new_range.emplace_back(lptr, rptr, 0);
                    BTVECLOG("input range %u %u with ilevel %u nlevel %u",
                             lptr, rptr, 0, lnode.level() - 1u);
                }
            }
            lnode.r_unlock_destroy();
        }
        ranges.swap(new_range);
        ann_helper::optional_destroy(new_range);
    }

    /* multiple range search algorithm */
    using range_cover = Vector<NodePtr>;
    using base_range = range_cover;
    struct index_range {
        NodePtr idx_ptr;
        range_cover nodes;
        index_range() = default;
        index_range(const index_range &ir) : idx_ptr(ir.idx_ptr), nodes(ir.nodes) {}
        index_range(NodePtr idx_ptr, range_cover &&nodes) : idx_ptr(idx_ptr), nodes(std::move(nodes)) {}
        void destroy() { ann_helper::optional_destroy(nodes); }
    };
    using index_ranges = Vector<Vector<index_range>>;
#if OUTPUT_BTVECLOG
    static char *to_str(const index_range &S)
    {
        std::stringstream ss;
        ss << S.idx_ptr << ", nodes: ";
        for (const auto &n : S.nodes) {
            ss << n << " ";
        }
        return strdup(ss.str().c_str());
    }
    static char *to_str(const index_ranges &S)
    {
        std::stringstream ss;
        for (const auto &ls : S) {
            if (ls.empty()) {
                continue;
            }
            ss << "level " << &ls - &S[0] << ": ";
            for (const auto &s : ls) {
                ss << to_str(s) << " ";
            }
            ss << "\n";
        }
        return strdup(ss.str().c_str());
    }
#else
    static char *to_str(const index_range &) { return NULL; }
    static char *to_str(const index_ranges &) { return NULL; }
#endif /* OUTPUT_BTVECLOG */

    /* range calculation: decide problem U and S */
    bool _extract_leaf(NodeImpl leaf, const ScanKeyType &scan_data, bool &forward, const float threshold)
    {
        double cover_threshold = sqrt(threshold);
        const OffsetNumber s = p_firstdatakey(leaf);
        const OffsetNumber e = leaf.size();
        const OffsetNumber fail_threshold = (1.0 - cover_threshold) * (e - s);
        const OffsetNumber success_threshold = cover_threshold * (e - s);
        OffsetNumber count = 0;
        OffsetNumber failed = 0;
        forward = true;
        for (OffsetNumber i = s; i <= e; i = OffsetNumberNext(i)) {
            if (scan_data(*leaf.get_data(i), forward)) {
                ++count;
                if (count >= success_threshold) {
                    return true;
                }
            } else if (!forward) {
                return false;
            } else {
                ++failed;
                if (failed > fail_threshold) {
                    return false;
                }
            }
        }
        return true; /* keep compiler quiet */
    }

    struct temp_index_range {
        VectorIndex *idx{NULL};
        range_cover nodes;
        size_t nfailed{0};
        uint16 level;
        uint16 nlevel{(uint16)-1};
        uint16 nexplored_children{0};
        bool valid;
    };
    NodeImpl _retrieve_coverage_preprocess(const ScanKeyType &scan_data, base_range &U, index_ranges &S,
        Vector<temp_index_range> &index_nodes, Vector<NodeImpl> &parent_nodes, const float cover_threshold)
    {
        const size_t index_magnitude_size = _ctx.index_magnitude()->size();

        Assert(S.empty());
        for (size_t i = 0; i < index_magnitude_size; ++i) {
            S.emplace_back();
        }
        PathStack path;
        NodeImpl leaf = iterate_to_leaf(scan_data,
            [this, &scan_data, &path](NodeImpl &node) {
                node.r_lock();
                _move_right(node, scan_data, path, false, false);
                if (!node.is_leaf()) {
                    path.emplace_back(node.ptr(), InvalidOffsetNumber);
                }
            },
            [&path](NodeImpl &node, OffsetNumber offset) {
                node.r_unlock();
            });
        /* trim top parts of path */
        while (!path.empty()) {
            NodeImpl top = _ctx.get_node(path.front().ptr);
            top.r_lock();
            bool has_index = false;
            for (size_t i = 0; i < index_magnitude_size; ++i) {
                NodePtr idx_ptr = top.get_index_ptr(i);
                if (idx_ptr != _ctx.invalid_ptr()) {
                    VectorIndex *idx = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), idx_ptr);
                    VectorIndexType type = idx->type();
                    delete_vector_index(idx);
                    if (type != VectorIndexType::None) {
                        has_index = true;
                        break;
                    }
                }
            }
            top.r_unlock_destroy();
            if (!has_index) {
                path.pop_front();
            } else {
                break;
            }
        }

        /* set up index and nodes on each layer */
        for (size_t i = 0; i < index_magnitude_size; ++i) {
            temp_index_range range;
            range.level = i;
            range.valid = false;
            index_nodes.push_back(std::move(range));
        }
        uint16 nlevel = path.size();
        for (const auto &entry : path) {
            --nlevel;
            NodeImpl node = _ctx.get_node(entry.ptr);
            parent_nodes.push_back(node);
            node.r_lock();
            BlockNumber idx_ptrs[index_magnitude_size];
            for (size_t i = 0; i < index_magnitude_size; ++i) {
                idx_ptrs[i] = node.get_index_ptr(i);
            }
            node.r_unlock();
            for (size_t i = 0; i < index_magnitude_size; ++i) {
                NodePtr idx_ptr = idx_ptrs[i];
                if (idx_ptr == _ctx.invalid_ptr()) {
                    continue;
                }
                VectorIndex *idx = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), idx_ptr);
                if (idx->type() == VectorIndexType::None) {
                    delete_vector_index(idx);
                    continue;
                }
                temp_index_range &range = index_nodes[i];
                range.idx = idx;
                range.level = i;
                range.nlevel = nlevel;
                size_t nblock, counter = 0;
                BlockNumber *blocks = idx->get_node_blkno(nblock);
                pfree_ext(blocks);
                double threshold = sqrt(cover_threshold);
                const size_t threshold_counter = (1 - threshold) * nblock;
                NodePtr cur = idx->leftmost_node();
                for (;;) {
                    Assert(cur != _ctx.invalid_ptr());
                    if (cur == entry.ptr || counter > threshold_counter) {
                        break;
                    }
                    NodeImpl cur_node = _ctx.get_node(cur);
                    cur_node.r_lock();
                    cur = cur_node.next();
                    cur_node.r_unlock_destroy();
                    ++counter;
                }
                range.valid = counter <= threshold_counter;
            }
        }
        ann_helper::optional_destroy(path);
        while (!index_nodes.empty() && !index_nodes.back().valid) {
            index_nodes.pop_back();
        }
        return leaf;
    }
    /**
     * @param scan_data: scan_data.
     * @param U: output base range.
     * @param S: output index ranges.
     * VEC TD: support multiple scan_data, it's easy to implement but not necessary for now.
     */
    scan_results _retrieve_coverage(const ScanKeyType &scan_data, base_range &U, index_ranges &S, const float cover_threshold)
    {
        Vector<temp_index_range> index_nodes(_ctx.index_magnitude()->size());
        Vector<NodeImpl> parent_nodes;
        NodeImpl leaf = _retrieve_coverage_preprocess(scan_data, U, S, index_nodes, parent_nodes, cover_threshold);

        /* helper methods */
        scan_results res;
        Vector<NodePtr> valid_leaves;
        const auto set_up_leaf = [&valid_leaves](NodePtr leaf_ptr) {
            valid_leaves.push_back(leaf_ptr);
        };
        const auto skip_leaf = [&index_nodes, &res](NodePtr leaf_ptr) {
            res.emplace_back(leaf_ptr, leaf_level);
        };
        const auto collect_node = [this, &index_nodes, &S, &cover_threshold](uint16 level, bool setup) {
            auto &index_node = index_nodes[level];
            bool collected = false;
            if (index_node.valid &&
                index_node.nfailed <= cover_threshold * (index_node.nodes.size() / (1.0 - cover_threshold))) {
                S[level].emplace_back(index_node.idx->ptr(), std::move(index_node.nodes));
                collected = true;
            }
            BTVECLOG("%s level-%u index %u", collected ? "Accept" : "Reject", level, index_node.idx->ptr());
            auto next_ptr = index_node.idx->next();
            delete_vector_index(index_node.idx);
            if (setup) {
                index_node.idx = VectorIndexFactory::create(_ctx.get_index(), _ctx.get_heap(), next_ptr);
                index_node.valid = true;
                index_node.nodes = range_cover();
            } else {
                index_node.valid = false;
            }
        };
        const auto iter_next_leaf = [&](NodePtr leaf_ptr) {
            const int s = parent_nodes.size();
            for (int i = s - 1; i >= 0; --i) {
                auto &parent_node = parent_nodes[i];
                parent_node.r_lock();
                size_t ps = parent_node.size();
                for (OffsetNumber j = p_firstdatakey(parent_node); j <= ps; j = OffsetNumberNext(j)) {
                    if (parent_node.get_ptr(j) == leaf_ptr) {
                        parent_node.r_unlock();
                        return; /* no need to recurse up */
                    }
                }
                if (i == s - 1) {
                    double threshold = sqrt(cover_threshold);
                    const OffsetNumber s = parent_node.size() - (parent_node.is_right_most() ? 1u : 0);
                    if (valid_leaves.size() >= threshold * s) {
                        const NodePtr cur_ptr = parent_node.ptr();
                        for (auto &idx : index_nodes) {
                            if (idx.valid) {
                                idx.nodes.push_back(cur_ptr);
                            }
                        }
                        U.push_back(cur_ptr);
                    } else {
                        for (auto &idx : index_nodes) {
                            ++idx.nfailed;
                        }
                    }
                    valid_leaves.clear();
                }
                NodePtr next_ptr = parent_node.next();
                for (auto &index_node : index_nodes) {
                    if (!index_node.valid || index_node.nlevel != i) {
                        continue;
                    }
                    Assert(index_node.level < _ctx.index_magnitude()->size());
                    if (parent_node.get_index_ptr(index_node.level) != index_node.idx->ptr()) {
                        collect_node(index_node.level, true);
                    }
                }
                parent_node.r_unlock_destroy();
                parent_node = _ctx.get_node(next_ptr);
            }
        };
        const auto end_index = [this, &index_nodes, &parent_nodes, &collect_node, &cover_threshold]() {
            const size_t s = parent_nodes.size();
            for (size_t i = 0; i < s; ++i) {
                for (auto &index_node : index_nodes) {
                    if (!index_node.valid || index_node.nlevel != i) {
                        continue;
                    }
                    size_t nblock, counter = 0;
                    BlockNumber *blocks = index_node.idx->get_node_blkno(nblock);
                    UnorderedSet<BlockNumber> block_set(blocks, blocks + nblock);
                    pfree(blocks);
                    double threshold = sqrt(cover_threshold);
                    const size_t threshold_counter = (1 - threshold) * nblock;
                    parent_nodes[i].r_lock();
                    NodePtr cur = parent_nodes[i].next();
                    parent_nodes[i].r_unlock();
                    for (;;) {
                        if (cur == _ctx.invalid_ptr() || counter > threshold_counter ||
                            !block_set.contains(cur)) {
                            break;
                        }
                        ++counter;
                        NodeImpl cur_node = _ctx.get_node(cur);
                        /* meaningless to apply lock here */
                        cur = cur_node.next();
                        cur_node.destroy();
                    }
                    ann_helper::optional_destroy(block_set);
                    if (counter <= threshold_counter) {
                        collect_node(index_node.level, false);
                    } else {
                        index_node.idx->destroy();
                        delete index_node.idx;
                    }
                }
            }
        };

        /* iterate over all leaves */
        NodePtr cur_ptr = leaf.ptr();
        bool forward;
        for (;;) {
            if (_extract_leaf(leaf, scan_data, forward, cover_threshold)) {
                set_up_leaf(cur_ptr);
            } else {
                skip_leaf(cur_ptr);
            }
            if (!forward) {
                leaf.r_unlock_destroy();
                break;
            }
            cur_ptr = leaf.next();
            leaf.r_unlock_destroy();
            if (cur_ptr == _ctx.invalid_ptr()) {
                break;
            }
            iter_next_leaf(cur_ptr);
            leaf = _ctx.get_node(cur_ptr);
            leaf.r_lock();
        }
        ann_helper::optional_destroy(valid_leaves);
        end_index();
        ann_helper::optional_destroy(index_nodes);
        ann_helper::optional_destroy(parent_nodes);
        return res;
    }
    scan_results preprocess_base(base_range &U, index_ranges &S)
    {
        scan_results res;
        UnorderedSet<NodePtr> block_set(U.size());
        for (const auto &ls : S) {
            for (const auto &s : ls) {
                for (NodePtr n : s.nodes) {
                    block_set.insert(n);
                }
            }
        }
        base_range new_U;
        for (const auto &n : U) {
            if (!block_set.contains(n)) {
                res.emplace_back(n, leaf_level);
            } else {
                new_U.emplace_back(n);
            }
        }
        ann_helper::optional_destroy(block_set);
        std::swap(new_U, U);
        ann_helper::optional_destroy(new_U);
        return res;
    }

    /* optimal index set calculation: calculate answer C */
    struct CandidateScan {
        scan_results explored;
        base_range U;   /* should be sorted for compare */
        index_ranges S;
        double explored_cost;
        CandidateScan() = default;
        CandidateScan(scan_results &&e, const base_range &u, const index_ranges &s, double c)
            : explored(std::move(e)), U(u), S(s), explored_cost(c) {}
        CandidateScan(const CandidateScan &other)
            : explored(other.explored), U(other.U), S(other.S), explored_cost(other.explored_cost) {}
        CandidateScan(CandidateScan &&other)
            : explored(std::move(other.explored)),
            U(std::move(other.U)),
            S(std::move(other.S)),
            explored_cost(other.explored_cost) {}
        void destroy()
        {
            ann_helper::optional_destroy(explored);
            ann_helper::optional_destroy(U);
            ann_helper::optional_destroy(S);
        }
        ~CandidateScan() {}
        void process_U() { std::sort(U.begin(), U.end()); }
        bool same_base(const CandidateScan &other) const { return U == other.U; }
        bool done_explore() const { return U.empty(); }
        scan_results extract_base(const ContextImpl &ctx) const
        {
            scan_results res;
            for (const auto &u : U) {
                NodeImpl node = ctx.get_node(u);
                node.r_lock();
                OffsetNumber s = node.size();
                for (OffsetNumber i = p_firstdatakey(node); i <= s; i = OffsetNumberNext(i)) {
                    res.emplace_back(node.get_ptr(i), leaf_level);
                }
                node.r_unlock_destroy();
            }
            return res;
        }
    };
    struct ScanState {
        Vector<CandidateScan> candidates;
        double cost_threshold{__DBL_MAX__};
        ScanState(const base_range &U, const index_ranges &S)
        {
            constexpr size_t reserve_size = 20ul;
            candidates.reserve(reserve_size);
            scan_results temp;
            Assert(temp.empty() && temp.size() == 0);
            candidates.emplace_back(std::move(temp), U, S, 0.0);
            Assert(!candidates[0].done_explore());
            Assert(candidates[0].U.size() == U.size());
            Assert(candidates[0].S.size() == S.size());
            Assert(candidates[0].explored.empty());
            candidates[0].process_U();
        }
        void destroy() { ann_helper::optional_destroy(candidates); }
        void emplace(const CandidateScan &old, uint16 level)
        {
            CandidateScan new_scan(old);
            index_range s = new_scan.S[level].front();
            new_scan.explored.emplace_back(s.idx_ptr, level);
            new_scan.explored_cost += _weight(level);
            if (new_scan.explored_cost > cost_threshold) {
                BTVECLOG("Stop: cost threshold %lf explored cost %lf", cost_threshold, new_scan.explored_cost);
                new_scan.destroy();
                return;
            }
            UnorderedSet<BlockNumber> snode(s.nodes.cbegin(), s.nodes.cend());
            new_scan.U.clear();
            for (const auto &u : old.U) {
                if (!snode.contains(u)) {
                    new_scan.U.push_back(u);
                }
            }
            ann_helper::optional_destroy(snode);
            for (const auto &c : candidates) {
                if (new_scan.same_base(c) && c.explored_cost <= new_scan.explored_cost) {
                    new_scan.destroy();
                    BTVECLOG("Stop: same base with lower cost at strategy %lu", &c - &candidates[0]);
                    return;
                }
            }
            _greedy_cover(new_scan.S, s, level);
            s.destroy();
            if (new_scan.explored_cost + max_unexplored_cost(new_scan) < cost_threshold) {
                cost_threshold = new_scan.explored_cost + max_unexplored_cost(new_scan);
                BTVECLOG("Update cost threshold: %lf + %lf = %lf",
                         new_scan.explored_cost, max_unexplored_cost(new_scan), cost_threshold);
            }
            candidates.push_back(std::move(new_scan));
        }
        static double _weight(uint16 level) { return (level + 1) * 2; }
        static double _weight(const ScanResult &c) { return _weight(c.level); }
        static double _weight(const scan_results &C)
        {
            double sum = 0.0;
            for (const auto &c : C) {
                sum += _weight(c);
            }
            return sum;
        }
        static double max_unexplored_cost(const CandidateScan &scan)
        {
            uint16 s = scan.S.size();
            for (uint16 i = 0; i < s; ++i) {
                if (!scan.S[i].empty()) {
                    return _weight(i) * scan.S[i].size();
                }
            }
            return scan.U.size();
        }
        void _greedy_cover(index_ranges &S, const index_range &c, uint16 c_level)
        {
            const auto overlap = [&c](const index_range &ir) -> bool {
                /* compare whether their nodes have overlap, their nodes is sorted already */
                auto it1 = c.nodes.cbegin();
                auto it2 = ir.nodes.cbegin();
                while (it1 != c.nodes.cend() && it2 != ir.nodes.cend()) {
                    if (*it1 < *it2) {
                        ++it1;
                    } else if (*it1 > *it2) {
                        ++it2;
                    } else {
                        return true;
                    }
                }
                return false;
            };
            const auto covered = [&c](const index_range &ir) -> bool {
                /* compare whether c's nodes cover ir's nodes */
                auto it1 = c.nodes.cbegin();
                auto it2 = ir.nodes.cbegin();
                while (it1 != c.nodes.cend() && it2 != ir.nodes.cend()) {
                    if (*it1 > *it2) {
                        return false;
                    } else if (*it1 < *it2) {
                        ++it1;
                    } else {
                        ++it1;
                        ++it2;
                    }
                }
                return it2 == ir.nodes.cend();
            };
            BTVECLOG("Greedy cover before S: %s c: %s", to_str(S), to_str(c));
            for (auto &ls : S) {
                uint16 s_level = &ls - &S[0];
                for (auto it = ls.begin(); it != ls.end();) {
                    if (s_level <= c_level ? covered(*it) : overlap(*it)) {
                        it = ls.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            BTVECLOG("Greedy cover after S: %s", to_str(S));
        }
    };
    scan_results _optimal_cover(base_range &U, index_ranges &S)
    {
        scan_results base = preprocess_base(U, S);
        if (U.empty()) {
            return base;
        }
        for (auto &ls : S) {
            for (auto &s : ls) {
                std::sort(s.nodes.begin(), s.nodes.end());
            }
        }
        ScanState state(U, S);
        for (size_t i = 0; i < state.candidates.size(); ++i) {
            const CandidateScan &c = state.candidates[i];
            if (c.done_explore()) {
                continue;
            }
            for (uint16 level = 0; level < S.size(); ++level) {
                if (c.S[level].empty()) {
                    continue;
                }
                BTVECLOG("greedy exploring strategy %lu(%u)", i, level);
                state.emplace(c, level);
            }
        }
        auto *C = std::min_element(state.candidates.cbegin(), state.candidates.cend(),
            [](const CandidateScan &a, const CandidateScan &b) {
                if (a.done_explore() && !b.done_explore()) {
                    return true;
                } else if (!a.done_explore() && b.done_explore()) {
                    return false;
                }
                return a.explored_cost < b.explored_cost;
            });
        Assert(C != state.candidates.cend());
        scan_results res = std::move(C->explored);
        if (!C->done_explore()) {
            BTVECLOG("No optimal cover found, brute force remaining nodes");
            merge_results(res, C->extract_base(_ctx));
        }
        state.destroy();
        merge_results(base, std::move(res));
        return base;
    }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_SEARCH_H */
