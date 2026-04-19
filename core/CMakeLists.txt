# core/CMakeLists.txt

add_library(cad_core STATIC
    src/cad_engine.cpp
    src/cad_types.cpp
    src/memory/arena_allocator.cpp
    src/memory/object_pool.cpp
    src/parser/dxf_parser.cpp
    src/parser/dxf_tokenizer.cpp
    src/parser/dxf_section_reader.cpp
    src/parser/dxf_header_reader.cpp
    src/parser/dxf_tables_reader.cpp
    src/parser/dxf_blocks_reader.cpp
    src/parser/dxf_entities_reader.cpp
    src/parser/dxf_objects_reader.cpp
    src/parser/dwg_reader.cpp
    src/parser/dwg_parser.cpp
    src/parser/dwg_objects.cpp
    src/scene/scene_graph.cpp
    src/scene/entity.cpp
    src/scene/layer.cpp
    src/scene/linetype.cpp
    src/scene/block.cpp
    src/scene/block_reference.cpp
    src/scene/viewport.cpp
    src/scene/spatial_index.cpp
    src/renderer/render_context.cpp
    src/renderer/render_command.cpp
    src/renderer/render_batcher.cpp
    src/renderer/camera.cpp
    src/renderer/frustum_culler.cpp
    src/renderer/lod_selector.cpp
    src/interaction/selection_engine.cpp
    src/interaction/snap_engine.cpp
    src/interaction/gesture_handler.cpp
    src/cad_ffi_api.cpp
)

target_include_directories(cad_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cad_core PUBLIC)

target_compile_definitions(cad_core
    PUBLIC
        CAD_VERSION_MAJOR=0
        CAD_VERSION_MINOR=2
        CAD_VERSION_PATCH=0
)

if(CAD_BUILD_TESTS)
    add_subdirectory(test)
endif()
