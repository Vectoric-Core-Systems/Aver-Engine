// The shape system: components, entry lifecycle, views, and the ops the framework relay and the graph rows reach. Jelly is in
// SoftShapeVolume.cpp, rope and cut in SoftShapeRope.cpp, the relay provider in SoftShapeProvider.cpp.
#include "aver/softbody/SoftShapeSystem.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/physics/physics_softbody_ext_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::softbody {
namespace {

u32 g_volumeType = 0;
u32 g_ropeType = 0;

f32 or1(f32 v) { return v > 0.0f ? v : 1.0f; }

// One body per entity root: a cloth or a destructible (SoftBodySystem) has the mesh already.
bool hasCloth(scene::World& world, scene::Entity e) {
    for (const char* name : {"CCloth", "CDestructible"}) {
        const u32 t = world.componentId(name);
        if (t != 0 && world.hasComponent(e, t)) return true;
    }
    return false;
}

}  // namespace

// The id of the engine cube: the placeholder mesh of a rope that has no renderer, and the id its (all generated) views carry.
u64 shapeRopeMeshId() {
    static const u64 id = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    return id;
}

// ---- components ------------------------------------------------------------------------------------------

u32 SoftShapeSystem::registerComponents(scene::World& world) {
    using scene::FieldKind;
    {
        auto b = world.registerComponent<CSoftVolume>("CSoftVolume");
        b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CSoftVolume, asset)))
            .field("preset", FieldKind::I32, static_cast<u16>(offsetof(CSoftVolume, preset)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CSoftVolume, flags)))
            .field("stiffnessScale", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, stiffnessScale)))
            .field("cellCm", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, cellCm)))
            .field("gravityScale", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, gravityScale)))
            .field("dampingScale", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, dampingScale)))
            .field("outVolumeRatio", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, outVolumeRatio)), 0, true)
            .field("outMaxStretch", FieldKind::F32, static_cast<u16>(offsetof(CSoftVolume, outMaxStretch)), 0, true)
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CSoftVolume, outState)), 0, true)
            .field("outNodes", FieldKind::I32, static_cast<u16>(offsetof(CSoftVolume, outNodes)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CSoftVolume)), "CSoftVolume");
        g_volumeType = b.typeId();
    }
    {
        auto b = world.registerComponent<CSoftRope>("CSoftRope");
        b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CSoftRope, asset)))
            .field("startEntity", FieldKind::Entity, static_cast<u16>(offsetof(CSoftRope, startEntity)))
            .field("endEntity", FieldKind::Entity, static_cast<u16>(offsetof(CSoftRope, endEntity)))
            .field("startOffset", FieldKind::Vec3, static_cast<u16>(offsetof(CSoftRope, startOffset)))
            .field("endOffset", FieldKind::Vec3, static_cast<u16>(offsetof(CSoftRope, endOffset)))
            .field("lengthCm", FieldKind::F32, static_cast<u16>(offsetof(CSoftRope, lengthCm)))
            .field("radiusCm", FieldKind::F32, static_cast<u16>(offsetof(CSoftRope, radiusCm)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CSoftRope, flags)))
            .field("outLengthCm", FieldKind::F32, static_cast<u16>(offsetof(CSoftRope, outLengthCm)), 0, true)
            .field("outStrain", FieldKind::F32, static_cast<u16>(offsetof(CSoftRope, outStrain)), 0, true)
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CSoftRope, outState)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CSoftRope)), "CSoftRope");
        g_ropeType = b.typeId();
    }
    return g_volumeType;
}

u32 SoftShapeSystem::volumeType() { return g_volumeType; }
u32 SoftShapeSystem::ropeType() { return g_ropeType; }

CSoftVolume* SoftShapeSystem::attachJelly(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset) {
    if (!g_volumeType || !world.valid(e) || !world.hasComponent(e, scene::kComponentMeshRenderer)) return nullptr;
    if (g_ropeType && world.hasComponent(e, g_ropeType)) return nullptr;
    if (hasCloth(world, e)) return nullptr;
    auto* c = static_cast<CSoftVolume*>(world.addComponent(e, g_volumeType));
    if (!c) return nullptr;
    *c = CSoftVolume{};
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    return c;
}

CSoftRope* SoftShapeSystem::attachRope(scene::World& world, scene::Entity e, const Vec3& start, const Vec3& end, u64 asset, f32 lengthCm,
                                       f32 radiusCm) {
    if (!g_ropeType || !world.valid(e)) return nullptr;
    if (g_volumeType && world.hasComponent(e, g_volumeType)) return nullptr;
    if (hasCloth(world, e)) return nullptr;
    auto* c = static_cast<CSoftRope*>(world.addComponent(e, g_ropeType));
    if (!c) return nullptr;
    *c = CSoftRope{};
    c->asset = asset;
    c->startOffset[0] = start.x; c->startOffset[1] = start.y; c->startOffset[2] = start.z;
    c->endOffset[0] = end.x; c->endOffset[1] = end.y; c->endOffset[2] = end.z;
    c->lengthCm = lengthCm;
    c->radiusCm = radiusCm;
    return c;
}

// ---- lifetime --------------------------------------------------------------------------------------------

SoftShapeSystem::SoftShapeSystem(SoftBodySystem* host) : host_(host) {}

SoftShapeSystem::~SoftShapeSystem() {
    // Nothing to release here: a singleton outlives the host's statics in an unspecified order, and a caller-owned instance (a
    // test's) leaves the host through uninstall().
}

SoftShapeSystem& softShapeSystem() {
    static SoftShapeSystem s;
    return s;
}

void SoftShapeSystem::install(scene::World& world) {
    world_ = &world;
    if (!g_volumeType || !g_ropeType) registerComponents(world);
    if (installed_) return;
    host().addViewSource(&SoftShapeSystem::viewThunk, this);
    // The rope tube's placeholder: the draw side asks the host for the mesh behind an all-generated view's id, and only needs
    // it to exist (every vertex, normal and UV comes from the view).
    fmt::OcMeshData m;
    m.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    m.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    m.uvs = {0, 0, 1, 0, 0, 1};
    m.indices = {0, 1, 2};
    host().registerSyntheticMesh(shapeRopeMeshId(), m);
    installed_ = true;
}

void SoftShapeSystem::ensureInstalled(scene::World& world) {
    if (!installed_ || world_ != &world) install(world);
}

void SoftShapeSystem::uninstall() {
    clear();
    if (installed_) host().removeViewSource(&SoftShapeSystem::viewThunk, this);
    installed_ = false;
    world_ = nullptr;
}

void SoftShapeSystem::clear() {
    for (auto& [e, v] : volumes_) destroyVolume(v);
    for (auto& [e, r] : ropes_) destroyRope(r);
    volumes_.clear();
    ropes_.clear();
    seeds_.clear();
    cuts_.clear();
    sensorEvents_.clear();
    stats_ = SoftShapeStats{};
    if (installed_) host().refreshViewSources();
}

void SoftShapeSystem::dropAll(scene::World&) {
    // No physics world: whatever was built against a world that is gone has no bodies. Forget them, keep the entities.
    for (auto& [e, v] : volumes_) v.body = 0;
    for (auto& [e, r] : ropes_) r.body = 0;
    volumes_.clear();
    ropes_.clear();
    cuts_.clear();
    sensorEvents_.clear();
}

// ---- tick ------------------------------------------------------------------------------------------------

void SoftShapeSystem::tick(scene::World& world, f32 dt) {
    ensureInstalled(world);
    buildsThisTick_ = 0;
    if (!aver_phys_ready()) {
        dropAll(world);
        stats_ = SoftShapeStats{};
        host().refreshViewSources();
        return;
    }
    for (auto& [e, v] : volumes_) v.needsPack = false;
    for (auto& [e, r] : ropes_) r.needsPack = false;

    processCuts(world);
    sync(world);
    for (auto& [e, v] : volumes_) updateVolume(world, v);
    for (auto& [e, r] : ropes_) updateRope(world, r, dt);
    drainSensors(world);

    SoftShapeStats s;
    s.volumes = static_cast<u32>(volumes_.size());
    s.ropes = static_cast<u32>(ropes_.size());
    for (const auto& [e, v] : volumes_) {
        s.nodes += static_cast<u32>(v.nodeWorld.size());
        s.failed += v.failed ? 1u : 0u;
        s.frozen += v.frozen ? 1u : 0u;
        s.awake += v.awake ? 1u : 0u;
    }
    for (const auto& [e, r] : ropes_) {
        s.nodes += r.nodes;
        s.failed += r.failed ? 1u : 0u;
        s.frozen += r.frozen ? 1u : 0u;
        if (r.body && !r.frozen && aver_phys_sb_is_active(r.body)) ++s.awake;
    }
    s.buildsLastTick = buildsThisTick_;
    stats_ = s;
    host().refreshViewSources();
}

void SoftShapeSystem::sync(scene::World& world) {
    // Entries whose entity, component or build inputs are gone or changed.
    for (auto it = volumes_.begin(); it != volumes_.end();) {
        const scene::Entity e = it->first;
        VolumeEntry& v = it->second;
        const auto* cv = g_volumeType ? world.component<CSoftVolume>(e, g_volumeType) : nullptr;
        const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        bool gone = !world.valid(e) || world.destroyPending(e) || !cv || !mr;
        if (!gone) {
            VolumeSpec cur;
            cur.meshId = mr->mesh; cur.assetId = cv->asset; cur.preset = cv->preset; cur.flags = cv->flags;
            cur.stiffness = or1(cv->stiffnessScale); cur.cell = cv->cellCm; cur.gravity = or1(cv->gravityScale); cur.damping = or1(cv->dampingScale);
            gone = !(cur == v.spec);
        }
        if (gone) {
            destroyVolume(v);
            it = volumes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = ropes_.begin(); it != ropes_.end();) {
        const scene::Entity e = it->first;
        RopeEntry& r = it->second;
        const auto* cr = g_ropeType ? world.component<CSoftRope>(e, g_ropeType) : nullptr;
        bool gone = !world.valid(e) || world.destroyPending(e) || !cr;
        if (!gone) {
            RopeSpecKey cur;
            cur.assetId = cr->asset; cur.lengthCm = cr->lengthCm; cur.radiusCm = cr->radiusCm; cur.flags = cr->flags & kRopeNoWorld;
            gone = !(cur == r.spec);
        }
        if (gone) {
            destroyRope(r);
            it = ropes_.erase(it);
        } else {
            ++it;
        }
    }

    // New entries, ascending entity id, within the build budget.
    if (g_volumeType)
        if (scene::ComponentPool* pool = world.pool(g_volumeType)) {
            std::vector<scene::Entity> candidates;
            for (usize i = 0; i < pool->size(); ++i) {
                const scene::Entity e = pool->entityAt(i);
                if (world.destroyPending(e) || volumes_.count(e)) continue;
                candidates.push_back(e);
            }
            std::sort(candidates.begin(), candidates.end());
            for (const scene::Entity e : candidates) {
                if (buildsThisTick_ >= opt_.maxBuildsPerFrame) break;
                VolumeEntry v;
                v.entity = e;
                if (buildVolume(world, v) == Built::NotReady) continue;   // no mesh yet: try again next tick
                ++buildsThisTick_;
                volumes_.emplace(e, std::move(v));
            }
        }
    if (g_ropeType)
        if (scene::ComponentPool* pool = world.pool(g_ropeType)) {
            std::vector<scene::Entity> candidates;
            for (usize i = 0; i < pool->size(); ++i) {
                const scene::Entity e = pool->entityAt(i);
                if (world.destroyPending(e) || ropes_.count(e)) continue;
                candidates.push_back(e);
            }
            std::sort(candidates.begin(), candidates.end());
            for (const scene::Entity e : candidates) {
                if (buildsThisTick_ >= opt_.maxBuildsPerFrame) break;
                RopeEntry r;
                r.entity = e;
                if (buildRope(world, r) == Built::NotReady) continue;
                ++buildsThisTick_;
                ropes_.emplace(e, std::move(r));
            }
        }
}

// ---- views -----------------------------------------------------------------------------------------------

void SoftShapeSystem::viewThunk(std::vector<PieceView>& out, void* user) { static_cast<const SoftShapeSystem*>(user)->appendViews(out); }

void SoftShapeSystem::appendViews(std::vector<PieceView>& out) const {
    for (const auto& [e, v] : volumes_) {
        if (v.failed || v.body == 0 || v.renderWorld.empty() || v.indices.empty()) continue;
        PieceView pv;
        pv.entity = e;
        pv.piece = 1;
        pv.epoch = v.epoch;
        pv.vertexCount = static_cast<u32>(v.renderWorld.size());
        pv.world = v.renderWorld.data();
        pv.indices = v.indices.data();
        pv.indexCount = static_cast<u32>(v.indices.size());
        pv.sourceVertex = v.sourceVertex.data();
        pv.meshId = v.spec.meshId;
        pv.centroid = v.centroid;
        pv.useEntityWorld = true;
        pv.frozen = !v.needsPack;
        out.push_back(pv);
    }
    for (const auto& [e, r] : ropes_) {
        if (r.failed || r.body == 0 || r.tubePos.empty() || r.tubeIdx.empty()) continue;
        PieceView pv;
        pv.entity = e;
        pv.piece = 1;
        pv.epoch = r.epoch;
        pv.vertexCount = static_cast<u32>(r.tubePos.size());
        pv.world = r.tubePos.data();
        pv.indices = r.tubeIdx.data();
        pv.indexCount = static_cast<u32>(r.tubeIdx.size());
        pv.sourceVertex = r.sourceVertex.data();
        pv.uv = r.tubeUv.data();
        pv.meshId = shapeRopeMeshId();
        pv.centroid = r.centroid;
        pv.useEntityWorld = true;
        pv.frozen = !r.needsPack;
        out.push_back(pv);
    }
}

// ---- sensors ---------------------------------------------------------------------------------------------

scene::Entity SoftShapeSystem::entityOfAnyBody(scene::World& world, i32 body) const {
    if (body == 0) return scene::kInvalidEntity;
    for (const auto& [e, v] : volumes_)
        if (v.body == body) return e;
    for (const auto& [e, r] : ropes_)
        if (r.body == body) return e;
    const scene::Entity soft = host().entityOfBody(body);
    if (soft != scene::kInvalidEntity) return soft;
    // A rigid body (a sensor volume): the entity whose CRigidBody holds the handle.
    if (scene::ComponentPool* pool = world.pool(scene::kComponentRigidBody))
        for (usize i = 0; i < pool->size(); ++i)
            if (static_cast<const scene::CRigidBody*>(pool->dataAt(i))->body == body) return pool->entityAt(i);
    return scene::kInvalidEntity;
}

void SoftShapeSystem::drainSensors(scene::World& world) {
    const i32 n = aver_phys_sbx_sensor_count();
    for (i32 i = 0; i < n; ++i) {
        i32 soft = 0, sensor = 0, entered = 0;
        if (!aver_phys_sbx_sensor_get(i, &soft, &sensor, &entered)) continue;
        SoftSensorEvent ev;
        ev.soft = entityOfAnyBody(world, soft);
        if (ev.soft == scene::kInvalidEntity) continue;   // a cloth or shape this system does not know
        ev.sensor = entityOfAnyBody(world, sensor);
        ev.entered = entered != 0;
        if (sensorEvents_.size() < 1024) sensorEvents_.push_back(ev);
        host().notifyEntity(ev.soft, ev.entered ? "SoftBodySensorEntered" : "SoftBodySensorExited");
    }
}

u32 SoftShapeSystem::drainSensorEvents(SoftSensorEvent* out, u32 max) {
    const u32 n = std::min<u32>(max, static_cast<u32>(sensorEvents_.size()));
    for (u32 i = 0; i < n; ++i) out[i] = sensorEvents_[i];
    sensorEvents_.erase(sensorEvents_.begin(), sensorEvents_.begin() + n);
    return n;
}

// ---- ops -------------------------------------------------------------------------------------------------

bool SoftShapeSystem::isShape(scene::Entity e) const {
    const auto v = volumes_.find(e);
    if (v != volumes_.end()) return !v->second.failed && v->second.body != 0;
    const auto r = ropes_.find(e);
    return r != ropes_.end() && !r->second.failed && r->second.body != 0;
}

i32 SoftShapeSystem::bodyOf(scene::Entity e) const {
    const auto v = volumes_.find(e);
    if (v != volumes_.end()) return v->second.failed ? 0 : v->second.body;
    const auto r = ropes_.find(e);
    if (r != ropes_.end()) return r->second.failed ? 0 : r->second.body;
    return host().bodyOf(e);
}

bool SoftShapeSystem::setPressure(scene::Entity e, f32 kPa) {
    if (!(kPa >= 0.0f) || !std::isfinite(kPa)) return false;
    if (ropes_.count(e)) return false;   // a rope has no volume
    const auto v = volumes_.find(e);
    if (v != volumes_.end()) return v->second.body != 0 && aver_phys_sbx_set_pressure(v->second.body, kPa) != 0;
    return host().setPressure(e, kPa);
}

bool SoftShapeSystem::volumeOf(scene::Entity e, f32& restCm3, f32& nowCm3) const {
    const i32 body = bodyOf(e);
    if (body == 0 || ropes_.count(e)) return false;
    f32 rest = 0.0f, now = 0.0f;
    if (!aver_phys_sbx_volume(body, &rest, &now)) return false;
    restCm3 = rest;
    nowCm3 = now;
    return true;
}

i32 SoftShapeSystem::grab(scene::Entity e, const Vec3& point, f32 maxDistanceCm, const Vec3& target) {
    const i32 body = bodyOf(e);
    if (body == 0 || !std::isfinite(point.x + point.y + point.z + target.x + target.y + target.z)) return 0;
    const i32 n = aver_phys_sb_vertex_count(body);
    if (n <= 0) return 0;
    std::vector<f32> pos(static_cast<usize>(n) * 3);
    if (aver_phys_sb_state(body, pos.data(), nullptr, n) != n) return 0;
    const auto rope = ropes_.find(e);
    i32 best = -1;
    f32 bestSq = maxDistanceCm > 0.0f ? maxDistanceCm * maxDistanceCm : 1e30f;
    for (i32 i = 0; i < n; ++i) {
        if (rope != ropes_.end()) {
            // The ends a pin holds are not grabbable (the anchor owns them).
            const RopeEntry& r = rope->second;
            if ((i == 0 && r.anchor[0].attached) || (i + 1 == n && r.anchor[1].attached)) continue;
        }
        const Vec3 p{pos[static_cast<usize>(i) * 3], pos[static_cast<usize>(i) * 3 + 1], pos[static_cast<usize>(i) * 3 + 2]};
        const f32 d = distSquared(p, point);
        if (d <= bestSq) { bestSq = d; best = i; }
    }
    if (best < 0) return 0;
    const f32 t[3] = {target.x, target.y, target.z};
    return aver_phys_sbx_attach(body, best, t, 2000.0f) ? best + 1 : 0;
}

bool SoftShapeSystem::moveGrab(scene::Entity e, i32 grab, const Vec3& target) {
    const i32 body = bodyOf(e);
    if (body == 0 || grab < 1) return false;
    const f32 t[3] = {target.x, target.y, target.z};
    return aver_phys_sbx_move_attachment(body, grab - 1, t) != 0;
}

bool SoftShapeSystem::releaseGrab(scene::Entity e, i32 grab) {
    const i32 body = bodyOf(e);
    if (body == 0 || grab < 1) return false;
    return aver_phys_sbx_detach(body, grab - 1, -1.0f) != 0;
}

bool SoftShapeSystem::setBuoyancy(scene::Entity e, f32 fluidDensityKgM3, f32 drag) {
    const i32 body = bodyOf(e);
    return body != 0 && aver_phys_sbx_set_buoyancy(body, fluidDensityKgM3, drag, AVER_PHYS_SBX_BUOY_WORLD_WATER) != 0;
}

bool SoftShapeSystem::submerged(scene::Entity e, f32& fraction, f32& volumeCm3) const {
    const i32 body = bodyOf(e);
    if (body == 0) return false;
    f32 f = 0.0f, v = 0.0f;
    if (!aver_phys_sbx_submerged(body, &f, &v)) return false;
    fraction = f;
    volumeCm3 = v;
    return true;
}

bool SoftShapeSystem::setContact(scene::Entity e, f32 softScale, f32 otherScale) {
    const i32 body = bodyOf(e);
    return body != 0 && aver_phys_sbx_set_contact_policy(body, softScale, otherScale, otherScale) != 0;
}

bool SoftShapeSystem::setPairMode(scene::Entity e, scene::Entity other, i32 mode) {
    const i32 body = bodyOf(e);
    if (body == 0 || mode < AVER_PHYS_SBX_PAIR_NORMAL || mode > AVER_PHYS_SBX_PAIR_IGNORE) return false;
    i32 otherBody = bodyOf(other);
    if (otherBody == 0 && world_)
        if (const auto* rb = world_->component<scene::CRigidBody>(other, scene::kComponentRigidBody)) otherBody = rb->body;
    if (otherBody == 0) return false;
    if (mode == AVER_PHYS_SBX_PAIR_NORMAL) return aver_phys_sbx_clear_pair_policy(body, otherBody) != 0;
    return aver_phys_sbx_set_pair_policy(body, otherBody, mode, 1.0f, 1.0f, 1.0f) != 0;
}

scene::Entity SoftShapeSystem::createRope(scene::World& world, const Vec3& start, const Vec3& end, f32 lengthCm, f32 radiusCm, u64 asset) {
    if (!g_ropeType) return scene::kInvalidEntity;
    const scene::Entity e = world.create("Rope");
    if (e == scene::kInvalidEntity) return e;
    if (!attachRope(world, e, start, end, asset, lengthCm, radiusCm)) {
        world.destroy(e);
        return scene::kInvalidEntity;
    }
    return e;
}

// ---- overlay queries -------------------------------------------------------------------------------------

bool SoftShapeSystem::ropeNodes(scene::Entity e, const Vec3*& nodes, u32& count) const {
    const auto it = ropes_.find(e);
    if (it == ropes_.end() || it->second.failed || it->second.nodeWorld.empty()) return false;
    nodes = it->second.nodeWorld.data();
    count = static_cast<u32>(it->second.nodeWorld.size());
    return true;
}

bool SoftShapeSystem::volumeLattice(scene::Entity e, const Lattice*& lattice, const Vec3*& nodeWorld) const {
    const auto it = volumes_.find(e);
    if (it == volumes_.end() || it->second.failed || it->second.nodeWorld.empty()) return false;
    lattice = &it->second.lattice;
    nodeWorld = it->second.nodeWorld.data();
    return true;
}

bool SoftShapeSystem::volumeEdgeRest(scene::Entity e, const f32*& rest, u32& count) const {
    const auto it = volumes_.find(e);
    if (it == volumes_.end() || it->second.failed || it->second.edgeRest.empty()) return false;
    rest = it->second.edgeRest.data();
    count = static_cast<u32>(it->second.edgeRest.size());
    return true;
}

bool SoftShapeSystem::ropeRods(scene::Entity e, const Quat*& rotations, u32& count) const {
    const auto it = ropes_.find(e);
    if (it == ropes_.end() || it->second.failed || !it->second.haveRods) return false;
    rotations = it->second.rodRot.data();
    count = static_cast<u32>(it->second.rodRot.size());
    return true;
}

bool SoftShapeSystem::ropeAnchors(scene::Entity e, bool& startAttached, bool& endAttached) const {
    const auto it = ropes_.find(e);
    if (it == ropes_.end() || it->second.failed) return false;
    startAttached = it->second.anchor[0].attached;
    endAttached = it->second.anchor[1].attached;
    return true;
}

} // namespace aver::softbody
