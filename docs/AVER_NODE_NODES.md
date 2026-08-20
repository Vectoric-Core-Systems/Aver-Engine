# Aver Node — Node Reference

Every node type the graph system knows, as of `41d6566`. This is the parser's own vocabulary:
each entry below is one `case` in `OcGraphParser.AddDefaultPins`
(`scripting/csharp/Aver.Graph/OcGraphParser.cs:606-1054`), which is the ground truth for a node's
default pin shape — not the editor palette, not this document's prose, not an older doc. Where the
editor's own catalog (`sandbox/src/GraphNodeDefs.hpp`, `buildCatalog()` at lines 101-330) and the
parser might disagree, **the parser wins**; see [Parser vs. editor catalog](#parser-vs-editor-catalog)
for how that was checked.

Compile-path behaviour is read from `scripting/csharp/Aver.Graph/GraphCompiler.cs` — specifically
`EmitNode`'s topological switch (`:332`–`:638`), the `IsExecCapable*` predicate family
(`:2386`–`:2580`), `IsExecOnlyNodeType` (`:3002`) and `EmitPullOutput`'s side-effect refusal
(`:3037`–`:3045`). Those line numbers were re-derived for this revision; the ones this paragraph
used to carry pointed into a version of the file about a thousand lines shorter.

This page is about node *shapes* — what pins a `NODE` of a given type gets and what it does. For the
class model, `PARAM`/`VAR`, and entry points, see **[the guide](VISUAL_SCRIPTING.md)**; for the
`.ocgraph` file grammar itself (every record's syntax, and where the C++ and C# readers disagree),
see **[`formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md)**.

**124 node types**, across 17 families — counted from `graphNodeCatalog()` rather than remembered,
and every one of them has a row below. `git log -p` on `OcGraphParser.cs` across its full history
shows no `case` label for a node type ever removed: every commit that touched this file added to
the vocabulary, never subtracted from it. The count in this line has been wrong before (it said 37
while the palette shipped 124), which is the argument for deriving it rather than typing it.

## Contents

1. [Read this first — three traps](#read-this-first--three-traps)
2. [Const](#const)
3. [Math](#math)
4. [Logic](#logic)
5. [Scene](#scene)
6. [Input](#input)
7. [Param](#param)
8. [Flow](#flow)
9. [Actor](#actor)
10. [Character](#character)
11. [Game](#game)
12. [Convert](#convert)
13. [Transform](#transform)
14. [Physics — reads and queries](#physics--reads-and-queries)
15. [Physics — writes and creation](#physics--writes-and-creation)
16. [Function](#function)
17. [Var](#var)
18. [Aliases invisible to the palette](#aliases-invisible-to-the-palette)
19. [Parser vs. editor catalog](#parser-vs-editor-catalog)
20. [What a node cannot do](#what-a-node-cannot-do)

### How to read a pin table

Each pin is `name (direction, type)`. `exec` is a pin **type**, exactly like `float`/`int`/`bool` —
a `LINK` between two `exec` pins is what a control-flow wire actually is; there is no separate
record for it (`Graph.cs:14-31`).

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

— `OcGraphParser.cs:603-604`. That is not "fill in whatever wasn't declared"; it is "if this node
has *any* explicit `PIN` record at all, stop, add nothing." A node type whose defaults include five
pins, given one hand-written `PIN` line, ends up with exactly one pin — the other four do not
silently reappear.

This is exactly why `test-content/GraphDemo/Content/Scripts/IdleMotion.ocgraph`'s `place` node
(`SetFieldVec3`, whose own default shape has **no** exec pins — see [Scene](#scene)) spells out all
seven of its pins by hand:

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

(`IdleMotion.ocgraph:122-133`, its own comment spells out the same trap in plain language). Leaving
off any one of those seven lines would not restore that pin from the default table — it would just
be gone.

### 2. Not every node runs on both compilers

A graph is one of exactly two shapes, decided once by `GraphHost.LoadFromText` from
`graph.EntryPoints.Count` (`GraphHost.cs:248-254`): zero `ENTRY` records compiles through
`Compile()` (pure dataflow, PULL); one or more compiles through `CompileEntryPoint()` (PUSH, walked
from an `ENTRY` node). The **Path** column above tells you which of the four behaviours a node type
gets under each. The practical read: `X`-path nodes (`OnStart`, `Branch`, …) are invisible to a
dataflow-only graph, and `P`-path nodes (`Spawn`, `SetVar`, `CharacterMove`) make a *dataflow* graph
fail to compile the moment they exist in the file at all — not when they're used, when they *exist*,
because `Compile()`'s topological pass visits every node in the graph regardless of whether it
feeds an `OUT` record (`GraphCompiler.cs:227-231`, `293-306`).

### 3. `Param` / `GetVar` / `SetVar`: the palette's pin type is a placeholder, not a promise

The parser types a `Param`/`GetVar`/`SetVar` node's `value` pin from the actual declared `PARAM`/
`VAR` — `Float`, `Int`, or `Bool`, whichever that record says (`OcGraphParser.cs:791-799,
960-966, 978-986`). The editor's Add-Node palette cannot do that: it has no declared `PARAM`/`VAR`
list to consult when the node is spawned, so it hardcodes the pin to `float`
(`GraphNodeDefs.hpp:190, 282, 291`, each with its own "adjustable per-instance" comment).

That hardcoded pin becomes a **real** `PIN` record the moment the node is dropped on the canvas
(`GraphEditor.cpp:757-758` copies the catalog entry's pins verbatim). Trap #1 above then applies:
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

The literal itself is a `value=` **NODE-line** attribute (e.g. `NODE k ConstFloat value=9.0`), read
generically by the `NODE`-line parsing loop (`OcGraphParser.cs:291-308`) — or, in the C++-writer
form, a default value trailing the `PIN … out float 5` record itself
(`OcGraphParser.cs:408-450`). Either way it lands in `graph.ConstantOutputs`, keyed by node id, not
on the `Pin` object.

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
`NaN`/`Infinity` (`GraphCompiler.cs:910-919`, `EmitDivide`'s own comment). A bare float divide
propagates `NaN`/`Infinity` silently through every downstream node until it surfaces frames later
as an object that vanished or exploded, nowhere near the divide that caused it — `0.0` is a value a
transform pipeline can keep running through. If a graph genuinely needs to detect the
divide-by-zero condition, there is no signal for it; a `compare-vs-self` `NaN` check would never
fire, because the result is never `NaN` in the first place.

`Sqrt`, `Pow`, `Floor`, `Ceil` and `Round` are the same shape as `Sin`/`Cos` below: a widen to
`double` for the `System.Math` call and a narrow back. **`Sqrt` does not inherit `Divide`'s
no-`NaN` convention** — `sqrt(-1)` really is `NaN` and really does propagate. That inconsistency is
deliberate rather than overlooked: `Divide` special-cases zero because dividing by a value that
happens to reach zero is a normal thing for a transform pipeline to do, while taking the root of a
negative number is a graph that has already computed something wrong.

`Sin`/`Cos` widen to `double` for the `System.Math` call and narrow back — both directions are
explicit `Conv_R8`/`Conv_R4` IL, called out in `EmitSin`'s own comment as a lesson already paid for
once (skipping either conversion still compiles, and silently reinterprets bits instead of
converting the value).

## Vector

THE ENGINE IS 3D AND EVERY MATH NODE ABOVE IS SCALAR. These take and return LOOSE COMPONENTS
rather than a vector value, because there is no Vec3 pin type -- `PinType` is `Float`, `Int`,
`Bool` and `Exec` -- and that is the same convention `GetFieldVec3` / `SetFieldVec3` already use.
All of them are PURE: no exec pins, safe to pull as often as anything asks, and an output nobody
reads costs nothing.

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
| `Compare` *(alias `compare_f32`)* | `a` (in, float), `b` (in, float), `result` (out, bool) | — | D | `a > b`, strict, via IL `Cgt` (`GraphCompiler.cs:611-624`). |
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

**`Compare` used to be the only comparison in this vocabulary, and there were no boolean
combinators at all.** `IdleMotion.ocgraph`'s own comment records the cost of that in the course of
explaining why it chose `OnStart` over a first-tick branch: *"There is no NOT node in the
vocabulary today either, which would have made the branch form awkward as well as unnecessary"*
(`IdleMotion.ocgraph:23-24`). The full set above closes that gap. `Compare` is unchanged and still
means `a > b`; `Greater` is a separate node type that does the same thing under the name a
reader looks for, so no existing graph had to be rewritten.

`Equal`/`NotEqual` compare floats **exactly**, with IL `Ceq`. There is no epsilon and no plan for
one: a tolerance small enough to be safe is too small to help, and one large enough to help is a
silent behaviour change in every graph that already worked. A graph comparing computed floats
should subtract, `Abs`, and `Less` against its own chosen epsilon — which is three nodes that say
what they mean, rather than one that hides the choice.

`Select`'s PULL-compiler behaviour is a genuine, deliberate asymmetry worth knowing: `Compile()`
computes **both** `ifTrue` and `ifFalse` regardless of `cond` (both upstream subgraphs already ran
during the topological walk; the branch there only picks which pre-computed local gets *loaded*).
`CompileEntryPoint`'s pulled form is a real short-circuit instead — `EmitPullInput` recursively
emits and runs only the chosen arm's subgraph (`GraphCompiler.cs:2312-2331`). Same node type, two
different cost profiles depending on which compiler reaches it.

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
| `SetMesh` | `entity` (in, int), `success` (out, bool) | `mesh=` | W | Ensures a mesh renderer on `entity`, pointed at `mesh=`'s asset path. |
| `SetMaterial` | `entity` (in, int), `success` (out, bool) | `material=` | W | Sets the material on `entity`'s mesh renderer. |

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
there is no reason to force an exec chain (`GraphCompiler.cs:756-759`).

**Wrong-path behaviour differs by node**, and the difference is worth knowing before you hit it:
- Pulling `SetField`/`SetFieldVec3`/`SetParent`/`SetName`/`SetMesh`/`SetMaterial` as a bare data
  value inside a PUSH graph with no exec edge into them fails with a clear, node-naming
  `InvalidOperationException`: *"has a side effect and must be reached by wiring it directly into
  the exec chain"* (`GraphCompiler.cs:2221-2230`).
- Pulling `Raycast` the same way fails too, from a different place: `Raycast` has no case in
  `EmitPullOutput` at all, deliberately, so it falls to that method's own `default` arm. This bullet
  used to say the resulting error text did not explain itself; it does — *"cannot be pulled as a
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
(`GraphCompiler.cs:636-639, 664-667`). A Vec3-kind field (`CLocal.position`, `CLight.colour`, …)
needs `GetFieldVec3`/`SetFieldVec3` instead — the pin-type model has no `Vec3` pin, so the split
into a second node pair is how a three-float value crosses the format at all, rather than a new
`PinType` member (`OcGraphParser.cs:653-659`).

**A TAG IS A BITMASK, NOT A STRING**, and that is why this family needed none of the compile-time
attribute machinery `class=`/`name=`/`field=`/`var=` exist for. `PinType` has no string, so every
other string-shaped API had to put its argument on the `NODE` line where a graph can never compute
it; `Entity.Tags` is a `uint` over `CTags.bits`, so an ordinary `int` pin carries it. A graph can
therefore **build** a mask at runtime `—` OR two together, read one out of a `VAR`, or test several
bits at once `—` which a string attribute could not have done at any price.

The pin is `int` rather than an unsigned type because `PinType` has none, and inventing one to
carry a bit pattern would be a new pin type for a reinterpretation. A mask with the top bit set
arrives as a negative int and works; `unchecked((uint))` is the whole of the conversion, in the
same direction `Entity.Tags` already does it.

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

`MoveAxis` has no `z` pin — `Input.MoveAxis`'s own Z component is hardcoded `0` in
`Aver.Framework/Input.cs`, so a pin that could only ever read a compile-time constant would add
noise, not information (`OcGraphParser.cs:883-886`).

**`InputKey` answers a STATE and the other two answer an EVENT**, and the output pin names say so:
`down` versus `triggered`. `down` is true every frame a key is held, which is the wrong answer for
jumping, firing a semi-auto, or toggling anything — all of which want one true per press. Before
`InputKeyPressed` existed, building that meant a `DoOnce` and a variable per key, while the
framework ABI (`aver_fw_input_key_pressed` / `_released`, what `Input.GetKeyDown`/`GetKeyUp` wrap
for C#) had answered it directly all along.

`InputKey` is the odd one out in this category: no exec pins, `D` path, freely pullable from either
compiler. Reading one polled key is idempotent — it returns the same answer however often it's
asked within a frame — so there's nothing to cache and no exec visit needed to anchor it to
(`OcGraphParser.cs:821-828`). `MouseDelta`/`MoveAxis` are given `Raycast`'s exec-cached shape
instead, even though neither read has `Raycast`'s per-call *cost* — the reason is a correctness
guarantee, not a cost one: "one frame of input costs exactly one native call regardless of how many
output pins a graph reads" has no other enforcement mechanism in the PUSH compiler besides the same
exec-visit caching `Raycast` uses (`GraphCompiler.cs:1704-1720`). `key` is a plain `int` with no
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

**One per type, not one generic.** `Validate` refuses a link whose two pins disagree on type and
there are no generic pins here, so a single wildcard `Reroute` would have to weaken that check —
the check that catches every genuinely wrong wiring. Four nodes is the cheaper price.

**`DoOnce`, `Gate` and `FlipFlop` REMEMBER something between activations**, which no other node
here does — `Branch` and `Sequence` decide from their inputs alone. That memory lives in the same
per-instance `GraphVarStore` a `VAR` uses, under a reserved name built from the node id (`$flow$`
plus the id, and a `VAR` name cannot contain `$`), so two entities running one graph file gate
independently. **A graph that declares no `VAR` records has no store at all**, and these three
refuse to compile in one with a message naming the node: declare any variable to give it one.

`Gate`'s `open`/`close` are **bool inputs, not exec pins**. An exec input can be driven by many
sources, so three separate exec entries would make "which one fired" unanswerable inside a single
activation. Both are sampled every activation and applied before the test, so opening and firing in
one activation works; `close` is applied after `open`, so a graph wiring both true ends closed —
one stated rule rather than an order that depends on which link the parser read first. `DoOnce`'s
`reset` is a bool for the same reason.

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
(`OcGraphParser.cs:774-786`, `GraphCompiler.cs:2191-2198`).

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
a graph run) by `tests/anim.scene/src/AnimNotifyGraphTest.cpp`. This is what the note above about
`GraphEvents.cs` having "no native caller" used to describe; it has one now.

`Sequence` defaults to two exec outputs (`then0`, `then1`); add more by hand-writing additional
`PIN … out exec` records — the compiler fans out however many exec-output pins the node actually
has, in file order, needing no special case (`GraphCompiler.EmitExecFanOut`). `tookTrue` and
`fireLog` are opt-in observability, not required wiring — a way to prove which way a branch went or
how many times a sequence fired without a live scene to read back from.

`ForEach` is the **counted-repeat** variant, not a per-element iterator — the format has no
array/collection pin type, so "for each item in a list" cannot be expressed at all today
(`OcGraphParser.cs:743-749`, called out in the parser's own comment as "left rough for phase 2").

`While`/`ForEach` share one runaway-loop guard: `GraphCompiler.MaxLoopIterations = 100_000`
(`GraphCompiler.cs:64`). Past that many passes the loop stops and logs a warning naming the node and
event (`WarnLoopGuardTripped`, `GraphCompiler.cs:2606-2611`) — it does not hang or throw.

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

`FireEvent` is how one graph talks to another. It carries **no payload**: the receiving `ENTRY` gets
the same closed `PARAM` vocabulary every entry gets (`entity`, `time`, `deltaTime`) and nothing else,
so anything the sender wants understood must be staged into the target's own `VAR`s first — which is
order-sensitive and worth designing deliberately rather than discovering.

`fired` is false, with a warning naming the reason, when the target does not exist, has no graph, or
declares no such event. The warning is emitted **once per (target, event) pair**, not once per tick,
because a mis-wired `FireEvent` on an `OnTick` chain would otherwise bury the log. Reentrancy is
bounded by a depth guard: a graph firing at itself, two graphs firing at each other, and a three-deep
cycle all terminate rather than overflowing the stack.

`Spawn` and `CharacterMove` are refused by `Compile()` **outright**, not merely skipped: a stray,
even unreachable, `Spawn`/`CharacterMove` node anywhere in a no-`ENTRY` dataflow graph fails to
compile with an explicit error naming the node
(`GraphCompiler.cs:472-489, 491-505`). This is a full step stricter than the `W`-path writes in
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
(`framework_abi.h:206`). There is no return code to surface, so none was invented
(`OcGraphParser.cs:1014-1021`).

`CharacterMove.success` is a real outcome, not a stub: `false`, logged, never a throw, when `entity`
isn't a live actor at all, or is a live actor that isn't an `AverCharacter`
(`OcGraphParser.cs:929-932`).

## Character

The gameplay API of `AverCharacter`, reached by entity handle. Every one of these takes an
`entity` pin rather than acting on "this" actor, because a graph class and the character it
drives are not the same entity — see [Transform](#transform) for the same convention applied
to scene entities generally.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetVelocity` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads an AverCharacter's current velocity, in centimetres per second. **success is false and x/y/z stay 0 when the entity has no live AverCharacter bound to it, or is a character whose physics capsule hasn't been created yet (GraphInterop.cs:397-405) -- zero velocity and "no answer" read identically unless success is checked.** |
| `IsGrounded` | `entity` (in, int), `grounded` (out, bool) | — | D | Reports whether an AverCharacter is currently standing on ground shallow enough to hold. **Returns false, silently, for an entity that isn't an AverCharacter at all -- unlike GetVelocity/SetVelocity/Teleport it bypasses the CharacterFor helper and logs no warning (GraphInterop.cs:420-423).** |
| `SetVelocity` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out), `success` (out, bool) | — | P | Replaces an AverCharacter's velocity outright -- a launch pad, a dash, a dead stop, not an added force. **Does nothing and reports success = false on a character that has never been simulated (no physics capsule yet) -- unlike Teleport, it does not create one for you (GraphInterop.cs:410-416; Character.cs:120-138).** |
| `Teleport` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out), `success` (out, bool) | — | P | Moves an AverCharacter and its physics capsule to a feet position, clearing velocity. **Not SetFieldVec3 on CLocal.position -- per the palette's own comment (GraphNodeDefs.hpp:135-139), setting the transform alone leaves the capsule behind and the character snaps back next step. Teleport moves both and zeroes velocity, and -- unlike SetVelocity/GetVelocity -- works even on a character that was never simulated, because it calls EnsureCapsule() itself (Character.cs:243-252, 263-271).** |

`GetVelocity`/`IsGrounded` are pure reads, reachable from either compiler exactly like `GetFieldVec3`/`GetForward` — no exec pins, and each has a matching case in *both* `Compile()`'s topological switch and `EmitPullOutput`. `SetVelocity`/`Teleport` are Actor-family side effects instead, but they are **not** refused the way `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` are — those four get their own hand-written, node-naming `InvalidOperationException` case inside `Compile()`'s own switch. `SetVelocity`/`Teleport` have no case there at all, so a stray, unwired one reached by `Compile()`'s topological pass (which walks every node in the graph, wired or not) falls through to the switch's `default` arm, which recognises a push-only node and refuses it with a node-naming `InvalidOperationException` saying the type IS supported and pointing at `CompileEntryPoint()`. The error a graph author actually sees in the common case — pulling `SetVelocity`/`Teleport`'s `success` output as data with no `exec` edge into the node — comes from a different place: `EmitPullOutput`'s own combined side-effect check, which groups `setvelocity`/`teleport` beside `Jump` and `FireEvent` (and `Print`, and the physics/transform writers) in one condition and raises a clear, node-naming `InvalidOperationException` there instead. Either way, `Compile()` cannot run them; only `CompileEntryPoint()`, through `EmitExecApiCall`, actually calls `SetVelocityForGraph`/`TeleportForGraph`.

A second trap sits in `Aver.Framework` itself, invisible from the pin table. `GetVelocity`/`SetVelocity`/`Teleport` all resolve their entity through the same `CharacterFor` helper, which logs one warning and returns null for anything that isn't a live `AverCharacter`; `IsGrounded` does not — it reads `Actors.Get` directly and returns `false` with no log for the identical case. And `GetVelocity`/`SetVelocity` both require an *already-simulated* character (`success = false`, and for `GetVelocity` the vector left at zero, when the physics capsule doesn't exist yet) — `Teleport` does not, because it calls the character's own `EnsureCapsule()` internally and so works, and creates the capsule, even on a character that has never been simulated.

## Game

The `Game` statics: who is playing, what they are possessing, and the two nodes that change
that. `index` on the player nodes is a PLAYER index, not a pawn or entity index.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetPlayerPawn` | `index` (in, int), `entity` (out, int) | — | D | Reads the pawn currently possessed by a player's controller, as an entity handle. **`index` is a player index, not a pawn index. The value returned is whatever pawn that player's controller currently possesses (via `aver_fw_controlled_pawn`), so it comes back 0/`Entity.None` whenever that player is unpossessed right now, not only when `index` is out of range.** |
| `GetPlayerController` | `index` (in, int), `entity` (out, int) | — | D | Reads a player's controller entity by 0-based player index; `Entity.None` if there is none. |
| `GetGameMode` | `entity` (out, int) | — | D | Reads the current GameMode's entity; `Entity.None` outside a play session. **Returns `Entity.None` any time there is no play session (editor), not specifically when a GameMode class is missing — this pin alone can't tell "no session" apart from "session with a null GameMode".** |
| `IsPlaying` | `playing` (out, bool) | — | D | True only while a play session is actively ticking. **False while the session is Paused, not just in the editor — `State` has three values (Editor/Playing/Paused) and this pin collapses two of them to false. A graph that wants "is a session running at all" (paused or not) needs `Game.HasSession`, which no node currently wraps.** |
| `Possess` | `exec` (in), `controller` (in, int), `pawn` (in, int), `then` (out), `success` (out, bool) | — | P | Makes the entity at `controller` take control of the entity at `pawn`, stealing it from another controller if one already holds it. **`controller` must resolve (via `Actors.Get`) to an actor that IS an `AverPlayerController` — pass a plain entity or a pawn there and it logs a warning and returns `false` rather than throwing. Also, `Possess`/`Unpossess` have no case at all in EmitNode's own topological switch, and are not in IsExecOnlyNodeType either, so Compile() calls EmitNode on them for EVERY node in the graph regardless of wiring and reaches its `default` arm. A `Compile()`-driven graph merely CONTAINING a Possess/Unpossess/SetVelocity/Teleport node — wired or not — therefore fails to compile, which is deliberate; the arm names the node and says the type is supported rather than claiming it is not.** |
| `Unpossess` | `exec` (in), `controller` (in, int), `then` (out), `success` (out, bool) | — | P | Releases whatever pawn the entity at `controller` currently possesses. **`success` is false for two different reasons the pin can't distinguish: `controller` isn't an `AverPlayerController` at all, or it is one but had nothing possessed. Same EmitNode-has-no-case nuance as Possess above: any Compile()-driven graph merely containing an unwired Unpossess node fails outright with a generic "not supported" error, before its own more explicit PULL-refusal message would ever be reached.** |

All six nodes' pins agree exactly between the palette (`GraphNodeDefs.hpp:144-149`) and `AddDefaultPins` (`OcGraphParser.cs:1023-1054`) — no palette/parser disagreement to report here. The four readers (`GetPlayerPawn`, `GetPlayerController`, `GetGameMode`, `IsPlaying`) are the simplest possible `D` shape: zero or one input, one output, dispatched through the shared `EmitSimpleApiRead` on the PUSH side and a direct `EmitPullOutput` case on the PULL side, with no branch or refusal anywhere.

`Possess`/`Unpossess` are `P`-path by the task's own test — they sit in `IsExecCapableApiCallType`, which is OR'd into `EmitPullOutput`'s refusal chain on the exact same line as `IsExecCapableJumpType` and `IsExecCapableFireEventType` (`GraphCompiler.cs:3037-3045`). But unlike `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` — which get their own hand-written `case` in `EmitNode`'s topological switch that throws a tailored, node-naming `InvalidOperationException` explaining *why* — `Possess`, `Unpossess`, and their siblings `SetVelocity`/`Teleport` have **no case in `EmitNode` at all**. Since none of the four are in `IsExecOnlyNodeType` either, `Compile()`'s topological pass calls `EmitNode` on them unconditionally for every node in the graph, wired or not, and they reach its `default` arm — before `EmitPullOutput`'s own refusal ever gets a chance to run. The practical trap: a `Compile()`-driven (no-`ENTRY`) graph that merely *contains* an unreferenced `Possess`/`Unpossess`/`SetVelocity`/`Teleport` node fails to compile at all, with a plain "not supported" message rather than the friendlier "has a side effect and must be reached by wiring it directly into the exec chain" text the four `EmitNode`-cased side effects give. The end state is the same (P-path, refused outright) but the diagnostic a graph author actually sees is worse for this family than for Spawn/CharacterMove/FireEvent/SetVar.

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

All three share one code path both ways: `EmitNode`'s topological switch dispatches every one of them to the same `EmitSimpleApiRead` helper (`GraphCompiler.cs:467-471`), which does nothing but call `EmitPullOutput` per output pin and store the result — there's no separate "why it's safe to run unconditionally" story to learn here, because these are arithmetically identical to `RerouteFloat`/`RerouteInt`/`RerouteBool`: one input read, one conversion opcode, no side effect. `IntToFloat` and `BoolToFloat` even compile to the literal same instruction — `int` and `bool` are both `I4` on the CIL stack, so widening either to `float` is one `Conv_R4` either way; the two node types exist so the GRAPH can tell them apart, not because the generated code differs (`GraphCompiler.cs:3191`, its own comment). None of the three appear in either compiler's refusal list or exec-only dispatch, so — like every other `Convert` node — they're freely pullable from a dataflow graph with no `ENTRY` at all, and freely usable inside a PUSH chain too, with no wiring requirement beyond having something feed `a`.

## Transform

Reads and writes on a scene entity's TRANSFORM, plus the two liveness queries. **Local is not
world**: `GetFieldVec3 CLocal.position` only ever gave the local half, and a parented
object's local position never changes as its parent moves. The axis nodes report the
TRANSFORM's orientation, deliberately not `GetForward`, which is an `AverCharacter`'s look
direction with its pitch clamp already applied.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetWorldPosition` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads an entity's WORLD-space position, composed through any parents, in centimetres. **LOCAL IS NOT WORLD: `GetFieldVec3` on `CLocal.position` reads local position only — a gun parented to a moving camera has the same local position forever. This node is the one that tracks where the entity actually is (GraphNodeDefs.hpp's own comment, lines 167-171).** |
| `GetEntityForward` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space forward axis (+X), composed through parents and normalised. **Not the same node as the existing `GetForward` (palette label "Get Forward (Look)", category Scene, GraphNodeDefs.hpp:311): that one reads an `AverCharacter`'s look direction including its pitch clamp. This node reads the raw WORLD-space +X axis of ANY entity's transform — the GraphInterop method's own comment says so explicitly (Aver.Framework/GraphInterop.cs:489-490).** |
| `GetEntityRight` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space right axis (+Y), composed through parents and normalised. |
| `GetEntityUp` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's WORLD-space up axis (+Z), composed through parents and normalised. |
| `GetLocalScale` | `entity` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads the entity's local scale multipliers (unitless). **On a dead/invalid entity this reports `x=y=z=0, success=false` — NOT `Entity.LocalScale`'s own default of `(1,1,1)` a raw C# read would give. GraphInterop.LocalScaleForGraph checks `IsAlive` and zeroes the outputs BEFORE ever touching the property (GraphInterop.cs:501-509).** |
| `IsAlive` | `entity` (in, int), `alive` (out, bool) | — | D | Whether the entity handle still names a live entity in the scene. **Asks the SCENE whether the handle still names anything, not merely whether it is non-zero — `Entity.IsValid` (handle != 0) is a weaker, different check. A graph holding a handle across frames wants IsAlive (GraphInterop.cs:530-531's own comment).** |
| `IsActor` | `entity` (in, int), `isActor` (out, bool) | — | D | Whether a gameplay class owns this entity, rather than it being a plain scene node. |
| `Translate` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Moves the entity by (x, y, z) centimetres, added to its current LOCAL position — a relative nudge, not an absolute set. **INCREMENTAL, not absolute, and LOCAL space, not world: `Entity.Translate(delta) => SetLocalPosition(LocalPosition + delta)` (Entity.cs:69). Moving along a world direction (e.g. `GetEntityForward`'s output) needs scaling that vector by a speed and wiring the result in yourself — this node does not convert a world-space delta for you.** |
| `SetLocalScale` | `exec` (in), `entity` (in, int), `x`/`y`/`z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the entity's local scale multipliers outright. **An absolute 'set the value' write — the same shape as `SetFieldVec3`, which the parser runs safely unconditionally under Compile() (W, see Scene table) — yet `SetLocalScale` is bucketed with `Translate`/`DestroyEntity` as P because all three share one emitter (`EmitExecTransformWrite`) and one predicate (`IsExecCapableTransformWriteType`, GraphCompiler.cs:2552-2556). Compile() refuses it outright even though overwriting the same scale twice would in fact be harmless.** |
| `DestroyEntity` | `exec` (in), `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Destroys the entity and its subtree — the full framework teardown if it's an actor, a plain scene delete otherwise. **`success` is `false`, not a throw, when the entity is already dead (GraphInterop.cs:538-544 checks `IsAlive` first). Destroying an actor runs the full framework teardown (`Fw.aver_fw_destroy`); a plain scene entity gets a lighter `aver_scene_destroy` (Entity.cs:175-179) — either way the whole subtree goes with it, so a child spawned under an actor does not need its own `DestroyEntity` node.** |

All ten types were added together (GraphNodeDefs.hpp's own comment at lines 167-171 calls this out as "THE ENTITY TRANSFORM, which `GetFieldVec3` on `CLocal.position` only half covered"), and the palette and `AddDefaultPins` agree, pin-for-pin, on every one of them — no mismatches found in this family.

The seven readers (`GetWorldPosition`, `GetEntityForward`/`Right`/`Up`, `GetLocalScale`, `IsAlive`, `IsActor`) are all dispatched the same way twice over: `EmitNode`'s dataflow switch routes all seven to one shared case (`EmitSimpleApiRead`, `GraphCompiler.cs:457-465`), which itself calls `EmitPullOutput` for every output pin rather than computing anything directly — so the *real* per-type logic lives once, in `EmitPullOutput`'s own switch (`GraphCompiler.cs:3177-3190`). `GetEntityForward`/`Right`/`Up` share a single native surface, `GraphInterop.EntityAxisForGraph(entity, axis, ...)`, differing only in the axis index (0/1/2) the emitter hardcodes per node type (`GraphCompiler.cs:3179-3184`) — three node types, one method, by design (`GraphInterop.cs:485-488`). All seven readers zero their outputs and report `success=false` on a dead handle *before* ever touching the underlying `Entity` property, which matters because two of those properties have friendlier fallbacks a raw C# caller would see instead: `Entity.LocalScale` defaults to `(1,1,1)`, not `(0,0,0)`, and `Entity.WorldForward`/`Right`/`Up` fall back to the local (unrotated-by-world) axis rather than reporting failure. A graph reading these nodes never sees either fallback — dead means zero, full stop.

The three writers (`Translate`, `SetLocalScale`, `DestroyEntity`) are **P**, but by a different mechanism than `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` get. Those four each have their own explicit `case` in `EmitNode`'s topological switch that throws a hand-written `InvalidOperationException` naming the node and explaining why (`GraphCompiler.cs:562-611, 619-635`). `Translate`/`SetLocalScale`/`DestroyEntity` have **no case at all** in that switch, so `Compile()` reaches its `default` arm instead. The practical effect is identical (Compile() refuses a graph carrying any of the three, wired or not), and the message is not generic: the default arm tests for a push-only node first, and only then falls back to "not supported". Separately, all three also sit in `EmitPullOutput`'s side-effect refusal list via `IsExecCapableTransformWriteType` (`GraphCompiler.cs:3044`), so pulling one's `success` (or, for `Translate`/`SetLocalScale`, any output) as a bare data value from inside a PUSH graph with no exec edge fails with the same named `InvalidOperationException` the Scene writers give (`GraphCompiler.cs:3046-3050`).

## Physics — reads and queries

The `Body` handle and the queries over it. A `Body` is a physics handle, distinct from an
entity handle — both arrive as `int` pins, and nothing in the type system stops you wiring one
where the other belongs, which is the trap to know about in this whole family.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `GetBodyPosition` | `body` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads a physics body's centre-of-mass position, in centimetres. **`success` only means the handle is nonzero (`Body.IsValid` is literally `Handle != 0`, no native check — Physics.cs:131) — after `DestroyBody` runs, the same handle still reads `success = true` with `x = y = z = 0.0` (Body.Position's own silent-zero-on-native-failure, Physics.cs:139), indistinguishable from a live body genuinely sitting at the origin.** |
| `GetBodyVelocity` | `body` (in, int), `x`/`y`/`z` (out, float), `success` (out, bool) | — | D | Reads a physics body's linear velocity, in centimetres per second. **Same trap as `GetBodyPosition`: `success` only proves the handle is nonzero, not that the body still exists — a destroyed body's handle reads `success = true`, `x = y = z = 0.0`, indistinguishable from a live body at rest (Physics.cs:131, 159).** |
| `IsBodyValid` | `body` (in, int), `valid` (out, bool) | — | D | Reports whether a body handle currently names a body. **Checks only that the handle number is nonzero — it never calls into native physics, so a handle from a body already removed via `DestroyBody` still reads `valid = true` (Physics.cs:131).** |
| `GetBodyCount` | `count` (out, int) | — | D | Returns how many physics bodies currently exist in the simulation. |
| `RaycastAny` | `originX`/`originY`/`originZ` (in, float), `dirX`/`dirY`/`dirZ` (in, float), `maxDist` (in, float), `hit` (out, bool) | — | D | Casts a ray up to `maxDist` centimetres and reports only whether it hit anything, discarding the point, normal, and owner a full raycast would give. **Exposes no `body`/point/owner pin at all — even on a hit there is no way to learn what was struck from this node alone; reach for `Raycast` (scene entity) or `SphereCast` (physics body) when the graph needs to know what it hit, not just that something is there.** |
| `SphereCast` | `exec` (in), `originX`/`originY`/`originZ` (in, float), `dirX`/`dirY`/`dirZ` (in, float), `maxDist` (in, float), `radius` (in, float), `then` (out, exec), `hit` (out, bool), `body` (out, int), `pointX`/`pointY`/`pointZ` (out, float) — 15 pins | — | P | Sweeps a sphere of `radius` outward from the origin along the given direction, up to `maxDist` centimetres, and reports the first physics body it touches. **The sweep never resolves an owning entity (`Physics.SphereCast`'s `RaycastHit.Entity` is hardcoded to `Entity.None`, Physics.cs:288) -- only the physics `body` handle comes back on this node's pins; stamp one with `SetBodyEntity` first if a graph needs the owner. And because SphereCast has no case at all in `EmitNode`'s switch (unlike `Raycast`, which does), a data-only graph merely containing an unwired SphereCast node fails Compile() with the bare 'Node type 'SphereCast' is not supported' -- not the purpose-written 'must be reached by wiring it into the exec chain' message Spawn/CharacterMove/FireEvent/SetVar give; give it an ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.** |

The physics family splits cleanly along the line `OcGraphParser.cs:1150`'s own comment states outright — *"Readers are pure; writers, creators and the sweep carry exec pins"* — and the palette agrees with the parser pin-for-pin on all six nodes here, so there is nothing to log in `mismatches`. `GetBodyPosition`, `GetBodyVelocity`, `IsBodyValid`, `GetBodyCount` and `RaycastAny` have no exec pins and are freely pullable from either compiler, `RaycastAny` included even though it runs a full `Physics.Raycast` internally on every pull and only keeps the bool — it simply has no exec pins to be cached against the way the Scene category's `Raycast` does. `SphereCast`, "the sweep", is the odd one out, and worth knowing before you hit it: it is not merely exec-only like `Branch`/`OnTick` (which `Compile()` silently skips), and it is not in the same handled bucket as `Raycast`, `MouseDelta`, `MoveAxis` either (all three of which DO have a case in `EmitNode`'s topological switch, so `Compile()` runs them once, unconditionally). `SphereCast` has no case anywhere in `EmitNode`, so a pure-dataflow graph merely *containing* one — wired or not — fails to compile, reaching the `default` arm rather than a hand-written case. The message it gets there names the node and points at `CompileEntryPoint()`, the same as the four hand-written refusals, just without their node-specific wording.

Separately: `IsBodyValid` and the `success` pin on `GetBodyPosition`/`GetBodyVelocity` all bottom out in `Body.IsValid`, which is literally `Handle != 0` (`Physics.cs:131`) — none of the three ever asks native physics whether that handle still names a live body. A handle read after `DestroyBody` has already run reports `valid = true`, and `GetBodyPosition`/`GetBodyVelocity` on that same handle report `success = true` with `x = y = z = 0.0` (`Body.Position`/`Velocity`'s own native call fails silently to `Vec3.Zero`, `Physics.cs:139, 159`, with nothing upstream re-checking it) — a destroyed body reads exactly like one that is very much alive and sitting at the origin.

## Physics — writes and creation

Writes, and the five creators. Every one of these is `P`: a physics write ticked
unconditionally by the dataflow compiler would run on every invocation with no way to gate
it, and a CREATOR would make a new body every frame.

| Node | Pins | Attribute | Path | What it does |
|---|---|---|---|---|
| `SetBodyPosition` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Teleports a body to a new position, in centimetres, ignoring collision on the way. **Moves through walls -- there is no collision response on the way, unlike driving the body by velocity.** |
| `SetBodyVelocity` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Replaces a body's linear velocity outright, in centimetres per second. |
| `AddBodyVelocity` | `exec` (in), `body` (in, int), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Adds to a body's current velocity -- a plain velocity change, not a mass-scaled impulse, so a heavy and a light body get the identical speed change for the same x/y/z input. **Reads like an impulse node (knockback, explosion, jump pad), but there is no mass division anywhere in the call chain -- it is SetVelocity(Velocity + delta), full stop. A 1 kg crate and a 1000 kg crate both get exactly the same delta-v.** |
| `DestroyBody` | `exec` (in), `body` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Removes a body from the simulation; the handle is dead afterwards. |
| `SetBodyEntity` | `exec` (in), `body`, `entity` (in, int), `then` (out, exec), `success` (out, bool) | — | P | Stamps a scene entity as a physics body's owner, so a later Raycast against that body reports the entity through `RaycastHit.Entity` instead of 0 (unowned). **A body created by AddStaticBox/AddDynamicBox/AddDynamicSphere/AddSensorBox/AddSensorSphere reports NO owner to Raycast (entity 0, read as 'unowned') until this node runs on it -- the palette's own Physics section comment calls this out explicitly (GraphNodeDefs.hpp:183-184).** |
| `SetGravity` | `exec` (in), `x`, `y`, `z` (in, float), `then` (out, exec), `success` (out, bool) | — | P | Sets the whole simulation's gravity vector, in centimetres per second squared. The engine default is (0, 0, -980). **`success` is always true here -- SetGravityForGraph has no failure path to report (unlike the other five writers, whose `success` reflects whether `body` was a valid handle), because setting a global has nothing to be invalid against.** |
| `AddStaticBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box collider that never moves, centred at (cx, cy, cz) with half-extents (hx, hy, hz), both in centimetres. |
| `AddDynamicBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz`, `mass` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box collider that falls and collides, with `mass` in kilograms; `mass` <= 0 derives the mass from the box's volume instead. **Leaving `mass` unwired is not an error -- an unwired float pin reads 0, which is exactly the "derive mass from volume" sentinel, not a zero-mass body (GraphInterop.cs:620-621).** |
| `AddDynamicSphere` | `exec` (in), `cx`, `cy`, `cz`, `radius`, `mass` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a sphere collider that falls and collides, `radius` in centimetres; `mass` <= 0 derives the mass from the sphere's volume instead. **Same unwired-mass rule as AddDynamicBox: 0 (an unwired pin's default) means "derive from volume", not "massless".** |
| `AddSensorBox` | `exec` (in), `cx`, `cy`, `cz`, `hx`, `hy`, `hz` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a box-shaped trigger volume that reports overlaps but never collides physically. |
| `AddSensorSphere` | `exec` (in), `cx`, `cy`, `cz`, `radius` (in, float), `then` (out, exec), `body` (out, int) | — | P | Creates a spherical trigger volume that reports overlaps but never collides physically. |

All eleven are `P`-path for the same reason `Spawn`/`CharacterMove`/`FireEvent`/`SetVar` are: each wraps a Jolt call that either mutates simulation state (`SetBodyPosition`, `SetBodyVelocity`, `AddBodyVelocity`, `DestroyBody`, `SetBodyEntity`, `SetGravity`) or creates a new body (`AddStaticBox`, `AddDynamicBox`, `AddDynamicSphere`, `AddSensorBox`, `AddSensorSphere`), and Compile()'s topological pass has no branch structure to gate any of that with. They are reached only from `EmitExecNode`'s default arm, through `IsExecCapablePhysicsWriteType`/`IsExecCapablePhysicsCreateType` (GraphCompiler.cs:2537-2547) dispatching to `EmitExecPhysicsWrite`/`EmitExecPhysicsCreate` (GraphCompiler.cs:1010-1067) -- give any of them an `ENTRY`-driven exec chain and reach them through `CompileEntryPoint()`.

A **body is not an entity** (`GraphNodeDefs.hpp:182-186`): a body is a Jolt handle with a shape and a velocity; an entity is a scene node that may or may not own one. A body an `Add*` node creates reports **no owner** to `Raycast` until `SetBodyEntity` stamps one on -- a trigger the graph just built is invisible to the graph asking what it hit. Body handles ride on `int` pins throughout, same convention as scene entity handles, and `0` means invalid on both sides (`Physics.cs:124-131`).

The five writers share one dispatch shape: `EmitExecPhysicsWrite` pulls `body` first for all of them except `SetGravity` (GraphCompiler.cs:1014, the one node in the family with no `body` pin), then the per-type inputs, then stores the returned bool into `success` or drops it. The five creators share the mirror shape: `EmitExecPhysicsCreate` always pulls `cx`/`cy`/`cz` first, then per-type extents/radius/mass, and stores the returned handle into an **exec-local**, not a pin-local -- the same reasoning `Raycast` uses, so the `body` pin any downstream node reads back is the *one* handle this creation produced, however many nodes read it. `AddStaticBox`/`AddSensorBox`/`AddSensorSphere` have no `mass` pin at all: a static body is immovable and a sensor never participates in dynamics, so mass has nothing to mean for either.

`AddBodyVelocity`'s name invites reading it as an impulse -- knockback, an explosion, a jump pad -- but `Body.AddVelocity` is exactly `SetVelocity(Velocity + delta)` (`Physics.cs:169`): no mass division, no scaling, just a direct velocity change. `SetBodyVelocity` replaces the velocity outright; `AddBodyVelocity` adds to whatever it already had. Neither one is mass-aware.

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
(`OcGraphParser.cs:960-966, 978-986`; `Graph.cs:378-415`). Same editor-palette placeholder-type trap
as `Param` too — see trap #3 above.

`GetVar` is a pure read (`D` path, reachable from either compiler with no exec needed at all —
`GraphCompiler.cs:2298`, `EmitPullGetVar`). `SetVar` is `P`-path, refused by `Compile()` outright for
the identical reason `Spawn`/`CharacterMove` are: a write has no "same value twice is harmless"
excuse the way a native field write does, so an ungated `SetVar` in a dataflow graph would overwrite
the variable every single invocation with no way to stop it
(`GraphCompiler.cs:513-529`). `SetVar` has no `success` output — a write into an in-process
`GraphVarStore` has no runtime failure mode the way a native field write does (unknown entity,
missing component), so there is nothing left to report.

A `VAR`'s value lives on the owning `GraphHost` and survives from one compiled-delegate invocation
to the next on that same host — but not a `Load()` call, hot-reload included, and not a process
restart. The full storage/lifetime story belongs to the guide, not this reference; this table only
covers the node's pin shape.

---

## Aliases invisible to the palette

The parser accepts a second spelling for seven node types (`OcGraphParser.cs:608-693, 788-789`).
The Add-Node palette only ever spawns the canonical name — these aliases exist for a hand-written
or C#-authored `.ocgraph`, never appear in the GUI, and are otherwise identical in every respect
(pins, attribute, compile path) to the type they alias:

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

`sandbox/src/GraphNodeDefs.hpp` is a **deliberate, separate copy** of the parser's vocabulary,
maintained by hand so the C++ editor never depends on a C# file
(`GraphNodeDefs.hpp:20-27`, its own header comment). Every node type's pin shape — name, type,
direction, declared order — has been checked against `AddDefaultPins` for this document: **zero
disagreements**, including the least obvious shapes (`Raycast`'s 14 pins, `SphereCast`'s 15,
`CharacterMove`'s 9, `SetFieldVec3`'s 5-pin order). The most recent sweep covered the 42 nodes
added since the previous one and re-derived each one's Path code from the compiler independently
of the palette; it found no pin disagreement either.

That parity is not enforced by anything automated, though. The one existing coverage test
(`tests/editor/src/GraphEditorGeometryTest.cpp`, `testNodeCatalogCoversTheCompiler`) checks that
both files mention the same *set* of type names, by scraping `case "x":` labels as text — it does
not compare pin shape. Nothing today would catch the next node addition landing with a mismatched
pin order between the two files. Until such a test exists, cross-checking a change to either file
against the other is a manual step, not a guaranteed one.

## What a node cannot do

- **No string pin.** `PinType` has exactly four members: `Float`, `Int`, `Bool`, `Exec`
  (`Graph.cs:14-31`). Every string a node needs — `field=`, `class=`, `var=`, `name=`, `mesh=`,
  `material=` — arrives as a NODE-line key=value attribute, never a pin, and never as data an
  upstream node computes.
- **No array/collection pin.** `ForEach` is a counted-repeat, not a per-element iterator — see
  [Flow](#flow).
- **No boolean combinator and no relational operator besides strict `>`.** See [Logic](#logic).
- **No cross-entity event dispatch.** A graph can *receive* `OnHit` (or any on-demand event a caller
  fires), but there is no node here that lets one entity's graph fire an event on a *different*
  entity's graph — `Fire()` is something a host calls into a specific `GraphHost`, not something a
  node can invoke.
- **No destroy/despawn node.** `Spawn` creates an entity; nothing in this catalog removes one.
- **No HUD or 2D-drawing node.** Nothing here reaches `Aver.UI`.
