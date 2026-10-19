# Feature: Gameplay cameras (spring-arm rigs, blended view targets, modifier stack)

Design contract for the implementing agent. Owner approved the Unreal parity port for wave 10. Nothing here is built or run. Format and depth follow `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` and `docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md`.

Rules in force: upgrade the existing camera path (no parallel camera system), no new third-party code, lean comments, never delete files, D3D12 and Vulkan parity (this feature adds no GPU work; both backends already consume the CPU-built view and projection matrices), Aver Node follows the wave 9 C contract, Debug object files near the COFF section cap must not grow (the node catalog stays in `GraphNodeDefs.cpp`'s generated include; new editor code is header-only or in its own `.cpp`), table-1 register bases are untouched because no shader or binding table changes.

`file:line` cites were read in this pass against the tree at `295c4b58`. **UNVERIFIED** marks anything reasoned from reading but not confirmed by reading the exact code path or running it. I did not read Unreal Engine source; behaviours are taken from Unreal's public documentation of property meanings and written clean-room.

## 0. Decisions in one screen

1. **One new component, `CCameraRig`, is the spring arm, the camera mode and the blend source in one.** Runtime-registered (the `CMotionMatcher` pattern, `MotionMatchSystem.hpp:34-50`), 160 bytes, zero-filled storage is a valid Chase rig, flags are negative sense. It is placed through Add Component, edited in Details, saved as `COMPONENT CameraRig`, and reached from C# and Aver Node. `CCamera` (`Components.hpp:140`, today read by nothing) becomes meaningful: it supplies FOV, clip planes and **priority** to the rig that carries it.
2. **Six modes, one evaluator:** `Chase`, `Orbit`, `FirstPerson`, `Seat` (in-car or fixed seat, rides the rig entity's own transform), `TopDown`, `Fixed`. Seven presets (`Chase`, `VehicleChase`, `Orbit`, `FirstPerson`, `Seat`, `TopDown`, `Fixed`) seed the tuning fields; the fields stay editable.
3. **Spring-arm parity with Unreal's `SpringArmComponent`:** target arm length, socket offset, target offset, swept-sphere probe collision, position lag and rotation lag each with a max distance, lag sub-stepping, inherit pitch/yaw/roll. Differences, stated: lag is the exact exponential `1 - exp(-speed*dt)` (frame-rate independent) instead of Unreal's linear interpolation; the probe includes the socket offset; the arm recovers outward with a half-life and snaps inward.
4. **A new physics query, `aver_phys_sphere_cast_q`**, because none of the existing sphere casts fits: `aver_phys_sphere_cast_ex` has no distance output and does not skip sensors (`PhysicsLayers.cpp:204-232`), so a trigger volume would stop the camera. The new cast has the `physics_queries_abi.h` filter set (sensors skipped by default, two ignore bodies, distance out). It is the only physics change.
5. **View targets blend by snapshot.** Switching rigs, switching a rig's mode, leaving a sequencer camera, or returning to the legacy boom freezes the last output pose and blends it into the live pose of the new source over `blendTimeS` with a chosen curve. One mechanism covers every transition. (Unreal keeps the outgoing target live; the snapshot is simpler, never reads a destroyed entity, and is what `lockOutgoing` does there.)
6. **Which rig is live:** an explicit `CameraRig.SetViewTarget(entity, ...)` wins; otherwise the best rig under the possessed pawn (the pawn, a descendant, or a rig whose `target` is the pawn) by `CCamera.priority`; otherwise nothing and the existing `drivePlayCamera` boom runs **unchanged and bit-identical**. A project with no rig renders exactly as before.
7. **A camera modifier stack** (Unreal `CameraModifier`): FOV kick, FOV scale (zoom), punch (recoil), roll lean, arm scale, socket offset, each with priority, blend in, hold, blend out, tag, optional exclusivity. **Shake is not a second shake system:** a shake modifier and `CameraRig.ShakeAt` add trauma to wave 9's `CameraFx` (`CameraFx.hpp:14-74`), which the host already applies last with the player's effective scales.
8. **Reduce motion still gates everything that moves the camera on its own.** A new `motionAllowed` input (read from `access.motion.reduce`) switches off look-ahead, FOV kick, punch, lean, bob and auto-recenter, and floors the position lag speed at 40 (a stiff follow, not a sway). Shake and bob keep going through `CameraFx`, whose own gate is unchanged (`AccessAbi.cpp:464-473`).
9. **FOV becomes a real input.** Today `pushCamera` hard-codes 60 degrees and 2/200000 cm (`GameCamera.hpp:39-40`, `:57`), and four more places hard-code the same 60 for LOD (`SandboxRender.cpp:396`, `:1068`, `:1215`, `:1349`, `GameLandscape.cpp:386`). A small `PlayView { fov, roll, near, far, ownerHideAll, cut }` travels with the pose; every site reads it. The renderer already derives its frustum from the matrices (`CameraFactor.hpp:26-44`), so nothing in a shader changes.
10. **Collision fade is a binary hide with hysteresis**, not a dithered alpha (the engine has no per-instance opacity: no dither path exists under `modules/rhi/shaders`). When the arm is squashed inside `fadeEndCm` of the pivot the target's meshes stop drawing in the raster pass (shadows, GI and RT geometry stay, the `ownerHideRoot` route in `GameRender.cpp:728-740`). One new bool on `DrawWorldOptions`.
11. **One module, `Aver.Camera` (`modules/camera`)**, split like `Aver.Anim` / `Aver.Anim.Scene`: header-only pure math and modifiers (Core only, testable anywhere), plus a system that needs the scene and the Jolt query. The relay ABI is `framework_camera_abi.h` (`aver_fw_camera_*`, one provider function, own `AVER_FW_CAMERA_ABI_VERSION`), the `framework_softbody_abi.h` pattern. Component add/remove and field writes use the generic scene ABI.
12. **Aver Node family `Camera`: 23 nodes** (20 exec, 3 pure), table `GraphCameraNodeTable.cs`, palette `GraphNodeDefsCamera.hpp`, owner `CameraGraph`, tests `CameraNodeTests` (`[AutoRun]`). Events `CameraViewTargetChanged`, `CameraBlendFinished`, `CameraCut` reach graphs through the existing notify seam.
13. **Cross-feature contracts, exact signatures in section 2.12.** VEHICLES attaches and points a rig through `Entity.AddCameraRig(CameraRigPreset.VehicleChase, target)`, `SetCameraRigMode`, `SetCameraRigTarget`, `SetCameraRigIgnoreBody` and `CameraRig.SetViewTarget`; it writes no camera math. `CameraRig.ShakeAt` is offered to SOFTBODY_DEPTH and VEHICLES for impacts; neither is required to call it.
14. **Not in v1, said plainly:** orthographic or isometric projection (`CameraFactor::factor` refuses an orthographic matrix; use `TopDown` with a narrow FOV), split-screen (one local player, as `aver_fw_view` already assumes), per-camera post-process blend, a camera-cut history reset in the temporal passes (the sequencer has none either), pattern-asset shakes (`.ocshake`), dithered collision fade, mesh-less rig placements in level files. Each is an open question in section 7.
15. **Size L, eight phases** (section 4). Phase A is pure math, phases C and D carry the risk.

---

## 1. What exists

Every row was read in the tree. Line numbers are last-known-good pointers; grep the symbol when one is off.

### 1.1 The camera today

| Fact | Where |
|---|---|
| The play camera is `game::drivePlayCamera(pos, yaw, pitch, standInPawn)`: a fixed boom. Pivot = the published view node (else pawn + eye height), position = pivot - forward * boom, look direction = the pivot's forward. **No collision, no lag, no blend, no roll, no FOV.** Returns the pawn to hide in first person | `Runtime/include/aver/game/GameCamera.hpp:102-168` |
| Mode, eye height and boom come from one process-wide request a character publishes: `aver_fw_set_view(mode, eye, boom)`, `aver_fw_view`, `aver_fw_set_view_entity`, `aver_fw_view_entity`. `AVER_FW_VIEW_FIRST_PERSON 0`, `AVER_FW_VIEW_THIRD_PERSON 1`. "One request (one local player) until split-screen exists" | `framework_abi.h:599-612`, `FrameworkAbi.cpp:1743-1767` |
| `AverCharacter` publishes it every drive: `CameraViewMode` (default third person), `EyeHeight` 160, `BoomLength` 450; `EnsureView` makes a child "View" entity at eye height carrying the pitch and publishes it | `Character.cs:59-65`, `:216`, `:388-404` |
| `AverPawn`, `AverPlayerController.Possess`, `Possessed` | `Pawn.cs:9-25`, `PlayerController.cs:9-28` |
| Editor Play: `SandboxApp::drivePlayCamera()` forwards to the shared function (the drone stands in for the pawn); the frame calls it, then `applyCameraFx` when a session is active, then a sequencer pilot override | `SandboxPlay.cpp:939-944`, `SandboxUpdate.cpp:942-967` |
| Packaged game: `GameApp::drivePlayCamera()`, then the sequencer's camera takes the view, then `applyCameraFx` unconditionally | `GameApp.cpp:600-604`, `:2244-2260` |
| The view is built by `pushCamera(device, pos, forward, aspect)`: `lookAtDirLH(pos, fwd, {0,0,1})`, `perspectiveLH(radians(60), aspect, 2, 200000)`, `device.setCamera` with the camera-relative inverse. Called from both hosts | `GameCamera.hpp:50-75`, `GameApp.cpp:1694-1697`, `SandboxUpdate.cpp:1099-1102` |
| FOV 60 is also hard-coded for LOD and terrain selection | `SandboxRender.cpp:396`, `:1068`, `:1215`, `:1349` (`trifactor::View::verticalFovRadians`), `GameLandscape.cpp:386` (`projScale`) |
| The renderer reads the matrices, not an FOV: `CameraFactor` recovers the separate view and projection from `viewProj` for any perspective; path-tracer view setup derives `tanV` from the matrix | `CameraFactor.hpp:26-44`, `PtSceneView.cpp:338` |
| `CCamera { fovYRad 1.0472, nearCm 5, farCm 500000, priority }` is registered and settable (`Builtins.cpp:93-98`, `EntityRenderProps.cs:83-100`, graph nodes `an_setcamerafov/clip/priority`) but **nothing reads it**: the Graph Editor's own blurb says "no renderer reads CCamera yet" | `Components.hpp:140-145`, `GraphEditorComponents.cpp:86`, `GraphComponentTree.cs:153-162` |
| First-person owner hide: `kMeshRendererHiddenFromOwner` meshes under `DrawWorldOptions::ownerHideRoot` skip the raster draw but still reach shadows, GI and RT | `Components.hpp:75-107`, `GameRender.cpp:728-740`, `GameRender.hpp:333-335`, hosts `SandboxRender.cpp:429`, `:1568`, `GameApp.cpp:2339` |
| The sequencer owns a camera pose of position + yaw + pitch and its own pairwise blend (`blendCamera`, smoothstep); no roll, no FOV | `LevelSequence.hpp:34-37`, `SequenceTracks.cpp:113-119`, `LevelSequence.cpp:423-440` |
| No camera-cut or history-reset signal exists for the temporal passes; streaming has `resetVelocityTracking()` for a teleport | tree grep (`cameraCut`, `resetHistory`: no hits under `Runtime`, `sandbox/src`, `modules/rhi/include`); `GameStreaming.hpp:98-100` |

### 1.2 Camera feel and accessibility (wave 9)

| Fact | Where |
|---|---|
| `fw::CameraFx`: trauma shake (decay 1.5/s, 1.5 deg yaw, 1.2 deg pitch, 1.5 cm at full trauma), speed-driven head bob; `apply(pos, yaw, pitch, dt, shakeScale, bobScale)` returns false and touches nothing when both scales are 0. Deterministic, no allocation | `CameraFx.hpp:14-74` |
| One global instance behind the C ABI: `aver_fw_camera_shake_add(trauma)`, `aver_fw_camera_bob_set(amount, speed)`, `aver_fw_camera_fx_apply(dt, pos3, yawPitch2)` which reads the effective scales | `AccessAbi.cpp:459-473`, `framework_access_abi.h:79-85` |
| Reduce motion is a gate: `shake`, `bob`, `blur`, `lens` are 0 while `access.motion.reduce` is on; `aver_fw_access_effective(out8)` and `aver_fw_access_get(key)` expose it | `AccessKeys.hpp:225-243`, `AccessAbi.cpp:329-334`, `framework_access_abi.h:29` |
| C# `CameraFx.AddShake/SetBob` and the graph nodes `addcamerashake`, `setcamerabob` | `Accessibility.cs:283-301`, `:370-380`, `GraphAccessibilityNodeTable.cs:25-26` |

### 1.3 Physics queries

| Fact | Where |
|---|---|
| `aver_phys_sphere_cast` and `_ex` report the first hit as body + point + normal, **no distance**; the `_ex` filter is layer mask + one ignore body and **does not skip sensors** | `physics_abi.h:580-585`, `physics_layers_abi.h:97-102`, `PhysicsLayers.cpp:204-232` |
| `aver_phys_box_cast` and `aver_phys_capsule_cast` have distance, two filters and skip sensors unless `AVER_PHYS_QUERY_INCLUDE_SENSORS`; a capsule needs `height > 2*radius`, so it cannot be a sphere | `physics_queries_abi.h:53-76`, `PhysicsQueries.cpp:101-131`, `:176-192` |
| `QueryBodyFilter` skips two body ids and sensors; `ignoredIdOf` accepts a rigid-body handle **or a character handle** (resolving it to the character's inner body) | `PhysicsQueryFilters.hpp:41-59` |
| `aver_phys_character_of_entity(entity)` finds a character's handle; there is **no** reverse lookup for rigid bodies (`aver_phys_set_entity` only stamps). `BodyRegistry` is a host-side map nobody shares | `physics_character_abi.h:163`, `physics_abi.h:298-306`, `BodyRegistry.hpp:1-30` |
| Cloth and character proxies live on user layers 14 and 15 by default (wave 9); a camera probe must not stop on them | `SoftBodySystem.hpp:78-95` |
| Shape casts use `JPH::SphereShape` stack-allocated with `SetEmbedded()` in the existing code; a shape that starts overlapping reports distance 0 | `PhysicsLayers.cpp:213-214`, `physics_queries_abi.h:27-29` |

### 1.4 Components, level records, editor

| Fact | Where |
|---|---|
| A runtime-registered component: `registerComponent<T>(name).field(...)` with `verify(sizeof)`, zero-filled storage, `attach` writes defaults, no C ABI, iterate with `world.pool(type)` | `MotionMatchSystem.hpp:34-50`, `MotionMatchSystem.cpp:46-92`, `:292-307` |
| Provider-relay ABI: the host installs one function, `aver_fw_*` forwards, no provider returns 0 | `framework_softbody_abi.h:1-60`, `framework_abi.h:57` |
| `COMPONENT <Type> key=value` records: the type list lives in `OcWorld.cpp:173-174` and `LevelComponents.hpp:28-29`; appliers and capture in `LevelComponents.cpp:151-160` (type ids), `:186-192` (attach rules), `:269-289` (an applier), `:472-488` (dispatch), `:551`, `:615` (capture). Canonical writing omits default keys | `FORMAT_SPECS.md:936-1008` |
| Add Component: `kPlacedKinds[]`, `canAttach` (**every kind requires a mesh placement**, line 117), `makeNewRecord`; Details sections per kind | `PlacedComponentModel.hpp:40-71`, `:117`, `:131-137`, `:197-206`, `SandboxComponentDetails.cpp:786` |
| A class-based pawn has no entity to attach a `COMPONENT` record to (class placements are skipped) | `FORMAT_SPECS.md:964-966` |
| A graph class assembles components from `COMP` kinds; `camera` and `light` are handled, unknown kinds are a bare transform with a warning | `GraphComponentTree.cs:153-171`, `GraphEditorComponents.cpp:73-87`, `:124-130` |
| C# class rows add components and defaults: `ClassBuilder.Camera`, `aver_fw_class_set_default_i32/f32` | `ClassBuilder.cs:37-44`, `framework_abi.h:129-133` |
| Host wiring is one shared header: `registerGameSystems`, `tickSoftBodies`, `resetGameSystems`; notify sinks are installed beside the animation sink in both hosts | `GameSystemsWiring.hpp:184`, `:248`, `:263`, `SandboxInit.cpp:330`, `:731-740`, `GameApp.cpp:1929-1931`, `:1341-1352`, `SandboxPlay.cpp:666` |
| The Aver Node wave 9 contract is implemented: `GraphRenderPropsNodeTable.cs` (camera FOV, clip, priority nodes exist, keys `an_setcamerafov` ...), `GraphNodeDefsRenderProps.hpp`, `RenderPropsNodeTests.cs` with `[AutoRun]` | `GraphRenderPropsNodeTable.cs:22-25`, `RenderPropsNodeTests.cs:1-60` |
| Module layout precedent for a scene + physics system and its test | `modules/softbody.scene/{module.json,CMakeLists.txt}`, root `CMakeLists.txt:305-313`, `:572`, `tests/softbody.scene/CMakeLists.txt` |
| ABI header/binding parity is checked by name, including parameter names | `tests/abi/src/AbiEnumTest.cpp:549-575` |

### 1.5 Gaps this feature closes

No collision, lag, blend, roll, FOV, look-ahead or modifier exists on the play camera; `CCamera` is write-only; a trigger volume would block a naive sphere cast; a rigid target cannot be ignored by entity; the 60-degree FOV is a constant in five places; there is no way to hide a third-person body when the camera is inside it.

---

## 2. Design

### 2.1 Architecture

```
 CCameraRig (per entity) + CCamera (fov, near, far, priority)        hosts (editor Play, packaged game)
        |                                                                  |
 Aver.Camera (modules/camera)                                              |  drivePlayCamera(pos,yaw,pitch,PlayView,dt)
   CameraRigMath.hpp      pure: lag, angles, blend, probe resolve, look-ahead  |    fallback = legacy boom pose (computed first, unchanged)
   CameraModifiers.hpp    pure: envelopes, stack evaluation                    |    frame = {dt, possessed, viewNode, motionAllowed, external}
   CameraRigSystem        scene + Jolt: select -> evaluate -> probe -> blend   |--> cameraRigSystem().drive(world, frame) -> RigResult
        |  aver_phys_sphere_cast_q (new)         |  notify sink (events)        |    pos/yaw/pitch/PlayView/ownerHideRoot written back
 Aver.Physics                              framework_camera_abi.h (relay) --- C# CameraRig / Entity members / Aver Node "Camera"
```

Frame order, both hosts (unchanged apart from the arguments): physics step -> `World::flush` -> `drivePlayCamera` (rig or legacy) -> sequencer camera override -> `applyCameraFx` (global shake and bob) -> `pushCamera(…, PlayView)`.

Units and axes are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed. Camera yaw/pitch are the `cameraForward` convention (`GameCamera.hpp:33-35`: positive pitch looks up), in radians inside C++ and degrees at every authored or scripted boundary.

### 2.2 Types, `modules/camera/include/aver/camera/CameraTypes.hpp` (Core only)

```cpp
namespace aver::camera {
enum class CameraMode : u32 { Chase = 0, Orbit = 1, FirstPerson = 2, Seat = 3, TopDown = 4, Fixed = 5 };
enum class CameraPreset : u32 { Chase = 0, VehicleChase = 1, Orbit = 2, FirstPerson = 3, Seat = 4, TopDown = 5, Fixed = 6 };
enum class BlendCurve : u32 { EaseInOut = 0, Linear = 1, EaseIn = 2, EaseOut = 3 };   // 0 = the zero-filled default; EaseInOut is smoothstep, the sequencer's smooth01
enum class ModKind : u32 { FovKick = 0, FovScale = 1, Punch = 2, RollLean = 3, ArmScale = 4, SocketOffset = 5 };

inline constexpr u32 kRigNoCollision = 1u << 0;      // negative sense throughout, so zero-filled storage is a working Chase rig
inline constexpr u32 kRigNoInheritYaw = 1u << 1;     // yaw comes from the rig's own yawDeg instead of the source rotation
inline constexpr u32 kRigNoInheritPitch = 1u << 2;
inline constexpr u32 kRigNoInheritRoll = 1u << 3;
inline constexpr u32 kRigNoFade = 1u << 4;
inline constexpr u32 kRigLookAtTarget = 1u << 5;     // Fixed: face the target, smoothed by rotLagSpeed
inline constexpr u32 kRigDisabled = 1u << 6;         // never selected
inline constexpr u32 kRigApplyPreset = 1u << 30;     // one-shot, script/class sets, the system applies `preset` over the tuning fields and clears it
inline constexpr u32 kRigCut = 1u << 31;             // one-shot, script sets, the system snaps the lag state next tick and clears it

inline constexpr u32 kRigStateActive = 1u << 0, kRigStateBlending = 1u << 1, kRigStateColliding = 1u << 2,
                     kRigStateFaded = 1u << 3, kRigStateLagging = 1u << 4;     // CCameraRig::outState

struct CameraPov { Vec3 pos; f32 yaw = 0, pitch = 0, roll = 0; f32 fovYRad = 1.0471976f; f32 nearCm = 2, farCm = 200000; };

struct ViewBlend { f32 seconds = 0.5f; BlendCurve curve = BlendCurve::EaseInOut; bool cut = false; };
}
```

`BlendCurve` values, `CameraMode`, `CameraPreset`, `ModKind` and the flag bits are restated as `#define AVER_FW_CAM_*` in `framework_camera_abi.h` and as C# enums; `AbiEnumTest` pins all three copies (section 5).

### 2.3 The component, `CCameraRig` (registered by `CameraRigSystem::registerComponents`)

160 bytes, 40 four-byte fields, `static_assert(sizeof(CCameraRig) == 160)` plus `verify(sizeof)`. **A value of 0 in a numeric tuning field reads as the default shown**, as `playRate 0 reads 1` does in `CMotionMatcher`; "off" is stated where 0 means off. Angles in degrees, lengths in cm, times in seconds.

| Off | Field | Type | Meaning (0 reads / zero-filled) | Unreal counterpart |
|---|---|---|---|---|
| 0 | `flags` | u32 | `kRig*` bits above | `bDoCollisionTest`, `bInherit*` (negated) |
| 4 | `mode` | u32 | `CameraMode`; 0 = Chase. Changing it at run time blends over `blendTimeS` | camera mode in Gameplay Cameras |
| 8 | `preset` | u32 | informational base for the editor and the level record; `kRigApplyPreset` applies it | rig asset |
| 12 | `target` | u32 | entity id the arm follows. 0 = the rig's parent, else the rig's own entity | the spring arm's attach parent |
| 16 | `ignoreBody` | i32 | an extra physics body handle the probe skips (a vehicle chassis). Process-local, **never saved** (`BodyRegistry.hpp:1-30` rule). The target's character is skipped automatically | `AddIgnoredActor` |
| 20 | `armLengthCm` | f32 | 0 reads 450 | `TargetArmLength` |
| 24 | `minArmCm` | f32 | 0 reads 20: the closest the camera may come to the pivot | (none) |
| 28 | `probeRadiusCm` | f32 | 0 reads 12; must exceed the near-plane corner radius, about `near * tan(fov/2) * sqrt(aspect^2 + 1)` = 2.3 cm at the defaults | `ProbeSize` |
| 32 | `collisionMask` | u32 | physics layer bitmask; 0 reads "all layers except the cloth and proxy layers" (`Options::defaultProbeMask`) | `ProbeChannel` |
| 36 | `recoverHalfLifeS` | f32 | 0 reads 0.25: how fast the arm returns outward after a hit. Inward is instant | (none) |
| 40,44,48 | `socketX/Y/Z` | f32 | offset at the arm's end in the **camera frame** (forward, right, up) | `SocketOffset` |
| 52,56,60 | `pivotX/Y/Z` | f32 | offset of the arm origin in **world** axes (eye height lives here) | `TargetOffset` |
| 64 | `posLagSpeed` | f32 | 0 = no position lag; else 1/s | `CameraLagSpeed` |
| 68 | `lagMaxDistCm` | f32 | 0 = unlimited | `CameraLagMaxDistance` |
| 72 | `rotLagSpeed` | f32 | 0 = no rotation lag | `CameraRotationLagSpeed` |
| 76 | `rotLagMaxDeg` | f32 | 0 = unlimited | `CameraRotationLagMaxDistance`-like |
| 80 | `lagMaxStepS` | f32 | 0 reads 1/60: lag is integrated in steps no longer than this | `CameraLagMaxTimeStep` |
| 84 | `lookAheadS` | f32 | 0 = off. Pivot leads by planar velocity times this | (Gameplay Cameras) |
| 88 | `lookAheadMaxCm` | f32 | 0 reads 150 | |
| 92,96 | `pitchMinDeg/MaxDeg` | f32 | both 0 reads -80 / +80 | `ViewPitchMin/Max` |
| 100,104 | `yawDeg`, `pitchDeg` | f32 | the rig's own rotation: Orbit/TopDown/Fixed yaw and pitch, a no-inherit axis, and the head-look offset in Seat | arm relative rotation |
| 108 | `recenterDelayS` | f32 | 0 = off. Orbit/Chase with own yaw eases behind the target after this long without look input | |
| 112 | `blendTimeS` | f32 | seconds to blend **into** this rig/mode; 0 = cut | `BlendTime` |
| 116 | `blendCurve` | u32 | `BlendCurve` | `BlendFunction` |
| 120 | `fovKickDeg` | f32 | 0 = off. Added FOV at `fovKickSpeedCmS` | |
| 124 | `fovKickSpeedCmS` | f32 | 0 reads 1500 | |
| 128,132 | `fadeStartCm`, `fadeEndCm` | f32 | both 0 reads 140 / 70 unless `kRigNoFade`. Camera nearer than `fadeEndCm` to the pivot hides the target's meshes; returns at `fadeEndCm * 1.15` | |
| 136 | `bobAmount` | f32 | 0 = off. FirstPerson/Seat head bob through `CameraFx.setBob` | |
| 140,144 | `seatYawLimitDeg`, `seatPitchLimitDeg` | f32 | 0 reads 110 / 50: head-look range in Seat | |
| 148 | `outArmCm` | f32 | OUT, read-only: arm length after collision and recovery | |
| 152 | `outState` | u32 | OUT: `kRigState*` | |
| 156 | `outFovDeg` | f32 | OUT: final FOV of this rig's last evaluation | |

OUT fields are registered read-only (the `CSoftBody::body` precedent, `Builtins.cpp:134-143`). `CameraRigSystem::attach` writes defaults over the zero-filled bytes, adds `CCamera` when absent with `nearCm 2`, `farCm 200000`, `fovYRad` 60 degrees (matching today's constants, not `CCamera`'s own 5 / 500000), and returns the pointer.

#### 2.3.1 Mode semantics

| Mode | Pivot | Rotation | Arm and probe | Lag | Typical use |
|---|---|---|---|---|---|
| Chase | the target's view node if the target is the possessed pawn and has one (today's head pivot), else target origin; plus `pivot*` | the source rotation: the view node's forward for a pawn, else the target's heading (yaw only unless pitch is inherited). A `kRigNoInherit*` axis takes `yawDeg/pitchDeg` | yes | position, rotation, look-ahead | third-person character, vehicle chase |
| Orbit | target origin + `pivot*` | the rig's own `yawDeg/pitchDeg`, driven by look input (`AddLook`) | yes | position | inspection, strategy, character orbit |
| FirstPerson | the view node (else target + `pivot*`), `socket*` in camera frame | the view node's forward | none, no probe | **forced off** (a lagging eye is nauseating) | first person; hides flagged meshes through the legacy owner-hide |
| Seat | the **rig entity's own world transform** (a child of the vehicle at the seat) | that transform's rotation + head-look offset (`yawDeg/pitchDeg`, clamped to the seat limits) | none | position lag allowed (default small, damps vibration) | in-car, cockpit, fixed seat |
| TopDown | target origin + `pivot*` + look-ahead | fixed `yawDeg/pitchDeg` (default -62 pitch), no inherit | yes, off by default (`kRigNoCollision`) | position | top-down, ARPG |
| Fixed | the rig entity's world transform | that rotation, or a smoothed look at the target when `kRigLookAtTarget` | none | rotation (when looking at) | security camera, cinematic fixed shot |

#### 2.3.2 Presets, `CameraRigPresets.hpp` (`CCameraRig presetRig(CameraPreset)`)

Starting values, tuned headed (section 6). All unspecified fields are 0 (the defaults above).

| Preset | mode | arm | probe | pivot | socket (fwd,right,up) | posLag / max | rotLag | look-ahead s / cm | pitch limits | blend s | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Chase | Chase | 450 | 12 | 0 | 0 | off | off | off | -80 / 80 | 0.4 | fade 140/70. Reproduces today's boom plus collision |
| VehicleChase | Chase | 600 | 14 | (0,0,120) | (0,0,60) | 8 / 150 | 5 | 0.25 / 250 | own pitch -12 (`kRigNoInheritPitch`, `pitchDeg -12`) | 0.8 | fovKick 10 deg at 3000 cm/s, `kRigNoFade` |
| Orbit | Orbit | 500 | 12 | 0 | 0 | 12 / 0 | off | off | -75 / 75 | 0.5 | `pitchDeg -20`, fade 140/70 |
| FirstPerson | FirstPerson | 0 | none | 0 | 0 | forced off | off | off | -85 / 85 | 0.25 | `bobAmount 0.5` |
| Seat | Seat | 0 | none | 0 | 0 | 25 / 20 | off | off | | 0.6 | head limits 110/50, fovKick 6 deg at 3000 cm/s |
| TopDown | TopDown | 1400 | none | 0 | 0 | 6 / 0 | off | 0.4 / 250 | | 0.7 | `pitchDeg -62`, all inherit off, `kRigNoCollision` |
| Fixed | Fixed | 0 | none | 0 | 0 | off | off | off | | 0.5 | |

The editor, the level record, the C# `CameraRigPreset` and `AddCameraRig(preset)` all go through this one table. `kRigApplyPreset` lets a C# class row (`ClassBuilder.CameraRig(preset)`) or a graph `COMP` write only `preset` and the flag; the system expands it on the first tick, so no preset values are duplicated in C#.

### 2.4 Pure math, `CameraRigMath.hpp` (header-only, `Aver.Core` only)

The contract the implementer fills in; each function is a pure function of its arguments, no clock, no allocation.

```cpp
namespace aver::camera {
// Exact exponential follow: cur + (target - cur) * (1 - exp(-speed * dt)). speed <= 0 returns target.
f32  expFollow(f32 cur, f32 target, f32 speed, f32 dt);
// UE-style lag with sub-stepping: the TARGET is interpolated from prevTarget to target across ceil(dt / maxStepS)
// (<= 8) steps, each step a expFollow of the vector; then |target - result| is clamped to maxDist (0 = unlimited).
Vec3 lagFollow(Vec3 cur, Vec3 prevTarget, Vec3 target, f32 speed, f32 maxStepS, f32 maxDist, f32 dt);
// Shortest-path angle follow in radians (wraps at pi), optional max angular distance (0 = unlimited).
f32  angleFollow(f32 cur, f32 target, f32 speed, f32 dt, f32 maxDist);

struct Basis { Vec3 fwd, right, up; };
Basis basisFromYawPitchRoll(f32 yaw, f32 pitch, f32 roll);       // right = normalize(cross(worldUp, fwd)); roll turns right/up about fwd
void  yawPitchRollFromBasis(const Basis&, f32& yaw, f32& pitch, f32& roll);
Quat  quatFromBasis(const Basis&);                               // for blends only
CameraPov blendPov(const CameraPov& a, const CameraPov& b, f32 alpha);   // lerp pos/fov/clip, slerp rotation (shortest path), alpha clamped
f32   blendAlpha(f32 t01, BlendCurve curve);                     // EaseInOut = t*t*(3-2t); endpoints exact, monotone

// Look-ahead: planar velocity estimate (half-life 0.1 s) and the clamped, smoothed lead. A displacement above
// teleportCm in one call resets the estimate and returns zero lead (and sets `teleported`).
struct LeadState { Vec3 lastPivot; Vec3 vel; Vec3 lead; bool valid = false; };
Vec3 lookAhead(LeadState&, Vec3 pivot, f32 aheadS, f32 maxCm, f32 dt, f32 teleportCm, bool& teleported);

// Arm resolve: hitDist = distance the probe travelled (or < 0 for a clear sweep); armWanted = unobstructed length.
// Inward is instant, outward recovers with the half-life; the result is never below minArm and never above armWanted.
f32  resolveArm(f32 armWanted, f32 hitDist, f32 prevArm, f32 minArm, f32 recoverHalfLifeS, f32 dt);

// Collision fade with hysteresis: fades below endCm, returns above endCm * 1.15.
bool fadeHide(bool wasHidden, f32 camToPivotCm, f32 startCm, f32 endCm);

// FOV: base + kick * clamp01(speed / kickSpeed) (smoothed by the caller), then the modifier stack, clamped 10..140 deg.
f32  composeFov(f32 baseRad, f32 kickRad, f32 modAddRad, f32 modScale);
}
```

### 2.5 Modifier stack, `CameraModifiers.hpp` (header-only, pure)

```cpp
struct CameraModifier { u32 id; ModKind kind; i32 priority; f32 inS, holdS, outS;   // holdS < 0 = until removed
                        f32 a, b, c, d; u32 tag; u32 flags; };   // flags: kModExclusive, kModMotion, kModNoMotionGate
struct ModResult { f32 fovAddRad = 0, fovScale = 1, armScale = 1; Vec3 socketAdd; f32 punchPitch = 0, punchYaw = 0, roll = 0; };
class ModifierStack {
 public:
  u32  push(const CameraModifier&);                       // returns a never-reused id (counter advanced in call order)
  bool remove(u32 id, f32 outS);                          // outS < 0 = the modifier's own; blends out from the CURRENT alpha
  u32  clear(u32 tag);                                    // tag 0 = all; each blends out over its own outS; returns the count
  ModResult evaluate(f32 dt, bool motionAllowed);         // advances every envelope, drops finished ones
  u32  count() const;
};
```

| Kind | Parameters | Effect | Motion-gated by default |
|---|---|---|---|
| FovKick | `a` = degrees | `fovAdd += a * alpha` | yes |
| FovScale | `a` = multiplier (0.6 zooms in) | `fovScale *= lerp(1, a, alpha)` | no (aim-down-sights is the player's own action) |
| Punch | `a` pitch, `b` yaw, `c` roll, degrees | camera rotation offset, fast in, long out | yes |
| RollLean | `a` = roll degrees | `roll += a * alpha` | yes |
| ArmScale | `a` = factor | `armScale *= lerp(1, a, alpha)` (zoom out for speed) | no |
| SocketOffset | `a,b,c` = forward,right,up cm | `socketAdd += v * alpha` (over-the-shoulder aim) | no |

Envelope: in = smoothstep over `inS`, hold for `holdS`, out = smoothstep over `outS` from the alpha at the time the out phase began. `kModExclusive`: while an exclusive modifier of a kind is at alpha >= 0.99, lower-priority modifiers of the same kind contribute nothing. Evaluation order: ascending priority, then id. When `motionAllowed` is false a motion-gated modifier contributes 0 but its envelope still advances (it ends on schedule).

**Shake.** `CameraRig.AddShake(trauma)` is the existing `CameraFx.AddShake`. `CameraRig.ShakeAt(pos, radiusCm, intensity, falloffPower)` computes `trauma = intensity * (1 - clamp01(d / radius))^falloffPower` against the **current camera position** and forwards it to `aver_fw_camera_shake_add` through the shake sink the host installs (the system does not link the framework). The accessibility scales therefore apply exactly once, in `CameraFx::apply`.

### 2.6 The system, `CameraRigSystem` (scene + Jolt; `modules/camera`)

```cpp
namespace aver::camera {
struct CCameraRig { ... };                                    // section 2.3
struct Options { f32 teleportCm = 1000, externalBlendS = 0.5f; u32 defaultProbeMask = 0xFFFFFFFFu; f32 reduceMotionLagFloor = 40; };

struct RigFrame {
  f32 dt = 0;                           // the host's frame dt, the value applyCameraFx gets (not the fixed physics step)
  scene::Entity possessed;              // player 0's pawn, kInvalidEntity when not playing or none
  scene::Entity viewNode;               // aver_fw_view_entity() if valid
  bool motionAllowed = true;            // false while access.motion.reduce is on
  CameraPov fallback;                   // the legacy boom pose, computed by the host first
  bool externalActive = false; CameraPov external;   // the sequencer camera, if it holds the view this frame
};
struct RigResult { bool active = false; CameraPov pov; scene::Entity ownerHideRoot; bool hideAll = false; bool cut = false; };

class CameraRigSystem {
 public:
  static u32 registerComponents(scene::World&);
  static CCameraRig* attach(scene::World&, scene::Entity rig, CameraPreset, scene::Entity target = scene::kInvalidEntity);
  static CCameraRig  presetValues(CameraPreset);
  void install(scene::World&);  void uninstall();
  void setNotifySink(void (*fn)(u32 entity, const char* name, void* user), void* user);
  void setShakeSink(void (*fn)(float trauma));  void setBobSink(void (*fn)(float amount, float speed));
  Options& options();
  RigResult drive(scene::World&, const RigFrame&);                       // once per frame, section 2.6.1
  RigResult preview(scene::World&, scene::Entity rig, const RigFrame&);  // stateless: editor "look through"; no lag, no blend, no modifiers
  // inbox (the framework provider and graph nodes call these; applied in arrival order at the top of the next drive)
  bool setViewTarget(scene::Entity, ViewBlend);   bool clearViewTarget(ViewBlend);   bool cut(scene::Entity);
  u32  pushModifier(const CameraModifier&);       bool removeModifier(u32 id, f32 outS);   u32 clearModifiers(u32 tag);
  void shakeAt(Vec3 pos, f32 radiusCm, f32 intensity, f32 falloffPower);
  // reads (last drive)
  const CameraPov& pose() const;  scene::Entity activeRig() const;  bool blending() const;  f32 blendAlpha() const;
  bool rigState(scene::Entity, f32& armCm, u32& state) const;
  void clear();                                   // Play stop / level unload: state, modifiers, inbox
};
CameraRigSystem& cameraRigSystem();               // process singleton, like anim::animSystem()
}
```

The inbox exists for the same reason as soft bodies: a C# or graph call arrives at any point in the frame, and applying all of them at one point in a fixed order keeps a replay deterministic.

#### 2.6.1 `drive` (once per frame, in this order; every loop ascends by entity id)

1. Apply the inbox in arrival order (view target, cut, modifiers, shake).
2. Fast path: no entity carries `CCameraRig` (`world.pool(type)` empty), no blend active, no external hold pending, no inbox work: return `{active = false}`. This is the cost of a project with no rigs.
3. Apply one-shots: for each rig with `kRigApplyPreset`, expand `preset` over the tuning fields and clear the bit; `kRigCut` snaps that rig's lag state next evaluation and clears the bit.
4. **Select the live source.** Explicit view target (valid, carries a rig, not `kRigDisabled`), else the best rig under `frame.possessed`: the pawn itself, a descendant (parent walk up to 16 hops), or any rig whose `target` is the pawn; best = highest `CCamera.priority` (0 when absent), ties by lowest entity id. Else "none" (the fallback pose). A change of source or of the rig's `mode` field starts a blend (2.6.3).
5. **Evaluate the live rig** (2.6.2) into `live`, or use `frame.fallback` for "none".
6. **Blend.** If a blend is running, `pov = blendPov(from, live, blendAlpha(t / dur, curve))`; at `t >= dur` the result is `live` exactly and the blend ends (`CameraBlendFinished`).
7. **External pose.** If `frame.externalActive`, remember `frame.external` as the last external pose and report `active = false` (the host lets the sequencer override stand). When it stops holding the view, start a blend `from = lastExternal` over `Options::externalBlendS`.
8. Apply the **modifier stack** to `pov` (FOV add/scale, punch and roll into the rotation) after the blend, so a recoil does not fight a transition. The stack is evaluated **once per frame** (`ModifierStack::evaluate(dt, motionAllowed)`, right after step 3); its arm-scale and socket-offset results feed the rig evaluation in 2.6.2 step 6, the rest are applied here.
9. Non-finite guard: any NaN or infinity in `pov` discards the frame's rig output, keeps the previous finite pose and logs once per rig.
10. Write OUT fields, publish `pose()`, raise events, return `RigResult`.

The legacy boom is computed by the host **before** `drive` and handed in as `fallback`; when `drive` returns `active == false` the host uses it untouched. A tick with no rig is therefore bit-identical to today.

#### 2.6.2 Evaluating one rig (Chase shown; the mode table in 2.3.1 changes steps 3, 4, 5)

1. **Target.** `target` field, else parent, else the rig entity (invalid or destroyed: treat as the rig entity; destroyed while active: hold the last pose 0.5 s, then blend to the fallback).
2. **Pivot.** The view node when it is a descendant of the target (today's head pivot), else the target origin, plus the world `pivot*` offset.
3. **Look-ahead** (`lookAhead`, planar). Off when `lookAheadS == 0` or `!motionAllowed`.
4. **Position lag** (`lagFollow`). `posLagSpeed` is raised to `reduceMotionLagFloor` when `!motionAllowed`. A pivot displacement above `teleportCm` in one frame, `kRigCut`, an explicit cut, or the first evaluation snaps the lag state and sets `RigResult::cut`.
5. **Rotation.** Source yaw/pitch/roll (2.3.1), each axis replaced by `yawDeg/pitchDeg/0` when its `kRigNoInherit*` bit is set, pitch clamped to the limits (and to +-89 degrees always), then `angleFollow` per axis for `rotLagSpeed` with `rotLagMaxDeg`. Orbit's rotation is the accumulated own angles (look input) with optional auto-recenter.
6. **Modifiers that act before the probe:** arm scale and socket offset.
7. **Desired end** = `pivot' + (-forward * arm * armScale) + (forward * socketX + right * socketY + up * socketZ)` in the rotated basis. This is the segment probed (the socket is inside it, unlike Unreal where it is added afterwards).
8. **Probe** (skipped for `kRigNoCollision`, FirstPerson, Seat, Fixed): `aver_phys_sphere_cast_q` from `pivot'` along the segment, radius `probeRadiusCm`, mask `collisionMask` (or `Options::defaultProbeMask`), ignoring the target's character (`aver_phys_character_of_entity`) and `ignoreBody`. `resolveArm` gives the fraction used; the camera is `pivot' + (end - pivot') * fraction`. A sweep that starts overlapping (distance 0) yields `minArmCm`.
9. **Collision fade** (`fadeHide`) on the final camera-to-pivot distance, for modes with an arm: sets `ownerHideRoot = target`, `hideAll = true`, `kRigStateFaded`.
10. **FOV.** Base from `CCamera.fovYRad` if present (else 60 degrees), plus a smoothed speed kick (`fovKickDeg * clamp01(speed / fovKickSpeedCmS)`, off when `!motionAllowed`), composed with the modifiers in 2.6.1 step 8. Near and far from `CCamera`.
11. **Bob.** FirstPerson and Seat call the bob sink with `bobAmount` and the target's planar speed when `motionAllowed`; otherwise `0, 0`. The global `CameraFx` applies it, with its own gate.

Cost: one sphere cast and a handful of vector operations per frame, no allocation. Per-rig runtime state (lag pivot, look-ahead, rotation lag, arm, fade hysteresis, last mode, blend) lives in a side table keyed by entity, cleared in `clear()`; nothing runtime is saved or serialised.

#### 2.6.3 View-target blending

State: `current` (entity or none), `from` (a `CameraPov`), `t`, `dur`, `curve`. A source change sets `from = lastOutputPov` (the previous frame's final pose, before modifiers), `t = 0`, `dur` = explicit blend seconds if given else the **incoming** rig's `blendTimeS` (the fallback uses 0.4), and `current` to the new source. A change mid-blend uses the current blended pose as the new `from`, so a transition is always continuous. `dur == 0` or `ViewBlend::cut` is a cut: `RigResult::cut` is true and hosts call `streaming_.resetVelocityTracking()` (`GameStreaming.hpp:98-100`). No first-ever output means the first source cuts in.

Events through the notify sink, raised at the new view-target entity (the pawn for "none"): `CameraViewTargetChanged`, `CameraBlendFinished`, `CameraCut`. A graph handles them with `AN_OnEvent`; no new entry node.

### 2.7 New physics query: `aver_phys_sphere_cast_q`

Added to `modules/physics/include/aver/physics/physics_queries_abi.h` (additive; a sphere belongs beside the box and capsule), implemented in `PhysicsQueries.cpp` with the file's own `castClosest` and `QueryBodyFilter`:

```c
/* Sweeps a sphere of `radius` cm from (cx,cy,cz) by the displacement (dx,dy,dz) cm. Returns the first body hit, or 0.
 * Sensors are skipped unless AVER_PHYS_QUERY_INCLUDE_SENSORS. TWO ignore bodies, the one place this header departs from
 * a single ignoreBody: a camera skips the target's character AND a vehicle chassis. Each may be a body or a character handle. */
AVER_PHYS_API int32_t aver_phys_sphere_cast_q(float cx, float cy, float cz,
                                              float dx, float dy, float dz, float radius,
                                              uint32_t layerMask, int32_t ignoreBodyA, int32_t ignoreBodyB, int32_t flags,
                                              float* outDistance, float* outPoint, float* outNormal);
```

Zero-length displacement, `radius <= 0` or no world returns 0 (as `box_cast`). C#: a `DllImport` with the same parameter names in `Native.Queries.cs` (the `AbiEnumTest` name check), and `Physics.SphereCastQ(Float3 centre, float radius, Float3 displacement, uint layerMask = LayerMaskAll, Body ignoreA = default, Body ignoreB = default, QueryFlags flags = QueryFlags.None)` returning `ShapeHit` in `Queries.cs`. The existing `aver_phys_sphere_cast*` functions keep their signatures.

### 2.8 Hosts: one shared header, two call sites each

`Runtime/include/aver/game/GameCamera.hpp` (owned by this feature) gains:

```cpp
struct PlayView {                                     // travels with camPos_/yaw_/pitch_; ONE new member per host
  f32 fovYRad = radians(60.0f);                       // the SAME expression pushCamera uses today, so the default is bit-identical
  f32 rollRad = 0;  f32 nearCm = kCameraNearCm, farCm = kCameraFarCm;
  bool ownerHideAll = false;                          // hide every mesh under ownerHideRoot, not only the flagged ones
  bool cut = false;                                   // this frame teleported the camera
};
inline CameraMatrices pushCamera(rhi::IDevice&, const Vec3& pos, const Vec3& forward, f32 aspect, const PlayView&);
//   up = (0,0,1) when roll == 0 (bit-identical), else the world up turned about `forward` by roll; the old 4-argument overload forwards with a default PlayView
inline scene::Entity drivePlayCamera(Vec3& pos, f32& yaw, f32& pitch, PlayView& view, f32 dt, scene::Entity standInPawn = scene::kInvalidEntity);
//   1. view = PlayView{}   2. compute the legacy pose (the existing body, moved to driveLegacyPlayCamera)
//   3. #if AVER_MODULE_CAMERA: cameraRigSystem().drive(...) with RigFrame{dt, possessed, viewNode, motionAllowed, fallback, sequencer}
//   4. active -> write pos/yaw/pitch/PlayView, return result.ownerHideRoot (valid with hideAll for the fade, else the first-person pawn)
//      not active -> the legacy result, untouched. The old 3-argument overload stays and calls the legacy body only.
```

`motionAllowed` is `aver_fw_access_get("access.motion.reduce") < 0.5f` under `AVER_MODULE_FRAMEWORK` (true otherwise). The sequencer pose is passed as `external` by each host; the `cameraRigSystem().drive` call sits in the shared header, so the hosts only pass `dt`, `PlayView&` and the sequencer pose.

Edits per host, all small and additive:
- **Editor** (`SandboxApp`): `drivePlayCamera(f32 dt)` (declaration `SandboxApp.hpp:1970`, body `SandboxPlay.cpp:939-944`, call `SandboxUpdate.cpp:947`), member `game::PlayView playView_`, `pushCamera(..., playView_)` at `SandboxUpdate.cpp:1101`, `opts.ownerHideAll = playView_.ownerHideAll` beside `ownerHideRoot` at `SandboxRender.cpp:429`, `:1568`, the three `trifactor::View` sites use `playView_.fovYRad` (`:396`, `:1068`, `:1215`, `:1349`), `playView_ = {}` whenever the free-fly camera is live (not Play, ejected). The pilot override (`viewOverride_`, `SandboxApp.hpp:3356-3361`) is untouched.
- **Packaged game** (`GameApp`): same member, `drivePlayCamera` at `GameApp.cpp:600-604` and `:2244`, `pushCamera` at `:1697`, `opts.ownerHideAll` at `:2339`, the one-time log line at `:1702` prints the live FOV, `GameLandscape::draw` gets a trailing `f32 fovYRad = radians(60.0f)` argument (`GameLandscape.cpp:364-386`).
- `GameRender.cpp:735`: `if (((mr->flags & kMeshRendererHiddenFromOwner) || options.ownerHideAll) && options.ownerHideRoot != kInvalidEntity)`. `GameRender.hpp:335`: one bool field, default false.
- `GameSystemsWiring.hpp`: `registerGameSystems` registers the component, installs the system, installs the provider (`aver_fw_set_camera_provider`), the shake sink (`aver_fw_camera_shake_add`) and the bob sink (`aver_fw_camera_bob_set`), and sets `defaultProbeMask` from `softBodySystem().options()` when that module is present; `resetGameSystems` calls `cameraRigSystem().clear()`. Notify sinks join the soft-body line in both hosts (`SandboxInit.cpp:733`, `GameApp.cpp:1343`).
- **Editor Play** only runs the rig during a session (`playSessionActive()`); authoring is untouched. The "look through rig" pilot in 2.11 uses `preview()`.

**D3D12 and Vulkan parity.** No shader, no RHI call, no binding table (the table-1 base rule is not touched). The only renderer-visible change is that the matrices the hosts already hand to `IDevice::setCamera` may have a different FOV or a roll. Both backends take the same matrices, and every consumer already factors them (`CameraFactor.hpp:26-44`). It is verified headed on both backends in section 6, including the temporal passes under an FOV kick.

### 2.9 Framework relay ABI, `framework_camera_abi.h` (new, `aver_fw_camera_*`)

New header and `modules/framework/src/CameraAbi.cpp`, own `AVER_FW_CAMERA_ABI_VERSION 1` (`AVER_FW_ABI_VERSION_MINOR` untouched), the `framework_softbody_abi.h` conventions: `int32_t/float` only, 1 on success and 0 on refusal, no provider returns 0 and writes nothing.

```c
#define AVER_FW_CAMERA_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_camera_abi_version(void);

#define AVER_FW_CAM_OP_ATTACH           1   /* entity = rig; in: preset target */
#define AVER_FW_CAM_OP_SET_VIEW_TARGET  2   /* entity = rig (0 = the legacy/pawn camera); in: blendSeconds curve cut */
#define AVER_FW_CAM_OP_CLEAR_VIEW_TARGET 3  /* in: blendSeconds curve; back to automatic selection */
#define AVER_FW_CAM_OP_PUSH_MODIFIER    4   /* in: kind priority inS holdS outS a b c d tag flags; out i[0] = id */
#define AVER_FW_CAM_OP_REMOVE_MODIFIER  5   /* in: id outSeconds (<0 = its own) */
#define AVER_FW_CAM_OP_CLEAR_MODIFIERS  6   /* in: tag (0 = all); out i[0] = count */
#define AVER_FW_CAM_OP_SHAKE_AT         7   /* in: x y z radius intensity falloffPower */
#define AVER_FW_CAM_OP_POSE             8   /* outF: x y z yawRad pitchRad rollRad fovYRad nearCm farCm; outI: activeRigEntity stateBits(bit0 blending) */
#define AVER_FW_CAM_OP_RIG_STATE        9   /* entity; outF: armCm blendAlpha; outI: outState */
#define AVER_FW_CAM_OP_CUT             10   /* entity (0 = the active rig): snap the lag state, no blend */

typedef int32_t (AVER_FW_CALL* aver_fw_camera_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                  float* outF, int32_t* outI, int32_t cap, void* user);
AVER_FW_ABI int32_t aver_fw_set_camera_provider(aver_fw_camera_fn fn, void* user);   /* null clears; always 1 */

AVER_FW_ABI int32_t aver_fw_camera_attach(int32_t entity, int32_t preset, int32_t targetEntity);
AVER_FW_ABI int32_t aver_fw_camera_set_view_target(int32_t entity, float blendSeconds, int32_t curve, int32_t cut);
AVER_FW_ABI int32_t aver_fw_camera_clear_view_target(float blendSeconds, int32_t curve);
AVER_FW_ABI int32_t aver_fw_camera_push_modifier(int32_t kind, int32_t priority, float inS, float holdS, float outS,
                                                 float a, float b, float c, float d, int32_t tag, int32_t flags, int32_t* outId);
AVER_FW_ABI int32_t aver_fw_camera_remove_modifier(int32_t id, float outSeconds);
AVER_FW_ABI int32_t aver_fw_camera_clear_modifiers(int32_t tag);
AVER_FW_ABI int32_t aver_fw_camera_shake_at(float x, float y, float z, float radiusCm, float intensity, float falloffPower);
AVER_FW_ABI int32_t aver_fw_camera_pose(float* outPose9, int32_t* outRigAndState2);
AVER_FW_ABI int32_t aver_fw_camera_rig_state(int32_t entity, float* outArmAndBlend2, int32_t* outState);
AVER_FW_ABI int32_t aver_fw_camera_cut(int32_t entity);
```

The provider `camera::cameraRigProvider` is a free function in `Aver.Camera` with `user = &cameraRigSystem()`, installed by `registerGameSystems`. Every op except the reads forwards to the inbox. The `AVER_FW_CAM_*` constants (modes, presets, curves, kinds, flag bits, state bits) sit in the same header.

### 2.10 C# API

`scripting/csharp/Aver.Framework/CameraRig.cs`, `CameraRigNative.cs` (P/Invoke, `AnimNative.cs` pattern), `EntityCameraRig.cs` (partial `Entity`, field writes over the generic scene ABI, `MotionMatching.cs` pattern: a host that never registered the component gets `false`, not a throw).

```csharp
public enum CameraMode { Chase = 0, Orbit = 1, FirstPerson = 2, Seat = 3, TopDown = 4, Fixed = 5 }
public enum CameraRigPreset { Chase = 0, VehicleChase = 1, Orbit = 2, FirstPerson = 3, Seat = 4, TopDown = 5, Fixed = 6 }
public enum CameraBlendCurve { EaseInOut = 0, Linear = 1, EaseIn = 2, EaseOut = 3 }
public enum CameraModifierKind { FovKick = 0, FovScale = 1, Punch = 2, RollLean = 3, ArmScale = 4, SocketOffset = 5 }
[Flags] public enum CameraRigFlags { NoCollision = 1, NoInheritYaw = 2, NoInheritPitch = 4, NoInheritRoll = 8, NoFade = 16, LookAtTarget = 32, Disabled = 64 }

/// <summary>The camera after the active rig and the modifier stack, before the player's shake and bob.</summary>
public readonly record struct CameraPose(Vec3 Position, float YawDegrees, float PitchDegrees, float RollDegrees,
                                         float FovDegrees, float NearCm, float FarCm);
/// <summary>One rig's runtime state.</summary>
public readonly record struct CameraRigState(float ArmCm, bool Active, bool Blending, bool Colliding, bool Faded);

public readonly partial struct Entity
{
    /// <summary>Adds a CCameraRig seeded from <paramref name="preset"/> (and a CCamera). <paramref name="target"/> is the entity
    /// the arm follows; none means the rig's parent, else the rig's own entity. False for a dead entity or a build without the module.</summary>
    public bool AddCameraRig(CameraRigPreset preset = CameraRigPreset.Chase, Entity target = default);
    public bool HasCameraRig { get; }
    public bool RemoveCameraRig();
    /// <summary>Changes the mode; the rig blends into it over its blend time.</summary>
    public bool SetCameraRigMode(CameraMode mode);
    public bool SetCameraRigTarget(Entity target);
    /// <summary>Arm length, probe radius and whether the arm collides (centimetres).</summary>
    public bool SetCameraRigArm(float lengthCm, float probeRadiusCm = 12f, bool collide = true);
    /// <summary>Offset at the arm's end in the camera frame: forward, right, up.</summary>
    public bool SetCameraRigSocket(Vec3 cameraFrameOffset);
    /// <summary>Offset of the arm origin in world axes (eye height).</summary>
    public bool SetCameraRigPivot(Vec3 worldOffset);
    public bool SetCameraRigLag(float positionSpeed, float rotationSpeed = 0f, float maxDistanceCm = 0f);
    public bool SetCameraRigLookAhead(float seconds, float maxCm = 150f);
    public bool SetCameraRigPitchLimits(float minDegrees, float maxDegrees);
    /// <summary>Adds look input in degrees to the rig's own yaw and pitch (Orbit, TopDown, Seat head look).</summary>
    public bool AddCameraRigLook(float yawDegrees, float pitchDegrees);
    public bool SetCameraRigFade(float startCm, float endCm);
    public bool SetCameraRigBlend(float seconds, CameraBlendCurve curve = CameraBlendCurve.EaseInOut);
    /// <summary>A physics body the camera probe skips (the chassis of a vehicle). Process-local; not saved.</summary>
    public bool SetCameraRigIgnoreBody(int bodyHandle);
    public bool SetCameraRigFlags(CameraRigFlags flags);
    public bool TryGetCameraRigState(out CameraRigState state);
}

public static class CameraRig
{
    public const string EventViewTargetChanged = "CameraViewTargetChanged", EventBlendFinished = "CameraBlendFinished", EventCut = "CameraCut";
    /// <summary>Makes <paramref name="rig"/> the view. Overrides automatic selection until <see cref="ClearViewTarget"/>.
    /// Entity.None returns to the legacy pawn camera. <paramref name="cut"/> skips the blend.</summary>
    public static bool SetViewTarget(Entity rig, float blendSeconds = 0.5f, CameraBlendCurve curve = CameraBlendCurve.EaseInOut, bool cut = false);
    public static bool ClearViewTarget(float blendSeconds = 0.5f, CameraBlendCurve curve = CameraBlendCurve.EaseInOut);
    public static bool Cut(Entity rig = default);
    public static Entity ActiveRig { get; }
    public static bool IsBlending { get; }
    public static bool TryGetPose(out CameraPose pose);
    // modifiers: every call returns an id (0 on refusal) to pass to RemoveModifier
    public static int PushFovKick(float deltaDegrees, float inSeconds = 0.05f, float holdSeconds = 0.1f, float outSeconds = 0.4f, int tag = 0);
    public static int PushFovScale(float multiplier, float blendSeconds = 0.2f, int tag = 0);      // held until removed
    public static int PushPunch(float pitchDegrees, float yawDegrees = 0f, float rollDegrees = 0f, float recoverSeconds = 0.3f, int tag = 0);
    public static int PushRoll(float degrees, float blendSeconds = 0.3f, int tag = 0);               // held until removed
    public static int PushArmScale(float factor, float blendSeconds = 0.3f, int tag = 0);            // held until removed
    public static int PushSocketOffset(Vec3 cameraFrameOffset, float blendSeconds = 0.2f, int tag = 0);   // held until removed
    public static bool RemoveModifier(int id, float outSeconds = -1f);
    public static int ClearModifiers(int tag = 0);
    /// <summary>Camera shake, 0..1, the existing CameraFx trauma.</summary>
    public static void AddShake(float trauma) => CameraFx.AddShake(trauma);
    /// <summary>Shake that fades with distance from the current camera: intensity * (1 - d/radius)^falloffPower.</summary>
    public static bool ShakeAt(Vec3 worldPosition, float radiusCm, float intensity, float falloffPower = 2f);
    /// <summary>One frame of orbit control from the mouse: look input scaled by <paramref name="degreesPerPixel"/> and the wheel as arm change.</summary>
    public static void DriveOrbitFromMouse(Entity rig, float degreesPerPixel = 0.15f, float wheelCmPerNotch = 40f);
}
internal static class CameraGraph { /* the ...ForGraph wrappers, section 2.11 */ }
```

`AverCharacter` (owned by this feature, `Character.cs`) gets `public bool AttachCameraRig(CameraRigPreset preset = CameraRigPreset.Chase)` (calls `Self.AddCameraRig(preset)` and seeds the arm from `BoomLength` and the pivot from nothing, since the view node already sits at `EyeHeight`). With a rig attached, `CameraViewMode` toggles between `FirstPerson` and the rig's configured third-person mode (`SetCameraRigMode`); `BoomLength` is ignored afterwards. No rig, no behaviour change. `ClassBuilder.CameraRig(CameraRigPreset)` adds the component and defaults `preset` plus `kRigApplyPreset`.

### 2.11 Aver Node: family `Camera` (23 nodes)

Follows the wave 9 C contract exactly: `scripting/csharp/Aver.Graph/GraphCameraNodeTable.cs` (`internal static partial class GameSystemNodes { private static GenericNodeSpec[] CameraTable() => new[] {...}; }`), palette mirror `sandbox/src/GraphNodeDefsCamera.hpp` (`inline void appendCameraNodeDefs(std::vector<GraphNodeDesc>& t)`), owner `internal static class CameraGraph` in `Aver.Framework` (methods `...ForGraph`, thin wrappers over the public members above), tests `[AutoRun] static class CameraNodeTests { public static int RunAll() }` built from `NodeRowKit`. All auto-discovered: **no edit** to `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Keys are lower-case palette ids with the `an_camera` prefix (no collision with the parser's labels, `an_setcamerafov/clip/priority`, `addcamerashake`, `setcamerabob`). Palette category `Camera` (shared with the RenderProps camera nodes). Failure: false/0, log once per node id, never throw. Pin types are `float/int/bool/exec`; entities and handles are ints; enums are ints documented on the generated page.

| Key | Kind | Pins (besides `exec`/`then`) | Wraps |
|---|---|---|---|
| `an_cameraaddrig` | Exec | in `entity:i`, `preset:i=0`, `target:i=0`; out `success:b` | `Entity.AddCameraRig` |
| `an_camerasetmode` | Exec | in `entity:i`, `mode:i=0`; out `success:b` | `SetCameraRigMode` |
| `an_camerasettarget` | Exec | in `entity:i`, `target:i`; out `success:b` | `SetCameraRigTarget` |
| `an_camerasetviewtarget` | Exec | in `entity:i`, `blendSeconds=0.5`, `curve:i=0`; out `success:b` | `CameraRig.SetViewTarget` |
| `an_cameraclearviewtarget` | Exec | in `blendSeconds=0.5`, `curve:i=0`; out `success:b` | `CameraRig.ClearViewTarget` |
| `an_cameracut` | Exec | in `entity:i=0`; out `success:b` | `CameraRig.Cut` |
| `an_camerasetarm` | Exec | in `entity:i`, `lengthCm=450`, `probeRadiusCm=12`, `collide:b=true`; out `success:b` | `SetCameraRigArm` |
| `an_camerasetsocket` | Exec | in `entity:i`, `x`, `y`, `z` (forward, right, up); out `success:b` | `SetCameraRigSocket` |
| `an_camerasetpivot` | Exec | in `entity:i`, `x`, `y`, `z` (world); out `success:b` | `SetCameraRigPivot` |
| `an_camerasetlag` | Exec | in `entity:i`, `posSpeed=10`, `rotSpeed=0`, `maxDistCm=0`; out `success:b` | `SetCameraRigLag` |
| `an_camerasetlookahead` | Exec | in `entity:i`, `seconds=0.25`, `maxCm=150`; out `success:b` | `SetCameraRigLookAhead` |
| `an_camerasetlimits` | Exec | in `entity:i`, `pitchMinDeg=-80`, `pitchMaxDeg=80`; out `success:b` | `SetCameraRigPitchLimits` |
| `an_cameraaddlook` | Exec | in `entity:i`, `yawDeg`, `pitchDeg`; out `success:b` | `AddCameraRigLook` |
| `an_camerasetfade` | Exec | in `entity:i`, `startCm=140`, `endCm=70`; out `success:b` | `SetCameraRigFade` |
| `an_camerapushfovkick` | Exec | in `deltaDeg=8`, `inSeconds=0.05`, `holdSeconds=0.1`, `outSeconds=0.4`; out `id:i` | `PushFovKick` |
| `an_camerapushzoom` | Exec | in `fovScale=0.6`, `blendSeconds=0.2`; out `id:i` | `PushFovScale` |
| `an_camerapunch` | Exec | in `pitchDeg`, `yawDeg`, `rollDeg`, `recoverSeconds=0.3`; out `id:i` | `PushPunch` |
| `an_cameraremovemodifier` | Exec | in `id:i`, `outSeconds=-1`; out `success:b` | `RemoveModifier` |
| `an_cameraclearmodifiers` | Exec | in `tag:i=0`; out `count:i` | `ClearModifiers` |
| `an_camerashakeat` | Exec | in `x`, `y`, `z`, `radiusCm=1500`, `intensity=0.6`; out `success:b` | `ShakeAt` |
| `an_cameragetpose` | Pure | out `x`, `y`, `z`, `yawDeg`, `pitchDeg`, `rollDeg`, `fovDeg`, `found:b` | `TryGetPose` |
| `an_cameragetrigstate` | Pure | in `entity:i`; out `armCm`, `colliding:b`, `active:b`, `blending:b`, `found:b` | `TryGetCameraRigState` |
| `an_cameragetviewtarget` | Pure | out `entity:i`, `blending:b`, `blendAlpha` | `ActiveRig`, `IsBlending` |

Row grammar example (verified against `GraphRenderPropsNodeTable.cs:22`):

```csharp
Reg("an_camerasetviewtarget", GenericKind.Exec, typeof(CameraGraph), "SetViewTargetForGraph",
    "entity:int,blendSeconds:float=0.5,curve:int=0", "success:bool", "p:entity,p:blendSeconds,p:curve", "success"),
Reg("an_cameragetpose", GenericKind.Pure, typeof(CameraGraph), "GetPoseForGraph", "", "x:float,y:float,z:float,yawDeg:float,pitchDeg:float,rollDeg:float,fovDeg:float,found:bool", ">x,>y,>z,>yawDeg,>pitchDeg,>rollDeg,>fovDeg", "found"),
```

`GraphComponentTree.cs` gains a `camerarig` `COMP` kind (attributes `preset`, `mode`, `arm`, `target`: a graph class assembles a rig with its pawn), and `GraphEditorComponents.cpp` gains the kind and attribute rows. The `camera` kind's blurb "no renderer reads CCamera yet" is corrected (a rig reads it).

### 2.12 Cross-feature contracts (exact signatures)

**Offered by GAMEPLAY_CAMERAS (VEHICLES consumes; it writes no camera math):**

```csharp
// C#, Aver.Framework
public bool Entity.AddCameraRig(CameraRigPreset preset = CameraRigPreset.Chase, Entity target = default);
public bool Entity.SetCameraRigMode(CameraMode mode);
public bool Entity.SetCameraRigTarget(Entity target);
public bool Entity.SetCameraRigIgnoreBody(int bodyHandle);
public static bool CameraRig.SetViewTarget(Entity rig, float blendSeconds = 0.5f, CameraBlendCurve curve = CameraBlendCurve.EaseInOut, bool cut = false);
public static bool CameraRig.ShakeAt(Vec3 worldPosition, float radiusCm, float intensity, float falloffPower = 2f);   // optional, for impacts
```
```cpp
// C++, Aver.Camera
camera::CCameraRig* camera::CameraRigSystem::attach(scene::World&, scene::Entity rig, camera::CameraPreset, scene::Entity target);
bool camera::CameraRigSystem::setViewTarget(scene::Entity, camera::ViewBlend);
```
```c
int32_t aver_fw_camera_attach(int32_t entity, int32_t preset, int32_t targetEntity);
```
The VEHICLES flow: create the rig entity as a child of the vehicle, `AddCameraRig(VehicleChase, vehicleEntity)`, `SetCameraRigIgnoreBody(chassisBody)`, and for the driver seat a second child rig `AddCameraRig(Seat)`; entering the car is `CameraRig.SetViewTarget(seatRig, 0.6f)`, a camera-mode key is `SetCameraRigMode` or another `SetViewTarget`.

**Consumed:**
- From GPU_PARTICLES: none. From SOFTBODY_DEPTH: none (its `cloth`/`proxy` layers are read through `softBodySystem().options()` when that module is present, wave 9 code already in the tree).
- Existing: wave 9 `CameraFx` (`aver_fw_camera_shake_add`, `aver_fw_camera_bob_set`) and `aver_fw_access_get`; `aver_phys_character_of_entity`; the wave 9 Aver Node and notify-sink contracts.

### 2.13 Level records, Add Component, Details, overlay

**`COMPONENT CameraRig`** (type appended to `OcWorld.cpp:173-174` and `LevelComponents.hpp:28-29`, enum and applier beside `applyCloth`). Record keys (canonical order, default omitted; `preset` always written; a key is omitted when it equals the preset's value, so a `VehicleChase` record stays short). The applier calls `CameraRigSystem::attach(preset)` then overrides:

| Key | Values (default) | Field |
|---|---|---|
| `preset` | `chase` (default), `vehiclechase`, `orbit`, `firstperson`, `seat`, `topdown`, `fixed` | `preset` |
| `mode` | `chase`, `orbit`, `firstperson`, `seat`, `topdown`, `fixed` (the preset's) | `mode` |
| `target` | `self` (default), `parent` | `target` (a level can name only these two; anything else is runtime) |
| `arm`, `minarm`, `probe`, `recover` | cm / seconds | `armLengthCm`, `minArmCm`, `probeRadiusCm`, `recoverHalfLifeS` |
| `socket`, `pivot` | `x,y,z` | `socket*`, `pivot*` |
| `lag`, `lagmax`, `rotlag`, `rotlagmax`, `lagstep` | | lag fields |
| `ahead`, `aheadmax` | | look-ahead |
| `pitchmin`, `pitchmax`, `yaw`, `pitch`, `recenter` | | rotation |
| `blend`, `curve` | seconds, `easeinout`/`linear`/`easein`/`easeout` | blend |
| `fovkick`, `fovkickspeed`, `fadestart`, `fadeend`, `bob`, `seatyaw`, `seatpitch` | | feel |
| `mask` | hex bitmask | `collisionMask` |
| `collision`, `inheritpitch`, `inherityaw`, `inheritroll`, `fade` | `1` default; `0` sets the negative flag | flags |
| `lookat`, `disabled` | `0` default | flags |

`ignoreBody` and the OUT fields are never saved. A loaded level starts with no active view target: a pawn with a rig under it is picked by possession as soon as Play starts. The spec text (types, keys, defaults, canonical form) is added to `docs/formats/FORMAT_SPECS.md` by the integrator from this table.

**Add Component.** `kPlacedKinds[]` gets `{"CameraRig", "Camera Rig", "Spring-arm camera for this entity: chase, orbit, first person, seat, top-down or fixed, with collision, lag and blends.", "Camera Rig", 0, "CCameraRig", {}}` (no asset). `canAttach` keeps the mesh rule (`PlacedComponentModel.hpp:117`): a rig on a placed mesh (a car, a prop camera) is supported; a class-based pawn gets its rig from code or a graph `COMP` (open question 4). `makeNewRecord` writes `preset=chase`.

**Details panel** (`SandboxComponentDetails.cpp`, widget half; ImGui-free model in `sandbox/src/CameraRigDetailsModel.hpp`): preset combo (applies `presetRig`, one undo step), mode combo, target (`self`/`parent`), sections Arm (length, min, probe radius, mask checklist from the layer names, collision, recover), Offsets (pivot, socket), Lag (position, rotation, max distances, sub-step), Look-ahead, Rotation (limits, own yaw/pitch, recenter, inherit toggles), Blend (time, curve), Feel (FOV kick, bob, fade distances), a read-only line of the OUT fields in Play (arm, colliding, faded, blending, FOV). Buttons: **Look through** (pilots the viewport with `preview()` until Esc, using the same `viewOverride_` path as the sequencer pilot, `SandboxUpdate.cpp:955-966`), **Make live** (in Play: `SetViewTarget`), **Cut**, **Test shake**, **Test FOV kick**. A warning row when `probeRadiusCm` is below the near-plane corner radius.

**Debug overlay** (`sandbox/src/CameraRigDebugOverlay.hpp/.cpp`, the `MotionDebugOverlay` pattern: `collect` emits `rhi::LineVertex`, hooked under `!noEditorChrome_`): pivot cross, lagged pivot (Unreal's debug lag markers), arm segment coloured green clear / orange shortened, the probe sphere at the end and at the hit, the desired end point, the look-ahead vector, the fade radius, the pitch limits as arcs. `Show > Camera Rigs`.

**Content.** `test-content/CameraLab/` (new): a level of cubes and ramps only (no third-party assets): a corridor, a doorway, a low ceiling, a trigger volume, a kinematic mover, a slope, a pawn with a Chase rig and a Seat child on a kinematic cart. Nothing is added to Content Browser types (no new asset).

### 2.14 Accessibility, determinism, failure, budgets

- **Reduce motion** (decision 8). The rig reads one bool per frame; `CameraFx` keeps its own gate. `ACCESSIBILITY.md`'s "Reduce motion and camera feel" section gains one sentence listing what the rig adds to the gated set (integrator).
- **Determinism.** Inputs: component fields, target world matrices after `World::flush`, the Jolt query results, `dt`, and the ordered inbox. No wall clock, no `rand`; the lead estimator, lag and envelopes are pure state machines; ids come from counters advanced in call order. Replay equality is tested (section 5). Cross-platform float identity is not claimed.
- **Failure.** A missing component, a dead entity, no physics world, no provider: the call returns false/0 and logs once per site; the camera falls back to the legacy pose. A rig on a destroyed target holds its last pose for 0.5 s then blends to the fallback. Non-finite output is discarded (2.6.1 step 9).
- **Budgets.** One probe per frame, one rig evaluated per frame, modifier stack capped at 32 (further pushes refused, logged once), per-rig state map bounded by the number of live rigs. No per-frame allocation.
- **Pause and Play boundaries.** `drivePlayCamera` already runs only while PLAYING (`GameCamera.hpp:104`); paused sessions keep the last pose and the rig state, resuming continues the lag. `resetGameSystems` (Play stop, level unload) clears the system.

### 2.15 Patents and clean-room notes

No technique here is known to carry live patent risk: exponential and critically damped following, swept-sphere camera probes, trauma-based shake (already shipped in `CameraFx.hpp`, Eiserloh GDC 2016), smoothstep blends and look-ahead are textbook. Game-camera patents from the 1990s (lock-on targeting, automatic re-centring behind the player) have expired by term to my knowledge; **UNVERIFIED**. `Fixed` with `kRigLookAtTarget` is a smoothed look-at, not a lock-on targeting system. If an automatic lock-on camera is ever added, run the USPTO Global Dossier check first (the `aver-uspto-global-dossier` memory note) and design a clean-room alternative (player-driven target cycling, no automatic framing). Unreal's source was not consulted; names and parameter meanings follow its public documentation only.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

Module `Aver.Camera`
- `modules/camera/module.json`
- `modules/camera/CMakeLists.txt`
- `modules/camera/README.md`
- `modules/camera/include/aver/camera/CameraTypes.hpp`
- `modules/camera/include/aver/camera/CameraRigMath.hpp`
- `modules/camera/include/aver/camera/CameraModifiers.hpp`
- `modules/camera/include/aver/camera/CameraRigPresets.hpp`
- `modules/camera/include/aver/camera/CameraRigSystem.hpp`
- `modules/camera/src/CameraRigSystem.cpp`
- `modules/camera/src/CameraRigEval.cpp`
- `modules/camera/src/CameraRigProvider.cpp`

Framework relay
- `modules/framework/include/aver/framework/framework_camera_abi.h`
- `modules/framework/src/CameraAbi.cpp`

C#
- `scripting/csharp/Aver.Framework/CameraRig.cs`
- `scripting/csharp/Aver.Framework/CameraRigNative.cs`
- `scripting/csharp/Aver.Framework/EntityCameraRig.cs`
- `scripting/csharp/Aver.Framework/CameraGraph.cs`
- `scripting/csharp/Aver.Graph/GraphCameraNodeTable.cs`
- `scripting/csharp/Aver.Graph.Tests/CameraNodeTests.cs`

Editor (`sandbox/src/`)
- `sandbox/src/GraphNodeDefsCamera.hpp`
- `sandbox/src/CameraRigDetailsModel.hpp`
- `sandbox/src/CameraRigDebugOverlay.hpp`
- `sandbox/src/CameraRigDebugOverlay.cpp`

Tests
- `tests/camera/CMakeLists.txt`
- `tests/camera/src/CameraRigMathTest.cpp`
- `tests/camera/src/CameraModifiersTest.cpp`
- `tests/camera/src/CameraRigSystemTest.cpp`
- `tests/framework_camera/CMakeLists.txt`
- `tests/framework_camera/src/FrameworkCameraAbiTest.cpp`
- `tests/physics/src/SphereCastQTest.cpp`
- `tests/game/src/GameCameraTest.cpp`
- `tests/game/src/CameraRigRecordTest.cpp`
- `tests/editor/src/CameraRigDetailsModelTest.cpp`

Content and docs
- `test-content/CameraLab/` (level and its cubes-only content, created in phase G)
- `docs/CAMERAS.md` (user-facing: modes, presets, the Unreal mapping table, scripting, accessibility)

### 3.2 Modified, owned by this feature

- `Runtime/include/aver/game/GameCamera.hpp`
- `modules/physics/include/aver/physics/physics_queries_abi.h`
- `modules/physics/src/PhysicsQueries.cpp`
- `scripting/csharp/Aver.Physics/Native.Queries.cs`
- `scripting/csharp/Aver.Physics/Queries.cs`
- `scripting/csharp/Aver.Framework/Character.cs`
- `scripting/csharp/Aver.Framework/ClassBuilder.cs`
- `scripting/csharp/Aver.Graph/GraphComponentTree.cs`
- `sandbox/src/GraphEditorComponents.cpp`
- `Runtime/include/aver/game/GameLandscape.hpp`
- `Runtime/src/GameLandscape.cpp`
- `tests/physics/CMakeLists.txt` (one target; the soft-body wave also added one, merge by keeping both)

Not touched: `scene_abi.h`, `Components.hpp` (the rig is runtime-registered; `CCamera` keeps its struct), `Builtins.cpp`, `framework_abi.h`, `modules/rhi*`, any shader, `CameraFx.hpp` (reused as is), the sequencer.

### 3.3 SHARED (another wave-10 feature or the integrator also touches these; every edit is small and additive)

- `CMakeLists.txt` (root): `add_subdirectory(modules/camera)` in the scene block beside soft body (`:305-313`), `add_subdirectory(tests/camera)` and `tests/framework_camera` in the test block (`:565-575`, `:588`)
- `cmake/modules.catalog.json` (regenerated by `scripts/module-catalog.py`)
- `docs/MODULES.md`
- `modules/framework/CMakeLists.txt` (add `src/CameraAbi.cpp`)
- `Runtime/CMakeLists.txt` (PUBLIC link `Aver.Camera` into `Aver.Runtime.Game.Core`, beside the soft-body links `:155-162`)
- `Runtime/include/aver/game/GameSystemsWiring.hpp`
- `Runtime/include/aver/game/GameRender.hpp`
- `Runtime/src/GameRender.cpp`
- `Runtime/include/aver/game/GameApp.hpp`
- `Runtime/src/GameApp.cpp`
- `Runtime/include/aver/game/LevelComponents.hpp`
- `Runtime/src/LevelComponents.cpp`
- `modules/formats/src/OcWorld.cpp`
- `sandbox/src/SandboxApp.hpp`
- `sandbox/src/SandboxApp.cpp`
- `sandbox/src/SandboxUpdate.cpp`
- `sandbox/src/SandboxInit.cpp`
- `sandbox/src/SandboxPlay.cpp`
- `sandbox/src/SandboxRender.cpp`
- `sandbox/src/SandboxComponentDetails.cpp`
- `sandbox/src/PlacedComponentModel.hpp`
- `sandbox/CMakeLists.txt` (add the overlay source)
- `tests/game/CMakeLists.txt` (two targets)
- `tests/editor/CMakeLists.txt` (one target)
- `tests/abi/src/AbiEnumTest.cpp` (the `AVER_FW_CAM_*` constants; the physics row already lists `physics_queries_abi.h`)
- `docs/ABI.md`
- `docs/formats/FORMAT_SPECS.md`
- `docs/SCRIPTING_API.md`
- `docs/ACCESSIBILITY.md` (one sentence)
- `CHANGELOG.md`

Generated by the integrator, not edited by hand: `docs/nodes/Camera.md` (`scripts/node-catalog.py --write`), `docs/api/Aver.Framework.md`, `sandbox/src/GraphNodeFamilies.gen.inc`.

To recycle: none. The legacy boom stays as the fallback; `drivePlayCamera`'s three-argument overload and `pushCamera`'s four-argument overload stay.

---

## 4. Phased implementation

Each phase leaves the tree buildable and its tests green before the next starts. Standing rules: lean comments, headed manual runs, do not rebuild over the owner's running editor.

**Phase A: pure core (M).** `CameraTypes.hpp`, `CameraRigMath.hpp`, `CameraModifiers.hpp`, `CameraRigPresets.hpp`, `modules/camera/{module.json,CMakeLists.txt}` as an INTERFACE-plus-sources target with no scene or physics yet, `tests/camera` math and modifier suites. Exit: `CameraRigMathTest`, `CameraModifiersTest` green.

**Phase B: physics query (S).** `aver_phys_sphere_cast_q`, `Native.Queries.cs`, `Queries.cs`, `SphereCastQTest`. Exit: new test green; the existing `QueryTest`, `LayerTest`, `CharacterTest` unchanged and green; `AbiEnumTest` physics row green.

**Phase C: component and system (L).** `CCameraRig` registration with size assert, `attach`, `drive`/`preview`, selection, evaluation of the six modes, probe, arm resolve, fade, blend, inbox, events, notify and shake/bob sinks, `clear`. `modules/camera` gains the scene and physics dependencies. `CameraRigSystemTest` (real Jolt). Exit: green.

**Phase D: hosts (M).** `PlayView`, `pushCamera` overload, `drivePlayCamera` rig branch, `GameRender` `ownerHideAll`, the FOV sites, `GameLandscape` argument, `GameSystemsWiring` registration and reset, both hosts' members and calls, notify sinks, `motionAllowed`. `GameCameraTest`. Exit: green; a project with no rig renders identically (screenshot hash compare on a fixed `--frames` run, D3D12, remote-run rules).

**Phase E: framework relay and C# (M).** `framework_camera_abi.h`, `CameraAbi.cpp`, provider, `CameraRig.cs`, `EntityCameraRig.cs`, `Character.AttachCameraRig`, `ClassBuilder.CameraRig`, `AbiEnumTest` constants, `FrameworkCameraAbiTest`. Exit: green; C# builds.

**Phase F: Aver Node (S).** `GraphCameraNodeTable.cs`, `CameraGraph.cs`, `GraphNodeDefsCamera.hpp`, `CameraNodeTests.cs`, `camerarig` COMP kind. Exit: `GraphManagedTests` green; `scripts/node-catalog.py --check` green (the integrator's `NodeCatalogCheck`).

**Phase G: level records and editor (M).** `COMPONENT CameraRig` applier and capture, `OcWorld.cpp` type, Add Component kind, Details model and section, Look through, debug overlay, `Show` toggle, `CameraLab` content. Exit: `CameraRigRecordTest`, `CameraRigDetailsModelTest` green.

**Phase H: docs and integration (S).** `docs/CAMERAS.md`; the integrator's shared-doc edits listed in 3.3. Exit: `docs-check.py` clean, `ctest` green.

---

## 5. Tests

All headless. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

**`tests/camera` (links `Aver.Camera Aver.Core`; the system test also `Aver.Physics Aver.Scene`)**
- `CameraRigMathTest`:
  - `expFollow` halves the error after `ln2 / speed` seconds (1e-3).
  - `lagFollow` frame-rate independence: a pivot moving at 600 cm/s settles to a steady lag of `v / speed` within 1.5% at 30, 60, 144 and 240 fps; sub-stepping off (`maxStepS` huge) is measurably worse at 30 fps, which proves the sub-step.
  - `lagMaxDist` is a hard bound after a 5000 cm jump; `speed <= 0` returns the target exactly.
  - `angleFollow` crosses +-180 by the short way; `maxDist` clamps.
  - `basisFromYawPitchRoll` / `yawPitchRollFromBasis` round trip to 1e-4 rad over a grid including roll, and `basis.fwd` equals `cameraForward(yaw, pitch)` bit for bit at roll 0 (the bit-identity the hosts rely on).
  - `blendPov`: alpha 0 and 1 return the endpoints exactly; slerp takes the short path; position, FOV and clip are linear.
  - `blendAlpha`: endpoints exact, monotone for all four curves, `EaseInOut(0.5) == 0.5`.
  - `lookAhead`: converges to `velocity * aheadS` clamped to `maxCm`; a 2000 cm jump (over `teleportCm`) resets and reports `teleported`; vertical velocity ignored.
  - `resolveArm`: inward instant, outward half-life correct to 5%, never below `minArm`, never above `armWanted`.
  - `fadeHide` hysteresis: hides below `endCm`, stays hidden until `endCm * 1.15`.
  - `composeFov` clamps to 10..140 degrees and composes kick, add and scale.
  - Determinism: the same scripted inputs give bit-identical outputs twice.
- `CameraModifiersTest`: envelope in/hold/out values at sampled times; `holdS < 0` holds until `remove`; `remove` mid-hold blends out from the **current** alpha (no jump); `clear(tag)` only matching; `kModExclusive` suppresses a lower-priority same-kind modifier; ids unique and ordered; motion-gated kinds contribute 0 when `motionAllowed` is false while their envelopes still end on schedule; Punch returns exactly 0 after its out phase; the stack cap refuses the 33rd push.
- `CameraRigSystemTest` (real Jolt world through the physics ABI, scene `World`, no GPU, no framework): 
  - Registration: `sizeof(CCameraRig) == 160`, OUT fields read-only, zero-filled storage evaluates as a working Chase rig, `attach` adds `CCamera` with near 2 / far 200000 / 60 degrees, `kRigApplyPreset` expands then clears.
  - **Spring arm vs Jolt:** a static box wall 200 cm behind the pivot: the arm settles to `200 - probeRadius` within 0.5 cm and the camera never passes the wall; a **sensor** body in the path is ignored; the target's **character** and `ignoreBody` are ignored; a body on the cloth layer (14) is ignored by the default mask but hit when `collisionMask` includes it; removing the wall recovers the arm with the configured half-life (within 5% at one half-life); a pivot inside geometry yields `minArmCm`, not zero.
  - **Lag convergence in the system:** a kinematic target stepped 400 cm: the camera follows with `posLagSpeed` and `lagMaxDistCm` bounding the gap every frame; a 3000 cm teleport sets `RigResult::cut` and snaps with no trail.
  - **Modes:** FirstPerson is exactly at the view node and never lags; Seat follows the parent transform and clamps head look to the seat limits; TopDown holds its fixed rotation while the target moves; Fixed with `kRigLookAtTarget` converges on the target; Orbit rotates only through `AddLook` and `recenterDelayS` returns it behind the target.
  - **Selection and blend:** possession picks the best `CCamera.priority` rig under the pawn; an explicit `setViewTarget` overrides it until cleared; at half the blend time `alpha == 0.5` for `EaseInOut`; at the end the result is bit-equal to the live pose; a mid-blend switch is continuous (no pose jump larger than one frame of motion); `setViewTarget(kInvalidEntity)` blends to the fallback pose; a destroyed active target holds 0.5 s then blends out; `CameraViewTargetChanged`, `CameraBlendFinished`, `CameraCut` are raised once each in order.
  - **Collision fade:** `ownerHideRoot`/`hideAll` set when the arm is squashed under `fadeEndCm`, cleared with hysteresis; `kRigNoFade` disables it.
  - **Reduce motion:** with `motionAllowed = false`: no look-ahead, no FOV kick, no punch, position lag floored at 40, bob sink receives `(0, 0)`; with it true the same scenario differs.
  - **External pose:** while `externalActive`, `drive` reports inactive; when it ends the blend starts from the last external pose over `externalBlendS`.
  - **No rig, no cost:** an empty world returns inactive and touches nothing; the legacy fallback is returned untouched.
  - **Replay equality:** two fresh systems run the same 600-frame script (moves, mode change, view-target switch, modifiers, shake) and produce bit-identical poses, events and modifier ids.
  - Non-finite target transform: output discarded, previous pose kept, logged once.

**`tests/physics/src/SphereCastQTest.cpp`:** distance to a box face equals the analytic value minus the radius; normal points back toward the sphere; sensors skipped by default and hit with `AVER_PHYS_QUERY_INCLUDE_SENSORS`; both ignore slots honoured, including a character handle; layer mask honoured; zero displacement and `radius <= 0` return 0; a start-overlapping sphere reports distance 0; the old `aver_phys_sphere_cast_ex` still sees the sensor (the reason this function exists).

**`tests/framework_camera/src/FrameworkCameraAbiTest.cpp`:** version; no provider returns 0 and writes nothing; a fake provider receives each op with the documented argument packing and its outputs round-trip; `aver_fw_camera_pose` fills nine floats and two ints.

**`tests/game/src/GameCameraTest.cpp` (`Aver.Runtime.Game.Core`, `Aver.Scene`, `Aver.Core`; no device):**
- `pushCamera(…, PlayView{})` returns matrices **bit-equal** to the four-argument overload for a grid of poses (the default is today's camera); FOV 90 changes `m[1][1]` by the expected `1/tan(fov/2)` ratio; roll 0 builds the same up vector, roll 30 degrees rotates it about `forward`.
- `driveLegacyPlayCamera` is unchanged: the existing behaviours (first person, boom, zero boom) reproduce; with no rig, `drivePlayCamera(…, PlayView&, dt)` equals the legacy result bit for bit and leaves `PlayView` default.
- With a rig under the possessed pawn the PlayView carries the rig's FOV, roll and `ownerHideAll`; first-person owner hide still returns the pawn with `ownerHideAll == false`.
- The existing `GameAccessibilityTest` camera-fx cases (`:165-166`) stay green unchanged.

**`tests/game/src/CameraRigRecordTest.cpp`:** `COMPONENT CameraRig` parse, apply and capture round trip with golden canonical strings (preset written, preset-equal keys omitted, flags as `0/1`); an unknown key survives; `target=parent`; a hand-written record with only `preset=seat`.

**`tests/editor/src/CameraRigDetailsModelTest.cpp`:** preset application writes the right fields, `canAttach` rules (mesh required, already present), mask checklist, the near-plane warning threshold.

**C# (`Aver.Graph.Tests/CameraNodeTests.cs`, `[AutoRun]`, `NodeRowKit`):** `CheckRows` (a real method behind every one of the 23 rows, types match pins), `CheckDefaultPins` (order and defaults, including `blendSeconds=0.5`, `curve=0`), `CheckIlValid`, `CheckCompilerSplit` (exec rows refused by the pull compiler, the three pure rows compile under both), the row set equals the designed 23, enums mirror the C header (`CameraMode`, `CameraRigPreset`, `CameraBlendCurve`, `CameraModifierKind`, flags), behaviour checks treat an unavailable scene DLL as a skip (`RenderPropsNodeTests` precedent).

**`AbiEnumTest`:** `AVER_FW_CAM_*` constants against the C# enums; the physics row (which already names `physics_queries_abi.h`) checks `aver_phys_sphere_cast_q` against `Native.Queries.cs` including parameter names.

---

## 6. Headed manual verification (the owner verifies; GPU idle; 1440p screenshots with a ping on remote runs)

Use `test-content/CameraLab`. Editor first, then the packaged runtime, then `--backend vulkan` for steps 1, 3, 7, 9.

1. **Parity first.** A level with no rig, Play: the third-person and first-person views look and move exactly as before (compare a screenshot against a pre-change build at the same `--frames` count).
2. **Chase + collision.** Add Component > Camera Rig (Chase) to the pawn, Play, back into a wall: the camera slides in smoothly, never shows the inside of the wall, never stops on the trigger volume; walk away: it eases back out in about a quarter of a second. The Details readout shows the arm shrinking.
3. **Collision fade.** Squeeze the arm to the minimum in the low-ceiling corridor: the character's body disappears (shadow stays), and returns without flicker when you back out. Toggle `kRigNoFade`: the camera sits inside the body as before.
4. **Lag.** Position lag 12 and rotation lag 8 on a Chase rig: the camera trails a sprint and settles; set max distance 80 and sprint again: the gap never exceeds it. Run at 30 and 144 fps (cap the frame rate): the trailing distance looks the same.
5. **Look-ahead and FOV kick (VehicleChase on the kinematic cart).** The camera leads the cart; speed widens the FOV and relaxes with no pop.
6. **Modes.** Cycle all six on one pawn (`SetCameraRigMode` from a graph on a key): each blends over its blend time; FirstPerson hides the flagged body; Orbit turns with the mouse (`DriveOrbitFromMouse`) and recentres; TopDown holds its angle; Seat rides the cart with a limited head look; Fixed with look-at turns to follow the pawn.
7. **View-target blend.** Two rigs, a trigger switches `SetViewTarget` between them with 0.8 s: smooth, no snap; switch back mid-blend: continuous; `cut` snaps. Streaming-heavy level: no stall on a cut.
8. **Modifiers.** Push FOV kick, zoom (aim), punch, roll lean, arm scale, socket offset from graph nodes: each ramps in, holds, ramps out; removing one mid-hold ramps from where it is.
9. **Shake and reduce motion.** `ShakeAt` an explosion near and far: shake falls off with distance. Turn Reduce motion on: shake, bob, FOV kick, punch, lean, look-ahead and auto-recenter stop, lag turns stiff; the camera still follows, collides and blends. Turn it off: all return, sliders unchanged.
10. **Sequencer handoff.** Play a level sequence with a camera track: it owns the view; when it ends the camera blends back into the rig over 0.5 s.
11. **Possession.** Possess a second pawn that carries its own rig: the view blends to that rig automatically; unpossess: back.
12. **Editor.** Add Component on a placed mesh, change every preset in the combo (values change, one undo step); **Look through** pilots the viewport; the overlay shows pivot, lagged pivot, arm, probe sphere, hit point, fade radius; save and reload the level: the record round-trips and runtime state is not saved.
13. **Aver Node.** A graph with `AN_CameraSetViewTarget`, `AN_CameraPunch`, `AN_CameraGetPose` and an `AN_OnEvent` for `CameraBlendFinished`: all fire as documented; `AN_CameraGetPose` agrees with what is on screen.
14. **Both backends.** Steps 2, 3, 5, 7 on D3D12 and `--backend vulkan`: identical framing, no shimmer or ghosting beyond what a camera of that speed already shows in the temporal passes, terrain LOD and cluster LOD stable across an FOV kick (the `projScale` sites).
15. **Cost.** `--gpu-timing` / the play profiler: the camera tick is below 0.05 ms CPU with a rig active, and a no-rig project shows no change.

---

## 7. Open questions (defaults chosen; proceed unless told otherwise)

1. **Orthographic and isometric.** Not in v1: `CameraFactor::factor` refuses an orthographic matrix (`CameraFactor.hpp:26-44`) and the denoiser and temporal passes depend on it. Default: `TopDown` with a narrow FOV (about 20 degrees) and a long arm approximates isometric; a true orthographic projection is a renderer feature of its own.
2. **Collision fade is binary.** No per-instance opacity or dither exists. Default: hide with hysteresis. A dithered fade needs a shader change on both backends and a material path; a follow-up.
3. **Shake patterns.** v1 shake is the existing global trauma `CameraFx`; Unreal-style shake pattern assets (oscillator per axis, `.ocshake`) are deferred. Default: global trauma, `ShakeAt` for distance falloff.
4. **Mesh-less rigs in level files.** `canAttach` requires a mesh placement. Default: unchanged; a fixed camera is a placed prop mesh or a graph/C# spawned rig. A `Camera` placement record would be a format change.
5. **Split-screen.** One local player, as `aver_fw_view` assumes. Default: single view target; the system holds one view-target state.
6. **Sequencer blend unification.** The sequencer keeps its own pairwise camera blend (`SequenceTracks.cpp:113`); the rig adds the external-pose hand-back only. Default: leave the sequencer alone. Moving it onto `blendPov` would add roll and FOV tracks and is a separate change.
7. **Which `dt`.** The host's frame dt (what `applyCameraFx` gets), not the fixed physics step and not time-dilated. Default: slow motion does not slow the camera. A `useGameTime` flag is a one-line follow-up.
8. **Look-ahead plane.** Planar only, so jumps and ramps do not bob the camera. Default: planar.
9. **Ignoring a rigid target automatically.** There is no entity-to-rigid-body lookup (`BodyRegistry` is host-private). Default: an explicit `ignoreBody` field plus the target's character; VEHICLES sets the chassis. Sharing `BodyRegistry` through the physics ABI is a separate decision.
10. **Automatic selection without possession.** Rigs are chosen by possession or explicitly. Default: no "highest `CCamera.priority` anywhere in the level" rule, to avoid a stray prop camera taking the view.
11. **Legacy `Character` fields.** `CameraViewMode`, `EyeHeight`, `BoomLength` keep working with no rig and switch FirstPerson/third person with one. Default: retained, documented, no deprecation yet.
12. **FOV kick gate.** Gated by Reduce motion only, not by the shake slider. Default: as stated; the slider scales trauma shake, which is what it is labelled for.
13. **Cloth and proxy layers.** The default probe mask excludes layers 14 and 15 only when the soft-body module is present (read from its options); otherwise all layers. Default: as stated; a project that repurposes those layers sets `collisionMask`.
