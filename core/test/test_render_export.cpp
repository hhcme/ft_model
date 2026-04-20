// Export DXF entities to JSON for browser preview.
// Usage: ./render_export input.dxf output.json.gz
//        ./render_export input.dwg output.json.gz  (auto-gzip if .gz suffix)
//        ./render_export input.dwg output.json     (raw JSON)
//
// Output format: { "entities": [ { "type": "line", "points": [[x,y],...], "color": [r,g,b] }, ... ] }

#include "cad/parser/dxf_parser.h"
#include "cad/parser/dwg_parser.h"
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
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <zlib.h>

using namespace cad;

// ============================================================
// Streaming output: wraps either ofstream or gzip stream.
// Provides operator<< so existing << chains work unchanged.
// ============================================================
class JsonWriter {
public:
    explicit JsonWriter(const std::string& path) {
        if (ends_with_gz(path)) {
            m_gz = gzopen(path.c_str(), "wb");
            m_is_gz = !!m_gz;
        } else {
            m_file.open(path, std::ios::binary);
        }
    }
    ~JsonWriter() { close(); }

    bool is_open() const { return m_is_gz ? !!m_gz : m_file.is_open(); }
    bool is_gzip() const { return m_is_gz; }

    void close() {
        if (m_is_gz && m_gz) {
            gzclose(m_gz);
            m_gz = nullptr;
        }
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    JsonWriter& write(const char* s, size_t len) {
        if (m_is_gz) {
            gzwrite(m_gz, s, static_cast<unsigned>(len));
        } else {
            m_file.write(s, static_cast<std::streamsize>(len));
        }
        return *this;
    }

    // operator<< for strings and numeric types
    JsonWriter& operator<<(const char* s) { write(s, strlen(s)); return *this; }
    JsonWriter& operator<<(const std::string& s) { write(s.c_str(), s.size()); return *this; }
    JsonWriter& operator<<(char c) { write(&c, 1); return *this; }

    template<class T>
    JsonWriter& operator<<(T v) {
        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
        write(buf, n);
        return *this;
    }
    JsonWriter& operator<<(int v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%d", v); write(buf, n); return *this; }
    JsonWriter& operator<<(long v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%ld", v); write(buf, n); return *this; }
    JsonWriter& operator<<(long long v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%lld", v); write(buf, n); return *this; }
    JsonWriter& operator<<(unsigned v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%u", v); write(buf, n); return *this; }
    JsonWriter& operator<<(unsigned long v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%lu", v); write(buf, n); return *this; }
    JsonWriter& operator<<(float v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%.10g", v); write(buf, n); return *this; }
    JsonWriter& operator<<(double v) { char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%.10g", v); write(buf, n); return *this; }

private:
    static bool ends_with_gz(const std::string& path) {
        return path.size() >= 3 && strcmp(path.c_str() + path.size() - 3, ".gz") == 0;
    }

    std::ofstream m_file;
    gzFile m_gz = nullptr;
    bool m_is_gz = false;
};

struct DrawCommand {
    std::string type; // "lines" or "linestrip"
    std::vector<float> points; // flat x,y pairs
    uint8_t r, g, b;
};

struct RenderBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool empty = true;

    void expand(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        if (std::abs(x) > 1.0e8f || std::abs(y) > 1.0e8f) return;
        if (empty) {
            min_x = max_x = x;
            min_y = max_y = y;
            empty = false;
            return;
        }
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
};

struct BoundsPoint {
    float x = 0.0f;
    float y = 0.0f;
};

static bool is_export_coord(float x, float y) {
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) <= 1.0e8f && std::abs(y) <= 1.0e8f;
}

static std::string uppercase_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool is_model_or_paper_space_block(const std::string& name) {
    std::string upper = uppercase_ascii(name);
    return upper == "*MODEL_SPACE" || upper == "*PAPER_SPACE";
}

static RenderBounds adaptive_visible_bounds(const std::vector<BoundsPoint>& points,
                                            const RenderBounds& fallback) {
    if (points.size() < 4) return fallback;

    constexpr size_t kMaxSamples = 10000;
    const size_t stride = std::max<size_t>(1, points.size() / kMaxSamples);

    std::vector<float> xs;
    std::vector<float> ys;
    xs.reserve(std::min(points.size(), kMaxSamples));
    ys.reserve(std::min(points.size(), kMaxSamples));
    for (size_t i = 0; i < points.size(); i += stride) {
        const auto& p = points[i];
        if (!is_export_coord(p.x, p.y)) continue;
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    if (xs.size() < 4) return fallback;

    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());

    auto quantile = [](const std::vector<float>& values, float q) {
        const size_t idx = std::min(values.size() - 1,
                                    static_cast<size_t>(std::floor(values.size() * q)));
        return values[idx];
    };

    const float q01x = quantile(xs, 0.01f);
    const float q05x = quantile(xs, 0.05f);
    const float q1x = quantile(xs, 0.25f);
    const float q3x = quantile(xs, 0.75f);
    const float q95x = quantile(xs, 0.95f);
    const float q99x = quantile(xs, 0.99f);
    const float q01y = quantile(ys, 0.01f);
    const float q05y = quantile(ys, 0.05f);
    const float q1y = quantile(ys, 0.25f);
    const float q3y = quantile(ys, 0.75f);
    const float q95y = quantile(ys, 0.95f);
    const float q99y = quantile(ys, 0.99f);
    const float median_x = quantile(xs, 0.50f);
    const float median_y = quantile(ys, 0.50f);
    const float iqr_x = q3x - q1x;
    const float iqr_y = q3y - q1y;

    auto split_axis = [](float q05, float q1, float q3, float q95) {
        const float iqr = q3 - q1;
        if (!std::isfinite(iqr) || iqr <= 0.0f) return false;
        const float threshold = std::max(iqr * 2.5f, 1.0f);
        return (q1 - q05) > threshold || (q95 - q3) > threshold;
    };

    RenderBounds iqr_bounds;
    if (std::isfinite(iqr_x) && iqr_x > 0.0f) {
        iqr_bounds.expand(q1x - iqr_x * 0.1f, median_y);
        iqr_bounds.expand(q3x + iqr_x * 0.1f, median_y);
    } else {
        iqr_bounds.expand(median_x - 0.5f, median_y);
        iqr_bounds.expand(median_x + 0.5f, median_y);
    }
    if (std::isfinite(iqr_y) && iqr_y > 0.0f) {
        iqr_bounds.expand(median_x, q1y - iqr_y * 0.1f);
        iqr_bounds.expand(median_x, q3y + iqr_y * 0.1f);
    } else {
        iqr_bounds.expand(median_x, median_y - 0.5f);
        iqr_bounds.expand(median_x, median_y + 0.5f);
    }

    if (split_axis(q05x, q1x, q3x, q95x) ||
        split_axis(q05y, q1y, q3y, q95y)) {
        return iqr_bounds.empty ? fallback : iqr_bounds;
    }

    RenderBounds broad_bounds;
    const float broad_w = q99x - q01x;
    const float broad_h = q99y - q01y;
    if (std::isfinite(broad_w) && broad_w > 0.0f &&
        std::isfinite(broad_h) && broad_h > 0.0f) {
        broad_bounds.expand(q01x - broad_w * 0.02f, q01y - broad_h * 0.02f);
        broad_bounds.expand(q99x + broad_w * 0.02f, q99y + broad_h * 0.02f);
    }
    if (broad_bounds.empty) return fallback;

    if (!fallback.empty) {
        const float full_w = fallback.max_x - fallback.min_x;
        const float full_h = fallback.max_y - fallback.min_y;
        const float bw = broad_bounds.max_x - broad_bounds.min_x;
        const float bh = broad_bounds.max_y - broad_bounds.min_y;
        if (full_w <= bw * 1.5f && full_h <= bh * 1.5f) {
            return fallback;
        }
    }
    return broad_bounds;
}

static RenderBounds bounds_for_entity_indices(
    const std::vector<EntityVariant>& entities,
    const std::vector<int32_t>& indices) {
    RenderBounds bounds;
    for (int32_t idx : indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= entities.size()) continue;
        const auto& b = entities[static_cast<size_t>(idx)].bounds();
        if (b.is_empty()) continue;
        bounds.expand(b.min.x, b.min.y);
        bounds.expand(b.max.x, b.max.y);
    }
    return bounds;
}

static bool should_render_dwg_block_direct(
    const Block& block,
    const std::vector<EntityVariant>& entities) {
    if (is_model_or_paper_space_block(block.name)) return true;

    RenderBounds bounds = bounds_for_entity_indices(entities, block.entity_indices);
    if (bounds.empty || block.entity_indices.empty()) return false;

    const float cx = (bounds.min_x + bounds.max_x) * 0.5f;
    const float cy = (bounds.min_y + bounds.max_y) * 0.5f;
    const float centroid_dist = std::sqrt(cx * cx + cy * cy);
    return block.entity_indices.size() > 50 && centroid_dist > 5000.0f;
}

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

// Format a coordinate with limited precision to reduce JSON file size.
// Uses %.4g (4 significant figures) for compact scientific notation on large values.
static std::string fmt_coord(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    // Strip unnecessary trailing zeros after decimal point
    char* dot = strchr(buf, '.');
    if (dot) {
        char* end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') { *end = '\0'; end--; }
        if (end == dot) *end = '\0'; // remove lone decimal point
    }
    return buf;
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

    // Dispatch to DWG or DXF parser based on file extension
    bool is_dwg = false;
    const char* ext = input_path + strlen(input_path);
    while (ext > input_path && *ext != '.' && *ext != '/' && *ext != '\\') ext--;
    if (*ext == '.') {
        is_dwg = (strcasecmp(ext, ".dwg") == 0);
    }

    Result parse_result;
    if (is_dwg) {
        DwgParser dwg_parser;
        parse_result = dwg_parser.parse_file(input_path, scene);
    } else {
        DxfParser dxf_parser;
        parse_result = dxf_parser.parse_file(input_path, scene);
    }
    if (!parse_result.ok()) {
        fprintf(stderr, "Parse error: %s\n", parse_result.message.c_str());
        return 1;
    }
    scene.shrink_to_fit();

    auto& entities = scene.entities();
    auto& layers = scene.layers();
    printf("Parsed %zu entities\n", entities.size());

    // Build set of entity indices that belong to block definitions.
    // DXF uses local-space block definitions expanded through INSERT. For DWG,
    // some parsed block children are already world-space geometry while others
    // are local definitions. Classify per block so local DWGs still expand
    // through INSERT, while large world-space blocks render directly.
    std::unordered_set<int32_t> block_entity_indices;
    std::unordered_set<int32_t> direct_block_indices;
    bool insert_expansion_active = false;

    if (is_dwg) {
        const auto& blocks = scene.blocks();
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            const auto& block = blocks[bi];
            if (should_render_dwg_block_direct(block, entities)) {
                direct_block_indices.insert(static_cast<int32_t>(bi));
                for (int32_t ei : block.entity_indices) {
                    if (ei >= 0 && static_cast<size_t>(ei) < entities.size()) {
                        entities[static_cast<size_t>(ei)].header.in_block = false;
                    }
                }
            } else {
                for (int32_t ei : block.entity_indices) {
                    block_entity_indices.insert(ei);
                    if (ei >= 0 && static_cast<size_t>(ei) < entities.size()) {
                        entities[static_cast<size_t>(ei)].header.in_block = true;
                    }
                }
            }
        }
    } else {
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
    }
    // Count entities with in_block still true
    size_t in_block_count = 0;
    for (const auto& e : entities) {
        if (e.header.in_block) in_block_count++;
    }
    printf("Block definition entities: %zu, still in_block: %zu (insert_expansion=%s)\n",
           block_entity_indices.size(), in_block_count, insert_expansion_active ? "yes" : "no");
    if (is_dwg) {
        printf("DWG direct blocks: %zu, local block entities: %zu\n",
               direct_block_indices.size(), block_entity_indices.size());
    }

    // Setup camera to fit the scene
    Camera camera;
    auto bounds = scene.total_bounds();
    if (!bounds.is_empty()) {
        camera.set_viewport(1920, 1080);
        camera.fit_to_bounds(bounds, 0.05f);
    }

    // Collect text entities for JSON export (rendered as actual text by the viewer)
    struct TextEntry {
        float x, y;
        float height;
        float rotation;
        float width_factor;
        std::string text;
        uint8_t r, g, b;
        int32_t layer_index;
        std::string layer_name;
    };
    std::vector<TextEntry> text_entries;

    // Tessellate all entities into draw commands
    RenderBatcher batcher;
    batcher.begin_frame(camera);
    const char* outlier_env = std::getenv("FT_DWG_OUTLIER_FILTER");
    const bool outlier_filter_enabled = is_dwg &&
        !(outlier_env && std::strcmp(outlier_env, "0") == 0);
    batcher.set_outlier_filter_enabled(outlier_filter_enabled);

    for (int32_t i = 0; i < static_cast<int32_t>(entities.size()); ++i) {
        // Skip entities that belong to block definitions — they are rendered
        // through INSERT entity references with proper transforms.
        if (block_entity_indices.count(i)) continue;

        const auto& entity = entities[i];

        // DWG: skip INSERT only when its block is already rendered directly.
        if (is_dwg && entity.type() == EntityType::Insert) {
            auto* ins = std::get_if<InsertEntity>(&entity.data);
            if (ins && direct_block_indices.count(ins->block_index)) continue;
        }

        // Extract text entities for separate rendering
        if (entity.type() == EntityType::Text || entity.type() == EntityType::MText) {
            const TextEntity* te = nullptr;
            if (entity.type() == EntityType::Text) {
                te = std::get_if<6>(&entity.data);
            } else {
                te = std::get_if<7>(&entity.data);
            }
            if (te && te->height > 0.0f && !te->text.empty() &&
                is_export_coord(te->insertion_point.x, te->insertion_point.y) &&
                std::isfinite(te->height) && std::isfinite(te->rotation) &&
                std::isfinite(te->width_factor)) {
                TextEntry entry;
                entry.x = te->insertion_point.x;
                entry.y = te->insertion_point.y;
                entry.height = te->height;
                entry.rotation = te->rotation;
                entry.width_factor = (te->width_factor > 0.0f) ? te->width_factor : 1.0f;
                entry.text = te->text;
                entry.layer_index = entity.header.layer_index;

                // Resolve color
                Color text_color;
                if (entity.header.has_true_color) {
                    text_color = entity.header.true_color;
                } else if (entity.header.color_override != 256 && entity.header.color_override != 0) {
                    text_color = Color::from_aci(entity.header.color_override);
                } else {
                    int32_t li = entity.header.layer_index;
                    const auto& layers = scene.layers();
                    if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
                        text_color = layers[static_cast<size_t>(li)].color;
                    } else {
                        text_color = Color::white();
                    }
                }
                entry.r = text_color.r;
                entry.g = text_color.g;
                entry.b = text_color.b;

                // Layer name
                const auto& layers = scene.layers();
                if (entry.layer_index >= 0 && static_cast<size_t>(entry.layer_index) < layers.size()) {
                    entry.layer_name = layers[static_cast<size_t>(entry.layer_index)].name;
                }

                text_entries.push_back(std::move(entry));
            }
        }

        // Also export dimension text as text entries
        if (entity.type() == EntityType::Dimension) {
            const auto* dim = std::get_if<8>(&entity.data);
            if (dim && !dim->text.empty() && dim->text != "<>" && dim->text != " " &&
                is_export_coord(dim->text_midpoint.x, dim->text_midpoint.y) &&
                std::isfinite(dim->rotation)) {
                TextEntry entry;
                entry.x = dim->text_midpoint.x;
                entry.y = dim->text_midpoint.y;
                entry.height = 3.0f; // Default dimension text height
                entry.rotation = dim->rotation;
                entry.width_factor = 1.0f;
                entry.text = dim->text;
                entry.layer_index = entity.header.layer_index;

                Color text_color;
                if (entity.header.has_true_color) {
                    text_color = entity.header.true_color;
                } else if (entity.header.color_override != 256 && entity.header.color_override != 0) {
                    text_color = Color::from_aci(entity.header.color_override);
                } else {
                    int32_t li = entity.header.layer_index;
                    if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
                        text_color = layers[static_cast<size_t>(li)].color;
                    } else {
                        text_color = Color::white();
                    }
                }
                entry.r = text_color.r;
                entry.g = text_color.g;
                entry.b = text_color.b;

                if (entry.layer_index >= 0 && static_cast<size_t>(entry.layer_index) < layers.size()) {
                    entry.layer_name = layers[static_cast<size_t>(entry.layer_index)].name;
                }

                text_entries.push_back(std::move(entry));
            }
        }

        batcher.submit_entity(entity, scene);
    }

    batcher.end_frame();

    // Export batches as JSON
    auto& batches = batcher.batches();
    printf("Generated %zu batches\n", batches.size());

    JsonWriter out(output_path);
    if (!out.is_open()) {
        fprintf(stderr, "Cannot open output: %s\n", output_path);
        return 1;
    }
    fprintf(stderr, "[output] %s (%s)\n", output_path, out.is_gzip() ? "gzip" : "raw");

    auto& meta = scene.drawing_info();
    auto raw_b = scene.total_bounds();
    RenderBounds render_bounds;
    std::vector<BoundsPoint> visible_points;
    for (const auto& batch : batches) {
        for (size_t i = 0; i + 1 < batch.vertex_data.size(); i += 2) {
            const float x = batch.vertex_data[i];
            const float y = batch.vertex_data[i + 1];
            render_bounds.expand(x, y);
            if (is_export_coord(x, y)) {
                visible_points.push_back({x, y});
            }
        }
    }
    for (const auto& te : text_entries) {
        render_bounds.expand(te.x, te.y);
        if (is_export_coord(te.x, te.y)) {
            visible_points.push_back({te.x, te.y});
        }
    }
    const RenderBounds output_bounds = is_dwg
        ? adaptive_visible_bounds(visible_points, render_bounds)
        : render_bounds;

    out << "{\n";
    out << "  \"filename\": \"" << escape_json(meta.filename) << "\",\n";
    out << "  \"acadVersion\": \"" << escape_json(meta.acad_version) << "\",\n";
    out << "  \"entityCount\": " << entities.size() << ",\n";
    out << "  \"bounds\": {\n";
    if (!output_bounds.empty) {
        out << "    \"minX\": " << output_bounds.min_x << ",\n";
        out << "    \"minY\": " << output_bounds.min_y << ",\n";
        out << "    \"maxX\": " << output_bounds.max_x << ",\n";
        out << "    \"maxY\": " << output_bounds.max_y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";
    out << "  \"rawBounds\": {\n";
    if (!raw_b.is_empty()) {
        out << "    \"minX\": " << raw_b.min.x << ",\n";
        out << "    \"minY\": " << raw_b.min.y << ",\n";
        out << "    \"maxX\": " << raw_b.max.x << ",\n";
        out << "    \"maxY\": " << raw_b.max.y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";

    // Export layers
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
    bool first_batch = true;
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
        int valid_vert_count = 0;
        for (int i = 0; i < vert_count; ++i) {
            double vx = batch.vertex_data[i * 2];
            double vy = batch.vertex_data[i * 2 + 1];
            if (std::isfinite(vx) && std::isfinite(vy) &&
                std::abs(vx) <= 1.0e8 && std::abs(vy) <= 1.0e8) {
                valid_vert_count++;
            }
        }
        if (valid_vert_count == 0) continue;
        total_vertices += static_cast<size_t>(valid_vert_count);

        // Extract layer index from RenderKey
        uint16_t layer_idx = static_cast<uint16_t>((batch.sort_key.key >> 48) & 0xFFFF);
        std::string layer_name_out;
        if (layer_idx < static_cast<uint16_t>(layers.size())) {
            layer_name_out = layers[layer_idx].name;
        }

        if (!first_batch) out << ",\n";
        first_batch = false;
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

        int first_valid = 0;
        for (int i = 0; i < vert_count; ++i) {
            double vx = batch.vertex_data[i * 2];
            double vy = batch.vertex_data[i * 2 + 1];
            if (!std::isfinite(vx) || !std::isfinite(vy) ||
                std::abs(vx) > 1.0e8 || std::abs(vy) > 1.0e8) continue;
            if (first_valid > 0) out << ",";
            out << "[" << vx << "," << vy << "]";
            first_valid++;
        }
        out << "]}";
    }
    out << "\n";
    out << "  ],\n";

    // Export text entities
    out << "  \"texts\": [\n";
    for (size_t ti = 0; ti < text_entries.size(); ++ti) {
        const auto& te = text_entries[ti];
        out << "    {\"x\": " << te.x << ", \"y\": " << te.y
            << ", \"height\": " << te.height
            << ", \"rotation\": " << te.rotation
            << ", \"widthFactor\": " << te.width_factor
            << ", \"text\": \"" << escape_json(te.text) << "\""
            << ", \"color\": [" << (int)te.r << "," << (int)te.g << "," << (int)te.b << "]"
            << ", \"layerIndex\": " << te.layer_index
            << ", \"layerName\": \"" << escape_json(te.layer_name) << "\"}";
        if (ti + 1 < text_entries.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"totalVertices\": " << total_vertices << "\n";
    out << "}\n";

    out.close();
    printf("Exported %zu vertices to %s\n", total_vertices, output_path);
    return 0;
}
