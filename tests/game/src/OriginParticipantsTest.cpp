// OriginParticipantsTest: the per-system halves of a floating-origin shift (docs/plans/wave11/LARGE_WORLD.md 2.2.5, 5.2), each
// tested alone against its real system with the scene shifted by hand: World::shiftRoots stands in for the scene participant, the
// system's own rebase is the call under test, and where it matters a control repeats the scene without it and shows the failure the
// rebase prevents. No GPU, no device, no window. Optional systems compile in only when the build has them (ORIGINTEST_* from CMake).
//
//   PlayMobility   10,000 static entities stay Static after a shift and a flush (control: they all turn Movable)
//   AnimSystem     an object-animated root does not snap back to its old frame (control: it does)
//   Sequencer      the base store follows the shift; Transform / camera / CLocal.position keys are level-absolute f64 and are
//                  written as key - O (zero origin writes the sampler's output unchanged)
//   Camera rig     no spurious cut and no lag across the shift (control: a cut)
//   Soft body      cached cage / draw positions move; hair: the call is harmless
//   Kinematic      an animated kinematic body keeps being DRIVEN across the shift (control: it is teleported)
//   Synapse        nav origin, agent path / goal, hearing memory and queue, cover points, crowd agents and steer targets
//   Audio          a looping 3D voice keeps its pan and level when voices and listener shift together
//   WorldOrigin    the same pieces driven through the real service by fake participants in the plan's order: order of calls,
//                  absolute positions kept, a refusal changes nothing, PlayMobility stays Static, no rebase loop
//                  (control: a participant left out shows its own failure)
//   Real           game::OriginParticipants itself (Runtime/src/OriginParticipants.cpp, the order table of 2.2.4) against the real
//                  systems: the registration order, scene roots + PlayMobility + host members + a recording renderer stand-in +
//                  Jolt in one slot, every deferral rule (a level loading, the host's busy hook, the editor's gesture gate and its
//                  urgent override, the flat streamers the gate refuses), the project keys, resetForLevel, the save-skip predicate
//
// Exit code = failure count. Not run by the author: see section 9 of the plan.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/game/LevelSequence.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#if ORIGINTEST_ANIM
#include "aver/anim/AnimSystem.hpp"
#include "aver/formats/OcAnim.hpp"
#endif
#if ORIGINTEST_PHYSICS
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/world/LevelInstance.hpp"
#endif
#if ORIGINTEST_CAMERA
#include "aver/camera/CameraRigSystem.hpp"
#endif
#if ORIGINTEST_SOFTBODY
#include "aver/physics/physics_softbody_abi.h"
#include "aver/softbody/SoftBodySystem.hpp"
#endif
#if ORIGINTEST_HAIR
#include "aver/hair/HairSystem.hpp"
#endif
#if ORIGINTEST_SYNAPSE
#include "aver/synapse/Nav.hpp"
#include "aver/synapse/SynapseAgent.hpp"
#include "aver/synapse/SynapseAi.hpp"
#endif
#if ORIGINTEST_AUDIO
#include "aver/audio/Mixer.hpp"
#endif
#if ORIGINTEST_WORLD
#include "aver/formats/OcNav.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameStreaming.hpp"
#include "aver/game/OriginParticipants.hpp"
#include "aver/game/SequenceDirector.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/world/LevelInstance.hpp"
#include "aver/world/WorldOrigin.hpp"
#if ORIGINTEST_PHYSICS
#include "aver/physics/physics_origin_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_origin_abi.h"
#include "aver/framework/framework_timers_abi.h"
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using aver::fmt::OcSeqInterp;
using aver::fmt::OcSeqKey;
using aver::fmt::OcSeqTrack;
using aver::fmt::OcSeqTrackKind;
using aver::fmt::OcSequence;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

[[maybe_unused]] static bool near(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }
static bool nearV(const Vec3& a, const Vec3& b, f32 eps) { return (a - b).size() <= eps; }

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

static scene::Entity spawn(scene::World& w, const char* name, Vec3 p, scene::Entity parent = scene::kInvalidEntity) {
    Transform xf;
    xf.position = p;
    return w.create(name, parent, xf);
}

static Vec3 worldPos(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// ---- PlayMobility -------------------------------------------------------------------------------------------------

static void testPlayMobility(scene::World& w) {
    AVER_INFO("=== PlayMobility ===");
    constexpr u32 N = 10000;
    std::vector<scene::Entity> es;
    es.reserve(N);
    for (u32 i = 0; i < N; ++i)
        es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 10.0f, static_cast<f32>(i % 100) * 10.0f, 0.0f}));
    w.flush();

    game::PlayMobility pm, ctrl;
    pm.begin(w);
    ctrl.begin(w);
    const auto frame = [&](game::PlayMobility& m) {
        m.beginFrame(scene::kInvalidEntity);
        u32 movable = 0;
        for (const scene::Entity e : es)
            if (m.movable(w, e, w.worldMatrix(e))) ++movable;
        return movable;
    };
    check(frame(pm) == 0 && frame(ctrl) == 0, "the first look at a level's entities finds nothing movable");
    check(frame(pm) == 0 && frame(ctrl) == 0, "and a second look at still entities finds nothing either");

    const Vec3 d{100000.0f, 50000.0f, 0.0f};
    w.shiftRoots(d);
    w.flush();
    pm.noteOriginShift(1);
    check(frame(pm) == 0 && pm.movableCount() == 0, "after a shift and a flush 10,000 static entities are still Static");
    check(frame(ctrl) == N && ctrl.movableCount() == N,
          "control: without noteOriginShift every one of them turns Movable for the session");

    // A real move after the rebase is still seen, and only that one.
    w.setLocalPosition(es[5], w.localTransform(es[5]).position + Vec3{100.0f, 0.0f, 0.0f});
    w.flush();
    check(frame(pm) == 1 && pm.movableCount() == 1, "an entity that really moves afterwards is judged Movable, alone");

    pm.noteOriginShift(1);   // a repeat of a generation already taken must not re-arm the rebase
    w.setLocalPosition(es[6], w.localTransform(es[6]).position + Vec3{100.0f, 0.0f, 0.0f});
    w.flush();
    check(frame(pm) == 2, "a repeated generation is ignored: the next real move is still counted");

    pm.end();
    ctrl.end();
    clearWorld(w);
}

// ---- AnimSystem ---------------------------------------------------------------------------------------------------

#if ORIGINTEST_ANIM
namespace animtest {

std::string g_dir;
constexpr u64 kClip = 0x0A91C70001ull;

std::string resolvePath(u64 id, void*) { return id == kClip ? g_dir + "/move.ocanim" : std::string(); }

// A one-track object clip: translation + rotation about Z, three keys over two seconds.
fmt::OcAnimation routeClip() {
    static const Vec3 pos[3] = {{1000, 0, 0}, {1000, 300, 0}, {700, 300, 0}};
    static const f32 deg[3] = {0.0f, 90.0f, 180.0f};
    fmt::OcAnimation a;
    a.duration = 2.0f;
    a.flags = fmt::kOcAnimObject;
    fmt::OcTrack t;
    t.boneIndex = 0;
    t.channels = static_cast<u8>(fmt::kOcChannelTranslation | fmt::kOcChannelRotation);
    t.interp = fmt::OcInterp::Linear;
    t.times = {0.0f, 1.0f, 2.0f};
    for (int k = 0; k < 3; ++k) {
        const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(deg[k]));
        t.values.insert(t.values.end(), {pos[k].x, pos[k].y, pos[k].z, q.x, q.y, q.z, q.w});
    }
    a.tracks.push_back(t);
    return a;
}

// Where a mesh-space point is in the world, through the entity's composed world matrix.
Vec3 worldPoint(scene::World& w, scene::Entity e, const Vec3& p) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

} // namespace animtest

static void testAnim(scene::World& w) {
    using namespace animtest;
    AVER_INFO("=== AnimSystem object base ===");
    g_dir = (std::filesystem::temp_directory_path() / "aver-originparticipants-test").string();
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::string why;
    check(fmt::saveOcAnim(g_dir + "/move.ocanim", routeClip(), &why), "the object clip writes: " + why);

    const Vec3 d{100000.0f, -20000.0f, 0.0f};
    const Vec3 probe{30.0f, -10.0f, 5.0f};

    // One scenario: a live object animation, a shift, and (or not) the rebase; the error is how far the animated point ended
    // from where it was, minus the shift.
    const auto scenario = [&](bool rebase) {
        anim::AnimSystem sys;
        sys.setResolver(&resolvePath, nullptr);
        Transform xf;
        xf.position = Vec3{5000.0f, 2000.0f, 100.0f};
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(20.0f));
        const scene::Entity car = w.create("car", scene::kInvalidEntity, xf);
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(car, scene::kComponentAnimator));
        a->clip = kClip;
        a->time = 0.5f;
        sys.setObjectAnimationLive(true);
        sys.tick(w, 0.5f);
        sys.tick(w, 0.5f);
        w.flush();
        const Vec3 before = worldPoint(w, car, probe);
        const bool captured = sys.objectAnimatedEntities() == 1;
        w.shiftRoots(d);
        if (rebase) sys.rebase(w, d);
        sys.tick(w, 0.0f);   // no time passes: writeObject re-derives CLocal from the base
        const f32 err = (worldPoint(w, car, probe) - (before - d)).size();
        w.destroy(car);
        w.flush();
        return captured ? err : 1e9f;
    };
    const f32 withRebase = scenario(true);
    check(withRebase < 0.5f, "an animated root stays where it was, in the new frame (error " + std::to_string(withRebase) + " cm)");
    const f32 without = scenario(false);
    check(without > 50000.0f, "control: without the rebase the base snaps it back to the old frame (error " + std::to_string(without) + " cm)");
    clearWorld(w);
}
#endif // ORIGINTEST_ANIM

// ---- Sequencer ----------------------------------------------------------------------------------------------------

namespace seqtest {

OcSeqKey key(f64 t, OcSeqInterp in, std::initializer_list<f64> vals) {
    OcSeqKey k;
    k.t = t;
    k.interp = in;
    usize i = 0;
    for (const f64 v : vals) k.v[i++] = v;
    return k;
}

OcSeqTrack track(OcSeqTrackKind kind, i32 target, std::vector<OcSeqKey> keys) {
    OcSeqTrack t;
    t.kind = kind;
    t.target = target;
    t.keys = std::move(keys);
    return t;
}

OcSequence seqOf(f64 length, std::vector<OcSeqTrack> tracks) {
    OcSequence s;
    s.name = "S";
    s.length = length;
    s.loop = false;
    s.tracks = std::move(tracks);
    return s;
}

} // namespace seqtest

static void testSequencer(scene::World& w) {
    using namespace seqtest;
    AVER_INFO("=== Sequencer ===");
    const Vec3 d{250000.0f, 100000.0f, 0.0f};

    {   // the base store follows the shift
        const scene::Entity root = spawn(w, "root", Vec3{500000.0f, 300000.0f, 40.0f});
        const scene::Entity child = spawn(w, "child", Vec3{30.0f, 40.0f, 50.0f}, root);
        const scene::Entity root2 = spawn(w, "root2", Vec3{400000.0f, 100000.0f, 7.0f});
        w.flush();
        game::SeqBaseStore st;
        st.captureTransform(w, root);
        st.captureTransform(w, child);
        st.capture(w, root2, scene::kComponentLocal, static_cast<u16>(offsetof(scene::CLocal, xf)), 12);   // Local.position bytes
        // The sequencer drives them somewhere else...
        w.setLocalPosition(root, Vec3{1.0f, 2.0f, 3.0f});
        w.setLocalPosition(child, Vec3{4.0f, 5.0f, 6.0f});
        w.setLocalPosition(root2, Vec3{0.0f, 0.0f, 0.0f});
        w.shiftRoots(d);
        st.rebase(w, d);
        st.restore(w);
        const Vec3 r = w.localTransform(root).position, c = w.localTransform(child).position, r2 = w.localTransform(root2).position;
        check(nearV(r, Vec3{250000.0f, 200000.0f, 40.0f}, 1e-3f), "a restored root lands at its authored place minus the shift");
        check(nearV(c, Vec3{30.0f, 40.0f, 50.0f}, 1e-3f), "a restored child is parent-relative and unshifted");
        check(nearV(r2, Vec3{150000.0f, 0.0f, 7.0f}, 1e-3f), "a captured Local.position field is rebased the same way");
        clearWorld(w);
    }

    {   // level-absolute keys are written as key - O
        game::SequencePlayer::setSceneOrigin(100000, 50000);
        const scene::Entity box = spawn(w, "box", Vec3{0, 0, 0});
        const scene::Entity parent = spawn(w, "parent", Vec3{900.0f, 0.0f, 0.0f});
        const scene::Entity child = spawn(w, "kid", Vec3{0, 0, 0}, parent);
        const scene::Entity prop = spawn(w, "prop", Vec3{0, 0, 0});
        w.flush();
        OcSequence s = seqOf(2.0, {
            track(OcSeqTrackKind::Transform, 0, {key(0, OcSeqInterp::Linear, {100500, 50020, 5}), key(2, OcSeqInterp::Linear, {100700, 50020, 5})}),
            track(OcSeqTrackKind::Transform, 1, {key(0, OcSeqInterp::Linear, {10, 20, 30}), key(2, OcSeqInterp::Linear, {10, 20, 30})}),
            track(OcSeqTrackKind::Camera, -1, {key(0, OcSeqInterp::Linear, {100600, 50020, 300, 0, 0}), key(2, OcSeqInterp::Linear, {100600, 50020, 300, 0, 0})}),
        });
        OcSeqTrack localPos = track(OcSeqTrackKind::Property, 2, {key(0, OcSeqInterp::Linear, {100600, 50020, 5}), key(2, OcSeqInterp::Linear, {100600, 50020, 5})});
        localPos.property = "CLocal.position";
        s.tracks.push_back(localPos);

        game::SequencePlayer p;
        p.setSequence(s);
        p.bind({box, child, prop});
        p.setTime(1.0);
        p.evaluate(w, game::SeqPhase::Pre);
        p.evaluate(w, game::SeqPhase::Post);
        check(nearV(w.localTransform(box).position, Vec3{600.0f, 20.0f, 5.0f}, 1e-2f), "a root's Transform key is written minus the scene origin");
        check(nearV(w.localTransform(child).position, Vec3{10.0f, 20.0f, 30.0f}, 1e-3f), "a child's key is parent-relative and unchanged");
        check(nearV(w.localTransform(prop).position, Vec3{600.0f, 20.0f, 5.0f}, 1e-2f), "a root's CLocal.position property key is written minus the origin");
        game::SeqCameraPose pose;
        check(p.camera(pose) && nearV(pose.position, Vec3{600.0f, 20.0f, 300.0f}, 1e-2f), "a free camera key is written minus the origin");
        p.restore(w);

        game::SequencePlayer::setSceneOrigin(0, 0);   // identity: exactly what the sampler returns
        p.setTime(1.0);
        p.evaluate(w, game::SeqPhase::Pre);
        p.evaluate(w, game::SeqPhase::Post);
        check(nearV(w.localTransform(box).position, Vec3{100600.0f, 50020.0f, 5.0f}, 1e-3f), "origin zero writes the sampled key unchanged");
        check(p.camera(pose) && nearV(pose.position, Vec3{100600.0f, 50020.0f, 300.0f}, 1e-3f), "and the camera too");
        p.restore(w);
        clearWorld(w);
    }
    game::SequencePlayer::setSceneOrigin(0, 0);
}

// ---- Camera rig ---------------------------------------------------------------------------------------------------

#if ORIGINTEST_CAMERA && ORIGINTEST_PHYSICS
static camera::RigResult runRig(camera::CameraRigSystem& sys, scene::World& w, scene::Entity pawn, int frames) {
    camera::RigFrame f;
    f.dt = 1.0f / 60.0f;
    f.possessed = pawn;
    f.fallback.pos = Vec3{-1000, 0, 500};
    f.fallback.yaw = 0.0f;
    f.fallback.pitch = -0.2f;
    camera::RigResult r;
    for (int i = 0; i < frames; ++i) { w.flush(); r = sys.drive(w, f); }
    return r;
}

static void testCameraRig(scene::World& w) {
    AVER_INFO("=== Camera rig ===");
    aver_phys_shutdown();
    aver_phys_init();
    camera::CameraRigSystem::registerComponents(w);
    const Vec3 d{50000.0f, 20000.0f, 0.0f};

    // Two systems, two pawns: one rebased, one not.
    camera::CameraRigSystem good, bad;
    good.install(w);
    bad.install(w);
    const scene::Entity pawnA = spawn(w, "pawnA", Vec3{60000.0f, 20000.0f, 0.0f});
    const scene::Entity pawnB = spawn(w, "pawnB", Vec3{-60000.0f, 20000.0f, 0.0f});
    const scene::Entity rigA = spawn(w, "rigA", Vec3{0, 0, 0}, pawnA);
    const scene::Entity rigB = spawn(w, "rigB", Vec3{0, 0, 0}, pawnB);
    if (auto* r = camera::CameraRigSystem::attach(w, rigA, camera::CameraPreset::Chase, pawnA)) r->pivotZ = 100.0f;
    if (auto* r = camera::CameraRigSystem::attach(w, rigB, camera::CameraPreset::Chase, pawnB)) r->pivotZ = 100.0f;

    camera::RigResult a = runRig(good, w, pawnA, 180);
    camera::RigResult b = runRig(bad, w, pawnB, 180);
    check(a.active && b.active, "both rigs are live before the shift");
    const Vec3 a0 = a.pov.pos, b0 = b.pov.pos;

    w.shiftRoots(d);
    good.rebase(d);
    a = runRig(good, w, pawnA, 1);
    b = runRig(bad, w, pawnB, 1);
    check(a.active && !a.cut, "rebased: the shift is not a camera cut");
    check(nearV(a.pov.pos, a0 - d, 1.0f), "rebased: the camera sits where it did, in the new frame");
    check(b.cut, "control: without the rebase the pivot looks like a 50 km teleport and cuts");
    (void)b0;

    good.clear();
    bad.clear();
    clearWorld(w);
    aver_phys_shutdown();
}
#endif // ORIGINTEST_CAMERA && ORIGINTEST_PHYSICS

// ---- Soft body and hair -------------------------------------------------------------------------------------------

#if ORIGINTEST_SOFTBODY && ORIGINTEST_PHYSICS
namespace sbtest {

constexpr u64 kSheet = 0x5B00D1ull;

struct Stores {
    std::map<u64, fmt::OcMeshData> meshes;
    static const fmt::OcMeshData* mesh(u64 id, void* user) {
        auto& s = *static_cast<Stores*>(user);
        const auto it = s.meshes.find(id);
        return it == s.meshes.end() ? nullptr : &it->second;
    }
    static const softbody::SoftAsset* asset(u64, void*) { return nullptr; }
};

// A cols x rows sheet hanging in the XZ plane from z = 0 down.
fmt::OcMeshData sheet(u32 cols, u32 rows, f32 spacing) {
    fmt::OcMeshData m;
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < cols; ++c) {
            const f32 u = static_cast<f32>(c) * spacing, v = static_cast<f32>(r) * spacing;
            m.positions.insert(m.positions.end(), {u, 0.0f, -v});
            m.normals.insert(m.normals.end(), {0.0f, -1.0f, 0.0f});
            m.uvs.insert(m.uvs.end(), {static_cast<f32>(c) / static_cast<f32>(cols - 1), static_cast<f32>(r) / static_cast<f32>(rows - 1)});
        }
    for (u32 r = 0; r + 1 < rows; ++r)
        for (u32 c = 0; c + 1 < cols; ++c) {
            const u32 a = r * cols + c, b = a + 1, d = a + cols, e = d + 1;
            for (const u32 i : {a, b, e, a, e, d}) m.indices.push_back(i);
        }
    m.computeBounds();
    return m;
}

} // namespace sbtest

static void testSoftBody(scene::World& w) {
    using namespace sbtest;
    namespace sb = aver::softbody;
    AVER_INFO("=== Soft body ===");
    aver_phys_shutdown();
    aver_phys_reset_layer_collisions();
    aver_phys_init();
    aver_phys_sb_set_wind(0, 0, 0, 0, 0, 0);
    sb::SoftBodySystem::registerComponents(w);
    sb::SoftBodySystem sys;
    Stores stores;
    stores.meshes[kSheet] = sheet(5, 5, 10.0f);
    sys.install(w);
    sys.setMeshSource(&Stores::mesh, &stores);
    sys.setAssetSource(&Stores::asset, &stores);

    const scene::Entity e = w.create("cloth");
    auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
    mr->mesh = kSheet;
    mr->flags = scene::kMeshRendererVisible;
    w.setLocalPosition(e, Vec3{100000.0f, 50000.0f, 200.0f});
    w.flush();
    sb::SoftBodySystem::attachCloth(w, e, sb::SoftPreset::ClothHeavy);
    for (int i = 0; i < 30; ++i) {
        aver_phys_step(1.0f / 60.0f);
        w.flush();
        sys.tick(w, 1.0f / 60.0f);
    }

    const sb::Cage* cage = sys.cageOf(e);
    check(cage != nullptr && !cage->particles.empty(), "the cloth is built");
    u32 viewCount = 0;
    const sb::PieceView* views = sys.pieces(viewCount);
    const sb::PieceView* view = nullptr;
    for (u32 i = 0; i < viewCount; ++i)
        if (views[i].entity == e && views[i].layer == 0) view = &views[i];
    check(view != nullptr && view->vertexCount > 0, "and published as a piece view");
    if (cage && view && !cage->particles.empty() && view->vertexCount > 0) {
        const Vec3 p0 = cage->particles.front().pos, v0 = view->world[0];
        const Vec3 d{100000.0f, 50000.0f, 0.0f};
        sys.rebase(d);
        check(nearV(cage->particles.front().pos, p0 - d, 1e-3f), "a cage particle moves by -delta");
        check(nearV(view->world[0], v0 - d, 1e-3f), "so does the published draw array (the view points at the piece's own buffer)");
    }

    sys.clear();
    if (w.valid(e)) w.destroy(e);
    w.flush();
    sys.uninstall();
    aver_phys_shutdown();
    clearWorld(w);
}
#endif // ORIGINTEST_SOFTBODY && ORIGINTEST_PHYSICS

#if ORIGINTEST_HAIR
static void testHair() {
    AVER_INFO("=== Hair ===");
    // A built groom needs a physics hair, a skeleton and a compute host (the headed pass); the rebase itself is one loop over the
    // instances' previous root position. What can run here: it is harmless on a system with none.
    hair::HairSystem sys;
    sys.rebase(Vec3{100000.0f, 0.0f, 0.0f});
    check(true, "HairSystem::rebase on an empty system is a no-op");
}
#endif

// ---- Kinematic driver ---------------------------------------------------------------------------------------------

#if ORIGINTEST_PHYSICS
static void testKinematicDriver(scene::World& w) {
    AVER_INFO("=== Kinematic driver ===");
    aver_phys_shutdown();
    aver_phys_reset_layer_collisions();
    aver_phys_init();
    const f32 step = aver_phys_fixed_step();
    const Vec3 d{100000.0f, 50000.0f, 0.0f};

    const scene::Entity e = spawn(w, "lift", Vec3{100000.0f, 50000.0f, 500.0f});
    w.flush();
    const i32 box = aver_phys_add_static_box(100000.0f, 50000.0f, 500.0f, 50.0f, 50.0f, 50.0f);
    check(box > 0 && aver_phys_body_set_motion_type(box, AVER_PHYS_MOTION_KINEMATIC) == 1, "a kinematic body for the animated entity");
    const std::vector<world::AnimatedBody> bodies{world::AnimatedBody{e, box}};

    world::driveKinematicBodies(w, bodies, step);   // first sight: records the pose
    aver_phys_step(step);

    // The shift: scene, physics (the PHYSICS participant, emulated by hand) and the driver's memory.
    w.shiftRoots(d);
    aver_phys_body_set_position(box, 0.0f, 0.0f, 500.0f);
    world::rebaseKinematicDrivers(d);
    w.setLocalPosition(e, Vec3{20.0f, 0.0f, 500.0f});   // the animation moves it 20 cm in the new frame
    w.flush();
    world::driveKinematicBodies(w, bodies, step);
    f32 vel[3] = {0, 0, 0};
    aver_phys_body_velocity(box, vel);
    check(vel[0] > 500.0f, "rebased: the body is driven across the shift (it carries riders), velocity " + std::to_string(vel[0]));
    aver_phys_step(step);
    f32 pos[3] = {0, 0, 0};
    aver_phys_body_position(box, pos);
    check(near(pos[0], 20.0f, 1.0f), "and reaches the animated pose");

    // Control: the same shift back, with the driver's memory left in the old frame, reads as a 100 km jump and is teleported.
    w.shiftRoots(Vec3{-d.x, -d.y, -d.z});
    aver_phys_body_set_position(box, 100020.0f, 50000.0f, 500.0f);
    w.setLocalPosition(e, w.localTransform(e).position + Vec3{20.0f, 0.0f, 0.0f});
    w.flush();
    world::driveKinematicBodies(w, bodies, step);
    aver_phys_body_velocity(box, vel);
    check(std::fabs(vel[0]) < 1.0f, "control: without the rebase the same move is a teleport with zero velocity");

    aver_phys_remove_body(box);
    clearWorld(w);
    aver_phys_shutdown();
}
#endif // ORIGINTEST_PHYSICS

// ---- Synapse ------------------------------------------------------------------------------------------------------

#if ORIGINTEST_SYNAPSE
namespace syntest {

u32 noOcclusion(void*, const Vec3&, const Vec3&) { return 0; }

fmt::OcNavData openGrid(u32 w, u32 h, f32 originX, f32 originY, f32 cell = 50.0f) {
    fmt::OcNavData nav;
    nav.cellSizeCm = cell;
    nav.originXCm = originX;
    nav.originYCm = originY;
    nav.widthCells = w;
    nav.heightCells = h;
    nav.cells.resize(static_cast<usize>(w) * h);
    for (fmt::OcNavCell& c : nav.cells) c.flags = fmt::kOcNavWalkable;
    synapse::buildRegions(nav);
    return nav;
}

} // namespace syntest

static void testSynapse(scene::World& w) {
    using namespace synapse;
    using namespace syntest;
    AVER_INFO("=== Synapse ===");
    clearWorld(w);
    agentSystem().registerComponents(w);
    SynapseAi ai;
    ai.registerComponents(w);
    ai.hearing().setOcclusion(&noOcclusion, nullptr);
    const Vec3 d{100000.0f, 50000.0f, 0.0f};

    {   // the grid
        fmt::OcNavData nav = openGrid(4, 4, 100000.0f, 50000.0f);
        const Vec3 c0 = cellToWorld(nav, 1, 1);
        rebaseNav(nav, d);
        const Vec3 c1 = cellToWorld(nav, 1, 1);
        check(near(c1.x, c0.x - d.x) && near(c1.y, c0.y - d.y) && near(c1.z, c0.z), "rebaseNav moves the grid origin by -delta on X and Y only");
        u32 gx = 0, gy = 0;
        check(worldToCell(nav, c1.x, c1.y, gx, gy) && gx == 1 && gy == 1, "and the cell still answers to its new coordinates");
    }

    {   // agent paths
        const auto scenario = [&](bool rebaseAgents) {
            clearWorld(w);
            fmt::OcNavData nav = openGrid(5, 5, 100000.0f, 50000.0f);
            const scene::Entity e = w.create("agent", scene::kInvalidEntity, [&] { Transform xf; xf.position = cellToWorld(nav, 0, 2); return xf; }());
            agentSystem().attach(w, e);
            agentSystem().setGoal(w, e, cellToWorld(nav, 4, 2));
            agentSystem().tick(w, &nav);
            const u32 type = agentSystem().componentType();
            auto* a = w.component<CSynapseAgent>(e, type);
            const bool pathing = a && a->status == static_cast<i32>(AgentStatus::Pathing);
            w.shiftRoots(d);
            rebaseNav(nav, d);
            if (rebaseAgents) agentSystem().rebase(w, d);
            agentSystem().tick(w, &nav);
            a = w.component<CSynapseAgent>(e, type);
            if (!pathing || !a) return -1.0f;
            const Vec3 pos = w.localTransform(e).position;
            const f32 dx = a->targetXCm - pos.x, dy = a->targetYCm - pos.y;
            return std::sqrt(dx * dx + dy * dy);
        };
        const f32 good = scenario(true);
        check(good >= 0.0f && good < 1000.0f, "rebased: the agent's current target is still on its path, next to it (" + std::to_string(good) + " cm)");
        const f32 bad = scenario(false);
        check(bad > 50000.0f, "control: without the rebase the target is left in the old frame (" + std::to_string(bad) + " cm away)");
    }

    {   // hearing: a memory and a queued noise
        const auto scenario = [&](bool rebaseHearing) {
            clearWorld(w);
            ai.reset();
            const Vec3 at{100000.0f, 50000.0f, 0.0f};
            const scene::Entity ear = spawn(w, "ear", at);
            ai.hearing().attach(w, ear);
            NoiseEvent first;
            first.pos = at + Vec3{300.0f, 0.0f, 0.0f};
            first.loudnessCm = 1000.0f;
            first.tag = 1;
            ai.hearing().emit(first);
            ai.hearing().tick(w, 0.1f);
            const HearingMemory* mem = ai.hearing().memoryOf(ear);
            const Vec3 remembered = (mem && mem->best()) ? mem->best()->pos : Vec3{};
            NoiseEvent queued;
            queued.pos = at + Vec3{0.0f, 400.0f, 0.0f};
            queued.loudnessCm = 1000.0f;
            queued.tag = 2;
            ai.hearing().emit(queued);   // not ticked yet
            w.shiftRoots(d);
            if (rebaseHearing) ai.hearing().rebase(d);
            const HearingMemory* after = ai.hearing().memoryOf(ear);
            const bool memoryMoved = after && after->best() && nearV(after->best()->pos, remembered - d, 1e-3f);
            ai.hearing().tick(w, 0.1f);
            after = ai.hearing().memoryOf(ear);
            const usize heard = after ? after->entries().size() : 0;
            return std::make_pair(memoryMoved, heard);
        };
        const auto good = scenario(true);
        check(good.first, "rebased: the remembered noise moves by -delta");
        check(good.second == 2, "rebased: a noise queued before the shift is heard from where it really was");
        const auto bad = scenario(false);
        check(!bad.first && bad.second == 1, "control: without the rebase the memory is stale and the queued noise is out of earshot");
    }

    {   // cover points
        const u32 id = ai.tactics().covers().addAuthored(V2{100200.0f, 50300.0f}, V2{1.0f, 0.0f});
        ai.tactics().rebase(d);
        const CoverPoint* c = ai.tactics().covers().find(id);
        check(c && near(c->pos.x, 200.0f) && near(c->pos.y, 300.0f), "a cover point moves by -delta");
        ai.tactics().reset();
    }

    {   // crowd
        clearWorld(w);
        ai.reset();
        const scene::Entity e = spawn(w, "walker", Vec3{100000.0f, 50000.0f, 0.0f});
        CSynapseCrowd* c = ai.crowd().attach(w, e);
        ai.crowd().tick(w, nullptr, 1.0f / 60.0f);
        const CrowdAgent* before = ai.crowd().sim().find(e);
        check(c != nullptr && before != nullptr && near(before->pos.x, 100000.0f), "the crowd agent is simulated at the entity");
        c = w.component<CSynapseCrowd>(e, ai.crowd().componentType());
        if (c) {
            c->mode = static_cast<i32>(CrowdMode::Seek);
            c->steerXCm = 100400.0f;
            c->steerYCm = 50000.0f;
        }
        w.shiftRoots(d);
        ai.crowd().rebase(w, d);
        const CrowdAgent* after = ai.crowd().sim().find(e);
        check(after && near(after->pos.x, 0.0f) && near(after->pos.y, 0.0f), "the agent moves by -delta (it matches the shifted entity)");
        c = w.component<CSynapseCrowd>(e, ai.crowd().componentType());
        check(c && near(c->steerXCm, 400.0f) && near(c->steerYCm, 0.0f), "a Seek target moves by -delta");
        ai.smartObjects().rebase();
        check(true, "smart objects re-resolve on the next sync (nothing to read back without an .ocso)");
    }
    clearWorld(w);
    ai.reset();
}
#endif // ORIGINTEST_SYNAPSE

// ---- Audio --------------------------------------------------------------------------------------------------------

#if ORIGINTEST_AUDIO
static f32 rmsOf(const std::vector<f32>& v, u32 offset) {
    f64 sum = 0.0;
    u32 n = 0;
    for (usize i = offset; i < v.size(); i += 2) { sum += static_cast<f64>(v[i]) * v[i]; ++n; }
    return n ? static_cast<f32>(std::sqrt(sum / n)) : 0.0f;
}

static void testAudio() {
    AVER_INFO("=== Audio ===");
    constexpr u32 kRate = 48000;
    audio::Mixer m;
    check(m.init(kRate, 2, 8, 8), "the mixer initialises");
    audio::SoundData dc;
    dc.channels = 1;
    dc.sampleRate = kRate;
    dc.samples.assign(kRate, 1.0f);
    const audio::SoundHandle s = m.addSound(std::move(dc));

    audio::Listener l;
    l.position[0] = 100000.0f;
    l.position[1] = 50000.0f;
    m.setListener(l);
    audio::PlayDesc p;
    p.sound = s;
    p.looping = true;
    p.positional = true;
    p.position[0] = 100000.0f;
    p.position[1] = 50050.0f;   // 50 cm to the listener's right
    p.attenuation = {100.0f, 2000.0f};
    const audio::VoiceHandle v = m.play(p);
    check(v != 0, "a looping 3D voice starts");

    std::vector<f32> a(64 * 2), b(64 * 2);
    m.mix(a.data(), 64);
    check(rmsOf(a, 1) > rmsOf(a, 0) * 4.0f && rmsOf(a, 1) > 0.1f, "before the shift it is loud and panned right");

    m.rebase(100000.0f, 50000.0f, 0.0f);
    m.mix(b.data(), 64);
    check(std::fabs(rmsOf(b, 0) - rmsOf(a, 0)) < 0.02f && std::fabs(rmsOf(b, 1) - rmsOf(a, 1)) < 0.02f,
          "after the shift (voices and listener together) it keeps its level and pan");
    check(near(m.listener().position[0], 0.0f) && near(m.listener().position[1], 0.0f), "the listener moved by -delta");
    check(m.playing(v), "and the voice is still playing");
}
#endif

// ---- WorldOrigin with fake participants ---------------------------------------------------------------------------

#if ORIGINTEST_WORLD
namespace servicetest {

// Absolute x of a scene-frame root, in f64: what the shift must keep.
f64 absX(const world::WorldOrigin& o, scene::World& w, scene::Entity e) {
    return static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(o.origin().x);
}

// The slice of the hosts' participant table this test can build without a host: a physics stand-in (order 10), scene roots and
// PlayMobility (20), a host camera member (30) and a renderer-order observer (60). `log` records the commit order; the physics
// stand-in's prepare refuses while `physicsOk` is false.
struct Rig {
    scene::World& w;
    game::PlayMobility& pm;
    Vec3 hostCam{};
    bool physicsOk = true;
    std::vector<std::string> log;
    u32 sceneShifts = 0;

    void wire(world::WorldOrigin& o, bool withPlayMobility, bool withHostCam) {
        world::Participant phys;
        phys.name = "physics"; phys.order = 10;
        phys.prepare = [this](const world::ShiftEvent&) {
            world::PrepareResult r;
            if (!physicsOk) { r.ok = false; r.deferReason = "physics busy"; }
            return r;
        };
        phys.commit = [this](const world::ShiftEvent&) { log.push_back("physics"); };
        o.addParticipant(phys);

        world::Participant scn;
        scn.name = "scene"; scn.order = 20;
        scn.commit = [this](const world::ShiftEvent& ev) { w.shiftRoots(ev.deltaF); ++sceneShifts; log.push_back("scene"); };
        o.addParticipant(scn);

        if (withPlayMobility) {
            world::Participant mob;
            mob.name = "playmobility"; mob.order = 20;
            mob.commit = [this](const world::ShiftEvent& ev) { pm.noteOriginShift(ev.generation); log.push_back("playmobility"); };
            o.addParticipant(mob);
        }
        if (withHostCam) {
            world::Participant host;
            host.name = "host"; host.order = 30;
            host.commit = [this](const world::ShiftEvent& ev) {
                hostCam = Vec3{hostCam.x - ev.deltaF.x, hostCam.y - ev.deltaF.y, hostCam.z - ev.deltaF.z};
                log.push_back("host");
            };
            o.addParticipant(host);
        }
        world::Participant rnd;
        rnd.name = "render"; rnd.order = 60;
        rnd.commit = [this, &o](const world::ShiftEvent& ev) {
            log.push_back(o.origin() == ev.newOrigin && o.generation() == ev.generation ? "render" : "render-saw-old-origin");
        };
        o.addParticipant(rnd);
    }
};

} // namespace servicetest

static void testOriginService(scene::World& w) {
    using namespace servicetest;
    AVER_INFO("=== WorldOrigin with fake participants ===");

    {   // the region a level loads at: the SPAWN or CAMERA point's region centre; a level in region 0 gets (0, 0)
        constexpr i64 step = world::kOriginStepCm;
        check(world::originRegionOf(0, 0) == world::OriginCm{}, "a level at the world origin loads at (0, 0)");
        check(world::originRegionOf(800000.0, -800000.0) == world::OriginCm{}, "and so does anything inside region 0 (+-819,200 cm)");
        check(world::originRegionOf(1000000.0, 0) == world::OriginCm{step, 0}, "a point 10 km out is in region +1");
        check(world::originRegionOf(-4.0 * static_cast<f64>(step) - 5.0e5, 3.0 * static_cast<f64>(step)) == world::OriginCm{-4 * step, 3 * step},
              "and far negative / positive points snap to their own region centres");
    }

    constexpr u32 N = 2000;
    std::vector<scene::Entity> es;
    for (u32 i = 0; i < N; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 10.0f - 5000.0f, static_cast<f32>(i % 50) * 20.0f, 0.0f}));
    const scene::Entity far1 = spawn(w, "far", Vec3{1100000.0f, 400000.0f, 90.0f});
    const scene::Entity kid = spawn(w, "kid", Vec3{25.0f, 0.0f, 0.0f}, far1);
    w.flush();

    const auto staticFrame = [&](game::PlayMobility& m) {
        m.beginFrame(scene::kInvalidEntity);
        u32 movable = 0;
        for (const scene::Entity e : es)
            if (m.movable(w, e, w.worldMatrix(e))) ++movable;
        return movable;
    };

    {   // the plan's order, once each, the new origin visible to later participants, absolute positions kept
        game::PlayMobility pm;
        pm.begin(w);
        check(staticFrame(pm) == 0 && staticFrame(pm) == 0, "the level's entities start Static");
        Rig rig{w, pm};
        rig.hostCam = Vec3{1000000.0f, 200.0f, 50.0f};
        world::WorldOrigin o;
        rig.wire(o, true, true);

        std::vector<f64> before;
        for (const scene::Entity e : es) before.push_back(absX(o, w, e));
        const f64 farBefore = absX(o, w, far1);
        const Vec3 kidBefore = w.localTransform(kid).position;
        const u32 genBefore = w.originGeneration();

        check(o.update(rig.hostCam), "a camera 10 km out shifts one region");
        check(o.origin() == world::OriginCm{world::kOriginStepCm, 0}, "the origin is one region along +X");
        const std::vector<std::string> want = {"physics", "scene", "playmobility", "host", "render"};
        check(rig.log == want, "participants commit once each, in order, the last seeing the new origin");
        check(rig.sceneShifts == 1 && w.originGeneration() == genBefore + 1, "and the scene shifted its roots exactly once");

        w.flush();
        f64 worst = 0.0;
        for (usize i = 0; i < es.size(); ++i) worst = std::max(worst, std::fabs(absX(o, w, es[i]) - before[i]));
        check(worst < 0.01, "every root keeps its absolute position (worst " + std::to_string(worst) + " cm)");
        check(std::fabs(absX(o, w, far1) - farBefore) < 0.01, "a root past the old window too");
        check(nearV(w.localTransform(kid).position, kidBefore, 1e-4f), "a child is parent-relative and untouched");
        check(staticFrame(pm) == 0 && pm.movableCount() == 0, "the statics stay Static through the whole chain");

        // No rebase loop: the host camera moved with the scene, so the next frame has nothing to do.
        const world::OriginCm again = o.decide(rig.hostCam);
        check(again.x == 0 && again.y == 0, "the camera member was rebased, so the same camera decides nothing next frame");
        check(!o.update(rig.hostCam), "and update does nothing");
        Vec3 wob = rig.hostCam;
        wob.x -= 100000.0f;   // a kilometre back toward the old centre
        check(!o.update(wob), "a kilometre of travel back does not flip the rebase");
        pm.end();
    }

    {   // a refusal changes nothing; the retry next frame goes through
        clearWorld(w);
        es.clear();
        for (u32 i = 0; i < 100; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 100.0f, 0.0f, 0.0f}));
        w.flush();
        game::PlayMobility pm;
        pm.begin(w);
        Rig rig{w, pm};
        rig.hostCam = Vec3{950000.0f, 0.0f, 0.0f};
        world::WorldOrigin o;
        rig.wire(o, true, true);
        rig.physicsOk = false;
        const Vec3 p0 = w.localTransform(es[7]).position;
        const u32 genBefore = w.originGeneration();
        check(!o.update(rig.hostCam), "a prepare that refuses defers the shift");
        check(rig.log.empty() && rig.sceneShifts == 0, "no commit ran");
        check(o.origin() == world::OriginCm{} && o.generation() == 0 && w.originGeneration() == genBefore,
              "origin, generation and the scene are untouched");
        check(o.stats().deferrals == 1 && o.stats().lastDeferReason && std::string(o.stats().lastDeferReason) == "physics busy",
              "the deferral and its reason are recorded");
        check(nearV(w.localTransform(es[7]).position, p0, 0.0f) && rig.hostCam.x == 950000.0f, "no position moved");
        rig.physicsOk = true;
        check(o.update(rig.hostCam), "next frame the same shift goes through");
        check(o.stats().shifts == 1 && o.generation() == 1, "once");
        pm.end();
    }

    {   // CONTROLS: a participant left out shows its own failure
        clearWorld(w);
        es.clear();
        for (u32 i = 0; i < 200; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 100.0f, 0.0f, 0.0f}));
        w.flush();
        game::PlayMobility pm;
        pm.begin(w);
        check(staticFrame(pm) == 0 && staticFrame(pm) == 0, "(control) statics start Static");
        Rig rig{w, pm};
        rig.hostCam = Vec3{1000000.0f, 0.0f, 0.0f};
        world::WorldOrigin o;
        rig.wire(o, false, false);   // no PlayMobility participant, no host camera
        check(o.update(rig.hostCam), "(control) the shift runs");
        w.flush();
        check(staticFrame(pm) == static_cast<u32>(es.size()), "(control) without the PlayMobility participant every static entity turns Movable");
        check(o.decide(rig.hostCam).x != 0, "(control) without the host camera participant the same camera asks for another shift");
        pm.end();
    }
    clearWorld(w);
}

// ---- The real participants ----------------------------------------------------------------------------------------

namespace realtest {

// A device with nothing behind it that records what the renderer participant tells it.
struct RecDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void shiftWorld(const rhi::ShiftWorldInfo& info) override { ++calls; last = info; org = info.newOrigin; }
    rhi::ShiftOrigin worldOrigin() const override { return org; }
    u32 calls = 0;
    rhi::ShiftWorldInfo last{};
    rhi::ShiftOrigin org{};
};

constexpr f32 kStep = 1638400.0f;

f64 absXOf(const game::OriginParticipants& op, scene::World& w, scene::Entity e) {
    return static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(op.origin().x);
}

} // namespace realtest

static void testRealParticipants(scene::World& w) {
    using namespace realtest;
    AVER_INFO("=== Real participants (OriginParticipants) ===");
    clearWorld(w);
    constexpr i64 step = world::kOriginStepCm;

    {   // the order table of 2.2.4, once, and install() twice does not register twice
        game::OriginParticipants op;
        op.install();
        op.install();
        const std::vector<std::string> want = {"gate", "physics", "scene", "host", "systems", "streaming", "render", "audio+particles", "editor", "event"};
        std::vector<std::string> got;
        for (usize i = 0; i < op.participantCount(); ++i) got.emplace_back(op.participantName(i));
        check(got == want, "ten participants in the plan's order: gate, physics, scene, host, systems, streaming, render, audio+particles, editor, event");
        check(op.service().participantCount() == want.size(), "and the service holds exactly those (install is idempotent)");
    }

    {   // scene + PlayMobility + host members + renderer + audio + events in ONE slot, in order, absolute positions kept
        std::vector<scene::Entity> es;
        for (u32 i = 0; i < 2000; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 10.0f - 5000.0f, static_cast<f32>(i % 50) * 20.0f, 0.0f}));
        const scene::Entity far1 = spawn(w, "far", Vec3{1100000.0f, 400000.0f, 90.0f});
        const scene::Entity kid = spawn(w, "kid", Vec3{25.0f, 0.0f, 0.0f}, far1);
        w.flush();

        game::PlayMobility pm;
        pm.begin(w);
        pm.beginFrame(scene::kInvalidEntity);
        game::SequenceDirector seqs;
        RecDev dev;
        Vec3 cam{1000000.0f, 200.0f, 50.0f}, eye{999000.0f, 200.0f, 50.0f}, gi{900000.0f, 0.0f, 300.0f};

        game::OriginParticipants op;
        game::OriginBindings& b = op.bindings();
        b.mobility = &pm;
        b.sequences = &seqs;
        b.device = &dev;
        b.positions = {&cam, &eye, &gi};
        std::vector<std::string> log;
        bool sceneShiftedAtHost = false, deviceToldAtEditor = false, deviceToldAtEvent = false, hostSeenAtRender = false;
        Vec3 audioDelta{};
        b.onHostCommit = [&](const world::ShiftEvent&) { log.push_back("host"); sceneShiftedAtHost = w.originGeneration() > 0 && w.localTransform(far1).position.x < 400000.0f; };
        b.onRenderCommit = [&](const world::ShiftEvent&) { log.push_back("render"); hostSeenAtRender = cam.x < 0.0f && dev.calls == 1; };
        b.audioShift = [&](f32 dx, f32 dy, f32 dz) { log.push_back("audio"); audioDelta = Vec3{dx, dy, dz}; };
        b.editorPrepare = [&](const world::ShiftEvent&) { log.push_back("editor-prepare"); return world::PrepareResult{}; };
        b.editorCommit = [&](const world::ShiftEvent&) { log.push_back("editor"); deviceToldAtEditor = dev.calls == 1; };
        u32 eventGen = 0;
        b.postEvent = [&](const world::ShiftEvent& ev) { log.push_back("event"); eventGen = ev.generation; deviceToldAtEvent = dev.calls == 1; };
        op.configure(1, -1, false);
        op.install();

        std::vector<f64> before;
        for (const scene::Entity e : es) before.push_back(absXOf(op, w, e));
        const f64 farBefore = absXOf(op, w, far1);
        const Vec3 kidBefore = w.localTransform(kid).position;
        const u32 sceneGen = w.originGeneration();

        check(op.update(cam), "a camera 10 km out shifts one region through the real participants");
        check(op.origin() == world::OriginCm{step, 0} && op.generation() == 1, "the origin is one region along +X, generation 1");
        const std::vector<std::string> want = {"editor-prepare", "host", "render", "audio", "editor", "event"};
        check(log == want, "the hooks run in the table's order: host, render, audio, editor (prepare first), event");
        check(sceneShiftedAtHost && hostSeenAtRender, "the scene had shifted before the host members, and the host members before the renderer");
        check(deviceToldAtEditor && deviceToldAtEvent, "the renderer had been told before the editor and the event");
        check(eventGen == 1 && w.originGeneration() == sceneGen + 1, "the event carries the generation and the scene shifted its roots once");
        check(dev.calls == 1 && dev.last.newOrigin.x == step && dev.last.newOrigin.y == 0 && dev.last.generation == 1 &&
              dev.last.deltaCm.x == static_cast<f64>(step) && dev.last.deltaCm.y == 0.0 && dev.org.x == step,
              "the device got the whole event (delta, new origin, generation) exactly once");
        check(audioDelta.x == kStep && audioDelta.y == 0.0f, "audio moved by the f32 delta");

        w.flush();
        f64 worst = 0.0;
        for (usize i = 0; i < es.size(); ++i) worst = std::max(worst, std::fabs(absXOf(op, w, es[i]) - before[i]));
        check(worst < 0.01, "every root keeps its absolute position (worst " + std::to_string(worst) + " cm)");
        check(std::fabs(absXOf(op, w, far1) - farBefore) < 0.01, "a root past the old window too");
        check(nearV(w.localTransform(kid).position, kidBefore, 1e-4f), "a child is parent-relative and untouched");
        check(cam.x == 1000000.0f - kStep && eye.x == 999000.0f - kStep && gi.x == 900000.0f - kStep && cam.y == 200.0f && cam.z == 50.0f,
              "the camera, eye and GI centre moved by exactly -delta, Y and Z alone");
        pm.beginFrame(scene::kInvalidEntity);
        u32 movable = 0;
        for (const scene::Entity e : es) if (pm.movable(w, e, w.worldMatrix(e))) ++movable;
        check(movable == 0, "PlayMobility: the statics stay Static through the shift and the flush");
        i64 sx = -1, sy = -1;
        game::SequencePlayer::sceneOrigin(sx, sy);
        check(sx == step && sy == 0, "the sequencer was told the origin its absolute keys convert into");
        check(!op.update(cam), "the camera member moved with the scene, so the next frame decides nothing (no rebase loop)");
        check(op.service().stats().shifts == 1 && op.service().stats().deferrals == 0, "one shift, no deferrals");
        pm.end();

        // resetForLevel: back to a clean frame, the renderer told, nothing else run
        op.resetForLevel();
        check(op.origin() == world::OriginCm{} && op.generation() == 2, "resetForLevel returns the service to (0, 0) and counts a generation");
        check(dev.calls == 2 && dev.last.newOrigin.x == 0 && dev.last.deltaCm.x == -static_cast<f64>(step) && dev.org.x == 0,
              "and tells the device the frame it now names (delta back, origin zero)");
        game::SequencePlayer::sceneOrigin(sx, sy);
        check(sx == 0 && sy == 0, "and the sequencer");
        const usize hooksBefore = log.size();
        op.resetForLevel();
        check(dev.calls == 2 && log.size() == hooksBefore, "a reset to the origin it already has tells nobody");
        clearWorld(w);
    }

    {   // the deferral rules (2.2.4): nothing runs, nothing moves, the reason is recorded, the retry goes through
        std::vector<scene::Entity> es;
        for (u32 i = 0; i < 50; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 100.0f, 0.0f, 0.0f}));
        w.flush();
        RecDev dev;
        Vec3 cam{950000.0f, 0.0f, 0.0f};
        game::OriginParticipants op;
        game::OriginBindings& b = op.bindings();
        b.device = &dev;
        b.positions = {&cam};
        bool busyNow = false, sawUrgent = false, gestureActive = false, gestureCancelled = false;
        b.busy = [&](bool urgent) -> const char* { sawUrgent = urgent; return busyNow ? "a play transition" : nullptr; };
        b.editorPrepare = [&](const world::ShiftEvent& ev) {
            world::PrepareResult r;
            if (gestureActive && !ev.urgent) { r.ok = false; r.deferReason = "a drag is in progress"; }
            else if (gestureActive) { gestureActive = false; gestureCancelled = true; }   // urgent: the editor cancels the gesture and accepts
            return r;
        };
        op.configure(1, -1, false);
        op.install();
        const auto reason = [&] { return op.service().stats().lastDeferReason ? std::string(op.service().stats().lastDeferReason) : std::string(); };
        const auto untouched = [&] { return op.generation() == 0 && dev.calls == 0 && cam.x == 950000.0f && w.localTransform(es[7]).position.x == 700.0f; };

        {   // a level loading: the Quiesce guard, and it nests
            game::OriginParticipants::Quiesce q(op, "a level is loading");
            check(!op.update(cam) && reason() == "a level is loading" && untouched(), "a level loading defers the shift, and nothing moved");
            {
                game::OriginParticipants::Quiesce inner(op, "a level is unloading");
                check(!op.update(cam) && reason() == "a level is unloading", "the innermost reason shows");
            }
            check(!op.update(cam) && reason() == "a level is loading", "and the outer one returns when the inner one ends");
        }
        busyNow = true;
        check(!op.update(cam) && reason() == "a play transition" && untouched(), "the host's busy hook (a play transition) defers");
        busyNow = false;
        gestureActive = true;
        check(!op.update(cam) && reason() == "a drag is in progress" && untouched() && !gestureCancelled, "the editor's gesture gate defers");
        check(op.service().stats().deferrals == 5, "every refusal is counted (" + std::to_string(op.service().stats().deferrals) + ")");

        // urgent (the camera beyond 10 km): the gate does not defer on a gesture, the editor cancels it and accepts
        Vec3 farCam{1100000.0f, 0.0f, 0.0f};
        check(op.update(farCam) && sawUrgent && gestureCancelled, "an urgent shift reaches the editor, which cancels its gesture and accepts");
        check(op.generation() == 1 && dev.calls == 1, "and it committed once");

        {   // the flat LevelStreaming follows a shift (absolute f64 bounds, viewers converted each tick); LevelInstances do not yet
            game::GameContent content;
            RecDev sdev;
            fmt::OcWorldData lw;
            std::string why;
            check(fmt::parseOcworld("OCWORLD 1\nPLACE Meshes/cube.ocmesh 100 0 0 0 0 0 1\n", lw, &why), "the streaming fixture parses: " + why);
            lw.stream.enabled = true;
            lw.stream.cellCm = 5000.0f;
            lw.stream.loadCm = 10000.0f;
            lw.stream.evictCm = 15000.0f;
            game::GameLevel level;
            world::InstantiateOptions opt;
            opt.createBodies = false;
            level.streaming().begin(lw, content, sdev, opt, {});
            check(level.streaming().active(), "LevelStreaming is active");
            op.bindings().level = &level;
            Vec3 c2{950000.0f, 0.0f, 0.0f};
            const u32 gen = op.generation();
            check(op.update(c2) && op.generation() == gen + 1, "the gate lets a shift through while LevelStreaming is active");
            check(level.streaming().origin() == op.origin(), "and the streamer was told the new origin (" + std::to_string(level.streaming().origin().x) + ")");
            op.resetForLevel();
            check(level.streaming().origin() == world::OriginCm{}, "a level reset names the origin to it too");
            level.streaming().end(w);
            op.bindings().level = nullptr;
        }
        clearWorld(w);
    }

    {   // the project keys, and a disabled service
        game::OriginParticipants op;
        op.configure(-1, -1, true);
        check(op.service().enabled(), "unstated WORLD.FLOATINGORIGIN is the host's default (game: on)");
        op.configure(-1, -1, false);
        check(!op.service().enabled(), "and off in the editor");
        op.configure(1, 204800, false);
        check(op.service().enabled() && op.service().settings().marginCm == 204800, "an explicit 1 beats the editor default, and the margin is taken");
        op.configure(0, -1, true);
        check(!op.service().enabled(), "an explicit 0 beats the game default");
        op.configure(1, 999999, true);
        check(op.service().settings().marginCm == world::kRebaseMarginMaxCm, "a margin past the cliff is clamped by the service");
        Vec3 cam{5000000.0f, 0.0f, 0.0f};
        op.configure(0, -1, true);
        op.install();
        check(!op.update(cam) && op.generation() == 0, "a disabled service never shifts");
    }

    {   // streaming, nav and the Synapse AI instance follow the shift; the save-skip predicate
        std::vector<scene::Entity> es;
        for (u32 i = 0; i < 10; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 100.0f, 0.0f, 0.0f}));
        w.flush();
        game::GameStreaming streaming;
        game::GameLevel level;
        fmt::OcNavData nav;
        nav.cellSizeCm = 50.0f; nav.originXCm = 1000000.0f; nav.originYCm = 20000.0f; nav.widthCells = 2; nav.heightCells = 2;
        game::OriginParticipants op;
        game::OriginBindings& b = op.bindings();
        b.streaming = &streaming;
        b.level = &level;
        b.nav = &nav;
#if ORIGINTEST_SYNAPSE
        synapse::SynapseAi ai;
        b.synapseAi = &ai;
#endif
        op.configure(1, -1, true);
        op.install();
        Vec3 cam{1000000.0f, 0.0f, 0.0f};
        check(op.update(cam), "a shift with streaming, a level and a nav grid bound");
        check(streaming.origin() == world::OriginCm{step, 0} && level.authored().origin() == world::OriginCm{step, 0},
              "GameStreaming and the cooked-chunk streamer name the new origin");
        check(nav.originXCm == 1000000.0f - kStep && nav.originYCm == 20000.0f, "the nav grid origin moved by -delta on X and Y only");
        check(!game::OriginParticipants::skipStreamed(es[0], &op) && !op.ownsStreamed(es[0]), "an ordinary entity is saved");
        check(!game::OriginParticipants::skipStreamed(es[0], nullptr), "and a null context skips nothing");
        op.resetForLevel();
        check(streaming.origin() == world::OriginCm{} && level.authored().origin() == world::OriginCm{}, "resetForLevel names the origin to the streamers too");
        clearWorld(w);
    }

#if ORIGINTEST_PHYSICS
    {   // Jolt in the same slot: no world is not a refusal; a world shifts by -delta; the position the ABI reports follows
        aver_phys_shutdown();
        game::OriginParticipants op;
        op.configure(1, -1, true);
        op.install();
        Vec3 cam{1000000.0f, 0.0f, 0.0f};
        check(op.update(cam) && op.generation() == 1, "with no physics world the shift goes through (nothing to move is not a refusal)");
        op.resetForLevel();

        aver_phys_reset_layer_collisions();
        aver_phys_init();
        const i32 box = aver_phys_add_static_box(1700000.0f, 900000.0f, 120.0f, 50.0f, 50.0f, 50.0f);
        const scene::Entity ent = spawn(w, "ent", Vec3{1700000.0f, 900000.0f, 120.0f});
        w.flush();
        Vec3 cam2{1000000.0f, 0.0f, 0.0f};
        check(op.update(cam2) && op.origin() == world::OriginCm{step, 0}, "with a world the shift goes through");
        f32 pos[3] = {0, 0, 0};
        aver_phys_body_position(box, pos);
        check(near(pos[0], 1700000.0f - kStep, 0.5f) && near(pos[1], 900000.0f, 0.5f) && near(pos[2], 120.0f, 0.5f),
              "the body moved by -delta on X (Y and Z alone): x = " + std::to_string(pos[0]));
        check(std::fabs(w.localTransform(ent).position.x - pos[0]) < 0.5f, "and agrees with the entity the scene participant shifted");
        check(aver_phys_origin_generation() == 1, "physics counted one shift");
        double tot[3] = {0, 0, 0};
        aver_phys_origin_total(tot);
        check(tot[0] == static_cast<double>(step) && tot[1] == 0.0, "and its total is one region");
        aver_phys_remove_body(box);
        clearWorld(w);
        aver_phys_shutdown();
        op.resetForLevel();   // the sequencer's origin is process-wide: leave it at zero
    }
#endif
}

#if AVER_MODULE_FRAMEWORK
// The script seam (framework_origin_abi.h): the provider the host installs, the shift a script requests, and the event with its payload.
namespace seamtest {
struct Heard {
    int count = 0;
    int64_t dx = 0, dy = 0, gen = 0, ox = 0, oy = 0;
    int32_t sender = -1, target = -1, args = 0;
};
Heard g_heard;
int32_t AVER_FW_CALL onEvent(int32_t, int64_t, void*) {
    ++g_heard.count;
    g_heard.dx = aver_fw_event_current_arg_int(0);
    g_heard.dy = aver_fw_event_current_arg_int(1);
    g_heard.gen = aver_fw_event_current_arg_int(2);
    g_heard.ox = aver_fw_event_current_arg_int(3);
    g_heard.oy = aver_fw_event_current_arg_int(4);
    g_heard.sender = aver_fw_event_current_sender();
    g_heard.target = aver_fw_event_current_target();
    g_heard.args = aver_fw_event_current_arg_count();
    return 0;
}
} // namespace seamtest

static void testScriptSeam(scene::World& w) {
    using namespace seamtest;
    AVER_INFO("=== Script seam (origin ABI, request_shift, world.origin_changed) ===");
    clearWorld(w);
    constexpr i64 step = world::kOriginStepCm;
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(&onEvent, nullptr);
    aver_fw_event_subscribe("world.origin_changed", 0, 0, 0, 1);

    {   // not allowed by default: the provider answers, the request is refused and nothing moves
        game::OriginParticipants op;
        op.configure(1, -1, true);
        op.install();
        int64_t xy[2] = {-1, -1};
        check(aver_fw_origin_get(xy) == 1 && xy[0] == 0 && xy[1] == 0 && aver_fw_origin_generation() == 0 && aver_fw_origin_enabled() == 1,
              "install() makes the object the ABI's provider: origin (0,0), generation 0, enabled");
        check(aver_fw_origin_request_shift(1, 0) == 0 && !op.requestShift(1, 0), "without WORLD.ALLOWSHIFTREQUEST a script's request is refused");
        Vec3 cam{0.0f, 0.0f, 0.0f};
        check(!op.update(cam) && op.generation() == 0, "and nothing moved");
    }
    check(aver_fw_origin_enabled() == 0, "the provider is gone with the object that installed it");

    {   // allowed: queued, run at the next slot S0, the event carries the shift
        std::vector<scene::Entity> es;
        for (u32 i = 0; i < 20; ++i) es.push_back(spawn(w, "s", Vec3{static_cast<f32>(i) * 500.0f, 3000.0f, 0.0f}));
        w.flush();
        Vec3 cam{100.0f, 200.0f, 50.0f};
        game::OriginParticipants op;
        op.bindings().positions = {&cam};
        op.configure(1, -1, true, 1);
        world::WorldOrigin::Settings manual = op.service().settings();
        manual.policy = world::OriginPolicy::Manual;   // under FollowCamera the camera pulls a requested shift straight back
        op.service().configure(manual);
        op.install();
        check(aver_fw_origin_request_shift(0, 0) == 0, "a zero request is refused");
        check(aver_fw_origin_request_shift(2, -1) == 1, "an allowed request is queued");
        check(op.origin() == world::OriginCm{} && op.generation() == 0, "queued means not done: the origin is where it was");
        double absBefore[3];
        aver_fw_origin_to_abs(w.localTransform(es[3]).position.x, w.localTransform(es[3]).position.y, 0.0f, absBefore);
        g_heard = Heard{};
        check(op.update(cam), "the next update (slot S0) runs the requested shift");
        check(op.origin() == world::OriginCm{2 * step, -step} && op.generation() == 1, "two regions along +X and one along -Y: generation 1");
        int64_t xy[2] = {};
        check(aver_fw_origin_get(xy) == 1 && xy[0] == 2 * step && xy[1] == -step && aver_fw_origin_generation() == 1, "the origin ABI reports the new origin");
        w.flush();
        double absAfter[3];
        aver_fw_origin_to_abs(w.localTransform(es[3]).position.x, w.localTransform(es[3]).position.y, 0.0f, absAfter);
        check(std::fabs(absAfter[0] - absBefore[0]) < 0.01 && std::fabs(absAfter[1] - absBefore[1]) < 0.01,
              "an entity's local position moved by -delta and O by +delta, so its absolute (to_abs) is the same place");
        check(g_heard.count == 0, "the event is posted, not dispatched inside the commit");
        check(aver_fw_events_pending() == 1, "one world.origin_changed is waiting");
        aver_fw_events_flush();
        check(g_heard.count == 1 && g_heard.args == 5, "a flush delivers it once with five payload values");
        check(g_heard.dx == 2 * step && g_heard.dy == -step && g_heard.gen == 1 && g_heard.ox == 2 * step && g_heard.oy == -step,
              "the payload is dxCm, dyCm, generation, originX, originY");
        check(g_heard.sender == 0 && g_heard.target == 0, "sender and target are 0");

        // a request that cannot run is dropped, not retried
        {
            game::OriginParticipants::Quiesce quiet(op, "a level is loading");
            check(aver_fw_origin_request_shift(1, 0) == 1, "a request is accepted while busy");
            check(!op.update(cam) && op.generation() == 1, "but the busy frame refuses it");
        }
        check(!op.update(cam) && op.generation() == 1 && aver_fw_events_pending() == 0, "and it is not retried: the next frame shifts nothing and posts nothing");

        // the origin off: no request is queued
        world::WorldOrigin::Settings off = op.service().settings();
        off.enabled = false;
        op.service().configure(off);
        check(aver_fw_origin_enabled() == 0 && aver_fw_origin_request_shift(1, 0) == 0, "with the origin off the ABI says so and refuses requests");
        clearWorld(w);
    }

    aver_fw_events_clear();
    aver_fw_set_event_dispatch(nullptr, nullptr);
}
#endif // AVER_MODULE_FRAMEWORK
#endif // ORIGINTEST_WORLD

int main() {
    AVER_INFO("OriginParticipantsTest");
    scene::World& w = scene::World::instance();

    testPlayMobility(w);
#if ORIGINTEST_ANIM
    testAnim(w);
#endif
    testSequencer(w);
#if ORIGINTEST_CAMERA && ORIGINTEST_PHYSICS
    testCameraRig(w);
#endif
#if ORIGINTEST_SOFTBODY && ORIGINTEST_PHYSICS
    testSoftBody(w);
#endif
#if ORIGINTEST_HAIR
    testHair();
#endif
#if ORIGINTEST_PHYSICS
    testKinematicDriver(w);
#endif
#if ORIGINTEST_SYNAPSE
    testSynapse(w);
#endif
#if ORIGINTEST_AUDIO
    testAudio();
#endif
#if ORIGINTEST_WORLD
    testOriginService(w);
    testRealParticipants(w);
#if AVER_MODULE_FRAMEWORK
    testScriptSeam(w);
#endif
#endif

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
