# Contact monitoring and Areas (Fabric G2)

Godot's `contact_monitor` / `max_contacts_reported` / `body_entered` / `body_exited` and `Area3D` (signals, gravity, damping and wind overrides, static-body detection, area-vs-area) on Aver.Physics. Opt-in throughout: no existing call changed meaning, the global contact and overlap queues behave as before, the determinism golden checkpoints are identical, and a world that never uses the feature creates none of its state (one pointer test per hook).

ABI: `physics_areas_abi.h` (`AVER_PHYS_AREAS_ABI_VERSION 1`). C#: `Body.SetContactMonitor`, `Body.Contacts`, `Body.TryGetContact`, `Body.AreaGravity`, `Body.AreaDamping`, `Area` (create, flags, collision, priority, `SetGravity`, `SetGravityPoint`, `SetDamping`, `SetWind`, `Overlaps`), `Physics.BodyContactEvents`, `Physics.AreaEvents`. Source: `modules/physics/src/PhysicsAreas.cpp`. Tests: `ContactMonitorTest` (61 checks), `AreaTest` (68), `AbiEnumTest`.

No Godot default is built in. The converter or `Aver.Fabric` passes `monitoring`, `monitorable`, the layer and mask bits, the override modes, `max_contacts_reported`, and the numbers (in cm; Godot's are metres). The engine's own defaults are only "monitoring on, monitorable on, mask = every bit".

## Contact monitor

A monitored body keeps the set of solid bodies it touches and reports BEGIN and END once per body pair, however many sub-shapes touch (`aver_phys_contact_event_*`, polled, cleared at the top of each `aver_phys_step` call like the other queues). `aver_phys_body_contact_count/get` read the tracked partners back with the point and normal of the first touch (not refreshed while the touch lasts).

How it works. The contact listener (worker threads) appends raw Added/Removed sub-shape contacts that involve a monitored body to a locked list, only when a monitor exists. At the end of each fixed step the game thread sorts them (deterministic whatever order the workers ran in), counts sub-shape contacts per pair in an ordered table, and decides BEGIN/END.

**Sleep.** Jolt calls `OnContactRemoved` for every contact of a body that falls asleep, so a naive END would fire on every rest. A pair whose count reaches zero is checked: if either body is asleep it stays (marked asleep, no END). Once nobody sleeps it gets a 3-step grace for the contact to be re-added by the wake; `PhysicsSystem::WereBodiesInContact` has the last word (it also repairs a count the listener missed). So a resting body that sleeps, wakes in place and sleeps again produces no records; one that is moved away produces its END after the grace. Limits: a sleeping body whose partner leaves is still reported as touching until it wakes (Jolt does not wake it), and kinematic bodies that stopped count as sleeping.

**max_contacts.** At most `maxContacts` partners are tracked per body. A further partner is held back (the pair is remembered); when a tracked pair ends, the lowest-key held-back pair is promoted with a BEGIN. 0 tracks nothing.

**Removal.** `aver_phys_remove_body` ends the body's pairs immediately; the END records arrive with the next `aver_phys_step` call's events (a body removed between steps would otherwise lose them to the clear).

**Switching on mid-touch.** Enabling a monitor runs one `CollideShape` of the body's own shape (within Jolt's 2 cm contact distance, honouring the layer matrix and groups) and begins what it finds, so Godot's "enabled while touching" behaves; a wrong guess ends at once when `WereBodiesInContact` says no.

**Not contacts:** sensors and areas (they have their own path), soft-body contacts through the soft-body listener, and `CharacterVirtual` (its inner body is not in the handle table; the global contact queue skips it too).

**Snapshots.** Monitor state is not simulation state and is not in a snapshot. A restore clears the pair table and re-seeds it silently with the same query (no BEGIN/END is replayed); the next step's `WereBodiesInContact` verifies the seeds. Rollback code that needs the events re-issues them from its own resimulation.

## Areas

An area is a **sensor body** (`aver_phys_add_sensor_*` or `aver_phys_body_set_sensor`) registered with `aver_phys_area_create`; the handle is the body's. Everything the sensor body already does (Jolt's own overlap queue) is unchanged.

**Detection is the area's own query**, not Jolt's sensor contacts. At the end of each fixed step the sensor's shape is tested against the world (`NarrowPhaseQuery::CollideShape`, one per monitoring area) and the unique bodies are diffed against what was reported. Why: a Jolt static sensor only detects awake dynamic or kinematic bodies (a body going to sleep loses its contact), and the documented cure (`Body::SetCollideKinematicVsNonDynamic`) needs the sensor to be kinematic and always active and costs a lot of collision work against everything static. The query finds static and sleeping bodies at the cost of one shape query per area per step, with nothing running when no area monitors. One enter and one exit per (area, body) however many sub-shapes overlap (the query is deduplicated by body). Plain sensors, soft bodies and character inner bodies are never detected. A body removed while inside exits on the next step end.

**Filtering.** The 16-layer matrix is not consulted. The rule is Godot's documented one: `(area.mask & other.layer) != 0`, where a body's layer bits are its Godot layer (`aver_phys_body_set_collision_filter`) or else the single bit of its user layer, and another area counts only when it is `monitorable` and its own layer (`aver_phys_area_set_collision`, default the user-layer bit) is in the mask. This follows Godot's documentation of `Area3D.collision_mask`; whether Godot's Jolt module also lets a body's own mask reach an area was not checked against its source.

**monitoring / monitorable.** Monitoring off ends what was reported (exits on the next step end) and reports nothing; the overrides still work. Turning it back on re-reports the overlaps as enters.

**Events and the list.** `aver_phys_area_event_*` (enter/exit, `isArea`) are cleared at the top of each step call; `aver_phys_area_overlap_*` is the reported set as of the last fixed step.

### Overrides

Each area has a priority and, independently, a gravity mode, a linear-damp mode and an angular-damp mode, each one of DISABLED, COMBINE, COMBINE_REPLACE, REPLACE, REPLACE_COMBINE, plus an optional wind. For a dynamic body inside several areas the areas are walked from the **highest priority down** (ties: lowest handle), accumulating per property:

| mode | effect |
|---|---|
| COMBINE | adds the area's value, keeps walking |
| COMBINE_REPLACE | adds, then stops (lower areas and the default are ignored) |
| REPLACE | discards what was gathered, takes this value, stops |
| REPLACE_COMBINE | discards what was gathered, takes this value, keeps walking |

If the walk was not stopped the default is added last: the world's gravity for gravity, the body's own (Jolt) damping for damping. The body's `aver_phys_body_set_gravity_factor` scales the final gravity as it always does.

**Gravity.** Directional: `magnitude` along a world-space direction. Point: toward a centre given in the area's local space (so it follows the sensor's pose); with a unit distance `u` the strength is `magnitude * (u / d)^2` (so `magnitude` at `u`, Godot's documented inverse-square), without it constant; `d` is floored at 1 mm and the pull is zero exactly at the centre. Applied as `AddForce(m * factor * (g_total - g_world))` before `Update` each fixed step, so the integration is Jolt's own: an area REPLACE equals changing the world gravity (`AreaTest` compares positions to 0.5 cm).

**Damping.** The Jolt body's damping is rewritten each step to the walk's total and restored when the body leaves (or the area goes), unless the caller changed it meanwhile (a value that is not the one we wrote is taken as the new base).

**Wind.** Godot's `wind_force_magnitude` only moves SoftBody3D; the request here is a force on rigid bodies, so this is a separate, simple one: a force (kg*cm/s^2, the unit of `aver_phys_body_add_force`) along a direction on dynamic bodies inside, weakening beyond the unit distance from the area's origin as `(u/d)^attenuation` (0 = uniform). It adds to, and ignores the modes of, the other overrides.

**Which bodies.** Dynamic bodies with motion properties. Kinematic and static bodies in an area only trigger events.

**Sleeping bodies** receive no force. A sleeping body is woken when it first falls under an area override or when the override's effect on it changes (compared to what was last applied), then pushed as usual. A body asleep in an unchanged override stays asleep, which is the price of not waking a world every step; `aver_phys_body_set_allow_sleeping(0)` is the escape.

**State and snapshots.** Overrides are re-queried at the start of every fixed step from present positions, so they carry no overlap state across a restore; the replay of an area-driven run after `aver_phys_snapshot_restore` is bit-identical (`AreaTest`). Area configuration (flags, modes, numbers) is configuration like the layer matrix and is not in a snapshot; only the small wake/damping bookkeeping and the reported sets are kept, and the reported sets correct themselves on the next step (a restore may emit the difference as enters/exits).

**Cost.** Nothing runs without areas or monitors. A monitoring area costs one shape query per fixed step; an override area one more. `aver_phys_body_area_gravity/damping` evaluate fresh on demand (Godot's total gravity / damping) for scripts.

## Not done

- Godot's `body_shape_entered` shape indices and `area_shape_entered`; the enter/exit is per body.
- Godot's per-body `_integrate_forces` direct state and constant velocity on static bodies are a separate G2 row.
- Area `audio_bus_override`, reverb and per-area `gravity_space_override` on characters (CharacterVirtual keeps the world gravity).
