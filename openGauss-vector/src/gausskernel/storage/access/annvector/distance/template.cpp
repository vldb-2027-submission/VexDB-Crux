#ifndef DISTANCE_FUNC_NAME
static_assert(false, "don't use the file without definition DISTANCE_FUNC_NAME");
#endif
#ifndef DISTANCE_STRUCT_NAME
static_assert(false, "don't use the file without definition DISTANCE_STRUCT_NAME");
#endif

#include <math.h>
#include <boost/preprocessor/seq.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <vtl/expr_helper>

#include "access/annvector/distance/distance_simd_macros.h"

#define ASSUME_ALIGNED(v) v = (const float *)__builtin_assume_aligned(v, vector_aligned_size)

namespace ann_helper {
using namespace internal;

#ifdef OPTIMIZE1
static void load4(const float *v, vectorize_floats &x0, vectorize_floats &x1,
                  vectorize_floats &x2, vectorize_floats &x3)
{
    x0 = load_vectors(v);
    x1 = load_vectors(v + k_per_iter);
    x2 = load_vectors(v + k_per_iter * 2u);
    x3 = load_vectors(v + k_per_iter * 3u);
}
static void loadu4(const float *v, vectorize_floats &x0, vectorize_floats &x1,
                   vectorize_floats &x2, vectorize_floats &x3)
{
    x0 = loadu_vectors(v);
    x1 = loadu_vectors(v + k_per_iter);
    x2 = loadu_vectors(v + k_per_iter * 2u);
    x3 = loadu_vectors(v + k_per_iter * 3u);
}
#endif

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
#ifdef OPTIMIZE1
    const uint32 n_full = dim / k_step;
    const uint32 n_tail = (dim % k_step) / k_per_iter;
    vectorize_floats acc0 = get_zero_vectors();
    vectorize_floats acc1 = get_zero_vectors();
    vectorize_floats acc2 = get_zero_vectors();
    vectorize_floats acc3 = get_zero_vectors();
    for (uint32 i = 0; i < n_full; ++i) {
        vectorize_floats x0, x1, x2, x3;
        load4(x, x0, x1, x2, x3);
        vectorize_floats y0, y1, y2, y3;
        load4(y, y0, y1, y2, y3);

        vectorize_floats diff0 = sub_vectors(x0, y0);
        vectorize_floats diff1 = sub_vectors(x1, y1);
        vectorize_floats diff2 = sub_vectors(x2, y2);
        vectorize_floats diff3 = sub_vectors(x3, y3);

        acc0 = madd_vectors(diff0, diff0, acc0);
        acc1 = madd_vectors(diff1, diff1, acc1);
        acc2 = madd_vectors(diff2, diff2, acc2);
        acc3 = madd_vectors(diff3, diff3, acc3);

        x += k_step;
        y += k_step;
    }
    switch (n_tail) {
        case 3u: {
            vectorize_floats x2 = load_vectors(x + k_per_iter * 2u);
            vectorize_floats y2 = load_vectors(y + k_per_iter * 2u);
            vectorize_floats diff2 = sub_vectors(x2, y2);
            acc2 = madd_vectors(diff2, diff2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_floats x1 = load_vectors(x + k_per_iter);
            vectorize_floats y1 = load_vectors(y + k_per_iter);
            vectorize_floats diff1 = sub_vectors(x1, y1);
            acc1 = madd_vectors(diff1, diff1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_floats x0 = load_vectors(x);
            vectorize_floats y0 = load_vectors(y);
            vectorize_floats diff0 = sub_vectors(x0, y0);
            acc0 = madd_vectors(diff0, diff0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    /* deal with remainder */
#ifdef USE_AVX512
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        vectorize_floats x3 = _mm512_maskz_load_ps(mask, x);
        vectorize_floats y3 = _mm512_maskz_load_ps(mask, y);
        vectorize_floats diff3 = _mm512_sub_ps(x3, y3);
        acc3 = _mm512_mask3_fmadd_ps(diff3, diff3, acc3, mask);
    }

    return _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(acc0, acc1), _mm512_add_ps(acc2, acc3)));
#elif defined(USE_AVX)
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        alignas(32) float x_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            x_buf[i] = x[i];
        }
        alignas(32) float y_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            y_buf[i] = y[i];
        }
        vectorize_floats x3 = load_vectors(x_buf);
        vectorize_floats y3 = load_vectors(y_buf);
        vectorize_floats diff3 = sub_vectors(x3, y3);
        acc3 = madd_vectors(diff3, diff3, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(USE_SSE)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = _mm_add_ps(_mm_add_ps(acc0, acc1), _mm_add_ps(acc2, acc3));
    acc1 = _mm_hadd_ps(acc0, acc0);
    float res = _mm_cvtss_f32(_mm_hadd_ps(acc1, acc1));
    switch (dim % k_per_iter) {
        case 3u: {
            const float diff2 = x[2] - y[2];
            res += diff2 * diff2;
        } /* fall through */
        case 2u: {
            const float diff1 = x[1] - y[1];
            res += diff1 * diff1;
        } /* fall through */
        case 1u: {
            const float diff0 = x[0] - y[0];
            res += diff0 * diff0;
        } break;
    }
    return res;
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = add_vectors(add_vectors(acc0, acc1), add_vectors(acc2, acc3));
#if defined(__aarch64__) || defined(_M_ARM64)
    float res = vaddvq_f32(acc0);
#else
    float32x2_t sum64 = vpadd_f32(vget_low_f32(acc0), vget_high_f32(acc0));
    float res = vget_lane_f32(vpadd_f32(sum64, sum64), 0);
#endif
    switch (dim % k_per_iter) {
        case 3u: {
            const float diff2 = x[2] - y[2];
            res += diff2 * diff2;
        } /* fall through */
        case 2u: {
            const float diff1 = x[1] - y[1];
            res += diff1 * diff1;
        } /* fall through */
        case 1u: {
            const float diff0 = x[0] - y[0];
            res += diff0 * diff0;
        } break;
    }
    return res;
#else
    static_assert(false, "Unhandled arch in x86 l2 dist");
#endif
#else
    float sum = 0;
    ASSUME_ALIGNED(x);
    ASSUME_ALIGNED(y);
    for (uint32 i = 0; i != dim; ++i) {
        const float diff = x[i] - y[i];
        sum += diff * diff;
    }
    return sum;
#endif
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
#ifdef OPTIMIZE1
    const uint32 n_full = dim / k_step;
    const uint32 n_tail = (dim % k_step) / k_per_iter;
    vectorize_floats acc0 = get_zero_vectors();
    vectorize_floats acc1 = get_zero_vectors();
    vectorize_floats acc2 = get_zero_vectors();
    vectorize_floats acc3 = get_zero_vectors();
    for (uint32 i = 0; i < n_full; ++i) {
        vectorize_floats x0, x1, x2, x3;
        loadu4(x, x0, x1, x2, x3);
        vectorize_floats y0, y1, y2, y3;
        loadu4(y, y0, y1, y2, y3);

        vectorize_floats diff0 = sub_vectors(x0, y0);
        vectorize_floats diff1 = sub_vectors(x1, y1);
        vectorize_floats diff2 = sub_vectors(x2, y2);
        vectorize_floats diff3 = sub_vectors(x3, y3);

        acc0 = madd_vectors(diff0, diff0, acc0);
        acc1 = madd_vectors(diff1, diff1, acc1);
        acc2 = madd_vectors(diff2, diff2, acc2);
        acc3 = madd_vectors(diff3, diff3, acc3);

        x += k_step;
        y += k_step;
    }
    switch (n_tail) {
        case 3u: {
            vectorize_floats x2 = loadu_vectors(x + k_per_iter * 2u);
            vectorize_floats y2 = loadu_vectors(y + k_per_iter * 2u);
            vectorize_floats diff2 = sub_vectors(x2, y2);
            acc2 = madd_vectors(diff2, diff2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_floats x1 = loadu_vectors(x + k_per_iter);
            vectorize_floats y1 = loadu_vectors(y + k_per_iter);
            vectorize_floats diff1 = sub_vectors(x1, y1);
            acc1 = madd_vectors(diff1, diff1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_floats x0 = loadu_vectors(x);
            vectorize_floats y0 = loadu_vectors(y);
            vectorize_floats diff0 = sub_vectors(x0, y0);
            acc0 = madd_vectors(diff0, diff0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    /* deal with remainder */
#ifdef USE_AVX512
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        vectorize_floats x3 = _mm512_maskz_loadu_ps(mask, x);
        vectorize_floats y3 = _mm512_maskz_loadu_ps(mask, y);
        vectorize_floats diff3 = _mm512_sub_ps(x3, y3);
        acc3 = _mm512_mask3_fmadd_ps(diff3, diff3, acc3, mask);
    }

    return _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(acc0, acc1), _mm512_add_ps(acc2, acc3)));
#elif defined(USE_AVX)
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        alignas(32) float x_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            x_buf[i] = x[i];
        }
        alignas(32) float y_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            y_buf[i] = y[i];
        }
        vectorize_floats x3 = load_vectors(x_buf);
        vectorize_floats y3 = load_vectors(y_buf);
        vectorize_floats diff3 = sub_vectors(x3, y3);
        acc3 = madd_vectors(diff3, diff3, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(USE_SSE)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = _mm_add_ps(_mm_add_ps(acc0, acc1), _mm_add_ps(acc2, acc3));
    acc1 = _mm_hadd_ps(acc0, acc0);
    float res = _mm_cvtss_f32(_mm_hadd_ps(acc1, acc1));
    switch (dim % k_per_iter) {
        case 3u: {
            const float diff2 = x[2] - y[2];
            res += diff2 * diff2;
        } /* fall through */
        case 2u: {
            const float diff1 = x[1] - y[1];
            res += diff1 * diff1;
        } /* fall through */
        case 1u: {
            const float diff0 = x[0] - y[0];
            res += diff0 * diff0;
        } break;
    }
    return res;
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = add_vectors(add_vectors(acc0, acc1), add_vectors(acc2, acc3));
#if defined(__aarch64__) || defined(_M_ARM64)
    float res = vaddvq_f32(acc0);
#else
    float32x2_t sum64 = vpadd_f32(vget_low_f32(acc0), vget_high_f32(acc0));
    float res = vget_lane_f32(vpadd_f32(sum64, sum64), 0);
#endif
    switch (dim % k_per_iter) {
        case 3u: {
            const float diff2 = x[2] - y[2];
            res += diff2 * diff2;
        } /* fall through */
        case 2u: {
            const float diff1 = x[1] - y[1];
            res += diff1 * diff1;
        } /* fall through */
        case 1u: {
            const float diff0 = x[0] - y[0];
            res += diff0 * diff0;
        } break;
    }
    return res;
#else
    static_assert(false, "Unhandled arch in x86 l2 dist");
#endif
#else
    float sum = 0;
    for (uint32 i = 0; i != dim; ++i) {
        const float diff = x[i] - y[i];
        sum += diff * diff;
    }
    return sum;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::COSINE, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
    float dot = 0;
    float norm_x = 0;
    float norm_y = 0;
    ASSUME_ALIGNED(x);
    ASSUME_ALIGNED(y);
    for (uint32 i = 0; i < dim; i++) {
        dot += x[i] * y[i];
        norm_x += x[i] * x[i];
        norm_y += y[i] * y[i];
    }
    return -dot / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::COSINE, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
    float dot = 0;
    float norm_x = 0;
    float norm_y = 0;
    for (uint32 i = 0; i < dim; i++) {
        dot += x[i] * y[i];
        norm_x += x[i] * x[i];
        norm_y += y[i] * y[i];
    }
    return -dot / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
}

#if defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
#include "access/annvector/distance/dot_kernel_asimd.h"
#endif
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
#if defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__) 
    return -dot_kernel_asimd(dim, x, y);
#elif defined(OPTIMIZE1)
    const uint32 n_full = dim / k_step;
    const uint32 n_tail = (dim % k_step) / k_per_iter;
    vectorize_floats acc0 = get_zero_vectors();
    vectorize_floats acc1 = get_zero_vectors();
    vectorize_floats acc2 = get_zero_vectors();
    vectorize_floats acc3 = get_zero_vectors();

    for (uint32 i = 0; i < n_full; ++i) {
        vectorize_floats vx0, vx1, vx2, vx3;
        load4(x, vx0, vx1, vx2, vx3);
        vectorize_floats vy0, vy1, vy2, vy3;
        load4(y, vy0, vy1, vy2, vy3);

        acc0 = madd_vectors(vx0, vy0, acc0);
        acc1 = madd_vectors(vx1, vy1, acc1);
        acc2 = madd_vectors(vx2, vy2, acc2);
        acc3 = madd_vectors(vx3, vy3, acc3);
    
        x += k_step;
        y += k_step;
    }

    switch (n_tail) {
        case 3u: {
            vectorize_floats vx2 = load_vectors(x + 2u * k_per_iter);
            vectorize_floats vy2 = load_vectors(y + 2u * k_per_iter);
            acc2 = madd_vectors(vx2, vy2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_floats vx1 = load_vectors(x + k_per_iter);
            vectorize_floats vy1 = load_vectors(y + k_per_iter);
            acc1 = madd_vectors(vx1, vy1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_floats vx0 = load_vectors(x);
            vectorize_floats vy0 = load_vectors(y);
            acc0 = madd_vectors(vx0, vy0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
#ifdef USE_AVX512
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        vectorize_floats vx = _mm512_maskz_load_ps(mask, x);
        vectorize_floats vy = _mm512_maskz_load_ps(mask, y);
        acc3 = _mm512_mask3_fmadd_ps(vx, vy, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
    return -_mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
#elif defined(USE_AVX)
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        alignas(32) float x_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            x_buf[i] = x[i];
        }
        alignas(32) float y_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            y_buf[i] = y[i];
        }
        vectorize_floats vx = load_vectors(x_buf);
        vectorize_floats vy = load_vectors(y_buf);
        acc3 = madd_vectors(vx, vy, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return -_mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(USE_SSE)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = _mm_add_ps(_mm_add_ps(acc0, acc1), _mm_add_ps(acc2, acc3));
    acc1 = _mm_hadd_ps(acc0, acc0);
    float res = -_mm_cvtss_f32(_mm_hadd_ps(acc1, acc1));
    switch (dim % k_per_iter) {
        case 3u: {
            res -= x[2] * y[2];
        } /* fall through */
        case 2u: {
            res -= x[1] * y[1];
        } /* fall through */
        case 1u: {
            res -= x[0] * y[0];
        } break;
    }
    return res;
#else
    static_assert(false, "Unhandled arch in x86 ip dist");
#endif
#else
    float sum = 0;
    ASSUME_ALIGNED(x);
    ASSUME_ALIGNED(y);
    for (uint32 i = 0; i < dim; ++i) {
        sum -= x[i] * y[i];
    }
    return sum;
#endif
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const float *x = (const float *)xx;
    const float *y = (const float *)yy;
#if defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__) 
    return -dot_kernel_asimd(dim, x, y);
#elif defined(OPTIMIZE1)
    const uint32 n_full = dim / k_step;
    const uint32 n_tail = (dim % k_step) / k_per_iter;
    vectorize_floats acc0 = get_zero_vectors();
    vectorize_floats acc1 = get_zero_vectors();
    vectorize_floats acc2 = get_zero_vectors();
    vectorize_floats acc3 = get_zero_vectors();

    for (uint32 i = 0; i < n_full; ++i) {
        vectorize_floats vx0, vx1, vx2, vx3;
        loadu4(x, vx0, vx1, vx2, vx3);
        vectorize_floats vy0, vy1, vy2, vy3;
        loadu4(y, vy0, vy1, vy2, vy3);

        acc0 = madd_vectors(vx0, vy0, acc0);
        acc1 = madd_vectors(vx1, vy1, acc1);
        acc2 = madd_vectors(vx2, vy2, acc2);
        acc3 = madd_vectors(vx3, vy3, acc3);
    
        x += k_step;
        y += k_step;
    }

    switch (n_tail) {
        case 3u: {
            vectorize_floats vx2 = loadu_vectors(x + 2u * k_per_iter);
            vectorize_floats vy2 = loadu_vectors(y + 2u * k_per_iter);
            acc2 = madd_vectors(vx2, vy2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_floats vx1 = loadu_vectors(x + k_per_iter);
            vectorize_floats vy1 = loadu_vectors(y + k_per_iter);
            acc1 = madd_vectors(vx1, vy1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_floats vx0 = loadu_vectors(x);
            vectorize_floats vy0 = loadu_vectors(y);
            acc0 = madd_vectors(vx0, vy0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
#ifdef USE_AVX512
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        vectorize_floats vx = _mm512_maskz_loadu_ps(mask, x);
        vectorize_floats vy = _mm512_maskz_loadu_ps(mask, y);
        acc3 = _mm512_mask3_fmadd_ps(vx, vy, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
    return -_mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
#elif defined(USE_AVX)
    const uint32 left = dim % k_per_iter;
    if (left) {
        x += k_per_iter * n_tail;
        y += k_per_iter * n_tail;
        alignas(32) float x_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            x_buf[i] = x[i];
        }
        alignas(32) float y_buf[k_per_iter] = {0};
        for (uint32 i = 0; i < left; ++i) {
            y_buf[i] = y[i];
        }
        vectorize_floats vx = load_vectors(x_buf);
        vectorize_floats vy = load_vectors(y_buf);
        acc3 = madd_vectors(vx, vy, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return -_mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(USE_SSE)
    x += k_per_iter * n_tail;
    y += k_per_iter * n_tail;
    acc0 = _mm_add_ps(_mm_add_ps(acc0, acc1), _mm_add_ps(acc2, acc3));
    acc1 = _mm_hadd_ps(acc0, acc0);
    float res = -_mm_cvtss_f32(_mm_hadd_ps(acc1, acc1));
    switch (dim % k_per_iter) {
        case 3u: {
            res -= x[2] * y[2];
        } /* fall through */
        case 2u: {
            res -= x[1] * y[1];
        } /* fall through */
        case 1u: {
            res -= x[0] * y[0];
        } break;
    }
    return res;
#else
    static_assert(false, "Unhandled arch in x86 ip dist");
#endif
#else
    float sum = 0;
    for (uint32 i = 0; i < dim; ++i) {
        sum -= x[i] * y[i];
    }
    return sum;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_COSINE, true>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, true>(x, y, dim); }
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_COSINE, false>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, false>(x, y, dim); }

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_INNER_PRODUCT, true>(
    const void *x, const void *y, uint32 dim)
{
    ASSUME_ALIGNED(x);
    ASSUME_ALIGNED(y);
    return -cblas_sdot(dim, (const float *)x, 1, (const float *)y, 1);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_INNER_PRODUCT, false>(
    const void *x, const void *y, uint32 dim)
    { return -cblas_sdot(dim, (const float *)x, 1, (const float *)y, 1); }

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_COSINE, true>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_INNER_PRODUCT, true>(x, y, dim); }
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_COSINE, false>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(distance)<Metric::FAST_BLAS_INNER_PRODUCT, false>(x, y, dim); }

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::SPHERICAL, true>(
    const void *x, const void *y, uint32 dim)
{
    float dist = -DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, true>(x, y, dim);  
    if (dist > 1) {
        dist = 1;
    } else if (dist < -1) {
        dist = -1;
    }
    return (acos(dist) / M_PI);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::SPHERICAL, false>(
    const void *x, const void *y, uint32 dim)
{
    float dist = -DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, false>(x, y, dim);  
    if (dist > 1) {
        dist = 1;
    } else if (dist < -1) {
        dist = -1;
    }
    return (acos(dist) / M_PI);
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2_SQRT, true>(
    const void *x, const void *y, uint32 dim)
{
    float dist = DISTANCE_FUNC_NAME(distance)<Metric::L2, true>(x, y, dim);
    return (sqrtf(dist));
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2_SQRT, false>(
    const void *x, const void *y, uint32 dim)
{
    float dist = DISTANCE_FUNC_NAME(distance)<Metric::L2, false>(x, y, dim);
    return (sqrtf(dist));
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2_NORM, true>(
    const void *x, const void *y, uint32 dim)
{
    float norm = -DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, true>(x, y, dim); 
    return sqrtf(norm);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(distance)<Metric::L2_NORM, false>(
    const void *x, const void *y, uint32 dim)
{
    float norm = -DISTANCE_FUNC_NAME(distance)<Metric::INNER_PRODUCT, false>(x, y, dim); 
    return sqrtf(norm);
}

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(Distancer) {
    static distance_func get_func(uint32 dim) {
        return [](const void *x, const void *y, uint32 dim) -> float {
            CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                          metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                return DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y, dim);
            }
            const uint32 d = dim - remainder;
            Assume(d % vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            float res = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y, d);
            CONSTEXPR_IF (remainder != 0) {
                const float *xx = (const float *)x;
                const float *yy = (const float *)y;
                res += DISTANCE_FUNC_NAME(distance)<metric, aligned>(xx + d, yy + d, remainder);
            }
            return res;
        };
    }
};

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(BatchDistancer) {
    static distance_func_batch get_func(uint32 dim)
    {
        return [](const void *xx, const void *yy, uint32 dim, uint32 y_size, float *out) {
            const float *x = (const float *)xx;
            const float *y = (const float *)yy;
            const uint32 d = dim - remainder;
            Assume(d % vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            for (uint32 i = 0; i != y_size; ++i) {
                CONSTEXPR_IF(max_dim <= vector_step_size) {
                    out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y, remainder);
                    y += remainder;
                } else {
                    CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                                  metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                        out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y, dim);
                    } else {
                        out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y, d);
                        CONSTEXPR_IF (remainder != 0) {
                            out[i] += DISTANCE_FUNC_NAME(distance)<metric, aligned>(x + d, y + d, remainder);
                        }
                    }
                    y += dim;
                }
                
            }
        };
    }
};

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(BatchDistancer2) {
    static distance_func_batch2 get_func(uint32 dim)
    {
        return [](const void *xx, void *const *yy, uint32 dim, uint32 y_size, float *out) {
            const float *x = (const float *)xx;
            float *const *y = (float *const *)yy;
            const uint32 d = dim - remainder;
            Assume(d % vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            for (uint32 i = 0; i != y_size; ++i) {
                CONSTEXPR_IF(max_dim <= vector_step_size) {
                    out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y[i], remainder);
                } else {
                    CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                                  metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                        out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y[i], dim);
                    } else {
                        out[i] = DISTANCE_FUNC_NAME(distance)<metric, aligned>(x, y[i], d);
                        CONSTEXPR_IF (remainder != 0) {
                            out[i] += DISTANCE_FUNC_NAME(distance)<metric, aligned>(x + d, y[i] + d, remainder);
                        }
                    }
                }
            }
        };
    }
};

template <Metric metric, bool aligned_input, uint32 min_dim, uint32 max_dim,
          template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto funcer_remainder(uint32 dim)
{
    static_assert((vector_step_size & (vector_step_size - 1)) == 0, "unsupported step size");
#define DISTANCE_FUNC_CASE(z, remainder, x)         \
    case remainder:                                 \
        return DistancerBase<metric, aligned_input, min_dim, max_dim, remainder>::get_func(dim);

    switch (dim % vector_step_size) {
        BOOST_PP_REPEAT(vector_step_size, DISTANCE_FUNC_CASE, x)
        default:
            __builtin_unreachable();
    }
#undef DISTANCE_FUNC_CASE
}

template <Metric metric, bool aligned_input, template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto funcer_dim(uint32 dim)
{
    const uint32 aligned_dim = dim - dim % vector_step_size;
    if (aligned_dim >= large_dim) {
        return funcer_remainder<metric, aligned_input, large_dim, 0xffffffff, DistancerBase>(dim);
    }
    if (aligned_dim >= medium_dim) {
        return funcer_remainder<metric, aligned_input, medium_dim, large_dim, DistancerBase>(dim);
    }
    if (aligned_dim >= small_dim) {
        return funcer_remainder<metric, aligned_input, small_dim, medium_dim, DistancerBase>(dim);
    }
    if (aligned_dim > 0) {
        return funcer_remainder<metric, aligned_input, vector_step_size, small_dim, DistancerBase>(dim);
    }
    return funcer_remainder<metric, aligned_input, 0, vector_step_size, DistancerBase>(dim);
}

template <bool aligned_input, template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto funcer_metric(Metric metric, uint32 dim)
{
    switch(metric) {
        case Metric::L2:
            return funcer_dim<Metric::L2, aligned_input, DistancerBase>(dim);
        case Metric::COSINE:
            return funcer_dim<Metric::COSINE, aligned_input, DistancerBase>(dim);
        case Metric::FAST_COSINE:
#ifdef ENABLE_OPENBLAS
            if (dim >= 136) {
                return funcer_dim<Metric::FAST_BLAS_COSINE, aligned_input, DistancerBase>(dim);
            }
#endif /* ENABLE_OPENBLAS */
            return funcer_dim<Metric::FAST_COSINE, aligned_input, DistancerBase>(dim);
        case Metric::INNER_PRODUCT:
#ifdef ENABLE_OPENBLAS
            if (dim >= 136) {
                return funcer_dim<Metric::FAST_BLAS_INNER_PRODUCT, aligned_input, DistancerBase>(dim);
            }
#endif /* ENABLE_OPENBLAS */
            return funcer_dim<Metric::INNER_PRODUCT, aligned_input, DistancerBase>(dim);
        case Metric::SPHERICAL:
            return funcer_dim<Metric::SPHERICAL, aligned_input, DistancerBase>(dim);    
        case Metric::L2_SQRT:
            return funcer_dim<Metric::L2_SQRT, aligned_input, DistancerBase>(dim);
        case Metric::L2_NORM:
            return funcer_dim<Metric::L2_NORM, aligned_input, DistancerBase>(dim);   
        default:
            __builtin_unreachable();
    }
}

distance_func DISTANCE_FUNC_NAME(funcer_metric_distancer)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return funcer_metric<true, DISTANCE_STRUCT_NAME(Distancer)>(metric, dim);
    }
    return funcer_metric<false, DISTANCE_STRUCT_NAME(Distancer)>(metric, dim);
}
distance_func_batch DISTANCE_FUNC_NAME(funcer_metric_batch)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return funcer_metric<true, DISTANCE_STRUCT_NAME(BatchDistancer)>(metric, dim);
    }
    return funcer_metric<false, DISTANCE_STRUCT_NAME(BatchDistancer)>(metric, dim);
}
distance_func_batch2 DISTANCE_FUNC_NAME(funcer_metric_batch2)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return funcer_metric<true, DISTANCE_STRUCT_NAME(BatchDistancer2)>(metric, dim);
    }
    return funcer_metric<false, DISTANCE_STRUCT_NAME(BatchDistancer2)>(metric, dim);
}
} /* namespace ann_helper */
