// render_batcher_curve.cpp — B-spline evaluation, spline tessellation,
// and shared batcher helper functions extracted from render_batcher.cpp.

#include "render_batcher_internal.h"
#include "cad/renderer/render_batcher.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cad {
namespace batcher {

Vec3 catmull_rom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                 float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return {
        0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
        0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3),
        0.0f,
    };
}

bool valid_knot_vector(const std::vector<float>& knots, size_t control_count, int degree) {
    if (degree < 1 || control_count < 2) return false;
    const size_t expected = control_count + static_cast<size_t>(degree) + 1;
    if (knots.size() < expected) return false;
    for (size_t i = 1; i < knots.size(); ++i) {
        if (!std::isfinite(knots[i]) || knots[i] < knots[i - 1]) return false;
    }
    return true;
}

int find_spline_span(const std::vector<float>& knots, int control_count, int degree,
                     float u) {
    const int n = control_count - 1;
    if (u >= knots[static_cast<size_t>(n + 1)]) return n;
    if (u <= knots[static_cast<size_t>(degree)]) return degree;

    int low = degree;
    int high = n + 1;
    int mid = (low + high) / 2;
    while (u < knots[static_cast<size_t>(mid)] ||
           u >= knots[static_cast<size_t>(mid + 1)]) {
        if (u < knots[static_cast<size_t>(mid)]) {
            high = mid;
        } else {
            low = mid;
        }
        mid = (low + high) / 2;
    }
    return mid;
}

Vec3 evaluate_bspline(const SplineEntity& spline, float u) {
    const auto& points = spline.control_points;
    const auto& knots = spline.knots;
    const int degree = std::clamp(spline.degree, 1, 16);
    const int point_count = static_cast<int>(points.size());
    const int span = find_spline_span(knots, point_count, degree, u);

    struct WeightedPoint {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double w = 1.0;
    };

    std::vector<WeightedPoint> d(static_cast<size_t>(degree + 1));
    for (int j = 0; j <= degree; ++j) {
        const int idx = span - degree + j;
        const float weight =
            (spline.weights.size() == points.size() && spline.weights[static_cast<size_t>(idx)] > 0.0f)
                ? spline.weights[static_cast<size_t>(idx)]
                : 1.0f;
        const Vec3& pt = points[static_cast<size_t>(idx)];
        d[static_cast<size_t>(j)] = {
            static_cast<double>(pt.x) * weight,
            static_cast<double>(pt.y) * weight,
            static_cast<double>(pt.z) * weight,
            static_cast<double>(weight),
        };
    }

    for (int r = 1; r <= degree; ++r) {
        for (int j = degree; j >= r; --j) {
            const int left = span - degree + j;
            const int right = span + 1 + j - r;
            const double denom =
                static_cast<double>(knots[static_cast<size_t>(right)] -
                                    knots[static_cast<size_t>(left)]);
            const double alpha = std::abs(denom) > 1.0e-12
                ? (static_cast<double>(u) - knots[static_cast<size_t>(left)]) / denom
                : 0.0;
            auto& cur = d[static_cast<size_t>(j)];
            const auto& prev = d[static_cast<size_t>(j - 1)];
            cur.x = (1.0 - alpha) * prev.x + alpha * cur.x;
            cur.y = (1.0 - alpha) * prev.y + alpha * cur.y;
            cur.z = (1.0 - alpha) * prev.z + alpha * cur.z;
            cur.w = (1.0 - alpha) * prev.w + alpha * cur.w;
        }
    }

    const auto& out = d[static_cast<size_t>(degree)];
    const double inv_w = std::abs(out.w) > 1.0e-12 ? (1.0 / out.w) : 1.0;
    return {
        static_cast<float>(out.x * inv_w),
        static_cast<float>(out.y * inv_w),
        static_cast<float>(out.z * inv_w),
    };
}

std::vector<Vec3> tessellate_spline_points(const SplineEntity& spline, float ppu) {
    std::vector<Vec3> out;

    if (!spline.fit_points.empty()) {
        const auto& pts = spline.fit_points;
        if (pts.size() < 2) return out;

        float seg_chord = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i) {
            seg_chord += distance_xy(pts[i - 1], pts[i]);
        }
        float total_px = seg_chord * ppu;
        int samples_per_seg = static_cast<int>(std::ceil(total_px / (pts.size() * 2.0f)));
        samples_per_seg = std::clamp(samples_per_seg, 4, 32);

        out.reserve((pts.size() - 1) * samples_per_seg + 1);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec3& p0 = (i == 0) ? pts[i] : pts[i - 1];
            const Vec3& p1 = pts[i];
            const Vec3& p2 = pts[i + 1];
            const Vec3& p3 = (i + 2 < pts.size()) ? pts[i + 2] : pts[i + 1];
            for (int s = 0; s < samples_per_seg; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(samples_per_seg);
                Vec3 p = catmull_rom(p0, p1, p2, p3, t);
                if (is_renderable_point(p)) out.push_back(p);
            }
        }
        if (is_renderable_point(pts.back())) out.push_back(pts.back());
        if (spline.is_closed && out.size() > 2) out.push_back(out.front());
        return out;
    }

    const auto& pts = spline.control_points;
    if (pts.size() < 2) return out;

    const int degree = std::clamp(spline.degree, 1, 16);
    if (valid_knot_vector(spline.knots, pts.size(), degree)) {
        const int n = static_cast<int>(pts.size()) - 1;
        const float u0 = spline.knots[static_cast<size_t>(degree)];
        const float u1 = spline.knots[static_cast<size_t>(n + 1)];
        if (std::isfinite(u0) && std::isfinite(u1) && u1 > u0) {
            float chord = 0.0f;
            float min_edge = 1e30f;
            for (size_t i = 1; i < pts.size(); ++i) {
                float d = distance_xy(pts[i - 1], pts[i]);
                chord += d;
                if (d > 1e-6f) min_edge = std::min(min_edge, d);
            }
            float effective_r = std::min(min_edge, chord * 0.5f);
            float tol_world = 0.5f / std::max(ppu, 1e-6f);
            float adaptive = chord / std::sqrt(std::max(8.0f * effective_r * tol_world, 1e-6f));
            float chord_px = chord * ppu;
            int samples = static_cast<int>(std::ceil(std::max(adaptive, chord_px * 0.5f)));
            samples = std::clamp(samples, static_cast<int>(pts.size()) * 2, 2048);

            out.reserve(static_cast<size_t>(samples + 1));
            for (int i = 0; i <= samples; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(samples);
                const float u = (i == samples) ? u1 : (u0 + (u1 - u0) * t);
                Vec3 p = evaluate_bspline(spline, u);
                if (is_renderable_point(p)) out.push_back(p);
            }
            if (spline.is_closed && out.size() > 2) out.push_back(out.front());
            return out;
        }
    }

    out.reserve(pts.size());
    for (const Vec3& pt : pts) {
        if (is_renderable_point(pt)) out.push_back(pt);
    }
    if (spline.is_closed && out.size() > 2) out.push_back(out.front());
    return out;
}

bool same_pattern(const LinePattern& a, const LinePattern& b) {
    return a.dash_array == b.dash_array;
}

void split_line_strip_coordinate_jumps(RenderBatch& batch) {
    if (batch.topology != PrimitiveTopology::LineStrip ||
        batch.vertex_data.size() < 8 ||
        batch.entity_starts.empty()) {
        return;
    }

    const auto& vd = batch.vertex_data;
    struct Range { size_t start = 0; size_t end = 0; };
    std::vector<Range> ranges;
    ranges.reserve(batch.entity_starts.size());
    for (size_t ei = 0; ei < batch.entity_starts.size(); ++ei) {
        const size_t start = static_cast<size_t>(batch.entity_starts[ei]) * 2;
        const size_t end = (ei + 1 < batch.entity_starts.size())
            ? static_cast<size_t>(batch.entity_starts[ei + 1]) * 2
            : vd.size();
        if (start + 3 < end && end <= vd.size()) {
            ranges.push_back({start, end});
        }
    }
    if (ranges.empty()) return;

    std::vector<float> filtered;
    std::vector<uint32_t> starts;
    std::vector<float> path;

    auto is_coordinate_jump = [](float x0, float y0, float x1, float y1, float len, float threshold) {
        if (!std::isfinite(len)) return true;
        if (len > threshold) return true;
        const float n0 = std::sqrt(x0 * x0 + y0 * y0);
        const float n1 = std::sqrt(x1 * x1 + y1 * y1);
        const float max_norm = std::max(n0, n1);
        const float min_norm = std::min(n0, n1);
        return len > 100000.0f &&
               max_norm > 100000.0f &&
               min_norm < max_norm * 0.01f;
    };

    auto flush_path = [&]() {
        if (path.size() >= 4) {
            starts.push_back(static_cast<uint32_t>(filtered.size() / 2));
            filtered.insert(filtered.end(), path.begin(), path.end());
        }
        path.clear();
    };

    for (const auto& range : ranges) {
        std::vector<float> lengths;
        for (size_t i = range.start; i + 3 < range.end; i += 2) {
            const float dx = vd[i + 2] - vd[i];
            const float dy = vd[i + 3] - vd[i + 1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (std::isfinite(len)) lengths.push_back(len);
        }
        float threshold = 1000000.0f;
        if (lengths.size() >= 3) {
            std::sort(lengths.begin(), lengths.end());
            const float median = lengths[lengths.size() / 2];
            const float q90 = lengths[std::min(lengths.size() - 1,
                                               static_cast<size_t>(lengths.size() * 0.90f))];
            threshold = std::max(10000.0f, std::max(median * 200.0f, q90 * 50.0f));
        }

        path.clear();
        path.push_back(vd[range.start]);
        path.push_back(vd[range.start + 1]);
        for (size_t i = range.start; i + 3 < range.end; i += 2) {
            const float dx = vd[i + 2] - vd[i];
            const float dy = vd[i + 3] - vd[i + 1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (is_coordinate_jump(vd[i], vd[i + 1], vd[i + 2], vd[i + 3], len, threshold)) {
                flush_path();
                path.push_back(vd[i + 2]);
                path.push_back(vd[i + 3]);
                continue;
            }
            path.push_back(vd[i + 2]);
            path.push_back(vd[i + 3]);
        }
        flush_path();
    }

    batch.vertex_data = std::move(filtered);
    batch.entity_starts = std::move(starts);
}

bool point_in_polygon(float x, float y, const float* poly_x, const float* poly_y, int n) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((poly_y[i] > y) != (poly_y[j] > y)) &&
            (x < (poly_x[j] - poly_x[i]) * (y - poly_y[i]) / (poly_y[j] - poly_y[i]) + poly_x[i])) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace batcher
} // namespace cad
