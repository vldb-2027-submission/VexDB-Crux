#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
#include "access/annvector/distance/distance_utils.h"
#include "access/annvector/distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  NEONV8_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) NEONV8_STRUCT(name)
#define __NEON_SUPPORT__
#include "../../template.cpp"
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* arm */
