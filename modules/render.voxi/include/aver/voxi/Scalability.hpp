// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"

// THE UE-STYLE "OVERALL QUALITY" PRESET: one button that moves every group this project has a ladder
// for to the same rung at once, and the machinery to tell whether a project's current settings still
// read as one of those rungs or have drifted into something a preset never produced (Custom).
//
// PURE AND HEADER-ONLY, same reason as RenderSettingsResolver.hpp: no ImGui, no Renderer::get(), no
// AVER_WARN. applyOverall MUTATES the Settings it is handed, but only that Settings -- there is no
// hidden global it reaches into, so the caller (the Overall row's button handler, or the console's
// voxi.scalability var) decides when and whether the result is actually committed through
// Renderer::setSettings.
//
// ONLY THREE GROUPS HAVE A RUNG FOR OverallQuality/Custom DETECTION: Global Illumination, Ray Tracing,
// Path Tracing (PT always Off, see scalabilityRung below -- a locked user decision, not an oversight).
// MSAA, shadows and post are deliberately NOT here: MSAA is a prerequisite the denoiser and the
// ray-driven path both read rather than a quality level of its own, shadow cascades are compile-time
// constants, and post-processing lives in editor preferences, not project settings. See the
// settings-separation plan's own section 3 ("Groups excluded, and why") for the full list this header
// does not repeat.
//
// AVERSR HAS A PER-RUNG DEFAULT TOO (ladder::averSrLevel, overallAverSrLevel and autoAverSrLevel
// below), but it deliberately sits OUTSIDE both voxi::Settings and this file's Custom detection. An
// earlier revision of this comment called AverSR "a per-user display preference the game does not even
// have," which stopped being true once the packaged game gained its own --aversr / RENDER.AVERSR chain
// (this project's optimisation-wave-2 plan, section 3.3 C) -- but the MODULE-BOUNDARY reason for
// keeping it out of groupFollowsLadder/overallFromSettings still holds regardless of who has one:
// render.voxi must never include render.sr, so the level this ladder computes can be READ by a host
// that links render.sr, never CONSULTED by this header to decide GI/RT/PT scalability. The consequence,
// stated rather than left for someone to discover by reading two files at once: "Overall: Epic" can
// still hide an AverSR level pinned away from Epic's own default, because Custom detection cannot see
// it. Where that shows is the Project Settings upscaling line (plan section 3.3 A), not here -- it
// appends "(differs from the <rung> preset's default, <level>)" whenever the resolved level and this
// ladder's rung default disagree.
namespace aver::voxi {

// Custom means "this project's settings do not read as any single rung" -- the state overallFromSettings
// returns when a project is a mix, not a level anything can be SET to. Low..Epic mirror Quality's own
// numbering (1..4) exactly, by construction, so scalabilityRung below is a cast rather than a table.
enum class OverallQuality : u32 { Custom = 0, Low = 1, Medium = 2, High = 3, Epic = 4 };

// The three ladders an Overall preset moves together. Count is not a real group; it sizes a loop and a
// bitmask (applyOverall's return value packs one bit per group, `1u << static_cast<u32>(group)`).
enum class ScalabilityGroup : u32 { GlobalIllumination, RayTracing, PathTracing, Count };

// The three tiers one Overall rung asks for.
struct ScalabilityRung {
    Quality globalIllumination;
    Quality rayTracing;
    Quality pathTracing;
};

// {q, q, Off} -- PT IS OFF AT EVERY RUNG. Locked user decision (settings-separation plan, section 11):
// picking an Overall preset never turns Path Tracing on, and there is deliberately no Cinematic rung
// above Epic that would change that. A PT tier above Off turns on PtSceneView, which takes over the
// entire raster/ray-driven view (see PtSceneView.hpp's own "render mode" note) -- too large a side
// effect for a single quality button to spring on a project silently.
constexpr ScalabilityRung scalabilityRung(OverallQuality q) {
    const Quality t = static_cast<Quality>(static_cast<u32>(q));   // Low=1..Epic=4 line up by
                                                                     // construction; Custom=0 maps to
                                                                     // Quality::Off and is never fed to
                                                                     // applyOverall in practice (the
                                                                     // console's own var validates
                                                                     // 1..4 before this ever runs).
    return ScalabilityRung{t, t, Quality::Off};
}

// Whether a group's tier can be set at all on this device -- Ready per featureStatus(), the same gate
// Renderer::setSettings itself already clamps the raw tier field against.
inline bool groupAvailable(ScalabilityGroup g, const DeviceInfo& d) {
    switch (g) {
        case ScalabilityGroup::GlobalIllumination:
            return featureStatus(Feature::GlobalIllumination, d) == Status::Ready;
        case ScalabilityGroup::RayTracing:
            return featureStatus(Feature::RayTracing, d) == Status::Ready;
        case ScalabilityGroup::PathTracing:
            return featureStatus(Feature::PathTracing, d) == Status::Ready;
        default: return false;
    }
}

// The tier a group is currently at, read out of `s`.
inline Quality groupTier(const Settings& s, ScalabilityGroup g) {
    switch (g) {
        case ScalabilityGroup::GlobalIllumination: return s.globalIllumination;
        case ScalabilityGroup::RayTracing:         return s.rayTracing;
        case ScalabilityGroup::PathTracing:        return s.pathTracing;
        default:                                    return Quality::Off;
    }
}

// Whether every DERIVED knob belonging to this group -- keyed (has a manifest key) or not (the two sky
// knobs) -- currently equals the ladder's value for the group's own tier. False the moment even one
// knob has been hand-edited away from its tier's rung, which is what lets the UI show "(modified)"
// next to a group that is nominally at the right tier but is not actually following it.
inline bool groupFollowsLadder(const Settings& s, ScalabilityGroup g) {
    switch (g) {
        case ScalabilityGroup::GlobalIllumination: {
            const Quality t = s.globalIllumination;
            return s.voxelResolution   == ladder::voxelResolution(t) &&
                   s.giCones           == ladder::giCones(t) &&
                   s.giUpdateInterval  == ladder::giUpdateInterval(t) &&
                   s.giRestirVisibility == ladder::giRestirVisibility(t);
        }
        case ScalabilityGroup::RayTracing: {
            const Quality t = s.rayTracing;
            return s.rtShadowRays       == ladder::rtShadowRays(t) &&
                   s.rtPixelsPerRayTile == ladder::rtPixelsPerRayTile(t) &&
                   s.rtShadowDenoise    == ladder::rtShadowDenoise(t) &&
                   s.rtRenderMode       == ladder::rtRenderMode(t) &&
                   s.refractionMode     == ladder::refraction(t) &&
                   s.giSkyOcclusionRays == ladder::giSkyOcclusionRays(t) &&
                   s.giSkyOcclusionTile == ladder::giSkyOcclusionTile(t);
        }
        case ScalabilityGroup::PathTracing:
            return s.ptBounces == ladder::ptBounces(s.pathTracing);
        default: return true;
    }
}

// Moves every AVAILABLE group to rung q's tier and writes every one of that group's derived knobs --
// keyed or not, sky knobs included -- to the ladder's value for that tier. An UNAVAILABLE group (this
// device cannot run it at all) is left completely untouched: its tier, its knobs, everything, exactly
// as captureVoxiSettings' own write rule needs (ProjectRenderApply.hpp, Lane 2) to keep a manifest's
// RT keys intact for a teammate opening the same project on hardware without ray tracing. Returns a
// bitmask of which groups were actually written, `1u << static_cast<u32>(group)` per bit -- a caller
// commits this alongside the mutated Settings so a later manifest-capture step knows which fields to
// treat as "follow the tier" rather than "the user's explicit pin".
inline u32 applyOverall(Settings& s, OverallQuality q, const DeviceInfo& d) {
    const ScalabilityRung rung = scalabilityRung(q);
    u32 mask = 0;

    if (groupAvailable(ScalabilityGroup::GlobalIllumination, d)) {
        s.globalIllumination  = rung.globalIllumination;
        s.voxelResolution     = ladder::voxelResolution(rung.globalIllumination);
        s.giCones             = ladder::giCones(rung.globalIllumination);
        s.giUpdateInterval    = ladder::giUpdateInterval(rung.globalIllumination);
        s.giRestirVisibility  = ladder::giRestirVisibility(rung.globalIllumination);
        mask |= 1u << static_cast<u32>(ScalabilityGroup::GlobalIllumination);
    }
    if (groupAvailable(ScalabilityGroup::RayTracing, d)) {
        s.rayTracing         = rung.rayTracing;
        s.rtShadowRays       = ladder::rtShadowRays(rung.rayTracing);
        s.rtPixelsPerRayTile = ladder::rtPixelsPerRayTile(rung.rayTracing);
        s.rtShadowDenoise    = ladder::rtShadowDenoise(rung.rayTracing);
        s.rtRenderMode       = ladder::rtRenderMode(rung.rayTracing);
        s.refractionMode     = ladder::refraction(rung.rayTracing);
        s.giSkyOcclusionRays = ladder::giSkyOcclusionRays(rung.rayTracing);
        s.giSkyOcclusionTile = ladder::giSkyOcclusionTile(rung.rayTracing);
        mask |= 1u << static_cast<u32>(ScalabilityGroup::RayTracing);
    }
    if (groupAvailable(ScalabilityGroup::PathTracing, d)) {
        s.pathTracing = rung.pathTracing;   // always Off -- see scalabilityRung's own comment
        s.ptBounces   = ladder::ptBounces(rung.pathTracing);
        mask |= 1u << static_cast<u32>(ScalabilityGroup::PathTracing);
    }
    return mask;
}

// Reads a project's settings back into an Overall rung, or Custom if they do not agree with one.
// UNAVAILABLE groups are ignored entirely -- exactly mirroring applyOverall's own "leave it untouched"
// rule, so a teammate without ray-tracing hardware still reads a project as "Medium" instead of
// permanently "Custom" for a group their machine could never have set in the first place. Path
// Tracing is a special case even among available groups: since every Overall rung is PT Off (locked
// decision), a PT tier above Off always reads as Custom regardless of what GI and RT are doing, and PT
// being Off does not by itself tell you WHICH rung GI/RT are following -- it agrees with all of them
// equally, so it never participates in choosing the rung, only in vetoing it.
inline OverallQuality overallFromSettings(const Settings& s, const DeviceInfo& d) {
    OverallQuality result = OverallQuality::Custom;
    bool haveRung = false;

    for (u32 i = 0; i < static_cast<u32>(ScalabilityGroup::Count); ++i) {
        const auto g = static_cast<ScalabilityGroup>(i);
        if (!groupAvailable(g, d)) continue;

        const Quality tier = groupTier(s, g);
        if (g == ScalabilityGroup::PathTracing) {
            if (tier != Quality::Off) return OverallQuality::Custom;
            continue;   // Off agrees with every rung equally; it never selects one
        }
        if (!groupFollowsLadder(s, g)) return OverallQuality::Custom;

        const auto asOverall = static_cast<OverallQuality>(static_cast<u32>(tier));
        if (!haveRung) { result = asOverall; haveRung = true; }
        else if (result != asOverall) return OverallQuality::Custom;
    }

    // No available group could be checked at all (a device with neither compute nor ray-tracing
    // hardware) -- there is nothing here to call a rung, so this reads as Custom rather than guessing.
    return haveRung ? result : OverallQuality::Custom;
}

// ================================================================= AverSR (U2) =====================
//
// THE OTHER HALF OF THE MODULE-BOUNDARY STATEMENT ABOVE: these four names are what a host (the editor,
// the packaged game) actually calls to find AverSR's default and to fold it together with the CLI, a
// user's own Display choice and a project manifest -- none of which this header reads itself. Every
// function here is pure (Settings/DeviceInfo/plain ints in, a plain struct out), same reason as the
// rest of this file and RenderSettingsResolver.hpp: no ImGui, no Renderer::get(), no AVER_WARN, and
// therefore no need for a GPU to unit-test any of it.
//
// PLACED AFTER overallFromSettings, NOT "below applyOverall" WHERE THIS PROJECT'S PLAN SAID TO PUT IT:
// autoAverSrLevel below calls overallFromSettings, and this is an ordinary (non-template) header-only
// function, not a class member or a template -- C++ has no two-phase lookup here, so the callee must
// already be declared above the call site or the build fails. Moved down here, past its dependency,
// with the same four signatures the plan named; nothing about the CONTRACT moved, only where the text
// sits in the file.

// ladder::averSrLevel expects a Quality; OverallQuality's own numbering lines up with Quality's by
// construction (see scalabilityRung above), so this is a cast, not a table -- Custom (0) casts to
// Quality::Off and reads the same "nothing left to buy back" level averSrLevel(Off) already returns.
constexpr u32 overallAverSrLevel(OverallQuality q) {
    return ladder::averSrLevel(static_cast<Quality>(static_cast<u32>(q)));
}

// AverSR's AUTOMATIC level for a project's live settings, when nothing (CLI, user choice, manifest) has
// pinned one explicitly -- the "auto" input to resolveAverSrLevel below. Reads as one Overall rung
// whenever the project's GI/RT/PT settings still agree with one (the common case: a project untouched
// since its last Overall pick, or one that never drifted); Custom -- the settings do NOT read as any
// single rung -- has no Overall level to ask ladder::averSrLevel for, so this takes the ladder's value
// at the HIGHER of the GI and RT tiers instead. Higher, not lower, because the more expensive of the
// two features actually running is the one an upscale trade matters most for, and it is the same tier
// every other ladder:: function in this file that keys off "the RT tier" or "the GI tier" individually
// already treats as authoritative for its own knob -- there is no third, blended answer anywhere else
// in this ladder to borrow instead. s.globalIllumination/s.rayTracing are already hardware-clamped by
// Renderer::setSettings (Voxi.cpp) by the time anything calls this, so a device missing one of the two
// features needs no separate check here: that tier already reads Off (0), the lowest possible value,
// and cannot win the max().
inline u32 autoAverSrLevel(const Settings& s, const DeviceInfo& d) {
    const OverallQuality rung = overallFromSettings(s, d);
    if (rung != OverallQuality::Custom) return overallAverSrLevel(rung);
    const u32 giTier = static_cast<u32>(s.globalIllumination);
    const u32 rtTier = static_cast<u32>(s.rayTracing);
    const Quality higher = static_cast<Quality>(giTier > rtTier ? giTier : rtTier);
    return ladder::averSrLevel(higher);
}

// Where a resolved AverSR level came from, for the "(source)" half of the mandatory startup log line
// and the Project Settings upscaling row (plan section 3.3 A). ForcedOff is never produced by
// resolveAverSrLevel below -- it is for a host to report directly when it overrides the resolved level
// with Off on its own account (the --edge-aa upscaler-slot conflict; a crash cookie tripped by a failed
// launch) rather than something this pure function could ever decide by itself.
enum class AverSrSource : u32 { Auto, Manifest, User, Cli, ForcedOff };

// One resolved AverSR level and why it won.
struct AverSrDecision {
    u32 level = 0;
    AverSrSource source = AverSrSource::Auto;
};

// THE PRECEDENCE CHAIN ITSELF (U2): CLI > the user's own Display choice > the project manifest > Auto
// (autoAverSrLevel above). Each of cliLevel/userLevel/manifestLevel is -1 when that source did not
// state a level at all -- mirrors ManifestAsks' own "-1 means absent" convention
// (RenderSettingsResolver.hpp) so a host can pass its raw fields straight through without translating
// the sentinel a second time. autoLevel is never absent: autoAverSrLevel above always has an answer, so
// there is nothing for a caller to omit there.
//
// CLAMPED TO 0..3 REGARDLESS OF WHICH SOURCE WINS, including autoLevel -- a garbage value from any one
// of them (a manifest typo, a future CLI parse bug) must not read past kAverSrPerformance into whatever
// an AverSrSource-keyed table happens to have at that index next.
inline AverSrDecision resolveAverSrLevel(int cliLevel, int userLevel, int manifestLevel, u32 autoLevel) {
    auto clamp3 = [](int v) -> u32 { return static_cast<u32>(v < 0 ? 0 : (v > 3 ? 3 : v)); };
    if (cliLevel >= 0)      return AverSrDecision{clamp3(cliLevel), AverSrSource::Cli};
    if (userLevel >= 0)     return AverSrDecision{clamp3(userLevel), AverSrSource::User};
    if (manifestLevel >= 0) return AverSrDecision{clamp3(manifestLevel), AverSrSource::Manifest};
    return AverSrDecision{autoLevel > 3u ? 3u : autoLevel, AverSrSource::Auto};
}

} // namespace aver::voxi
