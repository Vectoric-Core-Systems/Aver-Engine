// Rotate / scale a WORLD transform about a shared pivot: what the Rotate and Scale gizmos do to each
// member of a multi-selection (the pivot is the selection's bounds centre). Pure math, no ImGui, no
// scene::, so it is unit-tested on the CPU.
#pragma once

#include "aver/core/Math.hpp"

namespace aver::editor {

// Rotates `t` by the world-space rotation `dq` about `pivot`: the position swings round the pivot and
// the orientation composes with dq (dq on the left, as the gizmo's single-object rotate does).
inline Transform rotateAboutPivot(const Transform& t, const Vec3& pivot, const Quat& dq) {
    Transform r = t;
    r.position = pivot + dq.rotate(t.position - pivot);
    r.rotation = (dq * t.rotation).normalized();
    return r;
}

// Scales `t` about `pivot` by per-axis factors `f` measured along the axes of `frame` (the gizmo's
// own orientation). The offset from the pivot is stretched in that frame; the object's own scale is
// multiplied component-wise (exact for uniform factors, an approximation for a rotated object under
// a non-uniform one, since a TRS transform cannot hold the resulting shear).
inline Transform scaleAboutPivot(const Transform& t, const Vec3& pivot, const Quat& frame, const Vec3& f) {
    Transform r = t;
    const Quat inv{-frame.x, -frame.y, -frame.z, frame.w};
    Vec3 local = inv.rotate(t.position - pivot);
    local = Vec3{local.x * f.x, local.y * f.y, local.z * f.z};
    r.position = pivot + frame.rotate(local);
    r.scale = Vec3{t.scale.x * f.x, t.scale.y * f.y, t.scale.z * f.z};
    return r;
}

// The factors a scale drag applied, from the gizmo's scale before and after one step. `uniform`
// (the centre handle) uses one factor for all axes so the group keeps its shape even when the anchor
// is non-uniform. Scales are positive (the gizmo clamps them), so the ratios are finite.
inline Vec3 scaleFactors(const Vec3& before, const Vec3& after, bool uniform) {
    if (uniform) {
        const f32 sb = before.x + before.y + before.z;
        const f32 r = sb > 1e-8f ? (after.x + after.y + after.z) / sb : 1.0f;
        return Vec3{r, r, r};
    }
    auto ratio = [](f32 b, f32 a) { return b > 1e-8f ? a / b : 1.0f; };
    return Vec3{ratio(before.x, after.x), ratio(before.y, after.y), ratio(before.z, after.z)};
}

// The rotation that took `before` to `after` (after = delta * before).
inline Quat deltaRotation(const Quat& before, const Quat& after) {
    const Quat inv{-before.x, -before.y, -before.z, before.w};
    return (after * inv).normalized();
}

}  // namespace aver::editor
