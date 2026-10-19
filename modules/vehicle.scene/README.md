# Aver.Vehicle.Scene  (`modules/vehicle.scene`)

- **Language:** C++
- **Depends on:** Core, Formats, Physics, Scene, Vehicle
- **Docs:** [docs/VEHICLES.md](../../docs/VEHICLES.md)

Drivable vehicles on scene entities. `VehiclePhysicsSystem` owns every Jolt vehicle that belongs to an entity with a `CVehicle` (a runtime-registered component, 96 bytes, zero-filled storage means the useful state):

- `prePhysics` builds new vehicles (at most two a frame) from their spec, reads the `Drive.*` named actions for a vehicle with a driver, sends the pedals and the scales, runs one-shot flip / repair requests and sets the collision period by distance;
- `postPhysics` **writes the entity pose every frame in C++**, moves the wheel visuals (child entities, or control-rig inputs for bones), publishes telemetry into the `CVehicle` OUT fields, drains the physics event stream, parks the driver at the seat, and feeds sound and smoke.

Sound, smoke and sparks, crash damage, the camera and graph events leave through sinks (`setAudioPort`, `setEffectSink`, `setCrashSink`, `setCameraSink`, `setNotifySink`, `setRigInputSink`); this module links none of audio, particles, soft bodies or cameras. The relay provider `vehicleProvider` carries `aver_fw_vehicle_*`.

Host wiring is one block in `GameSystemsWiring.hpp` (`registerGameSystems`, `tickVehiclesPre`, `tickVehiclesPost`, `resetGameSystems`), shared by the editor's Play and the packaged game.
