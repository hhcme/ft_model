#pragma once

#include "cad/cad_types.h"
#include <cstdint>
#include <vector>

namespace cad {

// ============================================================
// Command types for the Flutter bridge render buffer
// ============================================================
enum class CommandType : uint32_t {
    SetTransform = 0,
    SetColor,
    SetLineWidth,
    SetLineDash,
    DrawLines,
    DrawTriangles,
    DrawText,
    DrawPath,
};

// ============================================================
// Command header prepended to each command in the buffer
// ============================================================
struct CommandHeader {
    CommandType type;
    uint32_t data_size;
};

// ============================================================
// RenderCommandBuffer: flat byte buffer consumed by Flutter
// ============================================================
class RenderCommandBuffer {
public:
    RenderCommandBuffer();

    void clear();

    // -- State commands --
    void set_transform(const Matrix4x4& matrix);
    void set_color(const Color& color);
    void set_line_width(float width);
    void set_line_dash(const float* dash_array, int count);

    // -- Draw commands --
    void draw_lines(const Vec2* vertices, int vertex_count);
    void draw_triangles(const Vec2* vertices, int vertex_count);
    void draw_text(const Vec2& position, const char* text, float size);

    // -- Buffer access --
    const uint8_t* data() const { return m_buffer.data(); }
    size_t size() const { return m_buffer.size(); }

private:
    void write_header(CommandType type, uint32_t data_size);
    void write_data(const void* data, size_t size);

    std::vector<uint8_t> m_buffer;
};

} // namespace cad
