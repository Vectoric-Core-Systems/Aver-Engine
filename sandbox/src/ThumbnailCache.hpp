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
//
// TEXTURE ASSETS DO NOT GO THROUGH ANY OF THE ABOVE. There is nothing to render -- the pixels
// already exist on disk -- so requestTexture() decodes and downscales them on the CPU and uploads
// the result directly via createTexture's initialData, with no ActorPreview, no CopyPass, and no
// inFlight_ handshake between the two. It shares everything else with the mesh path on purpose:
// the same entries_ map, the same kMaxThumbnails budget and LRU eviction, and the same release on
// shutdown/project-switch/device-loss, so a browser mixing meshes and textures is one budget, not
// two caches that don't know about each other.
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

// How many thumbnails may be resident at once, as a VRAM budget: 64 MB at kThumbnailPx.
//
// NOW AN LRU BOUND RATHER THAN A WALL. This was a hard cap on the stated grounds that "eviction
// needs a use-ordering the browser does not currently report" -- but it does report one, and always
// did: request() is called for every VISIBLE tile every frame, so the frame an asset was last
// requested IS its recency, with no new plumbing. Reaching the cap now evicts the coldest entry
// instead of refusing every asset thereafter for the rest of the process.
//
// WHY THE WALL HAD TO GO: entries_ is never cleared between projects (init/shutdown are tied to
// process start and exit), so the cap was cumulative across every project opened in one session. A
// user browsing a few hundred meshes -- an imported Sponza is 115 on its own -- passed it and then
// silently got the typed glyph for everything new, permanently, with one log line as the only clue.
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

    // Asks for a thumbnail of the image at `absPath`, decoding and downscaling it if it is not
    // cached yet. Cheap and idempotent like request() -- call it for every visible tile, every
    // frame -- EXCEPT that decoding is not free the way touching an already-uploaded mesh is:
    // update() below draws from this request's queue at the same one-per-frame pace request()'s
    // queue drains at, so a folder full of visible 4K textures fills in over multiple frames rather
    // than stalling the one it was opened on.
    //
    // KEYED BY THE ABSOLUTE PATH, not a caller-supplied id -- textures have no id space of their
    // own the way meshes share one with sceneMeshes_ (see the .ocmesh call site), so there is
    // nothing to gain from making the caller compute and remember one. Internally this hashes the
    // path into the same u64 key space request() uses; textureIdForPath() hashes it again to look
    // the entry up, so the two never need to agree on anything but the path string itself.
    void requestTexture(const std::string& absPath);

    // The ImGui texture id for a finished texture thumbnail, or 0 -- same contract as textureId().
    u64 textureIdForPath(const std::string& absPath) const;

    // Drives both queues: picks at most ONE pending mesh request and points the preview at it, and
    // separately decodes at most ONE pending texture request. Called once per frame by the host,
    // BEFORE the render features run.
    void update();

    // The feature that performs the copy. Registered by init(); exposed so the host can unregister
    // it at shutdown, matching how every other feature in the editor is torn down.
    rhi::IRenderFeature* copyFeature();

private:
    struct Entry {
        rhi::TextureHandle tex = 0;
        u64  uiId = 0;
        bool ready = false;   // false until the copy has actually run for it
        // HOW MANY TIMES THE COPY HAS BEEN ATTEMPTED AND NOT FINISHED.
        //
        // An entry used to be created by update() BEFORE the copy ran, and request() refuses any
        // asset already in entries_ -- so a single failed attempt left an entry that was never ready
        // and could never be asked for again. The copy can fail for reasons that are transient by
        // nature: the preview's colour target is momentarily 0 while createTargets rebuilds it after
        // a resize (ActorPreview.cpp sets color_ = 0 before recreating), and uiTextureId can return 0
        // for a texture the backend has not yet given a descriptor. Both pass. The old code turned
        // either into a permanently blank tile, for whichever assets happened to be in the queue at
        // the time -- alphabetically first, in a browser that queues in listing order.
        u32  attempts = 0;
        // THE FRAME THIS ASSET WAS LAST ASKED FOR, which is what makes eviction possible without the
        // browser telling us anything new: request() runs for every visible tile every frame, so a
        // tile scrolled out of view simply stops updating this and drifts to the back of the queue.
        u64  lastSeen = 0;
        // TEXTURE ENTRIES ONLY: the source file's last-write-time, as an opaque comparable value
        // (std::filesystem::file_time_type::rep, not a wall-clock unit) -- 0 both means "never
        // stamped" and "the stat failed", which is safe to conflate because either way there is
        // nothing trustworthy to compare against and requestTexture() re-decodes rather than guess.
        // A mesh entry never touches this; ready() alone is what a mesh checks.
        u64  srcMtime = 0;
        // TEXTURE ENTRIES ONLY: the frame srcMtime was last compared against the file, so a visible
        // tile stats its file about once a second rather than every frame (kMtimeRecheckFrames).
        u64  mtimeCheckFrame = 0;
    };

    // How often a READY texture thumbnail re-checks its file's write time. A gallery full of textures
    // would otherwise stat every visible file every frame -- thousands of filesystem calls a second on
    // a large folder, to notice an edit that a second's delay loses nothing on.
    static constexpr u64 kMtimeRecheckFrames = 60;

    // ATTEMPTS BEFORE GIVING UP ON ONE ASSET. Bounded rather than unbounded so a genuinely broken
    // asset costs a handful of frames and one log line instead of re-queueing forever; generous
    // enough that a device rebuild in the middle of a browse recovers on its own.
    static constexpr u32 kMaxThumbnailAttempts = 8;

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
    // Texture thumbnails awaiting a decode, keyed the same way pending_ is but carrying a path
    // instead of a mesh handle. Separate from pending_/inFlight_ because a texture never touches
    // the preview or CopyPass -- it is decoded and uploaded to completion inside update() itself,
    // so there is no cross-frame handoff for it to wait on.
    std::vector<std::pair<u64, std::string>> texPending_;
    // Ticked once per update(). Only ever compared against itself, so wrap is not a concern at one
    // increment per frame.
    u64 frame_ = 0;
    bool cappedWarned_ = false;

    // Frees the coldest resident entry to make room for one more. Returns false when there is
    // nothing evictable -- every resident entry was touched this frame, which means the visible tile
    // count genuinely exceeds the budget and dropping the request is the only honest answer.
    bool evictColdest(u64 protectId);

    // The two halves of update(), split out so neither queue's early-out ("nothing pending") can
    // skip the other -- a frame with a texture to decode but no mesh to render must still decode it,
    // and the old single-body shape returned out of the whole function the moment pending_ was empty.
    void updateMeshPending();
    void updateTexturePending();
};

} // namespace aver::editor
