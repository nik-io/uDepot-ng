cmake_minimum_required(VERSION 3.25)
project(udepot-ng VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

option(UDEPOT_BUILD_SPDK   "Build SPDK backend"            OFF)
option(UDEPOT_BUILD_URING  "Build io_uring backend"         ON)
option(UDEPOT_BUILD_AIO    "Build Linux AIO backend"        ON)
option(UDEPOT_BUILD_NET    "Build network/memcache server"  ON)
option(UDEPOT_BUILD_PYTHON "Build pyudepot shared library"  ON)
option(UDEPOT_BUILD_TESTS  "Build test suite"               ON)

add_library(udepot
    src/rcu.cc
    src/hash_table.cc
)
target_include_directories(udepot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(udepot PUBLIC cxx_std_23)

if(UDEPOT_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    function(udepot_add_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE udepot GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    udepot_add_test(coro_test       test/coro_test.cc)
    udepot_add_test(rcu_test        test/rcu_test.cc)
    udepot_add_test(buffer_test     test/buffer_test.cc)
    udepot_add_test(posix_test      test/io/posix_test.cc)
    udepot_add_test(hash_table_test test/hash_table_test.cc)
endif()
