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
    if (!entity.is_visible()) return;

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

    switch (entity.header.type) {

    case EntityType::Line: {
        auto* line = std::get_if<0>(&entity.data);
        if (!line) break;
        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        tessellate_line(line->start, line->end, *batch);
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
        tessellate_circle(circle->center, circle->radius, segments, *batch);
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
        tessellate_arc(arc->center, arc->radius, arc->start_angle, arc->end_angle, segments, *batch);
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
                // Straight segment
                batch->vertex_data.push_back(p0.x);
                batch->vertex_data.push_back(p0.y);
            } else {
                // Bulge arc: bulge = tan(theta/4), where theta is the sweep angle.
                // Positive bulge = arc to the left of p0->p1.
                float theta = 4.0f * std::atan(std::abs(bulge));
                float dx = p1.x - p0.x;
                float dy = p1.y - p0.y;
                float chord_len = std::sqrt(dx * dx + dy * dy);
                if (chord_len < 1e-12f) continue;

                float radius = chord_len / (2.0f * std::sin(theta * 0.5f));
                // Perpendicular direction from chord midpoint
                float mx = (p0.x + p1.x) * 0.5f;
                float my = (p0.y + p1.y) * 0.5f;
                // Normalized perpendicular to chord direction
                float nx = -dy / chord_len;
                float ny = dx / chord_len;
                // Signed distance from midpoint to center
                float s = (bulge > 0) ? 1.0f : -1.0f;
                float dist = radius * std::cos(theta * 0.5f);
                float cx = mx + s * dist * nx;
                float cy = my + s * dist * ny;

                float start_a = std::atan2(p0.y - cy, p0.x - cx);
                float end_a = std::atan2(p1.y - cy, p1.x - cx);
                // Ensure sweep direction matches bulge sign
                if (bulge > 0) {
                    if (end_a <= start_a) end_a += math::TWO_PI;
                } else {
                    if (end_a >= start_a) end_a -= math::TWO_PI;
                }

                float ppu = m_camera ? m_camera->pixels_per_unit() : 1.0f;
                int segs = LodSelector::compute_arc_segments(radius, std::abs(end_a - start_a),
                                                              ppu * m_tessellation_quality);
                segs = std::max(segs, 4);

                // Generate arc points, skipping the last if this is not the final segment
                // (the next segment will start with its own point)
                int limit = (i < count - 2) ? segs : segs + 1;
                for (int j = 0; j < limit; ++j) {
                    float t = static_cast<float>(j) / static_cast<float>(segs);
                    float a = start_a + t * (end_a - start_a);
                    batch->vertex_data.push_back(cx + radius * std::cos(a));
                    batch->vertex_data.push_back(cy + radius * std::sin(a));
                }
            }
        }
        // Add the last vertex if it wasn't added yet (for straight segments)
        {
            int32_t last_idx = offset + count - 1;
            if (last_idx >= 0 && static_cast<size_t>(last_idx) < vb.size()) {
                Vec3 last_pt = vb[static_cast<size_t>(last_idx)];
                float last_bulge = (static_cast<size_t>(count - 2) < poly->bulges.size())
                                       ? poly->bulges[static_cast<size_t>(count - 2)] : 0.0f;
                if (std::abs(last_bulge) < 1e-6f) {
                    batch->vertex_data.push_back(last_pt.x);
                    batch->vertex_data.push_back(last_pt.y);
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
        Matrix4x4 xform = Matrix4x4::affine_2d(
            ins->x_scale, ins->y_scale, ins->rotation,
            ins->insertion_point.x, ins->insertion_point.y);

        for (int32_t col = 0; col < ins->column_count; ++col) {
            for (int32_t row = 0; row < ins->row_count; ++row) {
                Vec3 offset(
                    static_cast<float>(col) * ins->column_spacing,
                    static_cast<float>(row) * ins->row_spacing, 0.0f);
                Matrix4x4 block_xform = Matrix4x4::translation_2d(offset.x, offset.y) * xform;

                for (int32_t ei : block.entity_indices) {
                    if (ei < 0 || static_cast<size_t>(ei) >= all_entities.size()) continue;
                    const auto& child = all_entities[static_cast<size_t>(ei)];
                    if (!child.is_visible()) continue;
                    // Recursively submit block entities
                    submit_entity(child, scene);
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

        int32_t count = poly->vertex_count;
        int32_t offset = poly->vertex_offset;
        for (int32_t i = 0; i < count; ++i) {
            int32_t idx = offset + i;
            if (idx < 0 || static_cast<size_t>(idx) >= vb.size()) continue;
            Vec3 pt = vb[static_cast<size_t>(idx)];
            batch->vertex_data.push_back(pt.x);
            batch->vertex_data.push_back(pt.y);
        }
        break;
    }

    // Hatch — solid fill polygon
    case EntityType::Hatch: {
        auto* hatch = std::get_if<9>(&entity.data);
        if (!hatch || !hatch->is_solid || hatch->loops.empty()) break;

        // Find or create a TriangleList batch for filled polygons
        auto* batch = find_batch(PrimitiveTopology::TriangleList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::TriangleList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));

        // For each boundary loop, triangulate using fan triangulation
        for (const auto& loop : hatch->loops) {
            if (loop.vertices.size() < 3) continue;
            // Fan triangulation: v0-v1-v2, v0-v2-v3, v0-v3-v4, ...
            for (size_t i = 1; i + 1 < loop.vertices.size(); ++i) {
                batch->vertex_data.push_back(loop.vertices[0].x);
                batch->vertex_data.push_back(loop.vertices[0].y);
                batch->vertex_data.push_back(loop.vertices[i].x);
                batch->vertex_data.push_back(loop.vertices[i].y);
                batch->vertex_data.push_back(loop.vertices[i + 1].x);
                batch->vertex_data.push_back(loop.vertices[i + 1].y);
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

        // DXF SOLID vertex order: 1,2,4,3 (not 1,2,3,4)
        // Triangulate as: v0-v1-v3 and v1-v2-v3
        if (solid->corner_count == 3) {
            // Triangle: v0-v1-v2
            batch->vertex_data.push_back(solid->corners[0].x);
            batch->vertex_data.push_back(solid->corners[0].y);
            batch->vertex_data.push_back(solid->corners[1].x);
            batch->vertex_data.push_back(solid->corners[1].y);
            batch->vertex_data.push_back(solid->corners[2].x);
            batch->vertex_data.push_back(solid->corners[2].y);
        } else {
            // 4 vertices: DXF order is 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3
            batch->vertex_data.push_back(solid->corners[0].x);
            batch->vertex_data.push_back(solid->corners[0].y);
            batch->vertex_data.push_back(solid->corners[1].x);
            batch->vertex_data.push_back(solid->corners[1].y);
            batch->vertex_data.push_back(solid->corners[3].x);
            batch->vertex_data.push_back(solid->corners[3].y);

            batch->vertex_data.push_back(solid->corners[1].x);
            batch->vertex_data.push_back(solid->corners[1].y);
            batch->vertex_data.push_back(solid->corners[2].x);
            batch->vertex_data.push_back(solid->corners[2].y);
            batch->vertex_data.push_back(solid->corners[3].x);
            batch->vertex_data.push_back(solid->corners[3].y);
        }
        break;
    }

    // Ellipse — tessellate as arc
    case EntityType::Ellipse: {
        auto* ellipse = std::get_if<12>(&entity.data);
        if (!ellipse) break;
        // Simplified: treat as circle for now
        int segments = compute_arc_segments(ellipse->radius);
        auto* batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        tessellate_circle(ellipse->center, ellipse->radius, segments, *batch);
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

        // Use fit points if available, otherwise control points
        const auto& pts = spline->fit_points.empty() ? spline->control_points : spline->fit_points;
        for (const auto& pt : pts) {
            batch->vertex_data.push_back(pt.x);
            batch->vertex_data.push_back(pt.y);
        }
        break;
    }

    // Text — render as placeholder rectangle
    case EntityType::Text:
    case EntityType::MText: {
        auto* text = std::get_if<6>(&entity.data);
        if (!text) text = std::get_if<7>(&entity.data);
        if (!text || text->height <= 0.0f) break;

        // Draw a placeholder rectangle showing the text bounds
        float hw = text->height * text->text.size() * text->width_factor * 0.3f; // rough width estimate
        float hh = text->height;
        float x = text->insertion_point.x;
        float y = text->insertion_point.y;

        // Simple axis-aligned rectangle
        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        // Four edges of the text box
        float x0 = x, y0 = y, x1 = x + hw, y1 = y + hh;
        float verts[] = { x0,y0, x1,y0,  x1,y0, x1,y1,  x1,y1, x0,y1,  x0,y1, x0,y0 };
        for (float v : verts) batch->vertex_data.push_back(v);
        break;
    }

    // Point — render as small cross
    case EntityType::Point: {
        auto* pt = std::get_if<11>(&entity.data);
        if (!pt) break;

        auto* batch = find_batch(PrimitiveTopology::LineList, draw_color);
        batch->sort_key = RenderKey::make(layer_u16,
            static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
            static_cast<uint32_t>(entity_index & 0x00FFFFFF));
        float s = 2.0f; // cross half-size
        batch->vertex_data.push_back(pt->x - s); batch->vertex_data.push_back(pt->y);
        batch->vertex_data.push_back(pt->x + s); batch->vertex_data.push_back(pt->y);
        batch->vertex_data.push_back(pt->x); batch->vertex_data.push_back(pt->y - s);
        batch->vertex_data.push_back(pt->x); batch->vertex_data.push_back(pt->y + s);
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

void RenderBatcher::tessellate_line(const Vec3& p0, const Vec3& p1, RenderBatch& batch) {
    batch.vertex_data.push_back(p0.x);
    batch.vertex_data.push_back(p0.y);
    batch.vertex_data.push_back(p1.x);
    batch.vertex_data.push_back(p1.y);
}

void RenderBatcher::tessellate_circle(const Vec3& center, float radius,
                                       int segments, RenderBatch& batch) {
    for (int i = 0; i <= segments; ++i) {
        float angle = static_cast<float>(i) / static_cast<float>(segments) * math::TWO_PI;
        float x = center.x + radius * std::cos(angle);
        float y = center.y + radius * std::sin(angle);
        batch.vertex_data.push_back(x);
        batch.vertex_data.push_back(y);
    }
}

void RenderBatcher::tessellate_arc(const Vec3& center, float radius,
                                    float start_angle, float end_angle,
                                    int segments, RenderBatch& batch) {
    float arc_span = end_angle - start_angle;
    for (int i = 0; i <= segments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(segments);
        float angle = start_angle + t * arc_span;
        float x = center.x + radius * std::cos(angle);
        float y = center.y + radius * std::sin(angle);
        batch.vertex_data.push_back(x);
        batch.vertex_data.push_back(y);
    }
}

} // namespace cad
