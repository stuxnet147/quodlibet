add_library(ql_test_plugin MODULE plugins/test_plugin.c)
target_include_directories(ql_test_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
set_target_properties(ql_test_plugin PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
)

file(GLOB QL_TEST_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)
if(NOT QL_TEST_SOURCES)
    message(FATAL_ERROR "QL_BUILD_TESTS is ON, but no test_*.cpp files exist")
endif()
list(SORT QL_TEST_SOURCES)

add_executable(quodlibet_tests ${QL_TEST_SOURCES})
target_link_libraries(quodlibet_tests PRIVATE
    quodlibet::quodlibet
    GTest::gtest
)
# src/ is on the path so the internal process runner can be tested where it
# lives rather than only through a backend that happens to use it. Nothing
# here crosses the public ABI; a shared build hides these symbols, and
# test_process_runner.cpp compiles to nothing in that configuration.
target_include_directories(quodlibet_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
add_dependencies(quodlibet_tests ql_test_plugin)

if(WIN32 AND QL_BUILD_SHARED)
    add_custom_command(TARGET quodlibet_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:quodlibet>
            $<TARGET_FILE_DIR:quodlibet_tests>
        VERBATIM
    )
endif()

if(MSVC)
    target_compile_options(quodlibet_tests PRIVATE /W4 /permissive-)
    target_compile_options(ql_test_plugin PRIVATE /W4 /permissive-)
else()
    target_compile_options(quodlibet_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ql_test_plugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(quodlibet_tests
    TEST_PREFIX "quodlibet."
    EXTRA_ARGS $<TARGET_FILE:ql_test_plugin>
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 30
)
