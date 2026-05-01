#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_distance_avx512_SRC)

set(TGT_distance_avx512_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH}
)

set(TGT_distance_avx512_opt_options
    -ftree-vectorize
    -fassociative-math
    -fno-signed-zeros
    -fno-trapping-math
    -mavx512f
    -mavx
    -mavx2
    -mfma
    -floop-interchange
    -Wno-volatile
    -Wno-register
    -fno-exceptions
    -std=c++17
    -g1
)

set(distance_avx512_DEF_OPTIONS ${MACRO_OPTIONS})
set(distance_avx512_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS} ${TGT_distance_avx512_opt_options})
set(distance_avx512_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_annvector_distance_avx512 TGT_distance_avx512_SRC TGT_distance_avx512_INC "${distance_avx512_DEF_OPTIONS}" "${distance_avx512_COMPILE_OPTIONS}" "${distance_avx512_LINK_OPTIONS}")
