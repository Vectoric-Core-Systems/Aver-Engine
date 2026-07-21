#pragma once
#include "aver/pbr/Material.hpp"

// Aver.Render.PBR.Materials — the GPU half of the material system.
//
// This is the second target on purpose, exactly as Voxi splits its settings DLL from its renderer:
// everything that needs the RHI lives here, so the DLL above can stay Core-only and keep
// render-hardware types off the P/Invoke boundary. It links Aver.RHI (the GENERIC interface) and
// NEVER Aver.RHI.D3D12 — a link line is the only place that rule can actually be enforced.
namespace aver::pbr {

// Which optional parts of a material are actually present, packed into one word so the shading
// model branches on a constant rather than on a texture fetch. Bit per texture slot, then the
// alpha rule: a shader cannot see an unbound slot (Tier 1 null-fills every declared descriptor with
// a valid view of the right dimension), so "is this map authored" has to be told, not detected.
enum MaterialFlag : u32 {
    MaterialFlag_BaseColorMap  = 1u << 0,
    MaterialFlag_MetalRoughMap = 1u << 1,
    MaterialFlag_NormalMap     = 1u << 2,
    MaterialFlag_OcclusionMap  = 1u << 3,
    MaterialFlag_EmissiveMap   = 1u << 4,
    MaterialFlag_AlphaMask     = 1u << 5,
    MaterialFlag_AlphaBlend    = 1u << 6,
    MaterialFlag_TwoSided      = 1u << 7,
};

// The packed per-material GPU constant block. Field order is the HLSL cbuffer's, and the size is a
// multiple of 16 bytes so it is legal both as a root-constant run and as a constant buffer — a
// cross-module ABI with no compiler behind it, so it gets the same one-owner treatment as
// rhi::sharedShaderPrelude()'s cbuffer layouts.
struct MaterialConstants {
    // rgb LINEAR, decoded from the authored sRGB by packMaterial(); a is authored coverage and is
    // not a colour, so it is not decoded. The decode happens here rather than in the shader so that
    // everything the shading model touches is linear with no exceptions -- a shader that decoded
    // some of its inputs and not others is one edit away from decoding the wrong one.
    f32 baseColorFactor[4];
    f32 emissiveFactor[3];    // rgb radiance
    f32 metallicFactor;
    f32 roughnessFactor;
    f32 normalScale;
    f32 occlusionStrength;
    f32 alphaCutoff;          // read only under MaterialFlag_AlphaMask
    u32 flags;                // MaterialFlag bits
    // Spent out of what used to be pure padding, so the block stays 64 bytes and no consumer of it
    // has to change size. See MaterialDesc for why a surface owns these rather than the shader.
    f32 reflectance;          // F0 of the dielectric base
    f32 f90;                  // reflectance at grazing incidence
    u32 _pad;
};

static_assert(sizeof(MaterialConstants) == 64, "the HLSL cbuffer mirrors this byte for byte");
static_assert(sizeof(MaterialConstants) % 16 == 0, "must be a legal constant-buffer size");

// Packs the authored description into the block the GPU reads. A texture slot counts as bound when
// EITHER reference form is set, because which of the two resolves is the asset system's business,
// not the shader's.
MaterialConstants packMaterial(const MaterialDesc& d);

} // namespace aver::pbr
