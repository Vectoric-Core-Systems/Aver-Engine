// MorphSystemTest -- CMorphTargets on entities through the real AnimSystem tick, with no scripting host and no GPU.
//
// What it asserts is what a face would show: weights from a clip's morph. curves on an unskinned entity and on a skinned
// character, the order the layers combine in (animation, defaults, lip-sync, sequencer, script, master), blend ramps,
// declared targets filtering and defaulting the frame, children inheriting a parent, lip-sync tracks and queued visemes,
// face-rig poses reaching both blend shapes and bones, the provider the framework relays to, and the entity-death
// guarantees. docs/plans/wave9/MORPH_TARGETS.md section 5.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/FaceRig.hpp"
#include "aver/anim/MorphSystem.hpp"
#include "aver/anim/Viseme.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static void nearEq(f32 got, f32 want, f32 eps, const std::string& what) {
    check(std::fabs(got - want) <= eps, what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

static u64 H(const char* s) { return fnv1a64(std::string_view(s)); }

static std::string g_dir;
static const u64 kFaceClip = 0x3A00AA01ull;   // a curve-only clip: morph.jawOpen 0 -> 1 over a second
static const u64 kTalkClip = 0x3A00AA02ull;   // a bone track plus morph.smile and pose.open curves
static const u64 kSkelId   = 0x3A00AA03ull;
static const u64 kRigId    = 0x3A00AA04ull;
static const u64 kTrackId  = 0x3A00AA05ull;   // an .ocvis: "aa" from 0 to 1 s
static const u64 kBadId    = 0x3A00AA06ull;

static std::string resolvePath(u64 id, void*) {
    switch (id) {
        case kFaceClip: return g_dir + "/face.ocanim";
        case kTalkClip: return g_dir + "/talk.ocanim";
        case kSkelId:   return g_dir + "/head.ocskel";
        case kRigId:    return g_dir + "/hero.ocface";
        case kTrackId:  return g_dir + "/hello.ocvis";
        case kBadId:    return g_dir + "/missing.ocface";
        default: return {};
    }
}

static fmt::OcCurve curve(const char* name, std::vector<f32> t, std::vector<f32> v) {
    fmt::OcCurve c;
    c.name = name;
    c.times = std::move(t);
    c.values = std::move(v);
    return c;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Morph targets on entities");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-morph-scene-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    std::string why;
    {   // a curve-only clip
        fmt::OcAnimation c;
        c.duration = 1.0f;
        c.flags = fmt::kOcAnimLoop;
        c.curves.push_back(curve("morph.jawOpen", {0.0f, 1.0f}, {0.0f, 1.0f}));
        check(fmt::saveOcAnim(g_dir + "/face.ocanim", c, &why), "(face clip written) " + why);
    }
    {   // a skeleton: root and a jaw
        fmt::OcSkeleton s;
        fmt::OcBone root, jaw;
        root.name = "root";
        jaw.name = "jaw"; jaw.parent = 0; jaw.translation = Vec3{0, 5, 0};
        s.bones = {root, jaw};
        s.rootBone = 0;
        check(fmt::saveOcSkel(g_dir + "/head.ocskel", s, &why), "(skeleton written) " + why);
    }
    {   // a talking clip: a bone track plus a blend shape curve and a face-pose curve
        fmt::OcAnimation c;
        c.duration = 1.0f;
        c.flags = fmt::kOcAnimLoop;
        fmt::OcTrack t;
        t.boneIndex = 1;
        t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 1.0f};
        t.values = {0, 5, 0, 0, 5, 0};
        c.tracks.push_back(t);
        c.curves.push_back(curve("morph.smile", {0.0f, 1.0f}, {0.5f, 0.5f}));
        c.curves.push_back(curve("pose.open", {0.0f, 1.0f}, {1.0f, 1.0f}));
        check(fmt::saveOcAnim(g_dir + "/talk.ocanim", c, &why), "(talk clip written) " + why);
    }
    {   // the face rig: "open" is jawOpen plus a quarter turn of the jaw bone
        anim::FaceRigAsset r;
        anim::FacePose p;
        p.name = "open";
        p.targets = {{"jawOpen", 1.0f}};
        anim::FaceJoint j;
        j.bone = "jaw";
        j.rotation = Quat::fromAxisAngle(Vec3{1, 0, 0}, 1.5707963f);
        p.joints = {j};
        r.poses = {p};
        check(anim::saveFaceRig(g_dir + "/hero.ocface", r, &why), "(face rig written) " + why);
    }
    {   // a lip-sync track
        anim::VisemeTrack t;
        t.set = anim::VisemeSet::Viseme;
        anim::VisemeEvent e;
        e.start = 0.0f; e.end = 1.0f; e.symbol = "aa";
        t.events = {e};
        check(anim::saveVisemeTrack(g_dir + "/hello.ocvis", t, &why), "(lip-sync track written) " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    anim::AnimSystem::registerComponents(w);
    anim::MorphSystem& ms = sys.morphs();
    const f32 dt = 0.25f;

    const auto face = [&](const char* name, bool component = true) {
        const scene::Entity e = w.create(name);
        w.addComponent(e, scene::kComponentMeshRenderer);
        if (component) anim::MorphSystem::attach(w, e);
        return e;
    };
    const auto weight = [&](scene::Entity e, const char* target) {
        f32 v = -9.0f;
        if (!ms.weightOf(e, H(target), v)) return -9.0f;
        return v;
    };
    const auto tick = [&](int n = 1) { for (int i = 0; i < n; ++i) { sys.tick(w, dt); w.flush(); } };

    AVER_INFO("=== the component ===");
    {
        const u32 id = w.componentId("CMorphTargets");
        check(id != 0 && w.componentVerified(id), "CMorphTargets registers with a verified field table");
        check(w.componentSize(id) == 24 && sizeof(anim::CMorphTargets) == 24, "24 bytes, padding-free");
        check(w.fieldId("CMorphTargets.faceRig") != 0 && w.fieldId("CMorphTargets.flags") != 0 && w.fieldId("CMorphTargets.weight") != 0,
              "its fields are registered by name (the sequencer and the framework write by name)");
        const scene::Entity e = w.create("raw");
        w.addComponent(e, scene::kComponentMeshRenderer);
        auto* raw = static_cast<anim::CMorphTargets*>(w.addComponent(e, id));
        check(raw && raw->weight == 0.0f && raw->flags == 0 && raw->faceRig == 0, "a raw attach is all zeros");
        auto* ok = anim::MorphSystem::attach(w, e, 0x55, anim::kMorphIgnoreAnim);
        check(ok && ok->weight == 1.0f && ok->faceRig == 0x55 && ok->flags == anim::kMorphIgnoreAnim, "attach() writes a fresh value: weight 1");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== animation: an unskinned face plays morph. curves from its own clip ===");
    {
        const scene::Entity e = face("anim");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kFaceClip;
        tick();
        nearEq(weight(e, "jawOpen"), a->time, 1e-4f, "the clip's curve reaches the frame (jawOpen = the clock)");
        check(a->time > 0.2f, "the animator clock advanced");
        const u32 rev = ms.frame(e)->revision;
        tick();
        check(ms.frame(e)->revision > rev && ms.frame(e)->moving, "a changing weight bumps the revision and sets moving");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== layers: animation, sequencer, script, master ===");
    {
        const scene::Entity e = face("layers");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kFaceClip;
        a->flags = scene::kAnimatorPaused;   // hold the clock at 0.4
        a->time = 0.4f;
        tick();
        nearEq(weight(e, "jawOpen"), 0.4f, 1e-4f, "paused clip: the animation layer alone");
        check(ms.setTarget(w, e, H("jawOpen"), 0.8f, 0.0f), "setTarget (script)");
        tick();
        nearEq(weight(e, "jawOpen"), 0.8f, 1e-5f, "a script override at full strength replaces the animation");
        check(ms.setTarget(w, e, H("jawOpen"), 0.2f, 0.0f, anim::MorphSystem::Layer::Sequencer, 42), "setTarget (sequencer, owner 42)");
        tick();
        nearEq(weight(e, "jawOpen"), 0.8f, 1e-5f, "the script layer sits above the sequencer layer");
        ms.clear(e, H("jawOpen"), 3, 0.0f);
        tick();
        nearEq(weight(e, "jawOpen"), 0.2f, 1e-5f, "clearing the script override leaves the sequencer's");
        ms.releaseOwner(42);
        tick();
        nearEq(weight(e, "jawOpen"), 0.4f, 1e-4f, "releasing the track's owner returns the animation");

        auto* c = w.component<anim::CMorphTargets>(e, anim::MorphSystem::typeId());
        c->weight = 0.5f;
        tick();
        nearEq(weight(e, "jawOpen"), 0.2f, 1e-4f, "the master weight scales every weight");
        c->weight = 0.0f;
        tick();
        nearEq(weight(e, "jawOpen"), 0.4f, 1e-4f, "a master weight of 0 reads as 1");
        c->flags = anim::kMorphOff;
        tick();
        check(ms.frame(e)->active.empty(), "kMorphOff zeroes the frame");
        c->flags = anim::kMorphIgnoreAnim;
        tick();
        check(ms.frame(e)->active.empty(), "kMorphIgnoreAnim stops reading the clip");
        c->flags = 0;
        tick();
        nearEq(weight(e, "jawOpen"), 0.4f, 1e-4f, "and clearing the flags brings the animation back");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== blend ramps (no animation: the base is 0) ===");
    {
        const scene::Entity e = face("ramp");
        check(ms.setTarget(w, e, H("jawOpen"), 1.0f, 1.0f), "ramp in over a second");
        tick();
        nearEq(weight(e, "jawOpen"), 0.25f, 1e-4f, "a quarter second in: a quarter strength");
        tick();
        nearEq(weight(e, "jawOpen"), 0.5f, 1e-4f, "half");
        tick(2);
        nearEq(weight(e, "jawOpen"), 1.0f, 1e-4f, "and whole after the second");
        check(ms.setTarget(w, e, H("jawOpen"), 0.0f, 1.0f), "ramp the VALUE back over a second");
        tick();
        nearEq(weight(e, "jawOpen"), 0.75f, 1e-4f, "value ramps linearly to the new value");
        ms.clear(e, 0, 3, 0.5f);
        tick();
        const f32 mid = weight(e, "jawOpen");
        check(mid < 0.75f, "clear with a blend fades the override out");
        tick(3);
        nearEq(weight(e, "jawOpen"), 0.0f, 1e-5f, "and it is gone after the blend");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== declared targets filter the frame and supply defaults ===");
    {
        const scene::Entity e = face("declared");
        check(ms.setTarget(w, e, H("jawOpen"), 0.6f, 0.0f) && ms.setTarget(w, e, H("notOnTheMesh"), 1.0f, 0.0f), "two overrides");
        tick();
        check(ms.frame(e)->active.size() == 2, "before any declaration everything is kept");
        const anim::MorphDecl decls[] = {{H("jawOpen"), 0.0f}, {H("smile"), 0.25f}};
        ms.declareTargets(e, decls);
        check(ms.declaredCount(e) == 2, "two targets declared");
        tick();
        nearEq(weight(e, "jawOpen"), 0.6f, 1e-5f, "a declared target keeps its weight");
        nearEq(weight(e, "smile"), 0.25f, 1e-5f, "an undriven declared target takes its default weight");
        nearEq(weight(e, "notOnTheMesh"), 0.0f, 0.0f, "an undeclared id is dropped once targets are declared");
        check(ms.frame(e)->active.size() == 2, "the frame is exactly the mesh's active set");
        bool sorted = true;
        for (usize i = 1; i < ms.frame(e)->active.size(); ++i)
            if (ms.frame(e)->active[i - 1].id >= ms.frame(e)->active[i].id) sorted = false;
        check(sorted, "sorted by id");
        const u32 rev = ms.frame(e)->revision;
        tick(2);
        check(ms.frame(e)->revision == rev && !ms.frame(e)->moving, "a steady frame keeps its revision and is not moving");
        ms.setTarget(w, e, H("smile"), 0.9f, 0.0f);
        tick();
        nearEq(weight(e, "smile"), 0.9f, 1e-5f, "an override beats the default");
        ms.declareTargets(e, {});
        check(ms.declaredCount(e) == 0, "an empty span forgets the declaration");
        ms.clearRuntime();
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== script-only entities (no animator) and the zero-fill rule ===");
    {
        const scene::Entity e = face("loose");
        check(!w.hasComponent(e, scene::kComponentAnimator), "no animator");
        ms.setTarget(w, e, H("blinkLeft"), 1.0f, 0.0f);
        tick();
        nearEq(weight(e, "blinkLeft"), 1.0f, 1e-5f, "tickLoose evaluates a CMorphTargets entity nothing else does");
        check(ms.evaluatedLastTick() >= 1, "evaluatedLastTick counts it");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a child inherits its parent even when it is evaluated first ===");
    {
        const scene::Entity child = face("child", false);
        const scene::Entity parent = face("parent");
        auto* cc = anim::MorphSystem::attach(w, child, 0, anim::kMorphInheritParent);
        check(cc != nullptr && w.setParent(child, parent), "child attached first, then parented");
        ms.setTarget(w, parent, H("jawOpen"), 0.6f, 0.0f);
        ms.setTarget(w, child, H("smile"), 0.3f, 0.0f);
        tick();
        nearEq(weight(parent, "jawOpen"), 0.6f, 1e-5f, "the parent's own frame");
        nearEq(weight(child, "jawOpen"), 0.6f, 1e-5f, "the child took the parent's final frame");
        nearEq(weight(child, "smile"), 0.3f, 1e-5f, "and keeps its own script weight on top");
        nearEq(weight(parent, "smile"), 0.0f, 1e-6f, "which the parent does not see");
        w.destroy(parent); w.flush();
    }

    AVER_INFO("=== lip-sync: a track, queued visemes, a slaved clock ===");
    {
        const scene::Entity e = face("talker");
        check(ms.lipSyncPlay(w, e, kTrackId, 1.0f, 0.0f, 1.0f), "play the .ocvis");
        tick(2);   // lip time 0.5: the viseme is whole
        nearEq(weight(e, "jawOpen"), 0.7f, 1e-3f, "viseme_aa drives jawOpen by the built-in composition");
        nearEq(weight(e, "mouthLowerDownLeft"), 0.3f, 1e-3f, "and the lower-lip shapes");
        f32 t = 0, len = 0, str = 0;
        bool playing = false;
        check(ms.lipSyncState(e, t, len, str, playing) && playing && std::fabs(t - 0.5f) < 1e-4f && std::fabs(len - 1.0f) < 1e-5f && str == 1.0f,
              "lipSyncState: playing, time 0.5, length 1");
        check(ms.lipSyncSetTime(e, 0.2f), "slave the clock");
        tick();
        check(ms.lipSyncState(e, t, len, str, playing) && std::fabs(t - 0.2f) < 1e-4f, "a set time is not also advanced that tick");
        ms.lipSyncStop(e, 0.5f);
        tick(1);
        const f32 fading = weight(e, "jawOpen");
        check(fading > 0.0f && fading < 0.7f, "stop fades the mouth out rather than cutting it");
        tick(3);
        nearEq(weight(e, "jawOpen"), 0.0f, 1e-4f, "and it is gone after the blend-out");
        check(ms.lipSyncState(e, t, len, str, playing) && !playing, "no longer playing");

        check(ms.lipSyncQueue(e, H("OW"), 0.0f, 0.5f, 1.0f), "queue a phoneme (OW -> viseme_oh)");
        check(!ms.lipSyncQueue(e, H("zzz-not-a-symbol"), 0.0f, 0.5f, 1.0f), "an unknown symbol is refused");
        tick();
        check(weight(e, "mouthFunnel") > 0.0f, "a queued viseme drives the mouth");
        check(ms.lipSyncState(e, t, len, str, playing) && playing && len == 0.0f, "queued-only: playing with length 0 (open-ended)");
        tick(4);
        check(ms.lipSyncState(e, t, len, str, playing) && !playing, "it ends itself when every event has expired");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a skinned character: pose curves, face-rig poses, bones ===");
    {
        const auto character = [&](const char* name, bool withComponent) {
            const scene::Entity e = w.create(name);
            w.addComponent(e, scene::kComponentMeshRenderer);
            auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
            sm->skeleton = kSkelId;
            auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
            a->clip = kTalkClip;
            if (withComponent) anim::MorphSystem::attach(w, e, kRigId);
            return e;
        };
        const scene::Entity plain = character("plainTalker", false);
        tick();
        check(ms.frame(plain) != nullptr, "weight curves on a skinned character work without the component");
        nearEq(weight(plain, "smile"), 0.5f, 1e-5f, "morph.smile reaches the frame");
        nearEq(weight(plain, "jawOpen"), 0.0f, 1e-6f, "pose.open names a pose only the rig has: the built-in rig leaves it alone");
        const anim::Pose* pp = sys.pose(plain);
        check(pp && pp->local.size() == 2 && std::fabs(pp->local[1].rotation.x) < 1e-4f, "the built-in pose moves no bone");

        const scene::Entity rigged = character("riggedTalker", true);
        tick();
        nearEq(weight(rigged, "smile"), 0.5f, 1e-5f, "the rigged character: morph.smile");
        nearEq(weight(rigged, "jawOpen"), 1.0f, 1e-5f, "and pose.open through the entity's own rig (jawOpen)");
        const anim::Pose* rp = sys.pose(rigged);
        check(rp && rp->local.size() == 2, "it has a pose");
        if (rp && rp->local.size() == 2) {
            nearEq(rp->local[1].rotation.x, std::sin(0.7853982f), 1e-3f, "the rig's joint delta reached the pose: the jaw turned a quarter about x");
            nearEq(rp->local[1].rotation.w, std::cos(0.7853982f), 1e-3f, "(w)");
            nearEq(rp->local[1].position.y, 5.0f, 1e-4f, "its position is untouched");
        }
        u32 boneCount = 0;
        check(sys.skinning(rigged, boneCount) != nullptr && boneCount == 2, "and skinning matrices were produced from the modified pose");

        // a script weight on top of the same character
        ms.setPose(w, rigged, H("open"), 0.0f, 0.0f);
        tick();
        nearEq(weight(rigged, "jawOpen"), 0.0f, 1e-5f, "a script pose weight of 0 closes the mouth");
        const anim::Pose* rp2 = sys.pose(rigged);
        nearEq(rp2->local[1].rotation.x, 0.0f, 1e-3f, "and the jaw bone with it");
        ms.clearRuntime();
        w.destroy(plain); w.destroy(rigged); w.flush();
    }

    AVER_INFO("=== the provider the framework relays to ===");
    {
        enum { SetTarget = 1, SetPose, Clear, GetWeight, SetRig, LipPlay, LipStop, LipTime, LipQueue, LipState, Info };
        const auto call = [&](i32 op, scene::Entity e, i64 h0, i64 h1, std::vector<f32> in, f32* outF = nullptr, i64* outI = nullptr) {
            return anim::morphProvider(op, static_cast<i32>(e), h0, h1, 0, in.data(), static_cast<i32>(in.size()), outF, outI, &ms);
        };
        const scene::Entity e = face("viaAbi", false);
        check(!w.hasComponent(e, anim::MorphSystem::typeId()), "no CMorphTargets yet");
        check(call(SetTarget, e, static_cast<i64>(H("jawOpen")), 0, {0.7f, 0.0f}) == 1, "SET_TARGET succeeds");
        check(w.hasComponent(e, anim::MorphSystem::typeId()), "and attached the component (the entity has a mesh)");
        tick();
        f32 out[3] = {};
        check(call(GetWeight, e, static_cast<i64>(H("jawOpen")), 0, {}, out) == 1 && std::fabs(out[0] - 0.7f) < 1e-5f, "GET_WEIGHT relays the final weight");
        check(call(SetPose, e, static_cast<i64>(H("viseme_aa")), 0, {1.0f, 0.0f}) == 1, "SET_POSE");
        tick();
        check(call(GetWeight, e, static_cast<i64>(H("mouthLowerDownLeft")), 0, {}, out) == 1 && out[0] > 0.2f, "a viseme pose expands to ARKit shapes");
        check(call(Clear, e, 0, 3, {0.0f}) == 1, "CLEAR everything");
        tick();
        check(call(GetWeight, e, static_cast<i64>(H("jawOpen")), 0, {}, out) == 1 && out[0] == 0.0f, "back to neutral");
        check(call(SetRig, e, static_cast<i64>(kRigId), 0, {}) == 1, "SET_RIG");
        i64 info[3] = {};
        f32 master = 0.0f;
        tick();
        check(call(Info, e, 0, 0, {}, &master, info) == 1 && info[2] == static_cast<i64>(kRigId) && info[1] == 1 && master == 1.0f,
              "INFO: rig hash, one rig pose, master weight 1");
        check(call(LipPlay, e, static_cast<i64>(kTrackId), 0, {0.0f, 0.0f, -1.0f}) == 1, "LIPSYNC_PLAY (rate 0 reads 1, strength <0 reads 1)");
        tick();
        check(call(LipState, e, 0, 0, {}, out) == 1 && out[1] > 0.9f, "LIPSYNC_STATE: playing, length ~1");
        check(call(LipTime, e, 0, 0, {0.1f}) == 1, "LIPSYNC_TIME");
        check(call(LipStop, e, 0, 0, {-1.0f}) == 1, "LIPSYNC_STOP (a negative blend reads 0.1 s)");
        check(call(LipQueue, e, static_cast<i64>(H("sil")), 0, {0.0f, 0.1f, 1.0f}) == 1, "LIPSYNC_QUEUE accepts the silence symbol");
        check(call(99, e, 0, 0, {}) == 0, "an unknown op is refused");
        check(anim::morphProvider(SetTarget, static_cast<i32>(e), 1, 0, 0, nullptr, 0, nullptr, nullptr, &ms) == 0, "a set with no floats is refused");
        check(anim::morphProvider(SetTarget, static_cast<i32>(e), 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr) == 0, "no user pointer, no provider");

        const scene::Entity bare = w.create("noMesh");
        check(call(SetTarget, bare, static_cast<i64>(H("jawOpen")), 0, {1.0f, 0.0f}) == 0, "an entity with no mesh is refused");
        w.destroy(e); w.destroy(bare); w.flush();
        check(call(SetTarget, e, static_cast<i64>(H("jawOpen")), 0, {1.0f, 0.0f}) == 0, "a dead entity is refused");
        check(call(SetRig, scene::kInvalidEntity, 0, 0, {}) == 0, "and an invalid handle");
    }

    AVER_INFO("=== entity death and recycled handles ===");
    {
        const scene::Entity e = face("mortal");
        ms.setTarget(w, e, H("jawOpen"), 1.0f, 0.0f);
        tick();
        check(ms.frame(e) != nullptr, "a frame exists");
        const u32 before = ms.trackedEntities();
        w.destroy(e);
        w.flush();
        tick();
        check(ms.frame(e) == nullptr && ms.trackedEntities() < before, "a destroyed entity's state is dropped on the next tick");
        const scene::Entity again = face("heir");
        tick();
        nearEq(weight(again, "jawOpen"), 0.0f, 1e-6f, "an entity that reuses the slot starts clean (keyed by the full handle)");
        w.destroy(again); w.flush();
    }

    AVER_INFO("=== clearRuntime and clear ===");
    {
        const scene::Entity e = face("reset");
        ms.setTarget(w, e, H("jawOpen"), 1.0f, 0.0f);
        ms.lipSyncPlay(w, e, kTrackId, 1.0f, 0.0f, 1.0f);
        tick();
        check(weight(e, "jawOpen") > 0.9f, "driven");
        ms.clearRuntime();
        check(ms.frame(e)->active.empty(), "clearRuntime empties the frames at once");
        tick();
        nearEq(weight(e, "jawOpen"), 0.0f, 1e-6f, "and the overrides and lip-sync do not come back");
        f32 t = 0, len = 0, str = 0;
        bool playing = true;
        ms.lipSyncState(e, t, len, str, playing);
        check(!playing, "lip-sync stopped");
        ms.setFaceRig(w, e, kBadId);
        tick();
        check(ms.frame(e) != nullptr, "a rig that fails to load falls back to the built-in rig without a crash");
        ms.clear();
        tick();
        check(ms.frame(e) != nullptr, "clear() drops loaded assets and the entity keeps working");
        w.destroy(e); w.flush();
    }

    if (g_failures == 0) AVER_INFO("MorphSystemTest: {} checks passed", g_checks);
    else                 AVER_ERROR("MorphSystemTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
