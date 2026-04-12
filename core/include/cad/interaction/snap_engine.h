#pragma once
#include "cad/cad_types.h"
#include <vector>

namespace cad {
enum class SnapMode { Endpoint = 1, Midpoint = 2, Center = 4, Intersection = 8, Nearest = 16 };
struct SnapPoint { Vec3 point; SnapMode mode; int64_t entity_id; };

class SnapEngine {
public:
    void set_modes(int modes_bitmask);  // Combination of SnapMode values
    SnapPoint snap(const Vec3& world_point, const class SceneGraph& scene,
                   const class SpatialIndex& index, float tolerance);
private:
    int m_modes = static_cast<int>(SnapMode::Endpoint);
};
} // namespace cad
