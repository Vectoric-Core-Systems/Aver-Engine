# Aver.Render.Voxi

Voxi is Aver Engine's **optional render-feature module** — the first module gated behind a CMake
option (`-DAVER_MODULE_VOXI=OFF` and the engine builds and runs exactly as before).

It owns the renderer's project-wide **quality settings** and, crucially, decides from the real
device capabilities which of them can actually be used:

| Setting | State |
|---|---|
| **Anti-Aliasing (MSAA)** | **Implemented.** Off / 2x / 4x / 8x, applied at runtime — rebuilds the scene targets and every PSO. |
| **Global Illumination** | **Implemented.** Voxel cone tracing: voxelise+inject -> mip filter -> 6-cone diffuse gather + AO. |
| **Ray Tracing** | Declared. Blocked on DXC/SM6.x (see below); most DX12 GPUs have no RT hardware at all. |
| **Path Tracing** | Declared. Reference tracer; not built yet. |

## Honest status reporting

Every feature reports one of `Ready` / `NotImplemented` / `Unsupported`, and the setter refuses
values it cannot honour. The editor greys out anything that is not `Ready`, so a toggle is never
shown as available when it would silently do nothing. When the GI/RT/PT passes land, only
`Renderer::status()` changes — the UI and the C# bindings pick it up for free.

## Layering

Voxi depends on **Aver.Core only**. It deliberately knows nothing about the RHI: the host pushes
`DeviceInfo` in (mirroring `rhi::DeviceCaps`) and reads `Settings` back out to drive the device.
That keeps the module a DAG leaf and lets it ship as a shared library.

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

1. **Voxelise + inject (one pass).** The scene is rasterised with no render target; a geometry
   shader projects each triangle along its dominant axis and the pixel shader computes direct
   sun + sky lighting and writes radiance straight into a `RWTexture3D`. Merging voxelisation
   with light injection avoids a second full scene traversal. Conservative rasterisation is used
   when the device reports it, so thin geometry still lights a voxel.
2. **Mip filter (compute).** `CSMip` box-filters each level into the next. Mip N is the cone
   footprint at distance N, which is what lets one sample stand in for a whole cone step.
3. **Cone trace (lit pass).** Six cones over the hemisphere - one along the normal, five in a
   ring - march the volume, widening with distance and reading a coarser mip each step, composited
   front-to-back. The alpha that accumulates doubles as ambient occlusion.

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
- **All descriptor-heap slots are null-filled.** Resource Binding Tier 1 hardware (NVIDIA Kepler /
  Maxwell gen 1, Intel Haswell/Broadwell) requires every descriptor in a bound table to be valid
  even when the shader ignores it - and the voxel slots are empty whenever GI is off.
- **Every pass binds both descriptor tables**, for the same Tier 1 reason.
- **The radiance volume uses per-mip resource transitions.** The mip filter reads level m-1 through
  a single-mip SRV while writing level m as a UAV; a whole-chain SRV would demand every mip be in
  the read state at once. Getting this wrong is undefined behaviour that renders correctly on one
  vendor and corrupts on another.

Known cost: voxelisation uses a geometry shader for dominant-axis projection. GS is core 11_0 and
runs everywhere, but is emulated through an off-chip ring buffer on all AMD GCN parts and is
markedly slower there. A GS-free variant (three instanced passes) is the fix if that bites.

## Ray tracing - why it is not done yet

DXR cannot be expressed in shader model 5.1. Inline ray tracing (`RayQuery`) needs SM 6.5 and a
full RT pipeline needs `lib_6_3`; this backend compiles HLSL with `D3DCompile` (FXC, SM 5.1), so
DXR needs the shader pipeline moved to **DXC** first, plus BLAS/TLAS acceleration structures.

**Target when it lands: DXR 1.1 inline ray tracing (`RayQuery`) only.** AMD has never shipped a
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

DXC migration then inline RayQuery; temporal accumulation to soften flicker; clearing stale voxels
so moving objects do not leave trails; a cascaded volume for large scenes.
