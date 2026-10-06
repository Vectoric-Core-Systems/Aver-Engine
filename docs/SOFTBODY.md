# Aver Engine — Plastic soft bodies: dents that stay, and tearing

`Aver.SoftBody` (`modules/softbody`) is the half of soft-body physics Jolt cannot do. Jolt's soft body
(`aver_phys_softbody_*`, see `physics_abi.h`) is **elastic**: push it and it springs back, collide it
with the world and it collides. This module is **plastic**: a bar bent past its yield **stays bent**, a
cloth strip pulled past its limit **tears in two**, and a glass pane **cracks** instead of denting.

| | Jolt soft body | `Aver.SoftBody` |
|---|---|---|
| Cloth, skinned characters, elastic blobs | yes | no |
| World collision | yes | no (the host feeds it impacts) |
| Permanent set past a yield force | no | yes |
| Break / tear, with the render mesh following | no | yes |
| Depends on | Jolt | `Aver.Core` only |

They never share state. A host that wants both (a car whose panels dent and whose flags flutter) runs
both and gives each its own handle. Nothing here includes or links `Aver.Physics`.

The solver is the OpenConstructor cage solver, ported UE-free; `docs/recon/softbody-solver.md` is the
spec and the "As built" section at its end lists every place this differs from it.

---

## 1. The model

A **cage** is particles joined by distance constraints (**beams**), optionally skinned by
**triangles**:

- `Particle` — position, Verlet previous position, rest position. `pinned` means *kinematic*: the
  solver never moves it, the host may (a cage riding a moving chassis, a hand holding cloth).
- `Beam` — two particles, a rest length that **creeps** when the beam yields, and the material it is
  made of. Beams are what yield and break.
- `Triangle` — three particles and the three beams on its edges. Only needed for tearing a *surface*
  (it is how a tear knows which side of a cut a particle's triangles are on) and for the render
  mapping. A bar or truss is just beams.
- `Material` — the yield curve:

```
force = axialStiffness * stretch                      N, stretch in cm

          force
  breakForceN -|-------------------------- X  instant snap
               |                      ...'
  bendForceN --|-----------.-----'''           plastic: rest length creeps toward the
               |        .'                      stretched length at plasticStiffness
               |     .'   elastic: springs back
               +---------------------------- stretch
```

Below `bendForceN` a beam is elastic and the cage returns to rest. Above it the rest length creeps by
`(force − yield) / plasticStiffness` per second (rate-limited by `damageRate`): the shape the beam
settles at is its new rest shape, so the dent stays. `hardening` raises the yield as the set grows
(0 = perfectly plastic). A beam tears when any of these happens:

- the force passes `breakForceN`;
- the accumulated set passes `maxBend` cm;
- its stretch passes `breakStrain` of the **original** length (a strain limit, not creeping away
  because the rest length moved); 0 turns it off;
- it is `Fracture` or `Shatter` (brittle) and the force passes `bendForceN` — carbon and glass snap at
  yield instead of bending.

A snap recoils the two ends (`breakAbsorb` of the energy is soaked, the rest kicks the neighbours),
which is what makes a crack run.

## 2. The step

`step(cage, cfg)` is deterministic. Per substep (default 4 per step at 64 Hz, 8 relaxation sweeps):

1. **Verlet.** `pos += (pos − prev) × damping`, clamped to `maxNodeSpeed`. Pinned particles take no
   velocity. Gravity applies to freed debris only, unless `gravityAll` (cloth, flags).
2. **Damage / plasticity.** Reads the stretch the integration (or an impact) just caused, **before**
   relaxation hides it: yield → creep, snap → break.
3. **Gauss-Seidel PBD.** Distance projection, stiffness per material.
4. **Tear resolution** if anything broke (§3).

The float evaluation order is part of the contract: a queued impact on the worker thread and the same
impact replayed by hand must end on the same bits (`PlasticSoftBodyAsyncTest` checks exactly that). Do not
reorder the arithmetic without re-running both tests.

`StepResult.settled` is true when no particle moved more than `settleThresholdCm` in any substep; a
host stops stepping then and idles until the next impact.

### Impacts

`applyImpact(cage, Impact, CrushParams)`:

- The raw depth goes through the **soft-knee crush curve**: identity up to `knee` (50 cm), then
  C1-continuous and asymptotic to `knee × ceilingMult` (90 cm). A 200 km/h crash is deep but bounded.
- Particles within `radiusCm` are pushed along `direction` with linear falloff — by seeding Verlet
  velocity (`prev −= dir × disp × falloff`), so a deep hit is spread over frames by the speed clamp
  and never teleports.
- Brittle particles (majority of their beams brittle) **do not dent**; instead brittle beams whose
  midpoint is within the **tear radius** snap outright.
- `depthFromImpulse` / `depthFromSpeedKmh` convert a rigid-body callback into a raw depth.

## 3. Tearing, and what happens to the mesh

A broken beam is just a constraint that stopped constraining. The interesting part is a **surface**: a
tear that opens a slit has to *duplicate* the particles along it, or the two lips stay welded to one
point.

`resolveTears` (run by `step`, or by hand after `breakBeam`) works per particle that touches a broken
edge:

1. Take the particle's incident triangles. Two triangles are joined around it if they share an
   **unbroken** edge that contains it.
2. If those triangles fall into more than one group, the particle is torn open: the first group keeps
   the particle, every other group gets a **duplicate** (same position and velocity, `origin` records
   what it was torn from), and that group's triangles and unbroken beams are re-pointed at it.
3. Each duplicate is logged as a `SplitEvent{old, new, triangles}`.

A single broken edge in the middle of a sheet does **not** split anything — the fan around each
endpoint is still connected the other way round. A *line* of broken edges does, which is exactly a
tear. Triangles with **two or more** broken edges are marked dead (`triDead`): their corners are no
longer held together and the triangle would be drawn stretched across the gap, so the renderer skips
it. A particle with no unbroken beam left is `freed` and falls as debris. Pieces are counted by
`countPieces` (connected components over unbroken beams).

Beams that belong to no triangle ("free" beams: a truss, a rope) are never re-pointed. A tear through a
truss is breaking its beams; there is no surface to open.

### The render mesh

`RenderBinding` maps render vertices to particles and render triangles to sim triangles
(`RenderBinding::identity(cage)` is the one-to-one case; custom meshes fill the three arrays and call
`finalize`). The host replays tears onto it:

```cpp
std::vector<u32> sources;
binding.applySplits(cage.splitLog, &sources);   // or snapshot.splits from the async worker
// for each k: new render vertex (oldCount + k) copies its UV/normal/etc. from vertex sources[k]
binding.visibleIndices(cage.triDead, indexBuffer);   // dead triangles omitted
binding.gatherPositions(particlePositions, vertexPositions);
```

`repair(cage)` undoes dents and tears; `binding.reset()` undoes the mapping.

## 4. The async worker

`AsyncSolver` runs the same `step()` on its own thread (below-normal priority on Windows) at a fixed
`hz` (15–240, default 64), over a **private copy** of the cage. The game thread never touches that
copy:

- Commands (`enqueueImpact`, `enqueueBreak`, `enqueueMove` for a kinematic particle, `enqueueRepair`)
  go in under one mutex and run at the top of the next step, in order. Commands queued before
  `start()` run first.
- The worker publishes a `Snapshot` (all particle positions, dead triangles, the split events since
  you last took one, step count, settled flag). `tryGetSnapshot` is an O(1) swap and never blocks.
- A time accumulator with a four-step spiral-of-death clamp; when settled it sleeps until a command
  arrives. `AsyncOptions::realTime = false` steps as fast as possible with identical results — tests
  and bakes use it.
- `waitSettled(ms)` blocks until every queued command has run and the worker is idle.

## 5. The C ABI

`softbody_abi.h` (`aver_sb_*`) is a handle API over the same solver: build a cage (`add_material`,
`add_particle`, `add_beam`, `add_triangle`, `build`), drive it (`step`, `impact`, `move_particle`,
`break_beam`, `repair`) or hand it to the worker (`async_start` / `async_poll`), and read positions
back. It is **statically linked** and not exported from a DLL; see Wiring for exposing it to C#.

## 6. The test panel

`sandbox/src/SoftBodyPanel.{hpp,cpp}` — Window > Soft Body (plastic): a side-on truss bar with the
material sliders, Hit tip / Hit middle / Repair, and a live readout of permanent set, broken beams and
pieces. It is for tuning a material, not an asset editor.

## 7. What is deliberately not here

- **No world collision, no self-collision.** The solver takes impacts from the host's collision layer.
- **No `.ocbeam` parser, parts or whole-panel detach** (spec §2, §8). The data model carries what they
  need (materials per beam, `origin`, `freed`); a loader and part registry go above this.
- **No node-mass weighting presets, strain-rate dashpot or debris chunks** beyond the `useNodeMass`
  hook (spec §11); the base solve is massless, as in the original.
- **No automatic Jolt hand-off.** A host decides which bodies are elastic (Jolt) and which are plastic.

## Wiring (what the integration step adds)

1. Root `CMakeLists.txt`, with the other leaf modules (needs Core only):
   ```cmake
   add_subdirectory(modules/softbody)
   ```
   and in the tests section:
   ```cmake
   if(TARGET Aver.SoftBody)
     add_subdirectory(tests/softbody)
   endif()
   ```
   There is no `AVER_MODULE_SOFTBODY` option yet; add one beside `AVER_MODULE_DEFORM` (and the edition
   defaults macro) if it should be switchable. The target carries `AVER_MODULE_SOFTBODY=1` publicly.
2. Editor: add `src/SoftBodyPanel.cpp` to `sandbox/CMakeLists.txt`, `target_link_libraries(Sandbox
   PRIVATE Aver.SoftBody)`, then the three lines in `SoftBodyPanel.hpp`'s header comment.
3. C# / scripts (optional): the ABI is plain C. To expose it through the existing DLL, link
   `Aver.SoftBody` into `Aver.Physics` and re-export with `AVER_PHYS_API` wrappers — this was left
   undone so `Aver.Physics` does not gain a dependency edge nobody has asked for yet.
4. `docs/ARCHITECTURE.md`: the `Aver.SoftBody` row still says "not built"; update it to name the
   as-built contents (§1–5 above) and the `Core`-only dependency.

## Tests

`tests/softbody` — `PlasticSoftBodyTest` (solver, tears, render mapping, ABI) and `PlasticSoftBodyAsyncTest` (worker
against a synchronous replay). Links `Aver.SoftBody` + `Aver.Core` only.

| Property | Check |
|---|---|
| A bar bent past yield keeps the bend | steel truss held 16 cm down keeps a permanent set of several cm; the elastic twin and the same steel at 4 cm return to rest |
| Impacts dent | same cage, same hit: elastic recovers, steel keeps more than 5 cm |
| A strip pulled past its limit tears in two | exactly two pieces, exactly the seam beams broken, none outside it |
| A slit duplicates particles | five particles on a cut row duplicated, two pieces, render triangles each wholly on one side |
| Energy does not explode | a swinging flag at 64 Hz over 8 s: finite, speed-limited, kinetic proxy bounded and decayed |
| Determinism | two runs, and a worker run vs a replay, are bitwise identical |
