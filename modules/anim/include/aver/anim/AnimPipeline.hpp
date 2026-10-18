// The post-state-machine pipeline of an .ocasm: slot, layered-blend-per-bone, additive and curve nodes,
// applied in order on the machine's pose. The asset structs live here (AnimStateMachineAsset embeds
// them) and so does the runtime that walks them. Pure over poses and clip pointers.
// Design and schema: docs/ANIM_MONTAGES.md.
#pragma once

#include "aver/anim/BlendSpace.hpp"
#include "aver/anim/RootMotion.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::anim {

using AsmClipFn = std::function<const fmt::OcAnimation*(const std::string&)>;
using AsmBlendSpaceFn = std::function<const BlendSpaceAsset*(const std::string&)>;

// Curve: drives one pose curve ("morph.<target>" or "pose.<name>") from a constant or a machine parameter, with
// no source of its own (MORPH_TARGETS.md 2.4). An .ocasm using it is refused by older builds.
enum class PipeNodeKind : u8 { Slot = 0, Layered = 1, Additive = 2, Curve = 3 };
enum class PipeSourceKind : u8 { None = 0, Slot = 1, Clip = 2, BlendSpace = 3 };
enum class PipeRefKind : u8 { FirstFrame = 0, Rest = 1, Clip = 2 };

struct PipeSource {
    PipeSourceKind kind = PipeSourceKind::None;
    std::string slot, asset, xParam, yParam;
    bool loop = true;
    f32 speed = 1.0f;
};
struct PipeRef {
    PipeRefKind kind = PipeRefKind::FirstFrame;
    std::string asset;
    f32 time = 0.0f;
};
struct PipeNode {
    PipeNodeKind kind = PipeNodeKind::Slot;
    std::string slot;                       // Slot nodes
    PipeSource source;                      // Layered / Additive
    std::vector<BranchEntry> branches;      // Layered
    PipeRef ref;                            // Additive
    f32 weight = 1.0f;
    std::string weightParam;                // float parameter of the machine, multiplies `weight`
    // Curve nodes. value = valueParam's parameter when it names one (and the machine has it), else `value`.
    // set: curve = lerp(existing or 0, v, w). add: curve += v * w.
    std::string curve;                      // "morph.<target>" | "pose.<name>"
    std::string valueParam;
    f32 value = 0.0f;
    bool curveAdd = false;
};

// Supplied per evaluation by the host: the slot's montage pose over `base`, and its coverage 0..1.
// False when nothing plays in that slot.
using SlotEvalFn = std::function<bool(u64 slotHash, const Pose& base, Pose& out, f32& coverage)>;
// A machine parameter by name. NaN means "no such parameter": a weightParam reads that as 1.
using ParamFn = std::function<f32(const std::string&)>;

class AnimPipeline {
public:
    // The nodes and whatever the resolvers return must outlive the binding.
    void bind(const std::vector<PipeNode>* nodes, const fmt::OcSkeleton* skel, AsmClipFn clips,
              AsmBlendSpaceFn spaces);
    bool bound() const { return nodes_ != nullptr; }
    bool empty() const { return !nodes_ || nodes_->empty(); }

    // Advances the clip and blend-space source clocks.
    void tick(f32 dt, const ParamFn& param);
    // Runs the nodes in order on the running pose.
    void evaluate(const ParamFn& param, const SlotEvalFn& slot, Pose& inOut);

    // The slot hash node i consumed in the last evaluate, or 0.
    u64 consumedSlot(usize i) const { return i < consumed_.size() ? consumed_[i] : 0; }
    // How much of the root motion a slot's montage keeps: 1 for a slot node, mask[rootBone] * weight
    // for a layered one; 0 when no node consumed the slot.
    f32 rootWeight(u64 slotHash) const;

private:
    struct Rt {
        std::vector<f32> mask;                          // Layered: per-bone branch weights
        const fmt::OcAnimation* clip = nullptr;         // Clip source
        f32 time = 0.0f;
        const BlendSpaceAsset* space = nullptr;         // BlendSpace source
        BlendSpacePlayer bs;
        u64 slotHash = 0;
        u32 curveKind = 0;                              // Curve: pose-curve kind and id, resolved at bind
        u64 curveId = 0;
        const fmt::OcAnimation* refClip = nullptr;      // Additive, ref.kind == Clip
        Pose ref;                                       // Additive reference pose
        bool refReady = false;
    };

    void sourcePose(Rt& rt, const PipeNode& n, const SlotEvalFn& slot, const Pose& running,
                    bool ontoRunning, Pose& out, f32& coverage, bool& ok);

    const std::vector<PipeNode>* nodes_ = nullptr;
    const fmt::OcSkeleton* skel_ = nullptr;
    AsmClipFn clips_;
    AsmBlendSpaceFn spaces_;
    std::vector<Rt> rt_;
    std::vector<u64> consumed_;
    std::vector<std::pair<u64, f32>> rootW_;
    std::vector<f32> perBone_;
    Pose src_, mix_;                                    // scratch, so a tick allocates nothing after warm-up
    u32 rootBone_ = kNoBone;
};

} // namespace aver::anim
