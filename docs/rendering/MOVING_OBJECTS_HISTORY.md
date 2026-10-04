# Temporal history and moving objects

Ray-traced sun shadows, local-light shadows, AO, reflections and ReSTIR GI all accumulate over
frames. Each reads last frame's answer by reprojecting the current surface point through last
frame's camera (`gPrevViewProj`).

## The bug (2026-10-04)

That reprojection assumed the world does not move. For an animated object (the swaying NeonDistrict
signs, moving cars) or the first-person viewmodel, the projected point lands where the surface was
not. The depth and plane tests then reject the history every frame, so the object shows coarse
one-sample noise (speckle on the weapon), or the history of a neighbour gets smeared onto it.

Separately, a shadow cast by a moving occluder onto a static surface kept 90% of its history per
frame, so the old shadow lingered for about ten frames: dark streaks trailing the swaying signs.

## The fix

- **Object motion in reprojection.** `rdSurfaceFromRecord` (staged ray-driven) and `PSRayDriven`
  (single pass) compute the surface point's rigid motion since last frame from the instance's
  `prevObjectToWorld` and `objectToWorld` and store it in `gAverReprojDelta`. Every history
  reprojection projects `wpos + gAverReprojDelta`, and the ReSTIR plane tests (half-rate visibility
  reconstruction, denoised-GI reprojection) compare last frame's stored position against
  `wpos + gAverReprojDelta`. The delta is exactly zero for static geometry. The raster path leaves it
  zero (its G-buffer velocity has no object motion either).
- **Shadows that change.** When this frame's sun visibility differs from history by more than 0.75
  (`rtShadowChanged`), the blend keeps 35% history instead of up to 90%. An occluder arriving or
  leaving changes a pixel by about 1; penumbra noise from two rays stays near 0.5. Static shadows
  keep their smoothing.

Verified on NeonDistrict Play (walking, `--play-test`): the weapon's speckle is gone, and a debug
tint of `gAverReprojDelta` lights exactly the weapon, cars and animated signs.

## Do not detect the occluder per shadow ray

The first version flagged moving occluders by reading the blocking instance after each shadow ray
(`CommittedInstanceID`, then its transforms). On the owner's RX 7800 XT every variant that kept that
reference live hung the GPU (TDR) within the first frames, whether the transform test sat inside or
after the ray loop, with or without a bounds check, and with the reference returned through a static
or an out parameter. Stubbing the test (which lets the compiler drop the reference) ran clean. Keep
shadow rays free of committed-hit reads; the visibility-change rule above needs nothing from the ray.

## Limits

- Rigid instance motion only. Skinned and soft-body deformation is not in `prevObjectToWorld`.
- The change rule cannot tell a moving occluder from a sudden static change (a light switching), so
  both respond fast.
