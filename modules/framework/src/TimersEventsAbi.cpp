// Timers and events C ABI: one process-wide TimerService and EventBus behind framework_timers_abi.h.
// Single game thread; see docs/GAME_TIMERS_EVENTS.md.

#include "aver/framework/framework_timers_abi.h"

#include "aver/framework/EventBus.hpp"
#include "aver/framework/TimerService.hpp"

#include <cmath>
#include <memory>
#include <string>

using namespace aver::fw;

namespace {

TimerService& timers() { static TimerService t; return t; }
EventBus&     bus()    { static EventBus b;      return b; }
EventPayload& staged() { static EventPayload p;  return p; }

aver_fw_timer_dispatch_fn g_timerFn   = nullptr;
void*                     g_timerUser = nullptr;
aver_fw_event_dispatch_fn g_eventFn   = nullptr;
void*                     g_eventUser = nullptr;

const EventValue* argAt(int32_t i) {
    const Event* ev = bus().current();
    if (!ev || i < 0 || static_cast<size_t>(i) >= ev->payload.args.size()) return nullptr;
    return &ev->payload.args[static_cast<size_t>(i)];
}

int32_t pushStaged(EventValue v) { return staged().push(std::move(v)) ? 1 : 0; }

Event takeEvent(const char* name, int32_t sender, int32_t target) {
    Event ev;
    ev.name    = name;
    ev.sender  = sender;
    ev.target  = target;
    ev.payload = std::move(staged());
    staged().clear();
    return ev;
}

} // namespace

extern "C" {

int32_t aver_fw_timers_abi_version(void) { return AVER_FW_TIMERS_ABI_VERSION; }

/* ---- timers ---- */

int32_t aver_fw_set_timer_dispatch(aver_fw_timer_dispatch_fn fn, void* user) {
    g_timerFn = fn;
    g_timerUser = user;
    return 1;
}

int32_t aver_fw_timer_set(float delay, float firstDelay, int32_t maxFires, int32_t flags,
                          int32_t owner, int64_t token) {
    TimerOptions o;
    o.repeat     = (flags & AVER_FW_TIMER_REPEAT) != 0;
    o.firstDelay = firstDelay;
    o.maxFires   = maxFires;
    o.flags      = static_cast<uint32_t>(flags) & (kTimerIgnorePause | kTimerIgnoreDilation);
    o.owner      = owner;
    return timers().set(delay, [token](TimerHandle h) {
        if (g_timerFn) g_timerFn(h, token, g_timerUser);
    }, o);
}

int32_t aver_fw_timer_cancel(int32_t h)          { return timers().cancel(h) ? 1 : 0; }
int32_t aver_fw_timer_cancel_owner(int32_t o)    { return timers().cancelOwner(o); }
int32_t aver_fw_timer_active(int32_t h)          { return timers().isActive(h) ? 1 : 0; }
float   aver_fw_timer_remaining(int32_t h)       { return timers().remaining(h); }
int32_t aver_fw_timer_fire_count(int32_t h)      { return timers().fireCount(h); }
int32_t aver_fw_timer_set_paused(int32_t h, int32_t p) { return timers().setPaused(h, p != 0) ? 1 : 0; }
int32_t aver_fw_timer_is_paused(int32_t h)       { return timers().isPaused(h) ? 1 : 0; }
int32_t aver_fw_timer_reset(int32_t h, float d)  { return timers().reset(h, d) ? 1 : 0; }

void aver_fw_timers_update(float rawDt) {
    // Leaving a play session drops its timers and queued events; a stale one-shot must not fire into the next.
    static int32_t s_prevState = AVER_FW_PLAY_EDITOR;
    const int32_t state = aver_fw_play_state();
    if (s_prevState != AVER_FW_PLAY_EDITOR && state == AVER_FW_PLAY_EDITOR) {
        timers().clear();
        bus().clearPending();
    }
    s_prevState = state;

    const bool wasPaused = timers().paused();
    if (state == AVER_FW_PLAY_PAUSED && !wasPaused) {
        timers().setPaused(true);
        timers().update(rawDt);
        timers().setPaused(wasPaused);
        return;
    }
    timers().update(rawDt);
}

void    aver_fw_timers_set_paused(int32_t p) { timers().setPaused(p != 0); }
int32_t aver_fw_timers_paused(void)          { return timers().paused() ? 1 : 0; }
void    aver_fw_timers_set_dilation(float s) { timers().setDilation(s); }
float   aver_fw_timers_dilation(void)        { return timers().dilation(); }

float aver_fw_timers_scaled_dt(float rawDt) {
    if (!(rawDt > 0.0f) || timers().paused() || aver_fw_play_state() == AVER_FW_PLAY_PAUSED) return 0.0f;
    return rawDt * timers().dilation();
}

int64_t aver_fw_timers_time_us(void)      { return static_cast<int64_t>(timers().time() * 1.0e6); }
int64_t aver_fw_timers_real_time_us(void) { return static_cast<int64_t>(timers().realTime() * 1.0e6); }
int32_t aver_fw_timers_count(void)        { return timers().count(); }
int32_t aver_fw_timers_epoch(void)        { return static_cast<int32_t>(timers().epoch()); }
void    aver_fw_timers_clear(void)        { timers().clear(); }

/* ---- events ---- */

int32_t aver_fw_set_event_dispatch(aver_fw_event_dispatch_fn fn, void* user) {
    g_eventFn = fn;
    g_eventUser = user;
    return 1;
}

int32_t aver_fw_event_subscribe(const char* name, int32_t owner, int32_t priority,
                                int32_t senderFilter, int64_t token) {
    if (!name || !*name) return 0;
    SubscribeOptions o;
    o.owner = owner;
    o.priority = priority;
    o.senderFilter = senderFilter;
    auto idp = std::make_shared<int32_t>(0);
    const int32_t id = bus().subscribe(name, [token, idp](const Event&) -> bool {
        return g_eventFn && g_eventFn(*idp, token, g_eventUser) != 0;
    }, o);
    *idp = id;
    return id;
}

int32_t aver_fw_event_unsubscribe(int32_t id)       { return bus().unsubscribe(id) ? 1 : 0; }
int32_t aver_fw_event_unsubscribe_owner(int32_t o)  { return bus().unsubscribeOwner(o); }
int32_t aver_fw_event_listener_count(const char* n) { return n ? bus().listenerCount(n) : 0; }

void    aver_fw_event_payload_clear(void)         { staged().clear(); }
int32_t aver_fw_event_push_int(int64_t v)         { return pushStaged(EventValue::ofInt(v)); }
int32_t aver_fw_event_push_float(float v)         { return pushStaged(EventValue::ofFloat(v)); }
int32_t aver_fw_event_push_bool(int32_t v)        { return pushStaged(EventValue::ofBool(v != 0)); }
int32_t aver_fw_event_push_string(const char* v)  { return pushStaged(EventValue::ofString(v ? v : "")); }
int32_t aver_fw_event_push_vec3(float x, float y, float z) { return pushStaged(EventValue::ofVec3(x, y, z)); }
int32_t aver_fw_event_push_entity(int32_t e)      { return pushStaged(EventValue::ofEntity(e)); }

int32_t aver_fw_event_dispatch(const char* name, int32_t sender, int32_t target) {
    if (!name || !*name) { staged().clear(); return -1; }
    const Event ev = takeEvent(name, sender, target);
    return bus().dispatch(ev);
}

int32_t aver_fw_event_post(const char* name, int32_t sender, int32_t target) {
    if (!name || !*name) { staged().clear(); return -1; }
    bus().post(takeEvent(name, sender, target));
    return 1;
}

int32_t aver_fw_events_flush(void)         { return bus().flush(); }
int32_t aver_fw_events_pending(void)       { return static_cast<int32_t>(bus().pending()); }
int32_t aver_fw_events_overflowed(void)    { return bus().overflowed(); }
void    aver_fw_events_clear_pending(void) { bus().clearPending(); }
void    aver_fw_events_clear(void)         { bus().clear(); }

int32_t aver_fw_event_current_valid(void) { return bus().current() ? 1 : 0; }
const char* aver_fw_event_current_name(void) { const Event* e = bus().current(); return e ? e->name.c_str() : ""; }
int32_t aver_fw_event_current_sender(void)   { const Event* e = bus().current(); return e ? e->sender : 0; }
int32_t aver_fw_event_current_target(void)   { const Event* e = bus().current(); return e ? e->target : 0; }
int32_t aver_fw_event_current_arg_count(void) {
    const Event* e = bus().current();
    return e ? static_cast<int32_t>(e->payload.args.size()) : 0;
}
int32_t aver_fw_event_current_arg_type(int32_t i) { const EventValue* v = argAt(i); return v ? static_cast<int32_t>(v->type) : 0; }
int64_t aver_fw_event_current_arg_int(int32_t i)  { const EventValue* v = argAt(i); return v ? v->asInt() : 0; }
float   aver_fw_event_current_arg_float(int32_t i){ const EventValue* v = argAt(i); return v ? v->asFloat() : 0.0f; }
int32_t aver_fw_event_current_arg_bool(int32_t i) { const EventValue* v = argAt(i); return v && v->asBool() ? 1 : 0; }
int32_t aver_fw_event_current_arg_entity(int32_t i) { const EventValue* v = argAt(i); return v ? static_cast<int32_t>(v->asInt()) : 0; }
const char* aver_fw_event_current_arg_string(int32_t i) {
    const EventValue* v = argAt(i);
    return v && v->type == EventValueType::String ? v->s.c_str() : "";
}
int32_t aver_fw_event_current_arg_vec3(int32_t i, float* out) {
    const EventValue* v = argAt(i);
    if (!v || v->type != EventValueType::Vec3) return 0;
    if (out) { out[0] = v->f[0]; out[1] = v->f[1]; out[2] = v->f[2]; }
    return 1;
}

} // extern "C"
