#ifndef AVER_FRAMEWORK_ABI_H
#define AVER_FRAMEWORK_ABI_H

/* Gameplay framework C ABI — GameInstance, GameMode, actors, pawns and controllers.
 *
 * Same plain-C idiom as scene_abi.h and pbr_abi.h, and under the same two extra rules: nothing but
 * int32_t / int64_t / float / const char* crosses this boundary, and there are no function
 * pointers, void*, structs or enums in it. The dispatch tables the framework calls THROUGH live in
 * framework_hooks.h, which is not a P/Invoke surface and which no managed code ever marshals.
 *
 * WHY THIS IS A SEPARATE MODULE FROM Aver.Scene, and not simply more of it:
 *
 * modules/scene/README.md commits the world to being "render/physics-agnostic (no UObject)". A
 * gameplay framework is precisely the vocabulary that constraint excludes — actor, pawn, possess,
 * begin play. Keeping it in a second module means `scene_abi.h` can be read end to end without
 * meeting any of those words, and "use Aver.Scene without the framework" is a question answered by
 * a link line rather than by discipline.
 *
 * The arrow points DOWN: Aver.Framework links Aver.Scene, never the reverse. Gameplay stands on
 * storage. The framework therefore sweeps its instance lists with the scene's own validity check
 * rather than asking the scene for a destroy callback, because a callback would be an edge pointing
 * back up.
 *
 * NOTE ON THE BUILD SHAPE: this is a SHARED library that links another SHARED library, which no
 * other module in this tree does. The rule that shape is tested against is the one
 * modules/render.pbr/CMakeLists.txt states — no RHI type may sit behind a P/Invoke DLL. The full
 * transitive closure here is {Core, Assets, Scene}, with no RHI anywhere behind it, so the property
 * the rule protects holds. Collapsing the two into one DLL would satisfy the letter of "depends on
 * Core only" and destroy the separation above.
 *
 * Error convention, matching pbr_abi.h: 1 on success, 0 on a rejected request.
 */

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

/* ABI version, as (major << 16) | minor — same contract as AVER_SCENE_ABI_VERSION, and versioned
 * INDEPENDENTLY of it. The framework's surface will move while the scene's is still settling, and a
 * single shared number would force a lockstep neither module needs. */
#define AVER_FW_ABI_VERSION_MAJOR 1
/* 1: added aver_fw_set_view_entity / aver_fw_view_entity. Additive only -- every entry point that
 * existed at 1.0 is unchanged in shape and meaning, so a binding built against 1.0 still runs. */
#define AVER_FW_ABI_VERSION_MINOR 1
#define AVER_FW_ABI_VERSION \
    ((AVER_FW_ABI_VERSION_MAJOR << 16) | AVER_FW_ABI_VERSION_MINOR)

AVER_FW_ABI int32_t aver_fw_abi_version(void);

/* The version of Aver.Scene this framework binary was BUILT against.
 *
 * Reported separately from aver_fw_abi_version because the two DLLs ship as separate files and can
 * be replaced independently. A framework built against scene major 1 loaded beside a scene major 2
 * is a mismatch the loader will not catch — the import lib resolves by NAME, and every name still
 * exists. Comparing this against aver_scene_abi_version() at bootstrap turns that into a message. */
AVER_FW_ABI int32_t aver_fw_scene_abi_version(void);

/* 1 when the Aver.Scene DLL actually LOADED reports the same major as this binary compiled against,
 * 0 when it does not. Unlike the two functions above it calls across the DLL boundary, which is the
 * only way to learn what is really loaded rather than what a header said at compile time.
 *
 * Keep it that way. If this ever stops calling into Aver.Scene the check silently becomes a
 * tautology, and — as the first build of this module demonstrated — the framework then imports
 * nothing from the scene at all and `dumpbin /dependents` shows no edge between them. */
AVER_FW_ABI int32_t aver_fw_scene_abi_matches(void);

/* ================================================================================================
 * STEP 8 — THE GAMEPLAY ABI: class registry, class defaults, spawn, class identity, possession.
 *
 * A CLASS IS DATA. There is no C++ base type behind a class handle and no virtual dispatch behind a
 * spawn: a class is a row in a registry holding a flattened component list and one contiguous blob of
 * default values, and spawning is a loop of memcpy over that blob into the scene's pools.
 *
 * Every handle — class, entity, component, field — crosses as int32_t and 0 == invalid, matching the
 * scene ABI. Inbound strings are UTF-8 (the one outbound string, aver_fw_class_name, is a UTF-8 char*
 * the caller decodes and must NOT free). Setters return 1 on success / 0 on a rejected request.
 *
 * These typedefs are documentation only — the exported signatures use int32_t so the C# P/Invokes,
 * which declare everything as `int`, bind by name with no marshalling surprises.
 * ============================================================================================== */
typedef int32_t aver_class;    /* a registry row; 0 invalid */
typedef int32_t aver_entity;   /* a scene entity; 0 invalid (== aver::scene::Entity across the ABI) */
typedef int32_t aver_field;    /* a dense scene field id; carries its component AND its kind */

/* Class flags — pinned to Aver.Framework's ClassFlags (Enums.cs). A script never sets these; the base
 * type and the builder decide them. The PAWN / CONTROLLER pair IS the whole of possess type-safety. */
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

/* ---- class registry ----
 * declare() is IDEMPOTENT BY NAME: the same name returns the same handle for the life of the process,
 * and that stable handle is the whole of hot-reload identity — a rebuilt assembly redeclares its class,
 * gets back the handle its live entities already store, and only the descriptor behind it is rewritten
 * (a redeclare resets the row's components and defaults in place). */
AVER_FW_ABI int32_t aver_fw_class_declare(const char* name, const char* parentName);
AVER_FW_ABI int32_t aver_fw_class_find(const char* name);           /* 0 when unknown */
AVER_FW_ABI const char* aver_fw_class_name(int32_t c);              /* "" for an invalid handle */
AVER_FW_ABI int32_t aver_fw_class_parent(int32_t c);               /* parent handle, 0 for a root */
AVER_FW_ABI int32_t aver_fw_class_reset(int32_t c);                /* clear components + defaults */
AVER_FW_ABI int32_t aver_fw_class_add_component(int32_t c, int32_t component);
AVER_FW_ABI int32_t aver_fw_class_set_flags(int32_t c, int32_t flags);
AVER_FW_ABI int32_t aver_fw_class_get_flags(int32_t c);
AVER_FW_ABI int32_t aver_fw_class_set_tick(int32_t c, int32_t tickGroup, int32_t tickOrder);
/* Flatten the parent chain into the resolved archetype: the union of components and the resolved
 * default bytes. 0 on a cycle in the parent chain or a named-but-undeclared parent. Spawning
 * auto-seals, so a caller that forgets is slow once, not wrong. */
AVER_FW_ABI int32_t aver_fw_class_seal(int32_t c);

/* ---- class defaults, addressed by the SAME dense field id the scene resolves ----
 * FIVE setters, one per storable kind. No set_default_bool (a bool rides i32) and no set_default_ref
 * (an entity default is meaningless in an archetype — it is per-instance). The kind is validated here:
 * a wrong-kind default is rejected with 0 and stored nowhere. A default lands in the class row's
 * archetype blob at the field's offset — it is NOT written to any live entity. */
AVER_FW_ABI int32_t aver_fw_class_set_default_f32(int32_t c, int32_t f, float v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i32(int32_t c, int32_t f, int32_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i64(int32_t c, int32_t f, int64_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_vec(int32_t c, int32_t f, const float* v);
AVER_FW_ABI int32_t aver_fw_class_set_default_str(int32_t c, int32_t f, const char* v);

/* ---- GameMode wiring, resolved by class NAME at seal so two game classes never take a compile-time
 *      reference to one another. ---- */
AVER_FW_ABI int32_t aver_fw_class_set_default_pawn(int32_t gameMode, const char* pawnClassName);
AVER_FW_ABI int32_t aver_fw_class_set_player_controller(int32_t gameMode, const char* controllerClassName);

/* ---- actors ----
 * spawn creates a world entity, attaches every component in the sealed archetype and memcpys the class
 * defaults into each, records the entity's CLASS (so class_of works), applies the optional name and the
 * optional transform overrides. Rotation crosses as a QUATERNION (quat4) though the author writes
 * degrees higher up. A null pos/quat/scale means "use the class default". Returns the entity, 0 on
 * failure. */
AVER_FW_ABI int32_t aver_fw_spawn(int32_t c, const char* name,
                                  const float* pos3, const float* quat4, const float* scale3);
AVER_FW_ABI int32_t aver_fw_destroy(int32_t e);

/* Spawn for a PREVIEW: bind and build_models, and STOP. OnBeginPlay is not dispatched.
 *
 * The distinction is the whole point and it is not a convenience. A normal spawn runs
 * bind -> build_models -> beginPlay, and OnBeginPlay is where a game does things: SkyForge's game
 * mode spawns five targets and six crates there, its target adds a physics body, its character
 * pushes an input context. An editor that spawned an actor to look at it would run all of that, into
 * the live world, every time somebody opened a tab.
 *
 * build_models is the part a preview wants -- it is this engine's construction script, the code that
 * says what the actor is MADE of -- and it is the part with no side effects outside the actor's own
 * child entities. UE draws the same line: its blueprint viewport runs the construction script and
 * does not run BeginPlay.
 *
 * Destroy with aver_fw_destroy_preview, never aver_fw_destroy: dispatching OnEndPlay to an instance
 * that never had OnBeginPlay is the same error in the other direction. */
AVER_FW_ABI int32_t aver_fw_spawn_preview(int32_t c, const char* name,
                                          const float* pos3, const float* quat4, const float* scale3);
AVER_FW_ABI int32_t aver_fw_destroy_preview(int32_t e);
AVER_FW_ABI int32_t aver_fw_class_of(int32_t e);   /* the entity's class, or 0 — != 0 IS "actor" */

/* ---- possession — flag-gated ----
 * Rejected unless the controller's class carries CONTROLLER and the pawn's class carries PAWN. That
 * flag check is the whole of the type safety here, which is why the base types set the flags for you. */
AVER_FW_ABI int32_t aver_fw_possess(int32_t controller, int32_t pawn);
AVER_FW_ABI int32_t aver_fw_unpossess(int32_t controller);
AVER_FW_ABI int32_t aver_fw_controlled_pawn(int32_t controller);   /* the pawn, or 0 */
AVER_FW_ABI int32_t aver_fw_controller_of(int32_t pawn);           /* the controller, or 0 */

/* ---- play lifecycle + session singletons (step 13) ----
 * The world has two lives: EDITOR authoring and PLAYING. aver_fw_begin_play spawns the session — an
 * optional GameInstance, the GameMode, and the GameMode's controller+pawn (possessed) — and moves the
 * play state to PLAYING; aver_fw_end_play tears that session down (OnEndPlay reason STOP) and returns to
 * EDITOR. PAUSED freezes the tick without tearing anything down. The singletons below read back what
 * begin_play spawned; each is 0 in EDITOR. */
#define AVER_FW_PLAY_EDITOR  0
#define AVER_FW_PLAY_PLAYING 1
#define AVER_FW_PLAY_PAUSED  2

/* Begin a play session. gameModeClass is mandatory (0 -> reject); gameInstanceClass is optional (pass 0
 * for none). Returns 1 on a session that started, 0 if one was already running or the GameMode was
 * invalid. The GameMode's pawn/controller (named on the class, resolved at seal) are spawned and
 * possessed; a GameMode may legally have neither. */
AVER_FW_ABI int32_t aver_fw_begin_play(int32_t gameInstanceClass, int32_t gameModeClass);
/* End the running session: OnEndPlay(STOP) + destroy every actor begin_play spawned, back to EDITOR.
 * Returns 0 if nothing was running. */
AVER_FW_ABI int32_t aver_fw_end_play(void);
/* Freeze (paused != 0) or resume (0) the tick without tearing the session down. 0 if not playing. */
AVER_FW_ABI int32_t aver_fw_set_paused(int32_t paused);
/* The first declared class carrying ALL of `flags` (AVER_FW_CLASS_*), or 0. The editor's Play button
 * uses it to find the GameMode/GameInstance to start without a hard-coded class name. 0 flags -> 0. */
AVER_FW_ABI int32_t aver_fw_find_class_with_flags(int32_t flags);

/* The session singletons begin_play populated. Each is 0 in EDITOR. player_controller takes a 0-based
 * index; only player 0 exists until split-screen does. */
AVER_FW_ABI int32_t aver_fw_game_instance(void);
AVER_FW_ABI int32_t aver_fw_game_mode(void);
AVER_FW_ABI int32_t aver_fw_player_controller(int32_t playerIndex);
AVER_FW_ABI int32_t aver_fw_play_state(void);

/* ---- input ------------------------------------------------------------------------------------
 * The framework holds no window, so the APP owns raw input: each frame it calls aver_fw_input_new_frame
 * (which rolls the current key state into the previous, giving edge detection), maps its platform/ImGui
 * keys onto the stable codes below and calls set_key/set_mouse. Gameplay then reads key()/key_pressed()/
 * key_released()/mouse() from C# (the Input class). Keeping the codes here — not in the app — is what lets
 * a script name a key without depending on the editor. */
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
/* Roll current->previous. Call ONCE per frame, before the set_key calls, so pressed/released are edges. */
AVER_FW_ABI void    aver_fw_input_new_frame(void);
/* Set the held state of a key (0..AVER_FW_KEY_COUNT-1). Out-of-range keys are ignored. */
AVER_FW_ABI void    aver_fw_input_set_key(int32_t key, int32_t down);
/* Set this frame's mouse delta (dx, dy, in pixels) and wheel notches. */
AVER_FW_ABI void    aver_fw_input_set_mouse(float dx, float dy, float wheel);
/* Read: held now / went down this frame / went up this frame. 0 for an out-of-range key. */
AVER_FW_ABI int32_t aver_fw_input_key(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_pressed(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_released(int32_t key);
/* Write {dx, dy, wheel} into out3. */
AVER_FW_ABI void    aver_fw_input_mouse(float* out3);

/* ---- play view -------------------------------------------------------------------------------
 * A possessed character PUBLISHES the camera it wants (first- vs third-person, and the eye/boom offsets)
 * so the editor's play camera can follow it without the C++ side reaching into a C# field. The framework
 * only stores the request; the editor reads it each frame and positions the view from the possessed pawn's
 * transform. One request (one local player) until split-screen exists. */
#define AVER_FW_VIEW_FIRST_PERSON 0
#define AVER_FW_VIEW_THIRD_PERSON 1
AVER_FW_ABI void aver_fw_set_view(int32_t mode, float eyeHeight, float boomLength);
AVER_FW_ABI void aver_fw_view(int32_t* outMode, float* outEyeHeight, float* outBoomLength);

/* THE VIEW ENTITY: the scene node the camera sits on, published so the editor can READ a transform
 * instead of RECONSTRUCTING one.
 *
 * Reconstructing it is what this replaces, and it had a real cost. The editor used to take the
 * possessed pawn's world matrix and use its forward axis as the look direction, which meant the only
 * way for a character to aim up or down was to pitch its whole body -- about its origin, which is
 * the feet. Anything parented to the character then swung on an arc of its own height: the SkyForge
 * gun, carried 154cm up, travelled 55cm through the world for 20 degrees of look, while the camera
 * (pinned at feet + eyeHeight along WORLD up) did not move at all. Two different pivots for one
 * head. Hanging the camera and the held item on the same node makes that class of bug unstateable.
 *
 * 0 means "no view node published" and the caller falls back to the pawn-matrix path above, so a
 * character that never sets one behaves exactly as it did before.
 *
 * The handle is a scene entity id, valid only while the scene says it is -- check aver_scene_valid
 * before use; a pawn can be destroyed between the publish and the read. */
AVER_FW_ABI void    aver_fw_set_view_entity(int32_t entity);
AVER_FW_ABI int32_t aver_fw_view_entity(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ABI_H */
