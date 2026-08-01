// The globality check: one mirror, one beacon outside the voxel volume, two tracers.
#include "ReflTest.hpp"
#include "aver/core/Log.hpp"
#include "aver/runtime/Engine.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>

namespace aver::editor {

namespace {

// How far along the reflection vector the beacon sits. Well beyond the editor's default 1200 cm
// voxel volume, and the report states the margin rather than trusting this number.
constexpr f32 kBeaconDistance = 5200.0f;

// What counts as a changed pixel, matching the other render checks so they report in one unit.
constexpr f32 kMinDelta = 0.05f;

// Frames to settle. Toggling ray tracing rebuilds pipelines, and the renderer replays LAST frame's
// draw list into its shadow and voxelise passes, so neither change is on screen the frame it happens.
constexpr u32 kSettle = 10;

f32 worst3(const f32 a[4], const f32 b[4]) {
    f32 w = 0.0f;
    for (u32 i = 0; i < 3; ++i) w = std::fmax(w, std::fabs(a[i] - b[i]));
    return w;
}

} // namespace

Vec3 ReflTest::beaconPosition(const Vec3& camPos, const Vec3& mirrorPoint) {
    // The mirror is the floor plane, normal +Z. A ray leaving the camera, striking the mirror and
    // reflecting, goes back up and away -- so that is where something must be for the mirror to
    // show it, and putting the beacon anywhere else would test nothing.
    const Vec3 v = (camPos - mirrorPoint).getSafeNormal();
    const Vec3 i{-v.x, -v.y, -v.z};
    const Vec3 r{i.x, i.y, i.z - 2.0f * i.z};   // reflect about (0,0,1)
    return mirrorPoint + r.getSafeNormal() * kBeaconDistance;
}

void ReflTest::tick(Engine& e, f32 vpX, f32 vpY, f32 vpW, f32 vpH) {
    if (done_) return;

    // The probe grid is built on first use, once the viewport rect is known to be real.
    if (step_ == 0 && phase_ == kRayWith && probes_[0].u == 0.5f && probes_[1].u == 0.5f) {
        u32 p = 0;
        // AROUND THE VIEWPORT CENTRE, because that is where the world origin projects and the
        // beacon was placed on the reflection vector THROUGH the origin. The first version swept
        // 0.60-0.76 -- below centre, looking at mirror points nearer the camera whose reflection
        // vectors point somewhere else entirely -- and measured 0.02 where the answer was elsewhere.
        const f32 us[3] = {0.46f, 0.50f, 0.54f};
        const f32 vs[3] = {0.46f, 0.50f, 0.54f};
        for (f32 vv : vs) for (f32 uu : us) { probes_[p].u = uu; probes_[p].v = vv; ++p; }
    }

    if (step_ < kSettle) { ++step_; return; }

    const u32 t = step_ - kSettle;
    const u32 slot = t / 2;
    if (slot >= kProbes) {
        if (phase_ + 1 < kPhases) { ++phase_; step_ = 0; return; }
        report();
        done_ = true;
        return;
    }

    Probe& pr = probes_[slot];
    if ((t % 2) == 0) {
        e.device()->requestCapture(static_cast<u32>(vpX + vpW * pr.u), static_cast<u32>(vpY + vpH * pr.v));
    } else {
        f32 c[4];
        if (e.device()->getCapture(c)) {
            for (u32 i = 0; i < 4; ++i) pr.c[phase_][i] = c[i];
            pr.have[phase_] = true;
        }
    }
    ++step_;
}

void ReflTest::report() {
    const f32 dist = std::sqrt(beaconPos_.x * beaconPos_.x + beaconPos_.y * beaconPos_.y +
                               beaconPos_.z * beaconPos_.z);
    AVER_INFO("[Refl] beacon at ({:.0f},{:.0f},{:.0f}), {:.0f} cm from the origin against a voxel "
              "volume of half-extent {:.0f} cm -- {:.1f}x outside it",
              beaconPos_.x, beaconPos_.y, beaconPos_.z, dist, volumeExtent_,
              volumeExtent_ > 1.0f ? dist / volumeExtent_ : 0.0f);

    u32 complete = 0, rayMoved = 0, coneMoved = 0;
    f32 rayBest = 0.0f, coneWorst = 0.0f;
    for (u32 i = 0; i < kProbes; ++i) {
        const Probe& p = probes_[i];
        bool all = true;
        for (u32 ph = 0; ph < kPhases; ++ph) all = all && p.have[ph];
        if (!all) continue;
        ++complete;
        const f32 dRay  = worst3(p.c[kRayWith],  p.c[kRayWithout]);
        const f32 dCone = worst3(p.c[kConeWith], p.c[kConeWithout]);
        rayBest   = std::fmax(rayBest, dRay);
        coneWorst = std::fmax(coneWorst, dCone);
        if (dRay  > kMinDelta) ++rayMoved;
        if (dCone > kMinDelta) ++coneMoved;
    }

    AVER_INFO("[Refl] {} of {} probes read back in all four phases; ray changed on {} (largest "
              "{:.4f}), cone changed on {} (largest {:.4f})",
              complete, kProbes, rayMoved, rayBest, coneMoved, coneWorst);

    if (complete == 0) {
        AVER_ERROR("[Refl] INCONCLUSIVE: no probe read back in all four phases");
        return;
    }

    // ---- 1. the ray reached it ----
    if (rayMoved == 0) {
        AVER_ERROR("[Refl] FAIL (not global): hiding a beacon {:.0f} cm away -- {:.1f}x outside the "
                   "voxel volume -- changed no mirror pixel under RAY TRACING (largest {:.4f}). "
                   "A reflection that cannot see it is still bounded by something",
                   dist, volumeExtent_ > 1.0f ? dist / volumeExtent_ : 0.0f, rayBest);
        return;
    }

    // ---- 2. and the cone could not, which is what makes this a proof rather than a difference ----
    if (coneMoved > 0) {
        AVER_ERROR("[Refl] INCONCLUSIVE: the CONE tracer also saw the beacon change ({} probes, "
                   "largest {:.4f}). The beacon is meant to be unreachable by a cone, so either it "
                   "is inside the voxel volume after all or it is being seen some other way -- and "
                   "the ray result above then proves nothing about globality",
                   coneMoved, coneWorst);
        return;
    }

    AVER_INFO("[Refl] PASS (global): the same beacon appearing and disappearing {:.0f} cm away moved "
              "{} mirror pixels under ray tracing (largest {:.4f}) and NOT ONE under cone tracing "
              "(largest {:.4f}). The ray reaches geometry the cone cannot reach at all, which is "
              "exactly the bound that was there before",
              dist, rayMoved, rayBest, coneWorst);
}

} // namespace aver::editor
