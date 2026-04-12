#pragma once

#include "cad/cad_types.h"

#include <string>

namespace cad {

struct Linetype {
    std::string name;
    std::string description;
    LinePattern pattern;
};

} // namespace cad
