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
#include <limits>
#include <unordered_map>

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
    m_block_cache.clear();  // Tessellate each block definition once per frame
    m_tessellating_blocks.clear();
    m_insert_vertex_count = 0;
}

void RenderBatcher::submit_entity(const EntityVariant& entity, const SceneGraph& scene) {
    // Skip block child entities — they are only rendered through INSERT expansion
    if (entity.header.in_block) return;
    submit_entity_impl(entity, scene, Matrix4x4::identity(), 0);
}

void RenderBatcher::submit_entity_impl(const EntityVariant& entity, const SceneGraph& scene,
                                        const Matrix4x4& xform, int depth) {
    if (!entity.is_visible()) return;
    if (depth > 16) return; // prevent infinite recursion

    // Resolve color: true_color (RGB) > entity ACI override > layer color
    Color draw_color;
    if (entity.header.has_true_color) {
        draw_color = entity.header.true_color;
    } else if (entity.header.color_override != 256 && entity.header.color_override != 0) {
        draw_color = Color::from_aci(entity.header.color_override);
    } else {
        int32_t li = entity.header.layer_index;
        const auto& layers = scene.layers();
        if (li >= 0 && static_cast<size_t>(li) < layers.size()) {
            draw_color = layers[static_cast<size_t>(li)].color;
        } else {
            draw_color = Color::white();
        }
    }

    // Helper: find or create a batch with matching (topology, color).
    auto find_batch = [this](PrimitiveTopology topo, const Color& col) -> RenderBatch* {
        for (auto& b : m_batches) {
            if (b.topology == topo && b.color == col) return &b;
        }
        m_batches.push_back(RenderBatch{});
        auto& b = m_batches.back();
        b.topology = topo;
        b.color = col;
        return &b;
    };

    uint8_t entity_type_u8 = static_cast<uint8_t>(entity.header.type);
    int32_t entity_index = static_cast<int32_t>(entity.header.entity_id);
    uint16_t layer_u16 = static_cast<uint16_t>(entity.header.layer_index);

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
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        tessellate_line(line->start, line->end, *batch, xform);
        break;
    }

    case EntityType::Circle: {
        auto* circle = std::get_if<1>(&entity.data);
        if (!circle) break;
        int segments = compute_arc_segments(circle->radius);
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));
        tessellate_circle(circle->center, circle->radius, segments, *batch, xform);
        break;
    }

    case EntityType::Arc: {
        auto* arc = std::get_if<2>(&entity.data);
        if (!arc) break;
        float arc_angle = arc->end_angle - arc->start_angle;
        float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
        int segments = LodSelector::compute_arc_segments(arc->radius, arc_angle, ppu * m_tessellation_quality);
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));
        tessellate_arc(arc->center, arc->radius, arc->start_angle, arc->end_angle, segments, *batch, xform);
        break;
    }

    case EntityType::LwPolyline: {
        auto* poly = std::get_if<4>(&entity.data);
        if (!poly) break;
        const auto& vb = scene.vertex_buffer();
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        int32_t count = poly->vertex_count;
        int32_t offset = poly->vertex_offset;
        if (count < 2) break;

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

            m_block_cache[bk_idx] = {std::move(m_batches), total_verts, cdist, cx, cy};
            m_tessellating_blocks.erase(bk_idx);
            m_batches.swap(local_batches);
            cache_it = m_block_cache.find(bk_idx);
        }

        if (cache_it->second.vertex_count > MAX_BLOCK_VERTICES) break;
        if (m_insert_vertex_count >= m_insert_vertex_budget) break;

        const auto& cached_batches = cache_it->second.batches;
        double cx = cache_it->second.centroid_x;
        double cy = cache_it->second.centroid_y;
        double centroid_dist = cache_it->second.centroid_dist;

        // World-space DWG blocks with many entities are usually already emitted
        // directly; expanding them through INSERT creates duplicate/huge geometry.
        if (centroid_dist > 5000.0 && block.entity_indices.size() > 50) break;

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

                for (const RenderBatch& src : cached_batches) {
                    RenderBatch* dst = find_batch(src.topology, src.color);
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
        const auto& vb = scene.vertex_buffer();
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        int32_t count = poly->vertex_count;
        int32_t offset = poly->vertex_offset;
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
        if (!hatch || !hatch->is_solid || hatch->loops.empty()) break;

        auto* batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));

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
        break;
    }

    // Solid — filled 3/4 vertex polygon
    case EntityType::Solid: {
        auto* solid = std::get_if<16>(&entity.data);
        if (!solid || solid->corner_count < 3) break;

        auto* batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));

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

    // Ellipse — tessellate as circle
    case EntityType::Ellipse: {
        auto* ellipse = std::get_if<12>(&entity.data);
        if (!ellipse) break;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
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

        int segments = compute_arc_segments(std::max(major_r, minor_r));
        float span = end_a - start_a;

        float cos_r = std::cos(rotation);
        float sin_r = std::sin(rotation);
        float cx = ellipse->center.x;
        float cy = ellipse->center.y;

        for (int i = 0; i <= segments; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(segments);
            float angle = start_a + t * span;
            // Parametric ellipse point (before rotation)
            float px = major_r * std::cos(angle);
            float py = minor_r * std::sin(angle);
            // Apply rotation and translate
            float x = px * cos_r - py * sin_r + cx;
            float y = px * sin_r + py * cos_r + cy;
            // Apply entity transform (INSERT etc.)
            auto [tx_x, tx_y] = tx(x, y);
            append_vertex(batch->vertex_data, tx_x, tx_y);
        }
        break;
    }

    // Spline — tessellate from fit points or control points
    case EntityType::Spline: {
        auto* spline = std::get_if<5>(&entity.data);
        if (!spline) break;

        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        batch->entity_starts.push_back(static_cast<uint32_t>(batch->vertex_data.size() / 2));

        const auto& pts = spline->fit_points.empty() ? spline->control_points : spline->fit_points;
        for (const auto& pt : pts) {
            auto [tpx, tpy] = tx(pt.x, pt.y);
            append_vertex(batch->vertex_data, tpx, tpy);
        }
        break;
    }

    // Text — render a thin underline indicator; actual text is emitted separately.
    case EntityType::Text:
    case EntityType::MText: {
        auto* text = std::get_if<6>(&entity.data);
        if (!text) text = std::get_if<7>(&entity.data);
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
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        append_vertex(batch->vertex_data, rx0, ry0);
        append_vertex(batch->vertex_data, rx1, ry1);
        break;
    }

    // Point — skip, no useful geometry
    case EntityType::Point:
        break;

    // Dimension — render as dimension lines (extension lines + dimension line)
    case EntityType::Dimension: {
        auto* dim = std::get_if<8>(&entity.data);
        if (!dim) break;

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));

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

    case EntityType::Ray:
    case EntityType::XLine:
    case EntityType::Viewport:
        break;
    }
}

void RenderBatcher::end_frame() {
    // Sort batches by render key for correct ordering
    std::sort(m_batches.begin(), m_batches.end(),
              [](const RenderBatch& a, const RenderBatch& b) {
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
            float threshold = std::max(lengths[lengths.size() / 2] * 20.0f, 1.0f);

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
            float threshold = std::max(extents[extents.size() / 2] * 20.0f, 1.0f);

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
            float threshold = std::max(extents[extents.size() / 2] * 20.0f, 1.0f);

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

} // namespace cad
