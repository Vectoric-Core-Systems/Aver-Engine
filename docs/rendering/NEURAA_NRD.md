# NeuRAA and NRD: neural anti-aliasing and neural denoising (Aver) -- design

**NeuRAA** = Neural Realtime Anti-Aliasing. **NRD** = Neural Realtime Denoising, a hybrid of the AMD
FidelityFX Denoiser the engine already runs and a small neural network.

**Status (2026-10-05):** phase 1 is in and the edge view was checked by the owner: the Neural Denoise
setting and NRD's resolve slot (running FidelityFX's resolve), NeuRAA's edge detection with the "Edge
Classes (NeuRAA)" view mode, and the raster + MSAA G-buffer resolve. Phase 3's baseline followed, built
but not yet run: the project setting RENDER.NEURAA (Anti-Aliasing page, `--neuraa` / `--no-neuraa`) and
the distance-to-edge blend. Not yet: alpha-masked edges (the material flags live in Voxi) and the
network. Every millisecond
below is an estimate unless it says *measured*.

**One-line summary:** a small network predicts the *parameters* of a filter we already have; it never
paints pixels. For NeuRAA that is how each edge pixel, found from primary visibility, blends with its
neighbours, every frame in ray-driven mode. For NRD it is how a spatial pyramid of the current frame is
mixed, per 8x8 tile, in place of FidelityFX's temporal resolve. The filter stays
hand-written and bounded, so a bad prediction can only pick a worse setting of a known-good filter,
never invent an image. Without weights, NeuRAA runs its analytic baseline and NRD runs FidelityFX's
resolve.

---

## 1. Why parameters, not pixels

Both problems are the same shape, and the measurements of 2026-10-04/05 show it:

- **The denoiser:** FidelityFX's constants are global. Its radiance-difference weight at the vendored
  0.6 returned 0.49 of the GI it was given; at 0 it kept 0.79 by day but turned night path tracing into
  blotches in motion; 0.2 is a compromise (`DENOISING.md` section 2). One number cannot be right both
  for a tile of rare fireflies and for a tile of genuinely bright bounce light.
- **TAA:** it smeared under motion badly enough that it now runs only while the camera is still
  (`modules/render.sr/README.md`), so in motion nothing anti-aliases edges. NeuRAA is a single-frame
  method with no history to smear, run every frame: motion gets anti-aliased edges, and the TAA at rest
  starts from them (section 3).

A network that looks at a tile's statistics and picks those numbers per tile addresses exactly that.
Predicting parameters rather than pixels also:

- **fits the hardware.** The dev GPU is an RX 7800 XT on D3D12, which has no matrix-core path
  (`modules/render.neural/README.md`: fp32 portable kernels; D3D12 LinAlg is SM 6.10, Vulkan cooperative
  matrix comes after Vulkan parity). An image-to-image convolutional network per output pixel is out of
  budget. NeuraFI measured 1.75M records at 3.5 ms (*measured*, its network size), so per-pixel at
  render resolution is too slow; NRD's one record per 8x8 tile at 1766x994 is ~27k records, and
  NeuRAA's edge pixels ~90-180k (estimates).
- **reuses what exists.** `Aver.Render.Neural`'s `Mlp` (fp32, inputs <= 64, outputs <= 16, width <= 64,
  layers <= 6, AVNN weight files, in-engine Adam) needs no new layer types for this.
- **follows NeuraFI's rule:** the network predicts a correction to an analytic baseline, the baseline is
  what runs until weights exist, and a gate keeps the network out where it measures worse.
- **clears most of the patent-dense prior art**, which needs image or per-pixel-kernel outputs. The
  findings that still constrain the design, two of them reaching shipped code, are in section 7.

---

## 2. Shared structure

```
G-buffer / visibility + signal -> [feature pass] -> records (StructuredBuffer<float>)
                                                       |
                                                 Mlp::recordInfer
                                                       |
                       bounded parameters -> the hand-written filter, which mixes real values
```

| | NeuRAA | NRD |
|---|---|---|
| record | one per edge pixel (~24 floats) | one per 8x8 tile (~20 floats) |
| outputs | 9 blend-weight corrections over the 3x3 | 4 candidate logits, edge-stopping, hit-distance coupling, gate |
| filter | distance-to-edge blend of neighbour colours | NRD resolve: pyramid candidates + short stabiliser |
| without weights | the analytic baseline blend | FidelityFX's own resolve |

- **Features** are normalised to O(1) (log-luminance, ratios to the tile mean, motion in pixels / 16), so
  one set of weights serves every scene brightness.
- **Bounded outputs:** every output goes through a sigmoid or a softmax mapped to a hand-chosen safe
  range. The network cannot leave the range the filter was tested in, and both filters only ever
  form convex mixes of values that already exist in the frame.
- **Gate:** a per-record confidence below threshold falls back to the "without weights" path for that
  record.

---

## 3. NeuRAA

### What it is

Single-frame edge anti-aliasing (redesigned 2026-10-05; decisions of the same day: it runs every frame,
TAA stays as it is, and raster mode uses MSAA instead). Primary visibility finds the edges; the network
only decides how to blend each edge pixel with its neighbours.

| mode | camera | path |
|---|---|---|
| ray-driven (default) | moving | NeuRAA, then AverSR's spatial fallback (FSR 1 EASU, which expects anti-aliased input) |
| ray-driven | still | NeuRAA, then AverSR's TAAU, unchanged ("TAA only while still" stays) |
| raster | any | the existing MSAA setting (hardware resolve); no NeuRAA |

In motion it replaces nothing: today edges there get no anti-aliasing at all, because TAA smeared and
now runs only while still. At rest the TAAU accumulates frames whose edges are already resolved, which
should shorten the time it takes to converge after the camera stops. NeuRAA has no history, so it
cannot ghost.

**Raster and MSAA.** Raster mode has no triangle IDs to find edges from, and the hardware already
resolves coverage there. But with MSAA above 1x the renderer turns the denoiser off
(`VoxiRenderer.cpp`: the G-buffer targets would need the same sample count). Raster + MSAA therefore
needs one fix before it is a complete path: resolve the G-buffer to one sample per pixel (nearest-depth
sample for depth, normal and motion) for the denoiser, NRD and AverSR to read. Colour uses the plain
hardware resolve: no edge classification with a per-group filter (section 7).

### Pipeline

```
primary visibility (gRdVisBuf / G-buffer)
   -> [1 detect]   edge code per pixel, 8x8 tile flags  -> indirect dispatch of edge tiles only
   -> [2 coverage] own-triangle edge distances + 4 alpha sub-samples
   -> [3 blend]    baseline weights  ->  Mlp correction (bounded)  ->  9 weights per edge pixel
   -> [4 resolve]  colour = sum(w_i * neighbour_i); non-edge pixels untouched
   -> AverSR spatial upscale
```

### 1. Edge detection (cheapest first)

The ray-driven path already writes, per pixel, `gRdVisBuf = (instance ref, triangle, barycentrics.xy)`,
plus `viewZ` and `normalRoughness`. Detection reads those, nothing new:

- **Tile pre-pass:** one 8x8 group per tile computes min/max instance ref and min/max `viewZ` in LDS,
  with `WaveActiveAllEqual` on the instance ref. Tiles that are one instance with a smooth depth range
  (most of the screen) are skipped by every later stage. Cost: one read of the visibility buffer
  (~28 MB at 1766x994), ~0.05 ms (estimate).
- **Per pixel, in surviving tiles:** an edge exists toward a neighbour when the **instance differs**, or
  the instance is the same but **depth jumps** (relative dz above a slope-aware threshold) or the
  **normal turns** more than ~30 degrees. A different triangle of the same smooth surface is not an
  edge: the triangle ID alone fires on every tessellation edge. The result is a byte: 4 direction bits
  and a class (silhouette, crease, alpha-masked). Nothing counts the triangles in a pixel (section 7).
- **Luma** (FXAA/SMAA style) is cheaper still, but fires on texture detail and misses equal-brightness
  edges. It is used only as a network input for shading contrast, never to decide where edges are.
- **Last frame's mask** is not reused: detection is already the cheap stage, and reprojecting a mask
  costs about as much as recomputing it.

Expected edge share: 5-15% of pixels; the tile flags make stages 2-4 scale with that, not the screen.

### 2. Coverage

How much of the pixel each side of the edge covers. Every edge pixel gets the **same** computation;
the edge class is a network input, never a switch between methods (section 7):

- **Own edge distance.** Each pixel uses only the triangle its own visibility record names. AverSR
  cannot read Voxi's vertex buffers (module boundary), so the distance comes from the record's
  barycentrics instead: their change to a neighbour on the same triangle is the per-pixel gradient,
  and the distance to the edge where a barycentric reaches 0 is `b / -(db/dx)` (DEAA's `v / (dv/dx)`);
  with no same-triangle neighbour on that axis the pixel is left unblended. Equivalently: project the
  triangle's edges to screen space and store, per direction with a detected discontinuity, the signed
  distance in pixels from the pixel centre to where that edge crosses (clamped to +-1 px; "no crossing"
  is +1). This is distance-to-edge AA (Malan 2010, Persson 2011) evaluated from ray-hit data, and it
  gives a continuous edge position rather than a few samples. A pixel never computes another
  triangle's distance; it reads its neighbours' own stored values.
- **Alpha coverage.** The alpha-test texture sampled at 4 sub-pixel UVs, from the barycentrics and their
  screen derivatives (1.0 for opaque materials). Foliage and fence edges live in the texture, not the
  triangle.
- **No extra rays in v1.** Tracing more primary rays at edge pixels is the classic answer (Whitted
  1980), but a pending NVIDIA application claims ray tracing to correct edge pixels in very broad terms
  (section 7). It stays out until that claim settles.

Estimated cost at render scale 0.5: ~0.15 ms.

**Known gap:** geometry thinner than a pixel that no primary ray hit is invisible to all of this and
will flicker in motion. At rest the TAA covers it; in motion it is the measured weak point (section 6's
thin-geometry level).

### 3. Blend weights: baseline, then network

- **Baseline (ships first, no network):** the closed-form distance-to-edge blend: each edge pixel mixes
  with the neighbour across the edge by the covered fraction its own and that neighbour's stored
  distances give (GBAA's resolve, Persson 2011), times the alpha coverage. It is what runs without
  weights, and it alone fixes the in-motion stair-stepping.
- **Network:** per edge pixel, an `Mlp` record (~24 floats): its own and its 8 neighbours' stored edge
  distances, alpha coverage, the neighbours' same-instance bits, the edge class, neighbours' luma
  relative to the pixel, and the depth gradient across the edge. It outputs 9 logits that **adjust** the
  baseline weights (softmax of baseline log-weights plus the bounded correction). Weights are
  non-negative and sum to 1, so the result is always a convex mix of real neighbour colours: it cannot
  invent colour or brightness.
- **What the network is for:** the cases the line model gets wrong: corners, two edges, thin features,
  edges along a texture or lighting gradient, and edge pixels whose neighbours are themselves mixed.
- **Size:** edge pixels only (~90-180k records at render scale 0.5), a 3-layer width-32 MLP; ~0.15 ms
  (estimate, scaled from NeuraFI's measured 1.75M records in 3.5 ms).

### 4. Training

Parameter space only, as NRD:

1. **Reference:** the same frame supersampled at 64 jittered sub-pixel positions with the camera still
   (the ray-driven path renders this directly), so every edge pixel has a true coverage-weighted colour.
2. **Oracle weights:** per edge pixel, the 9 non-negative weights summing to 1 whose mix of the
   neighbour colours is closest to the reference pixel (a small non-negative least-squares fit).
3. **Regression:** the network learns to map the record to those weights. No image loss is
   back-propagated (section 7).

Captures come from scripted camera paths on NeonDistrict Day and Night, NewSponza and the test levels,
all run in a visible window. Weights ship in `bin/data` as an AVNN file. A gate keeps the baseline where
the network measures worse.

### Training run v1 (2026-10-05)

Reproduce with `tools/neuraa/`:
1. `capture.py <Sandbox.exe> <level> <dir> <poses>` per scene: Unlit (authored colour, so lighting noise
   does not pollute the coverage targets), ray-driven staged, render scale 0.5, the camera drifting
   (`--cam-wander`) and holding still for each pose. Per pose: one unjittered base frame and the mean of
   64 frames on an 8x8 sub-pixel grid (NeuRaa's jitter override). v1: NeonDistrict Day 16 poses and
   NewSponza 16 for training, CyberCity2099 Day 8 held out.
2. `nraa_train.py <capture root> <weights>`: oracle 3x3 weights per edge pixel (non-negative, sum 1,
   fitted to the reference, regularised toward the baseline), then the 36-32-32-9 MLP regressed onto
   them; model selection on the held-out weight error. Output `modules/render.sr/data/neuraa_v1.bin`.
3. `nraa_eval.py <weights> <captures made with --with-neuraa>`: the engine's resolved frame against the
   script's own prediction and against the reference.

v1 results on the held-out scene (edge-pixel colour error against the 64-sample reference): no AA 0.0308,
baseline 0.0275 (-10%), network in the engine 0.0230 (-25%). The engine matches the script to 3.4e-4
relative; pixels off the edges are bit-identical. Not yet measured: lit scenes in motion, and cost.

### Cost (estimate)

Detect ~0.05 ms, coverage ~0.15 ms, inference ~0.15 ms, resolve ~0.05 ms: ~0.4 ms at render scale 0.5 on
the RX 7800 XT, every frame in ray-driven mode.

---

## 4. NRD

### What it is

A replacement for **one** FidelityFX pass, the temporal resolve, designed 2026-10-05. Reproject and
prefilter stay exactly as they are. Where FidelityFX's resolve trusts history first and rejects outliers
against an 8x8 average, NRD builds its estimate **spatially first**, from the current frame, and keeps
only a short hand-written temporal stabiliser. The two problems measured this week come from the
resolve: light lost to outlier rejection (denoised GI 0.84 of the true mean at the radiance weight of
0.2, `DENOISING.md`), and blotching and smearing from long history in motion.

```
Scale -> Reproject (FFX) -> Prefilter (FFX) -> [NRD pyramid] -> Mlp per 8x8 tile -> [NRD resolve]
                                                                                  \-> FFX resolve (fallback)
```

### Settings

- **Denoiser** (existing, `RENDER.DENOISER`) and **Neural Denoise** beneath it, a project key
  `RENDER.NEURALDENOISE` (absent = 0) shown on the denoiser page, greyed out while Denoiser is off.
  `--neural-denoise 0|1` overrides it, as the other render flags do.
- **Off by default** for every project until it beats FidelityFX's resolve on the rig (section 6).
- It covers every signal the denoiser runs: **ReSTIR GI**, **ReSTIR PT / path tracing**, and **sky
  occlusion** (the scalar build). Reflections and RT shadows are planned to use it later (below).

### The NRD resolve

It writes the same targets as FidelityFX's resolve (`history[cur]`, `varHistory[cur]`), so reproject
and prefilter see no difference, and switching per tile or toggling the setting never resets history.

1. **Pyramid.** One pass builds two levels from the current frame's **raw** noisy signal, not the
   prefiltered one (the prefilter's radiance weight darkens): 1/2 and 1/4 resolution, depth- and
   normal-aware (a child sample counts only if it lies on the parent's surface), plus a 1/8 level
   written by the same pass. FidelityFX's own 1/8 mean is **not** reused: it is 30% reprojected history
   (`lerp(radiance, reprojection, 0.3)` in its reproject), and every level must come from the current
   frame only (section 7). Pixels with no fresh sample
   this frame (half-rate GI's checkerboard, sky) get weight 0; the normalised weights fill them from
   fresh neighbours, push-pull style. All four candidates per pixel are then: FidelityFX's prefiltered
   value (finest, edge-aware) and levels 1-3, each upsampled with joint bilateral weights against this
   pixel's depth and normal.
2. **Network, once per 8x8 tile** (FidelityFX's group size, ~27k records per signal at render scale 0.5).
   The pyramid pass also writes the tile record (~20 floats):
   - log luminance of each level relative to the tile mean;
   - spatial variance and FidelityFX's temporal variance, as tile means;
   - **tail heaviness**: the fraction of the tile's energy in samples above 4x and 16x its mean (what
     separated fireflies from real night light on 10-05);
   - hit distance: log mean and spread;
   - depth range and normal spread; motion length; disocclusion fraction; mean roughness; half-rate
     flag.
   Deliberately **not** inputs: per-pixel colours, history colours, sample count and frame time
   (section 7). Outputs, all bounded: 4 logits over the candidates, depth and normal edge-stopping
   sharpness, hit-distance coupling, and a gate confidence.
3. **Per-pixel spatial estimate S.** The tile's logits plus per-pixel terms (a candidate whose bilateral
   weight failed drops out; a short **hit distance** shifts weight toward the fine candidates, since
   nearby geometry makes indirect light change quickly) go through a softmax. The weights are
   non-negative and sum to 1, so S keeps the local energy: a firefly is spread over its neighbourhood
   instead of being deleted, which is what keeps brightness right.
4. **Short stabiliser (hand-written, no network).** `out = lerp(S, clamp(H), a)`: `H` is reproject's
   history at this pixel, clamped to the min/max of S's 3x3 and the 1/8 level's 3x3 texels (Karis 2014,
   not a mean +- sigma box); `a = 1 - 1/min(n, N)` with `n` reproject's sample count and `N` = 12 frames
   while the camera or the surface moves, 16 at rest. The 1/8 level's texels widen the box because a
   coarse-level blob covers all of S's 3x3, and a box of S alone held history up to the blob. History only steadies the residual flicker; it never decides brightness.
   `varHistory` is updated with FidelityFX's formula so its prefilter keeps the same input.
5. **Fallback.** With no weights for a signal, the whole pass runs FidelityFX's own resolve. With
   weights, a tile whose gate confidence is low runs FidelityFX's resolve for that 8x8 group: the same
   shader carries both paths, and the branch is uniform per group because the tile is the group.

### Phase 4 measurements (2026-10-05)

NeonDistrict Night (ReSTIR path tracing, half-rate), render scale 0.5, exposure 4, no tonemap. GI-only
linear metric (`abgi` / `abspot`: a frame minus the same frame at `voxi.giIntensity 0`). Brightness is
against the reference path tracer (`voxi.ptMode 1`) at a still camera; spots and grain in motion use
`--cam-wobble 30 16`. Developer flag `--nrd-spatial 1` / `voxi.nrdSpatial`.

| resolve | still: GI vs reference | still: spots / grain | moving: spots / grain |
|---|---|---|---|
| FidelityFX | 0.39 | 2.93 / 0.195 | 4.04 / 0.332 |
| NRD, prefiltered + fine-leaning | 0.45 | 2.38 / 0.182 | 4.19 / 0.352 |
| NRD, raw + coarse-leaning (now default) | 0.62 | 1.66 / 0.172 | 3.46 / 0.312 |
| NRD, 1/8 level only | 0.71 | -- | -- |

- **FidelityFX loses about half the light the input has at night** (0.39 against the input's ~0.71); NRD's
  default loses about 13%. The input itself is ~0.71 of the reference: ReSTIR path tracing (or the
  difference between the two tracers) is darker before any denoiser, a separate issue.
- Using FidelityFX's prefiltered value as NRD's finest candidate inherited its darkening; the raw signal
  is the finest candidate now.
- **Not solved: blotches in motion.** At a still camera NRD is visibly cleaner. Moving, it trades fine
  speckle for soft coloured blotches: the coarse levels leave low-frequency noise and history cannot
  average it, because the sample count keeps resetting (a 4, 8 or 16-frame cap measured the same).
  The network did not fix it; the widened history clamp did (below).

### Network v1 (2026-10-05)

Trained on 20 NeonDistrict Day/Night poses, judged on 6 held-out Night poses at another wander speed
(`tools/nrd/nrd_train.py`): relative L2 against the converged input 1.10 fixed, **0.77 network**, 0.71
oracle; energy 1.003. Loading `bin/data/nrd_v1.bin` turns NRD's resolve on under Neural Denoise.

Night, still, total image against the reference path tracer: FidelityFX 0.943, NRD fixed 0.973, NRD
network 0.981 (the input reaches ~0.985). Moving (`--cam-wander 1.5 2.3`, frame 400), GI only, blotch =
band-pass energy between 9 and 33-pixel boxes over the GI mean (`blotch.py`):

| resolve | GI mean | blotch | blob pixels | spots | Denoise GPU |
|---|---|---|---|---|---|
| FidelityFX | 0.00114 | 0.269 | 0.96% | 2.62 | 1.22 ms |
| NRD network, 3x3-of-S clamp, N 4 | 0.00170 | 0.327 | 1.38% | 1.94 | 2.90 ms |
| NRD network, widened clamp, N 12 | 0.00172 | **0.260** | 1.04% | 1.80 | **1.49 ms** |

- The owner saw more bright blotches moving with the first network build: coarse-level blobs held by
  the 3x3-of-S clamp. A longer history alone did nothing (0.320); the widened box is the fix.
- The first build ran the reductions and the network on one thread per tile (2.90 ms). It is now an LDS
  tree reduction and one hidden unit per thread (1.49 ms, +0.27 ms over FidelityFX). Staging the
  weights in LDS with wave reductions measured the same; computing each candidate once was slower
  (more LDS).
- The rest of that frame is ~45 ms GPU at 3532x1987 x 0.5 with or without NRD (lighting stages 20 ms,
  voxelise 9 ms under the moving camera).

### Hit distance

FidelityFX is given a hit distance of 0 today. NRD needs the real one per pixel:

- **ReSTIR GI:** the reservoir already holds the sample's hit point. The GI pass writes the distance
  into the alpha of `gGiRadianceOut`, which is written as 0 today: one store, no new target. That pass
  is the register-pressure-sensitive `giRestirIndirect` (the RX 7800 XT hang of 10-04), so this is
  verified with DRED in a visible window before anything else builds on it. If it adds pressure, NRD
  reads the hit point from the reservoir buffer itself instead (~0.1 ms).
- **Sky occlusion:** the signal *is* a normalised hit distance.
- **ReSTIR PT:** the first bounce's distance if the PT path stores it; otherwise the input is 0 with
  a "missing" flag, and that profile's network learns without it.

### Signal profiles and later signals

One AVNN weight file per profile (`nrd_gi`, `nrd_pt`, `nrd_ao`), each trained on its own signal. For
**reflections**, the design keeps a roughness input and the reproject pass's parallax reprojection
(FidelityFX's native case); the pyramid's footprint has to follow the lobe, so it needs its own profile
and training. **RT shadows** would use blocker distance as the hit distance (penumbra width), with
FidelityFX's shadow denoiser as the fallback rather than the reflection pipeline.

### Training

Pre-trained offline; weights ship in `bin/data` like NeuraFI's, the same for every project.

1. **References:** the reference path tracer's converged still frames (up to 1,024 accumulated) for PT,
   and the same estimator accumulated at rest for ReSTIR GI and sky occlusion.
2. **Oracle parameters:** per tile, search the outputs (a coarse grid, then a local refine) for the
   setting whose result is closest to the reference, scored by relative L2, the **energy ratio** (tile
   mean against the reference mean) and **flicker** across a short moving sequence.
3. **Regression:** the network learns to map the tile record to the oracle parameters. No image loss is
   back-propagated, and the network's own output is never run through the filter and scored against
   the reference during training: validation, checkpoint choice and early stopping all use the
   parameter-space error. Image metrics on the rig judge only the finished weights (section 7).
4. **Rollout:** because the stabiliser feeds back, the frames are re-captured with the trained network
   running and the oracle refit once or twice. The short history (4 frames in motion) keeps this
   stable.

Scenes: NeonDistrict Day and Night, NewSponza and NewSponza_Night, captured on scripted camera paths in
a visible window; camera paths are split between training and evaluation.

### Cost (render scale 0.5, RX 7800 XT, per RGB signal)

Measured +0.27 ms over FidelityFX's resolve for the GI signal (network v1, above), against a budget of
**+1 ms** over FidelityFX's resolve for everything NRD runs. Within that budget the cheapest setting
that meets the quality bar wins: two pyramid levels before three; 8x8 tiles before 4x4.

---

## 5. Where the code goes

- `modules/render.neural`: unchanged for v1.
- `modules/render.sr`: `NeuRaa` beside `TemporalUpscaler` (tile pre-pass, edge detection, coverage, blend
  and resolve), called every frame in ray-driven mode before AverSR (its spatial fallback or the TAAU).
  It reads `gRdVisBuf`, `viewZ` and `normalRoughness`.
- `modules/render.voxi`: raster + MSAA resolves its G-buffer to one sample per pixel for its readers, so
  the denoiser no longer has to turn off. Setting `RENDER.NEURAA`,
  `--neuraa 0|1|2` (off, baseline, network), Display > Anti-aliasing, plus an edge-class debug view.
- `modules/render.denoise`: two new passes in `aver_denoise.hlsl`'s one-compile-per-pass scheme, a
  pyramid pass that also writes the tile records and the NRD resolve (which includes FidelityFX's
  resolve header for its fallback branch), with `Mlp::recordInfer` between them. `Denoiser` records them
  in place of the resolve when the setting is on. Setting `RENDER.NEURALDENOISE`,
  `--neural-denoise 0|1`, a checkbox under Denoiser on the denoiser page.
- `modules/render.voxi`: the GI hit distance in `gGiRadianceOut.a` (`voxi_restir.hlsli`).
- A capture mode (`--neuraa-capture` / `--nrd-capture`) that writes feature records and references, and
  an offline oracle-search and training tool built on `MlpReference` (CPU) or `Mlp::recordTrain` (GPU).
  Weights are pre-trained and shipped; there is no on-device training.

---

## 6. How it is judged

Every comparison runs in a visible window on fixed camera paths, with the linear metrics built on
2026-10-04/05:

- **NRD:** against FidelityFX's resolve, per signal: the GI-only linear mean against the reference
  (target within 3%, against FidelityFX's 0.84), the spot metric and temporal flicker on
  `--cam-wobble 30 16`, and cost with `--gpu-timing` (at most +1 ms for everything NRD runs). It stays
  off by default; it is proposed as the default only if it wins on brightness and spots without worse
  flicker, by day and by night, on NeonDistrict and NewSponza.
- **NeuRAA:** on the moving-camera rig, error against the 64-sample reference on edge pixels, edge
  crawl (frame-to-frame change on edges not present in the reference) and cost; at rest, TAA with
  NeuRAA against TAA alone, on converged quality and on frames to converge after the camera stops. The
  baseline ships if it beats today's no-AA motion path without making the still image worse; the network
  ships only if it beats the baseline, per scene. A thin-geometry test level (wires, fences, distant
  railings) tracks the known gap.
- **Raster + MSAA:** denoiser output with the G-buffer resolve matches 1x within noise.

---

## 7. Patents and licences

Sweeps were run on 2026-10-05: [NEURAA_NRD_PATENTS.md](NEURAA_NRD_PATENTS.md) (engineering mapping, not
legal advice). The design follows them unless counsel says otherwise:

1. **History clamps use min/max, not mean +- sigma.** NVIDIA US 10,116,916 (in force to 2037) claims
   TAA history clipped to a per-colour-channel mean +- sigma box. NRD's stabiliser clamps to the 3x3
   min/max (Karis 2014, Sousa 2013), and so should the shipped TAA. FidelityFX's own resolve, which NRD
   falls back to, keeps its box; that is the shipped-denoiser finding, not NRD's.
2. **NeuRAA's network never upscales and never touches history.** NVIDIA US 12,033,301 and its
   continuations claim networks generating higher-resolution video from upsampled frames blended with
   prior output (one pending claim: a network-predicted per-pixel blend factor with a prior frame).
   NeuRAA runs every frame, so at rest its output feeds the TAAU, which does upscale and blend history.
   The network works at render resolution, on one frame, and outputs only spatial 3x3 weights;
   upscaling and history blending stay in the non-neural TAAU. Whether a network upstream of a
   non-neural temporal upscaler is outside those claims is **counsel's question**; until answered, the
   fallback is NeuRAA's baseline (no network) whenever the TAAU runs.
3. **NeuRAA: one treatment, no triangle counting, no extra rays.** NVIDIA's 2018 edge-AA family claims
   selecting an AA technique from a count of primitives in a pixel (US 12,444,026, granted 2025-10-14),
   selecting among several AA algorithms by whether pixels are edges (US 12,141,946; with NeuRAA on
   edges and TAA everywhere at rest, counsel should confirm that is not such a selection: the tile skip
   only omits pixels whose result would be unchanged), and, pending,
   ray tracing to correct miscoloured pixels in very broad terms (US 2025/0299305). Every edge pixel
   gets the same computation, the edge class is only a network input, and no extra primary rays are
   traced until that application's claims settle. Qualcomm US 11,631,215 claims blending from a pixel's
   own and an auxiliary primitive's edge distances: each pixel computes only its own triangle's
   distance and reads its neighbours' own values (GBAA, 2011, practises that). AMD US 9,019,299 (to
   ~2029) claims grouping pixels by sample values with a filter per group; NeuRAA classifies from
   visibility records, which counsel should confirm is outside it, and raster MSAA uses the plain
   hardware resolve, never an edge-classified one.
4. **NRD trains in parameter space only.** The University of California's US 10,832,091 claims
   back-propagating an image error between filtered output and ground truth; its parent US 10,192,146
   (to ~2036) claims, without the word backpropagation, a model that computes a filter's parameters, an
   error metric applied to the filtered image, and the model corrected from it, repeated. NRD's network
   is regressed onto oracle parameters, and its own output is never filtered and scored during training,
   validation, checkpoint choice or early stopping.
5. **NRD's pyramid is the current frame's, and its level choice never depends on counts.** NVIDIA US
   11,113,792 claim 15 covers a spatial filter sized from the history count; NVIDIA US 12,182,927 (to
   ~2041) covers choosing a resolution level of an *accumulated* render from the number of renders in
   it, then blurring (its sibling US 11,508,113: blur radius from the accumulated-frame count). Every NRD
   level is built from the current frame's signal (hence its own 1/8 level, not FidelityFX's
   history-mixed one), level choice comes from the network's tile statistics and hit distance, and
   sample count only sets the stabiliser's history weight, never a radius or a level. The stabiliser's
   history length depends on motion, never on view angle or parallax (US 11,823,321).
6. **NRD's per-pixel weights come from hand-written code.** Pixar/Disney US 10,672,109 (to ~2038)
   claims a network that, from an image and its down-sampled version, generates denoised images and a
   set of per-pixel weights to blend them. In NRD the levels are made by fixed filters, the network
   outputs per-tile logits only, and the per-pixel terms (edge-stopping, hit distance) are fixed maths.
   The network must never emit a per-pixel weight map or a per-level image.
7. **NRD's stabiliser clamps to order statistics.** NVIDIA US 12,482,168 (granted 2025-11-25) claims a
   clamp range defined from the distribution of ray-traced samples; its parent US 11,600,036 from the
   first and second moments. The stabiliser's range is the 3x3 min and max of S, never a mean, variance
   or fitted distribution, and its footprint is never larger than the spatial estimate's (US 11,663,701
   claims a temporal clamp radius larger than the spatial radius; its parent US 10,991,079 a spatial
   filter on an exponentially accumulated signal, which NRD's current-frame pyramid is not).
8. **Fixed tap patterns.** NVIDIA US 12,423,782 (continuation of US 11,113,792, granted 2025-09-23)
   claims spatial filter taps at locations chosen by jittering a parameter. NRD's bilateral upsample uses
   fixed taps with no per-frame or per-pixel rotation; FidelityFX's prefilter already uses a fixed
   15-tap pattern.
9. **Shadows later: hit distance only biases the level.** NVIDIA US 10,740,954 claims filter dimensions
   from occluder distance through a light-shape (penumbra) geometry. When NRD extends to RT shadows,
   blocker distance only biases the pyramid level, never a computed penumbra footprint.
10. **NRD's network never sees per-pixel or history colours, or frame time.** Intel US 12,374,006 (in
   force to 2043) claims a history-validation network fed, per pixel, the current colour, depth,
   auxiliary buffers, the reprojected history colour and the time between frames. NRD's network gets
   tile aggregates only and outputs spatial parameters; history is handled by hand-written code.
11. **Un-jitter before the denoiser** (Arm US 18/497,608, granted 2026-09-29).
12. **Watch:** AMD's pending US 2026/0094228 (any trained network in a pipeline stage; every claim
   rejected as of 2026-08-12) and NVIDIA's US 2025/0299305 and US 2026/0073486.

NRD's own sweep (multi-scale learned blending, hit-distance kernels, push-pull filling) is section 9 of
the patents document. **Counsel should review before either feature ships.** Nothing is vendored: the
code is `Aver.Render.Neural` and the in-house filters, plus the MIT FidelityFX Denoiser (whose licence
grants no third-party patent rights).

---

## 8. Phases

1. **Plumbing, no visual change.** The Neural Denoise setting and the NRD resolve pass running only its
   FidelityFX branch (verified identical to today); NeuRAA's tile pre-pass, edge detection and the
   edge-class debug view; the raster + MSAA G-buffer resolve, so the denoiser stays on.
2. **GI hit distance** in `gGiRadianceOut.a`, checked with DRED in a visible window on the RX 7800 XT
   before anything uses it.
3. **NeuRAA baseline, every frame, no network.** Own-triangle edge distances, alpha sub-samples and the
   distance-to-edge blend. This alone fixes in-motion stair-stepping.
4. **NRD with fixed parameters**, behind a developer flag: the pyramid, hit-distance weighting and
   stabiliser with hand-tuned constants. This answers the two open questions (brightness and flicker of
   a spatial-first resolve) before any training.
5. **Capture and oracle.** The capture modes, references (64-sample still frames for NeuRAA, converged
   path tracing for NRD) and the oracle fits. Oracle results are the ceiling any network can reach.
6. **Train and ship NRD weights** (GI, PT, sky occlusion), off by default, behind the gate.
7. **Train and ship NeuRAA's network** over the baseline, behind its gate.
