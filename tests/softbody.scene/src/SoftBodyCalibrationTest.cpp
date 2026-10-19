// The preset calibration scenarios of docs/plans/wave10/SOFTBODY_DEPTH.md 6.2, headless: a fixed energy ladder against a steel door,
// a glass pane, a plank crate, and a flag in the wind. It PRINTS the measured table (dent depth, broken beams, pieces, strain) so a
// tuning pass has numbers to move, and asserts only coarse brackets so a later change cannot silently move a preset out of its
// intended class: the monotone ones (more energy never does less) and the two the design states outright (metal at 90 J dents and
// does not tear; glass at 1 J does nothing and at 90 J makes at least four pieces). Every preset number is an UNVERIFIED starting
// value until the owner's headed pass, so the printed table is the product and the asserts are guard rails.
#include "SoftBodyFixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

using namespace sbfix;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

constexpr f32 kLadder[5] = {5.0f, 25.0f, 90.0f, 400.0f, 3000.0f};
constexpr u64 kDoor = 601, kGlassPane = 602, kCrate = 603, kFlag = 604, kDoorAsset = 701, kGlassAsset = 702, kCrateAsset = 703;

// A closed cube of `size` cm, n x n quads a face, vertices shared across edges, outward winding.
static fmt::OcMeshData boxMesh(u32 n, f32 size) {
    fmt::OcMeshData m;
    std::map<std::tuple<long, long, long>, u32> seen;
    const f32 h = size * 0.5f;
    auto vert = [&](const Vec3& p) {
        const auto key = std::make_tuple(std::lround(p.x * 1000.0f), std::lround(p.y * 1000.0f), std::lround(p.z * 1000.0f));
        const auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        const u32 id = static_cast<u32>(m.positions.size() / 3);
        seen.emplace(key, id);
        m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
        m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
        m.uvs.insert(m.uvs.end(), {(p.x + h) / size, (p.y + h) / size});
        return id;
    };
    struct Face { Vec3 origin, du, dv; };
    const Face faces[6] = {
        {{-h, -h, h}, {size, 0, 0}, {0, size, 0}},    // +Z
        {{h, -h, -h}, {-size, 0, 0}, {0, size, 0}},   // -Z
        {{h, -h, -h}, {0, size, 0}, {0, 0, size}},    // +X
        {{-h, -h, -h}, {0, 0, size}, {0, size, 0}},   // -X
        {{-h, h, -h}, {0, 0, size}, {size, 0, 0}},    // +Y
        {{-h, -h, -h}, {size, 0, 0}, {0, 0, size}},   // -Y
    };
    for (const Face& f : faces)
        for (u32 j = 0; j < n; ++j)
            for (u32 i = 0; i < n; ++i) {
                auto at = [&](u32 a, u32 b) { return vert(f.origin + f.du * (static_cast<f32>(a) / static_cast<f32>(n)) + f.dv * (static_cast<f32>(b) / static_cast<f32>(n))); };
                const u32 p00 = at(i, j), p10 = at(i + 1, j), p11 = at(i + 1, j + 1), p01 = at(i, j + 1);
                for (const u32 k : {p00, p10, p11, p00, p11, p01}) m.indices.push_back(k);
            }
    m.computeBounds();
    return m;
}

struct Outcome {
    f32 dentCm = 0.0f;       // the largest displacement of any particle from where it was built, once settled
    f32 setCm = 0.0f;        // the largest permanent set of any edge
    u32 broken = 0, pieces = 0;
    f32 maxStrain = 0.0f;
};

static Outcome measure(const Rig& r, scene::Entity e, f32 restY) {
    Outcome o;
    if (const sb::Cage* c = r.sys.cageOf(e)) {
        for (const sb::Particle& p : c->particles) o.dentCm = std::max(o.dentCm, std::fabs(p.pos.y - restY));
        for (const sb::Beam& b : c->beams) o.setCm = std::max(o.setCm, b.plastic);
        o.broken = sb::brokenBeamCount(*c);
    }
    o.pieces = r.sys.pieceCount(e);
    o.maxStrain = r.sys.state(e).maxStrain;
    return o;
}

static void print(const char* label, f32 energy, const Outcome& o) {
    AVER_INFO("  {:<6} {:>6.0f} J   dent {:>7.2f} cm   set {:>6.2f} cm   broken {:>4}   pieces {:>3}   strain {:.4f}", label, static_cast<double>(energy),
              static_cast<double>(o.dentCm), static_cast<double>(o.setCm), o.broken, o.pieces, static_cast<double>(o.maxStrain));
}

static Outcome hit(sb::SoftPreset preset, u64 mesh, const sb::SoftAsset& asset, Vec3 at, f32 energy, f32 seconds, f32 restY, f32 radius = 40.0f) {
    Rig r;
    addFloor();
    r.stores.meshes[mesh] = mesh == kCrate ? boxMesh(6, 80.0f) : (mesh == kDoor ? sheetMesh(11, 22, 10.0f, Plane::XZ) : sheetMesh(13, 19, 10.0f, Plane::XZ));
    r.stores.assets[kDoorAsset] = asset;
    const f32 z = mesh == kCrate ? 41.0f : 400.0f;
    const scene::Entity e = r.entity("target", mesh, Vec3{0, 0, z});
    sb::SoftBodySystem::attachDestructible(r.w, e, preset, kDoorAsset);
    r.run(1.0f);
    r.sys.damage(e, at + Vec3{0, 0, z}, radius, energy, Vec3{0, 1, 0}, 2);
    r.run(seconds);
    return measure(r, e, restY);
}

static void testSteelDoor() {
    AVER_INFO("-- steel door (metal): the energy ladder, hung by its top edge --");
    sb::SoftAsset door = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
    Outcome out[5];
    for (int i = 0; i < 5; ++i) {
        out[i] = hit(sb::SoftPreset::Metal, kDoor, door, Vec3{50, 0, -105}, kLadder[i], 4.0f, 0.0f);
        print("door", kLadder[i], out[i]);
    }
    check(out[0].broken == 0, "5 J breaks nothing");
    check(out[2].broken == 0 && out[2].dentCm > 0.5f && out[2].dentCm < 8.0f, "90 J dents it between 0.5 and 8 cm and does not tear it");
    check(out[4].dentCm >= out[2].dentCm || out[4].broken > out[2].broken, "3000 J does at least as much as 90 J (a deeper dent or a tear)");
    bool monotone = true;
    for (int i = 1; i < 5; ++i) monotone = monotone && (out[i].setCm + 0.25f >= out[i - 1].setCm || out[i].broken >= out[i - 1].broken);
    check(monotone, "more energy never leaves less permanent set (unless it tore)");
}

static void testGlassPane() {
    AVER_INFO("-- glass pane: the energy ladder --");
    sb::SoftAsset glass = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    glass.destruct.minPieceTriangles = 3;
    glass.destruct.maxPieces = 24;
    const f32 ladder[5] = {1.0f, 25.0f, 90.0f, 400.0f, 3000.0f};
    Outcome out[5];
    for (int i = 0; i < 5; ++i) {
        out[i] = hit(sb::SoftPreset::Glass, kGlassPane, glass, Vec3{60, 0, -90}, ladder[i], 2.0f, 0.0f);
        print("glass", ladder[i], out[i]);
    }
    check(out[0].broken == 0 && out[0].pieces == 1, "1 J does nothing");
    check(out[2].pieces >= 4, "90 J makes at least four pieces");
    check(out[4].broken >= out[2].broken, "3000 J breaks at least as many edges as 90 J");
}

static void testPlankCrate() {
    AVER_INFO("-- plank crate (wood): the energy ladder at the middle of the top face --");
    sb::SoftAsset crate = sb::makeSoftAsset(sb::SoftKind::Destructible, sb::SoftPreset::Wood);
    crate.destruct.minImpactCmS = 180.0f;
    crate.destruct.maxPieces = 16;
    Outcome out[5];
    for (int i = 0; i < 5; ++i) {
        out[i] = hit(sb::SoftPreset::Wood, kCrate, crate, Vec3{0, 0, 40}, kLadder[i], 2.5f, 0.0f);
        print("crate", kLadder[i], out[i]);
    }
    check(out[0].broken <= out[4].broken, "3000 J breaks at least as many edges as 5 J");
}

static void testFlag() {
    AVER_INFO("-- flag (cloth light): wind 300 cm/s with gust 120 --");
    Rig r;
    r.stores.meshes[kFlag] = sheetMesh(24, 16, 6.0f, Plane::YZ);
    const scene::Entity flag = r.entity("flag", kFlag, Vec3{0, 0, 300});
    sb::SoftBodySystem::attachCloth(r.w, flag, sb::SoftPreset::ClothLight);
    r.sys.setWind(Vec3{300, 0, 0}, 120.0f, 1.0f, 0.0f);
    r.run(3.0f);
    f32 lo = 1e30f, hi = -1e30f, strain = 0.0f;
    for (int i = 0; i < 120; ++i) {
        r.frame();
        const auto& ps = r.particles(flag);
        const f32 x = ps.back().pos.x;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        strain = std::max(strain, r.sys.state(flag).maxStrain);
    }
    AVER_INFO("  flag   tip travels {:.1f} cm over two seconds of gusts, peak strain {:.4f}", static_cast<double>(hi - lo), static_cast<double>(strain));
    check(hi - lo > 1.0f, "the tip moves in the gusts");
    check(strain < 0.1f, "and the cloth does not stretch past 10 percent");
}

int main() {
    AVER_INFO("SoftBodyCalibrationTest");
    testSteelDoor();
    testGlassPane();
    testPlankCrate();
    testFlag();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
