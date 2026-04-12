#pragma once
#include "cad/gfx/gfx_device.h"

namespace cad::gfx {

// ============================================================
// SoftwareDevice — future CPU rasterizer fallback
// TODO: Phase 5 implementation
// ============================================================
class SoftwareDevice : public GfxDevice {
public:
    bool initialize() override { return true; }
    void shutdown() override {}
    void resize(uint32_t /*width*/, uint32_t /*height*/) override {}

    GfxBuffer* create_buffer(const BufferDesc& /*desc*/) override { return nullptr; }
    GfxPipeline* create_pipeline(const PipelineDesc& /*desc*/) override { return nullptr; }
    void destroy_buffer(GfxBuffer*) override {}
    void destroy_pipeline(GfxPipeline*) override {}

    void begin_frame() override {}
    void end_frame() override {}
    void present() override {}

    uint32_t max_texture_size() const override { return 4096; }
    bool supports_instancing() const override { return false; }
};

} // namespace cad::gfx
