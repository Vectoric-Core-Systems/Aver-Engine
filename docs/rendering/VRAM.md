# VRAM

Where the GPU memory goes, and what the engine does to keep it inside the budget. The status bar
shows usage against the OS budget (`IDevice::videoMemory()`): amber above 90%, red over budget.
Over budget, Windows pages resources to system memory and the frame rate drops sharply (measured on
NeonDistrict, 2026-10-03: about 14.4 GB used against a 14.0 GB budget, with denoiser data paged out).

## Measured breakdown (NeonDistrict, Epic GI, before the changes below)

| Category | MiB |
|---|---|
| BLAS (structures + per-BLAS scratch) | 5,227 |
| GI injection accumulator (512³) | 2,048 |
| GI radiance volume (512³ RGBA16F + mips) | 1,170 |
| Ray-driven staged targets | 161 |
| Denoiser | 120 |
| NeuraFI | 101 |
| TLAS | 41 |

Voxi logs its own share as `[Voxi] VRAM by category (MiB)`. Static meshes live on the Upload heap
(system memory, non-local), so they don't count against the local budget; on NeonDistrict they
used 6.1 GB of non-local memory.

## 1. BLAS: shared scratch and compaction (D3D12)

- **Scratch.** Before, every BLAS kept its own build scratch for its whole lifetime. Now only an
  updatable (refit) BLAS keeps one. Every other build borrows a shared scratch buffer (64 MB
  minimum, grows on demand), with a UAV barrier between builds that reuse it.
- **Compaction.** Static BLASes are built with `ALLOW_COMPACTION` and write their compacted size
  to a post-build buffer. The next frame's `beginFrame` reads the sizes and copies each structure
  into a shared 64 MB arena with `CopyRaytracingAccelerationStructure(COMPACT)`. The original
  buffer is then retired behind the fence. The log says
  `[RHI.D3D12] compacted N BLAS: X -> Y MiB`.
- **Moved addresses.** Compaction (and a later rebuild of a compacted BLAS, which needs a
  full-size buffer again) changes the BLAS GPU address. `IResourceFactory::blasGeneration()`
  counts those moves:
  - TLAS owners rebuild when it changes: Voxi's gate, and the path tracer before it traces.
  - A TLAS refit is never used across a move.
- **Pinned BLASes.** BLASes named by a TLAS static prefix (`setTlasStaticInstances`) are pinned
  and never compacted, because their addresses are baked into a persistent buffer. If one is
  rebuilt anyway, the prefix detects the moved address and leaves those instances out of the
  build (logged once) rather than reading freed memory.
- **Arena space.** A compacted BLAS that is later rebuilt leaves its arena slot unused; the arena
  is a bump allocator.
- **Vulkan.** No compaction yet; `blasGeneration()` stays 0.

## 2. GI injection in z slabs

The injection accumulator stores four `R32_UINT` atomics per voxel (r, g, b, fragment count): 16
bytes per voxel, 2 GiB at 512³. Injection now runs in `kGiInjectionSlabs` (2) z slabs, so the
accumulator holds `res / 2` layers (1 GiB at 512³). For each slab, `voxelizePass`:

1. clears the slab's part of the dispatch box;
2. rasterises only the draws whose voxel z range reaches the slab. The slab's first layer travels
   in `gMaterial.w`, and `PSVoxel` drops fragments outside the slab;
3. resolves the slab into mip 0 (`gSlabZ` in `MipCB`).

Costs and side effects:

- Draws that span both slabs are rasterised twice. This only happens on GI rebuild ticks; the
  gate skips most ticks.
- The second slab's injection samples a volume whose first slab is already fresh (the bounce
  feedback term), so one rebuild is not bit-identical to the old single pass. It converges to the
  same result.
- The accumulator is still freed after 240 quiet GI ticks (`voxi.giFreeAccumulator`). A scene
  with constant movers never goes quiet, which is why the size itself had to shrink.

## 3. Mesh vertex sharing

- **LOD levels** share LOD 0's vertex buffer by default (`--lod-share-vertices 1`) through
  `createMeshSharingVertices`, which refcounts the root buffer. Before, every coarser level
  uploaded its own copy of identical vertices.
- **Game depth proxies** (GameContent) share the same way.
- **Split multi-material meshes:** each part is an index buffer over the whole mesh's vertex
  buffer, not a compacted copy. Parts report the whole mesh's bounds (conservative for culling).
- The path tracer copies a shared vertex buffer into its flat table once
  (`MeshRow::copiesVertices`), not once per sharer.

All three fall back to a private copy if the device refuses to share (for example, a
compute-written source).
