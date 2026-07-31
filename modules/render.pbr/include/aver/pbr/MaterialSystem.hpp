#pragma once
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <unordered_map>

// The GPU residency of the material library: one binding set and one packed constant block per
// material, plus the identity textures every unset slot falls back to. Not an rhi::IRenderFeature —
// a material is not a pass.
namespace aver::pbr {

// A material's SRV table is the glTF metallic-roughness set in TextureSlot order, so a slot index
// IS its shader register offset.
inline constexpr u32 kMaterialSrvCount = kTextureSlotCount;
static_assert(kMaterialSrvCount <= rhi::kMaxBindingSlots,
              "a material table must fit in one binding set");

// Owns a binding set and a constant block per material, and keeps them current with the library.
class MaterialSystem {
public:
    // Turns a texture reference into a GPU texture. The host installs it, because resolving either
    // reference form needs the asset system. 0 means "not available" and the slot keeps its
    // fallback. The slot is passed because only it says what the pixels MEAN (sRGB colour, linear
    // data, a normal field). The handle returned becomes this system's and is destroyed at shutdown.
    using TextureResolver = rhi::TextureHandle (*)(const TextureRef& ref, TextureSlot slot, void* user);

    // Creates the fallback textures and the fallback set. `tableBaseRegister` is the consuming
    // pipeline's table-0 SRV count, carried on every set so the backend can catch a wrong table index.
    bool init(rhi::IDevice& device, u32 tableBaseRegister);
    // Destroys every set, every cached texture and the fallbacks.
    void shutdown();
    bool ready() const { return res_ != nullptr; }

    void setTextureResolver(TextureResolver fn, void* user) { resolve_ = fn; resolveUser_ = user; }

    // Drains MaterialLibrary::consumeDirty() and re-uploads whatever changed. Once per frame,
    // before any draw.
    void update();

    // The binding set a draw of `h` uses. An unknown or stale handle gets the fallback.
    rhi::BindingSetHandle bindingSet(MaterialHandle h);
    // The constant block a draw of `h` uses. An unknown or stale handle gets the fallback.
    const MaterialConstants& constants(MaterialHandle h);

    // The identity material's set: white base colour, flat normal, full roughness, no metal, no
    // emission. This is what rhi::IDevice::setDefaultDrawBinding should be given.
    rhi::BindingSetHandle fallbackBindingSet() const { return fallbackSet_; }
    const MaterialConstants& fallbackConstants() const { return fallbackConstants_; }

    u32 textureCacheSize() const { return static_cast<u32>(cache_.size()); }

private:
    // One material's GPU residency.
    struct Entry {
        rhi::BindingSetHandle set = 0;
        MaterialConstants      constants{};
    };

    // Creates the four 1x1 identity textures.
    bool createFallbackTextures();
    // Writes every SRV of `set`, using the identity texture wherever the material sets nothing. A
    // null descriptor reads zero on most hardware, which would give a black base colour and a
    // zero-length normal that normalize() turns into NaN.
    void writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set);
    // Resolves and caches one texture reference. 0 when there is no resolver or it declined.
    rhi::TextureHandle resolveTexture(const TextureRef& ref, TextureSlot slot);
    // The entry for `h`, built on first use.
    Entry& entryFor(MaterialHandle h);

    rhi::IResourceFactory* res_ = nullptr;
    u32 tableBase_ = 0;

    // 1x1, created before any material. The metal-rough one is (0,255,255,255): glTF packs
    // occlusion in R, roughness in G and metallic in B, so it reads roughness 1 and metallic 1.
    rhi::TextureHandle white_ = 0;       // base colour and occlusion
    rhi::TextureHandle flatNormal_ = 0;  // (128,128,255) — +Z in tangent space
    rhi::TextureHandle metalRough_ = 0;
    rhi::TextureHandle black_ = 0;       // emissive

    rhi::BindingSetHandle fallbackSet_ = 0;
    MaterialConstants     fallbackConstants_{};

    std::unordered_map<MaterialHandle, Entry> entries_;
    // Keyed by the reference, not the resolved handle, so two materials naming one texture share
    // one upload. The id and the path are two spellings of one key; the id wins when set.
    std::unordered_map<std::string, rhi::TextureHandle> cache_;

    TextureResolver resolve_ = nullptr;
    void*           resolveUser_ = nullptr;
};

} // namespace aver::pbr
