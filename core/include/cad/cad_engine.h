#pragma once

#include "cad/cad_errors.h"
#include "cad/cad_types.h"
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace cad {

class SceneGraph;
class Camera;
class RenderBatcher;
class SpatialIndex;
class SelectionEngine;

// ============================================================
// CadEngine — Public API facade
// ============================================================

struct DrawingInfo {
    std::string filename;
    Bounds3d extents;
    int layer_count = 0;
    int entity_count = 0;
    std::string acad_version;
};

struct LayerInfo {
    std::string name;
    Color color;
    bool is_visible = true;
    bool is_frozen = false;
    bool is_locked = false;
    int entity_count = 0;
};

struct ViewportState {
    Vec3 center;
    float zoom;
    int width;
    int height;
};

// Callback types
using EngineProgressCallback = std::function<void(float progress, const std::string& stage)>;
using SelectionCallback = std::function<void(int64_t entity_id)>;

class CadEngine {
public:
    CadEngine();
    ~CadEngine();

    // Non-copyable
    CadEngine(const CadEngine&) = delete;
    CadEngine& operator=(const CadEngine&) = delete;

    // ---- Lifecycle ----

    // Initialize with a native window handle (platform-specific)
    // For WASM: pass canvas element handle
    // For Flutter: pass null (uses command buffer instead)
    Result initialize(void* native_handle, int width, int height);
    void shutdown();

    // ---- File Loading ----

    Result load_file(const std::string& filepath, EngineProgressCallback progress = nullptr);
    Result load_buffer(const uint8_t* data, size_t size,
                       const std::string& format_hint = "", // "dxf" or "dwg"
                       EngineProgressCallback progress = nullptr);
    void close_file();

    // ---- View Controls ----

    void fit_to_extents(float margin = 0.05f);
    void pan(float dx, float dy);
    void zoom(float factor, float pivot_x, float pivot_y);
    void set_center(float x, float y);
    void set_zoom(float zoom_level);

    ViewportState get_viewport_state() const;
    void set_viewport_state(const ViewportState& state);

    // Screen <-> World coordinate conversion
    Vec2 screen_to_world(float screen_x, float screen_y) const;
    Vec3 world_to_screen(float world_x, float world_y) const;

    // ---- Rendering ----

    void render_frame();
    void resize(int width, int height);

    // ---- Layer Management ----

    std::vector<LayerInfo> get_layers() const;
    void set_layer_visibility(const std::string& name, bool visible);

    // ---- Selection ----

    int64_t pick_entity(float screen_x, float screen_y);
    std::vector<int64_t> box_select(float x1, float y1, float x2, float y2);
    std::vector<int64_t> get_selected_entities() const;
    void clear_selection();

    // ---- Callbacks ----

    void set_selection_callback(SelectionCallback callback);

    // ---- Drawing Info ----

    DrawingInfo get_drawing_info() const;

    // ---- Platform-specific ----

    // For Flutter: get the render command buffer
    const uint8_t* get_command_buffer() const;
    size_t get_command_buffer_size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cad
