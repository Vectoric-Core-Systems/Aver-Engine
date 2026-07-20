# Aver.Render.Voxi

Voxi is Aver Engine's **optional render-feature module** — the first module gated behind a CMake
option (`-DAVER_MODULE_VOXI=OFF` and the engine builds and runs exactly as before).

It owns the renderer's project-wide **quality settings** and, crucially, decides from the real
device capabilities which of them can actually be used:

| Setting | State |
|---|---|
| **Anti-Aliasing (MSAA)** | **Implemented.** Off / 2x / 4x / 8x, applied at runtime — rebuilds the scene targets and every PSO. |
| **Global Illumination** | **Implemented.** Voxel cone tracing: voxelise+inject -> mip filter -> 6-cone diffuse gather + AO. |
| **Ray Tracing** | Declared. Device reports DXR tier; no pipeline yet. |
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

## Next

Shadowed injection (the volume currently stores unshadowed direct light), temporal accumulation
to soften flicker, a cascaded volume for large scenes, and specular cones for glossy reflections.
