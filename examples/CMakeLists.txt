# The example plugin is built with the repository so that it cannot rot into
# an example that no longer compiles. tests/test_example_plugin.cpp loads the
# module this produces and checks it still behaves as its README says.
#
# The target is deliberately spelled the way an out-of-tree plugin would be:
# a MODULE library linking quodlibet::quodlibet, with no prefix and hidden
# visibility so that QL_PLUGIN_EXPORT names the only exported symbol.

add_library(ql_example_plugin MODULE plugin/normalize_plugin.c)
target_link_libraries(ql_example_plugin PRIVATE quodlibet::quodlibet)
set_target_properties(ql_example_plugin PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
)

if(MSVC)
    target_compile_options(ql_example_plugin PRIVATE /W4 /permissive-)
else()
    target_compile_options(ql_example_plugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The module is emitted beside the test executable so the test can find it
# from its own path rather than through a build-system variable it would have
# to be handed. This directory is added before tests/, so the test target does
# not exist yet and cannot be depended on from here; a whole-project build
# produces both regardless, and the test states plainly what is missing if the
# module is not there.
if(BUILD_TESTING AND QL_BUILD_TESTS)
    set(QL_EXAMPLE_PLUGIN_DIR "${CMAKE_BINARY_DIR}/tests")
    set_target_properties(ql_example_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${QL_EXAMPLE_PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${QL_EXAMPLE_PLUGIN_DIR}"
    )
    foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(ql_example_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${configuration_upper}
                "${QL_EXAMPLE_PLUGIN_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}
                "${QL_EXAMPLE_PLUGIN_DIR}")
    endforeach()
endif()
