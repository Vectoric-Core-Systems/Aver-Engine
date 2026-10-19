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

#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_access_abi.h"
#endif

// Gameplay cameras (docs/CAMERAS.md): the rig system that may replace the boom below.
#if AVER_MODULE_CAMERA && AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
#  include "aver/camera/CameraRigSystem.hpp"
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

// What travels with camPos_/yaw_/pitch_ besides the pose: the projection and the owner-hide mode. The defaults are the
// constants pushCamera used before FOV was an input, built from the SAME expressions, so a default PlayView is today's
// camera bit for bit. ONE member per host; every site that hard-coded 60 degrees reads fovYRad.
struct PlayView {
    f32 fovYRad = radians(60.0f);
    f32 rollRad = 0.0f;
    f32 nearCm = kCameraNearCm;
    f32 farCm = kCameraFarCm;
    bool ownerHideAll = false;   // hide every mesh under ownerHideRoot, not only the flagged ones (the collision fade)
    bool cut = false;            // this frame teleported the camera (the host resets streaming's velocity tracking)
};

// World up turned about `forward` by roll (positive tips the camera's top toward its right). Exactly (0,0,1) at roll 0.
inline Vec3 rolledUp(const Vec3& forward, f32 rollRad) {
    const Vec3 world{0, 0, 1};
    if (rollRad == 0.0f) return world;
    const Vec3 r0 = cross(world, forward).getSafeNormal();
    const Vec3 u0 = cross(forward, r0);
    return u0 * std::cos(rollRad) + r0 * std::sin(rollRad);
}

// Builds the view/projection looking from `pos` along `forward`, pushes it to the device, and
// returns both matrices for the caller's own members. FOV, roll and clip planes from `pv`
// (default 60 degrees, near/far 2/200000 centimetres).
inline CameraMatrices pushCamera(rhi::IDevice& device, const Vec3& pos, const Vec3& forward, f32 aspect, const PlayView& pv) {
    // lookAtDirLH, not lookAtLH(pos, pos + forward, ...): `forward` is already the direction, and
    // composing a target only for lookAtLH to subtract `pos` back off loses it to f32 absorption
    // when the camera is far from the origin and looking near-vertically -- far enough out, both
    // horizontal components round away and the view matrix collapses to no basis at all. See
    // Mat4::lookAtDirLH's own comment for the measurement.
    const Mat4 view = Mat4::lookAtDirLH(pos, forward, rolledUp(forward, pv.rollRad));
    const Mat4 proj     = Mat4::perspectiveLH(pv.fovYRad, aspect, pv.nearCm, pv.farCm);
    const Mat4 viewProj = view * proj;           // row-vector: v * M, so view then proj

    // THE DEVICE GETS THE CAMERA-RELATIVE INVERSE: the same view with its translation row zeroed
    // (the rotation rows never depended on `pos`), composed with `proj` and inverted, so every
    // entry is O(1) wherever the camera is. The absolute inverse carries about |pos|/near (1e5 at
    // 2 km) in two rows, which every ray reconstruction then had to cancel back down to O(1) in
    // float32 -- see PerFrameCB::invViewProjRel.
    Mat4 viewRot = view;
    viewRot.m[3][0] = viewRot.m[3][1] = viewRot.m[3][2] = 0.0f;
    const Mat4 invViewProjRel = (viewRot * proj).inverse();
    device.setCamera(&viewProj.m[0][0], &invViewProjRel.m[0][0], &pos.x);

    // The absolute inverse the hosts keep for mouse picking, COMPOSED rather than inverted: offset
    // from the eye, then translated by it, so the eye's magnitude enters only as an addition and
    // never through Mat4::inverse()'s cofactor products.
    const Mat4 invVP = invViewProjRel * Mat4::translation(pos);
    return { viewProj, invVP };
}

// The camera every host used before PlayView: FOV 60 degrees, near/far 2/200000 (centimetres).
inline CameraMatrices pushCamera(rhi::IDevice& device, const Vec3& pos, const Vec3& forward, f32 aspect) {
    return pushCamera(device, pos, forward, aspect, PlayView{});
}

#if AVER_MODULE_FRAMEWORK
// Camera shake and head bob (framework CameraFx) over the FINAL camera, with the player's effective scales; call right
// before pushCamera, after the pawn and sequencer cameras. Leaves pos/yaw/pitch alone when nothing is active, so a game
// that never calls AddShake/SetBob is bit-identical. The editor's free-fly camera does not use it.
inline void applyCameraFx(Vec3& pos, f32& yaw, f32& pitch, f32 dt) {
    float p[3] = {pos.x, pos.y, pos.z};
    float yp[2] = {yaw, pitch};
    if (!aver_fw_camera_fx_apply(dt, p, yp)) return;
    pos = Vec3{p[0], p[1], p[2]};
    yaw = yp[0];
    pitch = yp[1];
}
#endif

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
namespace detail {
// What the legacy boom found: whether it placed the camera, the pawn to hide (first person only), and the entities a rig
// needs (the pawn and the published view node).
struct LegacyCamera {
    bool placed = false;
    scene::Entity hide = scene::kInvalidEntity;
    scene::Entity pawn = scene::kInvalidEntity;
    scene::Entity viewNode = scene::kInvalidEntity;
};

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
inline LegacyCamera legacyPlayCamera(Vec3& pos, f32& yaw, f32& pitch, scene::Entity standInPawn) {
    scene::Entity e = scene::kInvalidEntity;
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (pawn == 0) return {};
        e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
    } else if (standInPawn != scene::kInvalidEntity) {
        // THE STAND-IN IS THE PAWN WHEN THERE IS NO SESSION, so the camera follows it like one.
        e = standInPawn;
    } else {
        return {};
    }

    scene::World& w = scene::World::instance();
    if (!w.valid(e)) return {};

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
    LegacyCamera out;
    out.placed = true;
    out.hide = firstPersonPawn;
    out.pawn = e;
    out.viewNode = haveView ? ve : scene::kInvalidEntity;
    return out;
}
} // namespace detail

// The boom alone, as before cameras were rigs: the editor's and the game's drive both start from it.
inline scene::Entity drivePlayCamera(Vec3& pos, f32& yaw, f32& pitch, scene::Entity standInPawn = scene::kInvalidEntity) {
    return detail::legacyPlayCamera(pos, yaw, pitch, standInPawn).hide;
}

// The pose the sequencer's camera holds this frame, when it does; the rig blends back from it when it lets go.
struct ExternalPose {
    bool active = false;
    Vec3 pos;
    f32 yaw = 0, pitch = 0;
};

// player controller 0's Reduce motion option (true without the framework's access store).
inline bool cameraMotionAllowed() {
    return aver_fw_access_get("access.motion.reduce") < 0.5f;
}

// The play camera with gameplay cameras: the legacy boom pose is computed first and handed to the rig system as the
// fallback; when no rig, blend or modifier is live the result is that boom pose untouched (and `view` stays default),
// so a project with no rig renders exactly as before. Otherwise pos/yaw/pitch/view carry the rig's pose, and the return
// value is the entity to hide from its own camera: the pawn in first person (flagged meshes), the target with
// view.ownerHideAll while the collision fade is on.
inline scene::Entity drivePlayCamera(Vec3& pos, f32& yaw, f32& pitch, PlayView& view, f32 dt,
                                     scene::Entity standInPawn = scene::kInvalidEntity, const ExternalPose* external = nullptr) {
    view = PlayView{};
    const detail::LegacyCamera legacy = detail::legacyPlayCamera(pos, yaw, pitch, standInPawn);
    if (!legacy.placed) return legacy.hide;
#if AVER_MODULE_CAMERA
    camera::RigFrame fr;
    fr.dt = dt;
    fr.possessed = legacy.pawn;
    fr.viewNode = legacy.viewNode;
    fr.motionAllowed = cameraMotionAllowed();
    fr.fallback.pos = pos;
    fr.fallback.yaw = yaw;
    fr.fallback.pitch = pitch;
    fr.fallback.roll = 0.0f;
    fr.fallback.fovYRad = view.fovYRad;
    fr.fallback.nearCm = view.nearCm;
    fr.fallback.farCm = view.farCm;
    if (external && external->active) {
        fr.externalActive = true;
        fr.external = fr.fallback;
        fr.external.pos = external->pos;
        fr.external.yaw = external->yaw;
        fr.external.pitch = external->pitch;
    }
    camera::CameraRigSystem& rigs = camera::cameraRigSystem();
    const camera::RigResult rr = rigs.drive(scene::World::instance(), fr);
    if (rr.active) {
        pos = rr.pov.pos;
        yaw = rr.pov.yaw;
        pitch = rr.pov.pitch;
        view.fovYRad = rr.pov.fovYRad;
        view.rollRad = rr.pov.roll;
        view.nearCm = rr.pov.nearCm;
        view.farCm = rr.pov.farCm;
        view.ownerHideAll = rr.hideAll;
        view.cut = rr.cut;
        if (rr.ownerHideRoot != scene::kInvalidEntity) return rr.ownerHideRoot;
        // No live rig (a blend back to the boom, or modifiers on it): the boom's own first-person hide stands.
        return rigs.activeRig() == scene::kInvalidEntity ? legacy.hide : scene::kInvalidEntity;
    }
#else
    (void)dt; (void)external;
#endif
    return legacy.hide;
}
#endif

} // namespace aver::game
