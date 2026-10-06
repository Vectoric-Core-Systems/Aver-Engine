#ifndef AVER_FRAMEWORK_TIMERS_ABI_H
#define AVER_FRAMEWORK_TIMERS_ABI_H

/* Timers and events C ABI (Aver.Framework). Separate header so the surface stays readable; same
 * conventions as framework_abi.h: int32_t/int64_t/float/const char* only, 0 == invalid handle,
 * inbound strings UTF-8, outbound strings never freed by the caller. Single game thread.
 * Semantics: docs/GAME_TIMERS_EVENTS.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

/* Own version, bumped on additive change. Integration may fold this into AVER_FW_ABI_VERSION_MINOR. */
#define AVER_FW_TIMERS_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_timers_abi_version(void);

/* ---- TIMERS ---------------------------------------------------------------------------------- */

#define AVER_FW_TIMER_REPEAT          0x1   /* re-arm after each fire                      */
#define AVER_FW_TIMER_IGNORE_PAUSE    0x2   /* keeps running while the service is paused   */
#define AVER_FW_TIMER_IGNORE_DILATION 0x4   /* runs on real time instead of dilated time   */

/* Fired from aver_fw_timers_update. The timer may be cancelled or created from inside. For a
 * one-shot the handle is already invalid when this runs. */
typedef void (AVER_FW_CALL* aver_fw_timer_dispatch_fn)(int32_t handle, int64_t token, void* user);
/* Installs the single native-to-host timer callback; null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_timer_dispatch(aver_fw_timer_dispatch_fn fn, void* user);

/* Creates a timer. `delay` seconds (0 = next update); with AVER_FW_TIMER_REPEAT it repeats every
 * `delay`, first firing after `firstDelay` when that is >= 0. `maxFires` > 0 stops a repeating
 * timer after that many fires. `owner` is a cancel_owner key (typically an entity); `token` is
 * handed back to the dispatch callback. Returns the handle, 0 on NaN delay or a full table. */
AVER_FW_ABI int32_t aver_fw_timer_set(float delay, float firstDelay, int32_t maxFires, int32_t flags,
                                      int32_t owner, int64_t token);
AVER_FW_ABI int32_t aver_fw_timer_cancel(int32_t handle);              /* 1 if it was active */
AVER_FW_ABI int32_t aver_fw_timer_cancel_owner(int32_t owner);         /* count cancelled; owner 0 is a no-op */
AVER_FW_ABI int32_t aver_fw_timer_active(int32_t handle);
AVER_FW_ABI float   aver_fw_timer_remaining(int32_t handle);           /* seconds, -1 for an invalid handle */
AVER_FW_ABI int32_t aver_fw_timer_fire_count(int32_t handle);          /* -1 for an invalid handle */
AVER_FW_ABI int32_t aver_fw_timer_set_paused(int32_t handle, int32_t paused);
AVER_FW_ABI int32_t aver_fw_timer_is_paused(int32_t handle);
AVER_FW_ABI int32_t aver_fw_timer_reset(int32_t handle, float delay);  /* delay < 0 keeps the interval */

/* Global clock. Update is the host's per-frame call, with the UNSCALED frame delta: dilation and
 * pause are applied inside. While aver_fw_play_state() == PAUSED the service counts as paused. A
 * play session ending (state returns to EDITOR) clears every timer and pending event. */
AVER_FW_ABI void    aver_fw_timers_update(float rawDt);
AVER_FW_ABI void    aver_fw_timers_set_paused(int32_t paused);
AVER_FW_ABI int32_t aver_fw_timers_paused(void);
AVER_FW_ABI void    aver_fw_timers_set_dilation(float scale);          /* clamped to [0, 100] */
AVER_FW_ABI float   aver_fw_timers_dilation(void);
/* rawDt * dilation, or 0 while paused: lets the host tick gameplay on the same clock. */
AVER_FW_ABI float   aver_fw_timers_scaled_dt(float rawDt);
AVER_FW_ABI int64_t aver_fw_timers_time_us(void);                      /* dilated, pause-respecting */
AVER_FW_ABI int64_t aver_fw_timers_real_time_us(void);
AVER_FW_ABI int32_t aver_fw_timers_count(void);
AVER_FW_ABI int32_t aver_fw_timers_epoch(void);                        /* bumps whenever timers are bulk-cleared */
AVER_FW_ABI void    aver_fw_timers_clear(void);

/* ---- EVENTS ---------------------------------------------------------------------------------- */

#define AVER_FW_EVENT_NONE   0
#define AVER_FW_EVENT_INT    1
#define AVER_FW_EVENT_FLOAT  2
#define AVER_FW_EVENT_BOOL   3
#define AVER_FW_EVENT_STRING 4
#define AVER_FW_EVENT_VEC3   5
#define AVER_FW_EVENT_ENTITY 6

/* Runs for each matching subscriber. Return non-zero to consume the event. The event's payload is
 * readable through the aver_fw_event_current_* accessors for the duration of the call only. */
typedef int32_t (AVER_FW_CALL* aver_fw_event_dispatch_fn)(int32_t subscription, int64_t token, void* user);
AVER_FW_ABI int32_t aver_fw_set_event_dispatch(aver_fw_event_dispatch_fn fn, void* user);

/* Subscribes to a named event. Higher `priority` runs first. `senderFilter` != 0 limits it to events
 * raised by that sender. `owner` doubles as the target filter: an event dispatched with a target
 * reaches only subscribers whose owner equals it. Subscribing inside a handler takes effect from
 * the next dispatch. Returns the subscription id, 0 on an empty name. */
AVER_FW_ABI int32_t aver_fw_event_subscribe(const char* name, int32_t owner, int32_t priority,
                                            int32_t senderFilter, int64_t token);
AVER_FW_ABI int32_t aver_fw_event_unsubscribe(int32_t subscription);   /* safe inside a handler */
AVER_FW_ABI int32_t aver_fw_event_unsubscribe_owner(int32_t owner);    /* count removed */
AVER_FW_ABI int32_t aver_fw_event_listener_count(const char* name);

/* Payload staging: push values, then dispatch or post, which takes and clears the staged payload.
 * Each push returns 1, or 0 when the 16-argument cap is reached. */
AVER_FW_ABI void    aver_fw_event_payload_clear(void);
AVER_FW_ABI int32_t aver_fw_event_push_int(int64_t v);
AVER_FW_ABI int32_t aver_fw_event_push_float(float v);
AVER_FW_ABI int32_t aver_fw_event_push_bool(int32_t v);
AVER_FW_ABI int32_t aver_fw_event_push_string(const char* v);
AVER_FW_ABI int32_t aver_fw_event_push_vec3(float x, float y, float z);
AVER_FW_ABI int32_t aver_fw_event_push_entity(int32_t e);

/* Immediate dispatch; returns the number of handlers run, -1 on an empty name. Past 8 nested
 * dispatches the event is queued instead (aver_fw_events_overflowed counts these). */
AVER_FW_ABI int32_t aver_fw_event_dispatch(const char* name, int32_t sender, int32_t target);
/* Deferred dispatch: queued until aver_fw_events_flush. Returns 1, -1 on an empty name. */
AVER_FW_ABI int32_t aver_fw_event_post(const char* name, int32_t sender, int32_t target);

/* The frame-safe point; call once per frame after timers_update and before the tick groups.
 * Returns events dispatched. */
AVER_FW_ABI int32_t aver_fw_events_flush(void);
AVER_FW_ABI int32_t aver_fw_events_pending(void);
AVER_FW_ABI int32_t aver_fw_events_overflowed(void);
AVER_FW_ABI void    aver_fw_events_clear_pending(void);
AVER_FW_ABI void    aver_fw_events_clear(void);                        /* pending events and every subscription */

/* The event being delivered, valid inside a handler only. Index accessors coerce numerically
 * (int/float/bool/entity read as each other) and return 0 / "" on a bad index or type. */
AVER_FW_ABI int32_t aver_fw_event_current_valid(void);                 /* 1 inside a handler */
AVER_FW_ABI const char* aver_fw_event_current_name(void);
AVER_FW_ABI int32_t aver_fw_event_current_sender(void);
AVER_FW_ABI int32_t aver_fw_event_current_target(void);
AVER_FW_ABI int32_t aver_fw_event_current_arg_count(void);
AVER_FW_ABI int32_t aver_fw_event_current_arg_type(int32_t index);     /* AVER_FW_EVENT_*, 0 out of range */
AVER_FW_ABI int64_t aver_fw_event_current_arg_int(int32_t index);
AVER_FW_ABI float   aver_fw_event_current_arg_float(int32_t index);
AVER_FW_ABI int32_t aver_fw_event_current_arg_bool(int32_t index);
AVER_FW_ABI int32_t aver_fw_event_current_arg_entity(int32_t index);
AVER_FW_ABI const char* aver_fw_event_current_arg_string(int32_t index);
AVER_FW_ABI int32_t aver_fw_event_current_arg_vec3(int32_t index, float* outXyz);   /* 1 if a Vec3 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_TIMERS_ABI_H */
