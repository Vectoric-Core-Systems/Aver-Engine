I now have everything needed from the writer, the runtime solver, the asset payload types, the CLAUDE.md cross-check, and the sample file. Here is the complete specification.

---

# `.ocaero` Format & Runtime Aerodynamics Solver — Implementation Spec

## 0. Source-of-truth map

| Concern | File | Key lines |
|---|---|---|
| Writer (wind-tunnel bake) | `OCCompiler/Main.java` | `AeroSim` 2833–3118; `AeroSim.Result` 2930; `AeroTunnelPanel` 3420–3547; bake dialog + writer 3824–3929 |
| Runtime parser (shared) | `VehicleAerodynamics.cpp` | `ParseOcaero` 238–303; `ParseFloats` 229–233 |
| Runtime solver | `VehicleAerodynamics.cpp` | `ApplyBakedAero` 416–473; `SampleBaked` 338–380; ride/GE 384–414; `TickComponent` (Surfaces path) 150–224 |
| Runtime types / API | `VehicleAerodynamics.h` | `FVehicleAeroSurface` 18–53; component 55–188 |
| Baked payload structs | `VehicleCageAsset.h` | `FVehicleAeroSample` 44–51; `FVehicleAeroPart` 67–74; asset axes 209–220 |
| Prose cross-check | `OpenConstructor27/CLAUDE.md` | `.ocaero format` 362–374; tunnel 354–360 |
| Sample | `Content/VehicleData/Ferrari499P.ocaero` | header 1–9; parts 10+ |

> Note a doc drift: `CLAUDE.md` and the bake popup (`Main.java:3918`) tell users to import as a "Vehicle Aero asset / AeroAsset", but the actual runtime source is (1) baked table inside the `UVehicleCageAsset`, (2) loose `.ocaero`, (3) hand-authored `Surfaces[]` — see §7.1. There is no separate "AeroAsset".

---

## 1. Coordinate system, units, conventions

- **Frame:** vehicle-local, **X = forward, Y = right, Z = up** (matches `.ocbeam`; `CLAUDE.md:364`).
- **Length:** centimetres in the file (nodes, CoP). The writer works internally in **metres** (nodes ×0.01 at `Main.java:2853`; CoP ×100 back to cm at `Main.java:3898`).
- **Force:** **Newtons**, vehicle-local vector `(Fx,Fy,Fz)`, evaluated **at `ReferenceSpeedKmh`**.
- **Angles:** degrees. **Ride height:** cm. **Air density:** kg/m³. **Speed:** km/h.
- **Sign conventions** (`Main.java:3899-3901`, sample `Ferrari499P.ocaero:20`):
  - Drag ⇒ `Fx < 0` (rearward, opposing +X travel); reported `drag = max(0,−Fx)`.
  - Downforce ⇒ `Fz < 0` (downward); reported `down = max(0,−Fz)`.
  - `Fy` = lateral (right +). CoP `(CoPx,CoPy,CoPz)` is the force application point, cm local.
- **Text format:** no endianness. UTF-8 (`Main.java:3906`). Line-oriented. **Delimiter = comma.** `#` begins a comment to end-of-line. Trailing `;` optional. Leading/trailing whitespace trimmed.

---

## 2. File grammar

### 2.1 Structure

```
OCAERO 1
HEADER{
  <key>, <values...>;      # one per line
  ...
}
PART{
  # optional comment lines
  <Name>, <yaw>, <pitch>, <ride>, <Fx>, <Fy>, <Fz>, <CoPx>, <CoPy>, <CoPz>;
  ...                      # one row per (part × yaw × pitch × ride)
}
```

Exact writer output order: `Main.java:3880-3905`.

### 2.2 Lexical rules (parser `ParseOcaero`, `VehicleAerodynamics.cpp:248-256`)

| Rule | Behaviour | Line |
|---|---|---|
| Comment strip | Everything from first `#` is discarded | 251 |
| Trim | `TrimStartAndEnd` after comment strip | 252 |
| Skip | Empty lines, and any line starting with `OCAERO` (magic/version line is **skipped, not validated**) | 253 |
| Trailing `;` | Removed (`LeftChop(1)` + `TrimEnd`) | 254 |
| Section open | A line containing `{` → `HEADER` if it `StartsWith("HEADER")`, `PART` if `StartsWith("PART")`, else `NONE` | 255 |
| Section close | A line exactly `}` → `NONE` | 256 |

- **Magic / version:** first line is `OCAERO 1` (`Main.java:3880`). The `1` is a version token but the reader only checks the `OCAERO` prefix and skips the line — **version is written but never parsed** (port should still validate it).
- **Number tokenizing in HEADER** (`ParseFloats`, `.cpp:229-233`): splits the whole line on `,`, keeps a token if `IsNumeric() || contains('.') || contains('-')`. The leading key token (e.g. `Yaw`) is dropped because it satisfies none of those. `FCString::Atof` parses each kept token (tolerates leading spaces).

### 2.3 HEADER fields

| Key | Type | Units | Meaning | Used at runtime? | Parse line |
|---|---|---|---|---|---|
| `ReferenceSpeedKmh` | float (1 value) | km/h | Speed at which baked forces hold; `Q=(v/ref)²` | **Yes** (`BakeRefSpeedKmh`) | `.cpp:260` |
| `AirDensity` | float (1 value) | kg/m³ | Bake-time ρ (default 1.225) | **No** — parsed into a discarded local; forces already embed ρ | `.cpp:261`, discarded `.cpp:314` |
| `SourceCage` | string | — | Provenance label (`cage`) | **No** — not matched by any `StartsWith`, silently ignored | — |
| `Yaw` | float list | deg | Yaw/sideslip axis (interpolation grid) | **Yes** | `.cpp:262` |
| `Pitch` | float list | deg (nose-up +) | Pitch axis | **Yes** | `.cpp:263` |
| `RideHeight` | float list | cm | Ground-clearance axis | **Yes** | `.cpp:264` |

Writer number formats: `ReferenceSpeedKmh` `%.1f`, `AirDensity` `%.3f`, axis lists via `fmt()` = integer if whole else float (`Main.java:3882-3887, 3931`).

**Axis ordering requirement:** the runtime `Bracket` (§6) does a linear ascending scan and assumes each axis is **monotonically increasing**. The writer emits axes in user-supplied order (defaults are ascending: `Main.java:3845-3847`). Non-ascending axes will mis-interpolate. (The *parser's* nearest-snap does not require sorting, but the *solver* does.)

### 2.4 PART rows

One row per `(part, yaw, pitch, ride)` combination. Row grammar (`.cpp:268-278`):

| # | Field | Type | Units | Meaning |
|---|---|---|---|---|
| 0 | `Name` | string | — | Part name; matches `.ocbeam` PART name for detach-skip (`VehicleCageAsset.h:66`). Writer sanitizes: commas→`_`, trimmed (`Main.java:3932`) |
| 1 | `yawDeg` | float | deg | Yaw sample |
| 2 | `pitchDeg` | float | deg | Pitch sample |
| 3 | `rideHeightCm` | float | cm | Ride sample *(present only in 10-field rows)* |
| 4–6 | `Fx,Fy,Fz` | float | N @ ref speed | Local force vector |
| 7–9 | `CoPx,CoPy,CoPz` | float | cm local | Centre of pressure |

- Field values written `%.1f` (`Main.java:3899`).
- Trailing comment ` # down %.0fN drag %.0fN` is **derived/informational only** (`Main.java:3899-3901`); ignored on read.
- **Row-length rule** (`.cpp:270-271`): `≥10` tokens ⇒ ride present; `<9` tokens ⇒ row skipped; `9` tokens ⇒ no-ride single-slice row (`Name,yaw,pitch,Fx,Fy,Fz,CoPx,CoPy,CoPz`). CLAUDE.md:373-374 confirms the 9-field variant.

---

## 3. In-memory model (parse target)

```
Axes:   YawAxisDeg[NY], PitchAxisDeg[NP], RideAxisCm[NR]     (.h:176-178)
Scalar: RefSpeedKmh                                            (.h:175)
Parts:  FVehicleAeroPart { FName PartName;                     (VehicleCageAsset.h:67-74)
                           FVehicleAeroSample Grid[NY*NP*NR] }
Sample: FVehicleAeroSample { FVector ForceLocalN;              (VehicleCageAsset.h:44-51)
                             FVector CoPLocalCm }
```

`NY=max(1,Yaw.Num)`, `NP=max(1,Pitch.Num)`, `NR=max(1,RideHeight.Num)` (`.cpp:280-282`).

**Grid flatten index (must replicate exactly):**
```
slot = (yi * NP + pi) * NR + ri          (.cpp:293, VehicleCageAsset.h:65)
```

**Parse-time placement** (`.cpp:284-294`): a part is created on first sighting with a zero-filled `Grid` of `NY*NP*NR`. Each row's `(yaw,pitch,ride)` is snapped to the **nearest axis index** via a linear min-|Δ| search (`Nearest`, `.cpp:287-292`) and written into `slot`. Cells never supplied stay zero (writer output is dense, so this only matters for hand-edited/sparse files).

**Success predicate** (`.cpp:298`): `Yaw.Num>0 && Pitch.Num>0 && Parts.Num>0`. (Ride axis may be empty ⇒ single slice, `ri=0`.)

---

## 4. Runtime data structures & load paths

The runtime grid types **are** the asset's serializable types (`.h:171-172`), so loading is a direct copy.

**Source priority at `BeginPlay`** (`.cpp:48-64`), gated by `bUseBakedAero`:
1. **Cage asset** `Damage->GetCageAsset()` → `LoadAeroFromCageAsset` (`.cpp:322-335`): copies `AeroYawDeg/AeroPitchDeg/AeroRideCm/AeroRefSpeedKmh/AeroParts`; requires `bHasAero && AeroParts.Num()>0` (`VehicleCageAsset.h:209-220`). Label `"asset"`. Preferred (cooks into packaged builds).
2. **Loose file** `AeroBakeFile` (relative to `Content/`) → `LoadAeroBake` (`.cpp:306-318`) via `ParseOcaero`. Label `"loose"`. Dev fallback — does **not** cook.
3. **Hand-authored `Surfaces[]`** if nothing baked loaded. Label `"surfaces"`. See §8.

---

## 5. Runtime attitude derivation (baked path)

Per tick, `ApplyBakedAero` (`.cpp:416-432`):

```
Vw       = Body world velocity (cm/s)                          (.cpp:418)
SpeedCmS = |Vw|;  SpeedKmh = SpeedCmS * 0.036                  (.cpp:419-420, CMS_TO_KMH .cpp:26)
if SpeedKmh < MinSpeedKmh (default 10) or SpeedCmS < 1 : return (.cpp:421)

WindLocal = Xf.InverseTransformVectorNoScale(-Vw / SpeedCmS)   (.cpp:426)  # oncoming-air travel dir, car frame
PitchDeg  = degrees( asin( clamp(WindLocal.Z, -1, 1) ) )       (.cpp:427)
YawDeg    = UnwindDegrees( degrees(atan2(WindLocal.Y, WindLocal.X)) - 180 )  (.cpp:428)
Q         = (SpeedKmh / max(1, BakeRefSpeedKmh))^2             (.cpp:430-431)
RideCm    = GetLiveRideHeightCm()                              (.cpp:432)
```

This inverts the writer's `flowDir` (`Main.java:2915-2928`), which at zero attitude points air travel along **−X**. Verified consistent: zero sideslip/zero pitch → `WindLocal=(−1,0,0)` → `Yaw=0, Pitch=0`; a `+Y` sideslip → `Yaw>0`; `WindLocal.Z>0` → `Pitch>0` (nose-up per `Main.java:2941`). **Port must reproduce both `flowDir` (bake) and this inverse (runtime) identically, including the `−180°`/`UnwindDegrees` fold**, or yaw sign flips.

**Live ride height** `GetLiveRideHeightCm` (`.cpp:384-400`): downward line trace (length `RideProbeMaxCm`, default 120 cm) from `RideProbeLocalOffset`; clearance `= (Start.Z − Hit.Z) − RideHeightBiasCm`, clamped `[0, RideProbeMaxCm]`; airborne ⇒ returns `RideProbeMaxCm` (tallest → least ground effect). `SampleBaked` clamps it to the ride axis.

---

## 6. Interpolation / lookup (`SampleBaked`, `.cpp:338-380`)

**Trilinear** over `(yaw, pitch, ride)` with **edge clamping**.

`Bracket(axis, V) → (I0, I1, frac)` (`.cpp:345-360`):
- `V ≤ axis[0]` → `I0=I1=0, frac=0` (clamp low)
- `V ≥ axis[N-1]` → `I0=I1=N-1, frac=0` (clamp high)
- else first interval `[axis[i], axis[i+1]]` containing `V`; `frac=(V−axis[i])/span` (`span>KINDA_SMALL_NUMBER`, else 0)

Then (`.cpp:362-379`), with `At(yi,pi,ri)=Grid[(yi*NP+pi)*NR+ri]`:
- `Lerp2(ri)` bilinearly blends the 4 `(yaw,pitch)` corners at ride slice `ri`: lerp over pitch (`fp`) then over yaw (`fy`), for **both** `ForceLocalN` and `CoPLocalCm` (component-wise `FMath::Lerp`).
- Final: `OutForce = Lerp(F@r0, F@r1, fr)`; `OutCoP = Lerp(C@r0, C@r1, fr)` (`.cpp:376-379`).
- Empty ride axis ⇒ `r0=r1=0, fr=0` (`.cpp:365`).
- Guard: `Yaw.Num==0 || Pitch.Num==0 || Grid.Num==0` ⇒ zero force/CoP (`.cpp:343`).

Both **force vector and CoP are interpolated** (CoP moves with attitude/ride). No extrapolation — values saturate at grid edges.

---

## 7. Force application (baked path, `.cpp:435-458`)

For each `FVehicleAeroPart P`:
```
if DetachedParts.Contains(P.PartName): continue                 (.cpp:437)
SampleBaked(P, YawDeg, PitchDeg, RideCm) -> ForceLocalN, CoPLocalCm
ForceWorld = Xf.TransformVectorNoScale(ForceLocalN * Q)         (.cpp:442)  # rotate only; scale by speed^2
WorldPos   = Xf.TransformPosition(CoPLocalCm)                   (.cpp:443)  # cm local -> world
Body->AddForceAtLocation(ForceWorld * 100, WorldPos)           (.cpp:444)  # NEWTONS_TO_UE=100 (.cpp:25)
```

Key points for the port:
- The **full 3-component local force** is applied (drag `Fx`, lateral `Fy`, lift/downforce `Fz`) — not just downforce+drag. It is rotated into world by the body transform and applied **at the interpolated CoP**, so it also produces yaw/pitch/roll moments and aero balance shift.
- **`Q=(speed/ref)²`** is the only speed scaling; air density is *not* re-applied (baked in).
- **`NEWTONS_TO_UE = 100`**: engine force units are kg·cm/s²; 1 N = 100 of them (`.cpp:22-25`). A royalty-free engine working in SI (N, metres) drops this factor and converts CoP cm→m instead.
- **Ground effect in the baked path** comes **entirely from the ride-height axis interpolation** (the bake pre-computed suction per ride height, §9). `GroundEffectFactor()` is **not** called here — it applies only to the `Surfaces[]` path.
- Debug read-out (`.cpp:460-472`) computes totals (downforce `Σ max(0,−Fz)·Q`, drag `Σ max(0,−Fx)·Q`, L/D, and a front-balance estimate using a hard-coded `HalfWheelbaseCm=144` for the Ferrari 499P) — display only, not physics.

### 7.1 Part detachment coupling
- `HandlePartDetached(name)` adds to `DetachedParts` (baked path skips it) and clears matching `Surfaces[].bActive` (`.cpp:122-134`).
- `HandleRepaired()` empties `DetachedParts`, re-enables all surfaces (`.cpp:143-148`).
- Bound to `UVehicleDamage::OnPartDetached / OnRepaired` at `BeginPlay` (`.cpp:70-71`); pre-detached parts caught defensively (`.cpp:74-80`).

---

## 8. Hand-authored `Surfaces[]` fallback (`.cpp:150-224`, `.h:18-53`)

Used only when no bake loaded. Distinct, simpler model — per surface `FVehicleAeroSurface`:

| Field | Units | Meaning |
|---|---|---|
| `Name` / `LinkedPartName` | — | label / detach link |
| `LocalPosition` | cm | application point |
| `DownforceAtRefN` | N | downforce at ref speed |
| `DragAtRefN` | N | drag at ref speed |
| `bGroundEffect` | bool | apply live GE multiplier |
| `bActive` | bool | runtime detach flag |

Per tick (`.cpp:191-217`): `Q=(SpeedKmh/ReferenceSpeedKmh)²`; downforce applied along **−body Z** (`-Xf.UnitAxis(Z)`), drag along **−velocity**; downforce scaled by `GroundEffectFactor(rideHeight)` if `bGroundEffect`. Only two scalar magnitudes per surface (no lateral force, no interpolation). `MinSpeedKmh` cutoff at `.cpp:178`.

**Live `GroundEffectFactor`** (`.cpp:404-414`), used only here: `1 + gain·max(0,(refClr/h)²−1)`, capped at `+4`, with linear stall roll-off below `GroundStallCm`. This mirrors the *bake-time* underbody model (§9) — the port can share one implementation.

---

## 9. Writer solver (`AeroSim`) — how forces are produced (for faithful re-bake)

Particle-momentum ("smoke") solver, `Main.java:2833-3118`:

1. **Geometry:** triangle soup from the cage; nodes cm→m (`:2853`); uniform spatial grid for O(1) collision (`:2839-2892`).
2. **Flow direction** `flowDir(yaw,pitch)` (`:2915-2928`): base **−X**; pitch about **Y**, then yaw about **Z**; unit vector.
3. **Inlet:** perpendicular basis `(U,W)`; nodes projected to size inlet rectangle + domain length (`:2978-3002`).
4. **Mass flow per particle:** `mdot = ρ · inletArea / nParticles · speed` (kg/s) (`:3003`).
5. **First-impact Newtonian transfer** (`:3014-3051`): each particle marches `dt=cell/speed`; on first triangle hit, reflect with `restitution` (bake uses `0` ⇒ fully inelastic/Newtonian) and deposit impulse `mdot·Δv` (N) onto that triangle's **part**; then stop (occluded/shadowed panels get nothing). CoP accumulated impulse-magnitude-weighted (`:3042-3044`).
6. **Ground-effect pass** `groundEffectPass` (`:2936-2968`): analytic continuity+Bernoulli suction on underside-facing panels (`normal·Z < −0.2`), `Cp = −gain·max(0,(refClr/h)²−1)`, capped `−4`, stall roll-off below `stallM`; force `= −Cp·q·area` along outward normal (`q=½ρv²`). Adds to per-part force/CoP.
7. **Per-part reduce:** `force[part]` (N), `cop[part]=Σ(mag·pos)/Σmag` (m) (`:3056-3065`).

**Bake sweep** (`bakeToFile`, `Main.java:3824-3929`): triple loop `for yaw × pitch × ride` (`:3893`), `bake(yaw,pitch,ride·0.01, speed, ρ, particles, restitution, geGain, refClearanceM, stallM)`. Bake `speed = airspeedKmh/3.6` m/s, so forces are exactly "at `ReferenceSpeedKmh`". `refClearanceM = maxRide·0.01` (`:3852`). Defaults: yaw `-15..15` (7), pitch `-2,0,3,6`, ride `4,7,10,15`, 6000 particles/attitude, geGain 1.0, stall 3 cm (`:3829-3850`). CoP written m→cm (`:3898`).

---

## 10. Worked cross-check (Ferrari sample)

Header `Ferrari499P.ocaero:1-9`: `ReferenceSpeedKmh=200.0`, `AirDensity=1.225`, `Yaw` 7 vals, `Pitch` 4 vals, `RideHeight` 8 vals (`3..10`) ⇒ grid **7×4×8 = 224** attitudes. 22 parts/block (`Wiper…RearBumper`, lines 12–33) ⇒ **4928 PART rows**. Row 20 `RearWing … Fx=-691.9, Fz=-837.0` ⇒ comment `down 837N drag 692N` confirms sign map (`−Fz`=down, `−Fx`=drag). Rows 12–33 vs 34–55 differ only where ride matters (e.g. `Floor`, `Diffuser`, `FrontSplitter` grow with lower ride; body panels identical across ride) — consistent with the underbody-only ground-effect pass.

---

## 11. Discrepancies, ambiguities, gotchas

1. **Version unchecked:** `OCAERO 1` line is skipped, not validated (`.cpp:253`). Port should validate the magic + version.
2. **`AirDensity` unused at runtime** — parsed then discarded (`.cpp:261,314`); no `AeroAirDensity` field on the asset. It is documentation/bake input only. Runtime scaling is purely `(v/ref)²`.
3. **`SourceCage` silently ignored** (no matching branch).
4. **Ascending-axis assumption** in `Bracket` (§2.3) — not enforced.
5. **Nearest-snap parsing** (`.cpp:290-294`): PART `(yaw,pitch,ride)` values must match axis samples closely; off-grid values snap to nearest cell (writer is exact, hand edits risk silent misplacement/overwrite).
6. **`ParseFloats` heuristic** (`.cpp:232`): keeps tokens that are numeric OR contain `.`/`-`. A stray non-numeric header value containing `-` or `.` would be mis-parsed as a number; a purely integer axis value like `0` relies on `IsNumeric()`. Reproduce carefully or replace with strict parsing.
7. **Two ground-effect implementations** with the same math: bake-time `groundEffectPass` (baked into the ride axis) and runtime `GroundEffectFactor` (Surfaces path only). Do **not** double-apply on the baked path.
8. **Debug-only constants** (`HalfWheelbaseCm=144`, arrow scales) are not physics.
9. **CLAUDE.md/popup "AeroAsset" naming** does not match code (§0).
10. **9-field (no-ride) rows** are legal (single ride slice) though the writer always emits 10 fields.

---

## 12. UE dependencies to strip/replace in the port

| UE type / API | Where | Port replacement |
|---|---|---|
| `FVector`, `FTransform`, `FString`, `FName`, `TArray`, `TSet`, `TMap`, `TObjectPtr` | throughout | your math/containers; `FName`→interned string/ID |
| `FMath::` `Lerp/Square/Max/Min/Clamp/Abs/Asin/Atan2/RadiansToDegrees/UnwindDegrees`, `KINDA_SMALL_NUMBER`, `TNumericLimits` | `SampleBaked`, `ApplyBakedAero`, GE | std math; implement `UnwindDegrees` (fold to (−180,180]) exactly |
| `Xf.TransformVectorNoScale / InverseTransformVectorNoScale / TransformPosition`, `GetUnitAxis` | `.cpp:426,442,443,185` | rotation-only transform + point transform (mind cm↔m) |
| `UPrimitiveComponent`: `GetComponentVelocity`, `GetComponentTransform`, `IsSimulatingPhysics`, `AddForceAtLocation`, `GetFName`, `ComponentHasTag` | body access + apply | your rigid-body: velocity, world transform, add-force-at-point |
| `NEWTONS_TO_UE=100`, `CMS_TO_KMH=0.036` | `.cpp:25-26` | drop the 100× if using SI N+metres; keep the km/h conversion |
| `UActorComponent`/`TickComponent`/`BeginPlay`, `AActor` (`GetOwner/GetRootComponent/FindComponentByClass/GetComponents`) | lifecycle | your component/tick + owner lookup |
| `UWorld::LineTraceSingleByChannel`, `FHitResult`, `FCollisionQueryParams`, `SCENE_QUERY_STAT`, `ECC_Visibility` | `GetLiveRideHeightCm` | your physics raycast (downward ground probe) |
| `FFileHelper::LoadFileToStringArray`, `FPaths::Combine/ProjectContentDir/FileExists` | `LoadAeroBake` | plain file read + path join |
| `FString`: `ParseIntoArray`, `TrimStartAndEnd/TrimEnd`, `FindChar`, `Left/LeftChop`, `StartsWith/EndsWith`, `Contains`, `IsNumeric`; `FCString::Atof` | `ParseOcaero`, `ParseFloats` | your tokenizer/`atof` (preserve the exact rules in §2.2) |
| `GEngine->AddOnScreenDebugMessage`, `DrawDebugDirectionalArrow`, `APlayerController`/`EKeys`/`UGameplayStatics` (F3+A toggle) | debug only | optional; drop |
| `UVehicleDamage` (`OnPartDetached`/`OnRepaired`/`IsPartDetached`/`GetCageAsset`), `UVehicleCageAsset` (`bHasAero`, `AeroYawDeg/PitchDeg/RideCm`, `AeroRefSpeedKmh`, `AeroParts`) | detach coupling + asset load | your damage event bus + baked-payload container |
| `USTRUCT/UCLASS/UPROPERTY/GENERATED_BODY` reflection | structs/component | plain structs/serialization |

Writer (`Main.java`) is pure Java/Swing — no UE deps. To re-bake in-engine, port `AeroSim` (§9) verbatim (spatial grid, Möller–Trumbore `segTri` at `Main.java:3090-3108`, first-impact momentum, `groundEffectPass`) keeping units (nodes cm→m, force N, CoP m→cm on write) and the `flowDir` convention identical to the runtime inverse in §5.