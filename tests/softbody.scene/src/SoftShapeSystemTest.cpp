// The soft shape system headless: jelly, rope and cable on scene entities, the cloth additions of wave 11 (fast solver,
// inflatables, pressure), the shared view list, grab and release, the cut, and the sensor events. Meshes and assets are
// injected and nothing is drawn, so the whole system is decidable with no GPU and no device. Physics is the real Jolt world,
// through Aver.Physics.
//
// NOTHING IN HERE HAS BEEN RUN. The numbers (settling times, volume tolerances, how far a jelly sinks) are starting values
// for the first headed pass; a failing threshold is a question about the number before it is a question about the code.
#include "SoftBodyFixtures.hpp"

#include "aver/physics/physics_softbody_ext_abi.h"
#include "aver/scene/scene_abi.h"
#include "aver/softbody/SoftShapeSystem.hpp"

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

constexpr u64 kBoxMesh = 301, kPillowMesh = 302, kSheetMesh = 303, kJellyAsset = 401, kFastAsset = 402, kInflateAsset = 403, kRopeAsset = 404;

// A rig that steps physics, flushes, ticks the cloth system and then the shape system, the order of a host frame.
struct ShapeRig : Rig {
    sb::SoftShapeSystem shapes{&sys};

    ShapeRig() {
        sb::SoftShapeSystem::registerComponents(w);
        shapes.install(w);
    }
    ~ShapeRig() { shapes.uninstall(); }

    void step(f32 dt = 1.0f / 60.0f) {
        aver_phys_step(dt);
        w.flush();
        sys.tick(w, dt);
        shapes.tick(w, dt);
    }
    void go(f32 seconds) {
        for (f32 t = 0.0f; t < seconds; t += 1.0f / 60.0f) step();
    }
    scene::Entity anchor(const char* name, Vec3 p) {
        const scene::Entity e = w.create(name);
        made.push_back(e);
        w.setLocalPosition(e, p);
        w.flush();
        return e;
    }
};

// A closed box of vertex-per-corner faces (so the weld has work to do), cross(B - A, C - A) outward.
static fmt::OcMeshData boxMesh(f32 hx, f32 hy, f32 hz) {
    fmt::OcMeshData m;
    struct Face { Vec3 n, u, v; };
    const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
                           {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    for (const Face& f : faces) {
        const Vec3 c{f.n.x * hx, f.n.y * hy, f.n.z * hz};
        const Vec3 u{f.u.x * hx, f.u.y * hy, f.u.z * hz}, v{f.v.x * hx, f.v.y * hy, f.v.z * hz};
        const Vec3 corner[4] = {c - u - v, c + u - v, c + u + v, c - u + v};
        const u32 base = static_cast<u32>(m.positions.size() / 3);
        const f32 uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k) {
            m.positions.insert(m.positions.end(), {corner[k].x, corner[k].y, corner[k].z});
            m.normals.insert(m.normals.end(), {f.n.x, f.n.y, f.n.z});
            m.uvs.insert(m.uvs.end(), {uv[k][0], uv[k][1]});
        }
        const Vec3 nrm = cross(corner[1] - corner[0], corner[2] - corner[0]);
        if (dot(nrm, c) >= 0.0f) m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        else m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    m.computeBounds();
    return m;
}

// A closed ellipsoid (UV sphere with poles), cross(B - A, C - A) outward. Flat in Z it is a pillow an inflation can fatten.
static fmt::OcMeshData ellipsoidMesh(f32 rx, f32 ry, f32 rz, u32 rings, u32 segs) {
    fmt::OcMeshData m;
    const auto add = [&](const Vec3& p, const Vec3& n, f32 u, f32 v) {
        m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
        m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
        m.uvs.insert(m.uvs.end(), {u, v});
    };
    add(Vec3{0, 0, rz}, Vec3{0, 0, 1}, 0.5f, 0.0f);
    for (u32 r = 1; r < rings; ++r) {
        const f32 phi = kPi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s < segs; ++s) {
            const f32 th = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segs);
            const Vec3 d{std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)};
            add(Vec3{rx * d.x, ry * d.y, rz * d.z}, d, static_cast<f32>(s) / static_cast<f32>(segs), static_cast<f32>(r) / static_cast<f32>(rings));
        }
    }
    add(Vec3{0, 0, -rz}, Vec3{0, 0, -1}, 0.5f, 1.0f);
    const u32 south = static_cast<u32>(m.positions.size() / 3) - 1;
    const auto ring = [&](u32 r, u32 s) { return 1 + (r - 1) * segs + (s % segs); };
    for (u32 s = 0; s < segs; ++s) m.indices.insert(m.indices.end(), {0u, ring(1, s + 1), ring(1, s)});
    for (u32 r = 1; r + 1 < rings; ++r)
        for (u32 s = 0; s < segs; ++s) {
            m.indices.insert(m.indices.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s)});
            m.indices.insert(m.indices.end(), {ring(r, s + 1), ring(r + 1, s + 1), ring(r + 1, s)});
        }
    for (u32 s = 0; s < segs; ++s) m.indices.insert(m.indices.end(), {south, ring(rings - 1, s), ring(rings - 1, s + 1)});
    m.computeBounds();
    return m;
}

static const sb::PieceView* viewOfEntity(const ShapeRig& r, scene::Entity e) {
    u32 count = 0;
    const sb::PieceView* v = r.sys.pieces(count);
    for (u32 i = 0; i < count; ++i)
        if (v[i].entity == e) return &v[i];
    return nullptr;
}

// ---- components -----------------------------------------------------------------------------------------

static void testComponents() {
    AVER_INFO("-- components --");
    ShapeRig r;
    scene::World& w = r.w;
    const u32 vol = w.componentId("CSoftVolume"), rope = w.componentId("CSoftRope");
    check(vol != 0 && rope != 0, "both are registered by name");
    check(w.componentSize(vol) == 48 && w.componentSize(rope) == 64, "sizes 48 and 64");
    check(w.componentVerified(vol) && w.componentVerified(rope), "their field tables cover the structs");
    check(sb::SoftShapeSystem::registerComponents(w) == vol, "registration is idempotent");
    const scene::FieldDesc* out = w.field(w.fieldId("CSoftVolume.outVolumeRatio"));
    const scene::FieldDesc* outR = w.field(w.fieldId("CSoftRope.outLengthCm"));
    const scene::FieldDesc* in = w.field(w.fieldId("CSoftRope.lengthCm"));
    const scene::FieldDesc* off = w.field(w.fieldId("CSoftRope.startOffset"));
    const scene::FieldDesc* ent = w.field(w.fieldId("CSoftRope.startEntity"));
    check(out && out->readOnly && outR && outR->readOnly && in && !in->readOnly, "OUT fields are read-only, inputs are not");
    check(off && off->kind == scene::FieldKind::Vec3 && ent && ent->kind == scene::FieldKind::Entity, "offsets are Vec3 fields and anchors are entity references");

    r.stores.meshes[kBoxMesh] = boxMesh(25, 25, 25);
    const scene::Entity bare = r.anchor("bare", Vec3{0, 0, 0});
    check(sb::SoftShapeSystem::attachJelly(w, bare) == nullptr, "a jelly needs a mesh renderer");
    check(sb::SoftShapeSystem::attachRope(w, bare, Vec3{0, 0, 0}, Vec3{100, 0, 0}) != nullptr, "a rope needs nothing else");
    check(sb::SoftShapeSystem::attachJelly(w, bare) == nullptr, "and an entity does not take both shapes");
    const scene::Entity box = r.entity("jelly", kBoxMesh, Vec3{0, 0, 100});
    auto* j = sb::SoftShapeSystem::attachJelly(w, box, sb::SoftPreset::Jelly);
    check(j && j->preset == static_cast<u32>(sb::SoftPreset::Jelly) && j->flags == 0 && j->stiffnessScale == 0.0f, "attach writes the preset over zero-filled storage");
    check(!w.hasComponent(box, scene::kComponentSoftBody) && !w.hasComponent(bare, scene::kComponentSoftBody), "neither shape carries the CSoftBody marker the cloth system would adopt");
}

// ---- rope -----------------------------------------------------------------------------------------------

static void testRopeHangsBetweenAnchors() {
    AVER_INFO("-- a rope between two anchors: nodes, length, sag, the published tube --");
    ShapeRig r;
    r.sys.setViewPos(Vec3{100, 0, 300});
    const scene::Entity a = r.anchor("a", Vec3{0, 0, 300});
    const scene::Entity b = r.anchor("b", Vec3{200, 0, 300});
    const scene::Entity rope = r.anchor("rope", Vec3{0, 0, 0});
    auto* c = sb::SoftShapeSystem::attachRope(r.w, rope, Vec3{0, 0, 0}, Vec3{0, 0, 0}, 0, 220.0f);
    check(c != nullptr, "the rope is attached");
    c->startEntity = a;
    c->endEntity = b;
    r.go(0.5f);
    check(r.shapes.isShape(rope) && r.shapes.bodyOf(rope) != 0, "the rope has a body after a few ticks");
    check(!r.w.hasComponent(rope, scene::kComponentSoftBody), "and no CSoftBody marker");
    check(r.w.hasComponent(rope, scene::kComponentMeshRenderer), "it was given a renderer for its tube");
    r.go(3.0f);

    const Vec3* nodes = nullptr;
    u32 n = 0;
    check(r.shapes.ropeNodes(rope, nodes, n) && n >= 20, "the rope has about 220 / 12 segments");
    check(((c->outState >> sb::kShapeCountShift) == n - 1) && (c->outState & sb::kShapeFailed) == 0, "outState reports the segment count");
    f32 lowest = 300.0f;
    for (u32 i = 0; i < n; ++i) lowest = std::min(lowest, nodes[i].z);
    AVER_INFO("  lowest node {:.1f}, length {:.1f}, strain {:.3f}", lowest, c->outLengthCm, c->outStrain);
    check(lowest < 300.0f - 15.0f, "the rope sags");
    check(dist(nodes[0], Vec3{0, 0, 300}) < 1.0f && dist(nodes[n - 1], Vec3{200, 0, 300}) < 1.0f, "both ends are held at their anchors");
    check(c->outLengthCm > 200.0f && c->outLengthCm < 240.0f, "its length stays near the 220 cm it was given");
    check(std::fabs(c->outStrain) < 0.05f, "a hanging rope is within 5% of its rest length");

    const sb::PieceView* v = viewOfEntity(r, rope);
    check(v != nullptr, "the rope publishes a view through the cloth system's list");
    if (v) {
        check(v->vertexCount == n * 9 && v->indexCount == (n - 1) * 8 * 6, "nodes x (sides + 1) vertices and two triangles per quad (8 sides)");
        bool allGenerated = true;
        for (u32 i = 0; i < v->vertexCount; ++i) allGenerated = allGenerated && v->sourceVertex[i] == sb::kGeneratedVertex;
        check(allGenerated && v->uv != nullptr && v->useEntityWorld, "every vertex is generated, with its own UVs, drawn through the entity's matrix");
        check(v->meshId == sb::shapeRopeMeshId() && r.sys.meshData(v->meshId) != nullptr, "its mesh id resolves to the placeholder the draw side asks for");
        f32 worstRing = 0.0f;
        for (u32 i = 0; i < n; ++i) worstRing = std::max(worstRing, dist(v->world[i * 9], v->world[i * 9 + 8]));
        check(worstRing < 1e-3f, "the seam vertex repeats the first of each ring");
    }

    // Moving an anchor moves the end it holds.
    r.w.setLocalPosition(b, Vec3{190, 0, 330});
    r.go(2.0f);
    check(r.shapes.ropeNodes(rope, nodes, n) && dist(nodes[n - 1], Vec3{190, 0, 330}) < 2.0f, "the end follows its anchor entity");

    // A freed end falls.
    c->flags |= sb::kRopeNoEndPin;
    r.go(2.0f);
    r.shapes.ropeNodes(rope, nodes, n);
    check(nodes[n - 1].z < 330.0f - 40.0f, "with the pin off the end drops");
    bool startAttached = false, endAttached = true;
    check(r.shapes.ropeAnchors(rope, startAttached, endAttached) && startAttached && !endAttached, "and the system reports only the start anchored");
}

static void testRopeFromAsset() {
    AVER_INFO("-- a rope asset sets the radius, the tube sides and the material --");
    ShapeRig r;
    sb::SoftAsset a = sb::makeSoftAsset(sb::SoftKind::Rope, sb::SoftPreset::Rope);
    a.rope.radiusCm = 2.5f;
    a.rope.sides = 6;
    a.rope.segmentCm = 20.0f;
    a.rope.material = "M_Cable";
    r.stores.assets[kRopeAsset] = a;
    const scene::Entity e = r.anchor("rope", Vec3{0, 0, 0});
    sb::SoftShapeSystem::attachRope(r.w, e, Vec3{0, 0, 300}, Vec3{300, 0, 300}, kRopeAsset);
    r.go(1.0f);
    const sb::PieceView* v = viewOfEntity(r, e);
    check(v != nullptr && v->vertexCount % 7 == 0, "six sides make seven vertices a ring");
    const auto* mr = r.w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    check(mr && mr->material == aver_scene_material(0, "M_Cable"), "the asset's material name reaches the renderer");
    if (v && v->vertexCount >= 14) {
        const f32 radius = dist(v->world[7 * 3], v->world[7 * 3 + 3]) * 0.5f;   // opposite vertices of a six sided ring (the seventh repeats the first)
        check(radius > 1.5f && radius < 2.6f, "the ring is about the asset's 2.5 cm radius");
    }
    sb::SoftAsset wrong = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
    r.stores.assets[kRopeAsset + 1] = wrong;
    const scene::Entity e2 = r.anchor("rope2", Vec3{0, 0, 0});
    auto* c2 = sb::SoftShapeSystem::attachRope(r.w, e2, Vec3{0, 500, 300}, Vec3{300, 500, 300}, kRopeAsset + 1);
    r.go(0.5f);
    check(c2 && (c2->outState & sb::kShapeAssetStale) != 0 && r.shapes.isShape(e2), "an asset of the wrong kind is flagged and the preset rope is used");
}

static void testGrabAndCut() {
    AVER_INFO("-- grab a rope, then cut it --");
    ShapeRig r;
    const scene::Entity top = r.anchor("top", Vec3{0, 0, 400});
    const scene::Entity rope = r.anchor("rope", Vec3{0, 0, 0});
    auto* c = sb::SoftShapeSystem::attachRope(r.w, rope, Vec3{0, 0, 0}, Vec3{0, 0, 0}, 0, 240.0f);
    c->startEntity = top;
    c->endOffset[2] = 160.0f;        // a free end still says where the rope is first laid out: 240 cm straight down from the hook
    c->flags |= sb::kRopeNoEndPin;   // hangs from the top, its far end free
    r.go(2.5f);
    const Vec3* nodes = nullptr;
    u32 n = 0;
    check(r.shapes.ropeNodes(rope, nodes, n) && n >= 10, "a hanging rope");
    check(nodes[n - 1].z < 400.0f - 100.0f, "its free end hangs well below the hook");
    const Vec3 tip = nodes[n - 1];

    // Grab the tip and pull it sideways.
    const Vec3 target = tip + Vec3{80, 0, 40};
    const i32 grab = r.shapes.grab(rope, tip, 30.0f, target);
    check(grab == static_cast<i32>(n), "the grab is the last vertex, id = index + 1");
    r.go(2.0f);
    r.shapes.ropeNodes(rope, nodes, n);
    check(dist(nodes[n - 1], target) < 6.0f, "the grabbed tip is carried to its target");
    check(r.shapes.moveGrab(rope, grab, target + Vec3{0, 60, 0}), "a grab can move");
    r.go(1.0f);
    r.shapes.ropeNodes(rope, nodes, n);
    check(dist(nodes[n - 1], target + Vec3{0, 60, 0}) < 6.0f, "and the tip follows");
    check(r.shapes.releaseGrab(rope, grab), "release");
    r.go(1.5f);
    r.shapes.ropeNodes(rope, nodes, n);
    check(nodes[n - 1].z < target.z + 5.0f, "after the release the tip swings down and does not rise above where it was let go");
    check(r.shapes.grab(rope, Vec3{5000, 5000, 5000}, 10.0f, target) == 0, "a grab with nothing in reach returns 0");

    // Cut near the middle.
    const u32 beforeNodes = n;
    r.shapes.ropeNodes(rope, nodes, n);
    const Vec3 at = nodes[n / 2];
    scene::Entity other = scene::kInvalidEntity;
    check(r.shapes.cutRope(r.w, rope, at, other) && other != scene::kInvalidEntity, "a cut near a node splits the rope and returns the new entity");
    r.made.push_back(other);                      // the world is shared: the rig must destroy the tail, or the next test finds a second rope
    scene::Entity none = scene::kInvalidEntity;   // cutRope clears its out parameter: the miss must not clear `other`
    check(!r.shapes.cutRope(r.w, rope, Vec3{9000, 0, 0}, none), "a point nowhere near the rope cuts nothing");
    r.go(0.5f);
    const Vec3 *hn = nullptr, *tn = nullptr;
    u32 headN = 0, tailN = 0;
    const bool both = r.shapes.ropeNodes(rope, hn, headN) && r.shapes.ropeNodes(other, tn, tailN);
    check(both, "both pieces are ropes");
    check(headN + tailN == beforeNodes + 1 && headN >= 2 && tailN >= 2, "the pieces share the cut node: head + tail = nodes + 1");
    const auto* tc = r.w.component<sb::CSoftRope>(other, sb::SoftShapeSystem::ropeType());
    check(tc && (tc->flags & sb::kRopeNoStartPin) != 0 && (tc->outState & sb::kShapeCut) != 0, "the new piece has a free start and says it was cut");
    check(viewOfEntity(r, rope) != nullptr && viewOfEntity(r, other) != nullptr, "each piece draws its own tube");
    r.go(2.0f);
    r.shapes.ropeNodes(other, tn, tailN);
    r.shapes.ropeNodes(rope, hn, headN);
    check(tn[0].z < at.z - 60.0f || tn[tailN - 1].z < at.z - 60.0f, "the severed tail falls");
    check(dist(hn[0], Vec3{0, 0, 400}) < 2.0f, "the head stays on its hook");
}

// ---- jelly ----------------------------------------------------------------------------------------------

static void testJellyDropsAndEmbeds() {
    AVER_INFO("-- a jelly cube drops, wobbles, keeps its volume and carries its mesh --");
    ShapeRig r;
    addFloor();
    r.stores.meshes[kBoxMesh] = boxMesh(25, 25, 25);
    sb::SoftAsset a = sb::makeSoftAsset(sb::SoftKind::Jelly, sb::SoftPreset::Jelly);
    a.jelly.cellCm = 10.0f;
    r.stores.assets[kJellyAsset] = a;
    r.sys.setViewPos(Vec3{0, 0, 100});
    const scene::Entity e = r.entity("jelly", kBoxMesh, Vec3{0, 0, 150});
    auto* c = sb::SoftShapeSystem::attachJelly(r.w, e, sb::SoftPreset::Jelly, kJellyAsset);
    r.go(0.3f);
    check(r.shapes.isShape(e), "the jelly has a body");
    check(c->outNodes >= 30 && c->outNodes <= 2048 && (c->outState & sb::kShapeFailed) == 0, "a 5 x 5 x 5 cell lattice, not failed");
    const sb::PieceView* v0 = viewOfEntity(r, e);
    check(v0 && v0->vertexCount == 24 && v0->indexCount == 36 && v0->meshId == kBoxMesh, "it publishes the authored mesh's own 24 vertices and 12 triangles");
    if (v0) {
        bool identity = true;
        for (u32 i = 0; i < v0->vertexCount; ++i) identity = identity && v0->sourceVertex[i] == i;
        check(identity && v0->uv == nullptr, "authored vertices: sourceVertex is the identity and no extra UVs");
    }
    r.go(3.0f);

    const sb::PieceView* v = viewOfEntity(r, e);
    Vec3 mean{0, 0, 0};
    if (v) mean = meanOf(*v);
    AVER_INFO("  rest height {:.1f}, volume ratio {:.3f}, max stretch {:.3f}", mean.z, c->outVolumeRatio, c->outMaxStretch);
    check(v && mean.z > 20.0f && mean.z < 60.0f, "it rests on the floor about half its height up");
    check(c->outVolumeRatio > 0.85f && c->outVolumeRatio < 1.15f, "its volume stays within 15% of rest");
    check(std::isfinite(c->outMaxStretch) && c->outMaxStretch < 0.6f, "no lattice edge is wildly stretched");

    // The embedded mesh stays inside the lattice's box, padded by half a cell.
    const sb::Lattice* lat = nullptr;
    const Vec3* nodes = nullptr;
    if (v && r.shapes.volumeLattice(e, lat, nodes)) {
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (usize i = 0; i < lat->nodes.size(); ++i) {
            lo = {std::min(lo.x, nodes[i].x), std::min(lo.y, nodes[i].y), std::min(lo.z, nodes[i].z)};
            hi = {std::max(hi.x, nodes[i].x), std::max(hi.y, nodes[i].y), std::max(hi.z, nodes[i].z)};
        }
        bool inside = true;
        for (u32 i = 0; i < v->vertexCount; ++i) {
            const Vec3& p = v->world[i];
            inside = inside && p.x > lo.x - 5.0f && p.x < hi.x + 5.0f && p.y > lo.y - 5.0f && p.y < hi.y + 5.0f && p.z > lo.z - 5.0f && p.z < hi.z + 5.0f;
        }
        check(inside, "every render vertex stays within half a cell of the lattice");
    } else {
        check(false, "the lattice is available to the overlay");
    }

    // Asleep, it stops repacking.
    r.go(5.0f);
    const sb::PieceView* sleepy = viewOfEntity(r, e);
    check(sleepy && (sleepy->frozen || (c->outState & sb::kShapeAwake) != 0), "a settled jelly reports its view frozen once the body sleeps");
}

static void testJellyRefusals() {
    AVER_INFO("-- a jelly over an open mesh, over the cap, or with no mesh --");
    {
        ShapeRig r;
        fmt::OcMeshData open = boxMesh(25, 25, 25);
        open.indices.resize(open.indices.size() - 3);   // one triangle gone
        r.stores.meshes[kBoxMesh] = open;
        const scene::Entity e = r.entity("open", kBoxMesh, Vec3{0, 0, 100});
        auto* c = sb::SoftShapeSystem::attachJelly(r.w, e);
        r.go(0.3f);
        check(c && (c->outState & sb::kShapeFailed) != 0 && !r.shapes.isShape(e), "an open mesh is refused; the entity keeps its authored mesh");
        check(viewOfEntity(r, e) == nullptr, "and publishes nothing");
    }
    {
        ShapeRig r;
        r.stores.meshes[kBoxMesh] = boxMesh(100, 100, 100);
        sb::SoftAsset a = sb::makeSoftAsset(sb::SoftKind::Jelly, sb::SoftPreset::Jelly);
        a.jelly.cellCm = 5.0f;
        a.jelly.maxNodes = 64;
        r.stores.assets[kJellyAsset] = a;
        const scene::Entity e = r.entity("big", kBoxMesh, Vec3{0, 0, 300});
        auto* c = sb::SoftShapeSystem::attachJelly(r.w, e, sb::SoftPreset::Jelly, kJellyAsset);
        r.go(0.3f);
        check(c && (c->outState & sb::kShapeOverCap) != 0 && !r.shapes.isShape(e), "a lattice over the node cap is refused with the over-cap bit");
    }
    {
        ShapeRig r;
        const scene::Entity e = r.entity("nomesh", 9999, Vec3{0, 0, 100});   // no such mesh
        auto* c = sb::SoftShapeSystem::attachJelly(r.w, e);
        r.go(0.3f);
        check(c && !r.shapes.isShape(e), "a mesh the sources do not resolve yet is retried, not built");
        r.stores.meshes[9999] = boxMesh(25, 25, 25);
        r.go(0.3f);
        check(r.shapes.isShape(e), "and builds once it resolves");
    }
}

// ---- cloth: fast solver and inflatables ----------------------------------------------------------------

static void testFastCloth() {
    AVER_INFO("-- a cloth on the fast solver drapes with Jolt's own constraints --");
    ShapeRig r;
    r.stores.meshes[kSheetMesh] = sheetMesh(10, 10, 10.0f, Plane::XZ);
    sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
    a.cloth.solver = sb::ClothSolver::Fast;
    a.cloth.bend = sb::ClothBend::Distance;
    a.cloth.lra = sb::ClothLra::Geodesic;
    r.stores.assets[kFastAsset] = a;
    const scene::Entity e = r.entity("flag", kSheetMesh, Vec3{0, 0, 300});
    auto* c = sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight, kFastAsset);
    c->flags |= sb::kClothTearable;   // ignored with a warning: the fast path has no beams to tear
    r.go(3.0f);
    check(r.sys.pieceCount(e) == 1 && !r.particles(e).empty(), "the fast cloth is built");
    const auto& ps = r.particles(e);
    f32 topZ = 0.0f, botZ = 0.0f;
    for (u32 col = 0; col < 10; ++col) { topZ += ps[col].pos.z; botZ += ps[9 * 10 + col].pos.z; }
    topZ /= 10.0f; botZ /= 10.0f;
    check(std::fabs(topZ - 300.0f) < 0.5f, "its pinned row stays");
    check(botZ < topZ - 70.0f && botZ > topZ - 100.0f, "it hangs about its 90 cm (the long range attachments hold it to its length)");
    check(r.sys.state(e).brokenEdges == 0, "nothing tears");
    bool finite = true;
    for (const sb::Particle& p : ps) finite = finite && std::isfinite(p.pos.x + p.pos.y + p.pos.z);
    check(finite, "every particle is finite");
}

static void testInflatable() {
    AVER_INFO("-- an inflatable fattens with pressure and deflates when asked --");
    f32 volume[2] = {0.0f, 0.0f};
    for (int pressurised = 0; pressurised < 2; ++pressurised) {
        ShapeRig r;
        r.stores.meshes[kPillowMesh] = ellipsoidMesh(40, 40, 12, 14, 24);
        sb::SoftAsset a = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        a.sim.gravityScale = 0.0f;
        a.inflate.pressureKPa = pressurised ? 6.0f : 0.0f;
        a.sim.allowSleep = false;
        r.stores.assets[kInflateAsset] = a;
        const scene::Entity e = r.entity("pillow", kPillowMesh, Vec3{0, 0, 400});
        sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight, kInflateAsset);
        r.go(4.0f);
        f32 rest = 0.0f, now = 0.0f;
        const bool ok = r.shapes.volumeOf(e, rest, now);
        check(ok && rest > 0.0f, std::string("the pillow reports its volume (") + (pressurised ? "pressurised" : "plain") + ")");
        volume[pressurised] = rest > 0.0f ? now / rest : 0.0f;
        AVER_INFO("  volume ratio {:.3f}", volume[pressurised]);
        if (pressurised) {
            check(r.sys.pressureOf(e) > 5.9f, "the live pressure is the asset's");
            check(r.shapes.setPressure(e, 0.0f), "setPressure(0) is accepted");
            r.go(0.5f);
            check(r.sys.pressureOf(e) == 0.0f, "and applied at the next tick");
            r.go(3.0f);
            f32 restB = 0.0f, nowB = 0.0f;
            r.shapes.volumeOf(e, restB, nowB);
            check(restB > 0.0f && nowB / restB < volume[1] - 0.02f, "a deflated pillow is thinner than an inflated one");
            check(!r.shapes.setPressure(e, -5.0f), "a negative pressure is refused");
        }
    }
    check(volume[1] > volume[0] + 0.05f, "with pressure the pillow holds more volume than without");

    // An open sheet cannot inflate: it simulates as plain cloth and says so.
    ShapeRig r;
    r.stores.meshes[kSheetMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ);
    sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
    a.inflate.pressureKPa = 3.0f;
    r.stores.assets[kInflateAsset] = a;
    const scene::Entity e = r.entity("sheet", kSheetMesh, Vec3{0, 0, 300});
    sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight, kInflateAsset);
    r.go(0.5f);
    check(r.sys.pieceCount(e) == 1 && r.sys.pressureOf(e) == 0.0f, "an inflate section over an open mesh is ignored");
}

static void testLeak() {
    AVER_INFO("-- a puncture ramps the pressure down --");
    ShapeRig r;
    r.stores.meshes[kPillowMesh] = ellipsoidMesh(40, 40, 12, 14, 24);
    sb::SoftAsset a = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
    a.sim.gravityScale = 0.0f;
    a.inflate.pressureKPa = 4.0f;
    a.inflate.leakKPaPerS = 2.0f;
    r.stores.assets[kInflateAsset] = a;
    const scene::Entity e = r.entity("leaky", kPillowMesh, Vec3{0, 0, 400});
    sb::SoftBodySystem::attachCloth(r.w, e, sb::SoftPreset::ClothLight, kInflateAsset);
    r.go(0.5f);
    const f32 early = r.sys.pressureOf(e);
    r.go(3.0f);
    check(early > 2.5f && early < 4.0f, "the pressure starts near the asset's and is falling");
    check(r.sys.pressureOf(e) == 0.0f, "after two seconds at 2 kPa/s it is flat");
}

// ---- the shared view list, buoyancy, sensors ---------------------------------------------------------------

static void testViewSources() {
    AVER_INFO("-- one view list: cloth, jelly and rope together --");
    ShapeRig r;
    r.stores.meshes[kSheetMesh] = sheetMesh(6, 6, 10.0f, Plane::XZ);
    r.stores.meshes[kBoxMesh] = boxMesh(20, 20, 20);
    const scene::Entity cloth = r.entity("cloth", kSheetMesh, Vec3{0, 0, 300});
    sb::SoftBodySystem::attachCloth(r.w, cloth, sb::SoftPreset::ClothLight);
    const scene::Entity jelly = r.entity("jelly", kBoxMesh, Vec3{300, 0, 200});
    sb::SoftShapeSystem::attachJelly(r.w, jelly);
    const scene::Entity rope = r.anchor("rope", Vec3{0, 0, 0});
    sb::SoftShapeSystem::attachRope(r.w, rope, Vec3{600, 0, 300}, Vec3{800, 0, 300});
    r.go(1.0f);
    u32 count = 0;
    const sb::PieceView* v = r.sys.pieces(count);
    u32 seen[3] = {0, 0, 0};
    for (u32 i = 0; i < count; ++i) {
        seen[0] += v[i].entity == cloth;
        seen[1] += v[i].entity == jelly;
        seen[2] += v[i].entity == rope;
    }
    check(seen[0] == 1 && seen[1] == 1 && seen[2] == 1, "the draw side reads one list with a view for each");
    check(r.sys.stats().entries == 1, "the cloth system's own stats count only its entry");
    check(r.shapes.stats().volumes == 1 && r.shapes.stats().ropes == 1 && r.shapes.stats().nodes > 0, "the shape system has its own");

    r.w.destroy(rope);
    r.w.flush();
    r.go(0.2f);
    check(!r.shapes.isShape(rope) && viewOfEntity(r, rope) == nullptr, "a destroyed rope's body and view are gone");

    r.shapes.clear();
    r.sys.refreshViewSources();
    check(viewOfEntity(r, jelly) == nullptr && viewOfEntity(r, cloth) != nullptr, "clear() empties the shape views and leaves the cloth's");
    r.go(0.5f);
    check(r.shapes.isShape(jelly), "and the next tick rebuilds the jelly from its component");
}

static void testBuoyancyAndContact() {
    AVER_INFO("-- soft-body ops reach a jelly: buoyancy, contact scales, a sensor pair --");
    ShapeRig r;
    addFloor();
    r.stores.meshes[kBoxMesh] = boxMesh(25, 25, 25);
    const scene::Entity e = r.entity("jelly", kBoxMesh, Vec3{0, 0, 80});
    sb::SoftShapeSystem::attachJelly(r.w, e);
    r.go(0.3f);
    check(r.shapes.setBuoyancy(e, 1000.0f, 1.0f), "buoyancy is accepted");
    f32 fraction = -1.0f, volumeCm3 = -1.0f;
    check(r.shapes.submerged(e, fraction, volumeCm3) && fraction >= 0.0f && fraction <= 1.0f, "submerged reads a fraction between 0 and 1");
    check(r.shapes.setContact(e, 1.0f, 0.0f), "contact scales are accepted");
    check(!r.shapes.setBuoyancy(r.anchor("nothing", Vec3{0, 0, 0}), 1000.0f, 1.0f), "an entity with no soft body is refused");

    // A box falling onto the jelly with the pair set to SENSOR passes through and produces an enter event.
    const scene::Entity boxE = r.anchor("box", Vec3{0, 0, 400});
    const i32 handle = aver_phys_add_dynamic_box(0.0f, 0.0f, 150.0f, 10.0f, 10.0f, 10.0f, 2.0f);
    auto* rb = static_cast<scene::CRigidBody*>(r.w.addComponent(boxE, scene::kComponentRigidBody));
    rb->body = handle;
    check(r.shapes.setPairMode(e, boxE, AVER_PHYS_SBX_PAIR_SENSOR), "the pair is set to sensor");
    u32 entered = 0, exited = 0;
    for (f32 t = 0.0f; t < 2.5f; t += 1.0f / 60.0f) {
        r.step();
        sb::SoftSensorEvent evs[16];
        const u32 n = r.shapes.drainSensorEvents(evs, 16);
        for (u32 i = 0; i < n; ++i) {
            if (evs[i].soft == e && evs[i].sensor == boxE) { entered += evs[i].entered; exited += !evs[i].entered; }
        }
    }
    AVER_INFO("  sensor events: {} entered, {} exited", entered, exited);
    check(entered >= 1, "the box entering the jelly raised an entered event naming both entities");
    float p[3] = {0, 0, 0};
    aver_phys_body_position(handle, p);
    check(p[2] < 40.0f, "and it fell through to the floor instead of resting on the jelly");
}

static void testDeterminism() {
    AVER_INFO("-- two identical runs of a rope, a jelly and a fast cloth hash alike --");
    u64 hashes[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        ShapeRig r;
        addFloor();
        r.stores.meshes[kBoxMesh] = boxMesh(25, 25, 25);
        r.stores.meshes[kSheetMesh] = sheetMesh(8, 8, 10.0f, Plane::XZ);
        sb::SoftAsset fast = pinnedTopAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        fast.cloth.solver = sb::ClothSolver::Fast;
        r.stores.assets[kFastAsset] = fast;
        const scene::Entity jelly = r.entity("jelly", kBoxMesh, Vec3{0, 0, 120});
        sb::SoftShapeSystem::attachJelly(r.w, jelly);
        const scene::Entity cloth = r.entity("cloth", kSheetMesh, Vec3{400, 0, 300});
        sb::SoftBodySystem::attachCloth(r.w, cloth, sb::SoftPreset::ClothLight, kFastAsset);
        const scene::Entity rope = r.anchor("rope", Vec3{0, 0, 0});
        sb::SoftShapeSystem::attachRope(r.w, rope, Vec3{-300, 0, 300}, Vec3{-100, 0, 300}, 0, 230.0f);
        r.go(2.5f);
        u64 h = 1469598103934665603ull;
        const auto mix = [&](f32 f) {
            u32 bits;
            std::memcpy(&bits, &f, sizeof bits);
            h = (h ^ bits) * 1099511628211ull;
        };
        u32 count = 0;
        const sb::PieceView* v = r.sys.pieces(count);
        for (u32 i = 0; i < count; ++i)
            for (u32 k = 0; k < v[i].vertexCount; ++k) { mix(v[i].world[k].x); mix(v[i].world[k].y); mix(v[i].world[k].z); }
        hashes[run] = h;
    }
    check(hashes[0] == hashes[1] && hashes[0] != 1469598103934665603ull, "the same inputs give the same published vertices bit for bit");
}

int main() {
    AVER_INFO("SoftShapeSystemTest");
    testComponents();
    testRopeHangsBetweenAnchors();
    testRopeFromAsset();
    testGrabAndCut();
    testJellyDropsAndEmbeds();
    testJellyRefusals();
    testFastCloth();
    testInflatable();
    testLeak();
    testViewSources();
    testBuoyancyAndContact();
    testDeterminism();
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
