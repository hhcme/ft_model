#include "cad/renderer/lod_selector.h"
#include "cad/cad_types.h"
#include <cmath>

namespace cad {

// Chord-height error tolerance in pixels.
// The sagitta (perpendicular distance from chord midpoint to arc) will be
// at most this many pixels for the computed segment count.
constexpr float kChordHeightTolerancePx = 0.5f;

int LodSelector::compute_circle_segments(float radius, float pixels_per_unit) {
    if (radius <= 0.0f || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }

    // Chord-height error formula for a full circle:
    //   sagitta = R * (1 - cos(pi / N)) <= T / ppu
    // Solving for N:
    //   cos(pi / N) >= 1 - T / (R * ppu)
    //   N >= pi / arccos(1 - T / (R * ppu))
    //
    // Where T = pixel tolerance, R = radius, ppu = pixels per world unit.
    float r_ppu = radius * pixels_per_unit;

    // For very small arcs (less than ~3 pixels radius), minimum segments suffice
    if (r_ppu < 3.0f) {
        return k_min_segments;
    }

    float ratio = kChordHeightTolerancePx / r_ppu;

    // Clamp ratio to valid range for arccos: argument must be in [-1, 1].
    // ratio > 2 means the tolerance is larger than the diameter — 3 segments suffice.
    if (ratio >= 2.0f) return 3;

    // ratio must be <= 2 for the formula to work; also ensure argument >= -1
    float cos_arg = 1.0f - ratio;
    if (cos_arg <= -1.0f) return k_max_segments;
    if (cos_arg >= 1.0f) return k_min_segments;

    float N = math::PI / std::acos(cos_arg);
    int segments = static_cast<int>(std::ceil(N));
    return math::clamp(segments, k_min_segments, k_max_segments);
}

int LodSelector::compute_arc_segments(float radius, float arc_angle, float pixels_per_unit) {
    if (radius <= 0.0f || arc_angle <= 0.0f || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }

    // Chord-height error formula for an arc of angle alpha:
    //   sagitta = R * (1 - cos(alpha / (2*N))) <= T / ppu
    // Solving for N:
    //   N >= alpha / (2 * arccos(1 - T / (R * ppu)))
    float r_ppu = radius * pixels_per_unit;

    if (r_ppu < 3.0f) {
        return k_min_segments;
    }

    float ratio = kChordHeightTolerancePx / r_ppu;
    if (ratio >= 2.0f) return 2;

    float cos_arg = 1.0f - ratio;
    if (cos_arg <= -1.0f) return k_max_segments;
    if (cos_arg >= 1.0f) return k_min_segments;

    float half_angle_per_seg = std::acos(cos_arg);
    float N = arc_angle / (2.0f * half_angle_per_seg);
    int segments = static_cast<int>(std::ceil(N));
    return math::clamp(segments, k_min_segments, k_max_segments);
}

int LodSelector::compute_spline_segments(int num_control_points, float pixels_per_unit) {
    if (num_control_points <= 0 || pixels_per_unit <= 0.0f) {
        return k_min_segments;
    }
    // Chord-height error: assume average edge length as curvature proxy.
    // For N control points spanning chord C, edge ~ C/N.
    //   samples >= C / sqrt(8 * (C/N) * (T/ppu))
    // Simplified:  samples >= N * sqrt(C * ppu / (8 * T))
    // With T = 0.5px: samples ~ N * sqrt(C_ppu / 4)
    float effective_ppu = pixels_per_unit;
    int segments = static_cast<int>(
        num_control_points * std::sqrt(std::max(effective_ppu * 0.25f, 1.0f)));
    return math::clamp(segments, k_min_segments, k_max_segments);
}

} // namespace cad
