#include "cad/scene/scene_graph.h"
#include "cad/scene/spatial_index.h"

#include <cctype>

namespace cad {

// ============================================================
// SceneGraph::Impl
// ============================================================
class SceneGraph::Impl {
public:
    // All entities in a single vector
    std::vector<EntityVariant> entities;

    // Polyline vertex buffer — shared by all polyline/lwpolyline entities
    std::vector<Vec3> vertex_buffer;

    // Table entries
    std::vector<Layer> layers;
    std::vector<Linetype> linetypes;
    std::vector<TextStyle> text_styles;
    std::vector<Block> blocks;
    std::vector<Viewport> viewports;
    std::vector<Layout> layouts;
    std::vector<SceneDiagnostic> diagnostics;

    // Name -> index maps for fast lookup
    std::unordered_map<std::string, int32_t> layer_name_map;
    std::unordered_map<std::string, int32_t> linetype_name_map;
    std::unordered_map<std::string, int32_t> text_style_name_map;
    std::unordered_map<std::string, int32_t> block_name_map;

    // Drawing metadata
    DrawingMetadata drawing_info;

    // Spatial index
    std::unique_ptr<Quadtree> spatial_index;

    // Scene tree (built on demand by build_scene_tree)
    std::vector<SceneNode> scene_nodes;
    std::unordered_map<uint32_t, uint32_t> node_id_to_index; // node ID -> index in scene_nodes
    uint32_t next_node_id = 1;
    uint32_t root_node_id = 0; // 0 = not built
};

// ============================================================
// SceneGraph constructors / destructor
// ============================================================

SceneGraph::SceneGraph() : m_impl(std::make_unique<Impl>()) {}

SceneGraph::~SceneGraph() = default;

SceneGraph::SceneGraph(SceneGraph&& other) noexcept
    : m_impl(std::move(other.m_impl)) {}

SceneGraph& SceneGraph::operator=(SceneGraph&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

// ============================================================
// Entity management
// ============================================================

int32_t SceneGraph::add_entity(EntityVariant entity) {
    int32_t idx = static_cast<int32_t>(m_impl->entities.size());
    m_impl->entities.push_back(std::move(entity));
    return idx;
}

int32_t SceneGraph::add_line(EntityVariant entity) {
    return add_entity(std::move(entity));
}

int32_t SceneGraph::add_circle(EntityVariant entity) {
    return add_entity(std::move(entity));
}

int32_t SceneGraph::add_arc(EntityVariant entity) {
    return add_entity(std::move(entity));
}

int32_t SceneGraph::add_polyline(EntityVariant entity) {
    return add_entity(std::move(entity));
}

int32_t SceneGraph::add_insert(EntityVariant entity) {
    return add_entity(std::move(entity));
}

int32_t SceneGraph::add_polyline_vertices(const Vec3* vertices, size_t count) {
    int32_t offset = static_cast<int32_t>(m_impl->vertex_buffer.size());
    m_impl->vertex_buffer.insert(m_impl->vertex_buffer.end(), vertices, vertices + count);
    return offset;
}

std::vector<Vec3>& SceneGraph::vertex_buffer() { return m_impl->vertex_buffer; }
const std::vector<Vec3>& SceneGraph::vertex_buffer() const { return m_impl->vertex_buffer; }

const std::vector<EntityVariant>& SceneGraph::entities() const { return m_impl->entities; }
std::vector<EntityVariant>& SceneGraph::entities() { return m_impl->entities; }

size_t SceneGraph::total_entity_count() const {
    return m_impl->entities.size();
}

// ============================================================
// Table management
// ============================================================

int32_t SceneGraph::add_layer(Layer layer) {
    int32_t idx = static_cast<int32_t>(m_impl->layers.size());
    std::string name = layer.name;
    m_impl->layers.push_back(std::move(layer));
    m_impl->layer_name_map[name] = idx;
    return idx;
}

int32_t SceneGraph::add_linetype(Linetype lt) {
    int32_t idx = static_cast<int32_t>(m_impl->linetypes.size());
    std::string name = lt.name;
    m_impl->linetypes.push_back(std::move(lt));
    m_impl->linetype_name_map[name] = idx;
    return idx;
}

int32_t SceneGraph::add_text_style(TextStyle style) {
    int32_t idx = static_cast<int32_t>(m_impl->text_styles.size());
    std::string name = style.name;
    m_impl->text_styles.push_back(std::move(style));
    m_impl->text_style_name_map[name] = idx;
    return idx;
}

int32_t SceneGraph::add_block(Block block) {
    int32_t idx = static_cast<int32_t>(m_impl->blocks.size());
    std::string name = block.name;
    std::string upper = name;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    block.is_model_space = (upper == "*MODEL_SPACE");
    block.is_paper_space = (upper == "*PAPER_SPACE");
    block.is_anonymous = !name.empty() && name[0] == '*';
    m_impl->blocks.push_back(std::move(block));
    m_impl->block_name_map[name] = idx;
    return idx;
}

int32_t SceneGraph::add_viewport(Viewport vp) {
    int32_t idx = static_cast<int32_t>(m_impl->viewports.size());
    m_impl->viewports.push_back(std::move(vp));
    return idx;
}

int32_t SceneGraph::add_layout(Layout layout) {
    int32_t idx = static_cast<int32_t>(m_impl->layouts.size());
    m_impl->layouts.push_back(std::move(layout));
    return idx;
}

void SceneGraph::add_diagnostic(SceneDiagnostic diagnostic) {
    m_impl->diagnostics.push_back(std::move(diagnostic));
}

int32_t SceneGraph::find_or_add_layer(const std::string& name) {
    auto it = m_impl->layer_name_map.find(name);
    if (it != m_impl->layer_name_map.end()) {
        return it->second;
    }
    Layer layer;
    layer.name = name;
    return add_layer(std::move(layer));
}

bool SceneGraph::update_layer(int32_t index, const Layer& layer) {
    if (index < 0 || index >= static_cast<int32_t>(m_impl->layers.size())) {
        return false;
    }
    m_impl->layers[index] = layer;
    return true;
}

const std::vector<Layer>& SceneGraph::layers() const { return m_impl->layers; }
const std::vector<Linetype>& SceneGraph::linetypes() const { return m_impl->linetypes; }
const std::vector<TextStyle>& SceneGraph::text_styles() const { return m_impl->text_styles; }
const std::vector<Block>& SceneGraph::blocks() const { return m_impl->blocks; }
std::vector<Block>& SceneGraph::blocks() { return m_impl->blocks; }
const std::vector<Viewport>& SceneGraph::viewports() const { return m_impl->viewports; }
std::vector<Viewport>& SceneGraph::viewports() { return m_impl->viewports; }
const std::vector<Layout>& SceneGraph::layouts() const { return m_impl->layouts; }
std::vector<Layout>& SceneGraph::layouts() { return m_impl->layouts; }
const std::vector<SceneDiagnostic>& SceneGraph::diagnostics() const { return m_impl->diagnostics; }

int32_t SceneGraph::find_layer(const std::string& name) const {
    auto it = m_impl->layer_name_map.find(name);
    return it != m_impl->layer_name_map.end() ? it->second : -1;
}

int32_t SceneGraph::find_linetype(const std::string& name) const {
    auto it = m_impl->linetype_name_map.find(name);
    return it != m_impl->linetype_name_map.end() ? it->second : -1;
}

int32_t SceneGraph::find_text_style(const std::string& name) const {
    auto it = m_impl->text_style_name_map.find(name);
    return it != m_impl->text_style_name_map.end() ? it->second : -1;
}

int32_t SceneGraph::find_block(const std::string& name) const {
    auto it = m_impl->block_name_map.find(name);
    return it != m_impl->block_name_map.end() ? it->second : -1;
}

// ============================================================
// Drawing info
// ============================================================

DrawingMetadata& SceneGraph::drawing_info() { return m_impl->drawing_info; }
const DrawingMetadata& SceneGraph::drawing_info() const { return m_impl->drawing_info; }

// ============================================================
// Queries
// ============================================================

std::vector<int32_t> SceneGraph::entities_in_bounds(const Bounds3d& query_bounds) const {
    // Use Quadtree O(log N) spatial query when available
    if (m_impl->spatial_index && m_impl->spatial_index->size() > 0) {
        return m_impl->spatial_index->query_bounds(query_bounds);
    }
    // Fallback: brute-force O(N) traversal
    std::vector<int32_t> result;
    for (size_t i = 0; i < m_impl->entities.size(); ++i) {
        if (query_bounds.intersects(m_impl->entities[i].bounds())) {
            result.push_back(static_cast<int32_t>(i));
        }
    }
    return result;
}

std::vector<int32_t> SceneGraph::entities_on_layer(int32_t layer_index) const {
    std::vector<int32_t> result;
    for (size_t i = 0; i < m_impl->entities.size(); ++i) {
        if (m_impl->entities[i].header.layer_index == layer_index) {
            result.push_back(static_cast<int32_t>(i));
        }
    }
    return result;
}

std::vector<int32_t> SceneGraph::entities_in_space(DrawingSpace space) const {
    std::vector<int32_t> result;
    for (size_t i = 0; i < m_impl->entities.size(); ++i) {
        if (m_impl->entities[i].header.space == space) {
            result.push_back(static_cast<int32_t>(i));
        }
    }
    return result;
}

std::vector<int32_t> SceneGraph::entities_for_layout(int32_t layout_index) const {
    std::vector<int32_t> result;
    for (size_t i = 0; i < m_impl->entities.size(); ++i) {
        if (m_impl->entities[i].header.layout_index == layout_index) {
            result.push_back(static_cast<int32_t>(i));
        }
    }
    return result;
}

Bounds3d SceneGraph::total_bounds() const {
    Bounds3d total = Bounds3d::empty();
    for (const auto& ent : m_impl->entities) {
        total.expand(ent.bounds());
    }
    return total;
}

const Layout* SceneGraph::active_layout() const {
    auto has_presentation_bounds = [](const Layout& layout) {
        return !layout.plot_window.is_empty() ||
               !layout.border_bounds.is_empty() ||
               !layout.paper_bounds.is_empty();
    };
    for (const auto& layout : m_impl->layouts) {
        if (layout.is_active && !layout.is_model_layout &&
            has_presentation_bounds(layout)) {
            return &layout;
        }
    }
    for (const auto& layout : m_impl->layouts) {
        if (layout.is_current && !layout.is_model_layout &&
            has_presentation_bounds(layout)) {
            return &layout;
        }
    }
    for (const auto& layout : m_impl->layouts) {
        if (!layout.is_model_layout && has_presentation_bounds(layout)) {
            return &layout;
        }
    }
    // Final fallback: any non-model layout, even without presentation bounds.
    // Layouts with zero paper dimensions (no page setup configured) still own
    // paper-space entities that should be rendered.
    for (const auto& layout : m_impl->layouts) {
        if (!layout.is_model_layout) {
            return &layout;
        }
    }
    return nullptr;
}

Bounds3d SceneGraph::presentation_bounds() const {
    if (const Layout* layout = active_layout()) {
        if (!layout->plot_window.is_empty()) return layout->plot_window;
        if (!layout->border_bounds.is_empty()) return layout->border_bounds;
        if (!layout->paper_bounds.is_empty()) return layout->paper_bounds;
    }

    Bounds3d paper_bounds = Bounds3d::empty();
    for (const auto& ent : m_impl->entities) {
        if (ent.header.space == DrawingSpace::PaperSpace) {
            paper_bounds.expand(ent.bounds());
        }
    }
    if (!paper_bounds.is_empty()) return paper_bounds;

    Bounds3d model_bounds = Bounds3d::empty();
    for (const auto& ent : m_impl->entities) {
        if (ent.header.space == DrawingSpace::ModelSpace) {
            model_bounds.expand(ent.bounds());
        }
    }
    if (!model_bounds.is_empty()) return model_bounds;

    return total_bounds();
}

// ============================================================
// Spatial index
// ============================================================

void SceneGraph::rebuild_spatial_index() {
    Bounds3d world = total_bounds();
    if (world.is_empty()) return;

    if (!m_impl->spatial_index) {
        m_impl->spatial_index = std::make_unique<Quadtree>(world);
    }

    m_impl->spatial_index->clear();

    for (size_t i = 0; i < m_impl->entities.size(); ++i) {
        const Bounds3d& b = m_impl->entities[i].bounds();
        if (!b.is_empty()) {
            m_impl->spatial_index->insert(static_cast<int32_t>(i), b);
        }
    }
}

SpatialIndex* SceneGraph::spatial_index() {
    return m_impl->spatial_index.get();
}

// ============================================================
// Iteration
// ============================================================

void SceneGraph::for_each_entity(const std::function<void(const EntityVariant&)>& fn) const {
    for (const auto& ent : m_impl->entities) {
        fn(ent);
    }
}

void SceneGraph::for_each_entity_of_type(
    EntityType type,
    const std::function<void(const EntityVariant&)>& fn) const
{
    for (const auto& ent : m_impl->entities) {
        if (ent.header.type == type) {
            fn(ent);
        }
    }
}

// ============================================================
// Utility
// ============================================================

void SceneGraph::clear() {
    m_impl->entities.clear();
    m_impl->vertex_buffer.clear();
    m_impl->layers.clear();
    m_impl->linetypes.clear();
    m_impl->text_styles.clear();
    m_impl->blocks.clear();
    m_impl->viewports.clear();
    m_impl->layouts.clear();
    m_impl->diagnostics.clear();
    m_impl->layer_name_map.clear();
    m_impl->linetype_name_map.clear();
    m_impl->text_style_name_map.clear();
    m_impl->block_name_map.clear();
    m_impl->drawing_info = DrawingMetadata();
    m_impl->spatial_index.reset();
    m_impl->scene_nodes.clear();
    m_impl->node_id_to_index.clear();
    m_impl->next_node_id = 1;
    m_impl->root_node_id = 0;
}

void SceneGraph::reserve(size_t entity_count, size_t vertex_count) {
    m_impl->entities.reserve(entity_count);
    m_impl->vertex_buffer.reserve(vertex_count);
}

void SceneGraph::shrink_to_fit() {
    m_impl->entities.shrink_to_fit();
    m_impl->vertex_buffer.shrink_to_fit();
    m_impl->layers.shrink_to_fit();
    m_impl->linetypes.shrink_to_fit();
    m_impl->text_styles.shrink_to_fit();
    m_impl->blocks.shrink_to_fit();
    m_impl->viewports.shrink_to_fit();
    m_impl->layouts.shrink_to_fit();
}

// ============================================================
// Scene tree (hierarchical overlay)
// ============================================================

void SceneGraph::build_scene_tree() {
    m_impl->scene_nodes.clear();
    m_impl->node_id_to_index.clear();
    m_impl->next_node_id = 1;
    m_impl->root_node_id = 0;

    auto& impl = *m_impl;
    const auto& entities = impl.entities;
    const auto& blocks = impl.blocks;
    const auto& layouts = impl.layouts;

    // Helper lambda: add a node and return its ID
    auto add_node = [&](SceneNodeType type, uint32_t parent_id) -> uint32_t {
        uint32_t id = impl.next_node_id++;
        uint32_t idx = static_cast<uint32_t>(impl.scene_nodes.size());
        impl.scene_nodes.push_back({});
        auto& node = impl.scene_nodes.back();
        node.type = type;
        node.id = id;
        node.parent_id = parent_id;
        impl.node_id_to_index[id] = idx;

        // Register as child of parent
        if (parent_id != kSceneNodeNoParent) {
            auto pit = impl.node_id_to_index.find(parent_id);
            if (pit != impl.node_id_to_index.end()) {
                impl.scene_nodes[pit->second].children.push_back(id);
            }
        }
        return id;
    };

    // Helper lambda: aggregate bounds from entity indices
    auto entity_bounds = [&](const std::vector<uint32_t>& indices) -> Bounds3d {
        Bounds3d b = Bounds3d::empty();
        for (uint32_t ei : indices) {
            if (ei < entities.size()) b.expand(entities[ei].bounds());
        }
        return b;
    };

    // 1. Create root node
    uint32_t root_id = add_node(SceneNodeType::ModelSpace, kSceneNodeNoParent);
    impl.root_node_id = root_id;

    // 2. Create ModelSpace root child
    uint32_t model_space_id = add_node(SceneNodeType::ModelSpace, root_id);

    // 3. Create PaperSpace root child (if any paper space entities exist)
    bool has_paper_space = false;
    for (const auto& ent : entities) {
        if (ent.header.space == DrawingSpace::PaperSpace) {
            has_paper_space = true;
            break;
        }
    }
    uint32_t paper_space_id = kSceneNodeNoParent;
    if (has_paper_space) {
        paper_space_id = add_node(SceneNodeType::PaperSpace, root_id);
    }

    // 4. Create BlockDefinition nodes
    std::vector<uint32_t> block_def_node_ids(blocks.size(), kSceneNodeNoParent);
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const auto& block = blocks[bi];
        if (block.is_model_space || block.is_paper_space) continue;
        if (block.entity_indices.empty()) continue;

        uint32_t node_id = add_node(SceneNodeType::BlockDefinition, root_id);
        auto it = impl.node_id_to_index.find(node_id);
        if (it != impl.node_id_to_index.end()) {
            auto& node = impl.scene_nodes[it->second];
            for (int32_t ei : block.entity_indices) {
                node.entity_indices.push_back(static_cast<uint32_t>(ei));
            }
        }
        block_def_node_ids[bi] = node_id;
    }

    // 5. Create LayoutRoot nodes (children of PaperSpace)
    std::vector<uint32_t> layout_node_ids(layouts.size(), kSceneNodeNoParent);
    for (size_t li = 0; li < layouts.size(); ++li) {
        const auto& layout = layouts[li];
        if (layout.is_model_layout) continue;

        uint32_t parent = (paper_space_id != kSceneNodeNoParent) ? paper_space_id : root_id;
        uint32_t node_id = add_node(SceneNodeType::LayoutRoot, parent);
        auto it = impl.node_id_to_index.find(node_id);
        if (it != impl.node_id_to_index.end()) {
            impl.scene_nodes[it->second].layout_index = static_cast<int32_t>(li);
        }
        layout_node_ids[li] = node_id;
    }

    // 6. Create Viewport nodes (children of LayoutRoot)
    const auto& viewports = impl.viewports;
    for (size_t vi = 0; vi < viewports.size(); ++vi) {
        const auto& vp = viewports[vi];
        uint32_t parent = root_id;
        if (vp.layout_index >= 0 && static_cast<size_t>(vp.layout_index) < layout_node_ids.size()) {
            uint32_t layout_node = layout_node_ids[static_cast<size_t>(vp.layout_index)];
            if (layout_node != kSceneNodeNoParent) parent = layout_node;
        }
        uint32_t node_id = add_node(SceneNodeType::Viewport, parent);
        auto it = impl.node_id_to_index.find(node_id);
        if (it != impl.node_id_to_index.end()) {
            impl.scene_nodes[it->second].viewport_index = static_cast<int32_t>(vi);
        }
    }

    // 7. Assign entities to leaf nodes
    for (size_t ei = 0; ei < entities.size(); ++ei) {
        const auto& entity = entities[ei];
        if (entity.header.in_block) continue;

        // Determine parent node
        uint32_t parent = model_space_id;
        if (entity.header.space == DrawingSpace::PaperSpace) {
            parent = (paper_space_id != kSceneNodeNoParent) ? paper_space_id : root_id;
            if (entity.header.layout_index >= 0 &&
                static_cast<size_t>(entity.header.layout_index) < layout_node_ids.size()) {
                uint32_t layout_node = layout_node_ids[static_cast<size_t>(entity.header.layout_index)];
                if (layout_node != kSceneNodeNoParent) parent = layout_node;
            }
        }

        if (entity.header.type == EntityType::Insert) {
            uint32_t node_id = add_node(SceneNodeType::BlockInstance, parent);
            auto it = impl.node_id_to_index.find(node_id);
            if (it != impl.node_id_to_index.end()) {
                auto& node = impl.scene_nodes[it->second];
                node.entity_indices.push_back(static_cast<uint32_t>(ei));
                Vec3 c = entity.header.bounds.center();
                node.local_transform = Matrix4x4::affine_2d(1.0f, 1.0f, 0.0f, c.x, c.y);
                node.modifiers = entity.header.modifiers;

                auto* ins = std::get_if<InsertEntity>(&entity.data);
                if (ins && ins->block_index >= 0 &&
                    static_cast<size_t>(ins->block_index) < block_def_node_ids.size()) {
                    node.block_def_id = block_def_node_ids[static_cast<size_t>(ins->block_index)];
                }
            }
        } else {
            // Regular entity leaf node
            uint32_t node_id = add_node(SceneNodeType::Entity, parent);
            auto it = impl.node_id_to_index.find(node_id);
            if (it != impl.node_id_to_index.end()) {
                auto& node = impl.scene_nodes[it->second];
                node.entity_indices.push_back(static_cast<uint32_t>(ei));
                node.modifiers = entity.header.modifiers;
            }
        }
    }

    // 8. Aggregate bounds bottom-up using a recursive lambda
    std::function<void(uint32_t)> agg_bounds = [&](uint32_t node_id) {
        auto it = impl.node_id_to_index.find(node_id);
        if (it == impl.node_id_to_index.end()) return;
        auto& node = impl.scene_nodes[it->second];

        Bounds3d bounds = entity_bounds(node.entity_indices);
        for (uint32_t child_id : node.children) {
            agg_bounds(child_id);
            auto cit = impl.node_id_to_index.find(child_id);
            if (cit != impl.node_id_to_index.end()) {
                bounds.expand(impl.scene_nodes[cit->second].world_bounds);
            }
        }
        node.world_bounds = bounds;
    };
    agg_bounds(root_id);
}

const std::vector<SceneNode>& SceneGraph::scene_nodes() const {
    return m_impl->scene_nodes;
}

uint32_t SceneGraph::scene_root_id() const {
    return m_impl->root_node_id;
}

const SceneNode* SceneGraph::find_node(uint32_t node_id) const {
    auto it = m_impl->node_id_to_index.find(node_id);
    if (it == m_impl->node_id_to_index.end()) return nullptr;
    return &m_impl->scene_nodes[it->second];
}

std::vector<int32_t> SceneGraph::subtree_entity_indices(uint32_t node_id) const {
    std::vector<int32_t> result;
    const SceneNode* node = find_node(node_id);
    if (!node) return result;

    // Collect from this node
    for (uint32_t ei : node->entity_indices) {
        result.push_back(static_cast<int32_t>(ei));
    }
    // Recurse into children
    for (uint32_t child_id : node->children) {
        auto child_result = subtree_entity_indices(child_id);
        result.insert(result.end(), child_result.begin(), child_result.end());
    }
    return result;
}

bool SceneGraph::is_node_visible(uint32_t node_id) const {
    const SceneNode* node = find_node(node_id);
    if (!node) return false;
    if (!node->visible) return false;
    if (node->parent_id != kSceneNodeNoParent) {
        return is_node_visible(node->parent_id);
    }
    return true;
}

} // namespace cad
