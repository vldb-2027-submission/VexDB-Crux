/**
 * Copyright ...
 * Definitions for distance names
 */

#ifndef DISTANCE_UTILS_H
#define DISTANCE_UTILS_H

#include <boost/preprocessor/seq.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/preprocessor/arithmetic/add.hpp>
#include <boost/preprocessor/control/if.hpp>

#include "c.h"
#include "access/annvector/distance/distance.h"
#include "access/annvector/floatvector.h"

#define NEONV8_FUNC(name) neonv8_##name
#define SVEV8_FUNC(name) svev8_##name
#define SVE2V8_FUNC(name) sve2v8_##name
#define NEONV9_FUNC(name) neonv9_##name
#define SVEV9_FUNC(name) svev9_##name
#define SVE2V9_FUNC(name) sve2v9_##name
#define SMEV9_FUNC(name) smev9_##name
#define SME2V9_FUNC(name) sme2v9_##name
#define SSE_FUNC(name) sse_##name
#define AVX_FUNC(name) avx_##name
#define AVX512_FUNC(name) avx512_##name
#define GENERAL_FUNC(name) genernal_##name
#define NEONV8_STRUCT(name) NeonV8##name
#define SVEV8_STRUCT(name) SveV8##name
#define SVE2V8_STRUCT(name) Sve2V8##name
#define NEONV9_STRUCT(name) NeonV9##name
#define SVEV9_STRUCT(name) SveV9##name
#define SVE2V9_STRUCT(name) Sve2V9##name
#define SMEV9_STRUCT(name) SmeV9##name
#define SME2V9_STRUCT(name) Sme2V9##name
#define SSE_STRUCT(name) Sse##name
#define AVX_STRUCT(name) Avx##name
#define AVX512_STRUCT(name) Avx512##name
#define GENERAL_STRUCT(name) Genernal##name

#define max_vector_bottom_dim 14
namespace detail {
/* n cannot be 0 */
constexpr inline uint32 floor_log2(uint32 n)
{
    uint32 res = 0;
    constexpr uint32 a[] = {16, 8, 4, 2, 1};
    for (uint32 off : a) {
        if (n >= (1u << off)) {
            n >>= off;
            res += off;
        }
    }
    return res;
}
static_assert(floor_log2(FLOATVECTOR_MAX_DIM) <= max_vector_bottom_dim,
    "incorrect max_vector_bottom_dim");
}   /* detail */

#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
#if __GNUC__ >= 12
#define ARM_ISAS (NEONV8)(SVEV8)(SVE2V8)(NEONV9)(SVEV9)(SVE2V9)(SMEV9)(SME2V9)
#else
#define ARM_ISAS (NEONV8)(SVEV8)(SVE2V8)
#endif
#else
#define ARM_ISAS
#endif
#ifdef __x86_64__
#define X86_ISAS (SSE)(AVX)(AVX512)
#else
#define X86_ISAS
#endif
#define GENERAL_ISAS (GENERAL)

#define DECL_DISTANCE(z, data, isa) \
    template <Metric metric, bool aligned> \
    float BOOST_PP_CAT(isa, _FUNC(distance))(const void *x, const void *y, uint32 dim);   \
    template <Metric metric, bool aligned> \
    float BOOST_PP_CAT(isa, _FUNC(half_distance))(const void *x, const void *y, uint32 dim);   \
    void BOOST_PP_CAT(isa, _FUNC(fvec_inner_products_ny))(float *dis, const float *x,   \
        const float *y, uint32 d, uint32 ny);   \
    void BOOST_PP_CAT(isa, _FUNC(fvec_L2sqr_ny))(float *dis, const float *x,    \
        const float *y, uint32 d, uint32 ny);   \
    uint32 BOOST_PP_CAT(isa, _FUNC(fvec_L2sqr_ny_nearest))(float *distances_tmp_buffer, \
        const float *x, const float *y, uint32 d, uint32 ny);
#define DECL_DISTANCE2(z, data, isa) \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_g))( \
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_8))( \
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_16))(\
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_g))(   \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);\
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_8))(   \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);\
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_16))(  \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);
#define DECL_DISTANCER(z, data, isa) \
    ann_helper::distance_func BOOST_PP_CAT(isa, _FUNC(funcer_metric_distancer))(Metric metric, bool aligned, uint32 dim); \
    ann_helper::distance_func_batch BOOST_PP_CAT(isa, _FUNC(funcer_metric_batch))(Metric metric, bool aligned, uint32 dim); \
    ann_helper::distance_func_batch2 BOOST_PP_CAT(isa, _FUNC(funcer_metric_batch2))(Metric metric, bool aligned, uint32 dim); \
    ann_helper::distance_func BOOST_PP_CAT(isa, _FUNC(funcer_metric_half_distancer))(Metric metric, bool aligned, uint32 dim); \
    ann_helper::distance_func_batch BOOST_PP_CAT(isa, _FUNC(funcer_metric_half_batch))(Metric metric, bool aligned, uint32 dim); \
    ann_helper::distance_func_batch2 BOOST_PP_CAT(isa, _FUNC(funcer_metric_half_batch2))(Metric metric, bool aligned, uint32 dim);

#define DECL_DISTANCE3_HELPER(z, i, isa)  \
    void BOOST_PP_CAT(isa, _FUNC(fht_helper_##i))(float *buf);
#define DECL_DISTANCE3(z, data, isa) \
    BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), DECL_DISTANCE3_HELPER, isa)  \
    void BOOST_PP_CAT(isa, _FUNC(flip_sign))(const uint8 *flip, float *data, size_t dim);   \
    void BOOST_PP_CAT(isa, _FUNC(kacs_walk))(float *data, size_t len);  \
    float BOOST_PP_CAT(isa, _FUNC(warmup_ip_x0_q))(uint64 *data, const uint64 *query, float delta, float vl, size_t dim);  \
    float BOOST_PP_CAT(isa, _FUNC(ip_fxi))(float *query, uint8 *data, size_t dim);  \
    float BOOST_PP_CAT(isa, _FUNC(mask_ip_x0_q))(float *query, uint64 *data, size_t dim);

#define GENERATE_ISA_DECLARATIONS(r, data, isa) \
    DECL_DISTANCE(_, _, isa)    \
    DECL_DISTANCE2(_, _, isa)   \
    DECL_DISTANCER(_, _, isa)   \
    DECL_DISTANCE3(_, _, isa)

namespace ann_helper {
enum class Arch {
    NEONV8, SVEV8, SVE2V8, NEONV9, SVEV9, SVE2V9, SMEV9, SME2V9,    /* arm */
    SSE, AVX, AVX512,   /* x86 */
    GENERAL
};
Arch get_best_arch();
BOOST_PP_SEQ_FOR_EACH(GENERATE_ISA_DECLARATIONS, _, ARM_ISAS)
BOOST_PP_SEQ_FOR_EACH(GENERATE_ISA_DECLARATIONS, _, X86_ISAS)
BOOST_PP_SEQ_FOR_EACH(GENERATE_ISA_DECLARATIONS, _, GENERAL_ISAS)
} /* namespace ann_helper */

#undef DECL_DISTANCE3_HELPER
#undef DECL_DISTANCE3_HELPER2
#undef DECL_DISTANCE3_HELPER3
#undef DECL_DISTANCE
#undef DECL_DISTANCE2
#undef DECL_DISTANCER
#undef DECL_DISTANCE3
#undef GENERATE_ISA_DECLARATIONS

#if __GNUC__ >= 10
#define INLINE_PROP FORCE_INLINE
#else
#define INLINE_PROP FORCE_INLINE
#endif

#endif /* DISTANCE_UTILS_H */
