# Aver Fluids — density, viscosity, and a solver that sloshes

`Aver.Water` and `Aver.Render.Fluid` are gone; `Aver.Fluids` (`modules/fluids/*`) is what replaced
them, in one commit (`b887a78`) whose own reasoning is worth repeating rather than paraphrasing: the
split "turned out to describe the implementation rather than the domain — an author placing a `WATER`
record does not care whether the surface they get is evaluated in a vertex shader or stepped by a
solver," and the old name was narrower than what the code does, because it simulates a volume of
arbitrary compliance and pressure, which is a fluid, not specifically water. The module still keeps
its physics dependency scoped rather than inherited: `FluidScene.cpp` — the only file that touches
Jolt's ABI — is compiled in only when `AVER_MODULE_PHYSICS` is on; the analytic surface, the
underwater fog override and `FluidVolume`'s own generate/readback arithmetic build without it, exactly
as they did as separate modules (`modules/fluids/CMakeLists.txt`).

This page is about the part that is new since the merge — real-unit authoring and the simulated
volume — and gives the analytic surface and underwater fog only what they need to sit beside it
honestly. For the wave math and fog override in their own right, `modules/fluids/README.md` still
holds; where this page and that one disagree, this page is the newer one and says why (§7).

---

## 1. What exists

| Piece | Where | Checked by |
| --- | --- | --- |
| The seed-shell geometry (closed, wound, sized) | `modules/fluids/{include,src}/aver/fluids/FluidVolume.{hpp,cpp}` — `generateFluidSeedShell` | `FluidVolumeTest.cpp`: `testClosedAndConsistentlyWound`, `testNoDuplicatedSeamVertices`, `testExtentMatchesRequest`, `testEveryTriangleFacesOutward`, `testSubdivisionScaling` |
| Density → mass, mass → pressure | same file — `fluidParticleMassKg`, `fluidPressureFor` | `testDerivedPressure`, `testShellParticleCountMatchesGenerator`, `testDensityScalesMassAndPressure` |
| Viscosity → damping (the calibrated fit) | same file — `fluidDampingForViscosity` | `testViscosityDampingFit`, `testMaterialPresets`; the calibration itself is `tests/physics/src/FluidDampingCalibrationTest.cpp` |
| The material precedence rule | same file — `fluidResolvePhysicsMaterial`, called only from `FluidScene::spawn` | `testResolveMaterialPrecedence` |
| The solver join (GPU mesh, staging ring, Jolt body) | `modules/fluids/{include,src}/aver/fluids/FluidScene.{hpp,cpp}` | no dedicated test — see §7 |
| Winding/pressure correctness inside Jolt itself | `modules/physics/src/PhysicsWorld.cpp` — `buildSoftShared`, `addSoftBody` | `tests/physics/src/SoftBodyTest.cpp::testPressureHoldsAShellUp` |
| `COMP <id> Fluid` (graph authoring) | `scripting/csharp/Aver.Graph/GraphComponentTree.cs` — `ApplyFluid` | — |
| `WATER ... simulate` (level authoring) | `sandbox/src/SandboxApp.cpp` — `applyLevelWater`; format in `modules/formats/{include,src}/aver/formats/OcWorld.*` | the FirstPerson template's own pool (`templates/FirstPerson/Content/Maps/Default.ocmap`) |
| The graph editor's preview | `sandbox/src/GraphEditor.cpp` — `buildFluidPreviewMesh` | — |
| The analytic Gerstner surface | `modules/fluids/{include,src}/aver/fluids/{GerstnerWave,WaterRenderer}.*` | `tests/fluids/src/GerstnerWaveTest.cpp` |
| The underwater fog override | `modules/fluids/include/aver/fluids/Underwater.hpp` | `tests/fluids/src/UnderwaterFogTest.cpp` |

Two authoring paths (§4) converge on one spawn function, which is the one place a design rule about
conflicting inputs is enforced (§3). Two rendered surfaces (analytic and simulated) exist side by side
and never both apply to the same body of water (§6).

---

## 2. The unit story — and it is not symmetric

An author no longer has to type Jolt's own solver knobs to describe a fluid. `FluidPhysicsMaterial`
(`FluidVolume.hpp`) adds a second, optional layer on top of the four raw ones
(`compliance`/`damping`/`iterations`/`pressure`): **density, in kg/m³, and dynamic viscosity, in
Pa·s.** The two are not the same kind of number, and the header is explicit about it rather than
letting a reader assume symmetry:

- **Density is real, not a fit.** It becomes an actual per-particle mass —
  `fluidParticleMassKg = densityKgM3 * enclosedVolumeM3 / particleCount` — which the solver honours
  directly through each particle's inverse mass. A denser fluid genuinely has more inertia and sags
  harder under the same pressure and compliance; `testDensityScalesMassAndPressure` checks exactly
  that relationship.
- **Viscosity is a calibrated fit, not real.** Jolt's soft-body solver has no shear-stress term at
  all — the closest proxy it has is per-vertex linear damping (`SoftBodyCreationSettings::mLinearDamping`,
  `dv/dt = -damping * v`), which removes energy uniformly rather than in proportion to shear.
  `fluidDampingForViscosity` maps a viscosity onto a damping value through a **measured** table, not a
  derived formula:

  `tests/physics/src/FluidDampingCalibrationTest.cpp` spawns the FirstPerson pool shell
  (300×200×60 cm, 8×8×4, 258 particles), settles it, hits every particle with the same lateral impulse,
  and fits the decay to `v(t) = v0 * exp(-t/tau)` by least squares. Swept across seven damping values,
  tau falls from 2.84 s at `damping=0.01` to a **minimum of 0.63 s at `damping=3.0`**, then rises and
  gets noisier at `damping=10` — a genuine second slosh mode shows up in that trace (speed falls, then
  visibly *rises* again before finally dying), which is exactly why that row has the sweep's worst fit
  (r²=0.618). The mapping is therefore built only on the reliable range, `[0.01, 3.0]`, as a **log-log
  interpolation** between two measured anchors — water (`1.0e-3` Pa·s → damping `0.01`) and honey's own
  preset value (`10` Pa·s → damping `3.0`) — and **clamps** rather than extrapolates past either one.
  Lava's preset viscosity (`1000` Pa·s, chosen from a cited 10²–10⁴ range) therefore maps to the
  *same* damping as honey — a real, acknowledged gap the header records rather than hides behind an
  invented formula reaching past where anything was ever measured.

Four presets exist as plain factory functions, not a second enum-keyed path — `FluidPhysicsMaterial::Water()`
(998 kg/m³, 1.0e-3 Pa·s), `LightOil()` (~900, 0.1 — the geometric midpoint of the water/honey anchors),
`Honey()` (1420, 10 — chosen to equal the calibration's own high anchor exactly), `Lava()` (2900, 1000
— real density, but a damping response presently indistinguishable from honey's, for the reason
above). `fluidPhysicsMaterialPreset("water"|"lightoil"/"light oil"/"oil"|"honey"|"lava")` resolves a
name to one, case-insensitively.

**Surface tension, pour, split, merge and puddle are refused, deliberately.** `aver_phys_softbody_create`'s
own `indices` never change after a body is built, so a shell's vertex count and the edges between them
are frozen for its whole lifetime — no parameter can make a fixed-topology shell tear, join or reflow,
so none is offered.

---

## 3. Precedence: a material and a raw knob never silently pick a winner

`FluidVolumeDesc` still carries the four raw solver knobs it always did
(`compliance`, `damping`, `iterations`, `pressure`) alongside the new, optional `material` field. Both
can be set on the same request — a `COMP ... Fluid` line, or a `WATER` record, can in principle name
`preset=honey` *and* a hand-typed `damping=`. Rather than decide which one an author meant,
`fluidResolvePhysicsMaterial` — called from exactly one place, `FluidScene::spawn`, regardless of which
authoring path produced the request — **refuses the whole spawn** when `desc.material` is set and
`desc.damping` has already been changed from `kDefaultFluidDamping` (0.1), and writes a message naming
both the material's own implied damping and the conflicting raw value. `testResolveMaterialPrecedence`
pins this. A request naming only one of the two (or neither) applies normally; a desc that never sets
`material` is untouched by this function at all, so every caller that predates the material layer
spawns identically to before.

---

## 4. Two authoring paths, one spawn function

**A graph component.** `COMP <id> Fluid [pos=...] [scale=...] [preset=|density=|viscosity=] [compliance=|damping=|iterations=|pressure=]`
(`GraphComponentTree.ApplyFluid`) is unlike every other component kind in the same switch: `fluids::FluidScene`
owns no entity and reads no `scene::World` at all, so the placeholder entity `Build()` creates for the
`COMP` record still exists (an inspector can find it by name) but nothing the solver reads ever comes
from it. `pos=` is read as an **absolute world** centre — not composed through a parent transform,
because that transform does not exist yet at the point `ApplyKind` runs. `scale=` multiplies
`FluidVolumeDesc`'s own default half-extent (100, 100, 50 cm), so `scale=1,1,1` — what an omitted
`scale=` already defaults to — spawns the same 2×2×1 m pool an unauthored `WATER` record's default
would. The call goes through `Game.SpawnFluidVolumeMaterial` (`Aver.Framework/Game.cs`), the same
managed relay a plain C# script can call directly — the graph line is one authoring path into it, not
a second mechanism. A build with no simulated-fluids module linked (physics off) logs a warning by
name and leaves the component a plain, inert transform rather than failing the whole actor.

**A level record.** `WATER name <n> level <cm> [infinite | bounds x0 y0 x1 y1] [simulate] [compliance <f>] [damping <f>] [iterations <i>] [pressure <f>] [preset <name>] [density <f>] [viscosity <f>]`
parses in `OcWorld.cpp` and is consumed by `SandboxApp::applyLevelWater`. `simulate` is what selects
this document's half over the analytic one (§6) — and is **only meaningful with bounds**: an infinite
ocean has no size to build a shell from, and a record that asks to be simulated while also declaring
`infinite` is left analytic with a named warning rather than silently ignored. When simulated, the
consumer derives a shell that hangs *below* `level` (the water's top), with depth clamped to the
shallower of 60 cm or the footprint itself, and — for this one call site specifically — overrides
`FluidVolumeDesc`'s own default 8×8×4 subdivision with **14×14×4**, a finer horizontal grid the
comment ties to a specific measured GPU/CPU cost check against this exact pool rather than to the
`SoftBodyTest` sweep, which was run at 8×8×4 and is called out as *not* automatically covering a
different subdivision. The FirstPerson template's own pool is the concrete, checked-in instance of
this:

```
WATER name Pool level -20 bounds 200 450 800 850 simulate
```

— a 600×400 cm (6×4 m) footprint (`boundsMax - boundsMin` on each axis; half-extents 300×200 cm, which
is what `applyLevelWater` actually hands `FluidVolumeDesc`), 20 cm below the surrounding deck. Only
the *first* declared `WATER` record is ever drawn — `WaterRenderer`
holds one level and one wave set, so a level naming a second gets a named warning
(`"the level declares {} WATER records; only '{}' is rendered"`) rather than a silent drop; the format
itself allows several (an ocean and a pool), the consumer does not yet.

---

## 5. The seed shell, and what the editor preview actually shows

`generateFluidSeedShell` builds a closed, subdivided-box triangle mesh by pure arithmetic — no device,
no solver, no randomness — partitioning a box's boundary lattice into three non-overlapping vertex
blocks by integer index alone, so two faces sharing an edge share the same output vertex rather than
being merged by comparing floating-point positions. `FluidVolume::generateSeedShell()` places that
local shell at the desc's own centre and seeds normals from it, so a renderer that draws before the
physics solver has run once still gets a correctly-shaped body rather than an empty one;
`updateFromSimulation` later overwrites the same buffer with the solver's own world-space vertices and
recomputes normals by area-weighted face averaging.

**The graph editor's Fluid preview shows exactly this seed shell — it does not simulate.**
`GraphEditor::buildFluidPreviewMesh` calls `generateFluidSeedShell` directly and caches the result by
size; its own comment states the reason plainly: the physics solver only advances inside
`aver_phys_step`, whose one non-test call site is gated on a Play session or `--spawn-test`, and in
edit mode it never runs. A preview that asked the solver for a shape would show a frozen one and imply
motion the fluid is not making; stepping the shared physics world just for a preview was considered
and rejected, because it would falsify the "no play session → step count 0" invariant `docs/GAME-LIFT.md`
records as tested. What the preview *does* prove is what the component is actually choosing —
footprint, depth, subdivision density — accurately and immediately, at the cost of never animating.

**Winding: the algebra is derived, and — despite the source's own caveat — it is now checked.**
`generateFluidSeedShell`'s per-face tangent-pair table (which pair of axes is treated as outward on
each of the box's six faces) is derived algebraically in the function's own comments, and both
`FluidVolume.hpp` and `FluidVolume.cpp` still say, in the same sentence, that it "has not been checked
by running anything." That sentence is stale: `FluidVolumeTest.cpp::testEveryTriangleFacesOutward`,
added in the **same commit** (`b887a78`) as the comment itself and wired into that binary's `main()`,
checks precisely the property the comment says only a per-face winding check could catch — every
triangle's `(C-A)×(B-A)` normal against its own centroid, over a non-uniform 4×3×2 lattice chosen so a
single wrong tangent-pair entry has more than one quad to show up on. Whether that test currently
*passes* was not re-verified for this document — the gates are recorded elsewhere as red pending a
re-record (see `docs/STATUS.md`) and this pass did not build or run anything — but the claim that the
winding is *unchecked* is not accurate as written; the check exists in the tree.

---

## 6. Two surfaces, never both on the same body of water

`simulate` on a `WATER` record is a hard fork, not a blend: `applyLevelWater` returns immediately after
spawning a simulated volume rather than falling through to the Gerstner path, because drawing both
would put two waters in the same hole fighting over the same depth. The unsimulated default — every
`WATER` record before `simulate` existed, and every one that omits it — is the analytic surface:
`WaterRenderer` draws one Gerstner-displaced 129×129 grid, recentred under the camera each frame by
moving a world-space origin uniform rather than re-uploading geometry, with the displacement applied
entirely in the vertex shader. `GerstnerWave.hpp` is pure (no `rhi::`, no device), which is what lets
`GerstnerWaveTest.cpp` check it with no GPU — but `WaterShaders.hpp` hand-ports the identical formula
into HLSL rather than sharing source with it (there is no C++/HLSL shared-source mechanism anywhere in
this engine), so the two copies can drift and nothing in the build checks that they still agree.

**Both surfaces share one real gap: neither goes through `IDevice::drawMesh`.** `WaterRenderer::transparentPass`
and `FluidScene::transparentPass` both call `ctx.drawIndexed` directly on their own vertex/index
buffers, so *either* kind of water — analytic ocean or simulated pool — never enters the shadow
cascades, the ray-tracing TLAS, or GI voxelisation, and casts no shadow of its own for the identical
reason: nothing routes it through the one call every other opaque or transparent draw in this engine
goes through.

Buoyancy reads a **flat, analytic plane** regardless of which surface is on screen —
`aver_phys_set_water_plane` is given `waterRenderer_.waterLevelCm()`, a constant height, never a
per-point sample of either the Gerstner sum or the simulated shell's own deformed vertices. A boat
riding the actual wave height, or floating debris that dips with a slosh, is still hypothetical.

---

## 7. What this does not do

Some of these correct `modules/fluids/README.md`, which was last edited in the same commit as the
merge (`b887a78`) — after the `WATER` record and the simulated volume had already landed — and still
carries at least one bullet ("No `.ocworld` water record") that was already false the day it was
written. This section is the current one; treat the module README's own limitations list as
superseded wherever the two disagree.

- **The simulated volume is reported to collapse under its own calibrated proportions, and nothing in
  the tree since has touched it.** A prior session's live editor capture (recorded 2026-08-24) found a
  `COMP ... Fluid` / `WATER ... simulate` volume that was genuinely stepping — its physics-world
  bytes moved frame to frame by an order of magnitude more with a play session running than without
  one — but settling into a thin, wispy puddle rather than holding a body of water, reproduced
  free-standing on a floor, inside a walled basin, and specifically at the FirstPerson pool's own
  proportions (6×4×1.2 m, 8×8×4) that `kFluidPressureHeadroom`'s own sweep comment
  (`FluidVolume.hpp`) records as holding 97% of starting depth at the same headroom value
  (`0.6`) the shell actually ships with. `git log 721ec74..b952b8a -- modules/fluids modules/physics
  modules/render.softbody tests/fluids tests/physics` returns nothing, so the exact code that capture
  observed collapsing is unchanged at this document's head. This pass could not rebuild or rerun the
  editor to reproduce the capture itself — that is out of scope for a documentation-only pass under
  this session's own rules — so this is reported as an **open, unresolved** finding carried forward
  from that prior observation, not one re-confirmed here. `SoftBodyTest::testPressureHoldsAShellUp`
  passes at unit-test scale, which is the gap worth starting from: what `FluidScene::spawn` builds for
  a live scene against what that test builds for the same numbers.
- **No splash, pour, merge or split** (§2) — a fixed-topology shell cannot tear or join, by
  construction, not by omission.
- **No soft shoreline.** Neither surface samples scene depth, so either one meets sloped terrain in a
  hard line rather than a fade. This is not currently a backend gap — `IDevice::sceneDepthTexture()`
  is implemented on both D3D12 and Vulkan (`VulkanDevice::sceneDepthTexture`, added `26220c6`, mirrors
  `D3D12Device`'s own structurally; only D3D11 still returns the base class's `0`) — it is simply
  never called from either water surface's own shader today.
- **No per-point buoyancy** (§6) — a body floats against a flat, constant-height plane regardless of
  which surface, or how disturbed it is, is actually on screen.
- **Neither surface reaches shadows, the RT TLAS, or GI voxelisation** — both draw outside
  `IDevice::drawMesh` (§6).
- **The analytic ocean's wave clock advances through `GameWater::update`.** `WaterRenderer::tick(dtSeconds)`
  is called from `game::GameWater::update` (`Runtime/src/GameWater.cpp`) whenever the analytic surface
  is attached, and both hosts call `water_.update(...)` once per frame (`Runtime/src/GameApp.cpp`,
  `sandbox/src/SandboxApp.cpp`), so the Gerstner grid's per-vertex phase moves in the editor and in a
  running game. The clock is `WaterRenderer`'s own, summed from each frame's `dt` (clamped to `[0, 1]`
  per call, see `WaterRenderer::tick`), not the composition root's `Timestep::total`. The simulated
  volume keeps its own separate clock (`FluidScene::update`'s `elapsedSeconds` parameter, the
  composition root's ordinary `Timestep::total`).
- **`FluidScene` — the solver join itself — has no dedicated test.** Everything under §1's "Checked
  by" column that touches it is either pure arithmetic (`FluidVolume`, tested with no device) or the
  physics-only damping calibration; nothing exercises `FluidScene::spawn`/`update`/`transparentPass`
  end-to-end.
- **No verified Vulkan parity** for either surface. Both are written against generic `rhi::`
  vocabulary with no backend-specific code, and nothing in the test suite compiles or runs either draw
  path through the Vulkan backend — so "should compile" is not the same claim as "produces the right
  image," and this page makes only the first. (The module's own README additionally claims Vulkan
  refuses the post chain either surface would composite through; that specific claim was not
  re-verified for this page, and `VulkanDevice.cpp`'s own `runPostChain`/`createPostPipelines` — bloom,
  eye adaptation, tonemap/gamma composite — reads as a real implementation rather than a stub, so treat
  that particular claim with suspicion until someone actually runs a fluid surface through it.)
- **Jolt soft-body traps worth knowing before authoring a new one**, all encountered and fixed the
  first time a fluid volume asked Jolt for real pressure (`3ae1e3a`; recorded in
  `modules/physics/src/PhysicsWorld.cpp`'s `buildSoftShared`/`addSoftBody` comments) but easy to
  reintroduce in a future caller:
  - **Inward winding silences pressure with no warning at all.** Jolt's `ApplyPressure` opens with
    `if (six_volume > 0.0f)` — an inside-out shell gets exactly zero pressure, which reads from the
    outside as "the pressure coefficient is too small," not as a winding bug. A ×375,000 change to the
    coefficient once moved the rendered image by 1317 pixels out of 7 million, because the code path
    genuinely never ran.
  - **Buoyancy hard-asserts on a soft body.** `Body::ApplyBuoyancyImpulse` starts with
    `JPH_ASSERT(IsRigidBody())`; `modules/physics/src/Buoyancy.cpp` now skips non-rigid bodies on both
    the per-body and global-plane paths, but a new buoyancy call site that does not check this will
    crash the instant a fluid volume (which sits below its own water plane by construction) is fully
    submerged from step one.
  - **Pressure must be derived in metres, not the engine's own centimetres.** Jolt's coefficient acts
    as `pressure * area / volume`; a constant tuned for one pool is a rounding error inside a larger
    one, and omitting the cm→m conversion factor (`kFluidCmToJolt`, `1.0e-4`) is a 10,000× error in one
    direction that inflates a shell until it swallows the camera, with no error message pointing at
    the cause.
