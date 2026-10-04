# Aver.Net  (`modules/net`)

- **Language:** C++
- **Depends on:** Core, Platform
- **Planned phase:** 6

[opt] Modular replication: payload/channel/module/coordinator triad, LE UDP codec, pose paging, damage fragmentation, join-parity gate. Interop-exact with the C# OCServer. See docs/recon/networking.md (protocol needs OCServer Wire.cs confirmation).

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/net)`) when Phase 6 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
