#pragma once

#include "cad/cad_types.h"
#include "cad/scene/entity.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cad {

class Camera;

// Forward declarations for scene types
class SceneGraph;

// ============================================================
// Primitive topology (own enum — decoupled from gfx backend)
// ============================================================
enum class PrimitiveTopology {
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

// ============================================================
// RenderKey: 64-bit sort key for batch ordering
// ============================================================
struct RenderKey {
    uint64_t key = 0;

    // Bit layout (MSB to LSB):
    // [63:48] layer          (16 bits)
    // [47:40] primitive type (8 bits)
    // [39:32] line type      (8 bits)
    // [31:24] entity type    (8 bits)
    // [23:0]  depth/order    (24 bits)

    static RenderKey make(uint16_t layer, uint8_t primitive, uint8_t linetype,
                          uint8_t entity_type, uint32_t depth_order);

    bool operator<(const RenderKey& rhs) const { return key < rhs.key; }
    bool operator==(const RenderKey& rhs) const { return key == rhs.key; }
};

// ============================================================
// RenderBatch: a single draw call's worth of geometry
// ============================================================
struct RenderBatch {
    RenderKey sort_key;
    PrimitiveTopology topology = PrimitiveTopology::LineList;
    void* pipeline = nullptr; // GfxPipeline*, void* to avoid full include
    std::vector<float> vertex_data;
    std::vector<uint32_t> index_data;
    // For LineStrip topology: records the vertex index (into vertex_data/2)
    // where each entity starts, so the renderer can break the path.
    std::vector<uint32_t> entity_starts;
    Matrix4x4 transform_matrix;
    LinePattern line_pattern;
    Color color;
    float line_width = 1.0f;
    DrawingSpace space = DrawingSpace::Unknown;
    int32_t layout_index = -1;
    int32_t viewport_index = -1;
    uint32_t draw_order = 0;
};

// ============================================================
// RenderBatcher: collects entities into sorted batches
// ============================================================
class RenderBatcher {
public:
    RenderBatcher();

    void set_tessellation_quality(float quality);
    int compute_arc_segments(float radius) const;

    void begin_frame(const Camera& camera);
    void submit_entity(const EntityVariant& entity, const SceneGraph& scene);
    void end_frame();

    void set_outlier_filter_enabled(bool enabled) { m_outlier_filter_enabled = enabled; }

    const std::vector<RenderBatch>& batches() const { return m_batches; }
    std::vector<RenderBatch>& batches() { return m_batches; }

    // Cache: tessellate each unique block definition once, reuse for all INSERTs.
    void clear_block_cache() { m_block_cache.clear(); }

    // Global INSERT vertex budget: stop expanding INSERTs once total exceeds this.
    void set_insert_vertex_budget(size_t budget) { m_insert_vertex_budget = budget; }
    size_t insert_vertex_count() const { return m_insert_vertex_count; }

private:
    // Internal submit with optional transform (for INSERT block entities)
    void submit_entity_impl(const EntityVariant& entity, const SceneGraph& scene,
                            const Matrix4x4& xform, int depth,
                            const EntityHeader* inherited_header = nullptr);

    // Tessellation helpers — transform applied to output vertices
    void tessellate_line(const Vec3& p0, const Vec3& p1, RenderBatch& batch,
                         const Matrix4x4& xform);
    void tessellate_circle(const Vec3& center, float radius, int segments,
                           RenderBatch& batch, const Matrix4x4& xform);
    void tessellate_arc(const Vec3& center, float radius, float start_angle,
                        float end_angle, int segments, RenderBatch& batch,
                        const Matrix4x4& xform);

    // Adaptive arc tessellation using recursive midpoint subdivision.
    // Used for arcs with high screen-space size (R * ppu > threshold).
    // Recursively subdivides until chord-height error < pixel tolerance.
    void tessellate_arc_adaptive(const Vec3& center, float radius,
                                 float start_angle, float end_angle,
                                 RenderBatch& batch, const Matrix4x4& xform);

    float m_tessellation_quality;
    std::vector<RenderBatch> m_batches;
    const Camera* m_camera = nullptr;
    // Block tessellation cache: block_index → pre-tessellated batches (identity transform)
    struct BlockCacheEntry {
        std::vector<RenderBatch> batches;
        size_t vertex_count = 0;
        double centroid_dist = 0; // distance of geometry centroid from origin
        double centroid_x = 0;   // centroid X (for world-space block base_point)
        double centroid_y = 0;   // centroid Y
    };
    std::unordered_map<int32_t, BlockCacheEntry> m_block_cache;
    // Cycle detection: blocks currently being tessellated (nested INSERT chain)
    std::unordered_set<int32_t> m_tessellating_blocks;
    // Global INSERT vertex budget and running counter
    size_t m_insert_vertex_budget = 200000000; // 200M vertices default
    size_t m_insert_vertex_count = 0;
    bool m_outlier_filter_enabled = false;
};

} // namespace cad
