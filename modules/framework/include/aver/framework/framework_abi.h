#ifndef AVER_FRAMEWORK_ABI_H
#define AVER_FRAMEWORK_ABI_H

/* Gameplay framework C ABI — GameInstance, GameMode, actors, pawns and controllers.
 *
 * A P/Invoke surface: nothing but int32_t / int64_t / float / const char* crosses it, and there are
 * no function pointers, void*, structs or enums in it. The dispatch tables live in
 * framework_hooks.h. Aver.Framework links Aver.Scene, never the reverse.
 * Error convention: 1 on success, 0 on a rejected request. */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_FW_BUILD)
#    define AVER_FW_ABI __declspec(dllexport)
#  else
#    define AVER_FW_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_FW_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* AVER_FW_CALL is defined by framework_hooks.h, which owns every other callback typedef in this
 * ABI. Defined here too rather than including that header, because this one is the SURFACE C#
 * binds and pulling in the dispatch-table header for one macro would widen it for no reason. */
#if defined(_WIN32) && !defined(AVER_FW_CALL)
#  define AVER_FW_CALL __cdecl
#elif !defined(AVER_FW_CALL)
#  define AVER_FW_CALL
#endif

/* ABI version, as (major << 16) | minor. Versioned independently of AVER_SCENE_ABI_VERSION. */
#define AVER_FW_ABI_VERSION_MAJOR 1
/* 1: added aver_fw_set_view_entity / aver_fw_view_entity. Additive only.
 * 2: added aver_fw_set_sky_clouds / aver_fw_sky_clouds / aver_fw_clear_sky_clouds. Additive only,
 *    so a host built against minor 1 links and runs unchanged against this header.
 * 3: added aver_fw_set_fluid_spawn_provider / aver_fw_fluid_spawn. Additive only, same reason.
 * 4: added aver_fw_set_fluid_spawn_material_provider / aver_fw_fluid_spawn_material -- the material
 *    layer (real density, calibrated-fit viscosity, named presets) on top of minor 3's four raw
 *    solver knobs. Additive only, same reason: minor 3's own pair is UNCHANGED, so a host built
 *    against it still links and spawns exactly as before against this header.
 * 5: added the named-action layer (aver_fw_action_register/find/bind/clear_bindings/value2/held/
 *    pressed/released), porting scripting/csharp/Aver.Framework/EnhancedInput.cs's algorithm onto
 *    this ABI so a graph node or a C++ system can finally reach it, where before only a C# script
 *    could; a raw-VK twin to the aver_fw_input_* pair above (aver_fw_input_set_vk/vk/vk_pressed/
 *    vk_released) for the Win32 VK range the named AVER_FW_KEY_* enum cannot grow to cover; and the
 *    gamepad ABI's SHAPE with deliberately no polling behind it yet (aver_fw_input_set_gamepad_
 *    button/axis, aver_fw_input_gamepad_button/axis). Additive only, same reason as every entry
 *    above: minor 4's own surface is UNCHANGED, so a host built against it still links and runs
 *    unchanged against this header.
 * 6: added AVER_FW_ACTION_SRC_GAMEPAD_BUTTON / AVER_FW_ACTION_SRC_GAMEPAD_AXIS, so aver_fw_action_bind
 *    can finally bind a named action onto the gamepad ABI minor 5 only shaped -- polling now fills it
 *    (modules/platform's Gamepad.hpp/pollGamepads, published via Runtime/src/GameInput.cpp and
 *    sandbox/src/SandboxPlay.cpp), so a binding actually reads something a player moved. Additive
 *    only, same reason as every entry above: minor 5's own two sources and every other export are
 *    UNCHANGED, so a host built against it still links and runs unchanged against this header.
 * 7: added the INPUT SCHEME section (aver_fw_input_scheme_load/error/context_name/context_priority/
 *    action_count/action_name/action_type/binding_count/binding/binding_key) -- the C ABI loader
 *    modules/formats/include/aver/formats/OcInput.hpp's own INTEGRATION NOTE said .ocinput did not
 *    have yet, exposing that module's existing parser (aver::fmt::parseOcinput) so a C# InputScheme
 *    loader can turn a parsed .ocinput file into a live InputMappingContext without a second parser
 *    on the managed side. See that section's own comment for the one-scheme-at-a-time shape. Additive
 *    only, same reason as every entry above: minor 6's own surface is UNCHANGED, so a host built
 *    against it still links and runs unchanged against this header. */
#define AVER_FW_ABI_VERSION_MINOR 7
#define AVER_FW_ABI_VERSION \
    ((AVER_FW_ABI_VERSION_MAJOR << 16) | AVER_FW_ABI_VERSION_MINOR)

/* This binary's framework ABI version. */
AVER_FW_ABI int32_t aver_fw_abi_version(void);

/* The Aver.Scene ABI version this framework binary was BUILT against. */
AVER_FW_ABI int32_t aver_fw_scene_abi_version(void);

/* 1 when the Aver.Scene DLL actually loaded reports the same major as this binary compiled against.
 * Must keep calling into Aver.Scene, or the check becomes a tautology and the link edge vanishes. */
AVER_FW_ABI int32_t aver_fw_scene_abi_matches(void);

/* A class is DATA: a registry row holding a flattened component list and one blob of default
 * values. Spawning is a loop of memcpy over that blob. Every handle crosses as int32_t, 0 ==
 * invalid. Inbound strings are UTF-8; the one outbound string must not be freed by the caller.
 * These typedefs are documentation only — the exported signatures use int32_t. */
typedef int32_t aver_class;    /* a registry row; 0 invalid */
typedef int32_t aver_entity;   /* a scene entity; 0 invalid (== aver::scene::Entity across the ABI) */
typedef int32_t aver_field;    /* a dense scene field id; carries its component AND its kind */

/* Class flags — pinned to Aver.Framework's ClassFlags (Enums.cs). The PAWN / CONTROLLER pair IS
 * the whole of possess type-safety. */
#define AVER_FW_CLASS_TICKS         0x0001
#define AVER_FW_CLASS_PAWN          0x0002
#define AVER_FW_CLASS_CONTROLLER    0x0004
#define AVER_FW_CLASS_GAME_MODE     0x0008
#define AVER_FW_CLASS_GAME_INSTANCE 0x0010
#define AVER_FW_CLASS_MANAGED       0x0020
#define AVER_FW_CLASS_ABSTRACT      0x0040
#define AVER_FW_CLASS_TICK_IN_EDITOR 0x0080

/* Tick groups — pinned to Aver.Framework's TickGroup (Enums.cs). */
#define AVER_FW_TICK_PRE_PHYSICS  0
#define AVER_FW_TICK_PHYSICS      1
#define AVER_FW_TICK_POST_PHYSICS 2
#define AVER_FW_TICK_COUNT        3

/* Declares a class. Idempotent by name: the same name returns the same handle for the life of the
 * process, and a redeclare rewrites the row's components and defaults in place. */
AVER_FW_ABI int32_t aver_fw_class_declare(const char* name, const char* parentName);
/* The handle for a class name. */
AVER_FW_ABI int32_t aver_fw_class_find(const char* name);           /* 0 when unknown */
/* The class's name. */
AVER_FW_ABI const char* aver_fw_class_name(int32_t c);              /* "" for an invalid handle */
/* The class's parent handle. */
AVER_FW_ABI int32_t aver_fw_class_parent(int32_t c);               /* parent handle, 0 for a root */
/* Clears the class's components and defaults, keeping its identity and lineage. */
AVER_FW_ABI int32_t aver_fw_class_reset(int32_t c);                /* clear components + defaults */
/* Adds a scene component to the class's archetype. */
AVER_FW_ABI int32_t aver_fw_class_add_component(int32_t c, int32_t component);
/* Sets the class's AVER_FW_CLASS_* flags. */
AVER_FW_ABI int32_t aver_fw_class_set_flags(int32_t c, int32_t flags);
/* The class's AVER_FW_CLASS_* flags. */
AVER_FW_ABI int32_t aver_fw_class_get_flags(int32_t c);
/* Sets the class's tick group and order within it. */
AVER_FW_ABI int32_t aver_fw_class_set_tick(int32_t c, int32_t tickGroup, int32_t tickOrder);
/* Flattens the parent chain into the resolved archetype. 0 on a cycle or an undeclared parent.
 * Spawning auto-seals. */
AVER_FW_ABI int32_t aver_fw_class_seal(int32_t c);

/* Class defaults, addressed by the same dense field id the scene resolves. One setter per storable
 * kind; a wrong-kind default is rejected and stored nowhere. A default lands in the class row's
 * archetype blob, never on a live entity. */
AVER_FW_ABI int32_t aver_fw_class_set_default_f32(int32_t c, int32_t f, float v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i32(int32_t c, int32_t f, int32_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i64(int32_t c, int32_t f, int64_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_vec(int32_t c, int32_t f, const float* v);
AVER_FW_ABI int32_t aver_fw_class_set_default_str(int32_t c, int32_t f, const char* v);

/* Names the GameMode's default pawn class. Resolved by name at seal. */
AVER_FW_ABI int32_t aver_fw_class_set_default_pawn(int32_t gameMode, const char* pawnClassName);
/* Names the GameMode's player controller class. Resolved by name at seal. */
AVER_FW_ABI int32_t aver_fw_class_set_player_controller(int32_t gameMode, const char* controllerClassName);

/* Spawns an actor: attaches the sealed archetype's components, memcpys the class defaults into
 * each, records the entity's class, applies the optional name and transform overrides. Rotation
 * crosses as a quaternion; a null pos/quat/scale means "use the class default". 0 on failure. */
AVER_FW_ABI int32_t aver_fw_spawn(int32_t c, const char* name,
                                  const float* pos3, const float* quat4, const float* scale3);
/* Destroys an actor, dispatching OnEndPlay(DESTROY). */
AVER_FW_ABI int32_t aver_fw_destroy(int32_t e);

/* Spawns for a PREVIEW: bind and build_models, and stop. OnBeginPlay is NOT dispatched, so an
 * editor gets the actor's construction without the side effects a game does at birth. Tear down
 * with aver_fw_destroy_preview, never aver_fw_destroy. */
AVER_FW_ABI int32_t aver_fw_spawn_preview(int32_t c, const char* name,
                                          const float* pos3, const float* quat4, const float* scale3);
/* Destroys a preview actor without dispatching OnEndPlay. */
AVER_FW_ABI int32_t aver_fw_destroy_preview(int32_t e);

/* Dispatches OnBeginPlay on an actor that was spawned WITHOUT it, once its caller has finished
 * setting it up. `reason` is an AVER_FW_BEGIN_* value (framework_hooks.h).
 *
 * WHY THIS EXISTS: aver_fw_spawn is synchronous -- bind, build_models and beginPlay all run
 * inline before it returns -- so anything that must configure an actor BEFORE it begins playing
 * has no moment in which to do it. A save restore is exactly that: patch the saved fields in,
 * THEN begin play, or every actor OnBeginPlay reads its class defaults instead of the values the
 * player left it with. Pair it with aver_fw_spawn_preview.
 *
 * 0 for a stale handle, an entity that is not an actor, or one with no managed instance bound.
 * Calling it twice dispatches twice -- this ABI does not remember, and the caller that chose to
 * split the spawn is the one that knows. */
AVER_FW_ABI int32_t aver_fw_dispatch_begin_play(int32_t e, int32_t reason);

/* ---- SAVE/LOAD, RELAYED -----------------------------------------------------------------
 *
 * The framework does not know what a save file is and does not link the module that does, for
 * the same reason it does not link the animation system: Aver.Save sits at its own tier and only
 * a composition root links both. The host installs a provider and these forward -- the identical
 * shape aver_fw_set_anim_curve_provider already uses, three exports down.
 *
 * A host that installs nothing leaves both returning 0, which is what a game with no save system
 * should report rather than crashing. */
typedef int32_t (AVER_FW_CALL* aver_fw_save_fn)(const char* utf8Path, void* user);
/* Installs the pair. Either may be null. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_save_provider(aver_fw_save_fn write, aver_fw_save_fn load, void* user);
/* Writes the whole world to `utf8Path`. 0 when there is no provider or the write failed. */
AVER_FW_ABI int32_t aver_fw_save_write(const char* utf8Path);
/* Replaces the whole world from `utf8Path`. 0 when there is no provider or the load failed. */
AVER_FW_ABI int32_t aver_fw_save_load(const char* utf8Path);
/* The entity's class. */
AVER_FW_ABI int32_t aver_fw_class_of(int32_t e);   /* the entity's class, or 0 — != 0 IS "actor" */

/* Possesses a pawn. Rejected unless the controller's class carries CONTROLLER and the pawn's
 * carries PAWN — that flag check is the whole of the type safety. */
AVER_FW_ABI int32_t aver_fw_possess(int32_t controller, int32_t pawn);
/* Releases whatever pawn this controller drives. */
AVER_FW_ABI int32_t aver_fw_unpossess(int32_t controller);
/* The pawn this controller drives. */
AVER_FW_ABI int32_t aver_fw_controlled_pawn(int32_t controller);   /* the pawn, or 0 */
/* The controller driving this pawn. */
AVER_FW_ABI int32_t aver_fw_controller_of(int32_t pawn);           /* the controller, or 0 */

/* Play state: the world has two lives, EDITOR authoring and PLAYING. PAUSED freezes the tick
 * without tearing anything down. */
#define AVER_FW_PLAY_EDITOR  0
#define AVER_FW_PLAY_PLAYING 1
#define AVER_FW_PLAY_PAUSED  2

/* Begins a play session: spawns the optional GameInstance, the mandatory GameMode, and the
 * GameMode's controller and pawn (possessed). 0 if one was already running or the mode was
 * invalid. */
AVER_FW_ABI int32_t aver_fw_begin_play(int32_t gameInstanceClass, int32_t gameModeClass);
/* Ends the running session: OnEndPlay(STOP) and destroy every actor it spawned, back to EDITOR.
 * 0 if nothing was running. */
AVER_FW_ABI int32_t aver_fw_end_play(void);
/* Freezes (paused != 0) or resumes the tick without tearing the session down. 0 if not playing. */
AVER_FW_ABI int32_t aver_fw_set_paused(int32_t paused);
/* The first declared non-abstract class carrying ALL of `flags`, or 0. 0 flags -> 0. */
AVER_FW_ABI int32_t aver_fw_find_class_with_flags(int32_t flags);

/* ---- ANIMATION CURVES, RELAYED --------------------------------------------------------------
 *
 *
 * The framework does not know what an animation is and does not link the module that does. It
 * holds a function pointer the composition root installs, and forwards. That is the same shape
 * AnimSystem itself uses for asset resolution and for notifies, and it is why C# can ask "what
 * does this curve read" through the library it ALREADY binds instead of needing a new one.
 *
 * A host that installs nothing leaves aver_fw_anim_curve returning 0 for everything, which is the
 * same answer a clip with no such curve gives -- a game with no animation system is not an error. */
typedef int32_t (AVER_FW_CALL* aver_fw_anim_curve_fn)(int32_t entity, int64_t nameHash,
                                                      float* outValue, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_anim_curve_provider(aver_fw_anim_curve_fn fn, void* user);
/* Writes the curve value and returns 1, or leaves *outValue alone and returns 0 when there is no
 * provider, no clip on the entity, or no curve of that name. THE CALLER MUST DISTINGUISH THOSE
 * FROM A VALUE OF ZERO: a curve that reads 0 and a curve that is not there mean opposite things. */
AVER_FW_ABI int32_t aver_fw_anim_curve(int32_t entity, int64_t nameHash, float* outValue);

/* ---- GRAPH-LOCAL VARIABLES, RELAYED -----------------------------------------------------------
 *
 * The framework does not know what a graph is and does not link the module that does, for the
 * same reason it does not know what a save file is (aver_fw_set_save_provider, three sections up)
 * or an animation curve. A host installs a provider and these forward.
 *
 * WHY THIS EXISTS: a save must capture and restore graph-local VAR storage (Aver.Graph's own
 * GraphVarStore), which is pure managed state with no representation in the native scene at all --
 * see modules/save/include/aver/save/SaveWorld.hpp's own header for why that module cannot reach
 * Aver.Framework, let alone Aver.Graph, to get at it directly. This is the seam that lets a save
 * do it anyway, without either module gaining an edge it must not have.
 *
 * COUNT THEN INDEX, not one bulk call, and deliberately: everything else in this file is scalars
 * and const char* (see the file's own opening comment) -- a struct or an array-of-structs crossing
 * here would be the first of either. A handful of ABI calls per entity, only during a save or a
 * load, costs nothing worth avoiding that purity for.
 *
 * A host that installs nothing leaves every one of these returning 0 -- a game with no graph
 * scripting reports exactly that, the same as a clip with no such curve does for
 * aver_fw_anim_curve. */
typedef int32_t (AVER_FW_CALL* aver_fw_graph_var_count_fn)(int32_t entity, void* user);
/* Fills the name (into nameBuf, capacity nameBufLen bytes, UTF8, always NUL-terminated even when
 * truncated), kind (AVER_SCENE_KIND_F32/I32/BOOL -- scene_abi.h; a VAR is never any other kind)
 * and value (outF for F32, outI for I32/BOOL -- the same "kind decides which member" convention
 * aver::fmt::OcSaveField's own fields use) of VAR `index`, 0..count-1 in DECLARATION order. */
typedef int32_t (AVER_FW_CALL* aver_fw_graph_var_at_fn)(int32_t entity, int32_t index,
                                                        char* nameBuf, int32_t nameBufLen,
                                                        int32_t* outKind, float* outF,
                                                        int32_t* outI, void* user);
/* Sets one VAR by name on the entity's live graph host. 0 when there is no host, the name was
 * never declared, or `kind` disagrees with what was declared -- the identical "a kind mismatch is
 * a dropped field, not a crash" rule aver::save::applyField already applies to component fields. */
typedef int32_t (AVER_FW_CALL* aver_fw_graph_var_set_fn)(int32_t entity, const char* name,
                                                          int32_t kind, float f, int32_t i,
                                                          void* user);
/* Installs the trio. Any may be null. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_graph_var_provider(aver_fw_graph_var_count_fn count,
                                                   aver_fw_graph_var_at_fn at,
                                                   aver_fw_graph_var_set_fn setVar, void* user);
/* How many graph-local VARs entity `e`'s live graph host currently holds. 0 for no provider, no
 * host, or a graph declaring none. */
AVER_FW_ABI int32_t aver_fw_graph_var_count(int32_t e);
/* Relays to the installed provider's at() -- see aver_fw_graph_var_at_fn's own comment. 0 (and
 * *outKind/*outF/*outI left untouched) for no provider or an out-of-range index. */
AVER_FW_ABI int32_t aver_fw_graph_var_at(int32_t e, int32_t index, char* nameBuf,
                                         int32_t nameBufLen, int32_t* outKind, float* outF,
                                         int32_t* outI);
/* Relays to the installed provider's setVar() -- see aver_fw_graph_var_set_fn's own comment. */
AVER_FW_ABI int32_t aver_fw_graph_var_set(int32_t e, const char* name, int32_t kind, float f, int32_t i);

/* ---- SYNAPSE STEERING TARGET, RELAYED ---------------------------------------------------------
 *
 * The framework does not know what Synapse is and does not link the module that does, for the
 * identical reason it does not know what a graph or an animation curve is: a host installs a
 * provider and this forwards. SYNAPSE ADVISES, IT DOES NOT MOVE (see SynapseAgent.hpp's own header
 * comment) -- AgentSystem tracks a path and writes the agent's current waypoint onto its own
 * CSynapseAgent component, and this is the one seam that lets a graph's GetSynapseTarget node read
 * that value back, exactly as aver_fw_anim_curve lets a graph read an animation curve it cannot see
 * the component of directly. CSynapseAgent's fields ARE registered through the generic scene
 * reflection API (World::registerComponent), but that reflection is a C++-only surface
 * (World::fieldId/field/fieldCount/fieldAt) with no C ABI of its own -- scene_abi.h exposes typed
 * get/set by dense FIELD ID, never a lookup BY NAME, so a caller that only has "CSynapseAgent" and
 * "targetXCm" as strings (as every managed caller does) has no other way in.
 *
 * A host that installs nothing leaves this returning 0 for everything -- a game with no Synapse
 * agents is not an error, the same as a clip with no curve or a graph with no VAR. */
typedef int32_t (AVER_FW_CALL* aver_fw_synapse_target_fn)(int32_t entity, float* outX, float* outY,
                                                           float* outZ, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_synapse_target_provider(aver_fw_synapse_target_fn fn, void* user);
/* Writes the entity's current steering target and returns 1, or leaves the outputs untouched and
 * returns 0 when there is no provider, `e` carries no CSynapseAgent, or its status is not Pathing
 * (None/Requested/Arrived/Failed all mean "nothing to head toward right now"). */
AVER_FW_ABI int32_t aver_fw_synapse_target(int32_t e, float* outX, float* outY, float* outZ);

/* ---- SYNAPSE PERCEPTION, RELAYED ---------------------------------------------------------------
 *
 * Same reason and same shape as the steering-target relay immediately above: Aver.Synapse.Scene
 * must not link Aver.Framework, so a host installs a provider and this forwards.
 *
 * UNLIKE aver_fw_synapse_target, 0/false here does NOT mean "nothing to report" -- "I currently
 * cannot see the target" is a real, common, meaningful answer for a perceiving agent, not an
 * absence. The RETURN VALUE distinguishes the two instead: 0 only when `e` carries no
 * CSynapsePerception at all (or there is no provider); 1 whenever it does, with *outCanSee telling
 * the caller which case applies. This is the same "false means absent, not zero" discipline
 * aver_fw_anim_curve's own comment states, applied one level up: here the OUTER call tells you
 * "there is an answer", and one of the outputs tells you what it is. */
typedef int32_t (AVER_FW_CALL* aver_fw_synapse_perception_fn)(int32_t entity, int32_t* outCanSee,
                                                               int32_t* outLastTarget,
                                                               float* outTimeSinceSeen, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_synapse_perception_provider(aver_fw_synapse_perception_fn fn, void* user);
/* Writes the entity's current perception state and returns 1, or leaves the outputs untouched and
 * returns 0 when there is no provider or `e` carries no CSynapsePerception. */
AVER_FW_ABI int32_t aver_fw_synapse_perception(int32_t e, int32_t* outCanSee, int32_t* outLastTarget,
                                               float* outTimeSinceSeen);

/* The session singletons begin_play populated; each is 0 in EDITOR. */
AVER_FW_ABI int32_t aver_fw_game_instance(void);
/* The running session's GameMode. */
AVER_FW_ABI int32_t aver_fw_game_mode(void);
/* The player controller for a 0-based index; only player 0 exists until split-screen does. */
AVER_FW_ABI int32_t aver_fw_player_controller(int32_t playerIndex);
/* The current AVER_FW_PLAY_* state. */
AVER_FW_ABI int32_t aver_fw_play_state(void);

/* Input: the framework holds no window, so the app pushes key and mouse state each frame and
 * gameplay reads it back. The codes live here so a script can name a key without depending on the
 * editor. */
enum {
    AVER_FW_KEY_A = 0, AVER_FW_KEY_B, AVER_FW_KEY_C, AVER_FW_KEY_D, AVER_FW_KEY_E, AVER_FW_KEY_F,
    AVER_FW_KEY_G, AVER_FW_KEY_H, AVER_FW_KEY_I, AVER_FW_KEY_J, AVER_FW_KEY_K, AVER_FW_KEY_L,
    AVER_FW_KEY_M, AVER_FW_KEY_N, AVER_FW_KEY_O, AVER_FW_KEY_P, AVER_FW_KEY_Q, AVER_FW_KEY_R,
    AVER_FW_KEY_S, AVER_FW_KEY_T, AVER_FW_KEY_U, AVER_FW_KEY_V, AVER_FW_KEY_W, AVER_FW_KEY_X,
    AVER_FW_KEY_Y, AVER_FW_KEY_Z,                                   /* A..Z = 0..25 */
    AVER_FW_KEY_0, AVER_FW_KEY_1, AVER_FW_KEY_2, AVER_FW_KEY_3, AVER_FW_KEY_4,
    AVER_FW_KEY_5, AVER_FW_KEY_6, AVER_FW_KEY_7, AVER_FW_KEY_8, AVER_FW_KEY_9,   /* 0..9 = 26..35 */
    AVER_FW_KEY_SPACE, AVER_FW_KEY_LSHIFT, AVER_FW_KEY_LCTRL, AVER_FW_KEY_LALT,
    AVER_FW_KEY_ENTER, AVER_FW_KEY_ESCAPE, AVER_FW_KEY_TAB,
    AVER_FW_KEY_LEFT, AVER_FW_KEY_RIGHT, AVER_FW_KEY_UP, AVER_FW_KEY_DOWN,
    AVER_FW_KEY_MOUSE_LEFT, AVER_FW_KEY_MOUSE_RIGHT, AVER_FW_KEY_MOUSE_MIDDLE,
    AVER_FW_KEY_COUNT
};
/* Rolls current key state into previous. Call once a frame, before the set_key calls. */
AVER_FW_ABI void    aver_fw_input_new_frame(void);
/* Sets the held state of a key. Out-of-range keys are ignored. */
AVER_FW_ABI void    aver_fw_input_set_key(int32_t key, int32_t down);
/* Sets this frame's mouse delta (pixels) and wheel notches. */
AVER_FW_ABI void    aver_fw_input_set_mouse(float dx, float dy, float wheel);
/* 1 while the key is held. */
AVER_FW_ABI int32_t aver_fw_input_key(int32_t key);
/* 1 on the frame the key went down. */
AVER_FW_ABI int32_t aver_fw_input_key_pressed(int32_t key);
/* 1 on the frame the key went up. */
AVER_FW_ABI int32_t aver_fw_input_key_released(int32_t key);
/* Writes {dx, dy, wheel} into out3. */
AVER_FW_ABI void    aver_fw_input_mouse(float* out3);

/* ---- NAMED ACTIONS (Enhanced Input), PORTED FROM Aver.Framework's EnhancedInput.cs ---------------
 *
 * scripting/csharp/Aver.Framework/EnhancedInput.cs is a complete, well-designed named-action layer
 * -- InputAction (Digital/Axis1D/Axis2D), InputBinding, InputMappingContext, priority-stacked
 * contexts with PER-LAYER KEY CONSUMPTION, a 0.15 dead zone, WasPressed/WasReleased -- with zero
 * consumers and zero tests, because it is pure C# with no way for a graph node or a C++ system to
 * reach it. Its ALGORITHM is ported here verbatim; its PLACEMENT (unreachable from anywhere but a
 * C# script) is the defect this section fixes.
 *
 * THIS ABI HAS NO CONTEXT HANDLE. EnhancedInput.cs pushes whole InputMappingContext OBJECTS onto a
 * priority-sorted stack (AddContext/RemoveContext), and every binding inside one object shares that
 * object's priority. A C ABI has no object to push, so `contextPriority` on EVERY binding stands in
 * for it -- two bindings sharing a `contextPriority` number ARE one context for consumption
 * purposes, exactly as two bindings inside one InputMappingContext are (EnhancedInput.cs's own
 * Update() comment: "Consumption is per layer, so two bindings in one context can share a key").
 * There is consequently no partial "pop this one context" call either -- aver_fw_action_clear_
 * bindings drops every binding at once, and a caller that wants to swap contexts re-binds everything
 * after that, which is what RemoveContext+AddContext amount to from outside EnhancedInput.cs anyway.
 *
 * NO SEPARATE PER-FRAME "Update()" ENTRY POINT, unlike EnhancedInput.cs's own Update(): held/
 * pressed/released/value2 below evaluate ON DEMAND, straight out of the SAME InputState cur/prev
 * (and mouse/prevMouse) arrays aver_fw_input_key / aver_fw_input_key_pressed already read, and, for
 * the two GAMEPAD_* sources, the SAME GamepadState buttons/axes (and prevButtons/prevAxes) arrays
 * aver_fw_input_gamepad_button/axis already read (all FrameworkAbi.cpp). That is a hard requirement,
 * not a style choice: an action bound to AVER_FW_KEY_W and a script calling
 * aver_fw_input_key(AVER_FW_KEY_W) directly must NEVER be able to disagree about whether the key is
 * down, and the only way to guarantee that is to have both read the identical bytes instead of two
 * copies that could drift out of step. This is also why aver_fw_input_new_frame now rolls a
 * `prevMouse` snapshot alongside `prev` (FrameworkAbi.cpp), and, since minor 6, a GamepadState
 * prevButtons/prevAxes snapshot alongside those -- a mouse- or gamepad-sourced action needs a "was
 * this channel already active last frame" answer to detect an edge on, the same thing a keyboard
 * binding gets for free from cur/prev, and that snapshot is the one piece of state EnhancedInput.cs's
 * Raw/Prev pair carried that plain InputState (and, now, GamepadState) had no slot for.
 *
 * DEAD ZONE 0.15, PINNED to EnhancedInput.cs's own Active() check -- not a parameter, because the
 * algorithm being ported is not configurable either; a caller who needs a different threshold is
 * asking for a different feature, not a variant of this one. */
#define AVER_FW_ACTION_DIGITAL 0
#define AVER_FW_ACTION_AXIS1D  1
#define AVER_FW_ACTION_AXIS2D  2

/* Where one binding reads from -- pinned to EnhancedInput.cs's own InputSource enum.
 * AVER_FW_ACTION_SRC_KEY reads an AVER_FW_KEY_* slot (the SAME enum aver_fw_input_key reads, never a
 * raw VK -- see the RAW WIN32 VK section below for that ABI instead); the three MOUSE_* sources read
 * whatever aver_fw_input_set_mouse published this frame (dx, dy, wheel respectively);
 * AVER_FW_ACTION_SRC_GAMEPAD_BUTTON and AVER_FW_ACTION_SRC_GAMEPAD_AXIS read the GAMEPAD section's own
 * pad-0 state (aver_fw_input_gamepad_button/axis) -- see that section, and aver_fw_action_bind's own
 * comment just below, for what `key` carries for each. */
#define AVER_FW_ACTION_SRC_KEY            0
#define AVER_FW_ACTION_SRC_MOUSE_X        1
#define AVER_FW_ACTION_SRC_MOUSE_Y        2
#define AVER_FW_ACTION_SRC_MOUSE_WHEEL    3
#define AVER_FW_ACTION_SRC_GAMEPAD_BUTTON 4
#define AVER_FW_ACTION_SRC_GAMEPAD_AXIS   5

/* Declares a named action, or returns the existing handle for `name` unchanged -- IDEMPOTENT BY
 * NAME, the same convention aver_fw_class_declare uses above and for the same reason: a script's
 * OnBeginPlay runs every time its actor spawns, and re-registering "Jump" on every possession must
 * hand back the ORIGINAL action rather than silently multiplying it. 0 for a null/empty name or a
 * valueType outside AVER_FW_ACTION_DIGITAL..AVER_FW_ACTION_AXIS2D. */
AVER_FW_ABI int32_t aver_fw_action_register(const char* name, int32_t valueType);
/* The handle for a previously registered action name, or 0. */
AVER_FW_ABI int32_t aver_fw_action_find(const char* name);
/* Adds one binding to `action`. `key` is an AVER_FW_KEY_* slot for AVER_FW_ACTION_SRC_KEY, an
 * AVER_FW_GAMEPAD_* button for AVER_FW_ACTION_SRC_GAMEPAD_BUTTON, an AVER_FW_GAMEPAD_AXIS_* axis for
 * AVER_FW_ACTION_SRC_GAMEPAD_AXIS, and ignored for the three MOUSE_* sources (EnhancedInput.cs's own
 * BindMouseLook/BindMouseWheel likewise carry an unused Key.A placeholder on a mouse-sourced
 * InputBinding -- ported as-is rather than inventing a second binding shape just to avoid one ignored
 * parameter). `scale` multiplies the source value before it accumulates into the action -- this is
 * how EnhancedInput.cs's BindAxis1D turns two opposed keys into one -1..1 axis: +1 scale on one key,
 * -1 on the other, both landing in the same component. `component` selects which channel the value
 * lands in: 0 = X, 1 = Y, anything else = Z (Z exists only because EnhancedInput.cs's own Accumulate()
 * has a third case; nothing here binds it, and aver_fw_action_value2 does not read it back).
 * `contextPriority` is the tier this section's own opening comment describes: a STRICTLY higher
 * `contextPriority` binding on the SAME key blocks this one from ever seeing it, win or lose based on
 * which context the caller considers "in front" this frame, not on declaration order -- KEY bindings
 * only; a gamepad button binding is never consumed this way (FrameworkAbi.cpp's own comment on
 * actionKeyConsumedByHigherPriority says why), so two contexts bound to the SAME button both see it,
 * the same as two MOUSE_* bindings sharing a channel already do. Silently ignored for an invalid
 * action, an out-of-range source, or an out-of-range key/button/axis for a source that reads one --
 * matching aver_fw_input_set_key's own "out-of-range keys are ignored" convention a few lines above. */
AVER_FW_ABI void    aver_fw_action_bind(int32_t action, int32_t source, int32_t key, float scale,
                                        int32_t component, int32_t contextPriority);
/* Drops every binding on every action. Registrations (and their handles) survive -- only the map
 * from keys to actions is cleared, so a caller re-establishes a whole set of contexts by calling
 * this once and then aver_fw_action_bind for each binding again, the same net effect as
 * EnhancedInput.ClearContexts() followed by fresh AddContext calls. */
AVER_FW_ABI void    aver_fw_action_clear_bindings(void);
/* Writes {X, Y} of the action's CURRENT accumulated value into out2 -- EnhancedInput.cs's Value2D
 * without the Z channel this ABI has no consumer for. Applies NO dead zone (same as Value2D/Raw) --
 * that only gates held/pressed/released below. {0, 0} for an invalid handle. */
AVER_FW_ABI void    aver_fw_action_value2(int32_t action, float* out2);
/* 1 while the action is active (any channel's magnitude exceeds the 0.15 dead zone). 0 for an
 * invalid handle. */
AVER_FW_ABI int32_t aver_fw_action_held(int32_t action);
/* 1 on the frame the action became active. 0 for an invalid handle. */
AVER_FW_ABI int32_t aver_fw_action_pressed(int32_t action);
/* 1 on the frame the action stopped being active. 0 for an invalid handle. */
AVER_FW_ABI int32_t aver_fw_action_released(int32_t action);

/* ---- RAW WIN32 VK, ADDITIVE TWIN TO AVER_FW_KEY_* -------------------------------------------------
 *
 * frameworkKeyFromVk (aver/framework/InputKeys.hpp) maps Win32 virtual keys onto the AVER_FW_KEY_*
 * enum above, and that header's own comment says why most of the VK range falls through to -1: "the
 * framework enum has 46 slots and Win32 has 256 codes, so F-keys, the numpad and every OEM key are
 * simply unreachable by gameplay today." The SAME comment gives the reason the enum above cannot
 * just grow to cover them: "The enum cannot be renumbered to fix it -- the InputKey graph node
 * takes a literal integer, so saved graphs depend on the current numbering." A saved .ocgraph's
 * InputKey node stores (say) AVER_FW_KEY_LEFT as whatever plain int that slot currently is; inserting
 * a new named key anywhere but the enum's own tail would silently repoint every saved graph's
 * InputKey node at the WRONG key, with no error at load.
 *
 * This is the escape hatch: a second, parallel cur/prev array indexed by the RAW vk code (0..255,
 * matching aver::platform::InputState::kKeyCount -- modules/platform/include/aver/platform/
 * InputState.hpp), published and read exactly like the named-slot pair above but never subject to
 * the renumbering constraint, because nothing here is a graph node's literal operand -- a caller
 * that wants F5 asks for vk 0x74 by value, not through an enum name that could move. */
/* The Win32 VK range this twin covers, 0..(AVER_FW_VK_COUNT-1) -- Win32's own VK codes are 0..255. */
#define AVER_FW_VK_COUNT 256
/* Sets the held state of a raw Win32 VK. Out-of-range (outside 0..255) is ignored, matching
 * aver_fw_input_set_key's own convention. Independent of aver_fw_input_set_key -- setting the named
 * AVER_FW_KEY_A slot does NOT also set raw vk 'A', and vice versa; they are two separate arrays that
 * happen to be fed the same physical key by whichever host publishes both. */
AVER_FW_ABI void    aver_fw_input_set_vk(int32_t vk, int32_t down);
/* 1 while the raw VK is held. */
AVER_FW_ABI int32_t aver_fw_input_vk(int32_t vk);
/* 1 on the frame the raw VK went down. */
AVER_FW_ABI int32_t aver_fw_input_vk_pressed(int32_t vk);
/* 1 on the frame the raw VK went up. */
AVER_FW_ABI int32_t aver_fw_input_vk_released(int32_t vk);

/* ---- GAMEPAD, STATE IN / STATE OUT -------------------------------------------------------------
 *
 * This ABI's OWN job stops at "state in, state out", like every other aver_fw_input_* pair above:
 * it does not open a device itself, does not detect hotplug, and applies no dead zone of its own.
 * That is by design, not an oversight -- XInput hotplug (a controller can vanish mid-frame and
 * XInputGetState keeps returning stale data for that slot, not a trustworthy error), trigger and
 * stick dead zones (XInput's own guidance is a per-stick radius, a different shape of problem than
 * the flat 0.15 the action layer above uses -- not a constant the two can share), and rumble are
 * each their own scope, better owned by whatever polls the real device than folded into this thin
 * ABI.
 *
 * POLLING NOW EXISTS: modules/platform's Gamepad.hpp (pollGamepads) opens the device, and
 * Runtime/src/GameInput.cpp and sandbox/src/SandboxPlay.cpp publish what it reads through
 * aver_fw_input_set_gamepad_button/axis below, once a frame, the same as they already publish keys
 * and mouse state. This section was shipped SHAPE ONLY, with deliberately no polling behind it, back
 * when minor 5 added it (this header's own changelog above) -- minor 6 is this layer's first real
 * consumer: aver_fw_action_bind's own AVER_FW_ACTION_SRC_GAMEPAD_BUTTON/AXIS (NAMED ACTIONS section
 * above) read pad 0's state back out through this section, exactly as a KEY-sourced binding reads
 * aver_fw_input_key's own state.
 *
 * Buttons and axes are modelled on XInput's own XINPUT_GAMEPAD_* bitmask and XINPUT_STATE thumbstick/
 * trigger fields -- 14 buttons, 6 axes -- so that a provider is a mechanical bit-to-index and
 * int16-to-float unpack, not a redesign. `pad` is fixed at 0 for every call below, the same "only
 * player 0 exists until split-screen does" precedent aver_fw_player_controller documents above --
 * every function here rejects any other value exactly as that one rejects any other player index. */
enum {
    AVER_FW_GAMEPAD_DPAD_UP = 0, AVER_FW_GAMEPAD_DPAD_DOWN, AVER_FW_GAMEPAD_DPAD_LEFT,
    AVER_FW_GAMEPAD_DPAD_RIGHT, AVER_FW_GAMEPAD_START, AVER_FW_GAMEPAD_BACK,
    AVER_FW_GAMEPAD_LEFT_THUMB, AVER_FW_GAMEPAD_RIGHT_THUMB,
    AVER_FW_GAMEPAD_LEFT_SHOULDER, AVER_FW_GAMEPAD_RIGHT_SHOULDER,
    AVER_FW_GAMEPAD_A, AVER_FW_GAMEPAD_B, AVER_FW_GAMEPAD_X, AVER_FW_GAMEPAD_Y,
    AVER_FW_GAMEPAD_BUTTON_COUNT
};
enum {
    AVER_FW_GAMEPAD_AXIS_LEFT_X = 0, AVER_FW_GAMEPAD_AXIS_LEFT_Y,
    AVER_FW_GAMEPAD_AXIS_RIGHT_X, AVER_FW_GAMEPAD_AXIS_RIGHT_Y,
    AVER_FW_GAMEPAD_AXIS_LEFT_TRIGGER, AVER_FW_GAMEPAD_AXIS_RIGHT_TRIGGER,
    AVER_FW_GAMEPAD_AXIS_COUNT
};
/* Sets one button's held state. `pad` must be 0; an out-of-range pad or button is ignored. */
AVER_FW_ABI void    aver_fw_input_set_gamepad_button(int32_t pad, int32_t button, int32_t down);
/* Sets one axis's value. Unclamped -- this ABI applies no dead zone (see this section's own header
 * comment), so a future provider's raw stick/trigger reading crosses exactly as read. */
AVER_FW_ABI void    aver_fw_input_set_gamepad_axis(int32_t pad, int32_t axis, float value);
/* 1 while the button is held. 0 for `pad` != 0 or an out-of-range button. */
AVER_FW_ABI int32_t aver_fw_input_gamepad_button(int32_t pad, int32_t button);
/* The axis's last-set value. 0.0 for `pad` != 0 or an out-of-range axis. */
AVER_FW_ABI float   aver_fw_input_gamepad_axis(int32_t pad, int32_t axis);

/* Play view: a possessed character publishes the camera it wants and the editor reads it back.
 * One request (one local player) until split-screen exists. */
#define AVER_FW_VIEW_FIRST_PERSON 0
#define AVER_FW_VIEW_THIRD_PERSON 1
/* Publishes the wanted view mode and its eye/boom offsets. */
AVER_FW_ABI void aver_fw_set_view(int32_t mode, float eyeHeight, float boomLength);
/* Reads the published view mode and offsets. */
AVER_FW_ABI void aver_fw_view(int32_t* outMode, float* outEyeHeight, float* outBoomLength);

/* Publishes the scene node the camera sits on, so the editor reads a transform instead of
 * reconstructing one from the pawn matrix. 0 means none published and the caller falls back. */
AVER_FW_ABI void    aver_fw_set_view_entity(int32_t entity);
/* The published view entity, or 0. Valid only while the scene says it is. */
AVER_FW_ABI int32_t aver_fw_view_entity(void);

/* ---- the sky's cloud layer, published by a script ------------------------------------------
 *
 * A REQUEST, NOT THE TRUTH: until a script calls the setter, aver_fw_sky_clouds returns 0 and the
 * host keeps whatever the level authored, so a project with no sky script renders exactly as it
 * did before these entry points existed.
 *
 * Lengths are CENTIMETRES and wind is centimetres per second, like everything else in this engine.
 * featureScale is 1 / the width of one noise feature in world units.
 */
AVER_FW_ABI void aver_fw_set_sky_clouds(int32_t seed, float coverage, float density,
                                        float bottomCm, float topCm, float featureScale,
                                        float windXCmPerSec, float windYCmPerSec);
/* Reads the published cloud layer. Returns 1 when one has been published, 0 otherwise, and writes
 * nothing through the out pointers when it returns 0. Every pointer may be NULL. */
AVER_FW_ABI int32_t aver_fw_sky_clouds(int32_t* outSeed, float* outCoverage, float* outDensity,
                                       float* outBottomCm, float* outTopCm, float* outFeatureScale,
                                       float* outWindX, float* outWindY);
/* Drops the request, handing the sky back to the level. */
AVER_FW_ABI void aver_fw_clear_sky_clouds(void);

/* ---- FLUID VOLUME SPAWN, RELAYED ------------------------------------------------------------
 *
 * The framework does not know what a fluid volume is and does not link the module that does --
 * Aver.Fluids sits beside Aver.Physics at its own tier (see modules/fluids/include/aver/fluids/
 * FluidVolume.hpp's own header: "this module must never learn what Jolt is", and only the
 * composition root links both). Same shape as SAVE/LOAD and ANIMATION CURVES above: a host
 * installs a provider and this forwards.
 *
 * WHY THIS EXISTS: a level's WATER record (SandboxApp::applyLevelWater) and a graph's
 * `COMP <id> Fluid ...` component (Aver.Graph's GraphComponentTree.ApplyKind, "fluid" case, via
 * Aver.Framework's Game.SpawnFluidVolume) both need to hand a fluids::FluidVolumeDesc to
 * fluids::FluidScene::spawn. This is the one seam a graph component or a plain C# script can
 * cross to ask for that, without either of them linking Aver.Fluids or Aver.Physics directly.
 * `subdivisions` is deliberately NOT a parameter here -- nobody asked for authorable mesh
 * resolution (see FluidVolume.hpp's own FluidVolumeDesc comment); every request gets that
 * struct's own default subdivision.
 *
 * QUEUED, NOT SYNCHRONOUS. The return value says a provider accepted the request, not that a
 * volume now simulates. applyLevelWater's own latch hit this exact problem first: FluidScene is
 * not ready() until after render features come up, and this relay can be reached from points
 * that run before that (an actor bound while a level loads, a script's own OnBeginPlay) just as
 * easily as from ones that run after. A provider is expected to queue the request and drain it
 * at the same frame-safe point applyLevelWater's own request is drained, not spawn from inside
 * the callback -- whether it actually spawned is reported by the host's own log, not by this
 * call returning.
 *
 * A host that installs nothing leaves this returning 0 -- a build with no fluids module linked
 * reports exactly that, the same as a clip with no such curve reports for aver_fw_anim_curve. */
typedef int32_t (AVER_FW_CALL* aver_fw_fluid_spawn_fn)(
    float cx, float cy, float cz,          /* world centre, cm */
    float hx, float hy, float hz,          /* half-extent, cm */
    float compliance, float damping, int32_t iterations, float pressure,
    const char* name,                      /* a label for the host's own log; never null */
    void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_fluid_spawn_provider(aver_fw_fluid_spawn_fn fn, void* user);
/* Requests a fluid volume. `pressure` < 0 asks the solver to derive it from the volume's own size
 * (fluids::kFluidPressureAuto -- the same sentinel OcWaterPlacement's own pressure field uses),
 * matching what an unauthored `pressure=` already means on a WATER record. Returns 1 when a
 * provider accepted the request, 0 when there is none. */
AVER_FW_ABI int32_t aver_fw_fluid_spawn(float cx, float cy, float cz, float hx, float hy, float hz,
                                        float compliance, float damping, int32_t iterations,
                                        float pressure, const char* name);

/* ---- FLUID VOLUME SPAWN, WITH A MATERIAL --------------------------------------------------
 *
 * A SECOND, ADDITIVE relay beside aver_fw_fluid_spawn above, not a replacement for it -- see this
 * header's own MINOR 4 changelog entry. Carries the same six placement floats and the same four
 * raw solver knobs PLUS the material layer (fluids::FluidPhysicsMaterial): `densityKgM3`/`viscosityPaS`
 * (< 0 means "not given", the same sentinel convention `pressure` already uses on the plain
 * relay), or `materialPreset` (a name -- "water", "lightoil", "honey", "lava", case-insensitive;
 * empty means none). A non-empty preset is resolved to its own density/viscosity by the provider,
 * which links fluids::FluidPhysicsMaterial's real presets -- this ABI carries only a name across the
 * boundary, never a duplicated set of literal numbers, so the framework still never learns what a
 * FluidPhysicsMaterial actually contains.
 *
 * `damping` HERE CAN STILL CONFLICT WITH A MATERIAL, DELIBERATELY: this relay does not itself
 * decide which one wins. An author who writes both a material (preset or density/viscosity) and a
 * non-default `damping` on the same request is forwarded through untouched, exactly as authored --
 * the refusal happens once, downstream, at fluids::FluidScene::spawn (the one place both this
 * relay and a level's own WATER record converge), not here and not twice. */
typedef int32_t (AVER_FW_CALL* aver_fw_fluid_spawn_material_fn)(
    float cx, float cy, float cz,          /* world centre, cm */
    float hx, float hy, float hz,          /* half-extent, cm */
    float compliance, float damping, int32_t iterations, float pressure,
    float densityKgM3, float viscosityPaS, /* < 0 means "not given" on either */
    const char* materialPreset,            /* preset name, or "" for none; never null */
    const char* name,                      /* a label for the host's own log; never null */
    void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_fluid_spawn_material_provider(aver_fw_fluid_spawn_material_fn fn,
                                                               void* user);
/* Requests a fluid volume with a material. Same `pressure` sentinel as aver_fw_fluid_spawn.
 * Returns 1 when a provider accepted the request, 0 when there is none. */
AVER_FW_ABI int32_t aver_fw_fluid_spawn_material(float cx, float cy, float cz,
                                                 float hx, float hy, float hz,
                                                 float compliance, float damping, int32_t iterations,
                                                 float pressure, float densityKgM3, float viscosityPaS,
                                                 const char* materialPreset, const char* name);

/* ---- INPUT SCHEME, LOADED FROM .ocinput -------------------------------------------------------
 *
 * modules/formats/include/aver/formats/OcInput.hpp defines the .ocinput text format -- named input
 * actions and their default key/mouse/gamepad bindings -- and until now nothing loaded one: that
 * header's own INTEGRATION NOTE said so plainly ("there is no C ABI export or C# loader for it
 * yet"). This section is that loader's C surface: the ONE existing C++ parser
 * (aver::fmt::parseOcinput, via loadOcinput, in OcInput.cpp) exposed here so
 * scripting/csharp/Aver.Framework/InputScheme.cs can turn a parsed file into a real
 * InputMappingContext without a second parser on the managed side -- the same division of labour
 * this ABI already draws for a graph (aver::fmt::parseOcgraph, reached through DeclareGraphClasses)
 * and, closer still, for the NAMED ACTIONS layer three sections up (EnhancedInput.cs's algorithm,
 * ported here so something other than a C# script can reach it).
 *
 * ONE PARSED SCHEME, NOT A HANDLE TABLE, on the same precedent as the NAMED ACTIONS section having
 * no context handle: a project names exactly one scheme (OcProject.hpp's own INPUT.SCHEME /
 * inputScheme field), so this ABI holds one file-static aver::fmt::OcInputData and every getter
 * below reads out of it. A second load REPLACES the slot outright, success or failure alike -- there
 * is no "keep the old one if the new one fails to parse" behaviour, matching aver_fw_action_clear_
 * bindings' own "drop everything, the caller rebuilds" shape a few sections up.
 *
 * COUNT THEN INDEX, the same convention aver_fw_graph_var_count/_at use above and for the same
 * reason stated there: a struct or an array-of-structs crossing this boundary would be the first of
 * either in this header, and a handful of calls once per load costs nothing worth breaking that for.
 *
 * RETURNED STRINGS STAY VALID UNTIL THE NEXT aver_fw_input_scheme_load -- not merely until the next
 * call, the lifetime every other const char* in this header gets (and which the caller must still
 * never free). A caller here is expected to hold action and binding-key names across several calls
 * while it walks a whole scheme, so this section states that wider lifetime explicitly. */

/* Loads and parses a scheme from an .ocinput file, replacing whatever was loaded before -- on
 * either outcome. 1 when it parsed, 0 when the path was null/empty or the file was missing,
 * unreadable or malformed (aver_fw_input_scheme_error carries why); either way the previously
 * loaded scheme is gone, so a caller wanting to fall back to it must have kept its own copy. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_load(const char* utf8Path);
/* The last load's parse error, or "" after a successful load. */
AVER_FW_ABI const char* aver_fw_input_scheme_error(void);
/* The loaded scheme's CONTEXT name, or "" when the file had no CONTEXT record (OcInputData::
 * contextName's own empty-is-absent convention) or nothing is loaded. */
AVER_FW_ABI const char* aver_fw_input_scheme_context_name(void);
/* The loaded scheme's CONTEXT priority, or 0 when absent or nothing is loaded. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_context_priority(void);
/* How many ACTION records the loaded scheme declares. 0 when nothing is loaded. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_action_count(void);
/* Action `index`'s name (0..action_count-1, declaration order), or nullptr out of range. */
AVER_FW_ABI const char* aver_fw_input_scheme_action_name(int32_t index);
/* Action `index`'s declared value type: 0 digital, 1 axis1d, 2 axis2d -- OcInputValueType's own
 * order, which is also C# InputValueType's order, so InputScheme.cs's loader reads this straight
 * into InputAction.Digital/Axis1D/Axis2D with no translation table. -1 out of range. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_action_type(int32_t index);
/* How many BIND records the loaded scheme declares. 0 when nothing is loaded. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_binding_count(void);
/* Binding `index`'s fields (0..binding_count-1, declaration order). *outActionIndex is the 0-based
 * index INTO aver_fw_input_scheme_action_name/_type of the ACTION this binding names -- NOT an
 * aver_fw_action_register handle; the caller resolves that itself once it has registered the
 * action. *outSource is an AVER_FW_ACTION_SRC_* value (the NAMED ACTIONS section above), OcInputSource
 * mapped one for one: Key -> SRC_KEY, MouseX/MouseY -> SRC_MOUSE_X/SRC_MOUSE_Y, MouseWheel ->
 * SRC_MOUSE_WHEEL, GamepadButton/GamepadAxis -> SRC_GAMEPAD_BUTTON/SRC_GAMEPAD_AXIS. *outScale and
 * *outComponent are OcInputBinding::scale/component, unchanged. 1 when `index` is in range, 0 (every
 * output left untouched) otherwise. */
AVER_FW_ABI int32_t     aver_fw_input_scheme_binding(int32_t index, int32_t* outActionIndex,
                                                     int32_t* outSource, float* outScale,
                                                     int32_t* outComponent);
/* Binding `index`'s key/button/axis NAME exactly as written in the file -- OcInputBinding::key.
 * Meaningful, and non-empty, for AVER_FW_ACTION_SRC_KEY/GAMEPAD_BUTTON/GAMEPAD_AXIS; "" for the
 * three mouse sources, matching OcInput.hpp's own convention. nullptr out of range. */
AVER_FW_ABI const char* aver_fw_input_scheme_binding_key(int32_t index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ABI_H */
