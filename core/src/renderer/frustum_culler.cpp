#include "cad/renderer/frustum_culler.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/spatial_index.h"

namespace cad {

std::vector<int32_t> FrustumCuller::cull(const SceneGraph& scene,
                                          const Bounds3d& camera_visible_bounds) {
    // Delegate to SceneGraph::entities_in_bounds which already uses
    // the Quadtree spatial index when available, with brute-force fallback.
    return scene.entities_in_bounds(camera_visible_bounds);
}

std::vector<int32_t> FrustumCuller::cull_with_index(const SpatialIndex& index,
                                                      const Bounds3d& camera_visible_bounds) {
    return index.query_bounds(camera_visible_bounds);
}

} // namespace cad
