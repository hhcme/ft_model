#pragma once

#include "cad/parser/dwg_reader.h"
#include "cad/cad_types.h"
#include "cad/scene/entity.h"

#include <unordered_map>
#include <vector>

namespace cad {

// ============================================================
// Shared utilities for DWG entity sub-modules.
//
// Moved from the anonymous namespace in dwg_objects.cpp so that
// geometry, annotation, hatch, and insert parsers can all use
// them without duplicating code.
// ============================================================

// ============================================================
// Diagnostic counters — defined in dwg_objects.cpp
// ============================================================
extern std::unordered_map<uint32_t, size_t> g_success_counts;
extern std::unordered_map<uint32_t, size_t> g_dispatch_counts;

// ============================================================
// Pending POLYLINE_2D / VERTEX_2D / SEQEND assembly state
// ============================================================
struct PendingPolyline2d {
    bool active = false;
    bool closed = false;
    EntityHeader header;
    std::vector<Vec3> vertices;
    std::vector<float> bulges;
};

extern PendingPolyline2d g_pending_polyline2d;

// ============================================================
// Helper: construct EntityVariant with header and in_place_index data.
// Must use std::in_place_index<N> because EntityData contains
// duplicate types (LineEntity at 0/13/14, CircleEntity at 1/12, etc.)
// ============================================================
template<size_t I, typename T>
inline EntityVariant make_entity(EntityHeader hdr, T data) {
    return EntityVariant{hdr, EntityData{std::in_place_index<I>, std::move(data)}};
}

// ============================================================
// Helper: check reader for error after reads, return true if OK
// ============================================================
inline bool reader_ok(const DwgBitReader& reader) {
    return !reader.has_error();
}

inline bool is_default_extrusion(double nx, double ny, double nz)
{
    return std::abs(nx) < 1.0e-9 &&
           std::abs(ny) < 1.0e-9 &&
           std::abs(nz - 1.0) < 1.0e-9;
}

inline Vec3 normalize_vec3(double x, double y, double z)
{
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len <= 1.0e-12) {
        return Vec3::unit_z();
    }
    return {
        static_cast<float>(x / len),
        static_cast<float>(y / len),
        static_cast<float>(z / len),
    };
}

// Safe double->float cast: returns 0.0f for values that would overflow float.
inline float safe_float(double v) {
    if (!std::isfinite(v)) return 0.0f;
    if (v > static_cast<double>(3.4e35f)) return 0.0f;
    if (v < static_cast<double>(-3.4e35f)) return 0.0f;
    return static_cast<float>(v);
}

// Check if a double value is safe to store as float (finite and representable).
inline bool is_safe_coord(double v) {
    return std::isfinite(v) && std::abs(v) < 1.0e8;
}

struct OcsBasis {
    Vec3 x_axis = {1.0f, 0.0f, 0.0f};
    Vec3 y_axis = {0.0f, 1.0f, 0.0f};
    Vec3 z_axis = Vec3::unit_z();
};

inline OcsBasis make_ocs_basis(double nx, double ny, double nz)
{
    OcsBasis basis;
    basis.z_axis = normalize_vec3(nx, ny, nz);

    const Vec3 world_y{0.0f, 1.0f, 0.0f};
    const Vec3 world_z{0.0f, 0.0f, 1.0f};
    const Vec3 helper =
        (std::abs(basis.z_axis.x) < 1.0f / 64.0f &&
         std::abs(basis.z_axis.y) < 1.0f / 64.0f)
            ? world_y
            : world_z;
    basis.x_axis = helper.cross(basis.z_axis).normalized();
    basis.y_axis = basis.z_axis.cross(basis.x_axis).normalized();
    return basis;
}

inline Vec3 ocs_point_to_wcs(double x, double y, double z, const OcsBasis& basis)
{
    return basis.x_axis * static_cast<float>(x) +
           basis.y_axis * static_cast<float>(y) +
           basis.z_axis * static_cast<float>(z);
}

inline Vec3 ocs_vector_to_wcs(double x, double y, double z, const OcsBasis& basis)
{
    return basis.x_axis * static_cast<float>(x) +
           basis.y_axis * static_cast<float>(y) +
           basis.z_axis * static_cast<float>(z);
}

} // namespace cad
