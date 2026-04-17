#include "cad/cad_errors.h"
#include "cad/parser/dwg_parser.h"
#include "cad/parser/dxf_parser.h"
#include "cad/renderer/camera.h"
#include "cad/renderer/render_batcher.h"
#include "cad/scene/entity.h"
#include "cad/scene/scene_graph.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cad;

namespace {

struct Summary {
    size_t entity_count = 0;
    size_t batch_count = 0;
    size_t total_vertices = 0;
    size_t text_count = 0;
    bool bounds_empty = true;
};

struct SmokeCase {
    std::string path;
    size_t min_entities = 0;
    size_t max_entities = static_cast<size_t>(-1);
    size_t min_batches = 0;
    size_t max_batches = static_cast<size_t>(-1);
    size_t min_vertices = 0;
    size_t max_vertices = static_cast<size_t>(-1);
    size_t min_texts = 0;
    size_t max_texts = static_cast<size_t>(-1);
    bool expect_bounds_empty = false;
    bool optional = false;
};

bool is_dwg_path(const std::string& path) {
    const char* input_path = path.c_str();
    const char* ext = input_path + std::strlen(input_path);
    while (ext > input_path && *ext != '.' && *ext != '/' && *ext != '\\') {
        --ext;
    }
    return *ext == '.' && strcasecmp(ext, ".dwg") == 0;
}

size_t collect_text_count(const SceneGraph& scene) {
    size_t count = 0;
    for (const auto& entity : scene.entities()) {
        if (entity.type() == EntityType::Text || entity.type() == EntityType::MText) {
            const TextEntity* te = (entity.type() == EntityType::Text)
                                       ? std::get_if<6>(&entity.data)
                                       : std::get_if<7>(&entity.data);
            if (te && te->height > 0.0f && !te->text.empty()) {
                ++count;
            }
            continue;
        }

        if (entity.type() == EntityType::Dimension) {
            const auto* dim = std::get_if<8>(&entity.data);
            if (dim && !dim->text.empty() && dim->text != "<>" && dim->text != " ") {
                ++count;
            }
        }
    }
    return count;
}

Result parse_scene(const std::string& path, SceneGraph& scene) {
    if (is_dwg_path(path)) {
        DwgParser parser;
        return parser.parse_file(path, scene);
    }

    DxfParser parser;
    return parser.parse_file(path, scene);
}

Summary summarize_scene(SceneGraph& scene) {
    Summary summary;
    summary.entity_count = scene.entities().size();
    summary.bounds_empty = scene.total_bounds().is_empty();
    summary.text_count = collect_text_count(scene);

    std::unordered_set<int32_t> block_entity_indices;
    // Only filter block entities if INSERT expansion is working
    bool insert_expansion_active = false;
    const auto& entities = scene.entities();
    for (const auto& entity : entities) {
        if (entity.type() == EntityType::Insert) {
            auto* ins = std::get_if<InsertEntity>(&entity.data);
            if (ins && ins->block_index >= 0) {
                insert_expansion_active = true;
                break;
            }
        }
    }
    if (insert_expansion_active) {
        for (const auto& block : scene.blocks()) {
            for (int32_t ei : block.entity_indices) {
                block_entity_indices.insert(ei);
            }
        }
    }

    Camera camera;
    auto bounds = scene.total_bounds();
    if (!bounds.is_empty()) {
        camera.set_viewport(1920, 1080);
        camera.fit_to_bounds(bounds, 0.05f);
    }

    RenderBatcher batcher;
    batcher.begin_frame(camera);
    for (int32_t i = 0; i < static_cast<int32_t>(entities.size()); ++i) {
        if (block_entity_indices.count(i)) {
            continue;
        }
        batcher.submit_entity(entities[static_cast<size_t>(i)], scene);
    }
    batcher.end_frame();

    summary.batch_count = batcher.batches().size();
    for (const auto& batch : batcher.batches()) {
        summary.total_vertices += batch.vertex_data.size() / 2;
    }

    return summary;
}

bool check_range(const char* label, const std::string& path, size_t value,
                 size_t min_value, size_t max_value) {
    if (value < min_value || value > max_value) {
        std::fprintf(stderr,
                     "[smoke] %s failed for %s: got=%zu expected=[%zu,%zu]\n",
                     label, path.c_str(), value, min_value, max_value);
        return false;
    }
    return true;
}

bool run_case(const SmokeCase& smoke_case) {
    if (smoke_case.optional &&
        !std::filesystem::exists(std::filesystem::path(smoke_case.path))) {
        std::printf("[smoke] skip optional fixture: %s\n", smoke_case.path.c_str());
        return true;
    }

    SceneGraph scene;
    Result result = parse_scene(smoke_case.path, scene);
    if (!result.ok()) {
        std::fprintf(stderr, "[smoke] parse failed for %s: %s\n",
                     smoke_case.path.c_str(), result.message.c_str());
        return false;
    }

    Summary summary = summarize_scene(scene);
    std::printf("[smoke] %s entities=%zu batches=%zu vertices=%zu texts=%zu boundsEmpty=%s\n",
                smoke_case.path.c_str(), summary.entity_count, summary.batch_count,
                summary.total_vertices, summary.text_count,
                summary.bounds_empty ? "true" : "false");

    bool ok = true;
    ok &= check_range("entityCount", smoke_case.path, summary.entity_count,
                      smoke_case.min_entities, smoke_case.max_entities);
    ok &= check_range("batchCount", smoke_case.path, summary.batch_count,
                      smoke_case.min_batches, smoke_case.max_batches);
    ok &= check_range("totalVertices", smoke_case.path, summary.total_vertices,
                      smoke_case.min_vertices, smoke_case.max_vertices);
    ok &= check_range("textCount", smoke_case.path, summary.text_count,
                      smoke_case.min_texts, smoke_case.max_texts);

    if (summary.bounds_empty != smoke_case.expect_bounds_empty) {
        std::fprintf(stderr,
                     "[smoke] bounds check failed for %s: got=%s expected=%s\n",
                     smoke_case.path.c_str(),
                     summary.bounds_empty ? "true" : "false",
                     smoke_case.expect_bounds_empty ? "true" : "false");
        ok = false;
    }

    return ok;
}

} // namespace

int main() {
    const std::vector<SmokeCase> smoke_cases = {
        {.path = "test_data/minimal.dxf",
         .min_entities = 4, .max_entities = 4,
         .min_batches = 2, .max_batches = 2,
         .min_vertices = 44, .max_vertices = 44,
         .min_texts = 0, .max_texts = 0,
         .expect_bounds_empty = false},
        {.path = "test_data/insert_blocks.dxf",
         .min_entities = 33, .max_entities = 33,
         .min_batches = 2, .max_batches = 2,
         .min_vertices = 783, .max_vertices = 783,
         .min_texts = 0, .max_texts = 0,
         .expect_bounds_empty = false},
        {.path = "test_data/text_entities.dxf",
         .min_entities = 7, .max_entities = 7,
         .min_batches = 1, .max_batches = 1,
         .min_vertices = 14, .max_vertices = 14,
         .min_texts = 7, .max_texts = 7,
         .expect_bounds_empty = false},
        {.path = "test_dwg/big.dwg",
         .min_entities = 130000,
         .min_batches = 50,
         .min_vertices = 350000,
         .min_texts = 5000, .max_texts = 7000,
         .expect_bounds_empty = false,
         .optional = true},
    };

    bool ok = true;
    for (const auto& smoke_case : smoke_cases) {
        ok &= run_case(smoke_case);
    }

    return ok ? 0 : 1;
}
