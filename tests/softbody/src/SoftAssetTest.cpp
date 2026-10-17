// .ocsoft: the text round trip, the preset table, refusals, forward compatibility and the paint codec.
// Pure text and arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-6f) { return std::fabs(a - b) <= eps; }

int main() {
    AVER_INFO("SoftAssetTest");

    AVER_INFO("the preset table");
    {
        const sb::Material& metal = sb::presetMaterial(sb::SoftPreset::Metal);
        check(approx(metal.areaDensityKgM2, 7.8f) && approx(metal.yieldStrain, 0.010f) && approx(metal.toughnessJ, 40.0f),
              "metal: 7.8 kg/m2, yield 0.010, toughness 40 J");
        check(approx(metal.maxSetFrac, 0.25f) && approx(metal.breakStrain, 0.30f), "metal: set cap 25% of rest, break 0.30");
        const sb::Material& glass = sb::presetMaterial(sb::SoftPreset::Glass);
        check(glass.behavior == sb::Behavior::Shatter && approx(glass.breakStrain, 0.006f) && approx(glass.toughnessJ, 0.4f),
              "glass shatters at 0.006 with 0.4 J per edge");
        const sb::Material& wood = sb::presetMaterial(sb::SoftPreset::Wood);
        check(wood.behavior == sb::Behavior::Fracture && approx(wood.breakStrain, 0.020f), "wood fractures at 0.020");
        const sb::Material& light = sb::presetMaterial(sb::SoftPreset::ClothLight);
        check(approx(light.areaDensityKgM2, 0.25f) && approx(light.bendCompliance, 0.05f) && light.compliance == 0.0f,
              "cloth light: 0.25 kg/m2, bend 0.05, inextensible");
        check(sb::presetInfo(sb::SoftPreset::ClothHeavy).iterations == 6 && approx(sb::presetInfo(sb::SoftPreset::ClothHeavy).damping, 0.5f),
              "cloth heavy: 6 iterations, damping 0.5");
        sb::SoftPreset p;
        check(sb::presetFromKey("glass", p) && p == sb::SoftPreset::Glass, "a preset key resolves");
        check(!sb::presetFromKey("rubber", p), "an unknown preset key is refused");
    }

    AVER_INFO("a minimal asset resolves to its preset");
    {
        sb::SoftAsset a;
        std::string why;
        check(sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"cloth","preset":"cloth_light"})", a, &why),
              "the minimal asset parses (" + why + ")");
        const sb::SoftAsset ref = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        check(a.kind == sb::SoftKind::Cloth && a.preset == sb::SoftPreset::ClothLight, "kind and preset");
        check(approx(a.material.areaDensityKgM2, ref.material.areaDensityKgM2) && a.sim.iterations == ref.sim.iterations &&
              approx(a.sim.damping, ref.sim.damping), "material and sim come from the preset");
        check(sb::writeSoftAsset(a) == sb::writeSoftAsset(ref), "it writes exactly the preset asset");

        check(sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"destructible"})", a, &why) &&
              a.preset == sb::SoftPreset::Metal, "a destructible with no preset reads as metal");
        check(sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"cloth","preset":"cloth_light",
                                      "material":{"areaDensityKgM2":0.5},"sim":{"iterations":9},"mystery":[1,2],"x":{"y":1}})", a, &why),
              "unknown keys are ignored");
        check(approx(a.material.areaDensityKgM2, 0.5f) && a.sim.iterations == 9, "and overrides apply over the preset");
    }

    AVER_INFO("write -> parse -> write is byte identical");
    {
        for (u32 k = 0; k < 2; ++k)
            for (u32 p = 0; p < sb::kSoftPresetCount; ++p) {
                sb::SoftAsset a = sb::makeSoftAsset(static_cast<sb::SoftKind>(k), static_cast<sb::SoftPreset>(p));
                a.sim.vertexRadiusCm = 0.123456f;
                a.cloth.tearable = (p & 1) != 0;
                a.destruct.maxPieces = 7 + p;
                a.paint.meshKey = 0x0123456789abcdefull;
                a.paint.pin = {0, 0, 255, 255, 255, 0};
                a.paint.reach = {255, 255, 0, 0, 100, 255};
                sb::PaintRule above; above.type = sb::PaintRule::Type::Above; above.axis = 2; above.valueCm = 170.5f;
                sb::PaintRule bone; bone.type = sb::PaintRule::Type::Bone; bone.bone = "spine_03"; bone.minWeight = 0.5f;
                sb::PaintRule sph; sph.type = sb::PaintRule::Type::Sphere; sph.centreCm[2] = 120.0f; sph.radiusCm = 8.0f;
                sb::PaintRule mask; mask.type = sb::PaintRule::Type::Mask; mask.texture = "Textures/cape \"mask\".png"; mask.pinBelow = 0.25f;
                a.paint.rules = {above, bone, sph, mask};
                const std::string once = sb::writeSoftAsset(a);
                sb::SoftAsset b;
                std::string why;
                const bool ok = sb::parseSoftAsset(once, b, &why);
                check(ok, "kind " + std::to_string(k) + " preset " + std::to_string(p) + " parses (" + why + ")");
                if (ok) {
                    check(sb::writeSoftAsset(b) == once, "and re-writes byte-identical");
                    check(b.paint.rules.size() == 4 && b.paint.rules[3].texture == mask.texture && b.paint.meshKey == a.paint.meshKey &&
                          b.paint.pin == a.paint.pin && b.paint.reach == a.paint.reach, "paint survives");
                }
            }
    }

    AVER_INFO("refusals");
    {
        sb::SoftAsset a;
        std::string why;
        check(!sb::parseSoftAsset("{ not json", a, &why) && !why.empty(), "malformed JSON");
        check(!sb::parseSoftAsset(R"({"format":"ocbeam","version":1})", a, &why), "wrong format");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft"})", a, &why), "missing version");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":2})", a, &why), "a newer version is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"rope"})", a, &why), "unknown kind");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"preset":"rubber"})", a, &why), "unknown preset");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"sim":{"iterations":0}})", a, &why), "zero iterations");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"sim":{"maxParticles":2}})", a, &why), "maxParticles under 3");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"material":{"areaDensityKgM2":0}})", a, &why), "zero area density");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"material":{"behavior":"melt"}})", a, &why), "unknown behaviour");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"destructible","destruct":{"impulseEfficiency":0}})", a, &why),
              "zero impulse efficiency");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"pin":"zz"}})", a, &why), "bad paint hex");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"pin":"ff01","reach":"ff0200ff"}})", a, &why), "pin/reach length mismatch");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"rules":[{"type":"above","axis":"w"}]}})", a, &why), "bad rule axis");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"rules":[{"type":"bone"}]}})", a, &why), "bone rule without a bone");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"rules":[{"type":"sphere","radiusCm":0}]}})", a, &why), "sphere with no radius");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"paint":{"rules":[{"type":"twist"}]}})", a, &why), "unknown rule type");
        sb::SoftAsset v = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        check(v.valid(), "a preset asset is valid");
        v.cloth.dragCoeff = 3.0f;
        check(!v.valid(&why), "drag over 2 is invalid");
        v = sb::makeSoftAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        v.destruct.maxPieces = 0;
        check(!v.valid(&why), "zero max pieces is invalid");
    }

    AVER_INFO("the paint codec");
    {
        std::vector<u8> bytes(700, 0);
        for (usize i = 300; i < 520; ++i) bytes[i] = 255;
        bytes[10] = 7;
        const std::string hex = sb::encodePaintRle(bytes);
        std::vector<u8> back;
        check(sb::decodePaintRle(hex, back) && back == bytes, "runs over 255 and mixed bytes round trip");
        check(sb::decodePaintRle("", back) && back.empty() && sb::encodePaintRle({}).empty(), "empty round trips");
        check(!sb::decodePaintRle("abc", back), "a length that is not a multiple of four is refused");
        check(!sb::decodePaintRle("ff00", back), "a zero run is refused");
        check(!sb::decodePaintRle("ffzz", back), "a non-hex digit is refused");
        check(sb::reachFromPaint(255, 6.0f) < 0.0f && approx(sb::reachFromPaint(0, 6.0f), 0.0f) &&
              approx(sb::reachFromPaint(254, 6.0f), 12.0f), "reach bytes map to free, hard skin and twice the default");
    }

    AVER_INFO("stale paint");
    {
        sb::PaintData p;
        check(!p.stale(5), "empty paint is never stale");
        p.meshKey = 5; p.pin = {255};
        check(!p.stale(5) && p.stale(6), "painted data is stale for a different mesh key");
        sb::PaintData rulesOnly;
        rulesOnly.rules.push_back(sb::PaintRule{});
        check(!rulesOnly.stale(9), "rules alone are never stale");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
