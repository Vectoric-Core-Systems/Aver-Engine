// ProjectRenderApply.hpp: the shared, header-only .ocproject -> voxi::Settings apply and the
// Settings -> .ocproject capture rule, both pure functions of Settings/DeviceInfo/ProjectDesc values
// (see that header's own top comment for why). Covers every RENDER.* key reaching its Settings field,
// the N6 fix (an absent derived knob resets to its tier's ladder value rather than a stale live one),
// the R2 regression the two-phase apply exists to avoid (proven THROUGH the live voxi::Renderer
// singleton, not just the pure helpers), the capture rule's four cases, N5 (a no-RT device's own
// clamp must not be mistaken for a user's edit), manifestContradictions, and N9 (hasRenderSettings
// seeing GIMODE/DENOISER). Compiled by the build; NEVER run from this workflow.
#include "aver/voxi/ProjectRenderApply.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures -- same idiom as tests/formats/src/OcInputTest.cpp.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A device with every gate this header's fields can ask about satisfied: compute shaders, RT
// hardware, mesh shaders, every MSAA count, NRD. Most tests below do not care WHICH device gates a
// field -- they exercise the manifest-apply and capture arithmetic, not the prerequisite table
// (RenderSettingsResolverTest.cpp, Lane 1, owns that) -- so one fully-capable device covers them.
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
    d.nrdSupported = true;
    return d;
}

// N5's own device: compute shaders only -- GI runs, but ray tracing and mesh shaders are both
// unsupported, the same shape a teammate's machine without an RT card actually has.
static voxi::DeviceInfo noRtDevice() {
    voxi::DeviceInfo d;
    d.msaaMask = 1u | 2u | 4u | 8u;
    d.maxMsaaSamples = 8;
    d.rayTracingTier = 0;
    d.computeShaders = true;
    d.shaderModel = 60;
    d.meshShaderTier = 0;
    d.dxcAvailable = true;
    d.nrdSupported = false;
    return d;
}

// Field-by-field equality, rather than memcmp -- Settings carries padding bytes memcmp would trip
// over for no reason this test cares about, and this reads as a list of what "unchanged" means.
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
           near(a.reblurDiffusePrepassBlurRadius, b.reblurDiffusePrepassBlurRadius) &&
           a.reblurMaxAccumulatedFrameNum == b.reblurMaxAccumulatedFrameNum &&
           a.reblurMaxStabilizedFrameNum == b.reblurMaxStabilizedFrameNum &&
           a.rtShadowDenoise == b.rtShadowDenoise &&
           a.rtRenderMode == b.rtRenderMode &&
           a.ptBounces == b.ptBounces;
}

// Every RENDER.* key that maps to a voxi::Settings field, set to a distinct non-default value, reaches
// it through applyManifestTiers/applyManifestKnobs.
//
// THE 4 TIER KEYS: GI, RAYTRACING, PATHTRACING, LAYEREDBSDF.
//
// THE 17 KNOB KEYS -- not 19: SandboxApp.cpp's applyProjectVoxiSettings (the function this header's
// applyManifestKnobs is copied from) carries exactly 17 Settings-backed knobs in its second
// setSettings call. RENDER.LODSELECT/LODTHRESHOLD/OCCLUSIONCULL/DEPTHPREPASS are real ProjectDesc
// keys but apply to editor-side flags OUTSIDE voxi::Settings entirely (SandboxApp.cpp's
// applyProjectRenderSettings applies them directly, never through voxi::Renderer) -- this header does
// not touch them, so they are not counted here.
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

        // Tiers left absent -- s stays Medium/Medium/Off, Settings{}'s own defaults -- so every
        // knob's landing value below is unambiguously the manifest's explicit ask, not a ladder
        // fallback (all 17 keys here are STATED, so the N6 else-branches never fire).
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
    }
}

// N6: a derived knob the manifest does not state resets to its NEW tier's ladder value, not whatever
// was already live -- even when the tier itself did not change in this same apply.
static void testN6AbsentDerivedKnobResetsToLadder() {
    AVER_INFO("=== N6: an absent derived knob resets to its tier's ladder value ===");
    voxi::Settings s{};
    s.globalIllumination = voxi::Quality::Epic;
    s.giCones = 7;   // stale -- a previous project's pin that happens to still be live

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Epic);   // restates the SAME tier -- no tier change
    // p.giCones intentionally left at its default (-1, absent)

    voxi::applyManifestTiers(p, s);
    check(s.giCones == 7, "restating the same tier alone does not touch the stale knob");
    voxi::applyManifestKnobs(p, s);
    check(s.giCones == voxi::ladder::giCones(voxi::Quality::Epic),
          "an absent GICONES resets to ladder::giCones(Epic) = 13, not the stale 7");
}

// An empty manifest is a genuine no-op against Settings{} -- the sanity check ProjectRenderApply.hpp's
// own comment makes: Settings{}'s defaults already equal ladder(Medium)/ladder(Off) per group (Lane
// 1's static_asserts, QualityLadder.hpp), so N6's unconditional reset lands on the same values.
static void testEmptyManifestLeavesSettingsUnchanged() {
    AVER_INFO("=== ProjectDesc{} (empty manifest) leaves Settings{} completely unchanged ===");
    fmt::ProjectDesc empty;
    check(!empty.hasRenderSettings(), "a default-constructed ProjectDesc states no RENDER.* key");

    voxi::Settings s{};
    const voxi::Settings before = s;
    voxi::applyManifestTiers(empty, s);
    voxi::applyManifestKnobs(empty, s);
    check(settingsEqual(s, before), "applying an empty manifest to Settings{} changes nothing");
}

// R2, THROUGH THE LIVE SINGLETON: live GI Medium (giCones 6, Medium's own ladder rung); a manifest
// states GI Epic plus GICONES 6 -- the SAME number Medium already had. Two-phase gives 6, exactly what
// the manifest asked for; a merged single call would have silently re-derived Epic's 13 over it,
// because 6-arriving-unchanged-from-live is indistinguishable from 6-never-asked-for to setSettings'
// own change-gated derivation. See ProjectRenderApply.hpp's own applyManifestTwoPhase comment for the
// full mechanism.
static void testR2RegressionThroughTheSingleton() {
    AVER_INFO("=== R2: two-phase through voxi::Renderer avoids the merged-call regression ===");
    voxi::Renderer& vx = voxi::Renderer::get();
    vx.setDeviceInfo(fullyCapableDevice());

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;   // giCones defaults to 6, Medium's own rung
    vx.setSettings(live);
    check(vx.settings().globalIllumination == voxi::Quality::Medium && vx.settings().giCones == 6,
          "singleton seeded at GI Medium, giCones 6");

    fmt::ProjectDesc p;
    p.giQuality = static_cast<int>(voxi::Quality::Epic);   // RENDER.GI 4
    p.giCones   = 6;                                        // RENDER.GICONES 6, explicit

    voxi::Settings s = vx.settings();
    voxi::applyManifestTwoPhase(p, s, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    check(s.giCones == 6, "two-phase apply: GICONES 6 survives even though Epic's ladder rung is 13");
    check(vx.settings().giCones == 6, "...and the singleton itself agrees");

    // A MERGED SINGLE CALL WOULD HAVE GIVEN 13 -- proven, not merely asserted in a comment: reset the
    // singleton to the same pre-edit live state, then apply both phases into ONE local Settings before
    // ever calling setSettings, and commit once.
    vx.setSettings(live);
    voxi::Settings merged = vx.settings();
    voxi::applyManifestTiers(p, merged);
    voxi::applyManifestKnobs(p, merged);   // giCones already forced to 6 here too (p.giCones is stated)
    check(merged.giCones == 6, "before the single commit, the merged buffer also holds the explicit 6");
    vx.setSettings(merged);
    check(vx.settings().giCones == 13,
          "...but the single commit's own change-gated derivation overwrites it with Epic's 13 -- "
          "the exact regression two-phase (above) avoids");
}

// ---- captureVoxiSettings ----

static void testCaptureTierChangeUneditedKnobFollows() {
    AVER_INFO("=== capture: a tier change with the knob left unedited follows the new tier (-1) ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Low;
    live.giCones = voxi::ladder::giCones(voxi::Quality::Low);   // 3, following Low's own rung

    voxi::Settings requested = live;
    requested.globalIllumination = voxi::Quality::Epic;   // GI 2 -> 4
    // giCones left completely unedited here -- still Low's 3, not Epic's 13

    fmt::ProjectDesc p;
    p.giCones = 7;   // an existing pin -- must be cleared back to -1 by this rule
    voxi::captureVoxiSettings(p, requested, live, d, /*overallFollowMask=*/0);
    check(p.giCones == -1, "a tier change with the knob unedited clears the pin back to -1");
}

static void testCaptureOverallMaskWins() {
    AVER_INFO("=== capture: an Overall-follow bit forces -1 regardless of the knob values ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.rayTracing = voxi::Quality::Medium;
    live.rtShadowRays = 1;

    voxi::Settings requested = live;
    requested.rtShadowRays = 4;   // looks like an explicit edit...

    fmt::ProjectDesc p;
    p.rtShadowRays = 1;
    const u32 mask = 1u << static_cast<u32>(voxi::ScalabilityGroup::RayTracing);
    voxi::captureVoxiSettings(p, requested, live, d, mask);
    check(p.rtShadowRays == -1,
          "...but the RT group's Overall-follow bit wins over the apparent edit -- an Overall preset "
          "just wrote this knob itself, so the manifest goes back to following the tier");
}

static void testCaptureUneditedKnobLeavesManifestAlone() {
    AVER_INFO("=== capture: an unedited knob at an unchanged tier leaves the manifest exactly as it was ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giCones = 6;
    const voxi::Settings requested = live;   // nothing changed at all

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

static void testCaptureExplicitEditIsPinned() {
    AVER_INFO("=== capture: an explicit edit at an unchanged tier is captured verbatim ===");
    const voxi::DeviceInfo d = fullyCapableDevice();

    voxi::Settings live{};
    live.globalIllumination = voxi::Quality::Medium;
    live.giCones = 6;
    voxi::Settings requested = live;
    requested.giCones = 9;   // tier unchanged, knob edited

    fmt::ProjectDesc p;
    p.giCones = -1;
    voxi::captureVoxiSettings(p, requested, live, d, 0);
    check(p.giCones == 9, "an edited giCones 9 is captured as 9");
}

// N5: on a device that cannot run RT or mesh shaders at all, requested == live for those fields
// (setSettings already clamped both to Off/false, every apply) must not be read as "the user
// reaffirmed this value" -- an unrelated edit on this machine must not overwrite a teammate's pin for
// hardware this machine simply does not have.
static void testN5NoRtDeviceLeavesManifestAlone() {
    AVER_INFO("=== N5: a no-RT device's own clamp does not overwrite a teammate's RT/mesh-shader pin ===");
    const voxi::DeviceInfo d = noRtDevice();

    voxi::Settings live{};
    live.rayTracing   = voxi::Quality::Off;   // already clamped by this device
    live.rtShadowRays = 4;                     // range-clamped only, not device-clamped
    live.meshShaders  = false;                 // already clamped by this device

    const voxi::Settings requested = live;   // an unrelated edit -- nothing about RT/mesh changed

    fmt::ProjectDesc p;
    p.rayTracing   = 4;   // RENDER.RAYTRACING 4 -- a teammate's pin for hardware this machine lacks
    p.rtShadowRays = 4;   // RENDER.RTSHADOWRAYS 4
    p.meshShaders  = 1;   // RENDER.MESHSHADERS 1

    voxi::captureVoxiSettings(p, requested, live, d, 0);
    check(p.rayTracing == 4, "RAYTRACING stays 4 -- this device's Off clamp is not read as a user choice");
    check(p.rtShadowRays == 4, "RTSHADOWRAYS stays 4 -- unedited, and the RT tier did not change either");
    check(p.meshShaders == 1, "MESHSHADERS stays 1 for the same reason as RAYTRACING");
}

// manifestContradictions: RT tier Off makes a GIMODE/RTRENDERMODE pin into a reportable contradiction;
// an unset ask (-1) is simply not checked.
static void testManifestContradictions() {
    AVER_INFO("=== manifestContradictions: RT Off turns GIMODE/RTRENDERMODE pins into reports ===");
    const voxi::DeviceInfo d = fullyCapableDevice();   // HAS RT hardware...
    voxi::Settings s{};
    s.rayTracing = voxi::Quality::Off;                  // ...but this project's RT tier is Off
    s.globalIllumination = voxi::Quality::Medium;

    voxi::ManifestAsks asks;
    asks.giMode = 1;
    asks.rtRenderMode = 1;
    voxi::FieldReport reports[4];
    u32 n = voxi::manifestContradictions(s, d, asks, reports);
    check(n == 2, "GIMODE 1 and RTRENDERMODE 1 both contradict RT Off -- two reports");

    asks.giMode = -1;   // unset
    n = voxi::manifestContradictions(s, d, asks, reports);
    check(n == 1, "with GIMODE unset, only the RTRENDERMODE contradiction remains");
}

// N9: hasRenderSettings used to be blind to a manifest stating only GIMODE or only DENOISER -- both
// loaders' whole apply block was gated on this one predicate, so either key alone was silently ignored.
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

int main() {
    testEveryKeyReachesSettings();
    testN6AbsentDerivedKnobResetsToLadder();
    testEmptyManifestLeavesSettingsUnchanged();
    testR2RegressionThroughTheSingleton();
    testCaptureTierChangeUneditedKnobFollows();
    testCaptureOverallMaskWins();
    testCaptureUneditedKnobLeavesManifestAlone();
    testCaptureExplicitEditIsPinned();
    testN5NoRtDeviceLeavesManifestAlone();
    testManifestContradictions();
    testHasRenderSettingsSeesGiModeAndDenoiser();

    AVER_INFO("==================================================");
    AVER_INFO("ProjectRenderApply tests done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
