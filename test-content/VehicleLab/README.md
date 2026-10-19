# VehicleLab

Engine test content for the vehicles (`docs/VEHICLES.md`). Not a demo game: the arena is the built-in `Meshes/cube.ocmesh`, the vehicles are
procedural meshes, nothing is third-party. Open the project, open `Maps/VehicleLab.ocworld`, press Play. The meshes are already cooked into
`Content/Meshes`; `make_vehicle_lab.py` rewrites them and everything else (below).

## Driving

The pawn is the editor's default flying one. To drive: select a vehicle, **Details > Vehicle > Drive** (it gets in with a ghost driver; **Exit** gets out).
The keys are `Input/Vehicle.ocinput`'s `Drive.*` actions: W A S D or the arrows, Space handbrake, Q / E shift (manual gearboxes), R flip and
repair, C camera, F exit; a gamepad works too. **Show > Vehicles** draws suspension, tyre forces, slip rings, collision boxes and velocity;
the Details section has live tuning (power, grip, brakes, suspension, steering, drag, downforce, ABS, TCS) and a per-wheel table.

## What is where

All distances are centimetres; the spawn is at (-600, 0) looking along +X.

| Where | What | Checks |
|---|---|---|
| y = -450 | **Hatchback**: all-wheel drive, limited-slip centre, limited-slip rear | Drives, steers, ABS and TCS lamps in the table; settles on four wheels |
| y = -1100 | **Truck6x4**: six wheels (front steered and undriven, two driven rear axles with limited-slip differentials), a centre differential, `BOX` cab, bed and rails | Settles on six wheels; both rear axles share the torque (Forces); the cab leans less than a car on a hard brake |
| y = 450 | **Tank**: tracked, manual gearbox, turret child | Pivots on the spot; Q and E shift; climbs the TankRamp (1400, 2800) without flipping |
| y = 1100 | **RiggedCar**: a skinned mesh whose wheels are BONES (`Source/RiggedCar.gltf`, `Rigs/RiggedCar.ocrig`) | The wheels roll, steer and ride the suspension through control-rig inputs, with no child entities |
| (400, 2000) | **Hatchback2** at the start of the **ice-split lane** (x = 800..4800: `IceLeft` under the left wheels, `TarmacRight` under the right) | See below |
| y = -1800, x = 800..5800 | **Brake strip**: a post every 10 m (`Brake0m` ... `Brake50m`) | Brake from 100 km/h at the first post, stop beside the one you chose; ABS keeps it steerable. Compare with the ABS toggle in Live tuning |
| x = 2000..6200 | **Drop**: a 20 m ramp up to a 3 m platform whose edge is at x = 6200 | Drive off: the `Land` event (a thump, dust) and the camera impact shake |
| y = -2600 | **Slalom**: twelve posts 6 m apart | Steering rate limit and the speed-sensitive steering curve; the camera follows without lag spikes |
| x = 7200 | **Crash wall** with a `Destructible` panel (`CrashPanel`, wood) in the gap | At 60 km/h or more: damage on the vehicle (health), a Crash event, and the panel dents and tears |

### The ice-split lane (differential check)

The level format has no friction key, so the ice is made by hand once: select **IceLeft**, **Details > Add Component > Rigid Body**, motion **Static**,
friction **0.05**; leave `TarmacRight` alone. Then drive **Hatchback2** along +X on full throttle with its left wheels on the ice:

- `diff open` on AXLE 1 (`Vehicles/Hatchback.ocvehicle`): the ice-side rear wheel spins, the car barely pulls (the front axle's open diff does the same);
- `diff lsd 2.2` (as shipped): some torque reaches the gripping wheel, the car pulls away with the wheel spinning;
- `diff locked`: both wheels turn at the same speed, the car pulls straight.

Edit AXLE 1 in the Vehicle editor (double-click the `.ocvehicle`), save, press Play again; the per-wheel table shows the slip on each side.

## What the generator writes

`python make_vehicle_lab.py` (needs `build/bin/AverAssetC.exe`, or pass `--assetc <path>`; `--no-import` writes without cooking) rewrites:

- `Content/Source/*.obj` and `Content/Source/RiggedCar.gltf` (procedural), then cooks each into `Content/Meshes/` with
  `AverAssetC convert <file> --out-dir Content/Meshes --base <Name>` (the glTF gives `RiggedCar.ocmesh` and `RiggedCar.ocskel`);
- `Content/Vehicles/*.ocvehicle`, `Content/Rigs/RiggedCar.ocrig`, `Content/Input/Vehicle.ocinput` (the template's) and `Content/Maps/VehicleLab.ocworld`.

The shapes live in `templates/Vehicle/vehicle_geo.py`, shared with the Vehicle template's generator.

## Known limits

- The importer re-derives the glTF's inverse bind matrices from the rest pose (it says so in its log); the file is written so the two agree.
- The skeletal route is also covered headlessly by `tests/vehicle.scene` (`VehicleSystemTest`, "bones"), which does not need the importer.
- No ice by file, no traffic, no AI drivers, no multiplayer, no save of vehicle state.
