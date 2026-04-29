# core/test/CMakeLists.txt — Test executables

find_package(ZLIB QUIET)

# Standalone parser test — no test framework dependency
add_executable(test_parser
    test_parser.cpp
)
target_link_libraries(test_parser PRIVATE cad_core)

# Render export: DXF → JSON for browser preview
add_executable(render_export
    test_render_export.cpp
)
if(ZLIB_FOUND)
    target_link_libraries(render_export PRIVATE cad_core ZLIB::ZLIB)
else()
    target_link_libraries(render_export PRIVATE cad_core ${CMAKE_DL_LIBS})
endif()

add_executable(regression_smoke
    test_regression_smoke.cpp
)
target_link_libraries(regression_smoke PRIVATE cad_core)

# Entity export: raw entity properties JSON (no tessellation) for structural comparison
add_executable(entity_export
    entity_export.cpp
)
if(ZLIB_FOUND)
    target_link_libraries(entity_export PRIVATE cad_core ZLIB::ZLIB)
else()
    target_link_libraries(entity_export PRIVATE cad_core ${CMAKE_DL_LIBS})
endif()

add_test(
    NAME regression_smoke
    COMMAND regression_smoke
)
set_tests_properties(regression_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Compare parser output against ezdxf reference data
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(compare_dwg
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/compare_parser_output.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS render_export
        COMMENT "Compare parser output against reference data"
    )
endif()
