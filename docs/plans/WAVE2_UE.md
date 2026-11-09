# UE5 parity, wave 2: plan (owner-approved scope 2026-11-09)

**Owner's scope (2026-11-09):**
- Most of the remaining UE features, starting with physics-driven hit reactions.
- Dynamic HLOD.
- A Trifactor rework.
- PCG graphs and PCG vegetation.
- Basic modelling in the editor.
- The queued renderer items.

**Interim defaults while the owner was asleep.** These were taken from the plan's recommendations, and the owner can override any of them:
- **D1:** Voronoi destruction is brittle rigid shatter, alongside the soft-body dents and tears. It is scheduled last among the CPU tracks.
- **D2:** HiZ ships off by default, and shadows use a clipmap, not VSM. No patent-gated GPU work happens overnight.
- **D3:** HLOD is auto-built, cached and swapped at runtime, as in section W2.
- **D4:** The PCG evaluator lives in `modules/world`.
- **D5:** The `.ocgraph` profile is used if it supports typed data flow. Otherwise `.ocpcg` uses AVR1.
- **D6:** The C# Geometry Script is deferred.
- **D7:** Shadows are fixed Vulkan cascades plus a cached clipmap.
- **D8:**
  - Cluster paging is design and CPU planner only.
  - Builder v3 is allowed: it marks meshes stale and never touches the sources.
  - Skinned meshes stay on discrete LOD.
- **D9:** No new dependencies.

**Overnight runs:**
- Phase A (headless) only, one agent at a time.
- Editor UI is written and compile-checked only.
- New renderer paths are off by default.
- Nothing runs headed.


Read-only survey of `C:/Users/User/AverBuilds/aver-merge` (branch head `66022228`). Nothing was built, run or edited.
Method: read `UE5_GAPS.md`, `UE_FEATURE_PARITY_PLAN.md`, `wave4/{VIRTUALIZED_GEOMETRY,TEXTURE_STREAMING}.md`, `PCG.md`,
`LEVEL_STREAMING.md`, `wave9/SOFTBODY_CLOTH_DESTRUCTION.md`, the Trifactor headers/sources, `Csg.hpp`, the ragdoll module + ABI,
`OcFoliage.hpp`, `ChunkGenerator.hpp`, `VulkanDevice.cpp` shadow-scope comment, `SandboxApp.hpp` modes. Spot-checked file:line claims
that are marked (V). Claims copied from the wave-4 design docs without re-checking are marked (D). Anything I could not check is marked (?).

Legend. Size: S = hours, M = a day, L = several days. **H** = HEADLESS-TESTABLE (CPU/format/editor-model logic with unit tests, no GPU,
no window). **G** = NEEDS-GPU/HEADED inspection (owner or a headed run; subject to the standing "no run likely to hang the GPU" rule, ask each time).

## 0. Findings that change the scope

1. **UE5_GAPS.md is wrong on physical animation.** "Missing" is not true: `modules/ragdoll` has `RagdollSystem` with Kinematic / Limp / Powered
   modes, a blend weight that ramps in and out, `start/stop/setStrength/addImpulse`, chained into AnimSystem's pose-modifier seam
   (`modules/ragdoll/include/aver/ragdoll/RagdollSystem.hpp` (V)), the Jolt ABI `physics_ragdoll_abi.h` (powered = motor per joint toward the target pose (V)),
   and a C# wrapper `scripting/csharp/Aver.Framework/Ragdoll.cs`. What is missing is the *UE physical-animation layer on top* (per-region strength, hit-reaction envelopes, partial-body simulation, recovery). Workstream W6 is sized for that, not for a ragdoll.
2. **Owner decision conflict on destruction.** `wave9/SOFTBODY_CLOTH_DESTRUCTION.md:3` records an owner decision (2026-10-17) that destruction uses soft bodies and
   "supersedes the Voronoi pre-fracture reading". The new request asks for Voronoi again. They are compatible only if scoped (soft = dents/tears/cloth,
   Voronoi = brittle rigid shatter). Decision D1 in section 3. `modules/fracture` no longer exists (not in `modules/`), `modules/softbody{,.scene}` do.
3. **The Trifactor virtualization design is complete but zero GPU phases are built.** `VIRTUALIZED_GEOMETRY.md` (327 lines, P0-P4) is frozen; no `ClusterRenderer` exists
   (grep over `modules`, `sandbox`, `Runtime` finds none). Its premise "the RHI has no indirect draw or dispatch" is now **stale**: `DeviceCaps::indirect`
   (`RHI.hpp:111-112` (V)), `IRenderContext::drawIndexedIndirect/dispatchIndirect` (`RHIResources.hpp:709-711` (V)) and a Vulkan `dispatchIndirect`
   (`VulkanRenderContext.cpp:753` (V)) landed with the GPU-particle tier. Mesh/amplification-shader indirect is still absent (?). P1 shrinks accordingly.
4. **Scatter-from-paint-layer already exists** (`SandboxApp::scatterFoliageFromLayer`, `sandbox/src/LandscapePaintTool.cpp:250,267` (V)) and there is a procedural chunk
   scatter (`world::GeneratedChunkSource`, palette + density bands + height source). PCG vegetation upgrades these; it does not start from zero.
5. **No HLOD, no data layers anywhere.** `grep -i hlod` in `modules`, `sandbox`, `Runtime` finds nothing; `OcWorld.hpp` has no layer concept for placements (V; the only "layer" hits are cloud and fluid-material layers).
   `trifactor::simplifyMesh(OcMeshData&, ratio)` (`ClusterBuilder.hpp:467` (V)) is submesh-aware and is the HLOD simplifier.
6. **Vulkan cascades are still unwritten**, with the exact cause and the real fix documented at `VulkanDevice.cpp:3395-3411` (V) and `docs/BUGS.md:360`.

---

## W1. Trifactor rework (the Nanite path)

### W1.1 Diagnosis (what is wrong or limited today)

| # | Area | Finding and evidence | Severity |
|---|---|---|---|
| 1 | Reach | The GPU cluster path is **editor-only and D3D12-only**: one `dispatchMeshClusters` per entity (`sandbox/src/SandboxRender.cpp:1050-1195` (D)), pipeline built in `SandboxAssets.cpp:295-440` (D), gated at `SandboxApp.cpp:624-655` (D), behind `--lod-mesh-shader`. The packaged game has no cluster rendering at all. | High |
| 2 | Shading | Clusters use a private pixel shader `sandbox/shaders/cluster_material.hlsl` whose header says "no ray tracing, ever" (`ClusterMaterialShader.hpp` (D)): no G-buffer, no velocity, no RT shadows/reflections, no denoiser inputs. So a clustered mesh shades differently from the same mesh on the normal path. | High |
| 3 | Selection cost | The CPU cut `selectClusterCut` is a flat scan of every cluster of every level: 129,666 for a re-cooked `pine_tree_01` and ~1.9 s/frame CPU on Electric Dreams in `--lod-per-cluster` (`ClusterAdapt.hpp:279-285` (V)). The GPU twin is one AS thread per cluster of every level, per entity, again flat (D). No hierarchy traversal, no instance culling, no batching. | High |
| 4 | Correctness (crack-freedom) | The local cut `inLocalCut` evaluates each cluster at **its own** bounding sphere, so a child and its parent can disagree and open a hole. The header itself says "THIS IS A REAL GAP" and that sphere containment across a DAG edge is **not a checked invariant** (`ClusterSelect.hpp:88-104` (V)). Tests only use synthetic DAGs built with containment. | High |
| 5 | Materials | Clusters are built over the merged index buffer and straddle submeshes; `SubmeshRange.MeshletStart/Count` are written 0,0 (`modules/trifactor/README.md` "What's here, continued" (V)). A multi-material mesh therefore cannot be drawn per material from clusters; the GPU path "draws one material per mesh" (D). | High |
| 6 | Vulkan parity | No cluster path on Vulkan. Voxi's mesh-shader scene pipelines are refused there (commit `449140a2`, refusal site not located even by the design author, `VIRTUALIZED_GEOMETRY.md` R4). | High |
| 7 | Occlusion | No per-cluster HiZ. The only culler (`Aver.Occlusion`) is CPU-driven, one frame stale, per entity, defaults to `waitIdle`, is linked into Sandbox only, and `occlusion_test.hlsl:29-39` skips behind-camera corners so a near-plane-straddling box can be falsely culled (D). Packaged game: no occlusion culling. | Med |
| 8 | Memory / streaming | `.ocmesh` stores every LOD level's indices + meshlets for the whole DAG (all resident); meshlet buffers live in **Upload** memory read over PCIe (`SandboxAssets.cpp:240-247` (D)). No cluster paging, so the DAG cannot exceed VRAM and nothing streams from disk. | Med |
| 9 | Ray tracing | TLAS builds BLAS from the full LOD 0 (`VoxiRenderer.cpp:1756-1759` (D)); shadow/GI use a 20 cm-error proxy mesh (`GameContent.cpp:419-445` (D)). A virtualized mesh costs full-detail BLAS memory. | Med |
| 10 | Builder quality | Grouping is a greedy region-grow (`groupClusters`, `ClusterBuilder.cpp` header notes, `kTargetGroupSize = 6`, `kMaxGroupSize = 8`, `:318-326` (V)), because METIS is forbidden by licence. Group boundaries are therefore irregular, boundary edges are locked on every level, and the lock-border flag is special-cased per "small shell" lineage (`ClusterBuilder.cpp:14-36,180-190` (V)). No measured edge-cut or coarsest-level triangle-count comparison exists in the repo (?). Complexity is already high: 1,695 lines in one file. | Med |
| 11 | Exclusions | Skinned, morph, blended, vertex-animated meshes are excluded (D). Foliage wind that offsets vertices would desync cluster bounds (R11 in the design). | Med |
| 12 | Tooling | `RelodTool` exists (imports log per-mesh clustering, commit `d2ebb9dd`) but there is no viewer: no cluster-colour / LOD-level / cull-stat debug view, no per-mesh cook report in the editor, no builder-version stale warning surfaced in the Content Browser (?). `kBuilderVersion = 2` (V) is stamped but nothing prompts a re-cook. | Low |
| 13 | Build time | Per-group `meshopt_simplify` setup was ~98% of time until `SimplifySparse` (`ClusterBuilder.cpp:17-23` (V)). I found no current build-time numbers per MTri (?); measure before and after any builder change. | Unknown |

### W1.2 Target and approach

Execute the frozen `VIRTUALIZED_GEOMETRY.md` design, **re-sequenced** so everything CPU-side lands and is tested first, and with three additions: (a) a better partitioner,
(b) a cluster page/stream layer (design only in wave 2), (c) a debug/report tool. Decisions already frozen there that I keep: group-shared LOD spheres derived at load,
material-pure clusters (builder v3 / MLET v4), compute-culled indirect VS-pull back end as the portable default (Vulkan too), mesh-shader as an optional faster back end,
two-pass HiZ behind `geom.hiz`, fixed-LOD `rtProxy`, reuse of `PSMainVoxi` (G-buffer + RT) via cluster front ends, default-on only when caps allow, delete the legacy path last. No software rasterizer (patent and scope).

### W1.3 Steps

1. **T-CPU-1 Group LOD spheres.** `trifactor::deriveLodSpheres`, `validateLodSpheres`, and a *coverage property test* (every cluster's `lodSphere` contains its members' bounds and the producing group's sphere; parent sphere >= child at every DAG edge). Fixes finding 4 for existing chunk-v3 files, no format change. **S, H.**
2. **T-CPU-2 Cut oracle.** Rewrite `inLocalCut`/`selectClusterCut` on shared group spheres; add a property test "for random cameras the cut is a valid antichain cut: every LOD-0 triangle is covered exactly once, no pair of selected clusters is ancestor/descendant" (this *proves* crack-freedom for the test corpus instead of asserting it). Becomes the oracle the GPU kernel is checked against. **M, H.**
3. **T-CPU-3 Material-pure cook.** `buildClusters` per submesh range, groups only inside a material slot, `u16 materialSlot` per meshlet (MLET v4, `kBuilderVersion = 3`), fills `SubmeshRange.MeshletStart/Count` for real. Round-trip + `validateLodDag` + "no cluster spans two slots" tests; a multi-material fixture. Keep v3 readers working (absent-vs-zero rule, `FORMAT_SPECS.md` 3.2). **M, H.**
4. **T-CPU-4 Partitioner upgrade (clean room, no new dependency).** Replace greedy region-grow with recursive spatial bisection of the cluster adjacency graph (Morton/longest-axis split to target 4-8 clusters, then Kernighan-Lin/FM boundary refinement on shared-edge weight). Gate: measured *locked-boundary edge count* and *coarsest-level triangle count* on the existing five protected meshes + corpus must not regress versus v2 (the v1 commit used the same method: "corpus coarsest 1,195,431 -> 140,489"). Ship only if better; otherwise keep v2 and record the numbers. Bump `kBuilderVersion` only with output change. **L, H.**
5. **T-CPU-5 Report/validation tool.** Extend `RelodTool` with per-mesh table (levels, clusters, tris per level, locked-edge ratio, seam cost, eligibility `virtualizable()` and *why not*), plus a stale-builder warning in the Content Browser details. **S, H** (the editor surface itself is G-light).
6. **T-GPU-1 RHI remainder.** Indirect mesh-shader/amplification dispatch (D3D12 + Vulkan `vkCmdDrawMeshTasksIndirectEXT`) only if the mesh back end is kept; resource-state/buffer flag already exist (V). Reusable `HizPyramid` extracted from `Aver.Occlusion`, previous-frame pyramid kept. `IndirectSmokeTest` extended (compute writes args -> indirect dispatch fills UAV -> indirect draw draws a known triangle). **M, G** (D3D12 first, then Vulkan).
7. **T-GPU-2 `ClusterRenderer` compute back end, no HiZ** (P2 of the design): `ClusterPool` in Default memory, instance table, instance cull -> cluster cull -> bin -> VS-pull indirect draw into the cluster twins of `PSMainVoxi` (`scenePso_`, `sceneGbufPso_`, `sceneRtPso_`, `sceneRtGbufPso_`), editor + packaged-game wiring, discrete-LOD fallback, `[GEOM]` stats, set-equality selftest vs the T-CPU-2 oracle (compare GPU-selected cluster set with CPU set on a fixed camera, readback only), pixel parity vs discrete LOD. Done = foliage scene draws via clusters on D3D12 **and** Vulkan with G-buffer and RT on, no black pixels. **L, G.**
8. **T-GPU-3 HiZ two-pass + mesh back end** (P3): passes 5-8, `geom.hiz` default **off until the zero-diff A/B in motion passes** and the owner decides the patent question (D2). Fix `occlusion_test.hlsl` near-plane corner skip separately (changes the existing `--occlusion-cull` image, flag it). **L, G.**
9. **T-GPU-4 RT proxy, deletions** (P4): `rtProxy` BLAS, remove `--lod-per-cluster`, `clusterCutCache_`, `AVER_MS_CLUSTER` block, `cluster_material.hlsl`, `dispatchMeshClusters` once unreferenced. **M, G** (build + existing suites are H; image diff is G).
10. **T-GPU-5 Cluster paging (design + CPU half only in wave 2).** Page = group of clusters + their vertex/triangle slices; `.ocmesh` v-next stores pages with a root page always resident; CPU residency planner by projected-error demand; GPU side deferred to wave 3. Rationale: finding 8 limits mesh size, but it is not on the critical path for a shooter. **M (CPU planner + format, H); GPU deferred.**

Risks specific to the owner's setup: all GPU steps need a headed Vulkan *and* D3D12 run; instance/cluster cull kernels must have bounded loops and be tried on a small scene first (standing no-TDR rule).

**Status 2026-11-09 (W1, CPU steps).**
- Done: T-CPU-1 (`deriveLodSpheres`/`validateLodSpheres`, `ClusterAdapt.hpp`; no format change), T-CPU-2 (`ClusterView::lodCenter/lodRadius`, `buildClusterDagViews`, `selectCutIds`; the cut now measures error at the producing group's shared sphere, and error 0 never hits the near-camera sentinel), T-CPU-3 (builder v3, MLET v4, real `SubmeshRange` meshlet spans).
- Proof: `ClusterCutOracleTest` over random cameras finds no overlap, hole or split group with shared spheres; the old per-cluster-sphere rule gives hundreds of overlaps and split groups on the same meshes. Note the plan's "every LOD-0 triangle covered exactly once" is checked at cluster/group granularity (a re-split group's outputs are all parents of every member, so descendant counting cannot say "once").
- Not done: the legacy `inLocalCut`/`MeshClusterView` path is unchanged because `MeshClusterView` is uploaded raw to the D3D12 amplification shader (a layout change needs the shader and a headed run); it is replaced when T-GPU-2 lands. No GPU, sandbox or Vulkan wiring. T-CPU-4..5 and T-GPU-1..5 untouched.
- Measured (Debug, `ClusterBuildBench`): one-material fixtures are cluster-for-cluster identical to v2 with the same build time (grid 44,402 tris: 461 L0 clusters, 1,088 total, coarsest 606, ~160 ms). Multi-material meshes get a slightly deeper-locked ladder because material borders are now locked (4-stripe grid coarsest 606 -> 1,490 tris, 3-band sphere 952 -> 1,704; build time unchanged). The five named corpus meshes (dead_tree_trunk, rock_07/_09, rock_moss_set_02) are not on this machine, so the protected-mesh comparison is not measured; they are one-material, so v3 should leave them unchanged.
- Existing cooked meshes read builder version 2 and show as stale until re-cooked; they keep loading.

**Status 2026-11-09 (W1, T-CPU-5, T-CPU-4, T-GPU-5 CPU half).**
- Done, T-CPU-5: `MeshReport` (`modules/trifactor`): `classifyBuilder`/`stalenessWarning` (Current, Stale, Unknown, Newer), `virtualizable()` with the first reason it fails, `measureLodDag` (per level clusters, groups, triangles, welded edges, locked group-seam edges, open edges). `RelodTool --report` prints them per mesh. Test `MeshReportTest`. The Content Browser details panel is NOT wired: the editor surface can call `stalenessWarning(md.builderVersion)` and `virtualizable(md)`, nothing in sandbox does yet.
- Done, T-CPU-4 (not shipped, by the gate): `ClusterPartition` is a clean-room recursive spatial bisection with FM boundary refinement, behind `setGroupPartitioner(GroupPartitioner::Bisect)`; the default stays meshopt_partitionClusters (v2 grouping), builder version unchanged. Plan finding 10 was off: the grouper is already `meshopt_partitionClusters`, not a greedy region-grow. Measured with `ClusterBuildBench <dir>` (Debug, LOD 0 of every virtualization candidate: static, >= 2000 tris, from the owner's project folders, 242 meshes with both runs, target group size 6): locked group-seam edges summed over levels meshopt 10,728,114 vs bisect 12,463,872 (+16.2%, worse on 202 of 242 meshes); summed coarsest-level triangles 17,538,873 vs 17,340,870 (-1.1%, worse on 99); build time +11%. With target 8: on 1,228 meshes (partial run) locked +16.6%, coarsest -2.4%. Both fail "locked-boundary edges must not regress", so v2 stays. Fixtures and small meshes are roughly a wash (locked -3% to +20%). `ClusterPartitionTest` checks sizes 4..8, coverage, cut below a scanline baseline and a valid DAG; the code is kept for a later tune (a better initial split or gain model is the obvious lever).
- Done, T-GPU-5 CPU half: `ClusterPages` (`buildPagePlan`, `validatePagePlan`, `computePageDemand`, `planResidency`) and the design in `docs/plans/CLUSTER_PAGES.md`. Tests `ClusterPagesTest`: closure (a resident page always has its parent pages resident) over 60 random views, budget respected, roots always resident, hysteresis. No file writer or reader and no GPU side.
- Not verified: nothing here ran on a GPU or in the editor; the five named protected meshes are not on this machine (the comparison used whatever .ocmesh files exist under the owner's project folders); Release timings.
- Remaining in W1: T-GPU-1..4, the paged-section writer/reader, and the sandbox surface for the report.


---

## W2. Dynamic HLOD

**Interpretation (confirm, D3):** "dynamic" = HLOD proxies are generated automatically from the level (editor-time background bake, content-hash cached, regenerated when a cell's content changes) and swapped at runtime by distance, *and* the same builder can run at runtime for procedurally generated chunks. Not a per-frame mesh merge.

**Current state.** Distance-cell streaming exists: `world::PlacementStreamer` (`modules/world/include/aver/world/PlacementStreamer.hpp`), `STREAM cell= load= evict=` record, `.ocstream` bake with per-placement bounds and cell-sorted foliage tables, `game::loadLevelFoliage` residency, fog hides the load edge (`LEVEL_STREAMING.md` sections 1, 2, 4, 4b (V)). Beyond `loadCm` the world is simply absent (fog). No proxy meshes, no hierarchy.

**Target.** Per streaming cell, an HLOD1 proxy (merged + simplified static meshes of that cell); optional HLOD2 per block of NxN cells; proxies resident in a wider ring (`hlodCm` > `loadCm`) and replaced by the real cell content as it streams in, with hysteresis (no cross-fade in v1; dithered fade is a later G item).

**Steps.**
1. **Data model.** `.ocstream` gains `HLOD <cell> <level> <proxy .ocmesh> <sourceHash>` records; `STREAM` gets `hlod=<cm>`; `OcWorldData` additive fields. Round-trip + absent-vs-zero tests. **S, H.**
2. **Merger.** `fmt::mergeStaticMeshes(placements, meshLoader) -> OcMeshData`: bake each placement's transform into vertices, group by material slot into submeshes (v1 keeps source materials; no atlas), drop tiny and fully-enclosed instances optionally (bounds-contained test), vertex-weld. Tests: triangle/volume conservation, submesh grouping, determinism. **M, H.**
3. **Simplifier + budget.** Call `trifactor::simplifyMesh` to a per-cell triangle budget (target ratio from cell triangle count and screen-size at `hlodCm`; error cap in cm). Reuse (it is submesh-aware, commit `7a7692ff`). Tests: budget respected, material slots preserved, no NaN, watertight-ness not required. **S, H.**
4. **Foliage handling.** Foliage tables (`.ocinst`) are millions of instances; HLOD for them = (a) coarsest Trifactor LOD of each *type* merged for the N largest instances in the cell, (b) the rest dropped, (c) ground-cover omitted. Impostor billboards = v2 (flag, D3). **M, H** (merger side), proxy look is G.
5. **Bake tool + cache.** `AverAssetC hlod` and World Settings > Level Streaming > Build HLOD; proxy files under `Binaries/Streaming/<level>/hlod/`; staleness by `hashPlacements` of the cell's placement ids + mesh hashes (the `SOURCE` mechanism already exists for the whole level, extend per cell). Background thread in the editor (the stutter work shows main-thread bakes are noticeable, commit `b26f2996`). **M, H** (CLI + hashing), editor UI minimal.
6. **Runtime ring.** `PlacementStreamer` gains a second residency radius; `LevelStreaming`/`GameStreaming` loads a cell's proxy when the cell is in the HLOD ring and not resident, hides it once the cell's real content is resident (swap on the frame the cell becomes ready, mirroring the existing "staged change applies only once all of it is ready" rule). Residency decisions are pure -> **H** (planner tests: ring transitions, hysteresis, no flicker over a camera walk); drawing proxies and BLAS policy (no BLAS for proxies, they must not occlude RT shadows differently) is **G**. **M.**
7. **Runtime generation for PCG chunks.** After W3/W4 chunk generation, build the proxy on the generation worker with the same merger (cap time per chunk). **M, H** (merger) + G.
8. **Editor view.** HLOD debug view (colour by level), per-cell triangle count in the streaming panel. **S, G-light.**

Dependency: W2 steps 1-3,5 are independent of everything else. Overlap with W10 (data layers): both edit `OcWorld.*` and the streamer; serialize their format steps.

**Status 2026-11-09 (W2 steps 1-3, 5, 6 planner, 7 merger; headless only, nothing drawn).**
- Done and tested: (1) `STREAM hlod=` and `HLOD` records in the `.ocstream` (`OcWorldDataLayersTest`); (2) `fmt::mergeStaticMeshes` (`OcMeshMergeTest`); (3) `trifactor::simplifyToBudget` plus `fmt::hlodTargetTriangles`
  (screen-area heuristic; the simplifier exposes no error cap in cm, so there is none); (5) `fmt::bakeHlod` and `AverAssetC hlod` with per-cell content-hash staleness (`OcHlodBakeTest`, `AverAssetCTest`);
  (6) `PlacementStreamer::buildHlod/updateHlod`, ring with hysteresis, walked in `DataLayersHlodTest`; (7) `world::buildChunkProxy` with item and time caps (`ChunkProxyTest`).
- Not done: (4) foliage handling; (6) the `LevelStreaming`/`GameStreaming` consumer, drawing proxies, and the no-BLAS policy for them (the renderer side must keep proxies out of RT by default);
  (7) calling `buildChunkProxy` from the chunk generation path (generation is synchronous on the frame thread today, there is no worker); (8) editor view; the Build HLOD button, a background bake thread
  in the editor (the bake takes `cancel` and `progress` callbacks for one), HLOD level 2 blocks; any run of the engine.

**Status 2026-11-09 (W2 step 5 editor half, compile-checked and model-tested only).**
- Done: World Settings > Level Streaming > HLOD: distance slider, **Build HLOD** (saves the level, then runs `fmt::bakeHlod` on a background thread with progress and Cancel, "Rebuild all" option), a summary line; the `STREAM hlod=` distance is applied when it finishes. `HlodBuildJob` (job, prerequisites, summary) is tested with a stand-in bake (`HlodBuildJobTest`).
- Not done: the HLOD debug view (colour by level), per-cell triangle counts in the panel, level 2 blocks. Never run in the editor; the real bake was not run from the panel.

---

## W3. PCG graph system and editor

**Current state.** (a) F# `Aver.Pcg` (`scripting/fsharp/Aver.Pcg/{Types,Primitives,Pcg}.fs`): `scatter` (uniform in bounds, MinSpacing, deterministic splitmix streams), `sampleInfinite`, `densityVolume`; C# `Aver.Pcg.Apply` spawns actors (`docs/PCG.md` sections 3-5 (V)). (b) C++ `render.pcg` GPU density volume (389 lines, "contract + CPU reference, no consumer" for bounded volumes). (c) `world::GeneratedChunkSource` (palette, density bands, height source) for chunk streaming. (d) `PCGVOLUME` level records exist but only the Sky volume is read by name (`PCG.md` section 4). There is **no graph, no point data type, no editor, no way to author a rule without writing F#**.

**Target.** A UE-PCG-shaped framework: point data with attributes -> samplers -> filters/transforms -> spawners, authored as a node graph asset, executed deterministically by one evaluator used by the editor (preview/bake), the chunk generator (runtime) and C# (`Pcg.Run(graph, seed, bounds)`).

**Design decisions (recommendation).**
- **One C++ evaluator** (so the editor, the streamer and tests share it and F# stays an authoring language for *rules-as-code*, not the only path). It must reproduce `Aver.Pcg.Rand` splitmix/`hash2`/`hash3` bit-for-bit, and a cross-language test (F# vs C++ same seed, same points) keeps the determinism contract in `PCG.md` section 2.
- **Where it lives:** CPU-only files in `modules/world` next to `ChunkGenerator` (RHI-free) or a new CPU module split from `render.pcg`. Module DAG impact not checked (?); decision D4. Not a new third-party dependency either way.
- **Asset format:** `.ocpcg`, either an AVR1 container (like `.ocfoliage`) or a profile of the existing `.ocgraph` container so the existing node-graph canvas (`GraphEditorGeometry` layout/hit-test/link rules, `GraphNodeDefs` descriptor table, clipboard, collapse, find, problems) is reused. I did not verify that `OcGraph` supports data-flow (non-exec) pins with typed edges; material graphs (58 nodes) suggest yes (?). Step 1 settles this. Decision D5.
- **Point data:** SoA `{pos, rot, scale, density, seed, color, steepness, bounds} + named f32/vec3/int attributes`, all in cm, +Z up, left-handed (the engine contract in `PCG.md` section 1).

**Steps.**
1. **Spike/decision (S, H):** confirm `.ocgraph` can carry typed data-flow nodes; otherwise write the AVR1 `.ocpcg` reader/writer with round-trip tests.
2. **Core types + determinism (M, H):** `PointData`, attribute tables, `Rand` port, spatial hash grid. Tests: bit-identical vs F# golden vectors (generated once from F# and committed as a table), order independence.
3. **Evaluator (M, H):** topological execution, per-node cache keyed by (node params, input hash, seed), cancellation, a time budget per call, error list. Nodes: *Create Points Grid*, *Surface Sampler* (heightfield/landscape via the existing `heightSource` callback; mesh surface sampling later), *Spline Sampler* (reuses river/lake spline code, `WaterSplineTool` (V exists)), *Volume Sampler*, *Bounds Filter*, *Density Noise* (reuses `TerrainNoise`/`sampleInfinite`), *Density Filter/Remap*, *Attribute Filter/Math/Set*, *Transform Points* (jitter, scale/rotate ranges), *Self Pruning* (priority-based radius prune, see W4), *Difference/Union/Intersect*, *Project to Surface*, *Slope/Height/Layer-Weight Filter* (landscape paint weights), *Distance to Spline/Actor Filter*, *Weighted Static Mesh Spawner* (outputs instance list), *Actor Spawner*, *Subgraph*, *Loop*. Each node is a pure function with its own unit test. Landscape-dependent nodes take an interface (`ILandscapeQuery`) so tests use a fake heightfield. **L.**
4. **Output sinks (M, H):** (a) instance table `.ocinst` / level `FOLIAGE` records (bake, cell-sorted via the existing `cellOcInstances`), (b) `ChunkPayload` for `GeneratedChunkSource` (runtime chunk mode, replaces the hard-coded palette generator while keeping `generatorVersion` semantics so existing generated worlds stay stable, `generatorVersion = 2` today (V)), (c) actor spawn via `Aver.Pcg.Apply`. Determinism + "regenerate equals persisted" tests (`persistChunk` already has the compare logic (V)). 
5. **PCG volume component (M, H model / G viewport):** a `PCGVOLUME` record gains `graph=<.ocpcg> seed= bounds=` (generalizing the Sky special case; keep Sky working). Generate-on-edit with a "Generate / Clean up" button, persistent vs transient result flag, partitioned generation per streaming cell (grid size = streaming cell) so editing a graph regenerates only dirty cells. 
6. **Graph editor (L, G):** `PcgGraphEditor` as an `AssetEditor` registered after the existing ones (the hook pattern at `GraphEditor.hpp:5-20` (V): `registerFactory`), node palette with search, typed pin colours, per-node inspect (point count, attribute table, bounds), node *debug* toggle drawing the node's output points in the viewport as debug spheres (existing debug draw (?)), error/warning badges, UE editor style per `EDITOR_UE_PARITY.md` (grey + orange, Material Icons). Model/validation/undo (`SnapshotUndo<State>`) are pure and **H**; canvas drawing is **G**.
7. **GPU density path hook (M, G):** a *GPU Density* node that calls the existing `render.pcg` compute volume for 3D noise (clouds/caves) and reads back; keep optional; CPU is the reference (the `sampleDensity` contract in `PCG.md` section 5).
8. **Docs + sample + F# interop (S, H):** `docs/PCG.md` rewrite; F# helper to run a graph; a sample graph asset in the templates.

**Not in v1:** hierarchical generation (grids of grids), runtime GPU point processing, PCG on arbitrary mesh surfaces beyond heightfield/spline/volume.

**Status 2026-11-09 (W3 steps 1-5 and 8, headless; `docs/PCG.md` section 7).**
- Done, step 1 (D5): the `.ocgraph` container carries typed data flow as is (pin type strings, `key=value` node tokens, `DOMAIN`), so a PCG graph is `DOMAIN pcg` (`OcGraphDomain::Pcg`); no AVR1 `.ocpcg`.
- Done, step 2: `PcgPoints` (SoA plus named f32/int/vec3 attributes), `pcgHash*`/`pcgFloat01` and `Pcg.scatter` (`pcgScatterReference`) ported bit for bit and compared with golden vectors generated from the F# module (`tests/world/data/pcg_golden.fsx`), spatial hash grid. Order independence is covered by the partition tests.
- Done, step 3 (`modules/world`, D4): topological evaluator with per-node cache, cancel flag, time budget, diagnostics, validation, Subgraph and Loop; 29 nodes (the planned set minus mesh-surface sampling), each unit-tested, landscape through an `ILandscapeQuery` fake. Not done: the GPU Density node (step 7) and a node-catalog entry (these are not script-graph nodes).
- Done, step 4: `.ocinst` table sink (cell-sorted through `cellOcInstances`), chunk payload sink wired into `GeneratedChunkSource` via `GeneratorSettings::pcgProgram` (palette path byte-identical when unset; graph worlds record `pcgGeneratorVersion` so stale saves are detected; `persistChunk` regenerate-equals-persisted tested), actor spawn records.
- Done, step 5 (model, headless): `PCGVOLUME ... graph <path> [transient]`, `PcgCellGenerator` (each point owned by exactly one cell, nine cells equal a whole run with local pruning), `PcgCellStore` dirty-cell tracking (graph edit or environment stamp makes cells stale, `invalidateBox` marks only the reachable cells), wired into `GameStreaming::enable` (shared by the editor and the game). The `Sky` volume and graph-less volumes are untouched.
- Done, step 8 (partly): `docs/PCG.md` section 7, sample graph `templates/FirstPerson/Content/PCG/PG_ForestScatter.ocgraph` (loaded and run by a test), CHANGELOG entry.
- Not done: step 6 (editor canvas, palette, inspect, viewport debug draw, Generate / Clean up button), step 7, F#/C# `Pcg.Run` and the `Aver.Pcg.Apply` bridge for actor spawn records, the F# graph-run helper from step 8, mesh-surface sampling.
- Not verified: nothing ran in the editor or the game; the `GameStreaming` graph hook compiles but no level has been streamed through it. Greedy `SelfPrune` across cell seams is exact only in `mode=local` (greedy uses a three-radius margin and passed the seam-spacing test, but is not proven equal to a whole run). Positions are world-space f32. Test-level only: the per-node cache keys on region, so it helps re-runs of the same cell, not neighbouring cells.

**Status 2026-11-09 (W3 step 6, compile-checked and model-tested only).**
- Done: the PCG Graph tab (`PcgGraphEditor`, `sandbox/src/PcgGraphEditor.*`, `PcgEditModel.hpp`). It claims only `DOMAIN pcg` files (registered before the general graph editor), keeps the original text so an untouched save is byte-identical, and has: node palette with search and categories (from `pcgNodeDefs()`), a canvas on `GraphEditorGeometry` (pan, zoom, drag nodes, drag pin to pin, typed pin colours, point-count and problem badges), undo/redo (`SnapshotUndo`), an inspector with each node's parameters (schema defaults, leftover tokens kept), a preview run over a box (seed, size, fit to landscape), **Generate** (bakes a cell-sorted `.ocinst` under Foliage, lists it in the level, shows it) and **Clean up**, and **Debug points** (a cross in the viewport at the selected node's output). Content Browser: New PCG Graph. Tests: `PcgEditModelTest` (38), `PcgGraphEditorTest` (30).
- Not done: the GPU Density node (step 7), mesh-surface sampling, copy/paste and comment boxes on the PCG canvas, a per-attribute table in the inspector (only point counts and diagnostics are shown). Nothing was drawn or run; the canvas interaction (hit-testing, drag, link) is unverified on screen.

---

## W4. PCG vegetation and biomes on landscape

**Current state.** Foliage type asset `.ocfoliage` (mesh, material, scaleMin/Max, weight, randomizeYaw, collisionRadiusCm, alignToNormal; deliberately no density/seed, `OcFoliage.hpp` (V)); brush tool + `FoliageTypeEditor`; `scatterFoliageFromLayer` bake; terrain layers (8 paint layers, weightmaps, each layer can name a foliage type + density, `LANDSCAPE_LAYERS_WATER.md` (V)); `heightSource`/`makeInfiniteHeightSource`; chunk generator scatter; streamed foliage tables with cell residency. **Missing:** biome concept, ecological rules (slope/altitude/moisture/layer), clumping, competition, per-type cull distances, a PCG-driven path, editor workflow.

**Target.** Biome = (region mask) x (PCG subgraph set) x (foliage palette). Painted landscape layers + noise masks decide where; PCG graphs place; results stream with the level.

**Steps.**
1. **Biome asset (S, H):** `.ocbiome`: name, mask source (landscape layer name | noise spec | spline/volume), priority, list of `{foliage type, density, slope range, height range, layer weight min, noise threshold, clump params, prune priority/radius}`. Round-trip tests.
2. **Rule nodes (M, H):** Slope, Altitude, Layer Weight, Noise-mask, *Clump* (parent points + children in radius, Poisson-disk), *Compete* (self-pruning where the larger type, by `collisionRadiusCm` and priority, removes smaller ones), *Edge falloff* (blend between two biomes by mask gradient with a seeded dither so borders interleave, no hard line). Tests on a synthetic heightfield + weightmap fixture: counts within tolerance, slope/altitude respected, no overlaps closer than radius, determinism.
3. **`.ocfoliage` additive fields (S, H):** `cullStartCm`, `cullEndCm`, `lodScaleBias`, `alignMaxAngleDeg`, `groundOffsetCm`, `collision: none|trunk|full`. Absent-vs-zero rule (`OcFoliage.hpp` header (V)). Consumers: `GameFoliage` (`Runtime/src/GameFoliage.cpp` exists (V)) applies cull range on residency; renderer side uses the existing per-mesh LOD (and clusters once W1 lands).
4. **Landscape integration (M, H model / G look):** *Generate Vegetation* in Landscape/Foliage mode runs the biome graph over the landscape sections, writes cell-sorted `.ocinst` + a record so it can be regenerated (replaces the one-shot, non-regenerating bake of `scatterFoliageFromLayer`; keep that button as a thin call into the new path). Manual brush edits are kept as an override set (the `persistChunk` baseline-vs-override idea in `LayeredChunkSource` (V) is the model).
5. **Runtime procedural mode (M, H):** `GeneratedChunkSource` driven by a biome graph for open worlds; heightfield provider from the landscape; tests for chunk-edge continuity (no seams or duplicates between neighbouring chunks: points in the overlap are owned by exactly one chunk).
6. **Editor UX (M, G):** Biome list in Landscape mode (create/assign/preview mask overlay as a coloured landscape overlay), density debug heatmap, instance count and memory readout, "Regenerate selected cells".
7. **Performance guardrails (S, H):** per-cell instance cap, memory estimate before generation, warning if a type lacks LODs or has no `cullEndCm`.

**Status 2026-11-09 (W4 steps 1-5 and 7, headless; `docs/PCG.md` section 8).**
- Done, step 1: `.ocbiome` is hand-editable text (mask: layer, noise, spline or volume with ramped edge and invert; entries with density, slope, height, layer, noise, clump, priority, prune). Canonical round trip, hand-written text, forward-compatible unknown records, refusals (`OcBiomeTest`).
- Done, step 2: nodes `NoiseMask`, `EdgeFalloff` (seeded dither, so two biomes interleave), `Clump` (children in radius, sibling spacing), `Compete` (priority, then radius); slope, altitude and layer weight are the existing filters. `pcgBuildBiomeGraph` compiles biomes plus `.ocfoliage` into one `DOMAIN pcg` graph. Tests on a tilted plane, a 30 degree ridge and a weightmap fixture: counts within tolerance, slope, altitude and layer respected, no overlaps closer than the scaled radii, no grass inside a trunk's radius, determinism, ramp densities falling and rising across a border (`PcgBiomeTest`).
- Done, step 3: `.ocfoliage` gains `cullStartCm`, `cullEndCm`, `lodScaleBias`, `alignMaxAngleDeg`, `groundOffsetCm`, `collision` (absent reads the default, not zero); `.ocinst` gains the optional `ICUL` chunk and collision flag bits; the biome graph consumes `alignMax` and `groundOffset`. `GameFoliage` applies the cull range on residency (quantised seeded thinning between start and end, whole run dropped past end). The keep rule is unit-tested; the `GameFoliage` change itself compiles but never ran (it needs a renderer).
- Done, step 4 (model): `VegetationGenerator` writes a cell-sorted `.ocinst` plus an `.ocveg` record with the manual override set (erase stamps, additions) and regenerates only stale cells; `scatterFoliageFromLayer` is a thin call into it through `sandbox/src/VegetationGenerate` (tested on a synthetic section and weightmap, `VegetationGenerateTest`). The sandbox wiring (extra foliage table, `levelHeader_.foliageFiles`) compiles but was not run.
- Done, step 5: a `PCGVOLUME` whose `graph` is an `.ocbiome` generates chunks from the biome (`GameStreaming`, `GameLandscape::layerWeightAtScene` wired in both hosts). Four neighbouring chunks equal a whole-area run instance for instance, clump and compete included, and no instance is in two chunks.
- Done, step 7: per-cell instance cap (deterministic: smallest seeds kept), memory estimate that refuses an over-cap setup before generating, warnings for no `cullEndCm`, no LODs (host callback), unknown paint layer.
- Not done: step 6 (all editor UX: biome list, mask overlay, heatmap, readouts, "Regenerate selected cells"); editor fields for the new `.ocfoliage` properties; brush edits writing the override set (the model and file format are ready); consumers of the `.ocinst` collision flags and of `lodScaleBias` (carried, not read); a mesh-LOD probe for the "no LODs" warning in the hosts (the hook takes a callback, none is wired); a biome set UI for several biomes at once (the compiler accepts a list).
- Not verified: nothing ran in the editor or the game. Greedy `Compete` across cell seams passed the four-cell comparison but is not proven equal to a whole run in pathological chains (margin is three diameters). Layer masks at runtime read the host's section data (`layerWeightAtScene`); not exercised. Positions are world-space `f32`, as for W3.

**Status 2026-11-09 (W4 step 6, part; compile-checked and tested headless).**
- Done: Landscape > Paint > Biomes (Generate Vegetation): the project's `.ocbiome` files as a checkbox list, New Biome from a foliage type, seed and cell size, the guardrail estimate line, and **Generate Vegetation** for the ticked biomes together (`generateBiomeVegetation`, `estimateBiomeVegetation` in `VegetationGenerate`, `BiomeListModel.hpp`; `VegetationGenerateTest` 41 checks). It writes `Foliage/<name>_vegetation.ocinst` plus the `.ocveg`, keeps hand edits, and shows the result through the level's extra foliage tables.
- Not done: mask overlay on the terrain, density heatmap, readouts beyond the estimate and the result line, "Regenerate selected cells", editing a biome's entries in the UI (they are text), editor fields for the new `.ocfoliage` properties.

---

## W5. In-engine modelling mode (UE Modeling Mode basics)

**Current state.** `fmt::csg` (`modules/formats/src/Csg.cpp`, 792 lines): BSP booleans on **convex polygons in doubles**, primitives (box, sphere, cylinder/cone, torus, extrude, spin, sweep), `transform`, `invertFaces`, `bounds`, `volume`, `surfaceArea`, `toSoup`, `fromTriangles` (`Csg.hpp:17-79` (V)); contract: closed outward-wound solids only. `LightmapUnwrap.cpp` (chart unwrap + packing) exists in Formats. `writeOcMesh` + `MeshBuild.cpp` exist. `EditorMode { Select, Landscape, Foliage, Simulate, Animate }` (`SandboxApp.hpp:831` (V)). No editable polygon mesh, no mesh-edit UI, no in-editor mesh asset authoring.

**Target (basic, as asked):** a Modeling mode that creates a mesh asset: primitives, select vertices/edges/faces, extrude, inset, bevel, delete, merge/weld, flip normals, boolean union/subtract/intersect on two meshes, transform/pivot, simple UV tools, save as `.ocmesh` placed in the level.

**Design.**
- New headless **`EditMesh`** (indexed polygon mesh with vertex/face/edge handles, per-face material and smoothing group, per-corner UV) in Aver.Formats beside Csg (no new module; Formats already owns mesh data and csg). Conversions `EditMesh <-> csg::Mesh` (triangulate/convexify non-planar quads before booleans) and `EditMesh <-> OcMeshData`.
- Booleans reuse `csg::combine` (standing rule). Known limits to surface to the user: closed solids only, double-precision BSP, polygon count growth; speed on 10k+ polygon meshes is unmeasured (?); validate input and refuse open meshes with a message instead of producing garbage.
- Mode UI follows landscape mode's pattern: a new `EditorMode::Modeling` value, a `ModelingMode.{hpp,cpp}` for tool panels and a pure `ModelingModel.hpp` for tool state, undo via `SnapshotUndo<State>` (the whole EditMesh is the state; fine for basic scale).
- Saving: `OcMeshData` through `writeOcMesh`, then optionally Trifactor cook (`#if AVER_MODULE_TRIFACTOR`, the conversion sits in `ConvertTool` today (V)) so the model joins W1's virtualized path, plus `LightmapUnwrap` for the second UV set.

**Steps.**
1. **EditMesh core (M, H):** structure, validation (manifold/closed/degenerate checks), triangulation, normals, bounds, conversion + round trip with `OcMeshData` and csg. **M.**
2. **Primitives (S, H):** box/sphere/cylinder/cone/torus/plane/stairs/arch via existing csg generators + a few new ones, with UV set and a "pivot at base" option (the EditorTransform code already cares about base-pivot meshes, `EditorTransform.hpp:75` (V)).
3. **Poly edit ops (L, H):** select modes, move/rotate/scale selection, extrude (face/edge), inset, bevel (edge, chamfer segments), delete, weld/merge, flip, subdivide (flat), loop cut (v2), mirror. Each op: topology + area/volume invariants as tests (extrude a cube face then check volume = base*height).
4. **Booleans (M, H):** operands = EditMesh vs another placed mesh/primitive; apply via csg; keep material per source; result cleanup (weld, remove degenerate). Tests with known volumes (union of overlapping boxes etc.; `csg::volume` exists).
5. **UV tools (M, H):** box/planar/cylindrical projection, per-face transform, auto-unwrap by calling `LightmapUnwrap` for UV1, texel-density display value.
6. **Mode UI (L, G):** viewport picking and gizmos for vertex/edge/face, selection overlay (reuse debug-line draw (?) and the existing gizmo/marquee code, `ViewportPick.hpp`, `ViewportMarquee.hpp` exist (V)), tool panels, hotkeys, Details integration. Pure pick math (ray vs vertex/edge/face, marquee select) is **H**; drawing is **G**.
7. **Asset round trip (M, H model / G):** New Mesh asset, save, reimport into Content Browser with thumbnail, "edit in Modeling" on an authored mesh (only meshes saved with edit metadata keep quads; imported meshes open as triangles); level placement.
8. **Out of v1:** sculpting, retopo, subdivision surface, n-gon booleans without triangulation, skinned meshes, Geometry Script C# API (the parity plan item 15 mentions "a C# geometry API"; EditMesh gets a thin C# wrapper later, decision D6).

**Status 2026-11-09 (steps 1-5 and the model half of 7 done, headless; no viewport UI).**
- Done: (1) `fmt::em::EditMesh` (`modules/formats/.../EditMesh.hpp`, `EditMesh.cpp`, `EditMeshOps.cpp`): n-gon faces, per-corner UV, material, smoothing group; `validate` (open / non-manifold / mixed-winding / degenerate), ear-clip triangulation, bounds / volume / area, conversions to and from `OcMeshData` and csg. (2) Primitives box, sphere, cylinder, cone, torus, plane, stairs, arch (Z up, UVs, pivot at base or centre). (3) Poly ops: move / rotate / scale, extrude faces (region) and boundary edges, inset, bevel (edges, chamfer or rounded; at a vertex either one or all of its edges), delete faces / edges / vertices, merge and weld, flip, flat subdivide, mirror, T-junction repair; each has volume or topology invariants in `EditMeshTest`. (4) `boolean()` over `csg::combine`: refuses open or inside-out operands with a message, merges materials by name, welds and repairs T-junctions. (5) Box / planar / cylindrical / spherical UV projection, per-face UV transform, texel density, `addLightmapUv1` through `LightmapUnwrap`. (7, model) `saveAuthoredMesh` writes the `.ocmesh` plus a `.ocmesh.edit` sidecar tied to the file by a hash; `loadAuthoredMesh` reopens quads from a matching sidecar and triangles otherwise; `SaveOptions::cook` is the hook for the Trifactor cook. `sandbox/src/ModelingModel.hpp`: document + selection modes + every command above with `SnapshotUndo`; a refusing command changes nothing.
- Tests: `EditMeshTest` (150 checks), `ModelingModelTest` (69 checks).
- Not done: step 6 (viewport picking, gizmos, overlay, panels, hotkeys, `EditorMode::Modeling`, `ModelingMode.cpp`); step 7's editor half (New Mesh asset, Content Browser thumbnail, "edit in Modeling", level placement) and wiring the Trifactor cook into `SaveOptions::cook`; loop cut, loop / ring selection, bevel at corners where only some of the edges are selected, bevel profile other than the corner-controlled curve.
- Known limits: boolean results keep no smoothing groups and come back as convex BSP pieces (many triangles / small polygons), speed on large meshes is unmeasured; `fromOcMesh` makes one smoothing group for every non-flat corner; the sidecar is ignored once the `.ocmesh` is changed by anything else (by design). Nothing was run in a window.

**Status 2026-11-09 (W5 step 6, compile-checked and tested headless).**
- Done: `EditorMode::Modeling`. `ModelingPick.hpp` (pure: project the mesh once, nearest vertex, nearest edge, front-most face, marquee collecting vertices, edges or faces, additive/replace; uses `ViewportMarquee.hpp`), `ModelingMode.{hpp,cpp}` (tool panels: New Mesh, Select, Transform, Edit with extrude/inset/bevel/delete/weld/flip/subdivide/mirror/boolean, UV, File; click/marquee glue; wireframe overlay with the selection in orange), `SandboxModeling.cpp` (viewport input: 1/2/3 select modes, Delete, Ctrl+Z/Y routed to the mode while it is active). Tests: `ModelingPickTest` (30), `ModelingModeTest` (17).
- Not done: a viewport gizmo for moving the selection (transforms are numeric fields), a shaded view of the edit mesh (it is drawn as lines), occlusion-aware picking (clicks prefer the nearer element, marquees select through), New Mesh asset / thumbnail / "edit in Modeling" from the Content Browser, placing the result in a level, the Details integration, wiring the Trifactor cook into Save.

---

## W6. Physics-driven animation (physical animation, hit reactions, blend in/out)

**Current state (V from headers).** `RagdollSystem::start(entity, mode, blendSeconds)` / `stop` (blend back, then destroy) / `setStrength` (one 0..1 value) / `addImpulse(entity, boneHash, impulse)`; weight ramp to write the simulated pose over the animated pose in `apply()`; chained after the control rig; Jolt ABI `aver_phys_ragdoll_{create,set_mode,set_strength,set_target,set_pose,add_impulse,body_of_bone,of_body}`. Motors pull each joint toward the target pose relative to parent; the root part is not driven; one global strength. C# `Ragdoll.cs`; framework relay `aver_fw_set_ragdoll_provider`.

**Gaps vs UE Physical Animation Component / hit reactions.** (1) strength/damping/blend are global, not per bone or per region; (2) no profile asset (UE: Physical Animation Profile); (3) no hit-reaction trigger (hit bone + impulse -> local blend-in, hold, blend-out); (4) no partial-body simulation (simulate arm only, rest stays animated; I did not check whether `RagdollBuilder` can build a subtree, ?); (5) no recovery/get-up; (6) no graph nodes, Details panel, debug overlay (nodes for other systems exist: `GraphNodeDefsPhysicalSurface.hpp`, `GraphNodeDefsIk.hpp`; a ragdoll node family not checked, ?); (7) no authoring of per-bone limits/mass in an editor (`RagdollBuilder` has `BuildOptions` and `definition()` (V)).

**Steps.**
1. **Profile asset `.ocphysanim` (S, H):** region -> bone list (or bone + descendants), per region `{strength, drive damping, maxTorqueScale, blendWeightTarget, hit response}`. Round trip + validation against a skeleton (unknown bones).
2. **Per-bone weight and strength (M, H):** additive ABI `aver_phys_ragdoll_set_part_strength(ragdoll, bone, strength)` and `..._set_part_damping`; `RagdollSystem::State` carries a per-bone blend weight array (instead of one `weight`) so `apply()` blends per bone; keep the single-weight API as "set all". ABI is additive only (minor bump). Jolt runs on CPU: tests run headless with a small test skeleton (build ragdoll, set pose, step, assert).
3. **Hit-reaction envelope (M, H):** pure `HitReaction` module: `trigger(bone, impulse, point, profile)` -> per-bone envelope (attack, hold, decay), falloff along the bone chain by hop distance, strength boost on the hit region, additive impulse on the hit part, auto-return to kinematic when envelope < epsilon. Tests: envelope math, chain falloff, repeated hits stack/cap, determinism, "recovers to animated pose within N steps".
4. **Partial-body simulation (M, H):** simulate subtree from a chosen bone, others kinematic-driven; needs builder support for attaching to a kinematic parent part (?). Fall back to whole-body powered ragdoll with high strength outside the region if the builder cannot do it. Test: parts outside region follow animation within tolerance while hit region deflects.
5. **Death / recovery (M, H logic):** powered -> limp transition rule (strength ramp to zero), get-up: choose montage by facing (prone/supine from the pelvis up-vector) via the existing Montage system; tests for the selection rule and state machine.
6. **Damage-event glue (S, H):** a neutral `HitEvent {entity, bone, impulse, point, damage}` published to scripting; the weapon/hit code supplies it (Fabric-converted games map their own damage events onto it; no game-specific code, standing rule). Same event feeds W7.
7. **Graph nodes + C# API (M, H):** `ApplyHitReaction`, `SetPhysicalAnimationProfile`, `BlendPhysicalAnimation(weight, seconds)`, region strength nodes; node-table tests like other families.
8. **Editor (M, G):** profile editor tab (region tree on the skeleton, sliders), debug overlay showing parts, strength heat and active envelopes, an "apply hit" button in Animate/Simulate mode. Visual tuning of feel is G and subjective.

**Status 2026-11-09 (steps 1-7 done, headless; `docs/physics/PHYSICAL_ANIMATION.md`).**
- Done: (1) `.ocphysanim` text profile with round trip, validation against a skeleton and resolve (`PhysAnimProfile`). (2) Additive ABI `aver_phys_ragdoll_set_part_strength / _damping / _torque_scale / _pinned`; per-bone blend weight in `RagdollSystem` (a multiplier on the kept global ramp). (3) `HitReaction` envelope (attack, hold, decay; falloff by hop; stacking capped at 1; deterministic; runs out by itself). (4) Partial-body simulation: `RagdollBuilder` needed no change, a pinned part is a kinematic part joined to simulated neighbours, so the fallback was not used (`simulateSubtree`). (5) `LifeMachine` (Alive/Dying/Dead/GettingUp) and the get-up montage rule from how the root part lies, `die()` / `getUp()` on the system, montage played through a handler or `AnimSystem::montages()`. (6) `HitEvent` in `aver/core/HitEvent.hpp` (+ C# `HitEvent`), the type W7.7 should consume; `RagdollSystem::applyHit` and a radial form. (7) `framework_physanim_abi.h` relay, `Entity.ApplyHit` / `BlendPhysicalAnimation` / `SimulateBoneSubtree` / `SetBone*` / `PinBone` / `DieAsRagdoll` / `GetUpFromRagdoll`, ten `PhysAnim` graph nodes; Aver.Physics C# bindings for the new calls.
- Tests: `PhysAnimProfileTest` (64 checks, pure), `PhysicalAnimationTest` (67 checks, headless Jolt, 12 bone skeleton), `FrameworkPhysAnimAbiTest` (18), `PhysAnimNodeTests` in the managed graph suite; full headless suite 552/552.
- Not done: step 8 (editor tab, overlay, apply-hit button). Not verified: the feel of any reaction in a window (profile defaults are untuned guesses); the sandbox and runtime wiring compiles but was never run; nothing wires a game weapon to `HitEvent` (adapters call `Entity.ApplyHit`); the get-up does not move the entity to where the body lies; a hit before the entity has been posed once is refused.

**Status 2026-11-09 (W6 step 8, compile-checked and tested headless).**
- Done: the Physical Animation Profile tab (`PhysAnimEditor`, `PhysAnimEditModel.hpp`, opens `.ocphysanim`): skeleton picker (the profile names none, so the previewed skeleton is chosen per session), the bone tree coloured by region with the heat of a previewed hit, regions (add, remove, rename, move = precedence, add/remove bone or subtree), drive and envelope sliders, reach and life settings, an offline hit preview that runs the real `HitReaction` envelope (curve, bones reached, settle time) and the problems list from `validatePhysAnimProfile`; Content Browser: New Physical Animation Profile. In the Simulate and Animate mode panels: a **Physical Animation** section with profile choice (session), bone, push, direction, radial, **Apply Hit**, Die and Get Up on the selected posed character. Tests: `PhysAnimEditModelTest` (33), `PhysAnimEditorTest` (16).
- Not done: the viewport debug overlay (parts, strength heat, active envelopes). Not verified: the feel of any reaction; Apply Hit was never run against a live ragdoll.

---

## W7. Voronoi fracture and destruction

**Current state.** Soft bodies exist (Jolt soft-body ABI `physics_softbody_abi.h`, `modules/softbody{,.scene}`: cage, islands, damage, tear) and rope. No rigid fracture. `Csg` supplies mesh intersection with convex solids and `volume()`; `Soup`/`toSoup` supply watertight weld. Rigid body creation from convex hulls: `physics_shape_builder_abi.h` exists (convex hull builder not verified, ?).

**Target.** UE Chaos-like geometry collection (basic): pre-fracture a static mesh into Voronoi pieces at edit time; at runtime the intact mesh is swapped for piece rigid bodies on damage, with connectivity, anchors, debris cleanup.

**Steps.**
1. **Fracture core (L, H):** `fracture::voronoi(mesh, seeds, options)`: seed generators (uniform, radial about a point, slab/brick, clustered), cell = intersection of bisector half-spaces (bounded by neighbour search to avoid O(n^2) for large counts), clipped against the closed mesh with `csg::combine(Intersection)`; interior faces get a dedicated inside material and planar UVs; optional two-level hierarchical fracture; fixed seed -> deterministic. Tests: sum of piece volumes = source volume within tolerance, every piece closed (`volume>0`, edge-manifold), no overlap (pairwise intersection volume ~ 0 on a sample), deterministic. Location: Formats geometry files next to Csg, no new dependency. Known risk: BSP robustness and speed on meshes with thousands of polygons; add a pre-step to fracture a convex-decomposed or simplified proxy (?).
2. **Connectivity graph (M, H):** adjacency from shared cell faces with contact area; anchor flags (touching ground/user-marked); `connectedComponents` after removing broken links (reuse the idea in `softbody/Islands.hpp`, do not couple).
3. **`.ocfrac` asset (M, H):** AVR1 container: pieces (indexed to one shared vertex pool for LOD/streaming friendliness), per-piece mass/centroid/convex hull, adjacency + area, break thresholds (per link), levels. Round-trip and `validate` tests.
4. **Runtime component (L, H physics / G look):** `Destructible` component: intact visual mesh + single body; `Damage(point, impulse, radius)` or contact impulse over threshold -> break links within radius (strain accumulation, not just one-shot), replace broken-off components with piece bodies (convex hull shapes), apply radial impulse, keep anchored component static, sleep/despawn rules, active-piece cap, pooling. Jolt runs headlessly, so body counts, momentum and island results are testable without a GPU. Visuals (pieces share the source material; inside material), shadows/RT for pieces = ordinary entities, **G**.
5. **Breakable constraints option (M, H):** instead of instant release, join adjacent pieces with Jolt breakable joints so partial collapse happens (the wave-1b Jolt list includes "breakable joints" (D); confirm they landed, ?).
6. **Editor (M, G):** Fracture tool in the mesh/Details panel: seed count/pattern preview, "Fracture" -> writes `.ocfrac`, exploded-view slider, anchor painting later; "Destructible" component in Details.
7. **Damage unification (S, H):** accept the same `HitEvent` as W6 plus explosion radial events; soft-body damage keeps its own path but takes the same event type.
8. **Audio/FX hooks (S):** fire a script event per break (surface from `PhysicalSurface`); no engine FX coupling.

**Status 2026-11-09 (headless steps 1-5 and 7).**

- Done: (1) `fracture::voronoi` in `Aver.Formats` (`Fracture.hpp`): uniform / radial / brick (stagger) / clustered seeds, bounded grid neighbour search (stops at 2x the cell's reach), cells clipped to the closed source with the new `csg::Prepared` (BSP built once), inside material (default one past the source's) with planar UVs, deterministic (own PRNG). Tests: volumes sum to the source (1e-6), pieces closed, no overlap on sampled pairs, brick lattice gives the exact 3x3x3 contact graph (54 links of area 9), 300-seed tiling, L-shaped non-convex source. (2) `makeGraph` / `anchorByPlane` / `components`. (3) `.ocfrac` (`OcFrac.hpp`, AVR1 subtype FRAC): shared vertex pool, per-material ranges, hull points, links with threshold, anchors; `validateOcFrac`, round trip, truncation and corruption refused. (4) `Aver.Destruct` (`AVER_MODULE_DESTRUCT`): `Destructible` over the physics ABI, Compound mode with strain accumulation, anchors (static body per anchored component), splitting with velocity inherited at the new centre of mass, hit share to the freed debris, `maxDebrisPieces` cap, sleeping despawn, per-piece hull shape pooling, cleanup verified. (5) Jointed mode: a body per piece, fixed joints with `aver_phys_joint_set_break` (breakable joints are in the ABI), break queue read in `update`. (7) `Destructible::applyHit(HitEvent)` (point, radial by `radius`, damage fallback) and `applyContact`. `docs/DESTRUCTION.md`.
- Measured (Debug build, not Release; the only configuration this tree builds): fracture of a 3528-polygon sphere takes 11.5 s for 32 seeds (about 360 ms per piece) and 19.8 s for 96 seeds (about 200 ms per piece); a 12-polygon box with 300 seeds takes 0.29 s. The cost is the BSP boolean against the full source per cell, so it grows with source polygons; cells that touch no source surface skip the boolean. Expect a large factor less in an optimised build, but that was not measured. The plan's proxy / convex-decomposition pre-step is not done.
- Not done: hierarchical two-level fracture; splitting a cell that cuts a non-convex source into several islands (one piece, hull of all of it); convex hulls are of the piece's vertices, not of a convex decomposition; body pooling (shapes are pooled, bodies are not); step 6 editor tool (out of scope); step 8 break audio/FX hook (events are available through `takeEvents()`); the scene component, visuals (pieces drawn with `piecePose`, shared and inside materials), shadows and RT for pieces; no soft-body damage path takes the `HitEvent` yet; rotated placement is tested by bounds and one link hit, not by a debris trajectory.

---

## W8. Texture streaming

**State.** Design is complete (`wave4/TEXTURE_STREAMING.md`, 352 lines): feedback is software, handle never changes, partial reads of the existing `.octex` container (no format change), off-thread staging, mip-window swap under the same handle. **Nothing is implemented** ("Not found anywhere: any texture residency tracking..." in the design; texture streaming is "Missing" in `UE5_GAPS.md`). Still open: patent counsel read (design R5), D3D12 bindless versioning costs 8192 descriptors, blocking tail seed, packaged builds have no usable DDC (R2) which intersects the unfinished `.ocpak`/cook item.

**Steps (design section 9, annotated).**
1. `readFileRange`, cache `statCachedTexture/readCachedMips`, mip-window math + `TextureStreamMathTest`. **S, H.**
2. Pure planner (budget, wanted-first-mip per texture, hysteresis, eviction order) + `TexturePlannerTest` with synthetic footprints. **M, H.**
3. Streamable predicate + MaterialSystem owns texture ids (behaviour-neutral refactor; existing suites must stay green; `texIndex` stays in the b2 block). **M, H** (build + suites) with an equivalence screenshot **G**.
4. RHI: texture stager, `bufferMapped`, `streamTextureMips`, descriptor rewrite incl. D3D12 bindless versioning, Vulkan equivalent. `TextureStreamRhiTest`. **L, G** (D3D12 then Vulkan).
5. Feedback shader + UAV slot + copy-out/readback + `TextureFeedbackShaderTest`. **M, G.**
6. Streamer worker, host wiring (editor + game), project keys, console vars, panel, and verification 4.1-4.6 of the design (resident MiB on Caldera/NeonDistrict, no hitch). **L, G.**
7. Optional deferred tail seed if the hitch gate trips. **M, G.**
Cook/package integration: later with `.ocpak` (cross-reference, not in wave 2).

**Status 2026-11-09 (CPU steps 1-3).**
- Done, headless-tested, full build and the 579-test suite green:
  - Step 1: `readFileRange` (platform, cases in `FileRangeTest`); `statCachedTexture`/`readCachedMips` and `CachedTextureInfo` in `TextureCache` (the whole-file loader now shares the header parser); `TextureStreamMath.hpp` (window bytes, legal tops, tail/initial mip, footprint to mip, `textureStreamable`); `tests/assets/TextureStreamMathTest`.
  - Step 2: `TexturePlanner`, `PoolController`, `UploadRing` (`modules/assets`, no device); `tests/assets/TexturePlannerTest`.
  - Step 3: `MaterialSystem::textureId` owns the ray path's texture ids (cap `kMaterialTexIdCap` = 3072, released two `update()` calls after a texture is destroyed); `VoxiRenderer` takes material texture indices from it and appends all other textures above the cap; `MaterialTextureIdTest`. The streamable predicate is `textureStreamable`.
- Not done: `MaterialConstants::texIndex` in the raster b2 block still carries `kUnboundTexture` (nothing reads it until the feedback pass, step 5); the `bindFeedbackId`/`forget` streamer hooks (they need `ITextureStreamer`, step 4); the equivalence screenshot (G) for step 3, which is the only check that the ray path's changed table order renders identically; everything in steps 4-7 (RHI, feedback shader, worker, host wiring), held for the owner's patent decision D2.
- Differences from the design text: fallback textures get ids too (the design said unbound) so the ray path keeps sampling exactly what raster binds; the planner is time-free (hold and retry are in frames) and the tests use the design's FileRangeTest name inside the existing `FileRangeTest`.

---

## W9. Shadows on non-RT GPUs

**State.** Cascades exist; **Vulkan never writes the cascade map** because `pushRenderScope` joins the open scope and discards the caller's attachments (`VulkanDevice.cpp:3395-3412` (V), `docs/BUGS.md:360`). The comment records a failed attempt (retargeting changed 10 -> 40 validation errors and the floor stopped drawing) and the real fix: *the scope must follow the bound targets (close on retarget, reopen lazily, persist across draws) and pipelines must match the targets they use* (pipeline `rasterizationSamples` and `VkPipelineRenderingCreateInfo` formats, shadow image layout transition to `DEPTH_ATTACHMENT_OPTIMAL`). The local-light shadow path already uses an "isolated" scope (R2, `setTargetsIsolated`, V). GI's shadow pass (`giShadowPass`) has the same shape. D3D12 cascades work. No clipmap, no VSM.

**Steps.**
1. **Vulkan scope fix (L, G):** make isolated scopes the rule for shadow and GI-shadow passes (extend the existing R2 mechanism to `shadowPass` and `giShadowPass`), pipelines created/selected against the actual depth-only target format with `rasterizationSamples = 1`, explicit barriers to/from `DEPTH_ATTACHMENT_OPTIMAL`/sampled. Gate: validation-error count does not increase (baseline: the 10 non-fatal errors of `VULKAN.md` (D)) and a `--no-rt --backend vulkan` capture matches D3D12 within tolerance on the cube scene (the comment quotes darkest floor pixel 13,15,19 vs 66,66,66 as the symptom). The existing screenshot A/B tooling (`scripts/shots.py` diff, commit `dc161180`) is the measurement. Needs a Vulkan device and a headed run.
2. **Cascade quality pass (M, G):** texel-snapped cascade fitting, cascade blend band, slope-scaled bias, PCF kernel; compare D3D12/Vulkan after step 1 (I did not check which of these already exist, ?).
3. **Clipmap/cached shadow (design M, H planner; implementation L, G).** *Cached shadow clipmap, non-sparse v1*: N concentric 2D levels around the camera (e.g. 4 x 4096), texel-snapped scroll so only newly exposed strips and dirty-caster rects re-render, per-level static-caster cache with invalidation when a casting entity moves/spawns. **Pure parts H:** level selection, scroll/snap math, dirty-rect union, invalidation tracker (tests with synthetic moves). Render integration G. This avoids sparse virtual-page residency (which needs RHI work and is where VSM patents concentrate).
4. **Patent check (S):** repeat the wave-4 method for "virtual shadow map page cache invalidation" and clipmap shadow techniques before any VSM-like page table design; no counsel read exists yet (D2).
5. **VSM-style page pool (L, G), explicitly stage 2 and gated on D2/D7:** virtual 16k per light, physical page pool, page marking from depth, per-page cache. Not scheduled in wave 2; the clipmap (step 3) is the deliverable.
6. **Non-RT GI/AO fallback** is a separate row in the gaps doc (Lumen without DXR) and is out of scope for this wave.

---

## W10. Data layers

**State.** Nothing in `OcWorld`/streaming; only editor-only Outliner folders and locks (commit `d7c889ce`) which are organization, not runtime state.

**Target (UE data layers, basic).** Named layers; each placement/actor belongs to zero or more layers; layers have an initial state (Unloaded / Loaded / Activated) and an editor "visible/loaded in editor" toggle; runtime state set from C#/graph; streamer honours state.

**Steps.**
1. **Format (S, H):** header `DATALAYER name=<> initial=<unloaded|loaded|activated> runtime=<0|1> [parent=<>]`; per placement `layers=<a,b>`; additive, absent = no layer (loads exactly as before). Round-trip and "old reader ignores" tests; `OcWorldPlacement` field.
2. **Residency (M, H):** `PlacementStreamer` takes a layer-state provider; a placement is eligible iff all its layers are Loaded/Activated; Activated additionally enables simulation/render (Loaded = resident but hidden, mirroring UE). Tests: state transitions, interaction with distance residency, foliage tables per layer (`FOLIAGE ... layers=`).
3. **Runtime API (S, H):** `DataLayers.SetState/Get` in C# framework + graph nodes (`GraphNodeDefsWorld.hpp` pattern), events on completion.
4. **Editor (M, G for panel, H for model):** Data Layers panel (create/rename/delete, assign selection, toggle editor visibility, load/unload in editor so large levels open light), saved with the level, undo. Pure model tests for assignment/rename cascade.
5. **Cook path (S, H):** `AverAssetC cook-chunks` and `STREAM chunks=` must carry layer membership; test that chunk cooking splits per layer-state without losing placements.
6. **HLOD interaction (S, H):** proxies only built from always-on layers; a layer with `runtime=1` excludes its placements from HLOD (decision D3).

**Status 2026-11-09 (W10 steps 1-3, 5, 6; headless only).**
- Done and tested: (1) `DATALAYER`, `layers=`, `FOLIAGE ... layers=` (`OcWorldDataLayersTest`); (2) `PlacementStreamer` layer eligibility, `world::DataLayerSet` and its completion events (`DataLayersHlodTest`),
  and `game::LevelStreaming` builds and applies the set (compile-checked only); (3) C# `DataLayers`, `framework_datalayer_abi.h` (relay tested in `FrameworkTest`) and nodes `AN_GetDataLayerState`,
  `AN_SetDataLayerState` (`DataLayerNodeTests`); (5) `LAYR` payload extra, cook hash and `world::splitPayloadByLayers` (`AuthoredLayersTest`); (6) `hlodLayersAllowed` in the HLOD bake and `buildChunkProxy`
  (`OcHlodBakeTest`, `ChunkProxyTest`).
- Not done: the Runtime installing the framework provider (so `DataLayers` answers defaults in a game today); Loaded versus Activated applied to entities (hide, no simulation);
  gating cooked-chunk loading and baked foliage tables by layer state; a layered level without `STREAM` still loads everything; a completion event node or C# event (only `TryPollCompleted`).

**Status 2026-11-09 (W10 step 4, compile-checked and tested headless).**
- Done: Window > Data Layers (`DataLayerModel.hpp`, `DataLayersPanel`, `SandboxDataLayers.cpp`): create, create child, rename (children, members and editor flags follow), delete (children move up), assign or remove the selection, select a layer's members, initial state and runtime flag, editor Show and Loaded toggles (editor only: they hide the layer's objects through the session hide and make them unpickable, nothing is unloaded or saved), saved with the level as `DATALAYER` records and each placement's `layers=`, and undoable through the editor's undo stack (`EditCmd::Kind::DataLayers`, which marks the level dirty). Tests: `DataLayerModelTest` (44).
- Not done: layers on streamed-in placements (the streamed path does not read or write membership), Outliner rows greyed for hidden layers, drag-to-assign, layer-aware selection filters. Never drawn.
---

## 2. Dependency-ordered schedule

Owner is remote, so: **Phase A** is only headless-testable steps (agent-runnable end to end: build + ctest, local commit, no GPU); **Phase B** is GPU/headed work queued for when the owner can inspect. Workstream tracks that touch disjoint files run in parallel. "Shared file" notes mark collisions.

### Phase A, headless (can start now; max 4 tracks at once on Sonnet per the usage rule)

| Track | Steps (in order) | Files it owns | Parallel with |
|---|---|---|---|
| A1 Trifactor CPU | T-CPU-1 -> T-CPU-2 -> T-CPU-3 -> T-CPU-5 -> T-CPU-4 -> T-GPU-5 CPU planner | `modules/trifactor/**`, `modules/formats/src/OcMesh.cpp` + `OcMesh.hpp` (MLET v4), `tests/formats/src/TrifactorTest.cpp`, `RelodTool` | everything below |
| A2 PCG core + vegetation rules | W3 1-5, W4 1-3,5,7 | new `modules/world/**/Pcg*`, `Aver.Pcg` F# only for golden vectors, `OcFoliage.*`, new `.ocbiome` files | A1, A3, A4 (shared with A5: `ChunkGenerator.*`, `OcWorld.*`) |
| A3 EditMesh + modeling model | W5 1-5 | new `modules/formats/**/EditMesh*`, `Csg.*` (additive only), new `sandbox/src/ModelingModel.hpp` | A1, A2, A4 |
| A4 Physical animation + fracture core | W6 1-7 in `modules/ragdoll/**`, `physics_ragdoll_abi.h` (additive), `scripting/csharp/Aver.Framework/Ragdoll.cs`; W7 1-5 in new fracture files and `.ocfrac` | disjoint from A1-A3; W6 and W7 steps share only the `HitEvent` type (define it first, S) | A1, A2, A3 |
| A5 World: data layers + HLOD (after A2's `OcWorld` edits settle) | W10 1-3,5; W2 1-3,5,6(planner) | `modules/formats/src/OcWorld.*`, `OcStream.*`, `modules/world/PlacementStreamer.*`, `Runtime/src/*Streaming*` | A1, A3, A4. **Serialize against A2 on `OcWorld.*` and `ChunkGenerator.*`** |
| A6 Texture streaming CPU | W8 1-3 | `modules/platform` (readFileRange), `modules/assets/TextureCache.*`, new planner files | all |
| A7 Docs/gap-doc fixes | correct `UE5_GAPS.md` (physical animation row), VG doc "no indirect" note | docs only | all |

Order inside Phase A that matters: T-CPU-1/2 before anything GPU; `HitEvent` (W6.6) before W7.7; EditMesh (W5.1) before booleans/UV; `.ocpcg` decision (W3.1) before the graph editor model; W3.3 before W4.2.

### Phase B, GPU/headed (queued; each run needs owner approval, small first scenes, bounded kernels)

Sequence and why: (1) **Vulkan shadow scope fix** (W9.1) because it unblocks every Vulkan parity check below and is small in scope; (2) **texture streaming RHI + feedback** (W8 4-6) vs (3) **ClusterRenderer** (W1 T-GPU-1/2): both edit `VoxiRenderer.cpp` and the RHI backends, so **do not run in parallel**; recommended order cluster renderer first (bigger frame win for the shooter), then texture streaming; (4) HiZ + mesh back end (T-GPU-3) after the owner's patent decision D2; (5) RT proxy + deletions (T-GPU-4); (6) shadow clipmap render integration (W9.3); (7) editor UIs and viewport drawing: PCG graph editor canvas (W3.6), Modeling mode viewport (W5.6), physical-animation tab and overlay (W6.8), fracture tool (W7.6), data layers panel (W10.4), HLOD ring look (W2.6/2.8), vegetation UX (W4.6). The UI items touch separate sandbox files but each needs a hook in `SandboxApp.hpp`/`SandboxApp.cpp` (mode enum, `registerFactory`, member lists): **serialize those hook edits** (one integrator), put the real logic in new files.

Parallel-safe within Phase B: editor UI tabs (disjoint new files) can be built while a renderer track runs, but the renderer tracks themselves are serial.

### Rough effort (sum of step sizes, Sonnet-workflow days, order-of-magnitude)
W1 CPU ~7, W1 GPU ~12+; W2 ~7; W3 ~9; W4 ~6; W5 ~9; W6 ~7; W7 ~9; W8 ~9; W9 ~7 (without VSM); W10 ~4. Headless share is about 60% of the total.

---

## 3. Decisions the owner must make

- **D1 Destruction scope.** The 2026-10-17 decision says destruction = soft bodies (supersedes Voronoi, `wave9/SOFTBODY_CLOTH_DESTRUCTION.md:3`). Confirm W7 as brittle-rigid Voronoi *in addition* to soft-body dents/tears/cloth, with shared damage events and no shared solver. If not, W7 is cut.
- **D2 Patent/legal reads.** Counsel read still unrequested for: HiZ two-pass culling (Microsoft US 11,004,255 claims 1/13/18, R1 in the VG design; fallback = ship `geom.hiz` off), texture streaming (AMD 10,388,058 and 12,461,751, design R5), and any VSM-style page cache (W9.5). Wave 2 ships with HiZ off and clipmap-not-VSM unless told otherwise.
- **D3 HLOD semantics and scope.** Confirm "dynamic" = auto-built, cached, runtime-swapped proxies (plus runtime build for PCG chunks). Also: proxy material strategy (v1 keeps source materials per slot vs atlas bake as a later L), foliage impostors (v2), whether `runtime=1` data layers are excluded from HLOD, and whether HLOD proxies appear in RT (recommended: no).
- **D4 Module placement of the PCG evaluator.** CPU-only inside `modules/world` (simplest) vs a new CPU module split from `render.pcg` (cleaner dependency, new module). Neither adds a third-party library. Module-DAG check not performed (?).
- **D5 `.ocpcg` format.** Profile of the existing `.ocgraph` container (reuses editor canvas, clipboard, find, problems) vs a new AVR1 `.ocpcg`. My recommendation is the former if step W3.1 confirms typed data-flow support.
- **D6 Geometry Script.** The parity plan lists "a C# geometry API" with Modeling. Include a thin C# `EditMesh` API in wave 2 or defer (recommended: defer; editor first).
- **D7 Shadow target.** Accept "fixed Vulkan cascades + cached clipmap" as the wave-2 non-RT shadow deliverable, VSM-with-sparse-pages deferred.
- **D8 Trifactor scope cuts.** (a) Is cluster paging (W1 T-GPU-5) wave 2 design-only (recommended)? (b) Keep the mesh-shader back end or ship compute-only everywhere (saves the indirect mesh RHI work and the Vulkan mesh-shader pairing, loses some raster speed)? (c) OK to bump `kBuilderVersion` to 3 and require a bulk re-cook of multi-material project meshes (R2 in the VG design; tool never touches sources)? (d) Accept that skinned/morph meshes stay on discrete LOD.
- **D9 New dependencies: none requested.** Everything above is buildable with what is vendored: meshoptimizer (simplify, meshlets), Jolt, `Csg`, `LightmapUnwrap`, ImGui. Items that *would* need a yes if scope grows: a convex-decomposition library (CoACD/VHACD) if fracture pieces need better collision than hulls; a robust exact-arithmetic boolean library (e.g. CGAL/Manifold) if BSP booleans prove too fragile for modelling; METIS/KaHIP if W1 T-CPU-4 cannot beat greedy grouping (previously forbidden for licence reasons, `modules/trifactor/README.md`). Not asking now.
- **D10 Order within the owner's list.** My proposed priority: W1 CPU + W10 + W6 + W3/W4 first (headless, high value for the shooter and world building), then GPU tracks W9.1, W1 GPU, W8, then W5 UI, W7 UI. Re-order if the shooter's combat feel (W6/W7) should lead.

## 4. What I could not verify

- Any runtime behaviour: nothing was built or run (no timing, no image, no test results). Existing claims "builds and passes" come from README/docs only.
- Line numbers marked (D) were copied from the wave-4 design docs (written 2026-10-11); the tree has moved since (e.g. the indirect-RHI premise is already stale). Re-grep before implementing.
- Whether `RagdollBuilder` supports subtree/partial ragdolls, whether breakable joints and the convex-hull shape builder landed in the physics ABI, whether `OcGraph` supports typed data-flow graphs, existing viewport debug-draw primitives for overlays, which cascade-quality features already exist, Trifactor current build-time per MTri and the group-quality numbers (edge cut) for the greedy partitioner.
- The module DAG consequences of placing PCG evaluator, EditMesh, fracture and HLOD merger code (checked only by reasoning from `Formats` below `Trifactor`).
- BSP boolean speed/robustness at modelling scale and on fracture inputs.
- Whether `docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md` was implemented beyond what `modules/softbody{,.scene}` headers suggest; `git log` was not mined for it.
