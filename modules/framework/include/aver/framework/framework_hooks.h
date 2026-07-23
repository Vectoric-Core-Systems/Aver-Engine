#ifndef AVER_FRAMEWORK_HOOKS_H
#define AVER_FRAMEWORK_HOOKS_H

/* Aver.Framework dispatch tables — the surface the framework calls THROUGH into gameplay code.
 *
 * This is the OTHER half of the framework's C surface, split off from framework_abi.h on purpose:
 *
 *   - framework_abi.h is a P/Invoke surface. Nothing but int32_t / int64_t / float / const char*
 *     crosses it, so a C# [DllImport] binds every entry point by name with no marshalling.
 *
 *   - framework_hooks.h (this file) is NOT a P/Invoke marshalling surface. It holds C FUNCTION
 *     POINTERS and the structs that carry them — the dispatch tables that gameplay code INSTALLS so
 *     the framework can call up into it. No managed code ever marshals this struct field by field;
 *     the bridge builds ONE table of native thunks and installs it once (see the by-value note on
 *     AvManagedDispatch below).
 *
 * The struct idiom is scripting_abi.h's AverScriptHostApi, NOT pbr_abi.h's: the first two fields are
 * int32_t structBytes then int32_t contractVersion, and install refuses a table whose size or version
 * does not match what this binary was compiled against. That check is the whole of the "a stale bridge
 * next to a newer framework is reported, not crashed through" guarantee, exactly as the scripting
 * bootstrap does it one module over.
 *
 * SCOPE (step 10): the managed-dispatch table + install/clear + the framework routing a managed actor's
 * begin/tick/end lifecycle THROUGH it. The C# bridge that FILLS this table and the per-class native
 * vtable tick loop are later stages; where they are referenced here it is in a comment, not in code.
 */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, aver_entity, aver_class */

#include <stdint.h>

/* Calling convention for every pointer in these tables. The bridge compiles its thunks __cdecl (its
 * [UnmanagedCallersOnly] entries are CallConvCdecl), so the native pointer types must say so too, or a
 * mismatch would corrupt the stack on the first call. Guarded because __cdecl is a Windows spelling and
 * this header, unlike scripting_abi.h, is built on non-Windows too (framework_abi.h has the #else). */
#if defined(_WIN32)
#  define AVER_FW_CALL __cdecl
#else
#  define AVER_FW_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================================
 * WHY A LIFECYCLE HOOK FIRED — BeginReason / EndReason.
 *
 * Passed to the hook, never inferred by the script: a script that must tell a fresh spawn from a hot
 * reload can only do so if it is TOLD, and every engine that omitted this grew a bool later — and a
 * bool is an ABI change. These pin, integer for integer, to Aver.Framework's C# enums
 * (Enums.cs: BeginReason / EndReason); this is the native side of that pinning the C# header promised
 * would "land when the lifecycle entry points stop being stubs".
 *
 * NOTE the asymmetry: BeginReason has THREE values, EndReason has FOUR — the extra is Travel (level
 * travel), which has no Begin counterpart because the arriving level's actors spawn fresh (SPAWN/PLAY).
 * ============================================================================================== */
#define AVER_FW_BEGIN_SPAWN   0   /* a brand-new actor was spawned into a live world              */
#define AVER_FW_BEGIN_PLAY    1   /* an already-attached actor is entering Play (Stop -> Play)    */
#define AVER_FW_BEGIN_RELOAD  2   /* re-entry after a hot reload; state a reload preserved is kept */

#define AVER_FW_END_DESTROY   0   /* the actor is being destroyed                                 */
#define AVER_FW_END_STOP      1   /* Play -> Stop; the actor survives, its play life does not     */
#define AVER_FW_END_RELOAD    2   /* leaving for a hot reload; the instance will be rebound after */
#define AVER_FW_END_TRAVEL    3   /* level travel; the current level's actors are torn down       */

/* ================================================================================================
 * THE PER-CLASS NATIVE VTABLE — AvActorVTable.
 *
 * ONE per CLASS, never per object. Native gameplay classes tick through a hoisted per-class vtable:
 * the framework reads the class's ONE `tick` pointer and straight-walks that class's dense instance
 * vector, so it costs one indirect call per class per tick group per frame, not one per actor.
 *
 * `user` is an opaque per-class cookie the native registrant chooses (NULL for a managed class, which
 * does not tick this way at all — see AvManagedDispatch). The framework stores this table BY VALUE in
 * the class record, so the registrant's memory may go away without leaving a pointer behind to call
 * into, and a NULL slot is inherited from the parent class at seal time — which is how "override only
 * tick" works with no storage inheritance.
 *
 * STEP-10 STATUS: this struct is declared here because §5.3 fixes it as part of this file's contract
 * and both sides must agree on its shape, but the native per-class tick LOOP that drives it — and the
 * aver_fw_class_set_vtable registrar — are a documented follow-up (see the report). Step 10 delivers
 * the MANAGED path (tick_all); no native class registers a vtable yet, so nothing reads this table.
 * ============================================================================================== */
#define AVER_FW_VTABLE_VERSION 1

typedef void (AVER_FW_CALL* aver_fw_vt_begin_play_fn)(void* user, aver_entity e, int32_t reason);
typedef void (AVER_FW_CALL* aver_fw_vt_tick_fn)      (void* user, aver_entity e, float   dt);
typedef void (AVER_FW_CALL* aver_fw_vt_end_play_fn)  (void* user, aver_entity e, int32_t reason);

typedef struct AvActorVTable {
    int32_t                  structBytes;      /* sizeof(AvActorVTable); install rejects a short table */
    int32_t                  contractVersion;  /* AVER_FW_VTABLE_VERSION as this registrant was built  */
    void*                    user;             /* opaque per-class cookie; NULL for a managed class     */
    aver_fw_vt_begin_play_fn beginPlay;        /* per actor of this class, at birth / play-entry        */
    aver_fw_vt_tick_fn       tick;             /* per actor of this class, once a frame in its group    */
    aver_fw_vt_end_play_fn   endPlay;          /* per actor of this class, before teardown              */
} AvActorVTable;

/* ================================================================================================
 * THE MANAGED DISPATCH TABLE — AvManagedDispatch.
 *
 * The framework never dispatches managed actors per entity. A reverse-P/Invoke plus a per-actor
 * dictionary lookup, once per actor per frame, is a cost this design would never be able to argue
 * away. Instead the framework makes exactly ONE managed call per tick group per frame — tick_all(group,
 * dt) — and the bridge walks its OWN dense instance list on the managed side. Note the contrast with
 * the vtable above: these pointers take NO `user`; they route by entity handle, class-name hash, or
 * tick group, because the single table serves EVERY managed class.
 *
 * WHY THE SHAPE IS FIXED — the hot-reload dangle argument. The managed side never registers a pointer
 * to USER code. It installs ONE table whose entries live in the bridge assembly, which hostfxr loads
 * once for the life of the process and which is NOT collectible. Every managed class then gets a
 * vtable of the SAME fixed native thunks (compiled into Aver.Framework) that route through this one
 * table. The framework stores the table BY VALUE and clears it to NULL on unload, so native state can
 * never hold a function pointer OWNED by the collectible AssemblyLoadContext — an ALC unload cannot
 * dangle one. That makes hot reload safe by construction rather than by ordering discipline, and it is
 * why clearing the table is simply a NULL store the call-site guards already handle.
 *
 * The six pointers below are §5.3's set; build_models is the seventh, the C# BuildModels hook the host
 * calls once after Self is bound and before beginPlay. Three possession/session hooks follow (v2):
 * possessed / unpossessed on a pawn and post_login on the GameMode. All parameters and returns are C-safe
 * (int32_t / int64_t / float / pointer) so no managed marshalling is ever implied.
 *
 * VERSION 2 added the three possession/session hooks by APPENDING them. Install validates structBytes
 * AND contractVersion, so a bridge built against v1 is refused by a v2 framework and vice versa — the two
 * are built together from this header, so they never disagree in practice.
 * ============================================================================================== */
#define AVER_FW_DISPATCH_VERSION 2

/* 1 == an instance of the named class now exists on the managed side and is bound to entity `e`. */
typedef int32_t (AVER_FW_CALL* aver_fw_bind_fn)       (int64_t classNameHash, aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_unbind_fn)     (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_begin_play_fn) (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_tick_all_fn)   (int32_t tickGroup, float dt);
typedef void    (AVER_FW_CALL* aver_fw_end_play_fn)   (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_rebound_fn)    (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_build_models_fn)(aver_entity e);
/* possessed: a pawn was possessed by `controller`. unpossessed: a pawn was released. post_login: a
 * controller entered the world under `gameMode` (fired on the GameMode). All dispatched per actor. */
typedef void    (AVER_FW_CALL* aver_fw_possessed_fn)  (aver_entity pawn, aver_entity controller);
typedef void    (AVER_FW_CALL* aver_fw_unpossessed_fn)(aver_entity pawn);
typedef void    (AVER_FW_CALL* aver_fw_post_login_fn) (aver_entity gameMode, aver_entity controller);

typedef struct AvManagedDispatch {
    int32_t                  structBytes;      /* sizeof(AvManagedDispatch); install rejects a short table */
    int32_t                  contractVersion;  /* AVER_FW_DISPATCH_VERSION as the bridge was built with    */
    aver_fw_bind_fn          bind;             /* construct+bind the class named by hash to an entity      */
    aver_fw_unbind_fn        unbind;           /* drop the managed instance bound to an entity             */
    aver_fw_begin_play_fn    beginPlay;        /* OnBeginPlay(reason) for one actor                        */
    aver_fw_tick_all_fn      tick_all;         /* OnTick for a WHOLE tick group in one managed transition  */
    aver_fw_end_play_fn      endPlay;          /* OnEndPlay(reason) for one actor, before its teardown     */
    aver_fw_rebound_fn       rebound;          /* OnRebound after a hot-reload rebind (in place of Begin)  */
    aver_fw_build_models_fn  build_models;     /* BuildModels once, after bind and before beginPlay        */
    aver_fw_possessed_fn     possessed;        /* OnPossessed(controller) on a pawn, when possessed  (v2)  */
    aver_fw_unpossessed_fn   unpossessed;      /* OnUnpossessed() on a pawn, when released            (v2)  */
    aver_fw_post_login_fn    post_login;       /* OnPostLogin(controller) on the GameMode, post-possess(v2) */
} AvManagedDispatch;

/* ---- install / clear -------------------------------------------------------------------------------
 * Install stores the table BY VALUE after validating structBytes + contractVersion. It REFUSES a second
 * non-null install while one is live (returns 0 and logs): only the EXECUTABLE may wire the bridge's
 * entries into this module — right after the script host bootstraps — and nothing in the build can
 * enforce that, so the refusal is the enforcement. The framework links Core/Assets/Scene only and must
 * never learn a CLR exists, which is why the wiring is a table installed from outside, not a link edge.
 *
 * Clear is a NULL store (call it BEFORE the ALC is unloaded). After a clear the tick ticks NOTHING
 * rather than faulting — the guards on every call site already test the pointer.
 *
 * Both return 1 on success / 0 on a rejected request, matching the framework's error convention. */
AVER_FW_ABI int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d);
AVER_FW_ABI int32_t aver_fw_clear_managed_dispatch(void);
AVER_FW_ABI int32_t aver_fw_managed_dispatch_installed(void);   /* 1 while a table is live, else 0 */

/* ---- the native tick entry point the app/host drives -----------------------------------------------
 * Called once per tick group per frame. If a managed dispatch is installed it makes EXACTLY ONE
 * tick_all(group, dt) call for that group (the bridge walks its own dense list; the framework does NOT
 * loop per managed actor — nobody adds a per-entity managed tick entry point in a year's time). Native
 * per-class vtable ticking would also run here once that path exists; today it is a documented
 * follow-up, so this drives the managed path only. Returns 1 if a managed tick_all fired, else 0. */
AVER_FW_ABI int32_t aver_fw_tick(int32_t tickGroup, float dt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_HOOKS_H */
