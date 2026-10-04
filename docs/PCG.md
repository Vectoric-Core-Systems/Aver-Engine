# Procedural content generation

The PCG API, as it stands at `bac1d87`. Two of its three paths are wired end to end and demonstrated working; the third is a type contract and a CPU reference with nothing consuming it yet. Read the status line at the top of each path before assuming it does something.

## 1. The split, and why

F# generates, C# applies. `scripting/fsharp/Aver.Pcg/Pcg.fs`:

> The general PCG API. Clouds are one consumer of this, not its design.

The division was settled before either half was written. F# owns generation and orchestration because procedural content is pure functions over immutable data, which is what F# is good at. C# owns application because that is where the engine's object model already lives — `ActorClass`, `Entity`, `Actors.Spawn` — and reimplementing that in F# would buy nothing. `Aver.Pcg` has **no reference to `Aver.Scene`** and must never gain one: it has to stay testable with nothing else running, and the moment it references scene types it can only run inside a live host.

Everything crossing the F#→C# boundary is a pure function of its arguments, and every boundary type is deliberately boring:

```fsharp
// Types.fs
type Vec3 = { X: float32; Y: float32; Z: float32 }
type Quat = { X: float32; Y: float32; Z: float32; W: float32 }
type Bounds = { Min: Vec3; Max: Vec3 }
type Placement = { Position: Vec3; Rotation: Quat; Scale: Vec3; Asset: string }
```

Records of primitives and arrays. No F# lists, no options, no discriminated unions, no tuples — those are excellent F# and poor C#: `FSharpList<T>` has no C# literal, `FSharpOption<T>` is not a C# nullable, and a tuple crossing the boundary arrives as `System.Tuple` with `.Item1` fields. F#'s expressive types are for F#'s inside; the boundary is arrays and plain records.

**Units, stated once because getting it wrong looks like a different bug.** Every distance is **centimetres**, `+Z` is up, the basis is left-handed — the engine's contract, not a PCG-specific one. A generator that thinks in metres silently produces a world 100× too small, and it reads as a scale bug in the renderer, not a units bug in the rules that actually caused it.

## 2. Determinism

`Aver.Pcg.Rand` (`Pcg.fs`) is the seed contract the whole API rests on: `splitmix32`, hand-written, not `System.Random`. That is not a style preference — `System.Random`'s algorithm is undocumented and has already changed once (.NET Core 2.x vs 3.0+ produce different sequences from the same seed). "Same seed, same world" is the entire promise of seeded PCG; building it on a sequence Microsoft is free to change means a saved seed can silently generate a different world after a runtime update.

`Rand` is also **stateless** — no cursor to advance — so generation can run in any order, in parallel, or partially, and still agree. `hash2 seed i` and `hash3 seed x y z` are order-independent by construction; a value at one lattice coordinate depends on nothing but the seed and that coordinate, never on what was generated before it.

**How to check a generator is actually seeded**, the pattern `PcgScatterBehaviour.cs` uses and any new rule should copy: generate twice with the same seed and compare **bit for bit** (not within an epsilon — a tolerance hides exactly the drift being checked for), then generate once more with a different seed and confirm it disagrees. The second check exists because the first one passes vacuously for a generator that just returns a constant array.

## 3. Path 1 — scatter / placement

**Status: wired end to end.** F# generates, `Aver.Pcg.Apply` spawns real entities through the existing scripting API. No engine ABI involved — `Actors.Spawn` already existed.

```fsharp
// Pcg.fs
type ScatterSpec =
    { Region: Bounds; Count: int; Seed: int; Assets: string[]
      RandomYaw: bool; ScaleMin: float32; ScaleMax: float32; MinSpacing: float32 }

module Pcg =
    let scatter (spec: ScatterSpec) : Placement[]
```

`scatter` throws `Count` points uniformly through `Region`, using four **independent hash streams per placement** (position×3, plus one for yaw/scale/asset selection) — distinct index spaces rather than a running counter, so a future fifth stream cannot shift the ones already in use. `MinSpacing` is enforced by rejecting a candidate against everything already accepted, in a **fixed index order** — O(n²), and honestly so: fine for the hundreds-to-low-thousands a hand-authored scatter uses, a Poisson-disc grid is the answer above that. The returned array is usually **shorter** than `Count`; a spacing constraint that always returns exactly what was asked for isn't enforcing anything.

```csharp
// Aver.Pcg.Apply / PcgApply.cs
public static ApplyResult Apply(Placement[] placements, List<Entity>? spawned = null)
```

Reads `Placement.Asset` as an **actor class name** — a class is what the framework can spawn — and resolves it with `ActorClass.Find`, cached **once per distinct name**, not once per placement (a scatter of ten thousand rocks names the same class ten thousand times, and `Find` crosses into native code every call). A placement naming an unknown class is counted, never guessed at: `ApplyResult.UnknownClass`/`EmptyAsset`/`Spawned` say exactly what happened, because a generator that quietly produces half a forest is worse than one that says so.

**Worked example:** `Aver.Pcg.SampleRules/Forest.fs` (rules) + `Aver.Scripting.SamplePcg/PcgScatterBehaviour.cs` (the whole apply-and-report loop in one file — comment: "if a generation rule ever appears here it is in the wrong file"). Run it with `Sandbox.exe --scripts PcgScripts`. `Forest.trees`/`Forest.undergrowth` are two separate `scatter` calls sharing one master seed with a large, constant offset between them (`seed + 7919`) — a constant, not `seed + 1`, because adjacent seeds correlate under most hashes and two layers built on neighbouring seeds would grow suspiciously near each other.

## 4. Path 2 — infinite fields, and the sky

**Status: wired end to end.** This is the sky's cloud layer, and currently the *only* consumer of `InfiniteSpec`.

```fsharp
// Types.fs
type InfiniteSpec = { Seed: int; Layers: NoiseLayer[]; CellSizeCm: float32
                       CoverageFloor: float32; CoverageBias: float32 }
module Pcg =
    let sampleInfinite (spec: InfiniteSpec) (wx: float32) (wy: float32) (wz: float32) : float32
    let infiniteExpanse (seed: int) (cellSizeCm: float32) (octaveCount: int) : InfiniteSpec
```

An `InfiniteSpec` has **no region and no resolution** — it is defined at every world position, including negative ones, forever. That is what makes seeded chunk generation possible in principle: because the value at a position depends on nothing but the seed and that position, two neighbouring regions generated independently, on different threads, a year apart, still agree at the seam. (One sharp edge: `sampleInfinite` floors its lattice coordinate rather than truncating, specifically so cells `-0.5` and `+0.5` don't both collapse to cell `0` — truncation toward zero would double the width of the one cell straddling the origin, a seam visible at world zero and nowhere else.)

**What actually crosses into the renderer is much smaller than the field itself, and the gap matters.** The cloud raymarch (`modules/rhi/src/RHIShaders.cpp`) has its own value noise; it does **not** evaluate `InfiniteSpec` per sample. What crosses is the field's **seed** (hashed into a translation of the renderer's own noise domain, so two seeds give two visibly different skies) and a **coverage** scalar measured by sampling the field on a coarse lattice and counting what clears `CoverageFloor`. The F# graph *parameterises* the sky; it does not replace the renderer's noise. Tuning octaves in a `SkySpec` expecting to see them rendered directly is the mistake this gap produces.

**The ABI seam** — `modules/framework/include/aver/framework/framework_abi.h`, minor version 1→2, additive only:

```c
void    aver_fw_set_sky_clouds(int32_t seed, float coverage, float density,
                               float bottomCm, float topCm, float featureScale,
                               float windXCmPerSec, float windYCmPerSec);
int32_t aver_fw_sky_clouds(int32_t* outSeed, float* outCoverage, /* ... */);  // 1 if published, 0 if not
void    aver_fw_clear_sky_clouds(void);
```

**A request, not the truth.** Until a script calls the setter, `aver_fw_sky_clouds` returns `0` and the host keeps whatever the level authored — a project with no sky script renders exactly as it did before these entry points existed. The C# wrapper is `Aver.Framework.Sky.SetClouds`/`Clear` (`scripting/csharp/Aver.Framework/Sky.cs`); the host reads it back **once a frame**, so hot-reloading the F# moves the sky live without a restart.

**There are two independent routes to a level's clouds, and only one goes through any of this.** A level's own `PCGVOLUME name Sky` record in its `.ocworld` is read directly by the host (`GameApp::pushFrame`, per frame, and the editor's `SandboxApp::loadLevel`, once at load) — by name, specifically `"Sky"`, since a level may declare other fields (a cave mask, a moisture field) that must not be mistaken for the sky. Its `coverageFloor` is inverted into coverage the same way the F# path measures it: a density floor is the threshold *below* which the field is empty, so a *higher* floor is *less* material surviving — less cloud, not more. **A script's published request wins over the level's own record when one exists**; nothing published, and the level's record still drives it exactly as before F# entered the picture.

**How a project gets one:** `sandbox/src/ProjectScaffold.cpp::skyScriptText` writes a per-project `Content/Sky.fs` (`module <ProjectName>.Sky`) on project creation — a `spec` record, `field()`, `measureCoverage()`, and an `Apply()` that calls `Aver.Framework.Sky.SetClouds`, meant to be called once from the game mode's `OnBeginPlay`. `Aver.Pcg.SampleRules/Sky.fs` is the fuller reference this scaffold is drawn from, including the `MeasureCoverage` derivation with its "why 16×16, not finer" reasoning (it runs once at level load on the game thread, and a finer grid costs real milliseconds to move a number too small to see). The scaffolded file is optional — `Scripts.csproj` references `Scripts.FSharp.fsproj` only when it exists, so deleting `Sky.fs` is a supported way to opt out, and the C# side still builds.

## 5. Path 3 — bounded volumes

**Status: type contract and a CPU reference. No consumer.** Do not write a rule against this path expecting something to render; nothing reads a `VolumeSpec` today outside its own test.

```fsharp
// Types.fs
type VolumeSpec = { Region: Bounds; ResX: int; ResY: int; ResZ: int; Seed: int
                     Layers: NoiseLayer[]; CoverageFloor: float32; CoverageBias: float32 }
module Pcg =
    let densityVolume (seed: int) (region: Bounds) (res: int) (layers: NoiseLayer[]) : VolumeSpec
    let sampleDensity (spec: VolumeSpec) (x: int) (y: int) (z: int) : float32
```

The design intent, stated in `Types.fs`: **F# owns the rules, a compute shader owns the volume.** A 128³ field is 2 million voxels, 256³ is 16 million, and filling those in idiomatic F# means allocating per voxel — fast enough, and also not F# worth writing. `VolumeSpec` is meant to be kilobytes of *parameters* that a GPU fills, the same relationship the physical atmosphere has to its HLSL mirror.

`sampleDensity` is explicitly **not that GPU path** — its own remark: "a contract, not a renderer. Nothing should fill a volume by calling this in a loop." It exists so a future compute-shader implementation has something to be checked *against*: same spec, same coordinate, same density on both sides, the way `AtmosphereTest` mirrors the atmosphere model between C++ and HLSL. **That compute shader does not exist yet**, and neither does an ABI seam to reach it — building both is the open half of this API. Until then, `VolumeSpec`/`densityVolume`/`sampleDensity` are exercised only by their own unit coverage.

## 6. Writing a new rule

1. New rules go in F#, referencing `Aver.Pcg` — not `Aver.Scene`, not the framework. If a file needs an entity or a device, generation has leaked into application.
2. Prefer `Pcg.scatter` (placement) or the `Sky` pattern (infinite field) over hand-rolled noise — both already carry the determinism contract in §2.
3. Check determinism the way §2 describes: two identical seeds must agree bit for bit, two different seeds must disagree. `PcgScatterBehaviour.cs` is the copyable pattern.
4. To spawn the result: `Aver.Pcg.Apply.PcgApply.Apply(placements)`, and declare the actor classes your `Asset` strings name — `ApplyResult.UnknownClass` is silent-forest debugging built in, read it before assuming nothing generated.
5. To drive the sky: edit the project's scaffolded `Content/Sky.fs`, or replace it with your own module that ends in an `Apply()` calling `Aver.Framework.Sky.SetClouds`.

## Related

`docs/SCRIPTING_API.md` for the C# surface `Aver.Pcg.Apply` and rules built on it actually run against. `docs/ABI.md` for every other C seam, and where `aver_fw_*_sky_clouds` sits among them. `docs/CHUNKS.md` for why a per-position-only field (§4's `InfiniteSpec`) is the property that makes seeded, streamed generation possible in principle — chunk streaming itself is unbuilt; nothing in this document depends on it existing.
