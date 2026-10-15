// game::LevelInstances against a fake host: realise, nest, cycle, overrides, unrealise, stream, edit, break, classes,
// lights and decals, records, rescan, runtime spawn, residency events. No GPU, no framework.
#include "LevelInstanceFixture.hpp"

// ---------------------------------------------------------------------------------------------------------------

static void testRealise() {
    AVER_INFO("realise");
    Fixture f;
    f.levels["Child"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 10 0 0 0 0 0 1 M_Old id=a\nPLACE Meshes/b.ocmesh 0 0 5 0 0 0 1 nocollide id=b\n");
    const fmt::OcWorldData host = parse("OCWORLD 1\nLEVELINST Child pos 1000 0 0 rot 90 0 0 scale 2 2 2 id=1\nENDLEVELINST\n");
    const i32 bodies0 = bodyCount();
    f.begin(host);
    check(f.li.count() == 1, "one instance");
    const scene::Entity root = f.li.at(0);
    check(root != scene::kInvalidEntity && f.li.loaded(root), "the instance is loaded");
    check(f.li.isMember(root) && f.li.rootOf(root) == root, "the root is a member of itself");
    const scene::Entity a = f.li.find(root, "a"), b = f.li.find(root, "000000000000000b");
    check(a != scene::kInvalidEntity && b != scene::kInvalidEntity, "find reaches both members by id");
    check(f.li.find(root, "") == root && f.li.find(root, "ff") == scene::kInvalidEntity, "an empty path is the root, an unknown id nothing");
    scene::World& w = scene::World::instance();
    check(w.parent(a) == root, "members hang under the root");
    const scene::CLevelInstanceLink la = linkOf(a);
    check(la.assetId == fnv1a64(std::string_view("Child")) && la.placementId == 0xa && la.root == root && la.depth == 0 &&
              la.instanceId == f.li.instanceIdOf(root) && la.instanceId != 0,
          "the link carries the asset id, placement id, root, depth and instance id");
    flush();
    const Vec3 pa = worldPos(a);
    // yaw 90, scale 2: local (10,0,0) -> (0,20,0) -> + (1000,0,0).
    check(near(pa.x, 1000.0f) && near(pa.y, 20.0f) && near(pa.z, 0.0f), "member world position = instance TRS x local");
    const Vec3 pb = worldPos(b);
    check(near(pb.x, 1000.0f) && near(pb.y, 0.0f) && near(pb.z, 10.0f), "a second member composes the same way");
    check(f.acquired.size() == 2 && f.acquireTotal() == 2, "each distinct mesh is acquired once");
    check(f.residency.size() == 1 && std::get<1>(f.residency[0]) && std::get<2>(f.residency[0]) == 3,
          "the residency callback fires once on load, with the entity count (root + 2)");
    check(f.created.size() >= 3, "the created hook saw the root and its members");
#if AVER_MODULE_PHYSICS
    if (aver_phys_ready()) check(bodyCount() == bodies0 + 1, "one static body for the colliding member");
#endif
    check(f.li.stats().entities == 3 && f.li.stats().loaded == 1, "stats count the entities and loaded instances");

    // Unrealise: everything goes, bodies and meshes too.
    f.li.end();
    flush();
    check(!w.valid(a) && !w.valid(b) && !w.valid(root), "end() destroys the members and the root");
    check(bodyCount() == bodies0, "the live-body count returns to the baseline");
    check(f.acquireTotal() == f.releaseTotal(), "every acquired mesh was released");
    check(f.li.count() == 0, "the table is empty");
}

static void testNestedAndSharing() {
    AVER_INFO("nested + shared parse");
    Fixture f;
    f.levels["Leaf"] = parse("OCWORLD 1\nPLACE Meshes/l.ocmesh 1 2 3 0 0 0 1 id=a1\n");
    f.levels["Mid"] = parse("OCWORLD 1\nPLACE Meshes/m.ocmesh 0 0 0 0 0 0 1 id=c1\nLEVELINST Leaf pos 0 50 0 id=5\nENDLEVELINST\n");
    const fmt::OcWorldData host = parse("OCWORLD 1\nLEVELINST Mid pos 100 0 0 id=1\nENDLEVELINST\nLEVELINST Leaf pos -100 0 0 id=2\nENDLEVELINST\n");
    f.begin(host);
    check(f.li.count() == 3, "two top-level instances and one nested");
    scene::Entity mid = scene::kInvalidEntity, leafTop = scene::kInvalidEntity;
    for (u32 i = 0; i < f.li.count(); ++i) {
        const scene::Entity r = f.li.at(i);
        if (f.li.assetOf(r) == "Mid") mid = r;
        else if (f.li.assetOf(r) == "Leaf" && !f.li.isNested(r)) leafTop = r;
    }
    check(mid != scene::kInvalidEntity && leafTop != scene::kInvalidEntity, "both top-level roots found");
    const scene::Entity nestedRoot = f.li.find(mid, "5");
    check(nestedRoot != scene::kInvalidEntity && f.li.isNested(nestedRoot), "find reaches a nested instance by its record id");
    const scene::Entity l = f.li.find(mid, "5/a1");
    check(l != scene::kInvalidEntity, "find reaches a placement inside a nested instance by path");
    flush();
    const Vec3 p = worldPos(l);
    check(near(p.x, 101.0f) && near(p.y, 52.0f) && near(p.z, 3.0f), "a nested member composes through both instances");
    check(linkOf(l).depth == 1 && f.li.rootOf(l) == nestedRoot && f.li.topRootOf(l) == mid, "depth 1; rootOf is the innermost, topRootOf the outermost");
    check(f.lib.loadCount() == 2, "each level parses once even though Leaf is instanced twice");

    // The same child instanced twice are independent.
    check(f.li.find(leafTop, "a1") != scene::kInvalidEntity && f.li.find(leafTop, "a1") != l, "two instances of one asset have their own entities");
}

static void testCycle() {
    AVER_INFO("cycle");
    Fixture f;
    f.levels["A"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 0 0 0 0 0 0 1 id=a1\nLEVELINST B pos 0 0 0 id=2\nENDLEVELINST\n");
    f.levels["B"] = parse("OCWORLD 1\nPLACE Meshes/b.ocmesh 0 0 0 0 0 0 1 id=b1\nLEVELINST A pos 0 0 0 id=3\nENDLEVELINST\n");
    f.levels["Ok"] = parse("OCWORLD 1\nPLACE Meshes/o.ocmesh 0 0 0 0 0 0 1 id=d1\n");
    f.begin(parse("OCWORLD 1\nLEVELINST A pos 0 0 0 id=1\nENDLEVELINST\nLEVELINST Ok pos 500 0 0 id=9\nENDLEVELINST\n"));
    scene::Entity a = scene::kInvalidEntity, ok = scene::kInvalidEntity;
    for (u32 i = 0; i < f.li.count(); ++i) {
        const scene::Entity r = f.li.at(i);
        if (f.li.assetOf(r) == "A" && !f.li.isNested(r)) a = r;
        if (f.li.assetOf(r) == "Ok") ok = r;
    }
    check(a != scene::kInvalidEntity && f.li.find(a, "a1") != scene::kInvalidEntity, "the top of the cycle still realises its own placements");
    check(ok != scene::kInvalidEntity && f.li.find(ok, "d1") != scene::kInvalidEntity, "a sibling instance is unaffected");
    const scene::Entity b = f.li.find(a, "2");
    check(b != scene::kInvalidEntity && f.li.find(b, "b1") != scene::kInvalidEntity, "B realises under A");
    const scene::Entity again = b != scene::kInvalidEntity ? f.li.find(b, "3") : scene::kInvalidEntity;
    check(again != scene::kInvalidEntity && f.li.find(again, "a1") == scene::kInvalidEntity,
          "the inner A (the cycle) exists as an empty root: no contents, no recursion");
}

static void testOverrides() {
    AVER_INFO("overrides");
    Fixture f;
    f.levels["Child"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 10 0 0 0 0 0 1 M_Old id=a\nPLACE Meshes/b.ocmesh 0 0 5 0 0 0 1 id=b\nPLACE Meshes/c.ocmesh 0 0 0 0 0 0 1 id=c\n");
    fmt::OcWorldData host = parse("OCWORLD 1\nLEVELINST Child pos 0 0 0 id=1\nENDLEVELINST\n");
    host.levelInstances[0].overrides.push_back(ovStr("a", fmt::kLiFieldMaterial, "M_Red"));
    host.levelInstances[0].overrides.push_back(ovBool("b", fmt::kLiFieldRemoved, true));
    host.levelInstances[0].overrides.push_back(ovVec3("c", fmt::kLiFieldPosition, 0, 0, 40));
    f.begin(host);
    const scene::Entity root = f.li.at(0);
    scene::World& w = scene::World::instance();
    const scene::Entity a = f.li.find(root, "a"), c = f.li.find(root, "c");
    check(f.li.find(root, "b") == scene::kInvalidEntity, "a removed placement has no entity");
    check(a != scene::kInvalidEntity && std::string(aver_scene_material_name(w.component<scene::CMeshRenderer>(a, scene::kComponentMeshRenderer)->material)) == "M_Red",
          "the material override applies");
    flush();
    check(c != scene::kInvalidEntity && near(worldPos(c).z, 40.0f), "the position override applies");

    // computeOverrides -> setOverrides is the identity.
    std::vector<fmt::OcPrefabOverride> ov;
    check(f.li.computeOverrides(root, ov), "computeOverrides succeeds");
    size_t removed = 0, material = 0, position = 0;
    for (const auto& o : ov) {
        if (o.value.name == fmt::kLiFieldRemoved) ++removed;
        if (o.value.name == fmt::kLiFieldMaterial) ++material;
        if (o.value.name == fmt::kLiFieldPosition) ++position;
    }
    check(ov.size() == 3 && removed == 1 && material == 1 && position == 1, "the diff finds exactly the three overrides that were applied");

    // Edit a member live and see the diff follow.
    w.setLocalPosition(a, Vec3{77.0f, 0.0f, 0.0f});
    std::vector<fmt::OcPrefabOverride> ov2;
    f.li.computeOverrides(root, ov2);
    bool movedA = false;
    for (const auto& o : ov2) if (o.path == "000000000000000a" && o.value.name == fmt::kLiFieldPosition && o.value.f.size() == 3 && near(o.value.f[0], 77.0f)) movedA = true;
    check(movedA, "a live move shows up as a position override");

    // Round trip through setOverrides.
    check(f.li.setOverrides(root, ov2), "setOverrides applies a computed list");
    flush();
    std::vector<fmt::OcPrefabOverride> ov3;
    f.li.computeOverrides(root, ov3);
    check(ov3.size() == ov2.size(), "computeOverrides -> setOverrides -> computeOverrides keeps the list");
    const scene::Entity a2 = f.li.find(root, "a");
    check(a2 != scene::kInvalidEntity && near(w.localTransform(a2).position.x, 77.0f), "the rebuilt member sits where the override says");
}

static void testRevertAndRecords() {
    AVER_INFO("revert, records, spawn, rescan");
    Fixture f;
    f.levels["Child"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 10 0 0 0 0 0 1 id=a\nPLACE Meshes/b.ocmesh 0 0 5 0 0 0 1 id=b\n");
    f.begin(parse("OCWORLD 1\nLEVELINST Child pos 300 0 0 id=1 name Room\nENDLEVELINST\n"));
    scene::World& w = scene::World::instance();
    const scene::Entity root = f.li.at(0);
    const scene::Entity a = f.li.find(root, "a");
    w.setLocalPosition(a, Vec3{99.0f, 0.0f, 0.0f});
    check(f.li.revertAll(root), "revertAll succeeds");
    const scene::Entity a2 = f.li.find(root, "a");
    check(a2 != scene::kInvalidEntity && near(w.localTransform(a2).position.x, 10.0f), "revertAll puts the member back where the child says");

    // A moved root shows in the captured records, which are records and not members.
    w.setLocalPosition(root, Vec3{450.0f, 0.0f, 0.0f});
    const auto recs = f.li.captureRecords();
    check(recs.size() == 1 && recs[0].asset == "Child" && near(static_cast<f32>(recs[0].x), 450.0f) && recs[0].id == 1 && recs[0].name == "Room",
          "captureRecords returns the record with the live transform, its id and name");

    // A runtime-only spawn is not a record, and end() takes it down.
    fmt::OcLevelInstance r;
    r.asset = "Child";
    r.x = -200;
    const scene::Entity rt = f.li.spawn(r);
    check(rt != scene::kInvalidEntity && f.li.loaded(rt) && f.li.isRuntimeOnly(rt), "spawn makes a loaded runtime-only instance");
    check(f.li.captureRecords().size() == 1, "a runtime-only instance is not captured");
    check(f.li.find(rt, "a") != scene::kInvalidEntity, "its contents exist");
    check(f.li.destroy(rt), "destroy removes it");
    flush();
    check(!w.valid(rt) && f.li.count() == 1, "the destroyed root is gone from the world and the table");
    const scene::Entity rt2 = f.li.spawn(r);
    const scene::Entity m2 = f.li.find(rt2, "b");

    // rescan rebuilds the table from the links.
    f.li.rescan();
    check(f.li.count() == 2, "rescan rebuilds the table from the links");
    check(f.li.find(rt2, "b") == m2 && f.li.find(root, "a") == a2, "rescan finds the same members by placement id");
    f.li.end();
    flush();
    check(!w.valid(rt2) && !w.valid(m2), "end() destroys the runtime-only instance as well");
}

static void testStreaming() {
    AVER_INFO("streaming");
    Fixture f;
    f.levels["Block"] = parse("OCWORLD 1\nEXTENT -100 -100 0 100 100 100\nPLACE Meshes/blk.ocmesh 0 0 0 0 0 0 1 id=a\n");
    f.levels["NoExtent"] = parse("OCWORLD 1\nPLACE Meshes/ne.ocmesh 0 0 0 0 0 0 1 id=a\n");
    world::PlacementStreamSettings ss;
    ss.cellCm = 6400; ss.loadCm = 25000; ss.evictCm = 30000;
    f.begin(parse("OCWORLD 1\n"
                  "LEVELINST Block pos 0 0 0 stream 1 id=1\nENDLEVELINST\n"
                  "LEVELINST Block pos 1000000 0 0 stream 1 id=2\nENDLEVELINST\n"
                  "LEVELINST NoExtent pos 5000 0 0 stream 1 id=3\nENDLEVELINST\n"
                  "LEVELINST Block pos 3000 0 0 stream 0 id=4\nENDLEVELINST\n"),
            &ss);
    scene::Entity nearR = scene::kInvalidEntity, farR = scene::kInvalidEntity, unknown = scene::kInvalidEntity, resident = scene::kInvalidEntity;
    for (u32 i = 0; i < f.li.count(); ++i) {
        const scene::Entity r = f.li.at(i);
        const fmt::OcLevelInstance* rec = f.li.recordOf(r);
        if (!rec) continue;
        if (rec->id == 1) nearR = r;
        if (rec->id == 2) farR = r;
        if (rec->id == 3) unknown = r;
        if (rec->id == 4) resident = r;
    }
    check(nearR && farR && unknown && resident, "all four roots exist from the start");
    check(!f.li.loaded(nearR) && !f.li.loaded(farR), "streamed units start unloaded (their roots exist for the script handles)");
    check(f.li.loaded(unknown), "an instance with unknown bounds is always loaded");
    check(f.li.loaded(resident), "stream 0 is resident from the start");

    f.meshReady = false;
    f.li.tick({Vec3{0, 0, 0}});
    check(!f.li.loaded(nearR), "a unit waits for its meshes to be read");
    f.meshReady = true;
    f.li.tick({Vec3{0, 0, 0}});
    check(f.li.loaded(nearR) && !f.li.loaded(farR), "near loads once ready; far stays out");
    check(f.prefetches > 0, "the unit's meshes were requested ahead");

    // Overrides set on a resident unit survive its eviction.
    std::vector<fmt::OcPrefabOverride> ov{ovStr("a", fmt::kLiFieldMaterial, "M_Keep")};
    f.li.setOverrides(nearR, ov);
    f.li.tick({Vec3{1000000, 0, 0}});
    check(f.li.loaded(farR) && !f.li.loaded(nearR), "far loads, near is evicted past the evict distance");
    check(f.li.loaded(resident) && f.li.loaded(unknown), "stream 0 and unknown-bounds units are never evicted");
    f.li.tick({Vec3{0, 0, 0}});
    check(f.li.loaded(nearR), "near reloads when the viewer returns");
    scene::World& w = scene::World::instance();
    const scene::Entity a = f.li.find(nearR, "a");
    check(a != scene::kInvalidEntity &&
              std::string(aver_scene_material_name(w.component<scene::CMeshRenderer>(a, scene::kComponentMeshRenderer)->material)) == "M_Keep",
          "an evicted-and-reloaded unit keeps its overrides");

    // Pinned units are never evicted.
    f.li.pin(nearR, true);
    f.li.tick({Vec3{1000000, 0, 0}});
    check(f.li.loaded(nearR), "a pinned unit stays loaded");
    f.li.pin(nearR, false);
    f.li.tick({Vec3{1000000, 0, 0}});
    check(!f.li.loaded(nearR), "and goes when unpinned");

    // Hand loading.
    check(f.li.setLoaded(nearR, true) && f.li.loaded(nearR), "setLoaded(true) loads by hand");
    f.li.tick({Vec3{1000000, 0, 0}});
    check(f.li.loaded(nearR), "a hand-loaded unit is not evicted");
    check(f.li.setLoaded(nearR, false) && !f.li.loaded(nearR), "setLoaded(false) unloads it");

    // Residency events: one per change.
    size_t loads = 0, unloads = 0;
    for (const auto& e : f.residency) (std::get<1>(e) ? loads : unloads)++;
    check(loads >= 5 && unloads >= 3, "the residency callback fired for each load and each eviction");
    Vec3 lo, hi;
    check(f.li.extentBounds(lo, hi) && lo.x <= -100.0f && hi.x >= 1000100.0f, "extentBounds covers the instances with known bounds");
}

// A unit too big for one tick's budget realises over several, in whole subtrees; a viewer leaving cancels it and a pin
// finishes it at once.
static void testStagedRealise() {
    AVER_INFO("staged realise");
    std::string big = "OCWORLD 1\nEXTENT -100 -100 0 7000 100 100\n";
    for (int i = 0; i < 60; ++i) {
        big += "PLACE Meshes/m" + std::to_string(i) + ".ocmesh " + std::to_string(i * 100) + " 0 0 0 0 0 1 id=" +
               std::to_string(1000 + i) + "\n";
        if (i == 9) big += "BEGIN\nCHILD Meshes/kid.ocmesh 0 50 0 0 0 0 1 id=c\nEND\n";
    }
    Fixture f;
    f.levels["Big"] = parse(big);
    f.materialsPerMesh = 1;
    f.materialMs = 2.0;
    world::PlacementStreamSettings ss;
    ss.cellCm = 6400; ss.loadCm = 25000; ss.evictCm = 30000;
    f.begin(parse("OCWORLD 1\nLEVELINST Big pos 0 0 0 stream 1 id=1\nENDLEVELINST\n"), &ss);
    const scene::Entity root = f.li.at(0);
    scene::World& w = scene::World::instance();

    // Ticks at the viewer until the unit is part-realised (entities wait for their slice's materials).
    const auto tickUntilPartial = [&] {
        for (int i = 0; i < 400 && descendants(root).empty() && !f.li.loaded(root); ++i) f.li.tick({Vec3{0, 0, 0}});
        return !f.li.loaded(root) && !descendants(root).empty();
    };
    f.li.tick({Vec3{0, 0, 0}});
    check(!f.li.loaded(root) && f.materialsBuilt > 0 && f.materialsBuilt < 61,
          "the first tick builds some of the unit's materials (" + std::to_string(f.materialsBuilt) + " of 61), not all");
    check(tickUntilPartial(), "entities appear before the unit is complete");
    int ticks = 1;
    while (!f.li.loaded(root) && ticks < 400) { f.li.tick({Vec3{0, 0, 0}}); ++ticks; }
    check(f.li.loaded(root) && ticks > 2, "it finishes over " + std::to_string(ticks) + " more ticks");
    check(descendants(root).size() == 61 && f.acquireTotal() == 61 && f.materialsBuilt == 61,
          "every placement, mesh and material is there once");
    const scene::Entity kid = f.li.find(root, "c");
    const scene::Entity parent = f.li.find(root, "1009");
    check(kid != scene::kInvalidEntity && parent != scene::kInvalidEntity && w.parent(kid) == parent,
          "a child in a later slice hangs under its parent");

    // Cancelled when the viewer leaves mid-load: what it made goes, its meshes are released.
    f.li.tick({Vec3{1000000, 0, 0}});
    flush();
    check(!f.li.loaded(root) && descendants(root).empty(), "evicted past the evict distance");
    check(tickUntilPartial(), "a second load is part-way");
    f.li.tick({Vec3{1000000, 0, 0}});
    flush();
    check(!f.li.loaded(root) && descendants(root).empty() && f.releaseTotal() == f.acquireTotal(),
          "the viewer leaving cancels it and releases its meshes");

    // A pin mid-load finishes it in that tick.
    check(tickUntilPartial(), "a third load is part-way");
    f.li.pin(root, true);
    f.li.tick({Vec3{0, 0, 0}});
    check(f.li.loaded(root) && descendants(root).size() == 61, "pinned mid-load, it finishes at once");
    f.li.pin(root, false);
}

static void testReloadAssetAndEdit() {
    AVER_INFO("reloadAsset, edit, commit, discard");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_levelinst_edit_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    Fixture f;
    f.contentDir = dir.string();
    f.levels["Edit.ocworld"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 10 0 0 0 0 0 1 M_Old id=a\nPLACE Meshes/b.ocmesh 0 0 5 0 0 0 1 id=b\n");
    f.begin(parse("OCWORLD 1\nLEVELINST Edit.ocworld pos 0 0 0 id=1\nENDLEVELINST\nLEVELINST Edit.ocworld pos 2000 0 0 id=2\nENDLEVELINST\n"));
    scene::World& w = scene::World::instance();
    scene::Entity i1 = scene::kInvalidEntity, i2 = scene::kInvalidEntity;
    for (u32 i = 0; i < f.li.count(); ++i) {
        const scene::Entity r = f.li.at(i);
        if (f.li.recordOf(r)->id == 1) i1 = r;
        if (f.li.recordOf(r)->id == 2) i2 = r;
    }
    check(i1 && i2 && f.lib.loadCount() == 1, "two instances of one asset, parsed once");

    // reloadAsset: handles and overrides survive, an added placement appears.
    f.li.setOverrides(i2, std::vector<fmt::OcPrefabOverride>{ovStr("a", fmt::kLiFieldMaterial, "M_Red")});
    fmt::OcWorldData grown = f.levels["Edit.ocworld"];
    grown.placements.push_back(parse("OCWORLD 1\nPLACE Meshes/c.ocmesh 0 0 9 0 0 0 1 id=c\n").placements[0]);
    f.lib.put("Edit.ocworld", grown);
    check(f.li.reloadAsset("Edit.ocworld"), "reloadAsset rebuilds the instances of the asset");
    check(f.li.find(i2, "c") != scene::kInvalidEntity && f.li.find(i1, "c") != scene::kInvalidEntity, "an added placement shows in every instance");
    const scene::Entity a2 = f.li.find(i2, "a");
    check(a2 != scene::kInvalidEntity &&
              std::string(aver_scene_material_name(w.component<scene::CMeshRenderer>(a2, scene::kComponentMeshRenderer)->material)) == "M_Red",
          "the override survived the reload; the root handle did too");

    // beginEdit shows the RAW asset; discard puts the overrides back.
    check(f.li.beginEdit(i2) && f.li.editing(i2) && f.li.anyDirtyEdit(), "beginEdit starts an edit");
    check(!f.li.beginEdit(i1), "only one instance is edited at a time");
    const scene::Entity ea = f.li.find(i2, "a");
    check(ea != scene::kInvalidEntity && w.component<scene::CMeshRenderer>(ea, scene::kComponentMeshRenderer)->material == aver_scene_material(0, "M_Old"),
          "while editing, the member shows the asset's own material, not the override");
    check(linkOf(ea).flags == scene::kLinkEditing, "members of an edited instance carry the editing flag");
    w.setLocalPosition(ea, Vec3{500.0f, 0.0f, 0.0f});
    check(f.li.discardEdit(i2) && !f.li.editing(i2) && !f.li.anyDirtyEdit(), "discardEdit ends the edit");
    const scene::Entity da = f.li.find(i2, "a");
    check(da != scene::kInvalidEntity && near(w.localTransform(da).position.x, 10.0f), "discard restores the asset's pose");
    check(std::string(aver_scene_material_name(w.component<scene::CMeshRenderer>(da, scene::kComponentMeshRenderer)->material)) == "M_Red",
          "...and the instance's overrides");

    // Edit, move a member, add an entity, commit: the asset is written and the OTHER instance rebuilt.
    check(f.li.beginEdit(i1), "edit instance 1");
    const scene::Entity ma = f.li.find(i1, "a");
    w.setLocalPosition(ma, Vec3{50.0f, 0.0f, 0.0f});
    const scene::Entity fresh = w.create("Meshes/new.ocmesh", i1, Transform{});
    w.addComponent(fresh, scene::kComponentMeshRenderer);
    std::string why;
    check(f.li.commitEdit(i1, &why), "commitEdit succeeds: " + why);
    check(!f.li.editing(i1), "the edit is over");
    fmt::OcWorldData saved;
    check(fmt::loadOcworld((dir / "Edit.ocworld").string(), saved) && saved.placements.size() == 4, "the child file was written with the new placement");
    bool aKept = false, allIds = true;
    for (const auto& p : saved.placements) { if (p.placementId == 0xa) aKept = true; if (!p.placementId) allIds = false; }
    check(aKept && allIds, "existing placements keep their ids; the new one got one");
    const scene::Entity oa = f.li.find(i2, "a");
    check(oa != scene::kInvalidEntity && near(w.localTransform(oa).position.x, 50.0f), "the other instance picked up the moved member");
    check(std::string(aver_scene_material_name(w.component<scene::CMeshRenderer>(oa, scene::kComponentMeshRenderer)->material)) == "M_Red",
          "...with its override intact");
    check(f.lib.get("Edit.ocworld")->data.placements.size() == 4, "the library holds the committed asset");
    std::filesystem::remove_all(dir, ec);
}

static void testBreak() {
    AVER_INFO("break");
    Fixture f;
    f.levels["Child"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 10 0 0 0 0 0 1 id=a\n");
    f.begin(parse("OCWORLD 1\nLEVELINST Child pos 700 0 0 rot 90 0 0 id=1\nENDLEVELINST\n"));
    scene::World& w = scene::World::instance();
    const scene::Entity root = f.li.at(0);
    const scene::Entity a = f.li.find(root, "a");
    flush();
    const Vec3 before = worldPos(a);
    check(f.li.breakInstance(root), "breakInstance succeeds");
    flush();
    check(f.li.count() == 0 && !w.valid(root), "the instance and its root are gone");
    check(w.valid(a) && !f.li.isMember(a), "the member is an ordinary entity now");
    const Vec3 after = worldPos(a);
    check(near(before.x, after.x) && near(before.y, after.y) && near(before.z, after.z), "it kept its world pose");
    check(f.releaseTotal() < f.acquireTotal(), "its mesh stays held for it");
    f.li.end();
    check(f.releaseTotal() == f.acquireTotal(), "and is released at end()");
    w.destroy(a);
    flush();
}

static void testClassesLightsDecals() {
    AVER_INFO("classes, lights, decals");
    Fixture f;
    fmt::OcWorldData lit = parse("OCWORLD 1\nPLACE Meshes/m.ocmesh 0 0 0 0 0 0 1 id=a\nPLACE Meshes/x.ocmesh 5 0 0 0 0 0 1 class MyActor id=7\n");
    fmt::OcLight l;
    l.name = "Lamp";
    l.x = 10; l.y = 0; l.z = 30;
    lit.lights.push_back(l);
    fmt::OcDecal d;
    d.name = "Stain";
    d.x = 0; d.y = 10; d.z = 0;
    lit.decals.push_back(d);
    f.levels["Lit"] = lit;
    f.begin(parse("OCWORLD 1\nLEVELINST Lit pos 100 0 0 id=1\nENDLEVELINST\n"));
    scene::World& w = scene::World::instance();
    const scene::Entity root = f.li.at(0);
    check(f.spawnedClasses.empty(), "a class placement waits for scripting");
    scene::Entity lamp = scene::kInvalidEntity, stain = scene::kInvalidEntity;
    for (const scene::Entity e : descendants(root)) {
        if (w.hasComponent(e, scene::kComponentLight)) lamp = e;
        if (w.hasComponent(e, scene::kComponentDecal)) stain = e;
    }
    check(lamp != scene::kInvalidEntity && w.parent(lamp) == root, "the child's light exists under the root");
    check(stain != scene::kInvalidEntity && w.parent(stain) == root, "the child's decal exists under the root");
    flush();
    check(lamp != scene::kInvalidEntity && near(worldPos(lamp).x, 110.0f) && near(worldPos(lamp).z, 30.0f), "the light sits in the child's frame");
    f.li.spawnPendingClasses();
    check(f.spawnedClasses.size() == 1 && f.spawnedClasses[0] == "MyActor", "spawnPendingClasses spawns the class through the hook");
    flush();
    const scene::Entity actor = f.li.find(root, "7");
    check(actor != scene::kInvalidEntity && near(worldPos(actor).x, 105.0f), "the actor is at the placement's composed world pose");
    f.li.spawnPendingClasses();
    check(f.spawnedClasses.size() == 1, "it is spawned once");
    f.li.end();
    check(f.destroyedClasses.size() == 1, "unrealise destroys classes through destroyClass");
    flush();
    check(!w.valid(lamp) && !w.valid(stain), "the light and decal go with the instance");
}

#if AVER_MODULE_PHYSICS
static void testAnimatedBodies() {
    AVER_INFO("animated bodies");
    if (!aver_phys_ready()) return;
    Fixture f;
    f.levels["Anim"] = parse("OCWORLD 1\nPLACE Meshes/f.ocmesh 0 0 0 0 0 0 1 anim Clips/spin.ocanim id=a\n");
    f.begin(parse("OCWORLD 1\nLEVELINST Anim pos 0 0 0 id=1\nENDLEVELINST\n"));
    check(f.li.animatedBodies().size() == 1, "an animated child placement's kinematic body is aggregated");
    f.li.end();
    flush();
    check(f.li.animatedBodies().empty(), "and removed with the instance");
}
#endif

static void testComponents() {
    AVER_INFO("COMPONENT records on members");
    Fixture f;
    f.levels["Kit"] = parse("OCWORLD 1\nPLACE Meshes/a.ocmesh 0 0 0 0 0 0 1 id=a\n  COMPONENT SkeletalMesh skeleton=Meshes/Char.ocskel\n"
                            "PLACE Meshes/b.ocmesh 0 0 0 0 0 0 1 id=b\n");
    f.begin(parse("OCWORLD 1\nLEVELINST Kit pos 0 0 0 id=1\nENDLEVELINST\nLEVELINST Kit pos 500 0 0 id=2\nENDLEVELINST\n"));
    scene::World& w = scene::World::instance();
    const u64 skel = fnv1a64(std::string_view("Meshes/Char.ocskel"));
    for (u32 i = 0; i < f.li.count(); ++i) {
        const scene::Entity root = f.li.at(i);
        const auto* sm = w.component<scene::CSkeletalMesh>(f.li.find(root, "a"), scene::kComponentSkeletalMesh);
        check(sm && sm->skeleton == skel, "instance " + std::to_string(i) + ": the member gets its child placement's component");
        check(!w.hasComponent(f.li.find(root, "b"), scene::kComponentSkeletalMesh), "instance " + std::to_string(i) + ": a placement without records gets none");
    }
}

int main() {
#if AVER_MODULE_PHYSICS
    aver_phys_init();
#endif
    testRealise();
    testNestedAndSharing();
    testCycle();
    testOverrides();
    testRevertAndRecords();
    testStreaming();
    testStagedRealise();
    testReloadAssetAndEdit();
    testBreak();
    testClassesLightsDecals();
    testComponents();
#if AVER_MODULE_PHYSICS
    testAnimatedBodies();
    aver_phys_shutdown();
#endif
    if (g_failures) { AVER_ERROR("LevelInstancesTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("LevelInstancesTest: all passed");
    return 0;
}
