# Synapse: steering, crowds, hearing, cover and squads

Part of the game-systems plan (`docs/plans/GAME_SYSTEMS_PLAN.md`, features 3, 4 runtime and 5). General
building blocks only: nothing here knows about health, weapons or factions. Everything ships three ways:
a C++ API, a C ABI with C# wrappers, and `AN_` graph nodes, plus behaviour-tree actions for the pieces a
tree drives.

**Status.** Written without a build: nothing in this page has been compiled or run. The unit tests listed at
the end are the first thing to run.

## Where it lives

| Piece | Target | Pure? |
|---|---|---|
| Steering, ORCA, `CrowdSim`, hearing maths and memory, cover/squad maths | `Aver.Synapse` (`modules/synapse`) | yes: no scene, no RHI, no physics |
| `CSynapseCrowd`, `CSynapseHearing`, `CSynapseCoverMarker`, `CSynapseSquad`, their systems, `SynapseAi`, BT actions | `Aver.Synapse.Scene` | scene (physics optional, for hearing occlusion) |
| GPU crowd backend | `Aver.Synapse.Gpu` (`modules/synapse.gpu`) | RHI only |
| C ABI | `Aver.Synapse.Abi` (`modules/synapse.abi`, a DLL) | |
| C# | `scripting/csharp/Aver.Synapse` (`Crowd`, `Hearing`, `Cover`, `Squad`, `SynapseAiGraph`) | |

**One instance.** The `SynapseAi` object that owns the three systems is a process singleton that lives in
`Aver.Synapse.Abi` and nowhere else. A host that also links `Aver.Synapse.Scene` statically must drive it
through `aver_syn_ai_tick` (or `aver_syn_ai_instance`) instead of building its own, or C# would talk to a
different crowd than the one being ticked. Tests and tools may construct their own `SynapseAi`.

Frame order: `AgentSystem::tick` (waypoints), `aver_syn_ai_tick` (hearing, then cover/squads, then crowd),
then the behaviour-tree tick.

## Steering

`Steering.hpp` returns a **desired velocity** (cm/s) per behaviour: `steerSeek`, `steerArrive` (slow radius,
stop radius), `steerFlee` (panic radius), `steerWander` (deterministic per rng seed), `steerSeparation`,
`steerPathFollow` (a polyline; slows into the last point). `blendSteering` is a weighted sum clamped to max
speed; `accelerateToward` limits the change per step.

`CSynapseCrowd.mode` picks one for an entity: `FollowAgent` (the default) heads for the `CSynapseAgent` path
target while it is `Pathing`, slowing into the goal on the last leg; `Seek`, `Arrive`, `Flee`, `Wander`,
`Hold` use the component's steer fields. That desired velocity is the crowd solver's preferred velocity, so
navmesh path following and avoidance are one chain: path follower picks the waypoint, steering picks the
speed, avoidance bends it.

## Crowds

### The solver

ORCA (optimal reciprocal collision avoidance, van den Berg et al. 2011) over a uniform grid
(`CrowdOrca.hpp`, `Crowd.hpp`). Each agent builds a half-plane per neighbour (nearest `maxNeighbors`) and per
nearby wall, then a small linear program picks the velocity closest to its preferred one that satisfies
every half-plane. Walls are the unwalkable cells of the baked `.ocnav` grid that border walkable ones; they
are hard constraints, never relaxed. Motion along a wall is free; closing on it is limited to what the agent
can stop within `obstacleHorizon` seconds; an agent already inside the radius is pushed out.

Velocity avoidance alone does not keep a dense doorway clear, so every fixed step ends with a
position-based contact pass (`contactIterations`): overlapping pairs are pushed apart, then agents are
pushed out of wall cells. The wall pass runs last, so a squeezed agent ends in the corridor.

### Not deadlocking in narrow spaces

- **Sidestep bias.** A preferred velocity is nudged a few percent in one rotational direction, per-agent
  jittered by id. Exactly head-on pairs would otherwise cancel perfectly and stop; with the bias they part
  on opposite sides.
- **Stand-off yielding.** An agent moving slower than `stuckSpeedFrac` of its preferred speed for
  `stuckSeconds` is "stuck". When two stuck agents face each other, the higher `priority` (ties: lower id)
  takes 20% of the avoidance and the other 80%, so one gives way instead of both waiting.
- **Idle agents are not walls.** Agents standing still still share avoidance (50/50); only `Hold`
  (static) agents force the full burden onto others.

### Choosing the backend and size

| Setting | Meaning |
|---|---|
| Backend `Cpu` (default) | ORCA over a spatial grid on the CPU. For hundreds of agents. **Deterministic** at the fixed step. |
| Backend `Gpu` | The same solve in a compute shader, one thread per agent, for thousands. Needs the host to install a `GpuCrowdBackend`; falls back to `Cpu` while none is available. |
| Max agents (`CrowdParams::maxAgents`, `aver_syn_crowd_set_max_agents`) | Hard cap on simulated agents. Past it the highest entity ids are left out and report `active = 0`; `overflowCount` says how many. Default 1024. |
| Drive `Advise` (default) | The crowd only publishes `velXCm`/`velYCm`; a character controller moves the entity (`AN_CrowdSteer` feeds `CharacterMove`). The entity position is the truth. |
| Drive `Move` | The crowd writes the entity's local position (z from the nav floor). For large crowds with no physics capsules, and the only sensible mode for thousands. Parented entities are not supported (local position is written). |

The simulation steps at a fixed `fixedStep` (1/30 s) from an accumulator, at most `maxStepsPerAdvance`
steps per frame.

**CPU determinism.** Agents are added in entity-id order, visited in index order, neighbours taken in
(distance, index) order, and nothing reads another agent's new velocity. The same scene at the same step
gives bit-identical positions in the same binary; `CrowdTest` checks it. It is not a cross-compiler promise.

**GPU trade-offs.** The CPU packs agents and a grid, the shader returns velocities, the CPU reads them
about three frames later. Planning is therefore a few frames old and results are not bit-identical to the
CPU's (neighbour and wall selection use insertion instead of a sort). The contact pass still runs every step
on the CPU, so agents do not overlap; only the planning lags. At most 16 neighbours and 8 wall lines per
agent. The shader (`synapse_crowd.hlsl`) is compiled at runtime: a green build proves nothing about it.

### Limits

2.5D like the grid: one floor per cell, no vertical separation. Walls are grid cells, so avoidance is as
coarse as the bake (50 cm). Group formations and flow fields are not here.

## Hearing

Pure model (`Hearing.hpp`): a `NoiseEvent` has a position, a **loudness** (the audible radius in cm for a
listener of sensitivity 1 in the open), a game-defined `tag` and a `source` entity. Level falls off linearly
from 1 at the source to 0 at the radius. A listener scales the radius by `sensitivity`, is capped by
`maxRangeCm`, and filters tags with `tagMask` (bit `tag & 31`). Each occluder on the line from noise to ear
multiplies the radius by `occlusionFactor` (default 0.5). Cheap rejects (tag, range) run before the
occlusion query.

In a scene (`CSynapseHearing`, `HearingSystem`): anything calls `emit` (or `aver_syn_emit_noise`, or
`AN_EmitNoise`); the next tick, each listener decides what it heard. Occlusion defaults to physics raycasts
re-cast past each hit, up to four occluders, with 10 cm trimmed from both ends so the source and the
listener's capsule do not count. A host can replace it with `setOcclusion`. A source never hears its own
noise. The queue is bounded (256) and counts drops.

### Memory

Each listener keeps up to four last-known-position entries (`HearingMemory`). A noise with the same source and
tag refreshes an entry in place; sourceless noises of one tag within 150 cm merge; a full list replaces the
weakest. **Confidence** falls linearly from 1 to 0 over the listener's `memorySec` (default 8 s), then the
entry is dropped. The strongest entry (confidence, then level, then newest) is mirrored on the component
(`hasMemory`, `heardXCm/YCm/ZCm`, `heardLevel`, `heardTag`, `heardSource`, `confidence`, `timeSinceHeardSec`,
`heardCount`). `HearingMemory::searchRadiusCm` grows with age for "search around the spot" behaviours.

### The blackboard seam

`IHearingMemorySink` is what the blackboard agent binds to:

```cpp
class IHearingMemorySink {
    virtual void onHeard(u32 listener, const HeardMemory& m) = 0;      // new or refreshed
    virtual void onForgotten(u32 listener, const HeardMemory& m) = 0;  // decayed to zero
};
hearingSystem.setMemorySink(&mySink);   // via SynapseAi::hearing(), or aver_syn_ai_instance()
```

`listener` is the entity id. `HearingBlackboardSink` (`modules/synapse.scene/include/aver/synapse/
HearingBlackboardSink.hpp`) is a ready adapter that writes a listener's strongest memory into its
`Blackboard`: `Heard.Valid` (bool), `Heard.Position` (vec3), `Heard.Level`, `Heard.Tag`, `Heard.Source`
(entity), `Heard.Confidence`. Define the keys with `HearingBlackboardSink::defineKeys(board)`, give it a
resolver from listener entity to board, and `setMemorySink(&sink)`. Writes go through `Blackboard::set`, so
the board's observers (and decorator aborts) fire on a change. Forgetting an entry clears `Heard.Valid` only
when it is the entry currently shown.

One sink at a time: `setMemorySink` replaces. C and C# hosts get the same events through
`aver_syn_hearing_set_memory_callback`, which installs its own sink (so use one or the other).
"OnHearNoise" also fires through a notify sink with `PerceptionSystem`'s `NotifyFn` signature.

## Cover and tactics

### Cover points

A `CoverPoint` is where an agent stands; `dir` points from it at the obstacle that protects it. It protects
against a threat when (1) the threat lies inside its arc around `dir` (`arcHalfAngleDeg`, default 70), and
(2) the straight line from threat to point is blocked. The line check defaults to the nav grid
(`navSegmentBlocked`: a blocked cell strictly between the end cells) and can be replaced by a host function
(a physics raycast).

Authored: put a `CSynapseCoverMarker` on an entity (`aver_syn_cover_marker_attach`). Its position is the
standing point and its local **+X axis points at the cover**. Markers are kept in step every tick with stable
ids: moving one moves its point, disabling or destroying it removes the point and any claim on it.
`Cover.Add` / `aver_syn_cover_add` adds a point with no entity.

Generated: `setAutoGenerate(true)` derives points from the nav grid next to walls (a walkable cell with a
blocked 4-neighbour, plus outside corners where only a diagonal is blocked). Opposite walls cancel (a
corridor has no useful side). Points are at least `minSpacingCm` apart and capped by `maxPoints`. The grid
has no heights, so generated points are full-height; an optional `isWall` probe can confirm a blocked
neighbour is a real wall rather than a hole. Regeneration changes generated ids and clears claims on them.

### Queries and reservations

`CoverMap::query` returns the cheapest enabled point that protects, is not reserved by someone else, is not
nearer the threat than `minThreatDistCm`, does not bring the seeker `maxCloserCm` closer to the threat, and
costs at most `maxSeekCm` to reach. Cost is the straight distance, or a path cost: the system's default is a
`findPath` length (capped at 1500 expansions) and runs only on the `maxPathCalls` (12) nearest candidates; a
host can replace it. `claim` is query plus reserve in one step, so two seekers are never handed one point.
An owner holds one point at a time (reserving moves its claim); claims can expire after a time; a dead
owner's claims are dropped on the next tick.

### Squads

Put `CSynapseSquad` (squad id, spacing) on members and give the squad a target (`setSquadTarget`). Each tick
assigns roles by distance to the target: nearest is the **Anchor** (holds position), the next two
**FlankLeft** / **FlankRight** (slots on a circle around the target, +-`flankAngleDeg` either side of the
bearing from the target to the squad centre), the rest **Support** (behind the anchor, spread sideways at
`spacingCm`). Role and slot are written on the component. `squadSpacingPush` gives the step that restores
spacing from squad-mates.

## Behaviour-tree vocabulary

`SynapseAi::registerBehaviors(BtRegistry&)` (`aver_syn_ai_register_behaviors`) adds these names. They move
agents by writing the path follower's goal, as `MoveTo` does, so they need a `CSynapseAgent`.

| Name | Kind | Parameters / behaviour |
|---|---|---|
| `HeardNoise` | Condition | strongest memory's confidence > `params[0]` |
| `HeardNoiseTag` | Condition | a memory with tag `params[0]` |
| `InCover` | Condition | holds a claim and is within `params[0]` cm (default 80) of it |
| `SquadRoleIs` | Condition | squad role == `params[0]` (`SquadRole`) |
| `InvestigateNoise` | Action | paths to the strongest remembered noise; Running until arrival, Failure if it fails or nothing is remembered |
| `TakeCover` | Action | `[0]` threat: 0 seen target (falls back to heard), 1 heard noise (falls back to seen); `[1]` max seek cm; `[2]` min threat distance cm; `[3]` 1 = full-height only. Claims a point, paths to it, Running until arrival; a failed path frees the claim |
| `LeaveCover` | Action | releases the claim |
| `SquadFlank` | Action | paths to this member's slot; Running until arrival |
| `SquadKeepSpacing` | Action | steps out of squad-mates' circles; Success when clear |
| `SquadSetTarget` | Action | `params[0..2]` target for this member's squad |
| `CrowdSetMode` | Action | `[0]` `CrowdMode`, `[1..3]` steer target |

## C ABI and C#

`modules/synapse.abi/include/aver/synapse/synapse_ai_abi.h` (`aver_syn_*`): lifecycle (`register`, `tick`,
`register_behaviors`, `instance`, `reset`), crowd (attach, configure, mode, velocity, backend, max agents,
drive, gpu backend), hearing (attach, configure, emit, get, forget, memory callback), cover (add, remove,
marker, auto-generate, find, release, is_covered) and squads (attach, target, slot, spacing push).

C#, `Aver.Synapse` (a leaf assembly; entities are plain ints): `Crowd`, `Hearing`, `Cover`, `Squad`, and
`SynapseAiGraph` for the nodes. Its native library is `Aver.Synapse.Abi`, named apart from the assembly so no
`NativeResolver` is needed.

```csharp
Crowd.Attach(e);
Crowd.Configure(e, radiusCm: 34, maxSpeedCm: 350);
Crowd.SetMode(e, CrowdMode.Arrive, x, y, z);
Crowd.Backend = CrowdBackend.Gpu;      // falls back to CPU until the host installs one
Crowd.MaxAgents = 4000;
Hearing.EmitNoise(x, y, z, loudnessCm: 1200, tag: 1, sourceEntity: shooter);
if (Cover.Find(e, tx, ty, tz, out int id, out float cx, out float cy)) { /* path to (cx, cy) */ }
```

## Graph nodes (`AN_`)

| Node | Kind | Pins (inputs -> outputs) |
|---|---|---|
| `AN_CrowdSetAgent` | exec | entity, radiusCm, maxSpeedCm, maxAccelCm, priority -> success |
| `AN_CrowdSetMode` | exec | entity, mode, x, y, z -> success |
| `AN_CrowdSetBackend` | exec | backend (0 CPU, 1 GPU), maxAgents (-1 keeps) -> success |
| `AN_GetCrowdVelocity` | pure | entity -> vx, vy, speed, success |
| `AN_CrowdSteer` | pure | entity, dt, turnRate, maxSpeedCm -> forward, right, yawDelta, success (feeds `CharacterMove`) |
| `AN_EmitNoise` | exec | x, y, z, loudnessCm, tag, source -> success |
| `AN_SetHearing` | exec | entity, sensitivity, maxRangeCm, memorySec -> success |
| `AN_GetHeard` | pure | entity -> heard, x, y, z, level, tag, confidence, timeSince, success |
| `AN_FindCover` | exec | entity, threatX/Y/Z, maxSeekCm, minThreatDistCm -> found, x, y, coverId, success |
| `AN_ReleaseCover` | exec | entity -> success |
| `AN_IsCovered` | pure | entity, threatX/Y/Z -> covered |
| `AN_SquadJoin` | exec | entity, squadId, spacingCm -> success |
| `AN_SquadSetTarget` | exec | squadId, x, y, z -> success |
| `AN_GetSquadSlot` | pure | entity -> role, x, y, z, success (false until the squad has a target) |

Definitions: palette entries in `sandbox/src/GraphNodeDefsSynapseAi.hpp`; bodies in
`SynapseAiGraph.<Name>ForGraph`; reflection targets in `Aver.Framework/GraphInteropSynapseAi.cs`, one method
per node, signature = input pins in order, then outputs as `out` parameters, `success` as the return value.

## Tests

`tests/synapse/SynapseAiTests.cmake` adds five executables:

- `SteeringTest`: every behaviour, blending, the acceleration limit, wander determinism.
- `CrowdTest`: ORCA primitives; two agents swapping ends of a corridor; four against four in a corridor two
  agents wide; **200 agents through a 200 cm doorway** with a worst-overlap bound and a throughput bound;
  determinism (bitwise); max-agents, backend fallback with a fake GPU backend, fixed-step advance.
- `HearingTest`: range and falloff, sensitivity, tag masks, occlusion, memory merge/decay/replacement.
- `CoverTest`: line and arc protection, generation validity, queries, **reservation conflicts and expiry**,
  roles, flank, spacing, support slots.
- `SynapseAiSceneTest`: against a real `scene::World` with a fake occlusion: listeners, sink and notify,
  crowd in both drive modes and under a cap, markers and claims that follow their entities, squads, and the
  BT actions driving the path follower.

The GPU path has no unit test: it needs a device. It has a CPU reference (`CrowdCpuSolver`) to compare
against once a device test exists.
