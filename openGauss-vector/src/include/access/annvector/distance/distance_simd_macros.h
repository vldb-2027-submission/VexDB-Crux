/**
 * Copyright ...
 * Definitions for distance macros
 */

#ifndef DISTANCE_MACROS_H
#define DISTANCE_MACROS_H

#ifdef USE_AVX512
#undef USE_AVX512
#endif
#ifdef USE_AVX
#undef USE_AVX
#endif
#ifdef USE_SSE
#undef USE_SSE
#endif

#ifdef __AVX512_SUPPORT__
#define USE_AVX512
#elif defined(__AVX_SUPPORT__)
#define USE_AVX
#elif defined(__SSE_SUPPORT__)
#define USE_SSE
#endif
#if defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
#include <arm_neon.h>
#endif

#if defined(USE_AVX512) || defined(USE_AVX)
#include <immintrin.h>
#endif
#ifdef USE_SSE
#include <immintrin.h>
#include <xmmintrin.h>
#endif

#ifdef USE_AVX512
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 16u;
constexpr uint32 half_k_per_iter = 16u;
using vectorize_floats = __m512;
using vectorize_half_repr = vectorize_floats;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm512_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm512_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm512_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm512_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm512_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm512_fmadd_ps(a, b, s); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return _mm512_setzero_ps(); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v)
    { return _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)v)); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
    { return _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)v)); }
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm512_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm512_add_ps(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return _mm512_fmadd_ps(a, b, s); }
#elif defined(USE_AVX)
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 8u;
constexpr uint32 half_k_per_iter = 8u;
using vectorize_floats = __m256;
using vectorize_half_repr = vectorize_floats;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm256_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm256_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm256_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm256_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm256_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm256_fmadd_ps(a, b, s); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return _mm256_setzero_ps(); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v)
    { return _mm256_cvtph_ps(_mm_load_si128((const __m128i *)v)); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
    { return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)v)); }
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm256_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm256_add_ps(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return _mm256_fmadd_ps(a, b, s); }
#elif defined(USE_SSE)
#define OPTIMIZE1
constexpr uint32 k_per_iter = 4u;
using vectorize_floats = __m128;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm_fmadd_ps(a, b, s); }
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 4u;
constexpr uint32 half_k_per_iter = 8u;
using vectorize_floats = float32x4_t;
using vectorize_half_repr = float16x8_t;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return vdupq_n_f32(0.0f); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return vld1q_f32(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v)
{
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    return vld1q_f32(v);
#else
    float32x4_t result;
    memcpy(&result, v, sizeof(result));
    return result;
#endif
}
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return vsubq_f32(a, b); }
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return vaddq_f32(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return vmlaq_f32(s, a, b); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return vdupq_n_f16((float16_t)0.0f); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v) { return vld1q_f16(v); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
{
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    return vld1q_f16(v);
#else
    vectorize_half_repr result;
    memcpy(&result, v, sizeof(result));
    return result;
#endif
}
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return vsubq_f16(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return vaddq_f16(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return vfmaq_f16(a, b, s); }
#endif

#ifdef OPTIMIZE1
constexpr uint32 k_unroll = 4u;
constexpr uint32 k_step = k_per_iter * k_unroll;
#endif

#ifdef OPTIMIZE_HALF
constexpr uint32 half_k_step = half_k_per_iter * k_unroll;
#endif

#endif /* DISTANCE_MACROS_H */
