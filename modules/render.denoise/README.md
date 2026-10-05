# Aver.Render.Denoise

The engine's spatio-temporal denoiser for Voxi's two noisy ray-traced signals: the ReSTIR GI
indirect radiance and the sky-occlusion hit distance.

The filter is AMD FidelityFX Denoiser, vendored at `third_party/fidelityfx-denoiser` under the MIT
licence. This module is the host around it: the compute pipelines, the history textures, the
G-buffer history copy and every resource-state transition.

It also holds `Nrd2` (`shaders/nrd2.hlsl`, `nrd2_resolve.hlsli`): NRD2 phase 1, a strictly
single-frame denoiser of Voxi Stage B's demodulated lighting with no history at all. Design and
what Stage B writes: `docs/rendering/NRD2.md`, "Phase 1 as built".

## How it works

FidelityFX Denoiser ships two denoisers: shadows, which filters a 1-bit-per-pixel hit mask, and
reflections, which filters a noisy per-pixel radiance over opaque surfaces. Neither Voxi signal is
a hit mask, so this module drives the **reflection pipeline as a diffuse denoiser**: the host
callbacks in `shaders/aver_denoise.hlsl` report roughness 1 everywhere (every pixel glossy, none a
mirror) and a hit distance of zero, which collapses the pipeline's mirror-parallax reprojection
onto the surface's own motion vector.

Three compute passes per signal, each compiled in a colour and a one-channel variant:

| pass | what it does |
|---|---|
| reproject | reprojects last frame's result, accumulates a sample count and a temporal variance, and reduces the noisy input to an 8x8 average |
| prefilter | variance-guided, depth/normal edge-stopped spatial filter of the noisy input |
| resolve | blends the prefiltered signal into the neighbourhood-clipped history; the result is the output and next frame's history |

Half-rate ReSTIR GI (Voxi's `rayDrivenStages 2`) traces only one pixel of each checkerboard pair per
frame. The input load reconstructs every skipped pixel as the mean of its four traced neighbours
before the passes run.

## Contract

- **Inputs** are last frame's G-buffer (view Z, motion vectors in pixels, the normal packed by
  `averPackNormalRoughness` in `modules/render.voxi/shaders/voxi.hlsl`) and last frame's noisy
  signal, because the denoiser records before this frame's scene pass.
- **History is per signal.** A signal that did not run last frame restarts its own history.
- **Outputs** rest in `ShaderResource`; `output()` returns zero for a signal that did not run, and a
  caller must read zero as "not denoised this frame".
- **Runtime-compiled HLSL**, no offline shader build. FidelityFX's headers are written against
  HLSL 2018 and are vendored unmodified, so these compiles opt into 2018 with the `AVER_HLSL_2018`
  flag-define, which both shader compilers consume.
- The G-buffer exists only on D3D12, so that is where it runs today. The module itself uses no
  register spaces or other backend-specific binding.
