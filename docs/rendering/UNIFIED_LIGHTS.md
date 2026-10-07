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

- **List (t18):** lamps and scene lights in importance order (no 32 cap, up to `kMaxListLights` = 4000), then the
  sun as a directional entry whenever ray tracing runs and it has radiance, then the **light grid**: a header record
  (`gDecalParams.y` names it; 0 = no grid), a cell table and an index pool, all packed as floats in the same buffer.
  Up to 32 cells per axis, at least 50 cm, bounded to 200 m around the camera; each cell keeps its 24 most important
  lights. Directional lights are global, not in cells. `rdLightsAt` / `rdLightIndex` (`voxi_rt.hlsli`) walk it.
- **Visible surface (`CSRdShadow`):** one exact light per pixel (the largest unshadowed contribution, the sun's
  full kernel: disc, history keyed by light id, tiles), plus one tail light by weighted reservoir with one shadow
  ray. `gRdLocalOut` = (exact id, tail id, tail visibility, lamp visibility for the raster replay). Stage B shades
  both with the shared `averDirectTerms`. `CSRdLocalLights` is gone.
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
