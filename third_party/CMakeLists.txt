set(QL_REQUIRED_VENDOR_DIRECTORIES
    libuv
    yyjson
    xxhash
    blake3
    tree-sitter
    tree-sitter-c
    zf_log
)

# zf_log has no vendor target on purpose. src/log.c defines the library's
# configuration macros and then includes the pinned zf_log.c as part of its own
# translation unit, so the logging engine inherits Quodlibet's warning set,
# visibility and sanitizer flags without a second compile step. Listing the
# directory here keeps a missing checkout a configure-time error.

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

# Artifact identity turned out to be on the solver hot path after all. Every
# judgement copies the private Bitwuzla snapshot and hashes its 4.49 MB four
# times, at creation, at the version probe, and before and during the check
# (src/solver.c). Portable-only spent 16.1 ms per judgement there; the vendored
# x86-64 assembly spends 3.5 ms. docs/perf/baseline.md holds the measurement.
#
# This does not weaken the identity it computes. BLAKE3 is defined by the
# algorithm, not by which kernel evaluates it, and the two builds were measured
# producing the same digest for the same input rather than assumed to:
# 0a989102d9dc2892826c4ae4d6148814b123a2dabe4e723bb1bd68dac57f94a9.
#
# blake3_dispatch.c selects a kernel from CPUID at run time, so a machine
# without AVX-512 simply does not take that path. Architectures and drivers
# that cannot assemble the vendored .S files keep the portable build.
set(ql_blake3_sources
    blake3/c/blake3.c
    blake3/c/blake3_dispatch.c
    blake3/c/blake3_portable.c
)
set(ql_blake3_definitions BLAKE3_USE_NEON=0)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Xx]86_64|AMD64|amd64)$"
        AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    enable_language(ASM)
    if(WIN32)
        set(ql_blake3_asm_abi "windows_gnu")
    else()
        set(ql_blake3_asm_abi "unix")
    endif()
    list(APPEND ql_blake3_sources
        blake3/c/blake3_sse2_x86-64_${ql_blake3_asm_abi}.S
        blake3/c/blake3_sse41_x86-64_${ql_blake3_asm_abi}.S
        blake3/c/blake3_avx2_x86-64_${ql_blake3_asm_abi}.S
        blake3/c/blake3_avx512_x86-64_${ql_blake3_asm_abi}.S
    )
else()
    list(APPEND ql_blake3_definitions
        BLAKE3_NO_SSE2
        BLAKE3_NO_SSE41
        BLAKE3_NO_AVX2
        BLAKE3_NO_AVX512
    )
endif()

add_library(ql_vendor_blake3 STATIC ${ql_blake3_sources})
target_include_directories(ql_vendor_blake3 PUBLIC blake3/c)
target_compile_definitions(ql_vendor_blake3 PRIVATE ${ql_blake3_definitions})
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

# --- SAT backend and proof checker -------------------------------------------
#
# The AIG/SAT path exists to shrink what a PROVED verdict trusts, so its two
# programs are built here as ordinary executables and invoked as isolated
# processes. Neither is linked into libquodlibet: CaDiCaL is C++ and the core
# is C17, and a checker that shared an address space with the thing it checks
# would be checking itself.
#
# Kissat was the recorded candidate and was disqualified by measurement, not by
# preference. docs/notes/sat-backend-and-proof-checker.md records why.
option(QL_ENABLE_SAT "Build the pinned CaDiCaL backend and LRAT checker" ON)

if(QL_ENABLE_SAT)
    foreach(sat_directory IN ITEMS cadical drat-trim)
        if(NOT EXISTS
           "${CMAKE_CURRENT_SOURCE_DIR}/${sat_directory}/.quodlibet-vendor")
            message(FATAL_ERROR
                "Missing pinned dependency '${sat_directory}'. "
                "Run ./scripts/vendor.sh from ${PROJECT_SOURCE_DIR}")
        endif()
    endforeach()

    # CaDiCaL's build normally generates this from git metadata. A vendored
    # checkout has none, and a build identity that varied per machine would
    # make the recorded backend identity useless anyway.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cadical-build/build.hpp"
        "#define VERSION \"2.2.1\"\n"
        "#define IDENTIFIER \"quodlibet-vendored\"\n"
        "#define COMPILER \"${CMAKE_CXX_COMPILER_ID}\"\n"
        "#define FLAGS \"\"\n"
        "#define DATE \"pinned\"\n")

    # mobical is CaDiCaL's own model-based tester, not part of the solver.
    file(GLOB ql_cadical_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src/*.c")
    list(FILTER ql_cadical_sources EXCLUDE REGEX "mobical\.cpp$")
    list(SORT ql_cadical_sources)

    add_executable(ql_cadical ${ql_cadical_sources})
    target_include_directories(ql_cadical SYSTEM PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/cadical-build"
        "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src")
    set_target_properties(ql_cadical PROPERTIES
        OUTPUT_NAME cadical
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)

    add_executable(ql_lrat_check
        "${CMAKE_CURRENT_SOURCE_DIR}/drat-trim/lrat-check.c")
    set_target_properties(ql_lrat_check PROPERTIES
        OUTPUT_NAME lrat-check
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)
    # getClause and setClause are file-scope C99 `inline`, which promises an
    # external definition somewhere and the file provides none. An optimising
    # build inlines every call and never asks for it; a build that suppresses
    # inlining, which is what the sanitizer presets do, fails to link. That is
    # a latent defect in the pinned source, not in how it is used here.
    #
    # The pinned bytes stay as they were checksummed, so the two definitions
    # are given internal linkage from the command line instead of being edited.
    # `inline` appears in this file exactly twice, on those two functions, so
    # the substitution reaches nothing else.
    if(NOT MSVC)
        target_compile_options(ql_lrat_check PRIVATE "-Dinline=static inline")
    endif()

    foreach(sat_target IN ITEMS ql_cadical ql_lrat_check)
        if(MSVC)
            target_compile_options(${sat_target} PRIVATE /w)
        else()
            target_compile_options(${sat_target} PRIVATE -w)
        endif()
        if(WIN32)
            # The pinned sources are unmodified. Windows lacks four POSIX
            # headers and a handful of names they use, so those are supplied
            # rather than patched in; see quodlibet-compat/README.md.
            target_include_directories(${sat_target} SYSTEM BEFORE PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/quodlibet-compat")
            target_compile_options(${sat_target} PRIVATE
                -include "${CMAKE_CURRENT_SOURCE_DIR}/quodlibet-compat/ql_sat_prelude.h")
            # Windows defaults to a 1 MB stack; the solver's recursion on a
            # large instance needs more, and running out of it looks like a
            # wrong answer rather than a crash at the wrong moment.
            target_link_options(${sat_target} PRIVATE "LINKER:/STACK:33554432")
        endif()
    endforeach()
    if(WIN32)
        # CaDiCaL reads its own resident set size through the Win32 API.
        target_compile_definitions(ql_cadical PRIVATE NUNLOCKED)
        target_link_libraries(ql_cadical PRIVATE psapi)
    endif()

    set(QL_CADICAL_EXECUTABLE "$<TARGET_FILE:ql_cadical>"
        CACHE STRING "Path to the CaDiCaL 2.2.1 executable" FORCE)
    set(QL_LRAT_CHECK_EXECUTABLE "$<TARGET_FILE:ql_lrat_check>"
        CACHE STRING "Path to the LRAT checker executable" FORCE)

    # The core library is created after this directory is processed, and the
    # root CMakeLists.txt is owned by the integrator. Deferring the call to the
    # end of the top-level scope adds the two paths without editing that file.
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
        CALL target_compile_definitions quodlibet PRIVATE
            "QL_CADICAL_EXECUTABLE=\"$<TARGET_FILE:ql_cadical>\""
            "QL_LRAT_CHECK_EXECUTABLE=\"$<TARGET_FILE:ql_lrat_check>\"")
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
        CALL add_dependencies quodlibet ql_cadical ql_lrat_check)
endif()
