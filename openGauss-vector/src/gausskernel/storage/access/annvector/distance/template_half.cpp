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
#include "access/annvector/halfvec.h"
#include "access/annvector/halfutils.h"
#include "access/annvector/distance/distance_simd_macros.h"

#define ASSUME_HALF_ALIGNED(v) v = (const half *)__builtin_assume_aligned(v, vector_aligned_size)

namespace ann_helper {
using namespace internal;

#ifdef OPTIMIZE_HALF
static void load4(const half *v, vectorize_half_repr &x0, vectorize_half_repr &x1,
                  vectorize_half_repr &x2, vectorize_half_repr &x3)
{
    x0 = load_halfs(v);
    x1 = load_halfs(v + half_k_per_iter);
    x2 = load_halfs(v + half_k_per_iter * 2u);
    x3 = load_halfs(v + half_k_per_iter * 3u);
}
static void loadu4(const half *v, vectorize_half_repr &x0, vectorize_half_repr &x1,
                  vectorize_half_repr &x2, vectorize_half_repr &x3)
{
    x0 = loadu_halfs(v);
    x1 = loadu_halfs(v + half_k_per_iter);
    x2 = loadu_halfs(v + half_k_per_iter * 2u);
    x3 = loadu_halfs(v + half_k_per_iter * 3u);
}
#endif

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;

    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();
	for (uint32 i = 0; i < n_full; ++i) {
		vectorize_half_repr x0, x1, x2, x3;
        load4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        load4(y, y0, y1, y2, y3);

        vectorize_half_repr diff0 = sub_halfs(x0, y0);
        vectorize_half_repr diff1 = sub_halfs(x1, y1);
        vectorize_half_repr diff2 = sub_halfs(x2, y2);
        vectorize_half_repr diff3 = sub_halfs(x3, y3);

        acc0 = madd_halfs(diff0, diff0, acc0);
        acc1 = madd_halfs(diff1, diff1, acc1);
        acc2 = madd_halfs(diff2, diff2, acc2);
        acc3 = madd_halfs(diff3, diff3, acc3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = load_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = load_halfs(y + half_k_per_iter * 2u);
            vectorize_half_repr diff2 = sub_halfs(x2, y2);
            acc2 = madd_halfs(diff2, diff2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = load_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = load_halfs(y + half_k_per_iter);
            vectorize_half_repr diff1 = sub_halfs(x1, y1);
            acc1 = madd_halfs(diff1, diff1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = load_halfs(x);
            vectorize_half_repr y0 = load_halfs(y);
            vectorize_half_repr diff0 = sub_halfs(x0, y0);
            acc0 = madd_halfs(diff0, diff0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        vectorize_floats diff3 = _mm512_sub_ps(vxs, vys);
        acc3 = _mm512_mask3_fmadd_ps(diff3, diff3, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
    return _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        vectorize_floats diff3 = sub_vectors(vxs, vys);
        acc3 = _mm256_fmadd_ps(diff3, diff3, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(16) half y_buf[8] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        float16x8_t y_left = vld1q_f16(y_buf);
        float16x8_t diff0 = vsubq_f16(x_left, y_left);
        acc0 = vfmaq_f16(acc0, diff0, diff0);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float32x4_t low = vcvt_f32_f16(vget_low_f16(acc0));
    float32x4_t high = vcvt_f32_f16(vget_high_f16(acc0));
    return (vaddvq_f32(low) + vaddvq_f32(high));
#endif
#else
    float distance = 0.0;
    ASSUME_HALF_ALIGNED(x);
    ASSUME_HALF_ALIGNED(y);
    for (uint32 i = 0; i < dim; ++i) {
        float diff = HalfToFloat4(x[i]) - HalfToFloat4(y[i]);
        distance += diff * diff;
    }
    return distance;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;

    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();
	for (uint32 i = 0; i < n_full; ++i) {
		vectorize_half_repr x0, x1, x2, x3;
        loadu4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        loadu4(y, y0, y1, y2, y3);

        vectorize_half_repr diff0 = sub_halfs(x0, y0);
        vectorize_half_repr diff1 = sub_halfs(x1, y1);
        vectorize_half_repr diff2 = sub_halfs(x2, y2);
        vectorize_half_repr diff3 = sub_halfs(x3, y3);

        acc0 = madd_halfs(diff0, diff0, acc0);
        acc1 = madd_halfs(diff1, diff1, acc1);
        acc2 = madd_halfs(diff2, diff2, acc2);
        acc3 = madd_halfs(diff3, diff3, acc3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = loadu_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = loadu_halfs(y + half_k_per_iter * 2u);
            vectorize_half_repr diff2 = sub_halfs(x2, y2);
            acc2 = madd_halfs(diff2, diff2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = loadu_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = loadu_halfs(y + half_k_per_iter);
            vectorize_half_repr diff1 = sub_halfs(x1, y1);
            acc1 = madd_halfs(diff1, diff1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = loadu_halfs(x);
            vectorize_half_repr y0 = loadu_halfs(y);
            vectorize_half_repr diff0 = sub_halfs(x0, y0);
            acc0 = madd_halfs(diff0, diff0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        vectorize_floats diff3 = _mm512_sub_ps(vxs, vys);
        acc3 = _mm512_mask3_fmadd_ps(diff3, diff3, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
   return _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        vectorize_floats diff3 = sub_vectors(vxs, vys);
        acc3 = _mm256_fmadd_ps(diff3, diff3, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        alignas(16) half y_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        float16x8_t y_left = vld1q_f16(y_buf);
        float16x8_t diff0 = vsubq_f16(x_left, y_left);
        acc0 = vfmaq_f16(acc0, diff0, diff0);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float32x4_t low = vcvt_f32_f16(vget_low_f16(acc0));
    float32x4_t high = vcvt_f32_f16(vget_high_f16(acc0));
    return (vaddvq_f32(low) + vaddvq_f32(high));
#endif
#else
    float distance = 0.0;
    for (uint32 i = 0; i < dim; ++i) {
        float diff = HalfToFloat4(x[i]) - HalfToFloat4(y[i]);
        distance += diff * diff;
    }
    return distance;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::COSINE, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF 
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;

    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();

    vectorize_half_repr nx0 = get_zero_halfs();
    vectorize_half_repr nx1 = get_zero_halfs();
    vectorize_half_repr nx2 = get_zero_halfs();
    vectorize_half_repr nx3 = get_zero_halfs();

    vectorize_half_repr ny0 = get_zero_halfs();
    vectorize_half_repr ny1 = get_zero_halfs();
    vectorize_half_repr ny2 = get_zero_halfs();
    vectorize_half_repr ny3 = get_zero_halfs();

	for (uint32 i = 0; i < n_full; ++i) {
        vectorize_half_repr x0, x1, x2, x3;
        load4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        load4(y, y0, y1, y2, y3);

        acc0 = madd_halfs(x0, y0, acc0);
        acc1 = madd_halfs(x1, y1, acc1);
        acc2 = madd_halfs(x2, y2, acc2);
        acc3 = madd_halfs(x3, y3, acc3);

        nx0 = madd_halfs(x0, x0, nx0);
        ny0 = madd_halfs(y0, y0, ny0);
        nx1 = madd_halfs(x1, x1, nx1);
        ny1 = madd_halfs(y1, y1, ny1);
        nx2 = madd_halfs(x2, x2, nx2);
        ny2 = madd_halfs(y2, y2, ny2);
        nx3 = madd_halfs(x3, x3, nx3);
        ny3 = madd_halfs(y3, y3, ny3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = load_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = load_halfs(y + half_k_per_iter * 2u);
            acc2 = madd_halfs(x2, y2, acc2);
            nx2 = madd_halfs(x2, x2, nx2);
            ny2 = madd_halfs(y2, y2, ny2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = load_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = load_halfs(y + half_k_per_iter);
            acc1 = madd_halfs(x1, y1, acc1);
            nx1 = madd_halfs(x1, x1, nx1);
            ny1 = madd_halfs(y1, y1, ny1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = load_halfs(x);
            vectorize_half_repr y0 = load_halfs(y);
            acc0 = madd_halfs(x0, y0, acc0);
            nx0 = madd_halfs(x0, x0, nx0);
            ny0 = madd_halfs(y0, y0, ny0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        acc3 = _mm512_mask3_fmadd_ps(vxs, vys, acc3, mask);
        nx3 = _mm512_mask3_fmadd_ps(vxs, vxs, nx3, mask);
        ny3 = _mm512_mask3_fmadd_ps(vys, vys, ny3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
    float dot = _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
    nx0 = _mm512_add_ps(nx0, nx1);
    nx2 = _mm512_add_ps(nx2, nx3);
    float nx = _mm512_reduce_add_ps(_mm512_add_ps(nx0, nx2));
    ny0 = _mm512_add_ps(ny0, ny1);
    ny2 = _mm512_add_ps(ny2, ny3);
    float ny = _mm512_reduce_add_ps(_mm512_add_ps(ny0, ny2));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        acc3 = _mm256_fmadd_ps(vxs, vys, acc3);
        nx3 = _mm256_fmadd_ps(vxs, vxs, nx3);
        ny3 = _mm256_fmadd_ps(vys, vys, ny3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    float dot = _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));

    nx0 = _mm256_add_ps(_mm256_add_ps(nx0, nx1), _mm256_add_ps(nx2, nx3));
    __m128 sum128_nx = _mm_add_ps(_mm256_extractf128_ps(nx0, 1), _mm256_castps256_ps128(nx0));
    sum128_nx = _mm_hadd_ps(sum128_nx, sum128_nx);
    float nx = _mm_cvtss_f32(_mm_hadd_ps(sum128_nx, sum128_nx));

    ny0 = _mm256_add_ps(_mm256_add_ps(ny0, ny1), _mm256_add_ps(ny2, ny3));
    __m128 sum128_ny = _mm_add_ps(_mm256_extractf128_ps(ny0, 1), _mm256_castps256_ps128(ny0));
    sum128_ny = _mm_hadd_ps(sum128_ny, sum128_ny);
    float ny = _mm_cvtss_f32(_mm_hadd_ps(sum128_ny, sum128_ny));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(16) half y_buf[8] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        float16x8_t y_left = vld1q_f16(y_buf);
        acc0 = vfmaq_f16(acc0, x_left, y_left);
        nx0 = vfmaq_f16(nx0, x_left, x_left);
        ny0 = vfmaq_f16(ny0, y_left, y_left);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float dot = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(acc0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(acc0))));

    nx0 = vaddq_f16(vaddq_f16(nx0, nx1), vaddq_f16(nx2, nx3));
    float nx = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(nx0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(nx0))));

    ny0 = vaddq_f16(vaddq_f16(ny0, ny1), vaddq_f16(ny2, ny3));
    float ny = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(ny0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(ny0))));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#endif
#else
    float dot = 0;
    float norm_x = 0;
    float norm_y = 0;
    ASSUME_HALF_ALIGNED(x);
    ASSUME_HALF_ALIGNED(y);
    for (uint32 i = 0; i < dim; i++) {
        float xf = HalfToFloat4(x[i]);
        float yf = HalfToFloat4(y[i]);
        dot += xf * yf;
        norm_x += xf * xf;
        norm_y += yf * yf;
    }
    return -dot / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
#endif    
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::COSINE, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;

    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();

    vectorize_half_repr nx0 = get_zero_halfs();
    vectorize_half_repr nx1 = get_zero_halfs();
    vectorize_half_repr nx2 = get_zero_halfs();
    vectorize_half_repr nx3 = get_zero_halfs();

    vectorize_half_repr ny0 = get_zero_halfs();
    vectorize_half_repr ny1 = get_zero_halfs();
    vectorize_half_repr ny2 = get_zero_halfs();
    vectorize_half_repr ny3 = get_zero_halfs();

	for (uint32 i = 0; i < n_full; ++i) {
        vectorize_half_repr x0, x1, x2, x3;
        loadu4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        loadu4(y, y0, y1, y2, y3);

        acc0 = madd_halfs(x0, y0, acc0);
        acc1 = madd_halfs(x1, y1, acc1);
        acc2 = madd_halfs(x2, y2, acc2);
        acc3 = madd_halfs(x3, y3, acc3);

        nx0 = madd_halfs(x0, x0, nx0);
        ny0 = madd_halfs(y0, y0, ny0);
        nx1 = madd_halfs(x1, x1, nx1);
        ny1 = madd_halfs(y1, y1, ny1);
        nx2 = madd_halfs(x2, x2, nx2);
        ny2 = madd_halfs(y2, y2, ny2);
        nx3 = madd_halfs(x3, x3, nx3);
        ny3 = madd_halfs(y3, y3, ny3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = loadu_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = loadu_halfs(y + half_k_per_iter * 2u);
            acc2 = madd_halfs(x2, y2, acc2);
            nx2 = madd_halfs(x2, x2, nx2);
            ny2 = madd_halfs(y2, y2, ny2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = loadu_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = loadu_halfs(y + half_k_per_iter);
            acc1 = madd_halfs(x1, y1, acc1);
            nx1 = madd_halfs(x1, x1, nx1);
            ny1 = madd_halfs(y1, y1, ny1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = loadu_halfs(x);
            vectorize_half_repr y0 = loadu_halfs(y);
            acc0 = madd_halfs(x0, y0, acc0);
            nx0 = madd_halfs(x0, x0, nx0);
            ny0 = madd_halfs(y0, y0, ny0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        acc3 = _mm512_mask3_fmadd_ps(vxs, vys, acc3, mask);
        nx3 = _mm512_mask3_fmadd_ps(vxs, vxs, nx3, mask);
        ny3 = _mm512_mask3_fmadd_ps(vys, vys, ny3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
    float dot = _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
    nx0 = _mm512_add_ps(nx0, nx1);
    nx2 = _mm512_add_ps(nx2, nx3);
    float nx = _mm512_reduce_add_ps(_mm512_add_ps(nx0, nx2));
    ny0 = _mm512_add_ps(ny0, ny1);
    ny2 = _mm512_add_ps(ny2, ny3);
    float ny = _mm512_reduce_add_ps(_mm512_add_ps(ny0, ny2));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        acc3 = _mm256_fmadd_ps(vxs, vys, acc3);
        nx3 = _mm256_fmadd_ps(vxs, vxs, nx3);
        ny3 = _mm256_fmadd_ps(vys, vys, ny3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    float dot = _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));

    nx0 = _mm256_add_ps(_mm256_add_ps(nx0, nx1), _mm256_add_ps(nx2, nx3));
    __m128 sum128_nx = _mm_add_ps(_mm256_extractf128_ps(nx0, 1), _mm256_castps256_ps128(nx0));
    sum128_nx = _mm_hadd_ps(sum128_nx, sum128_nx);
    float nx = _mm_cvtss_f32(_mm_hadd_ps(sum128_nx, sum128_nx));

    ny0 = _mm256_add_ps(_mm256_add_ps(ny0, ny1), _mm256_add_ps(ny2, ny3));
    __m128 sum128_ny = _mm_add_ps(_mm256_extractf128_ps(ny0, 1), _mm256_castps256_ps128(ny0));
    sum128_ny = _mm_hadd_ps(sum128_ny, sum128_ny);
    float ny = _mm_cvtss_f32(_mm_hadd_ps(sum128_ny, sum128_ny));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        alignas(16) half y_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t y_left = vld1q_f16(y_buf);
        acc0 = vfmaq_f16(acc0, x_left, y_left);
        nx0 = vfmaq_f16(nx0, x_left, x_left);
        ny0 = vfmaq_f16(ny0, y_left, y_left);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float dot = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(acc0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(acc0))));

    nx0 = vaddq_f16(vaddq_f16(nx0, nx1), vaddq_f16(nx2, nx3));
    float nx = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(nx0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(nx0))));

    ny0 = vaddq_f16(vaddq_f16(ny0, ny1), vaddq_f16(ny2, ny3));
    float ny = (vaddvq_f32(vcvt_f32_f16(vget_low_f16(ny0))) +
        vaddvq_f32(vcvt_f32_f16(vget_high_f16(ny0))));
    return -dot / (sqrtf(nx * ny) + __FLT_EPSILON__);
#endif
#else
    float dot = 0;
    float norm_x = 0;
    float norm_y = 0;
    for (uint32 i = 0; i < dim; i++) {
        float xf = HalfToFloat4(x[i]);
        float yf = HalfToFloat4(y[i]);
        dot += xf * yf;
        norm_x += xf * xf;
        norm_y += yf * yf;
    }
    return -dot / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, true>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;
    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();
	for (uint32 i = 0; i < n_full; ++i) {
		vectorize_half_repr x0, x1, x2, x3;
        load4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        load4(y, y0, y1, y2, y3);

        acc0 = madd_halfs(x0, y0, acc0);
        acc1 = madd_halfs(x1, y1, acc1);
        acc2 = madd_halfs(x2, y2, acc2);
        acc3 = madd_halfs(x3, y3, acc3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = load_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = load_halfs(y + half_k_per_iter * 2u);
            acc2 = madd_halfs(x2, y2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = load_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = load_halfs(y + half_k_per_iter);
            acc1 = madd_halfs(x1, y1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = load_halfs(x);
            vectorize_half_repr y0 = load_halfs(y);
            acc0 = madd_halfs(x0, y0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        acc3 = _mm512_mask3_fmadd_ps(vxs, vys, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
   return -_mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));

#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        acc3 = _mm256_fmadd_ps(vxs, vys, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return -_mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        alignas(16) half y_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        float16x8_t y_left = vld1q_f16(y_buf);
        acc0 = vfmaq_f16(acc0, x_left, y_left);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float32x4_t low = vcvt_f32_f16(vget_low_f16(acc0));
    float32x4_t high = vcvt_f32_f16(vget_high_f16(acc0));
    return -(vaddvq_f32(low) + vaddvq_f32(high));
#endif
#else
    float distance = 0;
    ASSUME_HALF_ALIGNED(x);
    ASSUME_HALF_ALIGNED(y);
    for (uint32 i = 0; i < dim; ++i) {
        distance -= HalfToFloat4(x[i]) * HalfToFloat4(y[i]);
    }
    return distance;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, false>(
    const void *xx, const void *yy, uint32 dim)
{
    const half *x = (const half *)xx;
    const half *y = (const half *)yy;
#ifdef OPTIMIZE_HALF 
    const uint32 n_full = dim / (half_k_step);
    const uint32 n_tail = (dim % half_k_step) / half_k_per_iter;
    vectorize_half_repr acc0 = get_zero_halfs();
    vectorize_half_repr acc1 = get_zero_halfs();
    vectorize_half_repr acc2 = get_zero_halfs();
    vectorize_half_repr acc3 = get_zero_halfs();
	for (uint32 i = 0; i < n_full; ++i) {
		vectorize_half_repr x0, x1, x2, x3;
        loadu4(x, x0, x1, x2, x3);
        vectorize_half_repr y0, y1, y2, y3;
        loadu4(y, y0, y1, y2, y3);

        acc0 = madd_halfs(x0, y0, acc0);
        acc1 = madd_halfs(x1, y1, acc1);
        acc2 = madd_halfs(x2, y2, acc2);
        acc3 = madd_halfs(x3, y3, acc3);

        x += half_k_step;
        y += half_k_step;
	}

    switch (n_tail) {
        case 3u: {
            vectorize_half_repr x2 = loadu_halfs(x + half_k_per_iter * 2u);
            vectorize_half_repr y2 = loadu_halfs(y + half_k_per_iter * 2u);
            acc2 = madd_halfs(x2, y2, acc2);
        } /* fall through */
        case 2u: {
            vectorize_half_repr x1 = loadu_halfs(x + half_k_per_iter);
            vectorize_half_repr y1 = loadu_halfs(y + half_k_per_iter);
            acc1 = madd_halfs(x1, y1, acc1);
        } /* fall through */
        case 1u: {
            vectorize_half_repr x0 = loadu_halfs(x);
            vectorize_half_repr y0 = loadu_halfs(y);
            acc0 = madd_halfs(x0, y0, acc0);
        } /* fall through */
        case 0u:
            /* do nothing */
            break;
    }
    const uint32 left = dim % half_k_per_iter;
#ifdef USE_AVX512
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        __mmask16 mask = (__mmask16)((1u << left) - 1u);
        __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
        __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
        __m512 vxs = _mm512_cvtph_ps(vx);
        __m512 vys = _mm512_cvtph_ps(vy);
        acc3 = _mm512_mask3_fmadd_ps(vxs, vys, acc3, mask);
    }

    acc0 = _mm512_add_ps(acc0, acc1);
    acc2 = _mm512_add_ps(acc2, acc3);
   return -_mm512_reduce_add_ps(_mm512_add_ps(acc0, acc2));
#elif defined(USE_AVX)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(32) half x_buf[half_k_per_iter] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        alignas(32) half y_buf[half_k_per_iter] = {0};
        memcpy(y_buf, y, left * sizeof(half));
        __m128i vx = _mm_load_si128((__m128i *)x_buf);
        __m128i vy = _mm_load_si128((__m128i *)y_buf);
        __m256	vxs = _mm256_cvtph_ps(vx);
        __m256	vys = _mm256_cvtph_ps(vy);
        acc3 = _mm256_fmadd_ps(vxs, vys, acc3);
    }

    acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(acc0, 1), _mm256_castps256_ps128(acc0));
    sum128 = _mm_hadd_ps(sum128, sum128);
    return -_mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));    
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
    if (left) {
        x += half_k_per_iter * n_tail;
        y += half_k_per_iter * n_tail;
        alignas(16) half x_buf[8] = {0};
        alignas(16) half y_buf[8] = {0};
        memcpy(x_buf, x, left * sizeof(half));
        memcpy(y_buf, y, left * sizeof(half));
        float16x8_t x_left = vld1q_f16(x_buf);
        float16x8_t y_left = vld1q_f16(y_buf);
        acc0 = vfmaq_f16(acc0, x_left, y_left);
    }

    acc0 = vaddq_f16(vaddq_f16(acc0, acc1), vaddq_f16(acc2, acc3));
    float32x4_t low = vcvt_f32_f16(vget_low_f16(acc0));
    float32x4_t high = vcvt_f32_f16(vget_high_f16(acc0));
    return -(vaddvq_f32(low) + vaddvq_f32(high));
#endif
#else
    float distance = 0;
    for (uint32 i = 0; i < dim; ++i) {
        distance -= HalfToFloat4(x[i]) * HalfToFloat4(y[i]);
    }
    return distance;
#endif
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::FAST_COSINE, true>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, true>(x, y, dim); }
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::FAST_COSINE, false>(
    const void *x, const void *y, uint32 dim)
    { return DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, false>(x, y, dim); }

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::SPHERICAL, true>(
    const void *x, const void *y, uint32 dim)
{
    float dist = -DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, true>(x, y, dim);  
    if (dist > 1) {
        dist = 1;
    } else if (dist < -1) {
        dist = -1;
    }
    return (acos(dist) / M_PI);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::SPHERICAL, false>(
    const void *x, const void *y, uint32 dim)
{
    float dist = -DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, false>(x, y, dim);  
    if (dist > 1) {
        dist = 1;
    } else if (dist < -1) {
        dist = -1;
    }
    return (acos(dist) / M_PI);
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2_SQRT, true>(
    const void *x, const void *y, uint32 dim)
{
    float dist = DISTANCE_FUNC_NAME(half_distance)<Metric::L2, true>(x, y, dim);
    return (sqrtf(dist));
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2_SQRT, false>(
    const void *x, const void *y, uint32 dim)
{
    float dist = DISTANCE_FUNC_NAME(half_distance)<Metric::L2, false>(x, y, dim);
    return (sqrtf(dist));
}

template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2_NORM, true>(
    const void *x, const void *y, uint32 dim)
{
    float norm = -DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, true>(x, y, dim);  
    return sqrtf(norm);
}
template <>
INLINE_PROP float DISTANCE_FUNC_NAME(half_distance)<Metric::L2_NORM, false>(
    const void *x, const void *y, uint32 dim)
{
    float norm = -DISTANCE_FUNC_NAME(half_distance)<Metric::INNER_PRODUCT, false>(x, y, dim);  
    return sqrtf(norm);
}

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(HalfDistancer) {
    static distance_func get_func(uint32 dim)
    {
        return [](const void *x, const void *y, uint32 dim) -> float {
            CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                          metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                return DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y, dim);
            }
            const uint32 d = dim - remainder;
            Assume(d % half_vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            float res = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y, d);
            CONSTEXPR_IF (remainder != 0) {
                const half *xx = (const half *)x;
                const half *yy = (const half *)y;
                res += DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(xx + d, yy + d, remainder);
            }
            return res;
        };
    }
};

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(BatchHalfDistancer) {
    static distance_func_batch get_func(uint32 dim)
    {
        return [](const void *xx, const void *yy, uint32 dim, uint32 y_size, float *out) {
            const half *x = (const half *)xx;
            const half *y = (const half *)yy;
            const uint32 d = dim - remainder;
            Assume(d % vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            for (uint32 i = 0; i != y_size; ++i) {
                CONSTEXPR_IF(max_dim <= vector_step_size) {
                    out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y, remainder);
                    y += remainder;
                } else {
                    CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                                  metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                        out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y, dim);
                    } else {
                        out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y, d);
                        CONSTEXPR_IF (remainder != 0) {
                            out[i] += DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x + d, y + d, remainder);
                        }
                    }
                    y += dim;
                }
                
            }
        };
    }
};

template <Metric metric, bool aligned, uint32 min_dim, uint32 max_dim, uint32 remainder>
struct DISTANCE_STRUCT_NAME(BatchHalfDistancer2) {
    static distance_func_batch2 get_func(uint32 dim)
    {
        return [](const void *xx, void *const *yy, uint32 dim, uint32 y_size, float *out) {
            const half *x = (const half *)xx;
            half *const *y = (half *const *)yy;
            const uint32 d = dim - remainder;
            Assume(d % vector_step_size == 0);
            Assume(d >= min_dim);
            Assume(d < max_dim);
            for (uint32 i = 0; i != y_size; ++i) {
                CONSTEXPR_IF(max_dim <= vector_step_size) {
                    out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y[i], remainder);
                } else {
                    CONSTEXPR_IF (metric == Metric::COSINE || metric == Metric::SPHERICAL ||
                                  metric == Metric::L2_SQRT || metric == Metric::L2_NORM) {
                        out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y[i], dim);
                    } else {
                        out[i] = DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x, y[i], d);
                        CONSTEXPR_IF (remainder != 0) {
                            out[i] += DISTANCE_FUNC_NAME(half_distance)<metric, aligned>(x + d, y[i] + d, remainder);
                        }
                    }
                }
            }
        };
    }
};

template <Metric metric, bool aligned_input, uint32 min_dim, uint32 max_dim,
          template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto half_funcer_remainder(uint32 dim)
{
    static_assert((half_vector_step_size & (half_vector_step_size - 1)) == 0, "unsupported step size");
#define DISTANCE_FUNC_CASE(z, remainder, x)         \
    case remainder:                                 \
        return DistancerBase<metric, aligned_input, min_dim, max_dim, remainder>::get_func(dim);

    switch (dim % half_vector_step_size) {
        BOOST_PP_REPEAT(half_vector_step_size, DISTANCE_FUNC_CASE, x)
        default:
            __builtin_unreachable();
    }
#undef DISTANCE_FUNC_CASE
}

template <Metric metric, bool aligned_input, template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto half_funcer_dim(uint32 dim)
{
    const uint32 aligned_dim = dim - dim % half_vector_step_size;
    if (aligned_dim >= half_large_dim) {
        return half_funcer_remainder<metric, aligned_input, half_large_dim, 0xffffffff, DistancerBase>(dim);
    }
    if (aligned_dim >= half_medium_dim) {
        return half_funcer_remainder<metric, aligned_input, half_medium_dim, half_large_dim, DistancerBase>(dim);
    }
    if (aligned_dim >= half_small_dim) {
        return half_funcer_remainder<metric, aligned_input, half_small_dim, half_medium_dim, DistancerBase>(dim);
    }
    if (aligned_dim > 0) {
        return half_funcer_remainder<metric, aligned_input, half_vector_step_size, half_small_dim, DistancerBase>(dim);
    }
    return half_funcer_remainder<metric, aligned_input, 0, half_vector_step_size, DistancerBase>(dim);
}

template <bool aligned_input, template <Metric, bool, uint32, uint32, uint32> class DistancerBase>
static auto half_funcer_metric(Metric metric, uint32 dim)
{
    switch(metric) {
        case Metric::L2:
            return half_funcer_dim<Metric::L2, aligned_input, DistancerBase>(dim);
        case Metric::COSINE:
            return half_funcer_dim<Metric::COSINE, aligned_input, DistancerBase>(dim);
        case Metric::FAST_COSINE:
            return half_funcer_dim<Metric::FAST_COSINE, aligned_input, DistancerBase>(dim);
        case Metric::INNER_PRODUCT:
            return half_funcer_dim<Metric::INNER_PRODUCT, aligned_input, DistancerBase>(dim);
        case Metric::SPHERICAL:
            return half_funcer_dim<Metric::SPHERICAL, aligned_input, DistancerBase>(dim);    
        case Metric::L2_SQRT:
            return half_funcer_dim<Metric::L2_SQRT, aligned_input, DistancerBase>(dim);
        case Metric::L2_NORM:
            return half_funcer_dim<Metric::L2_NORM, aligned_input, DistancerBase>(dim);   
        default:
            __builtin_unreachable();
    }
}

distance_func DISTANCE_FUNC_NAME(funcer_metric_half_distancer)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return half_funcer_metric<true, DISTANCE_STRUCT_NAME(HalfDistancer)>(metric, dim);
    }
    return half_funcer_metric<false, DISTANCE_STRUCT_NAME(HalfDistancer)>(metric, dim);
}

distance_func_batch DISTANCE_FUNC_NAME(funcer_metric_half_batch)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return half_funcer_metric<true, DISTANCE_STRUCT_NAME(BatchHalfDistancer)>(metric, dim);
    }
    return half_funcer_metric<false, DISTANCE_STRUCT_NAME(BatchHalfDistancer)>(metric, dim);
}

distance_func_batch2 DISTANCE_FUNC_NAME(funcer_metric_half_batch2)(Metric metric, bool aligned, uint32 dim)
{
    if (aligned) {
        return half_funcer_metric<true, DISTANCE_STRUCT_NAME(BatchHalfDistancer2)>(metric, dim);
    }
    return half_funcer_metric<false, DISTANCE_STRUCT_NAME(BatchHalfDistancer2)>(metric, dim);
}
} /* namespace ann_helper */
