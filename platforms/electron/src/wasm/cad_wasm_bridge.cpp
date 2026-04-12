#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include "cad/cad_engine.h"

using namespace emscripten;

// Wrapper to accept JS Uint8Array and forward to load_buffer
void loadBufferWrapper(cad::CadEngine& engine, val data) {
    auto bytes = convertJSArrayToNumberVector<uint8_t>(data);
    engine.load_buffer(bytes.data(), bytes.size(), "dxf");
}

// Wrapper for load_file (Emscripten has a virtual filesystem)
int loadFileWrapper(cad::CadEngine& engine, const std::string& filepath) {
    auto result = engine.load_file(filepath);
    return result.ok() ? 0 : static_cast<int>(result.code);
}

// Wrapper for initialize — accepts a JS canvas element via emscripten::val
int initializeWrapper(cad::CadEngine& engine, val canvas, int width, int height) {
    // Use #canvas or the actual CSS selector Emscripten expects
    // The simplest approach: pass a module.canvasOverrideId or use the default
    (void)canvas; // Emscripten's WebGL uses the default canvas unless overridden
    auto result = engine.initialize(nullptr, width, height);
    return result.ok() ? 0 : static_cast<int>(result.code);
}

// Wrapper for fit_to_extents with default margin
void fitToExtentsWrapper(cad::CadEngine& engine) {
    engine.fit_to_extents();
}

// Wrapper for get_drawing_info — returns a plain JS object
val getDrawingInfoWrapper(cad::CadEngine& engine) {
    auto info = engine.get_drawing_info();
    auto obj = val::object();
    obj.set("filename", info.filename);
    obj.set("layerCount", info.layer_count);
    obj.set("entityCount", info.entity_count);
    obj.set("acadVersion", info.acad_version);

    auto extents = val::object();
    auto minPt = val::object();
    minPt.set("x", info.extents.min.x);
    minPt.set("y", info.extents.min.y);
    minPt.set("z", info.extents.min.z);
    auto maxPt = val::object();
    maxPt.set("x", info.extents.max.x);
    maxPt.set("y", info.extents.max.y);
    maxPt.set("z", info.extents.max.z);
    extents.set("min", minPt);
    extents.set("max", maxPt);
    obj.set("extents", extents);

    return obj;
}

// Wrapper for get_viewport_state
val getViewportStateWrapper(cad::CadEngine& engine) {
    auto state = engine.get_viewport_state();
    auto obj = val::object();

    auto center = val::object();
    center.set("x", state.center.x);
    center.set("y", state.center.y);
    center.set("z", state.center.z);
    obj.set("center", center);
    obj.set("zoom", state.zoom);
    obj.set("width", state.width);
    obj.set("height", state.height);

    return obj;
}

// Wrapper for get_layers — returns array of JS objects
val getLayersWrapper(cad::CadEngine& engine) {
    auto layers = engine.get_layers();
    auto arr = val::array();
    for (const auto& layer : layers) {
        auto obj = val::object();
        obj.set("name", layer.name);
        obj.set("isVisible", layer.is_visible);
        obj.set("isFrozen", layer.is_frozen);
        obj.set("isLocked", layer.is_locked);
        obj.set("entityCount", layer.entity_count);
        arr.call<void>("push", obj);
    }
    return arr;
}

EMSCRIPTEN_BINDINGS(cad_engine) {
    class_<cad::CadEngine>("CadEngine")
        .constructor<>()
        .function("initialize", &initializeWrapper)
        .function("loadBuffer", &loadBufferWrapper)
        .function("loadFile", &loadFileWrapper)
        .function("renderFrame", &cad::CadEngine::render_frame)
        .function("pan", &cad::CadEngine::pan)
        .function("zoom", &cad::CadEngine::zoom)
        .function("fitToExtents", &fitToExtentsWrapper)
        .function("resize", &cad::CadEngine::resize)
        .function("closeFile", &cad::CadEngine::close_file)
        .function("shutdown", &cad::CadEngine::shutdown)
        .function("getDrawingInfo", &getDrawingInfoWrapper)
        .function("getViewportState", &getViewportStateWrapper)
        .function("getLayers", &getLayersWrapper)
        .function("setLayerVisibility", &cad::CadEngine::set_layer_visibility)
        ;

    value_object<cad::ViewportState>("ViewportState")
        .field("center", &cad::ViewportState::center)
        .field("zoom", &cad::ViewportState::zoom)
        .field("width", &cad::ViewportState::width)
        .field("height", &cad::ViewportState::height)
        ;

    value_object<cad::Vec3>("Vec3")
        .field("x", &cad::Vec3::x)
        .field("y", &cad::Vec3::y)
        .field("z", &cad::Vec3::z)
        ;
}

#endif // __EMSCRIPTEN__
