// Animation state machine runtime. See AnimStateMachine.hpp.
#include "aver/anim/AnimStateMachine.hpp"
#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <set>

namespace aver::anim {

namespace {
constexpr usize kMaxDepth = 16;
}

i32 AnimStateMachineAsset::paramIndex(const std::string& n) const {
    for (usize i = 0; i < params.size(); ++i)
        if (params[i].name == n) return static_cast<i32>(i);
    return -1;
}

bool AnimStateMachineAsset::valid(std::string* why) const {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (machines.empty()) return fail("no machines");
    std::set<std::string> names;
    for (const AsmParam& p : params)
        if (p.name.empty() || !names.insert(p.name).second) return fail("parameter names must be unique and non-empty");

    std::set<i32> referenced;
    const i32 mc = static_cast<i32>(machines.size());
    for (i32 mi = 0; mi < mc; ++mi) {
        const AsmMachine& m = machines[mi];
        const i32 sc = static_cast<i32>(m.states.size());
        if (sc == 0) return fail("machine '" + m.name + "' has no states");
        if (m.entry < 0 || m.entry >= sc) return fail("machine '" + m.name + "' entry out of range");
        for (const AsmState& s : m.states) {
            if (s.kind == AsmStateKind::SubMachine) {
                if (s.subMachine <= 0 || s.subMachine >= mc || s.subMachine == mi)
                    return fail("state '" + s.name + "' names a bad sub-machine");
                if (!referenced.insert(s.subMachine).second)
                    return fail("a machine can be the sub-machine of only one state");
            } else if (s.asset.empty()) {
                return fail("state '" + s.name + "' has no asset");
            }
        }
        for (const AsmTransition& t : m.transitions) {
            if (t.from < kAsmAny || t.from >= sc) return fail("transition source out of range");
            if (t.to == kAsmExit) {
                if (mi == 0) return fail("the root machine has nothing to exit");
            } else if (t.to < 0 || t.to >= sc) {
                return fail("transition target out of range");
            }
            if (t.blendTime < 0.0f) return fail("negative blend time");
            for (const AsmCondition& c : t.conditions)
                if (paramIndex(c.param) < 0) return fail("condition names unknown parameter '" + c.param + "'");
        }
    }
    // The sub-machine references must form a tree hanging off machine 0.
    std::set<i32> seen{0};
    std::vector<i32> stack{0};
    while (!stack.empty()) {
        const i32 mi = stack.back();
        stack.pop_back();
        for (const AsmState& s : machines[mi].states) {
            if (s.kind != AsmStateKind::SubMachine) continue;
            if (!seen.insert(s.subMachine).second) return fail("sub-machine cycle");
            stack.push_back(s.subMachine);
        }
    }
    return true;
}

void AnimStateMachine::bind(const AnimStateMachineAsset* asset, AsmClipFn clips, AsmBlendSpaceFn spaces) {
    asset_ = asset;
    clips_ = std::move(clips);
    spaces_ = std::move(spaces);
    values_.clear();
    triggers_.clear();
    index_.clear();
    byHash_.clear();
    path_.clear();
    layers_.clear();
    events_.clear();
    blockInterrupt_ = false;
    fired_ = 0;
    if (!asset_ || asset_->machines.empty()) { asset_ = nullptr; return; }

    for (usize i = 0; i < asset_->params.size(); ++i) {
        const AsmParam& p = asset_->params[i];
        values_.push_back(p.type == AsmParamType::Trigger ? 0.0f : p.def);
        triggers_.push_back(false);
        index_[p.name] = static_cast<u32>(i);
        byHash_[fnv1a64(p.name)] = static_cast<u32>(i);
    }
    path_.push_back({0, asset_->machines[0].entry, false});
    descend(path_);
    for (const PathEl& e : path_) raise(AsmEvent::Kind::Enter, e);
    pushLayer(path_.back().machine, path_.back().state, 0.0f);
}

void AnimStateMachine::descend(std::vector<PathEl>& path) const {
    for (usize depth = 0; depth < kMaxDepth; ++depth) {
        const AsmState& s = stateAt(path.back());
        if (s.kind != AsmStateKind::SubMachine) return;
        path.push_back({s.subMachine, asset_->machines[s.subMachine].entry, false});
    }
}

void AnimStateMachine::raise(AsmEvent::Kind kind, const PathEl& el) {
    const AsmState& s = stateAt(el);
    AsmEvent e;
    e.kind = kind;
    e.machine = asset_->machines[el.machine].name;
    e.state = s.name;
    e.name = kind == AsmEvent::Kind::Enter ? s.onEnter : s.onExit;
    events_.push_back(std::move(e));
}

void AnimStateMachine::pushLayer(i32 machine, i32 state, f32 blendTime) {
    Layer l;
    l.machine = machine;
    l.state = state;
    const AsmState& s = asset_->machines[machine].states[state];
    if (s.kind == AsmStateKind::Clip) {
        l.clip = clips_ ? clips_(s.asset) : nullptr;
        l.duration = (l.clip && l.clip->duration > 0.0f) ? l.clip->duration : 1.0f;
    } else if (s.kind == AsmStateKind::BlendSpace) {
        const BlendSpaceAsset* space = spaces_ ? spaces_(s.asset) : nullptr;
        if (space && space->valid()) {
            std::vector<const fmt::OcAnimation*> cs;
            for (const BlendSample& smp : space->samples) cs.push_back(clips_ ? clips_(smp.clip) : nullptr);
            l.bs.bind(space, std::move(cs));
            l.bs.setInput(s.xParam.empty() ? 0.0f : paramByName(s.xParam),
                          s.yParam.empty() ? 0.0f : paramByName(s.yParam));
            l.bs.snapInput();
            l.bs.advance(0.0f);
            l.isBlendSpace = true;
        }
    }
    if (blendTime <= 0.0f) {
        layers_.clear();
        l.alpha = 1.0f;
    } else {
        l.alpha = 0.0f;
        l.blendTime = blendTime;
    }
    layers_.push_back(std::move(l));
}

f32 AnimStateMachine::paramByName(const std::string& n) const {
    const auto it = index_.find(n);
    return it == index_.end() ? 0.0f : values_[it->second];
}

void AnimStateMachine::setFloat(const std::string& name, f32 v) {
    const auto it = index_.find(name);
    if (it != index_.end()) setByHash(fnv1a64(name), v);
}
void AnimStateMachine::setInt(const std::string& name, i32 v) { setFloat(name, static_cast<f32>(v)); }
void AnimStateMachine::setBool(const std::string& name, bool v) { setFloat(name, v ? 1.0f : 0.0f); }
void AnimStateMachine::setTrigger(const std::string& name) {
    const auto it = index_.find(name);
    if (it != index_.end()) triggers_[it->second] = true;
}

bool AnimStateMachine::setByHash(u64 h, f32 v) {
    const auto it = byHash_.find(h);
    if (it == byHash_.end()) return false;
    const u32 i = it->second;
    switch (asset_->params[i].type) {
        case AsmParamType::Float:   values_[i] = v; break;
        case AsmParamType::Int:     values_[i] = std::round(v); break;
        case AsmParamType::Bool:    values_[i] = v != 0.0f ? 1.0f : 0.0f; break;
        case AsmParamType::Trigger: if (v != 0.0f) triggers_[i] = true; break;
    }
    return true;
}

f32 AnimStateMachine::value(const std::string& name) const {
    const auto it = index_.find(name);
    if (it == index_.end()) return 0.0f;
    return asset_->params[it->second].type == AsmParamType::Trigger ? (triggers_[it->second] ? 1.0f : 0.0f)
                                                                   : values_[it->second];
}

bool AnimStateMachine::triggerPending(const std::string& name) const {
    const auto it = index_.find(name);
    return it != index_.end() && triggers_[it->second];
}

f32 AnimStateMachine::normalizedTime() const {
    if (layers_.empty()) return 0.0f;
    const Layer& l = layers_.back();
    if (l.isBlendSpace) return l.bs.cycles();
    return l.duration > 0.0f ? l.elapsed / l.duration : 0.0f;
}

bool AnimStateMachine::conditionsHold(const AsmTransition& t) {
    for (const AsmCondition& c : t.conditions) {
        const auto it = index_.find(c.param);
        if (it == index_.end()) return false;
        const f32 v = values_[it->second];
        bool ok = false;
        switch (c.op) {
            case AsmOp::Greater:   ok = v > c.value; break;
            case AsmOp::GreaterEq: ok = v >= c.value; break;
            case AsmOp::Less:      ok = v < c.value; break;
            case AsmOp::LessEq:    ok = v <= c.value; break;
            case AsmOp::Equal:     ok = std::fabs(v - c.value) < 1e-4f; break;
            case AsmOp::NotEqual:  ok = std::fabs(v - c.value) >= 1e-4f; break;
            case AsmOp::IsTrue:    ok = v != 0.0f; break;
            case AsmOp::IsFalse:   ok = v == 0.0f; break;
            case AsmOp::Trigger:   ok = triggers_[it->second]; break;
        }
        if (!ok) return false;
    }
    return true;
}

bool AnimStateMachine::exitTimeReached(const AsmTransition& t, usize level) const {
    if (stateAt(path_[level]).kind == AsmStateKind::SubMachine) return path_[level].exited;
    return normalizedTime() >= t.exitTime;
}

void AnimStateMachine::fire(usize level, const AsmTransition& t, i32 target) {
    for (const AsmCondition& c : t.conditions)
        if (c.op == AsmOp::Trigger) triggers_[static_cast<u32>(asset_->paramIndex(c.param))] = false;
    ++fired_;

    if (target == kAsmExit) {
        if (level >= 1) path_[level - 1].exited = true;
        return;
    }

    std::vector<PathEl> np(path_.begin(), path_.begin() + static_cast<std::ptrdiff_t>(level));
    np.push_back({path_[level].machine, target, false});
    descend(np);

    usize common = 0;
    while (common < level && common < np.size() && path_[common].machine == np[common].machine &&
           path_[common].state == np[common].state)
        ++common;
    for (usize i = path_.size(); i-- > common;) raise(AsmEvent::Kind::Exit, path_[i]);
    for (usize i = common; i < np.size(); ++i) raise(AsmEvent::Kind::Enter, np[i]);
    path_ = std::move(np);

    pushLayer(path_.back().machine, path_.back().state, t.blendTime);
    blockInterrupt_ = !t.interruptible && t.blendTime > 0.0f;
}

void AnimStateMachine::tick(f32 dt) {
    if (!asset_ || layers_.empty()) return;

    for (Layer& l : layers_) {
        const AsmState& s = asset_->machines[l.machine].states[l.state];
        const f32 speed = s.speed * (s.speedParam.empty() ? 1.0f : paramByName(s.speedParam));
        const f32 step = dt * speed;
        if (l.isBlendSpace) {
            l.bs.setInput(s.xParam.empty() ? 0.0f : paramByName(s.xParam),
                          s.yParam.empty() ? 0.0f : paramByName(s.yParam));
            l.bs.advance(step);
        } else {
            l.elapsed += step;
            l.time += step;
            if (s.loop) {
                if (l.time >= l.duration) l.time = std::fmod(l.time, l.duration);
            } else {
                l.time = std::min(l.time, l.duration);
                l.elapsed = std::min(l.elapsed, l.duration);
            }
        }
    }

    Layer& newest = layers_.back();
    if (newest.alpha < 1.0f && layers_.size() > 1) {
        newest.blendElapsed += dt;
        newest.alpha = newest.blendTime > 0.0f ? std::min(1.0f, newest.blendElapsed / newest.blendTime) : 1.0f;
        if (newest.alpha >= 1.0f - 1e-5f) {
            newest.alpha = 1.0f;
            layers_.erase(layers_.begin(), layers_.end() - 1);
        }
    }
    if (layers_.size() == 1) blockInterrupt_ = false;
    if (blockInterrupt_) return;

    for (usize level = path_.size(); level-- > 0;) {
        const PathEl el = path_[level];
        const AsmMachine& m = asset_->machines[el.machine];
        for (const AsmTransition& t : m.transitions) {
            if (t.from != el.state && t.from != kAsmAny) continue;
            if (t.from == kAsmAny && t.to == el.state && !t.allowSelf) continue;
            if (t.to == kAsmExit && (level == 0 || path_[level - 1].exited)) continue;
            if (t.hasExitTime && !exitTimeReached(t, level)) continue;
            if (!conditionsHold(t)) continue;
            fire(level, t, t.to);
            return;
        }
    }
}

std::vector<f32> AnimStateMachine::layerWeights() const {
    std::vector<f32> w(layers_.size(), 0.0f);
    f32 acc = 1.0f;
    for (usize i = layers_.size(); i-- > 0;) {
        w[i] = acc * (i == 0 ? 1.0f : layers_[i].alpha);
        acc *= 1.0f - layers_[i].alpha;
    }
    return w;
}

std::vector<std::string> AnimStateMachine::layerStates() const {
    std::vector<std::string> out;
    for (const Layer& l : layers_) out.push_back(asset_->machines[l.machine].states[l.state].name);
    return out;
}

void AnimStateMachine::evaluate(const fmt::OcSkeleton& skel, Pose& out) const {
    restPose(skel, out);
    if (!asset_) return;
    const std::vector<f32> w = layerWeights();
    f32 acc = 0.0f;
    Pose tmp, mix;
    for (usize i = 0; i < layers_.size(); ++i) {
        if (w[i] <= 0.0f) continue;
        const Layer& l = layers_[i];
        if (l.isBlendSpace) {
            l.bs.evaluate(skel, tmp);
        } else {
            restPose(skel, tmp);
            if (l.clip) sampleAnimation(*l.clip, l.time, tmp);
        }
        if (acc <= 0.0f) { out = tmp; acc = w[i]; continue; }
        blendPose(out, tmp, w[i] / (acc + w[i]), mix);
        out = mix;
        acc += w[i];
    }
}

void AnimStateMachine::drainEvents(std::vector<AsmEvent>& out) {
    for (AsmEvent& e : events_) out.push_back(std::move(e));
    events_.clear();
}

std::string AnimStateMachine::activePath() const {
    std::string s;
    for (const PathEl& e : path_) {
        if (!s.empty()) s += '/';
        s += stateAt(e).name;
    }
    return s;
}

std::string AnimStateMachine::activeState() const {
    return path_.empty() ? std::string{} : stateAt(path_.back()).name;
}

} // namespace aver::anim
