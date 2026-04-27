#include "cad/renderer/render_batcher.h"
#include "cad/renderer/camera.h"
#include "cad/renderer/lod_selector.h"
#include "cad/scene/entity.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/layer.h"
#include "cad/scene/block.h"
#include "cad/cad_types.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace cad {

namespace {

constexpr float kMaxRenderableCoord = 1.0e8f;

bool is_renderable_coord(float x, float y) {
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) <= kMaxRenderableCoord &&
           std::abs(y) <= kMaxRenderableCoord;
}

bool append_vertex(std::vector<float>& vertex_data, float x, float y) {
    if (!is_renderable_coord(x, y)) return false;
    vertex_data.push_back(x);
    vertex_data.push_back(y);
    return true;
}

bool is_renderable_point(const Vec3& pt) {
    return is_renderable_coord(pt.x, pt.y);
}

float distance_xy(const Vec3& a, const Vec3& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

Vec3 lerp_vec3(const Vec3& a, const Vec3& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

Vec3 catmull_rom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                 float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return {
        0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
        0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3),
        0.0f,
    };
}

bool valid_knot_vector(const std::vector<float>& knots, size_t control_count, int degree) {
    if (degree < 1 || control_count < 2) return false;
    const size_t expected = control_count + static_cast<size_t>(degree) + 1;
    if (knots.size() < expected) return false;
    for (size_t i = 1; i < knots.size(); ++i) {
        if (!std::isfinite(knots[i]) || knots[i] < knots[i - 1]) return false;
    }
    return true;
}

int find_spline_span(const std::vector<float>& knots, int control_count, int degree,
                     float u) {
    const int n = control_count - 1;
    if (u >= knots[static_cast<size_t>(n + 1)]) return n;
    if (u <= knots[static_cast<size_t>(degree)]) return degree;

    int low = degree;
    int high = n + 1;
    int mid = (low + high) / 2;
    while (u < knots[static_cast<size_t>(mid)] ||
           u >= knots[static_cast<size_t>(mid + 1)]) {
        if (u < knots[static_cast<size_t>(mid)]) {
            high = mid;
        } else {
            low = mid;
        }
        mid = (low + high) / 2;
    }
    return mid;
}

Vec3 evaluate_bspline(const SplineEntity& spline, float u) {
    const auto& points = spline.control_points;
    const auto& knots = spline.knots;
    const int degree = std::clamp(spline.degree, 1, 16);
    const int point_count = static_cast<int>(points.size());
    const int span = find_spline_span(knots, point_count, degree, u);

    struct WeightedPoint {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double w = 1.0;
    };

    std::vector<WeightedPoint> d(static_cast<size_t>(degree + 1));
    for (int j = 0; j <= degree; ++j) {
        const int idx = span - degree + j;
        const float weight =
            (spline.weights.size() == points.size() && spline.weights[static_cast<size_t>(idx)] > 0.0f)
                ? spline.weights[static_cast<size_t>(idx)]
                : 1.0f;
        const Vec3& pt = points[static_cast<size_t>(idx)];
        d[static_cast<size_t>(j)] = {
            static_cast<double>(pt.x) * weight,
            static_cast<double>(pt.y) * weight,
            static_cast<double>(pt.z) * weight,
            static_cast<double>(weight),
        };
    }

    for (int r = 1; r <= degree; ++r) {
        for (int j = degree; j >= r; --j) {
            const int left = span - degree + j;
            const int right = span + 1 + j - r;
            const double denom =
                static_cast<double>(knots[static_cast<size_t>(right)] -
                                    knots[static_cast<size_t>(left)]);
            const double alpha = std::abs(denom) > 1.0e-12
                ? (static_cast<double>(u) - knots[static_cast<size_t>(left)]) / denom
                : 0.0;
            auto& cur = d[static_cast<size_t>(j)];
            const auto& prev = d[static_cast<size_t>(j - 1)];
            cur.x = (1.0 - alpha) * prev.x + alpha * cur.x;
            cur.y = (1.0 - alpha) * prev.y + alpha * cur.y;
            cur.z = (1.0 - alpha) * prev.z + alpha * cur.z;
            cur.w = (1.0 - alpha) * prev.w + alpha * cur.w;
        }
    }

    const auto& out = d[static_cast<size_t>(degree)];
    const double inv_w = std::abs(out.w) > 1.0e-12 ? (1.0 / out.w) : 1.0;
    return {
        static_cast<float>(out.x * inv_w),
        static_cast<float>(out.y * inv_w),
        static_cast<float>(out.z * inv_w),
    };
}

std::vector<Vec3> tessellate_spline_points(const SplineEntity& spline, float ppu) {
    std::vector<Vec3> out;

    if (!spline.fit_points.empty()) {
        const auto& pts = spline.fit_points;
        if (pts.size() < 2) return out;

        float seg_chord = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i) {
            seg_chord += distance_xy(pts[i - 1], pts[i]);
        }
        // Adaptive samples per segment based on pixel-space chord length.
        // Target ~2px spacing in screen space.
        float total_px = seg_chord * ppu;
        int samples_per_seg = static_cast<int>(std::ceil(total_px / (pts.size() * 2.0f)));
        samples_per_seg = std::clamp(samples_per_seg, 4, 32);

        out.reserve((pts.size() - 1) * samples_per_seg + 1);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec3& p0 = (i == 0) ? pts[i] : pts[i - 1];
            const Vec3& p1 = pts[i];
            const Vec3& p2 = pts[i + 1];
            const Vec3& p3 = (i + 2 < pts.size()) ? pts[i + 2] : pts[i + 1];
            for (int s = 0; s < samples_per_seg; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(samples_per_seg);
                Vec3 p = catmull_rom(p0, p1, p2, p3, t);
                if (is_renderable_point(p)) out.push_back(p);
            }
        }
        if (is_renderable_point(pts.back())) out.push_back(pts.back());
        if (spline.is_closed && out.size() > 2) out.push_back(out.front());
        return out;
    }

    const auto& pts = spline.control_points;
    if (pts.size() < 2) return out;

    const int degree = std::clamp(spline.degree, 1, 16);
    if (valid_knot_vector(spline.knots, pts.size(), degree)) {
        const int n = static_cast<int>(pts.size()) - 1;
        const float u0 = spline.knots[static_cast<size_t>(degree)];
        const float u1 = spline.knots[static_cast<size_t>(n + 1)];
        if (std::isfinite(u0) && std::isfinite(u1) && u1 > u0) {
            // Chord-height error formula for B-spline sampling:
            //   sagitta ≈ chord²/(8·R) <= tolerance_world
            //   samples >= chord / sqrt(8·R·tol)
            // Where R ≈ min control-polygon edge length (conservative curvature bound).
            float chord = 0.0f;
            float min_edge = 1e30f;
            for (size_t i = 1; i < pts.size(); ++i) {
                float d = distance_xy(pts[i - 1], pts[i]);
                chord += d;
                if (d > 1e-6f) min_edge = std::min(min_edge, d);
            }
            float effective_r = std::min(min_edge, chord * 0.5f);
            float tol_world = 0.5f / std::max(ppu, 1e-6f);
            float adaptive = chord / std::sqrt(std::max(8.0f * effective_r * tol_world, 1e-6f));
            float chord_px = chord * ppu;
            int samples = static_cast<int>(std::ceil(std::max(adaptive, chord_px * 0.5f)));
            samples = std::clamp(samples, static_cast<int>(pts.size()) * 2, 2048);

            out.reserve(static_cast<size_t>(samples + 1));
            for (int i = 0; i <= samples; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(samples);
                const float u = (i == samples) ? u1 : (u0 + (u1 - u0) * t);
                Vec3 p = evaluate_bspline(spline, u);
                if (is_renderable_point(p)) out.push_back(p);
            }
            if (spline.is_closed && out.size() > 2) out.push_back(out.front());
            return out;
        }
    }

    out.reserve(pts.size());
    for (const Vec3& pt : pts) {
        if (is_renderable_point(pt)) out.push_back(pt);
    }
    if (spline.is_closed && out.size() > 2) out.push_back(out.front());
    return out;
}

bool same_pattern(const LinePattern& a, const LinePattern& b) {
    return a.dash_array == b.dash_array;
}

void split_line_strip_coordinate_jumps(RenderBatch& batch) {
    if (batch.topology != PrimitiveTopology::LineStrip ||
        batch.vertex_data.size() < 8 ||
        batch.entity_starts.empty()) {
        return;
    }

    const auto& vd = batch.vertex_data;
    struct Range { size_t start = 0; size_t end = 0; };
    std::vector<Range> ranges;
    ranges.reserve(batch.entity_starts.size());
    for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
        const size_t start = static_cast<size_t>(batch.entity_starts[ei]) * 2;
        const size_t end = (ei + 1 < batch.entity_starts.size())
            ? static_cast<size_t>(batch.entity_starts[ei + 1]) * 2
            : vd.size();
        if (start + 3 < end && end <= vd.size()) {
            ranges.push_back({start, end});
        }
    }
    if (ranges.empty()) return;

    std::vector<float> filtered;
    std::vector<uint32_t> starts;
    std::vector<float> path;

    auto is_coordinate_jump = [](float x0, float y0, float x1, float y1, float len, float threshold) {
        if (!std::isfinite(len)) return true;
        if (len > threshold) return true;
        const float n0 = std::sqrt(x0 * x0 + y0 * y0);
        const float n1 = std::sqrt(x1 * x1 + y1 * y1);
        const float max_norm = std::max(n0, n1);
        const float min_norm = std::min(n0, n1);
        return len > 100000.0f &&
               max_norm > 100000.0f &&
               min_norm < max_norm * 0.01f;
    };

    auto flush_path = [&]() {
        if (path.size() >= 4) {
            starts.push_back(static_cast<uint32_t>(filtered.size() / 2));
            filtered.insert(filtered.end(), path.begin(), path.end());
        }
        path.clear();
    };

    for (const auto& range : ranges) {
        std::vector<float> lengths;
        for (size_t i = range.start; i + 3 < range.end; i += 2) {
            const float dx = vd[i + 2] - vd[i];
            const float dy = vd[i + 3] - vd[i + 1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (std::isfinite(len)) lengths.push_back(len);
        }
        float threshold = 1000000.0f;
        if (lengths.size() >= 3) {
            std::sort(lengths.begin(), lengths.end());
            const float median = lengths[lengths.size() / 2];
            const float q90 = lengths[std::min(lengths.size() - 1,
                                               static_cast<size_t>(lengths.size() * 0.90f))];
            threshold = std::max(10000.0f, std::max(median * 200.0f, q90 * 50.0f));
        }

        path.clear();
        path.push_back(vd[range.start]);
        path.push_back(vd[range.start + 1]);
        for (size_t i = range.start; i + 3 < range.end; i += 2) {
            const float dx = vd[i + 2] - vd[i];
            const float dy = vd[i + 3] - vd[i + 1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (is_coordinate_jump(vd[i], vd[i + 1], vd[i + 2], vd[i + 3], len, threshold)) {
                flush_path();
                path.push_back(vd[i + 2]);
                path.push_back(vd[i + 3]);
                continue;
            }
            path.push_back(vd[i + 2]);
            path.push_back(vd[i + 3]);
        }
        flush_path();
    }

    batch.vertex_data = std::move(filtered);
    batch.entity_starts = std::move(starts);
}

} // namespace

RenderKey RenderKey::make(uint16_t layer, uint8_t primitive, uint8_t linetype,
                          uint8_t entity_type, uint32_t depth_order) {
    RenderKey rk;
    rk.key = (static_cast<uint64_t>(layer) << 48) |
             (static_cast<uint64_t>(primitive) << 40) |
             (static_cast<uint64_t>(linetype) << 32) |
             (static_cast<uint64_t>(entity_type) << 24) |
             (static_cast<uint64_t>(depth_order) & 0x00FFFFFF);
    return rk;
}

RenderBatcher::RenderBatcher()
    : m_tessellation_quality(1.0f)
    , m_camera(nullptr) {}

void RenderBatcher::set_tessellation_quality(float quality) {
    m_tessellation_quality = math::clamp(quality, 0.1f, 4.0f);
}

int RenderBatcher::compute_arc_segments(float radius) const {
    float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
    return LodSelector::compute_circle_segments(radius, ppu * m_tessellation_quality);
}

void RenderBatcher::begin_frame(const Camera& camera) {
    m_camera = &camera;
    m_batches.clear();
    m_block_cache.clear();
    m_cache_access_counter = 0;
    m_tessellating_blocks.clear();
    m_insert_vertex_count = 0;
}

void RenderBatcher::submit_entity(const EntityVariant& entity, const SceneGraph& scene) {
    // Skip block child entities — they are only rendered through INSERT expansion
    if (entity.header.in_block) return;
    submit_entity_impl(entity, scene, Matrix4x4::identity(), 0, nullptr);
}

void RenderBatcher::submit_entity_impl(const EntityVariant& entity, const SceneGraph& scene,
                                        const Matrix4x4& xform, int depth,
                                        const EntityHeader* inherited_header) {
    if (!entity.is_visible()) return;
    if (depth > 16) return; // prevent infinite recursion

    const auto& layers = scene.layers();
    const Layer* layer = nullptr;
    int32_t li = entity.header.layer_index;
    if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
        layer = &layers[static_cast<size_t>(li)];
    }

    auto layer_for = [&layers](const EntityHeader& hdr) -> const Layer* {
        int32_t idx = hdr.layer_index;
        if (idx >= 0 && static_cast<size_t>(idx) < layers.size()) {
            return &layers[static_cast<size_t>(idx)];
        }
        return nullptr;
    };

    auto color_for = [&](const EntityHeader& hdr, const Layer* hdr_layer) -> Color {
        if (hdr.has_true_color) return hdr.true_color;
        if (hdr.color_override != 256 && hdr.color_override != 0) {
            return Color::from_aci(hdr.color_override);
        }
        if (hdr_layer) return hdr_layer->color;
        return Color::white();
    };

    // Resolve color: TrueColor > explicit ACI > ByBlock inherited > ByLayer.
    Color draw_color;
    if (entity.header.color_override == 0 && inherited_header) {
        draw_color = color_for(*inherited_header, layer_for(*inherited_header));
    } else {
        draw_color = color_for(entity.header, layer);
    }

    float line_width = 1.0f;
    if (entity.header.lineweight < 0.0f && inherited_header) {
        const Layer* inherited_layer = layer_for(*inherited_header);
        if (inherited_header->lineweight > 0.0f) {
            line_width = inherited_header->lineweight;
        } else if (inherited_layer && inherited_layer->lineweight > 0.0f) {
            line_width = inherited_layer->lineweight;
        }
    } else if (entity.header.lineweight > 0.0f) {
        line_width = entity.header.lineweight;
    } else if (layer && layer->lineweight > 0.0f) {
        line_width = layer->lineweight;
    }
    if (!std::isfinite(line_width) || line_width <= 0.0f) line_width = 1.0f;

    LinePattern line_pattern;
    int32_t linetype_index = entity.header.linetype_index;
    if (linetype_index == -2 && inherited_header) {
        const Layer* inherited_layer = layer_for(*inherited_header);
        linetype_index = inherited_header->linetype_index;
        if (linetype_index < 0 && inherited_layer) {
            linetype_index = inherited_layer->linetype_index;
        }
    }
    if (linetype_index < 0 && layer) {
        linetype_index = layer->linetype_index;
    }
    const auto& linetypes = scene.linetypes();
    if (linetype_index >= 0 && static_cast<size_t>(linetype_index) < linetypes.size()) {
        line_pattern = linetypes[static_cast<size_t>(linetype_index)].pattern;
    }

    // Helper: find or create a batch with matching visible CAD appearance.
    auto find_batch = [this, line_width, &line_pattern, &entity](
        PrimitiveTopology topo, const Color& col) -> RenderBatch* {
        // Max vertices per batch — prevents single giant batches that stall
        // GPU upload and prevent per-entity frustum culling. 512K vertices
        // ≈ 4 MB vertex data, well within typical GPU buffer limits.
        static constexpr size_t kMaxBatchVertices = 512 * 1024;
        for (auto& b : m_batches) {
            if (b.topology == topo && b.color == col &&
                b.line_width == line_width &&
                same_pattern(b.line_pattern, line_pattern) &&
                b.space == entity.header.space &&
                b.layout_index == entity.header.layout_index &&
                b.viewport_index == entity.header.viewport_index) {
                if (b.vertex_data.size() / 2 < kMaxBatchVertices) {
                    return &b;
                }
            }
        }
        m_batches.push_back(RenderBatch{});
        auto& b = m_batches.back();
        b.topology = topo;
        b.color = col;
        b.line_width = line_width;
        b.line_pattern = line_pattern;
        b.space = entity.header.space;
        b.layout_index = entity.header.layout_index;
        b.viewport_index = entity.header.viewport_index;
        b.draw_order = entity.header.draw_order;
        return &b;
    };

    uint8_t entity_type_u8 = static_cast<uint8_t>(entity.header.type);
    int32_t entity_index = static_cast<int32_t>(entity.header.entity_id);
    uint16_t layer_u16 = static_cast<uint16_t>(entity.header.layer_index);
    uint32_t depth_order = entity.header.draw_order != 0
        ? entity.header.draw_order
        : static_cast<uint32_t>(entity_index & 0x00FFFFFF);

    // Helper: transform a world point through the current xform
    auto tx = [&xform](float x, float y) -> std::pair<float, float> {
        Vec3 p = xform.transform_point({x, y, 0.0f});
        if (!is_renderable_coord(p.x, p.y))
            return {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::quiet_NaN()};
        return {p.x, p.y};
    };

    switch (entity.header.type) {

    case EntityType::Line: {
        auto* line = std::get_if<0>(&entity.data);
        if (!line) break;
        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            depth_order);
        tessellate_line(line->start, line->end, *batch, xform);
        break;
    }

    case EntityType::Circle: {
        auto* circle = std::get_if<1>(&entity.data);
        if (!circle) break;
        float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        float screen_radius = circle->radius * ppu * m_tessellation_quality;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        // For large screen-space circles, use adaptive recursive midpoint subdivision.
        const float kAdaptiveThresholdPx = 100.0f / std::max(1.0f, m_tessellation_quality);
        if (screen_radius > kAdaptiveThresholdPx) {
            tessellate_arc_adaptive(circle->center, circle->radius,
                                    0.0f, math::TWO_PI, *batch, xform);
        } else {
            int segments = compute_arc_segments(circle->radius);
            tessellate_circle(circle->center, circle->radius, segments, *batch, xform);
        }
        break;
    }

    case EntityType::Arc: {
        auto* arc = std::get_if<2>(&entity.data);
        if (!arc) break;
        float arc_angle = arc->end_angle - arc->start_angle;
        float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        float screen_radius = arc->radius * ppu * m_tessellation_quality;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        // For large screen-space arcs, use adaptive recursive midpoint subdivision.
        // For small arcs, use uniform subdivision from chord-height LOD.
        const float kAdaptiveThresholdPx = 100.0f / std::max(1.0f, m_tessellation_quality);
        if (screen_radius > kAdaptiveThresholdPx) {
            tessellate_arc_adaptive(arc->center, arc->radius,
                                    arc->start_angle, arc->end_angle,
                                    *batch, xform);
        } else {
            int segments = LodSelector::compute_arc_segments(arc->radius, arc_angle, ppu * m_tessellation_quality);
            tessellate_arc(arc->center, arc->radius, arc->start_angle, arc->end_angle, segments, *batch, xform);
        }
        break;
    }

    case EntityType::LwPolyline: {
        auto* poly = std::get_if<4>(&entity.data);
        if (!poly) break;
        int32_t count = poly->vertex_count;
        int32_t offset = poly->vertex_offset;
        if (count < 2) break;
        const auto& vb = scene.vertex_buffer();
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        for (int32_t i = 0; i < count - 1; ++i) {
            int32_t idx0 = offset + i;
            int32_t idx1 = offset + i + 1;
            if (idx0 < 0 || static_cast<size_t>(idx0) >= vb.size()) continue;
            if (idx1 < 0 || static_cast<size_t>(idx1) >= vb.size()) continue;

            Vec3 p0 = vb[static_cast<size_t>(idx0)];
            Vec3 p1 = vb[static_cast<size_t>(idx1)];

            float bulge = (static_cast<size_t>(i) < poly->bulges.size())
                              ? poly->bulges[static_cast<size_t>(i)] : 0.0f;

            if (std::abs(bulge) < 1e-6f) {
                // Straight segment — transform the point
                auto [tx0, ty0] = tx(p0.x, p0.y);
                append_vertex(batch->vertex_data, tx0, ty0);
            } else {
                // Bulge arc: compute center/radius in local space, then transform
                float theta = 4.0f * std::atan(std::abs(bulge));
                float dx = p1.x - p0.x;
                float dy = p1.y - p0.y;
                float chord_len = std::sqrt(dx * dx + dy * dy);
                if (chord_len < 1e-12f) continue;

                float radius = chord_len / (2.0f * std::sin(theta * 0.5f));
                float mx = (p0.x + p1.x) * 0.5f;
                float my = (p0.y + p1.y) * 0.5f;
                float nx = -dy / chord_len;
                float ny = dx / chord_len;
                float s = (bulge > 0) ? 1.0f : -1.0f;
                float dist = radius * std::cos(theta * 0.5f);
                float cx = mx + s * dist * nx;
                float cy = my + s * dist * ny;

                float start_a = std::atan2(p0.y - cy, p0.x - cx);
                float end_a = std::atan2(p1.y - cy, p1.x - cx);
                if (bulge > 0) {
                    if (end_a <= start_a) end_a += math::TWO_PI;
                } else {
                    if (end_a >= start_a) end_a -= math::TWO_PI;
                }

                float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
                int segs = LodSelector::compute_arc_segments(radius, std::abs(end_a - start_a),
                                                              ppu * m_tessellation_quality);
                segs = std::max(segs, 4);

                int limit = (i < count - 2) ? segs : segs + 1;
                for (int j = 0; j < limit; ++j) {
                    float t = static_cast<float>(j) / static_cast<float>(segs);
                    float a = start_a + t * (end_a - start_a);
                    float ax = cx + radius * std::cos(a);
                    float ay = cy + radius * std::sin(a);
                    auto [tax, tay] = tx(ax, ay);
                    append_vertex(batch->vertex_data, tax, tay);
                }
            }
        }
        // Add the last vertex if it wasn't added yet
        {
            int32_t last_idx = offset + count - 1;
            if (last_idx >= 0 && static_cast<size_t>(last_idx) < vb.size()) {
                Vec3 last_pt = vb[static_cast<size_t>(last_idx)];
                float last_bulge = (static_cast<size_t>(count - 2) < poly->bulges.size())
                                       ? poly->bulges[static_cast<size_t>(count - 2)] : 0.0f;
                if (std::abs(last_bulge) < 1e-6f) {
                    auto [tlx, tly] = tx(last_pt.x, last_pt.y);
                    append_vertex(batch->vertex_data, tlx, tly);
                }
            }
        }
        // Close polyline if flagged
        if (poly->is_closed && count > 1) {
            int32_t first_idx = offset;
            if (first_idx >= 0 && static_cast<size_t>(first_idx) < vb.size()) {
                Vec3 first_pt = vb[static_cast<size_t>(first_idx)];
                auto [fx, fy] = tx(first_pt.x, first_pt.y);
                append_vertex(batch->vertex_data, fx, fy);
            }
        }
        break;
    }

    case EntityType::Insert: {
        auto* ins = std::get_if<10>(&entity.data);
        if (!ins) break;

        // Sanity check: skip INSERT entities with extreme coordinates
        {
            float max_coord = 1e8f;
            float ip = std::max(std::abs(ins->insertion_point.x), std::abs(ins->insertion_point.y));
            float sc = std::max(std::abs(ins->x_scale), std::abs(ins->y_scale));
            if (ip > max_coord || sc > 1e4f || (ip * sc > 1e9f)) break;
        }

        const auto& blocks = scene.blocks();
        if (ins->block_index < 0 ||
            static_cast<size_t>(ins->block_index) >= blocks.size()) break;

        const auto& block = blocks[static_cast<size_t>(ins->block_index)];
        const auto& all_entities = scene.entities();

        // Skip INSERTs of model/paper space (case-insensitive)
        // but allow *D (dimension) and other anonymous blocks.
        {
            std::string uname = block.name;
            std::transform(uname.begin(), uname.end(), uname.begin(), ::toupper);
            if (uname == "*MODEL_SPACE" || uname == "*PAPER_SPACE") break;
        }

        int32_t bk_idx = ins->block_index;

        constexpr size_t MAX_BLOCK_VERTICES = 500000;

        bool uses_byblock = false;
        for (int32_t ei : block.entity_indices) {
            if (ei < 0 || static_cast<size_t>(ei) >= all_entities.size()) continue;
            const auto& child_hdr = all_entities[static_cast<size_t>(ei)].header;
            if (child_hdr.color_override == 0 ||
                child_hdr.linetype_index == -2 ||
                child_hdr.lineweight < 0.0f) {
                uses_byblock = true;
                break;
            }
        }

        // Check block tessellation cache
        auto cache_it = m_block_cache.find(bk_idx);
        if (cache_it == m_block_cache.end()) {
            // Cycle detection: skip blocks already in the tessellation stack.
            if (m_tessellating_blocks.count(bk_idx)) break;
            m_tessellating_blocks.insert(bk_idx);

            std::vector<RenderBatch> local_batches;
            m_batches.swap(local_batches);
            for (int32_t ei : block.entity_indices) {
                if (ei < 0 || static_cast<size_t>(ei) >= all_entities.size()) continue;
                const auto& child = all_entities[static_cast<size_t>(ei)];
                if (!child.is_visible()) continue;
                submit_entity_impl(child, scene, Matrix4x4::identity(), depth + 1);
            }
            size_t total_verts = 0;
            double cx = 0.0, cy = 0.0;
            for (const auto& b : m_batches) {
                total_verts += b.vertex_data.size() / 2;
                for (size_t i = 0; i + 1 < b.vertex_data.size(); i += 2) {
                    cx += b.vertex_data[i];
                    cy += b.vertex_data[i + 1];
                }
            }
            if (total_verts > 0) { cx /= total_verts; cy /= total_verts; }
            double cdist = std::sqrt(cx * cx + cy * cy);

            m_block_cache[bk_idx] = {std::move(m_batches), total_verts, cdist, cx, cy, ++m_cache_access_counter};
            m_tessellating_blocks.erase(bk_idx);
            m_batches.swap(local_batches);
            cache_it = m_block_cache.find(bk_idx);

            // LRU eviction: if cache grew too large, evict oldest entries
            if (m_block_cache.size() > MAX_CACHE_ENTRIES) {
                uint32_t cutoff = m_cache_access_counter -
                    static_cast<uint32_t>(m_block_cache.size() / 2);
                for (auto it = m_block_cache.begin(); it != m_block_cache.end(); ) {
                    if (it->first != bk_idx && it->second.last_used < cutoff) {
                        it = m_block_cache.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        cache_it->second.last_used = ++m_cache_access_counter;

        if (cache_it->second.vertex_count > MAX_BLOCK_VERTICES) break;
        if (m_insert_vertex_count >= m_insert_vertex_budget) break;

        const auto& cached_batches = cache_it->second.batches;
        double cx = cache_it->second.centroid_x;
        double cy = cache_it->second.centroid_y;
        double centroid_dist = cache_it->second.centroid_dist;

        float bpx = std::isfinite(block.base_point.x) ? block.base_point.x : 0.0f;
        float bpy = std::isfinite(block.base_point.y) ? block.base_point.y : 0.0f;
        if (bpx == 0.0f && bpy == 0.0f && centroid_dist > 5000.0) {
            bpx = static_cast<float>(cx);
            bpy = static_cast<float>(cy);
        }
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

        for (int32_t col = 0; col < col_limit; ++col) {
            for (int32_t row = 0; row < row_limit; ++row) {
                Vec3 arr_off(
                    static_cast<float>(col) * ins->column_spacing,
                    static_cast<float>(row) * ins->row_spacing, 0.0f);
                Matrix4x4 final_xform = base_offset * insert_xform *
                    Matrix4x4::translation_2d(arr_off.x, arr_off.y) * xform;

                if (uses_byblock) {
                    for (int32_t ei : block.entity_indices) {
                        if (ei < 0 || static_cast<size_t>(ei) >= all_entities.size()) continue;
                        const auto& child = all_entities[static_cast<size_t>(ei)];
                        if (!child.is_visible()) continue;
                        submit_entity_impl(child, scene, final_xform, depth + 1, &entity.header);
                    }
                    continue;
                }

                for (const RenderBatch& src : cached_batches) {
                    RenderBatch* dst = nullptr;
                    for (auto& candidate : m_batches) {
                        if (candidate.topology == src.topology &&
                            candidate.color == src.color &&
                            candidate.line_width == src.line_width &&
                            same_pattern(candidate.line_pattern, src.line_pattern) &&
                            candidate.space == src.space &&
                            candidate.layout_index == src.layout_index &&
                            candidate.viewport_index == src.viewport_index) {
                            dst = &candidate;
                            break;
                        }
                    }
                    if (!dst) {
                        m_batches.push_back(RenderBatch{});
                        dst = &m_batches.back();
                        dst->sort_key = src.sort_key;
                        dst->topology = src.topology;
                        dst->color = src.color;
                        dst->line_width = src.line_width;
                        dst->line_pattern = src.line_pattern;
                        dst->space = src.space;
                        dst->layout_index = src.layout_index;
                        dst->viewport_index = src.viewport_index;
                        dst->draw_order = src.draw_order;
                    }
                    uint32_t dst_voff = static_cast<uint32_t>(dst->vertex_data.size() / 2);

                    std::vector<float> transformed;
                    transformed.reserve(src.vertex_data.size());
                    bool all_valid = true;
                    for (size_t i = 0; i < src.vertex_data.size(); i += 2) {
                        Vec3 p = final_xform.transform_point(
                            {src.vertex_data[i], src.vertex_data[i + 1], 0.0f});
                        if (!is_renderable_coord(p.x, p.y)) {
                            all_valid = false;
                            break;
                        }
                        transformed.push_back(p.x);
                        transformed.push_back(p.y);
                    }
                    if (!all_valid) continue;

                    dst->vertex_data.insert(dst->vertex_data.end(),
                                            transformed.begin(), transformed.end());
                    m_insert_vertex_count += src.vertex_data.size() / 2;

                    for (uint32_t es : src.entity_starts) {
                        dst->entity_starts.push_back(dst_voff + es);
                    }
                }
            }
        }
        break;
    }

    // Polyline (non-LW) — same structure, different index
    case EntityType::Polyline: {
        auto* poly = std::get_if<3>(&entity.data);
        if (!poly) break;
        int32_t count = poly->vertex_count;
        int32_t offset = poly->vertex_offset;
        if (count < 2) break;
        const auto& vb = scene.vertex_buffer();
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));
        for (int32_t i = 0; i < count; ++i) {
            int32_t idx = offset + i;
            if (idx < 0 || static_cast<size_t>(idx) >= vb.size()) continue;
            Vec3 pt = vb[static_cast<size_t>(idx)];
            auto [tpx, tpy] = tx(pt.x, pt.y);
            append_vertex(batch->vertex_data, tpx, tpy);
        }
        // Close polyline if flagged
        if (poly->is_closed && count > 1) {
            int32_t first_idx = offset;
            if (first_idx >= 0 && static_cast<size_t>(first_idx) < vb.size()) {
                Vec3 first_pt = vb[static_cast<size_t>(first_idx)];
                auto [fx, fy] = tx(first_pt.x, first_pt.y);
                append_vertex(batch->vertex_data, fx, fy);
            }
        }
        break;
    }

    // Hatch — solid fill polygon
    case EntityType::Hatch: {
        auto* hatch = std::get_if<9>(&entity.data);
        if (!hatch || hatch->loops.empty()) break;

        if (hatch->is_solid) {
            auto* batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
            batch->sort_key = RenderKey::make(layer_u16,
                static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
                depth_order);

            for (const auto& loop : hatch->loops) {
                if (loop.vertices.size() < 3) continue;
                for (size_t i = 1; i + 1 < loop.vertices.size(); ++i) {
                    auto [x0, y0] = tx(loop.vertices[0].x, loop.vertices[0].y);
                    auto [xi, yi] = tx(loop.vertices[i].x, loop.vertices[i].y);
                    auto [xip, yip] = tx(loop.vertices[i + 1].x, loop.vertices[i + 1].y);
                    if (!is_renderable_coord(x0, y0) ||
                        !is_renderable_coord(xi, yi) ||
                        !is_renderable_coord(xip, yip)) {
                        continue;
                    }
                    append_vertex(batch->vertex_data, x0, y0);
                    append_vertex(batch->vertex_data, xi, yi);
                    append_vertex(batch->vertex_data, xip, yip);
                }
            }
        } else {
            // Pattern hatch — generate line segments clipped to boundary loops.
            // Predefined pattern definitions: angle (deg), spacing, optional cross angle.
            struct PatLine { float angle; float spacing; };
            PatLine pat{};
            std::string name_upper;
            name_upper.reserve(hatch->pattern_name.size());
            for (char c : hatch->pattern_name) name_upper.push_back(static_cast<char>(std::toupper(c)));

            if (name_upper.find("ANSI31") != std::string::npos) {
                pat = {45.0f, 3.0f};
            } else if (name_upper.find("ANSI32") != std::string::npos) {
                pat = {45.0f, 8.0f};
            } else if (name_upper.find("ANSI33") != std::string::npos) {
                pat = {45.0f, 4.0f};
            } else if (name_upper.find("ANSI37") != std::string::npos) {
                pat = {45.0f, 3.0f};  // cross lines handled below
            } else if (name_upper.find("ANSI38") != std::string::npos) {
                pat = {45.0f, 6.0f};
            } else if (name_upper.find("AR-CONC") != std::string::npos ||
                       name_upper.find("ARCONC") != std::string::npos) {
                pat = {45.0f, 5.0f};
            } else if (name_upper.find("AR-SAND") != std::string::npos) {
                pat = {45.0f, 4.0f};
            } else if (name_upper.find("CROSS") != std::string::npos ||
                       name_upper.find("NET") != std::string::npos ||
                       name_upper.find("GRID") != std::string::npos) {
                pat = {45.0f, 3.0f};  // cross handled below
            } else {
                // Fallback: use pattern_angle from entity, or 45 degrees
                pat = {(hatch->pattern_angle != 0.0f) ? hatch->pattern_angle : 45.0f, 4.0f};
            }
            pat.spacing *= hatch->pattern_scale;
            if (pat.spacing < 0.1f) pat.spacing = 4.0f;

            // Compute bounding box of all loops
            float bmin_x = 1e30f, bmin_y = 1e30f, bmax_x = -1e30f, bmax_y = -1e30f;
            for (const auto& loop : hatch->loops) {
                for (const auto& v : loop.vertices) {
                    bmin_x = std::min(bmin_x, v.x); bmin_y = std::min(bmin_y, v.y);
                    bmax_x = std::max(bmax_x, v.x); bmax_y = std::max(bmax_y, v.y);
                }
            }
            if (bmin_x >= bmax_x || bmin_y >= bmax_y) break;

            float rad = math::DEG_TO_RAD * pat.angle;
            float cos_a = std::cos(rad), sin_a = std::sin(rad);
            // Direction perpendicular to pattern lines = offset direction
            float perp_x = -sin_a, perp_y = cos_a;
            // Line direction = along pattern lines
            float dir_x = cos_a, dir_y = sin_a;

            // Extent of boundary projected onto perpendicular direction
            float proj_min = bmin_x * perp_x + bmin_y * perp_y;
            float proj_max = bmax_x * perp_x + bmax_y * perp_y;
            float diag = std::sqrt((bmax_x - bmin_x) * (bmax_x - bmin_x) +
                                   (bmax_y - bmin_y) * (bmax_y - bmin_y));
            proj_min -= diag;
            proj_max += diag;

            // Point-in-polygon test for a single loop (ray casting)
            auto point_in_loop = [](const HatchEntity::BoundaryLoop& loop, float px, float py) -> bool {
                bool inside = false;
                const auto& v = loop.vertices;
                size_t n = v.size();
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    if (((v[i].y > py) != (v[j].y > py)) &&
                        (px < (v[j].x - v[i].x) * (py - v[i].y) / (v[j].y - v[i].y) + v[i].x)) {
                        inside = !inside;
                    }
                }
                return inside;
            };

            auto point_in_any_loop = [&](float px, float py) -> bool {
                for (const auto& loop : hatch->loops) {
                    if (point_in_loop(loop, px, py)) return true;
                }
                return false;
            };

            // Line segment length to extend beyond bounds (ensures full coverage)
            float line_half = diag * 1.5f;

            auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
            batch->sort_key = RenderKey::make(layer_u16,
                static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
                depth_order);

            // Generate pattern lines
            auto emit_pattern_lines = [&](float angle_rad) {
                float ca = std::cos(angle_rad), sa = std::sin(angle_rad);
                float px = -sa, py = ca;
                float dx = ca, dy = sa;
                float pmin = bmin_x * px + bmin_y * py - diag;
                float pmax = bmax_x * px + bmax_y * py + diag;

                // Fine-grained subdivision for clipping accuracy
                float seg_len = std::min(pat.spacing * 0.25f, 2.0f);

                for (float d = pmin; d <= pmax; d += pat.spacing) {
                    float ox = px * d, oy = py * d;
                    // Compute actual t range that covers boundary for this line
                    float t_corners[4] = {
                        (bmin_x - ox) * dx + (bmin_y - oy) * dy,
                        (bmax_x - ox) * dx + (bmin_y - oy) * dy,
                        (bmax_x - ox) * dx + (bmax_y - oy) * dy,
                        (bmin_x - ox) * dx + (bmax_y - oy) * dy,
                    };
                    float t_lo = t_corners[0], t_hi = t_corners[0];
                    for (int k = 1; k < 4; ++k) {
                        t_lo = std::min(t_lo, t_corners[k]);
                        t_hi = std::max(t_hi, t_corners[k]);
                    }
                    int steps = std::max(4, static_cast<int>((t_hi - t_lo) / seg_len));
                    float dt = (t_hi - t_lo) / static_cast<float>(steps);

                    for (int s = 0; s < steps; ++s) {
                        float t0 = t_lo + static_cast<float>(s) * dt;
                        float t1 = t0 + dt;
                        float ax = ox + dx * t0, ay = oy + dy * t0;
                        float bx = ox + dx * t1, by = oy + dy * t1;
                        if (point_in_any_loop(ax, ay) && point_in_any_loop(bx, by)) {
                            if (is_renderable_coord(ax, ay) && is_renderable_coord(bx, by)) {
                                auto [tax, tay] = tx(ax, ay);
                                auto [tbx, tby] = tx(bx, by);
                                append_vertex(batch->vertex_data, tax, tay);
                                append_vertex(batch->vertex_data, tbx, tby);
                            }
                        }
                    }
                }
            };

            emit_pattern_lines(rad);

            // Cross patterns: ANSI37, ANSI38, CROSS, NET, GRID
            bool is_cross = (name_upper.find("ANSI37") != std::string::npos ||
                             name_upper.find("CROSS") != std::string::npos ||
                             name_upper.find("NET") != std::string::npos ||
                             name_upper.find("GRID") != std::string::npos);
            if (is_cross) {
                emit_pattern_lines(rad + math::PI * 0.5f);
            }
        }
        break;
    }

    // Solid — filled 3/4 vertex polygon
    case EntityType::Solid: {
        auto* solid = std::get_if<16>(&entity.data);
        if (!solid || solid->corner_count < 3) break;

        auto* batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
            depth_order);

        auto [x0, y0] = tx(solid->corners[0].x, solid->corners[0].y);
        auto [x1, y1] = tx(solid->corners[1].x, solid->corners[1].y);
        auto [x2, y2] = tx(solid->corners[2].x, solid->corners[2].y);

        if (solid->corner_count == 3) {
            if (!is_renderable_coord(x0, y0) ||
                !is_renderable_coord(x1, y1) ||
                !is_renderable_coord(x2, y2)) {
                break;
            }
            append_vertex(batch->vertex_data, x0, y0);
            append_vertex(batch->vertex_data, x1, y1);
            append_vertex(batch->vertex_data, x2, y2);
        } else {
            auto [x3, y3] = tx(solid->corners[3].x, solid->corners[3].y);
            if (!is_renderable_coord(x0, y0) ||
                !is_renderable_coord(x1, y1) ||
                !is_renderable_coord(x2, y2) ||
                !is_renderable_coord(x3, y3)) {
                break;
            }
            // DXF order: 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3
            append_vertex(batch->vertex_data, x0, y0);
            append_vertex(batch->vertex_data, x1, y1);
            append_vertex(batch->vertex_data, x3, y3);

            append_vertex(batch->vertex_data, x1, y1);
            append_vertex(batch->vertex_data, x2, y2);
            append_vertex(batch->vertex_data, x3, y3);
        }
        break;
    }

    // Ellipse — tessellate as parametric curve
    case EntityType::Ellipse: {
        auto* ellipse = std::get_if<12>(&entity.data);
        if (!ellipse) break;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        float major_r = ellipse->radius;
        float minor_r = (ellipse->minor_radius > 0.0f) ? ellipse->minor_radius : major_r;
        float rotation = ellipse->rotation;
        float start_a = ellipse->start_angle;
        float end_a = ellipse->end_angle;

        // Full ellipse if start==end==0
        bool is_full = (start_a == 0.0f && end_a == 0.0f) ||
                       std::abs(end_a - start_a - 2.0f * math::PI) < 0.01f;
        if (is_full) {
            start_a = 0.0f;
            end_a = math::TWO_PI;
        }

        float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        float max_r = std::max(major_r, minor_r);
        float screen_size = max_r * ppu * m_tessellation_quality;
        float span = end_a - start_a;

        float cos_r = std::cos(rotation);
        float sin_r = std::sin(rotation);
        float cx = ellipse->center.x;
        float cy = ellipse->center.y;

        // Helper to compute ellipse point at parametric angle
        auto ellipse_point = [&](float angle) -> std::pair<float, float> {
            float px = major_r * std::cos(angle);
            float py = minor_r * std::sin(angle);
            float x = px * cos_r - py * sin_r + cx;
            float y = px * sin_r + py * cos_r + cy;
            return {x, y};
        };

        // For large screen-space ellipses, use adaptive subdivision
        // based on chord deviation in world space.
        const float kAdaptiveThresholdPx = 1000.0f / std::max(1.0f, m_tessellation_quality);
        if (screen_size > kAdaptiveThresholdPx) {
            const float kPixelTolerance = 0.5f / std::max(1.0f, m_tessellation_quality);
            constexpr int kMaxDepth = 12;

            std::function<void(float, float, int)> subdivide =
                [&](float a0, float a1, int depth) {
                // Compute chord deviation at midpoint
                float mid = (a0 + a1) * 0.5f;
                auto [mx, my] = ellipse_point(mid);
                auto [sx, sy] = ellipse_point(a0);
                auto [ex, ey] = ellipse_point(a1);

                // Approximate sagitta: distance from midpoint to chord
                float chord_dx = ex - sx;
                float chord_dy = ey - sy;
                float chord_len_sq = chord_dx * chord_dx + chord_dy * chord_dy;
                float sagitta_px = kPixelTolerance + 1.0f; // default: subdivide
                if (chord_len_sq > 1e-12f) {
                    // Perpendicular distance from midpoint to chord line
                    float cross = std::abs(chord_dx * (sy - my) - chord_dy * (sx - mx));
                    float chord_len = std::sqrt(chord_len_sq);
                    float sagitta_world = cross / chord_len;
                    sagitta_px = sagitta_world * ppu;
                }

                if (sagitta_px <= kPixelTolerance || depth >= kMaxDepth) {
                    auto [px, py] = ellipse_point(a1);
                    auto [tpx, tpy] = tx(px, py);
                    append_vertex(batch->vertex_data, tpx, tpy);
                    return;
                }
                subdivide(a0, mid, depth + 1);
                subdivide(mid, a1, depth + 1);
            };

            // Add start point
            {
                auto [sx, sy] = ellipse_point(start_a);
                auto [tpx, tpy] = tx(sx, sy);
                append_vertex(batch->vertex_data, tpx, tpy);
            }
            subdivide(start_a, end_a, 0);
        } else {
            int segments = compute_arc_segments(max_r);
            for (int i = 0; i <= segments; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(segments);
                float angle = start_a + t * span;
                auto [x, y] = ellipse_point(angle);
                auto [tx_x, tx_y] = tx(x, y);
                append_vertex(batch->vertex_data, tx_x, tx_y);
            }
        }
        break;
    }

    // Spline — tessellate B-spline knots/control points or fit-point curves.
    case EntityType::Spline: {
        auto* spline = std::get_if<5>(&entity.data);
        if (!spline) break;

        float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        std::vector<Vec3> sampled = tessellate_spline_points(*spline, ppu * m_tessellation_quality);
        if (sampled.size() < 2) break;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        for (const auto& pt : sampled) {
            auto [tpx, tpy] = tx(pt.x, pt.y);
            append_vertex(batch->vertex_data, tpx, tpy);
        }
        break;
    }

    // Text — render a thin underline indicator; actual text is emitted separately.
    case EntityType::Text:
    case EntityType::MText:
    case EntityType::Tolerance: {
        auto* text = std::get_if<6>(&entity.data);
        if (!text) text = std::get_if<7>(&entity.data);
        if (!text) text = std::get_if<18>(&entity.data);
        if (!text || text->height <= 0.0f) break;

        float x = text->insertion_point.x;
        float y = text->insertion_point.y;
        float hw = std::min(text->height * 4.0f,
                            text->height * static_cast<float>(text->text.size()) *
                                text->width_factor * 0.3f);

        auto [rx0, ry0] = tx(x, y);
        auto [rx1, ry1] = tx(x + hw, y);
        if (!is_renderable_coord(rx0, ry0) || !is_renderable_coord(rx1, ry1)) break;

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            depth_order);
        append_vertex(batch->vertex_data, rx0, ry0);
        append_vertex(batch->vertex_data, rx1, ry1);
        break;
    }

    // Point — render crosshair marker
    case EntityType::Point: {
        auto* pt = std::get_if<11>(&entity.data);
        if (!pt) break;
        auto [px, py] = tx(pt->position.x, pt->position.y);
        if (!is_renderable_coord(px, py)) break;

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            depth_order);

        // Draw a crosshair marker (±3 pixels in world space at current zoom)
        float marker = 3.0f;
        auto inv = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        if (inv > 1e-6f) marker = 3.0f / (inv * m_tessellation_quality);
        marker = std::max(marker, 0.5f);

        append_vertex(batch->vertex_data, px - marker, py);
        append_vertex(batch->vertex_data, px + marker, py);
        append_vertex(batch->vertex_data, px, py - marker);
        append_vertex(batch->vertex_data, px, py + marker);
        break;
    }

    // Dimension — render as dimension lines (extension lines + dimension line)
    case EntityType::Dimension: {
        auto* dim = std::get_if<8>(&entity.data);
        if (!dim) break;

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            depth_order);

        // The definition_point (10/20/30) is typically the point where dimension line meets
        // the second extension line. We need to draw:
        // 1. Extension lines from ext origins toward the dimension line
        // 2. Dimension line between the two definition points
        // Since we only have definition_point and text_midpoint, we draw the dimension line
        // and short extension line indicators.

        auto [dx, dy] = tx(dim->definition_point.x, dim->definition_point.y);
        auto [mx, my] = tx(dim->text_midpoint.x, dim->text_midpoint.y);

        // Draw a simple dimension line from midpoint to definition point
        // (In a full implementation, we'd use ext1/ext2 start points for extension lines)
        if (!is_renderable_coord(dx, dy) || !is_renderable_coord(mx, my)) break;

        append_vertex(batch->vertex_data, mx, my);
        append_vertex(batch->vertex_data, dx, dy);

        // Draw tick marks at the definition point end
        float tick_size = 3.0f;
        float angle = std::atan2(dy - my, dx - mx);
        float perp_x = -std::sin(angle) * tick_size;
        float perp_y = std::cos(angle) * tick_size;

        // Tick at definition point
        append_vertex(batch->vertex_data, dx - perp_x, dy - perp_y);
        append_vertex(batch->vertex_data, dx + perp_x, dy + perp_y);

        // Tick at midpoint side (mirrored direction)
        append_vertex(batch->vertex_data, mx - perp_x, my - perp_y);
        append_vertex(batch->vertex_data, mx + perp_x, my + perp_y);
        break;
    }

    // Leader — render as linestrip from vertex buffer
    case EntityType::MLine: {
        auto* ml = std::get_if<19>(&entity.data);
        if (!ml || ml->vertex_count < 2) break;
        {
            const auto& vb = scene.vertex_buffer();
            if (ml->vertex_offset < 0 ||
                static_cast<size_t>(ml->vertex_offset + ml->vertex_count) > vb.size()) break;

            auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
            batch->sort_key = RenderKey::make(layer_u16,
                static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
                depth_order);
            batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

            const Vec3* pts = vb.data() + ml->vertex_offset;
            for (int32_t i = 0; i < ml->vertex_count; ++i) {
                auto [px, py] = tx(pts[i].x, pts[i].y);
                append_vertex(batch->vertex_data, px, py);
            }
        }
        break;
    }

    case EntityType::Leader: {
        auto* ld = std::get_if<17>(&entity.data);
        if (!ld || ld->vertex_count < 2) break;
        const auto& vb = scene.vertex_buffer();
        if (ld->vertex_offset < 0 ||
            static_cast<size_t>(ld->vertex_offset + ld->vertex_count) > vb.size()) break;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            depth_order);
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        const Vec3* pts = vb.data() + ld->vertex_offset;
        for (int32_t i = 0; i < ld->vertex_count; ++i) {
            auto [px, py] = tx(pts[i].x, pts[i].y);
            append_vertex(batch->vertex_data, px, py);
        }

        // Arrowhead at first point (start of leader)
        if (ld->has_arrowhead && ld->vertex_count >= 2) {
            auto* arrow_batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
            arrow_batch->sort_key = RenderKey::make(layer_u16,
                static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
                depth_order);

            auto [ax, ay] = tx(pts[0].x, pts[0].y);
            auto [nx, ny] = tx(pts[1].x, pts[1].y);
            float dir_x = ax - nx;
            float dir_y = ay - ny;
            float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
            if (len > 1e-6f) {
                dir_x /= len;
                dir_y /= len;
                float arrow_size = 3.0f;
                auto inv = m_camera ? m_camera->pixels_per_unit() : 1.0f;
                if (inv > 1e-6f) arrow_size = 3.0f / (inv * m_tessellation_quality);
                arrow_size = std::max(arrow_size, 0.5f);

                // Triangle arrow: tip at first point, base perpendicular
                float perp_x = -dir_y * arrow_size * 0.5f;
                float perp_y = dir_x * arrow_size * 0.5f;
                float base_x = ax - dir_x * arrow_size;
                float base_y = ay - dir_y * arrow_size;

                append_vertex(arrow_batch->vertex_data, ax, ay);
                append_vertex(arrow_batch->vertex_data, base_x + perp_x, base_y + perp_y);
                append_vertex(arrow_batch->vertex_data, base_x - perp_x, base_y - perp_y);
            }
        }
        break;
    }

    case EntityType::Ray:
    case EntityType::XLine:
    case EntityType::Viewport:
        break;
    }
}

void RenderBatcher::end_frame() {
    // Sort batches by render key for correct ordering
    std::stable_sort(m_batches.begin(), m_batches.end(),
              [](const RenderBatch& a, const RenderBatch& b) {
                  if (a.draw_order != b.draw_order) return a.draw_order < b.draw_order;
                  return a.sort_key < b.sort_key;
              });

    if (!m_outlier_filter_enabled) return;

    // Filter outlier primitives — remove lines/triangles/strips whose extent
    // far exceeds the batch's median primitive extent. Handles DWG blocks with
    // mixed local/world coordinates producing giant geometry.
    for (auto& batch : m_batches) {
        auto& vd = batch.vertex_data;
        if (vd.size() < 4) continue;

        auto entity_extent = [&vd](size_t start, size_t end) -> float {
            if (end <= start + 1) return 0.0f;
            float minx = vd[start], maxx = vd[start];
            float miny = vd[start+1], maxy = vd[start+1];
            for (size_t i = start+2; i < end; i += 2) {
                if (vd[i] < minx) minx = vd[i];
                if (vd[i] > maxx) maxx = vd[i];
                if (vd[i+1] < miny) miny = vd[i+1];
                if (vd[i+1] > maxy) maxy = vd[i+1];
            }
            float dx = maxx - minx, dy = maxy - miny;
            return std::sqrt(dx*dx + dy*dy);
        };

        if (batch.topology == PrimitiveTopology::LineList) {
            std::vector<float> lengths;
            for (size_t i = 0; i + 3 < vd.size(); i += 4) {
                float dx = vd[i+2] - vd[i];
                float dy = vd[i+3] - vd[i+1];
                lengths.push_back(std::sqrt(dx*dx + dy*dy));
            }
            if (lengths.empty()) continue;
            std::sort(lengths.begin(), lengths.end());
            const float median = lengths[lengths.size() / 2];
            const float q95 = lengths[std::min(lengths.size() - 1,
                                               static_cast<size_t>(lengths.size() * 0.95f))];
            float threshold = std::max({median * 200.0f, q95 * 50.0f, std::max(median * 10.0f, 500.0f)});

            std::vector<float> filtered;
            for (size_t i = 0; i + 3 < vd.size(); i += 4) {
                float dx = vd[i+2] - vd[i], dy = vd[i+3] - vd[i+1];
                if (std::sqrt(dx*dx + dy*dy) <= threshold) {
                    for (int j = 0; j < 4; ++j) filtered.push_back(vd[i+j]);
                }
            }
            vd = std::move(filtered);
        } else if (batch.topology == PrimitiveTopology::TriangleList) {
            std::vector<float> extents;
            for (size_t i = 0; i + 5 < vd.size(); i += 6) {
                extents.push_back(entity_extent(i, i + 6));
            }
            if (extents.empty()) continue;
            std::sort(extents.begin(), extents.end());
            const float median = extents[extents.size() / 2];
            const float q95 = extents[std::min(extents.size() - 1,
                                               static_cast<size_t>(extents.size() * 0.95f))];
            float threshold = std::max({median * 100.0f, q95 * 20.0f, std::max(median * 10.0f, 500.0f)});

            std::vector<float> filtered;
            for (size_t i = 0; i + 5 < vd.size(); i += 6) {
                if (entity_extent(i, i + 6) <= threshold) {
                    for (int j = 0; j < 6; ++j) filtered.push_back(vd[i+j]);
                }
            }
            vd = std::move(filtered);
        } else if (batch.topology == PrimitiveTopology::LineStrip &&
                   !batch.entity_starts.empty()) {
            // Build entity ranges from entity_starts
            struct Range { size_t vstart, vend; };
            std::vector<Range> ranges;
            for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
                size_t vs = batch.entity_starts[ei] * 2;
                size_t ve = (ei + 1 < batch.entity_starts.size())
                    ? batch.entity_starts[ei + 1] * 2 : vd.size();
                if (ve > vs + 1) ranges.push_back({vs, ve});
            }
            if (ranges.empty()) continue;

            // Compute median entity extent
            std::vector<float> extents;
            for (auto& r : ranges) extents.push_back(entity_extent(r.vstart, r.vend));
            std::sort(extents.begin(), extents.end());
            const float median = extents[extents.size() / 2];
            const float q95 = extents[std::min(extents.size() - 1,
                                               static_cast<size_t>(extents.size() * 0.95f))];
            float threshold = std::max({median * 100.0f, q95 * 20.0f, std::max(median * 10.0f, 500.0f)});

            // Filter: keep entities within threshold
            std::vector<float> filtered_vd;
            std::vector<uint32_t> filtered_es;
            for (auto& r : ranges) {
                if (entity_extent(r.vstart, r.vend) <= threshold) {
                    filtered_es.push_back(static_cast<uint32_t>(filtered_vd.size() / 2));
                    for (size_t i = r.vstart; i < r.vend; ++i)
                        filtered_vd.push_back(vd[i]);
                }
            }
            vd = std::move(filtered_vd);
            batch.entity_starts = std::move(filtered_es);
            split_line_strip_coordinate_jumps(batch);
        }
    }
}

void RenderBatcher::tessellate_line(const Vec3& p0, const Vec3& p1,
                                     RenderBatch& batch, const Matrix4x4& xform) {
    Vec3 tp0 = xform.transform_point(p0);
    Vec3 tp1 = xform.transform_point(p1);
    if (!is_renderable_coord(tp0.x, tp0.y) ||
        !is_renderable_coord(tp1.x, tp1.y)) return;
    append_vertex(batch.vertex_data, tp0.x, tp0.y);
    append_vertex(batch.vertex_data, tp1.x, tp1.y);
}

void RenderBatcher::tessellate_circle(const Vec3& center, float radius,
                                       int segments, RenderBatch& batch,
                                       const Matrix4x4& xform) {
    for (int i = 0; i <= segments; ++i) {
        float angle = static_cast<float>(i) / static_cast<float>(segments) * math::TWO_PI;
        float x = center.x + radius * std::cos(angle);
        float y = center.y + radius * std::sin(angle);
        Vec3 tp = xform.transform_point({x, y, 0.0f});
        append_vertex(batch.vertex_data, tp.x, tp.y);
    }
}

void RenderBatcher::tessellate_arc(const Vec3& center, float radius,
                                    float start_angle, float end_angle,
                                    int segments, RenderBatch& batch,
                                    const Matrix4x4& xform) {
    float arc_span = end_angle - start_angle;
    for (int i = 0; i <= segments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(segments);
        float angle = start_angle + t * arc_span;
        float x = center.x + radius * std::cos(angle);
        float y = center.y + radius * std::sin(angle);
        Vec3 tp = xform.transform_point({x, y, 0.0f});
        append_vertex(batch.vertex_data, tp.x, tp.y);
    }
}

void RenderBatcher::tessellate_arc_adaptive(const Vec3& center, float radius,
                                             float start_angle, float end_angle,
                                             RenderBatch& batch,
                                             const Matrix4x4& xform) {
    // Recursive midpoint subdivision for high screen-space arcs.
    // Chord-height error (sagitta) = R * (1 - cos(half_angle)).
    // Subdivide until sagitta * ppu < pixel tolerance.
    const float kPixelTolerance = 0.5f / std::max(1.0f, m_tessellation_quality);
    constexpr int kMaxRecursionDepth = 12;

    float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;

    // Lambda for recursive subdivision
    std::function<void(float, float, int)> subdivide =
        [&](float a0, float a1, int depth) {
        float half_angle = (a1 - a0) * 0.5f;
        float sagitta = radius * (1.0f - std::cos(half_angle));
        float sagitta_px = sagitta * ppu;

        if (sagitta_px <= kPixelTolerance || depth >= kMaxRecursionDepth) {
            // Error is acceptable — add the endpoint
            float x = center.x + radius * std::cos(a1);
            float y = center.y + radius * std::sin(a1);
            Vec3 tp = xform.transform_point({x, y, 0.0f});
            append_vertex(batch.vertex_data, tp.x, tp.y);
            return;
        }

        // Subdivide at midpoint
        float mid = (a0 + a1) * 0.5f;
        subdivide(a0, mid, depth + 1);
        subdivide(mid, a1, depth + 1);
    };

    // Add the start point
    {
        float x = center.x + radius * std::cos(start_angle);
        float y = center.y + radius * std::sin(start_angle);
        Vec3 tp = xform.transform_point({x, y, 0.0f});
        append_vertex(batch.vertex_data, tp.x, tp.y);
    }

    // Recursively subdivide from start to end
    subdivide(start_angle, end_angle, 0);
}

} // namespace cad
