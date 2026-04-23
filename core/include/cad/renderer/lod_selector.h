#pragma once

#include "cad/cad_types.h"

namespace cad {

class LodSelector {
public:
    // Compute the number of segments to use when tessellating a circle.
    // Returns a value clamped to [k_min_segments, k_max_segments].
    static int compute_circle_segments(float radius, float pixels_per_unit);

    // Compute the number of segments to use when tessellating an arc.
    // arc_angle is in radians.
    static int compute_arc_segments(float radius, float arc_angle, float pixels_per_unit);

    // Compute the number of segments for a spline with the given control point count.
    static int compute_spline_segments(int num_control_points, float pixels_per_unit);

    static constexpr int k_min_segments = 8;
    static constexpr int k_max_segments = 512;
};

} // namespace cad
