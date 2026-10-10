# VRAM

The status bar shows usage against the OS budget (`IDevice::videoMemory()`): amber above 90%, red over budget. Over budget, Windows pages resources to system memory and frame rate collapses. On NeonDistrict (2026-10-04) that showed as 24 fps with frame interpolation, 12 real.

## Measured breakdown (NeonDistrict, Epic GI, Release, 2026-10-03)

| Category | MiB |
|---|---|
| BLAS (structures + per-BLAS build scratch) | 5,227 |
| GI injection accumulator (512³ × 16 B) | 2,048 |
| GI radiance volume (512³ RGBA16F + mips) | 1,170 |
| Ray-driven staged targets | 161 |
| Denoiser | 120 |
| NeuraFI | 101 |
| TLAS | 41 |

Total about 14.4 GB against a 14.0 GB budget. Voxi logs its share as `[Voxi] VRAM by category (MiB)`.

## Caldera pass (2026-10-10)

From the editor's VRAM log on Caldera_Capital (ReSTIR GI fed by Path Tracing, NRD2, 1766x994 render size):
radiance 1170 MiB and injection accumulator 2048 MiB (the 512^3 GI volume), BLAS 1.5-2 GB, the ray-traced
geometry table 1.5-2 GB, NRD2's stabiliser ~180 MiB; about 10-12 GB local of 15.4.

- **The voxel volume is a fallback under ReSTIR GI or Path Tracing** (rough reflections over 0.75 roughness, one
  ambient path); it was sized by the GI tier as if it were the GI. `applyManifestKnobs`: with RENDER.GIMODE 1 or
  Path Tracing on, the tier default stops at 256^3 (146 + 256 MiB, ~2.8 GB less). RENDER.VOXELRES still wins.
  Applied at project load, when the volume is created (it is never resized later).
- **The geometry table's next size** is allocated on a worker once the table is 90% full (was 75%) and released
  when unused for ~30 s (it is up to 1.5x the table; at 75% on a level that stopped growing it was held for good).
- The log now has `[Voxi] VRAM, ray-traced geometry table (MiB): N (P% used)[, next size held M]`.
- **Not done:** BLAS compaction (`ALLOW_COMPACTION` + postbuild size + compacting copy; typically 40-60% of BLAS
  memory), and packing the geometry table's vertices (32 to 16-20 bytes).

## BLAS build scratch freed after build (D3D12)

Static BLASes now get scratch in `buildBlas` and retire it behind the frame fence once the build is recorded. `beginFrame`'s per-frame `collect()` frees it when the GPU is done. Updatable (refit) BLASes keep theirs, as every refit needs it. No acceleration structure moves; TLAS is unaffected.

## First BLAS builds spread over frames (2026-10-04)

A level load used to build every BLAS in one frame, holding all scratch simultaneously (about 2.6 GB on NeonDistrict) on top of the 2 GiB GI injection accumulator. `buildAccelerationStructures` now limits first builds to `kBlasBuildBytesPerFrame` (512 MiB of BLAS per frame), deferring the rest. The TLAS skip gate is bypassed while builds are deferred (`blasBuildsDeferred_`), and the log says `bottom-level builds spread over frames` once. NeonDistrict finishes in 6 frames.

The GI accumulator is freed after 60 quiet GI ticks (was 240). NeonDistrict_Day load + Play at render scale 0.5: peak fell from 16.2 GB to 14.7 GB against a 14.0–14.2 GB budget. It drops to 13.4 GB once the accumulator is freed. Play brings it back to 14.7 GB, because movers keep GI rebuilding and the accumulator is recreated. The next lever is the accumulator's 16 bytes per voxel (512³ = 2 GiB).

## Tried and reverted (2026-10-03, ab97099f, reverted in 456cbbcc)

All three went in together and the build crashed the driver on startup, so none is known-bad individually. Re-introduce one at a time:

- BLAS compaction into a shared arena (moves BLAS addresses; TLAS owners rebuild on a generation counter). Most complex and most likely crash source.
- GI injection in two z slabs (accumulator half as deep). Touches `PSVoxel`/`CSClear`/`CSResolve`.
- Mesh vertex sharing for LODs, depth proxies and split parts (Upload heap, not local VRAM).

Lowering the GI voxel resolution from 512³ to 256³ would free about 2.8 GB with no code change.
