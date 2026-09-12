#pragma once
// The foliage brush's "Align to slope" option: computing a placed instance's orientation.
//
// THE DEFECT. foliageAlignToNormal_ (SandboxApp.cpp) was read NOWHERE: foliagePlaceOne computed a
// random yaw and nothing else, so the checkbox was dead UI, and its own comment claimed the option
// "is applied gently" when it did not exist at all.
//
// EXTRACTED, not left inline, for DropPlacementTest's and EditorEulerTest's own reason (see their
// header comments): this is placement arithmetic that is easy to get backwards and does not crash or
// log when it is, and SandboxApp.cpp cannot be compiled into a headless test target at all (it is a
// 27k-line WinMain executable, not a library). This header needs no ImGui, no scene::World, no
// Engine -- only the heightfield read API every foliage placement already calls.
//
// THE NORMAL COMES FROM FOUR EXTRA HEIGHT SAMPLES, NOT FROM THE BRUSH'S OWN RAYCAST HIT. The brush's
// raycastHeightfield (HeightfieldRay.hpp) finds one point at the CENTRE of the brush disc, and
// HeightfieldHit carries only a position and a distance -- no normal at all. Even if it did carry
// one, reusing it here would be the wrong normal for almost every instance: a brush can be up to
// 100 m across (see foliageRadiusCm_'s own slider range), and each instance already samples its OWN
// (x, y) via surfaceHeightAt, not the brush centre's -- see that function's own header on why a
// vertical per-candidate query, not a re-cast ray, is the right tool here too. Four more bilinear
// lookups per placement is the honest cost of getting the actual slope under each instance, and it
// is cheap next to everything else one placement already does (an entity create, a component add,
// an undo-batch append).
#include "aver/core/Math.hpp"
#include "aver/formats/OcLand.hpp"
#include "aver/landscape/HeightfieldRay.hpp"

#include <cmath>

namespace aver::editor {

// Central-difference surface normal at world (x, y), unit length. A heightfield is a function
// z = h(x, y), so the result always has a positive Z component -- there is no "upside-down" terrain
// normal to guard against here, unlike a general mesh.
//
// False (and outNormal left at world-up) wherever any of the four probe samples falls outside the
// section's footprint -- the same condition surfaceHeightAt itself refuses, for the reason ITS OWN
// header gives (clamping there would smear the section's rim slope across candidates beyond it).
inline bool foliageSurfaceNormal(const fmt::OcLandData& land, f32 x, f32 y, f32 eps, Vec3& outNormal) {
    outNormal = Vec3{0.0f, 0.0f, 1.0f};
    f32 hxp, hxm, hyp, hym;
    if (!landscape::surfaceHeightAt(land, x + eps, y, hxp)) return false;
    if (!landscape::surfaceHeightAt(land, x - eps, y, hxm)) return false;
    if (!landscape::surfaceHeightAt(land, x, y + eps, hyp)) return false;
    if (!landscape::surfaceHeightAt(land, x, y - eps, hym)) return false;

    // Tangent along X and along Y; their cross product is the surface normal (up to sign/scale) --
    // this is the standard n ~ (-dh/dx, -dh/dy, 1) heightfield-normal identity, arrived at via two
    // tangents rather than typed directly so it needs no separate sign check: the Z component of
    // tx (x) cross ty (y) is always +4*eps*eps, strictly positive for eps > 0.
    const Vec3 tx{2.0f * eps, 0.0f, hxp - hxm};
    const Vec3 ty{0.0f, 2.0f * eps, hyp - hym};
    const Vec3 n = cross(tx, ty).getSafeNormal();
    if (n.sizeSquared() < 0.5f) return false;   // degenerate: eps too small, or a NaN height
    outNormal = n;
    return true;
}

// The rotation a foliage instance is placed with: `yaw` about world-up, then -- only when
// `alignToNormal` is true and a normal can be sampled at (x, y) -- tilted so the instance's own
// up-vector matches the terrain's. Falls back to yaw-only (matching `alignToNormal == false`) when
// off the section's footprint, rather than asserting: a placement attempt already checked
// surfaceHeightAt for its OWN (x, y) before calling this, so this can only disagree at the very edge
// of a section, and standing upright there is a far better failure than refusing the placement.
//
// Yaw is composed FIRST (innermost), tilt SECOND (outermost): rotating world-up by a pure yaw is a
// no-op on that axis, so the instance's up-vector after both is exactly the sampled normal for every
// yaw value -- a random azimuth keeps reading as a random azimuth once the whole thing is tilted,
// rather than being measured against axes the slope has already rotated away from.
inline Quat foliagePlacementRotation(const fmt::OcLandData& land, f32 x, f32 y, f32 eps,
                                      f32 yaw, bool alignToNormal) {
    const Quat yawQ{0.0f, 0.0f, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
    if (!alignToNormal) return yawQ;

    Vec3 n;
    if (!foliageSurfaceNormal(land, x, y, eps, n)) return yawQ;

    const Vec3 up{0.0f, 0.0f, 1.0f};
    const Vec3 axis = cross(up, n);
    if (axis.sizeSquared() < 1e-12f) return yawQ;   // already vertical -- nothing to tilt

    const f32 c = dot(up, n);
    const f32 angle = std::acos(c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c));
    const Quat tilt = Quat::fromAxisAngle(axis, angle);   // fromAxisAngle normalises the axis itself
    return (tilt * yawQ).normalized();
}

} // namespace aver::editor
