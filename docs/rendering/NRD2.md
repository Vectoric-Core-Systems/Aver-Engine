# NRD v2: single-frame neural denoiser over the composed lighting

**Status:** design (2026-10-05). Owner decisions: replace the whole denoiser stack (FidelityFX and the
per-signal plumbing); **strictly single-frame** (no denoiser history, so no smear or ghosting in
motion); denoise the **composed lighting**; a **convolutional** network; **trained in-engine** like
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

**Strictly single-frame.** An NRD2 frame runs no FidelityFX pass and no Voxi history:
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
   smooth lobe (roughness below 0.35) and a short hit distance keep the own pixel. Writes D' Rd + S' Rs.
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

## The network

- **Shape.** A small encoder over 1/2-resolution inputs (3x3 convolutions, ReLU, stride-2 down to 1/8),
  then 1x1 heads to the per-tile outputs. Roughly 4 layers of 16-32 channels; target inference
  0.3-0.5 ms at 1766x994 on the RX 7800 XT.
- **Inputs.** Per pixel: log-luminance of D and S at this frame, guides (view Z relative to the tile,
  normal, roughness, albedo luminance, hit distance / view Z). No history of any kind, no frame time.
- **Outputs.** Per 8x8 tile only: level logits and edge sensitivities for D and for S. Never a per-pixel
  weight map, never an image (rule 6 of NEURAA_NRD.md section 7, Pixar/Disney US 10,672,109).

## Training, in-engine

1. **Capture session** (editor: Tools > Train Neural Denoiser; or `--nrd2-train`): the camera travels a
   path and holds at poses. At each hold the renderer keeps rendering the same estimator and averages
   it on the GPU (256+ frames): that mean is the target, the single frames are the inputs. Same
   estimator, so the target is exactly what a perfect denoiser of that estimator would return.
2. **Oracle fit** (GPU): per tile, gradient descent on the resolve's parameters against the mean, over
   several noisy frames of the pose. This fits parameters, not a model.
3. **Regression** (GPU, `Aver.Render.Neural`): the network learns to predict the oracle parameters from
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
