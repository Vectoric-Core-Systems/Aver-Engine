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
enum class TextureSlot : u32 { BaseColor = 0, MetalRough, Normal, Occlusion, Emissive, Count };

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

    AlphaMode alphaMode   = AlphaMode::Opaque;
    f32       alphaCutoff = 0.5f;   // read only under AlphaMode::Mask
    bool      twoSided    = false;
    bool      castShadow  = true;

    UvMode uvMode   = UvMode::Mesh;
    f32    uvTiling = 200.0f;   // world CENTIMETRES per tile, read only under WorldAligned

    TextureRef textures[kTextureSlotCount];
};

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
