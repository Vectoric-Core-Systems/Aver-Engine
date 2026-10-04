# Aver.Framework  (`modules/framework`)

- **Language:** C++
- **Depends on:** Core, Assets, **Scene**
- **Built:** SHARED (`Aver.Framework.dll`), because the C# layer P/Invokes it

The gameplay vocabulary: game instance, game mode, actors, pawns, player controllers — expressed
over [`Aver.Scene`](../scene/README.md)'s entities rather than as a class hierarchy.

> **Status: no longer a skeleton.** This used to say only the two version entry points existed. The
> ABI has since grown to 85 entry points at minor version 5 — class registry and defaults, spawn/
> destroy (and the preview pair), possession, the play lifecycle, input (keys, actions, raw VK,
> gamepad), the play view, sky-cloud and fluid-spawn authoring, and save/load providers — see
> [docs/ABI.md](../../docs/ABI.md) §4 for the current surface. The full design is
> [docs/SCENE_FRAMEWORK.md](../../docs/SCENE_FRAMEWORK.md).

## The idea

Unreal spells these concepts as a `UObject` inheritance tree. This engine cannot, because
`modules/scene/README.md` commits the world to dense arrays of plain data and an inheritance tree is
the shape that makes those impossible.

So the concepts become **data** instead of base types:

- an **actor** is an entity carrying a particular set of components, not an instance of `AActor`
- a **class** is a registry record describing which components an entity gets and with what
  defaults — declared by name, so a C# script can add one without a C++ edit
- **game instance** and **game mode** are singleton records, not objects with a vtable each

What this buys is that "spawn a `AN_Spinner`" stays a memcpy of a defaults blob into pools, while
the user-facing shape — class defaults, per-instance overrides, a game mode naming its default pawn
class — is the one someone arriving from Unreal expects.

## Why this is a second module and not more of the scene

Because `scene_abi.h` should be readable end to end without meeting the words *actor*, *pawn*,
*spawn*, *possess* or *play*. Keeping them here means "use `Aver.Scene` without the gameplay layer"
is answered by a link line rather than by discipline, and that separation is worth a second DLL.

The arrow points **down**: this module links `Aver.Scene`, never the reverse. In particular the
framework sweeps its own instance lists using the scene's validity check rather than registering a
destroy callback with it, because a callback would be an edge pointing back up. The cost is that an
end-of-play notification can arrive a frame after the entity went away.

## The SHARED-links-SHARED edge

This is the tree's first SHARED library that links another SHARED library, so the rule it is tested
against is worth naming: `modules/render.pbr/CMakeLists.txt` requires that **no RHI type sit behind
a P/Invoke DLL**. The transitive closure here is `{Core, Assets, Scene}` — no RHI is behind this
boundary, so the property that rule protects holds exactly.

Collapsing the two DLLs into one would satisfy a narrower reading of "a SHARED module depends on
Core only" while destroying the separation the split exists for. The build-shape risk was retired
first, before anything depended on the answer: `dumpbin /dependents Aver.Framework.dll` lists
`Aver.Scene.dll` and no RHI DLL.

## Two version numbers

`aver_fw_abi_version()` reports this module's own ABI. `aver_fw_scene_abi_version()` reports the
`Aver.Scene` version this binary was **compiled against** — deliberately the header constant, not a
call into whichever scene DLL is loaded, because that is precisely the mismatch it exists to detect.
The import library resolves by name, and every name still exists across a major bump, so nothing in
the loader catches this on its own.
