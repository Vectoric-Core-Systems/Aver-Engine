# Aver.Trifactor

The virtualized-geometry cluster builder and LOD DAG. Plan: `docs/VIRTUALIZED_GEOMETRY.md` section 7, "Slice 0". Written as an optional module, **OFF by default** in the `slim` edition (`AVER_MODULE_TRIFACTOR`), because its one permitted dependency, meshoptimizer (MIT), was not vendored at `third_party/meshoptimizer` until commit `4911686` closed that gate (which also vendored v1.2). No other dependency is allowed (no METIS, no hand-rolled partitioner) for licensing reasons (CORRECTION 1 of the task that built this module; `docs/formats/FORMAT_SPECS.md:13`, `docs/ASSET_IMPORT.md`). `AVER_MODULE_TRIFACTOR` is now `ON` by default in the `standard` and `full` editions (`CMakeLists.txt`'s `AVER_EDITION` presets).

## What's here

- `include/aver/trifactor/ClusterBuilder.hpp` / `src/ClusterBuilder.cpp` — `buildClusters()` (meshopt_buildMeshlets + meshopt_computeMeshletBounds, MLET-shaped output per `docs/formats/FORMAT_SPECS.md` 5.7), `buildLodHierarchy()` (group → locked-border meshopt_simplify → re-split, with explicit error-monotonicity propagation), and `validateLodDag()` (the five invariants: LOD-0 coverage, cluster size limits, bounds/cone validity, error monotonicity, DAG acyclicity + connectivity).

## What's here, continued (the persistence phase)

- MLET serialization/deserialization now lives in `modules/formats/src/OcMesh.cpp` (`OcMeshData::meshlets`, `modules/formats/include/aver/formats/OcMesh.hpp`) -- deliberately in Aver.Formats, NOT here, because Aver.Formats sits below Aver.Trifactor in the module DAG (`cmake/AvModule.cmake`) and `.ocmesh` must load/save with `AVER_MODULE_TRIFACTOR=OFF`. The conversion from this module's `Cluster`/`LodDag` to the format's `OcMeshMeshlet` lives at the one call site allowed to see both types: `tests/formats/src/ConvertTool.cpp`, behind `#if AVER_MODULE_TRIFACTOR`.
- `buildLodHierarchy`'s coarser levels persist to disk. `.ocmesh` now carries the whole DAG (`OcMeshData::coarserLods`, `modules/formats/include/aver/formats/OcMesh.hpp`), added by commit `81a4bb0` (".ocmesh persists the whole LOD hierarchy, with an error a runtime can use") and extended through `5a1eb6f`/`c511662`/`3be919b`. See `modules/formats/README.md` for what Aver.Formats owns today.
- `SubmeshRange.MeshletStart/MeshletCount` (FORMAT_SPECS.md 5.6) are written as 0,0 even when meshlets are present: `buildClusters` partitions the mesh's WHOLE merged index buffer without regard to submesh/material boundaries, so a meshlet can straddle two submeshes and there is no honest per-submesh range to report yet. See the comment at that write site in `OcMesh.cpp`.
- `tests/formats/src/TrifactorTest.cpp`, registered in `tests/formats/CMakeLists.txt` behind `if(TARGET Aver.Trifactor)` (so it does not exist at all when `AVER_MODULE_TRIFACTOR=OFF` -- now only the `slim` edition's default -- same as `MaterialTest`/`ActorScriptTest` needing `Aver.Formats.Material`).

## Verified: builds and tests

`AVER_MODULE_TRIFACTOR` is cached `ON` under the default `standard` edition (`build/CMakeCache.txt`, `build-release/CMakeCache.txt`), `Aver.Trifactor.lib` and `TrifactorTest.exe` are built in `build/`, `build-release/` and `build-vk/`, and running `TrifactorTest.exe` prints `=== all 201 Trifactor checks passed ===`.

The `SubmeshRange.MeshletStart/MeshletCount` gap noted above is unrelated and still real.
