#include <cmath>
#include <boost/preprocessor/repetition/repeat.hpp>

#include "utils/fmgroids.h"
#include "access/annvector/distance/cblas_interface.h"
#include "access/annvector/distance/pq/pq_endecode.h"
#include "access/annvector/distance/distance_utils.h"
#include "access/annvector/ann_utils.h"
#include "access/annvector/halfutils.h"
#include "knl/knl_instance.h"

using ann_helper::Arch;
static const Arch best_arch = ann_helper::get_best_arch();

Metric get_func_metric(Oid func_id)
{
    switch (func_id) {
        case F_L2_DISTANCE:
        case F_FLOATVECTOR_L2_SQUARED_DISTANCE:
        case F_HALFVECTOR_L2_DISTANCE:
        case F_HALFVECTOR_L2_SQUARED_DISTANCE:
            return Metric::L2;
        case F_COSINE_DISTANCE:
        case F_HALFVECTOR_COSINE_DISTANCE:
            return Metric::FAST_COSINE;
        case F_INNER_PRODUCT:
        case F_FLOATVECTOR_NEGATIVE_INNER_PRODUCT:
        case F_HALFVECTOR_INNER_PRODUCT:
        case F_HALFVECTOR_NEGATIVE_INNER_PRODUCT:
            return Metric::INNER_PRODUCT;
        case F_FLOATVECTOR_SPHERICAL_DISTANCE:
        case F_HALFVECTOR_SPHERICAL_DISTANCE:
            return Metric::COSINE;
        default:
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unsupported distance function")));
            return Metric::L2; /* keep compiler quiet */
    }
}

#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
#if __GNUC__ >= 12
#define ARM_FUNC_CALL(arg, call)    \
    case Arch::NEONV8:              \
        call(NEONV8_FUNC(arg));     \
    case Arch::SVEV8:               \
        call(SVEV8_FUNC(arg));      \
    case Arch::SVE2V8:              \
        call(SVE2V8_FUNC(arg));     \
    case Arch::NEONV9:              \
        call(NEONV9_FUNC(arg));     \
    case Arch::SVEV9:               \
        call(SVEV9_FUNC(arg));      \
    case Arch::SVE2V9:              \
        call(SVE2V9_FUNC(arg));     \
    case Arch::SMEV9:               \
        call(SMEV9_FUNC(arg));      \
    case Arch::SME2V9:              \
        call(SME2V9_FUNC(arg));
#else
#define ARM_FUNC_CALL(arg, call)    \
    case Arch::NEONV8:              \
        call(NEONV8_FUNC(arg));     \
    case Arch::SVEV8:               \
        call(SVEV8_FUNC(arg));      \
    case Arch::SVE2V8:              \
        call(SVE2V8_FUNC(arg));
#endif
#else
#define ARM_FUNC_CALL(arg, call)
#endif /* arm */
#ifdef __x86_64__
#define X86_FUNC_CALL(arg, call)    \
    case Arch::SSE:                 \
        call(SSE_FUNC(arg));        \
    case Arch::AVX:                 \
        call(AVX_FUNC(arg));        \
    case Arch::AVX512:              \
        call(AVX512_FUNC(arg));
#else
#define X86_FUNC_CALL(arg, call)
#endif
#define ARCH_FUNC_CALL(arch, arg, call) \
    switch (arch) {                     \
        ARM_FUNC_CALL(arg, call)        \
        X86_FUNC_CALL(arg, call)        \
        case Arch::GENERAL:             \
        default:                        \
            call(GENERAL_FUNC(arg));    \
    }

static void pairwise_distance_l2(const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    uint32 one_size = std::max(x_size, y_size);
    float *ones = (float *)palloc(one_size * sizeof(float));
    std::fill_n(ones, one_size, 1.0f);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, x_norm, 1, ones, 1, 0.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, ones, 1, y_norm, 1, 1.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, dim, -2.0f, x, dim, y, dim, 1.0f, out, y_size);
    pfree(ones);
}

static void pairwise_distance_dot(const float *x, const float *y, uint32 dim,
    uint32 x_size, uint32 y_size, float *out)
{
    cblas_sgemm_rnt(x_size, y_size, dim, -1.0f, x, dim, y, dim, 0.0f, out, y_size);
}

namespace ann_helper {
static auto funcer_metric_distancer(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_distancer
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

static auto funcer_metric_batch(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_batch
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

static auto funcer_metric_batch2(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_batch2
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

distance_func get_general_distance_func(Metric metric, uint32 dim)
    { return funcer_metric_distancer(best_arch, metric, false, dim); }
distance_func get_aligned_distance_func(Metric metric, uint32 dim)
    { return funcer_metric_distancer(best_arch, metric, support_aligned_vector, dim); }
distance_func_batch get_general_distance_batch_func(Metric metric, uint32 dim)
    { return funcer_metric_batch(best_arch, metric, false, dim); }
distance_func_batch get_aligned_distance_batch_func(Metric metric, uint32 dim)
    { return funcer_metric_batch(best_arch, metric, support_aligned_vector, dim); }
distance_func_batch2 get_general_distance_batch_func2(Metric metric, uint32 dim)
    { return funcer_metric_batch2(best_arch, metric, false, dim); }
distance_func_batch2 get_aligned_distance_batch_func2(Metric metric, uint32 dim)
    { return funcer_metric_batch2(best_arch, metric, support_aligned_vector, dim); }
distance_func get_general_distance_func(Metric metric)
{
    switch (metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::COSINE:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::COSINE, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::FAST_COSINE:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::FAST_COSINE, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL   

        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::INNER_PRODUCT, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::SPHERICAL:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::SPHERICAL, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::L2_SQRT:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2_SQRT, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::L2_NORM:
#define DISTANCER_ARCH_ARG distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2_NORM, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        default:
            __builtin_unreachable();
            return NULL;
    }
}

static auto funcer_metric_half_distancer(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_half_distancer
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

static auto funcer_metric_half_batch(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_half_batch
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

static auto funcer_metric_half_batch2(Arch arch, Metric metric, bool aligned_input, uint32 dim)
{
#define DISTANCER_ARCH_ARG funcer_metric_half_batch2
#define DISTANCER_ARCH_CALL(n) return n(metric, aligned_input, dim)
    ARCH_FUNC_CALL(arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

distance_func get_general_half_distance_func(Metric metric, uint32 dim)
    { return funcer_metric_half_distancer(best_arch, metric, false, dim); }
distance_func get_aligned_half_distance_func(Metric metric, uint32 dim)
    { return funcer_metric_half_distancer(best_arch, metric, support_aligned_vector, dim); }
distance_func_batch get_general_half_distance_batch_func(Metric metric, uint32 dim)
    { return funcer_metric_half_batch(best_arch, metric, false, dim); }
distance_func_batch get_aligned_half_distance_batch_func(Metric metric, uint32 dim)
    { return funcer_metric_half_batch(best_arch, metric, support_aligned_vector, dim); }
distance_func_batch2 get_general_half_distance_batch_func2(Metric metric, uint32 dim)
    { return funcer_metric_half_batch2(best_arch, metric, false, dim); }
distance_func_batch2 get_aligned_half_distance_batch_func2(Metric metric, uint32 dim)
    { return funcer_metric_half_batch2(best_arch, metric, support_aligned_vector, dim); }
distance_func get_general_half_distance_func(Metric metric)
{
    switch (metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::COSINE:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::COSINE, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::FAST_COSINE:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::FAST_COSINE, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL   

        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::INNER_PRODUCT, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::SPHERICAL:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::SPHERICAL, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::L2_SQRT:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2_SQRT, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        case Metric::L2_NORM:
#define DISTANCER_ARCH_ARG half_distance
#define DISTANCER_ARCH_CALL(n) return n<Metric::L2_NORM, false>
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

        default:
            __builtin_unreachable();
            return NULL;
    }
}

fvec_ny_distance_func get_fvec_ny_distance_func(Metric Metric)
{
    switch (Metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG  fvec_inner_products_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
            __builtin_unreachable();
    }
}

fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func()
{
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny_nearest
#define DISTANCER_ARCH_CALL(nearest) return nearest
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

distance_single_code_func get_distance_single_code_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_single_code_8
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_single_code_16
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
#define DISTANCER_ARCH_ARG distance_single_code_g
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
    }
}

distance_four_codes_func get_distance_four_codes_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_four_codes_8
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_four_codes_16
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
#define DISTANCER_ARCH_ARG distance_four_codes_g
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
    }
}

fht_func get_fht_func(uint32 bottom_log_dim)
{
#define FHT_HELPER(z, i, func) case i: return func##i;
#define DISTANCER_ARCH_ARG fht_helper_
#define DISTANCER_ARCH_CALL(fht)    \
    switch (bottom_log_dim) { \
        BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), FHT_HELPER, fht)   \
    }   \
    return NULL

    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
#undef FHT_HELPER
}

void init_rabitq_func()
{
#define DISTANCER_ARCH_ARG flip_sign
#define DISTANCER_ARCH_CALL(fs) g_instance.annvec_cxt.f_flip_sign = fs; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG kacs_walk
#define DISTANCER_ARCH_CALL(kw) g_instance.annvec_cxt.f_kacs_walk = kw; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG warmup_ip_x0_q
#define DISTANCER_ARCH_CALL(wiq) g_instance.annvec_cxt.f_warmup_ip_x0_q = wiq; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG ip_fxi
#define DISTANCER_ARCH_CALL(ipf) g_instance.annvec_cxt.f_ip_fxi = ipf; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG mask_ip_x0_q
#define DISTANCER_ARCH_CALL(miq) g_instance.annvec_cxt.f_mask_ip_x0_q = miq; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

vector_preprocess_func get_vector_preprocess_func(Metric metric, DistPrecisionType type)
{
    if (metric == Metric::FAST_COSINE) {
        if (type == DistPrecisionType::FLOAT) {
            return [](const void *x, uint32 dim, void *out) {
                float norm = -g_instance.annvec_cxt.negative_inner_product(x, x, dim);
                if (norm == 0 || norm == 1) {
                    if (x != out) {
                        memcpy(out, x, dim * sizeof(float));
                    }
                    return;
                }
                norm = 1.0f / sqrtf(norm);
                move_sscal(dim, norm, (const float *)x, 1, (float *)out);
            };
        }
        if (type == DistPrecisionType::HALF) {
            return [](const void *x, uint32 dim, void *out) {
                float norm = -g_instance.annvec_cxt.half_negative_inner_product(x, x, dim);
                if (norm == 0 || norm == 1) {
                    if (x != out) {
                        memcpy(out, x, dim * sizeof(half));
                    }
                    return;
                }
                norm = sqrtf(norm);
                if (norm > 0) {
                    half *hout = (half *)out;
                    half *hx = (half *)x;
                    for (uint32 i = 0; i < dim; ++i) {
                        hout[i] = Float4ToHalfUnchecked(HalfToFloat4(hx[i]) / norm);
                    }
                }
            };
        }
    }
    return NULL;
}

void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    if (metric == Metric::L2) {
        pairwise_distance_l2(x, y, x_norm, y_norm, dim, x_size, y_size, out);
    } else {
        pairwise_distance_dot(x, y, dim, x_size, y_size, out);
        if (metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
            for (uint32 i = 0; i != x_size * y_size; ++i) {
                out[i] /= x_norm[i / y_size] * y_norm[i % y_size];
            }
        }
    }
}

uint32 get_aligned_dim(uint32 dim)
    { return (dim + vector_step_size - 1) / vector_step_size * vector_step_size; }

size_t get_aligned_vec_size(size_t vec_size)
    { return (vec_size + vector_aligned_size - 1) / vector_aligned_size * vector_aligned_size; }
} /* namespace ann_helper */

float *alloc_floatvector(uint32 dim, size_t n)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, sizeof(float) * dim * n);
    return (float *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

char *alloc_vector(size_t vec_size)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, vec_size);
    return (char *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

bool is_aligned(const void *ptr)
{
    return reinterpret_cast<uintptr_t>(ptr) % ann_helper::vector_aligned_size == 0;
}
