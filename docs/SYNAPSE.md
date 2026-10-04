# Aver Engine — Synapse: navigation, perception and behaviour

Synapse is the engine's AI system. Before it there was nothing: `grep -rniE
"navmesh|pathfind|behaviou?rtree|steering"` over `modules sandbox scripting` returned no hits outside
vendored Jolt, so nothing in the engine could move toward a target or avoid an obstacle.

It is built as three tiers, and the split is the most important thing to understand about it:

| Target | Kind | Depends on | Owns |
|---|---|---|---|
| `Aver.Synapse` | STATIC, **no scene** | `Aver.Core`, `Aver.Formats` | the grid, A\*, and the behaviour-tree evaluator |
| `Aver.Synapse.Scene` | STATIC | `Aver.Synapse`, `Aver.Scene`, *(optional)* `Aver.Physics` | components, per-frame ticks, perception |
| the composition roots | — | everything | who the target is, and where events go |

**`Aver.Synapse` has no `scene::World` in it at all, deliberately.** That lets the entire
pathfinder be tested against a grid drawn in ASCII, and the entire behaviour-tree evaluator against a
hand-built tree and two fake callbacks — on a machine with no GPU, no physics and no world. See
`tests/synapse/src/NavTest.cpp` and `BtTest.cpp`: neither links `Aver.Scene`. The same split, for the
same stated reason, as `Aver.Anim` / `Aver.Anim.Scene`.

---

## 1. Navigation is a baked grid, not a navmesh

`.ocnav` is a **2.5D uniform grid** — one cell per 50 cm square of the XY plane, each carrying the
floor height under it, whether an agent fits, and which connected region it belongs to
(`modules/formats/include/aver/formats/OcNav.hpp`).

A polygonal navmesh was not chosen because this engine has no voxelisation, contour tracing or triangulation code anywhere — a polygon mesh would be a project of its own. A grid is cheap to bake, cheap to search, trivial to draw as an overlay, and its weakness (coarseness around tight geometry) is *visible* rather than subtle.

**2.5D means exactly one floor per cell.** A bridge over a path is not representable, and neither is
a second storey. That is a real limit, and `NLNK` is reserved in the format for off-mesh links.

### Baking

Baking is an **editor command, never a runtime cost** (Build ▸ Bake Navigation, or `--bake-nav`).
The bake casts one ray down per cell and one sphere cast up for headroom. Two accuracy limits matter before trusting a grid:

- Level colliders are **axis-aligned boxes built from the placement's scale, with rotation not applied** (`LevelInstance.cpp`). A rotated wall bakes as its unrotated bounding box.
- Placements marked `nocollide` — which is every tree `tools/MakeFoliage.cpp` writes — are **invisible to the bake**. Agents will walk through them.

`aver_phys_raycast` resolves its hit by linear scan over all bodies, so a bake is O(cells × bodies).
It is offline and paid once.

### Searching

`synapse::findPath(nav, request)` is 8-connected A\* with an octile heuristic
(`modules/synapse/include/aver/synapse/Nav.hpp`). It returns a **status, not a bool**:

| Status | Meaning |
|---|---|
| `Found` | a complete path |
| `Partial` | the node budget ran out; the path reaches the closest point found so far |
| `Unreachable` | start and goal are in different regions — **refused in O(1), no search run** |
| `OffMesh` | start or goal is not on (or near) the baked grid |
| `Invalid` | the grid itself is unusable |

The region id is what makes `Unreachable` free. Two cells with different ids have no path between
them. `regionId` is a **u16, not a u8** — 255 regions is reachable in a level with many separated interiors.

Returned paths are **string-pulled**: consecutive cells in a straight clear line collapse into one
segment.

---

## 2. Synapse advises, it does not move

This is the load-bearing design rule of the whole system.

`AgentSystem::tick` tracks a path and writes the agent's **current waypoint** onto its `CSynapseAgent` component. It never calls `setLocalPosition`, never touches a transform, never drives a capsule.

It could not: `AverCharacter._capsule` is a private C# int with no accessor, and there is no character API on the C ABI at all. Movement is expressed where the author can see it — as nodes:

```
GetSynapseTarget(entity) ──► x, y, z
                              │
SynapseSteer(entity, dt, target, turnRate, arriveRadius) ──► forward, right, yawDelta
                              │
                        CharacterMove(entity, dt, forward, right, yawDelta, pitchDelta)
```

`SynapseSteer` takes an **explicit target**, never `CSynapseAgent`'s own. The identical node does direct chase (target = a seen enemy's live position) as readily as path-following (target = `GetSynapseTarget`'s output).

Its outputs match `CharacterMove`'s existing contract: `forward`/`right` are **−1..1 axis intent** relative to current facing (scaled by `MoveSpeed`), and `yawDelta` is a **per-frame degree delta**. `right` is always 0 in v1, and `forward` eases toward 0 as misalignment grows.

**No local avoidance in v1.** Two agents in a doorway resolve through Jolt capsule collision.

An agent that cannot reach its goal **gives up after exactly one query**. `Unreachable`/`OffMesh`/
`Invalid` set status `Failed` once and are never retried.

---

## 3. Perception: a cone, then one ray

`CSynapsePerception` (`modules/synapse.scene/include/aver/synapse/SynapsePerception.hpp`) carries a
sight range, a cone half-angle, a think interval and an eye height.

The check is **cheap first, expensive second**: a dot-product cone test, and only if that passes, one occlusion raycast from the eye to the target. It is throttled per agent by its own think-interval
accumulator (default 0.2 s).

The raycast stops **10 cm short of the target** — otherwise it would report a hit against the target itself. It gates on the ray's **return value, never `outEntity`** — an occluding landscape body is ownerless.

`Aver.Synapse.Scene` links `Aver.Physics` directly (an optional CMake edge), which is allowed where linking `Aver.Framework` is not.

### Who the target is

Synapse must not link `Aver.Framework`, so it takes a **host-installed function pointer** instead — `PerceptionSystem::setTargetResolver` — and each composition root installs
`aver_fw_controlled_pawn(aver_fw_player_controller(0))`. Resolved **once per tick**, not once per agent.

---

## 4. Behaviour trees are a native asset

A Synapse behaviour is a `.ocbt` file with its own editor tab — **not** an Aver Node graph. A
behaviour tree's advantage over a graph is that *running state persists across ticks*.

`.ocbt` is an AVR1 container of node records, each with a kind, a parent index and parameters
(`modules/formats/include/aver/formats/OcBt.hpp`). Children are found by scanning for nodes whose
parent is a given index; **parents always precede their children**. Node 0 is always the root.

| Kind | Behaviour |
|---|---|
| `Selector` | first child to Succeed wins; Failure only once every child fails |
| `Sequence` | first child to Fail wins; Success only once every child succeeds |
| `Parallel` | every child ticked every tick; Failure if any fails, Success once all succeed |
| `Inverter` | single child; Success ↔ Failure swapped, Running passes through |
| `Succeeder` | single child; always Success once resolved |
| `Cooldown` | single child; refuses to re-run for `params[0]` seconds after a resolved outcome |
| `Condition` | a leaf, resolved by NAME; Success or Failure only |
| `Action` | a leaf, resolved by NAME; may report Running |

**A composite resumes at the same child it was Running on**, rather than restarting its subtree. That state lives in `BtSystem`'s own side table, keyed by the full entity handle and pruned every tick.

### Conditions and actions are named

Leaves resolve through a registry the host installs. Seven built-ins ship (`registerBuiltinBehaviors`):

| Name | Kind | Reads |
|---|---|---|
| `HasTarget` | Condition | `CSynapsePerception.lastKnownTargetEntity` |
| `CanSeeTarget` | Condition | `CSynapsePerception.canSeeTarget` |
| `DistanceToTargetLess` | Condition | `params[0]` = threshold, cm |
| `MoveTo` | Action | `params[0..2]` = world goal; issues one `AgentSystem::setGoal` |
| `Wait` | Action | `params[0]` = seconds |
| `LookAt` | Action | turns to face the last-known target |
| `FireEvent` | Action | `stringParam` = the event name to raise |

An unregistered name is a **warning and a Failure**, never a crash.

---

## 5. Events go through the animation-notify seam

`OnSeeTarget` and the `FireEvent` action both reach a graph through **the same seam animation
notifies use** — `ScriptHost::graphFire`, via a sink each composition root installs. There is no second wire.

`PerceptionSystem::NotifyFn` and `BtSystem::NotifyFn` are byte-for-byte
`aver::anim::AnimNotifyFn`'s signature, and both roots install the *same* `animNotify` method they
already had.

**`graphFire` carries no payload.** It is `(entity, eventName)` only, so `OnSeeTarget` cannot tell a graph *which* entity it saw. Use `GetSynapsePerception` to read agent state.

`OnSeeTarget` fires **exactly once per acquisition** — the tick `canSeeTarget` flips false→true, never while it stays true.

### Why Synapse never links Aver.Framework

Three seams exist because of this one rule:

- **Synapse asks the host:** `PerceptionSystem::setTargetResolver` (who is the target),
  `setNotifySink` (where events go). Function-pointer types declared in Synapse's own headers.
- **The host asks Synapse:** `aver_fw_synapse_target` and `aver_fw_synapse_perception`, relays in
  `framework_abi.h`, so `GetSynapseTarget` / `GetSynapsePerception` can read agent state.

Only a composition root links both sides. Everything else stays testable in isolation.

---

## 6. Where things live

| Path | What |
|---|---|
| `modules/formats/…/OcNav.hpp`, `OcBt.hpp` | asset formats |
| `modules/synapse/…/Nav.hpp`, `NavBake.hpp`, `Bt.hpp` | grid, A\*, bake, evaluator — all pure |
| `modules/synapse.scene/…/SynapseAgent.hpp` | `CSynapseAgent` + `AgentSystem` |
| `modules/synapse.scene/…/SynapsePerception.hpp` | `CSynapsePerception` + `PerceptionSystem` |
| `modules/synapse.scene/…/SynapseBt.hpp` | `CSynapseBehavior` + `BtSystem` + built-ins |
| `sandbox/src/NavBakeCommand.*` | editor bake command and overlay |
| `sandbox/src/BtEditor.*` | `.ocbt` editor tab |
| `tests/synapse/`, `tests/editor/src/BtEditorTest.cpp` | seven suites: `NavTest`, `BtTest`, `NavBakeTest` (pure `Aver.Synapse`, no `Aver.Scene`), `BtEditorTest` (links `Aver.Formats`/`Aver.Core`/`Aver.Platform` only), plus `AgentTest`, `PerceptionTest`, `BtSystemTest` (all link `Aver.Scene`) |

Components are registered dynamically at each composition root, so no edit to
`Components.hpp`, `scene_abi.h` or `kComponentBuiltinMax` is needed, and no ABI version bump occurs.

**Attach components through `attach()`, not `world.addComponent`.** `ComponentPool::add` zero-fills bytes and never runs a constructor, so `attach()` assigns a fresh value over the zeroed bytes.

---

## 7. What is not built

- **No local avoidance.** Agents push through each other via Jolt.
- **No off-mesh links.** No jumping, ladders or dropped ledges; `NLNK` is reserved, unwritten.
- **No runtime nav baking.** A level with no `.ocnav` beside it has no navigation.
- **No hearing.** `MakeNoise`/`OnHearNoise` were designed and not built; only sight exists.
- **No node canvas in the `.ocbt` editor.** It is a tree view plus a parameter panel; reparenting is a combo box.
- **One target, globally.** The resolver answers per tick, not per agent.
