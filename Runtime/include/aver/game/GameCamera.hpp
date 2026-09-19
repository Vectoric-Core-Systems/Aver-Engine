// GameCamera: the ONE copy of the camera forward axis from yaw/pitch, the 60-degree
// 2cm-200000cm perspective view/projection build + IDevice::setCamera push, and the play camera
// that follows player controller 0's possessed pawn (first/third person via aver_fw_view, the view
// node, owner-hide pawn) -- Sandbox.exe (the editor) and AverEngineRuntime.exe (the shipped game)
// each carried an identical copy of all three before this file existed (docs/RUNTIME-DEDUP.md,
// Phase C).
//
// HEADER-ONLY ON PURPOSE, like GameTick.hpp. EACH HOST KEEPS ITS OWN CAMERA STATE AND CALL
// POSITIONS -- camPos_/yaw_/pitch_/invVP_/viewProj_/eye_/firstPersonPawn_ stay members of
// SandboxApp/GameApp (the editor's free-fly camera writes the same members, so they cannot move
// here) and are passed by reference into these functions. Each host still supplies its own aspect
// (dockspace vs window), calls from its own positions, and GameApp's --cam-wobble temporary yaw
// offset stays applied around its own cameraForward() call, exactly as before this file existed.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/framework/framework_abi.h"
#endif

#include <cmath>

namespace aver::game {

// The camera's forward axis, built from yaw and pitch.
inline Vec3 cameraForward(f32 yaw, f32 pitch) {
    return Vec3{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) };
}

// The clip planes pushCamera uses, in centimetres -- named so a log line can report them without
// retyping the numbers.
inline constexpr f32 kCameraNearCm = 2.0f;
inline constexpr f32 kCameraFarCm  = 200000.0f;

struct CameraMatrices {
    Mat4 viewProj;
    Mat4 invVP;
};

// Builds the view/projection looking from `pos` along `forward`, pushes it to the device, and
// returns both matrices for the caller's own members. FOV 60 degrees, near/far 2/200000
// (centimetres).
inline CameraMatrices pushCamera(rhi::IDevice& device, const Vec3& pos, const Vec3& forward, f32 aspect) {
    // lookAtDirLH, not lookAtLH(pos, pos + forward, ...): `forward` is already the direction, and
    // composing a target only for lookAtLH to subtract `pos` back off loses it to f32 absorption
    // when the camera is far from the origin and looking near-vertically -- far enough out, both
    // horizontal components round away and the view matrix collapses to no basis at all. See
    // Mat4::lookAtDirLH's own comment for the measurement.
    const Mat4 view = Mat4::lookAtDirLH(pos, forward, Vec3{0, 0, 1});
    const Mat4 proj     = Mat4::perspectiveLH(radians(60.0f), aspect, kCameraNearCm, kCameraFarCm);
    const Mat4 viewProj = view * proj;           // row-vector: v * M, so view then proj
    const Mat4 invVP    = viewProj.inverse();
    device.setCamera(&viewProj.m[0][0], &invVP.m[0][0], &pos.x);
    return { viewProj, invVP };
}

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
// Drives the play camera from player controller 0's possessed pawn, writing `pos`/`yaw`/`pitch` in
// place -- from the pawn's published view node, falling back to the pawn's own matrix (row 3 the
// position, row 0 the forward/+X axis). Returns the pawn to hide from its own camera (valid only in
// first person, for DrawWorldOptions::ownerHideRoot), kInvalidEntity otherwise -- including every
// early return, so `pos`/`yaw`/`pitch` are left untouched exactly when this returns invalid without
// having reached the placement below.
//
// `standInPawn`: a pawn to follow when no session is playing (the editor passes its drone). Ignored
// while a session IS playing; with none given (the default) and no session playing, this is a
// no-op.
inline scene::Entity drivePlayCamera(Vec3& pos, f32& yaw, f32& pitch, scene::Entity standInPawn = scene::kInvalidEntity) {
    scene::Entity e = scene::kInvalidEntity;
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (pawn == 0) return scene::kInvalidEntity;
        e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
    } else if (standInPawn != scene::kInvalidEntity) {
        // THE STAND-IN IS THE PAWN WHEN THERE IS NO SESSION, so the camera follows it like one.
        e = standInPawn;
    } else {
        return scene::kInvalidEntity;
    }

    scene::World& w = scene::World::instance();
    if (!w.valid(e)) return scene::kInvalidEntity;

    int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f, boom = 450.0f;
    aver_fw_view(&mode, &eye, &boom);

    // ONLY FIRST PERSON HIDES ANYTHING. Set from `e` (the pawn itself), not the view node below --
    // a body mesh's renderer hangs off the pawn's own entity or an ancestor chain that ends there,
    // never off the camera transform, so owner-hide has to compare against the same entity the
    // hierarchy roots at.
    const scene::Entity firstPersonPawn = (mode == AVER_FW_VIEW_FIRST_PERSON) ? e : scene::kInvalidEntity;

    // Prefer the view node; fall back to the pawn if it has not published one.
    const int32_t viewId = aver_fw_view_entity();
    const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(viewId));
    const bool haveView = viewId != 0 && w.valid(ve);
    const Mat4& vm = haveView ? w.worldMatrix(ve) : w.worldMatrix(e);
    const Mat4& pm = w.worldMatrix(e);

    const Vec3 headPos{vm.m[3][0], vm.m[3][1], vm.m[3][2]};
    const Vec3 headFwd = Vec3{vm.m[0][0], vm.m[0][1], vm.m[0][2]}.getSafeNormal();
    const Vec3 pawnPos{pm.m[3][0], pm.m[3][1], pm.m[3][2]};
    const Vec3 pawnFwd = Vec3{pm.m[0][0], pm.m[0][1], pm.m[0][2]}.getSafeNormal();
    const Vec3 up{0, 0, 1};

    Vec3 look;
    if (mode == AVER_FW_VIEW_FIRST_PERSON) {
        pos  = haveView ? headPos : pawnPos + up * eye;
        look = headFwd;
    } else {
        const Vec3 pivot  = haveView ? headPos : pawnPos + up * eye;
        const Vec3 armDir = haveView ? headFwd : pawnFwd;
        pos  = pivot - armDir * boom;
        // armDir IS the look direction. `(pivot - pos).getSafeNormal()` stood here instead, which is
        // the eye+dir round trip pushCamera refuses (its own comment at :47-51) run backwards, and
        // it costs the same: pivot - pos is armDir * boom in exact arithmetic, and armDir is already
        // unit (getSafeNormal at :104 and :106), so the subtraction could never hand back anything
        // but armDir -- it only spent precision doing it. Far from the origin the boom offset falls
        // inside pivot's own f32 ulps and what comes back is a direction nobody asked for;
        // Mat4::lookAtDirLH's comment carries the measurement.
        //
        // AT boom == 0 IT DID NOT RETURN A DIRECTION AT ALL: pivot - pos is exactly zero, so
        // getSafeNormal answered {0,0,0} and the decode below turned that into atan2(0,0) = 0 and
        // asin(0) = 0 -- the camera silently snapping to level and facing +X. Nothing clamps the
        // boom on the way in (aver_fw_set_view stores it verbatim, and C# Character.BoomLength is a
        // plain settable float), so a game asking for a zero-length boom now simply looks where the
        // pawn looks from the pivot itself.
        look = armDir;
    }
    // cameraForward() composes {cosP cosY, cosP sinY, sinP}; invert the look direction to yaw/pitch.
    yaw   = std::atan2(look.y, look.x);
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
    return firstPersonPawn;
}
#endif

} // namespace aver::game
