# Timers and events

A frame-safe timer service and a typed event bus in `modules/framework`, with a C ABI
(`framework_timers_abi.h`), C# wrappers in `Aver.Framework` (`Timers`, `Events`) and four Aver Node
nodes (`AN_SetTimer`, `AN_ClearTimer`, `AN_DispatchEvent`, `AN_OnEvent`, plus the reader
`AN_EventPayload`). Single game thread only. Other features (AI, UI, prefabs) raise and hear events
through the bus rather than inventing their own delegate lists.

| Layer | File |
|---|---|
| Timer service (header-only) | `modules/framework/include/aver/framework/TimerService.hpp` |
| Event bus (header-only) | `modules/framework/include/aver/framework/EventBus.hpp` |
| C ABI | `framework_timers_abi.h`, `src/TimersEventsAbi.cpp` |
| C# | `scripting/csharp/Aver.Framework/Timers.cs`, `Events.cs`, `TimersEventsNative.cs` |
| Graph seam | `scripting/csharp/Aver.Graph/GraphTimerEvents.cs` |
| Tests | `tests/framework_timers/` (`FrameworkTimersTest`) |

The service classes are header-only so they unit-test without the DLL and keep the DLL surface to
the flat C ABI.

## Frame order

The host calls two functions once per frame, before the tick groups (`aver::game::tickGameplayGroups` in
`Runtime/include/aver/game/GameTick.hpp`, shared by the editor and the runtime):

1. `aver_fw_timers_update(rawDt)` with the UNSCALED frame delta. Pause and dilation are applied inside.
2. `aver_fw_events_flush()`. Events posted by timer callbacks are delivered the same frame.

Flushing is the "safe point": nothing else is mid-iteration, so a handler may spawn, destroy or
rewire freely. `Events.Dispatch` (immediate) is for code that already is at a safe point.

## Timers

```csharp
TimerHandle h = Timers.Set(2.0f, () => Spawn());                    // one-shot
Timers.Set(0.5f, () => Pulse(), repeat: true, maxFires: 10);        // repeating, 10 times
Timers.Set(1.0f, () => Ui.Tick(), flags: TimerFlags.IgnorePause);   // runs while the game is paused
Timers.Clear(h);
Timers.TimeDilation = 0.25f;                                        // slow motion
```

Semantics, each pinned by a test:

- **Ordering.** Within one frame timers fire in the order their due moment falls inside the frame,
  ties in creation order. A repeating timer interleaves with one-shots correctly rather than
  firing all its repeats first.
- **Catch-up.** A repeating timer fires once per elapsed interval in a long frame, carrying the
  leftover forward (no drift). One timer fires at most 8 times per update; the surplus of a bigger
  hitch is dropped. A zero interval fires once per update. The frame delta is clamped to 1 s so a
  debugger break does not fire a storm.
- **Pause and dilation.** `Timers.Paused` freezes every timer except those with `IgnorePause`.
  `TimeDilation` (clamped to 0..100) scales game time; `IgnoreDilation` timers run on real time.
  Dilation 0 freezes game timers. `Timers.Time` is dilated game time, `RealTime` is not. While
  `aver_fw_play_state()` is PAUSED the service counts as paused. Individual timers can be paused
  (`Timers.SetPaused(h, true)`) and keep their remaining time.
- **Callbacks may mutate the service.** Cancelling any timer from any callback, including the
  timer's own, takes effect immediately, even in the middle of a catch-up. A one-shot is already
  released inside its own callback (`Clear(self)` returns false). A timer created inside a callback
  starts counting at the next update, so a delay of 0 means "next frame". A callback that calls
  `Reset` on a due timer pushes it back. An exception in a callback is contained (and logged on the
  managed side); later timers still run.
- **Handles.** `(generation << 16) | slot`; a stale handle never aliases a reused slot, so
  cancelling an old handle is a safe no-op. Up to 65535 live timers.
- **Owners.** `owner` (typically an entity handle) lets `ClearOwner(entity)` cancel everything an
  entity started; owner 0 means none and is not a wildcard.
- **Session end.** When `aver_fw_play_state()` returns to EDITOR, `aver_fw_timers_update` clears all
  timers and pending events so a stale one-shot cannot fire into the next session. Subscriptions
  persist (they belong to their owners).

## Events

```csharp
var sub = Events.Subscribe("Damaged", e => Hurt(e.Target, e.GetFloat(0)), owner: me.Handle);
Events.Dispatch("Damaged", sender: attacker.Handle, target: victim.Handle, 12.5f);   // now
Events.Post("LevelCleared", args: 3);                                               // at the flush
Events.Unsubscribe(sub);
```

- **Names and payloads.** Events are named strings. The payload is an ordered list of up to 16 typed
  values: int, float, bool, string, Vec3, entity. Numeric kinds read as each other (`GetFloat` on an
  int works); a string reads 0 numerically. `EventData` is a snapshot, safe to keep.
- **Priority, filters, consume.** Higher `priority` first, ties in subscription order. A
  `senderFilter` limits delivery to one sender. A dispatch with a `target` reaches only
  subscribers whose `owner` equals it; a broadcast (target 0) reaches everyone. A handler registered
  with `SubscribeConsuming` returns true to stop later handlers.
- **Re-entrancy.** A handler may dispatch, post, subscribe, unsubscribe or clear.
  - Unsubscribing takes effect at once, even for the event in flight.
  - A subscriber added during a dispatch hears the NEXT event.
  - An immediate dispatch from a handler runs to completion first.
  - Nesting beyond 8 levels demotes the inner event to the deferred queue (counted in
    `Events.Overflowed`), not a stack overflow and not a drop.
- **Flush.** FIFO. Events posted by handlers run in the next pass, up to 8 passes per flush; a
  handler that always re-posts therefore costs one event per pass and the rest wait for the next
  flush. A `Flush` from inside a handler is refused. `ClearPending` from a handler abandons the
  rest of the batch.
- **Exceptions** in a handler are contained and counted; later handlers still run.

## C ABI

`framework_timers_abi.h`, own version `AVER_FW_TIMERS_ABI_VERSION` 1. Same conventions as
`framework_abi.h`: `int32_t`/`int64_t`/`float`/`const char*`, 0 is an invalid handle.

- Timers: `aver_fw_timer_set/cancel/cancel_owner/active/remaining/fire_count/set_paused/is_paused/reset`,
  `aver_fw_timers_update/set_paused/paused/set_dilation/dilation/scaled_dt/time_us/real_time_us/count/epoch/clear`.
  One native-to-host callback is installed with `aver_fw_set_timer_dispatch` and receives
  `(handle, token, user)`; the token is the caller's own id, so a language layer keeps its closures
  on its own side. `aver_fw_timers_epoch` bumps on every bulk clear so a managed layer can drop
  records that native code orphaned.
- Events: `aver_fw_event_subscribe/unsubscribe/unsubscribe_owner/listener_count`, payload staging
  `aver_fw_event_push_*`, `aver_fw_event_dispatch/post`, `aver_fw_events_flush/pending/overflowed/clear_pending/clear`,
  and the in-handler reads `aver_fw_event_current_*`. `aver_fw_set_event_dispatch` installs the single
  callback `(subscription, token, user)`; a non-zero return consumes the event.
  `aver_fw_event_dispatch` and `_post` take the staged payload and clear it.

`aver_fw_timers_scaled_dt(rawDt)` returns `rawDt * dilation` (0 while paused) for hosts that want
actor ticks on the same clock; wiring it is optional.

## Graph nodes

Node type names carry the `AN_` prefix. A timer or an event runs a graph through the existing
custom-event route: `GraphEvents.FireEventForGraph(entity, eventName)` runs the ENTRY of that name
on the entity's graph.

| Node | Pins | Attributes | Does |
|---|---|---|---|
| `AN_SetTimer` | in `exec`, `target` int (entity), `delay` float, `looping` bool; out `then` exec, `handle` int | `event=Name` | Fires graph event `Name` on `target` after `delay` (repeating if `looping`). `handle` is 0 if nothing was set. The timer is owned by `target`. |
| `AN_ClearTimer` | in `exec`, `handle` int; out `then` exec, `cleared` bool | none | Cancels a timer. |
| `AN_DispatchEvent` | in `exec`, `sender` int, `target` int, `i` int, `f` float, `immediate` bool; out `then` exec | `event=Name` | Raises bus event `Name` with payload `[i, f]`. Deferred to the frame flush unless `immediate`. |
| `AN_OnEvent` | out `exec` exec; no inputs (same shape as `OnHit`) | none (the `ENTRY` name IS the bus event name) | Entry node: `ENTRY e Damaged` + `NODE e AN_OnEvent` runs when bus event `Damaged` reaches this entity. |
| `AN_EventPayload` | in `exec`, `index` int; out `then` exec, `sender` int, `target` int, `i` int, `f` float, `b` bool | none | Query node (same shape as `MouseDelta`): argument `index` of the event whose chain is running, coerced. Zero outside an `AN_OnEvent` chain. |

Deferred dispatch is the default because a graph usually dispatches in the middle of its own exec
chain, where an immediate re-entry into another graph is the exact hazard the flush point removes.
`AN_OnEvent` entries should declare no `PARAM`s beyond what `FireEvent` supplies, same constraint as
`OnHit`. A graph subscribes when the bridge binds it (`GraphTimerEvents.BindGraph`) and loses its
subscriptions and timers when it unbinds (`UnbindEntity`).

## Not done

- No timer-by-name or "set if not set" variant; a graph keeps the `handle` in a `VAR`.
- No wildcard subscriptions and no event inheritance.
- Events are not saved with the world; timers are session state.
- The managed side (`Timers`, `Events`) has no C# unit tests; it needs the native DLL beside the test
  host, which `Aver.Graph.Tests` does not have today. The native service and ABI are covered by
  `FrameworkTimersTest`.
