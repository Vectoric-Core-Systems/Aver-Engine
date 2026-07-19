#pragma once
#include "Types.hpp"
#include <cmath>

// Aver math. Engine space: centimeters, +Z up, +X forward, +Y right, LEFT-handed
// (carried from OpenConstructor — see docs/recon/arch-conventions.md). Matrices are
// row-major; vectors are treated as rows (v * M). Header-only, inline.
namespace aver {

inline constexpr f32 kPi      = 3.14159265358979323846f;
inline constexpr f32 kTwoPi   = 6.28318530717958647692f;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

inline f32 radians(f32 deg) { return deg * kDegToRad; }
inline f32 degrees(f32 rad) { return rad * kRadToDeg; }

// ---------------------------------------------------------------- Vec2
struct Vec2 {
    f32 x = 0, y = 0;
    Vec2() = default;
    Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 b) const { return {x + b.x, y + b.y}; }
    Vec2 operator-(Vec2 b) const { return {x - b.x, y - b.y}; }
    Vec2 operator*(f32 s) const { return {x * s, y * s}; }
};

// ---------------------------------------------------------------- Vec3
struct Vec3 {
    f32 x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(const Vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
    Vec3& operator-=(const Vec3& b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
    Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }

    f32 sizeSquared() const { return x * x + y * y + z * z; }
    f32 size() const { return std::sqrt(sizeSquared()); }

    // Mirrors oc::Vec3 helpers the solver relies on (docs/recon/softbody-solver.md).
    Vec3 getSafeNormal(f32 tolerance = 1e-8f) const {
        const f32 sq = sizeSquared();
        if (sq <= tolerance) return {0, 0, 0};
        const f32 inv = 1.0f / std::sqrt(sq);
        return {x * inv, y * inv, z * inv};
    }
};

inline Vec3 operator*(f32 s, const Vec3& v) { return v * s; }
inline f32  dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f32  dist(const Vec3& a, const Vec3& b) { return (a - b).size(); }
inline f32  distSquared(const Vec3& a, const Vec3& b) { return (a - b).sizeSquared(); }
inline Vec3 lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- Vec4
struct Vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// ---------------------------------------------------------------- Quat (x,y,z,w)
struct Quat {
    f32 x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return {0, 0, 0, 1}; }

    static Quat fromAxisAngle(const Vec3& axis, f32 radians_) {
        const Vec3 a = axis.getSafeNormal();
        const f32 h = radians_ * 0.5f;
        const f32 s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }

    Quat operator*(const Quat& b) const {
        return {
            w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z};
    }

    Quat normalized() const {
        const f32 n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n <= 1e-8f) return identity();
        const f32 inv = 1.0f / n;
        return {x * inv, y * inv, z * inv, w * inv};
    }

    Vec3 rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
};

// ---------------------------------------------------------------- Mat4 (row-major)
struct Mat4 {
    f32 m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Mat4 identity() { return {}; }

    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }
    static Mat4 fromQuat(const Quat& q) {
        const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy + wz);     r.m[0][2] = 2 * (xz - wy);
        r.m[1][0] = 2 * (xy - wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz + wx);
        r.m[2][0] = 2 * (xz + wy);     r.m[2][1] = 2 * (yz - wx);     r.m[2][2] = 1 - 2 * (xx + yy);
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] +
                            m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
        return r;
    }

    Mat4 transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i][j] = m[j][i];
        return r;
    }

    // Left-handed perspective, depth range [0,1] (D3D convention). fovY in radians.
    static Mat4 perspectiveLH(f32 fovY, f32 aspect, f32 zn, f32 zf) {
        const f32 yScale = 1.0f / std::tan(fovY * 0.5f);
        const f32 xScale = yScale / aspect;
        Mat4 r{};
        r.m[0][0] = xScale; r.m[1][1] = yScale;
        r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1.0f;
        r.m[3][2] = -zn * zf / (zf - zn); r.m[3][3] = 0.0f;
        return r;
    }

    // Left-handed look-at (up defaults to +Z, engine's up axis).
    static Mat4 lookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = (target - eye).getSafeNormal();
        const Vec3 s = cross(up, f).getSafeNormal();
        const Vec3 u = cross(f, s);
        Mat4 r;
        r.m[0][0] = s.x; r.m[0][1] = u.x; r.m[0][2] = f.x; r.m[0][3] = 0;
        r.m[1][0] = s.y; r.m[1][1] = u.y; r.m[1][2] = f.y; r.m[1][3] = 0;
        r.m[2][0] = s.z; r.m[2][1] = u.z; r.m[2][2] = f.z; r.m[2][3] = 0;
        r.m[3][0] = -dot(s, eye); r.m[3][1] = -dot(u, eye); r.m[3][2] = -dot(f, eye); r.m[3][3] = 1;
        return r;
    }
};

// ---------------------------------------------------------------- Transform / AABB
struct Transform {
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();
    Vec3 scale{1, 1, 1};

    Mat4 toMatrix() const {
        return Mat4::scale(scale) * Mat4::fromQuat(rotation) * Mat4::translation(position);
    }
};

struct AABB {
    Vec3 min{0, 0, 0};
    Vec3 max{0, 0, 0};
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extent() const { return (max - min) * 0.5f; }
    void expand(const Vec3& p) {
        min.x = p.x < min.x ? p.x : min.x; min.y = p.y < min.y ? p.y : min.y; min.z = p.z < min.z ? p.z : min.z;
        max.x = p.x > max.x ? p.x : max.x; max.y = p.y > max.y ? p.y : max.y; max.z = p.z > max.z ? p.z : max.z;
    }
};

} // namespace aver
