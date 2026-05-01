#This is the main CMAKE for build bin.
set(CMAKE_CXX_STANDARD 17)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_annvector_xlog_SRC)

set(TGT_annvector_xlog_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH} 
)

set(TGT_xlog_opt_options
    -Wno-volatile
    -Wno-register
    -fno-exceptions
)

set(annvector_xlog_DEF_OPTIONS ${MACRO_OPTIONS})
set(annvector_xlog_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS} ${TGT_xlog_opt_options})
set(annvector_xlog_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_annvector_xlog TGT_annvector_xlog_SRC TGT_annvector_xlog_INC "${annvector_xlog_DEF_OPTIONS}" "${annvector_xlog_COMPILE_OPTIONS}" "${annvector_xlog_LINK_OPTIONS}")
