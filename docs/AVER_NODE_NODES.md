# Aver Node — Node Reference

**Pins are authoritative in [`docs/nodes/`](nodes/README.md).** Every table-driven node and every hand-parsed one is listed there, one page per family, generated from the C# tables and the editor palette (`python scripts/node-catalog.py --write`), and `NodeCatalogCheck` fails when a pin list and its source disagree. [`nodes/Core.md`](nodes/Core.md) lists the hand-parsed nodes (including `SaveGame`/`LoadGame`, `SetSkeleton` and the Synapse AI nodes, which have no table row). This page keeps the long-form behaviour notes; where a pin table below and a generated page differ, the generated page is right. It carries no node counts: `python scripts/node-catalog.py --counts` prints them.

This is the parser's vocabulary: each entry below is one `case` in `OcGraphParser.AddDefaultPins` (`scripting/csharp/Aver.Graph/OcGraphParser.cs`, method at line 810). It is the ground truth for a node's default pin shape — not the editor palette, not this document's prose. Where the editor's catalog (`sandbox/src/GraphNodeDefs.hpp`, `buildCatalog()`) and the parser might disagree, **the parser wins**; see [Parser vs. editor catalog](#parser-vs-editor-catalog).

Compile-path behaviour is read from `scripting/csharp/Aver.Graph/GraphCompiler.cs` — specifically `EmitNode`'s topological switch, the `IsExecCapable*` predicate family (grep `IsExecCapable`; roughly two dozen of them, one per side-effecting node group), `IsExecOnlyNodeType` (the exec-only allowlist) and `EmitPullOutput`'s two refusal throws (`"cannot be read as a data value"` for a named side-effect type, `"cannot be pulled as a data value inside an exec chain"` for the generic default case). **Line numbers for this file drift constantly** (it has grown past every number pinned for it, more than once; a citation once pointed at `Spawn`'s refusal code instead of `EmitCompare`'s `Cgt`). A specific number reads as more-checked than a bare name, which is backwards when the file edits this often: look up code by method name, and trust a `file:line` here only if it was checked in the same pass it is read.

This page is about node *shapes* — what pins a `NODE` of a given type gets and what it does. For the class model, `PARAM`/`VAR`, and entry points, see **[the guide](VISUAL_SCRIPTING.md)**; for the `.ocgraph` file grammar itself (every record's syntax, and where the C++ and C# readers disagree), see **[`formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md)**.

**Counts.** None are kept in prose any more, because every one went stale. `python scripts/node-catalog.py --counts` prints the table rows per family, the palette entries (gameplay and material-only, from a scanner that reads each `t.push_back({...})` whole) and the parser's vocabulary (distinct `case` labels inside `AddDefaultPins`, aliases included, of which the [aliases below](#aliases-invisible-to-the-palette) are a handful).

**`graphNodeCatalog()` is not one single vocabulary** (since `c608621`). The `kDomainMaterial` rows are arithmetic on a surface, compiled to HLSL by an entirely different C++ compiler (`aver::pbr::compileMaterialGraph`, `modules/render.pbr/src/MaterialGraphHlsl.cpp`), never reachable through `OcGraphParser.AddDefaultPins`. This page and every table below describe the `kDomainGameplay` rows only; the material rows are a different vocabulary with their own ground truth — see **[`MATERIALS.md`](MATERIALS.md)**.

## Contents

1. [Read this first — three traps](#read-this-first--three-traps)
2. [Const](#const)
3. [Math](#math)
4. [Vector](#vector)
5. [Debug](#debug)
6. [Logic](#logic)
7. [Scene](#scene)
8. [Input](#input)
9. [Param](#param)
10. [Flow](#flow)
11. [Actor](#actor)
12. [Character](#character)
13. [Game](#game)
14. [Convert](#convert)
15. [Transform](#transform)
16. [Physics — reads and queries](#physics--reads-and-queries)
17. [Physics — writes and creation](#physics--writes-and-creation)
18. [Function](#function)
19. [Var](#var)
20. [Game systems](#game-systems), including [Math, game flow, scene queries and the rest](#math-game-flow-scene-queries-and-the-rest)
21. [Aliases invisible to the palette](#aliases-invisible-to-the-palette)
22. [Parser vs. editor catalog](#parser-vs-editor-catalog)
23. [What a node cannot do](#what-a-node-cannot-do)

### How to read a pin table

Each pin is `name (direction, type)`. `exec` is a pin **type**, exactly like `float`/`int`/`bool` — a `LINK` between two `exec` pins is what a control-flow wire actually is; there is no separate record for it (`Graph.cs:14-31`).

**Path** is one of four codes, defined once here and used in every table below:

| Code | Meaning |
|---|---|
| **D** | Data node. Runs under *either* compiler, no restriction — a pure read or pure computation. |
| **W** | Write, or an expensive/continuous read. Runs unconditionally inside `Compile()`'s dataflow pass (every invocation, whether or not anything reads its result). Inside an ENTRY-driven (PUSH) graph it must be wired directly into the exec chain — pulled as a bare data value with no incoming exec edge, it fails to compile. |
| **X** | Exec-only trigger. `Compile()` (the PULL/dataflow compiler) silently skips it — it has no data value to pull, so it isn't a data-graph error, it just doesn't exist there. Only `CompileEntryPoint()` (PUSH, reached via an `ENTRY` record) runs it. |
| **P** | PUSH-only side effect. `Compile()` refuses it **outright** — a compile-time `InvalidOperationException` naming the node, even if that node is never wired to anything. Only `CompileEntryPoint()` can run it. |

## Read this first — three traps

### 1. One explicit `PIN` record erases *every* default pin, not just the missing ones

`AddDefaultPins` opens with:

```csharp
// Only add defaults if no pins are explicitly declared.
if (node.Pins.Count > 0) return;
```

— `OcGraphParser.cs:813`. That is not "fill in whatever wasn't declared"; it is "if this node has *any* explicit `PIN` record at all, stop, add nothing." A node type whose defaults include five pins, given one hand-written `PIN` line, ends up with exactly one pin — the other four do not silently reappear.

This is exactly why `test-content/GraphDemo/Content/Scripts/IdleMotion.ocgraph`'s `place` node (`SetFieldVec3`, whose own default shape has **no** exec pins — see [Scene](#scene)) spells out all seven of its pins by hand:

```
NODE place SetFieldVec3 field=CLocal.position
PIN place exec in exec
PIN place entity in int
PIN place x in float
PIN place y in float
PIN place z in float
PIN place then out exec
PIN place success out bool
```

(`IdleMotion.ocgraph:122-133`, its own comment spells out the same trap in plain language). Leaving off any one of those seven lines would not restore that pin from the default table — it would just be gone.

### 2. Not every node runs on both compilers

A graph is one of exactly two shapes, decided once by `GraphHost.LoadFromText` from `graph.EntryPoints.Count` (`GraphHost.cs:248-254`): zero `ENTRY` records compiles through `Compile()` (pure dataflow, PULL); one or more compiles through `CompileEntryPoint()` (PUSH, walked from an `ENTRY` node). The **Path** column above tells you which of the four behaviours a node type gets under each. The practical read: `X`-path nodes (`OnStart`, `Branch`, …) are invisible to a dataflow-only graph, and `P`-path nodes (`Spawn`, `SetVar`, `CharacterMove`) make a *dataflow* graph fail to compile the moment they exist in the file at all — not when they're used, when they *exist*, because `Compile()`'s topological pass visits every node in the graph regardless of whether it feeds an `OUT` record (`GraphCompiler.cs:227-231`, `293-306`).

### 3. `Param` / `GetVar` / `SetVar`: the palette's pin type is a placeholder, not a promise

The parser types a `Param`/`GetVar`/`SetVar` node's `value` pin from the actual declared `PARAM`/
`VAR` — `Float`, `Int`, or `Bool`, whichever that record says (`OcGraphParser.cs:2072-2084` for
`param`/`getparam`, `2506-2511` for `getvar`, `2524-2531` for `setvar`). The editor's Add-Node palette
cannot do that: it has no declared `PARAM`/`VAR` list to consult when the node is spawned, so it
hardcodes the pin to `float` (`GraphNodeDefs.hpp:588, 802, 810`, each with its own "adjustable
per-instance" comment).

That hardcoded pin becomes a **real** `PIN` record the moment the node is dropped on the canvas
. Trap #1 above
then applies:
the node now has an explicit pin, so `AddDefaultPins`'s type-inference path never runs for it again,
on any future load. Point that node's `param=`/`var=` at anything other than a `Float` declaration
and `Graph.Validate()` catches it — loudly, by design (`Graph.cs:340-376, 378-415`,
`"declares output pin 'value' as Float but parameter '…' is Int"`) — but there is **no editor
control that edits a pin's type**. The only fix is hand-editing the saved `.ocgraph` text, changing
the `PIN … value out float` token to `int` or `bool`. A palette-spawned `Param`/`GetVar`/`SetVar`
node against a non-`Float` declaration is broken until someone opens the file in a text editor.

---

## Const

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `ConstFloat` | `value` (out, float) | — | D | A fixed float literal. |
| `ConstInt` | `value` (out, int) | — | D | A fixed int literal. |
| `ConstBool` | `value` (out, bool) | — | D | A fixed bool literal. |

The literal itself is a `value=` **NODE-line** attribute (e.g. `NODE k ConstFloat value=9.0`), read by the `NODE`-line parsing loop (`OcGraphParser.cs:291-308`) — or, in the C++-writer form, a default value trailing the `PIN … out float 5` record itself (`OcGraphParser.cs:408-450`). Either way it lands in `graph.ConstantOutputs`, keyed by node id, not on the `Pin` object.

## Math

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Add` | `a` (in, float), `b` (in, float), `result` (out, float) | — | D | `a + b`. |
| `Multiply` | `a` (in, float), `b` (in, float), `result` (out, float) | — | D | `a * b`. |
| `Subtract` *(alias `sub`)* | `a` (in, float), `b` (in, float), `result` (out, float) | — | D | `a - b`. |
| `Divide` *(alias `div`)* | `a` (in, float), `b` (in, float), `result` (out, float) | — | D | `a / b`. |
| `Sin` | `a` (in, float), `result` (out, float) | — | D | `sin(a)`, radians. |
| `Cos` | `a` (in, float), `result` (out, float) | — | D | `cos(a)`, radians. |
| `Min` | `a`, `b` (in, float), `result` (out, float) | — | D | `min(a, b)`. |
| `Max` | `a`, `b` (in, float), `result` (out, float) | — | D | `max(a, b)`. |
| `Mod` *(alias `modulo`)* | `a`, `b` (in, float), `result` (out, float) | — | D | IL `Rem` — the C# `%`, so the sign follows the dividend. |
| `Pow` | `a`, `b` (in, float), `result` (out, float) | — | D | `a` to the power `b`. |
| `Abs` | `a` (in, float), `result` (out, float) | — | D | `|a|`. |
| `Negate` *(alias `neg`)* | `a` (in, float), `result` (out, float) | — | D | `-a`. |
| `Sqrt` | `a` (in, float), `result` (out, float) | — | D | Square root. Negative input gives `NaN`, unlike `Divide` — see below. |
| `Floor` | `a` (in, float), `result` (out, float) | — | D | Round toward negative infinity. |
| `Ceil` *(alias `ceiling`)* | `a` (in, float), `result` (out, float) | — | D | Round toward positive infinity. |
| `Round` | `a` (in, float), `result` (out, float) | — | D | `Math.Round` — **banker's rounding**, so 0.5 goes to 0 and 1.5 goes to 2. |
| `Saturate` | `a` (in, float), `result` (out, float) | — | D | `clamp(a, 0, 1)`, spelled with the same Min/Max `Clamp` uses. |
| `Clamp` | `a`, `min`, `max` (in, float), `result` (out, float) | — | D | `min(max(a, min), max)`. |
| `Lerp` | `a`, `b`, `t` (in, float), `result` (out, float) | — | D | `a + (b - a) * t`. **Not clamped** — `t` outside 0..1 extrapolates. |

`Divide`'s zero handling is a deliberate, non-IEEE convention: `b == 0.0` yields `0.0`, never
`NaN`/`Infinity` (`EmitDivide`'s own comment). A bare float divide
propagates `NaN`/`Infinity` silently through every downstream node until it surfaces frames later
as an object that vanished or exploded, nowhere near the divide that caused it — `0.0` is a value a
transform pipeline can keep running through. If a graph genuinely needs to detect the
divide-by-zero condition, there is no signal for it; a `compare-vs-self` `NaN` check would never
fire, because the result is never `NaN` in the first place.

`Sqrt`, `Pow`, `Floor`, `Ceil` and `Round` widen to `double` for the `System.Math` call and narrow back. **`Sqrt` does not inherit `Divide`'s no-`NaN` convention** — `sqrt(-1)` really is `NaN` and really does propagate. That inconsistency is deliberate rather than overlooked: `Divide` special-cases zero because dividing by a value that happens to reach zero is a normal thing for a transform pipeline to do, while taking the root of a negative number is a graph that has already computed something wrong.

`Sin`/`Cos` widen to `double` for the `System.Math` call and narrow back — both directions are explicit `Conv_R8`/`Conv_R4` IL, called out in `EmitSin`'s own comment as a lesson already paid for once (skipping either conversion still compiles, and silently reinterprets bits instead of converting the value).

## Vector

THE ENGINE IS 3D AND EVERY MATH NODE ABOVE IS SCALAR. These take and return LOOSE COMPONENTS rather than a vector value, because there is no Vec3 pin type -- `PinType` is `Float`, `Int`, `Bool` and `Exec` -- and that is the same convention `GetFieldVec3` / `SetFieldVec3` already use. All of them are PURE: no exec pins, safe to pull as often as anything asks, and an output nobody reads costs nothing.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `VecAdd` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `x` `y` `z` (out, float) | — | D | `a + b`, component-wise. |
| `VecSub` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `x` `y` `z` (out, float) | — | D | `a - b`, component-wise. |
| `VecScale` | `ax` `ay` `az` (in, float), `s` (in, float), `x` `y` `z` (out, float) | — | D | `a * s`. |
| `VecCross` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `x` `y` `z` (out, float) | — | D | `a × b`. |
| `VecNormalize` | `ax` `ay` `az` (in, float), `x` `y` `z` (out, float) | — | D | `a / max(\|a\|, 1e-6)`. A zero vector normalises to ZERO, not to NaN. |
| `VecLerp` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `t` (in, float), `x` `y` `z` (out, float) | — | D | `a + (b - a) * t`, exact at t = 0 and t = 1. |
| `VecDot` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `result` (out, float) | — | D | `a · b`. |
| `VecLength` | `ax` `ay` `az` (in, float), `result` (out, float) | — | D | `\|a\|`. |
| `VecDistance` | `ax` `ay` `az` (in, float), `bx` `by` `bz` (in, float), `result` (out, float) | — | D | `\|a - b\|`. |

## Debug

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Print` | `exec` (in), `value` (in, float), `then` (out, exec) | — | X | Writes `[Graph] <node id> = <value>` to the log. LABELLED BY THE NODE ID, so `NODE muzzleLen Print` prints `muzzleLen`. Refused if pulled as data, like every other side effect. |
| `PrintInt` | `exec` (in), `value` (in, int), `then` (out, exec) | — | P | Writes `[Graph] <node id> = <value>` to the log for an INT value — the same log line `Print` writes, without ever routing the value through a `float`. |

## Logic

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Compare` *(alias `compare_f32`)* | `a` (in, float), `b` (in, float), `result` (out, bool) | — | D | `a > b`, strict, via IL `Cgt` (`EmitCompare`). |
| `Select` | `cond` (in, bool), `ifTrue` (in, float), `ifFalse` (in, float), `result` (out, float) | — | D | Picks `ifTrue` or `ifFalse` by `cond`. |
| `And` | `a`, `b` (in, bool), `result` (out, bool) | — | D | Both. IL `And` — bools are `0`/`1` on the stack, so the bitwise op *is* the logical one. |
| `Or` | `a`, `b` (in, bool), `result` (out, bool) | — | D | Either. |
| `Xor` | `a`, `b` (in, bool), `result` (out, bool) | — | D | Exactly one. |
| `Not` | `a` (in, bool), `result` (out, bool) | — | D | `a == 0`, **not** a bitwise complement — `~1` is `-2`, which is truthy everywhere it would later be tested. |
| `Greater` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a > b`. Same op as `Compare`, under the name a reader looks for. |
| `GreaterEqual` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a >= b`, as `!(a < b)` — CIL has no `Cge`. |
| `Less` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a < b`. |
| `LessEqual` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a <= b`, as `!(a > b)`. |
| `Equal` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a == b`. Exact float equality — see below. |
| `NotEqual` | `a`, `b` (in, float), `result` (out, bool) | — | D | `a != b`, as `!(a == b)`. |

`Compare` used to be the only comparison in this vocabulary, and there were no boolean combinators at all. `IdleMotion.ocgraph`'s own comment records the cost of that in the course of explaining why it chose `OnStart` over a first-tick branch: *"There is no NOT node in the vocabulary today either, which would have made the branch form awkward as well as unnecessary"* (`IdleMotion.ocgraph:23-24`). The full set above closes that gap. `Compare` is unchanged and still means `a > b`; `Greater` is a separate node type that does the same thing under the name a reader looks for, so no existing graph had to be rewritten.

`Equal`/`NotEqual` compare floats **exactly**, with IL `Ceq`. There is no epsilon and no plan for one: a tolerance small enough to be safe is too small to help, and one large enough to help is a silent behaviour change in every graph that already worked. A graph comparing computed floats should subtract, `Abs`, and `Less` against its own chosen epsilon — which is three nodes that say what they mean, rather than one that hides the choice.

`Select`'s PULL-compiler behaviour is a genuine, deliberate asymmetry worth knowing: `Compile()` computes **both** `ifTrue` and `ifFalse` regardless of `cond` (both upstream subgraphs already ran during the topological walk; the branch there only picks which pre-computed local gets *loaded*). `CompileEntryPoint`'s pulled form is a real short-circuit instead — `EmitPullInput` recursively emits and runs only the chosen arm's subgraph. Same node type, two different cost profiles depending on which compiler reaches it.

## Scene

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetVisible` | `exec` (in), `entity` (in, int), `visible` (in, bool), `then` (out), `success` (out, bool) | — | P | Shows or hides an entity. |
| `AddTag` | `exec` (in), `entity` (in, int), `mask` (in, int), `then` (out), `success` (out, bool) | — | P | ORs `mask` into the entity's tag bits. |
| `RemoveTag` | `exec` (in), `entity` (in, int), `mask` (in, int), `then` (out), `success` (out, bool) | — | P | Clears every bit of `mask`. |
| `HasTag` | `entity` (in, int), `mask` (in, int), `has` (out, bool) | — | D | True when **every** bit of `mask` is set — all-of, not any-of, matching `Entity.HasTag`. A zero mask is false. |
| `GetTags` | `entity` (in, int), `mask` (out, int) | — | D | The whole tag bitmask, so a graph can stash it in a `VAR` or compare two entities directly. 0 for a dead handle. |
| `GetField` | `entity` (in, int), `value` (out, float) | `field=` | D | Reads a scalar (F32-kind) scene field by qualified name. |
| `SetField` | `entity` (in, int), `value` (in, float), `success` (out, bool) | `field=` | W | Writes a scalar scene field. |
| `GetFieldVec3` | `entity` (in, int), `x`/`y`/`z` (out, float) | `field=` | D | Reads a Vec3-kind scene field as three floats. |
| `GetForward` *(alias `get_forward`)* | `entity` (in, int), `x`/`y`/`z`/`eyeX`/`eyeY`/`eyeZ` (out, float), `success` (out, bool) | — | D | Reads a character's look direction (x/y/z unit vector) and eye position (eyeX/eyeY/eyeZ); used to fire rays from the character's perspective. |
| `GetViewEntity` *(alias `get_view_entity`)* | `entity` (in, int), `view` (out, int), `success` (out, bool) | — | D | The camera node a character looks through. Parent a first-person viewmodel to **this**, not to the character — parented to the pawn it stands still while the camera pitches around it. `success` is false, quietly, when the view node does not exist yet: `EnsureView` is lazy, so asking before `CharacterMove` has ever run is early rather than wrong. |
| `SetFieldVec3` | `entity` (in, int), `x`/`y`/`z` (in, float), `success` (out, bool) | `field=` | W | Writes a Vec3-kind scene field from three floats. |
| `Raycast` | `exec` (in), `originX`/`Y`/`Z` (in, float), `dirX`/`Y`/`Z` (in, float), `maxDist` (in, float), `then` (out), `hit` (out, bool), `entity` (out, int), `pointX`/`Y`/`Z` (out, float) — 14 pins | — | W | Casts a ray; reports whether it hit, which SCENE ENTITY (not physics body) owns what it hit, and where. |
| `SetParent` | `child` (in, int), `parent` (in, int), `success` (out, bool) | — | W | `aver_scene_set_parent(child, parent)`. |
| `SetName` | `entity` (in, int), `success` (out, bool) | `name=` | W | `aver_scene_set_name(entity, name)`. |
| `CreateEntity` | `exec` (in), `then` (out), `entity` (out, int), `success` (out, bool) | `name=` | X | `Entity.Create(name)`. EXEC-only for `Spawn`'s exact reason — it mints something, so a dataflow pull would mint one per invocation with nothing able to gate it. `success` is false only when the scene refused outright; a duplicate name is fine, because a name here is not unique. |
| `FindEntity` | `entity` (out, int), `found` (out, bool) | `name=` | D | `Game.Find(name)` — the FIRST live entity with that name, or 0. `found` distinguishes "not there" from a real handle, so a graph can branch rather than compare against 0 itself. A linear scan over live entities; not free in a tight loop. |
| `SetMesh` | `entity` (in, int), `success` (out, bool) | `mesh=` | W | Ensures a mesh renderer on `entity`, pointed at `mesh=`'s asset path. |
| `SetMaterial` | `entity` (in, int), `success` (out, bool) | `material=` | W | Sets the material on `entity`'s mesh renderer. |
| `AttachToSocket` | `entity` (in, int), `parent` (in, int), `success` (out, bool) | `socket=` | W | Hangs `entity` on a named socket of `parent`'s rig, so it rides the posed bone every frame. **Two entity pins**: the thing, then what it hangs from. Attaching *is* parenting plus a socket, so this also reparents `entity`. A socket name the parent's rig does not declare leaves the entity where it is rather than snapping it to the parent's origin. |
| `GetAnimCurve` | `entity` (in, int), `value` (out, float) | `curve=` | D | The named float curve on whatever clip `entity` is playing, at its current playhead. **Pure** — no exec pins, no side effect — so it works in both compilers. Returns **0** when the entity has no clip or the clip declares no such curve; a graph cannot tell that from a curve that genuinely reads zero, and C#'s `TryGetAnimationCurve` exists for callers that need to. |

**No exec pins by default on the four field-access nodes** — `GetField`/`SetField`/`GetFieldVec3`/
`SetFieldVec3` all get plain data pins only, including the two *writes*. This is the sharpest
version of trap #1 above, because it looks backwards at first glance: a write with no exec input
seems wrong until you read why. `SetField`/`SetFieldVec3` are dispatched **SetField-style**: inside
`Compile()`'s topological pass their case runs unconditionally, once per invocation, and
"overwriting the same field with the same value twice" is treated as harmless — the write is
idempotent, so there's no reason to force an author to build an exec chain just to run it
(`GraphCompiler.cs:387-404`). `SetParent`/`SetName`/`SetMesh`/`SetMaterial` share that same
dispatch and the same reasoning — all five wrap ABI calls that are safe to repeat with an
unchanged value (`OcGraphParser.cs:988-1004`). `Raycast` gets exec pins by
default *despite* having no side effect, for the opposite reason: a physics query is expensive
enough that the PUSH compiler wants "compute once per exec visit, cache it" rather than "recompute
on every pull" (`GraphCompiler.cs:1689-1702`). `GetForward` also gets plain data pins only, sharing
the same logic as `GetFieldVec3` — both are pure, idempotent reads (reading where a character is
pointing or a field's value changes nothing) that are safe to pull as often as anything asks, so
there is no reason to force an exec chain.

**Wrong-path behaviour differs by node**, and the difference is worth knowing before you hit it:
- Pulling `SetField`/`SetFieldVec3`/`SetParent`/`SetName`/`SetMesh`/`SetMaterial` as a bare data
  value inside a PUSH graph with no exec edge into them fails with a clear, node-naming
  `InvalidOperationException`: *"has a side effect and must be reached by wiring it directly into
  the exec chain"*.
- Pulling `Raycast` the same way fails too, from a different place: `Raycast` has no case in
  `EmitPullOutput` at all, deliberately, so it falls to that method's own `default` arm. The resulting error text is *"cannot be pulled as a
  data value inside an exec chain (it is not one of the pure expression kinds this compiler knows, and
  has no exec pins reaching it directly either)"*.

**`Raycast.entity` is a SCENE ENTITY id, not a physics body handle** — `GraphInterop.RaycastForGraph`
(`Aver.Framework/GraphInterop.cs`) reads `RaycastHit.Entity`, the id `aver_phys_set_entity` stamped on
whatever body or character the ray hit, not `RaycastHit.Body.Handle`. `entity == 0` on a real hit
(`hit == true`) means the ray struck something no entity owns — the landscape heightfield is the one
production example today — and is not the same outcome as a miss, where `hit == false` and `entity`
is left at its default 0 for the same reason but a different one. Check `hit` first; `entity == 0`
only tells you "unowned" once you already know the ray landed on something. `entity` is exactly the
pin `FireEvent.target` wants — `LINK raycastNode.entity fireEventNode.target` is how a graph aims one
entity's `OnHit` at whatever another entity's ray just found, with no spawn-and-remember `VAR` needed
to bridge the two.

**`GetField`/`SetField` only address F32-kind fields**, enforced at compile time — pointing either
node's `field=` at a Vec3/Quat/Bool/I32 field fails with an explicit "not an F32 field" error
. A
Vec3-kind field (`CLocal.position`, `CLight.colour`, …)
needs `GetFieldVec3`/`SetFieldVec3` instead — the pin-type model has no `Vec3` pin, so the split
into a second node pair is how a three-float value crosses the format at all, rather than a new
`PinType` member (`OcGraphParser.cs:653-659`).

**A TAG IS A BITMASK, NOT A STRING**, and that is why this family needed none of the compile-time
attribute machinery `class=`/`name=`/`field=`/`var=` exist for. `PinType` has no string, so every
other string-shaped API had to put its argument on the `NODE` line where a graph can never compute
it; `Entity.Tags` is a `uint` over `CTags.bits`, so an ordinary `int` pin carries it. A graph can
therefore **build** a mask at runtime `—` OR two together, read one out of a `VAR`, or test several
bits at once `—` which a string attribute could not have done at any price.

The pin is `int` rather than an unsigned type because `PinType` has none, and inventing one to carry a bit pattern would be a new pin type for a reinterpretation. A mask with the top bit set arrives as a negative int and works; `unchecked((uint))` is the whole of the conversion, in the same direction `Entity.Tags` already does it.

**`success` IS READABLE ON THESE**, and on `SetVelocity`/`Teleport`/`Possess`/`Unpossess`, as of the
same change: `EmitExecApiCall` stores it into an exec local rather than into `_pinLocals`, which
the exec compiler never reads. Before that it was a pin no graph could ever get a value out of.
**Ten other emitters still have that bug** (`EmitSetField`, `EmitSetParent`, `EmitSetName`,
`EmitSetMesh`, `EmitSetMaterial`, `EmitSetFieldVec3`, `EmitExecPhysicsWrite`,
`EmitExecTransformWrite`, `EmitGetForward`, `EmitGetViewEntity`) and were deliberately left alone:
each is a behaviour change to nodes that change did not otherwise touch.

## Input

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `MouseDelta` | `exec` (in), `then` (out), `deltaX`/`deltaY`/`wheel` (out, float) | — | W | One frame's mouse movement and wheel delta. |
| `MoveAxis` | `exec` (in), `then` (out), `forward`/`right` (out, float) | — | W | Polled forward/right movement axis (WASD-style). |
| `InputKey` | `key` (in, int), `down` (out, bool) | — | D | Whether a given key code is currently held. |
| `InputKeyPressed` | `key` (in, int), `triggered` (out, bool) | — | D | True only on the frame the key went **down**. |
| `InputKeyReleased` | `key` (in, int), `triggered` (out, bool) | — | D | True only on the frame the key came **up**. |
| `InputAction` | `action` (in, int), `x`/`y` (out, float), `held` (out, bool) | `action=` (optional) | D | A NAMED action's current value. `x`/`y` carry the axis components; a digital action reports 1/0 in `x`. |
| `InputActionPressed` | `action` (in, int), `triggered` (out, bool) | `action=` (optional) | D | True only on the frame the action went **down**. |
| `InputActionReleased` | `action` (in, int), `triggered` (out, bool) | `action=` (optional) | D | True only on the frame the action came **up**. |
| `SaveInputBindings` | `exec` (in), `then` (out), `success` (out, bool) | — | P | Writes every pushed `EnhancedInput` context's current bindings to `Settings.ini`. |
| `LoadInputBindings` | `exec` (in), `then` (out), `success` (out, bool) | — | P | Reloads every pushed context's bindings from `Settings.ini`, overwriting any in-memory rebinds. |
| `ResetInputBindings` | `exec` (in), `then` (out), `success` (out, bool) | — | P | Reverts every pushed context's bindings to its own declared defaults, in memory only (until `SaveInputBindings`). |
| `RebindAction` | `exec` (in), `slot`/`key` (in, int), `then` (out), `success` (out, bool) | `action=` (**required**) | P | Rebinds `action=<Name>`'s `slot`-th binding to physical `key` (a `Key`/`GamepadButton`/`GamepadAxis` enum int). Only a Key/GamepadButton/GamepadAxis slot is rebindable; `false` when the slot doesn't exist or `key` isn't a defined member of that slot's source enum. |
| `GetActionKey` | `slot` (in, int), `key` (out, int), `bound` (out, bool) | `action=` (**required**) | D | Which physical key/button/axis is bound to `action=<Name>`'s `slot`-th binding. `key` is `-1` (and `bound` is `false`) when that slot has no binding. |
| `GetPressedKey` | `key` (out, int), `pressed` (out, bool) | — | D | The lowest-valued key/mouse button down **this frame** -- what a "press a key to bind" rebinding prompt reads. `key` is `-1` (and `pressed` is `false`) when nothing is down. |

`MoveAxis` has no `z` pin — `Input.MoveAxis`'s own Z component is hardcoded `0` in
`Aver.Framework/Input.cs`, so a pin that could only ever read a compile-time constant would add
noise, not information (`OcGraphParser.cs:2250-2258`).

**THE `action` PIN TAKES A HANDLE, NOT A NAME.** The graph pin vocabulary has no string type, so an
action is addressed by the integer `aver_fw_action_register`/`aver_fw_action_find` returned -- exactly
as `InputKey`'s `key` pin takes a literal key code rather than `"W"`. Register the action once (from
the scheme asset or from C#) and feed the handle in through a `ConstInt`. A name pin would need a
string PinType across the parser, both compiler paths and the palette, which is a larger change than
this node needed.

**PREFER `InputAction` OVER `InputKey` for anything a player should be able to rebind** -- that is the whole point of the named layer. `InputKey` remains correct for a genuinely fixed key, and for content authored before actions existed; nothing about it changed.

**`action=` IS THE NAMED ALTERNATIVE TO THE `action` PIN**, on `InputAction`/`InputActionPressed`/
`InputActionReleased` only, and it is optional: give the node an `action=<Name>` NODE-line attribute
(the same mechanism `class=`/`event=`/`curve=` already use) and the compiler resolves the handle
itself at runtime (`GraphInterop.ActionHandleForGraph`, a per-name-cached `aver_fw_action_find`) --
the `action` pin is then ignored entirely. Leave it off and nothing changes: the `action` pin is read
exactly as it always was. `RebindAction` and `GetActionKey` have no pin fallback at all -- `action=`
is **required** on both, a compile-time error naming the node when it is missing, the same treatment
`Spawn`'s `class=` and `SaveGame`/`LoadGame`'s `path=` already get.

**THE REBINDING FAMILY** (`SaveInputBindings`/`LoadInputBindings`/`ResetInputBindings`/`RebindAction`/
`GetActionKey`/`GetPressedKey`) is what lets a graph-only project (no C# at all) offer Unreal Enhanced
Input-style rebinding: `GetPressedKey` reads what the player just pressed, `RebindAction` writes it
onto one action's one binding slot, and `SaveInputBindings`/`LoadInputBindings` persist or discard
that change. The first four are `P`-path, like `SaveGame`/`LoadGame` beside them -- none of the four
is safe to run unconditionally on every pull (`SaveInputBindings` writes to disk; the other three
overwrite every pushed context's live bindings). `GetActionKey`/`GetPressedKey` are pure reads (`D`),
InputKey's own shape: no exec pins, freely pullable from either compiler.

**`InputKey` answers a STATE and the other two answer an EVENT**, and the output pin names say so:
`down` versus `triggered`. `down` is true every frame a key is held, which is the wrong answer for
jumping, firing a semi-auto, or toggling anything — all of which want one true per press. Before
`InputKeyPressed` existed, building that meant a `DoOnce` and a variable per key, while the
framework ABI (`aver_fw_input_key_pressed` / `_released`, what `Input.GetKeyDown`/`GetKeyUp` wrap
for C#) had answered it directly all along.

`InputKey` is the odd one out in this category: no exec pins, `D` path, freely pullable from either
compiler. Reading one polled key is idempotent — it returns the same answer however often it's
asked within a frame — so there's nothing to cache and no exec visit needed to anchor it to
(`OcGraphParser.cs:2105-2117`). `MouseDelta`/`MoveAxis` are given `Raycast`'s exec-cached shape
instead, even though neither read has `Raycast`'s per-call *cost* — the reason is a correctness
guarantee, not a cost one: "one frame of input costs exactly one native call regardless of how many
output pins a graph reads" has no other enforcement mechanism in the PUSH compiler besides the same
exec-visit caching `Raycast` uses. `key` is a plain `int` with no
symbolic lookup — an author writes the numeric value from `Aver.Framework`'s `Key` enum directly.

## Param

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Param` *(alias `getparam`)* | `value` (out, type = the declared `PARAM`'s type) | `param=` | D | Reads one of the graph's own declared arguments. |

The `value` pin is added **only if `param=` resolves** to an actually-declared `PARAM`
(`OcGraphParser.cs:788-801`) — a missing or misspelled `param=` gets no pin at all, so a `LINK`/`OUT`
touching the node fails with `Graph.Validate()`'s specific "references undeclared parameter" message
rather than a generic "no output pin 'value'" (`Graph.cs:351-376`).

Which `PARAM` names are legal depends on which entry points the *whole file* wants, not per node:
- A dataflow graph (no `ENTRY` at all) accepts exactly `entity` (int) and `time` (float), by name,
  case-insensitively — anything else fails to load (`GraphHost.cs:256-275`).
- A graph wanting `OnStart` or `OnTick` accepts exactly `entity`, `time`, and `deltaTime`
  (`GraphHost.cs:326-373`). This check runs only when `OnStart`/`OnTick` is wanted; an on-demand-only
  event (see `OnHit` below) is unconstrained on its own — but since `Parameters` is one list for the
  whole file, mixing an on-demand event into a file that *also* has `OnStart`/`OnTick` forces every
  `PARAM` to fit that closed vocabulary too.

See trap #3 above for the editor-palette pin-type gap this node shares with `GetVar`.

## Flow

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Branch` | `exec` (in), `cond` (in, bool), `true` (out), `false` (out), `tookTrue` (out, bool) | — | X | Routes exec down `true` or `false` by `cond`. |
| `SwitchInt` | `exec` (in), `selector` (in, int), `case0`..`case3` (out), `default` (out), `taken` (out, int) | — | X | Routes exec to ONE of four cases by an integer, or `default`. `taken` reports which fired, **-1 for the default** so it does not depend on the case count. Emitted as a chain of compares, not an IL `switch` table: the selector is an arbitrary author-supplied int, and negative or far-out-of-range are ordinary inputs a dense table would not take. |
| `Sequence` | `exec` (in), `then0` (out), `then1` (out), `fireLog` (out, int) | — | X | Fires each exec output in order. |
| `While` | `exec` (in), `cond` (in, bool), `loop` (out), `done` (out), `iterations` (out, int) | — | X | Loops `loop` while `cond` holds; `cond` is re-checked every pass. |
| `ForEach` | `exec` (in), `count` (in, int), `loop` (out), `index` (out, int), `done` (out) | — | X | Counted-repeat loop, `count` times. |
| `DoOnce` | `exec` (in), `reset` (in, bool), `then` (out) | — | X | Fires `then` the first time only. A true `reset` re-arms it. |
| `Gate` | `exec` (in), `open` (in, bool), `close` (in, bool), `then` (out) | — | X | Passes exec through only while open. Starts **closed**, like Blueprint's own Gate. |
| `FlipFlop` | `exec` (in), `a` (out), `b` (out), `isA` (out, bool) | — | X | Alternates between `a` and `b`, starting with `a`. |
| `OnStart` | `exec` (out) | — | X | Entry point: fires once. |
| `OnTick` | `exec` (out) | — | X | Entry point: fires every tick after `OnStart` has fired. |
| `OnHit` | `exec` (out) | — | X | Entry point: fires only when something calls `GraphHost.Fire("OnHit", …)`. |
| `CustomEvent` | `exec` (out) | `name=` | X | Entry point under a name the author chooses. Fires on `GraphHost.Fire("<name>", …)`. |
| `RerouteFloat` | `a` (in, float), `result` (out, float) | — | D | Returns its input unchanged. A place to bend a wire. |
| `RerouteInt` | `a` (in, int), `result` (out, int) | — | D | As above, int. |
| `RerouteBool` | `a` (in, bool), `result` (out, bool) | — | D | As above, bool. |
| `RerouteExec` | `exec` (in), `then` (out) | — | X | As above, for the exec chain. |

**The four `Reroute*` nodes COMPILE TO NOTHING.** A data reroute emits its input and no
instruction of its own; the exec one needs no compiler case at all, because every exec node
ends with the same unconditional fan-out and a reroute is *only* that fan-out. Bending a wire
therefore costs a graph author nothing at run time, which is the only honest way to ship a node
that exists purely to be looked at.

**One per type, not one generic.** `Validate` refuses a link whose two pins disagree on type and there are no generic pins here, so a single wildcard `Reroute` would have to weaken that check — the check that catches every genuinely wrong wiring. Four nodes is the cheaper price.

**`DoOnce`, `Gate` and `FlipFlop` REMEMBER something between activations**, which no other node
here does — `Branch` and `Sequence` decide from their inputs alone. That memory lives in the same
per-instance `GraphVarStore` a `VAR` uses, under a reserved name built from the node id (`$flow$`
plus the id, and a `VAR` name cannot contain `$`), so two entities running one graph file gate
independently. **A graph that declares no `VAR` records has no store at all**, and these three
refuse to compile in one with a message naming the node: declare any variable to give it one.

`Gate`'s `open`/`close` are **bool inputs, not exec pins**. An exec input can be driven by many sources, so three separate exec entries would make "which one fired" unanswerable inside a single activation. Both are sampled every activation and applied before the test, so opening and firing in one activation works; `close` is applied after `open`, so a graph wiring both true ends closed — one stated rule rather than an order that depends on which link the parser read first. `DoOnce`'s `reset` is a bool for the same reason.

`CustomEvent` adds no runtime machinery at all, and that is the interesting part: the node type
has never been what makes an event fire, so a graph could always declare `ENTRY mine Whatever`
against a node of any type. What it could not do was say so from the palette. `name=` on the
NODE line is what the EDITOR shows and edits; nothing at runtime reads it, and the editor keeps
it in step with the `ENTRY` record (one function owns that — renaming one without the other
leaves a graph that looks renamed and has silently stopped firing).

What actually makes any of `OnStart`/`OnTick`/`OnHit` run is a top-level `ENTRY <nodeId>
<eventName>` record, not the node's type — the node type is just a labelled, no-input starting
shape an `ENTRY` record can point at. Nothing here is special-cased to the string `"OnHit"` either;
it is treated as one more on-demand event name, exactly as a project-invented event name would be
(`GraphHost.cs:24`'s own header comment: "'OnHit' is the worked example, not a special case").

**`OnHit` is still a demonstrated *mechanism*, not a wired collision hook** — no physics or collision
system raises it. A graph can declare an `OnHit` entry and a test harness can call `Fire("OnHit", …)`
directly (`Aver.Graph.Tests/OnHitAdversarialTests.cs`), but nothing connects a real collision to that
call yet.

**One engine system does now raise graph events, though: ANIMATION NOTIFIES.** A marker placed on a
clip in the animation editor names an event, and crossing it while the clip plays fires that name at
the playing entity’s graph — so a `CustomEvent` named `OnFootstep` runs on the frame the foot lands,
with no polling of the clip time from `OnTick`. The path is
`AnimSystem::tick` → a host-installed sink → `ScriptHost::graphFire` → `HostBridge.GraphFire` → the
same `FireEventRouter` a `FireEvent` node reaches, and it is proved end to end (a clip on disk making
a graph run) by `tests/anim.scene/src/AnimNotifyGraphTest.cpp`.

`Sequence` defaults to two exec outputs (`then0`, `then1`); add more by hand-writing additional
`PIN … out exec` records — the compiler fans out however many exec-output pins the node actually
has, in file order, needing no special case (`GraphCompiler.EmitExecFanOut`). `tookTrue` and
`fireLog` are opt-in observability, not required wiring — a way to prove which way a branch went or
how many times a sequence fired without a live scene to read back from.

`ForEach` is the **counted-repeat** variant, not a per-element iterator — the format has no
array/collection pin type, so "for each item in a list" cannot be expressed at all today
(`OcGraphParser.cs:743-749`, called out in the parser's own comment as "left rough for phase 2").

`While`/`ForEach` share one runaway-loop guard: `GraphCompiler.MaxLoopIterations = 100_000` (line 58). Past that many passes the loop stops and logs a warning naming the node and event (`WarnLoopGuardTripped`) — it does not hang or throw.

`OnStart` fires exactly once, on the **first** `Tick()` call (not inside `Load()`), strictly before
`OnTick` in that same call (`GraphHost.cs:125-138` explains the lazy-firing choice; `:553-568` is
where it happens).

## Actor

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `Spawn` | `exec` (in), `x`/`y`/`z` (in, float), `then` (out), `entity` (out, int) | `class=` | P | Creates a new entity of a registered class. |
| `Jump` | `exec` (in), `entity` (in, int), `then` (out), `jumped` (out, bool) | — | P | Calls `AverCharacter.Jump` on the actor bound to `entity`, which sets vertical velocity to jump speed only if the character is currently standing on something. |
| `CharacterMove` | `exec` (in), `entity` (in, int), `dt`/`forward`/`right`/`yawDelta`/`pitchDelta` (in, float), `then` (out), `success` (out, bool) | — | P | Drives an `AverCharacter`'s yaw/pitch and capsule velocity for one call. |
| `SetViewEntity` | `entity` (in, int) — **no output pin at all** | — | W | `aver_fw_set_view_entity(entity)`. |
| `FireEvent` | `exec` (in), `target` (in, int), `then` (out), `fired` (out, bool) | `event=` | P | Fires a named `ENTRY` event on **another entity's** graph. |

`FireEvent` is how one graph talks to another. It carries **no payload**: the receiving `ENTRY` gets the same closed `PARAM` vocabulary every entry gets (`entity`, `time`, `deltaTime`) and nothing else, so anything the sender wants understood must be staged into the target's own `VAR`s first — which is order-sensitive and worth designing deliberately rather than discovering.

`fired` is false, with a warning naming the reason, when the target does not exist, has no graph, or declares no such event. The warning is emitted **once per (target, event) pair**, not once per tick, because a mis-wired `FireEvent` on an `OnTick` chain would otherwise bury the log. Reentrancy is bounded by a depth guard: a graph firing at itself, two graphs firing at each other, and a three-deep cycle all terminate rather than overflowing the stack.

`Spawn` and `CharacterMove` are refused by `Compile()` **outright**, not merely skipped: a stray,
even unreachable, `Spawn`/`CharacterMove` node anywhere in a no-`ENTRY` dataflow graph fails to
compile with an explicit error naming the node
.
This is a full step stricter than the `W`-path writes in
[Scene](#scene): those are safe to run unconditionally once per invocation because writing the same
value twice is harmless; creating a new entity twice, or driving a character's velocity twice with
no gate, is not. Give either node an `ENTRY`-driven exec chain and reach it through
`CompileEntryPoint()` instead.

`class=` names the class to spawn **by name**, resolved at invocation time
(`GraphInterop.SpawnForGraph`), not baked to a handle at compile time — a class registered after
this graph compiles is still reachable the moment it's declared. `CharacterMove` has no attribute at
all: every one of its five scalar inputs is an ordinary pin, because a graph author computes
`dt`/`forward`/`right`/`yawDelta`/`pitchDelta` at *runtime* (a `PARAM`, a `MoveAxis`, a
`MouseDelta`), never chooses them at edit time the way `Spawn`'s class name is chosen.

`SetViewEntity` is the one node in the whole catalog with **zero** output pins — not even a fake
"success" — because its ABI call, `aver_fw_set_view_entity`, returns `void`
(`framework_abi.h:543`). There is no return code to surface, so none was invented
(`OcGraphParser.cs:2560-2567`).

`CharacterMove.success` is a real outcome, not a stub: `false`, logged, never a throw, when `entity`
isn't a live actor at all, or is a live actor that isn't an `AverCharacter`
(`OcGraphParser.cs:2295-2298`).

## Character

The gameplay API of `AverCharacter`, reached by entity handle. Every one of these takes an
`entity` pin rather than acting on "this" actor, because a graph class and the character it
drives are not the same entity — see [Transform](#transform) for the same convention applied
to scene entities generally.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetVelocity` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads an AverCharacter's own velocity, in centimetres per second, relative to the ground it stands on: a moving deck's or lift's motion is not included, so a character standing still on a train reads 0,0,0. **success is false and x/y/z stay 0 when the entity has no live AverCharacter bound to it, or is a character whose physics capsule hasn't been created yet (GraphInterop.cs:503-511) -- zero velocity and "no answer" read identically unless success is checked.** |
| `IsGrounded` | `entity` (in, int), `grounded` (out, bool) | — | D | Reports whether an AverCharacter is currently standing on ground shallow enough to hold. **Returns false, silently, for an entity that isn't an AverCharacter at all -- unlike GetVelocity/SetVelocity/Teleport it bypasses the CharacterFor helper and logs no warning (GraphInterop.cs:524-528).** |
| `SetVelocity` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out), `success` (out, bool) | — | P | Replaces an AverCharacter's velocity outright -- a launch pad, a dash, a dead stop, not an added force. The value is relative to the ground it stands on (on a moving deck 0,0,0 means stay put on the deck), and it also clears the motion a jump off moving ground carried away. **Does nothing and reports success = false on a character that has never been simulated (no physics capsule yet) -- unlike Teleport, it does not create one for you (GraphInterop.cs:516-522; Character.cs:120-138).** |
| `Teleport` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out), `success` (out, bool) | — | P | Moves an AverCharacter and its physics capsule to a feet position, clearing velocity. **Not SetFieldVec3 on CLocal.position -- per the palette's own comment, setting the transform alone leaves the capsule behind and the character snaps back next step. Teleport moves both and zeroes velocity, and -- unlike SetVelocity/GetVelocity -- works even on a character that was never simulated, because it calls EnsureCapsule() itself.** |

`GetVelocity`/`IsGrounded` are pure reads, reachable from either compiler exactly like `GetFieldVec3`/`GetForward` — no exec pins, and each has a matching case in *both* `Compile()`'s topological switch and `EmitPullOutput`. `SetVelocity`/`Teleport` are Actor-family side effects instead, but they are **not** refused the way `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` are — those four get their own hand-written, node-naming `InvalidOperationException` case inside `Compile()`'s own switch. `SetVelocity`/`Teleport` have no case there at all, so a stray, unwired one reached by `Compile()`'s topological pass (which walks every node in the graph, wired or not) falls through to the switch's `default` arm, which recognises a push-only node and refuses it with a node-naming `InvalidOperationException` saying the type IS supported and pointing at `CompileEntryPoint()`. The error a graph author actually sees in the common case — pulling `SetVelocity`/`Teleport`'s `success` output as data with no `exec` edge into the node — comes from a different place: `EmitPullOutput`'s own combined side-effect check, which groups `setvelocity`/`teleport` beside `Jump` and `FireEvent` (and `Print`, and the physics/transform writers) in one condition and raises a clear, node-naming `InvalidOperationException` there instead. Either way, `Compile()` cannot run them; only `CompileEntryPoint()`, through `EmitExecApiCall`, actually calls `SetVelocityForGraph`/`TeleportForGraph`.

A second trap sits in `Aver.Framework` itself, invisible from the pin table. `GetVelocity`/`SetVelocity`/`Teleport` all resolve their entity through the same `CharacterFor` helper, which logs one warning and returns null for anything that isn't a live `AverCharacter`; `IsGrounded` does not — it reads `Actors.Get` directly and returns `false` with no log for the identical case. And `GetVelocity`/`SetVelocity` both require an *already-simulated* character (`success = false`, and for `GetVelocity` the vector left at zero, when the physics capsule doesn't exist yet) — `Teleport` does not, because it calls the character's own `EnsureCapsule()` internally and so works, and creates the capsule, even on a character that has never been simulated.

## Game

The `Game` statics: who is playing, what they are possessing, and the two nodes that change that. `index` on the player nodes is a PLAYER index, not a pawn or entity index.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetPlayerPawn` | `index` (in, int), `entity` (out, int) | — | D | Reads the pawn currently possessed by a player's controller, as an entity handle. **`index` is a player index, not a pawn index. The value returned is whatever pawn that player's controller currently possesses (via `aver_fw_controlled_pawn`), so it comes back 0/`Entity.None` whenever that player is unpossessed right now, not only when `index` is out of range.** |
| `GetPlayerController` | `index` (in, int), `entity` (out, int) | — | D | Reads a player's controller entity by 0-based player index; `Entity.None` if there is none. |
| `GetGameMode` | `entity` (out, int) | — | D | Reads the current GameMode's entity; `Entity.None` outside a play session. **Returns `Entity.None` any time there is no play session (editor), not specifically when a GameMode class is missing — this pin alone can't tell "no session" apart from "session with a null GameMode".** |
| `IsPlaying` | `playing` (out, bool) | — | D | True only while a play session is actively ticking. **False while the session is Paused, not just in the editor — `State` has three values (Editor/Playing/Paused) and this pin collapses two of them to false. A graph that wants "is a session running at all" (paused or not) needs `Game.HasSession`, which no node currently wraps.** |
| `Possess` | `exec` (in), `controller` (in, int), `pawn` (in, int), `then` (out), `success` (out, bool) | — | P | Makes the entity at `controller` take control of the entity at `pawn`, stealing it from another controller if one already holds it. **`controller` must resolve (via `Actors.Get`) to an actor that IS an `AverPlayerController` — pass a plain entity or a pawn there and it logs a warning and returns `false` rather than throwing. Also, `Possess`/`Unpossess` have no case at all in EmitNode's own topological switch, and are not in IsExecOnlyNodeType either, so Compile() calls EmitNode on them for EVERY node in the graph regardless of wiring and reaches its `default` arm. A `Compile()`-driven graph merely CONTAINING a Possess/Unpossess/SetVelocity/Teleport node — wired or not — therefore fails to compile, which is deliberate; the arm names the node and says the type is supported rather than claiming it is not.** |
| `Unpossess` | `exec` (in), `controller` (in, int), `then` (out), `success` (out, bool) | — | P | Releases whatever pawn the entity at `controller` currently possesses. **`success` is false for two different reasons the pin can't distinguish: `controller` isn't an `AverPlayerController` at all, or it is one but had nothing possessed. Same EmitNode-has-no-case nuance as Possess above: any Compile()-driven graph merely containing an unwired Unpossess node fails outright with a generic "not supported" error, before its own more explicit PULL-refusal message would ever be reached.** |

All six nodes' pins agree exactly between the palette (`GraphNodeDefs.hpp:192-197`) and `AddDefaultPins` (`OcGraphParser.cs:1178-1209`) — no palette/parser disagreement to report here. The four readers (`GetPlayerPawn`, `GetPlayerController`, `GetGameMode`, `IsPlaying`) are the simplest possible `D` shape: zero or one input, one output, dispatched through the shared `EmitSimpleApiRead` on the PUSH side and a direct `EmitPullOutput` case on the PULL side, with no branch or refusal anywhere.

`Possess`/`Unpossess` are `P`-path by the task's own test — they sit in `IsExecCapableApiCallType`, which is OR'd into `IsPushOnlySideEffectType`'s refusal chain (the predicate `EmitPullOutput` consults) on the same line as `IsExecCapableJumpType` and `IsExecCapableFireEventType`. But unlike `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` — which get their own hand-written `case` in `EmitNode`'s topological switch that throws a tailored, node-naming `InvalidOperationException` explaining *why* — `Possess`, `Unpossess`, and their siblings `SetVelocity`/`Teleport` have **no case in `EmitNode` at all**. Since none of the four are in `IsExecOnlyNodeType` either, `Compile()`'s topological pass calls `EmitNode` on them unconditionally for every node in the graph, wired or not, and they reach its `default` arm — before `EmitPullOutput`'s own refusal ever gets a chance to run. The practical trap: a `Compile()`-driven (no-`ENTRY`) graph that merely *contains* an unreferenced `Possess`/`Unpossess`/`SetVelocity`/`Teleport` node fails to compile at all, with a plain "not supported" message rather than the friendlier "has a side effect and must be reached by wiring it directly into the exec chain" text the four `EmitNode`-cased side effects give. The end state is the same (P-path, refused outright) but the diagnostic a graph author actually sees is worse for this family than for Spawn/CharacterMove/FireEvent/SetVar.

## Convert

**Conversion nodes are not a convenience.** `Graph.Validate()` compares `srcPin.Type` to
`tgtPin.Type` and refuses a mismatch outright, so an `int` or `bool` output cannot reach a
`float` input at all — every entity handle was unprintable and every bool uncomparable until
these three existed. Unreal converts silently; here the cast has to be a node you can see.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `IntToFloat` | `a` (in, int), `result` (out, float) | — | D | Widens an int to a float. **Lossy above 2^24 — float32 has only 24 mantissa bits, so past 16777216 only even integers survive, and entity handles start exactly at 16777216. Converting a handle through this node silently rounds it to a neighbouring value; use `PrintInt` (not `Print` fed from this node) when the value being converted is a handle.** |
| `BoolToFloat` | `a` (in, bool), `result` (out, float) | — | D | Converts a bool to float: `true` becomes `1.0`, `false` becomes `0.0`. |
| `FloatToInt` | `a` (in, float), `result` (out, int) | — | D | Truncates a float to int, toward zero — the same rounding C#'s `(int)f` cast gives. **Truncates TOWARD ZERO, not floor — `-1.5` becomes `-1`, not `-2`. `Floor` (see Math) exists for the other rounding, specifically so an author never has to guess which one a given node gives them.** |

All three share one code path both ways: `EmitNode`'s topological switch dispatches every one of them to the same `EmitSimpleApiRead` helper, which does nothing but call `EmitPullOutput` per output pin and store the result — there's no separate "why it's safe to run unconditionally" story to learn here, because these are arithmetically identical to `RerouteFloat`/`RerouteInt`/`RerouteBool`: one input read, one conversion opcode, no side effect. `IntToFloat` and `BoolToFloat` even compile to the literal same instruction — `int` and `bool` are both `I4` on the CIL stack, so widening either to `float` is one `Conv_R4` either way; the two node types exist so the GRAPH can tell them apart, not because the generated code differs. None of the three appear in either compiler's refusal list or exec-only dispatch, so — like every other `Convert` node — they're freely pullable from a dataflow graph with no `ENTRY` at all, and freely usable inside a PUSH chain too, with no wiring requirement beyond having something feed `a`.

## Transform

Reads and writes on a scene entity's TRANSFORM, plus the two liveness queries. **Local is not world**: `GetFieldVec3 CLocal.position` only ever gave the local half, and a parented object's local position never changes as its parent moves. The axis nodes report the TRANSFORM's orientation, deliberately not `GetForward`, which is an `AverCharacter`'s look direction with its pitch clamp already applied.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetWorldPosition` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads an entity's WORLD-space position, composed through any parents, in centimetres. **LOCAL IS NOT WORLD: `GetFieldVec3` on `CLocal.position` reads local position only — a gun parented to a moving camera has the same local position forever. This node is the one that tracks where the entity actually is (GraphNodeDefs.hpp's own comment, lines 167-171).** |
| `GetEntityForward` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space forward axis (+X), composed through parents and normalised. **Not the same node as the existing `GetForward` (palette label "Get Forward (Look)", category Scene, GraphNodeDefs.hpp:311): that one reads an `AverCharacter`'s look direction including its pitch clamp. This node reads the raw WORLD-space +X axis of ANY entity's transform — the GraphInterop method's own comment says so explicitly (Aver.Framework/GraphInterop.cs:489-490).** |
| `GetEntityRight` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space right axis (+Y), composed through parents and normalised. |
| `GetEntityUp` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space up axis (+Z), composed through parents and normalised. |
| `GetLocalScale` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's local scale multipliers (unitless). **On a dead/invalid entity this reports `x=y=z=0, success=false` — NOT `Entity.LocalScale`'s own default of `(1,1,1)` a raw C# read would give. GraphInterop.LocalScaleForGraph checks `IsAlive` and zeroes the outputs BEFORE ever touching the property (GraphInterop.cs:619-627).** |
| `IsAlive` | `entity` (in, int), `alive` (out, bool) | — | D | Whether the entity handle still names a live entity in the scene. **Asks the SCENE whether the handle still names anything, not merely whether it is non-zero — `Entity.IsValid` (handle != 0) is a weaker, different check. A graph holding a handle across frames wants IsAlive (GraphInterop.cs:866-868's own comment).** |
| `IsActor` | `entity` (in, int), `isActor` (out, bool) | — | D | Whether a gameplay class owns this entity, rather than it being a plain scene node. |
| `Translate` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Moves the entity by (x, y, z) centimetres, added to its current LOCAL position — a relative nudge, not an absolute set. **INCREMENTAL, not absolute, and LOCAL space, not world: `Entity.Translate(delta) => SetLocalPosition(LocalPosition + delta)` (Entity.cs:69). Moving along a world direction (e.g. `GetEntityForward`'s output) needs scaling that vector by a speed and wiring the result in yourself — this node does not convert a world-space delta for you.** |
| `SetLocalPosition` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the entity's local position outright — the ABSOLUTE peer to `Translate`'s relative nudge, and parent-relative like `Translate`, not world space. **A real, fully-wired node (`OcGraphParser.cs:1301-1308`, `GraphInterop.SetLocalPositionForGraph`, `GraphCompiler.cs`'s `"setlocalposition"` case). It exists specifically so a first-person viewmodel parented to the view camera can hold a fixed offset from it (`GraphInterop.cs:838-849`, the node's own doc comment) — `SetBodyPosition` is world-space and physics, and `Translate` cannot express "always this exact offset" without first reading the current position back out.** |
| `SetLocalScale` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the entity's local scale multipliers outright. **An absolute 'set the value' write — the same shape as `SetFieldVec3`, which the parser runs safely unconditionally under Compile() (W, see Scene table) — yet `SetLocalScale` is bucketed with `Translate`/`DestroyEntity` as P because all three share one emitter (`EmitExecTransformWrite`) and one predicate. Compile() refuses it outright even though overwriting the same scale twice would in fact be harmless.** |
| `DestroyEntity` | `exec` (in), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Destroys the entity and its subtree — the full framework teardown if it's an actor, a plain scene delete otherwise. **`success` is `false`, not a throw, when the entity is already dead (GraphInterop.cs:874-879 checks `IsAlive` first). Destroying an actor runs the full framework teardown (`Fw.aver_fw_destroy`); a plain scene entity gets a lighter `aver_scene_destroy` (Entity.cs:175-179) — either way the whole subtree goes with it, so a child spawned under an actor does not need its own `DestroyEntity` node.** |

Ten of these eleven types were added together (GraphNodeDefs.hpp's own comment, near line 173, calls this out as "THE ENTITY TRANSFORM, which `GetFieldVec3` on `CLocal.position` only half covered") — `SetLocalPosition` (see above) landed separately, later, closing the one gap that batch left behind. The palette and `AddDefaultPins` agree, pin-for-pin, on every one of them — no mismatches found in this family.

The seven readers (`GetWorldPosition`, `GetEntityForward`/`Right`/`Up`, `GetLocalScale`, `IsAlive`, `IsActor`) are all dispatched the same way twice over: `EmitNode`'s dataflow switch routes all seven to one shared case (`EmitSimpleApiRead`), which itself calls `EmitPullOutput` for every output pin rather than computing anything directly — so the *real* per-type logic lives once, in `EmitPullOutput`'s own switch (no `GraphCompiler.cs` line numbers given for either — see the note at the top of this document). `GetEntityForward`/`Right`/`Up` share a single native surface, `GraphInterop.EntityAxisForGraph(entity, axis, ...)`, differing only in the axis index (0/1/2) the emitter hardcodes per node type — three node types, one method, by design (`GraphInterop.cs:604-609`). All seven readers zero their outputs and report `success=false` on a dead handle *before* ever touching the underlying `Entity` property, which matters because two of those properties have friendlier fallbacks a raw C# caller would see instead: `Entity.LocalScale` defaults to `(1,1,1)`, not `(0,0,0)`, and `Entity.WorldForward`/`Right`/`Up` fall back to the local (unrotated-by-world) axis rather than reporting failure. A graph reading these nodes never sees either fallback — dead means zero, full stop.

The three writers (`Translate`, `SetLocalScale`, `DestroyEntity`) are **P**, but by a different mechanism than `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` get. Those four each have their own explicit `case` in `EmitNode`'s topological switch that throws a hand-written `InvalidOperationException` naming the node and explaining why (look up the `"spawn"`/`"charactermove"`/`"firevent"`/`"setvar"` cases by name). `Translate`/`SetLocalScale`/`DestroyEntity` have **no case at all** in that switch, so `Compile()` reaches its `default` arm instead. The practical effect is identical (Compile() refuses a graph carrying any of the three, wired or not), and the message is not generic: the default arm tests for a push-only node first, and only then falls back to "not supported". Separately, all three also sit in `EmitPullOutput`'s side-effect refusal list via `IsExecCapableTransformWriteType`, so pulling one's `success` (or, for `Translate`/`SetLocalScale`, any output) as a bare data value from inside a PUSH graph with no exec edge fails with the same named `InvalidOperationException` the Scene writers give.

A fourth writer, `SetLocalPosition`, has the identical shape and the identical `P`-path reasoning — see the table above.

## Physics — reads and queries

The `Body` handle and the queries over it. A `Body` is a physics handle, distinct from an
entity handle — both arrive as `int` pins, and nothing in the type system stops you wiring one
where the other belongs, which is the trap to know about in this whole family.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetBodyPosition` | `body` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads a physics body's centre-of-mass position, in centimetres. **`success` only means the handle is nonzero (`Body.IsValid` is literally `Handle != 0`, no native check — `Aver.Framework/Physics.cs:116`) — after `DestroyBody` runs, the same handle still reads `success = true` with `x = y = z = 0.0` (Body.Position's own silent-zero-on-native-failure — `Aver.Framework/Physics.cs` now shims this straight through to `Aver.Physics/Body.cs:33-40`, where the actual native-call-fails-so-return-Float3.Zero logic lives), indistinguishable from a live body genuinely sitting at the origin.** |
| `GetBodyVelocity` | `body` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads a physics body's linear velocity, in centimetres per second. **Same trap as `GetBodyPosition`: `success` only proves the handle is nonzero, not that the body still exists — a destroyed body's handle reads `success = true`, `x = y = z = 0.0`, indistinguishable from a live body at rest (`Aver.Framework/Physics.cs:116`, and `Aver.Physics/Body.cs:53-60` for the Velocity fallback).** |
| `IsBodyValid` | `body` (in, int), `valid` (out, bool) | — | D | Reports whether a body handle currently names a body. **Checks only that the handle number is nonzero — it never calls into native physics, so a handle from a body already removed via `DestroyBody` still reads `valid = true` (`Aver.Framework/Physics.cs:116`).** |
| `GetBodyCount` | `count` (out, int) | — | D | Returns how many physics bodies currently exist in the simulation. |
| `IsPhysicsReady` | `ready` (out, bool) | — | D | `Physics.Ready` — whether `aver_phys_init` has run. A graph that adds bodies before it does gets silent zeros back from every creator, and until this node existed had no way to ask. |
| `GetFixedStep` | `seconds` (out, float) | — | D | `Physics.FixedStep`. What a graph integrating anything by hand should use instead of a hardcoded `1/60`, so it matches the simulation it runs alongside. |
| `RaycastAny` | `originX`/`originY`/`originZ` (in, float), `dirX`/`dirY`/`dirZ` (in, float), `maxDist` (in, float), `hit` (out, bool) | — | D | Casts a ray up to `maxDist` centimetres and reports only whether it hit anything, discarding the point, normal, and owner a full raycast would give. **Exposes no `body`/point/owner pin at all — even on a hit there is no way to learn what was struck from this node alone; reach for `Raycast` (scene entity) or `SphereCast` (physics body) when the graph needs to know what it hit, not just that something is there.** |
| `SphereCast` | `exec` (in), `originX`/`originY`/`originZ` (in, float), `dirX`/`dirY`/`dirZ` (in, float), `maxDist` (in, float), `radius` (in, float), `then` (out, exec), `hit` (out, bool), `body` (out, int), `pointX`/`pointY`/`pointZ` (out, float) — 15 pins | — | P | Sweeps a sphere of `radius` outward from the origin along the given direction, up to `maxDist` centimetres, and reports the first physics body it touches. **The sweep never resolves an owning entity (`Physics.SphereCast`'s `RaycastHit.Entity` is hardcoded to `Entity.None` — see the `RaycastHit.Entity` doc comment and the `SphereCast` method itself, `Aver.Framework/Physics.cs`) -- only the physics `body` handle comes back on this node's pins; stamp one with `SetBodyEntity` first if a graph needs the owner. And because SphereCast has no case at all in `EmitNode`'s switch (unlike `Raycast`, which does), a data-only graph merely containing an unwired SphereCast node fails Compile() with the bare 'Node type 'SphereCast' is not supported' -- not the purpose-written 'must be reached by wiring it into the exec chain' message Spawn/CharacterMove/FireEvent/SetVar give; give it an ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.** |

The physics family splits cleanly along the line `OcGraphParser.cs:1150`'s own comment states outright — *"Readers are pure; writers, creators and the sweep carry exec pins"* — and the palette agrees with the parser pin-for-pin on all six nodes here, so there is nothing to log in `mismatches`. `GetBodyPosition`, `GetBodyVelocity`, `IsBodyValid`, `GetBodyCount` and `RaycastAny` have no exec pins and are freely pullable from either compiler, `RaycastAny` included even though it runs a full `Physics.Raycast` internally on every pull and only keeps the bool — it simply has no exec pins to be cached against the way the Scene category's `Raycast` does. `SphereCast`, "the sweep", is the odd one out, and worth knowing before you hit it: it is not merely exec-only like `Branch`/`OnTick` (which `Compile()` silently skips), and it is not in the same handled bucket as `Raycast`, `MouseDelta`, `MoveAxis` either (all three of which DO have a case in `EmitNode`'s topological switch, so `Compile()` runs them once, unconditionally). `SphereCast` has no case anywhere in `EmitNode`, so a pure-dataflow graph merely *containing* one — wired or not — fails to compile, reaching the `default` arm rather than a hand-written case. The message it gets there names the node and points at `CompileEntryPoint()`, the same as the four hand-written refusals, just without their node-specific wording.

Separately: `IsBodyValid` and the `success` pin on `GetBodyPosition`/`GetBodyVelocity` all bottom out in `Body.IsValid`, which is literally `Handle != 0` (`Aver.Framework/Physics.cs:116`) — none of the three ever asks native physics whether that handle still names a live body. A handle read after `DestroyBody` has already run reports `valid = true`, and `GetBodyPosition`/`GetBodyVelocity` on that same handle report `success = true` with `x = y = z = 0.0` (`Body.Position`/`Velocity`'s own native call fails silently to `Vec3.Zero`, with nothing upstream re-checking it) — a destroyed body reads exactly like one that is very much alive and sitting at the origin. `Aver.Framework/Physics.cs` is a shim over `Aver.Physics` (its own file comment: "a SHIM OVER Aver.Physics, not a second P/Invoke surface") — `Body.Position`/`Velocity` there forward to `Aver.Physics/Body.cs`, where the actual native call and zero-fallback live.

## Physics — writes and creation

Writes, and the five creators. Every one of these is `P`: a physics write ticked unconditionally by the dataflow compiler would run on every invocation with no way to gate it, and a CREATOR would make a new body every frame.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetBodyPosition` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Teleports a body to a new position, in centimetres, ignoring collision on the way. **Moves through walls -- there is no collision response on the way, unlike driving the body by velocity.** |
| `SetBodyVelocity` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Replaces a body's linear velocity outright, in centimetres per second. |
| `AddBodyVelocity` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Adds to a body's current velocity -- a plain velocity change, not a mass-scaled impulse, so a heavy and a light body get the identical speed change for the same x/y/z input. **Reads like an impulse node (knockback, explosion, jump pad), but there is no mass division anywhere in the call chain -- it is SetVelocity(Velocity + delta), full stop. A 1 kg crate and a 1000 kg crate both get exactly the same delta-v.** |
| `DestroyBody` | `exec` (in), `body` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Removes a body from the simulation; the handle is dead afterwards. |
| `SetBodyEntity` | `exec` (in), `body`, `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Stamps a scene entity as a physics body's owner, so a later Raycast against that body reports the entity through `RaycastHit.Entity` instead of 0 (unowned). **A body created by AddStaticBox/AddDynamicBox/AddDynamicSphere/AddSensorBox/AddSensorSphere reports NO owner to Raycast (entity 0, read as 'unowned') until this node runs on it -- the palette's own Physics section comment calls this out explicitly (GraphNodeDefs.hpp:183-184).** |
| `SetGravity` | `exec` (in), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the whole simulation's gravity vector, in centimetres per second squared. The engine default is (0, 0, -980). **`success` is always true here -- SetGravityForGraph has no failure path to report (unlike the other five writers, whose `success` reflects whether `body` was a valid handle), because setting a global has nothing to be invalid against.** |
| `AddStaticBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box collider that never moves, centred at (cx, cy, cz) with half-extents (hx, hy, hz), both in centimetres. |
| `AddDynamicBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz`, `mass` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box collider that falls and collides, with `mass` in kilograms; `mass` <= 0 derives the mass from the box's volume instead. **Leaving `mass` unwired is not an error -- an unwired float pin reads 0, which is exactly the "derive mass from volume" sentinel, not a zero-mass body (GraphInterop.cs:957-960).** |
| `AddDynamicSphere` | `exec` (in), `cx`, `cy`, `cz`, `radius`, `mass` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a sphere collider that falls and collides, `radius` in centimetres; `mass` <= 0 derives the mass from the sphere's volume instead. **Same unwired-mass rule as AddDynamicBox: 0 (an unwired pin's default) means "derive from volume", not "massless".** |
| `AddSensorBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box-shaped trigger volume that reports overlaps but never collides physically. |
| `AddSensorSphere` | `exec` (in), `cx`, `cy`, `cz`, `radius` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a spherical trigger volume that reports overlaps but never collides physically. |

All eleven are `P`-path for the same reason `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` are: each wraps a Jolt call that either mutates simulation state (`SetBodyPosition`, `SetBodyVelocity`, `AddBodyVelocity`, `DestroyBody`, `SetBodyEntity`, `SetGravity`) or creates a new body (`AddStaticBox`, `AddDynamicBox`, `AddDynamicSphere`, `AddSensorBox`, `AddSensorSphere`), and Compile()'s topological pass has no branch structure to gate any of that with. They are reached only from `EmitExecNode`'s default arm, through `IsExecCapablePhysicsWriteType`/`IsExecCapablePhysicsCreateType` dispatching to `EmitExecPhysicsWrite`/`EmitExecPhysicsCreate` -- give any of them an `ENTRY`-driven exec chain and reach them through `CompileEntryPoint()`.

A **body is not an entity** (`GraphNodeDefs.hpp:242-245`): a body is a Jolt handle with a shape and a velocity; an entity is a scene node that may or may not own one. A body an `Add*` node creates reports **no owner** to `Raycast` until `SetBodyEntity` stamps one on -- a trigger the graph just built is invisible to the graph asking what it hit. Body handles ride on `int` pins throughout, same convention as scene entity handles, and `0` means invalid on both sides (`Aver.Framework/Physics.cs:105-116`).

The five writers share one dispatch shape: `EmitExecPhysicsWrite` pulls `body` first for all of them except `SetGravity`, then the per-type inputs, then stores the returned bool into `success` or drops it. The five creators share the mirror shape: `EmitExecPhysicsCreate` always pulls `cx`/`cy`/`cz` first, then per-type extents/radius/mass, and stores the returned handle into an **exec-local**, not a pin-local -- the same reasoning `Raycast` uses, so the `body` pin any downstream node reads back is the *one* handle this creation produced, however many nodes read it. `AddStaticBox`/`AddSensorBox`/`AddSensorSphere` have no `mass` pin at all: a static body is immovable and a sensor never participates in dynamics, so mass has nothing to mean for either.

`AddBodyVelocity`'s name invites reading it as an impulse -- knockback, an explosion, a jump pad -- but `Body.AddVelocity` carries no mass math anywhere in the chain: the C# side (`Aver.Physics/Body.cs:91`) is a one-line forward to `aver_phys_body_add_velocity`, and the native implementation (`modules/physics/src/PhysicsBody.cpp:128-132`) calls Jolt's `BodyInterface::AddLinearVelocity` directly -- a plain velocity add, not `AddImpulse`, so there is no scaling by mass anywhere in the call. `SetBodyVelocity` replaces the velocity outright; `AddBodyVelocity` adds to whatever it already had. Neither one is mass-aware.

## Function

The three node types a **user-defined function** is made of. None is placed from the palette — the
Functions panel creates them, because each needs to know which function it belongs to before it has any
pins at all. See [`formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md) for the `FUNC`/`FUNCIN`/
`FUNCOUT` records they read their shape from.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `FuncEntry` | `then` (out, exec, impure only), then one output per `FUNCIN` | `func=` | X | Where a function begins. Its output pins **are** the enclosing method's arguments. |
| `FuncReturn` | `exec` (in, impure only), then one input per `FUNCOUT` | `func=` | X | Stores the function's results. It does **not** return — a function has one `Ret`, after the whole body, so branches converge here. Exactly one per function. |
| `CallFunc` | `exec` (in) / `then` (out) when the callee is impure, one input per `FUNCIN`, one output per `FUNCOUT` | `call=`, and `func=` for where it lives | D for a pure callee, P for an impure one | Calls a function by a direct IL call. |

**All three take their pins from a declaration, not from a fixed list**, which is the same thing
`Param`/`GetVar`/`SetVar` already do. That is what makes a function's signature a single source of truth:
change a `FUNCIN` and every call node's pins change with it, in both readers — `AddDefaultPins` on the
C# side, `GraphEditor::resyncFunctionNodePins` on the C++ side. Those derived pins are **never written to
the file**: they would be a second copy of the signature, free to disagree with the first after any edit.

**A PURE FUNCTION CANNOT RECURSE, and this is worth knowing before you try.** A pure function has no exec
pins, so its only way to choose between a base case and a recursive step is `Select` — and `Select`
evaluates **both** of its sides regardless of its condition (see [Flow](#flow); it is not short-circuiting
the way `Branch`'s exec fan-out is). So a pure recursive function takes its recursive step on every call,
including the base case, and never terminates. `Branch`, on the exec chain, is the only thing in this
vocabulary that genuinely does not evaluate the path it did not take — so **recursion needs an impure
function**. A runaway one is stopped by `GraphCallGuard` at depth 120 with an error naming the function,
rather than a `StackOverflowException`, which cannot be caught and would take the process down.

## Var

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetVar` | `value` (out, type = the declared `VAR`'s type) | `var=` | D | Reads a graph-local variable. |
| `SetVar` | `exec` (in), `value` (in, type = the declared `VAR`'s type), `then` (out) | `var=` | P | Writes a graph-local variable. |

Same conditional-pin rule as `Param`: `value` is added only if `var=` resolves to a declared `VAR`;
otherwise the node gets no pin and `Graph.Validate()` reports the real reason
. Same editor-palette placeholder-type trap as `Param`
too — see trap #3 above.

`GetVar` is a pure read. `SetVar` is `P`-path, refused by
`Compile()` outright for the identical reason `Spawn`/`CharacterMove` are: a write has no "same value
twice is harmless" excuse the way a native field write does, so an ungated `SetVar` in a dataflow
graph would overwrite the variable every single invocation with no way to stop it. `SetVar` has no `success` output — a write into an in-process
`GraphVarStore` has no runtime failure mode the way a native field write does (unknown entity,
missing component), so there is nothing left to report.

A `VAR`'s value lives on the owning `GraphHost` and survives from one compiled-delegate invocation
to the next on that same host — but not a `Load()` call, hot-reload included, and not a process
restart. The full storage/lifetime story belongs to the guide, not this reference; this table only
covers the node's pin shape.

---

## Game systems

> **Pins are authoritative in [`docs/nodes/`](nodes/README.md).** The tables in this section are the prose companions of the generated pages: they explain behaviour and give a quick overview, but a pin name, type, default or attribute is whatever the generated page for the family says.

Timers and events, the behaviour-tree blackboard, streamed audio, animation state machines, game UI, prefabs, decals, crowds/hearing/cover, gameplay abilities, input actions, data tables, montages, AI queries, level instances, landscape/water, cloth and destruction, accessibility, blend shapes and the families under [Math, game flow and the rest](#math-game-flow-scene-queries-and-the-rest) below. These are **table-driven**: one row each in `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` (or the family's own `Graph<Family>NodeTable.cs`) names the `...ForGraph` method it calls and which pin feeds which parameter, so the parser's default pins and the compiler's emitter come from the row (`GameSystemNodes`, `GraphCompilerGameSystems.cs`). String arguments are NODE-line attributes (`key=`, `event=`, `sound=`, ...), required at compile time unless noted. Each family has its own table file (`Graph<Family>NodeTable.cs`) and its own palette file (`sandbox/src/GraphNodeDefs<Family>.hpp`); the table is found by reflection and the palette by CMake, so a family adds files and edits no shared list (see [Adding a node family](VISUAL_SCRIPTING.md#adding-a-node-family)). An attribute marked *(optional)* may be left out; every other one fails the compile by name when missing.



### Events



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_OnEvent` | `exec` (out, exec) | — | X | Entry node: runs when the bus event named by its `ENTRY` reaches this entity (`ENTRY e Damaged` + `NODE e AN_OnEvent`). |

| `AN_SetTimer` | `exec` (in, exec), `target` (in, int), `delay` (in, float), `looping` (in, bool), `then` (out, exec), `handle` (out, int) | `event=` | P | Fires graph event `event=` on `target` after `delay` seconds, repeating when `looping`. `handle` is 0 if nothing was set. Owned by `target`. |

| `AN_ClearTimer` | `exec` (in, exec), `handle` (in, int), `then` (out, exec), `cleared` (out, bool) | — | P | Cancels a timer by handle; `cleared` is true if it was still scheduled. |

| `AN_DispatchEvent` | `exec` (in, exec), `sender` (in, int), `target` (in, int), `i` (in, int), `f` (in, float), `immediate` (in, bool), `then` (out, exec) | `event=` | P | Raises bus event `event=` with payload `[i, f]`; deferred to the frame's flush unless `immediate`. |

| `AN_EventPayload` | `exec` (in, exec), `index` (in, int), `then` (out, exec), `sender` (out, int), `target` (out, int), `i` (out, int), `f` (out, float), `b` (out, bool) | — | P | Reads argument `index` of the event whose `AN_OnEvent` chain is running, coerced. Zero outside one. |



### Blackboard



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `GetBlackboardFloat` | `entity` (in, int), `value` (out, float), `success` (out, bool) | `key=` | D | Reads the board key `key=` on the entity's behaviour-tree blackboard (agent board, then its team board). |

| `SetBlackboardFloat` | `exec` (in, exec), `entity` (in, int), `value` (in, float), `then` (out, exec), `success` (out, bool) | `key=` | P | Writes the board key `key=`; observers fire only on a real change. |

| `GetBlackboardInt` | `entity` (in, int), `value` (out, int), `success` (out, bool) | `key=` | D | Reads the board key `key=` on the entity's behaviour-tree blackboard (agent board, then its team board). |

| `SetBlackboardInt` | `exec` (in, exec), `entity` (in, int), `value` (in, int), `then` (out, exec), `success` (out, bool) | `key=` | P | Writes the board key `key=`; observers fire only on a real change. |

| `GetBlackboardBool` | `entity` (in, int), `value` (out, bool), `success` (out, bool) | `key=` | D | Reads the board key `key=` on the entity's behaviour-tree blackboard (agent board, then its team board). |

| `SetBlackboardBool` | `exec` (in, exec), `entity` (in, int), `value` (in, bool), `then` (out, exec), `success` (out, bool) | `key=` | P | Writes the board key `key=`; observers fire only on a real change. |

| `GetBlackboardEntity` | `entity` (in, int), `value` (out, int), `success` (out, bool) | `key=` | D | Reads the board key `key=` on the entity's behaviour-tree blackboard (agent board, then its team board). |

| `SetBlackboardEntity` | `exec` (in, exec), `entity` (in, int), `value` (in, int), `then` (out, exec), `success` (out, bool) | `key=` | P | Writes the board key `key=`; observers fire only on a real change. |

| `GetBlackboardVec3` | `entity` (in, int), `x` (out, float), `y` (out, float), `z` (out, float), `success` (out, bool) | `key=` | D | Reads the Vec3 key `key=` as x, y, z. |

| `SetBlackboardVec3` | `exec` (in, exec), `entity` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `then` (out, exec), `success` (out, bool) | `key=` | P | Writes the Vec3 key `key=`. |



### Audio



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_PlayStream` | `exec` (in, exec), `volume` (in, float), `pitch` (in, float), `looping` (in, bool), `bus` (in, int), `fadeInSeconds` (in, float), `then` (out, exec), `voice` (out, int), `success` (out, bool) | `sound=` | P | Streams the `sound=` file from disk (no whole-file decode), optionally fading in. |

| `AN_PlayStreamAt` | `exec` (in, exec), `x` (in, float), `y` (in, float), `z` (in, float), `volume` (in, float), `pitch` (in, float), `looping` (in, bool), `bus` (in, int), `innerCm` (in, float), `outerCm` (in, float), `fadeInSeconds` (in, float), `then` (out, exec), `voice` (out, int), `success` (out, bool) | `sound=` | P | The positioned counterpart of `AN_PlayStream`. |

| `AN_PlayMusic` | `exec` (in, exec), `volume` (in, float), `fadeSeconds` (in, float), `looping` (in, bool), `curve` (in, int), `then` (out, exec), `voice` (out, int), `success` (out, bool) | `sound=` | P | Starts `sound=` as the current music track, crossfading from the previous one over `fadeSeconds` (`curve`: 0 linear, 1 equal power). |

| `AN_StopMusic` | `exec` (in, exec), `fadeSeconds` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Fades the current music track out and ends it. |

| `AN_IsMusicPlaying` | `playing` (out, bool) | — | D | True while a music track is current. |

| `AN_FadeSound` | `exec` (in, exec), `voice` (in, int), `targetGain` (in, float), `seconds` (in, float), `curve` (in, int), `stopWhenDone` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Ramps a voice's fade gain to `targetGain` over `seconds`; `stopWhenDone` ends the voice at the end. |

| `AN_SetVoiceOcclusion` | `exec` (in, exec), `voice` (in, int), `occlusion` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets a voice's occlusion (0 clear .. 1 solid): low-pass plus a volume floor. |

| `AN_SetReverb` | `exec` (in, exec), `wet` (in, float), `decaySeconds` (in, float), `damping` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the global send reverb. |

| `AN_AttachAudioOcclusion` | `exec` (in, exec), `entity` (in, int), `voice` (in, int), `rayCount` (in, int), `probeRadiusCm` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Makes the entity drive `voice`'s occlusion from line-of-sight rays to the listener. |

| `AN_SetReverbZone` | `exec` (in, exec), `entity` (in, int), `halfX` (in, float), `halfY` (in, float), `halfZ` (in, float), `blendDistanceCm` (in, float), `wet` (in, float), `decaySeconds` (in, float), `damping` (in, float), `priority` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Makes the entity a box reverb volume that fades out over `blendDistanceCm`; higher `priority` wins where zones overlap. |



### Animation



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `SetAnimGraph` | `exec` (in, exec), `entity` (in, int), `playRate` (in, float), `then` (out, exec), `success` (out, bool) | `machine=` | P | Binds the `machine=` state machine (`.ocasm`) to the entity. |

| `SetAnimParamFloat` | `exec` (in, exec), `entity` (in, int), `value` (in, float), `then` (out, exec), `success` (out, bool) | `param=` | P | Sets the state machine's `param=` float or int parameter. |

| `SetAnimParamBool` | `exec` (in, exec), `entity` (in, int), `value` (in, bool), `then` (out, exec), `success` (out, bool) | `param=` | P | Sets the state machine's `param=` bool parameter. |

| `TriggerAnim` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `param=` | P | Raises the `param=` trigger; it is consumed by the transition that takes it. |

| `IsInAnimState` | `entity` (in, int), `result` (out, bool) | `state=` | D | True while the entity's machine is in `state=` (or inside the sub-machine of that name). |

| `GetAnimStateTime` | `entity` (in, int), `time` (out, float) | — | D | Normalised time of the active state. |



### UI



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_OpenUiLayout` | `exec` (in, exec), `then` (out, exec), `layout` (out, int), `success` (out, bool) | `path=` | P | Instantiates the `path=` `.ocui` layout; `layout` is its root widget. |

| `AN_CloseUiLayout` | `exec` (in, exec), `layout` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Destroys a layout's widgets. |

| `AN_FindUiWidget` | `exec` (in, exec), `layout` (in, int), `then` (out, exec), `widget` (out, int), `success` (out, bool) | `name=` | P | Finds the widget named `name=` (under `layout`, or anywhere when 0). |

| `AN_CreateUiWidget` | `exec` (in, exec), `parent` (in, int), `kind` (in, int), `then` (out, exec), `widget` (out, int), `success` (out, bool) | `name=` | P | Creates a widget of `kind` (0 panel .. 11 keybind) under `parent` (0 = a new root), named `name=`. |

| `AN_SetUiText` | `exec` (in, exec), `widget` (in, int), `then` (out, exec), `success` (out, bool) | `text=` | P | Sets a widget's text to `text=`. |

| `AN_SetUiValue` | `exec` (in, exec), `widget` (in, int), `value` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets a slider, progress bar or choice value. |

| `AN_SetUiChecked` | `exec` (in, exec), `widget` (in, int), `checked` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Sets a toggle. |

| `AN_SetUiVisible` | `exec` (in, exec), `widget` (in, int), `visible` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Shows or hides a widget. |

| `AN_SetUiEnabled` | `exec` (in, exec), `widget` (in, int), `enabled` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Enables or disables a widget. |

| `AN_GetUiValue` | `exec` (in, exec), `widget` (in, int), `then` (out, exec), `value` (out, float), `checked` (out, bool), `selected` (out, int), `success` (out, bool) | — | P | Reads a widget's value, checked state and selected index. |

| `AN_UiWasClicked` | `exec` (in, exec), `widget` (in, int), `then` (out, exec), `clicked` (out, bool) | — | P | True when the widget was clicked in the events the last frame delivered. |

| `AN_UiCommandFired` | `exec` (in, exec), `then` (out, exec), `fired` (out, bool) | `name=` | P | True when command `name=` fired in the events the last frame delivered. |

| `AN_SetUiFocus` | `exec` (in, exec), `widget` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Moves keyboard/gamepad focus to a widget. |

| `AN_OpenUiSettings` | `exec` (in, exec), `then` (out, exec), `layout` (out, int), `success` (out, bool) | — | P | Opens the built-in settings screen (Graphics, Audio, Controls with key rebinding). |
| `AN_SetUiFont` | `exec` (in, exec), `widget` (in, int), `size` (in, float), `bold` (in, bool), `italic` (in, bool), `then` (out, exec), `success` (out, bool) | `family=` (optional) | P | Sets a widget's font: size in design pixels (0 = the theme's), bold, italic, and the font family from the project's font set when `family=` is given (empty keeps the current one). See GAME_UI_TEXT.md. |
| `AN_SetUiTextStyle` | `exec` (in, exec), `widget` (in, int), `align` (in, int), `dir` (in, int), `wrap` (in, bool), `ellipsis` (in, bool), `maxLines` (in, int), `lineSpacing` (in, float, default 1), `letterSpacing` (in, float), `rich` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Sets all nine text style properties at once. `align`: 0 left, 1 centre, 2 right, 3 start, 4 end (start/end follow the text direction); `dir`: 0 auto, 1 LTR, 2 RTL; `maxLines` 0 = unlimited. |
| `AN_SetUiTextColor` | `exec` (in, exec), `widget` (in, int), `r` (in, float), `g` (in, float), `b` (in, float), `a` (in, float, default 1), `then` (out, exec), `success` (out, bool) | — | P | Sets a widget's text colour; each channel is 0..1. |
| `AN_GetUiTextSize` | `widget` (in, int), `found` (out, bool), `width` (out, float), `height` (out, float), `lines` (out, int) | — | P | Pure read: the size of the widget's text as laid out by the last frame; `found` is false for a widget with no text or not yet laid out. |



### Prefab



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_SpawnPrefab` | `exec` (in, exec), `parent` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `yaw` (in, float), `scale` (in, float), `then` (out, exec), `entity` (out, int) | `prefab=` | P | Spawns an instance of `prefab=` at the position and yaw; `entity` is the instance root, 0 on failure. |

| `AN_DestroyPrefab` | `exec` (in, exec), `root` (in, int), `then` (out, exec) | — | P | Destroys an instance and everything under it. |

| `AN_GetPrefabRoot` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `root` (out, int) | — | P | The instance root `entity` belongs to; 0 if it is not part of one. |

| `AN_FindPrefabNode` | `exec` (in, exec), `root` (in, int), `then` (out, exec), `entity` (out, int) | `node=` | P | The entity of node path `node=` (empty = the root) inside an instance. |

| `AN_RevertPrefab` | `exec` (in, exec), `root` (in, int), `then` (out, exec) | — | P | Reverts every override of an instance back to its prefab. |



### Level instance

<!-- [wave3/level-instances begin] -->
Place another level inside this one, load and unload it, find things inside it (`docs/LEVEL_INSTANCES.md`). A level is named by `level=`, a child by its id path in `path=`, because graphs have no string pins. Rows: `GraphLevelInstanceNodeTable.cs`, owner `GraphLevelInstances` (`Aver.LevelInstance/LevelInstances.cs`). Tests: `Aver.Graph.Tests/LevelInstanceNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_SpawnLevelInstance` | `exec` (in, exec), `parent` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `yaw` (in, float), `scale` (in, float), `packed` (in, bool), `then` (out, exec), `entity` (out, int) | `level=` | P | Spawns a runtime-only instance of the level `level=`; `entity` is the instance root, 0 on failure. |
| `AN_DestroyLevelInstance` | `exec` (in, exec), `root` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Destroys an instance and everything under it. |
| `AN_GetLevelInstanceRoot` | `entity` (in, int), `root` (out, int) | — | D | The innermost instance root `entity` belongs to; 0 if it is not part of one. |
| `AN_FindLevelInstanceEntity` | `root` (in, int), `entity` (out, int) | `path=` | D | The entity of the child placement id path `path=` (hex, `b2/c9` through a nested instance; empty = the root). |
| `AN_SetLevelInstanceLoaded` | `exec` (in, exec), `root` (in, int), `loaded` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Loads or unloads an instance by hand; a hand-loaded instance is never evicted by streaming. |
| `AN_IsLevelInstanceLoaded` | `root` (in, int), `loaded` (out, bool) | — | D | True while the instance's contents exist. |
| `AN_ReloadLevelInstance` | `exec` (in, exec), `root` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Rebuilds an instance from its level asset, keeping its overrides. |
| `AN_SetLevelInstancePacked` | `exec` (in, exec), `root` (in, int), `packed` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Switches packed mode; `success` is true when the effective state matches the request. |
| `AN_GetLevelInstanceBounds` | `root` (in, int), `minX` ... `maxZ` (out, float), `success` (out, bool) | — | D | The instance's world box in cm; `success` false when unknown. |
| `AN_GetLevelInstanceCount` | `count` (out, int) | — | D | How many instances exist (nested included). |
| `AN_GetLevelInstanceAt` | `index` (in, int), `root` (out, int) | — | D | The root of the instance at `index`. |

Entries: `LevelInstance.Loaded` / `LevelInstance.Unloaded` through `AN_OnEvent` with the instance root as owner; `AN_EventPayload` index 0 is the entity count. C#-only: `LevelInstanceAbi.LastError`, `AbiVersion`, `AssetId`. Editor-only operations (edit, commit, create, break, bake) have no node.
<!-- [wave3/level-instances end] -->

### Decal



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_SpawnDecal` | `exec` (in, exec), `x` (in, float), `y` (in, float), `z` (in, float), `nx` (in, float), `ny` (in, float), `nz` (in, float), `sx` (in, float), `sy` (in, float), `sz` (in, float), `roll` (in, float), `lifetime` (in, float), `fadeOut` (in, float), `then` (out, exec), `entity` (out, int) | `base=`, `normalmap=`, `orm=` | P | Spawns a pooled gameplay decal on a surface (`nx,ny,nz` is the surface normal); the pool recycles its oldest when full. |

| `AN_ClearDecals` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | — | P | Releases every pooled decal. |

| `AN_SetDecalCapacity` | `exec` (in, exec), `capacity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Sets how many pooled decals may be alive at once; 0 disables spawning. |



### AI



| Node | Pins | Attribute | Path | What it does |

|---|---|---|---|---|

| `AN_CrowdSetAgent` | `exec` (in, exec), `entity` (in, int), `radiusCm` (in, float), `maxSpeedCm` (in, float), `maxAccelCm` (in, float), `priority` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Makes the entity a crowd agent with this radius, speed and acceleration. |

| `AN_CrowdSetMode` | `exec` (in, exec), `entity` (in, int), `mode` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets how the agent moves: 0 follow path agent, 1 seek, 2 arrive, 3 flee, 4 wander, 5 hold. |

| `AN_CrowdSetBackend` | `exec` (in, exec), `backend` (in, int), `maxAgents` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Chooses the CPU (deterministic) or GPU (thousands) solver and the max-agents cap (< 0 keeps it). |

| `AN_GetCrowdVelocity` | `entity` (in, int), `vx` (out, float), `vy` (out, float), `speed` (out, float), `success` (out, bool) | — | D | The agent's avoidance-adjusted velocity. |

| `AN_CrowdSteer` | `entity` (in, int), `dt` (in, float), `turnRate` (in, float), `maxSpeedCm` (in, float), `forward` (out, float), `right` (out, float), `yawDelta` (out, float), `success` (out, bool) | — | D | The crowd velocity as `CharacterMove` input (`forward`, `right`, `yawDelta`). |

| `AN_EmitNoise` | `exec` (in, exec), `x` (in, float), `y` (in, float), `z` (in, float), `loudnessCm` (in, float), `tag` (in, int), `source` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Raises a noise at a position; listeners within its occluded range remember it. |

| `AN_SetHearing` | `exec` (in, exec), `entity` (in, int), `sensitivity` (in, float), `maxRangeCm` (in, float), `memorySec` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Gives the entity an ear: sensitivity, range and memory time. |

| `AN_GetHeard` | `entity` (in, int), `heard` (out, bool), `x` (out, float), `y` (out, float), `z` (out, float), `level` (out, float), `tag` (out, int), `confidence` (out, float), `timeSince` (out, float), `success` (out, bool) | — | D | The listener's strongest remembered noise. `heard` false is ordinary. |

| `AN_FindCover` | `exec` (in, exec), `entity` (in, int), `threatX` (in, float), `threatY` (in, float), `threatZ` (in, float), `maxSeekCm` (in, float), `minThreatDistCm` (in, float), `then` (out, exec), `found` (out, bool), `x` (out, float), `y` (out, float), `coverId` (out, int), `success` (out, bool) | — | P | Finds AND reserves a cover point protecting from the threat; `found` false when nothing protects. |

| `AN_ReleaseCover` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Gives up the entity's cover reservation. |

| `AN_IsCovered` | `entity` (in, int), `threatX` (in, float), `threatY` (in, float), `threatZ` (in, float), `covered` (out, bool) | — | D | True when the entity stands where the threat cannot see it. |

| `AN_SquadJoin` | `exec` (in, exec), `entity` (in, int), `squadId` (in, int), `spacingCm` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Adds the entity to a squad. |

| `AN_SquadSetTarget` | `exec` (in, exec), `squadId` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the squad's target position. |

| `AN_GetSquadSlot` | `entity` (in, int), `role` (out, int), `x` (out, float), `y` (out, float), `z` (out, float), `success` (out, bool) | — | D | The entity's role (0 none, 1 anchor, 2 flank left, 3 flank right, 4 support) and slot position. |




### AI queries

Environment queries (EQS), smart objects and the entity brain, a behaviour tree (`.ocbt`) or a state tree (`.ocst`) (`docs/AI_QUERIES_SMARTOBJECTS_STATETREE.md`). The query, asset, tag list, event and state names are attributes; tag lists are comma lists. A failure answers `false` / `0` and never throws. Rows: `GraphAiQueryNodeTable.cs`, owners `Aver.Synapse.QueryGraph` (EQS and smart objects) and `BrainGraph` (`Aver.Framework/Ai/Brain.cs`). Tests: `Aver.Graph.Tests/AiQueryNodeTests.cs`. Positions are centimetres, `yaw` degrees.

Completion needs no node: a graph hears it as an event by name through `AN_OnEvent`. `EqsFinished` and `EqsFailed` fire on the querier; `SmartObjectClaimed`, `SmartObjectUsed` and `SmartObjectReleased` on the user; `SmartObjectSlot*` on the object; a slot's custom `USE event` by its own name. Custom tasks, conditions and EQS tests (`[AverAiAction]`, `[AverAiCondition]`, `[AverEqsTest]`), the EQS budget and the `Span` form of a user's claims are C#-only; the graph equivalent of a custom test is data in the asset.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_EqsRun` | `exec` (in, exec), `querier` (in, int), `target` (in, int), `px` (in, float), `py` (in, float), `pz` (in, float), `hasPoint` (in, bool), `then` (out, exec), `found` (out, bool), `count` (out, int), `x` (out, float), `y` (out, float), `z` (out, float), `entity` (out, int), `score` (out, float) | `query=`, `usertags=` (optional) | P | Runs the `.oceqs` query to completion now and reports the best item. `count` is how many items passed; `found` is false when none did. `px`..`pz` is the context point when `hasPoint` is true. |
| `AN_EqsStart` | `exec` (in, exec), `querier` (in, int), `target` (in, int), `px` (in, float), `py` (in, float), `pz` (in, float), `hasPoint` (in, bool), `priority` (in, int), `then` (out, exec), `handle` (out, int) | `query=`, `usertags=` (optional) | P | Queues a time-sliced run; `handle` is 0 when it was refused. Read it with Status and Get Item, hear the end as `EqsFinished`, and release it. |
| `AN_EqsStatus` | `handle` (in, int), `status` (out, int), `count` (out, int) | — | D | 0 unknown, 1 queued, 2 running, 3 finished, 4 failed, 5 cancelled; `count` is the items so far. |
| `AN_EqsGetItem` | `handle` (in, int), `index` (in, int), `success` (out, bool), `x` (out, float), `y` (out, float), `z` (out, float), `entity` (out, int), `score` (out, float), `obj` (out, int), `slot` (out, int) | — | D | The `index`-th item, best first (score in -1..1). `obj` and `slot` name a smart-object slot when the query generated slots, otherwise 0 and -1. |
| `AN_EqsCancel` | `exec` (in, exec), `handle` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Stops a queued or running query. |
| `AN_EqsRelease` | `exec` (in, exec), `handle` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Frees the run's items. Release every handle you start. |
| `AN_SoAttach` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `asset=` | P | Gives the entity the smart object definition `asset=` (`.ocso`) and its slots. |
| `AN_SoFind` | `user` (in, int), `x` (in, float), `y` (in, float), `z` (in, float), `radius` (in, float), `matchAll` (in, bool), `found` (out, bool), `obj` (out, int), `slot` (out, int), `ex` (out, float), `ey` (out, float), `ez` (out, float), `count` (out, int) | `slottags=` (optional), `usertags=` (optional) | D | The nearest free slot within `radius` (default 1500) whose tags match `slottags=` (any one, or every one with `matchAll`) and whose requirements `usertags=` meets, with its entry point; `count` is how many matched. |
| `AN_SoClaim` | `exec` (in, exec), `user` (in, int), `obj` (in, int), `slot` (in, int), `ttl` (in, float), `then` (out, exec), `claim` (out, int), `refusal` (out, int) | `usertags=` (optional) | P | Reserves the slot for the user. `claim` is 0 when refused, and `refusal` says why: 1 no such slot, 2 disabled, 3 not free, 4 tags, 5 user limit, 6 no user. `ttl` below 0 uses the slot's own claim time, 0 never expires, otherwise the claim frees itself after that many seconds. |
| `AN_SoUse` | `exec` (in, exec), `claim` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Starts using the claimed slot (claimed becomes occupied) and runs the slot's behaviour, if it has one. |
| `AN_SoRelease` | `exec` (in, exec), `claim` (in, int), `reason` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Ends the claim. `reason`: 0 released, 1 completed, 2 failed. |
| `AN_SoGetClaim` | `user` (in, int), `index` (in, int), `has` (out, bool), `claim` (out, int), `obj` (out, int), `slot` (out, int), `state` (out, int), `x` (out, float), `y` (out, float), `z` (out, float), `yaw` (out, float), `ex` (out, float), `ey` (out, float), `ez` (out, float) | — | D | The user's `index`-th claim (up to 4): its slot, state (1 claimed, 2 occupied), slot pose and entry point. |
| `AN_SoSlotInfo` | `entity` (in, int), `slot` (in, int), `success` (out, bool), `state` (out, int), `user` (out, int), `x` (out, float), `y` (out, float), `z` (out, float), `yaw` (out, float) | — | D | One slot of a smart object: state (0 free, 1 claimed, 2 occupied, 3 cooldown, 4 disabled), who holds it, and its world pose. |
| `AN_SoSlotCount` | `entity` (in, int), `count` (out, int) | — | D | How many slots the entity has; -1 when it is not a smart object. |
| `AN_SoSetSlotEnabled` | `exec` (in, exec), `entity` (in, int), `slot` (in, int), `enabled` (in, bool), `then` (out, exec), `success` (out, bool) | — | P | Turns a slot on or off. A disabled slot refuses claims. |
| `AN_BrainSet` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `asset=` | P | Gives the entity the brain `asset=` (`.ocbt` or `.ocst`), attaching the AI component if it has none. |
| `AN_BrainSendEvent` | `exec` (in, exec), `entity` (in, int), `payloadI` (in, int), `payloadF` (in, float), `then` (out, exec), `success` (out, bool) | `event=` | P | Sends a state tree an event its transitions on `event:Name` see at the next update. False for a behaviour tree. |
| `AN_BrainIsInState` | `entity` (in, int), `inState` (out, bool) | `state=` | D | True while the dotted state path (`Combat.Chase`, or just `Combat`) is in the active chain. |
| `AN_BrainStatus` | `entity` (in, int), `status` (out, int), `kind` (out, int) | — | D | `status`: 0 idle, 1 running, 2 succeeded, 3 failed. `kind`: 0 none, 1 behaviour tree, 2 state tree. |
| `AN_BrainRestart` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Exits every state and starts again at the next update. |
| `AN_BrainStop` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Exits every state and stays stopped until Restart. |

### Enhanced Input

Triggers and modifiers on a named action, and its live state. Actions are addressed by **name** (`action=`), so no handle pin is needed. Specs are the `.ocinput` text (`hold:0.5`, `tap`, `combo:0.4:A,B`, `deadzone:0.2`); a spec the parser rejects makes the node's `success` false. Rows: `GraphGameSystemNodeTable.cs`, owner `InputGraph` (`Aver.Framework/InputGraph.cs`). Tests: `Aver.Graph.Tests/EnhancedInputTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `InputActionState` | `state` (out, int), `started` (out, bool), `ongoing` (out, bool), `triggered` (out, bool), `completed` (out, bool), `canceled` (out, bool), `elapsed` (out, float), `triggeredTime` (out, float), `x` (out, float), `y` (out, float), `z` (out, float), `success` (out, bool) | `action=` | D | Everything the last update knows about the action `action=`: `state` (0 none, 1 ongoing, 2 triggered), this frame's event flags, seconds held, seconds since it triggered, and its value. `success` is false for an unknown action. |
| `AddInputTrigger` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `action=`, `trigger=` | P | Appends a trigger such as `hold:0.5` or `tap` to the action `action=`. Not idempotent: run Clear Input Config first in OnStart. |
| `AddInputModifier` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `action=`, `modifier=` | P | Appends a modifier such as `deadzone:0.2` to the action `action=`. Not idempotent. |
| `ClearInputConfig` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `action=` | P | Drops the action's own triggers and modifiers. |
| `OnInputAction` | `exec` (out, exec) | `action=`, `event=` | X | Entry node: runs when the action `action=` raises `event=` (Started, Ongoing, Triggered, Completed or Canceled). The editor keeps `ENTRY e Input.<action>.<event>` in step with the two attributes. `AN_EventPayload` index 0 to 3 reads x, y, seconds held, seconds since triggered. |

`OnInputAction` is the same runtime as `AN_OnEvent`: `GraphTimerEvents.BindGraph` accepts either node type. The editor writes and keeps `ENTRY <node> Input.<action>.<event>` in step with the two attributes (it stays empty until both are set); a hand-written equivalent is `NODE e AN_OnEvent` with `ENTRY e Input.Jump.Triggered`.

### Gameplay abilities

Tags, attributes, effects and abilities (GAS, `docs/plans/wave2/GAS.md`). Rows: `GraphGasNodeTable.cs`, owner `GasGraph` (`Aver.Framework/Gas/GasGraph.cs`). Tests: `Aver.Graph.Tests/GasNodeTests.cs`.

**One rule for every row.** A name can be given two ways: an **id pin** (from an `AN_Gas*Id` node) or the **attribute**. A non-zero id pin wins; with both empty the call fails (`false`, `0`, handle `0`) and logs once. `entity` pins are the owner; `0` means "not given" and fails.

**Entries need no node.** Use `AN_OnEvent` with `ENTRY` set to the bus event: `GAS.AbilityActivated.<Ability>`, `GAS.AttributeChanged.<Attribute>` (`AN_EventPayload` index 0 = id, 1 = new value, 2 = old), `GAS.TagChanged.<Tag>`, `GAS.GameplayEvent.<Tag>`. Ability bodies are graphs on those events; `GameplayAbility` subclasses are C#-only. An effect with several SetByCaller values is C#-only (the node carries one).

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_GasTagId` | `tagId` (out, int) | `tag=` | D | The id of the tag `tag=`, registering it if new. Feed it to the `tagId` pins. |
| `AN_GasAttributeId` | `attrId` (out, int) | `attribute=` | D | The id of the attribute `attribute=` (e.g. `Health`). Feed it to the attribute nodes. |
| `AN_GasEffectId` | `effectId` (out, int) | `effect=` | D | The id of the `.ocgas` effect `effect=`; 0 if no effect has that name. |
| `AN_GasAbilityId` | `abilityId` (out, int) | `ability=` | D | The id of the `.ocgas` ability `ability=`; 0 if no ability has that name. |
| `AN_GasHasTag` | `entity` (in, int), `tagId` (in, int), `has` (out, bool) | `tag=` (optional) | D | True if the entity owns the tag or a child of it (`State.Stunned` matches `State.Stunned.Hard`). A non-zero `tagId` wins over `tag=`; with neither the call fails. |
| `AN_GasHasTags` | `entity` (in, int), `all` (in, bool), `has` (out, bool) | `tags=` | D | True if the entity owns all (`all` true) or any of the comma-separated `tags=`. |
| `AN_GasTagCount` | `entity` (in, int), `tagId` (in, int), `count` (out, int) | `tag=` (optional) | D | How many copies of the tag the entity owns (several effects can grant one). A non-zero `tagId` wins over `tag=`; with neither the call fails. |
| `AN_GasAddTag` | `exec` (in, exec), `entity` (in, int), `tagId` (in, int), `count` (in, int), `then` (out, exec), `success` (out, bool) | `tag=` (optional) | P | Adds the tag `count` times as a loose tag, creating the entity's ability system if it has none. A non-zero `tagId` wins over `tag=`; with neither the call fails. |
| `AN_GasRemoveTag` | `exec` (in, exec), `entity` (in, int), `tagId` (in, int), `count` (in, int), `then` (out, exec), `success` (out, bool) | `tag=` (optional) | P | Removes `count` loose copies of the tag; `success` is false if the count did not change. A non-zero `tagId` wins over `tag=`; with neither the call fails. |
| `AN_GasGetAttribute` | `entity` (in, int), `attrId` (in, int), `value` (out, float), `base` (out, float), `success` (out, bool) | `attribute=` (optional) | D | `value` is the current (modified) value, `base` the stored one. A non-zero `attrId` wins over `attribute=`. |
| `AN_GasSetAttribute` | `exec` (in, exec), `entity` (in, int), `attrId` (in, int), `value` (in, float), `then` (out, exec), `success` (out, bool) | `attribute=` (optional) | P | Sets the attribute's base value. Active effects keep their modifiers on top. |
| `AN_GasAddAttributeSet` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `set=` | P | Gives the entity the `.ocgas` attribute set `set=` with its default values, creating its ability system if needed. |
| `AN_GasApplyEffect` | `exec` (in, exec), `target` (in, int), `source` (in, int), `effectId` (in, int), `callerValue` (in, float), `then` (out, exec), `handle` (out, int), `applied` (out, bool) | `effect=` (optional), `callertag=` (optional) | P | Applies an effect to `target`; `source` 0 means the target itself. `callerValue` fills the effect's SetByCaller magnitude named by `callertag=`. `handle` is 0 and `applied` false when it was refused. A non-zero `effectId` wins over `effect=`. |
| `AN_GasRemoveEffect` | `exec` (in, exec), `entity` (in, int), `handle` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Removes the active effect with this handle. |
| `AN_GasRemoveEffectsByTag` | `exec` (in, exec), `entity` (in, int), `tagId` (in, int), `then` (out, exec), `count` (out, int) | `tag=` (optional) | P | Removes the active effects that carry the tag; `count` is how many went. A non-zero `tagId` wins over `tag=`; with neither the call fails. |
| `AN_GasRemoveEffectsByDef` | `exec` (in, exec), `entity` (in, int), `effectId` (in, int), `then` (out, exec), `count` (out, int) | `effect=` (optional) | P | Removes every active instance of one effect definition. A non-zero `effectId` wins over `effect=`. |
| `AN_GasEffectInfo` | `entity` (in, int), `effectId` (in, int), `active` (out, bool), `stacks` (out, int), `remaining` (out, float), `handle` (out, int) | `effect=` (optional) | D | Whether the effect is active on the entity, with its stack count, seconds left (1e30 = infinite) and handle. |
| `AN_GasGrantAbility` | `exec` (in, exec), `entity` (in, int), `abilityId` (in, int), `level` (in, int), `then` (out, exec), `handle` (out, int) | `ability=` (optional) | P | Gives the entity the ability at `level`, creating its ability system if needed. `handle` 0 means it failed. |
| `AN_GasRevokeAbility` | `exec` (in, exec), `entity` (in, int), `abilityId` (in, int), `then` (out, exec), `success` (out, bool) | `ability=` (optional) | P | Takes the ability away, ending it first if it is running. |
| `AN_GasTryActivate` | `exec` (in, exec), `entity` (in, int), `abilityId` (in, int), `target` (in, int), `then` (out, exec), `success` (out, bool), `reason` (out, int) | `ability=` (optional) | P | Checks requirements, cost and cooldown, then starts the ability. `reason` is the failure code: 0 none, 1 no ability system, 2 not granted, 3 already active, 4 missing required tag, 5 blocked by tag, 6 blocked by ability, 7 on cooldown, 8 cannot afford, 9 refused by the behaviour, 10 no authority. |
| `AN_GasTryActivateByTag` | `exec` (in, exec), `entity` (in, int), `tagId` (in, int), `target` (in, int), `then` (out, exec), `success` (out, bool), `abilityId` (out, int), `reason` (out, int) | `tag=` (optional) | P | Starts a granted ability whose tags match; `abilityId` says which. `reason` as in Try Activate Ability. |
| `AN_GasCanActivate` | `entity` (in, int), `abilityId` (in, int), `can` (out, bool), `reason` (out, int) | `ability=` (optional) | D | The same checks as Try Activate Ability, without starting anything. |
| `AN_GasCommitAbility` | `exec` (in, exec), `entity` (in, int), `abilityId` (in, int), `then` (out, exec), `success` (out, bool) | `ability=` (optional) | P | Pays the running ability's cost and starts its cooldown together. False, with nothing paid, if it cannot be afforded. |
| `AN_GasEndAbility` | `exec` (in, exec), `entity` (in, int), `abilityId` (in, int), `cancelled` (in, bool), `then` (out, exec), `success` (out, bool) | `ability=` (optional) | P | Ends the running ability; `cancelled` tells listeners it did not finish. |
| `AN_GasCancelByTag` | `exec` (in, exec), `entity` (in, int), `tagId` (in, int), `then` (out, exec), `count` (out, int) | `tag=` (optional) | P | Cancels the running abilities that carry the tag; `count` is how many. |
| `AN_GasIsAbilityActive` | `entity` (in, int), `abilityId` (in, int), `active` (out, bool) | `ability=` (optional) | D | True while the ability is running on the entity. |
| `AN_GasCooldown` | `entity` (in, int), `abilityId` (in, int), `remaining` (out, float), `onCooldown` (out, bool) | `ability=` (optional) | D | Seconds of cooldown left (0 when ready). |
| `AN_GasSendEvent` | `exec` (in, exec), `entity` (in, int), `instigator` (in, int), `tagId` (in, int), `magnitude` (in, float), `then` (out, exec), `triggered` (out, int) | `tag=` (optional) | P | Sends a gameplay event to the entity; `triggered` is how many abilities it started. Listen with `AN_OnEvent` and `ENTRY e GAS.GameplayEvent.<Tag>`. |

### Data tables

Read rows and cells of a `.csv` or `.json` data table (`docs/DATA_TABLES.md`). `table=` is the table path, `column=` a column name, `row=` a row name; none can be a pin because graphs have no string pins, so dynamic lookup goes by int column (`AN_FindDataTableRowByInt`) and the `row` pin carries the index from there. A failed lookup is `found`/`success` false with zeroed outputs, never an error. Rows: `GraphGameSystemNodeTable.cs`, owner `DataContentGraph`. Tests: `Aver.Graph.Tests/DataLocNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_FindDataTableRow` | `row` (out, int), `found` (out, bool) | `table=`, `row=` | D | The index of the row named `row=` in the table `table=`. |
| `AN_FindDataTableRowByInt` | `value` (in, int), `row` (out, int), `found` (out, bool) | `table=`, `column=` | D | The first row of `table=` whose `column=` holds `value`. |
| `AN_DataTableRowCount` | `count` (out, int) | `table=` | D | How many rows `table=` has. |
| `AN_GetDataTableInt` | `row` (in, int), `value` (out, int), `success` (out, bool) | `table=`, `column=` | D | The cell at `row` in `column=` of `table=`, as an int. |
| `AN_GetDataTableFloat` | `row` (in, int), `value` (out, float), `success` (out, bool) | `table=`, `column=` | D | The cell as a float. |
| `AN_GetDataTableBool` | `row` (in, int), `value` (out, bool), `success` (out, bool) | `table=`, `column=` | D | The cell as a bool. |
| `AN_GetDataTableVec3` | `row` (in, int), `x` (out, float), `y` (out, float), `z` (out, float), `success` (out, bool) | `table=`, `column=` | D | The cell as a vector. |
| `AN_SetUiTextFromTable` | `exec` (in, exec), `widget` (in, int), `row` (in, int), `then` (out, exec), `success` (out, bool) | `table=`, `column=` | P | Sets a widget's text to the cell; a localization reference in the cell still resolves. |

### Localization

Culture switching (`docs/LOCALIZATION.md`). Text itself needs no node: write a reference into `AN_SetUiText` as `text=@UI:hud.ammo=Ammo` (table `UI`, key `hud.ammo`, default text `Ammo`) and the UI resolves it. The NODE-line parser keeps the `=` and `@` after the first `=`, does **not** strip quotes, and splits on whitespace, so a reference cannot contain a space. React to a language change with `AN_OnEvent` and `ENTRY e loc.culture_changed`. Owner `LocGraph`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_SetCulture` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `culture=` | P | Switches the game language to `culture=` (e.g. `fr` or `pt-BR`); false if no such culture is available. |
| `AN_IsCulture` | `matches` (out, bool) | `culture=` | D | True if the active culture equals `culture=` or starts with it and a dash (`fr` matches `fr-CA`). |
| `AN_CultureCount` | `count` (out, int) | — | D | How many cultures the project provides. |
| `AN_CurrentCultureIndex` | `index` (out, int) | — | D | The active culture's position in the list, -1 if unknown. |
| `AN_SetCultureByIndex` | `exec` (in, exec), `index` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Switches to the culture at this position in the list. |
| `AN_SetUiItemsToCultures` | `exec` (in, exec), `widget` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Fills a Choice or List widget with the cultures' native names and selects the current one. |

### Montages and root motion

Play authored montages in slots and sections, and use or read the root motion animations carry (`docs/ANIM_MONTAGES.md`). The entity needs a skeleton. Montage events and notifies need no node: they arrive as graph events by name (`AN_OnEvent` or `CustomEvent`). Layered and additive weights are state machine parameters, driven by `SetAnimParamFloat`. Rows: `GraphGameSystemNodeTable.cs`, owners `MontageGraphInterop` and `RootMotionGraphInterop`. Tests: `Aver.Graph.Tests/MontageNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `PlayMontage` | `exec` (in, exec), `entity` (in, int), `rate` (in, float), `blendIn` (in, float), `then` (out, exec), `success` (out, bool) | `montage=`, `section=` (optional), `slot=` (optional) | P | Plays the `.ocmontage` `montage=` on the entity, from `section=` (default the first) in `slot=` (default the montage's own). Negative `blendIn` uses the montage's. The entity needs a skeleton. |
| `StopMontage` | `exec` (in, exec), `entity` (in, int), `blendOut` (in, float), `then` (out, exec), `success` (out, bool) | `montage=` (optional) | P | Blends `montage=` out; empty stops every montage on the entity. 0 cuts, negative uses the montage's own blend. |
| `StopMontageSlot` | `exec` (in, exec), `entity` (in, int), `blendOut` (in, float), `then` (out, exec), `success` (out, bool) | `slot=` | P | Blends out every montage in the slot `slot=`. |
| `JumpToMontageSection` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `montage=`, `section=` | P | Jumps a playing montage to `section=` at once, keeping its blend weight. |
| `SetMontageNextSection` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `montage=`, `from=`, `to=` (optional) | P | Changes where section `from=` goes when it ends. An empty `to=` restores the authored link. |
| `SetMontageRate` | `exec` (in, exec), `entity` (in, int), `rate` (in, float), `then` (out, exec), `success` (out, bool) | `montage=` | P | Sets a playing montage's speed; 0 pauses it. |
| `IsMontagePlaying` | `entity` (in, int), `result` (out, bool) | `montage=` (optional) | D | True while `montage=` (empty = any) plays or blends out. |
| `GetMontageState` | `entity` (in, int), `found` (out, bool), `position` (out, float), `length` (out, float), `weight` (out, float) | `montage=` (optional) | D | Position and length in montage seconds and the blend weight of `montage=` (empty = the newest). `found` is false when nothing plays. |
| `IsInMontageSection` | `entity` (in, int), `result` (out, bool) | `montage=` (optional), `section=` | D | True while `montage=` (empty = the newest) is inside `section=`. |
| `SetRootMotion` | `exec` (in, exec), `entity` (in, int), `mode` (in, int), `axes` (in, int), `scale` (in, float), `then` (out, exec), `success` (out, bool) | — | P | How the entity uses the root motion its animations carry. `mode`: 0 none, 1 ignore (the pose stays in place), 2 apply (the entity moves), 3 accumulate (read it with Consume Root Motion). `axes` is a bit mask (1 X, 2 Y, 4 Z, 8 yaw; default 11 is X, Y and yaw). `scale` multiplies the travel. |
| `ConsumeRootMotion` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool), `x` (out, float), `y` (out, float), `z` (out, float), `yaw` (out, float), `seconds` (out, float) | — | P | Reads the root motion accumulated since the last read (centimetres in the entity's own frame, `yaw` in degrees, over `seconds`) and clears it. It collects in mode 3. |

### Assets

Import a model file into the project's Content folder from a graph (`docs/FBX_IMPORT.md`). Editor hosts only: a packaged game has no importer, so `IsModelImportAvailable` reads false and `ImportModel` reports `success` false. Rows: `GraphGameSystemNodeTable.cs`, owner `ModelImportGraph`. Tests: `Aver.Graph.Tests/ModelImportNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `ImportModel` | `exec` (in, exec), `overwrite` (in, bool), `scale` (in, float, default 100), `then` (out, exec), `success` (out, bool), `meshes` (out, int), `skeletons` (out, int), `clips` (out, int) | `source=`, `dest=` | X | Imports the `.fbx`/`.gltf`/`.glb` `source=` (absolute, or relative to the project folder) into the Content-relative folder `dest=`. `overwrite` sends the files it replaces to the recycle bin; `scale` is centimetres per metre (FBX). Runs synchronously. |
| `IsModelImportAvailable` | `available` (out, bool) | — | D | True when the host can import (the editor). |

After adding the rows: re-derive the node counts in this page's header with the counting method it states, and add `Assets` to the contents list.

### Landscape and water

Read the painted landscape and the water over it (`docs/LANDSCAPE_LAYERS_WATER.md`). All are reads, so they run under either compiler. A layer or surface is named by an attribute; a layer id pin of 0 or more beats `layer=`. Off the landscape, over no water or with no provider, a node answers its defaults (found false, weight 0, surface 0) and never errors. Painting, carving and spline editing are editor commands with no node. Entering water is not an event: poll `AN_EntityWater` from a timer, or use a physics sensor. Rows: `GraphTerrainNodeTable.cs`, owner `TerrainGraph` (`Aver.Framework/TerrainGraph.cs`). Tests: `Aver.Graph.Tests/TerrainNodeTests.cs`. Positions are centimetres.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `AN_LandscapeSample` | `x` (in, float), `y` (in, float), `found` (out, bool), `z` (out, float), `nx` (out, float), `ny` (out, float), `nz` (out, float), `layer` (out, int) | — | D | Ground height, surface normal and dominant paint layer at a world XY. `found` is false off the section. |
| `AN_LandscapeLayerId` | `layerId` (out, int) | `layer=` | D | The index of the named paint layer; -1 when unknown. |
| `AN_LandscapeLayerWeight` | `x` (in, float), `y` (in, float), `layerId` (in, int), `weight` (out, float) | `layer=` (optional) | D | How much of a layer is painted at a world XY, 0 to 1. `layerId` (default -1) wins over `layer=`; with neither the weight is 0. |
| `AN_LandscapeSurface` | `x` (in, float), `y` (in, float), `surfaceId` (out, int), `friction` (out, float), `restitution` (out, float), `layer` (out, int) | — | D | The dominant surface id (0 none) and the blended friction and restitution at a world XY. |
| `AN_SurfaceId` | `surfaceId` (out, int) | `surface=` | D | The id of a named surface, to compare with `AN_LandscapeSurface`; 0 for none. |
| `AN_WaterSample` | `x` (in, float), `y` (in, float), `z` (in, float), `over` (out, bool), `submerged` (out, bool), `surfaceZ` (out, float), `depth` (out, float), `flowX` (out, float), `flowY` (out, float), `flowZ` (out, float), `kind` (out, int) | — | D | The water at a world position. `over` is true when water covers the XY; `submerged` when the point is under the surface. `kind`: 0 none, 1 ocean, 2 lake, 3 river. `flow` is the current in cm/s. |
| `AN_EntityWater` | `entity` (in, int), `heightCm` (in, float), `over` (out, bool), `submersion` (out, float), `flowX` (out, float), `flowY` (out, float), `flowZ` (out, float), `kind` (out, int) | — | D | How much of a `heightCm` tall body (default 180) at the entity is under water, 0 to 1, and the current there. False for a dead entity or one not over water. |

### IK and rig inputs

Drive the IK rig an entity wears and the named inputs of its `.ocrig` (`docs/ANIM_IK_RETARGET.md`). The entity needs a skeleton. Goals and rig inputs are runtime data: they are not saved, and a destroyed target releases its goal. Rig, goal, foot and input names are attributes. A foot is named by its leg chain. The older `SetControlRig` node is unchanged. Rows: `GraphIkNodeTable.cs`, owner `IkGraphInterop` (`Aver.Framework/Ik.cs`). Tests: `Aver.Graph.Tests/IkNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetIkRig` | `exec` (in, exec), `entity` (in, int), `weight` (in, float), `then` (out, exec), `success` (out, bool) | `rig=` | P | Puts the `.ocikrig` `rig=` on the entity at `weight` (default 1). |
| `SetIkGoal` | `exec` (in, exec), `entity` (in, int), `x`, `y`, `z` (in, float), `useRotation` (in, bool), `pitch`, `yaw`, `roll` (in, float), `space` (in, int), `then` (out, exec), `success` (out, bool) | `goal=` | P | Places the goal `goal=` the rig declares: position in centimetres, plus a rotation in degrees when `useRotation` is set. `space` 0 is world, 1 is the entity's model space. The first call engages the goal over 0.15 s. |
| `BindIkGoal` | `exec` (in, exec), `entity` (in, int), `target` (in, int), `ox`, `oy`, `oz` (in, float), `then` (out, exec), `success` (out, bool) | `goal=`, `socket=` (optional) | P | Makes the goal follow the entity `target`, or its socket `socket=`, plus the offset `ox`, `oy`, `oz` in that frame. |
| `SetIkGoalWeight` | `exec` (in, exec), `entity` (in, int), `weight` (in, float), `blendSeconds` (in, float), `then` (out, exec), `success` (out, bool) | `goal=` | P | Blends the goal's strength to `weight` over `blendSeconds` (default 0.15; 0 cuts). |
| `ClearIkGoal` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `goal=` (optional) | P | Releases the goal (its weight blends to 0); empty releases every goal on the entity. |
| `GetIkGoalState` | `entity` (in, int), `found` (out, bool), `alpha` (out, float), `errorCm` (out, float), `reached` (out, bool) | `goal=` | D | The goal's blend weight, how far the chain's tip was from it after the last solve in centimetres, and whether the solve reached it. `found` is false without an IK rig or for an undeclared goal. |
| `SetFootIkWeight` | `exec` (in, exec), `entity` (in, int), `weight` (in, float), `blendSeconds` (in, float), `then` (out, exec), `success` (out, bool) | `foot=` (optional) | P | Blends foot placement to `weight` over `blendSeconds` (default 0.2); empty sets every foot. |
| `GetFootIkState` | `entity` (in, int), `found` (out, bool), `offsetCm` (out, float), `plant` (out, float), `nx`, `ny`, `nz` (out, float) | `foot=` (optional) | D | A foot's vertical offset in centimetres, its plant weight (1 planted) and the ground normal under it; empty reads the pelvis (how far it dropped). |
| `SetRigInputFloat` | `exec` (in, exec), `entity` (in, int), `value` (in, float), `then` (out, exec), `success` (out, bool) | `input=` | P | Sets a float input of the entity's `.ocrig`. |
| `SetRigInputVec3` | `exec` (in, exec), `entity` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | `input=` | P | Sets a vector input: a goal, a pole or a look-at point. The rig converts world-space inputs. |
| `ClearRigInput` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `input=` (optional) | P | Forgets an input so the rig's declared default applies again; empty clears them all. |

### Motion matching

Pick the animation frame that best fits where the entity is going (`docs/ANIM_MOTION_MATCHING.md`). A motion-matching state is a state in the `.ocasm` and needs no node; these nodes drive a matcher directly and feed it the trajectory. The entity needs a skeleton. Rows: `GraphMotionMatchNodeTable.cs`, owner `MotionMatchGraphInterop` (`Aver.Framework/MotionMatching.cs`). Tests: `Aver.Graph.Tests/MotionMatchNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetMotionMatching` | `exec` (in, exec), `entity` (in, int), `facing` (in, int), `drive` (in, int), `playRate` (in, float), `then` (out, exec), `success` (out, bool) | `database=` (optional) | P | Runs motion matching on the entity against the `.ocmotion` `database=`; empty leaves the choice to a motion-matching state of its state machine. `facing` 0 follows the velocity, 1 follows the yaw given to Set Motion Input. `drive` 0 lets the simulation move the entity, 1 lets the animation's root motion do it. `playRate` defaults to 1. |
| `SetMotionInput` | `exec` (in, exec), `entity` (in, int), `velocityX` (in, float), `velocityY` (in, float), `facingYaw` (in, float), `then` (out, exec), `success` (out, bool) | — | P | The world-space planar velocity the character is trying to reach, in cm/s, and, in yaw facing, the heading in degrees. Call it every tick. False without a matcher. |
| `SetMotionTags` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `required=` (optional), `excluded=` (optional) | P | Restricts the search to frames with the `required=` tags and without the `excluded=` tags (comma-separated names declared in the `.ocmotion`, up to four each; empty is none). False when over four. |
| `RequestMotionSearch` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Asks for a search on the next tick instead of waiting out the interval. |
| `GetMotionState` | `entity` (in, int), `found` (out, bool), `clipTime` (out, float), `cost` (out, float), `switches` (out, int) | — | D | The time in the clip the matcher plays, how well that frame fits the wanted trajectory (lower is better) and how many times it has switched. `found` is false without a matcher. |
| `IsMotionClip` | `entity` (in, int), `result` (out, bool) | `clip=` | D | True while the matcher plays the clip `clip=` (the path as written in the `.ocmotion`). |

### Sequencer

Play, scrub and bind the level's sequences and `.ocseq` files (`docs/SEQUENCER.md`). A sequence is named by an attribute and has no entity of its own. Lifecycle and event-track events need no node: they arrive as `AN_OnEvent` entries (`<Sequence>_Started`, `<Sequence>_Finished` and every authored event name) or `CustomEvent`. Property, visibility and animation tracks are authored, not scripted. Rows: `GraphSequenceNodeTable.cs`, owner `SequenceGraphInterop` (`Aver.Framework/LevelSequence.cs`). Tests: `Aver.Graph.Tests/SequenceNodeTests.cs`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `PlaySequence` | `exec` (in, exec), `start` (in, float), `rate` (in, float), `then` (out, exec), `success` (out, bool) | `sequence=` | P | Starts or resumes `sequence=`. A negative `start` (default -1) continues from where it is, or from 0 if it was stopped or finished; `rate` 0 (default) keeps the current rate. False for a sequence the level does not have. |
| `PauseSequence` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `sequence=` | P | Pauses `sequence=` where it is; what it wrote stays. |
| `StopSequence` | `exec` (in, exec), `then` (out, exec), `success` (out, bool) | `sequence=` (optional) | P | Stops `sequence=` (empty = every sequence): back to time 0, everything it wrote put back as authored, its sounds stopped. |
| `SetSequenceTime` | `exec` (in, exec), `seconds` (in, float), `then` (out, exec), `success` (out, bool) | `sequence=` | P | Jumps `sequence=` to `seconds`. Sampled tracks show that time exactly; events and sounds do not fire for the skipped span. |
| `SetSequenceRate` | `exec` (in, exec), `rate` (in, float), `then` (out, exec), `success` (out, bool) | `sequence=` | P | Sets the playback speed multiplier of `sequence=`, 0 or more; 0 holds the clock. |
| `BindSequenceActor` | `exec` (in, exec), `entity` (in, int), `then` (out, exec), `success` (out, bool) | `sequence=`, `binding=` | P | Makes `entity` the actor that the tracks bound by the name `binding=` drive. Entity 0 clears the override. False when the sequence has no track bound by that name. |
| `IsSequencePlaying` | `result` (out, bool) | `sequence=` (optional) | D | True while `sequence=` (empty = any sequence) plays. |
| `GetSequenceState` | `found` (out, bool), `time` (out, float), `length` (out, float), `rate` (out, float) | `sequence=` | D | The playhead and length in seconds and the playback rate of `sequence=`. `found` is false for a sequence the level lacks. |

### Cloth and destruction

Damage, cut, repair and release soft bodies and set the wind (`docs/SOFTBODY.md`). Every call is deferred to the system's next tick and applied in arrival order. Rows: `GraphSoftBodyNodeTable.cs`, owner `SoftBodyGraphInterop` (`Aver.Framework/SoftBodies.cs`); palette group Physics. Events (`SoftBodyTorn`, `SoftBodyDetached`, `SoftBodyDust`, `SoftBodyImpact`, `SoftBodyRepaired`) arrive through `OnEvent` and need no node.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SoftBodyDamage` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `radius` (in, float, 30), `energy` (in, float, 100), `dirX`/`dirY`/`dirZ` (in, float), `then` (out), `success` (out, bool) | — | P | Deposits `energy` joules within `radius` cm of the point; entity 0 hits whichever destructible is near. |
| `SoftBodyExplode` | `exec` (in), `x`/`y`/`z` (in, float), `radius` (in, float, 300), `impulse` (in, float, 600), `energy` (in, float, 500), `then` (out), `touched` (out, int), `success` (out, bool) | — | P | Damage and a radial impulse on every soft body in the sphere. |
| `SoftBodyTear` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `radius` (in, float, 20), `then` (out), `success` (out, bool) | — | P | Cuts the beams within `radius` of the point. |
| `SoftBodyRepair` | `exec` (in), `entity` (in, int), `then` (out), `success` (out, bool) | — | P | Undoes dents and tears. |
| `ReleaseSoftBody` | `exec` (in), `entity` (in, int), `then` (out), `success` (out, bool) | — | P | Drops all pins of a cloth. |
| `SetSoftBodyWind` | `exec` (in), `vx`/`vy`/`vz` (in, float), `gust` (in, float, 0), `gustHz` (in, float, 0.3), `turbulence` (in, float, 0), `then` (out), `success` (out, bool) | — | P | The global cloth wind in cm/s, independent of the sky's cloud drift. |
| `SetSoftBodyPreset` | `exec` (in), `entity` (in, int), `preset` (in, int, 0), `then` (out), `success` (out, bool) | — | P | Changes the preset of the entity's Cloth or Destructible (0 cloth_light, 1 cloth_heavy, 2 metal, 3 wood, 4 glass). |
| `GetSoftBodyState` | `entity` (in, int), `found` (out, bool), `damage` (out, float), `maxStrain` (out, float), `pieces` (out, int), `brokenEdges` (out, int) | — | D | Damage 0..1, worst strain, piece count and broken edges. `found` is false without a soft body. |
### Accessibility

Options, captions, narration and camera feel (`docs/ACCESSIBILITY.md`). Rows: `GraphAccessibilityNodeTable.cs`, owner `AccessibilityGraphInterop` (`Aver.Framework/Accessibility.cs`); palette group Accessibility (`GraphNodeDefsAccessibility.hpp`). Option keys, cues, speakers and texts are node attributes, never pins. `Access.Changed` (string key, float value) arrives through `OnEvent`.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetAccessibilityOption` | `exec` (in), `value` (in, float), `then` (out), `success` (out, bool) | `key` | P | Sets any accessibility option (clamped to its range); false for an unknown key. |
| `GetAccessibilityOption` | `value` (out, float), `found` (out, bool) | `key` | D | Reads an option. |
| `ShowCaption` | `exec` (in), `x`/`y`/`z` (in, float), `positional` (in, bool), `then` (out), `success` (out, bool) | `cue` | P | Shows a caption cue; positional applies its radius and direction hint. |
| `ShowCaptionText` | `exec` (in), `seconds` (in, float, 3), `kind` (in, int), `then` (out), `success` (out, bool) | `speaker?`, `text` | P | An ad-hoc caption line (text or `@Table:Key`); kind 0 dialogue, 1 sound, 2 music. |
| `StopCaption` | `exec` (in), `then` (out), `success` (out, bool) | `cue?` | P | Ends a cue's captions, or all when empty. |
| `Announce` | `exec` (in), `interrupt` (in, bool), `then` (out), `success` (out, bool) | `text` | P | Speaks through the OS voice; false when narration is off or no voice exists. |
| `StopSpeaking` | `exec` (in), `then` (out), `success` (out, bool) | - | P | Cuts off speech. |
| `IsSpeaking` | `result` (out, bool) | - | D | Speech in progress or queued. |
| `AddCameraShake` | `exec` (in), `trauma` (in, float, 0.5), `then` (out), `success` (out, bool) | - | P | Adds shake trauma (0..1, saturating). |
| `SetCameraBob` | `exec` (in), `amount` (in, float), `speed` (in, float), `then` (out), `success` (out, bool) | - | P | Head bob for this frame (amount 0..1, ground speed in cm/s); call each frame while moving. |
| `SetActionToggleAllowed` | `exec` (in), `allowed` (in, bool, true), `then` (out), `success` (out, bool) | `action` | P | False hides an action from the Input Assist page and ignores a saved toggle. |
### Blend shapes and lip-sync
Category Animation. Target, pose, rig, track and viseme names are attributes, never pins. Exec nodes take part in the push compiler only; the two reads work under either.

| Node | Inputs | Outputs | Attribute |
|---|---|---|---|
| Set Morph Weight | entity, weight, blendSeconds | then, success | target |
| Set Face Pose | entity, weight, blendSeconds | then, success | pose |
| Clear Morph Weights | entity, blendSeconds | then, success | target (empty = all) |
| Get Morph Weight | entity | found, weight | target |
| Set Face Rig | entity | then, success | rig (.ocface; empty = built-in) |
| Play Lip Sync | entity, rate, startSeconds, strength | then, success | track (.ocvis) |
| Stop Lip Sync | entity, blendSeconds | then, success | - |
| Queue Viseme | entity, startSeconds, durationSeconds, intensity | then, success | viseme or phoneme |
| Get Lip Sync State | entity | playing, time, length, strength | - |

See docs/ANIM_MORPH_FACE.md.
### Math, game flow, scene queries and the rest

The Aver Node and C# API sync (`docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md`) closed the gaps between what the public C# surface can do and what a graph can reach. Every node below is a table row, and its owner method is a thin `...ForGraph` wrapper over a public C# member, so a graph and a script call the same code. Pins and attributes are in the generated page of each family; this section gives the rules a pin table cannot.

Naming: a node whose name a user might also type as a plain verb or noun takes the `AN_` prefix (`AN_Tan`, `AN_GetTime`). The table key is the lower-case type id. The world-effects family keeps bare names because that is how it was specified. Failures return false or 0, log once per node id and never throw. Strings are attributes, entities and handles are `int` pins, vectors are loose `x`/`y`/`z` pins.

| Family | Page | Owner | Categories | Rules worth knowing |
|---|---|---|---|---|
| Math | [`nodes/Math.md`](nodes/Math.md) | `AverMath`, `AverRandom` (public, `Aver.Framework`) | Math, Integer, Random, Vector, Rotator | `AN_Log` of a value `<= 0` is 0 (the `Divide` convention, not `Sqrt`'s NaN). Integer nodes wrap unchecked; `AN_IntDivide` by 0 is 0 and `AN_IntModulo` takes the dividend's sign. Shift counts are masked to 0..31. `AN_IntCompare` is exact where a float loses integers above 2^24. The random nodes are **Exec**, not pure, because a pure node is recomputed per reader and two readers would get two values; `AN_RandomInt` is inclusive at both ends and `AN_SetRandomSeed` makes a sequence repeat. Rotator nodes use `Rot`'s convention (yaw about +Z, pitch about +Y, degrees). |
| Game flow | [`nodes/GameFlow.md`](nodes/GameFlow.md) | `GraphGameFlow` over `Game`, `Timers`, `Settings` | Game, Events, Settings | `AN_IsGamePaused` is the missing read (`Game.IsPlaying` is false while paused); `AN_SetGamePaused` freezes the session at the end of the frame. Timers can be queried, paused and reset by handle. The setting nodes read and write the project settings store by `key=`; a read returns its `fallback` when the key is unset or the store is closed. |
| Scene queries | [`nodes/SceneQuery.md`](nodes/SceneQuery.md) | `GraphSceneQueries` | Scene | No array pins, so a set is walked by count and index: `AN_CountActorsWithTag` plus `AN_GetActorWithTagAt` inside a counted `ForEach`. An index out of range gives entity 0. `AN_GetRagdollState` returns -1 when there is no ragdoll (0 Kinematic, 1 Limp, 2 Powered). |
| Render properties | [`nodes/RenderProps.md`](nodes/RenderProps.md) | `RenderPropsGraph` over the new partial `Entity` members (`EntityRenderProps.cs`) | Light, Camera, Particles | They write the scene's own component fields, so they work on any entity carrying the component. A light has no enable bit: switch it off with intensity 0. `AN_SetLightCone` takes degrees and keeps inner `<=` outer. `AN_SpawnEmitterAt` removes its entity after `lifetime` seconds (0 or negative: never). |
| Audio control | [`nodes/AudioControl.md`](nodes/AudioControl.md) | `AudioControlGraph` over `Audio`, `Voice`, `Music` | Audio | Master and bus volume, per-voice volume, pitch, position and stop, the occlusion curve, and `AN_GetAudioStats`. Voice handles come from the existing `AN_PlayStream`, `AN_PlayStreamAt` and `AN_PlayMusic` `voice` pin. |
| UI extensions | [`nodes/UiExt.md`](nodes/UiExt.md) | `UiExtGraph` over `Widget`, `UiSystem` | UI | A generic widget property (number or text), list items (`\|`-separated), selection, theme and the input state. UI scale is **not** here: it is the persisted accessibility option `graphics.uiScale`, set through `SetAccessibilityOption`. |
| World | [`nodes/World.md`](nodes/World.md) | `WorldGraph` | World | Sky clouds (`AN_SetSkyClouds`, `AN_ClearSkyClouds`) and `AN_SpawnFluidVolume`, which is queued: `success` means a provider accepted it. |
| Vehicles | [`nodes/Vehicle.md`](nodes/Vehicle.md) | `VehicleGraph` over the new public `Vehicles` and `CarSpec` | Vehicle | `AN_VehicleCreateCar` builds a car (chassis, four wheels, front steer, rear drive, engine) from `CarSpec.Default` and a scene entity that the physics step moves; `vehicle` is 0 on failure. Drive it with `AN_VehicleSetInput`. |
| World effects | [`nodes/WorldFx.md`](nodes/WorldFx.md) | `WorldFxGraph` over `Sky` and `PostFx` | World | Script overrides of the sun, fog and sky light, and named post-process layers (`framework_worldfx_abi.h`). Releasing an override restores the level's authored value. An absent `layer=` means the layer named `script`. A post layer is an infinite volume applied after the authored volumes and before the player's accessibility override, which wins by design. Both renderers consume the same CPU settings. |
| World origin | [`nodes/WorldOrigin.md`](nodes/WorldOrigin.md) | `WorldOriginGraph` over `WorldOrigin`, `WorldPos` and `Entity` | World | The floating origin (`framework_origin_abi.h`). A graph float is a single, so the nodes carry whole regions (16.384 km each) plus a position relative to the region's centre in centimetres, never a bare absolute: `AN_GetWorldOrigin` (region, generation, enabled), `AN_GetWorldRegion` (an entity's region and offset) and `AN_SetWorldRegion` (move an entity there). |

Also reachable from this wave without a node of their own: `Game.SetPaused`/`Pause`/`Resume`, `Game.ActorsOfClass`, the new `Sky` setters, `PostFx` and the `Entity` light, camera and emitter members (C# only, listed in `docs/api/Aver.Framework.md`). Features that were not given nodes on purpose: level streaming residency, `OpenLevel`, save slots, procedural audio, runtime landscape editing, material parameters, and String, Vec3 and array pin types (each named with its reason in the design document).

## Aliases invisible to the palette

The parser accepts a second spelling for seven node types. The Add-Node palette only ever spawns the canonical name — these aliases exist for a hand-written or C#-authored `.ocgraph`, never appear in the GUI, and are otherwise identical in every respect to the type they alias:

| Alias | Canonical type |
|---|---|
| `const_f32` | `ConstFloat` |
| `const_i32` | `ConstInt` |
| `const_bool` | `ConstBool` |
| `compare_f32` | `Compare` |
| `sub` | `Subtract` |
| `div` | `Divide` |
| `getparam` | `Param` |

## Parser vs. editor catalog

`sandbox/src/GraphNodeDefs.hpp` and the `GraphNodeDefs<Family>.hpp` files are a **deliberate, separate copy** of the parser's vocabulary, maintained by hand so the C++ editor never depends on a C# file (the header comment of `GraphNodeDefs.hpp` says why). The parser is the ground truth for a pin shape.

**Parity is enforced**, in two layers:

- **Table-driven nodes** (every `Graph<Family>NodeTable.cs` row): `python scripts/node-catalog.py --check`, registered as ctest `NodeCatalogCheck`, compares each row with its palette entry on pin name, type, direction, order and default value and on attribute keys, rejects a duplicate key across the tables, the palette and the parser's `case` labels, requires a test file for each newer family, and verifies the generated pages in [`docs/nodes/`](nodes/README.md). Its palette reader is a brace-matching scanner, because a single regular expression missed rows in `GraphNodeDefs.hpp`.
- **Hand-parsed nodes** (the `AddDefaultPins` vocabulary): `tests/editor/src/GraphEditorGeometryTest.cpp`, `testNodeCatalogCoversTheCompiler`, checks that the palette offers every type name the parser has a `case` for (it scrapes the labels as text); `buildCatalog()` also drops a repeated `(typeId, domain)`, so a family appended both by discovery and by hand is not listed twice. Pin shape for these nodes is still checked by hand against `AddDefaultPins`; `nodes/Core.md` lists what the palette says so a reviewer can compare.

The palette families are found by `cmake/GraphNodeFamilies.cmake`, which writes `sandbox/src/GraphNodeFamilies.gen.inc` (git-ignored); the C# tables are found by reflection (`GameSystemNodes`), and the managed test classes by an `[AutoRun]` attribute. A new family therefore edits no shared list.

## What a node cannot do

- **No string pin.** `PinType` has exactly four members: `Float`, `Int`, `Bool`, `Exec`
  (`Graph.cs:14-31`). Every string a node needs — `field=`, `class=`, `var=`, `name=`, `mesh=`,
  `material=` — arrives as a NODE-line key=value attribute, never a pin, and never as data an
  upstream node computes.
- **No array/collection pin.** `ForEach` is a counted-repeat, not a per-element iterator — see
  [Flow](#flow).
- **No immediate-mode 2D-drawing node.** The [UI nodes](#ui) drive widgets that already exist (`Aver.UI`); nothing draws.
- **Cross-entity event dispatch has one route.** `FireEvent` (see [Actor](#actor)) fires an `ENTRY` on another entity's graph, via `GraphEvents.FireEventForGraph(targetEntity, eventName)`. `Fire()` itself is something only a *host* calls into a specific `GraphHost` from outside the graph system (e.g. an animation notify) — no node lets a graph call `Fire()` directly on an arbitrary, ungated `GraphHost` reference.


