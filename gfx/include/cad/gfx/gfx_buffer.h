#pragma once

#include <cstddef>

namespace cad {
namespace gfx {

// ============================================================
// Abstract GPU buffer
// ============================================================
class GfxBuffer {
public:
    virtual ~GfxBuffer() = default;

    virtual void update_data(const void* data, size_t offset, size_t size) = 0;
    virtual size_t size() const = 0;
};

} // namespace gfx
} // namespace cad
