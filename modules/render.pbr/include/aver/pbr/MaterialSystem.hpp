#pragma once
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <array>
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
    // What a resolver hands back: the texture, and what colour it is ON AVERAGE.
    //
    // The average exists for consumers that CANNOT SAMPLE the texture -- the path tracer shades
    // from one flat colour per surface and has no texture units at all. Without it such a consumer
    // can only read baseColorFactor, and a modern material sets that to a plain white multiplier
    // and puts the whole look in the texture: every textured surface then renders pure white, both
    // far too bright and completely featureless. Measured on a real project whose forty materials
    // ALL declare `baseColorFactor 1 1 1 1`.
    struct ResolvedTexture {
        rhi::TextureHandle handle = 0;
        f32 averageLinear[3] = {1.0f, 1.0f, 1.0f};   // meaningful only when handle != 0
    };

    // Turns a texture reference into a GPU texture; a zero handle leaves the slot on its fallback.
    // The slot says what the pixels MEAN. The handle returned becomes this system's and is
    // destroyed at shutdown.
    using TextureResolver = ResolvedTexture (*)(const TextureRef& ref, TextureSlot slot, void* user);

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

    // Whether `s` is one of THIS system's binding sets. The question a consumer has to ask when it
    // is handed an OPAQUE draw binding (rhi::IRenderFeature::submitDraw forwards a handle plus raw
    // bytes) and must decide whether those bytes really are a MaterialConstants. The block's SIZE
    // cannot answer it -- any unrelated block of the same size would pass -- and nothing in the RHI
    // tags a binding with its type, so identity is the only honest test and this system is the only
    // thing that can perform it.
    bool ownsBindingSet(rhi::BindingSetHandle s) const;

    // The average LINEAR base colour a draw of `s` actually shades with: baseColorFactor times the
    // mean of its base-colour texture. False for a set this system does not own.
    //
    // This is the honest answer to "what colour is this surface" for something that cannot sample
    // a texture. baseColorFactor ALONE is not that answer and is not close to it -- see
    // ResolvedTexture above for what reading it alone actually produced.
    bool averageBaseColor(rhi::BindingSetHandle s, f32 out[3]) const;

    // The identity material's set: white base colour, flat normal, full roughness, no metal.
    rhi::BindingSetHandle fallbackBindingSet() const { return fallbackSet_; }
    const MaterialConstants& fallbackConstants() const { return fallbackConstants_; }

    u32 textureCacheSize() const { return static_cast<u32>(cache_.size()); }
    // How many cache entries are remembered FAILURES rather than textures.
    u32 failedResolveCount() const { return failedResolves_; }

    // Drops every remembered failure so the next resolve tries the filesystem again. Call this when
    // the project's content has changed underneath the running process -- a texture dropped into the
    // folder, an import finishing, a hot reload. Returns how many were dropped.
    //
    // Separate from update() on purpose: update() runs every frame, and retrying a missing file every
    // frame is the cost the negative cache exists to avoid.
    u32 forgetFailedResolves();

private:
    // One material's GPU residency.
    struct Entry {
        rhi::BindingSetHandle set = 0;
        MaterialConstants      constants{};
    };

    // Creates the four 1x1 identity textures.
    bool createFallbackTextures();
    // Writes every SRV of `set`, using the identity texture wherever the material sets nothing.
    // `retryFailed` re-resolves references whose last attempt failed, instead of trusting the
    // remembered 0. Set when a material is REDRAWN because it changed, which is the moment the
    // texture it names may finally exist.
    void writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set, bool retryFailed = false);
    // Resolves and caches one texture reference. 0 when there is no resolver or it declined.
    ResolvedTexture resolveTexture(const TextureRef& ref, TextureSlot slot, bool retryFailed = false);
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
    // Keyed by the reference — the id when set, else the path — PLUS the slot's colour class, so one
    // texture uploads once per way of decoding it. See colourClass() in the .cpp for why the second
    // half of the key is not optional.
    std::unordered_map<std::string, rhi::TextureHandle> cache_;
    // The mean of each cached texture, same key. Kept beside cache_ rather than inside it so a
    // remembered FAILURE (a cached 0) carries no colour and cannot be mistaken for a black texture.
    std::unordered_map<std::string, std::array<f32, 3>> cacheAverage_;
    // Per binding set: baseColorFactor times its base-colour texture's mean, filled by writeSlots.
    std::unordered_map<rhi::BindingSetHandle, std::array<f32, 3>> setAverage_;
    u32 failedResolves_ = 0;

    TextureResolver resolve_ = nullptr;
    void*           resolveUser_ = nullptr;
};

} // namespace aver::pbr
