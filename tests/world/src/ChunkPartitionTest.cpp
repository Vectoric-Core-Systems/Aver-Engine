// Partition a world into chunks, capture it, rebuild it, and prove nothing moved or went missing.
// Exit code = failure count.
//
// THIS IS THE PRECURSOR TO THE STRONGEST TEST IN docs/CHUNKS.md -- "chunked equals flat". That one
// compares a chunked load against a flat one; this one establishes the property it rests on, that
// scene -> payload -> scene is the identity on everything that matters. It needs no renderer, no
// GPU and no file.
//
// The hard cases are chosen, not incidental:
//   * NEGATIVE COORDINATES, because the world->chunk map floors and C++ integer division truncates.
//   * EXACT CHUNK BOUNDARIES, where an off-by-one puts an entity in the neighbouring chunk.
//   * A HIERARCHY WHOSE CHILDREN SIT IN OTHER CHUNKS, which must NOT be split -- CHierarchy is
//     intrusive, so a child without its parent is a dangling handle rather than a hole.
//   * AN ENTITY FAR FROM THE ORIGIN, where an absolute f32 has already lost the precision the
//     chunk-local split keeps.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkPartition.hpp"
#include "aver/world/ChunkPayload.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Everything about one entity that a round trip must preserve, flattened so two scenes can be
// compared without caring what handles they happened to get.
struct Snapshot {
    std::string name;
    u64 objectId = 0;
    f64 wx = 0, wy = 0, wz = 0;      // WORLD position, in f64 so the comparison is not the thing
    u64 mesh = 0;                    //                 losing the precision it is checking
    std::string material;
    u32 meshFlags = 0;
    int depth = 0;
    std::string parentName;
};

static f64 worldPosAxis(const scene::World& w, scene::Entity e, int axis) {
    // Walked rather than read from CWorld: composing is a per-frame pass and this test never runs a
    // frame. Rotation is identity throughout the fixture, so summing local positions is exact.
    f64 sum = 0.0;
    scene::Entity cur = e;
    for (int guard = 0; guard < 64 && cur != scene::kInvalidEntity; ++guard) {
        const auto* loc = w.component<scene::CLocal>(cur, scene::kComponentLocal);
        if (!loc) break;
        sum += axis == 0 ? loc->xf.position.x : axis == 1 ? loc->xf.position.y : loc->xf.position.z;
        const auto* h = w.component<scene::CHierarchy>(cur, scene::kComponentHierarchy);
        cur = h ? h->parent : scene::kInvalidEntity;
    }
    return sum;
}

static std::unordered_map<std::string, Snapshot> snapshot(const scene::World& w) {
    std::unordered_map<std::string, Snapshot> out;
    for (u32 i = 0; i < w.count(); ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e) || w.destroyPending(e)) continue;
        Snapshot s;
        s.name = w.name(e);
        s.objectId = w.objectId(e);
        s.wx = worldPosAxis(w, e, 0);
        s.wy = worldPosAxis(w, e, 1);
        s.wz = worldPosAxis(w, e, 2);
        if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            s.mesh = mr->mesh;
            s.meshFlags = mr->flags;
            if (mr->material) s.material = aver_scene_material_name(mr->material);
        }
        if (const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy)) {
            s.depth = static_cast<int>(h->depth);
            if (h->parent != scene::kInvalidEntity) s.parentName = w.name(h->parent);
        }
        out.emplace(s.name, std::move(s));
    }
    return out;
}

// Adds one entity with a mesh renderer. Names are unique so the snapshot can key on them.
static scene::Entity place(scene::World& w, const std::string& name, scene::Entity parent,
                           f32 x, f32 y, f32 z, const char* surface) {
    Transform xf;
    xf.position = Vec3{x, y, z};
    const scene::Entity e = w.create(name, parent, xf);
    if (e == scene::kInvalidEntity) return e;
    auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
    if (mr) {
        mr->mesh = 0x1234'5678'9ABC'DEF0ull ^ static_cast<u64>(e);
        mr->material = surface && *surface ? aver_scene_material(0, surface) : 0;
        mr->flags = scene::kMeshRendererVisible;
    }
    return e;
}

int main() {
    const i32 S = kDefaultChunkSizeCm;   // 1600
    scene::World& w = scene::World::instance();

    // ---- the fixture ------------------------------------------------------------------------------
    std::vector<scene::Entity> originals;
    originals.push_back(place(w, "origin",        scene::kInvalidEntity,     0,     0,    0, "M_Floor"));
    originals.push_back(place(w, "just_inside",   scene::kInvalidEntity,  1599,     0,    0, "M_Floor"));
    // Exactly one chunk out, and exactly one chunk back: the two places an off-by-one lives.
    originals.push_back(place(w, "boundary_pos",  scene::kInvalidEntity,  1600,     0,    0, "M_Wall"));
    originals.push_back(place(w, "boundary_neg",  scene::kInvalidEntity, -1600,     0,    0, "M_Wall"));
    // Just below zero: truncation would file this with `origin` instead of one chunk back.
    originals.push_back(place(w, "minus_one",     scene::kInvalidEntity,    -1,    -1,   -1, "M_Wall"));
    originals.push_back(place(w, "deep_negative", scene::kInvalidEntity, -9000, -7000, -300, "M_Accent"));
    // Far enough out that an absolute f32 has visibly coarsened.
    originals.push_back(place(w, "far_away",      scene::kInvalidEntity, 900000.0f, -450000.0f, 12000.0f, ""));

    // A hierarchy whose children are in OTHER chunks than its root. It must travel whole.
    const scene::Entity parent = place(w, "rig_root", scene::kInvalidEntity, 3200, 3200, 0, "M_Trim");
    originals.push_back(parent);
    place(w, "rig_child_a", parent,  5000,     0,   0, "M_Trim");   // +3 chunks away in x
    place(w, "rig_child_b", parent, -8000, -4000, 200, "");         // several chunks back
    const scene::Entity grandchild = place(w, "rig_child_c", parent, 100, 100, 100, "M_Trim");
    place(w, "rig_grandchild", grandchild, 50, 50, 50, "M_Trim");

    const auto before = snapshot(w);
    check(before.size() == 12, "the fixture has 12 entities");

    // ---- ownership --------------------------------------------------------------------------------
    check(ownerChunkOf(w, w.find("origin"), S) == ChunkCoord{0, 0, 0}, "the origin is in chunk 0");
    check(ownerChunkOf(w, w.find("just_inside"), S) == ChunkCoord{0, 0, 0},
          "1599 cm is still chunk 0 -- one centimetre inside the far face");
    check(ownerChunkOf(w, w.find("boundary_pos"), S) == ChunkCoord{1, 0, 0},
          "exactly 1600 cm is chunk 1, not chunk 0");
    check(ownerChunkOf(w, w.find("boundary_neg"), S) == ChunkCoord{-1, 0, 0},
          "exactly -1600 cm is chunk -1");
    check(ownerChunkOf(w, w.find("minus_one"), S) == ChunkCoord{-1, -1, -1},
          "-1 cm is chunk -1 on every axis -- the truncation bug, in three dimensions");

    // RULE 2: a child is owned by its ROOT's chunk, however far away it sits.
    const ChunkCoord rigChunk = ownerChunkOf(w, parent, S);
    check(rigChunk == ChunkCoord{2, 2, 0}, "the rig's root is in chunk (2,2,0)");
    check(ownerChunkOf(w, w.find("rig_child_a"), S) == rigChunk,
          "a child three chunks away is still owned by its root's chunk");
    check(ownerChunkOf(w, w.find("rig_child_b"), S) == rigChunk, "...and so is one several chunks back");
    check(ownerChunkOf(w, w.find("rig_grandchild"), S) == rigChunk, "...and a grandchild");

    // ---- the partition ----------------------------------------------------------------------------
    PartitionOptions po;
    po.chunkSizeCm = S;
    const Partition part = partitionWorld(w, po);

    check(part.entityCount == 12, "the partition counted every live entity");
    check(part.rootCount == 8, "...and found the 8 roots");
    u32 listed = 0;
    for (const auto& kv : part.chunks) listed += static_cast<u32>(kv.second.size());
    check(listed == part.rootCount, "every root is listed in exactly one chunk -- no duplicates, no drops");
    check(part.chunks.size() == 7, "the 8 roots fall into 7 distinct chunks");
    // Bounds are not filled by any loader today, so nothing can be judged oversize -- and the count
    // is what tells a caller that, rather than an empty list implying "everything fits".
    check(part.unknownBounds == 8, "every root had degenerate bounds, and that is REPORTED");
    check(part.oversize.empty(), "so nothing is claimed oversize");

    // ---- capture, tear down, restore ---------------------------------------------------------------
    const auto payloads = captureAll(w, part, S);
    check(payloads.size() == part.chunks.size(), "one payload per occupied chunk");

    u32 captured = 0;
    bool localsInRange = true;
    bool materialsAreNames = true;
    for (const auto& kv : payloads) {
        captured += static_cast<u32>(kv.second.entities.size());
        for (const PayloadEntity& pe : kv.second.entities) {
            // A ROOT's stored position must be inside its own chunk. This is the check that catches
            // partition and capture disagreeing about who owns what.
            if (pe.parent < 0) {
                const f32 lim = static_cast<f32>(S);
                if (pe.local.position.x < 0 || pe.local.position.x >= lim ||
                    pe.local.position.y < 0 || pe.local.position.y >= lim ||
                    pe.local.position.z < 0 || pe.local.position.z >= lim) localsInRange = false;
            }
            // Never a token. "M_Floor" is a name; "3" would be a token that means nothing tomorrow.
            if (!pe.material.empty() && pe.material.find_first_not_of("0123456789") == std::string::npos)
                materialsAreNames = false;
        }
    }
    check(captured == 12, "capture walked every entity, children included");
    check(localsInRange, "every root's stored position is inside its own chunk, in [0, 1600)");
    check(materialsAreNames, "materials are stored as NAMES, never as process-local tokens");

    // WHAT THE SPLIT ACTUALLY BUYS, stated precisely rather than implied by the drift figure above.
    //
    // That drift is 0 partly because this fixture uses whole centimetres, and f32 holds every
    // integer below 2^24 exactly -- so a round trip of 900000 would have survived as an absolute
    // coordinate too. The real payoff is not the round trip, it is the MAGNITUDE of the number that
    // goes to disk: 9 km from the origin, what gets stored is still a four-digit offset, where f32's
    // grid is ~50 nm rather than 0.625 mm and the text writer's %.6g is exact rather than lossy.
    {
        const ChunkCoord fc = ownerChunkOf(w, w.find("far_away"), S);
        const auto pit = payloads.find(fc);
        check(pit != payloads.end(), "the far entity's chunk has a payload");
        if (pit != payloads.end()) {
            const PayloadEntity* fe = nullptr;
            for (const PayloadEntity& pe : pit->second.entities) if (pe.name == "far_away") fe = &pe;
            check(fe != nullptr, "...containing it");
            if (fe) {
                const f32 storedX = fe->local.position.x;
                check(std::fabs(storedX) < static_cast<f32>(S),
                      "a root 9 km out stores a position under one chunk wide");
                const f32 gridStored = f32GridAtCm(storedX);
                const f32 gridAbs = f32GridAtCm(900000.0f);
                check(gridStored < gridAbs * 0.01f,
                      "...whose f32 grid is more than 100x finer than the absolute coordinate's");
                AVER_INFO("   note  9 km out: absolute f32 grid {:.4f} mm, stored-offset grid {:.6f} mm",
                          gridAbs * 10.0f, gridStored * 10.0f);
            }
        }
    }

    // Tear the world down completely, so the restore cannot accidentally be comparing against
    // anything that survived.
    for (const scene::Entity e : originals) w.destroy(e);
    w.flush();
    check(w.count() == 0, "the world is empty before the restore");

    std::vector<std::pair<i32, std::string>> bound;
    BodyRegistry bodies;
    RestoreOptions ro;
    ro.bindMaterial = [&bound](i32 token, const std::string& surface) { bound.emplace_back(token, surface); };
    // Bodies are stood in for: this module links no physics, and the point here is that the seam
    // gets CALLED with the right entity, not that Jolt agrees.
    i32 nextBody = 1;
    ro.createBody = [&bodies, &nextBody](scene::Entity e, const Vec3&, const Vec3&) {
        const i32 b = nextBody++;
        bodies.attach(e, b);
        return b;
    };

    for (const auto& kv : payloads) restore(kv.second, w, ro);
    check(w.count() == 12, "the restore rebuilt every entity");

    // ---- the comparison ----------------------------------------------------------------------------
    const auto after = snapshot(w);
    check(after.size() == before.size(), "the same number of entities came back");

    u32 missing = 0, movedPos = 0, wrongParent = 0, wrongMesh = 0, wrongMat = 0, wrongId = 0, wrongDepth = 0;
    f64 worstCm = 0.0;
    for (const auto& kv : before) {
        const auto it = after.find(kv.first);
        if (it == after.end()) { ++missing; continue; }
        const Snapshot& a = kv.second;
        const Snapshot& b = it->second;
        const f64 d = std::fabs(a.wx - b.wx) + std::fabs(a.wy - b.wy) + std::fabs(a.wz - b.wz);
        if (d > worstCm) worstCm = d;
        if (d != 0.0) ++movedPos;
        if (a.parentName != b.parentName) ++wrongParent;
        if (a.mesh != b.mesh) ++wrongMesh;
        if (a.material != b.material) ++wrongMat;
        if (a.objectId != b.objectId) ++wrongId;
        if (a.depth != b.depth) ++wrongDepth;
    }
    check(missing == 0, "no entity went missing");
    check(wrongParent == 0, "every parent link came back the same -- the hierarchy is intact");
    check(wrongDepth == 0, "...and so is every cached depth");
    check(wrongMesh == 0, "every mesh id survived");
    check(wrongMat == 0, "every material name survived the token round trip");
    check(wrongId == 0, "every objectId was carried, not recomputed from the name");
    // Exact, though note the fixture uses whole centimetres, which f32 holds exactly below 2^24 --
    // so this proves the round trip loses nothing, not that the split recovered anything. The
    // magnitude check above is where the split earns its place.
    check(movedPos == 0, "NOTHING MOVED -- every world position is bit-identical, 9 km out included");
    AVER_INFO("   note  worst world-position drift across the round trip: {:.6f} cm", worstCm);

    // ---- the physics seam ---------------------------------------------------------------------------
    // Nothing in the fixture asked for a body, so nothing should have been built. A seam that fires
    // when it was not asked to is worse than one that does not fire at all.
    check(bodies.size() == 0, "no body was created for a payload that declared none");

    {
        // And the registry itself, including the subtree rule World::destroy already follows.
        BodyRegistry r;
        const scene::Entity root = w.find("rig_root");
        const scene::Entity kid  = w.find("rig_child_c");
        const scene::Entity gkid = w.find("rig_grandchild");
        check(root != scene::kInvalidEntity && kid != scene::kInvalidEntity, "the rig came back findable");
        r.attach(root, 10);
        r.attach(kid, 11);
        r.attach(gkid, 12);
        check(r.bodyOf(kid) == 11, "a body can be found by its entity");
        check(r.attach(kid, 99) == 11, "attaching a second body returns the displaced one rather than leaking it");
        r.attach(kid, 11);
        std::vector<i32> freed;
        r.detachSubtree(w, root, freed);
        check(freed.size() == 3,
              "detachSubtree took the WHOLE subtree -- the leak World::destroy causes today");
        check(r.size() == 0, "...leaving nothing registered");
        check(r.bodyOf(root) == -1, "and a detached entity reports no body");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
