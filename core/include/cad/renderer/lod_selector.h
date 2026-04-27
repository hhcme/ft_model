#pragma once

#include "cad/cad_types.h"
#include <array>

namespace cad {

// ============================================================
// LodLevel — pre-computed segment count for a given LOD tier
// Inspired by HOOPS 3-level mesh (standard / low / extra-low).
// ============================================================
struct LodLevel {
    uint8_t  level;     // 0 = standard, 1 = low, 2 = minimal
    uint16_t segments;  // segment count for this level
};

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

    // Multi-level LOD: pre-compute segment counts for 3 LOD tiers.
    // Level 0 = standard (chord-height formula unchanged)
    // Level 1 = low precision (max(N/4, 6))
    // Level 2 = minimal (max(N/8, 4)) — outline only
    static std::array<LodLevel, 3> compute_lod_levels(float radius, float pixels_per_unit);

    static constexpr int k_min_segments = 48;
    static constexpr int k_max_segments = 512;
    static constexpr int k_low_min_segments = 6;     // minimum segments for LOD level 1
    static constexpr int k_minimal_min_segments = 4;  // minimum segments for LOD level 2
};

} // namespace cad
