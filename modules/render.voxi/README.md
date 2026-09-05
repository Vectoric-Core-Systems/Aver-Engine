# Aver.Render.Voxi

Voxi is Aver Engine's **optional render-feature module** — the first module gated behind a CMake
option (`-DAVER_MODULE_VOXI=OFF` and the engine builds and runs exactly as before).

It owns the renderer's project-wide **quality settings** and, crucially, decides from the real
device capabilities which of them can actually be used:

| Setting | State |
|---|---|
| **Anti-Aliasing (MSAA)** | **Implemented.** Off / 2x / 4x / 8x, applied at runtime — rebuilds the scene targets and every PSO. |
| **Global Illumination** | **Implemented.** Voxel cone tracing: voxelise+inject -> mip filter -> diffuse gather + AO, cone count tier-derived (Low 3 / Medium 6 / High 9 / Epic 13 — this row said a fixed "6-cone gather" for a long time; Medium kept the old hardcoded 6 as its default, but the other three tiers now spend or save real cost on it). |
| **Ray Tracing** | **Implemented.** DXR 1.1 inline `RayQuery` sun shadows (exact, hard-edged). Needs DXR 1.1 + SM 6.5. |
| **Mesh shaders** | **Implemented.** Replaces the input assembler, and removes the geometry shader from voxelisation. Needs mesh-shader tier 1 + SM 6.5. The DEVICE owns the toggle; Voxi only reports and stores it. |
| **Path Tracing** | **Implemented — this row said "Declared... not built yet" for a long time, and `Voxi.cpp`'s own `status()` comment now names that as the mistake.** `modules/render.pt` (`PtSceneView`) is a real, built path tracer over DXR 1.1 inline `RayQuery`; `Feature::PathTracing` used to hardcode `Status::NotImplemented` here even after `PtSceneView` existed, which left the settings page permanently greyed out and clamped `Settings::pathTracing` to Off on every device regardless of hardware. It now mirrors the tracer's own capability gate (`RaytracingTier>=11`, SM 6.5, DXC, compute shaders) and reports `Ready` when they hold. |

**MSAA, GI and the shadow map ask for shader model 5.1**, so they compile under FXC and work on a
machine with no `dxcompiler.dll` at all — measured bit-identical to the SM 6.6 hardware path at every
oracle gate. Only ray tracing and mesh shaders need SM 6.5, and they say so at their own call sites.

## Honest status reporting

Every feature reports one of `Ready` / `NotImplemented` / `Unsupported`, and the setter refuses
values it cannot honour. The editor greys out anything that is not `Ready`, so a toggle is never
shown as available when it would silently do nothing. When the GI/RT/PT passes land, only
`Renderer::status()` changes — the UI and the C# bindings pick it up for free.

A refusal is also **logged**, once per feature per device, naming the capability that is missing —
because a request refused in silence (`--ms` on a device without mesh shaders) is indistinguishable
from a request never made. Only a feature that was actually asked for is reported.

## Layering — two targets, and the split is load-bearing

| Target | Kind | Links | Holds |
|---|---|---|---|
| `Aver.Render.Voxi` | SHARED | **Aver.Core only** | the settings, the status reporting, the C ABI |
| `Aver.Render.Voxi.Renderer` | STATIC | Core, **`Aver.RHI`** (never `Aver.RHI.D3D12`) | every GPU resource and pass |

The DLL knows nothing about the RHI: the host pushes `DeviceInfo` in (mirroring `rhi::DeviceCaps`)
and reads `Settings` back out. That is what keeps render-hardware types off the P/Invoke boundary
and lets it ship as a shared library.

The renderer is a second target rather than a second file so the "no backend type crosses this
boundary" rule lives on a link line, where the linker enforces it, instead of in a comment. It
implements `rhi::IRenderFeature` and talks only to `IResourceFactory` / `IRenderContext` from
`modules/rhi/include/aver/rhi/RHIResources.hpp`. Nothing in it names a D3D12 type.

`VoxiRenderer::init` declines cleanly when the backend has no GPU support: `IDevice::resources()`
returns `nullptr` (the Null backend, and D3D11/Vulkan which never construct a device at all), so it
logs and returns `false`, and the engine runs without the feature.

Building with `-DAVER_MODULE_VOXI=OFF` still configures, compiles, links and renders — but the
image is **unshadowed with no GI**, because those are Voxi's and the backend has no copy. That is a
deliberate consequence of the decoupling, not a regression; `docs/STATUS.md` §4c-2 records the
expected probe values for that configuration.

## C# scripting

Built **SHARED** so the scripting layer can P/Invoke it. The stable surface is the plain-C ABI in
`include/aver/voxi/voxi_abi.h` (`aver_voxi_*`), bound by `scripting/csharp/Aver.Scripting`:

```csharp
Voxi.Msaa = 8;                                  // clamped to what the GPU supports
if (Voxi.IsAvailable(VoxiFeature.GlobalIllumination))
    Voxi.GlobalIllumination = VoxiQuality.High; // refuses while NotImplemented
Console.WriteLine(Voxi.StatusTextOf(VoxiFeature.RayTracing));
```

Verify the binding end-to-end with:

```
dotnet run --project scripting/csharp/Aver.Scripting.Sample
```

**Caveat:** a standalone C# process loads its *own* copy of the DLL, so it sees default (empty)
device caps and its settings are not the editor's. Driving the live editor from script needs the
CLR hosted in-process — the ABI is already shaped for it, that host just doesn't exist yet.

## How the GI works

1. **Voxelise + inject (one pass).** The scene is rasterised with no render target; the dominant
   axis is chosen per triangle by `GSVoxel` or, on the mesh-shader path, by `MSVoxel` — and the
   pixel shader computes direct sun + sky lighting and *adds* it into an accumulator. Merging
   voxelisation with light injection avoids a second full scene traversal. Conservative
   rasterisation is used when the device reports it, so thin geometry still lights a voxel.
2. **Resolve (compute).** `CSResolve` divides the accumulated sums by the fragment count and writes
   the filterable `RGBA16F` volume. See *Order-independent injection* below for why the
   accumulator exists at all.
3. **Mip filter (compute).** `CSMip` box-filters each level into the next. Mip N is the cone
   footprint at distance N, which is what lets one sample stand in for a whole cone step.
4. **Cone trace (INSIDE the lit pixel shader).** A tier-derived number of cones over the hemisphere —
   3 at Low, 6 at Medium (one along the normal, five in a ring — this was every tier's fixed count
   before the GI quality ladder existed), 9 at High, 13 at Epic — march the volume, widening with
   distance and reading a coarser mip each step, composited front-to-back. The alpha that accumulates
   doubles as ambient occlusion.

Step 4 is a whole pixel shader (`PSMainVoxi`), not an extra pass: sun visibility and indirect
radiance are *arguments* to the shared `shadeSurface`, and a term inside the shading is not
something any arrangement of passes can express. Voxi therefore owns four scene lit pipelines —
IA/mesh-shader crossed with shadow-map/RayQuery — and hands the right one to the backend through
`IRenderFeature::scenePipeline`.

## Order-independent injection

Several triangles legitimately cover one voxel: the cube's bottom face is coplanar with the ground
quad, and every box edge puts two differently-lit faces in the same cell. A plain
`gVoxelUAV[c] = radiance` store resolves that contest by whichever fragment retires last, which the
GPU does not promise to keep stable — so the volume was rebuilt to a *different* answer on most
frames and the GI probe visibly wobbled.

Injection sums into a fixed-point atomic accumulator instead (`R32_UINT`, the only typed format
D3D12 guarantees atomics on, four channels interleaved along x with the fourth a fragment count),
and `CSResolve` divides by that count. The result depends only on WHICH fragments covered a voxel,
never on their order, because integer addition is associative where last-writer-wins is not. A
contested voxel now holds the mean of the surfaces covering it, which is also a better answer than
an arbitrary winner.

Draws are replayed into the volume at the start of the *next* frame, so the volume is one frame
old. That is imperceptible and avoids restructuring the app's submission order.

**Debug view:** viewport `Lit` dropdown -> *Voxel Radiance (GI debug)* raymarches the volume
straight to screen. Use it first whenever GI looks wrong - it separates "voxelisation is broken"
from "cone tracing is broken".

Dev flags: `--gi`, `--gi-debug`.

## Shadowed injection

A 2048^2 directional shadow map is rendered from the sun before voxelisation, and sampled with 3x3
hardware PCF in BOTH the lit pass and the voxelisation pass. Shadowing the injection matters: a
surface in shadow must not emit sun radiance into the volume, or bounce light leaks through walls
and shadowed areas glow. Shadow maps are core feature-level 11_0 - deliberately chosen over
ray-traced shadows so this works on every DX12 GPU rather than only RT-capable ones.

## Portability (all DX12 GPUs, AMD + NVIDIA + Intel)

Baseline is feature level 11_0. Optional features are queried and gated, never assumed:

| Feature | Gate | Fallback |
|---|---|---|
| Conservative rasterisation | `ConservativeRasterizationTier` | Standard raster; thin geometry may miss voxels |
| MSAA 2x/4x/8x | `MULTISAMPLE_QUALITY_LEVELS` per count | Drops to the highest supported, or off |
| Ray tracing | `OPTIONS5.RaytracingTier` | Reported `Unsupported`; setting refuses to enable |
| `IDXGIFactory6` | `QueryInterface` | Falls back to `EnumAdapters1` (Win10 pre-1803) |

Deliberate correctness choices for cross-vendor behaviour:
- **All descriptor-heap slots are null-filled, by declared KIND.** Resource Binding Tier 1 hardware
  (NVIDIA Kepler / Maxwell gen 1, Intel Haswell/Broadwell) requires every descriptor in a bound
  table to be valid even when the shader ignores it — and a null descriptor whose dimension
  disagrees with what the shader declared is undefined too. That is why `BindingSetDesc` carries a
  `SlotKind` per slot rather than a count alone: counts cannot supply the dimension.
- **Every pass binds every declared table**, for the same Tier 1 reason, read or not.
- **All declared root CBVs are given an address** by the backend at `setPipeline`. Leaving one
  unset is undefined behaviour the debug layer cannot see — it validates API use, and this is not
  API misuse. It hung this RDNA part outright (`0x141` TDR) on the IA+GS voxelise pipeline while
  the mesh pipeline merely read zeros. Same defect, two symptoms.
- **The radiance volume uses per-mip resource transitions.** The mip filter reads level m-1 through
  a single-mip SRV while writing level m as a UAV; a whole-chain SRV would demand every mip be in
  the read state at once. Getting this wrong is undefined behaviour that renders correctly on one
  vendor and corrupts on another.

Voxelisation has a **GS-free variant**: `MSVoxel` makes the dominant-axis choice per primitive in a
mesh shader, so the geometry stage disappears entirely. That matters because GS is core 11_0 and
runs everywhere, but is emulated through an off-chip ring buffer on all AMD GCN parts and is
markedly slower there. The GS path remains for hardware below mesh-shader tier 1, and the two are
pixel-identical.

## Ray tracing (DXR 1.1 inline)

Ray-traced sun shadows via `RayQuery` traced straight from the pixel shader - no state objects,
no shader binding tables, no `DispatchRays`, so it drops into the existing raster pipeline.

- One BLAS per mesh, built lazily on first use, and rebuilt every frame for a mesh whose vertices
  are written by compute. A mesh that cannot produce one is remembered as a zero, so a failure is
  not retried and re-logged every frame. **Once per mesh per frame, not once per draw**: the draw
  list holds one entry per instance, so a mesh drawn twice used to be rebuilt twice — the second
  build recomputing the identical structure over the identical vertices.
- `[Voxi] bottom-level builds this frame: ...` is printed whenever that count changes. It is the
  only place the cost is visible, and the only place a wrong "is this mesh dynamic" answer shows up
  at all: a static scene that quietly rebuilds everything every frame looks identical on screen.
- TLAS rebuilt each frame from the same replayed draw list the shadow and voxel passes use. It is
  CREATED up front, sized for the draw-list cap, so its descriptor can be written into `t2` before
  any frame is recorded — a shader-visible descriptor an in-flight frame may be reading must not be
  rewritten.
- A second pixel-shader variant is compiled at `ps_6_5` with `-D AVER_RT=1`; the default variant
  asks only for **SM 5.1**, so a device without DXR — or without a DXIL compiler at all — still gets
  a working renderer and falls back to the shadow map.
- **The sun-shadow ray no longer uses `ACCEPT_FIRST_HIT_AND_END_SEARCH` by default — this line said
  it did, and that stopped being true once transmissive shadows landed.** `modules/render.voxi/
  shaders/voxi.hlsl`'s shadow ray now runs `RAY_FLAG_NONE` and its own traversal loop, multiplying a
  running transmittance through every translucent surface it crosses so a pane of glass attenuates a
  shadow instead of stopping it outright — deliberately dropping the early-out a closest-hit-free
  visibility query gave it. The flag is still there, but only under a measurement-only ablation
  (`AVER_RD_ABL_SHADOW_FIRSTHIT`) that restores the old opaque-only, stop-at-first-hit behaviour to
  price what was given up; it must never be wired to a real quality tier. **The `--rt-rays`
  per-ray-cost table below predates this change and was measured against the cheaper,
  `ACCEPT_FIRST_HIT`-enabled ray — the real per-ray cost today is higher and has not been
  re-measured.**
- **The FEATURE decides whether a ray may be traced**, publishing it as `gShadowParams.z`. Tracing
  an unbuilt or empty structure is not an error anyone can see: RayQuery reports no hit for every
  pixel, i.e. a fully lit scene, with nothing for the debug layer to say. The guard belongs where
  the fact is known, not in a pipeline choice a backend would have to keep in step.

Engine matrices are row-vector (`v*M`); DXR instance transforms are 3x4 column-vector, so the
upper 3x3 is transposed on the way in — **by the backend**, from the engine-convention matrix Voxi
hands over untouched. Getting that wrong leaves the raster image perfectly correct and puts every
ray somewhere else, which only a cast-shadow probe can see.

Also note: `IResourceFactory` has `createBlas`/`createTlas` but no matching destroy, so
acceleration structures are released only when the factory is. Harmless while meshes are static;
it needs an answer before geometry becomes dynamic.

### Sun shadow rays: nested, and priced

`--rt-rays N` (1..32, default 4) sets the occlusion rays per pixel. The sample sequence is
**nested** — sample *k* is at the same place whatever *N* is — so a sweep over ray counts is one
converging series rather than a set of unrelated images. `tests/render.voxi` decides that property
by arithmetic, and keeps the sequence it replaced around as a negative control so the check is seen
firing on every run.

Measured on this machine (RX 7800 XT, Debug tree, editor scene at 2750x1639, `--no-vsync`,
`--frame-time`, median of 370 frames, three runs per point spread ≤ 0.03 ms):

| rays | 1 | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|---|
| frame | 4.510 | 4.591 | 4.693 | 4.914 | 5.386 | 6.265 ms |

Linear at **~0.056 ms per ray per frame**, so the default 4 rays cost 0.22 ms of a 4.69 ms frame
and ray tracing as a whole costs 0.45 ms over the cascade-shadow path's 4.24 ms. `--frame-time`
measures the WHOLE frame's CPU period, not Voxi's share of it, and means nothing without
`--no-vsync` — with vsync on every reading is the refresh interval.

### Refit: what the RHI would need

Every build here is a full `PREFER_FAST_TRACE` build, the most expensive mode there is, and a
compute-skinned mesh pays it every frame. DXR can refit instead, at a fraction of the cost, and the
RHI cannot express it. Two verbs would be needed, and neither is a wrapper over what exists:

1. **`createBlas(MeshHandle, BlasUsage)`** — `ALLOW_UPDATE` has to be in the flags of the *prebuild
   query* and of the first build. It cannot be added later: it changes `ResultDataMaxSizeInBytes`,
   and the scratch buffer must then be sized by `max(ScratchDataSizeInBytes,
   UpdateScratchDataSizeInBytes)`. A refit against a structure built without it is invalid.
2. **`IRenderContext::refitBlas(BlasHandle)`** — records a build with `PERFORM_UPDATE` set and
   `SourceAccelerationStructureData = DestAccelerationStructureData` (DXR permits the in-place
   form), same geometry desc, same UAV barrier after.

With the contract stated rather than implied: a refit is only valid while the topology is
unchanged — same index buffer, same counts, same geometry flags — which is exactly the
compute-skinning case, and its quality degrades as the pose drifts from the one it was built at, so
a caller must still rebuild fully from time to time. Faking either verb by simply passing
`PERFORM_UPDATE` to today's `buildBlas` would produce a debug-layer error at best and a silently
wrong structure at worst.

**DXR 1.1 inline ray tracing (`RayQuery`) only.** AMD has never shipped a
Tier-1.0-only GPU (it entered at 1.1 with RDNA 2), Intel entered at 1.1 with Arc, and every
Turing-or-later NVIDIA part reports 1.1 - so supporting DXR 1.0 as well would add only NVIDIA
Pascal/Volta and GTX 16-series, which expose DXR through driver emulation with no RT cores and run
about an order of magnitude slower. Inline RayQuery also needs no state objects, shader binding
tables or `DispatchRays`, so it composes with the existing raster pipeline.

Gate on the capability (`RaytracingTier >= 1.1`), not on feature level 12_2: D3D12 Ultimate also
demands mesh shaders and sampler feedback this renderer does not use.

Note "works on all DX12 GPUs" is impossible for ray tracing by construction - no AMD GCN or RDNA 1
part, no pre-Turing NVIDIA and no pre-Arc Intel has RT hardware. The correct behaviour there is what
Voxi already does: report `Unsupported` and refuse the setting.

Full hardware matrix and launcher-ready spec text: `docs/MINIMUM_SPECS.md`.

## Next

Temporal accumulation to soften flicker; a cascaded volume for large scenes; cone-traced specular
(diffuse + AO only today); RT ambient occlusion and reflections, which mostly reuse the TLAS that
already exists. Open items are tracked in `docs/STATUS.md` §4d.
