The repo is empty (fresh greenfield), so this is a from-scratch module design. Below is the complete design deliverable, returned directly as my response.

---

# Aver Engine — Physics/Sim + Networking Modules — UE-Free Port Design

Faithful re-implementation of the OpenConstructor27 soft-body vehicle solver, aerodynamics, fracture/debris, and networking as separately-buildable Aver Engine modules. Every behavior below is traced to the authoritative recon; where the recon flags an ambiguity or discrepancy, the resolution is called out inline. Royalty-free only: no UE types, no Chaos, no GPL. C++20 core, stable `extern "C"` ABI, C# editor over P/Invoke, Rust asset pipeline over FFI.

---

## 0. Module map, boundaries, and the "pay-for-what-you-use" rule

The physics/sim + net surface is split into small, independently-linkable static libraries. Each has a minimal public header, a private impl, and (where it crosses a language boundary) a thin `extern "C"` shim. No module pulls in the renderer, and the renderer is the only thing that touches the RHI.

| Module | Static lib | Depends on | C ABI shim | Consumed by |
|---|---|---|---|---|
| Math/containers | `aver.core` | — (only `<cstdint>`, `<cmath>`, STL) | — | everything |
| Job system | `aver.jobs` | `aver.core` | — | physics, net |
| Soft-body solver | `aver.phys.softbody` | `aver.core`, `aver.jobs` | `aver_softbody.h` | runtime, C# editor, net |
| Aerodynamics | `aver.phys.aero` | `aver.core`, `aver.phys.softbody` (read-only cage) | `aver_aero.h` | runtime |
| Fracture/debris | `aver.phys.fracture` | `aver.core`, `aver.jobs` | `aver_fracture.h` | runtime, Rust bake tool |
| Rigid vehicle sim (interop) | `aver.phys.vehicle` | `aver.core` | `oc_sim.h` (**preserved verbatim**) | C# server host, client |
| GPU deform | `aver.render.deform` | `aver.rhi`, `aver.phys.softbody` (read-only disp) | — | renderer only |
| Networking | `aver.net` | `aver.core`, `aver.jobs` | `aver_net.h` | runtime, C# host |
| Matchmaking core (interop) | `aver.match` | `aver.core` | `oc_match.h` (**preserved verbatim**) | coordinator host |

**Hard boundaries that keep this from becoming Unreal-shaped:**

1. `aver.phys.softbody` performs **no collision, no rendering, no I/O, no allocation on the hot path**. It ingests impacts through a shim and emits per-node displacement + damage state. It is compilable and unit-testable with zero graphics/net deps — this is the single most important anti-bloat decision (mirrors the recon's finding that the UE solver's only world coupling is one transform at the impact boundary, `VehicleDamage.cpp:673-681`).
2. The `.ocbeam`/`.ocaero` **text parsers live in the compiler/tool path (Rust `aver.pipeline` + C++ importer), not in the runtime**. The runtime loads a compiled binary cage asset (the "bytecode", cf. `.ocbeam → .uasset`). One shared parser feeds both the offline compiler and a dev loose-file fallback, exactly as `UVehicleDamage::ParseOcbeam` was authoritative for both paths.
3. `aver.core` provides `Vec3/Vec2/Quat/Transform/Mat4` (float32, Z-up, left-handed to mirror `FVector`/`oc::Vec3` semantics — `Vec3.h:1`). It is header-heavy, dependency-free, and compiles into the client, the C# host (via C ABI), and the Rust tools.

---

## 1. Shared conventions (authoritative, single source of truth)

These are fixed constants baked into `aver.core` and asserted at module boundaries. All modules agree.

| Property | Value | Notes |
|---|---|---|
| Length unit | **cm** | cage nodes, CoP, ride height |
| Force unit | **N** (Newtons) | material thresholds, aero force |
| Stiffness | AxialStiffness/PlasticStiffness in **N/cm** | force = stiffness × absolute deformation (cm), never strain-normalized |
| Speed | cm/s internal; km/h on wire & aero header | `km/h→cm/s ×27.78`, `cm/s→km/h ×0.036` |
| Axes | **X fwd, Y right, Z up**, left-handed | vehicle-local frame |
| Handedness fix | `bDeformMirrorY` reflects disp/impact across Y | cage-space→actor-space flips Y, negates normal.Y, **reverses triangle winding** |
| glTF import | Y-up→Z-up `(x,y,z)→(x,−z,y)`, source metres×0.01→cm | happens **only** in the offline importer |
| SI unit choice | **Drop `NEWTONS_TO_UE=100`.** Aver works in SI (N + metres for force application) or keeps cm consistently; CoP cm→m at apply time | eliminates the UE unit fudge |
| Endianness | **little-endian** on all wire formats | matches existing OCServer |
| Vehicle id | `int32`, 0 = invalid, assigned from 1 | same id in sim, net, wire — **not** a UE GUID |

---

## 2. Soft-Body Solver Module (`aver.phys.softbody`)

### 2.1 Runtime data model (POD, index-parallel, cache-friendly)

All structs are plain C++ with no reflection macros. Arrays are `std::vector`, index-parallel; cross-references resolve to array indices once at init and never re-hash on the hot path. This is a byte-faithful port of `FVehicleMaterial/Node/Beam/Panel/Part` + `FOCSolveConfig/FOCSolveResult`.

```cpp
namespace aver::phys {

enum class DamageBehavior : uint8_t { Deform = 0, Fracture = 1, Shatter = 2 };

struct Material {                 // authored, referenced by name; defaults from recon §3.1
    std::string name;
    float stiffness         = 0.7f;   // 0..1  PBD relaxation feel
    float axialStiffnessNcm = 3500.f; // N/cm  elastic axial spring rate
    float bendForceN        = 3000.f; // N     yield onset (permanent bend begins)
    float breakForceN       = 12000.f;// N     instant catastrophic snap
    float plasticStiffNcm   = 800.f;  // N/cm  post-yield extra-bend resistance
    float maxBendCm         = 8.f;    // cm    accumulated bend before tear
    float bendAbsorb        = 0.3f;   // 0..1  energy soaked per cm of bend
    float breakAbsorb       = 0.6f;   // 0..1  energy soaked on snap
    DamageBehavior behavior = DamageBehavior::Deform;
    // writer-only extras (accept 10..13 fields on import; default these):
    float tearStrainTension = -1.f;   // -1 = unset
    float tearStrainCompr   = -1.f;   // -1 = unset
    float density           = 1.f;    // relative node mass
};

struct Node {                    // one point mass
    int32_t nodeID   = 0;        // stable parser key (beams reference by ID)
    Vec3    position;            // cm, car-local (live)
    Vec3    prevPosition;        // Verlet previous
    Vec3    localRest;           // captured in init
    float   invMass  = 1.f;      // only read when useNodeMass
    int32_t intactBeams = 0;
    int32_t totalBeams  = 0;     // tear denominator, frozen at init
    int32_t activeTicks = 0;     // active-set awake countdown
    bool    pinned   = false;    // fixed in car frame (base nodes)
    bool    freed    = false;    // all beams broken -> debris
    bool    torn     = false;    // >= tearNodeFraction beams broken
    bool    brittle  = false;    // majority brittle beams -> cracks not dents
};

struct Beam {
    int32_t beamID = 0, nodeA = 0, nodeB = 0;   // endpoints are NODE IDs
    // resolved copy of owning panel's material (pushed down at parse phase-2):
    float stiffness, axialStiffnessNcm, bendForceN, breakForceN;
    float plasticStiffNcm, maxBendCm, bendAbsorb, breakAbsorb;
    DamageBehavior behavior;
    // hot-path caches (resolved once in init):
    int32_t nodeAIdx = -1, nodeBIdx = -1;       // -1 = unresolved (INDEX_NONE)
    float   restLength = 0.f;    // creeps as beam plastically bends
    float   plasticAccum = 0.f;  // total permanent bend (ductile tear tracker)
    float   peakForce = 0.f;     // debug high-water
    bool    broken = false, retired = false;
};

struct Panel { int32_t panelID; int32_t beamA, beamB, beamC; std::string materialOverride; };
struct Part  { std::string name, role, materialName, meshName;
               std::vector<int32_t> panelIDs; float detachBrokenFraction = 0.3f; };
} // namespace
```

**Parse-time reference resolution (offline compiler, phase 2 — matches `VehicleDamage.cpp:439-518`):** materials indexed by name, panels by ID, beams by ID. For each `Part → Panel → its 3 beams`, copy the panel's effective material (panel `materialOverride` beats part material) down onto the beam's 9 physics fields. Beams owned by no part keep struct defaults. `StructuralStrength` is **NOT** applied here — the compiled asset stays strength-neutral and shareable; the multiplier is applied per-vehicle in `initSolver`.

**Import-format fidelity notes (resolved discrepancies):**
- MATERIAL: **accept 10..13 fields**, defaulting fields 10–12. (Recon flags that the shipped UE parser required exactly 10 while the writer emits 13 — the port fixes this rather than dropping materials.)
- PART `DETACH=<f>` is position-independent, scanned out before positional parsing; default 0.3.
- PANEL 4th field (material override) supported on read even though the writer never emits it.
- Behavior tokens: `SHATTER|GLASS → Shatter`, `FRACTURE → Fracture`, else `Deform` (case-insensitive).
- `OCBEAM 1` header tolerated/ignored. Compiled asset carries its own `formatVersion`.

### 2.2 Solve configuration bundle (thread-agnostic)

Copied by value per step so the identical `solveStep` runs on the game thread or the worker with zero member access — the key to the "one math path" determinism guarantee.

```cpp
struct SolveConfig {                 // mirrors FOCSolveConfig (VehicleDamage.h:294-321)
    int   substeps = 4, solverIterations = 8;
    float velocityDamping = 0.99f;
    float maxNodeSpeed = 1500.f;     // cm/s
    float pinHeight = 20.f;          // cm; bottom band anchored to chassis
    bool  enableDamage = true;
    float damageRate = 8.f;          // /s  plastic creep rate limiter
    // active-set:
    bool  activeSet = true;
    int   activeHoldTicks = 12;
    float activeWakeMoveCm = 0.02f;
    float settleThresholdCm = 0.05f;
    float tearNodeFraction = 0.6f;   // clamp 0.05..1
    float breakKick = 2.f;           // cm recoil on snap
    Vec3  localGravity;              // car-local down * debrisGravity, or 0
    float dt = 0.f;
    float damageTimer = 0.f;         // window: >0 means damage enabled this step
    // opt-in realism flags (base path byte-identical when off):
    bool  useNodeMass = false;
    bool  beamDashpot = false;   float beamDamping = 0.f;
    bool  workHardening = false;  float workHardenCoeff = 0.f;
    // impact-tear cvars (were oc.Damage.ImpactTear*):
    bool  impactTear = true;
    float impactTearMinDisp = 2.f;
    float impactTearRadiusFrac = 0.5f;
};

struct SolveResult { bool anyBreak=false, recounted=false, settled=false; int tornCount=0; };
```

### 2.3 Clean C++ solver interface

```cpp
namespace aver::phys {

class SoftBodySolver {
public:
    // ---- lifecycle ----
    void loadFromAsset(const CageAsset& asset);       // copies arrays verbatim (no parse)
    void initSolver(float structuralStrength);        // one-time; see §2.5
    void repair();                                    // reset to factory (§2.9)

    // ---- per-frame drive (game thread) ----
    // Sync path: solve inline. Async path: this just pushes config + mirrors snapshot.
    void tick(const SolveConfig& cfg, SolveResult& out);

    // ---- impact seams (UE-free collision shim feeds these) ----
    void onHit(const Vec3& worldPoint, const Vec3& worldNormal, float normalImpulse);
    void reportImpact(const Vec3& worldPoint, const Vec3& worldNormal, float speedKmh);

    // ---- outputs for renderer / net ----
    void getCageDisplacements(std::vector<Vec3>& out) const; // Position - LocalRest, cage space
    void getCageRestPositions(std::vector<Vec3>& out) const;
    // net state (durable end-state, never an impact log):
    void getPlasticState(std::vector<int32_t>& beamIdx, std::vector<float>& plasticCm) const;
    void getBrokenBitset(std::vector<uint8_t>& bits) const;   // LSB-first, beam i @ byte i>>3 bit i&7
    void applyPlasticState(const int32_t* idx, const float* cm, size_t n);
    void applyBrokenBitset(const uint8_t* bits, size_t nBits);
    int  repairCounter() const;

    // ---- part events (observer callbacks; no UE delegates) ----
    std::function<void(int partIdx, float impulse, DetachMode)> onPartDetached;
    std::function<void()> onRepaired;

    // ---- the static, thread-agnostic core (worker calls the SAME function) ----
    static void solveStep(std::vector<Node>&, std::vector<Beam>&,
                          const SolveConfig&, SolveResult&);
    // Transform provider so the core stays world-agnostic:
    void setCageTransform(const Transform& carToWorld);

private:
    std::vector<Material> materials_;
    std::vector<Node>     nodes_;
    std::vector<Beam>     beams_;
    std::vector<Panel>    panels_;
    std::vector<Part>     parts_;
    std::unordered_map<int32_t,int32_t> nodeIndexByID_;  // init-time only, never hot path
    Transform carToWorld_;
    int repairCounter_ = 0;
    // async worker (optional, §2.7)
    std::unique_ptr<SolverWorker> worker_;
    // ... derived part-runtime, part-beam map (§2.8)
};
} // namespace
```

The `setCageTransform` + `onHit/reportImpact` pair is the **only** world↔local coupling in the entire physics core, so the module compiles and runs headless in tests.

### 2.4 Solver-tick pseudocode (authoritative — reproduces phase order exactly)

Preserving phase order and float evaluation order is load-bearing for MP determinism. The three phases run per substep, in order: (1) Verlet integrate → (2) damage/plasticity → (3) Gauss-Seidel relaxation.

```
function solveStep(nodes, beams, cfg, out):
    numSub   = max(1, cfg.substeps)          # default 4
    subDt    = cfg.dt / numSub
    numIters = max(1, cfg.solverIterations)  # default 8
    windowOpen = cfg.damageTimer > 0
    maxStep  = cfg.maxNodeSpeed * subDt      # per-substep speed clamp (cm)
    maxMoveSq = 0

    for s in 0 .. numSub-1:

        # ---------- PHASE 1: Verlet integration ----------
        for N in nodes:
            if N.pinned:
                N.position = N.localRest; N.prevPosition = N.position; continue
            if (not windowOpen) and (not N.freed):        # rigid-ride freeze guard
                N.prevPosition = N.position; continue
            if cfg.activeSet and N.activeTicks <= 0 and not N.freed:
                N.prevPosition = N.position; continue
            vel = (N.position - N.prevPosition) * cfg.velocityDamping
            if lenSq(vel) > maxStep*maxStep: vel = normalize(vel) * maxStep
            N.prevPosition = N.position
            N.position += vel
            if cfg.activeSet and lenSq(vel) > cfg.activeWakeMoveCm^2:
                N.activeTicks = cfg.activeHoldTicks
            if N.freed:
                N.position += cfg.localGravity * (subDt*subDt)   # debris free-fall

        # ---------- PHASE 2: damage / plasticity / fracture ----------
        if cfg.enableDamage and windowOpen:
            for B in beams where not B.broken and endpoints resolved:
                NA = nodes[B.nodeAIdx]; NB = nodes[B.nodeBIdx]
                delta = NB.position - NA.position; len = length(delta)
                if len < 1e-4: continue
                axis = delta / len
                stretch = len - B.restLength                     # signed cm
                mag = abs(B.axialStiffnessNcm * stretch)         # N
                B.peakForce = max(B.peakForce, mag)
                brittle = (B.behavior == Fracture or Shatter)
                bendYield = cfg.workHardening
                          ? B.bendForceN + cfg.workHardenCoeff*B.plasticStiffNcm*B.plasticAccum
                          : B.bendForceN

                # (A) catastrophic / brittle snap
                if (mag > B.breakForceN) or (brittle and mag > B.bendForceN):
                    breakBeam(B, NA, NB, axis, B.breakAbsorb, cfg); out.anyBreak = true; continue

                # (B) ductile plastic yield (Deform only)
                if (not brittle) and (mag > bendYield):
                    over = mag - bendYield
                    give = over / max(B.plasticStiffNcm, 1)
                    dP   = give * clamp(cfg.damageRate * subDt, 0, 1)   # rate-limited creep
                    B.restLength   += sign(stretch) * dP               # PERMANENT dent
                    B.plasticAccum += dP
                    bleedVel(NA, clamp(B.bendAbsorb*dP, 0, 1))
                    bleedVel(NB, clamp(B.bendAbsorb*dP, 0, 1))
                    if B.plasticAccum > B.maxBendCm:
                        breakBeam(B, NA, NB, axis, B.breakAbsorb, cfg); out.anyBreak = true; continue

                # (C) optional strain-rate dashpot (surviving beam) — moves prevPos only
                if cfg.beamDashpot and cfg.beamDamping > 0:
                    vRel = dot((velOf(NB) - velOf(NA)), axis)
                    dPush = axis * (0.5 * min(cfg.beamDamping,1) * vRel)
                    NA.prevPosition -= dPush; NB.prevPosition += dPush

            if out.anyBreak (connectivity dirty this step):
                recountConnectivity(nodes, beams, cfg, out)     # §2.6

        # ---------- PHASE 3: Gauss-Seidel distance constraint ----------
        for it in 0 .. numIters-1:
            for B in beams where not B.broken and endpoints resolved:
                NA = nodes[B.nodeAIdx]; NB = nodes[B.nodeBIdx]
                if cfg.activeSet and NA.activeTicks<=0 and NB.activeTicks<=0
                   and not NA.freed and not NB.freed: continue
                delta = NB.position - NA.position; len = length(delta)
                if len < 1e-4: continue
                diff = (len - B.restLength) / len
                corr = delta * (0.5 * B.stiffness * diff)
                wA = NA.pinned ? 0 : (cfg.useNodeMass ? NA.invMass : 1)
                wB = NB.pinned ? 0 : (cfg.useNodeMass ? NB.invMass : 1)
                if wA + wB < 1e-4: continue
                NA.position += corr * (2*wA/(wA+wB))
                NB.position -= corr * (2*wB/(wA+wB))
                maxMoveSq = max(maxMoveSq, lenSq(corr))

    out.settled = (cfg.damageTimer <= 0) and (maxMoveSq <= cfg.settleThresholdCm^2)


function breakBeam(B, NA, NB, axis, absorb, cfg):
    B.broken = true
    released = clamp(1 - absorb, 0, 1)
    kick = released * cfg.breakKick               # cm recoil -> emergent cascade next substep
    NA.prevPosition += axis * kick
    NB.prevPosition -= axis * kick
    NA.activeTicks = NB.activeTicks = cfg.activeHoldTicks

function bleedVel(N, frac):                        # absorb kinetic energy (shrink Verlet gap)
    if N.pinned: return
    N.prevPosition = lerp(N.prevPosition, N.position, clamp(frac,0,1))
```

### 2.5 Initialization (`initSolver`, matches `VehicleDamage.cpp:553-671`)

1. Build `nodeIndexByID` (ID→index).
2. `minZ` over all nodes.
3. Per node: `localRest = prevPosition = position`; `pinned = (position.z <= minZ + pinHeight)` — bottom `pinHeight` cm anchored to chassis.
4. Per beam: resolve `nodeAIdx/nodeBIdx` (unresolved → `broken=true`); `restLength = dist(A,B)`; reset `broken/peakForce/plasticAccum`; **apply `structuralStrength`** multiplicatively to `axialStiffness, bendForceN, breakForceN, plasticStiffness`.
5. `totalBeams` per node from unbroken beams (frozen tear denominator).
6. Brittle vote: per node +1 for each Fracture/Shatter beam, −1 for Deform; `brittle = votes > 0`.
7. Inverse-mass (only used when `useNodeMass`): per-node mean beam axialStiffness, normalized so mean node = 1, clamped `[1/K, K]`, `K = sqrt(nodeMassRange)`; `invMass = 1/mass`.
8. Build part-beam map.

### 2.6 Connectivity recount (gated by dirty flag)

Runs once when the broken set changes:
- Recount `intactBeams` per node; `freed = (intactBeams==0) && !pinned`.
- `torn = !pinned && totalBeams>0 && (totalBeams-intactBeams) >= ceil(clamp(tearNodeFraction,0.05,1)*totalBeams)`.
- Report `tornCount`; caller bumps a `tearGeneration` counter only when it grows (drives the render "open a hole" retear).

### 2.7 Threading / job system (`aver.jobs` + `SolverWorker`)

Faithful port of `FVehicleSolverWorker` with UE primitives swapped for `std`:

- **Private-copy design**: the worker owns `wNodes_/wBeams_` and never touches the component's live arrays. It calls the *same static* `SoftBodySolver::solveStep`.
- **One dedicated thread**, below-normal priority (`std::thread` + platform priority hint / `SetThreadPriority` / `pthread_setschedparam`) — must never starve game/render.
- **One `std::mutex`** guards all cross-thread state; held only for O(1) pointer swaps and small POD copies. The ~60 KB snapshot is filled into scratch **outside** the lock, then pointer-swapped in.
- **Fixed-rate loop** at `1/hz` (default 64, clamp 15..240; server uses `netSendHz` in MP): time accumulator with `maxStepsPerWake = 4` spiral-of-death clamp; leftover capped at `step*4`. Woken by a `std::condition_variable` on `start`/`enqueueImpact`; idles ~50 ms otherwise.
- **Seed race avoidance**: `seedFromCage` stages a seed under lock; the **worker** swaps it into its private arrays at its own loop top.
- **Snapshot (worker→game)** `DeformSnapshot`: per-node `disp (Vec3 = position−localRest)`, per-node `torn (uint8)`, per-beam `brokenBits` (bit-packed), `damageTimerRemaining`, `settled`, `serial`. Published via `std::swap` under lock.
- **Commands (game→worker)** `AsyncSolverCmd`: `Impact{localPt,dir,disp,radius,window,tearRadius}` and `RetireBeams{indexList}`, moved under lock.
- **Game thread per tick** (`pumpAsyncEpisode`): push fresh config, `tryGetSnapshot` (O(1) swap), mirror `disp/torn/brokenBits` onto live arrays, never blocks. On settle handshake, `copyOutState` reclaims the worker's full state (restLength/plasticAccum/prevPosition — the per-frame snapshot omits these).
- **Active-set optimization** composes with the async path: only nodes with `activeTicks>0` (or freed) integrate/relax; impacts and breaks wake the region.

Because the worker only ever mutates its private copy and hands back a snapshot, every existing reader (deform/IK/draw/net) is race-free.

| UE threading dep | Replacement |
|---|---|
| `FRunnable` / `FRunnableThread::Create(TPri_BelowNormal)` | `std::thread` + priority hint |
| `FEvent` `Wait/Trigger` | `std::condition_variable` + `std::mutex` (`wait_for`) |
| `FCriticalSection` / `FScopeLock` | `std::mutex` / `std::lock_guard` / `std::unique_lock` |
| `FThreadSafeBool` / `FThreadSafeCounter` | `std::atomic<bool>` / `std::atomic<int32_t>` |
| `FPlatformTime::Seconds()` | `std::chrono::steady_clock` |
| `Swap` / `MoveTemp` | `std::swap` / `std::move` |

### 2.8 Part detach & fracture coupling

Parts registered on first tick into `PartRuntime{ nodeIdx[], interiorBeamIdx[], boundaryBeamIdx[], boundaryBreakTarget = max(1, ceil(boundaryCount*boundaryFraction)), centroid, tunables }`. Two mechanisms:

1. **Deformable (skeletal) parts — seam-driven**: count broken boundary beams each tick; when `>= boundaryBreakTarget` → detach: mark detached, stop cage-driving the mesh, `retirePartBeams` (sets `broken=retired=true`, marks connectivity dirty, wakes nodes, enqueues `RetireBeams` to the worker), hand mesh to physics with outward kick `centroidDir*detachImpulse + carVelocity`.
2. **Fracturable (geometry-collection) parts — impact-driven**: in `onHit`, the single nearest fracturable part with `normalImpulse >= fractureImpulse` within `impactRadius` is **deferred to next tick** (never run detach inside the collision callback) → detach as dynamic debris. The **largest** fracturable part (the monocoque) is auto-protected from whole detach.
3. **Accumulated panel shed**: when `detachBrokenFraction` of a PART's cage beams break, queue the panel in `pendingDetachParts` for the body skin to shed.

`onPartDetached(partIdx, impulse, mode)` fires the callback that the aero + net modules subscribe to.

### 2.9 Repair

Full factory reset: abort any async episode; per node `position = prevPosition = localRest`, clear `freed/torn`, `intactBeams = totalBeams`; per beam `broken=retired=false`, `peakForce=plasticAccum=0`, `restLength = dist(localRest_A, localRest_B)` (undoes plastic shortening). Bump `repairCounter` (net snapshot carries it — a change means "reset to factory", so a dropped repair can't leave a client permanently wrecked) and `tearGeneration`; fire `onRepaired`.

### 2.10 Impact injection math (behind the collision shim)

Both entry points open the damage window (`damageTimer = damageWindowDuration`), wake the solver, map the hit into cage-local space, apply Y-mirror (gated by `bDeformMirrorY`), compute injected displacement via the soft-knee crush curve, and push nodes in radius by writing `prevPosition` (so the resulting velocity is rate-limited by `maxNodeSpeed` — never a teleport).

```
onHit:        gate normalImpulse >= minImpactImpulse (200);  raw = |impulse| * impactScale(0.01)
reportImpact: gate speedKmh >= 5;                            raw = speedKmh*27.78 * impactSpeedScale(0.01)

crush curve (OCCrushCurve):
    knee = maxImpactStep (50);  ceil = maxImpactStep * max(1, crushCeilingMult 1.8)  # 90 cm
    if raw <= knee or ceil <= knee: disp = min(raw, knee)                # hard clamp
    else: disp = knee + (ceil-knee)*(1 - exp(-(raw-knee)/(ceil-knee)))   # C1, <= ceil

node push (skip pinned):
    activeTicks = max(1, activeHoldTicks)
    if node.brittle: continue                       # carbon cracks, doesn't dent
    falloff = 1 - sqrt(d2)/R                          # linear radial
    node.prevPosition -= localDir * (disp * falloff)

impact tear (brittle hole):
    tearRadius = impactTear && disp >= impactTearMinDisp
               ? R * clamp(impactTearRadiusFrac,0,1) * clamp(disp/maxStep, 0.3, 1) : 0
    break every unbroken Fracture/Shatter beam whose midpoint is within tearRadius

chassis rebound (applied to rigid chassis body, not cage):
    Vn = dot(V, N);  if Vn < 0: V -= N * (Vn * (1 + clamp(reboundCoefficient,0,1)))   # default e=0.25
```

`reboundCoefficient` is loaded from `.ocbeam REBOUND` (absent → keep configured default). `crashEnergyAbsorb` is legacy/unused (superseded by rebound).

### 2.11 Determinism considerations (MP-critical)

- **Fixed `dt`** on the authoritative path (async uses `1/hz`); same substeps/iterations everywhere.
- **Fixed iteration order**: beams and nodes iterated in stored array order; Gauss-Seidel is order-dependent and must match bit-for-bit across peers.
- **No hash-order iteration on the hot path** — `unordered_map` used only during init resolution; all runtime loops are over dense index-parallel vectors.
- **Consistent float semantics**: compile the solver TU with strict IEEE (`/fp:precise` on MSVC, `-ffp-contract=off` on clang/gcc), no fast-math, no FMA contraction, no `-Ofast`. The static `solveStep` is the single code path for game thread and worker, guaranteeing identical evaluation.
- **Crash seeds are replayed**, not results shipped: every peer runs the identical deterministic cage from the same seed, so dents land identically. Damage state (plastic deltas + broken bitset) is the durable end-state fallback for late joiners / reconciliation — never an impact log (replaying a log would double-apply).

### 2.12 Stripped UE dependencies (solver) → replacements

| UE type/API | Replacement |
|---|---|
| `FVector` (double), `FVector3f` | `aver::Vec3` (float; add `Dist/DistSquared/GetSafeNormal/Size/SizeSquared`) |
| `FTransform` + `Inverse/TransformVectorNoScale/TransformPosition` | `aver::Transform` (pos+quat[+scale]); the only world↔local coupling |
| `FQuat`, `FRotator`, `FVector2D` | `aver::Quat/Euler/Vec2` |
| `FMath::{Clamp,Max,Min,Abs,Sqrt,Exp,Lerp,CeilToInt,RoundToInt,Sign}` | `std::` / inlines |
| `KINDA_SMALL_NUMBER`(1e-4), `INDEX_NONE`(−1), `MAX_flt` | `constexpr` / `std::numeric_limits` |
| `TArray/TMap/TSet` | `std::vector/std::unordered_map/std::unordered_set` |
| `FString/FName` | `std::string` / interned `uint32` id |
| `USTRUCT/UCLASS/UPROPERTY/GENERATED_BODY/UENUM/UMETA` | plain struct/enum class, no macros |
| `TObjectPtr/TWeakObjectPtr/TSoftObjectPtr/TUniquePtr` | raw / `std::weak_ptr` / asset handle / `std::unique_ptr` |
| `DECLARE_DYNAMIC_MULTICAST_DELEGATE*` (`OnPartDetached/OnRepaired`) | `std::function` callback list |
| `UE_LOG` / `TAutoConsoleVariable` | own logging / config constants (`impactTear=1`, `impactTearMinDisp=2`, `impactTearRadiusFrac=0.5`) |
| `UPrimitiveComponent/FHitResult/OnComponentHit`, `Get/SetPhysicsLinearVelocity` | `onHit/reportImpact` shim + your rigid-body API |
| `FRandomStream` | `std::mt19937` + `std::uniform_real_distribution` |
| `WITH_EDITOR` Voronoi bake (`OC27FractureLibrary`, PlanarCut) | offline tool (§4), no runtime code |

---

## 3. Aerodynamics Module (`aver.phys.aero`)

### 3.1 In-memory model (compiled from `.ocaero`)

```cpp
struct AeroSample { Vec3 forceLocalN; Vec3 copLocalCm; };
struct AeroPart   { std::string partName; std::vector<AeroSample> grid; }; // NY*NP*NR
struct AeroTable  {
    std::vector<float> yawAxisDeg, pitchAxisDeg, rideAxisCm;
    float refSpeedKmh = 200.f;
    std::vector<AeroPart> parts;
    // NY=max(1,yaw.size), NP=max(1,pitch.size), NR=max(1,ride.size)
    int   idx(int yi,int pi,int ri) const { return (yi*NP + pi)*NR + ri; } // exact flatten
};
```

### 3.2 Loading `.ocaero` (parser in compiler + dev loose-file loader)

Line-oriented, `#`-comment, comma-delimited, trailing `;` optional; `OCAERO 1` header **validated** in the port (the UE reader skipped it). Sections `HEADER{}` / `PART{}`.
- HEADER numeric tokenizing: split on `,`, keep numeric tokens (drop the leading key). Keys: `ReferenceSpeedKmh` (used), `AirDensity` (**parsed then discarded** — forces already embed ρ), `SourceCage` (ignored), `Yaw/Pitch/RideHeight` (axes).
- PART rows: `Name, yaw, pitch, [ride,] Fx, Fy, Fz, CoPx, CoPy, CoPz`. `>=10` tokens ⇒ ride present; `9` ⇒ single ride slice; `<9` ⇒ skip.
- Grid placement: snap each row's `(yaw,pitch,ride)` to nearest axis index; write into `idx(yi,pi,ri)`. Writer output is dense; sparse cells stay zero.
- **Port fix**: enforce ascending axes (the runtime `bracket` assumes monotonic-increasing) and use strict numeric parsing rather than the `contains('.'|'-')` heuristic. Sign map: drag `Fx<0`, downforce `Fz<0`, lateral `Fy` right+.

The compiled binary aero payload cooks into the cage asset (preferred, packageable) exactly as the UE path preferred the asset over the loose file. Load priority: cage asset → loose `.ocaero` → hand-authored surfaces fallback.

### 3.3 Per-tick attitude derivation (baked path)

```
Vw       = body world velocity (cm/s)
speedCmS = |Vw|;  speedKmh = speedCmS * 0.036
if speedKmh < minSpeedKmh(10) or speedCmS < 1: return
windLocal = carToWorld.inverseTransformVectorNoScale(-Vw / speedCmS)   # oncoming air, car frame
pitchDeg  = degrees(asin(clamp(windLocal.z, -1, 1)))
yawDeg    = unwindDegrees(degrees(atan2(windLocal.y, windLocal.x)) - 180)  # (-180,180]
Q         = (speedKmh / max(1, refSpeedKmh))^2
rideCm    = liveRideHeightCm()   # downward ray probe; airborne -> max (least ground effect)
```
`unwindDegrees` and the `-180°` fold must be reproduced exactly or yaw sign flips relative to the bake's `flowDir` convention.

### 3.4 Interpolation (trilinear, edge-clamped)

```
bracket(axis, v) -> (i0, i1, frac):
    if v <= axis[0]:      return (0,0,0)
    if v >= axis[N-1]:    return (N-1,N-1,0)
    find interval [axis[i], axis[i+1]] containing v
    span = axis[i+1]-axis[i];  frac = span>1e-4 ? (v-axis[i])/span : 0
    return (i, i+1, frac)

sampleBaked(part, yaw, pitch, ride) -> (forceLocalN, copLocalCm):
    if yaw.empty or pitch.empty or grid.empty: return (0,0)
    (yi0,yi1,fy) = bracket(yawAxis, yaw)
    (pi0,pi1,fp) = bracket(pitchAxis, pitch)
    (ri0,ri1,fr) = rideAxis.empty ? (0,0,0) : bracket(rideAxis, ride)
    # bilinear over (yaw,pitch) at each ride slice, then lerp over ride; for BOTH force and CoP:
    F@r = lerp( lerp(At(yi0,pi0,r),At(yi0,pi1,r),fp), lerp(At(yi1,pi0,r),At(yi1,pi1,r),fp), fy )
    force = lerp(F@ri0, F@ri1, fr);  cop = lerp(C@ri0, C@ri1, fr)   # component-wise
    return (force, cop)
```
No extrapolation — values saturate at grid edges. Both force **and** CoP are interpolated (CoP moves with attitude/ride).

### 3.5 Force application (at CoP, scaled by speed²)

```
for each AeroPart P:
    if detachedParts.contains(P.partName): continue          # detach coupling
    (fLocalN, copCm) = sampleBaked(P, yawDeg, pitchDeg, rideCm)
    forceWorld = carToWorld.transformVectorNoScale(fLocalN * Q)   # rotate only, scale by speed^2
    worldPos   = carToWorld.transformPosition(copCm)              # cm->world (convert to m if SI)
    body.addForceAtLocation(forceWorld, worldPos)                 # SI: drop the *100 UE factor
```
- The **full 3-component** local force is applied (drag Fx + lateral Fy + lift/downforce Fz) at the interpolated CoP → produces yaw/pitch/roll moments and aero balance shift.
- `Q=(speed/ref)²` is the **only** speed scaling; density is baked in, not re-applied.
- **Ground effect on the baked path comes entirely from the ride-height axis** — do NOT also call the live `groundEffectFactor` (that belongs only to the hand-authored `surfaces[]` fallback, which shares the same math: `1 + gain·max(0,(refClr/h)²−1)`, capped +4, linear stall roll-off below `groundStallCm`).
- Detach coupling: subscribe to `SoftBodySolver::onPartDetached` → add name to `detachedParts` (baked path skips it) and clear matching `surfaces[].active`; `onRepaired` → clear the set.

### 3.6 Optional in-engine re-bake

Port `AeroSim` (particle-momentum "smoke" solver) verbatim into the Rust/C++ tool path: triangle soup from cage (nodes cm→m), uniform spatial grid, `flowDir(yaw,pitch)` (base −X, pitch about Y, yaw about Z), first-impact Newtonian transfer (Möller–Trumbore, restitution 0), impulse-weighted CoP, plus the analytic underbody `groundEffectPass` (Bernoulli suction on panels with `normal·Z < −0.2`). Keep units and the `flowDir` convention identical to §3.3's inverse or yaw sign flips.

### 3.7 Stripped UE deps (aero) → replacements

| UE | Replacement |
|---|---|
| `FMath::Lerp/Square/Asin/Atan2/UnwindDegrees`, `KINDA_SMALL_NUMBER` | `std::` + own `unwindDegrees` (fold to (−180,180]) |
| `NEWTONS_TO_UE=100`, `CMS_TO_KMH=0.036` | drop the 100× (SI N+m); keep 0.036 |
| `UWorld::LineTraceSingleByChannel/FHitResult` (ride probe) | your downward raycast |
| `FFileHelper/FPaths` | plain file read + path join |
| `AddForceAtLocation`, transform ops | your rigid-body API |

---

## 4. Fracture / Debris Module (`aver.phys.fracture`)

Two halves: an **offline bake tool** (was editor-only Voronoi) and a **runtime debris system**.

### 4.1 Offline fracture bake (Rust `aver.pipeline` + C++ helper)

The UE `OC27FractureLibrary` is entirely `#if WITH_EDITOR` — the runtime never calls it. In Aver it becomes an **asset-pipeline step**: scatter Voronoi sites → planar-cell cut → emit pre-diced shard meshes stored in the compiled asset. No runtime code. Royalty-free geometry: use a permissive Voronoi/half-plane clipper (e.g. MIT `voro`-style or a hand-rolled clipper), not UE `PlanarCut`/`Voronoi`. Fracturable parts ship with their shards; the runtime only instantiates them.

### 4.2 Runtime debris (`DebrisChunk`, from `OCDebrisChunk`)

Engine-agnostic algorithms to port; the mesh/physics/VFX glue is reinterfaced onto Aver's renderer/rigid-body.

- **Shard split** (`splitIntoClusters`): farthest-point seeding (first seed via `std::mt19937`) + nearest-seed triangle assignment → K jagged clusters, each re-indexed into an independent mesh chunk. `K = clamp(desired, 2, min(triCount/2, 48))`.
- **Mass** = `clamp(triangleArea_cm² * 1e-4 * arealDensityKgM2, 0.3, 80)` kg.
- **Launch**: outward from impact origin toward chunk centroid + up + random jitter; speed mass-scaled.
- **Secondary shatter**: shatterable if `generation < maxShatterGenerations && area >= minShatterAreaCm2 && tris >= 12`; on a rigid hit above `secondaryShatterImpulse`, defer re-split to next tick. Global cap `maxLiveDebris`.
- Debris are handed to Aver's rigid-body layer (host physics) — the soft-body solver does no ground contact; freed cage nodes only free-fall under `localGravity`.

### 4.3 Stripped UE deps (fracture/debris) → replacements

| UE | Replacement |
|---|---|
| `PlanarCut`, `Voronoi`, `FVoronoiDiagram`, `FPlanarCells`, `CutMultipleWithPlanarCells` | permissive Voronoi clipper in the offline tool |
| `UGeometryCollection/Component`, Chaos crumble/cluster APIs | pre-diced shard meshes + your rigid-body |
| `UProceduralMeshComponent` (debris mesh) | dynamic vertex-buffer mesh |
| `FRandomStream` | `std::mt19937` |
| Niagara (dust/spark/glass FX) | your particle system (cosmetic) |
| `GetTimerManager().SetTimerForNextTick` | deferred-command queue drained at frame start (keep the pattern — detach out of the hit callback) |

---

## 5. Physics → Renderer flow (CPU cage → GPU deform)

### 5.1 The two paths (keep both; parity is contractual)

| Path | Mechanism |
|---|---|
| CPU (working baseline) | dynamic mesh section per part; `vert = rigidPose(rest) + clamp(mirrorY(Σ_k w_k·nodeDisp[node_k]) · gain · deformStiffness, maxD) + boneSkinDelta`; per-section MID for livery; only recompute normals on moved panels |
| GPU (scale path) | one compute thread per vertex; writes a UAV that is **also** the position vertex stream |

**Binding**: once the solver's rest pose exists, bind each render vertex to its K = `skinK` (clamp 1..8, default 4) nearest cage nodes via a uniform spatial grid; inverse-distance weights `w = 1/(dist²+1)`, **normalized at bake**. Lookup in cage space (un-mirror the actor-space vert first).

### 5.2 GPU deform pipeline (`aver.render.deform`, behind the RHI)

Per-frame CPU→GPU:
1. Gate: skip if `getDeformVersion()` unchanged and no bone active (a settled dent persists in the output buffer).
2. `getCageDisplacements(disp)` = `position − localRest`, cage space cm.
3. Flatten to `float[numNodes*3]` (x,y,z interleaved).
4. Marshal to the render thread **by value** (moved copy; no shared mutable state).
5. Recreate the shared `NodeDisp` buffer from the upload view, dispatch per visible+bound+non-empty, non-skinned section.

Buffer contract (typed buffers, element stride 4B — **not** structured):

| Name | View | Format | Count | Lifetime |
|---|---|---|---|---|
| `RestPositions` | SRV `Buffer<float>` | R32_FLOAT | verts*3 | static per section |
| `BindNodeIdx` | SRV `Buffer<int>` | R32_SINT | verts*K | static; re-upload on tear |
| `BindNodeWt` | SRV `Buffer<float>` | R32_FLOAT | verts*K | static; re-upload on tear |
| `NodeDisp` | SRV `Buffer<float>` | R32_FLOAT | nodes*3 | recreated every frame, shared |
| `OutPositions` | UAV `RWBuffer<float>` **+ VB** | R32_FLOAT | verts*3 | static; also bound as position stream |

Compute math (must match CPU `DeformedVert` byte-for-byte — order is load-bearing):
```
disp = 0
for k in 0..K:
    ni = BindNodeIdx[vi*K + k]
    if 0 <= ni < NumNodes: disp += NodeDisp[ni] * BindNodeWt[vi*K + k]   # SUM, weights pre-normalized
if bMirrorY: disp.y = -disp.y            # cage -> actor
disp *= Gain                             # Gain already folds in per-panel DeformStiffness
if MaxD > 0 and length(disp) > MaxD: disp *= MaxD/length(disp)
OutPositions[vi] = RestPositions[vi] + disp
```
`[numthreads(64,1,1)]`, `vi = SV_DispatchThreadID.x`, guard `vi>=NumVerts`, dispatch `ceil(NumVerts/64)`. Scalars go in a `cbuffer b0`; typed buffers get explicit `register(t0..t3,u0)`.

**Renderer integration (the key detail)**: `OutPositions` is created as UAV+SRV+VB (dual-use) and bound as the **position vertex stream** (VET_Float3, stride 12, offset 0) so every raster pass (base/GBuffer/depth/shadow/velocity) reads deformed positions. Per-frame barrier: `(VertexOrIndexBuffer|SRV) → UAVCompute`, dispatch, `→ (VertexOrIndexBuffer|SRV)`.

RHI mapping (Aver's DX12-primary / DX11 / Vulkan abstraction): DX12 `UNORDERED_ACCESS ↔ VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE` + UAV barrier; Vulkan `SHADER_WRITE ↔ VERTEX_ATTRIBUTE_READ | SHADER_READ` with matching stages. Shader compiled with DXC → DXIL (DX12) / SPIR-V (Vulkan, off by default until SDK present) / FXC-or-DXC to DXBC (DX11).

**Scope**: GPU path implements only the cage-crumple blend. The CPU path additionally composes rigid pose + articulated linear-blend bone skin (`Σ w·(bonePalette[bone]·bindPos)`, mapped by cached glTF→cage transform then Y-mirrored). **Skinned sections are excluded from GPU eligibility and fall back to CPU** — same rule as the recon.

Eligibility gate: `driveMode==GpuCompute && renderBody && SM6/DX12-capable && >=1 non-broken non-skinned section with verts+tris`, else CPU fallback.

In-place topology updates (avoid the crash-frame rebuild spike): **hide** section (flag, skipped by draw+dispatch → shed panel hole); **retear** (swap reduced index buffer + re-upload weight SRV with torn nodes zeroed & survivors renormalized — CPU must match); **upload bind** (build idx/weight SRVs post-bind, flip `hasBind`).

### 5.3 Stripped UE deps (GPU deform) → replacements

| UE | Replacement |
|---|---|
| `FGlobalShader`/`IMPLEMENT_GLOBAL_SHADER`/`SHADER_USE_PARAMETER_STRUCT` | Aver shader object + compute PSO |
| `BEGIN_SHADER_PARAMETER_STRUCT`/`SHADER_PARAMETER_*` | explicit root signature / descriptor set layout + cbuffer |
| `FComputeShaderUtils::Dispatch` | `ID3D12GraphicsCommandList::Dispatch` / `vkCmdDispatch` |
| `FRWBuffer`/`FReadBuffer` | RHI buffer with UAV+SRV(+VB) / SRV views, R32_FLOAT / R32_SINT |
| `ERHIAccess/FRHITransitionInfo/RHICmdList.Transition` | Aver barrier API (§5.2) |
| `FLocalVertexFactory/FVertexStreamComponent/FStaticMeshVertexBuffers/FDynamicMeshIndexBuffer32` | Aver input layout binding UAV as position VB + static normal/UV/color VBs + IB |
| `FPrimitiveSceneProxy/GetDynamicMeshElements/FMeshBatch` | Aver mesh/draw record |
| `ENQUEUE_RENDER_COMMAND/FRHICommandListImmediate` | Aver render command queue |
| `AddShaderSourceDirectoryMapping` / `/OCShaders/…` virtual path | Aver shader include resolver |
| `#include "/Engine/Public/Platform.ush"` | remove; plain HLSL under DXC |
| `UMaterialInstanceDynamic` / runtime material API | Aver material/MID (build via runtime API, never an editor recompile that crashes headless) |

---

## 6. Networking Module (`aver.net`)

### 6.1 Design rules preserved from the recon

- **Reconstruct, don't send**: dents/debris/aero/audio are rebuilt locally from `config + impacts + replicated movement`. **No per-node / per-beam live transform stream ever crosses the wire.**
- **Damage is a durable end-state channel**, never an impact log (replaying doubles it).
- **Crash seeds replay** through each peer's deterministic local cage.
- **Client-authoritative relay** in the shipping Alpha (server stores + rebroadcasts pose; does not sim your car); server-authoritative path scaffolded (`oc_sim` input) but dormant.
- **Three serialization schemes existed; the port keeps only the one that reaches OCServer** — Protocol (A), the raw little-endian UDP wire. (B) `OCNetWire` framed-batch and (C) UE RPC/RepNotify were UE-internal and are the thing being replaced. The port re-implements the **modular channel/module/coordinator triad** (§6.7) internally, but frames it with Protocol (A) for OCServer interop.

### 6.2 Transport

Raw UDP, little-endian, **no packet header/length prefix/magic** — each datagram is `[u8 MsgType]` then fields. Game plane port **7777**, coordinator plane **7788**. Two independent client sockets (game + coordinator, the latter only during matchmaking). MTU budget 1400; pose paged, damage fragmented ≤1200B. No reliability layer on the game plane — "newest wins, loss self-heals". Replace UE `FSocket/ISocketSubsystem/FTSTicker` with BSD sockets / asio / enet and your own loop. String conventions: `[u16 len][utf8]` (`Str`) and `[u8 len][utf8]` (Join name). Additive-trailing-field rule: old clients omit `caps/name/matchToken`; the server does guarded reads.

### 6.3 Net message table — GAME PROTOCOL (A) (little-endian, after `[u8 MsgType]`)

MsgType enum (values load-bearing): `Join=1, Welcome=2, Input=3, PoseBatch=4, Leave=5, Heartbeat=6, Reject=7, ClientPose=8, Impact=9, Snapshot=10, Despawn=11, LobbyState=12, LobbyControl=13, LobbyClientInfo=14, PenaltyNotify=15, TimingState=16, SectorSplit=17, VehicleConfig=18, ConfigStatus=19`.

**Client → Server**

| Msg | Payload after `[u8 type]` |
|---|---|
| **Join (1)** | `[u16 protoVer=1][u32 clientBuildId][u64 mapContentId][u8 hashAlgo][32B mapRoot][u8 caps][u8 nameLen][name][u64 matchToken]` |
| Input (3) *(reserved/server-auth)* | `[u32 clientFrame][u8 throttle][u8 steer][u8 brake][u8 flags]` |
| **ClientPose (8)** *(live)* | `[u32 frame][f32 x][f32 y][f32 z][f32 yawDeg]` + optional wide `[f32 vx][f32 vy][f32 vz][f32 massKg]` |
| **Impact (9)** | `[f32 lx][f32 ly][f32 lz][f32 nx][f32 ny][f32 nz][f32 speedKmh]` (**car-local** frame) |
| **Snapshot (10)** | `[u16 seq][u8 chunkIdx][u8 chunkCount][chunkBytes]` (chunk ≤1200B) |
| Heartbeat (6) | `[u32 clientFrame]` |
| Leave (5) | *(empty)* |
| LobbyClientInfo (14) | `[u16 nameLen][name][u8 ready]` |
| VehicleConfig (18) | `[u8 category][u16 pathLen][path]` |
| LobbyControl (13) | `[u16 cmdLen][cmd]` (host car only) |

Reassembled Snapshot payload (client→server) = `[i32 beamCount]( [i32 beamIdx][i16 plasticMm] )*`, `plasticMm = round(plasticCm*10)`. **This UDP damage form carries only sparse plastic deltas** (no broken bitset / detached parts / repair counter — that richer form is the internal state model §6.5).

**Server → Client**

| Msg | Payload after `[u8 type]` |
|---|---|
| **Welcome (2)** | `[i32 vehicleId][u16 tickHz][u64 mapContentId][u16 mapNameLen][mapName][u8 serverFlags][u8 phase]` |
| **Reject (7)** | `[u8 reasonCode][u16 detailLen][detail]` |
| **PoseBatch (4)** | `[u16 count]( [i32 id][f32 x][f32 y][f32 z][f32 yawDeg] )*` (20B) **or wide** `+[f32 vx][f32 vy][f32 vz][f32 massKg]` (36B) — self-describing stride |
| **Impact (9)** | `[i32 sourceCarId][f32 lx][f32 ly][f32 lz][f32 nx][f32 ny][f32 nz][f32 speedKmh]` |
| **Snapshot (10)** | `[i32 carId][u16 seq][u8 idx][u8 count][chunkBytes]` |
| Despawn (11) | `[i32 carId]` |
| LobbyState (12) | `[u32 epoch][u8 phase][u8 format][u16 laps][u16 durationMin][u16 phaseRemainingSec][u16 maxPlayers][u16 flags][u64 mapContentId][u16 mapNameLen][mapName][u16 umapLen][umap][u16 rosterCount]( [i32 carId][u8 pflags][u8 nameLen][name] )*` |
| ConfigStatus (19) | `[u8 status][u16 detailLen][detail]` (Required=0/Accepted=1/Rejected=2) |
| PenaltyNotify (15) | `[i32 carId][u8 ruleId][u8 action][u16 addedTenthsSec][u32 lapNumber][u16 detailLen][detail]` |
| TimingState (16) | `[u16 count]( [i32 carId][u8 position][u8 lapsDone][u32 lastLapMs][u32 bestLapMs][i32 gapMs][u16 penTenths][u8 flags] )*` (21B/entry, paged) |
| SectorSplit (17) | `[i32 carId][u8 sectorIdx][u32 sectorMs][i32 deltaToBestMs][u8 flags]` |

Bitfields: `serverFlags` bit0 LobbyEnabled, bit1 VehicleConfigRequired. LobbyState `flags` bit0 InSession/bit1 LateJoinAllowed/bit2 StartCountdown; roster `pflags` bit0 Ready/bit1 Host/bit2 Configuring. TimingState `flags` bit0 DQ/bit1 lapInvalid/bit2 inPit/bit3 wrongWay. SectorSplit `flags` bit0 personalBest/bit1 sessionBest/bit2 invalidated.

Enums: `RejectReason{None=0,ProtocolMismatch=1,MapUnknown=2,MapHashMismatch=3,UnsupportedHash=4,ServerFull=5,Banned=6,SessionInProgress=7,InvalidMatchToken=8,VehicleConfigTimeout=9}`; `HashAlgo{None=0,XxHash128=1,Blake3=2,Sha256=3}` (default 3); `SessionPhase{Lobby=0,Practice=1,Qualifying=2,Race=3,Results=4}`; `SessionFormat{FreeRoam=0,Practice=1,Qualifying=2,Race=3}`; `PenaltyRule{TrackLimits=0,CornerCut=1,WrongWay=2,PitSpeed=3,CausingCollision=4,ImpossibleLap=5}`; `PenaltyAction{Warning=0,LapInvalidated=1,AddedTime=2,Disqualified=3}`.

### 6.4 Net message table — MATCHMAKING PROTOCOL (coordinator plane, after `[u8 CoordMsg]`)

`CoordMsg{QueueJoin=1,QueueUpdate=2,MatchFound=3,QueueLeave=4,QueueReject=5,SrvRegister=10,SrvWelcome=11,SrvHeartbeat=12,SrvReserve=13,SrvGoodbye=14}`; `str=[u16 len][utf8]`.

| Msg | Dir | Payload |
|---|---|---|
| QueueJoin (1) | C→Co | `[u16 protoVer][u8 mode][str mapPref][str name]` (mode 0=Casual,1=Premier) |
| QueueUpdate (2) | Co→C | `[u32 ticketId][u8 state][u16 position][u16 found][u16 needed][u16 etaSec]` |
| MatchFound (3) | Co→C | `[u32 ticketId][u64 token][str serverIp][u16 port][str map]` |
| QueueLeave (4) | C→Co | `[u32 ticketId]` |
| QueueReject (5) | Co→C | `[u32 ticketId][u8 reason]` |
| SrvRegister (10) | S→Co | `[u16 protoVer][str advertiseIp][u16 port][u16 maxPlayers][str map]` |
| SrvWelcome (11) | Co→S | `[u32 instanceId]` |
| SrvHeartbeat (12) | S→Co | `[u32 instanceId][u16 players][u8 phase][str map]` |
| SrvReserve (13) | Co→S | `[u64 token][str map][u16 count]( [i32 playerId] )*` |
| SrvGoodbye (14) | S→Co | `[u32 instanceId]` |

`QueueState{Searching=0,Matched=1}`; coordinator `RejectReason{None=0,PremierRequiresSubscription=1,BadRequest=2,NoCapacity=3}`.

Client matchmaking flow: open coordinator socket, `SendQueueJoin`, re-send every 2 s (keepalive); on `MatchFound` set `serverHost/port`, store `matchToken`, close coord socket, auto-`connect()` to the game server, which validates the token at Join against its live reservation. Anti-spoof: accept coordinator datagrams only from the known coordinator address. Matchmaking core (`oc_match`, pure/deterministic `oc_match_form`) is **reused verbatim** — Casual live, Premier (Elo-banded, subscription) reserved.

### 6.5 What is actually replicated for a vehicle

| Data | Wire | Cadence | Reconstruction |
|---|---|---|---|
| Body pose | `ClientPose`→`PoseBatch` (pos cm + yaw°, optional vel+mass) | `sendHz`(30) / `poseSendHz` | absolute, interpolated (`remoteInterpSpeed`; first update snaps) |
| Driving input | `Input` (dormant) | `netSendHz`(64) | server sim (P2) |
| Crash seed | `Impact` (car-local UDP) | event | **replayed through the deterministic cage** |
| Part detach | via snapshot `detachedParts` (+ reliable multicast in server-auth) | event+state | `commandDetachPart` |
| Damage end-state | `Snapshot` (sparse plastic on UDP; internal full form below) | dirty ≤ `snapshotPushInterval`(0.2s) | overwrite, no replay |
| Car config | `VehicleConfig` | once at join | rebuild locally |

**Internal durable damage state** (`DamageSnapshot`, the richer model the modules exchange and that late-joiners get): `yieldedBeams[]{beamIndex,plasticDeltaCm}` + `brokenBitset[]` (1 bit/beam, LSB-first) + `detachedParts[]` (part indices) + `repairCounter (i32, monotonic)`. `torn/freed` are derived locally from `brokenBitset`. This maps 1:1 onto the solver's `getPlasticState/getBrokenBitset/getDetachedPartIndices/repairCounter`. On the OCServer UDP wire this is projected down to just the sparse plastic deltas (§6.3 Snapshot); the full form is used internally and for the framed host API if you want one (re-derive each field explicitly — never UE `SerializeBin`).

### 6.6 Snapshot / delta / authority model

- **Pose** = unreliable, newest-wins, full absolute (no delta baseline), client-interpolated.
- **Impacts** = one-shot events.
- **Detaches** = events + carried in snapshot state (server-decided; a `Replica` never self-decides — `autonomousDamageDecisions=false`).
- **Damage** = durable state, pushed only when dirty, ≤ `snapshotPushInterval`. A clean race sends nothing (grid-scaling).
- **Repair rides the snapshot's `repairCounter`**, never a bare event (dropped-repair safety).
- Dirty tracking: `getDamageChangeToken()` / `isDamageDirtySince(token)`; capture with `deltaOnly` clears the token.
- Delta-baseline/ACK exist only as a declared contract (`deltaBaseline` profile flag) — not yet implemented; current damage sends are full end-state.
- Authority roles `{Standalone, Authority, Replica}`, auto-detected from session state (no `GetNetMode`).
- Sequence clock: monotonic `int32 nextSeq()` stamped on outbound payloads for cross-actor ordering/dedup.

Server tick shape the host loop must provide (from `UOCNetAuthoritySubsystem`, re-expressed UE-free):
```
serverApplyInbound(recvBytes)     # decode+apply this tick's client msgs (Input/Repair/Config)
world.step(dt)
serverCollectOutbound(sendBytes)  # one batch: impacts + detaches + repairs + dirty snapshots
```
Server flood control: per-session packet-rate token bucket, impact-rate bucket, pose plausibility (teleport/speed-hack) gate, NaN/Inf poison rejection. Poses broadcast at `poseSendHz` (< tick), timing at `raceStateSendHz`, both decoupled from tick.

### 6.7 Modular channel framework (build the port around this)

Re-implement the successor architecture as plain C++:
- `NetPayload { FName channel; u8 type; u8 flags; i32 seq; std::vector<u8> bytes }` — the only thing crossing the seam; transport never inspects `bytes`.
- `ChannelProfile { channel; reliability{Unreliable,Reliable,State}; targetHz; relevancy{Always,DistanceScaled,OwnerOnly}; deltaBaseline; basePriority }`.
- `NetModule` = one concern owning one channel: `applyNetCommand/validateNetCommand/drainNetEvents/isNetDirty/captureNetState/restoreNetState` + esports hooks `predictTick/reconcile/interpolate` (no-ops today).
- `NetCoordinator` hosts modules, exposes `applyCommand/drainEvents/captureState/restoreState` + the seq clock.
- Built-in vehicle modules: **Drive** (`Cmd_SetInput=0`), **Damage** (`Cmd_ApplyImpact=0, Cmd_DetachPart=1, Cmd_Repair=2`), **Config** (`Cmd_ApplyConfig=0`). Channels `Drive/Damage/Config`.

Frame it with Protocol (A) for OCServer interop; the internal module system stays transport-agnostic.

### 6.8 Interop checklist (existing OCServer/OCCoordinator must keep working)

1. Reproduce Protocol (A) exactly: MsgType byte values, field order/width/endianness, no packet header, MTU ≤1400, pose paging, damage fragmentation ≤1200B, `[u16]`/`[u8]` string conventions, additive-trailing-field rule.
2. Reproduce the coordinator protocol: CoordMsg values, 2 s queue keepalive, auto-connect-with-token handoff.
3. Join parity fields: `protoVer=1`, `mapContentId(u64)`, `hashAlgo` (default Sha256=3), `32B mapRoot`, `buildId`; mismatches → `RejectReason`. (Parity/compat gate, not the cheat boundary.)
4. Client-authoritative relay semantics: send `ClientPose`, expect `PoseBatch`; author own damage, relay seeds/snapshots.
5. Identity: `vehicleId` server-assigned `int32` from 1, used as `sourceCarId`/`carId` everywhere.

Reuse verbatim (already UE-free): `oc_sim` (physics interop ABI), `oc_match` (matchmaking core), the entire C# `OCServerHost`/`OCCoordinatorHost` incl. both `Wire.cs`. Port the client to speak them — do not reinvent.

### 6.9 Stripped UE deps (net) → replacements

| UE | Replacement |
|---|---|
| `USTRUCT/UCLASS/UENUM/UPROPERTY/UFUNCTION/UObject` reflection | plain structs/enums/classes |
| `FVector_NetQuantize100/10/Normal`, `Engine/NetSerialization.h` | plain vec3 + explicit quantization (cm×100 etc.) |
| `FMemoryWriter/Reader`, `FObjectAndNameAsStringProxyArchive`, `StaticStruct()->SerializeBin` (whole `OCNetWire` codec) | explicit little-endian field marshalling (the Wire.cs model) |
| `FSocket/FInternetAddr/ISocketSubsystem` | BSD sockets / asio / enet |
| `FTSTicker/FTickerDelegate` | own timer/loop |
| UE replication (`UFUNCTION(Server/NetMulticast)`, `DOREPLIFETIME`, `ReplicatedUsing`, `ForceNetUpdate`, `ENetRole/HasAuthority/GetNetMode`, `SetReplicateMovement`) | own net stack; role from session state |
| `DECLARE_DYNAMIC_MULTICAST_DELEGATE*` | `std::function` callbacks |
| `UActorComponent/UWorldSubsystem/UGameInstanceSubsystem` | your systems |
| `FCarBuild/FLivery/TSoftObjectPtr<UObject>` | engine-neutral build POD + content-id registry |
| `GFrameCounter/UE_LOG/FMemory::Memcpy/FTCHARToUTF8` | std equivalents + UTF-8 conv |
| Blueprint bindings (`BlueprintCallable`, `TSubclassOf<APawn>`) | native API + your pawn types |

---

## 7. Cross-module data flow summary

```
                 offline (Rust aver.pipeline + C++ importer)
   .ocbeam/.ocaero/.glb ──parse+resolve+bake──► CageAsset (binary "bytecode")
                                                  │ nodes/beams/panels/parts/materials
                                                  │ per-part mesh + PBR + textures (native .ocmesh/.ocmat/.octex)
                                                  │ baked aero grid + skeleton/skin/anim + shard meshes
                                                  ▼
   runtime load ──► SoftBodySolver.loadFromAsset ─► initSolver(structuralStrength)
                                                  │
   collision layer ──onHit/reportImpact──────────┤  (only world<->local coupling)
                                                  ▼
                            SolveConfig ──► tick() ──► [sync inline]  or  [async worker: private copy, snapshot swap]
                                                  │
             ┌────────────────────────────────────┼───────────────────────────────────┐
             ▼                                    ▼                                     ▼
    getCageDisplacements                 onPartDetached / onRepaired            getPlasticState /
             │                                    │                              getBrokenBitset / repairCounter
   flatten ► NodeDisp buffer            aero.detachedParts / debris spawn                │
   compute dispatch (per section)                 │                          DamageSnapshot (durable end-state)
   OutPositions (UAV=position VB) ► draw          │                                      │
             │                          aero.applyForces(at CoP, *Q)          Protocol(A) Snapshot (sparse plastic mm)
             ▼                                    ▼                                      ▼
        renderer                          rigid-body (aero + rebound)          UDP ► OCServer ► PoseBatch/Impact/Snapshot
```

Two invariants hold this together: **the CPU `DeformedVert` and the GPU `MainCS` produce identical deformed positions** (blend→mirrorY→gain→clamp→rest+disp, weights pre-normalized), and **every peer runs the identical deterministic `solveStep` from the same crash seeds** so dents match without shipping node state.

---

## 8. Open items flagged for implementation (not invented — carried from recon)

1. **`oc_sim` is a P0 stub** (forward-motion only). The real rigid-vehicle physics (suspension/tire/drivetrain/collision) is P2, to be extracted UE-free from `UOCVehicleMovementComponent` into `aver.phys.vehicle` behind the preserved `oc_sim` ABI so client and server link the same source. The soft-body damage solver above is the separate, fully-specified piece.
2. **Canonical scale constant**: pick one (`0.01` m→cm) and assert it at import; the two doc statements ("glTF metres ×100" vs "SCALE=0.01") are reciprocals — bake the choice into `aver.pipeline`.
3. **Keep both render paths** until GPU deform reaches parity; the `.usf`→DXC math contract in §5.2 is the spec if both are kept.
4. **`DebrisMaxDrop`/`MaxBoneDisplacement`** are mesh/bone-drive clamps, not `solveStep` terms — apply them in the render/deform layer, not the physics core.
5. **ROOT hash in `.ocmap`** is a placeholder (hashes only the name); the intended design is a Merkle hash over placements + asset content — implement the real one when the asset library exists, and use invariant/C locale + consistent LF line endings in any writer.