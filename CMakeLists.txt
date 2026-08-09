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

if(BUILD_TESTING AND QL_BUILD_TESTS)
    enable_language(CXX)
endif()

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
file(GLOB QL_OPTIONAL_CORE_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/src/c_frontend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/evidence.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/problem.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proof_method.c"
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

add_executable(quodlibet_cli cli/main.c)
set_target_properties(quodlibet_cli PROPERTIES OUTPUT_NAME quodlibet)
target_link_libraries(quodlibet_cli PRIVATE quodlibet::quodlibet)

if(BUILD_TESTING AND QL_BUILD_TESTS)
    add_subdirectory(tests)
endif()
