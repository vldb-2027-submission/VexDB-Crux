/**
 * Copyright ...
 * Storage interface for diskann algorithm.
 */

#ifndef DISKANN_STORAGE_INTERFACE_H
#define DISKANN_STORAGE_INTERFACE_H

#include "utils/palloc.h"
#include "access/diskann/diskann.h"
#include "access/annvector/module/perf_usage.h"

class StorageInterface : public BaseObject {
public:
    virtual ~StorageInterface() {}
    virtual void destroy() = 0;
    /* for pq distance calculation */
    virtual void preprocess(const float *query) = 0;
    virtual void postprocess() = 0;
    virtual size_t push_back_new_node(const float *point, const DiskAnnVamanaNode &node) = 0;
    virtual size_t size() = 0;
    virtual void print(size_t location) {}

    virtual AnnNeighbors get_neighbors(const size_t location) = 0;
    virtual bool set_neighbors(const size_t location, const AnnNeighbors &neighbors) = 0;
    virtual bool add_neighbor(const size_t location, const AnnNeighbor &neighbor) = 0;
    virtual void clear_neighbors(const size_t location) = 0;
    virtual float *get_vector(const size_t location) = 0;
    virtual void get_vector(const size_t location, float *dest) = 0;
    virtual float get_distance(const size_t loc1, const size_t loc2) = 0;
    virtual float get_distance(const size_t location, const float *vec) = 0;
    virtual void get_distance(const Vector<size_t> &neighbors, const float *vec, float *dists) = 0;
    virtual DiskAnnVamanaNode get_node_data(const size_t location) = 0;
    virtual void set_node_data(const size_t location, const DiskAnnVamanaNode &data) = 0;

    void perf_vec_cmp(size_t x) { PerfVecCmp(_perf_usage, x); }
    void perf_pq_cmp(size_t x) { PerfPQCmp(_perf_usage, x); }
    void perf_vec_read(size_t x) { PerfVecRead(_perf_usage, x); }
    void perf_vec_read_cmp(size_t x) { PerfVecReadCmp(_perf_usage, x); }
    void perf_pq_read(size_t x) { PerfPQRead(_perf_usage, x); }
    void perf_neighbor_read(size_t x) { PerfNeighborRead(_perf_usage, x); }
    void perf_neighbor_write(size_t x) { PerfNeighborWrite(_perf_usage, x); }
    void perf_tag_read(size_t x) { PerfTagRead(_perf_usage, x); }
    void perf_prune() { PerfPrune(_perf_usage); }
    void perf_iterate() { PerfIter(_perf_usage); }
    void perf_conflict() { PerfConflict(_perf_usage); }
    void perf_report() { PerfReport(_perf_usage); }
    void perf_report_to(PerfUsage &other) { PerfReportTo(_perf_usage, other); }
    void perf_stop() { PerfStop(_perf_usage); }
    PerfUsage &get_perf_usage() { return _perf_usage; }
protected:
    NO_UNIQUE_ADDRESS PerfUsage _perf_usage{};
};

#endif /* DISKANN_STORAGE_INTERFACE_H */
