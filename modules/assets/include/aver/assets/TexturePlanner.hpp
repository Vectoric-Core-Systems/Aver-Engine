#pragma once
// The residency planner for streamed textures: pure CPU bookkeeping with no device, no file and no thread.
// It decides, once per frame, which mip window each registered texture should hold inside a byte budget, and
// hands back the changes worth making this frame. The streamer (a later step) owns the I/O and the RHI calls;
// everything it needs to decide lives here so it can be tested with synthetic footprints.
// Design: docs/plans/wave4/TEXTURE_STREAMING.md 2.5.
#include "aver/assets/TextureStreamMath.hpp"
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::assets {

struct PlannerSettings {
    u32 tailDim = 64;               // a texture is never trimmed below the first mip whose larger extent <= this
    u32 initialDim = 256;           // the window a texture starts with and falls back to when unseen
    i32 biasMips = 0;               // added to every wanted mip (+1 quarters the memory)
    u32 holdFrames = 600;           // unseen textures keep their window this long (about 10 s at 60 fps)
    u32 seenWindow = 8;             // wants are the minimum over this many recent frames (at most 16)
    u32 downgradeFrames = 30;       // a lower target must persist this long before a downgrade (unless over pool)
    u64 uploadBytesPerFrame = 8ull << 20;
    u32 maxSwapsPerFrame = 4;
    u32 retryFrames = 120;          // wait after a failed request
    u64 perTextureOverhead = 64ull << 10;   // accounted on top of the mips
};

enum class PlannerState : u8 { Idle, Loading, Pinned };

struct PlannerTextureDesc {
    u32 fullW = 0, fullH = 0, fullMips = 0;
    u32 firstMip = 0;               // the window it is resident with now
    bool pinned = false;            // never changes; still counted in the pool
};

struct PlannerRequest {
    u32 id = 0;                     // planner id returned by add()
    u32 generation = 0;             // pass back to complete(); a stale one is dropped
    u32 fromFirst = 0;              // current top mip
    u32 toFirst = 0;                // wanted top mip (< fromFirst: upgrade, reads [toFirst, fromFirst); > : trim, no upload)
    u64 uploadBytes = 0;            // bytes to read for an upgrade, 0 for a trim
    bool trim() const { return toFirst > fromFirst; }
};

struct PlannerStats {
    u64 residentBytes = 0;          // sum of resident windows + overhead, all textures
    u64 targetBytes = 0;            // the same for the planned targets
    u64 poolBytes = 0;
    u32 fitBias = 0;                // the global coarsening the fit needed to reach the pool
    bool overPool = false;          // residency is above the pool
    u32 loading = 0;
};

class TexturePlanner {
public:
    explicit TexturePlanner(const PlannerSettings& s = {}) : s_(s) {}
    const PlannerSettings& settings() const { return s_; }
    void setSettings(const PlannerSettings& s) { s_ = s; }

    // Registers a texture and returns its id (ids are reused after remove()).
    u32 add(const PlannerTextureDesc& d, u64 frame);
    void remove(u32 id);
    u32 count() const { return live_; }

    // When false (no feedback pass ran), every unpinned texture wants mip 0 and the fit coarsens them evenly.
    void setFeedbackAvailable(bool on) { feedback_ = on; }
    // `footprint` = the smallest UV-per-pixel any visible pixel needed this frame (see wantedFirstMip).
    void onSeen(u32 id, f32 footprint, u64 frame);

    // Plans the frame and returns the requests worth issuing, most valuable first (trims first when over pool).
    // Marks each returned texture Loading and stamps its generation.
    std::vector<PlannerRequest> plan(u64 frame, u64 poolBytes);

    // A request finished. False when `generation` is stale (the texture was removed, re-requested or failed
    // meanwhile) and the result must be dropped. On true the texture's resident window becomes newFirstMip.
    bool complete(u32 id, u32 generation, u32 newFirstMip, u64 frame);
    // A request failed (file changed, OOM, ring full): the texture retries after retryFrames, or is pinned
    // when `permanent`. A stale generation is ignored.
    void fail(u32 id, u32 generation, bool permanent, u64 frame);
    void pin(u32 id);

    u32 firstMip(u32 id) const;
    u32 targetMip(u32 id) const;
    u32 tailFirst(u32 id) const;
    u32 initialFirst(u32 id) const;
    PlannerState state(u32 id) const;
    u64 residentBytes(u32 id) const;
    const PlannerStats& stats() const { return stats_; }

private:
    struct Rec {
        bool live = false;
        PlannerTextureDesc d;
        u32 allowed = 1, tail = 0, initial = 0;
        PlannerState state = PlannerState::Idle;
        u32 target = 0;
        u32 generation = 0;
        u64 lastSeen = 0;           // frame of the last onSeen, or of add() until the first sighting
        u32 recentCount = 0;        // valid entries in recentWant/recentFrame
        u64 downSince = 0;          // first frame the target was above the resident window, 0 = not
        u64 retryAt = 0;
        u32 recentWant[16] = {};    // wants from the most recent sightings
        u64 recentFrame[16] = {};
        u32 recentPos = 0;
    };
    u32 snap(const Rec& r, u32 m) const { return snapToAllowed(m > r.tail ? r.tail : m, r.allowed, r.d.fullMips); }
    u32 seenWant(const Rec& r, u64 frame) const;
    u64 cost(const Rec& r, u32 first) const { return windowBytes(r.d.fullW, r.d.fullH, first, r.d.fullMips) + s_.perTextureOverhead; }

    PlannerSettings s_;
    bool feedback_ = true;
    std::vector<Rec> recs_;
    std::vector<u32> free_;
    u32 live_ = 0;
    PlannerStats stats_;
};

// ---- pool size under memory pressure ----------------------------------------------------------------
struct PoolSettings {
    u64 poolBytes = 0;              // explicit pool; 0 = derive from the device budget
    f32 poolFraction = 0.25f;       // of the local budget
    u64 reserveBytes = 1ull << 30;  // left for everything that is not a texture
    u64 minAutoBytes = 256ull << 20;
    u64 unsupportedBytes = 1ull << 30;   // pool when the device cannot report VRAM and none is set
    f32 shrinkPerSecond = 0.85f;    // multiplier per second above highWater
    f32 growPerSecond = 1.02f;      // multiplier per second below lowWater
    f32 highWater = 0.92f, lowWater = 0.85f;
    u64 floorExtraBytes = 128ull << 20;   // the pool never drops below the tail sum plus this
};

class PoolController {
public:
    explicit PoolController(const PoolSettings& s = {}) : s_(s) {}
    const PoolSettings& settings() const { return s_; }
    // `localUsage` includes the textures' own resident bytes. dtSeconds since the last call.
    u64 update(bool vramSupported, u64 localBudget, u64 localUsage, u64 residentTextureBytes, u64 tailSumBytes, f64 dtSeconds);
    f64 scale() const { return scale_; }
private:
    PoolSettings s_;
    f64 scale_ = 1.0;
};

// ---- the staging ring ---------------------------------------------------------------------------------
// A byte range allocator over the upload buffer the worker fills. A region is handed out by reserve(), tagged
// with the frame whose command list reads it by submit(), and becomes reusable only once that frame is
// retireDelay frames old (the GPU may still be copying it until then).
class UploadRing {
public:
    static constexpr u64 kNone = ~0ull;
    UploadRing(u64 capacity, u32 offsetAlign = 512, u32 retireDelay = 4)
        : cap_(capacity), align_(offsetAlign ? offsetAlign : 1), delay_(retireDelay) {}
    // Returns the offset of a free region of `bytes`, or kNone when none fits. The region is held until
    // submit()+retire() or cancel().
    u64 reserve(u64 bytes);
    // The region at `offset` is read by the commands of `frame`.
    bool submit(u64 offset, u64 frame);
    // The region was never used (stale result, failure).
    bool cancel(u64 offset);
    // Frees every submitted region whose frame + retireDelay <= currentFrame.
    void retire(u64 currentFrame);
    u64 capacity() const { return cap_; }
    u64 inUse() const;
    u32 regions() const { return static_cast<u32>(blocks_.size()); }
private:
    struct Block { u64 offset, size; bool submitted; u64 frame; };
    u64 cap_;
    u32 align_;
    u32 delay_;
    std::vector<Block> blocks_;   // sorted by offset
};

} // namespace aver::assets
