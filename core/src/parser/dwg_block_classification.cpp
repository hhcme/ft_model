// DWG block classification — determines how blocks are rendered.
// Extracted from test_render_export.cpp to allow reuse by parser and renderer.

#include "cad/parser/dwg_block_classification.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace cad {
namespace block_classify {

static std::string uppercase_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c -= 32;
    }
    return out;
}

bool is_model_or_paper_space(const std::string& name) {
    const std::string upper = uppercase_ascii(name);
    return upper == "*MODEL_SPACE" || upper == "*PAPER_SPACE";
}

Bounds3d bounds_for_indices(
    const std::vector<EntityVariant>& entities,
    const std::vector<int32_t>& indices) {
    Bounds3d bounds = Bounds3d::empty();
    for (int32_t idx : indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= entities.size()) continue;
        const Bounds3d& eb = entities[static_cast<size_t>(idx)].bounds();
        if (eb.is_empty()) continue;
        bounds.expand(eb.min);
        bounds.expand(eb.max);
    }
    return bounds;
}

static float bounds_major_extent(const Bounds3d& b) {
    if (b.is_empty()) return 0.0f;
    return std::max(b.max.x - b.min.x, b.max.y - b.min.y);
}

bool should_render_direct(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert) {
    if (is_model_or_paper_space(block.name)) return true;

    const Bounds3d bounds = bounds_for_indices(entities, block.entity_indices);
    if (bounds.is_empty() || block.entity_indices.empty()) return false;

    const float major = bounds_major_extent(bounds);
    const float cx = (bounds.min.x + bounds.max.x) * 0.5f;
    const float cy = (bounds.min.y + bounds.max.y) * 0.5f;
    const float centroid_dist = std::sqrt(cx * cx + cy * cy);

    // Large world-space blocks: geometry is in absolute coordinates,
    // rendering through INSERT with transforms produces wrong results.
    if (centroid_dist > 5000.0f && block.entity_indices.size() > 50) {
        return true;
    }
    // Non-anonymous blocks with world-space geometry and significant extent.
    // These blocks contain entities parsed at absolute coordinates (e.g. R2004
    // blocks populated via header-owned entity resolution).  Expanding through
    // INSERT would apply a redundant transform to absolute coordinates.
    if (!block.is_anonymous && centroid_dist > 5000.0f && major > 1000.0f) {
        return true;
    }
    if (!block.is_anonymous && has_scaled_insert && centroid_dist > 5000.0f) {
        return true;
    }
    return !block.is_anonymous &&
           has_scaled_insert &&
           major > 1000.0f;
}

bool should_merge_header_entities(
    const Block& block,
    const std::vector<EntityVariant>& entities,
    bool has_scaled_insert) {
    if (block.is_anonymous || !has_scaled_insert ||
        block.header_owned_entity_indices.size() < 2) {
        return false;
    }
    const Bounds3d header_bounds =
        bounds_for_indices(entities, block.header_owned_entity_indices);
    if (header_bounds.is_empty()) {
        return false;
    }
    const float header_major = bounds_major_extent(header_bounds);
    if (!std::isfinite(header_major) || header_major <= 0.0f) {
        return false;
    }
    if (header_major > 50000.0f) {
        return false;
    }
    return true;
}

}  // namespace block_classify
}  // namespace cad
