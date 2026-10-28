// Camera projections beyond the symmetric perspective one (G4 R4, docs/plans/fabric/G4_RENDER.md): orthographic, off-centre frustum, and
// Godot's keep_aspect. Header-only and pure so the host, the renderers and the tests share one set of formulas.
//
// Row-vector, left-handed, depth [0,1] like Mat4::perspectiveLH. The default CameraProjection (perspective, keep height) builds
// EXACTLY Mat4::perspectiveLH(fov, aspect, near, far), so a camera that never asks for anything else is unchanged bit for bit.
#pragma once
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>

namespace aver {

enum class ProjectionKind : i32 { Perspective = 0, Orthographic = 1, Frustum = 2 };

inline ProjectionKind projectionKindFromInt(i32 v) {
    return v == 1 ? ProjectionKind::Orthographic : v == 2 ? ProjectionKind::Frustum : ProjectionKind::Perspective;
}

// What a camera asks of its projection. Lengths in centimetres. `fovRad` is the field of view of the KEPT dimension (vertical unless
// keepWidth), as Godot's `fov` is. `sizeCm` is the width or height (per keepWidth) of the orthographic view, or of the frustum at its
// near plane. `offsetXCm/offsetYCm` shift a frustum's window at the near plane (Godot's frustum_offset).
struct CameraProjection {
    ProjectionKind kind = ProjectionKind::Perspective;
    f32 fovRad = 1.0471975512f;
    f32 sizeCm = 100.0f;
    f32 offsetXCm = 0.0f;
    f32 offsetYCm = 0.0f;
    bool keepWidth = false;
    f32 nearCm = 2.0f;
    f32 farCm = 200000.0f;

    // True when this is the plain symmetric perspective camera every host built before G4 R4.
    bool isDefaultPerspective() const { return kind == ProjectionKind::Perspective && !keepWidth; }
};

// D3DXMatrixPerspectiveOffCenterLH: the window l..r, b..t is measured at the near plane.
inline Mat4 perspectiveOffCenterLH(f32 l, f32 r, f32 b, f32 t, f32 zn, f32 zf) {
    Mat4 m{};
    m.m[0][0] = 2.0f * zn / (r - l);
    m.m[1][1] = 2.0f * zn / (t - b);
    m.m[2][0] = (l + r) / (l - r);
    m.m[2][1] = (t + b) / (b - t);
    m.m[2][2] = zf / (zf - zn);
    m.m[2][3] = 1.0f;
    m.m[3][2] = zn * zf / (zn - zf);
    m.m[3][3] = 0.0f;
    return m;
}

// D3DXMatrixOrthoOffCenterLH.
inline Mat4 orthographicOffCenterLH(f32 l, f32 r, f32 b, f32 t, f32 zn, f32 zf) {
    Mat4 m{};
    m.m[0][0] = 2.0f / (r - l);
    m.m[1][1] = 2.0f / (t - b);
    m.m[2][2] = 1.0f / (zf - zn);
    m.m[3][0] = (l + r) / (l - r);
    m.m[3][1] = (t + b) / (b - t);
    m.m[3][2] = zn / (zn - zf);
    m.m[3][3] = 1.0f;
    return m;
}

// Half extents of the view: perspective, at distance 1 (tangents of the half angles); orthographic, in centimetres at any distance;
// frustum, in centimetres at the near plane. Godot's rule: keep_aspect names the dimension `fov` / `size` describes.
inline void projectionHalfExtents(const CameraProjection& p, f32 aspect, f32& halfW, f32& halfH) {
    if (!(aspect > 0.0f)) aspect = 1.0f;
    if (p.kind == ProjectionKind::Perspective) {
        const f32 half = std::tan(p.fovRad * 0.5f);
        if (p.keepWidth) { halfW = half; halfH = half / aspect; }
        else             { halfH = half; halfW = half * aspect; }
    } else {
        const f32 half = std::max(p.sizeCm, 1e-4f) * 0.5f;
        if (p.keepWidth) { halfW = half; halfH = half / aspect; }
        else             { halfH = half; halfW = half * aspect; }
    }
}

inline Mat4 projectionMatrix(const CameraProjection& p, f32 aspect) {
    if (p.isDefaultPerspective()) return Mat4::perspectiveLH(p.fovRad, aspect, p.nearCm, p.farCm);
    f32 hw = 1.0f, hh = 1.0f;
    projectionHalfExtents(p, aspect, hw, hh);
    switch (p.kind) {
    case ProjectionKind::Orthographic:
        return orthographicOffCenterLH(-hw, hw, -hh, hh, p.nearCm, p.farCm);
    case ProjectionKind::Frustum:
        return perspectiveOffCenterLH(-hw + p.offsetXCm, hw + p.offsetXCm, -hh + p.offsetYCm, hh + p.offsetYCm, p.nearCm, p.farCm);
    case ProjectionKind::Perspective:
    default:
        // keep width: a symmetric window at the near plane with the right tangents
        return perspectiveOffCenterLH(-hw * p.nearCm, hw * p.nearCm, -hh * p.nearCm, hh * p.nearCm, p.nearCm, p.farCm);
    }
}

// True for a view-projection whose last column carries no view depth: an orthographic camera (clip w stays 1). A perspective one has the
// view's forward axis in the first three entries of that column (a unit vector); an unset (all-zero) matrix is neither.
inline bool isOrthographicViewProj(const f32 vp[16]) {
    return std::fabs(vp[3]) + std::fabs(vp[7]) + std::fabs(vp[11]) < 1e-6f && std::fabs(vp[15] - 1.0f) < 1e-4f;
}

// Row-vector point through a projective matrix with the divide; false when w is not positive-finite.
inline bool projectToNdc(const Mat4& vp, const Vec3& p, f32& x, f32& y, f32& z) {
    f32 h[4];
    for (int c = 0; c < 4; ++c) h[c] = p.x * vp.m[0][c] + p.y * vp.m[1][c] + p.z * vp.m[2][c] + vp.m[3][c];
    if (!(h[3] > 1e-12f)) return false;
    x = h[0] / h[3]; y = h[1] / h[3]; z = h[2] / h[3];
    return true;
}

// The world ray through an NDC point, for either projection: the point on the near plane and the unit direction toward the far plane
// (parallel rays for an orthographic camera, rays through the eye for a perspective one). `invVP` is the ABSOLUTE inverse.
inline bool unprojectRay(const Mat4& invVP, f32 ndcX, f32 ndcY, Vec3& origin, Vec3& dir) {
    auto at = [&](f32 z, Vec3& out) {
        f32 h[4];
        for (int c = 0; c < 4; ++c) h[c] = ndcX * invVP.m[0][c] + ndcY * invVP.m[1][c] + z * invVP.m[2][c] + invVP.m[3][c];
        if (std::fabs(h[3]) < 1e-20f) return false;
        out = Vec3{h[0] / h[3], h[1] / h[3], h[2] / h[3]};
        return true;
    };
    Vec3 n, f;
    if (!at(0.0f, n) || !at(1.0f, f)) return false;
    origin = n;
    dir = (f - n).getSafeNormal();
    return dir.sizeSquared() > 0.5f;
}

} // namespace aver
