// The slot / layered / additive pipeline. See AnimPipeline.hpp.
#include "aver/anim/AnimPipeline.hpp"
#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

f32 clipClock(const fmt::OcAnimation& c, f32 t, bool loop) {
    if (!(c.duration > 0.0f)) return 0.0f;
    if (loop) {
        f32 w = std::fmod(t, c.duration);
        return w < 0.0f ? w + c.duration : w;
    }
    return t < 0.0f ? 0.0f : (t > c.duration ? c.duration : t);
}

f32 paramOr(const ParamFn& fn, const std::string& name, f32 fallback) {
    if (!fn || name.empty()) return fallback;
    const f32 v = fn(name);
    return std::isnan(v) ? fallback : v;
}

// "morph.x" / "pose.x" -> kind and the hash of the bare name. False for any other name.
bool resolvePipeCurve(const std::string& name, u32& kind, u64& id) {
    const std::string_view m = fmt::kOcCurvePrefixMorph, p = fmt::kOcCurvePrefixPose;
    const std::string_view nm = name;
    if (nm.size() > m.size() && nm.substr(0, m.size()) == m) { kind = kPoseCurveMorph; id = fnv1a64(nm.substr(m.size())); return true; }
    if (nm.size() > p.size() && nm.substr(0, p.size()) == p) { kind = kPoseCurveFace;  id = fnv1a64(nm.substr(p.size())); return true; }
    return false;
}

} // namespace

void AnimPipeline::bind(const std::vector<PipeNode>* nodes, const fmt::OcSkeleton* skel, AsmClipFn clips,
                        AsmBlendSpaceFn spaces) {
    nodes_ = nodes;
    skel_ = skel;
    clips_ = std::move(clips);
    spaces_ = std::move(spaces);
    rt_.clear();
    consumed_.clear();
    rootW_.clear();
    rootBone_ = skel ? rootBoneOf(*skel) : kNoBone;
    if (!nodes_ || !skel_) return;

    // A bone name the skeleton lacks is the ordinary case for a pipeline written for another rig:
    // not an error, warned once per node list.
    static std::unordered_set<const void*> warned;
    rt_.resize(nodes_->size());
    consumed_.assign(nodes_->size(), 0);
    for (usize i = 0; i < nodes_->size(); ++i) {
        const PipeNode& n = (*nodes_)[i];
        Rt& rt = rt_[i];
        if (n.kind == PipeNodeKind::Slot) { rt.slotHash = fnv1a64(std::string_view(n.slot)); continue; }
        if (n.kind == PipeNodeKind::Curve) { resolvePipeCurve(n.curve, rt.curveKind, rt.curveId); continue; }

        if (n.kind == PipeNodeKind::Layered) {
            const u32 matched = buildBranchMask(*skel_, n.branches, rt.mask);
            if (matched < n.branches.size() && warned.insert(nodes_).second)
                AVER_WARN("[AnimPipeline] a layered node names bones this skeleton does not have ({}/{} matched)",
                          matched, n.branches.size());
        }
        switch (n.source.kind) {
            case PipeSourceKind::Slot:
                rt.slotHash = fnv1a64(std::string_view(n.source.slot));
                break;
            case PipeSourceKind::Clip:
                rt.clip = clips_ ? clips_(n.source.asset) : nullptr;
                break;
            case PipeSourceKind::BlendSpace: {
                const BlendSpaceAsset* space = spaces_ ? spaces_(n.source.asset) : nullptr;
                if (!space || !space->valid()) break;
                std::vector<const fmt::OcAnimation*> cs;
                for (const BlendSample& smp : space->samples) cs.push_back(clips_ ? clips_(smp.clip) : nullptr);
                rt.space = space;
                rt.bs.bind(space, std::move(cs));
                rt.bs.setInput(0.0f, 0.0f);
                rt.bs.snapInput();
                rt.bs.advance(0.0f);
                break;
            }
            case PipeSourceKind::None: break;
        }

        if (n.kind != PipeNodeKind::Additive) continue;
        // The reference pose is cached here for rest and clip references; a blend-space first frame
        // needs the parameters, so it is taken on the first evaluate.
        if (n.ref.kind == PipeRefKind::Rest) {
            restPose(*skel_, rt.ref);
            rt.refReady = true;
        } else if (n.ref.kind == PipeRefKind::Clip) {
            rt.refClip = clips_ ? clips_(n.ref.asset) : nullptr;
            restPose(*skel_, rt.ref);
            if (rt.refClip) sampleAnimation(*rt.refClip, clipClock(*rt.refClip, n.ref.time, false), rt.ref);
            rt.refReady = true;
        } else if (n.source.kind == PipeSourceKind::Clip && rt.clip) {
            restPose(*skel_, rt.ref);
            sampleAnimation(*rt.clip, 0.0f, rt.ref);
            rt.refReady = true;
        } else if (n.source.kind != PipeSourceKind::BlendSpace) {
            restPose(*skel_, rt.ref);
            rt.refReady = true;
        }
    }
}

void AnimPipeline::tick(f32 dt, const ParamFn& param) {
    if (!nodes_) return;
    for (usize i = 0; i < nodes_->size() && i < rt_.size(); ++i) {
        const PipeNode& n = (*nodes_)[i];
        Rt& rt = rt_[i];
        if (n.kind == PipeNodeKind::Slot || n.kind == PipeNodeKind::Curve) continue;
        const f32 step = dt * n.source.speed;
        if (n.source.kind == PipeSourceKind::Clip) {
            rt.time += step;
            if (rt.clip && n.source.loop && rt.clip->duration > 0.0f && rt.time >= rt.clip->duration)
                rt.time = std::fmod(rt.time, rt.clip->duration);
        } else if (n.source.kind == PipeSourceKind::BlendSpace && rt.space) {
            rt.bs.setInput(paramOr(param, n.source.xParam, 0.0f), paramOr(param, n.source.yParam, 0.0f));
            rt.bs.advance(step);
        }
    }
}

void AnimPipeline::sourcePose(Rt& rt, const PipeNode& n, const SlotEvalFn& slot, const Pose& running,
                              bool ontoRunning, Pose& out, f32& coverage, bool& ok) {
    ok = false;
    coverage = 1.0f;
    switch (n.source.kind) {
        case PipeSourceKind::Slot:
            ok = slot && slot(rt.slotHash, running, out, coverage);
            break;
        case PipeSourceKind::Clip:
            if (!rt.clip) break;
            // Onto the running pose, so bones the clip does not animate keep what is under it.
            if (ontoRunning) out = running; else restPose(*skel_, out);
            sampleAnimation(*rt.clip, clipClock(*rt.clip, rt.time, n.source.loop), out);
            ok = true;
            break;
        case PipeSourceKind::BlendSpace:
            if (!rt.space) break;
            rt.bs.evaluate(*skel_, out);   // seeded from rest: a bone no sample animates reads rest
            ok = true;
            break;
        case PipeSourceKind::None: break;
    }
}

void AnimPipeline::evaluate(const ParamFn& param, const SlotEvalFn& slot, Pose& inOut) {
    std::fill(consumed_.begin(), consumed_.end(), 0);
    rootW_.clear();
    if (!nodes_ || !skel_ || rt_.size() != nodes_->size() || !inOut.matches(*skel_)) return;

    for (usize i = 0; i < nodes_->size(); ++i) {
        const PipeNode& n = (*nodes_)[i];
        Rt& rt = rt_[i];
        f32 w = n.weight;
        if (!n.weightParam.empty()) w *= paramOr(param, n.weightParam, 1.0f);

        if (n.kind == PipeNodeKind::Slot) {
            consumed_[i] = rt.slotHash;
            rootW_.push_back({rt.slotHash, 1.0f});
            f32 cov = 0.0f;
            if (slot && slot(rt.slotHash, inOut, src_, cov)) {
                // Overlay: the montage must not fade the base pose's face curves.
                blendPose(inOut, src_, clamp01(cov * w), mix_, CurveBlend::Overlay);
                inOut.swap(mix_);
            }
            continue;
        }

        if (n.kind == PipeNodeKind::Curve) {
            if (rt.curveId == 0) continue;
            const f32 v = paramOr(param, n.valueParam, n.value);
            const PoseCurve* cur = findPoseCurve(inOut, rt.curveKind, rt.curveId);
            const f32 old = cur ? cur->value : 0.0f;
            const f32 cw = clamp01(w);
            setPoseCurve(inOut, rt.curveKind, rt.curveId, n.curveAdd ? old + v * cw : old + (v - old) * cw);
            continue;
        }

        const bool isSlot = n.source.kind == PipeSourceKind::Slot;
        if (isSlot) consumed_[i] = rt.slotHash;

        if (n.kind == PipeNodeKind::Layered) {
            if (isSlot) {
                const f32 rootMask = rootBone_ < rt.mask.size() ? rt.mask[rootBone_] : 0.0f;
                rootW_.push_back({rt.slotHash, rootMask * clamp01(w)});
            }
            f32 cov = 1.0f;
            bool ok = false;
            sourcePose(rt, n, slot, inOut, true, src_, cov, ok);
            if (!ok || rt.mask.size() != inOut.local.size()) continue;
            perBone_.resize(rt.mask.size());
            for (usize b = 0; b < perBone_.size(); ++b) perBone_[b] = rt.mask[b] * w * cov;
            blendPoseMasked(inOut, src_, perBone_, mix_);
            inOut.swap(mix_);
            continue;
        }

        // Additive.
        f32 cov = 1.0f;
        bool ok = false;
        if (isSlot) {
            // The slot's montage applies its own additive rule; here it only needs weighting in.
            rootW_.push_back({rt.slotHash, 1.0f});
            sourcePose(rt, n, slot, inOut, true, src_, cov, ok);
            if (!ok) continue;
            blendPose(inOut, src_, clamp01(cov * w), mix_, CurveBlend::Overlay);
            inOut.swap(mix_);
            continue;
        }
        sourcePose(rt, n, slot, inOut, false, src_, cov, ok);
        if (!ok) continue;
        if (!rt.refReady) { rt.ref = src_; rt.refReady = true; }   // blend-space first frame
        addPose(inOut, src_, rt.ref, clamp01(w), mix_);
        inOut.swap(mix_);
    }
}

f32 AnimPipeline::rootWeight(u64 slotHash) const {
    for (const auto& r : rootW_)
        if (r.first == slotHash) return r.second;
    return 0.0f;
}

} // namespace aver::anim
