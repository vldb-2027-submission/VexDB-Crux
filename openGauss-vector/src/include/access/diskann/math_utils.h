/**
 * Copyright ...
 * Math utilities.
 */

#ifndef DISKANN_MATH_UTILS_H
#define DISKANN_MATH_UTILS_H

#include "c.h"

/**
 * @brief
 * Given data in num_points * new_dim row major
 * Pivots stored in full_pivot_data as num_centers * new_dim row major
 * Calculate the k closest pivot for each point and store it in vector
 * closest_centers_ivf (row major, num_points*k) (which needs to be allocated
 * outside).
 * Additionally, if inverted index is not null (and pre-allocated), it
 * will return inverted index for each center, assuming each of the inverted
 * indices is an empty vector.
 * Additionally, if pts_norms_squared is not null, then it will assume that
 * point norms are pre-computed and use those values.
 */
void compute_closest_centers(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers,
                             size_t k, uint32_t *closest_centers_ivf, float *pts_norms_squared = NULL);

/**
 * Run Lloyds until max_reps or stopping criterion
 * Final centers are output in centers as row major num_centers * dim
 */
float run_lloyds(float *data, size_t num_points, size_t dim, float *centers, const size_t num_centers,
                 const size_t max_reps, uint32_t *closest_center);

/* kmeans++ init */
void kmeanspp_selecting_pivots(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers);

#endif /* DISKANN_MATH_UTILS_H */
