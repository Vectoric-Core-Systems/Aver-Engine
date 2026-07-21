#include "aver/pbr/MaterialSystem.hpp"

#include "aver/core/Log.hpp"

namespace aver::pbr {

namespace {

// A 1x1 texture whose four bytes ARE the identity value for its slot. Created with initialData so
// the resource never exists in a state the caller has to reason about — see rhi::TextureDesc.
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

// One key for both spellings of a reference. The id wins where set, because it survives a file
// being moved and the path does not.
std::string cacheKey(const TextureRef& ref) {
    if (ref.id) return "#" + std::to_string(ref.id);
    return ref.path;
}

} // namespace

bool MaterialSystem::init(rhi::IDevice& device, u32 tableBaseRegister) {
    rhi::IResourceFactory* res = device.resources();
    // A null factory is how a backend without GPU support declines; that is not an error, it is the
    // engine running headless, so say nothing and stay un-ready.
    if (!res) return false;
    res_ = res;
    tableBase_ = tableBaseRegister;

    if (!createFallbackTextures()) { res_ = nullptr; return false; }

    // The fallback set is built from a default-constructed MaterialDesc, so it is the identity
    // material by construction rather than by a second list of values that could drift from the
    // defaults in Material.hpp.
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

bool MaterialSystem::createFallbackTextures() {
    // sRGB for base colour so the hardware decode applies on every tap exactly as it will for an
    // authored map; linear for the rest, whose channels are data and not colour. White is 1.0 under
    // either encoding, but the FORMAT still has to match or a real map would decode differently
    // from the fallback it replaces.
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

void MaterialSystem::shutdown() {
    if (!res_) return;
    for (auto& kv : entries_) if (kv.second.set) res_->destroyBindingSet(kv.second.set);
    entries_.clear();
    // Cached textures are OWNED here: the resolver handed over a handle and nothing else holds it.
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

rhi::TextureHandle MaterialSystem::resolveTexture(const TextureRef& ref) {
    if (ref.empty() || !resolve_) return 0;
    const std::string key = cacheKey(ref);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const rhi::TextureHandle t = resolve_(ref, resolveUser_);
    // A failed resolve is cached too, as 0. Otherwise a missing file is retried on every dirty
    // drain, which on a hot-reloading editor is a disk hit per material per frame.
    cache_.emplace(key, t);
    return t;
}

void MaterialSystem::writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set) {
    const rhi::TextureHandle fallback[kTextureSlotCount] = {
        white_,       // BaseColor
        metalRough_,  // MetalRough
        flatNormal_,  // Normal
        white_,       // Occlusion
        black_,       // Emissive
    };
    for (u32 i = 0; i < kTextureSlotCount; ++i) {
        const rhi::TextureHandle t = resolveTexture(d.textures[i]);
        res_->setSrv(set, i, t ? t : fallback[i]);
    }
}

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

void MaterialSystem::update() {
    if (!res_) return;
    MaterialLibrary& lib = MaterialLibrary::get();

    // Reading the flag CLEARS it, so this is the one consumer and a change is drained exactly once.
    // Driven off the library's own enumeration rather than off entries_, because a material created
    // this frame has no entry yet and must still get one before anything draws with it.
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

    // Retire the sets of materials the library no longer knows. Destruction is deferred by RHI
    // contract, so doing it mid-frame is safe; leaking them is not, since a long editing session
    // creates and destroys materials freely.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (lib.valid(it->first)) { ++it; continue; }
        if (it->second.set) res_->destroyBindingSet(it->second.set);
        it = entries_.erase(it);
    }
}

rhi::BindingSetHandle MaterialSystem::bindingSet(MaterialHandle h) {
    if (!res_ || !MaterialLibrary::get().valid(h)) return fallbackSet_;
    const rhi::BindingSetHandle s = entryFor(h).set;
    return s ? s : fallbackSet_;
}

const MaterialConstants& MaterialSystem::constants(MaterialHandle h) {
    if (!res_ || !MaterialLibrary::get().valid(h)) return fallbackConstants_;
    return entryFor(h).constants;
}

} // namespace aver::pbr
