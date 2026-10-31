# Physics interpolation

The simulation steps at a fixed rate and the screen redraws at another. With interpolation on, what is drawn is the state between the last two fixed steps, so motion is smooth at any frame rate. Gameplay never sees the blend: it reads the real pose of the last step.

## What blends

- **Dynamic rigid bodies**, when the switch is on. `PHYSICS.INTERPOLATION 1` in the project turns it on; a script can override it at run time (`PoseInterpolation.Enabled`, `null` returns to the project's value).
- **Any other entity a script lists.** `PoseInterpolation.SetMode(entity, On)` reads that entity's world pose around every fixed step, whatever moved it in the physics tick (a character controller, a platform, an animated mover). `Off` keeps an entity out, a rigid body included. `Inherit` (the default for every entity) follows the switch, which for anything but a dynamic body means "not blended".
- A child follows its parent. Only the top-most listed entity is blended; a child set to `Off` under a blended parent still moves with it.

## The frame

`BodyPoseSync` (Runtime, header only) runs inside `tickGameplayGroups`:

1. **restore**: every entity the last frame drew blended goes back to its real pose.
2. For each fixed step: the PHYSICS pass, then the step. From the second step of a frame on, **capture** records each dynamic body's pose (and writes it to its entity) and each listed entity's world pose.
3. After the last step, **capture** again.
4. The PROCESS pass.
5. **present**: each blended entity is placed between its previous and its current pose by the step alpha, and the pose drawn is published (`aver_fw_interp_pose`, `PoseInterpolation.TryGetDrawnPose`).

A listed entity must be moved in the physics tick. A move made in the PROCESS pass is overwritten by the blend. `PoseInterpolation.Reset(entity)` says the next pose is a teleport: the entity is drawn there at once with no blend from where it was. The reset is seen even in a frame that ran no step.

## Cost

Capture walks the entities for rigid bodies (as it did before) and reads the world matrix of each listed entity. Nothing else costs unless an entity is listed.

## Aver.Fabric

`Node.physics_interpolation_mode`, `SceneTree.physics_interpolation`, `reset_physics_interpolation()` and `Node3D.get_global_transform_interpolated()` sit on this. A node is listed when it is interpolated in Godot's terms and is a CharacterBody3D or AnimatableBody3D, or when its own mode is On; every other node follows the node above it. See Aver.Fabric `Nodes/PhysicsInterpolation.cs`.

## Tests

`tests/game/src/GameTickOrderTest.cpp`: a body drawn between steps, mode OFF, the script's switch over the project's, a listed mover blended, taken off, reset, and its drawn pose read. Nothing here has been seen on a screen: the headed check is a character moving at a frame rate that is not a multiple of the step.
