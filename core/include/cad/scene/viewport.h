#pragma once

#include "cad/cad_types.h"

#include <string>

namespace cad {

// Paper space viewport definition.
// Model space viewports are handled differently (tiled viewports)
// and are not represented here.
struct Viewport {
    std::string name;
    Vec3 center = Vec3::zero();
    float width = 0.0f;
    float height = 0.0f;
};

} // namespace cad
