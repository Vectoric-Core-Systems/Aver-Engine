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
// ONLY THREE GROUPS HAVE A RUNG: Global Illumination, Ray Tracing, Path Tracing (PT always Off, see
// scalabilityRung below -- a locked user decision, not an oversight). MSAA, AverSR, shadows and post
// are deliberately NOT here: MSAA is a prerequisite the denoiser and the ray-driven path both read
// rather than a quality level of its own, AverSR is a per-user display preference the game does not
// even have, shadow cascades are compile-time constants, and post-processing lives in editor
// preferences, not project settings. See the settings-separation plan's own section 3 ("Groups
// excluded, and why") for the full list this header does not repeat.
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
            return s.voxelResolution  == ladder::voxelResolution(t) &&
                   s.giCones          == ladder::giCones(t) &&
                   s.giUpdateInterval == ladder::giUpdateInterval(t);
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
        s.globalIllumination = rung.globalIllumination;
        s.voxelResolution    = ladder::voxelResolution(rung.globalIllumination);
        s.giCones            = ladder::giCones(rung.globalIllumination);
        s.giUpdateInterval   = ladder::giUpdateInterval(rung.globalIllumination);
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

} // namespace aver::voxi
