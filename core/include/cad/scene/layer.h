#pragma once

#include "cad/cad_types.h"

#include <string>

namespace cad {

struct Layer {
    std::string name;
    Color color = Color::white();
    int32_t linetype_index = 0;
    float lineweight = 0.0f;       // 0 = default
    bool is_frozen = false;
    bool is_off = false;
    bool is_locked = false;
};

} // namespace cad
