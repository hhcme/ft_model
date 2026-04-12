#pragma once

#include <cstdint>
#include <cstddef>

namespace cad {
namespace gfx {

// ============================================================
// Buffer usage hint for the GPU driver
// ============================================================
enum class BufferUsage : uint8_t {
    Vertex,
    Index,
    Uniform,
};

// ============================================================
// Primitive topology for draw calls
// ============================================================
enum class PrimitiveTopology : uint8_t {
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

// ============================================================
// Vertex format description
// ============================================================
enum class VertexFormat : uint8_t {
    Pos2f,                  // { float x, y }
    Pos2f_Color4ub,         // { float x, y; uint8_t r, g, b, a }
    Pos2f_Color4ub_TexCoord2f, // { float x, y; uint8_t r, g, b, a; float u, v }
};

// ============================================================
// Buffer description for create_buffer
// ============================================================
struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    size_t size = 0;
    const void* initial_data = nullptr;
    VertexFormat format = VertexFormat::Pos2f; // Hint for vertex buffers
};

// ============================================================
// Pipeline description for create_pipeline
// ============================================================
struct PipelineDesc {
    PrimitiveTopology topology = PrimitiveTopology::LineList;
    VertexFormat vertex_format = VertexFormat::Pos2f;

    // 3D reserved defaults (disabled for 2D)
    bool depth_test = false;
    bool depth_write = false;
    bool backface_culling = false;
};

} // namespace gfx
} // namespace cad
