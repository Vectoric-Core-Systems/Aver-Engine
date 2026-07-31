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
};

static_assert(sizeof(MaterialConstants) == 64, "the HLSL cbuffer mirrors this byte for byte");
static_assert(sizeof(MaterialConstants) % 16 == 0, "must be a legal constant-buffer size");

// Packs the authored description into the block the GPU reads. A slot counts as bound when either
// reference form is set.
MaterialConstants packMaterial(const MaterialDesc& d);

} // namespace aver::pbr
