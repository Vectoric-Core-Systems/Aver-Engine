# Aver.Render.PBR

**PBR is a material system. Voxi is the thing that renders it.**

That sentence is the architecture. This module owns what a surface *is* — its factors, its texture
references, how it blends — and, in its sibling static target, the BRDF that shades it. It is **not**
an `rhi::IRenderFeature`: Voxi remains the only registered feature, and it *composes* the material's
shading model into its own shaders rather than the material system inserting a pass of its own.

## Layering — two targets, and the split is load-bearing

| Target | Kind | Links | Holds |
|---|---|---|---|
| `Aver.Render.PBR` | SHARED | **Aver.Core only** | `MaterialDesc`, `MaterialLibrary`, the C ABI |
| `Aver.Render.PBR.Materials` | STATIC | Core, **`Aver.RHI`** (never `Aver.RHI.D3D12`) | the packed GPU constant block, and later the texture caches, fallback textures, per-material binding sets and `pbr::materialShaderPrelude()` |

The DLL must **never** include `aver/rhi/*`. That is the whole reason it can be P/Invoked: a
render-hardware type on that boundary breaks the scripting layer, and the restriction lives on a
link line where the linker enforces it rather than in a comment.

## Materials are instances

Voxi's ABI is a global settings block, so its calls take no subject. A material is an *instance*,
so `pbr_abi.h` is handle-based throughout: `aver_pbr_create` / `aver_pbr_destroy`, and every setter
takes the handle. Handles carry a generation (bits 20..30, never 0), so a reference to a destroyed
material fails `aver_pbr_valid` instead of silently addressing whatever reused its slot.

`MaterialLibrary::consumeDirty(h)` mirrors `voxi::Renderer::consumeMsaaDirty()`: reading the flag
clears it, so exactly one consumer acts on each change.

## Texture references

Each slot holds **both** an authoring path and an opaque 64-bit id, and this module interprets
**neither**. Resolving either one needs the asset system, which lives a tier up; holding both keeps
the DLL Core-only while staying forward-compatible with an `ObjectId` or an `.octex` GUID.

## Field names

`MaterialDesc`'s fields match the `.ocmat` PARAM names in `docs/formats/FORMAT_SPECS.md` §7
one-for-one (`baseColorFactor`, `metallicFactor`, `roughnessFactor`, `emissiveFactor`,
`normalScale`, `occlusionStrength`), and `textureSlotName()` spells the `TEX` slot names. The loader
that lands later is therefore a rename-free mapping rather than a translation table nobody can
audit.

## Honest status reporting

`aver_pbr_status` / `aver_pbr_status_text` report `Ready` / `NotImplemented` / `Unsupported` per
feature, exactly as Voxi does. **This used to say everything reports `NotImplemented`; that stopped
being true.** All eight declared features (`Factors`, `BaseColorMap`, `MetalRoughMap`, `NormalMap`,
`OcclusionMap`, `EmissiveMap`, `AlphaMask`, `AlphaBlend` — `Material.hpp`'s `Feature` enum) now report
`Ready` (`MaterialLibrary::status`, `modules/render.pbr/src/Material.cpp:191-234`): the b2 constant
block and the five maps are consumed by `averEvalMaterial()`, and alpha blending is a real
back-to-front blended pass (`IDevice::setDrawBlended` / `endFrame`'s per-frame sort). The `Ready`
verdict on `AlphaBlend` is deliberately narrow, and the source comment is explicit about the line: it
covers alpha **compositing** only — a blended surface is never voxelised, never in the ray-tracing
TLAS and never in the shadow map (the same exclusion drawMesh's opaque path already applies to any
translucent mesh), and `MaterialDesc`'s newer `ior`/`transmission` fields are carried into the alpha
computation but there is still no refraction: a window will show an unbent, translucent copy of what
is behind it, not a bent one. Reporting `Ready` beyond what is true would be precisely the lie the enum
exists to prevent, and that is exactly why this status is qualified rather than blanket.

## C# scripting

The stable surface is `include/aver/pbr/pbr_abi.h` (`aver_pbr_*`), bound by
`scripting/csharp/Aver.Scripting/Pbr.cs`:

```csharp
var m = Pbr.Create("carbon body");
m.RoughnessFactor = 0.45f;
m.SetTexturePath(PbrTextureSlot.BaseColor, "content/textures/carbon_bc.octex");
Console.WriteLine(Pbr.StatusTextOf(PbrFeature.BaseColorMap));   // "Ready" -- this used to say "not implemented yet"; it now renders
```

One deviation from the `voxi_abi.h` idiom, deliberate: a texture's opaque asset id crosses as
`int64_t`. An `ObjectId` or a GUID does not fit in 32 bits, and splitting it into halves would put
the burden of reassembling an identifier on every binding.
