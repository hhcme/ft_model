#include "cad/renderer/lod_selector.h"
#include "cad/cad_types.h"

namespace cad {

int LodSelector::compute_circle_segments(float radius, float pixels_per_unit) {
    if (radius <= 0.0f || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }
    // Circumference in pixels determines segment count.
    // Aim for ~2 pixel deviation max (approximate).
    float circumference_pixels = math::TWO_PI * radius * pixels_per_unit;
    int segments = static_cast<int>(circumference_pixels / 8.0f);
    return math::clamp(segments, k_min_segments, k_max_segments);
}

int LodSelector::compute_arc_segments(float radius, float arc_angle, float pixels_per_unit) {
    if (radius <= 0.0f || arc_angle <= 0.0f || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }
    // Proportional to full circle segments based on arc angle fraction.
    float fraction = arc_angle / math::TWO_PI;
    int circle_segs = compute_circle_segments(radius, pixels_per_unit);
    int segments = static_cast<int>(circle_segs * fraction);
    return math::clamp(segments, k_min_segments, k_max_segments);
}

int LodSelector::compute_spline_segments(int num_control_points, float pixels_per_unit) {
    if (num_control_points <= 0 || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }
    // More control points and higher zoom => more segments.
    int segments = static_cast<int>(num_control_points * pixels_per_unit * 2.0f);
    return math::clamp(segments, k_min_segments, k_max_segments);
}

} // namespace cad
