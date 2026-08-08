# Aver.Trifactor

The virtualized-geometry cluster builder and LOD DAG. Plan: `docs/VIRTUALIZED_GEOMETRY.md` section 7,
"Slice 0". Optional module, **OFF by default** (`AVER_MODULE_TRIFACTOR`) — see
`CMakeLists.txt` for why: its one permitted dependency, meshoptimizer (MIT), is not vendored in this
tree at `third_party/meshoptimizer` yet, and CORRECTION 1 in the task that built this module
forbids any other dependency (no METIS, no hand-rolled partitioner) for licensing reasons
(`docs/formats/FORMAT_SPECS.md:13`, `docs/ASSET_IMPORT.md`).

## What's here

- `include/aver/trifactor/ClusterBuilder.hpp` / `src/ClusterBuilder.cpp` — `buildClusters()`
  (meshopt_buildMeshlets + meshopt_computeMeshletBounds, MLET-shaped output per
  `docs/formats/FORMAT_SPECS.md` 5.7), `buildLodHierarchy()` (group → locked-border
  meshopt_simplify → re-split, with explicit error-monotonicity propagation), and
  `validateLodDag()` (the five invariants: LOD-0 coverage, cluster size limits, bounds/cone
  validity, error monotonicity, DAG acyclicity + connectivity).

## What's here, continued (the persistence phase)

- MLET serialization/deserialization now lives in `modules/formats/src/OcMesh.cpp`
  (`OcMeshData::meshlets`, `modules/formats/include/aver/formats/OcMesh.hpp`) -- deliberately in
  Aver.Formats, NOT here, because Aver.Formats sits below Aver.Trifactor in the module DAG
  (`cmake/AvModule.cmake`) and `.ocmesh` must load/save with `AVER_MODULE_TRIFACTOR=OFF`. The
  conversion from this module's `Cluster`/`LodDag` to the format's `OcMeshMeshlet` lives at the one
  call site allowed to see both types: `tests/formats/src/ConvertTool.cpp`, behind
  `#if AVER_MODULE_TRIFACTOR`.
- Only `buildClusters`' LOD-0 output is persisted. `.ocmesh` still writes a single `LodDesc` (see
  `writeOcMesh`), so `buildLodHierarchy`'s coarser levels exist in memory and are exercised by
  `tests/formats/src/TrifactorTest.cpp`, but are not yet written to disk -- multi-LOD `.ocmesh` is a
  later slice.
- `SubmeshRange.MeshletStart/MeshletCount` (FORMAT_SPECS.md 5.6) are written as 0,0 even when
  meshlets are present: `buildClusters` partitions the mesh's WHOLE merged index buffer without
  regard to submesh/material boundaries, so a meshlet can straddle two submeshes and there is no
  honest per-submesh range to report yet. See the comment at that write site in `OcMesh.cpp`.
- `tests/formats/src/TrifactorTest.cpp`, registered in `tests/formats/CMakeLists.txt` behind
  `if(TARGET Aver.Trifactor)` (so it does not exist at all in the default
  `AVER_MODULE_TRIFACTOR=OFF` tree, same as `MaterialTest`/`ActorScriptTest` needing
  `Aver.Formats.Material`).

## What's NOT here (or not yet PROVEN)

- meshoptimizer itself. Still not vendored at `third_party/meshoptimizer` as of this phase --
  re-checked directly (`Get-ChildItem -Recurse -Filter meshoptimizer*` across `C:\` found nothing)
  before writing this note, not assumed from the previous phase's report. See the top-of-file
  comment in `src/ClusterBuilder.cpp` for the full account of why it isn't vendored and what needs to
  happen before this module builds.
- Because of that, `-DAVER_MODULE_TRIFACTOR=ON` still fails at CMake CONFIGURE time with the
  `FATAL_ERROR` this module's `CMakeLists.txt` is designed to raise (verified this phase: a real
  `aver_build -DAVER_MODULE_TRIFACTOR=ON` against `build-trifactor` failed with `[build] configure
  failed`). `ClusterBuilder.cpp` has therefore still never compiled, and `TrifactorTest.exe` has
  never been built or run -- everything in this module and in `TrifactorTest.cpp` is written against
  the documented meshoptimizer API and against this module's own header, not proven by execution.
  What HAS been proven by execution this phase: the MLET read/write code in `OcMesh.cpp` (via
  `MeshTest.exe`, which exercises the format independent of Trifactor), and that
  `AVER_MODULE_TRIFACTOR=OFF` still builds clean and every pre-existing headless suite (42/42,
  including `MeshTest`, `GltfTest`, `ImportTest`) still passes with this module absent.
