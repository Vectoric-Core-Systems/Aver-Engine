# Soft bodies in Aver.Physics: the builder ABI for cloth and destruction

`modules/physics/include/aver/physics/physics_softbody_abi.h` (`aver_phys_sb_*`) is the physics half of
cloth and destruction. The scene half is `Aver.SoftBody.Scene`; the design is `docs/SOFTBODY.md`.
The older `aver_phys_softbody_*` surface (`physics_abi.h`) is untouched; Fluids still uses it.

Nothing here was compiled or run when it was written. Jolt API details marked UNVERIFIED below are the
first things to check in a build.

## Why a second surface

`aver_phys_softbody_create` builds a fixed triangle shell and can only skin it. Cloth and destructibles
need three things it cannot do:

1. **Edit rest state in place.** Jolt reads an edge's `mRestLength` and a dihedral bend's `mInitialAngle`
   live from the shared settings, so a dent is a write, not a rebuild
   (`aver_phys_sb_set_edge_rest`, `aver_phys_sb_rest_bends_from_pose`).
2. **Rebuild under the same handle.** A tear changes the vertex and face lists.
   `aver_phys_sb_builder_finish` with a `replaceBody` handle creates the new body, copies positions and
   velocities through a state map (new vertex -> old vertex, or -1 for rest), and removes the old one,
   so scripts and the scene keep their handle.
3. **Report impacts and apply aero.** `aver_phys_sb_impact_*` is a polled list of
   `(soft body, other body, point, normal, closing cm/s, other mass kg, contact vertices)` filled from a `SoftBodyContactListener`;
   `aver_phys_sb_set_aero` / `aver_phys_sb_set_wind` apply drag and lift per face.

## Surface

| Group | Calls |
|---|---|
| Builder | `builder_begin`, `_vertices`, `_faces`, `_edges`, `_bends`, `_skin`, `_params`, `_finish`, `_cancel` |
| Live edits | `set_edge_rest`, `rest_bends_from_pose`, `set_params`, `set_active`, `is_active`, `impulse` |
| Skin | `skin_world` (palette of row-major 4x4 matrices composed as `T(com) * skin * entityWorld * T(-com)`) |
| Aero | `set_aero`, `set_wind` |
| Readback | `state` (positions and velocities in world cm), `vertex_count`, `count` |
| Impacts | `impact_count`, `impact_get` (one record per soft body and other body per step, below 1 cm/s dropped) |

`AVER_PHYS_SB_P_*` indices (iterations, damping, friction, restitution, gravity scale, vertex radius,
max speed, allow sleep, double sided, layer, entity, impacts). A NaN entry leaves the value alone.

## Conventions

- Units and axes are the engine's at the boundary (cm, +X forward, +Y right, +Z up). Conversion lives in
  `Convert.hpp` (`toJolt`, `fromJolt`, `toJoltDir`); the mirrored basis means winding is flipped once, in the builder.
- Single threaded: call between `aver_phys_step`s, never from a step listener or contact callback.
  Edits take effect on the next step.
- Collision layers: cloth runs on user layer 14 and collides with layer 0 and the proxy layer 15 only;
  the layer pair set-up is done by the scene system (`aver_phys_set_layer_collision`).
- Hard-pinned vertices (`reach 0`) get invMass 0 and a `Skinned` max distance of 0; free vertices get no
  `Skinned` entry.

## Where it lives

`PhysicsSoftBody.cpp` (the ABI and the registry of shared settings per body), `SoftBodyAero.hpp/.cpp`
(per-face drag and lift kept out of the ABI file), and small hooks in `PhysicsWorld.cpp` (impact buffer
cleared at the top of `aver_phys_step`, contact-listener install, registry teardown on shutdown).
C#: `scripting/csharp/Aver.Physics/Native.SoftBody.cs` (P/Invoke) and `SoftBody.cs` (typed wrapper).
Tests: `tests/physics/src/SoftBodyBuilderTest.cpp`.

## UNVERIFIED Jolt details

- `BodyLockWrite` release order around `SoftBodyMotionProperties` edits.
- `SetSoftBodyContactListener` ownership on the physics system.
- `RMat44` for the skin palette on a double-precision Jolt build.
- Whether `SkinVertices` honours a changed `mMaxDistance` of 0 as a hard skin after a rebuild.
