# Aver.Vehicle  (`modules/vehicle`)

- **Language:** C++
- **Depends on:** Core, Formats, Physics (the C ABI only: no Jolt header, no scene, no GPU)
- **Docs:** [docs/VEHICLES.md](../../docs/VEHICLES.md)

The pure half of vehicles, beside `Aver.Vehicle.Scene` (`modules/vehicle.scene`) the way `Aver.SoftBody` sits beside `Aver.SoftBody.Scene`.

| Header | What it is |
|---|---|
| `VehicleSpec.hpp` | the spec: body, engine, torque curve, gearbox, axles, tyres, wheel classes, wheels, tracks, driver aids, aero, seat, audio, effects, damage, camera, HUD |
| `OcVehicle.hpp` | `.ocvehicle` text: `parseOcVehicle`, `writeOcVehicle` (canonical, idempotent), `validate` |
| `VehiclePresets.hpp` | `specFromBounds` (the traffic presets `car van truck bus sports` plus `tank`, derived from a mesh's size), `builtinSpec` (`sedan`, `tank`, what the Vehicle template ships) |
| `VehicleBuild.hpp` | `buildVehicle`: the one place a spec becomes `aver_phys_vehicle_*` calls |

`world::VehicleSystem` (AI traffic) builds its cars through `specFromBounds` + `buildVehicle`; `VehiclePhysicsSystem` (drivable cars) does the same. A spec equal to a traffic preset issues the same v1 physics calls with the same arguments as the code it replaced.

`modules/netvehicle` stays a skeleton: its replication needs the fixed-step input stream and the event sequence numbers this feature provides, nothing more is promised.
