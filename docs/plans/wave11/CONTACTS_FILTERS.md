# Contacts and filters: the full contact pipeline, pair rules, groups, query filters and rigid-body force volumes (wave 11)

Design contract for the implementing agent. Area of "integrate the rest of Jolt Physics". Nothing here is built or run. Rules in force: upgrade the existing physics ABI, layer matrix, query set, component pattern and node families (no parallel system), no new third-party code (everything used is already vendored in `modules/physics.jolt`), lean comments, never delete files, additive ABI only (new functions, new headers, no version bump of the old ones), D3D12 and Vulkan parity (this area adds no GPU work: the one overlay is `rhi::LineVertex` lines both backends draw), headless ctest where possible, headed manual runs by the owner, never rebuild over the owner's running editor. Format and depth follow `docs/plans/wave10/VEHICLES.md`.

Every "exists today" claim below was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it. Every threshold and budget number in this document is an UNVERIFIED starting value until the headed pass in section 6, and nothing here has been timed.

**Scope.** Solid-body contacts (Added, Persisted, Removed, with the numbers gameplay wants), wake and sleep events, pair rules (ignore a pair, team pass-through, one-way platforms), contact modification (apparent mass, conveyor surface velocity, per-surface friction and restitution, combine modes), collision groups for ordinary bodies, filter objects for queries, active-body enumeration with a bulk pose read, point velocity, "were they touching", collision prediction, kinematic sensors that see static bodies, per-body Jolt flags (gyroscopic force, manifold reduction, internal-edge removal), per-body and volume gravity (planets), wind and radial force fields on rigid bodies, and the host pump that turns all of it into `AN_OnEvent` events, C# actor hooks and a pose write-back that touches awake bodies only. **Not** in this area: Jolt ObjectStream and PhysicsScene (owner decision), soft-body fields (SOFTBODY_DEPTH owns `framework_softfield_abi.h`; rigid bodies are not its targets, section 1.4), vehicles' own wheel combine, ragdoll authoring, double-precision coordinates (LARGE_WORLD owns that flip; section 2.3.7 says what this area needs from it).

---

## 0. Decisions in one screen

1. **Four additive physics headers, each with its own version define:** `physics_contacts_abi.h` (event stream, per-body event gating, prediction, "were they in contact", point velocity), `physics_rules_abi.h` (pair ignore, teams, one-way, apparent mass, conveyors, surface table and combine modes, groups, body flags, kinematic-sensor-vs-static), `physics_filters_abi.h` (query filter objects and the `_f` queries, entity-to-body lookup, active-body list, bulk poses, activate-in-box), `physics_forcevolumes_abi.h` (gravity, wind and force volumes, per-body gravity vector, radial impulse). Nothing in `physics_abi.h` and the other twelve headers changes. C# mirrors are new `Native.*.cs` files in `Aver.Physics`; `tests/abi/src/AbiEnumTest.cpp` gets four rows.
2. **One per-body tag table, indexed by Jolt's `BodyID::GetIndex()`.** It holds what worker threads must read without a lock: handle, entity, event mask and thresholds, surface, team, rule bits, apparent-mass scales, combine modes and an index into a side slab for the bulky rules (one-way axis, conveyor velocity, gravity vector). Writes happen on the game thread between steps; reads happen on Jolt workers during a step. A stale slot (a body rebuilt or destroyed by a path that did not rebind) fails an id check and reads as defaults, so the failure mode is "feature silently off", never "wrong body".
3. **The new event stream is opt-in per body and polled.** `aver_phys_contact_*` (first manifold point, Added only, every pair) stays exactly as it is and keeps filling by default. The new stream (`aver_phys_cev_*`) records only for bodies whose event mask asks, carries impulse, depth, relative velocity, sub-shape ids, surfaces, entities and pair-level begin/end flags, and is sorted and numbered at the end of every fixed step so worker-thread order never leaks (the determinism contract).
4. **Pair-level Begin and End are derived on the game thread** from sub-shape Added and Removed with a small ordered pair table, and a sleeping body does not end its touches (Jolt reports Removed for every contact of a body that falls asleep). A touch lost to sleep ends only if it is not re-established within 3 steps of the body waking.
5. **Impulse is an estimate** from Jolt's `EstimateCollisionResponse` at `OnContactAdded` (before the solver runs, ignores third bodies), capped at 256 estimates per fixed step; past the cap a crude `reduced mass * approach speed * (1 + e)` is used and flagged. It exists for sound volume, FX scale and damage, not for physics.
6. **Pair rules run where they are cheapest.** Pair-ignore and team pass-through are a Jolt `SimShapeFilter` (asked before contact generation, fast exit when neither body has a rule bit). One-way platforms need the contact normal, so they use `ContactListener::OnContactValidate`, with the fast exit `AcceptAllContactsForThisBodyPair` for every pair that has no one-way body. Characters (`CharacterVirtual` does not go through either) get the same pair, team and group rules through a `CharacterContactListener`; one-way and conveyors are rigid-body only (open question 8).
7. **Contact modification is body-level state applied at Added and Persisted:** apparent mass and inertia scale (`mInvMassScale`, including infinite), conveyor linear and angular surface velocity (world or body-local), a per-surface-pair friction and restitution override. Because Jolt wants the scale constant over a pair's life, these are body constants, not per-call arguments.
8. **Combine modes are Jolt's own function pointers, installed lazily.** `PhysicsSystem::SetCombineFriction` and `SetCombineRestitution` take a capture-less function pointer; ours reads per-body modes from the tag table. It is installed only when a non-default mode is first set, and with every mode at default the world is bit-identical to today (Jolt's sqrt-product friction, max restitution). When two bodies disagree, the higher precedence wins: Max, Multiply, Min, Average, Default. That order is a choice made here, not copied from anywhere.
9. **Surface ids, not physical-material assets, in v1.** A surface is an id 1..255 with friction, restitution and two combine modes, defined from C, C#, a graph node or a level's start script; a body carries one; events report both bodies' surface ids; a terrain body can report a per-point id through a host sampler next to the existing friction sampler. A `.ocsurface` asset is a follow-up (open question 9).
10. **Collision groups for ordinary bodies reuse Jolt's `CollisionGroup` with one Aver `GroupFilter`.** Semantics: bodies of one group do not collide with each other unless the group allows self-collision or the sub-group pair is enabled (a machine, a trailer, a held object with its holder). Group ids map to Jolt group ids above 0x40000000 so they can never clash with the ragdoll's `GroupFilterTable` (group id = ragdoll id, `PhysicsRagdoll.cpp:73,435`). The 16-layer matrix is untouched and still decides category collisions.
11. **Query filters are objects, and the existing query signatures are frozen.** `aver_phys_qf_*` builds a filter (layer mask, flags, ignored bodies, characters, vehicles, ragdolls, groups, entities and compound sub-shapes); nine `_f` queries take it. A filter is a `BodyFilter` for everything except sub-shape ignores, which add a `ShapeFilter`. Ignoring a whole vehicle or ragdoll needs only the body filter (a vehicle is one body, a ragdoll is a list of bodies); the survey's "ShapeFilter" is used for the one thing a body filter cannot do.
12. **Activation is observed, not polled.** A `BodyActivationListener` feeds WAKE and SLEEP records for flagged bodies, `aver_phys_active_bodies` lists the awake ones, and `aver_phys_active_poses` returns handle, entity and pose for every awake dynamic body in one call.
13. **The "scene transform sync" the survey says to optimise does not exist.** No code in `sandbox`, `Runtime`, `modules/world` or `modules/scene` reads a rigid body's pose (the only callers of `aver_phys_body_position` and `_rotation` are inside `modules/physics` and `Aver.Physics`). `PhysicsSceneSync.cpp` only creates bodies and joints. A dynamic `CRigidBody` entity never follows its body unless a script copies the pose. This area adds the write-back, built on the active list from the start, so it only ever touches awake bodies (section 2.8.4, open question 1).
14. **A new module `Aver.Physics.Scene` (`modules/physics.scene`).** It holds the event pump, two runtime-registered components (`CContactSettings`, `CForceVolume`), the force-volume pose follower, the pose write-back, and the relocated body-and-joint creation code (open question 2). Links Core, Physics, Scene; no audio, particles, framework or camera. Events leave through a sink the host wires to the framework event bus.
15. **Events reach graphs and C# through the existing bus.** The pump posts `Physics.Hit`, `Physics.BeginTouch`, `Physics.EndTouch`, `Physics.Stay`, `Physics.Wake`, `Physics.Sleep`, `Physics.OverlapBegin`, `Physics.OverlapEnd` with `target` = the entity and a typed payload; `AN_OnEvent` entries hear them, and `AverActor` gains optional typed hooks (`OnHit`, `OnBeginTouch` ...) bound only for actors that override them. Sensor overlaps, which nothing in C++ consumes today, are routed through the same pump.
16. **Both hosts get it from one place:** `tickGameplayGroups` in `Runtime/include/aver/game/GameTick.hpp` (shared by the editor Play loop, the packaged runtime and `--play-test`), before and after `aver_phys_step`.
17. **Per-body custom gravity and gravity volumes are the same mechanism.** A force added in the fixed step before `Update`: `m * (g_field - g_world * gravityFactor)`, so a body in a planet's volume falls toward the planet exactly as it would under that gravity. Characters read the same field in the step loop, and may have their up axis follow it.
18. **Rigid wind and force fields are new work, not a soft-field extension.** `framework_softfield_abi.h` targets cloth, destructibles and debris only (`AVER_FW_SF_SPEC` target mask 1, 2, 4). Rigid volumes are evaluated over the awake dynamic bodies once per fixed step; a radial impulse (explosion) is a separate one-shot call.
19. **Aver Node: family `PhysicsContacts`, 21 rows** written to the wave 9 C contract; events need no node (`AN_OnEvent`), a pure `an_physlasthit` reads the current step's record for an entity.
20. **Editor:** Add Component `ContactSettings` and `ForceVolume`, Details sections with a live event log, `Show > Contacts` overlay (points, normals, impulse, volumes), level records `COMPONENT ContactSettings` and `COMPONENT ForceVolume`.
21. **Size XL, eight phases** (section 4). A and B carry the risk (the listener rewrite and the tag table); E (the scene module) is the largest.

---

## 1. What exists

### 1.1 Contact pipeline today

| Fact | Where |
|---|---|
| One `ContactListener` for the world, `EventListener`, installed at init next to the soft-body listener; it overrides `OnContactAdded`, `OnContactPersisted`, `OnContactRemoved` and nothing else (no `OnContactValidate`) | `PhysicsWorld.cpp:81-135,156,307-308`; soft-body listener is a separate `SoftBodyContactListener`, `PhysicsSoftBody.cpp:174,263` |
| Added: drops any pair where either body has no handle (`handleOf` reads `byId`), which silently drops every contact with a **character's inner body** (it is created by Jolt, never in `byId`); a sensor pair becomes an `OverlapEvent` (enter); a solid pair becomes a `ContactEvent` carrying only bodies, the **first** manifold point (on body 1) and the normal | `PhysicsWorld.cpp:84-104,150-154`; `PhysicsInternal.hpp:161,163` |
| The whole callback runs inside `std::lock_guard<std::mutex> eventMutex` from its first line, for every contact of every body, including the surface-friction branch | `PhysicsWorld.cpp:90` |
| Persisted: only re-applies the terrain surface friction; nothing is recorded | `PhysicsWorld.cpp:107-112` |
| Removed: recorded only when one of the pair is a sensor (an exit overlap); two solid bodies parting is not reported | `PhysicsWorld.cpp:114-125` |
| The queue is polled with `aver_phys_contact_count` and `_get`, cleared at the top of `aver_phys_step`; it is unbounded within a call | `PhysicsWorld.cpp:392,1039-1056`; `physics_abi.h:425-441` |
| Sensors: entered and left, polled with `aver_phys_overlap_*`; a static sensor only sees active dynamic or kinematic bodies | `PhysicsWorld.cpp:1060-1076`; Jolt `Body.h:68-71` |
| **Nothing in C++ outside `modules/physics` reads either queue.** The only consumers are C#: `Physics.Contacts` and `Physics.Overlaps` | grep of `Runtime`, `sandbox`, `modules/*`; `Aver.Physics/Physics.cs:236-262`, `Native.cs:122-125` |
| A chassis hit is noted from inside Added for the vehicle IMPACT event | `PhysicsWorld.cpp:103` calls `vehicleOnContact`, `PhysicsVehicleExt.cpp:556` |
| The terrain's per-point friction and restitution override `mCombinedFriction` and `mCombinedRestitution` in Added and Persisted, by a pure sampler on worker threads | `PhysicsWorld.cpp:128-146`; `physics_water_abi.h` |
| Friction and restitution combine with Jolt's defaults (`sqrt(f1 * f2)`, `max(r1, r2)`); the engine never calls the **system's** `SetCombineFriction` or `SetCombineRestitution`. The one `SetCombineFriction` in the tree is the **wheel** controller's, a different function | `ContactConstraintManager.h:554-555`; `PhysicsVehicle.cpp:510` |
| No `ContactSettings` field other than the two combined values is ever written: no mass or inertia scale, no surface velocity | `PhysicsWorld.cpp:128-146` |
| No per-pair accept or reject rule exists. The only pair rule is the layer matrix `ObjectLayerPairFilter` (16 user layers, symmetric, static-vs-static never) | `PhysicsInternal.hpp:125-157`, `PhysicsLayers.cpp:62-86` |

### 1.2 Filtering today

| Fact | Where |
|---|---|
| Collision groups are used by the ragdoll only: one `GroupFilterTable` per ragdoll, `SetGroupID(ragdoll id)`, parts disabled pairwise; ordinary bodies have no group | `PhysicsRagdoll.cpp:73,386-406,435` |
| Queries filter by a user-layer mask (`MaskLayerFilter`), by up to two ignored bodies (a `BodyFilter`) and, for the `_q` and `queries` family, by sensor inclusion; no group, no entity, no ragdoll or vehicle shorthand, no `ShapeFilter` anywhere | `PhysicsQueryFilters.hpp:19-60`, `PhysicsLayers.cpp:34-78`, `physics_queries_abi.h:26-45,70-80` |
| `aver_phys_raycast_ex`, `_overlap_sphere_ex`, `_sphere_cast_ex` use the one-hit `CastRay(ray, hit, bpFilter, layerFilter, bodyFilter)` overload, which has **no shape filter parameter**; the collector overload does | `PhysicsLayers.cpp:152`; Jolt `NarrowPhaseQuery.h:31,35` |
| Jolt 5.6 has `ShapeFilter` (queries) and `SimShapeFilter` (simulation, called before contact generation per shape pair, also in the CCD path) | `Collision/ShapeFilter.h`, `SimShapeFilter.h`, `PhysicsSystem.cpp:1106,2047`; the engine uses neither |
| Character movement filters through `CharacterFilter` (hides every character's inner body); no `CharacterContactListener` is set | `PhysicsCharacter.cpp:77-83`; no `SetListener` in `modules/physics/src` |

### 1.3 Activation, lookup, flags

| Fact | Where |
|---|---|
| Wake, sleep and ask: `aver_phys_body_activate`, `_deactivate`, `_is_active`; sleeping permission per body. **No `BodyActivationListener`, no active list** | `PhysicsBody.cpp:399-415`, `physics_body_ext_abi.h:75-80`; grep finds no listener |
| Enumeration is "all bodies": `aver_phys_body_handles` | `PhysicsWorld.cpp:643`, `physics_abi.h:77` |
| Entity to body: only for characters (`aver_phys_character_of_entity`, a walk of the character table). Body to entity: `Body::GetUserData`, stamped by `aver_phys_set_entity`, truncated to `int32` at every reader | `PhysicsCharacter.cpp:503-515`, `PhysicsWorld.cpp:774-789,822` |
| Per-body flags Jolt offers and the engine never sets on rigid bodies: gyroscopic force, manifold reduction, enhanced internal edge removal (characters have edge removal: `aver_phys_character_set_edge_removal`), `CollideKinematicVsNonDynamic` | grep of `modules/physics/src`; Jolt `Body.h:67-109`, `BodyInterface.h:286` |
| `ActivateBodiesInAABox`, `GetPointVelocity`, `WereBodiesInContact`, `EstimateCollisionResponse` are vendored and unused | Jolt `BodyInterface.h:144,212`, `PhysicsSystem.h:251`, `EstimateCollisionResponse.h:44` |
| A body can be **rebuilt with a new `BodyID` under an unchanged handle**: static to movable (`giveMotionProperties`, from `GetBodyCreationSettings`, so the collision group, flags and user data survive), the vehicle chassis swap, and a character's inner-body toggle | `PhysicsBody.cpp:97-160`, `PhysicsVehicle.cpp:425-445`, `PhysicsCharacter.cpp:397-399` |
| Bodies are created through `addBody` (box, sphere, shapes, hull, mesh, heightfield, sensors, vehicle chassis); ragdoll parts, character inner bodies and soft bodies are created elsewhere and registered in the handle tables by their own code | `PhysicsWorld.cpp:210-235`, `PhysicsRagdoll.cpp:435`, `PhysicsWorld.cpp:673-706` |

### 1.4 Gravity, force fields, material

| Fact | Where |
|---|---|
| Gravity is one world vector plus a per-body factor (`aver_phys_body_set_gravity_factor`) and a per-character factor kept in `World::characterGravity`; the character loop reads `system.GetGravity() * factor` | `physics_abi.h:229`, `PhysicsBody.cpp:334`, `PhysicsWorld.cpp:418-423` |
| The fixed step already has a "before `Update`" slot where buoyancy, soft-body wind and vehicle aids run, and an "after `Update`" slot for vehicle events and joint breaks | `PhysicsWorld.cpp:483-493` |
| Wind exists for builder soft bodies (`SoftBodyAero`), water drag for rigid bodies in water. **No wind, drag or force field acts on a rigid body in air.** Soft fields target cloth (1), destructibles (2), debris (4) | `SoftBodyAero.cpp`, `Buoyancy.cpp`; `framework_softfield_abi.h:AVER_FW_SF_SPEC` |
| No physical-material table. A surface is a body's friction and restitution; the vehicle doc says so and defers a table (VEHICLES open question 7) | `physics_abi.h:216-225`; `docs/plans/wave10/VEHICLES.md` section 7 item 7 |

### 1.5 What consumes physics events and poses today

| Fact | Where |
|---|---|
| Graph and actor events cross one seam, `ScriptHost::graphFire(entity, name)`, which carries **no payload**; the bus (`aver_fw_event_post`, `aver_fw_event_push_*`, `AN_OnEvent`, `AN_EventPayload`) does carry a typed payload and delivers to subscribers whose owner equals the event's target | `SandboxPlay.cpp:1157-1160`, `docs/GAME_TIMERS_EVENTS.md` "Events", `framework_timers_abi.h:81-124` |
| `AverActor` has one hook for named events, `OnEvent(string)`, with no payload | `Aver.Framework/Actor.cs:50` |
| Joint breaks are the one physics event queue with a documented "persists until drained, newest 1024" rule; contacts are cleared at the top of each step call | `docs/physics/JOINTS_AND_BODIES.md:28-33` |
| `PhysicsSceneSync.cpp` builds a body per `CRigidBody` and a constraint per `CJoint` (two passes) and removes them; it is called only from editor Play start and stop. **There is no pose write-back**, and the packaged runtime has no call to it at all | `sandbox/src/PhysicsSceneSync.cpp:218-308`, `SandboxPlay.cpp:152,686`; grep of `Runtime` finds no `CRigidBody` use except a vehicle-conflict check `LevelComponents.cpp:222` |
| The hosts share one tick helper that brackets the physics step; both call it inside the PLAYING gate | `GameTick.hpp:156` (`steps = aver_phys_step(dt)`), `SandboxUpdate.cpp:766-781`, `GameApp.cpp:593-602` |
| Entities moved by a system other than an object clip are declared to `PlayMobility::seedMovable`, and a mid-Play save must not write them | `Runtime/include/aver/game/PlayMobility.hpp:45-60`, `SandboxPlay.cpp:157`, `SandboxLevelEdit.cpp:620-640` |
| The modules this area copies the shape of: `vehicle` / `vehicle.scene` (runtime-registered component, system singleton, sinks, `GameSystemsWiring.hpp` register / tick / reset) | `modules/vehicle.scene/CMakeLists.txt`, `Runtime/include/aver/game/GameSystemsWiring.hpp:215-260,395-440,528-560` |

### 1.6 Jolt 5.6 facilities used (all vendored, all read)

`ContactListener::OnContactValidate(body1, body2, baseOffset, collisionResult)` with `ValidateResult` (`ContactListener.h:76-80`); `OnContactAdded` and `Persisted` with `ContactSettings` (`mCombinedFriction`, `mCombinedRestitution`, `mInvMassScale1/2`, `mInvInertiaScale1/2`, `mRelativeLinearSurfaceVelocity`, `mRelativeAngularSurfaceVelocity`, `mIsSensor`) (`:42-52`); `OnContactRemoved(SubShapeIDPair)` with no body access (`:130`). `PhysicsSystem::SetSimShapeFilter`, `SetBodyActivationListener`, `SetCombineFriction`, `SetCombineRestitution`, `GetActiveBodies(EBodyType, BodyIDVector&)`, `GetNumActiveBodies`, `WereBodiesInContact` (`PhysicsSystem.h:62-89,222-251`). `BodyInterface::ActivateBodiesInAABox`, `GetPointVelocity`, `SetUseManifoldReduction`, `SetCollisionGroup` (`BodyInterface.h:144,212,286,298`); `Body::SetApplyGyroscopicForce`, `SetEnhancedInternalEdgeRemoval`, `SetCollideKinematicVsNonDynamic` (`Body.h:85-109`). `EstimateCollisionResponse` (`EstimateCollisionResponse.h:44`). `CollisionGroup` and `GroupFilter::CanCollide` (`CollisionGroup.h:36-110`, `GroupFilterTable.h:96-108`). `ShapeFilter` with `mBodyID2` (`ShapeFilter.h`). `CharacterBase::SetUp` (`CharacterBase.h:69`). `BroadPhaseQuery::CollideAABox`. Two Jolt contracts bind the design: callbacks run on many threads at once with all bodies locked, so they may read but never call a locking `BodyInterface` function; and "for the same contact pair keep the mass scale the same over time" (`ContactListener.h:42`, the class comment).

### 1.7 The survey, checked

| Survey claim | Verdict |
|---|---|
| Added today gives first manifold point and normal only, `PhysicsInternal.hpp:161`, `PhysicsWorld.cpp:84-104` | **Confirmed**, and it also drops all character contacts (section 1.1) |
| Persisted and Removed events do not exist for solid bodies | **Confirmed** (Removed only for sensors) |
| `OnContactValidate`, `ContactSettings` modification, system combine modes unused | **Confirmed** |
| Collision groups only on ragdolls; no `ShapeFilter` on queries | **Confirmed** |
| `BodyActivationListener` and an active-body list missing | **Confirmed** |
| "Scene transform sync only touches awake bodies, find the sync code" | **Wrong premise: there is no sync code.** Decision 13 |
| Body user-data reverse lookup | Entity from body exists everywhere; **body from entity exists for characters only** |
| Rigid wind and force fields "if not covered by the wave-10 soft-body fields" | **Not covered** (section 1.4) |
| Event delivery "route to C#, Aver Node events (`AN_OnEvent` bus) and an OnHit-style component callback" | Nothing routes physics events to graphs today; sensor overlaps are as unrouted as contacts |
| Per-body custom gravity, `ActivateBodiesInAABox`, point velocity, `WereBodiesInContact`, `EstimateCollisionResponse`, gyroscopic / manifold reduction / edge removal flags, kinematic sensor vs static | **Confirmed unexposed** |

---

## 2. Design

### 2.1 Architecture

```
 Jolt worker threads                                     game thread
 --------------------                                    -----------------------------------------------
 SimShapeFilter  (pair ignore, teams)   <-- tags -->     setters (between steps only): events, rules, groups,
 OnContactValidate (one-way)                              surfaces, scales, conveyors, gravity, volumes
 OnContactAdded / Persisted (rules, estimate, candidates)
 OnContactRemoved (candidates, ids only)
 BodyActivationListener (wake / sleep)
                  |  candidates under a short mutex, only for flagged bodies
                  v
 contactsStepEnd (after Update): sort, pair table, flags, seq   -> cev queue (cleared at top of aver_phys_step)
 forceVolumesStepBegin (before Update): gravity / wind / force   |
                                                                  v
 Aver.Physics.Scene (modules/physics.scene), called from tickGameplayGroups
   preStep : CContactSettings -> tags;  CForceVolume pose -> volumes
   postStep: pump (records + overlaps -> sink -> bus), awake-body pose write-back
        |-- sink (host wires): aver_fw_event_* bus   --> AN_OnEvent, C# actor hooks, Events.Subscribe
```

Threading rule (stated once, enforced by a debug assert on a "stepping" flag): every setter in the four headers is **game-thread only and refused while `aver_phys_step` is running**; every reader inside a Jolt callback reads the tag table and rule tables without a lock. The stream queue is the only structure workers write, under the existing `eventMutex` and only for flagged bodies.

### 2.2 The tag table (src-local, `PhysicsContactsInternal.hpp`)

```cpp
struct BodyTag {                     // 40 bytes; one per BodyID index; size = World max bodies (65536 -> 2.5 MB)
  u32 idValue = 0;                   // BodyID::GetIndexAndSequenceNumber() it belongs to; 0 = unbound
  i32 handle = 0, entity = 0;
  u8  eventMask = 0;                 // AVER_PHYS_EVT_*
  u8  team = 0;                      // 0 none, 1..31
  u16 surface = 0;                   // 0 none, 1..255
  u16 rule = 0;                      // kRulePairIgnore, kRuleOneWay, kRuleConveyor, kRuleMassScale, kRuleGravity, kRuleWind
  u8  frictionMode = 0, restitutionMode = 0;
  f32 minHitImpulse = 0, minHitSpeed = 0;
  f32 massScale = 1, inertiaScale = 1;
  i32 extra = -1;                    // index into the BodyExtra slab (one-way axis, surface velocity, gravity, wind scale)
};
```

- `contactsBindBody(handle, id)` at `addBody`, `aver_phys_character_create` (the inner body id) and at each **rebuild site** (`giveMotionProperties`, the vehicle chassis swap, the character inner-body toggle): a rebind moves the tag, keeps the extra index. `contactsForgetBody(handle)` at `aver_phys_remove_body` and character destroy.
- Reads go through `tagOf(const JPH::Body&)`: index, then `idValue == body.GetID().GetIndexAndSequenceNumber()`, else a static default tag and a counter bump (`AVER_PHYS_CST_STALE_TAG_READS`, asserted zero by the tests that rebuild bodies).
- `aver_phys_set_entity` also writes `tag.entity` (needed because Removed callbacks may not read the body) and maintains `World::entityBodies`, an `unordered_multimap<i32, i32>` for `aver_phys_bodies_of_entity`.
- Bodies Jolt creates for us and `aver_phys_*` never registered have no tag and behave as today.
- Tag memory is a flat array (not a hash) because the hot callbacks run per contact; `idValue` makes reuse of an index safe.

### 2.3 Contact events: `physics_contacts_abi.h`

New header, new source `PhysicsContacts.cpp`. Types are `int32_t`, `uint32_t`, `float`; units are the engine's (cm, kg, cm/s; impulse in kg*cm/s, the unit `aver_phys_body_add_impulse` already uses). Every function returns 1 or 0 (`aver_phys_last_error` says why) except where stated.

```c
#define AVER_PHYS_CONTACTS_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_contacts_abi_version(void);

/* ---- which bodies record (all default 0: the new stream is silent until asked) ---- */
#define AVER_PHYS_EVT_HIT        0x01u  /* an Added contact whose approach speed and impulse pass this body's thresholds */
#define AVER_PHYS_EVT_TOUCH      0x02u  /* pair begin and pair end */
#define AVER_PHYS_EVT_STAY       0x04u  /* Persisted contacts, every contacts_set_stay_period steps per pair */
#define AVER_PHYS_EVT_WAKE_SLEEP 0x08u  /* this body woke or fell asleep */
#define AVER_PHYS_EVT_ALL        0x0Fu
AVER_PHYS_API int32_t aver_phys_body_set_events(int32_t body, uint32_t mask, float minHitImpulse, float minHitSpeedCmS);
AVER_PHYS_API int32_t aver_phys_body_events(int32_t body, uint32_t* outMask, float* outMinHitImpulse, float* outMinHitSpeedCmS);
                                    /* a body or a character handle; thresholds 0 = none; a record is emitted for a side when that side's bits ask */
AVER_PHYS_API int32_t aver_phys_contacts_set_stay_period(int32_t everyNSteps);   /* 1..60, default 4 */
AVER_PHYS_API int32_t aver_phys_contacts_set_legacy(int32_t on);                 /* aver_phys_contact_* queue, default 1 (today's behaviour) */
AVER_PHYS_API int32_t aver_phys_contacts_set_capacity(int32_t maxRecords);       /* 256..65536, default 8192; applies from the next aver_phys_step */

/* ---- the record stream: cleared at the top of aver_phys_step, read non-destructively by any number of consumers ---- */
#define AVER_PHYS_CEV_ADDED     1
#define AVER_PHYS_CEV_PERSISTED 2
#define AVER_PHYS_CEV_REMOVED   3
#define AVER_PHYS_CEV_WAKE      4
#define AVER_PHYS_CEV_SLEEP     5
AVER_PHYS_API int32_t aver_phys_cev_count(void);
AVER_PHYS_API int32_t aver_phys_cev_dropped(void);   /* records refused for lack of room in the last aver_phys_step call */
AVER_PHYS_API int32_t aver_phys_cev_get(int32_t first, int32_t max, int32_t* outI, float* outF);
                       /* copies up to `max` records starting at `first`; record r is outI[r*AVER_PHYS_CEI_COUNT ...] and
                          outF[r*AVER_PHYS_CEF_COUNT ...]; returns how many were copied; either array may be null */

/* int fields */   KIND 0, FLAGS 1, BODY_A 2, BODY_B 3, ENTITY_A 4, ENTITY_B 5, SUBSHAPE_A 6, SUBSHAPE_B 7,
                   SURFACE_A 8, SURFACE_B 9, STEP 10, SEQ 11, SIDES 12, COUNT 13
#define AVER_PHYS_CEI_KIND 0   /* ... one #define per index, then AVER_PHYS_CEI_COUNT 13 */
/* float fields */ POINT_X..Z 0..2 (world cm), NORMAL_X..Z 3..5 (unit, from A towards B as the legacy queue reports it),
                   PENETRATION_CM 6 (negative = speculative), REL_VEL_X..Z 7..9 (B relative to A at the point, cm/s),
                   APPROACH_CMS 10 ((vA - vB) . n, positive = closing), NORMAL_IMPULSE 11, FRICTION_IMPULSE 12,
                   SLIDE_CMS 13 (tangential relative speed), FRICTION 14, RESTITUTION 15, COUNT 16   (AVER_PHYS_CEF_*)
/* flags */  AVER_PHYS_CFL_PAIR_BEGIN 1, PAIR_END 2, SLEEP_REMOVAL 4, BODY_REMOVED 8, CHARACTER 16, CRUDE_IMPULSE 32, SYNTHETIC 64
/* sides */  bit 0 = body A's mask asked for this record, bit 1 = body B's
AVER_PHYS_API int32_t aver_phys_contacts_stats(int32_t* outI, int32_t n);     /* AVER_PHYS_CST_*: candidates, emitted, dropped (per class), estimates, crude estimates, pairs tracked, stale tag reads */

/* ---- state queries and prediction ---- */
AVER_PHYS_API int32_t aver_phys_bodies_in_contact(int32_t bodyA, int32_t bodyB);          /* 1 if they touched on the last fixed step (WereBodiesInContact) */
AVER_PHYS_API int32_t aver_phys_body_point_velocity(int32_t body, float px, float py, float pz, float* outVelocityXyz);   /* cm/s, world point */
AVER_PHYS_API int32_t aver_phys_estimate_impact(int32_t bodyA, int32_t bodyB, const float* pointXyz, const float* normalAToBXyz, float* outF8);
                       /* outF8: 0 normal impulse, 1 friction impulse (kg*cm/s), 2..4 A's velocity after, 5..7 B's (cm/s); uses the bodies'
                          current velocities and the same combined friction and restitution a real contact would get */
```

#### 2.3.1 Recording at the callback (worker threads)

`EventListener` is restructured, not rewritten, and its observable legacy behaviour is unchanged:

1. **Fast exit.** `tagOf(a)` and `tagOf(b)`; if both event masks are 0 (the overwhelming case) the only work is the legacy path and the rules (2.4). The legacy `lock_guard` moves to after the rules and covers only the legacy push and the sensor branch, so rule application and estimation never hold the mutex (today they would, `PhysicsWorld.cpp:91`).
2. **Candidate.** For a flagged side: `point` = the manifold's first world point on body 1 (`GetWorldSpaceContactPointOn1(0)`; centroid of the manifold points is a one-line change if the first point proves unrepresentative, open question 12), `normal`, depth, `rel = v_B(p) - v_A(p)`, `approach`, slide. If the side asks for HIT and `approach >= minHitSpeed`, the impulse is estimated (2.3.3). The candidate goes into a per-call vector under `eventMutex`.
3. **Removed** appends only ids (`handle`, `entity` and `surface` from the tags, sub-shape ids from the `SubShapeIDPair`); floats are 0 because Jolt forbids reading bodies there.
4. **Characters.** The inner body of a character is bound to the character's handle, so a rigid body hitting a character now records (flag `CHARACTER`); the legacy queue keeps skipping them.
5. **Sensors** keep the existing overlap queue and `OverlapEvent`; a sensor pair never produces a stream record. Wake and sleep records come from the activation listener (2.5.2).

#### 2.3.2 End of step: order, pair table, gating (`contactsStepEnd`, game thread, after `Update`)

1. Sort the candidates of this fixed step by `(kind, min(handle), max(handle), subShapeA, subShapeB)`: deterministic whatever order the workers ran in. `STEP` = the fixed-step ordinal within the `aver_phys_step` call, `SEQ` = a counter per world since init, assigned after the sort.
2. Walk them in order against `std::map<u64 pairKey, PairState>` (ordered, so iteration is deterministic). `PairState` = live sub-shape contact count, an "asleep" mark, a verify deadline, and the two entities cached for synthetic ends.
   - Added: count++; the record gets `PAIR_BEGIN` when the count was 0 and the pair is not marked asleep (an asleep pair that wakes and re-adds is a continuing touch, not a new one).
   - Removed: count--; at 0, one of three outcomes. A body of the pair was destroyed this step: `PAIR_END | BODY_REMOVED`, pair erased. A body of the pair has a SLEEP record in this batch, or is inactive now: `SLEEP_REMOVAL`, pair marked asleep, **no PAIR_END**. Otherwise, if `WereBodiesInContact(a, b)` is false: `PAIR_END`, pair erased.
   - A WAKE of either body of an asleep pair sets a verify deadline of step + 3; if neither an Added nor a Persisted for the pair arrives by then, a synthetic REMOVED (`SYNTHETIC | PAIR_END`) is emitted. `contactsForgetBody(handle)` emits `PAIR_END | BODY_REMOVED | SYNTHETIC` for every tracked pair of the handle immediately (Jolt's own Removed callbacks arrive a step later, for ids that no longer map to a handle, and are dropped).
3. Gating: an ADDED record is kept when a side with HIT passes its thresholds, or a side with TOUCH sees `PAIR_BEGIN`. A PERSISTED record is kept for a side with STAY when `(stepIndex + handleA) % stayPeriod == 0`. A REMOVED record is kept when a side with TOUCH sees `PAIR_END`. WAKE and SLEEP are kept for flagged bodies. Everything else is discarded.
4. **Capacity.** At 50 percent full PERSISTED is refused; at 100 percent everything is; `aver_phys_cev_dropped` counts and `stats` splits by kind. BEGIN, END, WAKE and SLEEP records are never refused before HIT and STAY ones because a missed END leaves gameplay state stuck; if even they do not fit the dropped count says so.

#### 2.3.3 Impulse estimate

`EstimateCollisionResponse(a, b, manifold, result, combinedFriction, combinedRestitution, minVelocityForRestitution = 1, iterations = 4)` at Added, with the friction and restitution after rules and surface overrides; `NORMAL_IMPULSE` = sum of `mContactImpulse` times 100 (kg m/s to kg cm/s), `FRICTION_IMPULSE` likewise. 4 iterations, not Jolt's default 10, because the answer feeds sound and FX (UNVERIFIED that 4 is close enough; the test in section 5 bounds the error). Budget `maxEstimatesPerStep = 256`; past it the crude estimate `reducedMass * approach * (1 + e)` is used and `CRUDE_IMPULSE` is set. The estimate is made **before the solver**, from pre-contact velocities, and "will not be accurate when more than 2 bodies collide at the same time" (Jolt's own words, `EstimateCollisionResponse.h:38`); it is documented as a predictor everywhere it is exposed.

#### 2.3.4 Sub-shape ids and material

`SUBSHAPE_A/B` are the raw `SubShapeID::GetValue()` of the manifold: opaque, stable for a given shape, comparable between ADDED, PERSISTED and REMOVED of the same contact. For `aver_phys_add_*_compound_boxes` bodies the child index is recoverable (`CompoundShape::GetSubShapeIndexFromID`) and `PhysicsContacts.cpp` exposes it as `aver_phys_subshape_index(body, subShapeId)` (declared in `physics_filters_abi.h`, 2.5). The **material** of a contact is the two bodies' surface ids (`SURFACE_A/B`); for a surface-sampled terrain body the id comes from the sampler (2.4.6). Manifold reduction merges contacts from different triangles; a body that needs the exact face turns it off (`AVER_PHYS_BF_MANIFOLD_REDUCTION`, 2.4.8).

#### 2.3.5 Legacy queue

`aver_phys_contact_*` and `aver_phys_overlap_*` are unchanged in signature and default behaviour. `aver_phys_contacts_set_legacy(0)` stops the legacy push for projects that only use the new stream (the legacy queue is unbounded within a call, which is a hazard on a large pile). The legacy queue gains the same cap as the new one when it is on (default 8192, newest dropped, one warning per call), a behaviour change only above a count no test or content approaches (UNVERIFIED; the NeonDistrict pile is the check in section 6).

#### 2.3.6 Cost, said plainly

Unflagged bodies pay two array reads and two byte compares per callback and take no new lock. The legacy path still takes `eventMutex` per contact, now for less work. A flagged body pays a candidate push under the mutex per contact and, for HIT, up to one estimate per step budget. **No timing exists**; section 6 item 12 measures a 2000-body pile with zero, ten and all bodies flagged.

#### 2.3.7 Large-world coordinates

`ContactManifold::mBaseOffset` and `GetWorldSpaceContactPointOn1` return `RVec3` (double under `JPH_DOUBLE_PRECISION`), and `fromJolt` takes `Vec3Arg`. All new code reads points through one helper, `fromJoltPos(JPH::RVec3Arg)`, which LARGE_WORLD is expected to add to `Convert.hpp` (shared); if it has not landed when phase A starts, phase A adds the 3-line overload to its own `PhysicsContactsInternal.hpp` and LARGE_WORLD removes the duplicate. Points leave as float cm from the world origin (precision far from the origin is LARGE_WORLD's rebase policy); relative quantities (velocity, depth, impulse) are unaffected. The existing `OnContactAdded` line `fromJolt(manifold.GetWorldSpaceContactPointOn1(0))` (`PhysicsWorld.cpp:99`) moves to the helper in the same edit.

### 2.4 Rules: `physics_rules_abi.h`

New header, sources `PhysicsContactRules.cpp` (pair, teams, one-way, scales, surface velocity, surfaces, combine, listener hooks) and `PhysicsGroups.cpp` (groups, flags, detect-static). `AVER_PHYS_RULES_ABI_VERSION 1`. All handles are body **or character** handles unless stated.

```c
#define AVER_PHYS_RULES_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_rules_abi_version(void);

/* ---- pair ignore: two specific bodies do not collide ---- */
AVER_PHYS_API int32_t aver_phys_pair_ignore(int32_t bodyA, int32_t bodyB, float seconds);
                       /* seconds > 0: for that long (counted in fixed steps from the next step); < 0: until cleared; 0: clears */
AVER_PHYS_API int32_t aver_phys_pair_ignored(int32_t bodyA, int32_t bodyB);
AVER_PHYS_API int32_t aver_phys_body_clear_ignores(int32_t body);

/* ---- teams: bodies of two teams that are marked pass-through do not collide ---- */
#define AVER_PHYS_TEAM_COUNT 32
AVER_PHYS_API int32_t aver_phys_body_set_team(int32_t body, int32_t team);       /* 0 = none, 1..31 */
AVER_PHYS_API int32_t aver_phys_body_team(int32_t body);                         /* -1 for a dead handle */
AVER_PHYS_API int32_t aver_phys_set_team_pass(int32_t teamA, int32_t teamB, int32_t pass);   /* symmetric; (t, t) = members pass each other */
AVER_PHYS_API int32_t aver_phys_team_pass(int32_t teamA, int32_t teamB);
AVER_PHYS_API void    aver_phys_reset_team_pass(void);

/* ---- one-way platforms (rigid bodies) ---- */
AVER_PHYS_API int32_t aver_phys_body_set_one_way(int32_t body, int32_t on, const float* localUpXyz, float maxAngleDeg, float maxPenetrationCm);
                       /* up is in the body's local frame (null = +Z); 0 angle = 60, 0 penetration = 10 */
AVER_PHYS_API int32_t aver_phys_body_one_way(int32_t body, float* outLocalUpXyz);                       /* 1 if on */

/* ---- apparent mass and conveyors (applied at Added and Persisted) ---- */
#define AVER_PHYS_CONTACT_SCALE_INFINITE 1.0e30f
AVER_PHYS_API int32_t aver_phys_body_set_contact_scale(int32_t body, float massScale, float inertiaScale);
                       /* > 0; 1 = unchanged; 2 = acts twice as heavy in contacts; INFINITE = cannot be pushed by a contact (it still moves on its own velocity) */
AVER_PHYS_API int32_t aver_phys_body_contact_scale(int32_t body, float* outMassScale, float* outInertiaScale);
AVER_PHYS_API int32_t aver_phys_body_set_surface_velocity(int32_t body, const float* linearCmS3, const float* angularRadS3, int32_t local);
                       /* the speed this body's surface carries what touches it, e.g. a conveyor; null = zero; local = in the body's frame */
AVER_PHYS_API int32_t aver_phys_body_surface_velocity(int32_t body, float* outLinear3, float* outAngular3, int32_t* outLocal);

/* ---- surfaces and combine modes ---- */
#define AVER_PHYS_SURFACE_COUNT 256
#define AVER_PHYS_COMBINE_DEFAULT  0   /* friction: sqrt(a*b); restitution: max(a, b) -- Jolt's own */
#define AVER_PHYS_COMBINE_AVERAGE  1
#define AVER_PHYS_COMBINE_MIN      2
#define AVER_PHYS_COMBINE_MULTIPLY 3
#define AVER_PHYS_COMBINE_MAX      4
AVER_PHYS_API int32_t aver_phys_surface_define(int32_t surface, float friction, float restitution, int32_t frictionMode, int32_t restitutionMode);
AVER_PHYS_API int32_t aver_phys_surface_pair(int32_t surfaceA, int32_t surfaceB, float friction, float restitution);   /* override for this pair; friction < 0 removes it */
AVER_PHYS_API int32_t aver_phys_body_set_surface(int32_t body, int32_t surface);   /* 0 clears; otherwise the body takes the surface's friction, restitution and modes */
AVER_PHYS_API int32_t aver_phys_body_surface(int32_t body);                        /* -1 for a dead handle */
AVER_PHYS_API int32_t aver_phys_body_set_combine(int32_t body, int32_t frictionMode, int32_t restitutionMode);
AVER_PHYS_API int32_t aver_phys_set_default_combine(int32_t frictionMode, int32_t restitutionMode);
AVER_PHYS_API void    aver_phys_reset_surfaces(void);
typedef int32_t (*aver_phys_surface_id_sampler_fn)(float xCm, float yCm, int32_t* outSurface, void* user);   /* worker threads: a pure read, as the friction sampler */
AVER_PHYS_API int32_t aver_phys_set_surface_id_sampler(aver_phys_surface_id_sampler_fn fn, void* user);
AVER_PHYS_API void    aver_phys_clear_surface_id_sampler(void);

/* ---- collision groups for ordinary bodies ---- */
AVER_PHYS_API int32_t aver_phys_group_create(void);                                  /* a fresh id > 0, never reused in this world */
AVER_PHYS_API int32_t aver_phys_group_destroy(int32_t group);                        /* members become group 0 */
AVER_PHYS_API int32_t aver_phys_body_set_group(int32_t body, int32_t group, int32_t subGroup);   /* group 0 = none; sub-group 0..31 */
AVER_PHYS_API int32_t aver_phys_body_group(int32_t body, int32_t* outGroup, int32_t* outSubGroup);
AVER_PHYS_API int32_t aver_phys_group_set_self_collision(int32_t group, int32_t on);            /* default 0: members do not collide */
AVER_PHYS_API int32_t aver_phys_group_set_subgroup_collision(int32_t group, int32_t subA, int32_t subB, int32_t enabled);   /* symmetric; only matters when self collision is off */

/* ---- per-body Jolt flags ---- */
#define AVER_PHYS_BF_GYROSCOPIC          0x01u  /* gyroscopic force (tumbling bodies; default off) */
#define AVER_PHYS_BF_MANIFOLD_REDUCTION  0x02u  /* merge coplanar contacts from different sub-shapes (default ON; off = exact faces in events) */
#define AVER_PHYS_BF_EDGE_REMOVAL        0x04u  /* ghost-contact removal on mesh edges (default off) */
#define AVER_PHYS_BF_DETECT_STATIC       0x08u  /* sensors only: also detect static bodies (the sensor becomes kinematic and stays awake) */
AVER_PHYS_API int32_t aver_phys_body_set_flags(int32_t body, uint32_t mask, uint32_t value);   /* bits in mask take the matching bit of value */
AVER_PHYS_API int32_t aver_phys_body_flags(int32_t body, uint32_t* outFlags);
AVER_PHYS_API int32_t aver_phys_set_default_body_flags(uint32_t mask, uint32_t value);          /* for bodies created from now on (addBody applies it) */
```

#### 2.4.1 Pair ignore and teams: a `SimShapeFilter`

`AverSimShapeFilter::ShouldCollide(b1, s1, id1, b2, s2, id2)`: `ta`, `tb` from the tags; `if (((ta.rule | tb.rule) & (kRulePairIgnore)) == 0 && (ta.team == 0 || tb.team == 0)) return true;` (the whole cost for an unruled pair); then the ignore table (`unordered_map<u64 key, i64 untilStep>`, key from the two handles, looked up only when a pair-ignore bit is set on either body) and the team matrix (`u32 teamPass[32]`). Installed with `SetSimShapeFilter` at init; the filter is asked for the CCD path too (`PhysicsSystem.cpp:2047`), which is why it is preferred to rejecting in `OnContactValidate`. Expiry: `contactsStepBegin` advances `stepIndex` and erases entries whose `untilStep` is reached, clearing the body bit when a body's last entry goes; entries are removed with their bodies. **Time-limited, not "until separated"**: Jolt does not tell us when an ignored pair stops overlapping (open question 10). UNVERIFIED that the filter is consulted for a sensor pair; the sensor-pair test in section 5 pins it, and if it is not, `OnContactValidate` rejects the same rules for sensors only.

#### 2.4.2 One-way: `OnContactValidate`

```cpp
ValidateResult contactsOnValidate(const Body& b1, const Body& b2, RVec3Arg, const CollideShapeResult& r) {
  const BodyTag &t1 = tagOf(b1), &t2 = tagOf(b2);
  if (!((t1.rule | t2.rule) & kRuleOneWay)) return ValidateResult::AcceptAllContactsForThisBodyPair;   // the fast exit
  // for each one-way body P (the platform) and its partner Q:
  //   up   = P.GetRotation() * localUp(P);  n = contact normal oriented from P to Q, from -r.mPenetrationAxis
  //   solid side: dot(n, up) >= cos(maxAngle)  AND  relVelAlongUp = dot(vQ(p) - vP(p), up) <= 5 cm/s  AND  depth <= maxPenetration
  // all rules satisfied -> AcceptContact; any not -> RejectContact (never RejectAll: every manifold is re-asked next step)
}
```
Sign: Jolt's `CollideShape.h:61` says `-mPenetrationAxis` is the contact normal while `ContactManifold::mWorldSpaceNormal` is documented as the direction to move body 2 out; the two conventions are therefore pinned by `ContactRulesTest` (a ball dropped onto a platform lands, a ball thrown up through it passes, a ball hitting its edge passes) rather than assumed. A resting contact is not re-validated while it persists (`ContactListener.h:63`), which is the behaviour a body standing on the platform needs. 60 degrees, 5 cm/s and 10 cm are UNVERIFIED starting values.

#### 2.4.3 Apparent mass and surface velocity

In Added and Persisted, after the terrain sampler: `io.mInvMassScale1 = 1 / massScale(b1)` (0 for INFINITE) and the same for 2 and for inertia; with both flagged the product is not taken, each body's own scale applies. Conveyor: for a body with a surface velocity `s` (world, or `R_body * s` when local), `io.mRelativeLinearSurfaceVelocity += (b2 is the conveyor ? +s : -s)` because Jolt defines it as surface velocity of body 2 minus that of body 1 (`ContactListener.h:52`); angular likewise, remembering Jolt's "relative to the centre of mass of body 1" (the correction term is in the comment there). Which body is body 1 is Jolt's order (lower id first for Added and Persisted), so the sign test uses both creation orders. Static bodies may carry a surface velocity (the usual belt).

#### 2.4.4 Surfaces and combine

`surface_define` stores friction, restitution and two modes in a 256-entry table; `body_set_surface` copies friction and restitution onto the body through the existing setters and stores the id and modes in the tag. The installed combine function (a plain function, `float(const Body&, const SubShapeID&, const Body&, const SubShapeID&)`) is:

```
mode = precedence(modeOf(b1), modeOf(b2))          // Max > Multiply > Min > Average > Default
Default  : sqrt(f1 * f2)  /  max(r1, r2)           // identical expressions to Jolt's, so float results match bit for bit
Average  : (f1 + f2) / 2
Min / Max / Multiply : min / max / f1 * f2
```
then, in Added and Persisted, a `surface_pair(sa, sb)` override replaces both values (the terrain sampler wins over it when a body is surface-sampled). The global default modes come from `aver_phys_set_default_combine`; a body whose mode is 0 follows it. Lazy install: neither function pointer is set until a non-default mode, surface or default is first requested, and they are restored to Jolt's when everything is back to default, so a project that never calls any of this runs Jolt's own lambdas. The wheel combine of vehicles (`PhysicsVehicle.cpp:510`) is a separate function and is untouched.

#### 2.4.5 Groups

One `AverGroupFilter : JPH::GroupFilter` per world. `CanCollide(g1, g2)`: ids differ -> true; same id -> `GroupState`: `selfCollision` -> true, else true only if the sub-group pair is enabled in a 32x32 bit table. User group `n` maps to Jolt group id `0x40000000 + n`; `BodyInterface::SetCollisionGroup(id, CollisionGroup(&filter, id, sub))`. A rebuilt body keeps it (it travels in `BodyCreationSettings`, `PhysicsBody.cpp:107`). Interaction with the ragdoll: a ragdoll part's filter is its own `GroupFilterTable`, whose `CanCollide` says "different group ids collide", and Aver ids sit far above any ragdoll id (`g_nextRagdoll` counts from 1), so a grouped crate and a ragdoll collide as before. Groups act in the simulation only; **queries and `CharacterVirtual` movement do not consult `CollisionGroup`** (Jolt applies it in the broad-phase pair finder), so queries get group ignores from the filter object (2.5) and characters from the listener below.

#### 2.4.6 Terrain surface ids

`aver_phys_set_surface_id_sampler` is the id twin of the friction sampler (`physics_water_abi.h`): a pure function of world XY, called on worker threads for contacts with a surface-sampled body, filling `SURFACE_B` (or A) of records and the surface-pair override. The landscape-layer-to-id mapping belongs to the landscape area (open question 9).

#### 2.4.7 Characters

`CharacterVirtual` has its own contact path. `characterCreated` installs one `CharacterContactListener` per world whose `OnContactValidate(character, bodyID, subShapeID)` applies pair-ignore, teams and groups (a character's group and team live in its tag, reached by handle) and returns false to skip the contact. One-way and conveyors are not applied to characters: a conveyor that moves a walker needs `mCanPushCharacter` / ground velocity work that is not a contact setting (open question 8). Characters keep their layer filtering exactly as `CharacterFilter` does.

#### 2.4.8 Flags and kinematic sensors

`body_set_flags` uses `BodyInterface::SetUseManifoldReduction` (so the contact cache is invalidated) and `Body::SetApplyGyroscopicForce` / `SetEnhancedInternalEdgeRemoval` under a `BodyLockWrite`. `AVER_PHYS_BF_DETECT_STATIC` is accepted only on a sensor: it calls the existing `aver_phys_body_set_motion_type(KINEMATIC)` path (which rebuilds a static body, `PhysicsBody.cpp:190`), `Body::SetCollideKinematicVsNonDynamic(true)`, `set_allow_sleeping(0)` and activates it, because Jolt's rule is "the sensor must be kinematic and active" (`Body.h:82-85`) and warns the extra collision work "can be CPU intensive" (one warning per call). The sensor's user layer must collide with the layers it should see; the existing matrix decides. Turning it off puts the sensor back to static.

### 2.5 Filters, lookup and activation: `physics_filters_abi.h`

New header, sources `PhysicsQueryFilterObj.cpp` and `PhysicsActivation.cpp`. `AVER_PHYS_FILTERS_ABI_VERSION 1`.

```c
#define AVER_PHYS_FILTERS_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_filters_abi_version(void);

/* ---- query filter objects ---- */
AVER_PHYS_API int32_t aver_phys_qf_create(void);                                   /* handle > 0; 0 if 64 are live */
AVER_PHYS_API int32_t aver_phys_qf_destroy(int32_t filter);
AVER_PHYS_API int32_t aver_phys_qf_reset(int32_t filter);                          /* mask = all layers, flags = default, nothing ignored */
AVER_PHYS_API int32_t aver_phys_qf_set_layer_mask(int32_t filter, uint32_t layerMask);
AVER_PHYS_API int32_t aver_phys_qf_set_flags(int32_t filter, int32_t flags);       /* AVER_PHYS_QUERY_* of physics_queries_abi.h */
AVER_PHYS_API int32_t aver_phys_qf_ignore_body(int32_t filter, int32_t body, int32_t on);        /* body or character handle */
AVER_PHYS_API int32_t aver_phys_qf_ignore_vehicle(int32_t filter, int32_t vehicle, int32_t on);  /* its chassis body */
AVER_PHYS_API int32_t aver_phys_qf_ignore_ragdoll(int32_t filter, int32_t ragdoll, int32_t on);  /* every part */
AVER_PHYS_API int32_t aver_phys_qf_ignore_group(int32_t filter, int32_t group, int32_t on);      /* every member, now and later */
AVER_PHYS_API int32_t aver_phys_qf_ignore_entity(int32_t filter, int32_t entity, int32_t on);    /* every body stamped with it */
AVER_PHYS_API int32_t aver_phys_qf_ignore_subshape(int32_t filter, int32_t body, int32_t subShapeIndex, int32_t on);   /* one child of a compound */
AVER_PHYS_API int32_t aver_phys_subshape_index(int32_t body, uint32_t subShapeId);              /* child index, or -1 */

/* ---- the nine filtered queries: the existing arguments, minus layerMask / ignoreBody / flags, plus `filter` ---- */
AVER_PHYS_API int32_t aver_phys_raycast_f(int32_t filter, float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                          float* outPoint, float* outNormal, int32_t* outEntity);
AVER_PHYS_API int32_t aver_phys_raycast_all_f(int32_t filter, float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                              int32_t maxCount, int32_t* outBodies, float* outDistances, float* outPoints, float* outNormals);
AVER_PHYS_API int32_t aver_phys_sphere_cast_f(int32_t filter, float ox, float oy, float oz, float dx, float dy, float dz, float radius,
                                              float* outDistance, float* outPoint, float* outNormal);
AVER_PHYS_API int32_t aver_phys_box_cast_f(int32_t filter, float cx, float cy, float cz, float hx, float hy, float hz,
                                           float qx, float qy, float qz, float qw, float dx, float dy, float dz,
                                           float* outDistance, float* outPoint, float* outNormal);
AVER_PHYS_API int32_t aver_phys_capsule_cast_f(int32_t filter, float cx, float cy, float cz, float radius, float height,
                                               float qx, float qy, float qz, float qw, float dx, float dy, float dz,
                                               float* outDistance, float* outPoint, float* outNormal);
AVER_PHYS_API int32_t aver_phys_overlap_sphere_f(int32_t filter, float x, float y, float z, float radius, int32_t* outBodies, int32_t maxCount);
AVER_PHYS_API int32_t aver_phys_overlap_box_f(int32_t filter, float cx, float cy, float cz, float hx, float hy, float hz,
                                              float qx, float qy, float qz, float qw, int32_t* outBodies, int32_t maxCount);
AVER_PHYS_API int32_t aver_phys_overlap_capsule_f(int32_t filter, float cx, float cy, float cz, float radius, float height,
                                                  float qx, float qy, float qz, float qw, int32_t* outBodies, int32_t maxCount);
AVER_PHYS_API int32_t aver_phys_point_query_f(int32_t filter, float x, float y, float z, int32_t* outBodies, int32_t maxCount);

/* ---- entity and body ---- */
AVER_PHYS_API int32_t aver_phys_bodies_of_entity(int32_t entity, int32_t* outBodies, int32_t maxCount);   /* count; bodies and characters stamped with it */
AVER_PHYS_API int32_t aver_phys_body_of_entity(int32_t entity);                                          /* the first (lowest handle), or 0 */

/* ---- awake bodies ---- */
AVER_PHYS_API int32_t aver_phys_active_count(void);                                  /* awake rigid bodies, dynamic and kinematic */
AVER_PHYS_API int32_t aver_phys_active_bodies(int32_t* outBodies, int32_t maxCount); /* handles, ascending; those without a handle (character inner bodies) are skipped */
AVER_PHYS_API int32_t aver_phys_active_poses(int32_t dynamicOnly, int32_t maxCount, int32_t* outBodies, int32_t* outEntities, float* outPose7);
                       /* per body: x y z qx qy qz qw (centre of mass, as aver_phys_body_position / _rotation); one pass, no per-body call */
AVER_PHYS_API int32_t aver_phys_body_poses(const int32_t* bodies, int32_t count, float* outPose7);   /* the same for a given list; 0 written for a dead handle */
AVER_PHYS_API int32_t aver_phys_activate_in_box(float cx, float cy, float cz, float hx, float hy, float hz, uint32_t layerMask);   /* wakes sleeping bodies; returns 1 */
```

#### 2.5.1 The filter object

`QueryFilterObj` lives in `World::queryFilters` (64 slots). Its `BodyFilter` side is `ShouldCollide(BodyID)` for ignored ids (small sorted vector, binary search) and `ShouldCollideLocked(Body&)` for what needs the body: sensors (per flags, as `QueryBodyFilter`), ignored **groups** (`body.GetCollisionGroup().GetGroupID() == 0x40000000 + g`), ignored **entities** (`GetUserData()` low 32 bits). Its `ShapeFilter` side (only built when a sub-shape ignore exists) stores `(BodyID, const Shape*)` of the ignored child (from the compound's `GetSubShape(i).mShape`, resolved when the ignore is added) and returns false for that shape; a child `Shape` object shared by several identical compound children is ignored for all of them, documented and pinned by a test. The `_f` queries use the collector overloads of `CastRay`, `CastShape`, `CollideShape` and `CollidePoint` (which accept a `ShapeFilter`) with `ClosestHitCollisionCollector` or `AllHitCollisionCollector`, and otherwise reuse the code of `PhysicsQueries.cpp`; they return exactly what the matching `_ex` or `queries` call returns when the filter has the same mask, ignore and flags (a property test over random rays, section 5).

#### 2.5.2 Activation

`ActivationListener : JPH::BodyActivationListener` is installed at init; its two callbacks (any thread, the body locked) read the tag and, for a body whose mask has `WAKE_SLEEP`, push `{kind, handle}` under the mutex. `contactsStepEnd` turns them into WAKE and SLEEP stream records (sorted, with `STEP` and `SEQ`) and feeds the pair table (2.3.2). `aver_phys_active_bodies` and `_active_poses` use `PhysicsSystem::GetActiveBodies(EBodyType::RigidBody, ids)` into a reused vector and `BodyLockInterfaceNoLock` after the step (no writer exists between steps), so a frame with 40 awake bodies out of 3000 reads 40 poses. `activate_in_box` converts the box through `toJolt` and `abs`, as the other box queries do, and uses a `MaskLayerFilter`.

### 2.6 Force volumes: `physics_forcevolumes_abi.h`

New header, source `PhysicsForceVolumes.cpp`. `AVER_PHYS_FORCEVOLUMES_ABI_VERSION 1`. Evaluated once per fixed step in `forceVolumesStepBegin(dt)`, called from `aver_phys_step` between `vehicleStepBegin` and `system.Update` (`PhysicsWorld.cpp:489-490`), so the force is part of that step and the result does not depend on the frame rate.

```c
#define AVER_PHYS_FORCEVOLUMES_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_forcevolumes_abi_version(void);

#define AVER_PHYS_FV_GRAVITY 0   /* replaces gravity inside the volume (acceleration, cm/s^2) */
#define AVER_PHYS_FV_WIND    1   /* pulls velocity towards a target air velocity: a = k * (vWind - v) */
#define AVER_PHYS_FV_FORCE   2   /* adds an acceleration, directional or radial */
#define AVER_PHYS_FVS_SPHERE 0
#define AVER_PHYS_FVS_BOX    1
#define AVER_PHYS_FVS_ALL    2   /* everywhere */

/* parameter block AVER_PHYS_FVP_*: 0 SHAPE, 1..3 CENTRE (cm), 4..6 EXTENTS (sphere radius = x; box half extents), 7..10 ROTATION (qx qy qz qw),
   11..13 VECTOR (gravity cm/s^2, wind velocity cm/s, force direction * cm/s^2), 14 STRENGTH (radial magnitude cm/s^2; wind drag k in 1/s), 15 RADIAL (0/1:
   gravity towards the centre, force away from it), 16 FALLOFF (0 none, 1 linear to the edge, 2 quadratic), 17 TURBULENCE (0..1), 18 PRIORITY, 19 LAYER_MASK
   (0 = all), 20 ALIGN_CHARACTER_UP (gravity only), 21 WAKE (1 = wake sleeping bodies when created, enabled or changed), COUNT 22 */
AVER_PHYS_API int32_t aver_phys_fv_create(int32_t kind, const float* p, int32_t n);        /* handle > 0, 0 refused; n may be shorter than COUNT */
AVER_PHYS_API int32_t aver_phys_fv_update(int32_t volume, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_fv_set_pose(int32_t volume, const float* posXyz, const float* rotQuat);   /* a volume that follows an entity */
AVER_PHYS_API int32_t aver_phys_fv_set_enabled(int32_t volume, int32_t on);
AVER_PHYS_API int32_t aver_phys_fv_destroy(int32_t volume);
AVER_PHYS_API int32_t aver_phys_fv_count(void);
AVER_PHYS_API int32_t aver_phys_fv_body_count(int32_t volume);                              /* bodies it acted on in the last fixed step */

AVER_PHYS_API int32_t aver_phys_body_set_gravity_vector(int32_t body, int32_t on, float gx, float gy, float gz);   /* body or character handle, cm/s^2 */
AVER_PHYS_API int32_t aver_phys_body_set_wind_response(int32_t body, float scale);          /* how strongly wind and force volumes act on it; 1 default, 0 immune */
AVER_PHYS_API int32_t aver_phys_gravity_at(float x, float y, float z, float* outGravityXyz);   /* the world gravity or the winning gravity volume's, cm/s^2 */
AVER_PHYS_API int32_t aver_phys_radial_impulse(float x, float y, float z, float radiusCm, float velocityChangeCmS, float falloff, float upBias, uint32_t layerMask);
                       /* one shot: every dynamic body in the sphere gets a velocity change away from the point (upBias lifts the direction), scaled by falloff
                          (0 none, 1 linear, 2 quadratic) (a velocity change, not a force: a crate and a car both leave at that speed);
                          returns the number of bodies hit; sleeping bodies wake */
```

**Evaluation.** One pass over the awake rigid bodies (`GetActiveBodies`, reused vector). For each dynamic body with a nonzero tag-visible rule bit or at least one enabled volume whose AABB contains its centre: sum the contributions in volume-handle order, then `BodyInterface::AddForce(id, F, EActivation::DontActivate)` once. GRAVITY: the highest `PRIORITY` volume containing the centre wins (ties: lowest handle); the per-body gravity vector, if set, wins over any volume; `F = m * (g_new - g_world * gravityFactor)` with `m` from `MotionProperties::GetInverseMass()`. WIND: `F = m * k * windScale * (vWind - v)`, plus turbulence `T * |vWind| * hashNoise(stepIndex, handle, axis)` where the noise is an integer hash of its arguments, so a replay is identical and no clock is read. FORCE: `F = m * windScale * a`, with radial direction from the volume centre and the falloff law. Sleeping bodies receive nothing unless the volume was created, enabled or updated with `WAKE`, which runs `ActivateBodiesInAABox` once on its bounds; a body that is asleep in a field stays asleep, which is the price of not waking the world every step (documented; `aver_phys_body_set_allow_sleeping(0)` is the escape).

**Characters.** The step loop's `gravity` for a character becomes `forceVolumeGravityFor(handle, position, base)`: the per-character vector, else the winning volume's, else `base * factor`. If the winning volume has `ALIGN_CHARACTER_UP`, `CharacterBase::SetUp(-normalize(g))` is applied before `ExtendedUpdate` so slope and ground tests follow the planet; otherwise up is left alone. The `characterCarry` logic (ground velocity) is orthogonal and untouched. One-line change in the shared step loop (`PhysicsWorld.cpp:418-423`).

**Budget.** At most 64 volumes and 4096 affected bodies per step (the rest are skipped with one warning per call). UNVERIFIED cost: O(awake bodies x volumes) AABB tests; section 6 item 12 measures it.

### 2.7 C#: `Aver.Physics`

New files, every P/Invoke name and parameter name copied character for character from the headers (`Native.BodyExt.cs` rule), so `AbiEnumTest` pairs them: `Native.Contacts.cs`, `Native.Rules.cs`, `Native.Filters.cs`, `Native.ForceVolumes.cs`; public surface in `Contacts.cs`, `BodyContacts.cs` (`partial struct Body`), `Rules.cs`, `QueryFilter.cs`, `ForceVolumes.cs`; index enums in `Contacts.cs` and `ForceVolumes.cs` named like the vehicle ones (`CeiIndex`, `CefIndex`, `FvpIndex`, `ContactStat`).

```csharp
public enum ContactKind { Added = 1, Persisted = 2, Removed = 3, Wake = 4, Sleep = 5 }
[Flags] public enum ContactFlags { PairBegin = 1, PairEnd = 2, SleepRemoval = 4, BodyRemoved = 8, Character = 16, CrudeImpulse = 32, Synthetic = 64 }
[Flags] public enum BodyEvents : uint { None = 0, Hit = 1, Touch = 2, Stay = 4, WakeSleep = 8, All = 15 }
public enum CombineMode { Default = 0, Average = 1, Min = 2, Multiply = 3, Max = 4 }
[Flags] public enum BodyFlags : uint { Gyroscopic = 1, ManifoldReduction = 2, EdgeRemoval = 4, DetectStatic = 8 }
public enum ForceVolumeKind { Gravity = 0, Wind = 1, Force = 2 }
public enum ForceVolumeShape { Sphere = 0, Box = 1, All = 2 }

/// <summary>One record of the new contact stream. Normal points from A towards B; ApproachSpeed is positive while closing.</summary>
public readonly record struct ContactRecord(ContactKind Kind, ContactFlags Flags, Body A, Body B, int EntityA, int EntityB,
    uint SubShapeA, uint SubShapeB, int SurfaceA, int SurfaceB, int Step, int Seq, int Sides,
    Float3 Point, Float3 Normal, float PenetrationCm, Float3 RelativeVelocity, float ApproachSpeed,
    float NormalImpulse, float FrictionImpulse, float SlideSpeed, float Friction, float Restitution)
{
    /// <summary>The same contact seen from <paramref name="self"/>: bodies, entities and surfaces swapped and the normal and relative velocity negated when self is B.</summary>
    public ContactRecord ViewFrom(Body self);
}

public static partial class Physics
{
    public static int ContactRecordCount { get; }   public static int ContactRecordsDropped { get; }
    /// <summary>Copies records [first, first + buffer.Length) of the current step call into the buffer; returns how many.</summary>
    public static int ReadContactRecords(Span<ContactRecord> buffer, int first = 0);
    public static IReadOnlyList<ContactRecord> ContactRecords { get; }       // allocating convenience, like Contacts
    public static bool SetContactLegacy(bool on);   public static bool SetContactStayPeriod(int steps);   public static bool SetContactCapacity(int records);
    public static bool BodiesInContact(Body a, Body b);
    public static bool EstimateImpact(Body a, Body b, Float3 point, Float3 normalAToB, out ImpactEstimate estimate);
    public static bool DefineSurface(int surface, float friction, float restitution, CombineMode frictionMode = CombineMode.Default, CombineMode restitutionMode = CombineMode.Default);
    public static bool SetSurfacePair(int a, int b, float friction, float restitution);   public static bool ClearSurfacePair(int a, int b);
    public static bool SetDefaultCombine(CombineMode friction, CombineMode restitution);  public static void ResetSurfaces();
    public static bool SetTeamPass(int teamA, int teamB, bool pass);   public static bool TeamPass(int teamA, int teamB);   public static void ResetTeamPass();
    public static int CreateGroup();   public static bool DestroyGroup(int group);
    public static bool SetGroupSelfCollision(int group, bool on);   public static bool SetSubGroupCollision(int group, int subA, int subB, bool enabled);
    public static bool SetDefaultBodyFlags(BodyFlags mask, BodyFlags value);
    public static int ActiveBodyCount { get; }   public static int ReadActiveBodies(Span<Body> buffer);
    public static int ReadActivePoses(Span<ActivePose> buffer, bool dynamicOnly = true);   // ActivePose(Body, int Entity, Float3 Position, Quaternion Rotation)
    public static bool ActivateInBox(Float3 centre, Float3 halfExtents, uint layerMask = LayerMaskAll);
    public static Body BodyOfEntity(int entity);   public static int BodiesOfEntity(int entity, Span<Body> buffer);
    public static ForceVolume CreateForceVolume(ForceVolumeKind kind, in ForceVolumeSettings settings);
    public static int RadialImpulse(Float3 centre, float radiusCm, float velocityChangeCmS, float falloff = 1f, float upBias = 0f, uint layerMask = LayerMaskAll);
    public static Float3 GravityAt(Float3 point);
}
public readonly partial struct Body   // BodyContacts.cs
{
    public bool SetEvents(BodyEvents events, float minHitImpulse = 0f, float minHitSpeedCmS = 30f);   public BodyEvents Events { get; }
    public bool SetSurface(int surface);   public int Surface { get; }   public bool SetCombine(CombineMode friction, CombineMode restitution);
    public bool SetTeam(int team);   public int Team { get; }
    public bool SetGroup(int group, int subGroup = 0);   public bool IgnoreBody(Body other, float seconds = -1f);   public bool ClearIgnores();
    public bool SetOneWay(bool on, Float3 localUp = default, float maxAngleDeg = 60f, float maxPenetrationCm = 10f);
    public bool SetContactScale(float massScale, float inertiaScale = 1f);   public bool SetInfiniteContactMass();
    public bool SetSurfaceVelocity(Float3 linearCmS, Float3 angularRadS = default, bool local = false);
    public BodyFlags Flags { get; }   public bool SetFlags(BodyFlags mask, BodyFlags value);
    public bool SetGravityVector(Float3 cmS2);   public bool ClearGravityVector();   public bool SetWindResponse(float scale);
    public Float3 PointVelocity(Float3 worldPoint);
}
public sealed class QueryFilter : IDisposable   // QueryFilter.cs: a handle with a finaliser backstop; Dispose destroys it
{
    public QueryFilter(uint layerMask = Physics.LayerMaskAll, QueryFlags flags = QueryFlags.None);
    public QueryFilter IgnoreBody(Body b);   public QueryFilter IgnoreVehicle(Vehicle v);   public QueryFilter IgnoreRagdoll(Ragdoll r);
    public QueryFilter IgnoreGroup(int group);   public QueryFilter IgnoreEntity(int entity);   public QueryFilter IgnoreSubShape(Body b, int childIndex);
    public RaycastHit Raycast(Float3 origin, Float3 direction, float maxDistanceCm);   public int RaycastAll(Float3 o, Float3 d, float max, Span<RayHit> hits);
    public Body SphereCast(...);  public Body BoxCast(...);  public Body CapsuleCast(...);
    public int OverlapSphere(Float3 c, float r, Span<Body> results);  public int OverlapBox(...);  public int OverlapCapsule(...);  public int PointQuery(Float3 p, Span<Body> results);
}
```
Everything fails to `false`, `0` or `default`, never throws, with the library absent (`NativeResolver` as today). A `QueryFilter` is not thread-safe and not shareable across threads.

### 2.8 `Aver.Physics.Scene` (`modules/physics.scene`)

`module.json`: `"name":"Aver.Physics.Scene"`, `"dir":"physics.scene"`, `"option":"AVER_MODULE_PHYSICS_SCENE"`, `"category":"Physics"`, default on, `dependsOn: Aver.Core, Aver.Physics, Aver.Scene`, consequence "no contact events reach graphs or actors, Contact Settings and Force Volume components do nothing, and dynamic Rigid Body entities do not follow their bodies". Added in the root `CMakeLists.txt` in the scene block beside `vehicle.scene` (`:317-322`), `tests/physics.scene` beside `:592-596`. Public macro `AVER_MODULE_PHYSICS_SCENE=1`. `cmake/modules.catalog.json` regenerated with `scripts/module-catalog.py`; `scripts/module-guard-audit.py` run. Namespace `aver::physicsscene`. It links none of audio, particles, framework, camera or soft bodies.

#### 2.8.1 Components (registered at runtime; no `Components.hpp` growth, the `CVehicle` pattern)

```cpp
// CContactSettings ("ContactSettings"): 64 bytes, zero-filled = every default
inline constexpr u32 kContactOneWay = 0x01, kContactConveyor = 0x02, kContactConveyorLocal = 0x04, kContactGyroscopic = 0x08,
                     kContactNoManifoldReduction = 0x10, kContactEdgeRemoval = 0x20, kContactDetectStatic = 0x40;   // negative sense where the default is on
struct CContactSettings {
  u32 events = 0;            //  0 BodyEvents bits
  u32 flags = 0;             //  4
  f32 minHitImpulse = 0;     //  8
  f32 minHitSpeedCmS = 0;    // 12   0 reads 30
  u16 surface = 0; u8 team = 0, subGroup = 0;   // 16
  i32 group = 0;             // 20   0 none; the component maps a positive number to a world group on first use
  f32 massScale = 0, inertiaScale = 0;          // 24, 28   0 reads 1
  f32 surfaceVelocity[3] = {0,0,0};             // 32       cm/s
  f32 oneWayUp[3] = {0,0,0};                    // 44       body-local; zero reads +Z
  f32 maxAngleDeg = 0;       // 56   0 reads 60
  i32 outBody = 0;           // 60 OUT: the body handle these settings were applied to
};
static_assert(sizeof(CContactSettings) == 64, "CContactSettings must be padding-free");

// CForceVolume ("ForceVolume"): 64 bytes
inline constexpr u32 kForceVolumeOff = 0x1, kForceVolumeAlignUp = 0x2, kForceVolumeRadial = 0x4, kForceVolumeWake = 0x8;
struct CForceVolume {
  u32 kind = 0, shape = 0, flags = 0;  i32 priority = 0;     //  0..15
  f32 extents[3] = {0,0,0};                                   // 16     sphere radius = [0]
  f32 vector[3] = {0,0,0};                                    // 28
  f32 strength = 0, falloff = 0, turbulence = 0;              // 40, 44, 48
  u32 layerMask = 0;                                          // 52    0 reads all
  i32 outVolume = 0;                                          // 56 OUT
  u32 reserved = 0;                                           // 60
};
static_assert(sizeof(CForceVolume) == 64, "CForceVolume must be padding-free");
```
Both register with `registerComponent<T>(name).field(...).verify(sizeof)`; OUT fields are read-only. Scripts change them through the generic scene field ABI, so no relay ABI is needed for settings.

#### 2.8.2 System

```cpp
class PhysicsSceneSystem {
 public:
  static u32 registerComponents(scene::World&);
  void install(scene::World&);  void uninstall();
  void setEventSink(EventSinkFn, void*);        // (name, targetEntity, otherEntity, const PhysicsEvent&) -> the host posts to the framework bus
  void preStep(scene::World&, f32 dt);          // before aver_phys_step, after the PRE_PHYSICS group
  void postStep(scene::World&, f32 dt, i32 steps);   // after it, before the PHYSICS group; `steps` = what aver_phys_step returned
  void clear();                                 // volumes and applied tags forgotten; bodies are not touched
  std::vector<scene::Entity> entities() const;  // body-driven entities, for PlayMobility::seedMovable and the mid-Play save set
  Stats stats() const;
};
PhysicsSceneSystem& physicsSceneSystem();       // process singleton, like vehicle::vehiclePhysicsSystem()
```
Every loop is ascending by entity id (the determinism contract).

**preStep.** (1) For each entity with `CContactSettings`: find its body (`CRigidBody.body`, else `aver_phys_body_of_entity`, else a vehicle chassis from `CVehicle.vehicle`); compare the component bytes with the copy applied last time; if changed, push the differences through the ABI (events, surface, team, group, scales, conveyor, one-way, flags) and write `outBody`. A group number in the component is mapped to a world group the first time it is seen (`group_create` once per number, kept in a small table cleared by `clear()`). Removal of the component or the entity resets the body to defaults. At most 64 applications per frame so a level placing 500 settings does not hitch. (2) For each `CForceVolume`: create the volume lazily, update it when the component changes, and `fv_set_pose` from the entity's world transform every frame it moved; destroy it with the component.

**postStep.** (3) **Pump** (2.8.3). (4) **Pose write-back** (2.8.4).

#### 2.8.3 The pump

Reads the new stream with `aver_phys_cev_get` in one bulk call, and the sensor queue with `aver_phys_overlap_get`. The pump remembers the highest `SEQ` it has posted and skips anything at or below it, so a tick that runs without a fresh `aver_phys_step` in between (the stream is cleared only when a step call begins) never delivers a record twice; the sensor queue, which has no `SEQ`, is read only when the step count of this frame's `aver_phys_step` is nonzero. Per record, for each side with a nonzero entity and an asking mask: map the kind to an event name and a payload, apply the pair cooldown to HIT (default 0.10 s per ordered entity pair, in fixed-step units so it is frame-rate independent), and call the sink. The sink installed by the host posts to the framework bus: `aver_fw_event_push_*` then `aver_fw_event_post(name, sender, target)` with `sender` = the other entity and `target` = the receiving entity. Events are posted, not dispatched: they are delivered at the next frame's `aver_fw_events_flush` (before PRE_PHYSICS), one frame after the contact, which keeps handlers out of the middle of the tick (`docs/GAME_TIMERS_EVENTS.md` "Flush"). `Options::dispatchImmediately` (default off) calls `aver_fw_event_dispatch` instead for projects that want same-frame handlers.

| Event name (target = receiver) | Fires for | Payload index: value |
|---|---|---|
| `Physics.Hit` | ADDED record that passed the receiver's thresholds | 0 other entity, 1 normal impulse (kg*cm/s), 2 approach speed (cm/s), 3 point (Vec3), 4 normal (Vec3, pointing out of the receiver), 5 surface of other, 6 own surface, 7 penetration (cm), 8 slide speed |
| `Physics.BeginTouch` | ADDED with `PAIR_BEGIN` | as Hit |
| `Physics.EndTouch` | REMOVED with `PAIR_END` | 0 other entity, 5 surface of other (0 when unknown), 9 reason (0 separated, 1 body removed) |
| `Physics.Stay` | PERSISTED | as Hit (impulse is 0) |
| `Physics.Wake`, `Physics.Sleep` | WAKE, SLEEP | 0 the entity itself |
| `Physics.OverlapBegin`, `Physics.OverlapEnd` | the sensor queue, to the sensor's entity and, when its body asks for TOUCH, the other's | 0 other entity |

A record whose receiver has entity 0 (terrain, level geometry) is not posted; the other side is. `AN_EventPayload` coerces index 0..2, 5..9 as int or float; the Vec3 entries (3, 4) are read in C# with `EventData.GetVec3` and in graphs through the pure node `an_physlasthit` (2.9). `CContactSettings.events` is the only thing a level author sets to receive any of this; a script may also call `Body.SetEvents`.

#### 2.8.4 Pose write-back

After the pump, `aver_phys_active_poses(dynamicOnly = 1, ...)` into a reused buffer; for each entry whose entity is valid, has a `CRigidBody` (so vehicles, characters and soft bodies keep their own writers) and is not marked kinematic-driven, convert the centre-of-mass pose to the entity's origin (`CRigidBody` shapes are built centred on the entity's origin by `PhysicsSceneSync`, so the offset is zero today; the system subtracts `shapeCom - origin` when a body reports one through `aver_phys_body_aabb`-derived data, open question 4), then write `CLocal` through the parent-relative conversion `VehicleSystem` uses (`VehicleSystem.hpp:239-245`), only when the value changed beyond 0.01 cm or 0.01 degrees. A body that falls asleep is written one last time (its SLEEP is in the same batch), then costs nothing. Static and kinematic bodies are never written (the entity drives them: `driveKinematicBodies`, `SandboxPlay.cpp:975-1010`). No interpolation: the entity shows the pose of the last fixed step, as vehicles do (open question 3). `entities()` lists the body-driven entities for `PlayMobility::seedMovable` (a simulated crate must not cost a GI rebuild per move) and for the mid-Play save set (`SandboxLevelEdit.cpp:620-640`), so autosave never writes a crate's mid-fall pose.

#### 2.8.5 Relocated creation

`PhysicsSceneSync.cpp`'s two functions (`syncPhysicsFromScene`, `clearPhysicsFromScene`) move, body for body, into `modules/physics.scene/src/BodyBuild.cpp` as `physicsscene::buildFromScene(world)` and `clearFromScene(world)`. The sandbox file is **kept** and each of its two functions becomes a one-line forward (never delete files; the callers `SandboxPlay.cpp:152,686` do not change). The packaged runtime gains the call it never had: `GameApp` calls `buildFromScene` once after the level and its components are applied and `clearFromScene` where it clears the vehicles (`GameApp.cpp:2471`). This is the missing half that the pose write-back needs to mean anything in a shipped game (open question 2).

### 2.9 Framework C# and Aver Node

**Framework (`Aver.Framework`, additive; the integrator bumps its version once for the wave).** `PhysicsEvents.cs` (event-name constants, `PhysicsHit`, `PhysicsTouch` structs decoded from `EventData`, and the actor binder), `EntityPhysics.cs` (`partial struct Entity`: `PhysicsBody`, `SetPhysicsEvents`, `SetSurface`, `SetTeam`, `SetCollisionGroup`, `IgnoreCollisionWith(Entity, seconds)`, `SetOneWay`, `SetConveyor`, `SetContactMassScale`, `SetBodyGravity`, `PointVelocity`, `IsTouching(Entity)`, `IsAwake`, `AddContactSettings`, `AddForceVolume`), and the owner of the node rows `PhysicsContactsGraph.cs`.

`AverActor` gains optional hooks, all empty by default:

```csharp
public virtual void OnHit(in PhysicsHit hit) { }            // impulse, approach speed, point, normal, other entity, surfaces
public virtual void OnBeginTouch(Entity other, in PhysicsHit hit) { }   public virtual void OnEndTouch(Entity other) { }
public virtual void OnStay(Entity other, in PhysicsHit hit) { }
public virtual void OnWake() { }   public virtual void OnSleep() { }
public virtual void OnOverlapBegin(Entity other) { }   public virtual void OnOverlapEnd(Entity other) { }
```
When an actor binds, a cached per-type reflection check (`MethodInfo.DeclaringType != typeof(AverActor)`) decides which of the eight event names to subscribe with `Events.Subscribe(name, handler, owner: entity)`; they are removed with the owner at unbind (`aver_fw_event_unsubscribe_owner`). An actor that overrides none pays nothing. The binder is one new file; `Actor.cs` and `Actors.cs` each get a few lines (shared).

**Aver Node: family `PhysicsContacts` v1**, wave 9 C contract exactly: `GraphPhysicsContactsNodeTable.cs` (in `Aver.Graph`, next to `GraphVehicleNodeTable.cs`) with a `PhysicsContactsTable()`, `sandbox/src/GraphNodeDefsPhysicsContacts.hpp` mirroring it pin for pin, `[AutoRun]` `PhysicsContactsNodeTests` built from `NodeRowKit`; owner methods `internal static` thin wrappers over the public C#; failures return false or 0 and log once per node id, never throw. Nothing is edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Entities are ints (the owner resolves entity to body), enums are ints documented in `docs/nodes/PhysicsContacts.md` (generated by `scripts/node-catalog.py --write`), palette category `Physics`.

| Key | Kind | Pins (besides `exec` / `then`) | Wraps |
|---|---|---|---|
| `an_physsetevents` | Exec | in `entity:i`, `flags:i` (1 hit, 2 touch, 4 stay, 8 wake/sleep), `minImpulse`, `minSpeed=30`; out `success:b` | `Entity.SetPhysicsEvents` |
| `an_physsetsurface` | Exec | in `entity:i`, `surface:i`; out `success:b` | `SetSurface` |
| `an_physsetteam` | Exec | in `entity:i`, `team:i`; out `success:b` | `SetTeam` |
| `an_physsetteampass` | Exec | in `teamA:i`, `teamB:i`, `pass:b`; out `success:b` | `Physics.SetTeamPass` |
| `an_physsetgroup` | Exec | in `entity:i`, `group:i`, `subGroup:i`; out `success:b` | `SetCollisionGroup` |
| `an_physignorepair` | Exec | in `entityA:i`, `entityB:i`, `seconds` (-1 forever, 0 clears); out `success:b` | `IgnoreCollisionWith` |
| `an_physsetoneway` | Exec | in `entity:i`, `on:b`, `upX`, `upY`, `upZ=1` (body-local); out `success:b` | `SetOneWay` |
| `an_physsetconveyor` | Exec | in `entity:i`, `vx`, `vy`, `vz`, `local:b`; out `success:b` | `SetConveyor` |
| `an_physsetmassscale` | Exec | in `entity:i`, `massScale=1`, `inertiaScale=1`; out `success:b` | `SetContactMassScale` |
| `an_physdefinesurface` | Exec | in `surface:i`, `friction`, `restitution`, `frictionMode:i`, `restitutionMode:i` (0 default, 1 average, 2 min, 3 multiply, 4 max); out `success:b` | `Physics.DefineSurface` |
| `an_physsetgravityvector` | Exec | in `entity:i`, `on:b`, `gx`, `gy`, `gz`; out `success:b` | `SetBodyGravity` |
| `an_physcreateforcevolume` | Exec | in `cx cy cz`, `hx hy hz`, `vx vy vz`, `strength`, `priority:i`; attributes `kind` (gravity, wind, force), `shape` (sphere, box, all); out `volume:i`, `success:b` | `Physics.CreateForceVolume` |
| `an_physdestroyforcevolume` | Exec | in `volume:i`; out `success:b` | `ForceVolume.Destroy` |
| `an_physradialimpulse` | Exec | in `x y z`, `radius`, `velocityChange`, `falloff=1`, `upBias`, `layerMask:i`; out `count:i` | `Physics.RadialImpulse` |
| `an_physactivatebox` | Exec | in `cx cy cz`, `hx hy hz`, `layerMask:i`; out `success:b` | `Physics.ActivateInBox` |
| `an_physpointvelocity` | Pure | in `entity:i`, `x y z`; out `found:b`, `vx vy vz` | `PointVelocity` |
| `an_physincontact` | Pure | in `entityA:i`, `entityB:i`; out `touching:b` | `IsTouching` |
| `an_physestimateimpact` | Pure | in `entityA:i`, `entityB:i`, `px py pz`, `nx ny nz`; out `found:b`, `impulse`, `frictionImpulse` | `Physics.EstimateImpact` |
| `an_physlasthit` | Pure | in `entity:i`; out `found:b`, `other:i`, `px py pz`, `nx ny nz`, `impulse`, `speed`, `surface:i`, `otherSurface:i` | scans the current step call's records (non-destructive) for the strongest one involving the entity |
| `an_physisawake` | Pure | in `entity:i`; out `found:b`, `awake:b` | `IsAwake` |
| `an_physgravityat` | Pure | in `x y z`; out `gx gy gz` | `Physics.GravityAt` |

Events need no node: `AN_OnEvent` with `ENTRY e Physics.Hit` (and the other seven names of 2.8.3); `AN_EventPayload` reads the scalar entries. A graph that wants the vectors calls `an_physlasthit` in the same chain.

### 2.10 Level records and format

`COMPONENT ContactSettings events=hit|touch surface=3 team=2 group=5 sub=1 mass=2 inertia=1 minimpulse=40 minspeed=30 oneway=0 up=0,0,1 angle=60 conveyor=0,0,0 local=0 gyro=0 noreduction=0 edge=0 detectstatic=0` and `COMPONENT ForceVolume kind=gravity shape=sphere extents=3000,0,0 vector=0,0,-980 strength=980 falloff=1 turbulence=0 priority=0 layers=0 radial=1 align=1 wake=1 off=0`. Canonical form omits default keys (`PlacedComponentModel.hpp` rule); runtime state (`outBody`, `outVolume`) is never saved. `"ContactSettings"` and `"ForceVolume"` are appended to `kPlacedComponentTypes` (`LevelComponents.hpp:27-29`) and to the type list in `OcWorld.cpp:173-174`, with enum values and `applyContactSettings` / `applyForceVolume` plus capture cases beside `applyCloth`. `FORMAT_SPECS.md` gains the two rows (written by the integrator). The legacy `.ocmap` cannot store a COMPONENT record, so level content for these lives in `.ocworld`, as it does for `Vehicle`.

### 2.11 Editor

- **Add Component** rows `ContactSettings` and `ForceVolume` in `PlacedComponentModel.hpp` (`canAttach`: any entity for ContactSettings, with the reason line "no physics body yet: settings apply when one exists" if it has none; any entity for ForceVolume). `makeNewRecord` seeds `events=hit|touch`, and for the volume a sphere of 1000 cm with gravity `0,0,-980`.
- **Details sections** (ImGui-free models `ContactSettingsDetailsModel.hpp` and `ForceVolumeDetailsModel.hpp`, tested; widget halves `ContactSettingsSection.cpp`, `ForceVolumeSection.cpp` called from `SandboxComponentDetails.cpp` by one dispatch block, the Vehicle precedent). ContactSettings: event checkboxes with threshold sliders, surface combo (names from a project-side list when one exists, ids otherwise), team and group fields, one-way and conveyor blocks with the direction drawn in the viewport, mass scale with an "infinite" checkbox; in Play a live block: awake or asleep, current touch list (from the stream), last hit impulse and speed, a rolling log of the last 12 events. ForceVolume: kind, shape, vector, strength, falloff, priority, radial and align toggles; in Play, bodies affected this step.
- **`Show > Contacts` overlay** (`ContactDebugOverlay.{hpp,cpp}`, the `SoftBodyDebugOverlay` pattern: `collect` into `rhi::LineVertex`, drawn under `!noEditorChrome_`; both backends implement `createLineMesh` / `drawLines`): *points* (a cross at each recorded point for the last 30 frames, red by impulse), *normals*, *relative velocity* arrows, *one-way* (an arrow from the platform and a plane patch), *conveyors* (arrows along the surface velocity), *volumes* (sphere or box wireframe, vector arrow, radial spokes), *labels* for team, group and surface of the selected body. Cost note: it reads the stream, so it only shows bodies whose events are on; a "record everything while this is open" toggle sets the event mask of every dynamic body through the ABI and restores it on close.
- COFF discipline: new editor code lives in its own translation units; `GraphNodeDefsPhysicsContacts.hpp` adds 21 `push_back`s to the translation unit that holds the whole node catalog. The implementing agent records the Debug object's section count before and after (the cap note in the memory index) and, if it is within 5 percent of the cap, moves family headers into a second catalog translation unit before merging.

### 2.12 Determinism, patents, what is claimed

- **Determinism.** Callback order across Jolt workers is not deterministic; the stream is sorted and numbered at step end, all tables are ordered or hashed by integer keys read in sorted order, noise is a hash of step index and handle, expiry is in fixed steps, and no clock is read. The claim: two worlds fed the same ordered calls produce a bit-identical record stream (`seq` included) on one machine and binary; the simulation's own cross-platform determinism is Jolt's and this build keeps `CROSS_PLATFORM_DETERMINISTIC` off (`modules/physics.jolt/CMakeLists.txt:51-56`).
- **Rules do not break replay** because they are data changed between steps; a recorded run replays if the same calls are made at the same step boundaries.
- **Patents.** Pair filtering by group and layer, one-way platforms from normal and relative velocity, conveyor surface velocity, per-pair friction override, combine-mode precedence, contact-event derivation from impulse estimates, planetary gravity volumes and wind as velocity drag are long-public, generic techniques, implemented here from first principles on Jolt's MIT-licensed callbacks. Combine mode names (Average, Min, Multiply, Max) are descriptive; the precedence order is this document's own. No vendor behaviour was copied or reproduced; the audio-from-collision area, which has live patents, is not touched (the impulse and speed are delivered, the sound design is the project's). Nothing here needs a third-party library.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this area touches them)

**Physics ABI headers** (`modules/physics/include/aver/physics/`)
- `physics_contacts_abi.h`, `physics_rules_abi.h`, `physics_filters_abi.h`, `physics_forcevolumes_abi.h`

**Physics sources** (`modules/physics/src/`)
- `PhysicsContactsInternal.hpp` (tag table, extras slab, rule tables, queue structs, hook declarations, `fromJoltPos` fallback)
- `PhysicsContacts.cpp` (listener hooks, candidates, step-end sort and pair table, record ABI, stats, prediction, point velocity)
- `PhysicsContactRules.cpp` (`SimShapeFilter`, validate, character listener, pair, teams, one-way, scales, surface velocity, surfaces, combine, id sampler)
- `PhysicsGroups.cpp` (group filter and tables, body flags, detect-static)
- `PhysicsQueryFilterObj.cpp` (filter objects and the nine `_f` queries)
- `PhysicsActivation.cpp` (activation listener, active list, bulk poses, activate-in-box, entity lookup, sub-shape index)
- `PhysicsForceVolumes.cpp` (volumes, gravity vectors, wind response, radial impulse, character gravity hook)

**Module `Aver.Physics.Scene`** (`modules/physics.scene/`)
- `module.json`, `CMakeLists.txt`, `README.md`
- `include/aver/physicsscene/PhysicsSceneSystem.hpp`, `Components.hpp` (`CContactSettings`, `CForceVolume`, flag constants)
- `src/PhysicsSceneSystem.cpp`, `ContactSettingsApply.cpp`, `ForceVolumeFollow.cpp`, `EventPump.cpp`, `PoseSync.cpp`, `BodyBuild.cpp`

**C#** (`scripting/csharp/Aver.Physics/`): `Native.Contacts.cs`, `Native.Rules.cs`, `Native.Filters.cs`, `Native.ForceVolumes.cs`, `Contacts.cs`, `BodyContacts.cs`, `Rules.cs`, `QueryFilter.cs`, `ForceVolumes.cs`. (`Aver.Framework/`): `PhysicsEvents.cs`, `EntityPhysics.cs`, `PhysicsContactsGraph.cs`. (`Aver.Graph/`): `GraphPhysicsContactsNodeTable.cs`.

**Sandbox**: `sandbox/src/GraphNodeDefsPhysicsContacts.hpp`, `ContactSettingsDetailsModel.hpp`, `ForceVolumeDetailsModel.hpp`, `ContactSettingsSection.cpp`, `ForceVolumeSection.cpp`, `ContactDebugOverlay.hpp`, `ContactDebugOverlay.cpp`

**Tests**: `tests/physics/src/ContactEventsTest.cpp`, `ContactRulesTest.cpp`, `QueryFilterTest.cpp`, `ActiveBodiesTest.cpp`, `ForceVolumeTest.cpp`; `tests/physics.scene/` (`PhysicsSceneSystemTest.cpp`, `EventPumpTest.cpp`, `PoseSyncTest.cpp`, `CMakeLists.txt`); `tests/editor/src/ContactSettingsDetailsModelTest.cpp`, `ForceVolumeDetailsModelTest.cpp`; `scripting/csharp/Aver.Graph.Tests/PhysicsContactsNodeTests.cs`, `PhysicsContactsApiTests.cs`

**Content and docs to be created by the implementers**: `test-content/ContactLab/` (generator `make_contact_lab.py`, `README.md`, level), `docs/physics/CONTACTS.md`, `docs/physics/FORCE_VOLUMES.md`, `docs/nodes/PhysicsContacts.md` (generated)

### 3.2 Modified, owned by this area

None. Every file this area changes is either new (3.1) or shared (3.3). In particular `Body.cs`, `Physics.cs` and `Queries.cs` in `Aver.Physics` and all twelve existing physics headers are **not** modified: the new C# members are `partial` in new files and the C ABI additions are new headers.

### 3.3 SHARED (another wave 11 area or the integrator also touches these; every edit is small and additive)

- `CMakeLists.txt`
- `cmake/modules.catalog.json`
- `modules/physics/CMakeLists.txt`
- `modules/physics/src/PhysicsInternal.hpp`
- `modules/physics/src/PhysicsWorld.cpp`
- `modules/physics/src/PhysicsBody.cpp`
- `modules/physics/src/PhysicsVehicle.cpp`
- `modules/physics/src/PhysicsCharacter.cpp`
- `modules/physics/src/Convert.hpp`
- `tests/abi/src/AbiEnumTest.cpp`
- `Runtime/include/aver/game/GameTick.hpp`
- `Runtime/include/aver/game/GameSystemsWiring.hpp`
- `Runtime/include/aver/game/LevelComponents.hpp`
- `Runtime/src/LevelComponents.cpp`
- `Runtime/src/GameApp.cpp`
- `Runtime/CMakeLists.txt`
- `modules/formats/src/OcWorld.cpp`
- `sandbox/CMakeLists.txt`
- `sandbox/src/PhysicsSceneSync.cpp`
- `sandbox/src/SandboxPlay.cpp`
- `sandbox/src/SandboxLevelEdit.cpp`
- `sandbox/src/SandboxInit.cpp`
- `sandbox/src/SandboxApp.hpp`
- `sandbox/src/SandboxApp.cpp`
- `sandbox/src/SandboxRender.cpp`
- `sandbox/src/SandboxShell.cpp`
- `sandbox/src/SandboxComponentDetails.cpp`
- `sandbox/src/PlacedComponentModel.hpp`
- `scripting/csharp/Aver.Framework/Actor.cs`
- `scripting/csharp/Aver.Framework/Actors.cs`
- `tests/physics/CMakeLists.txt`
- `tests/editor/CMakeLists.txt`
- `docs/ABI.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`, `docs/SCRIPTING_API.md`, `docs/formats/FORMAT_SPECS.md`, `CHANGELOG.md`

| Shared file | Edit made by this area | Who else |
|---|---|---|
| `CMakeLists.txt` | `add_subdirectory(modules/physics.scene)` in the scene block beside `vehicle.scene` (`:317-322`); `tests/physics.scene` beside `:592-596` | every wave 11 area adds its own lines |
| `modules/physics/CMakeLists.txt` | the seven new sources | other physics areas add sources |
| `modules/physics/src/PhysicsInternal.hpp` | forward declaration of `ContactsState`, one `std::unique_ptr<ContactsState> contacts` member and a `u64 stepIndex` in `World`; declarations of the hooks (`contactsInit/Shutdown/BeginCall/StepBegin/StepEnd`, `contactsBindBody/ForgetBody/NoteEntity`, `forceVolumesStepBegin`, `forceVolumeGravityFor`) | LARGE_WORLD (types), BODY areas |
| `modules/physics/src/PhysicsWorld.cpp` | `EventListener` calls the hooks and gains `OnContactValidate`; `aver_phys_init` / `shutdown` call `contactsInit` / `contactsShutdown`; `addBody` binds the tag and applies default body flags; `aver_phys_remove_body` forgets it; `aver_phys_step` calls `contactsBeginCall` at the top and the step hooks around `Update` (`:487-493`) and uses `forceVolumeGravityFor` in the character loop (`:418-423`); `aver_phys_character_create` binds the inner body; `aver_phys_set_entity` notes the entity | LARGE_WORLD edits `:99` and the listener, VEHICLES-style hooks live beside |
| `modules/physics/src/PhysicsBody.cpp`, `PhysicsVehicle.cpp`, `PhysicsCharacter.cpp` | one `contactsRebind(handle, oldId, newId)` line at each rebuild site (`giveMotionProperties`, the chassis swap `:436-445`, the inner-body toggle `:397-399`); `characterCreated` installs the character listener | body, vehicle and character areas |
| `modules/physics/src/Convert.hpp` | `fromJoltPos(RVec3Arg)` if LARGE_WORLD has not added it | LARGE_WORLD |
| `tests/abi/src/AbiEnumTest.cpp` | four header rows (`physics_contacts_abi.h` with `Native.Contacts.cs`, and the rest) in the physics `kAbis` row and the per-ABI rows; constant groups `AVER_PHYS_EVT_`, `_CEV_`, `_CEI_`, `_CEF_`, `_CFL_`, `_CST_`, `_COMBINE_`, `_BF_`, `_FV_`, `_FVS_`, `_FVP_` with their C# mirrors | every area adds rows |
| `Runtime/include/aver/game/GameTick.hpp` | `physicsSceneSystem().preStep(dt)` after the PRE_PHYSICS group and `postStep(dt, steps)` after `aver_phys_step` (`:156`), each under `#if AVER_MODULE_PHYSICS_SCENE` | cameras, particles |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | register the components and install the event sink (the bus poster) in `registerGameSystems`; `clear()` in `resetGameSystems`; the `seedMovable` helper | all |
| `Runtime/include/aver/game/LevelComponents.hpp`, `Runtime/src/LevelComponents.cpp`, `modules/formats/src/OcWorld.cpp` | two type names, enum values, apply and capture | other component areas |
| `Runtime/src/GameApp.cpp`, `Runtime/CMakeLists.txt` | `buildFromScene` after level apply, `clearFromScene` beside the vehicle clear (`:2471`), `seedMovable` (`:2026`), link `Aver.Physics.Scene` | all |
| `sandbox/src/PhysicsSceneSync.cpp` | the two functions become one-line forwards to `physicsscene::buildFromScene` and `clearFromScene` (file kept) | rigid-body authoring areas, if any |
| `sandbox/src/SandboxPlay.cpp`, `SandboxLevelEdit.cpp`, `SandboxInit.cpp` | seed (`:157`), the mid-Play save set (`:620-640`), registration through `registerGameSystems` | all |
| `sandbox/src/SandboxApp.hpp`, `SandboxApp.cpp`, `SandboxRender.cpp`, `SandboxShell.cpp`, `sandbox/CMakeLists.txt` | overlay options, frame and line-mesh members; overlay collect and draw; `Show > Contacts`; new sources | soft body, vehicles, cameras |
| `sandbox/src/SandboxComponentDetails.cpp`, `PlacedComponentModel.hpp` | one dispatch block; two `PlacedKind` rows | all |
| `scripting/csharp/Aver.Framework/Actor.cs`, `Actors.cs` | the eight virtual hooks; one binder call at bind and unbind | AI, GAS |
| `docs/*.md`, `CHANGELOG.md` | rows and sections named in 2.x, written by the integrator | every area |

Not touched: `scene_abi.h`, `Components.hpp`, `framework_abi.h`, any shader, `modules/rhi*`, `modules/render*`, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`, `physics_vehicle*_abi.h`, `physics_ragdoll_abi.h`, `framework_softfield_abi.h`.

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A L, B L, C M, D M, E L, F M, G M, H S. Overall **XL**. Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**Phase A: tag table and the contact stream (L).** `PhysicsContactsInternal.hpp`, `PhysicsContacts.cpp`, `physics_contacts_abi.h`; the listener restructure (legacy behaviour preserved, lock narrowed), character inner-body binding, the activation listener and WAKE / SLEEP records, step-end sort and pair table, the rebind lines at the three rebuild sites, stats. C# `Native.Contacts.cs`, `Contacts.cs`, `BodyContacts.cs` (events, point velocity, in-contact, estimate). `AbiEnumTest` rows. `ContactEventsTest`. Exit: every existing physics test unchanged and green; `ContactEventsTest` green including "an unflagged world is bit-identical to today" and "tags survive every rebuild".

**Phase B: rules (L).** `PhysicsContactRules.cpp`, `PhysicsGroups.cpp`, `physics_rules_abi.h`: `SimShapeFilter`, validate, character listener, pair, teams, one-way, scales, surface velocity, surfaces and combine (lazy install), groups, flags, detect-static, the id sampler. C# `Native.Rules.cs`, `Rules.cs`. `ContactRulesTest`. Exit: green, including "default combine modes are bit-identical to Jolt's" and the vehicle tests untouched.

**Phase C: filters, lookup, activation reads (M).** `PhysicsQueryFilterObj.cpp`, `PhysicsActivation.cpp`, `physics_filters_abi.h`. C# `Native.Filters.cs`, `QueryFilter.cs`. `QueryFilterTest`, `ActiveBodiesTest`. Exit: green; the `_f` parity property test passes.

**Phase D: force volumes (M).** `PhysicsForceVolumes.cpp`, `physics_forcevolumes_abi.h`, the character gravity hook. C# `Native.ForceVolumes.cs`, `ForceVolumes.cs`. `ForceVolumeTest`. Exit: green; `CharacterTest` and `BuoyancyTest` unchanged.

**Phase E: `Aver.Physics.Scene` and the hosts (L).** Module, components, system, preStep, pump, pose write-back, relocated creation, level records, `GameTick.hpp` and `GameSystemsWiring.hpp` wiring, `GameApp` calls, `seedMovable`, the mid-Play save set. `tests/physics.scene`. Exit: the three scene suites green; `TemplateScaffoldTest` and the level-component tests still green; a headed Play of `ContactLab` (section 6) in the editor and the packaged runtime.

**Phase F: framework C# and Aver Node (M).** `PhysicsEvents.cs`, `EntityPhysics.cs`, `PhysicsContactsGraph.cs`, the actor hooks and binder, 21 rows plus palette plus tests, `--write` for `docs/nodes/PhysicsContacts.md`. Exit: `PhysicsContactsNodeTests`, `PhysicsContactsApiTests`, `NodeCatalogCheck`, `CsharpApiIndexCheck` green.

**Phase G: editor (M).** Add Component rows, Details models and sections, `Show > Contacts`, level-record plumbing in the Details. Exit: the two editor model tests green; the Debug sandbox object section count recorded.

**Phase H: lab, docs (S).** `test-content/ContactLab`, the doc edits of 3.3, `module-catalog.py --write`, `module-guard-audit.py`, `docs-check.py`. Exit: full `ctest` green.

---

## 5. Tests

Everything below runs without a GPU or a window. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

**`tests/physics` (link `Aver.Physics Aver.Core`)**
- `ContactEventsTest`: (1) *Hit*: a 10 kg crate dropped 200 cm onto a floor, HIT set, yields exactly one ADDED with `APPROACH_CMS` within 15% of `sqrt(2 g h)` (about 626 cm/s) and `NORMAL_IMPULSE` within 30% of `m v` (6260 kg*cm/s) for restitution 0; a second run with restitution 0.5 is within 30% of `m v (1 + e)`. (2) *Silent by default*: an unflagged world fills the legacy queue exactly as before and the new stream stays empty; with `set_legacy(0)` the legacy queue is empty and the stream still works. (3) *Stay*: a resting crate with STAY gets PERSISTED records every 4th step; without STAY none. (4) *Pair begin once*: a crate lying across a two-box compound floor produces two ADDED sub-shape records (manifold reduction off) and exactly one with `PAIR_BEGIN`; lifting it gives exactly one `PAIR_END`. (5) *Sleep*: a crate left to rest falls asleep, emitting SLEEP and a REMOVED with `SLEEP_REMOVAL` and no `PAIR_END`; a nudge gives WAKE and an ADDED without `PAIR_BEGIN`; lifting it away then gives `PAIR_END`; a body that stays awake but ceases to touch gives `PAIR_END` on the step it lifts. (6) *Removed during contact*: `aver_phys_remove_body` mid-touch emits `PAIR_END | BODY_REMOVED | SYNTHETIC` once and Jolt's late Removed is dropped. (7) *Character*: a crate dropped on a character's capsule records with the character's handle and the `CHARACTER` flag; the legacy queue still skips it. (8) *Entities*: ENTITY_A/B on ADDED and REMOVED equal the stamps. (9) *Sub-shape ids* are equal between ADDED, PERSISTED and REMOVED of one contact. (10) *Capacity*: 20000 flagged contacts in one call with capacity 4096 leave `count == 4096`, `dropped > 0`, and every BEGIN and END present before any STAY loss. (11) *Estimates*: with `maxEstimatesPerStep` forced to 0 records carry `CRUDE_IMPULSE` and an impulse within 50% of the real one. (12) *Prediction*: `aver_phys_estimate_impact` for a falling crate and a floor is within 30% of the measured first-contact impulse. (13) *Tags survive rebuilds*: set events on a static box, `set_motion_type` to dynamic, swap a vehicle chassis with `set_chassis_boxes`, toggle a character's inner body; each still records and `STALE_TAG_READS` is 0. (14) *Replay*: two fresh worlds run the same script for 600 steps with 8 worker threads; every record, `seq` included, is bit-identical. (15) *Large coordinates*: ADDED at x = 50 km reports `PENETRATION_CM` and `APPROACH_CMS` unchanged from near the origin (skipped, with a printed reason, when the build is single precision).
- `ContactRulesTest`: (1) *Pair ignore*: a box falling through a pair-ignored floor lands on the next one; `seconds = 0.5` restores collision after 30 steps exactly; ignoring the same pair twice is idempotent; removal of either body drops the entry. (2) *Teams*: team 3 passes team 5 and not team 4; (t, t) pass-through; a sensor pair respects it (pins the open `SimShapeFilter`-for-sensors point). (3) *One-way*: a ball thrown up from below passes a platform and lands on it on the way down; a ball resting on it stays; a ball hitting the edge passes; a platform rotated 90 degrees about X is one-way along the rotated axis; both bodies one-way is the intersection of both rules; the contact normal sign is asserted. (4) *Conveyor*: a box on a static belt with +X surface velocity reaches within 15% of the belt speed and does so when the box is created before and after the belt (both body orders); a local velocity follows the belt's rotation; angular surface velocity spins a ball on a turntable. (5) *Apparent mass*: a body with infinite contact mass is not displaced by a heavy impactor but keeps falling under gravity; a mass scale of 4 on a light box makes it behave as one 4 times heavier in a two-body collision (momentum exchange within 10%). (6) *Surfaces and combine*: sliding distance on a 20 degree incline for each mode pair matches the formula for that mode within 10%; a mixed pair follows the precedence; `surface_pair` overrides both numbers; the terrain sampler wins over the table. (7) *Bit identity*: with every mode default and no surface, 300 steps of a 40-body scene end with poses equal to the old build's recorded values (a golden file from before the change) to the bit, and the combine function pointers are Jolt's. (8) *Groups*: same-group boxes pass through each other; self-collision on restores it; sub-groups (0, 1) enabled collide while (0, 2) pass; a grouped box still collides with an ungrouped one, with another group, and with a ragdoll part; a group survives `set_motion_type` rebuild; `group_destroy` frees members. (9) *Flags*: with gyroscopic on, a box spun about its intermediate axis flips its axis within 6 s and does not with it off; manifold reduction off yields distinct sub-shape records for a body across two coplanar boxes where on yields one; edge removal on a ball rolling over a mesh seam has lower peak vertical velocity than off. (10) *Detect static*: a plain static sensor over a static box reports nothing; the same sensor with `DETECT_STATIC` reports the overlap and `overlap` is released when cleared; calling it on a non-sensor is refused. (11) *Characters*: a pair-ignored body does not block a character; a team-pass body does not; layers still apply.
- `QueryFilterTest`: (1) *Parity*: for 500 random rays, boxes and spheres, the `_f` call with a filter of the same mask, ignore and flags returns the same body, point and normal as `_ex` or the `queries` call. (2) *Ignores*: body, vehicle (chassis), ragdoll (every part), group, entity, character, sub-shape (one box of a compound) each removed from a ray that passes through it and reappearing when the ignore is switched off. (3) *Shared child shapes* are ignored together (documented behaviour). (4) *Lifecycle*: 64 filters, the 65th refused, destroyed handles refused, `reset` clears. (5) *Entity lookup*: `bodies_of_entity` after stamping, restamping and removal; `body_of_entity` is the lowest handle; a character is included. (6) *Sub-shape index* of a compound hit equals its construction index.
- `ActiveBodiesTest`: (1) 200 bodies of which 7 are awake: `active_count == 7`, `active_bodies` is those handles ascending; sleeping ones appear after a nudge. (2) `active_poses` equals `aver_phys_body_position/rotation` for each. (3) `body_poses` writes zeros for a dead handle. (4) `activate_in_box` wakes exactly the sleepers inside the box on the masked layers. (5) `point_velocity` of a body spinning at 2 rad/s about Z at 100 cm radius is 200 cm/s tangent within 1%. (6) `bodies_in_contact` true while touching, false after parting, false for two sleepers.
- `ForceVolumeTest`: (1) A gravity volume of (0,0,+980) in a box lifts a crate 0.5 g t^2 within 3% in 1 s, and bodies outside fall as before. (2) Priority: the higher priority wins; ties go to the lower handle. (3) Radial gravity: a crate at 1000 cm from a point falls towards it; with falloff 1 the acceleration at the edge is 0. (4) Per-body gravity vector wins over a volume; clearing restores. (5) Wind: a crate in a 600 cm/s wind with k = 2 reaches 63% of the wind speed in 0.5 s within 10%; `wind_response(0)` is immune; turbulence is reproducible. (6) Force volume radial away from the centre pushes outward. (7) A character inside a gravity volume pointing +X is carried by it (grounded false) and with `ALIGN_CHARACTER_UP` stands on a wall built for it. (8) `radial_impulse`: bodies within radius change speed by the requested amount scaled by falloff, bodies outside do not, count is exact, sleepers wake. (9) Sleeping behaviour: a sleeping body in a wind stays asleep unless `WAKE`. (10) 65th volume refused. (11) Replay: two worlds, bit-identical. (12) `BuoyancyTest` and `CharacterTest` (existing) unchanged.

**`tests/physics.scene` (link `Aver.Physics.Scene Aver.Physics Aver.Scene Aver.Core`; sink injected)**
- Components: registered sizes 64 and 64, zero-filled defaults valid, `outBody` / `outVolume` read-only through the generic field ABI.
- `PhysicsSceneSystemTest`: `CContactSettings` on a `CRigidBody` entity reaches the body (events, surface, team, group, scales, conveyor, one-way); editing a field applies on the next tick; removing the component resets the body; at most 64 applications per frame; a `CForceVolume` follows its entity (move it 500 cm, the volume moves); disabling and destroying behave.
- `EventPumpTest`: HIT, BEGIN, END, STAY, WAKE, SLEEP and both overlap events reach the sink with the payload of 2.8.3 and the right target; the other side of a terrain contact (entity 0) is skipped; the HIT cooldown is measured in steps and holds at 30 and 144 fps frame rates; `dispatchImmediately` uses the other bus call; no events for an entity with no mask; a flagged sensor routes both directions.
- `PoseSyncTest`: 3000 bodies, 40 awake: the write-back touches 40 entities per tick (counted through a `CLocal` write counter), a sleeping body is written once more at its SLEEP and then never; static and kinematic bodies are never written; a child entity gets the parent-relative value; unchanged poses write nothing; `entities()` lists the written entities.
- Relocation: `buildFromScene` followed by `clearFromScene` reproduces the old `syncPhysicsFromScene` behaviour on the scenes `PhysicsSceneSync` was written for (box, sphere, capsule, cylinder, sensor, joints with two passes).

**`tests/editor`**: `ContactSettingsDetailsModelTest` (record keys and canonical omission, `canAttach` text, event-bit to checkbox mapping, formatting of the live log), `ForceVolumeDetailsModelTest` (seed record, shape-to-extents rules, priority ordering).

**`tests/abi`**: `AbiEnumTest` pairs the four new headers with their `Native.*.cs` files (names and parameter names) and every `AVER_PHYS_*` constant group with its C# mirror.

**C# (`Aver.Graph.Tests`, Windows only, ctest `GraphManagedTests`)**: `PhysicsContactsNodeTests` (the 21 rows through `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`; exec rows refused by the pure compiler; the pure rows work as pure reads; the methods return false rather than throw with no library); `PhysicsContactsApiTests` (`ContactRecord.ViewFrom` swaps and negates correctly on a hand-built buffer; `BodyEvents` values equal the C constants; `QueryFilter` lifecycle and finaliser backstop; every API answers false or default with the native library absent). Python: `NodeCatalogCheck` (table to palette parity for the 21 rows) and `CsharpApiIndexCheck`.

---

## 6. Headed manual checks for the owner

Use `test-content/ContactLab` (generated by `make_contact_lab.py`, level `.ocworld`, no third-party assets) and a project from any template. Editor first, then the packaged runtime, then `--backend vulkan`. The GPU must be idle, screenshots at 1440p with a ping for remote runs.

1. **Hits.** Drop a crate, a barrel and a sphere onto concrete and onto the ice slab: the Details log shows `Physics.Hit` with plausible speed and impulse; the console print graph prints the same numbers; a feather-light box produces no hit under the threshold.
2. **Touch begin and end.** Walk a crate onto a pressure plate (a `ContactSettings` body with `touch`): `BeginTouch` once, `EndTouch` once when it is dragged off; leave a crate on the plate until it sleeps: no `EndTouch`; kick it: `Wake`, no new `BeginTouch`.
3. **One-way platforms.** Jump up through a stack of one-way platforms with the character and a thrown crate: the crate behaves as specified; the character walks through from below and stands on top (the character path is the pair rule only, so the lab's platforms for the character are marked and the Details text says so).
4. **Conveyor.** A belt carries crates at the set speed in both directions; a rotated belt follows; a crate on a turntable spins.
5. **Surfaces.** Ice, rubber, mud slabs: a sliding crate stops in the order the table predicts; combine modes change it as the Details tooltip says; a ball bounces higher on the rubber.
6. **Teams and groups.** A door that only team 2 passes; a chained trailer made of three boxes in one group does not self-collide and still collides with the world.
7. **Detect static.** A kinematic trigger sees the lab's static wall while a plain trigger does not; a note of the frame time with 20 of them.
8. **Gravity.** A small planet (radial volume with align up): the walking pawn circles it; crates fall to its surface; the HUD `an_physgravityat` readout matches; leaving the volume restores normal gravity.
9. **Wind and explosion.** A wind tunnel throws light boxes and not rocks (`wind_response`); the barrel's `an_physradialimpulse` scatters crates by distance; sleeping crates in the wind stay still until the explosion wakes them.
10. **Active-only sync.** Place 3000 crates in a settled pile and knock one corner: the Profiler's Gameplay row does not grow with the pile; the Details "awake bodies" counter matches.
11. **Query filters.** The lab's raycast graph ignores a whole ragdoll and a car in one call; the same ray with the old `_ex` call hits them.
12. **Cost.** With the GPU idle: 2000 bodies in a pile with no body flagged, 10 flagged, all flagged (HIT | TOUCH | STAY); and a scene with 16 volumes and 800 awake bodies. Record milliseconds per frame on the physics span for each and report them (none exist today). Check the legacy queue cap does not trip on the NeonDistrict stress level.
13. **Backends and hosts.** Repeat 1, 2, 4, 8 on `--backend vulkan` and in the packaged `AverEngineRuntime.exe` (the `CContactSettings` records and the `ContactLab` level present in cooked content; bodies are built, crates move, events fire): this is the first time authored Rigid Body entities exist in a shipped game, so the packaged pass is a real test, not a regression run.
14. **Save mid-Play.** Autosave while crates tumble: reload, the crates are where the level placed them.
15. Full `ctest`, the graph suite via `dotnet run`, and open `docs/nodes/PhysicsContacts.md` to confirm 21 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **The missing pose write-back.** No code copies a rigid body's pose onto its entity (section 1.5). Default: build it in `Aver.Physics.Scene` for dynamic `CRigidBody` entities only, driven by the active list. If the owner knows of a sync elsewhere (a script convention in the templates), it is replaced by this and the templates' copying graphs become no-ops, not errors.
2. **The packaged runtime never builds `CRigidBody` bodies.** Default: relocate the creation into `Aver.Physics.Scene` (kept forwards in the sandbox file) and call it from `GameApp`, so authored physics works in a shipped game. Alternative: leave the runtime alone and ship the new features editor-only; not recommended, it makes every feature here untestable outside the editor.
3. **No interpolation of written poses.** Default: none, as vehicles. A visible 60 Hz stutter at a high display rate is the signal to add a render-time blend (a separate feature touching the draw side).
4. **Centre of mass vs entity origin.** The write-back assumes the shapes `PhysicsSceneSync` builds are centred on the entity origin, true today; a convex hull or mesh body with an offset centre of mass would sit offset. Default: the system subtracts the difference between the body's reported centre and the pose it was created at, recorded at build time; verified headed with a hull.
5. **Group semantics.** Default: same group ids do not collide (the ragdoll and machine case). The opposite (only same-group collide, team-vs-team style) is the layer matrix's job and is not duplicated.
6. **New stream is opt-in per body; the legacy queue stays default-on.** Default as stated. Turning the legacy queue off in new templates is a content decision for later.
7. **Impulse is an estimate** (decision 5). Default: documented as such everywhere; if a project needs the solver's applied impulse, Jolt exposes no per-contact lambda to a listener, so the only route is a custom contact constraint, which is out of scope.
8. **Characters.** Default: pair ignore, teams and groups apply through a `CharacterContactListener`; one-way platforms, conveyors and apparent mass do not (a `CharacterVirtual` is not a rigid body; conveyors need the ground-velocity path, one-way needs the contact normal Jolt does not hand `CharacterContactListener::OnContactValidate`). A follow-up uses `OnContactAdded`'s `ioSettings` and `OnAdjustBodyVelocity`.
9. **Surface authoring.** Default: ids defined from code, a graph node or a level start script; no asset. A `.ocsurface` table (names, friction, restitution, modes, footstep and impact sound slots) is the natural follow-up and touches Formats, the Content Browser and the landscape layer mapping, so it is not smuggled in here. Terrain per-point ids need the landscape area to supply the sampler.
10. **Ignore pairs are time-limited, not "until separated".** Default: seconds. Unreal-style "ignore until the overlap ends" would need a per-step overlap check of every ignored pair; cheap enough for a handful of pairs and a possible v2.
11. **`SimShapeFilter` for pair rules.** UNVERIFIED that it is consulted for sensor pairs and for every motion quality; the tests pin both. If either fails the same rules move into `OnContactValidate` for the failing case.
12. **Which point an event reports.** Default: the first manifold point on body 1. A manifold centroid is a one-line change if headed checks show a lopsided position for large flat contacts.
13. **Event delivery latency.** Default: posted, delivered at the next frame's flush (one frame after the contact). `dispatchImmediately` is the switch. Hit sounds that must land on the frame are the only case that cares.
14. **Doubles.** Section 2.3.7 is the whole dependency. If LARGE_WORLD has not chosen how contact points are rebased, events report absolute float cm until it does.
15. **Patent notes, collected.** See 2.12. Nothing in this area needs a third-party library or a vendor behaviour; Jolt callbacks, the standard geometric tests and the documented estimate function are the whole of it.
