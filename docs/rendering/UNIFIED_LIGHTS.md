# Unified lights (2026-10-07)

Status: **built (phases 1-3, phase 5 in part), not yet run by the owner.** See "As built" at the end. Owner's direction: one system for every emitter. The sun is an emitter with a
direction and an intensity, and nothing else about it is special. Every emitter gets the treatment that made the
sun clean. This is not a second, special path beside the old ones.

## Why

Lamp-lit areas showed spots that sunlit ones did not. The sun and lamps went through different systems:

| | Sun | Lamps |
|---|---|---|
| Visible surface | Evaluated exactly at every pixel, its own shadow ray, temporal history (FidelityFX mode), tiled shadow passes | One lamp of up to 32 picked at random per pixel, one shadow ray, that answer applied to all lamps |
| GI, reflection and path hits | Exact, one shadow ray | One lamp picked by irradiance, divided by its pick probability (`ptLamp`) |
| Falloff | None (distant) | 1/d^2, capped at the flame's surface (pi L, physically right): bright and rare near a flame, so noisy when sampled |
| Coverage | Always | 32 of about 1000 candles; the rest only when a ray hits the flame |

Code map: the 2026-10-07 survey (sun sites in `voxi.hlsl`, `voxi_rt.hlsli`, `voxi_restir.hlsli` and `voxi_pt.hlsli`;
lamps in `rdLocalLightsVisibility`, `rdLocalLightsShade`, `ptLamp` and `ptLampsAll`; host in `buildLocalLights`).

## Design

**One light list.** One structured buffer of `AverLightRec` (`aver_lights.hlsli`), built each frame:
- The **sun** is kind `AVER_LIGHT_DIRECTIONAL`. Its direction and radiance come from the sky (`gLightDir`,
  `averSunRadiance`), its disc angle from `rtParams.x`, and its range is infinite.
- **Lamp-flagged emissive draws** become sphere lights at their bounds.
- **Scene CLights** are added as point, spot or rect lights (`packSceneLight`).
- There is **no 32 cap**. The list holds every emitter, up to a large bound, and a light grid (below) keeps the
  per-pixel cost bounded.

`aversLightEval` gains the directional kind. The sphere falloff stays as it is: 1/d^2 capped at pi L on the
sphere's surface is already the exact bound.

**One rule for every pixel and every light.** Wherever direct light is needed (the visible surface, GI hits,
reflection hits, path vertices), the same evaluator runs:
1. **Candidates.** The lights that can reach this point come from the light grid: world cells, each listing the
   lights whose range touches it. Directional lights are in every cell.
2. **Exact set.** The K lights with the largest unshadowed contribution here (light x BRDF x cosine) are
   evaluated exactly, each with its own shadow ray. This is what the sun always had. By day the sun is almost
   always in this set because it delivers the most, not because it is the sun. K is 2 to 4, a measured trade
   against frame time.
3. **Tail.** The remaining candidates are estimated by resampled importance sampling: several candidates
   weighted by their contribution, one shadow ray to the winner, unbiased weight. They are dim, so their noise
   is small.

On the visible surface, the shadow results of the exact set keep the sun's quality tools for every light: the
penumbra disc from the light's size, the per-pixel temporal accumulation in FidelityFX mode (NRD2 frames take
the raw path, as today), and the tiling. Each pixel stores up to K (light id, visibility) slots. Stage B shades
the exact set from those slots and adds the tail.

**Sun uses that stay.** These read the sun's list entry, never a separate sun variable:
- the sky and atmosphere, fog and aerial perspective;
- caustic focus, applied to whichever entry is directional;
- voxel GI injection;
- the raster shadow-map cascades, the non-RT fallback.

## Phases

1. **List and evaluator:**
   - the unified list on the host, with the directional kind and the sphere solid angle;
   - one GPU light-eval library function that every secondary-hit site calls (GI candidates, reflections,
     path vertices), replacing the five hand-built `AverLight sun` sites, `ptLamp` and `ptLampsAll`.
2. **Visible surface:** one compute pass replaces `CSRdShadow` + `CSRdLocalLights`, producing K visibility slots
   and the tail. Stage B shades from the slots (plain, G-buffer and NRD2 variants).
3. **Light grid and no cap:** every emissive draw and every scene light becomes a light.
4. **Raster and glass:** `PSMainVoxi` reads the staged slots (the existing `rdReuse` route) instead of evaluating
   lights inline, so no new code goes into the fragile ray-traced raster variants. The single-pass kernel keeps
   its simple path until it is retired.
5. **Cleanup:** remove `rtShadowTemporal*` sun-only kernels, `rdLocalLightsVisibility`, the 32-lamp stratified
   picks and the double-count guards (an emitter is never counted at a hit once it is in the list). Update
   LIGHTS.md and NRD2.md.

## Constraints

- **GPU hangs.** The ray-traced `PSMainVoxi` variants and the single-pass kernel are at their register limit
  (three hangs on the RX 7800 XT this week: rect lights, decals, stratified picks). New code goes into compute
  passes and Stage B only. After every step, test NewSponza Night's default view with `--denoiser 0`, `1` and `2`
  and `--pt 3`, with the owner's OK, since a hang resets the driver.
- **FidelityFX stays stock** (the denoiser itself; its inputs change because lighting changes).
- **NRD2 patent rules:** this is renderer sampling, not denoising. History in the exact-set visibility follows
  today's rule: FidelityFX-mode only, raw on NRD2 frames.
- **Measure** against the Path Tracing reference (`--denoiser 1 --set voxi.ptMode 1`, 1500 frames): spots, bias,
  blob error and GPU time, at the gallery (still and moving), the default view, Path Tracing, and a daylight
  scene so the sun does not regress.

## As built (2026-10-07)

- **List (t18):** lamps and scene lights in importance order (no 32 cap, up to `kMaxListLights` = 2047, so every index fits `gRdLocalOut`'s half precision), then the
  sun as a directional entry whenever ray tracing runs and it has radiance, then the **light grid**: a header record
  (`gDecalParams.y` names it; 0 = no grid), a cell table and an index pool, all packed as floats in the same buffer.
  Up to 32 cells per axis, at least 50 cm, bounded to 200 m around the camera; each cell keeps its 24 most important
  lights. Directional lights are global, not in cells. `rdLightsAt` / `rdLightIndex` (`voxi_rt.hlsli`) walk it.
- **Visible surface (`CSRdShadow`):** one exact light per pixel (the largest unshadowed contribution, the sun's
  full kernel: disc, history reused only under the same light: `gRdLocalHist.y` key, see DENOISING.md 3b; tiles). Last frame's exact light keeps the slot while it delivers
  at least 80% of the strongest, so near-equal candles do not trade places. **Every other light (the tail) is
  treated the way the sun is:** shaded exactly and unshadowed in Stage B (`rdTailLights`), times ONE shadow
  fraction in [0, 1] (`rdTailVisibility`): one light picked by irradiance, one ray, its 0/1 answer accumulated
  with the lamp history pair (FidelityFX mode; raw and stratified on NRD2 frames) and filtered 5x5 in Stage B.
  This is the ratio estimator of Heitz, Hill and McGuire (I3D 2018). It replaced a first version whose tail was
  one light divided by its pick probability, unfiltered: an unbounded per-pixel spike that the FidelityFX mode
  never denoises (FidelityFX gets AO, GI and reflections only), and the spots that remained under lamps.
  `gRdLocalOut` = (exact id, unused, unused, tail fraction). Colour is approximate where tail lights are blocked
  differently. Patent check 2026-10-07: the paper is not patented as far as found; NVIDIA's shadow-denoising
  patents (US10740954, anisotropic kernels from light and occluder geometry; US11600036, self-guided
  spatiotemporal) are avoided: the filter is the fixed depth-weighted 5x5 and a fixed-rate history.
- **Hits (`averDirectLights`, `voxi_pt.hlsli`):** GI candidates, reflection hits and path vertices. Top K exact
  (K = 2 in the bindless staged passes, 1 elsewhere) plus a reservoir-sampled tail, one shading call in a loop
  (inlining it per light multiplied the compile time). `ptLamp`, `ptLampsAll` and the hand-built sun sites are gone.
- **Raster and glass:** unchanged code. They keep a 32-lamp working set (`gCameraMedium.z`), now the 32 most
  important lamps rather than a canonical cut, through `rdLocalLightsVisibility`, or read `gRdLocalOut` through
  the existing `rdReuse` route.
- **Double-count guard:** `rdLocalEmitterCarried` looks the hit up in the grid, so every listed emitter drops its
  own glow at hits; `rdLocalLightsCarryAll_` holds whenever every flagged lamp is listed.
- **Not done:** the `rtShadowTemporal*` sun-only kernels and `rdLocalLightsVisibility` stay (raster uses them);
  the stratified picks stay for the raster loops. Nothing here has been rendered yet.

## Ray budget (2026-10-07)

Every light traced every frame (`df91ecd7`) fixed the flicker but cost 85 ms a frame at NewSponza Night's default
view (v0.6.0: 14.8 ms). The budget keeps the rule that fixed it, **no light picked at random**:
- **Visible surface:** the exact light keeps the sun's kernel (CSRdShadow). `CSRdTailVis` runs at quarter
  resolution, one thread per 2x2 block, so a wave's rays go to the same lights in the same order: the block's
  strongest `voxi.lightRaysPerBlock` (default 6) tail lights get one ray each per frame, the rest share their
  irradiance-weighted visibility, and the result goes to the block's same-depth pixels (-1 across an edge, which
  Stage B's 5x5 fills). An earlier per-pixel interleave was 3 ms slower for fewer lights (incoherent rays).
  3 rays per block saves 1.8 ms but raises bright-spot area from 0.08% to 0.71% of the view.
  History follows the sun's rule (0.9 at rest, 0.5 by 32 px/frame, 0.35 where the shadow changed; bilinear read, no
  3x3 box), which removed the trailing of lamp light behind camera moves.
- **Hits (GI, reflection, path):** one cheap loop sums every plain sphere light's diffuse irradiance and keeps the
  top 4 with indices; the strongest `voxi.lightRaysPerHit` (default 1) are shaded exactly with their own ray, the
  rest take their visibility as diffuse light. Rect, spot, IES and cookie lights outside the traced set are shaded
  exactly (that walk is skipped where none reach).
- **Ranking** uses the cheap sphere estimate (`rdLightWeight`); shading stays exact.

Measured (RX 7800 XT, NewSponza Night default view, scale 0.5, still, GPU ms):

| Build | Shadow stage | GI trace | Reflection | Total |
|---|---|---|---|---|
| v0.6.0 | 1.5 (incl. lamp pass) | 3.1 | 1.5 | 14.8 |
| every light traced | 28.7 | 24.1 | 20.5 | 85.5 |
| top 6 / 3, no interleave | 17.3 | 10.0 | 8.1 | 45.1 |
| interleaved 2 / 1 (default) | 14.2 | 6.5 | 4.9 | 35.4 |
| interleaved 1 / 1 | 10.2 | 6.5 | 5.0 | 31.5 |
| quarter-res tail pass (6 / block), one-loop hits | 3.6 + 4.4 | 4.6 | 3.0 | 25.0 |
| + opaque first-hit lamp rays, exact lamp 1 ray, flat list <= 48 lights (default) | 1.4 + 2.7 | 4.0 | 2.6 | 19.35 |

A lamp ray costs about 3 ms per pixel here (long, incoherent rays); the exact light's ray about 2 ms.

Later findings (2026-10-07):
- Lamp rays had taken the transmittance walk (the scene has glass), and the exact lamp the tier's 8 sun rays. Lamp
  rays now take the opaque first-hit path, as lamp rays always did, and the exact lamp 1 ray (2 looked identical).
- A list of at most 48 lights is walked whole (`kFlatLightList`): every lane then loads the same light (uniform), where
  per-lane grid cells load different ones. The grid is for scenes with many lights.
- Tried and reverted: a full-resolution tail pass with the quad sharing its rays (6.2 ms vs 2.7: per-pixel surface
  rebuild, divergent lights); tracing a block's second surface at edges (+1 ms; the filter's fallback to the exact
  light's visibility, instead of "fully lit", fixes the white rims alone); a 12-light cell cap (no gain).
- `voxi.lightRaysPerBlock 4` saves 0.6 ms for bright-spot area 0.08% -> 0.31% of the view.
- Per-stage timings (`voxi.rayDrivenStageTiming`) add a barrier after every stage; confirm a win on the total without it.
