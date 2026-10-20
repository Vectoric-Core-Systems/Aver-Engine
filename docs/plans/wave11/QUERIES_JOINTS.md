# Queries and Joints: the rest of Jolt's query and constraint surface (wave 11)

Design contract for the implementing agent. Owner decision (wave 11): integrate the rest of Jolt Physics. Nothing here is built or run. Rules in force: upgrade the existing physics ABI and the existing Joint and Physics C# classes (no parallel system), no new third-party code (Jolt is vendored and every class used here is already compiled into `Aver.Physics.Jolt`), lean comments, never delete files, additive ABI changes only, headless ctest where possible, headed manual runs by the owner, never rebuild over the owner's running editor. This area adds no GPU work (D3D12 and Vulkan parity is trivially met: the one optional overlay is `rhi::LineVertex` lines both backends draw). Format and depth follow `docs/plans/wave10/VEHICLES.md`; the node contract is `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` section 2.5.

Every "exists today" claim below was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it; every tuning number is an UNVERIFIED starting value until the headed pass in section 6.

**Scope.** Two halves of one module (`modules/physics`):

- **Queries**: shape casts and collides with any shape handle or body, all hits, contact depth and normals, depenetration, closest points, ray against one body or one shape, the broad-phase queries, and a full hit record (sub-shape, face, material, soft-body vertex), with the Jolt cast and ray settings.
- **Joints**: local-space constraint frames, swing-twist motors and live limits, the Pyramid swing type, joint friction, the path constraint's missing half, gear and rack-and-pinion linking, stiffness-mode springs and asymmetric motor limits, constraint priority and solver-step overrides, and a readback block.

Not in scope: Jolt's `ObjectStream` and `PhysicsScene` files (owner decision), Jolt's debug renderer and profiler (other wave 11 areas), the shape and material tables themselves (SHAPES_MATERIALS owns them; section 2.7 is the join), large-world precision (section 7, question 8), and the packaged runtime building authored `CJoint` records (it does not today; section 1.6).

---

## 0. Decisions in one screen

1. **Two new headers, no edits to the old ones except comments.** `physics_queries_ext_abi.h` (`AVER_PHYS_QUERIES_EXT_ABI_VERSION 1`) and `physics_joints_ext_abi.h` (`AVER_PHYS_JOINTS_EXT_ABI_VERSION 1`), each with its own version function. `physics_queries_abi.h` and `physics_joints_abi.h` keep every signature; the joints header gets one wrong comment fixed (`:209-211`, section 1.2). Every v1 caller is unchanged.
2. **Three behaviours of existing calls change, all from refusal or undefined to defined**: (a) `aver_phys_joint_set_motor`, `_set_motor_strength`, `_set_motor_spring` accept swing-twist (axis 0 swing, 1 twist) and path joints, which Jolt supports and the ABI refused; (b) `aver_phys_joint_value` answers for path (distance, cm) and swing-twist (twist angle, rad), which returned 0; (c) **removing a body now removes the joints attached to it** (today they are left holding a pointer to a destroyed `JPH::Body`; section 1.2, finding F1).
3. **One hit record for every query.** Float and int blocks (`AVER_PHYS_HF_*`, `AVER_PHYS_HI_*`) the caller sizes by `maxHits * COUNT`; both arrays optional. The same record serves rays, casts, collides and closest points, so a script learns one layout. Nearest first for rays and casts, deepest first for collides, nearest first for closest points; ties break on body handle then sub-shape id, so results are deterministic (the old `aver_phys_raycast_all` is not, between equidistant bodies).
4. **Settings are a float block** (`AVER_PHYS_QS_*`, 13 entries, defaults are Jolt's own) passed as `const float*` plus a count, nullable, shorter than defined allowed, longer refused. Back faces, treat-convex-as-solid, active edges, deepest point, max separation, extra radius, shrunken shape, edge direction, internal-edge removal, tolerance.
5. **The query shape is a SHAPES_MATERIALS shape handle or a body.** A flag (`AVER_PHYS_QUERY_SOURCE_BODY`) says the `shape` argument is a body handle, whose current shape is used and which is ignored by the query. The shape-handle half is Phase E and needs SHAPES_MATERIALS' table through two seams (section 2.7); everything else here works on bodies and does not wait for it.
6. **Joints get one general creator with frames and a space.** `aver_phys_joint_create_ex(type, space, bodyA, bodyB, frameA, frameB, params, n)`; a frame is nine floats (position, axis X, axis Y), world or body-local (Jolt `EConstraintSpace::LocalToBodyCOM`; "body-local" is relative to the centre of mass and body rotation `aver_phys_body_position` / `_rotation` report). Covers all eleven creatable types, adds the Pyramid swing type, and is the only way to build a joint between bodies that are not yet posed. The twelve v1 creators stay as the convenient world-space forms.
7. **Everything else in joints is a setter on a handle that already exists**: friction, motor targets (including Jolt's `PositionAndVelocity` state, which the ABI lacked), asymmetric motor limits, spring mode, limit block (live limits for cone and swing-twist), target rotation, solver priority and step overrides, gear and rack coupling. New load-valued arguments are **SI (N, N*m, N/m)** with the unit in the parameter name, like `aver_phys_joint_set_break`; the older `_set_motor_strength` keeps its kg*cm units.
8. **Path joints become usable.** `aver_phys_joint_path_create` takes authored Hermite tangents and normals, a rotation constraint type, friction, a start distance, and **resamples to 100 cm segments by default** so Jolt's path fraction equals metres of arc and its position motor behaves in real units (section 2.3.3). `_path_move` drives the position and velocity motor by distance and speed in cm; `_state` reads distance and fraction back.
9. **One readback call.** `aver_phys_joint_state` returns a float block and an int block (angle or offset, rate, limits, swing-twist decomposition, six-DOF translation, loads, path distance and fraction, type, priority, couplings). `aver_phys_joint_frames` and `_handles` and `_path_sample` give an overlay or a script what it needs to draw a joint.
10. **Findings the survey missed**, fixed here: joints are not released with their body (F1); gear and rack-and-pinion joints are never linked to their hinge or slider, so Jolt's drift correction never runs (F2); `PositionAndVelocity` and the path constraint's motor are unreachable (F3); the packaged runtime never builds `CJoint` (not fixed, section 1.6). One survey item is **not a Jolt feature**: six-DOF rotation-limit springs (section 1.2, finding F5); the equivalent is a stiffness-mode position motor on the rotation axes, and the plan says so rather than faking it.
11. **Aver Node: two new families.** `Joint` (11 rows: friction, limits, motor targets, motor spring, motor limits, swing-twist motor and target, joint state, path create / move / state, coupling) and `PhysicsQuery` (7 rows: shape cast, shape overlap, depenetrate, closest point, body raycast, raycast hits, broad-phase bodies), in the wave 9 C contract. Rows with several results take a `hitIndex` input and a `count` output and re-run the query, so no array pin and no cached state.
12. **Authored joints get a small editor upgrade (Phase G, optional)**: swing-twist as a `CJoint` type and a runtime-registered `CJointTuning` component (friction, motor spring, priority) read by `PhysicsSceneSync`. A joint overlay is built only if no debug-renderer area ships constraint drawing first (question 12).
13. **Size L, seven phases** (section 4). A and B carry the joint risk, D the query risk; E waits on SHAPES_MATERIALS.

---

## 1. What exists

### 1.1 Queries today

| Fact | Where |
|---|---|
| Ray (`aver_phys_raycast`), sphere overlap, sphere cast in `physics_abi.h`; `_ex` forms with layer mask and one ignore body in `physics_layers_abi.h`; the `_ex` forms still see sensors | `physics_abi.h:570-585`, `physics_layers_abi.h:78-105` |
| `physics_queries_abi.h` adds box, capsule, sphere (two ignores) and body casts, three overlaps, `raycast_all`, `point_query`, each with `layerMask`, `ignoreBody`, `flags` (only `INCLUDE_SENSORS`); sensors skipped by default | `physics_queries_abi.h:54-144` |
| **Every cast reports the first hit only**: a `ClosestHitCollisionCollector`; there is no all-hits shape cast | `PhysicsQueries.cpp:110-132` |
| **Overlaps drop the contact data**: `overlapShape` collects `CollideShapeResult`s and keeps only the body handle; penetration depth, contact points and axis are discarded | `PhysicsQueries.cpp:136-155` |
| Cast results are body, distance, point, normal (normal negated from Jolt's penetration axis, so it points away from the surface). No sub-shape id, face, material, entity, back-face flag | `PhysicsQueries.cpp:123-131` |
| `raycast_all` uses `RayCastSettings{}` (back faces ignored, convex solid) with no way to change them; one entry per hit, nearest first, normal from `GetWorldSpaceSurfaceNormal` | `PhysicsQueries.cpp:278-303` |
| A body can be a query shape (`copyBodyShape`), but meshes, heightfields and soft bodies are refused; compounds and decorators are checked to their leaves | `PhysicsQueries.cpp:62-100` |
| No ray against one body or one shape, no `BroadPhaseQuery` use anywhere (`g_world->system.GetBroadPhaseQuery()` is never called), no closest-point or max-separation query, no depenetration | grep of `modules/physics/src` |
| Filters shared by the layer and query files: `MaskLayerFilter`, `handleForBodyId` (searches characters, whose inner body is not in `World::bodies`), `ignoredIdOf`, `QueryBodyFilter` (two skips, sensors) | `PhysicsQueryFilters.hpp:19-59` |
| Shape handles: only **mesh shapes**, a separate handle space with its own mutex-guarded map, file-local to `PhysicsWorld.cpp`, not cleared at shutdown | `PhysicsWorld.cpp:162-183`, `physics_abi.h:333-358` |
| No physical-material table: Jolt's per-face `PhysicsMaterial` is never set or read | grep of `modules/physics/src` |
| Soft bodies are raycast-visible (a soft body is a body); a ray hit's sub-shape id carries the face, and `SoftBodyShape::GetFaceIndex` and `SoftBodyMotionProperties::GetFace(i).mVertex[3]` are public | `SoftBodyShape.h:24-27`, `SoftBodyMotionProperties.h:71-74`; builder faces are wound straight through with degenerates skipped: `PhysicsSoftBody.cpp:450-456` |
| Tests: `QueryTest` (casts, overlaps, multi-hit, point), `SphereCastQTest`; scenes are static bodies that never step | `tests/physics/CMakeLists.txt:118-127` |
| Docs: `docs/physics/QUERIES.md` (126 lines) | |

### 1.2 Joints today, and the survey verdicts

| Survey claim | Verdict | Evidence |
|---|---|---|
| Joints are world-space only | **Confirmed.** Every creator fills a `*ConstraintSettings` with default `mSpace` (`WorldSpace`) and the same point and axes for both bodies | `PhysicsJoints.cpp:284-638` (no `mSpace` assignment anywhere); Jolt supports `LocalToBodyCOM` on 11 of 12 types (path has none: `PathConstraintSettings` has `mPathPosition`/`mPathRotation` relative to body 1) |
| SwingTwist has no motor and the header says so wrongly | **Confirmed.** The header says "HINGE, SLIDER AND SIX-DOF ONLY. The other constraint types have no motor in Jolt" (`physics_joints_abi.h:209-211`); Jolt's swing-twist has swing and twist motors with state, target angular velocity and target orientation (`SwingTwistConstraint.h:114-149`), and **the path constraint has a position motor too** (`PathConstraint.h:100-111`). Only point, fixed, distance, cone, gear, rack-and-pinion and pulley have none | |
| No live SwingTwist or Cone limits | **Confirmed.** `aver_phys_joint_set_limits` reports "no limits to change" for both (`PhysicsJoints.cpp:808-813`); Jolt has `SetNormalHalfConeAngle`, `SetPlaneHalfConeAngle`, `SetTwistMinAngle`, `SetTwistMaxAngle` (`SwingTwistConstraint.h:105-112`) and `ConeConstraint::SetHalfConeAngle` (`ConeConstraint.h:98`) | |
| Pyramid swing type | **Confirmed, and wider than the survey said.** `ESwingType::{Cone, Pyramid}` (`SwingTwistConstraintPart.h:14-18`) is on `SwingTwistConstraintSettings::mSwingType` (`SwingTwistConstraint.h:45`) **and on `SixDOFConstraintSettings::mSwingType`** (`SixDOFConstraint.h:63`). Creation-time only; no runtime setter. Cone type needs symmetric limits | `PhysicsJoints.cpp:416-438` never sets it |
| Joint friction (hinge, slider, path, swing-twist, six-DOF) | **Confirmed.** `HingeConstraint::SetMaxFrictionTorque` (`:111`), `SliderConstraint::SetMaxFrictionForce` (`:99`), `PathConstraint::SetMaxFrictionForce`, `SwingTwistConstraint::SetMaxFrictionTorque` (`:121`), `SixDOFConstraint::SetMaxFriction(axis, f)` (`:159`); none is called. Friction applies only while the motor is off | grep of `modules/physics/src` finds none |
| Path: rotation type, position motor, fraction readback, authored Hermite tangents and normals | **Confirmed.** The creator leaves `mRotationConstraintType` at `Free`, sets no friction or motor, derives Catmull-Rom tangents and a reference-axis normal itself, and never reads `GetPathFraction`. `maxSlideCm` is honoured by cutting the polyline | `PhysicsJoints.cpp:541-638`; `PathConstraint.h:55-60,92-110` |
| Gear and RackAndPinion `SetConstraints` | **Confirmed (finding F2).** `GearConstraint::SetConstraints(gear1, gear2)` (`GearConstraint.h:61`) and `RackAndPinionConstraint::SetConstraints(pinion, rack)` (`RackAndPinionConstraint.h:82`) are never called, so `SolvePositionConstraint` returns false at once (`GearConstraint.cpp:103`, `RackAndPinionConstraint.cpp:104`): only the velocity constraint runs and the pair drifts out of ratio. The header says "each body must already have a hinge of its own" and links nothing | grep: no `SetConstraints` in `modules/physics/src` |
| SpringSettings stiffness mode; split min and max motor force | **Confirmed.** `_set_motor_spring` and `_set_limit_spring` build `ESpringMode::FrequencyAndDamping` only (`PhysicsJoints.cpp:841,872`); `_set_motor_strength` uses the symmetric `SetForceLimit` / `SetTorqueLimit` (`:726-742`) while `MotorSettings::SetForceLimits(min, max)` and `SetTorqueLimits` exist (`MotorSettings.h:57-60`). `EMotorState::PositionAndVelocity` exists (`MotorSettings.h:19-25`) and `set_motor` refuses state 3 (`:647-655`) (finding F3) | |
| Six-DOF rotation-limit springs | **Not a Jolt feature (finding F5).** `SixDOFConstraint::SetLimitsSpringSettings(axis, ...)` asserts `inAxis < EAxis::NumTranslation` (`SixDOFConstraint.h:155-156`); the header already says rotation limits stay hard (`physics_joints_abi.h:262-264`). What Jolt does offer is a rotation **motor** with a stiffness-mode spring and a target orientation, which is a spring toward a rest pose, not a soft limit | |
| Constraint priority and per-constraint solver-step overrides | **Confirmed.** `Constraint::SetConstraintPriority`, `SetNumVelocityStepsOverride`, `SetNumPositionStepsOverride` (`Constraint.h:128-141`) are never called | |
| Richer readback (swing-twist rotation, distance, pulley length, lambdas) | **Confirmed.** `aver_phys_joint_value` answers for hinge and slider only (`PhysicsJoints.cpp:817-832`); `appliedImpulse` already reads the lambdas for the break check (`:172-238`) but nothing returns them | `SwingTwistConstraint::GetRotationInConstraintSpace` (`:149`), `PulleyConstraint::GetCurrentLength` (`PulleyConstraint.h:92`), `GetTotalLambda*` on every type |

**Findings the survey did not list.**

- **F1: removing a body does not remove its joints.** `aver_phys_remove_body` releases a vehicle and a ragdoll (`PhysicsWorld.cpp:539-540`) and then destroys the body (`:543-546`); it never looks at the joint table. A Jolt constraint holds raw `Body*` pointers (`TwoBodyConstraint::mBody1/2`), so the next step solves a constraint against freed memory. The static-to-movable rebuild is guarded against exactly this (`physics_abi.h:146-153`), so the hazard was known for one path and missed for the other. No test removes a jointed body (`grep remove_body tests/physics/src/Joint*.cpp` is empty).
- **F2** above; **F3** above.
- **F4: the joint table is file-local.** `struct Joint`, `g_joints` and `createJoint` live in an anonymous namespace in `PhysicsJoints.cpp:51-138`, so no other file can add a joint API. Phase A moves the entry struct and three helpers to an internal header (section 2.1).
- **C# state.** `Aver.Physics.Joint` is a `readonly struct` that is **not `partial`** (`Joint.cs:16`); `MotorState` has three values (`Enums.cs`). Both change (additively).

### 1.3 Joint ABI facts the design builds on

| Fact | Where |
|---|---|
| Joint handles come from their own counter; world is body handle 0; creation resolves both bodies and locks them in one `BodyLockMultiWrite` | `PhysicsJoints.cpp:61-62,109-138` |
| Units: points in cm, angles in radians; axes normalised but **not** orthogonalised for hinge and slider (the header requires perpendicular normals) | `PhysicsJoints.cpp:146-149,328-395` |
| Breakable joints: SI thresholds, load = applied impulse over the step divided by the step, polled queue of 1024 | `physics_joints_abi.h:267-296`, `PhysicsJoints.cpp:949-969` |
| Six-DOF: translation limits are cm with a +-1e29 free sentinel; Z axes are Jolt's own `cross(X, Y)`, which is the **negative** of the engine's `X x Y` (the basis change has determinant -1) | `PhysicsJoints.cpp:153-164,456-461` |
| Motor strength is kg*cm units (`SetTorqueLimit(x / 1e4)`, `SetForceLimit(x / 100)`) | `PhysicsJoints.cpp:726-742` |
| The world-body convention: handle 0 as `bodyB` is `Body::sFixedToWorld` | `PhysicsJoints.cpp:94-98` |
| Per-step hook for joints: `afterStepJoints()` after `system.Update` (breaks only) | `PhysicsWorld.cpp:493` |
| Tests: `JointTest` (written against the header alone), `JointBreakTest`; the one motor-refusal test uses a point joint, so enabling motors on swing-twist and path breaks nothing | `tests/physics/src/JointTest.cpp:467-480` |
| ABI parity: the physics headers are paired with `Native.cs` by name; springs and breaks live in `Native.Joints2.cs`; the vehicle v2 header has its own row | `tests/abi/src/AbiEnumTest.cpp:560-577` |
| Docs: `docs/physics/JOINTS_AND_BODIES.md` (73 lines), `docs/physics/GRAPH_NODES.md` (91) | |

### 1.4 C# and Aver Node today

| Fact | Where |
|---|---|
| `Joint` (208 lines): 12 creators, `Remove`, `Bodies`, `Enabled`, `SetMotor`, `SetMotorStrength`, `SetLimits`, `SetMotorSpring`, `SetLimitSpring`, `SetBreak`, `Value`; natives in `Native.cs:142-166` and `Native.Joints2.cs` | `scripting/csharp/Aver.Physics/Joint.cs:16-207` |
| `Physics` is `static partial`; `Body` is `readonly partial struct`; `ShapeHit`, `QueryFlags`, nine query methods | `Queries.cs:15-195` |
| Legacy graph rows live in the hand-written palette: `JointFixed` ... `JointSlider`, `JointSetMotor` (always axis 0, three states), `JointSetEnabled`, `JointRemove`, `GetJointValue`, `SetJointBreakable`, and `BoxCast`, `CapsuleCast`, `OverlapBox`, `RaycastAll`, `BodiesAtPoint` | `sandbox/src/GraphNodeDefs.hpp:283-335` |
| No node writes friction, limits, springs, swing-twist or path; no graph row takes a shape handle | |
| The wave 9 family contract (table, palette, owner class, tests, `AN_` prefix) and the Vehicle family as the worked example | `GraphVehicleNodeTable.cs:1-40`, `GraphNodeDefsVehicle.hpp`, `VehicleNodeTests.cs` |
| Graph tests run in a bare process with no native physics library: they prove row wiring, defaults and "returns false with no library"; Jolt's answers are the C++ tests' job | `PhysicsWave1bNodeTests.cs:8-15` |

### 1.5 Systems this area reuses

| System | State | Where |
|---|---|---|
| Parameter-block style (`const float* p, int32_t n`, shorter allowed, longer refused, index constants mirrored in C#) | in use: vehicle v2 blocks | `physics_vehicle_ext_abi.h`, `AbiEnumTest.cpp:792-817` |
| `aver_phys_last_error` reasons on refusal | in use | `physics_abi.h:82-98` |
| Own-file ABI version define plus version function | pattern from the vehicle v2 header | `physics_vehicle_ext_abi.h` |
| Entity stamp on bodies and characters (`GetUserData`) | read by every query that returns an entity | `PhysicsWorld.cpp:772-783` |
| Runtime-registered component pattern (`CCloth`, `CVehicle`) | for `CJointTuning` (Phase G) | `docs/plans/wave10/VEHICLES.md` section 2.6.1 |
| Editor-only line overlays (`rhi::LineVertex`) | for the optional joint overlay | `sandbox/src/SoftBodyDebugOverlay.hpp`, `SandboxRender.cpp:1995-2021` |

### 1.6 The authored joint and its limits

`CJoint` is a 64-byte built-in component (`Components.hpp:389-433`, `static_assert` at `:485`, registered `Builtins.cpp:166-179`) with six types (fixed, point, distance, hinge, slider, cone), one motor (`motorState`, `motorTarget`), an anchor in entity-local space and no spare bytes. `sandbox/src/PhysicsSceneSync.cpp:139-187` builds it with the world-space creators at Play. **Only `SandboxPlay.cpp:152,686` call that sync**; the packaged runtime has no equivalent (`grep syncPhysicsFromScene` outside the sandbox is empty), so an authored joint does nothing in a shipped game today. That is out of scope here (question 11); the Phase G upgrade is editor Play only and says so.

---

## 2. Design

### 2.1 Architecture

```
 physics_joints_abi.h  (v1, unchanged signatures)      physics_queries_abi.h (unchanged)
 physics_joints_ext_abi.h  (new)                       physics_queries_ext_abi.h (new)
        |                                                      |
 PhysicsJoints.cpp (owned)  PhysicsJointsExt.cpp (new)   PhysicsQueriesExt.cpp  PhysicsBroadphaseQueries.cpp (new)
 PhysicsJointPath.cpp (new)  PhysicsJointsInternal.hpp   PhysicsQueryHits.hpp/.cpp (hit record, collectors, face/vertex/material)
        |                                                      |  PhysicsQueryShapes.cpp (Phase E: the only file that names the SHAPES_MATERIALS seams)
 Aver.Physics (DLL)  --------------------  C#: Native.JointsExt.cs, JointExt.cs, Native.QueriesExt.cs, QueriesExt.cs
        |
 Aver.Framework: JointGraph, PhysicsQueryGraph  ->  Aver.Graph: GraphJointNodeTable, GraphPhysicsQueryNodeTable  ->  palette hpp
```

**Joints internal header** (`PhysicsJointsInternal.hpp`, src-local like `PhysicsInternal.hpp`): the entry struct moves out of the anonymous namespace.

```cpp
namespace aver::physics::detail {
struct JointEntry {
  JPH::Ref<JPH::Constraint> constraint;  int32_t bodyA = 0, bodyB = 0;
  float breakForceN = 0, breakTorqueNm = 0;                       // v1 break state, bool breakable() as before
  int32_t type = -1, space = 0, swingType = 0;                    // AVER_PHYS_JT_*, JSPACE, SWING
  int32_t coupledA = 0, coupledB = 0;                             // for GEAR / RACK_PINION: the hinge / slider joints
  std::vector<int32_t> couplers;                                  // reverse: gears and racks that reference this joint
  JPH::Ref<JPH::PathConstraintPath> path;  std::vector<float> arcCm; int32_t arcPerSegment = 0;   // PATH only: cumulative arc length table
};
JointEntry* findJointEntry(int32_t h);                            // null without a world
int32_t registerJoint(JPH::Constraint*, int32_t a, int32_t b, int32_t type, int32_t space);
template <typename Build> int32_t createJointBetween(int32_t a, int32_t b, Build&&);   // today's createJoint, unchanged body
void releaseJointsOfBody(int32_t bodyHandle);                     // F1; also drops couplings and break counts
}
```

`PhysicsJoints.cpp` includes it and keeps `g_joints` (definition moves to `PhysicsJointsInternal` via `extern`). `registerJoint` is what `createJoint` already does after `AddConstraint`; the v1 creators pass their `type` so `aver_phys_joint_type` answers for them.

**Joint lifetime (F1).** `aver_phys_remove_body` calls `releaseJointsOfBody(body)` first, beside `releaseVehicleOfBody` (`PhysicsWorld.cpp:539`). It collects handles whose `bodyA` or `bodyB` equals `body`, removes each (`RemoveConstraint`, break count, couplings), in ascending handle order. `aver_phys_body_set_motion_type`'s static-to-movable rebuild already refuses a jointed body; that stays. A removed joint is a dead handle exactly as after `aver_phys_joint_remove`. This changes a crash into a defined behaviour and is the one place the plan edits `PhysicsWorld.cpp` for joints.

**Queries internal header** (`PhysicsQueryHits.hpp`): the hit record, a bounded nearest-N collector, and the post-processing that needs a body lock.

```cpp
struct HitRec {                       // one result, neutral to the query kind
  float distanceCm, point[3], pointQuery[3], normal[3], depthCm, push[3];
  int32_t body, entity, subShape, subShapeQuery, face, material, vertex, flags;
};
template <typename Result> class NearestNCollector;    // keeps the N best by GetEarlyOutFraction, updates its early-out when full
void  fillBodyFields(HitRec&, const JPH::BodyID&, const JPH::SubShapeID&, JPH::RVec3 hitPoint);   // entity, flags, face, vertex, material under one BodyLockRead
void  writeHits(const HitRec*, int32_t n, float* outF, int32_t* outI);                              // packs HF_* / HI_*
Vec3  outPosition(JPH::RVec3 p);       // the single place a Jolt position becomes an engine float (large-world seam, question 8)
```

Hits are sorted with a stable comparator `(key, body handle, sub-shape)` before packing. Queries run on the game thread between steps like the existing ones; none is supported from a Jolt job thread.

### 2.2 Queries: `physics_queries_ext_abi.h`

Units and axes are the engine's; no Jolt type crosses the boundary. A **pose** is seven floats `x y z qx qy qz qw` (centimetres; the shape's centre of mass, as `aver_phys_overlap_body` already defines it; an all-zero quaternion is identity, normalised on entry). **scale3** is `sx sy sz`, nullable (1,1,1); a non-uniform scale on a sphere or capsule, a zero component, or an invalid scale for the shape is refused with `aver_phys_last_error` = -6. Every function returns the number of results written (or counted), 0 for a miss **or** a refusal (the reason is in `aver_phys_last_error`), and writes nothing it was not given room for. `maxHits` is clamped to 4096. `outF` needs `maxHits * AVER_PHYS_HF_COUNT` floats, `outI` needs `maxHits * AVER_PHYS_HI_COUNT` ints; **either may be null**, and with both null the call just counts (up to `maxHits`).

```c
#define AVER_PHYS_QUERIES_EXT_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_queries_ext_abi_version(void);               /* 1 */

#define AVER_PHYS_QUERY_INCLUDE_SENSORS 1        /* as physics_queries_abi.h (restated under #ifndef) */
#define AVER_PHYS_QUERY_ONE_PER_BODY    2        /* keep only the best result per body (a compound reports once) */
#define AVER_PHYS_QUERY_SOURCE_BODY     4        /* `shape` is a body handle: its current shape, that body ignored; pose null = its current pose */

/* ---- shape casts ---- */
AVER_PHYS_API int32_t aver_phys_shape_cast(int32_t shape, const float* pose7, const float* scale3,
        const float* displacementCm3, uint32_t layerMask, int32_t ignoreBody, int32_t flags,
        const float* settings, int32_t nSettings, int32_t maxHits, float* outF, int32_t* outI);
/* Sweeps the shape by the displacement; ALL hits nearest first (maxHits 1 = closest only, the cheap path). A shape that starts overlapping reports DISTANCE 0 and DEPTH > 0. */

/* ---- collide: contacts, depth, normals ---- */
AVER_PHYS_API int32_t aver_phys_shape_collide(int32_t shape, const float* pose7, const float* scale3,
        uint32_t layerMask, int32_t ignoreBody, int32_t flags,
        const float* settings, int32_t nSettings, int32_t maxHits, float* outF, int32_t* outI);
/* Every contact of the shape at the pose, DEEPEST FIRST. With QS_MAX_SEPARATION_CM > 0 it also reports near misses with negative DEPTH. */

AVER_PHYS_API int32_t aver_phys_shape_depenetrate(int32_t shape, const float* pose7, const float* scale3,
        uint32_t layerMask, int32_t ignoreBody, int32_t flags, int32_t maxIterations,
        float* outPose7, float* outMovedCm);
/* Moves the shape (position only) out of overlap. Returns 0 refused, 1 resolved (no contact deeper than 0.05 cm), 2 improved but still overlapping after maxIterations (clamped 1..32). outPose7 gets the final pose, outMovedCm the distance moved; either may be null. */

/* ---- closest points ---- */
AVER_PHYS_API int32_t aver_phys_closest_points(int32_t shape, const float* pose7, const float* scale3,
        float maxSeparationCm, uint32_t layerMask, int32_t ignoreBody, int32_t flags,
        int32_t maxHits, float* outF, int32_t* outI);
/* The nearest surface points within maxSeparationCm, NEAREST FIRST (DEPTH negative = separated by that much, positive = overlapping). Implies ONE_PER_BODY unless maxHits > 1 and flag 2 is clear. */

AVER_PHYS_API int32_t aver_phys_closest_point(const float* point3, float maxDistanceCm,
        uint32_t layerMask, int32_t ignoreBody, int32_t flags, float* outF, int32_t* outI);
/* The single nearest surface to a point: DISTANCE is SIGNED (negative when the point is inside a convex solid; a mesh has no inside). Built as a tiny-sphere collide with the sphere's radius removed. */

/* ---- rays with the full record and settings ---- */
AVER_PHYS_API int32_t aver_phys_raycast_hits(const float* origin3, const float* direction3, float maxDistanceCm,
        uint32_t layerMask, int32_t ignoreBody, int32_t flags,
        const float* settings, int32_t nSettings, int32_t maxHits, float* outF, int32_t* outI);
AVER_PHYS_API int32_t aver_phys_body_raycast(int32_t body, const float* origin3, const float* direction3,
        float maxDistanceCm, int32_t flags, const float* settings, int32_t nSettings,
        int32_t maxHits, float* outF, int32_t* outI);
/* The same ray against ONE body (Jolt TransformedShape): the bodies in front of it do not matter. A convex hit then its exit needs QS_BACKFACE_CONVEX 1. */
AVER_PHYS_API int32_t aver_phys_shape_raycast(int32_t shape, const float* pose7, const float* scale3,
        const float* origin3, const float* direction3, float maxDistanceCm, int32_t flags,
        const float* settings, int32_t nSettings, int32_t maxHits, float* outF, int32_t* outI);
/* A ray against a shape at a pose with no body (placement tools, gizmo picking). Phase E. */

AVER_PHYS_API int32_t aver_phys_hit_face(int32_t body, int32_t subShapeId, const float* directionHint3,
        float* outVertsXyz, int32_t maxVertices);
/* World-space polygon of the face a sub-shape id names (3 for a triangle, 4 for a box side): for decal orientation and footstep surfaces. directionHint3 picks the face when the id names a convex shape. Returns the vertex count. */

/* ---- broad phase (bounding boxes only, no shape test) ---- */
AVER_PHYS_API int32_t aver_phys_broadphase_aabb(const float* min3, const float* max3, uint32_t layerMask,
        int32_t ignoreBody, int32_t flags, int32_t* outBodies, int32_t maxCount);
AVER_PHYS_API int32_t aver_phys_broadphase_sphere(const float* centre3, float radiusCm, uint32_t layerMask,
        int32_t ignoreBody, int32_t flags, int32_t* outBodies, int32_t maxCount);
AVER_PHYS_API int32_t aver_phys_broadphase_ray(const float* origin3, const float* direction3, float maxDistanceCm,
        uint32_t layerMask, int32_t ignoreBody, int32_t flags, int32_t maxCount,
        int32_t* outBodies, float* outEntryCm);
AVER_PHYS_API int32_t aver_phys_world_bounds(float* outMin3, float* outMax3);     /* 0 with no bodies */
```

Broad-phase results are body handles in ascending handle order (the ray one in entry-distance order), filtered for `ignoreBody` and sensors after the Jolt call (Jolt's `BroadPhaseQuery` takes layer filters only, so the body filter is applied by a short `BodyLockRead`).

**Hit record.**

| Float `AVER_PHYS_HF_` | Index | Meaning |
|---|---|---|
| `DISTANCE` | 0 | cm along the ray or cast (for `closest_*` the signed surface distance; for collide, 0) |
| `POINT` | 1..3 | contact or hit point on the HIT body's surface, world cm |
| `POINT_QUERY` | 4..6 | contact point on the query shape (equals `POINT` for a ray) |
| `NORMAL` | 7..9 | unit vector away from the hit surface toward the query shape (rays: the surface normal at the hit, as `raycast_all` reports it) |
| `DEPTH` | 10 | penetration depth cm; negative = separated; 0 for a plain ray or a clean cast hit |
| `PUSH` | 11..13 | `NORMAL * DEPTH`: the displacement that resolves this contact for the query shape (0 when separated) |
| `COUNT` | 14 | |

| Int `AVER_PHYS_HI_` | Index | Meaning |
|---|---|---|
| `BODY` | 0 | body handle (a character's own handle for a character hit) |
| `ENTITY` | 1 | `aver_phys_set_entity` stamp, 0 if none |
| `SUBSHAPE` | 2 | Jolt `SubShapeID` value on the hit body; pass to `aver_phys_hit_face` |
| `SUBSHAPE_QUERY` | 3 | sub-shape id on the query shape (which part of a compound query shape touched); -1 for a ray |
| `FACE` | 4 | soft body: Jolt face index; mesh: the source triangle index **when SHAPES_MATERIALS builds the mesh with per-triangle user data** (section 2.7), else -1; other shapes -1 |
| `MATERIAL` | 5 | SHAPES_MATERIALS material id of the hit face, 0 = default or none |
| `VERTEX` | 6 | soft body only: the particle index, among the hit face's three, nearest the hit point; else -1 |
| `FLAGS` | 7 | `AVER_PHYS_HIT_BACKFACE 1`, `_STARTED_INSIDE 2` (ray origin inside a convex solid, or a cast that began overlapping), `_SENSOR 4`, `_SOFTBODY 8`, `_CHARACTER 16` |
| `COUNT` | 8 | |

**Settings block** `AVER_PHYS_QS_*` (defaults are Jolt's own so an absent block matches `raycast_all` and the existing casts):

| Index | Name | Default | Used by |
|---|---|---|---|
| 0 | `BACKFACE_TRIANGLES` (0 ignore, 1 collide) | 0 | rays, casts, collide (mesh and heightfield back faces) |
| 1 | `BACKFACE_CONVEX` | 0 | rays (exit hits), casts (moving out of an object) |
| 2 | `TREAT_CONVEX_AS_SOLID` | 1 | rays (0 = a ray starting inside a convex shape sees only its exit, with `BACKFACE_CONVEX` 1) |
| 3 | `ACTIVE_EDGES_ALL` (0 only active edges) | 0 | collide, casts |
| 4 | `DEEPEST_POINT` | 0 | casts: a cast that starts overlapped computes the deepest point (more CPU) |
| 5 | `MAX_SEPARATION_CM` | 0 | collide, closest points |
| 6 | `EXTRA_RADIUS_CM` | 0 | casts: inflates the query shape |
| 7 | `SHRUNKEN` | 0 | casts: `mUseShrunkenShapeAndConvexRadius` |
| 8..10 | `EDGE_DIR_X/Y/Z` (cm vector, 0 = none) | 0 | collide, casts: `mActiveEdgeMovementDirection` |
| 11 | `REMOVE_INTERNAL_EDGES` | 0 | collide: `CollideShapeWithInternalEdgeRemoval` (ghost-collision filter on meshes) |
| 12 | `TOLERANCE_CM` | 0.01 | collide, casts: `mCollisionTolerance` |
| | `COUNT` | 13 | |

A setting that does not apply to the call is ignored.

**Behaviour notes (the contract the tests pin).**
- `shape_collide` and `closest_*` use `NarrowPhaseQuery::CollideShape` with an `AllHitCollisionCollector` replaced by the bounded collector; `shape_cast` with `maxHits` 1 keeps `ClosestHitCollisionCollector` so the common case costs what `castClosest` costs today.
- `NORMAL` for casts and collides is `-normalise(penetrationAxis)`, the sign `castClosest` already uses (`PhysicsQueries.cpp:128-130`). `DEPTH` is `mPenetrationDepth` in cm; `PUSH` is `NORMAL * max(DEPTH, 0)`.
- **Depenetration algorithm (ours, UNVERIFIED):** each iteration collides, takes up to four deepest contacts with pairwise distinct normals, applies Gauss-Seidel pushes (`push_i = normal_i * (depth_i - dot(moved, normal_i) + 0.01 cm)`, four passes), then re-collides. It terminates at max depth < 0.05 cm. A box wedged into a corner converges in two or three iterations; a shape that cannot fit returns 2.
- `closest_point`: a sphere of radius 0.05 cm (Jolt rejects zero) with `MAX_SEPARATION` = `maxDistanceCm + 0.05`; `DISTANCE = 0.05 - DEPTH`, so a point 30 cm from a wall reports 30 and a point 10 cm inside a box reports -10. The 0.05 cm radius is the accuracy floor.
- Meshes, heightfields and soft bodies are refused as the **query** shape (`usableAsQueryShape`, `PhysicsQueries.cpp:62-76`, reused) and fine as the thing hit. A soft body as the hit body reports `FACE`, `VERTEX` and `FLAG_SOFTBODY`; the three particle indices of the face come from `SoftBodyMotionProperties::GetFace`, so `FACE` is Jolt's face index, not the builder's input triangle index when degenerate triangles were skipped (`PhysicsSoftBody.cpp:453`); `VERTEX` is the useful field and indexes the builder's vertices directly (feed it to `aver_phys_sb_add_velocity`).
- Cost: each result takes one short `BodyLockRead` for the entity, face and material fields; that is O(results), not O(candidates).

### 2.3 Joints: `physics_joints_ext_abi.h`

Same conventions as `physics_joints_abi.h`: engine units and axes, angles in radians, `AVER_PHYS_WORLD_BODY` 0, 1 or 0 returns, a created joint returns its handle. Parameter blocks may be shorter than defined (the rest default) and are refused if longer.

#### 2.3.1 Creation with frames and a space

A **frame** is nine floats: position (cm), axis X, axis Y. X is the joint's primary axis (hinge axis, slider axis, twist axis, cone axis, gear axis); Y is the secondary (hinge and slider normal, swing-twist plane axis); Z is implied (Jolt computes it). Axes are normalised and Y is orthogonalised against X; parallel axes are refused (-6).

```c
#define AVER_PHYS_JOINTS_EXT_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_joints_ext_abi_version(void);          /* 1 */

#define AVER_PHYS_JT_FIXED 0
#define AVER_PHYS_JT_POINT 1
#define AVER_PHYS_JT_DISTANCE 2
#define AVER_PHYS_JT_HINGE 3
#define AVER_PHYS_JT_SLIDER 4
#define AVER_PHYS_JT_CONE 5
#define AVER_PHYS_JT_SWING_TWIST 6
#define AVER_PHYS_JT_SIX_DOF 7
#define AVER_PHYS_JT_GEAR 8
#define AVER_PHYS_JT_RACK_PINION 9
#define AVER_PHYS_JT_PULLEY 10
#define AVER_PHYS_JT_PATH 11            /* read back only: made by aver_phys_joint_path / _path_create */
#define AVER_PHYS_JT_COUNT 12
#define AVER_PHYS_JSPACE_WORLD 0
#define AVER_PHYS_JSPACE_LOCAL 1        /* frames are body-local (centre of mass); Jolt LocalToBodyCOM */
#define AVER_PHYS_SWING_CONE 0
#define AVER_PHYS_SWING_PYRAMID 1

AVER_PHYS_API int32_t aver_phys_joint_create_ex(int32_t type, int32_t space, int32_t bodyA, int32_t bodyB,
        const float* frameA9, const float* frameB9, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_joint_type(int32_t joint);              /* AVER_PHYS_JT_*, -1 for a dead handle */
AVER_PHYS_API int32_t aver_phys_joint_handles(int32_t* out, int32_t cap);   /* live joints, ascending handle order; returns the count */
```

**World space:** `frameA9` is the frame in world cm, used for both bodies unless `frameB9` is non-null (distance, pulley, gear, rack and any joint that should pull two different points together); the relative pose is recorded at creation, as every v1 creator does. **Local space:** both frames are required and are relative to each body's centre of mass and rotation; the joint pulls the bodies into the relation the two frames describe, which is what makes "attach this prop to that hand bone" and "door ajar by 30 degrees at rest" expressible. For `bodyB` = world, `frameB9` is a world frame in either space.

| Type | `p` block (positional) | Frame use |
|---|---|---|
| FIXED | none | X, Y |
| POINT | none | position |
| DISTANCE | min cm, max cm (negative = current separation, **world space only**; local refuses a negative) | position per body |
| HINGE | min rad, max rad (as `aver_phys_joint_hinge`: clamped to [-pi, 0] and [0, pi]; both 0 refused) | X hinge axis, Y normal |
| SLIDER | min cm, max cm (clamped to <= 0 and >= 0; both 0 refused) | X slider axis, Y normal |
| CONE | half cone rad (clamped [0, pi]) | X twist axis |
| SWING_TWIST | normal half cone, plane half cone, twist min, twist max (rad), **swing type** (0 cone, 1 pyramid) | X twist axis, Y plane axis |
| SIX_DOF | 6 minimums (3 cm then 3 rad), 6 maximums, **swing type** (13 values; min > max locks, +-1e29 frees, as v1) | X, Y |
| GEAR | ratio (as `aver_phys_joint_gear`) | each frame's X is that body's hinge axis |
| RACK_PINION | ratio rad per cm | A's X = hinge (pinion), B's X = slider (rack) |
| PULLEY | ratio, min cm, max cm (negative = current), then fixed point A (3), fixed point B (3), world cm | the two frame positions are the rope attachment points |

Cone swing type needs symmetric limits; asymmetric limits with `SWING_CONE` are refused with a warning (Jolt's own rule). **The Z-axis caveat of v1 carries over unchanged**: Jolt's frame Z is `cross(X, Y)` in its right-handed space, the negative of the engine's `X x Y`, so a six-DOF translation or rotation limit on frame Z acts mirrored relative to the engine frame (`PhysicsJoints.cpp:456-461`). The new call does not compensate (question 2); the test pins the sign so a later change is deliberate.

Existing creators are re-expressed through `registerJoint` so `aver_phys_joint_type` answers for them; their behaviour does not change.

#### 2.3.2 Setters on an existing joint

All return 1 or 0 (`aver_phys_last_error` explains a 0). "Axis" is the six-DOF axis (`AVER_PHYS_DOF_*`) and is ignored by joints with one motor; swing-twist uses `AVER_PHYS_ST_SWING 0`, `AVER_PHYS_ST_TWIST 1`.

```c
#define AVER_PHYS_MOTOR_POSITION_VELOCITY 3        /* Jolt PositionAndVelocity: a spring toward a position AND a damper toward a velocity */
#define AVER_PHYS_ST_SWING 0
#define AVER_PHYS_ST_TWIST 1
#define AVER_PHYS_SPRING_FREQUENCY 0               /* a = Hz, b = damping ratio (what the v1 calls use) */
#define AVER_PHYS_SPRING_STIFFNESS 1               /* a = N/m or N*m/rad, b = N*s/m or N*m*s/rad */
#define AVER_PHYS_SPRING_MASS_NORMALISED 2         /* a = 1/s^2, b = 1/s: mass independent */

AVER_PHYS_API int32_t aver_phys_joint_set_motor_targets(int32_t joint, int32_t axis, int32_t state,
        float targetPosition, float targetVelocity);
/* hinge rad / rad per s, slider cm / cm per s, six-DOF per axis (a rotation axis position target replaces the joint's whole target orientation, as v1), swing-twist part (the position target is ignored: use _set_target_rotation), path: see _path_move. State 3 sets both targets. */
AVER_PHYS_API int32_t aver_phys_joint_set_motor_limits(int32_t joint, int32_t axis, float minLoad, float maxLoad);
/* SI: N for a linear axis, N*m for an angular one; asymmetric, minLoad <= maxLoad (a motor that can push but not pull). */
AVER_PHYS_API int32_t aver_phys_joint_set_motor_spring_ex(int32_t joint, int32_t axis, int32_t mode, float a, float b);
AVER_PHYS_API int32_t aver_phys_joint_set_limit_spring_ex(int32_t joint, int32_t axis, int32_t mode, float a, float b);
/* hinge, slider, distance; six-DOF translation axes (axis -1 = all three). A rotation axis is refused (Jolt has no rotation limit spring). */
AVER_PHYS_API int32_t aver_phys_joint_set_friction(int32_t joint, int32_t axis, float maxFrictionNOrNm);
/* SI. hinge and swing-twist: N*m, axis ignored; slider and path: N; six-DOF: per axis. Applies only while the motor is off. 0 removes it. */
AVER_PHYS_API int32_t aver_phys_joint_set_limit_block(int32_t joint, const float* p, int32_t n);
/* The create_ex `p` block for the joint's type, applied live: hinge, slider, distance, pulley (min, max), CONE and SWING_TWIST (the angles; swing type is creation-only and ignored), SIX_DOF (12). */
AVER_PHYS_API int32_t aver_phys_joint_set_target_rotation(int32_t joint, const float* quat4);
AVER_PHYS_API int32_t aver_phys_joint_set_target_angular_velocity(int32_t joint, const float* omega3);
/* SWING_TWIST and SIX_DOF. Both are in the joint's own frame (X, Y as given, Z implied) in the engine's quaternion convention; the implementation negates the x and y vector parts to reach Jolt's constraint space (derived on paper, UNVERIFIED, pinned by JointExtTest 3). */
AVER_PHYS_API int32_t aver_phys_joint_set_solver(int32_t joint, int32_t priority, int32_t velocitySteps, int32_t positionSteps);
/* priority >= 0 (higher is solved more correctly; give every joint a unique value for a deterministic order, Jolt's own advice); steps 0 = the world default, max 255; -1 leaves a value alone. */
AVER_PHYS_API int32_t aver_phys_joint_couple(int32_t gearOrRack, int32_t jointA, int32_t jointB);
/* GEAR: A and B are the hinge joints of the gear's two bodies. RACK_PINION: A the pinion's hinge, B the rack's slider. 0, 0 clears. Refused for the wrong types. Turns on Jolt's position-level ratio correction (F2). Removing A or B clears the coupling. */
```

**Extended v1 calls.** `aver_phys_joint_set_motor(joint, axis, state, target)` accepts SWING_TWIST (axis 0 swing, 1 twist; state only, targets come from `_set_target_*`) and PATH (axis ignored); state 3 is accepted wherever a motor exists and sets the position target from `target` and a zero velocity target (use `_set_motor_targets` for both). `_set_motor_strength` and `_set_motor_spring` accept swing-twist and path with the same kg*cm and Hz semantics as today. `aver_phys_joint_value` returns the twist angle (rad) for swing-twist and the distance along the path (cm) for path. The header comment at `physics_joints_abi.h:209-211` is corrected to list the types that do have motors.

**Why SI for the new load arguments.** Break thresholds are SI because breaking points are quoted that way (`physics_joints_abi.h:275-278`); friction and motor limits are the same kind of number (a door's friction is "0.5 N*m"). The risk is a caller mixing the old `_set_motor_strength` (kg*cm: 1 N*m = 10,000) with the new SI calls; the parameter names carry the unit (`maxFrictionNOrNm`, `minLoad` documented N or N*m) and the header says so at the top of the block. Conversion inside is `N*m` direct to Jolt (Jolt is SI).

#### 2.3.3 Path joints

```c
#define AVER_PHYS_PATH_ROT_FREE 0               /* same order as Jolt EPathRotationConstraintType */
#define AVER_PHYS_PATH_ROT_AROUND_TANGENT 1
#define AVER_PHYS_PATH_ROT_AROUND_NORMAL 2
#define AVER_PHYS_PATH_ROT_AROUND_BINORMAL 3
#define AVER_PHYS_PATH_ROT_TO_PATH 4            /* the body's rotation follows the path's tangent and normal: a car on rails */
#define AVER_PHYS_PATH_ROT_FIXED 5              /* rotation fully tied to body A's */
#define AVER_PHYS_JPA_ROTATION 0                /* default FREE */
#define AVER_PHYS_JPA_START_DISTANCE_CM 1       /* where body B starts; default -1 = the point nearest body B */
#define AVER_PHYS_JPA_FRICTION_N 2              /* default 0 */
#define AVER_PHYS_JPA_RESAMPLE_CM 3             /* default 100; 0 keeps the points as given */
#define AVER_PHYS_JPA_MAX_SLIDE_CM 4            /* default -1 = the whole path; open paths only, as v1 */
#define AVER_PHYS_JPA_COUNT 5

AVER_PHYS_API int32_t aver_phys_joint_path_create(int32_t bodyA, int32_t bodyB, int32_t space,
        const float* pointsCm, const float* tangentsCm, const float* normalsCm, int32_t pointCount,
        int32_t closed, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_joint_path_move(int32_t joint, int32_t state, float targetDistanceCm, float targetSpeedCmS);
AVER_PHYS_API int32_t aver_phys_joint_path_sample(int32_t joint, int32_t count, float* outXyz);   /* count >= 2 points, evenly by arc length, world cm; returns count */
```

`tangentsCm` and `normalsCm` are nullable, `pointCount * 3` floats each: Hermite tangents (their length sets the parameter speed, as Jolt) and reference normals (any vector not parallel to the tangent; Jolt rebuilds the frame). Null tangents keep v1's Catmull-Rom derivation; null normals keep v1's up-axis reference with the vertical fallback. `space` is `JSPACE_WORLD` (as v1: the path is recorded in body A's current frame) or `JSPACE_LOCAL` (points are relative to body A's centre of mass). At least two points; at most 65,535; a closed path must not repeat its first point.

**Resampling and the position motor (UNVERIFIED, central).** Jolt's `PathConstraintPathHermite` parameterises by control point (fraction 0 to N-1) and the position motor's error is `mPathFraction - mTargetPathFraction` fed into the spring **in those units as if they were metres** (`PathConstraint.cpp:113,121`). With control points a few metres apart the motor is `1/spacing` stiffer than its Hz setting says and the effect varies along an unevenly spaced path. The default `JPA_RESAMPLE_CM` 100 evaluates the authored spline exactly (cubic Hermite from the given or derived tangents), places points at equal arc length 100 cm apart (the last segment shorter is merged into its neighbour), and gives each the finite-difference tangent scaled to the new spacing, so fraction equals metres of arc to within the spline's arc-length error and `PATH_FRACTION * 100 ~ distance`. The original points are no longer control points (the curve passes within the resampler's tolerance, 1 cm, of each); a caller who needs them exactly passes `RESAMPLE_CM` 0 and accepts the spacing-dependent motor. A cap of 16,384 resampled points refuses a path longer than 1.6 km at 100 cm. The joint keeps an arc-length table (16 samples per segment) for distance-to-fraction and back; v1 paths build the same table lazily from their constraint's path.

`_path_move(joint, state, targetDistanceCm, targetSpeedCmS)`: state 0 off, 1 velocity (`SetTargetVelocity(speed / 100)`, m/s along the tangent, exact), 2 position (`SetTargetPathFraction(fraction(distance))`, clamped on open paths, wrapped on closed), 3 both. Strength, spring and friction use `_set_motor_strength`, `_set_motor_spring(_ex)` (axis ignored) and `_set_friction`.

#### 2.3.4 Readback

```c
AVER_PHYS_API int32_t aver_phys_joint_state(int32_t joint, float* outF, int32_t nF, int32_t* outI, int32_t nI);
/* writes min(n, defined) entries into each (either may be null); returns the number of floats written, 0 for a dead handle */
AVER_PHYS_API int32_t aver_phys_joint_frames(int32_t joint, float* outFrameA9, float* outFrameB9);   /* the constraint's two frames in WORLD space now (cm), the same layout create_ex takes */
```

| Float `AVER_PHYS_JS_` | Index | Meaning (types it is defined for; others 0) |
|---|---|---|
| `VALUE` | 0 | hinge angle rad; slider offset cm; distance current length cm; pulley current length cm; path distance cm |
| `VELOCITY` | 1 | rate of VALUE (hinge exact; slider and path from centre-of-mass velocities, exact without rotation) |
| `LIMIT_MIN`, `LIMIT_MAX` | 2, 3 | the active limits in VALUE's unit |
| `TWIST_RAD`, `SWING_Y_RAD`, `SWING_Z_RAD` | 4, 5, 6 | swing-twist and six-DOF: the rotation in constraint space decomposed as swing * twist, swing angles in Jolt's own `4 atan(q / (1 + w))` measure (the one its limits use, so they compare directly with the half-cone angles) |
| `TRANS_X_CM`, `TRANS_Y_CM`, `TRANS_Z_CM` | 7, 8, 9 | six-DOF: relative translation in constraint space |
| `LOAD_LINEAR_N`, `LOAD_ANGULAR_NM` | 10, 11 | the solver's constraint load over the last fixed step (impulse / step), the quantity the break check uses (`appliedImpulse`, `PhysicsJoints.cpp:172-238`) |
| `LOAD_MOTOR`, `LOAD_LIMIT` | 12, 13 | motor (or friction) load and limit load, N or N*m, where the type exposes them |
| `PATH_FRACTION`, `PATH_LENGTH_CM` | 14, 15 | path only |
| `TARGET` | 16 | the current motor position target in VALUE's unit |
| `COUNT` | 17 | |

| Int `AVER_PHYS_JSI_` | Index | Meaning |
|---|---|---|
| `TYPE` | 0 | `AVER_PHYS_JT_*` |
| `ENABLED` | 1 | |
| `MOTOR_STATE` | 2 | axis 0 (swing for swing-twist) |
| `AT_LIMIT` | 3 | -1 at or beyond the minimum, +1 the maximum, 0 free (within 1 percent of the range, hinge, slider, distance, pulley) |
| `SPACE` | 4 | `JSPACE_*` the joint was made in |
| `PRIORITY`, `VELOCITY_STEPS`, `POSITION_STEPS` | 5, 6, 7 | as set |
| `BODY_A`, `BODY_B` | 8, 9 | |
| `COUPLED_A`, `COUPLED_B` | 10, 11 | for gear and rack: the linked joints |
| `SWING_TYPE` | 12 | |
| `COUNT` | 13 | |

Loads are read after a step, so they lag one fixed step like `joint_break_get`. `aver_phys_joint_frames` is what an overlay or a "show the hinge" gizmo needs; it adds no per-step cost.

#### 2.3.5 Recipes (what a gameplay script calls)

| Goal | Calls |
|---|---|
| Door with friction (stops where released) | hinge with limits; `_set_friction(joint, 0, 0.8)` (N*m, UNVERIFIED) |
| Door with viscous damping | `_set_motor_spring_ex(joint, 0, STIFFNESS, 0, c)` then `_set_motor_targets(joint, 0, POSITION_VELOCITY, 0, 0)` with a high `_set_motor_limits` (stiffness 0, damping c: pure damper) |
| Self-closing door | stiffness mode k and c, `POSITION_VELOCITY`, target position = closed angle, velocity 0; turn the motor off while the player pushes |
| Locked door | `_set_limit_block(joint, {-0.01, 0.01})`; unlock with the real range |
| Powered door or lift | velocity motor with `_set_motor_limits` capping the load so it stalls on an obstacle |
| Platform on a rail | `_path_create` with `RESAMPLE_CM` 100, `_path_move(joint, 1, 0, 200)` for a constant 2 m/s, or state 2 to a stop |
| Rail car that banks | `JPA_ROTATION` = `TO_PATH` |
| Turret or robot joint | swing-twist, `_set_motor(joint, 0, POSITION, 0)`, `_set_target_rotation`, motor spring and limits |
| Soft rotation toward a rest pose (the survey's "rotation-limit spring") | six-DOF position motors on the rotation axes, `_set_motor_spring_ex(..., STIFFNESS, k, c)`, `_set_target_rotation` = rest; hard rotation limits stay as the backstop |
| Gear train that does not drift | hinges on both gears, then `_joint_gear`, then `_couple(gear, hingeA, hingeB)` |
| Prop attached by a prefab offset | `create_ex` with `JSPACE_LOCAL` and the two frames |

### 2.4 C# API (Aver.Physics)

`Joint` becomes `partial`; `MotorState` gains `PositionAndVelocity = 3`. New files: `Native.JointsExt.cs` and `Native.QueriesExt.cs` (P/Invokes, names copied from the headers character for character), `JointExt.cs`, `QueriesExt.cs`.

```csharp
public enum JointType { Fixed, Point, Distance, Hinge, Slider, Cone, SwingTwist, SixDof, Gear, RackAndPinion, Pulley, Path }
public enum JointSpace { World, Local }
public enum SwingType { Cone, Pyramid }
public enum SpringMode { Frequency, Stiffness, MassNormalised }
public enum PathRotation { Free, AroundTangent, AroundNormal, AroundBinormal, ToPath, Fixed }
public readonly record struct JointFrame(Float3 Position, Float3 AxisX, Float3 AxisY);   // + static JointFrame At(Float3 p)
public sealed class PathOptions { PathRotation Rotation; float StartDistanceCm = -1, FrictionN, ResampleCm = 100, MaxSlideCm = -1; JointSpace Space; }
public readonly struct JointState { /* every JS_ and JSI_ entry as a named property; AtLimit as an enum */ }

public readonly partial struct Joint {
  public static Joint Create(JointType t, JointSpace s, Body a, Body b, JointFrame fa, JointFrame? fb, params float[] p);
  public static Joint HingeLocal(Body a, Body b, JointFrame fa, JointFrame fb, float minRad, float maxRad);   // + SliderLocal, SwingTwistEx(..., SwingType), SixDofEx(..., SwingType), FixedLocal
  public static Joint PathEx(Body a, Body b, Float3[] points, Float3[]? tangents, Float3[]? normals, bool closed, PathOptions? o = null);
  public JointType Type { get; }
  public bool SetFriction(float maxFrictionNOrNm, int axis = 0);
  public bool SetMotorTargets(MotorState state, float position, float velocity, int axis = 0);
  public bool SetMotorLimits(float minLoad, float maxLoad, int axis = 0);
  public bool SetMotorSpring(SpringMode mode, float a, float b, int axis = 0);
  public bool SetLimitSpring(SpringMode mode, float a, float b, int axis = -1);
  public bool SetLimits(ReadOnlySpan<float> block);
  public bool SetTargetRotation(Quaternion q);   public bool SetTargetAngularVelocity(Float3 w);
  public bool SetSolver(int priority, int velocitySteps = 0, int positionSteps = 0);
  public bool Couple(Joint a, Joint b);
  public bool PathMove(MotorState state, float targetDistanceCm, float speedCmS);
  public Float3[] PathSample(int count);
  public JointState? State();   public (JointFrame A, JointFrame B)? Frames();
  public static Joint[] All();
}
```

Queries: `Physics.ShapeCast`, `ShapeCollide`, `Depenetrate`, `ClosestPoints`, `ClosestPoint`, `RaycastHits`, `BroadphaseAabb`, `BroadphaseSphere`, `BroadphaseRay`, `WorldBounds`, `HitFace`, and `Body.Raycast`, `Body.ClosestPoint`, `Body.Collide` (the body as the query shape: sets `SOURCE_BODY`).

```csharp
[Flags] public enum HitFlags { BackFace = 1, StartedInside = 2, Sensor = 4, SoftBody = 8, Character = 16 }
public readonly struct QueryHit { Body Body; int Entity; float Distance, Depth; Float3 Point, PointOnQuery, Normal, Push; uint SubShape; int SubShapeQuery, Face, Vertex, Material; HitFlags Flags; }
public struct QuerySettings { bool BackFaceTriangles, BackFaceConvex; bool TreatConvexAsSolid = true; bool AllActiveEdges, DeepestPoint, Shrunken, RemoveInternalEdges;
                              float MaxSeparationCm, ExtraRadiusCm, ToleranceCm = 0.01f; Float3 EdgeDirection; public static QuerySettings Default; }
public static partial class Physics {
  public static QueryHit[] ShapeCast(Shape shape, Float3 position, Quaternion rotation, Float3 displacement, QuerySettings? s = null, int maxHits = 1,
                                     uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None);
  // + ShapeCollide(...), ClosestPoints(...), RaycastHits(origin, direction, maxDist, ...), Depenetrate(Shape, position, rotation, out Float3 resolved, int maxIterations = 8) -> DepenetrateResult
}
```

`Shape` is SHAPES_MATERIALS' C# type (section 2.7); until it lands the overloads take `int shapeHandle`. Arrays are allocated per call (this is the scripting surface; the graph owners reuse buffers). `QueryFlags` gains `OnePerBody = 2`, `SourceBody = 4` (the C# `IncludeSensors = 1` is unchanged).

### 2.5 Aver Node

Two families in the wave 9 C contract: table `GraphJointNodeTable.cs` / `GraphPhysicsQueryNodeTable.cs` (a `JointTable()` / `PhysicsQueryTable()` inside `partial class GameSystemNodes`), palette `sandbox/src/GraphNodeDefsJoint.hpp` / `GraphNodeDefsPhysicsQuery.hpp` (`appendJointNodeDefs`, `appendPhysicsQueryNodeDefs`), owners `JointGraph` and `PhysicsQueryGraph` (`internal static`, in `Aver.Framework`, methods `...ForGraph`, primitives only, return false and log once per node id, never throw), tests `JointNodeTests.cs` and `PhysicsQueryNodeTests.cs` (`[AutoRun]`, `NodeRowKit`). Category `Physics` for both. Type ids carry the `an_` prefix because the legacy `JointSetMotor`, `GetJointValue`, `BoxCast`, `RaycastAll` ids exist in the hand-written palette. Never edit `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. A joint crosses the graph as its integer handle (not interchangeable with a body handle, as the legacy rows already warn).

**Family `Joint` (11 rows).**

| Type id | Kind | Inputs | Outputs | Attributes | Purpose |
|---|---|---|---|---|---|
| `an_jointsetfriction` | Exec | joint:int, axis:int, friction:float | success | | door and lever friction, N*m or N |
| `an_jointsetlimits` | Exec | joint:int, axis:int, minimum:float, maximum:float | success | | locked and unlocked doors; also live cone limit (minimum ignored) |
| `an_jointsetmotortargets` | Exec | joint:int, axis:int, state:int, position:float, velocity:float | success | | state 0 off, 1 velocity, 2 position, 3 both |
| `an_jointsetmotorspring` | Exec | joint:int, axis:int, mode:int, a:float, b:float | success | | door closer and damper |
| `an_jointsetmotorlimits` | Exec | joint:int, axis:int, minLoad:float, maxLoad:float | success | | powered door that stalls; SI |
| `an_jointsetswingtwistmotor` | Exec | joint:int, part:int, state:int, yaw:float, pitch:float, roll:float | success | | turret and arm: part 0 swing / 1 twist; the angles (degrees, `Set Local Rotation` convention) set the target rotation when state is position |
| `an_jointgetstate` | Pure | joint:int | found:bool, value:float, velocity:float, atLimit:int, twistDeg:float, swingYDeg:float, swingZDeg:float, loadN:float, loadNm:float, target:float | | door angle, lift height, lock-on tests |
| `an_jointcreatepath` | Exec | bodyA:int, bodyB:int, closed:bool, rotation:int, frictionN:float | joint:int, success | `points` ("x y z; x y z; ..." cm), `space` (world or local) | rail mover; resample 100 cm |
| `an_jointpathmove` | Exec | joint:int, state:int, distanceCm:float, speedCmS:float | success | | 0 off, 1 velocity, 2 position, 3 both |
| `an_jointpathstate` | Pure | joint:int | found:bool, distanceCm:float, lengthCm:float, fraction01:float, speedCmS:float | | arrival and progress |
| `an_jointcouple` | Exec | coupling:int, jointA:int, jointB:int | success | | gear and rack linking |

**Family `PhysicsQuery` (7 rows).** Rows with several hits take `hitIndex` (0 = nearest or deepest) and return `count`; the query re-runs per row (no cached state between nodes, so graphs may interleave freely). `layerMask` 0 means all layers (the legacy rows' rule), `includeSensors` false by default.

| Type id | Kind | Inputs | Outputs |
|---|---|---|---|
| `an_shapecast` | Exec | source:int, shape:int, x, y, z, yaw, pitch, roll, dx, dy, dz, layerMask:int, ignoreBody:int, includeSensors:bool, hitIndex:int | count:int, body:int, entity:int, distance, pointX/Y/Z, normalX/Y/Z, material:int, subShape:int, face:int, success:bool |
| `an_shapeoverlap` | Exec | source, shape, pose (x..roll), layerMask, ignoreBody, includeSensors, hitIndex | count, body, entity, depth, normalX/Y/Z, pushX/Y/Z, pointX/Y/Z, success |
| `an_depenetrate` | Exec | source, shape, pose, layerMask, ignoreBody, includeSensors, maxIterations:int=8 | resolved:bool, x, y, z, movedCm, success |
| `an_closestpoint` | Exec | x, y, z, maxDist, layerMask, ignoreBody, includeSensors | found:bool, body, entity, distance (signed), pointX/Y/Z, normalX/Y/Z |
| `an_bodyraycast` | Exec | body:int, ox, oy, oz, dx, dy, dz, maxDist, backFaces:bool, hitIndex | count, distance, pointX/Y/Z, normalX/Y/Z, material, face, success |
| `an_raycasthits` | Exec | ox..dz, maxDist, layerMask, ignoreBody, includeSensors, backFaces:bool, hitIndex | count, body, entity, distance, point, normal, material, face, vertex, success |
| `an_broadphasebodies` | Exec | cx, cy, cz, hx, hy, hz, layerMask, ignoreBody, includeSensors, index:int | count, body, success |

`source` 0 is a shape handle (SHAPES_MATERIALS' rows produce them), 1 a body handle. Until SHAPES_MATERIALS lands the shape-handle path answers `success` false, logged once. The rows are listed for `docs/nodes/` (generated by `scripts/node-catalog.py --write`); `docs/nodes/Joint.md` and `PhysicsQuery.md` appear without hand edits.

### 2.6 Editor and authored joints (Phase G, optional)

1. **`CJoint` stays 64 bytes.** `Components.hpp` gains `kJointTypeSwingTwist = 6` (one constant; swing-twist uses `limitMin/limitMax` for the twist range, `normalAxis` as the plane axis, and the cone angles from the tuning component). The comment at `Components.hpp:365-370` ("code-only" for swing-twist) is corrected.
2. **`CJointTuning`**, runtime-registered by `PhysicsSceneSync` (`registerJointTuning(world)`, called from `SandboxInit`), 48 bytes, zero-filled storage meaning "unset": `frictionN` (f32), `motorSpringMode`, `motorSpringA`, `motorSpringB`, `motorMinLoad`, `motorMaxLoad`, `normalHalfCone`, `planeHalfCone`, `priority` (i32), `velocitySteps`, `positionSteps`, `flags` (bit0 pyramid swing). `createJoint` applies it after creation. A level stores it as an ordinary `COMPONENT CJointTuning` record (one name appended to the type list in `OcWorld.cpp`, a shared edit; the packaged runtime ignores the record with one warning, consistent with section 1.6).
3. **Details section "Joint"** (model header plus widget half, the `SoftBodyDetailsModel` split): the existing `CJoint` fields stay in the generic table; the section adds the tuning fields, a friction slider with the SI value shown, the spring mode combo, and in Play a live `aver_phys_joint_state` read (angle, rate, loads, at-limit lamp). Add Component row "Joint Tuning" (`PlacedKind::JointTuning`, `canAttach` requires `CJoint`).
4. **Overlay `Show > Joints`** (only if question 12 resolves to "build it"): `aver_phys_joint_handles` plus `_frames` plus `_state` give anchors, axes, hinge arcs between the limits, slider ranges, cone and swing ellipses, and `_path_sample` polylines, as `rhi::LineVertex` lines under `!noEditorChrome_`, like `SoftBodyDebugOverlay`. No Jolt debug-renderer dependency.

### 2.7 Cross-area contracts

| Area | What this area needs or provides |
|---|---|
| **SHAPES_MATERIALS** (needs) | Two internal functions in `PhysicsInternal.hpp`, defined in their sources and named in my `PhysicsQueryShapes.cpp` only: `JPH::RefConst<JPH::Shape> shapeByHandle(int32_t shape)` (thread-safe copy-out of the generic shape table; the existing mesh-shape handles are members of it) and `int32_t materialIdOf(const JPH::PhysicsMaterial*)` (0 for the default or null). Optional third: `int32_t meshTriangleIndexOf(const JPH::Shape* leaf, const JPH::SubShapeID& remainder)` returning the source triangle index when the mesh was built with `mPerTriangleUserData`, else -1. Their C# `Shape` struct (an `int Handle`). **Until `shapeByHandle` exists, Phase E is not started, and Phase D writes `HI_MATERIAL` as 0 and mesh `FACE` as -1**; Phase E turns the material and face fills on by calling the two seams from `PhysicsQueryShapes.cpp` (the only file that names them, so the join with SHAPES_MATERIALS is one file and no stub symbol can collide with theirs). Their generic shape handles can be cast (box, sphere, capsule, cylinder, hull, compound); meshes and heightfields are refused as the cast shape. |
| **Large-world precision** (owner decision to enable `JPH_DOUBLE_PRECISION`; owned by another wave 11 area) | These headers keep `float` positions in the engine's existing cm frame. All query code uses `RVec3`, `RMat44`, `RRayCast`, `RShapeCast` as `PhysicsQueries.cpp` already does, and funnels every Jolt-to-engine position through `outPosition` (section 2.1), so whatever origin-rebase or `_d` scheme that area chooses changes one function. Joint frames in world space go through `toJolt` like the v1 creators; the same applies. |
| **SOFTBODY** (provides) | `HI_VERTEX` and `HI_FACE` for soft-body hits feed `aver_phys_sb_add_velocity` / `_impulse` (a poke or grab at the exact particle). No change to `physics_softbody_abi.h`. |
| **Debug renderer / profiler areas** (soft) | If one ships constraint drawing, Phase G step 4 is skipped; `aver_phys_joint_frames` and `_path_sample` remain useful regardless and cost nothing. |
| **VEHICLES, CHARACTER, RAGDOLL** | No change. The ragdoll builds its own swing-twist constraints inside Jolt's `Ragdoll` (`PhysicsRagdoll.cpp:415`) and is untouched; `aver_phys_joint_*` handles do not address them. Vehicle wheel casts use Jolt's own shape casts, not these. |

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this area touches them)

Physics
- modules/physics/include/aver/physics/physics_joints_ext_abi.h
- modules/physics/include/aver/physics/physics_queries_ext_abi.h
- modules/physics/src/PhysicsJointsInternal.hpp
- modules/physics/src/PhysicsJointsExt.cpp
- modules/physics/src/PhysicsJointPath.cpp
- modules/physics/src/PhysicsQueryHits.hpp
- modules/physics/src/PhysicsQueryHits.cpp
- modules/physics/src/PhysicsQueriesExt.cpp
- modules/physics/src/PhysicsBroadphaseQueries.cpp
- modules/physics/src/PhysicsQueryShapes.cpp

C#
- scripting/csharp/Aver.Physics/Native.JointsExt.cs
- scripting/csharp/Aver.Physics/JointExt.cs
- scripting/csharp/Aver.Physics/Native.QueriesExt.cs
- scripting/csharp/Aver.Physics/QueriesExt.cs
- scripting/csharp/Aver.Framework/JointGraph.cs
- scripting/csharp/Aver.Framework/PhysicsQueryGraph.cs
- scripting/csharp/Aver.Graph/GraphJointNodeTable.cs
- scripting/csharp/Aver.Graph/GraphPhysicsQueryNodeTable.cs
- scripting/csharp/Aver.Graph.Tests/JointNodeTests.cs
- scripting/csharp/Aver.Graph.Tests/PhysicsQueryNodeTests.cs

Editor and palette
- sandbox/src/GraphNodeDefsJoint.hpp
- sandbox/src/GraphNodeDefsPhysicsQuery.hpp
- sandbox/src/JointTuningModel.hpp (Phase G)
- sandbox/src/JointTuningSection.cpp (Phase G)
- sandbox/src/JointDebugOverlay.hpp, sandbox/src/JointDebugOverlay.cpp (Phase G, conditional)

Tests
- tests/physics/src/JointExtTest.cpp
- tests/physics/src/QueryExtTest.cpp
- tests/editor/src/JointTuningModelTest.cpp (Phase G)

Content
- test-content/JointLab/README.md and make_joint_lab.py (Phase G; generated `.ocworld`, no new engine feature for a level)

### 3.2 Modified, owned by this area

- modules/physics/src/PhysicsJoints.cpp (entry struct and helpers move to the internal header; creators call `registerJoint`; motor, strength, spring and value switches gain swing-twist and path; gear and rack store their type; `releaseJointsOfBody`; path creator stores its path)
- modules/physics/include/aver/physics/physics_joints_abi.h (comment fixes only: motors at `:209-211`, a pointer to the ext header)
- modules/physics/include/aver/physics/physics_queries_abi.h (comment cross-reference only)
- modules/physics/src/PhysicsQueries.cpp and modules/physics/src/PhysicsQueryFilters.hpp (helpers `usableAsQueryShape`, `copyBodyShape`, `joltRotation` moved to the filters header so the ext file reuses them; no behaviour change)
- scripting/csharp/Aver.Physics/Joint.cs (`partial`), Enums.cs (`MotorState.PositionAndVelocity`), Queries.cs (`QueryFlags` members)
- sandbox/src/PhysicsSceneSync.cpp and PhysicsSceneSync.hpp (Phase G)
- docs/physics/JOINTS_AND_BODIES.md, docs/physics/QUERIES.md, docs/physics/GRAPH_NODES.md

### 3.3 SHARED (another wave 11 area or the integrator also touches these; every edit here is small and additive)

- modules/physics/CMakeLists.txt
- modules/physics/src/PhysicsInternal.hpp
- modules/physics/src/PhysicsWorld.cpp
- modules/physics/include/aver/physics/physics_abi.h (NOT edited; listed because SHAPES_MATERIALS may extend it and my docs refer to it)
- scripting/csharp/Aver.Physics/Native.cs (NOT edited; new natives are in new files)
- tests/abi/src/AbiEnumTest.cpp
- tests/physics/CMakeLists.txt
- modules/scene/include/aver/scene/Components.hpp (Phase G: one constant and a comment)
- modules/formats/src/OcWorld.cpp (Phase G: `"CJointTuning"` appended to the component type list)
- sandbox/CMakeLists.txt, sandbox/src/SandboxInit.cpp, SandboxComponentDetails.cpp, PlacedComponentModel.hpp, SandboxRender.cpp, SandboxShell.cpp (Phase G)
- tests/editor/CMakeLists.txt (Phase G)
- docs/ABI.md, docs/SCRIPTING_API.md, docs/nodes/ (generated), CHANGELOG.md

| Shared file | Edit made by this area | Who else |
|---|---|---|
| `modules/physics/CMakeLists.txt` | `src/PhysicsJointsExt.cpp`, `PhysicsJointPath.cpp`, `PhysicsQueryHits.cpp`, `PhysicsQueriesExt.cpp`, `PhysicsBroadphaseQueries.cpp`, `PhysicsQueryShapes.cpp` | SHAPES_MATERIALS, other physics areas add sources |
| `PhysicsInternal.hpp` | one declaration, `void releaseJointsOfBody(int32_t body);`, beside `releaseVehicleOfBody` (`:393`) | SHAPES_MATERIALS adds `shapeByHandle` / `materialIdOf`; vehicle area owns the rest |
| `PhysicsWorld.cpp` | one call in `aver_phys_remove_body` before `releaseVehicleOfBody` (`:539`) | SHAPES_MATERIALS edits the shape table (`:162-183`) and `aver_phys_remove_body` neighbours |
| `tests/abi/src/AbiEnumTest.cpp` | the two new headers in the `kAbis` physics row (`:560-572`) plus their own `Abi` rows against `Native.JointsExt.cs` (prefix `aver_phys_joint_`) and `Native.QueriesExt.cs` (prefix `aver_phys_`); constant rows for `AVER_PHYS_JT_`, `_JSPACE_`, `_SWING_`, `_SPRING_`, `_PATH_ROT_`, `_JPA_`, `_JS_`, `_JSI_`, `_QS_`, `_HF_`, `_HI_`, `_HIT_` against C# mirror classes with `Count` | SHAPES_MATERIALS adds its rows |
| `tests/physics/CMakeLists.txt` | `JointExtTest`, `QueryExtTest` | every physics area |
| `Components.hpp`, `OcWorld.cpp`, sandbox files | Phase G only, one block each | other areas append their own names |
| `docs/ABI.md`, `docs/SCRIPTING_API.md`, `CHANGELOG.md` | rows and one entry, written by the integrator | every area |

Not touched: `scene_abi.h`, `physics_abi.h`, `Native.cs`, `physics_vehicle*_abi.h`, `physics_softbody_abi.h`, `PhysicsRagdoll.cpp`, `PhysicsVehicle*.cpp`, every shader and `modules/render*`, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`, `framework_abi.h`, the root `CMakeLists.txt` and `cmake/modules.catalog.json` (no new module).

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A S, B M, C M, D M, E S, F M, G S to M. Overall **L**. Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**Phase A: joint internals, lifetime, small setters (S).** `PhysicsJointsInternal.hpp`; `PhysicsJoints.cpp` refactor (entry struct, `registerJoint`, types recorded); `releaseJointsOfBody` plus the one call in `PhysicsWorld.cpp` (F1); `aver_phys_joint_type`, `_handles`, `_set_friction`, `_set_solver`, `_couple` (F2); swing-twist and path motors through the extended v1 calls; header comment fix; the ext header started with these. `JointExtTest` cases 1, 5 (friction), 9, 11. Exit: `JointTest` and `JointBreakTest` unchanged and green; the new cases green.

**Phase B: frames, swing type, live limits, springs, readback (M).** `aver_phys_joint_create_ex` (all eleven types, both spaces, Pyramid), `_set_limit_block`, `_set_motor_targets`, `_set_motor_limits`, `_set_motor_spring_ex`, `_set_limit_spring_ex`, `_set_target_rotation`, `_set_target_angular_velocity`, `_state`, `_frames`. C# `Native.JointsExt.cs`, `JointExt.cs`, `Joint` partial. `AbiEnumTest` rows. `JointExtTest` cases 2, 3, 4, 6, 7, 8, 12, 13, 14. Exit: green including the sign-pinning tests.

**Phase C: path joints (M).** `PhysicsJointPath.cpp`: authored tangents and normals, resampler and arc table, rotation type, friction, `_path_move`, `_path_sample`, state fields, v1 path joints gaining the table. `JointExtTest` case 10. Exit: green; the resampled fraction-equals-metres check within 3 percent.

**Phase D: queries on bodies and the world (M).** `PhysicsQueryHits.*` (record, collectors, face, vertex, entity), `PhysicsQueriesExt.cpp` (`raycast_hits`, `body_raycast`, `shape_collide`, `closest_points`, `closest_point`, `shape_cast`, `depenetrate`, `hit_face`; all with `SOURCE_BODY`), `PhysicsBroadphaseQueries.cpp`. C# `Native.QueriesExt.cs`, `QueriesExt.cs`. `QueryExtTest` cases 1 to 12. Exit: `QueryTest` and `SphereCastQTest` unchanged and green; the new cases green.

**Phase E: shape-handle source (S; needs SHAPES_MATERIALS).** `PhysicsQueryShapes.cpp` binds `shapeByHandle` / `materialIdOf` / `meshTriangleIndexOf`; `shape_raycast`; `HI_MATERIAL` and mesh `HI_FACE` fill; C# `Shape` overloads. `QueryExtTest` case 13. Exit: green; if SHAPES_MATERIALS has not landed, the phase is skipped and nothing else is blocked.

**Phase F: framework wrappers, node families (M).** `JointGraph`, `PhysicsQueryGraph`, the two tables and palettes, `JointNodeTests`, `PhysicsQueryNodeTests`, `docs/nodes` regenerated. Exit: both node test files, `NodeCatalogCheck` and `CsharpApiIndexCheck` green.

**Phase G: authored joints, overlay, lab, docs (S to M, optional as a whole).** `kJointTypeSwingTwist`, `CJointTuning`, `PhysicsSceneSync` reads it, Details section, Add Component row, `JointTuningModelTest`; the overlay only per question 12; `test-content/JointLab`; the doc edits of 3.3. Exit: `JointTuningModelTest` green, `module-guard-audit` and `docs-check` clean, the Debug sandbox object section count recorded.

---

## 5. Tests

Everything below runs without a GPU or a window. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

**`tests/physics` (link `Aver.Physics Aver.Core`; style of `QueryTest`: static scenes that never step unless stated, off-axis, unequal extents)**

- `QueryExtTest`:
  1. *Record layout*: a ray at a compound of three boxes reports `DISTANCE`, `POINT`, `NORMAL`, `BODY`, a stamped `ENTITY`, and a `SUBSHAPE` that differs between boxes; `FACE` -1; `maxHits` larger than the hit count returns the true count; both arrays null counts.
  2. *Ray settings*: a closed cube mesh from inside: default gives 0 hits, `BACKFACE_TRIANGLES` 1 gives a hit with `FLAG_BACKFACE`; a box from inside: default gives a hit at 0 with `STARTED_INSIDE`, `TREAT_CONVEX_AS_SOLID` 0 plus `BACKFACE_CONVEX` 1 gives the exit hit.
  3. *`body_raycast`*: with a nearer body on the same line only the named body is hit; entry and exit with `maxHits` 2; compound sub-shape ids identify the box.
  4. *`shape_collide` (body source)*: a box overlapping a wall by 10 cm reports `DEPTH` within 1 cm of 10, a `NORMAL` pointing from the wall toward the box, `PUSH = NORMAL * DEPTH`; two overlaps sorted deepest first; `ONE_PER_BODY` collapses a compound; `MAX_SEPARATION_CM` 40 reports a body 25 cm away with `DEPTH` about -25.
  5. *Closest*: a sphere body 30 cm from a wall: `closest_points` distance 30 and the wall point; beyond the max gives 0; `closest_point` is negative inside a box (about -10 at 10 cm deep), positive outside, and unsigned for a mesh.
  6. *Depenetrate*: a box sunk 12 cm into a floor and 5 cm into a wall resolves in at most 8 iterations to under 0.1 cm residual, returns 1, moved distance within 20 percent of the expected; a box in a slot narrower than itself returns 2.
  7. *`shape_cast` all hits*: three walls in a row come back in order with `maxHits` 3 and distances within 1.5 cm; `maxHits` 1 matches `aver_phys_body_cast` exactly; `DEEPEST_POINT` on an overlapped start reports `DEPTH` > 0; `EXTRA_RADIUS_CM` 5 shortens the distance by 5; `ignoreBody` and the sensor flag behave as `QueryTest`'s.
  8. *Broad phase*: a thin diagonal capsule overlaps an AABB it does not touch (so `broadphase_aabb` returns it and `overlap_box` does not), results are ascending by handle; the ray version returns entry distances sorted; `world_bounds` contains every body.
  9. *Soft body*: a pinned cloth quad hit by a ray reports `FLAG_SOFTBODY`, a `FACE` of 0 or 1, and a `VERTEX` that is one of that face's three particles and the nearest to the hit; a hit on the other side with `BACKFACE_TRIANGLES` 1.
  10. *Characters*: a ray, a cast and a collide against a live character report its character handle and entity, and `FLAG_CHARACTER`.
  11. *Determinism*: the same query twice is bit equal; two equidistant bodies are ordered by handle.
  12. *Refusals*: `maxHits` 0, NaN pose, zero-length displacement, a mesh as the cast shape (-5), a dead body source (-1), a longer settings block than defined, a zero scale component; each returns 0 and sets `aver_phys_last_error`.
  13. *(Phase E)* a hull shape handle cast equals the same shape built as a body (`SOURCE_BODY`) within 0.5 cm; `shape_raycast` against a mesh-shape handle at a pose; `HI_MATERIAL` matches the material table; mesh `FACE` is the source triangle index when per-triangle data exists.
- `JointExtTest` (the `JointTest` helpers, gravity off unless a case needs it):
  1. *Joints die with their body (F1)*: remove a body with a hinge and a slider: `joint_count` 0, `joint_value` 0, 300 steps run clean (the test would have been undefined behaviour before); removing the world-side joint's other body likewise.
  2. *Local frames*: a hinge made `JSPACE_LOCAL` between bodies 50 cm apart pulls the anchors within 1 cm in 1 s; the same frames in `JSPACE_WORLD` at the same spot leave them where they are; a frame B rotated 30 degrees about the hinge axis gives `joint_value` of 30 degrees in magnitude at rest; `bodyB` = world with a world frame B.
  3. *Swing-twist motors*: a velocity motor on the twist axis turns at the target rate within 10 percent; a position target of 40 degrees about frame X settles within 2 degrees and `JS_TWIST_RAD` agrees with the relative rotation measured from `aver_phys_body_rotation` (this pins the sign conversion of `_set_target_rotation`); the swing motor holds a hanging mass against gravity; a low `_set_motor_limits` stalls it.
  4. *Cone and pyramid*: `_set_limit_block` shrinking a cone limit pushes a body inside the new cone within 1 s; with 40 x 40 degree swing limits a diagonal swing reaches a larger angle with `SWING_PYRAMID` than `SWING_CONE`; asymmetric limits with cone are refused.
  5. *Friction*: a hinge door spun at 2 rad/s stops in a time within 15 percent of `I w / F`; friction 0 coasts; it is ignored under a velocity motor; slider and per-axis six-DOF friction; path friction on a rail.
  6. *Asymmetric motor limits*: a slider velocity motor with `minLoad` 0 pushes a mass up but does not pull it down against gravity.
  7. *Springs*: a hinge position motor in `STIFFNESS` mode oscillates at `sqrt(k/I)/(2 pi)` within 15 percent; the damper recipe (stiffness 0, damping c, `POSITION_VELOCITY`) decays angular speed with time constant `I/c` within 20 percent; frequency mode still behaves as v1.
  8. *Six-DOF rotation spring*: a position motor in stiffness mode on the rotation axes returns a displaced body to the rest orientation; `_set_limit_spring_ex` on a rotation axis is refused (documents Jolt's limit).
  9. *Gear and rack coupling (F2)*: two hinged gears at ratio 2 spun hard for 5 s keep `|theta1 + r theta2|` under 0.5 degrees with `_couple` and drift at least three times as far without it (measured side by side in one test); rack and pinion likewise; removing the hinge clears the coupling (`COUPLED_A` 0); wrong types refused.
  10. *Path*: an authored-tangent path passes within 2 cm of its given points (checked at `_path_sample`); `ROT_TO_PATH` yaws a car along a curve within 5 degrees; velocity 200 cm/s moves 800 cm in 4 s within 3 percent; position 500 stops within 5 cm; a closed path keeps going; with the default resample `PATH_FRACTION * 100` equals the travelled distance within 3 percent and the arc table length equals the polyline length within 2 percent; v1 `aver_phys_joint_path` joints answer `_state` and `_path_move`; `RESAMPLE_CM` 0 keeps the control points exactly.
  11. *Priority and steps*: two joints fighting over one body, the higher priority violated less; velocity-step override 1 versus 20 on a 12-link chain gives monotone sag; values read back from `JSI`.
  12. *State*: hinge and slider value, rate and at-limit; distance and pulley lengths; a pendulum hanging from a point joint reports `LOAD_LINEAR_N` within 15 percent of `m g`; swing-twist `TWIST_RAD` and swing angles.
  13. *Handles and frames*: `_handles` ascending and complete; `_frames` of a world-space joint equal the creation frames within 0.5 cm and 1 degree; `_type` for every creator.
  14. *Refusals*: parallel frame axes, unknown type, `n` shorter than the minimum for the type, longer block, negative distance in local space, NaN, both bodies world, a dead joint; each sets `aver_phys_last_error`.
  15. *Replay*: the same script twice for 300 steps is bit identical, including path and gear cases.

**`tests/abi`**: `AbiEnumTest` pairs both new headers with their binding files (names and parameter names) and every index block with its C# mirror and `Count`.

**C# (`Aver.Graph.Tests`, Windows, ctest `GraphManagedTests`; bare process, no native library)**: `JointNodeTests` (the 11 rows through `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`; the legacy joint rows unchanged; exec rows refused by the pure compiler; the pure rows work as reads; methods return false rather than throw with no library; the `points` attribute parser accepts and rejects the right strings), `PhysicsQueryNodeTests` (the 7 rows, `layerMask` 0 maps to all, `hitIndex` out of range gives `success` false). Python: `NodeCatalogCheck` and `CsharpApiIndexCheck`.

**`tests/editor` (Phase G)**: `JointTuningModelTest` (canAttach needs `CJoint`, zero-filled means unset, SI value formatting, record keys and canonical omission).

---

## 6. Headed manual checks for the owner

Editor first, then `--backend vulkan` for the overlay (the only visible output), GPU idle, screenshots at 1440p with a ping for remote runs. Use `test-content/JointLab` (Phase G) or a graph-built scene.

1. **Door friction.** A hinged door, friction 0.5 N*m: shove it, it swings and stops where it came to rest; friction 0 swings freely; the Details live angle matches the visible angle.
2. **Closer and damper.** Stiffness-mode spring toward the closed angle with a damper: the door shuts without ringing; push it open and it returns; a heavy damper makes it slow.
3. **Locked door.** The limit-block node narrows the range to +-0.01 rad: the door will not move; widen it and it swings.
4. **Rail platform.** A path with 5 points and `ROT_TO_PATH`: the platform follows the curve at a steady 2 m/s through the bends (velocity motor), banks into them, and stops at a position target; a character standing on it rides along (a kinematic parent comes from `aver_phys_body_move_kinematic`; a dynamic platform on a path is the case tested here).
5. **Turret.** A swing-twist joint with a position motor and a target rotation from a graph: the barrel follows, stalls against an obstacle when its motor load limit is low, recovers when freed; pyramid versus cone limits visibly differ at the corner.
6. **Gear train.** Two gears coupled: spin one with a motor against load; they stay in ratio (no creep) and a gear without `couple` visibly drifts over a minute.
7. **Prop attach.** A local-space fixed joint between a crate and a moving body at a prefab offset: the crate snaps to the offset smoothly.
8. **Queries.** A debug graph casts a box through a doorway with `an_shapecast` and prints the hit count and distances; `an_depenetrate` rescues a spawn point that was placed inside a wall; `an_closestpoint` reports the distance to the nearest wall while the player walks (signed inside a closed room's solid parts); `an_bodyraycast` picks one crate through a stack; `an_raycasthits` on cloth reports the vertex nearest the cursor and a poke at it moves that corner.
9. **Joint removal.** Delete a jointed crate in Play: no crash, the door it was attached to hangs free.
10. **Overlay (if built).** `Show > Joints`: axes, hinge arcs, slider ranges, cone ellipses and the path polyline match the physics; limits redraw when the limit node changes them.
11. **Cost.** With a quiet GPU, 500 joints in a chain scene: record milliseconds per step with the default solver and with a velocity-step override of 8 on one island (none exist today); 1000 `an_shapecast` calls in a frame: record the frame cost.
12. **Hosts.** Repeat 1, 4 and 8 in the packaged runtime for the node and C# paths (authored `CJoint` records do not build there; section 1.6).
13. Full `ctest`, the graph suite via `dotnet run`, and open `docs/nodes/Joint.md` and `PhysicsQuery.md` to confirm 18 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Units of the new load parameters.** SI (N, N*m, N/m) with the unit in the name, against the older `_set_motor_strength` in kg*cm. Default: SI, because it matches the break thresholds and Jolt's docs. Consequence: the doc must warn once, at the top of each header block.
2. **Frame Z mirror.** The new local-space six-DOF inherits v1's behaviour that Jolt's frame Z (`cross(X, Y)`) is the negative of the engine's. Default: do not compensate (a silent sign change to existing joints would be worse); the test pins the sign; compensating (negate and swap translation Z limits, flip rotation X and Y signs) is a follow-up if the owner wants true engine frames.
3. **Gear and rack auto-linking.** Default: explicit `_couple`; the C# `Joint.Gear(a, b, ...)` overload that takes the two hinge `Joint`s calls it. Auto-linking by searching the joint table for a hinge touching each body is ambiguous when a body has several hinges.
4. **Path resampling default and fidelity.** 100 cm segments, tolerance 1 cm from the authored points, in return for a motor that behaves in real units. Default: on; `RESAMPLE_CM` 0 keeps control points. UNVERIFIED: whether resampling needs a finer step for tight curves (the headed rail check in 6.4 decides).
5. **Joint removal on body removal.** Default: remove every joint touching the body (documented in the header and the migration note). The alternative, refusing to remove a jointed body, would break level teardown.
6. **Result caps and hit order.** `maxHits` caps at 4096; ties break on body handle then sub-shape id. Default: that; the graph rows re-run the query per `hitIndex` rather than caching a buffer.
7. **Depenetration algorithm and tolerance.** Gauss-Seidel on up to four contact normals, 0.05 cm tolerance, 0.01 cm skin (section 2.2). Default: ship, tune in 6.8.
8. **Large-world precision.** The new headers keep float positions. Default: all positions pass through `outPosition` and the `RVec3` types, so the owner's `JPH_DOUBLE_PRECISION` decision is a one-function change here; if that area adds `_d` variants of the position-taking calls, these headers follow in a v2 header, not in place.
9. **Soft-body vertex.** Reported as the nearest of the hit face's three particles plus the face index; barycentric weights are not returned. Default: that; add three floats to the hit record in a v2 if a cloth-tearing tool needs them.
10. **Mesh triangle index.** Needs SHAPES_MATERIALS to build meshes with per-triangle user data (the source triangle index, or a material index). Default: `FACE` is -1 for meshes until they do; nothing here depends on it.
11. **Authored joints in the packaged runtime.** Not built today (section 1.6). Default: out of scope; Phase G's `CJointTuning` is editor Play only and says so in its Details section. Moving `PhysicsSceneSync`'s joint pass into the runtime is a separate small feature the integrator may schedule.
12. **Joint overlay versus a debug-renderer area.** Default: build the line overlay in Phase G only if no wave 11 area has shipped constraint drawing by then; the readbacks (`_frames`, `_handles`, `_path_sample`) ship regardless.
13. **Soft rotation limits on six-DOF.** Jolt has none (finding F5). Default: documented recipe (rotation position motor with a stiffness-mode spring) and no fake limit spring. A true soft rotation limit would be our own solver term and is not attempted.
14. **`aver_phys_joint_create_ex` for path.** Path is built by `_path_create` because its inputs are arrays and it has no Jolt space. Default: keep separate; `create_ex` refuses `JT_PATH`.
15. **Patent and licence notes.** Everything here is Jolt (MIT) used through its public classes; the depenetration iteration is the textbook projection scheme written from first principles, the Hermite resampler is the cubic Hermite basis, and the door and damper recipes are spring-damper arithmetic. No vendor control law, no third-party code, no new library.
