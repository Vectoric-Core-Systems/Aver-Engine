// Packs an authored MaterialDesc into the MaterialConstants block the GPU reads.
#include "aver/pbr/MaterialGpu.hpp"

#include <cmath>
#include <cstring>

namespace aver::pbr {

namespace {

// Decodes one sRGB channel to linear. The exact inverse of the shared prelude's `srgbToLin`, which
// is a gamma-2.2 approximation rather than the piecewise sRGB curve; matching that is the point.
f32 srgbToLinear(f32 c) { return std::pow(c < 0.0f ? 0.0f : c, 2.2f); }

constexpr u32 kSlotFlag[kTextureSlotCount] = {
    MaterialFlag_BaseColorMap,
    MaterialFlag_MetalRoughMap,
    MaterialFlag_NormalMap,
    MaterialFlag_OcclusionMap,
    MaterialFlag_EmissiveMap,
    MaterialFlag_Layer1BaseColorMap,
    MaterialFlag_Layer1MetalRoughMap,
    MaterialFlag_Layer1NormalMap,
};
static_assert(sizeof(kSlotFlag) / sizeof(kSlotFlag[0]) == kTextureSlotCount,
              "one flag per texture slot; adding a slot without its bit silently disables it");
} // namespace

// Packs the authored description into the block the GPU reads.
MaterialConstants packMaterial(const MaterialDesc& d) {
    MaterialConstants c{};
    // Colour is decoded, coverage is not: glTF authors baseColorFactor's rgb in sRGB and its alpha
    // as a linear number.
    for (u32 i = 0; i < 3; ++i) c.baseColorFactor[i] = srgbToLinear(d.baseColorFactor[i]);
    c.baseColorFactor[3] = d.baseColorFactor[3];
    std::memcpy(c.emissiveFactor,  d.emissiveFactor,  sizeof(c.emissiveFactor));
    c.metallicFactor    = d.metallicFactor;
    c.roughnessFactor   = d.roughnessFactor;
    c.normalScale       = d.normalScale;
    c.occlusionStrength = d.occlusionStrength;
    c.alphaCutoff       = d.alphaCutoff;
    c.reflectance       = d.reflectance;
    c.f90               = d.f90;
    c.slopeBlendLo      = d.slopeBlendLo;
    c.slopeBlendHi      = d.slopeBlendHi;
    c.layer1UvScale     = d.layer1UvScale > 1e-4f ? d.layer1UvScale : 1.0f;
    c.pad0              = 0.0f;

    u32 flags = 0;
    for (u32 i = 0; i < kTextureSlotCount; ++i)
        if (!d.textures[i].empty()) flags |= kSlotFlag[i];
    if (d.alphaMode == AlphaMode::Mask)  flags |= MaterialFlag_AlphaMask;
    if (d.alphaMode == AlphaMode::Blend) flags |= MaterialFlag_AlphaBlend;
    if (d.twoSided)                      flags |= MaterialFlag_TwoSided;
    // Only when the author asked AND at least one layer-1 map is actually bound: a slope blend
    // against nothing would fade the surface to the fallback white texture on every slope.
    if (d.slopeBlend && (flags & (MaterialFlag_Layer1BaseColorMap | MaterialFlag_Layer1MetalRoughMap |
                                  MaterialFlag_Layer1NormalMap)))
        flags |= MaterialFlag_SlopeBlend;
    if (d.uvMode == UvMode::WorldAligned) flags |= MaterialFlag_WorldAlignedUv;
    c.flags = flags;

    // Reciprocal once per upload rather than per pixel. A tiling of zero or less collapses to zero
    // tiles per cm instead of producing a NaN UV.
    c.uvTilesPerCm = d.uvTiling > 0.0f ? 1.0f / d.uvTiling : 0.0f;
    return c;
}

} // namespace aver::pbr
