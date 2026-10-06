# Animation blend spaces and state machines

Two assets and the runtime that plays them:

- **Blend space** (`.ocblend`): clips placed on a 1D or 2D parameter space. A query point is turned into
  weights over the nearby clips, and one shared cycle phase keeps their foot cycles in step.
- **State machine** (`.ocasm`): states (a clip, a blend space, or a sub-machine), transitions between
  them with conditions and blend times, and enter/exit events, driven by named parameters.

Runtime code is pure arithmetic over asset structs (no GPU, no scene):

| Piece | Where |
|---|---|
| Weights, triangulation, sync, `BlendSpacePlayer` | `modules/anim` `BlendSpace.hpp/.cpp` |
| State machine runtime | `modules/anim` `AnimStateMachine.hpp/.cpp` |
| `.ocblend` / `.ocasm` read and write | `modules/anim` `AnimGraphAsset.hpp/.cpp` |
| Per-entity instances, `CAnimGraph` | `modules/anim.scene` `AnimGraphSystem.hpp/.cpp` |
| Pose-source seam in `AnimSystem` | `modules/anim.scene` `AnimSystem.hpp` (`setPoseSource`) |
| Editor tabs | `sandbox/src` `BlendSpaceEditor.*`, `AnimStateMachineEditor.*` |
| C# surface | `scripting/csharp/Aver.Framework/AnimGraph.cs` |

## 1. Blend space

### Weights

A space has `dims` 1 or 2, an axis per dimension (`name`, `min`, `max`, `smoothing`), and samples
(`clip`, `x`, `y`, `rate`, `markers`).

- **1D**: samples are sorted by x; the query is clamped to the range and linearly interpolated between
  its two neighbours.
- **2D**: the samples are triangulated once per asset (`buildBlendTopology`) and the query takes the
  barycentric weights of the triangle that contains it. A query outside the hull goes to the nearest
  point of the mesh (an edge, so two samples) rather than being extrapolated: weights never go
  negative and never sum to anything but one.
- Triangulation works in axis-normalised coordinates (each axis scaled by its range), so a space of
  `direction` in degrees against `speed` in cm/s is not stretched into slivers. It is a brute-force
  Delaunay (empty circumcircle) with a tiny deterministic jitter that breaks the cocircular ties of
  grid layouts; a space has a few dozen points and the mesh is built once. Duplicate positions are
  ignored; collinear layouts fall back to 1D along the line.

### Sync markers

A sample may list named markers (`L`, `R`) in its own clip's seconds. All blended clips share one
**phase** in [0,1). The marker list defines the phase map for that clip:

- the heaviest active sample with markers is the **leader** and names the cycle (its marker names in
  time order, starting from its first marker);
- every other active sample is rotated so its cycle starts at a marker with the leader's first name and
  must match the whole name sequence, otherwise it silently falls back to plain normalised time;
- phase `k/n` is the k-th marker, and phase between markers interpolates linearly through the clip
  (wrapping past the clip end).

So a walk with L at 0.0 and R at 0.5 and a run with L at 0.1 and R at 0.4 both put the right foot down
at phase 0.5. The cycle length is the weight-averaged clip length (each divided by the sample's
`rate`). `syncMarkers: false` turns the map off (normalised time for everything).

Blend spaces are cyclic: the phase wraps. A one-shot is a Clip state, not a blend space.

### Smoothing

An axis with `smoothing > 0` follows the input with an exponential, `v += (target - v)(1 - e^(-dt/s))`.

## 2. State machine

### Asset

```
params:   name, type (float | int | bool | trigger), default
machines: [ root, sub-machines... ]           machines[0] is the root
  machine: name, entry, states, transitions
  state:   name, kind (clip | blendspace | machine), asset, machine, speed, loop,
           speedParam, xParam, yParam, onEnter, onExit, pos
  transition: from (state | -1 = Any), to (state | -2 = Exit), blend, exitTime (-1 = none),
              interruptible, allowSelf, when: [ {param, op, value} ]
```

Transitions name states by index. The order of a machine's transition list is its priority.

### Tick

`AnimStateMachine::tick(dt)`:

1. every live state advances (clip time, or the blend-space phase) at `speed * speedParam`;
2. the newest state's blend-in advances;
3. **at most one** transition fires.

A transition fired in tick N starts blending in tick N+1, so a blend of 0.5 s at a fixed 0.125 s step
completes on exactly the fourth tick after the one that fired. A transition with blend time 0 switches
immediately.

### Transitions

Candidates are checked deepest-first: the transitions of the leaf's own machine, then those of each
enclosing machine from the sub-machine state outward. Within a machine, `from == state` and Any are
both candidates, in list order; the first that passes wins.

A transition passes when every condition holds and, if it has an exit time, the source has reached it.
Exit time is the **cumulative** normalised time of the source since it was entered, so it keeps counting
across loops: after one full pass any exit time up to 1 stays satisfied. Each visit to a state starts at 0.
A non-looping clip stops at 1.
A transition with neither conditions nor exit time fires immediately (the editor warns).

Conditions: `> >= < <= == !=` on float and int, `is true` / `is false` on bool, `trigger` on a trigger.
A trigger is set by `setTrigger` and **consumed when a transition that tests it fires**.

Any transitions skip a target equal to the current state unless `allowSelf`.

### Blending and interruption

Live states form a stack. State i has an alpha (its blend-in progress); its weight is
`alpha_i * prod_{j>i} (1 - alpha_j)`, the oldest taking whatever is left. The weights therefore always
sum to one, a transition can interrupt another mid-blend (three or more states can be live), and a
state is dropped when a newer one reaches alpha 1. `interruptible: false` holds all other transitions
off until that transition's blend has finished.

The pose is the weighted mix of the live states' poses (`blendPose`, same as `AnimPlayer`'s crossfade).

### Sub-machines

A state of kind `machine` owns another machine; entering it enters that machine's entry state, down to
a leaf. Machines form a tree under machine 0 (a machine is the sub-machine of at most one state; this
is validated). Rules:

- A transition **from the sub-machine state** (in the parent) is checked while any inner state is
  active, so "leave Ground when not grounded" is one transition, not one per inner state.
- Exit and enter events: on a change, events go **deepest exit first, then outermost enter first**,
  and only for the part of the path that changed. A move between two states inside one sub-machine does
  not exit or enter the sub-machine state.
- An inner transition **to Exit** does not move the leaf. It marks the sub-machine state finished; a
  parent transition from that state with `exitTime` set then fires (exit time on a sub-machine state means
  "the inner machine reached Exit").

### Events

Each enter/exit appends an `AsmEvent {kind, machine, state, name}`; `name` is the state's `onEnter` /
`onExit` and is empty when the state has none. `drainEvents` returns them in order. `AnimGraphSystem`
hands them to a sink installed with `setEventSink`; the integration step forwards `name` to the event bus
(feature 1) so script and graph handlers see them.

## 3. Assets (file formats)

Both are UTF-8 JSON with `"format"` (`ocblend` / `ocasm`) and `"version"` (1). A newer version is
refused; an unknown format tag is refused. Floats are written with nine significant digits, which is
lossless for f32, so write -> parse -> write is byte-identical (`AnimGraphAssetTest` checks this).

Parsing does **not** require the asset to be valid: a machine half-authored in the editor must round
trip. Runtime loaders call `valid()` before binding (`AnimGraphSystem` does, and logs once per asset).

Asset references (`clip`, `asset`) are project asset paths. At runtime a path becomes an asset id by
`fnv1a64(path)`, the same hash `Assets.ObjectIdOf` computes in C#, and resolves through
`AnimSystem`'s asset-path resolver exactly like a skeleton or a clip.

## 4. On an entity: `CAnimGraph`

Registered at runtime like `CControlRig` (no change to built-in component ids or the scene ABI).
Attach with `AnimGraphSystem::attach`, or `AddComponent("CAnimGraph")` from C#.

| Field | |
|---|---|
| `machine` (i64) | `.ocasm` id |
| `playRate` (f32) | 0 is read as 1 |
| `flags` (i32) | bit 0 paused |
| `activeState` (i64, out) | `fnv1a64` of the active leaf state's name |
| `stateTime` (f32, out) | cumulative normalised time of the leaf |
| `paramHash0..15` (i64), `paramValue0..15` (f32) | parameter slots |

**Parameters** are (name hash, value) slots, so scripts and graph nodes drive a machine through the
generic scene field ABI with no new P/Invoke. A writer finds the slot with the hash, or the first free
one. The system reads slots each tick; a trigger slot is zeroed once taken. Sixteen distinct parameters
per entity is the limit. A hash the machine does not declare is ignored.

`AnimGraphSystem::tick(world, dt)` runs **before** `AnimSystem::tick` in the same frame. It adds a
`CAnimator` to a graph entity if there is none (AnimSystem poses animator entities). The pose reaches
the skinning matrices through AnimSystem's new **pose source** seam: after the rest pose is written and
before the clip sample, a source may return true to replace the clip. The pose modifier (control rig)
still runs after it, so IK layers on a state machine.

The entity needs a skeleton (`SetSkeleton`).

## 5. Script surface

`Entity` (Aver.Framework `AnimGraph.cs`):

```csharp
SetAnimGraph(string machineAsset, float playRate = 1)
SetAnimParam(string name, float | bool | int value)
TriggerAnim(string name)
GetAnimParam(string name)            // float?
IsInAnimState(string stateName)
AnimStateTime, AnimGraphRate, PauseAnimGraph(), ResumeAnimGraph()
```

## 6. Editors

**Blend space tab** (`.ocblend`): a canvas with the axes, the triangulation, draggable sample points
(with grid snap), and a red probe that shows the live weights as ring sizes, percentages and bars. Click
to move the probe; drag a point to move a sample; double-click empty space to add a sample (it copies the
selected sample's clip); Delete removes. The panel edits the axes, sync toggle, selected sample (clip,
position, rate, markers) and shows a validation message.

**State machine tab** (`.ocasm`): a node graph of the current machine. Right-click adds states (clip,
blend space, sub-machine); drag moves; middle-drag pans; double-click a sub-machine state opens it (breadcrumb
and Up button return). "New transition" (or click the Any pill) then click the target; sub-machines also
show an Exit pill. The panel edits parameters, the selected state (asset, speed, loop, bound
parameters, events, entry) and the selected transition (blend, exit time, interruptible, conditions,
priority). A **simulator** runs the machine live against parameter widgets and shows the active path,
the live blend weights and the last events. Clip lengths are not loaded in the editor, so every clip
counts as 1 s there.

Both tabs: undo/redo (one entry per drag or typing gesture), save, and reload on external change when
clean. All structural edits are free functions (`bs*`, `asm*`) so they are testable headless.

## 7. Graph nodes (`AN_` surface)

Six nodes cover the runtime. Each needs the usual edits (a `GraphInterop.*ForGraph` bridge first, then
`GraphNodeDefs.hpp`, `GraphCompiler.cs` emit and its method field); the bridge bodies exist in
`AnimGraphInterop` (AnimGraph.cs).

| Node | Pins | Calls |
|---|---|---|
| `SetAnimGraph` | entity, playRate(=1), success; attr `machine=` | `SetAnimGraphForGraph(int,string,float)` |
| `SetAnimParamFloat` | entity, value, success; attr `param=` | `SetAnimParamFloatForGraph(int,string,float)` |
| `SetAnimParamBool` | entity, value, success; attr `param=` | `SetAnimParamBoolForGraph(int,string,bool)` |
| `TriggerAnim` | entity, success; attr `param=` | `TriggerAnimForGraph(int,string)` |
| `IsInAnimState` | entity, result(bool); attr `state=` | `IsInAnimStateForGraph(int,string)` |
| `GetAnimStateTime` | entity, time(float) | `AnimStateTimeForGraph(int)` |

## 8. Tests

- `AnimBlendSpaceTest` (`tests/anim`): weights sum to one and are non-negative across 1D sweeps and 600
  random 2D queries; the weights reproduce the query position (no triangulation hole); outside the hull;
  irregular, duplicate and collinear layouts; sync-marker phase (same phase for walk and run, rotated
  clips, mismatched markers fall back); smoothing; pose blending.
- `AnimStateMachineTest` (`tests/anim`): entry events, timed blend (0.25 / 0.5 / done), exit-time firing
  tick, triggers consumed, interruption keeps weights a partition of one, uninterruptible transitions,
  priority, Any/allowSelf, sub-machines (events order, parent transition from any inner state, Exit),
  validation, pose output, determinism at a fixed step, hash-addressed parameters.
- `AnimGraphAssetTest` (`tests/anim`): byte-identical round trips, every field, file round trip, wrong
  tag / newer version / garbage refused, invalid assets still round trip.
- `AnimGraphSystemTest` (`tests/anim.scene`): component registration, asset resolution, parameter
  slots, trigger zeroing, pose reaching the skinning output, pause, instance lifetime.
- `AnimGraphEditorTest` (`tests/editor`): the structural edits and their renumbering, canvas geometry,
  both tabs' load / save / undo.

## 9. Limits

- A blend space is a cycle; it has no one-shot mode.
- Sync markers are authored by hand in the blend-space tab (the clip format has no marker chunk).
- The simulator and the probe do not draw a skinned preview; the pose path is covered by tests, not by a
  viewport in the editor.
- Layered/partial-body (per-bone) blending, additive states, root-motion extraction from states and
  state-machine notifies are not part of this feature.
- A graph entity must have, or be given, a `CAnimator`; the clip animators' notify system does not see
  a machine's clips.
- Sixteen parameter slots per entity.
