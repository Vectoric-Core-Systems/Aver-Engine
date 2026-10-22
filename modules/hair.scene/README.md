# Aver.Hair.Scene  (`modules/hair.scene`)

- **Language:** C++
- **Depends on:** Core, Formats, Scene, Anim, Anim.Scene, Physics (C ABI), Hair
- **Option:** `AVER_MODULE_HAIR_SCENE` (ON; force-disabled without the scene and physics)

Strand hair on scene entities: `CHair` names an `.ochair` groom on a character's root entity and `HairSystem` does the rest. The pure half (the groom
asset, the generator, curve import, the settings builder, the distance rules in `HairLod.hpp`) is `Aver.Hair`; the simulation is Jolt's `Hair`, inside
`Aver.Physics`; the drawing is `Aver.Render.Hair`. Design: `docs/plans/wave11/SOFTBODY_HAIR.md` 2.15; everything about the feature: `docs/HAIR.md`.

## What `HairSystem::tick` does, per frame

1. Builds a groom the first time a component names it (the `.ochair`, its scalp mesh, `aver_phys_hair_settings_*`) and shares the settings between
   every instance of the same asset. A build that fails is logged once and latched until the asset changes; one that fails because the physics world
   does not exist yet is retried.
2. Feeds each hair: the owner's world matrix, the skinning matrices of the scalp's joints (the same palette the skinning pass uses, in the groom's
   `SCALPJOINTS` order), the world wind scaled by the component.
3. Decides how often to simulate it from its distance (`stepHairCadence`): every frame near, every second frame in the middle band, not at all beyond
   `cadence.farCm` or when unseen for two seconds; a groom that resumes after more than a second restarts from its default pose.
4. Moves the kinematic hull proxies (head, neck, shoulders, spine; the groom's own `PROXY` records, else a default set found by joint name) to their
   bones. Jolt's hair collides with convex hulls only.
5. Seals the step with one `aver_phys_hair_flush` and publishes a `HairView` per instance for the draw side.

## The compute host

`installComputeHost(table)` is the one call that decides where the simulation runs, and it must precede the first build: a groom built on Jolt's CPU
backend stays there. The composition root passes `HairScene::hostTable()` (Aver.Render.Hair) when it has a GPU, or null for the CPU backend (small
grooms; tests, a headless server).

## The draw child

The component sits on the skeleton's entity. The strands are drawn by a child entity named `Hair` (identity local transform, a placeholder cube mesh,
the groom's look material, a `CHairDraw` marker naming its owner) whose mesh the render side substitutes. A saved level carries the child; on load it
is found again through the marker, never made twice. It starts invisible; `HairScene` is the only thing that turns it on.

## The framework relay, script colliders and the look override

`hair::hairProvider` (`src/HairProvider.cpp`) is the provider behind `framework_hair_abi.h`: teleport, add and clear script colliders, and a state read.
The exported `aver_fw_hair_*` functions are `framework/FrameworkHairAbi.cpp`, compiled into `Aver.Framework` (its CMake names the file by path; this
module's own sources never include the framework). A script collider is a kinematic hull on the proxy layer that follows a bone of the owner's skeleton;
the list is kept per entity, so a rebuilt groom gets it again. `CHair.material` (an `.ocmat`) replaces the groom's generated look on the draw child's
`CMeshRenderer` every tick. For the editor: `guideSegments(entity)` is a sample of the groom's strands at rest and `debugProxies(entity)` the proxies at
their live pose (`sandbox/src/HairDebugOverlay.hpp`).

## Not here

The ray-tracing and shadow policy of the draw (`Aver.Render.Hair::HairScene`, `GameRender.cpp`), the Groom tab and the Hair nodes (`sandbox/src`,
`scripting/csharp`).
