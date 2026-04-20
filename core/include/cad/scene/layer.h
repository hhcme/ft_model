#pragma once

#include "cad/cad_types.h"

#include <string>

namespace cad {

struct Layer {
    std::string name;
    Color color = Color::white();
    int32_t linetype_index = 0;
    float lineweight = 0.0f;       // 0 = default
    int32_t plot_style_index = -1;
    bool is_frozen = false;
    bool is_off = false;
    bool is_locked = false;
    bool plot_enabled = true;
};

} // namespace cad
