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

/* ABI version, as (major << 16) | minor. Versioned independently of AVER_SCENE_ABI_VERSION. */
#define AVER_FW_ABI_VERSION_MAJOR 1
/* 1: added aver_fw_set_view_entity / aver_fw_view_entity. Additive only.
 * 2: added aver_fw_set_sky_clouds / aver_fw_sky_clouds / aver_fw_clear_sky_clouds. Additive only,
 *    so a host built against minor 1 links and runs unchanged against this header. */
#define AVER_FW_ABI_VERSION_MINOR 2
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
/* AVER_FW_CALL is defined by framework_hooks.h, which owns every other callback typedef in this
 * ABI. Defined here too rather than including that header, because this one is the SURFACE C#
 * binds and pulling in the dispatch-table header for one macro would widen it for no reason. */
#if defined(_WIN32) && !defined(AVER_FW_CALL)
#  define AVER_FW_CALL __cdecl
#elif !defined(AVER_FW_CALL)
#  define AVER_FW_CALL
#endif
typedef int32_t (AVER_FW_CALL* aver_fw_anim_curve_fn)(int32_t entity, int64_t nameHash,
                                                      float* outValue, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_anim_curve_provider(aver_fw_anim_curve_fn fn, void* user);
/* Writes the curve value and returns 1, or leaves *outValue alone and returns 0 when there is no
 * provider, no clip on the entity, or no curve of that name. THE CALLER MUST DISTINGUISH THOSE
 * FROM A VALUE OF ZERO: a curve that reads 0 and a curve that is not there mean opposite things. */
AVER_FW_ABI int32_t aver_fw_anim_curve(int32_t entity, int64_t nameHash, float* outValue);

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ABI_H */
