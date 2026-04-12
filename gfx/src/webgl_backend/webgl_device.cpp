// WebGL backend — only compiles with real GL headers (Emscripten).
// On native builds this file produces an empty translation unit.

#ifdef __EMSCRIPTEN__

#include "webgl_device.h"
#include <cstdio>
#include <cstring>

namespace cad::gfx {

// ============================================================
// Embedded shader sources
// ============================================================
const char* k_line_vertex_shader = R"glsl(
#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
uniform mat4 u_view_proj;
void main() {
    gl_Position = u_view_proj * vec4(a_position, 0.0, 1.0);
}
)glsl";

const char* k_line_fragment_shader = R"glsl(
#version 300 es
precision highp float;
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)glsl";

// ============================================================
// Helpers
// ============================================================
static GLenum topology_to_gl(PrimitiveTopology topo) {
    switch (topo) {
        case PrimitiveTopology::LineList:      return GL_LINES;
        case PrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
        case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    }
    return GL_LINES;
}

// ============================================================
// WebGLBuffer
// ============================================================
WebGLBuffer::WebGLBuffer(GLenum target, size_t size)
    : m_target(target), m_size(size) {
    glGenBuffers(1, &m_id);
    glBindBuffer(m_target, m_id);
    glBufferData(m_target, static_cast<GLsizei>(size), nullptr, GL_DYNAMIC_DRAW);
}

WebGLBuffer::~WebGLBuffer() {
    if (m_id) {
        glDeleteBuffers(1, &m_id);
        m_id = 0;
    }
}

void WebGLBuffer::update_data(const void* data, size_t offset, size_t size) {
    glBindBuffer(m_target, m_id);
    if (offset == 0 && size >= m_size) {
        glBufferData(m_target, static_cast<GLsizei>(size), data, GL_DYNAMIC_DRAW);
        m_size = size;
    } else {
        glBufferSubData(m_target, static_cast<GLint>(offset),
                        static_cast<GLsizei>(size), data);
    }
}

// ============================================================
// WebGLPipeline
// ============================================================
WebGLPipeline::WebGLPipeline(GLuint program, const PipelineDesc& desc)
    : m_program(program), m_desc(desc) {}

WebGLPipeline::~WebGLPipeline() {
    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
}

// ============================================================
// WebGLCommandList
// ============================================================
void WebGLCommandList::begin() {}

void WebGLCommandList::end() {}

void WebGLCommandList::set_viewport(uint32_t x, uint32_t y,
                                      uint32_t w, uint32_t h) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(w), static_cast<GLsizei>(h));
}

void WebGLCommandList::set_pipeline(GfxPipeline* pipeline) {
    auto* gl_pipeline = static_cast<WebGLPipeline*>(pipeline);
    if (gl_pipeline) {
        glUseProgram(gl_pipeline->program());
        m_current_pipeline = gl_pipeline;
        m_current_topology = topology_to_gl(gl_pipeline->desc().topology);
    }
}

void WebGLCommandList::set_vertex_buffer(uint32_t slot, GfxBuffer* buffer,
                                           size_t offset) {
    auto* gl_buf = static_cast<WebGLBuffer*>(buffer);
    if (!gl_buf) return;
    glBindBuffer(GL_ARRAY_BUFFER, gl_buf->gl_id());
    glEnableVertexAttribArray(slot);
    glVertexAttribPointer(slot, 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(float),
                          reinterpret_cast<const void*>(offset));
}

void WebGLCommandList::set_index_buffer(GfxBuffer* buffer, size_t /*offset*/) {
    auto* gl_buf = static_cast<WebGLBuffer*>(buffer);
    if (!gl_buf) return;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_buf->gl_id());
}

void WebGLCommandList::set_uniform_data(uint32_t slot, const void* data,
                                          size_t size) {
    if (!m_current_pipeline) return;
    GLint loc = static_cast<GLint>(slot);
    if (size == 4 * sizeof(float)) {
        const auto* f = static_cast<const float*>(data);
        glUniform4f(loc, f[0], f[1], f[2], f[3]);
    } else if (size == 16 * sizeof(float)) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, static_cast<const GLfloat*>(data));
    }
}

void WebGLCommandList::draw_instanced(uint32_t vertex_count,
                                        uint32_t /*instance_count*/,
                                        uint32_t first_vertex,
                                        uint32_t /*first_instance*/) {
    glDrawArrays(m_current_topology,
                 static_cast<GLint>(first_vertex),
                 static_cast<GLsizei>(vertex_count));
}

void WebGLCommandList::draw_indexed_instanced(uint32_t index_count,
                                                 uint32_t /*instance_count*/,
                                                 uint32_t first_index,
                                                 uint32_t /*base_vertex*/,
                                                 uint32_t /*first_instance*/) {
    glDrawElements(m_current_topology,
                   static_cast<GLsizei>(index_count),
                   GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(
                       static_cast<uintptr_t>(first_index * sizeof(uint32_t))));
}

void WebGLCommandList::draw_arrays(uint32_t vertex_count, uint32_t first_vertex) {
    glDrawArrays(m_current_topology,
                 static_cast<GLint>(first_vertex),
                 static_cast<GLsizei>(vertex_count));
}

// ============================================================
// WebGLDevice
// ============================================================
WebGLDevice::WebGLDevice() = default;
WebGLDevice::~WebGLDevice() = default;

bool WebGLDevice::initialize() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.1f, 0.1f, 0.18f, 1.0f);
    m_initialized = true;
    return true;
}

void WebGLDevice::shutdown() {
    m_buffers.clear();
    m_pipelines.clear();
    m_initialized = false;
}

void WebGLDevice::resize(uint32_t width, uint32_t height) {
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

GLuint WebGLDevice::compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        GLsizei log_len = 0;
        glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log)) - 1, &log_len, log);
        log[log_len] = '\0';
        std::fprintf(stderr, "[WebGL] Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint WebGLDevice::link_program(GLuint vert, GLuint frag) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        GLsizei log_len = 0;
        glGetProgramInfoLog(program, static_cast<GLsizei>(sizeof(log)) - 1, &log_len, log);
        log[log_len] = '\0';
        std::fprintf(stderr, "[WebGL] Program link error: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GfxBuffer* WebGLDevice::create_buffer(const BufferDesc& desc) {
    GLenum target = (desc.usage == BufferUsage::Index)
                        ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    auto buf = std::make_unique<WebGLBuffer>(target, desc.size);
    if (desc.initial_data && desc.size > 0) {
        buf->update_data(desc.initial_data, 0, desc.size);
    }
    auto* ptr = buf.get();
    m_buffers.push_back(std::move(buf));
    return ptr;
}

GfxPipeline* WebGLDevice::create_pipeline(const PipelineDesc& desc) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER, k_line_vertex_shader);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, k_line_fragment_shader);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return nullptr;
    }
    GLuint program = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!program) return nullptr;

    auto pipe = std::make_unique<WebGLPipeline>(program, desc);
    auto* ptr = pipe.get();
    m_pipelines.push_back(std::move(pipe));
    return ptr;
}

void WebGLDevice::destroy_buffer(GfxBuffer* buffer) {
    auto it = std::find_if(m_buffers.begin(), m_buffers.end(),
        [buffer](const std::unique_ptr<WebGLBuffer>& b) {
            return b.get() == buffer;
        });
    if (it != m_buffers.end()) m_buffers.erase(it);
}

void WebGLDevice::destroy_pipeline(GfxPipeline* pipeline) {
    auto it = std::find_if(m_pipelines.begin(), m_pipelines.end(),
        [pipeline](const std::unique_ptr<WebGLPipeline>& p) {
            return p.get() == pipeline;
        });
    if (it != m_pipelines.end()) m_pipelines.erase(it);
}

void WebGLDevice::begin_frame() {
    glClear(GL_COLOR_BUFFER_BIT);
}

void WebGLDevice::end_frame() {}

void WebGLDevice::present() {
    // Emscripten handles swap chain automatically
}

} // namespace cad::gfx

#endif // __EMSCRIPTEN__
