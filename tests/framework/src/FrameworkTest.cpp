// Hand-run test for Aver.Framework STEP 8: the class registry, class defaults, spawn, class identity
// and flag-gated possession. Exit code = failure count, matching tests/scene.
//
// It drives the framework's C ABI (framework_abi.h) and reads the results back through the ONE
// process-global World the framework spawned into — the same one-world design tests/scene relies on:
// the framework DLL, the scene DLL and this exe all resolve World::instance() to the single instance
// exported from Aver.Scene.dll, so what spawn wrote is exactly what getComponent here reads.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"

#include <cstring>
#include <string>

using namespace aver;
using namespace aver::scene;

static int g_checks   = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// ---------------------------------------------------------------------------- registry + identity

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

// --------------------------------------------------------------------- components + typed defaults

// Filled in by testDefaults, read back after spawn.
static int32_t g_actor    = 0;
static int64_t g_meshId   = 0x00000000DEADBEEFLL;
static int32_t g_material = 0;
static float   g_colour[3] = {0.10f, 0.20f, 0.30f};
static float   g_intensity = 2500.0f;
static float   g_scale[3]  = {2.0f, 2.0f, 2.0f};

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

    // One default of each storable kind. mesh is I64 written with set_default_i64 — NEVER set_default_str.
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
    // The resolved contradiction: a String default into mesh/material is rejected by the kind check.
    check(aver_fw_class_set_default_str(g_actor, fMesh, "meshes/box") == 0, "set_default_str into the I64 mesh field is rejected");
    check(aver_fw_class_set_default_str(g_actor, fMat, "steel") == 0, "set_default_str into the I32 material field is rejected");
    // A default for a component the class never added is rejected (CCamera was not added).
    check(aver_fw_class_set_default_f32(g_actor, aver_scene_field("CCamera.fovYRad"), 1.0f) == 0,
          "a default for an un-added component is rejected");

    check(aver_fw_class_seal(g_actor) == 1, "the class seals");
}

// -------------------------------------------------------------------------------- parent chains

static void testParentChainsAndCycle() {
    AVER_INFO("=== seal flattens a chain and refuses a cycle ===");

    // A legal two-level chain: Base owns a CLight default, Derived adds CMeshRenderer.
    const int32_t base = aver_fw_class_declare("Base", "");
    check(aver_fw_class_add_component(base, static_cast<int32_t>(kComponentLight)) == 1, "Base adds CLight");
    check(aver_fw_class_set_default_f32(base, aver_scene_field("CLight.intensityLux"), 777.0f) == 1,
          "Base sets a CLight default");
    const int32_t derived = aver_fw_class_declare("Derived", "Base");
    check(aver_fw_class_add_component(derived, static_cast<int32_t>(kComponentMeshRenderer)) == 1, "Derived adds CMeshRenderer");
    check(aver_fw_class_seal(derived) == 1, "Derived seals over its parent chain");

    // Spawn Derived and confirm it carries BOTH the inherited CLight and its own CMeshRenderer.
    const int32_t d = aver_fw_spawn(derived, "derived-inst", nullptr, nullptr, nullptr);
    check(d != 0, "Derived spawns");
    World& w = World::instance();
    const CLight* dl = static_cast<const CLight*>(w.getComponent(static_cast<Entity>(static_cast<uint32_t>(d)), kComponentLight));
    check(dl != nullptr, "the derived instance carries the parent's CLight component");
    check(dl && dl->intensityLux == 777.0f, "and the parent's CLight default flattened into it");
    check(w.getComponent(static_cast<Entity>(static_cast<uint32_t>(d)), kComponentMeshRenderer) != nullptr,
          "the derived instance also carries its own CMeshRenderer");

    // A cycle: A's parent is B, B's parent is A. Sealing either must refuse rather than loop.
    aver_fw_class_declare("CycleA", "CycleB");
    aver_fw_class_declare("CycleB", "CycleA");
    check(aver_fw_class_seal(aver_fw_class_find("CycleA")) == 0, "seal refuses a parent cycle (A)");
    check(aver_fw_class_seal(aver_fw_class_find("CycleB")) == 0, "seal refuses a parent cycle (B)");

    // A named-but-undeclared parent is a missing parent: seal refuses it too.
    const int32_t orphan = aver_fw_class_declare("Orphan", "NeverDeclared");
    check(aver_fw_class_seal(orphan) == 0, "seal refuses a named parent that was never declared");
}

// ------------------------------------------------------------- field-level default inheritance

// Regression for the whole-component clobber: seal used to memcpy each class's own component blob over
// the resolved one, so a subclass's un-authored built-in CLocal (scale 1,1,1 / pos 0) silently
// overwrote a parent's authored transform defaults. CLocal is auto-added to every class, so this fired
// on 100% of subclasses. The fix makes default inheritance field-level.
static void testDefaultInheritance() {
    AVER_INFO("=== a subclass inherits a parent's per-FIELD defaults it does not re-author ===");

    const int32_t fScale = aver_scene_field("CLocal.scale");        // Vec3
    const int32_t fPos   = aver_scene_field("CLocal.position");     // Vec3
    check(fScale && fPos, "CLocal.scale/position resolve to dense ids");

    World& w = World::instance();

    // Base authors two CLocal transform defaults on the auto-added CLocal channel.
    const int32_t base = aver_fw_class_declare("InheritBase", "");
    const float baseScale[3] = {5.0f, 5.0f, 5.0f};
    const float basePos[3]   = {1.0f, 2.0f, 3.0f};
    check(aver_fw_class_set_default_vec(base, fScale, baseScale) == 1, "Base authors CLocal.scale = (5,5,5)");
    check(aver_fw_class_set_default_vec(base, fPos,   basePos)   == 1, "Base authors CLocal.position = (1,2,3)");

    // Derived declares Base as parent and authors NOTHING on CLocal. It must inherit both defaults.
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

    // A grandchild that overrides only ONE field keeps the other inherited (leaf wins per field).
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

// ---------------------------------------------------------------- class_of after a subtree destroy

// Regression for the stale class-identity read: aver_fw_destroy forgets only the single handle it is
// given, but world().destroy takes the whole subtree, so a child destroyed via its parent kept a
// non-zero class_of until its index was reused. class_of must read 0 the instant the handle dies.
static void testSubtreeDestroyClassOf() {
    AVER_INFO("=== class_of reads 0 for an actor destroyed as part of a parent's subtree ===");

    const int32_t p = aver_fw_spawn(g_actor, "subtree-parent", nullptr, nullptr, nullptr);
    const int32_t q = aver_fw_spawn(g_actor, "subtree-child",  nullptr, nullptr, nullptr);
    check(p != 0 && q != 0, "the parent and child actors spawn");
    check(aver_scene_set_parent(q, p) == 1, "the child is parented under the parent via the scene ABI");
    check(aver_fw_class_of(q) == g_actor, "class_of on the live child is its class");

    // Destroying only the parent takes the child with it on flush. The framework forgot only P's row.
    check(aver_fw_destroy(p) == 1, "destroy accepts the parent");
    World::instance().flush();
    check(aver_fw_class_of(p) == 0, "class_of on the destroyed parent is 0");
    check(aver_fw_class_of(q) == 0, "class_of on the subtree-destroyed child is 0 (not a stale class)");

    // Same gap via the scene C ABI destroy, which never runs the framework's forgetClass at all.
    const int32_t s = aver_fw_spawn(g_actor, "scene-destroyed", nullptr, nullptr, nullptr);
    check(s != 0, "a fresh actor spawns");
    check(aver_scene_destroy(s) == 1, "the scene ABI accepts the destroy");
    World::instance().flush();
    check(aver_fw_class_of(s) == 0, "class_of on an actor destroyed via the scene ABI is 0");
}

// ------------------------------------------------------------------------------------------ spawn

static void testSpawnAndClassOf() {
    AVER_INFO("=== spawn lays the archetype defaults down byte-identical ===");

    World& w = World::instance();

    const int32_t e = aver_fw_spawn(g_actor, "hero", nullptr, nullptr, nullptr);
    check(e != 0, "spawn returns a non-zero entity");
    const Entity ent = static_cast<Entity>(static_cast<uint32_t>(e));
    check(w.valid(ent), "the spawned entity is valid in the world");
    check(std::string(w.name(ent)) == "hero", "the optional name was applied");

    // class_of IS the definition of 'actor'.
    check(aver_fw_class_of(e) == g_actor, "class_of on the spawned entity returns its class");
    const int32_t plain = aver_scene_create();
    check(aver_fw_class_of(plain) == 0, "class_of on a plain scene entity returns 0");

    // ---- the CMeshRenderer default laid down field-by-field. Compare only the DEFINED fields, never a
    // whole-struct memcmp: sizeof(CMeshRenderer) rounds up to an 8-aligned 48 bytes with 4 trailing
    // padding bytes the ABI never defines, and both the archetype blob and a stack `want{}` carry
    // compiler-left padding a memcmp would compare — an over-reach that can spuriously fail under a
    // toolchain that does not zero-init padding. Checking the members constrains exactly the bytes the
    // ABI defines.
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

    // CLocal: scale came from a default; position was NOT overridden, so it is the CLocal default (0).
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

    // destroy + class_of afterwards
    check(aver_fw_destroy(e2) == 1, "destroy accepts the actor");
    World::instance().flush();
    check(aver_fw_class_of(e2) == 0, "class_of on a destroyed actor is 0");
}

// ------------------------------------------------------------------------------------- possession

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

    // ---- rejections: the flag check is the whole of the type safety.
    check(aver_fw_possess(pawn, controller) == 0, "possess is refused when the controller side lacks CONTROLLER");
    check(aver_fw_possess(controller, controller) == 0, "possess is refused when the pawn side lacks PAWN");
    const int32_t plainC = aver_fw_spawn(g_actor, "not-a-controller", nullptr, nullptr, nullptr);
    check(aver_fw_possess(plainC, pawn) == 0, "a flagless actor cannot possess");
    const int32_t plainScene = aver_scene_create();
    check(aver_fw_possess(controller, plainScene) == 0, "a plain scene entity (no class) cannot be possessed");

    // ---- the valid pair
    check(aver_fw_possess(controller, pawn) == 1, "a CONTROLLER possessing a PAWN is accepted");
    check(aver_fw_controlled_pawn(controller) == pawn, "controlled_pawn reads the possessed pawn");
    check(aver_fw_controller_of(pawn) == controller, "controller_of reads the controller");

    check(aver_fw_unpossess(controller) == 1, "unpossess releases the pawn");
    check(aver_fw_controlled_pawn(controller) == 0, "controlled_pawn is 0 after unpossess");
    check(aver_fw_controller_of(pawn) == 0, "controller_of is 0 after unpossess");
    check(aver_fw_unpossess(controller) == 0, "unpossess with nothing to release returns 0");

    // ---- destroying a possessed PAWN must clean the controller's forward entry too, or the two maps
    // stop being mutual inverses and pawnByController leaks. Observable through the pure ABI: after the
    // pawn dies the controller controls nothing, so unpossess must report 0 (it returned 1 with the
    // dangling forward entry — the leak this asserts against).
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

// --------------------------------------------------------------------------- later-stage stubs

static void testStubs() {
    AVER_INFO("=== session singletons are later-stage stubs (return 0) ===");
    check(aver_fw_game_instance() == 0, "game_instance is stubbed to 0");
    check(aver_fw_game_mode() == 0, "game_mode is stubbed to 0");
    check(aver_fw_player_controller(0) == 0, "player_controller is stubbed to 0");
    check(aver_fw_play_state() == 0, "play_state is stubbed to 0");
}

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
    testStubs();

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
