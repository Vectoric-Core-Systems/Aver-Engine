#pragma once
// Godot-space maths for the scene converter: double precision, right-handed, +Y up, metres. Nothing here knows the
// engine; FabricConvert applies the shared axis map (aver/formats/AxisMap.hpp) at the very end.
#include <algorithm>
#include <cmath>

namespace aver::fabric {

struct V3 {
    double x = 0, y = 0, z = 0;
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
    V3 cross(const V3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(dot(*this)); }
    V3 normalized() const { const double l = length(); return l > 1e-12 ? V3{x / l, y / l, z / l} : V3{}; }
};

struct Q4 {
    double x = 0, y = 0, z = 0, w = 1;
};

// m[row][col]; the basis axes are the COLUMNS, as in Godot's text form after its row-by-row listing.
struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    V3 column(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
    void setColumn(int c, const V3& v) { m[0][c] = v.x; m[1][c] = v.y; m[2][c] = v.z; }

    M3 operator*(const M3& o) const {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double s = 0;
                for (int k = 0; k < 3; ++k) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
    V3 operator*(const V3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    double determinant() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
    static M3 diag(const V3& s) { M3 r; r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z; return r; }
    static M3 rotX(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
    static M3 rotY(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
    static M3 rotZ(double a) { M3 r; const double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }
    M3 transposed() const { M3 r; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i]; return r; }
};

// Godot's Basis::from_euler, order as the EulerOrder enum: 0 XYZ, 1 XZY, 2 YXZ (the default), 3 YZX, 4 ZXY, 5 ZYX.
inline M3 basisFromEuler(const V3& e, int order) {
    const M3 x = M3::rotX(e.x), y = M3::rotY(e.y), z = M3::rotZ(e.z);
    switch (order) {
        case 0: return x * (y * z);
        case 1: return x * (z * y);
        case 3: return y * (z * x);
        case 4: return z * (x * y);
        case 5: return z * (y * x);
        default: return y * (x * z);
    }
}

inline M3 basisFromQuat(const Q4& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const double s = n > 1e-12 ? 1.0 / n : 0.0;
    const double x = q.x * s, y = q.y * s, z = q.z * s, w = q.w * s;
    M3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

// A proper rotation matrix (columns unit and orthogonal) to a quaternion.
inline Q4 quatFromRotation(const M3& r) {
    Q4 q;
    const double t = r.m[0][0] + r.m[1][1] + r.m[2][2];
    if (t > 0) {
        const double s = std::sqrt(t + 1.0) * 2;
        q.w = 0.25 * s;
        q.x = (r.m[2][1] - r.m[1][2]) / s;
        q.y = (r.m[0][2] - r.m[2][0]) / s;
        q.z = (r.m[1][0] - r.m[0][1]) / s;
    } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
        const double s = std::sqrt(1.0 + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2;
        q.w = (r.m[2][1] - r.m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (r.m[0][1] + r.m[1][0]) / s;
        q.z = (r.m[0][2] + r.m[2][0]) / s;
    } else if (r.m[1][1] > r.m[2][2]) {
        const double s = std::sqrt(1.0 + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2;
        q.w = (r.m[0][2] - r.m[2][0]) / s;
        q.x = (r.m[0][1] + r.m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (r.m[1][2] + r.m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2;
        q.w = (r.m[1][0] - r.m[0][1]) / s;
        q.x = (r.m[0][2] + r.m[2][0]) / s;
        q.y = (r.m[1][2] + r.m[2][1]) / s;
        q.z = 0.25 * s;
    }
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-12) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    return q;
}

// A node's local transform: origin plus the full basis (rotation times scale along the local axes).
struct Xform {
    V3 pos;
    M3 basis;

    // Godot's Basis::get_scale: the column lengths, all negated when the determinant is negative.
    V3 scale() const {
        const double sign = basis.determinant() < 0 ? -1.0 : 1.0;
        return {sign * basis.column(0).length(), sign * basis.column(1).length(), sign * basis.column(2).length()};
    }
    // The basis with the scale removed. Not exactly a rotation when the basis is sheared.
    M3 rotation() const {
        const V3 s = scale();
        M3 r;
        for (int c = 0; c < 3; ++c) {
            const double d = c == 0 ? s.x : c == 1 ? s.y : s.z;
            r.setColumn(c, d != 0 ? basis.column(c) * (1.0 / d) : basis.column(c));
        }
        return r;
    }
    // True when rotation()/scale() cannot rebuild the basis (shear, or a collapsed axis).
    bool sheared() const {
        const M3 r = rotation();
        const M3 rtr = r.transposed() * r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (std::fabs(rtr.m[i][j] - (i == j ? 1.0 : 0.0)) > 1e-4) return true;
        return false;
    }
    void setScale(const V3& s) { basis = rotation() * M3::diag(s); }
    void setRotation(const M3& r) { basis = r * M3::diag(scale()); }
    Q4 quaternion() const { return quatFromRotation(rotation()); }
    // Godot's Node3D::to_global with a parent: this transform expressed in the parent's parent space.
    Xform composed(const Xform& child) const {
        Xform r;
        r.pos = pos + basis * child.pos;
        r.basis = basis * child.basis;
        return r;
    }
};

}  // namespace aver::fabric
