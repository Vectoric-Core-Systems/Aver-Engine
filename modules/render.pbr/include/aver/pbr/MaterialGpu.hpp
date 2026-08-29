#pragma once
#include "aver/pbr/Material.hpp"

// Aver.Render.PBR.Materials — the GPU half of the material system: the packed per-material constant
// block and the flags the shading model branches on. Links the generic Aver.RHI, never Aver.RHI.D3D12.
namespace aver::pbr {

// Which optional parts of a material are present, one bit each, so the shader branches on a
// constant rather than on a texture fetch.
enum MaterialFlag : u32 {
    MaterialFlag_BaseColorMap  = 1u << 0,
    MaterialFlag_MetalRoughMap = 1u << 1,
    MaterialFlag_NormalMap     = 1u << 2,
    MaterialFlag_OcclusionMap  = 1u << 3,
    MaterialFlag_EmissiveMap   = 1u << 4,
    MaterialFlag_AlphaMask     = 1u << 5,
    MaterialFlag_AlphaBlend    = 1u << 6,
    MaterialFlag_TwoSided      = 1u << 7,
    MaterialFlag_WorldAlignedUv = 1u << 8,
    MaterialFlag_Layer1BaseColorMap  = 1u << 9,
    MaterialFlag_Layer1MetalRoughMap = 1u << 10,
    MaterialFlag_Layer1NormalMap     = 1u << 11,
    // The second layer is blended in AT ALL only under this bit. Without it every Layer1 slot is
    // ignored, so a material that names no second layer costs exactly what it always did -- the
    // shader branches on a constant, and the fallback textures bound into the unused slots are
    // never sampled.
    MaterialFlag_SlopeBlend          = 1u << 12,
    // MaterialDesc::castShadow, reaching the GPU for the first time. It has been authorable since
    // the format existed -- parser, writer, C ABI, C# property, four test assertions, four doc
    // entries -- and packMaterial never forwarded it, so ticking it off changed no pixel anywhere.
    // A ray-traced shadow is the first thing in this engine that can honour it, because it is the
    // first shadow path that consults the material at all.
    MaterialFlag_CastShadow          = 1u << 13,
};

// The packed per-material GPU constant block. MIRRORS the HLSL `cbuffer AverMaterial` in
// PbrShaders.cpp field for field; nothing checks it at compile time.
struct MaterialConstants {
    f32 baseColorFactor[4];   // rgb LINEAR, decoded by packMaterial(); a is coverage, not decoded
    f32 emissiveFactor[3];    // rgb radiance
    f32 metallicFactor;
    f32 roughnessFactor;
    f32 normalScale;
    f32 occlusionStrength;
    f32 alphaCutoff;          // read only under MaterialFlag_AlphaMask
    u32 flags;                // MaterialFlag bits
    f32 reflectance;          // F0 of the dielectric base
    f32 f90;                  // reflectance at grazing incidence
    f32 uvTilesPerCm;         // reciprocal of MaterialDesc::uvTiling; read only under WorldAlignedUv

    // ---- second layer, read only under MaterialFlag_SlopeBlend ----
    // The SLOPE BAND the blend runs across, as the cosine of the surface angle from vertical -- i.e.
    // world normal Z, 1 on flat ground and 0 on a vertical face. Layer 0 wins at or above
    // slopeBlendHi, layer 1 wins at or below slopeBlendLo, and it is a smoothstep between them.
    //
    // HI IS THE FLATTER END, which reads backwards until you remember the axis is "flatness", not
    // "steepness". Authoring rock on cliffs means lo 0.55 / hi 0.80: fully rock below 0.55, fully
    // the base layer above 0.80.
    f32 slopeBlendLo;
    f32 slopeBlendHi;
    // Layer 1's own uv scale relative to layer 0's, so a rock face can tile at a different rate to
    // the ground without needing a second uvTiling concept.
    f32 layer1UvScale;
    // WHICH MATERIAL GRAPH SHADES THIS MATERIAL, or 0 for none -- which is every material that was
    // ever authored before graphs existed, and still most of them.
    //
    // IT COST NOTHING TO ADD -- AT THE TIME -- and that is why the whole feature was shaped around
    // it. This block was already handed to the GPU per draw (ctx.setDrawBinding(set, &constants,
    // sizeof)), it was already 80 bytes, and the last four of them were padding nobody read. So a
    // graph-shaded material needed no second constant buffer, no per-material pipeline and no change
    // to this struct's size -- the generated averEvalMaterial switches on this and every id-0
    // material takes the arm that shades exactly as it always did. See pbr::materialGraphHlsl().
    //
    // THAT FREE LUNCH IS SPENT. graphId used the last of the space the compiler was packing for free;
    // ior/transmission below needed a real 16 bytes more, which is why the struct grew to 96. Whoever
    // adds the NEXT field has _pad0/_pad1's 8 bytes to spend before this has to grow again -- see
    // their own comment.
    u32 graphId;

    // ---- dielectric transmission, read only where AVER_MAT_ALPHA_BLEND is set ----
    // Mirrors MaterialDesc::ior/transmission. ior travels with the block for completeness (and for
    // whatever future refraction pass wants it) but nothing reads it yet -- the only consumer today
    // is transmission, via averBuildSurface's alpha computation in PbrShaders.cpp. See
    // MaterialDesc::ior's own comment for why the two are not independent.
    f32 ior;
    f32 transmission;

    // EXPLICIT PADDING, READ BY NOTHING. ior and transmission above land the struct's real content at
    // 88 bytes; a constant buffer's size must still be a multiple of 16, and unlike graphId's arrival
    // (which fit inside bytes the compiler was already reserving) there was no such room left, so
    // these two floats are what buys the alignment back. Named and sized explicitly rather than left
    // as an implicit compiler tail, so the byte count is visible here instead of only in the
    // static_assert below. 8 bytes of headroom for the next field before 96 has to become 112.
    f32 _pad0;
    f32 _pad1;
};

static_assert(sizeof(MaterialConstants) == 96, "the HLSL cbuffer mirrors this byte for byte");
static_assert(sizeof(MaterialConstants) % 16 == 0, "must be a legal constant-buffer size");

// Packs the authored description into the block the GPU reads. A slot counts as bound when either
// reference form is set.
MaterialConstants packMaterial(const MaterialDesc& d);

} // namespace aver::pbr
