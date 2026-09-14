# Aver.Render.NRD

The engine's seam onto [NVIDIA Real-Time Denoisers](https://github.com/NVIDIA-RTX/NRD), vendored at
`third_party/nrd`.

**NRD is not permissively licensed.** It ships under the NVIDIA RTX SDKs License, unlike everything
else in `third_party/`. Read `third_party/nrd/AVER_README.md` before depending on this — the
flow-through consequence for anyone shipping a product built on Aver is real and is recorded there.

## What this module is

NRD never touches a GPU. It is a *planner*: you tell it what the camera did, and it returns a list of
compute dispatches — pipeline index, resource slots, constant bytes, thread-group counts — for
someone else to record. This module owns that conversation and translates both directions, so the
rest of the engine speaks `rhi::Format` and `SlotRole` and never sees an NRD header.

That translation boundary is not decoration. NRD's headers carry the licence above, so they are
`PRIVATE` to `NrdDenoiser.cpp` and never reach a consumer's include path.

**The target exists in every configuration**, including one where NRD itself does not.
`cmake/AverNRD.cmake` turns `AVER_WITH_NRD` off by itself on a machine with no offline shader
compiler; a module that disappeared with it would break consumers' link lines on exactly the
machines least able to diagnose that. Instead `Denoiser::create()` returns false, and the caller
takes the fallback path it needs anyway.

## What runs today

`NrdLinkTest` (`tests/render.nrd`) exercises the whole CPU half with no device: the library version
matches the header, 159 shader permutations are embedded, an instance is created, the binding model
and the texture pools are reported, and two consecutive frames each produce a valid dispatch plan
naming `IN_VIEWZ` / `IN_MV` / `IN_NORMAL_ROUGHNESS` / `IN_DIFF_HITDIST` → `OUT_DIFF_HITDIST`.

## The three blockers are closed

All three were fixed on 2026-09-09. What each needed, and what it cost:

**1. `Aver.RHI` could not make a shader from bytecode.** `rhi::ShaderDesc` now carries an optional
`bytecode` / `bytecodeSize` pair; setting it bypasses source, entry point, defines and shader model,
all of which are meaningless for something already compiled. The bytes are **copied**, so a caller
may free them the moment `createShader` returns. D3D12 stores them beside the DXC blob and both go
to the PSO through one accessor; Vulkan validates the SPIR-V magic number and word alignment, then
builds the module directly — its existing `if (s.source.empty()) return s.module;` in
`moduleForLayout` already does exactly the right thing for a module that cannot be re-patched.

**2. `rhi::PipelineLayout` could not express NRD's register spaces.** It now has `constantSpace` and
`samplerSpace`, both defaulting to 0. **D3D12 honours them.** The assignment writes into a
zero-initialised `D3D12_ROOT_PARAMETER`, so a layout leaving them at 0 serialises to the bytes it
always did; both fields are part of the root-signature cache key, because two layouts differing only
in a space must not share one.

**Vulkan refuses them, deliberately and loudly.** An HLSL register space is a Vulkan *descriptor
set*, and a set is a real allocation and layout-compatibility unit — honouring one means building a
different set layout *and* binding it at a different index in `VulkanRenderContext`, which hard-codes
`kVkSetConstants`/`kVkSetSamplers` at every bind site. Wiring the first half without the second gives
correctly-built descriptors bound at the wrong set: no validation error (there are no
validation-layer binaries on the machines this is developed on), no crash, just a shader reading the
wrong memory. And it would still not run NRD, whose SPIR-V carries ShaderMake's own register shifts —
consuming that needs the module's real bindings read back by reflection, which is a different
mechanism, not a parameter. So `descriptorLayout()` returns null and says why.

**3. Nothing produced `IN_DIFF_HITDIST`.** `rtAmbientTraced` computed a hit distance for its voxel
lookup and discarded it; `AverAmbientTraced` now carries it, and `rtSkyOcclusionTemporal` writes it
to a new R16Unorm target (`u5`, `VoxiRenderer::ambientHitDistanceTexture()`).

Two decisions worth knowing before reading that code:

* **It is the raw per-frame measurement, not the accumulated one** — the opposite of the history
  write on the line above it. That history is consumed by this shader next frame and wants the
  average; this goes to an external denoiser that keeps its own history, and handing it a value
  already blended against ten previous frames would be feeding a filter its own output.
* **It is normalised by the ray's own `TMax` (`Settings::giMaxDistance`), not by any denoiser's
  curve.** Baking NRD's normalisation into the engine's shader would make the texture mean whatever
  the current consumer happens to be. A consumer wanting NRD's convention passes
  `hitDistParams = {giMaxDistance, 0, 1}`, which makes NRD's own normalisation the identity against
  this encoding.

It is allocated, bound and written under exactly the same condition as the sky-occlusion history
pair (`aoHistoryWanted()` — High and Epic only), so `gRtDenoiseParams.w` already speaks for it and
no fourth constant was added. On Low and Medium the ray does not run, the target does not exist, and
the accessor returns 0.

**Verified** by surfacing the value as the ambient term in a Sponza capture: contact darkening at
every wall/floor junction, bright down the open corridor — a geometry-correlated distance field, not
a constant. 110/110 headless suites pass.

## What still does not run

**Nothing records these dispatches.** That is now the only thing between this and pixels, and it is
one piece of work rather than three: a pass that creates NRD's 11 pipelines from the bytecode,
allocates its 5 permanent and 4 transient pool textures, binds the four inputs — `IN_VIEWZ`,
`IN_MV`, `IN_NORMAL_ROUGHNESS` from the G-buffer (`--gbuffer`, MSAA 1 only) and `IN_DIFF_HITDIST`
from `VoxiRenderer::ambientHitDistanceTexture()` — uploads up to 912 bytes of opaque constants per
dispatch, and reads `OUT_DIFF_HITDIST` back into the ambient term.

Two things it will have to deal with, both known:

* **The normals need re-packing.** `NRD_FrontEnd_PackNormalAndRoughness` with the encodings
  `NrdLinkTest` prints (normal 2, roughness 1); the G-buffer's own RGB10A2 encoding is `n*0.5+0.5`
  and is not the same thing. A mismatch here is silent — a plausible wrong image, not an error.
* **It is D3D12-only** until the Vulkan descriptor-set work above is done.
