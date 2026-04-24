#pragma once

#include "cad/scene/block.h"
#include "cad/scene/entity.h"

#include <string>
#include <vector>

namespace cad {
namespace block_classify {

bool is_model_or_paper_space(const std::string& name);

Bounds3d bounds_for_indices(
    const std::vector<EntityVariant>& entities,
    const std::vector<int32_t>& indices);

bool should_render_direct(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert,
    float scene_extent = 5000.0f);

bool should_merge_header_entities(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert);

}  // namespace block_classify
}  // namespace cad
