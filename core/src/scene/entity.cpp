#include "cad/scene/entity.h"

#include <cmath>
#include <algorithm>

namespace cad {

// ============================================================
// EntityData variant indices
// ============================================================
// 0  LineEntity     (Line)
// 1  CircleEntity   (Circle)
// 2  ArcEntity      (Arc)
// 3  PolylineEntity (Polyline)
// 4  PolylineEntity (LwPolyline)
// 5  SplineEntity   (Spline)
// 6  TextEntity     (Text)
// 7  TextEntity     (MText)
// 8  DimensionEntity(Dimension)
// 9  HatchEntity    (Hatch)
// 10 InsertEntity   (Insert)
// 11 PointEntity    (Point)
// 12 CircleEntity   (Ellipse)
// 13 LineEntity     (Ray)
// 14 LineEntity     (XLine)
// 15 ViewportEntity (Viewport)
// 16 SolidEntity    (Solid)
// 17 LeaderEntity   (Leader)

// ============================================================
// EntityVariant convenience accessors
// ============================================================

const LineEntity* EntityVariant::as_line() const {
    if (header.type == EntityType::Line)  return std::get_if<0>(&data);
    if (header.type == EntityType::Ray)   return std::get_if<13>(&data);
    if (header.type == EntityType::XLine) return std::get_if<14>(&data);
    return nullptr;
}

const CircleEntity* EntityVariant::as_circle() const {
    if (header.type == EntityType::Circle)  return std::get_if<1>(&data);
    if (header.type == EntityType::Ellipse) return std::get_if<12>(&data);
    return nullptr;
}

const ArcEntity* EntityVariant::as_arc() const {
    return std::get_if<2>(&data);
}

const PolylineEntity* EntityVariant::as_polyline() const {
    if (header.type == EntityType::Polyline)  return std::get_if<3>(&data);
    if (header.type == EntityType::LwPolyline) return std::get_if<4>(&data);
    return nullptr;
}

const SplineEntity* EntityVariant::as_spline() const {
    return std::get_if<5>(&data);
}

const TextEntity* EntityVariant::as_text() const {
    if (header.type == EntityType::Text)  return std::get_if<6>(&data);
    if (header.type == EntityType::MText) return std::get_if<7>(&data);
    return nullptr;
}

const HatchEntity* EntityVariant::as_hatch() const {
    return std::get_if<9>(&data);
}

const InsertEntity* EntityVariant::as_insert() const {
    return std::get_if<10>(&data);
}

const DimensionEntity* EntityVariant::as_dimension() const {
    return std::get_if<8>(&data);
}

const PointEntity* EntityVariant::as_point() const {
    return std::get_if<11>(&data);
}

const ViewportEntity* EntityVariant::as_viewport() const {
    return std::get_if<15>(&data);
}

const LeaderEntity* EntityVariant::as_leader() const {
    return std::get_if<17>(&data);
}

const SolidEntity* EntityVariant::as_solid() const {
    return std::get_if<16>(&data);
}

const TextEntity* EntityVariant::as_tolerance() const {
    if (header.type == EntityType::Tolerance) return std::get_if<18>(&data);
    return nullptr;
}

// ============================================================
// Entity bounds calculations
// ============================================================

Bounds3d entity_bounds_line(const LineEntity& line) {
    Bounds3d b = Bounds3d::from_point(line.start);
    b.expand(line.end);
    return b;
}

Bounds3d entity_bounds_circle(const CircleEntity& circle) {
    float r = circle.radius;
    Bounds3d b;
    b.min = {circle.center.x - r, circle.center.y - r, circle.center.z};
    b.max = {circle.center.x + r, circle.center.y + r, circle.center.z};
    return b;
}

Bounds3d entity_bounds_arc(const ArcEntity& arc) {
    float r = arc.radius;
    float cx = arc.center.x;
    float cy = arc.center.y;

    float start = std::fmod(arc.start_angle, math::TWO_PI);
    float end = std::fmod(arc.end_angle, math::TWO_PI);
    if (start < 0) start += math::TWO_PI;
    if (end < 0) end += math::TWO_PI;

    float sx = cx + r * std::cos(start);
    float sy = cy + r * std::sin(start);
    float ex = cx + r * std::cos(end);
    float ey = cy + r * std::sin(end);

    Bounds3d b = Bounds3d::empty();
    b.expand({sx, sy, arc.center.z});
    b.expand({ex, ey, arc.center.z});

    auto crosses_angle = [](float s, float e, float angle) -> bool {
        if (s <= e) {
            return s <= angle && angle <= e;
        }
        return s <= angle || angle <= e;
    };

    if (crosses_angle(start, end, 0.0f))
        b.expand({cx + r, cy, arc.center.z});
    if (crosses_angle(start, end, math::HALF_PI))
        b.expand({cx, cy + r, arc.center.z});
    if (crosses_angle(start, end, math::PI))
        b.expand({cx - r, cy, arc.center.z});
    if (crosses_angle(start, end, 3.0f * math::HALF_PI))
        b.expand({cx, cy - r, arc.center.z});

    return b;
}

Bounds3d entity_bounds_spline(const SplineEntity& spline) {
    Bounds3d b = Bounds3d::empty();
    for (const auto& pt : spline.control_points) {
        b.expand(pt);
    }
    for (const auto& pt : spline.fit_points) {
        b.expand(pt);
    }
    return b;
}

Bounds3d entity_bounds_text(const TextEntity& text) {
    float half_h = text.height * 0.5f;
    float approx_width = text.rect_width > 0.0f
        ? text.rect_width
        : static_cast<float>(text.text.size()) * text.height *
              text.width_factor * 0.6f;

    float left = text.insertion_point.x;
    float bottom = text.insertion_point.y - half_h;
    float right = left + approx_width;
    float top = text.insertion_point.y + half_h;

    if (std::abs(text.rotation) > 1e-6f) {
        float c = std::cos(text.rotation);
        float s = std::sin(text.rotation);
        float dx0 = 0, dy0 = -half_h;
        float dx1 = approx_width, dy1 = -half_h;
        float dx2 = approx_width, dy2 = half_h;
        float dx3 = 0, dy3 = half_h;

        Bounds3d b = Bounds3d::empty();
        Vec3 corners[4] = {
            {left + dx0 * c - dy0 * s, bottom + dx0 * s + dy0 * c, text.insertion_point.z},
            {left + dx1 * c - dy1 * s, bottom + dx1 * s + dy1 * c, text.insertion_point.z},
            {left + dx2 * c - dy2 * s, bottom + dx2 * s + dy2 * c, text.insertion_point.z},
            {left + dx3 * c - dy3 * s, bottom + dx3 * s + dy3 * c, text.insertion_point.z},
        };
        for (const auto& corner : corners) {
            b.expand(corner);
        }
        return b;
    }

    return {{left, bottom, text.insertion_point.z},
            {right, top, text.insertion_point.z}};
}

Bounds3d entity_bounds_hatch(const HatchEntity& hatch) {
    Bounds3d b = Bounds3d::empty();
    for (const auto& loop : hatch.loops) {
        for (const auto& v : loop.vertices) {
            b.expand(v);
        }
    }
    return b;
}

Bounds3d entity_bounds_insert(const InsertEntity& insert) {
    Bounds3d b = Bounds3d::from_point(insert.insertion_point);

    if (insert.column_count > 1 || insert.row_count > 1) {
        float total_x = static_cast<float>(insert.column_count - 1) * insert.column_spacing;
        float total_y = static_cast<float>(insert.row_count - 1) * insert.row_spacing;
        b.expand({insert.insertion_point.x + total_x,
                  insert.insertion_point.y + total_y,
                  insert.insertion_point.z});
    }
    return b;
}

Bounds3d entity_bounds_dimension(const DimensionEntity& dim) {
    Bounds3d b = Bounds3d::from_point(dim.definition_point);
    b.expand(dim.text_midpoint);
    return b;
}

Bounds3d entity_bounds_point(const PointEntity& pt) {
    return Bounds3d::from_point(pt.position);
}

Bounds3d entity_bounds_viewport(const ViewportEntity& vp) {
    float hw = vp.width * 0.5f;
    float hh = vp.height * 0.5f;
    return {{vp.center.x - hw, vp.center.y - hh, vp.center.z},
            {vp.center.x + hw, vp.center.y + hh, vp.center.z}};
}

Bounds3d entity_bounds_leader(const LeaderEntity& leader, const Vec3* vertices) {
    Bounds3d b = Bounds3d::empty();
    for (int32_t i = 0; i < leader.vertex_count && vertices; ++i) {
        b.expand(vertices[i]);
    }
    return b;
}

Bounds3d entity_bounds_solid(const SolidEntity& solid) {
    Bounds3d b = Bounds3d::empty();
    for (int32_t i = 0; i < solid.corner_count; ++i) {
        b.expand(solid.corners[i]);
    }
    return b;
}

// ============================================================
// Dispatch bounds calculation based on entity type
// ============================================================

Bounds3d entity_bounds(const EntityVariant& entity) {
    Bounds3d result = Bounds3d::empty();

    switch (entity.header.type) {
    case EntityType::Line:
        if (auto* e = std::get_if<0>(&entity.data)) result = entity_bounds_line(*e);
        break;
    case EntityType::Ray:
        if (auto* e = std::get_if<13>(&entity.data)) result = entity_bounds_line(*e);
        break;
    case EntityType::XLine:
        if (auto* e = std::get_if<14>(&entity.data)) result = entity_bounds_line(*e);
        break;
    case EntityType::Circle:
        if (auto* e = std::get_if<1>(&entity.data)) result = entity_bounds_circle(*e);
        break;
    case EntityType::Ellipse:
        if (auto* e = std::get_if<12>(&entity.data)) result = entity_bounds_circle(*e);
        break;
    case EntityType::Arc:
        if (auto* e = std::get_if<2>(&entity.data)) result = entity_bounds_arc(*e);
        break;
    case EntityType::Polyline:
    case EntityType::LwPolyline:
        // Polyline bounds require vertex buffer access (resolved at scene level).
        break;
    case EntityType::Spline:
        if (auto* e = std::get_if<5>(&entity.data)) result = entity_bounds_spline(*e);
        break;
    case EntityType::Text:
        if (auto* e = std::get_if<6>(&entity.data)) result = entity_bounds_text(*e);
        break;
    case EntityType::MText:
        if (auto* e = std::get_if<7>(&entity.data)) result = entity_bounds_text(*e);
        break;
    case EntityType::Hatch:
        if (auto* e = std::get_if<9>(&entity.data)) result = entity_bounds_hatch(*e);
        break;
    case EntityType::Insert:
        if (auto* e = std::get_if<10>(&entity.data)) result = entity_bounds_insert(*e);
        break;
    case EntityType::Dimension:
        if (auto* e = std::get_if<8>(&entity.data)) result = entity_bounds_dimension(*e);
        break;
    case EntityType::Point:
        if (auto* e = std::get_if<11>(&entity.data)) result = entity_bounds_point(*e);
        break;
    case EntityType::Viewport:
        if (auto* e = std::get_if<15>(&entity.data)) result = entity_bounds_viewport(*e);
        break;
    case EntityType::Solid:
        if (auto* e = std::get_if<16>(&entity.data)) result = entity_bounds_solid(*e);
        break;
    case EntityType::Leader:
        break;  // Leader bounds require vertex buffer access (resolved at scene level)
    case EntityType::Tolerance:
        if (auto* e = std::get_if<18>(&entity.data)) result = entity_bounds_text(*e);
        break;
    case EntityType::MLine:
        break;  // MLine bounds require vertex buffer access (resolved at scene level)
    }

    return result;
}

} // namespace cad
