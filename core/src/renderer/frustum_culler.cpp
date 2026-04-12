#include "cad/renderer/frustum_culler.h"

namespace cad {

std::vector<size_t> FrustumCuller::cull(const Scene& /*scene*/,
                                          const Bounds3d& /*camera_visible_bounds*/) {
    // TODO: Implement once Scene and Entity types are defined.
    // Brute force approach:
    //   for each entity in scene:
    //     if entity.bounds.intersects(camera_visible_bounds):
    //       result.push_back(entity_index)
    // If spatial index is available on the scene, use that instead.
    return {};
}

std::vector<size_t> FrustumCuller::cull(const std::vector<const Entity*>& /*entities*/,
                                          const Bounds3d& /*camera_visible_bounds*/) {
    // TODO: Implement once Entity type is defined.
    return {};
}

} // namespace cad
