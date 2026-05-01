/**
 * Copyright ...
 * Diskann partition algorithm helpers.
 */

#ifndef DISKANN_PARTITION_H
#define DISKANN_PARTITION_H

#include "access/diskann/diskann_internal.h"

constexpr float OVERHEAD_FACTOR = 1.1f;
constexpr float GRAPH_SLACK_FACTOR = 1.3f;

inline double estimate_ram_usage(uint32_t size, uint32_t dim, uint32_t datasize, uint32_t degree)
{
    double size_of_data = ((double)size) * ((dim + 7u) / 8u) * datasize;
    double size_of_graph = ((double)size) * degree * sizeof(uint32_t) * GRAPH_SLACK_FACTOR;
    double size_of_outer_vector = ((double)size) * sizeof(ptrdiff_t);

    return OVERHEAD_FACTOR * (size_of_data + size_of_graph + size_of_outer_vector);
}

void gen_random_slice(DiskAnnBuildState *buildState, float *&sampled_data, uint32 *slice_size);

void retrieve_shard_data_from_ids(DiskAnnBuildState *buildState, uint32_t shardIndex);

bool partition_with_ram_budget(DiskAnnBuildState *buildState, uint32_t k_base);

void merge_shards(DiskAnnBuildState *buildState);

#endif /* DISKANN_PARTITION_H */
