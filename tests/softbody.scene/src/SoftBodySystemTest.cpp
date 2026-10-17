// The soft-body system headless: components, cloth (hanging, wind, palette, entity matrix, release, freeze,
// budgets), destruction (dents, shattering, explosions, cuts and repair, debris caps, rebuild budget), replay
// equality and the legacy CSoftBody path. Meshes, assets and palettes are injected; there is no GPU or device.
#include "SoftBodyFixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace sbfix;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

constexpr u64 kSheetMesh = 101, kSkinnedMesh = 102, kPaneMesh = 103, kPanelMesh = 104;
constexpr u64 kGlassAsset = 201, kMetalAsset = 202;

static f32 maxPlastic(const sb::Cage* c) {
    f32 m = 0.0f;
    if (c) for (const sb::Beam& b : c->beams) m = std::max(m, b.plastic);
    return m;
}

static u32 brokenOf(const sb::Cage* c) { return c ? sb::brokenBeamCount(*c) : 0u; }

static f32 meanOverTop(const Rig& r, scene::Entity e, u32 cols, int axis) {
    const auto& ps = r.particles(e);
    f32 sum = 0.0f;
    for (u32 c = 0; c < cols; ++c) {
        const Vec3& p = ps[c].pos;
        sum += axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
    }
    return sum / static_cast<f32>(cols);
}

static f32 meanOverBottom(const Rig& r, scene::Entity e, u32 cols, u32 rows, int axis) {
    const auto& ps = r.particles(e);
    f32 sum = 0.0f;
    for (u32 c = 0; c < cols; ++c) {
        const Vec3& p = ps[(rows - 1) * cols + c].pos;
        sum += axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
    }
    return sum / static_cast<f32>(cols);
}

// ---- components ------------------------------------------------------------------------------------------

static void testComponents() {
    AVER_INFO("-- components --");
    Rig r;
    scene::World& w = r.w;
    const u32 cloth = w.componentId("CCloth"), destruct = w.componentId("CDestructible");
    check(cloth != 0 && destruct != 0, "both are registered by name");
    check(w.componentSize(cloth) == 40 && w.componentSize(destruct) == 48, "sizes 40 and 48");
    check(w.componentVerified(cloth) && w.componentVerified(destruct), "their field tables cover the structs");
    check(sb::SoftBodySystem::registerComponents(w) == cloth, "registration is idempotent");
    const scene::FieldDesc* out = w.field(w.fieldId("CCloth.outState"));
    const scene::FieldDesc* outD = w.field(w.fieldId("CDestructible.outDamage"));
    const scene::FieldDesc* in = w.field(w.fieldId("CCloth.flags"));
    check(out && out->readOnly && outD && outD->readOnly && in && !in->readOnly, "OUT fields are read-only, inputs are not");

    r.stores.meshes[kSheetMesh] = sheetMesh(5, 5, 10.0f, Plane::XZ);
    const scene::Entity e = r.entity("cloth", kSheetMesh, Vec3{0, 0, 200});
    auto* c = sb::SoftBodySystem::attachCloth(w, e, sb::SoftPreset::ClothHeavy);
    check(c && c->preset == 1 && c->flags == 0 && c->windScale == 0.0f, "attach writes the preset over zero-filled storage");
    check(w.hasComponent(e, scene::kComponentSoftBody), "and adds the CSoftBody marker");
    check(sb::SoftBodySystem::attachDestructible(w, e, sb::SoftPreset::Metal) == nullptr, "cloth and destructible do not share an entity");
    const scene::Entity bare = w.create("bare");
    r.made.push_back(bare);
    check(sb::SoftBodySystem::attachCloth(w, bare, sb::SoftPreset::ClothLight) == nullptr, "an entity with no mesh renderer is refused");

    // Zero-filled storage added raw still means the useful state.
    const scene::Entity raw = r.entity("raw", kSheetMesh, Vec3{500, 0, 200});
    w.addComponent(raw, scene::kComponentSoftBody);
    w.addComponent(raw, cloth);
    r.run(0.5f);
    check(r.sys.pieceCount(raw) == 1 && r.sys.state(raw).found, "a raw zero-filled CCloth simulates (preset 0 = cloth light)");
    check(r.sys.pieceCount(e) == 1, "and so does the attached one");
}

// ---- cloth -------------------------------------------------------------------------------------------------

static void testClothHangsAndWind() {
    AVER_INFO("-- a flag hangs, the wind blows it, kClothNoWind does not --");
    Rig r;
    r.stores.meshes[kSheetMesh] = sheetMesh(10, 10, 10.0f, Plane::YZ);
    const scene::Entity still = r.entity("still", kSheetMesh, Vec3{0, 0, 300});
    const scene::Entity blown = r.entity("blown", kSheetMesh, Vec3{0, 400, 300});
    const scene::Entity immune = r.entity("immune", kSheetMesh, Vec3{0, 800, 300});
    sb::SoftBodySystem::attachCloth(r.w, still, sb::SoftPreset::ClothLight);
    sb::SoftBodySystem::attachCloth(r.w, blown, sb::SoftPreset::ClothLight);
    auto* nw = sb::SoftBodySystem::attachCloth(r.w, immune, sb::SoftPreset::ClothLight);
    nw->flags |= sb::kClothNoWind;

    r.run(3.0f);
    check(r.sys.pieceCount(still) == 1 && !r.particles(still).empty(), "the flag is built");
    const f32 topZ = meanOverTop(r, still, 10, 2), botZ = meanOverBottom(r, still, 10, 10, 2);
    check(std::fabs(topZ - 300.0f) < 0.5f, "its top edge stays where it is pinned");
    check(botZ < topZ - 70.0f && botZ > topZ - 100.0f, "it hangs about its 90 cm");
    const sb::SoftBodyState st = r.sys.state(still);
    check(st.found && st.pieces == 1 && st.brokenEdges == 0, "state reports one piece and no damage");

    r.sys.setWind(Vec3{600, 0, 0}, 0.0f, 0.0f, 0.0f);
    r.run(3.0f);
    const f32 x1 = meanOverBottom(r, blown, 10, 10, 0);
    const f32 x2 = meanOverBottom(r, immune, 10, 10, 0);
    AVER_INFO("  bottom deflection with wind {:.1f} cm, with kClothNoWind {:.1f} cm", x1, x2);
    check(x1 > 5.0f, "a wind along +X blows the flag downwind");
    check(std::fabs(x2) < 3.0f, "kClothNoWind leaves it hanging");
}

static void testPaletteAndEntityMatrix() {
    AVER_INFO("-- a skinned cape follows an injected palette; the entity matrix reaches the body --");
    {
        Rig r;
        r.sys.setPaletteSource(&PaletteSource::get, &r.palette);
        r.stores.meshes[kSkinnedMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ, true);
        const scene::Entity e = r.entity("cape", kSkinnedMesh, Vec3{0, 0, 200});
        auto* sm = static_cast<scene::CSkeletalMesh*>(r.w.addComponent(e, scene::kComponentSkeletalMesh));
        sm->skeleton = 77;
        sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight);
        r.run(1.0f);
        const f32 x0 = meanOverTop(r, e, 8, 0);
        r.palette.mats[0] = Mat4::translation(Vec3{100, 0, 0});
        r.run(2.0f);
        const f32 x1 = meanOverTop(r, e, 8, 0);
        check(std::fabs((x1 - x0) - 100.0f) < 1.5f, "the pinned row follows a palette that moved 100 cm");
        check(meanOverTop(r, e, 8, 2) > 198.0f, "and keeps its height");
    }
    {
        Rig r;
        r.stores.meshes[kSheetMesh] = sheetMesh(10, 6, 10.0f, Plane::XZ);
        const scene::Entity e = r.entity("banner", kSheetMesh, Vec3{0, 0, 200});
        sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothHeavy);
        r.run(1.0f);
        check(meanOverTop(r, e, 10, 0) > 40.0f, "built along +X");
        r.w.setLocalRotation(e, Quat::fromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f));
        r.run(2.0f);
        const auto& ps = r.particles(e);
        f32 maxX = 0.0f, maxY = 0.0f;
        for (u32 c = 0; c < 10; ++c) { maxX = std::max(maxX, std::fabs(ps[c].pos.x)); maxY = std::max(maxY, std::fabs(ps[c].pos.y)); }
        check(maxY > 85.0f && maxX < 3.0f, "a 90 degree yaw of the entity turns the pinned banner onto the Y axis");
    }
}

static void testReleaseAndFreeze() {
    AVER_INFO("-- release drops a cape; distance freezes and the budget --");
    {
        Rig r;
        r.stores.meshes[kSheetMesh] = sheetMesh(6, 6, 10.0f, Plane::XZ);
        const scene::Entity e = r.entity("cape", kSheetMesh, Vec3{0, 0, 400});
        auto* c = sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight);
        r.run(1.0f);
        const f32 before = meanOverTop(r, e, 6, 2);
        c->flags |= sb::kClothRelease;
        r.run(1.5f);
        check(c->flags == 0, "the system clears the one-shot flag");
        check(meanOverTop(r, e, 6, 2) < before - 50.0f, "the unpinned cape falls");
        check((c->outState & sb::kStateReleased) != 0, "and reports released");
    }
    {
        Rig r;
        r.stores.meshes[kSheetMesh] = sheetMesh(6, 6, 10.0f, Plane::XZ);
        const scene::Entity a = r.entity("a", kSheetMesh, Vec3{0, 0, 400});
        const scene::Entity b = r.entity("b", kSheetMesh, Vec3{1000, 0, 400});
        sb::SoftBodySystem::attachCloth(r.w, a, sb::SoftPreset::ClothLight);
        sb::SoftBodySystem::attachCloth(r.w, b, sb::SoftPreset::ClothLight);
        r.sys.setViewPos(Vec3{0, 0, 400});
        r.run(0.5f);
        check(r.sys.stats().frozenPieces == 0, "nothing freezes inside the distance");
        r.sys.setViewPos(Vec3{20000, 0, 400});
        r.run(0.2f);
        check(r.sys.stats().frozenPieces == 2, "beyond the freeze distance both hold their pose");
        const Vec3 frozenAt = r.particles(a).back().pos;
        r.run(1.0f);
        check(dist(r.particles(a).back().pos, frozenAt) < 1e-3f, "and cost nothing: the pose does not change");
        r.sys.setViewPos(Vec3{0, 0, 400});
        r.run(0.3f);
        check(r.sys.stats().frozenPieces <= 1, "coming back wakes the near one (the far one stays out of reach)");
        r.sys.options().maxActiveParticles = 40;
        r.sys.setViewPos(Vec3{0, 0, 400});
        r.run(0.3f);
        check(r.sys.stats().frozenPieces >= 1 && r.sys.stats().activeParticles <= 40 + 36, "the active-particle budget freezes the farthest");
    }
}

// ---- destruction ------------------------------------------------------------------------------------------

static void testDentAndCloth() {
    AVER_INFO("-- a metal panel keeps a dent, a cloth does not --");
    Rig r;
    r.stores.meshes[kPanelMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ);
    r.stores.assets[kMetalAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
    const scene::Entity metal = r.entity("metal", kPanelMesh, Vec3{0, 0, 300});
    const scene::Entity cloth = r.entity("cloth", kPanelMesh, Vec3{500, 0, 300});
    sb::SoftBodySystem::attachDestructible(r.w, metal, sb::SoftPreset::Metal, kMetalAsset);
    sb::SoftBodySystem::attachCloth(r.w, cloth, sb::SoftPreset::ClothHeavy);
    r.run(1.0f);

    // Stiff metal swings out of plane almost without stretching: 10 J over 15 cm reached 0.3% strain, short of its 1% yield.
    check(r.sys.damage(metal, Vec3{35, 0, 265}, 40.0f, 400.0f, Vec3{0, 1, 0}, 2), "damage is queued");
    check(r.sys.damage(cloth, Vec3{535, 0, 265}, 40.0f, 400.0f, Vec3{0, 1, 0}, 2), "so is the same hit on the cloth");
    r.run(4.0f);

    const sb::Cage* mc = r.sys.cageOf(metal);
    const sb::Cage* cc = r.sys.cageOf(cloth);
    check(mc && cc, "both have cages");
    AVER_INFO("  metal: plastic {:.3f} cm, broken {}; cloth: plastic {:.3f} cm", maxPlastic(mc), brokenOf(mc), maxPlastic(cc));
    check(maxPlastic(mc) > 0.0f || brokenOf(mc) > 0, "the metal yielded or broke");
    check(maxPlastic(cc) == 0.0f && brokenOf(cc) == 0, "the cloth neither yielded nor broke");
    if (mc && maxPlastic(mc) > 0.0f && brokenOf(mc) == 0) {
        f32 maxDisp = 0.0f;
        for (const sb::Particle& p : mc->particles) maxDisp = std::max(maxDisp, std::fabs(p.pos.y - p.rest.y));
        check(maxDisp > 0.5f, "and the dent is still there seconds later");
    }
}

static void testGlassShatters() {
    AVER_INFO("-- a glass pane shatters: Torn, then Detached, pieces capped, shards to dust --");
    Rig r;
    r.stores.meshes[kPaneMesh] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kGlassAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity pane = r.entity("pane", kPaneMesh, Vec3{0, 0, 400});
    auto* d = sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassAsset);
    r.run(1.0f);
    check(r.sys.pieceCount(pane) == 1 && aver_phys_sb_count() == 1, "one intact pane, one body");

    // A disk of breakage across the full width halfway down (reach ~0.85 of the radius for this energy): the top strip stays
    // pinned with live triangles, the rest comes loose. Centred on the top the strip would die and no piece would detach.
    r.sys.damage(pane, Vec3{45, 0, 350}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
    r.run(0.5f);
    check(r.kindCount(sb::SoftEventKind::Torn) >= 1, "edges tore");
    check(r.kindCount(sb::SoftEventKind::Detached) >= 1, "a piece detached");
    check(r.firstOf(sb::SoftEventKind::Torn) < r.firstOf(sb::SoftEventKind::Detached), "Torn comes before Detached");
    const u32 pieces = r.sys.pieceCount(pane);
    check(pieces >= 2 && pieces <= 16, "pieces are between 2 and the asset's maxPieces");
    check(aver_phys_sb_count() == static_cast<i32>(pieces), "every live piece has exactly one body");
    check(d->outBroken > 0 && d->outDamage > 0.0f && d->outPieces == pieces, "the OUT fields report it");
    const sb::Cage* c = r.sys.cageOf(pane);
    check(c && c->particles.size() >= 100, "the cage keeps its particles (tears only add duplicates)");
    check(r.kindCount(sb::SoftEventKind::Dust) >= 1, "shards under the minimum triangle count became dust");

    // The root keeps its body and its entity; the debris are scene entities of their own.
    u32 count = 0;
    const sb::PieceView* views = r.sys.pieces(count);
    u32 debris = 0, roots = 0;
    for (u32 i = 0; i < count; ++i) {
        if (views[i].entity == pane) ++roots;
        else ++debris;
        check(views[i].indexCount % 3 == 0 && views[i].vertexCount > 0, "a view has whole triangles");
    }
    check(roots == 1 && debris == pieces - 1, "one root view and one entity per detached piece");
}

static void testExplodeCutRepair() {
    AVER_INFO("-- explode, cut and repair --");
    Rig r;
    r.stores.meshes[kPaneMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ);
    r.stores.assets[kGlassAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity a = r.entity("a", kPaneMesh, Vec3{0, 0, 400});
    const scene::Entity b = r.entity("b", kPaneMesh, Vec3{150, 0, 400});
    const scene::Entity far = r.entity("far", kPaneMesh, Vec3{3000, 0, 400});
    for (const scene::Entity e : {a, b, far}) sb::SoftBodySystem::attachDestructible(r.w, e, sb::SoftPreset::Glass, kGlassAsset);
    r.run(1.0f);

    const u32 touched = r.sys.explode(Vec3{80, -30, 365}, 300.0f, 400.0f, 900.0f);
    check(touched == 2, "explode reports the two panes in reach");
    r.run(0.6f);
    check(brokenOf(r.sys.cageOf(a)) > 0 && brokenOf(r.sys.cageOf(b)) > 0, "both panes in reach break");
    check(r.sys.pieceCount(far) == 1 && brokenOf(r.sys.cageOf(far)) == 0, "the far one is untouched");

    // A scripted cut, then repair.
    const scene::Entity cut = r.entity("cut", kPaneMesh, Vec3{6000, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, cut, sb::SoftPreset::Glass, kGlassAsset);
    r.run(0.5f);
    check(r.sys.tearAt(cut, Vec3{6035, 0, 365}, 12.0f) == 1, "tearAt queues a cut");
    r.run(0.5f);
    check(brokenOf(r.sys.cageOf(cut)) > 0, "the cut broke edges");
    const u32 tornBefore = r.kindCount(sb::SoftEventKind::Torn);
    check(tornBefore >= 1, "and said so");
    check(r.sys.repair(cut), "repair queues");
    r.run(0.5f);
    check(r.sys.pieceCount(cut) == 1 && brokenOf(r.sys.cageOf(cut)) == 0, "repair restores the topology");
    check(r.kindCount(sb::SoftEventKind::Repaired) == 1, "and reports it");
    u32 views = 0;
    r.sys.pieces(views);
    check(static_cast<u32>(aver_phys_sb_count()) == views, "the body count matches the live pieces");
}

static void testDebrisCapsAndBudgets() {
    AVER_INFO("-- debris eviction and the rebuild budget --");
    {
        Rig r;
        r.stores.meshes[kPaneMesh] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        r.stores.assets[kGlassAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        r.sys.options().maxLivePieces = 1;
        r.sys.options().despawnShrinkS = 0.2f;
        const scene::Entity pane = r.entity("pane", kPaneMesh, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassAsset);
        r.run(0.5f);
        r.sys.damage(pane, Vec3{45, 0, 350}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
        u32 spawned = 0;
        for (f32 t = 0.0f; t < 0.4f; t += 1.0f / 60.0f) {
            r.frame();
            spawned = std::max(spawned, r.sys.stats().debris);
        }
        check(spawned >= 2, "debris spawned past the cap");
        r.run(2.5f);
        check(r.sys.stats().debris <= 1, "the debris cap evicts the oldest-farthest by shrinking them away");
        check(r.sys.stats().pieces >= 1, "the root stays");
    }
    {
        Rig r;
        r.stores.meshes[kPaneMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ);
        r.stores.assets[kGlassAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        r.sys.options().maxRebuildsPerFrame = 1;
        scene::Entity panes[3];
        for (int i = 0; i < 3; ++i) {
            panes[i] = r.entity("pane", kPaneMesh, Vec3{static_cast<f32>(i) * 400.0f, 0, 400});
            sb::SoftBodySystem::attachDestructible(r.w, panes[i], sb::SoftPreset::Glass, kGlassAsset);
            r.run(0.2f);
        }
        r.run(0.5f);
        for (int i = 0; i < 3; ++i) r.sys.damage(panes[i], Vec3{static_cast<f32>(i) * 400.0f + 35, 0, 360}, 50.0f, 500.0f, Vec3{0, 1, 0}, 2);
        r.frame();
        u32 torn = 0;
        for (int i = 0; i < 3; ++i) torn += r.sys.pieceCount(panes[i]) > 1 ? 1u : 0u;
        check(torn <= 1, "with one rebuild per frame at most one pane tears on the frame it was hit");
        r.run(0.5f);
        torn = 0;
        for (int i = 0; i < 3; ++i) torn += r.sys.pieceCount(panes[i]) > 1 ? 1u : 0u;
        check(torn == 3, "the rest follow on later frames; none is lost");
    }
}

// ---- replay equality ----------------------------------------------------------------------------------------

struct Replay {
    std::vector<f32> positions;
    std::vector<f32> eventBits;
    u32 pieces = 0;
};

static Replay runScript() {
    Rig r;
    // One entry: entity ids differ between two runs (generations advance), so a script that leaned on their
    // order would compare unlike things. The wind rides along to pin the aero determinism too.
    r.stores.meshes[kPaneMesh] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kGlassAsset] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity pane = r.entity("pane", kPaneMesh, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassAsset);
    for (int t = 0; t < 600; ++t) {
        if (t == 60) r.sys.setWind(Vec3{400, 0, 0}, 100.0f, 0.5f, 50.0f);
        if (t == 120) r.sys.damage(pane, Vec3{45, 0, 350}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
        if (t == 300) r.sys.tearAt(pane, Vec3{45, 0, 330}, 15.0f);
        r.frame();
    }
    Replay out;
    u32 count = 0;
    const sb::PieceView* views = r.sys.pieces(count);
    for (u32 i = 0; i < count; ++i)
        for (u32 v = 0; v < views[i].vertexCount; ++v) {
            out.positions.push_back(views[i].world[v].x);
            out.positions.push_back(views[i].world[v].y);
            out.positions.push_back(views[i].world[v].z);
        }
    for (const sb::SoftEvent& ev : r.events) {
        // Entity handles differ between runs (generations advance); everything else must not.
        out.eventBits.push_back(static_cast<f32>(ev.kind));
        out.eventBits.push_back(static_cast<f32>(ev.piece));
        out.eventBits.push_back(static_cast<f32>(ev.beam));
        out.eventBits.push_back(static_cast<f32>(ev.seq));
        out.eventBits.push_back(ev.point.x); out.eventBits.push_back(ev.point.y); out.eventBits.push_back(ev.point.z);
        out.eventBits.push_back(ev.value);
    }
    out.pieces = count;
    return out;
}

static void testReplayEquality() {
    AVER_INFO("-- replay equality over 600 ticks --");
    const Replay a = runScript();
    const Replay b = runScript();
    check(!a.positions.empty() && !a.eventBits.empty(), "the script produced geometry and events");
    check(a.pieces == b.pieces, "the same piece count");
    check(a.positions.size() == b.positions.size() && std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * sizeof(f32)) == 0,
          "vertex positions are bit identical");
    check(a.eventBits.size() == b.eventBits.size() && std::memcmp(a.eventBits.data(), b.eventBits.data(), a.eventBits.size() * sizeof(f32)) == 0,
          "events (kinds, ids, sequence numbers) are bit identical");
}

// ---- legacy ---------------------------------------------------------------------------------------------------

static void testLegacy() {
    AVER_INFO("-- a plain CSoftBody keeps working --");
    {
        Rig r;
        r.stores.meshes[kSheetMesh] = sheetMesh(5, 5, 10.0f, Plane::XY);
        const scene::Entity e = r.entity("jiggle", kSheetMesh, Vec3{0, 0, 300});
        r.w.addComponent(e, scene::kComponentSoftBody);
        r.run(0.2f);
        const f32 z0 = r.particles(e).empty() ? 0.0f : r.particles(e)[12].pos.z;
        r.run(0.5f);
        check(!r.particles(e).empty() && r.particles(e)[12].pos.z < z0 - 20.0f, "a mesh with no skin simulates free");
        check(r.sys.pieceCount(e) == 1, "as one piece");
    }
    {
        Rig r;
        r.sys.setPaletteSource(&PaletteSource::get, &r.palette);
        r.stores.meshes[kSkinnedMesh] = sheetMesh(5, 5, 10.0f, Plane::XY, true);
        const scene::Entity e = r.entity("jiggle", kSkinnedMesh, Vec3{0, 0, 300});
        auto* sm = static_cast<scene::CSkeletalMesh*>(r.w.addComponent(e, scene::kComponentSkeletalMesh));
        sm->skeleton = 5;
        auto* c = static_cast<scene::CSoftBody*>(r.w.addComponent(e, scene::kComponentSoftBody));
        c->maxDistanceCm = 5.0f;
        r.run(0.5f);
        const f32 x0 = r.particles(e)[12].pos.x;
        r.palette.mats[0] = Mat4::translation(Vec3{80, 0, 0});
        r.run(1.5f);
        check(std::fabs((r.particles(e)[12].pos.x - x0) - 80.0f) < 6.0f, "a skinned mesh now follows its palette within maxDistance (skin is finally called)");
    }
}

int main() {
    AVER_INFO("SoftBodySystemTest");
    testComponents();
    testClothHangsAndWind();
    testPaletteAndEntityMatrix();
    testReleaseAndFreeze();
    testDentAndCloth();
    testGlassShatters();
    testExplodeCutRepair();
    testDebrisCapsAndBudgets();
    testReplayEquality();
    testLegacy();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
