// Aver.World: a placement with an `anim` clip becomes an animated entity and, if it collides, a
// kinematic body the host can drive. Exit code = failure count.
//
// Same shape as LevelInstanceTest: the world is a string literal, nothing needs a GPU or a file, and
// every assertion is on entity or body state. The physics half needs aver_phys_init, which a headless
// process can do; it is compiled out when the tree has no physics module.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelInstance.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_joints_abi.h"
#endif

#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) <= tol) return;
    AVER_ERROR("   FAIL  {}: got {} want {}", what, got, want);
    ++g_failures;
}

// 0: an animated, colliding placement that plays once at twice the speed from half a second in.
// 1: an animated placement that does not collide. 2: a plain colliding placement, the control.
// The cubes are scaled up so the collision hull around the unit cube is a metre a side, not two cm.
static const char* kWorldText = R"(OCWORLD 1
NAME AnimLevel
PLACEG Meshes/cube.ocmesh 100 200 50 0 0 0 100 100 100 M_Car anim Animations/car.ocanim animspeed 2 animtime 0.5 animonce
PLACEG Meshes/cube.ocmesh 5000 0 0 0 0 0 1 1 1 nocollide anim Animations/fan.ocanim
PLACEG Meshes/cube.ocmesh -3000 0 0 0 0 0 100 100 100
)";

// A held animated root (animspeed 0) with a subtree: 1 collides, 2 does not and has a child of its own
// (3, which collides), and 4 is a plain placement after the subtree, the control. A child's authored
// offset is scaled by its parent's scale: the root is 100, so 1 is 300 cm along +X from it, 2 is 300 cm
// along +Y, and 3 is 6 * 50 = 300 cm above 2. World scales: 50 for 1 and 2, 20 for 3.
static const char* kNestedWorldText = R"(OCWORLD 1
NAME NestedLevel
PLACEG Meshes/cube.ocmesh 20000 0 0 0 0 0 100 100 100 anim Animations/car.ocanim animspeed 0
BEGIN
CHILDG Meshes/cube.ocmesh 3 0 0 0 0 0 0.5 0.5 0.5
CHILDG Meshes/cube.ocmesh 0 3 0 0 0 0 0.5 0.5 0.5 nocollide
BEGIN
CHILDG Meshes/cube.ocmesh 0 0 6 0 0 0 0.4 0.4 0.4
END
END
PLACEG Meshes/cube.ocmesh 30000 0 0 0 0 0 100 100 100
)";

int main() {
    fmt::OcWorldData w;
    std::string why;
    check(fmt::parseOcworld(kWorldText, w, &why), "the test world parses: " + why);
    check(w.placements.size() == 3, "three placements parsed");
    if (w.placements.size() != 3) {
        AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
        return g_failures ? g_failures : 1;
    }

    // ---- the record ---------------------------------------------------------------------------------
    check(w.placements[0].animClip == "Animations/car.ocanim", "anim names the clip");
    checkNear(w.placements[0].animSpeed, 2.0f, 0.0f, "animspeed parsed");
    checkNear(w.placements[0].animTime, 0.5f, 0.0f, "animtime parsed");
    check(w.placements[0].animOnce, "animonce parsed");
    check(w.placements[0].material == "M_Car", "the material fallback still finds the surface");
    check(w.placements[1].animClip == "Animations/fan.ocanim", "a second clip parses");
    checkNear(w.placements[1].animSpeed, 1.0f, 0.0f, "animspeed defaults to 1");
    checkNear(w.placements[1].animTime, 0.0f, 0.0f, "animtime defaults to 0");
    check(!w.placements[1].animOnce, "a placement loops unless animonce is present");
    check(w.placements[2].animClip.empty(), "an unmarked placement has no clip");

    // ---- the record writes back only when set ------------------------------------------------------
    {
        const std::string out = fmt::writeOcworld(w);
        fmt::OcWorldData back;
        std::string bwhy;
        check(fmt::parseOcworld(out, back, &bwhy), "the written world re-parses: " + bwhy);
        check(back.placements.size() == 3, "the round trip keeps every placement");
        if (back.placements.size() == 3) {
            check(back.placements[0].animClip == "Animations/car.ocanim", "anim survives a round trip");
            checkNear(back.placements[0].animSpeed, 2.0f, 0.0f, "animspeed survives a round trip");
            checkNear(back.placements[0].animTime, 0.5f, 0.0f, "animtime survives a round trip");
            check(back.placements[0].animOnce, "animonce survives a round trip");
            check(back.placements[2].animClip.empty(), "a placement with no clip still has none");
        }
        fmt::OcWorldData plain;
        plain.placements.push_back(w.placements[2]);
        check(fmt::writeOcworld(plain).find(" anim") == std::string::npos,
              "a placement with no clip writes no anim tokens");
    }

    scene::World& scn = scene::World::instance();

#if AVER_MODULE_PHYSICS
    check(aver_phys_init() == 1, "physics starts");
#endif

    const world::LevelInstance inst = world::instantiate(w);
    check(inst.entities.size() == 3, "one entity per placement");
    if (inst.entities.size() != 3) {
        AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
        return g_failures ? g_failures : 1;
    }

    // ---- the animator ---------------------------------------------------------------------------------
    {
        const auto* an = scn.component<scene::CAnimator>(inst.entities[0], scene::kComponentAnimator);
        check(an != nullptr, "an animated placement gets a CAnimator");
        if (an) {
            check(an->clip == fnv1a64(std::string_view("Animations/car.ocanim")),
                  "the clip id is fnv1a64 of the content path");
            checkNear(an->time, 0.5f, 0.0f, "the clock starts at animtime");
            checkNear(an->speed, 2.0f, 0.0f, "the speed is animspeed");
            checkNear(an->blendWeight, 1.0f, 0.0f, "the blend weight is full");
            check(an->flags == scene::kAnimatorOnce, "animonce sets kAnimatorOnce and nothing else");
        }
        const auto* fan = scn.component<scene::CAnimator>(inst.entities[1], scene::kComponentAnimator);
        check(fan != nullptr && fan->flags == 0, "a looping animator carries no flags");
        check(scn.component<scene::CAnimator>(inst.entities[2], scene::kComponentAnimator) == nullptr,
              "a placement with no clip gets no CAnimator");
    }

#if AVER_MODULE_PHYSICS
    // ---- the bodies -----------------------------------------------------------------------------------
    check(inst.animatedBodies.size() == 1, "exactly one animated body: the colliding animated placement");
    if (inst.animatedBodies.size() == 1) {
        const world::AnimatedBody& ab = inst.animatedBodies[0];
        check(ab.entity == inst.entities[0], "the animated body belongs to the animated placement");
        check(ab.body > 0 && aver_phys_body_motion_type(ab.body) == AVER_PHYS_MOTION_KINEMATIC,
              "its body is kinematic");
        check(inst.entityBody[0] == ab.body, "entityBody names the same body");

        // A body whose entity was moved by the animation tick follows it, from the pivot.
        const Vec3 target{130.0f, 200.0f, 50.0f};
        scn.setLocalPosition(ab.entity, target);
        const f32 step = aver_phys_fixed_step();
        world::driveKinematicBodies(scn, inst.animatedBodies, step);
        aver_phys_step(step);
        f32 pos[3] = {0, 0, 0};
        check(aver_phys_body_position(ab.body, pos) == 1, "the body position reads back");
        checkNear(pos[0], target.x, 1.0f, "driven body x");
        checkNear(pos[1], target.y, 1.0f, "driven body y");
        checkNear(pos[2], target.z, 1.0f, "driven body z");
    }
    check(inst.entityBody[1] == -1, "a nocollide animated placement has no body");
    check(inst.entityBody[2] > 0 && aver_phys_body_motion_type(inst.entityBody[2]) == AVER_PHYS_MOTION_STATIC,
          "a plain colliding placement keeps its static body");

    // ---- the ABI ---------------------------------------------------------------------------------------
    {
        const i32 stat = inst.entityBody[2];
        check(aver_phys_body_move_kinematic(stat, 0, 0, 0, 0, 0, 0, 1, 0.016f) == 0,
              "a static body cannot be driven");
        check(aver_phys_body_move_kinematic(0, 0, 0, 0, 0, 0, 0, 1, 0.016f) == 0, "a dead handle cannot be driven");

        // A static box switched to kinematic is rebuilt with motion properties and then follows a target.
        const i32 box = aver_phys_add_static_box(0, 8000, 500, 50, 50, 50);
        check(box > 0, "a static box is made");
        check(aver_phys_body_set_motion_type(box, AVER_PHYS_MOTION_KINEMATIC) == 1,
              "a static body can be made kinematic");
        check(aver_phys_body_motion_type(box) == AVER_PHYS_MOTION_KINEMATIC, "and reads back as kinematic");
        const f32 step = aver_phys_fixed_step();
        check(aver_phys_body_move_kinematic(box, 100, 8000, 500, 0, 0, 0, 1, 0.0f) == 0, "a zero dt is refused");
        check(aver_phys_body_move_kinematic(box, 100, 8000, 500, 0, 0, 0, 1, step) == 1, "a kinematic body is driven");
        aver_phys_step(step);
        f32 pos[3] = {0, 0, 0};
        aver_phys_body_position(box, pos);
        checkNear(pos[0], 100.0f, 1.0f, "the rebuilt body reached its target");
        check(aver_phys_body_set_motion_type(box, AVER_PHYS_MOTION_STATIC) == 1, "and can be made static again");
        check(aver_phys_body_motion_type(box) == AVER_PHYS_MOTION_STATIC, "which reads back");

        // A static box switched to dynamic falls.
        const i32 crate = aver_phys_add_static_box(0, -8000, 10000, 50, 50, 50);
        check(aver_phys_body_set_motion_type(crate, AVER_PHYS_MOTION_DYNAMIC) == 1, "a static body can be made dynamic");
        for (int k = 0; k < 30; ++k) aver_phys_step(step);
        aver_phys_body_position(crate, pos);
        check(pos[2] < 9950.0f, "a static body made dynamic falls under gravity");
    }

    // ---- the subtree of an animated placement, a held animator, and jumps -----------------------------
    {
        fmt::OcWorldData nw;
        std::string nwhy;
        check(fmt::parseOcworld(kNestedWorldText, nw, &nwhy), "the nested world parses: " + nwhy);
        check(nw.placements.size() == 5, "five nested placements parsed");
        const world::LevelInstance nested = world::instantiate(nw);
        check(nested.entities.size() == 5, "one entity per nested placement");
        check(nested.animatedBodies.size() == 3, "three bodies are driven: the root, a child and a grandchild");
        if (nested.entities.size() == 5 && nested.animatedBodies.size() == 3) {
            const auto* held = scn.component<scene::CAnimator>(nested.entities[0], scene::kComponentAnimator);
            check(held != nullptr && (held->flags & scene::kAnimatorPaused) != 0,
                  "an authored animspeed 0 holds the animator (a CAnimator speed of 0 would play at 1)");
            check(scn.component<scene::CAnimator>(nested.entities[1], scene::kComponentAnimator) == nullptr,
                  "a child of an animated placement has no animator of its own");

            check(nested.animatedBodies[0].entity == nested.entities[0] &&
                  nested.animatedBodies[1].entity == nested.entities[1] &&
                  nested.animatedBodies[2].entity == nested.entities[3],
                  "the root and both colliding descendants are driven, through a body-less middle one");
            bool allKinematic = true;
            for (const world::AnimatedBody& ab : nested.animatedBodies)
                allKinematic = allKinematic && aver_phys_body_motion_type(ab.body) == AVER_PHYS_MOTION_KINEMATIC;
            check(allKinematic, "and every one of those bodies is kinematic");
            check(nested.entityBody[2] == -1, "a nocollide descendant has no body");
            check(nested.entityBody[4] > 0 && aver_phys_body_motion_type(nested.entityBody[4]) == AVER_PHYS_MOTION_STATIC,
                  "a plain placement after the subtree keeps its static body");

            const f32 step = aver_phys_fixed_step();
            const scene::Entity root = nested.entities[0];
            const i32 childBody = nested.animatedBodies[1].body;
            const i32 grandBody = nested.animatedBodies[2].body;
            f32 pos[3] = {0, 0, 0};

            world::driveKinematicBodies(scn, nested.animatedBodies, step);   // first sight: no history
            scn.setLocalPosition(root, Vec3{20030.0f, 0.0f, 0.0f});          // 30 cm in a step: 18 m/s
            world::driveKinematicBodies(scn, nested.animatedBodies, step);
            aver_phys_body_position(childBody, pos);
            checkNear(pos[0], 20300.0f, 1.0f, "a fast-but-sane move is driven, not teleported: the body waits for the step");
            aver_phys_step(step);
            aver_phys_body_position(childBody, pos);
            checkNear(pos[0], 20330.0f, 1.0f, "and the child body reaches the pose its animated parent carried it to");

            scn.setLocalPosition(root, Vec3{25330.0f, 0.0f, 0.0f});          // 5000 cm in a step: a jump
            world::driveKinematicBodies(scn, nested.animatedBodies, step);
            aver_phys_body_position(childBody, pos);
            checkNear(pos[0], 25630.0f, 1.0f, "a jump is a teleport: the body is at the new pose before any step");
            f32 vel[3] = {1, 1, 1};
            aver_phys_body_velocity(childBody, vel);
            checkNear(vel[0], 0.0f, 0.01f, "and is left at rest, so nothing standing on it is flung");

            scn.setLocalPosition(root, Vec3{25390.0f, 0.0f, 0.0f});          // small, but no time passed
            world::driveKinematicBodies(scn, nested.animatedBodies, 0.0f);
            aver_phys_body_position(childBody, pos);
            checkNear(pos[0], 25690.0f, 1.0f, "a step with dt 0 teleports rather than driving");
            aver_phys_body_position(grandBody, pos);
            checkNear(pos[0], 25390.0f, 1.0f, "a grandchild under a body-less parent follows too (x)");
            checkNear(pos[2], 300.0f, 1.0f, "and keeps its offset (z)");

            // ---- a turn is judged by how far the body reaches; the reach measured once must not hide a jump
            {
                const i32 rootBody = nested.animatedBodies[0].body;
                // How close the root's body is to `degrees` about Z, as |dot| of the two rotations: 1 once there.
                const auto turnTo = [&](f32 degrees) {
                    const Quat want = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, radians(degrees));
                    scn.setLocalRotation(root, want);
                    world::driveKinematicBodies(scn, nested.animatedBodies, step);
                    f32 q[4] = {0, 0, 0, 1};
                    aver_phys_body_rotation(rootBody, q);
                    return std::fabs(q[0] * want.x + q[1] * want.y + q[2] * want.z + q[3] * want.w);
                };
                check(turnTo(10.0f) < 0.9999f, "a 10 degree turn in one step is driven: the body waits for the step");
                aver_phys_step(step);
                check(turnTo(20.0f) < 0.9999f, "and so is the next one, once the body's reach has been measured");
                aver_phys_step(step);
                check(turnTo(110.0f) > 0.9999f,
                      "a 90 degree turn in one step is a jump and teleports, whatever the measured reach says");
            }

            // ---- what a host caching its list needs to know
            check(world::driveKinematicBodies(scn, nested.animatedBodies, step) == 0,
                  "a list of live kinematic bodies drives with nothing stale");
            aver_phys_remove_body(grandBody);
            check(world::driveKinematicBodies(scn, nested.animatedBodies, step) == 1,
                  "a body that is gone is reported, so a host holding the list knows to rebuild it");
        }
    }

    // ---- a joint blocks the static -> kinematic rebuild, a mesh cannot be dynamic ----------------------
    {
        const f32 step = aver_phys_fixed_step();
        const i32 anchor = aver_phys_add_static_box(0, 20000, 500, 50, 50, 50);
        const i32 swing  = aver_phys_add_dynamic_box(0, 20200, 500, 50, 50, 50, 10.0f);
        const float pivot[3] = {0.0f, 20100.0f, 500.0f};
        const i32 joint = aver_phys_joint_point(anchor, swing, pivot);
        check(anchor > 0 && swing > 0 && joint > 0, "a joint is made between a static and a dynamic body");
        check(aver_phys_body_set_motion_type(anchor, AVER_PHYS_MOTION_KINEMATIC) == 0,
              "a static body with a joint on it is not rebuilt out from under the joint");
        check(aver_phys_body_motion_type(anchor) == AVER_PHYS_MOTION_STATIC, "it stays static");
        aver_phys_step(step);   // the joint still has live bodies to solve
        check(aver_phys_joint_remove(joint) == 1, "the joint goes");
        check(aver_phys_body_set_motion_type(anchor, AVER_PHYS_MOTION_KINEMATIC) == 1,
              "and with no joint on it the switch works");

        const f32 quad[12] = {-100, -100, 0, 100, -100, 0, 100, 100, 0, -100, 100, 0};
        const i32 tris[6] = {0, 1, 2, 0, 2, 3};
        const i32 tri = aver_phys_add_mesh(quad, 4, tris, 6, 40000, 0, 0);
        check(tri > 0, "a triangle-mesh body is made");
        check(aver_phys_body_set_motion_type(tri, AVER_PHYS_MOTION_DYNAMIC) == 0,
              "a triangle mesh cannot be made dynamic: it has no mass");
        check(aver_phys_body_motion_type(tri) == AVER_PHYS_MOTION_STATIC, "it stays static");
        check(aver_phys_body_set_motion_type(tri, AVER_PHYS_MOTION_KINEMATIC) == 1, "but it can be kinematic");
        check(aver_phys_body_set_motion_type(tri, AVER_PHYS_MOTION_DYNAMIC) == 0,
              "and a kinematic mesh is still not dynamic");
    }

    aver_phys_shutdown();
#else
    check(inst.animatedBodies.empty(), "no animated bodies without the physics module");
#endif

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
