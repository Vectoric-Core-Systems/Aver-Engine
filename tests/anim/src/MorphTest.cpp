// The CPU blend-shape reference: applyMorphTargets against a brute-force dense computation, the bounds
// allowance against a randomised skinned mesh, and the active-list selection. No GPU. The GPU pass is
// checked against these same functions (--skin-test), so a mistake here is a mistake everywhere.
#include "aver/anim/Morph.hpp"
#include "aver/anim/Pose.hpp"
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

// Deterministic generator, so a failure reproduces.
struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed) {}
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    f32 unit() { return f32(next() & 0xFFFF) / 65535.0f; }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    u32 below(u32 n) { return next() % n; }
};

struct Mesh {
    std::vector<f32> pos, nrm;
    std::vector<fmt::OcMorphTarget> targets;
    u32 vc() const { return static_cast<u32>(pos.size() / 3); }
};

static Mesh randomMesh(Rng& r, u32 vc, u32 targetCount, bool someWithoutNormals) {
    Mesh m;
    for (u32 v = 0; v < vc; ++v) {
        m.pos.insert(m.pos.end(), {r.range(-50, 50), r.range(-50, 50), r.range(-50, 50)});
        Vec3 n{r.range(-1, 1), r.range(-1, 1), r.range(0.1f, 1)};
        n = n.getSafeNormal();
        m.nrm.insert(m.nrm.end(), {n.x, n.y, n.z});
    }
    for (u32 t = 0; t < targetCount; ++t) {
        fmt::OcMorphTarget tg;
        tg.name = "t" + std::to_string(t);
        const bool withN = !(someWithoutNormals && t % 3 == 2);
        for (u32 v = 0; v < vc; ++v) {
            if (r.unit() > 0.3f) continue;       // sparse
            tg.vertices.push_back(v);
            tg.positionDeltas.insert(tg.positionDeltas.end(), {r.range(-9, 9), r.range(-9, 9), r.range(-9, 9)});
            if (withN) tg.normalDeltas.insert(tg.normalDeltas.end(), {r.range(-.5f, .5f), r.range(-.5f, .5f), r.range(-.5f, .5f)});
        }
        tg.finalize();
        m.targets.push_back(std::move(tg));
    }
    return m;
}

// The dense reference: expand every target to V entries and sum.
static void dense(const Mesh& m, const std::vector<anim::MorphTargetWeight>& w, std::vector<f32>& p, std::vector<f32>& n) {
    p = m.pos;
    n = m.nrm;
    for (const anim::MorphTargetWeight& e : w) {
        const fmt::OcMorphTarget& t = m.targets[e.target];
        std::vector<f32> dp(m.pos.size(), 0.0f), dn(m.pos.size(), 0.0f);
        for (usize k = 0; k < t.vertices.size(); ++k)
            for (int c = 0; c < 3; ++c) {
                dp[t.vertices[k] * 3 + c] = t.positionDeltas[k * 3 + c];
                if (!t.normalDeltas.empty()) dn[t.vertices[k] * 3 + c] = t.normalDeltas[k * 3 + c];
            }
        for (usize i = 0; i < p.size(); ++i) { p[i] += e.weight * dp[i]; n[i] += e.weight * dn[i]; }
    }
    for (usize v = 0; v < n.size() / 3; ++v) {
        const f32 l = std::sqrt(n[v*3]*n[v*3] + n[v*3+1]*n[v*3+1] + n[v*3+2]*n[v*3+2]);
        if (l > 1e-8f) { n[v*3] /= l; n[v*3+1] /= l; n[v*3+2] /= l; }
    }
}

static void testApply() {
    AVER_INFO("=== applyMorphTargets vs a dense reference ===");
    Rng r(7);
    const Mesh m = randomMesh(r, 301, 6, true);
    const std::vector<std::vector<anim::MorphTargetWeight>> cases = {
        {{0, 1.0f}},
        {{1, 0.5f}, {3, -0.75f}},
        {{0, 2.5f}, {2, 1.0f}, {4, -1.5f}, {5, 0.1f}},   // above 1, negative, a target without normals (index 2, 5)
        {{2, 0.0f}},                                       // weight 0
    };
    for (usize c = 0; c < cases.size(); ++c) {
        std::vector<f32> gp, gn, wp, wn;
        anim::applyMorphTargets(m.targets, cases[c].data(), static_cast<u32>(cases[c].size()), m.pos, m.nrm, gp, gn);
        dense(m, cases[c], wp, wn);
        f32 worstP = 0, worstN = 0;
        for (usize i = 0; i < gp.size(); ++i) { worstP = std::fmax(worstP, std::fabs(gp[i] - wp[i])); worstN = std::fmax(worstN, std::fabs(gn[i] - wn[i])); }
        check(gp.size() == m.pos.size(), "case " + std::to_string(c) + ": outputs are sized");
        check(worstP < 1e-3f && worstN < 1e-5f, "case " + std::to_string(c) + ": positions and normals match the dense sum (" +
                                                 std::to_string(worstP) + ", " + std::to_string(worstN) + ")");
    }
    std::vector<f32> p, n;
    anim::applyMorphTargets(m.targets, nullptr, 0, m.pos, m.nrm, p, n);
    check(p == m.pos && n == m.nrm, "an empty active list is the identity (normals untouched too)");

    // Normal renormalisation: one vertex, a delta that doubles the normal.
    Mesh one;
    one.pos = {0, 0, 0};
    one.nrm = {0, 0, 1};
    fmt::OcMorphTarget t;
    t.name = "n";
    t.vertices = {0};
    t.positionDeltas = {0, 0, 0};
    t.normalDeltas = {0, 0, 3};
    t.finalize();
    one.targets = {t};
    const anim::MorphTargetWeight w{0, 1.0f};
    anim::applyMorphTargets(one.targets, &w, 1, one.pos, one.nrm, p, n);
    checkNear(n[2], 1.0f, 1e-6f, "a normal pushed to length 4 is renormalised");
    const anim::MorphTargetWeight bad{9, 1.0f};
    anim::applyMorphTargets(one.targets, &bad, 1, one.pos, one.nrm, p, n);
    check(p.empty() && n.empty(), "an invalid target index yields empty outputs");
}

static void testBounds() {
    AVER_INFO("=== morphBoundsPad + posedBounds contain every posed, morphed vertex ===");
    Rng r(1234);
    const u32 vc = 400, bones = 4;
    Mesh m = randomMesh(r, vc, 5, false);
    std::vector<u16> joints;
    std::vector<f32> weights;
    for (u32 v = 0; v < vc; ++v) {
        f32 sum = 0;
        f32 w4[4];
        for (int k = 0; k < 4; ++k) { w4[k] = r.unit(); sum += w4[k]; }
        for (int k = 0; k < 4; ++k) {
            joints.push_back(static_cast<u16>(r.below(bones)));
            weights.push_back(w4[k] / sum * (v % 7 == 0 ? 0.6f : 1.0f));   // a few vertices do not sum to one
        }
    }
    // Skinning matrices: random rigid-ish transforms.
    std::vector<Mat4> skin(bones);
    for (u32 b = 0; b < bones; ++b) {
        Transform t;
        t.position = Vec3{r.range(-30, 30), r.range(-30, 30), r.range(-30, 30)};
        t.rotation = Quat::fromAxisAngle(Vec3{r.range(-1, 1), r.range(-1, 1), 1.0f}, r.range(0, 6.28f));
        t.scale = Vec3{r.range(0.7f, 1.4f), r.range(0.7f, 1.4f), r.range(0.7f, 1.4f)};
        skin[b] = t.toMatrix();
    }
    std::vector<Vec3> bmin, bmax;
    std::vector<u8> used;
    anim::boneRestBounds(m.pos, joints, weights, bones, 0.0f, bmin, bmax, used);
    Vec3 rmin{1e9f, 1e9f, 1e9f}, rmax{-1e9f, -1e9f, -1e9f};
    for (u32 v = 0; v < vc; ++v) {
        rmin = Vec3{std::fmin(rmin.x, m.pos[v*3]), std::fmin(rmin.y, m.pos[v*3+1]), std::fmin(rmin.z, m.pos[v*3+2])};
        rmax = Vec3{std::fmax(rmax.x, m.pos[v*3]), std::fmax(rmax.y, m.pos[v*3+1]), std::fmax(rmax.z, m.pos[v*3+2])};
    }

    for (int trial = 0; trial < 8; ++trial) {
        std::vector<anim::MorphTargetWeight> w;
        for (u32 t = 0; t < 5; ++t) w.push_back({t, r.range(-1.5f, 1.5f)});
        std::vector<f32> mp, mn, sp, sn;
        anim::applyMorphTargets(m.targets, w.data(), static_cast<u32>(w.size()), m.pos, m.nrm, mp, mn);
        anim::skinVertices(skin, mp, mn, joints, weights, sp, sn);
        const Vec3 pad = anim::morphBoundsPad(m.targets, w.data(), static_cast<u32>(w.size()));
        Vec3 lo, hi;
        anim::posedBounds(bmin, bmax, used, skin.data(), bones, rmin, rmax, lo, hi, pad);
        bool inside = true;
        for (u32 v = 0; v < vc && inside; ++v) {
            const f32 eps = 1e-2f;
            inside = sp[v*3] >= lo.x - eps && sp[v*3] <= hi.x + eps && sp[v*3+1] >= lo.y - eps && sp[v*3+1] <= hi.y + eps &&
                     sp[v*3+2] >= lo.z - eps && sp[v*3+2] <= hi.z + eps;
        }
        check(inside, "trial " + std::to_string(trial) + ": every posed + morphed vertex is inside the padded bounds");
    }
    // The pad is zero for no weights and for zero weights, and grows with |w|.
    const anim::MorphTargetWeight zero{0, 0.0f};
    const Vec3 p0 = anim::morphBoundsPad(m.targets, &zero, 1);
    check(p0.x == 0.0f && p0.y == 0.0f && p0.z == 0.0f, "a zero weight pads nothing");
    const anim::MorphTargetWeight neg{1, -2.0f}, pos{1, 2.0f};
    const Vec3 a = anim::morphBoundsPad(m.targets, &neg, 1), b = anim::morphBoundsPad(m.targets, &pos, 1);
    check(a.x == b.x && a.y == b.y && a.z == b.z && a.x > 0.0f, "the pad uses |w|");

    // Unskinned: the rest box widened by the pad.
    Vec3 lo, hi;
    anim::posedBounds({}, {}, {}, nullptr, 0, rmin, rmax, lo, hi, Vec3{1, 2, 3});
    check(lo.x == rmin.x - 1 && hi.z == rmax.z + 3, "with no skin the rest extent is widened by the pad");
}

static void testActive() {
    AVER_INFO("=== selectActiveTargets ===");
    std::vector<f32> maxLen(100, 1.0f);
    std::vector<anim::MorphTargetWeight> in;
    for (u32 t = 0; t < 100; ++t) in.push_back({t, 0.01f + 0.01f * f32(t)});     // importance rises with the index
    maxLen[5] = 0.0f;                                                             // a target that moves nothing
    in.push_back({200, 1.0f});                                                    // out of range
    in.push_back({3, 1e-5f});                                                     // under the eps (repeat of target 3: ignored)
    anim::MorphTargetWeight out[anim::kMorphMaxActive];
    const u32 n = anim::selectActiveTargets(maxLen.data(), 100, in.data(), static_cast<u32>(in.size()), out);
    check(n == anim::kMorphMaxActive, "capped at 64");
    check(out[0].target == 36 && out[n - 1].target == 99, "the 64 strongest by |w| * maxLen are kept, in ascending target order");
    bool ascending = true;
    for (u32 i = 1; i < n; ++i) if (out[i].target <= out[i - 1].target) ascending = false;
    check(ascending, "ascending order");

    std::vector<anim::MorphTargetWeight> few = {{4, 0.5f}, {2, 1e-5f}, {7, -0.25f}, {5, 1.0f}, {4, 0.75f}, {99, 1.0f}};
    const u32 m = anim::selectActiveTargets(maxLen.data(), 10, few.data(), static_cast<u32>(few.size()), out);
    check(m == 2 && out[0].target == 4 && out[1].target == 7, "under-threshold, zero-length and out-of-range entries are dropped");
    checkNear(out[0].weight, 0.75f, 1e-6f, "a repeated target keeps the later weight");
    checkNear(out[1].weight, -0.25f, 1e-6f, "negative weights are kept");
}

int main() {
    testApply();
    testBounds();
    testActive();
    if (g_failures == 0) AVER_INFO("=== all morph tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
