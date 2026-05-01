#ifdef __x86_64__
#include "access/annvector/distance/distance_utils.h"
#include "access/annvector/distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  AVX_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) AVX_STRUCT(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#include "../../template.cpp"
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* x86 */
