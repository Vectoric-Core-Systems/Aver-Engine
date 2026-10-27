# Code notes: hair.scene

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/hair.scene/include/aver/hair/HairSystem.hpp

- banner: docs/plans/wave11/SOFTBODY_HAIR.md 2.15 and docs/HAIR.md.
- banner WHAT IT OWNS: a CHair on a character's root entity names an .ochair groom. The system builds the groom into Jolt's hair settings (Aver.Hair's buildHairSettings, which drives the aver_phys_hair_* ABI). It shares those settings between every instance of the same groom and creates one physics hair per component. It feeds each tick (the head's transform, the skinning matrices of the scalp's joints, the world wind). It decides how often a groom is simulated by distance. It keeps kinematic convex-hull proxies at the head, neck and shoulders for the strands to collide with. It publishes one HairView per drawn instance for Aver.Render.Hair. The simulation is Jolt's, in Aver.Physics; this class never sees a Jolt type.
- banner THE DRAW CHILD: the component sits on the entity that owns the skeleton, but the strands are drawn by an ordinary mesh draw whose vertices the ribbon pass writes. The system makes (or re-adopts) a CHILD entity named "Hair" with an identity local transform, a CMeshRenderer on the engine cube as a placeholder mesh and the groom's look material, and a CHairDraw marker that names its owner. The draw side substitutes the ribbon mesh for the placeholder (the SkinnedScene / SoftBodyScene seam). Because the child's world matrix equals its owner's, the ribbons are written in the owner's model space (the space the scalp mesh is in), and the draw applies the world matrix like any other mesh. A saved level carries the child; on load it is found again through its CHairDraw rather than made twice.
- banner REGISTRATION is dynamic, the CCloth pattern. Attach through attachHair, never world.addComponent directly (storage arrives zero-filled and runs no constructor; zero in every scale field reads 1).
- banner ORDER OF A FRAME: animSystem().tick, aver_phys_step, World::flush, then tick() (it records the hair updates and seals them with one aver_phys_hair_flush), then the render side's update and prePass (which replay the recorded steps on the GPU).
- banner COMPUTE HOST: must be decided before the first groom is built. aver_phys_hair_set_compute_host is refused once any settings exist, and a groom built on Jolt's CPU backend stays there. installComputeHost() is the one call that does it. A host with a GPU hands in the table of Aver.Render.Hair's RhiComputeHost; a headless one hands in null (the CPU backend, for grooms of cpuMaxSimStrands or fewer simulated strands).
- kHairRayTraced: default off; strands are in the ray-tracing structure (HairScene::inRayTracing).
- kHairFrozen: beyond the cadence's far distance, or not seen for a while. kHairInert: the build failed or the device has no compute; the component does nothing.
- CHair.material: RESERVED. An .ocmat override of the groom's LOOK; the look comes from the groom's LOOK record today.
- CHair scale fields (widthScale, windScale, stiffnessScale) and densityScale: 0 reads 1. densityScale scales the fraction of the render strands drawn. stiffnessScale divides the bend and stretch compliance of every material.
- CHair.lodBias: added to the LOD level, in steps.
- CHair.teleportCm: 0 reads HairOptions::teleportCm. A root that moves further than this in one frame restarts from the default pose.
- CHair.outState: OUT kHair* bits. outRenderStrands: OUT, strands drawn. outSimStrands: OUT.
- CHairDraw: the marker. owner is the entity carrying the CHair.
- HairOptions.hairLayer 13: the layer hair collides as; the proxies are on proxyLayer and touch only this one.
- HairOptions.proxyLayer 12. freeLayer 11: a hair with kHairNoCollision collides as this layer, which touches nothing.
- HairOptions.hideBeyondCm 6000: the draw child is hidden past this distance from the view.
- HairOptions.vramBudgetMB 96: a groom whose draw buffers would exceed this is refused at build.
- HairOptions.cpuMaxSimStrands 64: the Jolt CPU backend is for tests; larger grooms are refused on it.
- HairLookView: the look of a groom, render only (the LOOK record of its first material).
- HairView: everything the draw side needs of one drawn instance. The pointers belong to the system's shared per-groom data and stay valid until the next tick().
- HairView.entity: the owner. drawEntity: the child whose mesh is substituted. key: instances of one key share static GPU data.
- HairView.renderBuffer: host handle of the render positions (0 on the CPU backend). strandTable: {first render vertex, count} per render strand. widthRootCm / widthTipCm: per render strand.
- HairView.world: hair space to world, the owner's world matrix.
- HairView.lodLevel: the strand-count level to draw (HairLod.hpp), hysteresis and lodBias applied.
- HairView.materialToken: the interned surface name the draw child's material is.
- HairView.visible: false means hidden by flag or distance; do not draw or expand.
- HairView.settled: the simulation has recorded at least one step, so the render positions mean something.
- HairView.estimatedBytes: the draw buffers' size, for the budget.
- registerComponents: registers CHair and CHairDraw (idempotent) and returns CHair's type id.
- attachHair: attaches a CHair with defaults over the zero-filled bytes. Null when the entity is invalid or already carries one.
- PathFn: the content-id to absolute-path resolver the host gives the animation system. An .ochair and its scalp mesh are found through it.
- installComputeHost: `table` may be null for Jolt's CPU backend. False when the physics layer refuses it (a groom already exists). Call once, before the first tick, on the composition root's thread.
- setWind: the world wind the soft bodies feel, forwarded by the host where it already lands (SoftBodies.SetWind). cm/s, and a gust amplitude in cm/s.
- tick: once per frame, after the pose, the physics step and World::flush. `view` is the camera position in world centimetres.
- clear: destroys every physics hair, proxy and settings object this system made; the components and entities stay. Call before the physics world goes, and when a level is unloaded or Play stops.
- views: the drawn instances of the last tick. Empty with no compute (an inert system draws nothing).
- teleport: the default pose at the next update.
- rebase: floating-origin shift (LARGE_WORLD 2.2.5). Subtracts deltaCm from each instance's previous root position; otherwise the teleportCm rule sees one 16 km move and restarts the groom from its default pose. Call after World::shiftRoots.
- hairOf: the physics hair handle, 0 when none.
- addCollider: a script hull proxy (Entity.AddHairCollider): a kinematic convex hull on the proxy layer that follows `spec.bone` of the owner's skeleton, so a prop or a held object can catch hair. Applied at the next tick once the groom is built (and again if it is rebuilt). False when the entity has no CHair or the record has no size. The groom's own proxies are not touched.
- clearColliders: removes every collider a script added to this entity. False when there was none.
- colliderCount: how many script colliders this entity asked for (including ones still waiting for the build).
- StateOut: for scripts and tools, the component's OUT fields plus whether the groom is built and how many script colliders exist. outState: kHair* bits, level from kHairLodShift.
- guideSegments: the groom's guide strands at rest, hair space, centimetres: a sample of the strands as 2 points (x y z, x y z) per segment. Null before the groom is built. Valid until the next tick.
- DebugProxy.world: the hull body's pose.
- debugProxies: the collision proxies of an instance at their live pose; returns how many were appended.
- "for the editor overlay (Show > Hair)": the guideSegments / DebugProxy / debugProxies group is for the editor overlay.
- materialNameOf: the name of the surface a groom's draw child is interned under.
- "the pure parts, public so a test reaches them": materialNameOf is public so tests can reach it.
- hairProvider: the framework relay's provider (framework_hair_abi.h, aver_fw_hair_fn). `user` is the HairSystem. The signature is spelled without the framework header because this module does not link Aver.Framework. The composition root casts it to the pointer type, as it does the soft body providers. Ops apply at once (a teleport and a collider are between-step calls). The op codes are in framework_hair_abi.h.
- "ops (the framework relay calls these)": the section banner above teleport. It covers teleport, rebase, hairOf, isBuilt, addCollider, clearColliders, colliderCount and stateOf.
- HairStats: no comments.
