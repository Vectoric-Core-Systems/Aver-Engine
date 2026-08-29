# View modes: wireframe/gizmo bugs, Unlit, and Shader Complexity

This is an implementation plan, not a report of work already done. Every claim below was re-verified
by direct reads of the files cited, in this working tree, during this pass — not taken on the word of
the four investigations that fed into it. Where those four disagreed, this document says which one
was right and why. Two caveats apply everywhere: `modules/render.voxi/*`, `modules/rhi.d3d12/src/
D3D12Device.cpp`, `modules/rhi.vulkan/src/VulkanDevice.cpp` and `modules/rhi/include/aver/rhi/
{RHI.hpp,RHIResources.hpp}` are all `M` in `git status` and `render.voxi` is being edited by another
workflow concurrently, so every line number is "as read at the time of this pass," not a promise the
file will still read that way tomorrow; and per this task's own rules, nothing here has been built or
run — every behavioural claim is a trace through source, not a screenshot.

---

## 1. The two bugs

### 1a. Wireframe

**Verdict: not a bug in raster mode. Structurally impossible in ray-driven primary visibility as
currently built, which is why it now reads as broken by default.**

Wireframe is mechanically sound today wherever a rasterizer actually runs. `VoxiRenderer::
scenePipeline()` explicitly declines to supply a pipeline when wireframe is requested:

```cpp
// modules/render.voxi/src/VoxiRenderer.cpp:2068-2071
// Returns the lit pipeline for this frame, or 0 to decline and let the backend use its own.
rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed,
                                                bool blended) const {
    if (wireframe) return 0;
```

Both backends treat a `0` return as "fall through to the backend's own fallback PSO," not as "drop the
draw" — confirmed by reading the fallthrough directly, not just the comment above it:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3281-3282, then :3332-3333
const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_, prepassed, false);
if (!fp) break;
...
ID3D12PipelineState* wantPso = useMs ? msPso_.Get() : (wireframe_ ? wirePso_.Get() : pso_.Get());
```

`wirePso_` is a real pipeline, built from the same root signature and the same shader as the solid
one, with only the fill mode changed:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:2304-2306
pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&wirePso_)), "wire pso")) return false;
pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
```

Vulkan mirrors this exactly (`VulkanDevice.cpp:2301-2302`'s `if (!fp) break;`, `:2328`'s
`wireframe_ ? wirePso_ : scenePso_`, and the PSO itself at `VulkanDevice.cpp:1359-1360` with
`rs.polygonMode = VK_POLYGON_MODE_LINE`). `IDevice::setWireframe` is not a no-op either — it is
overridden on both backends (`D3D12Device.cpp:997`, `VulkanCommon.hpp:1447`, the class every
`VulkanDevice` method in this trace actually belongs to), unlike the base class's inert default at
`RHI.hpp:641`. Nothing in this chain is broken. The selection-outline chrome
(`SandboxApp.cpp:5457/5471-5473`) already depends on exactly this mechanism working, by forcing
`setWireframe(true)` and drawing through the identical opaque `drawMesh()` path.

What has changed is which mode is active by default. `voxi::Settings::rtRenderMode` now defaults to
`1` — ray-driven primary visibility:

```cpp
// modules/render.voxi/include/aver/voxi/Voxi.hpp:244-248, :265
// 1 FOR EVERY TIER THAT CAN RUN IT, now -- BY EXPLICIT PRODUCT DECISION... The user calls this
// "the Wavefront Primary rays model" and has decided it is the default render path.
u32 rtRenderMode = 1;
```

and `Voxi.cpp:317-320` confirms the tier table agrees with the struct default (`rtRenderModeForQuality
(Quality::Medium) == 1`, and `Quality::Medium` is `Settings::rayTracing`'s own default). Whether this
default is actually *active* at runtime is gated by `VoxiRenderer::rayDrivenActive()`:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:755-758
bool rayDrivenActive() const { return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0; }
```

`rtActive_` itself is set at `VoxiRenderer.cpp:713-716` (`if (!rtSupported_ || settings_.rayTracing ==
Quality::Off || drawsPrev_.empty()) return;`) and `:799` (`rtActive_ = true;` after a successful TLAS
build) — so ray-driven mode is live on any RT-capable machine, with ray tracing not set to Off, that
drew at least one instance last frame. That is the ordinary case on the hardware this project targets,
not an edge case.

`VoxiRenderer::suppressesScene()` is true whenever ray-driven mode is active:

```cpp
// modules/render.voxi/src/VoxiRenderer.cpp:1860
bool VoxiRenderer::suppressesScene() const { return debugViewActive() || rayDrivenActive(); }
```

and both backends' `drawMesh()` return **before** the wireframe-aware code above is ever reached:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3269-3274
for (IRenderFeature* f : features_)
    f->submitDraw(mesh, world, color, metallic, roughness, ...);
for (IRenderFeature* f : features_) if (f->suppressesScene()) return;   // <-- exits here

for (IRenderFeature* f : features_) {
    if (!f->overridesScenePipeline() || !rhiContext_) continue;
    const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_, prepassed, false);
```

(`VulkanDevice.cpp:2250-2252` is the same shape.) There is no per-mesh raster draw at all in this
mode: the entire visible image comes from one full-screen ray-query pixel shader,
`PSRayDriven` (`modules/render.voxi/src/VoxiShaders.hpp:1279-1464`), which I read start to finish —
it never references `wireframe_`, `FillMode`, or any polygon-fill concept. There is no rasterizer
invocation for scene geometry in this mode, so there is categorically nothing for "wireframe" to mean.
This is not a regression in the fallback chain above; it is a rasterizer-only control colliding with a
path that has no rasterizer, and it now collides by default rather than only for someone who went
looking for ray-driven mode.

**Fix.** Do not touch the fallback chain — it is correct. Give Wireframe a defined, useful meaning
under ray-driven mode instead of a silent no-op: when Wireframe is selected, suppress ray-driven mode
for that frame and let the scene fall back to the raster path, where the existing, working
`scenePipeline()`-declines-to-`wirePso_` chain already produces a correct wireframe image. Concretely,
gate `rayDrivenActive()` on the current view mode:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp — new
bool rayDrivenActive() const {
    return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0 && !wireframeForced_;
}
```

with `wireframeForced_` set from a new `VoxiRenderer::setWireframeForced(bool)` called alongside
`setWireframe`, mirroring the existing precedent for threading an editor toggle into VoxiRenderer
(`SandboxApp.cpp:3159`, `voxiRenderer_.setDebugView(giDebugView_);`, called every frame the same way).
This is a two-line, low-risk change: it reuses machinery that already works rather than inventing a
wireframe concept for a ray-hit shader (a real option — barycentric/`ddx`/`ddy` edge shading inside
`PSRayDriven` — but strictly more code, a second place wireframe can drift from the raster look, and
not needed when the raster fallback already exists and is correct). The one thing worth stating
plainly rather than hiding: while Wireframe is selected, the frame's GI/reflection answer comes from
the raster cone-traced/shadow-map path instead of the ray-driven one, which is a real, visible
difference in the *lit* look on the wireframe overlay itself (not just the fill mode) — an accepted
side effect, not a defect, since ray-driven and raster already are two different approximations of the
same scene per `VoxiShaders.hpp:1434-1440`'s own comment on the gap between them.

Greying the option out (matching how "Unlit"/"Detail Lighting" are currently disabled at
`SandboxApp.cpp:12639-12640`) was considered and rejected: it is honest, but it makes Wireframe
strictly less useful than it is today for anyone who has ray-driven mode on, which is now everyone by
default. Falling back to raster keeps the control doing what it has always done.

### 1b. Gizmos

**Verdict: the transform gizmo itself is not suppressed by ray-driven mode, as the code reads right
now — but a different overlay the user is very likely to also call "the gizmo," the orange selection
outline, genuinely is, and that is a real, live bug.**

The `suppressesScene()`/`suppressesWholeFrame()` split that a prior fix introduced for exactly this
class of problem is present and does reach both backends' line-draw call sites:

```cpp
// modules/rhi/include/aver/rhi/RHIResources.hpp:816-838 (the contract)
virtual bool suppressesScene() const { return false; }
...
//   2. "THE FIRST SURFACE IS MINE" -- ray-driven primary visibility. It replaces the RASTER,
//      and nothing more: the frame still has a sky above it, still has editor gizmos in it,
//      still has particles in front of it. Suppressing those as well left the ray-driven
//      viewport with no clouds, no physical atmosphere, no sun disc and no gizmos...
virtual bool suppressesWholeFrame() const { return suppressesScene(); }
```

```cpp
// modules/render.voxi/src/VoxiRenderer.cpp:1867
bool VoxiRenderer::suppressesWholeFrame() const { return debugViewActive(); }
```

`debugViewActive()` (`VoxiRenderer.hpp:754`, `giReady_ && giEnabled() && debugView_`) is the separate
GI-debug-raymarch flag, not ray-driven mode. Both `drawLines()` implementations gate only on the wide
predicate:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3396-3401
// Gizmos and wireframes belong in a ray-driven viewport as much as in a rastered one, and they
// depth-test against the real depth the ray pass writes.
for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
```

```cpp
// modules/rhi.vulkan/src/VulkanDevice.cpp:2017-2018
// suppressesWholeFrame, matching D3D12: gizmos belong in a ray-driven viewport.
for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
```

and `drawGizmo()` — read in full — never calls `drawMesh()`, only `drawLines()`:

```cpp
// sandbox/src/SandboxApp.cpp:8784-8815 (the entire body, condensed)
void drawGizmo(Engine& e) {
    if (editorModeIsLandscape()) return;
    if (tool_!=Tool::Move && tool_!=Tool::Rotate && tool_!=Tool::Scale) return;
    ...
    e.device()->setLineDepth(false);
    for (int a=0;a<3;++a) e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
    e.device()->setLineDepth(true);
}
```

So per the code as it reads right now, the arrow/ring/scale-handle gizmo should render in both raster
and ray-driven mode. This matches three of the four prior investigations and I could not find a path
that contradicts it — with the standing caveat that `render.voxi` is under concurrent edit and this is
a snapshot, not a promise.

What I *did* find, directly, is a second overlay that is silently dropped in ray-driven mode and is
easily mistaken for "the gizmo is gone": the orange selection-outline mesh.

```cpp
// sandbox/src/SandboxApp.cpp:5447-5450
// The outline being invisible under path tracing is separate and INTENDED -- the Quality
// combo's own tooltip says the view "SUPPRESSES the raster view entirely while on". This
// does not restore it; it stops the suppressed draw from contaminating what replaced it.
if (hasSelection_ && maxFrames_ == 0 && !e.device()->sceneSuppressed()) {
```

`sceneSuppressed()` reflects `suppressesScene()`, not `suppressesWholeFrame()`:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:1054-1060
bool sceneSuppressed() const override {
    for (const IRenderFeature* f : features_) if (f->suppressesScene()) return true;
    return false;
}
```

`suppressesScene()` is true for ray-driven mode as well as the debug raymarch
(`VoxiRenderer.cpp:1860`, quoted in §1a). The comment at `SandboxApp.cpp:5447-5449` explains this gate
in terms of the **path tracer's** "Quality" combo alone; it does not mention that the *default*
"Primary visibility" combo (a different toggle — compare `SandboxApp.cpp:12511-12514`, which sets
`rtRenderMode`, against the Quality combo around `:12563`, which sets `pathTracing`) reaches the exact
same branch. So today, on the now-default configuration, selecting an object draws the actual
transform gizmo (if Move/Rotate/Scale is the active tool) but **no** orange highlight around it — a
real, reachable, currently-shipping gap, not a hypothetical one, and one the existing comment does not
disclose applies here. If the user's report is "I select something and nothing marks it as selected,"
this is very likely what they are seeing, since the axis gizmo only appears for the three transform
tools and is easy to overlook if the highlight itself is what they expect.

**Fix.** Give the outline the same narrow gate gizmos and lines already get, rather than the broad one
built for the path tracer. The cleanest route reuses state both backends already compute every frame:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3061-3068 (existing, in beginFrame)
sceneSuppressed_ = false;
frameSuppressed_ = false;
for (IRenderFeature* f : features_) {
    if (!f->suppressesScene()) continue;
    ...
    sceneSuppressed_ = true;
    if (f->suppressesWholeFrame()) frameSuppressed_ = true;
    return;
}
```

`frameSuppressed_` already exists and already means exactly "the debug raymarch or the path tracer has
taken the whole frame" — it is simply never exposed. Add `bool frameSuppressed() const override {
return frameSuppressed_; }` next to `sceneSuppressed()` on `IDevice`/`D3D12Device`/`VulkanDevice`
(Vulkan will need the equivalent member added; a quick check shows `VulkanDevice.cpp:2584-2587` already
computes the same two flags in its own `beginFrame`, so this is a same-shape addition, not new
bookkeeping), and change the outline's guard:

```cpp
// SandboxApp.cpp:5450, was: !e.device()->sceneSuppressed()
if (hasSelection_ && maxFrames_ == 0 && !e.device()->frameSuppressed()) {
```

This makes the outline behave exactly like the gizmo and the grid: gone under the path tracer and the
GI debug raymarch (both legitimately own the whole frame), present under plain ray-driven primary
visibility (which, per the RHIResources.hpp contract quoted above, is supposed to leave "the sky...
gizmos... particles" alone, and the outline is editor chrome of exactly that kind). No shader change is
needed — the outline already draws through the ordinary wireframe/opaque `drawMesh()` path and depth-
tests against whichever pass, raster or ray-driven, wrote the depth buffer this frame.

---

## 2. The view-mode refactor

`bool wireframe_` becomes a small, closed enum, because there are now four mutually exclusive states,
not two:

```cpp
// sandbox/src/SandboxApp.cpp, replaces the `bool wireframe_` declaration at :13048
enum class ViewMode : u8 { Lit, Wireframe, Unlit, ShaderComplexity };
ViewMode viewMode_ = ViewMode::Lit;
```

`Lit = 0` is deliberate: it is the enum value that must reproduce today's `wireframe_ == false`, and
giving it the default-constructed value (`ViewMode{}` is `Lit`) means any code path that forgets to
initialize `viewMode_` explicitly still lands on today's behaviour rather than an arbitrary one.
`Wireframe = 1` is the direct migration target for `wireframe_ == true` — see the preference migration
below, which depends on this ordering.

### The RHI surface

The important design decision here is that **wireframe stays exactly what it is** — a rasterizer fill-
mode toggle — and does not absorb the two new modes. `IDevice::setWireframe(bool)` (`RHI.hpp:641`)
keeps its current signature; only its call sites change what boolean they compute. This matches how
the RHI already separates these concerns: `rhi::FillMode` (`RHIResources.hpp:206`, `enum class FillMode
: u8 { Solid, Wireframe };`) is a real, already-used field on the generic pipeline descriptor
(`RHIResources.hpp:310`, consumed by both backends' generic `createGraphicsPipeline` at
`D3D12Device.cpp:5540` and `VulkanResourceFactory.cpp:2200`/`VulkanPipeline.cpp:539`), entirely separate
from `gShadingModel`/`AVER_MODEL_UNLIT`, which lives in the material layer (`render.pbr`). Folding
"unlit" or "shader complexity" into `setWireframe` would make the RHI's fill-mode concept also carry a
shading-model concept, which is exactly the kind of layering violation this codebase's own module
boundaries (RHI knows nothing about `AVER_MODEL_*`; that lives in `render.pbr`) argue against.

Instead, two additive interface members are needed:

```cpp
// modules/rhi/include/aver/rhi/RHIResources.hpp, beside FillMode at :206
// A whole-frame shading override a caller can force regardless of any one draw's own
// gShadingModel. None leaves every draw's own model in charge -- today's behaviour, unchanged.
enum class ShadingOverride : u8 { None, Unlit };
```

```cpp
// modules/rhi/include/aver/rhi/RHI.hpp, beside setWireframe at :640-641
// Forces every subsequent scene draw's shading to `mode` until changed again, independent of
// each draw's own gShadingModel. See RHIResources.hpp::ShadingOverride.
virtual void setShadingOverride(ShadingOverride mode) { (void)mode; }
```

Shader Complexity is deliberately **not** a third `ShadingOverride` value, because it does not fit the
"replace the shading model" shape at all — it needs to know *which optional per-pixel features a
specific draw call actually took* (RT shadow ray vs. cascade, GI cone count, RT reflection vs. cone vs.
sky — see §4), information that only exists inside `PSMainVoxi`/`PSRayDriven` themselves, not inside
the shared PBR prelude `ShadingOverride` would reach. It is instead a third sibling to the existing
GI-debug view, owned by `VoxiRenderer` the same way `debugView_` is:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp — new, beside setDebugView
void setFeatureCostView(bool on) { featureCostView_ = on; }
```

wired through `suppressesScene()` (so it joins `debugViewActive()`/`rayDrivenActive()` in taking over
per-pixel *shading*) but explicitly **not** through `suppressesWholeFrame()` (so, unlike the existing
GI debug raymarch, gizmos/grid/sky/particles keep drawing over it) — see §4 and §5 for why that
matters and exactly where the branch belongs.

### The three UI call sites

```cpp
// SandboxApp.cpp:12635-12644 -- the dropdown becomes the one place all four states are chosen
static const char* kViewModeLabel[] = {"Lit", "Wireframe", "Unlit", "Shader Complexity"};
if (dropButton(kViewModeLabel[(int)viewMode_])) ImGui::OpenPopup("viewMode");
if (ImGui::BeginPopup("viewMode")) {
    if (ImGui::Selectable("Lit",              viewMode_==ViewMode::Lit))              viewMode_ = ViewMode::Lit;
    if (ImGui::Selectable("Wireframe",        viewMode_==ViewMode::Wireframe))         viewMode_ = ViewMode::Wireframe;
    if (ImGui::Selectable("Unlit",            viewMode_==ViewMode::Unlit))             viewMode_ = ViewMode::Unlit;
    if (ImGui::Selectable("Shader Complexity",viewMode_==ViewMode::ShaderComplexity))  viewMode_ = ViewMode::ShaderComplexity;
    ImGui::Selectable("Detail Lighting", false, ImGuiSelectableFlags_Disabled);   // still out of scope
    ...
}
```

`"Detail Lighting"` stays disabled — it is not part of this request, and disabling it honestly (as
today) is correct where enabling Unlit and Shader Complexity for real is not.

The two checkbox sites (`SandboxApp.cpp:9736`, the Settings popup, and `:12097`, Preferences >
Viewport) cannot express four states as one checkbox, and widening both into a second copy of the
four-item picker is needless duplication for what has always been a quick raster-debug toggle in
those two places. The lowest-risk, most literal migration keeps them doing exactly what they did
before — toggling between Lit and Wireframe only, never touching Unlit/Shader Complexity even if one
of those happens to be selected:

```cpp
// both call sites, same pattern
bool isWire = viewMode_ == ViewMode::Wireframe;
if (ImGui::Checkbox("Wireframe", &isWire))
    viewMode_ = isWire ? ViewMode::Wireframe : ViewMode::Lit;
```

### The two `setWireframe()` call sites, plus the new ones

```cpp
// SandboxApp.cpp:4056, was: e.device()->setWireframe(wireframe_);
e.device()->setWireframe(viewMode_ == ViewMode::Wireframe);
e.device()->setShadingOverride(viewMode_ == ViewMode::Unlit
                                ? rhi::ShadingOverride::Unlit : rhi::ShadingOverride::None);
#if AVER_MODULE_VOXI
voxiRenderer_.setFeatureCostView(viewMode_ == ViewMode::ShaderComplexity);
#endif
```

```cpp
// SandboxApp.cpp:5473, restoring state after the selection-outline's forced setWireframe(true)
// was: e.device()->setWireframe(wireframe_);
e.device()->setWireframe(viewMode_ == ViewMode::Wireframe);
```

`SandboxApp.cpp:5477`'s `e.device()->setWireframe(false);`, immediately before the grid/nav-mesh line
draws, needs no enum-aware change: it already forces the fill mode to a fixed `false` regardless of
`wireframe_`'s value (line draws never consult it in the first place — only `drawMesh()` does), so it
stays a fixed `false` under the enum too.

### The saved preference

Today: `wireframe_ = prefBool("viewport.wireframe", wireframe_);` (`SandboxApp.cpp:11957`) and
`setPrefBool("viewport.wireframe", wireframe_);` (`:11992`). The preference store
(`sandbox/src/EditorPrefs.cpp:78-110`) is a flat `key=value` map with typed accessors that each take a
fallback and do not otherwise distinguish "absent" from "present but equal to the fallback" — there is
no `prefExists`/`prefHas` today (checked: `EditorPrefs.hpp` declares only `prefFloat/prefBool/prefInt/
prefString` and their setters). Rather than adding one, the migration is self-contained by construction
because of how `prefInt`'s fallback composes with the legacy bool:

```cpp
// loadEditorPreferences(), replacing SandboxApp.cpp:11957
const bool legacyWireframe = prefBool("viewport.wireframe", false);
const i32 migratedDefault  = legacyWireframe ? (i32)ViewMode::Wireframe : (i32)ViewMode::Lit;
viewMode_ = static_cast<ViewMode>(prefInt("viewport.viewMode", migratedDefault));
```

If `"viewport.viewMode"` has never been written, `prefInt` returns exactly `migratedDefault` — so a
user who already has `viewport.wireframe=true` saved gets `ViewMode::Wireframe` on first load with the
new build, and a user with it `false` or absent gets `ViewMode::Lit`, both silently correct with no new
storage primitive and no explicit "is this the first run of the new build" flag. Once
`saveEditorPreferences()` runs once under the new build, `"viewport.viewMode"` exists and shadows the
migration path on every subsequent load.

`saveEditorPreferences()` should keep writing the legacy key too, as a projection, so a user who rolls
back to a pre-enum build does not silently lose the preference (the definition of the silent reset the
task warns against):

```cpp
// saveEditorPreferences(), replacing SandboxApp.cpp:11992
setPrefBool("viewport.wireframe", viewMode_ == ViewMode::Wireframe);
setPrefInt ("viewport.viewMode",  (i32)viewMode_);
```

An old build reading only `"viewport.wireframe"` after that sees `true` if and only if the user's most
recent pick under the new build was Wireframe specifically — the same answer that key has always
given, and the best a bool can represent of a four-way choice. This mirrors is not a permanent
commitment: once the enum build has been out long enough that rollback is not a live concern, the
legacy write can be dropped in a later pass; nothing about the migration above depends on it staying
forever.

---

## 3. Unlit

**Mechanism.** Reuse the existing per-material switch end to end rather than inventing a second unlit
concept, exactly as the brief hoped, with one correction the investigations converged on and one this
pass adds precision to.

The switch already exists and is already dispatched per draw, not baked into any one PSO:

```cpp
// modules/render.pbr/src/PbrShaders.cpp:83-85
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post
```

```cpp
// PbrShaders.cpp:396, inside averBuildSurface — s.model is read FRESH per draw
s.model = gShadingModel;
```

`gShadingModel` is a `uint` inside `PerObject` (`RHIShaders.cpp:151-155`), a 32-dword root-constant
block rewritten by every `drawMesh()` call via `writeShadingConstants()`. Today, every writer of that
slot hardcodes `AVER_MODEL_STANDARD`, with no exception anywhere in the tree:

```cpp
// D3D12Device.cpp:211-213 and byte-identically VulkanDevice.cpp:377-379
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD in the material prelude
```
```cpp
// sandbox/src/SandboxApp.cpp:4927-4928, the LOD/cluster path's own copy of the same constants
const u32 shadingModel = 0;   // AVER_MODEL_STANDARD
```

So the switch arm is real and correctly implemented downstream (`averShadeDirect`/`averShadeIndirect`
at `PbrShaders.cpp:580-591` and `:660-679` both have working `AVER_MODEL_UNLIT` cases) but 100% dead
today, because nothing ever writes anything else into the slot. Making Unlit real is, for the ordinary
raster and cluster paths, a matter of adding exactly one more writer — a per-frame override, checked
right before the existing assignment:

```cpp
// PbrShaders.cpp:396, new
s.model = (gViewOverride.x > 0.5) ? AVER_MODEL_UNLIT : gShadingModel;
```

`gViewOverride` is a new `float4` in the shared `PerFrame` cbuffer (`RHIShaders.cpp:100-149`),
following the exact precedent `gFurnace` already sets one field above it — "a measuring instrument,
not a look," toggled by the application, read everywhere the shared prelude is compiled in. Adding a
field to this cbuffer is a coordinated, not a local, edit: the HLSL declaration
(`RHIShaders.cpp:100-149`) and its C++ mirror, `PerFrameCB` (`D3D12Device.cpp:496-520`), must agree
field for field, and the same mirrored struct is duplicated in `VulkanDevice.cpp`,
`VulkanCommon.hpp`, `VulkanRenderContext.cpp`, `GameApp.cpp`, and `WaterShaders.hpp` (all five turned
up by a repo-wide search for `PerFrameCB`) — six files total need the same new field added in the same
position, which is real but bounded, one-time cost, not a design risk.

**The correction the investigations converged on.** The mechanism that actually fires for a real,
authored material is `averDisplayColour`, not the two `AVER_MODEL_UNLIT` switch arms directly:

```cpp
// PbrShaders.cpp:396-402
s.model = gShadingModel;
...
s.display = gShadingModel == AVER_MODEL_UNLIT;
s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
```
```cpp
// PbrShaders.cpp:492-494
bool averDisplayColour(AverSurface s, out float4 rgba) { rgba = s.displayColor; return s.display; }
```
```cpp
// modules/render.voxi/src/VoxiShaders.hpp:1088-1090, inside PSMainVoxi
AverSurface s = averEvalMaterial(vtx, sun);
float4 display;
if (averDisplayColour(s, display)) return display;
```

`s.displayColor` is built from `gBaseColor`, the per-draw *tint* — and for any authored `.ocmat`,
that tint is deliberately neutralised to white before the draw, so a real material's own colour never
reaches it:

```cpp
// modules/runtime.game/src/GameRender.cpp:127-133
if (authored) {
    // An AUTHORED material supplies its own colour and its own metal/rough through the
    // binding set below, so the per-draw values are neutralised to 1 rather than left as
    // the fallback.
    col[0] = col[1] = col[2] = 1.0f;
    metallic = roughness = 1.0f;
}
```

So today's Unlit, if merely switched on, would paint every authored, textured mesh flat white, not its
albedo — this is not a hypothetical, it follows directly from these two pieces of code composed. The
fix is to source `displayColor` from the material's real evaluated albedo instead of the tint:

```cpp
// PbrShaders.cpp:402, was: s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
s.displayColor = float4(s.albedo, gBaseColor.a);   // s.albedo is built two lines below today; move it up
```

(`s.albedo` is built at `PbrShaders.cpp:403` today, one line after `displayColor` — the two need to
swap order, a small, contained edit.) The second, independently-wrong definition living in
`averShadeDirect`/`averShadeIndirect`'s own `AVER_MODEL_UNLIT` arms — `return radiance;` (direct) and
`return radiance + s.emissive;` (indirect), i.e. *emissive only* — is never reached for anything that
goes through `averDisplayColour`'s early return first, so it is inert dead code today, not a second
active bug, but it should be deleted or explicitly reconciled with the fixed `displayColor` so the
codebase does not carry two contradictory ideas of "unlit" indefinitely (`PbrShaders.cpp:702-706`'s own
comment already flags this as "an unlit surface has no specular lobe... its one diffuse contribution...
is its authored emissive colour," which is a real, coherent, but *different* definition of unlit than
"show the authored base colour" — a product decision, not just a bug, and one this plan resolves in
favour of "show the authored colour," matching the `AVER_MODEL_UNLIT` `#define`'s own comment,
`"authored colour"`, at `PbrShaders.cpp:85`).

**Where this override does and does not reach, and why that is the correct scope, not an oversight.**
`giReady_` — the flag that gates whether `VoxiRenderer::scenePipeline()` is even consulted — becomes
true once Voxi's own `init()` succeeds (`VoxiRenderer.cpp:279`) and stays true until a device-loss
teardown (`:346`); it does not depend on `rtRenderMode`, `rayTracing` quality, or wireframe. So on any
build with the Voxi module compiled in (the `standard` edition, `CMakeLists.txt:57` default, per
`:86-87`'s `_aver_edition_defaults(ON ...)`), `overridesScenePipeline()` (`VoxiRenderer.cpp:1855`,
`return giReady_;`) is true for the entire session, and every ordinary scene mesh's shading goes through
`averEvalMaterial` — via `PSMainVoxi` for the main scene and via `PSClusterMain`
(`sandbox/src/ClusterMaterialShader.hpp:91,104,134,187`, confirmed calling `averEvalMaterial` directly,
not the older flat-silhouette path its own header comment describes as superseded) for the LOD/cluster
path. Both are reached by the one override above.

The one raster path this override does **not** reach is the backend's own fallback pipeline
(`PSMainPlain` → `plainShadeSurface`, `D3D12Device.cpp:234-235`), which never calls
`averEvalMaterial` and has its own, third, independent "unlit" flag, `gMaterial.z`
(`RHIShaders.cpp:154`, `"z=unlit(0/1) for plainShadeSurface only"`, consulted at `RHIShaders.cpp:809-
811`). Every writer of `gMaterial.z` in the tree hardcodes it to `0.0f`
(`D3D12Device.cpp:3307/3337/4443`, `VulkanDevice.cpp:2311/2338`, `SandboxApp.cpp:4926`) — it is exactly
as dead as `gShadingModel` was, just a separate mechanism. This path is reached only when
`scenePipeline()` declines, which today means: wireframe is on, or Voxi never initialised. Because the
new `ViewMode` enum makes Wireframe and Unlit mutually exclusive by construction (the popup is a
single-selection group), a user can never be in both at once through the UI, so this third mechanism
is not a live gap for the ordinary case — it only matters on a build where the Voxi module is compiled
out entirely (a real, if unusual, configuration; `AVER_MODULE_VOXI` is checked at several call sites
above), where Unlit would need to *also* set `gMaterial.z` at the same writers listed above, gated the
same way. This plan scopes that out of v1 and states it here rather than silently limiting Unlit to
"projects that build Voxi in" without saying so.

**Ray-driven mode needs its own, different mechanism, not a flag on this one** — see §5.

### What each non-mesh thing does under Unlit

| Thing | Behaviour under Unlit | Why |
|---|---|---|
| Sky / atmosphere / clouds / sun disc | Unaffected — full atmosphere, exactly as Lit | `PSky` and the cloud raymarch never call `averEvalMaterial` (repo-wide search for the call found only `ActorPreview.cpp`, `Material.cpp`, `MaterialGraphHlsl.cpp`, `PbrShaders.cpp`, `VoxiShaders.hpp` — no sky shader). Sky is architecturally outside the material system, matching UE's own convention that Unlit still shows the sky. |
| Water | Unaffected — shades normally | `WaterRenderer.hpp:104-105` states plainly it deliberately does not override `suppressesScene`/`scenePipeline`/etc.; it is its own render path and never calls `averEvalMaterial`. Flattening water under Unlit would need a second, separate hook this plan does not build; flagged as a follow-up if the product wants full parity, not silently dropped. |
| Particles | Unaffected — additive/alpha-blended sprites shade as authored | Never route through `averEvalMaterial` either; most particle materials are already emissive-like by convention, so this is the least likely of the non-mesh cases to feel wrong left alone. |
| Landscape | **Affected**, same as any other mesh | No dedicated `LandscapeRenderer`/`IRenderFeature` exists (a repo search for `: public IRenderFeature` found only `VoxiRenderer`, `WaterRenderer`, `ParticleRenderer`, `UiRenderer`) — landscape terrain is ordinary geometry drawn through the same `drawMesh()`/`scenePipeline()` path as everything else, so it goes flat under Unlit exactly like a static mesh does, correctly, with no special-casing needed. |
| Gizmos | Unaffected | `drawLines()`/`drawGizmo()` never touch `gShadingModel`/`ShadingOverride` at all (§1b). |
| Editor UI (ImGui chrome) | Unaffected | Rendered through `uiNewFrame()`/`endFrame()`'s own UI backend, entirely outside the scene/material pipeline. |

**One deliberate exclusion that must be enforced, not merely hoped for.** `PSVoxel`, the voxelisation
pass that bakes radiance into Voxi's GI volume, also calls `averEvalMaterial`
(`MaterialGraphRegistry.hpp:5`'s own comment: "today PSMainVoxi, PSVoxel and PSClusterMain"). That pass
runs every frame regardless of which view mode is on-screen (`VoxiRenderer.cpp:580-581`'s `prePass()`,
gated only on `giReady_`). `gViewOverride` must be read only by the shaders that produce the
*displayed* image — `PSMainVoxi`, `PSClusterMain`, `PSRayDriven` — and explicitly not by `PSVoxel`. If
it leaked into the voxelisation pass, turning the viewport to Unlit would flatten indirect bounce
lighting for the *entire scene*, including geometry the camera is not even looking at, for as long as
Unlit stayed selected — a global, cross-frame side effect nobody asked for, and one that would silently
move several of the existing render gates (the ones that read GI-lit values) the moment Unlit was
exercised anywhere near a gated capture. This is the one place in this plan where "reuse the existing
override" must stop at a specific boundary rather than being threaded everywhere the prelude compiles.

---

## 4. Shader Complexity, honestly

**What Aver has, and does not have.** A repo-wide search for instruction counting, overdraw counters,
or any GPU shader-cost instrumentation (`overdraw|Overdraw|heatmap|instructionCount|shaderComplexity`
across `VoxiRenderer.*`, `VoxiShaders.hpp`, `SandboxApp.cpp`) returns nothing but two unrelated prose
comments about draw ordering. There is no per-instruction cost model anywhere in this engine, and DXC
does not expose one either. A literal copy of UE's F5 view — a heatmap keyed to estimated ALU/texture
instruction count per pixel — is not buildable here. Saying so plainly, rather than shipping a
colourful view that implies it is that, is the point of this section.

**What is chosen instead, and exactly what it measures.** "Feature cost": which of the optional,
expensive, per-pixel lighting features this exact pixel actually paid for this frame, on the two paths
that make that decision divergently per pixel — the raster GI path (`PSMainVoxi`) and ray-driven
primary visibility (`PSRayDriven`). This is not an estimate or a proxy; it is a direct readout of
branches the shader has already taken, re-purposed into a colour instead of being consumed by the
lighting math. Three real, already-computed decisions, read directly from `VoxiShaders.hpp` in this
pass (not paraphrased):

```cpp
// VoxiShaders.hpp:1070-1074, inside PSMainVoxi — shadow: cascade map, or a traced ray
if (gShadowParams.z > 0.5)
    sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos), (uint)max(gRtParams.y, 1.0));
else                       sunVis = shadowFactor(i.wpos, N, ndl);
```
```cpp
// VoxiShaders.hpp:1080 — indirect diffuse: a bounded voxel-cone gather, or none
if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);
```
```cpp
// VoxiShaders.hpp:1134,1142,1150 — reflection: a traced ray, a voxel cone, or flat sky, keyed
// on this pixel's own roughness (s.rough), so the SAME material can take different branches
// across its own surface as roughness varies (e.g. a scratched, spatially-varying metal)
if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) { ... /* RT reflection */ }
else if (gVoxelParams.w > 0.5)                                     { ... /* voxel cone */    }
else                                                                { ... /* flat sky */      }
```

`PSRayDriven` (`VoxiShaders.hpp:1279-1464`) adds one more real, per-pixel-varying cost that has no
raster equivalent: a bounded path-traced bounce loop, `const uint bounces = (uint)max(gPtBounceParams.x,
1.0);` (`:1392`) — ray-driven mode always pays at least a primary ray and a shadow ray per pixel, and
optionally several bounce rays on top, all real GPU work this view can honestly report.

**Where the branch lives, and why it must live there and not in a duplicate shader.** Following the
exact shape `averDisplayColour`'s early-out already uses (`PbrShaders.cpp:492-494`, consulted right
after the relevant state is known), insert the debug branch **inside** `PSMainVoxi` immediately after
the reflection block resolves (around `VoxiShaders.hpp:1150`, once all three decisions above are known
for this pixel) and **inside** `PSRayDriven` at the equivalent point, rather than writing a fourth,
separate full-screen shader. This project's own comments repeatedly warn about exactly this class of
bug — `PSRayDriven`'s header comment is explicit that it must stay in lockstep with `PSMainVoxi` or the
two pictures disagree for reasons nobody intended — and a debug view that reads the *same* branch
variables the real shading already computed cannot drift out of sync with what the frame actually did,
where a second shader re-deriving "was this pixel's reflection a ray or a cone" from scratch could.

**Score and colour ramp.** A weighted sum of the three (four, in ray-driven mode) already-known
booleans/counts, normalised to `[0,1]`:

- shadow: `0` (cascade) or `1` (ray) — weight `1`
- GI: `0` (no cone) or the actual ring count used, `gGiParams.x` clamped to Voxi's own configured max
  (`VoxiShaders.hpp:1021-1023`'s `ring`), scaled to `[0,1]`
- reflection: `0` (sky), `0.5` (voxel cone), `1` (RT ray)
- ray-driven only: `+ bounces / kMaxBounces` (`gPtBounceParams.x`, already clamped at source)

summed and re-normalised, then mapped green → yellow → red with a plain three-stop lerp (`0.0` =
`(0,1,0)`, `0.5` = `(1,1,0)`, `1.0` = `(1,0,0)`), chosen specifically because it is close enough to
UE's own ramp to read instantly, while the legend makes clear it is a different quantity underneath.

**Legend text, verbatim, to ship in the tooltip:**

> **Feature cost — not GPU instructions.** Green means this pixel took the cheapest path available:
> cascade shadow, no GI cone gather, sky-only reflection (or, in ray-driven mode, the primary ray and
> shadow ray only, no path-traced bounce). Red means it took every optional ray or cone this frame's
> settings allow: a traced shadow ray, a full GI cone gather, a traced reflection ray (or, in ray-driven
> mode, the maximum bounce count). This is a direct readout of which expensive lighting features this
> exact pixel invoked — not an estimate of shader instruction count, register pressure, or per-pixel
> GPU time. Aver has no instruction-level shader profiler; this is the honest substitute.

**What this does not cover, stated rather than hidden.** It says nothing about texture-sample count,
material-graph complexity, or overdraw. Two candidates were evaluated and deliberately deferred rather
than folded in:

- *Material-graph node count* (a static, per-object number — `MaterialGraphRegistry::Entry`
  (`MaterialGraphRegistry.hpp:59-64`) has no node-count field today, confirmed by reading the struct; it
  would need one added, storing `OcGraphData::nodes.size()` — `OcGraph.hpp:250` — at `add()` time,
  which is cheap but is a *different*, static-per-instance measure, not a per-pixel one, and would need
  its own, separately-labelled legend so it is not confused with the per-pixel score above). Good,
  low-risk follow-up; not in this plan's v1.
- *Overdraw* (a real, already-supported blend mode — `BlendMode::Additive`,
  `RHIResources.hpp:197-203`, already shipping for particles — makes an additive fragment-count pass
  low-risk to build later). Worth stating plainly now: overdraw is a *raster* concept, and ray-driven
  mode answers exactly one hit per pixel by construction, so an overdraw view under ray-driven mode
  would honestly read a flat `1` everywhere geometry was hit — a correct answer, not a bug, but one
  that makes the view much less interesting exactly where the engine's new default lives.
- *GPU timing* is already available (`RHI.hpp:273-279`'s `GpuTimingReport`, `D3D12Device.cpp:1061`) and
  should be surfaced as a supplementary text overlay alongside Shader Complexity, not folded into the
  picture — it is real per-pass milliseconds, genuinely the closest thing to ground truth this engine
  has, but it is D3D12-only today (no override exists anywhere in `VulkanDevice.cpp`/
  `VulkanCommon.hpp`, confirmed by search) and it answers "how long did the whole scene pass take,"
  not "which pixel cost the most," so it complements this view rather than replacing it.

---

## 5. Both new modes under ray-driven primary visibility

**Unlit: can work, but needs its own small mechanism — flipping `gShadingModel` is not enough.**
`PSRayDriven` never calls `averBuildSurface`/`averEvalMaterial`; it hand-builds a minimal `AverSurface`
directly from the ray hit (`VoxiShaders.hpp:1368-1383`), and explicitly hardcodes the model:

```cpp
// VoxiShaders.hpp:1363-1367, 1383
// TWO CONSTANTS ARE DEFAULTED because they are per-MATERIAL and a ray hit has no material
// constant buffer bound...
s.model = AVER_MODEL_STANDARD;
```

Its comment is correct that there is no per-draw constant buffer to read a *per-material* switch from —
but `gViewOverride` (§3) is a *per-frame*, not per-material, constant, already reachable from any shader
that includes the shared prelude, `PSRayDriven` included. The subtlety a naive fix would miss: simply
setting `s.model = AVER_MODEL_UNLIT` here and letting `averShadeDirect`/`averShadeIndirect` run as
normal would **not** show the authored colour — it would show black. `s.emissive` is never assigned in
this hand-built `AverSurface` (it defaults to zero from `(AverSurface)0`), so the fixed `AVER_MODEL_
UNLIT` arm (`return radiance + s.emissive;`, `PbrShaders.cpp:663-664`) returns nothing. The correct
mechanism is a direct early-out with the real value this pass already has cheaply and correctly —
`inst.albedo`, the baked, authored albedo (not the tint-neutralised `gBaseColor` that trips up the
raster path in §3, because a ray hit was never subject to that neutralisation in the first place) —
inserted right after the hit's albedo/normal/world-position are known (around `VoxiShaders.hpp:1310-
1322`, before the shadow ray at `:1335`):

```cpp
if (gViewOverride.x > 0.5) {
    o.col   = float4(inst.albedo, 1.0);
    o.depth = <the same depth this pass already writes for a hit, further down>;
    return o;
}
```

Placed there, this also *skips* the shadow ray, the bounce loop and the reflection cone for that pixel
entirely — a genuine efficiency side benefit worth stating plainly (ray-driven Unlit is cheaper than
ray-driven Lit, not the same cost with different math), and one the raster path's own current
`averDisplayColour` early-out does **not** get for free: in `PSMainVoxi`, the shadow ray
(`:1070-1077`) and the GI cone gather (`:1080`) both run *before* `averEvalMaterial`/`averDisplayColour`
is even reached (`:1088-1090`), so raster Unlit still pays for both before discarding them. Hoisting the
raster check earlier, to before line 1070, would close that gap too and is recommended as part of the
same change, not a separate follow-up — it is a small reordering, not new logic.

**Shader Complexity: works in ray-driven mode by design, and is arguably more informative there.**
Because the branch is inserted directly inside `PSRayDriven` itself (§4), not derived from a raster
concept that ray-driven mode lacks, there is no "does not apply here" case for this mode the way there
is for Wireframe. If anything, ray-driven mode's own bounce-count term gives Shader Complexity a fourth,
genuinely-only-visible-here signal (path-traced bounce depth) that the raster view cannot show at all,
since the raster path has no bounce loop of its own.

**Suppression semantics for both, stated against the same contract §1b traces.** Unlit changes only
*shading*, never which pass owns the frame — `suppressesScene()`/`suppressesWholeFrame()` for
`VoxiRenderer` are unaffected by it; ordinary ray-driven suppression rules (§1b) apply exactly as
before. Shader Complexity's `featureCostView_` should be OR'd into `suppressesScene()` (so it takes
over per-pixel colour output, same as ray-driven mode itself already does) but must **not** be OR'd
into `suppressesWholeFrame()` — unlike the existing GI-debug raymarch, this view's whole point is to
show the cost of a *specific frame*, and a frame with no sky, no gizmos and no grid drawn over it tells
the user less, not more, about what that frame paid for. This is a third distinct case for the contract
documented at `RHIResources.hpp:816-838` (which currently only names two), and that comment should gain
a third bullet describing it when this lands.

**Wireframe stays exactly what §1a already decided:** structurally impossible in ray-driven mode
because there is no rasterizer to put a fill mode on, and the fix is to make Wireframe force a raster
fallback for that frame rather than to invent a ray-space edge-shading substitute. Nothing about that
conclusion changes here — it is restated only to be explicit that, of the two new modes plus the one
existing control, Wireframe is the one genuinely one-mode-only feature; Unlit and Shader Complexity are
both real, first-class citizens of ray-driven mode once built as designed above, not degraded fallbacks.

---

## 6. Staging and verification

Every stage below is independently shippable — none depends on a later one, and each states, as a
prediction rather than a discovery, whether it should move any of the 18 existing render-gate values.
Two facts ground every prediction: first, a repo-wide search of `SandboxApp.cpp`'s command-line parsing
found no `--wireframe`/`--unlit`/`--shader-complexity` flag of any kind today — the gate harness has no
way to select any non-default view mode, so nothing described here can move a gate it cannot reach.
Second, `docs/BUGS.md`'s own account of the gate oracle (`3302f89`, "Nine of eighteen gates went blind")
is the concrete precedent for exactly the failure mode to guard against: a gate that never names the
flag this work introduces will silently measure whatever the *default* happens to be, forever, which is
not a fixed quantity across this plan's stages unless each stage adds the flag needed to reach it.

**Stage 1 — the two bug fixes (§1a, §1b), against the current `bool wireframe_`.**
Independent of the enum refactor; ship first since it is the smallest, most isolated change and fixes
something already reported broken.
- 1a: `VoxiRenderer::setWireframeForced(bool)`, gating `rayDrivenActive()`.
- 1b: `frameSuppressed()` on `IDevice`, changing `SandboxApp.cpp:5450`'s guard.
- **Gate prediction: none of the 18 existing gates move.** No gate sets `wireframe_` true (no CLI flag
  exists to do so), so 1a's new branch is never taken by any gate run. 1b's guard is behind `maxFrames_
  == 0` at the same call site — every gate/capture run passes a non-zero frame count, so this branch
  has never been reachable from a gate in the first place, before or after the fix.
- **New verification, not existing-gate verification:** add a `--wireframe` CLI flag (mirroring
  `--gi-debug`'s existing shape at `SandboxApp.cpp:16155`) so this path becomes reachable by the
  harness at all, then a small number of new probe points confirming (a) wireframe edges are visible
  under `--rt-render-mode 1 --wireframe` where they were previously flat-shaded solid colour, and (b)
  the selection-outline colour (`kSelect`, the orange `{1.0,0.62,0.12,1.0}` at `SandboxApp.cpp:5451`)
  is present at a probe over a selected object under plain ray-driven mode where it was previously
  absent.

**Stage 2 — the `ViewMode` enum and preference migration (§2).**
Pure refactor: `Lit`/`Wireframe` must render bit-identically to before, by construction, since
`ViewMode::Lit == wireframe_ == false` and `ViewMode::Wireframe == wireframe_ == true` are the only two
states any existing code path can reach until Stage 3/4 land.
- **Gate prediction: none of the 18 gates move**, and this stage is the one place that prediction is
  load-bearing rather than incidental — a refactor that is *supposed* to be behaviour-preserving is
  exactly the kind of change the gate oracle exists to catch if it is not. Run the existing suite
  unmodified after this stage and expect every one of the 18 values to match its pre-refactor baseline;
  a mismatch here means the migration or the call-site translation is wrong, not that a gate needs
  re-recording.
- Preference migration is a manual/exploratory check, not a gate: load the editor with a pre-existing
  `editor.ini` containing `viewport.wireframe=true`, confirm the dropdown shows "Wireframe" on first
  launch of the new build, save, and confirm `editor.ini` now contains both keys agreeing.

**Stage 3 — Unlit (§3).**
Ships the `gViewOverride` cbuffer field (coordinated edit across the six files named in §3), the
`ShadingOverride` RHI addition, the `s.model`/`s.displayColor` fixes in `PbrShaders.cpp`, and the
ray-driven early-out in `PSRayDriven` (§5).
- **Gate prediction: none of the 18 existing gates move.** `gViewOverride.x` defaults to `0` and no
  existing gate configuration sets it; every current gate's rendered pixels are computed with the
  override at its default off-state, identical to before this stage existed.
- **New verification:** add `--unlit`, and at least two new gates: one in plain raster mode confirming
  a known authored material's probe pixel reads its base-colour texture value (not white, not black,
  not the ambient-only "unlit-emissive" answer the old dead code would have given) with the sun
  disabled in the test scene (so a correct Unlit answer is trivially distinguishable from a lit one);
  and one under `--rt-render-mode 1 --unlit` confirming the ray-driven path's probe matches the raster
  Unlit probe for the same object within ordinary cross-path tolerance (the two are still two different
  approximations of the same scene, per §1a's fallback-mode caveat — an exact match is not the bar,
  parity with the *existing* raster/ray-driven agreement on Lit scenes is).
- Explicitly confirm — as its own gate or an assertion, not left to inspection — that a GI-lit probe
  pixel elsewhere in the scene (one that depends on bounced light from an Unlit-tagged surface) is
  unchanged with Unlit on versus off, guarding the `PSVoxel` exclusion called out at the end of §3.

**Stage 4 — Shader Complexity (§4).**
Ships the in-place branches inside `PSMainVoxi` and `PSRayDriven`, `VoxiRenderer::setFeatureCostView`,
and the `suppressesScene()`-only (not `suppressesWholeFrame()`) wiring.
- **Gate prediction: none of the 18 existing gates move**, for the same reason as Stage 3 — the new
  flag defaults off and nothing existing sets it.
- **New verification:** add `--shader-complexity`, and gates in at least two configurations known to
  force different branches today — one with `--no-rt` (cascade shadow, no cone reflection: should probe
  green/low-score) and one with ray tracing at a quality tier that enables the RT shadow ray and RT
  reflection (`--rt-render-mode 1`, ray tracing quality high enough for `gRtParams.w > 0.5`: should
  probe red/high-score) — turning the "does the colour actually track the branch" question into a
  pixel-value assertion instead of a visual spot-check. Also confirm, as a gate or explicit check, that
  gizmos/grid remain visible with Shader Complexity on (the `suppressesWholeFrame()` distinction from
  §5 actually taking effect), since a regression here would be silent otherwise — a screenshot that
  merely "looks like a heatmap" would not catch a gizmo quietly missing from it.

**General note on the harness.** This engine's capture harness (`--frames`, `--probe`/`--probe-rel`)
samples specific pixels in specific configurations; per `docs/BUGS.md`'s own account, it has already
gone silently blind once when a default flipped underneath gates that named no flag for the thing that
changed. Every new flag introduced above exists for exactly that reason — without it, the render-gate
oracle has no way to ever exercise Wireframe-under-ray-driven, Unlit, or Shader Complexity, and every
stage's "none of the 18 gates move" prediction would be true forever by default rather than by design,
which is precisely the failure mode that cost nine of eighteen gates their meaning last time.
