/**
 * Copyright ...
 * Distance calculation utilities.
 */

#ifndef DISKANN_UTILS_DISTANCE_H
#define DISKANN_UTILS_DISTANCE_H

#include "c.h"
#include "utils/palloc.h"
#include "access/annvector/halfvec.h"

enum Metric : uint32 {
    L2 = 0,
    COSINE = 1,
    INNER_PRODUCT = 2,
    FAST_COSINE = 4,
    FAST_BLAS_COSINE = 5,
    FAST_BLAS_INNER_PRODUCT = 7,
    SPHERICAL = 8,
    L2_SQRT = 9,
    L2_NORM = 10,
};
Metric get_func_metric(Oid func_id);

enum class DistPrecisionType {
	FLOAT = 0, // should be 0 for forward compatibility
	HALF,
    DIST_TYPE_NUM,
};

namespace ann_helper {
#ifdef __x86_64__
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#define half_vector_step_size 32
#elif defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
/* NEON & SVE only require 16-aligned to reach best performance, but some SME case may beed 32 */
constexpr size_t vector_aligned_size = 32ul;
#define vector_step_size 8
#define half_vector_step_size 16
#else
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#define half_vector_step_size 32
#endif

typedef float (*distance_func)(const void *x, const void *y, uint32 dim);
typedef void (*distance_func_batch)(const void *x, const void *y, uint32 dim, uint32 y_size, float *out);
typedef void (*distance_func_batch2)(const void *x, void *const *y, uint32 dim, uint32 y_size, float *out);
typedef void (*transform_func)(void *x, uint32 dim);
typedef void (*vector_preprocess_func)(const void *x, uint32 dim, void *out);

/* use dim as input can help improve dist calculation efficiency, I guess... */
distance_func get_general_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_distance_func(Metric metric, uint32 dim);
distance_func get_general_distance_func(Metric metric);
distance_func_batch get_general_distance_batch_func(Metric metric, uint32 dim);
distance_func_batch get_aligned_distance_batch_func(Metric metric, uint32 dim);
distance_func_batch2 get_general_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch2 get_aligned_distance_batch_func2(Metric metric, uint32 dim);

/* half vector related */
distance_func get_general_half_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_half_distance_func(Metric metric, uint32 dim);
distance_func get_general_half_distance_func(Metric metric);
distance_func_batch get_general_half_distance_batch_func(Metric metric, uint32 dim);
distance_func_batch get_aligned_half_distance_batch_func(Metric metric, uint32 dim);
distance_func_batch2 get_general_half_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch2 get_aligned_half_distance_batch_func2(Metric metric, uint32 dim);

vector_preprocess_func get_vector_preprocess_func(Metric metric,
    DistPrecisionType type = DistPrecisionType::FLOAT);

/*pq related*/
typedef void (*fvec_ny_distance_func)(float *dis, const float *x, const float *y, uint32 d, uint32 ny);
typedef uint32 (*fvec_L2sqr_ny_nearest_func)(float *distances_tmp_buffer, const float *x, const float *y, uint32 d, uint32 ny);
typedef float (*distance_single_code_func)(uint32 M, uint32 nbits, const float *sim_table, const uint8 *code);
typedef void (*distance_four_codes_func)(uint32 M, uint32 nbits, const float *sim_table,
    const uint8 *__restrict code0, const uint8 *__restrict code1,
    const uint8 *__restrict code2, const uint8 *__restrict code3,
    float &result0, float &result1, float &result2, float &result3);
typedef void (*fht_func)(float *buf);
typedef void (*flip_sign_func)(const uint8 *flip, float *data, size_t dim);
typedef void (*kacs_walk_func)(float *data, size_t len);
typedef float (*warmup_ip_x0_q_func)(uint64 *data, const uint64 *query, float delta, float vl, size_t dim);
typedef float (*ip_fxi_func)(float *query, uint8 *data, size_t dim);
typedef float (*mask_ip_x0_q_func)(float *query, uint64 *data, size_t dim);

fvec_ny_distance_func get_fvec_ny_distance_func(Metric metric);
fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func();
distance_single_code_func get_distance_single_code_func(uint32 nbits);
distance_four_codes_func get_distance_four_codes_func(uint32 nbits);
fht_func get_fht_func(uint32 bottom_log_dim);
void init_rabitq_func();

/* we don't use func pointer here since it is not trivial */
void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out);
uint32 get_aligned_dim(uint32 dim);
size_t get_aligned_vec_size(size_t vec_size);

namespace internal {
constexpr uint32 large_dim = 1536u;
constexpr uint32 medium_dim = 512u;
constexpr uint32 small_dim = 128u;

constexpr uint32 half_large_dim = 2 * 1536u;
constexpr uint32 half_medium_dim = 2 * 512u;
constexpr uint32 half_small_dim = 2 * 128u;
} /* namespace internal */
} /* namespace ann_helper */

constexpr bool support_aligned_vector = true;
float *alloc_floatvector(uint32 dim, size_t n = 1);
char *alloc_vector(size_t vec_size);
inline void free_vector(void *vec) { mem_align_free(vec); }
bool is_aligned(const void *ptr);
#endif /* DISKANN_UTILS_DISTANCE_H */
