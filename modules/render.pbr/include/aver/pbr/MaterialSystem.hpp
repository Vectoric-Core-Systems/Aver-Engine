#pragma once
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <unordered_map>

// The GPU residency of the material library: one binding set and one packed constant block per
// material, plus the identity textures every unset slot falls back to.
namespace aver::pbr {

// A material's SRV table is the glTF set in TextureSlot order: a slot index IS its register offset.
inline constexpr u32 kMaterialSrvCount = kTextureSlotCount;
static_assert(kMaterialSrvCount <= rhi::kMaxBindingSlots,
              "a material table must fit in one binding set");

// Owns a binding set and a constant block per material, and keeps them current with the library.
class MaterialSystem {
public:
    // Turns a texture reference into a GPU texture; 0 leaves the slot on its fallback. The slot says
    // what the pixels MEAN. The handle returned becomes this system's and is destroyed at shutdown.
    using TextureResolver = rhi::TextureHandle (*)(const TextureRef& ref, TextureSlot slot, void* user);

    // Creates the fallback textures and the fallback set. `tableBaseRegister` is the consuming
    // pipeline's table-0 SRV count, carried on every set.
    bool init(rhi::IDevice& device, u32 tableBaseRegister);
    // Destroys every set, every cached texture and the fallbacks.
    void shutdown();
    bool ready() const { return res_ != nullptr; }

    void setTextureResolver(TextureResolver fn, void* user) { resolve_ = fn; resolveUser_ = user; }

    // Drains MaterialLibrary::consumeDirty() and re-uploads whatever changed, once per frame.
    void update();

    // The binding set a draw of `h` uses. An unknown or stale handle gets the fallback.
    rhi::BindingSetHandle bindingSet(MaterialHandle h);
    // The constant block a draw of `h` uses. An unknown or stale handle gets the fallback.
    const MaterialConstants& constants(MaterialHandle h);

    // The identity material's set: white base colour, flat normal, full roughness, no metal.
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
    // Writes every SRV of `set`, using the identity texture wherever the material sets nothing.
    void writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set);
    // Resolves and caches one texture reference. 0 when there is no resolver or it declined.
    rhi::TextureHandle resolveTexture(const TextureRef& ref, TextureSlot slot);
    // The entry for `h`, built on first use.
    Entry& entryFor(MaterialHandle h);

    rhi::IResourceFactory* res_ = nullptr;
    u32 tableBase_ = 0;

    // 1x1, created before any material. The metal-rough one is (0,255,255,255): glTF packs
    // occlusion in R, roughness in G and metallic in B.
    rhi::TextureHandle white_ = 0;       // base colour and occlusion
    rhi::TextureHandle flatNormal_ = 0;  // (128,128,255) — +Z in tangent space
    rhi::TextureHandle metalRough_ = 0;
    rhi::TextureHandle black_ = 0;       // emissive

    rhi::BindingSetHandle fallbackSet_ = 0;
    MaterialConstants     fallbackConstants_{};

    std::unordered_map<MaterialHandle, Entry> entries_;
    // Keyed by the reference — the id when set, else the path — so one texture uploads once.
    std::unordered_map<std::string, rhi::TextureHandle> cache_;

    TextureResolver resolve_ = nullptr;
    void*           resolveUser_ = nullptr;
};

} // namespace aver::pbr
