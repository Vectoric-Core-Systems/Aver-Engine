// The rest of Jolt's soft bodies (physics_softbody_ext_abi.h), through the plain-C ABI only: pressure, tetrahedral
// volumes, long range attachments, Cosserat rods, Jolt's constraint generator, per-vertex inverse mass, grab and
// release, contact policy, sensors, the collision group, skin, buoyancy and determinism. Links Aver.Physics and
// Aver.Core only, like SoftBodyBuilderTest.
//
// Thresholds name what is being decided (a volume that grows against one that does not, a rod that is not longer than it was
// built, a box that rests on a rope against one that falls through it) rather than how close Jolt gets.
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_ragdoll_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/physics/physics_softbody_ext_abi.h"
#include "aver/physics/physics_water_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int32_t errCode(AbiError e) { return static_cast<int32_t>(e); }

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_reset_layer_collisions();
    aver_phys_sb_set_wind(0, 0, 0, 0, 0, 0);
    aver_phys_clear_water_plane();
    aver_phys_clear_water_sampler();
    aver_phys_set_fixed_step(1.0f / 60.0f);
    aver_phys_init();
}

static void stepFor(float seconds) {
    const float fixed = aver_phys_fixed_step();
    for (float t = 0.0f; t < seconds; t += fixed) aver_phys_step(fixed);
}

// ---- meshes ---------------------------------------------------------------------------------------------------

struct Mesh {
    std::vector<float> rest;            // 3 per vertex, world cm
    std::vector<float> invMass;
    std::vector<int32_t> faces;         // 3 per triangle, Aver winding: (C - A) x (B - A) points outward
    std::vector<int32_t> edges;         // 2 per edge, derived from the faces or set by hand
    std::vector<float> edgeRest;
    std::vector<int32_t> tets;          // 4 per tetrahedron (cubes)
    int32_t count() const { return static_cast<int32_t>(rest.size() / 3); }
    Vec3 at(int32_t i) const { return Vec3(rest[static_cast<size_t>(i) * 3], rest[static_cast<size_t>(i) * 3 + 1], rest[static_cast<size_t>(i) * 3 + 2]); }
};

static void addVertex(Mesh& m, const Vec3& p, float invMass) {
    m.rest.push_back(p.x); m.rest.push_back(p.y); m.rest.push_back(p.z);
    m.invMass.push_back(invMass);
}

static void setEdge(Mesh& m, int32_t a, int32_t b) {
    m.edges.push_back(a); m.edges.push_back(b);
    m.edgeRest.push_back((m.at(a) - m.at(b)).size());
}

// Unique edges of the faces (and of the tetrahedra, when there are any).
static void deriveEdges(Mesh& m) {
    std::set<std::pair<int32_t, int32_t>> seen;
    auto add = [&](int32_t a, int32_t b) {
        const std::pair<int32_t, int32_t> key{std::min(a, b), std::max(a, b)};
        if (seen.insert(key).second) setEdge(m, key.first, key.second);
    };
    m.edges.clear(); m.edgeRest.clear();
    for (size_t t = 0; t + 2 < m.faces.size(); t += 3)
        for (size_t k = 0; k < 3; ++k) add(m.faces[t + k], m.faces[t + (k + 1) % 3]);
    for (size_t t = 0; t + 3 < m.tets.size(); t += 4)
        for (size_t x = 0; x < 4; ++x)
            for (size_t y = x + 1; y < 4; ++y) add(m.tets[t + x], m.tets[t + y]);
}

// Swaps two indices of a triangle when it faces the centre, so the Aver rule holds.
static void addOutwardFace(Mesh& m, int32_t a, int32_t b, int32_t c, const Vec3& centre) {
    const Vec3 pa = m.at(a), pb = m.at(b), pc = m.at(c);
    const Vec3 n = cross(pc - pa, pb - pa);
    const Vec3 mid = (pa + pb + pc) * (1.0f / 3.0f);
    if (dot(n, mid - centre) < 0.0f) std::swap(b, c);
    m.faces.push_back(a); m.faces.push_back(b); m.faces.push_back(c);
}

// A UV sphere with a vertex at each pole: rings - 1 latitudes of `segs` vertices.
static Mesh makeSphere(int rings, int segs, float radius, const Vec3& centre, float invMass) {
    Mesh m;
    addVertex(m, centre + Vec3(0, 0, radius), invMass);
    for (int r = 1; r < rings; ++r) {
        const float phi = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segs; ++s) {
            const float th = kTwoPi * static_cast<float>(s) / static_cast<float>(segs);
            addVertex(m, centre + Vec3(radius * std::sin(phi) * std::cos(th), radius * std::sin(phi) * std::sin(th), radius * std::cos(phi)), invMass);
        }
    }
    const int32_t south = m.count();
    addVertex(m, centre - Vec3(0, 0, radius), invMass);
    auto ring = [&](int r, int s) { return 1 + (r - 1) * segs + (s % segs); };
    for (int s = 0; s < segs; ++s) addOutwardFace(m, 0, ring(1, s), ring(1, s + 1), centre);
    for (int r = 1; r + 1 < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            addOutwardFace(m, ring(r, s), ring(r + 1, s), ring(r + 1, s + 1), centre);
            addOutwardFace(m, ring(r, s), ring(r + 1, s + 1), ring(r, s + 1), centre);
        }
    for (int s = 0; s < segs; ++s) addOutwardFace(m, south, ring(rings - 1, s + 1), ring(rings - 1, s), centre);
    deriveEdges(m);
    return m;
}

// n x n x n nodes, Kuhn tetrahedra (six per cell around the main diagonal, so neighbouring cells conform), the
// boundary triangles as faces and every tetrahedron edge as an edge.
static Mesh makeCube(int n, float spacing, const Vec3& origin, float invMass) {
    Mesh m;
    auto id = [&](int i, int j, int k) { return (k * n + j) * n + i; };
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) addVertex(m, origin + Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) * spacing, invMass);
    const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int k = 0; k + 1 < n; ++k)
        for (int j = 0; j + 1 < n; ++j)
            for (int i = 0; i + 1 < n; ++i)
                for (const auto& p : perms) {
                    int c[3] = {i, j, k};
                    int32_t v[4];
                    v[0] = id(c[0], c[1], c[2]);
                    for (int s = 0; s < 3; ++s) {
                        ++c[p[s]];
                        v[s + 1] = id(c[0], c[1], c[2]);
                    }
                    m.tets.insert(m.tets.end(), v, v + 4);
                }
    // Boundary triangles: a face of a tetrahedron that no other tetrahedron has.
    std::map<std::array<int32_t, 3>, int> count;
    auto key = [](int32_t a, int32_t b, int32_t c) { std::array<int32_t, 3> k{a, b, c}; std::sort(k.begin(), k.end()); return k; };
    for (size_t t = 0; t < m.tets.size(); t += 4)
        for (int skip = 0; skip < 4; ++skip) {
            int32_t f[3]; int q = 0;
            for (int x = 0; x < 4; ++x) if (x != skip) f[q++] = m.tets[t + static_cast<size_t>(x)];
            ++count[key(f[0], f[1], f[2])];
        }
    const Vec3 centre = origin + Vec3(1, 1, 1) * (spacing * static_cast<float>(n - 1) * 0.5f);
    for (const auto& [k, c] : count)
        if (c == 1) addOutwardFace(m, k[0], k[1], k[2], centre);
    deriveEdges(m);
    return m;
}

// cols x rows grid, plane 0: XZ (vertical), 2: XY (horizontal). Row 0 is the top / near edge.
static Mesh makeSheet(int cols, int rows, float spacing, int plane, const Vec3& origin, float invMass) {
    Mesh m;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const float u = static_cast<float>(c) * spacing, v = static_cast<float>(r) * spacing;
            addVertex(m, plane == 0 ? origin + Vec3(u, 0, -v) : origin + Vec3(u, v, 0), invMass);
        }
    auto at = [&](int c, int r) { return r * cols + c; };
    for (int r = 0; r + 1 < rows; ++r)
        for (int c = 0; c + 1 < cols; ++c) {
            const int32_t a = at(c, r), b = at(c + 1, r), d = at(c + 1, r + 1), e = at(c, r + 1);
            for (const int32_t i : {a, b, d, a, d, e}) m.faces.push_back(i);
        }
    deriveEdges(m);
    return m;
}

// A chain of nodes along +X, no faces.
static Mesh makeRope(int nodes, float spacing, const Vec3& origin, float invMass) {
    Mesh m;
    for (int i = 0; i < nodes; ++i) addVertex(m, origin + Vec3(static_cast<float>(i) * spacing, 0, 0), invMass);
    return m;
}

static float meshVolume(const Mesh& m) {   // polyhedron volume from the faces, cm^3
    double six = 0.0;
    const Vec3 o = m.at(0);
    for (size_t t = 0; t + 2 < m.faces.size(); t += 3) {
        const Vec3 a = m.at(m.faces[t]) - o, b = m.at(m.faces[t + 1]) - o, c = m.at(m.faces[t + 2]) - o;
        six += static_cast<double>(dot(a, cross(c, b)));
    }
    return static_cast<float>(std::fabs(six) / 6.0);
}

// ---- building -------------------------------------------------------------------------------------------------

struct Opts {
    std::vector<std::pair<int, float>> params;           // AVER_PHYS_SB_P_* overrides
    std::vector<std::pair<int, float>> ext;               // AVER_PHYS_SBX_P_* overrides
    float edgeCompliance = 0.0f;
    bool stageEdges = true;
    std::function<void(int32_t)> before;                  // sbx builder calls, before finish
};

static int32_t make(const Mesh& m, const Opts& o = {}) {
    const int32_t b = aver_phys_sb_builder_begin(m.count());
    if (!b) return 0;
    aver_phys_sb_builder_vertices(b, m.rest.data(), m.invMass.data(), nullptr, nullptr);
    if (!m.faces.empty()) aver_phys_sb_builder_faces(b, m.faces.data(), static_cast<int32_t>(m.faces.size() / 3));
    if (o.stageEdges && !m.edges.empty()) {
        std::vector<float> compliance(m.edgeRest.size(), o.edgeCompliance);
        aver_phys_sb_builder_edges(b, m.edges.data(), m.edgeRest.data(), compliance.data(), static_cast<int32_t>(m.edgeRest.size()));
    }
    float p[AVER_PHYS_SB_P_COUNT];
    for (float& f : p) f = std::nanf("");
    for (const auto& kv : o.params) p[kv.first] = kv.second;
    aver_phys_sb_builder_params(b, p, AVER_PHYS_SB_P_COUNT);
    if (!o.ext.empty()) {
        float q[AVER_PHYS_SBX_P_COUNT];
        for (float& f : q) f = std::nanf("");
        for (const auto& kv : o.ext) q[kv.first] = kv.second;
        aver_phys_sbx_builder_params(b, q, AVER_PHYS_SBX_P_COUNT);
    }
    if (o.before) o.before(b);
    Vec3 c(0, 0, 0);
    for (int32_t i = 0; i < m.count(); ++i) c += m.at(i);
    c = c / static_cast<float>(m.count());
    const int32_t h = aver_phys_sb_builder_finish(b, c.x, c.y, c.z, 0, 0, nullptr, nullptr, nullptr);
    if (!h) aver_phys_sb_builder_cancel(b);
    return h;
}

struct State { std::vector<float> pos, vel; };

static State readState(int32_t body) {
    State st;
    const int32_t n = aver_phys_sb_vertex_count(body);
    st.pos.assign(static_cast<size_t>(n) * 3, 0.0f);
    st.vel.assign(static_cast<size_t>(n) * 3, 0.0f);
    aver_phys_sb_state(body, st.pos.data(), st.vel.data(), n);
    return st;
}

static Vec3 pos(const State& s, int32_t i) { return Vec3(s.pos[static_cast<size_t>(i) * 3], s.pos[static_cast<size_t>(i) * 3 + 1], s.pos[static_cast<size_t>(i) * 3 + 2]); }

static bool finiteAll(const State& s) {
    for (const float f : s.pos) if (!std::isfinite(f)) return false;
    for (const float f : s.vel) if (!std::isfinite(f)) return false;
    return true;
}

static Vec3 centroidOf(const State& s) {
    Vec3 c(0, 0, 0);
    const int32_t n = static_cast<int32_t>(s.pos.size() / 3);
    for (int32_t i = 0; i < n; ++i) c += pos(s, i);
    return c / static_cast<float>(std::max(1, n));
}

static uint64_t hashState(const State& s) {
    uint64_t h = 1469598103934665603ull;
    for (const float f : s.pos) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = (h ^ bits) * 1099511628211ull;
    }
    return h;
}

static float dist3(const Vec3& a, const Vec3& b) { return (a - b).size(); }

// Opts shortcuts
static Opts hang(float iterations = 8.0f, float damping = 1.0f) {
    Opts o;
    o.params = {{AVER_PHYS_SB_P_ITERATIONS, iterations}, {AVER_PHYS_SB_P_DAMPING, damping}, {AVER_PHYS_SB_P_ALLOW_SLEEP, 0.0f}};
    return o;
}

// ---- tests ----------------------------------------------------------------------------------------------------

static void testVersionAndRefusals() {
    AVER_INFO("-- the version, and calls on things that are not ours --");
    freshWorld();
    check(AVER_PHYS_SBX_ABI_VERSION == 1 && aver_phys_sbx_abi_version() == 1, "aver_phys_sbx_abi_version() == 1");
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 100, 10, 10, 10, 1.0f);
    float k = 0;
    check(aver_phys_sbx_get_pressure(box, &k) == 0 && aver_phys_sbx_set_pressure(box, 1.0f) == 0, "a rigid body is not a soft body");
    check(aver_phys_sbx_set_pressure(0, 1.0f) == 0 && aver_phys_sbx_attachment_count(box) == 0, "a dead handle and a rigid body have nothing");
    check(aver_phys_sbx_builder_volumes(999, nullptr, nullptr, 0) == 0, "a dead builder is refused");
    const int32_t b = aver_phys_sb_builder_begin(4);
    const int32_t tet[4] = {0, 1, 2, 9};
    check(aver_phys_sbx_builder_volumes(b, tet, nullptr, 1) == 0 && aver_phys_last_error() == errCode(AbiError::OutOfRange), "a tetrahedron index out of range is refused");
    const int32_t same[4] = {0, 1, 1, 3};
    check(aver_phys_sbx_builder_volumes(b, same, nullptr, 1) == 0 && aver_phys_last_error() == errCode(AbiError::InvalidArgument), "a repeated tetrahedron vertex is refused");
    check(aver_phys_sbx_builder_rod_chain(b, 0, 2, 0, 0) == 0, "a chain of two vertices has no bend-twist pair and is refused");
    check(aver_phys_sbx_builder_lra_auto(b, 3, 1.0f) == 0, "an LRA mode above 2 is refused");
    float auto5[5] = {0, 0, 0, 0, 1};
    check(aver_phys_sbx_builder_auto(b, auto5, 0, 1, 8.0f) == 0 && aver_phys_sbx_builder_auto(b, nullptr, 1, 1, 8.0f) == 0, "auto needs attributes");
    aver_phys_sb_builder_cancel(b);
    check(aver_phys_sbx_builder_params(b, auto5, 1) == 0, "cancel kills the staged Ext state with the builder");
}

// A body that never calls anything here is the body it was: the same positions after 2 s with and without the no-op calls.
static uint64_t clothHash(bool noOpCalls) {
    freshWorld();
    Mesh s = makeSheet(8, 8, 10.0f, 0, Vec3(0, 0, 300), 1.0f);
    for (int c = 0; c < 8; ++c) s.invMass[static_cast<size_t>(c)] = 0.0f;
    Opts o = hang(8.0f, 2.0f);
    if (noOpCalls) o.before = [](int32_t b) {
        float nan[AVER_PHYS_SBX_P_COUNT];
        for (float& f : nan) f = std::nanf("");
        aver_phys_sbx_builder_params(b, nan, AVER_PHYS_SBX_P_COUNT);
    };
    const int32_t body = make(s, o);
    if (noOpCalls) {
        aver_phys_sbx_set_contact_policy(body, 1.0f, 1.0f, 1.0f);
        aver_phys_sbx_set_skin(body, 1, 1.0f);
        aver_phys_sbx_set_collision_group(body, 0);
        aver_phys_sbx_set_buoyancy(body, 0.0f, 1.0f, 1);
    }
    stepFor(2.0f);
    return hashState(readState(body));
}

static void testIdentity() {
    AVER_INFO("-- a body that calls none of the new functions is unchanged --");
    const uint64_t plain = clothHash(false);
    const uint64_t again = clothHash(false);
    const uint64_t withCalls = clothHash(true);
    check(plain == again, "two plain runs are bit identical");
    check(plain == withCalls, "and the default-valued wave 11 calls change nothing");
}

static void testPressure() {
    AVER_INFO("-- pressure --");
    freshWorld();
    Mesh ball = makeSphere(12, 16, 50.0f, Vec3(0, 0, 300), 1.0f);
    ball.invMass.assign(ball.invMass.size(), static_cast<float>(ball.count()) / 1.0f);   // 1 kg of skin
    // 20 iterations: a 1 kg skin under kilopascals is carried by the solver's iterations, not by the edge compliance
    // (measured at 2 kPa: 1.83x with 8 iterations, 1.25x with 20, and the 1.83 shell crumples on release, there being no bend constraint).
    Opts o = hang(20.0f, 3.0f);
    o.edgeCompliance = 1.0e-4f;
    o.params.push_back({AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f});
    const int32_t body = make(ball, o);
    check(body != 0, "a closed UV sphere builds");
    float rest = 0, now = 0;
    check(aver_phys_sbx_volume(body, &rest, &now) == 1, "sbx_volume answers");
    const float poly = meshVolume(ball), analytic = 4.0f / 3.0f * kPi * 125000.0f;
    check(std::fabs(rest - poly) < 0.01f * poly, "the rest volume is the polyhedron's (within 1%)");
    check(std::fabs(rest - analytic) < 0.08f * analytic, "and within 8% of the sphere's");
    check(std::fabs(now - rest) < 0.01f * rest, "before any step the volume is the rest volume");

    check(aver_phys_sbx_set_pressure(body, -1.0f) == 0 && aver_phys_sbx_set_pressure(body, std::nanf("")) == 0, "a negative or NaN pressure is refused");
    check(aver_phys_sbx_set_pressure(body, 1.0f) == 1, "set_pressure(1 kPa)");
    float p = 0;
    check(aver_phys_sbx_get_pressure(body, &p) == 1 && std::fabs(p - 1.0f) < 1e-6f, "and reads back");
    stepFor(3.0f);
    aver_phys_sbx_volume(body, &rest, &now);
    AVER_INFO("  volume ratio at 1 kPa: {:.3f}", now / rest);
    check(finiteAll(readState(body)), "no NaN");
    check(now / rest > 1.02f && now / rest < 1.6f, "an inflated shell holds more than its rest volume");
    check(aver_phys_sbx_set_pressure(body, 0.0f) == 1, "set_pressure(0)");
    stepFor(4.0f);
    aver_phys_sbx_volume(body, &rest, &now);
    check(std::fabs(now / rest - 1.0f) < 0.03f, "and it returns to within 3% of rest");

    // An open sheet cannot hold pressure.
    Mesh sheet = makeSheet(5, 5, 10.0f, 2, Vec3(0, 0, 100), 1.0f);
    const int32_t flat = make(sheet);
    check(flat != 0 && aver_phys_sbx_set_pressure(flat, 1.0f) == 0 && aver_phys_last_error() == errCode(AbiError::Unsupported), "an open mesh refuses pressure (Unsupported)");
    check(aver_phys_sbx_set_pressure(flat, 0.0f) == 1, "but 0 is always allowed");

    // A pressure given to the builder on a closed mesh is applied at finish, and ignored (with a log line) on an open one.
    freshWorld();
    Opts inflated = o;
    inflated.ext = {{AVER_PHYS_SBX_P_PRESSURE_KPA, 2.0f}};
    const int32_t ball2 = make(ball, inflated);
    aver_phys_sbx_get_pressure(ball2, &p);
    check(ball2 != 0 && std::fabs(p - 2.0f) < 1e-6f, "builder params carry the pressure");
    Opts ignored;
    ignored.ext = {{AVER_PHYS_SBX_P_PRESSURE_KPA, 2.0f}};
    const int32_t open2 = make(sheet, ignored);
    aver_phys_sbx_get_pressure(open2, &p);
    check(open2 != 0 && p == 0.0f, "on an open mesh the builder's pressure is dropped");
}

static float tetVolume(const Mesh& m, const State& st) {
    double sum = 0.0;
    for (size_t t = 0; t + 3 < m.tets.size(); t += 4) {
        const Vec3 a = pos(st, m.tets[t]), b = pos(st, m.tets[t + 1]), c = pos(st, m.tets[t + 2]), d = pos(st, m.tets[t + 3]);
        sum += std::fabs(static_cast<double>(dot(cross(b - a, c - a), d - a))) / 6.0;
    }
    return static_cast<float>(sum);
}

static void testVolumes() {
    AVER_INFO("-- tetrahedral volumes --");
    float loss[2] = {0, 0};
    for (int withVolumes = 0; withVolumes < 2; ++withVolumes) {
        freshWorld();
        aver_phys_add_static_box(0, 0, -10, 300, 300, 10);
        const Mesh cube = makeCube(6, 10.0f, Vec3(-25, -25, 40), 5.0f);
        Opts o = hang(10.0f, 1.0f);
        o.edgeCompliance = 1.0e-3f;
        if (withVolumes) o.before = [&](int32_t b) {
            check(aver_phys_sbx_builder_volumes(b, cube.tets.data(), nullptr, static_cast<int32_t>(cube.tets.size() / 4)) == 1, "the tetrahedra stage");
        };
        const int32_t body = make(cube, o);
        check(body != 0, withVolumes ? "a jelly cube with volumes builds" : "the same cube without volumes builds");
        const float v0 = tetVolume(cube, readState(body));
        stepFor(3.0f);
        const State st = readState(body);
        check(finiteAll(st), "no NaN");
        loss[withVolumes] = 1.0f - tetVolume(cube, st) / v0;
        AVER_INFO("  volume lost after the drop: {:.3f} ({})", loss[withVolumes], withVolumes ? "with volumes" : "edges only");
        if (withVolumes) {
            const int32_t n = aver_phys_sbx_slots(body, 0, nullptr, 0);
            std::vector<int32_t> slots(static_cast<size_t>(std::max(n, 0)));
            aver_phys_sbx_slots(body, 0, slots.data(), n);
            std::set<int32_t> seen(slots.begin(), slots.end());
            check(n == static_cast<int32_t>(cube.tets.size() / 4) && static_cast<int32_t>(seen.size()) == n && *seen.rbegin() == n - 1, "slots(volume) is a permutation of the staged tetrahedra");
        }
    }
    check(loss[1] < 0.05f, "with volume constraints the cube keeps its volume within 5%");
    check(loss[0] > loss[1] + 0.02f, "without them it loses more");
}

static void testLra() {
    AVER_INFO("-- long range attachments --");
    float worst[2] = {0, 0};
    for (int lra = 0; lra < 2; ++lra) {
        freshWorld();
        Mesh s = makeSheet(8, 8, 10.0f, 0, Vec3(0, 0, 300), 10.0f);
        for (int c = 0; c < 8; ++c) s.invMass[static_cast<size_t>(c)] = 0.0f;
        Opts o = hang(8.0f, 1.0f);
        o.edgeCompliance = 1.0e-2f;
        if (lra) o.before = [](int32_t b) { check(aver_phys_sbx_builder_lra_auto(b, 2, 1.0f) == 1, "lra_auto(geodesic, 1.0)"); };
        const int32_t body = make(s, o);
        check(body != 0, lra ? "a stretchy sheet with LRA builds" : "the same sheet without LRA builds");
        stepFor(4.0f);
        const State st = readState(body);
        check(finiteAll(st), "no NaN");
        for (int r = 1; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                worst[lra] = std::max(worst[lra], dist3(pos(st, r * 8 + c), pos(st, c)) / (static_cast<float>(r) * 10.0f));
        AVER_INFO("  worst stretch ratio {:.3f} ({})", worst[lra], lra ? "with LRA" : "without");
        if (lra) {
            int32_t slots[128];
            const int32_t n = aver_phys_sbx_slots(body, 1, slots, 128);
            std::set<int32_t> seen(slots, slots + std::min(n, 128));
            check(n == 56 && static_cast<int32_t>(seen.size()) == n && *seen.rbegin() == n - 1, "one LRA per free vertex, and slots(lra) is a permutation");
        }
    }
    check(worst[1] < 1.03f, "with LRA no vertex is more than 3% farther from its pin than the rest pose says");
    check(worst[0] > 1.25f, "without it the same sheet stretches (more than 25%)");

    // Explicit pairs: a pin and a free vertex (100 cm apart at rest) tied at 110 cm.
    freshWorld();
    Mesh strip = makeSheet(6, 2, 20.0f, 2, Vec3(0, 0, 200), 10.0f);
    strip.invMass[0] = 0.0f;
    Opts o = hang(8.0f, 0.5f);
    o.edgeCompliance = 1.0e-2f;
    o.params.push_back({AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f});
    o.before = [](int32_t b) {
        const int32_t pairs[2] = {0, 5};
        const float maxCm[1] = {110.0f};
        check(aver_phys_sbx_builder_lra(b, pairs, maxCm, 1) == 1, "an explicit pair stages");
        const int32_t bad[2] = {3, 3};
        check(aver_phys_sbx_builder_lra(b, bad, maxCm, 1) == 0, "a pair of one vertex is refused");
    };
    const int32_t body = make(strip, o);
    const float pull[3] = {400.0f, 0, 0};
    const int32_t far[1] = {5};
    aver_phys_sb_add_velocity(body, far, pull, 1);
    stepFor(1.0f);
    const State st = readState(body);
    check(dist3(pos(st, 0), pos(st, 5)) < 110.0f * 1.01f, "a vertex pulled away stays within the explicit 110 cm of its pin");
}

static void testAuto() {
    AVER_INFO("-- Jolt's constraint generator --");
    freshWorld();
    Mesh s = makeSheet(8, 8, 10.0f, 0, Vec3(0, 0, 300), 10.0f);
    for (int c = 0; c < 8; ++c) s.invMass[static_cast<size_t>(c)] = 0.0f;
    Opts o = hang(8.0f, 1.0f);
    o.stageEdges = false;
    o.before = [](int32_t b) {
        const float attrs[5] = {1.0e-4f, 1.0e-4f, 1.0e-3f, 2.0f, 1.02f};
        check(aver_phys_sbx_builder_auto(b, attrs, 1, 1, 8.0f) == 1, "auto with distance bends and geodesic LRA");
    };
    const int32_t body = make(s, o);
    check(body != 0, "a sheet with faces only builds");
    check(aver_phys_sbx_slots(body, 1, nullptr, 0) == 56, "the generator made one LRA per free vertex");
    stepFor(4.0f);
    const State st = readState(body);
    float worst = 0.0f;
    for (int r = 1; r < 8; ++r)
        for (int c = 0; c < 8; ++c) worst = std::max(worst, dist3(pos(st, r * 8 + c), pos(st, c)) / (static_cast<float>(r) * 10.0f));
    check(finiteAll(st) && worst < 1.05f, "it hangs within its LRA (5% of the rest pose)");

    // Mixing it with staged edges is refused at finish, with a reason.
    freshWorld();
    Opts mixed = hang();
    mixed.before = o.before;
    const int32_t refused = make(s, mixed);
    check(refused == 0 && aver_phys_last_error() == errCode(AbiError::InvalidArgument), "staged edges and auto together are refused");
}

static void testRods() {
    AVER_INFO("-- Cosserat rods --");
    freshWorld();
    Mesh rope = makeRope(20, 10.0f, Vec3(0, 0, 300), 10.0f);
    rope.invMass[0] = 0.0f;
    Opts o = hang(10.0f, 1.0f);
    o.params.push_back({AVER_PHYS_SB_P_VERTEX_RADIUS_CM, 1.0f});
    o.before = [](int32_t b) {
        const int32_t ab[6] = {0, 1, 1, 2, 2, 3};
        const int32_t uncovered[2] = {0, 1};
        check(aver_phys_sbx_builder_rods(b, ab, nullptr, 3, uncovered, nullptr, 1) == 0, "a rod no bend-twist pair names is refused");
        // Bend-twist compliance 1e-6 is a rigid bar (measured: a 190 cm cantilever of 0.1 kg nodes droops 2 cm); 1e-2 hangs.
        check(aver_phys_sbx_builder_rod_chain(b, 0, 20, 0.0f, 1.0e-2f) == 1, "a 20 node chain stages");
    };
    const int32_t body = make(rope, o);
    check(body != 0, "a face-less rod body builds (the extra face is added)");
    check(aver_phys_sbx_rod_count(body) == 19, "19 rods");
    int32_t slots[19];
    check(aver_phys_sbx_slots(body, 2, slots, 19) == 19 && aver_phys_sbx_slots(body, 3, nullptr, 0) == 18, "19 rod slots, 18 bend-twist slots");
    std::set<int32_t> seen(slots, slots + 19);
    check(seen.size() == 19 && *seen.rbegin() == 18, "slots(rod) is a permutation");

    stepFor(4.0f);
    const State st = readState(body);
    check(finiteAll(st), "no NaN");
    float worstLen = 0.0f;
    for (int i = 0; i + 1 < 20; ++i) worstLen = std::max(worstLen, std::fabs(dist3(pos(st, i), pos(st, i + 1)) - 10.0f) / 10.0f);
    check(worstLen < 0.01f, "every rod keeps its length within 1%");
    check(dist3(pos(st, 0), Vec3(0, 0, 300)) < 0.5f, "the pinned end stays put");
    check(pos(st, 19).z < 300.0f - 100.0f, "and the rest hangs");

    float worstUnit = 0.0f, worstAxis = 0.0f;
    for (int i = 0; i + 1 < 20; ++i) {
        float q[4] = {0, 0, 0, 1}, w[3] = {0, 0, 0};
        if (aver_phys_sbx_rod_state(body, slots[i], 1, q, w) != 1) { worstUnit = 9.0f; continue; }
        worstUnit = std::max(worstUnit, std::fabs(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) - 1.0f));
        const Vec3 axis = Quat(q[0], q[1], q[2], q[3]).rotate(Vec3(1, 0, 0));
        const Vec3 dir = (pos(st, i + 1) - pos(st, i)).getSafeNormal();
        worstAxis = std::max(worstAxis, std::acos(std::clamp(dot(axis, dir), -1.0f, 1.0f)) * (180.0f / kPi));
    }
    check(worstUnit < 1e-3f, "rod rotations are unit quaternions (within 1e-3)");
    AVER_INFO("  worst rod axis error {:.2f} degrees", worstAxis);
    check(worstAxis < 3.0f, "and each rod's +X axis points along the chain (within 3 degrees)");
    float q[4], w[3];
    check(aver_phys_sbx_rod_state(body, 18, 2, q, w) == 0 && aver_phys_sbx_rod_state(body, -1, 1, q, w) == 0, "rod_state past the end is refused");

    // The face-less body survives queries and a rigid box (no assert in a Debug build is the point).
    float point[3], normal[3];
    int32_t entity = 0;
    aver_phys_raycast(50.0f, -100.0f, 250.0f, 0, 1, 0, 300.0f, point, normal, &entity);
    aver_phys_sphere_cast(50.0f, -100.0f, 250.0f, 0, 1, 0, 300.0f, 10.0f, point, normal);
    aver_phys_add_dynamic_box(60.0f, 0, 200.0f, 10, 10, 10, 2.0f);
    stepFor(1.0f);
    check(finiteAll(readState(body)), "a ray, a sphere cast and a box leave the rod body finite");
}

static void testRopeSupportsBox() {
    AVER_INFO("-- a taut rope holds a box; a box falls through a rope with the pair ignored --");
    float boxZ[2] = {0, 0};
    for (int ignore = 0; ignore < 2; ++ignore) {
        freshWorld();
        aver_phys_add_static_box(0, 0, -300, 2000, 2000, 10);
        Mesh rope = makeRope(21, 10.0f, Vec3(-100, 0, 100), 10.0f);
        rope.invMass[0] = rope.invMass[20] = 0.0f;
        Opts o = hang(10.0f, 2.0f);
        o.params.push_back({AVER_PHYS_SB_P_VERTEX_RADIUS_CM, 2.0f});
        o.before = [](int32_t b) { aver_phys_sbx_builder_rod_chain(b, 0, 21, 0.0f, 1.0e-6f); };
        const int32_t body = make(rope, o);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 200, 15, 15, 15, 5.0f);
        if (ignore) check(aver_phys_sbx_set_pair_policy(body, box, AVER_PHYS_SBX_PAIR_IGNORE, 1, 1, 1) == 1, "the pair is set to ignore");
        stepFor(3.0f);
        float p[3] = {0, 0, 0};
        aver_phys_body_position(box, p);
        boxZ[ignore] = p[2];
        AVER_INFO("  box height after 3 s: {:.1f} ({})", p[2], ignore ? "pair ignored" : "normal");
    }
    check(boxZ[0] > 100.0f, "a 5 kg box rests on a taut 20 node rope");
    check(boxZ[1] < 0.0f, "with the pair ignored it falls straight through");
}

static void testInvMass() {
    AVER_INFO("-- per-vertex inverse mass --");
    freshWorld();
    Mesh rope = makeRope(11, 10.0f, Vec3(0, 0, 300), 10.0f);
    rope.invMass[0] = 0.0f;
    Opts o = hang(10.0f, 0.5f);
    o.before = [](int32_t b) { aver_phys_sbx_builder_rod_chain(b, 0, 11, 0.0f, 1.0e-2f); };   // a limp rope, not a rigid bar
    const int32_t body = make(rope, o);
    stepFor(0.4f);
    const Vec3 before = pos(readState(body), 5);
    const int32_t v[1] = {5};
    const float zero[1] = {0.0f};
    check(aver_phys_sbx_set_inv_mass(body, v, zero, 1) == 1, "pinning the middle node");
    const int32_t bad[1] = {99};
    check(aver_phys_sbx_set_inv_mass(body, bad, zero, 1) == 0 && aver_phys_sbx_set_inv_mass(body, v, nullptr, 1) == 0, "a bad vertex and a null array are refused");
    const float neg[1] = {-1.0f};
    check(aver_phys_sbx_set_inv_mass(body, v, neg, 1) == 0, "a negative inverse mass is refused");
    stepFor(2.0f);
    State st = readState(body);
    check(dist3(pos(st, 5), before) < 0.5f, "the pinned node stays where it was (within 0.5 cm)");
    check(dist3(pos(st, 10), Vec3(100, 0, 300)) > 20.0f, "while the tip swings");
    const float one[1] = {10.0f};
    aver_phys_sbx_set_inv_mass(body, v, one, 1);
    // The released node swings about its hanging place, so the test takes the largest excursion, not one instant.
    float farthest = 0.0f;
    for (int d = 0; d < 6; ++d) {
        stepFor(0.25f);
        farthest = std::max(farthest, dist3(pos(readState(body), 5), before));
    }
    check(farthest > 15.0f, "restored, it moves again");
}

static void testAttachments() {
    AVER_INFO("-- attachments --");
    freshWorld();
    Mesh s = makeSheet(5, 5, 10.0f, 2, Vec3(0, 0, 200), 1.0f);
    Opts o = hang(8.0f, 1.0f);
    o.params.push_back({AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f});
    const int32_t body = make(s, o);
    const int32_t centre = 12;
    const Vec3 start = pos(readState(body), centre);
    const float target[3] = {start.x + 60.0f, start.y, start.z};
    check(aver_phys_sbx_attach(body, centre, target, 100.0f) == 1 && aver_phys_sbx_attachment_count(body) == 1, "attach a vertex 60 cm from its target at 100 cm/s");
    stepFor(0.3f);
    const Vec3 mid = pos(readState(body), centre);
    check(dist3(mid, start) > 20.0f && dist3(mid, start) < 40.0f, "after 0.3 s it has covered about 30 cm (the speed cap)");
    stepFor(0.9f);
    check(dist3(pos(readState(body), centre), Vec3(target[0], target[1], target[2])) < 1.0f, "and arrives within 1 cm");
    const float moved[3] = {target[0], target[1] + 40.0f, target[2]};
    check(aver_phys_sbx_move_attachment(body, centre, moved) == 1, "move_attachment");
    stepFor(1.0f);
    check(dist3(pos(readState(body), centre), Vec3(moved[0], moved[1], moved[2])) < 1.0f, "the vertex follows the new target");
    const float outOfRange[3] = {0, 0, 0};
    check(aver_phys_sbx_attach(body, 99, outOfRange, 100.0f) == 0 && aver_phys_sbx_move_attachment(body, 3, outOfRange) == 0 && aver_phys_sbx_detach(body, 3, -1.0f) == 0,
          "a bad vertex, and moving or detaching a vertex that is not attached, are refused");

    // Detach restores the mass: a corner held in the air lets go and the sheet falls.
    freshWorld();
    Mesh hung = makeSheet(4, 4, 10.0f, 0, Vec3(0, 0, 300), 1.0f);
    const int32_t h = make(hung, hang(8.0f, 1.0f));
    const float corner[3] = {0, 0, 300};
    aver_phys_sbx_attach(h, 0, corner, 0.0f);
    stepFor(1.5f);
    const float farHeld = pos(readState(h), 15).z;
    check(farHeld > 300.0f - 50.0f, "held by one corner the sheet hangs from it (the far corner is 42 cm away)");
    check(aver_phys_sbx_detach(h, 0, -1.0f) == 1 && aver_phys_sbx_attachment_count(h) == 0, "detach");
    stepFor(1.0f);
    check(pos(readState(h), 0).z < 300.0f - 100.0f, "and the released corner falls");

    // A body target follows a kinematic box.
    freshWorld();
    Mesh pad = makeSheet(4, 4, 10.0f, 2, Vec3(0, 0, 100), 1.0f);
    Opts po = hang(8.0f, 1.0f);
    po.params.push_back({AVER_PHYS_SB_P_GRAVITY_SCALE, 0.0f});
    const int32_t sb = make(pad, po);
    // The box sits 50 cm under the pad, so it never touches it; the offset (10, 0, 50) in its frame is the pad's corner.
    const int32_t box = aver_phys_add_dynamic_box(-10.0f, 0, 50.0f, 10, 10, 10, 5.0f);
    aver_phys_body_set_motion_type(box, 1);
    const float offset[3] = {10.0f, 0, 50.0f};
    check(aver_phys_sbx_attach_to_body(sb, 0, box, offset, 0.0f) == 1, "attach to a body");
    const float fixed = aver_phys_fixed_step();
    for (int i = 0; i < 60; ++i) {
        aver_phys_body_move_kinematic(box, -10.0f + static_cast<float>(i + 1) * 2.0f, 0, 50.0f, 0, 0, 0, 1, fixed);
        aver_phys_step(fixed);
    }
    float bp[3];
    aver_phys_body_position(box, bp);
    check(dist3(pos(readState(sb), 0), Vec3(bp[0] + 10.0f, bp[1], bp[2] + 50.0f)) < 5.0f, "the vertex rides the box at its local offset (within 5 cm; it is one step behind)");
    check(aver_phys_sbx_attach_to_body(sb, 1, 0, offset, 0.0f) == 0 && aver_phys_sbx_attach_to_body(sb, 1, sb, offset, 0.0f) == 0, "a null body and the body itself are refused");
    aver_phys_remove_body(box);
    stepFor(0.1f);
    check(aver_phys_sbx_attachment_count(sb) == 0, "a stale target body releases the vertex");

    // Frame-rate independence: the vertex that arrives at its target gets there at the same time at 60 and 120 Hz.
    Vec3 arrived[2];
    for (int k = 0; k < 2; ++k) {
        freshWorld();
        aver_phys_set_fixed_step(k == 0 ? 1.0f / 60.0f : 1.0f / 120.0f);
        const int32_t t = make(makeSheet(3, 3, 10.0f, 2, Vec3(0, 0, 200), 1.0f), po);
        const float to[3] = {50.0f, 0, 200.0f};
        aver_phys_sbx_attach(t, 4, to, 100.0f);
        const float fixedNow = aver_phys_fixed_step();
        for (int i = 0, n = static_cast<int>(std::lround(0.25f / fixedNow)); i < n; ++i) aver_phys_step(fixedNow);
        arrived[k] = pos(readState(t), 4);
    }
    aver_phys_set_fixed_step(1.0f / 60.0f);
    check(dist3(arrived[0], arrived[1]) < 0.5f, "the same capped approach at 60 Hz and 120 Hz ends within 0.5 cm");
}

// A drum: cloth pinned all round its border, 9 x 9, 10 cm apart, centred over the origin at height z.
static Mesh pinnedCloth(float z) {
    Mesh s = makeSheet(9, 9, 10.0f, 2, Vec3(-40, -40, z), 20.0f);
    for (int32_t r = 0; r < 9; ++r)
        for (int32_t c = 0; c < 9; ++c)
            if (r == 0 || c == 0 || r == 8 || c == 8) s.invMass[static_cast<size_t>(r * 9 + c)] = 0.0f;
    return s;
}

static Opts clothOpts() {
    Opts o = hang(8.0f, 2.0f);
    o.params.push_back({AVER_PHYS_SB_P_IMPACTS, 0.0f});
    o.edgeCompliance = 1.0e-5f;
    return o;
}

static float boxFloorZ(const char* what, const std::function<void(int32_t cloth, int32_t box)>& setup) {
    freshWorld();
    const int32_t cloth = make(pinnedCloth(100.0f), clothOpts());
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 160.0f, 12, 12, 12, 5.0f);
    aver_phys_add_static_box(0, 0, -310.0f, 600, 600, 10);   // a floor far below
    if (setup) setup(cloth, box);
    stepFor(2.5f);
    float p[3] = {0, 0, 0};
    aver_phys_body_position(box, p);
    AVER_INFO("  {}: box height {:.1f}", what, p[2]);
    return p[2];
}

static void testContactPolicy() {
    AVER_INFO("-- contact policy --");
    const float normal = boxFloorZ("default", nullptr);
    check(normal > 40.0f, "a box dropped on a pinned cloth stays on it");

    const float otherZero = boxFloorZ("other inverse mass scale 0", [](int32_t cloth, int32_t) { aver_phys_sbx_set_contact_policy(cloth, 1.0f, 0.0f, 0.0f); });
    check(otherZero < -150.0f, "with the other body's scale at 0 the cloth cannot stop it");

    const float bothZero = boxFloorZ("both scales 0", [](int32_t cloth, int32_t) { aver_phys_sbx_set_contact_policy(cloth, 0.0f, 0.0f, 0.0f); });
    check(bothZero < -150.0f, "a pair with both scales 0 is skipped (Jolt's own rule)");

    const float ignored = boxFloorZ("pair ignored", [](int32_t cloth, int32_t box) { aver_phys_sbx_set_pair_policy(cloth, box, AVER_PHYS_SBX_PAIR_IGNORE, 1, 1, 1); });
    check(ignored < -150.0f, "an ignored pair passes the box through");

    const float cleared = boxFloorZ("pair ignored then cleared", [](int32_t cloth, int32_t box) {
        aver_phys_sbx_set_pair_policy(cloth, box, AVER_PHYS_SBX_PAIR_IGNORE, 1, 1, 1);
        aver_phys_sbx_clear_pair_policy(cloth, box);
    });
    check(cleared > 40.0f, "clearing the pair policy puts the cloth back in the way");

    // The soft side's scale 0 against a static body: Jolt skips the contact, so the cloth falls through it.
    freshWorld();
    aver_phys_add_static_box(0, 0, 0, 100, 100, 10);
    Mesh falling = makeSheet(5, 5, 10.0f, 2, Vec3(-20, -20, 100), 1.0f);
    const int32_t fall = make(falling, hang());
    aver_phys_sbx_set_contact_policy(fall, 0.0f, 1.0f, 1.0f);
    stepFor(2.0f);
    check(centroidOf(readState(fall)).z < -50.0f, "a soft inverse mass scale of 0 against a static box is skipped and the cloth falls through");
    check(aver_phys_sbx_set_contact_policy(fall, -1.0f, 1.0f, 1.0f) == 0 && aver_phys_sbx_set_pair_policy(fall, fall, 0, 1, 1, 1) == 0, "a negative scale and a pair with itself are refused");
}

static void testSensors() {
    AVER_INFO("-- sensors --");
    // A pair set to SENSOR: the box passes through the cloth, once in and once out.
    freshWorld();
    const int32_t cloth = make(pinnedCloth(100.0f), clothOpts());
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 180.0f, 12, 12, 12, 5.0f);
    check(aver_phys_sbx_set_pair_policy(cloth, box, AVER_PHYS_SBX_PAIR_SENSOR, 1, 1, 1) == 1, "the pair is a sensor pair");
    int entered = 0, exited = 0;
    bool right = true, cleared = true;
    const float fixed = aver_phys_fixed_step();
    for (int i = 0; i < 180; ++i) {
        aver_phys_step(fixed);
        const int32_t n = aver_phys_sbx_sensor_count();
        for (int32_t k = 0; k < n; ++k) {
            int32_t soft = 0, sensor = 0, in = 0;
            aver_phys_sbx_sensor_get(k, &soft, &sensor, &in);
            right = right && soft == cloth && sensor == box;
            (in ? entered : exited)++;
        }
        if (i == 5) { aver_phys_step(0.0001f); cleared = cleared && aver_phys_sbx_sensor_count() == 0; }
    }
    float p[3];
    aver_phys_body_position(box, p);
    check(p[2] < 0.0f, "the box fell through the cloth");
    check(entered == 1 && exited == 1, "one enter and one exit (" + std::to_string(entered) + ", " + std::to_string(exited) + ")");
    check(right && cleared, "events name the cloth and the box, and a call that runs no step clears them");
    int32_t a = 0;
    check(aver_phys_sbx_sensor_get(50, &a, &a, &a) == 0, "an index past the events is refused");

    // A Jolt sensor body around a cloth.
    freshWorld();
    const int32_t c2 = make(pinnedCloth(100.0f), clothOpts());
    const int32_t zone = aver_phys_add_sensor_box(0, 0, 100.0f, 60, 60, 40);
    int in2 = 0, out2 = 0;
    for (int i = 0; i < 20; ++i) {
        aver_phys_step(fixed);
        for (int32_t k = 0; k < aver_phys_sbx_sensor_count(); ++k) {
            int32_t soft, sensor, in;
            aver_phys_sbx_sensor_get(k, &soft, &sensor, &in);
            if (soft == c2 && sensor == zone) (in ? in2 : out2)++;
        }
    }
    check(in2 == 1 && out2 == 0, "a sensor body around the cloth reports one enter and no exit");
    aver_phys_remove_body(zone);
    for (int i = 0; i < 5; ++i) {
        aver_phys_step(fixed);
        for (int32_t k = 0; k < aver_phys_sbx_sensor_count(); ++k) {
            int32_t soft, sensor, in;
            aver_phys_sbx_sensor_get(k, &soft, &sensor, &in);
            if (soft == c2 && sensor == zone && !in) ++out2;
        }
    }
    check(out2 == 1, "and one exit when the sensor goes away");
}

static void testGroup() {
    AVER_INFO("-- collision group --");
    float centreZ[2] = {0, 0};
    for (int grouped = 0; grouped < 2; ++grouped) {
        freshWorld();
        aver_phys_add_static_box(0, 0, -310.0f, 600, 600, 10);
        const int32_t parents[1] = {-1};
        const float bonePos[3] = {0, 0, 100.0f}, boneRot[4] = {0, 0, 0, 1};
        const float capsule[7] = {0, 0, 0, 0, 0, 0, 15.0f};
        const float limits[3] = {80.0f, 80.0f, 30.0f};
        const int32_t ragdoll = aver_phys_ragdoll_create(1, parents, bonePos, boneRot, capsule, limits, 20.0f, 0);
        check(ragdoll != 0, "a one part ragdoll");
        aver_phys_ragdoll_set_mode(ragdoll, AVER_PHYS_RAGDOLL_KINEMATIC);
        Mesh drape = makeSheet(7, 7, 10.0f, 2, Vec3(-30, -30, 140.0f), 5.0f);
        Opts o = hang(8.0f, 2.0f);
        o.edgeCompliance = 1.0e-4f;
        const int32_t cloth = make(drape, o);
        if (grouped) check(aver_phys_sbx_set_collision_group(cloth, ragdoll) == 1, "the cloth joins the ragdoll's group");
        stepFor(2.5f);
        centreZ[grouped] = pos(readState(cloth), 24).z;
        AVER_INFO("  centre vertex height {:.1f} ({})", centreZ[grouped], grouped ? "grouped" : "free");
    }
    check(centreZ[0] > 100.0f, "a cloth dropped on a ragdoll part rests on it");
    check(centreZ[1] < 0.0f, "in the ragdoll's group it falls through its parts");

    // The grouped cloth still collides with a free box.
    freshWorld();
    const int32_t cloth = make(pinnedCloth(100.0f), clothOpts());
    aver_phys_sbx_set_collision_group(cloth, 7);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 160.0f, 12, 12, 12, 5.0f);
    stepFor(2.0f);
    float p[3];
    aver_phys_body_position(box, p);
    check(p[2] > 40.0f, "and still stops a free box");
    check(aver_phys_sbx_set_collision_group(cloth, -1) == 0 && aver_phys_sbx_set_collision_group(cloth, 0) == 1, "a negative group is refused, 0 clears it");
}

static void testSkin() {
    AVER_INFO("-- skin constraints --");
    auto skinned = [](float reach) {
        Mesh s = makeSheet(5, 5, 10.0f, 0, Vec3(0, 0, 300), 1.0f);
        const int32_t b = aver_phys_sb_builder_begin(s.count());
        aver_phys_sb_builder_vertices(b, s.rest.data(), s.invMass.data(), nullptr, nullptr);
        aver_phys_sb_builder_faces(b, s.faces.data(), static_cast<int32_t>(s.faces.size() / 3));
        std::vector<float> compliance(s.edgeRest.size(), 0.0f);
        aver_phys_sb_builder_edges(b, s.edges.data(), s.edgeRest.data(), compliance.data(), static_cast<int32_t>(s.edgeRest.size()));
        std::vector<int32_t> joints(static_cast<size_t>(s.count()) * 4, 0);
        std::vector<float> weights(static_cast<size_t>(s.count()) * 4, 0.0f), reachCm(static_cast<size_t>(s.count()), reach);
        for (int32_t i = 0; i < s.count(); ++i) weights[static_cast<size_t>(i) * 4] = 1.0f;
        aver_phys_sb_builder_skin(b, joints.data(), weights.data(), reachCm.data(), -1.0f, 1);
        float p[AVER_PHYS_SB_P_COUNT];
        for (float& f : p) f = std::nanf("");
        p[AVER_PHYS_SB_P_ALLOW_SLEEP] = 0.0f;
        aver_phys_sb_builder_params(b, p, AVER_PHYS_SB_P_COUNT);
        return aver_phys_sb_builder_finish(b, 20, 0, 280, 0, 0, nullptr, nullptr, nullptr);
    };
    freshWorld();
    const int32_t hard = skinned(0.0f);
    stepFor(1.0f);
    check(centroidOf(readState(hard)).z > 270.0f, "a hard skinned cloth hangs in the air on its bind pose");
    check(aver_phys_sbx_set_skin(hard, 0, 1.0f) == 1, "set_skin(off)");
    stepFor(1.0f);
    check(centroidOf(readState(hard)).z < 200.0f, "and falls once the skin constraints are off");

    freshWorld();
    const int32_t loose = skinned(1000.0f);
    stepFor(1.0f);
    check(centroidOf(readState(loose)).z < 250.0f, "a loosely skinned cloth sags");
    check(aver_phys_sbx_set_skin(loose, 1, 0.0f) == 1, "set_skin(on, scale 0) is a hard skin");
    stepFor(0.5f);
    check(finiteAll(readState(loose)), "and stays finite");
    check(aver_phys_sbx_set_skin(loose, 1, -1.0f) == 0, "a negative scale is refused");
}

// ---- buoyancy -------------------------------------------------------------------------------------------------

static int32_t sampleFlat(float, float, float, AverPhysWaterSample* out, void*) {
    out->surfaceZCm = 0.0f;
    out->normal[0] = 0; out->normal[1] = 0; out->normal[2] = 1;
    out->currentCmS[0] = out->currentCmS[1] = out->currentCmS[2] = 0;
    out->buoyancy = 1.0f; out->linearDrag = 0.5f; out->angularDrag = 0.01f;
    return 1;
}

// A closed sphere of the given density (kg/m^3), edges stiff, starting with its centre at z0, settled for `seconds`.
static float settledCentre(float density, float z0, float seconds, int source) {
    freshWorld();
    aver_phys_set_gravity(0, 0, -980.0f);
    const float zero[3] = {0, 0, 0}, up[3] = {0, 0, 1};
    if (source == 0) aver_phys_set_water_plane(0.0f, up, 1.0f, 0.5f, 0.01f, zero);
    else aver_phys_set_water_sampler(&sampleFlat, nullptr);
    Mesh ball = makeSphere(12, 16, 50.0f, Vec3(0, 0, z0), 1.0f);
    const float massKg = density * meshVolume(ball) * 1.0e-6f;
    ball.invMass.assign(ball.invMass.size(), static_cast<float>(ball.count()) / massKg);
    // A shell with nothing inside is crushed by the water around it (measured: density 500 sinks as a crumpled ball), so
    // the sphere is inflated: 5 kPa, about the pressure at its own depth.
    Opts o = hang(20.0f, 2.0f);
    o.edgeCompliance = 1.0e-5f;
    o.ext = {{AVER_PHYS_SBX_P_PRESSURE_KPA, 5.0f}, {AVER_PHYS_SBX_P_FLUID_DENSITY, 1000.0f}, {AVER_PHYS_SBX_P_FLUID_DRAG, 1.0f}, {AVER_PHYS_SBX_P_BUOY_FLAGS, static_cast<float>(AVER_PHYS_SBX_BUOY_WORLD_WATER)}};
    const int32_t body = make(ball, o);
    stepFor(seconds);
    const State st = readState(body);
    const float z = centroidOf(st).z;
    aver_phys_clear_water_plane();
    aver_phys_clear_water_sampler();
    return z;
}

static void testBuoyancy() {
    AVER_INFO("-- buoyancy --");
    const float floatZ = settledCentre(500.0f, 30.0f, 6.0f, 0);
    AVER_INFO("  density 500: centre settles at {:.1f} (waterline at the centre by Archimedes)", floatZ);
    check(std::fabs(floatZ) < 5.0f, "a sphere of density 500 floats with its waterline at the equator (within 5 cm)");
    const float sinkZ = settledCentre(1500.0f, 0.0f, 3.0f, 0);
    check(sinkZ < -100.0f, "a sphere of density 1500 sinks");
    const float sampled = settledCentre(500.0f, 30.0f, 6.0f, 1);
    check(std::fabs(sampled) < 8.0f, "the host water sampler floats it too (within 8 cm)");

    // The displaced volume and the fractions.
    float fraction[5];
    const float depths[5] = {-80.0f, -30.0f, 0.0f, 30.0f, 80.0f};   // centre height: below, ..., above
    for (int i = 0; i < 5; ++i) {
        freshWorld();
        aver_phys_set_gravity(0, 0, 0);
        Mesh ball = makeSphere(12, 16, 50.0f, Vec3(0, 0, depths[i]), 1.0f);
        ball.invMass.assign(ball.invMass.size(), static_cast<float>(ball.count()) / 1000.0f);   // 1000 kg: it does not move in one step
        Opts o = hang();
        const int32_t body = make(ball, o);
        aver_phys_sbx_set_buoyancy(body, 1000.0f, 1.0f, AVER_PHYS_SBX_BUOY_OWN_PLANE);
        const float point[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, cur[3] = {0, 0, 0};
        aver_phys_sbx_set_water_plane(body, point, nrm, cur);
        aver_phys_step(aver_phys_fixed_step());
        float vol = 0;
        aver_phys_sbx_submerged(body, &fraction[i], &vol);
    }
    check(fraction[0] > fraction[1] && fraction[1] > fraction[2] && fraction[2] > fraction[3] && fraction[3] >= fraction[4], "submerged fractions fall monotonically as the sphere rises");
    check(fraction[0] > 0.95f && fraction[4] < 0.05f && fraction[2] > 0.3f && fraction[2] < 0.7f, "fully under, fully out and half in read as such");

    // An open sheet in a current is pushed by drag only.
    freshWorld();
    aver_phys_set_gravity(0, 0, 0);
    Mesh sheet = makeSheet(6, 6, 10.0f, 0, Vec3(0, 0, 0), 20.0f);
    Opts so = hang(8.0f, 0.2f);
    const int32_t flat = make(sheet, so);
    aver_phys_sbx_set_buoyancy(flat, 1000.0f, 1.0f, AVER_PHYS_SBX_BUOY_OWN_PLANE);
    const float point[3] = {0, 0, 100.0f}, nrm[3] = {0, 0, 1}, current[3] = {0, 200.0f, 0};
    aver_phys_sbx_set_water_plane(flat, point, nrm, current);
    stepFor(1.0f);
    float fr = 0, vol = 0;
    aver_phys_sbx_submerged(flat, &fr, &vol);
    check(fr > 0.9f && vol == 0.0f, "an open sheet reports submerged area and no displaced volume");
    check(centroidOf(readState(flat)).y > 5.0f, "the current pushes it along +Y by drag");
    check(aver_phys_sbx_set_buoyancy(flat, -1.0f, 1.0f, 1) == 0 && aver_phys_sbx_set_water_plane(flat, point, nullptr, current) == 0, "a negative density and a missing normal are refused");
}

// ---- determinism ----------------------------------------------------------------------------------------------

static uint64_t everythingOn() {
    freshWorld();
    aver_phys_set_gravity(0, 0, -980.0f);
    const float zero[3] = {0, 0, 0}, up[3] = {0, 0, 1};
    aver_phys_set_water_plane(-60.0f, up, 1.0f, 0.5f, 0.01f, zero);
    aver_phys_add_static_box(0, 0, -300.0f, 600, 600, 10);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const State& s) { h = (h ^ hashState(s)) * 1099511628211ull; };

    Mesh rope = makeRope(12, 10.0f, Vec3(-60, 0, 150), 10.0f);
    rope.invMass[0] = 0.0f;
    Opts ro = hang(10.0f, 1.0f);
    ro.before = [](int32_t b) { aver_phys_sbx_builder_rod_chain(b, 0, 12, 0.0f, 1.0e-6f); };
    const int32_t r = make(rope, ro);

    Mesh ball = makeSphere(8, 12, 30.0f, Vec3(0, 70, 200), 1.0f);
    ball.invMass.assign(ball.invMass.size(), static_cast<float>(ball.count()) / 8.0f);
    Opts bo = hang(10.0f, 2.0f);
    bo.edgeCompliance = 5.0e-5f;
    bo.ext = {{AVER_PHYS_SBX_P_PRESSURE_KPA, 1.0f}, {AVER_PHYS_SBX_P_FLUID_DENSITY, 1000.0f}};
    const int32_t b = make(ball, bo);

    Mesh drape = pinnedCloth(120.0f);
    Opts co = clothOpts();
    co.before = [](int32_t bb) { aver_phys_sbx_builder_lra_auto(bb, 1, 1.01f); };
    const int32_t c = make(drape, co);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 200.0f, 12, 12, 12, 4.0f);
    aver_phys_sbx_set_pair_policy(c, box, AVER_PHYS_SBX_PAIR_SENSOR, 1, 1, 1);
    const float target[3] = {-60.0f, 30.0f, 150.0f};
    aver_phys_sbx_attach(r, 11, target, 300.0f);
    stepFor(3.0f);
    mix(readState(r)); mix(readState(b)); mix(readState(c));
    float p[3];
    aver_phys_body_position(box, p);
    h = (h ^ static_cast<uint64_t>(std::llround(p[2] * 1000.0f))) * 1099511628211ull;
    aver_phys_clear_water_plane();
    return h;
}

static void testDeterminism() {
    AVER_INFO("-- determinism --");
    const uint64_t a = everythingOn();
    const uint64_t b = everythingOn();
    check(a == b, "two runs with rods, pressure, buoyancy, LRA, a sensor pair and an attachment hash alike");
}

int main() {
    AVER_INFO("SoftBodyExtTest");
    testVersionAndRefusals();
    testIdentity();
    testPressure();
    testVolumes();
    testLra();
    testAuto();
    testRods();
    testRopeSupportsBox();
    testInvMass();
    testAttachments();
    testContactPolicy();
    testSensors();
    testGroup();
    testSkin();
    testBuoyancy();
    testDeterminism();
    aver_phys_shutdown();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
