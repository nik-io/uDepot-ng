cmake_minimum_required(VERSION 3.25)
project(udepot-ng VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UDEPOT_BUILD_SPDK   "Build SPDK backend"            OFF)
option(UDEPOT_BUILD_URING  "Build io_uring backend"         ON)
option(UDEPOT_BUILD_AIO    "Build Linux AIO backend"        ON)
option(UDEPOT_BUILD_NET    "Build network/memcache server"  ON)
option(UDEPOT_BUILD_PYTHON "Build pyudepot shared library"  ON)
option(UDEPOT_BUILD_TESTS  "Build test suite"               ON)

# Vendored dependencies — built before our warning flags so -Werror
# doesn't hit vendored code.
add_subdirectory(extern/salsa)

# cityhash (git submodule — no CMakeLists.txt upstream).
add_library(cityhash STATIC extern/cityhash/src/city.cc)
target_include_directories(cityhash
    PUBLIC extern/cityhash/src
    PRIVATE extern/cityhash-config
)
set_target_properties(cityhash PROPERTIES
    CXX_STANDARD 11
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(cityhash PRIVATE -Wno-sign-compare)

# Our own warning flags — apply only to udepot targets, not vendored code.
add_compile_options(-Wall -Wextra -Werror)

add_library(udepot
    src/udepot/rcu.cc
    src/udepot/hash_table.cc
    src/udepot/directory.cc
    src/udepot/store.cc
    src/udepot/io/aio.cc
    src/udepot/io/net.cc
)
target_include_directories(udepot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:src>
)
target_compile_features(udepot PUBLIC cxx_std_23)
target_link_libraries(udepot PUBLIC salsa cityhash)

# ── SPDK backend ─────────────────────────────────────────────────────────────
if(UDEPOT_BUILD_SPDK)
    if(NOT DEFINED SPDK_DIR)
        if(DEFINED ENV{SPDK_DIR})
            set(SPDK_DIR "$ENV{SPDK_DIR}")
        else()
            set(SPDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/spdk")
        endif()
    endif()

    if(NOT EXISTS "${SPDK_DIR}/include/spdk/nvme.h")
        message(FATAL_ERROR
            "SPDK_DIR=${SPDK_DIR} does not contain include/spdk/nvme.h.\n"
            "Build SPDK first: cd extern/spdk && git submodule update --init "
            "&& ./configure && make -j$(nproc)")
    endif()

    target_sources(udepot PRIVATE src/udepot/io/spdk.cc)
    target_compile_definitions(udepot PUBLIC UDEPOT_BUILD_SPDK=1)

    target_include_directories(udepot PUBLIC
        "${SPDK_DIR}/include"
        "${SPDK_DIR}/dpdk/build/include"
    )

    # SPDK and DPDK static libraries.  The order matters for static linking.
    set(SPDK_LIBS
        "${SPDK_DIR}/build/lib/libspdk_nvme.a"
        "${SPDK_DIR}/build/lib/libspdk_env_dpdk.a"
        "${SPDK_DIR}/build/lib/libspdk_env_dpdk_rpc.a"
        "${SPDK_DIR}/build/lib/libspdk_util.a"
        "${SPDK_DIR}/build/lib/libspdk_log.a"
        "${SPDK_DIR}/build/lib/libspdk_sock.a"
        "${SPDK_DIR}/build/lib/libspdk_sock_posix.a"
        "${SPDK_DIR}/build/lib/libspdk_json.a"
        "${SPDK_DIR}/build/lib/libspdk_jsonrpc.a"
        "${SPDK_DIR}/build/lib/libspdk_rpc.a"
        "${SPDK_DIR}/build/lib/libspdk_trace.a"
        "${SPDK_DIR}/build/lib/libspdk_vfio_user.a"
    )

    set(DPDK_LIBS
        "${SPDK_DIR}/dpdk/build/lib/librte_eal.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_mempool.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_mempool_ring.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_ring.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_pci.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_bus_pci.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_kvargs.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_telemetry.a"
        "${SPDK_DIR}/dpdk/build/lib/librte_log.a"
    )

    target_link_libraries(udepot PUBLIC
        ${SPDK_LIBS}
        ${DPDK_LIBS}
        pthread dl rt numa uuid
    )
endif()

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
        set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    endfunction()

    udepot_add_test(coro_test       test/coro_test.cc)
    udepot_add_test(rcu_test        test/rcu_test.cc)
    udepot_add_test(buffer_test     test/buffer_test.cc)
    udepot_add_test(posix_test      test/io/posix_test.cc)
    udepot_add_test(hash_table_test test/hash_table_test.cc)
    udepot_add_test(directory_test  test/directory_test.cc)
    udepot_add_test(aio_test        test/io/aio_test.cc)
    udepot_add_test(store_test      test/store_test.cc)
    udepot_add_test(aio_store_test  test/aio_store_test.cc)
    udepot_add_test(concurrent_store_test test/concurrent_store_test.cc)
    udepot_add_test(concurrent_aio_store_test test/concurrent_aio_store_test.cc)
    udepot_add_test(aio_queue_depth_test test/aio_queue_depth_test.cc)
    set_tests_properties(aio_queue_depth_test PROPERTIES TIMEOUT 120)
    udepot_add_test(net_test test/io/net_test.cc)

    if(UDEPOT_BUILD_SPDK)
        udepot_add_test(spdk_test test/io/spdk_test.cc)
        udepot_add_test(spdk_store_test test/spdk_store_test.cc)
        set_tests_properties(spdk_test PROPERTIES TIMEOUT 120)
        set_tests_properties(spdk_store_test PROPERTIES TIMEOUT 120)
    endif()

    # Perf benchmark binary (not a gtest — standalone executable).
    add_executable(udepot_ng_bench test/perf/udepot_ng_bench.cc)
    target_link_libraries(udepot_ng_bench PRIVATE udepot)

    # Legacy uDepot benchmark for the perf regression gate.
    # Requires UDEPOT_ROOT pointing to a built uDepot tree with libudepot.a.
    if(NOT DEFINED UDEPOT_ROOT)
        if(DEFINED ENV{UDEPOT_ROOT})
            set(UDEPOT_ROOT "$ENV{UDEPOT_ROOT}")
        else()
            set(UDEPOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../uDepot")
        endif()
    endif()

    if(EXISTS "${UDEPOT_ROOT}/libudepot.a")
        add_executable(udepot_legacy_bench test/perf/udepot_legacy_bench.cc)
        target_include_directories(udepot_legacy_bench SYSTEM PRIVATE
            ${UDEPOT_ROOT}/src/include
            ${UDEPOT_ROOT}/salsa/src/include
            ${UDEPOT_ROOT}/trt/src
            ${UDEPOT_ROOT}/external/cityhash/..
            ${UDEPOT_ROOT}/trt/external/liburing/src/include
        )
        target_link_libraries(udepot_legacy_bench PRIVATE
            ${UDEPOT_ROOT}/libudepot.a
            ${UDEPOT_ROOT}/external/cityhash/src/.libs/libcityhash.a
            ${UDEPOT_ROOT}/trt/external/liburing/src/liburing.a
            pthread rt z
        )
        target_compile_options(udepot_legacy_bench PRIVATE -Wno-error)
    else()
        message(STATUS "UDEPOT_ROOT=${UDEPOT_ROOT} — libudepot.a not found, "
                       "skipping legacy benchmark build")
    endif()
endif()
