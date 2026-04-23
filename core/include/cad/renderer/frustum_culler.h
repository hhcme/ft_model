#pragma once

#include "cad/cad_types.h"
#include <vector>
#include <cstddef>

namespace cad {

class SceneGraph;
class SpatialIndex;

// ============================================================
// FrustumCuller — culls entities against a camera frustum.
//
// When the SceneGraph has a built spatial index (Quadtree),
// uses O(log N) spatial queries. Otherwise falls back to
// brute-force O(N) bounds intersection.
// ============================================================
class FrustumCuller {
public:
    // Returns entity indices visible within camera_visible_bounds.
    // Queries the SceneGraph's spatial index when available.
    std::vector<int32_t> cull(const SceneGraph& scene,
                              const Bounds3d& camera_visible_bounds);

    // Cull using a standalone spatial index (e.g. for sub-scenes or blocks).
    static std::vector<int32_t> cull_with_index(const SpatialIndex& index,
                                                 const Bounds3d& camera_visible_bounds);
};

} // namespace cad
