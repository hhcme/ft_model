# core/test/CMakeLists.txt — Test executables

# Standalone parser test — no test framework dependency
add_executable(test_parser
    test_parser.cpp
)
target_link_libraries(test_parser PRIVATE cad_core)

# Render export: DXF → JSON for browser preview
add_executable(render_export
    test_render_export.cpp
)
target_link_libraries(render_export PRIVATE cad_core -lz)

add_executable(regression_smoke
    test_regression_smoke.cpp
)
target_link_libraries(regression_smoke PRIVATE cad_core)

add_test(
    NAME regression_smoke
    COMMAND regression_smoke
)
set_tests_properties(regression_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
