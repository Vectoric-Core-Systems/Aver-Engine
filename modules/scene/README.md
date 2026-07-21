# Aver.Scene  (`modules/scene`)

- **Language:** C++
- **Depends on:** Core, Assets
- **Built:** SHARED (`Aver.Scene.dll`), because the C# layer P/Invokes it

Data-oriented entity/component world: entities, component storage, transforms, hierarchy. Render-
and physics-agnostic, and **no UObject** — there is no base class every game object inherits from,
because the storage this module exists to provide is dense arrays of plain data and an inheritance
tree is the shape that makes them impossible.

> **Status: skeleton.** Only `aver_scene_abi_version()` exists so far. The module is wired into the
> build and its DLL topology is proven (see below), but the world itself is not implemented.
> The full design is [docs/SCENE_FRAMEWORK.md](../../docs/SCENE_FRAMEWORK.md); the module DAG is
> [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).

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
