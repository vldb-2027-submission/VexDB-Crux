#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_distance_SRC)

set(TGT_distance_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH}
    ${BOOST186_INCLUDE_PATH}
)

set(distance_DEF_OPTIONS ${MACRO_OPTIONS})
set(distance_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(distance_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_annvector_distance TGT_distance_SRC TGT_distance_INC "${distance_DEF_OPTIONS}" "${distance_COMPILE_OPTIONS}" "${distance_LINK_OPTIONS}")

add_subdirectory(general)
EXECUTE_PROCESS( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE )
if("${ARCHITECTURE}" STREQUAL "x86_64")
    add_subdirectory(x86)
elseif("${ARCHITECTURE}" STREQUAL "aarch64" )
    add_subdirectory(armv8)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12.0")
        add_subdirectory(armv9)
    endif()
else()
    message( FATAL_ERROR "Found unsupported platform ${ARCHITECTURE}" )
endif()

