#pragma once

#include <string>

namespace cad {

struct TextStyle {
    std::string name;
    std::string font_file;
    float fixed_height = 0.0f;     // 0 = variable height
    float width_factor = 1.0f;
    bool is_shx = false;           // true if using SHX font (vs TTF)
};

} // namespace cad
