// PhysicsShapeCacheTest -- the shape binary cache (physics_state_abi.h, phase E): a mesh or heightfield shape to bytes and back.
// Through the ABI only. It checks the thing a cache must get right, which is that a restored shape IS the original:
//
//   * the blob of a restored shape equals the blob of the shape it came from, byte for byte (same BVH bits),
//   * rays, bounds and a dropped ball behave the same on a restored mesh and a restored heightfield as on the originals,
//   * every kind of bad blob (wrong key, cache version, precision, magic, truncated, one damaged byte) is a MISS: handle 0, no
//     warning in the log, and a good restore still works afterwards,
//   * what cannot be saved is refused (a box, a mesh with a surface palette, a handle that is not a shape),
//   * two threads restoring at once, the size of a blob against its triangles (a guard against saving them twice),
//   * a restored heightfield is a plain collision shape (set_heights refuses it) while the built one is still editable.
//
// Hand-run, exit code = failure count, same shape as PhysicsStateTest.cpp. UNVERIFIED thresholds are marked.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_terrain_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::atomic<int> g_warnings{0};
static void warnSink(void*, LogLevel level, std::string_view) {
    if (level >= LogLevel::Warn) ++g_warnings;
}

static const uint64_t kKey = 0x1234ABCD5678EF01ull;
static const float kDt = 1.0f / 60.0f;

// ---- the geometry --------------------------------------------------------------------------------------------------------

static float bump(float x, float y) { return 25.0f * std::sin(x * 0.045f) * std::cos(y * 0.03f); }

struct Mesh {
    std::vector<float> v;
    std::vector<int32_t> idx;
};

// An n x n vertex grid centred on the origin, `cell` cm apart; faces point +Z (the winding of PhysicsTest's floor).
static Mesh bumpyMesh(int n, float cell) {
    Mesh m;
    const float half = static_cast<float>(n - 1) * cell * 0.5f;
    for (int iy = 0; iy < n; ++iy)
        for (int ix = 0; ix < n; ++ix) {
            const float x = static_cast<float>(ix) * cell - half, y = static_cast<float>(iy) * cell - half;
            m.v.insert(m.v.end(), {x, y, bump(x, y)});
        }
    for (int iy = 0; iy + 1 < n; ++iy)
        for (int ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = iy * n + ix, b = a + 1, c = a + n + 1, d = a + n;
            m.idx.insert(m.idx.end(), {a, b, c, a, c, d});
        }
    return m;
}

static int32_t makeMesh(const Mesh& m) {
    return aver_phys_create_mesh_shape(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(), static_cast<int32_t>(m.idx.size()));
}

static std::vector<float> bumpySamples(int n) {
    std::vector<float> s(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) s[static_cast<size_t>(y) * n + x] = bump(static_cast<float>(x) * 25.0f, static_cast<float>(y) * 25.0f);
    return s;
}

static std::vector<uint8_t> blobOf(int32_t shape, uint64_t key) {
    const int32_t need = aver_phys_shape_save(shape, key, nullptr, 0);
    std::vector<uint8_t> b(need > 0 ? static_cast<size_t>(need) : 0u);
    if (need > 0 && aver_phys_shape_save(shape, key, b.data(), need) != need) b.clear();
    return b;
}

static int32_t place(int32_t shape, float x, float y, float z) {
    return aver_phys_add_mesh_shape_body(shape, x, y, z, 0, 0, 0, 1, 1, 1, 1);
}

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_init();
}

// ---- 1. a restored shape is the original ---------------------------------------------------------------------------------

// Rays down a lattice over the shape placed at `ax` and over the one at `bx`: every hit z within `tol` of its twin.
static void compareRays(const char* what, float ax, float bx, float x0, float x1, float y0, float y1, float tol) {
    int hits = 0, same = 0;
    float worst = 0.0f;
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 7; ++j) {
            const float x = x0 + (x1 - x0) * static_cast<float>(i) / 6.0f, y = y0 + (y1 - y0) * static_cast<float>(j) / 6.0f;
            float pa[3] = {}, na[3] = {}, pb[3] = {}, nb[3] = {};
            int32_t ea = 0, eb = 0;
            const int32_t ha = aver_phys_raycast(ax + x, y, 400.0f, 0, 0, -1.0f, 900.0f, pa, na, &ea);
            const int32_t hb = aver_phys_raycast(bx + x, y, 400.0f, 0, 0, -1.0f, 900.0f, pb, nb, &eb);
            if (ha && hb) {
                ++hits;
                const float d = std::fabs(pa[2] - pb[2]) + std::fabs(na[2] - nb[2]);
                worst = std::fmax(worst, d);
                if (d < tol) ++same;
            }
        }
    check(hits >= 40 && same == hits,
          std::string(what) + ": " + std::to_string(hits) + " of 49 rays hit both, worst difference " + std::to_string(worst) + " cm (tolerance " + std::to_string(tol) + ")");
}

static void compareBounds(const char* what, int32_t a, int32_t b, float dx) {
    float amin[3] = {}, amax[3] = {}, bmin[3] = {}, bmax[3] = {};
    check(aver_phys_body_aabb(a, amin, amax) == 1 && aver_phys_body_aabb(b, bmin, bmax) == 1, std::string(what) + ": both bodies have bounds");
    float worst = 0.0f;
    worst = std::fmax(worst, std::fabs(amin[0] + dx - bmin[0]));
    worst = std::fmax(worst, std::fabs(amax[0] + dx - bmax[0]));
    for (int k = 1; k < 3; ++k) {
        worst = std::fmax(worst, std::fabs(amin[k] - bmin[k]));
        worst = std::fmax(worst, std::fabs(amax[k] - bmax[k]));
    }
    check(worst < 0.05f, std::string(what) + ": the bounding boxes agree (worst " + std::to_string(worst) + " cm)");
}

// A ball dropped on each: both settle at the same height above the same ground.
static void compareDrop(const char* what, float ax, float bx, float x, float y) {
    const int32_t a = aver_phys_add_dynamic_sphere(ax + x, y, 150.0f, 10.0f, 5.0f);
    const int32_t b = aver_phys_add_dynamic_sphere(bx + x, y, 150.0f, 10.0f, 5.0f);
    for (int i = 0; i < 240; ++i) aver_phys_step(kDt);
    float pa[3] = {}, pb[3] = {};
    aver_phys_body_position(a, pa);
    aver_phys_body_position(b, pb);
    const float dz = std::fabs(pa[2] - pb[2]), dy = std::fabs(pa[1] - pb[1]), dx = std::fabs((pa[0] - ax) - (pb[0] - bx));
    check(pa[2] > -50.0f && pa[2] < 150.0f, std::string(what) + ": the ball on the original rests on it (z " + std::to_string(pa[2]) + ")");
    // UNVERIFIED tolerance: a ball rolling on a bumpy surface, 50 m apart in float.
    check(dz < 1.0f && dy < 2.0f && dx < 2.0f, std::string(what) + ": and the ball on the restored shape ends in the same place (dz " + std::to_string(dz) + ")");
}

// The same drop in a fresh world each, the original built and the restored one read from the blob. Side by side in one world they
// land on different floats 50 m apart, and one after the other the second run inherits the first's contact state (the same
// shape twice gives different rolls: measured 2 cm apart after 4 s); a ball rolling on a bumpy mesh amplifies either.
static void compareDropFresh(const char* what, const Mesh& m, const std::vector<uint8_t>& blob, float x, float y) {
    float p[2][3] = {};
    for (int k = 0; k < 2; ++k) {
        freshWorld();
        const int32_t shape = k == 0 ? makeMesh(m) : aver_phys_shape_restore(blob.data(), static_cast<int32_t>(blob.size()), kKey);
        place(shape, 0, 0, 0);
        const int32_t ball = aver_phys_add_dynamic_sphere(x, y, 150.0f, 10.0f, 5.0f);
        for (int i = 0; i < 240; ++i) aver_phys_step(kDt);
        aver_phys_body_position(ball, p[k]);
    }
    check(p[0][2] > -50.0f && p[0][2] < 150.0f, std::string(what) + ": the ball on the original rests on it (z " + std::to_string(p[0][2]) + ")");
    const float dz = std::fabs(p[0][2] - p[1][2]), dy = std::fabs(p[0][1] - p[1][1]), dx = std::fabs(p[0][0] - p[1][0]);
    check(dz < 0.5f && dy < 0.5f && dx < 0.5f, std::string(what) + ": and the ball on the restored shape ends in the same place (dz " + std::to_string(dz) + ")");
}

static void testMeshRoundTrip() {
    AVER_INFO("=== a restored mesh shape is the original ===");
    freshWorld();
    const Mesh m = bumpyMesh(31, 20.0f);
    const int32_t orig = makeMesh(m);
    check(orig != 0, "the mesh shape builds");
    const std::vector<uint8_t> blob = blobOf(orig, kKey);
    check(blob.size() > 48, "it saves: " + std::to_string(blob.size()) + " bytes for " + std::to_string(m.idx.size() / 3) + " triangles");
    check(aver_phys_shape_save(orig, kKey, nullptr, 0) == static_cast<int32_t>(blob.size()), "NULL out asks for the size");
    std::vector<uint8_t> small(blob.size() - 1);
    check(aver_phys_shape_save(orig, kKey, small.data(), static_cast<int32_t>(small.size())) == 0, "a buffer one byte short is refused, not cut");

    const int32_t back = aver_phys_shape_restore(blob.data(), static_cast<int32_t>(blob.size()), kKey);
    check(back != 0 && back != orig, "it restores to a new handle");
    check(blobOf(back, kKey) == blob, "and the restored shape saves to the SAME BYTES as the original");

    const int32_t a = place(orig, 0, 0, 0), b = place(back, 5000.0f, 0, 0);
    check(a != 0 && b != 0, "both place as static bodies");
    compareBounds("mesh", a, b, 5000.0f);
    compareRays("mesh", 0, 5000.0f, -250.0f, 250.0f, -250.0f, 250.0f, 0.05f);
    compareDropFresh("mesh", m, blob, 60.0f, 40.0f);
    aver_phys_shutdown();
}

static void testHeightfieldRoundTrip() {
    AVER_INFO("=== a restored heightfield is the original ===");
    freshWorld();
    const int n = 65;
    const std::vector<float> s = bumpySamples(n);
    const int32_t orig = aver_phys_create_heightfield_shape(s.data(), n, 25.0f);
    check(orig != 0, "the heightfield shape builds (aver_phys_create_heightfield_shape)");
    check(aver_phys_create_heightfield_shape(nullptr, n, 25.0f) == 0 && aver_phys_create_heightfield_shape(s.data(), 1, 25.0f) == 0 &&
          aver_phys_create_heightfield_shape(s.data(), n, 0.0f) == 0, "a null grid, one sample and no spacing are refused");
    const std::vector<uint8_t> blob = blobOf(orig, kKey);
    check(blob.size() > 48, "it saves: " + std::to_string(blob.size()) + " bytes for " + std::to_string(n * n) + " samples");
    const int32_t back = aver_phys_shape_restore(blob.data(), static_cast<int32_t>(blob.size()), kKey);
    check(back != 0, "it restores");
    // The user data (the edit tag) is the one thing that differs between the two shapes, so compare behaviour and the sizes.
    const std::vector<uint8_t> again = blobOf(back, kKey);
    check(again.size() == blob.size(), "the restored shape saves to a blob of the same size");

    // The field covers x in [-(n-1)*25, 0], y in [0, (n-1)*25] when placed at the origin.
    const int32_t a = place(orig, 0, 0, 0), b = place(back, 5000.0f, 0, 0);
    check(a != 0 && b != 0, "both place");
    compareBounds("heightfield", a, b, 5000.0f);
    compareRays("heightfield", 0, 5000.0f, -1400.0f, -200.0f, 200.0f, 1400.0f, 0.05f);
    compareDrop("heightfield", 0, 5000.0f, -700.0f, 700.0f);

    // The original was built here, so its metadata exists and an edit works; the restored one has none and is refused.
    float h[4] = {10.0f, 10.0f, 10.0f, 10.0f};
    check(aver_phys_heightfield_set_heights(a, 4, 4, 2, 2, h, 0) == 1, "the built field is still editable");
    check(aver_phys_heightfield_set_heights(b, 4, 4, 2, 2, h, 0) == 0, "the restored field is a plain collision shape: set_heights refuses it");
    aver_phys_shutdown();
}

// ---- 2. every bad blob is a miss -----------------------------------------------------------------------------------------

static void testMisses() {
    AVER_INFO("=== wrong key, version, build, magic, size and a damaged byte are all misses ===");
    freshWorld();
    const Mesh m = bumpyMesh(11, 20.0f);
    const int32_t orig = makeMesh(m);
    const std::vector<uint8_t> blob = blobOf(orig, kKey);
    const int32_t sz = static_cast<int32_t>(blob.size());
    check(!blob.empty(), "a blob to damage");

    g_warnings = 0;
    setLogSink(&warnSink, nullptr);
    check(aver_phys_shape_restore(blob.data(), sz, kKey + 1) == 0, "a wrong key");
    check(aver_phys_shape_restore(blob.data(), sz, 0) == 0, "key 0 is not the key");
    {
        std::vector<uint8_t> b = blob;
        b[4] ^= 0x7F;    // the cache version
        check(aver_phys_shape_restore(b.data(), sz, kKey) == 0, "a different cache version");
    }
    {
        std::vector<uint8_t> b = blob;
        b[8] = b[8] == 4 ? 8 : 4;    // sizeof(JPH::Real)
        check(aver_phys_shape_restore(b.data(), sz, kKey) == 0, "the other precision");
    }
    {
        std::vector<uint8_t> b = blob;
        b[12] ^= 0x01;   // Jolt major
        check(aver_phys_shape_restore(b.data(), sz, kKey) == 0, "a different Jolt version");
    }
    {
        std::vector<uint8_t> b = blob;
        b[0] = 'X';
        check(aver_phys_shape_restore(b.data(), sz, kKey) == 0, "a foreign magic");
    }
    check(aver_phys_shape_restore(blob.data(), sz - 1, kKey) == 0, "one byte short");
    check(aver_phys_shape_restore(blob.data(), sz / 2, kKey) == 0, "half the file");
    check(aver_phys_shape_restore(blob.data(), 10, kKey) == 0, "shorter than the header");
    check(aver_phys_shape_restore(blob.data(), 0, kKey) == 0 && aver_phys_shape_restore(nullptr, sz, kKey) == 0, "empty and null");
    {
        std::vector<uint8_t> b = blob;
        b.push_back(0);
        check(aver_phys_shape_restore(b.data(), sz + 1, kKey) == 0, "one byte long");
    }
    for (const size_t at : {size_t{48}, size_t{49}, blob.size() / 2, blob.size() - 1}) {
        std::vector<uint8_t> b = blob;
        b[at] ^= 0x10;
        check(aver_phys_shape_restore(b.data(), sz, kKey) == 0, "one damaged payload byte at " + std::to_string(at) + " (the checksum catches it)");
    }
    setLogSink(nullptr, nullptr);
    check(g_warnings == 0, "and none of those misses wrote a warning or an error to the log (" + std::to_string(g_warnings.load()) + ")");

    const int32_t ok = aver_phys_shape_restore(blob.data(), sz, kKey);
    check(ok != 0, "the good blob still restores afterwards");

    // A restore needs a world: without one it is a miss too.
    aver_phys_shutdown();
    check(aver_phys_shape_restore(blob.data(), sz, kKey) == 0, "no world: a miss");
}

// ---- 3. what cannot be saved ---------------------------------------------------------------------------------------------

static void testRefusals() {
    AVER_INFO("=== what the cache refuses to save ===");
    freshWorld();
    const int32_t box = aver_phys_shape_box(10.0f, 10.0f, 10.0f, 0, nullptr, 0);
    check(box != 0, "a box shape builds");
    check(aver_phys_shape_save(box, kKey, nullptr, 0) == 0, "a box is not a mesh or heightfield: not saved");
    check(aver_phys_shape_save(0, kKey, nullptr, 0) == 0 && aver_phys_shape_save(987654, kKey, nullptr, 0) == 0, "a handle that is no shape: not saved");

    const Mesh m = bumpyMesh(5, 30.0f);
    const uint8_t tri0[32] = {};
    const int32_t pal[1] = {0};
    const int32_t withPalette = aver_phys_shape_mesh(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(), static_cast<int32_t>(m.idx.size()),
                                                     tri0, pal, 1, nullptr, nullptr, 0);
    check(withPalette != 0, "a mesh with a surface palette builds");
    check(aver_phys_shape_save(withPalette, kKey, nullptr, 0) == 0, "and is not saved (a palette names live surface ids)");

    std::vector<int32_t> user(m.idx.size() / 3, 7);
    const int32_t withUser = aver_phys_shape_mesh(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(), static_cast<int32_t>(m.idx.size()),
                                                  nullptr, nullptr, 0, user.data(), nullptr, 0);
    check(withUser != 0 && aver_phys_shape_save(withUser, kKey, nullptr, 0) == 0, "nor is one with per-triangle user data");

    const int32_t plain = makeMesh(m);
    check(aver_phys_shape_save(plain, kKey, nullptr, 0) > 0, "while the same mesh with neither is");
    aver_phys_shutdown();
}

// ---- 4. threads, size, switch --------------------------------------------------------------------------------------------

static void testThreadsAndSize() {
    AVER_INFO("=== two threads restore at once; a blob is bounded ===");
    freshWorld();
    const Mesh m = bumpyMesh(41, 20.0f);
    const int32_t orig = makeMesh(m);
    const std::vector<uint8_t> blob = blobOf(orig, kKey);

    std::atomic<int> good{0};
    std::vector<int32_t> handles(8, 0);
    auto work = [&](int first) {
        for (int i = 0; i < 4; ++i) {
            const int32_t h = aver_phys_shape_restore(blob.data(), static_cast<int32_t>(blob.size()), kKey);
            handles[static_cast<size_t>(first + i)] = h;
            if (h) ++good;
        }
    };
    std::thread t1(work, 0), t2(work, 4);
    t1.join();
    t2.join();
    check(good == 8, "eight restores from two threads all succeed");
    bool distinct = true;
    for (size_t i = 0; i < handles.size(); ++i)
        for (size_t j = i + 1; j < handles.size(); ++j) distinct = distinct && handles[i] != handles[j];
    check(distinct, "with eight different handles");
    bool same = true;
    for (const int32_t h : handles) same = same && blobOf(h, kKey) == blob;
    check(same, "and every one saves back to the original bytes");

    // The triangle list is vertices (12 bytes) plus indices (12 bytes a triangle). Jolt's BVH copy is a few times that; saving the
    // triangles a second time as well would push it well past the bound. UNVERIFIED bound, tuned by the first run.
    const size_t raw = m.v.size() * sizeof(float) + m.idx.size() * sizeof(int32_t);
    check(blob.size() < raw * 4, "the blob is " + std::to_string(blob.size()) + " bytes for " + std::to_string(raw) + " bytes of triangles (bound 4x)");
    aver_phys_shutdown();
}

static void testSwitchAndVersion() {
    AVER_INFO("=== the switch and the version ===");
    aver_phys_shape_cache_enable(0);
    check(aver_phys_shape_cache_enabled() == 0, "the cache switch is off when set off");
    aver_phys_shape_cache_enable(1);
    check(aver_phys_shape_cache_enabled() == 1, "and on when set on");
    aver_phys_shape_cache_enable(0);
    check(aver_phys_shape_cache_version() == AVER_PHYS_SHAPECACHE_VERSION && aver_phys_shape_cache_version() >= 1, "the cache version is the header's");
}

int main() {
    AVER_INFO("=== PhysicsShapeCacheTest ===");
    testMeshRoundTrip();
    testHeightfieldRoundTrip();
    testMisses();
    testRefusals();
    testThreadsAndSize();
    testSwitchAndVersion();
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
