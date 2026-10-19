# CameraLab

Engine test content for the gameplay cameras (`docs/CAMERAS.md`). Cubes and ramps only, no third-party assets: every mesh is the built-in
`Meshes/cube.ocmesh`. Open the project, open `Maps/CameraLab.ocworld`, press Play. Nothing needs importing.

| Where | What | Checks |
|---|---|---|
| Spawn, facing +X | A wall 200 cm behind you, four pillars | Walk backwards into the wall: the Chase arm slides in smoothly and eases back out. Orbit (key 2) around a pillar |
| x = 600..2200 | A corridor 540 cm wide, 300 cm tall | The camera never shows the inside of a wall; it does not stop on the doorway frame |
| x = 1000 | A doorway, 120 cm wide, 220 cm tall | Walk through it with the camera behind you |
| x = 1500..2100 | A low ceiling (underside at 230 cm) | Look up under it: the arm squashes against the pivot, the body hides (shadow stays) and returns without flicker |
| x = 2500..3300 | A 15 degree slope | The probe follows the ground |
| (1000, -1500) | A cart driving back and forth along X | Press E: the view blends into its Seat rig over 0.6 s and rides it; Q gives the view back |

The pawn is `AN_LabPawn` (a Character with a rig added at start). Keys:

| Key | Does |
|---|---|
| 1 2 3 4 5 6 | Rig mode: Chase, Orbit, First person, Seat, Top down, Fixed (each blends over the rig's blend time) |
| C | Cut: snap the lag state |
| F | FOV kick (8 degrees) |
| Z | Zoom modifier (held until X) |
| G | Punch (recoil) |
| H | Shake at the pawn (falls off with distance from the camera) |
| X | Clear every modifier |
| E / Q | Make the cart's seat the view / return to the pawn |

Add lag, look-ahead and the other tuning in the Graph Editor with the `Camera` nodes (`AN_CameraSetLag`, `AN_CameraSetLookAhead`, ...) or in C#
(`Entity.SetCameraRigLag`, ...). A trigger volume in the camera's path (a sensor body) must not stop the camera; the engine's sensor skip is
pinned by `SphereCastQTest`, and a sensor is added from a script (`Shapes.AddSensorBox`), not from a level record. Reduce motion (Accessibility)
stops the kick, punch, look-ahead and recentre and stiffens the lag; zoom still works.
