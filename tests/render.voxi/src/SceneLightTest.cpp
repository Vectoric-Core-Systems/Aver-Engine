// SceneLightTest -- the CPU-checkable half of scene lights (aver/voxi/SceneLight.hpp): unit conversion,
// packing into the 80-byte GPU record, and the rectangle's polygonal form factor against closed-form
// answers. The HLSL (render.pt/shaders/aver_lights.hlsli) is the code that runs; its source is read at the
// end to check it still names the same constants and record layout the header packs for.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/voxi/SceneLight.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::voxi;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool closeTo(f32 a, f32 b, f32 relTol, f32 absTol = 1e-6f) {
    return std::fabs(a - b) <= std::max(absTol, relTol * std::fabs(b));
}

// A rect facing down (axis -Z) at height h above the origin, width x height_.
static PackedLight rectFacingDown(f32 h, f32 w, f32 hh) {
    SceneLight s;
    s.kind = 3;
    s.pos[0] = 0; s.pos[1] = 0; s.pos[2] = h;
    s.axis[0] = 0; s.axis[1] = 0; s.axis[2] = -1;
    s.right[0] = 0; s.right[1] = 1; s.right[2] = 0;
    s.intensityCd = 1000.0f;
    s.widthCm = w;
    s.heightCm = hh;
    return packSceneLight(s, SceneLightAssets{});
}

int main() {
    AVER_INFO("=== SceneLightTest ===");

    AVER_INFO("=== packing ===");
    {
        SceneLight p;
        p.kind = 0;
        p.pos[0] = 10; p.pos[1] = 20; p.pos[2] = 30;
        p.colour[0] = 1; p.colour[1] = 0.5f; p.colour[2] = 0.25f;
        p.intensityCd = 100000.0f / 3.0f;   // exactly one engine unit at 1 m
        p.radiusCm = 0.2f;                  // below the 1 cm floor
        f32 imp = 0;
        const PackedLight l = packSceneLight(p, SceneLightAssets{}, &imp);
        check(closeTo(l.radianceRange[0], 1.0f, 1e-6f) && closeTo(l.radianceRange[1], 0.5f, 1e-6f),
              "a point light of 33333 cd is 1 engine unit at 1 m, times its colour");
        check(l.posRadius[3] == 1.0f, "emitter radius floors at 1 cm");
        check(l.axisKind[3] == 0.0f && l.shape[2] == -1.0f && l.shape[3] == -1.0f,
              "kind 0, no IES, no cookie");
        check(closeTo(imp, 1.0f, 1e-6f), "importance is the colour-weighted 1 m irradiance");
        check(l.radianceRange[3] > 0.0f && l.radianceRange[3] <= kLightAutoRangeMaxCm,
              "an auto range is derived and capped");

        p.rangeCm = 900.0f;
        check(packSceneLight(p, SceneLightAssets{}).radianceRange[3] == 900.0f, "an authored range is kept");

        p.kind = 1; p.innerCos = 0.9f; p.outerCos = 0.8f; p.castShadows = false;
        const PackedLight sp = packSceneLight(p, SceneLightAssets{});
        check(sp.axisKind[3] == static_cast<f32>(kLightKindSpot + kLightNoShadowBit), "spot kind with the no-shadow bit");
        check(sp.shape[0] == 0.9f && sp.shape[1] == 0.8f, "cone cosines");
        check(closeTo(sp.right[3], std::sqrt(1.0f - 0.64f) / 0.8f, 1e-5f), "cookie frustum tangent from the outer angle");

        p.innerCos = 0.5f;   // inner wider than outer is repaired, not passed through
        check(packSceneLight(p, SceneLightAssets{}).shape[0] > 0.8f, "an inner cone wider than the outer is clamped");

        SceneLightAssets a;
        a.iesIndex = 7; a.cookieIndex = 9; a.iesPeakOverMean = 4.0f;
        p.kind = 0; p.iesPeak = false;
        PackedLight withIes = packSceneLight(p, a);
        check(withIes.shape[2] == 7.0f && withIes.shape[3] == 9.0f, "asset indices land in shape.zw");
        const f32 flux = withIes.radianceRange[0];
        p.iesPeak = true;
        check(closeTo(packSceneLight(p, a).radianceRange[0], flux * 0.25f, 1e-6f),
              "peak normalisation divides by peak/mean; the default keeps flux");
    }

    AVER_INFO("=== rect units ===");
    {
        // 1 m x 1 m at 1000 cd along its normal: radiance = 1000 cd / 1 m^2.
        const PackedLight l = rectFacingDown(200.0f, 100.0f, 100.0f);
        check(closeTo(l.radianceRange[0], 1000.0f / kEngineUnitCdm2, 1e-5f), "radiance is candela over area");
        check(l.shape[0] == 50.0f && l.shape[1] == 50.0f && l.posRadius[3] == 0.0f, "half extents; no sphere radius");
        const PackedLight big = rectFacingDown(200.0f, 200.0f, 100.0f);
        check(closeTo(big.radianceRange[0], l.radianceRange[0] * 0.5f, 1e-5f), "twice the area, half the radiance: same candela");
    }

    AVER_INFO("=== rect form factor ===");
    {
        const Vec3 up{0, 0, 1}, down{0, 0, -1};
        const Vec3 origin{0, 0, 0};

        // Far, small panel: irradiance -> I cos / d^2, i.e. radiance * area * cos / d^2 per unit radiance.
        const f32 h = 5000.0f, w = 20.0f, hh = 10.0f;
        const PackedLight small = rectFacingDown(h, w, hh);
        const f32 ff = rectDiffuseFormFactor(small, origin, up);
        check(closeTo(ff, w * hh / (h * h), 2e-3f), "a distant small panel is a point source (A / d^2)");

        // Off-axis: cosines on both ends.
        const Vec3 p{3000, 0, 0};
        const f32 d2 = 3000.0f * 3000.0f + h * h;
        const f32 cosT = h / std::sqrt(d2);
        const f32 ffOff = rectDiffuseFormFactor(small, p, up);
        check(closeTo(ffOff, w * hh * cosT * cosT / d2, 5e-3f), "off-axis falls with both cosines");

        // Infinite plane overhead: irradiance = pi * L.
        const PackedLight huge = rectFacingDown(100.0f, 2.0e6f, 2.0e6f);
        check(closeTo(rectDiffuseFormFactor(huge, origin, up), kPi, 5e-3f), "an infinite panel gives pi");

        // Surface turned 90 degrees to the plane's normal: half of that, the horizon clips the rest.
        check(closeTo(rectDiffuseFormFactor(huge, origin, Vec3{1, 0, 0}), kPi * 0.5f, 5e-3f),
              "a vertical wall under an infinite panel gets pi / 2 (horizon clipping)");

        // A surface facing away sees nothing.
        check(rectDiffuseFormFactor(huge, origin, down) == 0.0f, "a surface facing away is dark");

        // One-sided: a receiver above the panel's back gets nothing.
        check(rectDiffuseFormFactor(small, Vec3{0, 0, h + 100.0f}, down) == 0.0f, "the back of the panel emits nothing");

        // Clipping cannot add light: tilting the receiver monotonically reduces a big panel's form factor
        // below pi, and never produces a negative.
        f32 prev = 1e9f;
        bool mono = true, nonneg = true;
        for (int i = 0; i <= 18; ++i) {
            const f32 a = static_cast<f32>(i) * 5.0f * kDegToRad;
            const f32 f = rectDiffuseFormFactor(huge, origin, Vec3{std::sin(a), 0, std::cos(a)});
            if (f > prev + 1e-4f) mono = false;
            if (f < 0.0f) nonneg = false;
            prev = f;
        }
        check(mono && nonneg, "tilting away from an infinite panel only darkens");

        // Shape-correct: an elongated panel lights a surface more along its long axis's perpendicular.
        const PackedLight strip = rectFacingDown(100.0f, 400.0f, 20.0f);   // long along Y
        const f32 alongLong  = rectDiffuseFormFactor(strip, Vec3{0, 150, 0}, up);
        const f32 alongShort = rectDiffuseFormFactor(strip, Vec3{150, 0, 0}, up);
        check(alongLong > alongShort * 3.0f, "a strip lights 150 cm along its length far more than 150 cm across it");
    }

    AVER_INFO("=== the shader still agrees with the header ===");
    {
        std::string hlsl;
        const bool read = readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.pt/shaders/aver_lights.hlsli", hlsl);
        check(read, "aver_lights.hlsli is readable");
        const auto has = [&](const char* s) { return hlsl.find(s) != std::string::npos; };
        check(has("#define AVER_IES_V 64.0") && has("#define AVER_IES_H 32.0"), "IES table size matches kIesTableV/H");
        check(has("struct AverLightRec") && has("float4 posRadius;") && has("float4 radianceRange;") &&
                  has("float4 axisKind;") && has("float4 shape;") && has("float4 right;"),
              "the 80-byte record has the five float4 fields PackedLight packs");
        check(has("aversLightEval") && has("aversClipQuad") && has("aversPolygonVector"), "the rectangle maths is there");
        check(has("(l.axisKind.w + 0.5)) >> 3") || has("((uint)(l.axisKind.w + 0.5)) >> 3"),
              "the no-shadow bit is bit 3 of the kind, as kLightNoShadowBit says");
        check(has("(1e4 / max(d2, r * r))"), "the sphere falloff keeps the emissive-lamp constant (1 m in cm^2)");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
