# NeuRAA and NRD: neural anti-aliasing and neural denoising (Aver) -- design

**NeuRAA** = Neural Realtime Anti-Aliasing. **NRD** = Neural Realtime Denoising, a hybrid of the AMD
FidelityFX Denoiser the engine already runs and a small neural network.

**Status:** design only (2026-10-05). Nothing is built. Every millisecond below is an estimate unless
it says *measured*.

**One-line summary:** a small per-tile network predicts the *parameters* of a filter we already have
(the TAAU resolve for NeuRAA, FidelityFX's passes for NRD); it never paints pixels. The filter stays
the same hand-written, bounded, energy-checked code, so a bad prediction can only pick a worse setting
of a known-good filter, never invent an image, and without weights the engine renders exactly as today.

---

## 1. Why parameters, not pixels

Both problems are the same shape, and the measurements of 2026-10-04/05 show it:

- **The denoiser:** FidelityFX's constants are global. Its radiance-difference weight at the vendored
  0.6 returned 0.49 of the GI it was given; at 0 it kept 0.79 by day but turned night path tracing into
  blotches in motion; 0.2 is a compromise (`DENOISING.md` section 2). One number cannot be right both
  for a tile of rare fireflies and for a tile of genuinely bright bounce light.
- **TAA:** the resolve's clip box (1.25 sigma), blend weight (0.1 to 0.25) and reconstruction kernel are
  global too, and smeared under motion badly enough that TAA now runs only while the camera is still
  (`modules/render.sr/README.md`).

A network that looks at a tile's statistics and picks those numbers per tile addresses exactly that.
Predicting parameters rather than pixels also:

- **fits the hardware.** The dev GPU is an RX 7800 XT on D3D12, which has no matrix-core path
  (`modules/render.neural/README.md`: fp32 portable kernels; D3D12 LinAlg is SM 6.10, Vulkan cooperative
  matrix comes after Vulkan parity). An image-to-image convolutional network per output pixel is out of
  budget. NeuraFI measured 1.75M records at 3.5 ms (*measured*, its network size), so per-pixel at
  render resolution is too slow; one record per 4x4 tile at 1766x994 is ~110k records, roughly 0.2-0.3
  ms per small network (estimate).
- **reuses what exists.** `Aver.Render.Neural`'s `Mlp` (fp32, inputs <= 64, outputs <= 16, width <= 64,
  layers <= 6, AVNN weight files, in-engine Adam) needs no new layer types for this.
- **follows NeuraFI's rule:** the network predicts a correction to an analytic baseline, the baseline is
  what runs until weights exist, and a gate keeps the network out where it measures worse.
- **is a different shape from the patent-dense prior art** (section 7).

---

## 2. Shared structure

```
G-buffer + signal  ->  [feature pass]  ->  tile records (StructuredBuffer<float>)
                                              |
                                        Mlp::recordInfer (EMA weights)
                                              |
                       per-tile params  ->  [param texture, 1/4 res, bilinear]
                                              |
                       the existing filter reads its knobs from it, per pixel
```

- **Feature pass:** one compute pass per consumer, one thread group per 4x4 tile, writing a fixed
  feature vector per tile (section 3 and 4 list them). Features are normalised to O(1) (log-luminance,
  ratios to the tile mean, motion in pixels / 16) so one set of weights serves every scene brightness.
- **Parameter texture:** RGBA16F at quarter resolution; the filter samples it bilinearly so parameters
  never step at tile edges.
- **Bounded outputs:** every output goes through a sigmoid mapped to a hand-chosen safe range (for
  example NRD's radiance weight in [0, 0.8]). The network cannot leave the range the filter was tested
  in.
- **Fallback:** with no weights loaded, or with the gate closed, the parameter texture is cleared to
  today's constants. That path is bit-identical to the current renderer and is what ships first.

---

## 3. NeuRAA

### What it drives

The TAAU resolve in `modules/render.sr/shaders/sr_taa.hlsl`, unchanged in structure:

| parameter | today | NeuRAA range |
|---|---|---|
| history blend weight alpha | 0.1-0.25, scaled by `nmax` | [0.02, 1] |
| variance clip width (sigma) | 1.25 | [0.5, 4] |
| reconstruction sharpness (kernel scale) | fixed | [0.5, 2] |
| RCAS sharpening | editor setting | [0, setting] |

### Features per tile (~16 floats)

Motion length and its spread across the tile; depth discontinuity (max relative dz); disocclusion
fraction (pixels whose history position fails the depth test); the neighbourhood YCoCg mean and sigma;
history-to-mean distance in sigmas (how much the current clip would cut); `nmax` (how close this
frame's nearest jittered sample landed); luminance contrast; mean roughness; frames since the history
last reset; and whether the tile is under the editor's chrome.

### Goal

TAA that holds up in motion, so the "only while still" rule can retire. That rule stays until NeuRAA
beats it on the moving-camera rig (section 6).

### Training

The resolve is not differentiable through `Mlp`, which trains records against targets. So training
is two steps:

1. **Oracle parameters.** For a captured frame, per tile, search the parameter range (a coarse grid,
   then a local refine) for the setting whose resolved tile is closest to the reference. The
   reference is the same camera pose accumulated over 64 jittered frames at the same render scale.
2. **Regression.** Train the MLP to map the tile's features to its oracle parameters.

Loss: relative L2 against the reference, plus a temporal-stability term (frame-to-frame change not
present in the reference) and a ghosting term (energy left behind a moving object's previous
position). Captures come from scripted camera paths (`--cam-wobble`, `--cam-translate`) on NeonDistrict
Day and Night, NewSponza and the test levels, all run in a visible window.

Weights ship in `bin/data` as an AVNN file, as NeuraFI's do. An optional on-device refinement can use
the at-rest accumulation as its live reference, gated like NeuraFI's.

### Cost (estimate)

Feature pass ~0.15 ms, inference ~0.2 ms, resolve unchanged plus one bilinear fetch: ~0.4 ms at render
scale 0.5 on the RX 7800 XT.

---

## 4. NRD

### What it drives

FidelityFX's reflection pipeline in `aver_denoise.hlsl`, which already supplies its own copy of the
FidelityFX config. Today those are constants; under NRD three of them become per-thread values read
from the parameter texture (a `static` set at the top of each entry point, which the config macros
name, so the vendored headers stay untouched), and the clip weight, which FidelityFX already takes as
an argument, is passed per pixel:

| knob | today | NRD range |
|---|---|---|
| `RADIANCE_WEIGHT_BIAS` (firefly rejection) | 0.2 | [0, 0.8] |
| history clip weight | 4 | [0.5, 6] |
| accumulation speed / max samples | 32 | [4, 64] |
| `PREFILTER_VARIANCE_BIAS` (spatial spread) | 0.1 | [0.1, 1] |

### Features per tile (~20 floats)

Sample count; temporal and spatial variance; luminance relative to the 8x8 mean; **tail heaviness**
(the fraction of the tile's energy carried by samples above 4x and 16x the mean, the quantity that
separated "fireflies" from "real night light" in the 10-05 measurements); motion length; disocclusion
fraction; normal and depth gradients; roughness; the signal (GI or sky occlusion) and the GI method
(ReSTIR GI, ReSTIR PT).

### Ground truth

- **ReSTIR path tracing:** the reference path tracer (`ptMode` 1) already accumulates a converged
  image at a still camera (a running mean of up to 1,024 frames). That is the target.
- **ReSTIR GI:** the same estimator accumulated at rest over a static scene.

### Training

The same oracle-then-regress scheme as NeuRAA, with three loss terms chosen from what went wrong this
week:

- **Error:** relative L2 against the reference.
- **Energy:** the tile's mean against the reference's mean. This is the term that stops the network from
  learning to darken, which is what every global setting did (0.49-0.84 of the true GI).
- **Blotching:** temporal flicker and the spot metric from the 10-05 rig (`--cam-wobble 30 16`).

### Cost (estimate)

Feature pass ~0.15 ms, inference ~0.2 ms, denoiser unchanged: ~0.35 ms.

---

## 5. Where the code goes

- `modules/render.neural`: unchanged for v1.
- `modules/render.sr`: `NeuRaa` beside `TemporalUpscaler` (feature pass, parameter texture, the resolve
  reading it). Setting `RENDER.NEURAA`, `--neuraa 0|1`, Display > Anti-aliasing.
- `modules/render.denoise`: `Nrd` beside `Denoiser` (feature pass, parameter texture, the config statics).
  Setting `voxi.nrd`, `--nrd 0|1`, the denoiser page.
- A capture mode (`--neuraa-capture` / `--nrd-capture`) that writes feature records and references, and
  an offline oracle-search and training tool built on `MlpReference` (CPU) or `Mlp::recordTrain` (GPU).

---

## 6. How it is judged

Every comparison runs in a visible window on fixed camera paths, with the linear metrics built on
2026-10-04/05:

- **NRD:** GI-only linear mean against the reference (bias), the spot metric in motion, and temporal
  flicker. It ships only if it beats today's constants on all three, by day and by night.
- **NeuRAA:** error against the 64-frame reference on edges, temporal flicker, and ghost-trail length
  behind a moving object. It replaces "TAA only while still" only if it beats both that rule and plain
  TAA in motion.

---

## 7. Patents and licences

Neural anti-aliasing / upscaling and neural denoising are patent-dense areas (vendor super-resolution
and frame-generation families, and kernel-predicting denoiser families among them). This design is
deliberately a different shape: a tiny network predicting a few bounded parameters of a hand-written
filter per tile, not an image-to-image network and not per-pixel filter kernels. Published prior art
for that shape exists (Kalantari et al. 2015 predicted per-pixel filter parameters with an MLP for
Monte Carlo denoising). As with NeuRaC and NeuraFI (`NEURAFI_PATENTS.md`), **counsel should review
before either ships**; this section is a flag, not legal advice. Nothing is vendored: the network code
is `Aver.Render.Neural`, the filters are the in-house TAAU and the MIT FidelityFX Denoiser.

---

## 8. Phases

1. **Plumbing, no visual change.** Feature passes and parameter textures, filled with today's constants.
   Verified bit-identical.
2. **Capture and oracle.** The capture mode, the reference accumulation and the oracle search. This
   alone answers how much is available: oracle parameters are the ceiling any network can reach.
3. **Train and ship NRD** (the measured problem is sharper there), with weights in `bin/data` and the
   gate.
4. **Train and ship NeuRAA**, then retire "TAA only while still" if it measures better.
5. **Optional on-device refinement** at rest, behind the gate.
