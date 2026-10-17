# ufbx -- vendored for Aver

**Upstream:** https://github.com/ufbx/ufbx
**Version:** tag `v0.23.1` (`UFBX_HEADER_VERSION = ufbx_pack_version(0, 23, 1)`), pinned to a release
tag, not a branch. The importer asserts `UFBX_HEADER_VERSION >= 0.23.0`.
**Licence:** MIT or public domain (Unlicense), at our choice; used under MIT, (c) 2020 Samuli Raivio.
`LICENSE` is kept beside the sources (the editor payload's NOTICE lists it).

## Why it is here

`Aver.Formats.Fbx` (`modules/formats`) reads `.fbx` through it, at parity with the glTF importer.
Design: `docs/plans/wave9/FBX_IMPORT.md`; user-facing: `docs/FBX_IMPORT.md`. It is the only third-party code that
feature adds.

## What was taken

`ufbx.h`, `ufbx.c`, `LICENSE`, unmodified. Nothing else from the upstream tree (tests, examples,
bindings, build files). `CMakeLists.txt` here is Aver's own.

| file | sha256 |
|---|---|
| `ufbx.h` | `f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc` |
| `ufbx.c` | `4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d` |
| `LICENSE` | `0dd48ebadf52273c736256325c8f078c03c8bb4facee22a4122de0ad3f615391` |

## Build

`add_library(ufbx STATIC ufbx.c)`, compiled as C with warnings off. `ufbx_real` stays `double`
(`UFBX_REAL_IS_FLOAT` is not defined). The root `CMakeLists.txt` adds this directory only when
`third_party/ufbx/ufbx.c` exists; without it `Aver.Formats.Fbx` still builds with `AVER_HAVE_UFBX=0` and
`importFbx` fails with "built without ufbx".

## How to re-vendor

1. Download `ufbx.h`, `ufbx.c` and `LICENSE` of the new release tag from the upstream repository.
2. Replace the three files, update the version and the hashes above.
3. Re-check the names `FbxImport*.cpp` uses: `ufbx_bake_opts`, `UFBX_PIVOT_HANDLING_ADJUST_TO_ROTATION_PIVOT`,
   `ufbx_skin_cluster::bind_to_world` / `geometry_to_bone`, `ufbx_blend_shape`, `ufbx_load_opts::target_axes`.
4. Run `ctest -R "Fbx|GltfTest|ImportTest|AverAssetC"`.
