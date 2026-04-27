#pragma once

// Internal header shared between render_batcher.cpp and render_batcher_curve.cpp.
// NOT part of the public API.

#include "cad/cad_types.h"
#include "cad/renderer/render_batcher.h"
#include "cad/scene/entity.h"

#include <cmath>
#include <vector>

namespace cad {
namespace batcher {

constexpr float kMaxRenderableCoord = 1.0e8f;

inline bool is_renderable_coord(float x, float y) {
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) <= kMaxRenderableCoord &&
           std::abs(y) <= kMaxRenderableCoord;
}

inline bool append_vertex(std::vector<float>& vertex_data, float x, float y) {
    if (!is_renderable_coord(x, y)) return false;
    vertex_data.push_back(x);
    vertex_data.push_back(y);
    return true;
}

inline bool is_renderable_point(const Vec3& pt) {
    return is_renderable_coord(pt.x, pt.y);
}

inline float distance_xy(const Vec3& a, const Vec3& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline Vec3 lerp_vec3(const Vec3& a, const Vec3& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

Vec3 catmull_rom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                 float t);

bool valid_knot_vector(const std::vector<float>& knots, size_t control_count, int degree);

int find_spline_span(const std::vector<float>& knots, int control_count, int degree,
                     float u);

Vec3 evaluate_bspline(const SplineEntity& spline, float u);

std::vector<Vec3> tessellate_spline_points(const SplineEntity& spline, float ppu);

bool same_pattern(const LinePattern& a, const LinePattern& b);

void split_line_strip_coordinate_jumps(RenderBatch& batch);

bool point_in_polygon(float x, float y, const float* poly_x, const float* poly_y, int n);

} // namespace batcher
} // namespace cad
