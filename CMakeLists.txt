cmake_minimum_required(VERSION 3.21)

project(quodlibet
    VERSION 0.1.0
    DESCRIPTION "Composable native function-equivalence engine"
    LANGUAGES C
)

include(CTest)
include(GNUInstallDirs)

option(QL_BUILD_SHARED "Build libquodlibet as a shared library" OFF)
option(QL_BUILD_TESTS "Build native tests" ON)
option(QL_ENABLE_BITWUZLA "Enable the canonical Bitwuzla SMT backend" ON)
option(QL_ENABLE_LOGGING "Compile the logging service" ON)
option(QL_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
set(QL_BITWUZLA_EXECUTABLE "" CACHE FILEPATH
    "Override path to a Bitwuzla 0.9.1 executable")

if(BUILD_TESTING AND QL_BUILD_TESTS)
    enable_language(CXX)
endif()

# The tree contains a Python shared module that folds in the core and every
# vendored archive, so all objects must be PIC in every preset. 8ddd5e7 set
# this only on the standalone pip path and linux-sanitize kept failing at the
# libuv link (W8's report, 2026-08-10).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(third_party)

set(QL_LIBRARY_TYPE STATIC)
if(QL_BUILD_SHARED)
    set(QL_LIBRARY_TYPE SHARED)
endif()

set(QL_CORE_SOURCES
    src/allocator.c
    src/artifact.c
    src/builtins.c
    src/c_syntax.c
    src/hash.c
    src/method.c
    src/pipeline.c
    src/plugin.c
    src/registry.c
    src/scheduler.c
    src/semantics.c
    src/status.c
    src/version.c
)

# These modules are developed independently and may not exist in every source
# snapshot. CONFIGURE_DEPENDS makes an existing build tree notice when a module
# appears, while exact path patterns keep unrelated scratch C files out of the
# production library.
#
# Names are reserved here before the module exists so that parallel workstreams
# never have to edit this list at the same time. A reserved path that has no
# file is simply absent from the glob result.
file(GLOB QL_OPTIONAL_CORE_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/src/aig.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/budget.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cache.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_frontend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_lower.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_lower_expr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_lower_stmt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_types.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/combine.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/egraph.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/egraph_check.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ir.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ir_interp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ir_verify.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/policy.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/precondition.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/problem.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/product.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proof_aigsat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proof_diff.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proof_method.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proof_smt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/replay.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/signature.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/solver.c"
)
list(APPEND QL_CORE_SOURCES ${QL_OPTIONAL_CORE_SOURCES})

add_library(quodlibet ${QL_LIBRARY_TYPE} ${QL_CORE_SOURCES})
add_library(quodlibet::quodlibet ALIAS quodlibet)

target_include_directories(quodlibet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(quodlibet PRIVATE QL_BUILDING_LIBRARY)
if(QL_ENABLE_BITWUZLA)
    target_compile_definitions(quodlibet PRIVATE
        QL_BITWUZLA_EXECUTABLE="${QL_BITWUZLA_EXECUTABLE}")
endif()
if(NOT QL_ENABLE_LOGGING)
    # src/log.c keeps the whole public API callable in this configuration and
    # compiles no engine, so a caller's logging call sites still build. The
    # definition is private because only the implementation reads it.
    target_compile_definitions(quodlibet PRIVATE QL_ENABLE_LOGGING=0)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(quodlibet PRIVATE _POSIX_C_SOURCE=200112L)
endif()
if(QL_BUILD_SHARED)
    target_compile_definitions(quodlibet PUBLIC QL_BUILD_SHARED)
endif()
target_link_libraries(quodlibet
    PRIVATE
        ql_vendor_uv
        ql_vendor_yyjson
        ql_vendor_xxhash
        ql_vendor_blake3
        ql_vendor_tree_sitter
        ql_vendor_tree_sitter_c
)
set_target_properties(quodlibet PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
if(WIN32 AND NOT QL_BUILD_SHARED)
    # A PE executable can emit an import library. Keep the CLI's public
    # `quodlibet.exe` name without letting its `quodlibet.lib` overwrite the
    # static core library before the tests link.
    set_target_properties(quodlibet PROPERTIES OUTPUT_NAME quodlibet_static)
endif()

if(MSVC)
    target_compile_options(quodlibet PRIVATE /W4 /permissive-)
else()
    target_compile_options(quodlibet PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_executable(quodlibet_cli cli/main.c cli/coverage.c)
set_target_properties(quodlibet_cli PROPERTIES OUTPUT_NAME quodlibet)
target_link_libraries(quodlibet_cli PRIVATE quodlibet::quodlibet)
target_include_directories(quodlibet_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli)

if(QL_ENABLE_BITWUZLA)
    if(WIN32)
        set(QL_BITWUZLA_RUNTIME_NAME bitwuzla.exe)
    else()
        set(QL_BITWUZLA_RUNTIME_NAME bitwuzla)
    endif()
    add_custom_command(TARGET quodlibet_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QL_BITWUZLA_EXECUTABLE}"
            "$<TARGET_FILE_DIR:quodlibet_cli>/${QL_BITWUZLA_RUNTIME_NAME}"
        VERBATIM)
endif()

# W4 owns bindings/python. That directory decides for itself whether a CPython
# 3.11+ interpreter with the abi3 headers exists; without one it builds nothing
# and says so at configure time instead of registering a skipping test.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/CMakeLists.txt")
    add_subdirectory(bindings/python)
endif()

if(BUILD_TESTING AND QL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(QL_BUILD_FUZZERS)
    # Every tests/fuzz/fuzz_*.c is one libFuzzer entry point. The drivers are
    # owned by the workstream that owns the fuzzed layer; this rule only turns
    # them into targets, so adding a fuzzer never edits this file.
    file(GLOB QL_FUZZ_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/fuzz_*.c")
    foreach(QL_FUZZ_SOURCE ${QL_FUZZ_SOURCES})
        get_filename_component(QL_FUZZ_NAME ${QL_FUZZ_SOURCE} NAME_WE)
        add_executable(${QL_FUZZ_NAME} ${QL_FUZZ_SOURCE})
        target_link_libraries(${QL_FUZZ_NAME} PRIVATE quodlibet::quodlibet)
        target_compile_options(${QL_FUZZ_NAME} PRIVATE -fsanitize=fuzzer)
        target_link_options(${QL_FUZZ_NAME} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

install(TARGETS quodlibet quodlibet_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/quodlibet
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(QL_ENABLE_BITWUZLA)
    install(PROGRAMS "${QL_BITWUZLA_EXECUTABLE}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME "${QL_BITWUZLA_RUNTIME_NAME}")
endif()
