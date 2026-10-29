// MaterialFxTest -- G4 R6 below the renderer: the BaseMaterial3D feature block of a material (pbr::MaterialFx), its .ocmat FX record, the 64-byte extension
// block behind the 192-byte material constants, the variant key a draw is routed by, and the shader text that has to agree with all of it. No GPU, no window.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }
static bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }
static std::string slurp(const char* path) {
    std::string s;
    readFileText(path, s);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // the shader files may be checked out with CRLF
    return s;
}

// A material that uses a lot of the block, with non-default values everywhere.
static pbr::MaterialDesc busy() {
    pbr::MaterialDesc d;
    d.name = "Busy";
    pbr::MaterialFx& f = d.fx;
    f.uv1Scale[0] = 2; f.uv1Scale[1] = 3; f.uv1Offset[0] = 0.25f; f.uv1Offset[1] = -0.5f;
    f.uv2Scale[0] = 4; f.uv2Scale[1] = 0.5f; f.uv2Offset[0] = 0.125f; f.uv2Offset[1] = 0.75f;
    f.uv1Triplanar = true; f.uv1WorldTriplanar = true; f.uv1Sharpness = 4;
    f.uv2Triplanar = true; f.uv2Sharpness = 2;
    f.cull = pbr::FxCull::Front; f.blend = pbr::FxBlend::Sub; f.depthDraw = pbr::FxDepthDraw::Always; f.noDepthTest = true;
    f.renderPriority = -7; f.unshaded = true; f.alphaExt = pbr::FxAlphaExt::Antialias; f.alphaHashScale = 2.5f; f.shadowToOpacity = true;
    f.billboard = pbr::FxBillboard::Particles; f.billboardKeepScale = true; f.fixedSize = true; f.grow = true; f.growCm = 1.5f;
    f.particlesH = 4; f.particlesV = 2; f.particlesLoop = true;
    f.distanceFade = pbr::FxDistanceFade::ObjectDither; f.fadeMinCm = 200; f.fadeMaxCm = 2000;
    f.rim = true; f.rimAmount = 0.8f; f.rimTint = 0.25f;
    f.anisotropy = true; f.anisotropyAmount = -0.5f;
    f.heightmap = true; f.heightScale = 8; f.deepParallax = true; f.minLayers = 4; f.maxLayers = 16;
    f.flipTangent = true; f.flipBinormal = true; f.invertHeight = true;
    f.backlight = true; f.backlightColor[0] = 1; f.backlightColor[1] = 0.5f; f.backlightColor[2] = 0.25f;
    f.detail = true; f.detailBlend = pbr::FxBlendOp::Mul; f.detailUv = 1;
    f.emissionOp = 1; f.emissionOnUv2 = true; f.aoOnUv2 = true; f.aoLightAffect = 0.5f;
    f.specularMode = pbr::FxSpecular::Disabled; f.diffuseMode = pbr::FxDiffuse::LambertWrap;
    f.disableAmbient = true; f.noReceiveShadows = true;
    return d;
}

static bool sameFx(const pbr::MaterialFx& a, const pbr::MaterialFx& b) {
    const auto n = [](float x, float y) { return near(x, y, 1e-5); };
    return n(a.uv1Scale[0], b.uv1Scale[0]) && n(a.uv1Scale[1], b.uv1Scale[1]) && n(a.uv1Offset[0], b.uv1Offset[0]) && n(a.uv1Offset[1], b.uv1Offset[1]) &&
           n(a.uv2Scale[0], b.uv2Scale[0]) && n(a.uv2Scale[1], b.uv2Scale[1]) && n(a.uv2Offset[0], b.uv2Offset[0]) && n(a.uv2Offset[1], b.uv2Offset[1]) &&
           a.uv1Triplanar == b.uv1Triplanar && a.uv2Triplanar == b.uv2Triplanar && a.uv1WorldTriplanar == b.uv1WorldTriplanar && a.uv2WorldTriplanar == b.uv2WorldTriplanar &&
           n(a.uv1Sharpness, b.uv1Sharpness) && n(a.uv2Sharpness, b.uv2Sharpness) && a.cull == b.cull && a.blend == b.blend && a.depthDraw == b.depthDraw &&
           a.noDepthTest == b.noDepthTest && a.renderPriority == b.renderPriority && a.unshaded == b.unshaded && a.alphaExt == b.alphaExt && n(a.alphaHashScale, b.alphaHashScale) &&
           a.shadowToOpacity == b.shadowToOpacity && a.billboard == b.billboard && a.billboardKeepScale == b.billboardKeepScale && a.fixedSize == b.fixedSize && a.grow == b.grow &&
           n(a.growCm, b.growCm) && a.particlesH == b.particlesH && a.particlesV == b.particlesV && a.particlesLoop == b.particlesLoop && a.distanceFade == b.distanceFade &&
           n(a.fadeMinCm, b.fadeMinCm) && n(a.fadeMaxCm, b.fadeMaxCm) && a.rim == b.rim && n(a.rimAmount, b.rimAmount) && n(a.rimTint, b.rimTint) && a.anisotropy == b.anisotropy &&
           n(a.anisotropyAmount, b.anisotropyAmount) && a.heightmap == b.heightmap && n(a.heightScale, b.heightScale) && a.deepParallax == b.deepParallax && a.minLayers == b.minLayers &&
           a.maxLayers == b.maxLayers && a.flipTangent == b.flipTangent && a.flipBinormal == b.flipBinormal && a.invertHeight == b.invertHeight && a.backlight == b.backlight &&
           n(a.backlightColor[0], b.backlightColor[0]) && n(a.backlightColor[1], b.backlightColor[1]) && n(a.backlightColor[2], b.backlightColor[2]) && a.detail == b.detail &&
           a.detailBlend == b.detailBlend && a.detailUv == b.detailUv && a.emissionOp == b.emissionOp && a.emissionOnUv2 == b.emissionOnUv2 && a.aoOnUv2 == b.aoOnUv2 &&
           n(a.aoLightAffect, b.aoLightAffect) && a.specularMode == b.specularMode && a.diffuseMode == b.diffuseMode && a.disableAmbient == b.disableAmbient &&
           a.noReceiveShadows == b.noReceiveShadows;
}

int main() {
    AVER_INFO("=== a material that uses none of it is every material before R6 ===");
    {
        const pbr::MaterialDesc d;
        check(!d.fx.active(), "a default MaterialDesc has an inactive fx");
        check(pbr::fxVariantKey(d.fx) == 0, "and variant key 0");
        const pbr::MaterialConstants c = pbr::packMaterial(d);
        check((c.flags & pbr::MaterialFlag_Fx) == 0, "packMaterial sets no Fx flag");
        check(sizeof(pbr::MaterialConstants) == pbr::kFxBaseBytes && sizeof(pbr::MaterialConstants) == rhi::kDrawMaterialBaseBytes, "the base block is still 192 bytes, the figure the RHI reads the tail behind");
        check(sizeof(pbr::MaterialExtConstants) == 64 && sizeof(pbr::MaterialConstants) + sizeof(pbr::MaterialExtConstants) == rhi::kMaxDrawConstantBytes,
              "the tail is 64 bytes: 192 + 64 is exactly the draw-constant limit");
        const std::string text = fmt::writeOcmat(d);
        check(!has(text, "\nFX"), "its .ocmat has no FX record (every older file stays byte-identical)");
        pbr::MaterialDesc back;
        std::string err;
        check(fmt::parseOcmat(text, back, nullptr, &err) && !back.fx.active(), "and parses back to an inactive fx");
        check(rhi::drawMaterialFxKey(&c, sizeof c) == 0 && rhi::drawMaterialFxPriority(&c, sizeof c) == 0, "a 192-byte draw block has no variant key and no priority");
    }

    AVER_INFO("=== which settings count as a feature ===");
    {
        pbr::MaterialFx f;
        f.cull = pbr::FxCull::None;       check(!f.active(), "an explicit no-cull is the engine's own behaviour, not a feature");
        f.blend = pbr::FxBlend::Mix;      check(!f.active(), "blend mix is the alpha blend as it is");
        f.blend = pbr::FxBlend::PremultAlpha; check(!f.active(), "premult alpha is drawn with the engine's premultiplied blend");
        f = pbr::MaterialFx{}; f.cull = pbr::FxCull::Back; check(f.active(), "back-face culling is a feature (the engine culled nothing before)");
        f = pbr::MaterialFx{}; f.blend = pbr::FxBlend::Add; check(f.active(), "blend add is");
        f = pbr::MaterialFx{}; f.renderPriority = 3; check(f.active(), "a render priority is");
        f = pbr::MaterialFx{}; f.uv1Scale[0] = 2; check(f.active(), "a UV scale is");
        f = pbr::MaterialFx{}; f.unshaded = true; check(f.active(), "unshaded is");
    }

    AVER_INFO("=== the variant key ===");
    {
        const auto key = [](auto&& set) { pbr::MaterialFx f; set(f); return pbr::fxVariantKey(f); };
        using namespace pbr::fxkey;
        check(key([](pbr::MaterialFx& f) { f.uv1Offset[1] = 1; }) == kUvx, "a UV transform asks for kUvx alone");
        check(key([](pbr::MaterialFx& f) { f.uv1Triplanar = true; }) == kTri, "triplanar: kTri");
        check(key([](pbr::MaterialFx& f) { f.rim = true; }) == kRim, "rim: kRim");
        check(key([](pbr::MaterialFx& f) { f.anisotropy = true; }) == kAniso, "anisotropy: kAniso");
        check(key([](pbr::MaterialFx& f) { f.heightmap = true; }) == kParallax, "heightmap: kParallax");
        check(key([](pbr::MaterialFx& f) { f.backlight = true; }) == kBacklight, "backlight: kBacklight");
        check(key([](pbr::MaterialFx& f) { f.detail = true; }) == kDetail, "detail: kDetail");
        check(key([](pbr::MaterialFx& f) { f.emissionOp = 1; }) == kSurf && key([](pbr::MaterialFx& f) { f.aoLightAffect = 0.3f; }) == kSurf &&
                  key([](pbr::MaterialFx& f) { f.noReceiveShadows = true; }) == kSurf, "emission operator, ao affect and the shadow switch share kSurf");
        check(key([](pbr::MaterialFx& f) { f.specularMode = pbr::FxSpecular::Disabled; }) == kModes, "specular mode: kModes");
        check(key([](pbr::MaterialFx& f) { f.alphaExt = pbr::FxAlphaExt::Hash; }) == (kHash | kLateZ), "alpha hash: kHash and late-Z (it discards by noise)");
        check(key([](pbr::MaterialFx& f) { f.distanceFade = pbr::FxDistanceFade::PixelAlpha; }) == (kFade | kLateZ), "distance fade: kFade and late-Z");
        check(key([](pbr::MaterialFx& f) { f.unshaded = true; }) == kUnshaded, "unshaded: kUnshaded");
        check(key([](pbr::MaterialFx& f) { f.billboard = pbr::FxBillboard::Enabled; }) == kVertex && key([](pbr::MaterialFx& f) { f.fixedSize = true; }) == kVertex &&
                  key([](pbr::MaterialFx& f) { f.grow = true; f.growCm = 1; }) == kVertex, "billboard, fixed size and grow share the vertex stage");
        check(key([](pbr::MaterialFx& f) { f.grow = true; f.growCm = 0; }) == 0, "grow by nothing is nothing");
        check(((key([](pbr::MaterialFx& f) { f.cull = pbr::FxCull::Back; }) >> kCullShift) & 3u) == 1u && ((key([](pbr::MaterialFx& f) { f.cull = pbr::FxCull::Front; }) >> kCullShift) & 3u) == 2u,
              "cull back is 1, front is 2");
        check(((key([](pbr::MaterialFx& f) { f.blend = pbr::FxBlend::Add; }) >> kBlendShift) & 7u) == 1u && ((key([](pbr::MaterialFx& f) { f.blend = pbr::FxBlend::Sub; }) >> kBlendShift) & 7u) == 2u &&
                  ((key([](pbr::MaterialFx& f) { f.blend = pbr::FxBlend::Mul; }) >> kBlendShift) & 7u) == 3u && key([](pbr::MaterialFx& f) { f.blend = pbr::FxBlend::Mix; }) == 0,
              "blend add / sub / mul are 1 / 2 / 3, mix is no change");
        check(((key([](pbr::MaterialFx& f) { f.depthDraw = pbr::FxDepthDraw::Always; }) >> kDepthDrawShift) & 3u) == 1u && (key([](pbr::MaterialFx& f) { f.noDepthTest = true; }) & kNoDepthTest),
              "depth draw always is 1; no depth test has its bit");
        check((pbr::fxVariantKey(busy().fx) & ~kPipelineMask) == 0, "no key bit above 24");
        check(pbr::kFxNoPrepassKeys & kVertex && pbr::kFxNoPrepassKeys & kShaderMask && pbr::kFxNoPrepassKeys & kLateZ, "vertex, shader and late-Z variants are kept out of the depth prepass");
    }

    AVER_INFO("=== the extension block ===");
    {
        const pbr::MaterialDesc d = busy();
        const pbr::MaterialExtConstants e = pbr::packMaterialFx(d.fx);
        check((e.variant & 0xFFu) == static_cast<u32>(-7 + 128), "the low byte of dword 0 is the render priority, biased by 128");
        check((e.variant >> 8) == pbr::fxVariantKey(d.fx), "the rest is the variant key");
        // The RHI reads exactly these two things from the draw block, 192 bytes in.
        struct Block { pbr::MaterialConstants base; pbr::MaterialExtConstants ext; } blk{pbr::packMaterial(d), e};
        check(rhi::drawMaterialFxKey(&blk, sizeof blk) == pbr::fxVariantKey(d.fx) && rhi::drawMaterialFxPriority(&blk, sizeof blk) == -7, "drawMaterialFxKey / Priority read the block as the devices do");
        check(rhi::drawMaterialFxKey(&blk, sizeof blk.base) == 0, "and read nothing from a block that stops at 192 bytes");
        check(offsetof(Block, ext) == pbr::kFxBaseBytes, "the extension starts at byte 192");
        check(offsetof(pbr::MaterialExtConstants, modes) == 4 && offsetof(pbr::MaterialExtConstants, uv1) == 8 && offsetof(pbr::MaterialExtConstants, uv2Scale) == 24 &&
                  offsetof(pbr::MaterialExtConstants, frames) == 60, "dword offsets: modes 1, uv1 2..5, uv2Scale 6, frames 15");
        check(near(e.uv1[0], 2) && near(e.uv1[1], 3) && near(e.uv1[2], 0.25) && near(e.uv1[3], -0.5), "uv1 scale and offset are full floats");
        check(near(pbr::fxHalfToFloat(e.uv2Scale & 0xFFFF), 4) && near(pbr::fxHalfToFloat(e.uv2Scale >> 16), 0.5) && near(pbr::fxHalfToFloat(e.uv2Offset >> 16), 0.75), "uv2 scale and offset are half pairs");
        check(near(pbr::fxHalfToFloat(e.sharpness & 0xFFFF), 4) && near(pbr::fxHalfToFloat(e.rim & 0xFFFF), 0.8, 1e-3) && near(pbr::fxHalfToFloat(e.rim >> 16), 0.25), "sharpness and rim pairs");
        check(near(pbr::fxHalfToFloat(e.aniso & 0xFFFF), -0.5) && near(pbr::fxHalfToFloat(e.aniso >> 16), 0.5) && near(pbr::fxHalfToFloat(e.heightGrow & 0xFFFF), 8) &&
                  near(pbr::fxHalfToFloat(e.heightGrow >> 16), 1.5), "anisotropy, ao affect, height scale and grow pairs");
        check((e.backlight & 255u) == 255u && ((e.backlight >> 8) & 255u) == 128u && ((e.backlight >> 16) & 255u) == 64u, "backlight is unorm8 rgb");
        check(near(pbr::fxHalfToFloat(e.fade & 0xFFFF), 200) && near(pbr::fxHalfToFloat(e.fade >> 16), 2000), "fade distances in centimetres");
        check(near(pbr::fxHalfToFloat(e.hash & 0xFFFF), 2.5) && (e.hash & 0x10000u), "alpha hash scale and the antialias bit");
        check((e.frames & 63u) == 4 && ((e.frames >> 6) & 63u) == 2 && (e.frames & (1u << 12)) && ((e.frames >> 13) & 31u) == 4 && ((e.frames >> 18) & 63u) == 16, "frames and parallax layers");
        using namespace pbr::modes;
        check((e.modes & kBillboardMask) == 3u && (e.modes & kKeepScale) && (e.modes & kFixedSize) && (e.modes & kGrow), "billboard particles, keep scale, fixed size, grow");
        check(((e.modes >> kDetailBlendShift) & 3u) == 3u && (e.modes & kDetailUv2) && (e.modes & kEmissionMul) && (e.modes & kEmissionUv2) && (e.modes & kAoUv2), "detail blend multiply on UV2, emission multiply, emission and ao on UV2");
        check((e.modes & kUv1Tri) && (e.modes & kUv2Tri) && (e.modes & kUv1World) && !(e.modes & kUv2World), "triplanar flags");
        check(((e.modes >> kFadeShift) & 3u) == 3u && ((e.modes >> kSpecShift) & 3u) == 2u && ((e.modes >> kDiffShift) & 3u) == 2u, "fade, specular and diffuse modes");
        check((e.modes & kParallax) && (e.modes & kDeep) && (e.modes & kFlipT) && (e.modes & kFlipB) && (e.modes & kInvert), "parallax flags");
        check((e.modes & kRim) && (e.modes & kAniso) && (e.modes & kBacklight) && (e.modes & kDetail) && (e.modes & kNoShadows) && (e.modes & kNoAmbient), "feature switches");
        const pbr::MaterialExtConstants masked = pbr::packMaterialFx(pbr::MaterialFx{}, /*alphaMasked=*/true);
        check((masked.variant >> 8) == pbr::fxkey::kLateZ, "an alpha-masked material is compiled late-Z (forced early depth cannot take back a clip)");
        const pbr::MaterialConstants c = pbr::packMaterial(d);
        check((c.flags & pbr::MaterialFlag_Fx) != 0, "packMaterial sets MaterialFlag_Fx for a material with fx");
        pbr::MaterialDesc terrain = d;
        terrain.terrain.layers.resize(1);
        check((pbr::packMaterial(terrain).flags & pbr::MaterialFlag_Fx) == 0, "a terrain material never takes fx");
    }

    AVER_INFO("=== half floats ===");
    {
        for (const float v : {0.0f, 1.0f, -2.0f, 0.5f, 65504.0f, 3.14159f, 0.1f, 1000.5f, 6.1e-5f, 1.0e-6f}) {
            const float back = pbr::fxHalfToFloat(pbr::fxHalf(v));
            const double tol = std::fabs(v) * 1.0e-3 + 1.0e-7;
            check(near(back, v, tol), "half round trip of " + std::to_string(v) + " -> " + std::to_string(back));
        }
        check(pbr::fxHalfToFloat(pbr::fxHalf(1.0e9f)) == 65504.0f, "a value beyond the range saturates");
    }

    AVER_INFO("=== the .ocmat FX record ===");
    {
        const pbr::MaterialDesc d = busy();
        const std::string text = fmt::writeOcmat(d);
        check(has(text, "\nFX ") && has(text, "cull=front") && has(text, "blend=sub") && has(text, "uv1tri=1") && has(text, "billboard=particles"), "the record names what the material changes");
        pbr::MaterialDesc back;
        std::string err;
        check(fmt::parseOcmat(text, back, nullptr, &err), "parses (" + err + ")");
        check(sameFx(d.fx, back.fx), "every field round-trips");
        check(fmt::writeOcmat(back) == text, "and the text is byte-stable");
        pbr::MaterialFx only;
        only.uv1Scale[0] = 3;
        pbr::MaterialDesc one;
        one.fx = only;
        const std::string t1 = fmt::writeOcmat(one);
        check(has(t1, "FX uv1scale=3,1") && !has(t1, "cull=") && !has(t1, "rim"), "only values that differ from the defaults are written");

        // The legacy records that were recorded and never read are wired up: SHADER unlit, CULL front, BLEND additive. CULL back stays the record it was.
        pbr::MaterialDesc legacy;
        check(fmt::parseOcmat("OCMAT 1\nSHADER unlit\nCULL front\nBLEND additive\n", legacy, nullptr, &err), "a legacy file parses");
        check(legacy.fx.unshaded && legacy.fx.cull == pbr::FxCull::Front && legacy.fx.blend == pbr::FxBlend::Add && legacy.alphaMode == pbr::AlphaMode::Blend,
              "SHADER unlit, CULL front and BLEND additive now mean unshaded, front cull and the add blend");
        pbr::MaterialDesc plain;
        check(fmt::parseOcmat("OCMAT 1\nSHADER standard\nCULL back\nBLEND opaque\nFLAGS twosided=0 castshadow=1 worlduv=0\n", plain, nullptr, &err) && !plain.fx.active(),
              "the CULL back the editor wrote into every file is not a request to cull");
        pbr::MaterialDesc over;
        check(fmt::parseOcmat("OCMAT 1\nCULL front\nFX cull=back rim=1 bogus=1\n", over, nullptr, &err) && over.fx.cull == pbr::FxCull::Back && over.fx.rim, "an FX record overrides the legacy cull; an unknown key is skipped");

        pbr::MaterialFx viaPairs;
        fmt::parseOcmatFx("uv2scale=2,2 aniso=1 anisoamount=0.25 layers=2,9", viaPairs);
        check(near(viaPairs.uv2Scale[0], 2) && viaPairs.anisotropy && near(viaPairs.anisotropyAmount, 0.25) && viaPairs.minLayers == 2 && viaPairs.maxLayers == 9, "parseOcmatFx reads the pairs the converter composes");
    }

    AVER_INFO("=== the library sanitises the block ===");
    {
        pbr::MaterialDesc d;
        d.fx.uv1Scale[0] = 1.0e9f;
        d.fx.uv1Sharpness = -3;
        d.fx.growCm = std::nanf("");
        d.fx.renderPriority = 9999;
        d.fx.rimTint = 4;
        d.fx.minLayers = 50; d.fx.maxLayers = 2;
        d.fx.cull = static_cast<pbr::FxCull>(77);
        d.fx.particlesH = 0;
        const pbr::MaterialHandle h = pbr::MaterialLibrary::get().create(d);
        const pbr::MaterialDesc* s = pbr::MaterialLibrary::get().desc(h);
        check(s != nullptr, "the material is stored");
        if (s) {
            check(s->fx.uv1Scale[0] == 1000.0f && s->fx.uv1Sharpness == 0.0f && s->fx.growCm == 0.0f && s->fx.renderPriority == 127 && s->fx.rimTint == 1.0f, "scales, sharpness, grow, priority and tint are clamped");
            check(s->fx.cull == pbr::FxCull::Default && s->fx.particlesH == 1 && s->fx.minLayers == 31 && s->fx.maxLayers >= s->fx.minLayers, "an unknown enum, zero frames and swapped layer counts are repaired");
        }
        pbr::MaterialLibrary::get().destroy(h);
    }

    AVER_INFO("=== the shader text agrees ===");
    {
        const std::string prelude = slurp(AVER_PRELUDE_PATH);
        const std::string voxi = slurp(AVER_VOXI_PATH);
        check(!prelude.empty() && !voxi.empty(), "the shader sources were found");
        // The cbuffer tail, in the order of MaterialExtConstants.
        const char* const order[] = {"gFxVariant", "gFxModes", "gFxUv1ScaleX", "gFxUv1ScaleY", "gFxUv1OffsetX", "gFxUv1OffsetY", "gFxUv2Scale", "gFxUv2Offset",
                                     "gFxSharp", "gFxRim", "gFxAniso", "gFxHeightGrow", "gFxBacklight", "gFxFade", "gFxHash", "gFxFrames"};
        size_t pos = prelude.find("#if AVER_MATX\n    // G4 R6: pbr::MaterialExtConstants");
        bool inOrder = pos != std::string::npos;
        for (const char* n : order) {
            const size_t at = prelude.find(n, pos == std::string::npos ? 0 : pos);
            if (at == std::string::npos || at < pos) { inOrder = false; break; }
            pos = at;
        }
        check(inOrder, "the AverMaterial cbuffer declares the 16 tail dwords in the C++ struct's order");
        check(has(prelude, "#define AVER_MAT_FX             (1u << 20)") && pbr::MaterialFlag_Fx == (1u << 20), "AVER_MAT_FX mirrors MaterialFlag_Fx");
        // Every key bit has a define the renderer passes and a use in the shaders.
        for (const char* d : {"AVER_MATX_UVX", "AVER_MATX_TRI", "AVER_MATX_RIM", "AVER_MATX_ANISO", "AVER_MATX_PARALLAX", "AVER_MATX_BACKLIGHT", "AVER_MATX_DETAIL", "AVER_MATX_SURF",
                              "AVER_MATX_MODES", "AVER_MATX_HASH", "AVER_MATX_FADE", "AVER_MATX_UNSHADED"})
            check(has(prelude, std::string("#if ") + d), std::string("the prelude has a block for ") + d);
        check(has(voxi, "AVER_MATX_LATEZ") && has(voxi, "VSMainFx") && has(voxi, "AVER_MATX_MUL") && has(voxi, "AVER_MATX_SURF"), "voxi.hlsl has the late-Z switch, the vertex stage, the multiply blend and the shadow switch");
        check(has(voxi, "#define AVER_EARLYZ [earlydepthstencil]"), "the default scene shader still forces early depth");
        // The default scene shaders must not see the variant code: it all sits behind AVER_MATX.
        check(has(prelude, "#if AVER_MATX\n// ================= G4 R6") && has(prelude, "#endif   // AVER_MATX"), "the stock averEvalMaterial is the #else of the variant's");
        // Pre-existing mirrors unchanged: the material cbuffer's first 192 bytes still end at gScatterAnisotropy.
        check(has(prelude, "float  gScatterAnisotropy;\n#if AVER_MATX"), "the base cbuffer is unchanged up to gScatterAnisotropy");
        // Modes bit masks the shader names match pbr::modes.
        check(has(prelude, "#define AVER_FX_KEEP_SCALE   (1u << 2)") && pbr::modes::kKeepScale == (1u << 2) && has(prelude, "#define AVER_FX_NO_AMBIENT   (1u << 31)") && pbr::modes::kNoAmbient == (1u << 31),
              "the shader's mode masks match pbr::modes");
    }

    AVER_INFO("MaterialFxTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : exitCode(ExitCode::Failed);
}
