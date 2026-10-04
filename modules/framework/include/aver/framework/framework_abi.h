#ifndef AVER_FRAMEWORK_ABI_H
#define AVER_FRAMEWORK_ABI_H

/* Gameplay framework C ABI — GameInstance, GameMode, actors, pawns and controllers.
 *
 * A P/Invoke surface: nothing but int32_t/int64_t/float/const char* crosses it, no function
 * pointers, void*, structs or enums. Dispatch tables live in framework_hooks.h. Aver.Framework
 * links Aver.Scene, never the reverse. Error convention: 1 on success, 0 on a rejected request. */

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

/* ABI version, as (major << 16) | minor. Versioned independently of AVER_SCENE_ABI_VERSION.
 * Every minor bump below is additive only: a host built against an older minor still links and
 * runs unchanged against a newer header. */
#define AVER_FW_ABI_VERSION_MAJOR 1
/* 1: aver_fw_set_view_entity / aver_fw_view_entity.
 * 2: aver_fw_set_sky_clouds / aver_fw_sky_clouds / aver_fw_clear_sky_clouds.
 * 3: aver_fw_set_fluid_spawn_provider / aver_fw_fluid_spawn.
 * 4: aver_fw_set_fluid_spawn_material_provider / aver_fw_fluid_spawn_material -- density/
 *    calibrated-fit viscosity/presets layered on minor 3's four raw solver knobs.
 * 5: named-action layer (aver_fw_action_register/find/bind/clear_bindings/value2/held/pressed/
 *    released, see NAMED ACTIONS below); raw-VK twin (aver_fw_input_set_vk/vk/vk_pressed/
 *    vk_released) for VK codes AVER_FW_KEY_* can't grow to cover; gamepad ABI SHAPE, no polling yet.
 * 6: AVER_FW_ACTION_SRC_GAMEPAD_BUTTON / _AXIS, so aver_fw_action_bind can bind onto the gamepad
 *    ABI minor 5 only shaped -- polling now fills it (see GAMEPAD section below).
 * 7: INPUT SCHEME section (aver_fw_input_scheme_load/error/context_name/context_priority/
 *    action_count/action_name/action_type/binding_count/binding/binding_key) -- loads .ocinput,
 *    which nothing did before (see INPUT SCHEME below).
 * 8: HOST CONTROL section (aver_fw_set_quit_requested/aver_fw_quit_requested and
 *    aver_fw_cursor_request/aver_fw_cursor_release/aver_fw_cursor_requested) -- a shipped game had
 *    no way to exit itself or release the mouse cursor (see that section below).
 * 9: aver_fw_class_count (enumerate declared classes) and aver_fw_begin_play_with_pawn (a level's
 *    pawn override) -- for World Settings' game mode / default pawn pickers. */
#define AVER_FW_ABI_VERSION_MINOR 9
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
/* How many classes are declared; handles run 1..count. (minor 9) */
AVER_FW_ABI int32_t aver_fw_class_count(void);
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
/* Flattens the parent chain into the resolved archetype (0 on a cycle/undeclared parent); spawning auto-seals. */
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
 * setting it up. `reason` is an AVER_FW_BEGIN_* value (framework_hooks.h). WHY: aver_fw_spawn is
 * synchronous (bind/build_models/beginPlay run inline), so a save restore must patch saved fields
 * in BEFORE begin play, or OnBeginPlay reads class defaults instead. Pair with
 * aver_fw_spawn_preview. 0 for a stale handle, a non-actor entity, or no managed instance bound.
 * Calling it twice dispatches twice -- not remembered here. */
AVER_FW_ABI int32_t aver_fw_dispatch_begin_play(int32_t e, int32_t reason);

/* ---- SAVE/LOAD, RELAYED -----------------------------------------------------------------
 *
 * The framework does not link Aver.Save, for the same reason it does not link the animation
 * system: each sits at its own tier and only a composition root links both. The host installs a
 * provider and these forward -- the identical shape aver_fw_set_anim_curve_provider uses below.
 * No provider -> both return 0, what a game with no save system should report, not a crash. */
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
/* aver_fw_begin_play, spawning `pawnClass` instead of the GameMode's default pawn (0 = the default)
 * -- a level's pawn override. The GameMode class itself is left unchanged. (minor 9) */
AVER_FW_ABI int32_t aver_fw_begin_play_with_pawn(int32_t gameInstanceClass, int32_t gameModeClass,
                                                 int32_t pawnClass);
/* Ends the running session: OnEndPlay(STOP) and destroy every actor it spawned, back to EDITOR.
 * 0 if nothing was running. */
AVER_FW_ABI int32_t aver_fw_end_play(void);
/* Freezes (paused != 0) or resumes the tick without tearing the session down. 0 if not playing. */
AVER_FW_ABI int32_t aver_fw_set_paused(int32_t paused);
/* The first declared non-abstract class carrying ALL of `flags`, or 0. 0 flags -> 0. */
AVER_FW_ABI int32_t aver_fw_find_class_with_flags(int32_t flags);

/* ---- HOST CONTROL: QUIT AND CURSOR POLICY -----------------------------------------------------
 *
 * Both pairs plug a gap: a shipped game (GameApp.cpp) had no way to ask its host to exit (grepping
 * Quit/RequestExit/request_exit across scripting/csharp/Aver.* and every *_abi.h found nothing), or
 * to get the mouse cursor back once play starts. QUIT is a REQUEST the host polls once a frame, not
 * an immediate exit(): the setter can run deep inside a managed tick with a device/audio/physics
 * world live underneath, so tearing the process down there would bypass GameApp's ordered
 * shutdown -- the main loop decides when to act. DEFAULT: reads 0 until set(1). Plain set/get, not
 * one-shot (same precedent as aver_fw_set_paused/aver_fw_play_state above): Play-In-Editor runs
 * several sessions per process and must clear the flag itself (aver_fw_begin_play does NOT), so a
 * quit near one session's end can't re-fire in the next.
 *
 * CURSOR POLICY: wantCapture (GameApp.cpp) derives capture PURELY from aver_fw_play_state(), so a
 * pause menu or dialogue box can't show a cursor without ending play. NOT a bare
 * SetCursorVisible(bool): a dialogue box over a still-open pause menu closing and setting "false"
 * would vanish the cursor out from under it -- a shared bool has no memory of other holders.
 * A REQUEST COUNT instead (Win32 ShowCursor's shape): request()/release() inc/dec (floored at 0, so
 * a stray extra release can't go negative and flip the sign for every caller after it), requested()
 * is "count > 0" -- callers nest/overlap in any order and the cursor stays visible as long as the
 * highest count says it should. DEFAULT: starts at 0, today's PLAYING-only rule; combining the two
 * is the HOST's job. */
AVER_FW_ABI void    aver_fw_set_quit_requested(int32_t requested);
/* 1 once set_quit_requested(1) has been called and not since cleared with (0); 0 otherwise -- the
 * "never quits on its own" default the section above promises. */
AVER_FW_ABI int32_t aver_fw_quit_requested(void);

/* Increments the process-wide cursor-visibility request count and returns it (always >= 1); call
 * when something starts wanting the cursor free. */
AVER_FW_ABI int32_t aver_fw_cursor_request(void);
/* Decrements the count (floored at 0) and returns it; call once per matching
 * aver_fw_cursor_request(), per the section above. */
AVER_FW_ABI int32_t aver_fw_cursor_release(void);
/* 1 while the count is > 0, 0 at the default -- nothing has asked, so capture follows play state
 * alone (today's rule). */
AVER_FW_ABI int32_t aver_fw_cursor_requested(void);

/* ---- ANIMATION CURVES, RELAYED --------------------------------------------------------------
 *
 * The framework does not link the animation module. It holds a function pointer the composition
 * root installs and forwards -- the same shape AnimSystem uses for asset resolution and notifies,
 * so C# can ask "what does this curve read" through the library it already binds. No provider ->
 * aver_fw_anim_curve returns 0 for everything, the same answer a clip with no such curve gives. */
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
 * The framework does not link the graph module, same reason as save/animation above. A host
 * installs a provider and these forward. WHY: a save must capture/restore graph-local VAR storage
 * (Aver.Graph's GraphVarStore), pure managed state with no native-scene representation
 * (SaveWorld.hpp) -- this is the seam that lets a save reach it without either module gaining an
 * edge it must not have. COUNT THEN INDEX, not a bulk call: everything else in this file is
 * scalars/const char* -- a struct or array-of-structs here would be the first of either, not
 * worth it for a handful of calls per entity during save/load. No provider -> everything returns 0. */
typedef int32_t (AVER_FW_CALL* aver_fw_graph_var_count_fn)(int32_t entity, void* user);
/* Fills the name (nameBuf, nameBufLen bytes, UTF8, NUL-terminated even if truncated), kind
 * (AVER_SCENE_KIND_F32/I32/BOOL, scene_abi.h; a VAR is never any other kind) and value (outF for
 * F32, outI for I32/BOOL -- same "kind decides the member" convention as aver::fmt::OcSaveField)
 * of VAR `index`, 0..count-1, DECLARATION order. */
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
 * The framework does not link Aver.Synapse; a host installs a provider and this forwards. SYNAPSE
 * ADVISES, IT DOES NOT MOVE (SynapseAgent.hpp) -- AgentSystem writes the agent's waypoint onto its
 * own CSynapseAgent component, and this is the seam that lets a graph's GetSynapseTarget node read
 * it back: that component's fields ARE registered via World::registerComponent, but that
 * reflection is C++-only with no lookup BY NAME, and a managed caller only has strings. No
 * provider -> returns 0 for everything, same as a clip with no curve or a graph with no VAR. */
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
 * Same shape as the steering-target relay above: Aver.Synapse.Scene must not link Aver.Framework.
 * UNLIKE aver_fw_synapse_target, 0/false here does NOT mean "nothing to report" -- "cannot
 * currently see the target" is a real, meaningful answer, not an absence. The RETURN VALUE
 * distinguishes the two: 0 only when `e` has no CSynapsePerception (or no provider); 1 whenever it
 * does, with *outCanSee telling the caller which case applies -- the same "false means absent, not
 * zero" discipline as aver_fw_anim_curve, one level up. */
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
/* key/_pressed/_released: 1 while held, 1 on the frame it went down, 1 on the frame it went up. */
AVER_FW_ABI int32_t aver_fw_input_key(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_pressed(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_released(int32_t key);
/* Writes {dx, dy, wheel} into out3. */
AVER_FW_ABI void    aver_fw_input_mouse(float* out3);

/* ---- NAMED ACTIONS (Enhanced Input), PORTED FROM Aver.Framework's EnhancedInput.cs ---------------
 *
 * EnhancedInput.cs is a complete named-action layer (InputAction Digital/Axis1D/Axis2D,
 * InputBinding, priority-stacked InputMappingContext, per-layer key consumption, 0.15 dead zone)
 * with zero consumers and zero tests -- pure C# unreachable from a graph node or C++. Its ALGORITHM
 * is ported verbatim; only the unreachable PLACEMENT is fixed, and the dead zone stays PINNED, not a
 * parameter, since the algorithm isn't configurable either.
 *
 * NO CONTEXT HANDLE: EnhancedInput.cs pushes whole InputMappingContext objects onto a priority
 * stack; here `contextPriority` on EVERY binding stands in for that object -- two bindings sharing
 * a number ARE one context for consumption ("two bindings in one context can share a key" --
 * Update()). No partial "pop one context" either -- aver_fw_action_clear_bindings drops everything
 * and the caller re-binds. NO PER-FRAME "Update()": held/pressed/released/value2 evaluate ON
 * DEMAND, reading the SAME InputState cur/prev (+mouse/prevMouse) arrays aver_fw_input_key already
 * reads, and for the two GAMEPAD_* sources the same GamepadState buttons/axes
 * (+prevButtons/prevAxes) (FrameworkAbi.cpp) -- an action on AVER_FW_KEY_W and a script reading it
 * directly must never disagree, which only holds if both read identical bytes; a mouse/gamepad
 * action also needs a "was this active last frame" edge answer a keyboard binding gets free from
 * cur/prev, which is why aver_fw_input_new_frame also rolls a `prevMouse` snapshot (and, since
 * minor 6, GamepadState prevButtons/prevAxes). */
#define AVER_FW_ACTION_DIGITAL 0
#define AVER_FW_ACTION_AXIS1D  1
#define AVER_FW_ACTION_AXIS2D  2

/* Where one binding reads from -- pinned to EnhancedInput.cs's InputSource enum. SRC_KEY reads an
 * AVER_FW_KEY_* slot (never a raw VK -- see RAW WIN32 VK below); the three MOUSE_* sources read
 * whatever aver_fw_input_set_mouse published this frame (dx, dy, wheel); GAMEPAD_BUTTON/AXIS read
 * the GAMEPAD section's pad-0 state -- see that section and aver_fw_action_bind below for what
 * `key` carries for each. */
#define AVER_FW_ACTION_SRC_KEY            0
#define AVER_FW_ACTION_SRC_MOUSE_X        1
#define AVER_FW_ACTION_SRC_MOUSE_Y        2
#define AVER_FW_ACTION_SRC_MOUSE_WHEEL    3
#define AVER_FW_ACTION_SRC_GAMEPAD_BUTTON 4
#define AVER_FW_ACTION_SRC_GAMEPAD_AXIS   5

/* Declares a named action, or returns the existing handle for `name` unchanged -- IDEMPOTENT BY
 * NAME (same convention as aver_fw_class_declare): a script's OnBeginPlay re-registers "Jump" on
 * every possession and must get back the ORIGINAL action, not a duplicate. 0 for a null/empty
 * name or a valueType outside AVER_FW_ACTION_DIGITAL..AVER_FW_ACTION_AXIS2D. */
AVER_FW_ABI int32_t aver_fw_action_register(const char* name, int32_t valueType);
/* The handle for a previously registered action name, or 0. */
AVER_FW_ABI int32_t aver_fw_action_find(const char* name);
/* Adds one binding to `action`. `key` is an AVER_FW_KEY_* slot for SRC_KEY, an AVER_FW_GAMEPAD_*
 * button for SRC_GAMEPAD_BUTTON, an AVER_FW_GAMEPAD_AXIS_* axis for SRC_GAMEPAD_AXIS, ignored for
 * the three MOUSE_* sources (ported placeholder from BindMouseLook/BindMouseWheel). `scale`
 * multiplies the source value before it accumulates -- BindAxis1D's trick for turning two opposed
 * keys into one -1..1 axis: +1 on one key, -1 on the other, same component. `component` selects
 * the channel: 0=X, 1=Y, anything else=Z (Z ported from EnhancedInput.cs's Accumulate() third case;
 * nothing binds it here and aver_fw_action_value2 doesn't read it back). `contextPriority`: a
 * STRICTLY higher-priority binding on the SAME key blocks this one, decided by priority, not
 * declaration order -- KEY bindings only; a gamepad button is never consumed this way (see
 * actionKeyConsumedByHigherPriority in FrameworkAbi.cpp), so two contexts on the same button both
 * see it. Silently ignored for an invalid action, out-of-range source, or key/button/axis (same
 * out-of-range convention as aver_fw_input_set_key). */
AVER_FW_ABI void    aver_fw_action_bind(int32_t action, int32_t source, int32_t key, float scale,
                                        int32_t component, int32_t contextPriority);
/* Drops every binding on every action; registrations (and handles) survive -- only the key-to-
 * action map is cleared. Re-establish a set of contexts by calling this once, then
 * aver_fw_action_bind again -- the same net effect as ClearContexts()+fresh AddContext calls. */
AVER_FW_ABI void    aver_fw_action_clear_bindings(void);
/* Writes {X, Y} of the action's accumulated value into out2 (Value2D, no Z channel). Applies NO
 * dead zone -- that only gates held/pressed/released below. {0, 0} for an invalid handle. */
AVER_FW_ABI void    aver_fw_action_value2(int32_t action, float* out2);
/* held/pressed/released: 1 while active (magnitude exceeds the 0.15 dead zone) / on the frame it
 * became active / on the frame it stopped. 0 for an invalid handle. */
AVER_FW_ABI int32_t aver_fw_action_held(int32_t action);
AVER_FW_ABI int32_t aver_fw_action_pressed(int32_t action);
AVER_FW_ABI int32_t aver_fw_action_released(int32_t action);

/* ---- RAW WIN32 VK, ADDITIVE TWIN TO AVER_FW_KEY_* -------------------------------------------------
 *
 * frameworkKeyFromVk (InputKeys.hpp) maps Win32 VKs onto AVER_FW_KEY_* above; most of the VK range
 * falls through to -1: the enum has only AVER_FW_KEY_COUNT (50) slots against Win32's 256, so
 * F-keys/numpad/OEM keys are unreachable today, and it can't just grow to cover them -- a saved
 * .ocgraph's InputKey node stores a literal int, so inserting a name anywhere but the tail would
 * silently repoint every saved graph at the WRONG key, with no error at load. Escape hatch: a
 * second, parallel cur/prev array indexed by the RAW vk code (0..255, matching
 * aver::platform::InputState::kKeyCount), never subject to the renumbering constraint -- F5 is
 * asked for as vk 0x74 by value, not through a name that could move. */
/* The Win32 VK range this twin covers, 0..(AVER_FW_VK_COUNT-1) -- Win32's own VK codes are 0..255. */
#define AVER_FW_VK_COUNT 256
/* Sets the held state of a raw Win32 VK. Out-of-range (outside 0..255) is ignored, matching
 * aver_fw_input_set_key's own convention. Independent of aver_fw_input_set_key -- setting the named
 * AVER_FW_KEY_A slot does NOT also set raw vk 'A', and vice versa; they are two separate arrays that
 * happen to be fed the same physical key by whichever host publishes both. */
AVER_FW_ABI void    aver_fw_input_set_vk(int32_t vk, int32_t down);
/* vk/_pressed/_released: 1 while held, 1 on the frame it went down, 1 on the frame it went up. */
AVER_FW_ABI int32_t aver_fw_input_vk(int32_t vk);
AVER_FW_ABI int32_t aver_fw_input_vk_pressed(int32_t vk);
AVER_FW_ABI int32_t aver_fw_input_vk_released(int32_t vk);

/* ---- GAMEPAD, STATE IN / STATE OUT -------------------------------------------------------------
 *
 * Like every other aver_fw_input_* pair above, this ABI stops at "state in, state out": no device
 * open, no hotplug detection, no dead zone, no rumble (each its own scope, better owned by whoever
 * polls the real device -- XInput hotplug leaves stale data rather than an error, and its per-stick
 * dead-zone guidance differs in shape from the flat 0.15 the action layer uses). POLLING:
 * platform/Gamepad.hpp (pollGamepads) opens the device; Runtime/src/GameInput.cpp's publishGamepad
 * -- the ONE place in the tree reaching this section, via the editor's own publishInput policy;
 * sandbox/ makes no such call itself -- calls aver_fw_input_set_gamepad_button/axis once a frame.
 * Shipped SHAPE ONLY at minor 5 (no polling); minor 6 is its first consumer --
 * aver_fw_action_bind's GAMEPAD_BUTTON/AXIS sources read pad 0's state back through here. Buttons/
 * axes are modelled on XInput's XINPUT_GAMEPAD_* bitmask and XINPUT_STATE thumbstick/trigger
 * fields (14 buttons, 6 axes) so a provider is a mechanical unpack. `pad` is fixed at 0, the same
 * "only player 0 until split-screen" precedent as aver_fw_player_controller. */
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
 * host keeps whatever the level authored -- a project with no sky script renders as before.
 *
 * Lengths are CENTIMETRES, wind is centimetres/second (as elsewhere in this engine); featureScale
 * is 1 / the width of one noise feature in world units. */
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
 * The framework does not link Aver.Fluids (FluidVolume.hpp: "this module must never learn what
 * Jolt is") -- same provider-relay shape as SAVE/LOAD and ANIMATION CURVES above. WHY: a level's
 * WATER record and a graph's `COMP <id> Fluid ...` component both need to hand a
 * fluids::FluidVolumeDesc to fluids::FluidScene::spawn; this is the seam that lets either cross
 * without linking Aver.Fluids/Aver.Physics directly. `subdivisions` is deliberately not a
 * parameter -- every request gets FluidVolumeDesc's own default. QUEUED, NOT SYNCHRONOUS: the
 * return value says a provider accepted the request, not that a volume simulates -- FluidScene is
 * not ready() until after render features come up, and this can be reached earlier (a level-load
 * actor, OnBeginPlay); a provider should queue and drain it at the same frame-safe point as a
 * level's own WATER record, not spawn from inside the callback -- whether it spawned shows in the
 * host's log. No provider -> returns 0. */
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
 * A SECOND, ADDITIVE relay beside aver_fw_fluid_spawn above, not a replacement (minor 4). Same
 * six placement floats and four raw solver knobs PLUS the material layer
 * (fluids::FluidPhysicsMaterial): `densityKgM3`/`viscosityPaS` (< 0 = "not given", same sentinel
 * as `pressure`), or `materialPreset` ("water"/"lightoil"/"honey"/"lava", case-insensitive; empty =
 * none), resolved by the provider, which carries only a name, never duplicated literal numbers.
 * `damping` CAN STILL CONFLICT WITH A MATERIAL, DELIBERATELY: this relay forwards both untouched; the refusal
 * happens once, downstream, at fluids::FluidScene::spawn. */
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
 * OcInput.hpp defines the .ocinput format (named input actions + default bindings); nothing
 * loaded one until now. This exposes the existing parser (aver::fmt::parseOcinput, via
 * loadOcinput) so InputScheme.cs can turn a parsed file into an InputMappingContext with no
 * second parser managed-side -- same division of labour as the graph (parseOcgraph, via
 * DeclareGraphClasses) and the NAMED ACTIONS layer above. ONE PARSED SCHEME, NOT A HANDLE TABLE: a
 * project names exactly one scheme (OcProject.hpp's INPUT.SCHEME), so this ABI holds one
 * file-static aver::fmt::OcInputData; a second load REPLACES the slot outright on either outcome --
 * no "keep the old one on parse failure". COUNT THEN INDEX, same convention as
 * aver_fw_graph_var_count/_at: no struct or array-of-structs crossing this boundary, and a handful
 * of calls per load is cheap. RETURNED STRINGS STAY VALID UNTIL THE NEXT
 * aver_fw_input_scheme_load (wider than every other const char* here, valid only until the next
 * call, and still never to be freed by the caller) -- callers hold names across several calls while
 * walking a whole scheme. */

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
 * index INTO aver_fw_input_scheme_action_name/_type -- NOT an aver_fw_action_register handle; the
 * caller resolves that itself. *outSource is an AVER_FW_ACTION_SRC_* value, OcInputSource mapped
 * one for one (Key->SRC_KEY, MouseX/Y->SRC_MOUSE_X/Y, MouseWheel->SRC_MOUSE_WHEEL,
 * GamepadButton/Axis->SRC_GAMEPAD_BUTTON/AXIS). *outScale/*outComponent are
 * OcInputBinding::scale/component, unchanged. 1 in range, 0 (outputs untouched) otherwise. */
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
