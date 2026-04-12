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

namespace cad {

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
}

void RenderBatcher::submit_entity(const EntityVariant& entity, const SceneGraph& scene) {
    submit_entity_impl(entity, scene, Matrix4x4::identity(), 0);
}

void RenderBatcher::submit_entity_impl(const EntityVariant& entity, const SceneGraph& scene,
                                        const Matrix4x4& xform, int depth) {
    if (!entity.is_visible()) return;
    if (depth > 16) return; // prevent infinite recursion

    // Resolve color: if color_override != 256 and != 0, use ACI color;
    // otherwise use the layer color.
    Color draw_color;
    if (entity.header.color_override != 256 && entity.header.color_override != 0) {
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
                batch->vertex_data.push_back(tx0);
                batch->vertex_data.push_back(ty0);
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
                    batch->vertex_data.push_back(tax);
                    batch->vertex_data.push_back(tay);
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
                    batch->vertex_data.push_back(tlx);
                    batch->vertex_data.push_back(tly);
                }
            }
        }
        break;
    }

    case EntityType::Insert: {
        auto* ins = std::get_if<10>(&entity.data);
        if (!ins) break;
        const auto& blocks = scene.blocks();
        if (ins->block_index < 0 ||
            static_cast<size_t>(ins->block_index) >= blocks.size()) break;

        const auto& block = blocks[static_cast<size_t>(ins->block_index)];
        const auto& all_entities = scene.entities();

        // Build the transform for this insert
        Matrix4x4 insert_xform = Matrix4x4::affine_2d(
            ins->x_scale, ins->y_scale, ins->rotation,
            ins->insertion_point.x, ins->insertion_point.y);

        for (int32_t col = 0; col < ins->column_count; ++col) {
            for (int32_t row = 0; row < ins->row_count; ++row) {
                Vec3 offset(
                    static_cast<float>(col) * ins->column_spacing,
                    static_cast<float>(row) * ins->row_spacing, 0.0f);
                Matrix4x4 block_xform = Matrix4x4::translation_2d(offset.x, offset.y) * insert_xform;
                // Compose: parent xform * block_xform
                Matrix4x4 final_xform = xform * block_xform;

                for (int32_t ei : block.entity_indices) {
                    if (ei < 0 || static_cast<size_t>(ei) >= all_entities.size()) continue;
                    const auto& child = all_entities[static_cast<size_t>(ei)];
                    if (!child.is_visible()) continue;
                    submit_entity_impl(child, scene, final_xform, depth + 1);
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
            batch->vertex_data.push_back(tpx);
            batch->vertex_data.push_back(tpy);
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
                batch->vertex_data.push_back(x0);
                batch->vertex_data.push_back(y0);
                batch->vertex_data.push_back(xi);
                batch->vertex_data.push_back(yi);
                batch->vertex_data.push_back(xip);
                batch->vertex_data.push_back(yip);
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
            batch->vertex_data.push_back(x0); batch->vertex_data.push_back(y0);
            batch->vertex_data.push_back(x1); batch->vertex_data.push_back(y1);
            batch->vertex_data.push_back(x2); batch->vertex_data.push_back(y2);
        } else {
            auto [x3, y3] = tx(solid->corners[3].x, solid->corners[3].y);
            // DXF order: 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3
            batch->vertex_data.push_back(x0); batch->vertex_data.push_back(y0);
            batch->vertex_data.push_back(x1); batch->vertex_data.push_back(y1);
            batch->vertex_data.push_back(x3); batch->vertex_data.push_back(y3);

            batch->vertex_data.push_back(x1); batch->vertex_data.push_back(y1);
            batch->vertex_data.push_back(x2); batch->vertex_data.push_back(y2);
            batch->vertex_data.push_back(x3); batch->vertex_data.push_back(y3);
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
            batch->vertex_data.push_back(tx_x);
            batch->vertex_data.push_back(tx_y);
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
            batch->vertex_data.push_back(tpx);
            batch->vertex_data.push_back(tpy);
        }
        break;
    }

    // Text — render as placeholder rectangle
    case EntityType::Text:
    case EntityType::MText: {
        auto* text = std::get_if<6>(&entity.data);
        if (!text) text = std::get_if<7>(&entity.data);
        if (!text || text->height <= 0.0f) break;

        float hw = text->height * text->text.size() * text->width_factor * 0.3f;
        float hh = text->height;
        float x = text->insertion_point.x;
        float y = text->insertion_point.y;

        auto [rx0, ry0] = tx(x, y);
        auto [rx1, ry1] = tx(x + hw, y + hh);

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        // Four edges of the text box
        float verts[] = { rx0,ry0, rx1,ry0,  rx1,ry0, rx1,ry1,  rx1,ry1, rx0,ry1,  rx0,ry1, rx0,ry0 };
        for (float v : verts) batch->vertex_data.push_back(v);
        break;
    }

    // Point — render as small cross
    case EntityType::Point: {
        auto* pt = std::get_if<11>(&entity.data);
        if (!pt) break;

        auto [px, py] = tx(pt->x, pt->y);
        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        float s = 2.0f;
        batch->vertex_data.push_back(px - s); batch->vertex_data.push_back(py);
        batch->vertex_data.push_back(px + s); batch->vertex_data.push_back(py);
        batch->vertex_data.push_back(px); batch->vertex_data.push_back(py - s);
        batch->vertex_data.push_back(px); batch->vertex_data.push_back(py + s);
        break;
    }

    // Skip truly unimplemented types
    case EntityType::Dimension:
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
}

void RenderBatcher::tessellate_line(const Vec3& p0, const Vec3& p1,
                                     RenderBatch& batch, const Matrix4x4& xform) {
    Vec3 tp0 = xform.transform_point(p0);
    Vec3 tp1 = xform.transform_point(p1);
    batch.vertex_data.push_back(tp0.x);
    batch.vertex_data.push_back(tp0.y);
    batch.vertex_data.push_back(tp1.x);
    batch.vertex_data.push_back(tp1.y);
}

void RenderBatcher::tessellate_circle(const Vec3& center, float radius,
                                       int segments, RenderBatch& batch,
                                       const Matrix4x4& xform) {
    for (int i = 0; i <= segments; ++i) {
        float angle = static_cast<float>(i) / static_cast<float>(segments) * math::TWO_PI;
        float x = center.x + radius * std::cos(angle);
        float y = center.y + radius * std::sin(angle);
        Vec3 tp = xform.transform_point({x, y, 0.0f});
        batch.vertex_data.push_back(tp.x);
        batch.vertex_data.push_back(tp.y);
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
        batch.vertex_data.push_back(tp.x);
        batch.vertex_data.push_back(tp.y);
    }
}

} // namespace cad
