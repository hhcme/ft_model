#pragma once

#include "cad/cad_types.h"

namespace cad {

class Camera {
public:
    Camera();

    // -- Navigation (2D only) --
    void pan(float dx, float dy);
    void zoom(float factor, float pivot_x, float pivot_y);
    void fit_to_bounds(const Bounds3d& bounds, float margin = 0.1f);

    // -- Setters --
    void set_center(const Vec3& center);
    void set_zoom(float zoom);
    void set_viewport(float width, float height);

    // -- Transforms --
    Matrix4x4 view_matrix() const;
    Matrix4x4 inverse_view_matrix() const;
    Vec2 screen_to_world(const Vec2& screen_pos) const;
    Vec2 world_to_screen(const Vec3& world_pos) const;

    // -- Query --
    Bounds3d visible_bounds() const;
    float zoom_level() const;
    float pixels_per_unit() const;

    // -- Accessors --
    const Vec3& center() const { return m_center; }
    float zoom() const { return m_zoom; }
    float viewport_width() const { return m_viewport_width; }
    float viewport_height() const { return m_viewport_height; }

private:
    Vec3 m_center;
    float m_zoom; // World units per pixel
    float m_viewport_width;
    float m_viewport_height;
};

} // namespace cad
