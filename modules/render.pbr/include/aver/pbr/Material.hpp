#pragma once
#include "aver/core/Types.hpp"

#include <string>

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
    // (1 - transmission) before the view-Fresnel term lifts it back at grazing angles. NOT YET READ
    // BY THE BRDF ITSELF: this only reshapes alpha compositing, exactly like reflectance/f90 already
    // did before this change -- there is still no refraction, and light does not actually bend
    // passing through a transmissive surface. Say that here rather than let someone assume otherwise
    // from the field's name.
    f32 transmission = 0.0f;

    // ---- subsurface scattering ----
    // WRAP DIFFUSE PLUS A BACK-LIGHT LOBE, AND NOT ONE PHOTON MORE. Both default to 0, so every
    // material authored before this existed shades bit-identically and the gate baselines do not
    // move until something opts in.
    //
    // WHAT THIS IS NOT, stated here rather than left to be inferred from the word "subsurface":
    // it is not a BSSRDF. Light does not travel THROUGH the mesh -- there is no transport from where
    // a photon enters to where it leaves, so a lit ear does not glow on the far side of a head. It
    // is a per-pixel approximation evaluated at ONE surface point, which is why it costs two floats
    // and no passes. What it does buy is the thing whose absence reads as "plastic": light wrapping
    // slightly past the terminator, and a rim that brightens when the sun is behind the object.
    //
    // NO SEPARATE SCATTER TINT, deliberately. MaterialConstants had exactly 8 spare bytes (its own
    // comment says so: "8 bytes of headroom for the next field before 96 has to become 112"), and
    // two floats spend them exactly. A third float for an authored RGB tint would grow the block to
    // 112 and force every one of its GPU mirrors to be re-derived. The transmitted light is tinted
    // by baseColorFactor instead, which is right for skin, wax, marble and leaves -- the cases this
    // is for -- and wrong only where the interior colour differs from the surface colour.
    f32 subsurfaceWeight = 0.0f;   // [0,1] how far light wraps past the terminator; 0 = off
    f32 subsurfaceRadius = 0.0f;   // [0,1] thickness proxy; widens the back-light lobe

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
