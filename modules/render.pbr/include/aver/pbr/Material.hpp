#pragma once
#include "aver/core/Types.hpp"

#include <string>

// Aver.Render.PBR — the MATERIAL SYSTEM.
//
// PBR is a material system; Voxi is the thing that renders it. This module owns what a surface IS
// (its factors, its texture references, how it blends) and, in its sibling static target, the BRDF
// that shades it. It deliberately knows nothing about the RHI: no `aver/rhi/*` include may ever
// appear here, because that is the only thing keeping render-hardware types off the P/Invoke
// boundary the C# scripting layer binds to.
//
// Field names match docs/formats/FORMAT_SPECS.md section 7 (`.ocmat`) PARAM names one-for-one, so
// the loader that lands later is a rename-free mapping rather than a translation table nobody can
// audit.
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

// How the base colour's alpha is interpreted. Matches `.ocmat` BLEND: opaque / masked / translucent
// (additive is a separate authoring mode, not an alpha rule, so it is not one of these).
enum class AlphaMode : u32 { Opaque = 0, Mask, Blend };

// The glTF metallic-roughness texture set, which is what the asset pipeline imports. Count is the
// slot count, so an array indexed by this is exactly the right size.
enum class TextureSlot : u32 { BaseColor = 0, MetalRough, Normal, Occlusion, Emissive, Count };

// Where a surface's texture coordinates come from.
//
// Mesh is the mesh's own UV set and is right for anything that was unwrapped. WorldAligned projects
// world position onto the dominant axis of the surface normal instead, at a fixed number of
// centimetres per tile — which is the only thing that gives a BLOCKOUT a constant texel density.
// A level built from one unit cube scaled to a floor, a wall and a crate has the same 0..1 UVs on
// all three, so a mesh-UV material stretches one tile of texture over a sixteen-metre floor and
// packs the same tile into a fifty-centimetre crate. Unreal calls its version world-aligned
// texturing and it exists for exactly this reason.
//
// Dominant-axis PROJECTION rather than triplanar BLENDING: a blockout is axis-aligned boxes, where
// projection is exact and seamless, and blending would cost three samples per map instead of one.
enum class UvMode : u32 { Mesh = 0, WorldAligned };

inline constexpr u32 kTextureSlotCount = static_cast<u32>(TextureSlot::Count);

// Handle-with-generation: materials are INSTANCES that can be destroyed and their slot reused, so a
// bare index would let a stale reference silently address a different material. 0 is invalid.
//
// The generation lives in bits 20..30 and starts at 1, which makes a valid handle both non-zero and
// non-negative — it crosses the C ABI as int32_t, and a negative handle there would read as an
// error code in every FFI that follows the setters-return-1/0 convention.
using MaterialHandle = u32;

inline constexpr u32 kMaterialIndexBits = 20;
inline constexpr u32 kMaterialIndexMask = (1u << kMaterialIndexBits) - 1u;
inline constexpr u32 kMaterialGenerationMask = 0x7FFu;   // 11 bits, bit 31 stays clear

constexpr MaterialHandle makeMaterialHandle(u32 index, u32 generation) {
    return (index & kMaterialIndexMask) | ((generation & kMaterialGenerationMask) << kMaterialIndexBits);
}
constexpr u32 materialIndex(MaterialHandle h) { return h & kMaterialIndexMask; }
constexpr u32 materialGeneration(MaterialHandle h) { return (h >> kMaterialIndexBits) & kMaterialGenerationMask; }

// A texture reference held as BOTH an authoring path AND an opaque 64-bit id, with this module
// interpreting NEITHER. That is what keeps the DLL Core-only: resolving either one needs the asset
// system, which lives a tier up. The id is forward-compatible with an ObjectId or an `.octex` GUID
// without this header having to name one, and the path is the fallback `.ocmat` already specifies.
struct TextureRef {
    std::string path;   // empty = unset
    u64         id = 0; // 0 = unset

    bool empty() const { return id == 0 && path.empty(); }
};

// The authored surface. Defaults are glTF's, so an import that omits a field lands on the value the
// exporter assumed rather than on something this engine invented.
struct MaterialDesc {
    std::string name;

    f32 baseColorFactor[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 emissiveFactor[3]   = {0.0f, 0.0f, 0.0f};
    f32 metallicFactor      = 1.0f;
    f32 roughnessFactor     = 1.0f;
    f32 normalScale         = 1.0f;
    f32 occlusionStrength   = 1.0f;

    // The dielectric base reflectance, and the reflectance at grazing incidence. Authored rather
    // than hardcoded because 0.04 / 1.0 is one material, not a law: water is ~0.02, skin ~0.028,
    // gemstones ~0.17, and none of them can be expressed while the shading model owns the number.
    // This pair is the single clearest argument for the material system being a module at all --
    // a renderer has no business knowing what a surface is made of.
    //
    // f90 below 1 is what stops a ROUGH dielectric growing a bright rim at grazing angles: Schlick
    // drives every surface to full white reflectance at 90 degrees, which is true of a smooth one
    // and visibly wrong on a rough one. glTF's defaults are kept so an import that says nothing
    // lands where the exporter assumed.
    f32 reflectance         = 0.04f;   // F0 of the dielectric base
    f32 f90                 = 1.0f;    // F(90); 1.0 is the textbook Schlick term

    AlphaMode alphaMode   = AlphaMode::Opaque;
    f32       alphaCutoff = 0.5f;   // read only under AlphaMode::Mask
    bool      twoSided    = false;
    bool      castShadow  = true;

    // See UvMode. Mesh is the default so an imported asset keeps the parameterisation it was baked
    // against; nothing about an unwrapped mesh should change because this field was added.
    UvMode uvMode   = UvMode::Mesh;
    f32    uvTiling = 200.0f;   // world CENTIMETRES per tile, read only under WorldAligned

    TextureRef textures[kTextureSlotCount];
};

// What the material system can actually do right now, reported per feature so the editor and the
// bindings never advertise something that would silently do nothing. Same three-way answer Voxi
// gives, and for the same reason.
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

enum class Status : u32 {
    Ready = 0,        // authored here AND consumed by the renderer
    NotImplemented,   // the material system stores it, nothing renders it yet
    Unsupported,      // cannot be done at all
};

// Process-wide material store. Single instance so the editor, the runtime, the renderer and the C
// ABI all address the same materials — mirrors voxi::Renderer::get(), but the state here is a
// COLLECTION of instances rather than one global settings block, so everything below is by handle.
//
// Not thread-safe, matching the rest of the module tier: the editor and the render thread reach it
// through the frame's own ordering, not through a lock.
class AVER_PBR_API MaterialLibrary {
public:
    static MaterialLibrary& get();

    // 0 on failure (only when the index space is exhausted).
    MaterialHandle create(const MaterialDesc& desc);
    bool destroy(MaterialHandle h);
    bool valid(MaterialHandle h) const;

    // nullptr for a stale or never-issued handle. The pointer is invalidated by any create().
    const MaterialDesc* desc(MaterialHandle h) const;
    // Replaces the whole description and marks the material dirty.
    bool update(MaterialHandle h, const MaterialDesc& desc);
    // Mutate in place; the caller must mark it dirty itself via touch(). Used by the C ABI setters.
    MaterialDesc* mutableDesc(MaterialHandle h);
    void touch(MaterialHandle h);

    // True when the caller still owes the GPU an upload for this material. Mirrors
    // voxi::Renderer::consumeMsaaDirty(): the flag is per material, and reading it clears it, so
    // exactly one consumer acts on each change.
    bool consumeDirty(MaterialHandle h);

    // Enumeration for the editor. Indices are dense over LIVE materials and are not stable across
    // a destroy, so a caller holding one across frames must hold the handle instead.
    u32 count() const;
    MaterialHandle at(u32 i) const;

    static Status status(Feature f);
    static const char* statusText(Feature f);
    static const char* featureName(Feature f);
    static const char* textureSlotName(TextureSlot s);
    static const char* alphaModeName(AlphaMode m);

private:
    MaterialLibrary();
    ~MaterialLibrary();
    MaterialLibrary(const MaterialLibrary&) = delete;
    MaterialLibrary& operator=(const MaterialLibrary&) = delete;

    // Pimpl on purpose: the storage is std::vector/std::string, and a dllexported class with
    // standard-library members exports their layout too (MSVC C4251). Keeping them behind an opaque
    // pointer means only this DLL ever allocates or frees them.
    struct Impl;
    Impl* impl_;
};

} // namespace aver::pbr
