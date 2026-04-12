#pragma once

namespace cad {
namespace gfx {

// ============================================================
// Abstract graphics pipeline (shader state holder)
// Empty for now; will hold shader program and render state.
// ============================================================
class GfxPipeline {
public:
    virtual ~GfxPipeline() = default;
};

} // namespace gfx
} // namespace cad
