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
    if (!bounds.is_empty()) {
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

    const RenderBounds padded = expand_bounds(presentation, 0.08f);
    const bool intersects_presentation = segment_intersects_bounds(x0, y0, x1, y1, padded);

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(len)) return true;
    const bool inside0 = point_in_bounds(x0, y0, padded);
    const bool inside1 = point_in_bounds(x1, y1, padded);
    if (inside0 && inside1) return false;
    if (!intersects_presentation) {
        return len > diag * 0.03f;
    }

    if (len > diag * 1.25f) return true;
    if (len > diag * 0.45f) {
        const RenderBounds far_pad = expand_bounds(presentation, 0.35f);
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
    for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
        size_t start_vertex = static_cast<size_t>(batch.entity_starts[ei]);
        size_t end_vertex = (ei + 1 < batch.entity_starts.size())
            ? static_cast<size_t>(batch.entity_starts[ei + 1])
            : vd.size() / 2;
        start_vertex = std::min(start_vertex, vd.size() / 2);
        end_vertex = std::min(end_vertex, vd.size() / 2);
        if (end_vertex <= start_vertex) continue;

        RenderBounds subpath_bounds;
        for (size_t vi = start_vertex; vi < end_vertex; ++vi) {
            const size_t off = vi * 2;
            subpath_bounds.expand(vd[off], vd[off + 1]);
        }

        bool abnormal = false;
        if (!render_bounds_intersects(subpath_bounds, far_window) ||
            render_bounds_diag(subpath_bounds) > presentation_diag * 3.0f) {
            abnormal = true;
        }
        for (size_t vi = start_vertex; vi + 1 < end_vertex; ++vi) {
            const size_t off = vi * 2;
            if (is_abnormal_segment(vd[off], vd[off + 1],
                                    vd[off + 2], vd[off + 3],
                                    presentation)) {
                abnormal = true;
                break;
            }
        }
        if (abnormal) {
            removed++;
            continue;
        }

        filtered_starts.push_back(static_cast<uint32_t>(filtered_vertices.size() / 2));
        for (size_t vi = start_vertex; vi < end_vertex; ++vi) {
            const size_t off = vi * 2;
            filtered_vertices.push_back(vd[off]);
            filtered_vertices.push_back(vd[off + 1]);
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
    std::string upper = uppercase_ascii(name);
    return upper == "*MODEL_SPACE" || upper == "*PAPER_SPACE";
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

    RenderBounds broad_bounds;
    const float broad_w = q99x - q01x;
    const float broad_h = q99y - q01y;
    if (std::isfinite(broad_w) && broad_w > 0.0f &&
        std::isfinite(broad_h) && broad_h > 0.0f) {
        broad_bounds.expand(q01x - broad_w * 0.02f, q01y - broad_h * 0.02f);
        broad_bounds.expand(q99x + broad_w * 0.02f, q99y + broad_h * 0.02f);
    }
    if (broad_bounds.empty) return fallback;

    const bool has_split_axis =
        split_axis(q05x, q1x, q3x, q95x) ||
        split_axis(q05y, q1y, q3y, q95y);
    if (has_split_axis && !iqr_bounds.empty) {
        const float iw = std::max(1.0f, iqr_bounds.max_x - iqr_bounds.min_x);
        const float ih = std::max(1.0f, iqr_bounds.max_y - iqr_bounds.min_y);
        const float bw = std::max(1.0f, broad_bounds.max_x - broad_bounds.min_x);
        const float bh = std::max(1.0f, broad_bounds.max_y - broad_bounds.min_y);
        const float broad_aspect = std::max(bw / bh, bh / bw);
        const float area_ratio = (bw * bh) / std::max(1.0f, iw * ih);
        // Multi-view mechanical drawings often have legitimate separated
        // clusters inside one sheet. Keep the broad 1%-99% window when it is
        // still sheet-shaped; use the IQR core only for extreme mixed extents
        // such as model/paper coordinates several orders of magnitude apart.
        if (broad_aspect > 20.0f || area_ratio > 20.0f) {
            return iqr_bounds;
        }
    }

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
    bool has_scaled_insert) {
    if (is_model_or_paper_space_block(block.name)) return true;

    RenderBounds bounds = bounds_for_entity_indices(entities, block.entity_indices);
    if (bounds.empty || block.entity_indices.empty()) return false;

    const float w = bounds.max_x - bounds.min_x;
    const float h = bounds.max_y - bounds.min_y;
    const float major_extent = std::max(w, h);
    const float cx = (bounds.min_x + bounds.max_x) * 0.5f;
    const float cy = (bounds.min_y + bounds.max_y) * 0.5f;
    const float centroid_dist = std::sqrt(cx * cx + cy * cy);
    if (block.entity_indices.size() > 50 && centroid_dist > 5000.0f) {
        return true;
    }
    if (!block.is_anonymous && has_scaled_insert && centroid_dist > 5000.0f) {
        return true;
    }
    // Some DWGs, especially generated mechanical drawing views, store block
    // children in paper/model coordinates while INSERT carries an additional
    // presentation transform. Treat sheet-scale block definitions as already
    // placed so they do not get expanded a second time far outside the page.
    return !block.is_anonymous &&
           has_scaled_insert &&
           major_extent > 1000.0f;
}

static bool should_merge_dwg_block_header_entities(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert) {
    if (block.is_anonymous || !has_scaled_insert ||
        block.header_owned_entity_indices.size() < 2) {
        return false;
    }
    const RenderBounds header_bounds =
        bounds_for_entity_indices(entities, block.header_owned_entity_indices);
    if (header_bounds.empty) {
        return false;
    }
    const float header_w = header_bounds.max_x - header_bounds.min_x;
    const float header_h = header_bounds.max_y - header_bounds.min_y;
    const float header_major = std::max(header_w, header_h);
    if (!std::isfinite(header_major) || header_major <= 0.0f) {
        return false;
    }
    // BLOCK_HEADER owner recovery is only safe when the recovered children
    // look like a compact local block definition. Very large owner buckets are
    // usually table/model-space ownership noise and must not be transformed
    // through every INSERT reference.
    if (header_major > 50000.0f) {
        return false;
    }
    return true;
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
    std::vector<int32_t> local_insert_block_indices;
    bool insert_expansion_active = false;

    if (is_dwg) {
        auto& blocks = scene.blocks();
        const size_t original_block_count = blocks.size();
        const RenderBounds raw_scene_bounds = bounds3d_to_render_bounds(scene.total_bounds());
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
            const bool merge_header_owned =
                should_merge_dwg_block_header_entities(block, entities, scaled_insert);
            block_should_direct[bi] =
                should_render_dwg_block_direct(block, entities, scaled_insert);

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
            } else if (merge_header_owned) {
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
                        batcher.submit_entity(local_insert, scene);
                    }
                }
                continue;
            }
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
                entry.rect_width = (te->rect_width > 0.0f && std::isfinite(te->rect_width))
                    ? te->rect_width
                    : 0.0f;
                entry.rect_height = (te->rect_height > 0.0f && std::isfinite(te->rect_height))
                    ? te->rect_height
                    : 0.0f;
                entry.text = te->text;
                entry.layer_index = entity.header.layer_index;
                entry.space = entity.header.space;
                entry.layout_index = entity.header.layout_index;
                entry.viewport_index = entity.header.viewport_index;
                entry.style_index = te->text_style_index;
                entry.alignment = te->alignment;
                entry.kind = (entity.type() == EntityType::MText) ? "mtext" : "text";

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
                entry.kind = "dimension";
                entry.layer_index = entity.header.layer_index;
                entry.space = entity.header.space;
                entry.layout_index = entity.header.layout_index;
                entry.viewport_index = entity.header.viewport_index;

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

    auto infer_mechanical_detail_view_frames = [&]() -> size_t {
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
        (active_model_viewport_index >= 0) ? output_bounds : default_view_bounds;
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

    out << "{\n";
    out << "  \"filename\": \"" << escape_json(meta.filename) << "\",\n";
    out << "  \"acadVersion\": \"" << escape_json(meta.acad_version) << "\",\n";
    out << "  \"entityCount\": " << entities.size() << ",\n";
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
    out << "  \"activeViewId\": \"" << active_view_id << "\",\n";
    out << "  \"views\": [\n";
    bool wrote_view = false;
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
            << "\", \"count\": " << diagnostic.count << "}";
    };
    for (const auto& diagnostic : scene.diagnostics()) {
        gap_with_count(diagnostic);
    }
    if (is_dwg && !has_layouts) {
        gap("missing_layout_objects", "Semantic gap",
            "DWG layout objects are not fully parsed; default view uses finite geometry fallback.");
    }
    if (is_dwg && !has_paper_viewports) {
        gap("missing_layout_viewports", "View gap",
            "No full paper-space layout viewport metadata is available.");
    }
    if (rejected_partial_model_viewport) {
        if (!first_gap) out << ", ";
        first_gap = false;
        out << "{\"code\": \"partial_model_viewport_fallback\", "
            << "\"category\": \"View gap\", "
            << "\"message\": \"A model VPORT did not cover enough finite drawing geometry for the default preview, so a paper-space fallback window was inferred from finite drawing geometry.\", "
            << "\"count\": " << static_cast<int>(active_model_viewport_coverage * 100.0f) << "}";
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
            out << batch.line_pattern.dash_array[pi];
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
