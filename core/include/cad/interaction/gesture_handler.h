#pragma once
#include "cad/cad_types.h"

namespace cad {

struct GestureInput {
    enum Type { PanStart, PanMove, PanEnd, Zoom, PinchStart, PinchMove, PinchEnd, Tap, DoubleTap };
    Type type;
    float x = 0, y = 0;
    float dx = 0, dy = 0;
    float scale = 1.0f;
    int button = 0;
};

class GestureHandler {
public:
    GestureInput process(const GestureInput& raw_input);
    void set_pan_sensitivity(float s);
    void set_zoom_sensitivity(float s);
private:
    float m_pan_sensitivity = 1.0f;
    float m_zoom_sensitivity = 1.0f;
};
} // namespace cad
