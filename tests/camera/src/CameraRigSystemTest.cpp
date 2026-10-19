// CameraRigSystemTest -- the CCameraRig component and CameraRigSystem against a REAL Jolt world (through the physics ABI)
// and the scene World, no GPU and no framework: registration and presets, the spring arm against walls / sensors /
// characters / layers, lag, the six modes, rig selection and view-target blends, the collision fade, Reduce motion, the
// sequencer hand-back, replay equality and a non-finite transform.
//
// The physics world is never stepped: bodies are exactly where they are put, so every distance is arithmetic on the
// numbers below. Scenes are built along -X (the camera sits behind a pawn facing +X) with unequal extents.
#include "aver/camera/CameraRigSystem.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/scene/World.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::camera;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static bool bitEqual(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// ---- fixtures -------------------------------------------------------------------------------------------------------

static std::vector<std::string> g_events;
static void notify(scene::Entity, const char* name, void*) { g_events.push_back(name); }
static float g_trauma = 0.0f;
static void shakeSink(float t) { g_trauma += t; }
static float g_bobAmount = -1.0f, g_bobSpeed = -1.0f;
static void bobSink(float a, float s) { g_bobAmount = a; g_bobSpeed = s; }

// Entities made by a test; destroyed (with their subtrees) when it goes out of scope.
struct Scene {
    scene::World& w = scene::World::instance();
    std::vector<scene::Entity> made;
    scene::Entity mk(const char* name, Vec3 p, scene::Entity parent = scene::kInvalidEntity, float yawDeg = 0.0f) {
        Transform xf;
        xf.position = p;
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(yawDeg));
        scene::Entity e;
        if (parent == scene::kInvalidEntity) { e = w.create(name); w.setLocalTransform(e, xf); }
        else e = w.create(name, parent, xf);
        made.push_back(e);
        return e;
    }
    ~Scene() {
        for (const scene::Entity e : made) if (w.valid(e)) w.destroy(e);
        w.flush();
    }
};

static RigFrame frameFor(scene::Entity pawn, float dt = 1.0f / 60.0f) {
    RigFrame f;
    f.dt = dt;
    f.possessed = pawn;
    f.fallback.pos = Vec3{-1000, 0, 500};
    f.fallback.yaw = 0.0f;
    f.fallback.pitch = -0.2f;
    return f;
}

static RigResult run(CameraRigSystem& sys, scene::World& w, const RigFrame& f, int n) {
    RigResult r;
    for (int i = 0; i < n; ++i) { w.flush(); r = sys.drive(w, f); }
    return r;
}

static CCameraRig* rigOf(scene::World& w, scene::Entity e) { return w.component<CCameraRig>(e, CameraRigSystem::typeId()); }
static scene::CCamera* camOf(scene::World& w, scene::Entity e) { return w.component<scene::CCamera>(e, scene::kComponentCamera); }

// A Chase rig on a child of `pawn`, following it at eye height 100.
static scene::Entity chaseRig(Scene& s, scene::Entity pawn, CameraPreset preset = CameraPreset::Chase) {
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(s.w, re, preset, pawn);
    if (r) r->pivotZ = 100.0f;
    return re;
}

// ---- tests ------------------------------------------------------------------------------------------------------------

static void testRegistration() {
    AVER_INFO("=== registration, attach, presets ===");
    scene::World& w = scene::World::instance();
    const u32 type = CameraRigSystem::registerComponents(w);
    check(type != 0, "CCameraRig registers");
    check(sizeof(CCameraRig) == 160 && w.componentSize(type) == 160, "its storage is 160 bytes");
    check(w.componentVerified(type), "the field table covers the struct byte for byte");
    const scene::FieldDesc* outArm = w.field(w.fieldId("CCameraRig.outArmCm"));
    const scene::FieldDesc* outState = w.field(w.fieldId("CCameraRig.outState"));
    const scene::FieldDesc* outFov = w.field(w.fieldId("CCameraRig.outFovDeg"));
    const scene::FieldDesc* arm = w.field(w.fieldId("CCameraRig.armLengthCm"));
    check(outArm && outState && outFov && outArm->readOnly && outState->readOnly && outFov->readOnly, "the three OUT fields are read-only");
    check(arm && !arm->readOnly, "a tuning field is writable");

    CameraRigSystem sys;
    sys.install(w);
    {   // Zero-filled storage is a working Chase rig.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        auto* r = static_cast<CCameraRig*>(w.addComponent(re, type));
        const CCameraRig zero{};
        check(r && std::memcmp(r, &zero, sizeof zero) == 0, "storage arrives zero-filled");
        const RigResult res = run(sys, w, frameFor(pawn), 5);
        check(res.active && sys.activeRig() == re, "a zero-filled rig under the possessed pawn is selected");
        check(near(res.pov.pos.x, -450.0f, 0.01f) && near(res.pov.pos.z, 0.0f, 0.01f), "and sits 450 cm behind the pawn, got " + f2s(res.pov.pos.x));
        check(res.pov.fovYRad == radians(60.0f), "FOV is 60 degrees with no CCamera");
        check(r->outArmCm > 449.0f && (r->outState & kRigStateActive), "the OUT fields were written");
        check(near(r->outFovDeg, 60.0f, 1e-3f), "outFovDeg reports the final FOV");
    }
    sys.clear();
    {   // attach seeds a preset and CCamera.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = s.mk("seat", Vec3{0, 0, 0}, pawn);
        const CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Seat, pawn);
        const scene::CCamera* cam = camOf(w, re);
        check(r && r->mode == static_cast<u32>(CameraMode::Seat) && r->preset == static_cast<u32>(CameraPreset::Seat) && r->target == pawn, "attach seeds the Seat preset and the target");
        check(cam && cam->nearCm == 2.0f && cam->farCm == 200000.0f && cam->fovYRad == radians(60.0f), "and adds CCamera with near 2, far 200000, 60 degrees");
        check(CameraRigSystem::attach(w, scene::kInvalidEntity, CameraPreset::Chase) == nullptr, "attach on no entity is null");
    }
    {   // kRigApplyPreset expands then clears.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        auto* r = static_cast<CCameraRig*>(w.addComponent(re, type));
        r->preset = static_cast<u32>(CameraPreset::Orbit);
        r->flags = kRigApplyPreset;
        r->target = pawn;
        run(sys, w, frameFor(pawn), 2);
        check(r->mode == static_cast<u32>(CameraMode::Orbit) && r->armLengthCm == 500.0f && !(r->flags & kRigApplyPreset) && r->target == pawn,
              "kRigApplyPreset expands the preset over the tuning fields, clears the bit and keeps the target");
        check(camOf(w, re) != nullptr, "and ensures CCamera");
    }
    sys.clear();
}

static void testSpringArm() {
    AVER_INFO("=== spring arm against Jolt ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    aver_phys_init();
    {
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = chaseRig(s, pawn);
        const int32_t wall = aver_phys_add_static_box(-210, 0, 100, 10, 300, 300);   // face at x = -200
        RigResult res = run(sys, w, frameFor(pawn), 30);
        check(near(res.pov.pos.x, -188.0f, 0.8f), "a wall 200 cm behind the pivot: the camera stops at 200 - 12 = 188, got " + f2s(res.pov.pos.x));
        check(res.pov.pos.x > -200.0f, "and never passes the wall");
        check((rigOf(w, re)->outState & kRigStateColliding) != 0, "the rig reports colliding");
        check(near(rigOf(w, re)->outArmCm, 188.0f, 0.8f), "outArmCm is the shortened arm");

        // Recovery: remove the wall, one half-life (0.25 s = 15 frames) later the arm is halfway back.
        aver_phys_remove_body(wall);
        res = run(sys, w, frameFor(pawn), 15);
        const float expect = 188.0f + (450.0f - 188.0f) * 0.5f;
        check(near(-res.pov.pos.x, expect, expect * 0.05f), "the arm recovers outward with the half-life: 188 -> ~319, got " + f2s(-res.pov.pos.x));
        res = run(sys, w, frameFor(pawn), 240);
        check(near(res.pov.pos.x, -450.0f, 0.5f), "and arrives at 450");

        // A pivot hugging a wall (face 5 cm behind, probe radius 12): minArm, not zero.
        const int32_t hug = aver_phys_add_static_box(-15, 0, 100, 10, 300, 300);     // face at x = -5
        res = run(sys, w, frameFor(pawn), 5);
        check(near(dist(res.pov.pos, Vec3{0, 0, 100}), 20.0f, 0.5f), "a probe that starts overlapping gives minArmCm (20), got " + f2s(dist(res.pov.pos, Vec3{0, 0, 100})));
        aver_phys_remove_body(hug);
    }
    sys.clear();
    {   // A sensor in the path is ignored.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        chaseRig(s, pawn);
        aver_phys_add_sensor_box(-150, 0, 100, 30, 200, 200);
        const RigResult res = run(sys, w, frameFor(pawn), 20);
        check(near(res.pov.pos.x, -450.0f, 0.5f), "a trigger volume in the path does not stop the camera, got " + f2s(res.pov.pos.x));
    }
    aver_phys_shutdown();
    sys.clear();

    aver_phys_init();
    {   // The target's character and an explicit ignoreBody are skipped.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = chaseRig(s, pawn);
        const int32_t ch = aver_phys_character_create(30, 180, -100, 0, 100);
        aver_phys_set_entity(ch, 0);
        RigResult res = run(sys, w, frameFor(pawn), 20);
        check(res.pov.pos.x > -100.0f, "an unowned character in the path blocks the probe, got " + f2s(res.pov.pos.x));
        aver_phys_set_entity(ch, static_cast<int32_t>(pawn));
        res = run(sys, w, frameFor(pawn), 240);
        check(near(res.pov.pos.x, -450.0f, 0.5f), "stamped with the target's entity it is skipped, got " + f2s(res.pov.pos.x));
        aver_phys_character_destroy(ch);

        const int32_t box = aver_phys_add_static_box(-110, 0, 100, 10, 300, 300);   // face at x = -100
        res = run(sys, w, frameFor(pawn), 20);
        check(near(res.pov.pos.x, -88.0f, 0.8f), "a body in the path stops it at 100 - 12 = 88, got " + f2s(res.pov.pos.x));
        rigOf(w, re)->ignoreBody = box;
        res = run(sys, w, frameFor(pawn), 240);
        check(near(res.pov.pos.x, -450.0f, 0.5f), "ignoreBody skips it, got " + f2s(res.pov.pos.x));
        aver_phys_remove_body(box);   // the physics world outlives this block; a leftover wall would stop the layer test at 88
    }
    sys.clear();
    {   // Layers: the cloth layer is ignored by the default mask and hit when the rig names it.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = chaseRig(s, pawn);
        sys.options().defaultProbeMask = ~((1u << 14) | (1u << 15));
        const int32_t wall = aver_phys_add_static_box(-210, 0, 100, 10, 300, 300);
        aver_phys_body_set_layer(wall, 14);
        RigResult res = run(sys, w, frameFor(pawn), 20);
        check(near(res.pov.pos.x, -450.0f, 0.5f), "a body on layer 14 is ignored by the default mask, got " + f2s(res.pov.pos.x));
        rigOf(w, re)->collisionMask = 0xFFFFFFFFu;
        res = run(sys, w, frameFor(pawn), 5);
        check(near(res.pov.pos.x, -188.0f, 0.8f), "and hit when collisionMask includes it, got " + f2s(res.pov.pos.x));
        sys.options().defaultProbeMask = 0xFFFFFFFFu;
    }
    aver_phys_shutdown();
    sys.clear();
}

static void testFade() {
    AVER_INFO("=== collision fade ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    aver_phys_init();
    {
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = chaseRig(s, pawn);
        const int32_t wall = aver_phys_add_static_box(-60, 0, 100, 10, 300, 300);   // face at x = -50 -> arm 38
        RigResult res = run(sys, w, frameFor(pawn), 10);
        check(res.hideAll && res.ownerHideRoot == pawn, "a squashed arm hides the target's meshes (hideAll, root = the pawn)");
        check((rigOf(w, re)->outState & kRigStateFaded) != 0, "and the rig reports faded");

        aver_phys_remove_body(wall);
        int flips = 0;
        bool last = true, everFalse = false;
        for (int i = 0; i < 240; ++i) {
            res = run(sys, w, frameFor(pawn), 1);
            if (res.hideAll != last) ++flips;
            last = res.hideAll;
            everFalse = everFalse || !res.hideAll;
        }
        check(everFalse && !res.hideAll && flips == 1, "it returns once, without flicker, as the arm recovers past endCm * 1.15 (flips " + std::to_string(flips) + ")");

        rigOf(w, re)->flags |= kRigNoFade;
        const int32_t wall2 = aver_phys_add_static_box(-60, 0, 100, 10, 300, 300);
        res = run(sys, w, frameFor(pawn), 10);
        check(!res.hideAll && res.ownerHideRoot == scene::kInvalidEntity, "kRigNoFade disables it");
        aver_phys_remove_body(wall2);
    }
    aver_phys_shutdown();
    sys.clear();
}

static void testLag() {
    AVER_INFO("=== lag, teleport ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
    r->flags |= kRigNoCollision;
    r->posLagSpeed = 8.0f;
    r->lagMaxDistCm = 100.0f;
    RigResult res = run(sys, w, frameFor(pawn), 5);
    check(near(res.pov.pos.x, -450.0f, 0.01f), "at rest the camera is 450 behind");

    w.setLocalPosition(pawn, Vec3{400, 0, 0});
    float worst = 0.0f;
    for (int i = 0; i < 60; ++i) {
        res = run(sys, w, frameFor(pawn), 1);
        worst = std::fmax(worst, 400.0f - (res.pov.pos.x + 450.0f));
        if (res.cut) check(false, "a 400 cm step is not a teleport");
    }
    check(worst <= 100.05f && worst > 90.0f, "lagMaxDistCm bounds the gap every frame (worst " + f2s(worst) + ")");
    res = run(sys, w, frameFor(pawn), 240);
    check(near(res.pov.pos.x + 450.0f, 400.0f, 1.0f), "and the camera converges on the target");
    check((rigOf(w, re)->outState & kRigStateLagging) == 0, "no longer lagging at rest");

    w.setLocalPosition(pawn, Vec3{3400, 0, 0});
    res = run(sys, w, frameFor(pawn), 1);
    check(res.cut && near(res.pov.pos.x + 450.0f, 3400.0f, 0.01f), "a 3000 cm teleport sets cut and snaps with no trail");
    res = run(sys, w, frameFor(pawn), 1);
    check(!res.cut, "and only that frame");
}

static void testModes() {
    AVER_INFO("=== modes ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    {   // FirstPerson: exactly at the view node, never lags, hides the flagged body.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity view = s.mk("view", Vec3{0, 0, 160}, pawn, 30.0f);
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::FirstPerson, pawn);
        r->posLagSpeed = 20.0f;
        RigFrame f = frameFor(pawn);
        f.viewNode = view;
        RigResult res = run(sys, w, f, 10);
        check(near(res.pov.pos.x, 0.0f, 1e-3f) && near(res.pov.pos.z, 160.0f, 1e-3f), "FirstPerson sits exactly at the view node");
        check(near(res.pov.yaw, radians(30.0f), 1e-3f), "with the view node's rotation");
        check(res.ownerHideRoot == pawn && !res.hideAll, "it hides the flagged meshes through the owner-hide (not hideAll)");
        w.setLocalPosition(pawn, Vec3{250, 0, 0});
        res = run(sys, w, f, 1);
        check(near(res.pov.pos.x, 250.0f, 0.01f), "and never lags, even with a lag speed set");
    }
    sys.clear();
    {   // Seat: rides the rig entity's transform, lag bounded, head look clamped.
        Scene s;
        const scene::Entity cart = s.mk("cart", Vec3{0, 0, 0});
        const scene::Entity seat = s.mk("seat", Vec3{50, 0, 80}, cart);
        CCameraRig* r = CameraRigSystem::attach(w, seat, CameraPreset::Seat, cart);
        Transform xf;
        xf.position = Vec3{200, 0, 0};
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(90.0f));
        run(sys, w, frameFor(cart), 5);
        w.setLocalTransform(cart, xf);
        RigResult res = run(sys, w, frameFor(cart), 1);
        const Vec3 seatWorld{200, 50, 80};
        check(dist(res.pov.pos, seatWorld) <= 20.5f && dist(res.pov.pos, seatWorld) > 1.0f, "the seat camera lags by at most 20 cm after the cart moves, gap " + f2s(dist(res.pov.pos, seatWorld)));
        res = run(sys, w, frameFor(cart), 180);
        check(dist(res.pov.pos, seatWorld) < 0.5f && near(wrapPi(res.pov.yaw - radians(90.0f)), 0.0f, 0.01f), "and settles on the seat, facing the cart's heading");
        r->yawDeg = 500.0f;
        res = run(sys, w, frameFor(cart), 2);
        check(near(wrapPi(res.pov.yaw - radians(90.0f)), radians(110.0f), radians(1.0f)) && r->yawDeg == 110.0f, "head look is clamped to the seat limit (110), got " + f2s(degrees(wrapPi(res.pov.yaw - radians(90.0f)))));
    }
    sys.clear();
    {   // TopDown: a fixed rotation while the target moves and turns.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0}, scene::kInvalidEntity, 45.0f);
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CameraRigSystem::attach(w, re, CameraPreset::TopDown, pawn);
        run(sys, w, frameFor(pawn), 5);
        Transform xf;
        xf.position = Vec3{300, -200, 0};
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(200.0f));
        w.setLocalTransform(pawn, xf);
        const RigResult res = run(sys, w, frameFor(pawn), 300);
        check(near(res.pov.yaw, 0.0f, 1e-3f) && near(res.pov.pitch, radians(-62.0f), 1e-3f), "TopDown keeps its fixed rotation while the pawn turns");
        const Vec3 fwd = Vec3{std::cos(radians(-62.0f)), 0, std::sin(radians(-62.0f))};
        const Vec3 expect = Vec3{300, -200, 0} - fwd * 1400.0f;
        check(dist(res.pov.pos, expect) < 2.0f, "and sits 1400 cm back along it, off by " + f2s(dist(res.pov.pos, expect)));
    }
    sys.clear();
    {   // Fixed with look-at converges on the target.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{800, 0, 0});
        const scene::Entity cam = s.mk("securityCam", Vec3{0, 500, 200});
        CCameraRig* r = CameraRigSystem::attach(w, cam, CameraPreset::Fixed, pawn);
        r->flags |= kRigLookAtTarget;
        r->rotLagSpeed = 6.0f;
        const RigResult res = run(sys, w, frameFor(pawn), 240);
        const Vec3 d = Vec3{800, 0, 0} - Vec3{0, 500, 200};
        const float wantYaw = std::atan2(d.y, d.x), wantPitch = std::asin(d.z / d.size());
        check(res.active && sys.activeRig() == cam, "a Fixed rig whose target is the pawn is selected");
        check(near(wrapPi(res.pov.yaw - wantYaw), 0.0f, 0.01f) && near(res.pov.pitch, wantPitch, 0.01f), "look-at converges on the target");
        check(near(res.pov.pos.x, 0.0f, 1e-3f) && near(res.pov.pos.y, 500.0f, 1e-3f), "and the camera stays where it is placed");
    }
    sys.clear();
    {   // Orbit: rotates only through its own angles; recentres when idle.
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0}, scene::kInvalidEntity, 90.0f);
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Orbit, pawn);
        r->flags |= kRigNoCollision;
        RigResult res = run(sys, w, frameFor(pawn), 30);
        check(near(res.pov.yaw, 0.0f, 1e-3f) && near(res.pov.pitch, radians(-20.0f), 1e-3f), "Orbit ignores the pawn's heading and uses its own angles");
        r->yawDeg = 40.0f;
        res = run(sys, w, frameFor(pawn), 5);
        check(near(res.pov.yaw, radians(40.0f), 1e-3f), "AddLook (a field write) turns it");
        r->pitchDeg = -300.0f;
        res = run(sys, w, frameFor(pawn), 2);
        check(near(res.pov.pitch, radians(-75.0f), 1e-3f) && near(r->pitchDeg, -75.0f, 1e-3f), "and the pitch is clamped to the limits (and written back)");
        r->recenterDelayS = 1.0f;
        res = run(sys, w, frameFor(pawn), 60 * 6);
        check(near(wrapPi(res.pov.yaw - radians(90.0f)), 0.0f, radians(3.0f)), "after the idle delay it eases back behind the target, got " + f2s(degrees(res.pov.yaw)));
    }
    sys.clear();
}

static void testSelectionAndBlend() {
    AVER_INFO("=== selection, view targets, blends, events ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    sys.setNotifySink(&notify, nullptr);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity a = s.mk("rigA", Vec3{0, 0, 0}, pawn);
    const scene::Entity b = s.mk("rigB", Vec3{0, 0, 0}, pawn);
    CameraRigSystem::attach(w, a, CameraPreset::Chase, pawn);
    CameraRigSystem::attach(w, b, CameraPreset::Chase, pawn);
    CCameraRig* ra = rigOf(w, a);   // fetched after both attaches: adding to the pool may move its storage
    CCameraRig* rb = rigOf(w, b);
    ra->flags |= kRigNoCollision; rb->flags |= kRigNoCollision;
    ra->armLengthCm = 200.0f;
    rb->armLengthCm = 450.0f;
    camOf(w, b)->priority = 5;

    g_events.clear();
    RigResult res = run(sys, w, frameFor(pawn), 60);
    check(sys.activeRig() == b, "possession picks the rig with the best CCamera.priority");
    check(near(res.pov.pos.x, -450.0f, 0.01f), "its pose is live");
    check(g_events.size() >= 1 && g_events[0] == "CameraViewTargetChanged", "the first selection raises CameraViewTargetChanged");
    g_events.clear();

    // An explicit view target overrides it and blends over the requested time.
    ViewBlend vb;
    vb.seconds = 1.0f;
    vb.curve = BlendCurve::Linear;
    check(sys.setViewTarget(a, vb), "setViewTarget accepts a rig");
    res = run(sys, w, frameFor(pawn), 30);
    check(sys.activeRig() == a && sys.blending(), "an explicit view target overrides automatic selection and starts a blend");
    check(near(sys.blendAlpha(), 0.5f, 0.01f), "at half the blend time alpha is 0.5, got " + f2s(sys.blendAlpha()));
    check(near(res.pov.pos.x, -325.0f, 2.0f), "and the pose is halfway between the two, got " + f2s(res.pov.pos.x));
    res = run(sys, w, frameFor(pawn), 40);
    check(!sys.blending() && near(res.pov.pos.x, -200.0f, 0.01f), "at the end it is the live pose and the blend is over");
    check(g_events.size() == 2 && g_events[0] == "CameraViewTargetChanged" && g_events[1] == "CameraBlendFinished", "events: changed, then finished, once each");

    // A mid-blend switch is continuous.
    g_events.clear();
    check(sys.setViewTarget(b, vb), "switch back to B");
    run(sys, w, frameFor(pawn), 20);
    const float before = sys.pose().pos.x;
    check(sys.setViewTarget(a, vb), "and to A again, mid-blend");
    res = run(sys, w, frameFor(pawn), 1);
    check(std::fabs(res.pov.pos.x - before) < 6.0f, "a mid-blend switch is continuous (moved " + f2s(std::fabs(res.pov.pos.x - before)) + " cm in one frame)");
    run(sys, w, frameFor(pawn), 80);

    // A cut snaps and raises CameraCut.
    g_events.clear();
    ViewBlend cutBlend;
    cutBlend.cut = true;
    sys.setViewTarget(b, cutBlend);
    res = run(sys, w, frameFor(pawn), 1);
    check(res.cut && near(res.pov.pos.x, -450.0f, 0.01f) && !sys.blending(), "cut = true snaps to the new rig");
    check(g_events.size() == 2 && g_events[0] == "CameraViewTargetChanged" && g_events[1] == "CameraCut", "and raises changed, then CameraCut");

    // Back to the legacy camera: blends to the host's fallback pose, then lets go.
    ViewBlend toFallback;
    toFallback.seconds = 0.5f;
    sys.setViewTarget(scene::kInvalidEntity, toFallback);
    res = run(sys, w, frameFor(pawn), 10);
    check(res.active && sys.activeRig() == scene::kInvalidEntity, "SetViewTarget(none) blends toward the fallback");
    res = run(sys, w, frameFor(pawn), 40);
    check(!res.active && near(res.pov.pos.x, -1000.0f, 0.01f), "and then hands the fallback back untouched (inactive)");
    sys.clearViewTarget(toFallback);
    run(sys, w, frameFor(pawn), 60);
    check(sys.activeRig() == b, "clearViewTarget returns to automatic selection");
    check(!sys.setViewTarget(pawn, vb), "a view target without a rig is refused");

    // Possessing a second pawn that carries its own rig blends to it.
    Scene s2;
    const scene::Entity pawn2 = s2.mk("pawn2", Vec3{5000, 0, 0});
    const scene::Entity c = s2.mk("rigC", Vec3{0, 0, 0}, pawn2);
    CameraRigSystem::attach(w, c, CameraPreset::Chase, pawn2);
    rigOf(w, c)->flags |= kRigNoCollision;
    res = run(sys, w, frameFor(pawn2), 5);
    check(sys.activeRig() == c && sys.blending(), "possessing a second pawn blends to its rig automatically");
    run(sys, w, frameFor(pawn2), 60);
    res = run(sys, w, frameFor(pawn), 5);
    check(sys.activeRig() == b, "and unpossessing returns to the first");
    sys.clear();
}

static void testDestroyedTarget() {
    AVER_INFO("=== a destroyed target holds, then yields ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity prop = s.mk("prop", Vec3{300, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, prop);
    r->flags |= kRigNoCollision;
    RigResult res = run(sys, w, frameFor(pawn), 30);
    const Vec3 p0 = res.pov.pos;
    check(near(p0.x, -150.0f, 0.01f), "the rig follows its own target (300 - 450)");
    w.destroy(prop);
    w.flush();
    res = run(sys, w, frameFor(pawn), 20);
    check(res.active && dist(res.pov.pos, p0) < 1e-3f, "a destroyed target holds the last pose");
    res = run(sys, w, frameFor(pawn), 100);
    check(!res.active && near(res.pov.pos.x, -1000.0f, 0.01f), "then the rig yields to the fallback");
}

static void testReduceMotion() {
    AVER_INFO("=== reduce motion ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    sys.setShakeSink(&shakeSink);
    sys.setBobSink(&bobSink);

    // Look-ahead and FOV kick.
    for (const bool allowed : {true, false}) {
        sys.clear();
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::VehicleChase, pawn);
        r->flags |= kRigNoCollision;
        RigFrame f = frameFor(pawn);
        f.motionAllowed = allowed;
        RigResult res;
        for (int i = 0; i < 120; ++i) {
            w.setLocalPosition(pawn, Vec3{33.3333f * static_cast<float>(i), 0, 0});
            res = run(sys, w, f, 1);
        }
        CameraRigSystem::DebugView dv;
        const bool have = sys.debug(re, dv);
        if (allowed) {
            check(have && dv.lead.size() > 100.0f, "with motion allowed the pivot leads the cart, lead " + f2s(dv.lead.size()));
            check(res.pov.fovYRad > radians(60.0f) + radians(4.0f), "and speed widens the FOV, got " + f2s(degrees(res.pov.fovYRad)));
        } else {
            check(have && dv.lead.size() == 0.0f, "with Reduce motion on there is no look-ahead");
            check(res.pov.fovYRad == radians(60.0f), "and no FOV kick (exactly 60)");
        }
    }

    // Position lag floor.
    for (const bool allowed : {true, false}) {
        sys.clear();
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
        r->flags |= kRigNoCollision;
        r->posLagSpeed = 2.0f;
        RigFrame f = frameFor(pawn);
        f.motionAllowed = allowed;
        run(sys, w, f, 5);
        w.setLocalPosition(pawn, Vec3{200, 0, 0});
        const RigResult res = run(sys, w, f, 6);
        const float gap = 200.0f - (res.pov.pos.x + 450.0f);
        if (allowed) check(gap > 120.0f, "a slow lag (speed 2) trails far behind after 0.1 s, gap " + f2s(gap));
        else check(gap < 20.0f, "Reduce motion floors the lag speed at 40: a stiff follow, gap " + f2s(gap));
    }

    // Punch is gated; zoom is not; bob is fed (0, 0).
    for (const bool allowed : {true, false}) {
        sys.clear();
        g_bobAmount = g_bobSpeed = -1.0f;
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity view = s.mk("view", Vec3{0, 0, 160}, pawn);
        const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
        CameraRigSystem::attach(w, re, CameraPreset::FirstPerson, pawn);
        RigFrame f = frameFor(pawn);
        f.viewNode = view;
        f.motionAllowed = allowed;
        run(sys, w, f, 5);
        const float basePitch = sys.pose().pitch;
        CameraModifier punch;
        punch.kind = ModKind::Punch; punch.a = 10.0f; punch.holdS = -1.0f;
        CameraModifier zoom;
        zoom.kind = ModKind::FovScale; zoom.a = 0.5f; zoom.holdS = -1.0f;
        sys.pushModifier(punch);
        sys.pushModifier(zoom);
        const RigResult res = run(sys, w, f, 3);
        check(near(res.pov.pitch - basePitch, allowed ? radians(10.0f) : 0.0f, 1e-4f), allowed ? "punch kicks the view up 10 degrees" : "punch is gated off");
        check(near(res.pov.fovYRad, radians(30.0f), 1e-4f), "zoom (the player's own action) applies either way, got " + f2s(degrees(res.pov.fovYRad)));
        check(near(g_bobAmount, allowed ? 0.5f : 0.0f, 1e-6f) && g_bobSpeed == 0.0f, allowed ? "FirstPerson feeds bob (0.5)" : "and feeds bob (0, 0) when Reduce motion is on");
    }
    sys.clear();
}

static void testExternalPose() {
    AVER_INFO("=== sequencer hand-back ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
    r->flags |= kRigNoCollision;
    RigFrame f = frameFor(pawn);
    run(sys, w, f, 30);
    f.externalActive = true;
    f.external.pos = Vec3{5000, 5000, 0};
    RigResult res = run(sys, w, f, 10);
    check(!res.active, "while the sequencer holds the view, drive reports inactive");
    f.externalActive = false;
    res = run(sys, w, f, 1);
    const float full = dist(f.external.pos, Vec3{-450, 0, 0});
    check(res.active && dist(res.pov.pos, f.external.pos) < 0.05f * full, "when it lets go the blend starts from the last external pose");
    res = run(sys, w, f, 40);
    check(near(res.pov.pos.x, -450.0f, 0.01f) && !sys.blending(), "and lands on the rig after externalBlendS");
}

static void testNoRig() {
    AVER_INFO("=== no rig, no cost ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    RigFrame f = frameFor(pawn);
    RigResult res = run(sys, w, f, 5);
    check(!res.active && sys.activeRig() == scene::kInvalidEntity && !sys.blending(), "an empty world returns inactive");
    check(!res.ownerHideRoot && !res.hideAll && !res.cut, "and touches nothing");
    // A rig that is not under the possessed pawn does not take the view.
    const scene::Entity other = s.mk("other", Vec3{1000, 0, 0});
    const scene::Entity re = s.mk("strayRig", Vec3{0, 0, 0}, other);
    CameraRigSystem::attach(w, re, CameraPreset::Chase, other);
    res = run(sys, w, f, 5);
    check(!res.active && sys.activeRig() == scene::kInvalidEntity, "a rig not under the possessed pawn never takes the view (no automatic highest-priority-anywhere rule)");
    sys.clear();
}

static void testNonFinite() {
    AVER_INFO("=== non-finite transform ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
    r->flags |= kRigNoCollision;
    RigResult res = run(sys, w, frameFor(pawn), 20);
    const Vec3 good = res.pov.pos;
    w.setLocalPosition(pawn, Vec3{std::nanf(""), 0, 0});
    res = run(sys, w, frameFor(pawn), 3);
    check(std::isfinite(res.pov.pos.x) && dist(res.pov.pos, good) < 1e-3f, "a NaN target transform is discarded and the previous pose kept");
    w.setLocalPosition(pawn, Vec3{0, 0, 0});
    res = run(sys, w, frameFor(pawn), 3);
    check(near(res.pov.pos.x, -450.0f, 0.01f), "and the follow state recovers when the transform does");
}

// A scripted 600-frame session: moves, a mode change, a view-target switch, modifiers, a shake and a cut. Returns every
// number the camera produced plus the events and modifier ids.
struct Trace { std::vector<float> v; std::vector<std::string> ev; std::vector<u32> ids; float trauma = 0; };

static Trace runScript() {
    Trace t;
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    g_events.clear();
    g_trauma = 0.0f;
    sys.setNotifySink(&notify, nullptr);
    sys.setShakeSink(&shakeSink);
    aver_phys_init();
    {
        Scene s;
        const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
        const scene::Entity a = s.mk("rigA", Vec3{0, 0, 0}, pawn);
        const scene::Entity b = s.mk("rigB", Vec3{0, 0, 0}, pawn);
        CameraRigSystem::attach(w, a, CameraPreset::VehicleChase, pawn);
        CameraRigSystem::attach(w, b, CameraPreset::Orbit, pawn);
        CCameraRig* ra = rigOf(w, a);   // fetched after both attaches: adding to the pool may move its storage
        CCameraRig* rb = rigOf(w, b);
        ra->pivotZ = 100.0f;
        rb->pivotZ = 100.0f;
        camOf(w, a)->priority = 3;
        aver_phys_add_static_box(-600, 0, 100, 10, 800, 300);
        aver_phys_add_static_box(0, 500, 100, 600, 10, 300);
        for (int f = 0; f < 600; ++f) {
            w.setLocalPosition(pawn, Vec3{300.0f * std::cos(static_cast<float>(f) * 0.02f), 300.0f * std::sin(static_cast<float>(f) * 0.02f), 0.0f});
            if (f == 100) { CameraModifier m; m.kind = ModKind::FovKick; m.a = 8.0f; m.inS = 0.05f; m.holdS = 0.1f; m.outS = 0.4f; t.ids.push_back(sys.pushModifier(m)); }
            if (f == 150) { CameraModifier m; m.kind = ModKind::Punch; m.a = 4.0f; m.b = 2.0f; m.inS = 0.02f; m.holdS = 0.0f; m.outS = 0.5f; t.ids.push_back(sys.pushModifier(m)); }
            if (f == 200) rigOf(w, a)->mode = static_cast<u32>(CameraMode::Orbit);
            if (f == 300) { ViewBlend vb; vb.seconds = 0.8f; sys.setViewTarget(b, vb); }
            if (f == 400) sys.shakeAt(Vec3{100, 0, 100}, 1500.0f, 0.7f, 2.0f);
            if (f == 450) sys.cut(scene::kInvalidEntity);
            w.flush();
            const RigResult r = sys.drive(w, frameFor(pawn));
            t.v.insert(t.v.end(), {r.pov.pos.x, r.pov.pos.y, r.pov.pos.z, r.pov.yaw, r.pov.pitch, r.pov.roll, r.pov.fovYRad, r.cut ? 1.0f : 0.0f, r.hideAll ? 1.0f : 0.0f});
        }
        t.ev = g_events;
        t.trauma = g_trauma;
    }
    aver_phys_shutdown();
    return t;
}

static void testReplay() {
    AVER_INFO("=== replay equality ===");
    const Trace a = runScript();
    const Trace b = runScript();
    bool same = a.v.size() == b.v.size() && a.ev == b.ev && a.ids == b.ids && bitEqual(a.trauma, b.trauma);
    for (usize i = 0; same && i < a.v.size(); ++i) same = bitEqual(a.v[i], b.v[i]);
    check(same, "two fresh systems running the same 600-frame script produce bit-identical poses, events and modifier ids");
    check(a.ids.size() == 2 && a.ids[0] != 0 && a.ids[1] > a.ids[0], "modifier ids are issued in call order");
    check(!a.ev.empty() && a.trauma > 0.0f, "and the script raised events and a shake");
    bool finite = true;
    for (const float v : a.v) finite = finite && std::isfinite(v);
    check(finite, "every number is finite");
}

static void testShake() {
    AVER_INFO("=== shakeAt ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    sys.setShakeSink(&shakeSink);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{0, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
    r->flags |= kRigNoCollision;
    RigResult res = run(sys, w, frameFor(pawn), 10);
    g_trauma = 0.0f;
    sys.shakeAt(res.pov.pos + Vec3{100, 0, 0}, 1000.0f, 0.8f, 2.0f);
    run(sys, w, frameFor(pawn), 1);
    check(near(g_trauma, 0.8f * 0.81f, 0.01f), "shake falls off with distance from the camera: 0.8 * (1 - 0.1)^2, got " + f2s(g_trauma));
    g_trauma = 0.0f;
    sys.shakeAt(res.pov.pos + Vec3{2000, 0, 0}, 1000.0f, 0.8f, 2.0f);
    run(sys, w, frameFor(pawn), 1);
    check(g_trauma == 0.0f, "and is zero beyond the radius");
}

static void testPreview() {
    AVER_INFO("=== preview ===");
    scene::World& w = scene::World::instance();
    CameraRigSystem sys;
    sys.install(w);
    Scene s;
    const scene::Entity pawn = s.mk("pawn", Vec3{100, 0, 0});
    const scene::Entity re = s.mk("rig", Vec3{0, 0, 0}, pawn);
    CCameraRig* r = CameraRigSystem::attach(w, re, CameraPreset::Chase, pawn);
    r->flags |= kRigNoCollision;
    r->posLagSpeed = 5.0f;
    const CCameraRig before = *r;
    const RigResult p = sys.preview(w, re, frameFor(pawn));
    check(p.active && near(p.pov.pos.x, 100.0f - 450.0f, 0.01f), "preview gives the pose with no lag and no blend");
    check(std::memcmp(&before, r, sizeof before) == 0 && sys.activeRig() == scene::kInvalidEntity, "and writes nothing: not the component, not the system");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("camera rig system");
    testRegistration();
    testSpringArm();
    testFade();
    testLag();
    testModes();
    testSelectionAndBlend();
    testDestroyedTarget();
    testReduceMotion();
    testExternalPose();
    testNoRig();
    testNonFinite();
    testShake();
    testPreview();
    testReplay();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
