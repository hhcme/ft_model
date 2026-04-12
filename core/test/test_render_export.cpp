// Export DXF entities to JSON for browser preview.
// Usage: ./render_export input.dxf output.json
//
// Output format: { "entities": [ { "type": "line", "points": [[x,y],...], "color": [r,g,b] }, ... ] }

#include "cad/parser/dxf_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
#include "cad/renderer/render_batcher.h"
#include "cad/renderer/camera.h"
#include "cad/renderer/lod_selector.h"
#include "cad/cad_errors.h"
#include "cad/cad_types.h"
#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cad;

struct DrawCommand {
    std::string type; // "lines" or "linestrip"
    std::vector<float> points; // flat x,y pairs
    uint8_t r, g, b;
};

static std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.dxf> <output.json>\n", argv[0]);
        return 1;
    }

    const char* input_path = argv[1];
    const char* output_path = argv[2];

    printf("Parsing: %s\n", input_path);

    SceneGraph scene;
    DxfParser parser;
    auto result = parser.parse_file(input_path, scene);
    if (!result.ok()) {
        fprintf(stderr, "Parse error: %s\n", result.message.c_str());
        return 1;
    }

    auto& entities = scene.entities();
    printf("Parsed %zu entities\n", entities.size());

    // Build set of entity indices that belong to block definitions.
    // These should only be rendered through INSERT references, not directly.
    std::unordered_set<int32_t> block_entity_indices;
    for (const auto& block : scene.blocks()) {
        for (int32_t ei : block.entity_indices) {
            block_entity_indices.insert(ei);
        }
    }
    printf("Block definition entities: %zu\n", block_entity_indices.size());

    // Setup camera to fit the scene
    Camera camera;
    auto bounds = scene.total_bounds();
    if (!bounds.is_empty()) {
        camera.set_viewport(1920, 1080);
        camera.fit_to_bounds(bounds, 0.05f);
    }

    // Tessellate all entities into draw commands
    RenderBatcher batcher;
    batcher.begin_frame(camera);

    for (int32_t i = 0; i < static_cast<int32_t>(entities.size()); ++i) {
        // Skip entities that belong to block definitions — they are rendered
        // through INSERT entity references with proper transforms.
        if (block_entity_indices.count(i)) continue;
        batcher.submit_entity(entities[i], scene);
    }

    batcher.end_frame();

    // Export batches as JSON
    auto& batches = batcher.batches();
    printf("Generated %zu batches\n", batches.size());

    std::ofstream out(output_path);
    if (!out.is_open()) {
        fprintf(stderr, "Cannot open output: %s\n", output_path);
        return 1;
    }

    auto& meta = scene.drawing_info();
    auto total_b = scene.total_bounds();

    out << "{\n";
    out << "  \"filename\": \"" << escape_json(meta.filename) << "\",\n";
    out << "  \"acadVersion\": \"" << escape_json(meta.acad_version) << "\",\n";
    out << "  \"entityCount\": " << entities.size() << ",\n";
    out << "  \"bounds\": {\n";
    if (!total_b.is_empty()) {
        out << "    \"minX\": " << total_b.min.x << ",\n";
        out << "    \"minY\": " << total_b.min.y << ",\n";
        out << "    \"maxX\": " << total_b.max.x << ",\n";
        out << "    \"maxY\": " << total_b.max.y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";

    // Export layers
    auto& layers = scene.layers();
    out << "  \"layers\": [\n";
    for (size_t i = 0; i < layers.size(); ++i) {
        auto& l = layers[i];
        out << "    {\"name\": \"" << escape_json(l.name)
            << "\", \"color\": [" << (int)l.color.r << "," << (int)l.color.g << "," << (int)l.color.b
            << "], \"frozen\": " << (l.is_frozen ? "true" : "false")
            << ", \"locked\": " << (l.is_locked ? "true" : "false") << "}";
        if (i + 1 < layers.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"batches\": [\n";
    size_t total_vertices = 0;
    for (size_t bi = 0; bi < batches.size(); ++bi) {
        auto& batch = batches[bi];
        if (batch.vertex_data.empty()) continue;

        const char* topo;
        switch (batch.topology) {
            case PrimitiveTopology::LineList:     topo = "lines"; break;
            case PrimitiveTopology::LineStrip:    topo = "linestrip"; break;
            case PrimitiveTopology::TriangleList: topo = "triangles"; break;
            default:                              topo = "unknown"; break;
        }
        int vert_count = static_cast<int>(batch.vertex_data.size() / 2);
        total_vertices += vert_count;

        // Extract layer index from RenderKey
        uint16_t layer_idx = static_cast<uint16_t>((batch.sort_key.key >> 48) & 0xFFFF);
        std::string layer_name_out;
        if (layer_idx < static_cast<uint16_t>(layers.size())) {
            layer_name_out = layers[layer_idx].name;
        }

        out << "    {\"topology\": \"" << topo << "\", ";
        out << "\"color\": [" << (int)batch.color.r << "," << (int)batch.color.g << "," << (int)batch.color.b << "], ";
        out << "\"layerIndex\": " << layer_idx << ", ";
        out << "\"layerName\": \"" << escape_json(layer_name_out) << "\", ";
        // Export entity break points for linestrip topology
        if (batch.topology == PrimitiveTopology::LineStrip && !batch.entity_starts.empty()) {
            out << "\"breaks\": [";
            for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
                if (ei > 0) out << ",";
                out << batch.entity_starts[ei];
            }
            out << "], ";
        }
        out << "\"vertices\": [";

        for (int i = 0; i < vert_count; ++i) {
            if (i > 0) out << ",";
            out << "[" << batch.vertex_data[i * 2] << "," << batch.vertex_data[i * 2 + 1] << "]";
        }
        out << "]}";
        if (bi + 1 < batches.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"totalVertices\": " << total_vertices << "\n";
    out << "}\n";

    out.close();
    printf("Exported %zu vertices to %s\n", total_vertices, output_path);
    return 0;
}
