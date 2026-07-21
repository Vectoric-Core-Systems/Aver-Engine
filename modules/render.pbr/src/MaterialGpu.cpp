#include "aver/pbr/MaterialGpu.hpp"

#include <cmath>
#include <cstring>

namespace aver::pbr {

namespace {

// The exact inverse of the shared prelude's `srgbToLin`, which is the engine's gamma-2.2
// approximation rather than the piecewise sRGB curve. Matching THAT is the requirement: a
// piecewise-accurate decode here would disagree with every colour the shader still decodes itself,
// and the two would differ by a couple of 8-bit codes in the darks with nothing to attribute it to.
f32 srgbToLinear(f32 c) { return std::pow(c < 0.0f ? 0.0f : c, 2.2f); }

constexpr u32 kSlotFlag[kTextureSlotCount] = {
    MaterialFlag_BaseColorMap,
    MaterialFlag_MetalRoughMap,
    MaterialFlag_NormalMap,
    MaterialFlag_OcclusionMap,
    MaterialFlag_EmissiveMap,
};
} // namespace

MaterialConstants packMaterial(const MaterialDesc& d) {
    MaterialConstants c{};
    // Colour is decoded, coverage is not. glTF authors baseColorFactor's rgb in sRGB and its alpha
    // as a linear number, and running alpha through the curve would move every cutoff test.
    for (u32 i = 0; i < 3; ++i) c.baseColorFactor[i] = srgbToLinear(d.baseColorFactor[i]);
    c.baseColorFactor[3] = d.baseColorFactor[3];
    // emissiveFactor is radiance and is already linear by definition, in glTF and here.
    std::memcpy(c.emissiveFactor,  d.emissiveFactor,  sizeof(c.emissiveFactor));
    c.metallicFactor    = d.metallicFactor;
    c.roughnessFactor   = d.roughnessFactor;
    c.normalScale       = d.normalScale;
    c.occlusionStrength = d.occlusionStrength;
    c.alphaCutoff       = d.alphaCutoff;

    u32 flags = 0;
    for (u32 i = 0; i < kTextureSlotCount; ++i)
        if (!d.textures[i].empty()) flags |= kSlotFlag[i];
    if (d.alphaMode == AlphaMode::Mask)  flags |= MaterialFlag_AlphaMask;
    if (d.alphaMode == AlphaMode::Blend) flags |= MaterialFlag_AlphaBlend;
    if (d.twoSided)                      flags |= MaterialFlag_TwoSided;
    c.flags = flags;
    return c;
}

} // namespace aver::pbr
