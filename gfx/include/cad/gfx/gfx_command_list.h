#pragma once

#include "cad/gfx/gfx_types.h"
#include <cstdint>
#include <cstddef>

namespace cad {
namespace gfx {

class GfxPipeline;
class GfxBuffer;

// ============================================================
// Abstract command list for recording GPU draw commands
// ============================================================
class GfxCommandList {
public:
    virtual ~GfxCommandList() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    virtual void set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
    virtual void set_pipeline(GfxPipeline* pipeline) = 0;
    virtual void set_vertex_buffer(uint32_t slot, GfxBuffer* buffer, size_t offset = 0) = 0;
    virtual void set_index_buffer(GfxBuffer* buffer, size_t offset = 0) = 0;
    virtual void set_uniform_data(uint32_t slot, const void* data, size_t size) = 0;

    virtual void draw_instanced(uint32_t vertex_count, uint32_t instance_count,
                                 uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
    virtual void draw_indexed_instanced(uint32_t index_count, uint32_t instance_count,
                                         uint32_t first_index = 0, uint32_t base_vertex = 0,
                                         uint32_t first_instance = 0) = 0;
};

} // namespace gfx
} // namespace cad
