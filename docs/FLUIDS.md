# Aver Fluids — a particle fluid, real-unit materials, and two surfaces

`Aver.Water` and `Aver.Render.Fluid` are gone; `Aver.Fluids` (`modules/fluids/*`) is what replaced
them, in one commit (`b887a78`) whose own reasoning is worth repeating rather than paraphrasing: the
split "turned out to describe the implementation rather than the domain — an author placing a `WATER`
record does not care whether the surface they get is evaluated in a vertex shader or stepped by a
solver." The module still keeps its physics dependency scoped rather than inherited:
`FluidScene.cpp` — the only file that touches the physics world (one overlap query, to voxelise scene
colliders) — is compiled in only when `AVER_MODULE_PHYSICS` is on; the analytic surface, the underwater
fog override, `FluidVolume`'s desc and preview arithmetic and the whole particle solver and surface
mesher build without it (`modules/fluids/CMakeLists.txt`).

**The simulated volume is a particle fluid.** It used to be a Jolt soft-body shell (a pressurised
balloon); no single pressure held every shape, and it collapsed to a puddle. It is now a
position-based fluid (Macklin & Müller 2013) on the CPU. §2 is the model, its limits and its cost.

This page is about the part that is new since the merge — real-unit authoring and the simulated
volume — and gives the analytic surface and underwater fog only what they need to sit beside it
honestly. For the wave math and fog override in their own right, `modules/fluids/README.md` still
holds; where this page and that one disagree, this page is the newer one and says why (§7).

---

## 1. What exists

| Piece | Where | Checked by |
| --- | --- | --- |
| The position-based fluid solver, with its scene-collider distance field | `modules/fluids/{include,src}/aver/fluids/PbfFluid.{hpp,cpp}` | `PbfFluidTest.cpp`: `testTankHoldsItsDepth`, `testDamBreakSpreads`, `testSolidFieldAndFloorCollision`, `testDeterminism`, `testImpulse` |
| The surface mesher (density splat, marching tetrahedra) | `.../FluidSurface.{hpp,cpp}` | `PbfFluidTest.cpp::testSurfaceMesh` |
| Desc → solver settings (spacing, walls, viscosity) | `.../FluidVolume.{hpp,cpp}` — `fluidPbfSettings`, `fluidParticleSpacingCm`, `fluidXsphForDamping` | `PbfFluidTest.cpp::testDescMapping` |
| Viscosity → damping (the old calibrated fit, kept as the scale) | same file — `fluidDampingForViscosity` | `FluidVolumeTest.cpp`: `testViscosityDampingFit`, `testMaterialPresets` |
| The material precedence rule | same file — `fluidResolvePhysicsMaterial`, called only from `FluidScene::spawn` | `testResolveMaterialPrecedence` |
| The preview shell (a box, closed, wound, sized) | same file — `generateFluidSeedShell` | `FluidVolumeTest.cpp`: `testClosedAndConsistentlyWound`, `testNoDuplicatedSeamVertices`, `testExtentMatchesRequest`, `testEveryTriangleFacesOutward`, `testSubdivisionScaling` |
| The solver join (collider voxelisation, GPU mesh, staging ring) | `modules/fluids/{include,src}/aver/fluids/FluidScene.{hpp,cpp}` | no dedicated test — see §7 |
| `COMP <id> Fluid` (graph authoring) | `scripting/csharp/Aver.Graph/GraphComponentTree.cs` — `ApplyFluid` | — |
| `WATER ... simulate` (level authoring) | `Runtime/src/GameWater.cpp` — `applyLevel`; format in `modules/formats/{include,src}/aver/formats/OcWorld.*` | the FirstPerson template's own pool (`templates/FirstPerson/Content/Maps/Default.ocmap`) |
| The graph editor's preview | `sandbox/src/GraphEditorComponents.cpp` — the fluid preview mesh | — |
| The analytic Gerstner surface | `modules/fluids/{include,src}/aver/fluids/{GerstnerWave,WaterRenderer}.*` | `tests/fluids/src/GerstnerWaveTest.cpp` |
| The displaced mesh surface (`WATER ... material`) | `modules/fluids/{include,src,shaders}/aver/fluids/WaterSurface.*`, `water_displace.hlsl`; drawn by `game::GameWater` | `tests/fluids/src/WaterSurfaceTest.cpp`, `WaterSurfaceCompileTest.cpp` |
| The underwater fog override | `modules/fluids/include/aver/fluids/Underwater.hpp` | `tests/fluids/src/UnderwaterFogTest.cpp` |

Two authoring paths (§4) converge on one spawn function, which is the one place a design rule about
conflicting inputs is enforced (§3). Two rendered surfaces (analytic and simulated) exist side by side
and never both apply to the same body of water (§6).

---

## 2. The particle model, its limits and its cost

**Solver.** `PbfFluid` is a position-based fluid. Each fixed substep (1/60 s, at most two per frame):
apply gravity, predict positions, find neighbours with a spatial hash (kernel radius = 2 × spacing),
then `iterations` passes of the density constraint — `C = ρ/ρ0 − 1` from the poly6 kernel, a Lagrange
multiplier per particle, position corrections from the spiky-kernel gradient — with the tensile-
instability correction (`s_corr`) added to each pair. Velocity is the position change over the step,
then XSPH viscosity and optional vorticity confinement. Everything is in metres, local to the volume's
centre; the module converts engine centimetres at its boundary (`kFluidCmToM`).

Three choices that matter, each found by running it:

- **Rest density comes from the fill's own lattice** (the kernel sum over an unbounded lattice at the
  seed spacing), so a particle in the interior sits at `C = 0` exactly.
- **Only compression is corrected** (`C` is clamped at 0): free-surface particles have too few
  neighbours by construction and pulling them inward clumps the surface.
- **Walls are a mirrored ghost lattice**, tabulated against distance. Without it, particles on the
  floor see half a neighbourhood and compress to make up the density: a 1 m tank dropped to 68% of its
  depth at any iteration count or relaxation. With it the same tank holds 97% (`testTankHoldsItsDepth`
  asserts >= 90%). The scene's colliders use the same mechanism through the distance field.

**Seeding.** The volume's box is filled with a lattice; spacing comes from `particleSpacingCm`, or by
default from `subdivisions` (8 particles per authored cell, so the 8x8x4 default is ~12 cm and the
FirstPerson pool's 14x14x4 ~13 cm), coarsened until the count fits `maxParticles` (default 4000).
Particles inside scene colliders are not spawned.

**Collision.** The volume's own box: sides and floor are walls, the top is open (`containSides`,
`containFloor`, `containTop`). Scene colliders: at spawn, `FluidScene` voxelises the physics world's
static colliders around the volume into a signed-distance grid (`SolidField`, cell = half the spacing)
through `aver_phys_overlap_sphere`, skipping empty 4x4x4 blocks. Particles inside the skin are pushed
out along the gradient.

**Surface.** Each step the particles are splatted into a density grid (cell = 0.6 × spacing) and the
iso-surface is extracted with marching tetrahedra, normals from the field gradient. Walls mirror the
particles into the field so the water meets them flat. The render path is the shell's: a
`createSkinTargetMesh` with a fixed vertex count, so the surface is a triangle soup in the first 3 × N
vertices of a buffer sized for up to 60,000 triangles, the rest collapsed to a point. Nothing in the
renderer changed.

**Authored fields.** `damping` is now the thickness: `fluidXsphForDamping` maps it to the XSPH
coefficient (0.01 + 0.1 × damping, clamped to [0.01, 0.5]); a `viscosity` or `preset` maps to damping
through the old measured table (`fluidDampingForViscosity`), kept because it is a monotonic scale.
`iterations` is the density passes per substep (2..12). `density` is accepted and carried but does not
change the motion: a PBF fluid is incompressible at one rest density. **`compliance` and `pressure`
were soft-body knobs and are ignored**: they still parse in `.ocworld`, `COMP ... Fluid` and the script
relay, so existing files load, and the first spawn that sets one logs a single line saying so.

**Cost** (one thread, standalone MSVC `/O2` build of the solver alone on the development PC; not
measured inside the engine, so treat as a floor):

| Volume | Particles | Per substep | Surface mesh |
| --- | --- | --- | --- |
| 1 x 1 x 0.8 m tank, 10 cm | 800 | 1.1 ms | 2.7 ms, ~2.4k triangles |
| 6 x 4 x 0.6 m pool, 13.8 cm, 5 iterations | 5520 | 8.6 ms | 15 ms, ~35k triangles |

At 60 fps one frame runs one substep and one mesh build, so the 4000-particle default costs roughly
8 ms a frame and the 5500-particle pool about 24 ms — raise `maxParticles` knowing that. A hitch runs
at most two substeps and drops the rest of the time (the fluid slows rather than spiralling).
Determinism: the same settings and step count give the same bytes (`testDeterminism`).

**What it does not do.**

- Scene colliders are baked static at spawn: a door, platform or character that moves later is not
  seen, and one standing in the volume at spawn is solid for good. Particles do not push rigid bodies
  and nothing pushes them except `FluidScene::addImpulse`.
- No surface tension, foam, spray rendering or particle-level shading: only the iso-surface is drawn.
- The surface is cut off by `kFluidMaxSurfaceTris` (60,000); past it the excess is not drawn and one
  warning is logged.
- Buoyancy still reads the flat plane (§6).

---

## 3. Precedence: a material and a raw knob never silently pick a winner

`FluidVolumeDesc` still carries the four raw knobs it always did
(`compliance`, `damping`, `iterations`, `pressure`; the first and last are retired, §2) alongside the new,
optional `material` field. Both
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
ocean has no size to fill with particles, and a record that asks to be simulated while also declaring
`infinite` is left analytic with a named warning rather than silently ignored. When simulated, the
consumer derives a volume that hangs *below* `level` (the water's top), with depth clamped to the
shallower of 60 cm or the footprint itself, and — for this one call site specifically — overrides
`FluidVolumeDesc`'s own default 8×8×4 subdivision with **14×14×4**, which sets the particle spacing
(about 13 cm for the pool, §2). The FirstPerson template's own pool is the concrete, checked-in instance of
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

## 5. The preview shell, and what the editor preview actually shows

`generateFluidSeedShell` builds a closed, subdivided-box triangle mesh by pure arithmetic — no device,
no solver, no randomness — partitioning a box's boundary lattice into three non-overlapping vertex
blocks by integer index alone, so two faces sharing an edge share the same output vertex rather than
being merged by comparing floating-point positions. `FluidVolume::generateSeedShell()` places that
local shell at the desc's own centre and computes its normals. It exists only as the editor preview
now: the simulated surface comes from the particles (§2).

**The graph editor's Fluid preview shows exactly this box — it does not simulate.**
`GraphEditorComponents.cpp` calls `generateFluidSeedShell` directly and caches the result by size. The
solver only advances inside a Play session, and a preview that stepped it would imply motion the fluid
is not making in edit mode. What the preview *does* prove is what the component is choosing —
footprint and depth — accurately and immediately, at the cost of never animating.

**Winding is checked.** `FluidVolumeTest.cpp::testEveryTriangleFacesOutward` checks every triangle's
`(C-A)×(B-A)` normal against its own centroid over a non-uniform 4×3×2 lattice, so a single wrong
tangent-pair entry in the per-face table shows up. (The source comments that call it unchecked predate
that test.)

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

**A third surface: the displaced mesh.** A `WATER` record that names a `material` (an `.ocmat`) and
does not say `simulate` is drawn by `fluids::WaterSurface` instead of the analytic ocean (never both:
`GameWater` takes `WaterRenderer` off the device). It is a flat grid mesh that a compute pass
(`water_displace.hlsl`, run in `prePass`) moves by the same Gerstner waves, writing position, analytic
normal and world-XY uv into a compute-written vertex buffer; the host draws it through the ordinary
material path (`setDrawBinding` + `setDrawBlended` + `drawMesh`), so it *does* enter the ray-tracing
TLAS (Voxi refits a compute-written mesh's BLAS every frame) and rays see the same waves as the
rasteriser. The record's `cell <cm>` (default 100) and `radius <cm>` (default 12800) size the
camera-following grid (256×256 cells at the defaults; coarsened past 512 cells per axis); a `bounds`
record fills its bounds and stays put. An unbounded grid also carries flat horizon rings out to ~10 km,
each at most twice the previous one (a single ring that size made ~10 km triangles that the RX 7800 XT's
ray tests dropped in radial strips); the waves fade out over the grid's outer 15% so the seam is flat.
Open water over nothing (no seabed in range) reads as deep: a scattering material's thickness ray that
misses counts 40 attenuation distances, and Stage B composites translucent crossings over sky pixels too
(with their own depth, so the deferred sky does not paint over them). The wave maths is a third hand-port (with `water.hlsl` and
`GerstnerWave.hpp`) and drifts the same way. A record with no `material` is unchanged, which is also
why the "neither goes through `drawMesh`" gap below is now true only of the analytic ocean and the
(already mesh-drawn) simulated pool is unaffected.

Buoyancy reads a **flat, analytic plane** regardless of which surface is on screen —
`aver_phys_set_water_plane` is given `waterRenderer_.waterLevelCm()`, a constant height, never a
per-point sample of either the Gerstner sum or the simulated particles' surface. A boat
riding the actual wave height, or floating debris that dips with a slosh, is still hypothetical.

---

## 7. What this does not do

Some of these correct `modules/fluids/README.md`, which was last edited in the same commit as the
merge (`b887a78`) — after the `WATER` record and the simulated volume had already landed — and still
carries at least one bullet ("No `.ocworld` water record") that was already false the day it was
written. This section is the current one; treat the module README's own limitations list as
superseded wherever the two disagree.

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
- **`FluidScene` — the solver join itself — has no dedicated test and has never been run.** The
  solver, the mesher and the desc mapping are tested on the CPU (§1); the collider voxelisation against
  the real physics world, the GPU mesh and the staging ring are compiled but not exercised. Run a
  `WATER ... simulate` level to see it.
- **No verified Vulkan parity** for either surface. Both are written against generic `rhi::`
  vocabulary with no backend-specific code, and nothing in the test suite compiles or runs either draw
  path through the Vulkan backend — so "should compile" is not the same claim as "produces the right
  image," and this page makes only the first. (The module's own README additionally claims Vulkan
  refuses the post chain either surface would composite through; that specific claim was not
  re-verified for this page, and `VulkanDevice.cpp`'s own `runPostChain`/`createPostPipelines` — bloom,
  eye adaptation, tonemap/gamma composite — reads as a real implementation rather than a stub, so treat
  that particular claim with suspicion until someone actually runs a fluid surface through it.)
