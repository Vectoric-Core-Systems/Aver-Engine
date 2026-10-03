// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// THE QUALITY LADDER: every Off..Epic rung Renderer::setSettings derives a knob from, one function per
// knob. Keeping switch and rationale together avoids the "torn pair" bug where an edit orphaned a
// comment from the function it justified. Voxi.cpp's Renderer::XForQuality bodies are one-line forwards.
namespace aver::voxi::ladder {

// ================================================================= Global Illumination ===========

// VOXEL GRID EDGE per GI tier. Doubling the edge is 8x memory/cost. Medium keeps the long-standing
// default (128); Off/Low share (64): ~6 MB; High (256): ~400 MB; Epic (512): ~3.2 GB.
constexpr u32 voxelResolution(Quality q) {
    switch (q) {
        case Quality::Off:
        case Quality::Low:    return 64;
        case Quality::Medium: return 128;
        case Quality::High:   return 256;
        case Quality::Epic:   return 512;
        default:              return 128;
    }
}

// PER-PIXEL DIFFUSE GATHER CONE COUNT -- the GI knob that costs real frame time. The default tier
// (Medium) must match the struct default for derivation to fire on tier changes only.
constexpr u32 giCones(Quality q) {
    switch (q) {
        case Quality::Off:    return 6;   // inert: nothing gathers with GI off
        case Quality::Low:    return 3;
        case Quality::Medium: return 6;
        case Quality::High:   return 9;
        case Quality::Epic:   return 13;
        default:              return 6;
    }
}

// FRAMES BETWEEN GI RE-VOXELISATIONS. 1 = every frame; N>1 reuses previous voxelise+filter, trading
// indirect-light latency for amortised cost. Struct default must match Medium's rung below.
constexpr u32 giUpdateInterval(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // GI is not running; the value is inert either way
        case Quality::Low:    return 4;
        case Quality::Medium: return 2;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;   // always fresh
        default:              return 1;   // an unknown tier must not silently degrade lighting
    }
}

// RESTIR GI VISIBILITY: how much of per-pixel rays (F2/F3) each rung pays for. See
// Settings::giRestirVisibility (Voxi.hpp) for what each mode changes in the shader. Struct default
// (HalfResolution) must match Medium's rung below. High/Epic (Full) are the owner's decision.
constexpr u32 giRestirVisibility(Quality q) {
    switch (q) {
        case Quality::Off:    return 3;   // inert: ReSTIR GI is not running
        case Quality::Low:    return 1;   // Reconstructed
        case Quality::Medium: return 2;   // HalfResolution
        case Quality::High:   return 3;   // Full
        case Quality::Epic:   return 3;   // Full
        default:              return 3;   // must not silently under-correct the contrast fix
    }
}

// ================================================================= Ray Tracing ====================

// WHICH REFRACTION MODE THE TIER ASKS FOR. RayTraced only at top rungs (costs a ray per translucent
// pixel); ScreenSpace is nearly free. Off at Quality::Off keeps the "tier off means feature off"
// contract. Struct default must match Medium's rung.
constexpr u32 refraction(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // Off
        case Quality::Low:    return 1;   // ScreenSpace
        case Quality::Medium: return 1;   // ScreenSpace  <- Settings::refractionMode's default
        case Quality::High:   return 2;   // RayTraced
        case Quality::Epic:   return 2;   // RayTraced
        default:              return 1;
    }
}

// OCCLUSION RAYS PER PIXEL for the ray-traced sun shadow. Clamped to [1, VoxiRenderer::kMaxShadowRays].
// Struct default must match Medium's rung (Settings::rtShadowRays, Voxi.hpp).
constexpr u32 rtShadowRays(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // RT is not running; the value is inert either way
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 4;
        case Quality::Epic:   return 8;
        default:              return 1;   // an unknown tier must not silently cost more
    }
}

// EDGE LENGTH of the square tile one traced pixel is amortised over in ray-traced shadows: 1 = every
// pixel traces every frame; N>1 = one pixel per NxN tile traces, rotating. Always 1 at every rung.
constexpr u32 rtPixelsPerRayTile(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier must not silently reintroduce the history
    }
}

// SPATIAL DENOISE RADIUS, in pixels, for ray-traced shadows. 0 = unfiltered; N>0 averages a
// (2N+1)^2 neighbourhood weighted by depth. See Settings::rtShadowDenoise (Voxi.hpp).
constexpr u32 rtShadowDenoise(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // RT is not running; the filter has nothing to filter
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently start smearing shadows
    }
}

// WHICH THING FINDS THE FIRST SURFACE: 0 = rasteriser, 1 = a primary ray per pixel. See
// Settings::rtRenderMode (Voxi.hpp). 1 from Medium up; 0 at Off (no acceleration structure) and Low.
constexpr u32 rtRenderMode(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // no acceleration structure to trace against
        case Quality::Low:    return 0;   // deliberately excluded from the ray-driven default
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // unknown tier keeps the pre-existing rasteriser path
    }
}

// SKY-VISIBILITY RAYS the AMBIENT term traces per pixel. 0 = estimate from cone gather (Low only).
// See Settings::giSkyOcclusionRays (Voxi.hpp). High and Epic trace one ray; others fall back to gather.
constexpr u32 giSkyOcclusionRays(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // no acceleration structure; shader falls back to cone gather
        case Quality::Low:    return 0;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently cost more
    }
}

// EDGE, IN PIXELS, of the square sharing one sky-occlusion ray direction. Always 1 at every rung;
// inert with single-ray gather. See Settings::giSkyOcclusionTile (Voxi.hpp).
constexpr u32 giSkyOcclusionTile(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier gets the un-correlated, un-amortised path
    }
}

// ================================================================= Path Tracing ===================

// PATH-TRACING BOUNCE BUDGET. Off = 1 (one hit, direct lighting); higher rungs buy bounces.
// See Settings::ptBounces (Voxi.hpp).
constexpr u32 ptBounces(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // one hit, direct lighting: ray tracing, not path tracing
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 3;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently start bouncing
    }
}

// ================================================================= AverSR ==========================

// AVERSR: the spatial-upscale level each OverallQuality rung defaults to. Not laddered like GI/RT/PT
// above -- AverSR is a PRESENT-SIZE upscale every one of those renders behind (smaller internal
// resolution). See Scalability.hpp's header for why it stays out of voxi::Settings.
//
// kAverSrOff/Quality/Balanced/Performance mirror aver::sr::Quality's numbering (AverSrQuality.hpp:
// Off=0, Quality=1, Balanced=2, Performance=3) byte for byte.
inline constexpr u32 kAverSrOff = 0, kAverSrQuality = 1, kAverSrBalanced = 2, kAverSrPerformance = 3;

// LEVEL EACH RUNG DEFAULTS TO. Off returns native (kAverSrOff): with all tiers off, nothing left to
// buy back. Epic/High take gentlest (Quality); Medium takes Balanced; Low takes Performance (it has
// 4x MSAA, which already carries the AA cost).
constexpr u32 averSrLevel(Quality q) {
    switch (q) {
        case Quality::Off:    return kAverSrOff;           // nothing left to buy back at Off
        case Quality::Low:    return kAverSrPerformance;   // widest -- MSAA already carries the AA cost
        case Quality::Medium: return kAverSrBalanced;
        case Quality::High:   return kAverSrQuality;        // gentlest -- an image-quality rung
        case Quality::Epic:   return kAverSrQuality;        // gentlest -- an image-quality rung
        default:              return kAverSrOff;            // an unknown tier must not silently downscale
    }
}

} // namespace aver::voxi::ladder

namespace aver::voxi {

// TIER-DERIVATION HAZARD, ENFORCED AT COMPILE TIME: Renderer::setSettings derives a knob from its
// tier only on change (Voxi.cpp), so if a struct default and its default tier's ladder rung disagree,
// a freshly constructed Settings{} silently ships the wrong number. These asserts catch it at build
// time. Voxi.cpp includes this header, so every build of Aver.Render.Voxi runs these.
static_assert(Settings{}.globalIllumination == Quality::Medium, "GI's default tier moved");
static_assert(Settings{}.rayTracing         == Quality::Medium, "RT's default tier moved");
static_assert(Settings{}.pathTracing        == Quality::Off,    "PT's default tier moved");

static_assert(Settings{}.voxelResolution  == ladder::voxelResolution(Quality::Medium),
              "voxelResolution's struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giCones          == ladder::giCones(Quality::Medium),
              "giCones' struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giUpdateInterval == ladder::giUpdateInterval(Quality::Medium),
              "giUpdateInterval's struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giRestirVisibility == ladder::giRestirVisibility(Quality::Medium),
              "giRestirVisibility's struct default no longer matches GI Medium's ladder rung "
              "(and no rung may return 4, Cached: it is opt-in only)");

static_assert(Settings{}.rtShadowRays       == ladder::rtShadowRays(Quality::Medium),
              "rtShadowRays' struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtPixelsPerRayTile == ladder::rtPixelsPerRayTile(Quality::Medium),
              "rtPixelsPerRayTile's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtShadowDenoise    == ladder::rtShadowDenoise(Quality::Medium),
              "rtShadowDenoise's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtRenderMode       == ladder::rtRenderMode(Quality::Medium),
              "rtRenderMode's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.refractionMode     == ladder::refraction(Quality::Medium),
              "refractionMode's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.giSkyOcclusionRays == ladder::giSkyOcclusionRays(Quality::Medium),
              "giSkyOcclusionRays' struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.giSkyOcclusionTile == ladder::giSkyOcclusionTile(Quality::Medium),
              "giSkyOcclusionTile's struct default no longer matches RT Medium's ladder rung");

static_assert(Settings{}.ptBounces == ladder::ptBounces(Quality::Off),
              "ptBounces' struct default no longer matches PT Off's ladder rung");

} // namespace aver::voxi
