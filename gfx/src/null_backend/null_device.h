#pragma once
#include "cad/gfx/gfx_device.h"
#include "cad/gfx/gfx_buffer.h"
#include "cad/gfx/gfx_pipeline.h"
#include "cad/gfx/gfx_command_list.h"
#include <vector>
#include <memory>

namespace cad::gfx {

// ============================================================
// Null buffer — no-op, for testing
// ============================================================
class NullBuffer : public GfxBuffer {
public:
    explicit NullBuffer(size_t size) : m_size(size) {}
    void update_data(const void* /*data*/, size_t /*offset*/, size_t /*size*/) override {}
    size_t size() const override { return m_size; }
private:
    size_t m_size;
};

// ============================================================
// Null pipeline — no-op, for testing
// ============================================================
class NullPipeline : public GfxPipeline {};

// ============================================================
// Null command list — no-op, for testing
// ============================================================
class NullCommandList : public GfxCommandList {
public:
    void begin() override {}
    void end() override {}

    void set_viewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void set_pipeline(GfxPipeline*) override {}
    void set_vertex_buffer(uint32_t, GfxBuffer*, size_t) override {}
    void set_index_buffer(GfxBuffer*, size_t) override {}
    void set_uniform_data(uint32_t, const void*, size_t) override {}

    void draw_instanced(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void draw_indexed_instanced(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
};

// ============================================================
// NullDevice — headless backend for unit testing
// ============================================================
class NullDevice : public GfxDevice {
public:
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

    // Test helpers
    size_t buffer_count() const { return m_buffers.size(); }
    size_t pipeline_count() const { return m_pipelines.size(); }

private:
    NullCommandList m_command_list;
    std::vector<std::unique_ptr<NullBuffer>> m_buffers;
    std::vector<std::unique_ptr<NullPipeline>> m_pipelines;
};

} // namespace cad::gfx
