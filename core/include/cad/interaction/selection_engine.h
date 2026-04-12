#pragma once
#include "cad/cad_types.h"
#include <vector>
#include <cstdint>

namespace cad {
class SceneGraph;
class SpatialIndex;
class Camera;

class SelectionEngine {
public:
    int64_t pick(const Vec2& screen_point, const Camera& camera,
                 const SceneGraph& scene, const SpatialIndex& index,
                 float tolerance = 5.0f);
    std::vector<int64_t> box_select(const Bounds3d& world_bounds,
                                     const SceneGraph& scene,
                                     const SpatialIndex& index);
    void set_selected(int64_t entity_id);
    void set_selected(const std::vector<int64_t>& entity_ids);
    void clear_selection();
    const std::vector<int64_t>& selected() const;
private:
    std::vector<int64_t> m_selected;
};
} // namespace cad
