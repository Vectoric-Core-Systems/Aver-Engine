# Projected decals

A decal is a box that repaints the surfaces inside it: base colour, normal and roughness/metallic,
from up to three images. It is a scene component (`CDecal`), authored in the editor with a box gizmo,
saved in the level, and spawned at run time from a pool for impacts, footprints and scorch marks.
The renderer applies decals to the surface **before** it is lit, so the staged ray-driven path and the
raster path both light the repainted surface, and reflections and GI bounces that hit it see it too.

This page is the design, the data, the cost and what is not done. The plan it came from is
[GAME_SYSTEMS_PLAN.md](../plans/GAME_SYSTEMS_PLAN.md) (feature 7).

## The projector

The box projects along the entity's local **+X** (the same direction a light emits). Local **Y** is the
image's u axis (right) and local **Z** its v axis (up), so a decal placed on a wall with the entity's
identity rotation shows its image upright. The entity's scale scales the box. `sizeCm` is the full size:
x depth (how far the projection reaches), y width, z height.

What a surface gets, per pixel inside the box:

```
weight = softEdge(box) * angleFade(surface normal vs projector) * opacity * image alpha
base colour:  lerp(surface, tint * baseImage.rgb, weight)
roughness:    lerp(surface, roughness * orm.g,    weight)      metallic likewise with orm.b
normal:       lerp(surface normal, decal normal,  weight)
```

* **Soft edge** fades over `edgeFade` (default 0.1) of the half extent on every face, so an untextured
  decal does not end in a hard rectangle. A textured decal normally has transparent borders and is
  unaffected.
* **Angle fade** compares the *geometric* normal with the way the decal looks back: full strength up to
  `angleFadeStartDeg` (60), nothing past `angleFadeEndDeg` (85). It stops a decal smearing along a wall
  that runs parallel to the projection and stops it painting surfaces that face away.
* **Normal frame**: the normal image's tangent follows the decal's u axis projected onto the surface and
  its bitangent the v-up axis, so the map reads correctly on a tilted surface, not only a square one.
* **Order**: decals paint in `sortOrder` order, lower first; at equal order the farther one first, so the
  nearer lands on top.
* **Distance fade**: `fadeDistanceCm` fades a decal to nothing with distance (full strength to 60 percent
  of it). Gameplay decals use it so a thousand bullet holes cost nothing past the room you are in.
* **Lifetime**: `lifetimeSec` and `fadeOutSec` (pooled decals) fold into the opacity on the CPU.

Images (ObjectId of a content-relative path, like light cookies): the **base colour** image is sRGB with
coverage in alpha, which also gates the normal and roughness channels; the **normal** image is a
tangent-space normal map (+Y up in the image); the **ORM** image is linear with roughness in green and
metallic in blue (the glTF packing). Any of the three may be absent. A decal with no base image paints
a flat `tint`; with no ORM image `roughness`/`metallic` apply as plain values, and a decal that sets
neither leaves the surface's roughness alone.

## The component

`CDecal`, built-in component id **16** (appended; ids 1-15 do not move). Field table in
`modules/scene/src/Builtins.cpp`, readers in `modules/scene/include/aver/scene/DecalGather.hpp`.

Zero means "unset" for every field whose sane default is not zero, because a component attached with
`addComponent` is zero-filled and a struct's initialisers never run (the same rule as `CSoftBody`):
size 100 cm, tint white, normal strength 1, edge fade 0.1, angle fade 60 to 85, uv scale 1. The flags
are negative-sense for the same reason (`kDecalNoColour`, `kDecalNoNormal`, `kDecalNoRoughness`,
`kDecalDisabled`): zero is an active decal painting every channel it has data for. The accepted
ambiguity is a genuinely black tint or perfectly smooth roughness, which need a tiny non-zero value.
`age` and `serial` are read-only over the generic ABI and never saved.

## Rendering

`modules/render.voxi`:

| Piece | Where |
|---|---|
| Host-facing `SceneDecal`, the 192-byte `PackedDecal`, packing, CPU mirror of the projector, mip builder | `include/aver/voxi/SceneDecal.hpp` |
| The shader | `shaders/voxi_decal.hlsli` (`averApplyDecals`) |
| Raster hook | `PSMainVoxi` in `voxi.hlsl`, right after `averEvalMaterial` |
| Ray-hit hook | `rtHitSurface` in `voxi_rt.hlsli`, right after `averComposeSurface` |
| List upload, culling, sorting, image residency | `VoxiRenderer::buildDecals` (called from `prePass`) |
| API | `setSceneDecals`, `registerDecalTexture` on `VoxiRenderer` |

**Where it applies.** One function, two call sites. `rtHitSurface` is the one place every ray hit builds
its surface, so a decal shows up in the staged ray-driven primary surface (Stage B), the reflection
filter's roughness gate, reflection hits, ReSTIR GI candidate hits and Voxi's own Path Tracing vertices
from that one hook. Raster draws take it in `PSMainVoxi`. The G-buffer packs the repainted normal and
roughness, so the denoisers see what was shaded.

**Data path.** The host calls `setSceneDecals` once a frame (a copy) and `registerDecalTexture` the first
time it meets an image. `prePass` culls by distance fade, packs each decal *relative to the eye* (the 3x3
is inverted in double and the translation folded in, so a decal kilometres from the origin does not
jitter), keeps the 128 nearest-and-largest, sorts into paint order, and uploads to a structured buffer
bound at **t24** through the same eight-deep ring the local-light list uses. The count goes in
`gDecalParams.x` (a new last float4 of `VoxiFrame`; 736 to 752 bytes, mirrored in `voxi.hlsl`,
`voxi_gi.hlsli` and `FrameConstants`). Images upload on first use with a CPU-built mip chain
(alpha-weighted for colour, renormalised for normals) and become resident in the existing ray-path
bindless table; the CPU copy is then dropped.

**Cost.** With no decals `gDecalParams.x` is 0 and both call sites skip on a uniform branch; the list
bind stays on a one-record placeholder and nothing is uploaded. With decals, every shaded pixel tests
each one against its box first (about 20 ALU) and only decals that contain the point sample images, so
cost scales with the decals *touching* a pixel more than with the list length. The 128 cap bounds the
worst case. The code adds registers to the two kernels that hold it; check `--gpu-timing` with and
without decals when this is first run on hardware (see Not done).

## Gameplay decals (the pool)

`aver/scene/DecalPool.hpp` (header-only over `World`): a fixed-size pool of `CDecal` entities that are
re-armed, never destroyed.

```cpp
scene::DecalPool pool(256);
scene::DecalSpawn s;
s.position = hit.point;
s.rotation = scene::decalRotationForSurface(hit.normal, randomRoll);   // looks INTO the surface
s.params.sizeCm[0] = 30; s.params.sizeCm[1] = s.params.sizeCm[2] = 25;
s.params.baseTexture = fnv1a64("Decals/bullet_hole.png");
s.params.lifetimeSec = 20; s.params.fadeOutSec = 4; s.params.fadeDistanceCm = 3000;
pool.spawn(world, s);
pool.tick(world, dt);                                                   // once a frame, in play
```

* Entities are created on first need and then reused: after the pool is warm spawning allocates nothing.
* When every slot is in use the **oldest** visible decal is recycled (`recycledCount()` says how often).
* A released or expired decal is disabled and its slot is reused before anything is recycled.
* Pooled decals carry `kDecalPooled` and are never written to the level.
* If the world retires a pooled entity underneath (a level unload) the pool forgets the slot.

C ABI (`modules/scene/include/aver/scene/decal_abi.h`, in `Aver.Scene.dll`): `aver_decal_pool_set_capacity`,
`aver_decal_spawn(const AverDecalSpawnDesc*)` (a surface normal or an explicit rotation),
`aver_decal_release`, `aver_decal_clear`, `aver_decal_tick`, `aver_decal_active_count`. C#:
`Aver.Scene.Decals` (`scripting/csharp/Aver.Scene/Decals.cs`): `Decals.Spawn(DecalDesc.OnSurface(pos, normal, size) with {...})`.

Visual-script nodes (not yet registered; the exact entries are in "Wiring" below): `AN_SpawnDecal`
(position, normal, size, images, lifetime), `AN_ClearDecals`, `AN_SetDecalCapacity`.

## The level format

A `DECAL` record per authored decal, in `.ocworld`, like `LIGHT`:

```
DECAL name Wall%20stain pos 120.5 -40 75 rot 30 -15 90 size 60 120 80 base Decals/stain.png edge 0.1 angle 60 85
```

Optional tokens, written only when not at their default: `scale sx sy sz`, `tint r g b`, `opacity`,
`normalstrength`, `roughness`, `metallic`, `fadedist cm`, `order n`, `normalmap path`, `orm path`,
`uvscale u v`, `uvoffset u v`, and the bare `nocolour`, `nonormal`, `noroughness`, `disabled`. Always
written: `pos`, `rot`, `size`, `edge`, `angle`. A level with no decals writes no record and loads and
writes exactly as before; an older parser skips the whole record. Lifetime is not carried: an authored
decal is permanent. Format code: `OcDecal` and `OcWorldData::decals` in
`modules/formats/include/aver/formats/OcWorld.hpp`.

## The editor

New files under `sandbox/src/`: `DecalDetails.hpp` (the Details section and `makeNewDecal`),
`DecalLevelIo.hpp` (record <-> component, load and save, "Add > Decal"), `DecalGizmoMath.hpp` and
`DecalGizmo.hpp` (the box, the direction arrow, six face handles that resize it symmetrically, a ray-vs-box
pick because a decal draws nothing a mesh pick can hit), `DecalAssets.hpp` (image id to file, decode). The
host-neutral `Runtime/include/aver/game/SceneDecalFeed.hpp` feeds the renderer for either host. The
shared-file wiring is listed below.

## Wiring (shared files, done by the integration step)

1. `modules/scene/CMakeLists.txt`: add `src/DecalAbi.cpp` to the `Aver.Scene` sources.
2. Root `CMakeLists.txt`, inside `if(AVER_BUILD_TESTS)`: `if(TARGET Aver.Scene AND TARGET Aver.Formats) add_subdirectory(tests/decals) endif()`.
3. Re-run CMake so the new `voxi_decal.hlsli` is globbed and deployed (`aver_deploy_shaders` uses `CONFIGURE_DEPENDS`).
4. `SandboxApp.hpp`: `#include "DecalLevelIo.hpp"`, `"DecalGizmo.hpp"`, `"DecalAssets.hpp"`; members `aver::game::SceneDecalFeed sceneDecalFeed_; editor::DecalImageResolver decalImages_; editor::DecalGizmo decalGizmo_; bool showDecals_ = true;`.
5. `SandboxRender.cpp`, beside `sceneLightFeed_.update(...)`: `decalImages_.setContentDir(project_.contentDir()); sceneDecalFeed_.update(scene::World::instance(), voxiRenderer_, decalImages_.loader());` and, in the overlay stage next to `drawGizmo`: `decalGizmo_.draw(*e.device(), scene::World::instance(), selEntity_, showDecals_);`.
6. `SandboxLevelLoad.cpp` `onLevelInstantiated`: after the LIGHT loop, `for (scene::Entity d : editor::spawnDecalsFromRecords(lw, w.decals)) entityLabels_[static_cast<u32>(d)] = lw.name(d);`. `loadLevel`/`unloadLevel`: clear `levelHeader_.decals` and destroy decals by the `CDecal` pool, as for lights.
7. `SandboxLevelEdit.cpp` `saveLevel`, beside the LIGHT block: `w.decals = editor::recordsFromDecalEntities(world, editor::LightAssetPaths::scan(project_.contentDir()));` and skip decal entities in the placement slot loop the way `CLight`-only entities are skipped.
8. `SandboxPanels.cpp`, after the Light section: `if (auto* dc = w.component<scene::CDecal>(selEntity_, scene::kComponentDecal)) if (ImGui::CollapsingHeader("Decal", ImGuiTreeNodeFlags_DefaultOpen)) if (editor::drawDecalDetails(*dc, project_.contentDir())) markLevelUnsaved();` and an `Addable` row `{scene::kComponentDecal, "Decal", ...}` that seeds `editor::makeNewDecal()`.
9. `SandboxShell.cpp`: Add menu "Decal" calling a new `SandboxApp::spawnDecalAtCamera()` (copy `spawnLightAtCamera`, using `editor::createEditorDecal`), and Show menu "Decals" toggling `showDecals_`.
10. Viewport picking (`SandboxViewport.cpp`/`SandboxSelection.cpp`): test `editor::rayHitsDecalBox(origin, dir, editor::decalBoxOf(c, world.worldMatrix(e)), t)` for every `CDecal` in the same pass that picks meshes; on left-down with a decal selected call `decalGizmo_.beginDrag(world, e, origin, dir, pixelWorldCm)` first and skip the pick when it returns true; `updateDrag` while held (mark the level unsaved when it returns true), `endDrag` on release.
11. Play: call `pool.tick(world, dt)` or `Decals.Tick(dt)` once per game frame while playing, not while editing. Hosts that run C# gameplay call `aver_decal_pool_set_capacity` from the project's settings if it is not 256.
12. `Runtime` host: same feed call as step 5 with a loader that reads the packaged content (the editor's `DecalImageResolver` is the model).
13. Graph nodes (`OcGraphParser.AddDefaultPins`, `GraphCompiler`, `GraphNodeDefs.hpp`): `SpawnDecal` (exec in, vec3 position, vec3 normal, vec3 size, float roll, string base/normal/orm image paths, float lifetime, float fade-out, exec out, int entity) compiling to `Aver.Scene.Decals.Spawn`; `ClearDecals` (exec) to `Decals.Clear`; `SetDecalCapacity` (exec, int) to `Decals.SetCapacity`. Image paths hash with `Assets.ObjectIdOf`.
14. `docs/ARCHITECTURE.md` and `modules/scene/README.md`: the scene module now has **16** built-in components (`CDecal` added), `VoxiFrame` is 752 bytes, `t24` is the decal list, and `Aver.Scene.dll` exports the `aver_decal_*` ABI. `scene_abi.h` is unchanged (the decal ABI has its own header).
15. `tests/render.voxi`'s `VoxiShaderCompileTest` compiles the new shader code with the Voxi variants; run it after the build.

## Tests (`tests/decals`)

| Test | What it decides |
|---|---|
| `DecalProjectorTest` | Box test, soft edge, angle fade, uv orientation, the normal frame, rotation and scale, eye-relative precision at 2 km, packing refusals and flags, distance fade and paint order, the mip chain, and the HLSL source against the C++ record layout, flag bits, register `t24`, both cbuffer mirrors and both hooks. |
| `DecalPoolTest` | The component's field table, zero-means-default readers, the gather, lifetimes, surface pose, and the pool: recycles the oldest, no entity churn once warm, release, expiry, clear, a world that retires entities underneath. |
| `DecalAbiTest` | The `aver_decal_*` ABI. |
| `DecalFormatTest` | The `DECAL` record round trip and omissions, a level without decals unchanged, the editor's component <-> record conversion and load/save through the world. |
| `DecalGizmoTest` | The box, ray pick, wireframe, handle pick and symmetric drag. |

None of them needs a window or a GPU. They were written without being run (the build and test pass is the
integration step's).

## Not done / known limits

* **Never run.** No engine run, shader compile or build happened while this was written. Expect to fix
  compile errors first; `VoxiShaderCompileTest` covers the HLSL.
* **Register pressure** in Stage B and the reflection kernel is unmeasured. The single-pass kernel
  (`AVER_RD_SINGLE_PASS`) skips decals on ray hits entirely because it already sits at its register
  limit; it still gets them on raster draws.
* **Textured decals need the bindless texture table** (a ray-tracing-capable device). Without it flat
  decals still draw and textured ones are skipped, not drawn as a box.
* **Not seen by**: the GI voxel injection (decals do not tint bounced light), the shadow maps, the
  separate `render.pt` reference path tracer (Voxi's own Path Tracing mode does see them), translucent
  and transmissive materials, unlit materials, and LITE secondary ray hits.
* **One role per image id per decal slot** is handled by salting the renderer's id with the role, so one
  file may be both a colour and a data image; two different decals sharing a file share one upload.
* **No emissive channel** and no per-decal blend modes beyond opacity; a decal cannot cut holes.
* **128 decals per frame** (nearest and largest kept); the per-pixel loop is linear in the list. A tiled
  or clustered list is the next step if scenes need hundreds on screen at once.
* The editor's Details section has no thumbnail previews of the three images.

## Where decals are compiled in (2026-10-06)

Decals are applied in the plain raster `PSMainVoxi` (no `AVER_RT`) and in `rtHitSurface` for the bindless
ray-traced passes: the staged ray-driven primary (Stage B) and its GI, reflection and Path Tracing hits.

They are **left out** of:
- the non-bindless ray-traced raster variants (`sceneRtPso_`, `sceneRtGbufPso_`);
- the blended glass variant (`AVER_BLENDED_PASS`);
- the single-pass kernel.

Why: with the decal code compiled into the ray-traced `PSMainVoxi` variants, by either call site, NewSponza
Night's default view hung the RX 7800 XT (TDR, in a mesh-shader draw per DRED) in every denoiser mode. The
same hang came from the exact rectangle-light form factor earlier the same day. These shaders are at the
limit; test the default view in all three denoiser modes after adding code to them.

Consequence: with raster primary visibility and ray tracing on (`voxi.rtRenderMode 0`), primary surfaces
show no decals; the ray-driven primary (the default) does.

