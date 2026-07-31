// GPU residency of the material library: fallback textures, one binding set and one constant block
// per material, and the per-frame drain that keeps them current.
#include "aver/pbr/MaterialSystem.hpp"

#include "aver/core/Log.hpp"

namespace aver::pbr {

namespace {

// Creates a 1x1 texture whose four bytes are the identity value for its slot.
rhi::TextureHandle makePixel(rhi::IResourceFactory& res, const u8 rgba[4], rhi::Format fmt,
                             const char* name) {
    rhi::TextureDesc d;
    d.width = d.height = 1;
    d.format = fmt;
    d.bind = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    const void* levels[1] = {rgba};
    d.initialData = levels;
    d.initialDataCount = 1;
    d.initialRowPitch = 4;
    d.debugName = name;
    return res.createTexture(d);
}

// One cache key for both spellings of a reference. The id wins where set.
std::string cacheKey(const TextureRef& ref) {
    if (ref.id) return "#" + std::to_string(ref.id);
    return ref.path;
}

} // namespace

// Creates the fallback textures and the fallback binding set. False when there is no GPU.
bool MaterialSystem::init(rhi::IDevice& device, u32 tableBaseRegister) {
    rhi::IResourceFactory* res = device.resources();
    // A null factory is how a backend without GPU support declines: headless, not an error.
    if (!res) return false;
    res_ = res;
    tableBase_ = tableBaseRegister;

    if (!createFallbackTextures()) { res_ = nullptr; return false; }

    // Built from a default-constructed MaterialDesc, so it is the identity material by construction.
    const MaterialDesc identity{};
    fallbackConstants_ = packMaterial(identity);

    rhi::BindingSetDesc bd;
    bd.srvCount = kMaterialSrvCount;
    for (u32 i = 0; i < kMaterialSrvCount; ++i) bd.srvKinds[i] = rhi::SlotKind::Texture2D;
    bd.srvBaseRegister = tableBase_;
    fallbackSet_ = res_->createBindingSet(bd);
    if (!fallbackSet_) {
        AVER_ERROR("[PBR] the fallback material binding set could not be created");
        res_ = nullptr;
        return false;
    }
    writeSlots(identity, fallbackSet_);

    AVER_INFO("[PBR] material system ready: {} slots based at t{}", kMaterialSrvCount, tableBase_);
    return true;
}

// Creates the four 1x1 identity textures.
bool MaterialSystem::createFallbackTextures() {
    // sRGB for base colour, linear for the rest. White is 1.0 under either encoding, but the FORMAT
    // must match or a real map would decode differently from the fallback it replaces.
    const u8 white[4]  = {255, 255, 255, 255};
    const u8 normal[4] = {128, 128, 255, 255};
    const u8 mr[4]     = {0, 255, 255, 255};
    const u8 black[4]  = {0, 0, 0, 255};

    white_      = makePixel(*res_, white,  rhi::Format::RGBA8UnormSrgb, "pbr fallback white");
    flatNormal_ = makePixel(*res_, normal, rhi::Format::RGBA8Unorm,     "pbr fallback normal");
    metalRough_ = makePixel(*res_, mr,     rhi::Format::RGBA8Unorm,     "pbr fallback metalrough");
    black_      = makePixel(*res_, black,  rhi::Format::RGBA8Unorm,     "pbr fallback black");
    if (white_ && flatNormal_ && metalRough_ && black_) return true;
    AVER_ERROR("[PBR] the fallback textures could not be created");
    return false;
}

// Destroys every set, every cached texture and the fallbacks.
void MaterialSystem::shutdown() {
    if (!res_) return;
    for (auto& kv : entries_) if (kv.second.set) res_->destroyBindingSet(kv.second.set);
    entries_.clear();
    // Cached textures are owned here: the resolver handed the handle over.
    for (auto& kv : cache_) if (kv.second) res_->destroyTexture(kv.second);
    cache_.clear();
    if (fallbackSet_) res_->destroyBindingSet(fallbackSet_);
    fallbackSet_ = 0;
    for (rhi::TextureHandle* t : {&white_, &flatNormal_, &metalRough_, &black_}) {
        if (*t) res_->destroyTexture(*t);
        *t = 0;
    }
    res_ = nullptr;
}

// Resolves a reference through the host's resolver, caching the result. 0 when unavailable.
rhi::TextureHandle MaterialSystem::resolveTexture(const TextureRef& ref, TextureSlot slot) {
    if (ref.empty() || !resolve_) return 0;
    // The slot is not part of the key: one file bound to two slots is still one upload, first use wins.
    const std::string key = cacheKey(ref);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const rhi::TextureHandle t = resolve_(ref, slot, resolveUser_);
    // A failed resolve is cached as 0, or a missing file is retried on every dirty drain.
    cache_.emplace(key, t);
    return t;
}

// Writes every SRV of `set`, using the identity texture wherever the material sets nothing.
void MaterialSystem::writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set) {
    const rhi::TextureHandle fallback[kTextureSlotCount] = {
        white_,       // BaseColor
        metalRough_,  // MetalRough
        flatNormal_,  // Normal
        white_,       // Occlusion
        black_,       // Emissive
    };
    for (u32 i = 0; i < kTextureSlotCount; ++i) {
        const rhi::TextureHandle t = resolveTexture(d.textures[i], static_cast<TextureSlot>(i));
        res_->setSrv(set, i, t ? t : fallback[i]);
    }
}

// The entry for `h`, built and filled on first use.
MaterialSystem::Entry& MaterialSystem::entryFor(MaterialHandle h) {
    auto it = entries_.find(h);
    if (it != entries_.end()) return it->second;

    Entry e;
    rhi::BindingSetDesc bd;
    bd.srvCount = kMaterialSrvCount;
    for (u32 i = 0; i < kMaterialSrvCount; ++i) bd.srvKinds[i] = rhi::SlotKind::Texture2D;
    bd.srvBaseRegister = tableBase_;
    e.set = res_->createBindingSet(bd);
    if (e.set) {
        const MaterialDesc* d = MaterialLibrary::get().desc(h);
        if (d) { e.constants = packMaterial(*d); writeSlots(*d, e.set); }
    } else {
        AVER_ERROR("[PBR] binding set for material {} could not be created", h);
    }
    return entries_.emplace(h, e).first->second;
}

// Re-uploads every dirty material and retires the sets of destroyed ones.
void MaterialSystem::update() {
    if (!res_) return;
    MaterialLibrary& lib = MaterialLibrary::get();

    // Reading the flag clears it, so this is the one consumer. Driven off the library's own
    // enumeration, because a material created this frame has no entry yet.
    const u32 n = lib.count();
    for (u32 i = 0; i < n; ++i) {
        const MaterialHandle h = lib.at(i);
        if (!h) continue;
        const bool dirty = lib.consumeDirty(h);
        auto it = entries_.find(h);
        if (it == entries_.end()) { entryFor(h); continue; }   // freshly built, already current
        if (!dirty) continue;
        const MaterialDesc* d = lib.desc(h);
        if (!d) continue;
        it->second.constants = packMaterial(*d);
        if (it->second.set) writeSlots(*d, it->second.set);
    }

    // Destruction is deferred by RHI contract, so retiring mid-frame is safe.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (lib.valid(it->first)) { ++it; continue; }
        if (it->second.set) res_->destroyBindingSet(it->second.set);
        it = entries_.erase(it);
    }
}

// The binding set a draw of `h` uses. Falls back for an unknown or stale handle.
rhi::BindingSetHandle MaterialSystem::bindingSet(MaterialHandle h) {
    if (!res_ || !MaterialLibrary::get().valid(h)) return fallbackSet_;
    const rhi::BindingSetHandle s = entryFor(h).set;
    return s ? s : fallbackSet_;
}

// The constant block a draw of `h` uses. Falls back for an unknown or stale handle.
const MaterialConstants& MaterialSystem::constants(MaterialHandle h) {
    if (!res_ || !MaterialLibrary::get().valid(h)) return fallbackConstants_;
    return entryFor(h).constants;
}

} // namespace aver::pbr
