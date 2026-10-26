// Evaluating one CCameraRig (docs/CAMERAS.md, "Evaluating one rig"): target and pivot by mode, look-ahead, position
// lag, rotation, the swept-sphere probe, the collision fade, FOV and the stateless editor preview.
#include "aver/camera/CameraRigSystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_queries_abi.h"

#include <cmath>

namespace aver::camera {
namespace {

constexpr f32 kMaxPitchRad = 89.0f * (kPi / 180.0f);
constexpr f32 kRecenterSpeed = 2.5f;   // 1/s, once the look input has been idle for recenterDelayS
constexpr f32 kKickSmoothSpeed = 6.0f;
constexpr f32 kSpeedSmoothSpeed = 10.0f;

// Under Reduce motion a lag speed below the floor is raised to it: stiff, not swaying.
f32 reduceMotionFloor(f32 speed, bool motionAllowed, f32 floorSpeed) {
    return speed > 0.0f && !motionAllowed && speed < floorSpeed ? floorSpeed : speed;
}

Vec3 positionOf(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// The entity's axes as a basis: row 0 forward (+X), row 1 right (+Y), row 2 up (+Z), each normalised.
Basis basisOfEntity(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Basis{Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.getSafeNormal(), Vec3{m.m[1][0], m.m[1][1], m.m[1][2]}.getSafeNormal(),
                 Vec3{m.m[2][0], m.m[2][1], m.m[2][2]}.getSafeNormal()};
}

bool isDescendant(scene::World& w, scene::Entity e, scene::Entity ancestor) {
    scene::Entity p = e;
    for (int hops = 0; hops < 16 && p != scene::kInvalidEntity; ++hops) {
        if (p == ancestor) return true;
        p = w.parent(p);
    }
    return false;
}

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

scene::Entity CameraRigSystem::sourceEntity(scene::World& w, const CCameraRig& r, scene::Entity rigEntity) {
    if (r.target != 0 && w.valid(r.target)) return r.target;
    if (r.target == 0) {
        const scene::Entity p = w.parent(rigEntity);
        if (p != scene::kInvalidEntity && w.valid(p)) return p;
    }
    return rigEntity;
}

bool CameraRigSystem::evaluate(scene::World& w, scene::Entity re, CCameraRig& rig, RigState& st, const RigFrame& fr,
                               const ModResult& mods, bool write, EvalOut& out) {
    const RigTuning t = resolveTuning(rig);
    const CameraMode mode = modeOf(rig);
    const f32 dt = fr.dt > 0.0f ? fr.dt : 0.0f;

    // Returns a held pose for this frame. The arm keeps its last length and the state bits are cleared.
    auto hold = [&](const CameraPov& pose) {
        out.pov = pose;
        out.held = true;
        out.arm = st.arm < 0.0f ? 0.0f : st.arm;
        out.state = 0;
        return true;
    };

    // 1. Target. A dead explicit target holds the last pose for a moment, then yields to the fallback.
    scene::Entity target = sourceEntity(w, rig, re);
    if (rig.target != 0 && !w.valid(rig.target)) {
        if (write) st.deadFor += dt;
        // rigUsable() retires the rig once deadFor passes holdDestroyedS; until then (and on that frame) the pose holds.
        if (st.havePov) return hold(st.lastPov);
        target = re;
    } else if (write) {
        st.deadFor = 0.0f;
    }

    const bool isPawnTarget = fr.possessed != scene::kInvalidEntity && target == fr.possessed;
    const bool viewNodeOk = fr.viewNode != scene::kInvalidEntity && w.valid(fr.viewNode);
    const bool useViewNode = viewNodeOk && isPawnTarget &&
                             (mode == CameraMode::FirstPerson || (mode == CameraMode::Chase && isDescendant(w, fr.viewNode, target)));

    const Vec3 pivotOff{rig.pivotX, rig.pivotY, rig.pivotZ};
    const f32 ownYaw0 = radians(rig.yawDeg), ownPitch0 = radians(rig.pitchDeg);
    const f32 pitchMin = radians(t.pitchMinDeg), pitchMax = radians(t.pitchMaxDeg);

    bool lagAllowed = true, hasArm = true, wantProbe = (rig.flags & kRigNoCollision) == 0, lookAheadAllowed = true;
    bool rotLagAllowed = false;
    Vec3 base;
    f32 srcYaw = 0, srcPitch = 0, srcRoll = 0;     // the rotation the mode starts from, before inherit flags and own angles
    f32 ownYaw = ownYaw0, ownPitch = ownPitch0;
    f32 yaw = 0, pitch = 0, roll = 0;

    // 2./5. Pivot and source rotation by mode.
    switch (mode) {
        case CameraMode::Chase: {
            Basis src;
            if (useViewNode) {
                base = positionOf(w, fr.viewNode) + pivotOff;
                src = basisOfEntity(w, fr.viewNode);
            } else {
                base = positionOf(w, target) + pivotOff;
                src = basisOfEntity(w, target);
            }
            yawPitchRollFromBasis(src, srcYaw, srcPitch, srcRoll);
            if (useViewNode) srcRoll = 0.0f;
            rotLagAllowed = true;
            break;
        }
        case CameraMode::Orbit: {
            base = positionOf(w, target) + pivotOff;
            const Basis src = basisOfEntity(w, target);
            yawPitchRollFromBasis(src, srcYaw, srcPitch, srcRoll);
            break;
        }
        case CameraMode::FirstPerson: {
            lagAllowed = false; hasArm = false; wantProbe = false; lookAheadAllowed = false;
            Basis src;
            if (useViewNode) {
                base = positionOf(w, fr.viewNode);
                src = basisOfEntity(w, fr.viewNode);
            } else {
                base = positionOf(w, target) + pivotOff;
                src = basisOfEntity(w, target);
            }
            yawPitchRollFromBasis(src, srcYaw, srcPitch, srcRoll);
            srcRoll = 0.0f;
            break;
        }
        case CameraMode::Seat: {
            hasArm = false; wantProbe = false; lookAheadAllowed = false;
            base = positionOf(w, re) + pivotOff;
            break;
        }
        case CameraMode::TopDown: {
            base = positionOf(w, target) + pivotOff;
            break;
        }
        case CameraMode::Fixed: {
            lagAllowed = false; hasArm = false; wantProbe = false; lookAheadAllowed = false;
            base = positionOf(w, re) + pivotOff;
            rotLagAllowed = (rig.flags & kRigLookAtTarget) != 0;
            break;
        }
    }

    // Orbit and an own-yaw Chase: idle look input eases the yaw back behind the target.
    const bool recenterMode = mode == CameraMode::Orbit || (mode == CameraMode::Chase && (rig.flags & kRigNoInheritYaw));
    if (recenterMode) {
        const bool changed = st.lookValid && (rig.yawDeg != st.lastYawDeg || rig.pitchDeg != st.lastPitchDeg);
        if (changed) st.sinceLook = 0.0f;
        else st.sinceLook += dt;
        if (rig.recenterDelayS > 0.0f && fr.motionAllowed && st.sinceLook > rig.recenterDelayS) {
            ownYaw = angleFollow(ownYaw, srcYaw, kRecenterSpeed, dt, 0.0f);
            if (write) rig.yawDeg = degrees(ownYaw);
        }
        if (write) { st.lastYawDeg = rig.yawDeg; st.lastPitchDeg = rig.pitchDeg; st.lookValid = true; }
    }

    switch (mode) {
        case CameraMode::Chase:
            yaw = (rig.flags & kRigNoInheritYaw) ? ownYaw : srcYaw;
            pitch = (rig.flags & kRigNoInheritPitch) ? ownPitch : srcPitch;
            roll = (rig.flags & kRigNoInheritRoll) ? 0.0f : srcRoll;
            pitch = clampf(pitch, pitchMin, pitchMax);
            break;
        case CameraMode::Orbit:
            yaw = ownYaw;
            pitch = clampf(ownPitch, pitchMin, pitchMax);
            if (write) rig.pitchDeg = degrees(pitch);
            break;
        case CameraMode::FirstPerson:
            yaw = srcYaw;
            pitch = clampf(srcPitch, pitchMin, pitchMax);
            roll = 0.0f;
            break;
        case CameraMode::Seat: {
            const f32 hy = clampf(rig.yawDeg, -t.seatYawDeg, t.seatYawDeg), hp = clampf(rig.pitchDeg, -t.seatPitchDeg, t.seatPitchDeg);
            if (write) { rig.yawDeg = hy; rig.pitchDeg = hp; }
            const Basis seat = basisOfEntity(w, re);
            if (hy == 0.0f && hp == 0.0f) {
                yawPitchRollFromBasis(seat, yaw, pitch, roll);
            } else {
                // Head look is a rotation in the seat's own frame: yaw about its up, pitch about its right (up is positive).
                const Quat head = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(hy)) * Quat::fromAxisAngle(Vec3{0, 1, 0}, -radians(hp));
                const Quat q = (quatFromBasis(seat) * head).normalized();
                yawPitchRollFromBasis(basisFromQuat(q), yaw, pitch, roll);
            }
            break;
        }
        case CameraMode::TopDown:
            yaw = ownYaw;
            pitch = ownPitch;
            roll = 0.0f;
            break;
        case CameraMode::Fixed: {
            if ((rig.flags & kRigLookAtTarget) && target != re) {
                const Vec3 d = positionOf(w, target) - base;
                const f32 len = d.size();
                if (len > 1e-3f) {
                    yaw = std::atan2(d.y, d.x);
                    pitch = std::asin(clampf(d.z / len, -1.0f, 1.0f));
                    roll = 0.0f;
                } else {
                    yawPitchRollFromBasis(basisOfEntity(w, re), yaw, pitch, roll);
                }
            } else {
                yawPitchRollFromBasis(basisOfEntity(w, re), yaw, pitch, roll);
            }
            break;
        }
    }
    pitch = clampf(pitch, -kMaxPitchRad, kMaxPitchRad);

    // A non-finite transform or rotation must not reach the follow state: hold the last pose and say so once.
    if (!isFinite(base) || !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) {
        if (write && !st.warnedNonFinite) {
            st.warnedNonFinite = true;
            AVER_WARN("[Camera] rig {} sees a non-finite transform; holding its last pose", re);
        }
        return hold(st.havePov ? st.lastPov : fr.fallback);
    }

    // 3. Look-ahead (planar, off under Reduce motion).
    Vec3 lead;
    bool teleported = false;
    if (rig.lookAheadS > 0.0f && fr.motionAllowed && lookAheadAllowed)
        lead = lookAhead(st.lead, base, rig.lookAheadS, t.lookAheadMaxCm, dt, options_.teleportCm, teleported);
    else if (st.lead.valid)
        st.lead = LeadState{};

    // 4. Position lag.
    const Vec3 pivotTarget = base + lead;
    const bool farJump = st.lagValid && dist(pivotTarget, st.prevTarget) > options_.teleportCm;
    const bool snap = !st.lagValid || st.snapNext || teleported || farJump;
    const bool explicitSnap = st.lagValid && (st.snapNext || teleported || farJump);
    const f32 speed = reduceMotionFloor(lagAllowed ? rig.posLagSpeed : 0.0f, fr.motionAllowed, options_.reduceMotionLagFloor);
    Vec3 pivot;
    if (snap || speed <= 0.0f) pivot = pivotTarget;
    else pivot = lagFollow(st.lagPivot, st.prevTarget, pivotTarget, speed, t.lagStepS, rig.lagMaxDistCm, dt);
    const bool lagging = !snap && speed > 0.0f && dist(pivot, pivotTarget) > 1.0f;
    out.cut = explicitSnap;
    if (write) {
        st.lagPivot = pivot;
        st.prevTarget = pivotTarget;
        st.lagValid = true;
        st.snapNext = false;
    }

    // Rotation lag (and the first evaluation / a snap take the rotation as is).
    const f32 rs = reduceMotionFloor(rotLagAllowed ? rig.rotLagSpeed : 0.0f, fr.motionAllowed, options_.reduceMotionLagFloor);
    if (!st.rotValid || snap || rs <= 0.0f) {
        if (write) { st.yaw = yaw; st.pitch = pitch; st.roll = roll; st.rotValid = true; }
    } else {
        const f32 maxD = radians(rig.rotLagMaxDeg);
        const f32 ny = angleFollow(st.yaw, yaw, rs, dt, maxD);
        const f32 np = angleFollow(st.pitch, pitch, rs, dt, maxD);
        const f32 nr = angleFollow(st.roll, roll, rs, dt, maxD);
        if (write) { st.yaw = ny; st.pitch = np; st.roll = nr; }
        yaw = ny; pitch = np; roll = nr;
    }

    // 6./7. The arm and the socket in the rotated basis; arm scale and socket offset from the modifier stack.
    const Basis b = basisFromYawPitchRoll(yaw, pitch, roll);
    const f32 arm = hasArm ? t.armCm * mods.armScale : 0.0f;
    const Vec3 sock = b.fwd * (rig.socketX + mods.socketAdd.x) + b.right * (rig.socketY + mods.socketAdd.y) +
                      b.up * (rig.socketZ + mods.socketAdd.z);
    const Vec3 end = pivot - b.fwd * arm + sock;

    // 8. Probe the segment with a swept sphere.
    Vec3 camPos = end;
    const Vec3 disp = end - pivot;
    const f32 len = disp.size();
    bool colliding = false, didHit = false;
    Vec3 hitPoint;
    f32 used = len;
    if (wantProbe && len > 1e-3f) {
        const u32 mask = rig.collisionMask != 0 ? rig.collisionMask : options_.defaultProbeMask;
        const i32 character = aver_phys_character_of_entity(static_cast<i32>(target));
        float d = 0.0f, pt[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
        const i32 body = aver_phys_sphere_cast_q(pivot.x, pivot.y, pivot.z, disp.x, disp.y, disp.z, t.probeCm, mask,
                                                 character, rig.ignoreBody, 0, &d, pt, nrm);
        const f32 hitDist = body != 0 ? d : -1.0f;
        used = resolveArm(len, hitDist, st.arm, t.minArmCm, t.recoverS, dt);
        camPos = pivot + disp * (used / len);
        colliding = used < len - 0.01f;
        didHit = body != 0;
        if (didHit) hitPoint = Vec3{pt[0], pt[1], pt[2]};
        if (write) st.arm = used;
    } else if (write) {
        st.arm = len;
    }

    // 9. Collision fade: a camera squashed against the pivot hides the target's meshes (shadows and GI stay).
    bool faded = false;
    if (hasArm && !(rig.flags & kRigNoFade)) {
        faded = fadeHide(st.faded, dist(camPos, pivot), t.fadeStartCm, t.fadeEndCm);
        if (write) st.faded = faded;
        if (faded) { out.hideRoot = target; out.hideAll = true; }
    } else if (write) {
        st.faded = false;
    }
    if (mode == CameraMode::FirstPerson) { out.hideRoot = target; out.hideAll = false; }

    // 10. FOV and clip from CCamera, plus the smoothed speed kick.
    const auto* cam = w.component<scene::CCamera>(re, scene::kComponentCamera);
    const f32 baseFov = cam ? cam->fovYRad : radians(60.0f);
    const Vec3 tp = positionOf(w, target);
    f32 planarSpeed = st.speed;
    if (st.targetPosValid && dt > 0.0f && !snap) {
        const f32 inst = std::sqrt((tp.x - st.lastTargetPos.x) * (tp.x - st.lastTargetPos.x) +
                                   (tp.y - st.lastTargetPos.y) * (tp.y - st.lastTargetPos.y)) / dt;
        planarSpeed = expFollow(st.speed, inst, kSpeedSmoothSpeed, dt);
    } else if (snap) {
        planarSpeed = 0.0f;
    }
    f32 kick = 0.0f;
    if (fr.motionAllowed && rig.fovKickDeg != 0.0f) {
        const f32 want = radians(rig.fovKickDeg) * clamp01(planarSpeed / t.fovKickSpeed);
        kick = expFollow(st.kick, want, kKickSmoothSpeed, dt);
    }
    if (write) { st.kick = kick; st.speed = planarSpeed; st.lastTargetPos = tp; st.targetPosValid = true; }

    out.pov.pos = camPos;
    out.pov.yaw = yaw;
    out.pov.pitch = pitch;
    out.pov.roll = roll;
    out.pov.fovYRad = composeFov(baseFov, kick, 0.0f, 1.0f);
    out.pov.nearCm = cam ? cam->nearCm : 2.0f;
    out.pov.farCm = cam ? cam->farCm : 200000.0f;
    out.arm = used;
    out.state = (colliding ? kRigStateColliding : 0u) | (faded ? kRigStateFaded : 0u) | (lagging ? kRigStateLagging : 0u);
    if (write) { st.lastPov = out.pov; st.havePov = true; }

    out.dbg.pivot = base;
    out.dbg.laggedPivot = pivot;
    out.dbg.desiredEnd = end;
    out.dbg.camera = camPos;
    out.dbg.hit = hitPoint;
    out.dbg.lead = lead;
    out.dbg.probeRadius = t.probeCm;
    out.dbg.fadeEndCm = t.fadeEndCm;
    out.dbg.armWanted = len;
    out.dbg.armResolved = used;
    out.dbg.hasProbe = wantProbe;
    out.dbg.didHit = didHit;
    out.dbg.mode = static_cast<u32>(mode);
    out.dbg.fwd = b.fwd; out.dbg.right = b.right; out.dbg.up = b.up;
    out.dbg.pitchMinRad = pitchMin;
    out.dbg.pitchMaxRad = pitchMax;
    return true;
}

RigResult CameraRigSystem::preview(scene::World& world, scene::Entity rig, const RigFrame& fr) {
    RigResult r;
    if (CameraRigSystem::typeId() == 0 || !world.valid(rig)) return r;
    auto* c = world.component<CCameraRig>(rig, CameraRigSystem::typeId());
    if (!c) return r;
    RigState st;
    ModResult mods;
    RigFrame f = fr;
    f.dt = 0.0f;
    EvalOut ev;
    if (!evaluate(world, rig, *c, st, f, mods, false, ev)) return r;
    r.active = true;
    r.pov = ev.pov;
    r.ownerHideRoot = ev.hideRoot;
    r.hideAll = ev.hideAll;
    return r;
}

}  // namespace aver::camera
