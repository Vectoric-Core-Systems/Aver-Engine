# Blackboard, behaviour-tree decorators, the BT editor and AI debug views

Part of the game-systems plan (`docs/plans/GAME_SYSTEMS_PLAN.md`, features 2 and 4). Builds on Synapse
(`docs/SYNAPSE.md`): the evaluator is still pure, the scene join is still `Aver.Synapse.Scene`.

| Piece | Where |
|---|---|
| Typed blackboard (pure) | `modules/synapse/include/aver/synapse/Blackboard.hpp`, `src/Blackboard.cpp` |
| Decorators, observer aborts, trace | `Bt.hpp`, `src/Bt.cpp` (same files as the evaluator) |
| `.ocbt` asset: schema + decorators + comments | `BtAsset.hpp`, `src/BtAsset.cpp` |
| Per-entity boards, team boards, relay, hearing sink | `modules/synapse.scene/.../SynapseBt.hpp`, `src/SynapseBt.cpp` |
| C ABI | `modules/framework/include/aver/framework/framework_blackboard_abi.h`, `src/BlackboardAbi.cpp` |
| C# API | `scripting/csharp/Aver.Framework/Blackboard.cs` (`entity.Blackboard`) |
| Visual BT editor tab | `sandbox/src/BtGraphEditor.hpp/.cpp` |
| AI debug overlays | `modules/synapse/include/aver/synapse/AiDebug.hpp` (feed), `sandbox/src/AiDebugOverlay.hpp/.cpp` |
| Tests | `tests/synapse_bt/` |

## 1. The blackboard

Keys have a name, a type (`Bool`, `Int`, `Float`, `Vec3`, `String`, `Entity`), a default and a scope.

- **Agent scope**: the value lives on the agent's own board. Two agents running the same tree do not see
  each other's `Ammo`.
- **Shared scope**: the value lives on a *team board*. Every agent whose `CSynapseBehavior::teamId` names
  that team reads and writes the same value. Team `0` is the default board; `BtSystem::setTeam(world, e,
  "red")` (or `entity.Blackboard.SetTeam("red")`) moves an agent, and its shared keys follow it.

Writes convert between `Bool`/`Int`/`Float`/`Entity` (a `Float` into an `Int` key truncates) and refuse
everything else (a `String` into an `Int` key returns false and changes nothing). A write of the value
already stored is not a change.

**Change tracking.** Every real change bumps a process-wide stamp for that key. Two things use it:

- `Blackboard::observe(key, fn)` calls back synchronously with the old and new value. A shared key
  observed through an agent board registers on the team board, so it fires for any teammate's write;
  observers survive a team change and unregister when the board is destroyed.
- Behaviour-tree decorators compare stamps once per tick (cheap: no callback lifetimes inside the tree).

**Reserved keys.** `BtSystem::tick` mirrors perception into the board when the tree declares them:
`Target` (Entity), `CanSeeTarget` (Bool), `TimeSinceSeen` (Float). `BtHearingSink` (install with
`hearingSystem().setMemorySink(&btSystem().hearingSink())`) mirrors the strongest hearing memory:
`HasHeard`, `HeardPosition`, `HeardLevel`, `HeardTag`, `HeardSource`, `HeardConfidence` (defined on the
fly, so a tree can simply use them). A forgotten memory that is the mirrored one clears `HasHeard`.

## 2. Decorators and observer aborts

A **decorator** is a condition on one key (`key op value`, with `is set` / `not set`) attached to any
node. All of a node's decorators must pass for the node to be *entered*; a node that was already running
is not re-checked unless an abort mode says so. A missing board or undefined key counts as false.

| Abort mode | Behaviour |
|---|---|
| None | checked on entry only |
| Self | while the node runs, the condition turning **false** aborts its subtree; the parent moves on in the same tick |
| Lower Priority | the condition turning **true** while the node is not running aborts a running branch that sits *later* under a common Selector/Sequence, and evaluation restarts at this node |
| Both | both |

"Later" is child order, so **order is priority**. Parallel ancestors are skipped (their children all run).
An abort calls the running action's optional abort callback (`BtRegistry::registerAction(name, fn, user,
onAbort)`) so it can stop what it started. Flips are detected per decorator between ticks, so a key that
stays false does not abort again. Up to 16 decorators per node take part in aborts.

Leaf helpers on the same expression syntax (`key`, `!key`, `key <op> value`; strings keep spaces):
`BbCompare` (Condition), `BbSet` (`Ammo = 5`), `BbClear` (resets to default). `BtSystem` installs them
through `registerBuiltinBehaviors`; a bare `BtRegistry` needs `setBoardResolver` then
`registerBlackboardLeaves()`.

`BtRunningState` carries `abortCount`, `lastAbortedNode` and, when `trace` is on, a `BtNodeTrace` per node
(visited, last result, tick, refused-by-decorator) for the editor.

## 3. The asset

Still `.ocbt`: an AVR1 container whose `BNOD`/`STRT` chunks are exactly the old format, plus optional
chunks the old readers ignore: `BBSC` (schema), `BDEC` (decorators by node index), `BNEX` (per-node
comments). A plain `.ocbt` loads as an asset with an empty schema; an asset with no extras writes
byte-identical to the old writer; the old `fmt::loadOcBt` and `BtEditor` still open a rich file (and
`BtEditor` drops the extras if it re-saves). Corrupt extra chunks fail the load instead of half-loading.
`BtSystem::loadTree` reads this format; `reloadTree` swaps a re-saved tree in, restarts its users and
re-binds their schema.

## 4. The editor tab

`BtGraphEditor` (`.ocbt`): a top-down node canvas (pan by dragging empty space, Ctrl+wheel zooms,
right-click a node for add/delete/move/decorator), execution order shown beside each child. The side panel
has three tabs:

- **Node**: kind, name, parameters, expression (with parse errors for `Bb*` leaves), comment, decorators
  (key, condition, value, observer abort), attach-to and reorder.
- **Blackboard**: the schema table (rename follows into decorators and `Bb*` text, type, scope, default,
  remove). With a debug entity a **Live** column shows the values and flashes keys that just changed.
- **Debug**: pick an entity using this tree (Play). The canvas then shows, per node, the last result
  (`run` / `ok` / `fail` / `blocked`), a thick border on nodes running this tick, bright edges for the path
  that ran, and decorator lines green/red by their current truth. Root result, tick count, observer aborts
  and the last aborted node are listed; **Running now** selects a node.

Edits are snapshot-undoable, Save writes the asset and reloads the tree in the running scene. Warnings
(unknown keys in decorators, type mismatches, bad `Bb*` text) show in the toolbar.

## 5. AI debug overlays

`synapse::AiDebugSink` (header-only) is a list of world-space lines and labels by category (sight, hearing,
path, steering, BT state, cover). `aiDebugCollect(options)` clears it, runs built-in feeders and then every
provider registered with `AiDebugSink::addProvider`, so a new AI system shows up by registering one
function. Built-in feeders: sight cones (green while the target is seen, line to it), the current waypoint
arrow and goal circle, hearing range and the last heard position (fades with confidence), crowd
desired/actual velocity arrows and stand-off marker, and BT state above heads (tree, root result, running
leaves, first blackboard values). The full baked path is private to `AgentSystem`, so the path overlay is
the current leg plus the goal.

## 6. Aver Node (graph) nodes

Pure reads and exec writes, `key=` carrying the key name (`PinType` has no String), all calling the
`BlackboardGraph` helpers in `Blackboard.cs` through the `aver_fw_bb_*` relay:

| Node | Pins |
|---|---|
| `GetBlackboardFloat` / `Int` / `Bool` / `Entity` | `entity:int` in; `value`, `success:bool` out |
| `GetBlackboardVec3` | `entity:int` in; `x`, `y`, `z`, `success` out |
| `SetBlackboardFloat` / `Int` / `Bool` / `Entity` | `exec`, `entity:int`, `value` in; `then`, `success` out |
| `SetBlackboardVec3` | `exec`, `entity`, `x`, `y`, `z` in; `then`, `success` out |

`success` is false when the key is missing or the value cannot convert (a read of a missing key is not an
error, and gives zero). Entity-typed keys use `int` pins (handles are ints).

## 7. Limits

- `MoveTo` has no abort callback (the agent keeps its goal when its branch is aborted); register your own
  action with `onAbort` if you need to cancel.
- Decorator values compare against constants, not other keys.
- Shared keys of one name must have one type per team; a clashing definition is refused.
- Team boards are never destroyed while agents use them; `BtSystem::clearBlackboards()` drops boards
  before teams.
