# Aver.Camera  (`modules/camera`)

- **Language:** C++
- **Depends on:** Core, Scene, Physics
- **Option:** `AVER_MODULE_CAMERA` (ON)
- **Status:** written against the frozen design; not yet compiled or run

Gameplay cameras on scene entities: the `CCameraRig` component (spring arm, camera mode and blend source in one), the per-frame driver that
picks the live rig, probes with a swept sphere (`aver_phys_sphere_cast_q`), blends view targets and applies the modifier stack, and the
`aver_fw_camera_*` framework provider. User guide: **docs/CAMERAS.md**.

| File | What |
|---|---|
| `CameraTypes.hpp` | Modes, presets, flag bits, `CCameraRig` (160 bytes), `CameraPov` |
| `CameraRigMath.hpp` | Pure math: exponential follow, lag with sub-steps, angle follow, the yaw/pitch/roll basis, pose blends, look-ahead, spring-arm resolve, fade hysteresis, FOV composition |
| `CameraModifiers.hpp` | The modifier stack: envelopes, tags, exclusivity, the Reduce-motion gate, the 32-entry cap |
| `CameraRigPresets.hpp` | The seven presets, and the "0 reads the default" resolution of the tuning fields |
| `CameraRigSystem.hpp` | The public API: `CameraRigSystem`, `RigFrame`, `RigResult`, `cameraRigSystem()`, `cameraRigProvider` |
| `CameraRigSystem.cpp` | Registration, attach, the inbox, selection, view-target blends, events, `drive` |
| `CameraRigEval.cpp` | Evaluating one rig: pivot and rotation by mode, look-ahead, lag, the probe, the collision fade, FOV; `preview` |
| `CameraRigProvider.cpp` | The framework relay provider |

The headers in the first four rows are Core-only and testable anywhere. `drive` runs once per frame, after `World::flush` and before the
sequencer camera and `applyCameraFx`; with no rig, modifier or blend it returns `active = false` and the host's legacy boom pose stands bit for bit.
Nothing here draws. Tests: `tests/camera` (`CameraRigMathTest`, `CameraModifiersTest`, `CameraRigSystemTest` against a real Jolt world),
`tests/framework_camera`, `tests/game` (`GameCameraTest`, `CameraRigRecordTest`), `tests/editor` (`CameraRigDetailsModelTest`).
