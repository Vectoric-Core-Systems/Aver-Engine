# Aver Materials — node-graph shading

A surface can now be shaded two ways. The older one is a `.ocmat` file: a fixed `PARAM` block
(`baseColorFactor`, `metallicFactor`, …) plus up to five texture maps, authored either by hand or
generated from a `[AverMaterial]` C# class (`docs/EDITOR.md` Phase 9). The newer one is a **material
graph**: a `.ocgraph` carrying `DOMAIN material`, wired in the same node editor Aver Node uses for
gameplay, that computes some or all of a surface's inputs per pixel instead of taking them as
constants. A `.ocmat` names one with `GRAPHREF <path>`, and only the fields the graph actually
drives override that material's own factors and maps — the two are layered, not exclusive.

This document is about the graph half: what it is, how `GRAPHREF` connects it to a draw, the node
vocabulary, the preview, and — as plainly as the rest of this project's docs try to say it — what a
material graph cannot do yet. For the gameplay half of `.ocgraph` (the class model, `PARAM`/`VAR`,
`CLASS`/`ENTRY`), see **[`VISUAL_SCRIPTING.md`](VISUAL_SCRIPTING.md)** — the two share a text format
and nothing else. For the exact grammar of `DOMAIN` and `GRAPHREF`, see
**[`formats/FORMAT_SPECS.md` §7 and §10a](formats/FORMAT_SPECS.md)**; this page does not repeat
their wording, only what they mean for a material.

---

## 1. What exists

| Piece | Where | Checked by |
| --- | --- | --- |
| The `DOMAIN material` record | `modules/formats/{include,src}/aver/formats/OcGraph.cpp` | round-trip + domain tests, both readers |
| `GRAPHREF` on a `.ocmat` | `modules/formats/{include,src}/aver/formats/OcMat.*` | `MaterialTest` |
| The graph → HLSL compiler | `modules/render.pbr/src/MaterialGraphHlsl.cpp` | `MaterialGraphTest` — 59 `check()` calls (grep-counted; the file previously claimed 105, which is wrong by any reading — 56 node types exist in total, so "one graph per node type" cannot itself reach 105 checks), including 24 node types each compiled through real DXC individually via `kNewNodeCases` and the remaining node types exercised together in one combined `M_Everything` graph, not one-per-type |
| The process-wide registry | `modules/render.pbr/src/MaterialGraphRegistry.cpp` | `MaterialGraphRegistryTest` |
| The `AverAuthored`/`averBuildSurface` split | `modules/render.pbr/src/PbrShaders.cpp` | the stock path's own oracle gates (unchanged by construction — see §6) |
| `GRAPHREF` → `graphId` resolution | `sandbox/src/SandboxApp.cpp` (`resolveMaterialGraph`, `materialForSurface`) | end-to-end fixture, `test-content/MaterialGraph` |
| The editor's sphere preview | `sandbox/src/GraphEditor.cpp` (`drawMaterialViewport`) | — |
| Creating one | Content Browser → new `.ocgraph`, then hand-write (or editor-set) `DOMAIN material` | — |

Fifty-six node types across nine categories (§7), one compiled shader for every graph in the
process (§5), and a sphere in the graph editor that shows exactly what that shader will draw (§8).

---

## 2. A material graph versus a `.ocmat` PARAM block

A `.ocmat`'s `PARAM` block is eight scalars/vectors and five texture slots, authored once and read
as constants at draw time — the same shape every material in this engine has had since before this
feature existed. It cannot compute anything: a `roughnessFactor` is a number, not an expression, and
there is no way to say "roughness gets rougher near the ground" without either baking that into a
texture or writing C++.

A material graph computes. It reads what the renderer already knows about the pixel being shaded —
its UV, its world position and normal, the view direction, the camera and object positions — and
whatever textures the material's own `.ocmat` already declares, and produces some or all of the same
eight fields a `PARAM` block would have set directly: `BaseColor`, `Metallic`, `Roughness`, `Normal`,
`Emissive`, `Occlusion`, `Opacity`, `AlphaCutoff`.

**The relationship is additive, not a replacement.** `GRAPHREF` names a graph on a `.ocmat` that
still has its own complete `PARAM` block and its own maps. Only the `MaterialOutput` inputs the
author actually wired — LINKED to something, or given a literal value — override what the `.ocmat`
would have authored on its own; everything left unwired keeps the material's stock factors and maps
untouched. `MG_Bands.ocgraph` (§9) is the demonstrated case: it drives `BaseColor` and types a
literal into `Roughness`, and leaves metallic, the normal map, emissive, occlusion and opacity alone
entirely — the material using it still has a normal map, it just also has a banded base colour a
`PARAM` block could never produce.

---

## 3. `DOMAIN material`

The full reasoning for the `DOMAIN` record — why it exists, why absent means gameplay while an
unrecognised name means neither, why it round-trips verbatim — is in
**[`VISUAL_SCRIPTING.md` §1](VISUAL_SCRIPTING.md#1-what-aver-node-is)** and is not repeated here. The
one fact this page depends on: `compileMaterialGraph` refuses a graph whose own `DOMAIN` is not
`material`, naming what it found instead —

```
this graph's DOMAIN is 'gameplay (no DOMAIN record)', not 'material' -- nothing here can be
compiled into a shader
```

— so a `GRAPHREF` accidentally pointed at an ordinary gameplay graph fails loudly, with a message
that names the mistake, rather than being silently misread as an empty material.

---

## 4. `GRAPHREF`: how a graph reaches a draw

`GRAPHREF <path>` on a `.ocmat`, content-relative like `COMP mesh=` in `.ocgraph` and `TEX` in
`.ocmat` itself — never with the content directory on the front. The format layer only *records*
it (`fmt::OcMatExtras::graphRef`); nothing in the `.ocmat` reader resolves the path, loads the
graph, or checks that it compiles.

Resolution happens today in `SandboxApp::resolveMaterialGraph`, at the same point
`materialForSurface` already loads every other `.ocmat` — there is no separate engine-level API for
it yet, consistent with how material loading generally works at this stage (`docs/EDITOR.md` Phase 9).
It:

1. Resolves the path against the project's content root.
2. Asks `MaterialGraphRegistry::idOf` whether this exact path has already compiled — two materials
   naming one graph (a stone and a wet stone sharing a pattern) get the same id and the graph is
   read and compiled once, not twice.
3. On a miss, reads the `.ocgraph` and hands it to `MaterialGraphRegistry::add`, which compiles it
   and returns the id to store in the material's `graphId` field.

**A broken graph does not take the material down.** If the file cannot be read, or `add` reports a
compile failure, resolution returns `0` and logs why; `0` is reserved to mean "no graph" and is what
every material authored before this feature holds. The material falls back to shading exactly as its
own `PARAM` block and maps say — an author with a half-wired graph still sees their object, in
roughly the right colour, and can keep placing it while they fix the graph. A graph that never
compiles never enters the registry's dispatch table either (§5), so it cannot affect any *other*
graph's arm of the switch.

**`graphId` is process-local and never written back to a file.** What the `.ocmat` says is a *path*
(`GRAPHREF`); the id is what the registry hands back for that path *in this process*, assigned
starting at 1 and never recycled — even across a `MaterialGraphRegistry::clear()`, because an id may
still be sitting inside a constant block already uploaded to the GPU, and handing it to a different
graph later would shade the wrong object with the wrong surface. A loader fills `MaterialDesc::graphId`
in after parsing; nothing about it is ever saved.

---

## 5. One shader for every graph

`materialGraphHlsl()` does not emit one shader per material. It emits **one function**,
`averEvalMaterial`, that every registered graph shares, dispatched by a `switch` on
`gMaterialGraphId` — a value that rides in the material's existing `AverMaterial` cbuffer (`b2`), in
four bytes that used to be padding, so a graph-shaded draw needs no second constant buffer and no
pipeline variant:

```hlsl
switch (gMaterialGraphId) {
// MG_Bands
case 1: {
    ...
    break;
}
default: break;   // no graph: exactly the stock material
}
```

**Why a switch and not a shader per material.** Every standard material in this engine already
shares one pipeline and differs only by a binding set and an 80-byte constant block
(`pbr::MaterialSystem::Entry`); per-material shaders would have been this engine's first
per-material pipeline variant — a change to the hot draw path, its sorting and its lifetime rules,
bought to render one cube. `gMaterialGraphId` is a value the draw path already carries, so the
generated function costs one arm per graph and nothing else. Three shaders call `averEvalMaterial`
today — `PSMainVoxi`, `PSVoxel` and `PSClusterMain` — and all three see the same generated function,
which is the whole point of it being process-wide (`MaterialGraphRegistry` is a singleton,
`materialGraphs()`) rather than owned by one renderer: there is exactly one right answer to "what
does graph 7 do."

**The define, not a `-D` list.** `AVER_MATERIAL_GRAPH` removes the *stock* `averEvalMaterial` so the
generated one can take its place; it is concatenated between `rhi::sharedShaderPrelude()` and
`pbr::materialShaderPrelude()` rather than passed as a compiler flag, so every consumer that builds
against the Voxi prelude gets it automatically and cannot forget it on one shader and not another.
When no graph has ever compiled, the registry's `hlsl()` is empty, the define is never emitted, and
the compiled shader is byte-for-byte what it always was — every project that predates this feature.

**Pipelines rebuild on revision, pulled rather than pushed.** `MaterialGraphRegistry::revision()`
bumps whenever the generated text actually changes; a renderer checks it in its own `prePass` and
rebuilds its scene pipelines when it has moved. Checked rather than pushed because materials load
when a project opens, which is *after* pipelines are first built — a pull on a number cannot be
forgotten by some future third caller the way a push notification could be.

---

## 6. `AverAuthored` / `averBuildSurface`: what lets a graph be partial

Before this feature, `averEvalMaterial` did two things in one function: sample the maps and apply
the `PARAM` factors, then derive the BRDF terms (`F0`, `F`, `kdAlbedo`, `ndv`, the alpha clip) from
the result. Node-graph materials split that in two:

- **`AverAuthored`** — exactly what a material authors, and nothing else: `baseColor`, `opacity`,
  `metallic`, `roughness`, `normalTS`, `emissive`, `occlusion`, `alphaCutoff`. Eight fields, matching
  `MaterialOutput`'s eight input pins one for one.
- **`averStockAuthored(uv, geoN)`** — fills an `AverAuthored` the way every material always has: the
  five maps, blended by slope, times the `PARAM` factors.
- **`averBuildSurface(v, l, a, uv)`** — derives everything else a surface needs from an
  `AverAuthored`: the perturbed normal, `F0` from albedo and metallic, `F` from `F0` and the half
  vector, `kdAlbedo`, `ndv`, the alpha-mask clip. This is the material **system's** arithmetic, not
  any one material's, and a graph never gets to restate it.

A generated `averEvalMaterial` starts from `a = averStockAuthored(uv, v.N)` — so a graph inherits
this material's own maps and factors as the baseline — then, for each of the eight
`MaterialOutput` inputs, overwrites `a.<field>` **only if that input is driven**, and finally hands
`a` to `averBuildSurface` exactly as the stock path does. A graph that sets nothing but `BaseColor`
still gets the right `F0`, the right energy split and the right alpha clip, because it never
restates a line of the BRDF — it only ever changes what it explicitly touches.

**"Driven" means linked, or a literal typed onto the pin — not merely present.**
`MaterialOutput`'s eight pins carry **no default value** in the palette, on purpose: a `PARAM` block
already has a value for every one of these fields, so a graph's `MaterialOutput` needs a way to say
"leave this one alone" that is not "zero." A pin left both unlinked and untyped is exactly that; a
non-empty literal (or a wire) is unambiguously something an author wrote. A graph whose
`MaterialOutput` has nothing driven at all is refused at compile time — it would produce a material
identical to the stock one, which is a graph doing nothing rather than a graph the author meant to
write.

**Two more refusals, for the same reason as the one above: a silent no-op is worse than an error.**
Two `MaterialOutput` nodes in one graph are refused, naming both — a material describes one surface,
so it has one output. Zero `MaterialOutput` nodes is refused too: without it nothing is connected to
anything the renderer reads.

---

## 7. The node vocabulary

The emitter is **pull, not push**: compilation starts at `MaterialOutput` and walks backward along
links, emitting a node only when something actually reads it. A topological sort over every node
would also emit ones nothing reads — dead-code elimination here is a property of walking backward,
not a separate pass, and an author's half-finished experiment left floating on the canvas costs
nothing and is not an error.

**A type is arity, and only arity.** There is no bool, no int, no texture handle in a material
graph's own values — every value is a float vector of width 1 to 4, and `widen()` is the one place
promotion rules live:

- A **generic** operator (`Add`, `Multiply`, the single-input standard library, …) returns the
  widest of its *linked* inputs — a literal typed into one side does not count toward the width, so
  `Multiply` with `0.5` typed into `b` is a scalar scale of whatever `a` is, not a forced scalar
  multiply.
- A **scalar splats** into a wider type (`1` into a `float3` pin is white).
- A **wider vector truncates** (`.x`, `.xy`, `.xyz` — dropping components is something an author
  writes on purpose every day).
- A **narrower-to-wider promotion beyond a scalar is refused**, naming the node and the fix: a
  `float2` does not become a `float3` by invention, because the compiler would have to guess the
  third component, and it would guess zero — turning a UV wired into a colour into something
  silently blue-free. The message names the `MakeFloat3` node that actually says what the missing
  component should be.

The editor enforces exactly this and no more (`materialPinTypeMatch` in `GraphEditor.cpp`) — a
gameplay graph keeps exact-match wiring, unchanged, because `PinType` there has no vector types at
all.

### By category (56 node types, 9 categories)

| Category | Nodes |
| --- | --- |
| **Const** (4) | `ConstFloat`, `ConstFloat2`, `ConstFloat3`, `ConstFloat4` — a literal rides on the output pin's own default, the same idiom gameplay's `ConstFloat` already uses. |
| **Input** (6) | `UV`, `WorldPosition`, `WorldNormal`, `ViewDirection`, `CameraPosition`, `ObjectPosition` — what the renderer already knows about this pixel or this instance, read-only, no wiring needed. `ObjectPosition` reads row 3 of the instance transform, useful for anything that should vary per-instance (a phase offset, a per-object tint) that a graph has no other way to reach. |
| **Math** (27) | `Add` `Subtract` `Multiply` `Divide` `Min` `Max` `Power` `Modulo` `Lerp` `Clamp` `Smoothstep` `Step` `Remap` `Saturate` `Abs` `Frac` `Floor` `Ceil` `Sign` `Sqrt` `Exp` `Log` `Sin` `Cos` `Tan` `Normalize` `OneMinus` — generic over width per the rules above. `Remap` is emitted as arithmetic rather than a call (HLSL has no intrinsic for it); every other name maps straight to the matching HLSL intrinsic. |
| **Vector** (11) | `Dot` `Length` `Distance` `Cross` `Reflect` `BlendNormals` (reductions and geometry ops — `Dot`/`Length`/`Distance` always return a scalar regardless of input width) plus `MakeFloat2/3/4`, `Split` (the inverse: one statement per output component actually read, so an unread `.z` emits nothing) and `Swizzle`. |
| **UV** (2) | `TilingOffset`, `Rotator` — both read the surface's own UV when their `uv` input is left unwired (see "the one exception" below). |
| **Procedural** (2) | `Noise` (quintic-smoothed value noise), `Checker` — same unwired-UV convention as above. |
| **Texture** (1) | `SampleTexture` — the one sampling node. |
| **Utility** (2) | `Fresnel`, `If` (a branchless select — `lerp`+`step` under the hood, not HLSL's `?:`, because the condition compares scalars while the arms may be any matching width). |
| **Output** (1) | `MaterialOutput` — the one sink; see §6 for its partial-driving contract. |

**`SampleTexture` fetches once, however many of its outputs a graph reads.** It exposes three output
pins — `rgb`, `a`, `rgba` — off one texture read, memoised on the node: a graph reading only albedo
colour and albedo alpha costs one GPU sample, not two. `slot=` names which of the material's own
eight texture slots to read — `basecolor`, `metalrough`, `normal`, `occlusion`, `emissive`, and the
three `layer1*` slots for the slope-blended second layer — the same maps the `.ocmat` already
declares; a graph adds no new texture bindings of its own. An unrecognised `slot=` is a compile-time
error naming the node and listing the eight legal values.

**`Swizzle` is the one node whose output width is an attribute, not a pin type.** `mask=xyz` (or
`rrr`, or any one-to-four combination of `x/y/z/w`/`r/g/b/a`) decides the output arity at compile
time; a mask reading a component the input does not have, or containing a character that is
neither a coordinate nor a colour letter, is refused by name.

**The one exception to "an unwired pin means zero":** `TilingOffset`, `Rotator`, `Noise` and
`Checker` all take a `uv` input, and when it is left unwired they read the surface's own UV instead
of `(0, 0)`. An unwired texture-space node reading a literal zero would sample one texel and look
like a broken texture rather than the obvious default every material editor gives it.

---

## 8. The preview

The graph editor's Viewport tab for a `DOMAIN material` file draws a **sphere**, not the
actor/component tree a gameplay `CLASS` graph gets — a material graph has no `CLASS` and no
components, and the component toolbar's own warning ("no CLASS record, so nothing spawns them") is
actively misleading on a file that is not supposed to have one. A sphere rather than a cube because
it presents every angle between normal and view at once, so a Fresnel term, a roughness value and a
normal map all read on it in one frame.

**It is not an approximation.** The sphere is shaded through the *same* `MaterialGraphRegistry` a
placed `.ocmat`'s `GRAPHREF` resolves through, keyed on the file's own path — the id the preview
gets is the same id a real draw would get, running the same generated `case` arm. A preview that
could drift from the thing it previews would be worse than none.

**A compile error shows above the picture, not instead of it.** If the graph does not compile,
`materialPreviewGraphId_` stays `0` and the panel says so; if it *did* compile at some point and a
later edit broke it, the sphere keeps showing the **last surface that worked** while the message
above it says what to fix — an author needs to see both what they had and what is currently wrong,
not lose the picture the moment they start editing. Recompilation is triggered off the undo-stack
depth rather than per frame or on a dirty flag, since every edit path already pushes undo before
changing anything, so the depth moves exactly when the graph does.

**Built without the PBR module, there is nothing to preview** — the panel says so plainly rather
than showing a blank or stale image.

The camera is published at its own slot (`b4`), the same convention the actor-preview tab uses
(`docs/EDITOR.md` Phase 8) and for the same reason: the backend rebinds the engine's per-frame block on
every `setPipeline`, so a second camera set for a second viewport in the same frame would land
nowhere. Lighting is fixed and does not attempt to match the level.

---

## 9. Worked example: two cubes, one line of difference

`test-content/MaterialGraph` exists to *falsify*, not merely demonstrate. Two cubes on a floor, side
by side, lit by one sun (`Content/Maps/Main.ocmap`):

```
PLACE Meshes/cube.ocmesh 0 -120 60 0 0 0 60 M_PlainCube
PLACE Meshes/cube.ocmesh 0  120 60 0 0 0 60 M_GraphCube
```

`M_PlainCube.ocmat` and `M_GraphCube.ocmat` are identical `PARAM` blocks — same base colour, same
metallic, same roughness — differing by exactly one added line:

```
GRAPHREF Materials/MG_Bands.ocgraph
```

`Content/Materials/MG_Bands.ocgraph` — quoted here verbatim, its own header comments explaining the
choices in more depth than this page repeats:

```
OCGRAPH 1
DOMAIN material
NAME MG_Bands

NODE wp WorldPosition -760 -60
PIN wp xyz out float3

NODE rate ConstFloat3 -760 160
PIN rate value out float3 0.02,0.02,0.02

NODE scaled Multiply -420 30
PIN scaled a in float3
PIN scaled b in float3
PIN scaled result out float3

NODE bands Frac -140 30
PIN bands x in float3
PIN bands result out float3

NODE out MaterialOutput 160 -40
PIN out BaseColor in float3
PIN out Roughness in float 0.25

LINK wp.xyz scaled.a
LINK rate.value scaled.b
LINK scaled.result bands.x
LINK bands.result out.BaseColor
```

`frac(worldPosition * 0.02)` bands the surface every 50 cm along each axis — a gradient, not a flat
colour, because a flat colour is exactly what `baseColorFactor` already produces on its own, and a
picture of one could never distinguish a working graph from a graph that silently did nothing.
`Roughness` is a **typed literal**, not a wire — exercising the other half of §6's "driven" rule —
and `Metallic`, `Normal`, `Emissive`, `Occlusion` and `Opacity` are left untouched, so the cube keeps
this material's own values for all five. **Falsified, not just observed:** commenting out the one
`GRAPHREF` line makes the two cubes render identically; nothing else about the two placements
differs.

This pair is what `MaterialGraphTest` and `MaterialGraphRegistryTest` check mechanically — the
former by compiling node types through real DXC (59 `check()` calls total in the file — see §1's
correction; not 105), the latter by asserting ids are never recycled across a `clear()`, since an id
may still be sitting in an uploaded constant block.

---

## 10. What it does not do

Stated plainly, the way an honest limitation should be, rather than left for a reader to discover:

- **No vertex or displacement graphs.** Every value a material graph reads about geometry —
  `WorldPosition`, `WorldNormal`, `ViewDirection` — is already-computed *per-pixel* vertex output;
  nothing a graph produces feeds back into the vertex stage. `MaterialOutput`'s eight inputs are all
  pixel-stage surface properties (§6); there is no ninth pin for a vertex offset, and nothing in the
  compiler touches a vertex shader at all.
- **No per-graph dynamic constant buffer, and no exposed instance parameters.** `gMaterialGraphId`
  is four bytes that used to be padding inside the material's existing `AverMaterial` block (§5) —
  a graph gets no cbuffer of its own. There is no mechanism for a graph to declare a new authored
  numeric knob with its own per-material storage (the way, say, an Unreal material-instance
  parameter would); a graph's only inputs are the fixed renderer-provided values (§7 Input category),
  this material's own existing textures, and literals baked directly into the graph's own generated
  HLSL text. Two materials sharing one `GRAPHREF` are pixel-for-pixel identical in what the graph
  contributes; they can only differ through their own separate `PARAM`/map values for fields the
  graph leaves undriven.
- **No verified Vulkan parity.** `MaterialGraphTest` loads `dxcompiler.dll` directly and compiles
  against DXIL only — it never asks for `-spirv`, and no test anywhere compiles or runs a material
  graph through the Vulkan backend. Nothing about the emitted HLSL text is Vulkan-*specific*, but
  "should probably work" is not the same claim as "verified," and this page makes only the second
  kind.
- **No `Time` node, and no `VertexColor` node.** Both were in the original plan and both were cut for
  the same reason: this engine genuinely cannot supply them yet. There is no time uniform anywhere
  in the shared shader prelude, and `VSOut` carries no vertex colour — either would have been a
  palette entry whose only possible outcome is a compile error naming a symbol that does not exist.
  `Panner`, which needs a time input to be worth having, was cut alongside `Time` for the same
  reason.
- **Every graph in the process shares one compiled shader** (§5) — there is no per-graph shader
  variant, no per-graph optimisation pass, and no way for one graph's compile to diverge from what
  every other registered graph's `case` arm looks like. This is a deliberate trade (§5), not an
  oversight, but it means a future feature that wants a material-specific pipeline state (its own
  blend mode chosen by the graph, say) has nowhere to put that today.
- **`GRAPHREF` resolves through the sandbox, not through a standalone engine API.**
  `SandboxApp::resolveMaterialGraph` is where a path becomes a registered graph and a `graphId`;
  there is no separate load-time entry point outside the editor's own project-loading code today,
  consistent with how the rest of `.ocmat` loading works at this stage (`docs/EDITOR.md` Phase 9) but
  worth stating rather than assuming.
