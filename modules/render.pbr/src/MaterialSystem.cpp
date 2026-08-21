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
MaterialSystem::ResolvedTexture MaterialSystem::resolveTexture(const TextureRef& ref, TextureSlot slot,
                                                               bool retryFailed) {
    if (ref.empty() || !resolve_) return {};
    // THE COLOUR CLASS IS PART OF THE KEY. One file bound to two slots is still one upload when both
    // read it the same way -- but a PNG bound to BOTH base colour and occlusion is decoded sRGB for
    // one and linear for the other, and a key that ignored that would hand the second slot whichever
    // encoding happened to be resolved first. That failure is invisible per pixel and wrong
    // everywhere, which is the worst shape a rendering bug can have.
    const std::string key = cacheKey(ref) + "|" + colourClass(slot);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        if (it->second || !retryFailed) {
            ResolvedTexture r;
            r.handle = it->second;
            if (const auto a = cacheAverage_.find(key); a != cacheAverage_.end())
                for (int i = 0; i < 3; ++i) r.averageLinear[i] = a->second[i];
            return r;
        }
        // A remembered failure, and the caller has reason to think it may have been fixed.
        cache_.erase(it);
        cacheAverage_.erase(key);
        if (failedResolves_) --failedResolves_;
    }
    const ResolvedTexture r = resolve_(ref, slot, resolveUser_);
    const rhi::TextureHandle t = r.handle;
    // A FAILURE IS REMEMBERED, BUT NOT FOREVER. Caching the 0 is deliberate: without it a material
    // naming a texture that does not exist re-hits the filesystem on every dirty drain. But holding
    // it for the process lifetime means a texture dropped into the project after startup never
    // appears, and "restart the editor" is not an acceptable answer to "I added a PNG". So the
    // negatives are counted, and forgetFailedResolves() drops them when content changes.
    cache_.emplace(key, t);
    if (t) cacheAverage_.emplace(key, std::array<f32, 3>{r.averageLinear[0], r.averageLinear[1], r.averageLinear[2]});
    if (!t) ++failedResolves_;
    return r;
}

bool MaterialSystem::averageBaseColor(rhi::BindingSetHandle s, f32 out[3]) const {
    const auto it = setAverage_.find(s);
    if (it == setAverage_.end()) return false;
    for (int i = 0; i < 3; ++i) out[i] = it->second[i];
    return true;
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
        const ResolvedTexture r = resolveTexture(d.textures[i], static_cast<TextureSlot>(i), retryFailed);
        res_->setSrv(set, i, r.handle ? r.handle : fallback[i]);
        if (static_cast<TextureSlot>(i) != TextureSlot::BaseColor) continue;
        // baseColorFactor TIMES the texture mean, which is what the pixel shader computes too --
        // the factor is a multiplier over the sampled texel, not an alternative to it. With no
        // base-colour texture the mean is 1 and this reduces to the factor alone, which is then
        // genuinely the whole answer.
        // packMaterial(), not d.baseColorFactor: glTF authors the factor in sRGB and packMaterial
        // decodes it. Using the raw desc value here would report a colour in a different space
        // from the one the pixel shader multiplies, which is a subtle wrongness rather than a
        // visible one -- the worst kind.
        const MaterialConstants packed = packMaterial(d);
        std::array<f32, 3> avg{packed.baseColorFactor[0], packed.baseColorFactor[1], packed.baseColorFactor[2]};
        if (r.handle) for (int c = 0; c < 3; ++c) avg[c] *= r.averageLinear[c];
        setAverage_[set] = avg;
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

// Whether `s` is one of ours. Deliberately const and lookup-only: it must NOT go through entryFor(),
// which builds an entry on first use -- this answers a question about sets that already exist, and
// materialising one to answer it would be a side effect of asking.
//
// A linear scan over the resident materials. The caller is a per-draw path, but `entries_` holds one
// entry per material actually drawn this level (tens, not thousands) and the alternative -- a second
// set-keyed index to maintain -- would have to be kept in step with every create and evict for no
// measurable gain. The fallback is checked first because the un-authored case is the common one.
bool MaterialSystem::ownsBindingSet(rhi::BindingSetHandle s) const {
    if (!s) return false;
    if (s == fallbackSet_) return true;
    for (const auto& [handle, entry] : entries_) {
        (void)handle;
        if (entry.set == s) return true;
    }
    return false;
}

// The constant block a draw of `h` uses. Falls back for an unknown or stale handle.
const MaterialConstants& MaterialSystem::constants(MaterialHandle h) {
    if (!res_ || !MaterialLibrary::get().valid(h)) return fallbackConstants_;
    return entryFor(h).constants;
}

} // namespace aver::pbr
