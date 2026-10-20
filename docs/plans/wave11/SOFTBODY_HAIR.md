# Soft bodies, deeper, and strand hair on Jolt Hair (wave 11)

Design contract for the implementing agents. Owner decisions in force: integrate the rest of Jolt Physics, large-world coordinates (`JPH_DOUBLE_PRECISION`) and Jolt 5.6 Hair are IN, Jolt ObjectStream and PhysicsScene files are OUT. Nothing here is built or run. Rules: upgrade the existing systems and never add parallel ones, no new third-party code beyond enabling code already vendored in Jolt, lean comments, D3D12 and Vulkan parity for GPU work, never delete files, ABI changes additive (new functions, new headers). Format and depth follow `docs/plans/wave10/VEHICLES.md` and `docs/plans/wave10/SOFTBODY_DEPTH.md`.

Every claim in section 1 was read in the tree on 2026-10-19 (HEAD `3000c220`) with a `file:line`; line numbers are pointers, grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it. Every tuning number is an UNVERIFIED starting value until the headed pass in section 6. The survey the task quoted was wrong in four places, listed in 1.7.

**Scope.** Part A (sections 2.1 to 2.8) adds the unwrapped Jolt soft-body features and the scene-level shapes a gameplay script wants: inflatables, jelly, ropes and cables, grab and release, contact policy, sensors, groups, buoyancy. Part B (2.9 to 2.21) is strand hair: the asset, Jolt's Hair simulation on a compute host over Aver's RHI, the scene component, ribbon rendering and a hair shading model in Voxi. Both parts share one phase plan, one ownership table and one test plan so they can be built by independent agents.

---

## 0. Decisions in one screen

**Soft bodies**

1. **One new ABI header, `physics_softbody_ext_abi.h` (`aver_phys_sbx_*`, `AVER_PHYS_SBX_ABI_VERSION 1`).** `physics_softbody_abi.h` (currently version 2) is not edited. The new header adds builder calls (tetrahedral volumes, long range attachments, Cosserat rods, Jolt's own `CreateConstraints` with per-vertex attributes and the distance bend type), live setters (pressure, skin constraint enable and distance scale, per-vertex inverse mass), attachments (grab and release by driving a kinematic vertex), contact policy (mass scales, per-pair ignore or sensor), sensor enter and exit events, a collision group, and soft-body buoyancy. A body that never calls one of them behaves exactly as today.
2. **Pressure is a live property in kilopascal at rest volume.** The ABI converts to Jolt's `n R T` with the rest volume it measures at `finish`; a refusal for an open mesh. An inflatable is therefore **a cloth with a closed mesh and a `pressureKPa`**: no new component, `buildSimMesh` and the cloth path are reused, `.ocsoft` `cloth` gains a section.
3. **Jelly is a new component `CSoftVolume`, ropes and cables are `CSoftRope`.** Both live in a companion class `SoftShapeSystem` inside `modules/softbody.scene` (not a new module), publish `PieceView`s through one new view-source seam on `SoftBodySystem`, and draw through the unchanged `Aver.Render.SoftBody`. Jelly simulates a voxel tetrahedral lattice and embeds the render mesh by barycentric coordinates (pure library `Lattice.hpp`). A rope simulates a chain of Jolt Cosserat rods and draws a generated tube (`Rope.hpp`).
4. **Rods need no faces.** Jolt's `SoftBodyShape` assumes at least one face (`SoftBodyShape.cpp:28`, `GetFaces().size() - 1`). A face-less body gets one zero-area face over its first three vertices, which no ray, cast or collision can hit. The refusal of an empty face list in `finish` (`PhysicsSoftBody.cpp:456`) is relaxed only for a body that stages rods, volumes or auto constraints.
5. **Grab and release never rebuild.** A grabbed vertex gets inverse mass 0 and, each fixed step, a velocity that carries it to its target (a kinematic vertex is moved by its velocity: `SoftBodyMotionProperties.cpp` `IntegratePositions` runs for every vertex, the velocity update at `ApplyCollisionConstraintsAndUpdateVelocities` skips kinematic ones). Release restores the saved inverse mass. Frame-rate independent because it runs in `softbody::onStepBegin`.
6. **Contact policy runs in the existing `ImpactListener`.** `OnSoftBodyContactValidate` already runs for every soft-versus-rigid pair; it gains mass scales, per-pair `ignore` (return `RejectContact`) and `sensor` (set `mIsSensor`). Sensor contacts are diffed once per step into enter and exit events (a new `softbody::onStepEnd` hook in the fixed loop, beside `vehicleStepEnd`).
7. **Soft-body buoyancy is ours, not Jolt's.** Jolt asserts on `ApplyBuoyancyImpulse` for soft bodies (`Body.cpp:198`) and `Buoyancy.cpp:20` skips them. The new fixed-step pass applies hydrostatic pressure per submerged face (closed meshes) plus quadratic drag (any mesh), against the world water plane or sampler already in `Buoyancy.hpp`, or against a plane given to the body.
8. **Jolt `CollisionGroup` on soft bodies** with one Aver-owned `GroupFilter`: a soft body ignores every rigid body whose group id equals its own. A ragdoll's group id is its handle (`PhysicsRagdoll.cpp:73,434`), so a cape ignores its wearer's ragdoll parts without a layer.
9. **Aver Node.** `SoftBody` family v3 adds 10 rows; new families `Rope` (6 rows) and `Hair` (8 rows). All follow the wave 9 C contract; `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` and `Program.cs` are not edited.

**Hair**

10. **Use `JPH::Hair` as shipped. Port nothing.** This reverses the 2026-10-11 recommendation in `HAIR_JOLT_EVALUATION.md` on the owner's instruction. The evaluation's two objections are answered by the architecture below: hair state is GPU buffers owned by the compute host; the module boundary holds because `Aver.Physics` gets a C function table as its compute seam (it links no RHI).
11. **Jolt's DX12 and Vulkan compute backends stay OFF; Aver implements `JPH::ComputeSystem` over its own RHI** (a host function table installed by `Aver.Render.Hair`, a `HostComputeSystem` adapter inside `Aver.Physics`). Jolt's CPU compute backend is turned ON in `modules/physics.jolt/CMakeLists.txt` (`JPH_USE_CPU_COMPUTE`). Why, with file:line, in 2.10. Short form: sharing the device gives Jolt a private command queue and fence it waits on, private resource-state tracking, on Vulkan the very same `VkQueue` as the renderer (`VulkanDevice.cpp:830-831` creates one queue), a shader build that needs the Windows SDK `dxc` and the Vulkan SDK that Aver deliberately does not need, and an output buffer the RHI cannot import. The host adapter has none of those problems and gives D3D12 and Vulkan parity from one code path.
12. **The sim shaders are Jolt's, compiled by Aver's own DXC pipeline.** Aver wrapper `.hlsl` files include the vendored `Jolt/Shaders/Hair*.hlsl` unchanged and override Jolt's documented `JPH_SHADER_OVERRIDE_MACROS` hook (`ShaderCore.h:5`) to give every buffer an explicit register. The registers are generated from Jolt's `*Bindings.h` by a script and pinned by a test. The same source compiles to DXIL and SPIR-V through the pipeline that already serves both backends.
13. **`.ochair` is a new Aver text asset** (`OCHAIR 1`): guide and render strands, a scalp mesh reference, materials, width, colour, generation records. Sources: authored text, a procedural generator from a scalp mesh (clump, length, curl, part, density), and glTF `LINES`/`LINE_STRIP` curves through the existing importer (`GltfImport.cpp:375-376` skips every non-triangle primitive today). USD `BasisCurves` is phase H (the USD path has no curve handling). **No Alembic**: it needs a new dependency.
14. **Hair is a scene component, `CHair`,** runtime-registered (the `CCloth` pattern), joined by `Aver.Hair.Scene` (`HairSystem`) and drawn by `Aver.Render.Hair` (`HairScene`, an `IRenderFeature`). The scalp is skinned by Jolt's own `HairSkinVertices`, fed the engine's skinning palette entries of the scalp's joints with an identity inverse bind (the engine palette already carries `model * inverseBind`).
15. **Strands draw as camera-facing ribbons** written by one Aver compute pass into a `rhi::MeshVertex` skin-target mesh (`createSkinTargetMesh`, `RHI.hpp:518`), four index-prefix LOD parts (`createPosedPartMesh`), drawn by the ordinary `drawMesh`. Sub-pixel strands keep a one-pixel ribbon and carry the lost width as dithered coverage that TAA resolves.
16. **Hair shading is a new shading model `AVER_MODEL_HAIR`** in the shared evaluator (`material_prelude.hlsl:137`), selected by a new material flag so neither backend's per-draw constants change. A clean-room Kajiya-Kay diffuse plus three Marschner-style lobes (R, TT, TRT) with Gaussian longitudinal terms and cheap azimuthal terms. Patent notes in 2.19: three live US patents were found and none is infringed by this design as read; that is a reading of abstracts and claim 1, not a legal opinion.
17. **Hair collision is Jolt's: convex hulls only** (`Hair.cpp:211,268`). Aver supplies kinematic hull proxies (head, neck, shoulders, torso) generated from sphere and capsule specs on a dedicated layer pair, moved from bone sockets. Wind, which Jolt Hair lacks (`Hair.h:27`), is an Aver compute pass over Jolt's public velocity buffer.
18. **Large-world coordinates.** Soft bodies simulate relative to their centre of mass and hair relative to the head, so only the boundary transforms see double precision (2.21). All new ABI positions are float world centimetres; a `_d` twin is added only if the LARGE_WORLD area introduces its double-entry convention.
19. **Size XL, eleven phases** (section 4). Part A is phases A to D; part B is E to K. Phases A and E carry the ABI risk; G (the compute host and shaders) is the riskiest single phase.

---

## 1. What exists

### 1.1 Soft-body physics today

| Fact | Where |
|---|---|
| Builder-style ABI v2: `begin`, `vertices`, `faces`, `edges`, `bends`, `skin`, `params` (12 entries), `finish`, `cancel`; in-place `set_edge_rest`, `rest_bends_from_pose`; `impulse`, `add_velocity`, `skin_world`, `set_active`, `set_params`, aero, wind, impact events | `physics_softbody_abi.h:31-175` |
| `finish` builds the shared settings by hand: vertices, faces wound straight through, one `Edge` per input edge (rest length from the caller), one `DihedralBend` per bend, skinned constraints. It never creates volumes, LRA, rods or pressure and never calls `CreateConstraints` | `PhysicsSoftBody.cpp:436-506` |
| `finish` refuses a body with no faces | `PhysicsSoftBody.cpp:456` |
| Params cover iterations, damping, friction, restitution, gravity scale, vertex radius, max speed, sleep, double sided, layer, entity, impacts. **No pressure, no skin enable, no skin distance scale, no collision group, no mass scales** | `physics_softbody_abi.h:47-59`, `PhysicsSoftBody.cpp:137-149` |
| `SoftRecord` (file-local) keeps the non-const shared settings Ref, params, palette, aero, velocity cache. It is erased with the body; `finish` with `replaceBody` carries params, aero and palette across | `PhysicsSoftBody.cpp:88-104,509-586` |
| The contact listener already exists, is installed at world init, and its validate callback returns `AcceptContact` unconditionally. It sets no `SoftBodyContactSettings` field | `PhysicsSoftBody.cpp:174-253`, `softbody::installListener` at `:263` |
| `onStepBegin` runs inside the fixed loop before `system.Update`: aero, the sleep override, the velocity cache | `PhysicsSoftBody.cpp:277-305`, called at `PhysicsWorld.cpp:487` |
| There is **no** after-update soft-body hook; vehicles have `vehicleStepEnd` | `PhysicsWorld.cpp:489-492` |
| The older `aver_phys_softbody_create(... pressure ...)` already applies pressure for the fluids module, through `CreateConstraints` with edges only and `mPressure` on creation settings. The builder path has no equivalent. Winding is wound straight through because Jolt's `ApplyPressure` opens with `six_volume > 0`, which an inside-out shell fails | `PhysicsWorld.cpp:1150-1220` |
| `Buoyancy.cpp` skips every non-rigid body ("SOFT BODIES ARE NOT BUOYANT") because `Body::ApplyBuoyancyImpulse` asserts `IsRigidBody()` | `Buoyancy.cpp:19-34`, Jolt `Body.cpp:198` |
| The water table exposes the global plane (`hasPlane`, `plane()`) and a host sampler (`waterSampler()`) publicly; per-body overrides are private | `Buoyancy.hpp:74-129` |
| Rigid bodies have **no** collision group API; only ragdoll parts carry a `GroupFilterTable`, group id = ragdoll handle | `PhysicsRagdoll.cpp:73,386-406,434-435` |
| Aver layers: 16 user layers with a symmetric matrix; soft bodies use `Layers::encode(layer, true)` | `physics_layers_abi.h:34-49`, `PhysicsSoftBody.cpp:147` |

### 1.2 What Jolt 5.6.0 offers that the wrapper does not use (read in `modules/physics.jolt/Jolt/Physics/SoftBody`)

| Jolt feature | Facts that shape the ABI |
|---|---|
| **Pressure** | `SoftBodyCreationSettings::mPressure` and `SoftBodyMotionProperties::SetPressure/GetPressure` (live, public). `ApplyPressure` runs once per solver sub-step, uses `mSettings->mFaces`, needs `GetVolumeTimesSix() > 0` and applies `coefficient = n R T * dt / (6 V)` times the cross product to each face vertex scaled by inverse mass (`SoftBodyMotionProperties.cpp:291-325`). Units are SI: coefficient is joules |
| **Volume constraints** | `Volume{mVertex[4], mSixRestVolume, mCompliance}`; the constraint is `abs(signed six volume) - restVolume`, so winding is irrelevant (`ApplyVolumeConstraints`, `:455-500`). `CalculateVolumeConstraintVolumes()` fills the rest volume from the rest positions |
| **LRA** | `LRA{mVertex[2] (kinematic first), mMaxDistance}`, applied after the other constraints as a hard clamp (`ApplyLRAConstraints`, `:672-690`). `CreateConstraints` builds them from the closest kinematic vertex found by Dijkstra over edges and rods (`CalculateClosestKinematic`, private, `SharedSettings.cpp:109`). `CalculateLRALengths(multiplier)` is public |
| **Cosserat rods** | `RodStretchShear{mVertex[2], mLength, mInvMass, mCompliance, mBishop}` and `RodBendTwist{mRod[2], mCompliance, mOmega0}`. `CalculateRodProperties()` is public: Bishop frames by walking bend-twist connections, **may swap `mVertex` of a rod** to align directions, rod inverse mass = min of its vertices' (`:381-481`). Rod state is `mRodStates` (rotation, angular velocity), read through `GetRodRotation(i)` and `GetRodAngularVelocity(i)`; the rod axis is `rotation.RotateAxisZ()` (`:623-629`). Each rod needs at least one bend-twist constraint or it spins forever (Jolt comment, `SharedSettings.h:221`). Rod orientation integrates and damps with the body's linear damping (`:354-365`) |
| **`CreateConstraints(VertexAttributes[], EBendType, angleTol)`** | Edges, shear edges (two triangles forming a near-square), bends of type `None`, `Distance` (an extra distance edge) or `Dihedral`, and LRA of type `EuclideanDistance` or `GeodesicDistance`, all from per-vertex attributes (compliance, shear compliance, bend compliance, LRA type and multiplier); a list shorter than the vertex count repeats its last element. It **appends** to the arrays (`SharedSettings.cpp:189-368`) |
| **Skin constraints** | `SetEnableSkinConstraints(bool)` and `SetSkinnedMaxDistanceMultiplier(float)` are public and live (`SoftBodyMotionProperties.h:97-103`, used at `ApplySkinConstraints`, `:512,529`) |
| **Inverse mass at run time** | Jolt's `SoftBodyVertex` comment: "at run-time you should only modify the inverse mass and/or velocity" (`SoftBodyVertex.h:12-13`). A kinematic vertex is moved by its own velocity; `CalculateMassAndInertia()` is public and sets the body to infinite mass when any vertex is kinematic (`SoftBodyMotionProperties.cpp:30-52`). Rods keep their own `mInvMass`, "can be overridden afterwards" (`SharedSettings.h:221`) |
| **`SoftBodyContactSettings`** | `mInvMassScale1` (soft), `mInvMassScale2`, `mInvInertiaScale2` (other), `mIsSensor`; filled with defaults then offered to `OnSoftBodyContactValidate`. A pair with both scales 0 and no sensor is skipped (`:149-170`). A contact for which the callback sets `mIsSensor` goes to `mCollidingSensors`. Sensor results arrive in `OnSoftBodyContactAdded` as `GetNumSensorContacts()` and `GetSensorContactBodyID(i)`. **If no listener is installed, sensors are ignored** (`:157-160`); Aver installs one |
| **Collision group** | `SoftBodyCreationSettings::mCollisionGroup`; the collector tests `soft_body.GetCollisionGroup().CanCollide(body.GetCollisionGroup())` (`:148`). `CollisionGroup::CanCollide` calls the first non-null group filter, the soft body's first (`CollisionGroup.h:83-92`) |
| **Soft versus soft** | Not supported: the collector accepts rigid bodies only (`:147`, "TODO"). Self-collision absent. Unchanged from SOFTBODY_DEPTH 1.3 |
| **Buoyancy** | Not implemented for soft bodies (above) |
| **Volume query** | `GetVolume()` is public (`SoftBodyMotionProperties.h:117`) |

### 1.3 Soft-body scene, draw and script layers

| Fact | Where |
|---|---|
| `SoftBodySystem` owns every entity soft body; `CCloth` (40 B) and `CDestructible` (48 B) and `CSoftField` (64 B) are runtime-registered; `tick` order is fixed; `publish` builds `views_` of `PieceView` and the draw side reads `pieces(count)` | `modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp:50-98,331-345`, `SoftBodySystem.cpp` `tick` |
| `PieceView` carries world positions per draw vertex, indices, `sourceVertex` (or `kGeneratedVertex`), per-vertex `uv` for generated vertices, `meshId`, `useEntityWorld`, `layer`, `bake`. Generated vertices are an existing, tested path (the wave 10 caps) | `SoftBodySystem.hpp:179-203,241` |
| The draw side builds a skin-target mesh per `(entity, piece)`, rebuilds on an epoch change, and the scene walk substitutes the handle (a zero handle means "draw the authored mesh") | `modules/render.softbody/include/aver/render/SoftBodyScene.hpp:75-110`, `Runtime/src/GameRender.cpp:666-671` |
| `buildSimMesh` welds a render mesh to particles and refuses over `maxParticles` (4096); `ClothParams` has `tearable`, `defaultReachCm`, `backstopCm`; `SoftKind` is `Cloth` or `Destructible` | `modules/softbody/include/aver/softbody/SimMesh.hpp:55-60`, `SoftAsset.hpp:19-45` |
| Host wiring is one shared header: `registerGameSystems`, `tickSoftBodies`, `resetGameSystems`; the softbody notify and character-proxy installs | `Runtime/include/aver/game/GameSystemsWiring.hpp:168-195,248,263` |
| C#: `Aver.Physics.SoftBodyBuilder`/`SoftBodyHandle` (165 lines, low level) and `Aver.Framework.SoftBodies`/`Entity` members and `SoftBodyGraphInterop`; 15 graph rows in `GraphSoftBodyNodeTable.cs` pinned by `SoftBodyNodeTests` | `scripting/csharp/Aver.Physics/SoftBody.cs`, `Native.SoftBody.cs`, `Aver.Framework/SoftBodies.cs`, `Aver.Graph/GraphSoftBodyNodeTable.cs` |
| ABI parity test pairs each physics header with a `Native*.cs` by name; `physics_softbody_abi.h` and `physics_vehicle_ext_abi.h` are in `kAbis` | `tests/abi/src/AbiEnumTest.cpp:560-577` |

### 1.4 Jolt 5.6 Hair, read in full (`Jolt/Physics/Hair`, 2611 lines, `Jolt/Compute`, `Jolt/Shaders`)

The 2026-10-11 table in `HAIR_JOLT_EVALUATION.md` section 1 still holds. Facts that matter for this design, re-verified:

| Fact | Where |
|---|---|
| Status by its own header: in development; missing LOD, wind, grid advection, non-hull collision, a real gradient class, a working gravity preload, memory frugality, a second pose | `Hair.h:21-33` |
| `HairSettings` holds render strands (all of them) and simulation strands (a fraction, default 10% per material, picked by shuffling) and links every non-simulated render vertex to its closest simulated vertex. That step is quadratic: for each render strand, every simulated strand of the material and every vertex | `HairSettings.cpp:151-258` (`InitRenderAndSimulationStrands`) |
| Strands: up to 255 vertices, up to 255 materials, strand fraction in 8 bits. `sResample` makes a fixed vertex count | `HairSettings.h:58-72`, `HairSettings.cpp:262-317` |
| The scalp is a mesh (vertices, triangles, joint inverse bind pose, skin weights). Roots attach to scalp triangles by barycentrics (`Init` computes `SkinPoint`s, which requires every root to be within reach of a scalp triangle: `outMaxDistSqHairToScalp`). Skinning: `joint_matrix = inJointToHair * jointModelMatrix * scalpInverseBind` | `HairSettings.h:299-310`, `HairSettings.cpp:845-869` |
| `Hair::Update` **only records commands** into the `ComputeQueue` it is given; the caller executes. Per update it re-maps and rewrites materials, one constant block and one block per solver iteration (`Map(Write)` on UploadBuffer and ConstantBuffer types), the scalp joint matrices and, with collision, four shape buffers | `Hair.cpp:178-440` |
| Dispatch names used with `SetBuffer`, `SetRWBuffer`, `SetConstantBuffer`: `gContext`, `gIterationContext`, `gPositions`, `gVelocities` and about 30 others; a **separate constant buffer per solver iteration** | `Hair.cpp:442-640` |
| Iterations: 360 per second default (`cDefaultIterationsPerSecond`), delta time clamped to 1/30 s, so at most 12 and at 60 fps 6 iterations. Dispatch count per update about `9 + 4 N` | `HairSettings.h:330,344`, `Hair.cpp:83-90` |
| Output: `GetRenderPositionsCB()`, one `float3` (12 B) per render vertex in **hair-local Jolt space** (metres, Y up); its owner is the hair instance. Public getters also expose positions, velocities (`JPH_HairVelocity`: linear and angular) and the grid | `Hair.h:83-96` |
| Collision: leaf shapes of rigid, non-sensor bodies inside the simulation bounds, filtered by the hair's object layer, **ConvexHull only** (`GetSubType() == ConvexHull`); uploaded as planes, vertices and indices; a GPU pass finds the deepest hull per vertex | `Hair.cpp:111-181,211,268` |
| Shader sources use `JPH_SHADER_*` macros from `ShaderCore.h`, which are skipped when `JPH_SHADER_OVERRIDE_MACROS` is defined; buffers are bare `StructuredBuffer<T> name;` (no register), constants are a bare `cbuffer name` | `ShaderCore.h:5-72`, `HairIntegrateBindings.h`, `HairIntegrate.hlsl` |
| The same HLSL compiles as C++ for the CPU compute backend (`HLSLToCPP.h`, `HairWrapper.cpp`); `HairRegisterShaders(ComputeSystemCPU*)` registers it. `ComputeSystemCPU` is vendored and needs only `JPH_USE_CPU_COMPUTE` | `Compute/CPU`, `Shaders/HairWrapper.cpp:7-139`, `Jolt.cmake:541-564,909-911` |
| All `JPH_USE_DX12/VK/MTL/CPU_COMPUTE` defines only gate the compute backend files and the CPU wrappers; the Jolt core and `Hair.cpp` are identical either way | grep of `Jolt/` for `JPH_USE_` |
| Jolt's own backends: `ComputeSystemDX12::Initialize(ID3D12Device*)` and `ComputeSystemVKWithAllocator::Initialize(instance, physical device, proc addrs, device, computeQueueIndex)` accept an external device, create a private queue (`ComputeQueueDX12.cpp:23-52`, `ComputeQueueVK.cpp:36`), track buffer state privately (`ComputeBufferDX12.cpp:77-93`) and `Wait()` blocks the CPU on a fence | files cited |
| Settings serialisation is Jolt `ObjectStream` (OFF in our build) or `SaveBinaryState`/`RestoreBinaryState` (available) | `HairSettings.h:177-181` |

### 1.5 Aver's render and animation side, as it stands

| Fact | Where |
|---|---|
| RHI compute: `createComputePipeline(ComputePipelineDesc{cs, layout})`, binding sets of contiguous SRV and UAV structured-buffer slots (28 max), `setConstants`/`setConstantBuffer` (transient, root CBV), `dispatch`, `dispatchIndirect`, `bufferBarrier`, `uavBarrierBuffer`; caps `computeShaders`, `indirect`. Registers: b0 per frame, b1 per object, b2 draw block; **constant slot k = register b(k), up to `kMaxConstantSlots` 5** | `RHIResources.hpp:333-337,381-470,520-700`, `RHI.hpp:74-110` |
| Scene-pass compute recording is D3D12-only; features record compute in `prePass` (`computeInScenePass`). `SkinningPass` does | `RHI.hpp:101-104`; `modules/render.skin/include/aver/render/SkinningPass.hpp` |
| A compute-written mesh: `createSkinTargetMesh(source, &buf)` (writes `MeshVertex`, 32 B: position, normal, uv), `createPosedPartMesh` (own index buffer over the shared vertex buffer), `meshGeometry`, `createBlasUpdatable` + `refitBlas`; `drawMeshDepthOnly` accepts compute-written meshes | `RHI.hpp:518-545,638-650`, `RHIResources.hpp` |
| Descriptor writes are immediate while recorded commands execute later, so a binding set rewritten while in flight corrupts an already recorded dispatch (the `SkinnedMeshGpu` per-instance ring comment) | `SkinningPass.hpp:48-60` |
| Shading models: `AVER_MODEL_STANDARD 0`, `AVER_MODEL_UNLIT 1`, dispatched by a uniform switch in `averShadeDirect`; `d.model = gShadingModel` (the per-draw constant, written by both backends as `unlit ? 1 : 0`) | `material_prelude.hlsl:137-138,647,1198-1215`, `D3D12Device.cpp:366`, `VulkanDevice.cpp:279` |
| The material block is `pbr::MaterialConstants`, 192 bytes, mirrored in five places (prelude cbuffer, `RtMaterial`, two `static_assert`s, tests/formats). Flags run to bit 17 (terrain layers, which reinterpret the texture slots) | `MaterialGpu.hpp:11-54,234-236`, `material_prelude.hlsl:21-134` |
| Skinned characters expose `AnimSystem::skinning(entity, count)`: the engine palette, `model * inverseBind` per joint | `modules/anim.scene/include/aver/anim/AnimSystem.hpp:205` |
| No curve importer: `GltfImport.cpp` skips every non-triangle primitive; `Usd*.cpp` has no curve handling | `modules/formats/src/GltfImport.cpp:375-376`, grep of `Usd*.cpp` for `curve` |
| Kinematic hull proxies are possible with the existing ABI: `aver_phys_add_convex_hull`, `aver_phys_body_set_motion_type`, `aver_phys_body_move_kinematic`, `aver_phys_body_set_layer`; `Options` already reserves soft-body layers 14 and 15 | `physics_abi.h:311-314,137-173`, `SoftBodySystem.hpp:131-132` |
| Hair has no code anywhere. `Runtime/src/GameRender.cpp:925` has one stray comment "hair-card material slot" | grep, `HAIR_JOLT_EVALUATION.md` section 1 |

### 1.6 Wave 10 and 11 neighbours

SOFTBODY_DEPTH (committed) owns tearing, caps, fields, break effects, saved damage, proxy cages and baking; none of it is touched here. This feature adds **new kinds and new physics knobs beside it**: it does not change tear, plastic or debris behaviour, and a body that uses none of the new calls behaves as before. VEHICLES owns `physics_vehicle_ext_abi.h`; its pattern (own header, own version, `Native.*.cs`, relay ABI, graph family) is followed. GPU_PARTICLES owns the effect-spawn seam in `Aver.Core` (`aver::spawnEffectAt`); rope and hair do not use it in v1.

**UE parity plan item 16 (Hair / Groom).** `UE_FEATURE_PARITY_PLAN.md` row 16 asks for "strand asset from Alembic or curves, card generation, hair shading". Part B delivers: strand asset from curves (glTF, USD later) and a procedural generator (no Alembic, no new dependency); strand simulation (Jolt's, the owner's decision, replacing the evaluation's port recommendation); ribbon rendering with strand-count LOD and the shading model. **Card generation is not delivered** (open question 4). The evaluation's phase table (`HAIR_JOLT_EVALUATION.md` section 6) maps onto phases E to K here; its gate 0 (patent check) is 2.19, and its Alembic question is answered no.

### 1.7 Corrections to the survey

1. "Pressure in the builder": the **old** create path already takes `pressure` (`PhysicsWorld.cpp:1214-1217`); the **builder** does not. Fluids work, builder bodies cannot inflate.
2. "Skin-constraint enable and max-distance multiplier" are two public setters on motion properties, not creation settings. Easy.
3. "Soft-body buoyancy (Jolt asserts on rigid bodies)": the assert is the other way round. `Body::ApplyBuoyancyImpulse` asserts `IsRigidBody()` for soft bodies, and `SoftBodyShape::GetSubmergedVolume` is a stub, so there is nothing to wrap; the design is ours (2.3).
4. "Alembic"/"engine has no curve importer": confirmed, and the glTF importer is 40 lines from reading line strips (it already parses accessors for triangles).

---

## 2. Design

### 2.1 Architecture

```
 PART A
 .ocsoft cloth + inflate{}, jelly{}, rope{}      Aver.SoftBody (pure: Core + Formats)
 CCloth(+pressure)  CSoftVolume  CSoftRope  ->   Lattice (voxel tets, embedding), Rope (chain, tube), SimMesh (reused)
        |  aver_phys_sb_* (v2)  +  aver_phys_sbx_* (new)
 Aver.SoftBody.Scene   SoftBodySystem (cloth, destructible, fields)  +  SoftShapeSystem (jelly, rope; companion)
        |  PieceView via SoftBodySystem::addViewSource seam                    framework_softshape_abi.h
 Aver.Render.SoftBody  SoftBodyScene (unchanged)

 PART B
 .ochair  ->  Aver.Hair (pure)      OcHair, generator, glTF/USD curve import, proxy hulls, settings builder
              |  aver_phys_hair_* (settings builder, instances, update)           physics_hair_abi.h
 Aver.Physics (DLL)   JPH::Hair, HairSettings, HairShaders; HostComputeSystem : JPH::ComputeSystem
              |  AverPhysComputeHostV1 (C function table)   |  CPU host = Jolt ComputeSystemCPU (tests, fallback)
 Aver.Hair.Scene   CHair + HairSystem (palette, transform, LOD cadence, proxies, wind)      framework_hair_abi.h
 Aver.Render.Hair  RhiComputeHost, HairScene (IRenderFeature: sim replay, ribbon pass, LOD parts), AVER_MODEL_HAIR
```

Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed) at every ABI boundary; the Jolt conversion stays in `Convert.hpp`. Hair settings are converted once to Jolt metres, Y up, at `settings_finish`.

### 2.2 Physics ABI: `physics_softbody_ext_abi.h`

New header, new sources `PhysicsSoftBodyExt.cpp` (builder and live calls, attachments, policy, sensors, groups) and `SoftBodyFluid.cpp` (buoyancy), shared declarations in `SoftBodyInternal.hpp`. All int32/float/pointer types; units the engine's (cm, kg, kPa, cm/s). Every function returns 1 on success, 0 on refusal (`aver_phys_last_error` says why) unless noted. Builder calls are refused after `aver_phys_sb_builder_finish`; live calls are refused for a handle that is not a builder body.

```c
#define AVER_PHYS_SBX_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_sbx_abi_version(void);                                   /* 1 */

/* ---- builder (id from aver_phys_sb_builder_begin; the Ext state is keyed by that id and erased at finish/cancel) ---- */
/* Tetrahedra: 4 vertex indices each. Rest volume is measured from the staged rest positions at finish. */
AVER_PHYS_API int32_t aver_phys_sbx_builder_volumes(int32_t b, const int32_t* tet4, const float* compliance, int32_t count);
/* Explicit long range attachments: pairs2 = {kinematic vertex, dynamic vertex}; maxDistCm per pair. */
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra(int32_t b, const int32_t* pairs2, const float* maxDistCm, int32_t count);
/* LRA from the closest kinematic vertex (inverse mass 0) over the staged edges and rods:
   mode 0 off, 1 Euclidean, 2 geodesic. distanceMultiplier >= 1 (1.01 = 1% slack). */
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra_auto(int32_t b, int32_t mode, float distanceMultiplier);
/* Cosserat rods: ab = 2 vertices per rod; rodPairs2 = pairs of rod indices joined by a bend-twist constraint.
   Compliance arrays may be null (0). A rod not named in any pair is refused (it would spin). */
AVER_PHYS_API int32_t aver_phys_sbx_builder_rods(int32_t b, const int32_t* ab, const float* stretchCompliance, int32_t count,
                                                 const int32_t* rodPairs2, const float* bendTwistCompliance, int32_t pairCount);
/* A chain: vertices [firstVertex, firstVertex + vertexCount) joined by vertexCount - 1 rods and as many bend-twist pairs. */
AVER_PHYS_API int32_t aver_phys_sbx_builder_rod_chain(int32_t b, int32_t firstVertex, int32_t vertexCount,
                                                      float stretchCompliance, float bendTwistCompliance);
/* Jolt's own CreateConstraints over the staged faces. attrs5 = per vertex {compliance, shearCompliance, bendCompliance,
   lraMode (0/1/2), lraMultiplier}; attrCount may be less than the vertex count (the last repeats, as in Jolt).
   bendType 0 none, 1 distance, 2 dihedral. Excludes staged edges and bends (finish refuses the mix). */
AVER_PHYS_API int32_t aver_phys_sbx_builder_auto(int32_t b, const float* attrs5, int32_t attrCount, int32_t bendType, float angleToleranceDeg);
/* Extra parameters, AVER_PHYS_SBX_P_*; NaN = leave as is. */
AVER_PHYS_API int32_t aver_phys_sbx_builder_params(int32_t b, const float* p, int32_t n);

/* After finish: the final Jolt index of each staged item (Optimize reorders): kind 0 volume, 1 lra, 2 rod, 3 rod pair.
   Returns the count written. Valid for the body this builder made. */
AVER_PHYS_API int32_t aver_phys_sbx_slots(int32_t body, int32_t kind, int32_t* out, int32_t maxCount);

/* ---- live ---- */
AVER_PHYS_API int32_t aver_phys_sbx_set_pressure(int32_t body, float pressureKPa);       /* 0 off. Refused for an open mesh */
AVER_PHYS_API int32_t aver_phys_sbx_get_pressure(int32_t body, float* outKPa);
AVER_PHYS_API int32_t aver_phys_sbx_volume(int32_t body, float* outRestCm3, float* outNowCm3);   /* now can be negative: inside out */
AVER_PHYS_API int32_t aver_phys_sbx_set_skin(int32_t body, int32_t enabled, float distanceScale); /* 0 distanceScale = hard skin */
/* Per-vertex inverse mass (0 = pinned). Recomputes the body's mass and the inverse mass of every rod that touches the vertex. */
AVER_PHYS_API int32_t aver_phys_sbx_set_inv_mass(int32_t body, const int32_t* vertex, const float* invMass, int32_t n);

/* Attachments (grab/release, anchors): the vertex becomes kinematic and is carried to the target each fixed step. */
AVER_PHYS_API int32_t aver_phys_sbx_attach(int32_t body, int32_t vertex, const float* targetXyzCm, float maxSpeedCmS);
AVER_PHYS_API int32_t aver_phys_sbx_attach_to_body(int32_t body, int32_t vertex, int32_t otherBody, const float* localOffsetXyzCm, float maxSpeedCmS);
AVER_PHYS_API int32_t aver_phys_sbx_move_attachment(int32_t body, int32_t vertex, const float* targetXyzCm);   /* a plain attach's target */
AVER_PHYS_API int32_t aver_phys_sbx_detach(int32_t body, int32_t vertex, float restoreInvMass);     /* < 0: what it had before attach */
AVER_PHYS_API int32_t aver_phys_sbx_attachment_count(int32_t body);

/* Contact policy (applied in OnSoftBodyContactValidate). Scales: 0 = infinite mass, 1 = unchanged. */
AVER_PHYS_API int32_t aver_phys_sbx_set_contact_policy(int32_t body, float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
#define AVER_PHYS_SBX_PAIR_NORMAL 0
#define AVER_PHYS_SBX_PAIR_SENSOR 1   /* no response; enter/exit events */
#define AVER_PHYS_SBX_PAIR_IGNORE 2   /* RejectContact */
AVER_PHYS_API int32_t aver_phys_sbx_set_pair_policy(int32_t body, int32_t otherBody, int32_t mode,
                                                    float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
AVER_PHYS_API int32_t aver_phys_sbx_clear_pair_policy(int32_t body, int32_t otherBody);
/* Sensor contacts, polled, cleared at the top of aver_phys_step: bodies that are Jolt sensors OR pairs set to SENSOR. */
AVER_PHYS_API int32_t aver_phys_sbx_sensor_count(void);
AVER_PHYS_API int32_t aver_phys_sbx_sensor_get(int32_t index, int32_t* outSoftBody, int32_t* outSensorBody, int32_t* outEntered);

/* Collision group: 0 none; otherwise the soft body ignores every rigid body whose group id equals this (a ragdoll's handle). */
AVER_PHYS_API int32_t aver_phys_sbx_set_collision_group(int32_t body, int32_t group);

/* Buoyancy. fluidDensityKgM3 0 = off. flags: SBX_BUOY_WORLD_WATER (use aver_phys_set_water_plane / the sampler),
   SBX_BUOY_OWN_PLANE (use aver_phys_sbx_set_water_plane). drag: quadratic coefficient, 1 is a flat plate. */
#define AVER_PHYS_SBX_BUOY_WORLD_WATER 0x1
#define AVER_PHYS_SBX_BUOY_OWN_PLANE   0x2
AVER_PHYS_API int32_t aver_phys_sbx_set_buoyancy(int32_t body, float fluidDensityKgM3, float drag, int32_t flags);
AVER_PHYS_API int32_t aver_phys_sbx_set_water_plane(int32_t body, const float* pointXyzCm, const float* normalXyz, const float* currentXyzCmS);
AVER_PHYS_API int32_t aver_phys_sbx_submerged(int32_t body, float* outFraction01, float* outVolumeCm3);      /* last step */

/* Rod state, engine axes: rotation (x,y,z,w, the rod's local +Z is its axis in Jolt space, converted) and angular velocity rad/s. */
AVER_PHYS_API int32_t aver_phys_sbx_rod_state(int32_t body, int32_t firstRod, int32_t count, float* outQuatXyzw, float* outAngVelRadS);
AVER_PHYS_API int32_t aver_phys_sbx_rod_count(int32_t body);
```

**`AVER_PHYS_SBX_P_*` block** (defaults in brackets): 0 `PRESSURE_KPA` [0]; 1 `SKIN_ENABLED` [1]; 2 `SKIN_DISTANCE_SCALE` [1]; 3 `COLLISION_GROUP` [0]; 4 `SOFT_INV_MASS_SCALE` [1]; 5 `OTHER_INV_MASS_SCALE` [1]; 6 `OTHER_INV_INERTIA_SCALE` [1]; 7 `FLUID_DENSITY` [0]; 8 `FLUID_DRAG` [1]; 9 `BUOY_FLAGS` [1, world water]; `COUNT` 10. `AbiEnumTest` pins the block against `SoftBodyExt.cs` with a `_COUNT` mirror as the vehicle blocks are pinned (`AbiEnumTest.cpp:792-817`).

### 2.3 Semantics, per feature (the contract the implementation and the tests follow)

**Hooks in `PhysicsSoftBody.cpp` (owned).** `SoftRecord` moves into `SoftBodyInternal.hpp` with a new member `SoftExt ext` (pressure, rest volume, skin flags, group, contact policy, pair table, attachment list, buoyancy, rod adjacency, slot remaps, `closed` flag, sensor sets) so the Ext TUs see it. `finish` gains four hook calls: `sbxPrepare(b, shared, n)` after faces and before `Optimize` (appends volumes, LRA, rods, auto constraints; computes rod properties; may add the zero-area face), `sbxApply(b, nb, mp, rec, results)` after creation (pressure, skin, group, `restVolume = mp->GetVolume()`, `closed`, slot remaps), `sbxCarry(oldRec, rec)` when `replaceBody` (policy, buoyancy, pairs, attachments, pressure survive a tear rebuild exactly as aero does at `:577-579`), and the face-less relaxation. The validate callback calls `sbxValidate(soft, other, settings)`; `onStepBegin` calls `sbxStepBegin(dt)`; the new `onStepEnd` calls `sbxStepEnd(dt)`.

**Pressure.** `nRT = pressureKPa * 1000 * restVolume_m3` (Jolt coefficient). `set_pressure` is `mp->SetPressure(nRT)` under the body write lock, activates the body when the value changes, clears `frozen`. Refused (`Unsupported`) when `restVolume <= 0` or the mesh is not closed (every face edge shared by exactly two faces, checked once at finish). Pressure force is applied per sub-step inside Jolt, so it is frame-rate independent by construction. There is no ambient pressure term in Jolt: a body in equilibrium has internal pressure `P0 = nRT / V` pushing out against edge tension, which is why an inflatable needs `edgeCompliance` small and works as expected only for a closed mesh. UNVERIFIED starting values: beach ball 3 kPa, basketball 12 kPa, 0.25 kg per square metre of skin. A leak is the script ramping the value down; `sbx_volume` reports the ratio.

**Volumes and jelly.** `builder_volumes` takes tetrahedra from the lattice (2.4). Rest volume is `|six volume|` of the rest positions, so mirror winding is harmless. Compliance 0 is incompressible; useful jelly is `1e-4 .. 1e-3` per tetrahedron (UNVERIFIED). Edges of the lattice (every tetrahedron edge, deduplicated) carry the shear and stretch stiffness; the lattice surface faces are the Jolt faces (collision and the pressure surface).

**LRA.** Explicit pairs or `lra_auto`. Aver computes the closest kinematic vertex with its own Dijkstra over the staged edges and rods (Jolt's is private), then calls the public `CalculateLRALengths(multiplier)` for the Euclidean case, or sets `mMaxDistance = multiplier * geodesicDistance` itself. `Optimize` remaps LRA, reported by `sbx_slots(.., 1, ..)`. Use: a cape or flag whose edges stretch (cloth with `fast` path, 2.5); a rope with a fixed end.

**Rods and ropes.** `builder_rods`/`rod_chain` append `RodStretchShear` and `RodBendTwist` and call `CalculateRodProperties()` (which may swap a rod's vertices: the remap `sbx_slots(.., 2, ..)` plus `rod_state` tell the caller the final order; a rope chain is built in order so no swap happens). Rod orientation damping reuses the body's `Damping` param. Rod inverse mass follows `set_inv_mass` (the Ext code writes `mRodStretchShearConstraints[i].mInvMass = min(inv(v0), inv(v1))` for every incident rod through the record's own non-const settings Ref, the same legality the dents rely on). `rod_state` converts each rotation with `fromJolt(Quat)` (`Convert.hpp`). The face-less body (decision 4) is built with `AVER_PHYS_SB_P_DOUBLE_SIDED` irrelevant. A rope with one pinned end hangs; with two it sags to its slack; `maxSpeed` and `vertexRadiusCm` (collision thickness) are the usual params; a rope collides through its vertices only (Jolt collides vertices, not edges), so `vertexRadiusCm` should be at least 0.6 of the tube radius and segments no longer than the radius of the thinnest thing it must wrap (UNVERIFIED rule of thumb in the tuning table).

**Per-vertex inverse mass.** `set_inv_mass` writes `mVertices[i].mInvMass`, then `mp->CalculateMassAndInertia()`. Allowed on any builder body; the stored record's own `invMass` copy for rebuilds is updated so a tear does not undo it. Skinned constraints are untouched.

**Attachments.** `attach` saves the vertex's current inverse mass, sets it to 0, stores `{vertex, target, maxSpeed, bodyHandle?, localOffset}`. In `sbxStepBegin`, for each attachment, with the body write-locked: `v.mVelocity = clampLength((target - position) / dt, maxSpeed)` in COM-local space (target converted with the inverse COM transform). `attach_to_body` evaluates the other body's point (`BodyInterface::GetCenterOfMassTransform`, no lock needed on the main thread between steps) once per step. Coupling is **one way** (the vertex follows; the body feels nothing from the rope); contact response between rope and body is two way as for any soft-versus-rigid contact. `detach` restores the saved inverse mass (or the given one), zeroes the vertex velocity, wakes the body. A body with a stale `otherBody` handle detaches that vertex and logs once. Teleporting the target further than `maxSpeed * dt` just saturates; the host that wants a snap calls `aver_phys_sb_skin_world(... hardSkin)` or re-creates. Attachments survive a rebuild (`sbxCarry`) when the vertex index is still valid, which is the tear pipeline's `stateMap` identity case; a tear that drops the vertex detaches it.

**Contact policy and sensors.** In `sbxValidate`: pair table first (`IGNORE` returns `RejectContact`, `SENSOR` sets `mIsSensor = true`), then pair scales, else the body defaults. Two cautions from Jolt's header: keep a pair's scales constant over time, and a pair with `softScale == 0` and `otherScale == 0` is skipped. The existing impact bookkeeping (`g_pairs`) is unchanged. Sensor contacts (a Jolt sensor body in range, or a pair set to SENSOR) reach `OnSoftBodyContactAdded` as `GetSensorContactBodyID(i)`; `sbxContactAdded` records the set for the body this step. Jolt calls `OnSoftBodyContactAdded` only when something collided that step (`mNeedContactCallback`), so a quiet step has no callback; `sbxStepEnd` therefore compares each body's previous set to the set recorded this step (empty when no callback ran) and emits `entered`/`exited` records to a list cleared at the top of `aver_phys_step`, like contacts. Sensor event handles are ordinary body handles; the ABI never exposes a Jolt id. The callback runs on worker threads: the same `g_eventMutex` guards the sets, the pair table is read-only during the step.

**Collision group.** One file-local `AverGroupFilter : JPH::GroupFilter` (no serialisation; `SaveBinaryState` is never called) whose `CanCollide(a, b)` is `!(a.GetGroupID() != cInvalidGroup && a.GetGroupID() == b.GetGroupID())`. `set_collision_group(body, g)` sets `CollisionGroup(filter, g, 0)` with `Body::SetCollisionGroup` (a plain write, `Body.h:132`) under the write lock; `0` restores `CollisionGroup()`. The group is a creation setting for rebuilds (`mCollisionGroup`), carried by `sbxCarry`. Ragdoll parts already carry their own filter with `GroupID = ragdoll handle`; because the soft body's filter is asked first, this works unchanged.

**Buoyancy (`SoftBodyFluid.cpp`, called from `sbxStepBegin`).** For each body with `fluidDensity > 0`, with the water source resolved as: the body's own plane (if `OWN_PLANE`), else the global plane when `hasPlane()` and `plane().buoyancy > 0`, else the sampler at each face centroid (`waterSampler().fn`, same contract as `Buoyancy.cpp:61-99`, no corner averaging). Per face, in Jolt space with `g = |gravity|`: `centroid`, outward normal `n = normalize((v1 - v0) x (v2 - v0))` (the winding Jolt's pressure uses), `area`, `depth = dot(surfacePoint - centroid, up)`, `h = sqrt(area)`, submerged weight `s = clamp(depth / h + 0.5, 0, 1)`, effective depth `d = max(depth, 0)`.
- Closed bodies: pressure force `F_p = -n * (rho * g * d) * area * s` per face (outward normal, fluid pushes in). Summed over a fully submerged closed mesh this is exactly `rho g V` upward.
- All bodies: drag `F_d = 0.5 * rho * drag * area * s * |vrel_n| * vrel_n * n` where `vrel = fluidVelocity - mean(face vertex velocity)` and `vrel_n = dot(vrel, n)` (the same form as `applyAero`, `SoftBodyAero.cpp:73-84`).
- `dv_i = (F / 3) * invMass_i * dt` on each of the face's vertices, kinematic vertices skipped. `dt` is the fixed step, so the result is frame-rate independent.
An open mesh gets `closed = false` and only drag (hydrostatic pressure on a thin sheet cancels; applying it would push a cloth sideways). `submerged` reports the area-weighted fraction and the displaced volume of the last step. Keeps the body awake while any face is submerged (the aero pattern at `PhysicsSoftBody.cpp:295-300`). No patent concern is known for per-face hydrostatics plus quadratic drag (textbook physics); the formulas are stated here and implemented from this text.

**Skin enable and scale.** `mp->SetEnableSkinConstraints`, `SetSkinnedMaxDistanceMultiplier`; stored so a rebuild reapplies them. A cloth LOD can drop skin constraints for far characters (the cloth system may call it; not required in v1).

**What is not offered, and why.** Soft versus soft collision and self-collision (Jolt has neither); `SoftBodyContactSettings` per-vertex values (the callback is per body pair); changing the number of vertices live (a rebuild); making rods collide along their length (vertices only).

### 2.4 Scene layer: inflatables, jelly, ropes and cables

**Inflatable = a closed cloth.** `CCloth` and `buildSimMesh` are reused as they are. `.ocsoft` `cloth` gains an optional section (2.5) `inflate`; `buildEntry` (`SoftBodyBuild.cpp`) calls `aver_phys_sbx_builder_params` with `PRESSURE_KPA` before `finish`, so a rebuild keeps it. A mesh that is not closed logs `[SoftBody] entity N: inflate needs a closed mesh` once and simulates as plain cloth. The runtime knob is a relay op (`SetPressure`), not a `CCloth` field, so `static_assert(sizeof(CCloth) == 40)` is untouched. Pins on an inflatable (a balloon on a string) work as for cloth.

**`SoftShapeSystem`** (new, `modules/softbody.scene/include/aver/softbody/SoftShapeSystem.hpp`, sources `SoftShapeSystem.cpp`, `SoftShapeVolume.cpp`, `SoftShapeRope.cpp`). A companion class in the same module and namespace, one global accessor `softShapeSystem()` beside `softBodySystem()`. It owns the Jolt bodies of `CSoftVolume` and `CSoftRope` entities, builds them lazily at the first tick, reads back positions each tick (`aver_phys_sb_state`, positions only, as `SoftBodySystem` does at `SoftBodySystem.cpp:617`), and publishes `PieceView`s. It shares no state with `SoftBodySystem` except through one additive seam:

```cpp
// SoftBodySystem.hpp (additive)
using ViewSourceFn = void (*)(std::vector<PieceView>& out, void* user);   // appends this tick's views
void SoftBodySystem::addViewSource(ViewSourceFn fn, void* user);          // called at the end of publish()
```

The draw side keeps reading `softBodySystem().pieces(count)`; nothing in `render.softbody` changes. Entities of these components carry `CSoftBody` (the marker the draw seam keys on, `SoftBodySystem.hpp:6`) exactly as cloth does.

Registration: `SoftShapeSystem::registerComponents(world)` (idempotent) is called from `registerGameSystems` (`GameSystemsWiring.hpp:184`); `tickSoftBodies` calls `softShapeSystem().tick(world, dt)` right after `softBodySystem().tick`; `resetGameSystems` calls `clear()`. Both hosts already call those three, so there is no host edit beyond the one shared header.

**`CSoftVolume`** (jelly; registered at runtime, 48 bytes, zero-filled storage means the useful state):

```cpp
struct CSoftVolume {                 // "CSoftVolume"
    u64 asset = 0;                   // .ocsoft kind "jelly"; 0 = defaults
    u32 flags = 0;                   // kVolNoWorld 0x1 (no collision), kVolNoGravity 0x2
    f32 stiffnessScale = 0;          // 0 reads 1 (divides volume and edge compliance)
    f32 cellCm = 0;                  // 0 = asset default (the lattice cell edge)
    f32 gravityScale = 0;            // 0 reads 1
    f32 dampingScale = 0;            // 0 reads 1
    f32 outVolumeRatio = 0;          // OUT: current / rest volume
    f32 outMaxStretch = 0;           // OUT: largest lattice edge strain
    u32 outState = 0;                // OUT: bit0 awake, bit1 frozen, bit2 over the particle cap (not simulated), bits 8+ lattice node count
    u32 _pad = 0;
};
static_assert(sizeof(CSoftVolume) == 48);
```

Pure library `modules/softbody/include/aver/softbody/Lattice.hpp` (Core only):

```cpp
struct LatticeSpec { f32 cellCm = 12; u32 maxNodes = 2048; f32 snapCm = 0; };      // snap: lattice nodes within this of the surface move onto it
struct Lattice {
    std::vector<Vec3> nodes;               // mesh-local cm, rest
    std::vector<u32>  tets;                // 4 per tetrahedron
    std::vector<u32>  surfaceFaces;        // 3 per boundary triangle, winding outward (engine convention)
    std::vector<u32>  edges;               // 2 per unique tet edge
    f32 restVolumeCm3 = 0;
    std::string error;                     // closed-mesh, over-cap and empty-lattice messages
};
bool buildLattice(const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount, const LatticeSpec&, Lattice& out);

struct Embedding { std::vector<u32> tet; std::vector<f32> bary; };    // per render vertex: tet index and 4 barycentric weights
void embedMesh(const Lattice&, const f32* positions, u32 vertexCount, Embedding& out);
void embeddedPositions(const Lattice&, const Embedding&, const Vec3* nodeWorld, u32 vertexCount, Vec3* out);
```

Algorithm. (1) Inside test per lattice point by parity of ray crossings along +X against the triangle soup, with a per-row sort so a 16^3 lattice over a 20k triangle mesh is a few milliseconds (the mesh must be closed; an open mesh is refused with a message). (2) A cell is kept when its centre is inside. (3) Each kept cube is split by the Freudenthal (Kuhn) subdivision into six tetrahedra sharing the main diagonal; adjacent cubes use the same diagonal direction so the faces conform (textbook property; `LatticeTest` checks that every interior face is shared by exactly two tetrahedra). (4) Boundary triangles of the tetrahedral set are the surface faces; nodes within `snapCm` of the mesh move onto the nearest triangle point so the lattice hugs the shape. (5) Embedding: each render vertex goes to the tetrahedron containing it, else the nearest one with extrapolated (unclamped) barycentric weights, each weight capped at 2 in magnitude. Plain tetrahedral barycentrics, not harmonic or mean-value coordinates (the choice SOFTBODY_DEPTH 2.19 also made). Collision is against the stair-stepped lattice surface; the render mesh follows through the embedding and may poke up to half a cell beyond it. `vertexRadiusCm` defaults to `0.35 * cellCm` for jelly to hide that (UNVERIFIED). The over-cap rule is the cloth rule: `nodes > maxNodes` refuses with the over-cap state bit and the entity draws its authored mesh at rest.

`SoftShapeVolume.cpp` builds the body with `builder_begin(nodes)`, `vertices`, `faces(surfaceFaces)`, `edges(lattice edges, rest from the nodes, compliance)`, `builder_volumes(tets, volumeCompliance)`, `params` and `sbx params`, `finish`. A tick reads node positions, evaluates `embeddedPositions` for the render vertices and publishes one `PieceView` (`sourceVertex` identity, authored `meshId`, `useEntityWorld = true`) with the epoch fixed (no topology change, so the draw side never rebuilds). Cost is `O(render vertices)` per awake tick; a sleeping body publishes `frozen = true`.

**`CSoftRope`** (rope and cable; 64 bytes):

```cpp
struct CSoftRope {                   // "CSoftRope"
    u64 asset = 0;                   // .ocsoft kind "rope"
    u32 startEntity = 0, endEntity = 0;           // 0 = free end / world point
    f32 startOffset[3] = {}, endOffset[3] = {};   // cm, in the anchor entity's space (world space when the entity is 0)
    f32 lengthCm = 0;                // 0 = (distance between the ends) x slack
    f32 radiusCm = 0;                // 0 = asset default; the drawn tube
    u32 flags = 0;                   // kRopeNoWorld 0x1, kRopeNoStartPin 0x2, kRopeNoEndPin 0x4
    f32 outLengthCm = 0;             // OUT: current polyline length
    f32 outStrain = 0;               // OUT: (current - rest) / rest
    u32 outState = 0;                // OUT: bit0 awake, bit1 cut, bits 8+ segment count
};
static_assert(sizeof(CSoftRope) == 64);   // 8 + 8 + 24 + 6 x 4, padding free
```

Pure library `Rope.hpp`:

```cpp
struct RopeSpec { f32 lengthCm, segmentCm = 12, radiusCm = 1.2, massPerMeterKg = 0.4; u32 sides = 8; f32 uvCmPerTile = 20; f32 stretchCompliance = 1e-8, bendTwistCompliance = 1e-6; };
struct RopeBuild { u32 nodes; std::vector<Vec3> rest; std::vector<f32> invMass; };
bool planRope(const Vec3& a, const Vec3& b, const RopeSpec&, RopeBuild& out);                  // a sagging rest curve so the rope is not taut
void tubeVertices(const RopeBuild&, const Vec3* nodes, const Quat* rodRotation, f32 radiusCm, u32 sides, Vec3* outPos);   // sides x nodes
void tubeUvs(const Vec3* nodes, u32 count, u32 sides, f32 uvCmPerTile, f32* outUv);             // u along the length in tiles, v around
void tubeIndices(u32 nodes, u32 sides, std::vector<u32>& out);                                  // static; flat caps optional
```

Rest pose: nodes spaced by `segmentCm` along a parabola between the two anchors whose arc length equals `lengthCm` (taut when `slack = 1`; UNVERIFIED shape). Mass per node is `massPerMeterKg * segmentCm / 100`, inverse mass its reciprocal; both ends are pinned (inverse mass 0) unless the flag says free. The body is `builder_begin(nodes)`, `vertices`, `sbx rod_chain` (one stretch-shear rod per segment, bend-twist between neighbours), `params` with `vertexRadiusCm`, `finish` (face-less, decision 4). Anchors: an end whose entity is non-zero is `aver_phys_sbx_attach`ed to that entity's world point each tick (the system reads `CWorld`, so a rope follows a moving door, a vehicle or an animated hand with no extra code); an end with no entity attaches to a fixed world point. Detaching an end (script) frees it. The tube: `tubeVertices` places each ring in the frame of the average of the two adjoining rod rotations (`sbx_rod_state`, so a twisting cable twists its texture), published as a `PieceView` with every vertex `kGeneratedVertex`, `uv` from `tubeUvs`, `useEntityWorld = true`; the rope entity needs a `CMeshRenderer` with a material (the system adds the engine Cube primitive as the placeholder mesh whose own draw is replaced, visible bit set). **Cut:** `cut(point)` splits at the nearest node: the chain is rebuilt as two bodies (the same `finish` with `replaceBody` for the part holding the start, a new handle and a new entity for the rest; positions and velocities carried through `stateMap`), both cut ends left free. Cut is phase D; its absence does not block the rest.

Draw cost: `nodes * sides` vertices per rope (UNVERIFIED budget: 300 ropes of 40 nodes and 8 sides is 96k vertices, packed on the CPU only for awake ropes); distance tiers reuse the cloth freeze (`freezeDistanceClothCm`).

**Options.** `SoftBodySystem::Options` gains no members. The shape system has its own `SoftShapeOptions { u32 maxRopeNodes = 4000; u32 maxVolumeNodes = 8000; f32 freezeDistanceCm = 6000; u32 maxBuildsPerFrame = 2; i32 shapeLayer = 14; }` (the cloth layer, so shapes obey the same layer matrix as cloth).

### 2.5 `.ocsoft` additions (still `"version":1`; every addition optional, written only when non-default)

```json
{ "format":"ocsoft", "version":1, "kind":"cloth", "preset":"cloth_light",
  "cloth":   { "tearable":false, "solver":"fast", "bend":"distance", "lra":"geodesic", "lraSlack":1.02 },
  "inflate": { "pressureKPa":3.0, "leakKPaPerS":0.0 },
  "group":   { "ragdollOf":"owner" } }
{ "format":"ocsoft", "version":1, "kind":"jelly", "preset":"jelly",
  "jelly":   { "cellCm":10, "snapCm":3, "volumeCompliance":2e-4, "edgeCompliance":5e-5, "vertexRadiusCm":3.5, "maxNodes":2048 } }
{ "format":"ocsoft", "version":1, "kind":"rope", "preset":"rope",
  "rope":    { "radiusCm":1.2, "segmentCm":12, "massPerMeterKg":0.4, "slack":1.02, "sides":8, "uvCmPerTile":20,
               "stretchCompliance":1e-8, "bendTwistCompliance":1e-6, "material":"M_Rope" } }
```

- `kind` gains `jelly` and `rope` (`SoftKind` gets `Jelly = 2`, `Rope = 3`; the reader still ignores unknown keys and refuses a newer `version`). Presets `jelly` and `rope` are added to `presetInfo` (`Material.hpp:80`) as ids 5 and 6 (`SoftPreset` is pinned to the C# `SoftPreset`, which gains `Jelly` and `Rope` additively).
- `cloth.solver`: `"cage"` (default; the tearable wave 9 path) or `"fast"` (non-tearable only): the body is built with `sbx builder_auto` (Jolt shear edges, `bend` of `none`, `distance` or `dihedral`, `lra` from the pinned vertices, `lraSlack` as the multiplier) instead of the cage's beams. It is the cheap path for flags, curtains and capes: no dents, no tears, no `RenderBinding` splits. `tearable: true` with `solver: fast` is refused by `valid()`. A fast-path body has no `edgeSlot` mapping, so the wave 9 dent and tear code is skipped for it (`kClothTearable` is cleared at build with a log line).
- `inflate.pressureKPa` as above. `inflate.leakKPaPerS` makes the system ramp the pressure down (a puncture); the script can still `SetPressure`.
- `group.ragdollOf: "owner"` makes `buildEntry` call `aver_phys_sbx_set_collision_group(body, ragdollHandle)` for the ragdoll of the entity that owns the skeleton, once it exists, so a cape never fights its wearer's proxy ragdoll (the character-proxy ragdoll is started by `softBodyProxyFn`, `GameSystemsWiring.hpp:198-208`).
- Spec rows go to `docs/formats/FORMAT_SPECS.md` (the `.ocsoft` row, `:28`) via the integrator.

### 2.6 C# API

`Aver.Physics` (low level, tools and tests): `Native.SoftBodyExt.cs` (every name copied from the header character for character, as `Native.VehicleExt.cs` does) and `SoftBodyExt.cs`:

```csharp
public enum SoftBodyExtParam { PressureKPa, SkinEnabled, SkinDistanceScale, CollisionGroup, SoftInvMassScale, OtherInvMassScale,
                               OtherInvInertiaScale, FluidDensity, FluidDrag, BuoyFlags, Count }
public enum SoftPairMode { Normal = 0, Sensor = 1, Ignore = 2 }
public readonly record struct SoftBodySensorEvent(int SoftBody, int Sensor, bool Entered);

public sealed partial class SoftBodyBuilder    // additive members; the class becomes partial
{
    public bool Volumes(int[] tet4, float[]? compliance = null);
    public bool Lra(int[] pairs2, float[] maxDistCm);
    public bool LraAuto(LraMode mode, float distanceMultiplier = 1.0f);
    public bool Rods(int[] ab, float[]? stretch, int[] rodPairs2, float[]? bendTwist);
    public bool RodChain(int firstVertex, int vertexCount, float stretch = 0, float bendTwist = 0);
    public bool Auto(float[] attrs5, BendType bend, float angleToleranceDeg = 8);
    public void Set(SoftBodyExtParam p, float value);
}
public readonly partial struct SoftBodyHandle  // partial too
{
    public static int ExtAbiVersion => Native.aver_phys_sbx_abi_version();
    public bool SetPressure(float kPa);  public float Pressure { get; }
    public (float Rest, float Now) Volume();
    public bool SetSkin(bool enabled, float distanceScale = 1);
    public bool SetInvMass(ReadOnlySpan<int> vertices, ReadOnlySpan<float> invMass);
    public bool Attach(int vertex, Float3 target, float maxSpeedCmS = 2000);
    public bool AttachToBody(int vertex, int otherBody, Float3 localOffset, float maxSpeedCmS = 2000);
    public bool MoveAttachment(int vertex, Float3 target);  public bool Detach(int vertex, float restoreInvMass = -1);
    public bool SetContactPolicy(float softScale, float otherScale, float otherInertiaScale);
    public bool SetPairPolicy(int otherBody, SoftPairMode mode, float soft = 1, float other = 1, float inertia = 1);
    public bool SetCollisionGroup(int group);
    public bool SetBuoyancy(float fluidDensity, float drag = 1, BuoyFlags flags = BuoyFlags.WorldWater);
    public (float Fraction, float VolumeCm3) Submerged();
    public int RodState(int firstRod, Span<Quat> rotation, Span<Float3> angularVelocity);
    public static int DrainSensorEvents(Span<SoftBodySensorEvent> buffer);
}
```

`Aver.Framework` (game scripts) in the new `SoftShapes.cs` and `FwSoftShape.cs` (natives of `framework_softshape_abi.h`, `AVER_FW_SOFTSHAPE_ABI_VERSION 1`, one provider function, 0 with no provider, the `framework_softbody_abi.h` idiom):

```csharp
public readonly partial struct Entity
{
    public bool AddJelly(string asset = "", SoftPreset preset = SoftPreset.Jelly);
    public bool AddRope(Vec3 start, Vec3 end, string asset = "", float lengthCm = 0, float radiusCm = 0);
    public bool SetRopeEnd(RopeEnd end, Entity anchor, Vec3 offset);          // attach to an entity point
    public bool SetRopeEndFree(RopeEnd end, Vec3 worldPoint, bool pinned);
    public bool CutRope(Vec3 point, out Entity other);
    public bool TryGetRope(out RopeState state);                               // length, strain, nodes, cut
    public bool TryGetJelly(out JellyState state);
    // any soft entity (cloth, jelly, rope)
    public bool SetSoftPressure(float kPa);  public bool TryGetSoftVolume(out float restCm3, out float nowCm3);
    public int  GrabSoftVertex(Vec3 point, float maxDistanceCm, Vec3 target);  // nearest vertex; a grab id (vertex index + 1), 0 on none
    public bool MoveSoftGrab(int grab, Vec3 target);  public bool ReleaseSoftGrab(int grab);
    public bool SetSoftBuoyancy(float fluidDensityKgM3, float drag = 1);
    public bool TryGetSoftSubmerged(out float fraction, out float volumeCm3);
    public bool SetSoftContact(float softScale, float otherScale);
    public bool SetSoftPairMode(Entity other, SoftPairMode mode);
}
public static class SoftShapes { public static int DrainSensorEvents(Span<SoftSensorEvent> buffer); }   // entities, not body handles
```

`GrabSoftVertex` finds the nearest vertex of the entity's body within `maxDistanceCm` (the system already mirrors positions), attaches it and returns the id; grabbing two vertices is two calls. A `Ropes.Create(Vec3, Vec3, ...)` convenience mirrors `Vehicles.Spawn`: it creates the entity, adds the components and returns the handle. The relay applies every op at the soft-body system's next tick in arrival order (the `framework_softbody_abi.h` rule). Sensor events from the physics ABI become entity events (`SoftBodySensorEntered`, `SoftBodySensorExited`) on the named-event seam, so graphs need no node for them.

### 2.7 Aver Node rows (wave 9 C contract: table rows, palette mirror, thin `...ForGraph` wrappers, `[AutoRun]` tests)

**Family `SoftBody` v3** (`GraphSoftBodyNodeTable.cs`, `GraphNodeDefsSoftBody.hpp`, `docs/nodes/SoftBody.md`; the existing 15 rows and their pins do not change, 10 are appended; `SoftBodyNodeTests.TestRowSetIsTheDesignSet` becomes 25):

| Key | Kind | Pins in -> out |
|---|---|---|
| `setsoftpressure` | Exec | entity, kPa -> success |
| `getsoftvolume` | Pure | entity -> found, restCm3, nowCm3, ratio |
| `grabsoftbody` | Exec | entity, x, y, z, reach, targetX, targetY, targetZ -> grab:int, success |
| `movesoftgrab` | Exec | entity, grab, x, y, z -> success |
| `releasesoftgrab` | Exec | entity, grab -> success |
| `setsoftbuoyancy` | Exec | entity, density, drag -> success |
| `getsoftsubmerged` | Pure | entity -> found, fraction, volumeCm3 |
| `setsoftcontact` | Exec | entity, softScale, otherScale -> success |
| `setsoftpairmode` | Exec | entity, other, mode:int -> success |
| `addsoftjelly` | Exec | entity, preset:int -> success |

**Family `Rope`** (new: `GraphRopeNodeTable.cs`, `GraphNodeDefsRope.hpp`, `docs/nodes/Rope.md`, `RopeNodeTests.cs`): `createrope` (x1,y1,z1,x2,y2,z2,length,radius -> entity, success), `setropeend` (rope, end:int, anchor entity, ox,oy,oz -> success), `freeropeend` (rope, end:int, x,y,z, pinned:bool -> success), `cutrope` (rope, x,y,z -> other:entity, success), `getrope` (Pure: rope -> found, length, strain, nodes, cut), `destroyrope` (rope -> success). Ropes cross the graph as the entity id, never as a Jolt handle.

### 2.8 Editor (soft bodies)

- **Add Component** rows `Jelly` and `Rope` (`PlacedComponentModel.hpp:66`); Jelly needs a mesh placement, Rope needs none (it adds its own placeholder mesh and shows two gizmo handles for the ends in the viewport; dragging an end rewrites `startOffset`/`endOffset`). The legacy `.ocmap` cannot store COMPONENT records, as for every runtime component (`PlacedComponentModel.hpp:199`).
- **Details sections** in `SandboxComponentDetails.cpp` for `CSoftVolume`, `CSoftRope` and a "Pressure" block on `CCloth` (a field writes the relay op; a closed-mesh check greys it with the reason). Live OUT fields (volume ratio, strain, length) update during Play.
- **Show > Soft Constraints** overlay (extends `SoftBodyDebugOverlay.hpp`; `rhi::LineVertex` lines both backends draw): LRA as thin lines coloured by slack, rods as short frames (the three axes from `sbx_rod_state`), lattice tetrahedra as wire, attachments as small crosses, sensor pairs as dashed boxes. Editor-only (`!noEditorChrome_`, `SandboxRender.cpp:1995-2021`).
- **Content Browser:** `.ocsoft` of kind jelly and rope open in the existing `.ocsoft` tab with the new sections.
- **Test content:** `test-content/SoftBodyLab` gains `Balloon` (a closed UV sphere with a string pinned to an entity), `Jelly` (a cube and a torus), `Rope` (a hanging rope, a rope between two cubes, a cable on a kinematic crane), regenerated by `make_soft_lab.py` (extended; checked-in OBJ sources like the existing assets).


---

## Part B. Strand hair

### 2.9 Modules and the boundary rule

`modules/physics/README.md` and `modules/physics.jolt/README.md` say `Aver.Physics` links Jolt PRIVATE, depends on Core only, and that a second Jolt consumer would mean the rule stopped holding. `HAIR_JOLT_EVALUATION.md` section 3 found that `JPH::Hair` breaks it twice (it needs a `PhysicsSystem&` and a `ComputeSystem*` in one call). The resolution here keeps both edges:

- `JPH::Hair`, `HairSettings`, `HairShaders` and the `PhysicsSystem` all live **inside `Aver.Physics`** (new `PhysicsHair.cpp`). No second Jolt consumer exists. `tests/physics` stays the one permitted second consumer.
- The `ComputeSystem` Jolt needs is a thin adapter **inside `Aver.Physics`** (`HostComputeSystem`) that forwards to a **C function table the host installs** (`AverPhysComputeHostV1`, 2.10). `Aver.Physics` still links Core only and no RHI; a headless server build carries no GPU code, and runs the sim on Jolt's CPU backend if it wants hair at all.
- The RHI side lives in a new render module that implements the table and never sees a `JPH::` type.

| Module | Kind | Depends on | Owns |
|---|---|---|---|
| `Aver.Hair` (`modules/hair`) | SHARED or STATIC pure | Core, Formats, Physics (C ABI only) | `OcHair` asset, strand generator, curve import helpers, proxy hull points, settings builder driving `aver_phys_hair_*`, built-in material presets |
| `Aver.Hair.Scene` (`modules/hair.scene`) | SHARED | Core, Scene, Hair, Physics | `CHair`, `HairSystem`, `HairView`s for the draw side, the framework provider |
| `Aver.Render.Hair` (`modules/render.hair`) | STATIC | Core, RHI, Hair.Scene (headers); physics **headers only** | `RhiComputeHost`, `HairScene` (`IRenderFeature`), ribbon expansion, LOD parts, the Aver HLSL |

`modules/hair/module.json` etc. follow the manifest rule (`cmake/AvModule.cmake`, `aver_check_module_dag`); `scripts/module-guard-audit.py` and `module-matrix.ps1` stay green because every edge above exists in the manifests. The composition root (the two hosts, through `GameSystemsWiring.hpp`) is the only place that passes `rhiComputeHostTable()` to `aver_phys_hair_set_compute_host`.

### 2.10 The compute host, and why not Jolt's DX12 and Vulkan backends

**Decision.** Jolt's DX12 and Vulkan compute backends stay OFF (`JPH_USE_DX12`, `JPH_USE_VK` at `physics.jolt/CMakeLists.txt:63-65` are not changed). `JPH_USE_CPU_COMPUTE` is turned ON in the same file (one new `set(JPH_USE_CPU_COMPUTE ON)` line and its comment); it adds only `Compute/CPU/*` and `Shaders/HairWrapper.cpp` (`Jolt.cmake:541-564`) and no tool dependency. Jolt **can** share Aver's device (both backends take an external one: `ComputeSystemDX12::Initialize(ID3D12Device*)`, `ComputeSystemVKWithAllocator::Initialize(...)`), but sharing it is the wrong trade, for these verified reasons:

| Reason | Evidence |
|---|---|
| Jolt's D3D12 queue is a private `COMPUTE` command queue with its own allocator, list and fence, and `Wait()` blocks the CPU on the fence. The renderer's frame has no way to order after it except a CPU wait | `ComputeQueueDX12.cpp:23-52,172-205` |
| On Vulkan, Jolt calls `vkGetDeviceQueue(device, family, 0)` for the compute family; Aver's device creates exactly one queue in its graphics family (`queueCount = 1`), so both would submit to the same `VkQueue`, which Vulkan requires to be externally synchronised | `ComputeQueueVK.cpp:36`, `VulkanDevice.cpp:830-831` |
| Jolt tracks each buffer's resource state itself (`mCurrentState`) and leaves buffers in `UNORDERED_ACCESS` or `NON_PIXEL_SHADER_RESOURCE`; Aver's RHI also tracks state ("nothing transitions implicitly", `RHIResources.hpp` `ResourceState`). Two trackers over one resource are a corruption bug | `ComputeBufferDX12.cpp:77-93` |
| The RHI has no way to import an external buffer or to hand out its native device, so the ribbon pass could not read `GetRenderPositionsCB()` without new RHI API on both backends | `RHI.hpp`, `RHIResources.hpp` (no native accessors) |
| Jolt.cmake builds the HLSL with the Windows SDK `dxc` and `find_package(Vulkan)` (the Vulkan SDK), which Aver deliberately does not need (`docs/VULKAN.md` "Why no SDK is needed") and writes `.dxil`/`.spv` next to the vendored sources. Aver already ships a SPIR-V-capable DXC (`third_party/dxc-spirv`) and a DXIL/SPIR-V pipeline for its own shaders | `Jolt.cmake:567-647,719-775`, `CMakeLists.txt:82-85` |
| The scene-pass compute limit (D3D12-only) and the frame-in-flight ring rules already belong to the RHI; a second submission path would duplicate them | `RHI.hpp:101-104` |

The adapter gives D3D12 and Vulkan parity from a single code path (everything below the host table is the RHI's), makes the output an ordinary `rhi::BufferHandle` the ribbon pass binds with `setSrvBuffer`, records the whole sim in `prePass` like `SkinningPass`, and removes every CPU stall (`ReadBackGPUState` is unavailable on the GPU host and used only by CPU-host tests). The cost is one adapter pair (about 700 lines in `Aver.Physics`, about 600 on the RHI side) and the shader wrappers of 2.11. If the owner still wants Jolt's own backends (open question 1), the follow-up is exactly the five missing pieces in the table plus a native-handle RHI API; nothing else in this design changes, because everything above the host table is backend-neutral.

**The table** (`physics_hair_abi.h`; plain C; handles are opaque `uint64_t` the host chooses; `0` is failure):

```c
typedef uint64_t AverPhysHostHandle;
#define AVER_PHYS_CB_UPLOAD    0   /* CPU writes, GPU reads; Map(write) discards; ringed by the host */
#define AVER_PHYS_CB_READBACK  1   /* GPU writes, CPU reads (debug; a GPU host may return NULL from map) */
#define AVER_PHYS_CB_CONSTANT  2   /* small; the host snapshots it at bind time */
#define AVER_PHYS_CB_BUFFER    3   /* read-only after creation, must carry data */
#define AVER_PHYS_CB_RW        4   /* read/write on the GPU, optional initial data */
#define AVER_PHYS_BIND_CONSTANT 0
#define AVER_PHYS_BIND_READ     1
#define AVER_PHYS_BIND_RW       2

typedef struct AverPhysComputeHostV1 {
    uint32_t structSize;            /* sizeof, checked */
    uint32_t version;               /* 1 */
    void*    user;
    AverPhysHostHandle (*create_shader)(void* user, const char* name, uint32_t gx, uint32_t gy, uint32_t gz);
    AverPhysHostHandle (*create_buffer)(void* user, int32_t type, uint64_t count, uint32_t stride, const void* data);
    void*              (*map)(void* user, AverPhysHostHandle buffer, int32_t write);
    void               (*unmap)(void* user, AverPhysHostHandle buffer);
    void               (*release)(void* user, AverPhysHostHandle object);          /* shader, buffer or queue */
    AverPhysHostHandle (*create_queue)(void* user);
    void (*set_shader)(void* user, AverPhysHostHandle queue, AverPhysHostHandle shader);
    void (*bind)(void* user, AverPhysHostHandle queue, const char* name, AverPhysHostHandle buffer, int32_t role, int32_t barrier);
    void (*dispatch)(void* user, AverPhysHostHandle queue, uint32_t gx, uint32_t gy, uint32_t gz);
    void (*readback)(void* user, AverPhysHostHandle queue, AverPhysHostHandle dst, AverPhysHostHandle src);
    void (*execute)(void* user, AverPhysHostHandle queue);   /* seal what was recorded */
    void (*wait)(void* user, AverPhysHostHandle queue);      /* GPU host: returns at once; CPU host: already done */
} AverPhysComputeHostV1;

AVER_PHYS_API int32_t aver_phys_hair_set_compute_host(const AverPhysComputeHostV1* host);   /* NULL = Jolt's CPU backend */
AVER_PHYS_API int32_t aver_phys_hair_host_kind(void);                                         /* 0 CPU, 1 installed */
```

Rules: install before the first `aver_phys_hair_settings_finish` (refused afterwards while settings exist; the host kind is a world property and `aver_phys_shutdown` resets it). `create_shader` is name-based: the names are Jolt's fifteen (`HairIntegrate`, ...) plus Aver's `AverHairWind`; an unknown name returns 0. `bind` carries Jolt's name (`gPositions`) and the role; the host resolves the register from the generated table of 2.11 for the currently set shader.

**`HostComputeSystem` inside `Aver.Physics`** (`HairComputeHost.cpp/.hpp`): `class HostComputeSystem final : public JPH::ComputeSystem` (RTTI declared the way `ComputeSystemCPU` does, `ComputeSystemCPU.h:20`) with `HostShader`, `HostBuffer` (`MapInternal` -> `host->map`, `CreateReadBackBuffer` -> `create_buffer(READBACK, same size)`, destructor -> `release`) and `HostQueue` (`SetShader`/`Set*Buffer`/`Dispatch`/`ScheduleReadback`/`Execute`/`Wait` forward one to one; a `ComputeQueue` keeps a reference on every bound buffer as Jolt requires, so the adapter holds `Ref<HostBuffer>`s until `Wait`). With no host installed the same entry points construct Jolt's `ComputeSystemCPU`, call `HairRegisterShaders` (and `AverHairWind`'s registration) and use `ComputeQueueCPU`. One compute system and one queue per world; `aver_phys_hair_flush` is the only caller of `Execute`/`Wait`.

**`RhiComputeHost` inside `Aver.Render.Hair`** implements the table over `IResourceFactory` and `IRenderContext`:

- Buffers are `BufferDesc` `Default` (with `allowUnorderedAccess` for RW), `Upload` for UPLOAD and CONSTANT staging, `Readback` for READBACK. A BUFFER with data is created `Default` and filled by a `copyBuffer` from a transient `Upload` buffer at the head of the first replay that references it; the staging is destroyed when that frame retires (the retire-during-recording rule).
- **Upload ring.** UPLOAD buffers hold `kHostUploadRing = 6` slots (3 frames in flight + 2 sealed batches + 1; a `static_assert` ties it to `kSoftBodyFramesInFlight`-style constants). `map(write)` advances the slot and returns its memory; `bind` records the slot index with the op, so a replay reads the slot that was current when `Hair::Update` mapped it. A CONSTANT buffer is copied into the recorded op by value (about 300 bytes; the per-iteration block is 8 bytes).
- **Recording.** `set_shader`, `bind`, `dispatch` append ops to the queue's open list; `execute` seals the list into the host's pending batches (at most 3; older batches are dropped with a counter, the sim then simply runs fewer steps, which only happens when nothing renders); `wait` is a no-op. `RhiComputeHost::replay(IRenderContext&)` is called by `HairScene::prePass` and executes the sealed batches in order.
- **Binding sets** are created once per `(shader, ordered buffer tuple)` and cached; they are never rewritten, which is the rule the `SkinnedMeshGpu` ring exists to enforce (`SkinningPass.hpp:48-60`). The number of distinct tuples is bounded: shaders x ring slots of the upload buffers that vary. A set is destroyed when any member buffer is released.
- **Registers.** A shader's layout is `srvCount`/`uavCount` from the generated table; every slot is a `StructuredBuffer` (`slotKindsDeclared = true`). Constants: Jolt's `gContext` is a CBV at slot 3 (`constantDwords[3] = 0`, `setConstantBuffer(3, ...)`), `gIterationContext` at slot 4 (a CBV, or two root constants if a compute layout may not declare slot 4: UNVERIFIED, decided in phase G; `kFeatureFrameConstantRegister = 4` is declarable by contract, `RHIResources.hpp` b0..b4 comment). Slot 0 and the others are untouched (the skinning pass declares slot 3 alone, `SkinningPass.cpp:15,63`).
- **Barriers.** The host tracks each buffer's state (`Common` -> `NonPixelShaderResource` for READ, `UnorderedAccess` for RW) and emits `bufferBarrier` before a dispatch whose role differs, `uavBarrierBuffer` where Jolt asks for a RW barrier on a buffer already in UAV state (Jolt's `EBarrier::Yes`, the default), mirroring `ComputeQueueDX12::SetRWBuffer` (`ComputeQueueDX12.cpp:92-104`). UPLOAD buffers are GenericRead and never transitioned. `RhiComputeHost::ensureState(handle, state)` lets the ribbon pass move the render-positions buffer to `NonPixelShaderResource`.
- Dispatch count per update is about `9 + 4N` (2.4 in the evaluation): 33 at 60 fps with six iterations, 57 at the 30 fps clamp. Each is one `dispatch` with its own `setBindingSet`/`setConstantBuffer`; the cost on the CPU is a few microseconds each (UNVERIFIED).
- Device loss and shutdown: the host releases everything in `release`; `aver_phys_hair_set_compute_host(NULL)` is refused while instances exist, so the composition root destroys `HairSystem` instances first (the order `resetGameSystems` already uses for cloth).

**CPU host** (Jolt's `ComputeSystemCPU`): runs the same HLSL as C++ on the calling thread. It is the reference for tests and the fallback where `caps().computeShaders` is false. Jolt calls it "not optimized for performance"; `HairSystem` caps a CPU-hosted groom at `cpuMaxSimStrands = 64` and refuses larger ones (open question 3).

### 2.11 Shaders

**Jolt's fifteen kernels are compiled unchanged from `Jolt/Shaders/*.hlsl`** by Aver's pipeline (`ShaderDesc{source, prelude, entry="main", stage=Compute, minShaderModel=60}`, DXIL on D3D12, SPIR-V on Vulkan, same blob cache). For each kernel a one-line Aver wrapper `shaders/hair/jolt/<Name>.hlsl` does:

```hlsl
#define JPH_SHADER_OVERRIDE_MACROS
#include "AverHairMacros.hlsli"          // the ShaderCore.h macro set with explicit registers (below)
#include "<Name>.regs.h"                  // GENERATED: #define AVER_REG_gPositions u0 ... for this kernel
#include "Jolt/<Name>.hlsl"               // the vendored file, untouched
```

`AverHairMacros.hlsli` is the macro block of `ShaderCore.h:5-72` copied once with these changes only: `JPH_SHADER_BIND_BUFFER(type, name)` becomes `StructuredBuffer<type> name : register(AVER_REG_##name);`, `JPH_SHADER_BIND_RW_BUFFER` likewise, and `JPH_SHADER_CONSTANTS_BEGIN(type, name)` becomes `cbuffer name : register(AVER_REG_##name) {`. The generated header maps each bound name to `t<k>`, `u<k>` or `b3`/`b4` in **declaration order within its class**, which is what DXC would assign anyway and what the RHI's contiguous slot ranges need. The generator is `scripts/gen-hair-shader-regs.py` (inputs: Jolt's `*Bindings.h`; outputs: the per-kernel `.regs.h` and the C++ table `render.hair/src/HairRegs.inc` with `{shader, name, kind, slot}` rows). It is run by hand when Jolt is updated, its outputs are checked in, and `HairRegsTest` (headless) re-derives both from the vendored headers and fails on a difference, so a Jolt update that moves a binding cannot pass silently.

The wrapper directory `shaders/hair/jolt/` also receives, by a build copy step (`scripts/stage-payload.ps1` already copies shaders), the vendored headers the kernels include (`HairCommon.h`, `HairStructs.h`, `Shader*.h`, ...), flat, because the engine's shader namespace is flat (`aver-flat-shader-namespace`). The MIT notice for these files is carried by `THIRD-PARTY-NOTICES.txt` through the existing Jolt `LICENSE` flow (`scripts/stage-payload.ps1`, as `HAIR_JOLT_EVALUATION.md` section 4 records).

**Aver-owned HLSL** (new, `modules/render.hair/shaders/`):

| File | Job |
|---|---|
| `AverHairWind.hlsl` (also compiled as C++ through Jolt's CPU wrappers, `HairWindWrapper.cpp` in `Aver.Physics`) | adds wind to Jolt's velocity buffer before `Hair::Update`'s first dispatch (2.15). Idiom: written with the `JPH_SHADER_*` macros so one source runs on GPU and CPU |
| `AverHairRibbons.hlsl` | expands render positions into camera-facing ribbons (2.16) |
| `AverHairMacros.hlsli`, generated `*.regs.h` | above |

### 2.12 The asset: `.ochair`

A new Oc* format in `modules/formats` (`OcHair.hpp/.cpp`), the AVR1 container used by `OcLand.hpp`/`OcBt.hpp`/`OcFoliage.hpp` (a required header chunk plus binary chunks, `Avr1.hpp`): strand data is binary because 20k strands of 12 points is 2.9 MB of floats; every authored parameter is text so a diff reads. Chunks:

| Chunk | Content |
|---|---|
| `HEAD` (required) | OC text, records below |
| `STRN` (optional) | `u32 strandCount, pointCount`; `f32 points[pointCount * 3]` (cm, mesh-local of the scalp mesh, engine axes); `u32 start[strandCount + 1]`; `u8 material[strandCount]`; `f32 width[strandCount]` optional (cm; absent = material width) |
| `SCLP` (optional) | an embedded scalp (positions, triangles, 4 joint indices and weights per vertex, joint name table) so a generated groom is self-contained; absent = the `SCALP` record names a mesh |

`HEAD` records (one per line, `#` comments, unknown records ignored, a newer `OCHAIR` number refused; names follow `.ocmat` PARAM spelling where one exists):

```
OCHAIR 1
SCALP Meshes/Head.ocmesh [SUBMESH n]         # triangles, and joints and weights if the mesh is skinned
SCALPJOINTS head neck_01 spine_03             # names; the order is the skin weight index; resolved at attach
SIM iterationsPerSecond 360 maxDeltaTime 0.0333 grid 32 32 32 boundsPaddingCm 10 simFraction 0.1 resample 12
MATERIAL 0 collision 1 lra 1 linearDamping 2 angularDamping 2 maxLinearCmS 1000 maxAngularRadS 50
          gravity 0.1 1.0 0.2 0.8 friction 0.2 bendCompliance 1e-7 bendMul 1 100 100 1
          stretchCompliance 1e-8 inertiaMul 10 radiusCm 0.1 0.1 worldInfluence 0 1 gridVelocity 0.05 0.01
          gridDensityForce 0 globalPose 0.01 0 0 0.3 skinGlobalPose 1 0 0 0.1 gravityPreload 0
LOOK 0 baseColor 0.35 0.22 0.12 tipColor 0.55 0.40 0.25 colorVariation 0.15 widthRootCm 0.12 widthTipCm 0.04
         roughness 0.35 shift 0.07 tt 1.0 trt 1.0 azimuthRough 0.4 tint 0.9 0.6 0.4 scatter 0.5
GENERATE seed 12345 strands 18000 lengthCm 28 42 segments 12 curlCm 1.2 curlFrequency 3 clumps 220 clumpRadiusCm 1.5 clumpTightness 0.6
         partDirection 0 1 0 partWidthCm 0.8 style 0 0 -1 styleWeight 0.7 mask Textures/HairMask.octex
LOD distances 300 700 1500 strandFractions 1 0.5 0.25 0.12
```

- Gradient values (`gravity`, `radiusCm`, `globalPose`, ...) are `min max [minFraction maxFraction]`, mapped to `HairSettings::Gradient` (`HairSettings.h:112-140`). `radiusCm` and the velocity caps are converted to metres at settings build; the Jolt fields keep their meaning (`HairSettings::Material`, `HairSettings.h:187-228`).
- `LOOK` is render-only (the hair material of 2.17). `LOD` is render-only (2.16).
- `GENERATE` is deterministic from `seed`; a groom with `GENERATE` and no `STRN` is regenerated at load (a few milliseconds per 10k strands, UNVERIFIED), with `STRN` the explicit strands win and `GENERATE` is ignored. Parting, style and clumping are described in 2.13.
- `valid()` refuses: more than 255 vertices per strand or materials, a strand of fewer than 3 points, more than `maxRenderStrands` (default 40000), non-finite values, a `SCALPJOINTS` count above 16, a `SCALP` that is not an `.ocmesh`.

### 2.13 Authoring sources (the engine has no curve importer; this is the minimal set)

1. **Generator** (pure, `Aver.Hair`, `HairGenerate.hpp`). Input: the scalp mesh (positions, triangles, optional per-vertex mask from a texture or a vertex-colour-like `.octex` R channel) and the `GENERATE` record. Roots: area-weighted barycentric sampling on the scalp triangles weighted by the mask, from a PCG32 seeded stream, with a minimum spacing by dart rejection (spacing from strand count and scalp area). Each strand grows along a direction that blends the triangle normal into `style` by `styleWeight` along its length (a "fall" vector for long hair); clump centres are drawn among the roots and member strands pull toward their clump's curve by `clumpTightness` (the usual guide-and-follower construction); curl is a helix of amplitude `curlCm` and frequency per length; a parting line pushes roots' initial direction to either side. All of it is textbook procedural placement written from this description; no external code.
2. **glTF curves** (`GltfImport.cpp:375-376` today logs "only triangles are imported"). `mode` 1 (LINES) and 3 (LINE_STRIP) primitives are read through the accessor code the triangle path already uses; consecutive segments sharing endpoints form a strand; the glTF material index becomes the strand material; per-vertex width is read from a `_WIDTH` custom attribute when present. Output is `ImportedHairCurves` (a plain struct beside `ImportedCamera.hpp`); a tool command `AverAssetC hair <file.gltf> --scalp <mesh.ocmesh> --out <groom.ochair>` writes the `.ochair` with a default `SIM` and `MATERIAL 0`. The Content Browser import dialog offers it for files that contain curves.
3. **Authored text**: write `HEAD` with `GENERATE`, or the Groom tab (2.20).
4. **USD `BasisCurves`** (phase H): the USD importer is Aver's own (`UsdCrate.hpp`/`UsdImport.hpp`); reading the `curveVertexCounts` and `points` attributes of a `BasisCurves` prim and its `widths` is a few hundred lines. Not v1-blocking (open question 5).
5. **Alembic: no** (a new dependency; owner rule). **Hair cards** (mesh strips with an alpha texture) need no new code: they are ordinary skinned meshes and a LOD fallback is open question 4.

The scalp is the critical authoring input: Jolt's `Init` requires every root within reach of a scalp triangle and reports `outMaxDistSqHairToScalp` (`HairSettings.h:239-244`); `settings_finish` returns it and the importer fails with a clear message above 2 cm.

### 2.14 Physics ABI: `physics_hair_abi.h` (`AVER_PHYS_HAIR_ABI_VERSION 1`)

New header; sources `PhysicsHair.cpp` (instances, update, wind, proxies bookkeeping), `PhysicsHairSettings.cpp` (settings builder, conversion to Jolt space, `Init`), `HairComputeHost.cpp/.hpp`, `HairInternal.hpp`. Units are the engine's at the boundary and converted once. Returns 1/0 unless it says handle or count. Calls come from the main thread between `aver_phys_step` calls (hair reads the broad phase), never from a listener or a step callback.

```c
#define AVER_PHYS_HAIR_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_hair_abi_version(void);

/* compute host: see 2.10 (AverPhysComputeHostV1, aver_phys_hair_set_compute_host, aver_phys_hair_host_kind) */

/* ---- settings (shared by instances) ---- */
AVER_PHYS_API int32_t aver_phys_hair_settings_begin(void);                                                   /* builder id */
/* Render strands. points: cm, scalp-mesh space, engine axes (x,y,z per vertex). strandStart has strandCount + 1 entries.
   invMass: per vertex or NULL (1 each; 0 pins). */
AVER_PHYS_API int32_t aver_phys_hair_settings_strands(int32_t s, const float* pointsXyz, const float* invMass, int32_t vertexCount,
                                                      const int32_t* strandStart, const int32_t* strandMaterial, int32_t strandCount);
/* The scalp. joints4/weights4: 4 per vertex, joint = index into the scalp joint list (0..jointCount-1). */
AVER_PHYS_API int32_t aver_phys_hair_settings_scalp(int32_t s, const float* vertsXyz, int32_t vertexCount, const int32_t* tri3, int32_t triCount,
                                                    const int32_t* joints4, const float* weights4, int32_t jointCount);
AVER_PHYS_API int32_t aver_phys_hair_settings_material(int32_t s, int32_t index, const float* p, int32_t n);  /* AVER_PHYS_HAIRM_* */
AVER_PHYS_API int32_t aver_phys_hair_settings_params(int32_t s, const float* p, int32_t n);                   /* AVER_PHYS_HAIRP_* */
/* InitRenderAndSimulationStrands, sResample (0 = none), Init, InitCompute on the installed host. Returns the settings handle.
   outRootToScalpCm: the largest root-to-scalp distance. May take 0.2 to 1 s for 20k strands (UNVERIFIED): call on a worker. */
AVER_PHYS_API int32_t aver_phys_hair_settings_finish(int32_t s, int32_t resampleVertices, float* outRootToScalpCm);
AVER_PHYS_API int32_t aver_phys_hair_settings_cancel(int32_t s);
AVER_PHYS_API int32_t aver_phys_hair_settings_destroy(int32_t settings);                       /* refused while an instance uses it */
AVER_PHYS_API int32_t aver_phys_hair_settings_set_material(int32_t settings, int32_t index, const float* p, int32_t n);  /* live: re-read every update */
AVER_PHYS_API int32_t aver_phys_hair_settings_info(int32_t settings, int32_t* outI, int32_t nI, float* outF, int32_t nF); /* AVER_PHYS_HAIRI_* / _HAIRF_* */
/* Render strand table, in the order the render positions buffer holds them: start vertex and vertex count per strand. */
AVER_PHYS_API int32_t aver_phys_hair_settings_render_strands(int32_t settings, int32_t* outStartCount2, int32_t maxStrands);

/* ---- instances ---- */
/* world16: the hair space to world matrix, row-major engine convention (the head entity's world). layer: user layer 0..15. */
AVER_PHYS_API int32_t aver_phys_hair_create(int32_t settings, const float* world16, int32_t layer);          /* hair handle, 0 on refusal */
AVER_PHYS_API int32_t aver_phys_hair_destroy(int32_t hair);
AVER_PHYS_API int32_t aver_phys_hair_set_transform(int32_t hair, const float* world16);
AVER_PHYS_API int32_t aver_phys_hair_teleport(int32_t hair);                                                  /* default pose, zero velocity, next update */
/* The engine skinning matrices (model x inverse bind) of the scalp's joints, in scalp joint order. Identity inverse binds are
   baked into the settings, so these are used as the joint matrices unchanged. */
AVER_PHYS_API int32_t aver_phys_hair_set_palette(int32_t hair, const float* joints16, int32_t jointCount);
AVER_PHYS_API int32_t aver_phys_hair_set_wind(int32_t hair, const float* windCmS3, float dragPerS, float turbulenceCmS);
/* Records one simulation step for this hair (Hair::Update) into the shared queue. dt is the frame time; Jolt clamps it. */
AVER_PHYS_API int32_t aver_phys_hair_update(int32_t hair, float dt);
/* Seals what the updates of this tick recorded (Execute + Wait). Once per tick after all updates. */
AVER_PHYS_API int32_t aver_phys_hair_flush(void);
/* The host handle of the render positions buffer (float3 per render vertex, hair-local JOLT space: metres, Y up, right-handed). */
AVER_PHYS_API uint64_t aver_phys_hair_render_buffer(int32_t hair);
AVER_PHYS_API int32_t aver_phys_hair_info(int32_t hair, int32_t* outI, int32_t nI, float* outF, int32_t nF);  /* last update: iterations, collision shapes, dispatches */
/* CPU host only (debug and tests): copies out the simulated vertices (cm, hair space, engine axes) and render positions. */
AVER_PHYS_API int32_t aver_phys_hair_read_positions(int32_t hair, float* outSimXyz, int32_t maxSim, float* outRenderXyz, int32_t maxRender);
```

**Parameter blocks** (engine units; a short block keeps the defaults of the rest, a longer one is refused):

- `AVER_PHYS_HAIRP_*` (settings): 0 `ITERATIONS_PER_SECOND` 360; 1 `MAX_DELTA_TIME` 0.0333; 2 `GRID_X`, 3 `GRID_Y`, 4 `GRID_Z` 32; 5 `BOUNDS_PADDING_CM` 10; 6 `SIM_FRACTION` 0.1 (the default of material `mSimulationStrandsFraction`, which a `MATERIAL` record overrides); 7,8,9 `INITIAL_GRAVITY_X/Y/Z` cm/s^2 engine axes (0, 0, -981); `COUNT` 10.
- `AVER_PHYS_HAIRM_*` (per material, 46 floats): 0 `ENABLE_COLLISION`, 1 `ENABLE_LRA`, 2 `LINEAR_DAMPING`, 3 `ANGULAR_DAMPING`, 4 `MAX_LINEAR_CMS`, 5 `MAX_ANGULAR_RADS`, 6-9 `GRAVITY` gradient, 10 `FRICTION`, 11 `BEND_COMPLIANCE`, 12-15 `BEND_MULTIPLIER`, 16 `STRETCH_COMPLIANCE`, 17 `INERTIA_MULTIPLIER`, 18-21 `RADIUS_CM` gradient, 22-25 `WORLD_INFLUENCE`, 26-29 `GRID_VELOCITY`, 30 `GRID_DENSITY_FORCE`, 31-34 `GLOBAL_POSE`, 35-38 `SKIN_GLOBAL_POSE`, 39 `SIM_FRACTION`, 40 `GRAVITY_PRELOAD`, `COUNT` 41. (Each gradient is `min max minFraction maxFraction`.) `AbiEnumTest` pins both blocks against `Hair.cs`.
- `AVER_PHYS_HAIRI_*` (info ints): 0 sim strands, 1 sim vertices, 2 render strands, 3 render vertices, 4 max vertices per strand, 5 materials, 6 grid cells. `HAIRF_*`: 0 root-to-scalp cm, 1..6 bounds min/max (cm, hair space).

**What `aver_phys_hair_update` does, in order** (all on the calling thread): (1) if wind is set and non-zero, records `AverHairWind` over the instance's velocity buffer (`Hair::GetVelocitiesCB`); (2) `Hair::Update(dt, identityJointToHair, joints, system, shaders, computeSystem, queue)` with the palette converted by `toJolt(Mat4)` (the 4x4 conversion at `Convert.hpp:107-125`; the metre scale is in the translation only, the same conversion `pushPalette` relies on for soft-body skinning); (3) nothing else: no Execute. `set_transform` converts the world matrix with `toJolt(Mat4)`, takes position and rotation and calls `Hair::SetPosition/SetRotation`; a non-uniform or non-unit scale in `world16` is stripped (2.21). `layer` becomes `Layers::encode(layer, true)`.

**What does not cross the ABI:** the strand state (it lives in host buffers), Jolt types, the velocity grid. Public Jolt getters are used inside `Aver.Physics` only.

### 2.15 Scene: `CHair` and `HairSystem`

```cpp
struct CHair {                       // "CHair", runtime-registered, 64 bytes
    u64 asset = 0;                   // .ochair ObjectId
    u64 material = 0;                // optional .ocmat override for the LOOK
    u32 flags = 0;                   // kHairNoCollision 0x1, kHairNoShadow 0x2, kHairRayTraced 0x4, kHairNoWind 0x8, kHairHidden 0x10
    f32 widthScale = 0;              // 0 reads 1
    f32 densityScale = 0;            // 0 reads 1: scales the render strand fraction drawn
    f32 lodBias = 0;                 // added to the LOD choice, in steps
    f32 windScale = 0;               // 0 reads 1
    f32 stiffnessScale = 0;          // 0 reads 1 (divides bend and stretch compliance through settings_set_material)
    u32 outState = 0;                // OUT: bit0 simulating, bit1 frozen by distance, bit2 inert (no compute), bit3 settings building, bits 8+ LOD level
    u32 outRenderStrands = 0;        // OUT: strands drawn
    u32 outSimStrands = 0;           // OUT
    u32 _pad = 0;
};
static_assert(sizeof(CHair) == 64);
```

The component goes on the entity that owns the skeleton the scalp is skinned to (the character root). `HairSystem::registerComponents(world)` (idempotent), `attachHair(world, e, asset)` writes defaults. The hair needs no mesh on its own entity: the system creates a **child entity** `"Hair"` (identity local, `CMeshRenderer` with a placeholder quad mesh and the `LOOK` material, `CHairDraw` marker component of its own, 8 bytes) that carries the draw; destroying the component destroys the child.

`HairSystem` (singleton, `hairSystem()`; `tick(world, dt)` once per frame after `animSystem().tick`, called from `tickSoftBodies`' neighbour in `GameSystemsWiring.hpp`, plus `clear()`):

1. **Build.** For each `CHair` whose asset resolves: parse the `.ochair` through the asset cache (the `setPathResolver` pattern, `SoftBodySystem.hpp:293`); resolve `SCALPJOINTS` against `animSystem().posedSkeleton(e)` (the joints must exist, otherwise `[Hair] entity N: scalp joint 'x' not in the skeleton` and the hair is inert); read the scalp mesh through the mesh source; run the generator if needed; build the settings **on a worker thread** (`Aver.Hair`'s `buildHairSettings`, which calls `aver_phys_hair_settings_*` and `_finish` and so must be on a thread where the world is not stepping: the system takes the same physics step lock the ragdoll build uses; if there is no such lock it runs `_finish` on the main thread at the first tick with `outState` bit3 set and a one-frame hitch accepted, UNVERIFIED); cache by `(assetId, meshKey)` so ten characters with one groom share settings (`HairSettings` is shared by design, `HairSettings.h:20`). Instances: `aver_phys_hair_create`.
2. **Each tick.** Resolve the head world matrix (the entity's `CWorld`, scale stripped), `aver_phys_hair_set_transform`; gather the scalp joints' skinning matrices (`animSystem().skinning(e, count)` indexed by the skeleton joint index of each scalp joint) into a small array and `aver_phys_hair_set_palette`; wind from `aver_phys_sb_set_wind`'s world wind (`softbody` `g_wind`; the system takes the same `setWind` the soft-body system received, through `HairSystem::setWind`, wired in `GameSystemsWiring.hpp` where `SoftBodies.SetWind` already lands) scaled by `windScale`; then LOD cadence (below); then `aver_phys_hair_update(h, dt)`; after all entities, one `aver_phys_hair_flush()`.
3. **Teleport.** A root that moved more than `teleportCm` (default 200) in one frame, or a character that was disabled and shown, calls `aver_phys_hair_teleport` (the default pose, no spring from the old place). The first tick after build teleports.
4. **LOD cadence for the simulation** (Jolt has none, `Hair.h:25`): distance from the view `d`: `d < 800 cm` update every frame; `800..2500` every second frame with `dt` doubled (Jolt clamps to 1/30 s, so it effectively slows, which is acceptable far away); beyond `2500` or not visible for 2 s: no update (the hair keeps its last pose, `outState` bit1), and a resume after more than 1 s without update teleports. UNVERIFIED thresholds, `HairOptions`.
5. **Collision proxies** (decision 17): `HairSystem` owns per-entity sets of kinematic convex-hull bodies created from `HairProxySpec` records in the `.ochair` (`PROXY name bone shape sphere|capsule|box size...` lines; if none, defaults for a humanoid: head sphere, neck capsule, two shoulder capsules, torso box, derived from the skeleton's `head`/`neck`/`spine`/`shoulder` joints by name, UNVERIFIED names) using only existing ABI: `aver_phys_add_convex_hull(points, ...)` from `proxyHullPoints(kind, size)` (a 32-point sphere or capsule hull, 8-point box), `aver_phys_body_set_motion_type(kinematic)`, `aver_phys_body_set_layer(proxyLayer)`, each tick `aver_phys_body_move_kinematic(body, pose, dt)` from the bone's model matrix times the entity world (`AnimSystem::socketModel`, `AnimSystem.hpp:221`, for named sockets, else the skeleton pose). The layer pair is set once in `install`: `HairOptions{ hairLayer = 13, proxyLayer = 12 }`, `aver_phys_set_layer_collision(proxyLayer, x, 0)` for every `x != hairLayer` so proxies touch nothing but hair; hair collides only with convex hulls (Jolt), so a prop meant to catch hair is also a hull body on the proxy layer (script: `Entity.AddHairCollider`). Cost: a handful of hulls; the gather is a broad-phase AABB query per hair.
6. **Large worlds:** see 2.21.

### 2.16 Rendering the strands

`Aver.Render.Hair::HairScene` is an `rhi::IRenderFeature` (`name() == "Aver.Render.Hair"`) modelled on `SkinnedScene` + `SkinningPass`. It reads `hairSystem().views(count)` (a `HairView` per drawn instance: entity, draw child entity, settings id, `renderBuffer` host handle, strand count, world matrix, width scale, look constants, LOD distances) the way `SoftBodyScene` reads `PieceView`s, and is inert (`ready() == false`, hair draws nothing, one log line) when `!caps().computeShaders`.

**Per settings (static, shared by instances):**
- `points`: `StructuredBuffer<RibbonPoint>` (20 B: `renderVertex`, `prevVertex`, `nextVertex` (render-vertex indices within the strand, clamped at the ends), `strandFraction`, `strandHash`), strands laid out **in rank order**: ranks are a deterministic shuffle (PCG32, seed = asset hash) of the render strands, so any prefix of ranks is a uniform thinning of the groom.
- `width`: per strand root and tip width, cm.
- A source `MeshHandle` made with `createMesh` whose vertices are `2 * pointCount` zeroed `MeshVertex` and whose indices are the ribbon topology: per segment two triangles `(2k, 2k+1, 2k+2)`, `(2k+1, 2k+3, 2k+2)`, all strands concatenated in rank order (6 indices per segment). Four LOD parts via `createPosedPartMesh(posedSource, prefixIndices, count)` over the prefix of 100%, 50%, 25% and 12% of ranks (the `LOD strandFractions`).

**Per instance:** `createSkinTargetMesh(source, &vertexBuffer)` (one compute-written `MeshVertex` buffer of `2 * pointCount * 32 B`), a binding set (`points`, `width`, the host's render buffer as an SRV; the vertex buffer as the UAV), and `kRing = 2` constant ring like `SkinnedMeshGpu`. Memory (arithmetic, not measured): 20k render strands x 12 points = 240k points -> 15.4 MB of vertices, 7.2 MB of indices (shared per settings), 4.8 MB of `points`; 40k strands x 16 points doubles that. Strand-count LOD bounds the draw, not the memory; `maxRenderStrands` (default 40000) bounds the memory, and `HairOptions::vramBudgetMB` (default 96) refuses a groom over it at build (the engine's VRAM budget is tight, SOFTBODY streaming notes).

**`prePass(ctx)`** (the only hook that runs before the scene pass reads what it writes, `SoftBodyScene.hpp:99`), in this order: (1) `rhiComputeHost.replay(ctx)`: the sealed simulation batches (decision 12); (2) for every visible instance: `ensureState(renderBuffer, NonPixelShaderResource)`, `bufferBarrier(vertexBuffer, from, UnorderedAccess)`, `setPipeline(ribbon)`, `setBindingSet`, `setConstantBuffer` (below), `dispatch(ceil(prefixPoints / 64), 1, 1)`, `bufferBarrier(vertexBuffer, UnorderedAccess, GeometryRead)` (the state a skinned mesh holds so the input assembler, the shadow pass and a BLAS build can all read it, `RHIResources.hpp` `GeometryRead`). The LOD level is chosen on the CPU from the distance and `lodBias` with 15% hysteresis and passed as the prefix count; there is no indirect dispatch or draw in play (they exist, `RHI.hpp:109`, but a CPU-chosen count needs none). Ribbon pass constants: world matrix, camera position, `pixelWorldAt1cm` (vertical extent of one pixel at 1 cm from the camera, from the frame's projection), `widthScale * lodWidthBoost` where `lodWidthBoost = sqrt(1 / fractionDrawn)` (fewer, fatter strands keep the coverage), `minPixels = 1.0`, point count, the basis constants of the Jolt-to-engine conversion.

**`AverHairRibbons.hlsl`** (per ribbon point, group size 64): read `p = renderPositions[renderVertex]`, `pPrev`, `pNext`; convert `(x, y, z)_Jolt` to engine `(-z, x, y) * 100` (the inverse of `toJolt`, `Convert.hpp:21-32`) and transform by the hair world matrix; tangent `T = normalize(pNext - pPrev)`; width `w = lerp(rootWidth, tipWidth, fraction) * widthScale * lodWidthBoost`, `wPx = distance * pixelWorldAt1cm * minPixels`, drawn width `W = max(w, wPx)`, `coverage = w / W`; side `S = normalize(cross(T, toCamera))` (a near-parallel view falls back to the previous point's side); write two `MeshVertex`: position `P +- S * W/2`, normal slot = `T` (world, unit), uv = `(side * 2 - 1, float(strandHash8) + fraction * 0.999)`. The coverage is not stored; the pixel shader recomputes it from the interpolated across coordinate and the screen-space derivative of the world position (below), so it is exact per pixel. UNVERIFIED: deriving coverage this way costs nothing extra; if it flickers, a third attribute is a vertex-format change and out of scope.

**Draw.** The child entity's draw handle is the LOD part of its instance; the scene walk's handle substitution gets one more link: `skinnedHandle -> softBodyHandle -> hairHandle` (`GameRender.cpp:666-671` and the editor's `SandboxApp::posedHandle`, both shared edits; a zero handle draws the placeholder quad, so a failed build is invisible rather than wrong). The material is the child's `CMeshRenderer.material`, an `.ocmat` with `shadingModel hair` (2.17). Depth: `drawMeshDepthOnly` accepts compute-written meshes (`RHI.hpp:638-650`), so the alpha-tested hair writes depth for the forced-early-depth path the way alpha-masked materials do; the dithered coverage is an alpha test (`AlphaMask` semantics, `gAlphaCutoff` = the dither threshold bias). Shadow maps: the ribbon mesh is an ordinary caster (`kHairNoShadow` opts out). **Ray tracing:** `kHairRayTraced` (default off) builds `createBlasUpdatable` over the LOD 0 part and `refitBlas` each frame like a skinned mesh; with it off, hair casts no RT shadow and is absent from RT reflections and GI (shadow-map shadows still apply). The 15 MB ribbon BLAS is the reason it is off (open question 6). **GI voxelisation:** the hair mesh must be excluded from voxelisation like the AlphaBlend class ("a blended surface is never voxelised", `render.pbr/README.md`); UNVERIFIED whether a material-driven exclusion exists, otherwise a per-entity flag is the smallest addition (phase J checks).

**TAA and motion vectors:** the ribbon mesh is compute-written like a skinned mesh, and the existing path gives skinned meshes the same motion vector treatment it gives rigid ones (UNVERIFIED how well; ghosting on fast head turns is the expected symptom and is the same as skinned characters'). The dither pattern is interleaved gradient noise offset by the frame index so TAA integrates it; with TAA off the hair reads as a screen-door pattern (a known cost, stated in the headed checks).

### 2.17 Shading: `AVER_MODEL_HAIR`

**Selection without touching either backend's per-draw constants.** Today `d.model = gShadingModel` (`material_prelude.hlsl:647`), written as `unlit ? 1 : 0` by `D3D12Device.cpp:366` and `VulkanDevice.cpp:279`. Hair is selected by the **material**: a new flag `MaterialFlag_Hair = 1u << 18` (`MaterialGpu.hpp`) mirrored as `AVER_MAT_HAIR`, and `d.model = (gMaterialFlags & AVER_MAT_HAIR) ? AVER_MODEL_HAIR : gShadingModel`. `MaterialDesc` gains an authored field `shadingModel` (`Standard` default, `Hair`) and a `HairParams` struct with named fields; `packMaterial` writes them into **existing, otherwise unused slots under the flag** (the terrain-layer precedent of reinterpretation, `MaterialGpu.hpp:54`), so the 192-byte block, its five mirrors and both `static_assert`s do not change. `packMaterial` must not set `MaterialFlag_Coat` for a hair material even though it reuses the coat slots (a guard in `MaterialGpu.cpp`: the existing rule is coat only when `coatWeight > 0`, `Material.hpp:215`).

| Hair parameter | Slot reused | Default |
|---|---|---|
| fibre albedo (root), coverage | `baseColorFactor.rgb`, `.a` | LOOK `baseColor`, 1 |
| longitudinal roughness `beta` | `roughnessFactor` | 0.35 |
| R lobe weight | `coatWeight` | 1 |
| azimuthal roughness | `coatRoughness` | 0.4 |
| cuticle tilt `alpha` (radians) | `coatF0` | 0.07 |
| TT weight | `subsurfaceWeight` | 1 |
| TRT weight | `subsurfaceRadius` | 1 |
| TT/TRT tint (linear) | `subsurfaceColor` | LOOK `tint` |
| tip colour | `emissiveFactor` (not emissive under the flag) | LOOK `tipColor` |
| multiple-scatter ambient | `occlusionStrength` | 0.5 |
| colour variation | `metallicFactor` | 0.15 |
| dither bias | `alphaCutoff` | 0.5 |

`RtMaterial` (the ray-hit copy of the block) carries the flag unchanged; a ray hit shades hair with the **standard** model (the ray path has no hair model in v1), which uses these slots as their original meanings (so a TRT tint would leak into SSS in a ray hit): ray-traced hair is off by default, which makes the leak moot, and the RT path masks the flag to standard values (phase J).

**The evaluator** (new function `averHairDirect(AverSurface s, AverLight l, float3 T, float across, float fraction, float hash)` called from `averShadeDirect`'s new `case AVER_MODEL_HAIR`, beside the unlit case at `material_prelude.hlsl:1207`; `averShadeIndirect` gets the matching case). For the hair model `AverVertex.N` carries the strand tangent `T` (the ribbon's normal slot) and the surface builds the fake cylinder normal itself:

- `Vp = normalize(V - T * dot(V, T))` (view perpendicular to the strand), `B = cross(T, Vp)`, `a = across` in [-1, 1]; `N = normalize(Vp * sqrt(1 - a*a) + B * a)`: a ribbon becomes a cylinder to the lighting.
- Per light with direction `L`, half vector `H`: `sinThetaI = dot(L, T)`, `sinThetaR = dot(V, T)` (angles from the plane perpendicular to the strand), `thetaH = (asin(sinThetaI) + asin(sinThetaR)) / 2`, `phi` = the relative azimuth of `L` and `V` projected on the strand-normal plane, `cosHalfPhi = sqrt(max(0, (1 + cosPhi) / 2))`.
- **Longitudinal term**, a normalised Gaussian `M(x, mu, w) = exp(-(x - mu)^2 / (2 w^2)) / (w * sqrt(2 pi))`: R with `mu = -alpha, w = beta`; TT with `mu = +alpha/2, w = beta/2`; TRT with `mu = +3 alpha/2, w = 2 beta` (the standard shifts and width ratios of Marschner et al. 2003; the signs follow the engine's tangent pointing root to tip).
- **Azimuthal terms**, cheap fits: `N_R = 0.25 * cosHalfPhi`; `N_TT = exp(-azR * (pi - |phi|)^2)` with `azR` from azimuthal roughness; `N_TRT = max(0, cos(phi)) * 0.75 + 0.25` (a fit chosen for softness, UNVERIFIED; replaced by a small table if the headed pass shows a hard edge).
- **Colour:** R is uncoloured with `F = fresnelSchlick(dot(V, H), 0.046)` (hair's index 1.55); TT and TRT are `tint * albedo^k` (absorption: TT `albedo`, TRT `albedo^2`) weighted by the TT and TRT weights.
- **Diffuse** (Kajiya-Kay): `sqrt(1 - dot(T, L)^2) * albedo / pi` with a half-lambert wrap of 0.3 so back-lit hair does not go black, times `1 - scatter` where `scatter` blends toward an ambient-like "multiple-scattering" floor.
- **Sum:** `(diffuse + R*N_R*M_R + TT*N_TT*M_TT + TRT*N_TRT*M_TRT) * l.radiance * l.visibility * max(dot(N, L), 0.15)`. The `0.15` floor is the deliberate cheat that stands in for multiple scattering through the head of hair (the dual scattering idea of Zinke et al., published); the headed pass tunes it.
- **Indirect** (`averShadeIndirect`): the standard path with `N` and `albedo`, no specular environment term beyond the R lobe's split-sum fit at `beta`, plus `multiple-scatter ambient` = `albedo * ind.diffuse * occlusionStrength`. Ambient occlusion from the voxel GI is damped by 0.5 because hair is its own occluder.
- Root-to-tip colour is `lerp(albedo, tipColor, fraction)`, per-strand variation `colour *= 1 + variation * (hash8 - 0.5)`.

Energy is not conserved exactly (the azimuthal fits are not normalised); the tuning table in the headed checks lists the furnace test with a white fibre (expect 1.0 +- 0.15). The evaluator is the single implementation for raster and, if ever enabled, rays (`averDirectTerms` pattern), so no second copy.

### 2.18 Wind and the hair-specific passes

Jolt Hair has no wind (`Hair.h:27`). `AverHairWind` runs before `Hair::Update`'s first dispatch: per simulated vertex that is not fixed, `wind` in hair-local Jolt space (the world wind rotated by the inverse hair rotation on the CPU, converted to m/s), `vel += (wind - vel) * (1 - exp(-drag * tipFactor * dt))` with `tipFactor = 0.25 + 0.75 * strandFraction`, plus turbulence from the integer-hash noise of `SoftBodyAero.cpp` (`hash4(strand cell, step block)`), no clock, no rand. It operates on the public `JPH_HairVelocity` buffer (layout `HairStructs.h`) and the settings' `mVerticesStrandFractionCB`; it is the only code that touches Jolt buffers from outside and does so strictly through the getters Jolt exposes. Because it is in the same queue it needs no extra synchronisation. The same source runs on the CPU host through Jolt's HLSL-to-C++ wrapper (decision 12), which is what lets `HairTest` check wind headless.

### 2.19 Patent and licence notes

Searched 2026-10-19 with the web search and the Google Patents pages; results are abstracts and claim 1 only, not a full claim read, and none of this is legal advice. Per the engine rule (clean-room plus a live check before design for anything Epic- or NVIDIA-shaped) the owner should run the USPTO Global Dossier check before a release that ships hair.

| Item | Finding | Design consequence |
|---|---|---|
| **US 11,551,387 B2** (Electronic Arts, "Systems and methods for hair rasterization", priority 2020-09-16, active, expires about 2041) | Software rasterisation of strand segments sorted into per-tile buckets, grouped into depth clusters per tile, nearest cluster rasterised, colour and opacity accumulated, composited as an overlay | **Avoided.** Hair here is hardware triangle ribbons through the ordinary raster path with dithered coverage; there is no tile binning, no per-tile depth clustering and no overlay composite. A future software rasteriser (the 2026 TU Wien line of work) would need this patent re-read |
| **US 10,185,789 B2 / 11,449,649** (Autodesk, "Techniques for modeling elastic rods in position-based dynamics frameworks", priority 2013-12-03, expiry about 2037) | Claim 1: a polyline of the rod centre line with a **ghost point per edge** defining the material frame, new positions of points and ghost points | Jolt's rods (Kugelstadt and Schoemer 2016) carry a **quaternion per rod**, not ghost points; claim 1 reads on neither Jolt Hair nor the soft-body rods. Recorded because the ABI exposes Jolt rods (2.2): if the vendored Jolt ever changes to ghost-point frames this row must be re-read |
| US 12,586,284 (Digital Domain, manipulating simulated hair) | Claim 1 is about displacing groups of strand vertices while preserving shape with shape-preserving constraints; rod constraints only in dependent claims | Not implemented here: Aver has no hair manipulation or transfer tool. Authoring edits of strand points are not constraint solves |
| Hair meshes (Yuksel, patents named on the author's page, numbers not found) | Hair modelling and rendering from a volumetric mesh | Not used: strands are explicit |
| XPBD (NVIDIA patent per a developer's report, not verified) | Jolt's soft bodies and rods are XPBD already; vendored, shipping in every Jolt user | Inherent to the owner's decision to use Jolt; no new exposure from this feature beyond what is already compiled |
| Kajiya and Kay (1989), Marschner et al. (2003), Scheuermann (ATI, 2004), Zinke et al. (dual scattering, 2008) | Publications; the search found no patent claims on them. Scheuermann's polygon hair is the same family of technique as the ribbons here | Used as published ideas; the formulas in 2.17 are written from the papers' descriptions. **Epic's hair shading talk and engine source were not consulted for the formulas** (the clean-room rule) |
| MIT (Jolt) | Every Hair and Compute file carries `SPDX-License-Identifier: MIT` | Notice shipped through the existing `THIRD-PARTY-NOTICES.txt` flow; the generated `.regs.h` and the wrapper `.hlsl` files include the notice header |

### 2.20 C#, Aver Node and editor (hair)

`Aver.Physics`: `Native.Hair.cs`, `Hair.cs` (`HairSettingsBuilder`, `HairSettings`, `HairInstance`, enums for the blocks). `Aver.Framework`: `Hair.cs` and `FwHair.cs` (`framework_hair_abi.h`, `AVER_FW_HAIR_ABI_VERSION 1`: relay ops applied at the next tick):

```csharp
public readonly partial struct Entity
{
    public bool AddHair(string asset);                          // CHair + the draw child
    public bool SetHairLook(string materialAsset);
    public bool SetHairWidthScale(float scale);  public bool SetHairDensityScale(float scale);   // render strand fraction
    public bool SetHairStiffness(float scale);   public bool SetHairWindScale(float scale);
    public bool SetHairEnabled(bool on);                        // kHairHidden
    public bool TeleportHair();
    public bool AddHairCollider(HairColliderShape shape, Vec3 size, string bone);   // a hull proxy on the hair collision layer
    public bool TryGetHair(out HairState state);                // simulating, frozen, lod, render and sim strands
}
```

Aver Node family `Hair` (new: `GraphHairNodeTable.cs`, `GraphNodeDefsHair.hpp`, `docs/nodes/Hair.md`, `HairNodeTests.cs`): `addhair` (entity, asset:string -> success), `sethairparams` (entity, width, density, stiffness, wind -> success), `sethairlook` (entity, material:string -> success), `sethairenabled` (entity, on -> success), `teleporthair` (entity -> success), `addhaircollider` (entity, shape:int, sx, sy, sz, bone:string -> success), `gethair` (Pure: entity -> found, simulating, lod:int, renderStrands:int, simStrands:int), `sethairwind` (world: x, y, z, gust -> success; forwards to the shared wind).

Editor: Add Component `Hair` (needs a skeletal mesh placement, `PlacedComponentModel.hpp:123` rule); Details section (asset, look material, scales, the OUT fields); a **Groom tab** for `.ochair` (generation parameters with a live preview of root placement on the scalp, material and look gradients as the sliders `HairSettings::Gradient` implies, the LOD distances, the proxy list with viewport gizmos, an "Import curves" button); **Show > Hair** overlay (`rhi::LineVertex` lines: guide strands and proxies; the full-strand debug draw of Jolt's `Hair::Draw` is behind `JPH_DEBUG_RENDERER`, which stays off); a Content Browser entry and thumbnail (a rendered head is out of scope: a coloured strand swatch). `templates/` is untouched; the lab content is `test-content/HairLab` (a generated head scalp mesh, three grooms: short straight, long curly, ponytail with a collider, regenerated by `make_hair_lab.py`; a character with a skeleton from the existing test content for the head bone).

### 2.21 Budgets, determinism, large worlds

- **Memory** (arithmetic): per groom of 20k render strands, 2k simulated, 12 points: simulation state about 4 MB (positions 32 B, previous positions 32 B, velocity 24 B, collision plane 16 B per padded simulated vertex), settings buffers 3 MB, render positions 2.9 MB, ribbon vertices 15.4 MB, ribbon indices 7.2 MB shared, points 4.8 MB: about 38 MB first instance, 23 MB each further instance of the same groom. `HairOptions::vramBudgetMB` 96 and `maxInstances` 8.
- **Time** (not measured): simulation about 33 dispatches per update at 60 fps (the evaluation's count); the ribbon pass is one dispatch per visible instance; both are `prePass` GPU work. The CPU side is the broad-phase gather and the Map writes of about 20 KB per update. A GPU timer region `"Hair"` is added to `--gpu-timing` (`aver-gpu-timing-flag`).
- **Determinism.** The generator and the strand ranks are pure functions of the asset and seed. The simulation inherits Jolt's and the GPU's float behaviour: the GPU result is not claimed bit-identical to the CPU host (different summation orders in the grid accumulation are made order-independent by Jolt's fixed-point atomics, `HairCommon.h:12-13`, but float rounding differs), and cross-vendor determinism is not claimed. The CPU host is deterministic for one binary and is what the headless tests use.
- **Large worlds** (owner decision: `JPH_DOUBLE_PRECISION` is in). Soft bodies already hold vertices relative to the body centre (`PhysicsSoftBody.cpp:437-447,548`), so only the body transform is double; the new ABI reads and writes `RVec3`/`RMat44` only through the explicit `JPH::Vec3(com * x)` and `com.Multiply3x3` forms the existing code uses. Hair is simulated in head space; `Hair`'s constructor and `SetPosition` take `RVec3`, which the ABI fills from the float world matrix. The ABI positions are float world centimetres; if the LARGE_WORLD area introduces a rebased origin or `_d` twins for body positions, the two calls that take world positions here (`aver_phys_hair_set_transform`, `aver_phys_sbx_attach*`) get the same treatment by that area's convention and are listed there as consumers. Until then they are exact within the usual float range around the origin (the cloth system has the same limit).
- **Scale.** Entity scale is not simulated: the world matrix handed to the hair is orthonormalised, the ribbon pass multiplies positions by the entity's uniform scale for drawing, so a scaled character draws scaled hair that simulates at unit scale (gravity looks slightly wrong at extreme scales); non-uniform scale draws sheared hair. Documented, not fixed.

---

## 3. FILE OWNERSHIP

Rule for the implementing agents: new `.cpp/.hpp/.cs` files wherever possible; a file in 3.3 is edited only by the integrator-visible small additive change described there. Parallel agents: A to D (soft bodies) and E to F (hair physics and asset) are independent until G; G needs E; H needs F and G.

### 3.1 Created (only this feature touches them)

**Physics (`modules/physics`)**
- `include/aver/physics/physics_softbody_ext_abi.h`, `include/aver/physics/physics_hair_abi.h`
- `src/SoftBodyInternal.hpp` (the hoisted `SoftRecord` and `SoftExt`), `src/PhysicsSoftBodyExt.cpp`, `src/SoftBodyFluid.cpp`
- `src/PhysicsHair.cpp`, `src/PhysicsHairSettings.cpp`, `src/HairComputeHost.cpp`, `src/HairComputeHost.hpp`, `src/HairInternal.hpp`, `src/HairWindWrapper.cpp`
- `shaders/hair/AverHairWind.hlsl`

**Soft bodies (pure and scene)**
- `modules/softbody/include/aver/softbody/Lattice.hpp`, `Rope.hpp`; `modules/softbody/src/Lattice.cpp`, `Rope.cpp`
- `modules/softbody.scene/include/aver/softbody/SoftShapeSystem.hpp`; `modules/softbody.scene/src/SoftShapeSystem.cpp`, `SoftShapeVolume.cpp`, `SoftShapeRope.cpp`, `SoftShapeProvider.cpp`
- `modules/framework/include/aver/framework/framework_softshape_abi.h`

**Hair**
- `modules/formats/include/aver/formats/OcHair.hpp`, `modules/formats/src/OcHair.cpp`
- `modules/hair/{CMakeLists.txt, module.json, README.md}`; `include/aver/hair/HairGenerate.hpp`, `HairSettingsBuild.hpp`, `HairProxy.hpp`, `HairCurves.hpp`; `src/HairGenerate.cpp`, `HairSettingsBuild.cpp`, `HairProxy.cpp`
- `modules/hair.scene/{CMakeLists.txt, module.json, README.md}`; `include/aver/hair/HairSystem.hpp`; `src/HairSystem.cpp`, `HairProxies.cpp`, `HairProvider.cpp`
- `modules/render.hair/{CMakeLists.txt, module.json, README.md}`; `include/aver/render/HairScene.hpp`, `RhiComputeHost.hpp`; `src/HairScene.cpp`, `RhiComputeHost.cpp`, `HairRegs.inc` (generated, checked in); `shaders/AverHairRibbons.hlsl`, `shaders/AverHairMacros.hlsli`, `shaders/jolt/<Name>.hlsl` x15 and `<Name>.regs.h` x15 (generated)
- `modules/framework/include/aver/framework/framework_hair_abi.h`
- `scripts/gen-hair-shader-regs.py`

**Scripting, graph, editor**
- `scripting/csharp/Aver.Physics/Native.SoftBodyExt.cs`, `SoftBodyExt.cs`, `Native.Hair.cs`, `Hair.cs`
- `scripting/csharp/Aver.Framework/SoftShapes.cs`, `FwSoftShape.cs`, `Hair.cs`, `FwHair.cs`
- `scripting/csharp/Aver.Graph/GraphRopeNodeTable.cs`, `GraphHairNodeTable.cs`; `scripting/csharp/Aver.Graph.Tests/RopeNodeTests.cs`, `HairNodeTests.cs`
- `sandbox/src/GraphNodeDefsRope.hpp`, `GraphNodeDefsHair.hpp`, `SoftShapeDetailsModel.hpp`, `SoftShapeGizmos.cpp`, `HairDetailsModel.hpp`, `HairGroomTab.cpp`, `HairGroomTab.hpp`, `HairDebugOverlay.hpp`
- `docs/nodes/Rope.md`, `docs/nodes/Hair.md`, `docs/HAIR.md`

**Content**
- `test-content/HairLab/{make_hair_lab.py, README.md, Content/...}`

**Tests**
- `tests/physics/src/SoftBodyExtTest.cpp`, `HairTest.cpp`
- `tests/softbody/src/LatticeTest.cpp`, `RopeTest.cpp`
- `tests/softbody.scene/src/SoftShapeSystemTest.cpp`
- `tests/formats/src/OcHairTest.cpp`
- `tests/hair/{CMakeLists.txt, src/HairGenerateTest.cpp, src/HairSettingsBuildTest.cpp, src/HairRegsTest.cpp}`
- `tests/render.hair/{CMakeLists.txt, src/RibbonPackTest.cpp, src/HairSelfTest.cpp}` (the second is headed)
- `tests/framework_softshape/`, `tests/framework_hair/` (relay tests, the `framework_vehicle` pattern)

### 3.2 Modified, owned by this feature

- `modules/physics/src/PhysicsSoftBody.cpp` (record hoist, the four `finish` hooks, face-less relaxation, validate and step hooks)
- `modules/physics/src/SoftBodyAero.hpp` (declares `onStepEnd` and the `sbx*` hook prototypes beside the existing hooks)
- `modules/softbody/include/aver/softbody/SoftAsset.hpp`, `Material.hpp`, `src/SoftAsset.cpp` (new kinds, sections, presets, `valid()`)
- `modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp`, `src/SoftBodySystem.cpp` (`addViewSource`, registration chain), `src/SoftBodyBuild.cpp` (inflate and fast solver), `src/SoftBodyProvider.cpp` (new relay ops for cloth)
- `modules/framework/include/aver/framework/framework_softbody_abi.h` (version 3: pressure, grab and the buoyancy/contact ops are in `framework_softshape_abi.h`; this file only gains `SetPressure`)
- `scripting/csharp/Aver.Physics/SoftBody.cs` (`partial`), `scripting/csharp/Aver.Framework/SoftBodies.cs` (`SoftPreset` gains `Jelly`, `Rope`)
- `scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs`, `scripting/csharp/Aver.Graph.Tests/SoftBodyNodeTests.cs`, `sandbox/src/GraphNodeDefsSoftBody.hpp`, `docs/nodes/SoftBody.md`
- `sandbox/src/SoftBodyDebugOverlay.hpp`, `SoftBodyDetailsModel.hpp`, `SoftBodyPaintTool.cpp` (jelly and rope sections only)
- `docs/SOFTBODY.md` (sections for the new kinds), `test-content/SoftBodyLab/make_soft_lab.py`, `README.md` and its Content

### 3.3 SHARED (another wave 11 area, the integrator or an earlier wave also touches these; every edit here is small and additive)

| File | Edit by this feature |
|---|---|
| `modules/physics/src/PhysicsWorld.cpp` | one call `softbody::onStepEnd(g_world->fixedStep)` after `system.Update` (beside `vehicleStepEnd`, `:489-492`); hair teardown (`hairDestroyAll()`, host reset) in the shutdown path beside `softbody::destroyAll()`; nothing else |
| `modules/physics/src/PhysicsInternal.hpp` | read only; if a hair or sbx TU needs a new accessor it is added here, additively |
| `modules/physics/include/aver/physics/physics_abi.h` | not edited |
| `modules/physics/CMakeLists.txt` | the new `.cpp` files in the `add_library` list |
| `modules/physics.jolt/CMakeLists.txt` | `JPH_USE_CPU_COMPUTE ON` and its comment next to lines 63-65; the DX12/VK lines keep `OFF` with a comment pointing at 2.10. Other areas edit the same block (debug renderer, profiler, double precision) |
| `scripting/csharp/Aver.Physics/Native.cs` | not edited (new `Native.*.cs` parts) |
| `tests/abi/src/AbiEnumTest.cpp` | the two new headers in `kAbis`; the `AVER_PHYS_SBX_*`, `AVER_PHYS_HAIRP_/HAIRM_/HAIRI_/HAIRF_/CB_/BIND_` blocks and `framework_softshape_abi.h`, `framework_hair_abi.h` rows |
| root `CMakeLists.txt`, `cmake/` | `add_subdirectory` for `hair`, `hair.scene`, `render.hair` with the `AVER_MODULE_HAIR*` defines and the `aver_check_module_dag` entries; `scripts/module-guard-audit.py` module list |
| `modules/render.pbr/include/aver/pbr/Material.hpp`, `MaterialGpu.hpp`, `src/MaterialGpu.cpp`, `shaders/material_prelude.hlsl`, `modules/render.voxi/shaders/voxi_rt.hlsli`, `modules/pbr` C ABI `pbr_abi.h` and `scripting/csharp/Aver.Scripting/Pbr.cs` | `shadingModel` and hair fields on `MaterialDesc`; `MaterialFlag_Hair`; the packing and coat guard; `AVER_MODEL_HAIR` and `averHairDirect`; the ray-path mask; additive `aver_pbr_set_shading_model`, `aver_pbr_set_hair`. Material-instance work (UE parity #6) touches the same block |
| `modules/formats/CMakeLists.txt`, `GltfImport.cpp`, `GltfImport.hpp`, `OcMat.hpp`/`MaterialMap.hpp`/`MaterialCook.hpp` | `OcHair` sources; curve primitives; `.ocmat` PARAMs `shadingModel` and `hair*`; `docs/formats/FORMAT_SPECS.md` (rows for `.ochair` and the `.ocsoft` kinds) |
| `tools/AverAssetC.cpp` | the `hair` subcommand |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | `registerGameSystems` gains `SoftShapeSystem::registerComponents` and `HairSystem::registerComponents`; `tickSoftBodies` the two ticks; `resetGameSystems` the two clears; the compute-host install and the wind forward |
| `Runtime/src/GameApp.cpp`, `Runtime/src/GameRender.cpp`, `sandbox/src/SandboxInit.cpp`, `SandboxApp` render registration, `SandboxUpdate.cpp` | register the `HairScene` feature next to the soft-body one, install the compute host, one more link in the draw-handle chain (`GameRender.cpp:666-671` and the editor's `posedHandle`) |
| `sandbox/src/PlacedComponentModel.hpp`, `SandboxComponentDetails.cpp`, `SandboxRender.cpp`, `SandboxShell.cpp` | Add Component rows (`Jelly`, `Rope`, `Hair`), Details sections, the overlay draws, the Show menu entries, the Content Browser type for `.ochair` |
| `Runtime/include/aver/game/LevelComponents.hpp`, `Runtime/src/LevelComponents.cpp`, `modules/formats/include/aver/formats/OcWorld.hpp` | `kPlacedComponentTypes` gains `Jelly Rope Hair` and their apply/capture rows (the cloth precedent, `LevelComponents.cpp:397-420`); runtime damage and pose are never saved |
| `scripts/stage-payload.ps1` | the `shaders/hair` tree and the notice text |
| `docs/ABI.md`, `docs/formats/FORMAT_SPECS.md` | one paragraph per new header; spec rows |
| `modules/rhi/**` | not edited (decision 11 and the register conventions need none); the only RHI-adjacent check is in phase G |
| `modules/softbody.scene/src/SoftBodySystem.cpp` `publish` | the view-source loop (owned, but SOFTBODY_DEPTH-era code; review as a small shared edit) |

---

## 4. Phased implementation

Each phase ends green on its own tests and leaves the tree building. Sizes are hours (S), a day (M) and several days (L), one workflow per phase; all are estimates.

**Phase A, soft ABI core (L).** `physics_softbody_ext_abi.h`, `SoftBodyInternal.hpp`, the `finish` hooks, builder volumes, LRA (explicit and auto), rods and chain, `auto` via `CreateConstraints`, the face-less rule, pressure, skin enable and scale, `set_inv_mass` with rod updates, `slots`, `rod_state`, `Native.SoftBodyExt.cs` and `SoftBodyExt.cs`, `AbiEnumTest` rows, `SoftBodyExtTest` part 1. Exit: the existing `SoftBodyBuilderTest` and `SoftBodyTest` unchanged and green (a body that calls none of the new functions is identical), new cases green.

**Phase B, soft ABI dynamics (M).** `onStepEnd` hook, attachments, contact policy, pair modes, sensors, collision group filter, buoyancy and `submerged`. `SoftBodyExtTest` part 2. Exit: determinism case (two runs, same hash).

**Phase C, scene shapes (L).** `Lattice`, `Rope` pure libraries and tests; `SoftShapeSystem` (jelly, rope, anchors), `addViewSource`, cloth `fast` solver and `inflate`, `.ocsoft` kinds and presets, `framework_softshape_abi.h` provider, C# `SoftShapes`, `SoftBody` v3 and `Rope` graph families with `[AutoRun]` tests, `LevelComponents` rows, host wiring, `SoftBodyLab` content. Exit: headless `SoftShapeSystemTest` (no GPU: a world, physics, ticks) and the node tests.

**Phase D, soft editor and polish (M).** Add Component rows, Details, gizmos, `Show > Soft Constraints`, rope cut, `docs/SOFTBODY.md`, `docs/nodes`. Exit: editor builds and the Details/Add Component tests (`tests/editor`) pass.

**Phase E, hair physics on the CPU host (L).** `physics_hair_abi.h`, `JPH_USE_CPU_COMPUTE`, `HostComputeSystem` and its adapters (exercised by a **test-only fake host** that wraps the CPU backend, to prove the table end to end), `PhysicsHairSettings.cpp`, instances, `update`/`flush`, wind through `AverHairWind` + `HairWindWrapper.cpp`, `read_positions`, `Native.Hair.cs`/`Hair.cs`, `AbiEnumTest` rows, `HairTest`. Exit: sim tests on the CPU host and on the fake host agree to within 1e-4 m.

**Phase F, hair asset and authoring (L).** `OcHair` + round trip tests, `HairGenerate`, `HairSettingsBuild` (asset -> `aver_phys_hair_settings_*`), glTF curve import and `AverAssetC hair`, proxy hull points, `HairGenerateTest`, `OcHairTest`. Exit: a generated groom builds settings on the CPU host with root-to-scalp below 0.5 cm.

**Phase G, the RHI compute host and shaders (L, the riskiest).** `AverHairMacros.hlsli`, 15 wrappers, `gen-hair-shader-regs.py` + `HairRegsTest`, `RhiComputeHost` (buffers, ring, recording, replay, binding set cache, barriers), `HairScene` skeleton with the replay in `prePass`, `HairSelfTest` (headed: the same groom simulated on the CPU host and the GPU host, positions compared, D3D12 then Vulkan). Exit gate: GPU positions within 2 mm of the CPU host after 120 steps on D3D12; the same on Vulkan. Check in this phase: that a compute layout may declare the b4 slot (else the root-constant fallback of 2.10), that `computeInScenePass` is not needed, and the descriptor-write timing against frames in flight.

**Phase H, hair scene and render (L).** `CHair`, `HairSystem` (build, palette, transform, LOD cadence, teleport, proxies, wind), `HairScene` ribbons + LOD parts + draw + shadows, `AVER_MODEL_HAIR` and `MaterialFlag_Hair` in the shared evaluator, handle-chain links in both hosts, `Hair` Add Component and Details, `HairLab` content. D3D12 first. USD `BasisCurves` import as H2 if the owner asks (open question 5). Exit: `RibbonPackTest` (CPU mirror of the ribbon math) green; headed check 1 to 6 of section 6 on D3D12.

**Phase I, hair scripting, graph and editor tools (M).** `framework_hair_abi.h` and provider, C# `Hair`, the `Hair` node family, the Groom tab, overlay, Content Browser entries. Exit: node tests, relay tests.

**Phase J, integration (M).** Ray-traced option (`createBlasUpdatable` + refit), GI-exclusion check and its smallest fix, RT-path material mask, TAA behaviour notes, `--gpu-timing` region, VRAM budget refusal, CPU-fallback cap, `docs/HAIR.md`.

**Phase K, Vulkan parity and packaging (M).** Vulkan headed pass for everything in H and J (the parity rule; the Voxi-on-Vulkan baseline defect in the parity plan item V may limit what can be judged and is recorded as such), `stage-payload.ps1`, notices, `module-matrix.ps1` green.

---

## 5. Tests

**ctest, headless (no GPU, no window).**

`tests/physics` `SoftBodyExtTest` (links Aver.Physics and Aver.Core only, like `VehicleTest`):
- Identity: a cloth and a closed ball built with no `sbx` call match the pre-change positions after 120 steps bit for bit against a recorded hash (the pre-change binary's value is captured once and checked in).
- Pressure: a closed UV sphere (no gravity) settles at a volume ratio greater than 1 for `P > 0`; `set_pressure(0)` returns it to within 3% of rest; an open sheet refuses (`Unsupported`); `sbx_volume` rest equals the analytic sphere volume within 2% (the mesh is a polyhedron).
- Volumes: a jelly cube (6x6x6 nodes, tetrahedra, compliance 0) dropped on a floor keeps volume within 5% while a copy without volumes loses more; both with the same edges.
- LRA: a pinned strip whose free end is pulled by a high velocity stays within `maxDistance` of its pin within 1%; `lra_auto` geodesic equals the staged path length within 1%; `sbx_slots(.., 1, ..)` is a permutation.
- Rods: a 20-node chain pinned at one end hangs with every rod length within 1% of rest, `rod_state` quaternions are unit within 1e-3 and each rod axis points along the chain within 3 degrees; a rod not named in a pair is refused; a face-less body survives a ray cast through it, a sphere cast and a rigid box falling onto it (no assert in the Debug build, which is the point).
- Auto constraints: `builder_auto` with distance bends, an LRA from the pinned row, builds; mixing with staged edges is refused.
- Inverse mass: `set_inv_mass` pins a vertex (it stays within 0.1 cm while the rest swings), restores, and updates the incident rods' `mInvMass` (checked through a rope that no longer swings at a pinned middle node).
- Attachments: `attach` carries a vertex to the target within 1 cm in 1 s at the speed cap; `move_attachment` follows a moving target; `attach_to_body` follows a kinematic box; `detach` restores the mass and the vertex falls; a stale `otherBody` detaches; frame-rate independence: the same script stepped as 1x60 and 2x30 (the fixed loop) gives the same position within 0.5 cm.
- Contact policy: with `softInvMassScale = 0` a falling cloth does not move; with `otherInvMassScale = 0` a rigid box landing on cloth keeps its velocity change below 5% of the default; `IGNORE` passes a box through; `SENSOR` passes through and produces one `entered` and one `exited`; a Jolt sensor body does the same; events are cleared each step.
- Group: a cloth with `set_collision_group(ragdollHandle)` is not pushed by that ragdoll's parts and still collides with a free box.
- Buoyancy: a closed sphere of density 500 kg/m^3 (mass set through inverse masses) floats with the waterline within 5% of the analytic value; density 1500 sinks; an open sheet is pushed by drag only; `submerged` fractions are monotone with depth; the global plane and the sampler both drive it.
- Skin: `set_skin(false, ...)` frees a skinned cloth; scale 0 hard-skins.
- Determinism: two identical runs with every feature on produce identical position hashes.
- `aver_phys_sbx_abi_version() == 1`.

`tests/physics` `HairTest`:
- Host fake: the table wraps `ComputeSystemCPU`-equivalent recording; CPU host and fake host agree within 1e-4 m after 60 steps (proves the adapter).
- A 64-strand groom on a flat scalp with `GlobalPose` off falls under gravity, strand lengths stay within 3% of rest, no non-finite value, roots stay on the scalp.
- `teleport` restores the rest pose and zero velocity; moving `set_transform` makes the tips lag then settle.
- A sphere hull proxy (kinematic, on the proxy layer) stops penetration: no simulated vertex inside the sphere by more than the hair radius after 120 steps; moving the proxy pushes strands.
- Wind: a constant wind moves tips along the wind direction by a monotone amount in the wind speed; zero wind is bit identical to no wind call.
- Settings: `settings_finish` reports the root-to-scalp distance; a strand of 2 points, a material above 255 and a vertex count above 255 are refused; `settings_set_material` changes behaviour live.
- LOD cadence of the scene system is tested in `tests/hair` with a mock ABI.

`tests/softbody` `LatticeTest` (conforming faces, volume of the tetrahedra within 8% of the mesh volume for a sphere and a torus, embedding barycentric sums = 1 within 1e-5, open mesh refused, cap refused), `RopeTest` (`planRope` length and node spacing, `tubeVertices` radius and ring orthogonality, twist follows rod rotation, indices closed manifold, determinism).

`tests/softbody.scene` `SoftShapeSystemTest`: a world with a rope between two entities, one of which moves; node count, `outLengthCm`, the published `PieceView` (generated vertices, uv present); a jelly entity published with an embedded mesh that stays inside the lattice hull; `inflate` closed and open cases; cloth `solver: fast` builds and drapes; cut splits one rope into two entities.

`tests/formats` `OcHairTest` (round trip of `HEAD` and chunks byte-stable, a newer `OCHAIR` number refused, `valid()` refusals, glTF line strip import into strands with the material index, generator determinism by hash), `tests/hair` `HairGenerateTest` (roots on the scalp, spacing, length range, clump pull, mask honoured), `HairSettingsBuildTest` (asset to builder calls against the CPU host), `HairRegsTest` (regenerates the `.regs.h` and `HairRegs.inc` from Jolt's `*Bindings.h` and compares; fails on any binding moved).

`tests/render.hair` `RibbonPackTest`: the C++ mirror of `AverHairRibbons.hlsl` (the `anim::skinVertices` vs `skin.hlsl` discipline): ribbon width never below one pixel, coverage = true width / drawn width, tangent unit, uv ranges, the Jolt-to-engine basis conversion against `Convert.hpp`.

`tests/abi` `AbiEnumTest`: all new constants and the two headers against the C# mirrors.

**C# `[AutoRun]` (bare-process scope, the `VehicleNodeTests` precedent):** `SoftBodyNodeTests` (25 rows, new pins), `RopeNodeTests` (6), `HairNodeTests` (8); row wiring and defaults, not behaviour. A managed `SoftBodyExtTests` drives the low-level natives through the same scenarios as the pressure and rod cases above when the native DLL is present.

**What headless cannot cover** and the section 6 headed checks do: the RHI host on a GPU, both backends, the ribbon pass, the shading model, TAA behaviour, perf.

---

## 6. Headed manual checks for the owner

Run on the owner's PC with the owner's `editor.ini` (`aver-fi-present-stall`), never over the running editor (`aver-build-deploys-into-running-editor`); D3D12 first, then `--backend vulkan`.

1. **Balloon** (`SoftBodyLab`): inflates to a round shape; `SetPressure(0)` deflates over a second; poking it with a rigid box dents and recovers; it floats in the lab pool when its density is below the fluid's.
2. **Jelly**: wobbles for 1 to 2 s, keeps its volume visually, the render mesh stays inside no more than half a cell of the lattice stair-steps (look for poke-through at the corners).
3. **Rope and cable**: hangs from a pole; a rope between two cubes sags and swings; pulling a cube taut straightens it without stretching more than 3%; a cable on the kinematic crane follows; the tube twists its texture when the end rotates; cut splits it.
4. **Grab**: the `grabsoftbody` node drags a cloth corner and releases; no pop on release.
5. **Sensor and groups**: a cloth in a trigger volume fires enter and exit once each; a cape on a ragdolled character does not jitter against its wearer.
6. **Hair look**, `HairLab`: short straight, long curly and ponytail grooms from the front, side and back, in sun and in shade. Judge: no gap between scalp and roots, ribbon width reads as strands not tape, the two highlights (the sharp white R and the coloured shifted TRT) move along the strands as the head turns, no black hair when backlit, tips lighter than roots, no flicker at rest with TAA on. With TAA off the dither is visible (expected, record it).
7. **Hair motion**: head shake, quick turn and a jump: hair lags and settles, no stretching, no jitter; wind from a `Hair` node bends the tips; the ponytail swings; the head proxy stops hair sinking into the skull and shoulders; walking at the speed of a sprint does not detonate it (the velocity caps).
8. **Hair LOD**: walk away: strands thin and fatten without a visible pop beyond a slight density change, simulation stops at the far distance and resumes without a snap; frame time with 4 characters in view.
9. **Shadows and RT**: hair casts shadow-map shadows; with RT shadows on the hair is absent from them unless `kHairRayTraced` is set (check the cost).
10. **Parity**: items 6 to 8 identical in character on Vulkan.
11. **GPU vs CPU host** (`HairSelfTest`, headed): passes on both backends.
12. **Budgets**: `Stats` HUD numbers (soft shapes, hair VRAM) against 2.21; the first `settings_finish` hitch time for the 18k-strand groom.

Tuning table (symptom, number, direction): jelly floppy -> `volumeCompliance` down; rope stretchy -> `stretchCompliance` down and iterations up; rope passes through thin objects -> `segmentCm` down; hair too bouncy -> `linearDamping` up; hair stiff -> `bendCompliance` up or `globalPose` down; hair shoots up on a turn -> `worldInfluence` min up or `maxLinearCmS` down; tape-like ribbons -> `minPixels` down; dark hair in shade -> `scatter`/`occlusionStrength` up; highlights too sharp -> `beta` up; speckle with TAA off -> unavoidable, ship TAA.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Jolt's DX12 and Vulkan backends instead of the RHI host?** Default: RHI host, CPU backend on (2.10). If the owner insists, the work is the five items in the 2.10 table plus a native-handle RHI API on both backends; everything else is unchanged.
2. **Hair when the device has no compute** (`caps().computeShaders == false`)? Default: inert and logged, hair draws nothing. Alternative: card fallback (question 4).
3. **CPU-hosted hair for tiny grooms** (`cpuMaxSimStrands = 64`)? Default: allowed for tests and for a headless server that wants hair state; the renderer never uses it.
4. **Hair cards as a distance fallback** (mesh strips with an alpha texture through Trifactor)? Default: not in this wave; at the far LOD (12% of strands, fat ribbons) the ribbons are the fallback.
5. **USD `BasisCurves` import** (phase H2)? Default: deferred until asked; glTF curves and the generator cover the authoring paths.
6. **Ray-traced hair** (`kHairRayTraced`)? Default: off; the 15 MB ribbon BLAS refit each frame is not free.
7. **Two-way rope and body coupling** (the body feels the rope's pull)? Default: one-way for attachments (the vertex follows); contacts are two-way. A tension impulse from the attachment's required velocity change is the follow-up.
8. **Rope cut** in v1? Default: yes, phase D; if it slips the rest ships.
9. **`cloth.solver: fast`** default for new cloth assets? Default: `cage` (no behaviour change for any existing asset); the templates may opt into `fast`.
10. **Pressure units** kPa at rest volume (not Jolt's `n R T`)? Default: kPa; authors do not think in joules.
11. **Where hair collision proxies get their joints** when the `.ochair` lists none? Default: the skeleton joints named `head`, `neck`, `spine*`, `*shoulder*`, `*clavicle*` (case-insensitive); a rig without them gets none and a log line.
12. **Large-world twins** (`_d` signatures)? Default: none until the LARGE_WORLD area fixes a convention, then `aver_phys_hair_set_transform` and `aver_phys_sbx_attach*` follow it.
13. **Grooms over the VRAM budget**: refuse (default) or auto-thin the render strand fraction at build? Default: refuse with the number in the message.
14. **A second neutral pose / hair grid advection / LOD of the simulation** (the items Jolt lists as missing, `Hair.h:23-33`): not attempted; the distance cadence in 2.15 is the only simulation LOD.
15. **Hair material graph (Aver Node material)** support for the hair model? Default: no; the fixed model with the parameter slots of 2.17.

---

## 8. As built: deviations and notes

(To be written by the implementing agents. Nothing in this document was compiled or run.)
