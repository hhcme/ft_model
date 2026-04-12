#pragma once

#include "cad/cad_types.h"
#include <cstdint>
#include <vector>

namespace cad {

class Camera;

// Forward declarations for scene types
struct EntityVariant;
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

    const std::vector<RenderBatch>& batches() const { return m_batches; }
    std::vector<RenderBatch>& batches() { return m_batches; }

private:
    // Internal submit with optional transform (for INSERT block entities)
    void submit_entity_impl(const EntityVariant& entity, const SceneGraph& scene,
                            const Matrix4x4& xform, int depth);

    // Tessellation helpers — transform applied to output vertices
    void tessellate_line(const Vec3& p0, const Vec3& p1, RenderBatch& batch,
                         const Matrix4x4& xform);
    void tessellate_circle(const Vec3& center, float radius, int segments,
                           RenderBatch& batch, const Matrix4x4& xform);
    void tessellate_arc(const Vec3& center, float radius, float start_angle,
                        float end_angle, int segments, RenderBatch& batch,
                        const Matrix4x4& xform);

    float m_tessellation_quality;
    std::vector<RenderBatch> m_batches;
    const Camera* m_camera = nullptr;
};

} // namespace cad
