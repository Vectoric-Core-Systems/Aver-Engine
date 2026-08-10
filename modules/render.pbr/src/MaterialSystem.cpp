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

// How a slot's pixels are DECODED, which is the only reason two bindings of one file may not share
// an upload. Three classes, mirroring the switch every resolver makes -- SandboxApp.cpp:1167 and
// GameContent.cpp:229 both map base colour and emissive to Colour, normal to NormalMap, and
// everything else to Data. Anything coarser than this merges two colour spaces; anything finer
// uploads the same pixels twice for no gain.
//
// KEEP THIS IN STEP WITH THOSE TWO SWITCHES. A resolver that classified differently would make this
// key split on a distinction the upload does not actually make.
char colourClass(TextureSlot slot) {
    switch (slot) {
        case TextureSlot::BaseColor:
        case TextureSlot::Layer1BaseColor:
        case TextureSlot::Emissive:  return 'c';   // sRGB
        case TextureSlot::Normal:
        case TextureSlot::Layer1Normal: return 'n';   // linear, never sRGB whatever the options say
        default:                     return 'd';   // linear data: metal-rough, occlusion
    }
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
rhi::TextureHandle MaterialSystem::resolveTexture(const TextureRef& ref, TextureSlot slot,
                                                  bool retryFailed) {
    if (ref.empty() || !resolve_) return 0;
    // THE COLOUR CLASS IS PART OF THE KEY. One file bound to two slots is still one upload when both
    // read it the same way -- but a PNG bound to BOTH base colour and occlusion is decoded sRGB for
    // one and linear for the other, and a key that ignored that would hand the second slot whichever
    // encoding happened to be resolved first. That failure is invisible per pixel and wrong
    // everywhere, which is the worst shape a rendering bug can have.
    const std::string key = cacheKey(ref) + "|" + colourClass(slot);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        if (it->second || !retryFailed) return it->second;
        // A remembered failure, and the caller has reason to think it may have been fixed.
        cache_.erase(it);
        if (failedResolves_) --failedResolves_;
    }
    const rhi::TextureHandle t = resolve_(ref, slot, resolveUser_);
    // A FAILURE IS REMEMBERED, BUT NOT FOREVER. Caching the 0 is deliberate: without it a material
    // naming a texture that does not exist re-hits the filesystem on every dirty drain. But holding
    // it for the process lifetime means a texture dropped into the project after startup never
    // appears, and "restart the editor" is not an acceptable answer to "I added a PNG". So the
    // negatives are counted, and forgetFailedResolves() drops them when content changes.
    cache_.emplace(key, t);
    if (!t) ++failedResolves_;
    return t;
}

// Drops every remembered failure. The 0s are not GPU resources, so nothing is destroyed here.
u32 MaterialSystem::forgetFailedResolves() {
    u32 dropped = 0;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second) { ++it; continue; }
        it = cache_.erase(it);
        ++dropped;
    }
    failedResolves_ = 0;
    if (dropped) AVER_INFO("[PBR] {} failed texture resolve(s) forgotten; they will be retried", dropped);
    return dropped;
}

// Writes every SRV of `set`, using the identity texture wherever the material sets nothing.
void MaterialSystem::writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set, bool retryFailed) {
    // ONE ENTRY PER SLOT, and the static_assert is why this list is worth reading twice: a short
    // initialiser list zero-fills the tail rather than failing to compile, so adding a TextureSlot
    // and forgetting this array binds handle 0 into the new SRVs -- an invalid descriptor under
    // Tier 1, which is a device-removal-class bug rather than a wrong pixel.
    const rhi::TextureHandle fallback[kTextureSlotCount] = {
        white_,       // BaseColor
        metalRough_,  // MetalRough
        flatNormal_,  // Normal
        white_,       // Occlusion
        black_,       // Emissive
        white_,       // Layer1BaseColor
        metalRough_,  // Layer1MetalRough
        flatNormal_,  // Layer1Normal
    };
    static_assert(sizeof(fallback) / sizeof(fallback[0]) == kTextureSlotCount,
                  "every TextureSlot needs a fallback; a short list zero-fills and binds nothing");
    for (u32 i = 0; i < kTextureSlotCount; ++i) {
        const rhi::TextureHandle t = resolveTexture(d.textures[i], static_cast<TextureSlot>(i), retryFailed);
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
        // retryFailed: this material CHANGED, which is exactly when a texture it names that was
        // missing before may now exist -- a fresh import, a file copied in, a corrected path.
        if (it->second.set) writeSlots(*d, it->second.set, true);
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
