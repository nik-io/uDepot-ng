cmake_minimum_required(VERSION 3.25)

# Build salsa as a static library from the original C/C++ sources.
# No modifications to the salsa source files.

set(SALSA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SALSA_INCLUDE_DIR ${SALSA_SRC_DIR}/include)

add_library(salsa STATIC
    # C sources
    ${SALSA_SRC_DIR}/util/parse-storage-args.c
    ${SALSA_SRC_DIR}/util/scm_usr_helpers.c
    ${SALSA_SRC_DIR}/sto-ctlr/sto-capacity-mgr.c
    ${SALSA_SRC_DIR}/sto-ctlr/scm-dev-properties.c
    ${SALSA_SRC_DIR}/sto-ctlr/scm-seg-queues.c
    ${SALSA_SRC_DIR}/sto-ctlr/scm-seg-alloc.c
    ${SALSA_SRC_DIR}/sto-ctlr/scm-throttler.c
    ${SALSA_SRC_DIR}/gc/gc-common.c
    ${SALSA_SRC_DIR}/gc/gc-base.c
    ${SALSA_SRC_DIR}/gc/gc-nbin.c
    ${SALSA_SRC_DIR}/libos/os-thread.c
    ${SALSA_SRC_DIR}/frontends/usalsa++/allocator.c
    # C++ sources
    ${SALSA_SRC_DIR}/frontends/usalsa++/Scm.cc
    ${SALSA_SRC_DIR}/frontends/usalsa++/SalsaCtlr.cc
)

target_include_directories(salsa PUBLIC
    ${SALSA_INCLUDE_DIR}
    ${SALSA_INCLUDE_DIR}/data-structures
    ${SALSA_INCLUDE_DIR}/generic
    ${SALSA_SRC_DIR}/frontends/usalsa++
)

# Mark salsa headers as SYSTEM for consumers so -Werror doesn't hit
# vendored code when included from udepot sources.
target_include_directories(salsa SYSTEM INTERFACE
    ${SALSA_INCLUDE_DIR}
    ${SALSA_INCLUDE_DIR}/data-structures
    ${SALSA_INCLUDE_DIR}/generic
    ${SALSA_SRC_DIR}/frontends/usalsa++
)

target_compile_definitions(salsa PRIVATE
    _GNU_SOURCE
    __STDC_FORMAT_MACROS
)

set_target_properties(salsa PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 11
    POSITION_INDEPENDENT_CODE ON
)

# Salsa's own warnings — match the original Makefile but don't impose
# -Werror on vendored code (it was written for older compilers).
target_compile_options(salsa PRIVATE
    -Wall -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:C>:-Wshadow>
)

target_link_libraries(salsa PRIVATE pthread rt)
