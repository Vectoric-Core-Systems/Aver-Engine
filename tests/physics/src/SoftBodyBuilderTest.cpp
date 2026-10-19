// The builder-style soft bodies (physics_softbody_abi.h): hanging, rebuilding with the same handle and
// the same state, dents written in place, the bend rule, tears, the skin palette with an entity matrix,
// wind and drag, impacts, layers, sleep and freeze. ABI only: links Aver.Physics and Aver.Core.
//
// Written against the header, with thresholds that name what is being decided rather than how close
// Jolt gets: a dent that survives against one that springs back, a drag coefficient that orders the
// deflections, a closing speed within 15 percent of sqrt(2 g h).
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_softbody_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_reset_layer_collisions();
    aver_phys_sb_set_wind(0, 0, 0, 0, 0, 0);
    aver_phys_init();
}

static void stepFor(float seconds) {
    const float fixed = aver_phys_fixed_step();
    for (float t = 0.0f; t < seconds; t += fixed) aver_phys_step(fixed);
}

// ---- sheets ----------------------------------------------------------------------------------------------

struct Sheet {
    int32_t cols = 0, rows = 0;
    std::vector<float> rest;            // 3 per vertex
    std::vector<float> invMass;
    std::vector<int32_t> faces;         // 3 per triangle
    std::vector<int32_t> edges;         // 2 per edge, derived
    std::vector<float> edgeRest;
    std::vector<int32_t> bends;         // 4 per bend, derived
    int32_t count() const { return static_cast<int32_t>(rest.size() / 3); }
    int32_t at(int32_t c, int32_t r) const { return r * cols + c; }
    float x(int32_t v) const { return rest[static_cast<size_t>(v) * 3]; }
};

// Unique edges from the faces, and a bend for every edge two triangles share.
static void deriveTopology(Sheet& s) {
    s.edges.clear(); s.edgeRest.clear(); s.bends.clear();
    std::map<std::pair<int32_t, int32_t>, std::vector<int32_t>> opposite;
    for (size_t t = 0; t + 2 < s.faces.size(); t += 3)
        for (size_t k = 0; k < 3; ++k) {
            const int32_t a = s.faces[t + k], b = s.faces[t + (k + 1) % 3], c = s.faces[t + (k + 2) % 3];
            opposite[{std::min(a, b), std::max(a, b)}].push_back(c);
        }
    for (const auto& [e, opp] : opposite) {
        s.edges.push_back(e.first); s.edges.push_back(e.second);
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = s.rest[static_cast<size_t>(e.first) * 3 + static_cast<size_t>(k)] - s.rest[static_cast<size_t>(e.second) * 3 + static_cast<size_t>(k)];
            d2 += d * d;
        }
        s.edgeRest.push_back(std::sqrt(d2));
        if (opp.size() == 2) {
            s.bends.push_back(e.first); s.bends.push_back(e.second); s.bends.push_back(opp[0]); s.bends.push_back(opp[1]);
        }
    }
}

// plane 0: XZ (vertical, wind along Y hits it), 1: YZ (vertical, wind along X hits it), 2: XY (horizontal).
// Row 0 is the top (vertical) or the near edge (horizontal).
static Sheet makeSheet(int32_t cols, int32_t rows, float spacing, int plane, float ox, float oy, float oz) {
    Sheet s;
    s.cols = cols; s.rows = rows;
    for (int32_t r = 0; r < rows; ++r)
        for (int32_t c = 0; c < cols; ++c) {
            const float u = static_cast<float>(c) * spacing, v = static_cast<float>(r) * spacing;
            if (plane == 0)      { s.rest.push_back(ox + u); s.rest.push_back(oy);     s.rest.push_back(oz - v); }
            else if (plane == 1) { s.rest.push_back(ox);     s.rest.push_back(oy + u); s.rest.push_back(oz - v); }
            else                 { s.rest.push_back(ox + u); s.rest.push_back(oy + v); s.rest.push_back(oz); }
            s.invMass.push_back(1.0f);
        }
    for (int32_t r = 0; r + 1 < rows; ++r)
        for (int32_t c = 0; c + 1 < cols; ++c) {
            const int32_t a = s.at(c, r), b = s.at(c + 1, r), cc = s.at(c + 1, r + 1), d = s.at(c, r + 1);
            for (const int32_t i : {a, b, cc, a, cc, d}) s.faces.push_back(i);
        }
    deriveTopology(s);
    return s;
}

static void centreOf(const Sheet& s, float* c) {
    c[0] = c[1] = c[2] = 0.0f;
    for (int32_t i = 0; i < s.count(); ++i)
        for (int k = 0; k < 3; ++k) c[k] += s.rest[static_cast<size_t>(i) * 3 + static_cast<size_t>(k)];
    for (int k = 0; k < 3; ++k) c[k] /= static_cast<float>(s.count());
}

struct Made {
    int32_t body = 0;
    std::vector<int32_t> edgeSlot, bendSlot;
};

struct BuildOpts {
    std::vector<std::pair<int, float>> params;   // AVER_PHYS_SB_P_* overrides
    int32_t replace = 0, src = 0;
    const int32_t* map = nullptr;
    const float* statePos = nullptr;
    const float* stateVel = nullptr;
    float bendCompliance = 0.0f;
    // skin
    bool skin = false;
    std::vector<float> reach;
    int32_t jointCount = 1;
};

static Made build(const Sheet& s, const BuildOpts& o = {}) {
    Made m;
    const int32_t b = aver_phys_sb_builder_begin(s.count());
    if (!b) return m;
    aver_phys_sb_builder_vertices(b, s.rest.data(), s.invMass.data(), o.statePos, o.stateVel);
    aver_phys_sb_builder_faces(b, s.faces.data(), static_cast<int32_t>(s.faces.size() / 3));
    std::vector<float> compliance(s.edgeRest.size(), 0.0f), bendC(s.bends.size() / 4, o.bendCompliance);
    aver_phys_sb_builder_edges(b, s.edges.data(), s.edgeRest.data(), compliance.data(), static_cast<int32_t>(s.edgeRest.size()));
    if (!s.bends.empty())
        aver_phys_sb_builder_bends(b, s.bends.data(), bendC.data(), static_cast<int32_t>(s.bends.size() / 4));
    if (o.skin) {
        std::vector<int32_t> joints(static_cast<size_t>(s.count()) * 4, 0);
        std::vector<float> weights(static_cast<size_t>(s.count()) * 4, 0.0f);
        for (int32_t i = 0; i < s.count(); ++i) weights[static_cast<size_t>(i) * 4] = 1.0f;
        aver_phys_sb_builder_skin(b, joints.data(), weights.data(), o.reach.data(), -1.0f, o.jointCount);
    }
    float p[AVER_PHYS_SB_P_COUNT];
    for (float& f : p) f = std::nanf("");
    for (const auto& kv : o.params) p[kv.first] = kv.second;
    aver_phys_sb_builder_params(b, p, AVER_PHYS_SB_P_COUNT);
    m.edgeSlot.assign(s.edgeRest.size(), -1);
    m.bendSlot.assign(s.bends.size() / 4, -1);
    float c[3];
    centreOf(s, c);
    m.body = aver_phys_sb_builder_finish(b, c[0], c[1], c[2], o.replace, o.src, o.map,
                                         m.edgeSlot.empty() ? nullptr : m.edgeSlot.data(), m.bendSlot.empty() ? nullptr : m.bendSlot.data());
    if (!m.body) aver_phys_sb_builder_cancel(b);
    return m;
}

struct State {
    std::vector<float> pos, vel;
};

static State readState(int32_t body) {
    State st;
    const int32_t n = aver_phys_sb_vertex_count(body);
    st.pos.assign(static_cast<size_t>(n) * 3, 0.0f);
    st.vel.assign(static_cast<size_t>(n) * 3, 0.0f);
    aver_phys_sb_state(body, st.pos.data(), st.vel.data(), n);
    return st;
}

static float minZ(const State& st) {
    float z = 1e30f;
    for (size_t i = 2; i < st.pos.size(); i += 3) z = std::min(z, st.pos[i]);
    return z;
}

static bool finiteAll(const State& st) {
    for (const float f : st.pos) if (!std::isfinite(f)) return false;
    for (const float f : st.vel) if (!std::isfinite(f)) return false;
    return true;
}

static float meanOf(const State& st, const std::vector<int32_t>& ids, int axis, bool vel = false) {
    float sum = 0.0f;
    for (const int32_t i : ids) sum += (vel ? st.vel : st.pos)[static_cast<size_t>(i) * 3 + static_cast<size_t>(axis)];
    return sum / static_cast<float>(ids.size());
}

// ---- tests -----------------------------------------------------------------------------------------------

static void testHangsAndRefusals() {
    AVER_INFO("-- a pinned sheet hangs; refusals; the registry --");
    freshWorld();
    Sheet s = makeSheet(10, 10, 10.0f, 0, 0.0f, 0.0f, 200.0f);
    for (int32_t c = 0; c < s.cols; ++c) s.invMass[static_cast<size_t>(s.at(c, 0))] = 0.0f;
    BuildOpts o;
    o.params = {{AVER_PHYS_SB_P_ITERATIONS, 8.0f}, {AVER_PHYS_SB_P_DAMPING, 2.0f}};
    const Made m = build(s, o);
    check(m.body != 0 && aver_phys_sb_count() == 1 && aver_phys_sb_vertex_count(m.body) == 100, "a 10x10 sheet builds and registers");
    check(std::count(m.edgeSlot.begin(), m.edgeSlot.end(), -1) == 0 && std::count(m.bendSlot.begin(), m.bendSlot.end(), -1) == 0,
          "every edge and bend got a final slot");
    stepFor(3.0f);
    const State st = readState(m.body);
    check(finiteAll(st), "no NaN");
    float topDrift = 0.0f, bottomZ = 1e30f;
    for (int32_t c = 0; c < s.cols; ++c) {
        const size_t top = static_cast<size_t>(s.at(c, 0)) * 3, bot = static_cast<size_t>(s.at(c, s.rows - 1)) * 3;
        topDrift = std::max(topDrift, std::fabs(st.pos[top + 2] - 200.0f) + std::fabs(st.pos[top] - s.x(s.at(c, 0))));
        bottomZ = std::min(bottomZ, st.pos[bot + 2]);
    }
    check(topDrift < 0.5f, "the pinned edge stays put");
    check(bottomZ > 110.0f - 8.0f && bottomZ < 110.0f + 1.0f, "the sheet hangs at its length (110 cm), stretching a few cm at most");
    float maxSpeed = 0.0f;
    for (size_t i = 0; i < st.vel.size(); i += 3) maxSpeed = std::max(maxSpeed, std::sqrt(st.vel[i] * st.vel[i] + st.vel[i + 1] * st.vel[i + 1] + st.vel[i + 2] * st.vel[i + 2]));
    check(maxSpeed < 20.0f, "and it has settled");

    // A refused finish leaves the old body intact.
    Sheet bad = s;
    bad.faces[0] = 9999;
    BuildOpts ro;
    ro.replace = m.body;
    const Made refused = build(bad, ro);
    check(refused.body == 0 && aver_phys_last_error() != 0, "a face index out of range is refused with a reason");
    check(aver_phys_sb_vertex_count(m.body) == 100 && aver_phys_sb_state(m.body, nullptr, nullptr, 1) == 1, "and the old body is still there");
    check(aver_phys_sb_builder_begin(0) == 0 && aver_phys_sb_builder_begin(70000) == 0, "vertex counts outside 1..65536 are refused");
    check(aver_phys_sb_builder_finish(12345, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr) == 0, "a dead builder is refused");

    check(aver_phys_remove_body(m.body) == 1 && aver_phys_sb_count() == 0, "removing the body drops its record");
    build(s);
    check(aver_phys_sb_count() == 1, "a record exists again");
    aver_phys_shutdown();
    aver_phys_init();
    check(aver_phys_sb_count() == 0, "shutdown and re-init leave no registry entries");
}

static void testRebuildKeepsState() {
    AVER_INFO("-- a rebuild keeps the handle, the positions and the velocities --");
    freshWorld();
    const Sheet s = makeSheet(5, 5, 10.0f, 0, 0.0f, 0.0f, 300.0f);
    const Made m = build(s);
    stepFor(1.0f);
    const State before = readState(m.body);
    check(before.vel[2] < -300.0f, "the free sheet is falling");

    std::vector<int32_t> identity(static_cast<size_t>(s.count()));
    for (int32_t i = 0; i < s.count(); ++i) identity[static_cast<size_t>(i)] = i;
    BuildOpts o;
    o.replace = m.body; o.src = m.body; o.map = identity.data();
    const Made again = build(s, o);
    check(again.body == m.body, "the handle is unchanged");
    const State after = readState(m.body);
    float dp = 0.0f, dv = 0.0f;
    for (size_t i = 0; i < before.pos.size(); ++i) { dp = std::max(dp, std::fabs(before.pos[i] - after.pos[i])); dv = std::max(dv, std::fabs(before.vel[i] - after.vel[i])); }
    check(dp < 1e-2f && dv < 1e-1f, "positions and velocities carry across (within 0.01 cm and 0.1 cm/s)");
    check(aver_phys_sb_count() == 1, "one record, one body");

    // A duplicated vertex starts with its origin's state.
    Sheet dup = s;
    const int32_t n = s.count();
    dup.rest.insert(dup.rest.end(), {s.rest[12 * 3], s.rest[12 * 3 + 1], s.rest[12 * 3 + 2]});
    dup.invMass.push_back(1.0f);
    std::vector<int32_t> map(static_cast<size_t>(n + 1));
    for (int32_t i = 0; i < n; ++i) map[static_cast<size_t>(i)] = i;
    map[static_cast<size_t>(n)] = 12;
    BuildOpts d;
    d.replace = m.body; d.src = m.body; d.map = map.data();
    const State pre = readState(m.body);
    const Made grown = build(dup, d);
    check(grown.body == m.body && aver_phys_sb_vertex_count(m.body) == n + 1, "a rebuild may add vertices");
    const State post = readState(m.body);
    bool same = true;
    for (int k = 0; k < 3; ++k) {
        same = same && approx(post.vel[static_cast<size_t>(n) * 3 + static_cast<size_t>(k)], pre.vel[12 * 3 + static_cast<size_t>(k)], 0.1f);
        same = same && approx(post.pos[static_cast<size_t>(n) * 3 + static_cast<size_t>(k)], pre.pos[12 * 3 + static_cast<size_t>(k)], 0.05f);
    }
    check(same, "the duplicate begins with its origin's position and velocity");
}

static void testDentAndBendRule() {
    AVER_INFO("-- a dent is written in place; the bend rule keeps a pose --");
    freshWorld();
    // One triangle in zero gravity: shorten an edge and the vertices come together.
    Sheet tri;
    tri.cols = 3; tri.rows = 1;
    tri.rest = {0, 0, 100,  10, 0, 100,  0, 10, 100};
    tri.invMass = {1, 1, 1};
    tri.faces = {0, 1, 2};
    deriveTopology(tri);
    BuildOpts o;
    o.params = {{AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f}, {AVER_PHYS_SB_P_ITERATIONS, 10.0f}};
    const Made t = build(tri, o);
    check(t.body != 0 && t.edgeSlot.size() == 3, "a triangle with three edges");
    // Find the slot of edge (0,1).
    int32_t slot = -1;
    for (size_t e = 0; e < tri.edges.size() / 2; ++e)
        if (tri.edges[e * 2] == 0 && tri.edges[e * 2 + 1] == 1) slot = t.edgeSlot[e];
    const float eight = 8.0f;
    check(aver_phys_sb_set_edge_rest(t.body, &slot, &eight, 1) == 1, "set_edge_rest accepts the slot");
    stepFor(1.0f);
    const State st = readState(t.body);
    const float d01 = std::sqrt(std::pow(st.pos[0] - st.pos[3], 2.0f) + std::pow(st.pos[1] - st.pos[4], 2.0f) + std::pow(st.pos[2] - st.pos[5], 2.0f));
    const float d12 = std::sqrt(std::pow(st.pos[3] - st.pos[6], 2.0f) + std::pow(st.pos[4] - st.pos[7], 2.0f) + std::pow(st.pos[5] - st.pos[8], 2.0f));
    check(approx(d01, 8.0f, 0.4f), "the edge settles at its new rest length (8 cm)");
    check(approx(d12, std::sqrt(200.0f), 0.4f), "the other edges keep theirs");
    const int32_t outOfRange = 999;
    check(aver_phys_sb_set_edge_rest(t.body, &outOfRange, &eight, 1) == 0, "a slot out of range is refused");

    // Two flaps hinged on a pinned edge, one raised 60 degrees, gravity off. The other flap is pinned too:
    // with both free the bend settles on a tilted flat sheet (the raised flap at 30 degrees), not on flat.
    auto flaps = [] {
        Sheet f;
        f.cols = 4; f.rows = 1;
        f.rest = {0, 0, 100,  0, 10, 100,  10, 5, 100,  -10, 5, 100};
        f.invMass = {0, 0, 1, 0};
        f.faces = {0, 1, 2,  1, 0, 3};
        deriveTopology(f);
        return f;
    };
    const Sheet f = flaps();
    check(f.bends.size() == 4, "the hinge makes one bend");
    float bentPos[12] = {0, 0, 100,  0, 10, 100,  5, 5, 100.0f + 10.0f * std::sin(kPi / 3.0f),  -10, 5, 100};
    BuildOpts fo;
    fo.params = {{AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f}, {AVER_PHYS_SB_P_ITERATIONS, 10.0f}};
    fo.statePos = bentPos;
    const Made springy = build(f, fo);
    stepFor(1.5f);
    const State a = readState(springy.body);
    check(std::fabs(a.pos[2 * 3 + 2] - 100.0f) < 2.5f, "without the bend call the flap springs back flat");

    freshWorld();
    const Made kept = build(f, fo);
    check(aver_phys_sb_rest_bends_from_pose(kept.body, kept.bendSlot.data(), static_cast<int32_t>(kept.bendSlot.size())) == 1,
          "rest_bends_from_pose accepts the bend");
    stepFor(1.5f);
    const State b = readState(kept.body);
    check(b.pos[2 * 3 + 2] - 100.0f > 6.0f, "with it the flap keeps its raised pose (8.7 cm)");
}

static void testTear() {
    AVER_INFO("-- a tear: the halves fall apart, each keeping its momentum --");
    freshWorld();
    const Sheet full = makeSheet(6, 4, 10.0f, 0, 0.0f, 0.0f, 400.0f);
    const Made m = build(full);
    stepFor(0.5f);
    const State pre = readState(m.body);

    Sheet torn = full;
    torn.faces.clear();
    for (size_t t = 0; t + 2 < full.faces.size(); t += 3) {
        int32_t left = 0;
        for (size_t k = 0; k < 3; ++k) left += (full.faces[t + k] % full.cols) <= 2 ? 1 : 0;
        if (left == 0 || left == 3) for (size_t k = 0; k < 3; ++k) torn.faces.push_back(full.faces[t + k]);
    }
    deriveTopology(torn);
    check(torn.edges.size() < full.edges.size(), "the cut removes the edges across it");
    BuildOpts o;
    o.replace = m.body; o.src = m.body;
    std::vector<int32_t> identity(static_cast<size_t>(full.count()));
    for (int32_t i = 0; i < full.count(); ++i) identity[static_cast<size_t>(i)] = i;
    o.map = identity.data();
    const Made rebuilt = build(torn, o);
    check(rebuilt.body == m.body, "the rebuild keeps the handle");

    std::vector<int32_t> leftIds, rightIds;
    for (int32_t i = 0; i < full.count(); ++i) (i % full.cols <= 2 ? leftIds : rightIds).push_back(i);
    const float dirLeft[3] = {-1, 0, 0}, dirRight[3] = {1, 0, 0};
    const State mid = readState(m.body);
    const float cLeft[3] = {meanOf(mid, leftIds, 0), meanOf(mid, leftIds, 1), meanOf(mid, leftIds, 2)};
    const float cRight[3] = {meanOf(mid, rightIds, 0), meanOf(mid, rightIds, 1), meanOf(mid, rightIds, 2)};
    // 19 cm covers one half (its farthest vertex is 18 cm from its centre) and none of the other (20 cm away).
    aver_phys_sb_impulse(m.body, cLeft, 19.0f, dirLeft, 100.0f, 0.0f, 0.0f);
    aver_phys_sb_impulse(m.body, cRight, 19.0f, dirRight, 100.0f, 0.0f, 0.0f);
    const float gap0 = meanOf(mid, rightIds, 0) - meanOf(mid, leftIds, 0);
    stepFor(0.5f);
    const State post = readState(m.body);
    const float gap1 = meanOf(post, rightIds, 0) - meanOf(post, leftIds, 0);
    check(gap1 - gap0 > 60.0f, "the halves move apart (by about 100 cm in half a second)");
    check(meanOf(post, leftIds, 2, true) < -600.0f && meanOf(post, rightIds, 2, true) < -600.0f,
          "and both keep falling at the speed they had (momentum, not a restart)");
    (void)pre;
}

static void testSkinWorld() {
    AVER_INFO("-- skin_world: a pinned edge follows an entity matrix --");
    freshWorld();
    Sheet s = makeSheet(6, 5, 10.0f, 0, 100.0f, 50.0f, 150.0f);
    BuildOpts o;
    o.skin = true;
    // The hard skin snaps skinned vertices only, so the hanging rows get a loose reach (1000 cm) to follow the
    // 150 cm jump; left unskinned they are dragged across it in one step and the solver blows up.
    o.reach.assign(static_cast<size_t>(s.count()), 1000.0f);
    for (int32_t c = 0; c < s.cols; ++c) {
        s.invMass[static_cast<size_t>(s.at(c, 0))] = 0.0f;
        o.reach[static_cast<size_t>(s.at(c, 0))] = 0.0f;
    }
    const Made m = build(s, o);
    check(m.body != 0, "a skinned sheet builds");

    const Mat4 world = Mat4::fromQuat(Quat::fromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f)) * Mat4::translation(Vec3{300, -40, 25});
    float w16[16], identity16[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(w16, &world.m[0][0], sizeof w16);
    check(aver_phys_sb_skin_world(m.body, identity16, 1, w16, 1) == 1, "skin_world with hard skin");
    auto expectedTop = [&](int32_t c, float* out) {
        const float px = s.rest[static_cast<size_t>(s.at(c, 0)) * 3], py = s.rest[static_cast<size_t>(s.at(c, 0)) * 3 + 1], pz = s.rest[static_cast<size_t>(s.at(c, 0)) * 3 + 2];
        for (int k = 0; k < 3; ++k) out[k] = px * world.m[0][k] + py * world.m[1][k] + pz * world.m[2][k] + world.m[3][k];
    };
    float worst = 0.0f;
    State st = readState(m.body);
    for (int32_t c = 0; c < s.cols; ++c) {
        float e[3];
        expectedTop(c, e);
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(st.pos[static_cast<size_t>(s.at(c, 0)) * 3 + static_cast<size_t>(k)] - e[k]));
    }
    check(worst < 0.1f, "right after the hard skin the pinned row is at bind x entity world (0.1 cm)");
    stepFor(0.5f);
    st = readState(m.body);
    worst = 0.0f;
    for (int32_t c = 0; c < s.cols; ++c) {
        float e[3];
        expectedTop(c, e);
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(st.pos[static_cast<size_t>(s.at(c, 0)) * 3 + static_cast<size_t>(k)] - e[k]));
    }
    check(worst < 0.1f, "and it stays there while the rest hangs");
    check(finiteAll(st) && st.pos[static_cast<size_t>(s.at(0, s.rows - 1)) * 3 + 2] < 25.0f + 150.0f - 20.0f, "the bottom row hangs below the top");
    check(aver_phys_sb_skin_world(m.body, identity16, 0, nullptr, 0) == 0, "no joints is refused");
}

// Wind along +X on a vertical sheet in the YZ plane; the mean x of the bottom row over the last 2 s of 5.
static float windDeflection(float drag, float lift, float fixedStep, State* final = nullptr, bool wind = true) {
    freshWorld();
    if (fixedStep > 0.0f) aver_phys_set_fixed_step(fixedStep);
    Sheet s = makeSheet(8, 8, 10.0f, 1, 0.0f, 0.0f, 300.0f);
    for (int32_t c = 0; c < s.cols; ++c) s.invMass[static_cast<size_t>(s.at(c, 0))] = 0.0f;
    for (int32_t i = 0; i < s.count(); ++i) if (s.invMass[static_cast<size_t>(i)] > 0.0f) s.invMass[static_cast<size_t>(i)] = 50.0f;
    BuildOpts o;
    o.params = {{AVER_PHYS_SB_P_DAMPING, 3.0f}, {AVER_PHYS_SB_P_ITERATIONS, 8.0f}};
    const Made m = build(s, o);
    aver_phys_sb_set_aero(m.body, drag, lift, 0);
    if (wind) aver_phys_sb_set_wind(600.0f, 0, 0, 0, 0, 0);
    std::vector<int32_t> bottom;
    for (int32_t c = 0; c < s.cols; ++c) bottom.push_back(s.at(c, s.rows - 1));
    float sum = 0.0f;
    int samples = 0;
    const float dt = aver_phys_fixed_step();
    for (float t = 0.0f; t < 5.0f; t += dt) {
        aver_phys_step(dt);
        if (t >= 3.0f) { sum += meanOf(readState(m.body), bottom, 0); ++samples; }
    }
    if (final) *final = readState(m.body);
    return sum / static_cast<float>(samples);
}

static void testAero() {
    AVER_INFO("-- wind and drag --");
    const float d0 = windDeflection(0.0f, 0.0f, 0.0f);
    const float d02 = windDeflection(0.2f, 0.0f, 0.0f);
    const float d1 = windDeflection(1.0f, 0.0f, 0.0f);
    AVER_INFO("  mean deflection: drag 0 = {:.2f}, 0.2 = {:.2f}, 1 = {:.2f}", d0, d02, d1);
    check(std::fabs(d0) < 2.0f, "drag 0 does nothing");
    check(d02 > d0 + 3.0f, "drag 0.2 deflects the sheet downwind");
    check(d1 > d02 + 10.0f, "drag 1 deflects it much more than 0.2");
    const float lifted = windDeflection(1.0f, 0.3f, 0.0f);
    check(lifted > d0 + 3.0f, "drag with lift still deflects downwind");

    State a, b;
    const float ra = windDeflection(1.0f, 0.0f, 0.0f, &a);
    const float rb = windDeflection(1.0f, 0.0f, 0.0f, &b);
    check(ra == rb && a.pos.size() == b.pos.size() && std::memcmp(a.pos.data(), b.pos.data(), a.pos.size() * sizeof(float)) == 0,
          "the same run twice is bit identical");
    const float fast = windDeflection(1.0f, 0.0f, 1.0f / 120.0f);
    check(std::fabs(fast - d1) < 0.15f * d1, "a different fixed step gives the same deflection (within 15%)");
    aver_phys_set_fixed_step(1.0f / 60.0f);
}

static void testSleepAndFreeze() {
    AVER_INFO("-- sleep, wind keeping a body awake, freezing --");
    auto hanging = [](bool wind) {
        freshWorld();
        Sheet s = makeSheet(6, 6, 10.0f, 1, 0.0f, 0.0f, 300.0f);
        for (int32_t c = 0; c < s.cols; ++c) s.invMass[static_cast<size_t>(s.at(c, 0))] = 0.0f;
        BuildOpts o;
        o.params = {{AVER_PHYS_SB_P_DAMPING, 5.0f}};
        const Made m = build(s, o);
        aver_phys_sb_set_aero(m.body, 1.0f, 0.0f, 0);
        if (wind) aver_phys_sb_set_wind(20.0f, 0, 0, 0, 0, 0);
        stepFor(10.0f);
        return aver_phys_sb_is_active(m.body);
    };
    check(hanging(false) == 0, "a settled sheet with aero but no wind goes to sleep");
    check(hanging(true) == 1, "with wind it never does (SetAllowSleeping works on a soft body)");

    freshWorld();
    const Sheet s = makeSheet(4, 4, 10.0f, 1, 0.0f, 0.0f, 300.0f);
    const Made m = build(s);
    stepFor(0.2f);
    check(aver_phys_sb_set_active(m.body, 0) == 1 && aver_phys_sb_is_active(m.body) == 0, "set_active(0) deactivates");
    const State frozen = readState(m.body);
    stepFor(1.0f);
    const State later = readState(m.body);
    check(std::memcmp(frozen.pos.data(), later.pos.data(), frozen.pos.size() * sizeof(float)) == 0, "a deactivated body keeps its pose");
    aver_phys_sb_set_active(m.body, 1);
    stepFor(0.3f);
    check(readState(m.body).pos[2] < frozen.pos[2] - 5.0f, "and falls again when woken");
}

static void testImpacts() {
    AVER_INFO("-- impacts --");
    freshWorld();
    Sheet s = makeSheet(9, 9, 10.0f, 2, -40.0f, -40.0f, 0.0f);
    for (int32_t r = 0; r < s.rows; ++r)
        for (int32_t c = 0; c < s.cols; ++c)
            if (r == 0 || c == 0 || r == s.rows - 1 || c == s.cols - 1) s.invMass[static_cast<size_t>(s.at(c, r))] = 0.0f;
    BuildOpts o;
    o.params = {{AVER_PHYS_SB_P_IMPACTS, 1.0f}, {AVER_PHYS_SB_P_DAMPING, 5.0f}, {AVER_PHYS_SB_P_ITERATIONS, 8.0f}};
    const Made m = build(s, o);
    const int32_t sphere = aver_phys_add_dynamic_sphere(0, 0, 60.0f, 10.0f, 5.0f);
    check(m.body != 0 && sphere != 0, "a reporting sheet and a 5 kg sphere");

    const float expected = std::sqrt(2.0f * 980.0f * 50.0f);
    const float fixed = aver_phys_fixed_step();
    float firstClosing = -1.0f, mass = 0.0f, point[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
    int32_t softH = 0, otherH = 0, verts = 0;
    float maxClosing = 0.0f;
    bool onePerStep = true, cleared = false, clearedChecked = false;
    for (int i = 0; i < 90; ++i) {
        aver_phys_step(fixed);
        const int32_t n = aver_phys_sb_impact_count();
        if (n > 1) onePerStep = false;
        for (int32_t k = 0; k < n; ++k) {
            float c = 0.0f, mk = 0.0f, p[3], nn[3];
            int32_t sh = 0, oh = 0, v = 0;
            aver_phys_sb_impact_get(k, &sh, &oh, p, nn, &c, &mk, &v);
            maxClosing = std::max(maxClosing, c);
            if (firstClosing < 0.0f) { firstClosing = c; mass = mk; softH = sh; otherH = oh; verts = v; std::memcpy(point, p, sizeof p); std::memcpy(normal, nn, sizeof nn); }
        }
        if (n > 0 && !clearedChecked) {
            // A call that runs no step still clears last call's events.
            aver_phys_step(0.0001f);
            cleared = aver_phys_sb_impact_count() == 0;
            clearedChecked = true;
        }
    }
    AVER_INFO("  first closing {:.1f} cm/s, max {:.1f}, expected about {:.1f}", firstClosing, maxClosing, expected);
    check(firstClosing > 0.0f, "the sphere landing is reported");
    check(maxClosing > expected * 0.85f && maxClosing < expected * 1.15f, "the closing speed is within 15% of sqrt(2 g h)");
    // The documented bias (docs/SOFTBODY.md): the sphere's velocity is read after the step's integration, the vertex's before it,
    // so a falling body reads up to one step of gravity (g x dt) too fast.
    check(maxClosing - expected <= 980.0f * fixed + 0.15f * expected, "the closing-speed bias stays within g x dt plus the 15% bracket");
    check(softH == m.body && otherH == sphere && verts > 0, "it names the sheet, the sphere and the contact vertices");
    check(approx(mass, 5.0f, 0.25f), "and the sphere's mass");
    check(std::fabs(normal[2]) > 0.8f && std::fabs(point[2]) < 15.0f, "the normal is vertical and the point is on the sheet");
    check(onePerStep, "one record per pair per step");
    check(clearedChecked && cleared, "the events are cleared at the top of aver_phys_step");

    // Resting contact reports nothing.
    stepFor(12.0f);
    aver_phys_step(fixed);
    check(aver_phys_sb_impact_count() == 0, "a sphere resting on the settled sheet reports none");
}

static void testLayers() {
    AVER_INFO("-- layers --");
    auto drop = [](bool collide) {
        freshWorld();
        aver_phys_set_layer_collision(14, 0, collide ? 1 : 0);
        aver_phys_add_static_box(0, 0, -10.0f, 100.0f, 100.0f, 10.0f);
        Sheet s = makeSheet(5, 5, 10.0f, 2, -20.0f, -20.0f, 60.0f);
        BuildOpts o;
        o.params = {{AVER_PHYS_SB_P_LAYER, 14.0f}};
        const Made m = build(s, o);
        check(m.body != 0 && aver_phys_body_layer(m.body) == 14, "the sheet is on layer 14");
        stepFor(1.5f);
        return minZ(readState(m.body));
    };
    const float withCollision = drop(true);
    const float without = drop(false);
    AVER_INFO("  lowest vertex: collides {:.1f}, matrix off {:.1f}", withCollision, without);
    check(withCollision > -5.0f, "with the pair enabled the sheet lands on the box");
    check(without < -100.0f, "with the pair disabled it falls through");
    aver_phys_reset_layer_collisions();
}

static void testAddVelocity() {
    AVER_INFO("-- the indexed velocity add (ABI version 2) --");
    check(AVER_PHYS_SB_ABI_VERSION == 2 && aver_phys_sb_abi_version() == 2, "the version macro and the function agree on 2");

    freshWorld();
    Sheet s = makeSheet(5, 5, 10.0f, 1, 0.0f, 0.0f, 300.0f);
    for (int32_t c = 0; c < s.cols; ++c) s.invMass[static_cast<size_t>(s.at(c, 0))] = 0.0f;
    BuildOpts o;
    o.params = {{AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f}};
    const Made m = build(s, o);
    check(m.body != 0, "a pinned sheet with no gravity");
    stepFor(0.1f);
    const State before = readState(m.body);

    const int32_t target = s.at(2, 2), pinned = s.at(1, 0);
    const int32_t verts[2] = {target, pinned};
    const float dv[6] = {100.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f};
    check(aver_phys_sb_add_velocity(m.body, verts, dv, 2) == 1, "the call succeeds");
    const State after = readState(m.body);
    check(approx(after.vel[static_cast<size_t>(target) * 3] - before.vel[static_cast<size_t>(target) * 3], 100.0f, 0.5f), "the listed vertex gains 100 cm/s along +X");
    bool othersSame = true;
    for (int32_t v = 0; v < s.count(); ++v) {
        if (v == target) continue;
        for (int k = 0; k < 3; ++k)
            othersSame = othersSame && approx(after.vel[static_cast<size_t>(v) * 3 + static_cast<size_t>(k)], before.vel[static_cast<size_t>(v) * 3 + static_cast<size_t>(k)], 1e-3f);
    }
    check(othersSame, "no other vertex changes, and the kinematic one is skipped");

    // Refusals write nothing.
    const int32_t bad[2] = {target, 9999};
    const float dv2[6] = {5.0f, 0, 0, 5.0f, 0, 0};
    const State pre = readState(m.body);
    check(aver_phys_sb_add_velocity(m.body, bad, dv2, 2) == 0, "an index out of range refuses the call");
    check(aver_phys_sb_add_velocity(m.body, verts, nullptr, 2) == 0 && aver_phys_sb_add_velocity(m.body, nullptr, dv2, 1) == 0, "null arrays refuse it");
    const float nan[3] = {std::nanf(""), 0, 0};
    check(aver_phys_sb_add_velocity(m.body, verts, nan, 1) == 0, "a non-finite value refuses it");
    check(aver_phys_sb_add_velocity(0, verts, dv, 1) == 0 && aver_phys_sb_add_velocity(m.body, verts, dv, -1) == 0, "so do a bad handle and a negative count");
    const State post = readState(m.body);
    check(std::memcmp(pre.vel.data(), post.vel.data(), pre.vel.size() * sizeof(float)) == 0, "and none of those wrote anything");
    check(aver_phys_sb_add_velocity(m.body, verts, dv, 0) == 1, "n == 0 succeeds and does nothing");

    // Waking.
    aver_phys_sb_set_active(m.body, 0);
    const float zero[3] = {0, 0, 0};
    check(aver_phys_sb_add_velocity(m.body, verts, zero, 1) == 1 && aver_phys_sb_is_active(m.body) == 0, "an all-zero change leaves a sleeping body asleep");
    const float kick[3] = {0, 0, 40.0f};
    check(aver_phys_sb_add_velocity(m.body, verts, kick, 1) == 1 && aver_phys_sb_is_active(m.body) == 1, "a non-zero change wakes it");
    const int32_t onlyPinned[1] = {pinned};
    aver_phys_sb_set_active(m.body, 0);
    check(aver_phys_sb_add_velocity(m.body, onlyPinned, kick, 1) == 1 && aver_phys_sb_is_active(m.body) == 0, "a change that only names a kinematic vertex does not wake it");
}

int main() {
    AVER_INFO("SoftBodyBuilderTest");
    testHangsAndRefusals();
    testRebuildKeepsState();
    testDentAndBendRule();
    testTear();
    testSkinWorld();
    testAero();
    testSleepAndFreeze();
    testImpacts();
    testLayers();
    testAddVelocity();
    aver_phys_shutdown();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
