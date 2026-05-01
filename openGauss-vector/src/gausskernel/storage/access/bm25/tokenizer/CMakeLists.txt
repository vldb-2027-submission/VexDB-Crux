#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_bm25_tokenizer_SRC)

set(TGT_bm25_tokenizer_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${PROJECT_SRC_DIR}/include/libcomm
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH} 
)

set(bm25_tokenizer_DEF_OPTIONS ${MACRO_OPTIONS})
set(bm25_tokenizer_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(bm25_tokenizer_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_bm25_tokenizer TGT_bm25_tokenizer_SRC TGT_bm25_tokenizer_INC "${bm25_tokenizer_DEF_OPTIONS}" "${bm25_tokenizer_COMPILE_OPTIONS}" "${bm25_tokenizer_LINK_OPTIONS}")
