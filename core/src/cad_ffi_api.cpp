#include "cad/cad_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// C API boundary for Flutter FFI and other language bindings
// Stable ABI — no name mangling issues

void* cad_engine_create() {
    return new cad::CadEngine();
}

void cad_engine_destroy(void* engine) {
    delete static_cast<cad::CadEngine*>(engine);
}

int cad_engine_initialize(void* engine, void* native_handle, int width, int height) {
    auto* e = static_cast<cad::CadEngine*>(engine);
    auto result = e->initialize(native_handle, width, height);
    return result.ok() ? 0 : static_cast<int>(result.code);
}

int cad_engine_load_dxf(void* engine, const char* filepath) {
    auto* e = static_cast<cad::CadEngine*>(engine);
    auto result = e->load_file(filepath);
    return result.ok() ? 0 : static_cast<int>(result.code);
}

int cad_engine_load_dxf_buffer(void* engine, const uint8_t* data, size_t size) {
    auto* e = static_cast<cad::CadEngine*>(engine);
    auto result = e->load_buffer(data, size);
    return result.ok() ? 0 : static_cast<int>(result.code);
}

void cad_engine_render_frame(void* engine) {
    static_cast<cad::CadEngine*>(engine)->render_frame();
}

void cad_engine_pan_camera(void* engine, float dx, float dy) {
    static_cast<cad::CadEngine*>(engine)->pan(dx, dy);
}

void cad_engine_zoom_camera(void* engine, float factor, float px, float py) {
    static_cast<cad::CadEngine*>(engine)->zoom(factor, px, py);
}

void cad_engine_fit_to_extents(void* engine) {
    static_cast<cad::CadEngine*>(engine)->fit_to_extents();
}

void cad_engine_resize_viewport(void* engine, int width, int height) {
    static_cast<cad::CadEngine*>(engine)->resize(width, height);
}

int64_t cad_engine_pick_entity(void* engine, float screen_x, float screen_y) {
    return static_cast<cad::CadEngine*>(engine)->pick_entity(screen_x, screen_y);
}

void cad_engine_get_extents(void* engine, float* out_min_x, float* out_min_y,
                             float* out_max_x, float* out_max_y) {
    auto info = static_cast<cad::CadEngine*>(engine)->get_drawing_info();
    if (out_min_x) *out_min_x = info.extents.min.x;
    if (out_min_y) *out_min_y = info.extents.min.y;
    if (out_max_x) *out_max_x = info.extents.max.x;
    if (out_max_y) *out_max_y = info.extents.max.y;
}

void cad_engine_shutdown(void* engine) {
    static_cast<cad::CadEngine*>(engine)->shutdown();
}

// Command buffer access for Flutter rendering
const uint8_t* cad_engine_get_command_buffer(void* engine) {
    return static_cast<cad::CadEngine*>(engine)->get_command_buffer();
}

size_t cad_engine_get_command_buffer_size(void* engine) {
    return static_cast<cad::CadEngine*>(engine)->get_command_buffer_size();
}

#ifdef __cplusplus
}
#endif
