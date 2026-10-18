# NRD v2: single-frame neural denoiser over the composed lighting

**Status:** phases 1, 3 and 4 built (2026-10-06), network trained and shipped (v1 and v2 below); the temporal
stage took FidelityFX's history handling on 2026-10-12 ("History handling after FidelityFX"; built, not yet run in
the engine). Owner decisions: replace the whole denoiser stack (FidelityFX and the
per-signal plumbing); **single-frame** (the spatial filter and the network never see history, so no
smear or ghosting from them; an optional temporal stabiliser on jitter-free frames was added later, see
"Temporal stabiliser" and "History handling after FidelityFX"); denoise the **composed lighting**; a **convolutional** network; **trained in-engine** like
NeuraFI, not in Python. Supersedes NEURAA_NRD.md section 4 (NRD v1) once phase 1 lands.

## Why

The speckle owners see is in the final image, which combines terms filtered separately (GI and
reflections through FidelityFX, sun shadows, local-light shadows and sky occlusion through their own
filters) and terms filtered by nothing (the path at reflection hits, emissive under ReSTIR PT).
FidelityFX is a short clamped temporal accumulator: it removes energy (night darkening, road
reflections at 25-35% of their light), its history resets in motion so noise returns, and its history
is what smears. One denoiser after shading, single-frame, sees every noisy term once.

## Pipeline

```
Stage B (ray-driven compose), per pixel at render resolution:
  noisy diffuse lighting  / diffuse albedo   = D   (demodulated, rgb)
  noisy specular lighting / specular albedo  = S   (demodulated, rgb; FssEss as raster uses it)
  clean terms: emissive, unlit, sky, translucency  -> added after, never denoised
  guides: view Z, normal, roughness, diffuse albedo, specular hit distance (one frame, no history)
        |
NRD2 pyramid (current frame only): D and S, edge-aware 1/2, 1/4, 1/8 levels  [fixed maths]
        |
NRD2 network (conv, in-engine): tile statistics + guides at 1/4..1/8  ->  per 8x8 tile, per signal:
  level logits (4), depth / normal / luminance edge sensitivities (3)              [network]
        |
NRD2 resolve: per pixel, fixed edge-stopping weights from the tile's parameters over the candidates
  (own pixel, upsampled levels); D' and S' re-modulated, clean terms added          [fixed maths]
        |
AverSR / NeuRAA / tonemap / NeuraFI as today
```

Without weights (or with the network off) the resolve runs fixed default parameters: phase 1 ships that
on its own. FidelityFX and Voxi's per-signal reflection/shadow histories stay as the fallback behind the
Denoiser setting until NRD2 is measured better on both scenes; then they are removed.

## Phase 1 as built (2026-10-05)

Denoiser mode 2 (`voxi.denoiserMode 2`, `--denoiser 2`, `RENDER.DENOISER 2`; the Rendering > Denoising
combo). D3D12 staged ray-driven frames with the G-buffer only; anywhere else (Vulkan, single pass,
MSAA, debug views, Reference path tracing) the frame runs as before, FidelityFX if it can.

**Single-frame filter.** (The stabiliser added later is a separate pass after the resolve.) An NRD2 frame
runs no FidelityFX pass and no Voxi history:
`rtHistParams.xy = 0` puts the sun shadow, reflection, sky-occlusion and lamp visibility on their raw
paths (no reprojection, no history write) and `rtDenoiseParams.w = 0` the sky occlusion. Half-rate
tracing stays, filled from this frame only (next section). CSRdRefl takes its denoiser branch (one raw
lobe sample and its hit distance per pixel). ReSTIR's reservoirs (sampler-side reuse) and the lamp
visibility's 5x5 same-frame filter are kept. The first frame after NRD2 restarts every history.

**Half rate, single-frame.** Forcing full rate cost ~10 ms of "Voxi RD lighting stages" on NeonDistrict
Night (the half rates were rebuilt from FidelityFX or Voxi history). NRD2 frames instead trace one pixel
checkerboard half and fill the other from its four edge neighbours, all traced this frame:

| feature | skipped | filled by | dial (default on) | also needs |
|---|---|---|---|---|
| ReSTIR GI | CSRdGi's checkerboard variant (parity `denoiseFrame_ & 1`) | CSRdHalfFill -> gRdGiTex | `voxi.nrd2HalfRateGi` | `rayDrivenStages 2` |
| glossy reflections (mirror full rate) | CSRdRefl, `(x^y^frame)&1`, S.a = -1 | CSRdHalfFill -> gRdReflTex + S.a (hit distance) | `voxi.nrd2HalfRateRefl` | `rtReflectionHalfRate` |
| sky occlusion | CSRdSkyOcc, the other half to reflections, a = -1 (was 8x8 tiles over history) | CSRdHalfFill -> gRdAoTex | `voxi.nrd2HalfRateAo` | `rtSkyOcclusionHalfRate` |

Fill weight per neighbour (`rdHalfFillWeight`): view depth (gRdSunVisTex.a) within 2% + 1 cm of the
centre's or of the plane through the centre and the opposite neighbour (grazing surfaces), times cos^8
between the vertex normals CSRdShadow writes into u3 (NRD2's D target, unused until Stage B) as a guide.
A 1e-3 floor keeps the plain mean of the valid neighbours where none agrees; a reflection with no traced
neighbour (all rough or sky) stays black, a GI pixel with none keeps CSRdGi's reuse-only answer.
CSRdHalfFill is its own dispatch after the lighting stages (GPU span "Voxi RD half-rate fill", 8x8,
fp32, in place: it reads only the traced half and writes only the skipped one); Stage B is unchanged
apart from the lamp tap test. Bits in `giShadowParams.w`: 4 reflections, 64 fill live (normal guide,
lets CSRdRefl skip without FidelityFX), 128 sky occlusion, 256 lamps; bit 2 (tiles) stays off. Each
dial at 0 traces that feature at full rate. If CSRdHalfFill fails to compile NRD2 traces GI,
reflections and sky occlusion at full rate (lamps still half).

**Stage B** (`PSRayDriven`, `AVER_NRD2=1` variant, built on first use) shades the same terms, into
buckets instead of one radiance:

| target | slot | contents |
|---|---|---|
| D (RGBA16F) | u3 | (sun and lamp diffuse + subsurface, ambient kD part, bounce, subsurface ambient) / max(kdAlbedo, 1e-3); a = 1 where the albedo is usable (max channel > 0.01, lit model) |
| S (RGBA16F) | u23 | (sun and lamp specular, environment specular incl. coat, ambient multiple-scatter part FmsEms) / max(FssEss, 1e-3); a = reflection hit distance this frame (cm, 0 none, -1 unlit) |
| remod A (RGBA16F) | u9 | rgb Rd = M kdAlbedo, a Rs.r |
| remod B (RG16F) | u2 | Rs.gb, Rs = M FssEss |

M is the fog extinction times the glass throughput (`averFogTermsAirVis`, `rdTranslucentPath`), and 0
where the colour is replaced (unlit, debug views, the poison and NeuRaC overrides). The scene colour
gets only the clean terms: emissive x extinction + in-scatter, the glass layers, sky, unlit and debug
colours. The progressive Path Tracing accumulation is compiled out. The four slots are ones an NRD2
frame leaves unused (table 0 is full at 24/24); `VoxiRenderer::bindNrd2Targets` binds them and the
next frame without NRD2 rebinds the originals. Recomposed: final = D' Rd + S' Rs + C.

**Passes** (`modules/render.denoise`, `Nrd2`, `shaders/nrd2.hlsl`, maths in `nrd2_resolve.hlsli`),
recorded right after Stage B's draw so the sky dome, blended replay, particles and post see the result.
All compute is fp32, 8x8 (params 64x1), no wave intrinsics or atomics, groupshared 3 KB (the two temporal passes 12 KB), constants at b3.

1. `CSNrd2Pyramid`: D and S from this frame only, 2x2 reductions to 1/2, 1/4, 1/8, each keeping its
   nearest surface (others weighted exp2(-23 dz/z)); guide levels hold the averaged normal and view Z
   (m), value levels rgb and a validity fraction.
2. `CSNrd2Params`: the per-8x8-tile buffer (`StructuredBuffer<float>`, 12 planes, plane-major) filled
   with the defaults. Phase 4's network writes the same buffer.
3. `CSNrd2Resolve`: per pixel and signal, candidates = own pixel and the three levels upsampled by
   depth- and normal-weighted fixed bilinear taps. Weight = exp(logit) x surviving tap share x
   luminance term (log-ratio to the coarsest usable level); own logit 0. Specular adds fixed terms: a
   smooth lobe (roughness below 0.35) and a short hit distance keep the own pixel. Writes D' Rd + S' Rs
   (with the temporal stage on, D' and S' go to two more targets and passes 5-7 write the lit target).
4. Compose: a fullscreen additive draw of that into the scene colour (4-target PSO, G-buffer targets
   masked), the G-buffer back in RenderTarget.

**Defaults** (`Settings::nrd2Params`, console `voxi.nrd2{Diff,Spec}{Logit1,Logit2,Logit3,DepthSens,
NormalSens,LumSens}`): logits 1, 2, 2 (NRD v1's measured {-2, -1, 0, 0} with the own pixel pinned);
log2 depth sensitivity 4.5 (exp2(-22.6 dz/z), as v1), log2 normal power 3 (diffuse) and 4 (specular),
log2 luminance sensitivity -1. `voxi.nrd2Bypass 1` recomposes the split unfiltered: it should match
mode 0 apart from the histories, which checks the split itself.

Not yet: un-jittering before the resolve (NEURAA_NRD.md section 7, rule 11 -- it runs on the jittered
frame before TAA, as FidelityFX does today), a GI hit distance in D's alpha, per-channel albedo
validity (a saturated albedo's empty channels read as dark irradiance to its neighbours), the oracle
and network (phases 3-4).

## Phase 3 as built (2026-10-06): capture and oracle

Code: `Nrd2Capture` (state machine, host side), `shaders/nrd2_capture.hlsl` (GPU passes),
`nrd2_resolve.hlsli` (forward and `nrd2ResolveBackward`), `Nrd2ResolveReference` (CPU twin of the
pyramid, resolve forward/backward and the fit; the spec), `Nrd2Dataset` (pose files),
`tests/render.denoise` (`Nrd2ResolveTest`). Not yet run in the engine.

**Backward.** `nrd2ResolveBackward` returns d(output)/d(theta) (rgb, six parameters) for one pixel and
one signal, forward-mode through the tap weights (depth, normal sensitivities), the level confidences,
the reference level's luminance and the candidate weights. Levels, guides and the own value are data;
the reference level's switch (conf > 0.05) and the clamps are held constant. CPU twin checked against
central differences (100% of 7k checks), energy (equal candidates give that value and zero gradient).

**Features** (`CSNrd2Features`, `nrd2.hlsl` pass 4; also phase 4's inference input): 12 channels per
half-resolution texel, NCHW fp32 `StructuredBuffer`, plane = 4 tilesX x 4 tilesY (whole tiles, zero past
the viewport). log2 luminance of D and S (2x2 mean) relative to the tile's 1/8 texel; 2x2 contrast
log2(max/mean) of D and S; log2(viewZ / 1/8 texel viewZ) in +-4; view-space normal (basis from
`IDevice::camera`); roughness; albedo luminance (Rd, which carries fog/glass M); log2(1 + specular hit
distance / viewZ) in [0, 8]; validity = share of the 2x2 with a surface and a usable albedo. One frame
only: no history, no frame time.

**Capture** (`--nrd2-capture`, steps on NRD2 frames only): Settle(30) -> Travel(45) -> Hold(H, default
256, min 32) -> Fit -> Readback(6) -> write -> Travel. While it holds, the host freezes the camera wander
(as for the NRD v1 capture) and `IDevice::setJitterSuppressed` keeps the TAA jitter at zero (D3D12).
Hold frames 2/4/8/16: D, S and their pyramids copied (K = 4 snapshots), features read back; frame 2
also the guides and the per-pixel geometry (view Z, normal, roughness, loss mask). From frame 17: fp32
running means of D and S per pixel over fresh pixels (D on ReSTIR GI's checkerboard, S on the glossy
reflections'; `Nrd2::Inputs::halfRate`), split into two halves by frame pair ((h >> 1) & 1, so a pixel
traced every other frame feeds both). The sky-occlusion half that D also carries is not separated
(its filled half enters the mean). World time: the editor plays no animation outside Play; exposure is
the caller's (`--set post.autoExposure 0`).

**Oracle** (GPU, per 8x8 tile and signal, one 64-thread group per tile): loss = sum over the 4 snapshots,
tile pixels and rgb of (resolve(theta, snapshot) - mean)^2 / (mean tile luminance of the mean^2 + 1e-8),
over 4 x pixels, + 1e-3 ||theta - theta0||^2 (theta0 = the defaults). Pixels: a surface, an albedo for D,
a mean sample. 27 starts (logits all -3/0/+3, log2 depth -1.5/0/+1.5, log2 luminance -2/0/+2 around
theta0; start 13 is theta0 and gives the default loss), then 150 Adam steps (lr 0.05, 0.9/0.99) or, with
`--nrd2-oracle grid`, 24 rounds of derivative-free pattern search (+-step per parameter, halved when no
candidate improves) with the same forward. The best objective seen wins. 8 iterations per frame (each = 4
snapshot dispatches + a step dispatch), no long dispatch. Tile weight = valid share x 1 / (1 + r / 0.1),
r = rms(lum(even half) - lum(odd half)) / mean lum. Only these free per-tile variables are scored
through the filter; no model is involved (patent rule 4).

**Pose file** `pose_NNN.n2p` in `%LOCALAPPDATA%\AverEngine\nrd2_dataset\<level>\` or the CLI directory
(format in `Nrd2Dataset.hpp`): header N2PS v1, `kNrd2StageBVersion`, scene id, pose index, held-out
flag, half-res and tile sizes, K, channels; K fp16 feature frames; theta* (12 planes); tile weights
(2); oracle and default losses (2 + 2); CRC-32. About 44 MB a pose at 1766x994. Each written pose logs
`[NRD2] capture pose N written: ...` with per-signal "oracle beats default on a/b tiles" and mean
losses; the last logs `[NRD2] capture finished`.

Usage: `--denoiser 2 --cam-wander AMP SPEED --set post.autoExposure 0 --nrd2-capture DIR|default POSES
[HOLD] [HELDOUT_FROM] [--nrd2-oracle grad|grid]` on the open level. GPU memory while capturing: about
4 x (D, S and pyramids) + 22 fp32 planes per pixel + feature readbacks (~390 MiB at 1766x994).

## Phase 4 as built (2026-10-06): training and inference

Code: `Nrd2Trainer` (CPU pieces and the GPU session), `Nrd2Network` (inference), `shaders/nrd2_net.hlsl`
(record gather, standardise in, de-standardise out), editor `sandbox/src/Nrd2Session.*` (Tools > Train
Neural Denoiser, `--nrd2-train`). Tests: `Nrd2TrainerTest` (CPU) and `Nrd2TrainerGpuTest` (WARP: a short
session on synthetic poses, the saved held-out ratio against the CPU twin, inference against the CPU twin,
gate, resume, cancel); `NeuralConvTest` / `NeuralGpuParityTest` cover the per-element loss weight. Not yet
run in the engine or on real poses.

**Parameter space only** (rule 4). The loss, validation, checkpoint choice, early stop and live gate all
compare predicted tile parameters with the oracle's; nothing runs the network's output through the resolve.

**Network** (`nrd2NetworkDesc`): 12 -> 3x3 s2 16 -> 3x3 16 -> 3x3 s2 32 -> 3x3 32 -> 1x1 12 (ReLU, linear
head, 18,348 weights). Input the 12-channel half-resolution feature tensor (4 tilesX x 4 tilesY), output
tilesX x tilesY x 12 = the tile-parameter planes (plane = signal x 6 + field).

**Standardisation** (saved as the AVNN v2 io affine): per input channel, mean and std over every texel of
every frame of the training poses; per parameter, mean and std weighted by the signal's tile weight (std
floors 1e-3 / 1e-2). Inputs x * (1/std) - mean/std, targets likewise; the head predicts standardised
parameters.

**Records**: 56x56 half-resolution patches (14x14 tiles) at tile-aligned origins; texels past the pose's
edge read 0 in standardised space (the conv's padding at inference; exact for the first layer, the 3-tile
border of a frame is approximate). Targets on all 14x14 tiles, weights only on the central 8x8 (tiles
3..10), per tile and per signal (D's weight for parameters 0..5, S's for 6..11, `ConvLossWeight::PerElement`).
A non-finite theta has weight 0. Core tiles of a record are bit-identical to a full-frame forward
(`Nrd2TrainerTest`).

**Sampling**: batch 32, scenes take turns record by record over the resident poses; pose, frame and core
origin from a hash of (seed, step, record). Deterministic given the residents, which change on a fixed
step schedule.

**Pose cache**: one GPU buffer per pose ("slot": fp16 features, theta, tile weights as one
`StructuredBuffer<uint>`), held-out poses (frame 0 only) in at most a third of `vramBudget` (1.5 GB),
training poses in the rest. More training poses than slots: a worker thread reads the next pose, a 3-buffer
staging ring uploads it, one resident is replaced every 62 steps (training waits if the read is late, so
the schedule stays fixed). One upload per frame.

**Training** (`IRenderFeature::prePass` of a passive feature the session registers, outside any render
pass; GPU span "NRD2 training"): per step the 32 records are gathered (`CSNrd2Gather`, one dispatch each)
and `ConvNet::recordTrain` runs one Adam step; learning rate 5e-4 / (1 + lifetime steps / 1000), floored
2e-5 (NeuraFI's); EMA 0.995 for inference; loss norm 32 x 12 x 64. Steps per frame adapt to `gpuBudgetMs`
(8 ms) from `IDevice::gpuTiming` deltas of that span (a validation batch counts 0.35 of a step); without
timing, 2 a frame. Every 50 steps a training-batch loss is read back (4 frames later).

**Validation** every 250 steps, training paused: the EMA weights over every held-out pose's records whose
cores tile the frame once (`recordEvaluate`, loss norm 1), read back 4 frames later. Metric: sum w (p' -
t')^2 over the default parameters' sum w (d' - t')^2 in the same standardised space, per scene and overall.
Log: `[NRD2] train step N lr X loss Y | val V (default Z, ratio R)` (V and Z per unit weight).

**Checkpoints**: a new best ratio saves `nrd2_v1.avnn` (EMA weights + io affine) and `nrd2_v1.avnn.steps`
(`lifetimeSteps valRatio datasetId bestRatio evalsSinceBest`); every 500 steps and at the end
`nrd2_v1.last.avnn` (master weights) + sidecar. Writes go to a temporary file then rename. Early stop after 8
validations without improvement. Resume from `.last` when its dataset id (FNV-1a over scene, pose index,
held-out flag and CRC of every pose) matches; Adam restarts, lifetime steps continue. Cancel validates
where it stands, saves `.last`, then stops; editor exit saves the last read-back weights as `.last`.
Scenes with no held-out pose hold out every 4th (logged).

**Inference** (`Nrd2Network`, in `Nrd2::record` after the pyramid when `voxi.nrd2Network` is on and not
bypassed): `CSNrd2Features` (stores the tensor already standardised with the weights' input affine; the
capture asks it for the raw features instead) -> `ConvNet::recordInfer` (EMA) -> `CSNrd2NetOut` (output affine; logits clamped +-8, log2 sensitivities +-6; non-finite -> the defaults) into
the tile-parameter buffer; GPU span "NRD2 network". Defaults instead (said once) when the passes or the
network do not build, there are no weights, the file will not load or has no affine, the gate is closed, or
a buffer will not allocate. Weights: `%LOCALAPPDATA%\AverEngine\nrd2_v1.avnn` over `bin/data/nrd2_v1.avnn`
(CMake deploys `modules/render.denoise/data/nrd2_v1.avnn` and its `.steps` when they exist), re-read when
the file changes (polled every 120 frames). **Live gate** from the sidecar's held-out ratio: on at <= 0.8,
off above 0.9, unchanged between; no sidecar = off. Resolution change: bindings invalidated, tensors
re-reserved.

**Editor**: Tools > Train Neural Denoiser... (dataset folder, poses and held-out counts per scene, steps,
cache size, GPU ms a frame, resume; Start / Cancel; live status), a sticky toast with progress and Cancel,
and an Editor Preferences line (weights source, steps, held-out ratio, gate, in use).
CLI: `--nrd2-train STEPS [DATASETDIR...]` (steps this session; no dirs = the default dataset folder),
bounded with `--frames N`. Training needs the device, not NRD2 as the active denoiser.

Not yet: NRD2 in-frame on Vulkan (the trainer itself is portable compute).

## Plane-predicted tap depth (2026-10-06)

Upsampling taps are accepted by depth against the depth the pixel's surface plane predicts at the tap's
centre (`nrd2DepthSlope`: per axis the smaller one-sided depth difference), not against the pixel's own
depth. A tilted floor, wall or vault no longer rejects its coarse neighbours as if they were edges. The
oracle fits this resolve, so `kNrd2StageBVersion` is 2 and older captures are refused. The shipped network
v2 was trained against the previous form and still applies; a recapture and retrain would match it exactly.

## Network v3: sun outside D, no Caldera (2026-10-12)

Retrained because the sun left D (`2e538bff`). Caldera_Capital is Activision's non-commercial dataset, so no
shipped weights are trained on it: v3 uses 168 poses in 6 scenes, 126 training and 42 held out --
CyberCity2099 Day and Night, NewSponza and NewSponza_Night, NewSponza and NewSponza_Night with Path Tracing.
Trained from scratch; it stopped early at 9,750 steps with a best held-out ratio of 0.467 (about 1.5 minutes
on the RX 7800 XT), and a resumed session found nothing better. The capture now waits for the `--open-level`
level and ~10 s of streaming; before, it captured the startup level.

In-engine stills against a Path Tracing reference (2,500 frames, fixed exposure, scale 0.5) match a variant
that was also trained on Caldera, to four decimals: NewSponza MAE 0.0047, NewSponza_Night 0.0006,
CyberCity Night 0.0006, and Caldera (evaluation only, never trained on) 0.0090 vs 0.0091. At rest the
temporal stage dominates; the network matters on new pixels.

## Network v2: ray-traced and Path Tracing (2026-10-06)

v1 saw almost no Path Tracing input. v2 was trained on 256 poses in 8 scenes: NeonDistrict Day and Night
and NewSponza and NewSponza_Night, each captured twice, ray-traced (`--pt 0`) and Path Tracing tier 3. A
capture taken with Path Tracing on is now its own scene (level name + `_PT`), so training stratifies over both
modes and validation reports them separately.

Training ran to 11,250 steps over three resumed sessions, each ending on the early stop. Best held-out ratio
0.690:

| Scene | Ray-traced | Path Tracing |
|---|---|---|
| NeonDistrict Day | 0.736 | 0.708 |
| NeonDistrict Night | 0.730 | 0.700 |
| NewSponza | 0.643 | 0.663 |
| NewSponza Night | 0.712 | 0.698 |

These are not comparable to v1's 0.671, which was measured on a held-out set without Path Tracing.

In-engine check on NewSponza's arcade (scale 0.5, Path Tracing tier 3, still and during a 25-degree yaw swing):

- Brightness is unchanged.
- Fine-scale residual is 0.6% below v1 in every case: 0.1436 vs 0.1445 still, 0.1340 vs 0.1349 swinging,
  and 0.1421 vs 0.1427 swinging ray-traced.
- FidelityFX measures 0.160 and v0.6.0 0.174.

Most of the visible improvement under motion came from the stabiliser keeping its history through swings, not
from the network.

## Network v1 (M9, 2026-10-06)

Trained in-engine (`--nrd2-train 30000`) on 128 captured poses: NeonDistrict Day and Night, NewSponza and
NewSponza_Night, 24 training and 8 held-out per scene. It stopped early at 26,000 steps (8 validations
without improvement), with the best checkpoint at 24,000. That run took about 2 minutes on the RX 7800 XT.

Held-out parameter error, as a ratio to the default parameters, was 0.671 overall:

| Scene | Ratio |
|---|---|
| NeonDistrict Day | 0.675 |
| NeonDistrict Night | 0.676 |
| NewSponza | 0.650 |
| NewSponza Night | 0.690 |

The ratio was already 0.78 at step 500. The weights ship as `modules/render.denoise/data/nrd2_v1.avnn`
plus its `.steps`, and the live gate is open.

**Image rig** (D3D12, render scale 0.5, no TAA, tonemap and auto-exposure off, NeuRAA off, 400 frames):

- Speckles are per mille.
- Energy and error are against the reference path tracer (`voxi.ptMode 1`) on the still frame.
- "Blob" is the error after a 9 px box filter.

| | FidelityFX | NRD2 defaults | NRD2 network |
|---|---|---|---|
| NeonDistrict Night, speckles moving / still | 0.49 / 0.11 | 0.23 / 0.17 | 0.21 / 0.14 |
| NeonDistrict Night, energy, pixel / blob error | 0.983, 0.044 / 0.027 | 0.990, 0.077 / 0.059 | 0.990, 0.068 / 0.052 |
| NewSponza, energy, pixel / blob error | 1.007, 0.034 / 0.024 | 1.036, 0.110 / 0.060 | 1.018, 0.073 / 0.037 |
| GPU denoise (moving), Night / Sponza | 2.01 / 1.83 ms | 0.35 / 0.31 ms | 0.82 / 0.73 ms |

The network's 0.82 ms on Night breaks down as features 0.12, network 0.32 and resolve 0.38. NewSponza's
speckle counts are about 0.01-0.02 in every mode.

The network beats NRD2's fixed defaults everywhere: Sponza blob error drops 38% and its energy bias halves.
In motion it beats FidelityFX on speckle, energy and cost. At rest FidelityFX is still closer to the
reference, because its history accumulates. That is the price of being single-frame, which is also why
NRD2 cannot smear. The next levers are more poses per scene, more scenes, and a second network input
level (1/4).

## Input despeckle (2026-10-06)

**Problem.** The owner saw soft spots only where candles light the scene (NewSponza Night), worse in motion.
With the lamps off (`voxi.localLights 0`), the light left is candle emission found by GI rays, and it showed
exactly those spots. The raw input (`voxi.nrd2Bypass 1`) has them as lone bright samples, each a small cross,
because the half-rate fill copies one traced sample into its four neighbours. The pyramid is plain 2x2
averaging, so a sample F becomes F/4, F/16 and F/64 at 1/2, 1/4 and 1/8, and the resolve spreads that into a
soft 8-16 px disc. FidelityFX sees the same samples but down-weights bright outliers and averages them over
time on the raw signal. NRD2's temporal stage runs after the resolve, when the disc already exists. ReSTIR
GI's temporal reuse also keeps the same sample in place for up to 30 frames, so the stage cannot average it
away either.

**Fix.** `CSNrd2Despeckle` (pass 8) runs before the pyramid. For D and S separately it caps each pixel at
`voxi.nrd2DespeckleCap` (default 1) times the 5th brightest of its 24 neighbours in a 5x5, and the pyramid,
resolve and features read the result.
- The 5th brightest is past the 4-pixel fill cross, so the whole cross goes.
- An area brighter than a few pixels has 5 or more bright neighbours and stays.
- A 1-pixel-wide bright line in S (a thin highlight) would be capped. None was seen on the rig; use
  `voxi.nrd2Despeckle 1` (diffuse only) if one turns up.

It is single-frame and an order statistic (no mean or sigma). It never sees history. The energy in a capped
sample is lost: the rig showed less than the blotches had added.
- Implementation: one 8x8 group with a 12x12 groupshared apron of luminance, and two RGBA16F targets
  allocated on the first frame.
- Cost: 0.08 ms on the RX 7800 XT at 1766x994.
- Captures keep Stage B's raw values (despeckle off while one runs), so training data is unchanged.

Dials: `voxi.nrd2Despeckle`, 0 off, 1 D, 2 S, 3 both (default); `voxi.nrd2DespeckleCap` 1-8 (default 1).
The cap trades spots for light; the owner accepted some colour accuracy for fewer spots.

**Measured** on NewSponza Night, upper gallery (`--cam 600 -900 720 -12 90`, exposure 48, 400 frames). The
metric is the 9 px blob error, mean |log ratio| of luminance after the box blur.

| Case | Despeckle off | Despeckle on | FidelityFX |
|---|---|---|---|
| Lamps off, still, against the reference | 11.2% | 3.8% | 3.9% |
| Lamps on, still, against the reference | 2.35% | 2.23% | 0.83% |
| Lamps on, 4 degree swing (`--cam-wobble 4 60`), against FidelityFX | 3.4% | 3.2% | n/a |

With lamps on, the remaining gap at rest is edges and fine texture, not spots: FidelityFX keeps the Path
Tracing accumulation and the Voxi histories, and NRD2 does not. In the swing, the old frames had soft glow
blotches behind the candles and the new ones do not; FidelityFX shows fine sparkle along edges instead.

**Second round (cap and lamp rate).** Measured in motion against a Path Tracing reference at the same pose:
the swing's captured frame is frame 397, yaw 87.649, found by matching still frames.
- "Spots" is the share of the viewport where the 9 px blur is more than 10% brighter than the reference.
- "Bias" is the mean log ratio.

| Setting | Spots, moving | Spots, still | Bias, moving |
|---|---|---|---|
| Despeckle off | 3.66% | 3.34% | +1.1% |
| Cap 2, half-rate lamps (first commit) | 2.23% | 1.96% | +0.3% |
| Cap 1.5, full-rate lamps | 1.07% | n/a | +0.05% |
| **Cap 1, full-rate lamps (default)** | **0.45%** | **0.32%** | **-1.7%** |
| Cap 1, full-rate lamps, ReSTIR history 0 | 0.22% | n/a | -4.4% |
| FidelityFX | 0.92% | 0.17% | -2.0% |

With cap 1, candle-only light (lamps off) loses 6% of its energy; FidelityFX loses none there. Full-rate lamp
shadows (the `nrd2HalfRateLamps` setting, since removed) cost about 0.1 ms. The whole change is +0.3 ms on the RX
7800 XT at 1766x994, with the despeckle at 0.09 ms of that.

Another finding from the same session:
- **A reference needs `--denoiser 1`.** Under NRD2 the Path Tracing accumulation is compiled out, so a
  `voxi.ptMode 1` reference rendered with NRD2 selected is only NRD2 on one Path Tracing frame. Render it with
  FidelityFX selected (`--denoiser 1 --set voxi.ptMode 1`, 1500 frames). Earlier same-day figures that put NRD2
  closer to the reference than FidelityFX used such a reference and do not hold.

**Darkening at the default view.** NewSponza Night's own camera is lit mostly by candle GI, so the cap's lost
energy shows there. With the coarse-level cap 0, against the Path Tracing reference:

| Despeckle | Spots | Bias |
|---|---|---|
| Off | 5.8% | -0.5% |
| Cap 2 | 2.4% | -2.7% |
| Cap 1.5 | 1.65% | -3.7% |
| Cap 1 (default) | 0.53% | -7.1% |
| FidelityFX | 1.23% | +0.2% |

`voxi.nrd2DespeckleCap` is the trade.

**Tried and reverted: returning the clamped light.** Summing it per 8x8 tile and adding it back in the resolve
(bilinear across tiles, depth- and normal-weighted) overshot to +1.7% to +2.0% and put the light back as
tile-sized blotches (11.7% spots at the default view), for 0.27 ms. Getting the light back needs a much wider
and better-founded spread than one tile.

## Speckle blur (2026-10-06)

The owner asked for a ReBLUR-style blur as a selectable speckle removal: Render settings, Denoising,
**Speckle Removal**, None or Blur. The default is None: Blur measured worse (below). Console: `voxi.nrd2Speckle` (0 none, 1 blur) and
`voxi.nrd2BlurRadius` (1-32 px, default 8).

How it works: `CSNrd2Blur` (pass 9) runs after the resolve, on its demodulated D' and S'. Without the temporal
stage it writes the lit target; with it, the temporal stage reads the blurred D' and S'.
- Kernel: a fixed 3-ring, 24-tap Gaussian (sigma = radius / 2).
- Edge stops: depth against the plane-predicted depth (exp(-30 |dz| / z)) and normal (cos^8, loose because the
  G-buffer normals carry the normal maps; cos^32 left the blur almost inert on stone).
- No luminance stop, so a blotch spreads out instead of being kept.
- Specular blurs over radius x saturate((roughness - 0.1) / 0.3), so mirrors are not blurred.
- It is off while a capture runs.

**Not ReBLUR.** ReBLUR picks its radius per pixel from the accumulated frame count and hit distance. The
history-count part is exactly what NEURAA_NRD.md rule 5 forbids, so this radius is fixed and the blur uses this
frame only.

Measured on NewSponza Night, upper gallery, 4 degree swing, against the Path Tracing reference at the same pose:

| Setting | Spots | Bias |
|---|---|---|
| None | 0.52% | -2.0% |
| Blur 8 px | 0.70% | -1.8% |
| Blur 16 px | 1.13% | -1.6% |

- Cost: 0.60 ms on the RX 7800 XT at 1766x994.
- Spots are counted only below the viewport toolbar, which differs between editor sessions.
- In that view the remaining error is lamp light spread past shadow edges, which the blur adds to rather than
  removes. The despeckle had already taken the blotches there. The setting is for views where blotches
  remain; judge it there.

## Path Tracing accumulation (2026-10-06)

In Path Tracing (ReSTIR PT, `--pt` > 0) the spots stayed at rest. FidelityFX mode averages the path-traced
frames while the view holds, and NRD2 mode had that compiled out, so NRD2 only ever saw one frame.

**What it does now.** Under NRD2, Stage B averages the noisy halves instead: demodulated D and S each get a
running mean in u22. The buffer is now two planes:
- plane 1: D, with the count and depth in w;
- plane 2: S.

The same keep rule as FidelityFX mode applies, and NRD2 then filters the averaged input. While a pixel keeps
(`gPtBounceParams.y` 2), the **network is off** for the frame and the default tile parameters run
(NEURAA_NRD.md rule 10: it never sees history colours). This is the progressive path tracer, not a denoiser
history, and it is the same accumulation FidelityFX mode ships (its jitter order, rule 11, is unchanged and
still open).

**Measured** on NewSponza Night, upper gallery, `--pt 3`, 400 frames, against FidelityFX mode accumulated
for 1500 frames:

| | Before | After |
|---|---|---|
| Blob error | 2.36% | 1.38% |
| Pixel MAE | 0.0089 | 0.0065 |
| Bias | -2.3% | -0.2% |

Spots above 10% went from 0.26% to 0.36% at this threshold. FidelityFX at 400 frames: blob error 2.53%, bias
+4.0%. The darkening is gone because the despeckle sees the averaged input, which has few outliers. Moving
frames are unchanged, since the accumulation restarts.

## Converge at rest (2026-10-10)

Under Path Tracing at rest the filter kept blurring an input that had already converged: on Caldera's arcade
at 1200 still frames NRD2 smoothed the normal-mapped brick that the accumulated input showed crisply. Stage B
now stores each pixel's accumulated frame count in D's alpha (`1 + frames / 16384` while the albedo is usable;
every `a > 0.5` test still holds, and the pyramid weight saturates it), and `CSNrd2Converge` (pass 10, after
every other stage) blends the final lighting toward the pixel's own accumulated input: none below
`voxi.nrd2Converge / 16` frames, all of it at `voxi.nrd2Converge` (default 128; 0 = off). A pixel that
restarted (moved, disoccluded) has no count and stays filtered. It reads `lit_` back through its UAV, so it
needs `DeviceCaps::typedUavLoads`.

Measured on Caldera_Capital's arcade, Path Tracing tier 3, render scale 0.5, against the unfiltered input
after 1200 still frames (scratchpad cmp.py):

| | Pixel MAE | Fine detail (ref 35.14) |
|---|---|---|
| Filtered, 96 frames | 0.0132 | 32.58 |
| Filtered, 1200 frames | 0.0119 | 32.63 |
| Unfiltered input, 96 frames | 0.0062 | 35.46 |
| Converge 512, 300 frames | 0.0068 | 33.53 |
| Converge 512, 1200 frames | 0.0001 | 35.15 |

The unfiltered mean beats the filter from about 100 frames on, hence the default of 128.

## Sun outside the filter (2026-10-10)

The owner: NRD2 is better overall than FidelityFX but less crisp. Two reasons, found in the code:

- **The sun went through the filter.** Its lighting is noise-free apart from its shadow's penumbra, but it
  sat in D and S, so the pyramid and resolve (1/2 to 1/8 levels) smoothed the normal-mapped and contact
  shading the sun lights. FidelityFX filters only the noisy terms.
- **No temporal help under TAA.** The stabiliser runs only on jitter-free frames (the Arm US 18/497,608
  design-around, NEURAA_NRD.md). Since TAAU and its jitter run in motion too, jitter is never zero with TAA
  on, so NRD2 is single-frame throughout and the spatial filter carries all the noise. Changing this is the
  owner's call (counsel, or the un-jitter design-around); not done.

**Fix (the first).** `Settings::nrd2SunClean` (default on, `voxi.nrd2SunClean`; NRD2 frames set bit 1024,
`rtNrd2SunClean`): `nrd2ShadeSplit` adds the exact light's diffuse, subsurface and specular to the clean term
(beside emissive, fogged and glass-pathed the same way) instead of D and S, so NRD2 filters indirect light and
the lamps only. The sun's visibility, which NRD2 frames trace raw (no shadow history), is averaged in Stage B
over a depth-weighted 5x5 (`rdSunVisFiltered`, the lamp shadows' tap weights): the penumbra band softens by two
pixels and the lit shading is untouched. Not under Reference path tracing. Patent-neutral: a spatial
visibility filter, no history, no jitter.

**Expect** crisper sunlit surfaces (brick, normal maps, contact detail) in motion and at rest; penumbras a little
noisier than FidelityFX's (no temporal shadow history: its texture u2 is NRD2's remodulation target on these
frames). The network was trained with the sun inside D: retrain in-engine (NRD2.md "Training") for its best,
though the resolve is unchanged. Not measured yet (owner tests): `voxi.nrd2SunClean 0` is the A/B.

**Fix (2026-10-11): it never took effect.** Under unified lights (UNIFIED_LIGHTS.md) Stage B's `sun` carries no
radiance; the sun is the pixel's exact light, shaded by `rdListLightSplit` straight into D and S. So the sun still
went through the pyramid and resolve, and with the 5x5 mean on its visibility, thin shadows (tram wires, poles,
palm leaves) faded on NRD2 frames while FidelityFX frames kept them. Now a directional exact light goes to the
clean term, and its visibility takes `rtShadowSpatial`'s kernel instead of the 5x5 mean: a Gaussian of
`rtShadowDenoise`'s radius (sigma = radius / 2, at most 2), depth-weighted. A lamp as the exact light (night)
stays in D with the 5x5 mean, as before. On Caldera_Capital's dock view (Epic, scale 0.5, 400 frames) the NRD2
frame's mean absolute difference from FidelityFX halved (2.16 to 1.09 of 255) and the wire and pole shadows match.

## Many-light tail (2026-10-17)

With `voxi.manyLights` live (more than `voxi.manyLightsMin` lights, NRD2 frame; `MANY_LIGHTS.md`), Stage B's D and S
gain one more input: `CSRdManyLights` writes the stochastic tail's radiance, diffuse and subsurface into
`gRdManyDiff` (u25) and specular into `gRdManySpec` (u26), and Stage B adds them where it added `rdTailLightsSplit`'s
output, before demodulating (`nrdD / kdAlbedo`, `nrdS / FssEss`), so the filter sees them like any lamp light. The
exact light still goes the way it did (to D and S, or to the clean term for the sun). Nothing in the filter changes.

What the filter now sees: a single-frame stochastic estimate per pixel with up to `S` rays and `M` candidates, noisier
than the capped grid tail it replaces and unbiased where that was biased low. Its candidates come from the pixel's own
tree descent and nothing is reused from other pixels or frames, so this input needs no history (the filter has none) and
adds no new rule: `NEURAA_NRD.md` rule 18. **The network was trained on the grid tail's noise model**; default
parameters apply to the new noise until it is retrained in-engine (Training, below). The despeckle (cap at 2x the 5th
brightest of 5x5) is what bounds what a rare bright candidate leaves behind; `voxi.lightWeightClamp` is the knob if
sparkle remains. Not measured yet.

## Object stop, shared with NeuRAA (2026-10-10)

NRD2's stops were depth and normal only, so where two objects meet at a similar depth and facing (a post on a
wall, a crate on a floor, a sign on a facade) the 1/4 and 1/8 levels mixed their lighting across the contact,
which reads as softness. NeuRAA already knows objects: the primary-visibility record (`rhi::PrimaryVisibility`,
Voxi's visibility stage) with a tree's parts as one object. That definition now lives once in
`modules/rhi/shaders/aver_visibility.hlsli` (`averObjectOf`), read by NeuRAA's edge classes and by NRD2.

`Settings::nrd2ObjectStop` (default on, `voxi.nrd2ObjectStop`; `Nrd2Params::objectStop`, `Nrd2::Inputs::visibility`):
- `CSNrd2Pyramid` reads the visibility buffer once (UAV u13, as NeuRAA binds it) and writes each pixel's object
  (`objFull_`, R32Uint) and each level texel's (`objLevel_`): a texel takes its nearest child's object and averages
  only that object's children.
- `CSNrd2Resolve` (`nrd2UpsampleObj`): a level tap of another object keeps no bilinear weight, so its confidence
  drops and the combine leans on finer levels and the pixel itself.
- `CSNrd2Prefilter` (stabiliser): taps of another object are skipped.

Inference only, like the level caps: training, the capture and the CPU twin never see it (captures turn it off).
Off, or with no visibility this frame, every object is 0 and the frame is as before. Cost: one buffer read per
pixel and two uint compares per tap; 4 B a pixel plus a third of that for the levels.

**Expect** crisper silhouettes and contact edges, slightly more noise in a thin band along them (fewer samples
there). `voxi.nrd2ObjectStop 0` is the A/B. The temporal stage logs `[NRD2] temporal stage running` / `off (why)`
when it changes (Sandbox.log), which shows whether it runs on a given setup (TAA jitter stops it; NeuRAA and FSR
do not jitter).

## Sharing with the other neural passes: what was not done (2026-10-10)

Looked at and dropped with the owner after reading the code:
- **One device-owned previous-frame G-buffer history** for NRD2's stabiliser and NeuraFI (NeuraFI copies view Z
  and motion into a ring of three; the stabiliser keeps view Z and the normal). It would save about 8-12 bytes a
  pixel (15-20 MB at 1766x994) and one copy, but means reworking the G-buffer's resource states at the end of the
  D3D12 frame and NeuraFI's history handling. A shared reprojection-validity pass does not fit either: NeuraFI's
  gather deliberately has no validity test (NEURAFI.md, its patent design).
- **NeuRAA on `render.neural`.** Its 36-32-32-9 MLP runs inline per edge pixel in the resolve; `render.neural`'s
  networks are buffer-in, buffer-out compute passes, so moving it adds two passes and a 36-float record per edge
  pixel for the same image.
- **Un-jittering NRD2's input.** NeuRAA and FSR request no jitter; only TAA does, and the stabiliser already runs
  without it.

## History in motion (2026-10-10)

The owner: patchiness in shadows and bounce lighting while moving. The stabiliser's history ramps from
`nrd2StabFrames` (64 since 2026-10-14, was 32) at rest down to a fixed 8 frames at 8 px/frame and faster, which is nearly always while
flying; 8 frames do not average out the low-frequency blotches the coarse levels and one-ray lamp and bounce
noise leave. The floor is now `Settings::nrd2StabFramesMoving` (default 16, `voxi.nrd2StabFramesMoving` 1-64;
`Nrd2Params::stabFramesMoving`, sent as `gNrd2Stab.y`): a speed-dependent cap, not a per-pixel count, so the
shader, its CPU twin and NEURAA_NRD.md rule 5 are unchanged. Static geometry reprojects exactly under camera
motion and movers fail the history's depth test, so the cost is lag on lighting changes while moving, bounded by
the clip box (FidelityFX's mean +- sigma box by default, the 5x5 min/max box with `voxi.nrd2FfxClip 0`) and the
colour reject. Since 2026-10-12 the count is FidelityFX's `n = min(N, n_hist * factor + 1)`: this cap `N` is
unchanged and the disocclusion factor can only shorten it ("History handling after FidelityFX"). Try 24-32 if
patches remain; lower if lamp flicker or a sun step trails.

## Combine reference and 1/4-level cap (2026-10-06)

From the FidelityFX study (a read-only Sonnet workflow, 2026-10-06). FidelityFX has no pyramid: its prefilter
reaches 3 px and trusts the local pixel. In NRD2's combine, the luminance term compared each candidate with the
**coarsest level**. Around a lone bright sample that level already holds its energy, so the neighbours' own
values gave way and the sample grew into a disc.

**Changes, both at inference only.** `nrd2_capture.hlsl` (the oracle) keeps reference 0, and training is unchanged.
- **Reference:** the median of the usable candidates (own pixel and three levels), an order statistic
  (`voxi.nrd2CombineRef 1`).
- **1/4 level:** its logit is capped like the 1/8 level (`voxi.nrd2MidCap 0`).

**Measured** on NewSponza Night against the Path Tracing references (spots as in "Input despeckle"):

| View | Before | Both | Bias change |
|---|---|---|---|
| Gallery, still | 0.17% | 0.12% | -0.1% |
| Gallery, moving | 0.33% | 0.28% | -0.07% |
| Gallery, `--pt 3` still | 0.36% | 0.26% | -0.09% |
| Default view, still | 0.53% | 0.35% | -0.4% |

Each change alone gave about half of the gain. Blob error is unchanged in the gallery; PT goes 1.38% to 1.36%,
and the default view 5.23% to 5.37% (its extra darkening).

Not adopted from the study:
- a luminance-aware pyramid reduce, which changes the network's input statistics;
- FidelityFX-style soft weights in the despeckle;
- running the temporal stage at rest under TAA (rule 11, counsel).

The study also listed what must not be copied: `ClipAABB` and the variance-based history tests, among others.
The owner reversed that for the temporal stage on 2026-10-11 ("History handling after FidelityFX").

## Coarse-level cap (2026-10-06)

What is left at rest is not noise. NRD2 puts a bright halo around lamps (a lantern's dark rim filled in) and
brightens the glow around bright areas. That is the 1/8 pyramid level mixing lamp light across near-field
shadow edges.

**Fix.** An inference-only cap on that level's logit: `min(logit3, voxi.nrd2CoarseCap)`, default 0. It
applies to the network's tile parameters and to the hand defaults. Training, the oracle and the CPU twin see
the parameters uncapped. The constants buffer grows to 336 bytes (`gNrd2Extra`).

**Measured** on NewSponza Night, upper gallery, network on, Speckle Removal None, against the Path Tracing
reference. Spots are counted as in "Input despeckle".

| | Still | Moving (4 degree swing) | Bias, moving |
|---|---|---|---|
| Uncapped (logit3 up to 16) | 0.37% | 0.52% | -2.0% |
| Cap 0 (default) | 0.17% | 0.34% | -2.2% |
| Cap 0, plus Blur 8 px | 0.35% | 0.49% | -1.9% |
| FidelityFX | 0.20% | 1.01% | -2.2% |

- Overall blob error is unchanged (2.27% to 2.28% moving), and so is fine noise.
- With the network off, lowering the default 1/8 logit from 2 to 0 gave the same numbers, and -1 gave no
  further gain; the network did no better than the defaults here.
- The pixel-shift check (the owner's idea): the frame matches the reference best at offset (0, 0) for NRD2
  and FidelityFX alike. On lighting-scale blur, the per-tile best offsets of the spot tiles scattered evenly
  around zero (26 of 110 tiles at (0, 0), the rest split between the four neighbours) for about 5% gain:
  noise, not a consistent shift.

## Temporal stabiliser (2026-10-06, rewritten 2026-10-06 after FidelityFX's design)

The owner asked for smoothing of the speckle that remains in motion, where there is no temporal stage at
all: the camera moving zeroes the TAA jitter and TAAU falls back to its spatial path with no history, and
FidelityFX is off. The first stabiliser (one history blend inside a 7x7 min/max box, 12 frames) helped, but
FidelityFX still looked smoother, so the stage now follows **AMD FidelityFX's reflections denoiser's temporal
design** (MIT; reproject, prefilter, temporal resolve). It is a clean-room rewrite in NRD2's style: the ideas
and constants are FidelityFX's, no code is copied and none of its headers are included. Default on;
`voxi.nrd2Stab 0` is the A/B. It fills the jitter-free gap and nothing else.

**Gate.** Unchanged. It runs when `Settings::nrd2Stab`, the frame's TAA jitter is exactly (0, 0)
(`IDevice::taaJitter`), the G-buffer history is valid, the velocity target and previous matrix are supplied,
and no capture is active. Otherwise the resolve writes the lit target itself and the history is dropped, so
the first stage frame after a rest or a toggle has none. TAAU keeps the camera-at-rest case (TAA on and still:
jitter non-zero, no stage).

**Three passes** on the resolve's D' and S' (irradiance, before remodulation, so albedo detail never blurs; D and
S separately). Each is one 8x8 dispatch (`AVER_NRD2_PASS` 5, 6, 7; GPU spans `NRD2.Reproject`, `NRD2.Prefilter`,
`NRD2.Temporal` inside `NRD2`).

1. **`CSNrd2Reproject`.**
   - History position from the G-buffer velocity (exact for rigid motion; no jitter on these frames). Edge
     handling and the fetch are FidelityFX's since 2026-10-12 ("History handling after FidelityFX"): a history
     texel is scored by FidelityFX's disocclusion factor (normal, and relative depth against the depth camera
     motion predicts), a failed nearest texel searches its 3x3, a still failed one rebuilds from the valid
     texels of the 2x2, and the history is fetched with a 12-tap Catmull-Rom with per-tap validity (the history
     keeps the G-buffer's packed normal and the view Z). A rigid mover fails the camera-predicted depth test and
     gets none; each signal's history may also be dropped by the colour reject.
   - Sample count n (the history length): per signal `n = min(N, n_hist * factor + 1)`, `n_hist` the youngest of
     the taps that carry weight (> 0.1), 1 with no (or rejected) history. `N` = `voxi.nrd2StabFrames` (**64**) at rest up to 0.25 px per frame, a
     log-space ramp to min(8, that) at 8 px, flat to 128 px, none beyond (a whip), and 2 for two frames after
     the sun changes. Specular: `N_S = min(N, max(8, N (1 - exp(-100 roughness))))`, so a glossy lobe still keeps
     8 frames (FidelityFX does) but a rough one the full count. The history blend weight is `1 - 1/n`: 0, 1/2,
     2/3 ... up to 31/32.
   - Noise estimate per signal (FidelityFX's temporal variance, in relative units): `((lumCur - lumHist) /
     max(lumCur, lumHist, scale / 2))^2`, `scale` the tile's anchor luminance; `var = lerp(newVar, prevVar, 1/n)`;
     1 where there is no history.
   - Anchor per 8x8 tile and signal: the weighted mean of the tile's values, weight `max(exp(-0.3 lum /
     tileScale), 0.01)`, `tileScale` the tile's plain mean luminance. FidelityFX uses absolute luminance; ours is
     relative so irradiance units do not matter. Where history is valid, 0.3 of it is mixed into the value the
     anchor sees (FidelityFX does). A fixed-order tree sum in groupshared; one texel per tile (RGBA16F, a = valid),
     read bilinearly at the pixel afterwards.
2. **`CSNrd2Prefilter`.** FidelityFX's prefilter on the resolved D' and S': 15 fixed taps (its Halton offsets,
   radius 3, never rotated), weight = `cos^512` normal stop x `exp(-30 |dz| / z)` depth stop (relative to the
   plane-predicted depth, as the resolve's taps are) x `max(exp(-(0.6 + 0.1 var) |anchor - value| / scale),
   0.01)` radiance stop x `max(0.1, 1 - exp(-4.4 var))`, the centre carrying its radiance stop alone. `scale` is
   the pixel's anchor luminance. Smooth specular (roughness < 0.1, FidelityFX's mirror) is not filtered. The
   noise estimate (itself filtered the way FidelityFX does, weights squared) only weights; it never sets a radius,
   a level or a tap position.
3. **`CSNrd2Temporal`.** `cur` = the prefiltered value after the firefly clamp (2x the brightest 8-neighbour, as
   before). D's anchor is the tile anchor, reshaped by the NeuRaC guide where there is one. By default
   (`voxi.nrd2FfxClip`) FidelityFX's resolve follows: `cur` pulled toward the anchor by `1/(n + 1)`, `cur` and the
   history each clipped to a mean +- sigma box of this frame's 9x9 Gaussian moments (`nrd2TemporalBlendFfx`, the
   formula in "History handling after FidelityFX"), `out = lerp(cur, history, 1 - 1/n)`. With the setting off,
   the previous stage's min/max path: the box is the per-channel **min/max of this frame's 5x5** prefiltered values
   united with the anchor, `cur` and the history are clipped to it. The carried noise estimate is
   `lerp(tempVar(cur, history), prefilteredVar, 1 - 1/n)`, its scale the unguided anchor. Writes the new history
   (D'' + n, S'' + n, view Z + packed normal, noise estimate) and `lit = D'' Rd + S'' Rs`, clamped as the resolve
   does. (On the min/max path the box contains the centre value, so the clip on `cur` is a safety net: the
   firefly clamp and the pull toward the anchor do the despeckling.)

**Differences from FidelityFX** (as of 2026-10-12).
(a) The history and current-value clips are FidelityFX's mean +- sigma by default (`voxi.nrd2FfxClip`); the
min/max order-statistic path stays behind the setting 0 (NEURAA_NRD.md section 7, rules 1, 7 and 13, amended). The
moments are validity-weighted and taken on the prefiltered D' and S'; the colour reject runs per signal where
FidelityFX rejects one. (b) Luminance is **relative** to the tile (anchor weight, radiance stop, noise
estimate), not absolute, because D' and S' are irradiance. (c) **Surface-motion reprojection only**: no
hit-position (virtual) reprojection for mirrors, so smooth reflections can swim against their history; the
firefly clamp, the clip and the 8-frame floor bound it. (d) The 9x9 Gaussian mean and variance estimate is back
(it was dropped with the clip on 2026-10-06): twice, in `CSNrd2Reproject` for the colour reject (resolved D' and
S') and in `CSNrd2Temporal` for the clip (prefiltered), both separable in groupshared. (e) The disocclusion
factor is FidelityFX's, continuous, with its 3x3 search and 2x2 rebuild; the one deviation is that depth is
measured against the depth camera motion predicts (`zExp`), not the current depth. It replaces the binary
cos 0.9 test and the 2% + 1 cm + plane-slope depth tolerance. (f) Additions FidelityFX does not have: the
12-tap Catmull-Rom fetch and the NeuRaC guide.

**Resources.** All allocated on the first stage frame and freed with the targets or when the dial is off, 108
bytes per pixel at render size: D' and S' (RGBA16F, rest as UAVs), this frame's reprojected D and S history
(RGBA16F, n in a), reprojected noise (RG16F), prefiltered D' and S' (RGBA16F, a = noise estimate, -1 = no
surface), two anchors (RGBA16F, one texel per 8x8 tile), and two ping-pong sets of history: D'' + n, S'' + n,
view Z + packed normal (RGBA16F) and the noise estimate (RG16F). About 181 MiB at 1766x994 (an estimate; the
first version was 84). Cost, also an estimate and not measured: about +0.6 ms (0.4-0.9) on jitter-free frames,
none otherwise. The constants buffer grew to 384 bytes on 2026-10-12 (`gNrd2Ffx`, `gNrd2Hist`), plus a tile-sized `anchG_` for the guide.

**Dials.** `voxi.nrd2Stab` (bool), `voxi.nrd2StabFrames` (1-64, default 64); the FidelityFX dials are in the table under "History handling
after FidelityFX". The speed cut-offs, depth
tolerance, search score and every weight constant are at the top of `nrd2_resolve.hlsli`, FidelityFX's values
where it has one, starting values to tune on the rig.

**Not done.** No hit-position reprojection for glossy reflections (a later option, and the parallax rule needs
counsel first); no jitter-aware variant to run under TAAU at rest; perspective cameras only (the view-depth
reprojection assumes a symmetric frustum); the speckle that is specular aliasing or a heavy-tailed sample is
only averaged down, not removed. Risks to look for: ghosting from the longer history (32 frames; a moving
object's shadow on static ground, lamp flicker past the box), mirror reflections swimming, and a softer
look from the prefilter's radius 3.

**Check (owner-run, headed).** TAA off so jitter is 0, a moving camera, `voxi.nrd2Stab` 0 against 1:
speckles per mille and the 9 px blob error; energy against `voxi.ptMode 1` (target no worse than about 0.5%
lost on Night); a fast pan at 8, 32 and 64 px per frame against stage-off frames for lag; mirror and wet-road
reflections while strafing; one lamp toggle and one sun-time step; the three spans (budget 0.6 ms together).
Since 2026-10-12 also, with `voxi.denoiserMode 2`, `voxi.giRestirVisibility 4`, `voxi.rayDrivenStages 2` and TAA
off: A/B `voxi.nrd2FfxClip 0`, `voxi.nrd2FfxReject 0`, `voxi.nrd2HistSharp 0` and `voxi.nrd2NeuracGuide 0`;
tune `voxi.nrd2FfxClipWeight 1` and `voxi.nrd2FfxDepthWeight 8`. Look at disocclusions while strafing (with and
without the guide), lamp-lit rooms (the lamp-dominated guide case), a fast pan for lag, mirrors and wet roads, and
ghosting behind movers. CPU twin: `Nrd2ResolveReference` (`nrd2StabMaxFrames`, `nrd2StabSpecMaxFrames`, `nrd2StabSamples`,
`nrd2StabBlend`, `nrd2StabRelVar`, `nrd2AnchorWeight`, `nrd2TileAnchor`, `nrd2PrefilterWeight`,
`nrd2TemporalBlend`, and the FidelityFX set listed under "History handling after FidelityFX"), checked by
`Nrd2ResolveTest`. Patent mapping: NEURAA_NRD.md section 7, rules 13 and 17.

## History handling after FidelityFX (2026-10-12)

**Owner decision, 2026-10-11.** The temporal stage takes FidelityFX's reflections-denoiser history
handling whole, including the literal mean +- sigma clip, **on by default**, and its colour-distance
reject, **on by default**. The owner accepts the patent exposure (NEURAA_NRD.md section 7, rules 1, 7 and
13, amended the same day). The min/max path stays selectable (`voxi.nrd2FfxClip 0`). NeuRaC feeds the
temporal anchor as a guide ("fill new pixels"), flagged for counsel (rule 17). Built in commit `f1439403`;
design in `docs/plans/neurac_nrd2/DESIGN.md` (sections 2 and 3). **Not yet run in the engine**: whether
motion looks better, and what the guide does, is the owner's to judge.

**Why.** The old reprojection fetched history with a 4-tap bilinear. Every frame the history is resampled at
a fractional offset `f`, a bilinear tap adds a variance of `f (1 - f)` texels squared, and a history
accumulated over `n` frames has been resampled `n - 1` times: `sigma^2 = (n - 1) f (1 - f)`. At `f = 0.5`
and `n = 32` that is `sigma` of about 2.8 px. This is a model, not a measurement: it is the plan's reason that
indirect light blurred in motion, and why the fetch is Catmull-Rom. The old edge handling was a binary normal test (cos 0.9)
with a 2% + 1 cm + plane-slope depth gate; FidelityFX's continuous factor, search and rebuild replace it.

### Edge handling (`CSNrd2Reproject`, pass 5)

Per surface pixel, with `thr = 0.9`:

1. **Disocclusion factor.** A history texel is scored by FidelityFX's
   `nrd2FfxFactor = exp(-|1 - max(0, cos)| * 1.4) * exp(-|zHist - zExp| / zExp * depthW)`: `cos` is the
   angle between this pixel's normal and the texel's stored one, `zHist` the texel's stored view Z, and
   `zExp` the view Z camera motion alone predicts for this surface point in last frame's camera.
   **Measuring against `zExp`, not the current depth, is the one deviation from literal FidelityFX.**
   A texel outside the viewport or with no sample count has factor 0.
2. **Nearest texel first.** `f` is its factor. A pixel at or above `thr` keeps it.
3. **3x3 search.** Below `thr`, the other eight texels around it are scored; a strictly higher factor wins,
   so the centre wins ties.
4. **2x2 rebuild.** Still below `thr`: the four texels around the best one are taken as valid when
   `factor > thr / 2`, their weights (bilinear x validity) renormalised by `max(sum, 1e-3)`, their normals
   and depths blended **unnormalised** (literal FidelityFX) and the factor taken again; below `thr` after
   that, no history (factor 0).
5. **Sample count.** Per signal, `n = min(N, n_hist * f + 1)`, `N` the motion-dependent cap of
   "History in motion". 1 where the signal's history is rejected or missing.

The depth weight is `voxi.nrd2FfxDepthWeight` (default 1.0): with matching normals a relative depth error
of about 10% (`exp(-0.105) = 0.9`) still sits at the threshold, so it is **loose**; the old gate was 2% +
1 cm + slope. Expect smearing across small depth steps; tighten it in the editor, no rebuild needed
(try 8).

### The history fetch

When the best texel passes `thr` and all four texels of the 2x2 core are valid, the fetch is a **12-tap
Catmull-Rom** (Aver's addition; FidelityFX uses bilinear): the 4x4 neighbourhood minus its four corners,
per-tap validity (`factor > thr / 2`), the weights renormalised over the valid taps so a flat history stays
flat, the result clamped to the min and max of the valid taps (no ringing past the data) and floored at 0,
then mixed with the validity-weighted bilinear by `voxi.nrd2HistSharp` (default 1; 0 is bilinear). If any
core tap is invalid, or after a rebuild, the fetch is the validity-weighted bilinear. The weights are the
`sr_taa.hlsl` ones: at `f = 0.5` they are -1/16, 9/16, 9/16, -1/16.

### Colour reject (`voxi.nrd2FfxReject`, default 0; weight `voxi.nrd2FfxRejectWeight`, default 8)

Per signal, FidelityFX's test: the history is dropped when `dot(h - mean, h - mean) >= w * length(var)`.
`h` is the raw 4-tap bilinear of the history at the reprojected position (no validity, as FidelityFX's
`SampleRadianceHistory`), `mean` and `var` the validity-weighted 9x9 Gaussian moments of this frame's D' or S'
(`exp(-3 i^2 / 25)`, `|i| <= 4`, surface pixels only, computed separably in groupshared in `CSNrd2Reproject`).
A NaN rejects. It runs per signal, so D can lose its history while S keeps its own; FidelityFX rejects one
signal only. It is a mean/variance test inside history rejection (NEURAA_NRD.md rule 13).

**Default weight 8, not FidelityFX's 1.5 (2026-10-14).** FidelityFX runs the test on raw per-pixel radiance, whose
9x9 mean is steady. NRD2 runs it on D'/S', the single-frame pyramid resolve, which is correlated over 8-16 px: its
local mean wanders by about one sigma, so at 1.5 a converged history was dropped in blobs every frame and those
pixels restarted from one frame. Measured at rest in Caldera (owner prefs, 0.5 render scale, NeuRAA), mean
frame-to-frame change in display codes: weight 1.5 0.67, 4 0.25, 8 0.18, 16 0.15, reject off 0.13, FidelityFX
0.24. Pose-matched motion (just stopped vs settled, fixed exposure): 1.5 4.73, 8 4.43, off 4.39, so the low
weight bought nothing in motion either. 8 is about a 3-sigma test on the mean of a field that correlated.

**Off by default, history 64 at rest (2026-10-14, same rig).** At weight 8 the remaining pops were half brighter,
half darker: isolated pixels restarting from one frame, still 3x as many as with the reject off. Flicker at rest:
weight 8 0.18, off 0.13, off with `nrd2StabFrames` 64 0.08, weight 16 with 64 0.10; FidelityFX 0.24. Motion
unchanged (just stopped vs settled 4.43 / 4.39 / 4.39). The FidelityFX clip still bounds a stale history. A
FidelityFX-style brightness clamp in the pyramid reduce (each 2x2 child capped at 4x the median) changed neither
(at rest 0.13 -> 0.13, motion 4.43 -> 4.46) and was not kept.

### Clip and blend (`CSNrd2Temporal`, pass 7)

The firefly clamp runs first, as before. With `voxi.nrd2FfxClip` on (default; `voxi.nrd2FfxClipWeight`
default 4, which is Aver's FidelityFX weight), `nrd2TemporalBlendFfx` follows `resolve_temporal`:

```
mean, var = 9x9 Gaussian moments of the prefiltered D' (or S'), validity-weighted
sd        = (sqrt(var) + |mean - A|) * clipW * 1.4            A = the tile anchor (guided for D)
hist'     = ClipAABB(lerp(mean, A, 0.2) +- sd, hist)
cur'      = ClipAABB(A +- sd, lerp(cur, A, 1 / (n + 1)))
out       = lerp(cur', hist', 1 - 1/n)
```

`ClipAABB` pulls a point toward the box centre along the line to it, not per channel. At weight 4 the box
is +-5.6 sigma, so it is loose; lower the weight (try 1) to clip harder. The carried noise estimate and the
remodulation are unchanged.

**The min/max path** (`voxi.nrd2FfxClip 0`) is the previous stage's: this frame's 5x5 min/max of the
prefiltered values united with the anchor, per-channel clamps, no mean or variance in the clip
(`nrd2TemporalBlend`, unchanged, its tests stay). The reject is its own setting: with **both**
`voxi.nrd2FfxClip 0` and `voxi.nrd2FfxReject 0` the stage holds no mean/variance clip or rejection test
(its anchor is still a weighted mean: rule 13). The fetch and the edge handling above apply on either path.

### The NeuRaC guide (`voxi.nrd2NeuracGuide`, default 1)

**What.** For a new pixel the tile anchor `A` is the only information the temporal stage has, and it is a
flat 1/8-level mean. NeuRaC's radiance cache holds the shape of the indirect light. `NeuRaCGuide`
(NEURAC.md, 5.11) hands NRD2 a texture: rgb the cached irradiance / PI at the pixel in D's units, a its
confidence in [0, 1] (`Nrd2::Inputs::guide`, RGBA16F at render size, resting in `NonPixelShaderResource`).

**Formula** (`nrd2GuidedAnchor`, D only, temporal pass only):

```
A_g = clamp( A + c * k * s * (g - gbar),  0.25 A,  4 A )         s = min(1, lum(A) / lum(gbar))
```

`g` and `c` are the pixel's guide and confidence, `gbar` the confidence-weighted guide mean of the tile
(weights: D's anchor weight times `c`, written by pass 5 to `anchG_`, read bilinearly like the anchors), `k`
= `voxi.nrd2NeuracGuideStrength`. The tile's level comes from this frame's anchor; only the spatial shape
comes from the cache. The result replaces `A` in `nrd2TemporalBlendFfx` and in the min/max path alike, and it
acts where `n` is small, because the pull toward the anchor is `1 / (n + 1)`.

**Why this form.** The cache holds bounce light only, but D also holds the lamps' direct diffuse (the sun
is outside D, "Sun outside the filter"). In a lamp-dominated tile (`lum(A) > lum(gbar)`) `s = 1` and the
term is purely additive: the lamp light stays in `A` and the guide adds only the indirect deviation. In an
indirect-dominated tile (`A <= gbar`) `A + (g - gbar) A / gbar = A g / gbar`: the ratio form, which cancels
a scale error in the cache, including the `kD / kdAlbedo` factor the guide cannot see. The 4x clamp bounds
hot spots.

**Exact fall-back.** `c = 0`, no valid `gbar` (an empty guide tile), strength 0, `A = 0` or a NaN guide
return `A` bit for bit. With NeuRaC off, on Vulkan, or the pass unbuilt, `Inputs::guide` is 0, flag 32
stays clear, and the shader takes the no-guide path exactly. The guide only runs on frames the stage runs
(jitter-free).

**What it never does.** The guide enters one place: D's temporal anchor. It never reaches the network, the
level choice, the pyramid, the resolve, the prefilter or S (NEURAA_NRD.md rules 5 and 10 are untouched).
Note that the cache it reads is the one NeuRaC maintains; when the NeuRaC trust network is on, its trust
scales the lookup weights inside the guide as it does anywhere else (NEURAC.md 5.12), and that network is a
different one from NRD2's.

### Settings, passes, constants

| Console | Default | Range | Meaning |
|---|---|---|---|
| `voxi.nrd2FfxClip` | 1 | bool | mean +- sigma clip; 0 = 5x5 min/max box |
| `voxi.nrd2FfxClipWeight` | 4 | 0.1-8 | sigma weight (`history_clip_weight`) |
| `voxi.nrd2FfxReject` | 0 | bool | colour-distance reject, per signal |
| `voxi.nrd2FfxRejectWeight` | 8 | 0.1-16 | its discard weight (FidelityFX: 1.5) |
| `voxi.nrd2FfxDepthWeight` | 1 | 0-64 | relative depth error's weight in the factor |
| `voxi.nrd2HistSharp` | 1 | 0-1 | Catmull-Rom share of the fetch; 0 = bilinear |
| `voxi.nrd2NeuracGuide` | 1 | bool | the guide, only while NeuRaC is live |
| `voxi.nrd2NeuracGuideStrength` | 1 | 0-1 | scales the guide's deviation term |

The threshold 0.9 and the normal weight 1.4 are constants (`kNrd2FfxThreshold`, `kNrd2FfxNormalW`) that
travel in the constants block. Flags in `gNrd2Tiles.z`: 32 guide, 64 clip, 128 reject. Constants grew to 384
bytes (`gNrd2Ffx`, `gNrd2Hist`). Bindings: `CSNrd2Reproject` 10 SRV / 6 UAV (t9 the guide, u5 `anchG_`, the
guide's tile mean, RGBA16F at tile size), `CSNrd2Temporal` 12 SRV / 5 UAV (t10 the guide, t11 `anchG_`); a 1x1
stand-in is bound when there is no guide. Both passes peak at 12 KB of groupshared (compile-time guard at
16 KB). CPU twin: `Nrd2ResolveReference` (`nrd2FfxFactor`, `nrd2MomentWeight`, `nrd2CatmullRom`,
`nrd2ClipAabb`, `nrd2FfxReject`, `nrd2TemporalBlendFfx`, `nrd2GuidedAnchor`, `nrd2Moments`, `nrd2FfxSearch`,
`nrd2FfxRebuild`, `nrd2HistFetch`), checked by `Nrd2ResolveTest::testTemporalFfx`, which also compares its
constants with the shader's literals.

### Cost (estimates, not measured)

Increments over the previous temporal stage (itself an estimate of about 0.6 ms), 1440p, RX 7800 XT:

| Part | Estimate |
|---|---|
| `CSNrd2Reproject` (edge handling, 12-tap fetch, moments) | +0.15-0.3 ms |
| `CSNrd2Temporal` (16x16 halo, moments) | +0.1-0.2 ms |
| Guide pass (`NeuRaCGuide`) | +0.1-0.3 ms |
| **Total with the guide** | **+0.35-0.8 ms** |

Read the spans `NRD2.Reproject`, `NRD2.Temporal` and "Voxi NeuRaC guide" with `--gpu-timing`. The guide
texture is about 29 MB at 1440p, owned by `NeuRaCGuide`.

## The network

- **Shape.** A small encoder over 1/2-resolution inputs (3x3 convolutions, ReLU, stride-2 down to 1/8),
  then 1x1 heads to the per-tile outputs. Roughly 4 layers of 16-32 channels; target inference
  0.3-0.5 ms at 1766x994 on the RX 7800 XT.
- **Inputs.** Per pixel: log-luminance of D and S at this frame, guides (view Z relative to the tile,
  normal, roughness, albedo luminance, hit distance / view Z). No history of any kind, no frame time.
- **Outputs.** Per 8x8 tile only: level logits and edge sensitivities for D and for S. Never a per-pixel
  weight map, never an image (rule 6 of NEURAA_NRD.md section 7, Pixar/Disney US 10,672,109).

## Training, in-engine

1. **Capture session** (`--nrd2-capture`, phase 3): the camera travels a
   path and holds at poses. At each hold the renderer keeps rendering the same estimator and averages
   it on the GPU (256+ frames): that mean is the target, the single frames are the inputs. Same
   estimator, so the target is exactly what a perfect denoiser of that estimator would return.
2. **Oracle fit** (GPU): per tile, gradient descent on the resolve's parameters against the mean, over
   several noisy frames of the pose. This fits parameters, not a model.
3. **Regression** (GPU, `Aver.Render.Neural`; Tools > Train Neural Denoiser or `--nrd2-train`): the network learns to predict the oracle parameters from
   the inputs. **Parameter-space loss only**: the network's own output is never run through the filter
   and scored, in training, validation, checkpointing or the live gate (rule 4, UC US 10,192,146).
4. **Weights** save as `nrd2_v1.avnn` in the user data folder (the user's copy wins over the shipped one
   in `bin/data`, as NeuraFI's). A held-out set of poses judges parameter error before weights ship.

## Engine work

| Part | Where | New |
|---|---|---|
| Demodulated D/S + guides out of Stage B; clean terms split off | `voxi.hlsl` Stage B, `VoxiRenderer` | targets, Stage B outputs |
| Pyramid + resolve (fixed maths), compose back | `modules/render.denoise` (NRD2 passes) | yes |
| Conv2D forward, backward, Adam; CPU reference; tests | `modules/render.neural` | Conv layers (today: MLP only) |
| Still-hold mean, oracle fit | `modules/render.denoise` | capture + fit passes |
| Training session, progress UI, weights IO | sandbox Tools menu, `Aver.Render.Neural` | yes |

## Phases

1. **Composed-lighting split + NRD2 resolve with fixed parameters**, behind the Denoiser setting as a
   third option (Off / FidelityFX / NRD2). Measured against FidelityFX on NeonDistrict Night and Sponza:
   speckle and energy moving and still, cost.
2. **Conv layers in `Aver.Render.Neural`**: forward + backward + Adam on the GPU, CPU reference, unit
   tests matching the two to tolerance.
3. **In-engine capture and oracle fit.**
4. **Training session, network inference in the resolve, shipped weights.**
5. **Patent sweep** for the convolutional parts (encoder over guides, per-tile outputs) before shipping.

## How it is judged

On NeonDistrict Night and Sponza (day and night): speckle (per-mille isolated pixels and band-pass
blotch energy) moving and still, energy against the reference path tracer (no darkening), smear (none
by construction; checked on a fast pan), and GPU cost against FidelityFX + the reflection path it
replaces.
