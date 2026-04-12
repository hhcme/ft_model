#pragma once

#include "cad/gfx/gfx_device.h"
#include "cad/gfx/gfx_buffer.h"
#include "cad/gfx/gfx_pipeline.h"
#include "cad/gfx/gfx_command_list.h"
#include <vector>
#include <memory>

// ------------------------------------------------------------------
// WebGL 2.0 backend — only functional when compiled with Emscripten.
// Class declarations are always visible; method bodies live in the
// .cpp which is guarded by #ifdef __EMSCRIPTEN__.
// ------------------------------------------------------------------

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
// Forward-declare GL types used in the class interface so that
// headers compile without <GLES3/gl3.h>.
typedef unsigned int GLenum;
typedef unsigned int GLuint;
#endif

namespace cad::gfx {

// Embedded shader source strings

namespace cad::gfx {

// ============================================================
// Line shader source (embedded, no file I/O at runtime)
// ============================================================
extern const char* k_line_vertex_shader;
extern const char* k_line_fragment_shader;

// ============================================================
// WebGLBuffer — wraps a GPU buffer object
// ============================================================
class WebGLBuffer : public GfxBuffer {
public:
    explicit WebGLBuffer(unsigned int target, size_t size);
    ~WebGLBuffer() override;

    void update_data(const void* data, size_t offset, size_t size) override;
    size_t size() const override { return m_size; }

    unsigned int gl_id() const { return m_id; }
    unsigned int target() const { return m_target; }

private:
    unsigned int m_id = 0;
    unsigned int m_target;
    size_t m_size = 0;
};

// ============================================================
// WebGLPipeline — wraps a shader program + render state
// ============================================================
class WebGLPipeline : public GfxPipeline {
public:
    WebGLPipeline(unsigned int program, const PipelineDesc& desc);
    ~WebGLPipeline() override;

    unsigned int program() const { return m_program; }
    const PipelineDesc& desc() const { return m_desc; }

private:
    unsigned int m_program = 0;
    PipelineDesc m_desc;
};

// ============================================================
// WebGLCommandList — records and executes GL draw calls
// ============================================================
class WebGLCommandList : public GfxCommandList {
public:
    void begin() override;
    void end() override;

    void set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    void set_pipeline(GfxPipeline* pipeline) override;
    void set_vertex_buffer(uint32_t slot, GfxBuffer* buffer, size_t offset = 0) override;
    void set_index_buffer(GfxBuffer* buffer, size_t offset = 0) override;
    void set_uniform_data(uint32_t slot, const void* data, size_t size) override;

    void draw_instanced(uint32_t vertex_count, uint32_t instance_count,
                         uint32_t first_vertex = 0, uint32_t first_instance = 0) override;
    void draw_indexed_instanced(uint32_t index_count, uint32_t instance_count,
                                 uint32_t first_index = 0, uint32_t base_vertex = 0,
                                 uint32_t first_instance = 0) override;

    void draw_arrays(uint32_t vertex_count, uint32_t first_vertex = 0);

private:
    unsigned int topology_to_gl(PrimitiveTopology topo);
    WebGLPipeline* m_current_pipeline = nullptr;
    unsigned int m_current_topology = 1; // GL_LINES
};

// ============================================================
// WebGLDevice — WebGL 2.0 graphics backend
// ============================================================
class WebGLDevice : public GfxDevice {
public:
    WebGLDevice();
    ~WebGLDevice() override;

    bool initialize() override;
    void shutdown() override;
    void resize(uint32_t width, uint32_t height) override;

    GfxBuffer* create_buffer(const BufferDesc& desc) override;
    GfxPipeline* create_pipeline(const PipelineDesc& desc) override;
    void destroy_buffer(GfxBuffer* buffer) override;
    void destroy_pipeline(GfxPipeline* pipeline) override;

    void begin_frame() override;
    void end_frame() override;
    void present() override;

    uint32_t max_texture_size() const override { return 4096; }
    bool supports_instancing() const override { return true; }

private:
    unsigned int compile_shader(unsigned int type, const char* source);
    unsigned int link_program(unsigned int vert, unsigned int frag);

    std::vector<std::unique_ptr<WebGLBuffer>> m_buffers;
    std::vector<std::unique_ptr<WebGLPipeline>> m_pipelines;
    bool m_initialized = false;
};

} // namespace cad::gfx
