#include "aver/synapse/SynapseBt.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/synapse/SynapseAgent.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace aver::synapse {
namespace {

// Duplicated from SynapseAgent.cpp/SynapsePerception.cpp rather than shared -- see either of their
// own comments for why a three-line helper is not worth a fourth small translation unit.
Vec3 worldPositionOf(scene::World& world, scene::Entity e) {
    const Mat4& m = world.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// ---- the seven built-ins ------------------------------------------------------------------------
// Every one reaches agentSystem()/perceptionSystem() directly rather than through a `user` pointer
// -- see registerBuiltinBehaviors' own comment for why that is deliberate, not a shortcut.

bool hasTargetCondition(i32 subject, const fmt::OcBtNode&, void*) {
    const auto* p = scene::World::instance().component<CSynapsePerception>(
        static_cast<scene::Entity>(subject), perceptionSystem().componentType());
    return p && p->lastKnownTargetEntity != 0;
}

bool canSeeTargetCondition(i32 subject, const fmt::OcBtNode&, void*) {
    const auto* p = scene::World::instance().component<CSynapsePerception>(
        static_cast<scene::Entity>(subject), perceptionSystem().componentType());
    return p && p->canSeeTarget != 0;
}

// params[0] = the threshold, world-space centimetres.
bool distanceToTargetLessCondition(i32 subject, const fmt::OcBtNode& node, void*) {
    scene::World& w = scene::World::instance();
    const auto* p = w.component<CSynapsePerception>(static_cast<scene::Entity>(subject),
                                                     perceptionSystem().componentType());
    if (!p || p->lastKnownTargetEntity == 0) return false;
    const Vec3 a = worldPositionOf(w, static_cast<scene::Entity>(subject));
    const Vec3 b = worldPositionOf(w, static_cast<scene::Entity>(p->lastKnownTargetEntity));
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) < node.params[0];
}

// params[0..2] = the world-space goal. `elapsed` is reused as a one-shot "have I already issued
// this goal" flag rather than an actual duration -- see BtActionFn's own comment (Bt.hpp) for why
// that reuse is exactly what the scratch slot is for: tickBt erases it the moment this node
// resolves (Success or Failure), so the NEXT time MoveTo runs from scratch it correctly reads as
// "not yet issued" again, with no separate bookkeeping needed.
BtStatus moveToAction(i32 subject, const fmt::OcBtNode& node, f32, f32& elapsed, void*) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(subject);
    auto* a = w.component<CSynapseAgent>(e, agentSystem().componentType());
    if (!a) return BtStatus::Failure;   // this subject has no CSynapseAgent to path with at all

    if (elapsed == 0.0f) {
        agentSystem().setGoal(w, e, Vec3{node.params[0], node.params[1], node.params[2]});
        elapsed = 1.0f;
    }
    switch (static_cast<AgentStatus>(a->status)) {
        case AgentStatus::Arrived: return BtStatus::Success;
        case AgentStatus::Failed:  return BtStatus::Failure;
        default:                   return BtStatus::Running;   // None/Requested/Pathing: still working
    }
}

// params[0] = the duration, seconds. The one built-in with no scene dependency at all -- kept here
// beside the other six anyway, so a project reads all seven registrations in one place.
BtStatus waitAction(i32, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void*) {
    elapsed += dt;
    if (elapsed >= node.params[0]) { elapsed = 0.0f; return BtStatus::Success; }
    return BtStatus::Running;
}

// Turns the subject to face its last-known target's CURRENT position, directly (Rot-around-Z, this
// engine's own yaw convention -- see SynapseAgent.cpp's worldForwardOf for the identical math read
// the other way). NOT routed through AverCharacter/CharacterMove: unlike AgentSystem's own tick
// ("Synapse advises, it does not move" -- SynapseAgent.hpp), a BEHAVIOUR TREE ACTION is exactly the
// layer that IS meant to cause a real effect; that rule was about the PATHING tick specifically,
// not about every native system Synapse owns.
BtStatus lookAtAction(i32 subject, const fmt::OcBtNode&, f32, f32&, void*) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(subject);
    const auto* p = w.component<CSynapsePerception>(e, perceptionSystem().componentType());
    if (!p || p->lastKnownTargetEntity == 0) return BtStatus::Failure;

    const Vec3 self = worldPositionOf(w, e);
    const Vec3 target = worldPositionOf(w, static_cast<scene::Entity>(p->lastKnownTargetEntity));
    const f32 dx = target.x - self.x, dy = target.y - self.y;
    if (dx * dx + dy * dy < 1e-6f) return BtStatus::Success;   // standing on it; nothing to turn toward
    const f32 yawRad = std::atan2(dy, dx);
    w.setLocalRotation(e, Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, yawRad));
    return BtStatus::Success;
}

// stringParam = the event name to raise (NOT `name`, which is "FireEvent" itself -- see
// OcBtNode::stringParam's own comment for why the two are separate fields).
BtStatus fireEventAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void*) {
    if (node.stringParam.empty()) return BtStatus::Failure;
    if (!btSystem().fireNotify(static_cast<scene::Entity>(subject), node.stringParam.c_str()))
        return BtStatus::Failure;
    return BtStatus::Success;
}

} // namespace

u32 BtSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSynapseBehavior>("CSynapseBehavior");
    b.field("treeAssetId", scene::FieldKind::I64,
            static_cast<u16>(offsetof(CSynapseBehavior, treeAssetId)))
        .field("teamId", scene::FieldKind::I64,
               static_cast<u16>(offsetof(CSynapseBehavior, teamId)))
        .field("lastStatus", scene::FieldKind::I32,
               static_cast<u16>(offsetof(CSynapseBehavior, lastStatus)), 0, /*readOnly*/ true);
    AVER_ASSERTM(b.verify(sizeof(CSynapseBehavior)), "CSynapseBehavior");
    type_ = b.typeId();
    return type_;
}

CSynapseBehavior* BtSystem::attach(scene::World& world, scene::Entity e) {
    if (type_ == 0) return nullptr;
    auto* b = static_cast<CSynapseBehavior*>(world.addComponent(e, type_));
    if (!b) return nullptr;
    *b = CSynapseBehavior{};
    return b;
}

u64 BtSystem::loadTree(const std::string& path) {
    const u64 id = fnv1a64(path);
    if (trees_.find(id) != trees_.end()) return id;

    BtAsset asset;
    std::string why;
    if (!loadBtAsset(path, asset, &why)) {
        AVER_WARN("[Synapse] could not load behaviour tree '{}': {}", path, why);
        return 0;
    }
    BtRuntimeTree rt = compileBtAsset(asset);
    rt.path = path;
    trees_[id] = std::move(rt);
    return id;
}

u64 BtSystem::registerTree(const std::string& name, const BtAsset& asset) {
    if (!asset.valid()) return 0;
    const u64 id = fnv1a64(name);
    BtRuntimeTree rt = compileBtAsset(asset);
    rt.path = name;
    trees_[id] = std::move(rt);
    return id;
}

bool BtSystem::reloadTree(const std::string& path) {
    const u64 id = fnv1a64(path);
    BtAsset asset;
    std::string why;
    if (!loadBtAsset(path, asset, &why)) {
        AVER_WARN("[Synapse] could not reload behaviour tree '{}': {}", path, why);
        return false;
    }
    BtRuntimeTree rt = compileBtAsset(asset);
    rt.path = path;
    trees_[id] = std::move(rt);

    scene::World& world = scene::World::instance();
    for (auto& [e, state] : running_) {
        const auto* b = world.component<CSynapseBehavior>(e, type_);
        if (b && b->treeAssetId == id) state.reset();
    }
    for (auto& [e, rec] : boards_) {
        const auto* b = world.component<CSynapseBehavior>(e, type_);
        if (b && b->treeAssetId == id) rec->boundTree = 0;   // schema is re-applied on the next tick
    }
    return true;
}

const BtRuntimeTree* BtSystem::findTree(u64 id) const {
    const auto it = trees_.find(id);
    return it == trees_.end() ? nullptr : &it->second;
}

BtSystem::BoardRec& BtSystem::boardRec(scene::Entity e) {
    std::unique_ptr<BoardRec>& slot = boards_[e];
    if (!slot) slot = std::make_unique<BoardRec>();
    return *slot;
}

Blackboard* BtSystem::blackboard(scene::Entity e, bool create) {
    if (const auto it = boards_.find(e); it != boards_.end()) return &it->second->board;
    if (!create || !scene::World::instance().valid(e)) return nullptr;
    return &boardRec(e).board;
}

void BtSystem::bindBoard(scene::Entity e, BoardRec& rec, u64 team, u64 treeId, const BtRuntimeTree* tree) {
    if (rec.team != team) {
        rec.board.setShared(&teams_.get(team));   // before the schema, so Shared keys land on the team board
        rec.team = team;
    }
    if (tree && rec.boundTree != treeId) {
        rec.board.applySchema(tree->schema);
        running_[e].reset();
        rec.boundTree = treeId;
    }
}

bool BtSystem::setTeam(scene::World& world, scene::Entity e, std::string_view team) {
    auto* b = world.component<CSynapseBehavior>(e, type_);
    if (!b) return false;
    b->teamId = SharedBlackboards::idOf(team);
    teamNames_[b->teamId] = std::string(team);
    if (const auto it = boards_.find(e); it != boards_.end()) bindBoard(e, *it->second, b->teamId, 0, nullptr);
    return true;
}

const std::string* BtSystem::teamName(u64 teamId) const {
    const auto it = teamNames_.find(teamId);
    return it == teamNames_.end() ? nullptr : &it->second;
}

void BtSystem::clearBlackboards() {
    boards_.clear();   // entity boards first: they unregister from the team boards
    teams_.clear();
    teamNames_.clear();
}

bool BtSystem::debugView(scene::Entity e, BtDebugView& out) const {
    const auto* b = scene::World::instance().component<CSynapseBehavior>(e, type_);
    if (!b || b->treeAssetId == 0) return false;
    out = BtDebugView{};
    out.treeId = b->treeAssetId;
    out.tree = findTree(b->treeAssetId);
    if (const auto it = running_.find(e); it != running_.end()) out.state = &it->second;
    if (const auto it = boards_.find(e); it != boards_.end()) out.board = &it->second->board;
    return out.tree != nullptr;
}

std::vector<scene::Entity> BtSystem::entitiesUsing(scene::World& world, u64 treeId) const {
    std::vector<scene::Entity> out;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return out;
    for (usize i = 0; i < pool->size(); ++i) {
        const auto* b = static_cast<const CSynapseBehavior*>(pool->dataAt(i));
        if (b && b->treeAssetId != 0 && (treeId == 0 || b->treeAssetId == treeId)) out.push_back(pool->entityAt(i));
    }
    return out;
}

bool BtSystem::fireNotify(scene::Entity e, const char* name) const {
    if (!notify_ || !name) return false;
    notify_(e, name, notifyUser_);
    return true;
}

// Writes perception results into the keys a tree declares by these conventional names.
void BtSystem::syncPerception(scene::World& world, scene::Entity e, Blackboard& board) {
    const u32 perceptionType = perceptionSystem().componentType();
    if (perceptionType == 0) return;
    const auto* p = world.component<CSynapsePerception>(e, perceptionType);
    if (!p) return;
    if (board.has("CanSeeTarget")) board.setBool("CanSeeTarget", p->canSeeTarget != 0);
    if (board.has("Target")) board.setEntity("Target", static_cast<u32>(p->lastKnownTargetEntity));
    if (board.has("TimeSinceSeen")) board.setFloat("TimeSinceSeen", p->timeSinceSeenSec);
}

void BtSystem::tick(scene::World& world, f32 dt) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* b = static_cast<CSynapseBehavior*>(pool->dataAt(i));
        if (!b || b->treeAssetId == 0) continue;

        const auto treeIt = trees_.find(b->treeAssetId);
        if (treeIt == trees_.end()) continue;   // the id does not resolve to a loaded tree
        const BtRuntimeTree& rt = treeIt->second;

        BoardRec& rec = boardRec(e);
        bindBoard(e, rec, b->teamId, b->treeAssetId, &rt);
        syncPerception(world, e, rec.board);

        BtRunningState& state = running_[e];   // default-constructs on first use
        state.trace = (e == watched_);
        const BtBlackboardCtx ctx{&rt.decorators, &rec.board, &rt.children};
        const BtStatus s = tickBt(rt.tree, registry_, static_cast<i32>(e), dt, state, &ctx);
        b->lastStatus = static_cast<i32>(s);
    }

    prune(world);
}

void BtSystem::prune(scene::World& world) {
    // Mirrors AgentSystem::prune's own fix, for the identical reason: running_ is keyed by the full
    // entity handle, so a destroyed behaviour's entry cannot alias a fresh spawn that reuses its
    // index -- but it also never goes away on its own, so it must be erased explicitly once the
    // entity that owns it no longer has a tree assigned.
    for (auto it = running_.begin(); it != running_.end();) {
        const scene::Entity e = it->first;
        const auto* b = world.component<CSynapseBehavior>(e, type_);
        if (!b || b->treeAssetId == 0)
            it = running_.erase(it);
        else
            ++it;
    }
    // Boards live as long as their entity does (a board can pre-date the tree that fills it).
    for (auto it = boards_.begin(); it != boards_.end();) {
        if (!world.valid(it->first)) it = boards_.erase(it);
        else ++it;
    }
}

BtSystem& btSystem() {
    static BtSystem system;
    return system;
}

namespace {

Blackboard* resolveBoard(i32 subject, void*) {
    return btSystem().blackboard(static_cast<scene::Entity>(subject), true);
}

bool typeFromCode(i32 code, BbType& out) {
    if (code < 1 || code > 6) return false;
    out = static_cast<BbType>(code - 1);
    return true;
}

} // namespace

i32 blackboardRelay(i32 op, i32 entity, const char* key, i32 type, i64* i, f32* f, char* text,
                    i32 textCap, void*) {
    if (!key) return 0;
    BtSystem& sys = btSystem();
    scene::World& world = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(entity);
    if (!world.valid(e)) return 0;

    switch (op) {
    case kBbRelayType: {
        Blackboard* b = sys.blackboard(e);
        const i32 idx = b ? b->indexOf(key) : -1;
        return idx < 0 ? 0 : 1 + static_cast<i32>(b->keyDef(idx).type);
    }
    case kBbRelayGet: {
        Blackboard* b = sys.blackboard(e);
        const BbValue* stored = b ? b->get(key) : nullptr;
        if (!stored) return 0;
        BbValue v = *stored;
        BbType want;
        if (typeFromCode(type, want) && !bbCoerce(want, *stored, v)) return 0;
        switch (v.type) {
            case BbType::Bool: case BbType::Int: case BbType::Entity: if (i) *i = v.i; break;
            case BbType::Float: if (f) f[0] = v.f; break;
            case BbType::Vec3:  if (f) { f[0] = v.v.x; f[1] = v.v.y; f[2] = v.v.z; } break;
            case BbType::String:
                if (text && textCap > 0) {
                    const usize n = std::min(v.s.size(), static_cast<usize>(textCap - 1));
                    std::memcpy(text, v.s.data(), n);
                    text[n] = '\0';
                }
                break;
        }
        return 1;
    }
    case kBbRelaySet: {
        BbType t;
        if (!typeFromCode(type, t)) return 0;
        BbValue v = bbDefault(t);
        switch (t) {
            case BbType::Bool: case BbType::Int: case BbType::Entity: if (!i) return 0; v.i = *i; break;
            case BbType::Float: if (!f) return 0; v.f = f[0]; break;
            case BbType::Vec3:  if (!f) return 0; v.v = Vec3{f[0], f[1], f[2]}; break;
            case BbType::String: v.s = text ? text : ""; break;
        }
        if (t == BbType::Bool) v.i = v.i != 0 ? 1 : 0;
        Blackboard* b = sys.blackboard(e, true);
        if (!b) return 0;
        if (!b->has(key)) {
            BbKeyDef d;
            d.name = key;
            d.type = t;
            d.defaultValue = bbDefault(t);
            if (b->defineKey(d) < 0) return 0;
        }
        return b->set(key, v) ? 1 : 0;
    }
    case kBbRelayReset: {
        Blackboard* b = sys.blackboard(e);
        return (b && b->reset(key)) ? 1 : 0;
    }
    case kBbRelayDefine: {
        BbType t;
        if (!typeFromCode(type, t)) return 0;
        Blackboard* b = sys.blackboard(e, true);
        if (!b) return 0;
        BbKeyDef d;
        d.name = key;
        d.type = t;
        d.scope = (i && *i != 0) ? BbScope::Shared : BbScope::Agent;
        d.defaultValue = bbDefault(t);
        return b->defineKey(d) >= 0 ? 1 : 0;
    }
    case kBbRelaySetTeam:
        return sys.setTeam(world, e, key) ? 1 : 0;
    }
    return 0;
}

namespace {

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

} // namespace

void BtHearingSink::onHeard(u32 listener, const HeardMemory& m) {
    Blackboard* b = btSystem().blackboard(static_cast<scene::Entity>(listener), true);
    if (!b) return;
    putValue(*b, "HeardPosition", BbValue::ofVec3(m.pos));
    putValue(*b, "HeardLevel", BbValue::ofFloat(m.level));
    putValue(*b, "HeardTag", BbValue::ofInt(static_cast<i64>(m.tag)));
    putValue(*b, "HeardSource", BbValue::ofEntity(m.source));
    putValue(*b, "HeardConfidence", BbValue::ofFloat(m.confidence));
    putValue(*b, "HasHeard", BbValue::ofBool(true));   // last, so observers of it see the rest
}

void BtHearingSink::onForgotten(u32 listener, const HeardMemory& m) {
    Blackboard* b = btSystem().blackboard(static_cast<scene::Entity>(listener));
    if (!b || !b->has("HasHeard")) return;
    const Vec3 mirrored = b->getVec3("HeardPosition");
    const bool same = mirrored.x == m.pos.x && mirrored.y == m.pos.y && mirrored.z == m.pos.z &&
                      b->getInt("HeardTag") == static_cast<i64>(m.tag);
    if (!same) return;
    b->setFloat("HeardConfidence", 0.0f);
    b->setBool("HasHeard", false);
}

void registerBuiltinBehaviors(BtSystem& system) {
    BtRegistry& reg = system.registry();
    reg.setBoardResolver(&resolveBoard, nullptr);
    reg.registerBlackboardLeaves();
    reg.registerCondition("HasTarget", &hasTargetCondition);
    reg.registerCondition("CanSeeTarget", &canSeeTargetCondition);
    reg.registerCondition("DistanceToTargetLess", &distanceToTargetLessCondition);
    reg.registerAction("MoveTo", &moveToAction);
    reg.registerAction("Wait", &waitAction);
    reg.registerAction("LookAt", &lookAtAction);
    reg.registerAction("FireEvent", &fireEventAction);
}

} // namespace aver::synapse
