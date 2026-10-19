# Gameplay cameras

A **camera rig** is one component, `CCameraRig`, that is the spring arm, the camera mode and the blend source in one. It follows a target, keeps the camera out of walls with a swept sphere, can lag and lead, blends when the view changes, and takes camera modifiers (FOV kick, zoom, recoil, lean). A project with **no rig** renders exactly as before: the fixed boom (`AverCharacter.BoomLength`, `EyeHeight`, `CameraViewMode`) runs untouched.

Module `Aver.Camera` (`modules/camera`), option `AVER_MODULE_CAMERA`. Design contract: `docs/plans/wave10/GAMEPLAY_CAMERAS.md`. Test content: `test-content/CameraLab`.

Units are the engine's: centimetres, +X forward, +Y right, +Z up. Angles are degrees at every authored or scripted boundary.

## Which rig is the view

1. An explicit `CameraRig.SetViewTarget(rig)` wins until `ClearViewTarget`. `SetViewTarget(Entity.None)` returns to the legacy pawn camera.
2. Otherwise the best rig under the possessed pawn: on the pawn, on a descendant, or with `target` set to the pawn. Best is the highest `CCamera.priority`, ties by the lowest entity id.
3. Otherwise none, and the legacy boom runs.

There is no "highest priority anywhere in the level" rule: a stray prop camera never takes the view.

A change of view (a different rig, a rig's `mode` field, a sequencer camera letting go, back to the legacy boom) **blends**: the last output pose is frozen and blended into the live pose of the new source over the incoming rig's `blendTime` with its curve. `SetViewTarget(..., blendSeconds, curve, cut)` overrides both; `cut = true` or a zero time snaps and raises `CameraCut`. A change mid-blend starts from the current blended pose, so a transition is always continuous. Hosts reset streaming's velocity tracking on a cut.

## Modes

| Mode | Pivot | Rotation | Arm and probe | Lag | Use |
|---|---|---|---|---|---|
| Chase | The target's view node when the target is the possessed pawn (its head), else the target's origin; plus `pivot` | The source rotation (the view node's, else the target's heading). A no-inherit axis takes `yaw`/`pitch` | yes | position, rotation, look-ahead | Third-person character, vehicle chase |
| Orbit | Target origin + `pivot` | The rig's own `yaw`/`pitch`, driven by look input; optional auto-recentre | yes | position | Inspection, strategy |
| FirstPerson | The view node (else target + `pivot`) | The view node's | none | **forced off** | First person; the flagged body hides |
| Seat | The **rig entity's own transform** (a child of the vehicle) | That transform + a head look clamped to the seat limits | none | position (small, damps vibration) | In-car, cockpit |
| TopDown | Target origin + `pivot` + look-ahead | Fixed `yaw`/`pitch` (default -62 down) | yes, off by default | position | Top-down, ARPG |
| Fixed | The rig entity's own transform | That rotation, or a smoothed look at the target with `LookAtTarget` | none | rotation (when looking at) | Security camera, fixed shot |

Not in v1, said plainly: orthographic or isometric projection (use TopDown with a narrow FOV; the temporal passes refuse an orthographic matrix), split-screen (one local player), per-camera post-process blend, a history reset on a cut in the temporal passes (the sequencer has none either), pattern-asset shakes (`.ocshake`), a dithered collision fade (the fade is a binary hide), and mesh-less rig placements in level files.

## Presets

Seven presets seed the tuning fields; the fields stay editable. `Entity.AddCameraRig(preset)`, the editor's preset combo, the level record and `ClassBuilder.CameraRig(preset)` all go through one table (`CameraRigPresets.hpp`).

| Preset | Mode | Arm | Pivot / socket | Lag | Other |
|---|---|---|---|---|---|
| Chase | Chase | 450, probe 12 | none | none | Blend 0.4 s, fade 140/70. Today's boom plus collision |
| VehicleChase | Chase | 600, probe 14 | pivot (0,0,120), socket (0,0,60) | position 8 (max 150), rotation 5 | Look-ahead 0.25 s / 250 cm, FOV kick 10 degrees at 3000 cm/s, own pitch -12, no fade, blend 0.8 s |
| Orbit | Orbit | 500, probe 12 | none | position 12 | Pitch limits -75/75, pitch -20, blend 0.5 s |
| FirstPerson | FirstPerson | none | none | off | Pitch limits -85/85, head bob 0.5, blend 0.25 s |
| Seat | Seat | none | none | position 25 (max 20) | Head limits 110/50, FOV kick 6 degrees at 3000 cm/s, blend 0.6 s |
| TopDown | TopDown | 1400 | none | position 6 | Look-ahead 0.4 s / 250 cm, pitch -62, inherits nothing, no collision, blend 0.7 s |
| Fixed | Fixed | none | none | off | Blend 0.5 s |

## The component

`CCameraRig`, 160 bytes. **A 0 in a numeric tuning field reads as its default**; zero-filled storage is a working Chase rig. Flags are negative sense (`NoCollision`, `NoInheritYaw/Pitch/Roll`, `NoFade`) so zero is the common case.

| Field | 0 reads | Meaning |
|---|---|---|
| `flags` | | `NoCollision`, `NoInheritYaw/Pitch/Roll`, `NoFade`, `LookAtTarget` (Fixed), `Disabled` (never selected) |
| `mode`, `preset` | Chase | The mode (changing it at run time blends); the preset the editor and the record start from |
| `target` | the parent, else the rig's own entity | The entity the arm follows |
| `ignoreBody` | | A physics body the probe skips (a vehicle chassis). Process-local, **never saved**. The target's character is skipped automatically |
| `armLengthCm`, `minArmCm` | 450, 20 | Target arm length; the closest the camera may come to the pivot |
| `probeRadiusCm` | 12 | Must exceed the near-plane corner radius, about 2.3 cm at the defaults |
| `collisionMask` | every layer except cloth (14) and proxy (15) | Physics layers the probe stops on |
| `recoverHalfLifeS` | 0.25 | How fast the arm returns outward after a hit. Inward is instant |
| `socketX/Y/Z` | | Offset at the arm's end in the **camera frame** (forward, right, up) |
| `pivotX/Y/Z` | | Offset of the arm origin in **world** axes (eye height) |
| `posLagSpeed`, `lagMaxDistCm`, `lagMaxStepS` | off, unlimited, 1/60 | Exponential follow of the pivot; the farthest it may trail; sub-step length |
| `rotLagSpeed`, `rotLagMaxDeg` | off, unlimited | Rotation lag |
| `lookAheadS`, `lookAheadMaxCm` | off, 150 | The pivot leads by planar velocity times this |
| `pitchMinDeg`, `pitchMaxDeg` | -80, +80 | View pitch limits (always within +-89) |
| `yawDeg`, `pitchDeg` | | The rig's own rotation: Orbit/TopDown, a no-inherit axis, the Seat head look |
| `recenterDelayS` | off | Orbit or own-yaw Chase eases behind the target after this long without look input |
| `blendTimeS`, `blendCurve` | cut, EaseInOut | Blend **into** this rig/mode; `EaseInOut` (smoothstep), `Linear`, `EaseIn`, `EaseOut` |
| `fovKickDeg`, `fovKickSpeedCmS` | off, 1500 | FOV added up to this speed |
| `fadeStartCm`, `fadeEndCm` | 140, 70 | The camera nearer than `fadeEnd` to the pivot hides the target's meshes; they return at 1.15 times that |
| `bobAmount` | off | FirstPerson/Seat head bob through `CameraFx.SetBob` |
| `seatYawLimitDeg`, `seatPitchLimitDeg` | 110, 50 | Head-look range in Seat |
| `outArmCm`, `outState`, `outFovDeg` | | OUT, read-only: the arm after collision, state bits (active, blending, colliding, faded, lagging), the final FOV |

`CCamera` (FOV, near, far, priority) is now read: by the rig that carries it. `Entity.AddCameraRig` adds one with near 2, far 200000 and 60 degrees, today's constants.

## The spring arm

The probe is a swept sphere (`aver_phys_sphere_cast_q`, the only physics change: sensors are skipped unless asked, two ignore bodies, a distance out) along the segment from the pivot to the desired end, **including the socket offset**. The camera sits at the hit and the arm snaps inward and recovers outward with the half-life. A probe that starts overlapping gives `minArmCm`, never zero. Trigger volumes, the target's character and `ignoreBody` do not stop it.

Position lag is the exact exponential `1 - exp(-speed * dt)`, frame-rate independent, integrated in sub-steps no longer than `lagMaxStepS`. (Unreal's is a linear interpolation.)

**Collision fade** is a binary hide with hysteresis: the target's meshes stop drawing in the raster pass (shadows, GI and RT geometry stay, the first-person owner-hide route) while the camera is closer than `fadeEnd` to the pivot. The engine has no per-instance opacity; a dithered fade is a follow-up.

## Modifiers

A modifier has a kind, a priority, an in / hold / out envelope (smoothstep; `hold < 0` means until removed), a tag and optional exclusivity (a full-strength exclusive modifier silences lower-priority ones of its kind). The stack holds 32 and is evaluated once per frame, after the blend, so a recoil does not fight a transition.

| Kind | Effect | Gated by Reduce motion |
|---|---|---|
| FovKick | degrees added to the FOV | yes |
| FovScale | FOV multiplied (0.6 zooms in) | no: the player's own action |
| Punch | pitch, yaw, roll rotation offset | yes |
| RollLean | roll in degrees | yes |
| ArmScale | arm length multiplied | no |
| SocketOffset | offset at the arm's end | no |

Removing a modifier blends it out from its **current** strength; there is no jump.

**Shake** is not a second system. `CameraRig.AddShake(trauma)` is the existing `CameraFx.AddShake`; `CameraRig.ShakeAt(pos, radius, intensity, falloff)` computes `intensity * (1 - d/radius)^falloff` against the current camera and adds that trauma, so the player's shake option and Reduce motion apply once, in `CameraFx`.

## Scripting (C#)

```csharp
// A rig on a vehicle's chase node, a seat rig inside it
Entity chase = Entity.Create("VehicleCam");
chase.SetParent(car);
chase.AddCameraRig(CameraRigPreset.VehicleChase, car);
chase.SetCameraRigIgnoreBody(chassis.Handle);

Entity seat = Entity.Create("SeatCam");
seat.SetParent(car);
seat.AddCameraRig(CameraRigPreset.Seat, car);
CameraRig.SetViewTarget(seat, 0.6f);          // entering the car; SetCameraRigMode / another SetViewTarget changes the camera

CameraRig.PushFovKick(8f);                    // returns an id for RemoveModifier
int aim = CameraRig.PushFovScale(0.6f);       // held until removed
CameraRig.RemoveModifier(aim);
CameraRig.ShakeAt(explosionPosition, 1500f, 0.8f);
```

`AverCharacter.AttachCameraRig(preset)` puts a rig on a character; `CameraViewMode` then switches it between first person and the preset's third-person mode and `BoomLength` is ignored. `ClassBuilder.CameraRig(preset)` adds one to a class. Entity members: `AddCameraRig`, `HasCameraRig`, `RemoveCameraRig` (disables it: the component stays), `SetCameraRigMode`, `SetCameraRigTarget`, `SetCameraRigArm`, `SetCameraRigSocket`, `SetCameraRigPivot`, `SetCameraRigLag`, `SetCameraRigLookAhead`, `SetCameraRigPitchLimits`, `AddCameraRigLook`, `SetCameraRigFade`, `SetCameraRigBlend`, `SetCameraRigIgnoreBody`, `SetCameraRigFlags`, `TryGetCameraRigState`. Static `CameraRig`: `SetViewTarget`, `ClearViewTarget`, `Cut`, `ActiveRig`, `IsBlending`, `TryGetPose`, the `Push*` modifiers, `RemoveModifier`, `ClearModifiers`, `AddShake`, `ShakeAt`, `DriveOrbitFromMouse`. Every call answers false or 0 and never throws in a process with no engine or a build without the module.

The camera events `CameraViewTargetChanged`, `CameraBlendFinished` and `CameraCut` reach a graph through `AN_OnEvent`.

## Aver Node family `Camera` (23 nodes)

Exec: `AN_CameraAddRig`, `AN_CameraSetMode`, `AN_CameraSetTarget`, `AN_CameraSetViewTarget`, `AN_CameraClearViewTarget`, `AN_CameraCut`, `AN_CameraSetArm`, `AN_CameraSetSocket`, `AN_CameraSetPivot`, `AN_CameraSetLag`, `AN_CameraSetLookAhead`, `AN_CameraSetLimits`, `AN_CameraAddLook`, `AN_CameraSetFade`, `AN_CameraPushFovKick`, `AN_CameraPushZoom`, `AN_CameraPunch`, `AN_CameraRemoveModifier`, `AN_CameraClearModifiers`, `AN_CameraShakeAt`. Pure: `AN_CameraGetPose`, `AN_CameraGetRigState`, `AN_CameraGetViewTarget`. Enums cross as ints (mode 0 Chase ... 5 Fixed; preset 0 Chase ... 6 Fixed; curve 0 EaseInOut, 1 Linear, 2 EaseIn, 3 EaseOut). A graph class assembles a rig with a `COMP` of kind `CameraRig` (`preset=`, `mode=`, `arm=`, `target=parent|self`). The generated page is `docs/nodes/Camera.md`.

## Level records

```
PLACE Meshes/Car.ocmesh 0 0 0 0 0 0 1 name Car
  COMPONENT CameraRig preset=vehiclechase arm=700 socket=0,0,80
```

`preset` is always written; every other key only when it differs from the preset's value. Keys, in canonical order: `preset`, `mode`, `target` (`self` default, `parent`), `arm`, `minarm`, `probe`, `recover`, `socket`, `pivot`, `lag`, `lagmax`, `rotlag`, `rotlagmax`, `lagstep`, `ahead`, `aheadmax`, `pitchmin`, `pitchmax`, `yaw`, `pitch`, `recenter`, `blend`, `curve`, `fovkick`, `fovkickspeed`, `fadestart`, `fadeend`, `bob`, `seatyaw`, `seatpitch`, `mask` (hex), then the flags as `0`/`1`: `collision`, `inheritpitch`, `inherityaw`, `inheritroll`, `fade` (default 1) and `lookat`, `disabled` (default 0). `ignoreBody` and the OUT fields are never saved. A loaded level starts with no explicit view target.

## Editor

Add Component > Camera Rig on a placed mesh (a class-based pawn gets its rig from code or a graph `COMP`). The Details section has the preset combo (one undo step), the mode, target, the Arm, Offsets, Lag, Look-ahead, Rotation, Blend and Feel groups, a layer-mask checklist, a warning when the probe is smaller than the near-plane corner, and in Play a read-only line of the OUT fields. **Look through** pilots the viewport with the rig's pose until Esc; **Make live**, **Cut**, **Test shake** and **Test FOV kick** act on the running camera. `Show > Camera Rigs` draws the pivot and lagged pivot, the arm (green clear, orange shortened), the probe sphere and hit, the look-ahead, the fade radius and the pitch-limit arc.

## Accessibility

While Reduce motion is on, look-ahead, FOV kick, punch, roll lean and auto-recentre stop, and the position and rotation lag are raised to a stiff speed (40) instead of swaying; the camera still follows, collides and blends. Zoom, arm scale and socket offset are the player's own actions and stay. Shake and bob keep going through `CameraFx`, whose own gate is unchanged. See `docs/ACCESSIBILITY.md`.

## Unreal mapping

| Unreal | Here |
|---|---|
| `USpringArmComponent` | `CCameraRig` (Chase, Orbit, TopDown) |
| `TargetArmLength` | `armLengthCm` |
| `SocketOffset` / `TargetOffset` | `socket*` (camera frame) / `pivot*` (world) |
| `bDoCollisionTest`, `ProbeSize`, `ProbeChannel` | `NoCollision` (negated), `probeRadiusCm`, `collisionMask` |
| `AddIgnoredActor` | `ignoreBody` and the target's character |
| `bEnableCameraLag`, `CameraLagSpeed`, `CameraLagMaxDistance`, `CameraLagMaxTimeStep` | `posLagSpeed`, `lagMaxDistCm`, `lagMaxStepS` (exact exponential instead of linear) |
| `bEnableCameraRotationLag`, `CameraRotationLagSpeed` | `rotLagSpeed`, `rotLagMaxDeg` |
| `bInheritPitch/Yaw/Roll` | `NoInheritPitch/Yaw/Roll` (negated) |
| `SetViewTargetWithBlend`, `BlendTime`, `BlendFunction` | `CameraRig.SetViewTarget`, `blendTimeS`, `blendCurve` (snapshot blend of the outgoing pose) |
| `UCameraModifier` | the modifier stack |
| `CameraShake` | `CameraFx` trauma through `AddShake` / `ShakeAt` |
| `ViewPitchMin/Max` | `pitchMinDeg`, `pitchMaxDeg` |

Differences, stated: the lag is exponential rather than linear, the probe includes the socket offset, the arm snaps inward and recovers outward with a half-life, and the outgoing view target is a frozen snapshot rather than a live camera.

## Determinism, failure and budgets

Inputs are the component fields, world matrices after `World::flush`, the Jolt query results, the host's frame `dt` and the ordered inbox: no wall clock, no `rand`; a replay of the same script gives bit-identical poses, events and modifier ids (cross-platform float identity is not claimed). A missing component, a dead entity, no physics world or no provider returns false/0 and the camera falls back to the legacy pose. A rig whose target is destroyed holds its last pose for 0.5 s, then blends to the fallback. A non-finite pose is discarded and the previous one kept. The cost is one sphere cast and a few vector operations per frame; the modifier stack is capped at 32.
