/**
 * Copyright ...
 * Storage interface using memory.
 */

#ifndef DISKANN_MEM_STORE_H
#define DISKANN_MEM_STORE_H

#include <vtl/bitlock.hpp>

#include "access/diskann/storage_interface/storage_interface.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/distance/distance.h"

class MemStore : public StorageInterface {
    using super = StorageInterface;
public:
    MemStore(large_vector<float> &data, large_vector<DiskAnnVamanaNode> &nodes,
             large_vector<AnnNeighbors> &graph, pthread_mutex_t *mutex,
             Metric metric, uint32 dim, uint32 degree);
    ~MemStore() {}
    void destroy();

    void preprocess(const float *query) {}
    void postprocess() {}

    AnnNeighbors get_neighbors(const size_t location);
    bool set_neighbors(const size_t location, const AnnNeighbors &neighbors);
    bool add_neighbor(const size_t location, const AnnNeighbor &neighbor);
    void clear_neighbors(const size_t location);
    float *get_vector(const size_t location);
    void get_vector(const size_t location, float *dest);
    float get_distance(const size_t loc1, const size_t loc2);
    float get_distance(const size_t location, const float *vec);
    void get_distance(const Vector<size_t> &neighbors, const float *vec, float *dists);
    DiskAnnVamanaNode get_node_data(const size_t location);
    void set_node_data(const size_t location, const DiskAnnVamanaNode &data);

    size_t push_back_new_node(const float *point, const DiskAnnVamanaNode &node);
    size_t size() { return _nodes.size(); }
    void print(size_t location);
private:
    uint32 _max_degree{MAX_ANN_GRAPH_DEGREE};
    large_vector<float> &_vectors;
    large_vector<DiskAnnVamanaNode> &_nodes;
    large_vector<AnnNeighbors> &_neighbors;
    pthread_mutex_t *_mutex;
    uint32 _dim;
    ann_helper::distance_func _distance_func;
    ann_helper::distance_func_batch _distance_func_batch;

    float calc_distance(const float *x, const float *y) { return _distance_func(x, y, _dim); }
    void calc_distance_batch(float *out, const float *x, const float *y, size_t y_size)
        { _distance_func_batch(x, y, _dim, y_size, out); }
};

#endif /* DISKANN_MEM_STORE_H */
