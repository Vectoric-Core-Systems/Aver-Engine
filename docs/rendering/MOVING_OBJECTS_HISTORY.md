# Temporal history and moving objects

Ray-traced sun shadows, local-light shadows, AO, reflections and ReSTIR GI all accumulate over
frames. Each reads last frame's answer by reprojecting the current surface point through last
frame's camera (`gPrevViewProj`).

## The bug (2026-10-04)

That reprojection assumed the world does not move. For an animated object (the swaying NeonDistrict
signs) or the first-person viewmodel, the projected point lands where the surface was not:

- the depth test rejects the history every frame, so the object shows raw one-sample noise
  (speckle on the weapon);
- or it accepts a neighbour's history and smears it.

Separately, a shadow cast by a moving occluder onto a static surface kept 90% of its history per
frame, so the old shadow lingered for about ten frames: dark streaks trailing the swaying signs.

## The fix

- **Object motion in reprojection.** `rdSurfaceFromRecord` (staged ray-driven) and `PSRayDriven`
  (single pass) compute the surface point's motion since last frame from the instance's
  `prevObjectToWorld` and `objectToWorld` and store it in `gAverReprojDelta`. Every history
  reprojection projects `wpos + gAverReprojDelta` instead of `wpos`. The delta is exactly zero for
  static geometry, so static history is unchanged. The raster path leaves it zero (its G-buffer
  velocity has no object motion either).
- **Moving occluders.** `rtShadowEx` reports (`gAverShadowHitMover`) when a blocking hit is an
  instance that moved this frame (`rtInstanceMoved`). The sun-shadow history marks such pixels by
  storing visibility + 2 (`rtShadowHistVis` decodes it). While the current ray hits a mover, or the
  history was shaped by one, the blend keeps 35% history instead of 90%, and tiled pixels trace
  every frame instead of on their turn. The flag drops once the shadow stops changing (less than
  0.02 per frame), so a static shadow a mover once crossed goes back to full smoothing.
- **Tile probe.** A probe ray that hits a mover sets bit 8 in its tile mask, so the tile is never
  treated as uniformly lit or blocked and its pixels trace.

## Limits

- Rigid instance motion only. Skinned and soft-body deformation is not in `prevObjectToWorld`.
- Local-light shadows, AO and GI get the reprojection fix but not the moving-occluder flag.
