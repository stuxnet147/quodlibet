set(QL_REQUIRED_VENDOR_DIRECTORIES
    libuv
    yyjson
    xxhash
    blake3
    tree-sitter
    tree-sitter-c
)

if(BUILD_TESTING AND QL_BUILD_TESTS)
    list(APPEND QL_REQUIRED_VENDOR_DIRECTORIES googletest)
endif()

foreach(vendor_directory IN LISTS QL_REQUIRED_VENDOR_DIRECTORIES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${vendor_directory}/.quodlibet-vendor")
        message(FATAL_ERROR
            "Missing pinned dependency '${vendor_directory}'. "
            "Run ./scripts/vendor.sh from ${PROJECT_SOURCE_DIR}")
    endif()
endforeach()

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
add_subdirectory(libuv EXCLUDE_FROM_ALL)
add_library(ql_vendor_uv ALIAS uv_a)

add_library(ql_vendor_yyjson STATIC yyjson/src/yyjson.c)
target_include_directories(ql_vendor_yyjson PUBLIC yyjson/src)
set_target_properties(ql_vendor_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ql_vendor_xxhash STATIC xxhash/xxhash.c)
target_include_directories(ql_vendor_xxhash PUBLIC xxhash)
set_target_properties(ql_vendor_xxhash PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Artifact identities are not on the solver hot path. Start with the portable
# implementation on every architecture, then enable audited SIMD variants as a
# separate build option later.
add_library(ql_vendor_blake3 STATIC
    blake3/c/blake3.c
    blake3/c/blake3_dispatch.c
    blake3/c/blake3_portable.c
)
target_include_directories(ql_vendor_blake3 PUBLIC blake3/c)
target_compile_definitions(ql_vendor_blake3 PRIVATE
    BLAKE3_USE_NEON=0
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
)
set_target_properties(ql_vendor_blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ql_vendor_tree_sitter STATIC tree-sitter/lib/src/lib.c)
target_include_directories(ql_vendor_tree_sitter
    SYSTEM PUBLIC tree-sitter/lib/include
    PRIVATE tree-sitter/lib/src
)
target_compile_definitions(ql_vendor_tree_sitter PRIVATE
    _POSIX_C_SOURCE=200112L
    _DEFAULT_SOURCE
    _BSD_SOURCE
    _DARWIN_C_SOURCE
)
set_target_properties(ql_vendor_tree_sitter PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

add_library(ql_vendor_tree_sitter_c STATIC tree-sitter-c/src/parser.c)
target_include_directories(ql_vendor_tree_sitter_c
    SYSTEM PUBLIC tree-sitter-c/bindings/c
    PRIVATE tree-sitter-c/src
)
set_target_properties(ql_vendor_tree_sitter_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(BUILD_TESTING AND QL_BUILD_TESTS)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(googletest EXCLUDE_FROM_ALL)
endif()
