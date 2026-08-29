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
    // 0 = no graph, which is the arm the generated averEvalMaterial's `default:` takes. See
    // MaterialDesc::graphId for why the DESCRIPTION carries a runtime-assigned number at all.
    c.graphId           = d.graphId;
    c.ior               = d.ior;
    c.transmission      = d.transmission;
    c.subsurfaceWeight  = d.subsurfaceWeight;
    c.subsurfaceRadius  = d.subsurfaceRadius;
    c.coatWeight        = d.coatWeight;
    c.coatRoughness     = d.coatRoughness;
    c.coatF0            = d.coatF0;
    c._coatPad          = 0.0f;   // assigned, not left to the {} above -- see the note below
    // The two floats above are what _pad0/_pad1 used to be. The struct carries no padding now, so
    // every one of its members is assigned here rather than some being left at the zero the
    // `MaterialConstants c{};` above gives them -- if a field is ever added back without a line in
    // this function, it ships as a silent zero.

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
    // POSITIVE sense (set when it DOES cast), so a material packed before this bit existed -- every
    // one of them, with a zero in bit 13 -- would read as "casts nothing" if the sense were
    // inverted. castShadow defaults to true, so the positive spelling is the one that agrees with
    // the default for anything that predates the flag.
    if (d.castShadow) flags |= MaterialFlag_CastShadow;
    // Keyed on the WEIGHT alone: a radius with no weight scatters nothing, and letting it set the
    // flag would pay for the shader's subsurface branch to compute a zero.
    if (d.subsurfaceWeight > 0.0f) flags |= MaterialFlag_Subsurface;
    // Same reasoning as subsurface above: the WEIGHT alone decides. A coat roughness or F0 with no
    // weight coats nothing, and letting either set the flag would pay for the lobe to compute zero.
    if (d.coatWeight > 0.0f) flags |= MaterialFlag_Coat;
    c.flags = flags;

    // Reciprocal once per upload rather than per pixel. A tiling of zero or less collapses to zero
    // tiles per cm instead of producing a NaN UV.
    c.uvTilesPerCm = d.uvTiling > 0.0f ? 1.0f / d.uvTiling : 0.0f;
    return c;
}

} // namespace aver::pbr
