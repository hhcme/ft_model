#pragma once

#include "cad/gfx/gfx_types.h"
#include <cstdint>
#include <cstddef>

namespace cad {
namespace gfx {

class GfxBuffer;
class GfxPipeline;

// ============================================================
// Abstract graphics device interface
// ============================================================
class GfxDevice {
public:
    virtual ~GfxDevice() = default;

    // -- Lifecycle --
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;

    // -- Resource creation --
    virtual GfxBuffer* create_buffer(const BufferDesc& desc) = 0;
    virtual GfxPipeline* create_pipeline(const PipelineDesc& desc) = 0;
    virtual void destroy_buffer(GfxBuffer* buffer) = 0;
    virtual void destroy_pipeline(GfxPipeline* pipeline) = 0;

    // -- Frame --
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual void present() = 0;

    // -- Capabilities --
    virtual uint32_t max_texture_size() const = 0;
    virtual bool supports_instancing() const = 0;
};

} // namespace gfx
} // namespace cad
