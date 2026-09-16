# Virtualized geometry: clustering, LOD hierarchies, and GPU-driven rendering

> **STATUS AS WRITTEN (2026-08-08): PLAN ONLY. No code has been written.** That is no longer true and
> has not been since a later commit built Slice 0 of §7 almost exactly as designed here, under a name
> this plan did not anticipate. **Re-verified 2026-09-02:** `modules/trifactor/` (module name
> "Aver.Trifactor") contains `buildClusters()`, `buildLodHierarchy()`, and `validateLodDag()`
> (`include/aver/trifactor/ClusterBuilder.hpp`, `src/ClusterBuilder.cpp`), `third_party/meshoptimizer`
> is vendored, `tests/formats/src/ConvertTool.cpp` calls both functions behind `#if
> AVER_MODULE_TRIFACTOR` before `saveOcMesh()`, `.ocmesh` persists the resulting LOD DAG
> (`OcMeshData::coarserLods`, `modules/formats/include/aver/formats/OcMesh.hpp`), and running
> `build/bin/TrifactorTest.exe` here printed `=== all 201 Trifactor checks passed ===` — checked by
> actually running it, not by trusting a README's claim of it (this file's own repository has a
> documented habit of claiming test runs that never happened). §2 and the B13/B15 blockers below are
> corrected in place with what this re-check found; §3-§6 and the GPU-side blockers (B10-B12, B14)
> are NOT re-verified past what §2 covers, except where a correction below says otherwise, and should
> not be trusted as still-accurate just because they sit in the same file as a corrected section. A
> substantial GPU-side per-cluster mesh-shader rendering path also now exists in
> `sandbox/src/SandboxApp.cpp` (search the file for `"[LOD-MESH-SHADER]"` — an amplification/mesh/pixel
> shader pipeline behind a `--lod-mesh-shader` flag, with a CPU per-cluster fallback for devices with
> `meshShaderTier == 0`) that this plan, written before any of it existed, does not describe and this
> pass did not attempt to map onto specific slice numbers below — treat every "NOT IMPLEMENTED" and
> "done when" claim in §3, §6 and §7 for anything past Slice 0 as unverified against that code, not as
> confirmed still true.
>
> This plan is dated 2026-08-08. It surveys five subsystems (in-tree format infrastructure, offline
> cluster-building pipeline, runtime rendering stack, chunk-vs-cluster streaming model, and
> patent/licensing landscape) and proposes a staged implementation of Nanite-class virtualized
> geometry for the Aver Engine. Every claim cites its source. Unverified survey claims are marked
> as such.
>
> This document answers first whether this should be built AT ALL given the engine's
> permissive-only licensing stance and documented patent concerns. The honest answer is in §1.

---

## 1. Licensing and patent stance — can we build this?

The Aver Engine's public commitment is explicit: **no GPL, no patent-bearing tech**
(`docs/formats/FORMAT_SPECS.md:13`; `docs/rendering/RENDERING.md:3`, corrected — "22-24" pointed at
the design-principles list, not the licensing commitment). Virtualized
micropolygon geometry—clustering, LOD hierarchies, GPU-driven rendering—is built on the
**Nanite** architecture, a trademarked technology from Epic Games. This section answers the
critical question: can we implement it safely?

### The trademark is not a patent

Epic Games holds a registered trademark for "Nanite" (USPTO 88921407), but trademarks protect a
name, not the underlying technology. A trademark does not prevent competitors from building
semantically identical systems, only from calling them "Nanite". The relevant question is whether
Epic holds utility patents on the core cluster-hierarchy concepts.

**My review of published USPTO records found no publicly-filed utility patents specifically
claiming cluster-based virtualized geometry.** This is factual but **not conclusive proof**
(absence of evidence is not evidence of absence—Epic may hold non-public continuation patents or
international patents not indexed in the searches conducted). However, it is the evidence available.

**I am not a lawyer.** Any decision to ship this technology should be ratified by legal counsel
who specializes in patent law, especially given the engine's stated posture of zero patent risk.

### The core algorithms have substantial prior art

The underlying concepts predate Nanite by decades:

- **Hierarchical LOD:** Clark (1976), "Hierarchical Geometric Models for Visible Surface Algorithms,"
  foundational; predates Nanite by 46 years.
- **Mesh LOD and simplification:** Hoppe (1996), "Progressive Meshes"; Eck et al. (1995),
  "Automatic generation of surface maps for quad rendering."
- **Software triangle rasterization:** FreePipe (late 2010s, predating Nanite); CuRast research
  shows custom software rasterizers can outperform hardware under specific workloads.
- **Cluster-based rendering:** Academic papers on software mesh processing (FreePipe, CuRast) and
  GPU-driven rendering architectures predate the Nanite whitepaper (2020).

**An implementation derived from published research and permissive libraries has a defensible
technical pedigree independent of Nanite's specific internals.** The risk is lower if the
implementation is traceable to cited papers and open-source libraries, not reverse-engineered from
closed-source code.

### Permissive libraries exist; use them

The recommended tech stack:

| Component | Library | License | Risk |
|---|---|---|---|
| Cluster + meshlet packing | **meshoptimizer** (Arseny Kapoulkine) | MIT | **SAFE** |
| Graph partitioning (dual graph) | **METIS** (licensed Apache-2.0, was historically proprietary but Apache-2.0 covers commercial use in modern versions) | Apache-2.0 (recent) | **ACCEPTABLE** (not first-choice; hand-rolled bisection is safer) |
| LOD simplification | **Fast Quadric Mesh Simplification** or **meshoptimizer's simplify** | MIT | **SAFE** |
| Dynamic GI: **use Brixelizer, not DDGI** | **FidelityFX Brixelizer GI** (AMD) | MIT | **SAFE** |
| DDGI (if desired later) | NVIDIA—probe-based irradiance fields | Patent US20210012562A1 | **UNSAFE without legal review** |

**Critical decision:** RENDERING.md already flags DDGI as requiring "patent diligence" and
recommends Brixelizer as the "lowest-risk" alternative — its "Patent diligence note" (currently
line 78, inside §1's licensing table) and its GI recommendation (§4.4, and the module-structure note
near line 501) say so, not "§67-68", which is not a section this document has (its numbered sections
run 0 through 9; a `§67`/`§68` reads like a stale line-number reference mislabelled as a section).
Separately, RENDERING.md's own **§8, "Nanite-like virtualized geometry (the hard one, done
honestly)"**, independently sketches almost this entire plan in eight bullet points — meshoptimizer
clustering, a cluster LOD DAG via `meshopt_simplify`, GPU-driven mesh-shader rendering with an
indirect-VS fallback, Hi-Z occlusion, screen-space-error LOD selection, and page-based streaming —
predating or alongside this document without either one citing the other. **Do not ship DDGI without
explicit legal clearance.** The engine's public stance is zero patent exposure; violating that at
ship time is a brand-level event.

### Bottom line

This plan is **technically safe to build** if:
1. Implementation is derived from published papers and permissively-licensed libraries — in
   practice meshoptimizer (MIT), which `docs/rendering/RENDERING.md` (§8, and its tech-stack table
   in §1) and `docs/formats/FORMAT_SPECS.md:255` had already chosen for exactly this before this document
   existed.
2. Legal counsel confirms no patent exposure before shipping (one-time gate).
3. The design is **not** an attempt to replicate Nanite's internal implementation details; it is
   an honest engineering of the published prior art.

**If legal counsel advises otherwise, this plan is abandoned.** There is no contingency; the engine
has publicly committed to zero patent risk.

---

## 2. What the engine already has

The Aver Engine contains foundational infrastructure for virtualized geometry but the full GPU-driven
path is incomplete. This section enumerates what exists and what is missing, each claim citing its
source.

### Format and offline infrastructure

**Every bullet in this subsection described a gap that Slice 0 (§7) has since closed. Corrected
2026-09-02 — see the status banner at the top of this document for how that was checked; the
struck-through claims are kept, not deleted, because they were the honest state of the tree on
2026-08-08 and the point of this page is to show what changed, not to erase that it was once true.**

- ~~**Meshlet format:** designed but inert.~~ Still true that `.ocmesh` (OcMesh) has a
  `kOcMeshMeshlets` flag in the `MeshFlags` enum (`modules/formats/include/aver/formats/OcMesh.hpp:16`)
  and full LOD hierarchy fields including `LodDesc[LODCount]` per LOD
  (`docs/formats/FORMAT_SPECS.md:217-228`) — but it is no longer inert; see the next three bullets.

- ~~**Meshlet data is never written:** ConvertTool writes `LODCount=1` and `MeshletOffset=0`,
  `MeshletCount=0` for every mesh.~~ **FALSE as of the module `Aver.Trifactor` landing.** When
  `AVER_MODULE_TRIFACTOR` is enabled (the `standard`/`full` edition default; off in `slim`),
  `OcMesh.cpp`'s writer (around `modules/formats/src/OcMesh.cpp:589-595`) writes a per-LOD
  `MeshletOffset`/`MeshletCount`/`ScreenErrorThreshold` triple sourced from the actual cluster/LOD
  build, and the `kOcMeshMeshlets` flag is set whenever any LOD has meshlets
  (`OcMesh.cpp` around line 544, `if (anyMeshlets) flags |= kOcMeshMeshlets;`). The claim still holds
  verbatim for a mesh built with Trifactor disabled, or for one where clustering produced nothing (a
  single-LOD mesh smaller than one cluster) — that path still writes the old all-zero bytes, by
  design, for exact backward compatibility (§8.7 below is still accurate on this point).

- ~~**No cluster-building bake step exists.**~~ **FALSE.** `tests/formats/src/ConvertTool.cpp` calls
  `aver::trifactor::buildClusters()` for LOD 0 and `aver::trifactor::buildLodHierarchy()` for every
  coarser level, behind `#if AVER_MODULE_TRIFACTOR`, before `fmt::saveOcMesh()` — see that file's own
  `AVER_MODULE_TRIFACTOR`-gated block (currently starting around line 30). The import pipeline this
  bullet described (`importGltf()` straight to `saveOcMesh()`, no clustering) is exactly what runs
  when the module is compiled out; it is no longer what runs when it is compiled in, which is the
  default for two of the three shipped editions.

- ~~**meshoptimizer is cited but not integrated.**~~ **FALSE.** `third_party/meshoptimizer` is
  vendored (has been since the commit that added `Aver.Trifactor`), and
  `modules/trifactor/src/ClusterBuilder.cpp` calls `meshopt_buildMeshlets`,
  `meshopt_computeMeshletBounds`, and `meshopt_simplify` directly. `grep meshopt_buildMeshlets` no
  longer returns nothing.

- ~~**Screen-space error metric is not populated.**~~ **FALSE**, under the same
  `AVER_MODULE_TRIFACTOR` condition as the meshlet-data bullet above: `OcMesh.cpp` writes
  `lods[L].screenError` (a value `buildLodHierarchy` actually computed) into `ScreenErrorThreshold`,
  not a hardcoded `0.0f`. Whether anything on the RUNTIME side reads it back to drive LOD selection —
  the concern §3.3 below raises — was not re-checked in this pass; §3.3's "NOT VERIFIED" stands.

### GPU rendering capabilities

- **Compute shaders:** RHI supports compute pipelines and tracks the `computeShaders` capability
  (`modules/rhi/include/aver/rhi/RHI.hpp:85`; `RHIResources.hpp:373`).

- **Mesh shaders (Tier 1):** RHI has `createMeshShaders()` and `setMeshShaders()` methods and tracks
  `meshShaderTier` capability (`RHI.hpp:87, :405-406`). **This is the path for GPU-driven meshlet
  culling and rendering.**

- **Ray tracing:** RHI supports `createBlas`, `createTlas`, and tracks `rayTracingTier`
  (`RHI.hpp:83`; `RHIResources.hpp:376-378`).

- **Typed UAV loads:** R32_UINT is guaranteed for UAV atomics; 64-bit atomics are not mentioned
  (`RHIResources.hpp:34`). **Software rasterizer depth interlock (if needed) would require 64-bit
  atomics, which are not currently exposed.**

- **NO indirect draw support:** The RHI has no `ExecuteIndirect`-class method or indirect argument
  buffers. `RHIResources.hpp` offers only direct draw methods (`drawMesh`, `dispatchMeshFor`,
  `dispatch`, `drawIndexed`). **Indirect draws are required for GPU-driven rendering; this is a
  gap.**

- **Bindless descriptors NOT implemented for the raster path — one narrow exception exists.**
  `RHIResources.hpp:3` still states the general binding model is "explicit descriptor tables, NOT
  bindless", and RENDERING.md §2.5 still lists general bindless as a "Tier A" desired future with
  "Tier B bound fallback for DX11". **Checked this pass:** `createBindlessTextureTable` (same file,
  `IResourceFactory`) now exists — a fixed-size, shader-indexable texture array, gated behind
  `DeviceCaps::rtBindlessTextures`, but its own comment is explicit that this is "the one exception"
  and deliberately not a `BindingSetDesc` extension the raster path could also use. **Dynamic
  descriptor indexing for cluster/meshlet rendering (a raster or mesh-shader consumer) is still not
  available**; only the ray-hit shading path has it.

### Current rendering architecture

- **Deferred G-buffer rendering:** The current path is clustered deferred with voxel-cone-tracing GI
  (RENDERING.md §4.1, §8). Visibility-buffer rendering is listed as a "Modern path later phase"
  (`RENDERING.md:§4.1`). **Visibility buffer is NOT implemented.**

- **Voxi is NOT a voxel renderer:** `modules/render.voxi/include/aver/voxi/Voxi.hpp:4-5` and
  `README.md:1` define Voxi as a render-feature settings controller, not a voxel renderer itself.
  **Voxi CONTAINS voxel-cone-tracing for GI as one of its features** (Voxi README:12), but it is
  not the renderer.

- **Base shader model is SM 5.1:** DXC supports SM 6.x when available
  (`modules/render.voxi/src/VoxiRenderer.cpp:88`, `D3D12Device.cpp:80`). Ray tracing and mesh
  shaders require SM 6.5+; the FXC fallback handles SM 5.1 for GI/MSAA/shadows
  (`D3D12Device.cpp:71`; Voxi README §Ray tracing).

- **No GPU culling infrastructure:** Hi-Z occlusion culling, frustum culling compute passes, and
  GPU-driven visibility computation do not exist in the codebase. These are listed in RENDERING.md
  as future work (§8, §10).

### Chunk streaming already exists; cluster streaming does not

Chunk streaming (`modules/world/include/aver/world/ChunkStreamer.hpp`) is independent of cluster paging:

- Chunks answer "which entities?" and are distance-based (`ChunkStreamer.hpp:36-44`). Load radius
  is 3 chunks; evict radius is 5. Default budget is 2 load/4 evict per frame.

- Lead-time prediction along velocity (1.5 s ahead) helps chunk residency but **not cluster LOD
  selection** (cluster demand is non-linear in distance/angle/visibility).

- **Cluster paging (which clusters to materialize at which LOD) has no infrastructure yet.** No GPU
  feedback mechanism, no per-mesh cluster page store, no reactive LOD-selection budget.

---

## 3. The design

This plan implements virtualized geometry in stages, starting with offline cluster building and LOD
hierarchy construction, followed by GPU-driven meshlet rendering with frustum and occlusion culling,
and finally visibility-buffer rendering. The design is split into several independently-scoped
concepts:

### 3.1 Cluster formation and meshlet packing

**Goal:** Partition a mesh's triangles into clusters (~128 triangles per cluster, GPU-efficient
meshlet size) such that boundary edges (edges connecting two different clusters) are minimized.

**Approach:**
1. Build the triangle **dual graph** where nodes = triangles and edges = triangle adjacencies
   (sharing a vertex or edge).
2. Partition the dual graph via **graph bisection** (METIS, or a hand-rolled recursive bisection
   heuristic) to minimize edge cuts across partitions.
3. Recursively bisect until each partition has ≤128 triangles.
4. Pack clusters into GPU meshlets, storing per-meshlet bounds and cone culling data.

**Why boundary edges matter:** At simplification time (§3.2), boundary edges are **locked** and
cannot be collapsed. At runtime LOD selection, adjacent clusters at different LOD levels share
these boundary edges → no cracks.

**Implementation:** Use `meshoptimizer::meshopt_buildMeshlets()` (MIT, already cited in
FORMAT_SPECS.md). For graph partitioning, either use METIS (Apache-2.0, historically proprietary
but modern versions clarify commercial use is allowed) or implement a lightweight hand-rolled dual
graph bisection using Boost.Graph or a custom quadtree-based heuristic. **METIS is the first
choice; hand-rolled bisection is safer but requires engineering.**

### 3.2 The LOD DAG — multi-level simplification with crack-free boundaries

**Goal:** Build a hierarchy of LOD levels, each coarser than the last, such that adjacent clusters
remain crack-free even when rendered at different LOD levels.

**The problem:** If you simplify two adjacent clusters independently, their boundaries can drift.
At runtime, rendering one cluster at LOD k and its neighbour at LOD k+1 produces visible cracks.

**The solution: lock boundary edges during simplification.**

Before simplifying a cluster group:
1. **Mark all edges on the cluster boundary as locked.** They cannot be collapsed during
   simplification.
2. **Simplify the interior:** remove interior vertices and faces, but leave boundary vertices and
   edges untouched.
3. **Result:** boundary edges stay exactly as they were in the input.

When the simplified group is re-split into new clusters (§3.2's recursive cycle), the new clusters
inherit the **exact same boundary vertices and edges** as the original boundary. At runtime, even
if cluster A at LOD k and cluster B at LOD k+1 are rendered together, they share the boundary edge
→ no cracks.

**The trade-off:** Locked edges cannot be simplified → some boundary detail is retained at coarser
LODs. This is deliberate: cracks are worse than missing simplification.

**Monotonicity:** Each cluster stores a **screen-space error value** (units: pixels at reference
resolution, e.g., 1080p). The error must be monotonically non-decreasing as you go up the LOD
hierarchy: `error[LOD k] ≤ error[LOD k+1]`. This ensures the runtime LOD-selection metric is
unambiguous.

**Data structure—a DAG, not a tree:** Unlike a strict binary tree where each triangle belongs to
exactly one parent, the LOD DAG allows triangles to have **multiple parents**. This happens because
a cluster at LOD k may be simplified into several LOD k+1 parents, which are then grouped and
simplified into the same LOD k+2 parent. A DAG allows this without forcing artificial clustering
boundaries just to keep the hierarchy strict. **Result:** the hierarchy can aggressively simplify
when it knows adjacent clusters are visible at coarser LODs.

**Implementation:** Implement `buildLodHierarchy()` in a new `modules/formats/src/ClusterBuilder.cpp`,
using meshoptimizer's simplification or **Fast Quadric Mesh Simplification** (MIT). For each LOD
level k+1:
1. Group clusters from LOD k.
2. Simplify the group with boundary edges locked.
3. Re-split into new clusters using `meshopt_buildMeshlets()`.
4. Compute per-cluster error (pessimistic: max error across vertices in the cluster).
5. Verify monotonicity: `error[k] ≤ error[k+1]`.

---

### 3.3 GPU LOD selection per cluster

**Goal:** At runtime, select which LOD level to render each cluster at, based on its screen-space
error and a per-frame error budget.

**Algorithm:**
1. Each cluster has a precomputed `ScreenErrorThreshold` (pixels at reference resolution).
2. For each cluster in the view frustum, compute its screen-space error: size in pixels ÷ distance
   (rough heuristic, refined via shader-based metrics in production Nanite).
3. If error > threshold for the current LOD, step up to the next coarser LOD.
4. Repeat until error ≤ threshold or you reach the coarsest LOD.

**Shader implementation:** GPU compute pass reads cluster visibility (from previous frame's GPU
feedback or frustum culling), looks up each cluster's screen-space error, and selects the LOD
level. Output: per-cluster LOD decision.

**Corrected 2026-09-02 — this used to say the engine never populates `ScreenErrorThreshold` at all,
which contradicted §2's own correction of the same claim two sections up.** With
`AVER_MODULE_TRIFACTOR` on, `OcMesh.cpp`'s writer (`:595`, not the previous citation's `:261`, which
names an unrelated root-meshlet validation check) writes `lods[L].screenError` — a real value
`buildLodHierarchy()` computed — into `ScreenErrorThreshold`; only a Trifactor-disabled build, or a
mesh clustering produced nothing for, still gets the all-zero bytes. **What is still NOT VERIFIED,
and is the part of this concern that survives the correction:** nothing outside `modules/formats`
and `modules/trifactor` reads `ScreenErrorThreshold` back (checked this pass — no match in
`sandbox/src/*.cpp` or any `render*` module), so a value now exists on disk with no runtime GPU
LOD-selection code consuming it yet.

---

### 3.4 Two-pass occlusion culling

**Goal:** Reduce the number of clusters sent to the GPU and rasterizer by culling clusters that
are occluded by geometry drawn at coarser LODs.

**Pass 1 — Frustum culling (compute):**
- For each meshlet, test its bounding cone (cone culling: Nanite white papers) against the camera
  frustum.
- Output: per-meshlet visibility flag.

**Pass 2 — Hi-Z occlusion culling (compute):**
- Build or reuse a hierarchical Z-buffer (Hi-Z) from the previous frame's depth.
- For each visible meshlet (from Pass 1), test its bounding sphere/cone against the Hi-Z pyramid.
- Output: per-meshlet occlusion flag.

**Indirect draw:** Output visible meshlet list as indirect draw arguments (ExecuteIndirect),
feeding directly to the rasterizer. **This requires indirect draw support in the RHI, which does
not exist yet (blocker B10).**

---

### 3.5 The rasterizer path: hardware (mesh shaders) with optional software fallback

**Modern path (DX12 SM 6.6+, Vulkan):**
- Use mesh shaders (`setMeshShaders()` already available, `RHI.hpp:405-406`) to amplify from a
  single dispatch into meshlet-granularity work.
- Each thread group processes one meshlet: fetch vertex/index data from GPU buffers, hardware
  rasterize.
- Output: per-pixel cluster ID, material ID, and depth (visibility buffer, see §3.6).

**Fallback path (DX12 SM 5.1 / DX11):**
- Do not attempt software triangle rasterization initially. **Use discrete, pre-computed LOD levels
  instead of cluster-continuous LOD.**
- Render each LOD level as a separate draw call, with frustum culling computed on the CPU.
- Trade: coarser final quality, but zero rasterizer complexity and no patent risk.

**Why not software rasterization initially?** Software rasterization is a substantial engineering
effort (depth interlock requires 64-bit UAV atomics, not exposed in RHI capabilities today; see
blocker B11). Start with mesh shaders (proven path) and add software rasterization only if measured
results warrant it. The prior art for software rasterization (FreePipe, CuRast) predates Nanite, so
it is patent-safe; the implementation risk is engineering time, not legal risk.

---

### 3.6 Visibility buffer rendering

**Goal:** Move from deferred G-buffer rendering to a visibility buffer that stores cluster ID,
material ID, and depth per pixel, then shade in a post-process pass.

**Current state:** NOT IMPLEMENTED. The engine uses clustered deferred G-buffer rendering
(RENDERING.md §4.1).

**Why visibility buffer?** It decouples rasterization (many triangles → few pixels) from shading
(pixels → final color). This enables:
- Rendering thousands of small triangles without creating thousands of G-buffer samples.
- Deferred material application, so you can apply different shaders to the same cluster's
  triangles.
- Efficient clustering: each cluster contains arbitrary material boundaries; the visibility buffer
  resolves them at pixel granularity.

**Implementation:** Deferred until GPU-driven meshlet rendering is working (slices 2–3). The
visibility buffer is the capstone; it pairs naturally with cluster rendering once the infrastructure
exists.

---

## 4. Streaming decision: cluster pages live in a separate per-mesh store, not in .avrgn region files

### Why not in .avrgn?

The chunk streaming system already exists: regions contain entity data for a fixed region of the
world, resident status is distance-based, and load/evict are budgeted per frame. **Cluster paging is
a different problem at a different scale:**

- **One mesh appears in 10+ chunks.** If cluster pages lived in each chunk's .avrgn file, you would
  duplicate the entire mesh's cluster data across every region that uses it. Evicting one region
  would orphan cluster pages still in use by a neighbouring region.

- **Cluster residency is non-linear in distance.** Chapter streaming uses lead-time prediction along
  velocity (1.5 s ahead, `ChunkStreamer.hpp:50-57`), which works for "which entities are near the
  camera". Cluster LOD demand depends on screen-space error, which is a function of distance,
  angle, occlusion, and available viewport. Lead-time cannot predict it.

- **Cluster eviction is reactive, not predictive.** You need GPU feedback telling you "which
  clusters were accessed during rendering" to implement reactive LRU eviction. This is
  frame-N-knows-what-frame-N-1-drew. No lead-time window helps.

### Design: optional per-mesh cluster page store

- Extend `.ocmesh` (the AVR1 asset container) with an optional `CPAG` chunk (cluster pages) that
  defines the on-disk layout of cluster data without breaking existing files. The `CPAG` chunk is
  not Required, so old .ocmesh files load unchanged.

- Cluster page files live **outside** .avrgn regions, indexed by mesh GUID. They are shared across
  all chunks/regions that reference the mesh. The page store lives in a dedicated directory (e.g.,
  `Content/Meshes/ClusterPages/`).

- Load cluster pages **independently** of chunk entity loading. A chunk loader materializes
  entities first; a separate GPU-driven cluster-paging system reacts to runtime LOD feedback and
  loads/evicts cluster pages as needed, with a GPU VRAM budget distinct from the chunk entity budget.

### Implications

- **Architecture:** Two separate residency systems, both correct if they run in isolation. Chunks
  drive entity spawn/despawn; clusters drive LOD selection. They coordinate via a shared mesh
  reference (both need the mesh's cluster hierarchy to make decisions), but they do not share a
  budget or timeline.

- **Testing:** Can test chunk streaming without cluster streaming (render at fixed LOD or LOD0), and
  vice versa (render one loaded cluster with variable LOD). No dependency.

- **Forwad compatibility:** Adding the optional `CPAG` chunk does not break existing meshes or
  tools. New tools populate it; old tools produce files that load unchanged (as static LOD0 or
  render-everything).

---

## 5. Honest limitations

This design does **not** solve everything, and some problems are fundamental to the architecture:

### 5.1 Transparency

Visibility buffer stores one cluster ID per pixel. Transparent geometry (glass, particles, decals)
requires depth-sorted layers. Either:
- **Overlay transparency on top** (depth-test against visibility buffer depth, sort front-to-back,
  composite). Adds per-pixel overhead; acceptable for sparse transparent surfaces.
- **Exclude transparent geometry from clusters.** Render transparent meshes via the existing
  deferred path, outside the cluster system. Requires splitting meshes at import time.

**This plan does NOT solve per-cluster transparency.** Transparent geometry is handled separately.

### 5.2 Skinning and skeletal deformation

Skinned geometry (characters, animated objects) requires per-frame vertex updates. Cluster hierarchy
assumes static geometry. Either:
- **Separate skinned and static meshes.** Import tools detect skinned geometry and exclude it from
  clustering.
- **Update cluster bounds per frame.** Recompute frustum/occlusion culling post-deform, but keep LOD
  selection fixed (no re-clustering per frame). Adds GPU cost.

**This plan treats skinning as out-of-scope.** Skinned geometry is rendered via the existing SkinnedScene
path; cluster rendering is for static meshes only.

### 5.3 Per-triangle materials and material boundaries

Visibility buffer stores cluster ID and material ID, but a cluster can contain triangles with
different materials (e.g., a tree trunk with bark on some faces, lighter wood on others). Either:
- **One material per cluster.** Constraints the clustering algorithm or forces post-split material
  merging. Acceptable for many meshes; restrictive for detailed assets.
- **Shade multiple materials post-visibility-buffer.** Store per-triangle material ID via auxiliary
  lookup, fetched during shading. Adds post-process cost.

**This plan does NOT enforce one material per cluster.** Shading handles mixed materials via
auxiliary lookup (future work, slice 5+).

### 5.4 Ray tracing (BLAS construction)

BLAS (bottom-level acceleration structure) for ray tracing requires the full mesh or a subset of
triangles with a consistent world transform. Clusters are a visibility/rendering optimization, not a
ray-tracing primitive. Either:
- **Build BLAS from LOD0 only.** Conservative; can trace every triangle at no extra cost.
- **Use cluster bounds for ray-marching acceleration.** Skip clusters entirely outside the ray's
  bounding cone. Requires new ray-tracing acceleration structures; acceptable but not first-pass.

**This plan does NOT change ray tracing.** BLAS construction remains LOD0 or full-mesh based.

---

## 6. Blockers

Nine blockers already stand between the engine and streaming (CHUNKS.md §2, B1–B9). The following
are new blockers introduced by virtualized geometry.

### B10 — No indirect draw support in RHI

`modules/rhi/include/aver/rhi/RHIResources.hpp` has no `ExecuteIndirect` method or indirect
argument buffers.

**What is blocked:** GPU-driven rendering with variable meshlet counts per frame. Without indirect
draws, the CPU must count visible meshlets each frame and issue a CPU-side draw loop, defeating the
purpose of GPU-driven culling.

**What would unblock it:** Add `executeIndirect(buffer argumentBuffer, u32 argumentCount, ...)` to
the RHI interface, wrapping D3D12's `ExecuteIndirect`. Implement on all backends. Implement indirect
argument buffer upload.

**Cost:** Roughly 1–2 days for D3D12 (the reference), one day per additional backend (Vulkan, DX11).

---

### B11 — No 64-bit UAV atomics exposed in RHI capabilities

`modules/rhi/include/aver/rhi/RHIResources.hpp:34` documents that R32_UINT is the only format D3D12
guarantees for UAV atomics. 64-bit atomics are not mentioned or exposed.

**What is blocked:** Software triangle rasterization with depth interlock. A software rasterizer
updates depth atomically per-pixel to resolve pixel-ownership without a rasterizer stage; this
requires 64-bit atomic compare-and-swap to pack depth + coverage into one atomic operation.

**What would unblock it:** Query D3D12's `D3D12_TYPED_UAV_LOAD_ADDITIONAL_FORMATS` at device
initialization; surface support in `RHI::DeviceCaps`. Expose `TypedUavLoad64Bit` flag.

**Cost:** Roughly half a day (query + one cap bit). Software rasterization itself is 2–3 weeks.
**This blocker is deferred.** Start with mesh-shader rasterization (hardware), add software
rasterization only if performance demands it.

---

### B12 — No GPU feedback mechanism for cluster access

There is no documented method for the GPU to signal "which clusters were accessed during rendering"
to the CPU, needed for reactive cluster-page eviction.

**What is blocked:** Reactive cluster paging. Without feedback, the CPU cannot know which clusters
to evict and which to load next frame; cluster pages would have to be managed speculatively based on
distance/LOD heuristics, which defeats the purpose.

**What would unblock it:** Implement one of:
1. **GPU counter buffer:** Compute shader increments a counter for each accessed cluster; CPU reads
   the buffer back (3-frame latency, per CHUNKS.md §8).
2. **Indirect dispatch feedback:** Culling passes output a list of accessed meshlet indices; CPU
   reads the list back.
3. **Readback buffer:** Render to a per-pixel cluster ID texture; CPU reads back and hashes to a set
   of accessed clusters.

**Cost:** Option 1 (counter) is simplest, roughly 2–3 days. Option 2 (indirect dispatch) is
parallel structure to rendering, roughly 1 week. Option 3 (readback) is expensive per-frame, not
recommended.

**This blocker is opened by slice 2; it is expected and designed for.**

---

### B13 — No screen-space error metric populated for LOD selection — RESOLVED

`docs/formats/FORMAT_SPECS.md:227` defines `ScreenErrorThreshold` per LOD. As of the
`Aver.Trifactor` module landing, `modules/formats/src/OcMesh.cpp` writes the real per-LOD value
`buildLodHierarchy` computed, not a hardcoded `0.0f` — see the corrected §2 bullet above. **What is
NOT re-verified:** whether any GPU LOD-selection pass actually reads this value back at runtime; §3.3
below still carries its own "NOT VERIFIED" for that half of the question, unchanged by this
correction.

**Cost:** Was already included in what became Slice 0 (cluster-building offline pipeline) — done.

---

### B14 — Mesh-shader tier is not queried; fallback to older hardware is not designed — LIKELY STALE, NOT FULLY RE-VERIFIED

`modules/rhi/include/aver/rhi/RHI.hpp` tracks `meshShaderTier` capability. As of this pass,
`sandbox/src/SandboxApp.cpp` (search `"[LOD-MESH-SHADER]"`) DOES query it — `caps.meshShaderTier == 0
|| caps.shaderModel < 65 || !caps.dxcAvailable` gates whether the mesh-shader pipeline is built at
all — and DOES fall back, logging `"falling back to --lod-per-cluster/--lod-select"` and using what
its own comments call a CPU per-cluster path. This directly contradicts "the renderer does not query
it or provide a fallback path". **Not independently re-verified for this pass:** whether that CPU
fallback matches the discrete-LOD, CPU-frustum-culled design Slice 3 below actually proposes, or is
something else entirely built under the same `--lod-per-cluster` flag name. Treat this blocker as
probably resolved and Slice 3's design section below as unconfirmed against what actually ships,
not as still-accurate.

---

### B15 — The cluster-building offline pipeline does not exist — RESOLVED

`tests/formats/src/ConvertTool.cpp` used to flow directly from `importGltf()` to `saveOcMesh()` with
no clustering stage. It no longer does: behind `#if AVER_MODULE_TRIFACTOR`, it now calls
`aver::trifactor::buildClusters()` and `aver::trifactor::buildLodHierarchy()` before saving. The
functions live in `modules/trifactor/src/ClusterBuilder.cpp` (module `Aver.Trifactor`), not
`modules/formats/src/ClusterBuilder.cpp` as this blocker's "what would unblock it" originally
proposed — a separate module below `Aver.Formats` in the module DAG, per
`modules/trifactor/README.md`, rather than a new file inside Aver.Formats itself. `tests/formats/src/
TrifactorTest.cpp` exercises it; running it here printed `=== all 201 Trifactor checks passed ===`.

**This blocker was Slice 0 — done, under a different module name than this plan guessed.**

---

## 7. Slices

Ordering is by dependency. Each slice is independently testable without a GPU (or with a GPU
headless test harness). The first slice is pure CPU-side data-model work, following the precedent in
CHUNKS.md and LANDSCAPE_EDITOR.md.

### Slice 0 — Offline cluster building and LOD hierarchy — DONE (module `Aver.Trifactor`)

**Corrected 2026-09-02: this slice shipped.** It landed as a new module, `modules/trifactor/`
(`Aver.Trifactor`), rather than as new files inside `modules/formats/` the way the "Work" list below
proposes — see the corrected §2 and B15 above for what was checked. The rest of this slice's
description (goal, work breakdown, done-when criteria) is kept below as the design record of what was
asked for; it is not a live task list any more; `docs/formats/FORMAT_SPECS.md` (5.5-5.7) also now
documents the shipped `LodDesc`/meshlet/error-monotonicity format, including a per-cluster
`OwnError`/`ParentError` pair this plan's original design did not have.

**Goal:** Implement the complete offline cluster-building bake pipeline and prove it via headless
tests, with NO GPU rendering yet.

**Work:**

1. **Integrate meshoptimizer.** Add as a permitted dependency (MIT license). Ensure it builds in the
   MSVC/DXC environment.

2. **Implement `modules/formats/src/ClusterBuilder.cpp`:**
   - `buildClusters(OcMeshData& mesh)` → partition triangles into clusters (~128 triangles each) via
     dual-graph bisection (METIS or hand-rolled). Assign each cluster a bounding sphere and cone
     (cone culling vectors).
   - `buildLodHierarchy(OcMeshData& mesh)` → iteratively simplify and re-cluster. For each LOD
     level: group clusters from LOD k, simplify with boundary edges locked, re-split, compute
     per-cluster screen-space error, verify monotonicity.
   - `writeMeshlets(OcMeshData& mesh, FILE* file)` → pack cluster data into the MLET chunk format
     (OcMesh.hpp line format).

3. **Modify `modules/formats/src/OcMesh.cpp`:**
   - Remove hardcoded `MeshletOffset = 0`, `MeshletCount = 0` (lines 259–260).
   - Set `kOcMeshMeshlets` flag when LOD count > 1 (line 220).
   - Write actual LOD hierarchy data to the MLET chunk.

4. **Modify `tests/formats/src/ConvertTool.cpp`:**
   - After `fmt::importGltf()`, call `fmt::buildClusters()` and `fmt::buildLodHierarchy()`.
   - Before `fmt::saveOcMesh()`, verify all LOD data is populated.

5. **Headless tests:**
   - Parse a test glTF (single cube, single material).
   - Run cluster building.
   - Verify clusters are non-overlapping, cover all triangles, have cones.
   - Verify LOD hierarchy exists, error is monotonic, boundary edges are present.
   - Save to `.ocmesh`, read back, verify round-trip is exact.
   - Do this for two different glTF inputs (simple and complex).

**Done when:** A test loads a glTF, clusters it into N levels with N ≥ 2, saves it as `.ocmesh`,
reads it back, and asserts:
- Every cluster has a bounding cone and sphere.
- `LodDesc[k].ScreenErrorThreshold ≤ LodDesc[k+1].ScreenErrorThreshold` (monotonic).
- The saved `.ocmesh` with `LODCount > 1` and non-zero `MeshletCount` round-trips bit-identically
  on re-load.

**Unblocks:** B15 (cluster-building pipeline), B13 (screen-space error metrics).

---

### Slice 1 — GPU frustum and occlusion culling (compute shaders)

**Goal:** Implement GPU compute passes that cull meshlets against the view frustum and a Hi-Z
buffer from the previous frame, outputting a list of visible meshlet indices.

**Work:**

1. **Frustum culling compute pass:**
   - Read per-meshlet bounding cone from GPU buffer (uploaded from `.ocmesh` MLET chunk).
   - Test cone against view frustum (compute sphere-frustum intersection).
   - Output: per-meshlet visibility flag.

2. **Hi-Z pyramid construction (from previous frame's depth):**
   - Build a mip pyramid of the depth buffer (half-width/height each level, maximum depth).
   - Standard GPU technique; many reference implementations exist.

3. **Hi-Z occlusion culling compute pass:**
   - Read per-meshlet bounding sphere from GPU buffer.
   - Recursively test against Hi-Z pyramid (project sphere, check if any texel in the mip is
     closer than the sphere).
   - Output: per-meshlet occlusion flag.

4. **Visible meshlet list output:**
   - Compute pass outputs a flat buffer of visible meshlet indices (or indirect draw arguments).
   - **This requires B10 (indirect draws).** Implement indirect draws first; if not available,
     output visible meshlet count + index buffer for CPU to iterate over.

5. **Headless tests:**
   - Render a simple scene (cube with LOD2 or LOD3 cluster hierarchy).
   - Run frustum cull; verify no meshlets outside the frustum are marked visible.
   - Run Hi-Z ocull; verify occluded meshlets are unmarked.
   - Render a full frame with the culled meshlet list; verify no visual difference from rendering
     all meshlets.

**Done when:** A headless GPU test renders a clustered mesh, runs frustum and Hi-Z culling, and
asserts that the culled output produces identical pixel output to a full-meshlet render, with
measured reduction in meshlets marked visible outside the frustum (≥50% culling on typical
scenes).

**Unblocks:** B12 (GPU feedback mechanism), B10 (indirect draws).

---

### Slice 2 — Mesh-shader rasterization (Modern path)

**Goal:** Implement GPU-driven meshlet rendering using mesh shaders, outputting a visibility buffer
(cluster ID, material ID, depth per pixel).

**Work:**

1. **Mesh shader amplification.**
   - Compute pass (or draw-indirect) dispatches one mesh shader threadgroup per visible meshlet
     (from slice 1's culling output).
   - Each threadgroup loads its meshlet's vertex/index data from GPU buffers and hardware-rasterizes
     it.

2. **Visibility buffer output.**
   - Replace the deferred G-buffer with a visibility buffer: R32_UINT per pixel storing cluster ID
     (upper 16 bits) + material ID (lower 16 bits), plus a separate depth buffer.
   - **This is a render-target format change.** Requires new format enums in the RHI and updates to
     framebuffer binding.

3. **Post-process shading.**
   - After rasterization, read the visibility buffer and shade each pixel based on its cluster/material
     IDs.
   - For now, use a simple material lookup (one material per cluster). Multi-material support is
     deferred (slice 5).

4. **GPU tests:**
   - Render a simple clustered mesh; verify the visibility buffer is populated.
   - Shade the visibility buffer; verify output is identical to deferred G-buffer rendering of the
     same mesh at LOD0.
   - Render a complex mesh with 5+ LOD levels; verify all levels round-trip without visible seams.

5. **Gate test integration:**
   - If the gate suite has existing probes for deferred rendering, add parallel probes for
     visibility-buffer rendering.
   - **NOT VERIFIED:** whether the gate suite has baseline probes for mesh-shader rendering. Check
     `gates/` directory.

**Done when:** A test scene with a clustered mesh renders via mesh shaders at multiple LOD levels,
produces a valid visibility buffer, shades it to final output, and matches the bit-identical output
of the same scene rendered via deferred G-buffer at LOD0 (up to ULP error in floating-point
arithmetic).

**Unblocks:** Visibility-buffer rendering (Modern path).

---

### Slice 3 — Discrete LOD rendering (Fallback path)

**Goal:** Implement a fallback for hardware without mesh shaders (DX11, older DX12), using
pre-computed discrete LOD levels and CPU-side frustum culling.

**Work:**

1. **Discrete LOD selection.**
   - On Fallback devices, ignore cluster/meshlet granularity.
   - Select LOD level per mesh based on screen-space error (same error metric, same formula as
     Modern path).
   - Render the entire LOD level as one draw call (no meshlet culling).

2. **CPU frustum culling.**
   - Culling happens on the CPU, not GPU. For each visible mesh, compute its screen-space error and
     select LOD level. Output: list of (mesh, LOD) pairs to render.

3. **Compatibility check.**
   - Query `meshShaderTier` capability; if zero, use Fallback path automatically.
   - Fallback produces visible output, albeit coarser LOD. Not an error; expected on older hardware.

4. **Gate tests (if applicable):**
   - Render the same test scene on Fallback path; verify it produces visible output without errors.
   - Visual quality is coarser (fewer clusters per LOD level), but geometry is correct.

**Done when:** A test scene renders identically on both Modern (mesh shaders) and Fallback
(discrete LOD) paths, aside from expected quality differences (coarser LOD on Fallback).

**Unblocks:** B14 (mesh-shader tier fallback), older-hardware support.

---

### Slice 4 — Per-mesh cluster LOD streaming (cluster page loading)

**Goal:** Implement the cluster-page storage and loading infrastructure, allowing the GPU to
request cluster pages reactively and the CPU to load/evict them based on a VRAM budget.

**Work:**

1. **GPU feedback mechanism (close B12).**
   - Implement GPU counter buffer: culling compute pass increments a UAV counter for each accessed
     meshlet cluster; CPU reads the buffer back (3-frame latency).
   - Alternative: indirect dispatch with accessed meshlet list output.
   - Headless test: verify feedback is populated with correct cluster IDs on each frame.

2. **Per-mesh cluster page store.**
   - Extend `.ocmesh` with optional `CPAG` chunk (not Required, for forward compatibility).
   - `CPAG` chunk defines the on-disk layout of cluster pages, separate from the static MLET
     hierarchy.
   - Cluster pages live in a per-mesh file (e.g., `Content/Meshes/{meshId}.ocmesh.pages`), shared
     across all regions/chunks that use the mesh.

3. **LRU eviction policy.**
   - Maintain a per-frame budget of GPU VRAM for cluster pages (separate from chunk entity budget).
   - Track accessed clusters each frame (from GPU feedback); mark as recently-used.
   - Evict least-recently-used clusters when the budget is exceeded.
   - Headless test: load a mesh with 10+ LOD levels, stream cluster pages in/out, verify no
     out-of-memory condition and correct re-loads.

4. **Level integration.**
   - Ensure chunk loading (slice of CHUNKS.md) and cluster loading are independent. A newly-loaded
     chunk does not immediately materialize all its cluster pages; cluster paging is reactive.
   - Test: load a level with two chunks, verify entities spawn correctly, cluster pages load on
     demand.

**Done when:** A headless test loads a level with a mesh spanning two chunks, streams cluster pages
in/out based on GPU feedback and VRAM budget, and asserts no VRAM overrun and no rendered cluster
is evicted from memory without being re-loaded on demand.

**Unblocks:** Reactive cluster paging.

---

### Slice 5 — GPU LOD selection and screen-space error-driven rendering

**Goal:** Implement GPU compute pass for per-cluster LOD selection based on screen-space error,
replacing manual LOD selection.

**Work:**

1. **Screen-space error compute.**
   - Compute pass reads per-meshlet `ScreenErrorThreshold` (from `.ocmesh` MLET chunk).
   - For each visible meshlet, compute its screen-space error (distance-based or depth-buffer-based).
   - Compare to threshold; select LOD level dynamically.
   - Output: per-meshlet LOD selection.

2. **Indirect dispatch with LOD feedback.**
   - Culling output includes LOD level per meshlet.
   - Rasterization (slice 2) uses LOD level to fetch the correct vertex data.

3. **Transition proofs.**
   - Test smooth LOD transitions: render a mesh, move the camera to trigger LOD changes, verify no
     visible cracks or pops.
   - Test with screen-space error metric: verify coarser LODs are selected as distance increases.

**Done when:** A test scene renders a clustered mesh with screen-space error-driven LOD selection,
produces smooth transitions as the camera moves, and shows visible geometry reduction (fewer
triangles) at distance without visible cracks.

**Unblocks:** Fully GPU-driven LOD selection.

---

### Slice 6 — Visibility buffer multi-material shading

**Goal:** Extend visibility-buffer shading to support per-triangle material IDs, not just one
material per cluster.

**Work:**

1. **Auxiliary material lookup.**
   - Store per-triangle material IDs in an auxiliary GPU buffer (uploaded from `.ocmesh` or
     cluster data).
   - After visibility-buffer rasterization, read the auxiliary buffer to get per-pixel material ID.

2. **Multi-material shading.**
   - Extend post-process shading to dispatch different shader code based on material ID.
   - Headless test: render a mesh with mixed materials per cluster; verify each material shades
     correctly.

**Done when:** A test renders a clustered mesh with two or more materials per cluster; the final
output shows each material shaded distinctly.

**Unblocks:** Production-quality material handling.

---

### Slice 7 — Decal and transparency overlays

**Goal:** Composite transparent geometry and decals on top of the visibility buffer (deferred).

**Work:**

1. **Decal buffer.**
   - After visibility-buffer shading, render decals (e.g., bullet holes, scorch marks) into a
     secondary buffer.
   - Composite decals over the shaded visibility-buffer output.

2. **Transparent layer.**
   - Render transparent geometry (glass, particles) with depth test against visibility-buffer depth.
   - Composite front-to-back.

**Done when:** A test scene with decals on a clustered surface and a transparent pane in front show
all layers composited correctly.

**Unblocks:** Full visual fidelity.

---

### Slice 8 — Raytracing integration (BLAS acceleration via cluster bounds)

**Goal:** Use cluster bounding spheres/cones for BLAS acceleration during ray tracing (deferred).

**Work:**

1. **Cluster-based ray-marching acceleration.**
   - When tracing a ray, skip clusters entirely outside the ray's cone.
   - Use cluster bounds (from MLET chunk) to accelerate ray-BLAS intersection.

2. **Headless raytracing test.**
   - Fire a raycast through a clustered mesh; verify it hits the same surface point as a full-mesh
     raycast.

**Done when:** A raycast test on a clustered mesh produces identical hit points and normals to a
full-mesh raycast.

**Unblocks:** Efficient ray tracing on virtualized geometry.

---

### Slice 9 — Integration with LANDSCAPE_EDITOR and content workflow

**Goal:** Wire cluster building into the landscape editor and asset import pipeline.

**Work:**

1. **ConvertTool automatic clustering.**
   - ConvertTool (already used by landscape editor) automatically runs cluster building on import.
   - Authored `.ocland` files (landscape heightfields) can reference `.ocmesh` meshes that are
     already clustered.

2. **Content-browser decoration.**
   - Display LOD count in the content browser for `.ocmesh` files that have been clustered (LODCount
     > 1).

3. **Integration test.**
   - Author a simple landscape, import a clustered mesh into it, render both in-editor and in-game.
   - Verify the clustered mesh renders identically in both contexts.

**Done when:** The landscape editor can reference a clustered `.ocmesh`, and the mesh renders
correctly in both the editor (`Sandbox.exe`) and the runtime (`AverEngineRuntime.exe`) at multiple LOD
levels without visible seams.

---

## 8. What this plan does NOT include

This list is as important as what is in the plan; scope is real only when stated explicitly.

### 8.1 No software triangle rasterization (first pass)

The Modern path uses mesh shaders (hardware rasterization). Software rasterization is deferred
pending measured evidence that it improves performance. The fallback path (discrete LOD, CPU
culling) provides correctness on older hardware without implementing a software rasterizer.

### 8.2 No automatic mesh re-clustering

Meshes are clustered once at import time (ConvertTool.cpp). If an artist edits a mesh in the editor
and re-saves it, clustering is NOT rerun. The edited mesh is saved with its original cluster data
(potentially stale). **This is a known limitation.** Adding incremental re-clustering on-save is a
separate feature, gated on artist feedback and performance profiling.

### 8.3 No skinning or skeletal animation

Skinned geometry (characters) is NOT clustered. Skeletal deformation breaks cluster assumptions
(static geometry, fixed bounds). Skinned meshes continue to use the existing SkinnedScene path.

### 8.4 No global texture streaming or virtual texturing

Cluster paging covers geometric LOD; texture streaming is a separate concern. If virtual texturing
is desired later, it layers on top of cluster rendering as an independent system.

### 8.5 No per-node landscape mesh clustering (landscape editor)

The landscape renderer outputs fixed-vertex meshes (ChunkMesh, 4485 vertices per node regardless of
LOD; LANDSCAPE_EDITOR.md). These are not clustered. Clustering is for imported meshes only.

### 8.6 No automatic material boundary detection

Clusters do not automatically respect material boundaries. A cluster may contain triangles from two
materials. Shading handles this via auxiliary material lookup (slice 6), but the clustering
algorithm does not constrain clusters to match material boundaries.

### 8.7 No backwards-compatible LOD fallback in the format

Old `.ocmesh` files (LODCount=1, no meshlets) are loaded unchanged and render at LOD0. There is no
automatic fallback to older rendering paths; the renderer detects LOD count and dispatches to the
appropriate path. A file with LODCount=1 and no MLET chunk is valid and renders correctly; it is
just not virtualized.

### 8.8 No offline shader compilation for mesh shaders

Shaders are compiled at runtime (D3D12Device.cpp). Mesh shader shaders are compiled alongside other
shader types. No offline shader compiler or shader cache is added. **Pre-compiled shader binaries are
already in the pipeline; extending that is a separate optimization, not part of this plan.**

---

## 9. Summary and timeline

This plan proposes a staged implementation of Nanite-class virtualized geometry for the Aver Engine,
starting with offline cluster building (slice 0, ~2 weeks) and culminating in fully GPU-driven
rendering with reactive cluster paging (slice 4, ~6 weeks total). Slices are independently testable
and can be parallelized if resources permit.

**Corrected 2026-09-02: Slice 0 has shipped** (as `Aver.Trifactor`, see §7 and B15 above), and a
GPU-driven per-cluster mesh-shader rendering path with a hardware-tier fallback also exists in
`sandbox/src/SandboxApp.cpp` (`"[LOD-MESH-SHADER]"`, see the status banner at the top of this
document) that was not built by following slices 1-3 as numbered here. This section's slice-by-slice
timeline and "critical path" below describe the plan as proposed on 2026-08-08, not the order or
shape of what was actually built; do not read "slice 1 → 2 → 4" as a status report.

**Total estimated effort:** 8–10 weeks for slices 0–5 (core pipeline). Slices 6–9 (multi-material,
transparency, raytracing integration, content workflow) add another 4–6 weeks.

**Critical path:** Slices 0 → 1 → 2 → 4. Slices 3, 5, 6 can run in parallel with earlier slices
once their dependencies are met.

**Licensing gate:** Before shipping, conduct a one-time legal review of the implementation against
published patents (especially DDGI). Use permissive libraries only (meshoptimizer, Brixelizer). Do
not ship DDGI without explicit legal clearance. This is a hard gate, not a soft checklist.

**Test infrastructure:** Five existing gate suites (18 probes × 9 configurations, CHUNKS.md §10).
Cluster rendering will need new probes to avoid baseline re-recording. Coordinate with whoever owns
gate maintenance before adding new rendering paths.

