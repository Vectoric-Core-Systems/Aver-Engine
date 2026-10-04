// LandscapeRenderer's mesh cache: does it actually RECLAIM residency, or only refuse to grow?
// Exit code = failure count.
//
// WHY THIS IS HEADLESS AND NEEDS NO GPU. rhi::IDevice's mesh entry points are ordinary virtuals with
// harmless defaults, so a test can implement the three this renderer uses and count the calls. That
// is the same trick tests/render.ui and tests/render.actorpreview already use for IResourceFactory,
// and it turns "the cache leaks GPU memory" -- previously only observable as a number in a GPU
// profiler -- into an arithmetic property: destroyed + live == created, always.
//
// WHAT IT IS GUARDING. Until IDevice::destroyMesh existed, this cache could only fill up and stop.
// Its own warning said so: "There is no destroyMesh, so residency cannot be reclaimed -- raise
// maxResidentNodes or use smaller sections." Everything past the cap drew a coarser ancestor for the
// rest of the session, and forgetAll() dropped the handles without freeing one byte.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/landscape/LandscapeRenderer.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

using namespace aver;
using namespace aver::landscape;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Counts what the renderer asks of a device, and nothing else. Handles are minted sequentially from
// 1 and never reused, which mirrors the real backend's rule -- a destroyed handle must stay dead
// rather than come back as something else.
struct CountingDevice final : public rhi::IDevice {
    // The six pure virtuals IDevice declares. None is exercised here; they exist so this class is
    // concrete, exactly as tests/landscape's own Recorder and tests/render.ui's mock do.
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "counting device"; }
    rhi::IResourceFactory* resources() override { return nullptr; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}

    u32 nextHandle = 1;
    u32 created = 0, destroyed = 0, draws = 0;
    std::unordered_set<u32> live;
    std::vector<u32> destroyedOrder;

    rhi::MeshHandle createMesh(const rhi::MeshVertex*, u32 vcount, const u32*, u32 icount) override {
        if (vcount == 0 || icount == 0) return 0;
        const rhi::MeshHandle h = nextHandle++;
        live.insert(h);
        ++created;
        return h;
    }
    bool destroyMesh(rhi::MeshHandle m) override {
        if (!live.erase(m)) return false;     // double-free or never-created would fail here
        destroyedOrder.push_back(m);
        ++destroyed;
        return true;
    }
    void drawMesh(rhi::MeshHandle m, const f32*, const f32*, f32, f32) override {
        // A draw of a mesh this device has already freed is the bug the whole slice is about.
        if (!live.count(m)) { AVER_ERROR("   FAIL  drawMesh on a destroyed handle {}", m); ++g_failures; }
        ++draws;
    }
};

// An n x n section with enough relief that the LOD metric actually descends.
static fmt::OcLandData makeTerrain(u32 n, f32 spacing = 100.0f) {
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = spacing;
    d.heights.resize(static_cast<usize>(n) * n);
    for (u32 iy = 0; iy < n; ++iy)
        for (u32 ix = 0; ix < n; ++ix) {
            const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
            d.heights[static_cast<usize>(iy) * n + ix] =
                400.0f * std::sin(fx * 0.11f) + 250.0f * std::sin(fy * 0.27f)
                + 60.0f * std::sin((fx + fy) * 0.9f);
        }
    return d;
}

int main() {
    // 16*64 + 1 -> 5 levels, 341 nodes, over a 1.024 km section. Big enough that the union of what
    // a moving camera wants is far larger than any sane cache, which is the situation eviction is
    // for. A 21-node tree cannot demonstrate it: everything fits, or nothing does.
    const u32 N = 1025;
    const fmt::OcLandData terrain = makeTerrain(N);

    LandscapeTree tree;
    std::string why;
    if (!tree.build(terrain, 64, &why)) {
        AVER_ERROR("the test tree failed to build: {}", why);
        return 1;
    }

    const f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // THE CACHE MUST FIT ONE FRAME AND NOT THE SWEEP. That is the whole shape of the problem, and
    // getting it wrong is instructive: at a cap of 4 against a 14-node selection, every resident is
    // wanted on the current frame, evictOne correctly refuses to take any of them, and the renderer
    // falls back to coarser ancestors exactly as it should. Eviction is not a way to serve a working
    // set larger than the cache -- it is a way to stop yesterday's working set from holding it.
    //
    // So maxDraws bounds one frame to 24 nodes and the cache holds 32.
    const u32 kCap = 32;
    const u32 kPerFrame = 24;

    {
        CountingDevice dev;
        LandscapeRenderer r(kCap);

        // Sweep the camera across the section so the selected set changes frame to frame -- a
        // stationary camera would select the same nodes forever and never exercise eviction.
        for (int frame = 0; frame < 24; ++frame) {
            SelectParams p;
            // Right across the section over the sweep, so each frame wants a different neighbourhood.
            p.cameraCm[0] = static_cast<f32>(frame) * 4000.0f;
            p.cameraCm[1] = static_cast<f32>(frame) * 3500.0f;
            p.cameraCm[2] = 1500.0f;
            p.screenErrorPx = 1.0f;          // demanding, so it descends to the leaves
            p.maxDraws = kPerFrame;
            SelectResult sel;
            tree.select(p, sel);
            r.draw(dev, terrain, tree, sel, world);

            check(static_cast<u32>(dev.live.size()) <= kCap,
                  "frame " + std::to_string(frame) + ": live meshes never exceed the cache cap");
            if (dev.live.size() > kCap) break;
        }

        check(dev.created > kCap,
              "the sweep really did need more distinct nodes than the cache holds");
        AVER_INFO("   note  {} created, {} destroyed, {} still resident, cap {}",
                  dev.created, dev.destroyed, dev.live.size(), kCap);
        check(dev.destroyed > 0,
              "...and residency was RECLAIMED rather than merely refused -- this is the whole slice");
        check(dev.created - dev.destroyed == static_cast<u32>(dev.live.size()),
              "created - destroyed == live: no handle leaked and none was freed twice");
        check(dev.draws > 0, "and it actually drew something throughout");

        // The root is the ancestor every other node falls back to, so evicting it turns a
        // substitution into a hole. It is created first, so its handle is 1.
        bool rootEvicted = false;
        for (const u32 h : dev.destroyedOrder) if (h == 1) rootEvicted = true;
        check(!rootEvicted, "the root mesh is never evicted -- it is the universal fallback");

        // forgetAll must FREE, not merely forget. This is the call whose own comment used to read
        // "Call when the device goes; it cannot free them."
        const u32 liveBefore = static_cast<u32>(dev.live.size());
        check(liveBefore > 0, "there are resident meshes to release");
        r.forgetAll(dev);
        check(dev.live.empty(), "forgetAll released every resident mesh");
        check(dev.created == dev.destroyed, "created == destroyed once the cache is emptied: nothing leaked");
    }

    // A cache big enough for the whole tree never evicts -- eviction must be a response to pressure,
    // not something that happens on its own.
    {
        CountingDevice dev;
        LandscapeRenderer r(4096);   // comfortably more than the tree's 341 nodes
        for (int frame = 0; frame < 8; ++frame) {
            SelectParams p;
            p.cameraCm[0] = static_cast<f32>(frame) * 4000.0f;
            p.cameraCm[2] = 1500.0f;
            p.screenErrorPx = 1.0f;
            p.maxDraws = kPerFrame;
            SelectResult sel;
            tree.select(p, sel);
            r.draw(dev, terrain, tree, sel, world);
        }
        check(dev.destroyed == 0, "a cache with room to spare evicts nothing");
        check(r.stats().evicted == 0, "...and reports no evictions");
        r.forgetAll(dev);
        check(dev.live.empty(), "forgetAll still frees everything in the roomy case");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
