#pragma once
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <array>
#include <unordered_map>
#include <vector>

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

    // THE EFFECTIVE TEXTURE IN EVERY SLOT of a binding set, fallbacks included, or nullptr for a
    // set this system did not build. Exists for the ray path, which cannot bind a per-draw
    // descriptor table and instead needs the handles themselves so it can put them in its own
    // bindless array.
    //
    // EFFECTIVE, not authored: a slot the material never set reports the identity texture that
    // writeSlots actually bound, not 0. That is what makes the ray path sample the same thing the
    // raster path does without needing a branch per slot -- and an unset normal map sampling flat
    // (128,128,255) is the whole reason those fallbacks exist.
    const std::array<rhi::TextureHandle, kTextureSlotCount>* textures(rhi::BindingSetHandle set) const;
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

    // ---- dense GPU material table ----
    // A consumer that shades from a NUMBER rather than a MaterialHandle -- a ray hit walking an
    // instance buffer, which has no room for a 4-byte handle-with-generation next to its geometry
    // indices, only for a small dense index -- cannot use bindingSet()/constants() at all: those are
    // per-draw binding-table lookups, not something a StructuredBuffer<MaterialConstants> read at an
    // arbitrary GPU thread can index. This is that other shape of the same data: every live
    // material's MaterialConstants, packed contiguously in one array a caller uploads as-is.
    //
    // ALL THREE ACCESSORS BELOW ARE READ ONLY, and none of them force GPU residency the way
    // bindingSet()/constants() do (those build an Entry, including a binding set, on first ask this
    // system has never been asked to draw a material before). A material nobody has drawn yet still
    // gets a row here the next time update() runs, because the row order is driven off
    // MaterialLibrary's own enumeration, not off who has asked for a binding set.

    // `h`'s row in gpuMaterialTable(), or 0 (the fallback/identity material's row -- see
    // gpuMaterialTable() below) for a zero, stale, or otherwise-unrecognised handle. Exactly the
    // same "unknown handle gets the fallback" rule bindingSet()/constants() already apply.
    //
    // STABLE ONLY BETWEEN ONE update() CALL AND THE NEXT -- see gpuMaterialRevision() for why, and
    // for how a caller that needs to hold an index (or a buffer built from gpuMaterialTable())
    // across more than one frame learns whether it is still correct. Named `gpuMaterialIndex`
    // rather than reusing pbr::materialIndex() (Material.hpp) on purpose: that free function decodes
    // a HANDLE's own encoded slot, a completely different number that survives exactly as long as
    // the handle itself does. This one is a GPU row, current for one frame, and confusing the two
    // would hand a ray hit the wrong material with no way to notice.
    u32 gpuMaterialIndex(MaterialHandle h) const;

    // How many rows gpuMaterialTable() holds: 0 before update() has ever run, otherwise
    // 1 + MaterialLibrary::count() as of the last call (row 0 is always the fallback material, so
    // the "+1" is not the live count moving under a caller that reads it between update() calls).
    u32 gpuMaterialCount() const { return static_cast<u32>(gpuTable_.size()); }

    // Row 0 is always fallbackConstants() -- the identity material -- so gpuMaterialIndex()'s
    // fallback answer of 0 is always safe to read without a bounds check on the consuming side.
    // Rows 1..gpuMaterialCount()-1 are every live material, in MaterialLibrary's own dense
    // enumeration order (its count()/at()) as of the last update(). A caller builds its
    // StructuredBuffer<MaterialConstants> by copying gpuMaterialCount() elements starting here --
    // the exact shape IResourceFactory::setSrvBuffer already expects (a pointer, a stride of
    // sizeof(MaterialConstants), and a count), the same recipe already used for the ray-tracing
    // instance and vertex tables. Valid only until the next call to update() or shutdown(); this
    // system owns the storage, a caller does not keep the pointer.
    const MaterialConstants* gpuMaterialTable() const { return gpuTable_.data(); }

    // Bumped by update() exactly when the SET of live materials changed since the PREVIOUS
    // update() -- a create, a destroy, or (because MaterialLibrary::destroy() compacts its dense
    // list to keep count()/at() dense) a destroy that shifted every LATER material's row down by
    // one, without that material itself having changed at all. touch()/MaterialLibrary::update() on
    // a material's own factors or textures never moves its row, only its CONTENTS at the row it
    // already has, so an ordinary edit does not bump this.
    //
    // THIS IS HOW A CONSUMER LEARNS AN INDEX WENT STALE. Nothing else does: gpuMaterialIndex()'s
    // answer for a given handle, and the layout of gpuMaterialTable(), are only guaranteed to agree
    // with each other and with the GPU-side geometry that was built to match them between one
    // update() and the next. A consumer that caches an index (or a buffer built from
    // gpuMaterialTable()) across a frame boundary without checking this counter first risks reading
    // a DIFFERENT material's row after a create/destroy reshuffled the table -- and that is not a
    // crash, it is one object silently wearing another object's colour, metalness and roughness,
    // which is exactly the failure this system exists to prevent for the per-DRAW path
    // (bindingSet()/constants() never have this problem, because a draw always looks its material up
    // by handle, fresh, every time). The robust pattern -- and the one this codebase already uses
    // for the instance table itself, see VoxiRenderer::buildAccelerationStructures rebuilding
    // RtInstance from source every call rather than caching anything across frames -- is to rebuild
    // the index and the buffer fresh every frame and never read this at all. It exists for a
    // consumer that wants to SKIP that rebuild when nothing changed, and that consumer must check it
    // first every time, not just the first time.
    u32 gpuMaterialRevision() const { return gpuRevision_; }

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
    // Per binding set, the handle writeSlots actually bound into each slot. Same keying and same
    // lifetime as setAverage_ above.
    std::unordered_map<rhi::BindingSetHandle, std::array<rhi::TextureHandle, kTextureSlotCount>> setTextures_;
    u32 failedResolves_ = 0;

    TextureResolver resolve_ = nullptr;
    void*           resolveUser_ = nullptr;

    // The dense GPU material table (see gpuMaterialTable() above) and its handle-to-row index,
    // rebuilt together by update() from the SAME walk over MaterialLibrary's enumeration so they can
    // never disagree with each other. gpuIndexOf_ is deliberately a SEPARATE map from entries_
    // rather than one more field bolted onto Entry: entries_ can hold a handle that has never been
    // through update() at all (bindingSet()/constants() build one lazily, on first ask, for a
    // material nobody has drawn yet), and such an entry must answer gpuMaterialIndex() with the
    // fallback row 0, not with a stale or never-set number that happens to live in the same struct.
    // Keying the row lookup off its own map, populated ONLY by update(), makes that the only place
    // a handle can appear in it, so "not in gpuIndexOf_" and "not on this frame's table" are the
    // same fact instead of two facts that a future edit could pull apart.
    std::unordered_map<MaterialHandle, u32> gpuIndexOf_;
    std::vector<MaterialConstants> gpuTable_;
    // See gpuMaterialRevision(): bumped by update() only when gpuIndexOf_'s CONTENT (not merely its
    // size) differs from what it held before that call.
    u32 gpuRevision_ = 0;
};

} // namespace aver::pbr
