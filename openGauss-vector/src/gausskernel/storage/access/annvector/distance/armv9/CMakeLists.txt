SET(CMAKE_VERBOSE_MAKEFILE ON) #显示详细的原始编译信息
SET(CMAKE_RULE_MESSAGES OFF)   #减少一些不必要的输出
SET(CMAKE_SKIP_RPATH TRUE)     #TRUE表示在编译及安装阶段忽略RPATH

set(CMAKE_MODULE_PATH 
    ${CMAKE_CURRENT_SOURCE_DIR}/neon
    ${CMAKE_CURRENT_SOURCE_DIR}/sve
    ${CMAKE_CURRENT_SOURCE_DIR}/sve2
    ${CMAKE_CURRENT_SOURCE_DIR}/sme
    ${CMAKE_CURRENT_SOURCE_DIR}/sme2
)

add_subdirectory(neon)
add_subdirectory(sve)
add_subdirectory(sve2)
add_subdirectory(sme)
add_subdirectory(sme2)
