// render_batcher.cpp — Core RenderBatcher class methods.
// Entity dispatch → render_batcher_entity.cpp
// Curve/math helpers → render_batcher_curve.cpp

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


void RenderBatcher::submit_insert(const EntityVariant& entity, const SceneGraph& scene,
                                   const Matrix4x4& xform, int depth) {
auto* ins = std::get_if<10>(&entity.data);
if (!ins) return;

// Sanity check: skip INSERT entities with extreme coordinates
{
    float max_coord = 1e8f;
    float ip = std::max(std::abs(ins->insertion_point.x), std::abs(ins->insertion_point.y));
    float sc = std::max(std::abs(ins->x_scale), std::abs(ins->y_scale));
    if (ip > max_coord || sc > 1e4f || (ip * sc > 1e9f)) return;
}

const auto& blocks = scene.blocks();
if (ins->block_index < 0 ||
    static_cast<size_t>(ins->block_index) >= blocks.size()) return;

const auto& block = blocks[static_cast<size_t>(ins->block_index)];
const auto& all_entities = scene.entities();

// Skip INSERTs of model/paper space (case-insensitive)
// but allow *D (dimension) and other anonymous blocks.
{
    std::string uname = block.name;
    std::transform(uname.begin(), uname.end(), uname.begin(), ::toupper);
    if (uname == "*MODEL_SPACE" || uname == "*PAPER_SPACE") return;
}

// Xref blocks: render a dashed bounding box instead of geometry
if (block.is_xref) {
    const Bounds3d& bb = block.bounds;
    if (bb.is_empty()) return;
    RenderBatch xref_batch;
    xref_batch.sort_key = RenderKey::make(
        static_cast<uint16_t>(entity.header.layer_index), 0, 0,
        static_cast<uint8_t>(entity.header.type),
        entity.header.draw_order);
    xref_batch.topology = PrimitiveTopology::LineList;
    xref_batch.color = {255, 255, 255};
    xref_batch.line_width = 1.0f;
    xref_batch.space = entity.header.space;
    xref_batch.layout_index = entity.header.layout_index;
    xref_batch.viewport_index = entity.header.viewport_index;
    xref_batch.draw_order = entity.header.draw_order;
    Vec3 corners[4] = {
        {bb.min.x, bb.min.y, 0}, {bb.max.x, bb.min.y, 0},
        {bb.max.x, bb.max.y, 0}, {bb.min.x, bb.max.y, 0},
    };
    for (int i = 0; i < 4; ++i) {
        Vec3 tp0 = xform.transform_point(corners[i]);
        Vec3 tp1 = xform.transform_point(corners[(i + 1) % 4]);
        append_vertex(xref_batch.vertex_data, tp0.x, tp0.y);
        append_vertex(xref_batch.vertex_data, tp1.x, tp1.y);
    }
    if (!xref_batch.vertex_data.empty()) {
        m_batches.push_back(std::move(xref_batch));
    }
    return;
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
    if (m_tessellating_blocks.count(bk_idx)) return;
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

if (cache_it->second.vertex_count > MAX_BLOCK_VERTICES) return;
if (m_insert_vertex_count >= m_insert_vertex_budget) return;

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
}

} // namespace cad
