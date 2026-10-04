# VRAM

The status bar shows usage against the OS budget (`IDevice::videoMemory()`): amber above 90%, red over
budget. Over budget, Windows pages resources to system memory and the frame rate collapses. On
NeonDistrict (2026-10-04) that showed as 24 fps with frame interpolation, 12 real.

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

## BLAS build scratch is freed after the build (D3D12)

A static (non-updatable) BLAS used to keep its build scratch for its whole life. It now gets scratch
in `buildBlas` and retires it behind the frame fence once the build is recorded; `beginFrame`'s
per-frame `collect()` frees it when the GPU is done. A rebuild allocates scratch again. Updatable
(refit) BLASes keep theirs, because every refit needs it. No acceleration structure moves, so TLAS
contents are unaffected.

## First BLAS builds are spread over frames (2026-10-04)

A level load used to build every BLAS in one frame. All of their scratch was alive at once (about
2.6 GB on NeonDistrict), on top of the 2 GiB GI injection accumulator. `buildAccelerationStructures`
now stops first builds after `kBlasBuildBytesPerFrame` (512 MiB of BLAS) in a frame, and the rest
follow on later frames. The TLAS skip gate is bypassed while builds are deferred
(`blasBuildsDeferred_`), and the log says `bottom-level builds spread over frames` once. NeonDistrict
finishes in 6 frames. The GI accumulator is freed after 60 quiet GI ticks (was 240).

NeonDistrict_Day load + Play, render scale 0.5: the peak fell from 16.2 GB to 14.7 GB against a
14.0-14.2 GB budget. It drops to 13.4 GB once the accumulator is freed. Play brings it back to 14.7
GB, because movers keep GI rebuilding and the accumulator is recreated. The next lever is the
accumulator's 16 bytes per voxel (512^3 = 2 GiB).

## Tried and reverted (2026-10-03, ab97099f, reverted in 456cbbcc)

All three went in together and the build crashed the driver on startup, so none is known-bad
individually. Re-introduce one at a time:

- BLAS compaction into a shared arena (moves BLAS addresses; TLAS owners rebuild on a generation
  counter). Most complex and the most likely crash source.
- GI injection in two z slabs (accumulator half as deep). Touches `PSVoxel`/`CSClear`/`CSResolve`.
- Mesh vertex sharing for LODs, depth proxies and split parts (Upload heap, so not local VRAM).

Lowering the GI voxel resolution from 512³ to 256³ would free about 2.8 GB with no code change.
