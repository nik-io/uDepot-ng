cmake_minimum_required(VERSION 3.25)

add_library(cityhash STATIC city.cc)
target_include_directories(cityhash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(cityhash PROPERTIES
    CXX_STANDARD 11
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(cityhash PRIVATE -Wno-sign-compare)
