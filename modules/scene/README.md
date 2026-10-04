# Aver.Scene  (`modules/scene`)

- **Language:** C++
- **Depends on:** Core, Assets
- **Built:** SHARED (`Aver.Scene.dll`), because the C# layer P/Invokes it

Data-oriented entity/component world: entities, component storage, transforms, hierarchy. Render-
and physics-agnostic, and **no UObject** — there is no base class every game object inherits from,
because the storage this module exists to provide is dense arrays of plain data and an inheritance
tree is the shape that makes them impossible.

> **Status: the world runs and the C ABI is live.** Entities, component pools, the field tables,
> transform/hierarchy propagation AND the full `scene_abi.h` surface (field resolution, typed
> get/set, create/destroy/parent, name/objectId, material intern) are implemented and exercised by
> `SceneTest.exe` (`tests/scene`) — so the C# layer can now reach the world. Internal bookkeeping
> fields (name-blob cursors, `CWorld` derived data, hierarchy links) are read-only over the generic
> ABI. What is still missing is the gameplay framework on top (steps 8–11): spawning, ticking,
> possession. The full design is [docs/SCENE_FRAMEWORK.md](../../docs/SCENE_FRAMEWORK.md); the module
> DAG is [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).

## What is here

| Header | What it owns |
| --- | --- |
| `Entity.hpp` | The handle: 24 bits of index, 7 of generation, bit 31 always clear |
| `ComponentPool.hpp` | One sparse set per component type |
| `Fields.hpp` | Field kinds and the `ComponentBuilder` that declares a table |
| `Components.hpp` | The built-in components and their fixed dense ids — **15** now (`CLocal`, `CWorld`, `CHierarchy`, `CName`, `CTags`, `CMeshRenderer`, `CLight`, `CCamera`, `CSkeletalMesh`, `CAnimator`, `CParticleEmitter`, `CAttachment`, `CSoftBody`, `CRigidBody`, `CJoint`), up from the original eight — `scene_abi.h` still only names fixed `AVER_SCENE_COMP_*` constants for the first twelve; the three physics ones are appended the same way but resolved by name/qualified field id rather than a published constant |
| `World.hpp` | Lifetime, the registry, the hierarchy and the propagation pass |

**Index 0 is never handed out and a live generation starts at 1**, so no legal handle can encode to
0 and a default-constructed `Entity` is invalid. Bit 31 stays clear so the same value crosses the C
ABI as a positive `int32_t` and can never be confused with an error return. Seven generation bits is
few, so a slot whose generation would wrap past 127 is **retired** rather than recycled — an aliased
handle that silently addresses the wrong entity is the failure the whole packing exists to prevent,
and `World::retiredSlotCount()` exists so a world churning hard enough to retire slots in bulk is
visible rather than merely slow.

`ComponentPool`'s sparse array stores **dense slot + 1**, so 0 means "absent" inside the storage too
rather than only at the handle.

Destruction is **deferred to `flush()`** and takes the whole subtree with it: an entity destroyed
inside a tick must stay valid for the rest of that tick, because the layer above sweeps its own
instance lists with `valid()` instead of taking a callback downward.

## Transforms

Centimetres, +Z up, +X forward, +Y right, **left-handed, row-major with row vectors**. Composition is
therefore left to right and **translation lives in the last row**:

```cpp
world = local.toMatrix() * parentWorld;   // v * (L * P)
```

`CLocal` is authored data; `CWorld` is derived data exactly one pass writes. Staleness is a revision
compare rather than a dirty bit, because a child's staleness is a question about its *parent's* last
change and a bit that one reader has already consumed cannot answer it. The topological order is
rebuilt only when a parent link moves. `World::worldMatrix()` composes on demand so a caller reading
mid-frame gets exactly what the next `flush()` would have written.

## Field tables

Every component registers a hand-written table of `(name, kind, offsetof)` next to its struct,
terminated by `.verify(sizeof(T))`. One table serves the generic get/set ABI, save/load and the
editor's Details panel, so there is no second place to update and therefore no second place to
forget. `verify()` refuses a table that leaves an interior gap, an overlap, or more trailing bytes
than the struct's own alignment — a missing member — and names the component when it does.

## What is deliberately NOT here

No actor, no pawn, no possession, no begin-play. Those are gameplay vocabulary and they live in
[`modules/framework`](../framework/README.md), one module up. `scene_abi.h` can be read end to end
without meeting any of them, and that is the point: "use the world without the gameplay layer" is a
question this split answers on a link line rather than by discipline.

The division is the one the tree already draws between `Aver.Render.PBR` (what a material *is*) and
`Aver.Render.Voxi` (the thing that renders it). Storage does not know what its data means.

## Why SHARED

The same reason `Aver.Render.PBR` is: the C# scripting layer P/Invokes this DLL, and keeping the
world in **one** binary means the editor and the bindings address the same entities. Two static
copies would each hold a world that looked correct in isolation — the worst shape a bug of this kind
can take, because nothing about it looks wrong until two subsystems disagree about an entity that
both of them can see.

It links **Core and Assets only**. The moment `aver/rhi/*` appears behind this target the scripting
boundary is broken, so the GPU-facing half will be a separate STATIC target (`Aver.Scene.Renderer`)
exactly as `Aver.Render.PBR.Materials` is.

## Versioning

`aver_scene_abi_version()` returns `(major << 16) | minor`, compiled **into** the DLL so it reports
what that binary was built with rather than what the caller's header says. Major changes when an
entry point changes shape; minor changes when entry points are only added, so an older binding still
works and checks `minor >= what it needs`.

This is a **third** version boundary, separate from the two `scripting_abi.h` documents (the
host↔bridge contract, and the `Aver.Scripting` assembly version). It is separate because this DLL
can be replaced without either of those moving, and the failure is a wrong pointer rather than a
missing method.
