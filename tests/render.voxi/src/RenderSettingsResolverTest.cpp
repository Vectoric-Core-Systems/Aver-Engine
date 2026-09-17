// RenderSettingsResolverTest -- the quality ladder (QualityLadder.hpp), the prerequisite resolver
// (RenderSettingsResolver.hpp) and the Overall Quality scalability preset (Scalability.hpp), all three
// new in this change, plus a handful of regressions through the live Renderer singleton for the one
// behaviour (F-d: giMode/denoiser stored as requested, resolved at read time) that only the singleton's
// own setSettings() can actually exercise.
//
// Links Aver.Render.Voxi and Aver.Core, unlike this directory's CameraFactorTest (which deliberately
// links only Aver.Core because the header it tests never calls an exported Renderer function): this
// file calls Renderer::get(), Renderer::setDeviceInfo() and the exported Renderer::*ForQuality statics,
// all of which live in the Aver.Render.Voxi shared library, not merely in a header.
//
// check() follows tests/editor/src/PtRenderConflictTest.cpp's own idiom exactly (plain std::printf,
// no AVER_ macros) -- consistent with this file testing three more header-only, dependency-free
// decision points in the same "pure function, no ImGui, no globals" shape PtRenderConflict.hpp set the
// precedent for.
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"
#include "aver/voxi/Scalability.hpp"
#include "aver/voxi/Voxi.hpp"

#include <cstdio>
#include <cstring>

// Both directives are needed: u32/f32 live in aver itself (aver/core/Types.hpp), while Quality,
// Settings, Feature, Renderer, DisableReason and ladder:: live one level down in aver::voxi -- exactly
// the pattern CameraFactorTest.cpp already uses in this same directory for the same reason.
using namespace aver;
using namespace aver::voxi;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

const char* tierName(Quality q) {
    switch (q) {
        case Quality::Off:    return "Off";
        case Quality::Low:    return "Low";
        case Quality::Medium: return "Medium";
        case Quality::High:   return "High";
        case Quality::Epic:   return "Epic";
        default:              return "?";
    }
}

// One row of section 4's ladder tables, checked against ladder::fn directly.
void checkLadder(const char* name, u32 (*fn)(Quality), u32 offV, u32 lowV, u32 medV, u32 highV, u32 epicV) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ladder::%s(Off) == %u", name, offV);
    check(fn(Quality::Off) == offV, buf);
    std::snprintf(buf, sizeof(buf), "ladder::%s(Low) == %u", name, lowV);
    check(fn(Quality::Low) == lowV, buf);
    std::snprintf(buf, sizeof(buf), "ladder::%s(Medium) == %u", name, medV);
    check(fn(Quality::Medium) == medV, buf);
    std::snprintf(buf, sizeof(buf), "ladder::%s(High) == %u", name, highV);
    check(fn(Quality::High) == highV, buf);
    std::snprintf(buf, sizeof(buf), "ladder::%s(Epic) == %u", name, epicV);
    check(fn(Quality::Epic) == epicV, buf);
}

// Every Renderer::XForQuality body is now a one-line forward to ladder::X -- proves the forward
// actually forwards, at every tier, rather than trusting the one-liner by inspection alone.
void checkForwards(const char* name, u32 (*legacy)(Quality), u32 (*ladderFn)(Quality)) {
    for (u32 t = 0; t <= 4; ++t) {
        const Quality q = static_cast<Quality>(t);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Renderer::%sForQuality(%s) == ladder::%s(%s)",
                      name, tierName(q), name, tierName(q));
        check(legacy(q) == ladderFn(q), buf);
    }
}

// A device that can run every Voxi feature this resolver gates: compute, DXR 1.1 + SM 6.5 + DXC (so
// Ray Tracing AND Path Tracing are both Ready), mesh-shader Tier 1, and NRD.
DeviceInfo fullDevice() {
    DeviceInfo d;
    d.msaaMask = 1u | 2u | 4u | 8u;
    d.maxMsaaSamples = 8;
    d.rayTracingTier = 11;
    d.computeShaders = true;
    d.typedUavLoads = true;
    d.conservativeRaster = true;
    d.shaderModel = 65;
    d.meshShaderTier = 1;
    d.dxcAvailable = true;
    d.nrdSupported = true;
    return d;
}

// The same device with ray-tracing hardware removed -- since Path Tracing's own gate (Renderer::status,
// mirrored by featureStatus) shares the RT hardware set, this also makes Path Tracing Unsupported;
// GlobalIllumination and MeshShaders are untouched and stay Ready.
DeviceInfo noRtHardwareDevice() {
    DeviceInfo d = fullDevice();
    d.rayTracingTier = 0;
    return d;
}

} // namespace

int main() {
    std::printf("[INFO ] === quality ladder values (section 4's tables) ===\n");
    checkLadder("voxelResolution",   ladder::voxelResolution,   64, 64, 128, 256, 512);
    checkLadder("giCones",           ladder::giCones,            6,  3,   6,   9,  13);
    checkLadder("giUpdateInterval",  ladder::giUpdateInterval,   1,  4,   2,   1,   1);
    checkLadder("refraction",        ladder::refraction,         0,  1,   1,   2,   2);
    checkLadder("rtShadowRays",      ladder::rtShadowRays,       1,  1,   1,   4,   8);
    checkLadder("rtPixelsPerRayTile",ladder::rtPixelsPerRayTile, 1,  1,   1,   1,   1);
    checkLadder("rtShadowDenoise",   ladder::rtShadowDenoise,    0,  2,   2,   1,   1);
    checkLadder("rtRenderMode",      ladder::rtRenderMode,       0,  0,   1,   1,   1);
    checkLadder("giSkyOcclusionRays",ladder::giSkyOcclusionRays, 0,  0,   0,   1,   1);
    checkLadder("giSkyOcclusionTile",ladder::giSkyOcclusionTile, 1,  1,   1,   1,   1);
    checkLadder("ptBounces",         ladder::ptBounces,          1,  2,   2,   3,   4);
    // U1: {Off 3, Low 1, Medium 2, High 3, Epic 3} -- see ladder::giRestirVisibility's own comment for
    // why High/Epic are your decision (Full) and Low/Medium are mine, UNMEASURED.
    checkLadder("giRestirVisibility",ladder::giRestirVisibility, 3,  1,   2,   3,   3);
    // U2: {Off 0, Low 3, Medium 2, High 1, Epic 1} -- kAverSr* mirror sr::Quality's own numbering.
    checkLadder("averSrLevel",       ladder::averSrLevel, ladder::kAverSrOff, ladder::kAverSrPerformance,
                ladder::kAverSrBalanced, ladder::kAverSrQuality, ladder::kAverSrQuality);

    std::printf("[INFO ] === Renderer::*ForQuality forwards ladder::* at every tier ===\n");
    checkForwards("refraction",         Renderer::refractionForQuality,         ladder::refraction);
    checkForwards("giCones",            Renderer::giConesForQuality,            ladder::giCones);
    checkForwards("giRestirVisibility", Renderer::giRestirVisibilityForQuality, ladder::giRestirVisibility);
    checkForwards("voxelResolution",    Renderer::voxelResolutionForQuality,    ladder::voxelResolution);
    checkForwards("giUpdateInterval",   Renderer::giUpdateIntervalForQuality,   ladder::giUpdateInterval);
    checkForwards("giSkyOcclusionRays", Renderer::giSkyOcclusionRaysForQuality, ladder::giSkyOcclusionRays);
    checkForwards("giSkyOcclusionTile", Renderer::giSkyOcclusionTileForQuality, ladder::giSkyOcclusionTile);
    checkForwards("rtShadowRays",       Renderer::rtShadowRaysForQuality,       ladder::rtShadowRays);
    checkForwards("rtRenderMode",       Renderer::rtRenderModeForQuality,       ladder::rtRenderMode);
    checkForwards("ptBounces",          Renderer::ptBouncesForQuality,          ladder::ptBounces);
    checkForwards("rtShadowDenoise",    Renderer::rtShadowDenoiseForQuality,    ladder::rtShadowDenoise);
    checkForwards("rtPixelsPerRayTile", Renderer::rtPixelsPerRayTileForQuality, ladder::rtPixelsPerRayTile);

    std::printf("[INFO ] === Settings{} equals ladder(default tier), per group ===\n");
    check(Settings{}.globalIllumination == Quality::Medium, "Settings{}.globalIllumination is Medium");
    check(Settings{}.rayTracing         == Quality::Medium, "Settings{}.rayTracing is Medium");
    check(Settings{}.pathTracing        == Quality::Off,    "Settings{}.pathTracing is Off");
    check(Settings{}.voxelResolution  == ladder::voxelResolution(Quality::Medium),
          "Settings{}.voxelResolution matches ladder::voxelResolution(Medium)");
    check(Settings{}.giCones          == ladder::giCones(Quality::Medium),
          "Settings{}.giCones matches ladder::giCones(Medium)");
    check(Settings{}.giUpdateInterval == ladder::giUpdateInterval(Quality::Medium),
          "Settings{}.giUpdateInterval matches ladder::giUpdateInterval(Medium) -- the 1 -> 2 move");
    check(Settings{}.giRestirVisibility == ladder::giRestirVisibility(Quality::Medium),
          "Settings{}.giRestirVisibility matches ladder::giRestirVisibility(Medium) -- U1's HalfResolution default");
    check(Settings{}.rtShadowRays       == ladder::rtShadowRays(Quality::Medium),
          "Settings{}.rtShadowRays matches ladder::rtShadowRays(Medium)");
    check(Settings{}.rtPixelsPerRayTile == ladder::rtPixelsPerRayTile(Quality::Medium),
          "Settings{}.rtPixelsPerRayTile matches ladder::rtPixelsPerRayTile(Medium)");
    check(Settings{}.rtShadowDenoise    == ladder::rtShadowDenoise(Quality::Medium),
          "Settings{}.rtShadowDenoise matches ladder::rtShadowDenoise(Medium)");
    check(Settings{}.rtRenderMode       == ladder::rtRenderMode(Quality::Medium),
          "Settings{}.rtRenderMode matches ladder::rtRenderMode(Medium)");
    check(Settings{}.refractionMode     == ladder::refraction(Quality::Medium),
          "Settings{}.refractionMode matches ladder::refraction(Medium)");
    check(Settings{}.giSkyOcclusionRays == ladder::giSkyOcclusionRays(Quality::Medium),
          "Settings{}.giSkyOcclusionRays matches ladder::giSkyOcclusionRays(Medium)");
    check(Settings{}.giSkyOcclusionTile == ladder::giSkyOcclusionTile(Quality::Medium),
          "Settings{}.giSkyOcclusionTile matches ladder::giSkyOcclusionTile(Medium)");
    check(Settings{}.ptBounces == ladder::ptBounces(Quality::Off),
          "Settings{}.ptBounces matches ladder::ptBounces(Off)");

    std::printf("[INFO ] === adjacent rungs differ (Low..Epic), per group ===\n");
    {
        auto giDiffers = [](Quality a, Quality b) {
            return ladder::voxelResolution(a) != ladder::voxelResolution(b) ||
                   ladder::giCones(a)          != ladder::giCones(b) ||
                   ladder::giUpdateInterval(a) != ladder::giUpdateInterval(b);
        };
        check(giDiffers(Quality::Low, Quality::Medium),  "GI Low and Medium differ (voxelResolution/giCones/giUpdateInterval)");
        check(giDiffers(Quality::Medium, Quality::High), "GI Medium and High differ");
        check(giDiffers(Quality::High, Quality::Epic),   "GI High and Epic differ");
    }
    {
        auto rtDiffers = [](Quality a, Quality b) {
            return ladder::rtShadowRays(a)       != ladder::rtShadowRays(b) ||
                   ladder::rtPixelsPerRayTile(a)  != ladder::rtPixelsPerRayTile(b) ||
                   ladder::rtShadowDenoise(a)     != ladder::rtShadowDenoise(b) ||
                   ladder::rtRenderMode(a)        != ladder::rtRenderMode(b) ||
                   ladder::refraction(a)          != ladder::refraction(b) ||
                   ladder::giSkyOcclusionRays(a)  != ladder::giSkyOcclusionRays(b) ||
                   ladder::giSkyOcclusionTile(a)  != ladder::giSkyOcclusionTile(b);
        };
        check(rtDiffers(Quality::Low, Quality::Medium),  "RT Low and Medium differ (rtRenderMode: raster -> ray-driven, D3)");
        check(rtDiffers(Quality::Medium, Quality::High), "RT Medium and High differ (rays/denoise/sky-occlusion/refraction)");
        check(rtDiffers(Quality::High, Quality::Epic),   "RT High and Epic differ (rtShadowRays 4 -> 8)");
    }
    {
        // PT is the one group where Low and Medium are DELIBERATELY equal on the ladder's only knob --
        // section 4's own "Neighbouring tiers differ" list says so ("the accumulator grows at each
        // step; High and Epic add a bounce"), and the accumulator resolution that actually
        // differentiates Low from Medium is PtSceneView's, not a ladder:: function (see
        // QualityLadder.hpp's own top comment for why it is deliberately not here). Asserting
        // Low != Medium here would be asserting something section 4 does not claim.
        check(ladder::ptBounces(Quality::Low) == ladder::ptBounces(Quality::Medium),
              "PT Low and Medium share the same bounce count by design (2, 2) -- the accumulator, not ptBounces, differs there");
        check(ladder::ptBounces(Quality::Medium) != ladder::ptBounces(Quality::High),
              "PT Medium and High differ (ptBounces 2 -> 3)");
        check(ladder::ptBounces(Quality::High) != ladder::ptBounces(Quality::Epic),
              "PT High and Epic differ (ptBounces 3 -> 4)");
    }

    std::printf("[INFO ] === monotonicity, Low..Epic (Off excluded: every knob documents Off as inert, and\n"
                "[INFO ]     two of them -- giCones, and both non-increasing knobs -- are cheaper at Low\n"
                "[INFO ]     than at Off, which is the stated giConesForQuality(Off) exception generalised) ===\n");
    {
        auto nonDecreasing = [](const char* name, u32 (*fn)(Quality)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "ladder::%s is non-decreasing Low..Epic", name);
            check(fn(Quality::Low) <= fn(Quality::Medium) &&
                  fn(Quality::Medium) <= fn(Quality::High) &&
                  fn(Quality::High) <= fn(Quality::Epic), buf);
        };
        auto nonIncreasing = [](const char* name, u32 (*fn)(Quality)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "ladder::%s is non-increasing Low..Epic", name);
            check(fn(Quality::Low) >= fn(Quality::Medium) &&
                  fn(Quality::Medium) >= fn(Quality::High) &&
                  fn(Quality::High) >= fn(Quality::Epic), buf);
        };
        nonDecreasing("voxelResolution",   ladder::voxelResolution);
        nonDecreasing("giCones",           ladder::giCones);
        nonDecreasing("rtShadowRays",      ladder::rtShadowRays);
        nonDecreasing("refraction",        ladder::refraction);
        nonDecreasing("giSkyOcclusionRays",ladder::giSkyOcclusionRays);
        nonDecreasing("ptBounces",         ladder::ptBounces);
        nonDecreasing("giRestirVisibility",ladder::giRestirVisibility);
        nonIncreasing("giUpdateInterval",  ladder::giUpdateInterval);
        nonIncreasing("rtShadowDenoise",   ladder::rtShadowDenoise);
        // Exception 1 (section 4): rtRenderMode at Low changes the METHOD (raster), not the amount --
        // it is not cost-monotone across cameras (D3's own evidence is PARTIAL, not a speed claim), so
        // this file deliberately does not assert monotonicity for it at all.
    }

    std::printf("[INFO ] === giMode = 1 (ReSTIR GI) prerequisites ===\n");
    {
        Settings s{};
        s.giMode = 1;
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Medium;

        {
            const Resolution r = resolve(s, noRtHardwareDevice());
            check(r.giMode.requested == 1, "giMode.requested stays 1 even when hardware refuses it (resolve() never rewrites the request)");
            check(r.giMode.effective == 0, "giMode.effective is 0 with no RT hardware");
            check(r.giMode.reason == DisableReason::RequiresRayTracingHardware,
                  "giMode.reason is RequiresRayTracingHardware with no RT hardware");
        }
        {
            Settings s2 = s;
            s2.rayTracing = Quality::Off;
            const Resolution r = resolve(s2, fullDevice());
            check(r.giMode.reason == DisableReason::RequiresRayTracingEnabled,
                  "giMode.reason is RequiresRayTracingEnabled with capable hardware but RT tier Off");
            check(r.giMode.effective == 0, "giMode.effective is 0 when RT tier is Off");
        }
        {
            Settings s3 = s;
            s3.globalIllumination = Quality::Off;
            const Resolution r = resolve(s3, fullDevice());
            check(r.giMode.reason == DisableReason::RequiresGlobalIllumination,
                  "giMode.reason is RequiresGlobalIllumination with RT on but GI tier Off");
            check(r.giMode.effective == 0, "giMode.effective is 0 when GI tier is Off");
        }
        {
            const Resolution r = resolve(s, fullDevice());
            check(r.giMode.reason == DisableReason::None, "giMode has no reason when RT hardware, RT tier and GI tier are all satisfied");
            check(r.giMode.effective == 1, "giMode.effective is 1 when every prerequisite is met");
        }
    }

    std::printf("[INFO ] === F-d regression, through Renderer::get(): giMode survives a hardware refusal ===\n");
    {
        Renderer::get().setDeviceInfo(noRtHardwareDevice());
        Settings s = Renderer::get().settings();
        s.giMode = 1;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().giMode == 1,
              "settings().giMode stays 1 after setSettings on a device with no RT hardware (the Voxi.cpp:60 clamp is gone)");
    }

    std::printf("[INFO ] === rtRenderMode = 1 prerequisites, and its tier-change derivation ===\n");
    {
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.rayTracing = Quality::Off;
        s.rtRenderMode = 1;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().rtRenderMode == 0,
              "rtRenderMode requested 1 with RT tier Off (hardware present) is STORED as 0");
    }
    {
        Renderer::get().setDeviceInfo(noRtHardwareDevice());
        Settings s = Renderer::get().settings();
        s.rtRenderMode = 1;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().rtRenderMode == 0,
              "rtRenderMode requested 1 with no RT hardware is STORED as 0");
    }
    {
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.rayTracing = Quality::Off;
        Renderer::get().setSettings(s);   // establish Off as the live tier, rtRenderMode derives to 0

        s = Renderer::get().settings();
        s.rayTracing = Quality::Low;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().rtRenderMode == 0,
              "an Off -> Low tier change derives rtRenderMode 0 (ladder::rtRenderMode(Low), D3)");

        s = Renderer::get().settings();
        s.rayTracing = Quality::Off;
        Renderer::get().setSettings(s);

        s = Renderer::get().settings();
        s.rayTracing = Quality::Medium;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().rtRenderMode == 1,
              "an Off -> Medium tier change derives rtRenderMode 1 (ladder::rtRenderMode(Medium))");
    }

    std::printf("[INFO ] === refractionMode: the RT-hardware prerequisite, and the garbage-value range clamp ===\n");
    {
        Settings s{};
        s.rayTracing = Quality::Off;
        s.refractionMode = 2;
        Resolution r = resolve(s, fullDevice());
        check(r.refractionMode.effective == 1, "refractionMode 2 with RT tier Off resolves to 1 (ScreenSpace), not 0");

        s.rayTracing = Quality::High;
        r = resolve(s, fullDevice());
        check(r.refractionMode.effective == 2, "refractionMode 2 with RT tier High on capable hardware resolves to 2 (RayTraced)");
    }
    {
        // Through the full pipeline: setSettings' own range clamp catches the garbage value BEFORE
        // resolve() ever runs (resolve() itself trusts an already range-clamped request, per its own
        // top comment) -- so this is a regression test for the pipeline order, not for resolve() alone.
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.refractionMode = 7;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().refractionMode == 1, "refractionMode 7 (garbage) is stored as 1");
    }

    std::printf("[INFO ] === denoiser prerequisites, including the soft MSAA reason ===\n");
    {
        Settings s{};
        s.denoiser = true;
        s.rayTracing = Quality::Medium;
        s.globalIllumination = Quality::Medium;
        s.giMode = 1;
        s.giSkyOcclusionRays = 1;
        s.msaa = Msaa::Off;   // 1x

        {
            DeviceInfo d = fullDevice();
            d.nrdSupported = false;
            const Resolution r = resolve(s, d);
            check(r.denoiser.reason == DisableReason::RequiresNrd, "denoiser.reason is RequiresNrd when the device lacks NRD support");
            check(!r.denoiserGBufferWanted, "denoiserGBufferWanted is false when NRD itself is unsupported");
        }
        {
            Settings s2 = s;
            s2.rayTracing = Quality::Off;
            const Resolution r = resolve(s2, fullDevice());
            check(r.denoiser.reason == DisableReason::RequiresRayTracingEnabled, "denoiser.reason is RequiresRayTracingEnabled when RT tier is Off");
        }
        {
            Settings s3 = s;
            s3.giMode = 0;
            s3.giSkyOcclusionRays = 0;
            const Resolution r = resolve(s3, fullDevice());
            check(r.denoiser.reason == DisableReason::NothingToDenoise,
                  "denoiser.reason is NothingToDenoise with giMode 0 and no sky-occlusion rays");
        }
        {
            Settings s4 = s;
            s4.msaa = Msaa::X4;
            const Resolution r = resolve(s4, fullDevice());
            check(r.denoiser.reason == DisableReason::RequiresMsaaOne, "denoiser.reason is RequiresMsaaOne (soft) at 4x MSAA with a real signal");
            check(r.denoiser.effective == 0, "denoiser.effective is 0 at 4x MSAA -- NRD really does skip itself there");
            check(r.denoiserGBufferWanted, "denoiserGBufferWanted stays true under the soft MSAA reason");
        }
        {
            const Resolution r = resolve(s, fullDevice());
            check(r.denoiser.reason == DisableReason::None, "denoiser has no reason when every prerequisite is met");
            check(r.denoiser.effective == 1, "denoiser.effective is 1 when every prerequisite is met");
            check(r.denoiserGBufferWanted, "denoiserGBufferWanted is true when every prerequisite is met");
        }
    }

    std::printf("[INFO ] === Overall Quality scalability preset ===\n");
    {
        for (u32 t = 1; t <= 4; ++t) {
            const OverallQuality q = static_cast<OverallQuality>(t);
            const Quality tier = static_cast<Quality>(t);
            Settings s{};
            const u32 mask = applyOverall(s, q, fullDevice());
            char buf[128];
            std::snprintf(buf, sizeof(buf), "applyOverall(%s) sets GI and RT to %s, PT to Off", tierName(tier), tierName(tier));
            check(s.globalIllumination == tier && s.rayTracing == tier && s.pathTracing == Quality::Off, buf);
            check(mask == 0x7u, "applyOverall's mask has all three group bits set on a fully capable device");
            check(groupFollowsLadder(s, ScalabilityGroup::GlobalIllumination), "GI knobs follow the ladder after applyOverall");
            check(groupFollowsLadder(s, ScalabilityGroup::RayTracing), "RT knobs follow the ladder after applyOverall");
            check(groupFollowsLadder(s, ScalabilityGroup::PathTracing), "PT knobs follow the ladder after applyOverall");
        }
    }
    check(overallFromSettings(Settings{}, fullDevice()) == OverallQuality::Medium,
          "a fresh project's default settings read as Medium");
    {
        Settings s{};
        s.globalIllumination = Quality::High;
        s.voxelResolution  = ladder::voxelResolution(Quality::High);
        s.giCones          = ladder::giCones(Quality::High);
        s.giUpdateInterval = ladder::giUpdateInterval(Quality::High);
        // rayTracing is left at Settings{}'s own Medium, following Medium's ladder by construction.
        check(overallFromSettings(s, fullDevice()) == OverallQuality::Custom,
              "GI High with RT still at Medium reads as Custom (the groups disagree)");
    }
    {
        Settings s{};
        s.pathTracing = Quality::Low;
        s.ptBounces   = ladder::ptBounces(Quality::Low);
        check(overallFromSettings(s, fullDevice()) == OverallQuality::Custom,
              "Path Tracing above Off (Low) reads as Custom regardless of GI/RT -- every Overall rung is PT Off");
    }
    {
        const DeviceInfo noRt = noRtHardwareDevice();
        Settings s{};
        s.rayTracing   = Quality::Epic;
        s.rtShadowRays = 99;   // not any tier's ladder value at all (Off/Low/Medium=1, High=4, Epic=8)
        check(overallFromSettings(s, noRt) == OverallQuality::Medium,
              "a no-RT device reads Medium from GI alone; RT's own (unavailable) tier is ignored, not consulted");

        const u32 mask = applyOverall(s, OverallQuality::Low, noRt);
        check((mask & (1u << static_cast<u32>(ScalabilityGroup::RayTracing))) == 0,
              "applyOverall's mask excludes RayTracing on a no-RT device");
        check(s.rayTracing == Quality::Epic, "applyOverall leaves the RT tier completely untouched on a no-RT device");
        check(s.rtShadowRays == 99, "applyOverall leaves RT's derived knobs completely untouched on a no-RT device");
    }
    {
        Settings s{};
        s.giCones = ladder::giCones(Quality::Medium) + 1;
        check(!groupFollowsLadder(s, ScalabilityGroup::GlobalIllumination),
              "groupFollowsLadder is false once a knob (giCones) is hand-overridden away from the ladder");
    }

    std::printf("[INFO ] === U1: giRestirVisibility prerequisites, through resolve() ===\n");
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Medium;
        s.giMode = 0;
        s.giRestirVisibility = 3;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirVisibility.reason == DisableReason::RequiresRestirGi,
              "giRestirVisibility.reason is RequiresRestirGi when giMode is 0, with RT/GI tiers otherwise fine");
        check(r.giRestirVisibility.effective == r.giRestirVisibility.requested,
              "giRestirVisibility.effective == requested even while inert (never clamped to 0 on a failed prerequisite)");
    }
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Off;
        s.giMode = 1;
        s.giRestirVisibility = 2;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirVisibility.reason == DisableReason::RequiresRayTracingEnabled,
              "giRestirVisibility inherits giMode's own reason (RequiresRayTracingEnabled) when RT tier is Off");
        check(r.giRestirVisibility.effective == r.giRestirVisibility.requested,
              "giRestirVisibility.effective == requested here too");
    }
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Medium;
        s.giMode = 1;
        s.giRestirVisibility = 0;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirVisibility.reason == DisableReason::None,
              "giRestirVisibility has no reason once giMode itself resolves to ReSTIR with every prerequisite met");
        check(r.giRestirVisibility.effective == 0,
              "giRestirVisibility.effective passes an in-range request (NoRay, 0) straight through");
    }
    {
        Settings s{};
        s.giRestirVisibility = 99;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirVisibility.requested == 3,
              "resolve() clamps an out-of-range giRestirVisibility (99) to 3 on its own, independent of Voxi.cpp's clamp");
    }
    check(std::strcmp(disableReasonText(DisableReason::RequiresRestirGi), "Unavailable.") != 0,
          "disableReasonText(RequiresRestirGi) is its own sentence, not the generic fallback");

    std::printf("[INFO ] === U1: giRestirVisibility follows Overall Quality and groupFollowsLadder ===\n");
    {
        for (u32 t = 1; t <= 4; ++t) {
            const OverallQuality q = static_cast<OverallQuality>(t);
            const Quality tier = static_cast<Quality>(t);
            Settings s{};
            applyOverall(s, q, fullDevice());
            char buf[128];
            std::snprintf(buf, sizeof(buf), "applyOverall(%s) writes giRestirVisibility to ladder::giRestirVisibility(%s)",
                          tierName(tier), tierName(tier));
            check(s.giRestirVisibility == ladder::giRestirVisibility(tier), buf);
        }
    }
    {
        Settings s{};
        applyOverall(s, OverallQuality::Medium, fullDevice());
        check(groupFollowsLadder(s, ScalabilityGroup::GlobalIllumination),
              "GI still follows the ladder immediately after applyOverall (giRestirVisibility included)");
        s.giRestirVisibility = (ladder::giRestirVisibility(Quality::Medium) + 1u) % 4u;   // any other legal value
        check(!groupFollowsLadder(s, ScalabilityGroup::GlobalIllumination),
              "groupFollowsLadder is false once giRestirVisibility alone is hand-overridden away from the ladder");
        check(overallFromSettings(s, fullDevice()) == OverallQuality::Custom,
              "overallFromSettings reads Custom once giRestirVisibility alone has drifted from Medium's rung");
    }

    std::printf("[INFO ] === U1: giRestirVisibility, through Renderer::get() -- tier-change derivation, explicit override, clamp ===\n");
    {
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.globalIllumination = Quality::Off;
        Renderer::get().setSettings(s);   // establish Off as the live tier

        s = Renderer::get().settings();
        s.globalIllumination = Quality::Low;
        Renderer::get().setSettings(s);
        // Off and Low deliberately differ (3 vs 1) -- unlike Off and High/Epic, which share Full (3) and
        // would let this check pass even if the derivation never ran at all.
        check(Renderer::get().settings().giRestirVisibility == ladder::giRestirVisibility(Quality::Low),
              "an Off -> Low tier change re-derives giRestirVisibility (field left untouched in the same call)");
    }
    {
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.globalIllumination = Quality::Off;
        Renderer::get().setSettings(s);

        s = Renderer::get().settings();
        s.globalIllumination = Quality::Epic;
        s.giRestirVisibility = 1;   // explicit request in the SAME call that changes the tier
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().giRestirVisibility == 1,
              "an explicit giRestirVisibility set in the same call as a tier change survives the derivation");
    }
    {
        Renderer::get().setDeviceInfo(fullDevice());
        Settings s = Renderer::get().settings();
        s.giRestirVisibility = 9;
        Renderer::get().setSettings(s);
        check(Renderer::get().settings().giRestirVisibility == 3,
              "giRestirVisibility 9 (garbage) clamps to 3 (Full), never to 0 (NoRay)");
    }

    std::printf("[INFO ] === giRestirReuse prerequisites, through resolve() -- mirrors giRestirVisibility ===\n");
    // NOT tier-derived (Settings::giRestirReuse's own comment), so unlike giRestirVisibility above
    // this gets no ladder-following / Overall-Quality / Renderer-singleton-tier-change coverage --
    // there is no ladder function for it and setSettings never re-derives it from a tier change. What
    // it DOES share with giRestirVisibility is the resolver's gating, added only so the Settings page
    // can grey both combos on the identical condition -- that is what these mirror.
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Medium;
        s.giMode = 0;
        s.giRestirReuse = 2;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirReuse.reason == DisableReason::RequiresRestirGi,
              "giRestirReuse.reason is RequiresRestirGi when giMode is 0, with RT/GI tiers otherwise fine");
        check(r.giRestirReuse.effective == r.giRestirReuse.requested,
              "giRestirReuse.effective == requested even while inert");
    }
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Off;
        s.giMode = 1;
        s.giRestirReuse = 1;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirReuse.reason == DisableReason::RequiresRayTracingEnabled,
              "giRestirReuse inherits giMode's own reason (RequiresRayTracingEnabled) when RT tier is Off");
        check(r.giRestirReuse.effective == r.giRestirReuse.requested,
              "giRestirReuse.effective == requested here too");
    }
    {
        Settings s{};
        s.globalIllumination = Quality::Medium;
        s.rayTracing = Quality::Medium;
        s.giMode = 1;
        s.giRestirReuse = 1;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirReuse.reason == DisableReason::None,
              "giRestirReuse has no reason once giMode itself resolves to ReSTIR with every prerequisite met");
        check(r.giRestirReuse.effective == 1,
              "giRestirReuse.effective passes an in-range request (Settled, 1) straight through");
    }
    {
        Settings s{};
        s.giRestirReuse = 99;
        const Resolution r = resolve(s, fullDevice());
        check(r.giRestirReuse.requested == 2,
              "resolve() clamps an out-of-range giRestirReuse (99) to 2 on its own, independent of Voxi.cpp's clamp");
    }

    std::printf("[INFO ] === U2: ladder::averSrLevel / overallAverSrLevel / autoAverSrLevel ===\n");
    {
        for (u32 t = 1; t <= 4; ++t) {
            const OverallQuality q = static_cast<OverallQuality>(t);
            const Quality tier = static_cast<Quality>(t);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "overallAverSrLevel(%s) == ladder::averSrLevel(%s)", tierName(tier), tierName(tier));
            check(overallAverSrLevel(q) == ladder::averSrLevel(tier), buf);
        }
        check(overallAverSrLevel(OverallQuality::Custom) == ladder::averSrLevel(Quality::Off),
              "overallAverSrLevel(Custom) casts to Quality::Off's level (0) -- never called this way in practice, still defined");
    }
    {
        // GI Epic, RT Low, NEITHER following its own ladder rung (both left at Settings{}'s Medium
        // knobs) -- overallFromSettings must read Custom for this pair, which is the precondition
        // autoAverSrLevel's Custom branch needs.
        Settings s{};
        s.globalIllumination = Quality::Epic;
        s.rayTracing = Quality::Low;
        check(overallFromSettings(s, fullDevice()) == OverallQuality::Custom,
              "GI Epic + RT Low (neither following its own ladder) reads Custom -- precondition for the next check");
        check(autoAverSrLevel(s, fullDevice()) == ladder::averSrLevel(Quality::Epic),
              "autoAverSrLevel on Custom takes the HIGHER of GI/RT tier (Epic over Low) -- averSrLevel(Epic) == 1 (Quality)");
    }
    {
        const DeviceInfo noRt = noRtHardwareDevice();
        Settings s{};
        s.globalIllumination  = Quality::High;
        s.voxelResolution     = ladder::voxelResolution(Quality::High);
        s.giCones             = ladder::giCones(Quality::High);
        s.giUpdateInterval    = ladder::giUpdateInterval(Quality::High);
        s.giRestirVisibility  = ladder::giRestirVisibility(Quality::High);
        s.rayTracing = Quality::Epic;   // ignored entirely: RayTracing is unavailable on this device
        check(overallFromSettings(s, noRt) == OverallQuality::High,
              "a no-RT device reads High from GI alone -- precondition for the next check");
        check(autoAverSrLevel(s, noRt) == ladder::averSrLevel(Quality::High),
              "autoAverSrLevel on a device without RT hardware follows GI's tier alone, via overallFromSettings");
    }

    std::printf("[INFO ] === U2: resolveAverSrLevel precedence -- CLI > user > manifest > auto ===\n");
    {
        // A correct oracle, written independently of resolveAverSrLevel's own body, against the stated
        // precedence -- checked against the real function rather than against itself.
        auto oracleCorrect = [](int cli, int user, int manifest, u32 autoL) -> AverSrDecision {
            if (cli >= 0)      return AverSrDecision{static_cast<u32>(cli),      AverSrSource::Cli};
            if (user >= 0)     return AverSrDecision{static_cast<u32>(user),     AverSrSource::User};
            if (manifest >= 0) return AverSrDecision{static_cast<u32>(manifest), AverSrSource::Manifest};
            return AverSrDecision{autoL, AverSrSource::Auto};
        };
        // THE NEGATIVE CONTROL: user ranked ABOVE cli, the wrong order -- exists to prove this sweep can
        // actually fail. It only diverges from the correct oracle in the cases where BOTH cli and user
        // are present at once (the only cases the two orderings disagree on a winner).
        auto oracleWrongOrder = [](int cli, int user, int manifest, u32 autoL) -> AverSrDecision {
            if (user >= 0)     return AverSrDecision{static_cast<u32>(user),     AverSrSource::User};
            if (cli >= 0)      return AverSrDecision{static_cast<u32>(cli),      AverSrSource::Cli};
            if (manifest >= 0) return AverSrDecision{static_cast<u32>(manifest), AverSrSource::Manifest};
            return AverSrDecision{autoL, AverSrSource::Auto};
        };

        const int kAbsent = -1;
        const u32 autoL = 2u;
        bool allAgree = true;
        bool wrongOracleEverDisagreed = false;
        int combos = 0;
        // 3 presence bits (cli/user/manifest) x 2 value variants per present source = 16 cases --
        // covering both "who wins" (presence) and "which value wins" for whichever source does.
        for (int hasCli = 0; hasCli < 2; ++hasCli) {
            for (int hasUser = 0; hasUser < 2; ++hasUser) {
                for (int hasManifest = 0; hasManifest < 2; ++hasManifest) {
                    for (int variant = 0; variant < 2; ++variant) {
                        const int cli      = hasCli      ? (variant == 0 ? 0 : 3) : kAbsent;
                        const int user     = hasUser     ? (variant == 0 ? 1 : 2) : kAbsent;
                        const int manifest = hasManifest ? (variant == 0 ? 2 : 1) : kAbsent;
                        ++combos;

                        const AverSrDecision got  = resolveAverSrLevel(cli, user, manifest, autoL);
                        const AverSrDecision want = oracleCorrect(cli, user, manifest, autoL);
                        if (got.level != want.level || got.source != want.source) allAgree = false;

                        if (hasCli && hasUser) {
                            const AverSrDecision wrong = oracleWrongOrder(cli, user, manifest, autoL);
                            if (wrong.level != want.level || wrong.source != want.source)
                                wrongOracleEverDisagreed = true;
                        }
                    }
                }
            }
        }
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "resolveAverSrLevel agrees with the CLI>user>manifest>auto oracle over all %d presence/value combinations",
                      combos);
        check(allAgree, buf);
        check(wrongOracleEverDisagreed,
              "negative control: a user-above-cli oracle DOES disagree with the correct one somewhere in the sweep (the test can fail)");
    }
    {
        const AverSrDecision fallback = resolveAverSrLevel(-1, -1, -1, 1u);
        check(fallback.source == AverSrSource::Auto && fallback.level == 1u,
              "resolveAverSrLevel falls back to Auto with autoLevel passed through when nothing else is present");
        const AverSrDecision clampedCli = resolveAverSrLevel(99, -1, -1, 0u);
        check(clampedCli.level == 3u && clampedCli.source == AverSrSource::Cli,
              "resolveAverSrLevel clamps an out-of-range CLI level (99) to 3, not to 0 or a wraparound");
        const AverSrDecision clampedAuto = resolveAverSrLevel(-1, -1, -1, 99u);
        check(clampedAuto.level == 3u,
              "resolveAverSrLevel clamps an out-of-range autoLevel too, not only the -1-able inputs");
    }

    std::printf("[INFO ] === disableReasonText and refusalFeatureFor ===\n");
    {
        bool allNonNull = true;
        for (u32 i = 0; i < static_cast<u32>(DisableReason::Count); ++i) {
            if (disableReasonText(static_cast<DisableReason>(i)) == nullptr) allNonNull = false;
        }
        check(allNonNull, "disableReasonText never returns null for any DisableReason below Count");
    }
    {
        Feature f;
        check(refusalFeatureFor(DisableReason::RequiresComputeShaders, f) && f == Feature::GlobalIllumination,
              "RequiresComputeShaders maps to Feature::GlobalIllumination");
        check(refusalFeatureFor(DisableReason::RequiresRayTracingHardware, f) && f == Feature::RayTracing,
              "RequiresRayTracingHardware maps to Feature::RayTracing");
        check(refusalFeatureFor(DisableReason::RequiresPathTracingHardware, f) && f == Feature::PathTracing,
              "RequiresPathTracingHardware maps to Feature::PathTracing");
        check(refusalFeatureFor(DisableReason::RequiresMeshShaderHardware, f) && f == Feature::MeshShaders,
              "RequiresMeshShaderHardware maps to Feature::MeshShaders");
        check(refusalFeatureFor(DisableReason::NotImplemented, f),
              "NotImplemented maps to a feature (this project has only ever used it for LayeredBsdf)");

        check(!refusalFeatureFor(DisableReason::None, f), "None does not map to a feature");
        check(!refusalFeatureFor(DisableReason::RequiresRayTracingEnabled, f),
              "RequiresRayTracingEnabled does not map -- reported fresh by the caller, not via refuse()'s existing log");
        check(!refusalFeatureFor(DisableReason::RequiresGlobalIllumination, f), "RequiresGlobalIllumination does not map");
        check(!refusalFeatureFor(DisableReason::RequiresNrd, f), "RequiresNrd does not map");
        check(!refusalFeatureFor(DisableReason::NothingToDenoise, f), "NothingToDenoise does not map");
        check(!refusalFeatureFor(DisableReason::RequiresMsaaOne, f), "RequiresMsaaOne does not map (it is the soft reason)");
        check(!refusalFeatureFor(DisableReason::RequiresRestirGi, f),
              "RequiresRestirGi does not map -- nothing already logs it the way refuse() logs a hardware refusal");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
