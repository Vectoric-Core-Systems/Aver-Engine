# Aver Engine - Cloth and destruction on Jolt soft bodies

Cloth and destructible objects are Jolt soft bodies. The engine owns what Jolt has no notion of: the
topology (cage), the material yield model, tear resolution, the pieces a tear leaves, the `.ocsoft`
asset, and the scene join. The earlier Verlet/Gauss-Seidel solver, its `Aver.SoftBody` async worker, the
`aver_sb_*` C ABI, the Window > Soft Body (plastic) panel and the `modules/fracture` skeleton are retired
(see `docs/STALE_CODE.md`). Plan and rationale: `docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md`.

Status: wave 9 is built and tested; the wave 10 depth work (section 10) is written against its frozen design
(`docs/plans/wave10/SOFTBODY_DEPTH.md`) and awaits its first build and run. Every number marked UNVERIFIED in
this file is a starting value that the headed tuning pass (`test-content/SoftBodyLab`, "Tuning") sets.

## 1. Layers

```
 .ocsoft + preset          Aver.SoftBody (pure; Core + Formats)
 + CCloth/CDestructible -> SoftAsset, SimMesh (weld -> Cage + RenderBinding), Damage, Islands, Material
                                   |  arrays only
 Aver.SoftBody.Scene  SoftBodySystem  (scene + physics + anim.scene)
   inbox -> tick: impacts -> readback -> damage/plastic -> tear/rebuild/split -> skin+wind+LOD -> publish
                                   |  aver_phys_sb_*                |  PieceView
 Aver.Physics (DLL)  physics_softbody_abi.h        Aver.Render.SoftBody  SoftBodyScene (draw only)
```

| Module | Owns |
|---|---|
| `Aver.SoftBody` (`modules/softbody`) | `Material`/presets, `Cage`, `SimMesh`, `Damage`, `Islands`, `RenderBinding`, `SoftAsset` (.ocsoft). No scene, GPU or Jolt. |
| `Aver.Physics` (`modules/physics`) | `aver_phys_sb_*`: builder, rebuild under the same handle, in-place rest edits, aero, skin push, impact events. |
| `Aver.SoftBody.Scene` (`modules/softbody.scene`) | `CCloth`/`CDestructible`, `SoftBodySystem`, the tick, the inbox, events, the framework provider. |
| `Aver.Render.SoftBody` (`modules/render.softbody`) | Draw only: epoch mesh rebuild, upload ring, bounds. |
| `Aver.Framework` | `aver_fw_softbody_*` relay ABI for C# and graph nodes. |

Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed) at every boundary;
conversion to Jolt stays in `modules/physics/src/Convert.hpp`.

## 2. The model

A cage is particles joined by edges (`Beam`), dihedral bends between triangle pairs, and the triangles.
`SimMesh` welds a render mesh (vertices equal within `weldCm`) into particles and keeps a `RenderBinding`
from render vertices to particles, so a seam in the render mesh does not become a tear in the sim.

Jolt reads an edge's rest length and a bend's rest angle live. That is what makes damage cheap:

- A **dent** (plastic set) is an in-place write of new edge rests and bend angles
  (`aver_phys_sb_set_edge_rest`, `aver_phys_sb_rest_bends_from_pose`). The body is not rebuilt.
- A **tear** is a topology change: the system runs `resolveTears`, plans islands, and rebuilds the body
  under the same handle (`aver_phys_sb_builder_finish` with `replaceBody`), carrying every surviving
  particle's position and velocity across through a state map. Extra islands become new bodies and
  debris entities.

Materials (`Material.hpp`) use Jolt's compliance (m/N; 0 is inextensible) plus yield and break values:
`yieldStrain` (plastic creep starts), `hardening`, `maxSetCm`, `breakStrain`, `toughnessJ` per edge,
`behavior` (`deform`, `fracture`, `shatter`: brittle behaviours snap at yield and do not dent).

| Preset key | Area density kg/m2 | Edge compliance | Bend compliance | Iterations | Behaviour | breakStrain |
|---|---|---|---|---|---|---|
| `cloth_light` | 0.25 | 0 | 0.05 | 6 | deform | 0.40 (tearable only) |
| `cloth_heavy` | 0.8 | 0 | 0.01 | 6 | deform | 0.35 (tearable only) |
| `metal` | 7.8 | 2e-7 | 1e-6 | 8 | deform, yield 0.010, set to 25% of rest | 0.30 |
| `wood` | 6.0 | 1e-6 | 5e-6 | 8 | fracture | 0.020 |
| `glass` | 10 | 2e-7 | 1e-6 | 16 | shatter | 0.006 |

All preset values are UNVERIFIED starting points. Glass runs 16 iterations: at 8 a 180 cm pane hung by its top edge settles at 0.0058 strain against the 0.006 yield, so waking it with any touch (0.001 J) shattered it (measured in `SoftBodyCalibrationTest`).

## 3. `.ocsoft`

JSON, UTF-8, `"format":"ocsoft"`, `"version":1`. Unknown keys are ignored; a newer version is refused.
Read with `fmt::JsonValue`, written by the small emitter in `SoftAsset.cpp`. A minimal asset:

```json
{"format":"ocsoft","version":1,"kind":"cloth","preset":"cloth_light"}
```

`kind` selects which of `cloth` and `destruct` is read; `preset` names a row of the table above and any
key under `material` and `sim` overrides it. Full schema:

```json
{ "format":"ocsoft", "version":1, "kind":"cloth", "preset":"cloth_light",
  "material": { "areaDensityKgM2":0.25, "compliance":0, "bendCompliance":0.05, "yieldStrain":0,
                "hardening":0, "maxSetCm":0, "breakStrain":0.4, "toughnessJ":0,
                "behavior":"deform", "friction":0.4, "restitution":0 },
  "sim":      { "iterations":6, "damping":0.3, "vertexRadiusCm":0.4, "maxSpeedCmS":2500,
                "gravityScale":1, "weldCm":0.01, "maxParticles":4096, "allowSleep":true },
  "cloth":    { "dragCoeff":1.0, "liftCoeff":0.3, "tearable":false,
                "skin": { "defaultReachCm":6, "backstopCm":-1 } },
  "destruct": { "minImpactCmS":150, "impulseEfficiency":0.6, "maxKickCmS":1200,
                "minPieceTriangles":4, "debrisLifetimeS":12, "maxPieces":16 },
  "paint":    { "meshKey":"<hex fnv1a64 of vertex count + welded positions>",
                "pin":"<hex RLE, one u8 per sim particle, 255 = pinned>",
                "reach":"<hex RLE, u8 per particle, 0 = hard skin, 255 = free>",
                "rules":[ {"type":"above","axis":"z","valueCm":170},
                          {"type":"bone","bone":"spine_03","minWeight":0.5},
                          {"type":"sphere","centreCm":[0,0,120],"radiusCm":8},
                          {"type":"mask","texture":"Textures/cape_mask.png","pinBelow":0.5} ] } }
```

Paint is stored per welded particle and keyed by `meshKey`. When the key no longer matches the mesh the
editor warns ("mesh changed since painting") and the runtime applies `rules` instead. `reach` maps
linearly to [0, 2 x defaultReachCm]. A pinned particle has inverse mass 0 and a hard skin (max distance
0). The `mask` rule samples the R channel at the vertex UV (below `pinBelow` pins) and G scales reach.

## 4. Components

Both are registered at runtime (`SoftBodySystem::registerComponents`); zero-filled storage means the
useful state (a `0` scale reads as 1, a `0` preset is the first preset).

- `CCloth` (40 bytes): `asset`, `preset`, `flags` (negative sense: `kClothNoWorld`,
  `kClothNoCharacterProxy`, `kClothNoWind`; positive: `kClothTearable`, `kClothRelease` one-shot),
  `windScale`, `stiffnessScale`, `reachScale`, `freezeDistanceCm`, OUT `outMaxStrain`, `outState`.
- `CDestructible` (48 bytes): `asset`, `preset`, `flags` (`kDestructNoDebris`, `kDestructIndestructible`),
  `toughnessScale`, `impactScale`, `debrisLifetimeS`, `maxPieces`, OUT `outDamage`, `outBroken`,
  `outPieces`, `outState`.

`attachCloth` / `attachDestructible` write defaults and add the `CSoftBody` marker (`kSoftBodyPiece = 0x2`
marks runtime pieces: owned by the system, never saved). Cloth and Destructible on one entity are
refused; a tearable cloth is a cloth with `kClothTearable`.

Level records (`.ocworld`):

```
COMPONENT Cloth asset=<path> preset=cloth_light wind=1 stiffness=1 reach=1 freeze=0 tearable=1 world=0 proxy=0 windon=0
COMPONENT Destructible asset=<path> preset=metal toughness=1 impact=1 lifetime=0 maxpieces=0 indestructible=1 nodebris=1
```

Default keys are omitted when the editor saves. A level file never stores runtime state (damage, pieces): a loaded level
starts intact. Damage rides the save game's `softbody` channel instead (section 10).

## 5. Tick

`game::tickSoftBodies(dt, viewPos)` runs once per frame right after `animSystem().tick` and before the
skinned scene update, in the editor (Play) and the packaged game. Order is the determinism contract
(entities ascending by id, then piece id):

1. Resolve new or changed entries (at most 2 builds per frame, the rest wait).
2. Apply the inbox in arrival order: `damage`, `explode`, `damageNear`, `tearAt`, `repair`, `release`,
   `setPreset`, `setWind`, `setBudget`.
3. Poll physics impacts and turn closing speeds above `minImpactCmS` into energy deposits.
4. Read back each awake destructible or tearable piece once, mirror into the cage.
5. Apply deposits, evaluate strain, write plastic set in place.
6. Tear pipeline for any newly broken beam, within the per-frame rebuild budget; leftovers carry over.
7. Cloth: refresh the palette, push skin targets (hard skin after a teleport), aero and wind, character
   proxies, LOD tiers and freezing.
8. Debris lifecycle, OUT fields, publish `PieceView`s, fire events.

Outside Play nothing steps and soft entities show their bind pose.

### Cloth

All cloth is Jolt `Skinned`. A cape uses the skeleton palette; cloth on a prop uses a single joint whose
matrix is the entity world, so pinned vertices ride the entity. The palette math is
`jointMatrices[j] = invBuildWorld * S[j]`, composed in the ABI with the entity world. Cloth lives on
physics user layer 14 and collides with layer 0 and the proxy layer 15 only (`Options::clothLayer`,
`proxyLayer`). For cloth under a skeleton the host starts a kinematic ragdoll on the proxy layer so the
body is a set of capsules (`setCharacterProxyFn`). Wind is global (`setWind`) scaled per entity by
`windScale` and the asset's drag and lift; it is independent of the sky's `cloudWind`
(Environment has a "Copy to cloth wind" button). Release drops all pins and rebuilds without skin entries.
No self-collision in v1.

### Destruction

Inputs: rigid-body impacts, `damage` (point, radius, energy, direction, kind), `explode` (sphere overlap
with falloff and radial impulse), `tearAt`. After a tear, islands smaller than `minPieceTriangles`
become dust (a `Dust` event, no body). Each other island becomes a debris piece (a `SoftPiece` entity with
`CSoftBody{kSoftBodyPiece}`), itself destructible until `maxPieces`. Debris sleeps, shrinks and is
destroyed after its lifetime; a global cap evicts oldest-farthest.

Budgets (`SoftBodySystem::Options`): `maxLivePieces` 48, `maxActiveParticles` 20000, `maxRebuildsPerFrame`
2, `maxDepositsPerFrame` 64, `debrisLifetimeS`/`debrisSleepS`/`despawnShrinkS` 12/2/0.6, freeze distance
cloth/debris 6000/4000 cm, distance iteration tiers at 1500 and 3500 cm.

### Events

`SoftEventKind`: Impact, Torn, Detached, Dust, Repaired, Released. Each carries a per-system `seq`.
The notify sink fires `SoftBodyImpact`, `SoftBodyTorn`, `SoftBodyDetached`, `SoftBodyDust`,
`SoftBodyRepaired` at the owning entity (Detached also at the new piece). C# polls
`SoftBodies.DrainEvents`.

## 6. Render

`SoftBodyScene` no longer creates bodies. It reads `SoftBodySystem::pieces()` and keeps one resident per
(entity, piece). A topology epoch change builds a new source mesh and skin-target mesh and puts the old
ones on a retire list destroyed `kSoftBodyFramesInFlight + 1` frames later. Between epochs it copies
positions through the upload ring. Each frame it writes the piece's AABB into `CMeshRenderer` so a flung
piece is not culled at its old place. The packaged game now has a `SoftBodyScene` too
(`DrawWorldOptions::softBodies`). No shader or RHI call was added.

## 7. Framework relay and scripting

`framework_softbody_abi.h` (`aver_fw_softbody_*`, one provider function, deferred ops): damage, explode,
tear, repair, release, wind, budget, preset, state, events. C# (`Aver.Framework/SoftBodies.cs`): `Entity`
gains `AddCloth`, `AddDestructible`, `SetSoftPreset`, `ApplySoftDamage`, `TearSoftBody`, `RepairSoftBody`,
`ReleaseCloth` and `TryGetSoftBodyState`; the static `SoftBodies` class has `SetWind`, `Explode`,
`SetBudget` and `DrainEvents`. The raw `aver_phys_sb_*` calls are `Aver.Physics.SoftBody`
(`scripting/csharp/Aver.Physics`).

Aver Node nodes (`GraphSoftBodyNodeTable.cs`, matching `sandbox/src/GraphNodeDefsSoftBody.hpp`):
`softbodydamage`, `softbodyexplode`, `softbodytear`, `softbodyrepair`, `releasesoftbody`,
`setsoftbodywind`, `setsoftbodypreset` (exec) and `getsoftbodystate` (pure).

## 8. Editor

- Details panel sections for Cloth and Destructible (preset, asset, scales, flags, live OUT readout).
- Add Component entries for both.
- `SoftBodyPaintTool`: pin and reach painting, rule editing, undo through `SnapshotUndo`.
- Show > Soft Bodies: strain, pins, pieces, wind and budget overlays.
- `test-content/SoftBodyLab` is a level with a caped character, flag, curtain, glass pane, steel door and a
  plank crate.

## 9. Not here

- No self-collision, no cloth-on-cloth.
- No render LOD: one draw mesh per piece; sim meshes over `maxParticles` (4096) are refused, not decimated.
- No runtime Voronoi or pre-diced shards; fracture is the tear pipeline over the authored triangle mesh.
- Saved damage covers soft bodies only (section 10); rigid bodies keep their own save rules.
- Replication is not designed here; `seq` and the deterministic tick order are what a net layer would key on.

## 10. Depth (wave 10)

Design and the decisions behind each item: `docs/plans/wave10/SOFTBODY_DEPTH.md`.

**Capped breaks.** A torn surface is no longer a paper edge. `Caps.hpp` builds, per piece, a back skin (the front offset
inward along its smooth normal by the thickness) and side walls along every boundary edge. Draw arrays are laid out
`[front | back skin | walls]`; cap vertices carry `sourceVertex = kGeneratedVertex` and generated UVs
(`caps.uvCmPerTile`). With `caps.interiorMaterial` set, the walls are a child cap entity published as a second
`PieceView` (layer 1); empty keeps one material. Thickness defaults per preset (`PresetInfo::capThicknessCm`: cloth 0,
metal 0.5, wood 2, glass 0.6 cm); `caps.mode` is auto / off / on, `caps.backSkin` auto / off / debris / all, and
`caps.lodCm` drops caps past a distance. Meshes without boundary edges (closed solids) get no walls.

**Soft Fields.** One force-and-effect volume with six kinds (radial, directional, damage, release, kill, sleep), four
shapes (sphere, box, capsule, all), a falloff, turbulence and a target mask (cloth, destructible, debris). It exists as the
`CSoftField` component (64 bytes, placed through the `SoftField` COMPONENT row), as a transient field started from script
(`SoftFields.Start`, with a lifetime), as a one-shot `Pulse`, as a spawned field entity, or as `SoftFields.Impact`.
Order within a tick: pulses in arrival order, component fields by ascending entity id, transient fields by ascending handle,
then hold/release and lifetimes. Component fields apply continuously (per second) unless `once` is set, which makes them a
pulse. Velocity goes through `aver_phys_sb_add_velocity` (physics ABI v2), so a field never rebuilds a body.

**Break effects.** `effects.tear`, `effects.detach` and `effects.dust` each take a particle path, a sound list, count,
volume, pitch jitter and a cooldown; the particle half goes through `aver/core/EffectSpawn.hpp` (the GPU particle
module owns the effects themselves) and the sound half through `setSoundSink`, which the host installs with
`installSoftBodyEffects` (`Runtime/include/aver/game/SoftBodyEffects.hpp`). Both are cheap to skip: `setEffectsEnabled(false)`
turns them off, and `restoring_` (loading a save) suppresses them.

**Saved damage.** The `softbody` `$State` channel stores one record per damaged entity: `SB1 <meshKey> <assetKey> <base64>`.
The body is a tear log (replayed through the same tear pipeline, so topology is bit-identical), beam deltas, a gone list
and particle position and velocity, quantised to 16 bits above 512 KB and merged into fewer groups above 64. A record
whose mesh or asset key no longer matches is stale and skipped. `softBodySaveSkip`, `softBodyStateCount/At/Set` and
`softBodyResetForRestore` (`SoftBodySave.hpp`) are the host hooks.

**Proxy cage.** A mesh over the 4096-particle cap, or one the author wants cheap, simulates a low-poly proxy and the full
mesh follows it. `proxy.mesh` names the proxy; each render vertex binds to its closest proxy triangle by barycentric
coordinates plus a smooth-normal offset (deliberately not mean-value, harmonic or Green coordinates). A tear on the proxy
tears the render mesh along the same triangle edges. The sandbox's Proxy tool cooks a proxy (weld, then decimate through
Trifactor when built in) and writes the path into the asset.

**Cheaper debris.** Pieces are created invisible and shown only when the draw side reports the mesh ready
(`revealOnReady`), so a tear never pops a frame of wrong geometry. `Options::maxDrawBuildsPerFrame` budgets the draw-side
mesh builds; a settled debris piece is baked (`destruct.bake`, default on) to a static mesh and its soft body is removed;
far pieces step on a cadence and sub-pixel pieces are culled.

**ABIs.** `aver_phys_sb_*` version 2 (`aver_phys_sb_abi_version`, `aver_phys_sb_add_velocity`).
`aver_fw_softbody_*` version 2 (provider ops 11 to 15: clear debris, set asleep, dents, effects enable, repair all).
`aver_fw_softfield_*` version 1 (`framework_softfield_abi.h`: one 24-float spec, ops 1 to 5). All three refuse before their
provider is installed. C#: `Aver.Framework/SoftFields.cs` (`SoftFields`, `SoftFieldSpec`, `Entity.AddSoftField`) and the
SoftBody v2 members. Aver Node: seven new SoftBody rows and the Fields family (`GraphFieldsNodeTable.cs`, eight rows).

**Editor.** Details sections for SoftField (kind, shape, extents, strength, energy, falloff, direction, targets, once, off,
and a gizmo in the field's colour: radial orange, directional blue, damage red, release yellow, kill magenta, sleep grey),
and for Destructible Caps, Effects, Proxy and Debris. Show > Soft Bodies gains Fields and Tuning; the Tuning HUD prints
the last deposit, hit entity, dent depth and strain.

**Tuning.** `test-content/SoftBodyLab/Content/Maps/SoftBodyTune.ocworld` has one target per preset; the Hit buttons of the
Details section deposit 5, 25, 90, 400 and 3000 J. `SoftBodyCalibrationTest` prints the same ladder headless and asserts
only coarse brackets. Every preset number stays UNVERIFIED until that pass is done headed.

## Tests

`tests/softbody` (`SoftBodyTest`, `SoftAssetTest`, `SimMeshTest`, `DamageTest`, `IslandsTest`, `FieldsTest`, `CapsTest`,
`SoftStateTest`, `ProxyBindTest`),
`tests/physics` (`SoftBodyBuilderTest`), `tests/softbody.scene` (`SoftBodySystemTest`, `SoftBodyDepthTest`, `SoftBodyCalibrationTest`), `tests/framework_softfield`,
`tests/render.softbody` (`SoftBodySceneTest`), `tests/editor` (`SoftBodyDetailsModelTest`, `SoftFieldDetailsModelTest`),
`scripting/csharp/Aver.Graph.Tests` (`SoftBodyNodeTests`, `FieldsNodeTests`). Physics-dependent thresholds are UNVERIFIED
until the first run.
