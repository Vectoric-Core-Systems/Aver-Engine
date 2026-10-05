// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// One place for all "why is this greyed out?" answers: pure function of Settings + DeviceInfo.
// Headless: no ImGui, no Renderer::get(), no AVER_WARN. Enables unit testing without GPU.
// resolve() answers finer-grained prerequisites (ReSTIR, ray-driven refraction, denoiser).
// Device-level clamps (Off when unsupported) are already done; this adds field-level gates.

namespace aver::voxi {

// Why a control that asked for something the device or the current tier configuration cannot grant
// right now is greyed, or a checkbox reads a value different from what was requested. Each value
// answers exactly one prerequisite failure; see disableReasonText() for the sentence a tooltip shows.
enum class DisableReason : u8 {
    None = 0,
    RequiresComputeShaders,      // Feature::GlobalIllumination's own device gate (Voxi.cpp status())
    RequiresRayTracingHardware,  // DXR 1.1 / ray query + SM 6.5 + DXC -- Feature::RayTracing's gate
    RequiresRayTracingEnabled,   // the hardware is fine; the project's rayTracing tier is set to Off
    RequiresGlobalIllumination,  // ReSTIR GI specifically needs globalIllumination not Off too --
                                 // giRestirWanted() requires giEnabled() (VoxiRenderer.hpp)
    RequiresPathTracingHardware, // Feature::PathTracing's device gate (mirrors RayTracing's, plus
                                 // compute shaders)
    RequiresMeshShaderHardware,  // Feature::MeshShaders' device gate
    RequiresDenoiserBackend,     // DeviceInfo::denoiserSupported is false: not D3D12 (no G-buffer)
    NothingToDenoise,            // hardware and tiers are fine, but nothing is producing a signal the
                                 // denoiser could filter (VoxiRenderer's denoiser create gate)
    RequiresMsaaOne,              // SOFT: the denoiser needs single-sample targets. Warns; does not grey.
    NotImplemented,               // the engine itself has not built this yet, on any device
    RequiresRestirGi,             // U1: giRestirVisibility only applies once giMode itself resolves
                                   // to ReSTIR -- Resolution::giRestirVisibility's own gate
    RequiresRayDrivenPrimary,     // milestone 1: rayDrivenStages only applies once rtRenderMode
                                   // itself resolves to primary rays -- Resolution::rayDrivenStages'
                                   // own gate
    RequiresStagedRayDriven,      // SOFT: giRestirVisibility = Cached (4) only runs in the STAGED
                                   // ray-driven path on D3D12 (rtRenderMode 1, rayDrivenStages >= 1);
                                   // anywhere else it behaves as Half resolution. Warns; does not grey.
    RequiresDenoiser,             // neuralDenoise only acts while the denoiser itself runs
    Count
};

// Feature usability on a given device (same as Renderer::status, but takes DeviceInfo directly).
inline Status featureStatus(Feature f, const DeviceInfo& d) {
    switch (f) {
        case Feature::Msaa:
            return d.maxMsaaSamples > 1 ? Status::Ready : Status::Unsupported;
        case Feature::GlobalIllumination:
            if (!d.computeShaders) return Status::Unsupported;
            return Status::Ready;
        case Feature::RayTracing:
            if (d.rayTracingTier < 11 || d.shaderModel < 65 || !d.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::PathTracing:
            // Mirrors PathTracer::init()'s gate: RT hardware + compute shaders.
            if (d.rayTracingTier < 11 || d.shaderModel < 65 || !d.dxcAvailable || !d.computeShaders)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::MeshShaders:
            if (d.meshShaderTier == 0 || d.shaderModel < 65 || !d.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::LayeredBsdf:
            // Arithmetic in pixel shader; no device gate needed.
            return Status::Ready;
        default: return Status::Unsupported;
    }
}

// One sentence per reason, for a tooltip. Never null -- an unlisted or future enum value still reads
// as a (deliberately vague) sentence rather than crashing a caller that forgot to update a switch.
inline const char* disableReasonText(DisableReason r) {
    switch (r) {
        case DisableReason::None:
            return "No prerequisite is unmet.";
        case DisableReason::RequiresComputeShaders:
            return "Needs a device with compute shader support.";
        case DisableReason::RequiresRayTracingHardware:
            return "Needs DXR 1.1 / ray query hardware, shader model 6.5 and a DXIL compiler.";
        case DisableReason::RequiresRayTracingEnabled:
            return "Ray Tracing is set to Off on this project.";
        case DisableReason::RequiresGlobalIllumination:
            return "Global Illumination is set to Off on this project.";
        case DisableReason::RequiresPathTracingHardware:
            return "Needs the same ray-tracing hardware Ray Tracing does, plus compute shaders.";
        case DisableReason::RequiresMeshShaderHardware:
            return "Needs mesh-shader Tier 1, shader model 6.5 and a DXIL compiler.";
        case DisableReason::RequiresDenoiserBackend:
            return "Needs the D3D12 backend, the only one with the G-buffer the denoiser reads.";
        case DisableReason::NothingToDenoise:
            return "Nothing is producing a signal for the denoiser to filter yet.";
        case DisableReason::RequiresMsaaOne:
            return "The denoiser needs single-sample render targets; it skips itself above 1x MSAA.";
        case DisableReason::NotImplemented:
            return "This engine does not implement it yet, on any device.";
        case DisableReason::RequiresRestirGi:
            return "Only applies when Indirect diffuse is ReSTIR.";
        case DisableReason::RequiresRayDrivenPrimary:
            return "Only applies when Primary visibility is Primary rays.";
        case DisableReason::RequiresStagedRayDriven:
            return "Cached needs staged ray-driven primary visibility on D3D12; until then it runs as Half resolution.";
        case DisableReason::RequiresDenoiser:
            return "Only applies when the Denoiser is on.";
        default:
            return "Unavailable.";
    }
}

// Whether reason greys control (hides choice) vs. warns. Two reasons are soft: MSAA (fixable in one click) and staged-ray-driven (has working fallback).
inline bool greysControl(DisableReason r) {
    return r != DisableReason::None && r != DisableReason::RequiresMsaaOne &&
           r != DisableReason::RequiresStagedRayDriven;
}

// One field's resolved state: what was asked for, what is actually in effect, and why they differ
// (None when they don't).
struct FieldResolution {
    u32 requested = 0;
    u32 effective = 0;
    DisableReason reason = DisableReason::None;
};

// Prerequisite resolution: globalIllumination/rayTracing/pathTracing are copied as-is (already hardware-clamped).
struct Resolution {
    Quality globalIllumination, rayTracing, pathTracing;
    FieldResolution giMode, rtRenderMode, refractionMode, denoiser;
    FieldResolution neuralDenoise;        // Borrows the denoiser's reason chain
    FieldResolution giRestirVisibility;   // U1: effective == requested always (gated by giMode)
    FieldResolution giRestirMaxHistory;   // Borrows giRestirVisibility's reason chain
    FieldResolution rayDrivenStages;      // Borrows rtRenderMode's reason chain
    FieldResolution fogOcclusion;         // Built from GI voxel volume
    DisableReason rtSubControls = DisableReason::None;
    DisableReason ptSubControls = DisableReason::None;
    bool denoiserGBufferWanted = false;
    u32  sampleCount = 1;   // what the device should run at: Settings::msaa, or 1 under ray-driven primary
};

// Resolve prerequisites: effective = requested && reason == None. refractionMode falls back to ScreenSpace (1), not Off.
// denoiser checks giMode.effective (not request), so resolve giMode first.
// Reasons computed regardless of selection (reused by UI for greying and warnings).
inline Resolution resolve(const Settings& s, const DeviceInfo& d) {
    Resolution r{};
    r.globalIllumination = s.globalIllumination;
    r.rayTracing         = s.rayTracing;
    r.pathTracing         = s.pathTracing;

    const bool rtHardware = featureStatus(Feature::RayTracing, d) == Status::Ready;
    // RT gate: hardware first (GPU limit), then tier (project choice). Shared by all RT-gated fields.
    const DisableReason rtGate =
        !rtHardware ? DisableReason::RequiresRayTracingHardware
                    : (s.rayTracing == Quality::Off ? DisableReason::RequiresRayTracingEnabled
                                                     : DisableReason::None);

    // ---- giMode = 1 (ReSTIR GI): RT hardware, RT tier not Off, GI tier not Off, in that order ----
    r.giMode.requested = s.giMode;
    r.giMode.reason = (rtGate != DisableReason::None)
                           ? rtGate
                           : (s.globalIllumination == Quality::Off
                                  ? DisableReason::RequiresGlobalIllumination
                                  : DisableReason::None);
    r.giMode.effective = (s.giMode != 0 && r.giMode.reason == DisableReason::None) ? 1u : 0u;

    // ---- giRestirVisibility (U1): only meaningful while ReSTIR GI itself runs ----
    // Borrows giMode's reason; adds RequiresRestirGi when giMode not enabled. 4 (Cached) passes; >4 clamps to 3 (Full).
    r.giRestirVisibility.requested = s.giRestirVisibility > 4u ? 3u : s.giRestirVisibility;
    r.giRestirVisibility.reason = (r.giMode.reason != DisableReason::None)
                                       ? r.giMode.reason
                                       : (s.giMode == 0 ? DisableReason::RequiresRestirGi : DisableReason::None);
    // SOFT reason for Cached (4): cache code only in staged ray-driven on D3D12; elsewhere runs as Half resolution.
    if (r.giRestirVisibility.reason == DisableReason::None && r.giRestirVisibility.requested == 4u &&
        !(s.rtRenderMode == 1u && s.rayDrivenStages >= 1u && d.denoiserSupported))
        r.giRestirVisibility.reason = DisableReason::RequiresStagedRayDriven;
    // effective == requested ALWAYS -- deliberately unlike giMode/rtRenderMode/denoiser above. Clamping to 0 on a
    // failed prerequisite would read "No ray (over-bright)" while no ReSTIR runs at all; inertness is carried by
    // `reason` alone. Do not "fix" this to match the file's general rule (the comment at :169).
    r.giRestirVisibility.effective = r.giRestirVisibility.requested;

    // ---- giRestirMaxHistory: borrows giRestirVisibility's prerequisite, not tier-derived, plain pass-through [0,31]. ----
    r.giRestirMaxHistory.requested = s.giRestirMaxHistory;
    r.giRestirMaxHistory.reason = (r.giMode.reason != DisableReason::None)
                                       ? r.giMode.reason
                                       : (s.giMode == 0 ? DisableReason::RequiresRestirGi : DisableReason::None);
    // effective == requested ALWAYS, for giRestirVisibility's own reason (:169's comment): clamping to 0
    // on a failed prerequisite would read as a live choice rather than an inert one. Inertness is
    // carried by `reason` alone; do not "fix" this to match the file's general rule.
    r.giRestirMaxHistory.effective = r.giRestirMaxHistory.requested;

    // ---- rtRenderMode = 1 (ray-driven primary visibility): RT hardware, RT tier not Off ----
    r.rtRenderMode.requested = s.rtRenderMode;
    r.rtRenderMode.reason    = rtGate;
    r.rtRenderMode.effective = (s.rtRenderMode != 0 && rtGate == DisableReason::None) ? 1u : 0u;

    // ---- rayDrivenStages (milestone 1): borrows rtRenderMode's reason, plain pass-through [0,1]. ----
    r.rayDrivenStages.requested = s.rayDrivenStages;
    r.rayDrivenStages.reason = (r.rtRenderMode.reason != DisableReason::None)
                                   ? r.rtRenderMode.reason
                                   : (s.rtRenderMode == 0 ? DisableReason::RequiresRayDrivenPrimary
                                                           : DisableReason::None);
    // effective == requested ALWAYS, for giRestirMaxHistory's own reason (see its comment above):
    // clamping to 0 on a failed prerequisite would read as a live choice (Single pass) rather than
    // an inert one. Inertness is carried by `reason` alone.
    r.rayDrivenStages.effective = r.rayDrivenStages.requested;

    // ---- fogOcclusion: built from GI voxel volume. Compute shaders first, then GI tier. ----
    r.fogOcclusion.requested = s.fogOcclusion ? 1u : 0u;
    r.fogOcclusion.reason =
        featureStatus(Feature::GlobalIllumination, d) != Status::Ready
            ? DisableReason::RequiresComputeShaders
            : (s.globalIllumination == Quality::Off ? DisableReason::RequiresGlobalIllumination
                                                     : DisableReason::None);
    r.fogOcclusion.effective =
        (s.fogOcclusion && r.fogOcclusion.reason == DisableReason::None) ? 1u : 0u;

    // ---- refractionMode = 2 (RayTraced): RT hardware, RT tier not Off. Falls back to ScreenSpace (1), not Off. ----
    r.refractionMode.requested = s.refractionMode;
    r.refractionMode.reason    = rtGate;
    r.refractionMode.effective =
        (s.refractionMode >= 2u && rtGate != DisableReason::None) ? 1u : s.refractionMode;

    // ---- the device's sample count: one sample a pixel whenever a ray finds the first surface ----
    // A ray-driven frame shades once per pixel whatever MSAA says, so the hosts push 1 then
    // (effectiveSampleCount below). Raster frames keep MSAA; the backend resolves their G-buffer.
    const bool rayPrimary = r.rtRenderMode.effective == 1u ||
                            (s.pathTracing != Quality::Off && rtGate == DisableReason::None);
    r.sampleCount = rayPrimary ? 1u : static_cast<u32>(s.msaa);

    // ---- denoiser: RT hardware, RT tier not Off, denoiserSupported, something to denoise ----
    // MSAA no longer blocks it: the D3D12 backend (the only one with a G-buffer) resolves its
    // multisampled G-buffer to one sample a pixel (gbuffer_msaa_resolve.hlsl).
    DisableReason denoiseReason = rtGate;
    if (denoiseReason == DisableReason::None && !d.denoiserSupported)
        denoiseReason = DisableReason::RequiresDenoiserBackend;
    // NRD2 (denoiserKind 2) denoises the whole composed lighting, so it always has a signal.
    if (denoiseReason == DisableReason::None && r.giMode.effective == 0 && s.giSkyOcclusionRays == 0 &&
        s.denoiserKind != 2u)
        denoiseReason = DisableReason::NothingToDenoise;
    r.denoiser.requested = s.denoiser ? 1u : 0u;
    r.denoiser.reason    = denoiseReason;
    r.denoiser.effective = (s.denoiser && denoiseReason == DisableReason::None) ? 1u : 0u;
    r.neuralDenoise.requested = s.neuralDenoise ? 1u : 0u;
    r.neuralDenoise.reason    = denoiseReason != DisableReason::None ? denoiseReason
                              : (!s.denoiser ? DisableReason::RequiresDenoiser : DisableReason::None);
    r.neuralDenoise.effective = (s.neuralDenoise && r.denoiser.effective) ? 1u : 0u;

    // Allocate ~54 MB G-buffer when: denoiser requested (Path Tracing turns it on in VoxiRenderer::setSettings)
    // AND (no reason, or only soft MSAA reason).
    r.denoiserGBufferWanted =
        (s.denoiser || s.pathTracing != Quality::Off) &&
        (denoiseReason == DisableReason::None || denoiseReason == DisableReason::RequiresMsaaOne);

    // ---- the RT/PT page rows below their Quality combos ----
    r.rtSubControls = rtGate;
    // PT sub-rows: only one observable state (tier Off), so reuse RequiresPathTracingHardware.
    r.ptSubControls =
        (s.pathTracing == Quality::Off) ? DisableReason::RequiresPathTracingHardware : DisableReason::None;

    return r;
}

// One manifest-stated value per watched field, or -1 if absent (mirrors fmt::ProjectDesc convention).
struct ManifestAsks {
    int giMode = -1;
    int denoiser = -1;
    int rtRenderMode = -1;
    int refractionMode = -1;
};

// One field the manifest stated whose value did not survive into the effective settings, and why.
struct FieldReport {
    const char* field;
    const char* manifestKey;
    u32 requested;
    DisableReason reason;
};

// Reports manifest fields (where asked.X >= 0) whose effective value differs from requested.
// Returns count written into out[] (capacity 4).
inline u32 manifestContradictions(const Settings& effective, const DeviceInfo& d, const ManifestAsks& asks,
                                   FieldReport out[4]) {
    const Resolution r = resolve(effective, d);
    u32 n = 0;
    if (asks.giMode >= 0 && static_cast<u32>(asks.giMode) != r.giMode.effective)
        out[n++] = FieldReport{"giMode", "GIMODE", static_cast<u32>(asks.giMode), r.giMode.reason};
    if (asks.denoiser >= 0 && (static_cast<u32>(asks.denoiser) != 0) != (r.denoiser.effective != 0))
        out[n++] = FieldReport{"denoiser", "DENOISER", static_cast<u32>(asks.denoiser), r.denoiser.reason};
    if (asks.rtRenderMode >= 0 && static_cast<u32>(asks.rtRenderMode) != r.rtRenderMode.effective)
        out[n++] = FieldReport{"rtRenderMode", "RTRENDERMODE", static_cast<u32>(asks.rtRenderMode),
                                r.rtRenderMode.reason};
    if (asks.refractionMode >= 0 && static_cast<u32>(asks.refractionMode) != r.refractionMode.effective)
        out[n++] = FieldReport{"refractionMode", "REFRACTIONMODE", static_cast<u32>(asks.refractionMode),
                                r.refractionMode.reason};
    return n;
}

// Maps DisableReason to Feature (for already-logged hardware refusals). Returns false for soft/tier reasons.
inline bool refusalFeatureFor(DisableReason r, Feature& out) {
    switch (r) {
        case DisableReason::RequiresComputeShaders:      out = Feature::GlobalIllumination; return true;
        case DisableReason::RequiresRayTracingHardware:  out = Feature::RayTracing;          return true;
        case DisableReason::RequiresPathTracingHardware: out = Feature::PathTracing;         return true;
        case DisableReason::RequiresMeshShaderHardware:  out = Feature::MeshShaders;         return true;
        // Kept for future unimplemented features (LayeredBsdf is Ready in Voxi.cpp)
        case DisableReason::NotImplemented:              out = Feature::LayeredBsdf;         return true;
        default: return false;
    }
}

} // namespace aver::voxi
