// Soft-body destruction depth, headless (docs/plans/wave10/SOFTBODY_DEPTH.md): fields (pulses, continuous and transient volumes, the
// CSoftField component, release, kill, sleep), the reveal handshake and the draw budget, thick breaks (back skin, walls, the
// interior-material cap entity, cap LOD), break effects (particles and sounds under budgets), saved damage (capture, replay,
// stale keys, merged logs), baking of settled debris, the proxy cage, and replay equality with all of it on. Meshes, assets and
// palettes are injected and nothing is drawn; physics is the real Jolt world.
#include "SoftBodyFixtures.hpp"

#include "aver/scene/scene_abi.h"
#include "aver/softbody/SoftState.hpp"

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

constexpr u64 kSheet = 401, kPane = 402, kPanel = 403, kPlate = 404, kBigPane = 405, kWide = 406;
constexpr u64 kGlass = 501, kMetal = 502, kGlassFx = 503, kGlassInterior = 504, kGlassNoBake = 505, kMetalPlate = 506, kMetalPlateProxy = 507, kGlassBig = 508;

static sb::FieldSpec spec(sb::FieldKind kind, sb::FieldShape shape, Vec3 centre, Vec3 extents) {
    sb::FieldSpec f;
    f.kind = kind;
    f.shape = shape;
    f.world = Mat4::translation(centre);
    f.extents = extents;
    return f;
}

static f32 meanY(const Rig& r, scene::Entity e) {
    const auto& ps = r.particles(e);
    f32 sum = 0.0f;
    for (const sb::Particle& p : ps) sum += p.pos.y;
    return ps.empty() ? 0.0f : sum / static_cast<f32>(ps.size());
}

static f32 meanZ(const Rig& r, scene::Entity e) {
    const auto& ps = r.particles(e);
    f32 sum = 0.0f;
    for (const sb::Particle& p : ps) sum += p.pos.z;
    return ps.empty() ? 0.0f : sum / static_cast<f32>(ps.size());
}

// debrisOf lists every non-root view in the system, so with two panes in the rig a pane's own debris is told apart by where it is.
static Vec3 meanDebrisNear(const Rig& r, scene::Entity root, f32 x, f32 reachCm, u32* count = nullptr) {
    Vec3 sum{0, 0, 0};
    u32 n = 0;
    for (const sb::PieceView* v : debrisOf(r, root)) {
        const Vec3 m = meanOf(*v);
        if (std::fabs(m.x - x) > reachCm) continue;
        sum += m;
        ++n;
    }
    if (count) *count = n;
    return n ? sum / static_cast<f32>(n) : Vec3{0, 0, 0};
}

static void shatter(Rig& r, scene::Entity pane, f32 x = 0.0f, f32 energy = 600.0f) {
    r.sys.damage(pane, Vec3{x + 45, 0, 350}, 55.0f, energy, Vec3{0, 1, 0}, 2);
}

// ---- fields -----------------------------------------------------------------------------------------------------

static void testForceFields() {
    AVER_INFO("-- radial and directional force: a pulse moves cloth; negative strength pulls; targets and reach count --");
    Rig r;
    r.stores.meshes[kSheet] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity cloth = r.entity("cloth", kSheet, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachCloth(r.w, cloth, sb::SoftPreset::ClothHeavy);
    const scene::Entity glass = r.entity("glass", kPane, Vec3{1000, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, glass, sb::SoftPreset::Glass, kGlass);
    r.run(1.5f);

    const f32 y0 = meanY(r, cloth);
    sb::FieldSpec push = spec(sb::FieldKind::RadialForce, sb::FieldShape::Sphere, Vec3{45, -20, 355}, Vec3{150, 0, 0});
    push.strength = 600.0f;
    push.targets = sb::kFieldTargetCloth;
    check(r.sys.pulseField(push) == 1, "a radial pulse over the cloth reports one piece in reach (the glass is out of reach and not targeted)");
    r.run(0.4f);
    const f32 y1 = meanY(r, cloth);
    AVER_INFO("  mean y {:.2f} -> {:.2f}", y0, y1);
    check(y1 - y0 > 3.0f, "the cloth is pushed away from the centre (+Y)");

    r.run(2.0f);
    const f32 y2 = meanY(r, cloth);
    sb::FieldSpec pull = spec(sb::FieldKind::RadialForce, sb::FieldShape::Sphere, Vec3{45, -40, 355}, Vec3{150, 0, 0});
    pull.strength = -600.0f;
    r.sys.pulseField(pull);
    r.run(0.4f);
    check(meanY(r, cloth) - y2 < -3.0f, "a negative strength pulls it toward the centre (-Y)");

    r.run(2.0f);
    const f32 y3 = meanY(r, cloth);
    sb::FieldSpec gust = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
    gust.dir = Vec3{0, 1, 0};
    gust.strength = 500.0f;
    gust.targets = sb::kFieldTargetCloth;   // Everywhere is unbounded: without this the glass, in range of every volume, is gusted too
    r.sys.pulseField(gust);
    r.run(0.4f);
    check(meanY(r, cloth) - y3 > 3.0f, "a directional pulse follows its axis");

    const f32 glassBefore = meanZ(r, glass);
    r.run(0.2f);
    check(std::fabs(meanZ(r, glass) - glassBefore) < 0.5f, "the glass, outside every volume, did not move");
}

static void testDebrisFieldAndTargets() {
    AVER_INFO("-- a continuous box field accelerates only the debris inside it; targets exclude the rest --");
    Rig r;
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity a = r.entity("A", kPane, Vec3{0, 0, 400});
    const scene::Entity b = r.entity("B", kPane, Vec3{1500, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, a, sb::SoftPreset::Glass, kGlass);
    sb::SoftBodySystem::attachDestructible(r.w, b, sb::SoftPreset::Glass, kGlass);
    r.run(0.5f);
    shatter(r, a, 0.0f);
    shatter(r, b, 1500.0f);
    r.run(0.4f);
    u32 aDebris = 0, bDebris = 0;
    const Vec3 a0 = meanDebrisNear(r, a, 45.0f, 700.0f, &aDebris), b0 = meanDebrisNear(r, b, 1545.0f, 700.0f, &bDebris);
    check(aDebris > 0 && bDebris > 0, "both panes shed debris");

    sb::FieldSpec f = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Box, Vec3{45, 0, 330}, Vec3{300, 300, 300});
    f.dir = Vec3{0, 1, 0};
    f.strength = 1500.0f;
    f.targets = sb::kFieldTargetDebris;
    const u32 handle = r.sys.startField(f, 0.4f);
    check(handle != 0, "startField returns a handle");
    r.run(0.5f);
    const Vec3 a1 = meanDebrisNear(r, a, 45.0f, 700.0f), b1 = meanDebrisNear(r, b, 1545.0f, 700.0f);
    AVER_INFO("  debris dy: inside {:.1f}, outside {:.1f}", a1.y - a0.y, b1.y - b0.y);
    check((a1.y - a0.y) - (b1.y - b0.y) > 5.0f, "the debris inside the volume is pushed along +Y more than the debris outside");
    check(std::fabs(r.particles(a)[0].pos.y) < 0.5f, "the pinned top of the pane, a destructible, is not a target and stays put");
    check(r.sys.stats().transientFields == 0, "the field expired with its lifetime");
    check(!r.sys.stopField(handle), "and stopping it again says it was not live");

    sb::FieldSpec cloth = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
    cloth.dir = Vec3{0, 1, 0};
    cloth.strength = 1500.0f;
    cloth.targets = sb::kFieldTargetCloth;
    const u32 reach = r.sys.pulseField(cloth);
    check(reach == 0, "a cloth-only field reaches none of two destructibles and their debris");
}

static void testDamageField() {
    AVER_INFO("-- a damage field breaks a pane past its toughness and respects indestructible --");
    Rig r;
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    const scene::Entity soft = r.entity("soft", kPane, Vec3{0, 0, 400});
    const scene::Entity tough = r.entity("tough", kPane, Vec3{800, 0, 400});
    const scene::Entity gentle = r.entity("gentle", kPane, Vec3{1600, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, soft, sb::SoftPreset::Glass, kGlass);
    auto* t = sb::SoftBodySystem::attachDestructible(r.w, tough, sb::SoftPreset::Glass, kGlass);
    t->flags |= sb::kDestructIndestructible;
    sb::SoftBodySystem::attachDestructible(r.w, gentle, sb::SoftPreset::Glass, kGlass);
    r.run(0.5f);

    sb::FieldSpec weak = spec(sb::FieldKind::Damage, sb::FieldShape::Box, Vec3{1645, 0, 355}, Vec3{200, 200, 200});
    weak.energyJ = 0.01f;
    r.sys.pulseField(weak);
    r.run(0.3f);
    check(brokenCount(r.sys.cageOf(gentle)) == 0, "a field whose energy is under the toughness breaks nothing");

    sb::FieldSpec strong = spec(sb::FieldKind::Damage, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
    strong.energyJ = 3000.0f;
    check(r.sys.pulseField(strong) >= 3, "a field over everything reports the pieces in reach");
    r.run(0.6f);
    check(brokenCount(r.sys.cageOf(soft)) > 0, "past the toughness the pane breaks");
    check(brokenCount(r.sys.cageOf(tough)) == 0 && r.sys.pieceCount(tough) == 1, "the indestructible one is untouched");
    check(r.sys.lastHit(soft).valid && r.sys.lastHit(soft).broken > 0, "the Tuning HUD sees the deposit");
}

static void testReleaseAnchors() {
    AVER_INFO("-- release anchors: a hinge-pinned panel falls, a cloth loses only the pins inside the volume --");
    {
        Rig r;
        r.stores.meshes[kPanel] = sheetMesh(8, 8, 10.0f, Plane::XZ);
        r.stores.assets[kMetal] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
        const scene::Entity door = r.entity("door", kPanel, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, door, sb::SoftPreset::Metal, kMetal);
        r.run(1.0f);
        const f32 z0 = meanZ(r, door);
        check(z0 > 355.0f, "the panel hangs from its pinned top");
        sb::FieldSpec f = spec(sb::FieldKind::ReleaseAnchors, sb::FieldShape::Box, Vec3{35, 0, 400}, Vec3{80, 30, 12});
        r.sys.pulseField(f);
        r.run(1.0f);
        check(z0 - meanZ(r, door) > 100.0f, "released from its hinge it falls");
        check(r.kindCount(sb::SoftEventKind::Released) >= 1, "and says Released");
    }
    {
        Rig r;
        r.stores.meshes[kSheet] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        const scene::Entity cape = r.entity("cape", kSheet, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachCloth(r.w, cape, sb::SoftPreset::ClothHeavy);
        r.run(1.0f);
        // Six of the ten pins (x = 0 to 50). Two would leave a corner a triangulated sheet still holds up (measured: 0.3 cm of sag).
        sb::FieldSpec f = spec(sb::FieldKind::ReleaseAnchors, sb::FieldShape::Sphere, Vec3{0, 0, 400}, Vec3{55, 0, 0});
        r.sys.pulseField(f);
        r.run(1.5f);
        check(r.particles(cape)[0].pos.z < 395.0f, "the pinned corner inside the volume let go and sagged");
        check(std::fabs(r.particles(cape)[9].pos.z - 400.0f) < 2.0f, "the pin at the other end, outside it, still holds");
    }
}

static void testKillAndSleep() {
    AVER_INFO("-- kill shrinks debris and leaves roots unless targeted; sleep holds pieces and a stop releases them --");
    {
        Rig r;
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        r.sys.options().despawnShrinkS = 0.2f;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        const scene::Entity victim = r.entity("victim", kPane, Vec3{800, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlass);
        sb::SoftBodySystem::attachDestructible(r.w, victim, sb::SoftPreset::Glass, kGlass);
        r.run(0.5f);
        shatter(r, pane);
        r.run(0.4f);
        check(r.sys.stats().debris > 0, "debris exists");
        sb::FieldSpec kill = spec(sb::FieldKind::Kill, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
        kill.targets = sb::kFieldTargetDebris;
        r.sys.pulseField(kill);
        r.run(0.8f);
        check(r.sys.stats().debris == 0, "every piece of debris was shrunk away and destroyed");
        check(r.sys.pieceCount(pane) >= 1 && r.sys.pieceCount(victim) == 1, "the roots are untouched");

        sb::FieldSpec all = kill;
        all.targets = sb::kFieldTargetDestructible;
        all.shape = sb::FieldShape::Sphere;
        all.world = Mat4::translation(Vec3{845, 0, 355});
        all.extents = Vec3{400, 0, 0};
        r.sys.pulseField(all);
        r.run(0.3f);
        check(r.sys.pieceCount(victim) == 0 && !visible(r, victim), "targeting destructibles hides the root and drops its pieces");
        r.sys.repair(victim);
        r.run(0.5f);
        check(r.sys.pieceCount(victim) == 1 && visible(r, victim), "Repair brings it back");
    }
    {
        Rig r;
        r.stores.meshes[kSheet] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        const scene::Entity flag = r.entity("flag", kSheet, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachCloth(r.w, flag, sb::SoftPreset::ClothLight);
        r.sys.setWind(Vec3{600, 0, 0}, 100.0f, 0.5f, 0.0f);
        r.run(1.5f);
        sb::FieldSpec hold = spec(sb::FieldKind::Sleep, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
        const u32 handle = r.sys.startField(hold, 30.0f);
        r.run(0.3f);
        check(r.sys.stats().heldPieces == 1, "the sleep field holds the flag");
        std::vector<Vec3> p1;
        for (const sb::Particle& q : r.particles(flag)) p1.push_back(q.pos);
        r.run(0.5f);
        f32 moved = 0.0f;
        for (usize i = 0; i < p1.size(); ++i) moved = std::max(moved, dist(p1[i], r.particles(flag)[i].pos));
        check(moved < 1e-3f, "a held flag does not move in the wind");
        check(r.sys.stopField(handle), "stopping the field");
        r.run(0.6f);
        moved = 0.0f;
        for (usize i = 0; i < p1.size(); ++i) moved = std::max(moved, dist(p1[i], r.particles(flag)[i].pos));
        check(r.sys.stats().heldPieces == 0 && moved > 0.2f, "releases it, and the wind carries on");
    }
}

static void testOnceTransientAndParent() {
    AVER_INFO("-- kFieldOnce fires one tick; a transient field expires; a parent moves a component field --");
    Rig r;
    r.stores.meshes[kSheet] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    const scene::Entity cloth = r.entity("cloth", kSheet, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachCloth(r.w, cloth, sb::SoftPreset::ClothHeavy);
    r.run(1.5f);

    // Once.
    const scene::Entity once = r.w.create("once");
    r.made.push_back(once);
    r.w.setLocalPosition(once, Vec3{45, -20, 355});
    sb::CSoftField* c = sb::SoftBodySystem::attachSoftField(r.w, once);
    c->kind = static_cast<u32>(sb::FieldKind::RadialForce);
    c->shape = static_cast<u32>(sb::FieldShape::Sphere);
    c->extentX = 150.0f;
    c->strength = 600.0f;
    c->flags = sb::kFieldOnce;
    const f32 y0 = meanY(r, cloth);
    r.run(0.4f);
    check((c->flags & sb::kFieldDisabled) != 0 && (c->outState & sb::kFieldStateFired) != 0, "a once field disables itself and reports fired");
    check(meanY(r, cloth) - y0 > 3.0f, "after pushing the cloth");
    check(c->outTouched == 1, "outTouched counted the piece it reached");

    // Transient.
    sb::FieldSpec f = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
    f.dir = Vec3{0, 1, 0};
    f.strength = 200.0f;
    const u32 h = r.sys.startField(f, 0.3f);
    r.frame();
    check(r.sys.stats().transientFields == 1, "a transient field is live");
    r.run(0.5f);
    check(r.sys.stats().transientFields == 0 && !r.sys.stopField(h), "and gone after its lifetime");
    const u32 h2 = r.sys.startField(f, 10.0f);
    check(r.sys.stopField(h2) && !r.sys.stopField(h2), "a field can be stopped once");
    r.run(0.1f);
    check(r.sys.stats().transientFields == 0, "and a stopped one is not applied");
    check(r.sys.startField(f, 0.0f) == 0 && r.sys.startField(f, -1.0f) == 0, "a lifetime of zero or less is refused");

    // Parent.
    const scene::Entity parent = r.w.create("parent");
    r.made.push_back(parent);
    r.w.setLocalPosition(parent, Vec3{45, -20, 355});
    const scene::Entity child = r.w.create("child");
    r.made.push_back(child);
    r.w.setParent(child, parent);
    sb::CSoftField* cf = sb::SoftBodySystem::attachSoftField(r.w, child);
    cf->extentX = 60.0f;
    r.run(0.2f);
    check(cf->outTouched == 1, "a field on a child of a parent at the cloth reaches it");
    r.w.setLocalPosition(parent, Vec3{5000, 0, 0});
    r.run(0.2f);
    check(cf->outTouched == 0, "move the parent away and the field goes with it");
    r.w.setLocalPosition(parent, Vec3{45, -20, 355});
    r.run(0.2f);
    check(cf->outTouched == 1, "and back");
    cf->flags = sb::kFieldDisabled;
    r.run(0.2f);
    check(cf->outTouched == 0 && (cf->outState & sb::kFieldStateActive) == 0, "a disabled field touches nothing");
}

static void testImpactHelper() {
    AVER_INFO("-- impact(): the crash helper damages like a hit and honours indestructible --");
    Rig r;
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.meshes[kPanel] = sheetMesh(8, 8, 10.0f, Plane::XZ);
    r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    r.stores.assets[kMetal] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
    const scene::Entity glass = r.entity("glass", kPane, Vec3{0, 0, 400});
    const scene::Entity metal = r.entity("metal", kPanel, Vec3{600, 0, 300});
    const scene::Entity solid = r.entity("solid", kPane, Vec3{1200, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, glass, sb::SoftPreset::Glass, kGlass);
    sb::SoftBodySystem::attachDestructible(r.w, metal, sb::SoftPreset::Metal, kMetal);
    sb::SoftBodySystem::attachDestructible(r.w, solid, sb::SoftPreset::Glass, kGlass)->flags |= sb::kDestructIndestructible;
    r.run(0.8f);
    const u32 reach = r.sys.impact(Vec3{45, -5, 350}, Vec3{0, 1, 0}, 60.0f, 600.0f, 0.0f);
    r.sys.impact(Vec3{1245, -5, 350}, Vec3{0, 1, 0}, 60.0f, 600.0f, 0.0f);
    r.sys.impact(Vec3{635, -5, 265}, Vec3{0, 1, 0}, 40.0f, 400.0f, 200.0f);
    check(reach >= 1, "impact reports the pieces in reach");
    r.run(1.5f);
    check(brokenCount(r.sys.cageOf(glass)) > 0, "the glass breaks");
    check(brokenCount(r.sys.cageOf(solid)) == 0, "the indestructible does not");
    const sb::DentInfo d = r.sys.dents(metal);
    check(d.found && (d.dentedEdges > 0 || brokenCount(r.sys.cageOf(metal)) > 0), "the metal dents or tears");
    check(r.sys.impact(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 0.0f, 100.0f, 0.0f) == 0 && r.sys.impact(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 10.0f, -1.0f, 0.0f) == 0,
          "a non-positive radius or negative energy is refused");
}

// ---- the reveal handshake and the draw budget -------------------------------------------------------------------

static void testRevealAndBudget() {
    AVER_INFO("-- debris is created invisible and revealed only once a resident exists; builds are budgeted --");
    Rig r;
    r.stores.meshes[kBigPane] = sheetMesh(16, 16, 8.0f, Plane::XZ);
    sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    a.destruct.maxPieces = 48;
    r.stores.assets[kGlassBig] = a;
    r.sys.options().maxLivePieces = 64;
    r.sys.options().maxRebuildsPerFrame = 8;
    r.sys.options().cullPixelRatio = 0.0f;   // tiny shards must not be culled here: this test is about the reveal
    const scene::Entity pane = r.entity("pane", kBigPane, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassBig);
    r.run(0.4f);

    SceneMimic mimic;
    mimic.budget = 2;
    // 200 J: the glass breaks into shards (measured 10 pieces); at 900 J every shard is under the minimum triangle count and the pane is all dust.
    r.sys.damage(pane, Vec3{60, 0, 350}, 110.0f, 200.0f, Vec3{0, 1, 0}, 2);
    bool ghost = false;
    u32 maxDebris = 0;
    for (int i = 0; i < 30; ++i) {
        r.frame();
        for (const sb::PieceView* v : debrisOf(r, pane)) {
            // A debris entity must not draw before its resident exists (a zero handle draws the WHOLE authored mesh).
            if (visible(r, v->entity) && !mimic.hasResident(v->entity)) ghost = true;
        }
        maxDebris = std::max(maxDebris, static_cast<u32>(debrisOf(r, pane).size()));
        mimic.update(r);
    }
    AVER_INFO("  {} debris pieces, at most {} residents built in a frame", maxDebris, mimic.maxBuiltInAFrame);
    check(maxDebris >= 4, "the pane sheds several pieces");
    check(!ghost, "no debris entity was ever visible without a resident");
    bool allRevealed = true;
    for (const sb::PieceView* v : debrisOf(r, pane)) allRevealed = allRevealed && visible(r, v->entity);
    check(allRevealed, "and every piece is revealed once its resident is built");
    check(mimic.maxBuiltInAFrame <= 2, "builds respect the per-frame budget of the draw side");
    check(r.sys.options().maxDrawBuildsPerFrame == 8, "the default draw budget is 8 builds a frame");
    const Views vs = viewsOf(r);
    bool flagged = true;
    for (u32 i = 0; i < vs.n; ++i)
        if (vs.v[i].entity != pane && vs.v[i].layer == 0) flagged = flagged && vs.v[i].revealOnReady;
    check(flagged, "debris views carry revealOnReady");
}

// ---- thick breaks -----------------------------------------------------------------------------------------------

static void testCaps() {
    AVER_INFO("-- caps: a pane publishes front, back skin and walls with UVs; an interior material makes cap entities; LOD drops them --");
    {
        Rig r;
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlass);
        r.run(0.6f);
        Views vs = viewsOf(r);
        check(vs.n == 1, "one view for the intact pane");
        u32 generated = 0, authored = 0;
        for (u32 i = 0; i < vs.v[0].vertexCount; ++i) (vs.v[0].sourceVertex[i] == sb::kGeneratedVertex ? generated : authored)++;
        check(authored == 100 && generated > 0, "100 authored vertices plus generated back-skin and wall vertices");
        check(vs.v[0].uv != nullptr, "the view carries UVs for the generated vertices");
        bool valid = true, finite = true;
        for (u32 i = 0; i < vs.v[0].indexCount; ++i) valid = valid && vs.v[0].indices[i] < vs.v[0].vertexCount;
        for (u32 i = 0; i < vs.v[0].vertexCount; ++i) finite = finite && std::isfinite(vs.v[0].world[i].x) && std::isfinite(vs.v[0].world[i].y) && std::isfinite(vs.v[0].world[i].z);
        check(valid && finite && vs.v[0].indexCount % 3 == 0, "indices are valid and positions finite");
        // The back skin sits 0.6 cm behind the front (glass preset), along -normal.
        f32 maxDepth = 0.0f;
        for (u32 i = 0; i < vs.v[0].vertexCount; ++i)
            if (vs.v[0].sourceVertex[i] == sb::kGeneratedVertex) maxDepth = std::max(maxDepth, std::fabs(vs.v[0].world[i].y));
        check(maxDepth > 0.3f && maxDepth < 1.0f, "generated vertices stand about the glass thickness (0.6 cm) off the sheet");

        shatter(r, pane);
        r.run(0.5f);
        vs = viewsOf(r);
        u32 withCaps = 0;
        for (u32 i = 0; i < vs.n; ++i) {
            bool any = false;
            for (u32 k = 0; k < vs.v[i].vertexCount; ++k) any = any || vs.v[i].sourceVertex[k] == sb::kGeneratedVertex;
            withCaps += any ? 1u : 0u;
        }
        check(vs.n >= 2 && withCaps == vs.n, "after the break every piece, the root and the shards, has caps");
        r.sys.options().capsEnabled = false;
        r.sys.repair(pane);
        r.run(0.5f);
        vs = viewsOf(r);
        bool none = true;
        for (u32 k = 0; k < vs.v[0].vertexCount; ++k) none = none && vs.v[0].sourceVertex[k] != sb::kGeneratedVertex;
        check(none && vs.v[0].vertexCount == 100, "with caps switched off the view is the plain 100 vertices");
    }
    {
        Rig r;
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        a.caps.interiorMaterial = "M_GlassEdge";
        r.stores.assets[kGlassInterior] = a;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassInterior);
        r.run(0.6f);
        const i32 token = aver_scene_material(0, "M_GlassEdge");
        Views vs = viewsOf(r);
        u32 caps = 0, surfaces = 0;
        scene::Entity capEntity = scene::kInvalidEntity;
        for (u32 i = 0; i < vs.n; ++i) {
            if (vs.v[i].layer == 1) { ++caps; capEntity = vs.v[i].entity; }
            else ++surfaces;
        }
        check(surfaces == 1 && caps == 1, "an interior material adds one cap view beside the surface view");
        const auto* mr = capEntity != scene::kInvalidEntity ? r.w.component<scene::CMeshRenderer>(capEntity, scene::kComponentMeshRenderer) : nullptr;
        check(token > 0 && mr && mr->material == token, "the cap entity draws with the interior material's token");
        check(capEntity != scene::kInvalidEntity && !visible(r, capEntity), "and starts invisible, to be revealed by the draw side");
        const auto* sb2 = capEntity != scene::kInvalidEntity ? r.w.component<scene::CSoftBody>(capEntity, scene::kComponentSoftBody) : nullptr;
        check(sb2 && (sb2->flags & scene::kSoftBodyPiece), "marked as a piece so no entry is built for it");
        bool noWalls = true;
        for (u32 i = 0; i < vs.n; ++i)
            if (vs.v[i].layer == 0 && vs.v[i].indexCount != 0) {
                // The surface view holds front and back skin only: its generated vertices equal the front count (one back vertex each).
                u32 generated = 0;
                for (u32 k = 0; k < vs.v[i].vertexCount; ++k) generated += vs.v[i].sourceVertex[k] == sb::kGeneratedVertex ? 1u : 0u;
                noWalls = noWalls && generated == 100;
            }
        check(noWalls, "the walls left the surface mesh");

        // LOD: beyond caps.lodCm (2500 cm) the caps go, with one epoch change, and come back inside the band.
        const u32 epoch0 = vs.v[0].epoch;
        r.sys.setViewPos(Vec3{3500, 0, 400});
        r.run(0.2f);
        vs = viewsOf(r);
        u32 capViews = 0;
        for (u32 i = 0; i < vs.n; ++i) capViews += vs.v[i].layer == 1 ? 1u : 0u;
        u32 gen = 0;
        for (u32 k = 0; k < vs.v[0].vertexCount; ++k) gen += vs.v[0].sourceVertex[k] == sb::kGeneratedVertex ? 1u : 0u;
        check(capViews == 0 && gen == 0, "far away the pane draws without caps");
        check(vs.v[0].epoch != epoch0, "the draw mesh was rebuilt once");
        const u32 epoch1 = vs.v[0].epoch;
        r.run(0.3f);
        check(viewsOf(r).v[0].epoch == epoch1, "and not again while it stays there");
        r.sys.setViewPos(Vec3{2400, 0, 400});   // inside the 10% band: still no caps
        r.run(0.2f);
        gen = 0;
        for (u32 k = 0; k < viewsOf(r).v[0].vertexCount; ++k) gen += viewsOf(r).v[0].sourceVertex[k] == sb::kGeneratedVertex ? 1u : 0u;
        check(gen == 0, "a 10% hysteresis band keeps them off at 2400 cm");
        r.sys.setViewPos(Vec3{0, 0, 400});
        r.run(0.2f);
        gen = 0;
        for (u32 k = 0; k < viewsOf(r).v[0].vertexCount; ++k) gen += viewsOf(r).v[0].sourceVertex[k] == sb::kGeneratedVertex ? 1u : 0u;
        check(gen > 0, "back inside, they return");
    }
}

// ---- break effects ----------------------------------------------------------------------------------------------

static const SpawnRecord* firstSpawn(const FxRecorder& rec, u64 id) {
    for (const SpawnRecord& s : rec.spawns)
        if (s.id == id) return &s;
    return nullptr;
}

static void testEffects() {
    AVER_INFO("-- effects: requests per slot with counts and directions, then cooldown, per-tick caps, distance and the master switch --");
    const u64 tearId = fnv1a64("FX/Shards.ocparticle"), detachId = fnv1a64("FX/Chunk.ocparticle"), dustId = fnv1a64("FX/Dust.ocparticle");
    {
        Rig r;
        FxRecorder rec;
        rec.install(r.sys);
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        r.stores.assets[kGlassFx] = glassWithEffects();
        r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        r.sys.options().fxMaxPerTick = 64;
        r.sys.options().fxSoundsPerTick = 64;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        const scene::Entity plain = r.entity("plain", kPane, Vec3{600, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassFx);
        sb::SoftBodySystem::attachDestructible(r.w, plain, sb::SoftPreset::Glass, kGlass);
        r.run(0.5f);
        shatter(r, pane);
        shatter(r, plain, 600.0f);
        r.run(0.6f);
        const SpawnRecord* tear = firstSpawn(rec, tearId);
        check(tear && tear->count >= 12, "the tear slot spawned its effect with the bound count");
        check(firstSpawn(rec, detachId) != nullptr, "a detached piece spawned the detach effect");
        check(firstSpawn(rec, dustId) != nullptr, "fragments too small for pieces spawned dust");
        bool unit = true;
        for (const SpawnRecord& s : rec.spawns) unit = unit && std::fabs(s.dir.size() - 1.0f) < 1e-3f;
        check(unit, "every direction is a unit vector");
        bool sounds = !rec.sounds.empty();
        for (const sb::FxRequest& q : rec.sounds)
            sounds = sounds && q.sound != 0 && q.volume > 0.0f && q.outerCm >= q.innerCm;
        check(sounds, "sound requests carry a path hash, a volume and attenuation radii");
        bool variants = true;
        for (const sb::FxRequest& q : rec.sounds)
            if (q.slot == sb::FxSlot::Tear) variants = variants && (q.sound == fnv1a64("Audio/Break1.wav") || q.sound == fnv1a64("Audio/Break2.wav"));
        check(variants, "a tear sound is one of the two bound variants");
        // Nothing for the pane with no binding: every recorded request came from the bound pane's side of the room.
        bool onlyBound = true;
        for (const SpawnRecord& s : rec.spawns) onlyBound = onlyBound && s.pos.x < 400.0f;
        check(onlyBound, "the unbound pane requested nothing");
        // Order: tear, then dust, then detach, as the events are emitted.
        usize iTear = rec.spawns.size(), iDust = rec.spawns.size(), iDetach = rec.spawns.size();
        for (usize i = 0; i < rec.spawns.size(); ++i) {
            if (rec.spawns[i].id == tearId && iTear == rec.spawns.size()) iTear = i;
            if (rec.spawns[i].id == dustId && iDust == rec.spawns.size()) iDust = i;
            if (rec.spawns[i].id == detachId && iDetach == rec.spawns.size()) iDetach = i;
        }
        check(iTear < iDust && iDust < iDetach, "requests come out in emit order: tear, dust, detach");

        // The master switch.
        rec.clear();
        r.sys.setEffectsEnabled(false);
        r.sys.repair(pane);
        r.run(0.3f);
        shatter(r, pane);
        r.run(0.5f);
        check(rec.spawns.empty() && rec.sounds.empty(), "with effects switched off nothing is requested");
        r.sys.setEffectsEnabled(true);
        r.sys.repair(pane);
        r.run(0.3f);
        shatter(r, pane);
        r.run(0.5f);
        check(!rec.spawns.empty(), "and switched on again they flow");

        // Distance cull.
        rec.clear();
        r.sys.repair(pane);
        r.run(0.3f);
        r.sys.setViewPos(Vec3{20000, 0, 0});
        shatter(r, pane);
        r.run(0.5f);
        check(rec.spawns.empty() && rec.sounds.empty(), "beyond the distance cull nothing is dispatched");
        rec.uninstall(r.sys);
    }
    {
        // Per-tick caps: three events in one tick, one allowed.
        Rig r;
        FxRecorder rec;
        rec.install(r.sys);
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        r.stores.assets[kGlassFx] = glassWithEffects();
        r.sys.options().fxMaxPerTick = 1;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassFx);
        r.run(0.5f);
        shatter(r, pane);
        u32 worst = 0, dropped = 0;
        for (int i = 0; i < 20; ++i) {
            rec.clear();
            r.frame();
            worst = std::max(worst, static_cast<u32>(rec.spawns.size()));
            dropped = std::max(dropped, r.sys.stats().fxDropped);
        }
        check(worst <= 1, "fxMaxPerTick caps the particle spawns of a tick");
        check(dropped >= 1, "and the rest are counted as dropped");
        rec.uninstall(r.sys);
    }
    {
        // Cooldown: two cuts on consecutive frames, one tear effect.
        Rig r;
        FxRecorder rec;
        rec.install(r.sys);
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        sb::SoftAsset a = glassWithEffects();
        a.effects.slot[static_cast<u32>(sb::FxSlot::Tear)].cooldownS = 5.0f;
        r.stores.assets[kGlassFx] = a;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassFx);
        r.run(0.5f);
        r.sys.tearAt(pane, Vec3{25, 0, 350}, 12.0f);
        r.frame();
        r.sys.tearAt(pane, Vec3{65, 0, 330}, 12.0f);
        r.run(0.4f);
        u32 tears = 0;
        for (const SpawnRecord& s : rec.spawns) tears += s.id == tearId ? 1u : 0u;
        check(r.kindCount(sb::SoftEventKind::Torn) >= 2 && tears == 1, "two tears inside the cooldown raise two events and one effect");
        rec.uninstall(r.sys);
    }
    {
        // The seam is harmless with no spawner.
        setEffectSpawner(nullptr, nullptr);
        check(spawnEffectAt(123, Vec3{0, 0, 0}, Vec3{0, 0, 1}, 4) == 0, "spawnEffectAt returns 0 with no spawner installed");
    }
    {
        // An impact slot: a falling sphere hits a bound metal door.
        Rig r;
        addFloor();
        FxRecorder rec;
        rec.install(r.sys);
        r.stores.meshes[kPanel] = sheetMesh(8, 8, 10.0f, Plane::XY);
        sb::SoftAsset a = glassWithEffects(true);
        a.kind = sb::SoftKind::Destructible;
        a.preset = sb::SoftPreset::Metal;
        a.material = sb::presetMaterial(sb::SoftPreset::Metal);
        a.destruct.minImpactCmS = 120.0f;
        a.paint.rules.clear();   // nothing pinned: the plate falls to the floor and the sphere lands on it
        r.stores.assets[kMetal] = a;
        const scene::Entity plate = r.entity("plate", kPanel, Vec3{-35, -35, 100});
        sb::SoftBodySystem::attachDestructible(r.w, plate, sb::SoftPreset::Metal, kMetal);
        r.run(0.3f);
        aver_phys_add_dynamic_sphere(0.0f, 0.0f, 260.0f, 10.0f, 8.0f);
        r.run(1.5f);
        const u64 sparkId = fnv1a64("FX/Spark.ocparticle");
        const SpawnRecord* spark = firstSpawn(rec, sparkId);
        bool clang = false;
        for (const sb::FxRequest& q : rec.sounds) clang = clang || q.slot == sb::FxSlot::Impact;
        check(spark != nullptr || clang || r.sys.lastHit(plate).valid, "a sphere dropped on the plate raised an impact (effect, sound or at least a recorded hit)");
        if (spark) check(spark->dir.size() > 0.99f && spark->dir.size() < 1.01f, "the impact effect's direction is the unit contact normal");
        rec.uninstall(r.sys);
    }
}

// ---- saved damage -----------------------------------------------------------------------------------------------

struct Snap {
    std::vector<u8> broken;
    std::vector<f32> plastic, damage;
    std::vector<Vec3> pos;
    u32 pieces = 0, debris = 0;
};

static Snap snapOf(const Rig& r, scene::Entity e) {
    Snap s;
    if (const sb::Cage* c = r.sys.cageOf(e)) {
        for (const sb::Beam& b : c->beams) { s.broken.push_back(b.broken ? 1 : 0); s.plastic.push_back(b.plastic); s.damage.push_back(b.damage); }
        for (const sb::Particle& p : c->particles) s.pos.push_back(p.pos);
    }
    s.pieces = r.sys.pieceCount(e);
    return s;
}

static void setupSaveWorld(Rig& r, scene::Entity& metal, scene::Entity& pane) {
    r.stores.meshes[kPanel] = sheetMesh(8, 8, 10.0f, Plane::XZ);
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.assets[kMetal] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
    r.stores.assets[kGlass] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    r.sys.options().debrisSleepS = 0.2f;
    r.sys.options().despawnShrinkS = 0.2f;
    metal = r.entity("metal", kPanel, Vec3{0, 0, 300});
    pane = r.entity("pane", kPane, Vec3{500, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, metal, sb::SoftPreset::Metal, kMetal);
    auto* d = sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlass);
    d->debrisLifetimeS = 1.0f;
}

static void testSaveAndRestore() {
    AVER_INFO("-- saved damage: capture, restore into a fresh world, stale keys, pristine entities, merged logs --");
    std::string blobMetal, blobPane;
    Snap metalA, paneA;
    u32 debrisA = 0;
    {
        Rig a;
        addFloor();
        scene::Entity metal, pane;
        setupSaveWorld(a, metal, pane);
        a.run(0.8f);
        std::string none;
        check(!a.sys.saveState(metal, none) && none.empty(), "a pristine entity writes no channel");
        a.sys.damage(metal, Vec3{35, 0, 265}, 40.0f, 400.0f, Vec3{0, 1, 0}, 2);
        a.sys.damage(pane, Vec3{545, 0, 350}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
        a.run(4.0f);
        metalA = snapOf(a, metal);
        paneA = snapOf(a, pane);
        debrisA = a.sys.stats().debris;
        const bool savedMetal = a.sys.saveState(metal, blobMetal);
        const bool savedPane = a.sys.saveState(pane, blobPane);
        check(savedPane && blobPane.rfind("SB1 ", 0) == 0, "the shattered pane saves a channel");
        AVER_INFO("  metal saved {} ({} bytes), pane saved {} ({} bytes), debris alive {}", savedMetal, blobMetal.size(), savedPane, blobPane.size(), debrisA);
        sb::SoftState st;
        check(savedPane && sb::decodeSoftState(blobPane, st) && !st.tearGroups.empty(), "and it decodes with a tear log");
    }
    {
        Rig b;
        addFloor();
        FxRecorder rec;
        rec.install(b.sys);
        scene::Entity metal, pane;
        setupSaveWorld(b, metal, pane);
        if (!blobMetal.empty()) check(b.sys.restoreState(metal, blobMetal), "the metal payload is accepted");
        check(b.sys.restoreState(pane, blobPane), "the pane payload is accepted");
        b.frame();
        const Snap metalB = snapOf(b, metal), paneB = snapOf(b, pane);
        if (!blobMetal.empty()) {
            check(metalB.broken == metalA.broken, "metal: the same beams are broken");
            bool same = metalB.plastic.size() == metalA.plastic.size();
            for (usize i = 0; same && i < metalA.plastic.size(); ++i) same = std::fabs(metalA.plastic[i] - metalB.plastic[i]) < 1e-4f && std::fabs(metalA.damage[i] - metalB.damage[i]) < 1e-4f;
            check(same, "metal: the permanent set and the damage are equal");
            f32 worst = 0.0f;
            for (usize i = 0; i < metalA.pos.size() && i < metalB.pos.size(); ++i) worst = std::max(worst, dist(metalA.pos[i], metalB.pos[i]));
            AVER_INFO("  metal worst position error {:.5f} cm", worst);
            check(worst < 0.05f, "metal: positions match");
        }
        check(paneB.broken == paneA.broken, "pane: the same beams are broken");
        check(paneB.pieces == paneA.pieces, "pane: the same number of live pieces (despawned shards are not recreated)");
        check(b.sys.stats().debris == debrisA, "and the same number of debris entities");
        check(b.kindCount(sb::SoftEventKind::Torn) == 0 && b.kindCount(sb::SoftEventKind::Detached) == 0 && b.kindCount(sb::SoftEventKind::Dust) == 0,
              "replaying the log raised no events");
        check(rec.spawns.empty() && rec.sounds.empty(), "and no effects");
        b.run(0.5f);
        check(b.sys.stats().debris <= debrisA, "the world carries on from the restored state");
        rec.uninstall(b.sys);
    }
    {
        // A changed mesh drops the state: the entity loads intact.
        Rig c;
        scene::Entity metal, pane;
        setupSaveWorld(c, metal, pane);
        c.stores.meshes[kPane].positions[3] += 1.0f;
        check(c.sys.restoreState(pane, blobPane), "a payload is accepted before its keys are checked");
        c.run(0.5f);
        check(c.sys.pieceCount(pane) == 1 && brokenCount(c.sys.cageOf(pane)) == 0, "a changed mesh loads intact");
    }
    {
        Rig c;
        scene::Entity metal, pane;
        setupSaveWorld(c, metal, pane);
        c.stores.assets[kGlass].material.toughnessJ *= 3.0f;
        check(c.sys.restoreState(pane, blobPane), "same again for the asset");
        c.run(0.5f);
        check(c.sys.pieceCount(pane) == 1 && brokenCount(c.sys.cageOf(pane)) == 0, "a changed asset loads intact");
    }
    {
        Rig c;
        scene::Entity metal, pane;
        setupSaveWorld(c, metal, pane);
        check(!c.sys.restoreState(pane, "SB1 nonsense") && !c.sys.restoreState(pane, ""), "a malformed payload is refused");
        c.run(0.3f);
        check(c.sys.pieceCount(pane) == 1, "and the entity builds normally");
    }

    // More than 64 tear groups are merged on load; the cage stays valid.
    std::string wideBlob;
    u32 wideBroken = 0, wideGroups = 0;
    {
        Rig d;
        d.stores.meshes[kWide] = sheetMesh(24, 24, 10.0f, Plane::XZ);
        d.stores.assets[kMetal] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);   // glass hung 24 rows cascades from a lone cut
        d.sys.options().maxRebuildsPerFrame = 4;
        const scene::Entity wide = d.entity("wide", kWide, Vec3{0, 0, 400});
        auto* de = sb::SoftBodySystem::attachDestructible(d.w, wide, sb::SoftPreset::Metal, kMetal);
        de->flags |= sb::kDestructNoDebris;
        d.run(0.4f);
        // One lone horizontal beam at a time, 20 cm apart across and down: a tear that disconnects nothing, so the pane
        // stays whole and every tear is a pipeline run of its own (a crack that lets the body fall apart leaves later tears in dust, which logs nothing).
        for (int j = 0; j < 10; ++j)
            for (int i = 0; i < 11; ++i) {
                d.sys.tearAt(wide, Vec3{5.0f + 20.0f * static_cast<f32>(i), 0, 370.0f - 20.0f * static_cast<f32>(j)}, 2.0f);
                d.frame();
            }
        d.run(0.3f);
        wideBroken = brokenCount(d.sys.cageOf(wide));
        check(d.sys.saveState(wide, wideBlob), "the wide pane saves");
        sb::SoftState st;
        check(sb::decodeSoftState(wideBlob, st), "and decodes");
        wideGroups = static_cast<u32>(st.tearGroups.size());
        AVER_INFO("  {} tear groups, {} broken beams", wideGroups, wideBroken);
        check(wideGroups > 64, "more than 64 tears were logged");
    }
    {
        Rig e;
        e.stores.meshes[kWide] = sheetMesh(24, 24, 10.0f, Plane::XZ);
        e.stores.assets[kMetal] = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
        const scene::Entity wide = e.entity("wide", kWide, Vec3{0, 0, 400});
        auto* de = sb::SoftBodySystem::attachDestructible(e.w, wide, sb::SoftPreset::Metal, kMetal);
        de->flags |= sb::kDestructNoDebris;
        check(e.sys.options().restoreMaxReplayGroups == 64, "the replay cap defaults to 64 groups");
        check(e.sys.restoreState(wide, wideBlob), "the payload is accepted");
        e.run(0.4f);
        check(brokenCount(e.sys.cageOf(wide)) == wideBroken && e.sys.pieceCount(wide) >= 1, "merged groups still reach the same broken beams and a valid cage");
        f32 worst = 0.0f;
        bool finite = true;
        for (const sb::Particle& p : e.particles(wide)) { worst = std::max(worst, std::fabs(p.pos.z)); finite = finite && std::isfinite(p.pos.x) && std::isfinite(p.pos.z); }
        check(finite && worst < 2000.0f, "with finite, sane positions");
    }
}

// ---- baking -----------------------------------------------------------------------------------------------------

static void testBaking() {
    AVER_INFO("-- bake: a settled piece loses its body, publishes bake with a new origin, still despawns, and is skipped by damage --");
    auto run = [](bool bakeAsset, bool bakeGlobal, u32& bakedOut, i32& bodiesOut, u32& piecesOut, bool check2) {
        Rig r;
        addFloor();
        r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
        sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        a.destruct.bake = bakeAsset;
        a.destruct.debrisLifetimeS = 60.0f;
        r.stores.assets[kGlass] = a;
        r.sys.options().debrisSleepS = 0.3f;
        r.sys.options().bakeAfterS = 0.5f;
        r.sys.options().bakeEnabled = bakeGlobal;
        r.sys.options().despawnShrinkS = 0.4f;
        const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 120});
        auto* d = sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlass);
        r.run(0.5f);
        // The pane hangs from z = 120 down to 30: hit it low so the shards have a short way to fall to the floor.
        r.sys.damage(pane, Vec3{45, 0, 80}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
        for (int i = 0; i < 60 * 14 && r.sys.stats().bakedPieces == 0; ++i) r.frame();
        bakedOut = r.sys.stats().bakedPieces;
        bodiesOut = aver_phys_sb_count();
        piecesOut = r.sys.stats().pieces;
        if (check2 && bakedOut > 0) {
            const Views vs = viewsOf(r);
            u32 bakedViews = 0;
            bool originOk = true;
            for (u32 i = 0; i < vs.n; ++i)
                if (vs.v[i].bake) {
                    ++bakedViews;
                    originOk = originOk && dist(vs.v[i].origin, vs.v[i].centroid) < 1e-3f && vs.v[i].frozen;
                }
            check(bakedViews == bakedOut, "every baked piece publishes bake = true");
            check(originOk, "with its origin moved to the centroid, and never repacked (frozen)");
            check(bodiesOut == static_cast<i32>(piecesOut - bakedOut), "and a baked piece owns no Jolt body");

            // Damage and fields skip a baked piece.
            const Vec3 where = meanOfDebris(r, pane);
            const u32 before = r.sys.stats().bakedPieces;
            r.sys.damage(pane, where, 80.0f, 800.0f, Vec3{0, 0, 0}, 2);
            sb::FieldSpec f = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
            f.dir = Vec3{0, 1, 0};
            f.strength = 2000.0f;
            r.sys.pulseField(f);
            r.run(0.3f);
            check(r.sys.stats().bakedPieces == before, "a hit and a gust leave baked pieces baked and in place");

            // The despawn shrink is an entity scale, and the piece still goes away.
            d->debrisLifetimeS = 0.2f;
            r.sys.options().maxLivePieces = 0;   // and the cap evicts whatever never fell asleep
            f32 minScale = 1.0f;
            for (int i = 0; i < 60 * 3; ++i) {
                r.frame();
                for (const sb::PieceView* v : debrisOf(r, pane)) minScale = std::min(minScale, r.w.localTransform(v->entity).scale.x);
            }
            check(minScale < 0.99f, "a baked piece's shrink shows as its entity scale");
            check(r.sys.stats().debris == 0, "and every baked piece is eventually destroyed");
        }
    };
    u32 baked = 0, pieces = 0;
    i32 bodies = 0;
    run(true, true, baked, bodies, pieces, true);
    check(baked > 0, "settled debris was baked");
    u32 bakedOff = 0;
    run(false, true, bakedOff, bodies, pieces, false);
    check(bakedOff == 0, "destruct.bake = false switches it off per asset");
    run(true, false, bakedOff, bodies, pieces, false);
    check(bakedOff == 0, "and Options::bakeEnabled = false switches it off for the system");
}

// ---- proxy cage ---------------------------------------------------------------------------------------------------

static void testProxy() {
    AVER_INFO("-- proxy: an over-cap plate with a proxy runs where it failed before; the draw mesh follows; tears never bridge a crack --");
    const char* proxyPath = "Meshes/PlateProxy.ocmesh";
    Rig r;
    r.stores.meshes[kPlate] = sheetMesh(101, 101, 1.0f, Plane::XZ);                       // 10201 welded vertices: over the 4096 default
    r.stores.meshes[fnv1a64(proxyPath)] = sheetMesh(21, 21, 5.0f, Plane::XZ);            // 441 particles
    sb::SoftAsset plain = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Metal);
    r.stores.assets[kMetal] = plain;
    sb::SoftAsset withProxy = plain;
    withProxy.proxy.mesh = proxyPath;
    r.stores.assets[kMetalPlateProxy] = withProxy;

    const scene::Entity bare = r.entity("bare", kPlate, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, bare, sb::SoftPreset::Metal, kMetal);
    const scene::Entity plate = r.entity("plate", kPlate, Vec3{600, 0, 400});
    auto* d = sb::SoftBodySystem::attachDestructible(r.w, plate, sb::SoftPreset::Metal, kMetalPlateProxy);
    r.run(1.0f);

    auto* bareC = r.w.component<sb::CDestructible>(bare, sb::SoftBodySystem::destructibleType());
    check((bareC->outState & sb::kStateOverCap) != 0 && r.sys.pieceCount(bare) == 0, "a plate with no proxy still fails with kStateOverCap");
    check((d->outState & sb::kStateProxy) != 0 && (d->outState & sb::kStateOverCap) == 0 && r.sys.pieceCount(plate) == 1, "with a proxy it runs, reporting kStateProxy");

    auto frontCount = [&](scene::Entity e, u32& generated) {
        const Views vs = viewsOf(r);
        u32 authored = 0;
        generated = 0;
        for (u32 i = 0; i < vs.n; ++i)
            if (vs.v[i].entity == e && vs.v[i].layer == 0)
                for (u32 k = 0; k < vs.v[i].vertexCount; ++k) (vs.v[i].sourceVertex[k] == sb::kGeneratedVertex ? generated : authored)++;
        return authored;
    };
    u32 generated = 0;
    check(frontCount(plate, generated) == 10201, "the draw mesh is the full 10201-vertex render mesh");

    // A hit dents the proxy and the render vertices follow.
    r.sys.damage(plate, Vec3{650, -10, 350}, 40.0f, 400.0f, Vec3{0, 1, 0}, 2);
    r.run(2.0f);
    f32 maxY = 0.0f;
    {
        const Views vs = viewsOf(r);
        for (u32 i = 0; i < vs.n; ++i)
            if (vs.v[i].entity == plate && vs.v[i].layer == 0)
                for (u32 k = 0; k < vs.v[i].vertexCount; ++k)
                    if (vs.v[i].sourceVertex[k] != sb::kGeneratedVertex) maxY = std::max(maxY, std::fabs(vs.v[i].world[k].y));
    }
    AVER_INFO("  largest out-of-plane displacement of a render vertex: {:.2f} cm", maxY);
    check(maxY > 0.3f, "the render vertices follow the dented proxy");

    // A tear: no render triangle may bridge the crack.
    // The hit swung the plate (measured up to 18 cm out of plane), so a cut at the rest plane would miss it: cut where the particles are.
    for (const f32 z : {330.0f, 310.0f}) {
        Vec3 where{650, 0, z};
        f32 best = 1e30f;
        for (const sb::Particle& p : r.particles(plate))
            if (dist(p.pos, Vec3{650, 0, z}) < best) { best = dist(p.pos, Vec3{650, 0, z}); where = p.pos; }
        r.sys.tearAt(plate, where, 8.0f);
    }
    r.run(1.2f);
    f32 longest = 0.0f;
    {
        const Views vs = viewsOf(r);
        for (u32 i = 0; i < vs.n; ++i) {
            if (vs.v[i].layer != 0 || vs.v[i].meshId != kPlate) continue;
            for (u32 t = 0; t + 2 < vs.v[i].indexCount; t += 3) {
                const u32 a = vs.v[i].indices[t], b = vs.v[i].indices[t + 1], c = vs.v[i].indices[t + 2];
                if (vs.v[i].sourceVertex[a] == sb::kGeneratedVertex || vs.v[i].sourceVertex[b] == sb::kGeneratedVertex || vs.v[i].sourceVertex[c] == sb::kGeneratedVertex) continue;
                longest = std::max({longest, dist(vs.v[i].world[a], vs.v[i].world[b]), dist(vs.v[i].world[b], vs.v[i].world[c]), dist(vs.v[i].world[c], vs.v[i].world[a])});
            }
        }
    }
    AVER_INFO("  longest render-triangle edge after the tear: {:.2f} cm (authored edges are about 1.4 cm)", longest);
    check(longest < 8.0f, "no visible render triangle is stretched across the crack");
    check(brokenCount(r.sys.cageOf(plate)) > 0, "the tear broke proxy edges");
}

// ---- replay equality with everything on ---------------------------------------------------------------------------

struct Replay {
    std::vector<f32> positions;
    std::vector<f32> eventBits;
    u32 pieces = 0, spawns = 0;
};

static Replay runScript() {
    Rig r;
    addFloor();
    FxRecorder rec;
    rec.install(r.sys);
    r.stores.meshes[kPane] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    r.stores.meshes[kSheet] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    sb::SoftAsset a = glassWithEffects();
    a.caps.interiorMaterial = "M_GlassEdge";
    r.stores.assets[kGlassFx] = a;
    r.sys.options().bakeAfterS = 1.0f;
    r.sys.options().debrisSleepS = 0.4f;
    const scene::Entity pane = r.entity("pane", kPane, Vec3{0, 0, 400});
    const scene::Entity flag = r.entity("flag", kSheet, Vec3{600, 0, 400});
    sb::SoftBodySystem::attachDestructible(r.w, pane, sb::SoftPreset::Glass, kGlassFx);
    sb::SoftBodySystem::attachCloth(r.w, flag, sb::SoftPreset::ClothLight);
    for (int t = 0; t < 600; ++t) {
        if (t == 30) r.sys.setWind(Vec3{400, 0, 0}, 100.0f, 0.5f, 50.0f);
        if (t == 60) {
            sb::FieldSpec f = spec(sb::FieldKind::DirectionalForce, sb::FieldShape::Box, Vec3{645, 0, 350}, Vec3{200, 200, 200});
            f.dir = Vec3{0, 1, 0};
            f.strength = 300.0f;
            f.turbulence = 0.5f;
            r.sys.startField(f, 1.0f);
        }
        if (t == 90) r.sys.damage(pane, Vec3{45, 0, 350}, 55.0f, 600.0f, Vec3{0, 1, 0}, 2);
        if (t == 150) r.sys.impact(Vec3{20, -5, 330}, Vec3{0, 1, 0}, 30.0f, 40.0f, 100.0f);
        if (t == 200) {
            sb::FieldSpec f = spec(sb::FieldKind::RadialForce, sb::FieldShape::Sphere, Vec3{30, -30, 300}, Vec3{200, 0, 0});
            f.strength = 400.0f;
            r.sys.pulseField(f);
        }
        if (t == 300) r.sys.tearAt(pane, Vec3{45, 0, 330}, 15.0f);
        r.frame();
    }
    Replay out;
    const Views vs = viewsOf(r);
    // The system lists views by entity id, and a fresh rig may hand out ids in another order, so compare them in an order of their own.
    std::vector<u32> order(vs.n);
    for (u32 i = 0; i < vs.n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 x, u32 y) {
        const sb::PieceView &p = vs.v[x], &q = vs.v[y];
        if (p.meshId != q.meshId) return p.meshId < q.meshId;
        if (p.vertexCount != q.vertexCount) return p.vertexCount < q.vertexCount;
        for (u32 k = 0; k < p.vertexCount; ++k) {
            if (p.world[k].x != q.world[k].x) return p.world[k].x < q.world[k].x;
            if (p.world[k].y != q.world[k].y) return p.world[k].y < q.world[k].y;
            if (p.world[k].z != q.world[k].z) return p.world[k].z < q.world[k].z;
        }
        return false;
    });
    for (const u32 i : order)
        for (u32 v = 0; v < vs.v[i].vertexCount; ++v) {
            out.positions.push_back(vs.v[i].world[v].x);
            out.positions.push_back(vs.v[i].world[v].y);
            out.positions.push_back(vs.v[i].world[v].z);
        }
    for (const sb::SoftEvent& ev : r.events) {
        out.eventBits.push_back(static_cast<f32>(ev.kind));
        out.eventBits.push_back(static_cast<f32>(ev.piece));
        out.eventBits.push_back(static_cast<f32>(ev.beam));
        out.eventBits.push_back(static_cast<f32>(ev.seq));
        out.eventBits.push_back(ev.point.x); out.eventBits.push_back(ev.point.y); out.eventBits.push_back(ev.point.z);
        out.eventBits.push_back(ev.value);
    }
    for (const SpawnRecord& s : rec.spawns) {
        out.eventBits.push_back(static_cast<f32>(s.id & 0xFFFFFF));
        out.eventBits.push_back(static_cast<f32>(s.count));
        out.eventBits.push_back(s.pos.x); out.eventBits.push_back(s.pos.y); out.eventBits.push_back(s.pos.z);
    }
    out.pieces = vs.n;
    out.spawns = static_cast<u32>(rec.spawns.size());
    rec.uninstall(r.sys);
    return out;
}

static void testReplayEquality() {
    AVER_INFO("-- replay equality over 600 ticks with fields, caps, effects and baking on --");
    const Replay a = runScript();
    const Replay b = runScript();
    check(!a.positions.empty() && !a.eventBits.empty() && a.spawns > 0, "the script produced geometry, events and effect requests");
    check(a.pieces == b.pieces && a.spawns == b.spawns, "the same piece count and the same number of effect requests");
    check(a.positions.size() == b.positions.size() && std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * sizeof(f32)) == 0,
          "vertex positions are bit identical");
    check(a.eventBits.size() == b.eventBits.size() && std::memcmp(a.eventBits.data(), b.eventBits.data(), a.eventBits.size() * sizeof(f32)) == 0,
          "events and effect requests are bit identical");
}

int main() {
    AVER_INFO("SoftBodyDepthTest");
    testForceFields();
    testDebrisFieldAndTargets();
    testDamageField();
    testReleaseAnchors();
    testKillAndSleep();
    testOnceTransientAndParent();
    testImpactHelper();
    testRevealAndBudget();
    testCaps();
    testEffects();
    testSaveAndRestore();
    testBaking();
    testProxy();
    testReplayEquality();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
