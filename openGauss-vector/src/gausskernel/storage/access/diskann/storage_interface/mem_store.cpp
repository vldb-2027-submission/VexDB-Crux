/**
 * Copyright ...
 */

#include <cstring>

#include "access/annvector/store/vector_smgr.h"
#include "access/diskann/storage_interface/mem_store.h"

MemStore::MemStore(large_vector<float> &data, large_vector<DiskAnnVamanaNode> &nodes,
                   large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex,
                   Metric metric, uint32 dim, uint32 max_degree)
    : super(),
      _max_degree(max_degree),
      _vectors(data),
      _nodes(nodes),
      _neighbors(graph),
      _mutex(mutex),
      _dim(dim),
      _distance_func(ann_helper::get_general_distance_func(metric, _dim)),
      _distance_func_batch(ann_helper::get_general_distance_batch_func(metric, _dim)) {}

void MemStore::destroy() { ann_helper::optional_destroy(_perf_usage); }

AnnNeighbors MemStore::get_neighbors(const size_t location)
{
    perf_neighbor_read(1);
    pthread_mutex_lock(_mutex);
    AnnNeighbors res = _neighbors[location];
    pthread_mutex_unlock(_mutex);
    perf_stop();
    return res;
}

bool MemStore::set_neighbors(const size_t location, const AnnNeighbors &neighbors)
{
    bool res;
    perf_neighbor_write(1);
    pthread_mutex_lock(_mutex);
    AnnNeighbors &n = _neighbors[location];
    if (n.version != neighbors.version) {
        res = n == neighbors;
        if (res && neighbors.version > n.version) {
            n.version = neighbors.version;
        }
    } else {
       n = neighbors;
       ++n.version;
       res = true;
    }
    pthread_mutex_unlock(_mutex);
    perf_stop();
    return res;
}

bool MemStore::add_neighbor(const size_t location, const AnnNeighbor &neighbor)
{
    bool res;
    perf_neighbor_write(1);
    pthread_mutex_lock(_mutex);
    AnnNeighbors &n = _neighbors[location];
    if (n.num_neighbors >= _max_degree) {
        res = false;
    } else {
        n.neighbors[n.num_neighbors] = neighbor;
        ++n.num_neighbors;
        ++n.version;
        res = true;
    }
    pthread_mutex_unlock(_mutex);
    perf_stop();
    return res;
}

void MemStore::clear_neighbors(const size_t location)
{
    perf_neighbor_write(1);
    pthread_mutex_lock(_mutex);
    AnnNeighbors n;
    n.num_neighbors = 0;
    _neighbors.set(location, n);
    pthread_mutex_unlock(_mutex);
    perf_stop();
}

float *MemStore::get_vector(const size_t location)
{
    perf_vec_read(1);
    float *res = alloc_floatvector(_dim);
    std::memcpy(res, _vectors.data() + location * _dim, _dim * sizeof(float));
    perf_stop();
    return res;
}

void MemStore::get_vector(const size_t location, float *dest)
{
    perf_vec_read(1);
    std::memcpy(dest, _vectors.data() + location * _dim, _dim * sizeof(float));
    perf_stop();
}

float MemStore::get_distance(const size_t loc1, const size_t loc2)
{
    float res;
    perf_vec_cmp(1);
    res = calc_distance(_vectors.data() + loc1 * _dim, _vectors.data() + loc2 * _dim);
    perf_stop();
    return res;
}

float MemStore::get_distance(const size_t location, const float *vec)
{
    float res;
    perf_vec_cmp(1);
    res = calc_distance(vec, _vectors.data() + location * _dim);
    perf_stop();
    return res;
}

void MemStore::get_distance(const Vector<size_t> &neighbors, const float *vec, float *dists)
{
    if (neighbors.empty()) {
        return;
    }
    Assert(dists);
    size_t cur = 0;
    for (auto id : neighbors) {
        dists[cur] = get_distance(id, vec);
        ++cur;
    }
}

DiskAnnVamanaNode MemStore::get_node_data(const size_t location)
{
    perf_tag_read(1);
    DiskAnnVamanaNode res = _nodes[location];
    perf_stop();
    return res;
}

void MemStore::set_node_data(const size_t location, const DiskAnnVamanaNode &data)
{
    pthread_mutex_lock(_mutex);
    _nodes.set(location, data);
    pthread_mutex_unlock(_mutex);
}

size_t MemStore::push_back_new_node(const float *point, const DiskAnnVamanaNode &node)
{
    Assert(point);
    pthread_mutex_lock(_mutex);
    size_t loc = _vectors.size() / _dim;
    _vectors.push_back(&point[0], &point[_dim]);
    _nodes.push_back(node);
    _neighbors.push_back({0, 0, });
    pthread_mutex_unlock(_mutex);
    return loc;
}

void MemStore::print(size_t location)
{
    char buf[_neighbors.size() * 96 * 8] = {'\0'};
    sprintf(buf, "Point %lu inserted:\n", location);
    for (size_t i = 0; i < _neighbors.size(); ++i) {
        AnnNeighbors &n = _neighbors[i];
        if (n.num_neighbors > 0) {
            char pbuf[n.num_neighbors * 8] = {'\0'};
            sprintf(pbuf, "\t%lu:(", i);
            for (uint32 j = 0; j < n.num_neighbors; ++j) {
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
