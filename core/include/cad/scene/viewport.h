#pragma once

#include "cad/cad_types.h"

#include <string>
#include <vector>

namespace cad {

// Paper space viewport definition.
// Model space viewports are handled differently (tiled viewports)
// and are not represented here.
struct Viewport {
    std::string name;
    Vec3 center = Vec3::zero();
    float width = 0.0f;
    float height = 0.0f;
    int32_t layout_index = -1;
    Vec3 paper_center = Vec3::zero();
    float paper_width = 0.0f;
    float paper_height = 0.0f;
    Vec3 model_view_center = Vec3::zero();
    Vec3 model_view_target = Vec3::zero();
    float view_height = 0.0f;
    float custom_scale = 1.0f;
    float twist_angle = 0.0f;
    Bounds3d clip_boundary = Bounds3d::empty();
    std::vector<int32_t> frozen_layer_indices;
    bool is_paper_space = true;
};

} // namespace cad
