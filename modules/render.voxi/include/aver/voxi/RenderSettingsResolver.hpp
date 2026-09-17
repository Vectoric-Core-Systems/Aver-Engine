// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// WHY A SETTING IS GREYED OUT, IN ONE PLACE, ANSWERED THE SAME WAY BY setSettings, THE EDITOR UI, THE
// CONSOLE AND A MANIFEST-CONTRADICTION WARNING. Before this header existed, "is ReSTIR GI available"
// was answered up to four times: once by a clamp inside Renderer::setSettings (silently, by force),
// once by whatever a UI BeginDisabled block happened to check, once by a console var's own validate(),
// and never at all by anything that could warn a project's manifest had asked for something the
// device or the current tier selection could never grant. Four answers to one question drift; this
// header is the one place that question gets answered, as a PURE function of a Settings snapshot and
// a DeviceInfo, so every caller reads the same reason in the same words.
//
// PURE AND HEADER-ONLY, DELIBERATELY, for sandbox/src/PtRenderConflict.hpp's exact reason (see that
// header's own top comment): no ImGui types, no aver::voxi::Renderer::get(), no AVER_WARN call, no
// globals -- plain Settings/DeviceInfo values in, plain data out. That is what makes every decision
// below a headless unit test (tests/render.voxi/src/RenderSettingsResolverTest.cpp) in a codebase
// where almost nothing about the renderer or the editor can be tested without a GPU. Every ImGui call,
// every AVER_WARN, and every read of the live Renderer singleton stays at its call site; only the
// DECISION moves here.
//
// resolve() ONLY ANSWERS PREREQUISITES FOR SETTINGS THAT Renderer::setSettings HAS ALREADY PRODUCED.
// The device-level clamps (globalIllumination/rayTracing/pathTracing/meshShaders forced to Off/false
// when the device cannot run them, Voxi.cpp) happen before anything in this header ever runs, and this
// header does not re-derive them -- Quality::rayTracing arriving here as Quality::Off already means
// EITHER the device refused it or the project asked for Off; this header does not need to know which,
// because both leave the same fields Off. What resolve() adds on top is the finer-grained question
// setSettings never answered before: given a tier that IS legal, is this specific NON-default value
// (ReSTIR GI, ray-driven refraction, the denoiser) itself legal right now.

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
    RequiresNrd,                 // DeviceInfo::nrdSupported is false: not D3D12, or NRD not built in
    NothingToDenoise,            // hardware and tiers are fine, but nothing is producing a signal NRD
                                 // could filter (VoxiRenderer's NRD-instance create gate)
    RequiresMsaaOne,              // SOFT: NRD needs single-sample targets. Warns; does not grey.
    NotImplemented,               // the engine itself has not built this yet, on any device
    RequiresRestirGi,             // U1: giRestirVisibility only applies once giMode itself resolves
                                   // to ReSTIR -- Resolution::giRestirVisibility's own gate
    Count
};

// Returns whether a feature is usable on this device -- the same answer Renderer::status(f) gives
// (Renderer::status is now a one-line forward to this, Voxi.cpp), lifted into a free function so it
// can be asked about a DeviceInfo that is not (yet, or ever) the live Renderer singleton's own: a
// manifest apply resolving what a project asked for before it is committed, or a unit test's
// hand-built device.
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
            // MIRRORS aver::pt::PathTracer::init()'s own gate field for field (kRayTracingTier=11,
            // kShaderModel=65, caps.dxcAvailable, caps.computeShaders -- see PathTracer.cpp): this IS
            // the capability check for modules/render.pt's reference view (PtSceneView), the only path
            // tracer this engine has ever built. This used to read `return Status::NotImplemented;`
            // unconditionally -- true the day this enum was declared, and left true long after
            // SandboxApp grew a real PtSceneView, so the Path Tracing settings-page combo that reads
            // this status stayed permanently grey and Renderer::setSettings clamped whatever
            // Settings::pathTracing held back to Off, on every device, forever, regardless of
            // hardware -- a persisted, C#-scriptable setting with no relationship whatsoever to the
            // real path tracer. See SandboxApp.cpp's buildRenderingSettings(page==4), the only reader
            // of a Ready status here, for what actually reconciles this against PtSceneView now.
            if (d.rayTracingTier < 11 || d.shaderModel < 65 || !d.dxcAvailable || !d.computeShaders)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::MeshShaders:
            if (d.meshShaderTier == 0 || d.shaderModel < 65 || !d.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::LayeredBsdf:
            // Ready as of the commit that added averCoatTerms and wired AVER_LAYERED_BSDF into the
            // material define string. Before that this returned NotImplemented and the clamp in
            // Renderer::setSettings pinned the setting to Off -- deliberately, so a combo could not
            // change a persisted value and alter nothing.
            //
            // NO DEVICE GATE, and that is a claim rather than an omission: the coat is arithmetic in a
            // pixel shader built from the same split-sum helpers the base BRDF already uses. It needs
            // no ray tracing, no mesh shaders, no compute, no shader model above what every
            // material-shaded draw already requires. If a future layer needs something the device may
            // not have, THIS is where the check goes.
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
        case DisableReason::RequiresNrd:
            return "Needs the D3D12 backend with NVIDIA's denoiser library built in.";
        case DisableReason::NothingToDenoise:
            return "Nothing is producing a signal for the denoiser to filter yet.";
        case DisableReason::RequiresMsaaOne:
            return "NRD needs single-sample render targets; it skips itself above 1x MSAA.";
        case DisableReason::NotImplemented:
            return "This engine does not implement it yet, on any device.";
        case DisableReason::RequiresRestirGi:
            return "Only applies when Indirect diffuse is ReSTIR.";
        default:
            return "Unavailable.";
    }
}

// Whether a reason should GREY the control (hide the choice entirely) rather than merely warn beside
// it. Exactly one reason is soft today: MSAA above 1x is a condition the same page's own MSAA radios
// can undo in one click, so the denoiser checkbox stays clickable and reachable with a warning line
// next to it, per the prerequisite table's own "soft reasons only warn inline" rule.
inline bool greysControl(DisableReason r) {
    return r != DisableReason::None && r != DisableReason::RequiresMsaaOne;
}

// One field's resolved state: what was asked for, what is actually in effect, and why they differ
// (None when they don't).
struct FieldResolution {
    u32 requested = 0;
    u32 effective = 0;
    DisableReason reason = DisableReason::None;
};

// The full prerequisite resolution for one Settings/DeviceInfo pair. globalIllumination/rayTracing/
// pathTracing are copied through as-is (setSettings has already hardware-clamped them); everything
// else answers a finer-grained question setSettings did not used to answer at all.
struct Resolution {
    Quality globalIllumination, rayTracing, pathTracing;
    FieldResolution giMode, rtRenderMode, refractionMode, denoiser;
    // U1: gated on giMode's OWN resolution, not on a fresh hardware/tier check -- see resolve()'s own
    // comment on this field for why effective == requested always, unlike every FieldResolution above.
    FieldResolution giRestirVisibility;
    // giRestirReuse is NOT tier-derived (Settings::giRestirReuse's own comment) and asks no prerequisite
    // of its own -- this entry exists only so the Settings page can grey its combo on the identical
    // condition giRestirVisibility's does (both are inert without ReSTIR GI running), computed the
    // same way for the same reason: see resolve()'s own comment on this field, right beside
    // giRestirVisibility's.
    FieldResolution giRestirReuse;
    DisableReason rtSubControls = DisableReason::None;
    DisableReason ptSubControls = DisableReason::None;
    bool denoiserGBufferWanted = false;
};

// Resolves every prerequisite-gated field in `s` against `d`. Reasons are computed WHETHER OR NOT the
// option is selected -- an unselected ReSTIR entry greys exactly the same as a selected one that lost
// its prerequisite, so a UI never has to special-case "off, but also would be forced off anyway".
//
// General rule: effective = requested && reason == None. refractionMode is the one exception, because
// its fallback is not "off" but one rung down: requested >= 2 && reason != None resolves to 1
// (ScreenSpace), never to 0 -- a request below RayTraced never needed a ray tracer to begin with and
// is passed through untouched.
//
// denoiser's NothingToDenoise reads THIS resolution's own giMode.effective, not the raw request, which
// is why giMode is resolved before denoiser below: a ReSTIR request a hardware or tier gate has
// already turned back is not a signal the denoiser could filter either.
inline Resolution resolve(const Settings& s, const DeviceInfo& d) {
    Resolution r{};
    r.globalIllumination = s.globalIllumination;
    r.rayTracing         = s.rayTracing;
    r.pathTracing         = s.pathTracing;

    const bool rtHardware = featureStatus(Feature::RayTracing, d) == Status::Ready;

    // Shared by every RT-gated field below: hardware first, then whether the RT TIER itself is Off.
    // Checked in that order because a device that fails the hardware gate ALSO has rayTracing forced
    // to Off by Renderer::setSettings -- so hardware-absent and tier-set-to-Off would otherwise be
    // indistinguishable from here, and the two need different messages: RequiresRayTracingHardware
    // names a GPU limit nothing on this device can fix; RequiresRayTracingEnabled names a choice the
    // project itself can undo in one click.
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
    // Not a fresh hardware/tier check of its own: giMode just above already ran that exact check, and
    // this field cannot be MORE available than the estimator it modifies. Its reason is giMode's own
    // reason when giMode has one (the RT/GI prerequisite that also blocks giMode blocks this); otherwise
    // RequiresRestirGi when the project simply has not turned ReSTIR on (s.giMode == 0), which is not a
    // prerequisite failure but is still a reason this control should read as inert.
    r.giRestirVisibility.requested = s.giRestirVisibility > 3u ? 3u : s.giRestirVisibility;
    r.giRestirVisibility.reason = (r.giMode.reason != DisableReason::None)
                                       ? r.giMode.reason
                                       : (s.giMode == 0 ? DisableReason::RequiresRestirGi : DisableReason::None);
    // effective == requested ALWAYS -- deliberately unlike giMode/rtRenderMode/denoiser above. Clamping to 0 on a
    // failed prerequisite would read "No ray (over-bright)" while no ReSTIR runs at all; inertness is carried by
    // `reason` alone. Do not "fix" this to match the file's general rule (the comment at :169).
    r.giRestirVisibility.effective = r.giRestirVisibility.requested;

    // ---- giRestirReuse: identical gating to giRestirVisibility just above, same reason ----
    // Not a fresh prerequisite of its own -- it is inert on exactly the same condition (ReSTIR GI
    // itself not running), so it reuses giMode's resolution the same way giRestirVisibility does, and
    // effective == requested always for the identical reason: forcing it to 0 (Adaptive) on a failed
    // prerequisite would read as a real choice rather than as "nothing is running to apply this to".
    r.giRestirReuse.requested = s.giRestirReuse > 2u ? 2u : s.giRestirReuse;
    r.giRestirReuse.reason = (r.giMode.reason != DisableReason::None)
                                  ? r.giMode.reason
                                  : (s.giMode == 0 ? DisableReason::RequiresRestirGi : DisableReason::None);
    r.giRestirReuse.effective = r.giRestirReuse.requested;

    // ---- rtRenderMode = 1 (ray-driven primary visibility): RT hardware, RT tier not Off ----
    r.rtRenderMode.requested = s.rtRenderMode;
    r.rtRenderMode.reason    = rtGate;
    r.rtRenderMode.effective = (s.rtRenderMode != 0 && rtGate == DisableReason::None) ? 1u : 0u;

    // ---- refractionMode = 2 (RayTraced): RT hardware, RT tier not Off ----
    // The one field whose fallback is not "off": a request at or above RayTraced that cannot be
    // granted resolves to ScreenSpace (1), the nearly-free approximation, not to 0 -- see
    // ladder::refraction's own comment for why Off is reserved for globalIllumination-tier-off, not
    // for "asked for RayTraced, got refused". A request already below RayTraced (0 or 1) never needed
    // a ray tracer and passes straight through regardless of rtGate.
    r.refractionMode.requested = s.refractionMode;
    r.refractionMode.reason    = rtGate;
    r.refractionMode.effective =
        (s.refractionMode >= 2u && rtGate != DisableReason::None) ? 1u : s.refractionMode;

    // ---- denoiser: RT hardware, RT tier not Off, nrdSupported, something to denoise, MSAA 1 (soft) --
    DisableReason denoiseReason = rtGate;
    if (denoiseReason == DisableReason::None && !d.nrdSupported)
        denoiseReason = DisableReason::RequiresNrd;
    if (denoiseReason == DisableReason::None && r.giMode.effective == 0 && s.giSkyOcclusionRays == 0)
        denoiseReason = DisableReason::NothingToDenoise;
    if (denoiseReason == DisableReason::None && static_cast<u32>(s.msaa) != 1u)
        denoiseReason = DisableReason::RequiresMsaaOne;   // SOFT: warns, does not grey
    r.denoiser.requested = s.denoiser ? 1u : 0u;
    r.denoiser.reason    = denoiseReason;
    r.denoiser.effective = (s.denoiser && denoiseReason == DisableReason::None) ? 1u : 0u;

    // Whether the ~54 MB G-buffer should be allocated at all: wanted when the ONLY thing standing
    // between "requested" and "running" is the soft MSAA reason (which the same session can clear by
    // changing the MSAA radios) or nothing at all -- so the targets are already there the moment MSAA
    // comes back to 1x, rather than allocating on one frame's delay after the fact.
    r.denoiserGBufferWanted =
        s.denoiser && (denoiseReason == DisableReason::None || denoiseReason == DisableReason::RequiresMsaaOne);

    // ---- the RT page's rows below its Quality combo, and the PT page's rows below its own ----
    r.rtSubControls = rtGate;
    // ptBounces and the rest of the Path Tracing page's sub-rows: this project's existing UI already
    // greys them on one condition, "the tier is Off" (SandboxApp.cpp), with no separate hardware-vs-
    // choice split the way RT's two grey regions need -- a hardware refusal has already forced
    // pathTracing to Off by the time anything reads it (Renderer::setSettings), so there is exactly
    // one observable state here, not two. RequiresPathTracingHardware is reused as that single label;
    // nothing downstream calls refusalFeatureFor on ptSubControls the way manifestContradictions does
    // on rtGate-derived reasons, so the reuse costs nothing.
    r.ptSubControls =
        (s.pathTracing == Quality::Off) ? DisableReason::RequiresPathTracingHardware : DisableReason::None;

    return r;
}

// One manifest-stated value per field this resolver watches, or -1 when the manifest did not state it
// -- mirrors fmt::ProjectDesc's own "-1 means absent" convention for these ints (OcProject.hpp) so a
// caller can pass the raw manifest fields straight through without translating the sentinel.
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

// Reports only the fields the MANIFEST actually stated (asks.X >= 0) whose EFFECTIVE value -- resolved
// fresh against `effective`/`d`, not the raw stored field -- differs from what was asked. `effective`
// is the Settings the loader actually committed (after flag precedence, hardware clamping and this
// same resolve() step Renderer::setSettings itself already ran); calling resolve() again here is
// cheap and keeps this function correct even if a future caller hands it settings that were never
// run through the singleton at all (a dry-run preview, say). Returns the count written into `out`
// (capacity 4, one slot per watched field).
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

// Maps a DisableReason back to the Feature whose EXISTING refuse() call (Renderer::setSettings,
// Voxi.cpp) already logs it at load time -- so a caller reporting manifest contradictions can skip a
// reason refuse() has already told the log about, rather than saying the same device limitation twice
// from two different call sites (Renderer::refusalLogged(f) is the skip check; see its own comment,
// Voxi.hpp). Returns false for a reason nothing already logs (RequiresRayTracingEnabled,
// RequiresGlobalIllumination, NothingToDenoise, RequiresNrd, RequiresMsaaOne, RequiresRestirGi, and the
// synthetic ptSubControls reuse of RequiresPathTracingHardware never reaches here because callers only
// feed this the four hardware reasons and NotImplemented) -- those get reported fresh by the caller
// instead.
inline bool refusalFeatureFor(DisableReason r, Feature& out) {
    switch (r) {
        case DisableReason::RequiresComputeShaders:      out = Feature::GlobalIllumination; return true;
        case DisableReason::RequiresRayTracingHardware:  out = Feature::RayTracing;          return true;
        case DisableReason::RequiresPathTracingHardware: out = Feature::PathTracing;         return true;
        case DisableReason::RequiresMeshShaderHardware:  out = Feature::MeshShaders;         return true;
        // No feature returns NotImplemented today (LayeredBsdf is Ready, Voxi.cpp) -- LayeredBsdf is
        // the one feature this project's history has actually used this status for, so it is the
        // answer here too, kept for whichever feature next needs an unfinished rung.
        case DisableReason::NotImplemented:              out = Feature::LayeredBsdf;         return true;
        default: return false;
    }
}

} // namespace aver::voxi
