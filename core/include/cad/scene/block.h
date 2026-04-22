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
    std::vector<int32_t> header_owned_entity_indices;
    bool is_model_space = false;
    bool is_paper_space = false;
    bool is_anonymous = false;
    int32_t owner_layout_index = -1;
    uint64_t dwg_block_handle = 0;
    uint64_t dwg_block_header_handle = 0;
};

} // namespace cad
