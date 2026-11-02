#pragma once
// A camera built the way the engine's is (row-major matrices, row vectors, left-handed, a vertical fov), for tests of code that
// reads IDevice::camera(): the view-projection, and the inverse of the same with the view's translation removed.
#include "aver/core/Types.hpp"
#include "aver/render/ui/WorldTextPlace.hpp"

#include <cmath>
#include <utility>

namespace testcam {

using aver::f32;
using aver::u32;

struct M4 {
    f32 m[16] = {};
};

inline M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

// Gauss-Jordan; the matrices here are well conditioned.
inline M4 inverse(const M4& a) {
    double w[4][8] = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) w[i][j] = a.m[i * 4 + j];
        w[i][4 + i] = 1.0;
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(w[r][c]) > std::fabs(w[p][c])) p = r;
        for (int j = 0; j < 8; ++j) std::swap(w[c][j], w[p][j]);
        const double d = w[c][c];
        for (int j = 0; j < 8; ++j) w[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = w[r][c];
            for (int j = 0; j < 8; ++j) w[r][j] -= f * w[c][j];
        }
    }
    M4 r;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i * 4 + j] = static_cast<f32>(w[i][4 + j]);
    return r;
}

inline void norm3(f32 v[3]) {
    const f32 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; ++i) v[i] /= l;
}

inline aver::render::ui::WorldTextCamera makeCamera(const f32 eye[3], const f32 target[3], const f32 upHint[3], f32 fovY, f32 aspect, u32 height) {
    f32 f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    norm3(f);
    f32 r[3] = {upHint[1] * f[2] - upHint[2] * f[1], upHint[2] * f[0] - upHint[0] * f[2], upHint[0] * f[1] - upHint[1] * f[0]};
    norm3(r);
    f32 u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    M4 vRel, v;
    for (int i = 0; i < 3; ++i) { vRel.m[i * 4 + 0] = r[i]; vRel.m[i * 4 + 1] = u[i]; vRel.m[i * 4 + 2] = f[i]; }
    vRel.m[15] = 1.0f;
    v = vRel;
    v.m[12] = -(eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2]);
    v.m[13] = -(eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2]);
    v.m[14] = -(eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2]);
    v.m[15] = 1.0f;
    const f32 ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect, zn = 0.1f, zf = 1000.0f;
    M4 p;
    p.m[0] = xs; p.m[5] = ys; p.m[10] = zf / (zf - zn); p.m[11] = 1.0f; p.m[14] = -zn * zf / (zf - zn);
    aver::render::ui::WorldTextCamera cam;
    const M4 vp = mul(v, p), rel = inverse(mul(vRel, p));
    for (int i = 0; i < 16; ++i) { cam.viewProj[i] = vp.m[i]; cam.invViewProjRel[i] = rel.m[i]; }
    for (int i = 0; i < 3; ++i) cam.eye[i] = eye[i];
    cam.viewportHeight = height;
    return cam;
}

} // namespace testcam
