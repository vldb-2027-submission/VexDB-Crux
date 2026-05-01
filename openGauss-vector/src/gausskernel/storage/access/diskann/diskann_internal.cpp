/**
 * Copyright ...
 */

#include <algorithm>    /* max */
#include <chrono>       /* system_clock */
#include <random>

#include <vtl/vector>
#include <vtl/btree>
#include <vtl/triangle_matrix>
#include <vtl/disk_container/freespace.hpp>

#include "miscadmin.h"
#include "access/diskann/diskann_internal.h"
#include "access/diskann/storage_interface/disk_store.h"
#include "access/diskann/storage_interface/mem_store.h"
#include "access/annvector/store/vector_smgr.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/ann_utils.h"
#include "nodes/execnodes.h"
#include "postmaster/bgworker.h"
#include "catalog/pg_partition_fn.h"

using namespace ann_helper;
using namespace disk_container;

PruneContent::PruneContent(const float *q, size_t l, size_t dim, double selectivity)
    : query_alloced(!is_aligned(q)),
      query(query_alloced ? alloc_floatvector(dim) : q),
      l_size(l),
      candidates(l),
      assistances(l / selectivity)
{
    if (query_alloced) {
        errno_t rc = memcpy_s((void *)query, sizeof(float) * dim, q, sizeof(float) * dim);
        securec_check(rc, "\0", "\0");
    }
}

void PruneContent::destroy()
{
    if (query_alloced) {
        free_vector((float *)query);
        query_alloced = false;
    }
    optional_destroy(expanded_nodes);
    optional_destroy(pruned_list);
    candidates.destroy();
    optional_destroy(visited);
#if VERIFY_VISITED
    optional_destroy(verify_visited);
#endif /* VERIFY_VISITED */
}

void CandidateQueue::pop()
{
    auto right_it = _right.crbegin();
    Assert(right_it != _right.crend());
    if (_left.empty()) {
        ++right_it;
        /* should not happen, we assume _capacity is greater than 1 */
        Assert(right_it != _right.crend());
        _max_distance = right_it->distance;
        _right.erase(right_it.base());
        return;
    }
    auto left_it = _left.crbegin();
    
    if (right_it->distance < left_it->distance) {
        ++left_it;
        if (left_it != _left.crend()) {
            _max_distance = std::max(right_it->distance, left_it->distance);
        } else {
            _max_distance = right_it->distance;
        }
        _left.erase(left_it.base());
        return;
    }

    ++right_it;
    if (right_it != _right.crend()) {
        _max_distance = std::max(right_it->distance, left_it->distance);
    } else {
        _max_distance = left_it->distance;
    }
    _right.erase(right_it.base());
}


void CandidateEdgeQueue::pop()
{
    auto right_it = _right.crbegin();
    Assert(right_it != _right.crend());
    if (_left.empty()) {
        ++right_it;
        /* should not happen, we assume _capacity is greater than 1 */
        Assert(right_it != _right.crend());
        _max_distance = right_it->distance;
        _right.erase(right_it.base());
        return;
    }
    auto left_it = _left.crbegin();
    
    if (right_it->distance < left_it->distance) {
        ++left_it;
        if (left_it != _left.crend()) {
            _max_distance = std::max(right_it->distance, left_it->distance);
        } else {
            _max_distance = right_it->distance;
        }
        _left.erase(left_it.base());
        return;
    }

    ++right_it;
    if (right_it != _right.crend()) {
        _max_distance = std::max(right_it->distance, left_it->distance);
    } else {
        _max_distance = left_it->distance;
    }
    _right.erase(right_it.base());
}

bool CandidateQueue::insert(const QueryNeighbor &n)
{
    if (_size >= _capacity) {
        if (n.distance > _max_distance) {
            return false;
        }
        if (n.distance == _max_distance && rand() % 2 == 0) {
            return false;
        }
    }
    _right.insert(n);
    if (_size < _capacity) {
        ++_size;
        _max_distance = std::max(n.distance, _max_distance);
    } else {
        pop();
    }
    return true;
}

bool CandidateEdgeQueue::insert(const QueryEdgeNeighbor &n)
{
    if (_size >= _capacity) {
        if (n.distance > _max_distance) {
            return false;
        }
        if (n.distance == _max_distance && rand() % 2 == 0) {
            return false;
        }
    }
    _right.insert(n);
    if (_size < _capacity) {
        ++_size;
        _max_distance = std::max(n.distance, _max_distance);
    } else {
        pop();
    }
    return true;
}

QueryNeighbor CandidateQueue::pop_unexplored()
{
    Assert(has_unexplored());
    auto it = _right.cbegin();
    auto res = *it;
    _right.erase(it);
    _left.insert(res);
    return res;
}

QueryEdgeNeighbor CandidateEdgeQueue::pop_unexplored()
{
    Assert(has_unexplored());
    auto it = _right.cbegin();
    auto res = *it;
    _right.erase(it);
    _left.insert(res);
    return res;
}

DiskANNIndex::DiskANNIndex(Relation rel, BlockNumber meta_blkno, bool use_pq, bool need_wal)
    : _rel(rel),
      _alpha(DiskAnnGetOcclusionFactor(rel)),
      _max_degree(DiskAnnGetMaxGraphDegree(rel)),
      _list_size(DiskAnnGetBuildListSize(rel)),
      _isWal(need_wal)
{
    load_meta(meta_blkno);
    _use_pq = use_pq && BlockNumberIsValid(_meta.pqCompressedMetaBlkNo);
    if (_use_pq) {
        _storage = NEW DiskStore<true>(rel, _meta, need_wal);
    } else {
        _storage = NEW DiskStore<false>(rel, _meta, need_wal);
    }
}

DiskANNIndex::DiskANNIndex(Relation rel, BlockNumber meta_blkno, large_vector<float> &data,
    large_vector<DiskAnnVamanaNode> &node, large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex)
    : _rel(rel),
      _alpha(DiskAnnGetOcclusionFactor(rel)),
      _max_degree(DiskAnnGetMaxGraphDegree(rel)),
      _list_size(DiskAnnGetBuildListSize(rel))
{
    load_meta(meta_blkno);
    _meta.medoid = 0; /* always 0 for memory build */
    _storage = NEW MemStore(data, node, graph, mutex, _meta.metric, _meta.dimensions, _max_degree);
}

void DiskANNIndex::load_meta(BlockNumber meta_blkno)
{
    BlockMgr block_mgr(_rel);
    PageData meta_pd = block_mgr.get_page_data(meta_blkno);
    /* no need to lock the page since no one write to it */
    _meta = *((DiskAnnMetaPage *) meta_pd.page);
    Assert(_meta.magicNumber == DISKANN_MAGIC_NUMBER);
    Assert(_meta.version == DISKANN_VERSION_ONE);
    block_mgr.release_page(meta_pd);
    block_mgr.destroy();
}

void DiskANNIndex::destroy()
{
    _storage->perf_report();
    _storage->destroy();
    delete _storage;
}

Vector<size_t> DiskANNIndex::get_init_ids() { return Vector<size_t>(1ul, _meta.medoid); }

template <bool search_invocation, class F>
void DiskANNIndex::iterate_to_fixed_point(PruneContent &content, const Vector<size_t> &init_ids, F &&filter)
{
    static_assert(search_invocation || IS_INVOCABLE_R(F, bool, size_t),
                  "F must be invocable with size_t and return bool");
    /* Lambda to determine if a node has been visited */
    const auto is_not_visited = [&content](const size_t id) {
        bool res = !content.visited.contains(id);
#if VERIFY_VISITED
        Assert(res == !content.verify_visited.get(id));
#endif /* VERIFY_VISITED */
        return res;
    };
    const auto set_visited = [&content](const size_t id) {
        content.visited.insert(id);
#if VERIFY_VISITED
        content.verify_visited.set(id, true);
#endif /* VERIFY_VISITED */
    };

    /* we can use whether the function is constant but __builtin_constant_p seems not to work (at least in debug mode) */
    /* std::is_same<std::remove_reference<std::remove_cv<F>>, DummyFilter> returns false for dummy_filter :( */
    constexpr bool filtered_search = search_invocation && !std::is_convertible_v<F, DummyFilter>;
    _storage->perf_iterate();
    _storage->preprocess(content.query);
    for (const auto id : init_ids) {
        if (is_not_visited(id)) {
            set_visited(id);
            float distance = _storage->get_distance(id, content.query);
            content.candidates.emplace(id, distance);
        }
    }

    Vector<size_t> ids(_max_degree);
    float *dists = (float *)palloc(MAX_ANN_GRAPH_DEGREE * sizeof(float));
    while (content.candidates.has_unexplored() ||
           (filtered_search && content.assistances.has_unexplored() && !content.candidates.full())) {
        CHECK_FOR_INTERRUPTS();
        auto nbr = (!filtered_search || !content.assistances.has_unexplored() ||
            (content.candidates.has_unexplored() &&
             content.candidates.top_dist() <= content.assistances.top_dist())) ?
            content.candidates.pop_unexplored() : content.assistances.pop_unexplored();
        /* Add node to expanded nodes to create pool for prune later */
        if (!search_invocation && filter(nbr.id)) {
            content.expanded_nodes.emplace_back(nbr);
        }

        /* Find which of the nodes in des have not been visited before */
        ids.clear();
        auto neighbors = _storage->get_neighbors(nbr.id);
        for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
            const auto id = neighbors.neighbors[i];
            if (!is_not_visited(id)) {
                continue;
            }
            set_visited(id);
            ids.push_back(id);
        }
        _storage->get_distance(ids, content.query, dists);
        const size_t s = ids.size();
        for (size_t m = 0; m < s; ++m) {
            if (filtered_search && !filter(ids[m])) {
                content.assistances.emplace(ids[m], dists[m]);
            } else {
                content.candidates.emplace(ids[m], dists[m]);
            }
        }
    }
    pfree(dists);
    optional_destroy(ids);
    _storage->postprocess();
    _storage->perf_stop();
}

void DiskANNIndex::occlude_list(const size_t location, Vector<QueryNeighbor> &pool,
                                Vector<QueryNeighbor> &result)
{
    Assert(result.empty());
    if (unlikely(pool.empty())) {
        return;
    }
    if (pool.size() > max_candidate_size) {
        pool.resize(max_candidate_size);
    }

    const size_t pool_size = pool.size();
    TriangleMatrix<float> djks(pool_size, pool_size, FLT_MAX);
    size_t loaded_idx = pool_size;
    float *vec_buffer = alloc_floatvector(dim());
    const auto get_distance = [&](const size_t i, const size_t j) {
        float &dist = djks.get(i, j);
        if (dist != FLT_MAX) {
            return dist;
        }
        if (i != loaded_idx) {
            loaded_idx = i;
            _storage->get_vector(pool[i].id, vec_buffer);
        }
        dist = _storage->get_distance(pool[j].id, vec_buffer);
        return dist;
    };

    float *occlude_factor = (float *)palloc0(pool_size * sizeof(float));
    size_t result_size = 0;
    const auto do_occlude = [&](auto &&f) {
        /* used for MIPS, where we store a value of eps in alpha to
           denote pruned out entries which we can skip in later rounds. */ 
        float cur_alpha = 1.0;
        while (cur_alpha <= _alpha && result_size < _max_degree) {
            const float eps = cur_alpha + 0.01f;
            for (size_t i = 0; result_size < _max_degree && i < pool_size; ++i) {
                if (occlude_factor[i] > cur_alpha) {
                    continue;
                }
                /* Set the entry to float::max so that is not considered again */
                occlude_factor[i] = FLT_MAX;
                /* Add the entry to the result if its not been deleted, and doesn't add a self loop */
                if (pool[i].id != location) {
                    result.push_back(pool[i]);
                    ++result_size;
                }

                for (size_t j = i + 1ul; j < pool_size; ++j) {
                    if (occlude_factor[j] > _alpha) {
                        continue;
                    }
                    const auto djk = get_distance(i, j);
                    f(occlude_factor[j], djk, eps, cur_alpha, pool[j].distance);
                }
            }
            cur_alpha *= _alpha;
        }
    };

    switch (_meta.metric) { /* TD: verify distance calculation */
        case Metric::L2:
        case Metric::COSINE:
        case Metric::FAST_BLAS_COSINE:
        case Metric::FAST_COSINE:
            do_occlude([](float &o, float djk, float, float, float pdist) {
                o = (djk == 0) ? FLT_MAX : std::max(o, pdist / djk);
            });
            break;
        case Metric::INNER_PRODUCT:
        case Metric::FAST_BLAS_INNER_PRODUCT:
            /* Improvization for flipping max and min dist for MIPS */
            do_occlude([](float &o, float djk, float eps, float alpha, float pdist) {
                if (djk < alpha * pdist) {
                    o = std::max(o, eps);
                }
            });
            break;
        default:
            break;  /* make compiler happy */
    }

    free_vector(vec_buffer);
    pfree(occlude_factor);
    djks.destroy();
    std::sort(result.begin(), result.end(), QueryNeighbor::compare_id);
}

void DiskANNIndex::prune_neighbors(const size_t location, PruneContent &content)
{
    if (content.expanded_nodes.empty()) {
        return;
    }
    std::sort(content.expanded_nodes.begin(), content.expanded_nodes.end(), QueryNeighbor::compare_distance);
    content.pruned_list.reserve(_max_degree);
    occlude_list(location, content.expanded_nodes, content.pruned_list);
    Assert(content.pruned_list.size() <= _max_degree);
}

void DiskANNIndex::search_for_point_and_prune(PruneContent &content, size_t location)
{
    _storage->perf_prune();
    auto init_ids = get_init_ids();
    float *vec = _storage->get_vector(location);
    const float *tmp = content.query;
    content.query = vec;
    if (_is_building) {
        iterate_to_fixed_point<false>(content, init_ids, dummy_filter);
    } else {
        iterate_to_fixed_point<false>(content, init_ids, [this](size_t loc) {
            return diskann_node_flag::is_existing(_storage->get_node_data(loc).flag);
        });
    }
    vec = const_cast<float *>(content.query);
    content.query = tmp;
    free_vector(vec);
    optional_destroy(init_ids);

#if !CONTAINER_USE_STL_VECTOR
    content.expanded_nodes.erase_if([location](const QueryNeighbor &ngh) {
        return ngh.id == location;
    });
#else
    std::erase_if(content.expanded_nodes, [location](const QueryNeighbor &ngh) {
        return ngh.id == location;
    });
#endif /* CONTAINER_USE_STL_VECTOR */
    Assert(content.pruned_list.empty());
    prune_neighbors(location, content);
    _storage->perf_stop();
}

size_t DiskANNIndex::reserve_location(const float *point, const DiskAnnVamanaNode &node)
{
    FreeSpace<size_t> free_space(_rel, _meta.freespaceMetaBlkNo);
    size_t res;
    bool success = free_space.pop(res);
    free_space.destroy();
    if (success) {
        return res;
    }

    res = _storage->push_back_new_node(point, node);
    if (unlikely(res == 0)) {
        auto flag = diskann_node_flag::init_flag;
        diskann_node_flag::set_frozen(flag);
        _storage->set_node_data(0, {{0, 0}, disk_container::PlainStore::invalid_key(), flag});
        return _storage->push_back_new_node(point, node);
    }

    constexpr size_t safe_guard_size = 64;
    if (res < safe_guard_size) {
        auto flag = _storage->get_node_data(0).flag;
        while (!diskann_node_flag::is_frozen(flag)) {
            pg_usleep(1);
            flag = _storage->get_node_data(0).flag;
        }
    }
    return res;
}

void DiskANNIndex::inter_insert(size_t n, PruneContent &content)
{
    /* avoid deadlock */
    Assert(std::is_sorted(content.pruned_list.cbegin(), content.pruned_list.cend(), QueryNeighbor::compare_id));
    /* des_pool contains the neighbors of the neighbors of n */
    Vector<size_t> des_pool;
    for (auto des : content.pruned_list) {
optimize_retry:
        CHECK_FOR_INTERRUPTS();
        des_pool.clear();
        auto neighbors = _storage->get_neighbors(des.id);
        des_pool.reserve(neighbors.num_neighbors + 1u);
        for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
            des_pool.push_back(neighbors.neighbors[i]);
        }

        bool prune_needed = false;
        if (std::find(des_pool.begin(), des_pool.end(), n) == des_pool.end()) {
            if (des_pool.size() < _max_degree) {
                if (!_storage->add_neighbor(des.id, n)) {
                    _storage->perf_conflict();
                    goto optimize_retry;
                }
            } else {
                des_pool.push_back(n);
                prune_needed = true;
            }
        }

        if (!prune_needed) {
            continue;
        }

        UnorderedSet<size_t> dummy_visited(_max_degree);
        Vector<QueryNeighbor> dummy_pool(_max_degree);
        float *des_vec = _storage->get_vector(des.id);
        for (const auto &cur_nbr : des_pool) {
            if (cur_nbr != des.id && !dummy_visited.contains(cur_nbr)) {
                float dist = _storage->get_distance(cur_nbr, des_vec);
                dummy_pool.emplace_back(cur_nbr, dist);
                dummy_visited.insert(cur_nbr);
            }
        }
        free_vector(des_vec);
        optional_destroy(dummy_visited);
        Vector<QueryNeighbor> new_out_neighbors;
        std::swap(content.pruned_list, new_out_neighbors);
        std::swap(content.expanded_nodes, dummy_pool);
        prune_neighbors(des.id, content);
        std::swap(content.pruned_list, new_out_neighbors);
        std::swap(content.expanded_nodes, dummy_pool);
        optional_destroy(dummy_pool);

        Assert(std::is_sorted(new_out_neighbors.cbegin(), new_out_neighbors.cend(), QueryNeighbor::compare_id));
        neighbors.num_neighbors = new_out_neighbors.size();
        for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
            neighbors.neighbors[i] = new_out_neighbors[i].id;
        }
        optional_destroy(new_out_neighbors);
        if (!_storage->set_neighbors(des.id, neighbors)) {
            _storage->perf_conflict();
            goto optimize_retry;
        }
    }
    optional_destroy(des_pool);
}

void DiskANNIndex::insert_point_to(const float *point, const size_t location)
{
    PruneContent content(point, _list_size, dim());
    search_for_point_and_prune(content, location);

    AnnNeighbors neighbors;
    neighbors.num_neighbors = content.pruned_list.size();
    for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
        neighbors.neighbors[i] = content.pruned_list[i].id;
    }
    _storage->set_neighbors(location, neighbors);
    inter_insert(location, content);
#if DISKANN_PRINT_NEIGHBORS_PER_POINT
    _storage->print(location);
#endif
    content.destroy();
}

void DiskANNIndex::insert_point(float *point, const ItemPointerData &tid, Datum *values, const bool *isnull)
{
    auto preprocessor = get_vector_preprocess_func(metric());
    if (preprocessor) {
        preprocessor(point, dim(), point);
    }
    DiskAnnVamanaNode node(tid, PlainStore::invalid_key(), diskann_node_flag::init_flag);
    if (BlockNumberIsValid(_meta.attrMetaBlkNo)) {
        auto tuple = index_form_tuple(RelationGetDescr(_rel), values, isnull);
        PlainStore store(_rel, _meta.attrMetaBlkNo, true);
        node.attr_ptr = store.put(tuple, IndexTupleSize(tuple));
        store.destroy();
    }
    size_t loc = reserve_location(point, node);
    Assert(loc != 0);
    insert_point_to(point, loc);
}

void DiskANNIndex::insert_point_to(size_t location)
{
    VecBuffer vec_buf = vec_read_buffer(_rel, location, dim() * sizeof(float));
    float *point = (float *)vec_buf.get_vecbuf();
    insert_point_to(point, location);
    vec_buf.release();
}

struct TidDist {
    ItemPointerData tid;
    float dist;
    TidDist(const ItemPointerData &t, float d) : tid(t), dist(d) {}
    static bool compare(const TidDist &lhs, const TidDist &rhs) { return lhs.dist < rhs.dist; }
};

size_t DiskANNIndex::search(const float *query, const size_t K, const size_t L,
                            ItemPointerData *tids, float *dists_out,
                            uint32 num_scan_key, ScanKey scanKey)
{
    Assert(K <= L);
    PruneContent content(query, L, dim());
    auto init_ids = get_init_ids();
    if (num_scan_key == 0) {
        iterate_to_fixed_point<true>(content, init_ids, dummy_filter);
    } else {
        PlainStore store(_rel, _meta.attrMetaBlkNo, false);
        iterate_to_fixed_point<true>(content, init_ids, [this, &store, num_scan_key, scanKey](size_t loc) {
            for (uint32 i = 0; i < num_scan_key; ++i) {
                auto node = _storage->get_node_data(loc);
                if (diskann_node_flag::is_frozen(node.flag)) {
                    return true;
                }
                bool result = false;
                store.get(node.attr_ptr, [this, scanKey, i, &result](const void *data, uint32 size) -> void {
                    bool is_null;
                    IndexTuple itup = (IndexTuple)data;
                    Datum v = index_getattr(itup, scanKey[i].sk_attno, RelationGetDescr(_rel), &is_null);
                    result = !is_null &&
                             DatumGetBool(FunctionCall2Coll(&scanKey[i].sk_func, scanKey[i].sk_collation,
                                                            v, scanKey[i].sk_argument));
                });
                if (!result) {
                    return false;
                }
            }
            return true;
        });
        store.destroy();
    }
    optional_destroy(init_ids);
    ItemPointerData *cur_tid = tids;
    if (_use_pq) {
        _storage->postprocess();
        Vector<TidDist> tid_dist(K);
        for (const auto &ngh : content.candidates) {
            const auto node = _storage->get_node_data(ngh.id);
            if (!diskann_node_flag::visible(node.flag)) {
                continue;
            }
            tid_dist.emplace_back(node.heapTid, _storage->get_distance(ngh.id, content.query));
        }
        std::sort(tid_dist.begin(), tid_dist.end(), TidDist::compare);
        for (const auto &td : tid_dist) {
            *cur_tid++ = td.tid;
        }
        optional_destroy(tid_dist);
    } else {
        float *cur_dist = dists_out;
        for (const auto &ngh : content.candidates) {
            const auto node = _storage->get_node_data(ngh.id);
            if (!diskann_node_flag::visible(node.flag)) {
                continue;
            }
            *cur_tid++ = node.heapTid;
            if (cur_dist != nullptr) {
                *cur_dist++ = ngh.distance;
            }
        }
    }
    content.destroy();
    return cur_tid - tids;
}

template <class F1, class F2>
void DiskANNIndex::get_deleted_point_idx(IndexBulkDeleteCallback callback, void *callback_state,
                                         IdxSet &delete_set, size_t total_size, F1 &&get_node, F2 &&set_node)
{
    for (size_t idx = 0; idx < total_size; ++idx) {
        DiskAnnVamanaNode node = get_node(idx);
        if (!diskann_node_flag::is_frozen(node.flag) &&
            /* partition oid is always zero since it's merely used for global indexes (cross partitions) */
            (!diskann_node_flag::is_existing(node.flag) || callback(&node.heapTid, callback_state, InvalidOid, InvalidBktId))) {
            delete_set.insert(idx);
            diskann_node_flag::unset_existing(node.flag);
            set_node(idx, node);
        }
    }
}

void DiskANNIndex::get_deleted_point_idx(Relation rel, BlockNumber node_blkno, IndexBulkDeleteCallback callback,
                                         void *callback_state, IdxSet &delete_set)
{
    DiskVector<DiskAnnVamanaNode> nodes(rel, node_blkno, true);
    get_deleted_point_idx(callback, callback_state, delete_set, nodes.size(),
        [&](size_t idx) {
            return nodes.get<AccessorLockType::NoLockRead>(idx);
        },
        [&](size_t idx, const DiskAnnVamanaNode &node) {
            nodes.set<AccessorLockType::NoLockWrite>(idx, node);
        }
    );
    nodes.destroy();
}

void DiskANNIndex::get_deleted_point_idx(IndexBulkDeleteCallback callback, void *callback_state,
                                         IdxSet &delete_set, size_t &total_size)
{
    total_size = _storage->size();
    get_deleted_point_idx(callback, callback_state, delete_set, total_size,
        [this](size_t idx) {
            return _storage->get_node_data(idx);
        },
        [this](size_t idx, const DiskAnnVamanaNode &node) {
            _storage->set_node_data(idx, node);
        }
    );
}

void DiskANNIndex::consolidate_all_points(size_t upto, IdxSet &delete_set)
{
    for (size_t i = 0; i < upto; ++i) {
        /* don't deal with nodes that are already deleted */
        if (delete_set.contains(i)) {
            continue;
        }
        CHECK_FOR_INTERRUPTS();
        process_deleted_neighbors(i, [&delete_set](size_t j){
            return delete_set.contains(j);
        });
    }
}

void DiskANNIndex::consolidate_all_points(IdxSet &delete_set, large_vector<size_t> &valid_ids)
{
    size_t size = valid_ids.size();
    const size_t report_step_size = size > report_threshold ? 10'000ul : 1'000ul;
    Timer timer(size, report_step_size);
    for (size_t i = 0; i < size; ++i) {
        CHECK_FOR_INTERRUPTS();
        process_deleted_neighbors(valid_ids[i], [&delete_set](size_t j){
            return delete_set.contains(j);
        });
        report_progress_vacuum(i, timer);
    }
    timer.destroy();
}

void DiskANNIndex::collect_valid_points(IdxSet &delete_set, large_vector<size_t> &valid_ids,
                                        const size_t default_capacity)
{
    const size_t medoid = _meta.medoid;
    UnorderedSet<size_t> visited{uint32(default_capacity)};
    Vector<size_t> candidates(default_capacity);

    candidates.push_back(medoid);
    while (!candidates.empty()) {
        size_t id = candidates.back();
        candidates.pop_back();
        /* don't deal with nodes that are already deleted */
        if (!delete_set.contains(id)) {
            valid_ids.push_back(id);
        }
        auto neighbors = _storage->get_neighbors(id);
        for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
            size_t nid = size_t(neighbors.neighbors[i]);
            if (visited.contains(nid)) {
                continue;
            }
            visited.insert(nid);
            candidates.push_back(nid);
        }
    }

#if !CONTAINER_USE_STL_VECTOR
    valid_ids.erase_if([medoid](const size_t &id) {
        return id == medoid;
    });
#else
    std::erase_if(valid_ids, [medoid](const size_t &id) {
        return id == medoid;
    });
#endif /* CONTAINER_USE_STL_VECTOR */

    if (valid_ids.size() > 0) {
        std::sort(valid_ids.begin(), valid_ids.end());
        valid_ids.push_back(medoid); /* process the medoid at the end */
    }

    optional_destroy(visited);
    optional_destroy(candidates);
}

template <class F>
void DiskANNIndex::process_deleted_neighbors(size_t loc, F &&delete_filter)
{
    static_assert(IS_INVOCABLE_R(F, bool, size_t), "F must be invocable with size_t and return bool");
    auto neighbors = _storage->get_neighbors(loc);
    UnorderedSet<size_t> neighbor_to_probe(neighbors.num_neighbors);
    bool has_deleted_neighbor = false;
    for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
        size_t neighbor_loc = size_t(neighbors.neighbors[i]);
        if (!delete_filter(neighbor_loc)) {
            neighbor_to_probe.insert(neighbor_loc);
            continue;
        }
        has_deleted_neighbor = true;
        /* add its neighbors' neighbors */
        auto neighbor_neighobors = _storage->get_neighbors(neighbor_loc);
        for (uint32 j = 0; j < neighbor_neighobors.num_neighbors; ++j) {
            size_t neighbor_neighbor_loc = size_t(neighbor_neighobors.neighbors[j]);
            if (neighbor_neighbor_loc != loc && !delete_filter(neighbor_neighbor_loc)) {
                neighbor_to_probe.insert(neighbor_neighbor_loc);
            }
        }
    }
    if (!has_deleted_neighbor) {
        optional_destroy(neighbor_to_probe);
        return;
    }

    float *src_vec = _storage->get_vector(loc);
    if (neighbor_to_probe.size() < _max_degree) {
        insert_point_to(src_vec, loc);
    } else {
        Vector<QueryNeighbor> neighbor_to_probe_vec(neighbor_to_probe.size());
        for (auto n : neighbor_to_probe) {
            neighbor_to_probe_vec.emplace_back(n, _storage->get_distance(n, src_vec));
        }
        std::sort(neighbor_to_probe_vec.begin(), neighbor_to_probe_vec.end());
        Vector<QueryNeighbor> final_neighbor(_max_degree);
        occlude_list(loc, neighbor_to_probe_vec, final_neighbor);
        Assert(final_neighbor.size() <= _max_degree);
        AnnNeighbors neighbors;
        neighbors.num_neighbors = final_neighbor.size();
        for (uint32 i = 0; i < neighbors.num_neighbors; ++i) {
            neighbors.neighbors[i] = final_neighbor[i].id;
        }
        _storage->set_neighbors(loc, neighbors);
        optional_destroy(neighbor_to_probe_vec);
        optional_destroy(final_neighbor);
    }
    free_vector(src_vec);
    optional_destroy(neighbor_to_probe);
}

template <class F>
typename DiskANNIndex::VacuumReport DiskANNIndex::retrieve_deleted_slots(Relation rel, size_t upto,
    IdxSet &delete_set, BlockNumber freespace_blkno, F &&set_node)
{
    VacuumReport res;
    constexpr size_t counter_step = 2'000ul;
    size_t *deleted_idx = (size_t *)palloc(sizeof(size_t) * counter_step);
    DiskAnnVamanaNode node;
    diskann_node_flag::unset_valid(node.flag);

    Buffer buf = ReadBuffer(rel, DISKANN_METAPAGE_BLKNO);
    auto *base = reinterpret_cast<DiskAnnMetaPageBase *>(PageGetContents(BufferGetPage(buf)));
    /* no need to lock it, we don't expect dim to change */
    const uint32 d = base->dimensions;
    ReleaseBuffer(buf);

    FreeSpace<size_t> free_space(rel, freespace_blkno);
    auto it = delete_set.cbegin();
    size_t counter = 0;
    while (it != delete_set.cend()) {
        counter = 0;
        for (; it != delete_set.cend(); ++it) {
            size_t idx = *it;
            set_node(idx, node);
            deleted_idx[counter] = idx;
            ++counter;
            if (counter >= counter_step) {
                LogManager logmgr(rel);
                for (size_t i = 0; i < counter; ++i) {
                    vec_invalidate_buffer_cache(rel->rd_smgr->smgr_rnode.node.relNode, deleted_idx[i], d * sizeof(float));
                    logmgr.log_invalidate_vector_cache(rel, deleted_idx[i], d, RM_DISKANN_ID, XLOG_DISKANN_INVALIDATE_VECTOR_CACHE);
                }
                logmgr.destroy();
                free_space.insert(deleted_idx, counter);
                res.num_point_deleted += counter;
                break;
            }
        }
    }
    if (counter > 0) {
        LogManager logmgr(rel);
        for (size_t i = 0; i < counter; ++i) {
            vec_invalidate_buffer_cache(rel->rd_smgr->smgr_rnode.node.relNode, deleted_idx[i], d * sizeof(float));
            logmgr.log_invalidate_vector_cache(rel, deleted_idx[i], d, RM_DISKANN_ID, XLOG_DISKANN_INVALIDATE_VECTOR_CACHE);
        }
        logmgr.destroy();
        free_space.insert(deleted_idx, counter);
        res.num_point_deleted += counter;
    }
    pfree(deleted_idx);
    free_space.destroy();
    res.num_point_remained = upto - res.num_point_deleted;
    res.num_page_remained = RelationGetNumberOfBlocks(rel);
    return res;
}

void DiskANNIndex::retrieve_deleted_slots(Relation rel, BlockNumber node_blkno,
    BlockNumber freespace_blkno, IdxSet &delete_set)
{
    DiskVector<DiskAnnVamanaNode> nodes(rel, node_blkno, true);
    retrieve_deleted_slots(rel, nodes.size(), delete_set, freespace_blkno,
                           [&](size_t idx, DiskAnnVamanaNode &node) {
        nodes.set<AccessorLockType::NoLockWrite>(idx, node);
    });
    nodes.destroy();
}

DiskANNIndex::VacuumReport DiskANNIndex::retrieve_deleted_slots(size_t upto, IdxSet &delete_set)
{
    return retrieve_deleted_slots(_rel, upto, delete_set, _meta.freespaceMetaBlkNo,
                                  [this](size_t idx, DiskAnnVamanaNode &node) {
        _storage->set_node_data(idx, node);
    });
}

size_t DiskANNIndex::calculate_entry_point(large_vector<float> &data, Metric metric, uint32 dim)
{
    Timer timer;
    timer.report("  Calculate entry point start");

    float *center = alloc_floatvector(dim);
    double *temp_center = (double *)palloc0(sizeof(double) * dim);
    size_t size = data.size() / dim - 1ul;
    for (size_t i = 0; i < size; ++i) {
        for (size_t j = 0; j < dim; ++j) {
            temp_center[j] += data[(i + 1ul) * dim + j];
        }
    }
    for (size_t j = 0; j < dim; ++j) {
        center[j] = temp_center[j] / size;
    }
    pfree(temp_center);

    /* compute all to one distance */
    const auto func = dim % vector_step_size == 0 ?
        get_aligned_distance_batch_func(metric, dim) :
        get_general_distance_batch_func(metric, dim);
    float *distances = (float *)palloc(sizeof(float) * size);
    constexpr size_t block_size = 1'000ul;
    for (size_t i = 0; i < size; i += block_size) {
        size_t block = std::min(block_size, size - i);
        func(center, data.data() + (i + 1ul) * dim, dim, block, distances + i);
    }
    free_vector(center);

    /* find imin */
    size_t min_idx = 0;
    float min_dist = distances[0];
    for (size_t i = 1ul; i < size; ++i) {
        if (distances[i] < min_dist) {
            min_idx = i;
            min_dist = distances[i];
        }
    }
    pfree(distances);

    timer.report("  Calculate entry point done");
    timer.destroy();
    return min_idx;
}

size_t DiskANNIndex::calculate_entry_point(size_t size)
{
    Timer timer;
    timer.report("  Calculate entry point start");

    constexpr size_t block_size = 1024ul;
    float *buf = alloc_floatvector(sizeof(float) * dim() * block_size);
    float *center = alloc_floatvector(dim());

    double *temp_center = (double *)palloc0(sizeof(double) * dim());
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        vec_read(_rel->rd_smgr, (i + 1ul) * dim() * sizeof(float), block * dim() * sizeof(float), (char *)buf);
        for (size_t j = 0; j < block; ++j) {
            for (size_t k = 0; k < dim(); ++k) {
                temp_center[k] += buf[j * dim() + k];
            }
        }
    }
    for (size_t j = 0; j < dim(); ++j) {
        center[j] = temp_center[j] / size;
    }
    pfree(temp_center);

    /* compute all to one distance */
    const auto func = dim() % vector_step_size == 0 ?
        get_aligned_distance_batch_func(metric(), dim()) :
        get_general_distance_batch_func(metric(), dim());
    float *distances = (float *)palloc(sizeof(float) * size);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        vec_read(_rel->rd_smgr, (i + 1ul) * dim() * sizeof(float), block * dim() * sizeof(float), (char *)buf);
        func(center, buf, dim(), block, distances + i);
    }
    free_vector(buf);
    free_vector(center);

    /* find imin */
    size_t min_idx = 0;
    float min_dist = distances[0];
    for (size_t i = 1ul; i < size; ++i) {
        if (distances[i] < min_dist) {
            min_idx = i;
            min_dist = distances[i];
        }
    }
    pfree(distances);
    timer.report("  Calculate entry point done");
    timer.destroy();
    return min_idx;
}

size_t DiskANNIndex::calculate_entry_point(tmpvector<float> &data, Metric metric, uint32 dim)
{
    Timer timer;
    timer.report("  Calculate entry point start");

    size_t size = data.size() / dim;
    constexpr size_t block_size = 1024ul;
    float *buf = alloc_floatvector(sizeof(float) * dim * block_size);
    float *center = alloc_floatvector(dim);

    double *temp_center = (double *)palloc0(sizeof(double) * dim);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        data.get_n(i * dim, block * dim, buf);
        for (size_t j = 0; j < block; ++j) {
            for (size_t k = 0; k < dim; ++k) {
                temp_center[k] += buf[j * dim + k];
            }
        }
    }
    for (size_t j = 0; j < dim; ++j) {
        center[j] = temp_center[j] / size;
    }
    pfree(temp_center);

    /* compute all to one distance */
    const auto func = dim % vector_step_size == 0 ?
        get_aligned_distance_batch_func(metric, dim) :
        get_general_distance_batch_func(metric, dim);
    float *distances = (float *)palloc(sizeof(float) * size);
    for (size_t i = 0; i < size; i += block_size) {
        CHECK_FOR_INTERRUPTS();
        size_t block = std::min(block_size, size - i);
        data.get_n(i * dim, block * dim, buf);
        func(center, buf, dim, block, distances + i);
    }

    free_vector(buf);
    free_vector(center);

    /* find imin */
    size_t min_idx = 0;
    float min_dist = distances[0];
    for (size_t i = 1ul; i < size; ++i) {
        if (distances[i] < min_dist) {
            min_idx = i;
            min_dist = distances[i];
        }
    }
    pfree(distances);
    timer.report("  Calculate entry point done");
    timer.destroy();
    return min_idx;
}

/* Fisher-Yates shuffle */
static void generate_sample_data(Relation rel, vector_pair_vector &data, uint32 dim, size_t sample_size,
                                 Vector<size_t> &sample_vids, Vector<float> &sample_data) {
    sample_data.resize(sample_size * dim);
    size_t total_size = data.size();
    for (size_t i = 0; i < sample_size; ++i) {
        size_t picked_idx = random() % total_size;
        read_vector(rel, data[picked_idx].vid, dim, (char *)(sample_data.data() + i * dim));
        std::swap(data[picked_idx], data[--total_size]);
        sample_vids.push_back(data[total_size].vid);
    }
}

size_t DiskANNIndex::calculate_entry_point(Relation rel, vector_pair_vector &data, Metric metric, uint32 dim)
{
    Timer timer;
    timer.report("  Calculate entry point start");

    constexpr size_t max_sample_size = 131'072ul;
    constexpr size_t block_size = 1'024ul;
    size_t sample_size = std::min(max_sample_size, data.size());
    float *center = alloc_floatvector(dim);
    double *temp_center = (double *)palloc0(sizeof(double) * dim);

    Vector<size_t> sample_vids;
    Vector<float> sample_data;

    /* we have to use sample data to calculate since the data may not be contiguous */
    generate_sample_data(rel, data, dim, sample_size, sample_vids, sample_data);
    for (size_t i = 0; i < sample_size; ++i) {
        for (size_t j = 0; j < dim; ++j) {
            temp_center[j] += sample_data[i * dim + j];
        }
    }
    for (size_t j = 0; j < dim; ++j) {
        center[j] = temp_center[j] / sample_size;
    }
    pfree(temp_center);

    /* compute all to one distance */
    const auto func = dim % vector_step_size == 0 ?
        get_aligned_distance_batch_func(metric, dim) :
        get_general_distance_batch_func(metric, dim);
    float *distances = (float *)palloc(sizeof(float) * sample_size);
    for (size_t i = 0; i < sample_size; i += block_size) {
        size_t block = std::min(block_size, sample_size - i);
        func(center, sample_data.data() + i * dim, dim, block, distances + i);
    }

    free_vector(center);
    optional_destroy(sample_data);

    /* find imin */
    size_t min_idx = 0;
    float min_dist = distances[0];
    for (size_t i = 1ul; i < sample_size; ++i) {
        if (distances[i] < min_dist) {
            min_idx = i;
            min_dist = distances[i];
        }
    }
    pfree(distances);
    size_t res = sample_vids[min_idx];
    optional_destroy(sample_vids);
    timer.report("  Calculate entry point done");
    timer.destroy();
    return res;
}

void DiskANNIndex::init_disk_storage(size_t size)
{
    size_t medoid = calculate_entry_point(size);
    Timer timer(size);

    if (BlockNumberIsValid(_meta.pqCompressedMetaBlkNo)) {
        DiskVector<uint8> pq(_rel, _meta.pqCompressedMetaBlkNo, false);
        uint8 *medoid_pq = (uint8 *)palloc(_meta.numPQChunks * sizeof(uint8));
        pq.template get_n<AccessorLockType::NoLockRead>((medoid + 1ul) * _meta.numPQChunks,
                                                        _meta.numPQChunks, medoid_pq);
        pq.template set_n<AccessorLockType::NoLockWrite>(0, _meta.numPQChunks, medoid_pq);
        pfree(medoid_pq);
        pq.destroy();
    }

    float *vec_buf = (float *)palloc(sizeof(float) * dim());
    timer.report("  Start setting entry point for vectors");
    read_vector(_rel, medoid + 1ul, dim(), (char *)vec_buf);
    write_vector(_rel, 0, dim() * sizeof(float), (char *)vec_buf);
    pfree(vec_buf);

    timer.report("  Start setting entry point for nodes");
    DiskVector<DiskAnnVamanaNode> nodes(_rel, _meta.nodeMetaBlkNo, false);
    DiskAnnVamanaNode tmp_node;
    tmp_node.heapTid = nodes.get<AccessorLockType::NoLockRead>(medoid + 1ul).heapTid;
    tmp_node.flag = diskann_node_flag::init_flag;
    diskann_node_flag::set_frozen(tmp_node.flag);
    nodes.set<AccessorLockType::NoLockWrite>(0, tmp_node);
    nodes.destroy();

    timer.report("  Start expanding disk space for graph");
    DiskVector<AnnNeighbors> neighbors(_rel, _meta.graphMetaBlkNo, false);
    ++size;
    AnnNeighbors ngh = {0, 0, };
    neighbors.push_back_n(ngh, size);
    neighbors.destroy();

    timer.report("  Disk storage initialization done");
    timer.destroy();
}

struct DiskANNBuildContext : public BaseObject {
    Oid base_relid;
    Oid part_id;
    void *build_params;
    bool use_btree;
    std::atomic<size_t> counter;
    size_t size;
    slock_t lock;
    PerfUsage &perf;
    bool _isWal;
    Timer *_timer;
    DiskANNBuildContext(Relation rel, void *build_params, size_t start,
                        size_t size, PerfUsage &usage, bool isWal, Timer *timer)
        : build_params(build_params),
          counter(start),
          size(size),
          perf(usage),
          _isWal(isWal),
          _timer(timer)
    {
        if (RelationIsPartition(rel)) {
            base_relid = GetBaseRelOidOfParition(rel);
            part_id = RelationGetRelid(rel);
        } else {
            base_relid = RelationGetRelid(rel);
            part_id = InvalidOid;
        }
        use_btree = DiskAnnUseBTree(rel);
        SpinLockInit(&lock);
    }
};

static void build_cleanup(const BgWorkerContext *bwc)
{
    DiskANNBuildContext *context = (DiskANNBuildContext *)bwc->bgshared;
    SpinLockFree(&context->lock);
}

static void build_parallel(const BgWorkerContext *bwc)
{
    DiskANNBuildContext *context = (DiskANNBuildContext *)bwc->bgshared;
    Relation target;
    Relation parent;
    Partition part;
    if (context->part_id == InvalidOid) {
        target = index_open(context->base_relid, NoLock);
    } else {
        parent = index_open(context->base_relid, NoLock);
        part = partitionOpen(parent, context->part_id, NoLock);
        target = partitionGetRelation(parent, part);
    }
    RelationOpenSmgr(target);

    const auto build_index = [&](DiskANNIndex &&index, auto &&f) {
        index.set_building();
        for (;;) {

            const size_t idx = context->counter.fetch_add(1ul, std::memory_order_relaxed);
            if (idx > context->size) {
                break;
            }
            f(index, idx);
        }
        index.unset_building();
        SpinLockAcquire(&context->lock);
        index.report_to(context->perf);
        SpinLockRelease(&context->lock);
        index.destroy();
    };

    if (context->build_params) {
        if (context->use_btree) {
            DiskAnnBTreeData *btree_data = (DiskAnnBTreeData *)context->build_params;
            switch (btree_data->type) {
                case BTreeBuildParamType::BUILD:
                    {
                        DiskAnnBuildBTreeData *build_data = (DiskAnnBuildBTreeData *)btree_data;
                        build_index(DiskANNIndex(target, build_data->index_meta_blkno, false, context->_isWal),
                                    [&](DiskANNIndex &index, size_t idx) {
                            index.insert_point_to(build_data->data[idx].vid);
                            context->_timer->inc_loop_count_forground_report("Build Index");
                        });
                    }
                    break;
                case BTreeBuildParamType::VACUUM:
                    {
                        DiskAnnVacuumBTreeData *vacuum_data = (DiskAnnVacuumBTreeData *)btree_data;
                        build_index(DiskANNIndex(target, vacuum_data->index_meta_blkno, false, context->_isWal),
                                    [&](DiskANNIndex &index, size_t idx) {
                            index.process_deleted_neighbors(vacuum_data->data[idx], [&](size_t j){
                                return vacuum_data->delete_set.contains(j);
                            });
                            context->_timer->inc_loop_count_forground_report("Vacuum Index");
                        });
                    }
                    break;
                case BTreeBuildParamType::MEM_BUILD:
                    {
                        DiskAnnBuildMemBTreeData *mem_build_data = (DiskAnnBuildMemBTreeData *)btree_data;
                        const uint32 dim = mem_build_data->dim;
                        build_index(DiskANNIndex(target, mem_build_data->index_meta_blkno, mem_build_data->data,
                                    mem_build_data->node, mem_build_data->graph, mem_build_data->mutex),
                                    [mem_build_data, dim, context](DiskANNIndex &index, size_t idx) {
                            index.insert_point_to(mem_build_data->data.at(idx * dim), idx);
                            context->_timer->inc_loop_count_forground_report("Build Index");
                        });
                    }
                    break;
                default:
                    Assert(false);
                    break;
            }
        } else { /* for memory build */
            DiskAnnBuildMemData *mem_data = (DiskAnnBuildMemData *)context->build_params;
            const uint32 dim = mem_data->dim;
            build_index(DiskANNIndex(target, mem_data->data, mem_data->node, mem_data->graph, mem_data->mutex),
                        [mem_data, dim, context](DiskANNIndex &index, size_t idx) {
                index.insert_point_to(mem_data->data.at(idx * dim), idx);
                context->_timer->inc_loop_count_forground_report("Build Index");
            });
        }
    } else { /* for disk build */
        build_index(DiskANNIndex(target, false, false), [context](DiskANNIndex &index, size_t idx) {
            index.insert_point_to(idx);
            context->_timer->inc_loop_count_forground_report("Build Index");
        });
    }

    index_close(target, NoLock);
    if (context->part_id != InvalidOid) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        partitionClose(parent, part, NoLock);
        index_close(parent, NoLock);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized */
    }
}

void DiskANNIndex::do_parallel_build(void *build_params, size_t start, size_t total_size, size_t num_parallel)
{
    const size_t report_step_size = total_size > report_threshold ? 10'000ul : 1'000ul;
    char indexName[NAMEDATALEN + 1];
	char partIndexName[NAMEDATALEN + 1];
    populate_index_partition_name(_rel, indexName, partIndexName);
    Timer timer(total_size - start + 1ul, report_step_size, indexName, partIndexName);
    auto *context = NEW DiskANNBuildContext(_rel, build_params, start, total_size, _storage->get_perf_usage(), _isWal, &timer);

    bool for_build = true;
    if (context->use_btree) {
        DiskAnnBTreeData *btree_data = (DiskAnnBTreeData *)build_params;
        for_build = btree_data->type != BTreeBuildParamType::VACUUM;
    }

    if (for_build) {
        timer.set_stage("Build Index");
    } else {
        timer.set_stage("Vacuum Index");
    }

    int res = LaunchBackgroundWorkers(int(num_parallel), (void *)context, build_parallel, build_cleanup);
    BgworkerListWaitStart(&res);
    if (res <= int(num_parallel / 2ul)) {
        BgworkerListSyncQuit();
        if (context->use_btree) {
            g_instance.diskann_cxt.vec_indexer_worker_count -= num_parallel;
        }
        ereport(ERROR, (errmsg("Failed to launch enough background workers")));
    }

    const size_t wait_time = total_size > report_threshold ? 5'000'000ul : 1'000'000ul;


    size_t cur_report_idx;
    while ((cur_report_idx = (context->counter.load(std::memory_order_relaxed))) < total_size) {
        CHECK_FOR_INTERRUPTS();
        pg_usleep(wait_time);
        BgworkerListCheckStatus();

        if (for_build) {
            timer.forground_report("Build Index");
        } else {
            timer.forground_report("Vacuum Index");
        }
    }
    if (for_build) {
        timer.set_stage("Build Index finished");
    } else {
        timer.set_stage("Vacuum Index finished");
    }
    timer.destroy();
    BgworkerListWaitFinish(&res);
    BgworkerListSyncQuit();
}

static void preprocess_vector(large_vector<float> &data, uint32 dim, Metric metric)
{
    auto preprocessor = get_vector_preprocess_func(metric);
    if (preprocessor) {
        Timer timer(data.size() / dim, 500'000ul);
        timer.report("  Start preprocessing data");
        for (size_t i = 0; i < data.size(); i += dim) {
            preprocessor(data.at(i), dim, data.at(i));
            timer.report_loop("  Preprocessing data");
        }
        timer.report("  Data preprocessing done");
        timer.destroy();
    }
}

void DiskANNIndex::build(size_t size, size_t num_parallel)
{
    Assert(size > 0);

    Timer timer;
    timer.report("Initiating index");
    init_disk_storage(size);

    set_building();
    timer.report("Start building index");
    if (num_parallel <= 1ul) {
        const size_t report_step_size = size > report_threshold ? 10'000ul : 1'000ul;
        char indexName[NAMEDATALEN + 1];
        char partIndexName[NAMEDATALEN + 1];
        populate_index_partition_name(_rel, indexName, partIndexName);
        Timer build_timer(size, report_step_size, indexName, partIndexName);
        build_timer.set_stage("Build Index");
        for (size_t i = 1ul; i <= size; ++i) {
            insert_point_to(i);
            build_timer.report_loop("Build Index");
        }
        build_timer.destroy();
    } else {
        do_parallel_build(NULL, 1ul, size, num_parallel);
    }

    insert_point_to(0);
    unset_building();
    timer.report("Index built");
    timer.destroy();
}

void DiskANNIndex::build(large_vector<float> &data, large_vector<DiskAnnVamanaNode> &node,
                         large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex,
                         size_t num_parallel, BlockNumber meta_blkno)
{
    Assert(data.size() % dim() == 0);
    if (data.empty()) {
        return;
    }

    Timer timer;
    size_t size = data.size() / dim();
    timer.report("Initiating index");
    preprocess_vector(data, dim(), metric());

    set_building();
    timer.report("Start building index");
    if (num_parallel <= 1ul) {
        const size_t report_step_size = size > report_threshold ? 10'000ul : 1'000ul;
        char indexName[NAMEDATALEN + 1];
        char partIndexName[NAMEDATALEN + 1];
        populate_index_partition_name(_rel, indexName, partIndexName);
        Timer build_timer(size - 1ul, report_step_size, indexName, partIndexName);
        build_timer.set_stage("Build Index");
        for (size_t i = 1ul; i < size; ++i) {
            insert_point_to(&data[i * dim()], i);
            build_timer.report_loop("Build Index");
        }
        build_timer.destroy();
    } else {
        if (DiskAnnUseBTree(_rel)) {
            DiskAnnBuildMemBTreeData btree_data =
                {BTreeBuildParamType::MEM_BUILD, meta_blkno, data, node, graph, mutex, dim()};
            do_parallel_build(&btree_data, 1ul, size - 1ul, num_parallel);
        } else {
            DiskAnnBuildMemData mem_data = {data, node, graph, mutex, dim()};
            do_parallel_build(&mem_data, 1ul, size - 1ul, num_parallel);
        }
    }
    insert_point_to(data.data(), 0);
    unset_building();
    timer.report("Index built");
    timer.destroy();
}

void DiskANNIndex::flush(large_vector<float> &&data, large_vector<DiskAnnVamanaNode> &&node,
    large_vector<AnnNeighbors> &&graph, uint32 medoid)
{
    Assert(data.size() % dim() == 0);
    if (data.size() <= 0) {
        return;
    }

    Timer timer;
    const size_t size = data.size() / dim();
    if (BlockNumberIsValid(_meta.pqCompressedMetaBlkNo)) {
        DiskVector<uint8> pq(_rel, _meta.pqCompressedMetaBlkNo, false);
        uint8 *medoid_pq = (uint8 *)palloc(_meta.numPQChunks * sizeof(uint8));
        pq.template get_n<AccessorLockType::NoLockRead>((medoid + 1ul) * _meta.numPQChunks, _meta.numPQChunks, medoid_pq);
        pq.template set_n<AccessorLockType::NoLockWrite>(0, _meta.numPQChunks, medoid_pq);
        pfree(medoid_pq);
        pq.destroy();
    }

    timer.report("  Start expanding disk space for vectors");
    constexpr size_t block_size = 10'000ul;
    for (size_t i = 0; i < size; i += block_size) {
        size_t block = std::min(block_size, size - i);
        vec_write(_rel->rd_smgr, i * dim() * sizeof(float), block * dim() * sizeof(float), (const char *)&data[i * dim()], true);
    }
    optional_destroy(data);

    timer.report("  Start expanding disk space for graph");
    DiskVector<AnnNeighbors> neighbors(_rel, _meta.graphMetaBlkNo, false);
    neighbors.push_back_n(graph.data(), size);
    neighbors.destroy();
    optional_destroy(graph);

    timer.report("  Start expanding disk space for nodes");
    DiskVector<DiskAnnVamanaNode> nodes(_rel, _meta.nodeMetaBlkNo, false);
    nodes.push_back_n(node.data(), size);
    nodes.destroy();
    optional_destroy(node);

    timer.report("  Disk storage initialization done");
    timer.destroy();
}

void DiskANNIndex::flush(size_t size)
{
    Assert(size > 0);

    Timer timer;
    uint32 medoid = calculate_entry_point(size);

    if (BlockNumberIsValid(_meta.pqCompressedMetaBlkNo)) {
        DiskVector<uint8> pq(_rel, _meta.pqCompressedMetaBlkNo, false);
        uint8 *medoid_pq = (uint8 *)palloc(_meta.numPQChunks * sizeof(uint8));
        pq.template get_n<AccessorLockType::NoLockRead>((medoid + 1ul) * _meta.numPQChunks,
                                                        _meta.numPQChunks, medoid_pq);
        pq.template set_n<AccessorLockType::NoLockWrite>(0, _meta.numPQChunks, medoid_pq);
        pfree(medoid_pq);
        pq.destroy();
    }

    float *vec_buf = (float *)palloc(sizeof(float) * dim());
    timer.report("  Start setting entry point for vectors");
    read_vector(_rel, medoid + 1ul, dim(), (char *)vec_buf);
    write_vector(_rel, 0, dim() * sizeof(float), (char *)vec_buf);
    pfree(vec_buf);

    timer.report("  Start setting entry point for nodes");
    DiskVector<DiskAnnVamanaNode> nodes(_rel, _meta.nodeMetaBlkNo, false);
    DiskAnnVamanaNode tmp_node;
    tmp_node.heapTid = nodes.get<AccessorLockType::NoLockRead>(medoid + 1ul).heapTid;
    tmp_node.flag = diskann_node_flag::init_flag;
    diskann_node_flag::set_frozen(tmp_node.flag);
    nodes.set<AccessorLockType::NoLockWrite>(0, tmp_node);
    nodes.destroy();

    timer.report("  Start setting entry point for graph");
    DiskVector<AnnNeighbors> neighbors(_rel, _meta.graphMetaBlkNo, false);
    AnnNeighbors tmp_nbrs = neighbors.get<AccessorLockType::NoLockRead>(medoid + 1ul);
    neighbors.set<AccessorLockType::NoLockWrite>(0, tmp_nbrs);
    neighbors.destroy();

    timer.report("  Disk storage initialization done");
    timer.destroy();
}
