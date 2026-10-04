# Aver.NetVehicle  (`modules/netvehicle`)

- **Language:** C++
- **Depends on:** Net, Vehicle, SoftBody
- **Planned phase:** 6

[opt] Vehicle net modules (Drive/Damage/Config): quantized input, crash-seed replay (WORLD-space impacts + PartHint), durable damage snapshots (broken bitset/detached/repair), detach impulse+mode. See docs/physics-net/physnet-verify.json.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/netvehicle)`) when Phase 6 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
