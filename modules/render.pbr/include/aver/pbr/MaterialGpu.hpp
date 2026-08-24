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
    // IT COST NOTHING TO ADD, and that is why the whole feature is shaped around it. This block is
    // already handed to the GPU per draw (ctx.setDrawBinding(set, &constants, sizeof)), it was
    // already 80 bytes, and the last four of them were padding nobody read. So a graph-shaded
    // material needs no second constant buffer, no per-material pipeline, no change to the draw
    // path and no change to this struct's size -- the generated averEvalMaterial switches on this
    // and every id-0 material takes the arm that shades exactly as it always did. See
    // pbr::materialGraphHlsl().
    u32 graphId;
};

static_assert(sizeof(MaterialConstants) == 80, "the HLSL cbuffer mirrors this byte for byte");
static_assert(sizeof(MaterialConstants) % 16 == 0, "must be a legal constant-buffer size");

// Packs the authored description into the block the GPU reads. A slot counts as bound when either
// reference form is set.
MaterialConstants packMaterial(const MaterialDesc& d);

} // namespace aver::pbr
