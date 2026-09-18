// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"
#include "aver/voxi/Scalability.hpp"
#include "aver/formats/OcProject.hpp"

// THE ONE PLACE A .ocproject's RENDER.* KEYS BECOME A voxi::Settings, SHARED BY BOTH HOSTS. Before
// this header existed, the editor's applyProjectVoxiSettings (sandbox/src/SandboxApp.cpp) and the
// game's applyProjectRenderSettings (Runtime/src/GameApp.cpp) each carried their own
// copy of the same tier/knob apply logic, and the game's copy was the one that had drifted: it never
// seeded the volume before init (N2), never derived an absent knob back to its tier on reopen (N6),
// and applied everything in ONE setSettings call instead of two (so a manifest naming both a tier and
// a knob at that tier's OWN rung raced the derivation and could lose the knob to a coincidence -- R2's
// own regression test is built on exactly that race). This header is now the only place either fact is
// encoded; SandboxApp.cpp's applyProjectVoxiSettings becomes a thin two-phase caller of it (Lane 4),
// the same as GameApp's two call sites below.
//
// PURE AND HEADER-ONLY, RenderSettingsResolver.hpp's exact reason: applyManifestTiers and
// applyManifestKnobs take and mutate a Settings& directly and touch nothing else -- no
// voxi::Renderer::get(), no AVER_WARN, no device. That is what makes the two-phase apply, the N6 knob
// reset and the capture rule each a headless unit test (tests/formats/src/ProjectRenderApplyTest.cpp)
// rather than something only exercisable through a live singleton and a real project on disk.
namespace aver::voxi {

// ---- APPLY: manifest -> Settings ------------------------------------------------------------------

// Applies the manifest's four TIER keys (GI, RT, PT, LayeredBsdf) to `s`, and nothing else. Split out
// from applyManifestKnobs for the same reason SandboxApp.cpp's own two-call apply is split: a knob
// asked for at the value it already holds is indistinguishable, to setSettings' own change-gated
// derivation, from a knob never asked for at all -- so the tiers must already be COMMITTED (see
// applyManifestTwoPhase below) before any knob is read against them, or a manifest naming both a tier
// and a same-rung knob races the derivation and can lose the knob (this is exactly what R2's
// regression test in ProjectRenderApplyTest.cpp exists to pin down).
//
// Logic copied from SandboxApp.cpp's applyProjectVoxiSettings, first setSettings call: same four
// fields, same "-1 means unstated" sentinel, same order.
inline void applyManifestTiers(const fmt::ProjectDesc& project, Settings& s) {
    if (project.giQuality   >= 0) s.globalIllumination = static_cast<Quality>(project.giQuality);
    if (project.rayTracing  >= 0) s.rayTracing         = static_cast<Quality>(project.rayTracing);
    if (project.pathTracing >= 0) s.pathTracing        = static_cast<Quality>(project.pathTracing);
    if (project.layeredBsdf >= 0) s.layeredBsdf        = static_cast<Quality>(project.layeredBsdf);
}

// Applies every OTHER manifest key that lands on a Settings field -- everything
// SandboxApp.cpp's applyProjectVoxiSettings applies in its SECOND setSettings call, same order, same
// sentinels, with ONE deliberate change (N6):
//
// THE N6 FIX. Ten of these knobs are TIER-DERIVED: Renderer::setSettings only recomputes one of them
// from its tier when the tier CHANGES in that same call (Voxi.cpp's own derivation block) -- so a
// manifest that states a tier but says nothing about one of its derived knobs used to just inherit
// whatever was ALREADY LIVE, tier change or not. Project A pins GICONES 7 at GI Epic; project B states
// GI Epic too and never mentions GICONES; opening B right after A used to run Epic's cones at 7, not
// the ladder's 13, because the tier never "changed" out from under B's own apply. Here, the ABSENCE of
// one of these ten keys does not mean "leave it" -- it means "follow the tier `s` was just committed
// to", which is what running this function AFTER applyManifestTiers has already been committed through
// setSettings (see applyManifestTwoPhase) is what actually gives it: `s.globalIllumination`,
// `s.rayTracing` and `s.pathTracing` here are the DEVICE-CLAMPED, POST-DERIVATION tiers, not the raw
// manifest ask. The ten: voxelResolution, giCones, giUpdateInterval, giRestirVisibility (GI);
// rtShadowRays, rtPixelsPerRayTile, rtShadowDenoise, rtRenderMode, refractionMode (RT); ptBounces (PT).
//
// SANITY THIS FIX COSTS NOTHING TODAY: Settings{}'s own defaults already equal ladder(Medium) per
// group (QualityLadder.hpp's static_asserts enforce this), and a tier CHANGE already re-derives these
// same values through setSettings regardless of this function -- so a fresh launch, or any project that
// actually changes a tier, sees byte-identical behaviour. This only changes the case that used to leak:
// tier UNCHANGED, knob UNSTATED.
//
// giSkyOcclusionRays/giSkyOcclusionTile have NO manifest key at all (OcProject.hpp never grew one for
// either) -- they are ALWAYS set to the RT tier's ladder value here, every apply, manifest or not.
//
// Every startup knob CLI flag (SandboxApp.cpp's own take() block) still outranks whatever this writes,
// because that block runs AFTER this one and only takes a flag whose value actually differs from what
// setSettings already holds -- this function changes what "already holds" means for an unstated
// derived knob, it does not touch flag precedence at all.
inline void applyManifestKnobs(const fmt::ProjectDesc& project, Settings& s) {
    if (project.voxelResolution > 0) s.voxelResolution = static_cast<u32>(project.voxelResolution);
    else                              s.voxelResolution = ladder::voxelResolution(s.globalIllumination);

    if (project.giIntensity   >= 0.0f) s.giIntensity   = project.giIntensity;
    if (project.giMaxDistance >= 0.0f) s.giMaxDistance = project.giMaxDistance;

    if (project.rtShadowRays >= 0) s.rtShadowRays = static_cast<u32>(project.rtShadowRays);
    else                            s.rtShadowRays = ladder::rtShadowRays(s.rayTracing);

    if (project.rtPixelsPerRayTile >= 0) s.rtPixelsPerRayTile = static_cast<u32>(project.rtPixelsPerRayTile);
    else                                  s.rtPixelsPerRayTile = ladder::rtPixelsPerRayTile(s.rayTracing);

    if (project.rtShadowDenoise >= 0) s.rtShadowDenoise = static_cast<u32>(project.rtShadowDenoise);
    else                               s.rtShadowDenoise = ladder::rtShadowDenoise(s.rayTracing);

    if (project.rtRenderMode >= 0) s.rtRenderMode = static_cast<u32>(project.rtRenderMode);
    else                             s.rtRenderMode = ladder::rtRenderMode(s.rayTracing);

    if (project.ptBounces >= 0) s.ptBounces = static_cast<u32>(project.ptBounces);
    else                          s.ptBounces = ladder::ptBounces(s.pathTracing);

    if (project.giCones >= 0) s.giCones = static_cast<u32>(project.giCones);
    else                        s.giCones = ladder::giCones(s.globalIllumination);

    if (project.giMode   >= 0) s.giMode   = static_cast<u32>(project.giMode);
    if (project.denoiser  >= 0) s.denoiser = project.denoiser != 0;

    // THE TENTH TIER-DERIVED KNOB (N6): absent RENDER.RESTIRVISIBILITY follows the GI tier that was
    // just committed above, exactly like voxelResolution/giCones/giUpdateInterval do -- not whatever
    // giRestirVisibility happened to already be live at.
    if (project.restirVisibility >= 0) s.giRestirVisibility = static_cast<u32>(project.restirVisibility);
    else                                 s.giRestirVisibility = ladder::giRestirVisibility(s.globalIllumination);

    if (project.refractionMode >= 0) s.refractionMode = static_cast<u32>(project.refractionMode);
    else                               s.refractionMode = ladder::refraction(s.rayTracing);

    if (project.refractionStrength >= 0.0f) s.refractionStrength = project.refractionStrength;
    if (project.refractionEdgeFade >= 0.0f) s.refractionEdgeFade = project.refractionEdgeFade;

    // THE THREE KEYS THAT ARE NOT TIER-DERIVED but still ride this same second call in
    // SandboxApp.cpp's own applyProjectVoxiSettings (msaa/meshShaders/giUpdateInterval) -- kept here
    // rather than split into applyManifestTiers for the same reason that file states: this is the call
    // that carries every knob a manifest can state, and separating them by derivation would be a
    // distinction only that file's own history explains. giUpdateInterval IS tier-derived, unlike its
    // two neighbours here, and gets the same N6 else-branch as the ten above.
    if (project.msaa        >  0) s.msaa        = static_cast<Msaa>(project.msaa);
    if (project.meshShaders >= 0) s.meshShaders = project.meshShaders != 0;

    if (project.giUpdateInterval >= 0) s.giUpdateInterval = static_cast<u32>(project.giUpdateInterval);
    else                                 s.giUpdateInterval = ladder::giUpdateInterval(s.globalIllumination);

    // NO MANIFEST KEY EXISTS FOR EITHER OF THESE -- unconditional, every apply.
    s.giSkyOcclusionRays = ladder::giSkyOcclusionRays(s.rayTracing);
    s.giSkyOcclusionTile = ladder::giSkyOcclusionTile(s.rayTracing);
}

// THE TWO-PHASE APPLY, shared by both hosts (R2). `s` is the Settings a caller is building up --
// seeded from whatever is already live before this call -- and `commit` is invoked with NO ARGUMENTS
// after each phase; it is expected to push `s` through Renderer::setSettings and read the
// (device-clamped, tier-derived) result back into the SAME `s` the caller passed in, by reference --
// e.g. `voxi::Settings x = vx.settings(); applyManifestTwoPhase(project, x, [&](){ vx.setSettings(x);
// x = vx.settings(); });`. That is the shape both call sites below actually need: `s` and the value the
// lambda reads/writes are one and the same object, so applyManifestTiers' write is visible to `commit`,
// and `commit`'s read-back is visible to applyManifestKnobs.
//
// STRICT ORDER, NEVER MERGED (R2): apply the four tiers, commit them (so setSettings' own
// change-gated derivation runs against the NEW tiers, and `s` comes back holding whatever it derived),
// THEN apply every knob -- now against the post-derivation effective tiers rather than the pre-commit
// ones -- and commit again.
//
// COLLAPSING THIS INTO ONE setSettings CALL IS THE EXACT BUG R2's REGRESSION TEST CATCHES. Live
// settings: GI Medium, giCones 6 (Medium's own ladder rung). A manifest states GI 4 (Epic) and GICONES
// 6 -- explicitly asking for the SAME cone count Medium already had. Merge both edits into `s` before
// ever calling setSettings and the single commit sees globalIllumination go Medium -> Epic (a real
// change) with giCones ARRIVING AT 6 -- which is indistinguishable, to setSettings' own change-gated
// derivation, from "the caller left giCones alone", because 6 also happens to be what was already
// live. The tier changed and the knob "didn't", so the derivation fires and overwrites the caller's
// explicit 6 with ladder(Epic)'s 13 -- silently discarding a value the manifest asked for by name,
// purely because it collided with the OLD tier's own default.
//
// TWO-PHASE AVOIDS IT: phase 1 commits ONLY the tier change with giCones still untouched at 6, so the
// SAME derivation fires -- correctly, this time, since giCones really is unedited at this point -- and
// `s.giCones` comes back 13. Phase 2 then applies the manifest's explicit GICONES 6 on top of that,
// and commits again with the tier already Epic (unchanged in this second commit), so nothing re-derives
// over it. Final result: 6, exactly what the manifest asked for. See ProjectRenderApplyTest.cpp's own
// R2 case, which asserts two-phase gives 6 and records in a comment that a merged single call would
// have given 13.
template <class CommitReadBack>
inline void applyManifestTwoPhase(const fmt::ProjectDesc& project, Settings& s, CommitReadBack&& commit) {
    applyManifestTiers(project, s);
    commit();
    applyManifestKnobs(project, s);
    commit();
}

// ---- CAPTURE: Settings -> manifest -----------------------------------------------------------------

// Applies the capture rule for ONE of the ten keyed derived knobs to one ProjectDesc field.
// `group` is which Scalability group this knob belongs to, for the overallFollowMask bit test and for
// reading each side's TIER through groupTier (Scalability.hpp) -- the same tier accessor
// groupFollowsLadder and applyOverall already use, so "the group's tier" means the same thing
// everywhere this codebase asks the question.
namespace detail {
inline void captureKnob(int& manifestField, u32 requestedField, u32 liveField,
                         ScalabilityGroup group, const Settings& requested, const Settings& live,
                         u32 overallFollowMask) {
    // 1. An Overall preset just wrote this group -- the manifest goes back to "follow the tier".
    if ((overallFollowMask & (1u << static_cast<u32>(group))) != 0) { manifestField = -1; return; }
    // 2. The group's TIER changed and this specific knob did NOT -- the same test setSettings' own
    //    derivation uses to decide whether to recompute a knob from its tier. If the user only moved
    //    the tier slider and left this knob alone, let it keep following whatever tier comes next.
    if (groupTier(requested, group) != groupTier(live, group) && requestedField == liveField) {
        manifestField = -1;
        return;
    }
    // 3. An explicit edit -- captured as a pin, verbatim.
    if (requestedField != liveField) { manifestField = static_cast<int>(requestedField); return; }
    // 4. Neither of the above: leave the manifest value exactly as it already was (-1 stays -1; an
    //    existing pin stays pinned). Nothing to do.
}

// Applies the capture rule for one of the five DEVICE-CLAMPED fields (the three tier enums,
// meshShaders, msaa) to one ProjectDesc field. `deviceCanRun` answers "could THIS device even produce
// a `requested` different from `live` here" -- when it could not (N5), an unrelated edit on a machine
// without the hardware must not be the thing that overwrites a teammate's pin for hardware it does
// have.
inline void captureClamped(int& manifestField, u32 requestedField, u32 liveField, bool deviceCanRun) {
    // 1. An actual difference -- write it, regardless of whether this device could have produced it
    //    itself (a value already loaded from a manifest a capable machine wrote counts here).
    if (requestedField != liveField) { manifestField = static_cast<int>(requestedField); return; }
    // 2. requested == live, and this device could never have made them differ -- leave the manifest
    //    alone (N5): the equality tells us nothing about what the USER wants, only that Renderer::
    //    setSettings already clamped this field for us, silently, on every apply.
    if (!deviceCanRun) return;
    // 3. requested == live, and this device COULD run it -- today's existing behaviour: write it
    //    through anyway (a no-op byte-for-byte, since it already equals liveField).
    manifestField = static_cast<int>(requestedField);
}
} // namespace detail

// Writes every Voxi-owned RENDER.* field of `project` from `requested` (the Settings about to be
// applied -- e.g. buildRenderingSettings' local `s`, before vx.setSettings(s) runs) against `live`
// (voxi::Renderer::get().settings(), the settings from BEFORE this edit -- this call must run before
// the commit, so "live" means what it says). `overallFollowMask` is whatever applyOverall (or an
// Overall button that just called it) returned this same edit, 0 when no Overall preset ran.
//
// THE TEN KEYED DERIVED KNOBS use captureKnob's four-step rule above: GI's voxelResolution/giCones/
// giUpdateInterval/giRestirVisibility, RT's rtShadowRays/rtPixelsPerRayTile/rtShadowDenoise/
// rtRenderMode/refractionMode, PT's ptBounces. giSkyOcclusionRays/giSkyOcclusionTile have NO manifest
// key and are never captured -- there is nothing in `project` to write them into.
//
// THE FIVE DEVICE-CLAMPED FIELDS (the three tier enums, meshShaders, msaa) use captureClamped's rule.
//
// EVERYTHING ELSE keeps today's unconditional write-requested behaviour: giIntensity, giMaxDistance,
// layeredBsdf, giMode, denoiser, refractionStrength, refractionEdgeFade. giMode and denoiser now
// round-trip the stored REQUEST rather than a clamped value -- Lane 1 removed setSettings' own giMode
// clamp (Voxi.cpp), and denoiser was never clamped to begin with, so `requested.giMode`/
// `requested.denoiser` already ARE the honest ask in both cases.
inline void captureVoxiSettings(fmt::ProjectDesc& project, const Settings& requested,
                                 const Settings& live, const DeviceInfo& d, u32 overallFollowMask) {
    using detail::captureKnob;
    using detail::captureClamped;

    captureKnob(project.voxelResolution, requested.voxelResolution, live.voxelResolution,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.giCones, requested.giCones, live.giCones,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.giUpdateInterval, requested.giUpdateInterval, live.giUpdateInterval,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.restirVisibility, requested.giRestirVisibility, live.giRestirVisibility,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);

    captureKnob(project.rtShadowRays, requested.rtShadowRays, live.rtShadowRays,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtPixelsPerRayTile, requested.rtPixelsPerRayTile, live.rtPixelsPerRayTile,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtShadowDenoise, requested.rtShadowDenoise, live.rtShadowDenoise,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtRenderMode, requested.rtRenderMode, live.rtRenderMode,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.refractionMode, requested.refractionMode, live.refractionMode,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);

    captureKnob(project.ptBounces, requested.ptBounces, live.ptBounces,
                ScalabilityGroup::PathTracing, requested, live, overallFollowMask);

    captureClamped(project.giQuality, static_cast<u32>(requested.globalIllumination),
                   static_cast<u32>(live.globalIllumination),
                   featureStatus(Feature::GlobalIllumination, d) == Status::Ready);
    captureClamped(project.rayTracing, static_cast<u32>(requested.rayTracing),
                   static_cast<u32>(live.rayTracing),
                   featureStatus(Feature::RayTracing, d) == Status::Ready);
    captureClamped(project.pathTracing, static_cast<u32>(requested.pathTracing),
                   static_cast<u32>(live.pathTracing),
                   featureStatus(Feature::PathTracing, d) == Status::Ready);
    captureClamped(project.meshShaders, requested.meshShaders ? 1u : 0u, live.meshShaders ? 1u : 0u,
                   featureStatus(Feature::MeshShaders, d) == Status::Ready);
    // "a stated count not in msaaMask" -- DeviceInfo::msaaMask is tested directly against the sample
    // COUNT (bit N set means N samples are supported: `device_.msaaMask & samples`, Voxi.cpp's own
    // MSAA clamp), never against 1u << count.
    captureClamped(project.msaa, static_cast<u32>(requested.msaa), static_cast<u32>(live.msaa),
                   (d.msaaMask & static_cast<u32>(requested.msaa)) != 0);

    project.giIntensity        = requested.giIntensity;
    project.giMaxDistance      = requested.giMaxDistance;
    project.layeredBsdf        = static_cast<int>(requested.layeredBsdf);
    project.giMode             = static_cast<int>(requested.giMode);
    project.denoiser           = requested.denoiser ? 1 : 0;
    project.refractionStrength = requested.refractionStrength;
    project.refractionEdgeFade = requested.refractionEdgeFade;
}

} // namespace aver::voxi
