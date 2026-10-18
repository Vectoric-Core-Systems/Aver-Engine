// The blend-shape GPU layout, with no device: rank words and prefix counts, delta bases, the active list, the
// 128 MiB refusal, and evalMorphLayout (the C++ twin of skin.hlsl's lookup loop) against anim::applyMorphTargets.
#include "aver/anim/Morph.hpp"
#include "aver/core/Log.hpp"
#include "aver/render/MorphLayout.hpp"

#include <bit>
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

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed) {}
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    f32 unit() { return f32(next() & 0xFFFF) / 65535.0f; }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

static fmt::OcMorphTarget target(const char* name, std::vector<u32> verts, bool normals, f32 seed = 1.0f) {
    fmt::OcMorphTarget t;
    t.name = name;
    t.vertices = std::move(verts);
    for (usize k = 0; k < t.vertices.size(); ++k) {
        t.positionDeltas.insert(t.positionDeltas.end(), {seed + f32(k), seed * 2 + f32(k), seed * 3 + f32(k)});
        if (normals) t.normalDeltas.insert(t.normalDeltas.end(), {0.01f * f32(k + 1), 0.0f, -0.01f * f32(k + 1)});
    }
    t.finalize();
    return t;
}

constexpr u32 kV = 501;   // not a multiple of 32 or 64

static void testRank() {
    AVER_INFO("=== rank words and prefix counts ===");
    std::vector<u32> full;
    for (u32 v = 0; v < 32; ++v) full.push_back(v);       // exactly one full word
    full.push_back(500);                                   // and the last vertex
    std::vector<fmt::OcMorphTarget> ts = {
        target("edges", {0, 31, 32, 63, 64, 500}, true, 1.0f),
        target("full", full, false, 5.0f),
        target("empty", {}, false),
        target("last", {500}, true, 9.0f),
    };
    render::MorphLayout L;
    std::string why;
    check(render::buildMorphLayout(ts, kV, L, &why), "builds: " + why);
    check(L.vertexCount == kV && L.targetCount == 4 && L.wordsPerTarget == 16, "501 vertices -> 16 words per target, 4 targets");
    check(L.rank.size() == 4 * 16 * 2, "two u32 per word");
    // target 0
    const u32* r0 = &L.rank[0];
    check(r0[0 * 2] == ((1u << 0) | (1u << 31)) && r0[0 * 2 + 1] == 0, "word 0: vertices 0 and 31, prefix 0");
    check(r0[1 * 2] == ((1u << 0) | (1u << 31)) && r0[1 * 2 + 1] == 2, "word 1: vertices 32 and 63, prefix 2");
    check(r0[2 * 2] == 1u && r0[2 * 2 + 1] == 4, "word 2: vertex 64, prefix 4");
    check(r0[15 * 2] == (1u << (500 & 31)) && r0[15 * 2 + 1] == 5, "word 15: vertex 500 (bit 20), prefix 5");
    check(r0[3 * 2] == 0 && r0[3 * 2 + 1] == 5, "an empty word still carries the running prefix");
    // target 1: a full word
    const u32* r1 = &L.rank[16 * 2];
    check(r1[0] == 0xFFFFFFFFu && r1[1] == 0, "a full word is all ones");
    check(r1[2] == 0 && r1[3] == 32, "the next word's prefix is 32");
    // target 2: empty
    bool allZero = true;
    for (u32 w = 0; w < 32; ++w) if (L.rank[usize(2 * 16 * 2) + w] != 0) allZero = false;
    check(allZero && L.entryCount[2] == 0, "an empty target has an empty bitmap");

    check(L.entryCount[0] == 6 && L.entryCount[1] == 33 && L.entryCount[2] == 0 && L.entryCount[3] == 1, "entry counts");
    check(L.deltaBase[0] == 0 && L.deltaBase[1] == 6 && L.deltaBase[2] == 39 && L.deltaBase[3] == 39, "delta bases are prefix sums");
    check(L.totalEntries() == 40 && L.delta.size() == 40 * 6, "six floats per entry");
    // delta layout: dp then dn, dn zero without normals
    checkNear(L.delta[0], 1.0f, 1e-6f, "first entry dp.x");
    checkNear(L.delta[3], 0.01f, 1e-6f, "first entry dn.x");
    checkNear(L.delta[5], -0.01f, 1e-6f, "first entry dn.z");
    const usize e1 = usize(L.deltaBase[1]) * 6;
    check(L.delta[e1 + 3] == 0.0f && L.delta[e1 + 4] == 0.0f && L.delta[e1 + 5] == 0.0f, "a target without normals has zero dn");
    checkNear(L.maxLen[3], std::sqrt(9.0f * 9.0f + 18.0f * 18.0f + 27.0f * 27.0f), 1e-3f, "maxLen is the longest position delta");

    // the lookup finds each entry: popcount below the bit plus the prefix plus the base
    render::MorphActiveRecord rec{0, 0, 1.0f, 0};
    Vec3 dp, dn;
    render::evalMorphLayout(L, &rec, 1, 64, dp, dn);
    checkNear(dp.x, 1.0f + 4.0f, 1e-5f, "vertex 64 is entry 4 of target 0");
    render::evalMorphLayout(L, &rec, 1, 65, dp, dn);
    check(dp.x == 0.0f && dp.y == 0.0f && dp.z == 0.0f, "a vertex outside the target adds nothing");
}

static void testEval() {
    AVER_INFO("=== evalMorphLayout equals applyMorphTargets ===");
    Rng r(99);
    for (int trial = 0; trial < 6; ++trial) {
        const u32 vc = 130 + trial * 97;
        std::vector<f32> pos, nrm;
        for (u32 v = 0; v < vc; ++v) {
            pos.insert(pos.end(), {r.range(-50, 50), r.range(-50, 50), r.range(-50, 50)});
            Vec3 n = Vec3{r.range(-1, 1), r.range(-1, 1), r.range(0.2f, 1)}.getSafeNormal();
            nrm.insert(nrm.end(), {n.x, n.y, n.z});
        }
        std::vector<fmt::OcMorphTarget> ts;
        for (u32 t = 0; t < 10; ++t) {
            fmt::OcMorphTarget tg;
            tg.name = "t" + std::to_string(t);
            for (u32 v = 0; v < vc; ++v) {
                if (r.unit() > 0.25f) continue;
                tg.vertices.push_back(v);
                tg.positionDeltas.insert(tg.positionDeltas.end(), {r.range(-9, 9), r.range(-9, 9), r.range(-9, 9)});
                if (t % 4 != 3) tg.normalDeltas.insert(tg.normalDeltas.end(), {r.range(-.4f, .4f), r.range(-.4f, .4f), r.range(-.4f, .4f)});
            }
            tg.finalize();
            ts.push_back(std::move(tg));
        }
        render::MorphLayout L;
        std::string why;
        if (!render::buildMorphLayout(ts, vc, L, &why)) { check(false, "layout: " + why); continue; }

        std::vector<anim::MorphTargetWeight> w;
        for (u32 t = 0; t < 10; ++t) w.push_back({t, r.range(-1.5f, 2.0f)});
        std::vector<f32> refP, refN;
        anim::applyMorphTargets(ts, w.data(), static_cast<u32>(w.size()), pos, nrm, refP, refN);

        render::MorphActiveRecord rec[render::kMorphMaxActive];
        const u32 n = render::buildActiveList(L, w.data(), static_cast<u32>(w.size()), rec);
        f32 worstP = 0, worstN = 0;
        for (u32 v = 0; v < vc; ++v) {
            Vec3 dp, dn;
            render::evalMorphLayout(L, rec, n, v, dp, dn);
            Vec3 p{pos[v * 3] + dp.x, pos[v * 3 + 1] + dp.y, pos[v * 3 + 2] + dp.z};
            Vec3 nn{nrm[v * 3] + dn.x, nrm[v * 3 + 1] + dn.y, nrm[v * 3 + 2] + dn.z};
            const f32 l = nn.size();
            if (l > 1e-8f) nn = nn / l;
            worstP = std::fmax(worstP, std::fabs(p.x - refP[v * 3]) + std::fabs(p.y - refP[v * 3 + 1]) + std::fabs(p.z - refP[v * 3 + 2]));
            worstN = std::fmax(worstN, std::fabs(nn.x - refN[v * 3]) + std::fabs(nn.y - refN[v * 3 + 1]) + std::fabs(nn.z - refN[v * 3 + 2]));
        }
        check(n == 10 && worstP < 1e-4f && worstN < 1e-5f,
              "trial " + std::to_string(trial) + " (" + std::to_string(vc) + " vertices): twin matches the reference (" +
              std::to_string(worstP) + ", " + std::to_string(worstN) + ")");
    }
}

static void testActiveList() {
    AVER_INFO("=== buildActiveList ===");
    std::vector<fmt::OcMorphTarget> ts;
    for (u32 t = 0; t < 100; ++t) ts.push_back(target(("t" + std::to_string(t)).c_str(), {t, t + 100}, false, 1.0f));
    ts[7] = target("flat", {}, false);                       // moves nothing
    render::MorphLayout L;
    check(render::buildMorphLayout(ts, 300, L), "builds");
    std::vector<anim::MorphTargetWeight> in;
    for (u32 t = 0; t < 100; ++t) in.push_back({t, 0.01f + 0.01f * f32(t)});
    in.push_back({3, 0.00001f});           // under the threshold
    in.push_back({500, 1.0f});             // out of range
    render::MorphActiveRecord out[render::kMorphMaxActive];
    const u32 n = render::buildActiveList(L, in.data(), static_cast<u32>(in.size()), out);
    check(n == render::kMorphMaxActive, "capped at 64");
    check(out[0].rankBase == 36 * L.wordsPerTarget && out[0].deltaBase == L.deltaBase[36], "the strongest 64 survive, in target order: first is target 36");
    check(out[n - 1].rankBase == 99 * L.wordsPerTarget && out[n - 1].deltaBase == L.deltaBase[99], "and the last is target 99");
    checkNear(out[0].weight, 0.37f, 1e-5f, "the weight is carried");
    bool noFlat = true;
    for (u32 i = 0; i < n; ++i) if (out[i].rankBase == 7 * L.wordsPerTarget) noFlat = false;
    check(noFlat, "a target with no entries is never chosen");

    std::vector<anim::MorphTargetWeight> few = {{4, 0.5f}, {2, 1e-5f}, {9, -0.25f}};
    const u32 m = render::buildActiveList(L, few.data(), 3, out);
    check(m == 2 && out[0].weight == 0.5f && out[1].weight == -0.25f, "under-threshold weights are dropped, negatives kept");
    check(render::buildActiveList(L, nullptr, 0, out) == 0, "no weights, no records");
}

static void testLimit() {
    AVER_INFO("=== the 128 MiB limit ===");
    // 200 targets over 4,194,304 vertices: 200 x 131,072 words x 8 B = 200 MiB of rank data alone. The size is
    // checked before anything is allocated, so this costs nothing.
    std::vector<fmt::OcMorphTarget> ts;
    for (u32 t = 0; t < 200; ++t) ts.push_back(target(("big" + std::to_string(t)).c_str(), {0}, false));
    render::MorphLayout L;
    std::string why;
    check(!render::buildMorphLayout(ts, 4194304, L, &why) && !why.empty(), "refused: " + why);
    check(L.rank.empty() && L.delta.empty(), "and nothing was allocated");

    std::vector<fmt::OcMorphTarget> bad = {target("a", {5, 5}, false)};
    check(!render::buildMorphLayout(bad, 100, L, &why), "duplicate vertices are refused: " + why);
    bad = {target("a", {500}, false)};
    check(!render::buildMorphLayout(bad, 100, L, &why), "a vertex past the count is refused: " + why);
    check(!render::buildMorphLayout({}, 100, L, &why), "no targets is refused");
}

int main() {
    testRank();
    testEval();
    testActiveList();
    testLimit();
    if (g_failures == 0) AVER_INFO("=== all morph layout tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
