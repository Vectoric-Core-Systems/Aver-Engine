# OpenConstructor Soft-Body Vehicle Damage Solver — Implementation-Ready Port Spec

Authoritative sources (all paths absolute):
- `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/Source/OpenConstructor27/Public/VehicleDamage.h` (data structures, tunables)
- `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/Source/OpenConstructor27/Private/VehicleDamage.cpp` (solver core, parser, impact, detach)
- `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/Source/OpenConstructor27/Public/VehicleSolverAsync.h` + `Private/VehicleSolverAsync.cpp` (async worker)
- `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/Source/OpenConstructor27/Private/OCDebrisChunk.cpp` (runtime debris)
- `.../Private/OC27FractureLibrary.cpp` (editor-only Voronoi bake — see §12)
- Clean port reference: `C:/Users/User/Documents/OpenConstructorSupportAssets/OCServer/OCSimCore/{include/oc_sim.h, src/oc_sim.cpp, src/math/Vec3.h}`

Note: `OCSimCore` (`oc_sim.cpp`) is an **explicit P0 stub** proving C#↔C++ interop, not the damage solver. Its only reusable artifact is `oc::Vec3` (header-only float vector mirroring `FVector` semantics). The damage solver itself lives only in the UE files; this spec extracts from UE source and flags every UE dependency (§14).

---

## 1. Coordinate system, units, conventions

| Property | Value | Source |
|---|---|---|
| Coordinate frame | Car/vehicle **local** space; node positions authored in this frame | `VehicleDamage.h:79` (`Position // LOCAL space (car frame)`) |
| Axis convention | Z-up, left-handed (UE `FVector`); `oc::Vec3` mirrors it | `Vec3.h:1`, `oc_sim.h:31` |
| Length unit | **centimetres (cm)** | `VehicleDamage.h:55,67` |
| Force unit | **Newtons (N)** | `VehicleDamage.h:55,63-66` |
| Stiffness units | AxialStiffness/PlasticStiffness = **N per cm** of stretch | `VehicleDamage.h:63,66` |
| Velocity | cm/s (`MaxNodeSpeed`, debris kick) | `VehicleDamage.h:243` |
| Gravity | cm/s² (UE world ≈ 980) | `VehicleDamage.h:575` |
| Time | seconds | throughout |
| Y-mirror | The `.ocbeam` is **Y-mirrored** vs the Blender/glTF mesh export; impact injection flips `.Y` under `bDeformMirrorY` | `VehicleDamage.cpp:2704-2713`, `1255-1259` |
| Speed conversions | km/h → cm/s `× 27.78`; cm/s → km/h `× 0.036` | `VehicleDamage.cpp:1264, 2863` |

The cage solver operates **entirely in car-local space**. World↔local mapping happens only at the impact boundary via a transform (`GetCageTransform`, `VehicleDamage.cpp:673-681`). Freed-debris gravity is transformed into car-local space so loose nodes fall "down" regardless of car orientation (`VehicleDamage.cpp:2414-2416`).

---

## 2. `.ocbeam` file format (text)

Plain UTF-8 text, line-oriented. Parsed by `ParseOcbeam` (`VehicleDamage.cpp:199-524`).

**Lexing rules** (`VehicleDamage.cpp:221-253`):
- Each line is trimmed; a single trailing `;` is stripped.
- Blank lines and lines starting with `#` are comments/skipped.
- A bare keyword line switches the current section **mode**: `MATERIAL`, `NODE`, `BEAM`, `PANEL`, `PART`.
- `}` closes a section (mode → None).
- Sections `BONE`, `SKIN`, `ANIM` open a **skip-until-`}`** block (import-factory rig data; ignored by the cage parser). `GLB`, `COLLISION`, `HULL` set mode None (import-only).
- `REBOUND <0..1>` is a top-level scalar line → whole-car restitution (`OutRebound`, default sentinel `-1` if absent).

**Record grammar** (delimiter = comma; beams/panels/parts wrap fields in `id(...)`):

| Section | Line form | Fields | Source |
|---|---|---|---|
| MATERIAL | `Name,Stiffness,AxialStiffness,BendForceN,BreakForceN,PlasticStiffness,MaxBend,BendAbsorb,BreakAbsorb,Behavior` | exactly **10**, comma-sep | `VehicleDamage.cpp:257-283` |
| NODE | `ID,X,Y,Z` | exactly **4** | `VehicleDamage.cpp:287-304` |
| BEAM | `BeamID(NodeA,NodeB)` | ≥2 inner | `VehicleDamage.cpp:308-332` |
| PANEL | `PanelID(BeamA,BeamB,BeamC[,MaterialOverride])` | ≥3 inner | `VehicleDamage.cpp:336-365` |
| PART | `<id>(Name,Role,Material[,MeshName][,DETACH=<frac>][ ,[panelID,panelID,…] ])` | ≥3 head fields; optional `[...]` panel-id list | `VehicleDamage.cpp:369-434` |

Notes:
- `Behavior` token: `"SHATTER"` or `"GLASS"` → `Shatter`; `"FRACTURE"` → `Fracture`; anything else → `Deform` (`VehicleDamage.cpp:278-281`).
- Panel material list is inside `[...]`; head fields are before `[` (`VehicleDamage.cpp:378-392`).
- `DETACH=<frac>` is a **position-independent** token pulled out before positional parsing (`VehicleDamage.cpp:405-413`); clamped 0..1.
- PART has **no persisted numeric ID** — the `id(` prefix is parsed but discarded for parts (unlike beams/panels).

**Reference resolution (parser phase 2, `VehicleDamage.cpp:439-518`):** Materials are indexed by name, panels by ID, beams by ID. For each Part→each Panel→each of its 3 beams, the material is pushed **down onto the beam** (`Beam.AxialStiffness = PanelMat->AxialStiffness`, etc.). Panel-level `MaterialOverride` beats the part material. `StructuralStrength` is **not** applied here (kept strength-neutral for shareable compiled assets) — it is applied per-vehicle in `InitSolver`.

The parser is authoritative for both the runtime loose-file path and the editor import factory (one parser, `VehicleDamage.h:653-665`). A compiled binary asset path (`LoadCageFromAsset`, `VehicleDamage.cpp:528`) copies the same arrays in without re-parsing — for the port, treat the text parser as the reference and define your own binary cache format if desired.

---

## 3. Core data structures

### 3.1 Material (`FVehicleMaterial`, `VehicleDamage.h:57-71`) — authored, referenced by name

| Field | Type | Units | Default | Meaning |
|---|---|---|---|---|
| Name | string | — | — | lookup key |
| Stiffness | float | 0..1 | 0.7 | PBD relaxation feel (constraint stiffness) |
| AxialStiffness | float | N/cm | 3500 | elastic axial spring rate; force = Axial × stretch |
| BendForceN | float | N | 3000 | yield force where **permanent** bending begins |
| BreakForceN | float | N | 12000 | instant catastrophic-snap force |
| PlasticStiffness | float | N/cm | 800 | resistance per cm of extra bend after yield |
| MaxBend | float | cm | 8 | accumulated plastic bend before the beam tears |
| BendAbsorb | float | 0..1 | 0.3 | energy soaked per cm of bend |
| BreakAbsorb | float | 0..1 | 0.6 | energy soaked on snap (rest recoils) |
| Behavior | enum | — | Deform | see below |

`EDamageBehavior` (`VehicleDamage.h:47-52`): `Deform` (ductile steel: bends, detaches whole, never shatters), `Fracture` (carbon: whole-detach at detach impulse, shatters only above higher shatter impulse), `Shatter` (glass: always shatters).

### 3.2 Node (`FVehicleNode`, `VehicleDamage.h:74-94`)

| Field | Type | Units | Persisted? | Meaning |
|---|---|---|---|---|
| NodeID | int32 | — | yes | stable id (parser key) |
| Position | Vec3 | cm | yes (authored) | live local-space position |
| PrevPosition | Vec3 | cm | transient | Verlet previous; velocity = Position − PrevPosition |
| LocalRest | Vec3 | cm | transient | original rest pos (captured in InitSolver) |
| bPinned | bool | — | transient | fixed in car frame (base nodes) |
| IntactBeams | int32 | — | transient | unbroken beams still holding node |
| bFreed | bool | — | transient | all beams broken → falls as debris |
| TotalBeams | int32 | — | transient | beam degree at InitSolver (tear denominator) |
| bTorn | bool | — | transient | ≥ TearNodeFraction of beams broken → skin opens hole |
| ActiveTicks | int32 | — | transient | active-set awake countdown |
| bBrittle | bool | — | transient | majority beams brittle → cracks (no dent) on impact |
| InvMass | float | — | transient | inverse mass for weighted PBD (only read when `bUseNodeMass`) |

### 3.3 Beam (`FVehicleBeam`, `VehicleDamage.h:97-124`)

| Field | Type | Units | Meaning |
|---|---|---|---|
| BeamID, NodeA, NodeB | int32 | — | authored id + endpoint node **IDs** |
| Stiffness / AxialStiffness / BendForceN / BreakForceN / PlasticStiffness / MaxBend / BendAbsorb / BreakAbsorb / Behavior | — | — | resolved copy of owning panel's material (§3.1) |
| NodeAIdx, NodeBIdx | int32 | — | endpoint **array indices**, resolved once in InitSolver (hot-path cache; `INDEX_NONE` = −1 if unresolved) |
| RestLength | float | cm | rest length; **creeps** as the beam plastically bends |
| bBroken | bool | — | stops constraining once true |
| bRetired | bool | — | removed administratively (part left), not a stress failure |
| PeakForce | float | N | debug high-water mark |
| PlasticAccum | float | cm | total permanent bend taken (ductile tear tracker) |

### 3.4 Panel / Part / Solver config

- **Panel** (`VehicleDamage.h:127-136`): `PanelID`, 3 beam IDs (`BeamA/B/C`), optional `MaterialOverride`. A panel = one triangle of 3 beams; it is the material-assignment unit.
- **Part** (`VehicleDamage.h:139-155`): `Name`, `Role`, `MaterialName`, `MeshName`, `PanelIDs[]`, `DetachBrokenFraction` (default **0.3** — fraction of the part's cage beams that must break before the whole panel sheds).
- **Solver tunables** (`FVehicleSolver`, `VehicleDamage.h:236-247`): `Substeps=4`, `SolverIterations=8`, `VelocityDamping=0.99`, `MaxNodeSpeed=1500 cm/s`, `PinHeight=20 cm`, `bEnableDamage=true`, `DamageRate=8 /s`.

### 3.5 POD solve config (`FOCSolveConfig`, `VehicleDamage.h:294-321`)

The **thread-agnostic parameter bundle** copied per-step so the same `SolveStep` runs on the game thread or the worker with no member access. Built by `BuildSolveConfig` (`VehicleDamage.cpp:2397-2424`). Fields mirror `FVehicleSolver` plus: `bActiveSet`, `ActiveHoldTicks=12`, `ActiveWakeMoveCm=0.02`, `SettleThresholdCm=0.05`, `TearNodeFraction=0.6`, `BreakKick=2`, `LocalGravity` (car-local down × DebrisGravity, or 0), `Dt`, `DamageTimer`, and the opt-in realism flags `bUseNodeMass`, `bBeamDashpot`+`BeamDamping`, `bWorkHardening`+`WorkHardenCoeff`.

`FOCSolveResult` (`VehicleDamage.h:324-331`): `bAnyBreak`, `bRecounted`, `TornCount`, `bSettled` — outputs the owning thread acts on.

---

## 4. Initialization pipeline (`InitSolver`, `VehicleDamage.cpp:553-671`)

Runs once when the solver first ticks. Order:

1. Build `NodeIndexByID` map (ID → array index).
2. Compute `MinZ` over all nodes.
3. Per node: `LocalRest = Position`, `PrevPosition = Position`, `bPinned = (Position.Z <= MinZ + PinHeight)` — **the bottom `PinHeight` cm of the cage is anchored to the chassis frame** (`VehicleDamage.cpp:566-577`).
4. Per beam: resolve `NodeAIdx/NodeBIdx`; unresolved → mark `bBroken=true`. `RestLength = Dist(A,B)`. Reset `bBroken/PeakForce/PlasticAccum`. **Apply `StructuralStrength`** multiplicatively to `AxialStiffness, BendForceN, BreakForceN, PlasticStiffness` (`VehicleDamage.cpp:601-604`).
5. `TotalBeams` per node counted from unbroken beams (tear denominator; frozen at rest structure).
6. **Brittle classification**: per node, vote +1 for each Fracture/Shatter beam, −1 for Deform; `bBrittle = votes > 0` (`VehicleDamage.cpp:620-630`).
7. **Inverse-mass** derivation (only consumed when `bUseNodeMass`): per-node mean beam `AxialStiffness`, normalized so the mean node = 1, clamped to `[1/K, K]` where `K = sqrt(NodeMassRange)`; `InvMass = 1/Mass` (`VehicleDamage.cpp:637-657`).
8. `BuildPartBeamMap` (PART name → its cage beam indices).

`RestPoseLength(beam)` = `Dist(LocalRest_A, LocalRest_B)` — the un-dented reference length (`VehicleDamage.cpp:2031`).

---

## 5. Integration scheme

**Position-Based Dynamics (PBD) with Verlet integration**, fixed substeps, Gauss-Seidel constraint relaxation. Base solve is **massless** (uniform weights); mass/damping/hardening are opt-in realism terms that keep the base path byte-identical when off (`VehicleDamage.h:522-560`).

Per full step (`SolveStep`, `VehicleDamage.cpp:2171-2393`):
- `NumSub = max(1, Substeps)` (default 4); `SubDt = Dt / NumSub`.
- `NumIters = max(1, SolverIterations)` (default 8).
- Each substep runs **three phases in order**: (1) Verlet integrate → (2) damage/plasticity → (3) `NumIters` Gauss-Seidel relaxation sweeps.

`Dt` source: sync path uses frame `DeltaTime` (`VehicleDamage.cpp:2612`); async path uses fixed `1/Hz` (default 64 Hz) (`VehicleDamage.cpp:2481, 273`).

### Phase 1 — Verlet integration (`VehicleDamage.cpp:2230-2272`)

```
MaxStep = MaxNodeSpeed * SubDt      // per-substep speed clamp (cm)
for each node N:
    if N.bPinned:            N.Position = N.LocalRest; N.PrevPosition = N.Position; continue
    if (!windowOpen && !N.bFreed):  N.PrevPosition = N.Position; continue   // rigid-ride freeze guard
    if (activeSet && N.ActiveTicks<=0 && !N.bFreed): N.PrevPosition = N.Position; continue
    Vel = (N.Position - N.PrevPosition) * VelocityDamping
    if |Vel|^2 > MaxStep^2:  Vel = normalize(Vel) * MaxStep
    N.PrevPosition = N.Position
    N.Position    += Vel
    if activeSet && |Vel|^2 > WakeMoveSq: N.ActiveTicks = HoldTicks
    if N.bFreed:  N.Position += LocalGravity * (SubDt*SubDt)   // debris free-fall
```

Key behaviors: gravity is a Verlet acceleration term applied only to freed nodes. The freeze guard means an **undamaged / window-closed** cage does zero deformation work (rides rigidly). `windowOpen = DamageTimer > 0`.

### Phase 3 — Gauss-Seidel distance constraint (`VehicleDamage.cpp:2350-2382`)

```
for it in 0..NumIters:
  for each unbroken beam B (endpoints resolved):
    if activeSet && both ends asleep && neither freed: continue
    Delta = NB.Pos - NA.Pos;  Len = |Delta|;  if Len < 1e-4: continue
    Diff = (Len - RestLength) / Len
    Corr = Delta * (0.5 * Stiffness * Diff)
    wA = pinnedA ? 0 : (useNodeMass ? InvMass_A : 1)
    wB = pinnedB ? 0 : (useNodeMass ? InvMass_B : 1)
    if wA+wB < 1e-4: continue
    NA.Pos += Corr * (2*wA/(wA+wB))
    NB.Pos -= Corr * (2*wB/(wA+wB))
```

This is the classic symmetric PBD distance projection; `Stiffness` (0..1, per material) scales convergence. `MaxMoveSq` accumulates the largest per-substep motion for settle detection.

### Settle detection (`VehicleDamage.cpp:2391-2392`)

`bSettled = (DamageTimer <= 0) && (MaxMoveSq <= SettleThresholdCm^2)`. When settled, the caller freezes the whole solver until the next impact (`VehicleDamage.cpp:2583-2586`) — the dominant idle-time saving.

---

## 6. Plasticity + fracture/break handling (Phase 2, `VehicleDamage.cpp:2274-2347`)

Runs **only** when `bEnableDamage && DamageTimer > 0` (the post-impact window). Per unbroken beam:

```
Delta = NB.Pos - NA.Pos;  Len = |Delta|;  Axis = Delta/Len
Stretch = Len - RestLength                 // signed cm
Mag     = |AxialStiffness * Stretch|       // N
PeakForce = max(PeakForce, Mag)
bBrittle = (Behavior == Fracture || Shatter)

// optional work-hardening: raise ductile yield with accumulated bend
BendYield = bWorkHardening ? BendForceN + WorkHardenCoeff*PlasticStiffness*PlasticAccum
                           : BendForceN

// (A) catastrophic / brittle snap
if (Mag > BreakForceN) || (bBrittle && Mag > BendForceN):
    BreakBeam(B, NA, NB, Axis, BreakAbsorb);  continue

// (B) ductile plastic yield (Deform beams only)
if (!bBrittle && Mag > BendYield):
    OverForce = Mag - BendYield
    TargetGive = OverForce / max(PlasticStiffness, 1)
    dPlastic  = TargetGive * clamp(DamageRate * SubDt, 0, 1)   // rate-limited creep
    RestLength   += sign(Stretch) * dPlastic                   // permanent shortening/lengthening
    PlasticAccum += dPlastic
    BleedNodeVelocity(NA, clamp(BendAbsorb*dPlastic,0,1))      // absorb energy
    BleedNodeVelocity(NB, clamp(BendAbsorb*dPlastic,0,1))
    if PlasticAccum > MaxBend:  BreakBeam(B, NA, NB, Axis, BreakAbsorb); continue

// (C) optional strain-rate dashpot (surviving beam)
if bBeamDashpot && BeamDamping>0:
    vRel = dot((VB - VA), Axis)                 // relative axial velocity (Verlet)
    dP   = Axis * (0.5 * min(BeamDamping,1) * vRel)
    NA.PrevPosition -= dP;  NB.PrevPosition += dP   // moves PrevPosition only; force measure untouched
```

**Break model (`BreakBeam` lambda, `VehicleDamage.cpp:2183-2194`; game-thread twin `BreakBeamWithEnergy`, `VehicleDamage.cpp:1342-1360`):**
```
B.bBroken = true
Released = clamp(1 - Absorb, 0, 1)
Kick     = Released * BreakKick            // cm of recoil
NA.PrevPosition += Axis * Kick             // recoil loads neighbours next substep → emergent cascade
NB.PrevPosition -= Axis * Kick
NA.ActiveTicks = NB.ActiveTicks = HoldTicks
// sets Out.bAnyBreak + connectivity-dirty (worker) / MarkDamageChanged (game thread)
```

**`BleedNodeVelocity`** (`VehicleDamage.cpp:1336-1340`): `PrevPosition = Lerp(PrevPosition, Position, clamp(Frac,0,1))` — shrinks the Verlet velocity gap = kinetic energy absorbed. Pinned nodes skip.

**Connectivity recount** (gated by `bConnectivityDirty`, once when broken-set changes; `VehicleDamage.cpp:2196-2220`):
- `IntactBeams` per node recounted; `bFreed = (IntactBeams==0) && !bPinned`.
- `bTorn = !bPinned && TotalBeams>0 && (TotalBeams-IntactBeams) >= ceil(clamp(TearNodeFraction,0.05,1) * TotalBeams)`.
- `TornCount` reported; the caller bumps `TearGeneration` only when it grows (`VehicleDamage.cpp:2617`).

**Semantics summary:**
- Deform (steel): plastic creep of `RestLength` → permanent dent; tears when `PlasticAccum > MaxBend`.
- Fracture (carbon): does **not** dent (brittle nodes skip injection); snaps directly when `Mag > BendForceN`; whole-part detach on hard impact.
- Shatter (glass): always brittle; same snap path.

---

## 7. Impact injection & collision handling

The soft-body solver performs **no self-collision and no world-collision internally**. Collision enters only at boundaries:

### 7.1 Two entry points

**`OnHit`** — engine rigid-body collision callback (`VehicleDamage.cpp:2633-2874`). Energy proxy = `NormalImpulse` magnitude.
**`ReportImpact`** — replay / scripted / multiplayer entry (`VehicleDamage.cpp:1238-1332`). Energy proxy = closing **speed** (km/h).

Both: open the damage window (`DamageTimer = DamageWindowDuration`), wake the solver, map the impact into cage-local space, apply the Y-mirror (gated by `bDeformMirrorY`), compute an injected displacement `Disp`, then push nodes within `ImpactRadius`.

### 7.2 Gate & displacement math

- `OnHit` gate: ignore if `NormalImpulse.Size() < MinImpactImpulse` (default 200) (`VehicleDamage.cpp:2663`).
- `ReportImpact` gate: ignore if `SpeedKmh < 5` (`VehicleDamage.cpp:1243`).
- Raw depth: `OnHit` → `Raw = Mag * ImpactScale` (0.01); `ReportImpact` → `Raw = SpeedKmh*27.78 * ImpactSpeedScale` (0.01) (`VehicleDamage.cpp:2724, 1264-1266`).
- **Soft-knee crush curve** (`OCCrushCurve`, `VehicleDamage.cpp:70-75`):
  ```
  Ceil = MaxImpactStep * max(1, CrushCeilingMult)      // default 50*1.8 = 90 cm
  if Raw<=Knee || Ceil<=Knee:  Disp = min(Raw, Knee)   // classic hard clamp (bNonlinearCrush off)
  else: Disp = Knee + (Ceil-Knee)*(1 - exp(-(Raw-Knee)/(Ceil-Knee)))   // C1-continuous, ≤ Ceil
  ```
  where `Knee = MaxImpactStep` (default 50).

### 7.3 Node push (dent) (`VehicleDamage.cpp:2749-2759`, worker twin `VehicleSolverAsync.cpp:162-171`)

```
for each node within ImpactRadius R (skip pinned):
    N.ActiveTicks = max(1, ActiveSetHoldTicks)   // wake impact cone
    if N.bBrittle: continue                       // carbon cracks, doesn't dent
    Falloff = 1 - sqrt(D2)/R                       // linear radial falloff
    N.PrevPosition -= LocalDir * (Disp * Falloff)  // seed velocity toward the hit
```
The push writes `PrevPosition`, so the resulting Verlet velocity is **rate-limited by `MaxNodeSpeed`** over the window substeps — a deep `Disp` is spread over frames, never a teleport (`VehicleDamage.cpp:2718-2721`).

### 7.4 Impact-driven tearing (the carbon "hole", `VehicleDamage.cpp:2146-2163`)

Smooth dent injection never stretches beams past break (neighbours move together), so a hard hit breaks brittle beams directly: `BreakBrittleBeamsNear(nodes, beams, LocalPt, TearRadius)` breaks every unbroken Fracture/Shatter beam whose **midpoint** is within `TearRadius`. `TearRadius = OCComputeImpactTearRadius(Disp, R, MaxStep)` (`VehicleDamage.cpp:53-62`): 0 if disabled or `Disp < MinDisp` (cvar 2.0); else `R * clamp(RadiusFrac,0,1) * clamp(Disp/MaxStep, 0.3, 1)`.

### 7.5 Chassis rebound (restitution) (`VehicleDamage.cpp:2671-2695`)

Applied to the **rigid chassis body** (not the cage): with inbound normal speed `Vn = dot(V,N) < 0`, set `V := V - N*(Vn*(1+e))`, `e = clamp(ReboundCoefficient,0,1)` (default 0.25, loaded from `.ocbeam REBOUND`). This is the whole-car bounce; tangential velocity untouched. Supersedes the older `CrashEnergyAbsorb`.

### 7.6 Debris "collision"

Freed cage nodes simply fall under `LocalGravity` in Phase 1 — there is **no ground contact** in the solver. The `DebrisMaxDrop` (200 cm) and `MaxBoneDisplacement` (60 cm) values are **cosmetic clamps applied at the mesh/bone-drive stage**, not in `SolveStep` (`VehicleDamage.h:435-438, 577-580`). Real rigid-body collision exists only for **detached debris actors** (§11), which the host physics engine simulates.

---

## 8. Whole-part detach & fracture

Parts are registered on first tick (`RegisterParts`, `VehicleDamage.cpp:1696-1812`). Each `FPartRuntime` (`VehicleDamage.h:964-982`) records the part's node set, **interior beams** (both endpoints inside) vs **boundary beams** (exactly one inside = weld seam to body), a `BoundaryBreakTarget = max(1, ceil(BoundaryBeamIdx.Num() * BoundaryFraction))`, centroid, and resolved tunables.

Two detach mechanisms:
1. **Deformable (skeletal) parts** — seam-driven. `UpdateDetachableParts` (`VehicleDamage.cpp:1814-1840`) counts broken boundary beams each tick; when `>= BoundaryBreakTarget` → `DetachDeformablePart` (`VehicleDamage.cpp:1912-1954`): mark detached, stop cage-driving the mesh, `RetirePartBeams`, hand mesh to physics with an outward kick `LocalCentroid.normalized × DetachImpulse` + car velocity.
2. **Fracturable (geometry-collection) parts** — impact-driven. In `OnHit`, the single **nearest** fracturable part whose `NormalImpulse ≥ FractureImpulse` and within `ImpactRadius` is deferred to next tick (physics-callback safety) → `FracturePart` (`VehicleDamage.cpp:1963-2016`): detach as dynamic debris, kick, optionally `CrumbleActiveClusters`. The **largest** fracturable part (most nodes = the monocoque) is auto-protected from whole detach (`VehicleDamage.cpp:2781-2795`).

Also: **accumulated panel shed** — when `DetachBrokenFraction` of a PART's cage beams break, the panel is queued in `PendingDetachParts` for the body skin to shed (`VehicleDamage.cpp:2588-2609`, drained via `DrainPartsToDetach`).

`RetirePartBeams` (`VehicleDamage.cpp:1842-1870`): sets `bBroken=bRetired=true` on interior+boundary beams, marks connectivity dirty, wakes all nodes, and (if an async episode is live) enqueues a `RetireBeams` command to the worker's private copy.

---

## 9. Repair (`RepairVehicle`, `VehicleDamage.cpp:1564-1621`)

Full reset to factory: abort any async episode; per node `Position = PrevPosition = LocalRest`, clear `bFreed/bTorn`, `IntactBeams = TotalBeams`; per beam `bBroken=bRetired=false`, `PeakForce=PlasticAccum=0`, `RestLength = Dist(LocalRest_A, LocalRest_B)` (undoes plastic shortening). Clears damage bookkeeping, re-inerts fracturable parts, bumps `RepairCounter`/`TearGeneration`, broadcasts `OnRepaired`.

---

## 10. Threading model (async solver)

`FVehicleSolverWorker : FRunnable` (`VehicleSolverAsync.h/.cpp`). Off-thread, fixed-rate, private-copy design.

**Architecture:**
- Worker owns a **private copy** of the cage (`WNodes`/`WBeams`); never touches the component's live arrays (`VehicleSolverAsync.h:110-111`).
- One dedicated thread at **below-normal priority** (`VehicleSolverAsync.cpp:37`) — must never starve game/render threads.
- All cross-thread state under **one `FCriticalSection`**, held only for O(1) swaps / small POD copies / a command-list move (`VehicleSolverAsync.h:22-24`). The 60 KB snapshot fills happen **outside** the lock into scratch, then pointer-swapped in.
- Same math both paths: worker calls the static `UVehicleDamage::SolveStep` (`VehicleSolverAsync.cpp:288`).

**Fixed-rate loop** (`Run`, `VehicleSolverAsync.cpp:226-312`):
- Rate = `1/ResolveSolverHz()` (default 64 Hz, or server `NetSendHz` in MP; clamped 15..240) (`VehicleDamage.cpp:2438-2453`).
- Time-accumulator with `MaxStepsPerWake = 4` **spiral-of-death clamp**; leftover accumulation capped at `Step*4`.
- Wakes on an `FEvent` roughly each step while active, else idles ~50 ms; woken by `Start`/`EnqueueImpact`.
- Each step: drain commands → decrement window → `SolveStep` → publish snapshot; break early on settle.

**Data flow:**
- `SeedFromCage` stages a seed under lock; the **worker** swaps it into its private arrays on its own thread at the next loop top (avoids a seed data race) (`VehicleSolverAsync.cpp:47-55, 250-256`).
- Game→worker commands (`FAsyncSolverCmd`, `VehicleSolverAsync.h:44-57`): `Impact` (LocalPt/Dir/Disp/Radius/Window/TearRadius) and `RetireBeams` (index list).
- Worker→game: `FDeformSnapshot` (`VehicleSolverAsync.h:31-41`): per-node `Disp` (`FVector3f = Position − LocalRest`), per-node `Torn` (uint8), per-beam `BrokenBits` (bit-packed, byte `i>>3`, bit `i&7`), `DamageTimerRemaining`, `bSettled`, `Serial`. Published via `Swap` under lock (`VehicleSolverAsync.cpp:194-224`).
- Game thread per tick (`PumpAsyncEpisode`, `VehicleDamage.cpp:2490-2531`): push fresh config, `TryGetSnapshot` (O(1) swap), **mirror** `Disp/Torn/BrokenBits` onto live `Nodes/Beams`, never blocks. On settle handshake (`ConsumeSettled`), `CopyOutState` reclaims the worker's **full** state (RestLength/PlasticAccum/PrevPosition — the per-frame snapshot omits these).

**Episode lifecycle:** `StartAsyncEpisode` (`VehicleDamage.cpp:2455-2485`) seeds + starts on first impact (idempotent; folds in any prior settled dent for cumulative damage). Snapshot mirroring makes every existing reader (deform/IK/draw/net) race-free since the worker only ever mutates its private copy. `EndPlay`/dtor join the thread before freeing.

**Active-set optimization** (single-threaded, `bActiveSetSolver`): only nodes with `ActiveTicks>0` (or freed) integrate/relax; impacts and breaks wake the affected region, which grows/shrinks with the dent. `HoldTicks=12`, `WakeMoveCm=0.02`. Composes with the async path.

---

## 11. Runtime debris (`OCDebrisChunk.cpp`)

`AOCDebrisChunk` — a procedural-mesh actor spawned when a panel/part sheds. Relevant algorithms for the port (engine-agnostic parts):

- **Shard splitting** (`OCDebris::SplitIntoClusters`, `OCDebrisChunk.cpp:19-92`): farthest-point seeding (first seed random) + nearest-seed triangle assignment → K jagged clusters; each cluster re-indexed into an independent mesh chunk. `K` clamped `[2, min(TriCount/2, 48)]`.
- **Mass** = `clamp(triangleArea_cm² × 1e-4 × ArealDensityKgM2, 0.3, 80)` kg (`OCDebrisChunk.cpp:169-174`).
- **Launch**: outward from impact origin toward chunk centroid + up + random jitter, speed mass-scaled (`OCDebrisChunk.cpp:224-237`).
- **Secondary shatter**: shatterable if `Generation < MaxShatterGenerations && area ≥ MinShatterAreaCm2 && tris ≥ 12`; on a rigid hit above `SecondaryShatterImpulse`, deferred re-split next tick (`OCDebrisChunk.cpp:213-262`). Live-count cap `MaxLiveDebris`.

Everything else in this file (physics sim flags, Niagara FX, convex cook) is engine glue (§14).

---

## 12. Fracture library — editor-only (not needed at runtime)

`OC27FractureLibrary.cpp` is **entirely `#if WITH_EDITOR`** (`OC27FractureLibrary.cpp:5-21, 153-263`). It Voronoi-fractures `UGeometryCollection` assets **at author time** (scatter sites → `FVoronoiDiagram`/`FPlanarCells` → `CutMultipleWithPlanarCells`). The runtime solver never calls it. For the port: replace the authored geometry-collection shards with your own pre-fractured meshes (or the runtime clustering split in §11). No runtime algorithm to port here — only the concept that fracturable parts ship with pre-diced geometry.

---

## 13. Record cross-references (index invariants)

- Node `NodeID` → array index via `NodeIndexByID`; beams cache resolved `NodeAIdx/NodeBIdx` **once** in InitSolver (never re-hash on the hot path).
- Beam/Panel keyed by ID during parse resolution only; runtime uses array indices.
- Panel → 3 beam IDs; Part → panel IDs → (via panels) beams → (via beams) nodes. `PartBeamMap` and `FPartRuntime.NodeIdx/InteriorBeamIdx/BoundaryBeamIdx` are the runtime derived cross-refs.
- Snapshot arrays are strictly **index-parallel** to `Nodes[]`/`Beams[]`; `BrokenBits` packs beam `i` at byte `i>>3` bit `i&7`.
- Net serialization seams (for MP determinism): plastic state = `(beamIdx[], plasticCm[])`; broken set = beam bitset LSB-first; detached parts = part indices (`VehicleDamage.h:714-721`).

---

## 14. UE dependencies to strip/replace (prioritized)

### P0 — Core math & containers (touch every line of the solver)

| UE type | Where | Permissively-licensed replacement |
|---|---|---|
| `FVector` (double in UE5) | all node/beam math | `oc::Vec3` (`Vec3.h`) — **already exists**; add `Dist`, `DistSquared`, `GetSafeNormal`, `Size`/`SizeSquared` (currently only `Dot/Cross/Length/LengthSq/Normalized`) |
| `FVector3f` | snapshot `Disp` | keep `oc::Vec3` (already float) |
| `FVector2D` | UVs (mesh only) | small `Vec2` struct |
| `FTransform` + `InverseTransformPosition/Vector`, `TransformVectorNoScale` | `GetCageTransform`, impact mapping, gravity | POD transform (position + quaternion [+ scale]); implement inverse-transform. This is the **only** world↔local coupling in the physics core |
| `FQuat`, `FRotator` | debris launch rotation | quaternion/euler structs |
| `FMath::{Clamp,Max,Min,Abs,Sqrt,Exp,Lerp,CeilToInt,RoundToInt,Sign}` | throughout | `std::` equivalents / trivial inlines |
| `KINDA_SMALL_NUMBER` (1e-4), `INDEX_NONE` (−1), `MAX_flt`, `TNumericLimits<float>::Max()` | guards | `constexpr` constants, `std::numeric_limits` |
| `TArray<T>` | Nodes/Beams/Panels/Parts/snapshots | `std::vector<T>` |
| `TMap<K,V>` | `NodeIndexByID`, `PartBeamMap`, `BeamIndexByID` | `std::unordered_map` |
| `TSet<T>` | node sets, `PartsDetachReported` | `std::unordered_set` |
| `FString` | names, parser | `std::string` |
| `FName` | part/socket keys | interned id / `std::string` / hashed `uint32` |

### P1 — Threading (async solver)

| UE type | Where | Replacement |
|---|---|---|
| `FRunnable` / `FRunnableThread::Create` (`TPri_BelowNormal`) | worker thread | `std::thread` + a priority hint (`SetThreadPriority` / `pthread_setschedparam`) |
| `FEvent` + `WakeEvent->Wait(ms)/Trigger()` | wake/idle | `std::condition_variable` + `std::mutex` (timed `wait_for`) |
| `FCriticalSection` / `FScopeLock` | the one lock | `std::mutex` / `std::lock_guard` / `std::unique_lock` |
| `FThreadSafeBool` | `bStopRequested` | `std::atomic<bool>` |
| `FThreadSafeCounter` | anchor cache index | `std::atomic<int32_t>` |
| `FPlatformTime::Seconds()` | accumulator clock | `std::chrono::steady_clock` |
| `FPlatformProcess::Get/ReturnSynchEventFromPool` | event pool | own the `condition_variable` directly |
| `Swap(a,b)` | snapshot swap | `std::swap` |
| `MoveTemp` | moves | `std::move` |

### P2 — Object model / reflection (delete; no runtime behavior)

| UE construct | Replacement |
|---|---|
| `UObject`, `UActorComponent`, `UCLASS`, `USTRUCT`, `GENERATED_BODY`, `UPROPERTY`, `UFUNCTION`, `UENUM`, `UMETA`, `BlueprintCallable`/`EditAnywhere` | plain `struct`/`class`/`enum class`; drop all reflection macros. Tunables become plain members loaded from config |
| `TObjectPtr<T>` / `TWeakObjectPtr<T>` / `TSoftObjectPtr<T>` | raw pointers / `std::weak_ptr` / asset handle |
| `TUniquePtr<T>` | `std::unique_ptr` |
| `DECLARE_DYNAMIC_MULTICAST_DELEGATE*` (`OnPartDetached`, `OnRepaired`, `OnImpactQualified`) | callback list / `std::function` / small event bus |
| `UE_LOG` / `LogTemp` | your logging macro (or strip) |
| `TAutoConsoleVariable` (`oc.Damage.ImpactTear*`) | config constants: `ImpactTear=1`, `ImpactTearMinDisp=2.0`, `ImpactTearRadiusFrac=0.5` (`VehicleDamage.cpp:38-49`) |

### P3 — Engine subsystems (physics/render/collision — reinterface, not port)

| UE type | Where | Replacement |
|---|---|---|
| `UPrimitiveComponent`, `FHitResult`, `OnComponentHit`, `SetNotifyRigidBodyCollision` | `OnHit` collision callback | feed impacts from **your** collision layer into an `OnHit(point, normalImpulse, normal)` / `ReportImpact(point, normal, speed)` shim (both already exist as clean seams) |
| `GetPhysicsLinearVelocity` / `SetPhysicsLinearVelocity` | rebound | your rigid-body API |
| `USkeletalMeshComponent`, `UStaticMeshComponent`, `UProceduralMeshComponent`, `UGeometryCollectionComponent`, Chaos (`EObjectStateTypeEnum`, `CrumbleActiveClusters`, `RecreatePhysicsState`) | mesh deform + debris | your renderer / rigid-body / mesh-cut system; **out of the physics-core scope** |
| `FRandomStream` | debris scatter | `std::mt19937` + `std::uniform_real_distribution` |
| `DrawDebug*`, `GEngine->AddOnScreenDebugMessage`, `EKeys::F3` | debug HUD | strip or your debug overlay |
| Niagara (`UNiagaraFunctionLibrary`, `UNiagaraSystem`) | dust/spark FX | your particle system (cosmetic) |
| `GetTimerManager().SetTimerForNextTick` | defer detach out of hit callback | a deferred-command queue drained at frame start (keep this pattern — running detach inside a physics callback is unsafe) |
| `WITH_EDITOR` fracture bake (`OC27FractureLibrary`, PlanarCut, Voronoi) | author-time only | omit at runtime; pre-author shards (§12) |

### Porting priority order (drives the physics module)

1. `Vec3` (+ Dist/DistSquared/GetSafeNormal/Size) and a `Transform` type. **P0 blocker.**
2. `FVehicleMaterial/Node/Beam/Panel/Part` + `FOCSolveConfig`/`FOCSolveResult` as plain structs.
3. `ParseOcbeam` → `std::string`/`std::vector` (§2).
4. `InitSolver` (§4) and `SolveStep` (§5–6) — the algorithmic heart; keep the exact phase order and formulas.
5. `OnHit`/`ReportImpact` impact seeding + `OCCrushCurve`/tear-radius helpers (§7), behind a UE-free collision shim.
6. Part registration/detach (§8) and repair (§9).
7. Async worker (§10) with `std::thread`/`std::mutex`/`std::condition_variable`/`std::atomic`.
8. Debris split/launch (§11) if debris is in scope; else defer to render/physics glue.

### Known ambiguities / things to verify against runtime

- `DebrisMaxDrop` / `MaxBoneDisplacement` are **not** referenced inside `SolveStep`; they are described in the header as bone/vertex-drive clamps (`VehicleDamage.h:435-438, 577-580`). Confirm your mesh-drive layer applies them; the physics core leaves freed nodes unbounded except by gravity + `MaxNodeSpeed`.
- `CrashEnergyAbsorb` (`VehicleDamage.h:461`) is documented as **superseded** by `ReboundCoefficient` for the chassis bounce; `OnHit` uses only the rebound path (`VehicleDamage.cpp:2671-2695`). Treat `CrashEnergyAbsorb` as legacy/unused in the solver.
- The `id(` prefix on PART lines is parsed but discarded (PARTs have no numeric ID field); only Name/Role/Material/panels are used.
- Determinism for MP relies on identical `Dt` and iteration order; the async path uses a fixed `1/Hz` step and the same static `SolveStep`, so a faithful port must preserve float evaluation order in Phases 1–3.

---

## 15. As built (`modules/softbody`)

The solver in §4–§7 and §10 is implemented in `modules/softbody`; docs/SOFTBODY.md is the usage guide.
Where the implementation differs from this spec, deliberately:

| Spec | As built | Why |
|---|---|---|
| Pinned nodes snap to `LocalRest` every substep (§5 Phase 1) | Pinned nodes are **kinematic**: the solver leaves `pos` alone and zeroes velocity; the host moves them | A cage can be driven by a chassis, a hand, a test rig; snapping to rest made the pin a fixture |
| Damage runs only while a post-impact window is open; Verlet is frozen outside it (§5, §6) | Damage runs every step; idle cost is covered by `settled` | The window is a rigid-ride optimisation for a cage attached to a chassis; a general solver does not know when it is being driven |
| Break = force or accumulated set | Also `Material::breakStrain`, a stretch limit on the **original** length | A tear limit must not creep away as the rest length yields; this is what a cloth wants |
| Tear = a beam stops constraining; `bTorn`/`TearNodeFraction` mark nodes where the skin opens (§6) | A line of broken triangle edges **duplicates** particles (`resolveTears`), logs `SplitEvent`s, and marks triangles with 2+ broken edges dead; `RenderBinding` replays the splits | The spec only hides geometry; a real slit needs the lips to separate |
| Gravity on freed nodes only | Same, plus `StepConfig::gravityAll` | Cloth and flags are not riding a chassis |
| `RestLength` unbounded | Clamped to at least 10% of the as-authored length | A creeping beam must not collapse to zero length |
| Snapshot is per-node `Disp` + bit-packed broken flags (§10) | Snapshot carries all particle positions (the count grows with tears), dead-triangle flags and the split events since last taken | Tears change the particle count, so a fixed index-parallel displacement array is not enough |
| Async worker idles ~50 ms (§10) | Idles on a condition variable until a command arrives; paced mode waits only the remainder of the step | Same behaviour without the polling latency |
| `.ocbeam` parser, parts, whole-panel detach, repair of fracturable parts, debris (§2, §8, §11) | **Not implemented** | Out of this task's scope; the data model carries the hooks (`origin`, `freed`, per-beam material) |
| Node mass, strain-rate dashpot, work hardening (opt-in realism) | `useNodeMass` and `Material::hardening` only; no dashpot | Base solve stays massless and byte-stable |
