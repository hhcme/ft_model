#include "cad/cad_engine.h"
#include "cad/scene/scene_graph.h"
#include "cad/renderer/camera.h"
#include "cad/renderer/render_batcher.h"
#include "cad/renderer/render_command.h"
#include "cad/renderer/frustum_culler.h"
#include "cad/scene/spatial_index.h"
#include "cad/parser/dxf_parser.h"

namespace cad {

struct CadEngine::Impl {
    std::unique_ptr<SceneGraph> scene;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<RenderBatcher> batcher;
    std::unique_ptr<RenderCommandBuffer> command_buffer;
    std::unique_ptr<SpatialIndex> spatial_index;

    std::vector<int64_t> selected_entities;
    SelectionCallback selection_callback;
    bool initialized = false;
    int viewport_width = 0;
    int viewport_height = 0;
};

CadEngine::CadEngine()
    : m_impl(std::make_unique<Impl>())
{
    m_impl->scene = std::make_unique<SceneGraph>();
    m_impl->camera = std::make_unique<Camera>();
    m_impl->batcher = std::make_unique<RenderBatcher>();
    m_impl->command_buffer = std::make_unique<RenderCommandBuffer>();
}

CadEngine::~CadEngine() = default;

Result CadEngine::initialize(void* native_handle, int width, int height) {
    m_impl->viewport_width = width;
    m_impl->viewport_height = height;
    m_impl->camera->set_viewport(width, height);
    m_impl->initialized = true;
    return Result::success();
}

void CadEngine::shutdown() {
    m_impl->initialized = false;
}

Result CadEngine::load_file(const std::string& filepath, EngineProgressCallback progress) {
    DxfParser parser;
    auto result = parser.parse_file(filepath, *m_impl->scene,
        [progress](const ParseProgress& p) {
            if (progress) {
                float pct = p.total_entities_estimate > 0
                    ? static_cast<float>(p.entities_parsed) / static_cast<float>(p.total_entities_estimate)
                    : 0.0f;
                progress(pct, p.current_section);
            }
        });
    if (!result.ok()) return result;

    m_impl->scene->rebuild_spatial_index();
    fit_to_extents();
    return Result::success();
}

Result CadEngine::load_buffer(const uint8_t* data, size_t size,
                               const std::string& format_hint,
                               EngineProgressCallback progress) {
    DxfParser parser;
    auto result = parser.parse_buffer(data, size, *m_impl->scene);
    if (!result.ok()) return result;

    m_impl->scene->rebuild_spatial_index();
    fit_to_extents();
    return Result::success();
}

void CadEngine::close_file() {
    m_impl->scene = std::make_unique<SceneGraph>();
    m_impl->selected_entities.clear();
}

void CadEngine::fit_to_extents(float margin) {
    auto info = get_drawing_info();
    if (info.extents.is_empty()) return;
    m_impl->camera->fit_to_bounds(info.extents, margin);
}

void CadEngine::pan(float dx, float dy) {
    m_impl->camera->pan(dx, dy);
}

void CadEngine::zoom(float factor, float pivot_x, float pivot_y) {
    m_impl->camera->zoom(factor, pivot_x, pivot_y);
}

void CadEngine::set_center(float x, float y) {
    m_impl->camera->set_center({x, y, 0});
}

void CadEngine::set_zoom(float zoom_level) {
    m_impl->camera->set_zoom(zoom_level);
}

ViewportState CadEngine::get_viewport_state() const {
    ViewportState state;
    // TODO: extract from camera
    state.zoom = m_impl->camera->zoom_level();
    state.width = m_impl->viewport_width;
    state.height = m_impl->viewport_height;
    return state;
}

void CadEngine::set_viewport_state(const ViewportState& state) {
    m_impl->camera->set_center(state.center);
    m_impl->camera->set_zoom(state.zoom);
    m_impl->camera->set_viewport(state.width, state.height);
}

Vec2 CadEngine::screen_to_world(float screen_x, float screen_y) const {
    return m_impl->camera->screen_to_world({screen_x, screen_y});
}

Vec3 CadEngine::world_to_screen(float world_x, float world_y) const {
    return m_impl->camera->world_to_screen({world_x, world_y, 0});
}

void CadEngine::render_frame() {
    m_impl->command_buffer->clear();
    if (!m_impl->scene || !m_impl->camera || !m_impl->batcher) return;

    // 1. Begin frame on the batcher
    m_impl->batcher->begin_frame(*m_impl->camera);

    // 2. Get camera visible bounds and query visible entities
    Bounds3d visible_bounds = m_impl->camera->visible_bounds();
    std::vector<int32_t> visible_indices = m_impl->scene->entities_in_bounds(visible_bounds);

    // If spatial query returns nothing (no spatial index), fall back to all entities
    const auto& all_entities = m_impl->scene->entities();
    if (visible_indices.empty() && !all_entities.empty()) {
        visible_indices.reserve(all_entities.size());
        for (int32_t i = 0; i < static_cast<int32_t>(all_entities.size()); ++i) {
            visible_indices.push_back(i);
        }
    }

    // 3. Submit each visible entity to the batcher
    for (int32_t idx : visible_indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= all_entities.size()) continue;
        const auto& entity = all_entities[static_cast<size_t>(idx)];
        if (!entity.is_visible()) continue;
        m_impl->batcher->submit_entity(entity, *m_impl->scene);
    }

    // 4. End frame (sorts batches by render key)
    m_impl->batcher->end_frame();

    // 5. Build command buffer from sorted batches
    const auto& batches = m_impl->batcher->batches();
    for (const auto& batch : batches) {
        if (batch.vertex_data.empty()) continue;

        m_impl->command_buffer->set_color(batch.color);
        m_impl->command_buffer->set_line_width(batch.line_width);

        int vertex_count = static_cast<int>(batch.vertex_data.size() / 2);
        const Vec2* verts = reinterpret_cast<const Vec2*>(batch.vertex_data.data());
        m_impl->command_buffer->draw_lines(verts, vertex_count);
    }
}

void CadEngine::resize(int width, int height) {
    m_impl->viewport_width = width;
    m_impl->viewport_height = height;
    m_impl->camera->set_viewport(width, height);
}

std::vector<LayerInfo> CadEngine::get_layers() const {
    std::vector<LayerInfo> result;
    if (!m_impl->scene) return result;

    const auto& layers = m_impl->scene->layers();
    result.reserve(layers.size());

    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        LayerInfo info;
        info.name = layer.name;
        info.color = layer.color;
        info.is_visible = !layer.is_off && !layer.is_frozen;
        info.is_frozen = layer.is_frozen;
        info.is_locked = layer.is_locked;

        // Count entities on this layer
        auto on_layer = m_impl->scene->entities_on_layer(static_cast<int32_t>(i));
        info.entity_count = static_cast<int>(on_layer.size());

        result.push_back(std::move(info));
    }
    return result;
}

void CadEngine::set_layer_visibility(const std::string& name, bool visible) {
    // TODO: toggle layer visibility
}

int64_t CadEngine::pick_entity(float screen_x, float screen_y) {
    // TODO: implement selection
    return -1;
}

std::vector<int64_t> CadEngine::box_select(float x1, float y1, float x2, float y2) {
    // TODO: implement box selection
    return {};
}

std::vector<int64_t> CadEngine::get_selected_entities() const {
    return m_impl->selected_entities;
}

void CadEngine::clear_selection() {
    m_impl->selected_entities.clear();
}

void CadEngine::set_selection_callback(SelectionCallback callback) {
    m_impl->selection_callback = std::move(callback);
}

DrawingInfo CadEngine::get_drawing_info() const {
    DrawingInfo info;
    if (!m_impl->scene) return info;

    const auto& meta = m_impl->scene->drawing_info();
    info.filename = meta.filename;
    info.extents = meta.extents;
    info.acad_version = meta.acad_version;
    info.layer_count = static_cast<int>(m_impl->scene->layers().size());
    info.entity_count = static_cast<int>(m_impl->scene->total_entity_count());

    // If extents from metadata are empty, compute from scene
    if (info.extents.is_empty()) {
        info.extents = m_impl->scene->total_bounds();
    }

    return info;
}

const uint8_t* CadEngine::get_command_buffer() const {
    return m_impl->command_buffer->data();
}

size_t CadEngine::get_command_buffer_size() const {
    return m_impl->command_buffer->size();
}

} // namespace cad
