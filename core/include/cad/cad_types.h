#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace cad {

// ============================================================
// 2D Vector (convenience for pure 2D operations)
// ============================================================
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& rhs) const { return {x + rhs.x, y + rhs.y}; }
    Vec2 operator-(const Vec2& rhs) const { return {x - rhs.x, y - rhs.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2 operator-() const { return {-x, -y}; }

    Vec2& operator+=(const Vec2& rhs) { x += rhs.x; y += rhs.y; return *this; }
    Vec2& operator-=(const Vec2& rhs) { x -= rhs.x; y -= rhs.y; return *this; }

    float dot(const Vec2& rhs) const { return x * rhs.x + y * rhs.y; }
    float cross(const Vec2& rhs) const { return x * rhs.y - y * rhs.x; }
    float length_squared() const { return x * x + y * y; }
    float length() const { return std::sqrt(length_squared()); }
    float distance_to(const Vec2& rhs) const { return (*this - rhs).length(); }

    Vec2 normalized() const {
        float len = length();
        return len > 0 ? *this / len : Vec2{0, 0};
    }

    static constexpr Vec2 zero() { return {0, 0}; }
};

// ============================================================
// 3D Vector (primary type — 2D is just z=0)
// ============================================================
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_ = 0.0f) : x(x_), y(y_), z(z_) {}

    // Implicit conversion from Vec2 (z=0)
    constexpr Vec3(Vec2 v) : x(v.x), y(v.y), z(0.0f) {}

    // Convenience: extract 2D part
    Vec2 xy() const { return {x, y}; }

    Vec3 operator+(const Vec3& rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    Vec3 operator-(const Vec3& rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    Vec3& operator+=(const Vec3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    Vec3& operator-=(const Vec3& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }

    float dot(const Vec3& rhs) const { return x * rhs.x + y * rhs.y + z * rhs.z; }
    Vec3 cross(const Vec3& rhs) const {
        return {
            y * rhs.z - z * rhs.y,
            z * rhs.x - x * rhs.z,
            x * rhs.y - y * rhs.x
        };
    }
    float length_squared() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(length_squared()); }
    float distance_to(const Vec3& rhs) const { return (*this - rhs).length(); }

    Vec3 normalized() const {
        float len = length();
        return len > 0 ? *this / len : Vec3{0, 0, 0};
    }

    bool operator==(const Vec3& rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }

    static constexpr Vec3 zero() { return {0, 0, 0}; }
    static constexpr Vec3 unit_z() { return {0, 0, 1}; }
};

// ============================================================
// 4x4 Matrix (unified transform — 2D is a subset)
// ============================================================
struct Matrix4x4 {
    float m[4][4] = {};

    // Identity matrix
    static Matrix4x4 identity() {
        Matrix4x4 mat;
        mat.m[0][0] = 1.0f; mat.m[1][1] = 1.0f;
        mat.m[2][2] = 1.0f; mat.m[3][3] = 1.0f;
        return mat;
    }

    // 2D translation (z=0)
    static Matrix4x4 translation_2d(float tx, float ty) {
        auto mat = identity();
        mat.m[3][0] = tx;
        mat.m[3][1] = ty;
        return mat;
    }

    // 2D rotation around Z axis (angle in radians)
    static Matrix4x4 rotation_2d(float angle) {
        auto mat = identity();
        float c = std::cos(angle);
        float s = std::sin(angle);
        mat.m[0][0] = c;  mat.m[0][1] = s;
        mat.m[1][0] = -s; mat.m[1][1] = c;
        return mat;
    }

    // 2D scale
    static Matrix4x4 scale_2d(float sx, float sy) {
        auto mat = identity();
        mat.m[0][0] = sx;
        mat.m[1][1] = sy;
        return mat;
    }

    // 2D affine transform: scale + rotation + translation combined
    static Matrix4x4 affine_2d(float sx, float sy, float angle, float tx, float ty) {
        float c = std::cos(angle);
        float s = std::sin(angle);
        Matrix4x4 mat;
        mat.m[0][0] = sx * c;  mat.m[0][1] = sx * s;
        mat.m[1][0] = -sy * s; mat.m[1][1] = sy * c;
        mat.m[3][0] = tx;      mat.m[3][1] = ty;
        mat.m[2][2] = 1.0f;    mat.m[3][3] = 1.0f;
        return mat;
    }

    // 3D translation (reserved for 3D expansion)
    static Matrix4x4 translation_3d(float tx, float ty, float tz) {
        auto mat = identity();
        mat.m[3][0] = tx; mat.m[3][1] = ty; mat.m[3][2] = tz;
        return mat;
    }

    // Multiply two matrices
    Matrix4x4 operator*(const Matrix4x4& rhs) const {
        Matrix4x4 result;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                result.m[row][col] =
                    m[row][0] * rhs.m[0][col] +
                    m[row][1] * rhs.m[1][col] +
                    m[row][2] * rhs.m[2][col] +
                    m[row][3] * rhs.m[3][col];
            }
        }
        return result;
    }

    Matrix4x4& operator*=(const Matrix4x4& rhs) {
        *this = *this * rhs;
        return *this;
    }

    // Transform a 3D point (homogeneous coordinates)
    Vec3 transform_point(const Vec3& p) const {
        return {
            m[0][0]*p.x + m[1][0]*p.y + m[2][0]*p.z + m[3][0],
            m[0][1]*p.x + m[1][1]*p.y + m[2][1]*p.z + m[3][1],
            m[0][2]*p.x + m[1][2]*p.y + m[2][2]*p.z + m[3][2]
        };
    }

    // Transform a 2D point (z=0)
    Vec2 transform_point_2d(const Vec2& p) const {
        return {
            m[0][0]*p.x + m[1][0]*p.y + m[3][0],
            m[0][1]*p.x + m[1][1]*p.y + m[3][1]
        };
    }

    // Inverse (full 4x4 inverse for general case)
    Matrix4x4 inverse() const;

    // 2D inverse (optimized — treats as 2D affine)
    Matrix4x4 inverse_2d() const;

    // Orthographic projection (2D and 3D)
    static Matrix4x4 orthographic(float left, float right, float bottom, float top,
                                   float near_plane = -1.0f, float far_plane = 1.0f);

    // Perspective projection (3D reserved)
    static Matrix4x4 perspective(float fov_radians, float aspect,
                                  float near_plane, float far_plane);

    // Data access for GPU upload (column-major as expected by OpenGL/Vulkan)
    const float* data() const { return &m[0][0]; }
};

// ============================================================
// 3D Bounding Box (unified — 2D uses z=0)
// ============================================================
struct Bounds3d {
    Vec3 min;
    Vec3 max;

    static Bounds3d empty() {
        return {
            {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
            {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()}
        };
    }

    static Bounds3d from_point(const Vec3& p) {
        return {p, p};
    }

    bool is_empty() const {
        return min.x > max.x;
    }

    void expand(const Vec3& point) {
        // DWG R2010+ encodes unset coordinate fields as half-float
        // exponent=31, which half_to_float maps to ±1e9f.
        // Skip sentinel coordinates to avoid corrupting bounds.
        constexpr float kSentinel = 1e9f;
        if (std::abs(point.x) >= kSentinel) return;
        if (std::abs(point.y) >= kSentinel) return;
        if (std::abs(point.z) >= kSentinel) return;

        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }

    void expand(const Bounds3d& other) {
        if (other.is_empty()) return;
        if (is_empty()) { *this = other; return; }
        expand(other.min);
        expand(other.max);
    }

    bool intersects(const Bounds3d& other) const {
        if (is_empty() || other.is_empty()) return false;
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }

    bool contains(const Vec3& point) const {
        if (is_empty()) return false;
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    bool contains_2d(const Vec2& point) const {
        if (is_empty()) return false;
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y;
    }

    Vec3 center() const {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    }

    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
    float depth() const { return max.z - min.z; }

    // Inflate by amount on all sides
    Bounds3d inflated(float amount) const {
        if (is_empty()) return *this;
        return {
            {min.x - amount, min.y - amount, min.z - amount},
            {max.x + amount, max.y + amount, max.z + amount}
        };
    }
};

// ============================================================
// Color
// ============================================================
struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}

    // Convert to packed uint32 (ABGR for some GPU APIs)
    uint32_t to_packed_abgr() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(r);
    }

    // Convert to float array [0..1]
    void to_float(float out[4]) const {
        out[0] = r / 255.0f;
        out[1] = g / 255.0f;
        out[2] = b / 255.0f;
        out[3] = a / 255.0f;
    }

    // Create from DXF ACI (AutoCAD Color Index) color number
    static Color from_aci(int aci_index);

    // Common colors
    static constexpr Color white() { return {255, 255, 255}; }
    static constexpr Color black() { return {0, 0, 0}; }
    static constexpr Color red()   { return {255, 0, 0}; }
    static constexpr Color green() { return {0, 255, 0}; }
    static constexpr Color blue()  { return {0, 0, 255}; }
    static constexpr Color yellow(){ return {255, 255, 0}; }
    static constexpr Color cyan()  { return {0, 255, 255}; }
    static constexpr Color magenta(){ return {255, 0, 255}; }

    bool operator==(const Color& rhs) const {
        return r == rhs.r && g == rhs.g && b == rhs.b && a == rhs.a;
    }
};

// ============================================================
// Line style types
// ============================================================
enum class LineCapStyle : uint8_t {
    Butt   = 0,
    Round  = 1,
    Square = 2
};

enum class LineJoinStyle : uint8_t {
    Miter = 0,
    Round = 1,
    Bevel = 2
};

struct LinePattern {
    // Dash/gap lengths in drawing units. Empty = solid line.
    // Format: [dash, gap, dash, gap, ...]
    std::vector<float> dash_array;

    bool is_solid() const { return dash_array.empty(); }

    float total_length() const {
        float total = 0;
        for (float v : dash_array) total += v;
        return total;
    }
};

// ============================================================
// Math constants and utilities
// ============================================================
namespace math {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 2.0f * PI;
constexpr float HALF_PI = PI * 0.5f;
constexpr float DEG_TO_RAD = PI / 180.0f;
constexpr float RAD_TO_DEG = 180.0f / PI;

inline float radians(float degrees) { return degrees * DEG_TO_RAD; }
inline float degrees(float radians) { return radians * RAD_TO_DEG; }

// Clamp value to [lo, hi]
template<typename T>
inline T clamp(T val, T lo, T hi) {
    return std::max(lo, std::min(val, hi));
}

// Linear interpolation
inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace math

} // namespace cad
