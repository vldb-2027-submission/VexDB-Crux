/**
 * Copyright ...
 * PQ helpers.
 */

#ifndef DISKANN_DISK_PQ_H
#define DISKANN_DISK_PQ_H

#include "access/diskann/diskann_internal.h"

size_t min_ndistinct_pivots(const float *const passed_train_data, size_t num_train, uint32 nchunk,
                            uint32 dim);
void generate_pq_pivots(DiskAnnBuildState *buildState, const float *const passed_train_data,
                        size_t num_train, uint32 max_k_means_reps);
void generate_pq_data_from_pivots(DiskAnnBuildState *build_state);

#endif /* DISKANN_DISK_PQ_H */
