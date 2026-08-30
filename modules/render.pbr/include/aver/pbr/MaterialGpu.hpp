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

    // Set when subsurfaceWeight > 0. A FLAG RATHER THAN A BRANCH ON THE FLOAT so the shader's
    // subsurface term compiles out entirely for the overwhelming majority of materials that do not
    // want it -- the same shape every other optional term in this block uses.
    MaterialFlag_Subsurface          = 1u << 14,

    // Set when coatWeight > 0, for the reason stated one field up: a flag, not a branch on the
    // float, so the coat lobe compiles out for every material that never asked for one.
    MaterialFlag_Coat                = 1u << 15,
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
    // THAT FREE LUNCH IS SPENT, AND SO IS THE PADDING AFTER IT. graphId used the last of the space
    // the compiler was packing for free; ior/transmission below needed a real 16 bytes more, which is
    // why the struct grew to 96; and subsurfaceWeight/subsurfaceRadius have since spent the 8 bytes of
    // explicit padding that arrival left over. There is no slack left. The next field added to this
    // struct takes it to 112 and obliges whoever adds it to revisit every GPU mirror.
    u32 graphId;

    // ---- dielectric transmission, read only where AVER_MAT_ALPHA_BLEND is set ----
    // Mirrors MaterialDesc::ior/transmission. ior travels with the block for completeness (and for
    // whatever future refraction pass wants it) but nothing reads it yet -- the only consumer today
    // is transmission, via averBuildSurface's alpha computation in PbrShaders.cpp. See
    // MaterialDesc::ior's own comment for why the two are not independent.
    f32 ior;
    f32 transmission;

    // ---- subsurface, and the headroom this block reserved ----
    // These two floats ARE the 8 bytes the padding above used to hold. The comment that stood here
    // said "8 bytes of headroom for the next field before 96 has to become 112"; this is that next
    // field, and it fits exactly, so sizeof stays 96 and both static_asserts below are unchanged
    // rather than re-derived. There is now NO padding left: the next field added here grows the
    // block to 112 and every GPU mirror of it has to be revisited.
    //
    // Mirrors MaterialDesc::subsurfaceWeight/subsurfaceRadius; read by averDirectTerms in
    // PbrShaders.cpp under AVER_MAT_SUBSURFACE, which is the one BRDF the raster path, PSMainVoxi
    // and the cluster path all share.
    f32 subsurfaceWeight;
    f32 subsurfaceRadius;

    // ---- the coat, and the growth to 112 the block above said was coming ----
    //
    // THIS IS THE FIELD THAT SPENT THE LAST OF IT. The comment above ends "the next field added here
    // grows the block to 112 and every GPU mirror of it has to be revisited". This is that field, and
    // they were: the HLSL cbuffer in PbrShaders.cpp, RtMaterial in VoxiShaders.hpp, packMaterial in
    // MaterialGpu.cpp, both static_asserts below and the runtime one in tests/formats.
    //
    // 112 AND NOT 128: three floats and one pad is exactly one 16-byte row, which is the smallest
    // legal growth. Reserving a second row "for the next lobe" would be inventing a requirement --
    // sheen and anisotropy have different shapes and neither is designed yet, so the row they need is
    // not knowable now and a guessed one would be either wrong or wasted.
    //
    // CHEAP TO CARRY, AND THAT IS CHECKED RATHER THAN ASSUMED: kMaxDrawConstantBytes is 256
    // (RHIResources.hpp), enforced in both backends, so at 112 there are still 144 bytes of headroom
    // at the register every material-shaded draw already binds. No new binding, no new descriptor,
    // nothing to plumb. The cost when the layered BSDF is off is 16 more bytes per material on an
    // upload that already happens -- not zero, and not worth pretending is.
    //
    // Read only where MaterialFlag_Coat is set, which packMaterial only sets when coatWeight > 0.
    f32 coatWeight;      // [0,1]; 0 is off and every coat term is then identically zero
    f32 coatRoughness;   // [0,1]; the coat has its own GGX lobe, independent of the base
    f32 coatF0;          // normal-incidence reflectance of the coat itself; 0.04 is ordinary lacquer
    f32 _coatPad;        // keeps the row 16 bytes; not read anywhere

    // ---- where each texture lives in the ray path's bindless table ----
    //
    // ONE u32 PER TextureSlot, in slot order, so slot N is texIndex[N] with no mapping table. The
    // RASTER PATH DOES NOT READ THESE and never will: it binds a per-material descriptor table per
    // draw and addresses its textures by register, which is cheaper and works on tier-1 hardware.
    // These exist for the one case that cannot do that -- a ray hit, where a single fullscreen pass
    // shades every material in the scene and has no per-draw table to bind.
    //
    // kUnboundTexture, not 0, for absent. Zero is a REAL index into the table (whatever landed
    // there first), so a zero-initialised material would silently sample another material's
    // base colour rather than fall back -- the kind of wrong that looks like a content bug.
    u32 texIndex[kTextureSlotCount];
};

// No texture in that slot. Deliberately not 0; see MaterialConstants::texIndex.
inline constexpr u32 kUnboundTexture = 0xFFFFFFFFu;

// SIZED FROM THE ENUM, so adding a TextureSlot is a compile error here rather than a silent
// mismatch against the HLSL mirror. 8 is asserted separately because the 144-byte figure below
// depends on it: a ninth slot is a deliberate decision about the constant-buffer size, not a
// change to wave through.
static_assert(kTextureSlotCount == 8, "texIndex sizing and the 144-byte block below assume 8 slots");
static_assert(sizeof(MaterialConstants) == 144, "the HLSL cbuffer mirrors this byte for byte");
static_assert(sizeof(MaterialConstants) % 16 == 0, "must be a legal constant-buffer size");

// Packs the authored description into the block the GPU reads. A slot counts as bound when either
// reference form is set.
MaterialConstants packMaterial(const MaterialDesc& d);

} // namespace aver::pbr
