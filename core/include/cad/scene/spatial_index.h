#pragma once

#include "cad/cad_types.h"
#include <vector>
#include <cstdint>
#include <functional>

namespace cad {

// ============================================================
// Abstract SpatialIndex — 2D Quadtree interface
// ============================================================
class SpatialIndex {
public:
    virtual ~SpatialIndex() = default;

    virtual void insert(int32_t entity_id, const Bounds3d& bounds) = 0;
    virtual void remove(int32_t entity_id) = 0;
    virtual void clear() = 0;
    virtual void rebuild() = 0;

    // Query entities whose bounds intersect the given bounds
    virtual std::vector<int32_t> query_bounds(const Bounds3d& bounds) const = 0;

    // Query entities near a point (2D)
    virtual std::vector<int32_t> query_point(const Vec3& point, float tolerance = 0.0f) const = 0;

    // Statistics
    virtual size_t size() const = 0;
};

// ============================================================
// Quadtree — 2D spatial index implementation
// ============================================================
class Quadtree : public SpatialIndex {
public:
    // Constructor: bounds of the entire space, max entities per leaf before split
    explicit Quadtree(const Bounds3d& world_bounds = Bounds3d::empty(),
                      int max_entities_per_node = 32,
                      int max_depth = 8);
    ~Quadtree() override;

    // Non-copyable, movable
    Quadtree(const Quadtree&) = delete;
    Quadtree& operator=(const Quadtree&) = delete;
    Quadtree(Quadtree&&) noexcept;
    Quadtree& operator=(Quadtree&&) noexcept;

    void insert(int32_t entity_id, const Bounds3d& bounds) override;
    void remove(int32_t entity_id) override;
    void clear() override;
    void rebuild() override;

    std::vector<int32_t> query_bounds(const Bounds3d& bounds) const override;
    std::vector<int32_t> query_point(const Vec3& point, float tolerance = 0.0f) const override;

    size_t size() const override { return m_entity_count; }

    // Bulk insert — clears and rebuilds from scratch (faster than individual inserts)
    void bulk_insert(const std::vector<std::pair<int32_t, Bounds3d>>& entities);

private:
    struct Entry {
        int32_t entity_id;
        Bounds3d bounds;
    };

    struct Node {
        Bounds3d bounds;              // 2D bounds of this quadrant (z ignored)
        std::vector<Entry> entries;   // Entities stored in this leaf
        Node* children[4] = {};       // NW, NE, SW, SE (null = not subdivided)
        bool is_leaf = true;

        ~Node() {
            for (auto& child : children) {
                delete child;
                child = nullptr;
            }
        }
    };

    Node* m_root = nullptr;
    int m_max_entities_per_node;
    int m_max_depth;
    size_t m_entity_count = 0;

    void subdivide(Node* node);
    int get_quadrant(const Bounds3d& node_bounds, const Bounds3d& entry_bounds) const;
    void insert_recursive(Node* node, const Entry& entry, int depth);
    void query_recursive(const Node* node, const Bounds3d& query,
                         std::vector<int32_t>& result) const;
    void query_point_recursive(const Node* node, const Vec3& point,
                               float tolerance, std::vector<int32_t>& result) const;
    void collect_all(const Node* node, std::vector<Entry>& entries) const;
};

} // namespace cad
