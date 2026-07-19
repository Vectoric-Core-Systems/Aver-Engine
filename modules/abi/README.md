# Aver.ABI  (`modules/abi`)

- **Language:** C
- **Depends on:** Runtime + public module headers
- **Planned phase:** 7

The flat extern "C" seam: opaque handles, out-pointer returns, aver_abi_version(). The single interop boundary C#/Rust bind against. (Lives at ../abi, mirrored here for the map.)

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/abi)`) when Phase 7 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
