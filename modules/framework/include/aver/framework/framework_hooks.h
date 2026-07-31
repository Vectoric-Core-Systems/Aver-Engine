#ifndef AVER_FRAMEWORK_HOOKS_H
#define AVER_FRAMEWORK_HOOKS_H

/* Aver.Framework dispatch tables: the C function pointers gameplay code installs so the framework
 * can call up into it. Not a P/Invoke marshalling surface — framework_abi.h is that half. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, aver_entity, aver_class */

#include <stdint.h>

/* Must be __cdecl to match the bridge's [UnmanagedCallersOnly] thunks, or the stack corrupts. */
#if defined(_WIN32)
#  define AVER_FW_CALL __cdecl
#else
#  define AVER_FW_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Why a lifecycle hook fired. Pinned integer for integer to Aver.Framework's BeginReason /
 * EndReason (Enums.cs). */
#define AVER_FW_BEGIN_SPAWN   0   /* a brand-new actor was spawned into a live world              */
#define AVER_FW_BEGIN_PLAY    1   /* an already-attached actor is entering Play (Stop -> Play)    */
#define AVER_FW_BEGIN_RELOAD  2   /* re-entry after a hot reload; state a reload preserved is kept */

#define AVER_FW_END_DESTROY   0   /* the actor is being destroyed                                 */
#define AVER_FW_END_STOP      1   /* Play -> Stop; the actor survives, its play life does not     */
#define AVER_FW_END_RELOAD    2   /* leaving for a hot reload; the instance will be rebound after */
#define AVER_FW_END_TRAVEL    3   /* level travel; the current level's actors are torn down       */

#define AVER_FW_VTABLE_VERSION 1

typedef void (AVER_FW_CALL* aver_fw_vt_begin_play_fn)(void* user, aver_entity e, int32_t reason);
typedef void (AVER_FW_CALL* aver_fw_vt_tick_fn)      (void* user, aver_entity e, float   dt);
typedef void (AVER_FW_CALL* aver_fw_vt_end_play_fn)  (void* user, aver_entity e, int32_t reason);

/* One per CLASS, never per object: a native class's lifecycle entry points, stored by value. */
typedef struct AvActorVTable {
    int32_t                  structBytes;      /* sizeof(AvActorVTable); install rejects a short table */
    int32_t                  contractVersion;  /* AVER_FW_VTABLE_VERSION as this registrant was built  */
    void*                    user;             /* opaque per-class cookie; NULL for a managed class     */
    aver_fw_vt_begin_play_fn beginPlay;        /* per actor of this class, at birth / play-entry        */
    aver_fw_vt_tick_fn       tick;             /* per actor of this class, once a frame in its group    */
    aver_fw_vt_end_play_fn   endPlay;          /* per actor of this class, before teardown              */
} AvActorVTable;

#define AVER_FW_DISPATCH_VERSION 2

/* 1 == an instance of the named class now exists on the managed side and is bound to entity `e`. */
typedef int32_t (AVER_FW_CALL* aver_fw_bind_fn)       (int64_t classNameHash, aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_unbind_fn)     (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_begin_play_fn) (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_tick_all_fn)   (int32_t tickGroup, float dt);
typedef void    (AVER_FW_CALL* aver_fw_end_play_fn)   (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_rebound_fn)    (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_build_models_fn)(aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_possessed_fn)  (aver_entity pawn, aver_entity controller);
typedef void    (AVER_FW_CALL* aver_fw_unpossessed_fn)(aver_entity pawn);
typedef void    (AVER_FW_CALL* aver_fw_post_login_fn) (aver_entity gameMode, aver_entity controller);

/* The ONE table serving every managed class. Routes by entity handle, class-name hash or tick
 * group, never by a per-class cookie. Stored by value so no ALC-owned pointer is ever retained. */
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

/* Stores the table by value after validating structBytes + contractVersion. Refuses a second
 * install while one is live. 1 on success, 0 on a rejected request. */
AVER_FW_ABI int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d);
/* Null-stores the table. Call before the ALC is unloaded; afterwards the tick fires nothing. */
AVER_FW_ABI int32_t aver_fw_clear_managed_dispatch(void);
/* 1 while a table is live, else 0. */
AVER_FW_ABI int32_t aver_fw_managed_dispatch_installed(void);

/* Ticks one group. Makes exactly ONE tick_all(group, dt) call; the bridge walks its own list.
 * Returns 1 if a managed tick_all fired, else 0. */
AVER_FW_ABI int32_t aver_fw_tick(int32_t tickGroup, float dt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_HOOKS_H */
