# Aver Node — Node Reference

Every node type the graph system knows, as of `41d6566`. This is the parser's own vocabulary:
each entry below is one `case` in `OcGraphParser.AddDefaultPins`
(`scripting/csharp/Aver.Graph/OcGraphParser.cs:606-1054`), which is the ground truth for a node's
default pin shape — not the editor palette, not this document's prose, not an older doc. Where the
editor's own catalog (`sandbox/src/GraphNodeDefs.hpp`, `buildCatalog()` at lines 101-330) and the
parser might disagree, **the parser wins**; see [Parser vs. editor catalog](#parser-vs-editor-catalog)
for how that was checked.

Compile-path behaviour is read from `scripting/csharp/Aver.Graph/GraphCompiler.cs` — specifically
the `IsExecOnlyNodeType` / `IsExecCapable*` predicate family (lines 1686–1812, 2189–2200) and
`EmitPullOutput`'s side-effect refusal (lines 2221–2230).

This page is about node *shapes* — what pins a `NODE` of a given type gets and what it does. For the
class model, `PARAM`/`VAR`, and entry points, see **[the guide](VISUAL_SCRIPTING.md)**; for the
`.ocgraph` file grammar itself (every record's syntax, and where the C++ and C# readers disagree),
see **[`formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md)**.

**37 node types.** `git log -p` on `OcGraphParser.cs` across its full history shows no `case`
label for a node type ever removed — every commit that touched this file added to the vocabulary,
never subtracted from it.

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
10. [Var](#var)
11. [Aliases invisible to the palette](#aliases-invisible-to-the-palette)
12. [Parser vs. editor catalog](#parser-vs-editor-catalog)
13. [What a node cannot do](#what-a-node-cannot-do)

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
- Pulling `Raycast` the same way fails too, but with a generic `NotSupportedException` — `Raycast`
  has no case in `EmitPullOutput` at all, deliberately, so it falls to the `default` arm
  (`GraphCompiler.cs:2333-2360`). The outcome is the same ("wire it into the exec chain"); the error
  text does not say so.

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

**`OnHit` is a demonstrated *mechanism*, not a wired collision hook.** Grepping the engine outside
the graph/scripting layer and its own tests turns up no caller anywhere that invokes
`GraphHost.Fire("OnHit", …)` — no physics or collision system raises it today. A graph can declare
an `OnHit` entry and a test harness can call `Fire("OnHit", …)` directly
(`Aver.Graph.Tests/OnHitAdversarialTests.cs`), but nothing in the shipped engine connects a real
collision to that call yet.

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
(`GraphNodeDefs.hpp:20-27`, its own header comment). Every one of the 36 canonical node types' pin
shape — name, type, direction, declared order — was hand-checked against `AddDefaultPins` for this
document: **zero disagreements**, including the least obvious shapes (`Raycast`'s 14 pins,
`CharacterMove`'s 9, `SetFieldVec3`'s 5-pin order).

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
