#pragma once

#include "cad/cad_types.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace cad {

// ============================================================
// Entity type enumeration
// ============================================================
enum class EntityType : uint8_t {
    Line,
    Circle,
    Arc,
    Polyline,
    LwPolyline,
    Spline,
    Text,
    MText,
    Dimension,
    Hatch,
    Insert,
    Point,
    Ellipse,
    Ray,
    XLine,
    Viewport,
    Solid,
};

// ============================================================
// Entity header — common metadata shared by all entity types
// ============================================================
struct EntityHeader {
    EntityType type = EntityType::Line;
    int64_t entity_id = -1;
    int32_t layer_index = 0;
    int32_t linetype_index = -1;       // -1 = BYLAYER
    int32_t color_override = 256;      // ACI color, 256 = BYLAYER
    float lineweight = 0.0f;           // 0 = BYLAYER, negative = BYBLOCK
    bool is_visible = true;
    bool in_block = false;             // True if entity belongs to a block definition (skip in top-level iteration)
    Bounds3d bounds = Bounds3d::empty();
    int32_t block_index = -1;          // For INSERT: resolved block index (-1 = unresolved)
    uint8_t dimensionality = 0x02;     // 0x02 = 2D, 0x03 = 3D (reserved)
};

// ============================================================
// Concrete entity data structs
// ============================================================

struct LineEntity {
    Vec3 start;
    Vec3 end;
};

struct CircleEntity {
    Vec3 center;
    float radius = 0.0f;           // major radius for ellipse
    Vec3 normal = Vec3::unit_z();
    // Ellipse extensions (ignored for pure circles):
    float minor_radius = 0.0f;     // 0 means circle (minor == major)
    float rotation = 0.0f;         // major axis rotation angle (radians)
    float start_angle = 0.0f;      // parametric start angle (radians, 0 for full)
    float end_angle = 0.0f;        // parametric end angle (radians, 2*pi for full)
};

struct ArcEntity {
    Vec3 center;
    float radius = 0.0f;
    float start_angle = 0.0f;          // radians
    float end_angle = 0.0f;            // radians
};

struct PolylineEntity {
    int32_t vertex_offset = 0;         // index into scene vertex buffer
    int32_t vertex_count = 0;
    bool is_closed = false;
    std::vector<float> bulges;         // bulge factor per vertex (for arcs)
};

struct SplineEntity {
    int32_t degree = 3;
    std::vector<Vec3> control_points;
    std::vector<Vec3> fit_points;
    std::vector<float> knots;
};

struct TextEntity {
    Vec3 insertion_point;
    float height = 0.0f;
    float rotation = 0.0f;            // radians
    float width_factor = 1.0f;
    std::string text;
    int32_t text_style_index = 0;
    int32_t alignment = 0;            // 0=left, 1=center, 2=right, etc.
};

struct HatchEntity {
    std::string pattern_name;
    float pattern_scale = 1.0f;
    float pattern_angle = 0.0f;
    bool is_solid = false;

    struct BoundaryLoop {
        std::vector<Vec3> vertices;
        bool is_closed = true;
    };

    std::vector<BoundaryLoop> loops;
};

struct InsertEntity {
    int32_t block_index = -1;
    Vec3 insertion_point;
    float x_scale = 1.0f;
    float y_scale = 1.0f;
    float rotation = 0.0f;            // radians
    int32_t column_count = 1;
    int32_t row_count = 1;
    float column_spacing = 0.0f;
    float row_spacing = 0.0f;
};

struct DimensionEntity {
    Vec3 definition_point;
    Vec3 text_midpoint;
    std::string text;
    int32_t dimension_type = 0;
    float rotation = 0.0f;            // radians
};

struct SolidEntity {
    Vec3 corners[4];                  // up to 4 vertices; unused corners default to third corner
    int32_t corner_count = 3;         // 3 or 4
};

// ============================================================
// EntityVariant — tagged union via std::variant
// ============================================================
using EntityData = std::variant<
    LineEntity,
    CircleEntity,
    ArcEntity,
    PolylineEntity,
    PolylineEntity,    // LwPolyline reuses PolylineEntity
    SplineEntity,
    TextEntity,
    TextEntity,        // MText reuses TextEntity (extended text)
    DimensionEntity,
    HatchEntity,
    InsertEntity,
    Vec3,              // Point — just a position
    CircleEntity,      // Ellipse reuses CircleEntity (with normal for orientation)
    LineEntity,        // Ray reuses LineEntity
    LineEntity,        // XLine reuses LineEntity
    Vec3,              // Viewport placeholder (center point)
    SolidEntity        // Solid — filled 3/4 vertex polygon
>;

struct EntityVariant {
    EntityHeader header;
    EntityData data;

    // Type-safe access helpers
    EntityType type() const { return header.type; }
    int64_t id() const { return header.entity_id; }
    bool is_visible() const { return header.is_visible; }
    const Bounds3d& bounds() const { return header.bounds; }

    // Typed data access by index — returns nullptr if index does not hold T
    template<size_t I, typename T>
    const T* get_if_at() const {
        return std::get_if<I>(&data);
    }

    template<size_t I, typename T>
    T* get_if_at() {
        return std::get_if<I>(&data);
    }

    // Typed data access for unique types in the variant.
    // WARNING: Do NOT use with types that appear more than once
    // in EntityData (LineEntity, CircleEntity, TextEntity, PolylineEntity, Vec3).
    // Use index-based get_if_at<I, T>() for those.
    template<typename T>
    const T* get_if_unique() const {
        return std::get_if<T>(&data);
    }

    // Convenience accessors using index-based lookup for duplicate types
    const LineEntity* as_line() const;
    const CircleEntity* as_circle() const;
    const ArcEntity* as_arc() const;
    const PolylineEntity* as_polyline() const;
    const SplineEntity* as_spline() const;
    const TextEntity* as_text() const;
    const HatchEntity* as_hatch() const;
    const InsertEntity* as_insert() const;
    const DimensionEntity* as_dimension() const;
    const SolidEntity* as_solid() const;
};

// ============================================================
// Entity bounds calculation
// ============================================================

// Compute the bounding box for a given entity variant
Bounds3d entity_bounds(const EntityVariant& entity);

// Overloads for individual entity types
Bounds3d entity_bounds_line(const LineEntity& line);
Bounds3d entity_bounds_circle(const CircleEntity& circle);
Bounds3d entity_bounds_arc(const ArcEntity& arc);
Bounds3d entity_bounds_spline(const SplineEntity& spline);
Bounds3d entity_bounds_text(const TextEntity& text);
Bounds3d entity_bounds_hatch(const HatchEntity& hatch);
Bounds3d entity_bounds_insert(const InsertEntity& insert);
Bounds3d entity_bounds_dimension(const DimensionEntity& dim);
Bounds3d entity_bounds_solid(const SolidEntity& solid);

} // namespace cad
