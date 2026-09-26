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
    //
    // THAT PREMISE IS PROBABLY WRONG FOR glTF, AND IS DELIBERATELY LEFT ALONE PENDING A DECISION.
    // The glTF 2.0 specification defines pbrMetallicRoughness.baseColorFactor as LINEAR multipliers
    // on the sampled base-colour texels -- only the TEXTURE is sRGB-encoded, not the numeric factor.
    // If that is right, this applies a second, spurious decode to every authored factor: pow(x, 2.2)
    // on x in (0,1) pulls toward zero, so a tinted material renders DARKER and more saturated than
    // authored. It is a no-op for the common {1,1,1,1}, which is why nothing has noticed.
    //
    // WHY IT IS NOT SIMPLY FLIPPED HERE: the three importers do not agree about what they hand over.
    // OBJ's `Kd` is conventionally authored in sRGB, so for that source the decode is CORRECT; glTF
    // and USD (diffuseColor) are linear, so for those it is not. One blanket rule is wrong whichever
    // way it points -- the fix belongs in each importer, converting to a single documented convention
    // before it reaches here, and it changes the appearance of every tinted material in every
    // existing project. That is a content decision, not a cleanup.
    //
    // AND NOTHING CHECKS IT: MaterialTest.cpp:587-598 packs a factor and asserts only that ALPHA is
    // exempt -- the three decoded channels are never compared against an expected value, so the
    // premise this comment states has never been tested in either direction.
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
    // Volume absorption, forwarded verbatim. NOT clamped or normalised here: attenuationColor is a
    // transmittance in [0,1] per channel and attenuationDistance is a length in centimetres, both
    // already range-checked by pbr::sanitize (Material.cpp) the way every other authored float in
    // this block is. A distance of 0 means "no volume" and the shader gates on it -- see
    // MaterialConstants::attenuationDistance.
    c.attenuationColor[0] = d.attenuationColor[0];
    c.attenuationColor[1] = d.attenuationColor[1];
    c.attenuationColor[2] = d.attenuationColor[2];
    c.attenuationDistance = d.attenuationDistance;
    // The two floats above are what _pad0/_pad1 used to be. The struct carries no padding now, so
    // every one of its members is assigned here rather than some being left at the zero the
    // `MaterialConstants c{};` above gives them -- if a field is ever added back without a line in
    // this function, it ships as a silent zero.
    // Lamp light, forwarded verbatim for the same reason: not clamped here, only floored at 0 by
    // pbr::sanitize (Material.cpp), the way every other authored float in this block already is.
    c.lightIntensity = d.lightIntensity;
    c._lightPad[0] = c._lightPad[1] = c._lightPad[2] = 0.0f;   // assigned, not left to the {} above

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
    // Same weight-alone convention again: a positive lightIntensity is what turns this material's
    // draws into ray-driven local lights (CSRdLocalLights), so the bit tracks the one field that
    // means anything on its own.
    if (d.lightIntensity > 0.0f) flags |= MaterialFlag_Light;

    // EVERY SLOT UNBOUND UNTIL SOMETHING RESIDENT-IFIES IT. packMaterial works from a MaterialDesc
    // alone and has no idea where (or whether) a texture landed in the ray path's bindless table --
    // that is the renderer's business, and it fills these in when it uploads the RT material table.
    // Defaulting them here means a material that never reaches that path still carries a defined
    // "no texture" rather than an index into whatever happened to be at 0.
    for (u32 i = 0; i < kTextureSlotCount; ++i) c.texIndex[i] = kUnboundTexture;
    c.flags = flags;

    // Reciprocal once per upload rather than per pixel. A tiling of zero or less collapses to zero
    // tiles per cm instead of producing a NaN UV.
    c.uvTilesPerCm = d.uvTiling > 0.0f ? 1.0f / d.uvTiling : 0.0f;
    return c;
}

} // namespace aver::pbr
