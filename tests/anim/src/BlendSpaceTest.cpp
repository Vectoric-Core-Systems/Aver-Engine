// Blend spaces: weights, triangulation, sync-marker phase and pose blending, with no GPU.
//
// What can go wrong here is invisible until a character walks: weights that do not sum to one make
// the pose pop, a triangulation hole makes it snap, and a sync phase that is off by half a cycle makes
// the feet slide while looking plausible. All of it is arithmetic.
#include "aver/anim/BlendSpace.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

static anim::BlendSample sample(const char* clip, f32 x, f32 y = 0.0f) {
    anim::BlendSample s;
    s.clip = clip;
    s.x = x;
    s.y = y;
    return s;
}

static f32 sumOf(const std::vector<anim::SampleWeight>& w) {
    f32 s = 0.0f;
    for (const auto& e : w) s += e.weight;
    return s;
}

static bool nonNegative(const std::vector<anim::SampleWeight>& w) {
    for (const auto& e : w)
        if (e.weight < 0.0f) return false;
    return true;
}

static f32 weightOf(const std::vector<anim::SampleWeight>& w, u32 index) {
    for (const auto& e : w)
        if (e.index == index) return e.weight;
    return 0.0f;
}

// A deterministic stream in [0,1).
static f32 rnd(u32& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<f32>((s >> 8) & 0xFFFF) / 65536.0f;
}

static anim::BlendSpaceAsset space1D() {
    anim::BlendSpaceAsset a;
    a.dims = 1;
    a.axisX = {"speed", 0.0f, 400.0f, 0.0f};
    // Deliberately authored out of order: the topology sorts.
    a.samples = {sample("run", 400.0f), sample("idle", 0.0f), sample("walk", 150.0f)};
    return a;
}

static anim::BlendSpaceAsset gridSpace() {
    anim::BlendSpaceAsset a;
    a.dims = 2;
    a.axisX = {"x", 0.0f, 1.0f, 0.0f};
    a.axisY = {"y", 0.0f, 1.0f, 0.0f};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) a.samples.push_back(sample("c", i * 0.5f, j * 0.5f));
    return a;
}

int main() {
    AVER_INFO("BlendSpaceTest");

    AVER_INFO("1D weights");
    {
        const anim::BlendSpaceAsset a = space1D();
        check(a.valid(), "the 1D space is valid");
        const anim::BlendTopology t = anim::buildBlendTopology(a);
        std::vector<anim::SampleWeight> w;

        anim::computeBlendWeights(a, t, 0.0f, 0.0f, w);
        checkNear(weightOf(w, 1), 1.0f, 1e-6f, "at the idle sample it is all idle");
        anim::computeBlendWeights(a, t, 75.0f, 0.0f, w);
        checkNear(weightOf(w, 1), 0.5f, 1e-5f, "half way to walk: idle half");
        checkNear(weightOf(w, 2), 0.5f, 1e-5f, "half way to walk: walk half");
        anim::computeBlendWeights(a, t, 275.0f, 0.0f, w);
        checkNear(weightOf(w, 2), 0.5f, 1e-5f, "between walk and run: walk half");
        checkNear(weightOf(w, 0), 0.5f, 1e-5f, "between walk and run: run half");
        anim::computeBlendWeights(a, t, -50.0f, 0.0f, w);
        checkNear(weightOf(w, 1), 1.0f, 1e-6f, "below the range clamps to the first sample");
        anim::computeBlendWeights(a, t, 9999.0f, 0.0f, w);
        checkNear(weightOf(w, 0), 1.0f, 1e-6f, "above the range clamps to the last sample");

        bool allOne = true, allPos = true;
        for (int i = -20; i <= 500; ++i) {
            anim::computeBlendWeights(a, t, static_cast<f32>(i) * 1.37f, 0.0f, w);
            if (std::fabs(sumOf(w) - 1.0f) > 1e-5f) allOne = false;
            if (!nonNegative(w)) allPos = false;
        }
        check(allOne, "1D weights sum to one across a sweep, inside and outside the range");
        check(allPos, "1D weights are never negative");
    }

    AVER_INFO("2D triangulation on a grid");
    {
        const anim::BlendSpaceAsset a = gridSpace();
        const anim::BlendTopology t = anim::buildBlendTopology(a);
        check(t.tris.size() == 8, "a 3x3 grid triangulates into eight triangles");
        std::vector<anim::SampleWeight> w;

        bool exact = true;
        for (u32 i = 0; i < a.samples.size(); ++i) {
            anim::computeBlendWeights(a, t, a.samples[i].x, a.samples[i].y, w);
            if (std::fabs(weightOf(w, i) - 1.0f) > 1e-4f) exact = false;
        }
        check(exact, "a query on a sample position gives that sample weight one");

        u32 seed = 12345;
        bool sums = true, reproduces = true, positive = true;
        f32 worst = 0.0f;
        for (int n = 0; n < 600; ++n) {
            const f32 qx = rnd(seed), qy = rnd(seed);
            anim::computeBlendWeights(a, t, qx, qy, w);
            if (std::fabs(sumOf(w) - 1.0f) > 1e-5f) sums = false;
            if (!nonNegative(w)) positive = false;
            f32 px = 0, py = 0;
            for (const auto& e : w) { px += e.weight * a.samples[e.index].x; py += e.weight * a.samples[e.index].y; }
            worst = std::fmax(worst, std::fmax(std::fabs(px - qx), std::fabs(py - qy)));
            if (std::fabs(px - qx) > 1e-4f || std::fabs(py - qy) > 1e-4f) reproduces = false;
        }
        check(sums, "2D weights sum to one for 600 interior queries");
        check(positive, "2D weights are never negative");
        check(reproduces, "the weights reproduce the query position (no hole, no wrong triangle)");
        AVER_INFO("        worst reproduction error {:.7f}", worst);

        bool outside = true;
        for (int n = 0; n < 200; ++n) {
            const f32 qx = rnd(seed) * 3.0f - 1.0f, qy = rnd(seed) * 3.0f - 1.0f;
            anim::computeBlendWeights(a, t, qx, qy, w);
            f32 px = 0, py = 0;
            for (const auto& e : w) { px += e.weight * a.samples[e.index].x; py += e.weight * a.samples[e.index].y; }
            const f32 cx = std::fmin(std::fmax(qx, 0.0f), 1.0f), cy = std::fmin(std::fmax(qy, 0.0f), 1.0f);
            if (std::fabs(sumOf(w) - 1.0f) > 1e-5f || !nonNegative(w) ||
                std::fabs(px - cx) > 1e-3f || std::fabs(py - cy) > 1e-3f) outside = false;
        }
        check(outside, "outside the hull the result is the nearest point of the space, still summing to one");
    }

    AVER_INFO("2D irregular layout, duplicates and collinear");
    {
        anim::BlendSpaceAsset a;
        a.dims = 2;
        a.axisX = {"dir", -180.0f, 180.0f, 0.0f};
        a.axisY = {"speed", 0.0f, 600.0f, 0.0f};
        a.samples = {sample("idle", 0, 0), sample("fwd", 0, 300), sample("right", 90, 300),
                     sample("left", -90, 300), sample("back", 180, 200), sample("run", 0, 600)};
        const anim::BlendTopology t = anim::buildBlendTopology(a);
        check(!t.tris.empty(), "an irregular layout triangulates");
        std::vector<anim::SampleWeight> w;
        u32 seed = 777;
        bool ok = true;
        for (int n = 0; n < 400; ++n) {
            const f32 qx = (rnd(seed) * 2.0f - 1.0f) * 200.0f, qy = rnd(seed) * 700.0f - 50.0f;
            anim::computeBlendWeights(a, t, qx, qy, w);
            if (std::fabs(sumOf(w) - 1.0f) > 1e-5f || !nonNegative(w)) ok = false;
        }
        check(ok, "arbitrary queries, inside and outside, always sum to one");

        // A duplicate position must not poison the mesh.
        a.samples.push_back(sample("dup", 0, 300));
        const anim::BlendTopology t2 = anim::buildBlendTopology(a);
        anim::computeBlendWeights(a, t2, 30.0f, 250.0f, w);
        check(std::fabs(sumOf(w) - 1.0f) < 1e-5f, "a duplicate sample position still gives weights summing to one");

        anim::BlendSpaceAsset line;
        line.dims = 2;
        line.axisX = {"x", 0, 1, 0};
        line.axisY = {"y", 0, 1, 0};
        line.samples = {sample("a", 0, 0), sample("b", 0.5f, 0.5f), sample("c", 1, 1)};
        const anim::BlendTopology tl = anim::buildBlendTopology(line);
        check(tl.collinear, "three collinear 2D samples are detected");
        anim::computeBlendWeights(line, tl, 0.25f, 0.25f, w);
        checkNear(sumOf(w), 1.0f, 1e-5f, "collinear weights sum to one");
        checkNear(weightOf(w, 0), 0.5f, 1e-4f, "collinear: half way a to b");
        checkNear(weightOf(w, 1), 0.5f, 1e-4f, "collinear: half way b from a");
    }

    AVER_INFO("sync markers");
    {
        // Walk is 1.0 s with L at 0.0 and R at 0.5. Run is 0.6 s with L at 0.1 and R at 0.4, so a
        // plain normalised-time blend would put the run's right foot down at phase 0.667.
        anim::BlendSpaceAsset a;
        a.dims = 1;
        a.axisX = {"speed", 0, 400, 0};
        a.samples = {sample("walk", 0.0f), sample("run", 400.0f)};
        a.samples[0].markers = {{"L", 0.0f}, {"R", 0.5f}};
        a.samples[1].markers = {{"L", 0.1f}, {"R", 0.4f}};

        anim::BlendSpacePlayer p;
        p.bindDurations(&a, {1.0f, 0.6f});
        p.setInput(0.0f, 0.0f);
        p.snapInput();
        p.setPhase(0.5f);
        checkNear(p.sampleTime(0), 0.5f, 1e-5f, "phase 0.5: walk is on its R marker");

        p.setInput(100.0f, 0.0f);
        p.snapInput();
        p.setPhase(0.5f);
        checkNear(p.sampleTime(0), 0.5f, 1e-5f, "blended: walk's R marker at phase 0.5");
        checkNear(p.sampleTime(1), 0.4f, 1e-5f, "blended: run's R marker at the SAME phase");
        p.setPhase(0.0f);
        checkNear(p.sampleTime(1), 0.1f, 1e-5f, "phase 0: run is on its L marker, not at time 0");

        // Both clips agree on the phase at every step of a playthrough, and the cycle length is the
        // weighted average of the two clip lengths.
        p.setInput(200.0f, 0.0f);
        p.snapInput();
        p.setPhase(0.0f);
        checkNear(p.duration(), 0.8f, 1e-5f, "50/50 blend runs a 0.8 s cycle");
        bool agree = true;
        for (int i = 0; i < 40; ++i) {
            p.advance(0.02f);
            anim::SyncCycle cw, cr;
            anim::makeSyncCycle(a.samples[0].markers, 1.0f, {"L", "R"}, cw);
            anim::makeSyncCycle(a.samples[1].markers, 0.6f, {"L", "R"}, cr);
            const f32 pw = anim::syncTimeToPhase(cw, p.sampleTime(0));
            const f32 pr = anim::syncTimeToPhase(cr, p.sampleTime(1));
            if (std::fabs(pw - p.phase()) > 1e-4f || std::fabs(pr - p.phase()) > 1e-4f) agree = false;
        }
        check(agree, "walk and run report the same phase through a whole playthrough");
        checkNear(p.cycles(), 1.0f, 1e-3f, "0.8 s of playback at a 0.8 s cycle is one cycle");

        // A clip authored with the other foot first rotates onto the leader's names.
        anim::BlendSpaceAsset b = a;
        b.samples[1].markers = {{"R", 0.1f}, {"L", 0.4f}};
        anim::BlendSpacePlayer q;
        q.bindDurations(&b, {1.0f, 0.6f});
        q.setInput(100.0f, 0.0f);
        q.snapInput();
        q.setPhase(0.0f);
        checkNear(q.sampleTime(1), 0.4f, 1e-5f, "a rotated clip starts its cycle on L");
        q.setPhase(0.5f);
        checkNear(q.sampleTime(1), 0.1f, 1e-5f, "and reaches R half a cycle later");
        q.setPhase(0.25f);
        checkNear(q.sampleTime(1), 0.55f, 1e-5f, "between them it wraps through the clip end (0.4 -> 0.7 -> 0.1)");

        // Markers that do not line up fall back to linear phase rather than guessing.
        anim::BlendSpaceAsset c = a;
        c.samples[1].markers = {{"L", 0.1f}, {"X", 0.3f}, {"R", 0.4f}};
        anim::BlendSpacePlayer r;
        r.bindDurations(&c, {1.0f, 0.6f});
        r.setInput(100.0f, 0.0f);
        r.snapInput();
        r.setPhase(0.5f);
        checkNear(r.sampleTime(1), 0.3f, 1e-5f, "mismatched markers: linear phase (0.5 of 0.6 s)");

        // Phase <-> time is a bijection on a cycle.
        anim::SyncCycle cy;
        anim::makeSyncCycle(a.samples[1].markers, 0.6f, {"L", "R"}, cy);
        bool inverse = true;
        for (int i = 0; i < 50; ++i) {
            const f32 ph = static_cast<f32>(i) / 50.0f;
            if (std::fabs(anim::syncTimeToPhase(cy, anim::syncPhaseToTime(cy, ph)) - ph) > 1e-4f) inverse = false;
        }
        check(inverse, "syncTimeToPhase inverts syncPhaseToTime");

        anim::BlendSpaceAsset off = a;
        off.syncMarkers = false;
        anim::BlendSpacePlayer u;
        u.bindDurations(&off, {1.0f, 0.6f});
        u.setInput(100.0f, 0.0f);
        u.snapInput();
        u.setPhase(0.5f);
        checkNear(u.sampleTime(1), 0.3f, 1e-5f, "sync switched off: plain normalised time");
    }

    AVER_INFO("input smoothing");
    {
        anim::BlendSpaceAsset a = space1D();
        a.axisX.smoothing = 0.25f;
        anim::BlendSpacePlayer p;
        p.bindDurations(&a, {1.0f, 1.0f, 1.0f});
        p.setInput(0.0f, 0.0f);
        p.snapInput();
        p.setInput(400.0f, 0.0f);
        p.advance(0.1f);
        check(p.x() > 0.0f && p.x() < 400.0f, "a smoothed input moves toward the target without jumping");
        for (int i = 0; i < 100; ++i) p.advance(0.1f);
        checkNear(p.x(), 400.0f, 0.5f, "and arrives");
    }

    AVER_INFO("pose blending");
    {
        fmt::OcSkeleton skel;
        fmt::OcBone root;
        root.name = "root";
        skel.bones = {root};
        skel.rootBone = 0;
        auto slide = [](f32 to, f32 dur) {
            fmt::OcAnimation c;
            c.duration = dur;
            c.flags = fmt::kOcAnimLoop;
            fmt::OcTrack t;
            t.boneIndex = 0;
            t.channels = fmt::kOcChannelTranslation;
            t.times = {0.0f, dur};
            t.values = {0, 0, 0, to, 0, 0};
            c.tracks.push_back(t);
            return c;
        };
        const fmt::OcAnimation a = slide(100.0f, 1.0f), b = slide(300.0f, 1.0f);
        anim::BlendSpaceAsset s;
        s.dims = 1;
        s.axisX = {"x", 0, 1, 0};
        s.samples = {sample("a", 0.0f), sample("b", 1.0f)};
        anim::BlendSpacePlayer p;
        p.bind(&s, {&a, &b});
        p.setInput(0.25f, 0.0f);
        p.snapInput();
        p.setPhase(0.5f);
        anim::Pose pose;
        p.evaluate(skel, pose);
        // 0.75 of 50 + 0.25 of 150.
        checkNear(pose.local[0].position.x, 0.75f * 50.0f + 0.25f * 150.0f, 1e-3f, "the posed bone is the weighted mix of the clips");
    }

    AVER_INFO(g_failures == 0 ? "BlendSpaceTest: PASS" : "BlendSpaceTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
