#This is the CMAKE for build ut_demo components.
set(TGT_ut_demo_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/ut_demo.cpp
        )

INCLUDE_DIRECTORIES(
        ${PROJECT_SRC_DIR}/include
)
add_executable(ut_demo_vexdb ${TGT_ut_demo_SRC})
TARGET_LINK_LIBRARIES(ut_demo_vexdb ${UNIT_TEST_BASE_LIB_LIST})

target_compile_options(ut_demo_vexdb PRIVATE ${OPTIMIZE_LEVEL})
target_link_options(ut_demo_vexdb PRIVATE ${UNIT_TEST_LINK_OPTIONS_LIB_LIST})
add_custom_command(TARGET ut_demo_vexdb
        POST_BUILD
        COMMAND mkdir -p ${CMAKE_BINARY_DIR}/ut_bin
        COMMAND rm -rf ${CMAKE_BINARY_DIR}/ut_bin/ut_demo_vexdb
        COMMAND cp ${CMAKE_BINARY_DIR}/${vexdb}/src/test/ut/demo/ut_demo_vexdb ${CMAKE_BINARY_DIR}/ut_bin/ut_demo_vexdb
        COMMAND chmod +x ${CMAKE_BINARY_DIR}/ut_bin/ut_demo_vexdb
        )
# convenient to test
add_custom_target(ut_demo_test
        DEPENDS ut_demo_vexdb
        COMMAND ${CMAKE_BINARY_DIR}/ut_bin/ut_demo_vexdb || sleep 0
        COMMENT "begin unit test..."
        )
