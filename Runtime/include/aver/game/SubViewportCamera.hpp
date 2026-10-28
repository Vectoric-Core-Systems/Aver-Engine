// The camera of a sub-viewport (G4 R5): the pose of an entity and the lens of its CCamera, as the view-projection a secondary view draws with. Header-only and
// small so the host and the tests share one answer. The conventions are the play camera's (GameCamera.hpp): an entity looks along its local +X (row 0 of the world
// matrix), its +Z is up, row 3 is the eye.
#pragma once
#include "aver/core/CameraProjection.hpp"
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/Components.hpp"
#  include "aver/scene/World.hpp"
#endif

#include <cmath>

namespace aver::game {

struct ViewCamera {
    Mat4 viewProj;
    Vec3 eye{0, 0, 0};
    Vec3 forward{1, 0, 0};
    Vec3 up{0, 0, 1};
    CameraProjection lens;
    u32 cullInv = 0;   // CCamera::cullInv: the camera's cull mask, stored inverted (0 = every layer)
};

// The view-projection of a view looking from `eye` along `forward` with the given up and lens. `up` is made orthogonal to `forward`; a view looking straight along
// it falls back to world +Z, and then +X.
inline ViewCamera makeViewCamera(const Vec3& eye, const Vec3& forward, const Vec3& upHint, const CameraProjection& lens, f32 aspect) {
    ViewCamera v;
    v.eye = eye;
    v.forward = forward.getSafeNormal();
    if (v.forward.sizeSquared() < 0.5f) v.forward = Vec3{1, 0, 0};
    Vec3 up = upHint - v.forward * dot(upHint, v.forward);
    if (up.sizeSquared() < 1e-6f) {
        const Vec3 z{0, 0, 1};
        up = z - v.forward * dot(z, v.forward);
        if (up.sizeSquared() < 1e-6f) {
            const Vec3 x{1, 0, 0};
            up = x - v.forward * dot(x, v.forward);
        }
    }
    v.up = up.getSafeNormal();
    v.lens = lens;
    const Mat4 view = Mat4::lookAtDirLH(eye, v.forward, v.up);
    v.viewProj = view * projectionMatrix(lens, aspect > 0.0f ? aspect : 1.0f);
    return v;
}

#if AVER_MODULE_SCENE
// The view of a camera entity. False when the entity is gone. A CCamera supplies the lens, the clip planes, the offsets (h_offset / v_offset slide the eye along the
// camera's own right and up) and the cull mask; an entity with none looks with the play camera's defaults (60 degrees, 2 cm to 2 km).
inline bool viewCameraOf(scene::World& w, scene::Entity e, f32 aspect, ViewCamera& out) {
    if (e == scene::kInvalidEntity || !w.valid(e)) return false;
    const Mat4& m = w.worldMatrix(e);
    Vec3 eye{m.m[3][0], m.m[3][1], m.m[3][2]};
    const Vec3 fwd{m.m[0][0], m.m[0][1], m.m[0][2]};
    const Vec3 up{m.m[2][0], m.m[2][1], m.m[2][2]};

    CameraProjection lens;
    lens.fovRad = radians(60.0f);
    lens.nearCm = 2.0f;
    lens.farCm = 200000.0f;
    u32 cull = 0;
    f32 hOff = 0.0f, vOff = 0.0f;
    if (const auto* cam = w.component<scene::CCamera>(e, scene::kComponentCamera)) {
        lens.kind = projectionKindFromInt(cam->projection);
        lens.keepWidth = (cam->flags & scene::kCameraKeepWidth) != 0;
        lens.fovRad = cam->fovYRad > 0.01f ? cam->fovYRad : lens.fovRad;
        lens.nearCm = cam->nearCm > 0.0f ? cam->nearCm : lens.nearCm;
        lens.farCm = cam->farCm > lens.nearCm ? cam->farCm : lens.farCm;
        lens.sizeCm = cam->sizeCm > 0.0f ? cam->sizeCm : 100.0f;
        lens.offsetXCm = cam->offsetXCm;
        lens.offsetYCm = cam->offsetYCm;
        cull = cam->cullInv;
        hOff = cam->hOffsetCm;
        vOff = cam->vOffsetCm;
    }
    ViewCamera probe = makeViewCamera(eye, fwd, up, lens, aspect);
    if (hOff != 0.0f || vOff != 0.0f) {
        const Vec3 right = cross(probe.up, probe.forward).getSafeNormal();   // left-handed: right = up x forward
        eye = eye + right * hOff + probe.up * vOff;
        probe = makeViewCamera(eye, fwd, up, lens, aspect);
    }
    out = probe;
    out.cullInv = cull;
    return true;
}
#endif

} // namespace aver::game
