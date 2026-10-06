// DecalPoolTest -- CDecal as a scene component (its field table, its zero-means-default readers), the
// gather the renderer is fed from, decal lifetimes, and the gameplay pool: it recycles the OLDEST decal
// when full, creates no entity once warm, takes expired and released decals back, and survives the
// world retiring its entities under it. No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/DecalGather.hpp"
#include "aver/scene/DecalPool.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol; }
static bool approxV(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) {
    return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) && approx(a.z, b.z, tol);
}

// Every paintable decal in the world, by entity.
static std::unordered_map<u32, WorldDecal> gathered(World& w) {
    std::vector<WorldDecal> v;
    gatherDecals(w, v);
    std::unordered_map<u32, WorldDecal> m;
    for (const WorldDecal& d : v) m.emplace(static_cast<u32>(d.entity), d);
    return m;
}

static Entity makeDecalEntity(World& w, const char* name, const Transform& xf, const CDecal& c) {
    const Entity e = w.create(name, kInvalidEntity, xf);
    if (auto* p = static_cast<CDecal*>(w.addComponent(e, kComponentDecal))) *p = c;
    return e;
}

int main() {
    AVER_INFO("=== DecalPoolTest ===");
    World& w = World::instance();

    AVER_INFO("=== the component ===");
    {
        check(kComponentDecal == 16 && w.componentId("CDecal") == kComponentDecal, "CDecal is built-in component 16");
        check(w.componentVerified(kComponentDecal), "its field table covers the struct byte for byte");
        check(w.componentSize(kComponentDecal) == sizeof(CDecal) && sizeof(CDecal) == 120, "its stride is 120 bytes");
        const u32 age = w.fieldId("CDecal.age"), serial = w.fieldId("CDecal.serial"), tint = w.fieldId("CDecal.tint");
        check(age && serial && tint, "age, serial and tint resolve by qualified name");
        check(w.field(age) && w.field(age)->readOnly && w.field(serial) && w.field(serial)->readOnly,
              "the pool's clock and ordering are read-only over the generic ABI (never saved)");
        check(w.field(tint) && w.field(tint)->kind == FieldKind::Vec3 && !w.field(tint)->readOnly, "tint is an editable Vec3");
        check(w.field(w.fieldId("CDecal.baseTexture"))->kind == FieldKind::I64, "the image ids are I64 ObjectIds");
        check(w.fieldId("CDecal.uvScaleU") && w.fieldId("CDecal.uvOffsetV") && w.fieldId("CDecal.sortOrder") &&
                  w.fieldId("CDecal.lifetimeSec") && w.fieldId("CDecal.flags"), "the remaining fields resolve");
    }

    AVER_INFO("=== zero means default ===");
    {
        const CDecal z{};
        check(decalSizeCm(z, 0) == 100.0f && decalSizeCm(z, 2) == 100.0f, "an unset size is 100 cm");
        f32 t[3];
        decalTint(z, t);
        check(t[0] == 1.0f && t[1] == 1.0f && t[2] == 1.0f, "an unset tint is white");
        check(decalNormalStrength(z) == 1.0f && decalEdgeFade(z) == 0.1f, "normal strength 1, edge fade 0.1");
        check(decalAngleStartDeg(z) == 60.0f && decalAngleEndDeg(z) == 85.0f, "angle fade 60 to 85");
        check(decalUvScale(z, 0) == 1.0f && decalUvScale(z, 1) == 1.0f, "uv scale 1");
        CDecal bad{};
        bad.angleFadeStartDeg = 80.0f; bad.angleFadeEndDeg = 70.0f;
        check(decalAngleEndDeg(bad) >= decalAngleStartDeg(bad) + 0.5f, "the end angle can never sit before the start");
        bad.angleFadeStartDeg = 200.0f;
        check(decalAngleStartDeg(bad) <= 89.0f, "the start angle is kept below the horizon");

        // The path that matters: storage the world hands back zero-filled.
        const Entity e = w.create("zero");
        auto* c = static_cast<CDecal*>(w.addComponent(e, kComponentDecal));
        check(c && c->sizeCm[0] == 0.0f && c->flags == 0 && c->tint[0] == 0.0f, "addComponent zero-fills");
        const auto g = gathered(w);
        const auto it = g.find(static_cast<u32>(e));
        check(it != g.end(), "a zero-filled decal is active and paints (colour only)");
        if (it != g.end()) {
            check(it->second.halfExtentsCm[0] == 50.0f && it->second.opacity == 1.0f && it->second.tint[1] == 1.0f,
                  "it reads as a 100 cm white opaque box");
            check(it->second.colour && !it->second.normal && !it->second.roughnessMetal,
                  "only the colour channel, since it names no normal or roughness data");
        }
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("=== the gather ===");
    {
        CDecal base = CDecal{};
        base.sizeCm[0] = 60.0f; base.sizeCm[1] = 120.0f; base.sizeCm[2] = 80.0f;
        base.tint[0] = 0.5f; base.tint[1] = 0.25f; base.tint[2] = 1.0f;
        base.sortOrder = 7;
        Transform xf;
        xf.position = Vec3{100, 200, 300};
        xf.rotation = Quat::fromAxisAngle({0, 0, 1}, radians(90.0f));
        const Entity a = makeDecalEntity(w, "a", xf, base);
        auto g = gathered(w);
        check(g.count(static_cast<u32>(a)) == 1, "an authored decal is gathered");
        const WorldDecal& d = g[static_cast<u32>(a)];
        check(approx(d.world[12], 100.0f) && approx(d.world[13], 200.0f) && approx(d.world[14], 300.0f),
              "with its world translation");
        check(d.halfExtentsCm[0] == 30.0f && d.halfExtentsCm[1] == 60.0f && d.halfExtentsCm[2] == 40.0f, "half extents are half the size");
        check(d.tint[0] == 0.5f && d.sortOrder == 7 && d.opacity == 1.0f, "tint, sort order and opacity carry over");
        check(approx(d.world[1], 1.0f, 1e-3f), "the matrix row 0 is the rotated projection axis (+X to +Y)");

        CDecal off = base; off.flags = kDecalDisabled;
        const Entity eOff = makeDecalEntity(w, "off", xf, off);
        CDecal clear = base; clear.transparency = 1.0f;
        const Entity eClear = makeDecalEntity(w, "clear", xf, clear);
        CDecal part = base; part.transparency = 0.25f;
        const Entity ePart = makeDecalEntity(w, "part", xf, part);
        CDecal noChannel = base; noChannel.flags = kDecalNoColour;
        const Entity eNone = makeDecalEntity(w, "none", xf, noChannel);
        CDecal normalOnly = base; normalOnly.flags = kDecalNoColour; normalOnly.normalTexture = 5;
        const Entity eNormal = makeDecalEntity(w, "normal", xf, normalOnly);
        CDecal rough = base; rough.flags = kDecalNoColour; rough.roughness = 0.3f;
        const Entity eRough = makeDecalEntity(w, "rough", xf, rough);
        Transform flat = xf; flat.scale = Vec3{0, 1, 1};
        const Entity eFlat = makeDecalEntity(w, "flat", flat, base);
        g = gathered(w);
        check(g.count(static_cast<u32>(eOff)) == 0, "a disabled decal is not gathered");
        check(g.count(static_cast<u32>(eClear)) == 0, "a fully transparent decal is not gathered");
        check(g.count(static_cast<u32>(ePart)) == 1 && approx(g[static_cast<u32>(ePart)].opacity, 0.75f), "transparency 0.25 is opacity 0.75");
        check(g.count(static_cast<u32>(eNone)) == 0, "a decal with every channel off is not gathered");
        check(g.count(static_cast<u32>(eNormal)) == 1 && g[static_cast<u32>(eNormal)].normal && !g[static_cast<u32>(eNormal)].colour,
              "a normal-only decal needs its normal image and paints only that");
        check(g.count(static_cast<u32>(eRough)) == 1 && g[static_cast<u32>(eRough)].roughnessMetal &&
                  approx(g[static_cast<u32>(eRough)].roughness, 0.3f), "an explicit roughness is a roughness channel");
        check(g.count(static_cast<u32>(eFlat)) == 0, "a collapsed matrix axis is not gathered");

        w.destroy(a);
        g = gathered(w);
        check(g.count(static_cast<u32>(a)) == 0, "an entity queued for destruction is not gathered");
        for (const Entity e : {a, eOff, eClear, ePart, eNone, eNormal, eRough, eFlat}) w.destroy(e);
        w.flush();
    }

    AVER_INFO("=== lifetimes ===");
    {
        check(decalLifeFade(5.0f, 0.0f, 3.0f) == 1.0f, "a permanent decal never fades");
        check(decalLifeFade(0.0f, 10.0f, 2.0f) == 1.0f && decalLifeFade(7.9f, 10.0f, 2.0f) == 1.0f, "full strength until the fade-out starts");
        check(approx(decalLifeFade(9.0f, 10.0f, 2.0f), 0.5f) && decalLifeFade(10.0f, 10.0f, 2.0f) == 0.0f, "then a linear ramp to nothing at the end");
        check(decalLifeFade(9.99f, 10.0f, 0.0f) == 1.0f && decalLifeFade(10.0f, 10.0f, 0.0f) == 0.0f, "with no fade-out it vanishes at once");

        CDecal c{};
        c.lifetimeSec = 1.0f; c.fadeOutSec = 0.5f;
        const Entity e = makeDecalEntity(w, "mortal", Transform{}, c);
        std::vector<Entity> expired;
        tickDecalLifetimes(w, 0.6f, &expired);
        auto* p = w.component<CDecal>(e, kComponentDecal);
        check(p && approx(p->age, 0.6f) && !(p->flags & kDecalDisabled) && expired.empty(), "ticking ages it");
        check(approx(gathered(w)[static_cast<u32>(e)].opacity, 0.8f, 1e-3f), "inside the fade-out the gathered opacity is the ramp");
        tickDecalLifetimes(w, 0.6f, &expired);
        check(p->flags & kDecalDisabled, "past its lifetime it is disabled");
        check(expired.size() == 1 && expired[0] == e, "and reported as expired exactly once");
        tickDecalLifetimes(w, 0.6f, &expired);
        check(expired.size() == 1, "a disabled decal is not reported again");
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("=== the pose of a surface decal ===");
    {
        Vec3 ex = decalRotationForSurface({0, 0, 1}).rotate({1, 0, 0});
        check(approxV(ex, {0, 0, -1}), "on a floor the decal looks straight down");
        for (const Vec3& n : {Vec3{0, 0, 1}, Vec3{-1, 0, 0}, Vec3{0, -1, 0}, Vec3{0.6f, 0.0f, 0.8f}, Vec3{0.3f, -0.5f, 0.2f}}) {
            const Vec3 nn = n.getSafeNormal();
            const Quat q = decalRotationForSurface(nn);
            const Vec3 x = q.rotate({1, 0, 0}), y = q.rotate({0, 1, 0}), z = q.rotate({0, 0, 1});
            check(approxV(x, nn * -1.0f, 1e-3f), "projection axis is minus the normal");
            check(approx(dot(x, y), 0.0f, 1e-3f) && approx(dot(y, z), 0.0f, 1e-3f) && approx(dot(x, z), 0.0f, 1e-3f), "the frame is orthogonal");
            check(approxV(cross(z, x), y, 1e-3f), "and the engine's handedness: Y = Z x X");
        }
        const Quat wall = decalRotationForSurface({-1, 0, 0});
        check(approxV(wall.rotate({0, 0, 1}), {0, 0, 1}, 1e-3f), "on a wall the image's up is the world's up");
        const Quat rolled = decalRotationForSurface({-1, 0, 0}, kPi * 0.5f);
        check(approxV(rolled.rotate({1, 0, 0}), {1, 0, 0}, 1e-3f) && approxV(rolled.rotate({0, 0, 1}), {0, -1, 0}, 1e-3f),
              "a roll turns the image about the projection axis and leaves the projection alone");
        const Quat tilted = decalRotationForSurface({-1, 0, 0}, 0.0f, Vec3{0, 1, 0});
        check(approxV(tilted.rotate({0, 0, 1}), {0, 1, 0}, 1e-3f), "an up hint other than +Z orients the image");
    }

    AVER_INFO("=== the pool ===");
    {
        const u32 before = w.count();
        DecalPool pool(4);
        DecalSpawn sp;
        sp.params.sizeCm[0] = 10.0f; sp.params.sizeCm[1] = 20.0f; sp.params.sizeCm[2] = 30.0f;
        sp.params.baseTexture = 77;
        sp.params.sortOrder = 3;
        sp.params.flags = kDecalNoNormal | kDecalDisabled;   // a stale disabled bit must not survive a spawn
        sp.position = Vec3{10, 20, 30};

        std::vector<Entity> made;
        for (int i = 0; i < 4; ++i) {
            sp.position.x = static_cast<f32>(i);
            made.push_back(pool.spawn(w, sp));
        }
        check(pool.createdCount() == 4 && pool.activeCount() == 4 && pool.recycledCount() == 0, "four spawns fill a pool of four");
        check(w.count() == before + 4, "and created four entities");
        bool distinct = true, allValid = true;
        for (usize i = 0; i < made.size(); ++i) {
            allValid = allValid && w.valid(made[i]) && pool.owns(made[i]) && pool.isActive(made[i]);
            for (usize j = i + 1; j < made.size(); ++j) distinct = distinct && made[i] != made[j];
        }
        check(allValid && distinct, "each is a distinct live entity the pool owns");
        const CDecal* c0 = w.component<CDecal>(made[0], kComponentDecal);
        check(c0 && c0->sizeCm[1] == 20.0f && c0->baseTexture == 77 && c0->sortOrder == 3, "the spawn's parameters land on the component");
        check(c0 && (c0->flags & kDecalPooled) && !(c0->flags & kDecalDisabled) && (c0->flags & kDecalNoNormal),
              "it is marked pooled and enabled, keeping the spawn's own flags");
        check(c0 && c0->age == 0.0f && c0->serial == 1, "age 0 and the first serial");
        check(approx(w.localTransform(made[2]).position.x, 2.0f), "the pose is applied");
        check(gathered(w).count(static_cast<u32>(made[0])) == 1, "and the gather sees it");

        const Entity fifth = pool.spawn(w, sp);
        check(fifth == made[0], "a fifth spawn takes the OLDEST decal's entity");
        check(pool.recycledCount() == 1 && pool.createdCount() == 4 && w.count() == before + 4, "without creating an entity");
        check(w.component<CDecal>(fifth, kComponentDecal)->serial == 5, "and is stamped as the newest");
        const Entity sixth = pool.spawn(w, sp);
        check(sixth == made[1] && pool.recycledCount() == 2, "the next one takes the next oldest");
        check(pool.activeCount() == 4, "the pool never holds more than its capacity");

        check(pool.release(w, made[3]) && !pool.isActive(made[3]) && pool.activeCount() == 3, "release gives one back");
        check(w.component<CDecal>(made[3], kComponentDecal)->flags & kDecalDisabled, "and disables it so it stops painting");
        check(gathered(w).count(static_cast<u32>(made[3])) == 0, "the gather no longer sees it");
        check(!pool.release(w, made[3]), "releasing it twice is refused");
        check(!pool.release(w, Entity(0x12345)), "so is releasing an entity the pool does not own");
        const Entity reuse = pool.spawn(w, sp);
        check(reuse == made[3] && pool.recycledCount() == 2, "a released slot is reused before anything is recycled");

        pool.clear(w);
        check(pool.activeCount() == 0 && pool.createdCount() == 4, "clear releases everything and keeps the entities");
        bool allOff = true;
        for (const Entity e : made) allOff = allOff && (w.component<CDecal>(e, kComponentDecal)->flags & kDecalDisabled) != 0;
        check(allOff, "all of them disabled");

        // Lifetimes: only decals with one expire, and the slot comes back.
        DecalSpawn mortal = sp;
        mortal.params.flags = 0;
        mortal.params.lifetimeSec = 1.0f;
        mortal.params.fadeOutSec = 0.5f;
        const Entity m = pool.spawn(w, mortal);
        const Entity forever = pool.spawn(w, sp);
        pool.tick(w, 0.5f);
        check(pool.isActive(m) && pool.isActive(forever), "both survive half a second");
        pool.tick(w, 0.6f);
        check(!pool.isActive(m) && pool.isActive(forever), "the mortal one expires and the permanent one stays");
        check(pool.activeCount() == 1, "one active left");
        const u32 recycledBefore = pool.recycledCount();
        for (int i = 0; i < 3; ++i) pool.spawn(w, sp);
        check(pool.activeCount() == 4 && pool.recycledCount() == recycledBefore, "the expired slot is reused with no recycling");

        pool.destroyAll(w);
        w.flush();
        check(pool.createdCount() == 0 && w.count() == before, "destroyAll retires every entity the pool made");
    }

    AVER_INFO("=== the pool against the world changing under it ===");
    {
        DecalPool pool(2);
        DecalSpawn sp;
        const Entity a = pool.spawn(w, sp);
        const Entity b = pool.spawn(w, sp);
        check(pool.createdCount() == 2, "two slots");
        w.destroy(a);
        w.flush();   // a level unload retires it behind the pool's back
        check(!w.valid(a), "the world retired the first decal's entity");
        const Entity c = pool.spawn(w, sp);
        check(c != kInvalidEntity && w.valid(c), "the next spawn gets a live entity");
        check(pool.createdCount() == 2 && pool.activeCount() == 2 && pool.isActive(b), "the stale slot was forgotten, not reused");
        check(!pool.owns(a), "and the dead entity is not the pool's any more");
        pool.destroyAll(w);
        w.flush();

        DecalPool none(0);
        check(none.spawn(w, sp) == kInvalidEntity && none.activeCount() == 0, "a pool of zero spawns nothing");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
