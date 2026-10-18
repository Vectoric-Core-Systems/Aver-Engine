# Many lights: stochastic direct lighting from a light tree (Voxi)

Status: implemented 2026-10-17 from `docs/plans/wave4/MANY_LIGHTS.md` (the frozen design: read it for the reasoning and
the patent table). Steps S0, P0 and 1 to 4 are in; the optional steps 6 to 8 (single-frame fallback filter, tree refit
and a 16k cap, raster deferred pass) are not. **Nothing here has been run on a GPU yet.** The CPU half is tested
(`tests/render.voxi/src/LightTreeTest.cpp`), `CSRdManyLights` compiles to SPIR-V and to DXIL at SM 6.5 and every entry that reaches `averDirectLights` still compiles; every
number in the budget table below is a target, not a measurement.

## What it does

A scene with thousands of shadowed lights used to be lit through the light grid (`UNIFIED_LIGHTS.md`): each point sees
only the 24 most important lights of its 50 cm cell, so every light past 24 was silently dropped, and the more lights
there were the worse that got. With many lights on, the **tail** of a point's lights (everything but its exact light)
is drawn from a **light tree** (a BVH over every light) by importance, resampled with a BRDF-aware target, and traced
with a bounded number of shadow rays:

- rays per pixel are `1 + S` (the exact light, plus S samples; S is 1 at Medium), whatever the light count;
- the tree is rebuilt from the current light set each frame it changes; **no light, reservoir or visibility is carried
  from another pixel or another frame** (the patent rule below), so all noise reduction is NRD2's and TAA's;
- the exact light (the sun by day, the dominant lamp at night) keeps the sun's full shadow kernel, exactly as before;
- a scene with `voxi.manyLightsMin` (48) lights or fewer takes the old path and renders as before.

It needs **NRD2** (`--denoiser 2`), ray tracing and D3D12's staged ray-driven passes. FidelityFX and None frames keep
the grid tail. Vulkan has no staged passes or light list yet, so the setting does nothing there and says so once in the
log; the pass compiles for SPIR-V at 6.5 so it is ready when they land.

## Using it

| Where | What |
|---|---|
| Rendering settings, Ray Tracing page, "Lights" | "Many lights": Auto (default), Off, On. Round-trips as `RENDER.MANYLIGHTS` 1, 0, 2 |
| `voxi.manyLights` | 0 legacy, 1 auto, 2 force at any count, 3 exhaustive (every light, one ray each, at most 512 lights, hits only: the ground truth) |
| `voxi.manyLightsMin` | auto engages above this many lamps and scene lights (default 48) |
| `voxi.lightSamples` | S, shadow rays per point for the tail, 1 to 4 |
| `voxi.lightCandidates` | M, candidates drawn per point, 2 to 16 |
| `voxi.lightHitCandidates` | M at GI, reflection and path hits, 1 to 8 (rays there: `voxi.lightRaysPerHit`, at most 4) |
| `voxi.lightWeightClamp` | 0 (off). Caps a candidate's weight at this many times the running mean: a bias knob for scenes that still sparkle |
| `voxi.debugStressLights`, `voxi.debugStressSeed` | N synthetic point lights around the camera, for scale tests (below) |

S, M and hit M follow the ray-tracing tier (`QualityLadder.hpp`):

| Rung | S | M | hit M |
|---|---|---|---|
| Off, Low | 1 | 4 | 2 |
| Medium (default) | 1 | 8 | 4 |
| High | 2 | 8 | 4 |
| Epic | 4 | 16 | 8 |

Debug views (the viewport's View Mode, or a `ViewDebug` value): `LightCount` (7) is a heat map of the lights reaching the
pixel (tree leaves with importance, capped at 255; computed by `CSRdManyLights`, so it reads black unless the pass ran);
`ManyLightsRaw` (8) is the pass's diffuse + specular before NRD2.

**Stress lights.** `--set voxi.debugStressLights 2000` adds 2000 shadowed point lights (5 cm radius, 20 to 400 cd, a
seeded hue) on a seeded jittered 3-D grid, +-30 m horizontally and -1 to +5 m vertically around the camera (+Z up). The
layout is a pure function of `(seed, count, camera snapped to 1 m)`, so captures repeat; at 0 it costs nothing. They
join the list like any scene light, so the existing cut to `kMaxListLights` (4000) applies.

## How it is built

### The tree (CPU, `LightTree.hpp/.cpp`)

Built over the list's non-directional lights (the first `rdLocalLampCount_` entries; the sun follows them and is not in
the tree). Leaf quantities: centre, power `Phi` (luminance of the radiance-at-1-m colour, times the area in m^2 for a
rectangle but never below 1 m^2, which is what `rdLightIrradianceCheap` assumes), range, emitter radius, orientation
cone and emission spread (point: omni; spot: axis, spread = its outer cone; rect: axis, a front hemisphere). Morton
sort of the positions (10 bits an axis), split at the highest differing bit, median when the codes are equal or the
depth cap (40) would otherwise be passed. Inner nodes merge the AABB of the centres, the power, the largest range, the
largest emitter radius, and the cone by the standard cone union. A NaN, infinite or negative power or range refuses the
tree for the frame (logged once) and the frame keeps the grid.

Order. The list reorders every frame the camera moves, so the tree is built in canonical (byte) light order and
`fill()` rewrites only the leaves' list indices when the same set comes back in another order; a changed set rebuilds.
The set key is order-independent.

**Packing.** One 80-byte record per node, appended after the grid pool in the light buffer (t18) and cached with the
grid tail by the grid's key (which also covers the tree switch and the sampling settings):

| floats | field |
|---|---|
| 0-2, 3 | AABB min of the centres (cm), `Phi` |
| 4-6, 7 | AABB max, largest range (cm) |
| 8-10, 11 | cone axis (unit; zero = omni), `cosThetaO` |
| 12 | `cosThetaE` |
| 13, 14 | left, right child **node index** (leaf: left = -1, right = the light's list index) |
| 15 | parent node index (root: -1) |
| 16 | largest emitter radius |
| 17 | flags: 1 leaf, 2 every light is no-shadow, 4 contains a rectangle |

The **tree header** record comes first in the tail: `posRadius` = (root node 0, node count, buffer record of node 0,
buffer record of the leaf map), `radianceRange` = (S, M, hit M, weight clamp), `axisKind.x` = 1. Its record index is in
the grid header's `right.x` (0 = no tree), so no constant-buffer field changed. Then the nodes, then the **leaf map**
(a float pool of 20 per record: light-list index to leaf node), which is how the estimator finds the exact light's tree
probability. Size: `2N + ceil((N + 1) / 20)` records of 80 bytes, 640 KiB at 4000 lights, rewritten only when the tail key
changes for the upload ring slot.

### Importance and descent (HLSL, `voxi_lighttree.hlsli`)

Included from `voxi_rt.hlsli` straight after `rdLightIndex`; no resource, loads from t18 only, SM 6.0 arithmetic (no
derivatives, wave ops or groupshared), no recursion or stack array. `rdTreeImportance` is the bound of Conty Estevez and
Kulla (2018) in this record's units. Written from the paper's formulas.

- **Zeros and gates are exact.** Zero if the nearest the box of centres comes to the point is outside the largest
  range, or if the emission cone (`thetaW - thetaO - thetaU < thetaE`) misses the point: a zero never hides a lit
  light. The receiver cosine is `cos(max(0, thetaI - thetaU))` without trig for the sphere term; a node that may hold a
  rectangle keeps the 0.1 floor.
- **Magnitude.** `Phi * 1e4 * cosI * win / max(dmin^2, floor, 1)`, `win` the same `(1 - x^4)^2` window as the shading.
  A leaf's `floor` is its emitter radius squared and its value is **exactly** its light's `rdLightWeight`. An inner
  node's is its box's half-diagonal squared: strict `1 / dmin^2` piled the whole probability on the node under a camera
  that sits inside the cloud of lights (the common case), so the distance term is regularised there as the paper does.
  Where the point is at least the half-diagonal outside the box the bound is strict and tested to hold.
- **Emitter factor is a 0/1 gate**, not the paper's `cos(thetaP)`: `rdLightIrradianceCheap` has no emission cosine, and
  the tree's selection density should follow the target it feeds.

`rdTreeSample` descends with probability `IL / (IL + IR)` per level and rescales `u`; it returns false when a node's
children are both zero (the AABB of two lights is not their union, so near a range edge a lit parent can have two dark
children; that mass is a candidate of weight 0, never an early exit, because the estimator's divisor is the draw
count). `rdTreePdfOf` walks up from a leaf through the parent pointers.

### The estimator (`averManyLights`, `voxi_pt.hlsli`)

Per shading point, normal N, exact light `e0` (or none):

1. Directional lights other than `e0` (they are not in the tree) are shaded exactly with one ray each.
2. **M candidates**: `rdTreeSample`; a draw that lands on `e0` is redrawn (three tries). With `q0` the tree probability
   of `e0`, the candidate density of any other light is `q (1 + q0 + q0^2)` and the failed mass `q0^3` is a candidate of
   weight 0, so the estimate is unbiased without the spike a dominant `e0` would cause. The first dimension of each draw
   is rotated per 4x4 tile by interleaved gradient noise.
3. **Target** `phat(y) = lum(cheap irradiance) * (kd/pi + F0 D_GGX(NoH, a) / (4 NoV NoL))`, `a` widened by the emitter
   size as `ptLampLight` does. Only a density: any positive function is unbiased, so it needs no texture fetch.
4. **RIS into S slots**, streaming, `S <= 4` statically unrolled (no runtime-indexed array, which sent a shader to
   scratch memory and hung the RX 7800 XT): slot s accepts candidate j with probability `w_j / wsum_j`. Slot weight
   `W = wsum / (M phat_s)`.
5. **Roulette**: a slot whose unshadowed estimate `phat * W = wsum / M` is under `tau` survives with probability
   `that / tau` and its weight is divided by it. `tau` is a 256th of the exact light's `phat` (1e-4 with none). This
   replaces the biased 1/128 drop of `rdTailFloor`.
6. **One shading call site in a loop** over the slots and the directional extras (`ptLampLight`, `averLightVisibility`
   which is `rdLocalShadow`: opaque, accept-first-hit, disc or rect sample, `averDirectTerms`), diffuse + subsurface
   and specular kept apart. `contrib = BRDF * radiance * cos * V * W / S`.
7. `voxi.lightWeightClamp` caps a candidate at the clamp times the running mean (default 0, off).
8. The PCG stream of `ptSeed` (per pixel, per frame, per stream); the frame index is mixed in.

**Unbiasedness hole**, as the shipped ranking has: where `phat = 0` but the exact contribution is not (a rectangle's
back lobe, a cookie's edge, subsurface light through a back face) the estimate is biased low.

### `CSRdManyLights` (the visible surface) and Stage B

`voxi.hlsl`, `[numthreads(8,8,1)]`, compiled `cs_6_5` on D3D12 and 6.5 for SPIR-V. It replaces `CSRdTailVis` on a frame
that runs it: per pixel it reads the visibility record, rebuilds the textured surface Stage B shades
(`rdHitSurface(AVER_RT_HIT_FULL)`, footprint from the neighbouring primary rays, as `CSRdRefl` does, no `ddx`),
skips pixels with no exact light, sky and unlit, skips the descent when the root's importance is under 1e-6, and runs
the estimator with `e0 = gRdLocalOut.x`. Outputs, written for every pixel: `gRdManyDiff` (u25) and `gRdManySpec` (u26),
RGBA16F radiance with the albedo in it (Stage B demodulates it as it does the old tail), and the tail's shadowed
fraction into `gRdLocalOut.a`, so `CSRdTailFilter` (kept) and the glass replay's `rdLocalVisFiltered` work unchanged.
Stage B (NRD2 variant): `if (rtManyLights()) { nrdD += gRdManyDiff; nrdS += gRdManySpec; } else rdTailLightsSplit(...)`.

Order and barriers are `CSRdTailVis`'s: UAV barriers on `rdLocalOut` and `rdSunVisTex` before it, it joins the
barrier-free group with GI, sky occlusion and reflections, a barrier on `rdLocalOut` then `CSRdTailFilter`, and a
barrier on the two textures before the Stage B draw. GPU span `"Voxi RD many lights"`; `voxi.rayDrivenStageTiming`
keeps its extra barriers.

**Hits** (GI candidates, reflection hits, path vertices): `averDirectLights` takes the tree branch
(`if (rtManyLights() || rtManyLightsExhaustive())`) with `M = lightHitCandidates`, `S = min(lightRaysPerHit, 4)`, no
exact light, the same estimator, still one shading call in a loop. The lite arm (ray-traced raster, glass, the single
pass) is untouched. `voxi.manyLights 3` walks every list light with one ray each, at hits only.

### Engage rules

`manyLightsLive` this frame requires: a tree published (`manyLights != 0`, above the threshold or forced, every
power finite), localLights, an NRD2 frame, not Path Tracing Reference mode, `CSRdManyLights` compiled, both textures
allocated, the tail filter and the light slots present. Any failure logs once (INFO for an expected skip, WARN for a
missing pipeline or texture) and the frame runs the grid tail; nothing is half applied. Two bits of the shader-visible
word `gGiShadowParams.w` carry it: 2048 (the stochastic estimator runs: pass, Stage B, hits) and 4096 (exhaustive).
The textures exist only while wanted (a hysteresis at half the threshold keeps a count hovering at it from creating and
freeing 15 MB); 14.7 MB at 720p internal, 59 MB at 1440p.

### Binding slots (P0)

Table 0 had 25 SRV and 25 UAV slots, full. `kMaxBindingSlots` is now 28 and `kVoxiUavCount` 27: u25 and u26 are this
feature's (`kManyLightDiffUavSlot`, `kManyLightSpecUavSlot`, `static_assert`ed against the shader's `register(uN)` in
`giTableKinds`). Both backends size their per-set arrays from the constant; the Vulkan UAV binding base follows it, and
the variant key packs counts in 5 bits (31 is the ceiling).

**Table 1 moves with table 0.** A second table's registers start where table 0's end, so the volumetric fog's scratch
volume (`gFxCur` in `voxi_froxel.hlsl`) moved from u25 to u27. Left at u25 it read the many-light slot (a null 2D
descriptor while the feature is off) as a 3D volume, and the GPU hung in the first frames whenever fog ran, many lights
on or off. `VoxiFroxelFog::kHistSrvRegister`/`kCurUavRegister` are now `static_assert`ed against the table sizes.

## Patents (a design constraint, not a taste)

NVIDIA US 11,948,246 claims, in effect, ReSTIR DI's spatiotemporal reuse (lights from other pixels or a prior frame),
to about 2042; US 12,361,635 adds prior-image lights as candidates; US 12,299,801 is ReGIR (lights sampled per spatial
cell). So: candidates come only from the pixel's own tree descent over the current light table; nothing selected for
any pixel, this frame or last, is read by another pixel or frame; **no stochastic per-cell or per-tile light selection is
stored**; the deterministic grid is a cull and a cut, not a sample. The exact-light hysteresis that shipped (the last
frame's pick kept while within 80% of the strongest) read on claim 1, so **S0** replaced it: among the lights delivering
at least 80% of the strongest at this frame, the one with the smallest stable key is exact, a pure function of the
current frame (`CSRdShadow`). `NEURAA_NRD.md` rule 18 records the rule. If counsel or a licence clears 11,948,246,
reservoir reuse is the first thing to add on top of the estimator. The light BVH, importance bound and descent are from
published papers (Lightcuts, Conty Estevez and Kulla 2018, Moreau et al. 2019); no code was copied.

## Budget (targets, none measured)

Hardware of record: RX 7800 XT, NewSponza_Night default view, render scale 0.5, NRD2.

| Item | Target |
|---|---|
| Scenes with <= 48 lights | +0.00 ms, identical image |
| Tree CPU build, 4000 lights | <= 0.3 ms, cached on a still set |
| `CSRdManyLights` Medium (S=1, M=8), 2000 lights, 720p | <= 1.5 ms |
| Whole-frame delta at 2000 lights vs the 48-light baseline | <= +2.0 ms |
| Hit sites | <= +0.5 ms total at M = 4 |
| Texture VRAM | 14.7 MB at 720p, 59 MB at 1440p |
| Tree upload | 640 KiB per set change |

If Medium misses 1.5 ms the levers are, in order: M 8 to 4, a higher roulette threshold, then a quarter-resolution
trace with the pass's own bilateral fill (optional step 7), before touching S. Measure with `--gpu-timing` and
`voxi.rayDrivenStageTiming 1` (the barriers it adds change the total; confirm on the total without it).

## Verification

CPU, `tests/render.voxi/src/LightTreeTest.cpp` (compiles `LightTree.cpp` directly): structure (2N-1 nodes, AABB, power,
cone, parent and depth invariants, coincident lights, a geometric chain), canonical order and `fill()` (byte for byte),
packing against `voxi_lighttree.hlsli`'s record offsets and flag bits, the importance bound against the cheap
irradiance over 6e4 random (point, normal, node), `pdfOf` against the descent and a histogram, a CPU twin of the RIS
estimator against the brute-force sum (M = 1, 4, 16, S = 1, 4, with and without the exact light, with roulette;
tolerance the larger of 0.5% and 4.5 standard errors, and a mutation that drops the redraw multiplier fails it by 19%),
and degenerate inputs. Shader compile: `VoxiSpirvCompileTest` (`CSRdManyLights` at 6.5; the UAV shift is now 28) and
`VoxiShaderCompileTest` (`manyLightsCompiled()`). `FormatTest` round-trips `RENDER.MANYLIGHTS`.

Headed GPU verification (the owner's rules: visible window, 1440p screenshots when remote, GPU idle first, restore the
owner's `editor.ini` after a crash). In this order, since a hang resets the driver:
1. **TDR safety:** NewSponza_Night default view, `--denoiser 0`, `1`, `2` and `--pt 3`, with `voxi.manyLights 2`, on a
   60-light scene before any stress.
2. **Unbiasedness on the GPU:** `--set voxi.debugStressLights 128`; reference `voxi.manyLights 3` at `--denoiser 1 --set
   voxi.ptMode 1`, 1500 frames; compare `voxi.manyLights 2` (`--denoiser 1`, raw D + S recomposed with
   `voxi.nrd2Bypass 1`): mean luminance within 1% of 1.0.
3. **Image quality with NRD2** against the same reference, still and `--cam-wobble 4 60`, at 48 (must equal legacy bit
   for bit), 256, 1000, 2000, 4000 lights: no worse than legacy +0.3% absolute at 256; at 2000 blob error under 1.5%
   still and 2.5% moving, spot area under 0.5%.
4. **Flicker:** frame-to-frame MAD at rest with TAA on and off, and the exact-light swap rate for S0 (within 2x of the
   shipped value; the fallback is a coarser band, 0.6, on the same rule, never the prior pick).
5. Cross-API, when Vulkan's staged passes exist.

The rig is `scripts/many-lights-ab.ps1` (refuses a capture the tree did not light, an unconverged reference and an empty
scene) with `scripts/many_lights_metrics.py` (mean luminance ratio, per-block error, blob error, bright-spot area,
flicker, byte identity).

## Limits

- NRD2 frames on D3D12 only. FidelityFX and None keep the grid tail, capped at 24 lights per cell.
- Raster keeps its 32-lamp working set (`PSMainVoxi` is at its register limit and the G-buffer has no albedo).
- The list cut `kMaxListLights` (4000) is unchanged: far, dim lights past it are dropped as before.
- NRD2's network saw the old lamp noise model; default parameters apply until it is retrained on stochastic tails.
- Without temporal reuse the tail is noisier than ReSTIR DI at equal rays. Mitigations in order: M, S, the clamp.
- Emissive triangles are not lights here (US 2025/0182387 claims sampling lights and geometry in one step).
- The register-limit TDR history applies to the hit branch: it adds code to the compute kernels that call
  `averDirectLights`. If the first headed run shows occupancy or watchdog trouble, split the estimator into a select
  pass and a shade-and-trace pass (nothing in `PSMainVoxi` changes either way).
