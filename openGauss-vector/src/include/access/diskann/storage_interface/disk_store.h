/**
 * Copyright ...
 * Storage interface using disks.
 */

#ifndef DISKANN_DISK_STORE_H
#define DISKANN_DISK_STORE_H

#include <vtl/optional>
#include <vtl/disk_container/diskvector.hpp>

#include "access/diskann/storage_interface/storage_interface.h"
#include "access/diskann/diskann_internal.h"
#include "access/annvector/distance/distance.h"

template <bool use_pq = false>
class DiskStore : public StorageInterface {
    using super = StorageInterface;
public:
    DiskStore(Relation rel, const DiskAnnMetaPage &meta, bool need_wal = true);
    void destroy();
    void preprocess(const float *query);
    void postprocess();
    size_t push_back_new_node(const float *point, const DiskAnnVamanaNode &node);
    size_t size() { return _ptrs.size(); }
    void print(size_t location);

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
private:
    static constexpr uint32 _default_buf_size = !use_pq ? 2ul : MAX_ANN_GRAPH_DEGREE;

    Relation _rel;
    uint32 _max_degree{MAX_ANN_GRAPH_DEGREE};
    disk_container::DiskVector<AnnNeighbors> _neighbors;
    disk_container::DiskVector<DiskAnnVamanaNode> _ptrs;
    Optional<disk_container::DiskVector<uint8>> _pq_data; /* no use case for tempfile use pq */
    Metric _metric;
    uint32 _dim;
    uint32 _aligned_dim;
    ann_helper::distance_func _distance_func;
    float *_vec_buf;
    const DiskAnnPQCache *_pq_cache{NULL};
    float *_pq_dists{NULL};
    DiskAnnPQCacheParameter _pq_param;

    inline float calc_distance(const float *x, const float *y) { return _distance_func(x, y, _dim); }
    void load_pq_cache();
};

#endif /* DISKANN_DISK_STORE_H */
