set(QL_REQUIRED_VENDOR_DIRECTORIES
    libuv
    yyjson
    xxhash
    blake3
    tree-sitter
    tree-sitter-c
)

set(QL_BITWUZLA_BUNDLED FALSE)
if(QL_ENABLE_BITWUZLA)
    set(ql_bitwuzla_bundle_directory "")
    set(ql_bitwuzla_bundle_executable "")
    set(ql_bitwuzla_bundle_sha256 "")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        if(WIN32)
            set(ql_bitwuzla_bundle_directory bitwuzla-windows-x86_64)
            set(ql_bitwuzla_bundle_executable
                "${CMAKE_CURRENT_SOURCE_DIR}/${ql_bitwuzla_bundle_directory}/bin/bitwuzla.exe")
            set(ql_bitwuzla_bundle_sha256
                ce880e5402abc0df542b8c1f9f65e57ba9353690848d3f2d69054ff2f29cc796)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(ql_bitwuzla_bundle_directory bitwuzla-linux-x86_64)
            set(ql_bitwuzla_bundle_executable
                "${CMAKE_CURRENT_SOURCE_DIR}/${ql_bitwuzla_bundle_directory}/bin/bitwuzla")
            set(ql_bitwuzla_bundle_sha256
                d98164badcd34c12ccbbd9e5aab9373854bb187e79f99ccda4ec2aa9951c0eab)
        endif()
    endif()

    if("${QL_BITWUZLA_EXECUTABLE}" STREQUAL "")
        if("${ql_bitwuzla_bundle_executable}" STREQUAL "")
            message(FATAL_ERROR
                "No pinned Bitwuzla bundle exists for this target; set "
                "QL_BITWUZLA_EXECUTABLE to a runnable Bitwuzla 0.9.1 binary")
        endif()
        set(QL_BITWUZLA_EXECUTABLE "${ql_bitwuzla_bundle_executable}"
            CACHE FILEPATH "Override path to a Bitwuzla 0.9.1 executable" FORCE)
    endif()

    if("${QL_BITWUZLA_EXECUTABLE}" STREQUAL
       "${ql_bitwuzla_bundle_executable}")
        set(QL_BITWUZLA_BUNDLED TRUE)
        list(APPEND QL_REQUIRED_VENDOR_DIRECTORIES
            "${ql_bitwuzla_bundle_directory}")
    endif()
endif()

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

if(QL_ENABLE_BITWUZLA)
    if(NOT EXISTS "${QL_BITWUZLA_EXECUTABLE}")
        message(FATAL_ERROR
            "Missing Bitwuzla executable: ${QL_BITWUZLA_EXECUTABLE}")
    endif()
    if(QL_BITWUZLA_BUNDLED)
        file(SHA256 "${QL_BITWUZLA_EXECUTABLE}" ql_bitwuzla_actual_sha256)
        if(NOT ql_bitwuzla_actual_sha256 STREQUAL ql_bitwuzla_bundle_sha256)
            message(FATAL_ERROR
                "Bitwuzla executable checksum mismatch: ${QL_BITWUZLA_EXECUTABLE}")
        endif()
    endif()
    execute_process(
        COMMAND "${QL_BITWUZLA_EXECUTABLE}" --version
        RESULT_VARIABLE ql_bitwuzla_version_result
        OUTPUT_VARIABLE ql_bitwuzla_version
        ERROR_VARIABLE ql_bitwuzla_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        TIMEOUT 10)
    if(NOT ql_bitwuzla_version_result EQUAL 0 OR
       NOT ql_bitwuzla_version STREQUAL "0.9.1")
        message(FATAL_ERROR
            "Bitwuzla 0.9.1 is not runnable: ${QL_BITWUZLA_EXECUTABLE}\n"
            "stdout: ${ql_bitwuzla_version}\n"
            "stderr: ${ql_bitwuzla_version_error}")
    endif()
endif()

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
