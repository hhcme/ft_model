#include "cad/renderer/render_command.h"
#include <cstring>

namespace cad {

RenderCommandBuffer::RenderCommandBuffer() {
    // Reserve a reasonable initial capacity
    m_buffer.reserve(4096);
}

void RenderCommandBuffer::clear() {
    m_buffer.clear();
}

void RenderCommandBuffer::set_transform(const Matrix4x4& matrix) {
    write_header(CommandType::SetTransform, sizeof(Matrix4x4));
    write_data(matrix.data(), sizeof(float) * 16);
}

void RenderCommandBuffer::set_color(const Color& color) {
    write_header(CommandType::SetColor, sizeof(Color));
    write_data(&color, sizeof(Color));
}

void RenderCommandBuffer::set_line_width(float width) {
    write_header(CommandType::SetLineWidth, sizeof(float));
    write_data(&width, sizeof(float));
}

void RenderCommandBuffer::set_line_dash(const float* dash_array, int count) {
    uint32_t data_size = sizeof(int) + static_cast<uint32_t>(count) * sizeof(float);
    write_header(CommandType::SetLineDash, data_size);
    write_data(&count, sizeof(int));
    if (count > 0 && dash_array) {
        write_data(dash_array, static_cast<size_t>(count) * sizeof(float));
    }
}

void RenderCommandBuffer::draw_lines(const Vec2* vertices, int vertex_count) {
    uint32_t data_size = sizeof(int) + static_cast<uint32_t>(vertex_count) * sizeof(Vec2);
    write_header(CommandType::DrawLines, data_size);
    write_data(&vertex_count, sizeof(int));
    if (vertex_count > 0 && vertices) {
        write_data(vertices, static_cast<size_t>(vertex_count) * sizeof(Vec2));
    }
}

void RenderCommandBuffer::draw_triangles(const Vec2* vertices, int vertex_count) {
    uint32_t data_size = sizeof(int) + static_cast<uint32_t>(vertex_count) * sizeof(Vec2);
    write_header(CommandType::DrawTriangles, data_size);
    write_data(&vertex_count, sizeof(int));
    if (vertex_count > 0 && vertices) {
        write_data(vertices, static_cast<size_t>(vertex_count) * sizeof(Vec2));
    }
}

void RenderCommandBuffer::draw_text(const Vec2& position, const char* text, float size) {
    uint32_t text_len = text ? static_cast<uint32_t>(std::strlen(text)) : 0;
    uint32_t data_size = sizeof(Vec2) + sizeof(float) + sizeof(uint32_t) + text_len;
    write_header(CommandType::DrawText, data_size);
    write_data(&position, sizeof(Vec2));
    write_data(&size, sizeof(float));
    write_data(&text_len, sizeof(uint32_t));
    if (text_len > 0 && text) {
        write_data(text, text_len);
    }
}

void RenderCommandBuffer::write_header(CommandType type, uint32_t data_size) {
    CommandHeader header;
    header.type = type;
    header.data_size = data_size;
    write_data(&header, sizeof(CommandHeader));
}

void RenderCommandBuffer::write_data(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    m_buffer.insert(m_buffer.end(), bytes, bytes + size);
}

} // namespace cad
