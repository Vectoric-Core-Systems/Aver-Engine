// LightTreeTest -- the CPU half of many-light direct lighting (aver/voxi/LightTree.hpp, docs/plans/wave4/MANY_LIGHTS.md
// section 5A): the BVH's structure and packing, the canonical order and fill(), the importance bound against the light
// list's cheap irradiance, the descent's probability against the path walk, a CPU twin of the RIS estimator against the
// brute-force sum, and degenerate inputs. The HLSL (shaders/voxi_lighttree.hlsli) is the code that runs; its record
// offsets and flag bits are read at the end and checked against the header.
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/voxi/LightTree.hpp"
#include "aver/voxi/SceneLight.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::voxi;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// ---- a small deterministic generator (PCG32) ----
struct Rng {
    u64 state = 0x853c49e6748fea9bull, inc = 0xda3e39cb94b95bdbull;
    explicit Rng(u64 seed = 1) { state = 0; next(); state += seed * 0x9E3779B97F4A7C15ull + 1; next(); }
    u32 next() {
        const u64 old = state;
        state = old * 6364136223846793005ull + inc;
        const u32 xs = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xs >> rot) | (xs << ((~rot + 1u) & 31u));
    }
    f32 u01() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * u01(); }
};

static void unitVec(Rng& r, f32 out[3]) {
    for (;;) {
        const f32 x = r.range(-1, 1), y = r.range(-1, 1), z = r.range(-1, 1);
        const f32 l2 = x * x + y * y + z * z;
        if (l2 > 1e-4f && l2 <= 1.0f) {
            const f32 inv = 1.0f / std::sqrt(l2);
            out[0] = x * inv; out[1] = y * inv; out[2] = z * inv;
            return;
        }
    }
}

// A random scene light packed as the renderer packs it. kind: 0 point, 1 spot, 3 rect (SceneLight's numbering).
static PackedLight randomLight(Rng& r, int kind, const f32 lo[3], const f32 hi[3], bool shadow = true) {
    SceneLight s;
    s.kind = kind;
    for (int a = 0; a < 3; ++a) s.pos[a] = r.range(lo[a], hi[a]);
    f32 ax[3];
    unitVec(r, ax);
    for (int a = 0; a < 3; ++a) s.axis[a] = ax[a];
    f32 t[3];
    unitVec(r, t);
    // any tangent: remove the axis component
    const f32 d = t[0] * ax[0] + t[1] * ax[1] + t[2] * ax[2];
    f32 rr[3] = {t[0] - ax[0] * d, t[1] - ax[1] * d, t[2] - ax[2] * d};
    const f32 rl = std::sqrt(rr[0] * rr[0] + rr[1] * rr[1] + rr[2] * rr[2]);
    for (int a = 0; a < 3; ++a) s.right[a] = rl > 1e-3f ? rr[a] / rl : (a == 0 ? 1.0f : 0.0f);
    for (int a = 0; a < 3; ++a) s.colour[a] = r.range(0.2f, 1.0f);
    s.intensityCd = std::exp(r.range(std::log(200.0f), std::log(60000.0f)));
    s.rangeCm = r.range(300.0f, 4000.0f);
    s.outerCos = r.range(0.2f, 0.9f);
    s.innerCos = std::min(s.outerCos + r.range(0.02f, 0.08f), 1.0f);
    s.widthCm = r.range(20.0f, 400.0f);
    s.heightCm = r.range(20.0f, 300.0f);
    s.radiusCm = r.range(1.0f, 25.0f);
    s.castShadows = shadow;
    return packSceneLight(s, SceneLightAssets{});
}

static std::vector<PackedLight> randomScene(Rng& r, u32 n, const f32 lo[3], const f32 hi[3], bool mixed = true,
                                            bool shadow = true) {
    std::vector<PackedLight> L(n);
    for (u32 i = 0; i < n; ++i) {
        const u32 k = mixed ? r.next() % 5u : 0u;   // 0 point, 1 spot, 2 rect, 3 and 4 point
        L[i] = randomLight(r, k == 1 ? 1 : (k == 2 ? 3 : 0), lo, hi, shadow);
    }
    return L;
}

static bool sameBytes(const TreeNodeRec& a, const TreeNodeRec& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
static u32 flagsOf(const TreeNodeRec& n) { return static_cast<u32>(n.flags + 0.5f); }
static bool isLeaf(const TreeNodeRec& n) { return (flagsOf(n) & kTreeFlagLeaf) != 0u; }

// Collects the light-list indices under a node.
static void leavesUnder(const LightTree& t, u32 node, std::vector<u32>& out) {
    const TreeNodeRec& n = t.nodes()[node];
    if (isLeaf(n)) { out.push_back(static_cast<u32>(n.right + 0.5f)); return; }
    leavesUnder(t, static_cast<u32>(n.left + 0.5f), out);
    leavesUnder(t, static_cast<u32>(n.right + 0.5f), out);
}

static f32 angleBetween(const f32 a[3], const f32 b[3]) {
    const f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::acos(std::clamp(d, -1.0f, 1.0f));
}

// ---- structure -----------------------------------------------------------------------------------------
static void structureChecks(const std::vector<PackedLight>& L, const char* name, bool allNoShadow = false) {
    LightTree t;
    const bool built = t.build(L.data(), static_cast<u32>(L.size()));
    const u32 N = static_cast<u32>(L.size());
    check(built && t.valid(), std::string(name) + ": builds");
    if (!built) return;
    const auto& nodes = t.nodes();
    check(nodes.size() == 2u * N - 1u, std::string(name) + ": 2N-1 nodes");
    u32 leaves = 0;
    bool aabb = true, phi = true, cone = true, parents = true, flags = true;
    std::vector<u8> seen(N, 0);
    for (u32 i = 0; i < nodes.size(); ++i) {
        const TreeNodeRec& n = nodes[i];
        if (isLeaf(n)) {
            ++leaves;
            const u32 li = static_cast<u32>(n.right + 0.5f);
            if (li >= N || seen[li]) { parents = false; continue; }
            seen[li] = 1;
            if (n.left != -1.0f) parents = false;
            for (int a = 0; a < 3; ++a) if (n.bmin[a] != L[li].posRadius[a] || n.bmax[a] != L[li].posRadius[a]) aabb = false;
            if (allNoShadow && (flagsOf(n) & kTreeFlagNoShadow) == 0u) flags = false;
            continue;
        }
        const u32 l = static_cast<u32>(n.left + 0.5f), r = static_cast<u32>(n.right + 0.5f);
        if (l >= nodes.size() || r >= nodes.size()) { parents = false; continue; }
        const TreeNodeRec& A = nodes[l];
        const TreeNodeRec& B = nodes[r];
        if (static_cast<u32>(A.parent + 0.5f) != i || static_cast<u32>(B.parent + 0.5f) != i) parents = false;
        for (int a = 0; a < 3; ++a) {
            if (n.bmin[a] > std::min(A.bmin[a], B.bmin[a]) || n.bmax[a] < std::max(A.bmax[a], B.bmax[a])) aabb = false;
        }
        if (std::fabs(n.phi - (A.phi + B.phi)) > 1e-4f * std::max(n.phi, 1.0f)) phi = false;
        if (n.range < std::max(A.range, B.range)) phi = false;
        // The cone holds both children's cones (omni holds everything).
        if (n.cosThetaO > -0.99999f) {
            for (const TreeNodeRec* c : {&A, &B}) {
                if (c->cosThetaO <= -0.99999f) { cone = false; continue; }
                const f32 thP = std::acos(std::clamp(n.cosThetaO, -1.0f, 1.0f));
                const f32 thC = std::acos(std::clamp(c->cosThetaO, -1.0f, 1.0f));
                if (angleBetween(n.axis, c->axis) + thC > thP + 2e-3f) cone = false;
            }
        }
        if (n.cosThetaE > std::min(A.cosThetaE, B.cosThetaE) + 1e-6f) cone = false;   // spread = the widest
        if (allNoShadow && (flagsOf(n) & kTreeFlagNoShadow) == 0u) flags = false;
    }
    check(parents && leaves == N, std::string(name) + ": one leaf per light, each list index once, parents consistent");
    check(aabb, std::string(name) + ": AABBs contain their children's (leaves sit on their light)");
    check(phi, std::string(name) + ": power sums, range is the max");
    check(cone, std::string(name) + ": cones contain the children's cones, spread is the widest");
    check(flags, std::string(name) + ": no-shadow flag");
    check(t.depth() <= kLightTreeMaxDepth, std::string(name) + ": depth " + std::to_string(t.depth()) + " <= 40");
    bool map = true;
    for (u32 i = 0; i < N; ++i) {
        const i32 leaf = t.leafOfList(i);
        if (leaf < 0 || static_cast<u32>(nodes[static_cast<usize>(leaf)].right + 0.5f) != i) map = false;
    }
    check(map, std::string(name) + ": the leaf map inverts the leaves' list indices");
}

// ---- the bound ----------------------------------------------------------------------------------------------
static f32 sumCheap(const std::vector<PackedLight>& L, const std::vector<u32>& idx, const f32 p[3], const f32 n[3]) {
    f32 s = 0.0f;
    for (const u32 i : idx) s += lightCheapLuminance(L[i], p, n);
    return s;
}

// The probability mass that reaches a leaf (descents that die at a node whose children are both zero lose theirs).
static f32 reachMass(const LightTree& t, u32 node, const f32 p[3], const f32 n[3]) {
    const TreeNodeRec& nd = t.nodes()[node];
    if (isLeaf(nd)) return 1.0f;
    const u32 l = static_cast<u32>(nd.left + 0.5f), r = static_cast<u32>(nd.right + 0.5f);
    const f32 il = LightTree::importance(t.nodes()[l], p, n), ir = LightTree::importance(t.nodes()[r], p, n);
    const f32 sum = il + ir;
    if (!(sum > 0.0f)) return 0.0f;
    return (il / sum) * reachMass(t, l, p, n) + (ir / sum) * reachMass(t, r, p, n);
}

// ---- the estimator's CPU twin (voxi_pt.hlsli averManyLights) -------------------------------------------------
struct Scene {
    std::vector<PackedLight> L;
    LightTree tree;
    f32 p[3] = {0, 0, 0}, n[3] = {0, 0, 1};
    std::vector<f32> g, h, v;   // phat factor, shading factor, visibility per light (all positive; v in [0,1])
    f32 f(u32 y) const { return lightCheapLuminance(L[y], p, n) * h[y]; }
    f32 phat(u32 y) const { return lightCheapLuminance(L[y], p, n) * g[y]; }
};

// One estimate of sum over y != e0 of f(y) V(y). tau is the roulette threshold on a slot's contribution estimate.
static f32 estimate(const Scene& sc, Rng& rng, u32 e0, u32 M, u32 S, f32 tau) {
    const bool e0Local = e0 != 0xFFFFFFFFu;
    f32 mult = 1.0f;
    if (e0Local) {
        const i32 leaf = sc.tree.leafOfList(e0);
        const f32 q0 = leaf >= 0 ? sc.tree.pdfOf(static_cast<u32>(leaf), sc.p, sc.n) : 0.0f;
        mult = 1.0f + q0 + q0 * q0;
    }
    const u32 tries = e0Local ? 3u : 1u;
    u32 sel[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    f32 ph[4] = {0, 0, 0, 0};
    f32 wsum = 0.0f;
    for (u32 j = 0; j < M; ++j) {
        u32 y = 0xFFFFFFFFu;
        f32 q = 0.0f;
        for (u32 t = 0; t < tries; ++t) {
            u32 yy;
            f32 qq;
            if (!sc.tree.sample(sc.p, sc.n, rng.u01(), yy, qq)) break;
            if (yy != e0) { y = yy; q = qq; break; }
        }
        if (y == 0xFFFFFFFFu) continue;
        const f32 pv = sc.phat(y);
        if (!(pv > 0.0f)) continue;
        const f32 w = pv / (q * mult);
        wsum += w;
        for (u32 s = 0; s < S; ++s)
            if (rng.u01() * wsum < w) { sel[s] = y; ph[s] = pv; }
    }
    f32 total = 0.0f;
    for (u32 s = 0; s < S; ++s) {
        if (sel[s] == 0xFFFFFFFFu) continue;
        const f32 c = wsum / static_cast<f32>(M);
        f32 W = c / ph[s];
        if (c < tau) {
            const f32 pr = c / tau;
            if (rng.u01() >= pr) continue;
            W /= pr;
        }
        total += sc.f(sel[s]) * sc.v[sel[s]] * W / static_cast<f32>(S);
    }
    return total;
}

static void unbiasednessChecks() {
    AVER_INFO("=== unbiasedness: the RIS twin against the brute-force sum ===");
    Rng r(7);
    const f32 lo[3] = {-1500, -1500, 100}, hi[3] = {1500, 1500, 700};
    Scene sc;
    sc.L = randomScene(r, 64, lo, hi, true, true);
    check(sc.tree.build(sc.L.data(), 64), "64-light scene builds");
    sc.p[2] = 50.0f;
    sc.g.resize(64); sc.h.resize(64); sc.v.resize(64);
    for (u32 i = 0; i < 64; ++i) {
        sc.g[i] = 0.8f + 0.4f * r.u01();       // the selection density's surface factor, off from the shading factor
        sc.h[i] = 0.8f + 0.4f * r.u01();
        sc.v[i] = i % 5u == 0u ? 0.0f : 0.4f + 0.6f * r.u01();   // a fifth fully blocked, the rest partly
    }
    // The exact light: the strongest by cheap luminance, as CSRdShadow's weight picks it.
    u32 e0 = 0;
    f32 best = -1.0f;
    for (u32 i = 0; i < 64; ++i) { const f32 w = lightCheapLuminance(sc.L[i], sc.p, sc.n); if (w > best) { best = w; e0 = i; } }
    for (const bool withE0 : {false, true}) {
        f32 truth = 0.0f;
        for (u32 y = 0; y < 64; ++y) if (!(withE0 && y == e0)) truth += sc.f(y) * sc.v[y];
        check(truth > 0.0f, std::string("reference sum is positive") + (withE0 ? " (e0 excluded)" : ""));
        f32 totalPhat = 0.0f;
        for (u32 y = 0; y < 64; ++y) if (!(withE0 && y == e0)) totalPhat += sc.phat(y);
        for (const u32 M : {1u, 4u, 16u}) {
            for (const u32 S : {1u, 4u}) {
                for (const bool roulette : {false, true}) {
                    if (roulette && M != 4u) continue;   // roulette is checked at one candidate count, both slot counts
                    Rng rng(1000 + M * 13 + S);
                    // A threshold at the typical slot estimate (the target mass), so roulette fires on most draws with p near 0.7.
                    const f32 tau = roulette ? totalPhat : 0.0f;
                    f64 sum = 0.0, sum2 = 0.0;
                    u64 count = 0;
                    const u64 maxEst = std::max<u64>(60000, 1200000 / M);
                    for (; count < maxEst;) {
                        for (u32 b = 0; b < 10000; ++b) {
                            const f64 e = estimate(sc, rng, withE0 ? e0 : 0xFFFFFFFFu, M, S, tau);
                            sum += e; sum2 += e * e; ++count;
                        }
                        const f64 mean = sum / static_cast<f64>(count);
                        const f64 var = std::max(sum2 / static_cast<f64>(count) - mean * mean, 0.0);
                        if (std::sqrt(var / static_cast<f64>(count)) < 0.0008 * mean) break;   // 0.08% standard error is enough
                    }
                    const f64 mean = sum / static_cast<f64>(count);
                    const f64 var = std::max(sum2 / static_cast<f64>(count) - mean * mean, 0.0);
                    const f64 se = std::sqrt(var / static_cast<f64>(count));
                    const f64 err = std::fabs(mean - truth);
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "M=%u S=%u%s%s: mean %.4f vs %.4f (error %.3f%%, se %.3f%%, %llu estimates)", M, S,
                                  withE0 ? " e0" : "", roulette ? " roulette" : "", mean, static_cast<f64>(truth),
                                  100.0 * err / truth, 100.0 * se / truth, static_cast<unsigned long long>(count));
                    check(err <= std::max(0.005 * truth, 4.5 * se), buf);
                }
            }
        }
    }
}

int main() {
    AVER_INFO("=== LightTreeTest ===");

    // ---------------------------------------------------------------- structure
    {
        AVER_INFO("=== structure ===");
        Rng r(3);
        const f32 lo[3] = {-5000, -5000, 0}, hi[3] = {5000, 5000, 800};
        structureChecks(randomScene(r, 2, lo, hi), "N=2");
        structureChecks(randomScene(r, 4, lo, hi), "N=4");
        structureChecks(randomScene(r, 257, lo, hi), "N=257 mixed");
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<PackedLight> big = randomScene(r, 4000, lo, hi);
        structureChecks(big, "N=4000 mixed");
        {
            LightTree t;
            const auto b0 = std::chrono::steady_clock::now();
            t.build(big.data(), 4000);
            const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - b0).count();
            AVER_INFO("   info  4000-light build: {:.2f} ms (target 0.3 ms release; {:.1f} ms with the scene generator)", ms,
                      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        // coincident lights: all Morton codes equal, the median split must keep the depth
        std::vector<PackedLight> same(4000);
        {
            Rng r2(9);
            const f32 p0[3] = {100, 200, 300};
            for (u32 i = 0; i < 4000; ++i) same[i] = randomLight(r2, 0, p0, p0);
        }
        structureChecks(same, "N=4000 coincident");
        // positions doubling along a line: the longest Morton chain a scene can make
        std::vector<PackedLight> chain(200);
        {
            Rng r2(11);
            for (u32 i = 0; i < 200; ++i) {
                f32 pp[3] = {std::pow(1.07f, static_cast<f32>(i)) - 1.0f, 0.0f, 0.0f};
                chain[i] = randomLight(r2, 0, pp, pp);
            }
        }
        structureChecks(chain, "N=200 geometric line");
        structureChecks(randomScene(r, 300, lo, hi, true, false), "N=300 all no-shadow", true);
        {
            LightTree t;
            check(!t.build(nullptr, 0) && !t.valid(), "N=0 does not build");
            const std::vector<PackedLight> one = randomScene(r, 1, lo, hi);
            check(t.build(one.data(), 1) && t.nodeCount() == 1 && t.depth() == 0 && isLeaf(t.nodes()[0]) &&
                      t.nodes()[0].parent == -1.0f && static_cast<u32>(t.nodes()[0].right + 0.5f) == 0u,
                  "N=1 is a single leaf at the root");
            f32 pp[3] = {0, 0, 100}, nn[3] = {0, 0, 1};
            u32 li; f32 pd;
            const f32 imp = LightTree::importance(t.nodes()[0], pp, nn);
            check(t.sample(pp, nn, 0.4f, li, pd) == (imp > 0.0f) && (imp <= 0.0f || (li == 0u && pd == 1.0f)),
                  "a one-light tree samples its light with probability 1 where it is lit");
            const std::vector<PackedLight> tooMany = randomScene(r, kMaxTreeLights + 1, lo, hi);
            check(!t.build(tooMany.data(), static_cast<u32>(tooMany.size())), "past kMaxTreeLights it refuses");
        }
    }

    // ---------------------------------------------------------------- canonical order and fill()
    {
        AVER_INFO("=== canonical order ===");
        Rng r(5);
        const f32 lo[3] = {-3000, -3000, 0}, hi[3] = {3000, 3000, 600};
        std::vector<PackedLight> L = randomScene(r, 500, lo, hi);
        L[10] = L[11];   // two bit-identical lights: interchangeable in the canonical order
        std::vector<PackedLight> P = L;
        for (u32 i = static_cast<u32>(P.size()) - 1; i > 0; --i) std::swap(P[i], P[r.next() % (i + 1)]);
        LightTree a, b;
        check(a.build(L.data(), 500) && b.build(P.data(), 500), "the same lights in two orders both build");
        check(a.key() == b.key(), "the set key does not depend on the order");
        bool modLeaves = a.nodeCount() == b.nodeCount();
        for (u32 i = 0; modLeaves && i < a.nodeCount(); ++i) {
            TreeNodeRec x = a.nodes()[i], y = b.nodes()[i];
            if (isLeaf(x)) x.right = y.right = 0.0f;
            modLeaves = sameBytes(x, y);
        }
        check(modLeaves, "the two builds' nodes are identical modulo the leaves' list indices");
        check(b.fill(L.data(), 500), "fill() accepts the same set in another order");
        bool same = a.nodeCount() == b.nodeCount();
        for (u32 i = 0; same && i < a.nodeCount(); ++i) same = sameBytes(a.nodes()[i], b.nodes()[i]);
        check(same, "after fill() the nodes equal a fresh build over that order, byte for byte");
        bool map = true;
        for (u32 i = 0; i < 500; ++i) map = map && a.leafOfList(i) == b.leafOfList(i);
        check(map, "and so does the leaf map");
        std::vector<PackedLight> changed = L;
        changed[77].radianceRange[0] *= 1.5f;
        check(LightTree::keyOf(changed.data(), 500) != a.key(), "a changed light changes the key");
        check(!b.fill(changed.data(), 500), "fill() refuses a changed set");
        check(!b.fill(L.data(), 499), "fill() refuses a different count");
    }

    // ---------------------------------------------------------------- packing
    {
        AVER_INFO("=== packing ===");
        check(sizeof(TreeNodeRec) == 80 && sizeof(PackedLight) == 80, "a node is one 80-byte light record");
        TreeHeader th;
        th.nodeCount = 7; th.nodeBaseRec = 123; th.leafMapRec = 456; th.samples = 2; th.candidates = 8;
        th.hitCandidates = 4; th.weightClamp = 3.0f;
        const PackedLight h = packTreeHeader(th);
        check(h.posRadius[0] == 0.0f && h.posRadius[1] == 7.0f && h.posRadius[2] == 123.0f && h.posRadius[3] == 456.0f &&
                  h.radianceRange[0] == 2.0f && h.radianceRange[1] == 8.0f && h.radianceRange[2] == 4.0f &&
                  h.radianceRange[3] == 3.0f && h.axisKind[0] == 1.0f,
              "the header record carries root, count, node base, leaf map, S, M, hit M, clamp, valid");
        std::string hlsl;
        const bool read = readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_lighttree.hlsli", hlsl);
        check(read, "voxi_lighttree.hlsli is readable");
        auto define = [&](const char* name) -> i64 {
            const std::string key = std::string("#define ") + name;
            const size_t at = hlsl.find(key);
            if (at == std::string::npos) return -1;
            size_t b = at + key.size();
            while (b < hlsl.size() && (hlsl[b] == ' ' || hlsl[b] == '\t')) ++b;
            return std::strtoll(hlsl.c_str() + b, nullptr, 10);
        };
        check(define("AVER_TREE_OFF_BMIN") == static_cast<i64>(offsetof(TreeNodeRec, bmin) / 4) &&
                  define("AVER_TREE_OFF_PHI") == static_cast<i64>(offsetof(TreeNodeRec, phi) / 4) &&
                  define("AVER_TREE_OFF_BMAX") == static_cast<i64>(offsetof(TreeNodeRec, bmax) / 4) &&
                  define("AVER_TREE_OFF_RANGE") == static_cast<i64>(offsetof(TreeNodeRec, range) / 4) &&
                  define("AVER_TREE_OFF_AXIS") == static_cast<i64>(offsetof(TreeNodeRec, axis) / 4) &&
                  define("AVER_TREE_OFF_COSO") == static_cast<i64>(offsetof(TreeNodeRec, cosThetaO) / 4) &&
                  define("AVER_TREE_OFF_COSE") == static_cast<i64>(offsetof(TreeNodeRec, cosThetaE) / 4) &&
                  define("AVER_TREE_OFF_LEFT") == static_cast<i64>(offsetof(TreeNodeRec, left) / 4) &&
                  define("AVER_TREE_OFF_RIGHT") == static_cast<i64>(offsetof(TreeNodeRec, right) / 4) &&
                  define("AVER_TREE_OFF_PARENT") == static_cast<i64>(offsetof(TreeNodeRec, parent) / 4) &&
                  define("AVER_TREE_OFF_MAXR") == static_cast<i64>(offsetof(TreeNodeRec, maxRadius) / 4) &&
                  define("AVER_TREE_OFF_FLAGS") == static_cast<i64>(offsetof(TreeNodeRec, flags) / 4),
              "the HLSL record offsets match the host struct (floats 0 to 17)");
        check(define("AVER_TREE_F_LEAF") == kTreeFlagLeaf && define("AVER_TREE_F_NOSHADOW") == kTreeFlagNoShadow &&
                  define("AVER_TREE_F_RECT") == kTreeFlagRect && define("AVER_TREE_MAX_DEPTH") == kLightTreeMaxDepth,
              "the HLSL flag bits and the depth bound match the header");
        const std::string vox = [&] { std::string s; readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_rt.hlsli", s); return s; }();
        check(vox.find("#include \"voxi_lighttree.hlsli\"") != std::string::npos && vox.find("bool rtManyLights()") != std::string::npos,
              "voxi_rt.hlsli includes the tree and exposes rtManyLights()");
        std::string vhlsl;
        readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi.hlsl", vhlsl);
        check(vhlsl.find("void CSRdManyLights(") != std::string::npos && vhlsl.find("gRdManyDiff    : register(u25)") != std::string::npos &&
                  vhlsl.find("gRdManySpec    : register(u26)") != std::string::npos,
              "voxi.hlsl has CSRdManyLights and the two UAVs at u25 and u26 (kManyLightDiff/SpecUavSlot)");
    }

    // ---------------------------------------------------------------- the bound
    {
        AVER_INFO("=== the importance bound ===");
        Rng r(21);
        const f32 lo[3] = {-2500, -2500, 0}, hi[3] = {2500, 2500, 500};
        for (const bool mixed : {false, true}) {
            const std::vector<PackedLight> L = randomScene(r, 220, lo, hi, mixed);
            LightTree t;
            t.build(L.data(), 220);
            std::vector<std::vector<u32>> under(t.nodeCount());
            for (u32 i = 0; i < t.nodeCount(); ++i) leavesUnder(t, i, under[i]);
            u32 violations = 0, falseZero = 0, leafMismatch = 0, nonzero = 0, strictInner = 0;
            f32 worst = 0.0f;
            for (u32 trial = 0; trial < 60000; ++trial) {
                const u32 node = r.next() % t.nodeCount();
                f32 p[3] = {r.range(-3200, 3200), r.range(-3200, 3200), r.range(-50, 600)}, n[3];
                unitVec(r, n);
                // keep clear of light centres: below 1 cm the bound is regularised, by design
                bool close = false;
                for (const u32 li : under[node]) {
                    const f32 dx = L[li].posRadius[0] - p[0], dy = L[li].posRadius[1] - p[1], dz = L[li].posRadius[2] - p[2];
                    if (dx * dx + dy * dy + dz * dz < 4.0f) close = true;
                }
                if (close) continue;
                const TreeNodeRec& nd = t.nodes()[node];
                const f32 I = LightTree::importance(nd, p, n);
                const f32 S = sumCheap(L, under[node], p, n);
                // Where the point is at least the box's half-diagonal outside it, the bound is strict. Closer in, the
                // distance term is floored at the half-diagonal (a regularisation: only the variance moves); the zeros and
                // the gating stay strict everywhere.
                f32 dmin2 = 0.0f, ext2 = 0.0f;
                for (int a = 0; a < 3; ++a) {
                    const f32 q = std::max(std::max(nd.bmin[a] - p[a], p[a] - nd.bmax[a]), 0.0f);
                    dmin2 += q * q;
                    ext2 += (nd.bmax[a] - nd.bmin[a]) * (nd.bmax[a] - nd.bmin[a]);
                }
                const bool strict = under[node].size() == 1 || dmin2 >= 0.25f * ext2;
                if (!std::isfinite(I)) ++violations;
                if (strict && I < S * (1.0f - 1e-4f) - 1e-6f) { ++violations; worst = std::max(worst, (S - I) / std::max(S, 1e-9f)); }
                if (I == 0.0f && S > 0.0f) ++falseZero;
                if (I > 0.0f) ++nonzero;
                if (strict && under[node].size() > 1) ++strictInner;
                // A leaf IS its light's cheap irradiance, except a rect larger than 1 m^2, whose power carries its area.
                const bool bigRect = under[node].size() == 1 && ((static_cast<u32>(L[under[node][0]].axisKind[3] + 0.5f) & 7u) == kLightKindRect) &&
                                     4.0f * L[under[node][0]].shape[0] * L[under[node][0]].shape[1] > 1e4f;
                if (under[node].size() == 1 && !bigRect) {
                    const f32 e = std::fabs(I - S);
                    if (e > 1e-4f * std::max(S, 1e-6f) + 1e-6f) ++leafMismatch;
                }
            }
            const std::string tag = mixed ? "mixed kinds" : "point lights";
            check(violations == 0, tag + ": I(node) >= the leaves' cheap luminance (strict outside the half-diagonal), 6e4 random (point, normal, node) (worst shortfall " +
                                       std::to_string(worst) + ")");
            check(falseZero == 0, tag + ": zero only where every leaf is zero");
            check(leafMismatch == 0, tag + ": a leaf's importance IS its light's cheap irradiance");
            check(nonzero > 1000 && strictInner > 1000, tag + ": the sample exercised the nonzero branch and strict inner nodes");
        }
    }

    // ---------------------------------------------------------------- the descent's probabilities
    {
        AVER_INFO("=== sampling probabilities ===");
        Rng r(33);
        const f32 lo[3] = {-1500, -1500, 150}, hi[3] = {1500, 1500, 400};
        // Equal ranges, omni lights, every light above the point and the normal up: no descent can die (a node is lit
        // only if a child is, and every light has a positive cosine), so the leaf probabilities sum to exactly one.
        std::vector<PackedLight> L(150);
        for (auto& l : L) { l = randomLight(r, 0, lo, hi); l.radianceRange[3] = 2500.0f; }
        LightTree t;
        t.build(L.data(), 150);
        bool sumOk = true, pathOk = true, massOk = true;
        u32 lit = 0, nearOne = 0;
        for (u32 trial = 0; trial < 400; ++trial) {
            f32 p[3] = {r.range(-1500, 1500), r.range(-1500, 1500), r.range(-100, 50)};
            const f32 up[3] = {0, 0, 1};
            f32 total = 0.0f;
            for (u32 i = 0; i < 150; ++i) total += t.pdfOf(static_cast<u32>(t.leafOfList(i)), p, up);
            // A box of centres is not its children's union, so near the range edge a node can be lit while both children
            // are not; that mass is lost, and small. The sum is the reaching mass, and nearly 1.
            if (total > 0.0f) {
                ++lit;
                if (std::fabs(total - reachMass(t, 0, p, up)) > 1e-5f || total > 1.00002f) sumOk = false;
                if (total > 0.9f) ++nearOne;
            }
            f32 n[3];
            unitVec(r, n);
            f32 total2 = 0.0f;
            for (u32 i = 0; i < 150; ++i) total2 += t.pdfOf(static_cast<u32>(t.leafOfList(i)), p, n);
            if (std::fabs(reachMass(t, 0, p, n) - total2) > 1e-5f) massOk = false;
        }
        check(sumOk && lit > 200 && nearOne > lit / 2,
              "equal-range omni lights above the point: the leaves' probabilities sum to the reaching mass (at most 1, mostly near it)");
        check(massOk, "the sum of pdfOf equals the independent recursion's reaching mass (random normals)");
        // Mixed kinds and ranges: a draw may die at a node whose children are both zero, so the sum is the reaching mass.
        const std::vector<PackedLight> M = randomScene(r, 300, lo, hi, true);
        LightTree tm;
        tm.build(M.data(), 300);
        bool mixedMass = true;
        u32 compared = 0;
        for (u32 trial = 0; trial < 300; ++trial) {
            f32 p[3] = {r.range(-1500, 1500), r.range(-1500, 1500), r.range(0, 400)}, n[3];
            unitVec(r, n);
            f32 total = 0.0f;
            for (u32 i = 0; i < 300; ++i) total += tm.pdfOf(static_cast<u32>(tm.leafOfList(i)), p, n);
            const f32 mass = reachMass(tm, 0, p, n);
            if (std::fabs(total - mass) > 1e-4f) mixedMass = false;
            if (total > 0.0f) ++compared;
            // sample() returns pdfOf of the light it returns
            for (u32 k = 0; k < 20; ++k) {
                u32 li; f32 pd;
                if (!tm.sample(p, n, r.u01(), li, pd)) continue;
                const f32 want = tm.pdfOf(static_cast<u32>(tm.leafOfList(li)), p, n);
                if (std::fabs(pd - want) > 1e-4f * std::max(want, 1e-6f)) pathOk = false;
            }
        }
        check(mixedMass && compared > 100, "mixed kinds: the probabilities sum to the reaching mass");
        check(pathOk, "sample()'s probability equals pdfOf's walk up from the leaf");
        // A histogram of sample() against pdfOf for one point
        {
            f32 p[3] = {0, 0, 50}, n[3] = {0, 0, 1};   // every light is above this point and in range
            std::vector<u32> hist(150, 0);
            u32 hits = 0;
            const u32 draws = 400000;
            for (u32 k = 0; k < draws; ++k) {
                u32 li; f32 pd;
                if (t.sample(p, n, r.u01(), li, pd)) { ++hist[li]; ++hits; }
            }
            f32 worst = 0.0f;
            for (u32 i = 0; i < 150; ++i) {
                const f32 q = t.pdfOf(static_cast<u32>(t.leafOfList(i)), p, n);
                if (q < 0.03f) continue;
                worst = std::max(worst, std::fabs(static_cast<f32>(hist[i]) / draws - q) / q);
            }
            check(hits == draws && worst < 0.05f, "a histogram of 4e5 draws follows pdfOf (worst relative gap " + std::to_string(worst) + ")");
        }
    }

    unbiasednessChecks();

    // ---------------------------------------------------------------- degenerate
    {
        AVER_INFO("=== degenerate inputs ===");
        Rng r(41);
        const f32 lo[3] = {-500, -500, 0}, hi[3] = {500, 500, 300};
        std::vector<PackedLight> L = randomScene(r, 20, lo, hi);
        LightTree t;
        L[5].radianceRange[1] = std::numeric_limits<f32>::quiet_NaN();
        check(!t.build(L.data(), 20) && !t.valid(), "a NaN power refuses the tree");
        L[5] = randomLight(r, 0, lo, hi);
        L[5].radianceRange[0] = -1.0f; L[5].radianceRange[1] = L[5].radianceRange[2] = 0.0f;
        check(!t.build(L.data(), 20), "a negative power refuses the tree");
        L[5] = randomLight(r, 0, lo, hi);
        L[7].radianceRange[3] = std::numeric_limits<f32>::infinity();
        check(!t.build(L.data(), 20), "an infinite range refuses the tree");
        L[7] = randomLight(r, 0, lo, hi);
        PackedLight sun{};
        sun.axisKind[3] = static_cast<f32>(kLightKindDirectional);
        std::vector<PackedLight> withSun = L;
        withSun.push_back(sun);
        check(!t.build(withSun.data(), 21), "a directional light is not a tree light");
        check(t.build(L.data(), 20), "a clean set builds again after refusals");
        // a point inside a node's box is finite
        const TreeNodeRec& root = t.nodes()[0];
        f32 inside[3] = {0.5f * (root.bmin[0] + root.bmax[0]), 0.5f * (root.bmin[1] + root.bmax[1]), 0.5f * (root.bmin[2] + root.bmax[2])};
        f32 nn[3] = {0, 0, 1};
        const f32 Iin = LightTree::importance(root, inside, nn);
        check(std::isfinite(Iin) && Iin >= 0.0f, "a point inside the root's box gives a finite importance");
        // out of every range: all zero, no NaN
        f32 far[3] = {1.0e7f, 1.0e7f, 1.0e7f};
        u32 li = 0; f32 pd = 1.0f;
        check(LightTree::importance(root, far, nn) == 0.0f && !t.sample(far, nn, 0.3f, li, pd) && pd == 0.0f &&
                  t.pdfOf(static_cast<u32>(t.leafOfList(0)), far, nn) == 0.0f,
              "out of every range: importance 0, sample() false, pdfOf 0, no NaN");
        // a point on top of a light, normal pointing away: finite
        f32 on[3] = {L[3].posRadius[0], L[3].posRadius[1], L[3].posRadius[2]};
        f32 down[3] = {0, 0, -1};
        bool finite = true;
        for (u32 i = 0; i < t.nodeCount(); ++i) finite = finite && std::isfinite(LightTree::importance(t.nodes()[i], on, down));
        check(finite, "at a light's own position every node's importance is finite");
    }

    if (g_failures == 0) {
        AVER_INFO("[LightTree] PASS: {} checks", g_checks);
        return 0;
    }
    AVER_ERROR("[LightTree] FAIL: {} of {} checks failed", g_failures, g_checks);
    return 1;
}
