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
- **Not done:** packing the geometry table's vertices (32 to 16-20 bytes). BLAS compaction: next section.

## BLAS compaction (D3D12, 2026-10-11)

Voxi's static draw BLASes and terrain/large-mesh chunks are made with `createBlasCompactable` /
`createBlasMultiCompactable` (`ALLOW_COMPACTION`). Each build writes its compacted size to a slot of a small UAV
buffer (postbuild info); the next `compactBlases` copies the slots to a readback buffer, and the first one after
that frame's fence copies each structure (`COPY_MODE_COMPACT`) into new memory of that size, retiring the old
behind the fence. A structure moves only if it saves at least an eighth; at most 64 per frame, and at most one
over 4 MiB (that one is a committed resource made on the main thread).

A move changes the BLAS's address, so it only happens right before Voxi rebuilds `tlas_` in the same call: in
the main accel build, and in the unchanged-gate branch (a still scene compacts too), never while the late scene
pass owes the TLAS its refit. `refitTlas` sees the new addresses and does a full build. Not compacted: foliage
prototypes (the TLAS static prefix bakes their addresses), updatable (skinned/compute) BLASes, and BLASes other
features make. The path tracer shares Voxi's BLASes (`blasForMesh`), so `IResourceFactory::blasMoves()` counts
moves and `PtSceneView` rebuilds its TLASes when it changes. A compacted BLAS that is rebuilt goes back to its
full prebuild size first (also a move).

Vulkan: not implemented (the defaults are full-size creates and a no-op). Console `voxi.rtCompactBlas 0` stops
new compactions. Fixed on the way: a pooled BLAS range freed while a frame was recording waited on a fence that
was never resolved, so the range never went back to the pool.

## BLAS build scratch freed after build (D3D12)

Static BLASes now get scratch in `buildBlas` and retire it behind the frame fence once the build is recorded. `beginFrame`'s per-frame `collect()` frees it when the GPU is done. Updatable (refit) BLASes keep theirs, as every refit needs it. No acceleration structure moves; TLAS is unaffected.

## First BLAS builds spread over frames (2026-10-04)

A level load used to build every BLAS in one frame, holding all scratch simultaneously (about 2.6 GB on NeonDistrict) on top of the 2 GiB GI injection accumulator. `buildAccelerationStructures` now limits first builds to `kBlasBuildBytesPerFrame` (512 MiB of BLAS per frame), deferring the rest. The TLAS skip gate is bypassed while builds are deferred (`blasBuildsDeferred_`), and the log says `bottom-level builds spread over frames` once. NeonDistrict finishes in 6 frames.

The GI accumulator is freed after 60 quiet GI ticks (was 240). NeonDistrict_Day load + Play at render scale 0.5: peak fell from 16.2 GB to 14.7 GB against a 14.0–14.2 GB budget. It drops to 13.4 GB once the accumulator is freed. Play brings it back to 14.7 GB, because movers keep GI rebuilding and the accumulator is recreated. The next lever is the accumulator's 16 bytes per voxel (512³ = 2 GiB).

## Tried and reverted (2026-10-03, ab97099f, reverted in 456cbbcc)

All three went in together and the build crashed the driver on startup, so none is known-bad individually. Re-introduce one at a time:

- BLAS compaction into a shared arena (moves BLAS addresses; TLAS owners rebuild on a generation counter). Most complex and most likely crash source. Redone differently on 2026-10-11 (above): moves only right before the TLAS rebuild that uses them.
- GI injection in two z slabs (accumulator half as deep). Touches `PSVoxel`/`CSClear`/`CSResolve`.
- Mesh vertex sharing for LODs, depth proxies and split parts (Upload heap, not local VRAM).

Lowering the GI voxel resolution from 512³ to 256³ would free about 2.8 GB with no code change.

## Meshes in VRAM, ray hits read them directly (2026-10-17)

Static meshes were on the Upload heap (system memory, read over PCIe by every draw), and Voxi kept a second, VRAM
copy of every mesh for ray hits (the ray-traced geometry table, 2.9-4.4 GB on NeonDistrict).

- Static meshes are on the Default heap by default (D3D12 and Vulkan; `--mesh-heap upload` keeps system memory).
  Uploads are not waited on: the staging ring on D3D12, an unwaited one-shot submit on Vulkan. The mesh stager
  fills an upload buffer and makes the empty Default buffers on the worker; adopting the mesh submits the copy.
  A Vulkan mesh whose device allocation fails goes to host memory.
- Multi-material parts and depth proxies are index buffers over the whole mesh's vertices.
- With `DeviceCaps::rtBindlessBuffers`, the bindless table also holds 8192 vertex-buffer and 8192 index-buffer
  slots (`createBindlessTable`, `setBindlessBuffer`; spaces 2 and 3). Every ray-traced Voxi pipeline reads geometry
  through `rtTriangle`/`rtVertex` (voxi_rt.hlsli): `RtInstance.firstVertex` packs the two slots, `firstIndex` is
  the chunk offset. No geometry table is built and skinned vertices are read where skinning leaves them.
  `AVER_RT_GEOMETRY_TABLE=1` keeps the table for A/B.

NeonDistrict 4000-frame flight against 7492634d: committed RAM 19.5 -> 15.3 GB average, system memory the GPU
uses 5.7 -> 0.7 GB; VRAM about 0.5-1 GB higher mid-flight and 1.5 GB lower at the end (the meshes are now in it,
the table is not), load peak 13.4 GB against a 12.35 GB budget that run. Slow frames unchanged.
Vulkan on NeonDistrict fails before this change too (BLAS and descriptor pools out of memory; one device
allocation per buffer).
