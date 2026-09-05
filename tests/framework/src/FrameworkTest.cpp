// Test for Aver.Framework: the class registry, defaults, spawn, possession, managed dispatch, the
// play lifecycle and input. Drives the C ABI and reads back through the one process-global World.
// Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"

#include <cstring>
#include <string>

using namespace aver;
using namespace aver::scene;

static int g_checks   = 0;
static int g_failures = 0;

// Counts one assertion, and logs it if it failed.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Checks that declaring a class is idempotent by name, and that lookups round-trip.
static void testDeclareIdentity() {
    AVER_INFO("=== declare is idempotent by name ===");

    const int32_t a1 = aver_fw_class_declare("Idem", "");
    const int32_t a2 = aver_fw_class_declare("Idem", "");
    check(a1 != 0, "declare returns a non-zero handle");
    check(a1 == a2, "declaring the same name twice returns the SAME handle (hot-reload identity)");
    check(aver_fw_class_find("Idem") == a1, "find resolves the name to that handle");
    check(aver_fw_class_find("NoSuchClass") == 0, "an unknown name resolves to 0");
    check(std::string(aver_fw_class_name(a1)) == "Idem", "class_name round-trips the name");
    check(std::string(aver_fw_class_name(0)).empty(), "class_name of the invalid handle is empty");

    const int32_t b = aver_fw_class_declare("IdemChild", "Idem");
    check(b != 0 && b != a1, "a second class gets its own handle");
    check(aver_fw_class_parent(b) == a1, "class_parent resolves the named parent");
    check(aver_fw_class_parent(a1) == 0, "a root class has parent 0");
}

// Filled in by testDefaults, read back after spawn.
static int32_t g_actor    = 0;
static int64_t g_meshId   = 0x00000000DEADBEEFLL;
static int32_t g_material = 0;
static float   g_colour[3] = {0.10f, 0.20f, 0.30f};
static float   g_intensity = 2500.0f;
static float   g_scale[3]  = {2.0f, 2.0f, 2.0f};

// Adds components to a class, sets one default of each kind, and checks the wrong-kind refusals.
static void testDefaults() {
    AVER_INFO("=== add components + set a default of each kind ===");

    g_actor = aver_fw_class_declare("Prop", "");
    check(g_actor != 0, "the actor class declares");

    check(aver_fw_class_add_component(g_actor, static_cast<int32_t>(kComponentMeshRenderer)) == 1, "add CMeshRenderer");
    check(aver_fw_class_add_component(g_actor, static_cast<int32_t>(kComponentLight)) == 1, "add CLight");
    check(aver_fw_class_add_component(g_actor, 0) == 0, "add of component id 0 is rejected");
    check(aver_fw_class_add_component(g_actor, 9999) == 0, "add of an unregistered component is rejected");

    const int32_t fMesh  = aver_scene_field("CMeshRenderer.mesh");       // I64
    const int32_t fMat   = aver_scene_field("CMeshRenderer.material");   // I32
    const int32_t fLux   = aver_scene_field("CLight.intensityLux");      // F32
    const int32_t fCol   = aver_scene_field("CLight.colour");            // Vec3
    const int32_t fScale = aver_scene_field("CLocal.scale");             // Vec3
    check(fMesh && fMat && fLux && fCol && fScale, "the five fields resolve to dense ids");

    g_material = aver_scene_material(0, "steel");
    check(g_material > 0, "a material name resolves to a positive handle");

    check(aver_fw_class_set_default_i64(g_actor, fMesh, g_meshId) == 1, "set_default_i64 writes the mesh ObjectId");
    check(aver_fw_class_set_default_i32(g_actor, fMat, g_material) == 1, "set_default_i32 writes the material handle");
    check(aver_fw_class_set_default_f32(g_actor, fLux, g_intensity) == 1, "set_default_f32 writes the light intensity");
    check(aver_fw_class_set_default_vec(g_actor, fCol, g_colour) == 1, "set_default_vec writes the light colour");
    check(aver_fw_class_set_default_vec(g_actor, fScale, g_scale) == 1, "set_default_vec writes the CLocal scale");

    AVER_INFO("--- a wrong-kind default is rejected, and stored nowhere ---");
    check(aver_fw_class_set_default_f32(g_actor, fMesh, 1.0f) == 0, "f32 into an I64 field is rejected");
    check(aver_fw_class_set_default_i64(g_actor, fLux, 1) == 0, "i64 into an F32 field is rejected");
    check(aver_fw_class_set_default_vec(g_actor, fMesh, g_colour) == 0, "vec into an I64 field is rejected");
    check(aver_fw_class_set_default_i32(g_actor, fMesh, 7) == 0, "i32 into an I64 field is rejected");
    check(aver_fw_class_set_default_str(g_actor, fMesh, "meshes/box") == 0, "set_default_str into the I64 mesh field is rejected");
    check(aver_fw_class_set_default_str(g_actor, fMat, "steel") == 0, "set_default_str into the I32 material field is rejected");
    check(aver_fw_class_set_default_f32(g_actor, aver_scene_field("CCamera.fovYRad"), 1.0f) == 0,
          "a default for an un-added component is rejected");

    check(aver_fw_class_seal(g_actor) == 1, "the class seals");
}

// Checks that seal flattens a parent chain, and refuses a cycle or a parent that was never declared.
static void testParentChainsAndCycle() {
    AVER_INFO("=== seal flattens a chain and refuses a cycle ===");

    const int32_t base = aver_fw_class_declare("Base", "");
    check(aver_fw_class_add_component(base, static_cast<int32_t>(kComponentLight)) == 1, "Base adds CLight");
    check(aver_fw_class_set_default_f32(base, aver_scene_field("CLight.intensityLux"), 777.0f) == 1,
          "Base sets a CLight default");
    const int32_t derived = aver_fw_class_declare("Derived", "Base");
    check(aver_fw_class_add_component(derived, static_cast<int32_t>(kComponentMeshRenderer)) == 1, "Derived adds CMeshRenderer");
    check(aver_fw_class_seal(derived) == 1, "Derived seals over its parent chain");

    const int32_t d = aver_fw_spawn(derived, "derived-inst", nullptr, nullptr, nullptr);
    check(d != 0, "Derived spawns");
    World& w = World::instance();
    const CLight* dl = static_cast<const CLight*>(w.getComponent(static_cast<Entity>(static_cast<uint32_t>(d)), kComponentLight));
    check(dl != nullptr, "the derived instance carries the parent's CLight component");
    check(dl && dl->intensityLux == 777.0f, "and the parent's CLight default flattened into it");
    check(w.getComponent(static_cast<Entity>(static_cast<uint32_t>(d)), kComponentMeshRenderer) != nullptr,
          "the derived instance also carries its own CMeshRenderer");

    aver_fw_class_declare("CycleA", "CycleB");
    aver_fw_class_declare("CycleB", "CycleA");
    check(aver_fw_class_seal(aver_fw_class_find("CycleA")) == 0, "seal refuses a parent cycle (A)");
    check(aver_fw_class_seal(aver_fw_class_find("CycleB")) == 0, "seal refuses a parent cycle (B)");

    const int32_t orphan = aver_fw_class_declare("Orphan", "NeverDeclared");
    check(aver_fw_class_seal(orphan) == 0, "seal refuses a named parent that was never declared");
}

// Checks that default inheritance is per FIELD: a subclass keeps a parent's defaults it does not
// re-author, and a leaf's own override wins for that field alone.
static void testDefaultInheritance() {
    AVER_INFO("=== a subclass inherits a parent's per-FIELD defaults it does not re-author ===");

    const int32_t fScale = aver_scene_field("CLocal.scale");        // Vec3
    const int32_t fPos   = aver_scene_field("CLocal.position");     // Vec3
    check(fScale && fPos, "CLocal.scale/position resolve to dense ids");

    World& w = World::instance();

    const int32_t base = aver_fw_class_declare("InheritBase", "");
    const float baseScale[3] = {5.0f, 5.0f, 5.0f};
    const float basePos[3]   = {1.0f, 2.0f, 3.0f};
    check(aver_fw_class_set_default_vec(base, fScale, baseScale) == 1, "Base authors CLocal.scale = (5,5,5)");
    check(aver_fw_class_set_default_vec(base, fPos,   basePos)   == 1, "Base authors CLocal.position = (1,2,3)");

    const int32_t derived = aver_fw_class_declare("InheritDerived", "InheritBase");
    check(aver_fw_class_seal(derived) == 1, "Derived seals over Base");
    const int32_t d = aver_fw_spawn(derived, "inherit-derived", nullptr, nullptr, nullptr);
    check(d != 0, "Derived spawns");
    const CLocal* dl = static_cast<const CLocal*>(
        w.getComponent(static_cast<Entity>(static_cast<uint32_t>(d)), kComponentLocal));
    check(dl != nullptr, "the derived instance carries CLocal");
    check(dl && dl->xf.scale.x == 5.0f && dl->xf.scale.y == 5.0f && dl->xf.scale.z == 5.0f,
          "the parent's authored CLocal.scale is INHERITED, not clobbered to (1,1,1)");
    check(dl && dl->xf.position.x == 1.0f && dl->xf.position.y == 2.0f && dl->xf.position.z == 3.0f,
          "the parent's authored CLocal.position is inherited too");

    const int32_t grand = aver_fw_class_declare("InheritGrand", "InheritDerived");
    const float grandScale[3] = {9.0f, 9.0f, 9.0f};
    check(aver_fw_class_set_default_vec(grand, fScale, grandScale) == 1, "Grandchild overrides only CLocal.scale");
    check(aver_fw_class_seal(grand) == 1, "Grandchild seals");
    const int32_t g = aver_fw_spawn(grand, "inherit-grand", nullptr, nullptr, nullptr);
    check(g != 0, "Grandchild spawns");
    const CLocal* gl = static_cast<const CLocal*>(
        w.getComponent(static_cast<Entity>(static_cast<uint32_t>(g)), kComponentLocal));
    check(gl && gl->xf.scale.x == 9.0f && gl->xf.scale.y == 9.0f && gl->xf.scale.z == 9.0f,
          "the grandchild's own CLocal.scale override wins per field");
    check(gl && gl->xf.position.x == 1.0f && gl->xf.position.y == 2.0f && gl->xf.position.z == 3.0f,
          "and the un-overridden CLocal.position is still inherited down the chain");
}

// Checks that class_of reads 0 the instant a handle dies, including a child destroyed with its
// parent's subtree and an actor destroyed through the scene ABI.
static void testSubtreeDestroyClassOf() {
    AVER_INFO("=== class_of reads 0 for an actor destroyed as part of a parent's subtree ===");

    const int32_t p = aver_fw_spawn(g_actor, "subtree-parent", nullptr, nullptr, nullptr);
    const int32_t q = aver_fw_spawn(g_actor, "subtree-child",  nullptr, nullptr, nullptr);
    check(p != 0 && q != 0, "the parent and child actors spawn");
    check(aver_scene_set_parent(q, p) == 1, "the child is parented under the parent via the scene ABI");
    check(aver_fw_class_of(q) == g_actor, "class_of on the live child is its class");

    check(aver_fw_destroy(p) == 1, "destroy accepts the parent");
    World::instance().flush();
    check(aver_fw_class_of(p) == 0, "class_of on the destroyed parent is 0");
    check(aver_fw_class_of(q) == 0, "class_of on the subtree-destroyed child is 0 (not a stale class)");

    const int32_t s = aver_fw_spawn(g_actor, "scene-destroyed", nullptr, nullptr, nullptr);
    check(s != 0, "a fresh actor spawns");
    check(aver_scene_destroy(s) == 1, "the scene ABI accepts the destroy");
    World::instance().flush();
    check(aver_fw_class_of(s) == 0, "class_of on an actor destroyed via the scene ABI is 0");
}

// Checks that spawn lays the archetype defaults down, applies a pos override, and that class_of
// answers for a spawned actor, a plain scene entity and a destroyed one.
static void testSpawnAndClassOf() {
    AVER_INFO("=== spawn lays the archetype defaults down byte-identical ===");

    World& w = World::instance();

    const int32_t e = aver_fw_spawn(g_actor, "hero", nullptr, nullptr, nullptr);
    check(e != 0, "spawn returns a non-zero entity");
    const Entity ent = static_cast<Entity>(static_cast<uint32_t>(e));
    check(w.valid(ent), "the spawned entity is valid in the world");
    check(std::string(w.name(ent)) == "hero", "the optional name was applied");

    check(aver_fw_class_of(e) == g_actor, "class_of on the spawned entity returns its class");
    const int32_t plain = aver_scene_create();
    check(aver_fw_class_of(plain) == 0, "class_of on a plain scene entity returns 0");

    // Field by field, never a whole-struct memcmp: CMeshRenderer carries padding the ABI never defines.
    const CMeshRenderer* mr =
        static_cast<const CMeshRenderer*>(w.getComponent(ent, kComponentMeshRenderer));
    check(mr != nullptr, "the actor carries CMeshRenderer");
    check(mr && mr->mesh == static_cast<u64>(g_meshId), "CMeshRenderer.mesh is the I64 ObjectId set via set_default_i64");
    check(mr && mr->material == g_material, "CMeshRenderer.material is the I32 handle set via set_default_i32");
    check(mr && mr->flags == 1u, "the built-in visible flag survived the memcpy (not zeroed)");
    check(mr && mr->dirty == 1u, "the built-in dirty flag survived the memcpy (not zeroed)");
    check(mr && mr->aabbMin[0] == 0.0f && mr->aabbMin[1] == 0.0f && mr->aabbMin[2] == 0.0f,
          "CMeshRenderer.aabbMin is the built-in default (0,0,0)");
    check(mr && mr->aabbMax[0] == 0.0f && mr->aabbMax[1] == 0.0f && mr->aabbMax[2] == 0.0f,
          "CMeshRenderer.aabbMax is the built-in default (0,0,0)");

    const CLight* li = static_cast<const CLight*>(w.getComponent(ent, kComponentLight));
    check(li != nullptr, "the actor carries CLight");
    check(li && li->intensityLux == g_intensity, "CLight.intensityLux is the F32 default");
    check(li && li->colour[0] == g_colour[0] && li->colour[1] == g_colour[1] && li->colour[2] == g_colour[2],
          "CLight.colour is the Vec3 default");

    const CLocal* lo = static_cast<const CLocal*>(w.getComponent(ent, kComponentLocal));
    check(lo != nullptr, "the actor carries CLocal (a birth component)");
    check(lo && lo->xf.scale.x == g_scale[0] && lo->xf.scale.y == g_scale[1] && lo->xf.scale.z == g_scale[2],
          "CLocal.scale is the Vec3 default laid down by spawn");
    check(lo && lo->xf.position.x == 0.0f && lo->xf.position.y == 0.0f && lo->xf.position.z == 0.0f,
          "a null pos override keeps the class-default position");

    AVER_INFO("--- a pos override lands where it was asked ---");
    const float pos[3] = {10.0f, 20.0f, 30.0f};
    const int32_t e2 = aver_fw_spawn(g_actor, "placed", pos, nullptr, nullptr);
    check(e2 != 0, "the override spawn succeeds");
    const Entity ent2 = static_cast<Entity>(static_cast<uint32_t>(e2));
    const CLocal* lo2 = static_cast<const CLocal*>(w.getComponent(ent2, kComponentLocal));
    check(lo2 && lo2->xf.position.x == 10.0f && lo2->xf.position.y == 20.0f && lo2->xf.position.z == 30.0f,
          "the pos override landed at (10,20,30)");
    check(lo2 && lo2->xf.scale.x == g_scale[0], "and the un-overridden scale is still the class default");

    check(aver_fw_destroy(e2) == 1, "destroy accepts the actor");
    World::instance().flush();
    check(aver_fw_class_of(e2) == 0, "class_of on a destroyed actor is 0");
}

// Checks that possession is gated on the CONTROLLER and PAWN class flags, and that destroying a
// possessed pawn leaves no dangling forward entry.
static void testPossession() {
    AVER_INFO("=== possession is flag-gated ===");

    int32_t ctrlC = aver_fw_class_declare("Ctrl", "");
    aver_fw_class_set_flags(ctrlC, AVER_FW_CLASS_CONTROLLER);
    aver_fw_class_seal(ctrlC);

    int32_t pawnC = aver_fw_class_declare("Pwn", "");
    aver_fw_class_set_flags(pawnC, AVER_FW_CLASS_PAWN);
    aver_fw_class_seal(pawnC);

    const int32_t controller = aver_fw_spawn(ctrlC, "controller", nullptr, nullptr, nullptr);
    const int32_t pawn       = aver_fw_spawn(pawnC, "pawn", nullptr, nullptr, nullptr);
    check(controller != 0 && pawn != 0, "the controller and pawn spawn");

    check(aver_fw_possess(pawn, controller) == 0, "possess is refused when the controller side lacks CONTROLLER");
    check(aver_fw_possess(controller, controller) == 0, "possess is refused when the pawn side lacks PAWN");
    const int32_t plainC = aver_fw_spawn(g_actor, "not-a-controller", nullptr, nullptr, nullptr);
    check(aver_fw_possess(plainC, pawn) == 0, "a flagless actor cannot possess");
    const int32_t plainScene = aver_scene_create();
    check(aver_fw_possess(controller, plainScene) == 0, "a plain scene entity (no class) cannot be possessed");

    check(aver_fw_possess(controller, pawn) == 1, "a CONTROLLER possessing a PAWN is accepted");
    check(aver_fw_controlled_pawn(controller) == pawn, "controlled_pawn reads the possessed pawn");
    check(aver_fw_controller_of(pawn) == controller, "controller_of reads the controller");

    check(aver_fw_unpossess(controller) == 1, "unpossess releases the pawn");
    check(aver_fw_controlled_pawn(controller) == 0, "controlled_pawn is 0 after unpossess");
    check(aver_fw_controller_of(pawn) == 0, "controller_of is 0 after unpossess");
    check(aver_fw_unpossess(controller) == 0, "unpossess with nothing to release returns 0");

    const int32_t ctrl2 = aver_fw_spawn(ctrlC, "controller2", nullptr, nullptr, nullptr);
    const int32_t pawn2 = aver_fw_spawn(pawnC, "pawn2", nullptr, nullptr, nullptr);
    check(ctrl2 != 0 && pawn2 != 0, "a second controller and pawn spawn");
    check(aver_fw_possess(ctrl2, pawn2) == 1, "the second controller possesses the second pawn");
    check(aver_fw_destroy(pawn2) == 1, "the possessed pawn is destroyed");
    World::instance().flush();
    check(aver_fw_controlled_pawn(ctrl2) == 0, "controlled_pawn is 0 once the possessed pawn is dead");
    check(aver_fw_unpossess(ctrl2) == 0,
          "unpossess returns 0 after a possessed-pawn destroy (no dangling forward entry left to release)");
}

// What the stand-in managed dispatch recorded: a count and the arguments for each hook. The *Order
// fields are stamps off g_dispSeq, so the test can pin the order the edges fired in.
struct DispatchLog {
    int     beginCount = 0;
    int32_t beginEntity = 0;
    int32_t beginReason = -1;
    int     tickCount = 0;
    int32_t tickGroup = -1;
    float   tickDt = -1.0f;
    int     endCount = 0;
    int32_t endEntity = 0;
    int32_t endReason = -1;
    bool    endSawLiveEntity = false;
    int32_t endClass = -1;
    int     bindCount = 0;
    int     unbindCount = 0;
    int     buildCount = 0;
    int32_t bindOrder = 0;
    int32_t buildOrder = 0;
    int32_t beginOrder = 0;
    int32_t endOrder = 0;
    int32_t unbindOrder = 0;
    int     possessCount = 0;
    int32_t possessPawn = 0, possessController = 0;
    int     unpossessCount = 0;
    int32_t unpossessPawn = 0;
    int     postLoginCount = 0;
    int32_t postLoginMode = 0, postLoginController = 0;
    void reset() { *this = DispatchLog{}; }
};
static DispatchLog g_disp;
static int32_t g_dispSeq = 0;   // shared monotonic counter the stand-ins stamp their call order from

// Records a begin_play call.
static void AVER_FW_CALL standInBeginPlay(aver_entity e, int32_t reason) {
    ++g_disp.beginCount;
    g_disp.beginEntity = e;
    g_disp.beginReason = reason;
    g_disp.beginOrder = ++g_dispSeq;
}
// Records a build_models call.
static void AVER_FW_CALL standInBuildModels(aver_entity /*e*/) {
    ++g_disp.buildCount;
    g_disp.buildOrder = ++g_dispSeq;
}
// Records a tick_all call and its group and dt.
static void AVER_FW_CALL standInTickAll(int32_t group, float dt) {
    ++g_disp.tickCount;
    g_disp.tickGroup = group;
    g_disp.tickDt = dt;
}
// Records an end_play call, plus whether the entity was still live and what its class read as.
static void AVER_FW_CALL standInEndPlay(aver_entity e, int32_t reason) {
    ++g_disp.endCount;
    g_disp.endEntity = e;
    g_disp.endReason = reason;
    g_disp.endSawLiveEntity = World::instance().valid(static_cast<Entity>(static_cast<uint32_t>(e)));
    g_disp.endClass = aver_fw_class_of(e);
    g_disp.endOrder = ++g_dispSeq;
}
// Records a bind call. Returns 1, which means a managed instance now exists.
static int32_t AVER_FW_CALL standInBind(int64_t /*classNameHash*/, aver_entity /*e*/) {
    ++g_disp.bindCount;
    g_disp.bindOrder = ++g_dispSeq;
    return 1;
}
// Records an unbind call.
static void AVER_FW_CALL standInUnbind(aver_entity /*e*/) {
    ++g_disp.unbindCount;
    g_disp.unbindOrder = ++g_dispSeq;
}
// Records a possessed call and its pair.
static void AVER_FW_CALL standInPossessed(aver_entity pawn, aver_entity controller) {
    ++g_disp.possessCount; g_disp.possessPawn = pawn; g_disp.possessController = controller;
}
// Records an unpossessed call and its pawn.
static void AVER_FW_CALL standInUnpossessed(aver_entity pawn) {
    ++g_disp.unpossessCount; g_disp.unpossessPawn = pawn;
}
// Records a post_login call and its game mode and controller.
static void AVER_FW_CALL standInPostLogin(aver_entity mode, aver_entity controller) {
    ++g_disp.postLoginCount; g_disp.postLoginMode = mode; g_disp.postLoginController = controller;
}

static int32_t g_rpCtrl = 0, g_rpPawn2 = 0;
static int     g_rpDepth = 0;
static int32_t g_rpNestedResult = -1;
// A possessed hook that re-possesses its controller onto another pawn, recording the nested result
// and its own depth.
static void AVER_FW_CALL standInReentrantPossessed(aver_entity /*pawn*/, aver_entity /*ctrl*/) {
    ++g_rpDepth;
    if (g_rpDepth == 1)
        g_rpNestedResult = aver_fw_possess(g_rpCtrl, g_rpPawn2);
}

static int  g_reentrantEndCount = 0;
static bool g_reentrantReentered = false;
// An end_play hook that calls Destroy(Self) once, guarding its own re-entry.
static void AVER_FW_CALL standInReentrantEndPlay(aver_entity e, int32_t /*reason*/) {
    ++g_reentrantEndCount;
    if (!g_reentrantReentered) {
        g_reentrantReentered = true;
        aver_fw_destroy(static_cast<int32_t>(e));
    }
}

// Checks that a managed actor's lifecycle routes through the installed dispatch: the begin and end
// edges and their order, the preview edge, ticks, possession hooks, and install/clear.
static void testManagedDispatch() {
    AVER_INFO("=== managed actors route their lifecycle through the installed dispatch ===");

    const int32_t managedC = aver_fw_class_declare("ManagedActor", "");
    aver_fw_class_set_flags(managedC, AVER_FW_CLASS_MANAGED | AVER_FW_CLASS_TICKS);
    aver_fw_class_set_tick(managedC, AVER_FW_TICK_PHYSICS, 0);
    check(aver_fw_class_seal(managedC) == 1, "the managed class seals");

    AvManagedDispatch table{};
    table.structBytes     = static_cast<int32_t>(sizeof(AvManagedDispatch));
    table.contractVersion = AVER_FW_DISPATCH_VERSION;
    table.bind            = &standInBind;
    table.unbind          = &standInUnbind;
    table.beginPlay       = &standInBeginPlay;
    table.tick_all        = &standInTickAll;
    table.endPlay         = &standInEndPlay;
    table.build_models    = &standInBuildModels;
    table.possessed       = &standInPossessed;
    table.unpossessed     = &standInUnpossessed;
    table.post_login      = &standInPostLogin;

    check(aver_fw_managed_dispatch_installed() == 0, "no dispatch is installed to begin with");
    check(aver_fw_install_managed_dispatch(&table) == 1, "the first install is accepted");
    check(aver_fw_managed_dispatch_installed() == 1, "the dispatch reports installed");
    check(aver_fw_install_managed_dispatch(&table) == 0, "a SECOND install is refused (returns 0)");
    AvManagedDispatch bad{};
    bad.structBytes = 4;   // not sizeof
    bad.contractVersion = AVER_FW_DISPATCH_VERSION;
    check(aver_fw_install_managed_dispatch(&bad) == 0, "a wrong-structBytes table is rejected");

    g_disp.reset();
    g_dispSeq = 0;
    const int32_t m = aver_fw_spawn(managedC, "managed-1", nullptr, nullptr, nullptr);
    check(m != 0, "the managed actor spawns");
    check(g_disp.beginCount == 1, "begin_play fired exactly once on a managed spawn");
    check(g_disp.beginEntity == m, "begin_play got the spawned entity");
    check(g_disp.beginReason == AVER_FW_BEGIN_SPAWN, "begin_play got reason SPAWN");
    check(g_disp.bindCount == 1, "step 11 calls bind() once on a managed spawn");
    check(g_disp.buildCount == 1, "step 11 calls build_models() once on a managed spawn");
    check(g_disp.bindOrder < g_disp.buildOrder && g_disp.buildOrder < g_disp.beginOrder,
          "the begin edge fires in order: bind -> build_models -> begin_play");

    // The preview edge: spawn_preview binds and builds models, but does not begin play.
    g_disp.reset();
    g_dispSeq = 0;
    const int32_t prev = aver_fw_spawn_preview(managedC, "preview-1", nullptr, nullptr, nullptr);
    check(prev != 0, "the managed actor spawns for a preview");
    check(g_disp.bindCount == 1, "spawn_preview still binds the managed instance");
    check(g_disp.buildCount == 1, "spawn_preview still runs build_models — that is what a preview shows");
    check(g_disp.beginCount == 0, "spawn_preview does NOT fire begin_play");
    check(g_disp.bindOrder < g_disp.buildOrder, "bind still precedes build_models on the preview edge");

    check(aver_fw_destroy_preview(prev) == 1, "the preview actor is destroyed");
    check(g_disp.endCount == 0, "destroy_preview does NOT fire end_play");
    check(g_disp.unbindCount == 1, "destroy_preview still unbinds — an instance was bound, so one is dropped");

    g_disp.reset();
    g_dispSeq = 0;
    const int32_t normal = aver_fw_spawn(managedC, "after-preview", nullptr, nullptr, nullptr);
    check(g_disp.beginCount == 1, "an ordinary spawn still fires begin_play after the preview split");
    aver_fw_destroy(normal);
    check(g_disp.endCount == 1, "an ordinary destroy still fires end_play after the preview split");

    g_disp.reset();
    const int32_t plain = aver_fw_spawn(g_actor, "plain-1", nullptr, nullptr, nullptr);
    check(plain != 0, "the non-managed actor spawns");
    check(g_disp.beginCount == 0, "a non-managed spawn fires no begin_play");

    g_disp.reset();
    check(aver_fw_tick(AVER_FW_TICK_PHYSICS, 0.25f) == 1, "aver_fw_tick reports a managed tick fired");
    check(g_disp.tickCount == 1, "tick_all fired exactly once");
    check(g_disp.tickGroup == AVER_FW_TICK_PHYSICS, "tick_all got the group it was called with");
    check(g_disp.tickDt == 0.25f, "tick_all got the dt it was called with");
    aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, 0.5f);
    check(g_disp.tickCount == 2 && g_disp.tickGroup == AVER_FW_TICK_POST_PHYSICS,
          "each aver_fw_tick is exactly one tick_all for its group");

    g_disp.reset();
    check(aver_fw_destroy(m) == 1, "the managed actor is destroyed");
    check(g_disp.endCount == 1, "end_play fired exactly once on a managed destroy");
    check(g_disp.endEntity == m, "end_play got the destroyed entity");
    check(g_disp.endReason == AVER_FW_END_DESTROY, "end_play got reason DESTROY");
    check(g_disp.endSawLiveEntity, "end_play saw the entity still valid at hook time");
    check(g_disp.endClass == managedC, "end_play could still resolve the actor's class (fired before forgetClass)");
    check(g_disp.unbindCount == 1, "step 11 calls unbind() once on a managed destroy");
    check(g_disp.endOrder < g_disp.unbindOrder, "the end edge fires in order: end_play -> unbind");
    World::instance().flush();

    g_disp.reset();
    check(aver_fw_destroy(plain) == 1, "the non-managed actor is destroyed");
    check(g_disp.endCount == 0, "a non-managed destroy fires no end_play");
    World::instance().flush();

    const int32_t hkPawnC = aver_fw_class_declare("HookPawn", "");
    aver_fw_class_set_flags(hkPawnC, AVER_FW_CLASS_MANAGED | AVER_FW_CLASS_PAWN);
    aver_fw_class_seal(hkPawnC);
    const int32_t hkCtrlC = aver_fw_class_declare("HookCtrl", "");
    aver_fw_class_set_flags(hkCtrlC, AVER_FW_CLASS_MANAGED | AVER_FW_CLASS_CONTROLLER);
    aver_fw_class_seal(hkCtrlC);
    const int32_t hkPawn = aver_fw_spawn(hkPawnC, "hook-pawn", nullptr, nullptr, nullptr);
    const int32_t hkCtrl = aver_fw_spawn(hkCtrlC, "hook-ctrl", nullptr, nullptr, nullptr);

    g_disp.reset();
    check(aver_fw_possess(hkCtrl, hkPawn) == 1, "possess accepts the managed pair");
    check(g_disp.possessCount == 1, "possess fired the possessed hook once");
    check(g_disp.possessPawn == hkPawn && g_disp.possessController == hkCtrl,
          "the possessed hook received (pawn, controller)");

    g_disp.reset();
    check(aver_fw_unpossess(hkCtrl) == 1, "unpossess releases the pair");
    check(g_disp.unpossessCount == 1 && g_disp.unpossessPawn == hkPawn,
          "unpossess fired the unpossessed hook for the released pawn");

    const int32_t hkPawn2 = aver_fw_spawn(hkPawnC, "hook-pawn-2", nullptr, nullptr, nullptr);
    aver_fw_possess(hkCtrl, hkPawn);
    g_disp.reset();
    check(aver_fw_possess(hkCtrl, hkPawn2) == 1, "the controller moves to a second pawn");
    check(g_disp.unpossessCount == 1 && g_disp.unpossessPawn == hkPawn, "the displaced first pawn was unpossessed");
    check(g_disp.possessCount == 1 && g_disp.possessPawn == hkPawn2, "the second pawn was possessed");

    const int32_t hkCtrl2 = aver_fw_spawn(hkCtrlC, "hook-ctrl-2", nullptr, nullptr, nullptr);
    aver_fw_possess(hkCtrl, hkPawn);   // hkCtrl drives hkPawn again
    g_disp.reset();
    check(aver_fw_possess(hkCtrl2, hkPawn) == 1, "a second controller steals the pawn");
    check(g_disp.unpossessCount == 1 && g_disp.unpossessPawn == hkPawn, "the stolen pawn is unpossessed from its old controller");
    check(g_disp.possessCount == 1 && g_disp.possessController == hkCtrl2, "then possessed by the new controller");
    check(aver_fw_controller_of(hkPawn) == hkCtrl2, "the pawn now reports the new controller");
    check(aver_fw_controlled_pawn(hkCtrl) == 0, "and the old controller no longer drives it");

    g_disp.reset();
    check(aver_fw_possess(hkCtrl2, hkPawn) == 1, "re-possessing the same pair returns 1");
    check(g_disp.possessCount == 0 && g_disp.unpossessCount == 0, "and fires no hooks (idempotent)");

    aver_fw_destroy(hkPawn); aver_fw_destroy(hkPawn2); aver_fw_destroy(hkCtrl); aver_fw_destroy(hkCtrl2);
    World::instance().flush();

    check(aver_fw_clear_managed_dispatch() == 1, "clear returns 1");
    check(aver_fw_managed_dispatch_installed() == 0, "the dispatch reports not-installed after clear");
    g_disp.reset();
    const int32_t m2 = aver_fw_spawn(managedC, "managed-after-clear", nullptr, nullptr, nullptr);
    check(m2 != 0, "a managed spawn still succeeds after clear (no CLR required)");
    check(g_disp.beginCount == 0, "no begin_play fires after clear");
    check(aver_fw_tick(AVER_FW_TICK_PHYSICS, 0.1f) == 0, "aver_fw_tick fires nothing after clear (ticks nothing, no fault)");
    check(g_disp.tickCount == 0, "no tick_all fires after clear");
    check(aver_fw_destroy(m2) == 1, "a managed destroy still succeeds after clear");
    check(g_disp.endCount == 0, "no end_play fires after clear");
    World::instance().flush();

    check(aver_fw_install_managed_dispatch(&table) == 1, "install is accepted again after a clear");
    check(aver_fw_clear_managed_dispatch() == 1, "and clears again");
}

// Checks that a managed OnEndPlay calling Destroy(Self) does not double-fire endPlay.
static void testDestroyReentrancy() {
    AVER_INFO("=== a managed OnEndPlay calling Destroy(Self) does not double-fire endPlay ===");

    const int32_t managedC = aver_fw_class_declare("ReentrantActor", "");
    aver_fw_class_set_flags(managedC, AVER_FW_CLASS_MANAGED);
    check(aver_fw_class_seal(managedC) == 1, "the re-entrant managed class seals");

    AvManagedDispatch table{};
    table.structBytes     = static_cast<int32_t>(sizeof(AvManagedDispatch));
    table.contractVersion = AVER_FW_DISPATCH_VERSION;
    table.endPlay         = &standInReentrantEndPlay;
    check(aver_fw_install_managed_dispatch(&table) == 1, "the re-entrant stand-in installs");

    g_reentrantEndCount  = 0;
    g_reentrantReentered = false;
    const int32_t r = aver_fw_spawn(managedC, "reentrant-1", nullptr, nullptr, nullptr);
    check(r != 0, "the re-entrant managed actor spawns");

    check(aver_fw_destroy(r) == 1, "the outer destroy is accepted");
    check(g_reentrantEndCount == 1, "endPlay fired exactly once despite Destroy(Self) inside OnEndPlay");
    World::instance().flush();

    check(aver_fw_clear_managed_dispatch() == 1, "the re-entrant stand-in clears");
}

// Checks that a possession hook re-entering possess is refused rather than recursed to a crash.
static void testPossessReentrancy() {
    AVER_INFO("=== a possession hook re-entering possession is refused, not recursed to a crash ===");

    AvManagedDispatch table{};
    table.structBytes     = static_cast<int32_t>(sizeof(AvManagedDispatch));
    table.contractVersion = AVER_FW_DISPATCH_VERSION;
    table.possessed       = &standInReentrantPossessed;
    check(aver_fw_install_managed_dispatch(&table) == 1, "install the re-entrant possessed stand-in");

    const int32_t ctrlC = aver_fw_class_declare("RpCtrl", "");
    aver_fw_class_set_flags(ctrlC, AVER_FW_CLASS_MANAGED | AVER_FW_CLASS_CONTROLLER);
    aver_fw_class_seal(ctrlC);
    const int32_t pawnC = aver_fw_class_declare("RpPawn", "");
    aver_fw_class_set_flags(pawnC, AVER_FW_CLASS_MANAGED | AVER_FW_CLASS_PAWN);
    aver_fw_class_seal(pawnC);
    const int32_t ctrl  = aver_fw_spawn(ctrlC, "rp-ctrl", nullptr, nullptr, nullptr);
    const int32_t pawn1 = aver_fw_spawn(pawnC, "rp-pawn-1", nullptr, nullptr, nullptr);
    const int32_t pawn2 = aver_fw_spawn(pawnC, "rp-pawn-2", nullptr, nullptr, nullptr);
    g_rpCtrl = ctrl; g_rpPawn2 = pawn2; g_rpDepth = 0; g_rpNestedResult = -1;

    // The outer possess fires OnPossessed, which re-possesses the same controller onto pawn2.
    check(aver_fw_possess(ctrl, pawn1) == 1, "the outer possess succeeds");
    check(g_rpDepth == 1, "the possessed hook fired exactly once — no recursion");
    check(g_rpNestedResult == 0, "the re-entrant possess was refused (returned 0)");
    check(aver_fw_controlled_pawn(ctrl) == pawn1, "the controller still drives the outer pawn (nested was rejected)");

    aver_fw_destroy(ctrl); aver_fw_destroy(pawn1); aver_fw_destroy(pawn2);
    World::instance().flush();
    check(aver_fw_clear_managed_dispatch() == 1, "clear the re-entrant possess table");
}

// Checks the play lifecycle: begin_play spawns the session singletons, pause flips state, end_play
// tears the whole session down.
static void testPlayLifecycle() {
    AVER_INFO("=== play lifecycle: begin_play spawns the session, end_play tears it down ===");

    check(aver_fw_play_state() == AVER_FW_PLAY_EDITOR, "play state starts in EDITOR");
    check(aver_fw_game_instance() == 0, "no GameInstance before begin_play");
    check(aver_fw_game_mode() == 0, "no GameMode before begin_play");
    check(aver_fw_player_controller(0) == 0, "no player controller before begin_play");

    // Pawn, controller and GameInstance, an ABSTRACT GameMode base declared first, then the GameMode.
    const int32_t pawnC = aver_fw_class_declare("PlayPawn", "");
    aver_fw_class_set_flags(pawnC, AVER_FW_CLASS_PAWN);
    aver_fw_class_seal(pawnC);
    const int32_t ctrlC = aver_fw_class_declare("PlayCtrl", "");
    aver_fw_class_set_flags(ctrlC, AVER_FW_CLASS_CONTROLLER);
    aver_fw_class_seal(ctrlC);
    const int32_t giC = aver_fw_class_declare("PlayGI", "");
    aver_fw_class_set_flags(giC, AVER_FW_CLASS_GAME_INSTANCE);
    aver_fw_class_seal(giC);

    const int32_t absGM = aver_fw_class_declare("PlayGMBase", "");
    aver_fw_class_set_flags(absGM, AVER_FW_CLASS_GAME_MODE | AVER_FW_CLASS_ABSTRACT);
    aver_fw_class_seal(absGM);

    const int32_t gmC = aver_fw_class_declare("PlayGM", "");
    aver_fw_class_set_flags(gmC, AVER_FW_CLASS_GAME_MODE);
    aver_fw_class_set_default_pawn(gmC, "PlayPawn");
    aver_fw_class_set_player_controller(gmC, "PlayCtrl");
    aver_fw_class_seal(gmC);

    // find_class_with_flags skips the ABSTRACT base (declared first) and lands on the concrete class.
    check(aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) == gmC,
          "find_class_with_flags(GAME_MODE) skips the ABSTRACT base and returns the user class");
    check(aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE) == giC,
          "find_class_with_flags(GAME_INSTANCE) returns the GameInstance class");
    check(aver_fw_find_class_with_flags(0) == 0, "find_class_with_flags(0) matches nothing");

    // An abstract class is never spawnable — the spawn choke point rejects it.
    check(aver_fw_spawn(absGM, "shouldNotSpawn", nullptr, nullptr, nullptr) == 0,
          "aver_fw_spawn refuses an ABSTRACT class");

    // begin_play spawns GameInstance -> GameMode -> Controller -> Pawn and possesses the pawn.
    check(aver_fw_begin_play(giC, gmC) == 1, "begin_play starts a session");
    check(aver_fw_play_state() == AVER_FW_PLAY_PLAYING, "play state is PLAYING after begin_play");
    const int32_t gm = aver_fw_game_mode();
    const int32_t gi = aver_fw_game_instance();
    const int32_t ctrl = aver_fw_player_controller(0);
    check(gm != 0 && aver_fw_class_of(gm) == gmC, "the GameMode singleton is a live instance of the GameMode class");
    check(gi != 0 && aver_fw_class_of(gi) == giC, "the GameInstance singleton is a live instance of the GameInstance class");
    check(ctrl != 0 && aver_fw_class_of(ctrl) == ctrlC, "the controller singleton is a live instance of the controller class");
    const int32_t pawn = aver_fw_controlled_pawn(ctrl);
    check(pawn != 0 && aver_fw_class_of(pawn) == pawnC, "the controller possesses a pawn of the pawn class");

    // A second begin_play while a session runs is refused, leaving the running session intact.
    check(aver_fw_begin_play(giC, gmC) == 0, "begin_play is refused while a session is already running");
    check(aver_fw_game_mode() == gm, "the refused begin_play did not disturb the running session");

    // Pause and resume flip the state without tearing anything down.
    check(aver_fw_set_paused(1) == 1, "set_paused(1) is accepted while playing");
    check(aver_fw_play_state() == AVER_FW_PLAY_PAUSED, "play state is PAUSED");
    check(aver_fw_game_mode() == gm, "the GameMode singleton survives a pause");
    check(aver_fw_set_paused(0) == 1, "set_paused(0) resumes");
    check(aver_fw_play_state() == AVER_FW_PLAY_PLAYING, "play state is PLAYING again after resume");

    // end_play tears the whole session down, back to EDITOR with every singleton cleared.
    check(aver_fw_end_play() == 1, "end_play ends the session");
    check(aver_fw_play_state() == AVER_FW_PLAY_EDITOR, "play state is EDITOR after end_play");
    check(aver_fw_game_instance() == 0, "the GameInstance singleton is cleared");
    check(aver_fw_game_mode() == 0, "the GameMode singleton is cleared");
    check(aver_fw_player_controller(0) == 0, "the controller singleton is cleared");
    check(aver_fw_end_play() == 0, "a second end_play with nothing running is refused");
    check(aver_fw_set_paused(1) == 0, "set_paused is refused outside a session");
    World::instance().flush();   // retire the deferred destroys the ended session left

    // end_play must clear actors spawned mid-session too, not just the four session roots.
    check(aver_fw_begin_play(giC, gmC) == 1, "begin_play starts a session (extra-actor case)");
    const int32_t extra = aver_fw_spawn(pawnC, "extra-play-actor", nullptr, nullptr, nullptr);
    check(extra != 0 && aver_fw_class_of(extra) == pawnC, "an extra actor spawns mid-session");
    check(aver_fw_end_play() == 1, "end_play ends the session with an extra actor still live");
    check(aver_fw_class_of(extra) == 0, "end_play tore down the extra play-spawned actor, not just the roots");
    check(aver_fw_game_mode() == 0, "and the session roots are cleared too");
    World::instance().flush();

    // A session cannot start without a valid GameMode.
    check(aver_fw_begin_play(giC, 0) == 0, "begin_play with an invalid GameMode is refused");
    check(aver_fw_play_state() == AVER_FW_PLAY_EDITOR, "a refused begin_play leaves the state in EDITOR");
    check(aver_fw_game_instance() == 0, "a refused begin_play leaves no GameInstance behind");
    World::instance().flush();
}

// Checks input: key edges, out-of-range keys, mouse delta and its clearing, and the play-view request.
static void testInput() {
    AVER_INFO("=== input: keys with edge detection, mouse delta, and the play-view request ===");

    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_W, 1);
    check(aver_fw_input_key(AVER_FW_KEY_W) == 1, "a set key reads held");
    check(aver_fw_input_key_pressed(AVER_FW_KEY_W) == 1, "and pressed this frame (was up last frame)");
    check(aver_fw_input_key_released(AVER_FW_KEY_W) == 0, "not released");
    check(aver_fw_input_key(AVER_FW_KEY_A) == 0, "an unset key reads up");

    aver_fw_input_new_frame();                 // W held across the frame boundary -> no longer a press edge
    aver_fw_input_set_key(AVER_FW_KEY_W, 1);
    check(aver_fw_input_key(AVER_FW_KEY_W) == 1, "still held next frame");
    check(aver_fw_input_key_pressed(AVER_FW_KEY_W) == 0, "no longer a pressed edge");

    aver_fw_input_new_frame();                 // release
    aver_fw_input_set_key(AVER_FW_KEY_W, 0);
    check(aver_fw_input_key(AVER_FW_KEY_W) == 0, "released reads up");
    check(aver_fw_input_key_released(AVER_FW_KEY_W) == 1, "released this frame");

    aver_fw_input_set_key(-1, 1);              // out of range: ignored, and reads 0
    aver_fw_input_set_key(AVER_FW_KEY_COUNT, 1);
    check(aver_fw_input_key(AVER_FW_KEY_COUNT) == 0, "an out-of-range key reads 0");

    aver_fw_input_set_mouse(3.5f, -2.0f, 1.0f);
    float m[3] = {0, 0, 0};
    aver_fw_input_mouse(m);
    check(m[0] == 3.5f && m[1] == -2.0f && m[2] == 1.0f, "mouse delta round-trips");
    aver_fw_input_new_frame();
    aver_fw_input_mouse(m);
    check(m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f, "new_frame clears the mouse delta");

    aver_fw_set_view(AVER_FW_VIEW_FIRST_PERSON, 170.0f, 500.0f);
    int32_t mode = -1; float eye = 0.0f, boom = 0.0f;
    aver_fw_view(&mode, &eye, &boom);
    check(mode == AVER_FW_VIEW_FIRST_PERSON && eye == 170.0f && boom == 500.0f, "the play-view request round-trips");
}

// Checks the raw Win32 VK twin: the same edge-detection shape as testInput's key checks above, plus
// independence from the named AVER_FW_KEY_* array and out-of-range handling. See framework_abi.h's
// own RAW WIN32 VK section for why this parallel array exists at all.
static void testRawVk() {
    AVER_INFO("=== raw Win32 VK twin: edge detection, independence, and out-of-range ===");

    const int32_t kF1 = 0x70;   // VK_F1 -- exactly the kind of key AVER_FW_KEY_* cannot reach
                                // (frameworkKeyFromVk falls through to -1 for it, InputKeys.hpp:48)

    aver_fw_input_new_frame();
    aver_fw_input_set_vk(kF1, 1);
    check(aver_fw_input_vk(kF1) == 1, "a set raw VK reads held");
    check(aver_fw_input_vk_pressed(kF1) == 1, "and pressed this frame (was up last frame)");
    check(aver_fw_input_vk_released(kF1) == 0, "not released");

    aver_fw_input_new_frame();                 // held across the frame boundary -> no longer a press edge
    aver_fw_input_set_vk(kF1, 1);
    check(aver_fw_input_vk(kF1) == 1, "still held next frame");
    check(aver_fw_input_vk_pressed(kF1) == 0, "no longer a pressed edge");

    aver_fw_input_new_frame();                 // release
    aver_fw_input_set_vk(kF1, 0);
    check(aver_fw_input_vk(kF1) == 0, "released reads up");
    check(aver_fw_input_vk_released(kF1) == 1, "released this frame");

    aver_fw_input_set_vk(-1, 1);                // out of range: ignored, and reads 0
    aver_fw_input_set_vk(AVER_FW_VK_COUNT, 1);
    check(aver_fw_input_vk(-1) == 0, "a negative VK reads 0, not a crash");
    check(aver_fw_input_vk(AVER_FW_VK_COUNT) == 0, "a VK at/past AVER_FW_VK_COUNT reads 0");

    // The named AVER_FW_KEY_* slots and the raw VK twin are two SEPARATE arrays, not aliases of one
    // another -- setting one must never leak into the other. 'W' the char literal IS VK_W (Win32's
    // letter VKs are plain ASCII uppercase, the same fact frameworkKeyFromVk relies on).
    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_W, 1);
    check(aver_fw_input_vk('W') == 0, "setting the named W slot does not also set raw vk 'W'");
    aver_fw_input_set_vk('W', 1);
    check(aver_fw_input_key(AVER_FW_KEY_W) == 1, "and the named slot keeps reading what set_key gave it");
}

// Checks the gamepad ABI's round trip and out-of-range handling. No polling exists behind it (see
// framework_abi.h's own GAMEPAD section), so this is pure state in / state out.
static void testGamepad() {
    AVER_INFO("=== gamepad: button/axis round trip, and out-of-range pad/button/axis ===");

    check(aver_fw_input_gamepad_button(0, AVER_FW_GAMEPAD_A) == 0, "an unset button reads up");
    aver_fw_input_set_gamepad_button(0, AVER_FW_GAMEPAD_A, 1);
    check(aver_fw_input_gamepad_button(0, AVER_FW_GAMEPAD_A) == 1, "a set button reads held");
    aver_fw_input_set_gamepad_button(0, AVER_FW_GAMEPAD_A, 0);
    check(aver_fw_input_gamepad_button(0, AVER_FW_GAMEPAD_A) == 0, "and clears back to up");

    aver_fw_input_set_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_LEFT_X, 0.75f);
    check(aver_fw_input_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_LEFT_X) == 0.75f, "an axis round-trips exactly");
    aver_fw_input_set_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_LEFT_X, -3.0f);
    check(aver_fw_input_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_LEFT_X) == -3.0f, "unclamped -- no dead zone of its own");

    // Only pad 0 exists -- aver_fw_player_controller's own "player 0 only" precedent, applied here.
    aver_fw_input_set_gamepad_button(1, AVER_FW_GAMEPAD_A, 1);
    check(aver_fw_input_gamepad_button(1, AVER_FW_GAMEPAD_A) == 0, "pad != 0 is refused on both setter and getter");
    aver_fw_input_set_gamepad_axis(1, AVER_FW_GAMEPAD_AXIS_LEFT_X, 1.0f);
    check(aver_fw_input_gamepad_axis(1, AVER_FW_GAMEPAD_AXIS_LEFT_X) == 0.0f, "same for an axis on pad != 0");

    aver_fw_input_set_gamepad_button(0, -1, 1);
    aver_fw_input_set_gamepad_button(0, AVER_FW_GAMEPAD_BUTTON_COUNT, 1);
    check(aver_fw_input_gamepad_button(0, -1) == 0, "a negative button index reads 0, not a crash");
    check(aver_fw_input_gamepad_button(0, AVER_FW_GAMEPAD_BUTTON_COUNT) == 0, "a button index at/past COUNT reads 0");

    aver_fw_input_set_gamepad_axis(0, -1, 1.0f);
    aver_fw_input_set_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_COUNT, 1.0f);
    check(aver_fw_input_gamepad_axis(0, -1) == 0.0f, "a negative axis index reads 0, not a crash");
    check(aver_fw_input_gamepad_axis(0, AVER_FW_GAMEPAD_AXIS_COUNT) == 0.0f, "an axis index at/past COUNT reads 0");
}

// Checks the named-action layer ported from Aver.Framework's EnhancedInput.cs: registration
// identity, held/pressed/released edge detection, the 0.15 dead zone, and the layer's central
// behaviour -- two different-priority "contexts" (here: two contextPriority values) binding the SAME
// key, where the higher one consumes it and the lower one must never see it. The assertions below
// are EnhancedInput.cs's own documented behaviour (its Update() bindings loop and the "Consumption
// is per layer" comment on its consumed-set pass), ported into checks rather than re-derived.
static void testActions() {
    AVER_INFO("=== named actions: registration, edges, dead zone, and priority key consumption ===");

    aver_fw_action_clear_bindings();

    // ---- registration is idempotent by name, like aver_fw_class_declare ----
    const int32_t jump1 = aver_fw_action_register("Jump", AVER_FW_ACTION_DIGITAL);
    const int32_t jump2 = aver_fw_action_register("Jump", AVER_FW_ACTION_DIGITAL);
    check(jump1 != 0, "register returns a non-zero handle");
    check(jump1 == jump2, "registering the same name twice returns the SAME handle");
    check(aver_fw_action_find("Jump") == jump1, "find resolves the name to that handle");
    check(aver_fw_action_find("NoSuchAction") == 0, "an unknown name resolves to 0");
    check(aver_fw_action_register("", AVER_FW_ACTION_DIGITAL) == 0, "an empty name is refused");
    check(aver_fw_action_register("BadType", 99) == 0, "an out-of-range valueType is refused");

    // ---- held state and edge detection: one key, one binding, scale 1.0 (well clear of the dead zone) ----
    aver_fw_action_bind(jump1, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_SPACE, 1.0f, 0, /*contextPriority=*/0);

    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_SPACE, 1);
    check(aver_fw_action_held(jump1) == 1, "the action is held while its bound key is down");
    check(aver_fw_action_pressed(jump1) == 1, "and pressed this frame (was up last frame)");
    check(aver_fw_action_released(jump1) == 0, "not released");

    aver_fw_input_new_frame();                 // held across the frame boundary
    aver_fw_input_set_key(AVER_FW_KEY_SPACE, 1);
    check(aver_fw_action_held(jump1) == 1, "still held next frame");
    check(aver_fw_action_pressed(jump1) == 0, "no longer a pressed edge");

    aver_fw_input_new_frame();                 // release
    aver_fw_input_set_key(AVER_FW_KEY_SPACE, 0);
    check(aver_fw_action_held(jump1) == 0, "released reads not-held");
    check(aver_fw_action_released(jump1) == 1, "released this frame");

    // ---- the dead zone, ported verbatim from EnhancedInput.cs's Active() (0.15) ----
    const int32_t move = aver_fw_action_register("Move", AVER_FW_ACTION_AXIS1D);
    aver_fw_action_bind(move, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_D, 0.10f, 0, 0);   // under 0.15
    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_D, 1);
    float v2[2] = {-1.0f, -1.0f};
    aver_fw_action_value2(move, v2);
    check(v2[0] == 0.10f, "value2 reports the raw accumulated value regardless of the dead zone");
    check(aver_fw_action_held(move) == 0, "but held is 0 -- 0.10 does not clear the 0.15 dead zone");

    aver_fw_action_clear_bindings();
    aver_fw_action_bind(move, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_D, 0.20f, 0, 0);   // over 0.15
    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_D, 1);
    check(aver_fw_action_held(move) == 1, "0.20 clears the dead zone");

    // ---- the central behaviour: two priorities on the SAME key, higher consumes it ----
    aver_fw_action_clear_bindings();
    const int32_t fireHigh = aver_fw_action_register("FireHighPriority", AVER_FW_ACTION_DIGITAL);
    const int32_t fireLow  = aver_fw_action_register("FireLowPriority",  AVER_FW_ACTION_DIGITAL);
    // EnhancedInput.cs sorts layers so the higher priority runs FIRST, and its own pass over the
    // consumed set blocks every key IT bound before the lower layer is ever evaluated. Ported here
    // as "any STRICTLY higher priority binding on the same key blocks this one" -- the same rule
    // with no layer object needed to express it (see aver_fw_action_bind's own header comment).
    aver_fw_action_bind(fireHigh, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_MOUSE_LEFT, 1.0f, 0, 10);
    aver_fw_action_bind(fireLow,  AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_MOUSE_LEFT, 1.0f, 0, 0);

    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT, 1);
    check(aver_fw_action_held(fireHigh) == 1, "the higher-priority action sees the key");
    check(aver_fw_action_held(fireLow) == 0, "the lower-priority action does NOT -- its key was consumed");

    // Remove the higher-priority binding (clear + re-bind without it): consumption is a property of
    // what is CURRENTLY bound, not a permanent lockout, so the lower-priority action sees the key again.
    aver_fw_action_clear_bindings();
    aver_fw_action_bind(fireLow, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_MOUSE_LEFT, 1.0f, 0, 0);
    check(aver_fw_action_held(fireLow) == 1, "with the higher-priority binding gone, the lower one sees the key");

    // Two bindings at the SAME priority never consume each other -- EnhancedInput.cs's own comment:
    // "Consumption is per layer, so two bindings in one context can share a key."
    aver_fw_action_clear_bindings();
    const int32_t both = aver_fw_action_register("BothMouseButtons", AVER_FW_ACTION_DIGITAL);
    aver_fw_action_bind(both, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_MOUSE_LEFT,  1.0f, 0, 0);
    aver_fw_action_bind(both, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_MOUSE_RIGHT, 1.0f, 0, 0);
    aver_fw_input_new_frame();
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT, 1);
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_RIGHT, 1);
    check(aver_fw_action_held(both) == 1, "two same-priority bindings on one action both contribute");

    // ---- invalid handles are quiet, not crashes ----
    check(aver_fw_action_held(0) == 0, "action 0 (invalid) reads not-held");
    check(aver_fw_action_pressed(-1) == 0, "a negative handle reads not-pressed");
    check(aver_fw_action_released(999999) == 0, "a handle past every registration reads not-released");
    float bad[2] = {7.0f, 7.0f};
    aver_fw_action_value2(0, bad);
    check(bad[0] == 0.0f && bad[1] == 0.0f, "value2 zeroes the output for an invalid handle");

    aver_fw_action_clear_bindings();   // leave global state clean for anything that runs after this
}

// Runs every framework test. Returns the failure count as the exit code.
int main() {
    AVER_INFO("Aver.Framework test");
    check(aver_fw_scene_abi_matches() == 1, "the framework's scene ABI major matches the loaded scene DLL");

    testDeclareIdentity();
    testDefaults();
    testParentChainsAndCycle();
    testDefaultInheritance();
    testSpawnAndClassOf();
    testSubtreeDestroyClassOf();
    testPossession();
    testManagedDispatch();
    testDestroyReentrancy();
    testPossessReentrancy();
    testPlayLifecycle();
    testInput();
    testRawVk();
    testGamepad();
    testActions();

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
