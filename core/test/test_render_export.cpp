// Export DXF entities to JSON for browser preview.
// Usage: ./render_export input.dxf output.json.gz
//        ./render_export input.dwg output.json.gz  (auto-gzip if .gz suffix)
//        ./render_export input.dwg output.json     (raw JSON)
//
// Output format: { "entities": [ { "type": "line", "points": [[x,y],...], "color": [r,g,b] }, ... ] }

#include "cad/parser/dxf_parser.h"
#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_block_classification.h"
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
#include <functional>
#include <utility>
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

static void write_render_bounds(JsonWriter& out, const RenderBounds& bounds) {
    out << "{";
    if (!bounds.empty) {
        out << "\"minX\": " << bounds.min_x
            << ", \"minY\": " << bounds.min_y
            << ", \"maxX\": " << bounds.max_x
            << ", \"maxY\": " << bounds.max_y
            << ", \"isEmpty\": false";
    } else {
        out << "\"isEmpty\": true";
    }
    out << "}";
}

static void write_bounds3d_xy(JsonWriter& out, const Bounds3d& bounds) {
    out << "{";
    if (!bounds.is_empty() &&
        std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) &&
        std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y)) {
        out << "\"minX\": " << bounds.min.x
            << ", \"minY\": " << bounds.min.y
            << ", \"maxX\": " << bounds.max.x
            << ", \"maxY\": " << bounds.max.y
            << ", \"isEmpty\": false";
    } else {
        out << "\"isEmpty\": true";
    }
    out << "}";
}

static void write_vec3_xy(JsonWriter& out, const Vec3& p) {
    out << "{";
    if (std::isfinite(p.x) && std::isfinite(p.y)) {
        out << "\"x\": " << p.x
            << ", \"y\": " << p.y
            << ", \"z\": " << (std::isfinite(p.z) ? p.z : 0.0f);
    } else {
        out << "\"isEmpty\": true";
    }
    out << "}";
}

static bool bounds3d_has_finite_xy(const Bounds3d& bounds) {
    return !bounds.is_empty() &&
           std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) &&
           std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y);
}

static RenderBounds viewport_model_bounds(const Viewport& viewport) {
    RenderBounds bounds;
    if (std::isfinite(viewport.model_view_center.x) &&
        std::isfinite(viewport.model_view_center.y) &&
        std::isfinite(viewport.width) &&
        std::isfinite(viewport.view_height) &&
        viewport.width > 0.0f &&
        viewport.view_height > 0.0f) {
        const float half_w = viewport.width * 0.5f;
        const float half_h = viewport.view_height * 0.5f;
        bounds.expand(viewport.model_view_center.x - half_w,
                      viewport.model_view_center.y - half_h);
        bounds.expand(viewport.model_view_center.x + half_w,
                      viewport.model_view_center.y + half_h);
    }
    return bounds;
}

static RenderBounds layout_viewport_model_bounds(const Viewport& viewport) {
    RenderBounds bounds;
    if (std::isfinite(viewport.model_view_center.x) &&
        std::isfinite(viewport.model_view_center.y) &&
        std::isfinite(viewport.paper_width) &&
        std::isfinite(viewport.paper_height) &&
        std::isfinite(viewport.view_height) &&
        viewport.paper_width > 0.0f &&
        viewport.paper_height > 0.0f &&
        viewport.view_height > 0.0f) {
        const float aspect = viewport.paper_width / viewport.paper_height;
        const float half_w = viewport.view_height * aspect * 0.5f;
        const float half_h = viewport.view_height * 0.5f;
        bounds.expand(viewport.model_view_center.x - half_w,
                      viewport.model_view_center.y - half_h);
        bounds.expand(viewport.model_view_center.x + half_w,
                      viewport.model_view_center.y + half_h);
    }
    return bounds;
}

static RenderBounds layout_viewport_model_bounds_with_center(const Viewport& viewport,
                                                             const Vec3& center) {
    Viewport candidate = viewport;
    candidate.model_view_center = center;
    return layout_viewport_model_bounds(candidate);
}

static RenderBounds layout_viewport_paper_bounds(const Viewport& viewport) {
    RenderBounds bounds;
    if (std::isfinite(viewport.paper_center.x) &&
        std::isfinite(viewport.paper_center.y) &&
        std::isfinite(viewport.paper_width) &&
        std::isfinite(viewport.paper_height) &&
        viewport.paper_width > 0.0f &&
        viewport.paper_height > 0.0f) {
        const float half_w = viewport.paper_width * 0.5f;
        const float half_h = viewport.paper_height * 0.5f;
        bounds.expand(viewport.paper_center.x - half_w,
                      viewport.paper_center.y - half_h);
        bounds.expand(viewport.paper_center.x + half_w,
                      viewport.paper_center.y + half_h);
    }
    return bounds;
}

static float bounds_point_coverage(const RenderBounds& bounds,
                                   const std::vector<BoundsPoint>& points) {
    if (bounds.empty || points.empty()) return 0.0f;
    size_t inside = 0;
    for (const auto& p : points) {
        if (p.x >= bounds.min_x && p.x <= bounds.max_x &&
            p.y >= bounds.min_y && p.y <= bounds.max_y) {
            inside++;
        }
    }
    return static_cast<float>(inside) / static_cast<float>(points.size());
}

static bool is_export_coord(float x, float y);

static bool point_in_bounds(float x, float y, const RenderBounds& bounds) {
    return !bounds.empty &&
           x >= bounds.min_x && x <= bounds.max_x &&
           y >= bounds.min_y && y <= bounds.max_y;
}

static RenderBounds expand_bounds(const RenderBounds& bounds, float ratio) {
    if (bounds.empty) return bounds;
    RenderBounds expanded;
    const float w = bounds.max_x - bounds.min_x;
    const float h = bounds.max_y - bounds.min_y;
    const float pad = std::max(w, h) * ratio;
    expanded.expand(bounds.min_x - pad, bounds.min_y - pad);
    expanded.expand(bounds.max_x + pad, bounds.max_y + pad);
    return expanded;
}

static float render_bounds_diag(const RenderBounds& bounds) {
    if (bounds.empty) return 0.0f;
    const float w = bounds.max_x - bounds.min_x;
    const float h = bounds.max_y - bounds.min_y;
    return std::sqrt(w * w + h * h);
}

static bool render_bounds_intersects(const RenderBounds& a, const RenderBounds& b) {
    if (a.empty || b.empty) return false;
    return !(a.max_x < b.min_x || a.min_x > b.max_x ||
             a.max_y < b.min_y || a.min_y > b.max_y);
}

static const char* drawing_space_name(DrawingSpace space) {
    switch (space) {
        case DrawingSpace::ModelSpace: return "model";
        case DrawingSpace::PaperSpace: return "paper";
        default: return "unknown";
    }
}

static std::string summarize_layer_counts(const std::vector<size_t>& counts,
                                          const std::vector<Layer>& layers,
                                          size_t limit) {
    std::vector<std::pair<size_t, size_t>> ranked;
    for (size_t i = 0; i < counts.size() && i < layers.size(); ++i) {
        if (counts[i] > 0) {
            ranked.push_back({counts[i], i});
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    std::string summary;
    const size_t n = std::min(limit, ranked.size());
    for (size_t i = 0; i < n; ++i) {
        if (!summary.empty()) summary += "; ";
        const size_t layer_index = ranked[i].second;
        summary += layers[layer_index].name.empty() ? "<unnamed>" : layers[layer_index].name;
        summary += "=";
        summary += std::to_string(ranked[i].first);
    }
    return summary;
}

static RenderBounds batch_render_bounds(const RenderBatch& batch) {
    RenderBounds bounds;
    const auto& vd = batch.vertex_data;
    for (size_t i = 0; i + 1 < vd.size(); i += 2) {
        bounds.expand(vd[i], vd[i + 1]);
    }
    return bounds;
}

static RenderBounds nearby_content_bounds_for_sheet(const std::vector<RenderBatch>& batches,
                                                    const RenderBounds& reference) {
    if (reference.empty) return reference;
    RenderBounds content = reference;
    const RenderBounds search = expand_bounds(reference, 0.20f);
    const float ref_diag = std::max(render_bounds_diag(reference), 1.0f);
    for (const auto& batch : batches) {
        RenderBounds bb = batch_render_bounds(batch);
        if (bb.empty) continue;
        if (!render_bounds_intersects(bb, search)) continue;
        if (render_bounds_diag(bb) > ref_diag * 2.0f) continue;
        content.expand(bb.min_x, bb.min_y);
        content.expand(bb.max_x, bb.max_y);
    }
    return content;
}

static RenderBounds padded_sheet_bounds(const RenderBounds& content) {
    if (content.empty) return content;
    RenderBounds sheet;
    const float w = content.max_x - content.min_x;
    const float h = content.max_y - content.min_y;
    const float pad = std::max(std::max(w, h) * 0.06f, 1.0f);
    sheet.expand(content.min_x - pad, content.min_y - pad);
    sheet.expand(content.max_x + pad, content.max_y + pad);

    // When DWG Layout objects are missing but the file clearly behaves like a
    // paper-space mechanical sheet, keep the fallback canvas sheet-shaped
    // instead of tightly wrapping the annotation cloud. ISO/ANSI landscape
    // paper is near sqrt(2); expanding only the shorter dimension preserves
    // content positions while making initial fitView match CAD viewers better.
    constexpr float kLandscapePaperAspect = 1.41421356f;
    const float sheet_w = sheet.max_x - sheet.min_x;
    const float sheet_h = sheet.max_y - sheet.min_y;
    if (sheet_w > 0.0f && sheet_h > 0.0f) {
        const float aspect = sheet_w / sheet_h;
        const float cx = (sheet.min_x + sheet.max_x) * 0.5f;
        const float cy = (sheet.min_y + sheet.max_y) * 0.5f;
        if (aspect < kLandscapePaperAspect) {
            const float half_w = sheet_h * kLandscapePaperAspect * 0.5f;
            sheet.min_x = cx - half_w;
            sheet.max_x = cx + half_w;
        } else if (aspect > kLandscapePaperAspect * 1.25f) {
            const float half_h = sheet_w / kLandscapePaperAspect * 0.5f;
            sheet.min_y = cy - half_h;
            sheet.max_y = cy + half_h;
        }
    }
    return sheet;
}

static RenderBounds outer_paper_bounds(const RenderBounds& plot_window) {
    if (plot_window.empty) return plot_window;
    const float w = plot_window.max_x - plot_window.min_x;
    const float h = plot_window.max_y - plot_window.min_y;
    const float pad = std::max(std::max(w, h) * 0.035f, 1.0f);
    RenderBounds paper;
    paper.expand(plot_window.min_x - pad, plot_window.min_y - pad);
    paper.expand(plot_window.max_x + pad, plot_window.max_y + pad);

    constexpr float kLandscapePaperAspect = 1.41421356f;
    const float paper_w = paper.max_x - paper.min_x;
    const float paper_h = paper.max_y - paper.min_y;
    if (paper_w > 0.0f && paper_h > 0.0f) {
        const float cx = (paper.min_x + paper.max_x) * 0.5f;
        const float cy = (paper.min_y + paper.max_y) * 0.5f;
        const float aspect = paper_w / paper_h;
        if (aspect < kLandscapePaperAspect) {
            const float half_w = paper_h * kLandscapePaperAspect * 0.5f;
            paper.min_x = cx - half_w;
            paper.max_x = cx + half_w;
        } else if (aspect > kLandscapePaperAspect) {
            const float half_h = paper_w / kLandscapePaperAspect * 0.5f;
            paper.min_y = cy - half_h;
            paper.max_y = cy + half_h;
        }
    }
    return paper;
}

static bool segment_intersects_bounds(float x0, float y0, float x1, float y1,
                                      const RenderBounds& bounds) {
    if (bounds.empty) return true;
    const float min_x = std::min(x0, x1);
    const float max_x = std::max(x0, x1);
    const float min_y = std::min(y0, y1);
    const float max_y = std::max(y0, y1);
    return !(max_x < bounds.min_x || min_x > bounds.max_x ||
             max_y < bounds.min_y || min_y > bounds.max_y);
}

static bool is_abnormal_segment(float x0, float y0, float x1, float y1,
                                const RenderBounds& presentation) {
    if (!is_export_coord(x0, y0) || !is_export_coord(x1, y1)) return true;
    if (presentation.empty) return false;

    const float w = presentation.max_x - presentation.min_x;
    const float h = presentation.max_y - presentation.min_y;
    const float diag = std::sqrt(w * w + h * h);
    if (!std::isfinite(diag) || diag <= 0.0f) return false;

    const RenderBounds padded = expand_bounds(presentation, 0.12f);
    const bool intersects_presentation = segment_intersects_bounds(x0, y0, x1, y1, padded);

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(len)) return true;
    const bool inside0 = point_in_bounds(x0, y0, padded);
    const bool inside1 = point_in_bounds(x1, y1, padded);
    if (inside0 && inside1) return false;
    if (!intersects_presentation) {
        return len > diag * 0.05f;
    }

    if (len > diag * 1.0f) return true;
    if (len > diag * 0.5f) {
        const RenderBounds far_pad = expand_bounds(presentation, 0.40f);
        return !point_in_bounds(x0, y0, far_pad) ||
               !point_in_bounds(x1, y1, far_pad);
    }
    return false;
}

static size_t count_abnormal_segments(const std::vector<RenderBatch>& batches,
                                      const RenderBounds& presentation) {
    if (presentation.empty) return 0;
    size_t count = 0;
    for (const auto& batch : batches) {
        const auto& vd = batch.vertex_data;
        if (batch.topology == PrimitiveTopology::LineList) {
            for (size_t i = 0; i + 3 < vd.size(); i += 4) {
                if (is_abnormal_segment(vd[i], vd[i + 1], vd[i + 2], vd[i + 3], presentation)) {
                    count++;
                }
            }
        } else if (batch.topology == PrimitiveTopology::LineStrip) {
            if (!batch.entity_starts.empty()) {
                for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
                    size_t start = static_cast<size_t>(batch.entity_starts[ei]) * 2;
                    size_t end = (ei + 1 < batch.entity_starts.size())
                        ? static_cast<size_t>(batch.entity_starts[ei + 1]) * 2
                        : vd.size();
                    end = std::min(end, vd.size());
                    for (size_t i = start; i + 3 < end; i += 2) {
                        if (is_abnormal_segment(vd[i], vd[i + 1], vd[i + 2], vd[i + 3], presentation)) {
                            count++;
                        }
                    }
                }
            } else {
                for (size_t i = 0; i + 3 < vd.size(); i += 2) {
                    if (is_abnormal_segment(vd[i], vd[i + 1], vd[i + 2], vd[i + 3], presentation)) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

static size_t filter_abnormal_segments(RenderBatch& batch,
                                       const RenderBounds& presentation) {
    if (presentation.empty || batch.vertex_data.empty()) return 0;

    size_t removed = 0;
    const RenderBounds far_window = expand_bounds(presentation, 0.50f);
    const float presentation_diag = std::max(render_bounds_diag(presentation), 1.0f);
    const auto& vd = batch.vertex_data;
    std::vector<float> filtered_vertices;
    std::vector<uint32_t> filtered_starts;

    if (batch.topology == PrimitiveTopology::TriangleList ||
        batch.topology == PrimitiveTopology::TriangleStrip) {
        const RenderBounds bb = batch_render_bounds(batch);
        if (!render_bounds_intersects(bb, far_window) ||
            render_bounds_diag(bb) > presentation_diag * 3.0f) {
            removed = std::max<size_t>(1, batch.vertex_data.size() / 6);
            batch.vertex_data.clear();
            batch.index_data.clear();
        }
        return removed;
    }

    if (batch.topology == PrimitiveTopology::LineList) {
        filtered_vertices.reserve(vd.size());
        for (size_t i = 0; i + 3 < vd.size(); i += 4) {
            if (!segment_intersects_bounds(vd[i], vd[i + 1], vd[i + 2], vd[i + 3],
                                           far_window) ||
                is_abnormal_segment(vd[i], vd[i + 1], vd[i + 2], vd[i + 3],
                                    presentation)) {
                removed++;
                continue;
            }
            filtered_vertices.insert(filtered_vertices.end(),
                                     {vd[i], vd[i + 1], vd[i + 2], vd[i + 3]});
        }
        batch.vertex_data = std::move(filtered_vertices);
        batch.index_data.clear();
        return removed;
    }

    if (batch.topology != PrimitiveTopology::LineStrip ||
        batch.entity_starts.empty()) {
        return 0;
    }

    filtered_vertices.reserve(vd.size());
    filtered_starts.reserve(batch.entity_starts.size());
    auto append_kept_segment = [&](float x0, float y0, float x1, float y1,
                                   bool& run_open) {
        if (!run_open) {
            filtered_starts.push_back(static_cast<uint32_t>(filtered_vertices.size() / 2));
            filtered_vertices.push_back(x0);
            filtered_vertices.push_back(y0);
            run_open = true;
        }
        filtered_vertices.push_back(x1);
        filtered_vertices.push_back(y1);
    };
    for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
        size_t start_vertex = static_cast<size_t>(batch.entity_starts[ei]);
        size_t end_vertex = (ei + 1 < batch.entity_starts.size())
            ? static_cast<size_t>(batch.entity_starts[ei + 1])
            : vd.size() / 2;
        start_vertex = std::min(start_vertex, vd.size() / 2);
        end_vertex = std::min(end_vertex, vd.size() / 2);
        if (end_vertex <= start_vertex) continue;

        bool kept_any = false;
        bool run_open = false;
        for (size_t vi = start_vertex; vi + 1 < end_vertex; ++vi) {
            const size_t off = vi * 2;
            const float x0 = vd[off];
            const float y0 = vd[off + 1];
            const float x1 = vd[off + 2];
            const float y1 = vd[off + 3];
            if (!segment_intersects_bounds(x0, y0, x1, y1, far_window) ||
                is_abnormal_segment(x0, y0, x1, y1, presentation)) {
                removed++;
                run_open = false;
                continue;
            }
            append_kept_segment(x0, y0, x1, y1, run_open);
            kept_any = true;
        }
        if (!kept_any && end_vertex > start_vertex) {
            removed++;
        }
    }

    batch.vertex_data = std::move(filtered_vertices);
    batch.entity_starts = std::move(filtered_starts);
    batch.index_data.clear();
    return removed;
}

static const char* space_to_json(DrawingSpace space) {
    switch (space) {
        case DrawingSpace::ModelSpace: return "model";
        case DrawingSpace::PaperSpace: return "paper";
        case DrawingSpace::Unknown:
        default: return "unknown";
    }
}

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
    return block_classify::is_model_or_paper_space(name);
}

static RenderBounds adaptive_visible_bounds(const std::vector<BoundsPoint>& points,
                                            const RenderBounds& fallback) {
    if (points.size() < 4) return fallback;

    constexpr size_t kMaxSamples = 100000;
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

    const float q05x = quantile(xs, 0.05f);
    const float q10x = quantile(xs, 0.10f);
    const float q25x = quantile(xs, 0.25f);
    const float q75x = quantile(xs, 0.75f);
    const float q90x = quantile(xs, 0.90f);
    const float q95x = quantile(xs, 0.95f);
    const float q05y = quantile(ys, 0.05f);
    const float q10y = quantile(ys, 0.10f);
    const float q25y = quantile(ys, 0.25f);
    const float q75y = quantile(ys, 0.75f);
    const float q90y = quantile(ys, 0.90f);
    const float q95y = quantile(ys, 0.95f);

    const float iqr_x = q75x - q25x;
    const float iqr_y = q75y - q25y;
    const float min_gap_x = std::max(iqr_x * 1.5f, 1.0f);
    const float min_gap_y = std::max(iqr_y * 1.5f, 1.0f);

    // Adaptive expansion from IQR core, checking density gaps at each level.
    float lo_x = q25x, hi_x = q75x;
    float lo_y = q25y, hi_y = q75y;

    if (q25x - q10x < min_gap_x) lo_x = q10x;
    if (q90x - q75x < min_gap_x) hi_x = q90x;
    if (q25y - q10y < min_gap_y) lo_y = q10y;
    if (q90y - q75y < min_gap_y) hi_y = q90y;

    if (lo_x == q10x && q10x - q05x < min_gap_x) lo_x = q05x;
    if (hi_x == q90x && q95x - q90x < min_gap_x) hi_x = q95x;
    if (lo_y == q10y && q10y - q05y < min_gap_y) lo_y = q05y;
    if (hi_y == q90y && q95y - q90y < min_gap_y) hi_y = q95y;

    const float range_x = (hi_x - lo_x) > 0 ? (hi_x - lo_x) : 1.0f;
    const float range_y = (hi_y - lo_y) > 0 ? (hi_y - lo_y) : 1.0f;

    // Dense annotation/table bands often sit at the sheet edge. A pure
    // percentile crop removes them together with true outliers, so preserve
    // nearby dense edge clusters when they overlap the main horizontal span.
    const float x_band_min = lo_x - range_x * 0.20f;
    const float x_band_max = hi_x + range_x * 0.20f;
    const size_t dense_threshold = std::max<size_t>(40, xs.size() / 250);
    auto include_dense_y_edge = [&](bool lower_edge) {
        size_t count = 0;
        float edge = lower_edge ? lo_y : hi_y;
        const float max_gap = std::max(range_y * 0.60f, 50.0f);
        for (const auto& p : points) {
            if (!is_export_coord(p.x, p.y)) continue;
            if (p.x < x_band_min || p.x > x_band_max) continue;
            if (lower_edge) {
                if (p.y >= lo_y || lo_y - p.y > max_gap) continue;
                edge = std::min(edge, p.y);
            } else {
                if (p.y <= hi_y || p.y - hi_y > max_gap) continue;
                edge = std::max(edge, p.y);
            }
            ++count;
        }
        if (count >= dense_threshold) {
            if (lower_edge) lo_y = edge;
            else hi_y = edge;
        }
    };
    include_dense_y_edge(true);
    include_dense_y_edge(false);

    const float final_range_x = (hi_x - lo_x) > 0 ? (hi_x - lo_x) : range_x;
    const float final_range_y = (hi_y - lo_y) > 0 ? (hi_y - lo_y) : range_y;

    RenderBounds result;
    const float margin_x = final_range_x * 0.05f;
    const float margin_y = final_range_y * 0.05f;
    result.expand(lo_x - margin_x, lo_y - margin_y);
    result.expand(hi_x + margin_x, hi_y + margin_y);

    if (!result.empty) return result;
    return fallback;
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

static RenderBounds bounds3d_to_render_bounds(const Bounds3d& bounds) {
    RenderBounds out;
    if (bounds.is_empty()) return out;
    out.expand(bounds.min.x, bounds.min.y);
    out.expand(bounds.max.x, bounds.max.y);
    return out;
}

static float render_bounds_major(const RenderBounds& bounds) {
    if (bounds.empty) return 0.0f;
    return std::max(bounds.max_x - bounds.min_x, bounds.max_y - bounds.min_y);
}

static RenderBounds transform_bounds_2d(const Bounds3d& bounds, const Matrix4x4& xform) {
    RenderBounds out;
    if (bounds.is_empty()) return out;
    const Vec3 corners[] = {
        {bounds.min.x, bounds.min.y, 0.0f},
        {bounds.min.x, bounds.max.y, 0.0f},
        {bounds.max.x, bounds.min.y, 0.0f},
        {bounds.max.x, bounds.max.y, 0.0f},
    };
    for (const Vec3& corner : corners) {
        Vec3 p = xform.transform_point(corner);
        out.expand(p.x, p.y);
    }
    return out;
}

static bool center_in_expanded_bounds(
    const RenderBounds& candidate,
    const RenderBounds& reference,
    float ratio) {
    if (candidate.empty || reference.empty) return false;
    const float rw = reference.max_x - reference.min_x;
    const float rh = reference.max_y - reference.min_y;
    const float pad_x = std::max(1.0f, rw * ratio);
    const float pad_y = std::max(1.0f, rh * ratio);
    const float cx = (candidate.min_x + candidate.max_x) * 0.5f;
    const float cy = (candidate.min_y + candidate.max_y) * 0.5f;
    return cx >= reference.min_x - pad_x &&
           cx <= reference.max_x + pad_x &&
           cy >= reference.min_y - pad_y &&
           cy <= reference.max_y + pad_y;
}

static std::vector<int32_t> local_entities_for_scaled_dwg_insert(
    const Block& block,
    const InsertEntity& insert,
    const std::vector<EntityVariant>& entities,
    const RenderBounds& raw_scene_bounds,
    const RenderBounds& header_bounds) {
    std::vector<int32_t> local_indices;
    if (block.entity_indices.empty() || raw_scene_bounds.empty) return local_indices;

    const Matrix4x4 base_offset = Matrix4x4::translation_2d(
        -block.base_point.x, -block.base_point.y);
    const Matrix4x4 insert_xform = Matrix4x4::affine_2d(
        insert.x_scale, insert.y_scale, insert.rotation,
        insert.insertion_point.x, insert.insertion_point.y);
    const Matrix4x4 final_xform = base_offset * insert_xform;

    const float raw_major = std::max(1.0f, render_bounds_major(raw_scene_bounds));
    const float header_major = render_bounds_major(header_bounds);
    const float local_radius = std::max(5000.0f, header_major * 20.0f);
    const float bx = std::isfinite(block.base_point.x) ? block.base_point.x : 0.0f;
    const float by = std::isfinite(block.base_point.y) ? block.base_point.y : 0.0f;

    for (int32_t eidx : block.entity_indices) {
        if (eidx < 0 || static_cast<size_t>(eidx) >= entities.size()) continue;
        const auto& entity = entities[static_cast<size_t>(eidx)];
        const Bounds3d& eb = entity.bounds();
        if (eb.is_empty()) continue;

        const float cx = (eb.min.x + eb.max.x) * 0.5f;
        const float cy = (eb.min.y + eb.max.y) * 0.5f;
        const float dist_from_block_base =
            std::sqrt((cx - bx) * (cx - bx) + (cy - by) * (cy - by));
        if (!std::isfinite(dist_from_block_base) ||
            dist_from_block_base > local_radius) {
            continue;
        }

        const RenderBounds transformed = transform_bounds_2d(eb, final_xform);
        if (transformed.empty) continue;
        const float transformed_major = render_bounds_major(transformed);
        if (!std::isfinite(transformed_major) ||
            transformed_major > raw_major * 0.75f) {
            continue;
        }
        if (!center_in_expanded_bounds(transformed, raw_scene_bounds, 0.10f)) {
            continue;
        }
        local_indices.push_back(eidx);
    }
    return local_indices;
}

static bool should_render_dwg_block_direct(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert,
    float scene_extent = 5000.0f) {
    return block_classify::should_render_direct(block, entities, has_scaled_insert, scene_extent);
}

static bool should_merge_dwg_block_header_entities(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert) {
    return block_classify::should_merge_header_entities(block, entities, has_scaled_insert);
}

static std::string escape_json(const std::string& s) {
    std::string out;
    auto append_byte_escape = [&out](unsigned char c) {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
        out += buf;
    };
    auto is_continuation = [](unsigned char c) {
        return (c & 0xC0u) == 0x80u;
    };
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; ++i; break;
            case '\\': out += "\\\\"; ++i; break;
            case '\b': out += "\\b"; ++i; break;
            case '\f': out += "\\f"; ++i; break;
            case '\n': out += "\\n"; ++i; break;
            case '\r': out += "\\r"; ++i; break;
            case '\t': out += "\\t"; ++i; break;
            default:
                if (c < 0x20) {
                    append_byte_escape(c);
                    ++i;
                } else if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                    ++i;
                } else {
                    size_t len = 0;
                    uint32_t codepoint = 0;
                    if ((c & 0xE0u) == 0xC0u) {
                        len = 2;
                        codepoint = c & 0x1Fu;
                    } else if ((c & 0xF0u) == 0xE0u) {
                        len = 3;
                        codepoint = c & 0x0Fu;
                    } else if ((c & 0xF8u) == 0xF0u) {
                        len = 4;
                        codepoint = c & 0x07u;
                    }
                    bool valid = len > 0 && i + len <= s.size();
                    for (size_t j = 1; valid && j < len; ++j) {
                        const unsigned char cc = static_cast<unsigned char>(s[i + j]);
                        valid = is_continuation(cc);
                        codepoint = (codepoint << 6) | (cc & 0x3Fu);
                    }
                    if (valid) {
                        if ((len == 2 && codepoint < 0x80u) ||
                            (len == 3 && codepoint < 0x800u) ||
                            (len == 4 && (codepoint < 0x10000u || codepoint > 0x10FFFFu)) ||
                            (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
                            valid = false;
                        }
                    }
                    if (valid) {
                        out.append(s, i, len);
                        i += len;
                    } else {
                        append_byte_escape(c);
                        ++i;
                    }
                }
        }
    }
    return out;
}

// Format a coordinate with sufficient precision for large CAD coordinates.
// Uses %.10g to preserve ~1mm precision for coordinates in the millions range.
static std::string fmt_coord(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
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

    // Debug: count entity types
    {
        std::unordered_map<std::string, size_t> type_counts;
        for (const auto& e : entities) {
            switch (e.type()) {
                case EntityType::Line: type_counts["Line"]++; break;
                case EntityType::Circle: type_counts["Circle"]++; break;
                case EntityType::Arc: type_counts["Arc"]++; break;
                case EntityType::Polyline: type_counts["Polyline"]++; break;
                case EntityType::LwPolyline: type_counts["LwPolyline"]++; break;
                case EntityType::Spline: type_counts["Spline"]++; break;
                case EntityType::Text: type_counts["Text"]++; break;
                case EntityType::MText: type_counts["MText"]++; break;
                case EntityType::Dimension: type_counts["Dimension"]++; break;
                case EntityType::Hatch: type_counts["Hatch"]++; break;
                case EntityType::Insert: type_counts["Insert"]++; break;
                case EntityType::Point: type_counts["Point"]++; break;
                case EntityType::Ellipse: type_counts["Ellipse"]++; break;
                case EntityType::Ray: type_counts["Ray"]++; break;
                case EntityType::XLine: type_counts["XLine"]++; break;
                case EntityType::Viewport: type_counts["Viewport"]++; break;
                case EntityType::Solid: type_counts["Solid"]++; break;
                case EntityType::Leader: type_counts["Leader"]++; break;
                case EntityType::Tolerance: type_counts["Tolerance"]++; break;
                case EntityType::MLine: type_counts["MLine"]++; break;
                case EntityType::Multileader: type_counts["Multileader"]++; break;
            }
        }
        printf("Entity types:\n");
        for (const auto& [name, count] : type_counts) {
            printf("  %s: %zu\n", name.c_str(), count);
        }
        // Debug: check bounds validity for key types
        size_t zero_bounds_line = 0, zero_bounds_arc = 0, zero_bounds_circle = 0;
        size_t nonzero_bounds_line = 0, nonzero_bounds_arc = 0, nonzero_bounds_circle = 0;
        for (const auto& e : entities) {
            bool empty = e.bounds().is_empty();
            switch (e.type()) {
                case EntityType::Line:
                    if (empty) zero_bounds_line++; else nonzero_bounds_line++;
                    break;
                case EntityType::Arc:
                    if (empty) zero_bounds_arc++; else nonzero_bounds_arc++;
                    break;
                case EntityType::Circle:
                    if (empty) zero_bounds_circle++; else nonzero_bounds_circle++;
                    break;
                default: break;
            }
        }
        printf("Line: empty_bounds=%zu nonempty_bounds=%zu\n", zero_bounds_line, nonzero_bounds_line);
        printf("Arc: empty_bounds=%zu nonempty_bounds=%zu\n", zero_bounds_arc, nonzero_bounds_arc);
        printf("Circle: empty_bounds=%zu nonempty_bounds=%zu\n", zero_bounds_circle, nonzero_bounds_circle);
        // Debug: sample some Arc entities with empty bounds
        size_t arc_samples = 0;
        for (const auto& e : entities) {
            if (e.type() != EntityType::Arc || !e.bounds().is_empty()) continue;
            if (arc_samples >= 5) break;
            auto* arc = std::get_if<2>(&e.data);
            if (arc) {
                printf("Arc sample %zu: center=(%.3f,%.3f,%.3f) radius=%.3f angles=(%.3f,%.3f)\n",
                       arc_samples, arc->center.x, arc->center.y, arc->center.z,
                       arc->radius, arc->start_angle, arc->end_angle);
            }
            arc_samples++;
        }
    }

    // Build set of entity indices that belong to block definitions.
    // DXF uses local-space block definitions expanded through INSERT. For DWG,
    // some parsed block children are already world-space geometry while others
    // are local definitions. Classify per block so local DWGs still expand
    // through INSERT, while large world-space blocks render directly.
    std::unordered_set<int32_t> block_entity_indices;
    std::unordered_set<int32_t> direct_block_indices;
    std::vector<int32_t> local_insert_block_indices;
    bool insert_expansion_active = false;

    if (is_dwg) {
        auto& blocks = scene.blocks();
        const size_t original_block_count = blocks.size();
        const RenderBounds raw_scene_bounds = bounds3d_to_render_bounds(scene.total_bounds());
        const float scene_extent = raw_scene_bounds.empty ? 5000.0f :
            std::max(raw_scene_bounds.max_x - raw_scene_bounds.min_x,
                     raw_scene_bounds.max_y - raw_scene_bounds.min_y);
        std::vector<bool> block_has_scaled_insert(original_block_count, false);
        std::vector<int32_t> representative_scaled_insert(original_block_count, -1);
        std::vector<bool> block_should_direct(original_block_count, false);
        std::vector<std::unordered_set<int32_t>> local_entities_for_insert(original_block_count);
        local_insert_block_indices.assign(original_block_count, -1);
        for (int32_t eidx = 0; eidx < static_cast<int32_t>(entities.size()); ++eidx) {
            const auto& entity = entities[static_cast<size_t>(eidx)];
            if (entity.type() != EntityType::Insert) continue;
            const auto* ins = std::get_if<InsertEntity>(&entity.data);
            if (!ins || ins->block_index < 0 ||
                static_cast<size_t>(ins->block_index) >= block_has_scaled_insert.size()) {
                continue;
            }
            const float max_scale = std::max(std::abs(ins->x_scale), std::abs(ins->y_scale));
            if (std::isfinite(max_scale) && max_scale > 4.0f) {
                block_has_scaled_insert[static_cast<size_t>(ins->block_index)] = true;
                if (representative_scaled_insert[static_cast<size_t>(ins->block_index)] < 0) {
                    representative_scaled_insert[static_cast<size_t>(ins->block_index)] = eidx;
                }
            }
        }
        for (size_t bi = 0; bi < original_block_count; ++bi) {
            auto& block = blocks[bi];
            const bool scaled_insert =
                bi < block_has_scaled_insert.size() && block_has_scaled_insert[bi];
            bool merge_header_owned =
                should_merge_dwg_block_header_entities(block, entities, scaled_insert);
            // When BLOCK/ENDBLK bracketing produced empty entity_indices,
            // header_owned_entity_indices is the only source of block entities.
            // Always merge in this case regardless of other merge conditions.
            if (!merge_header_owned &&
                block.entity_indices.empty() &&
                block.header_owned_entity_indices.size() >= 1) {
                merge_header_owned = true;
            }

            // For non-direct blocks, merge header-owned entities into
            // entity_indices BEFORE the should_render_direct check so the
            // bounds evaluation sees all block geometry.  (Direct blocks keep
            // header-owned entities separate for the local-insert path.)
            block_should_direct[bi] = false;
            if (merge_header_owned) {
                // Tentatively merge to check direct-ness.
                std::unordered_set<int32_t> existing(
                    block.entity_indices.begin(), block.entity_indices.end());
                for (int32_t eidx : block.header_owned_entity_indices) {
                    if (eidx < 0 || static_cast<size_t>(eidx) >= entities.size()) continue;
                    if (!existing.insert(eidx).second) continue;
                    block.entity_indices.push_back(eidx);
                    const Bounds3d entity_bounds = entities[static_cast<size_t>(eidx)].bounds();
                    if (!entity_bounds.is_empty()) {
                        block.bounds.expand(entity_bounds.min);
                        block.bounds.expand(entity_bounds.max);
                    }
                }
            }
            block_should_direct[bi] =
                should_render_dwg_block_direct(block, entities, scaled_insert, scene_extent);

            if (block_should_direct[bi] && scaled_insert &&
                representative_scaled_insert[bi] >= 0 &&
                static_cast<size_t>(representative_scaled_insert[bi]) < entities.size()) {
                const auto& insert_entity =
                    entities[static_cast<size_t>(representative_scaled_insert[bi])];
                const auto* representative_insert =
                    std::get_if<InsertEntity>(&insert_entity.data);
                if (representative_insert) {
                    const RenderBounds header_bounds =
                        bounds_for_entity_indices(entities, block.header_owned_entity_indices);
                    for (int32_t local_eidx : local_entities_for_scaled_dwg_insert(
                             block, *representative_insert, entities,
                             raw_scene_bounds, header_bounds)) {
                        local_entities_for_insert[bi].insert(local_eidx);
                    }
                }
            }

            if (block_should_direct[bi]) {
                if (merge_header_owned) {
                    for (int32_t eidx : block.header_owned_entity_indices) {
                        if (eidx >= 0 && static_cast<size_t>(eidx) < entities.size()) {
                            local_entities_for_insert[bi].insert(eidx);
                        }
                    }
                }
                if (!local_entities_for_insert[bi].empty()) {
                    Block local_block = block;
                    local_block.name += "$LOCAL_INSERT";
                    local_block.entity_indices.assign(
                        local_entities_for_insert[bi].begin(),
                        local_entities_for_insert[bi].end());
                    std::sort(local_block.entity_indices.begin(), local_block.entity_indices.end());
                    local_block.header_owned_entity_indices.clear();
                    local_block.bounds = Bounds3d::empty();
                    for (int32_t eidx : local_block.entity_indices) {
                        if (eidx < 0 || static_cast<size_t>(eidx) >= entities.size()) continue;
                        const Bounds3d entity_bounds = entities[static_cast<size_t>(eidx)].bounds();
                        if (!entity_bounds.is_empty()) {
                            local_block.bounds.expand(entity_bounds.min);
                            local_block.bounds.expand(entity_bounds.max);
                        }
                    }
                    local_insert_block_indices[bi] = scene.add_block(std::move(local_block));
                }
            }
        }
        for (size_t bi = 0; bi < original_block_count; ++bi) {
            const auto& block = blocks[bi];
            if (block_should_direct[bi]) {
                direct_block_indices.insert(static_cast<int32_t>(bi));
                for (int32_t ei : block.entity_indices) {
                    if (ei >= 0 && static_cast<size_t>(ei) < entities.size()) {
                        if (local_entities_for_insert[bi].count(ei)) {
                            block_entity_indices.insert(ei);
                            entities[static_cast<size_t>(ei)].header.in_block = true;
                        } else {
                            entities[static_cast<size_t>(ei)].header.in_block = false;
                        }
                    }
                }
                if (local_insert_block_indices[bi] >= 0) {
                    for (int32_t ei : block.header_owned_entity_indices) {
                        if (ei < 0 || static_cast<size_t>(ei) >= entities.size()) continue;
                        block_entity_indices.insert(ei);
                        entities[static_cast<size_t>(ei)].header.in_block = true;
                    }
                    insert_expansion_active = true;
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
        const char* debug_env = std::getenv("FT_DWG_DEBUG");
        if (debug_env && debug_env[0] != '\0' && std::strcmp(debug_env, "0") != 0) {
            size_t printed_inserts = 0;
            for (const auto& entity : entities) {
                if (entity.type() != EntityType::Insert) continue;
                const auto* ins = std::get_if<InsertEntity>(&entity.data);
                if (!ins || ins->block_index < 0 ||
                    static_cast<size_t>(ins->block_index) >= scene.blocks().size()) {
                    continue;
                }
                const auto& block = scene.blocks()[static_cast<size_t>(ins->block_index)];
                RenderBounds bb = bounds_for_entity_indices(entities, block.entity_indices);
                RenderBounds hbb = bounds_for_entity_indices(entities, block.header_owned_entity_indices);
                fprintf(stderr,
                        "[DWG] export_insert block='%s' block_index=%d entities=%zu header_owned=%zu direct=%u localBlock=%d ins=(%.3f,%.3f) base=(%.3f,%.3f) scale=(%.6f,%.6f) rot=%.6f block_bounds=(%.3f,%.3f)-(%.3f,%.3f) header_bounds=(%.3f,%.3f)-(%.3f,%.3f)\n",
                        block.name.c_str(),
                        ins->block_index,
                        block.entity_indices.size(),
                        block.header_owned_entity_indices.size(),
                        direct_block_indices.count(ins->block_index) ? 1u : 0u,
                        (static_cast<size_t>(ins->block_index) < local_insert_block_indices.size())
                            ? local_insert_block_indices[static_cast<size_t>(ins->block_index)]
                            : -1,
                        ins->insertion_point.x,
                        ins->insertion_point.y,
                        block.base_point.x,
                        block.base_point.y,
                        ins->x_scale,
                        ins->y_scale,
                        ins->rotation,
                        bb.empty ? 0.0f : bb.min_x,
                        bb.empty ? 0.0f : bb.min_y,
                        bb.empty ? 0.0f : bb.max_x,
                        bb.empty ? 0.0f : bb.max_y,
                        hbb.empty ? 0.0f : hbb.min_x,
                        hbb.empty ? 0.0f : hbb.min_y,
                        hbb.empty ? 0.0f : hbb.max_x,
                        hbb.empty ? 0.0f : hbb.max_y);
                if (++printed_inserts >= 40) break;
            }
        }
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
        float rect_width = 0.0f;
        float rect_height = 0.0f;
        std::string text;
        uint8_t r, g, b;
        int32_t layer_index;
        std::string layer_name;
        DrawingSpace space = DrawingSpace::Unknown;
        int32_t layout_index = -1;
        int32_t viewport_index = -1;
        int32_t style_index = -1;
        int32_t alignment = 0;
        const char* kind = "text";
    };
    std::vector<TextEntry> text_entries;

    auto transform_scale_rotation = [](const Matrix4x4& xform) {
        const Vec3 origin = xform.transform_point({0.0f, 0.0f, 0.0f});
        const Vec3 ux = xform.transform_point({1.0f, 0.0f, 0.0f});
        const Vec3 uy = xform.transform_point({0.0f, 1.0f, 0.0f});
        const float sx = std::hypot(ux.x - origin.x, ux.y - origin.y);
        const float sy = std::hypot(uy.x - origin.x, uy.y - origin.y);
        const float rot = std::atan2(ux.y - origin.y, ux.x - origin.x);
        struct Result {
            float sx;
            float sy;
            float rotation;
        };
        return Result{
            std::isfinite(sx) && sx > 0.0f ? sx : 1.0f,
            std::isfinite(sy) && sy > 0.0f ? sy : 1.0f,
            std::isfinite(rot) ? rot : 0.0f,
        };
    };

    auto resolve_text_color = [&](const EntityVariant& entity,
                                  const EntityHeader* parent_header = nullptr) {
        if (entity.header.has_true_color) {
            return entity.header.true_color;
        }
        if (entity.header.color_override != 256 && entity.header.color_override != 0) {
            return Color::from_aci(entity.header.color_override);
        }
        if (entity.header.color_override == 0 && parent_header != nullptr) {
            if (parent_header->has_true_color) {
                return parent_header->true_color;
            }
            if (parent_header->color_override != 256 && parent_header->color_override != 0) {
                return Color::from_aci(parent_header->color_override);
            }
        }
        int32_t li = entity.header.layer_index;
        if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
            return layers[static_cast<size_t>(li)].color;
        }
        return Color::white();
    };

    auto layer_name_for = [&](int32_t layer_index) {
        if (layer_index >= 0 && static_cast<size_t>(layer_index) < layers.size()) {
            return layers[static_cast<size_t>(layer_index)].name;
        }
        return std::string{};
    };

    auto append_text_entry = [&](const EntityVariant& entity,
                                 const Matrix4x4& xform,
                                 const EntityHeader* parent_header = nullptr) {
        const auto scaled = transform_scale_rotation(xform);
        if (entity.type() == EntityType::Text || entity.type() == EntityType::MText) {
            const TextEntity* te = nullptr;
            if (entity.type() == EntityType::Text) {
                te = std::get_if<6>(&entity.data);
            } else {
                te = std::get_if<7>(&entity.data);
            }
            if (!te || te->height <= 0.0f || te->text.empty() ||
                !std::isfinite(te->height) || !std::isfinite(te->rotation) ||
                !std::isfinite(te->width_factor)) {
                return;
            }
            const Vec3 pos = xform.transform_point(te->insertion_point);
            if (!is_export_coord(pos.x, pos.y)) return;

            TextEntry entry;
            entry.x = pos.x;
            entry.y = pos.y;
            entry.height = te->height * scaled.sy;
            entry.rotation = te->rotation + scaled.rotation;
            entry.width_factor = (te->width_factor > 0.0f ? te->width_factor : 1.0f) *
                                 (scaled.sy > 1.0e-6f ? scaled.sx / scaled.sy : 1.0f);
            entry.rect_width = (te->rect_width > 0.0f && std::isfinite(te->rect_width))
                ? te->rect_width * scaled.sx
                : 0.0f;
            entry.rect_height = (te->rect_height > 0.0f && std::isfinite(te->rect_height))
                ? te->rect_height * scaled.sy
                : 0.0f;
            entry.text = te->text;
            entry.layer_index = entity.header.layer_index;
            entry.space = entity.header.space;
            entry.layout_index = entity.header.layout_index;
            entry.viewport_index = entity.header.viewport_index;
            entry.style_index = te->text_style_index;
            entry.alignment = te->alignment;
            entry.kind = (entity.type() == EntityType::MText) ? "mtext" : "text";

            Color text_color = resolve_text_color(entity, parent_header);
            entry.r = text_color.r;
            entry.g = text_color.g;
            entry.b = text_color.b;
            entry.layer_name = layer_name_for(entry.layer_index);
            text_entries.push_back(std::move(entry));
            return;
        }

        if (entity.type() == EntityType::Dimension) {
            const auto* dim = std::get_if<8>(&entity.data);
            if (!dim || dim->text.empty() || dim->text == "<>" || dim->text == " " ||
                !std::isfinite(dim->rotation)) {
                return;
            }
            const Vec3 pos = xform.transform_point(dim->text_midpoint);
            if (!is_export_coord(pos.x, pos.y)) return;

            TextEntry entry;
            entry.x = pos.x;
            entry.y = pos.y;
            entry.height = 3.0f * scaled.sy;
            entry.rotation = dim->rotation + scaled.rotation;
            entry.width_factor = scaled.sy > 1.0e-6f ? scaled.sx / scaled.sy : 1.0f;
            entry.text = dim->text;
            entry.kind = "dimension";
            entry.layer_index = entity.header.layer_index;
            entry.space = entity.header.space;
            entry.layout_index = entity.header.layout_index;
            entry.viewport_index = entity.header.viewport_index;

            Color text_color = resolve_text_color(entity, parent_header);
            entry.r = text_color.r;
            entry.g = text_color.g;
            entry.b = text_color.b;
            entry.layer_name = layer_name_for(entry.layer_index);
            text_entries.push_back(std::move(entry));
        }
    };

    std::function<void(const EntityVariant&, const Matrix4x4&, int, const EntityHeader*)>
        collect_insert_text;
    collect_insert_text = [&](const EntityVariant& entity,
                              const Matrix4x4& xform,
                              int depth,
                              const EntityHeader* parent_header) {
        if (depth > 16 || !entity.is_visible()) return;
        if (entity.type() != EntityType::Insert) {
            append_text_entry(entity, xform, parent_header);
            return;
        }

        const auto* ins = std::get_if<InsertEntity>(&entity.data);
        if (!ins || ins->block_index < 0 ||
            static_cast<size_t>(ins->block_index) >= scene.blocks().size()) {
            return;
        }
        const Block& block = scene.blocks()[static_cast<size_t>(ins->block_index)];
        if (block.entity_indices.empty()) return;

        float bpx = std::isfinite(block.base_point.x) ? block.base_point.x : 0.0f;
        float bpy = std::isfinite(block.base_point.y) ? block.base_point.y : 0.0f;
        Matrix4x4 base_offset = Matrix4x4::translation_2d(-bpx, -bpy);
        Matrix4x4 insert_xform = Matrix4x4::affine_2d(
            ins->x_scale, ins->y_scale, ins->rotation,
            ins->insertion_point.x, ins->insertion_point.y);

        const int32_t total_instances = ins->column_count * ins->row_count;
        constexpr int32_t MAX_INSTANCES = 100;
        int32_t col_limit = ins->column_count;
        int32_t row_limit = ins->row_count;
        if (total_instances > MAX_INSTANCES) {
            float sc = std::sqrt(static_cast<float>(MAX_INSTANCES) /
                                 static_cast<float>(total_instances));
            col_limit = std::max(1, static_cast<int32_t>(ins->column_count * sc));
            row_limit = std::max(1, static_cast<int32_t>(ins->row_count * sc));
        }
        col_limit = std::max(1, col_limit);
        row_limit = std::max(1, row_limit);

        for (int32_t col = 0; col < col_limit; ++col) {
            for (int32_t row = 0; row < row_limit; ++row) {
                Matrix4x4 final_xform = base_offset * insert_xform *
                    Matrix4x4::translation_2d(
                        static_cast<float>(col) * ins->column_spacing,
                        static_cast<float>(row) * ins->row_spacing) * xform;
                for (int32_t ei : block.entity_indices) {
                    if (ei < 0 || static_cast<size_t>(ei) >= entities.size()) continue;
                    const auto& child = entities[static_cast<size_t>(ei)];
                    collect_insert_text(child, final_xform, depth + 1, &entity.header);
                }
            }
        }
    };

    // Rebuild spatial index for fast bounds queries
    scene.rebuild_spatial_index();

    // Tessellate all entities into draw commands
    RenderBatcher batcher;
    if (is_dwg) {
        batcher.set_tessellation_quality(16.0f);
    }
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
            if (ins && direct_block_indices.count(ins->block_index)) {
                const int32_t local_block_index =
                    (ins->block_index >= 0 &&
                     static_cast<size_t>(ins->block_index) < local_insert_block_indices.size())
                        ? local_insert_block_indices[static_cast<size_t>(ins->block_index)]
                        : -1;
                if (local_block_index >= 0) {
                    EntityVariant local_insert = entity;
                    auto* local_ins = std::get_if<InsertEntity>(&local_insert.data);
                    if (local_ins) {
                        local_ins->block_index = local_block_index;
                        collect_insert_text(local_insert, Matrix4x4::identity(), 0, nullptr);
                        batcher.submit_entity(local_insert, scene);
                    }
                }
                continue;
            }
        }

        // Extract text entities for separate rendering. INSERT-owned block
        // text must be expanded with the same transform as RenderBatcher;
        // otherwise CAD tables/title blocks only show underline geometry.
        if (entity.type() == EntityType::Insert) {
            collect_insert_text(entity, Matrix4x4::identity(), 0, nullptr);
        } else if (entity.type() == EntityType::Text ||
                   entity.type() == EntityType::MText ||
                   entity.type() == EntityType::Dimension) {
            append_text_entry(entity, Matrix4x4::identity(), nullptr);
        }

        batcher.submit_entity(entity, scene);
    }

    batcher.end_frame();

    // Export batches as JSON
    auto& batches = batcher.batches();

    auto infer_mechanical_detail_view_frames = [&]() -> size_t {
        const char* infer_env = std::getenv("FT_INFER_DETAIL_FRAMES");
        if (!(infer_env && std::strcmp(infer_env, "1") == 0)) {
            return 0;
        }
        if (!is_dwg || text_entries.empty() || layers.empty()) {
            return 0;
        }

        int32_t detail_layer_index = -1;
        for (size_t li = 0; li < layers.size(); ++li) {
            std::string layer_lower = layers[li].name;
            std::transform(layer_lower.begin(), layer_lower.end(), layer_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (layer_lower.find("detail") != std::string::npos &&
                layer_lower.find("boundary") != std::string::npos) {
                detail_layer_index = static_cast<int32_t>(li);
                break;
            }
        }
        if (detail_layer_index < 0) {
            return 0;
        }

        auto is_detail_view_title = [](const std::string& text) {
            std::string normalized = text;
            normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'),
                             normalized.end());
            return normalized.rfind("Detail ", 0) == 0 && normalized.size() <= 16;
        };

        auto batch_layer_index = [](const RenderBatch& batch) {
            return static_cast<int32_t>((batch.sort_key.key >> 48) & 0xFFFF);
        };

        std::vector<const TextEntry*> detail_titles;
        for (const auto& text : text_entries) {
            if (is_detail_view_title(text.text) &&
                std::isfinite(text.x) && std::isfinite(text.y)) {
                detail_titles.push_back(&text);
            }
        }

        size_t added = 0;
        for (const auto* text_ptr : detail_titles) {
            const auto& text = *text_ptr;
            if (!std::isfinite(text.x) || !std::isfinite(text.y)) continue;

            std::vector<std::pair<float, float>> candidate_points;
            const float half_x = std::max(1700.0f, text.height * 42.0f);
            const float min_y = text.y + std::max(60.0f, text.height * 1.2f);
            const float max_y = text.y + std::max(1250.0f, text.height * 30.0f);
            float left_partition = text.x - half_x;
            float right_partition = text.x + half_x;
            const float same_row_threshold = std::max(650.0f, text.height * 15.0f);
            for (const TextEntry* other_ptr : detail_titles) {
                if (other_ptr == text_ptr) continue;
                const auto& other = *other_ptr;
                if (std::abs(other.y - text.y) > same_row_threshold) continue;
                const float divider = (other.x + text.x) * 0.5f;
                if (other.x < text.x) {
                    left_partition = std::max(left_partition, divider);
                } else {
                    right_partition = std::min(right_partition, divider);
                }
            }

            for (const auto& batch : batches) {
                const int32_t li = batch_layer_index(batch);
                if (li == detail_layer_index) continue;
                std::string layer_lower;
                if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
                    layer_lower = layers[static_cast<size_t>(li)].name;
                    std::transform(layer_lower.begin(), layer_lower.end(), layer_lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                }
                if (layer_lower.find("symbol") != std::string::npos ||
                    layer_lower.find("leader") != std::string::npos ||
                    layer_lower.find("note") != std::string::npos ||
                    layer_lower.find("text") != std::string::npos ||
                    layer_lower.find("dim") != std::string::npos) {
                    continue;
                }

                const auto& vd = batch.vertex_data;
                for (size_t i = 0; i + 1 < vd.size(); i += 2) {
                    const float x = vd[i];
                    const float y = vd[i + 1];
                    if (!is_export_coord(x, y)) continue;
                    if (std::abs(x - text.x) > half_x) continue;
                    if (x < left_partition || x > right_partition) continue;
                    if (y < min_y || y > max_y) continue;
                    candidate_points.push_back({x, y});
                }
            }

            if (candidate_points.size() < 30) continue;

            // Native Mechanical detail-view metadata is still pending, so this
            // fallback must avoid one large "near the title" box. Split nearby
            // visible geometry into horizontal clusters and frame the cluster
            // most associated with the Detail A/B/C title.
            std::vector<float> sorted_xs;
            sorted_xs.reserve(candidate_points.size());
            for (const auto& point : candidate_points) sorted_xs.push_back(point.first);
            std::sort(sorted_xs.begin(), sorted_xs.end());

            struct XCluster {
                float min_x = 0.0f;
                float max_x = 0.0f;
                size_t count = 0;
            };
            std::vector<XCluster> clusters;
            const float cluster_gap = std::max(220.0f, text.height * 5.0f);
            XCluster current{sorted_xs.front(), sorted_xs.front(), 0};
            for (float x : sorted_xs) {
                if (x - current.max_x > cluster_gap && current.count > 0) {
                    clusters.push_back(current);
                    current = XCluster{x, x, 0};
                }
                current.max_x = x;
                current.count++;
            }
            if (current.count > 0) clusters.push_back(current);

            bool has_cluster = false;
            XCluster selected_cluster;
            float best_score = 0.0f;
            for (const XCluster& cluster : clusters) {
                if (cluster.count < 30) continue;
                const float center_x = (cluster.min_x + cluster.max_x) * 0.5f;
                const float score = std::abs(center_x - text.x) -
                                    static_cast<float>(std::min<size_t>(cluster.count, 500)) * 0.3f;
                if (!has_cluster || score < best_score) {
                    has_cluster = true;
                    selected_cluster = cluster;
                    best_score = score;
                }
            }
            if (!has_cluster) continue;

            std::vector<float> xs;
            std::vector<float> ys;
            for (const auto& point : candidate_points) {
                if (point.first < selected_cluster.min_x || point.first > selected_cluster.max_x) {
                    continue;
                }
                xs.push_back(point.first);
                ys.push_back(point.second);
            }
            if (xs.size() < 30 || ys.size() < 30) continue;
            std::sort(xs.begin(), xs.end());
            std::sort(ys.begin(), ys.end());
            auto percentile = [](const std::vector<float>& values, float p) {
                if (values.empty()) return 0.0f;
                const float clamped = std::clamp(p, 0.0f, 1.0f);
                const size_t idx = static_cast<size_t>(
                    std::floor(static_cast<float>(values.size() - 1) * clamped));
                return values[idx];
            };

            RenderBounds content;
            content.expand(percentile(xs, 0.005f), percentile(ys, 0.005f));
            content.expand(percentile(xs, 0.995f), percentile(ys, 0.995f));
            const float w = content.max_x - content.min_x;
            const float h = content.max_y - content.min_y;
            if (!std::isfinite(w) || !std::isfinite(h) || w < 250.0f || h < 180.0f) {
                continue;
            }

            const float pad = std::clamp(std::max(w, h) * 0.055f, 45.0f, 140.0f);
            content = expand_bounds(content, 0.0f);
            content.min_x -= pad;
            content.max_x += pad;
            content.min_y -= pad;
            content.max_y += pad;

            bool duplicate = false;
            for (const auto& batch : batches) {
                if (batch_layer_index(batch) != detail_layer_index) continue;
                if (batch.topology != PrimitiveTopology::LineStrip ||
                    batch.vertex_data.size() != 10) {
                    continue;
                }
                const auto& vd = batch.vertex_data;
                const bool closed_rect =
                    std::abs(vd[0] - vd[8]) < 1.0f &&
                    std::abs(vd[1] - vd[9]) < 1.0f;
                if (!closed_rect) continue;
                const RenderBounds existing = batch_render_bounds(batch);
                if (existing.empty) continue;
                const float existing_w = existing.max_x - existing.min_x;
                const float existing_h = existing.max_y - existing.min_y;
                const float ix0 = std::max(existing.min_x, content.min_x);
                const float iy0 = std::max(existing.min_y, content.min_y);
                const float ix1 = std::min(existing.max_x, content.max_x);
                const float iy1 = std::min(existing.max_y, content.max_y);
                const float intersection =
                    std::max(0.0f, ix1 - ix0) * std::max(0.0f, iy1 - iy0);
                const float min_area = std::max(1.0f, std::min(existing_w * existing_h, w * h));
                if (existing_w > w * 0.55f && existing_h > h * 0.55f &&
                    intersection / min_area > 0.55f) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            RenderBatch frame;
            frame.sort_key = RenderKey::make(
                static_cast<uint16_t>(detail_layer_index),
                static_cast<uint8_t>(PrimitiveTopology::LineList),
                0,
                static_cast<uint8_t>(EntityType::Polyline),
                0x00ffffu);
            frame.topology = PrimitiveTopology::LineList;
            frame.color = layers[static_cast<size_t>(detail_layer_index)].color;
            frame.line_width = std::max(0.07f, layers[static_cast<size_t>(detail_layer_index)].lineweight);
            frame.space = DrawingSpace::ModelSpace;
            frame.vertex_data = {
                content.min_x, content.min_y, content.max_x, content.min_y,
                content.max_x, content.min_y, content.max_x, content.max_y,
                content.max_x, content.max_y, content.min_x, content.max_y,
                content.min_x, content.max_y, content.min_x, content.min_y,
            };
            batches.push_back(std::move(frame));
            added++;
        }

        auto is_boundary_marker = [&](const TextEntry& text) {
            if (text.layer_index != detail_layer_index) return false;
            std::string normalized = text.text;
            normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                            [](unsigned char c) { return std::isspace(c); }),
                             normalized.end());
            if (normalized.empty() || normalized.size() > 2) return false;
            for (char ch : normalized) {
                if (!std::isalpha(static_cast<unsigned char>(ch))) return false;
            }
            return true;
        };

        auto overlaps_existing_detail_frame = [&](const RenderBounds& content) {
            const float w = content.max_x - content.min_x;
            const float h = content.max_y - content.min_y;
            for (const auto& batch : batches) {
                if (batch_layer_index(batch) != detail_layer_index) continue;
                if (batch.topology != PrimitiveTopology::LineList ||
                    batch.vertex_data.size() != 16) {
                    continue;
                }
                const RenderBounds existing = batch_render_bounds(batch);
                if (existing.empty) continue;
                const float existing_w = existing.max_x - existing.min_x;
                const float existing_h = existing.max_y - existing.min_y;
                if (existing_w < 120.0f || existing_h < 80.0f) continue;
                const float ix0 = std::max(existing.min_x, content.min_x);
                const float iy0 = std::max(existing.min_y, content.min_y);
                const float ix1 = std::min(existing.max_x, content.max_x);
                const float iy1 = std::min(existing.max_y, content.max_y);
                const float intersection =
                    std::max(0.0f, ix1 - ix0) * std::max(0.0f, iy1 - iy0);
                const float min_area = std::max(1.0f, std::min(existing_w * existing_h, w * h));
                if (intersection / min_area > 0.45f) {
                    return true;
                }
            }
            return false;
        };

        auto append_detail_frame = [&](const RenderBounds& content) {
            RenderBatch frame;
            frame.sort_key = RenderKey::make(
                static_cast<uint16_t>(detail_layer_index),
                static_cast<uint8_t>(PrimitiveTopology::LineList),
                0,
                static_cast<uint8_t>(EntityType::Polyline),
                0x00ffffu);
            frame.topology = PrimitiveTopology::LineList;
            frame.color = layers[static_cast<size_t>(detail_layer_index)].color;
            frame.line_width = std::max(0.07f, layers[static_cast<size_t>(detail_layer_index)].lineweight);
            frame.space = DrawingSpace::ModelSpace;
            frame.vertex_data = {
                content.min_x, content.min_y, content.max_x, content.min_y,
                content.max_x, content.min_y, content.max_x, content.max_y,
                content.max_x, content.max_y, content.min_x, content.max_y,
                content.min_x, content.max_y, content.min_x, content.min_y,
            };
            batches.push_back(std::move(frame));
            added++;
        };

        for (const auto& marker : text_entries) {
            if (!is_boundary_marker(marker) ||
                !std::isfinite(marker.x) || !std::isfinite(marker.y)) {
                continue;
            }

            std::vector<std::pair<float, float>> candidate_points;
            const float half_x = std::max(1050.0f, marker.height * 17.0f);
            const float min_y = marker.y - std::max(1550.0f, marker.height * 22.0f);
            const float max_y = marker.y - std::max(45.0f, marker.height * 0.6f);

            for (const auto& batch : batches) {
                const int32_t li = batch_layer_index(batch);
                if (li == detail_layer_index) continue;
                std::string layer_lower;
                if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
                    layer_lower = layers[static_cast<size_t>(li)].name;
                    std::transform(layer_lower.begin(), layer_lower.end(), layer_lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                }
                if (layer_lower.find("symbol") != std::string::npos ||
                    layer_lower.find("leader") != std::string::npos ||
                    layer_lower.find("note") != std::string::npos ||
                    layer_lower.find("text") != std::string::npos ||
                    layer_lower.find("dim") != std::string::npos) {
                    continue;
                }

                const auto& vd = batch.vertex_data;
                for (size_t i = 0; i + 1 < vd.size(); i += 2) {
                    const float x = vd[i];
                    const float y = vd[i + 1];
                    if (!is_export_coord(x, y)) continue;
                    if (std::abs(x - marker.x) > half_x) continue;
                    if (y < min_y || y > max_y) continue;
                    candidate_points.push_back({x, y});
                }
            }
            if (candidate_points.size() < 25) continue;

            std::vector<float> sorted_xs;
            sorted_xs.reserve(candidate_points.size());
            for (const auto& point : candidate_points) sorted_xs.push_back(point.first);
            std::sort(sorted_xs.begin(), sorted_xs.end());
            struct MarkerCluster {
                float min_x = 0.0f;
                float max_x = 0.0f;
                size_t count = 0;
            };
            std::vector<MarkerCluster> clusters;
            const float cluster_gap = std::max(180.0f, marker.height * 3.2f);
            MarkerCluster current{sorted_xs.front(), sorted_xs.front(), 0};
            for (float x : sorted_xs) {
                if (x - current.max_x > cluster_gap && current.count > 0) {
                    clusters.push_back(current);
                    current = MarkerCluster{x, x, 0};
                }
                current.max_x = x;
                current.count++;
            }
            if (current.count > 0) clusters.push_back(current);

            bool has_cluster = false;
            MarkerCluster selected_cluster;
            float best_score = 0.0f;
            for (const MarkerCluster& cluster : clusters) {
                if (cluster.count < 20) continue;
                const float center_x = (cluster.min_x + cluster.max_x) * 0.5f;
                const float score = std::abs(center_x - marker.x) -
                                    static_cast<float>(std::min<size_t>(cluster.count, 420)) * 0.25f;
                if (!has_cluster || score < best_score) {
                    has_cluster = true;
                    selected_cluster = cluster;
                    best_score = score;
                }
            }
            if (!has_cluster) continue;

            std::vector<std::pair<float, float>> selected_points;
            for (const auto& point : candidate_points) {
                if (point.first < selected_cluster.min_x ||
                    point.first > selected_cluster.max_x) {
                    continue;
                }
                selected_points.push_back(point);
            }
            if (selected_points.size() < 25) continue;

            struct MarkerYCluster {
                float min_y = 0.0f;
                float max_y = 0.0f;
                size_t count = 0;
            };
            std::vector<float> sorted_ys_for_clusters;
            sorted_ys_for_clusters.reserve(selected_points.size());
            for (const auto& point : selected_points) {
                sorted_ys_for_clusters.push_back(point.second);
            }
            std::sort(sorted_ys_for_clusters.begin(), sorted_ys_for_clusters.end());

            std::vector<MarkerYCluster> y_clusters;
            const float y_cluster_gap = std::max(180.0f, marker.height * 3.0f);
            MarkerYCluster current_y{
                sorted_ys_for_clusters.front(),
                sorted_ys_for_clusters.front(),
                0,
            };
            for (float y : sorted_ys_for_clusters) {
                if (y - current_y.max_y > y_cluster_gap && current_y.count > 0) {
                    y_clusters.push_back(current_y);
                    current_y = MarkerYCluster{y, y, 0};
                }
                current_y.max_y = y;
                current_y.count++;
            }
            if (current_y.count > 0) y_clusters.push_back(current_y);

            bool has_y_cluster = false;
            MarkerYCluster selected_y_cluster;
            float best_y_score = 0.0f;
            for (const MarkerYCluster& cluster : y_clusters) {
                if (cluster.count < 18) continue;
                const float vertical_gap = std::max(0.0f, marker.y - cluster.max_y);
                const float cluster_h = cluster.max_y - cluster.min_y;
                const float score =
                    vertical_gap +
                    cluster_h * 0.18f -
                    static_cast<float>(std::min<size_t>(cluster.count, 260)) * 0.45f;
                if (!has_y_cluster || score < best_y_score) {
                    has_y_cluster = true;
                    selected_y_cluster = cluster;
                    best_y_score = score;
                }
            }
            if (!has_y_cluster) continue;

            std::vector<float> xs;
            std::vector<float> ys;
            for (const auto& point : selected_points) {
                if (point.second < selected_y_cluster.min_y ||
                    point.second > selected_y_cluster.max_y) {
                    continue;
                }
                xs.push_back(point.first);
                ys.push_back(point.second);
            }
            if (xs.size() < 25 || ys.size() < 25) continue;
            std::sort(xs.begin(), xs.end());
            std::sort(ys.begin(), ys.end());
            auto percentile = [](const std::vector<float>& values, float p) {
                if (values.empty()) return 0.0f;
                const float clamped = std::clamp(p, 0.0f, 1.0f);
                const size_t idx = static_cast<size_t>(
                    std::floor(static_cast<float>(values.size() - 1) * clamped));
                return values[idx];
            };

            const float local_frame_height = std::max(760.0f, marker.height * 12.0f);
            const float selected_h = percentile(ys, 0.99f) - percentile(ys, 0.01f);
            if (std::isfinite(selected_h) && selected_h > local_frame_height) {
                const float upper_cut = percentile(ys, 0.99f) - local_frame_height;
                std::vector<float> local_xs;
                std::vector<float> local_ys;
                for (const auto& point : selected_points) {
                    if (point.first < selected_cluster.min_x ||
                        point.first > selected_cluster.max_x ||
                        point.second < upper_cut ||
                        point.second < selected_y_cluster.min_y ||
                        point.second > selected_y_cluster.max_y) {
                        continue;
                    }
                    local_xs.push_back(point.first);
                    local_ys.push_back(point.second);
                }
                if (local_xs.size() >= 20 && local_ys.size() >= 20) {
                    xs = std::move(local_xs);
                    ys = std::move(local_ys);
                    std::sort(xs.begin(), xs.end());
                    std::sort(ys.begin(), ys.end());
                }
            }

            const float local_frame_width = std::max(1250.0f, marker.height * 18.0f);
            const float selected_w = percentile(xs, 0.99f) - percentile(xs, 0.01f);
            if (std::isfinite(selected_w) && selected_w > local_frame_width) {
                const float left_cut = marker.x - local_frame_width * 0.5f;
                const float right_cut = marker.x + local_frame_width * 0.5f;
                const float y0 = percentile(ys, 0.005f);
                const float y1 = percentile(ys, 0.995f);
                std::vector<float> local_xs;
                std::vector<float> local_ys;
                for (const auto& point : selected_points) {
                    if (point.first < left_cut || point.first > right_cut ||
                        point.second < y0 || point.second > y1) {
                        continue;
                    }
                    local_xs.push_back(point.first);
                    local_ys.push_back(point.second);
                }
                if (local_xs.size() >= 20 && local_ys.size() >= 20) {
                    xs = std::move(local_xs);
                    ys = std::move(local_ys);
                    std::sort(xs.begin(), xs.end());
                    std::sort(ys.begin(), ys.end());
                }
            }

            RenderBounds content;
            content.expand(percentile(xs, 0.01f), percentile(ys, 0.01f));
            content.expand(percentile(xs, 0.99f), percentile(ys, 0.99f));
            const float w = content.max_x - content.min_x;
            const float h = content.max_y - content.min_y;
            if (!std::isfinite(w) || !std::isfinite(h) || w < 220.0f || h < 160.0f) {
                continue;
            }
            const float pad = std::clamp(std::max(w, h) * 0.045f, 35.0f, 115.0f);
            content.min_x -= pad;
            content.max_x += pad;
            content.min_y -= pad;
            content.max_y += pad;

            if (overlaps_existing_detail_frame(content)) continue;
            append_detail_frame(content);
        }

        if (added > 0) {
            scene.add_diagnostic({
                "mechanical_detail_view_frame_proxy",
                "Render gap",
                "Detail view crop frame proxies were inferred from DWG Mechanical detail titles and nearby visible geometry while native detail-view custom object semantics remain incomplete.",
                static_cast<int32_t>(std::min<size_t>(added, 2147483647u)),
            });
        }
        return added;
    };
    const size_t inferred_detail_frames = infer_mechanical_detail_view_frames();
    if (inferred_detail_frames > 0) {
        printf("Inferred %zu Mechanical detail view frame proxies\n", inferred_detail_frames);
    }
    printf("Generated %zu batches\n", batches.size());

    JsonWriter out(output_path);
    if (!out.is_open()) {
        fprintf(stderr, "Cannot open output: %s\n", output_path);
        return 1;
    }
    fprintf(stderr, "[output] %s (%s)\n", output_path, out.is_gzip() ? "gzip" : "raw");

    auto& meta = scene.drawing_info();
    auto raw_b = scene.total_bounds();
    const bool raw_bounds_nonfinite = !raw_b.is_empty() && !bounds3d_has_finite_xy(raw_b);
    const char* debug_env_for_bounds = std::getenv("FT_DWG_DEBUG");
    if (is_dwg && debug_env_for_bounds && debug_env_for_bounds[0] != '\0' &&
        std::strcmp(debug_env_for_bounds, "0") != 0 && !raw_b.is_empty()) {
        fprintf(stderr,
                "[DWG] scene_bounds_xyz min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)\n",
                raw_b.min.x, raw_b.min.y, raw_b.min.z,
                raw_b.max.x, raw_b.max.y, raw_b.max.z);
    }
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
    const bool has_layouts = !scene.layouts().empty();
    const bool has_viewports = !scene.viewports().empty();
    const bool has_paper_viewports = std::any_of(
        scene.viewports().begin(), scene.viewports().end(),
        [](const Viewport& vp) { return vp.is_paper_space; });
    const bool has_complete_paper_viewports = std::any_of(
        scene.viewports().begin(), scene.viewports().end(),
        [](const Viewport& vp) {
            return vp.is_paper_space &&
                   std::isfinite(vp.paper_width) &&
                   std::isfinite(vp.paper_height) &&
                   std::isfinite(vp.view_height) &&
                   vp.paper_width > 0.0f &&
                   vp.paper_height > 0.0f &&
                   vp.view_height > 0.0f;
        });
    RenderBounds presentation_bounds = output_bounds;
    Bounds3d scene_presentation = scene.presentation_bounds();
    const Layout* active_layout = has_layouts ? scene.active_layout() : nullptr;
    if (((active_layout != nullptr) || !is_dwg) && !scene_presentation.is_empty()) {
        presentation_bounds = RenderBounds{};
        presentation_bounds.expand(scene_presentation.min.x, scene_presentation.min.y);
        presentation_bounds.expand(scene_presentation.max.x, scene_presentation.max.y);
    }
    int32_t active_layout_index = -1;
    if (active_layout != nullptr) {
        const auto& layouts = scene.layouts();
        for (size_t i = 0; i < layouts.size(); ++i) {
            if (&layouts[i] == active_layout) {
                active_layout_index = static_cast<int32_t>(i);
                break;
            }
        }
    }
    size_t active_layout_visible_entities = 0;
    size_t active_layout_visible_geometry = 0;
    size_t active_layout_visible_text = 0;
    size_t active_layout_owned_entities = 0;
    size_t active_layout_owned_block_entities = 0;
    size_t active_layout_viewport_entities = 0;
    size_t active_layout_layer_hidden_geometry = 0;
    size_t active_layout_layer_hidden_text = 0;
    bool rejected_metadata_only_layout = false;
    if (is_dwg && active_layout_index >= 0) {
        const RenderBounds layout_bounds = presentation_bounds;
        for (int32_t i = 0; i < static_cast<int32_t>(entities.size()); ++i) {
            const auto& entity = entities[static_cast<size_t>(i)];
            const bool belongs_to_layout =
                entity.header.layout_index == active_layout_index ||
                entity.header.space == DrawingSpace::PaperSpace;
            if (!belongs_to_layout) continue;
            ++active_layout_owned_entities;
            const bool is_block_definition_entity = block_entity_indices.count(i) != 0;
            if (is_block_definition_entity) {
                ++active_layout_owned_block_entities;
            }
            if (entity.type() == EntityType::Viewport) {
                ++active_layout_viewport_entities;
            }
            if (entity.header.layer_index >= 0 &&
                static_cast<size_t>(entity.header.layer_index) < layers.size()) {
                const auto& layer = layers[static_cast<size_t>(entity.header.layer_index)];
                if ((layer.is_frozen || layer.is_off) &&
                    render_bounds_intersects(bounds3d_to_render_bounds(entity.bounds()), layout_bounds)) {
                    ++active_layout_layer_hidden_geometry;
                }
            }
            if (is_block_definition_entity) continue;
            if (render_bounds_intersects(bounds3d_to_render_bounds(entity.bounds()), layout_bounds)) {
                ++active_layout_visible_entities;
                ++active_layout_visible_geometry;
            }
        }
        for (const auto& text : text_entries) {
            const bool belongs_to_layout =
                text.layout_index == active_layout_index ||
                text.space == DrawingSpace::PaperSpace;
            if (!belongs_to_layout) continue;
            if (text.layer_index >= 0 && static_cast<size_t>(text.layer_index) < layers.size()) {
                const auto& layer = layers[static_cast<size_t>(text.layer_index)];
                if ((layer.is_frozen || layer.is_off) && point_in_bounds(text.x, text.y, layout_bounds)) {
                    ++active_layout_layer_hidden_text;
                }
            }
            if (point_in_bounds(text.x, text.y, layout_bounds)) {
                ++active_layout_visible_entities;
                ++active_layout_visible_text;
            }
        }

        // A decoded LAYOUT object can carry valid paper/plot metadata before
        // the DWG layout viewport and owner graph are complete. Do not use a
        // metadata-only sheet as the active export window, or model-space
        // content is incorrectly clipped to paper millimetres.
        const bool has_valid_paper_bounds = active_layout &&
            !active_layout->paper_bounds.is_empty() &&
            (active_layout->paper_bounds.max.x - active_layout->paper_bounds.min.x) > 1.0f;
        const float relative_threshold = std::max(5.0f,
            static_cast<float>(scene.entities().size()) * 0.001f);
        if (active_layout_visible_entities < static_cast<size_t>(relative_threshold) &&
            active_layout_viewport_entities == 0 &&
            !has_valid_paper_bounds) {
            rejected_metadata_only_layout = true;
            active_layout = nullptr;
            active_layout_index = -1;
            presentation_bounds = output_bounds;
        }
    }
    int32_t active_model_viewport_index = -1;
    float active_model_viewport_coverage = 0.0f;
    bool rejected_partial_model_viewport = false;
    if (active_layout_index < 0 && is_dwg) {
        const auto& viewports = scene.viewports();
        int32_t candidate_index = -1;
        RenderBounds candidate_bounds;
        for (size_t i = 0; i < viewports.size(); ++i) {
            const auto& vp = viewports[i];
            if (vp.is_paper_space) continue;
            RenderBounds vb = viewport_model_bounds(vp);
            if (vb.empty) continue;
            if (candidate_index >= 0) {
                candidate_index = -1;
                break;
            }
            candidate_index = static_cast<int32_t>(i);
            candidate_bounds = vb;
        }
        if (candidate_index >= 0) {
            active_model_viewport_coverage = bounds_point_coverage(candidate_bounds, visible_points);
            if (active_model_viewport_coverage >= 0.90f) {
                active_model_viewport_index = candidate_index;
                presentation_bounds = candidate_bounds;
            } else {
                rejected_partial_model_viewport = active_model_viewport_coverage >= 0.05f;
            }
        }
    }
    RenderBounds paper_fallback_plot_bounds;
    RenderBounds paper_fallback_sheet_bounds;
    if (rejected_partial_model_viewport) {
        // `output_bounds` is already the finite, presentation-oriented point
        // window. Re-expanding it with whole batch bounds can pull in a mixed
        // layer's off-sheet fragments and shrink the actual drawing on paper.
        RenderBounds paper_fallback_content_bounds = output_bounds;
        const RenderBounds text_search = expand_bounds(output_bounds, 0.20f);
        for (const auto& te : text_entries) {
            if (point_in_bounds(te.x, te.y, text_search)) {
                paper_fallback_content_bounds.expand(te.x, te.y);
            }
        }
        paper_fallback_plot_bounds = padded_sheet_bounds(paper_fallback_content_bounds);
        paper_fallback_sheet_bounds = outer_paper_bounds(paper_fallback_plot_bounds);
        if (!paper_fallback_sheet_bounds.empty) {
            presentation_bounds = paper_fallback_sheet_bounds;
        }
    }
    std::string active_view_id = active_layout_index >= 0
        ? ("layout-" + std::to_string(active_layout_index))
        : (active_model_viewport_index >= 0
            ? ("model-vport-" + std::to_string(active_model_viewport_index))
            : "default");
    RenderBounds default_view_bounds =
        (active_layout_index >= 0 || active_model_viewport_index >= 0)
            ? presentation_bounds
            : (!paper_fallback_sheet_bounds.empty ? paper_fallback_sheet_bounds : output_bounds);
    const RenderBounds abnormal_reference_bounds =
        (active_layout_index >= 0 || active_model_viewport_index >= 0)
            ? output_bounds : default_view_bounds;
    const size_t abnormal_segment_count = is_dwg
        ? count_abnormal_segments(batches, abnormal_reference_bounds)
        : 0;
    size_t filtered_abnormal_segment_count = 0;
    if (abnormal_segment_count > 0) {
        for (auto& batch : batches) {
            filtered_abnormal_segment_count +=
                filter_abnormal_segments(batch, abnormal_reference_bounds);
        }
    }
    if (abnormal_segment_count > 0) {
        printf("DWG abnormal preview segments: %zu (filtered %zu)\n",
               abnormal_segment_count, filtered_abnormal_segment_count);
    }

    // Layout coordinate consistency check: if the active view is a layout but
    // all exported batches are in model-space coordinates (no viewport transform
    // applied), the paper-space bounds are meaningless for the actual geometry.
    // Fall back to model-space default view to avoid a blank display.
    // Exception: keep the layout if it has valid paper bounds so the user still
    // sees the paper canvas and viewport frames.
    if (active_layout_index >= 0) {
        bool has_paper_space_batches = false;
        for (const auto& b : batches) {
            if (!b.vertex_data.empty() && b.space == DrawingSpace::PaperSpace) {
                has_paper_space_batches = true;
                break;
            }
        }
        const bool keep_layout_for_paper_canvas = active_layout &&
            !active_layout->paper_bounds.is_empty() &&
            (active_layout->paper_bounds.max.x - active_layout->paper_bounds.min.x) > 1.0f;
        if (!has_paper_space_batches && !keep_layout_for_paper_canvas) {
            active_layout = nullptr;
            active_layout_index = -1;
            presentation_bounds = output_bounds;
            active_view_id = active_model_viewport_index >= 0
                ? ("model-vport-" + std::to_string(active_model_viewport_index))
                : "default";
            default_view_bounds = (!paper_fallback_sheet_bounds.empty
                ? paper_fallback_sheet_bounds : output_bounds);
        }
    }

    out << "{\n";
    out << "  \"filename\": \"" << escape_json(meta.filename) << "\",\n";
    out << "  \"acadVersion\": \"" << escape_json(meta.acad_version) << "\",\n";
    out << "  \"entityCount\": " << entities.size() << ",\n";
    // Entity type counts for comparison testing
    {
        std::unordered_map<std::string, size_t> tc;
        for (const auto& e : entities) {
            switch (e.type()) {
                case EntityType::Line: tc["Line"]++; break;
                case EntityType::Circle: tc["Circle"]++; break;
                case EntityType::Arc: tc["Arc"]++; break;
                case EntityType::Polyline: tc["Polyline"]++; break;
                case EntityType::LwPolyline: tc["LwPolyline"]++; break;
                case EntityType::Spline: tc["Spline"]++; break;
                case EntityType::Text: tc["Text"]++; break;
                case EntityType::MText: tc["MText"]++; break;
                case EntityType::Dimension: tc["Dimension"]++; break;
                case EntityType::Hatch: tc["Hatch"]++; break;
                case EntityType::Insert: tc["Insert"]++; break;
                case EntityType::Point: tc["Point"]++; break;
                case EntityType::Ellipse: tc["Ellipse"]++; break;
                case EntityType::Ray: tc["Ray"]++; break;
                case EntityType::XLine: tc["XLine"]++; break;
                case EntityType::Viewport: tc["Viewport"]++; break;
                case EntityType::Solid: tc["Solid"]++; break;
                case EntityType::Leader: tc["Leader"]++; break;
                case EntityType::Tolerance: tc["Tolerance"]++; break;
                case EntityType::MLine: tc["MLine"]++; break;
                case EntityType::Multileader: tc["Multileader"]++; break;
            }
        }
        out << "  \"entityTypeCounts\": {";
        bool first_ec = true;
        for (const auto& [name, count] : tc) {
            if (!first_ec) out << ",";
            out << "\"" << name << "\":" << count;
            first_ec = false;
        }
        out << "},\n";
    }
    out << "  \"bounds\": {\n";
    if (!default_view_bounds.empty) {
        out << "    \"minX\": " << default_view_bounds.min_x << ",\n";
        out << "    \"minY\": " << default_view_bounds.min_y << ",\n";
        out << "    \"maxX\": " << default_view_bounds.max_x << ",\n";
        out << "    \"maxY\": " << default_view_bounds.max_y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";
    out << "  \"presentationBounds\": {\n";
    if (!presentation_bounds.empty) {
        out << "    \"minX\": " << presentation_bounds.min_x << ",\n";
        out << "    \"minY\": " << presentation_bounds.min_y << ",\n";
        out << "    \"maxX\": " << presentation_bounds.max_x << ",\n";
        out << "    \"maxY\": " << presentation_bounds.max_y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";
    out << "  \"rawBounds\": {\n";
    if (bounds3d_has_finite_xy(raw_b)) {
        out << "    \"minX\": " << raw_b.min.x << ",\n";
        out << "    \"minY\": " << raw_b.min.y << ",\n";
        out << "    \"maxX\": " << raw_b.max.x << ",\n";
        out << "    \"maxY\": " << raw_b.max.y << ",\n";
        out << "    \"isEmpty\": false\n";
    } else {
        out << "    \"isEmpty\": true\n";
    }
    out << "  },\n";
    out << "  \"drawingInfo\": {";
    out << "\"acadVersion\": \"" << escape_json(meta.acad_version) << "\"";
    out << ", \"headerVarsBytes\": " << meta.dwg_header_vars_bytes;
    out << ", \"extents\": ";
    write_bounds3d_xy(out, meta.extents);
    out << ", \"insertionBase\": ";
    write_vec3_xy(out, meta.insertion_base);
    out << ", \"textSize\": " << meta.text_size;
    out << "},\n";
    out << "  \"activeViewId\": \"" << active_view_id << "\",\n";
    out << "  \"views\": [\n";
    bool wrote_view = false;
    size_t complete_layout_viewport_views = 0;
    size_t low_coverage_layout_viewport_views = 0;
    struct ViewportEntityInfo {
        int32_t layout_index = -1;
        DrawingSpace space = DrawingSpace::Unknown;
        bool has_entity = false;
    };
    std::vector<ViewportEntityInfo> viewport_entity_info(scene.viewports().size());
    for (const auto& entity : entities) {
        if (entity.type() != EntityType::Viewport) continue;
        const int32_t viewport_index = entity.header.viewport_index;
        if (viewport_index < 0 ||
            static_cast<size_t>(viewport_index) >= viewport_entity_info.size()) {
            continue;
        }
        auto& info = viewport_entity_info[static_cast<size_t>(viewport_index)];
        info.layout_index = entity.header.layout_index;
        info.space = entity.header.space;
        info.has_entity = true;
    }
    if (has_layouts) {
        const auto& layouts = scene.layouts();
        for (size_t i = 0; i < layouts.size(); ++i) {
            const auto& layout = layouts[i];
            if (layout.is_model_layout) continue;
            Bounds3d layout_presentation = !layout.plot_window.is_empty() ? layout.plot_window :
                !layout.border_bounds.is_empty() ? layout.border_bounds :
                !layout.paper_bounds.is_empty() ? layout.paper_bounds : Bounds3d::empty();
            if (layout_presentation.is_empty()) continue;
            Bounds3d clip = !layout.plot_window.is_empty() ? layout.plot_window : layout_presentation;
            if (wrote_view) out << ",\n";
            out << "    {\"id\": \"layout-" << i << "\", "
                << "\"name\": \"" << escape_json(layout.name.empty() ? "Layout" : layout.name) << "\", "
                << "\"type\": \"layout\", "
                << "\"source\": \"layout\", "
                << "\"layoutIndex\": " << i << ", "
                << "\"bounds\": ";
            write_bounds3d_xy(out, layout_presentation);
            out << ", \"presentationBounds\": ";
            write_bounds3d_xy(out, layout_presentation);
            out << ", \"paperBounds\": ";
            write_bounds3d_xy(out, layout.paper_bounds);
            out << ", \"plotWindow\": ";
            write_bounds3d_xy(out, layout.plot_window);
            out << ", \"limits\": ";
            write_bounds3d_xy(out, layout.limits);
            out << ", \"extents\": ";
            write_bounds3d_xy(out, layout.extents);
            out << ", \"insertionBase\": ";
            write_vec3_xy(out, layout.insertion_base);
            out << ", \"plotScale\": " << layout.plot_scale
                << ", \"plotRotation\": " << layout.plot_rotation
                << ", \"paperUnits\": " << layout.paper_units;
            out << ", \"clipBounds\": ";
            write_bounds3d_xy(out, clip);
            out << "}";
            out << "\n";
            wrote_view = true;
        }
        if (active_layout_index < 0) {
            if (wrote_view) out << ",\n";
            const RenderBounds& view_bounds = !paper_fallback_sheet_bounds.empty
                ? paper_fallback_sheet_bounds
                : output_bounds;
            const RenderBounds& plot_bounds = !paper_fallback_plot_bounds.empty
                ? paper_fallback_plot_bounds
                : output_bounds;
            out << "    {\"id\": \"default\", "
                << "\"name\": \"Default\", "
                << "\"type\": \"model\", "
                << "\"source\": \""
                << (!paper_fallback_sheet_bounds.empty ? "paperSpaceFallback" : "finiteGeometryFallback")
                << "\", "
                << "\"bounds\": ";
            write_render_bounds(out, view_bounds);
            out << ", \"presentationBounds\": ";
            write_render_bounds(out, view_bounds);
            if (!paper_fallback_sheet_bounds.empty) {
                out << ", \"paperMode\": true, \"paperBounds\": ";
                write_render_bounds(out, paper_fallback_sheet_bounds);
                out << ", \"plotWindow\": ";
                write_render_bounds(out, plot_bounds);
                out << ", \"clipBounds\": ";
                write_render_bounds(out, paper_fallback_sheet_bounds);
            }
            out << "}\n";
            wrote_view = true;
        }
    } else {
        out << "    {\"id\": \"default\", "
            << "\"name\": \"Default\", "
            << "\"type\": \"model\", "
            << "\"source\": \"finiteGeometryFallback\", "
            << "\"bounds\": ";
        write_render_bounds(out, output_bounds);
        out << ", \"presentationBounds\": ";
        write_render_bounds(out, output_bounds);
        out << "}\n";
        wrote_view = true;
    }
    const auto& viewports = scene.viewports();
    for (size_t i = 0; i < viewports.size(); ++i) {
        const auto& vp = viewports[i];
        if (!vp.is_paper_space) continue;
        RenderBounds paper_vb = layout_viewport_paper_bounds(vp);
        RenderBounds model_vb = layout_viewport_model_bounds(vp);
        if (paper_vb.empty || model_vb.empty) continue;
        const float model_coverage = bounds_point_coverage(model_vb, visible_points);
        RenderBounds target_model_vb =
            layout_viewport_model_bounds_with_center(vp, vp.model_view_target);
        Vec3 target_plus_center{
            vp.model_view_target.x + vp.model_view_center.x,
            vp.model_view_target.y + vp.model_view_center.y,
            vp.model_view_target.z + vp.model_view_center.z
        };
        RenderBounds target_plus_center_vb =
            layout_viewport_model_bounds_with_center(vp, target_plus_center);
        const float target_model_coverage =
            bounds_point_coverage(target_model_vb, visible_points);
        const float target_plus_center_coverage =
            bounds_point_coverage(target_plus_center_vb, visible_points);
        ++complete_layout_viewport_views;
        if (model_coverage < 0.05f) {
            ++low_coverage_layout_viewport_views;
        }
        if (wrote_view) out << ",\n";
        out << "    {\"id\": \"layout-vport-" << i << "\", "
            << "\"name\": \"" << escape_json(vp.name.empty() ? "Layout Viewport" : vp.name) << "\", "
            << "\"type\": \"layoutViewport\", "
            << "\"source\": \"layoutViewport\", "
            << "\"viewportId\": " << i << ", "
            << "\"layoutIndex\": " << (i < viewport_entity_info.size()
                    ? viewport_entity_info[i].layout_index : -1) << ", "
            << "\"entitySpace\": \"" << (i < viewport_entity_info.size()
                    ? drawing_space_name(viewport_entity_info[i].space) : "unknown") << "\", "
            << "\"hasViewportEntity\": "
            << ((i < viewport_entity_info.size() && viewport_entity_info[i].has_entity)
                    ? "true" : "false") << ", "
            << "\"bounds\": ";
        write_render_bounds(out, paper_vb);
        out << ", \"presentationBounds\": ";
        write_render_bounds(out, paper_vb);
        out << ", \"modelBounds\": ";
        write_render_bounds(out, model_vb);
        out << ", \"viewHeight\": " << vp.view_height
            << ", \"customScale\": " << vp.custom_scale
            << ", \"twistAngle\": " << vp.twist_angle
            << ", \"modelCoverage\": " << model_coverage
            << ", \"paperCenter\": ";
        write_vec3_xy(out, vp.paper_center);
        out << ", \"modelViewCenter\": ";
        write_vec3_xy(out, vp.model_view_center);
        out << ", \"modelViewTarget\": ";
        write_vec3_xy(out, vp.model_view_target);
        out << ", \"center\": ";
        write_vec3_xy(out, vp.center);
        out << ", \"targetModelBounds\": ";
        write_render_bounds(out, target_model_vb);
        out << ", \"targetModelCoverage\": " << target_model_coverage
            << ", \"targetPlusCenterModelBounds\": ";
        write_render_bounds(out, target_plus_center_vb);
        out << ", \"targetPlusCenterModelCoverage\": " << target_plus_center_coverage
            << ", \"frozenLayerCount\": " << vp.frozen_layer_indices.size()
            << ", \"frozenLayers\": [";
        const size_t frozen_limit = std::min<size_t>(vp.frozen_layer_indices.size(), 12);
        for (size_t fi = 0; fi < frozen_limit; ++fi) {
            if (fi > 0) out << ", ";
            const int32_t layer_index = vp.frozen_layer_indices[fi];
            std::string layer_name;
            if (layer_index >= 0 && static_cast<size_t>(layer_index) < scene.layers().size()) {
                layer_name = scene.layers()[static_cast<size_t>(layer_index)].name;
            }
            out << "\"" << escape_json(layer_name) << "\"";
        }
        out << "]";
        out
            << "}\n";
        wrote_view = true;
    }
    for (size_t i = 0; i < viewports.size(); ++i) {
        const auto& vp = viewports[i];
        if (vp.is_paper_space) continue;
        RenderBounds vb = viewport_model_bounds(vp);
        if (vb.empty) continue;
        if (wrote_view) out << ",\n";
        out << "    {\"id\": \"model-vport-" << i << "\", "
            << "\"name\": \"" << escape_json(vp.name.empty() ? "Model View" : vp.name) << "\", "
            << "\"type\": \"model\", "
            << "\"source\": \"vport\", "
            << "\"viewportId\": " << i << ", "
            << "\"bounds\": ";
        write_render_bounds(out, vb);
        out << ", \"presentationBounds\": ";
        write_render_bounds(out, vb);
        out << ", \"modelViewCenter\": ";
        write_vec3_xy(out, vp.model_view_center);
        out << ", \"modelViewTarget\": ";
        write_vec3_xy(out, vp.model_view_target);
        out << ", \"center\": ";
        write_vec3_xy(out, vp.center);
        out << "}\n";
        wrote_view = true;
    }
    out << "  ],\n";
    out << "  \"diagnostics\": {\n";
    out << "    \"layouts\": " << scene.layouts().size() << ",\n";
    out << "    \"viewports\": " << scene.viewports().size() << ",\n";
    out << "    \"gaps\": [";
    bool first_gap = true;
    auto gap = [&](const char* code, const char* category, const char* message) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"" << code << "\", \"category\": \"" << category
            << "\", \"message\": \"" << message << "\"}";
    };
    auto gap_with_count = [&](const SceneDiagnostic& diagnostic) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"" << escape_json(diagnostic.code)
            << "\", \"category\": \"" << escape_json(diagnostic.category)
            << "\", \"message\": \"" << escape_json(diagnostic.message)
            << "\", \"count\": " << diagnostic.count;
        if (!diagnostic.version_family.empty()) {
            out << ", \"version_family\": \"" << escape_json(diagnostic.version_family) << "\"";
        }
        if (!diagnostic.object_family.empty()) {
            out << ", \"object_family\": \"" << escape_json(diagnostic.object_family) << "\"";
        }
        if (diagnostic.object_type != 0) {
            out << ", \"object_type\": " << diagnostic.object_type;
        }
        if (!diagnostic.class_name.empty()) {
            out << ", \"class_name\": \"" << escape_json(diagnostic.class_name) << "\"";
        }
        if (diagnostic.sample_handle != 0) {
            out << ", \"sample_handle\": " << diagnostic.sample_handle;
        }
        out << "}";
    };
    for (const auto& diagnostic : scene.diagnostics()) {
        gap_with_count(diagnostic);
    }
    if (is_dwg && has_layouts && active_layout_index < 0) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"layout_entity_ownership_unresolved\", "
            << "\"category\": \"View gap\", "
            << "\"message\": \"DWG layout objects were decoded, but paper-space entity ownership/viewport content was not complete enough to select a layout as the default view.";
        if (rejected_metadata_only_layout) {
            out << " The best layout candidate had only "
                << active_layout_visible_entities
                << " paper/layout-owned visible entities (geometry="
                << active_layout_visible_geometry
                << ", text=" << active_layout_visible_text
                << ", owned=" << active_layout_owned_entities
                << ", blockDefinitionOwned=" << active_layout_owned_block_entities
                << ", viewportEntities=" << active_layout_viewport_entities
                << ", layerHiddenGeometry=" << active_layout_layer_hidden_geometry
                << ", layerHiddenText=" << active_layout_layer_hidden_text
                << "), so it remains a view candidate rather than the active export window.";
        }
        out << "\", "
            << "\"count\": " << scene.layouts().size()
            << ", \"object_family\": \"LAYOUT/VIEWPORT\"}";
    }
    if (is_dwg && has_complete_paper_viewports && active_layout_index < 0) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"layout_viewport_projection_deferred\", "
            << "\"category\": \"View gap\", "
            << "\"message\": \"Full paper-space viewport metadata was decoded, but model-space content is not yet projected through layout viewports in the export/render path.\", "
            << "\"count\": " << scene.viewports().size()
            << ", \"object_family\": \"VIEWPORT\"}";
    }
    if (is_dwg && low_coverage_layout_viewport_views > 0) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"layout_viewport_model_coverage_low\", "
            << "\"category\": \"View gap\", "
            << "\"message\": \"Decoded layout viewport model windows cover little or none of the finite model geometry, so viewport projection remains diagnostic-only until field alignment and owner binding are verified.\", "
            << "\"count\": " << low_coverage_layout_viewport_views
            << ", \"object_family\": \"VIEWPORT\", "
            << "\"object_type\": " << complete_layout_viewport_views << "}";
    }
    if (is_dwg && complete_layout_viewport_views > 0) {
        size_t unowned_layout_viewports = 0;
        for (size_t i = 0; i < viewports.size(); ++i) {
            const auto& vp = viewports[i];
            if (!vp.is_paper_space) continue;
            if (layout_viewport_paper_bounds(vp).empty ||
                layout_viewport_model_bounds(vp).empty) {
                continue;
            }
            if (i >= viewport_entity_info.size() ||
                viewport_entity_info[i].layout_index < 0 ||
                viewport_entity_info[i].space != DrawingSpace::PaperSpace) {
                ++unowned_layout_viewports;
            }
        }
        if (unowned_layout_viewports > 0) {
            if (!first_gap) out << ", ";
            first_gap = false;
            out << "{\"code\": \"layout_viewport_owner_unresolved\", "
                << "\"category\": \"Handle resolution gap\", "
                << "\"message\": \"One or more decoded layout viewports are not yet bound to a paper-space layout owner, so viewport projection remains diagnostic-only.\", "
                << "\"count\": " << unowned_layout_viewports
                << ", \"object_family\": \"VIEWPORT\"}";
        }
    }
    if (is_dwg && !default_view_bounds.empty && !layers.empty()) {
        size_t hidden_text_in_view = 0;
        size_t hidden_geometry_in_view = 0;
        size_t hidden_layer_count = 0;
        size_t hidden_geometry_layer_count = 0;
        std::vector<bool> layer_seen(layers.size(), false);
        std::vector<bool> geometry_layer_seen(layers.size(), false);
        std::vector<size_t> hidden_text_by_layer(layers.size(), 0);
        std::vector<size_t> hidden_geometry_by_layer(layers.size(), 0);
        for (const auto& te : text_entries) {
            if (!point_in_bounds(te.x, te.y, default_view_bounds)) continue;
            if (te.layer_index < 0 || static_cast<size_t>(te.layer_index) >= layers.size()) continue;
            const auto& layer = layers[static_cast<size_t>(te.layer_index)];
            if (!layer.is_frozen && !layer.is_off) continue;
            ++hidden_text_in_view;
            ++hidden_text_by_layer[static_cast<size_t>(te.layer_index)];
            if (!layer_seen[static_cast<size_t>(te.layer_index)]) {
                layer_seen[static_cast<size_t>(te.layer_index)] = true;
                ++hidden_layer_count;
            }
        }
        for (int32_t i = 0; i < static_cast<int32_t>(entities.size()); ++i) {
            if (block_entity_indices.count(i)) continue;
            const auto& entity = entities[static_cast<size_t>(i)];
            if (entity.type() == EntityType::Text || entity.type() == EntityType::MText ||
                entity.type() == EntityType::Dimension) {
                continue;
            }
            if (entity.header.layer_index < 0 ||
                static_cast<size_t>(entity.header.layer_index) >= layers.size()) {
                continue;
            }
            const auto& layer = layers[static_cast<size_t>(entity.header.layer_index)];
            if (!layer.is_frozen && !layer.is_off) continue;
            RenderBounds entity_bounds = bounds3d_to_render_bounds(entity.bounds());
            if (!render_bounds_intersects(entity_bounds, default_view_bounds)) continue;
            ++hidden_geometry_in_view;
            ++hidden_geometry_by_layer[static_cast<size_t>(entity.header.layer_index)];
            if (!geometry_layer_seen[static_cast<size_t>(entity.header.layer_index)]) {
                geometry_layer_seen[static_cast<size_t>(entity.header.layer_index)] = true;
                ++hidden_geometry_layer_count;
            }
        }
        const std::string hidden_text_layers =
            summarize_layer_counts(hidden_text_by_layer, layers, 5);
        const std::string hidden_geometry_layers =
            summarize_layer_counts(hidden_geometry_by_layer, layers, 5);
        if (hidden_text_in_view > 0) {
            if (!first_gap) out << ", ";
            first_gap = false;
            out << "{\"code\": \"layer_state_hides_presentation_text\", "
                << "\"category\": \"View gap\", "
                << "\"message\": \"Text exists inside the active presentation bounds on frozen/off layers. This may be intentional layer state, or a missing layout-viewport layer override until per-viewport layer state is decoded.";
            if (!hidden_text_layers.empty()) {
                out << " Top hidden text layers: " << escape_json(hidden_text_layers) << ".";
            }
            out << "\", "
                << "\"count\": " << hidden_text_in_view
                << ", \"object_family\": \"LAYER/VIEWPORT\", "
                << "\"object_type\": " << hidden_layer_count << "}";
        }
        if (hidden_geometry_in_view > 0) {
            if (!first_gap) out << ", ";
            first_gap = false;
            out << "{\"code\": \"layer_state_hides_presentation_geometry\", "
                << "\"category\": \"View gap\", "
                << "\"message\": \"Drawable geometry intersects the active presentation bounds on frozen/off layers. If it should be visible through a layout viewport, per-viewport layer overrides are still missing.";
            if (!hidden_geometry_layers.empty()) {
                out << " Top hidden geometry layers: " << escape_json(hidden_geometry_layers) << ".";
            }
            out << "\", "
                << "\"count\": " << hidden_geometry_in_view
                << ", \"object_family\": \"LAYER/VIEWPORT\", "
                << "\"object_type\": " << hidden_geometry_layer_count << "}";
        }
    }
    if (is_dwg && !has_layouts) {
        gap("missing_layout_objects", "Semantic gap",
            "DWG layout objects are not fully parsed; default view uses finite geometry fallback.");
    }
    if (is_dwg && !has_complete_paper_viewports) {
        gap("missing_layout_viewports", "View gap",
            has_paper_viewports
                ? "Only placeholder paper-space VIEWPORT entities were decoded; full paper-space rectangle, model view, scale, and clip metadata are still missing."
                : "No full paper-space layout viewport metadata is available.");
    }
    if (rejected_partial_model_viewport) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"partial_model_viewport_fallback\", "
            << "\"category\": \"View gap\", "
            << "\"message\": \"A model VPORT did not cover enough finite drawing geometry for the default preview, so a paper-space fallback window was inferred from finite drawing geometry.\", "
            << "\"count\": " << static_cast<int>(active_model_viewport_coverage * 100.0f) << "}";
    }
    if (raw_bounds_nonfinite) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"raw_bounds_nonfinite\", "
            << "\"category\": \"Render gap\", "
            << "\"message\": \"Scene raw bounds contained NaN or infinite coordinates. Exported rawBounds were suppressed while finite presentation bounds were used for preview fitting.\", "
            << "\"count\": 1}";
    }
    if (abnormal_segment_count > 0) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"abnormal_preview_segments\", "
            << "\"category\": \"Render gap\", "
            << "\"message\": \"Line segments outside the active presentation window were detected and filtered from the default export; remaining causes should be traced to source entities.\", "
            << "\"count\": " << abnormal_segment_count << "}";
    }
    gap("plot_style_deferred", "Plot appearance gap",
        "CTB/STB plot style evaluation is not implemented in 0.8.0.");
    gap("wipeout_deferred", "Render gap",
        "Wipeout/mask objects are not yet represented as first-class entities.");
    gap("leader_mleader_deferred", "Parse gap",
        "Leader/MLeader entities are tracked as a remaining annotation parser gap.");
    out << "]\n";
    out << "  },\n";

    // Export layers
    out << "  \"layers\": [\n";
    for (size_t i = 0; i < layers.size(); ++i) {
        auto& l = layers[i];
        out << "    {\"name\": \"" << escape_json(l.name)
            << "\", \"color\": [" << (int)l.color.r << "," << (int)l.color.g << "," << (int)l.color.b
            << "], \"frozen\": " << (l.is_frozen ? "true" : "false")
            << ", \"off\": " << (l.is_off ? "true" : "false")
            << ", \"locked\": " << (l.is_locked ? "true" : "false")
            << ", \"plotEnabled\": " << (l.plot_enabled ? "true" : "false")
            << ", \"lineweight\": " << l.lineweight
            << ", \"linetypeIndex\": " << l.linetype_index
            << ", \"plotStyleIndex\": " << l.plot_style_index << "}";
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
        RenderBounds batch_bounds;
        for (int i = 0; i < vert_count; ++i) {
            double vx = batch.vertex_data[i * 2];
            double vy = batch.vertex_data[i * 2 + 1];
            if (std::isfinite(vx) && std::isfinite(vy) &&
                std::abs(vx) <= 1.0e8 && std::abs(vy) <= 1.0e8) {
                valid_vert_count++;
                batch_bounds.expand(static_cast<float>(vx), static_cast<float>(vy));
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
        out << "\"lineWidth\": " << batch.line_width << ", ";
        out << "\"linePattern\": [";
        for (size_t pi = 0; pi < batch.line_pattern.dash_array.size(); ++pi) {
            if (pi > 0) out << ",";
            float v = batch.line_pattern.dash_array[pi];
            out << (std::isfinite(v) ? v : 0.0f);
        }
        out << "], ";
        out << "\"space\": \"" << space_to_json(batch.space) << "\", ";
        out << "\"layoutId\": " << batch.layout_index << ", ";
        out << "\"viewportId\": " << batch.viewport_index << ", ";
        out << "\"drawOrder\": " << batch.draw_order << ", ";
        out << "\"bounds\": ";
        write_render_bounds(out, batch_bounds);
        out << ", ";
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
            out << fmt_coord(vx) << "," << fmt_coord(vy);
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
            << ", \"rectWidth\": " << te.rect_width
            << ", \"rectHeight\": " << te.rect_height
            << ", \"text\": \"" << escape_json(te.text) << "\""
            << ", \"color\": [" << (int)te.r << "," << (int)te.g << "," << (int)te.b << "]"
            << ", \"layerIndex\": " << te.layer_index
            << ", \"layerName\": \"" << escape_json(te.layer_name) << "\""
            << ", \"space\": \"" << space_to_json(te.space) << "\""
            << ", \"layoutId\": " << te.layout_index
            << ", \"viewportId\": " << te.viewport_index
            << ", \"styleIndex\": " << te.style_index
            << ", \"kind\": \"" << te.kind << "\""
            << ", \"align\": " << te.alignment << "}";
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
