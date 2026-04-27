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
    Leader,
    Tolerance,
    MLine,
};

enum class DrawingSpace : uint8_t {
    Unknown,
    ModelSpace,
    PaperSpace,
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
    Color true_color;                  // RGB True Color from DWG ENC/CMC
    bool has_true_color = false;       // true when entity has explicit RGB color
    float lineweight = 0.0f;           // 0 = BYLAYER, negative = BYBLOCK
    bool is_visible = true;
    bool in_block = false;             // True if entity belongs to a block definition (skip in top-level iteration)
    Bounds3d bounds = Bounds3d::empty();
    int32_t block_index = -1;          // For INSERT: resolved block index (-1 = unresolved)
    DrawingSpace space = DrawingSpace::ModelSpace;
    int32_t owner_block_index = -1;
    int32_t layout_index = -1;
    int32_t viewport_index = -1;
    int32_t plot_style_index = -1;
    uint32_t draw_order = 0;
    float annotation_scale = 1.0f;
    uint64_t dwg_handle = 0;
    uint64_t owner_handle = 0;
    uint64_t block_header_handle = 0;
    uint32_t validation_flags = 0;
    bool is_proxy_fallback = false;
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
    bool is_rational = false;
    bool is_closed = false;
    bool is_periodic = false;
    std::vector<Vec3> control_points;
    std::vector<Vec3> fit_points;
    std::vector<float> knots;
    std::vector<float> weights;
};

struct TextEntity {
    Vec3 insertion_point;
    float height = 0.0f;
    float rotation = 0.0f;            // radians
    float width_factor = 1.0f;
    float rect_width = 0.0f;          // MTEXT reference rectangle width, 0 when absent
    float rect_height = 0.0f;         // MTEXT reference rectangle height, 0 when absent
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

struct PointEntity {
    Vec3 position;
    float pdsize = 0.0f;             // display size (0 = system default 5% of viewport)
};

struct ViewportEntity {
    Vec3 center;                      // viewport center in paper space
    float width = 0.0f;
    float height = 0.0f;
    int32_t status = 1;              // -1=off, 0=maximized, 1=on
    Vec3 target = {0, 0, 0};         // model view target point
    float view_height = 0.0f;        // model view height
    float view_width = 0.0f;         // model view width
    float twist_angle = 0.0f;        // model view twist (radians)
    float custom_scale = 1.0f;       // viewport scale factor
    bool has_custom_scale = false;
};

struct LeaderEntity {
    int32_t vertex_offset = 0;         // index into scene vertex buffer
    int32_t vertex_count = 0;
    bool is_spline = false;            // path_type: 0=straight, 1=spline
    bool has_arrowhead = true;
    Vec3 horizontal_direction = {1, 0, 0};
    float arrowhead_size = 0.0f;
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
    PointEntity,       // Point
    CircleEntity,      // Ellipse reuses CircleEntity (with normal for orientation)
    LineEntity,        // Ray reuses LineEntity
    LineEntity,        // XLine reuses LineEntity
    ViewportEntity,    // Viewport
    SolidEntity,       // Solid — filled 3/4 vertex polygon
    LeaderEntity,      // Leader — leader line path + arrowhead
    TextEntity,        // Tolerance — GD&T feature control frame (reuses TextEntity)
    PolylineEntity     // MLine — multiline entity (reuses PolylineEntity)
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
    // in EntityData (LineEntity, CircleEntity, TextEntity, PolylineEntity).
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
    const PointEntity* as_point() const;
    const ViewportEntity* as_viewport() const;
    const LeaderEntity* as_leader() const;
    const SolidEntity* as_solid() const;
    const TextEntity* as_tolerance() const;
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
Bounds3d entity_bounds_point(const PointEntity& pt);
Bounds3d entity_bounds_viewport(const ViewportEntity& vp);
Bounds3d entity_bounds_leader(const LeaderEntity& leader, const Vec3* vertices);
Bounds3d entity_bounds_solid(const SolidEntity& solid);

} // namespace cad
