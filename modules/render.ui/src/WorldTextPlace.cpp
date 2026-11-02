// WorldTextPlace: billboard and fixed-size maths for world labels. See WorldTextPlace.hpp.
#include "aver/render/ui/WorldTextPlace.hpp"

#include <cmath>

namespace aver::render::ui {
namespace {

// Applies the row-vector inverse view-projection to an NDC point and dehomogenises it: an offset from the eye.
void unproject(const f32 ivp[16], f32 nx, f32 ny, f32 nz, f32 out[3]) {
    const f32 v[4] = {nx, ny, nz, 1.0f};
    f32 r[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) r[j] += v[i] * ivp[i * 4 + j];
    const f32 inv = r[3] != 0.0f ? 1.0f / r[3] : 1.0f;
    out[0] = r[0] * inv; out[1] = r[1] * inv; out[2] = r[2] * inv;
}

f32 len(const f32 v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

bool normalise(f32 v[3]) {
    const f32 l = len(v);
    if (!(l > 1.0e-12f) || !std::isfinite(l)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

f32 dot(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The point's clip-space w under viewProj (row-vector): its depth along the view.
f32 clipW(const f32 vp[16], const f32 p[3]) { return p[0] * vp[3] + p[1] * vp[7] + p[2] * vp[11] + vp[15]; }

} // namespace

bool worldTextCameraAxes(const WorldTextCamera& cam, f32 right[3], f32 up[3], f32 forward[3]) {
    f32 c[3], r[3], u[3], f[3];
    unproject(cam.invViewProjRel, 0.0f, 0.0f, 0.5f, c);
    unproject(cam.invViewProjRel, 0.5f, 0.0f, 0.5f, r);
    unproject(cam.invViewProjRel, 0.0f, 0.5f, 0.5f, u);
    unproject(cam.invViewProjRel, 0.0f, 0.0f, 0.9f, f);
    for (int i = 0; i < 3; ++i) { right[i] = r[i] - c[i]; up[i] = u[i] - c[i]; forward[i] = f[i] - c[i]; }
    return normalise(right) && normalise(up) && normalise(forward);
}

f32 worldTextUnitsPerPixel(const WorldTextCamera& cam, const f32 point[3]) {
    if (cam.viewportHeight == 0) return 0.0f;
    const f32 w = clipW(cam.viewProj, point);
    if (!(w > 1.0e-6f)) return 0.0f;
    const f32 x = point[0] * cam.viewProj[0] + point[1] * cam.viewProj[4] + point[2] * cam.viewProj[8] + cam.viewProj[12];
    const f32 y = point[0] * cam.viewProj[1] + point[1] * cam.viewProj[5] + point[2] * cam.viewProj[9] + cam.viewProj[13];
    const f32 z = point[0] * cam.viewProj[2] + point[1] * cam.viewProj[6] + point[2] * cam.viewProj[10] + cam.viewProj[14];
    const f32 nx = x / w, ny = y / w, nz = z / w;
    f32 a[3], b[3];
    unproject(cam.invViewProjRel, nx, ny, nz, a);
    unproject(cam.invViewProjRel, nx, ny + 2.0f / static_cast<f32>(cam.viewportHeight), nz, b);   // one pixel up
    const f32 d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const f32 l = len(d);
    return std::isfinite(l) ? l : 0.0f;
}

f32 worldTextDepth(const f32 world[16], const WorldTextCamera& cam) {
    const f32 p[3] = {world[12], world[13], world[14]};
    return clipW(cam.viewProj, p);
}

bool worldTextMatrix(const aver::ui::UiWorldText& t, const WorldTextCamera& cam, f32 out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = t.world[i];
    const bool billboard = (t.flags & aver::ui::kWorldTextBillboard) != 0;
    const bool billboardY = !billboard && (t.flags & aver::ui::kWorldTextBillboardY) != 0;
    const bool fixed = t.fixedPxPerUnit > 0.0f;
    if (!billboard && !billboardY && !fixed) return true;

    const f32 origin[3] = {t.world[12], t.world[13], t.world[14]};
    f32 s = 1.0f;
    if (fixed) {
        const f32 upp = worldTextUnitsPerPixel(cam, origin);
        if (!(upp > 0.0f)) return false;
        s = upp * t.fixedPxPerUnit;
    }
    // The label's own axis lengths (its scale), kept.
    const f32 rx[3] = {t.world[0], t.world[1], t.world[2]}, ry[3] = {t.world[4], t.world[5], t.world[6]}, rz[3] = {t.world[8], t.world[9], t.world[10]};
    const f32 sx = len(rx) * s, sy = len(ry) * s, sz = len(rz) * s;

    f32 X[3] = {rx[0], rx[1], rx[2]}, Y[3] = {ry[0], ry[1], ry[2]}, Z[3] = {rz[0], rz[1], rz[2]};
    if (billboard || billboardY) {
        f32 right[3], up[3], fwd[3];
        if (!worldTextCameraAxes(cam, right, up, fwd)) return false;
        if (!(clipW(cam.viewProj, origin) > 1.0e-6f)) return false;
        if (billboard) {
            for (int i = 0; i < 3; ++i) { X[i] = right[i]; Y[i] = up[i]; Z[i] = -fwd[i]; }
        } else {
            // About the label's own up axis: x is the camera's right projected off it, so the text still reads left to right.
            f32 yh[3] = {ry[0], ry[1], ry[2]};
            if (!normalise(yh)) return false;
            const f32 d = dot(right, yh);
            for (int i = 0; i < 3; ++i) X[i] = right[i] - yh[i] * d;
            if (!normalise(X)) return false;
            for (int i = 0; i < 3; ++i) { Y[i] = yh[i]; Z[i] = -fwd[i]; }
        }
    } else {
        if (!normalise(X) || !normalise(Y) || !normalise(Z)) return false;   // fixed size alone: the label's own axes
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = X[i] * sx;
        out[4 + i] = Y[i] * sy;
        out[8 + i] = Z[i] * sz;
    }
    return true;
}

void worldTextPlace(const aver::ui::UiVertex* in, usize vertexCount, const f32 m[16], WorldTextVertex* out) {
    for (usize i = 0; i < vertexCount; ++i) {
        const f32 x = in[i].x, y = in[i].y;
        out[i] = WorldTextVertex{x * m[0] + y * m[4] + m[12], x * m[1] + y * m[5] + m[13], x * m[2] + y * m[6] + m[14], in[i].u, in[i].v, in[i].rgba};
    }
}

} // namespace aver::render::ui
