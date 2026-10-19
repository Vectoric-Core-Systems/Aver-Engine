// GameCameraTest -- Runtime/include/aver/game/GameCamera.hpp, headless: pushCamera with a PlayView (the default is today's
// camera bit for bit; FOV and roll are inputs now), the legacy boom, and drivePlayCamera with and without a camera rig.
// A recording stand-in device captures what pushCamera hands IDevice::setCamera; the framework's view request and a real
// scene::World stand in for a game. No window, no GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/game/GameCamera.hpp"
#include "aver/scene/World.hpp"

#if AVER_MODULE_CAMERA
#  include "aver/camera/CameraRigSystem.hpp"
#endif

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Records the camera pushCamera hands the device.
struct RecDevice final : rhi::IDevice {
    f32 viewProj[16] = {}, invRel[16] = {}, pos[3] = {};
    int calls = 0;
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void setCamera(const f32 vp[16], const f32 inv[16], const f32 p[3]) override {
        std::memcpy(viewProj, vp, sizeof viewProj);
        std::memcpy(invRel, inv, sizeof invRel);
        std::memcpy(pos, p, sizeof pos);
        ++calls;
    }
};

// The camera every host built before PlayView existed, restated independently.
static Mat4 oldViewProj(const Vec3& pos, const Vec3& fwd, f32 aspect) {
    const Mat4 view = Mat4::lookAtDirLH(pos, fwd, Vec3{0, 0, 1});
    const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, game::kCameraNearCm, game::kCameraFarCm);
    return view * proj;
}

// Clip-space y / w of a world point.
static float ndcY(const f32 vp[16], const Vec3& p) {
    const f32 y = p.x * vp[1] + p.y * vp[5] + p.z * vp[9] + vp[13];
    const f32 w = p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15];
    return y / w;
}

static void testPushCamera() {
    AVER_INFO("=== pushCamera and PlayView ===");
    RecDevice dev;
    bool same = true;
    const f32 yaws[] = {0.0f, 0.7f, -2.1f, 3.0f};
    const f32 pitches[] = {0.0f, 0.4f, -0.9f, 1.2f};
    const Vec3 positions[] = {{0, 0, 0}, {120, -40, 160}, {-5000, 8000, 300}, {40000, 12000, 900}};
    for (const f32 yaw : yaws)
        for (const f32 pitch : pitches)
            for (const Vec3& pos : positions) {
                const Vec3 fwd = game::cameraForward(yaw, pitch);
                const Mat4 want = oldViewProj(pos, fwd, 16.0f / 9.0f);
                const game::CameraMatrices a = game::pushCamera(dev, pos, fwd, 16.0f / 9.0f);
                const bool a4 = std::memcmp(dev.viewProj, want.m, sizeof dev.viewProj) == 0 && std::memcmp(&a.viewProj, &want, sizeof want) == 0;
                const game::CameraMatrices b = game::pushCamera(dev, pos, fwd, 16.0f / 9.0f, game::PlayView{});
                const bool a5 = std::memcmp(dev.viewProj, want.m, sizeof dev.viewProj) == 0 && std::memcmp(&a.invVP, &b.invVP, sizeof a.invVP) == 0;
                same = same && a4 && a5;
            }
    check(same, "the four-argument overload and a default PlayView are today's camera, bit for bit, over a grid of poses");

    const game::PlayView def;
    check(def.fovYRad == radians(60.0f) && def.rollRad == 0.0f && def.nearCm == game::kCameraNearCm && def.farCm == game::kCameraFarCm &&
              !def.ownerHideAll && !def.cut,
          "a default PlayView is 60 degrees, no roll, 2 / 200000, no owner-hide-all, no cut");

    const Vec3 fwd{1, 0, 0}, eye{0, 0, 0};
    game::pushCamera(dev, eye, fwd, 1.0f);
    const float y60 = ndcY(dev.viewProj, Vec3{100, 0, 100});
    game::PlayView wide;
    wide.fovYRad = radians(90.0f);
    game::pushCamera(dev, eye, fwd, 1.0f, wide);
    const float y90 = ndcY(dev.viewProj, Vec3{100, 0, 100});
    check(near(y60, 1.0f / std::tan(radians(30.0f)), 1e-3f) && near(y90, 1.0f, 1e-3f), "FOV 90 changes the projection by 1/tan(fov/2): " + f2s(y60) + " -> " + f2s(y90));
    check(near(y60 / y90, std::tan(radians(45.0f)) / std::tan(radians(30.0f)), 1e-3f), "the ratio is tan(45) / tan(30)");

    check(game::rolledUp(fwd, 0.0f).x == 0.0f && game::rolledUp(fwd, 0.0f).y == 0.0f && game::rolledUp(fwd, 0.0f).z == 1.0f, "roll 0 builds exactly the world up");
    const Vec3 up30 = game::rolledUp(fwd, radians(30.0f));
    check(near(up30.x, 0.0f, 1e-6f) && near(up30.y, 0.5f, 1e-5f) && near(up30.z, std::cos(radians(30.0f)), 1e-5f), "roll 30 turns the up vector about forward");
    check(near(up30.size(), 1.0f, 1e-5f) && near(dot(up30, fwd), 0.0f, 1e-6f), "and stays a unit vector perpendicular to it");
    game::PlayView rolled;
    rolled.rollRad = radians(30.0f);
    game::pushCamera(dev, eye, fwd, 1.0f, rolled);
    const float level = ndcY(dev.viewProj, Vec3{100, 0, 100});
    check(level != y60 && near(std::fabs(level), 1.0f / std::tan(radians(30.0f)) * 0.866f, 0.05f), "a rolled camera's view differs from the level one (a point above the axis moves toward the side)");
}

// A pawn the legacy boom can follow without a session: the stand-in path.
struct Pawn {
    scene::World& w = scene::World::instance();
    scene::Entity e;
    explicit Pawn(Vec3 p, float yawDeg = 0.0f) {
        e = w.create("pawn");
        Transform xf;
        xf.position = p;
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(yawDeg));
        w.setLocalTransform(e, xf);
        w.flush();
    }
    ~Pawn() { w.destroy(e); w.flush(); }
};

static void testLegacyBoom() {
    AVER_INFO("=== the legacy boom is unchanged ===");
    aver_fw_set_view_entity(0);
    Pawn pawn(Vec3{100, 0, 0});

    aver_fw_set_view(AVER_FW_VIEW_THIRD_PERSON, 160.0f, 450.0f);
    Vec3 pos{9, 9, 9};
    f32 yaw = 9.0f, pitch = 9.0f;
    scene::Entity hide = game::drivePlayCamera(pos, yaw, pitch, pawn.e);
    check(hide == scene::kInvalidEntity && near(pos.x, -350.0f, 1e-3f) && near(pos.z, 160.0f, 1e-3f) && yaw == 0.0f && pitch == 0.0f,
          "third person: the camera is the pivot (pawn + eye height) minus the boom, looking where the pawn faces");

    Vec3 pos2{9, 9, 9};
    f32 yaw2 = 9.0f, pitch2 = 9.0f;
    game::PlayView view;
    view.fovYRad = 1.0f;
    const scene::Entity hide2 = game::drivePlayCamera(pos2, yaw2, pitch2, view, 0.016f, pawn.e);
    check(hide2 == hide && std::memcmp(&pos, &pos2, sizeof pos) == 0 && yaw == yaw2 && pitch == pitch2,
          "with no rig the PlayView overload equals the legacy result bit for bit");
    check(view.fovYRad == radians(60.0f) && view.rollRad == 0.0f && !view.ownerHideAll && !view.cut, "and leaves PlayView at its defaults");

    aver_fw_set_view(AVER_FW_VIEW_FIRST_PERSON, 160.0f, 450.0f);
    hide = game::drivePlayCamera(pos, yaw, pitch, pawn.e);
    check(hide == pawn.e && near(pos.x, 100.0f, 1e-3f) && near(pos.z, 160.0f, 1e-3f), "first person: at the eyes, and the pawn is returned to hide");
    const scene::Entity hide3 = game::drivePlayCamera(pos2, yaw2, pitch2, view, 0.016f, pawn.e);
    check(hide3 == pawn.e && !view.ownerHideAll, "with no rig the first-person hide is unchanged and is not hide-all");

    aver_fw_set_view(AVER_FW_VIEW_THIRD_PERSON, 160.0f, 0.0f);
    game::drivePlayCamera(pos, yaw, pitch, pawn.e);
    check(near(pos.x, 100.0f, 1e-3f) && near(pos.z, 160.0f, 1e-3f) && yaw == 0.0f, "a zero boom looks where the pawn looks, from the pivot");

    Vec3 untouched{4, 5, 6};
    f32 uy = 1.0f, up = 2.0f;
    check(game::drivePlayCamera(untouched, uy, up) == scene::kInvalidEntity && untouched.x == 4.0f && uy == 1.0f && up == 2.0f,
          "with no session and no stand-in, nothing is touched");
    aver_fw_set_view(AVER_FW_VIEW_THIRD_PERSON, 160.0f, 450.0f);
}

#if AVER_MODULE_CAMERA
static void testWithRig() {
    AVER_INFO("=== drivePlayCamera with a rig ===");
    scene::World& w = scene::World::instance();
    camera::CameraRigSystem::registerComponents(w);
    camera::CameraRigSystem& sys = camera::cameraRigSystem();
    sys.install(w);
    sys.clear();
    aver_fw_set_view_entity(0);
    aver_fw_set_view(AVER_FW_VIEW_THIRD_PERSON, 160.0f, 450.0f);

    Pawn pawn(Vec3{100, 0, 0});
    const scene::Entity re = w.create("rig", pawn.e, Transform{});
    camera::CCameraRig* rig = camera::CameraRigSystem::attach(w, re, camera::CameraPreset::Chase, pawn.e);
    rig->pivotZ = 160.0f;
    rig->flags |= camera::kRigNoCollision;
    w.component<scene::CCamera>(re, scene::kComponentCamera)->fovYRad = radians(90.0f);
    w.flush();

    Vec3 legacyPos{};
    f32 legacyYaw = 0, legacyPitch = 0;
    game::drivePlayCamera(legacyPos, legacyYaw, legacyPitch, pawn.e);

    Vec3 pos{};
    f32 yaw = 0, pitch = 0;
    game::PlayView view;
    scene::Entity hide = game::drivePlayCamera(pos, yaw, pitch, view, 0.016f, pawn.e);
    check(hide == scene::kInvalidEntity && std::fabs(pos.x - legacyPos.x) < 1e-2f && std::fabs(pos.z - legacyPos.z) < 1e-2f && yaw == legacyYaw,
          "a Chase rig with the pivot at eye height reproduces the boom, got x " + f2s(pos.x));
    check(near(view.fovYRad, radians(90.0f), 1e-5f), "and PlayView carries the rig's FOV (CCamera is read now)");
    check(sys.activeRig() == re, "the rig under the possessed pawn is the live one");

    // A roll lean reaches PlayView.
    camera::CameraModifier lean;
    lean.kind = camera::ModKind::RollLean;
    lean.a = 10.0f;
    lean.holdS = -1.0f;
    sys.pushModifier(lean);
    game::drivePlayCamera(pos, yaw, pitch, view, 0.016f, pawn.e);
    check(near(view.rollRad, radians(10.0f), 1e-4f), "a roll-lean modifier arrives as PlayView::rollRad");
    sys.clearModifiers(0);
    for (int i = 0; i < 30; ++i) game::drivePlayCamera(pos, yaw, pitch, view, 0.1f, pawn.e);

    // First person through the rig: the pawn is hidden by the owner-hide route, not hide-all.
    rig->mode = static_cast<u32>(camera::CameraMode::FirstPerson);
    for (int i = 0; i < 60; ++i) hide = game::drivePlayCamera(pos, yaw, pitch, view, 0.016f, pawn.e);
    check(hide == pawn.e && !view.ownerHideAll, "a first-person rig hides the pawn through the flagged-meshes route (ownerHideAll false)");

    // A disabled rig hands the camera back to the boom.
    rig->flags |= camera::kRigDisabled;
    for (int i = 0; i < 120; ++i) hide = game::drivePlayCamera(pos, yaw, pitch, view, 0.016f, pawn.e);
    game::drivePlayCamera(legacyPos, legacyYaw, legacyPitch, pawn.e);
    check(sys.activeRig() == scene::kInvalidEntity && std::memcmp(&pos, &legacyPos, sizeof pos) == 0 && view.fovYRad == radians(60.0f),
          "disabling the rig returns to the legacy pose and the default PlayView");

    w.destroy(re);
    w.flush();
    sys.clear();
}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("game camera");
    testPushCamera();
    testLegacyBoom();
#if AVER_MODULE_CAMERA
    testWithRig();
#else
    AVER_INFO("(built without Aver.Camera: the rig cases are skipped)");
#endif
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
