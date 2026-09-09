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

## What does not run yet, and what stands in the way

**Nothing records these dispatches.** Three things are missing, and they are stated here rather than
left to be discovered:

1. **`Aver.RHI` cannot make a shader from bytecode.** `rhi::ShaderDesc` takes HLSL *text* and
   compiles it through DXC at runtime (`modules/rhi/include/aver/rhi/RHIResources.hpp`). NRD hands
   over pre-compiled DXIL and SPIR-V. A precompiled path has to exist before a single NRD pipeline
   can be created.

2. **`rhi::PipelineLayout` cannot express NRD's register spaces.** Measured, from `NrdLinkTest`'s
   own output: NRD wants its **SRVs and UAVs in space 0** (which matches Aver) and its **constant
   buffer `b0` and two immutable samplers `s0` in space 1** (which does not — Aver puts every
   register in space 0, and its space 1 is already the bindless texture table, the one place
   `D3D12Device.cpp` sets `RegisterSpace` explicitly). These spaces are baked into the compiled
   bytecode, so they are not negotiable; NRD's dispatches need a root signature of their own rather
   than a reused `PipelineLayout`. Also worth knowing before writing it: the entry point is `main`,
   the constant buffer is up to 912 bytes, and REBLUR_DIFFUSE_OCCLUSION uses 11 pipelines over a
   5-texture permanent pool and a 4-texture transient one.

3. **Nothing produces `IN_DIFF_HITDIST`.** The sky-occlusion ray in `modules/render.voxi` returns
   visibility and discards `CommittedRayT()`. The other three inputs map onto G-buffer targets the
   D3D12 backend already creates, but they are gated behind `gbufferEnabled_` and the normals need
   re-packing through `NRD_FrontEnd_PackNormalAndRoughness` to match the encodings `NrdLinkTest`
   prints.

Items 1 and 2 are in `Aver.RHI` and are shared plumbing; item 3 is Voxi's.
