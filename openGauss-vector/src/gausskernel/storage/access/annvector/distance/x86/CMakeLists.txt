SET(CMAKE_VERBOSE_MAKEFILE ON) #显示详细的原始编译信息
SET(CMAKE_RULE_MESSAGES OFF)   #减少一些不必要的输出
SET(CMAKE_SKIP_RPATH TRUE)     #TRUE表示在编译及安装阶段忽略RPATH

set(CMAKE_MODULE_PATH 
    ${CMAKE_CURRENT_SOURCE_DIR}/avx
    ${CMAKE_CURRENT_SOURCE_DIR}/avx512
    ${CMAKE_CURRENT_SOURCE_DIR}/sse
)

add_subdirectory(avx)
add_subdirectory(avx512)
add_subdirectory(sse)
