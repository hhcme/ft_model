#pragma once

#include "cad/cad_types.h"
#include <vector>
#include <cstddef>

namespace cad {

struct Entity;
struct Scene;

class FrustumCuller {
public:
    // Returns indices of visible entities from the scene.
    // Uses spatial index if available, otherwise brute-force bounds test.
    std::vector<size_t> cull(const Scene& scene, const Bounds3d& camera_visible_bounds);

    // Overload: cull a pre-filtered list of entity pointers
    std::vector<size_t> cull(const std::vector<const Entity*>& entities,
                             const Bounds3d& camera_visible_bounds);
};

} // namespace cad
