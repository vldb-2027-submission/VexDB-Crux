/**
 * Copyright ...
 */

#include "access/annvector/store/vector_smgr.h"
#include "access/diskann/storage_interface/disk_store.h"
#include "access/annvector/xlog/log_manager.h"

using namespace ann_helper;
using disk_container::AccessorLockType;

/* thread unsafe */
#define CACHE_ALLOC(size)   \
    MemoryContextMemalignAllocDebug(DISKANN_CTX.ctx, ann_helper::vector_aligned_size, (size), __FILE__, __LINE__)
#define CACHE_FREE(ptr) MemoryContextMemalignFree(DISKANN_CTX.ctx, (ptr))

/* require external read lock */
template <bool use_pq>
void DiskStore<use_pq>::load_pq_cache()
{
    if (_pq_cache) {
        return;
    }
    LWLockAcquire(VectorPQCacheLock, LW_SHARED);
    _pq_cache = diskann_get_pq_cache(_rel->rd_id);
    LWLockRelease(VectorPQCacheLock);
    if (!_pq_cache) {
        float *pivots = (float *)CACHE_ALLOC(_pq_param.num_centers * _pq_param.dim * sizeof(float));
        float *tmp_pivots = (float *)palloc(_pq_param.num_centers * _pq_param.dim * sizeof(float));
        disk_container::DiskVector<float> pivots_vec(_rel, _pq_param.pivot_blkno, false);
        pivots_vec.template get_n<AccessorLockType::NoLockUnsafe>(0u, _pq_param.num_centers * _pq_param.dim, tmp_pivots);
        pivots_vec.destroy();
        const uint32 step_size = _pq_param.dim / _pq_param.num_pq_chunks;
        for (uint32 chunk = 0; chunk < _pq_param.num_pq_chunks; ++chunk) {
            const uint32 chunk_length =
                chunk == _pq_param.num_pq_chunks - 1 ? _pq_param.dim - (chunk * step_size) : step_size;
            for (uint32 center = 0; center < _pq_param.num_centers; ++center) {
                for (uint32 i = 0; i < chunk_length; ++i) {
                    pivots[chunk * _pq_param.num_centers * step_size + center * chunk_length + i] =
                        tmp_pivots[center * _pq_param.dim + chunk * step_size + i];
                }
            }
        }
        pfree(tmp_pivots);
        LWLockAcquire(VectorPQCacheLock, LW_EXCLUSIVE);
        if (!diskann_set_pq_cache(_rel->rd_id, pivots)) {
            CACHE_FREE(pivots);
        }
        _pq_cache = diskann_get_pq_cache(_rel->rd_id);
        LWLockRelease(VectorPQCacheLock);
    }
}

template <bool use_pq>
DiskStore<use_pq>::DiskStore(Relation rel, const DiskAnnMetaPage &meta, bool need_wal)
    : super(),
      _rel(rel),
      _max_degree(DiskAnnGetMaxGraphDegree(rel)),
      _neighbors(_rel, meta.graphMetaBlkNo, need_wal),
      _ptrs(_rel, meta.nodeMetaBlkNo, need_wal),
      _pq_data(),
      _metric(meta.metric),
      _dim(meta.dimensions),
      _aligned_dim(get_aligned_dim(_dim)),
      _distance_func(get_aligned_distance_func(_metric, _dim))
{
    _vec_buf = alloc_floatvector(_aligned_dim, _default_buf_size);
    if (use_pq) {
        _pq_param = {meta.numCenters, meta.numPQChunks, _dim, meta.pqPivotsMetaBlkNo};
        _pq_data.emplace(_rel, meta.pqCompressedMetaBlkNo, need_wal);
    }
}

template <bool use_pq>
void DiskStore<use_pq>::postprocess()
{
    if (use_pq) {
        pfree_ext(_pq_dists);
    }
}

template <bool use_pq>
void DiskStore<use_pq>::destroy()
{
    optional_destroy(_perf_usage);
    optional_destroy(_neighbors);
    optional_destroy(_ptrs);
    _pq_data.destroy();
    free_vector(_vec_buf);
    postprocess();
}

template <bool use_pq>
void DiskStore<use_pq>::preprocess(const float *query)
{
    if (!use_pq) {
        return;
    }

    load_pq_cache();
    Assert(_pq_cache);

    perf_vec_cmp(_pq_param.num_centers);

    _pq_dists = (float *)palloc0(_pq_param.num_pq_chunks * _pq_param.num_centers * sizeof(float *));
    const uint32 step_size = _pq_param.dim / _pq_param.num_pq_chunks;
    auto func = ann_helper::get_general_distance_batch_func(_metric, step_size);
    for (uint32 chunk = 0; chunk < _pq_param.num_pq_chunks - 1; ++chunk) {
        func(query + chunk * step_size, _pq_cache->pivots + chunk * _pq_param.num_centers * step_size,
             step_size, _pq_param.num_centers, _pq_dists + chunk * _pq_param.num_centers);
    }
    const uint32 cur_pos = (step_size * (_pq_param.num_pq_chunks - 1));
    uint32 last_dim = _dim - cur_pos;
    auto func_last = ann_helper::get_general_distance_batch_func(_metric, last_dim);
    func_last(query + cur_pos, _pq_cache->pivots + cur_pos * _pq_param.num_centers,
              last_dim, _pq_param.num_centers,
              _pq_dists + (_pq_param.num_pq_chunks - 1) * _pq_param.num_centers);

    perf_stop();
}

template <bool use_pq>
AnnNeighbors DiskStore<use_pq>::get_neighbors(const size_t location)
{
    perf_neighbor_read(1);
    AnnNeighbors res = _neighbors.template get<AccessorLockType::ReadLock>(location);
    perf_stop();
    return res;
}

template <bool use_pq>
bool DiskStore<use_pq>::set_neighbors(const size_t location, const AnnNeighbors &neighbors)
{
    const auto filt = [&neighbors](AnnNeighbors &n) -> bool {
        if (n.version != neighbors.version) {
            if (n != neighbors) {
                return false;
            }
            if (neighbors.version > n.version) {
                n.version = neighbors.version;
            }
            return false;
        }
        n = neighbors;
        ++n.version;
        return true;
    };
    perf_neighbor_write(1);
    bool res = _neighbors.template apply<AccessorLockType::WriteLock>(filt)(location);
    perf_stop();
    return res;
}

template <bool use_pq>
bool DiskStore<use_pq>::add_neighbor(const size_t location, const AnnNeighbor &neighbor)
{
    const auto filt = [this, &neighbor](AnnNeighbors &n) -> bool {
        if (n.num_neighbors >= _max_degree) {
            return false;
        }
        n.neighbors[n.num_neighbors] = neighbor;
        ++n.num_neighbors;
        ++n.version;
        return true;
    };
    perf_neighbor_write(1);
    bool res = _neighbors.template apply<AccessorLockType::WriteLock>(filt)(location);
    perf_stop();
    return res;
}

template <bool use_pq>
void DiskStore<use_pq>::clear_neighbors(const size_t location)
{
    perf_neighbor_write(1);
    AnnNeighbors neighbors;
    neighbors.num_neighbors = 0;
    _neighbors.template set<AccessorLockType::WriteLock>(location, neighbors);
    perf_stop();
}

template <bool use_pq>
float *DiskStore<use_pq>::get_vector(const size_t location)
{
    perf_vec_read(1);
    float *res = alloc_floatvector(_dim);
    VecBuffer buffer = vec_read_buffer(_rel, location, _dim * sizeof(float));
    memcpy(res, buffer.get_vecbuf(), _dim * sizeof(float));
    buffer.release();
    perf_stop();
    return res;
}

template <bool use_pq>
void DiskStore<use_pq>::get_vector(const size_t location, float *dest)
{
    perf_vec_read(1);
    VecBuffer buffer = vec_read_buffer(_rel, location, _dim * sizeof(float));
    memcpy(dest, buffer.get_vecbuf(), _dim * sizeof(float));
    buffer.release();
    perf_stop();
}

template <bool use_pq>
float DiskStore<use_pq>::get_distance(const size_t loc1, const size_t loc2)
{
    if (use_pq && _pq_dists) {
        Assert(_pq_data);
        uint8 *buf = (uint8 *)_vec_buf;
        perf_pq_cmp(1);
        _pq_data->get_n<AccessorLockType::NoLockUnsafe>(loc1 * _pq_param.num_pq_chunks, _pq_param.num_pq_chunks, buf);
        float res = 0;
        for (uint32 i = 0; i < _pq_param.num_pq_chunks; i++) {
            res += _pq_dists[i * _pq_param.num_centers + buf[i]];
        }
        perf_stop();
        return res;
    }
    perf_vec_read(2);
    VecBuffer buffer1 = vec_read_buffer(_rel, loc1, _dim * sizeof(float));
    VecBuffer buffer2 = vec_read_buffer(_rel, loc2, _dim * sizeof(float));
    perf_stop();
    perf_vec_cmp(1);
    float res = calc_distance((float *)buffer1.get_vecbuf(), (float *)buffer2.get_vecbuf());
    perf_stop();
    buffer1.release();
    buffer2.release();
    return res;
}

template <bool use_pq>
float DiskStore<use_pq>::get_distance(const size_t location, const float *vec)
{
    if (use_pq && _pq_dists) {
        Assert(_pq_data);
        uint8 *buf = (uint8 *)_vec_buf;
        perf_pq_cmp(1);
        _pq_data->get_n<AccessorLockType::NoLockUnsafe>(location * _pq_param.num_pq_chunks, _pq_param.num_pq_chunks, buf);
        float res = 0;
        for (uint32 i = 0; i < _pq_param.num_pq_chunks; i++) {
            res += _pq_dists[i * _pq_param.num_centers + buf[i]];
        }
        perf_stop();
        return res;
    }
    perf_vec_read(1);
    VecBuffer buffer = vec_read_buffer(_rel, location, _dim * sizeof(float));
    perf_stop();
    perf_vec_cmp(1);
    float res = calc_distance(vec, (float *)buffer.get_vecbuf());
    perf_stop();
    buffer.release();
    return res;
}

template <bool use_pq>
void DiskStore<use_pq>::get_distance(
    const Vector<size_t> &neighbors, const float *vec, float *dists)
{
    if (neighbors.empty()) {
        return;
    }
    Assert(dists);
    if (use_pq && _pq_dists) {
        Assert(_pq_data);
        errno_t rc = memset_s(dists, neighbors.size() * sizeof(float), 0, neighbors.size() * sizeof(float));
        securec_check(rc, "\0", "\0");
        uint8 *buf = (uint8 *)_vec_buf;
        perf_pq_read(neighbors.size());
        for (size_t id : neighbors) {
            _pq_data->get_n<AccessorLockType::NoLockUnsafe>(id * _pq_param.num_pq_chunks, _pq_param.num_pq_chunks, buf);
            buf += _pq_param.num_pq_chunks;
        }
        perf_stop();
        buf = (uint8 *)_vec_buf;
        perf_pq_cmp(neighbors.size());
        __builtin_prefetch(dists, 0, 3);
        __builtin_prefetch(buf, 0, 3);
        __builtin_prefetch(buf + 64, 0, 3);
        __builtin_prefetch(buf + 128, 0, 3);
        const float *dists_table = _pq_dists;
        for (uint32 j = 0; j < _pq_param.num_pq_chunks; ++j) {
            __builtin_prefetch(dists_table + _pq_param.num_centers, 0, 3);
            for (size_t i = 0; i < neighbors.size(); ++i) {
                dists[i] += dists_table[buf[i * _pq_param.num_pq_chunks + j]];
            }
            dists_table += _pq_param.num_centers;
        }
        perf_stop();
        return;
    }
    for (auto id : neighbors) {
        perf_vec_read(1);
        VecBuffer buffer = vec_read_buffer(_rel, id, _dim * sizeof(float));
        perf_stop();
        perf_vec_cmp(1);
        *dists = calc_distance(vec, (float *)buffer.get_vecbuf());
        perf_stop();
        buffer.release();
        ++dists;
    }
}

template <bool use_pq>
DiskAnnVamanaNode DiskStore<use_pq>::get_node_data(const size_t location)
{
    perf_tag_read(1);
    DiskAnnVamanaNode res = _ptrs.template get<AccessorLockType::NoLockRW>(location);
    perf_stop();
    return res;
}

template <bool use_pq>
void DiskStore<use_pq>::set_node_data(const size_t location, const DiskAnnVamanaNode &data)
{
    _ptrs.template set<AccessorLockType::NoLockRW>(location, data);
}

template <bool use_pq>
size_t DiskStore<use_pq>::push_back_new_node(
    const float *point, const DiskAnnVamanaNode &node)
{
    Assert(point);
    size_t loc = _ptrs.push_back(node);
    LogManager logmgr(_rel);
    logmgr.log_write_vector(loc * _dim * sizeof(float), _dim * sizeof(float), (char *)point, false);
    logmgr.destroy();
    write_vector(_rel, loc, _dim * sizeof(float), (char *)point);
    _neighbors.extend(loc + 1);
    if (BlockNumberIsValid(_pq_param.pivot_blkno)) {
        Assert(use_pq && _pq_data);
        load_pq_cache();
        const uint32 step_size = _pq_param.dim / _pq_param.num_pq_chunks;
        auto dist_func = get_general_distance_batch_func(Metric::L2, step_size);
        float *dist_buf = alloc_floatvector(_pq_param.num_centers);
        uint8 *buf = (uint8 *)_vec_buf;
        for (uint32 i = 0; i < _pq_param.num_pq_chunks - 1; ++i) {
            dist_func(point + i * step_size, _pq_cache->pivots + i * _pq_param.num_centers * step_size,
                      _pq_param.num_centers, step_size, dist_buf);
            float min_dist = FLT_MAX;
            for (uint32 j = 0; j < _pq_param.num_centers; ++j) {
                if (dist_buf[j] < min_dist) {
                    min_dist = dist_buf[j];
                    buf[i] = j;
                }
            }
        }
        uint32 last_dim = _dim - (step_size * (_pq_param.num_pq_chunks - 1));
        auto dist_func_last = get_general_distance_batch_func(_metric, last_dim);
        dist_func_last(point + (step_size * (_pq_param.num_pq_chunks - 1)),
                       _pq_cache->pivots + (step_size * (_pq_param.num_pq_chunks - 1) * _pq_param.num_centers),
                       _pq_param.num_centers, last_dim, dist_buf);
        float min_dist = FLT_MAX;
        for (uint32 j = 0; j < _pq_param.num_centers; ++j) {
            if (dist_buf[j] < min_dist) {
                min_dist = dist_buf[j];
                buf[_pq_param.num_pq_chunks - 1] = j;
            }
        }
        free_vector(dist_buf);
        _pq_data->template set_n<AccessorLockType::WriteLock>(loc * _pq_param.num_pq_chunks, _pq_param.num_pq_chunks, buf);
    }
    return loc;
}

template <bool use_pq>
void DiskStore<use_pq>::print(size_t location)
{
    char buf[_neighbors.size() * 96 * 8] = {'\0'};
    sprintf(buf, "Point %lu inserted:\n", location);
    for (size_t i = 0; i < _neighbors.size(); i++) {
        AnnNeighbors n = get_neighbors(i);
        if (n.num_neighbors > 0) {
            char pbuf[n.num_neighbors * 8] = {'\0'};
            sprintf(pbuf, "\t%lu:(", i);
            for (uint32 j = 0; j < n.num_neighbors; j++) {
                char nbuf[8] = {'\0'};
                sprintf(nbuf, "%u,", n.neighbors[j]);
                strcat(pbuf, nbuf);
            }
            strcat(pbuf, ")\n");
            strcat(buf, pbuf);
        }
    }
    ereport(NOTICE, (errmsg("%s", buf)));
}

template class DiskStore<true>;
template class DiskStore<false>;
