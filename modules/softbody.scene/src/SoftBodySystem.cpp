// The system: components, entry lifecycle, readback, publishing. The tick order is the determinism contract:
// everything loops ascending entity id, then piece id. Damage in SoftBodyDamage.cpp, the tear pipeline in
// SoftBodyTear.cpp, fields in SoftBodyFields.cpp, thick breaks / baking / cadence in SoftBodyCaps.cpp, break effects in
// SoftBodyFx.cpp, saved damage in SoftBodySave.cpp, proxy cages in SoftBodyProxy.cpp, cloth skinning/LOD/freeze in
// SoftBodyCloth.cpp, body construction in SoftBodyBuild.cpp.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/platform/Image.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::softbody {
namespace {

u32 g_clothType = 0;
u32 g_destructType = 0;
u32 g_fieldType = 0;

f32 or1(f32 v) { return v > 0.0f ? v : 1.0f; }

f32 uniformScaleOf(const Mat4& m) { return Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.size(); }

const char* notifyName(SoftEventKind k) {
    switch (k) {
        case SoftEventKind::Impact:   return "SoftBodyImpact";
        case SoftEventKind::Torn:     return "SoftBodyTorn";
        case SoftEventKind::Detached: return "SoftBodyDetached";
        case SoftEventKind::Dust:     return "SoftBodyDust";
        case SoftEventKind::Repaired: return "SoftBodyRepaired";
        default:                      return nullptr;
    }
}

// Mask textures are decoded once per path and sampled nearest.
struct MaskCache {
    struct Tex { ImageData img; bool ok = false; };
    std::unordered_map<std::string, Tex> tex;
};

} // namespace

// ---- components ------------------------------------------------------------------------------------------

u32 SoftBodySystem::registerComponents(scene::World& world) {
    using scene::FieldKind;
    {
        auto b = world.registerComponent<CCloth>("CCloth");
        b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CCloth, asset)))
            .field("preset", FieldKind::I32, static_cast<u16>(offsetof(CCloth, preset)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CCloth, flags)))
            .field("windScale", FieldKind::F32, static_cast<u16>(offsetof(CCloth, windScale)))
            .field("stiffnessScale", FieldKind::F32, static_cast<u16>(offsetof(CCloth, stiffnessScale)))
            .field("reachScale", FieldKind::F32, static_cast<u16>(offsetof(CCloth, reachScale)))
            .field("freezeDistanceCm", FieldKind::F32, static_cast<u16>(offsetof(CCloth, freezeDistanceCm)))
            .field("outMaxStrain", FieldKind::F32, static_cast<u16>(offsetof(CCloth, outMaxStrain)), 0, true)
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CCloth, outState)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CCloth)), "CCloth");
        g_clothType = b.typeId();
    }
    {
        auto b = world.registerComponent<CDestructible>("CDestructible");
        b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CDestructible, asset)))
            .field("preset", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, preset)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, flags)))
            .field("toughnessScale", FieldKind::F32, static_cast<u16>(offsetof(CDestructible, toughnessScale)))
            .field("impactScale", FieldKind::F32, static_cast<u16>(offsetof(CDestructible, impactScale)))
            .field("debrisLifetimeS", FieldKind::F32, static_cast<u16>(offsetof(CDestructible, debrisLifetimeS)))
            .field("maxPieces", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, maxPieces)))
            .field("outDamage", FieldKind::F32, static_cast<u16>(offsetof(CDestructible, outDamage)), 0, true)
            .field("outBroken", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, outBroken)), 0, true)
            .field("outPieces", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, outPieces)), 0, true)
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CDestructible, outState)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CDestructible)), "CDestructible");
        g_destructType = b.typeId();
    }
    {
        auto b = world.registerComponent<CSoftField>("CSoftField");
        b.field("kind", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, kind)))
            .field("shape", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, shape)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, flags)))
            .field("targets", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, targets)))
            .field("extentX", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, extentX)))
            .field("extentY", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, extentY)))
            .field("extentZ", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, extentZ)))
            .field("strength", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, strength)))
            .field("energyJPerS", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, energyJPerS)))
            .field("falloff", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, falloff)))
            .field("dirX", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, dirX)))
            .field("dirY", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, dirY)))
            .field("dirZ", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, dirZ)))
            .field("turbulence", FieldKind::F32, static_cast<u16>(offsetof(CSoftField, turbulence)))
            .field("outTouched", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, outTouched)), 0, true)
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CSoftField, outState)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CSoftField)), "CSoftField");
        g_fieldType = b.typeId();
    }
    return g_clothType;
}

u32 SoftBodySystem::clothType() { return g_clothType; }
u32 SoftBodySystem::destructibleType() { return g_destructType; }
u32 SoftBodySystem::softFieldType() { return g_fieldType; }

CCloth* SoftBodySystem::attachCloth(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset) {
    if (!g_clothType || !world.valid(e) || !world.hasComponent(e, scene::kComponentMeshRenderer)) return nullptr;
    if (world.hasComponent(e, g_destructType)) return nullptr;
    auto* c = static_cast<CCloth*>(world.addComponent(e, g_clothType));
    if (!c) return nullptr;
    *c = CCloth{};
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    world.addComponent(e, scene::kComponentSoftBody);
    return c;
}

CDestructible* SoftBodySystem::attachDestructible(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset) {
    if (!g_destructType || !world.valid(e) || !world.hasComponent(e, scene::kComponentMeshRenderer)) return nullptr;
    if (world.hasComponent(e, g_clothType)) return nullptr;
    auto* c = static_cast<CDestructible*>(world.addComponent(e, g_destructType));
    if (!c) return nullptr;
    *c = CDestructible{};
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    world.addComponent(e, scene::kComponentSoftBody);
    return c;
}

CSoftField* SoftBodySystem::attachSoftField(scene::World& world, scene::Entity e) {
    if (!g_fieldType || !world.valid(e)) return nullptr;
    auto* c = static_cast<CSoftField*>(world.addComponent(e, g_fieldType));
    if (!c) return nullptr;
    *c = CSoftField{};
    c->extentX = c->extentY = c->extentZ = 100.0f;
    c->falloff = 1.0f;
    return c;
}

// ---- lifetime and sources --------------------------------------------------------------------------------

SoftBodySystem::SoftBodySystem() = default;
SoftBodySystem::~SoftBodySystem() = default;

SoftBodySystem& softBodySystem() {
    static SoftBodySystem s;
    return s;
}

void SoftBodySystem::install(scene::World& world) {
    world_ = &world;
    if (!g_clothType || !g_fieldType) registerComponents(world);
    installed_ = true;
}

void SoftBodySystem::uninstall() {
    if (world_) clear();
    world_ = nullptr;
    installed_ = false;
}

void SoftBodySystem::setPathResolver(PathFn fn, void* user) {
    pathFn_ = fn;
    pathUser_ = user;
}

const fmt::OcMeshData* SoftBodySystem::meshData(u64 id) {
    if (!id) return nullptr;
    if (meshFn_) return meshFn_(id, meshUser_);
    if (!pathFn_) return nullptr;
    const auto it = meshCache_.find(id);
    if (it != meshCache_.end()) return it->second.get();
    const std::string path = pathFn_(id, pathUser_);
    if (path.empty()) return nullptr;   // not resolvable yet: try again later
    auto md = std::make_unique<fmt::OcMeshData>();
    std::string why;
    if (!fmt::loadOcMesh(path, *md, &why)) {
        AVER_WARN("[SoftBody] mesh {} would not load ({})", id, why);
        meshCache_[id] = nullptr;
        return nullptr;
    }
    return (meshCache_[id] = std::move(md)).get();
}

const SoftAsset* SoftBodySystem::assetFor(u64 id) {
    if (!id) return nullptr;
    if (assetFn_) return assetFn_(id, assetUser_);
    if (!pathFn_) return nullptr;
    const auto it = assetCache_.find(id);
    if (it != assetCache_.end()) return it->second.get();
    const std::string path = pathFn_(id, pathUser_);
    if (path.empty()) return nullptr;
    auto a = std::make_unique<SoftAsset>();
    std::string why;
    if (!loadSoftAsset(path, *a, &why)) {
        AVER_WARN("[SoftBody] asset {} would not load ({})", id, why);
        assetCache_[id] = nullptr;
        return nullptr;
    }
    return (assetCache_[id] = std::move(a)).get();
}

SoftBodySystem::Entry* SoftBodySystem::find(scene::Entity e) {
    const auto it = entries_.find(e);
    return it == entries_.end() ? nullptr : &it->second;
}

const SoftBodySystem::Entry* SoftBodySystem::find(scene::Entity e) const {
    const auto it = entries_.find(e);
    return it == entries_.end() ? nullptr : &it->second;
}

SoftBodySystem::Piece* SoftBodySystem::pieceById(Entry& en, u32 id) {
    for (Piece& p : en.pieces)
        if (p.id == id) return &p;
    return nullptr;
}

void SoftBodySystem::clear() {
    if (world_) {
        for (auto& [e, en] : entries_) destroyEntry(*world_, en);
        for (const auto& [e, life] : spawnedLife_)
            if (world_->valid(e)) world_->destroy(e);
        if (world_->count() > 0) world_->flush();
    }
    entries_.clear();
    inbox_.clear();
    events_.clear();
    views_.clear();
    byBody_.clear();
    proxyUsers_.clear();
    fields_.clear();
    liveHandles_.clear();
    spawnedLife_.clear();
    fxQueue_.clear();
    pendingRestore_.clear();
    proxyWait_.clear();
    liveDebris_ = 0;
    layersInstalled_ = false;
    restoring_ = false;
    meshCache_.clear();
    assetCache_.clear();
    stats_ = Stats{};
}

// ---- layers ----------------------------------------------------------------------------------------------

void SoftBodySystem::installLayers() {
    if (!aver_phys_ready()) { layersInstalled_ = false; return; }
    const i32 cl = opt_.clothLayer, pl = opt_.proxyLayer;
    const i32 probe = (cl == 1 || pl == 1) ? 2 : 1;
    // The collision matrix belongs to the physics world and is reset with it: the probe pair is disabled by
    // this install, so seeing it enabled again means the matrix was reset and must be rebuilt.
    if (layersInstalled_ && aver_phys_layer_collision(cl, probe) == 0) return;
    for (i32 i = 0; i < AVER_PHYS_LAYER_COUNT; ++i) {
        aver_phys_set_layer_collision(cl, i, (i == 0 || i == pl) ? 1 : 0);
        if (i != cl) aver_phys_set_layer_collision(pl, i, 0);
    }
    layersInstalled_ = true;
}

// ---- events ----------------------------------------------------------------------------------------------

void SoftBodySystem::emit(Entry& en, const Piece* p, SoftEventKind kind, u32 beam, const Vec3& point, f32 value) {
    if (restoring_) return;   // a saved tear log is being replayed: it is state, not news
    SoftEvent ev;
    ev.kind = kind;
    ev.entity = en.entity;
    ev.piece = p ? p->id : 0;
    ev.beam = beam;
    ev.point = point;
    ev.value = value;
    ev.seq = nextSeq_++;
    events_.push_back(ev);
    if (const char* name = notifyName(kind); name && notifyFn_) {
        if (kind == SoftEventKind::Detached && p) {
            notifyFn_(p->entity, name, notifyUser_);   // at the new piece ...
            notifyFn_(en.entity, name, notifyUser_);   // ... and at the parent
        } else {
            notifyFn_(en.entity, name, notifyUser_);
        }
    }
}

u32 SoftBodySystem::drainEvents(SoftEvent* out, u32 max) {
    const u32 n = std::min<u32>(max, static_cast<u32>(events_.size()));
    for (u32 i = 0; i < n; ++i) out[i] = events_[i];
    events_.erase(events_.begin(), events_.begin() + n);
    return n;
}

// ---- inbox adders ----------------------------------------------------------------------------------------

bool SoftBodySystem::damage(scene::Entity e, Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind) {
    if (!(radiusCm > 0.0f) || !(energyJ >= 0.0f)) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::Damage;
    it.entity = e; it.point = point; it.dir = dir; it.radius = radiusCm; it.energy = energyJ; it.depositKind = kind;
    inbox_.push_back(it);
    return true;
}

u32 SoftBodySystem::explode(Vec3 centre, f32 radiusCm, f32 impulseCmS, f32 energyJ) {
    if (!(radiusCm > 0.0f)) return 0;
    InboxItem it;
    it.kind = InboxItem::Kind::Explode;
    it.point = centre; it.radius = radiusCm; it.impulse = impulseCmS; it.energy = energyJ;
    inbox_.push_back(it);
    u32 touched = 0;
    for (const auto& [e, en] : entries_) {
        bool in = false;
        for (const Piece& p : en.pieces) {
            if (!p.alive || p.empty) continue;
            Vec3 q{std::clamp(centre.x, p.boundsMin.x, p.boundsMax.x), std::clamp(centre.y, p.boundsMin.y, p.boundsMax.y),
                   std::clamp(centre.z, p.boundsMin.z, p.boundsMax.z)};
            in = in || dist(q, centre) <= radiusCm;
        }
        touched += in ? 1u : 0u;
    }
    return touched;
}

u32 SoftBodySystem::tearAt(scene::Entity e, Vec3 point, f32 radiusCm) {
    if (!(radiusCm > 0.0f) || !find(e)) return 0;
    InboxItem it;
    it.kind = InboxItem::Kind::Tear;
    it.entity = e; it.point = point; it.radius = radiusCm;
    inbox_.push_back(it);
    return 1;
}

bool SoftBodySystem::repair(scene::Entity e) {
    if (!find(e)) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::Repair;
    it.entity = e;
    inbox_.push_back(it);
    return true;
}

u32 SoftBodySystem::repairAll() {
    u32 n = 0;
    for (const auto& [e, en] : entries_) {
        InboxItem it;
        it.kind = InboxItem::Kind::Repair;
        it.entity = e;
        inbox_.push_back(it);
        ++n;
    }
    return n;
}

bool SoftBodySystem::release(scene::Entity e) {
    const Entry* en = find(e);
    if (!en || en->destructible) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::Release;
    it.entity = e;
    inbox_.push_back(it);
    return true;
}

bool SoftBodySystem::setAsleep(scene::Entity e, bool asleep) {
    if (!find(e)) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::Asleep;
    it.entity = e;
    it.flag = asleep;
    inbox_.push_back(it);
    return true;
}

bool SoftBodySystem::clearDebris(scene::Entity e) {
    if (!find(e)) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::ClearDebris;
    it.entity = e;
    inbox_.push_back(it);
    return true;
}

bool SoftBodySystem::setPreset(scene::Entity e, u32 preset) {
    if (preset >= kSoftPresetCount) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::Preset;
    it.entity = e; it.preset = preset;
    inbox_.push_back(it);
    return true;
}

void SoftBodySystem::setWind(Vec3 v, f32 gust, f32 gustHz, f32 turbulence) {
    InboxItem it;
    it.kind = InboxItem::Kind::Wind;
    it.dir = v; it.a = gust; it.b = gustHz; it.c = turbulence;
    inbox_.push_back(it);
}

void SoftBodySystem::setBudget(const Budget& b) {
    InboxItem it;
    it.kind = InboxItem::Kind::Budget;
    it.budget = b;
    inbox_.push_back(it);
}

// ---- entries ---------------------------------------------------------------------------------------------

void SoftBodySystem::syncEntries(scene::World& world) {
    // Drop entries whose entity, component or build inputs are gone or changed.
    std::vector<scene::Entity> dead;
    for (auto& [e, en] : entries_) {
        const auto* sb = world.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
        const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        bool gone = !world.valid(e) || world.destroyPending(e) || !sb || (sb->flags & (scene::kSoftBodyDisabled | scene::kSoftBodyPiece)) || !mr;
        if (!gone) {
            Spec cur;
            cur.meshId = mr->mesh;
            const auto* cl = g_clothType ? world.component<CCloth>(e, g_clothType) : nullptr;
            const auto* de = g_destructType ? world.component<CDestructible>(e, g_destructType) : nullptr;
            if (cl && !de) {
                cur.assetId = cl->asset; cur.preset = cl->preset; cur.flags = cl->flags & (kClothNoWorld | kClothNoCharacterProxy | kClothNoWind | kClothTearable);
                cur.scaleA = or1(cl->windScale); cur.scaleB = or1(cl->stiffnessScale); cur.scaleC = or1(cl->reachScale);
            } else if (de && !cl) {
                cur.assetId = de->asset; cur.preset = de->preset;
                cur.scaleA = or1(de->toughnessScale); cur.scaleB = or1(de->impactScale);
            } else if (cl && de) {
                cur.preset = 0xFFFFu;
            } else {
                cur.preset = 0xFFFEu; cur.scaleA = sb->maxDistanceCm; cur.scaleB = sb->compliance;
            }
            if (!(cur == en.spec)) gone = true;
        }
        if (gone) dead.push_back(e);
    }
    for (const scene::Entity e : dead) {
        const auto it = entries_.find(e);
        destroyEntry(world, it->second);
        entries_.erase(it);
    }

    // New entries, ascending entity id, within the build budget.
    scene::ComponentPool* pool = world.pool(scene::kComponentSoftBody);
    if (!pool) return;
    std::vector<scene::Entity> candidates;
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        const auto* sb = static_cast<const scene::CSoftBody*>(pool->dataAt(i));
        if (sb->flags & (scene::kSoftBodyDisabled | scene::kSoftBodyPiece)) continue;
        if (world.destroyPending(e) || entries_.count(e)) continue;
        candidates.push_back(e);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const scene::Entity e : candidates) {
        if (buildsThisTick_ >= opt_.maxRebuildsPerFrame) break;
        Entry en;
        en.entity = e;
        if (!buildEntry(world, en)) continue;   // not ready (no mesh yet): try again next tick
        ++buildsThisTick_;
        ++rebuildsThisTick_;
        entries_.emplace(e, std::move(en));
    }
}

bool SoftBodySystem::buildEntry(scene::World& world, Entry& en) {
    const scene::Entity e = en.entity;
    const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    auto* sb = world.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
    if (!mr || !sb || mr->mesh == 0) return false;
    const auto* cl = g_clothType ? world.component<CCloth>(e, g_clothType) : nullptr;
    const auto* de = g_destructType ? world.component<CDestructible>(e, g_destructType) : nullptr;

    en.spec = Spec{};
    en.spec.meshId = mr->mesh;
    en.destructible = de && !cl;
    en.legacy = !cl && !de;
    if (cl && de) {
        en.spec.preset = 0xFFFFu;
        en.conflict = true;
        en.failed = true;
        AVER_WARN("[SoftBody] entity {} has both CCloth and CDestructible; one body per entity root, so it is not simulated", e);
        return true;
    }
    if (cl) {
        en.spec.assetId = cl->asset; en.spec.preset = cl->preset; en.spec.flags = cl->flags & (kClothNoWorld | kClothNoCharacterProxy | kClothNoWind | kClothTearable);
        en.spec.scaleA = or1(cl->windScale); en.spec.scaleB = or1(cl->stiffnessScale); en.spec.scaleC = or1(cl->reachScale);
    } else if (de) {
        en.spec.assetId = de->asset; en.spec.preset = de->preset;
        en.spec.scaleA = or1(de->toughnessScale); en.spec.scaleB = or1(de->impactScale);
    } else {
        en.spec.preset = 0xFFFEu; en.spec.scaleA = sb->maxDistanceCm; en.spec.scaleB = sb->compliance;
    }

    const fmt::OcMeshData* mesh = meshData(mr->mesh);
    if (!mesh) return false;
    en.mesh = mesh;
    if (mesh->positions.empty() || mesh->indices.empty()) {
        AVER_WARN("[SoftBody] mesh {} has no triangles to simulate; entity {} keeps its authored mesh", mr->mesh, e);
        en.failed = true;
        return true;
    }

    // The asset: .ocsoft + overrides, or the preset alone.
    const SoftKind kind = en.destructible ? SoftKind::Destructible : SoftKind::Cloth;
    SoftPreset preset = kind == SoftKind::Cloth ? SoftPreset::ClothLight : SoftPreset::Metal;
    if (en.spec.preset < kSoftPresetCount) preset = static_cast<SoftPreset>(en.spec.preset);
    SoftAsset a = makeSoftAsset(kind, preset);
    en.assetStale = false;
    if (en.spec.assetId) {
        if (const SoftAsset* src = assetFor(en.spec.assetId)) a = *src;
        else en.assetStale = true;
    }
    a.kind = kind;
    if (en.legacy) {
        a = makeSoftAsset(SoftKind::Cloth, SoftPreset::ClothLight);
        a.material.compliance = std::max(0.0f, sb->compliance);
        a.cloth.defaultReachCm = scene::softBodyMaxDistanceCm(*sb);
    }
    if (cl) {
        a.material.compliance /= en.spec.scaleB;
        a.material.bendCompliance /= en.spec.scaleB;
        a.cloth.defaultReachCm *= en.spec.scaleC;
        a.cloth.tearable = a.cloth.tearable || (cl->flags & kClothTearable) != 0;
    } else if (de) {
        a.material.toughnessJ *= en.spec.scaleA;
        a.material.yieldStrain *= en.spec.scaleA;
        a.material.breakStrain *= en.spec.scaleA;
    }
    en.asset = a;
    en.windScale = cl ? en.spec.scaleA : 1.0f;

    // The cage. The entity's world matrix is baked into the particles, which lifts the old rotation/scale limit.
    const Mat4 W0 = world.worldMatrix(e);
    en.uniformScale = uniformScaleOf(W0);
    SimMeshInput in;
    in.positions = mesh->positions.data();
    in.uvs = mesh->uvs.empty() ? nullptr : mesh->uvs.data();
    in.indices = mesh->indices.data();
    in.vertexCount = mesh->vertexCount();
    in.indexCount = static_cast<u32>(mesh->indices.size());
    if (mesh->hasSkin()) { in.joints = mesh->joints.data(); in.weights = mesh->weights.data(); }

    SimBuildSpec spec;
    spec.material = a.material;
    spec.weldCm = a.sim.weldCm;
    spec.maxParticles = a.sim.maxParticles;
    spec.entityWorld = W0;
    spec.uniformScale = en.uniformScale;
    spec.paint = a.paint;
    spec.destructible = en.destructible || en.legacy;
    spec.defaultReachCm = a.cloth.defaultReachCm;
    {
        static MaskCache cache;
        PathFn pf = pathFn_;
        void* pu = pathUser_;
        spec.mask = [pf, pu](const std::string& texture, f32 u, f32 v, f32& r, f32& g) {
            auto it = cache.tex.find(texture);
            if (it == cache.tex.end()) {
                MaskCache::Tex t;
                const std::string path = pf ? pf(fnv1a64(texture), pu) : texture;
                t.ok = !path.empty() && decodeImage(path, t.img) && t.img.valid();
                it = cache.tex.emplace(texture, std::move(t)).first;
            }
            if (!it->second.ok) return false;
            const ImageData& img = it->second.img;
            const u32 x = std::min<u32>(img.width - 1, static_cast<u32>(std::clamp(u, 0.0f, 1.0f) * static_cast<f32>(img.width)));
            const u32 y = std::min<u32>(img.height - 1, static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(img.height)));
            const u8* px = &img.pixels[(static_cast<usize>(y) * img.width + x) * 4];
            r = static_cast<f32>(px[0]) / 255.0f;
            g = static_cast<f32>(px[1]) / 255.0f;
            return true;
        };
        if (boneFn_) {
            BoneIndexFn bf = boneFn_;
            void* bu = boneUser_;
            const scene::Entity ent = e;
            const fmt::OcMeshData* md = mesh;
            spec.boneWeight = [bf, bu, ent, md](const std::string& bone, u32 renderVertex) {
                const i32 idx = bf(ent, bone, bu);
                if (idx < 0 || md->weights.empty() || renderVertex >= md->vertexCount()) return 0.0f;
                f32 w = 0.0f;
                for (u32 k = 0; k < 4; ++k)
                    if (static_cast<i32>(md->joints[renderVertex * 4 + k]) == idx) w += md->weights[renderVertex * 4 + k];
                return w;
            };
        }
    }

    // A cooked proxy cage when the asset names one (SoftBodyProxy.cpp), else the authored mesh itself.
    en.proxyMode = false;
    bool built = false;
    if (!a.proxy.mesh.empty()) {
        bool notReady = false;
        if (buildProxy(world, en, in, spec, notReady)) built = true;
        else if (notReady) return false;
    }
    if (!built) {
        if (!buildSimMesh(in, spec, en.sim)) {
            AVER_WARN("[SoftBody] entity {}: {}; it keeps its authored mesh", e, en.sim.error);
            en.overCap = en.sim.error.find("over the limit") != std::string::npos;
            en.failed = true;
            return true;
        }
        en.saveMeshKey = en.sim.meshKey;
    }
    if (en.legacy) {
        for (Particle& q : en.sim.cage.particles) {
            q.pinned = false;
            q.reachCm = mesh->hasSkin() ? a.cloth.defaultReachCm : -1.0f;
            q.invMass = 1.0f / std::max(q.mass, 1e-4f);
        }
    }
    en.invBuildWorld = W0.inverse();
    en.lastWorld = W0;
    en.hasLastWorld = true;
    en.hardSkinNext = true;
    en.totalBeams = static_cast<u32>(en.sim.cage.beams.size());

    // Which backing the thick-break rule uses: an open sheet gets a back skin on every piece (BackSkin::Auto).
    {
        std::vector<u32> vis, weld(en.sim.binding.vertexCount());
        en.sim.binding.visibleIndices(en.sim.cage.triDead, vis);
        for (u32 v = 0; v < weld.size(); ++v)
            weld[v] = en.proxyMode && v < en.renderWeld.size() ? en.renderWeld[v] : en.sim.binding.vertexParticle[v];
        en.openSheet = hasBoundaryEdges(vis.data(), static_cast<u32>(vis.size()), weld.data());
    }
    if (!a.caps.interiorMaterial.empty()) en.interiorToken = aver_scene_material(0, a.caps.interiorMaterial.c_str());

    // Which entity carries the skeleton (this one or an ancestor), and the joint count.
    for (scene::Entity x = e; x != scene::kInvalidEntity && world.valid(x); x = world.parent(x))
        if (world.hasComponent(x, scene::kComponentSkeletalMesh)) { en.skeletonEntity = x; break; }
    en.propPalette = true;
    en.jointCount = 1;
    if (mesh->hasSkin() && en.skeletonEntity != scene::kInvalidEntity && !en.sim.joint4.empty()) {
        u32 count = 0;
        const Mat4* pal = paletteSet_ ? (paletteFn_ ? paletteFn_(en.skeletonEntity, count, paletteUser_) : nullptr)
                                      : anim::animSystem().skinning(en.skeletonEntity, count);
        if (pal && count > 0) { en.propPalette = false; en.jointCount = count; }
    }

    // Saved damage waiting for this entity: used when its keys still match the mesh and the asset.
    if (const auto rit = pendingRestore_.find(e); rit != pendingRestore_.end()) {
        std::unique_ptr<SoftState> st = std::move(rit->second);
        pendingRestore_.erase(rit);
        if (softStateStale(*st, en.saveMeshKey, softAssetKey(en.asset)))
            AVER_WARN("[SoftBody] entity {}: saved damage is for a different mesh or asset; loading intact", e);
        else
            en.restore = std::move(st);
    }

    installLayers();
    if (!initialPiece(world, en)) {
        AVER_WARN("[SoftBody] entity {}: the physics body could not be made; it keeps its authored mesh", e);
        en.pieces.clear();
        en.failed = true;
        return true;
    }
    if (!en.destructible && !en.legacy) setCharacterProxy(world, en, true);
    sb->body = en.pieces.front().body;
    return true;
}

void SoftBodySystem::destroyPiece(scene::World& world, Entry& en, Piece& p) {
    if (p.body) {
        aver_phys_remove_body(p.body);
        byBody_.erase(p.body);
        p.body = 0;
    }
    if (p.capEntity != scene::kInvalidEntity) {
        if (world.valid(p.capEntity)) world.destroy(p.capEntity);
        p.capEntity = scene::kInvalidEntity;
    }
    if (p.debris && (p.alive || p.pendingKill) && p.entity != scene::kInvalidEntity && world.valid(p.entity)) {
        world.destroy(p.entity);
        if (liveDebris_ > 0) --liveDebris_;
    }
    p.alive = false;
    p.pendingKill = false;
    p.needsBody = false;
    p.particles.clear(); p.beams.clear(); p.triangles.clear(); p.renderVerts.clear();
    p.drawIndices.clear(); p.sourceVertex.clear(); p.drawUv.clear(); p.drawWeld.clear(); p.drawEvalTri.clear(); p.drawWorld.clear();
    p.caps = CapGeometry{};
    p.capWorld.clear(); p.capIndices.clear(); p.capSource.clear(); p.capUv.clear();
    (void)en;
}

void SoftBodySystem::destroyEntry(scene::World& world, Entry& en) {
    setCharacterProxy(world, en, false);
    for (Piece& p : en.pieces)
        if (p.alive || p.pendingKill) destroyPiece(world, en, p);
    en.pieces.clear();
    if (world.valid(en.entity)) {
        if (auto* sb = world.component<scene::CSoftBody>(en.entity, scene::kComponentSoftBody)) sb->body = 0;
        if (en.hidden)
            if (auto* mr = world.component<scene::CMeshRenderer>(en.entity, scene::kComponentMeshRenderer)) mr->flags |= scene::kMeshRendererVisible;
    }
    en.hidden = false;
}

// ---- per-tick work ---------------------------------------------------------------------------------------

// The front draw vertices (the authored triangles of the piece) at the particles' current positions, plus the
// piece's bounds and centroid. Caps are refreshed separately from these (refreshCaps).
void SoftBodySystem::frontPositions(Entry& en, Piece& p) {
    const RenderBinding& rb = en.sim.binding;
    const Cage& c = en.sim.cage;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    if (en.proxyMode) {
        proxyPositions(en, p);
    } else {
        for (usize i = 0; i < p.renderVerts.size(); ++i) {
            const u32 particle = rb.vertexParticle[p.renderVerts[i]];
            p.drawWorld[i] = particle < c.particles.size() ? c.particles[particle].pos : Vec3{0, 0, 0};
        }
    }
    for (usize i = 0; i < p.renderVerts.size(); ++i) {
        const Vec3& pos = p.drawWorld[i];
        lo = {std::min(lo.x, pos.x), std::min(lo.y, pos.y), std::min(lo.z, pos.z)};
        hi = {std::max(hi.x, pos.x), std::max(hi.y, pos.y), std::max(hi.z, pos.z)};
    }
    if (p.renderVerts.empty()) { lo = hi = Vec3{0, 0, 0}; }
    p.boundsMin = lo;
    p.boundsMax = hi;
    Vec3 sum{0, 0, 0};
    for (const u32 pid : p.particles) sum += c.particles[pid].pos;
    p.centroid = p.particles.empty() ? Vec3{0, 0, 0} : sum / static_cast<f32>(p.particles.size());
}

void SoftBodySystem::refreshDraw(Entry& en, Piece& p) {
    if (p.drawWorld.size() < p.renderVerts.size()) p.drawWorld.resize(p.renderVerts.size(), Vec3{0, 0, 0});
    frontPositions(en, p);
    refreshCaps(en, p);
    p.needsPack = true;
}

void SoftBodySystem::readback(Entry& en) {
    Cage& c = en.sim.cage;
    for (Piece& p : en.pieces) {
        if (!p.alive || p.body == 0 || p.frozen) continue;
        const bool active = aver_phys_sb_is_active(p.body) != 0;
        if (!active && !p.needsRead) continue;
        const i32 n = static_cast<i32>(p.particles.size());
        p.posBuf.resize(static_cast<usize>(n) * 3);
        if (aver_phys_sb_state(p.body, p.posBuf.data(), nullptr, n) != n) continue;
        for (i32 i = 0; i < n; ++i)
            c.particles[p.particles[static_cast<usize>(i)]].pos = Vec3{p.posBuf[static_cast<usize>(i) * 3], p.posBuf[static_cast<usize>(i) * 3 + 1], p.posBuf[static_cast<usize>(i) * 3 + 2]};
        p.needsRead = false;
        refreshDraw(en, p);
    }
}

void SoftBodySystem::refreshOut(scene::World& world, Entry& en) {
    u32 alive = 0, awake = 0, frozen = 0;
    for (const Piece& p : en.pieces) {
        if (!p.alive || p.empty) continue;
        ++alive;
        if (p.frozen) ++frozen;
        else if (p.body && aver_phys_sb_is_active(p.body)) ++awake;
    }
    u32 state = (awake ? kStateAwake : 0u) | (frozen && !awake ? kStateFrozen : 0u) | (en.released ? kStateReleased : 0u) |
                (en.assetStale ? kStateAssetStale : 0u) | (en.failed && en.overCap ? kStateOverCap : 0u) | (en.proxyMode ? kStateProxy : 0u) |
                (alive << kStatePieceShift);
    if (auto* cl = g_clothType ? world.component<CCloth>(en.entity, g_clothType) : nullptr) {
        cl->outMaxStrain = en.maxStrain;
        cl->outState = state;
    }
    if (auto* de = g_destructType ? world.component<CDestructible>(en.entity, g_destructType) : nullptr) {
        de->outDamage = en.totalBeams ? static_cast<f32>(en.brokenBeams) / static_cast<f32>(en.totalBeams) : 0.0f;
        de->outBroken = en.brokenBeams;
        de->outPieces = alive;
        de->outState = state;
    }
}

void SoftBodySystem::publish(scene::World& world) {
    views_.clear();
    Stats s;
    s.entries = static_cast<u32>(entries_.size());
    for (auto& [e, en] : entries_) {
        refreshOut(world, en);
        for (Piece& p : en.pieces) {
            if (!p.alive) continue;
            ++s.pieces;
            s.debris += p.debris ? 1u : 0u;
            s.bakedPieces += p.baked ? 1u : 0u;
            s.heldPieces += p.fieldHeld ? 1u : 0u;
            if (p.frozen) ++s.frozenPieces;
            else if (!p.baked) { s.activeParticles += static_cast<u32>(p.particles.size()); if (p.body && aver_phys_sb_is_active(p.body)) ++s.awakePieces; }
            if (p.empty) continue;

            bool culled = false;
            publishVisibility(world, en, p, culled);

            // Draw cadence: a far piece repacks every 2nd or 4th tick, a settled one not at all (baked, asleep, frozen).
            bool frozen = p.frozen || p.baked;
            if (!frozen) {
                const u32 period = p.lodTier == 1 ? 2u : (p.lodTier == 2 ? 4u : 1u);
                if (p.shrinking) p.needsPack = true;
                const bool due = period <= 1 || p.shrinking || ((tickCount_ + p.id) % period) == 0;
                frozen = !p.needsPack || !due;
                if (!frozen) p.needsPack = false;
            }

            PieceView v;
            v.entity = p.entity;
            v.piece = p.id;
            v.epoch = p.epoch;
            v.vertexCount = static_cast<u32>(p.drawWorld.size());
            v.world = p.drawWorld.data();
            v.indices = p.drawIndices.data();
            v.indexCount = static_cast<u32>(p.drawIndices.size());
            v.sourceVertex = p.sourceVertex.data();
            v.uv = p.drawUv.empty() ? nullptr : p.drawUv.data();
            v.meshId = en.spec.meshId;
            v.origin = p.origin;
            v.centroid = p.centroid;
            v.useEntityWorld = !p.debris;
            v.scale01 = !p.baked && p.shrinking && opt_.despawnShrinkS > 0.0f ? std::max(0.0f, 1.0f - p.shrinkT / opt_.despawnShrinkS) : 1.0f;
            v.frozen = frozen;
            v.revealOnReady = p.debris && !culled;
            v.bake = p.baked;
            views_.push_back(v);

            if (p.capEntity != scene::kInvalidEntity && !p.capIndices.empty() && !p.capsHidden) {
                PieceView cv = v;
                cv.entity = p.capEntity;
                cv.vertexCount = static_cast<u32>(p.capWorld.size());
                cv.world = p.capWorld.data();
                cv.indices = p.capIndices.data();
                cv.indexCount = static_cast<u32>(p.capIndices.size());
                cv.sourceVertex = p.capSource.data();
                cv.uv = p.capUv.data();
                cv.layer = 1;
                cv.revealOnReady = !culled;
                views_.push_back(cv);
            }
        }
    }
    s.rebuildsLastTick = rebuildsThisTick_;
    s.depositsLastTick = depositsThisTick_;
    s.eventsPending = static_cast<u32>(events_.size());
    s.fieldsApplied = stats_.fieldsApplied;
    s.transientFields = static_cast<u32>(fields_.size());
    s.fxSent = stats_.fxSent;
    s.fxDropped = stats_.fxDropped;
    stats_ = s;
}

const PieceView* SoftBodySystem::pieces(u32& count) const {
    count = static_cast<u32>(views_.size());
    return views_.data();
}

SoftBodyState SoftBodySystem::state(scene::Entity e) const {
    SoftBodyState st;
    const Entry* en = find(e);
    if (!en) return st;
    st.found = true;
    st.maxStrain = en->maxStrain;
    st.brokenEdges = en->brokenBeams;
    st.damage01 = en->totalBeams ? static_cast<f32>(en->brokenBeams) / static_cast<f32>(en->totalBeams) : 0.0f;
    for (const Piece& p : en->pieces) {
        if (!p.alive || p.empty) continue;
        ++st.pieces;
        if (p.frozen) st.frozen = true;
        else if (p.body && aver_phys_sb_is_active(p.body)) st.awake = true;
    }
    if (st.awake) st.frozen = false;
    return st;
}

HitInfo SoftBodySystem::lastHit(scene::Entity e) const {
    const Entry* en = find(e);
    if (!en) return HitInfo{};
    HitInfo h = en->hit;
    h.maxStrain = en->maxStrain;
    for (const Beam& b : en->sim.cage.beams) h.maxSetCm = std::max(h.maxSetCm, b.plastic);
    for (const Piece& p : en->pieces) {
        if (!p.alive || p.empty) continue;
        ++h.pieces;
        h.baked += p.baked ? 1u : 0u;
    }
    return h;
}

DentInfo SoftBodySystem::dents(scene::Entity e) const {
    DentInfo d;
    const Entry* en = find(e);
    if (!en || en->failed) return d;
    d.found = true;
    for (const Beam& b : en->sim.cage.beams) {
        if (b.broken || !(b.plastic > 0.0f)) continue;
        ++d.dentedEdges;
        d.maxSetCm = std::max(d.maxSetCm, b.plastic);
    }
    for (const Piece& p : en->pieces) d.debrisPieces += p.alive && p.debris ? 1u : 0u;
    return d;
}

std::vector<SoftBodySystem::FieldView> SoftBodySystem::transientFields() const {
    std::vector<FieldView> out;
    for (const auto& [h, t] : fields_) out.push_back(FieldView{h, t.spec, t.remainingS});
    return out;
}

u32 SoftBodySystem::pieceCount(scene::Entity e) const {
    const Entry* en = find(e);
    u32 n = 0;
    if (en)
        for (const Piece& p : en->pieces) n += p.alive && !p.empty ? 1u : 0u;
    return n;
}

const Cage* SoftBodySystem::cageOf(scene::Entity e) const {
    const Entry* en = find(e);
    return en && !en->failed ? &en->sim.cage : nullptr;
}

void SoftBodySystem::tick(scene::World& world, f32 dt) {
    rebuildsThisTick_ = 0;
    buildsThisTick_ = 0;
    depositsThisTick_ = 0;
    fxDroppedThisTick_ = 0;
    stats_.fieldsApplied = 0;
    stats_.fxSent = 0;
    stats_.fxDropped = 0;
    ++tickCount_;
    if (!aver_phys_ready()) {
        // No physics: nothing simulates. Drop whatever was built against a world that is gone.
        if (!entries_.empty()) {
            for (auto& [e, en] : entries_) {
                for (Piece& p : en.pieces) { p.body = 0; }
                destroyEntry(world, en);
            }
            entries_.clear();
            byBody_.clear();
            layersInstalled_ = false;
        }
        fxQueue_.clear();
        publish(world);
        return;
    }
    installLayers();
    const u32 stepsThisTick = std::max(1u, static_cast<u32>(std::lround(std::max(dt, 0.0f) / std::max(aver_phys_fixed_step(), 1e-4f))));
    stepClock_ += stepsThisTick;
    const f32 stepSeconds = static_cast<f32>(stepsThisTick) * aver_phys_fixed_step();
    stepSeconds_ = stepSeconds;

    syncEntries(world);
    for (auto& [e, en] : entries_)
        if (!en.failed) {
            readback(en);
            en.stepAccum += stepsThisTick;
            for (f32& cd : en.fxCooldown) cd = std::max(0.0f, cd - dt);
        }

    ++fieldTick_;
    applyInbox(world);
    fieldStep(world, stepSeconds);
    pollImpacts(world);

    for (auto& [e, en] : entries_) {
        if (en.failed) continue;
        plasticStep(world, en, dt);
        tearPipeline(world, en);
    }
    for (auto& [e, en] : entries_)
        if (!en.failed) clothStep(world, en, dt);
    applyLodAndFreeze(world, dt);
    debrisLifecycle(world, dt);
    evictDebris(world);
    capLod(world);
    for (auto& [e, en] : entries_)
        for (Piece& p : en.pieces)
            if (p.alive) p.hotFor = std::max(0.0f, p.hotFor - dt);
    if (needFlush_) { world.flush(); needFlush_ = false; }
    flushFx();
    publish(world);
}

} // namespace aver::softbody
