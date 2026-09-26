// GPU residency of the material library: fallback textures, one binding set and one constant block
// per material, and the per-frame drain that keeps them current.
#include "aver/pbr/MaterialSystem.hpp"

#include "aver/core/Log.hpp"

#include <chrono>

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

// Creates the three 1x1 identity textures. There is no separate black fallback: every slot,
// emissive included, multiplies a factor by its map, so the map's unbound value must be the
// multiplicative identity (white) or an unmapped factor reads as zero. See writeSlots() below.
bool MaterialSystem::createFallbackTextures() {
    // sRGB for base colour, linear for the rest. White is 1.0 under either encoding, but the FORMAT
    // must match or a real map would decode differently from the fallback it replaces.
    const u8 white[4]  = {255, 255, 255, 255};
    const u8 normal[4] = {128, 128, 255, 255};
    const u8 mr[4]     = {0, 255, 255, 255};

    white_      = makePixel(*res_, white,  rhi::Format::RGBA8UnormSrgb, "pbr fallback white");
    flatNormal_ = makePixel(*res_, normal, rhi::Format::RGBA8Unorm,     "pbr fallback normal");
    metalRough_ = makePixel(*res_, mr,     rhi::Format::RGBA8Unorm,     "pbr fallback metalrough");
    if (white_ && flatNormal_ && metalRough_) return true;
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
    for (rhi::TextureHandle* t : {&white_, &flatNormal_, &metalRough_}) {
        if (*t) res_->destroyTexture(*t);
        *t = 0;
    }
    // Not GPU resources -- just CPU-side bookkeeping -- but left non-empty they would answer
    // gpuMaterialCount()/gpuMaterialTable() with a stale table from a device that no longer exists,
    // and ready() cannot warn a caller who reads those two inline getters directly instead of
    // checking it first. Clearing them makes "0 rows" the honest answer for the gap between this
    // shutdown() and whatever update() eventually re-populates the table after the next init().
    gpuIndexOf_.clear();
    gpuTable_.clear();
    // W10's scratch pair holds the PREVIOUS generation's backing storage between update() calls (see
    // its own comment in the header) -- left non-empty here it would be exactly as stale as
    // gpuIndexOf_/gpuTable_ above would have been, for the same reason.
    indexOfScratch_.clear();
    tableScratch_.clear();
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
        // Emissive multiplies emissiveFactor like BaseColor/Occlusion multiply theirs, so it falls
        // back to the identity too. It used to be black, which zeroed every emissiveFactor-only
        // material (a lamp bulb with no emissive texture).
        white_,       // Emissive
        white_,       // Layer1BaseColor
        metalRough_,  // Layer1MetalRough
        flatNormal_,  // Layer1Normal
    };
    static_assert(sizeof(fallback) / sizeof(fallback[0]) == kTextureSlotCount,
                  "every TextureSlot needs a fallback; a short list zero-fills and binds nothing");
    std::array<rhi::TextureHandle, kTextureSlotCount> effective{};
    for (u32 i = 0; i < kTextureSlotCount; ++i) {
        const ResolvedTexture r = resolveTexture(d.textures[i], static_cast<TextureSlot>(i), retryFailed);
        const rhi::TextureHandle bound = r.handle ? r.handle : fallback[i];
        res_->setSrv(set, i, bound);
        // Recorded from the SAME expression that binds it, on the same line of reasoning, so the two
        // cannot drift into disagreeing about what this slot holds.
        effective[i] = bound;
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
    setTextures_[set] = effective;
}

// The effective texture in every slot of `set`; see the header for why fallbacks are included.
const std::array<rhi::TextureHandle, kTextureSlotCount>* MaterialSystem::textures(
        rhi::BindingSetHandle set) const {
    const auto it = setTextures_.find(set);
    return it == setTextures_.end() ? nullptr : &it->second;
}

// The entry for `h`, built and filled on first use.
MaterialSystem::Entry& MaterialSystem::entryFor(MaterialHandle h) {
    auto it = entries_.find(h);
    if (it != entries_.end()) return it->second;

    // M2(b): this is the ONE place a material's GPU residency is actually built, whether the call
    // came from update()'s own loop or -- lazily, for a material nobody has drawn yet -- from
    // bindingSet()/constants() at a draw site. inUpdate_ (set only around update()'s loop, see its
    // own comment) is what tells the two apart for the log line's split.
    const auto t0 = std::chrono::steady_clock::now();

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

    buildMsPending_ += std::chrono::duration<f64, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
    ++buildsPending_;
    if (inUpdate_) ++buildsInUpdate_; else ++buildsOnDraw_;

    return entries_.emplace(h, e).first->second;
}

// Re-uploads every dirty material, retires the sets of destroyed ones, and rebuilds the dense GPU
// material table gpuMaterialTable()/gpuMaterialIndex() read. All three jobs share the one walk over
// MaterialLibrary's enumeration below because that enumeration order IS the table's row order (row 0
// is the fallback, rows 1..n follow lib.at(0..n-1)) -- a second walk elsewhere to build the table
// would just be a second place this could disagree with the first.
void MaterialSystem::update() {
    if (!res_) return;
    const auto t0 = std::chrono::steady_clock::now();
    MaterialLibrary& lib = MaterialLibrary::get();

    // W10 / DO-NOT-DO #21: tableScratch_/indexOfScratch_ are the SECOND, PERSISTENT container of the
    // alternating pair -- built fresh into here every call, then swapped with gpuIndexOf_/gpuTable_
    // below, never compared against or filled into the SAME object twice in a row. See the header's
    // comment on tableScratch_/indexOfScratch_ for the bug a single reused container produces.
    tableScratch_.clear();
    const u32 n = lib.count();
    tableScratch_.reserve(n + 1u);
    tableScratch_.push_back(fallbackConstants_);   // row 0, always -- see gpuMaterialTable()'s own comment
    indexOfScratch_.clear();
    indexOfScratch_.reserve(n);

    // M2(b): a burst of Entry builds may have gone quiet between the previous update() call and this
    // one -- a project's materials finished streaming in, or a run of first-draw builds stopped. This
    // is checked BEFORE this call's own loop runs (which may start a new burst of its own), so a
    // quiet call reports the OLD burst and a busy one keeps accumulating instead of reporting early.
    if (buildsPending_ > 0 && buildsPending_ == buildsPendingAtLastUpdateEnd_) {
        AVER_INFO("[PBR] {} material(s) built in {:.1f} ms ({} inside update(), {} on first draw)",
                  buildsPending_, buildMsPending_, buildsInUpdate_, buildsOnDraw_);
        buildsPending_ = 0;
        buildMsPending_ = 0.0;
        buildsInUpdate_ = 0;
        buildsOnDraw_ = 0;
    }

    // Reading the flag clears it, so this is the one consumer. Driven off the library's own
    // enumeration, because a material created this frame has no entry yet.
    //
    // inUpdate_ brackets exactly this loop -- see entryFor()'s own comment -- so a build entryFor()
    // performs here is counted "inside update()" and one a draw site triggers between update() calls
    // is counted "on first draw", and the two can never be confused with each other.
    inUpdate_ = true;
    for (u32 i = 0; i < n; ++i) {
        const MaterialHandle h = lib.at(i);
        if (!h) continue;
        const bool dirty = lib.consumeDirty(h);
        auto it = entries_.find(h);
        if (it == entries_.end()) {
            entryFor(h);                     // freshly built, already current
            it = entries_.find(h);
        } else if (dirty) {
            const MaterialDesc* d = lib.desc(h);
            if (d) {
                it->second.constants = packMaterial(*d);
                // retryFailed: this material CHANGED, which is exactly when a texture it names that
                // was missing before may now exist -- a fresh import, a file copied in, a corrected
                // path.
                if (it->second.set) writeSlots(*d, it->second.set, true);
            }
        }
        indexOfScratch_.emplace(h, static_cast<u32>(tableScratch_.size()));
        tableScratch_.push_back(it->second.constants);
    }
    inUpdate_ = false;

    // Destruction is deferred by RHI contract, so retiring mid-frame is safe.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (lib.valid(it->first)) { ++it; continue; }
        if (it->second.set) res_->destroyBindingSet(it->second.set);
        it = entries_.erase(it);
    }

    // The table's LAYOUT changed -- a create or a destroy since the last call, not merely an edit to
    // a material already on it -- exactly when the handle-to-row mapping itself differs from what it
    // held before. Comparing the two maps directly is simpler and harder to get wrong than tracking
    // every insert/erase by hand as the loop above goes: std::unordered_map::operator== already
    // compares by CONTENT (every key present in both, with equal values), not bucket order or size
    // alone, which is exactly "did any material's row move" and nothing more or less than that.
    //
    // THE TWO SIDES OF THIS COMPARISON MUST BE DISTINCT OBJECTS, which is the entire reason
    // indexOfScratch_ is a separate persistent member rather than a local rebuilt every call: a
    // container compared against itself is always equal to itself, gpuRevision_ would stop
    // advancing, and every material created after the first update() call would never reach
    // gpuIndexOf_/gpuTable_ at all (DO-NOT-DO #21).
    if (indexOfScratch_ != gpuIndexOf_) ++gpuRevision_;
    gpuIndexOf_.swap(indexOfScratch_);
    gpuTable_.swap(tableScratch_);

    buildsPendingAtLastUpdateEnd_ = buildsPending_;

    // M2(a): total update() cost, on the same power-of-two cadence D3D12Device.cpp already uses for
    // its shader-compile report. UNMEASURED against a profiler -- this is a wall-clock straddle of
    // the function body and nothing more.
    const f64 ms = std::chrono::duration<f64, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    ++updateCalls_;
    if ((updateCalls_ & (updateCalls_ - 1)) == 0)
        AVER_INFO("[PBR] material update {:.3f} ms over {} material(s)", ms, n);
}

// `h`'s row in gpuMaterialTable() as of the last update(), or 0 (the fallback row) for a handle this
// call has never placed there -- a zero/stale handle, or one whose Entry (if any) was only ever built
// lazily by bindingSet()/constants() and has not yet been through an update() call. Deliberately does
// NOT go through entryFor(): like ownsBindingSet(), this answers a question about the CURRENT table,
// and materialising an Entry (or a row) as a side effect of asking would be wrong for the same reason
// it would be wrong there.
u32 MaterialSystem::gpuMaterialIndex(MaterialHandle h) const {
    if (!res_ || !h) return 0;
    const auto it = gpuIndexOf_.find(h);
    return it != gpuIndexOf_.end() ? it->second : 0;
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
