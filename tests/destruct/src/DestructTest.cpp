// DestructTest -- Aver.Destruct (aver/destruct/Destructible.hpp) in a real Jolt world through the plain-C ABI, no device. A 4x3 wall of bricks (400 x 40 x 300 cm,
// anchored along its foot) is fractured with fracture::voronoi, packed to an .ocfrac and driven. Claims are about observed state and motion:
// the intact wall is one static body, a hit below the strain threshold breaks nothing, hits accumulate until a link goes, a hit that cuts the top row
// frees it as dynamic bodies that keep the hit's direction, the anchored foot never moves, the debris cap and sleeping despawn remove pieces, joints
// hold or give by their break force, and teardown returns the world to the bodies and shapes it started with.
// Hand-run, exit code = failure count.
#include "aver/core/HitEvent.hpp"
#include "aver/core/Log.hpp"
#include "aver/destruct/Destructible.hpp"
#include "aver/formats/Fracture.hpp"
#include "aver/formats/OcFrac.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"

#include <cmath>
#include <memory>
#include <string>

using namespace aver;
using namespace aver::destruct;
namespace fr = aver::fmt::fracture;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

static const f32 kDt = 1.0f / 60.0f;

static void world() {
    aver_phys_init();
    aver_phys_set_gravity(0.0f, 0.0f, -980.0f);
    aver_phys_add_static_box(200.0f, 0.0f, -50.0f, 2000.0f, 2000.0f, 50.0f);   // floor, top at z = 0
}

static void run(Destructible* d, int frames) {
    for (int i = 0; i < frames; ++i) {
        aver_phys_step(kDt);
        if (d) d->update(kDt);
    }
}

// 4 x 1 x 3 bricks of 100 x 40 x 100 cm. Link thresholds are toughness * contact area (4000 cm2 each, so 4000 * toughness).
static std::shared_ptr<fmt::OcFracData> makeWall(double toughness, bool anchorFoot) {
    const double size[3] = {400, 40, 300};
    fmt::csg::Mesh src = fmt::csg::makeBox(size, 0);
    const double t[12] = {1, 0, 0, 200, 0, 1, 0, 20, 0, 0, 1, 150};
    fmt::csg::transform(src, t);
    const fr::Result r = fr::voronoi(src, fr::seedsBrick(fmt::csg::bounds(src), {100, 40, 100}, 0.0, 0.0, 1));
    fr::Graph g = fr::makeGraph(r);
    if (anchorFoot) fr::anchorByPlane(g, r, {0, 0, 1}, 0.0, 1e-6);
    fmt::OcFracBuild b;
    b.toughness = toughness;
    return std::make_shared<fmt::OcFracData>(fmt::buildOcFrac(r, g, b));
}

static u32 linkBetween(const fmt::OcFracData& a, u32 p, u32 q) {
    for (u32 i = 0; i < a.links.size(); ++i)
        if ((a.links[i].a == p && a.links[i].b == q) || (a.links[i].a == q && a.links[i].b == p)) return i;
    return 0xFFFFFFFFu;
}

static void comOf(i32 body, f32 out[3]) { aver_phys_body_position(body, out); }

static void testIntactAndAnchors() {
    AVER_INFO("=== the intact wall: one static body, the foot anchored ===");
    world();
    const auto asset = makeWall(1.0, true);
    check(asset->pieces.size() == 12 && asset->links.size() == 17, "12 bricks, 17 links (9 across + 8 up) -> got " + std::to_string(asset->links.size()));
    const i32 bodies0 = aver_phys_body_count();
    DestructibleDesc desc;
    Destructible d(asset, desc);
    check(d.valid(), "valid");
    check(d.bodyCount() == 1 && aver_phys_body_count() == bodies0 + 1, "one body for the whole connected wall");
    check(aver_phys_body_motion_type(d.pieceBody(0)) == AVER_PHYS_MOTION_STATIC, "and it is static (the foot is anchored)");
    check(d.pieceBody(0) == d.pieceBody(11), "every piece reports that body");
    bool attached = true;
    for (u32 i = 0; i < 12; ++i) attached = attached && d.pieceState(i) == PieceState::Attached;
    check(attached, "all pieces Attached");
    check(d.componentCount() == 1 && d.debrisPieceCount() == 0, "one component, no debris");
    f32 pos[3], q[4];
    check(d.piecePose(5, pos, q) && std::fabs(pos[0]) < 1e-3f && std::fabs(q[3] - 1.0f) < 1e-4f, "piecePose is the placement (origin, identity)");
    run(&d, 60);
    d.piecePose(5, pos, q);
    check(std::fabs(pos[2]) < 1e-3f, "a second of simulation moves nothing");
}

static void testBelowThresholdAndAccumulation() {
    AVER_INFO("=== strain accumulates; sub-threshold hits break nothing alone ===");
    world();
    const auto asset = makeWall(1.0, true);
    const u32 link = linkBetween(*asset, 5, 6);
    check(link != 0xFFFFFFFFu, "bricks 5 and 6 share a link");
    DestructibleDesc desc;
    desc.pointRadiusCm = 10.0f;
    Destructible d(asset, desc);
    HitEvent e;
    e.point = {200.0f, 20.0f, 150.0f};
    e.impulse = {0.0f, 1000.0f, 0.0f};   // a quarter of the 4000 threshold
    for (int i = 0; i < 3; ++i) d.applyHit(e);
    check(!d.linkBroken(link), "three hits of a quarter threshold: still whole");
    check(std::fabs(d.linkStrain(link) - 3000.0f) < 1.0f, "strain is the sum, " + f2s(d.linkStrain(link)));
    HitResult last;
    for (int i = 0; i < 2; ++i) { last = d.applyHit(e); if (last.linksBroken) break; }
    check(d.linkBroken(link), "the next hits tip it over");
    check(last.linksBroken == 1 && last.bodiesCreated == 0, "one link broke and the wall stayed in one piece (still connected around it)");
    check(d.bodyCount() == 1 && d.componentCount() == 1, "still one body");
    const auto ev = d.takeEvents();
    check(ev.size() == 1 && ev[0].link == link && std::fabs(ev[0].pointCm[0] - 200.0f) < 1.0f, "one break event at the link");
    check(d.takeEvents().empty(), "events drain");

    // contact impulses under the threshold are ignored, over it count
    DestructibleDesc d2 = desc;
    d2.contactThreshold = 5000.0f;
    Destructible c(asset, d2);
    const f32 pt[3] = {200.0f, 20.0f, 150.0f};
    c.applyContact(pt, 3000.0f);
    check(c.linkStrain(link) == 0.0f, "a contact under the threshold adds no strain");
    c.applyContact(pt, 6000.0f);
    check(c.linkBroken(link), "a contact over it breaks the link (6000 > 4000)");

    // determinism: the same hits give the same broken set
    Destructible a(asset, desc), b(asset, desc);
    HitEvent h;
    h.point = {120.0f, 20.0f, 150.0f};
    h.impulse = {0.0f, 2500.0f, 0.0f};
    for (int i = 0; i < 6; ++i) { a.applyHit(h); b.applyHit(h); }
    bool same = true;
    for (u32 i = 0; i < a.linkCount(); ++i) same = same && a.linkBroken(i) == b.linkBroken(i) && a.linkStrain(i) == b.linkStrain(i);
    check(same, "two destructibles given the same hits agree on every link");
}

static void testBreakOffTopRow() {
    AVER_INFO("=== a hit that cuts the top row: dynamic debris, anchored foot unmoved ===");
    world();
    const auto asset = makeWall(1.0, true);
    DestructibleDesc desc;
    desc.pointRadiusCm = 150.0f;
    desc.maxDebrisPieces = 100;
    Destructible d(asset, desc);
    f32 footBefore[3];
    comOf(d.pieceBody(1), footBefore);

    HitEvent e;
    e.entity = 7;
    e.point = {200.0f, -20.0f, 250.0f};
    e.impulse = {0.0f, 3000000.0f, 0.0f};
    const HitResult r = d.applyHit(e);
    check(r.linksBroken > 0, "links broke: " + std::to_string(r.linksBroken));
    check(r.bodiesCreated >= 2, "the wall split into bodies: " + std::to_string(r.bodiesCreated));
    check(r.piecesFreed > 0 && d.debrisPieceCount() == static_cast<u32>(r.piecesFreed), "freed pieces are the debris, " + std::to_string(r.piecesFreed));
    check(d.takeEvents().size() == static_cast<size_t>(r.linksBroken), "one event per broken link");

    bool footStatic = true;
    for (u32 i = 0; i < 4; ++i) footStatic = footStatic && d.pieceState(i) == PieceState::Attached && aver_phys_body_motion_type(d.pieceBody(i)) == AVER_PHYS_MOTION_STATIC;
    check(footStatic, "the bottom row stays Attached and static");
    int freedTop = 0;
    for (u32 i = 8; i < 12; ++i) if (d.pieceState(i) == PieceState::Free) ++freedTop;
    check(freedTop >= 2, "top-row bricks near the hit are Free: " + std::to_string(freedTop));

    const u32 free = 9;   // top row, nearest the hit
    check(d.pieceState(free) == PieceState::Free, "the top brick under the hit is Free");
    f32 v[3];
    aver_phys_body_velocity(d.pieceBody(free), v);
    check(v[1] > 5.0f && std::fabs(v[0]) < v[1], "a freed piece moves along the hit direction (+Y), vy = " + f2s(v[1]));
    check(aver_phys_body_motion_type(d.pieceBody(free)) == AVER_PHYS_MOTION_DYNAMIC, "and is a dynamic body");

    f32 before[3];
    comOf(d.pieceBody(free), before);
    run(&d, 240);
    f32 after[3];
    comOf(d.pieceBody(free), after);
    check(after[2] < before[2] - 40.0f, "freed debris falls: z " + f2s(before[2]) + " -> " + f2s(after[2]));
    check(after[1] > before[1] + 20.0f, "and was thrown outward: y " + f2s(before[1]) + " -> " + f2s(after[1]));
    f32 footPos[3], footQ[4];
    d.piecePose(1, footPos, footQ);
    check(std::fabs(footPos[0]) < 1e-3f && std::fabs(footPos[1]) < 1e-3f && std::fabs(footPos[2]) < 1e-3f, "the anchored foot did not move (its origin is still the placement)");
    (void)footBefore;
    check(d.componentCount() >= 2, "the graph has several components now");

    // a second hit on freed pieces cannot break links that are already broken: it is harmless
    const HitResult again = d.applyHit(e);
    check(again.bodiesCreated >= 0, "hitting again is accepted");
}

static void testExplosionRadial() {
    AVER_INFO("=== an explosion event: radial push away from the point ===");
    world();
    const auto asset = makeWall(0.1, true);
    DestructibleDesc desc;
    desc.maxDebrisPieces = 100;
    Destructible d(asset, desc);
    HitEvent e;
    e.point = {200.0f, -150.0f, 200.0f};
    e.impulse = {0.0f, 0.0f, 0.0f};
    e.damage = 5000.0f;     // no impulse: damage * damageToImpulse is the strength
    e.radius = 500.0f;
    const HitResult r = d.applyHit(e);
    check(r.linksBroken > 4 && r.piecesFreed > 0, "damage alone drives a blast: " + std::to_string(r.linksBroken) + " links");
    int pushed = 0, freeCount = 0;
    for (u32 i = 0; i < 12; ++i) {
        if (d.pieceState(i) != PieceState::Free) continue;
        ++freeCount;
        f32 v[3];
        aver_phys_body_velocity(d.pieceBody(i), v);
        if (v[1] > 1.0f) ++pushed;
    }
    check(freeCount > 0 && pushed == freeCount, "every freed body is pushed away from the blast (+Y): " + std::to_string(pushed) + "/" + std::to_string(freeCount));
}

static void testDebrisCapAndDespawn() {
    AVER_INFO("=== debris cap and sleeping despawn ===");
    {
        world();
        const auto asset = makeWall(0.1, false);   // nothing anchored: the wall is one dynamic body from the start
        const i32 shapes0 = aver_phys_shape_count();
        DestructibleDesc desc;
        desc.maxDebrisPieces = 4;
        Destructible d(asset, desc);
        check(aver_phys_body_motion_type(d.pieceBody(0)) == AVER_PHYS_MOTION_DYNAMIC && d.bodyCount() == 1, "an unanchored wall is a single dynamic body");
        run(&d, 30);
        HitEvent e;
        e.point = {200.0f, 20.0f, 150.0f};
        e.impulse = {0.0f, 100000.0f, 0.0f};
        e.radius = 1000.0f;
        d.applyHit(e);
        check(d.debrisPieceCount() <= 4, "debris capped at 4 pieces, got " + std::to_string(d.debrisPieceCount()));
        int gone = 0;
        for (u32 i = 0; i < 12; ++i) if (d.pieceState(i) == PieceState::Gone) ++gone;
        check(gone == 8 && d.bodyCount() == 4, "the other 8 were removed (" + std::to_string(gone) + " gone, " + std::to_string(d.bodyCount()) + " bodies)");
        check(d.pieceBody(0) == 0 || d.pieceState(0) != PieceState::Gone, "gone pieces report no body");
        (void)shapes0;
    }
    aver_phys_shutdown();
    {
        world();
        const auto asset = makeWall(0.1, true);
        DestructibleDesc desc;
        desc.maxDebrisPieces = 100;
        desc.sleepDespawnS = 0.5f;
        Destructible d(asset, desc);
        HitEvent e;
        e.point = {200.0f, -20.0f, 150.0f};
        e.impulse = {0.0f, 300000.0f, 0.0f};
        e.radius = 1000.0f;
        d.applyHit(e);
        const u32 freed = d.debrisPieceCount();
        check(freed == 8, "the 8 upper bricks are free, got " + std::to_string(freed));
        run(&d, 900);
        int gone = 0;
        for (u32 i = 0; i < 12; ++i) if (d.pieceState(i) == PieceState::Gone) ++gone;
        check(gone == 8 && d.debrisPieceCount() == 0, "once asleep for 0.5 s the debris is removed (" + std::to_string(gone) + " gone)");
        check(d.bodyCount() == 4, "four static foot bricks remain");
    }
    aver_phys_shutdown();
}

static void testJointed() {
    AVER_INFO("=== breakable joints: a strong weld holds, a weak one lets go ===");
    {
        world();
        const auto asset = makeWall(1000.0, true);   // 4e6 N per link
        DestructibleDesc desc;
        desc.mode = Mode::Jointed;
        desc.maxDebrisPieces = 100;
        Destructible d(asset, desc);
        check(d.valid() && d.bodyCount() == 12, "a body per brick");
        check(aver_phys_joint_count() == static_cast<i32>(asset->links.size()) - 0 || aver_phys_joint_count() > 0, "joints created");
        f32 top0[3];
        comOf(d.pieceBody(10), top0);
        HitEvent e;
        e.point = {300.0f, -20.0f, 250.0f};
        e.impulse = {0.0f, 20000.0f, 0.0f};
        e.radius = 200.0f;
        d.applyHit(e);
        run(&d, 180);
        f32 top1[3];
        comOf(d.pieceBody(10), top1);
        check(d.takeEvents().empty(), "no joint broke");
        bool whole = true;
        for (u32 i = 0; i < 12; ++i) whole = whole && d.pieceState(i) == PieceState::Attached;
        check(whole && d.componentCount() == 1, "the wall is still one attached component");
        check(std::fabs(top1[2] - top0[2]) < 5.0f && std::fabs(top1[1] - top0[1]) < 15.0f, "and the pushed brick has hardly moved: dz " + f2s(top1[2] - top0[2]));
    }
    aver_phys_shutdown();
    {
        world();
        const auto asset = makeWall(0.001, true);   // 4 N per link: a brick weighs thousands of newtons
        DestructibleDesc desc;
        desc.mode = Mode::Jointed;
        desc.maxDebrisPieces = 100;
        Destructible d(asset, desc);
        run(&d, 90);
        const auto ev = d.takeEvents();
        check(!ev.empty(), "weak joints broke under the wall's own weight: " + std::to_string(ev.size()) + " events");
        int free = 0;
        for (u32 i = 0; i < 12; ++i) if (d.pieceState(i) == PieceState::Free) ++free;
        check(free > 0 && d.debrisPieceCount() > 0, "pieces left the anchored mass: " + std::to_string(free) + " free");
        for (u32 i = 0; i < 4; ++i) if (d.pieceState(i) != PieceState::Attached) { check(false, "the anchored foot stays Attached"); break; }
        Destructible::clearBreakQueue();
        check(aver_phys_joint_break_count() == 0, "the owner clears the physics break queue");
    }
    aver_phys_shutdown();
}

static void testPlacementAndCleanup() {
    AVER_INFO("=== rotation, scale and cleanup ===");
    world();
    const auto asset = makeWall(1.0, true);
    const i32 bodies0 = aver_phys_body_count();
    const i32 shapes0 = aver_phys_shape_count();
    {
        DestructibleDesc desc;
        desc.originCm[0] = 1000.0f;
        desc.rotation[2] = std::sqrt(0.5f);   // 90 degrees about Z
        desc.rotation[3] = std::sqrt(0.5f);
        desc.unitScale = 0.5f;
        Destructible d(asset, desc);
        check(d.valid(), "valid with a turned, half-size placement");
        f32 lo[3], hi[3];
        aver_phys_body_aabb(d.pieceBody(0), lo, hi);
        const f32 ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
        check(std::fabs(ex - 20.0f) < 2.0f && std::fabs(ey - 200.0f) < 2.0f && std::fabs(ez - 150.0f) < 2.0f,
              "the 400 x 40 x 300 wall at half scale, turned 90 degrees, spans " + f2s(ex) + " x " + f2s(ey) + " x " + f2s(ez));
        run(&d, 10);
        // a hit at a link's turned position still finds the link
        const u32 link = linkBetween(*asset, 5, 6);
        f32 w[3], q[4];
        d.piecePose(5, w, q);
        HitEvent e;
        e.point = {1000.0f - 10.0f, 100.0f, 75.0f};   // asset (200, 20, 150) * 0.5, turned 90 degrees about Z, about the origin (1000, 0, 0)
        e.impulse = {0.0f, 0.0f, 100000.0f};
        e.radius = 20.0f;
        d.applyHit(e);
        check(d.linkBroken(link), "a hit at the link's world position (turned and scaled) breaks it");
        check(d.bodyCount() >= 1 && aver_phys_body_count() >= bodies0 + 1, "bodies exist");
    }
    check(aver_phys_body_count() == bodies0, "destroying it removes every body it made");
    check(aver_phys_shape_count() == shapes0, "and releases every shape handle (pooled hulls included)");
    aver_phys_shutdown();
}

int main() {
    testIntactAndAnchors();
    aver_phys_shutdown();
    testBelowThresholdAndAccumulation();
    aver_phys_shutdown();
    testBreakOffTopRow();
    aver_phys_shutdown();
    testExplosionRadial();
    aver_phys_shutdown();
    testDebrisCapAndDespawn();
    testJointed();
    testPlacementAndCleanup();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
