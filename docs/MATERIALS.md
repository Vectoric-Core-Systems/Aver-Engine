# Aver Materials — node-graph shading

A surface can be shaded two ways. The older one is a `.ocmat` file: a fixed `PARAM` block (`baseColorFactor`, `metallicFactor`, …) plus up to eight texture maps (five base slots, plus three for the slope-blended second layer), authored either by hand or generated from a `[AverMaterial]` C# class (`docs/EDITOR.md` Phase 9). The newer one is a **material graph**: a `.ocgraph` carrying `DOMAIN material`, wired in the same node editor Aver Node uses for gameplay, that computes some or all of a surface's inputs per pixel. A `.ocmat` names one with `GRAPHREF <path>`, and only the fields the graph actually drives override that material's own factors and maps — the two are layered, not exclusive.

This document is about the graph half: what it is, how `GRAPHREF` connects it to a draw, the node vocabulary, the preview, and what a material graph cannot do yet. For the gameplay half of `.ocgraph` (the class model, `PARAM`/`VAR`, `CLASS`/`ENTRY`), see **[`VISUAL_SCRIPTING.md`](VISUAL_SCRIPTING.md)**. For the exact grammar of `DOMAIN` and `GRAPHREF`, see **[`formats/FORMAT_SPECS.md` §7 and §10a](formats/FORMAT_SPECS.md)**.

---

## 1. What exists

| Piece | Where | Checked by |
| --- | --- | --- |
| The `DOMAIN material` record | `modules/formats/{include,src}/aver/formats/OcGraph.cpp` | round-trip + domain tests, both readers |
| `GRAPHREF` on a `.ocmat` | `modules/formats/{include,src}/aver/formats/OcMat.*` | `MaterialTest` |
| The graph → HLSL compiler | `modules/render.pbr/src/MaterialGraphHlsl.cpp` | `MaterialGraphTest` — 26 node types each compiled through real DXC individually via `kNewNodeCases`, plus one extra `Panner` case with `time` wired, plus the remaining node types exercised together in one combined `M_Everything` graph, not one-per-type |
| The process-wide registry | `modules/render.pbr/src/MaterialGraphRegistry.cpp` | `MaterialGraphRegistryTest` |
| The `AverAuthored`/`averBuildSurface` split | `modules/render.pbr/src/PbrShaders.cpp` | the stock path's own oracle gates (unchanged by construction — see §6) |
| `GRAPHREF` → `graphId` resolution | `Runtime/src/GameContent.cpp` (`resolveMaterialGraph`, `materialForSurface`; ported from `SandboxApp`, which no longer holds it) | end-to-end fixture, `test-content/MaterialGraph` |
| The editor's sphere preview | `sandbox/src/GraphEditor.cpp` (`drawMaterialViewport`) | — |
| Creating one | Content Browser → new `.ocgraph`, then hand-write (or editor-set) `DOMAIN material` | — |

Fifty-eight node types across nine categories (§7), one compiled shader for every graph in the process (§5), and a sphere in the graph editor showing what that shader will draw (§8).

---

## 2. A material graph versus a `.ocmat` PARAM block

A `.ocmat`'s `PARAM` block is a growing set of scalars/vectors (`pbr::MaterialDesc` — well past the original eight now that subsurface, dielectric/transmission, volume attenuation and the coat have each added their own) and up to eight texture slots, authored once and read as constants at draw time. It cannot compute anything: a `roughnessFactor` is a number, not an expression, and there is no way to say "roughness gets rougher near the ground" without either baking that into a texture or writing C++.

A material graph computes. It reads what the renderer already knows about the pixel being shaded — its UV, world position and normal, view direction, camera and object positions, the engine clock (`Time`) — and whatever textures the material's own `.ocmat` already declares, and produces some or all of these fields: `BaseColor`, `Metallic`, `Roughness`, `Normal`, `Emissive`, `Occlusion`, `Opacity`, `AlphaCutoff`, `SubsurfaceWeight`, `SubsurfaceRadius`, `SubsurfaceColor`, `Ior`, `Transmission`, `AttenuationColor`, `AttenuationDistance`, `CoatWeight`, `CoatRoughness`, `CoatF0` — eighteen fields (`kOutputFields`, `modules/render.pbr/src/MaterialGraphHlsl.cpp`; eight when this page was first written).

**The relationship is additive, not a replacement.** `GRAPHREF` names a graph on a `.ocmat` that still has its own complete `PARAM` block and maps. Only the `MaterialOutput` inputs the author actually wired — linked or given a literal value — override what the `.ocmat` would have authored; everything unwired keeps the material's stock values. `MG_Bands.ocgraph` (§9) drives `BaseColor` and types a literal `Roughness`, leaving metallic, normal, emissive, occlusion and opacity untouched — the material still has a normal map, just also a banded base colour.

**`Emissive` is linear outgoing radiance, not a "strength" applied to something else.** `EmissiveFactor` is multiplied by the emissive map when bound (`a.emissive = gEmissiveFactor * map.emissive`, `material_prelude.hlsl:487`) and used alone, at full weight, when none is — the unbound map's identity is white (1, 1, 1), the same convention `BaseColor`/`Occlusion` use, not black. Values above 1 are expected for light sources: a sunlit white wall sits around 1, a lamp bulb that should actually bloom and light the room it is in wants 10–20, and the Material Editor's control goes to 32. `Emissive` alone, times the map when there is one, *is* the outgoing radiance.

**`subsurfaceWeight`/`subsurfaceRadius`/`subsurfaceColor` make a thin or fleshy surface read as lit from within.** `subsurfaceWeight` (`PARAM subsurfaceWeight`) is the [0,1] strength of three terms computed per pixel with no extra pass and no extra ray (`material_prelude.hlsl`'s `averDirectTerms`/`averSubsurfaceAmbient`): a **wrap** that lets light reach past the terminator, tinted by `subsurfaceColor`; a **transmission** term for light arriving on the surface's far side and diffusing through (a leaf, curtain or ear glowing with the sun behind it), whose shadow is deliberately asked from the *light-facing* side, pushed `subsurfaceRadius`'s depth through the surface, so a sheet thinner than that depth transmits and a body thicker than it (a head, a statue) does not; and an **ambient transmission** share from sky and bounce, so thin foliage in shade reads lighter than an opaque leaf would. `subsurfaceRadius` [0,1] doubles as scatter depth (0.25–5 cm) and the width of the forward-scatter lobe. `subsurfaceColor` (`PARAM subsurfaceColor`, sRGB, authored like `baseColorFactor`) is the colour light takes travelling through the material — white (the default) keeps scattering in the surface's own colour; skin wants deep red, leaves yellow-green, wax orange. **Not a BSSRDF:** no screen-space blur or lateral transport — what this buys is what reads as translucency.

**`lightIntensity` is a separate knob.** `Emissive` is what a surface looks like; `lightIntensity` (`pbr::MaterialDesc::lightIntensity`, `PARAM lightIntensity` in `.ocmat`) is whether it casts light the way the sun does. 0 (default) leaves a material exactly as the paragraph above describes — bright to look at, dark to everything around it. A positive value is a **multiplier** on the light the material's glow and size already, physically, cast at 1 metre in the sun's units (`SkyAtmosphere::sunIntensity`): `1` lights exactly what `EmissiveFactor` and the draw's world bounding sphere (`Draw::boundsCentre`/`boundsRadius`) cast, `2` lights twice that — the glow is treated as a Lambertian sphere's radiance, the size as that sphere's radius (`VoxiRenderer::buildLocalLights` does the math). Every draw using the material becomes a small sphere light, that same bounding sphere coloured by `EmissiveFactor` (normalised to max component 1; white if left at zero), that the staged ray-driven local-light pass (`CSRdLocalLights`, D3D12 only, `Settings::rayDrivenStages` 1 or 2) sums, shadows with one traced ray, and adds as direct diffuse light. **It is read nowhere else:** raster, the single-pass megakernel (`AVER_RD_SINGLE_PASS`) and `PSMainVoxi`'s blended reuse all shade the material by its `Emissive` factor alone, so a lamp shows the same glow everywhere and only lights the room it sits in where ray-driven local lights are actually running. A lamp's own glass shade (a `BLEND translucent` bulb around the filament) does not block its own light — not because the pass tests for the material, but because translucent draws are not in the ray-traced scene at all (§10's blended-draw limitation applies), so the light has nothing of the bulb's geometry to be shadowed by. An emissive surface with `lightIntensity` zero still lights surroundings only through GI — voxel GI and ReSTIR GI pick it up like any other radiance source — because there are still no point lights: `CLight` (`docs/SCENE_FRAMEWORK.md` §3.7) is authored data no renderer currently reads.

A lamp whose bounding sphere is bigger than its visible emitter over-lights at `1`, since the sphere it is treated as is larger than the glow it shows — for example a whole fixture carrying the light material rather than just the bulb; give it a lower multiplier or split the bulb into its own mesh. Content authored while `lightIntensity` meant an absolute brightness (value `2` meaning "twice the sun") reads much dimmer under this multiplier meaning; `1` is the physically matched starting point to re-author from, not a like-for-like replacement for the old numbers.

---

## 3. `DOMAIN material`

The full reasoning for the `DOMAIN` record — why it exists, why absent means gameplay while an unrecognised name means neither, why it round-trips verbatim — is in **[`VISUAL_SCRIPTING.md` §1](VISUAL_SCRIPTING.md#1-what-aver-node-is)**. The one fact this page depends on: `compileMaterialGraph` refuses a graph whose own `DOMAIN` is not `material`, naming what it found instead:

```
this graph's DOMAIN is 'gameplay (no DOMAIN record)', not 'material' -- nothing here can be
compiled into a shader
```

A `GRAPHREF` accidentally pointed at a gameplay graph fails loudly with a message naming the mistake, rather than being silently misread.

---

## 4. `GRAPHREF`: how a graph reaches a draw

`GRAPHREF <path>` on a `.ocmat`, content-relative like other paths in `.ocgraph` and `.ocmat` — never with the content directory on the front. The format layer only *records* it (`fmt::OcMatExtras::graphRef`); nothing in the `.ocmat` reader resolves the path, loads the graph, or checks that it compiles.

Resolution happens in `GameContent::resolveMaterialGraph` (`Runtime/src/GameContent.cpp:966`; originally `SandboxApp::resolveMaterialGraph`, which no longer exists), at the same point `GameContent::materialForSurface` already loads every other `.ocmat` — there is no separate engine-level API for it yet, consistent with how material loading generally works at this stage (`docs/EDITOR.md` Phase 9). `AnimEditor.cpp` (~:509-560) resolves a `GRAPHREF` on its own for its preview. It:

1. Resolves the path against the project's content root.
2. Asks `MaterialGraphRegistry::idOf` whether this exact path has already compiled — two materials naming one graph (a stone and a wet stone sharing a pattern) get the same id and the graph is read and compiled once, not twice.
3. On a miss, reads the `.ocgraph` and hands it to `MaterialGraphRegistry::add`, which compiles it and returns the id.

**A broken graph does not take the material down.** If the file cannot be read, or `add` reports a compile failure, resolution returns `0` and logs why; `0` is reserved to mean "no graph" and is what every material authored before this feature holds. The material falls back to shading exactly as its own `PARAM` block and maps say — an author with a half-wired graph still sees their object, in roughly the right colour, and can keep placing it while they fix the graph. A graph that never compiles never enters the registry's dispatch table (§5), so it cannot affect any other graph's arm of the switch.

**`graphId` is process-local and never written back to a file.** What the `.ocmat` says is a *path* (`GRAPHREF`); the id is what the registry hands back for that path *in this process*, assigned starting at 1 and never recycled — even across a `MaterialGraphRegistry::clear()`, because an id may still be sitting inside a constant block already uploaded to the GPU, and handing it to a different graph later would shade the wrong object with the wrong surface. A loader fills `MaterialDesc::graphId` after parsing; nothing about it is ever saved.

---

## 5. One shader for every graph

`materialGraphHlsl()` emits **one function**, `averEvalMaterial`, that every registered graph shares, dispatched by a `switch` on `gMaterialGraphId` — a value that rides in the material's existing `AverMaterial` cbuffer (`b2`), in four bytes that used to be padding:

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

**Why a switch and not a shader per material.** Every standard material already shares one pipeline and differs only by a binding set and an 80-byte constant block (`pbr::MaterialSystem::Entry`); per-material shaders would have been this engine's first per-material pipeline variant, bought to render one cube. `gMaterialGraphId` is a value the draw path already carries, so the generated function costs one arm per graph and nothing else — per-material shaders would also have changed the hot draw path, its sorting and its lifetime rules. Three shaders call `averEvalMaterial` today — `PSMainVoxi`, `PSVoxel` and `PSClusterMain` — and all three see the same generated function, which is the whole point of it being process-wide (`MaterialGraphRegistry` is a singleton, `materialGraphs()`) rather than owned by one renderer: there is exactly one right answer to "what does graph 7 do."

**The define, not a `-D` list.** `AVER_MATERIAL_GRAPH` removes the *stock* `averEvalMaterial` so the generated one can take its place; it is concatenated between `rhi::sharedShaderPrelude()` and `pbr::materialShaderPrelude()`, so every consumer that builds against the Voxi prelude gets it automatically and cannot forget it on one shader and not another. When no graph has compiled, the registry's `hlsl()` is empty, the define is never emitted, and the compiled shader is byte-for-byte what it always was — every project that predates this feature.

**Pipelines rebuild on revision, pulled rather than pushed.** `MaterialGraphRegistry::revision()` bumps whenever the generated text changes; a renderer checks it in its own `prePass` and rebuilds its pipelines when it has moved. Pulled rather than pushed because materials load when a project opens, which is *after* pipelines are first built — a pull on a number cannot be forgotten by some future third caller the way a push notification could be.

---

## 6. `AverAuthored` / `averBuildSurface`: what lets a graph be partial

Before this feature, `averEvalMaterial` did two things: sample the maps and apply the `PARAM` factors, then derive BRDF terms from the result. Node-graph materials split that in two:

- **`AverAuthored`** — exactly what a material authors: `baseColor`, `opacity`, `metallic`, `roughness`, `normalTS`, `emissive`, `occlusion`, `alphaCutoff`, `subsurfaceWeight`, `subsurfaceRadius`, `subsurfaceColor`, `ior`, `transmission`, `attenuationColor`, `attenuationDistance`, `coatWeight`, `coatRoughness`, `coatF0` (`modules/render.pbr/shaders/material_prelude.hlsl`). Eighteen fields (eight originally, before subsurface, the dielectric pair, volume attenuation and the coat triple each added their own) matching `MaterialOutput`'s eighteen input pins one for one.
- **`averStockAuthored(uv, geoN)`** — fills an `AverAuthored` the way every material always has: the five maps, blended by slope, times the `PARAM` factors.
- **`averBuildSurface(v, l, a, uv)`** — derives everything else a surface needs from an `AverAuthored`: the perturbed normal, `F0` from albedo and metallic, `F` from `F0` and the half vector, `kdAlbedo`, `ndv`, the alpha-mask clip. This is the material **system's** arithmetic, not any one material's, and a graph never restates it.

A generated `averEvalMaterial` starts from `a = averStockAuthored(uv, v.N)` — so a graph inherits this material's own maps and factors as the baseline — then, for each of the eighteen `MaterialOutput` inputs, overwrites `a.<field>` **only if that input is driven**, and finally hands `a` to `averBuildSurface` exactly as the stock path does. A graph that sets nothing but `BaseColor` still gets the right `F0`, the right energy split and the right alpha clip, because it never restates a line of the BRDF — it only changes what it explicitly touches.

**"Driven" means linked, or a literal typed onto the pin — not merely present.** `MaterialOutput`'s eighteen pins carry **no default value** by purpose: a `PARAM` block already has a value for every field, so a graph's `MaterialOutput` needs a way to say "leave this one alone" that is not "zero." A pin left both unlinked and untyped is exactly that; a non-empty literal (or a wire) is unambiguously something an author wrote. A graph whose `MaterialOutput` has nothing driven is refused at compile time — it would produce a material identical to the stock one, a graph doing nothing rather than one the author meant to write. **Two more refusals, same reason: a silent no-op is worse than an error.** Two `MaterialOutput` nodes in one graph are refused too, naming both. Zero `MaterialOutput` nodes is refused: without it nothing is connected to what the renderer reads.

---

## 7. The node vocabulary

The emitter is **pull, not push**: compilation starts at `MaterialOutput` and walks backward along links, emitting a node only when something actually reads it. Dead-code elimination is a property of walking backward, not a separate pass: a topological sort over every node would also emit ones nothing reads, whereas an author's half-finished experiment left floating on the canvas costs nothing and is not an error.

**A type is arity, and only arity.** There is no bool, int or texture handle — every value is a float vector of width 1 to 4. `widen()` is the one place promotion rules live:

- A **generic** operator (`Add`, `Multiply`, the single-input standard library) returns the widest of its *linked* inputs — a literal typed into one side does not count toward the width, so `Multiply` with `0.5` typed into `b` is a scalar scale of whatever `a` is, not a forced scalar multiply.
- A **scalar splats** into a wider type (`1` into a `float3` pin is white).
- A **wider vector truncates** (`.x`, `.xy`, `.xyz` — dropping components is something an author writes on purpose every day).
- A **narrower-to-wider promotion beyond a scalar is refused**, naming the node: a `float2` does not become a `float3` by invention, because the compiler would have to guess the third component, and it would guess zero — turning a UV wired into a colour into something silently blue-free. The message names the `MakeFloat3` node that actually says what the missing component should be.

The editor enforces exactly this and no more (`materialPinTypeMatch` in `GraphEditor.cpp`) — a gameplay graph keeps exact-match wiring, unchanged, because `PinType` there has no vector types at all.

### By category (58 node types, 9 categories)

| Category | Nodes |
| --- | --- |
| **Const** (4) | `ConstFloat`, `ConstFloat2`, `ConstFloat3`, `ConstFloat4` — a literal rides on the output pin's own default, the same idiom gameplay's `ConstFloat` already uses. |
| **Input** (6) | `UV`, `WorldPosition`, `WorldNormal`, `ViewDirection`, `CameraPosition`, `ObjectPosition` — what the renderer already knows about this pixel or this instance, read-only, no wiring needed. `ObjectPosition` reads row 3 of the instance transform, useful for anything that should vary per-instance (a phase offset, a per-object tint) that a graph has no other way to reach. |
| **Math** (27) | `Add` `Subtract` `Multiply` `Divide` `Min` `Max` `Power` `Modulo` `Lerp` `Clamp` `Smoothstep` `Step` `Remap` `Saturate` `Abs` `Frac` `Floor` `Ceil` `Sign` `Sqrt` `Exp` `Log` `Sin` `Cos` `Tan` `Normalize` `OneMinus` — generic over width. `Remap` is emitted as arithmetic rather than a call (HLSL has no intrinsic for it); every other name maps straight to the matching HLSL intrinsic. |
| **Vector** (11) | `Dot` `Length` `Distance` `Cross` `Reflect` `BlendNormals` (reductions and geometry ops — `Dot`/`Length`/`Distance` always return a scalar regardless of input width) plus `MakeFloat2/3/4`, `Split` (the inverse: one statement per output component actually read, so an unread `.z` emits nothing) and `Swizzle`. |
| **UV** (3) | `TilingOffset`, `Panner`, `Rotator` — all read the surface's own UV when their `uv` input is left unwired (see "the one exception" below). `Panner` is `uv + speed * time` (Unreal's node of that name): `speed` is a `float2` defaulting to `(0, 0)`, and an unwired `time` reads `Time`'s wrapped seconds, so setting `speed` alone scrolls the UV. Wire a `Time.raw`, or any other value, into `time` to replace the clock. |
| **Procedural** (2) | `Noise` (quintic-smoothed value noise), `Checker` — same unwired-UV convention. |
| **Texture** (1) | `SampleTexture` — the one sampling node. |
| **Utility** (3) | `Fresnel`, `If` (a branchless select — `lerp`+`step` under the hood, not HLSL's `?:`, because the condition compares scalars while the arms may be any matching width), and `Time`. |
| **Output** (1) | `MaterialOutput` — the one sink; see §6 for its partial-driving contract. |

**`SampleTexture` fetches once, however many outputs a graph reads.** It exposes `rgb`, `a`, `rgba` off one texture read, memoised on the node: a graph reading only albedo colour and albedo alpha costs one GPU sample, not two. `slot=` names which of the material's own eight texture slots to read — `basecolor`, `metalrough`, `normal`, `occlusion`, `emissive`, `layer1basecolor`, `layer1metalrough`, `layer1normal` — the same maps the `.ocmat` already declares; a graph adds no new texture bindings of its own. An unrecognised `slot=` is a compile-time error naming the node and listing the legal values.

**`Swizzle` is the one node whose output width is an attribute, not a pin type.** `mask=xyz` (or `rrr`, or any one-to-four combination of `x/y/z/w`/`r/g/b/a`) decides the output arity; a mask reading a component the input does not have, or containing an invalid character, is refused.

**`Time` is the engine clock, three outputs.** `time` is seconds wrapped at an hour (the one for `Sin`, scrolling, pulsing, because `sin()` of an ever-growing float loses its period). `raw` is unwrapped monotonic seconds, for the rare graph that accepts the precision decay. `delta` is the last frame's length. All three read the engine's per-frame block (`b0`), which `Engine::frameStep` feeds the clock into once per frame (`IDevice::setFrameTime`, `Engine.cpp:254`), so every graph evaluated in a frame sees the same instant. There is deliberately no `scale` input: an unwired one would multiply the clock by zero with no error, so put a `Multiply` beside the node instead. A `Panner` scrolls at `speed` UV units per second; its wrapped clock makes it hop once an hour unless `speed * 3600` is a whole number of tiles.

**The one exception to "an unwired pin means zero":** `TilingOffset`, `Panner`, `Rotator`, `Noise` and `Checker` all take a `uv` input. Unwired, they read the surface's own UV instead of `(0, 0)` — unwired would sample one texel and look like a broken texture. `Panner`'s `time` is the one other such pin: unwired, it reads `Time`'s wrapped seconds, because a zero would multiply `speed` away. A number typed onto either pin is not read; wire a `ConstFloat` for a fixed value.

---

## 8. The preview

The graph editor's Viewport tab for a `DOMAIN material` file draws a **sphere**, not the actor/component tree a gameplay `CLASS` graph gets — a material graph has no `CLASS` and no components, and the component toolbar's own warning ("no CLASS record, so nothing spawns them") would be actively misleading on a file that is not supposed to have one. A sphere rather than a cube presents every angle between normal and view at once, so a Fresnel term, a roughness value and a normal map all read on it in one frame.

**It is not an approximation.** The sphere is shaded through the *same* `MaterialGraphRegistry` a placed `.ocmat`'s `GRAPHREF` resolves through, keyed on the file's own path — the id the preview gets is the same id a real draw would get, running the same generated `case` arm. A preview that could drift from the thing it previews would be worse than none.

**A compile error shows above the picture, not instead of it.** If the graph does not compile, `materialPreviewGraphId_` stays `0` and the panel says so; if it *did* compile and a later edit broke it, the sphere keeps showing the **last surface that worked** while the message above says what to fix — an author needs to see both what they had and what is currently wrong. Recompilation is triggered off the undo-stack depth rather than per frame or on a dirty flag, since every edit path already pushes undo before changing anything, so the depth moves exactly when the graph does.

**Built without the PBR module, there is nothing to preview** — the panel says so plainly.

The camera is published at its own slot (`b4`), the same convention the actor-preview tab uses (`docs/EDITOR.md` Phase 8) and for the same reason: the backend rebinds the engine's per-frame block on every `setPipeline`, so a second camera set for a second viewport in the same frame would land nowhere. Lighting is fixed and does not attempt to match the level.

---

## 9. Worked example: two cubes, one line of difference

`test-content/MaterialGraph` exists to *falsify*, not merely demonstrate. Two cubes on a floor, side by side, lit by one sun (`Content/Maps/Main.ocmap`):

```
PLACE Meshes/cube.ocmesh 0 -120 60 0 0 0 60 M_PlainCube
PLACE Meshes/cube.ocmesh 0  120 60 0 0 0 60 M_GraphCube
```

`M_PlainCube.ocmat` and `M_GraphCube.ocmat` are identical `PARAM` blocks — same base colour, same metallic, same roughness — differing by exactly one added line:

```
GRAPHREF Materials/MG_Bands.ocgraph
```

`Content/Materials/MG_Bands.ocgraph` — quoted here verbatim, its own header comments explaining the choices in more depth than this page repeats:

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

`frac(worldPosition * 0.02)` bands the surface every 50 cm along each axis — a gradient, not a flat colour, because a flat colour is exactly what `baseColorFactor` already produces on its own, and a picture of one could never distinguish a working graph from a graph that silently did nothing. `Roughness` is a **typed literal**, exercising the other half of §6's "driven" rule, and `Metallic`, `Normal`, `Emissive`, `Occlusion` and `Opacity` are left untouched, so the cube keeps this material's own values for all five. **Falsified, not just observed:** commenting out the one `GRAPHREF` line makes the two cubes render identically; nothing else about the two placements differs.

`MaterialGraphTest` and `MaterialGraphRegistryTest` check this mechanically — the former by compiling node types through real DXC, the latter by asserting ids are never recycled across a `clear()`, since an id may still be sitting in an uploaded constant block.

---

## 10. What it does not do

Stated plainly, the way an honest limitation should be, rather than left for a reader to discover:

- **No vertex or displacement graphs.** Every value a material graph reads about geometry is already-computed *per-pixel* vertex output; nothing a graph produces feeds back into the vertex stage. `MaterialOutput`'s eighteen inputs are all pixel-stage surface properties (§6); none is a vertex offset, and nothing in the compiler touches a vertex shader at all.
- **No per-graph dynamic constant buffer, and no exposed instance parameters.** `gMaterialGraphId` is four bytes inside the material's existing `AverMaterial` block (§5) — a graph gets no cbuffer of its own. There is no mechanism for a graph to declare a new authored numeric knob with its own per-material storage (the way an Unreal material-instance parameter would); a graph's only inputs are the fixed renderer-provided values (§7 Input category), the `Time` clock, this material's own existing textures, and literals baked directly into the graph's own generated HLSL text. Two materials sharing one `GRAPHREF` are pixel-for-pixel identical in what the graph contributes; they can only differ through their own `PARAM`/map values for fields the graph leaves undriven.
- **No verified Vulkan parity.** `MaterialGraphTest` loads `dxcompiler.dll` directly and compiles against DXIL only — it never asks for `-spirv`, and no test anywhere compiles or runs a material graph through the Vulkan backend. Nothing about the emitted HLSL text is Vulkan-*specific*, but "should probably work" is not the same claim as "verified," and this page makes only the second kind.
- **`WaveNormal` has no palette entry.** It is emitted (`ciEquals(ty, "WaveNormal")` in `MaterialGraphHlsl.cpp`, for the water surface's own ripple set — `position`/`height`/`focus`) and `MaterialGraphTest` never names it, so the only way to use it is to hand-author the `.ocgraph` text. `Time` and `Panner` (§7) are in the palette and the test.
- **Graphs run on every path.** Raster calls `averEvalMaterial`; every ray hit (the ray-driven primary, ReSTIR GI candidates, reflections, Path Tracing vertices, and alpha-mask cut-out tests) runs the generated twin `averApplyMaterialGraphRt(graphId, ...)` from voxi_rt.hlsli's `rtHitSurface`, keyed on the hit's own material row. Both edit the same `AverAuthored` that `averComposeSurface` turns into the surface, so `Time` and `Panner` animate on ray-driven frames too. In the twin, `ObjectPosition` is the hit instance's position.
- **No `VertexColor` node.** `VSOut` carries no vertex colour, so a palette entry for it could only produce a compile error naming a symbol that does not exist.
- **Every graph in the process shares one compiled shader** (§5) — there is no per-graph shader variant, optimisation pass, or way for one graph's compile to diverge from what every other graph's `case` arm looks like. This is a deliberate trade (§5), not an oversight, but it means a future feature that wants a material-specific pipeline state (its own blend mode chosen by the graph, say) has nowhere to put that today.
- **`GRAPHREF` resolves through `GameContent`, not through a standalone engine API.** `GameContent::resolveMaterialGraph` (`Runtime/src/GameContent.cpp:966`) is where a path becomes a registered graph and a `graphId`; there is no separate load-time entry point outside the game/editor content-loading code (the Anim editor's preview resolves `GRAPHREF` separately), consistent with how the rest of `.ocmat` loading works at this stage (`docs/EDITOR.md` Phase 9) but worth stating rather than assuming.

## One material, every path (2026-10-04)

A material is evaluated the same way by every renderer. Only how textures are fetched differs per path.

- **Data.** `AverMaterialData` (material_prelude.hlsl) is `pbr::MaterialConstants` field for field. Raster reads it from the `b2` cbuffer (`averMaterialData()`); ray hits read the identical struct from the material table (`RtMaterial` is a typedef of it). Per-draw terms (tint, metal/rough scale, glow, model) are `AverDrawTerms`: the per-draw cbuffer on raster, `RtInstance` on rays (`rtDrawTerms`, tint linearised like raster).
- **Composition.** `averAuthoredFrom` (factors × maps), the material graph (`averEvalMaterial` on raster, `averApplyMaterialGraphRt` at ray hits, keyed on the hit's own `graphId`), then `averComposeSurface` (F0, energy split, coverage, subsurface, coat, transmission). World-aligned UVs (`averWorldUV`) and the slope layer weight (`averSlopeLayerWeight`) are shared too.
- **Ray hits.** `rtHitSurface` (voxi_rt.hlsli) builds every hit's surface: the ray-driven primary, ReSTIR GI candidates, reflections, Path Tracing vertices and translucent crossings. It reads all eight maps with normal mapping (the LITE detail level, used in the single pass's secondary hits, reads base colour and metal/roughness). The reflection stage builds the same surface, so its gate and its normal are Stage B's.
- **Translucency in the path** (`Settings::translucencyInPath`, staged ray-driven, D3D12). Stage B composites the primary ray's translucent crossings (up to 4) front to back over the lit opaque surface. Each crossing gets full-strength reflection plus coverage-weighted diffuse (`averShadeSplit`), and the absorption over the measured thickness is applied per channel. The device then skips those draws' blended replay (`blendedDrawsResolvedInScene`). Path Tracing paths pass through translucent surfaces with probability 1 − coverage. Kept on the replay: raster, Vulkan, the single pass, and an eye inside a medium.