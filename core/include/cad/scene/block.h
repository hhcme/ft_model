#pragma once

#include "cad/cad_types.h"

#include <string>
#include <vector>

namespace cad {

struct Block {
    std::string name;
    Vec3 base_point = Vec3::zero();
    Bounds3d bounds = Bounds3d::empty();
    std::vector<int32_t> entity_indices;
};

} // namespace cad
