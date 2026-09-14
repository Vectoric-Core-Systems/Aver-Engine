# Aver Node

A graph you build in the editor can, since `41d6566`, declare itself an actor class and drive a
real, level-placed entity — no C# involved at all. This page is the guide: what the system is, how
its class model works, how it maps to Blueprints for readers who know them, and a worked example
you can run verbatim. For the full per-node pin catalog, see
**[the node reference](AVER_NODE_NODES.md)** — this page links to it rather than repeating it. For
the `.ocgraph` file grammar itself — every record's exact syntax, and where the C++ and C# readers
disagree — see **[`formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md)**; this page is about
what a graph *means*, that one is about what a graph *says*.

**This page is specifically about the GAMEPLAY graph.** Since the `DOMAIN` record (§1), `.ocgraph`
is a shared grammar for two unrelated languages — gameplay, described here, and a `DOMAIN material`
graph that shades a surface instead of driving an actor, described in
**[`MATERIALS.md`](MATERIALS.md)**. Everything below this point — the class model, `PARAM`/`VAR`,
entry points, the Blueprint mapping — is gameplay-only and does not apply to a material graph.

Every claim below was checked against the parser, the compiler, the bridge and the level-loading
code that actually run it, not against an earlier design doc — several already turned out to be
stale (see the note at the bottom). Where this page shows a `.ocgraph`/`.ocworld` fragment, it is
lifted from a real file in `test-content/GraphDemo`, not invented for the example.

**A pass against the tree found most of this page's specific `File.cs:NNN` citations pointing at the
wrong content** — normal line drift from ordinary commits to `GraphHost.cs`, `OcGraphParser.cs`,
`HostBridge.cs`, `Graph.cs` and `OcWorld.cpp` over time, not any one dramatic edit. The ones this
pass re-derived now carry a corrected number and say what they used to cite; a handful of the less
load-bearing ones were left with the file/symbol only, since finding their exact current line was
not worth the churn against files that will keep moving. Treat every number below as a
last-known-good pointer, not a promise — grep the named symbol if it doesn't land where expected.

---

## 1. What Aver Node is

`.ocgraph` is a line-based text format, in the same family as `.ocworld`/`.ocmap` — diffable,
hand-editable, and read by both a C++ reader (`modules/formats/src/OcGraph.cpp`) and a C# one
(`Aver.Graph/OcGraphParser.cs`). A graph is nodes, typed pins (`float`/`int`/`bool`/`exec`) and
`LINK` records between them; the editor's canvas (`sandbox/src/GraphNodeDefs.hpp` drives its
palette) is one way to produce that text, but the text is the format, not the canvas.

**A graph is now written in one of two entirely different LANGUAGES, and that is a separate axis
from the SHAPE described below.** An optional `DOMAIN <name>` record, at most one per file and
written directly under the `OCGRAPH` magic, says which:

- **`DOMAIN gameplay`, or no `DOMAIN` record at all** — everything this page describes. Nodes call
  the framework; `GraphCompiler.cs` compiles the graph to CLR IL.
- **`DOMAIN material`** — a different vocabulary entirely, where nodes are arithmetic on a surface
  (UV, world position, texture samples, a BRDF's inputs) and the graph compiles to HLSL through a
  C++ compiler, `aver::pbr::compileMaterialGraph` (`modules/render.pbr/src/MaterialGraphHlsl.cpp`).
  See **[`MATERIALS.md`](MATERIALS.md)** — this page does not cover that vocabulary, and the class
  model, `PARAM`/`VAR`, entry points and everything else below this line do not apply to it: a
  material graph has no `CLASS`, no `ENTRY`, and is not spawnable.

**Absent means gameplay, and an unrecognised name means neither.** That asymmetry is deliberate,
not an oversight: every `.ocgraph` written before this record existed is a gameplay graph and has
to keep working untouched, so a missing `DOMAIN` cannot be an error. But a file that says
`DOMAIN sound` (say) is telling this build it is something this build has never heard of, and the
safe reading of that is "not mine" — it maps to `OcGraphDomain::Unknown` and every consumer skips
it, rather than falling back to gameplay and trying to compile a graph whose author explicitly
said it was not one. Two places open EVERY `.ocgraph` under a project without being asked
(`HostBridge.DeclareGraphClasses` and `GameApp::discoverProjectGraphs`), and both would otherwise
have reached a material graph and tried to run it as gameplay — not hypothetical, since the two
kinds of file live in the same `Content` tree. The name is kept verbatim rather than normalised
(round-trips through load/save byte for byte), so an older build opening a newer project does not
quietly rewrite a domain it does not understand. See `formats/FORMAT_SPECS.md`'s own `DOMAIN`
entry for the record's exact grammar.

**The editor has no second notion of domain.** `GraphEditor::openGraphDomain()` reads the same
`DOMAIN` record the compiler will read, so the add-node palette, the pin-type-compatibility check
and everything else the canvas offers is filtered to the vocabulary that graph's own compiler
actually accepts — a material graph's palette has no `Branch` and no `CharacterMove`, and a
gameplay graph's has none of the material vocabulary, rather than either editor offering wiring
whose only possible outcome is a compile error naming a node the palette itself suggested. See
`MATERIALS.md` for what that vocabulary is and how the two disagree even on names they share.

**Execution is direct IL, not generated C#.** `GraphCompiler` emits CLR IL straight from the graph
via `System.Reflection.Emit` — there is no `Graph → C# source → Roslyn` step anywhere in the
pipeline, and no collectible `AssemblyLoadContext` involved. Compilation happens once, inside
`GraphHost.Load()`, the first time a graph is loaded — which for a class instance is *every time
one is spawned* (§2). A shipped game never needs a C# compiler on the player's machine; it needs
this IL emitter, which is already linked in.

A graph is exactly one of two shapes, decided once from whether it has any `ENTRY` record
(`GraphHost.LoadFromText`, `GraphHost.cs:274` for the `EntryPoints.Count > 0` check itself, inside a
method starting at `:244` — drifted from an earlier `:248-254`, which named the same method but not
the check):

- **No `ENTRY` at all** — a pure dataflow graph. `PARAM entity int` / `PARAM time float` go in,
  0–3 `OUT` floats come out, pulled fresh every tick with no memory between calls. This is the
  original shape, predates everything else in this guide, and still works unchanged.
- **One or more `ENTRY` records** — an event-driven graph, walked from `OnStart`/`OnTick`/any
  other named event. This is the shape the rest of this guide is about, because it is the shape
  that can declare a `CLASS` and drive a real entity.

The two never mix in one file. A graph either computes values on request, or it runs as a small
state machine with entry points — never both.

## 2. The class model: a graph *is* a class

A top-level record turns a plain graph into a spawnable actor class:

```
CLASS AN_Orbiter Actor
```

`CLASS <name> [parentName] [mesh=<path>] [material=<name>] [view=firstperson|thirdperson] [pawn=<className>] [controller=<className>]`
(the `"CLASS"` key case, `OcGraphParser.cs:332` onward as of this pass, drifted from an earlier
`:210-270`, which now falls inside a `PARAM` default-value switch instead). This is
the Blueprint model, not a component that references a graph: **the graph file is the class
asset.** `Aver.Scripting.Bridge`'s `HostBridge` reads it and registers it through the *exact same*
`aver_fw_class_declare` / `aver_fw_class_set_flags(Managed)` / `aver_fw_class_seal` sequence a C#
`[AverClass]` type goes through (`HostBridge.cs:599-616` as of this pass, drifted from an earlier
`:427-434, 486-509`, which now falls on an unrelated `GraphHost` dictionary comment and `GraphFire`
instead) — one flat registry, so
`ActorClass.Find("AN_Orbiter")` cannot tell, and does not need to, whether what it found came from
a graph or from C#.

A few things worth knowing before you rely on this:

- **The parent defaults to `"Actor"`** if you omit it — `CLASS AN_Orbiter` alone is already a
  complete, sealable, spawnable declaration. `Actor` is one of five abstract base classes
  (`Actor`/`Pawn`/`PlayerController`/`GameMode`/`GameInstance`) the engine declares and seals at
  scripting bootstrap, *before* any project script or graph scan runs (`HostBridge.cs:771-793`) —
  so there is always something for a bare `CLASS Foo` to parent to.
- **`mesh=`/`material=` on the `CLASS` line are class *defaults***, registered as a default
  `CMeshRenderer` component on the class itself (the `ClassBuilder.Mesh(...)` call,
  `HostBridge.cs:608-609` — the previous citation, `Graph.cs:224-231`, named `GraphVariable`'s own
  persistence comment, a different file entirely; `Graph.cs` only holds the parsed `ClassMesh`/
  `ClassMaterial` strings themselves, at `:449-450`) — not the same mechanism as a
  `SetMesh`/`SetMaterial` *node*'s own `mesh=`/`material=` attribute, which is a per-invocation
  write. The class default is what a spawned instance looks like before its own `OnStart` has run
  even once; a node's write is what happens after.
- **`view=firstperson`/`view=thirdperson` on the `CLASS` line sets the camera default for a
  `Character`-parented class**, applied once per spawned instance (`HostBridge.cs`'s `DispBind`,
  the ancestor-construction branch) to `AverCharacter.CameraViewMode` — a plain C# field with no
  scene-field or node path to it otherwise, so before this existed a graph-declared character could
  not ask for a first-person camera at all; it always ran in `CameraViewMode`'s own default,
  `ThirdPerson`. Meaningless (and silently ignored, not an error) on a class whose native ancestor
  isn't `AverCharacter` — the same tolerance an unrecognised `CLASS`-line attribute already gets.
- **`pawn=<className>` and `controller=<className>` on the `CLASS` line set the default pawn and
  player-controller classes for a `GameMode`-parented class** (the second-pass loop over
  `pendingRoles`, `HostBridge.cs:670-703` as of this pass, drifted from an earlier `:597-623`, which
  now falls on the unrelated class-declare/seal sequence, and an earlier `Graph.cs:261-291`, which
  named `GraphComponent`'s own fields — `Graph.cs`'s actual `ClassPawn` field is at `:486`).
  Both attributes are **GameMode-only** — `HostBridge` warns and ignores them on any other class parent
  (`HostBridge.cs:676-682` as of this pass, drifted from an earlier `:618-623`). `pawn=` alone does nothing; `aver_fw_begin_play` possesses only when it has
  *both* a pawn and a controller handle. The built-in `PlayerController` is abstract and cannot be spawned
  directly, but a graph can declare `CLASS AN_FPController PlayerController` to create a concrete,
  spawnable controller class with the `CONTROLLER` flag inheritance. Both names are resolved at class-seal
  time against declared classes (not file paths), so a graph naming a pawn/controller whose own class had
  not been declared yet would resolve to 0 and silently possess nothing — which is why `HostBridge` applies
  these in a *second pass* after every graph class is declared, rather than inline, and warns if either
  name is unresolvable (the second-pass loop's own comment, `HostBridge.cs:664-670` as of this pass,
  drifted from an earlier `:629-639`, which now falls on an unrelated `ticks =` computation).
- **Every spawned instance gets its own, freshly loaded `GraphHost`.** Binding a class instance
  (`DispBind`, `HostBridge.cs:1293` as of this pass, drifted from an earlier `:988-1010`, which now
  falls on the unrelated `InstallGraphVarProvider`) parses and compiles the `.ocgraph` file again from disk,
  per spawn — it is not a shared compiled graph reused across instances. That cost buys the thing
  the worked example in §7 exists to prove: two instances of one class carry **independent** `VAR`
  storage automatically, because independence falls out of "two `GraphHost` objects" with no extra
  code. The named, accepted cost is re-parsing and re-JITing per spawn — fine at the tens of
  instances this slice targets, a real limitation at thousands.
- **An undeclared or misspelled parent fails almost silently.** `aver_fw_class_seal` returns 0 on a
  cycle or an unresolvable parent name, and `HostBridge` reports it as a single `WARN` naming the
  file and the parent (`HostBridge.cs:617-621` as of this pass, drifted from an earlier `:503-509`)
  — the graph is not registered as a class, and it
  is *also* not picked up by the older CLASS-less project-graph discovery, because that path skips
  any file whose text merely looks like it declares a `CLASS` (`GameApp.cpp`'s
  `ocgraphDeclaresClass` text scan). A broken parent does not degrade to old behaviour: the graph
  runs nowhere, with one log line as the only trace.
- **Parenting a graph class to another graph class is file-sort-order fragile.**
  `DeclareGraphClasses` does one linear pass over `*.ocgraph`, ordered alphabetically by path, with
  no forward-declare step (the `.OrderBy(p => p, StringComparer.Ordinal)` scan, `HostBridge.cs:566`
  as of this pass, drifted from an earlier `:456-461`). A child graph class whose filename sorts
  *before* its graph-class parent's fails to seal, even though the parent declares moments later in
  the same scan. Parenting to a C# class or a bootstrap base never has this problem, because both
  are declared earlier, during script load, before the graph scan starts at all.

## 3. Mapping to Blueprints

If you know Unreal's Blueprints, most of Aver Node reads directly:

| Blueprint | Aver Node | What's different |
|---|---|---|
| A Blueprint asset, parent class | `CLASS <name> [parent]` at the top of a `.ocgraph` | The graph file *is* the asset — no separate wrapper. |
| Event BeginPlay | `OnStart` entry | Fires once, on the graph's own first `Tick()` call — not inside `Load()`, and not a separate editor-preview construction phase (§5). |
| Event Tick | `OnTick` entry | Fires every `Tick()` call, always strictly after `OnStart` in the tick where both run. |
| A custom event | Any `ENTRY` name other than `OnStart`/`OnTick`, fired via `Fire()` | See §5 — `OnHit` is the one demonstrated example, and it is not wired to anything today. |
| A Blueprint variable | `VAR` | Float/Int/Bool only, no struct/array/object reference (§4, §8). |
| A function/event input | `PARAM` | Closed vocabulary, and it differs by which entry points the file wants (§4) — not free-form the way a Blueprint's own inputs are. |
| SpawnActorFromClass | `Spawn` node, `class=` attribute | Resolved **by name** at invocation time, not baked to a handle at compile time. |
| Get/Set on a component variable | `GetField`/`SetField` (scalar), `GetFieldVec3`/`SetFieldVec3` (vector) | Addressed by a `field=` string naming a scene field (e.g. `CLocal.position`), not a live pin reference to a component instance. |
| A Blueprint **function** | `FUNC <name>` + `FUNCIN`/`FUNCOUT`, body tagged `func=`, called by a `CallFunc` node | Its own method, its own canvas, and it **can recurse** (bounded at depth 120). Float/int/bool signatures only. **A `pure` function cannot recurse** — `Select` evaluates both sides, so only an impure function's `Branch` can terminate one. |
| A Blueprint **macro** | — | Not implemented. Functions are real calls, not inlined, so there is nothing macro-shaped to reach for. |
| Branch / Sequence / For Loop / While Loop | `Branch` / `Sequence` / `ForEach` / `While` | `ForEach` is counted-repeat only — there is no array pin type, so no per-element iterator (§8). |

**Where it deliberately differs, structurally:** one file is one graph and one class at most, and there
are no collapsed/macro sub-graphs. There IS now an event-graph-versus-function-graph split — this
paragraph used to say there was not, and `FUNC` made that false. A file holds one event graph plus any
number of named functions; the editor shows one subgraph at a time and a wire cannot cross between them,
because they compile to separate methods. What is still true is that `ENTRY` records mark starting
points rather than defining a graph of their own, and that node ids remain a single **file-wide**
namespace rather than being scoped per function. There is no Construction
Script equivalent either; see §5 for why the difference is more interesting than a missing
feature.

## 4. `PARAM` versus `VAR`: why a variable is not a parameter

The short version, in the file's own words (`IdleMotion.ocgraph:45`): *"A VAR is the opposite of a
PARAM: the caller owns a PARAM, the graph owns a VAR."*

**`PARAM`** is supplied fresh by the caller on every invocation, and which names are legal depends
on which entry points the *whole file* wants — `Parameters` is one list for the file, not one per
entry point (`Graph.cs`):

- A dataflow graph (no `ENTRY` at all) accepts exactly `entity` (int) and `time` (float),
  case-insensitively — anything else fails at `Load()`, naming the offending `PARAM`
  (the `PARAM` type/name check loop, `GraphHost.cs:277-294` as of this pass, drifted from an earlier
  `:256-275`, which named the doc comment just above the loop rather than the loop itself).
- A graph wanting `OnStart` or `OnTick` accepts exactly `entity`, `time`, and `deltaTime`
  (`GraphHost.cs:326-373`).
- A graph with **only** an on-demand event (no `OnStart`/`OnTick`) is unconstrained — it declares
  whatever `PARAM` list its own payload needs, because `Fire()`'s args are positional, matching
  declaration order, not drawn from a named-slot vocabulary (`GraphHost.cs:633-645` (`Fire`, drifted from an earlier `:580-604`, which named an on-demand-path comment rather than the method)).

The sharp edge here: mix an on-demand event into a file that *also* wants `OnStart`/`OnTick`, and
every `PARAM` in that file — including the ones only the on-demand event reads — gets checked
against the `OnStart`/`OnTick` vocabulary, because it is one shared list. **An on-demand event with
a custom payload needs its own file**, separate from any `OnStart`/`OnTick` graph, not a design
preference but a real constraint this format enforces.

**`VAR`** is owned by the graph/host and survives from one `Tick()`/`Fire()` invocation to the
next — but only on the same `GraphHost` instance. It does not survive a `Load()` call (hot-reload
included), and it does not survive a process restart (§8). Storage is one `Dictionary<string,
object>` per `GraphHost` (`GraphVarStore.cs`), seeded from each `VAR`'s own declared default the
moment the graph loads — which is what makes "read before any write" deterministic rather than
undefined.

**In the editor**, `VAR` records are now modelled as first-class objects. A **Variables panel**
appears in the graph editor, showing every declared variable with its type and default value —
you can declare, rename, retype, and delete variables without hand-editing the `.ocgraph` text.
`GetVar` and `SetVar` nodes take a variable picker instead of free-text `var=` attributes,
refusing to compile if the named variable does not exist. This was not possible before `VAR`
became part of the model: `Graph.Validate()` checks undeclared variables and rejects them at
load time (the undeclared-variable check, `Graph.cs:818` as of this pass, inside `Validate()`
starting at `:507` — drifted from an earlier `:366-375, 462-465`, which named the unrelated `Domain`/
`DomainKind` property and a `view=` comment), which meant the editor's graph-loading path could not
open any file with an undeclared `GetVar`/`SetVar` node; now every real graph round-trips
without read-only-mode fallbacks.

## 5. Entry points and their ordering

An `ENTRY <nodeId> <eventName>` record is what makes a node run at all on the event-driven path —
the node type (`OnStart`, `OnTick`, `OnHit`, or any other type with no inputs) is just a labelled,
no-input starting shape the `ENTRY` record points at. Nothing in the parser or compiler special-
cases the string `"OnHit"`; it sorts into "on-demand" the same way any project-invented event name
would (`GraphHost.cs:24`'s own header comment — "'OnHit' is the worked example, not a special case" —
replacing an earlier `OcGraphParser.cs:774-786` citation, which named `PinnedValue` parsing).

**`OnStart` fires exactly once, on the graph's own first `Tick()` call** — not inside `Load()`.
That is deliberate: `Load()` can run long before the game loop's first real frame (a project's
graphs are all discovered and compiled at project open), while "OnStart fires once when play
begins" reads most literally as "the first time something actually ticks this graph". One
consequence worth knowing: a *compile* error surfaces at `Load()` time (project open, early), but
an `OnStart` *runtime* error only surfaces at the first real `Tick()` (frame 1) — the same
load/runtime split the rest of this system already has.

**`OnTick` fires every `Tick()` call, strictly after `OnStart` in the tick where both run** — and
`_execSimTime` (the clock a graph's `PARAM time` reads) accumulates once per `Tick()` call, not
once per entry fired, so `OnStart` and `OnTick` see the *identical* time value on the frame where
both run (`_startInvoked`'s declaration at `GraphHost.cs:132` and its lazy-fire site at `:582-586`
for the "once" half; `_execSimTime += deltaTime` at `:535`, inside `TickEventGraph` starting at `:533`,
for the shared-clock half — drifted from an earlier `:125-138, 553-568`, whose second range fell
inside an unrelated boxing-bug comment).

**Both class instances and CLASS-less project graphs tick every frame, ungated on play state.**
Deliberately: a pure-graph project has no C# `GameMode` to ever call `aver_fw_begin_play`, so
gating a class instance's tick on "is the game playing" would make graph-as-class silently inert
in exactly the configuration it exists to serve. The consequence is real and worth knowing in the
editor specifically: **a class instance's `OnTick` runs the moment it is placed, in the editor,
whether or not you have pressed Play** — unlike an ordinary C# actor, which is gated on Play. This
is also the closest thing to a Construction-Script moment Aver Node has, and it is not a separate
phase at all — it is the same `OnStart`/`OnTick` a spawned instance always runs.

**Any other `ENTRY` name is on-demand**, compiled the same way but invoked later via
`GraphHost.Fire(eventName, args, out result)` rather than driven by `Tick()`'s own cadence.
`Fire()`'s `args` are **positional**, matching the fired entry's declared `PARAM` list in file
order — a deliberately different contract from `Tick()`'s named `entity`/`time`/`deltaTime`
vocabulary, because the caller firing an on-demand event already knows its specific payload shape,
and a second growing named-slot vocabulary would only recreate the trap `PARAM` itself avoids
(`GraphHost.cs:633-645` (`Fire`, drifted from an earlier `:580-604`, which named an on-demand-path comment rather than the method)). `Fire()`'s result is **not** auto-applied to the entity's position the
way `Tick()`'s is — an on-demand event has no fixed target the way `Tick(entityId, …)` does.

**There is no teardown entry point.** `DispUnbind` is the only teardown hook for a class instance:
it drops the instance from the live table (stopping it from ticking) and releases its `GraphHost`
and `VAR` storage for GC. No `OnStart`/`OnTick`/on-demand entry means "I am being destroyed" — a
graph cannot run its own cleanup logic.

## 6. How an instance reaches a level

A `PLACE`/`PLACEG` line in an `.ocworld`/`.ocmap` gets an optional trailing `class <ClassName>`
keyword-argument:

```
PLACE none 0 0 0 0 0 0 1 class AN_Orbiter
```

(`test-content/GraphDemo/Content/Maps/OrbitDemo.ocmap:17`). The leading asset column (`none`,
above) is never read for a class placement — the parser consumes `class <name>` as a keyword pair,
not a bare flag, so the argument token is captured rather than misread as a material name
(`OcWorld.cpp:264` as of this pass, drifted from an earlier `:206-211`, which named an unrelated
water-placement field parse).

The pipeline from there is the same shape in both composition roots (the shipped game and the
editor):

1. **Parse**: the placement's `className` is set; non-empty means "this is a class instance, not
   an ordinary mesh".
2. **Skip the raw entity**: `aver::world::instantiate` builds no mesh/physics entity for a
   non-empty `className` — a class instance is framework-free by design; whatever it looks like is
   the class's own `mesh=`/`material=` defaults, or whatever its own graph does.
3. **Collect, don't spawn yet**: level load gathers every class placement (`classPlacements_`) but
   does not spawn them — scripting is not ready that early in either root's boot order.
4. **Spawn for real**: a later call, `spawnClassPlacements()`, resolves each placement's class
   name via `aver_fw_class_find` and spawns it via `aver_fw_spawn` — not `aver_fw_spawn_preview`.
   There is no "placed, live in edit mode, promoted at Play" precedent for a class placement the
   way there might be for an ordinary actor; `OnBeginPlay`/`OnStart` fire immediately, at this
   call, in both roots.
5. **An unresolvable class name** (never declared, or failed to seal per §2) gets one `WARN` and
   produces **no entity at all** — not even a fallback mesh (`GameLevel.cpp:259-286`).

**Authoring one today is a text edit, not a palette drag.** There is no editor UI that places a
class instance the way dragging a mesh from the content browser places one — you write
`class AN_Foo` into the `.ocworld`/`.ocmap` file yourself, exactly like `OrbitDemo.ocmap` above.
The editor's **File ▸ Open Level** does call `spawnClassPlacements()` so a class placement appears
after loading a level that has one — but see §8 for what **Save** does to it.

## 7. Worked example: two orbiting entities, no C#

`test-content/GraphDemo` is a small, self-contained project built to prove exactly this feature —
not a demo of a game, a genre-neutral fixture. It has one graph and one map:

**`Content/Scripts/IdleMotion.ocgraph`** declares itself class `AN_Orbiter`, remembers where its
own entity started (`OnStart` reads `CLocal.position` via `GetFieldVec3` and stores it in three
`VAR`s), then every `OnTick` orbits that remembered point:

```
CLASS AN_Orbiter Actor

PARAM entity int
PARAM time float

VAR homeX float 0
VAR homeY float 0
VAR homeZ float 0

ENTRY seed OnStart
NODE seed OnStart
NODE here GetFieldVec3 field=CLocal.position
LINK ent.value here.entity
NODE setHomeX SetVar var=homeX
LINK seed.exec setHomeX.exec
LINK here.x setHomeX.value
# ...setHomeY, setHomeZ chained the same way through each SetVar's own `then`

ENTRY tick OnTick
NODE tick OnTick
# ...bob/drift computed from PARAM time, then added to the remembered home position...
NODE place SetFieldVec3 field=CLocal.position
PIN place exec in exec
PIN place entity in int
PIN place x in float
PIN place y in float
PIN place z in float
PIN place then out exec
PIN place success out bool
LINK tick.exec place.exec
```

The full file explains its own choices in its header comments — including why `OnStart` was
chosen over a first-tick `Branch` (there is no `NOT` node in the vocabulary, which would have made
that awkward) and why `place`'s pins are spelled out by hand (see [Trap
1](AVER_NODE_NODES.md#read-this-first--three-traps) in the node reference — `SetFieldVec3`'s
default shape has no exec pins at all).

**`Content/Maps/OrbitDemo.ocmap`** places two instances of that one class at different starting
points — the point being that *placement*, by itself, proves independent `VAR` state: if the two
instances shared one store, both would orbit whichever entity's `OnStart` ran last.

```
PLACE none 0 0 0 0 0 0 1 class AN_Orbiter
PLACE none 400 0 0 0 0 0 1 class AN_Orbiter
```

**Run it** — this is a real command against a real build, not a hypothetical:

```
build/bin/Sandbox.exe --headless --frames 5 --project test-content/GraphDemo/Game.ocproject
```

The actual output (captured against this repository, trimmed to the relevant lines):

```
[INFO ] [Graph] declared class 'AN_Orbiter' (parent 'Actor', ticks) from '...IdleMotion.ocgraph'
[INFO ] [Graph] 1 graph class(es) declared from '...Content'
[INFO ] [Level] 2 class instance(s) placed -- an entity exists for each; whether its graph COMPILED
        is reported per instance above, ...
[GraphHost] 'IdleMotion' entity 16777217: OnStart -> [5, 0, 6.12E-06]
[GraphHost] 'IdleMotion' entity 16777217: OnTick  -> [5, 0, 6.12E-06]
[GraphHost] 'IdleMotion' entity 16777218: OnStart -> [405, 0, 6.12E-06]
[GraphHost] 'IdleMotion' entity 16777218: OnTick  -> [405, 0, 6.12E-06]
...
[GraphHost] 'IdleMotion' entity 16777217: OnTick -> [4.9999776, 0, 0.063581675]
[GraphHost] 'IdleMotion' entity 16777218: OnTick -> [404.99997, 0, 0.063581675]
```

Two things this proves, directly, not by design intent: **`OnStart` and `OnTick` share one clock**
— entity `16777217`'s first `OnStart` and first `OnTick` line are numerically identical, because
both ran inside the same `Tick()` call before the sim clock advanced again. And **the two
instances never converge** — one settles orbiting `x≈5`, the other `x≈405` — which is only
possible if each entity's `VAR homeX/Y/Z` seeded from *its own* `GetFieldVec3` read and never
leaked into the other's store.

## 8. Honest limits

Aver Node is a real way to build gameplay without C# — the example above is the whole chain,
running. It is also new, and smaller than what it resembles. Specifically:

- **No cross-entity event dispatch.** A graph can *receive* an on-demand event (`OnHit`, or any
  other name a caller fires) via `Fire()`, but there is no node anywhere in the catalog that lets
  one entity's graph raise an event *on a different entity's* graph. `Fire()` is something a host
  calls into one specific `GraphHost`; it is not something a node can invoke.
- **No HUD or 2D drawing from a graph.** Nothing in the palette reaches `Aver.UI` — still true,
  checked against the catalog rather than remembered: **239 node types across 24 categories**
  (re-derived this pass: `grep -cP '^\s*t\.push_back\('` in `sandbox/src/GraphNodeDefs.hpp`, and 24
  distinct category strings, an "Audio" one among them, which this page's own node reference does
  not yet document at all), none of them a draw call. This bullet's number has now gone stale three
  times in a row — 36, then 124, then 240 (that last one counted a comment that happened to quote
  the literal text `t.push_back({...})`, not a real entry) — while the claim itself ("no draw call
  in the catalog") has held every time. A stale number beside a true statement is what teaches a
  reader to stop trusting the statement; see [the node reference](AVER_NODE_NODES.md)'s own intro for
  the same lesson stated at length.
- **No `String` pin.** `PinType` has exactly four members — `Float`, `Int`, `Bool`, `Exec`. Every
  string a node needs (`field=`, `class=`, `var=`, `name=`, `mesh=`, `material=`) arrives as a
  `NODE`-line attribute, never as data an upstream node computes or a pin carries.
- **`VAR` is Float/Int/Bool only, and does not survive a level reload.** Storage is one object per
  `GraphHost`, created fresh at `Load()` time and seeded from each `VAR`'s declared default —
  reloading the graph (hot-reload included), respawning the instance, or restarting the process
  all reset every `VAR` to its default. There is no persistence layer underneath it.
- **A graph class needs an already-declared parent, but for a character that no longer means any
  C# at all.** `CLASS Foo` alone parents to the bootstrap `Actor` base for free, and `CLASS Foo
  Character` now parents directly to the framework's own concrete `AverCharacter` registry row —
  `Character` stopped being an abstract type nothing could construct the moment it became a
  *concrete*, spawnable class (see `Aver.Framework.Character.cs`'s own class comment for why it is
  concrete rather than an abstract anchor like `Actor`/`Pawn`/`GameMode`). `CharacterMove` succeeds
  against any such instance with no project C# involved — `test-content/AN_Playable`'s
  `CLASS AN_Player Character` is exactly this, proven by a headless run. (This paragraph used to say
  a project still needed one minimal C# subclass for the graph's `CLASS` line to name; that stopped
  being true once `Character` itself became declarable, and this doc had not caught up.)
- **~~The editor's Save does not round-trip a class placement.~~ FIXED, TWICE OVER.** This said that
  opening a level with a class placement and hitting Save silently dropped the placement's entire
  line, and that the workaround was to edit the `.ocworld`/`.ocmap` text by hand. Both halves of that
  are now out of date.

  The dropped line was fixed first: `saveLevel` writes `classPlacements_` back out. The *second* half
  survived longer and was harder to see -- the line was written from the copy read off disk, so a
  placement you selected and dragged in the viewport was saved exactly where it had been, with
  nothing logged. The comment defending that said "the editor cannot currently EDIT a class placement
  (there is no entity to select and drag)", which was false: `spawnClassPlacements` spawns a real
  entity per placement, `aver_fw_spawn`'s handle IS a scene entity, and the World Outliner lists
  anything carrying a mesh or a name. Both are fixed; `sandbox/src/LevelClassSave.hpp` holds the
  rebuild, and `GraphEditorLoadSaveTest` fails against the old behaviour.

A few smaller, related edges, already covered in more depth above: an undeclared/misspelled
`CLASS` parent fails almost silently (§2), parenting one graph class to another is file-sort-order
fragile (§2), and `Fire("OnHit", …)` is a demonstrated mechanism with no engine-side collision
system wired to call it yet (§5). None of these are fixed in this pass — this page documents them
so the next person does not have to rediscover them by reading source.

---

*For every node's pin shape, attribute and compile-path behaviour, see [the node
reference](AVER_NODE_NODES.md). This page previously described a pre-implementation feasibility
plan (dated 2026-08-08, Roslyn-based, predating the class model entirely); that plan is superseded
by what is written above, which describes the system as it actually shipped through `41d6566`.*
