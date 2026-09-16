// ThumbnailCache implementation: the dedicated preview, the one-asset-per-frame mesh queue, the
// CopyPass that is meant to turn a transient render into a texture that survives past this frame,
// and the separate one-per-frame texture queue that decodes and uploads directly with no preview
// involved at all.
//
// See ThumbnailCache.hpp for the whole design rationale (registration order, one asset per frame,
// a second dedicated ActorPreview). This file is the mechanism the header promises -- and it is
// NOT COMPLETE. CopyPass::prePass below cannot perform the copy it is named for: see its own
// comment for why, and the top-level report accompanying this file for the one-line header change
// that would unblock it. Everything else -- registration order, the queue, the cap, the destination
// texture -- is implemented and correct on its own.
#include "ThumbnailCache.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace aver::editor {

namespace {

// The source file's last-write-time as an opaque, comparable u64 -- see Entry::srcMtime's own
// comment for why 0 doubling as "unstamped" and "unreadable" is safe here. std::error_code rather
// than the throwing overload: a texture mid-write by another process, or deleted between the
// browser listing it and this stat, is exactly the kind of thing a thumbnailer must shrug off, not
// throw through UI code over.
u64 fileMtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<u64>(t.time_since_epoch().count());
}

// Downscales `src` to fit within kThumbnailPx x kThumbnailPx -- NEVER upscaling, so a small source
// icon sits at its own size rather than blurring out to fill a box it was never meant to fill --
// and letterboxes the result into a kThumbnailPx-square, transparent-padded RGBA8 buffer so every
// texture thumbnail is the same shape as a mesh one: this header's own kMaxThumbnails comment
// prices the whole budget on every resident entry being exactly that size.
//
// AREA-AVERAGED IN THE ENCODED BYTES DIRECTLY, unlike modules/formats/Texture.cpp's mip-chain
// downsample(), which filters in linear space because a rendered mip has to be radiometrically
// right. A content-browser tile is a picture of a texture, not an input to lighting, so the extra
// linear round trip would spend cycles on a correctness nobody looking at a 168dp tile can see.
std::vector<u8> letterboxThumbnail(const ImageData& src) {
    std::vector<u8> out(static_cast<usize>(kThumbnailPx) * kThumbnailPx * 4, 0);   // transparent pad
    if (!src.valid()) return out;

    const f32 scale = std::min(1.0f, std::min(static_cast<f32>(kThumbnailPx) / static_cast<f32>(src.width),
                                               static_cast<f32>(kThumbnailPx) / static_cast<f32>(src.height)));
    // CLAMPED, not just rounded -- offX/offY below subtract dstW/dstH from kThumbnailPx as unsigned
    // math, and float rounding at scale's boundary case (the constraining dimension lands AT
    // kThumbnailPx) landing one texel over would underflow that into a giant offset and a
    // buffer overrun rather than a merely-wrong picture.
    const u32 dstW = std::min(kThumbnailPx, std::max<u32>(1, static_cast<u32>(static_cast<f32>(src.width) * scale + 0.5f)));
    const u32 dstH = std::min(kThumbnailPx, std::max<u32>(1, static_cast<u32>(static_cast<f32>(src.height) * scale + 0.5f)));
    const u32 offX = (kThumbnailPx - dstW) / 2;
    const u32 offY = (kThumbnailPx - dstH) / 2;

    for (u32 y = 0; y < dstH; ++y) {
        const u32 sy0 = y * src.height / dstH;
        const u32 sy1 = std::max(sy0 + 1, (y + 1) * src.height / dstH);
        for (u32 x = 0; x < dstW; ++x) {
            const u32 sx0 = x * src.width / dstW;
            const u32 sx1 = std::max(sx0 + 1, (x + 1) * src.width / dstW);

            u32 acc[4] = {0, 0, 0, 0};
            u32 taps = 0;
            for (u32 sy = sy0; sy < sy1; ++sy) {
                const u8* row = &src.pixels[(static_cast<usize>(sy) * src.width + sx0) * 4];
                for (u32 sx = sx0; sx < sx1; ++sx, row += 4, ++taps)
                    for (int c = 0; c < 4; ++c) acc[c] += row[c];
            }
            u8* o = &out[(static_cast<usize>(y + offY) * kThumbnailPx + (x + offX)) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<u8>(acc[c] / taps);
        }
    }
    return out;
}

}   // namespace

// The feature half of the cache. Registered SECOND (see init()), so its prePass runs after the
// preview's own -- the ordering that makes "this frame's render" and "this frame's copy" mean the
// same frame rather than a one-frame lag. Declared here, not in the header, for the reason the
// header's own comment on the `pass_` member gives: a UI-driven cache and a backend-driven render
// feature are two different call disciplines, and conflating them is how a UI call ends up inside a
// command list.
class ThumbnailCache::CopyPass final : public rhi::IRenderFeature {
public:
    explicit CopyPass(ThumbnailCache& owner) : owner_(owner) {}

    const char* name() const override { return "Aver.Editor.ThumbnailCache.Copy"; }

    void prePass(rhi::IRenderContext& ctx) override;

private:
    ThumbnailCache& owner_;
};

void ThumbnailCache::CopyPass::prePass(rhi::IRenderContext& ctx) {
    if (owner_.inFlight_ == 0) return;   // nothing was pointed at the preview this frame

    const u64 assetId = owner_.inFlight_;
    const auto it = owner_.entries_.find(assetId);
    if (it == owner_.entries_.end() || !it->second.tex) {
        // update() always creates the destination texture before setting inFlight_, so reaching
        // this means THAT invariant broke, not that there is genuinely nothing to do here. Logged
        // rather than asserted: a release build should degrade to "this one thumbnail never
        // finishes" with the browser tab still open, not crash it.
        AVER_ERROR("[ThumbnailCache] asset {} is in flight with no destination texture", assetId);
        owner_.inFlight_ = 0;
        return;
    }

    // ============================================================================================
    // BLOCKED. This is the one thing this class exists to do, and it cannot be written against the
    // API this header declares. Read in full before touching this method again.
    //
    // The copy needs the PREVIEW'S OWN colour target as a rhi::TextureHandle -- copyTexture and
    // textureBarrier both take one on each side (RHIResources.hpp). render::preview::ActorPreview
    // does not expose one. Its public surface (ActorPreview.hpp) is: uiTextureId() -> u64,
    // width()/height() -> u32, ready()/viewProj()/frameAll()/setDrawList()/setCamera(). `color_`
    // (the TextureHandle ActorPreview.cpp actually creates and draws into) is a private member with
    // no accessor.
    //
    // uiTextureId() IS NOT A SUBSTITUTE, even though it is also "a handle to the same texture" in
    // some sense. Traced to its D3D12 implementation (D3D12Device::uiTextureId -> ActorPreview.cpp:
    // 224 calls `device.uiTextureId(color_)`): it returns `rhiFactory_->uiDescriptor(t)`, a GPU-
    // visible descriptor-heap identifier ImGui consumes as an opaque texture id -- a DIFFERENT value
    // space from the u32 index into the resource factory's own table that TextureHandle actually is.
    // Passing uiTextureId()'s result to textureBarrier/copyTexture as a TextureHandle is not a type
    // error the compiler catches (both are integers), but it is not the same number: it would either
    // silently no-op against a slot nothing lives in, or -- on a backend where the two numberings
    // happen to collide -- transition and copy whatever resource that index actually names, which is
    // exactly the silent-corruption failure mode copyTexture's own contract (RHIResources.hpp) exists
    // to refuse rather than risk. Reinterpreting it was considered and rejected for that reason.
    //
    // WHAT THIS METHOD WOULD DO, once ActorPreview.hpp gains (for example) a
    // `rhi::TextureHandle colorTexture() const` accessor returning color_:
    //
    //   const rhi::TextureHandle previewColor = owner_.preview_->colorTexture();
    //   // ActorPreview's OWN prePass (registered first) already left previewColor back in
    //   // ShaderResource by the time this runs -- see ActorPreview.cpp:361, the transition at the
    //   // end of its prePass. That is this method's "from" state; it is not assumed, it is read
    //   // off that comment.
    //   ctx.textureBarrier(previewColor, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
    //   ctx.textureBarrier(entry->second.tex, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDest);
    //   ctx.copyTexture(entry->second.tex, previewColor);
    //   ctx.textureBarrier(previewColor, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);
    //   ctx.textureBarrier(entry->second.tex, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource);
    //   it->second.ready = true;
    //   it->second.uiId = owner_.dev_->uiTextureId(it->second.tex);
    //
    // That both leaves previewColor exactly where ActorPreview itself expects to find it next frame
    // (ShaderResource, matching the `everRendered_` branch at ActorPreview.cpp:306) and leaves the
    // destination sampleable by ImGui the instant this method returns -- every barrier paired, none
    // left open, per the header's own CRITICAL DETAILS.
    //
    // WHAT THIS METHOD ACTUALLY DOES, in the absence of that accessor: nothing to the GPU, once,
    // loudly. The in-flight request is dropped rather than retried -- there is nothing about next
    // frame that would make the handle reachable -- and the entry is left NOT ready. That is a safe,
    // documented state: ThumbnailCache::textureId()'s own comment says zero means "draw the typed
    // glyph instead", which is exactly what a browser tile does today already. No texture is left in
    // CopyDest, because none was ever entered.
    // ============================================================================================
    // THE ACCESSOR THIS METHOD WAS WAITING FOR NOW EXISTS. ActorPreview::colorTexture() returns the
    // real rhi::TextureHandle; the comment above worked out precisely why uiTextureId() could never
    // stand in for it, and that reasoning is why this is a copy of PIXELS rather than a copy of a
    // handle -- the preview's target is cleared and redrawn every prePass, so keeping the handle
    // would be keeping a pointer to next frame's picture.
    const rhi::TextureHandle previewColor = owner_.preview_ ? owner_.preview_->colorTexture() : 0;
    const auto entry = owner_.entries_.find(owner_.inFlight_);
    if (previewColor == 0 || entry == owner_.entries_.end() || entry->second.tex == 0) {
        // TRANSIENT, AND NOW RETRIED. previewColor is 0 for exactly as long as ActorPreview is
        // rebuilding its targets, which a viewport resize does mid-browse. Leaving the entry
        // not-ready is still right -- it has no picture in it -- and request() will pick it up again
        // rather than treating "present" as "finished".
        owner_.inFlight_ = 0;
        return;
    }

    // BOTH SIDES INTO THE STATES copyTexture DEMANDS, then both back. copyTexture's contract
    // (RHIResources.hpp) is that the two textures are ALREADY in CopySource / CopyDest -- it issues
    // no barriers of its own -- so an unpaired transition here is not a warning, it is a texture the
    // UI then samples as garbage.
    //
    // previewColor returns to ShaderResource rather than to Common, because that is where
    // ActorPreview itself expects to find it on its next prePass (its everRendered_ branch), and the
    // destination returns to ShaderResource because ImGui samples it the instant this returns.
    ctx.textureBarrier(previewColor, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
    ctx.textureBarrier(entry->second.tex, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDest);
    ctx.copyTexture(entry->second.tex, previewColor);
    ctx.textureBarrier(entry->second.tex, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource);
    ctx.textureBarrier(previewColor, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);

    // READY ONLY NOW, and the ordering matters: textureId() returns 0 until this line runs, which is
    // what stops the browser drawing one frame of an uninitialised texture before the first copy.
    entry->second.uiId = owner_.dev_->uiTextureId(entry->second.tex);
    // A zero descriptor leaves ready false, which -- since request() now retries -- means the next
    // pass at this asset asks the backend again instead of blanking the tile for the whole run.
    entry->second.ready = entry->second.uiId != 0;
    owner_.inFlight_ = 0;
}

ThumbnailCache::~ThumbnailCache() { shutdown(); }

bool ThumbnailCache::init(rhi::IDevice& dev) {
    shutdown();   // idempotent: a second init() rebuilds cleanly rather than leaking the first

    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) {
        AVER_WARN("[ThumbnailCache] backend exposes no resource factory; the content browser falls "
                 "back to typed glyphs only");
        dev_ = nullptr;
        return false;
    }

    // A SECOND, SMALLER preview, deliberately not the asset editors' shared 1024x1024 instance --
    // see this header's own top comment for why sharing it would fight every open editor tab for
    // the draw list and force every cached thumbnail to be 4x the size a tile actually needs.
    preview_ = render::preview::ActorPreview::create(dev, kThumbnailPx, kThumbnailPx);
    if (!preview_) {
        AVER_WARN("[ThumbnailCache] no ActorPreview on this backend; thumbnails are unavailable");
        res_ = nullptr;
        dev_ = nullptr;
        return false;
    }

    pass_ = new CopyPass(*this);

    // REGISTRATION ORDER IS LOAD-BEARING -- see this method's own comment in ThumbnailCache.hpp.
    // IRenderFeature::prePass runs in registration order, and this cache's copy must run AFTER the
    // preview has drawn into its target IN THE SAME FRAME, or it reads last frame's pixels instead
    // of this one's. The preview therefore registers first and the copy second, with nothing else
    // permitted to register between the two calls below.
    dev.addRenderFeature(preview_);
    dev.addRenderFeature(pass_);

    ready_ = true;
    AVER_INFO("[ThumbnailCache] ready: {}x{} preview, up to {} resident", kThumbnailPx, kThumbnailPx,
             kMaxThumbnails);
    return true;
}

void ThumbnailCache::shutdown() {
    if (dev_) {
        // Unregistered in the REVERSE of registration order on general principle (last-in,
        // first-out mirrors every other stack-shaped teardown in this codebase), though the two
        // removals do not actually depend on each other's ordering the way the registrations did:
        // removeRenderFeature just erases-by-value, and neither feature's destructor touches the
        // other.
        if (pass_) dev_->removeRenderFeature(pass_);
        if (preview_) dev_->removeRenderFeature(preview_);
    }
    delete pass_;
    pass_ = nullptr;
    delete preview_;
    preview_ = nullptr;

    if (res_) {
        for (auto& kv : entries_)
            if (kv.second.tex) res_->destroyTexture(kv.second.tex);
    }
    entries_.clear();
    pending_.clear();
    texPending_.clear();
    inFlight_ = 0;
    cappedWarned_ = false;

    res_ = nullptr;
    dev_ = nullptr;
    ready_ = false;
}

void ThumbnailCache::request(u64 assetId, rhi::MeshHandle mesh) {
    if (!ready_) return;
    // RESIDENT MEANS FINISHED, NOT MERELY PRESENT. update() creates the entry before the copy runs,
    // so "in entries_" used to include "attempted once and failed" -- and because nothing ever
    // erased such an entry, that asset could never be requested again for the life of the process.
    // A tile that failed once retries, up to kMaxThumbnailAttempts, reusing the destination texture
    // it already owns rather than churning one per attempt.
    if (const auto it = entries_.find(assetId); it != entries_.end()) {
        // TOUCHED FIRST, AND ON EVERY PATH, because the early returns below are the COMMON case:
        // a finished, visible thumbnail returns on the very next line, and if recency were stamped
        // after that, the entries most worth keeping would be exactly the ones that never recorded
        // being used, and LRU would evict them first.
        it->second.lastSeen = frame_;
        if (it->second.ready) return;                          // genuinely done
        if (it->second.attempts >= kMaxThumbnailAttempts) return;   // given up on, and said so
        if (inFlight_ == assetId) return;                      // this frame's copy is still to run
    }
    for (const auto& p : pending_)
        if (p.first == assetId) return;   // already queued

    if (entries_.size() >= kMaxThumbnails && !evictColdest(assetId)) {
        // Only reachable when every resident entry was touched this same frame -- i.e. more tiles
        // are visible at once than the budget holds. Dropping is then correct: evicting something
        // also on screen would just thrash it back in next frame.
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] {} thumbnails are visible at once, which is the whole "
                     "budget; the rest draw the typed glyph until something scrolls away",
                     kMaxThumbnails);
            cappedWarned_ = true;
        }
        return;
    }
    pending_.emplace_back(assetId, mesh);
}

u64 ThumbnailCache::textureId(u64 assetId) const {
    const auto it = entries_.find(assetId);
    if (it == entries_.end() || !it->second.ready) return 0;
    return it->second.uiId;
}

void ThumbnailCache::requestTexture(const std::string& absPath) {
    if (!ready_) return;
    const u64 assetId = fnv1a64(std::string_view(absPath));

    if (const auto it = entries_.find(assetId); it != entries_.end()) {
        it->second.lastSeen = frame_;   // touched first, on every path -- see request()'s own comment
        if (it->second.ready) {
            // STILL THE SAME FILE ON DISK? A mesh only ever changes by being re-imported, which
            // gives it a fresh assetId along with the fresh bytes; a texture can be overwritten or
            // hand-edited in place while its tab stays open, and mtime is the one cheap-enough
            // (one stat, not a re-decode) signal that catches that without re-reading the file
            // every frame just to find out nothing changed.
            if (frame_ - it->second.mtimeCheckFrame < kMtimeRecheckFrames) return;
            it->second.mtimeCheckFrame = frame_;
            const u64 mtime = fileMtime(absPath);
            if (mtime == 0 || mtime == it->second.srcMtime) return;
            it->second.ready = false;
            it->second.attempts = 0;   // a changed file earns its own fresh run at the attempt cap
        } else {
            if (it->second.attempts >= kMaxThumbnailAttempts) return;   // given up on, and said so
        }
    }
    for (const auto& p : texPending_)
        if (p.first == assetId) return;   // already queued

    if (entries_.size() >= kMaxThumbnails && !evictColdest(assetId)) {
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] {} thumbnails are visible at once, which is the whole "
                     "budget; the rest draw the typed glyph until something scrolls away",
                     kMaxThumbnails);
            cappedWarned_ = true;
        }
        return;
    }
    texPending_.emplace_back(assetId, absPath);
}

u64 ThumbnailCache::textureIdForPath(const std::string& absPath) const {
    return textureId(fnv1a64(std::string_view(absPath)));
}

// Frees the coldest resident thumbnail so a newly visible one can take its slot.
//
// SKIPS ANYTHING TOUCHED THIS FRAME. Those are the tiles currently on screen, and evicting one to
// make room for another on-screen tile would thrash both: each would destroy the other's texture
// every frame and neither would ever finish. Refusing instead is what the caller's "budget is full"
// message reports, and it is the honest answer -- the budget really is too small for that view.
//
// SKIPS THE IN-FLIGHT ASSET AND THE ONE BEING REQUESTED for the same reason a copy must not have its
// destination pulled out from under it mid-frame.
bool ThumbnailCache::evictColdest(u64 protectId) {
    u64 victim = 0;
    u64 oldest = ~0ull;
    for (const auto& kv : entries_) {
        if (kv.first == protectId || kv.first == inFlight_) continue;
        if (kv.second.lastSeen >= frame_) continue;   // on screen right now
        if (kv.second.lastSeen < oldest) { oldest = kv.second.lastSeen; victim = kv.first; }
    }
    if (!victim) return false;
    const auto it = entries_.find(victim);
    // destroyTexture also releases the UI descriptor slot the thumbnail held (the backend frees it
    // in its own destroy path), which is the half that actually mattered: descriptors, not VRAM,
    // were the binding constraint before the pool was resized.
    if (it != entries_.end()) {
        if (it->second.tex) res_->destroyTexture(it->second.tex);
        entries_.erase(it);
    }
    return true;
}

void ThumbnailCache::update() {
    if (!ready_) return;
    ++frame_;

    // Last frame's copy, if any, ran in CopyPass::prePass -- which happens AFTER this method, since
    // the host calls update() before the render features run (see the header). So by the time THIS
    // call happens, whatever was in flight has already had its one chance at a copy this frame or
    // last frame's copy already consumed it; either way it is stale now and must be cleared before a
    // new request can claim the preview.
    inFlight_ = 0;

    // INDEPENDENT OF EACH OTHER: a texture never touches inFlight_/CopyPass, so there is no reason
    // a frame with nothing in pending_ should skip texPending_, or vice versa -- see the header's
    // own comment on why this split exists.
    updateMeshPending();
    updateTexturePending();
}

void ThumbnailCache::updateMeshPending() {
    if (pending_.empty()) return;

    const auto front = pending_.front();
    const u64 assetId = front.first;
    const rhi::MeshHandle mesh = front.second;
    pending_.erase(pending_.begin());

    // Defends the SAME invariant request() enforces, for a case request() cannot see on its own:
    // the browser calls request() for every visible tile every frame, so a single frame can queue
    // many more than kMaxThumbnails distinct assets before entries_ -- which only ever grows HERE,
    // one at a time -- has caught up enough for request()'s own entries_.size() check to start
    // rejecting them. Without this, a big enough burst would walk entries_ straight past the cap
    // this class exists to enforce, one resident per frame, silently.
    if (entries_.size() >= kMaxThumbnails && !evictColdest(assetId)) {
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] the whole {}-thumbnail budget is on screen while draining a "
                     "backlog; the rest is dropped as it is popped", kMaxThumbnails);
            cappedWarned_ = true;
        }
        return;
    }

    std::vector<render::preview::PreviewDraw> draws(1);
    draws[0].mesh = mesh;
    // Centres the mesh on the preview's pivot and sizes frameAll's framing distance from its real
    // extent, rather than leaving PreviewDraw's defaults (identity world, boundsRadius 0.0f) -- at
    // boundsRadius 0 frameAll (ActorPreview.cpp) computes a zero span and falls back to its own
    // clamp-floor distance, which frames every mesh identically regardless of size and is wrong for
    // anything that is not coincidentally that one size. meshBounds gives a LOCAL-space sphere
    // (RHI.hpp's own comment: conservative, computed once at createMesh from the vertex AABB), so
    // recentring the world's translation on -centre is what actually puts the mesh's own middle at
    // the pivot rather than merely at whatever the mesh happened to be authored around.
    if (dev_) {
        f32 centre[3] = {};
        f32 radius = 0.0f;
        if (dev_->meshBounds(mesh, centre, &radius)) {
            draws[0].world[12] = -centre[0];
            draws[0].world[13] = -centre[1];
            draws[0].world[14] = -centre[2];
            draws[0].boundsRadius = radius;
        }
    }
    preview_->setDrawList(std::move(draws));
    preview_->frameAll();

    // NOT NECESSARILY FRESH ANY MORE: request() now re-queues an entry whose copy did not
    // finish, which is the whole point -- it keeps the destination texture it already created.
    Entry& e = entries_[assetId];
    // Stamped here too, not only in request(): this is the path that CREATES the entry, and an entry
    // born with lastSeen 0 would be the coldest thing in the map the instant it existed and could be
    // evicted before its own copy ever ran.
    e.lastSeen = frame_;
    ++e.attempts;
    if (e.attempts == kMaxThumbnailAttempts) {
        AVER_WARN("[ThumbnailCache] asset {} has not produced a thumbnail in {} attempts; it draws "
                 "the typed glyph from here on", assetId, kMaxThumbnailAttempts);
    }
    if (!e.tex) {
        rhi::TextureDesc td;
        td.dim = rhi::TextureDim::Tex2D;
        td.width = kThumbnailPx;
        td.height = kThumbnailPx;
        td.mips = 1;   // copyTexture requires the mip counts to agree, and the preview target has 1
        // MUST match the preview target's own format bit-for-bit (ActorPreview.cpp's createTargets:
        // Format::RGBA8Unorm, deliberately NOT the sRGB variant -- its pixel shader gamma-encodes
        // itself) or copyTexture refuses the pair as a size/format mismatch (RHIResources.hpp).
        td.format = rhi::Format::RGBA8Unorm;
        // ShaderResource is enough for BOTH roles this texture plays. CopySource/CopyDest are
        // RESOURCE STATES (ResourceState), not bind flags -- ResourceBind only gates UAV/RT/DS
        // allocation flags on the backend (D3D12ResourceFactory::createTexture,
        // D3D12_RESOURCE_FLAG_ALLOW_*); a plain ShaderResource-bind resource can still be
        // transitioned into CopyDest/CopySource, which is all a copy destination ever needs beyond
        // being sampleable by ImGui afterwards.
        td.bind = rhi::ResourceBind::ShaderResource;
        // Matches the preview target's own created state (ActorPreview.cpp's createTargets), so a
        // freshly created entry is already in the state CopyPass's first barrier on it must
        // transition FROM -- see textureBarrier's contract: nothing transitions implicitly, and a
        // resource's declared initialState IS that first "from".
        td.initialState = rhi::ResourceState::ShaderResource;
        td.debugName = "ThumbnailCache.Entry";
        e.tex = res_->createTexture(td);
        if (!e.tex) {
            AVER_ERROR("[ThumbnailCache] could not create a destination texture for asset {}", assetId);
            entries_.erase(assetId);
            return;
        }
    }

    inFlight_ = assetId;
}

// Decodes and uploads at most ONE texture per call, the same one-per-frame pace updateMeshPending()
// keeps -- a 4K source is a real decode, not a copy of pixels someone else already rendered, and
// this is what keeps a folder full of them from stalling the frame it was opened on.
void ThumbnailCache::updateTexturePending() {
    if (texPending_.empty()) return;

    const auto front = texPending_.front();
    const u64 assetId = front.first;
    const std::string path = front.second;
    texPending_.erase(texPending_.begin());

    // Same defence as updateMeshPending()'s own copy of this check, against the same race: a burst
    // of requestTexture() calls in one frame can queue past the budget before entries_ has grown
    // enough for requestTexture()'s own size check to start refusing them.
    if (entries_.size() >= kMaxThumbnails && !evictColdest(assetId)) {
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] the whole {}-thumbnail budget is on screen while draining a "
                     "backlog; the rest is dropped as it is popped", kMaxThumbnails);
            cappedWarned_ = true;
        }
        return;
    }

    Entry& e = entries_[assetId];
    // Stamped here too, not only in requestTexture() -- see updateMeshPending()'s identical comment
    // on why the entry-creating path must stamp its own recency rather than trust an earlier call.
    e.lastSeen = frame_;
    ++e.attempts;

    ImageData img;
    std::string why;
    if (!decodeImage(path, img, &why) || !img.valid()) {
        AVER_WARN("[ThumbnailCache] '{}' not decoded ({}); it draws the typed glyph", path, why);
        if (e.attempts == kMaxThumbnailAttempts) {
            AVER_WARN("[ThumbnailCache] '{}' has not decoded in {} attempts; it draws the typed "
                     "glyph from here on", path, kMaxThumbnailAttempts);
        }
        // NOT ERASED: an entry with no tex yet and attempts < the cap is exactly what
        // requestTexture() knows how to retry, the same shape a stalled mesh copy leaves behind.
        return;
    }

    const std::vector<u8> pixels = letterboxThumbnail(img);

    // REPLACED, NOT REUSED: unlike the mesh path's copyTexture, there is no in-place "write new
    // pixels into an existing texture" on this RHI (IResourceFactory only creates-with-initialData
    // or destroys) -- so a re-decode after an mtime change destroys the old GPU copy before making
    // a new one, same as evictColdest already does for a cold entry.
    if (e.tex) { res_->destroyTexture(e.tex); e.tex = 0; e.ready = false; e.uiId = 0; }

    rhi::TextureDesc td;
    td.dim = rhi::TextureDim::Tex2D;
    td.width = kThumbnailPx;
    td.height = kThumbnailPx;
    td.mips = 1;
    td.format = rhi::Format::RGBA8Unorm;   // matches decodeImage's straight (non-sRGB) bytes
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "ThumbnailCache.TextureEntry";
    const void* levels[1] = {pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = kThumbnailPx * 4;
    e.tex = res_->createTexture(td);
    if (!e.tex) {
        AVER_ERROR("[ThumbnailCache] '{}' could not be uploaded", path);
        entries_.erase(assetId);
        return;
    }

    // READY THE SAME FRAME IT WAS DECODED: unlike the mesh path there is no render feature to wait
    // on -- createTexture's initialData is already on the GPU by the time it returns -- so there is
    // no inFlight_-shaped handshake for this to defer through.
    e.uiId = dev_->uiTextureId(e.tex);
    e.ready = e.uiId != 0;
    e.srcMtime = fileMtime(path);
}

rhi::IRenderFeature* ThumbnailCache::copyFeature() { return pass_; }

} // namespace aver::editor
