#pragma once

#include "cad/cad_types.h"
#include "cad/scene/entity.h"
#include "cad/scene/layer.h"
#include "cad/scene/linetype.h"
#include "cad/scene/block.h"
#include "cad/scene/text_style.h"
#include "cad/scene/viewport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad {

class SpatialIndex;

// ============================================================
// Drawing metadata from HEADER section
// ============================================================
struct DrawingMetadata {
    std::string filename;
    Bounds3d extents = Bounds3d::empty();
    Vec3 insertion_base = Vec3::zero();
    float text_size = 0.0f;
    std::string acad_version;
};

// ============================================================
// SceneGraph — central data structure for a CAD drawing
//
// Stores entities in a single vector using EntityVariant.
// Tables (layers, linetypes, etc.) are stored as separate vectors
// with name-based lookup maps. A vertex buffer supports polyline
// entities that reference shared vertex data via offset/count.
// ============================================================
class SceneGraph {
public:
    SceneGraph();
    ~SceneGraph();

    // Non-copyable, movable
    SceneGraph(const SceneGraph&) = delete;
    SceneGraph& operator=(const SceneGraph&) = delete;
    SceneGraph(SceneGraph&&) noexcept;
    SceneGraph& operator=(SceneGraph&&) noexcept;

    // ---- Entity management ----

    // Add an entity (EntityVariant). Returns the entity index.
    int32_t add_entity(EntityVariant entity);

    // Type-specific adders — construct the EntityVariant internally.
    // Each returns the entity index in the entities vector.
    int32_t add_line(EntityVariant entity);
    int32_t add_circle(EntityVariant entity);
    int32_t add_arc(EntityVariant entity);
    int32_t add_polyline(EntityVariant entity);
    int32_t add_insert(EntityVariant entity);

    // Polyline vertex buffer — append vertices and return the starting offset.
    int32_t add_polyline_vertices(const Vec3* vertices, size_t count);
    std::vector<Vec3>& vertex_buffer();
    const std::vector<Vec3>& vertex_buffer() const;

    // Access all entities
    const std::vector<EntityVariant>& entities() const;
    std::vector<EntityVariant>& entities();

    // Entity count
    size_t total_entity_count() const;

    // ---- Table management ----

    int32_t add_layer(Layer layer);
    int32_t add_linetype(Linetype lt);
    int32_t add_text_style(TextStyle style);
    int32_t add_block(Block block);
    int32_t add_viewport(Viewport vp);

    // Find or create a layer by name. Returns the layer index.
    int32_t find_or_add_layer(const std::string& name);

    // Update an existing layer's properties by index.
    // Returns true if the layer was found and updated.
    bool update_layer(int32_t index, const Layer& layer);

    const std::vector<Layer>& layers() const;
    const std::vector<Linetype>& linetypes() const;
    const std::vector<TextStyle>& text_styles() const;
    const std::vector<Block>& blocks() const;
    const std::vector<Viewport>& viewports() const;

    // Name-based lookups (returns index or -1)
    int32_t find_layer(const std::string& name) const;
    int32_t find_linetype(const std::string& name) const;
    int32_t find_text_style(const std::string& name) const;
    int32_t find_block(const std::string& name) const;

    // ---- Drawing metadata ----
    DrawingMetadata& drawing_info();
    const DrawingMetadata& drawing_info() const;

    // ---- Queries ----

    // Get all entity indices within a bounding region
    std::vector<int32_t> entities_in_bounds(const Bounds3d& bounds) const;

    // Get all entity indices on a given layer (by layer index)
    std::vector<int32_t> entities_on_layer(int32_t layer_index) const;

    // Overall bounding box of all entities
    Bounds3d total_bounds() const;

    // ---- Spatial index ----
    void rebuild_spatial_index();
    SpatialIndex* spatial_index();

    // ---- Iteration ----

    // Iterate over all entities
    void for_each_entity(const std::function<void(const EntityVariant&)>& fn) const;

    // Iterate over entities of a specific type
    void for_each_entity_of_type(EntityType type,
                                  const std::function<void(const EntityVariant&)>& fn) const;

    // ---- Utility ----
    void clear();

    // Pre-allocate vectors for known entity/vertex counts (call before parsing).
    void reserve(size_t entity_count, size_t vertex_count = 0);

    // Release excess vector capacity after parsing completes.
    void shrink_to_fit();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cad
