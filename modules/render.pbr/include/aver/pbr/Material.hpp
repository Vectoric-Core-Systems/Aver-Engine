#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

// Aver.Render.PBR — what a surface IS: its factors, its texture references, how it blends, and the
// process-wide library holding them. Core-only; no `aver/rhi/*` include may ever appear here.
// Field names match docs/formats/FORMAT_SPECS.md section 7 (`.ocmat`) PARAM names one-for-one.
namespace aver::pbr {

#if defined(_WIN32)
#  if defined(AVER_PBR_BUILD)
#    define AVER_PBR_API __declspec(dllexport)
#  else
#    define AVER_PBR_API __declspec(dllimport)
#  endif
#else
#  define AVER_PBR_API
#endif

// How the base colour's alpha is interpreted. Matches `.ocmat` BLEND.
enum class AlphaMode : u32 { Opaque = 0, Mask, Blend };

// The glTF metallic-roughness texture set. Count is the slot count.
// The glTF metallic-roughness set, then a SECOND LAYER of the same maps.
//
// THE SECOND LAYER IS WHAT MAKES TERRAIN LOOK LIKE TERRAIN. One material over a heightfield gives
// one surface everywhere -- leaf litter running up a cliff face, or rock on flat ground. Real
// terrain is at minimum two surfaces chosen by SLOPE, and every engine that draws landscapes has
// some form of this.
//
// A MATERIAL FEATURE, NOT A LANDSCAPE FEATURE, and that is the design decision worth recording.
// The obvious alternative was to give Aver.Landscape.Renderer its own pipeline and shader so it
// could sample several material tables -- but a landscape-owned pipeline takes over the scene PSO
// wholesale (VoxiRenderer::overridesScenePipeline is all-or-nothing), which would have cost the
// terrain every bit of Voxi's GI and shadowing. Blending inside the material instead means the
// landscape keeps drawing through exactly the path it already does, still lit by GI, still using
// the opaque single-binding seam it already has -- and any MESH can use a layered material too.
enum class TextureSlot : u32 {
    BaseColor = 0, MetalRough, Normal, Occlusion, Emissive,
    Layer1BaseColor, Layer1MetalRough, Layer1Normal,
    Count
};

// Where a surface's texture coordinates come from: the mesh's own UVs, or a planar projection.
// FROZEN: UvMode::WorldAligned, AVER_PBR_UV_WORLD_ALIGNED and the `.ocmat` key `worlduv`.
enum class UvMode : u32 { Mesh = 0, WorldAligned };

inline constexpr u32 kTextureSlotCount = static_cast<u32>(TextureSlot::Count);

// Handle-with-generation; 0 is invalid. The generation lives in bits 20..30 and starts at 1, so a
// valid handle stays non-zero and non-negative for the int32_t C ABI.
using MaterialHandle = u32;

inline constexpr u32 kMaterialIndexBits = 20;
inline constexpr u32 kMaterialIndexMask = (1u << kMaterialIndexBits) - 1u;
inline constexpr u32 kMaterialGenerationMask = 0x7FFu;   // 11 bits, bit 31 stays clear

// Packs an index and a generation into a handle.
constexpr MaterialHandle makeMaterialHandle(u32 index, u32 generation) {
    return (index & kMaterialIndexMask) | ((generation & kMaterialGenerationMask) << kMaterialIndexBits);
}
constexpr u32 materialIndex(MaterialHandle h) { return h & kMaterialIndexMask; }
constexpr u32 materialGeneration(MaterialHandle h) { return (h >> kMaterialIndexBits) & kMaterialGenerationMask; }

// A texture reference: an authoring path and an opaque 64-bit id, neither interpreted here.
struct TextureRef {
    std::string path;   // empty = unset
    u64         id = 0; // 0 = unset

    bool empty() const { return id == 0 && path.empty(); }
};

// ---- wave3 terrain layers ----
// A LAYERED TERRAIN MATERIAL: up to 8 named layers blended per pixel by a section's weightmaps and a
// height-based mix (docs/MATERIALS.md). A material feature for the reason the slope layer above is one:
// it rides the scene PSO, so the landscape keeps GI, shadows and ray hits. Paint data in `.ocland`
// names layers by NAME, so reordering or editing the layers here never scrambles paint.
inline constexpr u32 kMaxTerrainLayers = 8;

struct TerrainLayerDesc {
    std::string name, surface, foliage;   // surface: physical-surface name, empty = the layer's name
    TextureRef baseColor, normal, metalRough, height;
    f32 tilingCm = 400.0f;                // world cm per texture tile
    f32 heightBias = 0.0f;
    f32 friction = 0.6f, restitution = 0.0f;
    f32 foliageDensity = 0.0f;            // instances per 100 m^2
    f32 foliageMinWeight = 0.5f;

    const std::string& surfaceName() const { return surface.empty() ? name : surface; }
};

struct TerrainDesc {
    u32 tileTexels = 512;                 // 256 | 512 | 1024
    u32 padTexels = 8;                    // 4..16, even: wrapped texels around each tile in the atlas
    f32 blendDepth = 0.15f;               // 0..1 height-blend depth; large = plain linear weights
    std::vector<TerrainLayerDesc> layers; // file order = layer index; the first is the base

    bool enabled() const { return !layers.empty(); }
};
// ---- end wave3 terrain layers ----

// How a surface is lit. Standard is the metallic-roughness model every material has always used. Hair is the strand model of the shared
// evaluator (AVER_MODEL_HAIR in material_prelude.hlsl): Kajiya-Kay diffuse plus the R, TT and TRT lobes. It is a property of the MATERIAL, not of
// the draw, so neither backend's per-draw constants change: packMaterial sets MaterialFlag_Hair and the shader picks the model from the flag.
enum class ShadingModel : u32 { Standard = 0, Hair };

// The named parameters of a Hair material (read only when shadingModel is Hair). packMaterial writes them into slots of the constant block the hair
// model has no other use for (docs/plans/wave11/SOFTBODY_HAIR.md 2.17), so the 192-byte block and its mirrors do not change.
// Colours are sRGB like baseColorFactor. The root colour is baseColorFactor.
struct HairParams {
    f32 tipColor[3] = {0.55f, 0.40f, 0.25f};   // the colour at the tip; the strand blends from the root colour to this
    f32 colorVariation = 0.15f;                // [0,1] per-strand brightness variation
    f32 roughness = 0.35f;                     // beta: the width of the longitudinal highlight (the R lobe; TT is half, TRT twice)
    f32 shift = 0.07f;                         // alpha: the cuticle tilt in radians, which moves the R and TRT highlights along the strand
    f32 specular = 1.0f;                       // the weight of the white R lobe
    f32 tt = 1.0f;                             // the weight of the lobe through the fibre
    f32 trt = 1.0f;                            // the weight of the coloured lobe that bounces off the far wall
    f32 azimuthRough = 0.4f;                   // [0,1] how wide the azimuthal lobes are
    f32 tint[3] = {0.9f, 0.6f, 0.4f};          // the colour the TT and TRT lobes take on top of the strand colour
    f32 scatter = 0.5f;                        // [0,1] the multiple-scatter ambient: lifts the shadow floor of a thick head of hair
};

// The authored surface. Defaults are glTF's.
struct MaterialDesc {
    std::string name;

    f32 baseColorFactor[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 emissiveFactor[3]   = {0.0f, 0.0f, 0.0f};
    f32 metallicFactor      = 1.0f;
    f32 roughnessFactor     = 1.0f;
    f32 normalScale         = 1.0f;
    f32 occlusionStrength   = 1.0f;

    f32 reflectance         = 0.04f;   // F0 of the dielectric base
    f32 f90                 = 1.0f;    // F(90); 1.0 is the textbook Schlick term

    // Index of refraction and how much light passes straight through rather than being absorbed or
    // diffusely scattered. NOT INDEPENDENT OF reflectance ABOVE: F0 = ((1-ior)/(1+ior))^2, and 1.5 is
    // exactly the ior for which 0.04 is already the right F0 -- ordinary soda-lime glass. Change one
    // of the pair without the other and the surface's Fresnel curve stops matching its own stated
    // base reflectance; nothing here derives one from the other automatically, so an author (or a
    // future .ocmat importer) that sets ior alone and expects reflectance to follow will be wrong.
    f32 ior          = 1.5f;
    // 0 = opaque as far as this field goes, which is every material authored before it existed. Read
    // only where it matters -- see MaterialConstants::transmission and averBuildSurface's
    // AVER_MAT_ALPHA_BLEND branch in PbrShaders.cpp, which pulls the blended coverage down toward
    // (1 - transmission) before the view-Fresnel term lifts it back at grazing angles. It also scales
    // the diffuse lobe by (1 - transmission) (kdAlbedo in material_prelude.hlsl). Refraction is a
    // separate switch (voxi Settings::refractionMode).
    f32 transmission = 0.0f;

    // ---- volume absorption: what the INTERIOR of a transmissive material does to light ----
    //
    // glTF's KHR_materials_volume, and the natural companion to the KHR_materials_transmission split
    // `transmission` above already implements. `transmission` says light passes THROUGH the surface;
    // these two say what happens to it on the way across the inside.
    //
    // WHY THIS IS NOT A TINT. A coloured sheet removes the same fraction of each channel however far
    // you look through it. A volume removes light exponentially in distance and per channel -- red
    // dies within about a metre of water, blue survives tens of metres -- which is the entire reason
    // water reads cyan and reads DEEPER cyan the further you look through it, why thick glass goes
    // green at its edges, and why a thin sliver of jade is pale where a block is saturated. One
    // exp() gets all of that; no amount of tuning a constant colour does, because the constant
    // cannot know the distance.
    //
    // attenuationColor is the transmittance after light has travelled exactly attenuationDistance
    // through the medium, so the extinction the shader wants is
    //     extinction = -log(attenuationColor) / attenuationDistance
    // and the transmittance over an arbitrary path is exp(-extinction * thickness). Authoring it as
    // "the colour at a known distance" rather than as a raw extinction coefficient is glTF's choice
    // and it is the right one: an artist can see a colour, and nobody can see a reciprocal centimetre.
    //
    // ZERO IS OFF, not "infinitely dense". glTF's own default is an infinite attenuationDistance,
    // which is the same thing said in a way that does not survive a zero-initialised struct -- and a
    // zero-initialised material must mean "no volume", because every material authored before this
    // existed has these bytes as zero and must shade bit-identically. The shader gates on
    // attenuationDistance > 0, which is also why this costs no MaterialFlag bit: the off state is
    // already representable in the data.
    //
    // DISTANCE IS IN CENTIMETRES, this engine's world unit throughout -- see uvTilesPerCm and
    // FluidVolume's halfExtentCm for the same convention. Water is roughly
    // attenuationColor (0.30, 0.73, 0.80) at attenuationDistance 100.
    f32 attenuationColor[3] = {1.0f, 1.0f, 1.0f};   // transmittance at exactly attenuationDistance
    f32 attenuationDistance = 0.0f;                 // cm; <= 0 disables volume absorption entirely

    // SINGLE SCATTERING IN THAT VOLUME. Absorption alone makes water a dark filter; the cyan haze you
    // see looking into a sea is light scattered toward the eye from inside it. scatterColor is the
    // per-channel single-scattering ALBEDO sigma_s / sigma_t (the share of an extinction event that
    // scatters instead of absorbing), LINEAR and not sRGB-decoded: it is a ratio, not a colour. The
    // extinction itself still comes from attenuationColor/Distance, so scatter needs a volume
    // (attenuationDistance > 0) and is ignored without one. 0 (the default) is off and every term it
    // gates is skipped, so a material that never set it shades bit-identically. Open water is about
    // (0.05, 0.35, 0.55) beside attenuationColor (0.65, 0.93, 0.97) at 100.
    // scatterAnisotropy is the Henyey-Greenstein g of the phase function: 0 isotropic, > 0 forward
    // (sunlit haze brightens looking toward the sun), < 0 back.
    f32 scatterColor[3] = {0.0f, 0.0f, 0.0f};
    f32 scatterAnisotropy = 0.0f;                   // [-0.99, 0.99]

    // ---- subsurface scattering: skin, wax, marble, leaves, curtains, paper ----
    // Weight 0 (the default) is off, so every material authored before this existed shades
    // bit-identically and the gate baselines do not move until something opts in.
    //
    // THREE TERMS, ALL PER PIXEL, NO EXTRA PASS AND NO EXTRA RAY (material_prelude.hlsl's
    // averDirectTerms/averSubsurfaceAmbient):
    //   - WRAP: light reaching past the terminator, tinted by subsurfaceColor -- the soft, reddened
    //     shadow edge that stops skin reading as plastic.
    //   - TRANSMISSION: light arriving on the FAR side and diffusing through -- a leaf, curtain or
    //     lampshade glowing with the sun behind it -- plus a forward-scatter lobe when the viewer looks
    //     toward the light through it. Its shadow is asked from the side FACING THE LIGHT, pushed
    //     subsurfaceRadius's depth through the surface (averSubsurfaceDepthCm), using the same one
    //     shadow query the pixel already makes: a sheet thinner than that depth transmits, a body
    //     thicker than it (a head, a statue) meets its own far side and does not.
    //   - AMBIENT TRANSMISSION: the same through-light from the sky and bounce, so thin foliage in
    //     shade is lighter than an opaque leaf would be.
    //
    // WHAT THIS IS NOT: a BSSRDF. There is no screen-space blur and no transport from where light
    // enters to where it leaves along the surface; what it buys is what reads as translucency.
    //
    // subsurfaceColor is the colour light takes INSIDE the material, authored like baseColorFactor
    // (sRGB, decoded by packMaterial) and multiplied onto the diffuse albedo: white (the default)
    // scatters in the surface's own colour; skin wants a deep red, leaves a yellow-green, wax an
    // orange. It spends the three pad floats the lamp-light row carried, so the block stays 176 bytes.
    f32 subsurfaceWeight = 0.0f;   // [0,1] strength of all three terms; 0 = off
    f32 subsurfaceRadius = 0.0f;   // [0,1] scatter depth 0.25-5 cm; also widens the forward lobe
    f32 subsurfaceColor[3] = {1.0f, 1.0f, 1.0f};   // sRGB tint of the scattered light

    // ---- the coat: a second specular layer over everything above ----
    // A CLEAR LACQUER ON TOP OF THE BASE MATERIAL, which is what a car body, a varnished table, a
    // phone back or a wet stone all are: a smooth dielectric film over something rougher and often
    // coloured. The base keeps its own metallic/roughness response; the coat adds a second, usually
    // much smoother, GGX lobe over it and attenuates what shows through by its own Fresnel.
    //
    // ALL THREE DEFAULT TO OFF, so every material authored before this shades bit-identically --
    // packMaterial only sets MaterialFlag_Coat when coatWeight > 0, and the shader term is behind
    // that flag, so there is nothing to compute and nothing to round differently.
    //
    // READ ONLY WHEN Settings::layeredBsdf IS NOT Off. These are authored per material, but whether
    // the renderer evaluates them at all is a project-wide decision -- see voxi::Settings for why
    // that switch is global and why it is not live-switchable.
    //
    // coatF0 is authored rather than derived from a coat IOR, which is the same split baseColor's
    // `reflectance` already has. Worth knowing it is a split: 0.04 is IOR 1.5, ordinary lacquer.
    f32 coatWeight    = 0.0f;   // [0,1] how much coat there is; 0 = no coat, and the flag stays clear
    f32 coatRoughness = 0.0f;   // [0,1] the coat's own roughness, independent of the base's
    f32 coatF0        = 0.04f;  // normal-incidence reflectance of the coat film itself

    // ---- lamp light: a material that CASTS light, not merely looks bright ----
    // Emissive already makes a surface look bright; this makes it light its surroundings, the way
    // the sun does, without waiting for GI to carry it. 0 is off -- every material authored before
    // this field existed. A positive value is a MULTIPLIER on the light the material's own glow
    // (emissiveFactor) and size (the draw's world bounding sphere) already, physically, cast at
    // 1 metre in the SUN's OWN UNITS (SkyAtmosphere::sunIntensity): 1 lights exactly that, 2 lights
    // twice that, so a lamp and the sun still sit on one dial instead of a separate "intensity" scale
    // nobody could calibrate against anything. Consumed only by the staged ray-driven local-light pass
    // (CSRdLocalLights, D3D12): a draw whose material has this set becomes a small sphere light --
    // its world bounding sphere, coloured by emissiveFactor -- that pass sums and shadows (the
    // physical-output math is VoxiRenderer::buildLocalLights', not this struct's). Raster, the
    // single-pass megakernel and PSMainVoxi's blended reuse never read it, so a lamp still shades
    // exactly as its emissive factor says everywhere those paths run. A lamp whose bounding sphere is
    // larger than its visible emitter (e.g. a whole fixture carrying the material, not just the bulb)
    // over-lights at 1 and wants either a lower value or a separate bulb mesh to carry the light.
    f32 lightIntensity = 0.0f;   // multiplier on the glow+size output; 0 = not a light

    AlphaMode alphaMode   = AlphaMode::Opaque;
    f32       alphaCutoff = 0.5f;   // read only under AlphaMode::Mask
    bool      twoSided    = false;
    bool      castShadow  = true;

    UvMode uvMode   = UvMode::Mesh;
    f32    uvTiling = 200.0f;   // world CENTIMETRES per tile, read only under WorldAligned

    // ---- the second layer, blended by SLOPE ----
    // Off unless slopeBlend is true. See MaterialConstants::slopeBlendLo/Hi for the axis: these are
    // world-normal Z, so 1 is flat ground and 0 is a vertical face, and `lo` is the STEEPER end.
    bool slopeBlend    = false;
    f32  slopeBlendLo  = 0.55f;
    f32  slopeBlendHi  = 0.80f;
    f32  layer1UvScale = 1.0f;

    TextureRef textures[kTextureSlotCount];

    // WHICH MATERIAL GRAPH SHADES THIS, or 0 for none -- which is every material anyone has ever
    // authored, and still most of them. See pbr::MaterialGraphRegistry.
    //
    // RUNTIME-ASSIGNED, UNLIKE EVERY OTHER FIELD HERE, and the distinction is worth stating because
    // this struct is otherwise purely what a file said. What the FILE says is a PATH (.ocmat's
    // GRAPHREF record); the id is what the registry hands back for that path in THIS process, and
    // it means nothing in another one. So a loader fills this in after parsing, and nothing ever
    // writes it back out to a file.
    u32 graphId = 0;

    // wave3 terrain layers: non-empty `terrain.layers` makes this a layered terrain material (the
    // texture slots are then reinterpreted, see MaterialFlag_TerrainLayers).
    TerrainDesc terrain;

    // wave11 strand hair: Standard (the default, every material before this existed) or Hair, with the parameters above. packMaterial sets
    // MaterialFlag_Hair for Hair, marks the material two sided and never sets the Coat or Subsurface flags for it.
    ShadingModel shadingModel = ShadingModel::Standard;
    HairParams hair;
};

// ------------------------------------------------------- the one translucency test, and its answer
//
// THERE WAS NO CANONICAL PREDICATE, and that is why these exist. `alphaMode == AlphaMode::Blend` was
// spelled out at six separate sites in four shapes -- the editor's colour walk, its depth-prepass
// walk, a SECOND independent shadow exclusion for off-screen casters, the packaged game's walk, the
// GPU flag packer, and the path tracer (which additionally ignored the authored `ior` and substituted
// a hardcoded constant). Six copies of a rule is six chances for one of them to drift, and one of
// them had already drifted: the off-screen caster path excluded blended draws for its own reasons,
// so a culled pane behaved differently from a visible one.
//
// Every one of those files already includes this header, so putting the test here costs no new
// dependency edge.

// True when light passes through this surface in any amount: authored translucency, OR measurable
// transmission on a material that is nominally opaque.
//
// NOT "is the blend mode Blend". Transmission is a SUBSTRATE property -- it describes what the
// material is made of, not how the rasteriser composites it -- and an opaque-blended material may
// legitimately author it. Keying the rule on the authored field rather than the blend mode is what
// keeps this from being a special case for a thing called "glass".
AVER_PBR_API bool isTranslucent(const MaterialDesc& d);

// The RGB fraction of sunlight ONE crossing of this surface lets through. {1,1,1} means it casts no
// shadow at all; {0,0,0} means it casts a solid one.
//
// The rule, entirely in terms of authored fields:
//   castShadow == false            -> {1,1,1}. This is the field's FIRST consumer: it has a parser, a
//                                     writer, a C ABI, a C# property, four test assertions and four
//                                     doc entries, and until now `packMaterial` never forwarded it,
//                                     so it changed no pixel anywhere.
//   otherwise  k = max(1 - baseColorFactor.a, transmission)
//              T = baseColorFactor.rgb * k
//
// So a clear pane attenuates a little and tints not at all; a blue pane at alpha 0.2 passes 0.8 of
// the sun, blue-tinted; and an opaque material authored with transmission > 0 casts a partial shadow
// too. AlphaMode::Mask stays binary and is handled by the depth-prepass clip, not here.
//
// THIS IS THE SURFACE MODEL, AND IT IS NOW THE FALLBACK RATHER THAN THE WHOLE ANSWER.
//
// It is thickness-free by construction -- "ONE crossing" is the contract, and this function has no
// ray, so it cannot know how far light actually travelled inside anything. That is fine for a sheet
// with no authored medium, which is what it is for.
//
// A material that authors a VOLUME (attenuationColor / attenuationDistance, just above) is a
// different question, and the answer lives where the distance is knowable: rtShadow in
// modules/render.voxi/shaders/voxi.hlsl gathers the ray's entry and exit t through each medium and
// applies averVolumeTransmittance -- the same Beer-Lambert the VIEW path uses, so the light going
// DOWN through water agrees with the light coming back UP through it. Before that, water shadowed
// its own pool floor with THIS rule, and (0.38,0.60,0.66)*0.92 has nothing to do with 130 cm of
// water; the floor read almost black.
//
// So the HLSL is deliberately NO LONGER identical in shape to this, and its own comment says which
// branch is which. The two agree exactly on materials with no volume -- which is what this function's
// test assertions cover -- and the ray path adds a branch this one structurally cannot have.
AVER_PBR_API void shadowTransmittance(const MaterialDesc& d, f32 outRgb[3]);

// The material features the editor and the bindings may advertise.
enum class Feature : u32 {
    Factors = 0,      // the scalar/vector PARAM block
    BaseColorMap,
    MetalRoughMap,
    NormalMap,
    OcclusionMap,
    EmissiveMap,
    AlphaMask,
    AlphaBlend,
    Count
};

// How far along a feature is.
enum class Status : u32 {
    Ready = 0,        // authored here AND consumed by the renderer
    NotImplemented,   // the material system stores it, nothing renders it yet
    Unsupported,      // cannot be done at all
};

// Process-wide material store, addressed by handle. Not thread-safe.
class AVER_PBR_API MaterialLibrary {
public:
    static MaterialLibrary& get();

    // Creates a material. 0 on failure, which only happens when the index space is exhausted.
    MaterialHandle create(const MaterialDesc& desc);
    // Destroys a material and bumps its slot's generation.
    bool destroy(MaterialHandle h);
    // True while `h` still names a live material.
    bool valid(MaterialHandle h) const;

    // Reads a material, or nullptr for a stale handle. Invalidated by any create().
    const MaterialDesc* desc(MaterialHandle h) const;
    // Replaces the whole description and marks the material dirty.
    bool update(MaterialHandle h, const MaterialDesc& desc);
    // Mutate in place; the caller must mark it dirty itself via touch().
    MaterialDesc* mutableDesc(MaterialHandle h);
    // Sanitises the description and marks it dirty.
    void touch(MaterialHandle h);

    // True when the caller still owes the GPU an upload. Reading the flag clears it.
    bool consumeDirty(MaterialHandle h);
    // Advances whenever a material is created, destroyed, updated or touched, i.e. whenever
    // consumeDirty() could newly answer true or count()/at() could change.
    u64 changeCount() const;

    // How many materials are live. Indices are dense over live materials and shift on destroy.
    u32 count() const;
    // The handle at a live index, or 0.
    MaterialHandle at(u32 i) const;

    // How far along a feature is.
    static Status status(Feature f);
    // The human sentence for a feature's status.
    static const char* statusText(Feature f);
    // The human name of a feature.
    static const char* featureName(Feature f);
    // The `.ocmat` TEX name of a slot.
    static const char* textureSlotName(TextureSlot s);
    // The `.ocmat` BLEND name of an alpha mode.
    static const char* alphaModeName(AlphaMode m);

private:
    MaterialLibrary();
    ~MaterialLibrary();
    MaterialLibrary(const MaterialLibrary&) = delete;
    MaterialLibrary& operator=(const MaterialLibrary&) = delete;

    // Pimpl: a dllexported class with standard-library members exports their layout too (C4251).
    struct Impl;
    Impl* impl_;
};

} // namespace aver::pbr
