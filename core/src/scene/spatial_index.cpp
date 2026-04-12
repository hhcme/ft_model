#include "cad/scene/spatial_index.h"
#include <algorithm>
#include <cmath>

namespace cad {

// ============================================================
// Quadtree Implementation
// ============================================================

Quadtree::Quadtree(const Bounds3d& world_bounds, int max_entities_per_node, int max_depth)
    : m_max_entities_per_node(max_entities_per_node)
    , m_max_depth(max_depth)
{
    m_root = new Node();
    m_root->bounds = world_bounds.is_empty()
        ? Bounds3d{{-100000, -100000, 0}, {100000, 100000, 0}}
        : world_bounds;
}

Quadtree::~Quadtree() {
    delete m_root;
}

Quadtree::Quadtree(Quadtree&& other) noexcept
    : m_root(other.m_root)
    , m_max_entities_per_node(other.m_max_entities_per_node)
    , m_max_depth(other.m_max_depth)
    , m_entity_count(other.m_entity_count)
{
    other.m_root = new Node();
    other.m_root->bounds = Bounds3d{{-100000, -100000, 0}, {100000, 100000, 0}};
    other.m_entity_count = 0;
}

Quadtree& Quadtree::operator=(Quadtree&& other) noexcept {
    if (this != &other) {
        delete m_root;
        m_root = other.m_root;
        m_max_entities_per_node = other.m_max_entities_per_node;
        m_max_depth = other.m_max_depth;
        m_entity_count = other.m_entity_count;
        other.m_root = new Node();
        other.m_entity_count = 0;
    }
    return *this;
}

void Quadtree::insert(int32_t entity_id, const Bounds3d& bounds) {
    Entry entry{entity_id, bounds};
    insert_recursive(m_root, entry, 0);
    m_entity_count++;
}

void Quadtree::remove(int32_t entity_id) {
    // Simple removal: rebuild without the entity
    // For better performance, could track entity -> node mapping
    std::vector<Entry> all;
    collect_all(m_root, all);

    all.erase(
        std::remove_if(all.begin(), all.end(),
            [entity_id](const Entry& e) { return e.entity_id == entity_id; }),
        all.end()
    );

    clear();
    for (const auto& entry : all) {
        insert(entry.entity_id, entry.bounds);
    }
}

void Quadtree::clear() {
    // Delete children, keep root
    for (auto& child : m_root->children) {
        delete child;
        child = nullptr;
    }
    m_root->entries.clear();
    m_root->is_leaf = true;
    m_entity_count = 0;
}

void Quadtree::rebuild() {
    // Collect all entries, clear, and re-insert
    std::vector<Entry> all;
    collect_all(m_root, all);

    clear();

    // Recompute world bounds
    Bounds3d new_bounds = Bounds3d::empty();
    for (const auto& e : all) {
        new_bounds.expand(e.bounds);
    }
    if (!new_bounds.is_empty()) {
        m_root->bounds = new_bounds;
    }

    for (const auto& entry : all) {
        insert(entry.entity_id, entry.bounds);
    }
}

std::vector<int32_t> Quadtree::query_bounds(const Bounds3d& bounds) const {
    std::vector<int32_t> result;
    query_recursive(m_root, bounds, result);
    return result;
}

std::vector<int32_t> Quadtree::query_point(const Vec3& point, float tolerance) const {
    std::vector<int32_t> result;
    query_point_recursive(m_root, point, tolerance, result);
    return result;
}

void Quadtree::bulk_insert(const std::vector<std::pair<int32_t, Bounds3d>>& entities) {
    clear();

    // Compute world bounds from all entities
    Bounds3d new_bounds = Bounds3d::empty();
    for (const auto& [id, bounds] : entities) {
        new_bounds.expand(bounds);
    }
    if (!new_bounds.is_empty()) {
        m_root->bounds = new_bounds;
    }

    for (const auto& [id, bounds] : entities) {
        insert(id, bounds);
    }
}

// ============================================================
// Private methods
// ============================================================

int Quadtree::get_quadrant(const Bounds3d& node_bounds, const Bounds3d& entry_bounds) const {
    // Determine which quadrant the entry fits completely into
    // Returns -1 if it doesn't fit entirely in any single quadrant
    Vec3 mid = node_bounds.center();

    bool in_left = entry_bounds.max.x <= mid.x;
    bool in_right = entry_bounds.min.x >= mid.x;
    bool in_bottom = entry_bounds.max.y <= mid.y;
    bool in_top = entry_bounds.min.y >= mid.y;

    if (in_top && in_left)   return 0; // NW (top-left)
    if (in_top && in_right)  return 1; // NE (top-right)
    if (in_bottom && in_left)  return 2; // SW (bottom-left)
    if (in_bottom && in_right) return 3; // SE (bottom-right)

    return -1; // Spans multiple quadrants
}

void Quadtree::subdivide(Node* node) {
    Vec3 mid = node->bounds.center();
    float hw = node->bounds.width() * 0.5f;
    float hh = node->bounds.height() * 0.5f;

    // NW (top-left)
    node->children[0] = new Node();
    node->children[0]->bounds = {
        {node->bounds.min.x, mid.y, 0},
        {mid.x, node->bounds.max.y, 0}
    };

    // NE (top-right)
    node->children[1] = new Node();
    node->children[1]->bounds = {
        {mid.x, mid.y, 0},
        {node->bounds.max.x, node->bounds.max.y, 0}
    };

    // SW (bottom-left)
    node->children[2] = new Node();
    node->children[2]->bounds = {
        {node->bounds.min.x, node->bounds.min.y, 0},
        {mid.x, mid.y, 0}
    };

    // SE (bottom-right)
    node->children[3] = new Node();
    node->children[3]->bounds = {
        {mid.x, node->bounds.min.y, 0},
        {node->bounds.max.x, mid.y, 0}
    };

    node->is_leaf = false;

    // Re-distribute existing entries to children
    std::vector<Entry> remaining;
    for (auto& entry : node->entries) {
        int quadrant = get_quadrant(node->bounds, entry.bounds);
        if (quadrant >= 0) {
            node->children[quadrant]->entries.push_back(std::move(entry));
        } else {
            remaining.push_back(std::move(entry));
        }
    }
    node->entries = std::move(remaining);
}

void Quadtree::insert_recursive(Node* node, const Entry& entry, int depth) {
    if (node->is_leaf) {
        if (static_cast<int>(node->entries.size()) < m_max_entities_per_node || depth >= m_max_depth) {
            node->entries.push_back(entry);
            return;
        }

        // Need to subdivide
        subdivide(node);
    }

    // Try to fit into a child quadrant
    int quadrant = get_quadrant(node->bounds, entry.bounds);
    if (quadrant >= 0) {
        insert_recursive(node->children[quadrant], entry, depth + 1);
    } else {
        // Doesn't fit in any child — store in this node
        node->entries.push_back(entry);
    }
}

void Quadtree::query_recursive(const Node* node, const Bounds3d& query,
                                std::vector<int32_t>& result) const {
    if (!node->bounds.intersects(query)) {
        return;
    }

    // Check entries in this node
    for (const auto& entry : node->entries) {
        if (entry.bounds.intersects(query)) {
            result.push_back(entry.entity_id);
        }
    }

    // Recurse into children
    if (!node->is_leaf) {
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                query_recursive(node->children[i], query, result);
            }
        }
    }
}

void Quadtree::query_point_recursive(const Node* node, const Vec3& point,
                                      float tolerance, std::vector<int32_t>& result) const {
    Bounds3d query_bounds = tolerance > 0
        ? Bounds3d::from_point(point).inflated(tolerance)
        : Bounds3d::from_point(point);

    if (!node->bounds.intersects(query_bounds)) {
        return;
    }

    for (const auto& entry : node->entries) {
        if (tolerance > 0) {
            // Check if point is within tolerance of the entry bounds
            Bounds3d expanded = entry.bounds.inflated(tolerance);
            if (expanded.contains(point)) {
                result.push_back(entry.entity_id);
            }
        } else {
            if (entry.bounds.contains(point)) {
                result.push_back(entry.entity_id);
            }
        }
    }

    if (!node->is_leaf) {
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                query_point_recursive(node->children[i], point, tolerance, result);
            }
        }
    }
}

void Quadtree::collect_all(const Node* node, std::vector<Entry>& entries) const {
    for (const auto& entry : node->entries) {
        entries.push_back(entry);
    }
    if (!node->is_leaf) {
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                collect_all(node->children[i], entries);
            }
        }
    }
}

} // namespace cad
