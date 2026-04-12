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
target_link_libraries(render_export PRIVATE cad_core)
