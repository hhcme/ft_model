#pragma once

#include "cad/cad_types.h"

namespace cad {

class Camera;

// ============================================================
// Per-frame render state
// ============================================================
struct RenderContext {
    const Camera* camera = nullptr;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    Color background_color = Color::white();
};

} // namespace cad
