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
const std::vector<Layout>& SceneGraph::layouts() const { return m_impl->layouts; }
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
    auto has_layout_entities = [&](int32_t layout_index) {
        for (const auto& ent : m_impl->entities) {
            if (ent.header.layout_index == layout_index) {
                return true;
            }
        }
        return false;
    };
    for (const auto& layout : m_impl->layouts) {
        const int32_t layout_index = static_cast<int32_t>(&layout - m_impl->layouts.data());
        if (layout.is_active && !layout.is_model_layout &&
            has_presentation_bounds(layout) && has_layout_entities(layout_index)) {
            return &layout;
        }
    }
    for (const auto& layout : m_impl->layouts) {
        const int32_t layout_index = static_cast<int32_t>(&layout - m_impl->layouts.data());
        if (!layout.is_model_layout && has_presentation_bounds(layout) &&
            has_layout_entities(layout_index)) {
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

} // namespace cad
