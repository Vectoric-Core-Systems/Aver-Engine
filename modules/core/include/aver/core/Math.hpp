#pragma once
#include "Types.hpp"
#include <cmath>

// Aver math. Engine space: centimeters, +Z up, +X forward, +Y right, LEFT-handed.
// Matrices are row-major; vectors are treated as rows (v * M). Header-only, inline.
namespace aver {

inline constexpr f32 kPi      = 3.14159265358979323846f;
inline constexpr f32 kTwoPi   = 6.28318530717958647692f;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

// Degrees to radians.
inline f32 radians(f32 deg) { return deg * kDegToRad; }
// Radians to degrees.
inline f32 degrees(f32 rad) { return rad * kRadToDeg; }

// ---------------------------------------------------------------- Vec2
// Two floats.
struct Vec2 {
    f32 x = 0, y = 0;
    Vec2() = default;
    Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 b) const { return {x + b.x, y + b.y}; }
    Vec2 operator-(Vec2 b) const { return {x - b.x, y - b.y}; }
    Vec2 operator*(f32 s) const { return {x * s, y * s}; }
};

// ---------------------------------------------------------------- Vec3
// Three floats, and the engine's position/direction type.
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

    // Unit vector, or zero when shorter than the tolerance.
    Vec3 getSafeNormal(f32 tolerance = 1e-8f) const {
        const f32 sq = sizeSquared();
        if (sq <= tolerance) return {0, 0, 0};
        const f32 inv = 1.0f / std::sqrt(sq);
        return {x * inv, y * inv, z * inv};
    }
};

inline Vec3 operator*(f32 s, const Vec3& v) { return v * s; }
// Dot product.
inline f32  dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// Cross product.
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// Distance between two points.
inline f32  dist(const Vec3& a, const Vec3& b) { return (a - b).size(); }
// Squared distance between two points.
inline f32  distSquared(const Vec3& a, const Vec3& b) { return (a - b).sizeSquared(); }
// Linear interpolation from a to b.
inline Vec3 lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- Vec4
// Four floats.
struct Vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// ---------------------------------------------------------------- Quat (x,y,z,w)
// A rotation, stored x, y, z, w.
struct Quat {
    f32 x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}

    // The zero rotation.
    static Quat identity() { return {0, 0, 0, 1}; }

    // Rotation of `radians_` about an axis.
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

    // Unit quaternion, or identity when degenerate.
    Quat normalized() const {
        const f32 n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n <= 1e-8f) return identity();
        const f32 inv = 1.0f / n;
        return {x * inv, y * inv, z * inv, w * inv};
    }

    // Applies this rotation to a vector.
    Vec3 rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }

    // Shortest-arc spherical interpolation. Falls back to a normalised lerp when the two are nearly
    // parallel, where the sine denominator loses all its precision.
    static Quat slerp(const Quat& a, const Quat& b, f32 t) {
        f32 d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        // q and -q are the same rotation, so flip to take the short way round.
        Quat e = b;
        if (d < 0.0f) { e = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
        if (d > 0.9995f) {
            return Quat{a.x + (e.x - a.x) * t, a.y + (e.y - a.y) * t,
                        a.z + (e.z - a.z) * t, a.w + (e.w - a.w) * t}.normalized();
        }
        const f32 theta = std::acos(d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d));
        const f32 s = std::sin(theta);
        const f32 wa = std::sin((1.0f - t) * theta) / s;
        const f32 wb = std::sin(t * theta) / s;
        return {a.x * wa + e.x * wb, a.y * wa + e.y * wb,
                a.z * wa + e.z * wb, a.w * wa + e.w * wb};
    }
};

// ---------------------------------------------------------------- Mat4 (row-major)
// A 4x4 row-major matrix. Vectors multiply on the left (v * M).
struct Mat4 {
    f32 m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    // The identity matrix.
    static Mat4 identity() { return {}; }

    // Pure translation.
    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
        return r;
    }
    // Pure per-axis scale.
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }
    // Rotation matrix for a quaternion.
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

    // General 4x4 inverse. Returns identity if singular.
    Mat4 inverse() const {
        const f32* a = &m[0][0];
        f32 inv[16], det;
        inv[0]  =  a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
        inv[4]  = -a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
        inv[8]  =  a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
        inv[12] = -a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
        inv[1]  = -a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
        inv[5]  =  a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
        inv[9]  = -a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
        inv[13] =  a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
        inv[2]  =  a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
        inv[6]  = -a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
        inv[10] =  a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
        inv[14] = -a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
        inv[3]  = -a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
        inv[7]  =  a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
        inv[11] = -a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
        inv[15] =  a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
        det = a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
        Mat4 r;
        if (det == 0.0f) return r;
        det = 1.0f / det;
        for (int i = 0; i < 16; ++i) (&r.m[0][0])[i] = inv[i] * det;
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

    // Left-handed look-at view matrix, from a DIRECTION rather than a target.
    //
    // PREFER THIS WHENEVER THE CALLER ALREADY HAS A DIRECTION, and a camera driven by yaw/pitch
    // always does. Composing `eye + dir` only to subtract `eye` back off inside lookAtLH is not
    // free in f32: when the eye is far from the origin and a direction component is small, the
    // addition ABSORBS it outright. eye.x = -42000 has an f32 ulp of about 0.005, so a dir.x of
    // 0.0017 -- a look direction a tenth of a degree off vertical -- rounds away to nothing, and
    // `target - eye` hands back a direction the caller never asked for.
    //
    // That is not a rounding nuisance, it is a collapse. Lose both horizontal components and f
    // becomes exactly (0,0,1), where cross(up, f) is the zero vector, getSafeNormal returns zero,
    // and s and u come out all-zero -- a view matrix with no basis at all. Measured: an eye at
    // (-42000, 17000, -900) looking 89.9 degrees up produced exactly that, and it is the reason
    // CameraFactorTest's cameras 100-103 failed. The window scales with distance from the origin --
    // about 0.14 degrees of vertical at 420 m out, about 1.4 degrees at 4 km.
    static Mat4 lookAtDirLH(const Vec3& eye, const Vec3& dir, const Vec3& up) {
        const Vec3 f = dir.getSafeNormal();
        const Vec3 s = cross(up, f).getSafeNormal();
        const Vec3 u = cross(f, s);
        Mat4 r;
        r.m[0][0] = s.x; r.m[0][1] = u.x; r.m[0][2] = f.x; r.m[0][3] = 0;
        r.m[1][0] = s.y; r.m[1][1] = u.y; r.m[1][2] = f.y; r.m[1][3] = 0;
        r.m[2][0] = s.z; r.m[2][1] = u.z; r.m[2][2] = f.z; r.m[2][3] = 0;
        r.m[3][0] = -dot(s, eye); r.m[3][1] = -dot(u, eye); r.m[3][2] = -dot(f, eye); r.m[3][3] = 1;
        return r;
    }

    // Left-handed look-at view matrix, from a target. Correct for an eye and a target that are
    // genuinely separate points (a shadow view framing a bounds centre, say). If the caller is
    // about to write `lookAtLH(p, p + d, up)`, call lookAtDirLH(p, d, up) instead and read that
    // function's comment for what the round trip through a target costs.
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
// Position, rotation and scale.
struct Transform {
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();
    Vec3 scale{1, 1, 1};

    // The combined scale-rotate-translate matrix.
    Mat4 toMatrix() const {
        return Mat4::scale(scale) * Mat4::fromQuat(rotation) * Mat4::translation(position);
    }
};

// The TRS a matrix was built from. THE INVERSE OF Transform::toMatrix, and only that: it assumes
// the matrix is scale-then-rotate-then-translate with no shear, which is what every matrix in this
// engine composed from a Transform is.
//
// A MIRRORED MATRIX CANNOT BE REPRESENTED and is not silently accepted: a negative determinant is
// a reflection, no quaternion encodes one, and the usual trick of pushing the sign into one scale
// axis picks an axis arbitrarily and produces a DIFFERENT matrix than the one passed in. The X
// scale carries the sign here so the determinant at least survives, and the caller is expected to
// know it is outside the contract -- a rig with a mirrored bone is a broken rig, not a case to
// support.
inline Transform transformFromMatrix(const Mat4& m) {
    Transform t;
    t.position = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};

    Vec3 ax{m.m[0][0], m.m[0][1], m.m[0][2]};
    Vec3 ay{m.m[1][0], m.m[1][1], m.m[1][2]};
    Vec3 az{m.m[2][0], m.m[2][1], m.m[2][2]};
    f32 sx = std::sqrt(ax.x*ax.x + ax.y*ax.y + ax.z*ax.z);
    const f32 sy = std::sqrt(ay.x*ay.x + ay.y*ay.y + ay.z*ay.z);
    const f32 sz = std::sqrt(az.x*az.x + az.y*az.y + az.z*az.z);

    // det < 0 is a reflection; see the note above.
    const f32 det = ax.x * (ay.y*az.z - ay.z*az.y)
                  - ax.y * (ay.x*az.z - ay.z*az.x)
                  + ax.z * (ay.x*az.y - ay.y*az.x);
    if (det < 0.0f) sx = -sx;

    t.scale = Vec3{sx, sy, sz};

    // A ZERO SCALE AXIS LEAVES THE ROTATION UNDEFINED, so it falls back to identity rather than
    // dividing by zero and filling the quaternion with NaN -- which would then propagate into every
    // transform downstream and be attributed to something else entirely.
    if (std::fabs(sx) < 1e-8f || sy < 1e-8f || sz < 1e-8f) return t;

    Mat4 r = Mat4::identity();
    r.m[0][0] = ax.x/sx; r.m[0][1] = ax.y/sx; r.m[0][2] = ax.z/sx;
    r.m[1][0] = ay.x/sy; r.m[1][1] = ay.y/sy; r.m[1][2] = ay.z/sy;
    r.m[2][0] = az.x/sz; r.m[2][1] = az.y/sz; r.m[2][2] = az.z/sz;

    // Shepperd's method: pick the branch whose divisor is largest, so no branch divides by a small
    // number. The naive single-branch form loses most of its precision near a 180-degree rotation.
    const f32 tr = r.m[0][0] + r.m[1][1] + r.m[2][2];
    if (tr > 0.0f) {
        const f32 s = std::sqrt(tr + 1.0f) * 2.0f;
        t.rotation = Quat{(r.m[1][2] - r.m[2][1]) / s, (r.m[2][0] - r.m[0][2]) / s,
                          (r.m[0][1] - r.m[1][0]) / s, 0.25f * s};
    } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
        const f32 s = std::sqrt(1.0f + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2.0f;
        t.rotation = Quat{0.25f * s, (r.m[1][0] + r.m[0][1]) / s, (r.m[2][0] + r.m[0][2]) / s,
                          (r.m[1][2] - r.m[2][1]) / s};
    } else if (r.m[1][1] > r.m[2][2]) {
        const f32 s = std::sqrt(1.0f + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2.0f;
        t.rotation = Quat{(r.m[1][0] + r.m[0][1]) / s, 0.25f * s, (r.m[2][1] + r.m[1][2]) / s,
                          (r.m[2][0] - r.m[0][2]) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2.0f;
        t.rotation = Quat{(r.m[2][0] + r.m[0][2]) / s, (r.m[2][1] + r.m[1][2]) / s, 0.25f * s,
                          (r.m[0][1] - r.m[1][0]) / s};
    }
    return t;
}

// An axis-aligned bounding box.
struct AABB {
    Vec3 min{0, 0, 0};
    Vec3 max{0, 0, 0};
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extent() const { return (max - min) * 0.5f; }
    // Grows the box to contain a point.
    void expand(const Vec3& p) {
        min.x = p.x < min.x ? p.x : min.x; min.y = p.y < min.y ? p.y : min.y; min.z = p.z < min.z ? p.z : min.z;
        max.x = p.x > max.x ? p.x : max.x; max.y = p.y > max.y ? p.y : max.y; max.z = p.z > max.z ? p.z : max.z;
    }
};

} // namespace aver
