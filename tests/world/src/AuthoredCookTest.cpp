// The cook: LEVELINST flattened in f64, an authored level classified, owned by exact chunks, written as packed regions
// (LARGE_WORLD 2.4.5, tests 5.1 AuthoredCookTest). Exit code = failure count.
//
//   FLATTEN       an instance a hundred thousand kilometres out composes in f64 (rotation, offset, parents untouched), overrides
//                 apply, a missing child is a warning, a cycle and a nest past the depth cap are refused, ids are unique.
//   CLASSIFY      class, vehicle, sequence-target, unpackable and unbounded roots stay level-global; a reach past half the wide
//                 threshold lists the root (a child's reach counts for its root); everything else is chunk-owned.
//   OWNERSHIP     the owner chunk is the root's f64 position split once: negative and far positions land in the exact chunk with
//                 the exact local; children travel with their root.
//   PAYLOAD       rotation is quatFromEulerDeg of the f32 casts (bit for bit); PLID, ANIM, COLL, CMPN, LABL, SNAP and EULR round trip.
//   DISK          the set reads back through AuthoredChunkSource: index kind and hash, wide list, every payload byte for byte, the
//                 occupancy query; two cooks are byte-identical; a re-cook replaces the set and leaves no temporary directory.
//   SLIM LEVEL    only level-global placements, sequence targets renumbered, lights composed out of an instance, no LEVELINST.
//   STALENESS     the source hash moves with a placement, the rest of the level, the instanced level's content and the options.
//   SCALE         a 20,000-placement slice cooks, loses nothing, and is timed.
// No GPU, no window, no .NET.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Time.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/world/AuthoredChunkSource.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkCook.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/LevelAssetLibrary.hpp"
#include "aver/world/LevelFlatten.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;
static std::string why;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static void checkWhy(bool cond, const std::string& what) { check(cond, cond ? what : what + ": " + why); }

static bool isNear(f64 a, f64 b, f64 tol) { return std::fabs(a - b) <= tol; }

// ---- fixtures ---------------------------------------------------------------------------------------------------------

static const char* kBox = "Meshes/Box.ocmesh";            // local bounds +-100 cm
static const char* kMissing = "Meshes/Missing.ocmesh";     // no bounds anywhere

static fmt::OcWorldPlacement place(const std::string& asset, f64 x, f64 y, f64 z, u64 id) {
    fmt::OcWorldPlacement p;
    p.asset = asset;
    p.objectId = fnv1a64(std::string_view(asset));
    p.x = x; p.y = y; p.z = z;
    p.placementId = id;
    return p;
}

static CookInput inputFor(LevelAssetLibrary* lib, const std::string& projectDir = std::string()) {
    CookInput in;
    in.levelAsset = "Maps/Test.ocworld";
    in.levelStem = "Test";
    in.projectDir = projectDir;
    in.library = lib;
    in.meshBounds = [](const std::string& asset, Vec3& lo, Vec3& hi) {
        if (asset == kBox) { lo = Vec3{-100, -100, -100}; hi = Vec3{100, 100, 100}; return true; }
        return false;   // everything else falls to the built-in unit meshes, then to "unknown"
    };
    return in;
}

static fmt::OcPrefabOverride removedOverride(u64 id) {
    fmt::OcPrefabOverride ov;
    ov.op = fmt::OcOverrideOp::Set;
    ov.path = fmt::ocLevelInstIdText(id);
    ov.component = fmt::kOcLevelInstPlacementComponent;
    ov.value.name = fmt::kLiFieldRemoved;
    ov.value.kind = fmt::kOcPrefabKindBool;
    ov.value.i = 1;
    return ov;
}

static std::map<std::string, fmt::OcWorldData> g_files;

static void installLoader(LevelAssetLibrary& lib) {
    lib.setLoader([](const std::string& path, fmt::OcWorldData& out, std::string* err) {
        const auto it = g_files.find(path);
        if (it == g_files.end()) { if (err) *err = "no such level " + path; return false; }
        out = it->second;
        return true;
    });
}

static std::vector<u8> readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::set<std::string> filesIn(const std::filesystem::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) out.insert(e.path().filename().string());
    return out;
}

// ---- the level most checks use ------------------------------------------------------------------------------------------

namespace Idx { enum : int { Plain = 0, NegEdge, NegInside, Far, Rotated, Parent, ChildOfParent, Class, Vehicle, Seq, Unbounded,
                             Wide, Anim, Labelled, Count }; }

static constexpr u32 kPlacementCount = static_cast<u32>(Idx::Count);

static fmt::OcWorldData makeLevel() {
    fmt::OcWorldData w;
    w.name = "Test";
    w.stream.enabled = true;
    w.stream.cellCm = 3200.0f;
    w.stream.loadCm = 20000.0f;
    w.stream.evictCm = 25000.0f;
    w.hasSpawn = true;
    w.spawnX = 10; w.spawnY = 20; w.spawnZ = 30;

    auto add = [&](fmt::OcWorldPlacement p) { w.placements.push_back(std::move(p)); };
    add(place(kBox, 100, 200, 50, 0xA0));                       // Plain
    add(place(kBox, -1600.0, 3200.0, 0, 0xA1));                 // NegEdge: exactly on the -1/-2 border -> chunk -1, local 0
    add(place(kBox, -1600.5, -0.25, -20, 0xA2));                // NegInside: chunk -2, local 1599.5
    add(place(kBox, 123456789.25, -987654321.5, 40, 0xA3));     // Far
    {
        fmt::OcWorldPlacement p = place(kBox, 500, 500, 500, 0xA4);   // Rotated
        p.yaw = 33.3; p.pitch = 12.5; p.roll = -7.25;
        p.sx = 2; p.sy = 3; p.sz = 4;
        add(p);
    }
    add(place(kBox, 5000, 5000, 0, 0xA5));                      // Parent (small)
    {
        fmt::OcWorldPlacement c = place(kBox, 5000, 0, 0, 0xA6);      // child 5000 cm out: reach 5100 > 3200 -> the ROOT is wide
        c.parent = Idx::Parent;
        add(c);
    }
    {
        fmt::OcWorldPlacement p = place(kBox, 7000, 7000, 0, 0xA7);   // Class
        p.className = "Barrel";
        add(p);
    }
    {
        fmt::OcWorldPlacement p = place(kBox, 8000, 8000, 0, 0xA8);   // Vehicle
        p.vehiclePreset = "car";
        add(p);
    }
    add(place(kBox, 9000, 9000, 0, 0xA9));                      // Seq (a sequence track targets it)
    add(place(kMissing, 1000, 1000, 0, 0xAA));                  // Unbounded
    {
        fmt::OcWorldPlacement p = place(kBox, 12000, 12000, 0, 0xAB);  // Wide: 100 cm box scaled 100x -> 10,000 cm reach
        p.sx = p.sy = p.sz = 100;
        add(p);
    }
    {
        fmt::OcWorldPlacement p = place(kBox, 2000, 2000, 0, 0xAC);    // Anim
        p.animClip = "Anims/Spin.ocanim"; p.animSpeed = 0.5f; p.animTime = 1.25f; p.animOnce = true;
        add(p);
    }
    {
        fmt::OcWorldPlacement p = place(kBox, 2400, 2000, 0, 0xAD);    // Labelled: label, snap, nocollide, hidden, a component
        p.name = "Front Door";
        p.snapToGround = true;
        p.collide = false;
        p.visible = false;
        fmt::OcPlacedComponent c;
        c.type = "Light";
        c.kv = {{"color", "1 0.5 0.25"}, {"intensity", "800"}};
        p.components.push_back(c);
        fmt::OcPlacedComponent c2;
        c2.type = "Tag";
        c2.kv = {{"name", "door"}};
        p.components.push_back(c2);
        add(p);
    }

    fmt::OcSequence seq;
    seq.name = "Intro";
    fmt::OcSeqTrack tr;
    tr.kind = fmt::OcSeqTrackKind::Transform;
    tr.target = Idx::Seq;
    seq.tracks.push_back(tr);
    fmt::OcSeqTrack camTrack;
    camTrack.kind = fmt::OcSeqTrackKind::Camera;
    camTrack.target = -1;
    seq.tracks.push_back(camTrack);
    w.sequences.push_back(seq);

    fmt::OcLight l;
    l.name = "TopLight";
    l.x = 10; l.y = 20; l.z = 30;
    w.lights.push_back(l);
    return w;
}

static const ChunkPayload* findChunk(const std::vector<ChunkPayload>& all, const ChunkCoord& c) {
    for (const ChunkPayload& p : all) if (p.coord == c) return &p;
    return nullptr;
}

// The payload entity with placement id `id`, or null.
static const PayloadEntity* byId(const std::vector<ChunkPayload>& all, u64 id) {
    for (const ChunkPayload& p : all)
        for (const PayloadEntity& e : p.entities) {
            PlacementExtras x;
            if (getPlacementExtras(e, x) && x.hasPlacementId && x.placementId == id) return &e;
        }
    return nullptr;
}

static std::vector<ChunkPayload> allPayloads(const CookPlan& plan) {
    std::vector<ChunkPayload> out;
    for (usize i = 0; i < plan.chunks.size(); ++i) out.push_back(plan.payload(i));
    return out;
}

// ---- 1: flatten -----------------------------------------------------------------------------------------------------------

static void flattenChecks() {
    AVER_INFO("-- flatten");
    g_files.clear();

    fmt::OcWorldData child;
    child.name = "Child";
    child.placements.push_back(place(kBox, 100, 0, 0, 0xC1));          // C: 100 cm along the instance's +X
    {
        fmt::OcWorldPlacement d = place(kBox, 0, 10, 0, 0xC2);         // D: child of C
        d.parent = 0;
        child.placements.push_back(d);
    }
    child.placements.push_back(place(kBox, 300, 0, 0, 0xC3));          // E: removed by the instance's override
    {
        fmt::OcLight l;
        l.name = "ChildLight";
        l.x = 50; l.y = 0; l.z = 0;
        child.lights.push_back(l);
    }
    g_files["Maps/Child.ocworld"] = child;

    fmt::OcWorldData top;
    top.name = "Top";
    top.placements.push_back(place(kBox, 100, 200, 0, 0x10));
    fmt::OcLevelInstance rec;
    rec.asset = "Maps/Child.ocworld";
    rec.x = 1.0e8; rec.y = -1.0e8; rec.z = 0;
    rec.yaw = 90;
    rec.id = 0x77;
    rec.overrides.push_back(removedOverride(0xC3));
    top.levelInstances.push_back(rec);

    LevelAssetLibrary lib;
    installLoader(lib);
    FlatLevel fl;
    checkWhy(flattenLevelInstances(top, "Maps/Test.ocworld", lib, fl, &why), "a level with one instance flattens");
    check(fl.placements.size() == 3, "the level's one placement + C and D (E was removed by the override)");
    check(fl.instances == 1 && fl.deepest == 1 && fl.unloadedInstances == 0, "one instance expanded, one level deep");
    if (fl.placements.size() == 3) {
        const FlatPlacement& a = fl.placements[0];
        check(a.topIndex == 0 && a.depth == 0 && !a.composed && a.pos[0] == 100.0 && a.pos[1] == 200.0, "the level's own placement is untouched");
        const FlatPlacement& c = fl.placements[1];
        const FlatPlacement& d = fl.placements[2];
        check(c.composed && c.depth == 1 && c.topIndex == -1, "C came out of the instance and carries the composed pose");
        // yaw 90 turns the instance's +X into world +Y: C = (1e8, -1e8 + 100, 0), in f64 at 10^8 cm.
        check(isNear(c.pos[0], 1.0e8, 1e-3) && isNear(c.pos[1], -1.0e8 + 100.0, 1e-3) && isNear(c.pos[2], 0.0, 1e-3),
              "C's world position is the instance + rotated offset, exact in f64 at 10^8 cm");
        check(isNear(c.yaw, 90.0f, 1e-3) && isNear(c.pitch, 0.0f, 1e-3) && isNear(c.roll, 0.0f, 1e-3), "C's rotation is the instance's, as Euler degrees");
        check(d.parent == 1 && !d.composed && d.pos[1] == 10.0 && d.pos[0] == 0.0, "D keeps its authored parent-relative pose under C");
        std::set<u64> ids;
        for (const FlatPlacement& fp : fl.placements) ids.insert(fp.id);
        check(ids.size() == 3 && ids.count(0) == 0, "ids are unique and non-zero");
        check(a.id == 0x10, "a level's own authored id is kept");
    }
    check(fl.lights.size() == 1 && isNear(fl.lights[0].x, 1.0e8, 1e-3) && isNear(fl.lights[0].y, -1.0e8 + 50.0, 1e-3),
          "the instance's light is composed into the world frame");

    // a missing child is a warning, as at runtime
    {
        fmt::OcWorldData t2 = top;
        fmt::OcLevelInstance gone = rec;
        gone.asset = "Maps/Nowhere.ocworld";
        gone.id = 0x78;
        t2.levelInstances.push_back(gone);
        LevelAssetLibrary lib2;
        installLoader(lib2);
        FlatLevel f2;
        check(flattenLevelInstances(t2, "Maps/Test.ocworld", lib2, f2, &why) && f2.unloadedInstances == 1 && !f2.warnings.empty(),
              "an instance that does not load is a warning and the rest still flattens");
    }
    // a cycle
    {
        fmt::OcWorldData a;
        fmt::OcLevelInstance back;
        back.asset = "Maps/Test.ocworld";
        back.id = 1;
        a.levelInstances.push_back(back);
        g_files["Maps/A.ocworld"] = a;
        fmt::OcWorldData t3;
        fmt::OcLevelInstance into;
        into.asset = "Maps/A.ocworld";
        into.id = 2;
        t3.levelInstances.push_back(into);
        LevelAssetLibrary lib3;
        installLoader(lib3);
        FlatLevel f3;
        why.clear();
        check(!flattenLevelInstances(t3, "Maps/Test.ocworld", lib3, f3, &why) && why.find("->") != std::string::npos,
              "a level that instances itself through a child is refused with the chain: " + why);
    }
    // a nest past the depth cap
    {
        for (int i = 0; i < 12; ++i) {
            fmt::OcWorldData n;
            n.placements.push_back(place(kBox, 1, 1, 1, 0x500 + static_cast<u64>(i)));
            fmt::OcLevelInstance next;
            next.asset = "Maps/N" + std::to_string(i + 1) + ".ocworld";
            next.id = 0x600 + static_cast<u64>(i);
            n.levelInstances.push_back(next);
            g_files["Maps/N" + std::to_string(i) + ".ocworld"] = n;
        }
        fmt::OcWorldData t4;
        fmt::OcLevelInstance into;
        into.asset = "Maps/N0.ocworld";
        into.id = 3;
        t4.levelInstances.push_back(into);
        LevelAssetLibrary lib4;
        installLoader(lib4);
        FlatLevel f4;
        why.clear();
        check(!flattenLevelInstances(t4, "Maps/Test.ocworld", lib4, f4, &why) && why.find("deeper") != std::string::npos,
              "a nest deeper than the cap is refused: " + why);
    }
}

// ---- 2..4: classify, ownership, payload ----------------------------------------------------------------------------------------

static void planChecks() {
    AVER_INFO("-- classify, ownership, payload");
    g_files.clear();
    const fmt::OcWorldData level = makeLevel();
    LevelAssetLibrary lib;
    installLoader(lib);
    const CookInput in = inputFor(&lib);
    const CookOptions opt;
    CookPlan plan;
    CookReport rep;
    checkWhy(planAuthoredCook(level, in, opt, plan, rep, &why), "planAuthoredCook succeeds");
    check(rep.placementsIn == kPlacementCount && rep.flatPlacements == kPlacementCount && rep.instances == 0, "all placements counted, no instances");

    // Level-global roots by reason: class, vehicle, sequence target, unbounded. (Unpackable has its own check below.)
    check(rep.globalClass == 1 && rep.globalVehicle == 1 && rep.globalSequence == 1 && rep.globalUnbounded == 1 && rep.globalUnpackable == 0,
          "one root each for class, vehicle, sequence target and unbounded mesh");
    check(rep.levelGlobal == 4, "four placements stay level-global");
    check(rep.chunkOwned == kPlacementCount - 4 && rep.chunkOwnedRoots == kPlacementCount - 4 - 1, "the rest are chunk-owned (the child counts with its root)");
    check(rep.unknownBoundsAssets.size() == 1 && rep.unknownBoundsAssets[0] == kMissing, "the mesh with no bounds is named");
    check(rep.wide == 2 && plan.wide.size() == 2, "two roots are wide: the 100x box and the parent whose child reaches 5100 cm");
    check(rep.beyondSidecarLimit == 1, "one chunk-owned root is beyond +-1,048,576 cm of world (0, 0)");

    const std::vector<ChunkPayload> all = allPayloads(plan);
    usize entities = 0;
    for (const ChunkPayload& p : all) entities += p.entities.size();
    check(entities == rep.chunkOwned, "the payloads hold exactly the chunk-owned placements");
    check(plan.chunks.size() == all.size() && rep.chunks == all.size(), "one payload per chunk");

    // ---- ownership: the f64 position split once ----
    const auto chunkOf = [&](u64 id) {
        for (const ChunkPayload& p : all)
            for (const PayloadEntity& e : p.entities) {
                PlacementExtras x;
                if (getPlacementExtras(e, x) && x.hasPlacementId && x.placementId == id) return p.coord;
            }
        return ChunkCoord{INT32_MIN, 0, 0};
    };
    check(chunkOf(0xA0) == (ChunkCoord{0, 0, 0}), "(100, 200, 50) is chunk (0, 0, 0)");
    check(chunkOf(0xA1) == (ChunkCoord{-1, 2, 0}), "x = -1600 exactly is chunk -1 with local 0, not -2");
    {
        const PayloadEntity* e = byId(all, 0xA1);
        check(e && e->local.position.x == 0.0f && e->local.position.y == 0.0f, "...and its chunk-local x is exactly 0");
    }
    check(chunkOf(0xA2) == (ChunkCoord{-2, -1, -1}), "(-1600.5, -0.25, -20) is chunk (-2, -1, -1)");
    {
        const PayloadEntity* e = byId(all, 0xA2);
        check(e && e->local.position.x == 1599.5f && e->local.position.y == 1599.75f && e->local.position.z == 1580.0f,
              "...with chunk-local (1599.5, 1599.75, 1580) exactly");
    }
    // 123456789.25 / 1600 = 77160.49...; -987654321.5 / 1600 = -617284.2..: both exact in the chunk index and the local.
    check(chunkOf(0xA3) == (ChunkCoord{77160, -617284, 0}), "a root at 10^8 cm lands in the exact chunk");
    {
        const PayloadEntity* e = byId(all, 0xA3);
        const f64 lx = 123456789.25 - 77160.0 * 1600.0;
        const f64 ly = -987654321.5 + 617284.0 * 1600.0;
        // 617284 * 1600 = 987,654,400: y local = 78.5
        check(e && e->local.position.x == static_cast<f32>(lx) && e->local.position.y == static_cast<f32>(ly),
              "...with its exact chunk-local offset (no f32 absolute anywhere): the 10 km quantisation is gone");
        check(e && isNear(lx, 789.25, 0) && isNear(ly, 78.5, 0), "...which is the value worked out by hand");
    }

    // ---- children travel with the root, parent-relative ----
    {
        const ChunkPayload* pc = findChunk(all, chunkOf(0xA5));
        check(pc != nullptr, "the parent's chunk exists");
        if (pc) {
            i32 pi = -1, ci = -1;
            for (usize i = 0; i < pc->entities.size(); ++i) {
                PlacementExtras x;
                getPlacementExtras(pc->entities[i], x);
                if (x.placementId == 0xA5) pi = static_cast<i32>(i);
                if (x.placementId == 0xA6) ci = static_cast<i32>(i);
            }
            check(pi >= 0 && ci > pi && pc->entities[static_cast<usize>(ci)].parent == pi, "the child follows its parent in the same chunk");
            check(ci >= 0 && pc->entities[static_cast<usize>(ci)].local.position.x == 5000.0f &&
                      pc->entities[static_cast<usize>(ci)].local.position.y == 0.0f,
                  "...at its authored parent-relative position (5000 cm is outside the chunk, as parent-relative numbers may be)");
            check(pi >= 0 && pc->entities[static_cast<usize>(pi)].parent == -1, "...and the root has no parent");
        }
    }

    // ---- the rotation is instantiate()'s, bit for bit ----
    {
        const PayloadEntity* e = byId(all, 0xA4);
        const Quat q = quatFromEulerDeg(Vec3{static_cast<f32>(-7.25), static_cast<f32>(12.5), static_cast<f32>(33.3)});
        check(e && e->local.rotation.x == q.x && e->local.rotation.y == q.y && e->local.rotation.z == q.z && e->local.rotation.w == q.w,
              "the payload rotation is quatFromEulerDeg of the f32 casts, bit for bit");
        check(e && e->local.scale.x == 2.0f && e->local.scale.y == 3.0f && e->local.scale.z == 4.0f, "...and the scale is the f32 cast");
        PlacementExtras x;
        check(e && getPlacementExtras(*e, x) && x.hasEuler && x.euler[0] == -7.25f && x.euler[1] == 12.5f && x.euler[2] == static_cast<f32>(33.3),
              "EULR carries the same three floats");
        const PayloadEntity* plain = byId(all, 0xA0);
        PlacementExtras px;
        check(plain && getPlacementExtras(*plain, px) && !px.hasEuler && plain->local.rotation.w == 1.0f, "an unrotated placement writes no EULR");
        check(plain && plain->hasMesh && plain->name == kBox && plain->mesh == fnv1a64(std::string_view(kBox)) &&
                  plain->meshFlags == scene::kMeshRendererVisible,
              "mesh, name (the asset path) and the visible flag are set the way instantiate() sets them");
    }

    // ---- the extras ----
    {
        const PayloadEntity* e = byId(all, 0xAC);
        PlacementExtras x;
        check(e && getPlacementExtras(*e, x) && x.hasAnim && x.animClip == "Anims/Spin.ocanim" && x.animSpeed == 0.5f && x.animTime == 1.25f && x.animOnce,
              "ANIM carries the clip, speed, start time and once");
        check(e && !x.hasCollide && !x.hasLabel && !x.snapToGround, "...and an animated plain placement carries nothing else");
        const PayloadEntity* d = byId(all, 0xAD);
        PlacementExtras y;
        check(d && getPlacementExtras(*d, y) && y.hasCollide && !y.collide, "COLL is written when collide is off");
        check(d && y.hasLabel && y.label == "Front Door" && y.snapToGround, "LABL and SNAP");
        check(d && y.components.size() == 2 && y.components[0].type == "Light" && y.components[0].kv.size() == 2 &&
                  y.components[0].kv[1].first == "intensity" && y.components[0].kv[1].second == "800" && y.components[1].type == "Tag",
              "CMPN keeps the components verbatim, in file order");
        check(d && d->meshFlags == 0u, "a hidden placement has no visible flag");
        const PayloadEntity* a = byId(all, 0xA0);
        PlacementExtras z;
        check(a && getPlacementExtras(*a, z) && z.hasPlacementId && z.placementId == 0xA0 && !z.hasCollide, "COLL is absent for the default (collide)");
    }

    // ---- wide ----
    {
        bool bigBox = false, parentWide = false;
        for (const WideItem& w : plan.wide) {
            if (w.owner == (ChunkCoord{7, 7, 0})) {    // (12000, 12000) -> chunk 7
                bigBox = (w.minCm[0] == 2000 && w.maxCm[0] == 22000 && w.minCm[1] == 2000 && w.maxCm[1] == 22000);
            }
            if (w.owner == (ChunkCoord{3, 3, 0})) {    // (5000, 5000)
                parentWide = (w.minCm[0] == 4900 && w.maxCm[0] == 10100 && w.minCm[1] == 4900 && w.maxCm[1] == 5100);
            }
        }
        check(bigBox, "the 100x box is listed with its owner chunk and an exact i64 AABB (+-10,000 cm)");
        check(parentWide, "the parent is listed with the UNION of its own and its child's box");
    }

    // ---- regions: the far root is in another region file than the near ones ----
    {
        std::set<std::tuple<i32, i32, i32>> regs;
        for (const CookPlan::Region& r : plan.regions) regs.insert({r.coord.x, r.coord.y, r.coord.z});
        check(plan.regions.size() == regs.size() && plan.regions.size() >= 2, "roots 10^8 cm apart are in different regions");
        usize covered = 0;
        bool own = true;
        for (const CookPlan::Region& r : plan.regions) {
            covered += r.end - r.first;
            for (usize i = r.first; i < r.end; ++i)
                if (!(regionOf(plan.chunks[i]) == r.coord)) own = false;
        }
        check(own && covered == plan.chunks.size(), "each region is a contiguous run of exactly its own chunks");
    }

    // ---- an unpackable value keeps its root out of the chunks ----
    {
        fmt::OcWorldData big = makeLevel();
        big.placements[Idx::Plain].name = std::string(70000, 'n');
        CookPlan p2;
        CookReport r2;
        check(planAuthoredCook(big, in, opt, p2, r2, &why) && r2.globalUnpackable == 1, "a 70,000-character label makes its root level-global, not a dropped one");
    }

    // ---- options ----
    {
        CookOptions huge;
        huge.wideCm = 1.0e7;   // clamped to the streamer's pad
        CookPlan p3;
        CookReport r3;
        check(planAuthoredCook(level, in, huge, p3, r3, &why) && r3.wide == 2, "a larger wideCm is clamped: the same two roots are wide");
        CookOptions badSize;
        badSize.chunkSizeCm = 4096;
        CookPlan p4;
        CookReport r4;
        check(!planAuthoredCook(level, in, badSize, p4, r4, &why), "a chunk size outside the precision budget is refused");
        CookOptions fine;
        fine.chunkSizeCm = 800;   // a legal size: every owner chunk is recomputed at 800
        CookPlan p5;
        CookReport r5;
        check(planAuthoredCook(level, in, fine, p5, r5, &why) && p5.chunkSizeCm == 800, "another legal chunk size plans");
        if (!p5.chunks.empty()) {
            const ChunkPayload pl = p5.payload(0);
            bool inRange = pl.chunkSizeCm == 800;
            for (const PayloadEntity& e : pl.entities)
                if (e.parent < 0 && (e.local.position.x < 0 || e.local.position.x >= 800 || e.local.position.y < 0 || e.local.position.y >= 800)) inRange = false;
            check(inRange, "...and root locals are inside [0, 800)");
        }
    }
}

// ---- 5..7: disk, determinism, the slim level ------------------------------------------------------------------------------------

static void diskChecks() {
    AVER_INFO("-- disk");
    g_files.clear();
    // The level with an instance, so the slim level has something composed to carry.
    fmt::OcWorldData child;
    child.name = "Child";
    child.placements.push_back(place(kBox, 100, 0, 0, 0xC1));
    {
        fmt::OcWorldPlacement k = place("Meshes/Barrel.ocmesh", 0, 0, 0, 0xC4);
        k.className = "Barrel";   // a class inside an instance: level-global, composed
        k.x = 200;
        child.placements.push_back(k);
    }
    {
        fmt::OcLight l;
        l.name = "ChildLight";
        l.x = 50;
        child.lights.push_back(l);
    }
    g_files["Maps/Child.ocworld"] = child;

    fmt::OcWorldData level = makeLevel();
    fmt::OcLevelInstance rec;
    rec.asset = "Maps/Child.ocworld";
    rec.x = 40000; rec.y = 40000; rec.z = 0;
    rec.yaw = 90;
    rec.id = 0x77;
    level.levelInstances.push_back(rec);

    LevelAssetLibrary lib;
    installLoader(lib);

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aver-authoredcook-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path dirA = root / "A", dirB = root / "B";

    CookInput in = inputFor(&lib, root.string());
    CookOptions optA;
    optA.outDir = dirA.string();
    CookReport rep;
    checkWhy(cookAuthoredLevel(level, in, optA, rep, &why), "the cook writes the set");
    check(rep.dir == dirA.string() && rep.bytes > 0 && rep.sourceHash != 0, "the report names the directory, the bytes and the hash");
    check(rep.instances == 1 && rep.flatPlacements == kPlacementCount + 2, "the instance's two placements joined the flat list");
    check(rep.globalClass == 2, "the class inside the instance is level-global too");

    std::set<std::string> names = filesIn(dirA);
    check(names.count(kCookIndexName) == 1 && names.count(kCookLevelName) == 1, "world.ocindex and level.ocworld are there");
    check(names.count("r.0.0.0.avrgn") == 1 && names.size() >= 4, "the near chunks are in r.0.0.0.avrgn and the far ones in their own region files");
    check(!fs::exists(fs::path(dirA.string() + ".tmp")), "no temporary directory is left behind");

    // ---- read back through the source ----
    AuthoredChunkSource src;
    checkWhy(src.open(dirA.string(), &why), "AuthoredChunkSource opens the set");
    check(src.index().kind == kIndexKindCooked && src.chunkSizeCm() == kDefaultChunkSizeCm, "kind cooked, chunk size 1600");
    check(src.sourceHash() == rep.sourceHash && src.levelId() == fnv1a64(std::string_view("Test")), "the index carries the source hash and the level id");
    CookPlan plan;
    CookReport rep2;
    checkWhy(planAuthoredCook(level, in, optA, plan, rep2, &why), "the same plan again");
    check(src.wide() == plan.wide, "the wide list in the index is the plan's");
    check(cookSourceHash(level, in, optA) == rep.sourceHash, "cookSourceHash gives the hash the cook stamped");

    bool allSame = true;
    for (usize i = 0; i < plan.chunks.size(); ++i) {
        ChunkPayload got;
        if (!src.has(plan.chunks[i]) || !src.load(plan.chunks[i], got, &why)) { allSame = false; break; }
        if (encodeChunk(got) != encodeChunk(plan.payload(i)) || !(got.coord == plan.chunks[i])) { allSame = false; break; }
    }
    check(allSame, "every chunk reads back from disk byte for byte what the plan built");
    check(!src.has(ChunkCoord{1000, 1000, 1000}), "a chunk the cook did not write is empty");
    const std::string levelPathStr = src.levelPath();

    std::set<std::tuple<i32, i32, i32>> seen;
    src.forEachOccupied(ChunkBox{}, [&](const ChunkCoord& c) { seen.insert({c.x, c.y, c.z}); });
    bool sameSet = seen.size() == plan.chunks.size();
    for (const ChunkCoord& c : plan.chunks) if (!seen.count({c.x, c.y, c.z})) sameSet = false;
    check(sameSet, "the unbounded occupancy query returns exactly the cooked chunks");

    // ---- determinism and replacement ----
    // The source holds region files open, and a set cannot be replaced under an open handle: that is a game running on it.
    src.close();
    CookOptions optB = optA;
    optB.outDir = dirB.string();
    CookReport repB;
    checkWhy(cookAuthoredLevel(level, in, optB, repB, &why), "a second cook into another directory");
    bool same = filesIn(dirA) == filesIn(dirB);
    if (same)
        for (const std::string& n : filesIn(dirA)) if (readAll(dirA / n) != readAll(dirB / n)) { same = false; AVER_ERROR("   differs: {}", n); }
    check(same, "two cooks of the same level are byte-identical, file for file");
    CookReport rep3;
    checkWhy(cookAuthoredLevel(level, in, optA, rep3, &why), "a re-cook over the existing set");
    check(filesIn(dirA) == filesIn(dirB) && !fs::exists(fs::path(dirA.string() + ".tmp")), "...replaces it and leaves no temporary directory");
    check(readAll(dirA / kCookIndexName) == readAll(dirB / kCookIndexName), "...to the same bytes");

    // ---- the slim level ----
    {
        fmt::OcWorldData slim;
        checkWhy(fmt::loadOcworld(levelPathStr, slim, &why), "level.ocworld parses");
        check(slim.levelInstances.empty(), "no LEVELINST records remain");
        // global: Class (Barrel), Vehicle, Seq, Unbounded, and the instance's class = 5
        check(slim.placements.size() == 5, "five level-global placements remain");
        check(slim.stream.enabled && slim.stream.cellCm == 3200.0f && slim.stream.loadCm == 20000.0f, "the STREAM header is retained");
        check(slim.hasSpawn && slim.spawnX == 10.0, "the rest of the level is retained");
        check(slim.lights.size() == 2, "the level's light and the instance's light (composed) are both there");
        bool composedLight = false;
        for (const fmt::OcLight& l : slim.lights)
            if (l.name == "ChildLight") composedLight = isNear(l.x, 40000.0, 1e-2) && isNear(l.y, 40050.0, 1e-2);
        check(composedLight, "...the latter at its world position (40000 + the rotated 50 cm offset)");
        const fmt::OcSeqTrack* tt = nullptr;
        for (const fmt::OcSequence& s : slim.sequences)
            for (const fmt::OcSeqTrack& t : s.tracks) if (t.kind == fmt::OcSeqTrackKind::Transform) tt = &t;
        check(tt && tt->target >= 0 && static_cast<usize>(tt->target) < slim.placements.size() &&
                  slim.placements[static_cast<usize>(tt->target)].placementId == 0xA9,
              "the sequence track's target was renumbered to the placement it named");
        const fmt::OcSeqTrack* cam = nullptr;
        for (const fmt::OcSequence& s : slim.sequences)
            for (const fmt::OcSeqTrack& t : s.tracks) if (t.kind == fmt::OcSeqTrackKind::Camera) cam = &t;
        check(cam && cam->target == -1, "a free camera track keeps -1");
        bool classInside = false;
        for (const fmt::OcWorldPlacement& p : slim.placements)
            if (p.className == "Barrel" && p.placementId != 0xA7) classInside = isNear(p.x, 40000.0, 1e-2) && isNear(p.y, 40200.0, 1e-2);
        check(classInside, "the instance's class placement is in the slim level at its composed world position");
    }

    // ---- staleness ----
    {
        const u64 base = cookSourceHash(level, in, optA);
        check(base == rep.sourceHash, "an unchanged level hashes as stamped");
        fmt::OcWorldData moved = level;
        moved.placements[Idx::Plain].x += 0.5;
        check(cookSourceHash(moved, in, optA) != base, "moving a placement by half a centimetre changes the hash");
        fmt::OcWorldData lit = level;
        lit.lights[0].intensityCd += 1.0;
        check(cookSourceHash(lit, in, optA) != base, "editing a light (the rest of the level) changes it");
        fmt::OcWorldData tuned = level;
        tuned.stream.loadCm += 5000.0f;
        tuned.stream.evictCm += 5000.0f;
        check(cookSourceHash(tuned, in, optA) == base, "moving the STREAM distances does not (they are runtime policy, not what the cook makes)");
        fmt::OcWorldData relabelled = level;
        relabelled.placements[Idx::Labelled].name = "Back Door";
        check(cookSourceHash(relabelled, in, optA) != base, "renaming a label changes it");
        fmt::OcWorldData reinst = level;
        reinst.levelInstances[0].x += 1.0;
        check(cookSourceHash(reinst, in, optA) != base, "moving the instance changes it");
        fmt::OcWorldData childEdit = g_files["Maps/Child.ocworld"];
        childEdit.placements[0].x = 150;
        lib.put("Maps/Child.ocworld", childEdit);
        check(cookSourceHash(level, in, optA) != base, "editing the instanced level changes it");
        lib.put("Maps/Child.ocworld", g_files["Maps/Child.ocworld"]);
        check(cookSourceHash(level, in, optA) == base, "...and putting it back restores it");
        CookOptions other = optA;
        other.chunkSizeCm = 800;
        check(cookSourceHash(level, in, other) != base, "a different chunk size changes it");
        CookOptions tighter = optA;
        tighter.wideCm = 4000.0;
        check(cookSourceHash(level, in, tighter) != base, "a different wide threshold changes it");
    }

    // ---- a plain AuthoredChunkSource refuses what is not a cooked set ----
    {
        AuthoredChunkSource none;
        check(!none.open((root / "nowhere").string(), &why), "a directory with no index is refused");
    }
    fs::remove_all(root, ec);
}

// ---- 7b: a track bound by NAME keeps its actor level-global -------------------------------------------------------------------

static void bindByNameChecks() {
    AVER_INFO("-- sequence track bound by name");
    g_files.clear();
    fmt::OcWorldData level;
    level.placements.push_back(place(kBox, 5000, 5000, 0, 0xB0));
    level.placements.back().name = "Elevator";
    level.placements.push_back(place(kBox, 9000, 5000, 0, 0xB1));
    level.placements.push_back(place(kBox, 12000, 5000, 0, 0xB2));
    level.placements.back().name = "Unrelated";
    level.placements.push_back(place(kBox, 15000, 5000, 0, 0xB3));
    level.placements.back().parent = 0;   // a child of "Elevator"
    level.placements.back().name = "ElevatorDoor";

    fmt::OcSequence seq;
    seq.name = "Ride";
    fmt::OcSeqTrack tr;
    tr.kind = fmt::OcSeqTrackKind::Transform;
    tr.bind = "ElevatorDoor";
    seq.tracks.push_back(tr);
    level.sequences.push_back(seq);

    LevelAssetLibrary lib;
    installLoader(lib);
    CookPlan plan;
    CookReport rep;
    checkWhy(planAuthoredCook(level, inputFor(&lib), CookOptions{}, plan, rep, &why), "the plan succeeds");
    check(rep.globalSequence == 1, "one root is level-global for the name binding (the root that holds the named child)");
    check(rep.levelGlobal == 2 && rep.chunkOwned == 2, "the named child travels with its root: two placements stay, two are chunk-owned");
    bool kept = false, named = false;
    for (const fmt::OcWorldPlacement& p : plan.slim.placements) {
        if (p.placementId == 0xB0) kept = true;
        if (p.name == "ElevatorDoor") named = true;
    }
    check(kept && named, "the slim level holds the root and the bound child");
    check(plan.slim.sequences.size() == 1 && plan.slim.sequences[0].tracks[0].bind == "ElevatorDoor", "the track keeps its name binding");

    // A name nothing carries binds nothing at the cook: no placement is held back for it.
    level.sequences[0].tracks[0].bind = "NobodyHasThisName";
    CookPlan plan2;
    CookReport rep2;
    checkWhy(planAuthoredCook(level, inputFor(&lib), CookOptions{}, plan2, rep2, &why), "a plan with an unknown bind name succeeds");
    check(rep2.globalSequence == 0 && rep2.levelGlobal == 0, "an unmatched bind name keeps everything chunk-owned");
}

// ---- 8: scale -------------------------------------------------------------------------------------------------------------------

static void scaleChecks() {
    AVER_INFO("-- scale");
    g_files.clear();
    fmt::OcWorldData level;
    level.name = "Slice";
    const int cols = 200, rows = 100;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            fmt::OcWorldPlacement p = place(kBox, c * 400.0 - 30000.0, r * 400.0 - 20000.0, (r + c) % 5 * 10.0,
                                            0x100000ull + static_cast<u64>(r) * 1000 + static_cast<u64>(c));
            if ((r * cols + c) % 997 == 0) p.sx = p.sy = p.sz = 40;       // a few wide ones
            if ((r * cols + c) % 13 == 0) p.yaw = static_cast<f64>((r + c) % 360);
            level.placements.push_back(std::move(p));
        }
    LevelAssetLibrary lib;
    installLoader(lib);
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aver-authoredcook-scale";
    std::error_code ec;
    fs::remove_all(dir, ec);
    CookInput in = inputFor(&lib, dir.string());
    CookOptions opt;
    opt.outDir = (dir / "Slice").string();
    CookReport rep;
    checkWhy(cookAuthoredLevel(level, in, opt, rep, &why), "a 20,000-placement slice cooks");
    check(rep.chunkOwned == 20000 && rep.levelGlobal == 0, "nothing was lost and nothing went level-global");
    check(rep.wide == 21, "21 roots are wide (every 997th: 20 of them, plus the one at index 0)");
    AVER_INFO("   note  {} placements -> {} chunks in {} region(s), {:.2f} MB, largest chunk {} entities, {:.0f} ms",
              rep.chunkOwned, rep.chunks, rep.regions, static_cast<f64>(rep.bytes) / (1024.0 * 1024.0), rep.largestChunkEntities, rep.elapsedMs);

    AuthoredChunkSource src;
    checkWhy(src.open(opt.outDir, &why), "the slice opens");
    usize read = 0;
    std::set<u64> ids;
    std::vector<ChunkCoord> occupied;
    src.forEachOccupied(ChunkBox{}, [&](const ChunkCoord& c) { occupied.push_back(c); });   // the callback must not call back into the source
    for (const ChunkCoord& c : occupied) {
        ChunkPayload p;
        if (!src.load(c, p, &why)) continue;
        for (const PayloadEntity& e : p.entities) {
            ++read;
            PlacementExtras x;
            if (getPlacementExtras(e, x)) ids.insert(x.placementId);
        }
    }
    check(occupied.size() == rep.chunks && read == 20000 && ids.size() == 20000,
          "all 20,000 are read back through the occupancy query, each with its own id");
    src.close();
    fs::remove_all(dir, ec);
}

int main() {
    flattenChecks();
    planChecks();
    diskChecks();
    bindByNameChecks();
    scaleChecks();
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
