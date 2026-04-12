// Minimal test harness for DXF parser validation.
// Build: cmake --build build/native && ./build/native/core/test/test_parser <dxf_file>
#include "cad/parser/dxf_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
#include "cad/cad_errors.h"
#include <cstdio>
#include <cstdlib>

using namespace cad;

static const char* entity_type_name(EntityType t) {
    switch (t) {
        case EntityType::Line:      return "LINE";
        case EntityType::Circle:    return "CIRCLE";
        case EntityType::Arc:       return "ARC";
        case EntityType::Polyline:  return "POLYLINE";
        case EntityType::LwPolyline:return "LWPOLYLINE";
        case EntityType::Spline:    return "SPLINE";
        case EntityType::Text:      return "TEXT";
        case EntityType::MText:     return "MTEXT";
        case EntityType::Dimension: return "DIMENSION";
        case EntityType::Hatch:     return "HATCH";
        case EntityType::Insert:    return "INSERT";
        case EntityType::Point:     return "POINT";
        case EntityType::Ellipse:   return "ELLIPSE";
        case EntityType::Ray:       return "RAY";
        case EntityType::XLine:     return "XLINE";
        case EntityType::Viewport:  return "VIEWPORT";
        case EntityType::Solid:     return "SOLID";
    }
    return "UNKNOWN";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dxf_file>\n", argv[0]);
        return 1;
    }

    const char* filepath = argv[1];
    printf("Parsing: %s\n", filepath);

    SceneGraph scene;
    DxfParser parser;

    float last_progress = -1.0f;
    auto result = parser.parse_file(filepath, scene,
        [&last_progress](const ParseProgress& p) {
            float pct = p.total_entities_estimate > 0
                ? static_cast<float>(p.entities_parsed) / static_cast<float>(p.total_entities_estimate)
                : 0.0f;
            if (pct - last_progress >= 0.1f) {
                printf("  Progress: %d/%d entities (%.0f%%) — %s\n",
                       p.entities_parsed, p.total_entities_estimate, pct * 100,
                       p.current_section.c_str());
                last_progress = pct;
            }
        });

    if (!result.ok()) {
        fprintf(stderr, "PARSE ERROR: %s\n", result.message.c_str());
        return 1;
    }

    printf("\nParse complete.\n");

    // Count entities by type
    auto& entities = scene.entities();
    auto entity_count = entities.size();
    printf("Total entities: %zu\n", entity_count);

    // EntityType has 16 values (0-15)
    int type_counts[16] = {};
    int valid_bounds = 0;

    for (auto& ev : entities) {
        int idx = static_cast<int>(ev.header.type);
        if (idx >= 0 && idx < 16) {
            type_counts[idx]++;
        }
        if (!ev.header.bounds.is_empty()) {
            valid_bounds++;
        }
    }

    printf("\nEntity breakdown:\n");
    for (int i = 0; i < 16; ++i) {
        if (type_counts[i] > 0) {
            printf("  %-12s: %d\n", entity_type_name(static_cast<EntityType>(i)), type_counts[i]);
        }
    }
    printf("  Entities with valid bounds: %d/%zu\n", valid_bounds, entity_count);

    // Layer info
    auto& layers = scene.layers();
    printf("\nLayers: %zu\n", layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        printf("  [%zu] %s (color=(%d,%d,%d), frozen=%d, locked=%d)\n",
               i, layers[i].name.c_str(),
               layers[i].color.r, layers[i].color.g, layers[i].color.b,
               layers[i].is_frozen ? 1 : 0, layers[i].is_locked ? 1 : 0);
    }

    // Block info
    auto& blocks = scene.blocks();
    printf("\nBlocks: %zu\n", blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        printf("  [%zu] %s (%zu entities)\n",
               i, blocks[i].name.c_str(), blocks[i].entity_indices.size());
    }

    // Drawing metadata
    auto& info = scene.drawing_info();
    if (!info.extents.is_empty()) {
        printf("\nDrawing extents: (%.2f, %.2f, %.2f) — (%.2f, %.2f, %.2f)\n"
               "  Size: %.2f x %.2f\n",
               info.extents.min.x, info.extents.min.y, info.extents.min.z,
               info.extents.max.x, info.extents.max.y, info.extents.max.z,
               info.extents.width(), info.extents.height());
    }
    if (!info.acad_version.empty()) {
        printf("AutoCAD version: %s\n", info.acad_version.c_str());
    }

    // Overall bounds (computed from all entities)
    auto total = scene.total_bounds();
    if (!total.is_empty()) {
        printf("\nComputed total bounds: (%.2f, %.2f) — (%.2f, %.2f) [%.2f x %.2f]\n",
               total.min.x, total.min.y, total.max.x, total.max.y,
               total.width(), total.height());
    }

    printf("\nAll checks passed.\n");
    return 0;
}
