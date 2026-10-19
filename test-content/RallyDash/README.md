# RallyDash

A small lap game made **only from content**: one level, four Aver Node graphs, an input scheme, a HUD layout, a vehicle, particle effects, materials and procedural meshes and tones. No C#, no engine change. Everything is written by `make_rally_dash.py`.

> Status: written without running it. The graphs are checked against `docs/nodes/*.md` (every node type, pin and attribute) and their logic was run through a stand-in interpreter for three laps, but nothing here has been loaded by the editor or played. Expect a first-run tweak or two.

## Open it

1. Start the editor, **File > Open Project**, choose `test-content/RallyDash/Game.ocproject`.
2. The start map is `Maps/RallyDash.ocworld`. Press **Play**.
3. A 3 - 2 - 1 countdown runs with the car held at the start grid; on GO the pawn gets in and you drive.

## Controls

| Input | Does |
|---|---|
| W / Up, S / Down | Throttle, brake (and reverse from a standstill) |
| A D / Left Right | Steer |
| Space | Handbrake |
| R | Respawn at the last checkpoint, upright and repaired |
| C | Next camera (chase, hood, cockpit) |
| Gamepad | Right / left trigger gas / brake, left stick steer, A handbrake, Y respawn, right stick click camera |

## What to expect

- A 650 m loop of 16 m wide asphalt (a bean shape: long rights, two lefts, one tight kink), red and white kerbs, concrete barriers on both sides, cones on the kerbs, lamp posts, a pit building, buildings, billboards, trees and a ring of rocks on the horizon, under a physical sky.
- **3 laps through 8 checkpoints.** Gate 0 is the chequered start/finish line under the gold arch; the cyan arches are checkpoints 1 to 7. Gates must be passed in order; passing the line with all of them behind you completes a lap.
- **HUD:** speed (km/h, with a bar), gear, lap n/3, current lap time, best lap, a CHECKPOINT flash, a WRECKED - PRESS R warning when the car is destroyed, health bar, a big 3-2-1-GO! countdown and a FINISH panel with the total time and best lap. Beeps for the countdown, chimes for checkpoints, a jingle per lap and at the finish; a golden spark burst at the end of each lap.
- **Juice that comes from the vehicle spec:** engine note from RPM, tyre smoke and a skid sound when the tyres slip, sparks and a crash sound on hard hits, camera shake on a crash (graph) and a small punch on landing after a jump, FOV kick with speed.
- **Two kickers** on the left half of the road (10 degrees, about 9 m long) to jump off.
- **Things to smash** on the left half: two pyramids of wooden `Destructible` crates and a glass pane.
- **Two slow traffic cars** (blue and yellow) follow the right-hand lane of `Maps/RallyDash.oclanes` at 50 km/h. They are obstacles, not opponents.

## What is where

| File | What it is |
|---|---|
| `Game.ocproject` | The project: start map, input scheme, game mode `AN_RallyRules` |
| `Content/Maps/RallyDash.ocworld` | The level (about 1100 placements). The player's car is `COMPONENT Vehicle` plus a chase `COMPONENT CameraRig`; the race is a placed graph class |
| `Content/Maps/RallyDash.oclanes` | The traffic lanes (14 lanes in a ring) |
| `Content/Scripts/AN_RallyRace.ocgraph` | The game: countdown, gates by position, laps, times, respawn, HUD. One state machine in float VARs |
| `Content/Scripts/AN_RallyRules / Controller / Driver.ocgraph` | Game mode, controller, and the pawn (it hears the car's crash and landing events and drives the camera shake) |
| `Content/Input/Rally.ocinput` | The input scheme: the template's `Drive.*` actions plus `Respawn` |
| `Content/UI/RallyHud.ocui` | The HUD layout |
| `Content/Data/Numbers.csv` | Row N is the number N (plus gear, lap and countdown texts): graphs have no number-to-text node, so every digit is looked up with `AN_SetUiTextFromTable` |
| `Content/Vehicles/Sedan.ocvehicle` | The template's sedan, retuned for laps (less torque, more drag, traction control) |
| `Content/Meshes`, `Sounds`, `Particles`, `Materials` | Procedural meshes (`Sedan` and `Wheel` copied from the Vehicle template; `Crate`, `GlassPane`, `Cone` generated), tones, effects, flat-colour materials (`M_Rally*`) |
| `Source/*.obj` | The generated meshes before cooking |

## Regenerate

```
python make_rally_dash.py              # writes everything, cooks Crate/GlassPane/Cone with build/bin/AverAssetC.exe if it exists
python make_rally_dash.py --no-import  # skip the cook
```

Needs only Python 3 and the repository (it imports `templates/Vehicle/graphkit.py` and `vehicle_geo.py`). The track, gates, scenery, lanes and graph data all come from the script, so tune the loop (`_centre_curve`), the lap count (`LAPS`), the gate count (`GATES`) or the car (`SEDAN`) there and re-run.

## Not done on purpose

- No opponents racing you: the traffic only follows its lanes.
- No restart key: Stop and press Play again (the race state is a graph instance and starts fresh).
- Checkpoints are tested by distance from the car's origin to each gate centre (12.5 m radius), not by a trigger volume.
- Rally-style surfaces (gravel, ice) need a per-placement friction the level format does not have.
