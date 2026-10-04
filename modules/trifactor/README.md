# Aver.Trifactor

The virtualized-geometry cluster builder and LOD DAG. Plan: `docs/VIRTUALIZED_GEOMETRY.md` section 7,
"Slice 0". Written as an optional module, **OFF by default** (`AVER_MODULE_TRIFACTOR`), because its
one permitted dependency, meshoptimizer (MIT), was not yet vendored at `third_party/meshoptimizer`,
and CORRECTION 1 in the task that built this module forbids any other dependency (no METIS, no
hand-rolled partitioner) for licensing reasons (`docs/formats/FORMAT_SPECS.md:13`,
`docs/ASSET_IMPORT.md`). **That gate closed in the same commit that wrote this file** (`4911686`,
which also vendored `third_party/meshoptimizer` v1.2) — see "What's NOT here" below for what that
made obsolete. `AVER_MODULE_TRIFACTOR` is now `ON` by default in the `standard` and `full` editions
(`CMakeLists.txt`'s `AVER_EDITION` presets) and `OFF` only in `slim`.

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
- `buildLodHierarchy`'s coarser levels no longer stop in memory. This bullet used to say multi-LOD
  `.ocmesh` was "a later slice" that had not arrived; it has. `.ocmesh` now persists the whole DAG
  (`OcMeshData::coarserLods`, `modules/formats/include/aver/formats/OcMesh.hpp`), added by commit
  `81a4bb0` (".ocmesh persists the whole LOD hierarchy, with an error a runtime can use") and
  extended through `5a1eb6f`/`c511662`/`3be919b`. See `modules/formats/README.md` for what
  Aver.Formats owns today.
- `SubmeshRange.MeshletStart/MeshletCount` (FORMAT_SPECS.md 5.6) are written as 0,0 even when
  meshlets are present: `buildClusters` partitions the mesh's WHOLE merged index buffer without
  regard to submesh/material boundaries, so a meshlet can straddle two submeshes and there is no
  honest per-submesh range to report yet. See the comment at that write site in `OcMesh.cpp`.
- `tests/formats/src/TrifactorTest.cpp`, registered in `tests/formats/CMakeLists.txt` behind
  `if(TARGET Aver.Trifactor)` (so it does not exist at all when `AVER_MODULE_TRIFACTOR=OFF` --
  now only the `slim` edition's default -- same as `MaterialTest`/`ActorScriptTest` needing
  `Aver.Formats.Material`).

## What's NOT here (or not yet PROVEN)

This section used to say meshoptimizer was still not vendored, that `-DAVER_MODULE_TRIFACTOR=ON`
still failed at CMake configure time, and that `ClusterBuilder.cpp` had therefore never compiled and
`TrifactorTest.exe` had never been built or run. All of that was wrong the moment it was written:
`third_party/meshoptimizer` was vendored in the very same commit that wrote this README (`4911686`
touches both paths, and `git log` shows no other commit ever touching `third_party/meshoptimizer` --
it has been there since). Re-verified against the current tree: `AVER_MODULE_TRIFACTOR` is cached
`ON` under the default `standard` edition (`build/CMakeCache.txt`, `build-release/CMakeCache.txt`),
`Aver.Trifactor.lib` and `TrifactorTest.exe` are built in `build/`, `build-release/` and `build-vk/`,
and running `TrifactorTest.exe` prints `=== all 201 Trifactor checks passed ===`. (The
`SubmeshRange.MeshletStart/MeshletCount` gap noted above is still real and unrelated to any of this.)
