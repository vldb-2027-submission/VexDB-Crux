#if (defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64) && __GNUC__ >= 12)
#include "access/annvector/distance/distance.h"
#include "access/annvector/distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  SMEV9_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) SMEV9_STRUCT(name)
#define __NEON_SUPPORT__
#define __SVE_SUPPORT__
#define __SVE2_SUPPORT__
#define __SME_SUPPORT__
#include "../../template.cpp"
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* arm */
