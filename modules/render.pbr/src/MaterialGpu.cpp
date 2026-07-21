#include "aver/pbr/MaterialGpu.hpp"

#include <cstring>

namespace aver::pbr {

namespace {
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
    std::memcpy(c.baseColorFactor, d.baseColorFactor, sizeof(c.baseColorFactor));
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
