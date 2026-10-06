# NRD v2: single-frame neural denoiser over the composed lighting

**Status:** phases 1, 3 and 4 built (2026-10-06); weights not yet trained. Owner decisions: replace the whole denoiser stack (FidelityFX and the
per-signal plumbing); **single-frame** (the spatial filter and the network never see history, so no
smear or ghosting from them; an optional temporal stabiliser on jitter-free frames was added later, see
"Temporal stabiliser"); denoise the **composed lighting**; a **convolutional** network; **trained in-engine** like
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
| lamp visibility | CSRdLocalLights, its usual `(x+y+frame)&1`, stores -1 | Stage B's existing 5x5 `rdLocalVisFiltered`, which skips negative taps | `voxi.nrd2HalfRateLamps` | -- |

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
All compute is fp32, 8x8 (params 64x1), no wave intrinsics or atomics, groupshared 3 KB, constants at b3.

1. `CSNrd2Pyramid`: D and S from this frame only, 2x2 reductions to 1/2, 1/4, 1/8, each keeping its
   nearest surface (others weighted exp2(-23 dz/z)); guide levels hold the averaged normal and view Z
   (m), value levels rgb and a validity fraction.
2. `CSNrd2Params`: the per-8x8-tile buffer (`StructuredBuffer<float>`, 12 planes, plane-major) filled
   with the defaults. Phase 4's network writes the same buffer.
3. `CSNrd2Resolve`: per pixel and signal, candidates = own pixel and the three levels upsampled by
   depth- and normal-weighted fixed bilinear taps. Weight = exp(logit) x surviving tap share x
   luminance term (log-ratio to the coarsest usable level); own logit 0. Specular adds fixed terms: a
   smooth lobe (roughness below 0.35) and a short hit distance keep the own pixel. Writes D' Rd + S' Rs
   (with the stabiliser on, D' and S' go to two more targets and pass 5 writes the lit target).
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

## Temporal stabiliser (2026-10-06)

The owner asked for smoothing of the speckle that remains in motion, where there is no temporal stage at
all: the camera moving zeroes the TAA jitter and TAAU falls back to its spatial path with no history, and
FidelityFX is off. The stabiliser (`CSNrd2Stabilise`, `AVER_NRD2_PASS 5`, GPU span `NRD2.Stabilise`) fills
that gap and nothing else. Default on; `voxi.nrd2Stab 0` is the A/B.

**Gate.** It runs when `Settings::nrd2Stab`, the frame's TAA jitter is exactly (0, 0) (`IDevice::taaJitter`),
the G-buffer history is valid, the velocity target and previous matrix are supplied, and no capture is
active. Otherwise the resolve writes the lit target itself, as before, and the history is dropped, so the
first stabilised frame after a rest or a toggle has none. Jitter-free only means history never mixes
jittered samples, and TAAU keeps the camera-at-rest case (TAA on and still: jitter non-zero, no stabiliser).

**What it does.** Per pixel, on the resolve's D' and S' (irradiance, before remodulation, so albedo detail
never blurs; D and S separately, so the clamp box is in lighting units):

1. History position from the G-buffer velocity (rigid motion, exact on jitter-free frames). Four bilinear
   taps by `Load`; each is kept or dropped by its reprojected view depth: the stored depth must match the
   depth this surface point should have in last frame's camera within 2% + 1 cm + the local depth slope
   (per axis the smaller one-sided difference to the neighbours, so a silhouette does not widen it). Less
   than half the bilinear weight left, a position outside the viewport, or no valid previous frame: no
   history for this pixel (age 0), which is plain single-frame NRD2. A rigid mover fails the camera-only
   depth test and so gets none either.
2. The clamp box is the per-channel min and max of this frame's valid 7x7 of D' (S'), a sliding window,
   united with the 1/8 level bilinearly interpolated at the pixel: inside the spatial estimate's footprint,
   never larger. (The first version used the four raw 1/8 texels; the box, and so the clamped history, then
   stepped at every 8x8 block and showed as squares on vaults and flat walls. A 3x3 box without them was
   smooth but too tight to remove blotches.) Before the box, a firefly clamp scales a pixel down to 2x its
   brightest 8-neighbour. Costs 0.44 ms at scale 0.5 on NewSponza (was 0.17 ms with the 3x3). History outside the box is clamped to its edge. Never a mean or variance.
3. Blend weight `a = 1 - 1/min(age + 1, N)`, so 0, 1/2, 2/3, 3/4 ... The age is the youngest of the taps that
   carry weight (> 0.1), and the stored age counts frames up to 255. It feeds this weight only. `N` comes from
   motion alone: `voxi.nrd2StabFrames` (12) up to 0.25 px per frame, a log-space ramp to min(8, that) at
   8 px, flat to 128 px, none beyond (only a whip takes no history; a normal camera swing keeps it, as
   FidelityFX does, with the clamp box and depth test guarding against smear); 2 for two frames after the sun changes
   (the hold FidelityFX gets). Specular takes `a` times saturate((roughness - 0.35) / 0.3): a glossy
   reflection does not move with its surface, so smooth lobes stay single-frame.
4. out = lerp(current, clamp(history, box), a), stored as the next history (D'' with the age, S'' with the
   view Z in metres), and lit = D'' Rd + S'' Rs, clamped as the resolve does.

Why it should not smear: the velocity is exact for a camera pan, so history is aligned and only bilinear
blur of already smooth irradiance remains; strips entering the screen and disocclusions get age 0; `N` falls
with speed; the box bounds any misaligned history to this frame's neighbourhood; reflections that swim with
parallax are excluded. Remaining risks: a moving object's shadow on static ground (velocity 0, the edge moves:
lag up to `N` frames inside the box), skinned or soft-body motion (not in the velocity; the depth test and the
box bound it), lamp flicker (the clamp snaps past the box, so no lag for global changes).

**Resources.** D' and S' (RGBA16F) and two ping-pong pairs of history (D'' + age, S'' + Z), all render size,
allocated on the first stabilised frame and freed when the dial is off: about 84 MiB at 1766x994 (estimate).
Cost, also an estimate and not measured: about +0.3 ms (0.2-0.4) on jitter-free frames, none otherwise.
Constants grow from 128 to 320 bytes (stabiliser rows and the features' input affine).

**Dials.** `voxi.nrd2Stab` (bool), `voxi.nrd2StabFrames` (1-64). The speed cut-offs, the depth tolerance and the
roughness ramp are constants at the top of `nrd2_resolve.hlsli`, starting values to tune on the rig.

**Not done.** Glossy road reflections stay single-frame (no reflected-point reprojection; a later option, and
the parallax rule needs counsel first); no jitter-aware variant to run under TAAU at rest; perspective
cameras only (the view-depth reprojection assumes a symmetric frustum); the speckle that is specular aliasing
or a heavy-tailed sample is only averaged down, not removed. An estimate from the rig numbers above: at most
about a third of the 0.21 moving speckle (toward the 0.14 still level) is lack of accumulation.

**Check (owner-run, headed).** TAA off so jitter is 0, a moving camera, `voxi.nrd2Stab` 0 against 1:
speckles per mille and the 9 px blob error; energy against `voxi.ptMode 1` (target no worse than about 0.5%
lost on Night); a fast pan at 8, 32 and 64 px per frame against stab-off frames for lag; one lamp toggle and
one sun-time step; the `NRD2.Stabilise` span (budget 0.3 ms). CPU twin: `Nrd2ResolveReference`
(`nrd2StabilisePixel`), checked by `Nrd2ResolveTest`. Patent mapping: NEURAA_NRD.md section 7, rule 13.

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
