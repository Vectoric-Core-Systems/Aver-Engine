// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/voxi_abi.h"
#include "aver/core/Log.hpp"
// QualityLadder.hpp: every *ForQuality body below is now a one-line forward into aver::voxi::ladder --
// see that header for the switch statements and the reasoning that used to live in this file.
// RenderSettingsResolver.hpp: the new resolve() step inside setSettings, and Renderer::status's own
// forward to featureStatus. Including both here means every build of this shared library runs
// QualityLadder.hpp's tier-derivation static_asserts, not merely whatever happens to include it
// directly.
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"

#include <algorithm>

// The Voxi settings service and the C ABI the C# scripting layer P/Invokes.
namespace aver::voxi {

// Returns the process-wide instance.
Renderer& Renderer::get() {
    static Renderer inst;
    return inst;
}

// Records what the device can do and re-clamps the current settings against it.
void Renderer::setDeviceInfo(const DeviceInfo& info) {
    device_ = info;
    refusalLogged_ = 0;
    setSettings(settings_);
}

// Applies what is legal for this device; unsupported requests are clamped and logged once.
void Renderer::setSettings(const Settings& s) {
    Settings n = s;

    auto refuse = [&](Feature f) {
        const u32 bit = 1u << static_cast<u32>(f);
        if (refusalLogged_ & bit) return;
        refusalLogged_ |= bit;
        // THE DEVICE IS NOT ALWAYS AT FAULT, and saying it is sends the reader to the wrong place.
        // Unsupported means this GPU cannot; NotImplemented means the engine does not, on any GPU.
        // The old wording blamed hardware for both, so a NotImplemented feature read as "your card is
        // too old" -- someone could reasonably go shopping over a line of missing code.
        AVER_INFO(status(f) == Status::NotImplemented
                      ? "[Voxi] {} was requested but this engine does not implement it yet ({}); it stays off"
                      : "[Voxi] {} was requested but this device cannot run it ({}); it stays off",
                  featureName(f), statusText(f));
    };

    u32 samples = static_cast<u32>(n.msaa);
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) samples = 1;
    while (samples > 1 && !(device_.msaaMask & samples)) samples >>= 1;
    n.msaa = static_cast<Msaa>(samples);

    if (status(Feature::GlobalIllumination) != Status::Ready) {
        if (n.globalIllumination != Quality::Off) refuse(Feature::GlobalIllumination);
        n.globalIllumination = Quality::Off;
    }
    if (status(Feature::RayTracing) != Status::Ready) {
        if (n.rayTracing != Quality::Off) refuse(Feature::RayTracing);
        n.rayTracing = Quality::Off;
        // giMode==1 (RTXDI ReSTIR GI) needs the same RayQuery hardware the shadow/reflection rays
        // do -- it traces its own candidate ray through the identical acceleration structure and
        // flat geometry table. THIS USED TO FORCE n.giMode TO 0 HERE, as a CONSEQUENCE of the
        // RayTracing refusal just logged above, the same way rayTracing itself is clamped.
        //
        // NOT ANY MORE: giMode is now stored EXACTLY AS REQUESTED, unclamped, and resolved at READ
        // time instead (RenderSettingsResolver.hpp's resolve(), called near the bottom of this
        // function). That is safe because nothing downstream ever reads this raw field to decide
        // whether ReSTIR GI actually runs -- VoxiRenderer::giRestirWanted() is the only reader of
        // giMode_ (VoxiRenderer.hpp), and the shader itself branches on gGiRestirParams.x, a
        // per-frame constant that is reset to 0 every frame and set to 1 only inside the block
        // already gated on giRestirWanted()'s own ReSTIR history textures (VoxiRenderer.cpp) -- so a
        // value this device cannot honour is never seen by a shader regardless of what setSettings
        // does to the field it came from. Keeping the request lets the UI and the console show a
        // combo the way it was left (see resolve()'s FieldResolution: requested vs effective) instead
        // of silently discarding a choice the moment ray tracing goes off and on again.
    }
    if (status(Feature::PathTracing) != Status::Ready) {
        if (n.pathTracing != Quality::Off) refuse(Feature::PathTracing);
        n.pathTracing = Quality::Off;
    }
    if (status(Feature::MeshShaders) != Status::Ready) {
        if (n.meshShaders) refuse(Feature::MeshShaders);
        n.meshShaders = false;
    }

    // Clamped exactly like the four above it, and for the same reason -- a setting that a device or a
    // half-built feature cannot honour must not keep a value that says otherwise. While
    // status(LayeredBsdf) is NotImplemented this pins the field to Off, so nothing downstream has to
    // ask whether the value it is reading is real, and a project manifest carrying a stale rung is
    // reported once by refuse() rather than acted on.
    if (status(Feature::LayeredBsdf) != Status::Ready) {
        if (n.layeredBsdf != Quality::Off) refuse(Feature::LayeredBsdf);
        n.layeredBsdf = Quality::Off;
    }

    // Quality tiers used to leave voxelResolution completely alone -- selecting Epic cost nothing
    // extra because nothing read the tier to size the grid. When the caller changes the GI tier and
    // leaves voxelResolution exactly as it already was -- the common case: the editor's Quality
    // combo touched alone, or a project manifest that states giQuality but not voxelResolution (see
    // applyProjectRenderSettings in SandboxApp.cpp) -- derive the grid edge from the new tier. An
    // explicit voxelResolution request arriving in the SAME call (the incoming value differs from
    // what is currently active) always wins over the derived one, so the editor's "Voxel grid"
    // combo and a manifest's explicit voxelResolution keep overriding it exactly as before.
    if (n.globalIllumination != settings_.globalIllumination && n.voxelResolution == settings_.voxelResolution)
        n.voxelResolution = voxelResolutionForQuality(n.globalIllumination);

    // The same derivation for HOW OFTEN the volume is rebuilt, by exactly the same rule and for the
    // same reason: the tier sized the grid but said nothing about the revoxelisation rate, so every
    // tier paid the full always-fresh cost. Revoxelising measured at 108 ms of a 229 ms frame -- 47%,
    // the single largest item in it -- and dropping to one rebuild in four took the frame from 121.1
    // to 104.5 ms on the Electric Dreams scene.
    //
    // EPIC STAYS AT 1, which is the point. Epic means "do not compromise", so its indirect light
    // remains bit-identical to the always-fresh behaviour every tier used to have; only the cheaper
    // tiers buy speed with latency. The trade is temporal, not spatial -- indirect light lags scene
    // changes by up to N-1 frames and a static scene converges to exactly the same image.
    if (n.globalIllumination != settings_.globalIllumination && n.giUpdateInterval == settings_.giUpdateInterval)
        n.giUpdateInterval = giUpdateIntervalForQuality(n.globalIllumination);

    // And the CONE COUNT, by the same rule -- the one that was missing, and the one that carries
    // most of the cost. See Settings::giCones.
    if (n.globalIllumination != settings_.globalIllumination && n.giCones == settings_.giCones)
        n.giCones = giConesForQuality(n.globalIllumination);

    // U1: how much of F2/F3's cost this tier pays for, derived exactly as giCones is above -- same
    // "changed tier AND untouched field" rule, same reason. See Settings::giRestirVisibility.
    if (n.globalIllumination != settings_.globalIllumination && n.giRestirVisibility == settings_.giRestirVisibility)
        n.giRestirVisibility = giRestirVisibilityForQuality(n.globalIllumination);

    // REFRACTION FOLLOWS THE RAY-TRACING TIER, by the same by-value rule as every derived knob here:
    // only when the tier MOVED and the caller did not set the mode itself in the same call. A caller
    // asking for the value the field already holds is indistinguishable from one who never asked --
    // that is the trap SandboxApp's two-call setSettings pattern exists to step around, and it
    // applies to this exactly as it does to rtShadowRays.
    if (n.rayTracing != settings_.rayTracing && n.refractionMode == settings_.refractionMode)
        n.refractionMode = refractionForQuality(n.rayTracing);

    // The RT sun-shadow knobs follow their own tier the same way, and for a sharper reason: with RT
    // on by default there is no longer any configuration in which these are inert, so a tier change
    // that left them alone would advertise Medium while running whatever the last tier paid for.
    // Same "only if the caller did not set it explicitly in this call" rule as GI above.
    if (n.rayTracing != settings_.rayTracing && n.rtShadowRays == settings_.rtShadowRays)
        n.rtShadowRays = rtShadowRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtPixelsPerRayTile == settings_.rtPixelsPerRayTile)
        n.rtPixelsPerRayTile = rtPixelsPerRayTileForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtShadowDenoise == settings_.rtShadowDenoise)
        n.rtShadowDenoise = rtShadowDenoiseForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtRenderMode == settings_.rtRenderMode)
        n.rtRenderMode = rtRenderModeForQuality(n.rayTracing);
    // KEYED ON rayTracing, not globalIllumination, even though it is the AMBIENT term it corrects:
    // what it costs is a ray, and what makes it possible at all is the acceleration structure. A
    // project raising GI quality on hardware with ray tracing off must not start paying for rays.
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionRays == settings_.giSkyOcclusionRays)
        n.giSkyOcclusionRays = giSkyOcclusionRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionTile == settings_.giSkyOcclusionTile)
        n.giSkyOcclusionTile = giSkyOcclusionTileForQuality(n.rayTracing);
    // KEYED ON pathTracing, not rayTracing. A bounce budget is a path-tracing quantity; deriving
    // it from the ray-tracing tier is what let the two run out of step.
    if (n.pathTracing != settings_.pathTracing && n.ptBounces == settings_.ptBounces)
        n.ptBounces = ptBouncesForQuality(n.pathTracing);

    n.voxelResolution = std::clamp(n.voxelResolution, 32u, 512u);
    // At least the axial cone, or the gather returns nothing and GI silently switches itself off.
    // 16 is a ceiling on a per-pixel loop, for the same reason the bounce count has one.
    n.giCones         = std::clamp(n.giCones, 1u, 16u);
    // 1 is "a fresh direction per pixel" and is the no-op; 16 is well past where a tile stops being a
    // local neighbourhood and starts being a visible block. Clamped rather than rejected for the same
    // reason every other dial here is: a project asking for something silly gets the nearest sane
    // renderer, not a refusal to start.
    n.giSkyOcclusionTile = std::clamp(n.giSkyOcclusionTile, 1u, 16u);
    n.giIntensity     = std::clamp(n.giIntensity, 0.0f, 8.0f);
    n.giMaxDistance   = std::clamp(n.giMaxDistance, 1.0f, 100000.0f);
    // The GI radiance ceiling (AVER_VOX_MAXRAD's live half -- see the field's own comment). Lower
    // bound is deliberately > 0: the shader macro (voxi.hlsl/voxi_gi.hlsli) falls back to the
    // engine default 16.0 only when gViewParams.y arrives as exactly 0 (an UNSET FrameConstants
    // block, e.g. giFrameConstants() read before VoxiRenderer::init() has run at all), and letting a
    // real `set voxi.giRadianceCeiling 0` through here would collide with that sentinel and silently
    // do nothing instead of the near-zero ceiling the user actually asked for. Upper bound is
    // generous headroom, not a measured ceiling of its own.
    n.giRadianceCeiling = std::clamp(n.giRadianceCeiling, 0.1f, 256.0f);
    // Mirrors VoxiRenderer::kMaxShadowRays / kMaxPixelsPerRayTile, restated rather than shared: this
    // library is core-only and must not depend on the RHI-backed renderer that owns those constants.
    // The renderer's own setters are the authority on the exact contract (kMaxPixelsPerRayTile also
    // rounds to a power of two); this is just enough to keep a wild request off the wire to it.
    n.rtShadowRays       = std::clamp(n.rtShadowRays, 1u, 32u);
    // 3 is a (2*3+1)^2 = 49-tap neighbourhood, which is already past the point where a wider
    // kernel buys anything a second iteration would not buy more cheaply. Kept low deliberately:
    // this runs per FRAGMENT inside the shading shader, so the tap count multiplies by overdraw.
    n.rtShadowDenoise    = std::clamp(n.rtShadowDenoise, 0u, 3u);
    // 1 is the only mode that exists besides raster; anything else is a manifest typo, and
    // clamping to 1 rather than 0 would turn a typo into a silent renderer swap.
    n.rtRenderMode       = n.rtRenderMode > 1u ? 0u : n.rtRenderMode;
    // Same idiom as rtRenderMode directly above: 0..2 are the only staged modes that exist
    // (single pass, staged, staged + half-rate GI), so a typo clamps to 0 (single pass, the
    // comparison baseline) rather than silently opting a project into an experimental split or a
    // GI quality trade it never asked for.
    n.rayDrivenStages    = n.rayDrivenStages > 2u ? 0u : n.rayDrivenStages;
    // Same reasoning as rtRenderMode directly above: 1 is the only mode besides the cone gather, so
    // a garbage value clamps to the DEFAULT (0, cones) rather than silently landing on ReSTIR GI.
    n.giMode             = n.giMode > 1u ? 0u : n.giMode;
    // U1: 3 (Full) is the top of RestirVisibility, so a typo lands on the CORRECTED transport -- never
    // on 0 (NoRay), which would silently reintroduce the over-brightness cb4b48df's contrast fix exists
    // to remove. Unlike giMode/rtRenderMode just above, whose typos clamp to "nothing changed" (0), a
    // typo here clamps to the tier's own safest answer instead.
    n.giRestirVisibility = n.giRestirVisibility > 3u ? 3u : n.giRestirVisibility;
    // giRestirVisibility's typo-safety ternary immediately above, there is no "wrong" end of the
    // range here -- 0 (legacy, always 30) through 31 are all legitimate choices -- so a plain
    // std::clamp is enough, same idiom as giCones/giSkyOcclusionTile above. 31, not a rounder number,
    // because givis::packAmbientW packs this into exactly five bits (bits 7-11).
    // Settings::giRestirSpatialSamples's own comment has the bisection this splits reuse for. 15
    // (AUTO) through 0 (temporal only) are all legitimate choices with nothing to typo-guard against,
    // rounder number, because givis::packAmbientW packs this into exactly four bits (bits 12-15).
    n.giRestirSpatialSamples = std::clamp(n.giRestirSpatialSamples, 0u, 15u);
    // giRestirMaxHistory). 2 is RTXDI's highest bias-correction mode and 63 its own documented
    // ceiling for maxHistoryLength, which is also what the six packed bits hold.
    n.giRestirMaxHistory     = std::clamp(n.giRestirMaxHistory, 0u, 31u);
    // The boiling-filter strength the two disproven reuse tolerances used to occupy here: a
    // 8 is arbitrary but finite: an unbounded bounce count in a shader loop is a hang, and the
    // useful range for a real-time path tracer is nowhere near it.
    n.ptBounces          = std::clamp(n.ptBounces, 1u, 8u);
    n.rtPixelsPerRayTile = std::clamp(n.rtPixelsPerRayTile, 1u, 16u);
    // Mirrors VoxiRenderer::kMaxGiUpdateInterval for the same reason as rtPixelsPerRayTile above.
    n.giUpdateInterval   = std::clamp(n.giUpdateInterval, 1u, 8u);
    // REBLUR history/prepass dials -- ranges are NRD's OWN (third_party/nrd/Include/NRDSettings.h's
    // ReblurSettings), not guessed: maxAccumulatedFrameNum and maxStabilizedFrameNum are each
    // documented there as "[0; REBLUR_MAX_HISTORY_FRAME_NUM]" (63); a maxStabilizedFrameNum at or
    // above maxAccumulatedFrameNum is NRD's own business to clamp down further (its header says so
    // explicitly), not this engine's -- which is exactly the relationship today's defaults (63, 30)
    // already have, unchanged here. diffusePrepassBlurRadius's own doc gives only a lower bound ("0 =
    // disabled"); 100 is a defensive ceiling this engine adds so a manifest typo cannot select an
    // unbounded pixel radius, not a value NRD itself states.
    n.reblurMaxAccumulatedFrameNum = std::clamp(n.reblurMaxAccumulatedFrameNum, 0u, 63u);
    n.reblurMaxStabilizedFrameNum  = std::clamp(n.reblurMaxStabilizedFrameNum, 0u, 63u);
    n.reblurDiffusePrepassBlurRadius = std::clamp(n.reblurDiffusePrepassBlurRadius, 0.0f, 100.0f);
    // The residual-noise dials, NRD's documented ranges. The two antilag scales only need to stay
    // positive; 100 is a defensive ceiling, not an NRD figure. The frame counts keep NRD's own
    // ordering (historyFix < fast <= main) so a dial set alone cannot hand NRD an illegal triple.
    n.reblurAntilagSigmaScale     = std::clamp(n.reblurAntilagSigmaScale, 0.01f, 100.0f);
    n.reblurAntilagSensitivity    = std::clamp(n.reblurAntilagSensitivity, 0.01f, 100.0f);
    n.reblurMinHitDistanceWeight  = std::clamp(n.reblurMinHitDistanceWeight, 0.001f, 0.2f);
    n.reblurFastHistoryClampSigma = std::clamp(n.reblurFastHistoryClampSigma, 1.0f, 3.0f);
    n.reblurMaxFastAccumulatedFrameNum =
        std::min(n.reblurMaxFastAccumulatedFrameNum, n.reblurMaxAccumulatedFrameNum);
    n.reblurHistoryFixFrameNum = n.reblurMaxFastAccumulatedFrameNum == 0u ? 0u
        : std::min(n.reblurHistoryFixFrameNum, n.reblurMaxFastAccumulatedFrameNum - 1u);
    n.reblurSunMovingFrameNum = std::min(n.reblurSunMovingFrameNum, 63u);
    n.reblurMinBlurRadius = std::clamp(n.reblurMinBlurRadius, 0.0f, 100.0f);
    n.reblurMaxBlurRadius = std::clamp(n.reblurMaxBlurRadius, n.reblurMinBlurRadius, 100.0f);

    // A GARBAGE VALUE, NOT A HARDWARE ONE: mirrors n.rtRenderMode's own range clamp a few lines above
    // rather than replacing it -- 2 is the top of the enum RayTraced names, so anything past it is a
    // manifest typo and clamps down to ScreenSpace (1), never to 0 (Off would silently turn refraction
    // off outright over a typo, which is a bigger behaviour change than a typo earns). The HARDWARE
    // question -- can this device actually run RayTraced refraction right now -- is a separate concern
    // and belongs to resolve() below, not to this range check.
    n.refractionMode = n.refractionMode > 2u ? 1u : n.refractionMode;

    // THE PREREQUISITE PASS: everything above this line clamps a field against its OWN valid range or
    // against whether its FEATURE is supported at all (globalIllumination/rayTracing/pathTracing/
    // meshShaders, at the top of this function). What those clamps do not answer is the finer-grained
    // question RenderSettingsResolver.hpp exists for: given a tier that IS legal, is this specific
    // NON-default value inside it legal right now -- ReSTIR GI needs RT hardware AND the RT tier on
    // AND the GI tier on; ray-driven primary visibility and ray-traced refraction each need RT
    // hardware AND the RT tier on. giMode is stored as requested regardless (see the comment where its
    // old forced-Off clamp used to be, above) and resolved fresh by every reader instead -- but
    // rtRenderMode and refractionMode are still clamped here, in settings_ itself, so a raw reader
    // (the A2 self-contradiction check, SandboxApp.cpp, among others) sees the same honest value the
    // UI and the console do, without every one of them having to call resolve() itself just to avoid
    // being lied to by the stored field.
    const Resolution r = resolve(n, device_);
    n.rtRenderMode   = r.rtRenderMode.effective;
    n.refractionMode = r.refractionMode.effective;

    if (n.msaa != settings_.msaa) msaaDirty_ = true;
    settings_ = n;
}

// Returns whether a feature is usable on this device. The switch statement and its per-feature
// reasoning now live in RenderSettingsResolver.hpp's featureStatus -- lifted there rather than kept
// here so the same device-capability answer is reachable without a live Renderer instance (a manifest
// apply resolving a project's request before it is committed, or a unit test's hand-built device).
Status Renderer::status(Feature f) const { return featureStatus(f, device_); }

// Returns a readable reason for a feature's status.
const char* Renderer::statusText(Feature f) const {
    switch (status(f)) {
        case Status::Ready:          return "Ready";
        case Status::NotImplemented: return "Not implemented yet";
        case Status::Unsupported:
            switch (f) {
                case Feature::Msaa:               return "Device reports no MSAA";
                case Feature::GlobalIllumination: return "Device has no compute support";
                case Feature::MeshShaders:        return "Needs mesh-shader Tier 1 + SM 6.5 (D3D12 Ultimate)";
                default:                          return "Needs DXR 1.1 + SM 6.5 (D3D12 Ultimate)";
            }
        default: return "Unknown";
    }
}

// Returns a feature's display name.
const char* Renderer::featureName(Feature f) {
    switch (f) {
        case Feature::Msaa:               return "Anti-Aliasing (MSAA)";
        case Feature::GlobalIllumination: return "Global Illumination";
        case Feature::RayTracing:         return "Ray Tracing";
        case Feature::PathTracing:        return "Path Tracing";
        case Feature::MeshShaders:        return "Mesh Shaders";
        case Feature::LayeredBsdf:        return "Layered BSDF";
        default: return "?";
    }
}

// EVERY *ForQuality BODY BELOW IS NOW A ONE-LINE FORWARD into aver::voxi::ladder
// (QualityLadder.hpp) -- the switch statements and the reasoning that used to sit directly above
// each of them (one measured table, one retuning history, one torn-pair postmortem, apiece) have
// moved there instead, so an edit to a rung's value and the comment justifying it can no longer
// drift apart the way ladder::rtPixelsPerRayTile's own history records happening here once. These
// stay in this file, as one-liners, only because Renderer::*ForQuality is this shared library's
// exported, P/Invoke-adjacent API surface, and QualityLadder.hpp is not.
u32 Renderer::refractionForQuality(Quality q) { return ladder::refraction(q); }         // reasoning: QualityLadder.hpp, ladder::refraction
u32 Renderer::giConesForQuality(Quality q) { return ladder::giCones(q); }               // reasoning: QualityLadder.hpp, ladder::giCones
u32 Renderer::giRestirVisibilityForQuality(Quality q) { return ladder::giRestirVisibility(q); } // reasoning: QualityLadder.hpp, ladder::giRestirVisibility
u32 Renderer::voxelResolutionForQuality(Quality q) { return ladder::voxelResolution(q); } // reasoning: QualityLadder.hpp, ladder::voxelResolution
u32 Renderer::giUpdateIntervalForQuality(Quality q) { return ladder::giUpdateInterval(q); } // reasoning: QualityLadder.hpp, ladder::giUpdateInterval
u32 Renderer::giSkyOcclusionRaysForQuality(Quality q) { return ladder::giSkyOcclusionRays(q); } // reasoning: QualityLadder.hpp, ladder::giSkyOcclusionRays
u32 Renderer::giSkyOcclusionTileForQuality(Quality q) { return ladder::giSkyOcclusionTile(q); } // reasoning: QualityLadder.hpp, ladder::giSkyOcclusionTile
u32 Renderer::rtShadowRaysForQuality(Quality q) { return ladder::rtShadowRays(q); }     // reasoning: QualityLadder.hpp, ladder::rtShadowRays
u32 Renderer::rtRenderModeForQuality(Quality q) { return ladder::rtRenderMode(q); }     // reasoning: QualityLadder.hpp, ladder::rtRenderMode
u32 Renderer::ptBouncesForQuality(Quality q) { return ladder::ptBounces(q); }           // reasoning: QualityLadder.hpp, ladder::ptBounces
u32 Renderer::rtShadowDenoiseForQuality(Quality q) { return ladder::rtShadowDenoise(q); } // reasoning: QualityLadder.hpp, ladder::rtShadowDenoise
u32 Renderer::rtPixelsPerRayTileForQuality(Quality q) { return ladder::rtPixelsPerRayTile(q); } // reasoning: QualityLadder.hpp, ladder::rtPixelsPerRayTile

// Returns a quality level's display name.
const char* Renderer::qualityName(Quality q) {
    switch (q) {
        case Quality::Off:    return "Off";
        case Quality::Low:    return "Low";
        case Quality::Medium: return "Medium";
        case Quality::High:   return "High";
        case Quality::Epic:   return "Epic";
        default: return "?";
    }
}

// Returns true once after settings.msaa changes, then clears the flag.
bool Renderer::consumeMsaaDirty() {
    const bool d = msaaDirty_;
    msaaDirty_ = false;
    return d;
}

// Same one-shot shape as consumeMsaaDirty() above, one per reset* console command -- see the header's
// own comment on requestGiHistoryReset() and its siblings.
bool Renderer::consumeGiHistoryResetRequest() {
    const bool d = giHistoryResetRequested_;
    giHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeRtHistoryResetRequest() {
    const bool d = rtHistoryResetRequested_;
    rtHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeAoHistoryResetRequest() {
    const bool d = aoHistoryResetRequested_;
    aoHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeNrdHistoryResetRequest() {
    const bool d = nrdHistoryResetRequested_;
    nrdHistoryResetRequested_ = false;
    return d;
}

} // namespace aver::voxi

// ------------------------------------------------------------------ C ABI
using aver::voxi::Feature;
using aver::voxi::Quality;
using aver::voxi::Renderer;
using aver::voxi::Settings;

namespace {
// True when a feature id is in range.
bool validFeature(int32_t f) { return f >= 0 && f < AVER_VOXI_FEATURE_COUNT; }
// Returns the quality field a feature id names, or null.
Quality* qualitySlot(Settings& s, int32_t f) {
    switch (f) {
        case AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION: return &s.globalIllumination;
        case AVER_VOXI_FEATURE_RAY_TRACING:         return &s.rayTracing;
        case AVER_VOXI_FEATURE_PATH_TRACING:        return &s.pathTracing;
        default: return nullptr;
    }
}
} // namespace

extern "C" {

// Returns the number of features.
int32_t aver_voxi_feature_count(void) { return AVER_VOXI_FEATURE_COUNT; }

// Returns a feature's display name.
const char* aver_voxi_feature_name(int32_t f) {
    return validFeature(f) ? Renderer::featureName(static_cast<Feature>(f)) : "?";
}
// Returns a feature's status id.
int32_t aver_voxi_feature_status(int32_t f) {
    return validFeature(f) ? static_cast<int32_t>(Renderer::get().status(static_cast<Feature>(f)))
                           : AVER_VOXI_STATUS_UNSUPPORTED;
}
// Returns a feature's status as readable text.
const char* aver_voxi_feature_status_text(int32_t f) {
    return validFeature(f) ? Renderer::get().statusText(static_cast<Feature>(f)) : "?";
}

// Returns the current MSAA sample count.
int32_t aver_voxi_get_msaa(void) { return static_cast<int32_t>(Renderer::get().settings().msaa); }

// Sets the MSAA sample count. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_msaa(int32_t samples) {
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return 0;
    Settings s = Renderer::get().settings();
    s.msaa = static_cast<aver::voxi::Msaa>(samples);
    Renderer::get().setSettings(s);
    return static_cast<int32_t>(Renderer::get().settings().msaa) == samples ? 1 : 0;
}

// Returns the supported sample counts as a bit mask.
int32_t aver_voxi_msaa_mask(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().msaaMask); }

// Returns a feature's quality level.
int32_t aver_voxi_get_quality(int32_t f) {
    Settings s = Renderer::get().settings();
    const Quality* q = qualitySlot(s, f);
    return q ? static_cast<int32_t>(*q) : AVER_VOXI_QUALITY_OFF;
}

// Sets a feature's quality level. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_quality(int32_t f, int32_t quality) {
    if (quality < AVER_VOXI_QUALITY_OFF || quality > AVER_VOXI_QUALITY_EPIC) return 0;
    Settings s = Renderer::get().settings();
    Quality* slot = qualitySlot(s, f);
    if (!slot) return 0;
    *slot = static_cast<Quality>(quality);
    Renderer::get().setSettings(s);
    Settings after = Renderer::get().settings();
    const Quality* now = qualitySlot(after, f);
    return (now && static_cast<int32_t>(*now) == quality) ? 1 : 0;
}

// Returns the cubic voxel grid edge.
int32_t aver_voxi_get_voxel_resolution(void) { return static_cast<int32_t>(Renderer::get().settings().voxelResolution); }
// Sets the cubic voxel grid edge. Returns 0 if out of range.
int32_t aver_voxi_set_voxel_resolution(int32_t res) {
    if (res < 32 || res > 512) return 0;
    Settings s = Renderer::get().settings(); s.voxelResolution = static_cast<aver::u32>(res);
    Renderer::get().setSettings(s); return 1;
}
// Returns the indirect bounce multiplier.
float   aver_voxi_get_gi_intensity(void) { return Renderer::get().settings().giIntensity; }
// Sets the indirect bounce multiplier.
int32_t aver_voxi_set_gi_intensity(float v) {
    Settings s = Renderer::get().settings(); s.giIntensity = v; Renderer::get().setSettings(s); return 1;
}
// Returns the cone trace range in centimetres.
float   aver_voxi_get_gi_max_distance(void) { return Renderer::get().settings().giMaxDistance; }
// Sets the cone trace range, in centimetres.
int32_t aver_voxi_set_gi_max_distance(float cm) {
    Settings s = Renderer::get().settings(); s.giMaxDistance = cm; Renderer::get().setSettings(s); return 1;
}

// Returns how many frames apart the GI volume is re-voxelised (1 = every frame).
int32_t aver_voxi_get_gi_update_interval(void) { return static_cast<int32_t>(Renderer::get().settings().giUpdateInterval); }
// Sets the GI revoxelise interval, in frames. Returns 0 if out of range.
int32_t aver_voxi_set_gi_update_interval(int32_t frames) {
    if (frames < 1 || frames > 8) return 0;
    Settings s = Renderer::get().settings(); s.giUpdateInterval = static_cast<aver::u32>(frames);
    Renderer::get().setSettings(s); return 1;
}

// Returns the device's ray tracing tier.
int32_t aver_voxi_ray_tracing_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().rayTracingTier); }
// Returns the device's highest supported MSAA sample count.
int32_t aver_voxi_max_msaa(void)         { return static_cast<int32_t>(Renderer::get().deviceInfo().maxMsaaSamples); }
// Returns the device's mesh shader tier.
int32_t aver_voxi_mesh_shader_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().meshShaderTier); }
// Returns the device's shader model.
int32_t aver_voxi_shader_model(void)     { return static_cast<int32_t>(Renderer::get().deviceInfo().shaderModel); }
// Returns 1 when the mesh shader submission path is on.
int32_t aver_voxi_get_mesh_shaders(void) { return Renderer::get().settings().meshShaders ? 1 : 0; }
// Turns the mesh shader submission path on or off. Returns 0 if the device refused it.
int32_t aver_voxi_set_mesh_shaders(int32_t on) {
    Settings s = Renderer::get().settings(); s.meshShaders = on != 0;
    Renderer::get().setSettings(s);
    return Renderer::get().settings().meshShaders == (on != 0) ? 1 : 0;
}

} // extern "C"
