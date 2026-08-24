// applyUnderwaterFog: a pure function over rhi::SkyAtmosphere, so this is a CPU-only test -- no
// device, no window, no GPU. It constructs SkyAtmosphere by value (a plain struct with in-class
// defaults) and calls the function directly, exactly as Underwater.hpp's own header comment
// describes the module boundary.
#include "aver/fluids/Underwater.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A SkyAtmosphere with fog fields moved away from their in-class defaults (and one unrelated field,
// sunIntensity, moved too), so that a test which finds the tuning's values or the authored values
// unchanged is actually distinguishing them rather than coincidentally matching a default.
//
// Zero-BRACE-initialised first, matching the pattern VoxiRenderer.cpp's giSky_ comparison documents
// at length (modules/render.voxi/src/VoxiRenderer.cpp:778-786): SkyAtmosphere opens with a bool
// followed by padding, so any test that later memcmp's a whole SkyAtmosphere needs both sides to
// have started from the same zeroed padding, or the comparison is comparing unspecified bytes.
static rhi::SkyAtmosphere makeAuthored() {
    rhi::SkyAtmosphere a{};
    a.enabled = true;
    a.fogColor[0] = 0.85f;
    a.fogColor[1] = 0.90f;
    a.fogColor[2] = 0.95f;
    a.fogDensity    = 4e-6f;
    a.fogHeight     = 12000.0f;
    a.fogFalloff    = 0.3f;
    a.fogStart      = 500.0f;
    a.fogMaxOpacity = 0.8f;
    a.sunIntensity  = 3.0f;   // applyUnderwaterFog must never touch this -- it is not a fog field
    return a;
}

// Case 1: well above the fade band is the identity path. Whole-struct memcmp is deliberate here
// (rather than field-by-field) -- it is the strongest statement of "untouched" available, and it is
// safe specifically because both `authored` and `result` were zero-brace-initialised before any
// field was set, so padding matches on both sides per the VoxiRenderer precedent cited above.
static void testAboveWaterIsIdentity() {
    const rhi::SkyAtmosphere authored = makeAuthored();
    const fluids::UnderwaterFogTuning tuning{};
    const f32 waterLevelCm = 0.0f;
    const f32 cameraZCm = waterLevelCm + tuning.fadeBandCm + 10000.0f;   // well clear of the band

    rhi::SkyAtmosphere result{};
    result = fluids::applyUnderwaterFog(authored, cameraZCm, waterLevelCm, tuning);

    check(std::memcmp(&result, &authored, sizeof(rhi::SkyAtmosphere)) == 0,
          "well above the fade band returns the authored SkyAtmosphere untouched, byte for byte");
}

// Case 2: well below the fade band is a full override of exactly the six fog fields. fogHeight is
// checked against waterLevelCm rather than against a tuning field, because UnderwaterFogTuning has
// no height field on purpose -- see the anchoring rationale in Underwater.hpp's own comment.
static void testWellBelowIsFullOverride() {
    const rhi::SkyAtmosphere authored = makeAuthored();
    const fluids::UnderwaterFogTuning tuning{};
    const f32 waterLevelCm = 0.0f;
    const f32 cameraZCm = waterLevelCm - tuning.fadeBandCm - 10000.0f;   // well clear of the band

    const rhi::SkyAtmosphere result = fluids::applyUnderwaterFog(authored, cameraZCm, waterLevelCm, tuning);

    check(near(result.fogColor[0], tuning.color[0]) && near(result.fogColor[1], tuning.color[1]) &&
              near(result.fogColor[2], tuning.color[2]),
          "well below the fade band, fogColor equals tuning.color exactly");
    check(near(result.fogDensity, tuning.densityPerCm), "...fogDensity equals tuning.densityPerCm exactly");
    check(near(result.fogHeight, waterLevelCm), "...fogHeight is anchored at waterLevelCm exactly");
    check(near(result.fogFalloff, tuning.falloff), "...fogFalloff equals tuning.falloff exactly");
    check(near(result.fogStart, tuning.startCm), "...fogStart equals tuning.startCm exactly");
    check(near(result.fogMaxOpacity, tuning.maxOpacity), "...fogMaxOpacity equals tuning.maxOpacity exactly");
    check(near(result.sunIntensity, authored.sunIntensity),
          "...and a non-fog field (sunIntensity) is left exactly as authored");
}

// Case 3: cameraZCm == waterLevelCm must land exactly halfway between authored and tuning for every
// one of the six fog fields -- proof the blend is centred on the surface, not offset toward either
// side. waterLevelCm is deliberately nonzero here, so this also exercises the offset term in the
// blend fraction rather than only the degenerate waterLevelCm==0 case.
static void testAtSurfaceIsExactlyHalfway() {
    const rhi::SkyAtmosphere authored = makeAuthored();
    const fluids::UnderwaterFogTuning tuning{};
    const f32 waterLevelCm = 500.0f;
    const f32 cameraZCm = waterLevelCm;

    const rhi::SkyAtmosphere result = fluids::applyUnderwaterFog(authored, cameraZCm, waterLevelCm, tuning);

    check(near(result.fogColor[0], (authored.fogColor[0] + tuning.color[0]) * 0.5f), "at the surface, fogColor.r is exactly halfway");
    check(near(result.fogColor[1], (authored.fogColor[1] + tuning.color[1]) * 0.5f), "...fogColor.g is exactly halfway");
    check(near(result.fogColor[2], (authored.fogColor[2] + tuning.color[2]) * 0.5f), "...fogColor.b is exactly halfway");
    check(near(result.fogDensity, (authored.fogDensity + tuning.densityPerCm) * 0.5f), "...fogDensity is exactly halfway");
    check(near(result.fogHeight, (authored.fogHeight + waterLevelCm) * 0.5f), "...fogHeight is exactly halfway toward waterLevelCm");
    check(near(result.fogFalloff, (authored.fogFalloff + tuning.falloff) * 0.5f), "...fogFalloff is exactly halfway");
    check(near(result.fogStart, (authored.fogStart + tuning.startCm) * 0.5f), "...fogStart is exactly halfway");
    check(near(result.fogMaxOpacity, (authored.fogMaxOpacity + tuning.maxOpacity) * 0.5f), "...fogMaxOpacity is exactly halfway");
}

// Case 4: the concrete regression for "no jarring pop at the surface". Sweeping cameraZCm in small
// steps from well above to well below must produce a fogDensity sequence with no reversal anywhere
// -- not just at the two band edges, where the identity/full-override branches meet the lerp branch,
// but at every step in between.
static void testSweepIsMonotonic() {
    // THIS TEST USED TO WATCH ONE FIELD. It swept the camera through the fade band and checked that
    // fogDensity moved monotonically -- while applyUnderwaterFog overrides SIX fields, and the other
    // five were checked nowhere. A swapped lerp(a, b, t) argument order on fogColor, fogHeight,
    // fogFalloff, fogStart or fogMaxOpacity produces a real, visible pop at the waterline and left
    // this suite entirely green.
    //
    // Every overridden field is now swept, and the argument-order failure is what the check is built
    // to catch: a swapped lerp still moves monotonically, it just moves the WRONG WAY, so
    // monotonicity alone cannot see it. The endpoints are therefore pinned as well -- at the top of
    // the band each field must equal what the author wrote, at the bottom it must equal the tuning.
    // A swap exchanges those two, and no amount of smoothness in between hides it.
    const rhi::SkyAtmosphere authored = makeAuthored();
    const fluids::UnderwaterFogTuning tuning{};
    const f32 waterLevelCm = 0.0f;
    const f32 topCm = waterLevelCm + tuning.fadeBandCm * 2.0f;      // starts outside the band
    const f32 bottomCm = waterLevelCm - tuning.fadeBandCm * 2.0f;   // ends outside the band
    constexpr int kSteps = 400;

    // Every field the override touches, read out of a result by one accessor each, so the sweep
    // below is written once rather than six times.
    struct Field {
        const char* name;
        f32 (*get)(const rhi::SkyAtmosphere&);
    };
    static const Field kFields[] = {
        {"fogColor.r",     [](const rhi::SkyAtmosphere& a) { return a.fogColor[0]; }},
        {"fogColor.g",     [](const rhi::SkyAtmosphere& a) { return a.fogColor[1]; }},
        {"fogColor.b",     [](const rhi::SkyAtmosphere& a) { return a.fogColor[2]; }},
        {"fogDensity",     [](const rhi::SkyAtmosphere& a) { return a.fogDensity; }},
        {"fogHeight",      [](const rhi::SkyAtmosphere& a) { return a.fogHeight; }},
        {"fogFalloff",     [](const rhi::SkyAtmosphere& a) { return a.fogFalloff; }},
        {"fogStart",       [](const rhi::SkyAtmosphere& a) { return a.fogStart; }},
        {"fogMaxOpacity",  [](const rhi::SkyAtmosphere& a) { return a.fogMaxOpacity; }},
    };
    constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

    bool nonDecreasing[kFieldCount], nonIncreasing[kFieldCount];
    f32 previous[kFieldCount];
    for (size_t f = 0; f < kFieldCount; ++f) {
        nonDecreasing[f] = true;
        nonIncreasing[f] = true;
        previous[f] = 0.0f;
    }

    rhi::SkyAtmosphere atTop{}, atBottom{};
    for (int i = 0; i <= kSteps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kSteps);
        const f32 cameraZCm = topCm + (bottomCm - topCm) * t;
        const rhi::SkyAtmosphere result =
            fluids::applyUnderwaterFog(authored, cameraZCm, waterLevelCm, tuning);
        if (i == 0) atTop = result;
        if (i == kSteps) atBottom = result;
        for (size_t f = 0; f < kFieldCount; ++f) {
            const f32 v = kFields[f].get(result);
            constexpr f32 eps = 1e-7f;
            if (i > 0) {
                if (v < previous[f] - eps) nonDecreasing[f] = false;
                if (v > previous[f] + eps) nonIncreasing[f] = false;
            }
            previous[f] = v;
        }
    }

    for (size_t f = 0; f < kFieldCount; ++f)
        check(nonDecreasing[f] || nonIncreasing[f],
              std::string("sweeping through the surface moves ") + kFields[f].name +
              " monotonically -- no pop at the waterline");

    // The endpoints, which is what actually catches a swapped lerp. Above the band the author's own
    // sky must survive untouched; below it, the tuning must have taken over completely.
    for (size_t f = 0; f < kFieldCount; ++f) {
        check(std::abs(kFields[f].get(atTop) - kFields[f].get(authored)) < 1e-6f,
              std::string("well above the surface, ") + kFields[f].name +
              " is still exactly what the author wrote");
    }
    const rhi::SkyAtmosphere fullyUnder =
        fluids::applyUnderwaterFog(authored, waterLevelCm - tuning.fadeBandCm * 4.0f,
                                  waterLevelCm, tuning);
    for (size_t f = 0; f < kFieldCount; ++f) {
        check(std::abs(kFields[f].get(atBottom) - kFields[f].get(fullyUnder)) < 1e-6f,
              std::string("well below the surface, ") + kFields[f].name +
              " has fully reached the underwater value");
    }
}

int main() {
    AVER_INFO("UnderwaterFogTest");
    testAboveWaterIsIdentity();
    testWellBelowIsFullOverride();
    testAtSurfaceIsExactlyHalfway();
    testSweepIsMonotonic();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
