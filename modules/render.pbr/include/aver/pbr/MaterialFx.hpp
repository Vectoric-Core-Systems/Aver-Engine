#pragma once
#include "aver/core/Types.hpp"

// G4 R6: the BaseMaterial3D features a converted project expects (R6 of https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md). Core-only, header-only, no RHI.
//
// A material that uses none of this has `fx.active() == false`: it packs to exactly the 192-byte MaterialConstants block it always did, draws
// through the pipelines it always did and is shaded by the shader code it always was. A material that uses any of it gets
//   - MaterialFlag_Fx in its flags,
//   - a 64-byte MaterialExtConstants block appended to the b2 draw constants (192 + 64 = 256 = rhi::kMaxDrawConstantBytes), and
//   - a variant key (fxVariantKey) naming the compile-time shader features, the fixed-function state and the vertex stage it needs.
// The ray-traced material table, GI and every RT hit read the first 192 bytes only, so RT hits ignore all of it.
namespace aver::pbr {

enum class FxCull : u8 { Default = 0, Back, Front, None };                 // Default = today's: no culling
enum class FxBlend : u8 { Default = 0, Add, Sub, Mul, Mix, PremultAlpha };  // Default = the alpha-blend path as it is
enum class FxDepthDraw : u8 { Default = 0, Always, Never };                // Default = opaque-only (blended: no write; opaque: write)
enum class FxAlphaExt : u8 { None = 0, Hash, Antialias };                  // AlphaMode::Mask stays the plain scissor
enum class FxBillboard : u8 { Off = 0, Enabled, FixedY, Particles };
enum class FxDistanceFade : u8 { Off = 0, PixelAlpha, PixelDither, ObjectDither };
enum class FxBlendOp : u8 { Mix = 0, Add, Sub, Mul };                      // detail blend
enum class FxSpecular : u8 { Schlick = 0, Toon, Disabled };
enum class FxDiffuse : u8 { Burley = 0, Lambert, LambertWrap, Toon };

struct MaterialFx {
    // ---- UV transforms (uv1 drives the base maps, uv2 the maps that ask for it; UV2 needs the R1 second vertex stream, else uv1 is used)
    f32  uv1Scale[2] = {1.0f, 1.0f}, uv1Offset[2] = {0.0f, 0.0f};
    f32  uv2Scale[2] = {1.0f, 1.0f}, uv2Offset[2] = {0.0f, 0.0f};
    bool uv1Triplanar = false, uv2Triplanar = false;           // uv2 triplanar: the engine has one projection, so uv2 maps share uv1's
    bool uv1WorldTriplanar = false, uv2WorldTriplanar = false;
    f32  uv1Sharpness = 1.0f, uv2Sharpness = 1.0f;

    // ---- fixed-function state
    FxCull      cull = FxCull::Default;
    FxBlend     blend = FxBlend::Default;
    FxDepthDraw depthDraw = FxDepthDraw::Default;
    bool        noDepthTest = false;
    i32         renderPriority = 0;                           // -128..127, sorts blended draws (higher draws later)
    bool        unshaded = false;                             // shading_mode unshaded
    FxAlphaExt  alphaExt = FxAlphaExt::None;
    f32         alphaHashScale = 1.0f;
    bool        shadowToOpacity = false;                      // carried, not drawn

    // ---- vertex stage
    FxBillboard billboard = FxBillboard::Off;
    bool        billboardKeepScale = false;
    bool        fixedSize = false;
    bool        grow = false;
    f32         growCm = 0.0f;
    u8          particlesH = 1, particlesV = 1;
    bool        particlesLoop = false;

    // ---- fades (distances in centimetres)
    FxDistanceFade distanceFade = FxDistanceFade::Off;
    f32 fadeMinCm = 0.0f, fadeMaxCm = 1000.0f;

    // ---- surface
    bool rim = false;          f32 rimAmount = 1.0f, rimTint = 0.5f;
    bool anisotropy = false;   f32 anisotropyAmount = 0.0f;
    bool heightmap = false;    f32 heightScale = 5.0f; bool deepParallax = false; u8 minLayers = 8, maxLayers = 32;
    bool flipTangent = false, flipBinormal = false, invertHeight = false;
    bool backlight = false;    f32 backlightColor[3] = {0.0f, 0.0f, 0.0f};   // linear
    bool detail = false;       FxBlendOp detailBlend = FxBlendOp::Mix; u8 detailUv = 0;
    u8   emissionOp = 0;       // 0 add, 1 multiply
    bool emissionOnUv2 = false, aoOnUv2 = false;
    f32  aoLightAffect = 0.0f;
    FxSpecular specularMode = FxSpecular::Schlick;
    FxDiffuse  diffuseMode = FxDiffuse::Burley;
    bool disableAmbient = false, noReceiveShadows = false;

    // ---- an embedded HLSL shader (the graph named by the .ocmat GRAPHREF; OcGraphHlsl.hpp)
    bool embVertex = false;    // the shader has a vertex(): VSMainEmb runs it (a raster variant; ray hits and shadow maps see the unmoved mesh)
    bool embDiscard = false;   // fragment() can discard or writes ALPHA_SCISSOR_THRESHOLD: the variant is compiled without forced early depth

    bool usesUv() const {
        return uv1Scale[0] != 1.0f || uv1Scale[1] != 1.0f || uv1Offset[0] != 0.0f || uv1Offset[1] != 0.0f ||
               uv2Scale[0] != 1.0f || uv2Scale[1] != 1.0f || uv2Offset[0] != 0.0f || uv2Offset[1] != 0.0f;
    }
    bool usesTriplanar() const { return uv1Triplanar || uv2Triplanar; }
    bool usesSurface() const {
        return disableAmbient || noReceiveShadows || aoLightAffect != 0.0f || emissionOp != 0 || emissionOnUv2 || aoOnUv2 ||
               specularMode != FxSpecular::Schlick || diffuseMode != FxDiffuse::Burley;
    }
    bool usesVertex() const { return billboard != FxBillboard::Off || fixedSize || (grow && growCm != 0.0f); }
    bool active() const {
        return usesUv() || usesTriplanar() || usesSurface() || usesVertex() || cull == FxCull::Back || cull == FxCull::Front || blend == FxBlend::Add || blend == FxBlend::Sub || blend == FxBlend::Mul ||
               depthDraw != FxDepthDraw::Default || noDepthTest || renderPriority != 0 || unshaded || alphaExt != FxAlphaExt::None ||
               shadowToOpacity || distanceFade != FxDistanceFade::Off || rim || anisotropy || heightmap || backlight || detail || embVertex || embDiscard;
    }
};

// ---- the variant key: 24 bits, stored in the upper bits of MaterialExtConstants::variant (the low byte is the render priority, biased by 128).
namespace fxkey {
inline constexpr u32 kCullShift = 0;          // 2 bits: FxCull
inline constexpr u32 kBlendShift = 2;         // 3 bits: FxBlend
inline constexpr u32 kDepthDrawShift = 5;     // 2 bits: FxDepthDraw
inline constexpr u32 kNoDepthTest = 1u << 7;
inline constexpr u32 kLateZ = 1u << 8;        // compile PSMainVoxi without [earlydepthstencil]: it discards by hash / dither
inline constexpr u32 kVertex = 1u << 9;       // VSMainFx instead of VSMain
inline constexpr u32 kUvx = 1u << 10;
inline constexpr u32 kTri = 1u << 11;
inline constexpr u32 kRim = 1u << 12;
inline constexpr u32 kAniso = 1u << 13;
inline constexpr u32 kParallax = 1u << 14;
inline constexpr u32 kBacklight = 1u << 15;
inline constexpr u32 kDetail = 1u << 16;
inline constexpr u32 kSurf = 1u << 17;        // emission operator, ao on uv2 / light affect, disable ambient / shadows
inline constexpr u32 kModes = 1u << 18;       // specular / diffuse modes
inline constexpr u32 kHash = 1u << 19;        // alpha hash and alpha-to-coverage approximation
inline constexpr u32 kFade = 1u << 20;        // distance fade
inline constexpr u32 kUnshaded = 1u << 21;
inline constexpr u32 kEmbVertex = 1u << 22;   // VSMainEmb (an embedded shader's vertex()) instead of VSMain; also compiles the stage's varyings in
inline constexpr u32 kShaderMask = kUvx | kTri | kRim | kAniso | kParallax | kBacklight | kDetail | kSurf | kModes | kHash | kFade | kUnshaded;
inline constexpr u32 kPipelineMask = (1u << 24) - 1u;
}  // namespace fxkey

// Which of the engine's passes a draw with this key must not join: the frame-wide depth prepass and the per-draw depth-only draw write depth
// through the ordinary vertex and pixel shaders, which disagree with a variant that moves vertices, transforms UVs or discards by noise.
inline constexpr u32 kFxNoPrepassKeys = fxkey::kVertex | fxkey::kShaderMask | fxkey::kLateZ | fxkey::kEmbVertex;

// What the ray tracer's material table and everything else keyed on the first 192 bytes must not see.
inline constexpr u32 kFxBaseBytes = 192;

// The 64 bytes appended to the draw constants. Dword layout (mirrored by the `gFx*` fields of the AverMaterial cbuffer under AVER_MATX in
// material_prelude.hlsl; MaterialTest compares offsets):
//  0 variant     (priority + 128) | key << 8
//  1 modes       bit 0-1 billboard, 2 keep scale, 3 fixed size, 4-5 detail blend, 6 detail uv2, 7 emission multiply, 8 emission uv2, 9 ao uv2,
//                10 uv1 triplanar, 11 uv2 triplanar, 12 uv1 world, 13 uv2 world, 14-15 distance fade, 16 grow, 17-18 specular, 19-20 diffuse,
//                21 parallax, 22 deep, 23 flip tangent, 24 flip binormal, 25 invert height, 26 rim, 27 anisotropy, 28 backlight, 29 detail,
//                30 no shadows, 31 no ambient
//  2..5 uv1 scale.xy, offset.xy (f32)
//  6 uv2 scale (half2)   7 uv2 offset (half2)   8 triplanar sharpness uv1, uv2 (half2)   9 rim amount, tint (half2)
//  10 anisotropy, ao light affect (half2)   11 height scale, grow cm (half2)   12 backlight rgb unorm8, byte 3 unused
//  13 fade min, max cm (half2)   14 alpha hash scale (half, low) , spare   15 frames: bits 0-5 h, 6-11 v, 12 loop, 13-17 min layers, 18-23 max layers
struct MaterialExtConstants {
    u32 variant;
    u32 modes;
    f32 uv1[4];
    u32 uv2Scale, uv2Offset, sharpness, rim, aniso, heightGrow, backlight, fade, hash, frames;
};
static_assert(sizeof(MaterialExtConstants) == 64, "the tail is one 64-byte block: 192 + 64 = kMaxDrawConstantBytes");

namespace modes {
inline constexpr u32 kBillboardMask = 3u, kKeepScale = 1u << 2, kFixedSize = 1u << 3, kDetailBlendShift = 4, kDetailUv2 = 1u << 6, kEmissionMul = 1u << 7,
                     kEmissionUv2 = 1u << 8, kAoUv2 = 1u << 9, kUv1Tri = 1u << 10, kUv2Tri = 1u << 11, kUv1World = 1u << 12, kUv2World = 1u << 13,
                     kFadeShift = 14, kGrow = 1u << 16, kSpecShift = 17, kDiffShift = 19, kParallax = 1u << 21, kDeep = 1u << 22, kFlipT = 1u << 23,
                     kFlipB = 1u << 24, kInvert = 1u << 25, kRim = 1u << 26, kAniso = 1u << 27, kBacklight = 1u << 28, kDetail = 1u << 29,
                     kNoShadows = 1u << 30, kNoAmbient = 1u << 31;
}  // namespace modes

// float -> IEEE half (round to nearest, saturating at the largest finite half); the HLSL side reads it with f16tof32.
inline u32 fxHalf(f32 f) {
    union { f32 f; u32 u; } v{f};
    const u32 sign = (v.u >> 16) & 0x8000u;
    const u32 absu = v.u & 0x7FFFFFFFu;
    if (absu >= 0x7F800000u) return sign | (absu > 0x7F800000u ? 0x7E00u : 0x7BFFu);   // NaN -> quiet NaN, inf -> max
    if (absu >= 0x477FF000u) return sign | 0x7BFFu;                                    // saturate
    if (absu < 0x33000001u) return sign;                                              // underflows to 0
    if (absu < 0x38800000u) {                                                         // subnormal half
        const u32 e = absu >> 23;
        const u32 m = (absu & 0x7FFFFFu) | 0x800000u;
        const u32 shift = 126u - e;
        u32 h = m >> shift;
        if ((m >> (shift - 1u)) & 1u) ++h;
        return sign | h;
    }
    u32 h = ((absu - 0x38000000u) >> 13);
    if (absu & 0x1000u) ++h;   // round to nearest (ties up); a carry into the exponent is correct
    return sign | h;
}
inline f32 fxHalfToFloat(u32 h) {
    const u32 sign = (h & 0x8000u) << 16;
    u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 out;
    if (e == 0) {
        if (m == 0) out = sign;
        else { e = 127 - 14; while (!(m & 0x400u)) { m <<= 1; --e; } m &= 0x3FFu; out = sign | (e << 23) | (m << 13); }
    } else if (e == 31) out = sign | 0x7F800000u | (m << 13);
    else out = sign | ((e + 112u) << 23) | (m << 13);
    union { u32 u; f32 f; } v{out};
    return v.f;
}
inline u32 fxHalf2(f32 lo, f32 hi) { return fxHalf(lo) | (fxHalf(hi) << 16); }

// The key of a material's fx, and the pack into the tail. Both are pure.
inline u32 fxVariantKey(const MaterialFx& fx) {
    u32 k = 0;
    k |= (fx.cull == FxCull::Back ? 1u : (fx.cull == FxCull::Front ? 2u : 0u)) << fxkey::kCullShift;
    // Mix is the engine's alpha blend and Premult alpha its premultiplied output (approximated as the same): only add / sub / mul change the pipeline.
    k |= (fx.blend == FxBlend::Add ? 1u : (fx.blend == FxBlend::Sub ? 2u : (fx.blend == FxBlend::Mul ? 3u : 0u))) << fxkey::kBlendShift;
    k |= static_cast<u32>(fx.depthDraw) << fxkey::kDepthDrawShift;
    if (fx.noDepthTest) k |= fxkey::kNoDepthTest;
    if (fx.usesVertex()) k |= fxkey::kVertex;
    if (fx.usesUv()) k |= fxkey::kUvx;
    if (fx.usesTriplanar()) k |= fxkey::kTri;
    if (fx.rim) k |= fxkey::kRim;
    if (fx.anisotropy) k |= fxkey::kAniso;
    if (fx.heightmap) k |= fxkey::kParallax;
    if (fx.backlight) k |= fxkey::kBacklight;
    if (fx.detail) k |= fxkey::kDetail;
    if (fx.disableAmbient || fx.noReceiveShadows || fx.aoLightAffect != 0.0f || fx.emissionOp != 0 || fx.emissionOnUv2 || fx.aoOnUv2) k |= fxkey::kSurf;
    if (fx.specularMode != FxSpecular::Schlick || fx.diffuseMode != FxDiffuse::Burley) k |= fxkey::kModes;
    if (fx.alphaExt != FxAlphaExt::None) k |= fxkey::kHash | fxkey::kLateZ;
    if (fx.distanceFade != FxDistanceFade::Off) k |= fxkey::kFade | fxkey::kLateZ;
    if (fx.unshaded) k |= fxkey::kUnshaded;
    if (fx.embVertex) k |= fxkey::kEmbVertex;
    if (fx.embDiscard) k |= fxkey::kLateZ;
    return k & fxkey::kPipelineMask;
}

// alphaMasked: the material also clips (AlphaMode::Mask), which the forced early depth of PSMainVoxi cannot take back, so its variant is compiled late-Z.
inline MaterialExtConstants packMaterialFx(const MaterialFx& fx, bool alphaMasked = false) {
    MaterialExtConstants e{};
    const i32 pr = fx.renderPriority < -128 ? -128 : (fx.renderPriority > 127 ? 127 : fx.renderPriority);
    e.variant = static_cast<u32>(pr + 128) | ((fxVariantKey(fx) | (alphaMasked ? fxkey::kLateZ : 0u)) << 8);
    u32 m = static_cast<u32>(fx.billboard) & modes::kBillboardMask;
    if (fx.billboardKeepScale) m |= modes::kKeepScale;
    if (fx.fixedSize) m |= modes::kFixedSize;
    m |= (static_cast<u32>(fx.detailBlend) & 3u) << modes::kDetailBlendShift;
    if (fx.detailUv) m |= modes::kDetailUv2;
    if (fx.emissionOp) m |= modes::kEmissionMul;
    if (fx.emissionOnUv2) m |= modes::kEmissionUv2;
    if (fx.aoOnUv2) m |= modes::kAoUv2;
    if (fx.uv1Triplanar) m |= modes::kUv1Tri;
    if (fx.uv2Triplanar) m |= modes::kUv2Tri;
    if (fx.uv1WorldTriplanar) m |= modes::kUv1World;
    if (fx.uv2WorldTriplanar) m |= modes::kUv2World;
    m |= (static_cast<u32>(fx.distanceFade) & 3u) << modes::kFadeShift;
    if (fx.grow) m |= modes::kGrow;
    m |= (static_cast<u32>(fx.specularMode) & 3u) << modes::kSpecShift;
    m |= (static_cast<u32>(fx.diffuseMode) & 3u) << modes::kDiffShift;
    if (fx.heightmap) m |= modes::kParallax;
    if (fx.deepParallax) m |= modes::kDeep;
    if (fx.flipTangent) m |= modes::kFlipT;
    if (fx.flipBinormal) m |= modes::kFlipB;
    if (fx.invertHeight) m |= modes::kInvert;
    if (fx.rim) m |= modes::kRim;
    if (fx.anisotropy) m |= modes::kAniso;
    if (fx.backlight) m |= modes::kBacklight;
    if (fx.detail) m |= modes::kDetail;
    if (fx.noReceiveShadows) m |= modes::kNoShadows;
    if (fx.disableAmbient) m |= modes::kNoAmbient;
    e.modes = m;
    e.uv1[0] = fx.uv1Scale[0]; e.uv1[1] = fx.uv1Scale[1]; e.uv1[2] = fx.uv1Offset[0]; e.uv1[3] = fx.uv1Offset[1];
    e.uv2Scale = fxHalf2(fx.uv2Scale[0], fx.uv2Scale[1]);
    e.uv2Offset = fxHalf2(fx.uv2Offset[0], fx.uv2Offset[1]);
    e.sharpness = fxHalf2(fx.uv1Sharpness, fx.uv2Sharpness);
    e.rim = fxHalf2(fx.rimAmount, fx.rimTint);
    e.aniso = fxHalf2(fx.anisotropyAmount, fx.aoLightAffect);
    e.heightGrow = fxHalf2(fx.heightScale, fx.growCm);
    const auto u8c = [](f32 v) { return static_cast<u32>((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f); };
    e.backlight = u8c(fx.backlightColor[0]) | (u8c(fx.backlightColor[1]) << 8) | (u8c(fx.backlightColor[2]) << 16);
    e.fade = fxHalf2(fx.fadeMinCm, fx.fadeMaxCm);
    e.hash = fxHalf(fx.alphaHashScale) | (fx.alphaExt == FxAlphaExt::Antialias ? 0x10000u : 0u);
    const auto cl = [](u32 v, u32 lo, u32 hi) { return v < lo ? lo : (v > hi ? hi : v); };
    e.frames = cl(fx.particlesH, 1, 63) | (cl(fx.particlesV, 1, 63) << 6) | ((fx.particlesLoop ? 1u : 0u) << 12) | (cl(fx.minLayers, 1, 31) << 13) |
               (cl(fx.maxLayers, 1, 63) << 18);
    return e;
}

}  // namespace aver::pbr
