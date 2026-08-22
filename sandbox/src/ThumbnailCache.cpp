// ThumbnailCache implementation: the dedicated preview, the one-asset-per-frame queue, and the
// CopyPass that is meant to turn a transient render into a texture that survives past this frame.
//
// See ThumbnailCache.hpp for the whole design rationale (registration order, one asset per frame,
// a second dedicated ActorPreview). This file is the mechanism the header promises -- and it is
// NOT COMPLETE. CopyPass::prePass below cannot perform the copy it is named for: see its own
// comment for why, and the top-level report accompanying this file for the one-line header change
// that would unblock it. Everything else -- registration order, the queue, the cap, the destination
// texture -- is implemented and correct on its own.
#include "ThumbnailCache.hpp"

#include "aver/core/Log.hpp"

namespace aver::editor {

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
    inFlight_ = 0;
    cappedWarned_ = false;

    res_ = nullptr;
    dev_ = nullptr;
    ready_ = false;
}

void ThumbnailCache::request(u64 assetId, rhi::MeshHandle mesh) {
    if (!ready_) return;
    if (entries_.find(assetId) != entries_.end()) return;   // resident, or already in flight
    for (const auto& p : pending_)
        if (p.first == assetId) return;   // already queued

    if (entries_.size() >= kMaxThumbnails) {
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] at the {}-thumbnail cap; further requests are dropped for "
                     "the rest of this run rather than evicting an older one -- see the header's "
                     "own comment on kMaxThumbnails for why there is no LRU here yet", kMaxThumbnails);
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

void ThumbnailCache::update() {
    if (!ready_) return;

    // Last frame's copy, if any, ran in CopyPass::prePass -- which happens AFTER this method, since
    // the host calls update() before the render features run (see the header). So by the time THIS
    // call happens, whatever was in flight has already had its one chance at a copy this frame or
    // last frame's copy already consumed it; either way it is stale now and must be cleared before a
    // new request can claim the preview.
    inFlight_ = 0;

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
    if (entries_.size() >= kMaxThumbnails) {
        if (!cappedWarned_) {
            AVER_WARN("[ThumbnailCache] at the {}-thumbnail cap while draining a request backlog; "
                     "the rest of the backlog is dropped as it is popped", kMaxThumbnails);
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

    Entry& e = entries_[assetId];   // fresh: request() already refused a duplicate assetId
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

rhi::IRenderFeature* ThumbnailCache::copyFeature() { return pass_; }

} // namespace aver::editor
