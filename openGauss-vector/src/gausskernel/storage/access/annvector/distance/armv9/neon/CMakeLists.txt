#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_distance_arm9neon_SRC)

set(TGT_distance_arm9neon_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH}
)

set(TGT_distance_arm9neon_opt_options
    -ftree-vectorize
    -fassociative-math
    -fno-signed-zeros
    -fno-trapping-math
    -march=armv9-a+simd
    -floop-interchange
    -Wno-volatile
    -Wno-register
    -fno-exceptions
    -std=c++17
    -g1
)

set(distance_arm9neon_DEF_OPTIONS ${MACRO_OPTIONS})
set(distance_arm9neon_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS} ${TGT_distance_arm9neon_opt_options})
set(distance_arm9neon_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_annvector_distance_arm9neon TGT_distance_arm9neon_SRC TGT_distance_arm9neon_INC "${distance_arm9neon_DEF_OPTIONS}" "${distance_arm9neon_COMPILE_OPTIONS}" "${distance_arm9neon_LINK_OPTIONS}")
