# Game systems plan (2026-10-06)

Status: **plan only, nothing built.** Built after the owner signs off, as a separate Sonnet workflow in its own
worktree off `0.7-main-nrdneuraa`.

Source: the 2026-10-06 full-game gap scan (five read-only area scans, claims checked against the code). The owner
chose ten features and asked for **general-purpose engine systems, not genre-specific ones** (no health/inventory
style gameplay; those are left to game code).

## Principles

- General building blocks only. Every system is usable by any genre and contains no game rules.
- Each feature ships three ways: a C++ API in its module, a C# API (`scripting/csharp/Aver.*`, P/Invoke over the
  module's C ABI like `Aver.Physics`), and visual-script nodes named `AN_*` aiming at Blueprint parity.
- Editor support where a designer needs it, as new editor files registered in one place.
- Unit tests per feature (no engine runs needed to pass them), a doc page per feature under `docs/`, and brief
  code comments.
- Old `.ocworld`/project files keep loading.

## What exists today (checked)

| Area | Present | Missing |
|---|---|---|
| Game AI | `modules/synapse`: `Bt.hpp` (behaviour trees), `Nav.hpp`/`NavBake.hpp` (navmesh, A*). `modules/synapse.scene`: `SynapseAgent.hpp` (path following), `SynapsePerception.hpp` (sight cones), `SynapseBt.hpp` | Blackboard, BT editor, steering/avoidance, crowds, hearing, cover/tactics, AI debug views |
| UI | `modules/ui`: `UiDrawList`, `UiFont` (draw primitives and text); `render.ui`; `ui.abi`; C# `Aver.UI` | Widgets, layout, focus/navigation, menus, HUD widgets, settings and rebinding screens |
| Animation | `modules/anim`: `AnimSampler`, `Pose`, `Ik`; montages and notifies in the anim editor | Blend spaces, animation state machines |
| Audio | `modules/audio`: `Mixer`, `Sound`; WASAPI backend; 3D pan and distance | Streaming playback, occlusion, reverb zones |
| Decals | none | Projected decals |
| Prefabs | none (only an asset-id mention) | Prefab assets, instances, overrides |
| Timers/events | animation notifies only | General timers and an event/delegate system |

## Features

### 1. Timers and events (S)
- **Build:** a frame-safe timer service (one-shot, repeating, pause/time-dilation aware, cancel by handle) and a
  typed event bus (named events with payloads, subscribe/unsubscribe, deferred dispatch at a safe point in the
  frame).
- **Where:** `modules/framework` (service + C ABI), C# `Aver.Framework`, `AN_SetTimer`, `AN_ClearTimer`,
  `AN_DispatchEvent`, `AN_OnEvent`.
- **Tests:** ordering, pause, cancel inside a callback, re-entrancy.
- **Note:** first in the order. Several other features (AI, UI) raise events through it.

### 2. Blackboard + behaviour-tree editor (M)
- **Build:** a typed blackboard (keys with types, per-agent and shared/team scopes, change observers that BT
  decorators can watch) wired into `Bt.hpp`. A visual BT editor as an asset-editor tab, with live debugging:
  active node highlight, blackboard values, last result per node.
- **Where:** `modules/synapse` (blackboard + BT decorators), `modules/synapse.scene` (per-entity binding), a new
  editor file for the BT tab, `AN_` nodes for blackboard get/set.
- **Tests:** observer abort semantics, scope isolation, BT round trip through the asset format.

### 3. Steering and crowds (M)
- **Build:** steering behaviours (seek, arrive, flee, wander, separation, path following) and local avoidance for
  crowds (reciprocal velocity obstacles or an equivalent), integrated with navmesh path following so agents in
  narrow spaces do not deadlock.
- **Where:** `modules/synapse.scene` (new files), C# + `AN_` nodes.
- **Tests:** two agents passing in a corridor, 200 agents through a doorway without overlap, determinism at a
  fixed step.

### 4. Hearing + AI debug views (S)
- **Build:** noise events (position, loudness, tag) heard by agents within range, attenuated by occlusion where
  physics raycasts allow; memory of last known position in the blackboard. Viewport debug overlays (toggle in
  Show menu): sight cones, hearing radii, current path, steering vectors, BT state above heads.
- **Where:** `modules/synapse.scene` (perception), a new editor overlay file.
- **Tests:** hearing range and occlusion, memory decay.

### 5. Cover and tactics (M)
- **Build:** cover points (authored markers plus optional automatic generation from navmesh edges next to walls),
  cover queries (protected from a threat position, distance and path cost), reservation so agents do not share a
  point, and simple squad coordination primitives (assign roles, keep spacing, flank) exposed as BT tasks.
- **Where:** `modules/synapse.scene`, BT task registrations, an editor gizmo for cover markers.
- **Tests:** cover validity against a threat, reservation conflicts.

### 6. Game UI widgets + menus (L)
- **Build:** a retained widget system on top of `UiDrawList`/`UiFont`: panels, text, image, button, toggle, slider,
  list/scroll, text input, progress bar; layout (stack, grid, anchors, DPI scaling); focus and gamepad/keyboard
  navigation; styling/themes; input routing that pauses game input while a menu has focus. Ready-made general
  screens: settings (graphics, audio, controls) and key rebinding using the existing input system.
- **Where:** `modules/ui` (widgets/layout), `render.ui` (drawing), `ui.abi` + C# `Aver.UI`, `AN_` nodes to create and
  bind widgets, a UI layout asset with a simple editor tab.
- **Tests:** layout maths, focus navigation order, hit testing, DPI scaling.
- **Note:** the largest item. It may be split into widgets/layout first and menus second.

### 7. Decals (M)
- **Build:** projected decals (box projector, base colour/normal/roughness, fade by angle, sort order), as a
  component and a runtime spawn API with a pool for gameplay decals (impacts, footprints).
- **Where:** the staged ray-driven path in `modules/render.voxi` (applied on the G-buffer/surface before lighting so
  ray hits see them too), plus the raster path; editor component and gizmo.
- **Tests:** projector maths, pool recycling; `VoxiShaderCompileTest` covers the shaders.
- **Note:** touches `render.voxi`, like the lighting work in `feat/gaps-1006`; merge after that lands.

### 8. Animation blend spaces + state machines (M)
- **Build:** 1D and 2D blend spaces (sample-point triangulation, sync markers so foot phase matches) and animation
  state machines (states, transitions with conditions and blend times, sub-machines, events on enter/exit), driven
  by parameters from C#/`AN_` nodes.
- **Where:** `modules/anim` (runtime), `modules/anim.scene` (per-entity instances), the anim editor (new tabs/files).
- **Tests:** blend weights sum to one, transition timing, sync-marker phase.

### 9. Audio streaming + occlusion (M)
- **Build:** streamed playback for long files (music, dialogue, ambience) with a decoder thread and ring buffer,
  crossfades between tracks; occlusion and obstruction from physics raycasts (low-pass + volume), and reverb zones
  (volumes with blend distance).
- **Where:** `modules/audio`, `modules/sound`, `audio.abi`, C# + `AN_` nodes, a reverb zone component.
- **Tests:** streaming under-run handling, crossfade gain, occlusion filter response.

### 10. Prefabs (M)
- **Build:** prefab assets (a saved entity hierarchy with components), instances in levels that keep a link, per-
  instance overrides recorded as property deltas, nested prefabs, "apply to prefab" / "revert", and propagation of
  prefab edits to every instance. Undo-aware.
- **Where:** `modules/formats` (prefab asset + override records in `.ocworld`), `modules/scene` (instance link), the
  editor (content browser "create prefab", Details overrides UI).
- **Tests:** override survives a prefab edit, nested propagation, round trip.
- **Note:** touches the scene format; merge after `feat/gaps-1006` (which edits `CLight`).

## Execution

- **Worktree:** `feat/game-systems` off the current `0.7-main-nrdneuraa`, created only when this plan is approved.
- **Agents (Sonnet), each owning its files:**
  1. Timers and events.
  2. Blackboard + BT editor + AI debug views (editor side).
  3. Steering and crowds, hearing, cover and tactics (runtime side).
  4. Game UI widgets + menus.
  5. Decals.
  6. Blend spaces + state machines.
  7. Audio streaming + occlusion.
  8. Prefabs.
  9. Integration: wiring, build in the worktree, unit tests, `VoxiShaderCompileTest`; no engine runs.
- **Shared files** (graph node registration, editor panel registration, top-level CMake): agents write new files
  and list the wiring; only the integration agent edits the shared files.
- **Order:** timers/events first (others raise events through it), the rest in parallel; decals and prefabs merge
  after the lighting/soft-body branch.
- **Review:** each feature lands as its own commit so it can be reviewed or reverted alone; the owner tests in the
  editor before anything is pushed.

## Open questions for the owner

- UI: should menus be authored in a visual layout editor, or code/graphs first (editor later)?
- Prefabs: should edits to a prefab propagate automatically, or ask per level?
- Crowds: target size (tens or hundreds of agents)? It decides whether avoidance runs on the CPU or the GPU.
