// CIkRig and its runtime. See IkRigSystem.hpp for the shape and the rules.
#include "aver/anim/IkRigSystem.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::anim {

namespace {

u32 g_ikType = 0;

constexpr f32 kDefaultBlend = 0.15f;

Vec3 xformPoint(const Vec3& v, const Mat4& m) {
    return Vec3{v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0],
                v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]};
}

Quat conjq(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }
f32 clamp01f(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

struct IkRigRuntime::GroundCtx {
    IkRigRuntime* rt = nullptr;
    scene::Entity e = scene::kInvalidEntity;
    Mat4 W, Winv;
    Quat wrotInv = Quat::identity();   // world rotation -> model rotation
    f32 scale = 1.0f;
};

u32 IkRigRuntime::registerComponent(scene::World& world) {
    auto b = world.registerComponent<CIkRig>("CIkRig");
    b.field("asset", scene::FieldKind::I64, static_cast<u16>(offsetof(CIkRig, asset)))
        .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CIkRig, weight)))
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CIkRig, flags)))
        .field("reserved0", scene::FieldKind::I32, static_cast<u16>(offsetof(CIkRig, reserved)), 0, true)
        .field("reserved1", scene::FieldKind::I32, static_cast<u16>(offsetof(CIkRig, reserved) + sizeof(u32)), 0, true);
    AVER_ASSERTM(b.verify(sizeof(CIkRig)), "CIkRig");
    g_ikType = b.typeId();
    return g_ikType;
}

u32 IkRigRuntime::typeId() { return g_ikType; }

CIkRig* IkRigRuntime::attach(scene::World& world, scene::Entity e, u64 asset, f32 weight) {
    if (g_ikType == 0) return nullptr;
    auto* c = static_cast<CIkRig*>(world.addComponent(e, g_ikType));
    if (!c) return nullptr;
    // A fresh value over the zero-filled bytes, then the weight: see the header on why a raw attach is inert.
    *c = CIkRig{};
    c->asset = asset;
    c->weight = weight;
    return c;
}

const IkRigAsset* IkRigRuntime::assetFor(u64 asset) {
    if (asset == 0 || !anim_) return nullptr;
    const auto it = infos_.find(asset);
    if (it != infos_.end()) return it->second->ok ? &it->second->asset : nullptr;

    auto info = std::make_unique<Info>();
    const std::string path = anim_->assetPath(asset);
    std::string why;
    if (path.empty() || !loadIkRig(path, info->asset, &why) || !info->asset.valid(&why)) {
        // Cached as failed: a missing rig is reported once, not per entity per frame.
        AVER_WARN("[IkRig] could not load rig asset {}: {}", asset, why.empty() ? path : why);
    } else {
        info->ok = true;
        if (!info->asset.body.goal.empty()) info->goals.insert(fnv1a64(std::string_view(info->asset.body.goal)));
        for (const IkRigChainDef& c : info->asset.chains)
            if (!c.goal.empty()) info->goals.insert(fnv1a64(std::string_view(c.goal)));
        for (const IkRigFootDef& f : info->asset.feet) info->feet.insert(fnv1a64(std::string_view(f.chain)));
    }
    const Info* raw = info.get();
    infos_[asset] = std::move(info);
    return raw->ok ? &raw->asset : nullptr;
}

const IkRigRuntime::Bound* IkRigRuntime::boundFor(u64 asset, u64 skeletonId, const fmt::OcSkeleton& skel) {
    const std::pair<u64, u64> key{asset, skeletonId};
    const auto it = bounds_.find(key);
    if (it != bounds_.end()) return it->second->bound.ok ? it->second.get() : nullptr;

    const IkRigAsset* a = assetFor(asset);
    if (!a) return nullptr;
    auto b = std::make_unique<Bound>();
    bindIkRig(*a, skel, b->bound);
    for (const std::string& issue : b->bound.issues) AVER_WARN("[IkRig] rig {}: {}", asset, issue);
    if (!a->skeleton.empty()) {
        std::string path = a->skeleton;
        std::replace(path.begin(), path.end(), '\\', '/');
        if (fnv1a64(std::string_view(path)) != skeletonId)
            AVER_WARN("[IkRig] rig {} was authored for '{}' but is applied to another skeleton; bound by bone name", asset, a->skeleton);
    }
    const Bound* raw = b.get();
    bounds_[key] = std::move(b);
    return raw->bound.ok ? raw : nullptr;
}

void IkRigRuntime::stepAlpha(f32& alpha, f32 goal, f32 rate, f32 dt) {
    if (alpha == goal) return;
    if (rate <= 0.0f) { alpha = goal; return; }
    const f32 step = rate * dt;
    alpha = alpha < goal ? std::min(goal, alpha + step) : std::max(goal, alpha - step);
}

bool IkRigRuntime::resolveBound(const IkGoalState& g, Vec3& posWorld, Quat& rotWorld) const {
    if (!world_ || !anim_) return false;
    if (!world_->valid(g.target) || world_->destroyPending(g.target)) return false;
    Mat4 frame = world_->worldMatrix(g.target);
    if (g.socket != 0) {
        const fmt::OcSkeleton* sk = anim_->posedSkeleton(g.target);
        const Pose* tp = anim_->pose(g.target);
        if (!sk || !tp) return false;
        const fmt::OcSocket* sock = sk->socketById(g.socket);
        if (!sock) return false;
        std::vector<Mat4> model;
        poseToModel(*sk, *tp, model);
        Mat4 sm;
        if (!socketModelMatrix(model, *sock, sm)) return false;
        frame = sm * frame;
    }
    posWorld = xformPoint(g.off, frame);
    rotWorld = (transformFromMatrix(frame).rotation.normalized() * g.rotOff).normalized();
    return true;
}

bool IkRigRuntime::groundThunk(const Vec3& originModel, f32 distance, u32 layerMask, Vec3& pointModel,
                               Vec3& normalModel, void* user) {
    auto* c = static_cast<GroundCtx*>(user);
    IkRigRuntime* rt = c->rt;
    if (!rt->probe_) return false;
    ++rt->probesThis_;
    Vec3 pw{0, 0, 0}, nw{0, 0, 1};
    if (!rt->probe_(c->e, xformPoint(originModel, c->W), distance * c->scale, layerMask, pw, nw, rt->probeUser_)) return false;
    pointModel = xformPoint(pw, c->Winv);
    normalModel = c->wrotInv.rotate(nw).getSafeNormal();
    return true;
}

bool IkRigRuntime::curveThunk(u64 nameHash, f32& out, void* user) {
    auto* c = static_cast<GroundCtx*>(user);
    return c->rt->anim_->curveValue(c->e, nameHash, out);
}

void IkRigRuntime::beginTick(u64 tickIndex) {
    if (tickIndex == tickSeen_) return;
    tickSeen_ = tickIndex;
    probesLast_ = probesThis_;
    probesThis_ = 0;
    if (!world_) return;
    for (auto it = ents_.begin(); it != ents_.end();) {
        if (!world_->valid(it->first) || world_->destroyPending(it->first)) it = ents_.erase(it);
        else ++it;
    }
}

bool IkRigRuntime::apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, const CIkRig& c, u32 probeBudget) {
    if (!world_ || !anim_ || c.weight <= 0.0f) return false;
    if (!assetFor(c.asset)) return false;
    const auto* sm = world_->component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    const Bound* b = boundFor(c.asset, sm ? sm->skeleton : 0, skel);
    if (!b) return false;

    EntityRt& r = ents_[e];
    if (r.bound != b || r.asset != c.asset) {
        r.bound = b;
        r.asset = c.asset;
        r.state = IkRigState{};      // a rebind (new rig, new skeleton) starts clean and snaps on its first tick
    }
    const f32 dt = anim_->tickDt();
    const IkRigBound& rig = b->bound;

    GroundCtx ctx;
    ctx.rt = this;
    ctx.e = e;
    ctx.W = world_->worldMatrix(e);
    ctx.Winv = ctx.W.inverse();
    ctx.wrotInv = conjq(transformFromMatrix(ctx.W).rotation.normalized());
    ctx.scale = std::sqrt(ctx.W.m[0][0] * ctx.W.m[0][0] + ctx.W.m[0][1] * ctx.W.m[0][1] + ctx.W.m[0][2] * ctx.W.m[0][2]);
    if (!(ctx.scale > 1e-6f)) ctx.scale = 1.0f;

    // Goals: bound ones follow their socket, alphas ease, everything lands in model space.
    goalVals_.assign(rig.goalHashes.size(), IkGoalValue{});
    for (usize i = 0; i < rig.goalHashes.size(); ++i) {
        const auto it = r.goals.find(rig.goalHashes[i]);
        if (it == r.goals.end()) continue;
        IkGoalState& g = it->second;
        if (!g.script || !g.havePos) continue;
        if (g.bound) {
            Vec3 pw;
            Quat qw;
            if (resolveBound(g, pw, qw)) {
                g.pos = pw;
                g.rot = qw;
                g.hasRot = true;
                g.space = 0;
                g.everResolved = true;
            } else if (!g.everResolved) {
                continue;       // a target spawned this frame is not posed yet: wait for it
            } else if (g.alphaGoal > 0.0f) {
                // The target died or lost the socket: release the goal, do not freeze it.
                g.alphaGoal = 0.0f;
                g.alphaRate = 1.0f / kDefaultBlend;
            }
        }
        stepAlpha(g.alpha, g.alphaGoal, g.alphaRate, dt);
        Vec3 p = g.pos;
        Quat q = g.rot;
        if (g.space == 0) {
            p = xformPoint(p, ctx.Winv);
            q = (ctx.wrotInv * q).normalized();
        }
        IkGoalValue& v = goalVals_[i];
        v.active = true;
        v.pos = p;
        v.hasRot = g.hasRot;
        v.rot = q;
        v.alpha = g.alpha;
    }

    // Foot alphas: a named foot overrides the "every foot" ramp.
    stepAlpha(r.all.alpha, r.all.goal, r.all.rate, dt);
    for (auto& kv : r.feet) stepAlpha(kv.second.alpha, kv.second.goal, kv.second.rate, dt);
    footAlphas_.assign(rig.feet.size(), 1.0f);
    for (usize f = 0; f < rig.feet.size(); ++f) {
        const auto it = r.feet.find(rig.feet[f].nameHash);
        footAlphas_[f] = it != r.feet.end() ? it->second.alpha : r.all.alpha;
    }

    IkRigInputs in;
    in.weight = c.weight;
    in.flags = c.flags;
    in.dt = dt;
    in.goals = goalVals_.data();
    in.goalCount = goalVals_.size();
    in.footAlpha = footAlphas_.data();
    in.curve = &IkRigRuntime::curveThunk;
    in.curveUser = &ctx;
    if (probe_) {
        in.ground = &IkRigRuntime::groundThunk;
        in.groundUser = &ctx;
        // Over budget: the feet hold their last offsets for this tick instead of casting.
        in.probe = probesThis_ + static_cast<u32>(rig.feet.size()) <= probeBudget;
    }
    solveIkRig(skel, rig, r.state, in, pose, r.report, scratch_);

    for (usize i = 0; i < rig.goalHashes.size() && i < r.report.goalError.size(); ++i) {
        const auto it = r.goals.find(rig.goalHashes[i]);
        if (it == r.goals.end()) continue;
        it->second.errorCm = r.report.goalError[i];
        it->second.reached = r.report.goalReached[i] != 0;
    }
    return r.report.changed;
}

void IkRigRuntime::clear() {
    infos_.clear();
    bounds_.clear();
    for (auto& kv : ents_) kv.second.bound = nullptr;   // rebinds (and snaps) next tick
}

void IkRigRuntime::rebase(const Vec3& delta) {
    for (auto& kv : ents_)
        for (auto& g : kv.second.goals)
            if (g.second.space == 0) g.second.pos -= delta;
}
void IkRigRuntime::clearRuntime() {
    ents_.clear();
    probesThis_ = probesLast_ = 0;
}

// ---- the script surface ----

const IkRigRuntime::Info* IkRigRuntime::infoFor(scene::Entity e, const CIkRig*& c) const {
    c = nullptr;
    if (!world_ || g_ikType == 0) return nullptr;
    c = world_->component<CIkRig>(e, g_ikType);
    if (!c) return nullptr;
    const auto it = infos_.find(c->asset);
    if (it != infos_.end()) return it->second->ok ? it->second.get() : nullptr;
    // Not loaded yet (no tick has run for this entity): load now so a script's first call is not refused.
    const IkRigAsset* a = const_cast<IkRigRuntime*>(this)->assetFor(c->asset);
    if (!a) return nullptr;
    return infos_.find(c->asset)->second.get();
}

bool IkRigRuntime::setGoal(scene::Entity e, u64 goal, i32 space, const Vec3& pos, const Quat* rot) {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || !info->goals.count(goal)) return false;
    IkGoalState& g = ents_[e].goals[goal];
    g.pos = pos;
    g.space = space == 1 ? 1 : 0;
    g.hasRot = rot != nullptr;
    if (rot) g.rot = rot->normalized();
    g.bound = false;
    g.havePos = true;
    g.script = true;
    // The one-line call works: a goal nobody weighted engages on its own (0.15 s); an explicit weight wins.
    if (!g.weightSet) { g.alphaGoal = 1.0f; g.alphaRate = 1.0f / kDefaultBlend; }
    return true;
}

bool IkRigRuntime::bindGoal(scene::Entity e, u64 goal, scene::Entity target, u64 socket, const Vec3& off, const Quat* rotOff) {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || !info->goals.count(goal)) return false;
    IkGoalState& g = ents_[e].goals[goal];
    g.bound = true;
    g.everResolved = false;
    g.target = target;
    g.socket = socket;
    g.off = off;
    g.rotOff = rotOff ? rotOff->normalized() : Quat::identity();
    g.havePos = true;
    g.script = true;
    if (!g.weightSet) { g.alphaGoal = 1.0f; g.alphaRate = 1.0f / kDefaultBlend; }
    return true;
}

bool IkRigRuntime::setGoalWeight(scene::Entity e, u64 goal, f32 weight, f32 blendSeconds) {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || !info->goals.count(goal)) return false;
    IkGoalState& g = ents_[e].goals[goal];
    g.script = true;
    g.weightSet = true;
    g.alphaGoal = clamp01f(weight);
    const f32 blend = blendSeconds < 0.0f ? kDefaultBlend : blendSeconds;
    if (blend <= 0.0f) { g.alpha = g.alphaGoal; g.alphaRate = 0.0f; }
    else g.alphaRate = 1.0f / blend;
    return true;
}

bool IkRigRuntime::clearGoal(scene::Entity e, u64 goal) {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || (goal != 0 && !info->goals.count(goal))) return false;
    const auto it = ents_.find(e);
    if (it == ents_.end()) return true;
    for (auto& kv : it->second.goals) {
        if (goal != 0 && kv.first != goal) continue;
        kv.second.alphaGoal = 0.0f;
        kv.second.alphaRate = 1.0f / kDefaultBlend;
        kv.second.weightSet = false;       // the next SetIkGoal engages it again
        kv.second.bound = false;
    }
    return true;
}

bool IkRigRuntime::goalState(scene::Entity e, u64 goal, f32* out) const {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || !info->goals.count(goal)) return false;
    f32 alpha = 0.0f, err = 0.0f, reached = 0.0f;
    const auto it = ents_.find(e);
    if (it != ents_.end()) {
        const auto g = it->second.goals.find(goal);
        if (g != it->second.goals.end()) { alpha = g->second.alpha; err = g->second.errorCm; reached = g->second.reached ? 1.0f : 0.0f; }
    }
    if (out) { out[0] = alpha; out[1] = err; out[2] = reached; }
    return true;
}

bool IkRigRuntime::setFootWeight(scene::Entity e, u64 foot, f32 weight, f32 blendSeconds) {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || (foot != 0 && !info->feet.count(foot))) return false;
    EntityRt& r = ents_[e];
    const f32 w = clamp01f(weight);
    const f32 blend = blendSeconds < 0.0f ? 0.2f : blendSeconds;
    auto apply = [&](FootRt& f) {
        f.goal = w;
        if (blend <= 0.0f) { f.alpha = w; f.rate = 0.0f; }
        else f.rate = 1.0f / blend;
    };
    if (foot == 0) {
        apply(r.all);
        for (auto& kv : r.feet) apply(kv.second);
    } else {
        auto it = r.feet.find(foot);
        if (it == r.feet.end()) {
            FootRt f;
            f.alpha = r.all.alpha;
            f.goal = r.all.goal;
            it = r.feet.emplace(foot, f).first;
        }
        apply(it->second);
    }
    return true;
}

bool IkRigRuntime::footState(scene::Entity e, u64 foot, f32* out) const {
    const CIkRig* c = nullptr;
    const Info* info = infoFor(e, c);
    if (!info || (foot != 0 && !info->feet.count(foot))) return false;
    const auto it = ents_.find(e);
    if (it == ents_.end() || !it->second.bound) return false;
    const EntityRt& r = it->second;
    f32 v[5] = {0, 0, 0, 0, 1};
    if (foot == 0) {
        v[0] = r.state.pelvis;
        v[1] = 1.0f;
    } else {
        const i32 idx = r.bound->bound.findFoot(foot);
        if (idx < 0 || static_cast<usize>(idx) >= r.state.feet.size()) return false;
        const IkFootState& fs = r.state.feet[static_cast<usize>(idx)];
        v[0] = fs.offset;
        v[1] = fs.plant;
        v[2] = fs.normal.x; v[3] = fs.normal.y; v[4] = fs.normal.z;
    }
    if (out) for (int i = 0; i < 5; ++i) out[i] = v[i];
    return true;
}

// ---- provider ----

namespace {
// framework_ik_abi.h
constexpr i32 kInFloat = 1, kInVec3 = 2, kInQuat = 3, kInClear = 4, kGoalSet = 5, kGoalBind = 6, kGoalWeight = 7,
              kGoalClear = 8, kGoalState = 9, kFootWeight = 10, kFootState = 11;
}

i32 ikProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI, void* user) {
    (void)outI;
    if (!user) return 0;
    ControlRigSystem& rig = *static_cast<ControlRigSystem*>(user);
    IkRigRuntime& ik = rig.ik();
    const scene::Entity e = static_cast<scene::Entity>(entity);
    auto need = [&](i32 n) { return in != nullptr && inCount >= n; };
    switch (op) {
        case kInFloat: return need(1) && rig.setInput(e, static_cast<u64>(h0), in, 1) ? 1 : 0;
        case kInVec3:  return need(3) && rig.setInput(e, static_cast<u64>(h0), in, 3) ? 1 : 0;
        case kInQuat:  return need(4) && rig.setInput(e, static_cast<u64>(h0), in, 4) ? 1 : 0;
        case kInClear: rig.clearInput(e, static_cast<u64>(h0)); return 1;
        case kGoalSet: {
            if (!need(3)) return 0;
            const Quat q = inCount >= 7 ? Quat{in[3], in[4], in[5], in[6]} : Quat::identity();
            return ik.setGoal(e, static_cast<u64>(h0), static_cast<i32>(h1), Vec3{in[0], in[1], in[2]}, inCount >= 7 ? &q : nullptr) ? 1 : 0;
        }
        case kGoalBind: {
            if (!need(3)) return 0;
            const Quat q = inCount >= 7 ? Quat{in[3], in[4], in[5], in[6]} : Quat::identity();
            return ik.bindGoal(e, static_cast<u64>(h0), static_cast<scene::Entity>(h1), static_cast<u64>(h2),
                               Vec3{in[0], in[1], in[2]}, inCount >= 7 ? &q : nullptr) ? 1 : 0;
        }
        case kGoalWeight: return need(1) && ik.setGoalWeight(e, static_cast<u64>(h0), in[0], inCount >= 2 ? in[1] : -1.0f) ? 1 : 0;
        case kGoalClear:  return ik.clearGoal(e, static_cast<u64>(h0)) ? 1 : 0;
        case kGoalState:  return ik.goalState(e, static_cast<u64>(h0), outF) ? 1 : 0;
        case kFootWeight: return need(1) && ik.setFootWeight(e, static_cast<u64>(h0), in[0], inCount >= 2 ? in[1] : -1.0f) ? 1 : 0;
        case kFootState:  return ik.footState(e, static_cast<u64>(h0), outF) ? 1 : 0;
        default: return 0;
    }
}

} // namespace aver::anim
