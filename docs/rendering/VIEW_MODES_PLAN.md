# View modes: wireframe/gizmo bugs, Unlit, and Shader Complexity

> **Caveats.** This is an implementation plan, not a report of work done; status notes below mark what shipped differently. Every code excerpt is a snapshot of 2026-08 and every file:line is stale: re-anchor from the quoted symbol before acting. Two tree-wide moves killed the citations:
>
> 1. **The HLSL left the C++ headers.** Quotes of `modules/render.voxi/src/VoxiShaders.hpp` (cited up to `:1464`; now 17 lines), `modules/render.pbr/src/PbrShaders.cpp` (30 lines), `modules/rhi/src/RHIShaders.cpp` (cited up to `:809`; now ~170 lines, no HLSL), `modules/render.pcg/src/PcgShaders.hpp` (22 lines) and `sandbox/src/ClusterMaterialShader.hpp` (54 lines) now live in `modules/<mod>/shaders/*.hlsl` and `sandbox/shaders/*.hlsl` (`voxi.hlsl`, `material_prelude.hlsl`, `scene.hlsl`/`shared_prelude.hlsl`, `cluster_material.hlsl`). Nobody has diffed the quoted HLSL against them.
> 2. **`SandboxApp.cpp` was split** (`sandbox/CMakeLists.txt:9-11`); all 22 citations into it point past the end of the file. The viewport code is in `sandbox/src/SandboxViewport.cpp`.
>
> The *reasoning* (which view mode needs which pass, why the bugs happen, what Shader Complexity would count) is design and survives a file move. Commit `063e84e` ("Compress comments in the eight heaviest source files") shifted line numbers unevenly in the files this plan cites (e.g. `scenePipeline()`'s wireframe decline `:2068-2071` moved to `:2640`; `suppressesScene()`/`suppressesWholeFrame()` `:1860`/`:1867` to `:2329`/`:2336`).

> **STATUS, 2026-09-26: §1a's Wireframe fix has LANDED, through a DIFFERENT mechanism.** There is no `wireframeForced_`/`VoxiRenderer::setWireframeForced(bool)` and none is planned. `SandboxApp::onUpdate` already builds a PER-FRAME SCRATCH COPY of `voxi::Settings` (for `frameBudgetTick`) and now also forces `vs.rtRenderMode = 0` on that copy whenever Wireframe or a G-buffer debug view (`gbufferDebugView_`) is selected. It is never written back to `voxi::Renderer::get()`'s singleton, so Project Settings and `editor.ini` still show what was authored, and the override is released the instant the mode is left. Wireframe is not greyed out. §1a's rejection of greying out stands, and the other rejected option (silently doing nothing) never shipped; the third option, auto-switching, is what landed.
>
> **Four new debug views also shipped, which §2/§4 do not describe**: Ray Hit: Instances/Materials/Distance and Triangles, a `voxi::VoxiRenderer::ViewDebug` enum (VoxiRenderer.hpp) reusing the SAME pass-level float (`cb_.viewParams[0]`/`gViewParams.x` in `voxi.hlsl`, already carrying Unlit's `0`/`1`) rather than a new cbuffer field. `gViewOverride` was never added. Implemented ONLY in `PSRayDriven`, symmetrically with Wireframe: onUpdate's scratch copy forces `vs.rtRenderMode = 1` (only when `VoxiRenderer::rayDrivenAvailable()` allows it). This is NOT the `ViewMode` enum / `ShadingOverride` / "Shader Complexity" design below: `bool wireframe_` and `bool unlit_` are both still plain bools, beside the new `debugView_` and an independent `bool undenoised_` (a bundle of existing denoiser/ray-tile/spatial-filter/history-reset knobs, combinable with every mode). A `--view-mode lit|unlit|wireframe|rayhit-*|triangles|undenoised` CLI flag exists (SandboxMain.cpp), and `--unlit`. None of §2's preference migration or §4's Shader Complexity has shipped.

> **STATUS (checked against the tree, 2026-10): §1b's selection-outline bug is also FIXED, by a different mechanism than §1b proposes.** `SandboxViewport.cpp` now draws the outline as LINES (boundary and crease edges, cached per mesh) through `drawLines`, which gates on the narrow `suppressesWholeFrame()`, not as a mesh through `drawMesh`. Measured before the fix with the interactive gate lifted: ONE orange pixel in the viewport (a leaf vein). `frameSuppressed()` is still not an `IDevice` accessor (only the private `frameSuppressed_` member on both backends).

---

## 1. The two bugs

### 1a. Wireframe

**Verdict: not a bug in raster mode. Structurally impossible in ray-driven primary visibility as currently built, which is why it reads as broken by default.**

Wireframe is mechanically sound wherever a rasterizer runs. `VoxiRenderer::scenePipeline()` explicitly declines to supply a pipeline when wireframe is requested:

```cpp
// modules/render.voxi/src/VoxiRenderer.cpp:2068-2071
// Returns the lit pipeline for this frame, or 0 to decline and let the backend use its own.
rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed,
                                                bool blended) const {
    if (wireframe) return 0;
```

Both backends treat a `0` return as "fall through to the backend's own fallback PSO," not "drop the draw" (confirmed by reading the fallthrough):

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3281-3282, then :3332-3333
const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_, prepassed, false);
if (!fp) break;
...
ID3D12PipelineState* wantPso = useMs ? msPso_.Get() : (wireframe_ ? wirePso_.Get() : pso_.Get());
```

`wirePso_` is a real pipeline from the same root signature and shader as the solid one, only the fill mode changed (`D3D12Device.cpp:2304-2306`, `pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;`). Vulkan mirrors it exactly (`VulkanDevice.cpp:2301-2302` `if (!fp) break;`, `:2328` `wireframe_ ? wirePso_ : scenePso_`, the PSO at `:1359-1360` with `rs.polygonMode = VK_POLYGON_MODE_LINE`). `IDevice::setWireframe` is overridden on both backends (`D3D12Device.cpp:997`, `VulkanCommon.hpp:1447`), unlike the inert base default at `RHI.hpp:641`. Nothing in this chain is broken; the selection outline (then `SandboxApp.cpp:5457/5471-5473`) depended on exactly this mechanism by forcing `setWireframe(true)`.

What changed is the default mode. `voxi::Settings::rtRenderMode` now defaults to `1`, ray-driven primary visibility:

```cpp
// modules/render.voxi/include/aver/voxi/Voxi.hpp:244-248, :265
// 1 FOR EVERY TIER THAT CAN RUN IT, now -- BY EXPLICIT PRODUCT DECISION... The user calls this
// "the Wavefront Primary rays model" and has decided it is the default render path.
u32 rtRenderMode = 1;
```

`Voxi.cpp:317-320` agrees (`rtRenderModeForQuality(Quality::Medium) == 1`, and `Quality::Medium` is `Settings::rayTracing`'s default). Whether it is *active* is gated by:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:755-758
bool rayDrivenActive() const { return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0; }
```

`rtActive_` is set at `VoxiRenderer.cpp:713-716` (`if (!rtSupported_ || settings_.rayTracing == Quality::Off || drawsPrev_.empty()) return;`) and `:799` (after a successful TLAS build), so ray-driven mode is live on any RT-capable machine with ray tracing not Off that drew at least one instance last frame: the ordinary case, not an edge case.

`VoxiRenderer::suppressesScene()` is true whenever ray-driven mode is active:

```cpp
// modules/render.voxi/src/VoxiRenderer.cpp:1860
bool VoxiRenderer::suppressesScene() const { return debugViewActive() || rayDrivenActive(); }
```

and both backends' `drawMesh()` return **before** the wireframe-aware code is reached:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3269-3274
for (IRenderFeature* f : features_)
    f->submitDraw(mesh, world, color, metallic, roughness, ...);
for (IRenderFeature* f : features_) if (f->suppressesScene()) return;   // <-- exits here

for (IRenderFeature* f : features_) {
    if (!f->overridesScenePipeline() || !rhiContext_) continue;
    const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_, prepassed, false);
```

(`VulkanDevice.cpp:2250-2252` is the same shape.) No per-mesh raster draw happens in this mode: the whole image comes from one full-screen ray-query pixel shader, `PSRayDriven` (`VoxiShaders.hpp:1279-1464`, read start to finish), which never references `wireframe_`, `FillMode` or any polygon-fill concept. Nothing is for "wireframe" to mean. This is not a regression in the fallback chain: a rasterizer-only control collides with a path that has no rasterizer, and now collides by default.

**Fix (proposed; superseded by the STATUS above).** Do not touch the fallback chain. Give Wireframe a defined meaning under ray-driven mode: when selected, suppress ray-driven for that frame and fall back to the raster path, where `scenePipeline()`-declines-to-`wirePso_` already produces a correct wireframe:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp — proposed
bool rayDrivenActive() const {
    return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0 && !wireframeForced_;
}
```

with `wireframeForced_` set from a `VoxiRenderer::setWireframeForced(bool)` called alongside `setWireframe`, mirroring the existing precedent for threading an editor toggle into VoxiRenderer (`SandboxApp.cpp:3159`, `voxiRenderer_.setDebugView(giDebugView_);`, every frame). Alternative rejected: barycentric/`ddx`/`ddy` edge shading inside `PSRayDriven` (real, but more code, a second place wireframe can drift from the raster look, unneeded given the raster fallback). Accepted side effect, not a defect: while Wireframe is selected the GI/reflection answer comes from the raster cone-traced/shadow-map path, a visible difference in the *lit* look of the overlay, since raster and ray-driven are already two approximations of the same scene (`VoxiShaders.hpp:1434-1440`'s comment on the gap).

Greying the option out (as "Unlit"/"Detail Lighting" were disabled at `SandboxApp.cpp:12639-12640`) was considered and rejected: honest, but it makes Wireframe strictly less useful for anyone with ray-driven on, which is everyone by default.

### 1b. Gizmos

**Verdict: the transform gizmo is not suppressed by ray-driven mode, but a different overlay users likely also call "the gizmo," the orange selection outline, genuinely was, a real live bug (fixed since; see STATUS above).**

The `suppressesScene()`/`suppressesWholeFrame()` split introduced by an earlier fix reaches both backends' line-draw call sites:

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

`debugViewActive()` (`VoxiRenderer.hpp:754`, `giReady_ && giEnabled() && debugView_`) is the GI-debug-raymarch flag, not ray-driven mode. Both `drawLines()` gate only on the wide predicate:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3396-3401
// Gizmos and wireframes belong in a ray-driven viewport as much as in a rastered one, and they
// depth-test against the real depth the ray pass writes.
for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
// modules/rhi.vulkan/src/VulkanDevice.cpp:2017-2018 -- same
```

`drawGizmo()` (`SandboxApp.cpp:8784-8815`) never calls `drawMesh()`, only `drawLines()` (with `setLineDepth(false)`/`true` around it), so the arrow/ring/scale gizmo renders in both raster and ray-driven mode (three of four prior investigations agreed; no contradicting path found).

What was silently dropped in ray-driven mode is the orange selection-outline mesh:

```cpp
// sandbox/src/SandboxApp.cpp:5447-5450 (as read then)
// The outline being invisible under path tracing is separate and INTENDED -- the Quality
// combo's own tooltip says the view "SUPPRESSES the raster view entirely while on". This
// does not restore it; it stops the suppressed draw from contaminating what replaced it.
if (hasSelection_ && maxFrames_ == 0 && !e.device()->sceneSuppressed()) {
```

`sceneSuppressed()` (`D3D12Device.cpp:1054-1060`) reflects `suppressesScene()`, true for ray-driven mode too. The comment explained the gate in terms of the path tracer's "Quality" combo only, not that the *default* "Primary visibility" combo (`SandboxApp.cpp:12511-12514`, sets `rtRenderMode`; vs. the Quality combo ~`:12563`, sets `pathTracing`) reaches the same branch. So on the default config, selecting an object drew the transform gizmo but no orange highlight.

**Proposed fix (not the one that landed).** Give the outline the narrow gate. Both backends already compute the state in `beginFrame`:

```cpp
// modules/rhi.d3d12/src/D3D12Device.cpp:3061-3068 (existing)
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

`frameSuppressed_` already means "the debug raymarch or the path tracer has taken the whole frame"; it was never exposed. Add `bool frameSuppressed() const override { return frameSuppressed_; }` beside `sceneSuppressed()` on `IDevice`/`D3D12Device`/`VulkanDevice` (Vulkan needs the member added; `VulkanDevice.cpp:2584-2587` already computes the same two flags in `beginFrame`) and change the guard `SandboxApp.cpp:5450` from `!sceneSuppressed()` to `!frameSuppressed()`. The outline then behaves like the gizmo and grid: gone under the path tracer and GI debug raymarch, present under plain ray-driven. No shader change; it depth-tests against whichever pass wrote depth.

---

## 2. The view-mode refactor (NOT shipped)

`bool wireframe_` becomes a closed enum, because there are four mutually exclusive states:

```cpp
// sandbox/src/SandboxApp.cpp, replaces the `bool wireframe_` declaration at :13048
enum class ViewMode : u8 { Lit, Wireframe, Unlit, ShaderComplexity };
ViewMode viewMode_ = ViewMode::Lit;
```

`Lit = 0` is deliberate: it must reproduce `wireframe_ == false`, and as the default-constructed value any path that forgets to initialise lands on today's behaviour. `Wireframe = 1` is the direct migration target for `wireframe_ == true` (the preference migration depends on this ordering).

### The RHI surface

**Wireframe stays exactly what it is** (a rasterizer fill-mode toggle) and does not absorb the new modes. `IDevice::setWireframe(bool)` (`RHI.hpp:641`) keeps its signature; only call sites change what boolean they compute. `rhi::FillMode` (`RHIResources.hpp:206`, `enum class FillMode : u8 { Solid, Wireframe };`) is already a field on the generic pipeline descriptor (`RHIResources.hpp:310`, consumed by `D3D12Device.cpp:5540` and `VulkanResourceFactory.cpp:2200`/`VulkanPipeline.cpp:539`), separate from `gShadingModel`/`AVER_MODEL_UNLIT` in the material layer (`render.pbr`). Folding unlit into `setWireframe` would make the RHI's fill-mode carry a shading-model concept; the RHI knows nothing about `AVER_MODEL_*`.

Two additive interface members:

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

Shader Complexity is deliberately **not** a third `ShadingOverride` value: it needs to know *which optional per-pixel features a specific draw actually took* (RT shadow vs. cascade, GI cone count, RT reflection vs. cone vs. sky; §4), which exists only inside `PSMainVoxi`/`PSRayDriven`, not in the shared PBR prelude `ShadingOverride` reaches. It is a sibling of the GI-debug view, owned by `VoxiRenderer` like `debugView_`:

```cpp
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp — new, beside setDebugView
void setFeatureCostView(bool on) { featureCostView_ = on; }
```

wired through `suppressesScene()` (takes over per-pixel *shading*) but explicitly **not** `suppressesWholeFrame()` (gizmos/grid/sky/particles keep drawing over it); see §4/§5.

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

"Detail Lighting" stays disabled (not part of this request). The two checkbox sites (`SandboxApp.cpp:9736` Settings popup, `:12097` Preferences > Viewport) cannot express four states and a second four-item picker would be duplication; keep them toggling Lit/Wireframe only, never touching Unlit/Shader Complexity:

```cpp
bool isWire = viewMode_ == ViewMode::Wireframe;
if (ImGui::Checkbox("Wireframe", &isWire))
    viewMode_ = isWire ? ViewMode::Wireframe : ViewMode::Lit;
```

### The `setWireframe()` call sites, plus the new ones

```cpp
// SandboxApp.cpp:4056, was: e.device()->setWireframe(wireframe_);
e.device()->setWireframe(viewMode_ == ViewMode::Wireframe);
e.device()->setShadingOverride(viewMode_ == ViewMode::Unlit
                                ? rhi::ShadingOverride::Unlit : rhi::ShadingOverride::None);
#if AVER_MODULE_VOXI
voxiRenderer_.setFeatureCostView(viewMode_ == ViewMode::ShaderComplexity);
#endif
// SandboxApp.cpp:5473, restoring state after the outline's forced setWireframe(true):
e.device()->setWireframe(viewMode_ == ViewMode::Wireframe);
```

`SandboxApp.cpp:5477`'s `setWireframe(false)` before the grid/nav-mesh line draws needs no change (line draws never consult the fill mode; only `drawMesh()` does).

### The saved preference

Today: `wireframe_ = prefBool("viewport.wireframe", wireframe_);` (`SandboxApp.cpp:11957`) and `setPrefBool("viewport.wireframe", wireframe_);` (`:11992`). The store (`sandbox/src/EditorPrefs.cpp:78-110`) is a flat `key=value` map whose typed accessors take a fallback and cannot distinguish "absent" from "equal to the fallback"; there is no `prefExists`/`prefHas` (`EditorPrefs.hpp` declares only `prefFloat/prefBool/prefInt/prefString` and setters). The migration needs none, because `prefInt`'s fallback composes with the legacy bool:

```cpp
// loadEditorPreferences(), replacing SandboxApp.cpp:11957
const bool legacyWireframe = prefBool("viewport.wireframe", false);
const i32 migratedDefault  = legacyWireframe ? (i32)ViewMode::Wireframe : (i32)ViewMode::Lit;
viewMode_ = static_cast<ViewMode>(prefInt("viewport.viewMode", migratedDefault));
```

If `viewport.viewMode` was never written, `prefInt` returns `migratedDefault`: saved `viewport.wireframe=true` becomes Wireframe on first load, `false`/absent becomes Lit, with no new storage primitive and no first-run flag. Once `saveEditorPreferences()` runs, `viewport.viewMode` shadows the migration. Keep writing the legacy key as a projection so rolling back to a pre-enum build does not silently lose the preference:

```cpp
// saveEditorPreferences(), replacing SandboxApp.cpp:11992
setPrefBool("viewport.wireframe", viewMode_ == ViewMode::Wireframe);
setPrefInt ("viewport.viewMode",  (i32)viewMode_);
```

An old build then sees `true` iff the latest pick was Wireframe, the best a bool can say of a four-way choice. Not permanent: drop the legacy write once rollback is no longer a concern. (Today's tree has the separate `unlit_` bool with its own stored `viewport.unlit`; see the STATUS note.)

---

## 3. Unlit

**Mechanism.** Reuse the existing per-material switch end to end rather than inventing a second unlit concept. It already exists and is dispatched per draw, not baked into a PSO:

```cpp
// modules/render.pbr/src/PbrShaders.cpp:83-85 (now material_prelude.hlsl)
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post
// PbrShaders.cpp:396, inside averBuildSurface -- s.model is read FRESH per draw
s.model = gShadingModel;
```

> **STATUS, 2026-09-08: the Unlit half of this plan has LANDED (31fe41f1), and the analysis below describes the tree BEFORE it.** `writeShadingConstants` takes an `unlit` argument on both backends and writes `AVER_MODEL_UNLIT`; D3D12's `if (unlit_) break;` diversion is gone; `s.displayColor` carries the sampled albedo rather than the raw factor; the GPU cluster path's hand-built PerObject block honours the mode; PSRayDriven answers it from a pass-level `gViewParams`, so Unlit works under ray-driven primary visibility and the editor no longer disables it. WIREFRAME was still unavailable in ray-driven at that date (since solved by auto-switching, see top): it needs a different rasteriser state, not a different shading branch.

`gShadingModel` is a `uint` in `PerObject` (`RHIShaders.cpp:151-155`), a 32-dword root-constant block rewritten per `drawMesh()` via `writeShadingConstants()`. At the time of writing every writer hardcoded `AVER_MODEL_STANDARD`:

```cpp
// D3D12Device.cpp:211-213 and byte-identically VulkanDevice.cpp:377-379
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD in the material prelude
// sandbox/src/SandboxApp.cpp:4927-4928, the LOD/cluster path's own copy
const u32 shadingModel = 0;   // AVER_MODEL_STANDARD
```

So the switch arm was real (`averShadeDirect`/`averShadeIndirect`, `PbrShaders.cpp:580-591` and `:660-679`, both have working `AVER_MODEL_UNLIT` cases) but 100% dead. The proposed minimal mechanism for raster and cluster paths, a per-frame override checked before the assignment:

```cpp
// PbrShaders.cpp:396, proposed
s.model = (gViewOverride.x > 0.5) ? AVER_MODEL_UNLIT : gShadingModel;
```

`gViewOverride` would be a new `float4` in the shared `PerFrame` cbuffer (`RHIShaders.cpp:100-149`), following `gFurnace`'s precedent ("a measuring instrument, not a look"). A cbuffer field is a coordinated edit: the HLSL declaration and its C++ mirror `PerFrameCB` (`D3D12Device.cpp:496-520`) must agree field for field, and the mirror is duplicated in `VulkanDevice.cpp`, `VulkanCommon.hpp`, `VulkanRenderContext.cpp`, `GameApp.cpp` and `WaterShaders.hpp` (all found by repo-wide search for `PerFrameCB`): six files, same new field in the same position; bounded one-time cost, not a design risk. (As shipped, the pass-level `gViewParams` was reused instead.)

**The correction the investigations converged on.** What actually fires for a real authored material is `averDisplayColour`, not the two `AVER_MODEL_UNLIT` arms:

```cpp
// PbrShaders.cpp:396-402
s.model = gShadingModel;
...
s.display = gShadingModel == AVER_MODEL_UNLIT;
s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
// PbrShaders.cpp:492-494
bool averDisplayColour(AverSurface s, out float4 rgba) { rgba = s.displayColor; return s.display; }
// VoxiShaders.hpp:1088-1090, inside PSMainVoxi
AverSurface s = averEvalMaterial(vtx, sun);
float4 display;
if (averDisplayColour(s, display)) return display;
```

`s.displayColor` is built from `gBaseColor`, the per-draw *tint*, and for any authored `.ocmat` that tint is deliberately neutralised to white:

```cpp
// Runtime/src/GameRender.cpp:127-133
if (authored) {
    // An AUTHORED material supplies its own colour and its own metal/rough through the
    // binding set below, so the per-draw values are neutralised to 1 rather than left as
    // the fallback.
    col[0] = col[1] = col[2] = 1.0f;
    metallic = roughness = 1.0f;
}
```

So Unlit merely switched on would paint every authored, textured mesh flat WHITE. The fix sources `displayColor` from the evaluated albedo:

```cpp
// PbrShaders.cpp:402, was: s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
s.displayColor = float4(s.albedo, gBaseColor.a);   // s.albedo is built at :403; swap the order
```

The second, independently wrong definition in the `AVER_MODEL_UNLIT` arms of `averShadeDirect` (`return radiance;`) and `averShadeIndirect` (`return radiance + s.emissive;`, i.e. emissive only) is never reached through `averDisplayColour`'s early return, so it is inert dead code, but should be deleted or reconciled so the codebase does not carry two contradictory ideas of "unlit" (`PbrShaders.cpp:702-706`'s comment calls it "an unlit surface has no specular lobe... its one diffuse contribution... is its authored emissive colour"). A product decision resolved here in favour of "show the authored colour," matching the `#define`'s own comment at `PbrShaders.cpp:85`.

**Where the override does and does not reach.** `giReady_` (gates whether `VoxiRenderer::scenePipeline()` is consulted) is true once Voxi `init()` succeeds (`VoxiRenderer.cpp:279`) until a device-loss teardown (`:346`), independent of `rtRenderMode`, `rayTracing` or wireframe. On any build with Voxi compiled in (the `standard` edition, `CMakeLists.txt:57`, `:86-87` `_aver_edition_defaults(ON ...)`), `overridesScenePipeline()` (`VoxiRenderer.cpp:1855`, `return giReady_;`) is true all session, so every ordinary scene mesh goes through `averEvalMaterial`: via `PSMainVoxi` for the main scene and `PSClusterMain` (`ClusterMaterialShader.hpp:91,104,134,187`, calls `averEvalMaterial` directly, not the older flat-silhouette path its header calls superseded) for LOD/cluster. One override reaches both.

The one raster path it does **not** reach is the backend's fallback pipeline (`PSMainPlain` -> `plainShadeSurface`, `D3D12Device.cpp:234-235`), which never calls `averEvalMaterial` and has a third independent "unlit" flag, `gMaterial.z` (`RHIShaders.cpp:154`, `"z=unlit(0/1) for plainShadeSurface only"`, consulted at `:809-811`). Every writer hardcodes it `0.0f` (`D3D12Device.cpp:3307/3337/4443`, `VulkanDevice.cpp:2311/2338`, `SandboxApp.cpp:4926`), as dead as `gShadingModel` was. It is reached only when `scenePipeline()` declines (wireframe on, or Voxi never initialised). Wireframe and Unlit are mutually exclusive, so this is not a live gap in the ordinary case; it matters only with the Voxi module compiled out (`AVER_MODULE_VOXI`), where Unlit would also need to set `gMaterial.z` at the same writers. Scoped out of v1 and stated, not silently limited to "projects that build Voxi."

**Ray-driven mode needs its own, different mechanism**; see §5.

### What each non-mesh thing does under Unlit

| Thing | Behaviour under Unlit | Why |
|---|---|---|
| Sky / atmosphere / clouds / sun disc | Unaffected, full atmosphere | `PSky` and the cloud raymarch never call `averEvalMaterial` (repo-wide search found only `ActorPreview.cpp`, `Material.cpp`, `MaterialGraphHlsl.cpp`, `PbrShaders.cpp`, `VoxiShaders.hpp`). Sky is outside the material system, matching UE, where Unlit still shows the sky. |
| Water | Unaffected | `WaterRenderer.hpp:104-105` states it deliberately does not override `suppressesScene`/`scenePipeline`; own render path, never calls `averEvalMaterial`. Flattening it would need a separate hook; follow-up if the product wants full parity. |
| Particles | Unaffected | Never route through `averEvalMaterial`; most particle materials are emissive-like already. |
| Landscape | **Affected**, like any mesh | No dedicated `LandscapeRenderer`/`IRenderFeature` (only `VoxiRenderer`, `WaterRenderer`, `ParticleRenderer`, `UiRenderer` derive `IRenderFeature`); terrain goes through `drawMesh()`/`scenePipeline()`. |
| Gizmos | Unaffected | `drawLines()`/`drawGizmo()` never touch `gShadingModel`/`ShadingOverride` (§1b). |
| Editor UI (ImGui) | Unaffected | Own UI backend (`uiNewFrame()`/`endFrame()`), outside the material pipeline. |

**One deliberate exclusion that must be enforced, not hoped for.** `PSVoxel`, the voxelisation pass that bakes radiance into Voxi's GI volume, also calls `averEvalMaterial` (`MaterialGraphRegistry.hpp:5`: "today PSMainVoxi, PSVoxel and PSClusterMain"), and runs every frame regardless of view mode (`VoxiRenderer.cpp:580-581`'s `prePass()`, gated only on `giReady_`). The override must be read only by the shaders producing the *displayed* image (`PSMainVoxi`, `PSClusterMain`, `PSRayDriven`), NOT by `PSVoxel`. If it leaked in, Unlit would flatten indirect bounce for the *entire scene*, including geometry off-camera, for as long as Unlit stays selected, and silently move the existing GI-lit render gates.

---

## 4. Shader Complexity, honestly (NOT shipped)

**What Aver has, and does not have.** A repo-wide search for instruction counting, overdraw counters or GPU shader-cost instrumentation (`overdraw|Overdraw|heatmap|instructionCount|shaderComplexity` across `VoxiRenderer.*`, `VoxiShaders.hpp`, `SandboxApp.cpp`) found only two unrelated prose comments. There is no per-instruction cost model and DXC does not expose one. A literal copy of UE's F5 view (heatmap of estimated ALU/texture instruction count) is not buildable; say so rather than ship a colourful view implying it is.

**What is chosen: "Feature cost"**, which optional, expensive per-pixel lighting features this exact pixel paid for this frame, on the two paths that decide divergently per pixel: the raster GI path (`PSMainVoxi`) and ray-driven primary visibility (`PSRayDriven`). A direct readout of branches the shader already took, not an estimate. Three real decisions (read from `VoxiShaders.hpp` that pass):

```cpp
// VoxiShaders.hpp:1070-1074, inside PSMainVoxi — shadow: cascade map, or a traced ray
if (gShadowParams.z > 0.5)
    sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos), (uint)max(gRtParams.y, 1.0));
else                       sunVis = shadowFactor(i.wpos, N, ndl);
// VoxiShaders.hpp:1080 — indirect diffuse: a bounded voxel-cone gather, or none
if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);
// VoxiShaders.hpp:1134,1142,1150 — reflection: a traced ray, a voxel cone, or flat sky, keyed
// on this pixel's own roughness (s.rough), so the SAME material can take different branches
// across its own surface as roughness varies (e.g. a scratched, spatially-varying metal)
if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) { ... /* RT reflection */ }
else if (gVoxelParams.w > 0.5)                                     { ... /* voxel cone */    }
else                                                                { ... /* flat sky */      }
```

`PSRayDriven` (`:1279-1464`) adds a per-pixel cost with no raster equivalent: a bounded path-traced bounce loop, `const uint bounces = (uint)max(gPtBounceParams.x, 1.0);` (`:1392`). Ray-driven always pays a primary ray and a shadow ray per pixel plus optional bounces.

**Where the branch lives.** Following `averDisplayColour`'s early-out shape (`PbrShaders.cpp:492-494`), insert the debug branch **inside** `PSMainVoxi` right after the reflection block resolves (~`VoxiShaders.hpp:1150`, when all three decisions are known) and **inside** `PSRayDriven` at the equivalent point, not in a fourth full-screen shader. `PSRayDriven`'s header comment says it must stay in lockstep with `PSMainVoxi` or the two pictures disagree; a view reading the *same* branch variables cannot drift from what the frame did, where a second shader re-deriving "was this reflection a ray or a cone" could.

**Score and colour ramp.** Weighted sum of the already-known decisions, normalised to `[0,1]`:

- shadow: `0` (cascade) or `1` (ray), weight `1`
- GI: `0` (no cone) or the actual ring count, `gGiParams.x` clamped to Voxi's configured max (`VoxiShaders.hpp:1021-1023`'s `ring`), scaled to `[0,1]`
- reflection: `0` (sky), `0.5` (voxel cone), `1` (RT ray)
- ray-driven only: `+ bounces / kMaxBounces` (`gPtBounceParams.x`, clamped at source)

Summed, re-normalised, mapped green -> yellow -> red with a plain three-stop lerp (`0.0` = `(0,1,0)`, `0.5` = `(1,1,0)`, `1.0` = `(1,0,0)`): close enough to UE's ramp to read instantly, while the legend says it is a different quantity.

**Legend text, verbatim, to ship in the tooltip:**

> **Feature cost — not GPU instructions.** Green means this pixel took the cheapest path available: cascade shadow, no GI cone gather, sky-only reflection (or, in ray-driven mode, the primary ray and shadow ray only, no path-traced bounce). Red means it took every optional ray or cone this frame's settings allow: a traced shadow ray, a full GI cone gather, a traced reflection ray (or, in ray-driven mode, the maximum bounce count). This is a direct readout of which expensive lighting features this exact pixel invoked — not an estimate of shader instruction count, register pressure, or per-pixel GPU time. Aver has no instruction-level shader profiler; this is the honest substitute.

**Not covered, stated rather than hidden.** Nothing about texture-sample count, material-graph complexity or overdraw. Evaluated and deferred:

- *Material-graph node count* (static per object): `MaterialGraphRegistry::Entry` (`MaterialGraphRegistry.hpp:59-64`) has no node-count field; add one storing `OcGraphData::nodes.size()` (`OcGraph.hpp:250`) at `add()`. Cheap but a different, static-per-instance measure needing its own separately labelled legend. Good low-risk follow-up.
- *Overdraw*: a real supported blend mode (`BlendMode::Additive`, `RHIResources.hpp:197-203`, shipping for particles) makes an additive fragment-count pass low-risk later. But overdraw is a *raster* concept; ray-driven answers one hit per pixel by construction, so under ray-driven it would read a flat `1` everywhere geometry was hit: correct, not a bug, but uninteresting where the new default lives.
- *GPU timing* is available (`RHI.hpp:273-279` `GpuTimingReport`, `D3D12Device.cpp:1061`); surface it as a supplementary text overlay beside Shader Complexity, not folded into the picture. Real per-pass milliseconds, the closest thing to ground truth, but D3D12-only (no override in `VulkanDevice.cpp`/`VulkanCommon.hpp`) and it answers "how long did the scene pass take," not "which pixel cost the most."

---

## 5. Both new modes under ray-driven primary visibility

**Unlit: can work, but needs its own small mechanism; flipping `gShadingModel` is not enough.** `PSRayDriven` never calls `averBuildSurface`/`averEvalMaterial`; it hand-builds a minimal `AverSurface` from the ray hit (`VoxiShaders.hpp:1368-1383`) and hardcodes the model:

```cpp
// VoxiShaders.hpp:1363-1367, 1383
// TWO CONSTANTS ARE DEFAULTED because they are per-MATERIAL and a ray hit has no material
// constant buffer bound...
s.model = AVER_MODEL_STANDARD;
```

The comment is right that there is no per-draw constant buffer to read a per-*material* switch from, but a per-*frame* constant (`gViewOverride` as proposed, `gViewParams.x` as shipped) is reachable from any shader including the shared prelude. The trap: setting `s.model = AVER_MODEL_UNLIT` and letting `averShadeDirect`/`averShadeIndirect` run would show BLACK, not the authored colour: `s.emissive` is never assigned in the hand-built surface (zero from `(AverSurface)0`), so the `AVER_MODEL_UNLIT` arm (`return radiance + s.emissive;`, `PbrShaders.cpp:663-664`) returns nothing. The correct mechanism is a direct early-out with `inst.albedo`, the baked authored albedo (not the tint-neutralised `gBaseColor` that trips the raster path in §3; a ray hit never had that neutralisation), placed after the hit's albedo/normal/world-position are known (~`VoxiShaders.hpp:1310-1322`, before the shadow ray at `:1335`):

```cpp
if (gViewOverride.x > 0.5) {
    o.col   = float4(inst.albedo, 1.0);
    o.depth = <the same depth this pass already writes for a hit, further down>;
    return o;
}
```

This also *skips* the shadow ray, bounce loop and reflection cone for that pixel: ray-driven Unlit is cheaper than ray-driven Lit. The raster path's `averDisplayColour` early-out does **not** get that for free: in `PSMainVoxi` the shadow ray (`:1070-1077`) and GI cone gather (`:1080`) both run *before* `averEvalMaterial`/`averDisplayColour` (`:1088-1090`), so raster Unlit pays for both and discards them. Hoisting the raster check to before line 1070 closes that gap and is recommended as part of the same change (a small reordering, not new logic).

**Shader Complexity works in ray-driven mode by design, arguably more informatively.** The branch sits inside `PSRayDriven` (§4), not derived from a raster concept, so there is no "does not apply" case as for Wireframe. The bounce-count term adds a signal (path-traced bounce depth) the raster view cannot show.

**Suppression semantics, against the contract §1b traces.** Unlit changes only *shading*, never which pass owns the frame; `suppressesScene()`/`suppressesWholeFrame()` for `VoxiRenderer` are unaffected and ordinary ray-driven rules apply. Shader Complexity's `featureCostView_` should be OR'd into `suppressesScene()` (takes over per-pixel colour, as ray-driven does) but must **not** be OR'd into `suppressesWholeFrame()`: unlike the GI-debug raymarch, the point is the cost of a *specific frame*, and a frame with no sky, gizmos or grid tells the user less. This is a third distinct case for the contract at `RHIResources.hpp:816-838` (which names two); that comment should gain a third bullet when this lands.

**Wireframe stays what §1a decided:** structurally impossible in ray-driven (no rasterizer to put a fill mode on); the fix is a raster fallback for that frame, not a ray-space edge-shading substitute. Of the new modes plus the existing control, Wireframe is the one genuinely one-mode-only feature; Unlit and Shader Complexity are first-class in ray-driven, not degraded fallbacks.

---

## 6. Staging and verification

Every stage is independently shippable; each predicts whether it should move any of the 18 existing render-gate values. Two grounding facts: (1) at the time, `SandboxApp.cpp`'s command-line parsing had no `--wireframe`/`--unlit`/`--shader-complexity` flag, so the gate harness could not select a non-default view mode and nothing here could move a gate it cannot reach (today `--unlit` and `--view-mode ...` exist; `--shader-complexity` does not); (2) `docs/BUGS.md`'s account of the gate oracle (`3302f89`, "Nine of eighteen gates went blind") is the precedent for the failure to guard against: a gate that never names the flag this work introduces silently measures whatever the *default* is, forever, unless each stage adds the flag needed to reach it.

**Stage 1 — the two bug fixes (§1a, §1b), against the current `bool wireframe_`.** Independent of the enum refactor; ship first (smallest change, fixes something reported broken).
- 1a: `VoxiRenderer::setWireframeForced(bool)` gating `rayDrivenActive()` (shipped as the scratch-copy auto-switch).
- 1b: `frameSuppressed()` on `IDevice`, changing `SandboxApp.cpp:5450`'s guard (fixed instead by drawing the outline as lines).
- **Gate prediction: none of the 18 move.** No gate sets `wireframe_` (no CLI flag existed), so 1a's branch is never taken; 1b's guard is behind `maxFrames_ == 0` and every gate/capture run passes a non-zero frame count, so it was never reachable from a gate.
- **New verification:** add a `--wireframe` CLI flag (mirroring `--gi-debug`, `SandboxApp.cpp:16155`; `--view-mode wireframe` now covers it), then a few probe points confirming (a) wireframe edges visible under `--rt-render-mode 1 --wireframe` where they were flat-shaded solid, and (b) the selection-outline colour (`kSelect`, orange `{1.0,0.62,0.12,1.0}`, `SandboxApp.cpp:5451`) present at a probe over a selected object under plain ray-driven where it was absent.

**Stage 2 — the `ViewMode` enum and preference migration (§2).** Pure refactor: `ViewMode::Lit == wireframe_ == false` and `ViewMode::Wireframe == wireframe_ == true` are the only states reachable until Stage 3/4, so Lit/Wireframe must render bit-identically by construction.
- **Gate prediction: none of the 18 move**, and here it is load-bearing, not incidental: a supposedly behaviour-preserving refactor is what the gate oracle exists to catch. Run the existing suite unmodified and expect every value to match its pre-refactor baseline; a mismatch means the migration or call-site translation is wrong, not that a gate needs re-recording.
- Preference migration is a manual check, not a gate: load the editor with an `editor.ini` containing `viewport.wireframe=true`, confirm the dropdown shows "Wireframe" on first launch of the new build, save, confirm `editor.ini` now has both keys agreeing.

**Stage 3 — Unlit (§3).** Ships the `gViewOverride` cbuffer field (six-file edit), the `ShadingOverride` RHI addition, the `s.model`/`s.displayColor` fixes in `PbrShaders.cpp`, and the ray-driven early-out in `PSRayDriven` (§5).
- **Gate prediction: none of the 18 move**: `gViewOverride.x` defaults `0` and no gate sets it.
- **New verification:** add `--unlit` and at least two new gates: (i) plain raster, a known authored material's probe pixel reads its base-colour texture value (not white, not black, not the ambient-only "unlit-emissive" answer the old dead code would give) with the sun disabled in the test scene so a correct Unlit answer is trivially distinguishable from a lit one; (ii) `--rt-render-mode 1 --unlit`, ray-driven probe matches the raster Unlit probe for the same object within ordinary cross-path tolerance (parity with the *existing* raster/ray-driven agreement on Lit scenes is the bar, not an exact match).
- Confirm explicitly, as its own gate or assertion, that a GI-lit probe pixel elsewhere (depending on bounced light from an Unlit-tagged surface) is unchanged with Unlit on vs. off, guarding the `PSVoxel` exclusion (§3).

**Stage 4 — Shader Complexity (§4).** Ships the in-place branches in `PSMainVoxi` and `PSRayDriven`, `VoxiRenderer::setFeatureCostView`, and the `suppressesScene()`-only wiring.
- **Gate prediction: none of the 18 move** (flag defaults off, nothing sets it).
- **New verification:** add `--shader-complexity` and gates in at least two configurations that force different branches: `--no-rt` (cascade shadow, no cone reflection: probe green/low) and RT at a quality enabling the RT shadow ray and RT reflection (`--rt-render-mode 1`, `gRtParams.w > 0.5`: probe red/high), making "does the colour track the branch" a pixel-value assertion, not a spot-check. Also confirm that gizmos/grid remain visible with Shader Complexity on (the `suppressesWholeFrame()` distinction of §5 taking effect); a screenshot that merely "looks like a heatmap" would not catch a gizmo silently missing.

**General note on the harness.** The capture harness (`--frames`, `--probe`/`--probe-rel`) samples specific pixels in specific configurations; per `docs/BUGS.md` it already went silently blind once when a default flipped under gates that named no flag for the thing that changed. Every new flag above exists for that reason: without it the gate oracle can never exercise Wireframe-under-ray-driven, Unlit or Shader Complexity, and every stage's "none of the 18 gates move" would be true forever by default rather than by design, which cost nine of eighteen gates their meaning last time.
