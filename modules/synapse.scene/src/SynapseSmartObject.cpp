#include "aver/synapse/SynapseSmartObject.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/synapse/AiPaths.hpp"
#include "aver/synapse/SynapseAi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::synapse {
namespace {

constexpr usize kEndedKept = 64;
constexpr f32 kRadToDeg = 57.29577951308232f;

bool live(scene::World& w, scene::Entity e) { return e != scene::kInvalidEntity && w.valid(e) && !w.destroyPending(e); }

// v * M with the row-vector convention the world matrices use.
Vec3 xform(const Mat4& m, const Vec3& v) {
    return Vec3{v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0],
                v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]};
}

void putValue(Blackboard& b, const char* name, const BbValue& v) {
    if (!b.has(name)) {
        BbKeyDef d;
        d.name = name;
        d.type = v.type;
        d.defaultValue = bbDefault(v.type);
        b.defineKey(d);
    }
    b.set(name, v);
}

const char* const kMirrorKeys[] = {"SmartObject", "SmartObjectSlot", "SmartObjectHandle", "SmartObjectPos", "SmartObjectEntry", "SmartObjectYaw"};

} // namespace

u32 SmartObjectSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSmartObject>("CSmartObject");
    b.field("asset", scene::FieldKind::String, static_cast<u16>(offsetof(CSmartObject, asset)))
        .field("enabled", scene::FieldKind::I32, static_cast<u16>(offsetof(CSmartObject, enabled)))
        .field("slotsFree", scene::FieldKind::I32, static_cast<u16>(offsetof(CSmartObject, slotsFree)), 0, /*readOnly*/ true);
    AVER_ASSERTM(b.verify(sizeof(CSmartObject)), "CSmartObject");
    type_ = b.typeId();
    assetField_ = world.fieldId("CSmartObject.asset");
    return type_;
}

CSmartObject* SmartObjectSystem::attach(scene::World& world, scene::Entity e, const std::string& assetPath) {
    if (type_ == 0) return nullptr;
    auto* c = static_cast<CSmartObject*>(world.addComponent(e, type_));
    if (!c) return nullptr;
    *c = CSmartObject{};
    if (!assetPath.empty() && assetField_ != 0) aver_scene_set_str(static_cast<int32_t>(e), static_cast<int32_t>(assetField_), assetPath.c_str());
    return c;
}

bool SmartObjectSystem::setEnabled(scene::World& world, scene::Entity e, bool enabled) {
    auto* c = type_ ? world.component<CSmartObject>(e, type_) : nullptr;
    if (!c) return false;
    c->enabled = enabled ? 1 : 0;
    sync(world);
    return true;
}

const SoDef* SmartObjectSystem::defFor(const std::string& path) {
    if (path.empty()) return nullptr;
    if (const auto it = defs_.find(path); it != defs_.end()) return it->second.get();
    if (failed_.count(path)) return nullptr;
    auto def = std::make_unique<SoDef>();
    std::string why;
    if (!loadSoDef(resolveAiAsset(path), *def, &why)) {
        failed_.insert(path);
        AVER_WARN("[Synapse] could not load smart object '{}': {}", path, why);
        return nullptr;
    }
    def->path = path;
    const SoDef* raw = def.get();
    defs_[path] = std::move(def);
    return raw;
}

void SmartObjectSystem::applyWorld(scene::World& world, scene::Entity e, u32 object) {
    const SoDef* def = reg_.defOf(object);
    if (!def) return;
    const Mat4& m = world.worldMatrix(e);
    const f32 ownerYaw = std::atan2(m.m[0][1], m.m[0][0]) * kRadToDeg;
    for (u32 i = 0; i < def->slots.size(); ++i) {
        const SoSlotDef& s = def->slots[i];
        SoWorldSlot w;
        w.pos = xform(m, s.pos);
        w.entry = xform(m, s.entry);
        w.yawDeg = ownerYaw + s.yawDeg;
        w.entryYawDeg = ownerYaw + s.entryYawDeg;
        reg_.setWorld(object, i, w);
    }
}

void SmartObjectSystem::sync(scene::World& world) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;
    std::set<scene::Entity> seen;
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* c = static_cast<CSmartObject*>(pool->dataAt(i));
        if (!c || !live(world, e)) continue;
        const std::string path = assetField_ ? aver_scene_get_str(static_cast<int32_t>(e), static_cast<int32_t>(assetField_)) : "";
        ObjRec& rec = objs_[e];
        const bool enabled = c->enabled != 0;
        bool changed = false;
        if (path != rec.path || rec.def == nullptr) {
            const SoDef* def = path.empty() ? nullptr : defFor(path);
            rec.path = path;
            if (def) {
                rec.def = def;
                rec.enabled = enabled;
                reg_.setObject(e, def, enabled);
                changed = true;
            }
        } else if (rec.enabled != enabled) {
            rec.enabled = enabled;
            reg_.setObject(e, rec.def, enabled);
        }
        if (!rec.def) continue;
        seen.insert(e);
        world.worldMatrix(e);   // composes if stale, so the revision below is current
        const u32 rev = world.worldRevision(e);
        if (changed || rev != rec.rev) {
            rec.rev = rev;
            applyWorld(world, e, e);
        }
        i32 freeSlots = 0;
        for (u32 s = 0; s < reg_.slotCount(e); ++s) {
            SoSlotInfo info;
            if (reg_.slotInfo(e, s, info) && info.state == SoSlotState::Free) ++freeSlots;
        }
        c->slotsFree = freeSlots;
    }
    std::vector<SoRelease> released;
    for (auto it = objs_.begin(); it != objs_.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        reg_.removeObject(it->first, released);
        it = objs_.erase(it);
    }
    if (!released.empty() && ai_) {
        SynapseAi::CallScope scope(*ai_);
        handleReleases(world, released);
    }
}

i32 SmartObjectSystem::slotCount(scene::World& world, scene::Entity e) {
    sync(world);
    return reg_.hasObject(e) ? static_cast<i32>(reg_.slotCount(e)) : -1;
}

bool SmartObjectSystem::slotInfo(scene::World& world, scene::Entity e, u32 slot, SoSlotInfo& out) {
    sync(world);
    return reg_.slotInfo(e, slot, out);
}

bool SmartObjectSystem::setSlotEnabled(scene::World& world, scene::Entity e, u32 slot, bool enabled) {
    sync(world);
    return reg_.setSlotEnabled(e, slot, enabled);
}

u32 SmartObjectSystem::find(scene::World& world, const SoQuery& q, SoSlotRef* out, f32* dist, u32 cap) {
    sync(world);
    return reg_.find(q, out, dist, cap);
}

u32 SmartObjectSystem::claim(scene::World& world, scene::Entity user, SoSlotRef slot, const TagList& userTags, f32 ttl, SoRefusal* why) {
    sync(world);
    if (!live(world, user)) {
        if (why) *why = SoRefusal::NoUser;
        return 0;
    }
    SynapseAi::CallScope scope(*ai_);
    const u32 handle = reg_.claim(slot, user, userTags, ttl, why);
    if (!handle) return 0;
    rt_[handle].user = user;
    ai_->raise(user, "SmartObjectClaimed");
    ai_->raise(static_cast<scene::Entity>(slot.object), "SmartObjectSlotClaimed");
    mirror(world, user);
    return handle;
}

bool SmartObjectSystem::use(scene::World& world, u32 claim) {
    SoClaimInfo info;
    if (!reg_.claimInfo(claim, info)) return false;
    SynapseAi::CallScope scope(*ai_);
    if (!reg_.use(claim)) return false;
    ai_->raise(static_cast<scene::Entity>(info.user), "SmartObjectUsed");
    ai_->raise(static_cast<scene::Entity>(info.slot.object), "SmartObjectSlotUsed");
    if (info.def && info.def->use == SoUse::Event && !info.def->useArg.empty())
        ai_->raise(static_cast<scene::Entity>(info.user), info.def->useArg.c_str());
    mirror(world, static_cast<scene::Entity>(info.user));
    return true;
}

bool SmartObjectSystem::release(scene::World& world, u32 claim, SoReleaseReason reason) {
    SynapseAi::CallScope scope(*ai_);
    SoRelease rel;
    if (!reg_.release(claim, reason, &rel)) return false;
    std::vector<SoRelease> one{rel};
    handleReleases(world, one);
    return true;
}

bool SmartObjectSystem::endedReason(u32 claim, SoReleaseReason& out) const {
    for (auto it = ended_.rbegin(); it != ended_.rend(); ++it)
        if (it->first == claim) { out = it->second; return true; }
    return false;
}

void SmartObjectSystem::abortBehaviour(u32, ClaimRt& rt) {
    BtRegistry* reg = ai_ ? ai_->behaviorRegistry() : nullptr;
    if (!reg || !rt.started) return;
    if (rt.btTree) abortBt(rt.btTree->tree, *reg, static_cast<i32>(rt.user), rt.bt);
    if (rt.stAsset) abortStateTree(*rt.stAsset, *reg, static_cast<i32>(rt.user), rt.st);
}

void SmartObjectSystem::handleReleases(scene::World& world, std::vector<SoRelease>& released) {
    for (const SoRelease& r : released) {
        if (const auto it = rt_.find(r.claim); it != rt_.end()) {
            abortBehaviour(r.claim, it->second);
            rt_.erase(it);
        }
        ended_.emplace_back(r.claim, r.reason);
        if (ended_.size() > kEndedKept) ended_.erase(ended_.begin());
        const scene::Entity user = static_cast<scene::Entity>(r.user);
        const scene::Entity obj = static_cast<scene::Entity>(r.slot.object);
        if (live(world, user)) ai_->raise(user, "SmartObjectReleased");
        if (live(world, obj)) ai_->raise(obj, "SmartObjectSlotReleased");
        if (live(world, user)) mirror(world, user);
    }
}

void SmartObjectSystem::mirror(scene::World& world, scene::Entity user) {
    if (!ai_ || !live(world, user)) return;
    Blackboard* b = ai_->boardOf(user);
    if (!b) return;
    u32 handles[kSoMaxClaimsPerUser];
    const u32 n = reg_.claimsOf(user, handles, kSoMaxClaimsPerUser);
    if (n == 0) {
        for (const char* k : kMirrorKeys)
            if (b->has(k)) b->reset(k);
        return;
    }
    SoClaimInfo info;
    if (!reg_.claimInfo(handles[0], info)) return;
    putValue(*b, "SmartObject", BbValue::ofEntity(info.slot.object));
    putValue(*b, "SmartObjectSlot", BbValue::ofInt(info.slot.slot));
    putValue(*b, "SmartObjectHandle", BbValue::ofInt(info.handle));
    putValue(*b, "SmartObjectPos", BbValue::ofVec3(info.world.pos));
    putValue(*b, "SmartObjectEntry", BbValue::ofVec3(info.world.entry));
    putValue(*b, "SmartObjectYaw", BbValue::ofFloat(info.world.yawDeg));
}

void SmartObjectSystem::runBehaviour(scene::World&, u32 claim, ClaimRt& rt, f32 dt, std::vector<SoRelease>& released) {
    BtRegistry* reg = ai_ ? ai_->behaviorRegistry() : nullptr;
    SoClaimInfo info;
    if (!reg || rt.finished || !reg_.claimInfo(claim, info) || info.state != SoSlotState::Occupied || !info.def) return;
    const SoUse use = info.def->use;
    if (use != SoUse::Bt && use != SoUse::StateTree) return;
    const i32 subject = static_cast<i32>(rt.user);
    Blackboard* board = ai_->boardOf(rt.user);

    SoReleaseReason reason = SoReleaseReason::Completed;
    bool done = false;
    if (use == SoUse::Bt) {
        if (!rt.btTree) {
            auto& slot = btCache_[info.def->useArg];
            if (!slot) {
                BtAsset asset;
                std::string why;
                if (loadBtAsset(resolveAiAsset(info.def->useArg), asset, &why)) {
                    slot = std::make_unique<BtRuntimeTree>(compileBtAsset(asset));
                } else {
                    AVER_WARN("[Synapse] smart object behaviour '{}' did not load: {}", info.def->useArg, why);
                }
            }
            rt.btTree = slot.get();
            if (!rt.btTree) {
                rt.finished = true;
                done = true;
                reason = SoReleaseReason::Failed;
            }
        }
        if (rt.btTree && !done) {
            if (!rt.started) {
                if (board) board->applySchema(rt.btTree->schema);
                rt.started = true;
            }
            const BtBlackboardCtx ctx{&rt.btTree->decorators, board, &rt.btTree->children};
            const BtStatus s = tickBt(rt.btTree->tree, *reg, subject, dt, rt.bt, &ctx);
            if (s != BtStatus::Running) {
                done = true;
                reason = s == BtStatus::Success ? SoReleaseReason::Completed : SoReleaseReason::Failed;
            }
        }
    } else {
        if (!rt.stAsset) {
            auto& slot = stCache_[info.def->useArg];
            if (!slot) {
                auto asset = std::make_unique<StateTreeAsset>();
                std::string why;
                if (loadStateTree(resolveAiAsset(info.def->useArg), *asset, &why)) {
                    slot = std::move(asset);
                } else {
                    AVER_WARN("[Synapse] smart object behaviour '{}' did not load: {}", info.def->useArg, why);
                }
            }
            rt.stAsset = slot.get();
            if (!rt.stAsset) {
                rt.finished = true;
                done = true;
                reason = SoReleaseReason::Failed;
            }
        }
        if (rt.stAsset && !done) {
            if (!rt.started) {
                if (board) board->applySchema(rt.stAsset->schema);
                rt.started = true;
            }
            const StStatus s = tickStateTree(*rt.stAsset, *reg, subject, dt, rt.st, board);
            if (s == StStatus::Succeeded || s == StStatus::Failed) {
                done = true;
                reason = s == StStatus::Succeeded ? SoReleaseReason::Completed : SoReleaseReason::Failed;
            }
        }
    }
    if (!done) return;
    rt.finished = true;
    if (info.def->autoRelease) {
        SoRelease rel;
        if (reg_.release(claim, reason, &rel)) released.push_back(rel);
    }
}

void SmartObjectSystem::tick(scene::World& world, f32 dt) {
    if (!ai_) return;
    SynapseAi::CallScope scope(*ai_);
    sync(world);

    std::vector<SoRelease> released;
    reg_.tick(dt, released);

    // Users that are gone.
    std::vector<u32> handles;
    handles.reserve(rt_.size());
    for (const auto& [h, rt] : rt_) handles.push_back(h);
    for (const u32 h : handles) {
        const auto it = rt_.find(h);
        if (it == rt_.end()) continue;
        if (live(world, it->second.user)) continue;
        SoRelease rel;
        if (reg_.release(h, SoReleaseReason::UserGone, &rel)) released.push_back(rel);
    }

    // Behaviours of Occupied slots.
    for (const u32 h : handles) {
        const auto it = rt_.find(h);
        if (it != rt_.end()) runBehaviour(world, h, it->second, dt, released);
    }

    handleReleases(world, released);

    // Keep the mirrored position fresh for objects that move.
    for (const auto& [h, rt] : rt_) mirror(world, rt.user);
}

u32 SmartObjectSystem::findSlots(scene::World& world, const EqsGenerator& g, const Vec3& centre, const EqsRunParams& p, EqsItem* out, TagList* tags, u32 cap) {
    sync(world);
    SoQuery q;
    q.origin = centre;
    q.radius = g.radius;
    q.slotTags = g.slotTags;
    q.matchAll = g.matchAll;
    q.includeClaimed = !g.freeOnly;
    q.userTags = p.userTags;
    q.max = cap;
    std::vector<SoSlotRef> refs(cap);
    const u32 n = reg_.find(q, refs.data(), nullptr, cap);
    for (u32 i = 0; i < n; ++i) {
        SoSlotInfo info;
        if (!reg_.slotInfo(refs[i].object, refs[i].slot, info)) continue;
        out[i] = EqsItem{};
        out[i].pos = info.world.entry;
        out[i].entity = refs[i].object;
        out[i].object = refs[i].object;
        out[i].slot = static_cast<i32>(refs[i].slot);
        if (tags && info.def) tags[i] = info.def->tags;
    }
    return n;
}

void SmartObjectSystem::rebase() {
    for (auto& [e, rec] : objs_) rec.rev = ~0u;
}

void SmartObjectSystem::reset() {
    reg_ = SmartObjectRegistry{};
    objs_.clear();
    rt_.clear();
    ended_.clear();
    leafClaims_.clear();
    failed_.clear();
}

} // namespace aver::synapse
