#include "cad/renderer/camera.h"

namespace cad {

Camera::Camera()
    : m_center(0.0f, 0.0f, 0.0f)
    , m_zoom(1.0f)
    , m_viewport_width(800.0f)
    , m_viewport_height(600.0f) {}

void Camera::pan(float dx, float dy) {
    m_center.x += dx;
    m_center.y += dy;
}

void Camera::zoom(float factor, float pivot_x, float pivot_y) {
    // Convert pivot from screen space to world space before zoom change
    Vec2 pivot_world = screen_to_world({pivot_x, pivot_y});

    float new_zoom = m_zoom / factor;
    // Clamp zoom to reasonable range
    new_zoom = math::clamp(new_zoom, 0.0001f, 100000.0f);
    m_zoom = new_zoom;

    // After zoom change, the same world point should still be under the pivot
    // Recompute where pivot maps to now, then adjust center
    Vec2 new_pivot_world = screen_to_world({pivot_x, pivot_y});
    m_center.x += pivot_world.x - new_pivot_world.x;
    m_center.y += pivot_world.y - new_pivot_world.y;
}

void Camera::fit_to_bounds(const Bounds3d& bounds, float margin) {
    if (bounds.is_empty()) return;

    float bw = bounds.width();
    float bh = bounds.height();
    if (bw <= 0.0f || bh <= 0.0f) return;

    // Apply margin (inflation factor)
    bw *= (1.0f + margin);
    bh *= (1.0f + margin);

    // Compute zoom to fit bounds in viewport
    float zoom_x = bw / m_viewport_width;
    float zoom_y = bh / m_viewport_height;
    m_zoom = std::max(zoom_x, zoom_y);

    // Center on bounds center
    Vec3 c = bounds.center();
    m_center = c;
}

void Camera::set_center(const Vec3& center) {
    m_center = center;
}

void Camera::set_zoom(float zoom) {
    m_zoom = math::clamp(zoom, 0.0001f, 100000.0f);
}

void Camera::set_viewport(float width, float height) {
    m_viewport_width = width;
    m_viewport_height = height;
}

Matrix4x4 Camera::view_matrix() const {
    // View matrix: scale by 1/zoom, then translate by -center
    float inv_zoom = 1.0f / m_zoom;
    // Build as: Scale * Translation(-center)
    Matrix4x4 scale = Matrix4x4::scale_2d(inv_zoom, inv_zoom);
    Matrix4x4 translate = Matrix4x4::translation_2d(-m_center.x, -m_center.y);
    return scale * translate;
}

Matrix4x4 Camera::inverse_view_matrix() const {
    // Inverse: translate by +center, then scale by zoom
    Matrix4x4 translate = Matrix4x4::translation_2d(m_center.x, m_center.y);
    Matrix4x4 scale = Matrix4x4::scale_2d(m_zoom, m_zoom);
    return translate * scale;
}

Vec2 Camera::screen_to_world(const Vec2& screen_pos) const {
    // Convert screen coords to normalized [0..1], then to world
    float nx = screen_pos.x / m_viewport_width;
    float ny = screen_pos.y / m_viewport_height;

    float world_x = m_center.x + (nx - 0.5f) * m_viewport_width * m_zoom;
    float world_y = m_center.y + (ny - 0.5f) * m_viewport_height * m_zoom;
    return {world_x, world_y};
}

Vec2 Camera::world_to_screen(const Vec3& world_pos) const {
    float screen_x = (world_pos.x - m_center.x) / m_zoom + m_viewport_width * 0.5f;
    float screen_y = (world_pos.y - m_center.y) / m_zoom + m_viewport_height * 0.5f;
    return {screen_x, screen_y};
}

Bounds3d Camera::visible_bounds() const {
    float half_w = m_viewport_width * 0.5f * m_zoom;
    float half_h = m_viewport_height * 0.5f * m_zoom;
    return {
        {m_center.x - half_w, m_center.y - half_h, 0.0f},
        {m_center.x + half_w, m_center.y + half_h, 0.0f}
    };
}

float Camera::zoom_level() const {
    return m_zoom;
}

float Camera::pixels_per_unit() const {
    return 1.0f / m_zoom;
}

} // namespace cad
