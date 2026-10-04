// ProjectRenderApply: manifest apply/capture, N6/R2/N7/N5/N9 fixes, and CLI override precedence.
// Compiled by build, never run from workflow.
#include "aver/voxi/ProjectRenderApply.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

// Same idiom as tests/formats/src/OcInputTest.cpp.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Fully capable device: compute, RT hardware, mesh shaders, all MSAA counts, denoiser.
static voxi::DeviceInfo fullyCapableDevice() {
    voxi::DeviceInfo d;
    d.msaaMask = 1u | 2u | 4u | 8u;
    d.maxMsaaSamples = 8;
    d.rayTracingTier = 11;
    d.computeShaders = true;
    d.typedUavLoads = true;
    d.conservativeRaster = true;
    d.shaderModel = 65;
    d.meshShaderTier = 1;
    d.dxcAvailable = true;
    d.denoiserSupported = true;
    return d;
}

// Compute shaders only: no RT or mesh shaders, as some actual hardware.
static voxi::DeviceInfo noRtDevice() {
    voxi::DeviceInfo d;
    d.msaaMask = 1u | 2u | 4u | 8u;
    d.maxMsaaSamples = 8;
    d.rayTracingTier = 0;
    d.computeShaders = true;
    d.shaderModel = 60;
    d.meshShaderTier = 0;
    d.dxcAvailable = true;
    d.denoiserSupported = false;
    return d;
}

// Field-by-field equality (memcmp would trip on padding). Defines "unchanged".
static bool settingsEqual(const voxi::Settings& a, const voxi::Settings& b) {
    return a.msaa == b.msaa &&
           a.globalIllumination == b.globalIllumination &&
           a.rayTracing == b.rayTracing &&
           a.pathTracing == b.pathTracing &&
           a.layeredBsdf == b.layeredBsdf &&
           a.meshShaders == b.meshShaders &&
           a.voxelResolution == b.voxelResolution &&
           a.giCones == b.giCones &&
           a.giSkyOcclusionRays == b.giSkyOcclusionRays &&
           a.giSkyOcclusionTile == b.giSkyOcclusionTile &&
           near(a.giIntensity, b.giIntensity) &&
           near(a.causticStrength, b.causticStrength) &&
           near(a.giMaxDistance, b.giMaxDistance) &&
           near(a.giRadianceCeiling, b.giRadianceCeiling) &&
           a.refractionMode == b.refractionMode &&
           near(a.refractionStrength, b.refractionStrength) &&
           near(a.refractionEdgeFade, b.refractionEdgeFade) &&
           a.rtShadowRays == b.rtShadowRays &&
           a.rtPixelsPerRayTile == b.rtPixelsPerRayTile &&
           a.giUpdateInterval == b.giUpdateInterval &&
           a.giMode == b.giMode &&
           a.denoiser == b.denoiser &&
           a.giRestirMaxHistory == b.giRestirMaxHistory &&
           a.denoiserMaxSamples == b.denoiserMaxSamples &&
           near(a.denoiserHistoryClipWeight, b.denoiserHistoryClipWeight) &&
           a.denoiserSunMovingSamples == b.denoiserSunMovingSamples &&
           a.rtShadowDenoise == b.rtShadowDenoise &&
           a.rtRenderMode == b.rtRenderMode &&
           a.rayDrivenStages == b.rayDrivenStages &&
           a.fogOcclusion == b.fogOcclusion &&
           a.ptBounces == b.ptBounces &&
           a.ptMode == b.ptMode;
}

// 4 tier keys (GI, RAYTRACING, PATHTRACING, LAYEREDBSDF) and 17 knob keys reach their Settings fields.
// LODSELECT/LODTHRESHOLD/OCCLUSIONCULL/DEPTHPREPASS are ProjectDesc keys but not handled here.
static void testEveryKeyReachesSettings() {
    AVER_INFO("=== every RENDER.* key that maps to a Settings field reaches it ===");

    {
        fmt::ProjectDesc p;
        p.giQuality   = static_cast<int>(voxi::Quality::High);
        p.rayTracing  = static_cast<int>(voxi::Quality::Low);
        p.pathTracing = static_cast<int>(voxi::Quality::Epic);
        p.layeredBsdf = static_cast<int>(voxi::Quality::Medium);

        voxi::Settings s{};
        voxi::applyManifestTiers(p, s);
        check(s.globalIllumination == voxi::Quality::High, "RENDER.GI -> Settings::globalIllumination");
        check(s.rayTracing == voxi::Quality::Low, "RENDER.RAYTRACING -> Settings::rayTracing");
        check(s.pathTracing == voxi::Quality::Epic, "RENDER.PATHTRACING -> Settings::pathTracing");
        check(s.layeredBsdf == voxi::Quality::Medium, "RENDER.LAYEREDBSDF -> Settings::layeredBsdf");
    }
    {
        fmt::ProjectDesc p;
        p.voxelResolution    = 384;
        p.giIntensity        = 2.5f;
        p.giMaxDistance      = 1234.0f;
        p.rtShadowRays       = 6;
        p.rtPixelsPerRayTile = 8;
        p.rtShadowDenoise    = 3;
        p.rtRenderMode       = 1;
        p.ptBounces          = 5;
        p.giCones            = 11;
        p.giMode             = 1;
        p.denoiser           = 1;
        p.refractionMode     = 2;
        p.refractionStrength = 0.42f;
        p.refractionEdgeFade = 0.05f;
        p.msaa               = 8;
        p.meshShaders        = 1;
        p.giUpdateInterval   = 7;
        p.ptMode             = 1;

        // All 17 keys stated, so N6 else-branches never fire: every landing value is the manifest's ask.
        voxi::Settings s{};
        voxi::applyManifestTiers(p, s);
        voxi::applyManifestKnobs(p, s);

        check(s.voxelResolution == 384, "RENDER.VOXELRES -> Settings::voxelResolution");
        check(near(s.giIntensity, 2.5f), "RENDER.GIINTENSITY -> Settings::giIntensity");
        check(near(s.giMaxDistance, 1234.0f), "RENDER.GIDISTANCE -> Settings::giMaxDistance");
        check(s.rtShadowRays == 6, "RENDER.RTSHADOWRAYS -> Settings::rtShadowRays");
        check(s.rtPixelsPerRayTile == 8, "RENDER.RTPIXELSPERRAY -> Settings::rtPixelsPerRayTile");
        check(s.rtShadowDenoise == 3, "RENDER.RTSHADOWDENOISE -> Settings::rtShadowDenoise");
        check(s.rtRenderMode == 1, "RENDER.RTRENDERMODE -> Settings::rtRenderMode");
        check(s.ptBounces == 5, "RENDER.PTBOUNCES -> Settings::ptBounces");
        check(s.giCones == 11, "RENDER.GICONES -> Settings::giCones");
        check(s.giMode == 1, "RENDER.GIMODE -> Settings::giMode");
        check(s.denoiser == true, "RENDER.DENOISER -> Settings::denoiser");
        check(s.refractionMode == 2, "RENDER.REFRACTIONMODE -> Settings::refractionMode");
        check(near(s.refractionStrength, 0.42f), "RENDER.REFRACTIONSTRENGTH -> Settings::refractionStrength");
        check(near(s.refractionEdgeFade, 0.05f), "RENDER.REFRACTIONEDGEFADE -> Settings::refractionEdgeFade");
        check(s.msaa == voxi::Msaa::X8, "RENDER.MSAA -> Settings::msaa");
        check(s.meshShaders == true, "RENDER.MESHSHADERS -> Settings::meshShaders");
        check(s.giUpdateInterval == 7, "RENDER.GIUPDATEINTERVAL -> Settings::giUpdateInterval");
        check(s.ptMode == 1, "RENDER.PTMODE -> Settings::ptMode");
    }
}

// N6: absent derived knob resets to its NEW tier's ladder value, not stale live.
static void testN6AbsentDerivedKnobResetsToLadder() {
    AVER_INFO("=== N6: an absent derived knob resets to its tier's ladder value ===");
    voxi::Settings s{};
    s.globalIllumination = voxi::Quality::Epic;
    s.giCones = 7;   // stale pin

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Epic);   // same tier, no change

    voxi::applyManifestTiers(p, s);
    check(s.giCones == 7, "restating the same tier alone does not touch the stale knob");
    voxi::applyManifestKnobs(p, s);
    check(s.giCones == voxi::ladder::giCones(voxi::Quality::Epic),
          "an absent GICONES resets to ladder::giCones(Epic) = 13, not the stale 7");
}

// N6 extended to giRestirVisibility: absent RESTIRVISIBILITY also resets to ladder value.
static void testN6RestirVisibilityFollowsTier() {
    AVER_INFO("=== N6: an absent RESTIRVISIBILITY resets to its tier's ladder value ===");
    voxi::Settings s{};
    s.globalIllumination = voxi::Quality::Low;
    s.giRestirVisibility = 3;   // stale pin

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Low);   // same tier

    voxi::applyManifestTiers(p, s);
    check(s.giRestirVisibility == 3, "restating the same tier alone does not touch the stale knob");
    voxi::applyManifestKnobs(p, s);
    check(s.giRestirVisibility == voxi::ladder::giRestirVisibility(voxi::Quality::Low),
          "an absent RESTIRVISIBILITY resets to ladder::giRestirVisibility(Low), not the stale 3");
}

// R2 extended: two-phase apply avoids the regression where single setSettings re-derives the knob.
static void testR2RestirVisibilityRaceThroughTheSingleton() {
    AVER_INFO("=== R2: giRestirVisibility race, two-phase through voxi::Renderer avoids it too ===");
    voxi::Renderer& vx = voxi::Renderer::get();
    vx.setDeviceInfo(fullyCapableDevice());

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    vx.setSettings(live);
    const u32 mediumRestirVis = voxi::ladder::giRestirVisibility(voxi::Quality::Medium);
    const u32 epicRestirVis   = voxi::ladder::giRestirVisibility(voxi::Quality::Epic);
    check(vx.settings().globalIllumination == voxi::Quality::Medium &&
          vx.settings().giRestirVisibility == mediumRestirVis,
          "singleton seeded at GI Medium, giRestirVisibility at Medium's own ladder rung");

    fmt::ProjectDesc p;
    p.giQuality        = static_cast<int>(voxi::Quality::Epic);
    p.restirVisibility = static_cast<int>(mediumRestirVis);

    voxi::Settings s = vx.settings();
    voxi::applyManifestTwoPhase(p, s, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    check(s.giRestirVisibility == mediumRestirVis,
          "two-phase apply: the explicit RESTIRVISIBILITY survives even though Epic's ladder rung differs");
    check(vx.settings().giRestirVisibility == mediumRestirVis, "...and the singleton itself agrees");

    // Single merged call would have re-derived Epic's value: proven by applying both phases in one buffer.
    check(mediumRestirVis != epicRestirVis,
          "sanity: Medium and Epic's own RESTIRVISIBILITY ladder rungs must differ for this race to be live");
    vx.setSettings(live);
    voxi::Settings merged = vx.settings();
    voxi::applyManifestTiers(p, merged);
    voxi::applyManifestKnobs(p, merged);
    check(merged.giRestirVisibility == mediumRestirVis,
          "before the single commit, the merged buffer also holds the explicit value");
    vx.setSettings(merged);
    check(vx.settings().giRestirVisibility == epicRestirVis,
          "...but the single commit's own change-gated derivation overwrites it with Epic's ladder value "
          "-- the exact regression two-phase (above) avoids");
}

// Empty manifest leaves Settings{} unchanged: Settings{}'s defaults already match ladder(Medium)/ladder(Off).
static void testEmptyManifestLeavesSettingsUnchanged() {
    AVER_INFO("=== ProjectDesc{{}} (empty manifest) leaves Settings{{}} completely unchanged ===");
    fmt::ProjectDesc empty;
    check(!empty.hasRenderSettings(), "a default-constructed ProjectDesc states no RENDER.* key");

    voxi::Settings s{};
    const voxi::Settings before = s;
    voxi::applyManifestTiers(empty, s);
    voxi::applyManifestKnobs(empty, s);
    check(settingsEqual(s, before), "applying an empty manifest to Settings{} changes nothing");
}

// R2 through singleton: two-phase apply avoids merging live tier state before applying manifest knobs.
static void testR2RegressionThroughTheSingleton() {
    AVER_INFO("=== R2: two-phase through voxi::Renderer avoids the merged-call regression ===");
    voxi::Renderer& vx = voxi::Renderer::get();
    vx.setDeviceInfo(fullyCapableDevice());

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    vx.setSettings(live);
    check(vx.settings().globalIllumination == voxi::Quality::Medium && vx.settings().giCones == 6,
          "singleton seeded at GI Medium, giCones 6");

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Epic);
    p.giCones   = 6;

    voxi::Settings s = vx.settings();
    voxi::applyManifestTwoPhase(p, s, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    check(s.giCones == 6, "two-phase apply: GICONES 6 survives even though Epic's ladder rung is 13");
    check(vx.settings().giCones == 6, "...and the singleton itself agrees");

    // Single merged call would have re-derived 13: reset, apply both phases to one buffer, commit once.
    vx.setSettings(live);
    voxi::Settings merged = vx.settings();
    voxi::applyManifestTiers(p, merged);
    voxi::applyManifestKnobs(p, merged);
    check(merged.giCones == 6, "before the single commit, the merged buffer also holds the explicit 6");
    vx.setSettings(merged);
    check(vx.settings().giCones == 13,
          "...but the single commit's own change-gated derivation overwrites it with Epic's 13 -- "
          "the exact regression two-phase (above) avoids");
}

// ---- applyCliOverrides: the command-line half (N7) ----

// Counts override notes. The HOST words the sentences; this test checks WHETHER they were produced.
struct NoteCounter {
    u32 total = 0;
    u32 takes = 0;
    u32 forceOffs = 0;
    u32 giIntensities = 0;

    void operator()(const voxi::CliOverrideNote& n) {
        ++total;
        switch (n.kind) {
            case voxi::CliOverrideKind::Take:        ++takes; break;
            case voxi::CliOverrideKind::ForceOff:    ++forceOffs; break;
            case voxi::CliOverrideKind::GiIntensity: ++giIntensities; break;
        }
    }
};

// Every RenderCliOverrides field reaches the Settings field it names (wiring check after collapse).
// applyCliTiers/applyCliKnobs mutate Settings& only, no singleton derivation: "the flag wrote this field".
// Note: --restir-visibility and --rt-render-mode use -1-sentinel so 0 stays expressible (0 = "No ray", "rasteriser").
static void testCliEveryFlagReachesItsField() {
    AVER_INFO("=== CLI: every RenderCliOverrides field reaches the Settings field it names ===");

    voxi::RenderCliOverrides cli;
    cli.globalIllumination = static_cast<int>(voxi::Quality::Epic);
    cli.rayTracing         = static_cast<int>(voxi::Quality::Low);
    cli.pathTracing        = static_cast<int>(voxi::Quality::High);
    cli.rtRenderMode       = 0;
    cli.refractionMode     = 2;
    cli.rtShadowDenoise    = 5;
    cli.ptBounces          = 6;
    cli.layeredBsdf        = static_cast<int>(voxi::Quality::Medium);
    cli.msaa               = static_cast<int>(voxi::Msaa::X8);
    cli.rtShadowRays       = 7;
    cli.giSkyOcclusionRays = 3;
    cli.giSkyOcclusionTile = 4;
    cli.giIntensity        = 2.5f;
    cli.rtPixelsPerRayTile = 2;
    cli.giUpdateInterval   = 5;
    cli.giMode             = 1;
    cli.giRestirVisibility = 0;
    cli.denoiser           = 1;

    NoteCounter notes;
    voxi::Settings s{};
    check(voxi::applyCliTiers(cli, s, notes), "Phase A reports that it changed something");
    check(voxi::applyCliKnobs(cli, s, notes), "Phase B reports that it changed something");

    check(s.globalIllumination == voxi::Quality::Epic, "--gi reaches Settings::globalIllumination");
    check(s.rayTracing == voxi::Quality::Low,          "--rt reaches Settings::rayTracing");
    check(s.pathTracing == voxi::Quality::High,        "--pt reaches Settings::pathTracing");
    check(s.rtRenderMode == 0,        "--rt-render-mode 0 reaches Settings::rtRenderMode (0 is an ask)");
    check(s.refractionMode == 2,      "--refraction reaches Settings::refractionMode");
    check(s.rtShadowDenoise == 5,     "--rt-shadow-denoise reaches Settings::rtShadowDenoise");
    check(s.ptBounces == 6,           "--pt-bounces reaches Settings::ptBounces");
    check(s.layeredBsdf == voxi::Quality::Medium, "--layered-bsdf reaches Settings::layeredBsdf");
    check(s.msaa == voxi::Msaa::X8,   "--msaa reaches Settings::msaa");
    check(s.rtShadowRays == 7,        "--rt-rays reaches Settings::rtShadowRays");
    check(s.giSkyOcclusionRays == 3,  "--gi-sky-occlusion-rays reaches Settings::giSkyOcclusionRays");
    check(s.giSkyOcclusionTile == 4,  "--gi-sky-occlusion-tile reaches Settings::giSkyOcclusionTile");
    check(near(s.giIntensity, 2.5f),  "--gi-intensity reaches Settings::giIntensity");
    check(s.rtPixelsPerRayTile == 2,  "--rt-pixels-per-ray reaches Settings::rtPixelsPerRayTile");
    check(s.giUpdateInterval == 5,    "--gi-update-interval reaches Settings::giUpdateInterval");
    check(s.giMode == 1,              "--gi-mode reaches Settings::giMode");
    check(s.giRestirVisibility == 0,  "--restir-visibility 0 reaches Settings::giRestirVisibility (0 is an ask)");
    check(s.denoiser,                 "--denoiser reaches Settings::denoiser");

    // 3 notes from Phase A (tier takes), 15 from Phase B (knob takes + giIntensity special case).
    check(notes.total == 18 && notes.takes == 17 && notes.giIntensities == 1 && notes.forceOffs == 0,
          "one note per flag that won: 3 tier takes, 14 knob takes, 1 --gi-intensity, 0 force-offs");
}

// Absent flags (default RenderCliOverrides) leave Settings untouched, both phases return false.
// Four flags use 0-means-absent: msaa, rtShadowRays, rtPixelsPerRayTile, giUpdateInterval default to 0.
// take() treats NEGATIVE as absent, so applyCliKnobs' `> 0` guards must not be dropped.
static void testCliAbsentOverridesLeaveSettingsAlone() {
    AVER_INFO("=== CLI: a default RenderCliOverrides leaves Settings untouched (all three sentinels) ===");

    voxi::Settings s{};
    s.msaa               = voxi::Msaa::X8;
    s.globalIllumination = voxi::Quality::Epic;
    s.rayTracing         = voxi::Quality::High;
    s.pathTracing        = voxi::Quality::Low;
    s.layeredBsdf        = voxi::Quality::Medium;
    s.rtShadowRays       = 7;
    s.rtPixelsPerRayTile = 3;
    s.giUpdateInterval   = 5;
    s.giMode             = 1;
    s.giRestirVisibility = 4;
    s.giIntensity        = 3.25f;
    s.denoiser           = true;
    const voxi::Settings before = s;

    NoteCounter notes;
    const voxi::RenderCliOverrides none{};
    check(!voxi::applyCliTiers(none, s, notes), "Phase A reports no change, so the caller skips its commit");
    check(!voxi::applyCliKnobs(none, s, notes), "Phase B reports no change, so the caller skips its commit");
    check(notes.total == 0, "an empty command line prints nothing");
    check(settingsEqual(s, before), "an empty command line leaves every Settings field exactly as it was");
    // settingsEqual doesn't compare giRestirVisibility.
    check(s.giRestirVisibility == 4, "...giRestirVisibility included, which settingsEqual does not cover");

    // Four 0-sentinel flags against non-zero Settings: each must survive being stated at 0.
    voxi::RenderCliOverrides zeros;
    zeros.msaa               = 0;
    zeros.rtShadowRays       = 0;
    zeros.rtPixelsPerRayTile = 0;
    zeros.giUpdateInterval   = 0;
    check(!voxi::applyCliKnobs(zeros, s, notes), "0 in the four 0-means-absent flags is not a request");
    check(s.msaa == voxi::Msaa::X8 && s.rtShadowRays == 7 && s.rtPixelsPerRayTile == 3 &&
              s.giUpdateInterval == 5,
          "...and none of msaa/rtShadowRays/rtPixelsPerRayTile/giUpdateInterval was zeroed");
    check(notes.total == 0, "...and nothing claimed to have outranked anything");
}

// --no-gi/--no-rt are boolean Phase A tier changes, not integer sentinels. Inert when already Off.
static void testCliForceOffIsAPhaseATierChange() {
    AVER_INFO("=== CLI: --no-gi/--no-rt are Phase A tier changes, and are inert when already Off ===");

    voxi::RenderCliOverrides cli;
    cli.giForceOff = true;
    cli.rtForceOff = true;

    NoteCounter notes;
    voxi::Settings s{};
    s.globalIllumination = voxi::Quality::Epic;
    s.rayTracing         = voxi::Quality::High;
    check(voxi::applyCliTiers(cli, s, notes), "Phase A reports the force-offs changed something");
    check(s.globalIllumination == voxi::Quality::Off, "--no-gi forced GI Off");
    check(s.rayTracing == voxi::Quality::Off,         "--no-rt forced RT Off");
    check(notes.forceOffs == 2 && notes.takes == 0,
          "both force-offs said so out loud, and neither was mistaken for an integer take");

    // Already Off: not a change, caller must not be told to commit.
    NoteCounter again;
    check(!voxi::applyCliTiers(cli, s, again), "--no-gi/--no-rt against an already-Off tier is inert");
    check(again.total == 0, "...and prints nothing, rather than claiming to have won");
}

// N7: two-phase CLI apply keeps explicit override even when it matches the old tier's value.
// Single merged call would re-derive the new tier's value, discarding the explicit ask.
static void testN7CliPhaseOrderThroughTheSingleton() {
    AVER_INFO("=== N7: the two-phase CLI apply keeps --rt-rays 8 against --rt 1 ===");
    voxi::Renderer& vx = voxi::Renderer::get();
    vx.setDeviceInfo(fullyCapableDevice());

    voxi::Settings seed{};
    seed.rayTracing   = voxi::Quality::Epic;
    seed.rtShadowRays = 8;
    vx.setSettings(seed);
    const voxi::Settings live = vx.settings();
    check(live.rayTracing == voxi::Quality::Epic && live.rtShadowRays == 8,
          "singleton seeded at RT Epic, rtShadowRays 8");

    voxi::RenderCliOverrides cli;
    cli.rayTracing   = static_cast<int>(voxi::Quality::Low);
    cli.rtShadowRays = 8;

    NoteCounter notes;
    voxi::Settings s = vx.settings();
    voxi::applyCliOverrides(cli, s, notes, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    check(s.rayTracing == voxi::Quality::Low, "two-phase: --rt 1 took the tier to Low");
    check(s.rtShadowRays == 8,
          "two-phase: --rt-rays 8 survives even though Low's ladder rung is 1");
    check(vx.settings().rtShadowRays == 8, "...and the singleton itself agrees");
    check(notes.total == 2,
          "both flags were SEEN: Phase B compared --rt-rays against Low's derived 1, not Epic's 8");

    // Single merged call would re-derive Low's value: proven by applying both phases to one buffer.
    vx.setSettings(live);
    NoteCounter mergedNotes;
    voxi::Settings merged = vx.settings();
    voxi::applyCliTiers(cli, merged, mergedNotes);
    voxi::applyCliKnobs(cli, merged, mergedNotes);
    check(mergedNotes.total == 1,
          "merged: --rt-rays 8 read against the OLD tier's live 8 looks like a no-op and logs nothing");
    check(merged.rtShadowRays == 8,
          "merged: the buffer still reads 8, but only because nothing ever wrote it");
    vx.setSettings(merged);
    check(vx.settings().rtShadowRays == 1,
          "...and the single commit's own tier-derivation replaces it with Low's 1 -- the exact "
          "regression the Phase A/B split exists to avoid");
}

// ---- captureVoxiSettings ----

// Tier change with unedited knob: follows new tier's ladder (-1).
static void testCaptureTierChangeUneditedKnobFollows() {
    AVER_INFO("=== capture: a tier change with the knob left unedited follows the new tier (-1) ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Low;
    live.giCones = voxi::ladder::giCones(voxi::Quality::Low);

    voxi::Settings requested = live;
    requested.globalIllumination = voxi::Quality::Epic;

    fmt::ProjectDesc p;
    p.giCones = 7;
    voxi::captureVoxiSettings(p, requested, live, d, /*overallFollowMask=*/0);
    check(p.giCones == -1, "a tier change with the knob unedited clears the pin back to -1");
}

// Overall-follow bit forces -1 regardless of knob values.
static void testCaptureOverallMaskWins() {
    AVER_INFO("=== capture: an Overall-follow bit forces -1 regardless of the knob values ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.rayTracing = voxi::Quality::Medium;
    live.rtShadowRays = 1;

    voxi::Settings requested = live;
    requested.rtShadowRays = 4;

    fmt::ProjectDesc p;
    p.rtShadowRays = 1;
    const u32 mask = 1u << static_cast<u32>(voxi::ScalabilityGroup::RayTracing);
    voxi::captureVoxiSettings(p, requested, live, d, mask);
    check(p.rtShadowRays == -1,
          "...but the RT group's Overall-follow bit wins over the apparent edit -- an Overall preset "
          "just wrote this knob itself, so the manifest goes back to following the tier");
}

// Unedited knob at unchanged tier: manifest stays exactly as it was.
static void testCaptureUneditedKnobLeavesManifestAlone() {
    AVER_INFO("=== capture: an unedited knob at an unchanged tier leaves the manifest exactly as it was ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giCones = 6;
    const voxi::Settings requested = live;

    {
        fmt::ProjectDesc p;
        p.giCones = -1;
        voxi::captureVoxiSettings(p, requested, live, d, 0);
        check(p.giCones == -1, "an already-absent knob with nothing edited stays absent");
    }
    {
        fmt::ProjectDesc p;
        p.giCones = 7;
        voxi::captureVoxiSettings(p, requested, live, d, 0);
        check(p.giCones == 7, "an existing pin with nothing edited stays exactly as pinned");
    }
}

// Explicit edit at unchanged tier: captured verbatim.
static void testCaptureExplicitEditIsPinned() {
    AVER_INFO("=== capture: an explicit edit at an unchanged tier is captured verbatim ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giCones = 6;
    voxi::Settings requested = live;
    requested.giCones = 9;

    fmt::ProjectDesc p;
    p.giCones = -1;
    voxi::captureVoxiSettings(p, requested, live, d, 0);
    check(p.giCones == 9, "an edited giCones 9 is captured as 9");
}

// giRestirVisibility: tier change unedited knob follows new tier (-1).
static void testCaptureRestirVisibilityTierChangeUneditedKnobFollows() {
    AVER_INFO("=== capture: a GI tier change with RESTIRVISIBILITY unedited follows the new tier (-1) ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Low;
    live.giRestirVisibility = voxi::ladder::giRestirVisibility(voxi::Quality::Low);

    voxi::Settings requested = live;
    requested.globalIllumination = voxi::Quality::Epic;

    fmt::ProjectDesc p;
    p.restirVisibility = 1;
    voxi::captureVoxiSettings(p, requested, live, d, /*overallFollowMask=*/0);
    check(p.restirVisibility == -1, "a GI tier change with RESTIRVISIBILITY unedited clears the pin to -1");
}

// giRestirVisibility: Overall-follow bit forces -1 regardless of value.
static void testCaptureRestirVisibilityOverallMaskWins() {
    AVER_INFO("=== capture: an Overall-follow bit forces RESTIRVISIBILITY back to -1 regardless of value ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giRestirVisibility = 2;

    voxi::Settings requested = live;
    requested.giRestirVisibility = 3;

    fmt::ProjectDesc p;
    p.restirVisibility = 2;
    const u32 mask = 1u << static_cast<u32>(voxi::ScalabilityGroup::GlobalIllumination);
    voxi::captureVoxiSettings(p, requested, live, d, mask);
    check(p.restirVisibility == -1,
          "...but the GI group's Overall-follow bit wins over the apparent edit, exactly as it does for "
          "giCones");
}

// giRestirVisibility: explicit edit at unchanged tier captured verbatim.
static void testCaptureRestirVisibilityExplicitEditIsPinned() {
    AVER_INFO("=== capture: an explicit RESTIRVISIBILITY edit at an unchanged tier is captured verbatim ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giRestirVisibility = 2;
    voxi::Settings requested = live;
    requested.giRestirVisibility = 0;

    fmt::ProjectDesc p;
    p.restirVisibility = -1;
    voxi::captureVoxiSettings(p, requested, live, d, 0);
    check(p.restirVisibility == 0, "an edited giRestirVisibility 0 is captured as 0, not dropped as falsy");
}

// N5: no-RT device's own clamp must not overwrite a teammate's RT/mesh-shader pin.
static void testN5NoRtDeviceLeavesManifestAlone() {
    AVER_INFO("=== N5: a no-RT device's own clamp does not overwrite a teammate's RT/mesh-shader pin ===");
    const voxi::DeviceInfo d = noRtDevice();

    voxi::Settings live{};
    live.rayTracing   = voxi::Quality::Off;   // already clamped
    live.rtShadowRays = 4;                     // range-clamped only
    live.meshShaders  = false;                 // already clamped

    const voxi::Settings requested = live;

    fmt::ProjectDesc p;
    p.rayTracing   = 4;
    p.rtShadowRays = 4;
    p.meshShaders  = 1;

    voxi::captureVoxiSettings(p, requested, live, d, 0);
    check(p.rayTracing == 4, "RAYTRACING stays 4 -- this device's Off clamp is not read as a user choice");
    check(p.rtShadowRays == 4, "RTSHADOWRAYS stays 4 -- unedited, and the RT tier did not change either");
    check(p.meshShaders == 1, "MESHSHADERS stays 1 for the same reason as RAYTRACING");
}

// manifestContradictions: RT Off turns GIMODE/RTRENDERMODE pins into reports; unset asks (-1) unchecked.
static void testManifestContradictions() {
    AVER_INFO("=== manifestContradictions: RT Off turns GIMODE/RTRENDERMODE pins into reports ===");
    const voxi::DeviceInfo d = fullyCapableDevice();
    voxi::Settings s{};
    s.rayTracing = voxi::Quality::Off;
    s.globalIllumination = voxi::Quality::Medium;

    voxi::ManifestAsks asks;
    asks.giMode = 1;
    asks.rtRenderMode = 1;
    voxi::FieldReport reports[4];
    u32 n = voxi::manifestContradictions(s, d, asks, reports);
    check(n == 2, "GIMODE 1 and RTRENDERMODE 1 both contradict RT Off -- two reports");

    asks.giMode = -1;
    n = voxi::manifestContradictions(s, d, asks, reports);
    check(n == 1, "with GIMODE unset, only the RTRENDERMODE contradiction remains");
}

// N9: hasRenderSettings sees GIMODE and DENOISER alone (was blind to both).
static void testHasRenderSettingsSeesGiModeAndDenoiser() {
    AVER_INFO("=== N9: hasRenderSettings sees a manifest stating only GIMODE or DENOISER ===");
    fmt::ProjectDesc p;
    check(!p.hasRenderSettings(), "a default-constructed ProjectDesc states nothing");

    p.giMode = 1;
    check(p.hasRenderSettings(), "GIMODE alone is now enough to trip hasRenderSettings");

    fmt::ProjectDesc p2;
    p2.denoiser = 1;
    check(p2.hasRenderSettings(), "DENOISER alone is also enough");
}

// hasRenderSettings: RESTIRVISIBILITY and AVERSR at 0 are legitimate (not just >= 0 truthy check).
static void testHasRenderSettingsSeesRestirVisibilityAndAverSr() {
    AVER_INFO("=== hasRenderSettings sees a manifest stating only RESTIRVISIBILITY or only AVERSR, even at 0 ===");
    fmt::ProjectDesc p;
    p.restirVisibility = 0;
    check(p.hasRenderSettings(), "RESTIRVISIBILITY 0 alone is enough to trip hasRenderSettings");

    fmt::ProjectDesc p2;
    p2.averSr = 0;
    check(p2.hasRenderSettings(), "AVERSR 0 alone is also enough");
}

// Round-trip: RESTIRVISIBILITY and AVERSR write, parse, and rewrite without duplication.
// isOwnedKey check prevents keys appended to kOwned but missing from isOwnedKey's array (copy-through + re-emit = duplicate).
static void testRestirVisibilityAndAverSrRoundTrip() {
    AVER_INFO("=== round-trip: RENDER.RESTIRVISIBILITY and RENDER.AVERSR write, parse and rewrite cleanly ===");
    fmt::ProjectDesc p;
    p.name             = "RoundTrip";
    p.restirVisibility = 2;
    p.averSr           = 1;

    const std::string first = fmt::writeOcproject(p, "");
    check(first.find("RENDER.RESTIRVISIBILITY 2") != std::string::npos, "RENDER.RESTIRVISIBILITY 2 is written");
    check(first.find("RENDER.AVERSR 1") != std::string::npos, "RENDER.AVERSR 1 is written");

    fmt::ProjectDesc reparsed;
    std::string err;
    check(fmt::parseOcproject(first, reparsed, &err), "the written manifest parses back: " + err);
    check(reparsed.restirVisibility == 2, "RESTIRVISIBILITY round-trips through parse");
    check(reparsed.averSr == 1, "AVERSR round-trips through parse");

    // Rewrite against existing manifest: the isOwnedKey path that could duplicate keys.
    const std::string second = fmt::writeOcproject(reparsed, first);
    auto countOccurrences = [](const std::string& haystack, const std::string& needle) {
        int n = 0;
        for (usize pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
        return n;
    };
    check(countOccurrences(second, "RENDER.RESTIRVISIBILITY") == 1,
          "RENDER.RESTIRVISIBILITY appears exactly once after a rewrite, not duplicated");
    check(countOccurrences(second, "RENDER.AVERSR") == 1,
          "RENDER.AVERSR appears exactly once after a rewrite, not duplicated");

    // FRAMEINTERP: same device-level shape as AVERSR.
    fmt::ProjectDesc fg;
    fg.name = "RoundTrip";
    fg.frameInterp = 1;
    const std::string fgFirst = fmt::writeOcproject(fg, "");
    check(fgFirst.find("RENDER.FRAMEINTERP 1") != std::string::npos, "RENDER.FRAMEINTERP 1 is written");
    fmt::ProjectDesc fgBack;
    check(fmt::parseOcproject(fgFirst, fgBack, &err), "the FRAMEINTERP manifest parses back: " + err);
    check(fgBack.frameInterp == 1, "FRAMEINTERP round-trips through parse");
    check(countOccurrences(fmt::writeOcproject(fgBack, fgFirst), "RENDER.FRAMEINTERP") == 1,
          "RENDER.FRAMEINTERP appears exactly once after a rewrite, not duplicated");
    check(first.find("RENDER.FRAMEINTERP") == std::string::npos, "an unset FRAMEINTERP writes no line");
}

// ---- RENDER.RESTIRHISTORY: NOT tier-derived, unlike RESTIRVISIBILITY ----
// Absent leaves Settings alone (not ladder). Every legal value applies. Capture unconditional. Writes once, no duplicate.

// Absent RESTIRHISTORY leaves Settings::giRestirMaxHistory alone even across a tier change.
static void testRestirHistoryAbsentLeavesSettingsAlone() {
    AVER_INFO("=== RESTIRHISTORY is NOT tier-derived: absent leaves Settings::giRestirMaxHistory alone ===");
    voxi::Settings s{};
    s.globalIllumination = voxi::Quality::Low;
    s.giRestirMaxHistory = 5;

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Epic);

    voxi::applyManifestTiers(p, s);
    voxi::applyManifestKnobs(p, s);
    check(s.giRestirMaxHistory == 5,
          "an absent RESTIRHISTORY leaves giRestirMaxHistory at 5 even though the GI tier just changed");
}

// Every legal value (0..8, the UI slider range) applies verbatim.
static void testRestirHistoryEveryLegalValueApplies() {
    AVER_INFO("=== every legal RENDER.RESTIRHISTORY value (0..8) applies to Settings::giRestirMaxHistory ===");
    for (int v = 0; v <= 8; ++v) {
        fmt::ProjectDesc p;
        p.restirHistory = v;
        voxi::Settings s{};
        voxi::applyManifestTiers(p, s);
        voxi::applyManifestKnobs(p, s);
        check(static_cast<int>(s.giRestirMaxHistory) == v,
              "RENDER.RESTIRHISTORY " + std::to_string(v) + " -> Settings::giRestirMaxHistory");
    }
}

// Capture: RESTIRHISTORY unconditional, not tier-aware like RESTIRVISIBILITY.
static void testCaptureRestirHistoryIsUnconditional() {
    AVER_INFO("=== capture: RESTIRHISTORY writes the requested value unconditionally, unlike RESTIRVISIBILITY ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    {
        // GI tier change, knob unedited: tier-derived would reset to -1.
        voxi::Settings live{};
        live.globalIllumination = voxi::Quality::Low;
        live.giRestirMaxHistory = 3;
        voxi::Settings requested = live;
        requested.globalIllumination = voxi::Quality::Epic;

        fmt::ProjectDesc p;
        p.restirHistory = -1;
        voxi::captureVoxiSettings(p, requested, live, d, /*overallFollowMask=*/0);
        check(p.restirHistory == 3,
              "a GI tier change with RESTIRHISTORY unedited still writes 3, not -1 the way a tier-derived knob would");
    }
    {
        // Overall-follow bit on GI group: tier-derived would force -1.
        voxi::Settings live{};
        live.globalIllumination = voxi::Quality::Medium;
        live.giRestirMaxHistory = 1;
        voxi::Settings requested = live;
        requested.giRestirMaxHistory = 4;

        fmt::ProjectDesc p;
        p.restirHistory = 1;
        const u32 mask = 1u << static_cast<u32>(voxi::ScalabilityGroup::GlobalIllumination);
        voxi::captureVoxiSettings(p, requested, live, d, mask);
        check(p.restirHistory == 4,
              "the GI group's Overall-follow bit is ignored -- RESTIRHISTORY still captures the requested 4");
    }
    {
        // Nothing edited: unconditional still writes the (unchanged) requested value.
        voxi::Settings live{};
        live.giRestirMaxHistory = 0;
        const voxi::Settings requested = live;

        fmt::ProjectDesc p;
        p.restirHistory = 6;
        voxi::captureVoxiSettings(p, requested, live, d, 0);
        check(p.restirHistory == 0,
              "nothing edited: RESTIRHISTORY still writes the (unchanged) requested 0 over the stale pin 6");
    }
}

// Round-trip: RESTIRHISTORY writes, parses, rewrites without duplication.
static void testRestirHistoryRoundTrip() {
    AVER_INFO("=== round-trip: RENDER.RESTIRHISTORY writes, parses and rewrites cleanly ===");
    fmt::ProjectDesc p;
    p.name          = "RoundTrip";
    p.restirHistory = 4;

    const std::string first = fmt::writeOcproject(p, "");
    check(first.find("RENDER.RESTIRHISTORY 4") != std::string::npos, "RENDER.RESTIRHISTORY 4 is written");

    fmt::ProjectDesc reparsed;
    std::string err;
    check(fmt::parseOcproject(first, reparsed, &err), "the written manifest parses back: " + err);
    check(reparsed.restirHistory == 4, "RESTIRHISTORY round-trips through parse");

    const std::string second = fmt::writeOcproject(reparsed, first);
    auto countOccurrences = [](const std::string& haystack, const std::string& needle) {
        int n = 0;
        for (usize pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
        return n;
    };
    check(countOccurrences(second, "RENDER.RESTIRHISTORY") == 1,
          "RENDER.RESTIRHISTORY appears exactly once after a rewrite, not duplicated");

    fmt::ProjectDesc absent;
    absent.name = "NoHistory";
    const std::string noLine = fmt::writeOcproject(absent, "");
    check(noLine.find("RENDER.RESTIRHISTORY") == std::string::npos,
          "an absent (-1) RESTIRHISTORY writes no RENDER.RESTIRHISTORY line at all");
}

int main() {
    testEveryKeyReachesSettings();
    testN6AbsentDerivedKnobResetsToLadder();
    testN6RestirVisibilityFollowsTier();
    testEmptyManifestLeavesSettingsUnchanged();
    testR2RegressionThroughTheSingleton();
    testR2RestirVisibilityRaceThroughTheSingleton();
    testCliEveryFlagReachesItsField();
    testCliAbsentOverridesLeaveSettingsAlone();
    testCliForceOffIsAPhaseATierChange();
    testN7CliPhaseOrderThroughTheSingleton();
    testCaptureTierChangeUneditedKnobFollows();
    testCaptureOverallMaskWins();
    testCaptureUneditedKnobLeavesManifestAlone();
    testCaptureExplicitEditIsPinned();
    testCaptureRestirVisibilityTierChangeUneditedKnobFollows();
    testCaptureRestirVisibilityOverallMaskWins();
    testCaptureRestirVisibilityExplicitEditIsPinned();
    testN5NoRtDeviceLeavesManifestAlone();
    testManifestContradictions();
    testHasRenderSettingsSeesGiModeAndDenoiser();
    testHasRenderSettingsSeesRestirVisibilityAndAverSr();
    testRestirVisibilityAndAverSrRoundTrip();
    testRestirHistoryAbsentLeavesSettingsAlone();
    testRestirHistoryEveryLegalValueApplies();
    testCaptureRestirHistoryIsUnconditional();
    testRestirHistoryRoundTrip();

    AVER_INFO("==================================================");
    AVER_INFO("ProjectRenderApply tests done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
