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
    src/parser/dwg_reader_stream.cpp
    src/parser/dwg_reader_crc_lz77.cpp
    src/parser/dwg_parser.cpp
    src/parser/dwg_diagnostics.cpp
    src/parser/dwg_r2004_decoder.cpp
    src/parser/dwg_r2000_decoder.cpp
    src/parser/dwg_r2007_codec.cpp
    src/parser/dwg_r2007_page_decode.cpp
    src/parser/dwg_header_vars.cpp
    src/parser/dwg_object_map.cpp
    src/parser/dwg_parse_helpers.cpp
    src/parser/dwg_table_prescan.cpp
    src/parser/dwg_block_tracking.cpp
    src/parser/dwg_block_classification.cpp
    src/parser/dwg_handle_stream.cpp
    src/parser/dwg_custom_annotation_proxy.cpp
    src/parser/dwg_parse_diagnostics_emit.cpp
    src/parser/dwg_post_processing.cpp
    src/parser/dwg_object_recovery.cpp
    src/parser/dwg_objects.cpp
    src/parser/dwg_entity_geometry.cpp
    src/parser/dwg_entity_annotation.cpp
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
    src/renderer/render_batcher_curve.cpp
    src/renderer/render_batcher_entity.cpp
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

# iconv for CJK string encoding conversion (GBK/Shift-JIS → UTF-8)
if(APPLE)
    target_link_libraries(cad_core PUBLIC iconv)
endif()

target_compile_definitions(cad_core
    PUBLIC
        CAD_VERSION_MAJOR=0
        CAD_VERSION_MINOR=2
        CAD_VERSION_PATCH=0
)

if(CAD_BUILD_TESTS)
    add_subdirectory(test)
endif()
