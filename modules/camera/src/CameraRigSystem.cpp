// CameraRigSystem: registration, attach, the inbox and the per-frame driver (select the live rig, blend view targets,
// apply the modifier stack). Evaluating one rig is CameraRigEval.cpp; the framework provider is CameraRigProvider.cpp.
#include "aver/camera/CameraRigSystem.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::camera {
namespace {

u32 g_type = 0;

constexpr const char* kEvViewTargetChanged = "CameraViewTargetChanged";
constexpr const char* kEvBlendFinished = "CameraBlendFinished";
constexpr const char* kEvCut = "CameraCut";
constexpr f32 kFallbackBlendS = 0.4f;

}  // namespace

u32 CameraRigSystem::typeId() { return g_type; }

CameraRigSystem& cameraRigSystem() {
    static CameraRigSystem s;
    return s;
}

u32 CameraRigSystem::registerComponents(scene::World& world) {
    using FK = scene::FieldKind;
    auto b = world.registerComponent<CCameraRig>("CCameraRig");
    auto f = [&](const char* name, FK kind, usize off, bool readOnly = false) {
        b.field(name, kind, static_cast<u16>(off), 0, readOnly);
    };
#define AVER_CAM_F(member, kind) f(#member, FK::kind, offsetof(CCameraRig, member))
    AVER_CAM_F(flags, I32);
    AVER_CAM_F(mode, I32);
    AVER_CAM_F(preset, I32);
    AVER_CAM_F(target, Entity);
    AVER_CAM_F(ignoreBody, I32);
    AVER_CAM_F(armLengthCm, F32);
    AVER_CAM_F(minArmCm, F32);
    AVER_CAM_F(probeRadiusCm, F32);
    AVER_CAM_F(collisionMask, I32);
    AVER_CAM_F(recoverHalfLifeS, F32);
    AVER_CAM_F(socketX, F32);
    AVER_CAM_F(socketY, F32);
    AVER_CAM_F(socketZ, F32);
    AVER_CAM_F(pivotX, F32);
    AVER_CAM_F(pivotY, F32);
    AVER_CAM_F(pivotZ, F32);
    AVER_CAM_F(posLagSpeed, F32);
    AVER_CAM_F(lagMaxDistCm, F32);
    AVER_CAM_F(rotLagSpeed, F32);
    AVER_CAM_F(rotLagMaxDeg, F32);
    AVER_CAM_F(lagMaxStepS, F32);
    AVER_CAM_F(lookAheadS, F32);
    AVER_CAM_F(lookAheadMaxCm, F32);
    AVER_CAM_F(pitchMinDeg, F32);
    AVER_CAM_F(pitchMaxDeg, F32);
    AVER_CAM_F(yawDeg, F32);
    AVER_CAM_F(pitchDeg, F32);
    AVER_CAM_F(recenterDelayS, F32);
    AVER_CAM_F(blendTimeS, F32);
    AVER_CAM_F(blendCurve, I32);
    AVER_CAM_F(fovKickDeg, F32);
    AVER_CAM_F(fovKickSpeedCmS, F32);
    AVER_CAM_F(fadeStartCm, F32);
    AVER_CAM_F(fadeEndCm, F32);
    AVER_CAM_F(bobAmount, F32);
    AVER_CAM_F(seatYawLimitDeg, F32);
    AVER_CAM_F(seatPitchLimitDeg, F32);
#undef AVER_CAM_F
    f("outArmCm", FK::F32, offsetof(CCameraRig, outArmCm), true);
    f("outState", FK::I32, offsetof(CCameraRig, outState), true);
    f("outFovDeg", FK::F32, offsetof(CCameraRig, outFovDeg), true);
    AVER_ASSERTM(b.verify(sizeof(CCameraRig)), "CCameraRig");
    g_type = b.typeId();
    return g_type;
}

CCameraRig* CameraRigSystem::attach(scene::World& world, scene::Entity e, CameraPreset preset, scene::Entity target) {
    if (g_type == 0 || !world.valid(e)) return nullptr;
    auto* c = static_cast<CCameraRig*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    *c = presetRig(preset);
    c->target = target;
    if (!world.hasComponent(e, scene::kComponentCamera)) {
        // CCamera's own defaults (5 / 500000) are not today's camera; a rig starts from the constants pushCamera used.
        if (auto* cam = static_cast<scene::CCamera*>(world.addComponent(e, scene::kComponentCamera))) {
            cam->fovYRad = radians(60.0f);
            cam->nearCm = 2.0f;
            cam->farCm = 200000.0f;
            cam->priority = 0;
        }
    }
    return c;
}

void CameraRigSystem::install(scene::World& world) { world_ = &world; }

void CameraRigSystem::uninstall() {
    clear();
    world_ = nullptr;
}

void CameraRigSystem::rebase(const Vec3& d) {
    for (auto& [e, st] : states_) {
        st.lagPivot -= d;
        st.prevTarget -= d;
        st.lead.lastPivot -= d;           // vel and lead are a rate and an offset: frame-independent
        st.lastTargetPos -= d;
        st.lastPov.pos -= d;
    }
    for (Inbox& op : inbox_)
        if (op.kind == Inbox::Kind::Shake) op.pos -= d;
    lastOut_.pos -= d;
    lastFinite_.pos -= d;
    from_.pos -= d;
    lastExternal_.pos -= d;
    debug_.pivot -= d;
    debug_.laggedPivot -= d;
    debug_.desiredEnd -= d;
    debug_.camera -= d;
    debug_.hit -= d;
}

void CameraRigSystem::clear() {
    inbox_.clear();
    mods_.reset();
    states_.clear();
    scratch_.clear();
    explicit_ = Selection::Auto;
    explicitRig_ = scene::kInvalidEntity;
    hasPendingBlend_ = false;
    current_ = scene::kInvalidEntity;
    haveSource_ = false;
    haveLast_ = false;
    haveFinite_ = false;
    blending_ = false;
    blendT_ = blendDur_ = 0;
    blendAlphaNow_ = 0;
    externalWas_ = false;
    events_.clear();
    frame_ = 0;
    possessed_ = scene::kInvalidEntity;
    haveDebug_ = false;
    warnedAttach_ = false;
    if (bobActive_ && bobSink_) bobSink_(0.0f, 0.0f);
    bobActive_ = false;
}

// ---- the inbox -------------------------------------------------------------------------------------------------

bool CameraRigSystem::setViewTarget(scene::Entity e, ViewBlend b) {
    if (!world_ || g_type == 0) return false;
    if (e != scene::kInvalidEntity && (!world_->valid(e) || !world_->hasComponent(e, g_type))) return false;
    Inbox op;
    op.kind = Inbox::Kind::SetView;
    op.entity = e;
    op.blend = b;
    inbox_.push_back(op);
    return true;
}

bool CameraRigSystem::clearViewTarget(ViewBlend b) {
    if (!world_ || g_type == 0) return false;
    Inbox op;
    op.kind = Inbox::Kind::ClearView;
    op.blend = b;
    inbox_.push_back(op);
    return true;
}

bool CameraRigSystem::cut(scene::Entity e) {
    if (!world_ || g_type == 0) return false;
    if (e != scene::kInvalidEntity && (!world_->valid(e) || !world_->hasComponent(e, g_type))) return false;
    Inbox op;
    op.kind = Inbox::Kind::Cut;
    op.entity = e;
    inbox_.push_back(op);
    return true;
}

u32 CameraRigSystem::pushModifier(const CameraModifier& m) {
    const u32 id = mods_.push(m);
    if (id == 0) AVER_WARN("[Camera] the modifier stack is full ({} entries); the push was refused", kMaxModifiers);
    return id;
}

bool CameraRigSystem::removeModifier(u32 id, f32 outS) { return mods_.remove(id, outS); }
u32 CameraRigSystem::clearModifiers(u32 tag) { return mods_.clear(tag); }

void CameraRigSystem::shakeAt(Vec3 pos, f32 radiusCm, f32 intensity, f32 falloffPower) {
    Inbox op;
    op.kind = Inbox::Kind::Shake;
    op.pos = pos;
    op.radius = radiusCm;
    op.intensity = intensity;
    op.falloff = falloffPower;
    inbox_.push_back(op);
}

void CameraRigSystem::applyInbox(scene::World&) {
    for (const Inbox& op : inbox_) {
        switch (op.kind) {
            case Inbox::Kind::SetView:
                if (op.entity == scene::kInvalidEntity) { explicit_ = Selection::Legacy; explicitRig_ = scene::kInvalidEntity; }
                else { explicit_ = Selection::Rig; explicitRig_ = op.entity; }
                pendingBlend_ = op.blend;
                hasPendingBlend_ = true;
                break;
            case Inbox::Kind::ClearView:
                explicit_ = Selection::Auto;
                explicitRig_ = scene::kInvalidEntity;
                pendingBlend_ = op.blend;
                hasPendingBlend_ = true;
                break;
            case Inbox::Kind::Cut: {
                const scene::Entity e = op.entity != scene::kInvalidEntity ? op.entity : current_;
                if (e != scene::kInvalidEntity) states_[e].snapNext = true;
                break;
            }
            case Inbox::Kind::Shake: {
                if (!shakeSink_ || !haveFinite_ || op.radius <= 0.0f || op.intensity <= 0.0f) break;
                const f32 d = dist(lastFinite_.pos, op.pos);
                const f32 falloff = 1.0f - clamp01(d / op.radius);
                if (falloff <= 0.0f) break;
                const f32 trauma = op.intensity * std::pow(falloff, op.falloff > 0.0f ? op.falloff : 1.0f);
                if (trauma > 0.0f) shakeSink_(trauma > 1.0f ? 1.0f : trauma);
                break;
            }
        }
    }
    inbox_.clear();
}

// ---- reads -------------------------------------------------------------------------------------------------------

bool CameraRigSystem::rigState(scene::Entity e, f32& armCm, u32& state) const {
    if (!world_ || g_type == 0 || !world_->valid(e)) return false;
    const auto* r = world_->component<CCameraRig>(e, g_type);
    if (!r) return false;
    armCm = r->outArmCm;
    state = r->outState;
    return true;
}

bool CameraRigSystem::debug(scene::Entity rig, DebugView& out) const {
    if (!haveDebug_ || debug_.rig != rig) return false;
    out = debug_;
    return true;
}

// ---- events ------------------------------------------------------------------------------------------------------

void CameraRigSystem::raise(scene::Entity e, const char* name) { events_.push_back(Event{e, name}); }

void CameraRigSystem::flushEvents() {
    if (events_.empty()) return;
    if (notifyFn_) {
        // A sink may call back into the system (a graph that sets a view target); iterate a copy of the count.
        for (usize i = 0; i < events_.size(); ++i) {
            const Event ev = events_[i];
            notifyFn_(ev.e, ev.name, notifyUser_);
        }
    }
    events_.clear();
}

// ---- selection ---------------------------------------------------------------------------------------------------

bool CameraRigSystem::rigUsable(scene::World& w, scene::Entity e) {
    if (e == scene::kInvalidEntity || !w.valid(e)) return false;
    const auto* r = w.component<CCameraRig>(e, g_type);
    if (!r || (r->flags & kRigDisabled)) return false;
    const auto it = states_.find(e);
    return it == states_.end() || it->second.deadFor <= options_.holdDestroyedS;
}

scene::Entity CameraRigSystem::selectAuto(scene::World& w, const RigFrame& fr) {
    if (!w.valid(fr.possessed)) return scene::kInvalidEntity;
    scene::Entity best = scene::kInvalidEntity;
    i32 bestPri = 0;
    for (const scene::Entity e : scratch_) {                      // ascending ids: ties keep the lowest
        if (!rigUsable(w, e)) continue;
        const auto* r = w.component<CCameraRig>(e, g_type);
        bool under = e == fr.possessed || (r->target != 0 && r->target == fr.possessed);
        for (scene::Entity p = w.parent(e), hops = 0; !under && p != scene::kInvalidEntity && hops < 16; p = w.parent(p), ++hops)
            under = p == fr.possessed;
        if (!under) continue;
        const auto* cam = w.component<scene::CCamera>(e, scene::kComponentCamera);
        const i32 pri = cam ? cam->priority : 0;
        if (best == scene::kInvalidEntity || pri > bestPri) { best = e; bestPri = pri; }
    }
    return best;
}

void CameraRigSystem::startBlend(f32 seconds, BlendCurve curve, bool cutNow, const CameraPov* from) {
    if (cutNow || seconds <= 0.0f || (!haveLast_ && from == nullptr)) {
        blending_ = false;
        blendAlphaNow_ = 1.0f;
        raise(current_ != scene::kInvalidEntity ? current_ : possessed_, kEvCut);
        return;
    }
    from_ = from ? *from : lastOut_;
    blending_ = true;
    blendT_ = 0;
    blendDur_ = seconds;
    blendCurve_ = curve;
    blendAlphaNow_ = 0;
}

// ---- the driver ----------------------------------------------------------------------------------------------------

RigResult CameraRigSystem::drive(scene::World& world, const RigFrame& fr) {
    RigResult res;
    ++frame_;
    if (!world_) world_ = &world;
    possessed_ = fr.possessed;
    applyInbox(world);
    if (g_type == 0) return res;

    scene::ComponentPool* pool = world.pool(g_type);
    const bool anyRig = pool && pool->size() > 0;

    // The cost of a project with no rigs: remember the host's pose (a rig that appears later blends from it) and stop.
    if (!anyRig && mods_.count() == 0 && !blending_ && !externalWas_) {
        lastOut_ = fr.externalActive ? fr.external : fr.fallback;
        haveLast_ = true;
        haveFinite_ = false;
        if (current_ != scene::kInvalidEntity) { current_ = scene::kInvalidEntity; states_.clear(); }
        if (bobActive_ && bobSink_) { bobSink_(0.0f, 0.0f); bobActive_ = false; }
        flushEvents();
        return res;
    }

    // One-shots, over every rig in ascending id order.
    scratch_.clear();
    if (pool)
        for (usize i = 0; i < pool->size(); ++i) {
            const scene::Entity e = pool->entityAt(i);
            if (world.valid(e)) scratch_.push_back(e);
        }
    std::sort(scratch_.begin(), scratch_.end());
    for (const scene::Entity e : scratch_) {
        auto* r = world.component<CCameraRig>(e, g_type);
        if (!r) continue;
        if (r->flags & kRigApplyPreset) {
            const CCameraRig p = presetRig(r->preset < kCameraPresetCount ? static_cast<CameraPreset>(r->preset) : CameraPreset::Chase);
            const scene::Entity keepTarget = r->target;
            const i32 keepBody = r->ignoreBody;
            const u32 keepFlags = r->flags & kRigDisabled;
            *r = p;
            r->target = keepTarget;
            r->ignoreBody = keepBody;
            r->flags = p.flags | keepFlags;
            if (!world.hasComponent(e, scene::kComponentCamera)) {
                if (auto* cam = static_cast<scene::CCamera*>(world.addComponent(e, scene::kComponentCamera))) {
                    cam->fovYRad = radians(60.0f); cam->nearCm = 2.0f; cam->farCm = 200000.0f; cam->priority = 0;
                }
            }
        }
        if (r->flags & kRigCut) {
            states_[e].snapNext = true;
            r->flags &= ~kRigCut;
        }
    }
    // Drop runtime for rigs that no longer exist.
    for (auto it = states_.begin(); it != states_.end();) {
        if (!world.valid(it->first) || !world.hasComponent(it->first, g_type)) it = states_.erase(it);
        else ++it;
    }

    // The modifier stack, once per frame.
    const ModResult mods = mods_.evaluate(fr.dt, fr.motionAllowed);

    // Which rig is live.
    scene::Entity want = scene::kInvalidEntity;
    if (explicit_ == Selection::Rig) {
        if (rigUsable(world, explicitRig_)) want = explicitRig_;
        else { explicit_ = Selection::Auto; explicitRig_ = scene::kInvalidEntity; want = selectAuto(world, fr); }
    } else if (explicit_ == Selection::Auto) {
        want = selectAuto(world, fr);
    }

    bool cutNow = false;
    const bool firstDecision = !haveSource_;
    const bool sourceChanged = firstDecision ? want != scene::kInvalidEntity || hasPendingBlend_ : want != current_;
    if (sourceChanged) {
        if (current_ != scene::kInvalidEntity && world.valid(current_)) {
            if (auto* old = world.component<CCameraRig>(current_, g_type)) old->outState &= ~(kRigStateActive | kRigStateBlending);
        }
        current_ = want;
        ViewBlend vb;
        if (hasPendingBlend_) vb = pendingBlend_;
        else if (want != scene::kInvalidEntity) {
            const auto* r = world.component<CCameraRig>(want, g_type);
            vb.seconds = r->blendTimeS;
            vb.curve = curveOf(r->blendCurve);
        } else {
            vb.seconds = kFallbackBlendS;
        }
        const bool cutWanted = vb.cut || !haveLast_;
        raise(current_ != scene::kInvalidEntity ? current_ : fr.possessed, kEvViewTargetChanged);
        startBlend(vb.seconds, vb.curve, cutWanted, nullptr);
        cutNow = !blending_;
    }
    haveSource_ = true;
    hasPendingBlend_ = false;

    // Evaluate the live rig, or take the host's fallback pose.
    RigState* st = nullptr;
    EvalOut ev;
    bool haveLive = false;
    CameraPov live = fr.fallback;
    if (current_ != scene::kInvalidEntity) {
        auto* rig = world.component<CCameraRig>(current_, g_type);
        st = &states_[current_];
        if (st->lastFrame + 1 != frame_ && st->lastFrame != 0) {
            // The rig was not live last frame: its follow state is stale.
            st->lagValid = false; st->lead = LeadState{}; st->rotValid = false; st->arm = -1.0f; st->faded = false;
            st->modeValid = false; st->kick = 0; st->targetPosValid = false; st->lookValid = false; st->deadFor = 0; st->speed = 0;
        }
        if (rig) {
            if (!sourceChanged && st->modeValid && st->lastMode != rig->mode) {
                // A mode change on the live rig blends over its own blend time.
                startBlend(rig->blendTimeS, curveOf(rig->blendCurve), false, nullptr);
                cutNow = cutNow || !blending_;
            }
            st->lastMode = rig->mode;
            st->modeValid = true;
            haveLive = evaluate(world, current_, *rig, *st, fr, mods, true, ev);
            st->lastFrame = frame_;
            if (haveLive) live = ev.pov;
        }
    }

    // The sequencer camera: remember its pose; when it lets go, blend back from it.
    if (fr.externalActive) {
        lastExternal_ = fr.external;
        externalWas_ = true;
    } else if (externalWas_) {
        externalWas_ = false;
        if (options_.externalBlendS > 0.0f) {
            from_ = lastExternal_;
            blending_ = true;
            blendT_ = 0;
            blendDur_ = options_.externalBlendS;
            blendCurve_ = BlendCurve::EaseInOut;
            blendAlphaNow_ = 0;
        }
    }

    // Blend into the live pose.
    CameraPov pov = live;
    if (blending_) {
        blendT_ += fr.dt > 0.0f ? fr.dt : 0.0f;
        const f32 t01 = blendDur_ > 0.0f ? blendT_ / blendDur_ : 1.0f;
        if (t01 >= 1.0f) {
            blending_ = false;
            blendAlphaNow_ = 1.0f;
            raise(current_ != scene::kInvalidEntity ? current_ : fr.possessed, kEvBlendFinished);
        } else {
            blendAlphaNow_ = camera::blendAlpha(t01, blendCurve_);
            pov = blendPov(from_, live, blendAlphaNow_);
        }
    } else {
        blendAlphaNow_ = 1.0f;
    }
    CameraPov blended = pov;

    // The modifier stack acts on the blended pose, so a recoil never fights a transition.
    if (mods.fovAddRad != 0.0f || mods.fovScale != 1.0f) pov.fovYRad = composeFov(pov.fovYRad, 0.0f, mods.fovAddRad, mods.fovScale);
    if (mods.punchPitch != 0.0f || mods.punchYaw != 0.0f || mods.roll != 0.0f) {
        const f32 lim = radians(89.0f);
        pov.pitch = std::fmax(-lim, std::fmin(lim, pov.pitch + mods.punchPitch));
        pov.yaw = wrapPi(pov.yaw + mods.punchYaw);
        pov.roll += mods.roll;
    }

    // Non-finite guard: keep the previous finite pose.
    if (!isFinite(pov) || !isFinite(blended)) {
        if (st && !st->warnedNonFinite) {
            st->warnedNonFinite = true;
            AVER_WARN("[Camera] rig {} produced a non-finite pose; holding the previous one", current_);
        }
        pov = haveFinite_ ? lastFinite_ : fr.fallback;
        blended = haveLast_ ? lastOut_ : fr.fallback;
    }
    lastOut_ = blended;
    haveLast_ = true;
    lastFinite_ = pov;
    haveFinite_ = true;

    // OUT fields and the bob feed.
    if (current_ != scene::kInvalidEntity && haveLive) {
        if (auto* rig = world.component<CCameraRig>(current_, g_type)) {
            rig->outArmCm = ev.arm;
            rig->outState = ev.state | kRigStateActive | (blending_ ? kRigStateBlending : 0u);
            rig->outFovDeg = degrees(pov.fovYRad);
            const CameraMode mode = modeOf(*rig);
            const bool bobMode = mode == CameraMode::FirstPerson || mode == CameraMode::Seat;
            if (bobSink_) {
                if (bobMode && rig->bobAmount > 0.0f) {
                    bobSink_(fr.motionAllowed ? rig->bobAmount : 0.0f, fr.motionAllowed && st ? st->speed : 0.0f);
                    bobActive_ = true;
                } else if (bobActive_) {
                    bobSink_(0.0f, 0.0f);
                    bobActive_ = false;
                }
            }
        }
        debug_ = ev.dbg;
        debug_.rig = current_;
        haveDebug_ = true;
    } else {
        if (bobActive_ && bobSink_) { bobSink_(0.0f, 0.0f); bobActive_ = false; }
        haveDebug_ = false;
    }

    const bool snapCut = haveLive && ev.cut;
    res.cut = cutNow || snapCut;
    if (snapCut) raise(current_ != scene::kInvalidEntity ? current_ : fr.possessed, kEvCut);

    res.pov = pov;
    // Nothing live, nothing blending, no modifier: the host's own pose stands, untouched.
    res.active = !fr.externalActive && (haveLive || blending_ || mods_.count() > 0);
    if (haveLive) {
        res.ownerHideRoot = ev.hideRoot;
        res.hideAll = ev.hideAll;
    }
    flushEvents();
    return res;
}

}  // namespace aver::camera
