#pragma once
// Rendered asset thumbnails for the content browser: one small offscreen render per asset, kept.
//
// WHY THIS COULD NOT EXIST UNTIL NOW, because the shape of it is otherwise puzzling. A render target
// is transient by construction -- whatever is drawn into it is gone the moment the next frame reuses
// it -- and until this session the RHI could copy BUFFERS and not TEXTURES. There was literally no
// way to move rendered pixels somewhere that outlived the frame. IRenderContext::copyTexture is that
// missing primitive, and this class is its first consumer.
//
// ONE ASSET PER FRAME, and that is the whole scheduling design. render::preview::ActorPreview has
// exactly one colour target and one draw list, so it can render one thing per frame and no more.
// Rendering N thumbnails therefore takes N frames -- which is fine, because a thumbnail is needed
// once and kept forever, and a browser that fills in over half a second reads as loading rather than
// as broken. The alternative, N live ActorPreview instances, is far worse than it sounds:
// IRenderFeature has no enable/disable gate, so every registered instance's prePass runs
// unconditionally EVERY frame forever, whether or not the browser is even open.
//
// A SECOND, SMALLER ActorPreview, not the one the asset editors share. That shared instance renders
// at 1024x1024 and is driven by whichever editor tab is active; borrowing it would fight those tabs
// for the draw list every frame, and a whole-resource copy would force every cached thumbnail to be
// 1024x1024 -- 4 MB each, which is not a cache. This one is small and owned.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/render/preview/ActorPreview.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// The edge of a cached thumbnail, in pixels. 512 rather than 256 because the browser's tile-size
// slider reaches 168 dp, which is over 500 physical pixels at the 300% displays this is developed
// on -- a 256 thumbnail is visibly soft there. 512x512 RGBA8 is 1 MB per asset, which is what
// kMaxThumbnails is sized against.
inline constexpr u32 kThumbnailPx = 512;

// How many thumbnails may be resident at once. A hard cap rather than an LRU eviction, for this
// slice: eviction needs a use-ordering the browser does not currently report, and a cap that LOGS
// when it is reached is honest about stopping, where a silent LRU that thrashes is not.
inline constexpr u32 kMaxThumbnails = 64;

class ThumbnailCache {
public:
    ~ThumbnailCache();

    // Creates the dedicated preview and its own render feature registration.
    //
    // REGISTRATION ORDER IS LOAD-BEARING AND THIS METHOD OWNS IT. Features run their prePass in
    // registration order, and the copy this class performs must happen AFTER the preview has drawn
    // into its target in the same frame. init() therefore registers the preview FIRST and this
    // cache SECOND, and nothing else may register between them. Getting it backwards copies the
    // PREVIOUS frame's pixels, which looks like a one-frame lag and is actually a wrong thumbnail
    // whenever the requested asset changes.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // Asks for `assetId`'s thumbnail, rendering `mesh` if it is not cached yet. Cheap and idempotent
    // -- the browser calls it for every visible tile, every frame.
    void request(u64 assetId, rhi::MeshHandle mesh);

    // The ImGui texture id for a finished thumbnail, or 0 when it is not ready. Zero is the normal
    // answer for the first few frames after a request and means "draw the typed glyph instead", not
    // "this failed".
    u64 textureId(u64 assetId) const;

    // Drives the queue: picks at most ONE pending request and points the preview at it. Called once
    // per frame by the host, BEFORE the render features run.
    void update();

    // The feature that performs the copy. Registered by init(); exposed so the host can unregister
    // it at shutdown, matching how every other feature in the editor is torn down.
    rhi::IRenderFeature* copyFeature();

private:
    struct Entry {
        rhi::TextureHandle tex = 0;
        u64  uiId = 0;
        bool ready = false;   // false until the copy has actually run for it
    };

    // The feature half, separate from the cache so the cache itself is not an IRenderFeature -- it
    // is called from UI code every frame and a render feature is called from the backend, and
    // conflating the two is how a UI call ends up inside a command list.
    class CopyPass;

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    render::preview::ActorPreview* preview_ = nullptr;
    CopyPass*              pass_ = nullptr;
    bool ready_ = false;

    std::unordered_map<u64, Entry> entries_;
    std::vector<std::pair<u64, rhi::MeshHandle>> pending_;
    // The asset the preview was pointed at this frame, and therefore the one the copy must write
    // into. Zero when the preview was not driven this frame and no copy should happen at all.
    u64 inFlight_ = 0;
    bool cappedWarned_ = false;
};

} // namespace aver::editor
