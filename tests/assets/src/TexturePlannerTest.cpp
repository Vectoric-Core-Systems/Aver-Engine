// TexturePlannerTest -- the pure texture residency planner, the pool controller and the staging ring, driven by
// synthetic footprints (docs/plans/wave4/TEXTURE_STREAMING.md 2.5). No device, no files, no threads.
//
// All byte figures are literals for a 4096x4096 BC7 chain with 13 mips, windowBytes by top mip:
//   0: 22369648   1: 5592432   2: 1398128   3: 349552   4: 87408   5: 21872   6: 5488   7: 1392   8: 368
// Per-texture overhead is 0 in every test so the pools below can be exact. The default tail is mip 6 and the
// initial window mip 4.
#include "aver/assets/TexturePlanner.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::assets;

static int g_checks = 0;
static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

constexpr u64 F0 = 22369648, F1 = 5592432, F4 = 87408, F6 = 5488;
// The footprint that asks for exactly `mip` on a 4096 texture.
static f32 fpFor(u32 mip) { return static_cast<f32>(1u << mip) / 4096.0f; }

static PlannerSettings base() {
    PlannerSettings s;
    s.perTextureOverhead = 0;
    s.maxSwapsPerFrame = 100;
    s.uploadBytesPerFrame = 1ull << 40;
    return s;
}
static PlannerTextureDesc tex4096(u32 firstMip = 4, bool pinned = false) {
    PlannerTextureDesc d;
    d.fullW = d.fullH = 4096; d.fullMips = 13; d.firstMip = firstMip; d.pinned = pinned;
    return d;
}
static const PlannerRequest* find(const std::vector<PlannerRequest>& v, u32 id) {
    for (const PlannerRequest& r : v) if (r.id == id) return &r;
    return nullptr;
}

int main() {
    // ---- registration -------------------------------------------------------------------------------
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(), 1);
        check(p.tailFirst(a) == 6 && p.initialFirst(a) == 4, "a 4096 chain has tail mip 6 and initial mip 4");
        check(p.firstMip(a) == 4 && p.targetMip(a) == 4 && p.state(a) == PlannerState::Idle, "it starts at its resident window");
        PlannerTextureDesc odd; odd.fullW = odd.fullH = 1500; odd.fullMips = 11; odd.firstMip = 0;
        const u32 b = p.add(odd, 1);
        check(p.tailFirst(b) == 0 && p.initialFirst(b) == 0, "a 1500 chain can only ever hold mip 0");
        check(p.add(tex4096(), 1) == 2 && p.count() == 3, "ids are dense");
        p.remove(a);
        check(p.count() == 2 && p.add(tex4096(), 1) == a, "a removed id is reused");
    }

    // ---- the fit chooses the smallest bias that reaches the pool ---------------------------------------
    {
        TexturePlanner p(base());
        std::vector<u32> ids;
        for (int i = 0; i < 4; ++i) ids.push_back(p.add(tex4096(), 1));
        for (u32 id : ids) p.onSeen(id, fpFor(0), 1);
        auto out = p.plan(1, 4 * F0);
        check(p.stats().fitBias == 0 && out.size() == 4, "a pool of four full chains needs no bias: all four go to mip 0");
        bool all0 = true;
        for (u32 id : ids) all0 = all0 && p.targetMip(id) == 0;
        check(all0, "...every target is mip 0");
    }
    {
        TexturePlanner p(base());
        std::vector<u32> ids;
        for (int i = 0; i < 4; ++i) ids.push_back(p.add(tex4096(), 1));
        for (u32 id : ids) p.onSeen(id, fpFor(0), 1);
        p.plan(1, 4 * F0 - 1);
        bool all1 = true;
        for (u32 id : ids) all1 = all1 && p.targetMip(id) == 1;
        check(p.stats().fitBias == 1 && all1, "one byte short of that coarsens everything by exactly one mip");
        check(p.stats().targetBytes == 4 * F1, "...and the planned bytes are four mip-1 windows");
    }
    for (int pass = 0; pass < 2; ++pass) {
        TexturePlanner p(base());
        const u32 big = p.add(tex4096(4), 1);
        std::vector<u32> small;
        for (int i = 0; i < 3; ++i) small.push_back(p.add(tex4096(6), 1));
        p.onSeen(big, fpFor(0), 1);
        for (u32 id : small) p.onSeen(id, fpFor(6), 1);
        if (pass == 0) {
            p.plan(1, F0 + 3 * F6);
            check(p.stats().fitBias == 0 && p.targetMip(big) == 0 && p.targetMip(small[0]) == 6, "a mixed scene that fits keeps each texture's own want");
        } else {
            p.plan(1, F0 + 3 * F6 - 1);
            check(p.stats().fitBias == 1 && p.targetMip(big) == 1 && p.targetMip(small[0]) == 6,
                  "one byte short: the bias lands on the texture that can give, the tail-limited ones stay at the tail");
        }
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(), 1);
        p.onSeen(a, fpFor(0), 1);
        p.plan(1, 10);
        check(p.targetMip(a) == 6 && p.stats().fitBias >= 6, "a pool that cannot hold even the tails still never goes below the tail");
    }
    {
        PlannerSettings s = base();
        s.biasMips = 1;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(), 1);
        p.onSeen(a, fpFor(2), 1);
        p.plan(1, 1ull << 40);
        check(p.targetMip(a) == 3, "the global mip bias is added to every want");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(), 1);
        p.onSeen(a, fpFor(5), 1);
        p.onSeen(a, fpFor(2), 1);
        p.onSeen(a, fpFor(4), 1);
        p.plan(1, 1ull << 40);
        check(p.targetMip(a) == 2, "several sightings in one frame keep the finest");
    }
    {
        PlannerSettings s = base();
        s.downgradeFrames = 0;   // so the want window is the only thing between a sighting and a trim
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(2), 1);
        bool trimmedEarly = false;
        u64 trimFrame = 0;
        for (u64 f = 1; f <= 12 && !trimFrame; ++f) {
            p.onSeen(a, fpFor(f == 1 ? 2 : 7), f);
            const auto out = p.plan(f, 1ull << 40);
            if (!out.empty()) { trimFrame = f; trimmedEarly = f < 9; }
        }
        check(trimFrame == 9 && !trimmedEarly, "a one-frame sighting of fine detail holds the want for 8 frames, then the trim to mip 7 goes through");
    }

    // ---- no feedback source ---------------------------------------------------------------------------
    for (int pass = 0; pass < 2; ++pass) {
        TexturePlanner p(base());
        p.setFeedbackAvailable(false);
        std::vector<u32> ids;
        for (int i = 0; i < 3; ++i) ids.push_back(p.add(tex4096(), 1));
        p.plan(1, pass == 0 ? 3 * F0 : 3 * F1);
        bool all = true;
        for (u32 id : ids) all = all && p.targetMip(id) == static_cast<u32>(pass);
        check(all && p.stats().fitBias == static_cast<u32>(pass),
              pass == 0 ? "without feedback every texture wants mip 0" : "...and a tighter pool coarsens them evenly");
    }

    // ---- unseen textures trim least-recently-seen first -------------------------------------------------
    {
        PlannerSettings s = base();
        s.holdFrames = 10;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(0), 1);
        const u32 b = p.add(tex4096(0), 1);
        p.onSeen(a, fpFor(0), 50);
        p.onSeen(b, fpFor(0), 60);
        auto out = p.plan(100, F0 + F4);
        check(p.targetMip(b) == 0 && p.targetMip(a) == 4, "with room for one full chain the more recently seen keeps it and the other falls to its initial window");
        const PlannerRequest* ra = find(out, a);
        check(out.size() == 1 && ra && ra->trim() && ra->toFirst == 4 && ra->uploadBytes == 0,
              "...as a trim with no upload, issued at once because residency is above the pool");
    }
    {
        PlannerSettings s = base();
        s.holdFrames = 10;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(0), 1);
        const u32 b = p.add(tex4096(0), 1);
        p.onSeen(a, fpFor(0), 50);
        p.onSeen(b, fpFor(0), 60);
        auto out = p.plan(100, 2 * F0);
        check(out.empty() && p.targetMip(a) == 0 && p.targetMip(b) == 0, "unseen textures that fit are left alone");
    }
    {
        PlannerSettings s = base();
        s.holdFrames = 100;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(0), 1);
        p.plan(50, F4);
        check(p.targetMip(a) == 0, "an unseen texture is held for holdFrames before it is trimmed");
        p.plan(101, F4);
        check(p.targetMip(a) == 4, "...and falls to its initial window after");
    }

    // ---- hysteresis ---------------------------------------------------------------------------------------
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(2), 1);
        // The want must also leave the 8-frame window before a lower want counts, so keep feeding mip 5.
        bool early = false;
        for (u64 f = 1; f <= 38; ++f) {
            p.onSeen(a, fpFor(5), f);
            auto out = p.plan(f, 1ull << 40);
            if (f < 31 && !out.empty()) early = true;
        }
        check(!early, "a downgrade is not requested before its target has persisted 30 frames");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(2), 1);
        std::vector<PlannerRequest> out;
        u64 first = 0;
        for (u64 f = 1; f <= 40 && !first; ++f) {
            p.onSeen(a, fpFor(5), f);
            out = p.plan(f, 1ull << 40);
            if (!out.empty()) first = f;
        }
        check(first == 31, "the downgrade is requested on the 31st frame (target first lower at frame 1)");
        check(out.size() == 1 && out[0].trim() && out[0].toFirst == 5, "...as a trim to mip 5");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(2), 1);
        bool any = false;
        for (u64 f = 1; f <= 20; ++f) { p.onSeen(a, fpFor(5), f); any = any || !p.plan(f, 1ull << 40).empty(); }
        for (u64 f = 21; f <= 60; ++f) { p.onSeen(a, fpFor(2), f); any = any || !p.plan(f, 1ull << 40).empty(); }
        check(!any, "a want that returns before 30 frames resets the clock: nothing is ever requested");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(4), 1);
        for (u64 f = 1; f <= 5; ++f) { p.onSeen(a, fpFor(4), f); p.plan(f, 1ull << 40); }
        p.onSeen(a, fpFor(0), 6);
        auto out = p.plan(6, 1ull << 40);
        check(out.size() == 1 && !out[0].trim() && out[0].toFirst == 0 && out[0].fromFirst == 4, "an upgrade is requested the frame it is wanted");
        check(out[0].uploadBytes == F0 - F4, "...and reads exactly the missing mips (mip 0 down to mip 3)");
    }

    // ---- pressure forces the downgrade through the hysteresis ----------------------------------------------
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(0), 1);
        for (u64 f = 1; f <= 3; ++f) p.onSeen(a, fpFor(5), f);
        auto out = p.plan(3, F4);
        check(out.size() == 1 && out[0].trim() && out[0].toFirst >= 5, "over the pool a downgrade does not wait 30 frames");
    }

    // ---- residency never climbs past the pool ----------------------------------------------------------------
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(0), 1);   // resident at full detail, now wanted small
        const u32 b = p.add(tex4096(4), 1);   // resident small, now wanted at full detail
        const u64 pool = F0 + F1;
        u32 aTrimFrame = 0, bUpFrame = 0;
        std::vector<PlannerRequest> out;
        for (u64 f = 1; f <= 40; ++f) {
            p.onSeen(a, fpFor(4), f);
            p.onSeen(b, fpFor(0), f);
            out = p.plan(f, pool);
            if (const PlannerRequest* r = find(out, a)) {
                aTrimFrame = static_cast<u32>(f);
                p.complete(a, r->generation, r->toFirst, f);
            }
            if (const PlannerRequest* r = find(out, b)) {
                bUpFrame = static_cast<u32>(f);
                check(p.stats().residentBytes <= pool, "upgrade issued while residency is within the pool");
                p.complete(b, r->generation, r->toFirst, f);
            }
            check(p.stats().residentBytes <= pool || f == 0, "residency stays within the pool every frame");
        }
        check(aTrimFrame == 31 && bUpFrame == 31, "the upgrade waits for the trim that pays for it, and both land on frame 31");
    }

    // ---- caps ---------------------------------------------------------------------------------------------------
    {
        PlannerSettings s = base();
        s.maxSwapsPerFrame = 2;
        TexturePlanner p(s);
        std::vector<u32> ids;
        for (int i = 0; i < 5; ++i) ids.push_back(p.add(tex4096(4), 1));
        for (u32 id : ids) p.onSeen(id, fpFor(0), 1);
        check(p.plan(1, 1ull << 40).size() == 2, "at most maxSwapsPerFrame requests per frame");
    }
    {
        PlannerSettings s = base();
        s.uploadBytesPerFrame = 1ull << 20;
        TexturePlanner p(s);
        std::vector<u32> ids;
        for (int i = 0; i < 3; ++i) ids.push_back(p.add(tex4096(4), 1));
        for (u32 id : ids) p.onSeen(id, fpFor(0), 1);
        check(p.plan(1, 1ull << 40).size() == 1, "the upload budget admits one request larger than itself and then stops");
        p.plan(2, 1ull << 40);
    }
    {
        PlannerSettings s = base();
        s.maxSwapsPerFrame = 1;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(4), 1);   // wants 2 mips more
        const u32 b = p.add(tex4096(4), 1);   // wants 4 mips more
        p.onSeen(a, fpFor(2), 1);
        p.onSeen(b, fpFor(0), 1);
        auto out = p.plan(1, 1ull << 40);
        check(out.size() == 1 && out[0].id == b, "the larger detail deficit is served first");
    }

    // ---- pinned and unstreamable ---------------------------------------------------------------------------------
    {
        TexturePlanner p(base());
        const u32 pin = p.add(tex4096(0, true), 1);
        PlannerTextureDesc odd; odd.fullW = odd.fullH = 1500; odd.fullMips = 11; odd.firstMip = 0;
        const u32 odd1 = p.add(odd, 1);
        p.onSeen(pin, fpFor(8), 1);
        p.onSeen(odd1, fpFor(8), 1);
        auto out = p.plan(1, 1000);
        check(out.empty() && p.targetMip(pin) == 0 && p.targetMip(odd1) == 0, "a pinned texture and a 1500 texture are never touched, even over a tiny pool");
        check(p.stats().overPool && p.residentBytes(pin) == F0, "...their bytes still count and the planner reports it is over the pool");
        check(p.state(pin) == PlannerState::Pinned, "a pinned descriptor stays pinned");
    }

    // ---- generations: stale results are dropped ------------------------------------------------------------------
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(4), 1);
        p.onSeen(a, fpFor(0), 1);
        auto out = p.plan(1, 1ull << 40);
        check(out.size() == 1 && p.state(a) == PlannerState::Loading, "a request marks the texture Loading");
        check(p.plan(2, 1ull << 40).empty(), "a loading texture is not requested again");
        const u32 gen = out[0].generation;
        check(!p.complete(a, gen + 1, 0, 2), "a result with the wrong generation is dropped");
        check(p.complete(a, gen, 0, 2) && p.firstMip(a) == 0 && p.state(a) == PlannerState::Idle, "the right generation lands and sets the window");
        check(!p.complete(a, gen, 0, 3), "...but only once");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(4), 1);
        p.onSeen(a, fpFor(0), 1);
        const u32 gen = p.plan(1, 1ull << 40)[0].generation;
        p.remove(a);
        const u32 b = p.add(tex4096(4), 1);
        check(b == a && !p.complete(b, gen, 0, 2), "a result for a removed texture stays stale after its id is reused");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(4), 1);
        p.onSeen(a, fpFor(0), 1);
        const u32 gen = p.plan(1, 1ull << 40)[0].generation;
        p.pin(a);
        check(!p.complete(a, gen, 0, 2) && p.firstMip(a) == 4 && p.state(a) == PlannerState::Pinned, "pinning an in-flight texture drops its result");
    }
    {
        PlannerSettings s = base();
        s.retryFrames = 10;
        TexturePlanner p(s);
        const u32 a = p.add(tex4096(4), 1);
        for (u64 f = 1; f <= 30; ++f) p.onSeen(a, fpFor(0), f);
        const u32 gen = p.plan(1, 1ull << 40)[0].generation;
        p.fail(a, gen, false, 1);
        check(p.state(a) == PlannerState::Idle && p.firstMip(a) == 4, "a failed request leaves the window unchanged");
        check(p.plan(5, 1ull << 40).empty(), "...and waits retryFrames before asking again");
        check(p.plan(11, 1ull << 40).size() == 1, "...then asks again");
    }
    {
        TexturePlanner p(base());
        const u32 a = p.add(tex4096(4), 1);
        p.onSeen(a, fpFor(0), 1);
        const u32 gen = p.plan(1, 1ull << 40)[0].generation;
        p.fail(a, gen, true, 1);
        check(p.state(a) == PlannerState::Pinned, "a permanent failure pins the texture");
        check(p.plan(500, 1ull << 40).empty(), "...and it is never requested again");
        p.fail(a, gen - 5, false, 2);
        check(p.state(a) == PlannerState::Pinned, "a stale failure is ignored");
    }

    // ---- the pool under memory pressure ------------------------------------------------------------------------------
    {
        PoolSettings ps;
        ps.poolBytes = 1ull << 30;
        PoolController c(ps);
        const u64 GiB = 1ull << 30;
        const u64 pool0 = c.update(true, 8 * GiB, 5 * GiB, GiB, 700ull << 20, 0.0);
        check(pool0 == GiB, "an explicit pool is used as is when the device is comfortable");
        const u64 pool1 = c.update(true, 8 * GiB, 7600ull << 20, GiB, 700ull << 20, 1.0);
        check(pool1 < GiB && pool1 > (GiB * 8) / 10 && pool1 < (GiB * 9) / 10, "above 92% of the budget the pool shrinks by about 15% per second");
        const u64 pool2 = c.update(true, 8 * GiB, 7600ull << 20, GiB, 700ull << 20, 1.0);
        check(pool2 < pool1, "...and keeps shrinking while pressure lasts");
        const u64 floorBytes = (700ull << 20) + (128ull << 20);
        u64 last = pool2;
        for (int i = 0; i < 30; ++i) last = c.update(true, 8 * GiB, 7600ull << 20, GiB, 700ull << 20, 1.0);
        check(last == floorBytes, "...down to the tail sum plus 128 MiB and no further");
        const u64 mid = c.update(true, 8 * GiB, 7000ull << 20, GiB, 700ull << 20, 1.0);
        check(mid == floorBytes, "between the water marks the pool neither shrinks nor grows");
        u64 grown = mid;
        for (int i = 0; i < 400; ++i) grown = c.update(true, 8 * GiB, 5 * GiB, GiB, 700ull << 20, 1.0);
        check(grown == GiB && c.scale() == 1.0, "below 85% it recovers by 2% per second back to the full pool");
    }
    {
        PoolSettings ps;
        PoolController c(ps);
        const u64 GiB = 1ull << 30;
        check(c.update(true, 16 * GiB, 4 * GiB, GiB, 0, 0.0) == 4 * GiB, "auto: a quarter of the budget");
        PoolController c2(ps);
        check(c2.update(true, 16 * GiB, 14 * GiB, GiB, 0, 0.0) == 16 * GiB - 13 * GiB - GiB, "auto: the room left is budget - non-texture usage - reserve (2 GiB here)");
        PoolController c3(ps);
        check(c3.update(true, 512ull << 20, 0, 0, 1000, 0.0) == 1000 + (128ull << 20), "auto: a tiny budget falls back to the floor");
        PoolController c4(ps);
        check(c4.update(false, 0, 0, 0, 0, 1.0) == GiB, "a device that cannot report VRAM gets the 1 GiB default");
        PoolSettings ps2; ps2.poolBytes = 512ull << 20;
        PoolController c5(ps2);
        check(c5.update(false, 0, 0, 0, 0, 1.0) == (512ull << 20), "...or the explicit pool");
        PoolSettings ps3; ps3.reserveBytes = 256ull << 20;
        PoolController c6(ps3);
        check(c6.update(true, 512ull << 20, 0, 0, 0, 0.0) == (256ull << 20), "auto: the 256 MiB minimum applies below the quarter-budget fraction, room permitting");
    }

    // ---- the staging ring ---------------------------------------------------------------------------------------------
    {
        UploadRing r(8192, 512, 4);
        check(r.reserve(1000) == 0, "the first region starts at 0 and takes 1024 bytes (512-aligned)");
        check(r.reserve(1000) == 1024, "the next follows it");
        check(r.reserve(3000) == 2048, "a 3000-byte region takes 3072");
        check(r.inUse() == 1024 + 1024 + 3072 && r.regions() == 3, "in-use bytes are the aligned sizes");
        check(r.reserve(4000) == UploadRing::kNone, "a request that does not fit anywhere is refused (5120 + 4096 > 8192)");
        check(r.reserve(3072) == 5120, "...but one that fits the tail is served");
        check(r.cancel(0) && r.reserve(500) == 0, "a cancelled region is reusable at once");
        r.cancel(0);

        check(r.submit(1024, 10), "a region is tagged with the frame that reads it");
        check(!r.submit(777, 10), "an unknown offset cannot be submitted");
        check(!r.cancel(1024), "a submitted region cannot be cancelled");
        r.retire(13);
        check(r.reserve(1024) == 0 && r.reserve(1024) == UploadRing::kNone, "three frames later the GPU may still be reading it: only the cancelled gap is free");
        r.cancel(0);
        r.retire(14);
        check(r.reserve(1024) == 0, "(gap at 0 reusable)");
        check(r.reserve(1024) == 1024, "four frames later the submitted region is free and is reused");
    }
    {
        UploadRing r(4096, 512, 4);
        const u64 a = r.reserve(1024), b = r.reserve(1024), c = r.reserve(1024);
        r.submit(a, 1); r.submit(b, 2); r.submit(c, 3);
        r.retire(5);
        check(r.regions() == 2, "retire frees only the regions old enough (frame 1 at frame 5)");
        r.retire(100);
        check(r.regions() <= 1 && r.inUse() <= 1536, "everything submitted eventually retires");
        UploadRing z(1024, 512, 4);
        check(z.reserve(0) == 0 && z.inUse() == 512, "a zero-byte request still takes one aligned unit so offsets stay unique");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
