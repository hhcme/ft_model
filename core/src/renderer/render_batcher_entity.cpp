// render_batcher_entity.cpp — Entity type dispatch for submit_entity_impl.
// Extracted from render_batcher.cpp for file size compliance.

#include "cad/renderer/render_batcher.h"
#include "cad/renderer/camera.h"
#include "cad/renderer/lod_selector.h"
#include "render_batcher_internal.h"
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
using namespace batcher;

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
    // Apply entity-specific linetype scale
    float ltscale = entity.header.linetype_scale;
    if (ltscale != 1.0f && ltscale > 0.0f && !line_pattern.is_solid()) {
        for (auto& d : line_pattern.dash_array) {
            d *= ltscale;
        }
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
        // Propagate semantic and modifiers from entity header
        b.semantic = static_cast<uint8_t>(entity.header.semantic);
        b.modifiers = entity.header.modifiers;
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
        submit_insert(entity, scene, xform, depth);
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

    case EntityType::Multileader: {
        auto* ml = std::get_if<20>(&entity.data);
        if (!ml) break;
        // Render text at insertion point
        if (!ml->text.empty() && ml->text_height > 0.0f) {
            auto* text_batch = find_batch(PrimitiveTopology::LineList, draw_color);
            text_batch->sort_key = RenderKey::make(layer_u16,
                static_cast<uint8_t>(PrimitiveTopology::LineList), 0, entity_type_u8,
                depth_order);
            auto [px, py] = tx(ml->insertion_point.x, ml->insertion_point.y);
            float h = ml->text_height;
            // Underline bar for text placement indicator
            append_vertex(text_batch->vertex_data, px, py - h * 0.5f);
            append_vertex(text_batch->vertex_data, px + static_cast<float>(ml->text.size()) * h * 0.6f, py - h * 0.5f);
        }
        // Render leader line vertices if present
        if (ml->vertex_count >= 2 && ml->vertex_offset >= 0) {
            const auto& vb = scene.vertex_buffer();
            if (static_cast<size_t>(ml->vertex_offset + ml->vertex_count) <= vb.size()) {
                auto* line_batch = find_batch(PrimitiveTopology::LineStrip, draw_color);
                line_batch->sort_key = RenderKey::make(layer_u16,
                    static_cast<uint8_t>(PrimitiveTopology::LineStrip), 0, entity_type_u8,
                    depth_order);
                line_batch->entity_starts.push_back(static_cast<uint32_t>(line_batch->vertex_data.size() / 2));
                const Vec3* pts = vb.data() + ml->vertex_offset;
                for (int32_t i = 0; i < ml->vertex_count; ++i) {
                    auto [px, py] = tx(pts[i].x, pts[i].y);
                    append_vertex(line_batch->vertex_data, px, py);
                }
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

} // namespace cad
