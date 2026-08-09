cmake_minimum_required(VERSION 3.26)

# 3.26 is the floor because Development.SABIModule and python_add_library's
# USE_SABI are what produce a real abi3 module instead of a version-tagged one.
project(quodlibet_python LANGUAGES C)

# This directory is usable two ways: on its own, as `pip install .` configures
# it, and as a subdirectory of the root project, where it also registers the
# CTest entry that keeps `ctest` able to tell that the bindings broke.
if(TARGET quodlibet::quodlibet)
    set(QL_PYTHON_IN_TREE TRUE)
else()
    set(QL_PYTHON_IN_TREE FALSE)
endif()

if(NOT QL_PYTHON_IN_TREE)
    # The extension links the core statically so one file is self-contained:
    # there is no companion shared library to place on a loader path at import
    # time.
    set(QL_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(QL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                     "${CMAKE_CURRENT_BINARY_DIR}/quodlibet_core"
                     EXCLUDE_FROM_ALL)
endif()

# A missing interpreter is stated, never turned into a skipped test. An
# environment without CPython development files simply does not get the
# bindings, and the configure log says so.
find_package(Python 3.11 QUIET
    COMPONENTS Interpreter Development.Module Development.SABIModule)
if(NOT Python_FOUND)
    message(STATUS
        "quodlibet: no CPython 3.11+ Interpreter/Development.SABIModule was "
        "found. The Python bindings are NOT built and no binding test is "
        "registered.")
    return()
endif()

python_add_library(_quodlibet MODULE USE_SABI 3.11 WITH_SOABI
    src/quodlibet_module.c
    src/ql_check.c
)
target_include_directories(_quodlibet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(_quodlibet PRIVATE quodlibet::quodlibet)
set_target_properties(_quodlibet PROPERTIES
    C_VISIBILITY_PRESET hidden
    # An importable package tree inside the build directory, so neither the
    # test nor the source tree needs a stray copy of the compiled module.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/quodlibet"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/quodlibet"
)
foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${configuration}" configuration_upper)
    set_target_properties(_quodlibet PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${configuration_upper}
            "${CMAKE_CURRENT_BINARY_DIR}/package/quodlibet"
        RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}
            "${CMAKE_CURRENT_BINARY_DIR}/package/quodlibet")
endforeach()

add_custom_command(TARGET _quodlibet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/quodlibet/__init__.py"
        "${CMAKE_CURRENT_BINARY_DIR}/package/quodlibet/__init__.py"
    VERBATIM)

if(MSVC)
    target_compile_options(_quodlibet PRIVATE /W4)
else()
    target_compile_options(_quodlibet PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Only this component is installed, so configuring from this directory never
# drags the core's own install rules (CLI, headers, solver binary) into a
# wheel. scikit-build-core selects it through install.components.
install(TARGETS _quodlibet
    LIBRARY DESTINATION quodlibet COMPONENT quodlibet_python
    RUNTIME DESTINATION quodlibet COMPONENT quodlibet_python)

if(QL_PYTHON_IN_TREE AND BUILD_TESTING AND QL_BUILD_TESTS)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import pytest"
        RESULT_VARIABLE ql_python_pytest_result
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT ql_python_pytest_result EQUAL 0)
        message(STATUS
            "quodlibet: ${Python_EXECUTABLE} has no pytest. The Python "
            "binding test is NOT registered; run it separately with "
            "`pytest bindings/python/tests` once pytest is installed.")
    else()
        add_test(NAME quodlibet.python_bindings
            COMMAND "${Python_EXECUTABLE}" -m pytest -q
                "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        set_tests_properties(quodlibet.python_bindings PROPERTIES
            TIMEOUT 600
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/package;PYTHONDONTWRITEBYTECODE=1")
    endif()
endif()
