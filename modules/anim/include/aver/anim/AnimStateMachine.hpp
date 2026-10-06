// Animation state machines: states, conditional transitions with blend times, sub-machines and
// enter/exit events, driven by named parameters.
//
// Pure logic over asset structs and clip pointers, so transition timing is decidable with no
// scene and no GPU. Design notes and the asset format live in docs/ANIM_BLEND_STATE.md.
#pragma once

#include "aver/anim/BlendSpace.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::anim {

enum class AsmParamType : u8 { Float = 0, Int = 1, Bool = 2, Trigger = 3 };

struct AsmParam {
    std::string name;
    AsmParamType type = AsmParamType::Float;
    f32 def = 0.0f;
};

enum class AsmStateKind : u8 { Clip = 0, BlendSpace = 1, SubMachine = 2 };

struct AsmState {
    std::string name;
    AsmStateKind kind = AsmStateKind::Clip;
    std::string asset;            // clip (Clip) or .ocblend (BlendSpace) reference
    i32 subMachine = -1;          // index into AnimStateMachineAsset::machines (SubMachine)
    f32 speed = 1.0f;
    bool loop = true;
    std::string speedParam;       // optional float parameter multiplying speed
    std::string xParam, yParam;   // blend-space axis inputs
    std::string onEnter, onExit;  // event names raised on entry / exit
    f32 posX = 0.0f, posY = 0.0f; // graph editor placement
};

enum class AsmOp : u8 { Greater = 0, GreaterEq, Less, LessEq, Equal, NotEqual, IsTrue, IsFalse, Trigger };

struct AsmCondition {
    std::string param;
    AsmOp op = AsmOp::Greater;
    f32 value = 0.0f;
};

inline constexpr i32 kAsmAny = -1;    // transition source: any state of the machine
inline constexpr i32 kAsmExit = -2;   // transition target: leave the enclosing sub-machine

struct AsmTransition {
    i32 from = 0;
    i32 to = 0;
    std::vector<AsmCondition> conditions;   // all must hold
    f32 blendTime = 0.2f;
    bool hasExitTime = false;
    f32 exitTime = 0.9f;                    // normalised, cumulative over loops
    bool interruptible = true;              // false: no other transition while this one blends
    bool allowSelf = false;                 // from Any, may re-enter the current state
};

struct AsmMachine {
    std::string name;
    std::vector<AsmState> states;
    std::vector<AsmTransition> transitions; // order is priority
    i32 entry = 0;
};

struct AnimStateMachineAsset {
    std::string name;
    std::vector<AsmParam> params;
    std::vector<AsmMachine> machines;       // machines[0] is the root

    bool valid(std::string* why = nullptr) const;
    i32 paramIndex(const std::string& name) const;
};

struct AsmEvent {
    enum class Kind : u8 { Enter = 0, Exit = 1 };
    Kind kind = Kind::Enter;
    std::string machine;
    std::string state;
    std::string name;             // the state's onEnter/onExit; empty when it has none
};

using AsmClipFn = std::function<const fmt::OcAnimation*(const std::string&)>;
using AsmBlendSpaceFn = std::function<const BlendSpaceAsset*(const std::string&)>;

class AnimStateMachine {
public:
    // Enters the root machine's entry state. The asset and whatever the resolvers return must
    // outlive the instance.
    void bind(const AnimStateMachineAsset* asset, AsmClipFn clips, AsmBlendSpaceFn spaces);
    bool bound() const { return asset_ != nullptr; }

    void setFloat(const std::string& name, f32 v);
    void setInt(const std::string& name, i32 v);
    void setBool(const std::string& name, bool v);
    void setTrigger(const std::string& name);
    // By fnv1a64(name), the form the scene component carries. Triggers fire on a non-zero value.
    bool setByHash(u64 nameHash, f32 v);
    bool isTriggerHash(u64 nameHash) const {
        const auto it = byHash_.find(nameHash);
        return it != byHash_.end() && asset_->params[it->second].type == AsmParamType::Trigger;
    }
    f32 value(const std::string& name) const;
    bool triggerPending(const std::string& name) const;

    // Advances the states and the blend, then fires at most one transition.
    void tick(f32 dt);

    void evaluate(const fmt::OcSkeleton& skel, Pose& out) const;

    // Events since the last drain, in the order they happened.
    void drainEvents(std::vector<AsmEvent>& out);

    // "Root/Sub/Leaf" names of the active leaf, and its cumulative normalised time.
    std::string activePath() const;
    std::string activeState() const;
    f32 normalizedTime() const;
    bool blending() const { return layers_.size() > 1; }
    // Effective weight of each live layer, oldest first. Sums to one.
    std::vector<f32> layerWeights() const;
    std::vector<std::string> layerStates() const;
    u32 transitionsFired() const { return fired_; }

private:
    struct PathEl {
        i32 machine = 0;
        i32 state = 0;
        bool exited = false;      // a sub-machine whose inner machine took an Exit transition
    };
    struct Layer {
        i32 machine = 0;
        i32 state = 0;
        const fmt::OcAnimation* clip = nullptr;
        BlendSpacePlayer bs;
        bool isBlendSpace = false;
        f32 time = 0.0f;
        f32 elapsed = 0.0f;       // cumulative seconds, for the normalised exit time
        f32 duration = 1.0f;
        f32 alpha = 1.0f;
        f32 blendTime = 0.0f;
        f32 blendElapsed = 0.0f;
    };

    bool conditionsHold(const AsmTransition& t);
    bool exitTimeReached(const AsmTransition& t, usize level) const;
    void fire(usize level, const AsmTransition& t, i32 target);
    void pushLayer(i32 machine, i32 state, f32 blendTime);
    void descend(std::vector<PathEl>& path) const;
    void raise(AsmEvent::Kind kind, const PathEl& el);
    f32 paramByName(const std::string& n) const;
    const AsmState& stateAt(const PathEl& e) const { return asset_->machines[e.machine].states[e.state]; }

    const AnimStateMachineAsset* asset_ = nullptr;
    AsmClipFn clips_;
    AsmBlendSpaceFn spaces_;
    std::vector<f32> values_;
    std::vector<bool> triggers_;
    std::unordered_map<std::string, u32> index_;
    std::unordered_map<u64, u32> byHash_;
    std::vector<PathEl> path_;
    std::vector<Layer> layers_;
    std::vector<AsmEvent> events_;
    bool blockInterrupt_ = false;
    u32 fired_ = 0;
};

} // namespace aver::anim
