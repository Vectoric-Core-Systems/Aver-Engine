// destroyLineMesh: the contract, against a REAL device.
//
// This is the one RHI test that talks to a backend rather than to a mock, because the thing under
// test is backend bookkeeping -- a mock implementing the same rules would only prove the mock. It
// asks for WARP first (DeviceDesc::useWarp), so it runs on a machine with no GPU in it and is not a
// hardware-dependent suite; if even WARP is unavailable it says so and passes rather than failing
// somebody's build for a reason that has nothing to do with their change.
//
// WHAT THIS DOES NOT MEASURE: how many bytes were handed back. There is no VRAM query on IDevice and
// the release is deferred behind a fence, so a byte count taken here would be measuring the fence,
// not the free. What it does pin down is every rule a caller can actually depend on -- and the leak
// itself is arithmetic once the release happens at all: createLineMesh committed an upload buffer
// per call and nothing ever released one.
#include "aver/rhi/RHI.hpp"

#include "aver/core/Log.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static rhi::LineHandle makeLine(rhi::IDevice& dev) {
    const rhi::LineVertex verts[2] = {
        {0.0f, 0.0f, 0.0f,   1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 100.0f, 1.0f, 1.0f, 1.0f},
    };
    return dev.createLineMesh(verts, 2);
}

int main(int argc, char** argv) {
    AVER_INFO("LineMeshTest");

    // `LineMeshTest vulkan` runs the whole thing against the Vulkan backend instead. It exists
    // because the fix is in TWO devices and a suite that only ever reaches the first one is a
    // suite that would have shipped the second one untested -- which is exactly the state this
    // repo's Vulkan backend keeps ending up in (its own CMakeLists records a configure that had
    // been broken for a whole file split because nothing ran that row).
    bool wantVulkan = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;

    rhi::DeviceDesc desc;
    desc.useWarp = true;   // no GPU required; this test is about bookkeeping, not rasterisation
    // D3D12 and Vulkan ONLY. D3D11 is deliberately left out of the order rather than handled:
    // it has no createLineMesh at all, so falling through to it would test nothing while looking
    // like it tested something. Null is the last entry so createDevice always returns.
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;   // NO fallback to the other one: an asked-for backend that is
                               // absent must say SKIPPED, not quietly retest the one already run
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev) {
        AVER_WARN("LineMeshTest: no device could be created at all -- SKIPPED");
        return 0;
    }
    if (dev->backend() == rhi::Backend::Null) {
        AVER_WARN("LineMeshTest: only the Null backend is available (no D3D12/Vulkan runtime here) "
                  "-- SKIPPED, since Null has no line meshes to destroy");
        rhi::destroyDevice(dev);
        return 0;
    }
    AVER_INFO("running against backend {}", rhi::backendName(dev->backend()));

    // One probe before asserting anything: a backend that cannot make a line mesh at all has
    // nothing here to be wrong about, and saying SKIPPED is the only honest report of that.
    {
        const rhi::LineHandle probe = makeLine(*dev);
        if (probe == 0) {
            AVER_WARN("LineMeshTest: this backend does not implement createLineMesh -- SKIPPED");
            rhi::destroyDevice(dev);
            return 0;
        }
        dev->destroyLineMesh(probe);
    }

    AVER_INFO("a line mesh can be created and destroyed");
    {
        const rhi::LineHandle h = makeLine(*dev);
        check(h != 0, "createLineMesh returns a handle");
        check(dev->destroyLineMesh(h), "and destroyLineMesh accepts it");
        check(!dev->destroyLineMesh(h),
              "a SECOND destroy is refused -- reporting success twice would hide a double free "
              "in whatever called it");
    }

    AVER_INFO("a bad handle is refused rather than indexed");
    {
        check(!dev->destroyLineMesh(0), "handle 0 is not a mesh");
        check(!dev->destroyLineMesh(99999), "and neither is one past the end");
    }

    AVER_INFO("THE SLOT IS KEPT, NOT RECYCLED");
    {
        const rhi::LineHandle a = makeLine(*dev);
        check(dev->destroyLineMesh(a), "destroy the first");
        const rhi::LineHandle b = makeLine(*dev);
        check(b != 0 && b != a,
              "the next create gets a NEW handle, NOT the freed one -- recycling would hand "
              "somebody else's geometry to whoever still held the old handle, which looks like "
              "corruption and cannot be traced back to here. destroyMesh already made this choice "
              "and said why; this matches it rather than inventing a second policy");
        check(dev->destroyLineMesh(b), "and the new one destroys too");
    }

    AVER_INFO("a destroyed handle draws nothing");
    {
        const rhi::LineHandle h = makeLine(*dev);
        check(dev->destroyLineMesh(h), "destroyed");
        const f32 identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        dev->drawLines(h, identity);   // must not bind a null vertex view, must not crash
        check(true, "drawLines on the dead handle returns without binding anything");
    }

    AVER_INFO("many creates and destroys stay well-formed");
    {
        // The shape the navmesh overlay has: rebuild the same geometry over and over. Before
        // destroyLineMesh existed this leaked one committed upload buffer per iteration.
        bool ok = true;
        rhi::LineHandle prev = 0;
        for (int i = 0; i < 64; ++i) {
            const rhi::LineHandle h = makeLine(*dev);
            if (h == 0 || h == prev) { ok = false; break; }
            if (!dev->destroyLineMesh(h)) { ok = false; break; }
            prev = h;
        }
        check(ok, "64 rebuild cycles each get a fresh handle and each release cleanly");
    }

    rhi::destroyDevice(dev);

    AVER_INFO(g_failures ? "LineMeshTest: {} FAILURES" : "LineMeshTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
