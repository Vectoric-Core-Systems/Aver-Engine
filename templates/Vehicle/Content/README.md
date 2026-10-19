# Vehicle template

A sedan and a tank in a test arena, driven by the engine's own vehicle system (`docs/VEHICLES.md`). Nothing here is C#: the vehicles are
data (`Vehicles/*.ocvehicle`), the map places them with a `COMPONENT Vehicle` record each, and the only logic is five small Aver Node
graphs. Press Play.

## Controls

On foot (the first-person pawn `AN_VehicleDriver`):

| Key | Does |
|---|---|
| W A S D, mouse | Walk and look |
| E, within 4 m of the sedan or the tank | Get in |

In a vehicle (the `Drive.*` actions of `Input/Vehicle.ocinput`; the gamepad bindings are in the same file):

| Key | Does |
|---|---|
| W / Up, S / Down | Throttle, brake (and reverse from a standstill) |
| A D / Left Right | Steer (the tank pivots on the spot when standing) |
| Space | Handbrake |
| Q, E | Shift down, up (only a manual gearbox listens) |
| R | Flip it upright and repair |
| C | Next camera: chase, hood, cockpit |
| F | Get out |
| Mouse, in the tank | Turn the turret |

## What is where

| File | What it is |
|---|---|
| `Maps/Default.ocworld` | The arena: a floor, a ramp, a brake wall, a slalom. The sedan (`COMPONENT Vehicle asset=Vehicles/Sedan.ocvehicle`) and the tank, each with its wheels (and the tank's turret) as nested `CHILD` placements the system moves every frame. An `.ocworld`, not an `.ocmap`, because the legacy `.ocmap` cannot store a COMPONENT record |
| `Vehicles/Sedan.ocvehicle` | Rear-wheel drive, a five-speed automatic, open differentials, ABS, a speed-sensitive steering curve, sound, smoke, damage, three cameras |
| `Vehicles/Tank.ocvehicle` | Tracked: five road wheels a side, 38 t, pivot steering. Open it in the Vehicle editor (double-click) to see the torque curve, each gear's top speed and the wheel layout |
| `Input/Vehicle.ocinput` | The project's one input scheme: `Move`, `Look`, `Interact` and the nine `Drive.*` actions, keyboard and gamepad. The vehicle system reads `Drive.*` only while somebody is driving |
| `UI/VehicleHud.ocui` | The HUD layout: bars for speed, revs, gear and health, and ABS / TCS lamps. Graphs have no number-to-text node, so it is bars; a C# project formats real numbers with `VehicleHud.FormatSpeed` |
| `Scripts/AN_VehicleRules.ocgraph` | The game mode: names the pawn and the controller |
| `Scripts/AN_VehicleController.ocgraph` | The concrete player controller the mode possesses with |
| `Scripts/AN_VehicleDriver.ocgraph` | On foot: walk, look, E to get in (`AN_VehicleEnter`) |
| `Scripts/AN_VehicleHud.ocgraph` | Opens the HUD, shows it while the sedan or the tank has a driver, fills the bars from `AN_VehicleTelemetry` |
| `Scripts/AN_TankTurret.ocgraph` | Turns the tank's `Turret` child with the mouse while somebody drives it |
| `Meshes/`, `Sounds/`, `Particles/` | Procedural geometry, tones and smoke/spark effects. `Meshes/SOURCES.md` says where they come from |

## Make it your own

- **Another car.** Copy `Vehicles/Sedan.ocvehicle`, open it in the Vehicle editor, change the numbers, and point a placed mesh's
  `Vehicle` component at it (Details > Add Component > Vehicle). **Create .ocvehicle** in that section writes one from a preset sized to
  the mesh. A vehicle mesh's origin is its bottom centre, +X forward; each wheel is a separate mesh with its axle on Y and its origin at the hub.
- **Tuning while it runs.** Select a vehicle in Play and open Details > Vehicle > Live tuning: power, grip, brakes, suspension, steering lock, drag.
- **Show the physics.** Show > Vehicles draws suspension (green to red by compression), tyre forces, slip rings, the collision boxes and the velocity.
- **Edits to an `.ocvehicle` apply to vehicles built afterwards** (the next Play or spawn).

## What this template deliberately does not do

- No ice or gravel strip: the level format has no per-placement friction, so a surface needs a Rigid Body added in the editor
  (select the strip, Add Component > Rigid Body, set its friction). `test-content/VehicleLab` has the lanes for that.
- No AI drivers and no traffic (`vehicle <preset>` placements and `.oclanes` are the traffic system, separate from this).
- No multiplayer. No saving of a vehicle's state: a loaded level starts with every vehicle intact and unoccupied.
- No number readouts on the HUD (see above).
