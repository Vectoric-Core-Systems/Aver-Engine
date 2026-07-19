# OpenConstructor27 — North-Star Engine Conventions (extracted from source)

Faithful spec for re-implementation in a permissively-licensed C++ engine. Every non-obvious claim is cited `file:line`. Ambiguities are flagged inline. "UE dependency" = must be provided natively by the new engine.

Sources read: `OpenConstructor27.uproject`, `Source/OpenConstructor27/OpenConstructor27.Build.cs`, `CLAUDE.md`, `Docs/ART_PIPELINE_SPEC.md`, `OCSimCore/{include/oc_sim.h, src/oc_sim.cpp, src/math/Vec3.h, CMakeLists.txt}`, `Source/.../Net/OCNetTypes.h`, `Plugins/OCShaders/*`, `ATTRIBUTIONS.txt`.

---

## 1. World coordinate system, units, handedness (AUTHORITATIVE)

All three subsystems (sim core, art pipeline, on-disk formats) agree on **one** frame. This is the single most load-bearing convention.

| Property | Value | Source |
|---|---|---|
| Length unit | **centimetre** (1 unit = 1 cm); model at real-world scale | `ART_PIPELINE_SPEC.md:32`; `oc_sim.h:31`; `Vec3.h:1` |
| Up axis | **+Z up** | `oc_sim.h:31`; `ART_PIPELINE_SPEC.md:34,191`; `CLAUDE.md:270` |
| Vehicle-local axes | **+X forward, +Y right, +Z up** ("UE vehicle convention") | `ART_PIPELINE_SPEC.md:191`; `CLAUDE.md:270` |
| Engine bank-local axes | **+X crank axis (engine length), +Y bank width (outboard), +Z up** | `ART_PIPELINE_SPEC.md:51-58` |
| Handedness | **Left-handed** — the sim's `OCVec3`/`Vec3` "mirror FVector semantics"; `Cross` is the standard formula (`Vec3.h:22`), so it inherits UE's left-handed Z-up interpretation | `oc_sim.h:31`; `Vec3.h:1,22` |
| Vector type (sim) | `struct OCVec3 { float x,y,z; }` (C ABI) / `oc::Vec3` (C++), float32, no UE dep | `oc_sim.h:32`; `Vec3.h:8-26` |
| Velocity / accel units | **cm/s**, **cm/s²** | `oc_sim.cpp:23,71` |

**Cross-check against the formats (all consistent):**
- `.ocbeam`: "cm, vehicle local, Z-up (X fwd/length, Y right/width, Z up)" (`CLAUDE.md:270`).
- `.ocaero`: "Coords/axes match `.ocbeam` (cm, vehicle-local, X fwd / Y right / Z up)" (`CLAUDE.md:364-365`).
- Net wire: crash-seed `WorldPoint`/`WorldNormal` are **world space**, cm, un-mirrored (`OCNetTypes.h:108-122`).

**Import handedness conversion (the one gotcha to port).** Authored content comes from glTF/Blender (right-handed, Y-up). Two conversions exist and must be reproduced exactly:
- **Axis conversion at import**: glTF Y-up → engine Z-up is `(x,y,z) → (x,-z,y)`, with `SCALE=0.01` (Blender metres → cm) (`CLAUDE.md:272-273`). Note the art spec says glTF is metres → UE ×100 (`ART_PIPELINE_SPEC.md:33`).
- **A separate render/physics Y-mirror** (`bDeformMirrorY`) corrects a "cage/rig handedness mismatch from the Blender/glTF export" (`CLAUDE.md:96`). Impact point/dir is flipped on Y (`CLAUDE.md:96`); cage-space→actor-space conversion flips Y, negates `normal.Y`, and **reverses triangle winding** or the body renders inside-out/backface-culled (`CLAUDE.md:140-143,638-641`). On the wire this mirror is deliberately **not** pre-applied — each client applies its own (`OCNetTypes.h:108-110`).

**Force / speed units (secondary but authoritative):**
| Quantity | Authored unit | Engine-internal | Source |
|---|---|---|---|
| Aero force | **Newtons** at `ReferenceSpeedKmh` | ×100 → UE `kg·cm/s²` (`NEWTONS_TO_UE`) | `CLAUDE.md:235-239,250-251` |
| Speed (aero/wire) | **km/h** | scaled `(speed/ref)²` | `CLAUDE.md:236`; `OCNetTypes.h:120` |
| Material stiffness | `AxialStiffness` **N/cm**, `BendForceN`/`BreakForceN` **N**, `MaxBend` **cm** | force = `stiffness × absolute deformation (cm)`, never strain-normalized | `CLAUDE.md:296-300,604` |
| Fracture energy | `NormalImpulse.Size()` — a **Chaos impulse magnitude, not joules** | — | `CLAUDE.md:405` |

---

## 2. Naming conventions

| Domain | Rule | Source |
|---|---|---|
| C++ symbol prefixes | `OC` product prefix throughout: `UOC…`, `AOC…`, `FOC…` (`FOCVehicleId`, `UOCVehicleMovementComponent`, `AOCRigidVehiclePawn`, `AOCDebrisChunk`); cage subsystem uses `UVehicle…` (`UVehicleDamage`, `UVehicleCageAsset/Component/Factory`) | `CLAUDE.md:8-9,49-52`; `OCNetTypes.h:73` |
| Module API macro | `OPENCONSTRUCTOR27_API` | `CLAUDE.md:7`; `OCNetTypes.h:73` |
| Copyright header | `Unreal Studios` (note: unrelated to Epic; also on OCShaders) | `CLAUDE.md:7`; `OCShaders.Build.cs:1` |
| Line endings | Deliverables use **CRLF** | `CLAUDE.md:28` |
| Custom asset extensions | `.ocbeam` (cage source), `.ocaero` (baked aero table) | `CLAUDE.md:55-56` |
| Mesh part naming (art) | **File name = part name**; code matches by a **key token** (`Head`, `BaseModule_V90`, `Collector_Tubular4-1`, `FrontBumper`); `SM_` prefix allowed if the token still matches | `ART_PIPELINE_SPEC.md:42,96,283-286` |
| Skeleton↔cage binding | glTF bones named `node_<ID>` map 1:1 to `.ocbeam` NODE IDs | `CLAUDE.md:174-175,273` |
| Delivery folder layout | `/Engine/BaseModules`, `/Engine/Slots`, `/Engine/Variants`, `/Body/Panels`, `/Body/Glass`, `/Wheels`, `/Exhaust` | `ART_PIPELINE_SPEC.md:285-286` |
| Module include folders | Public subfolders `Powertrain/ Regulation/ Build/ Livery/` + `Net/` registered manually (IWYU doesn't auto-add them) | `Build.cs:15-21` |

---

## 3. On-disk format conventions (versioning / magic / delimiters / endianness)

| Format | Magic / version | Encoding | Delimiters / notes | Source |
|---|---|---|---|---|
| `.ocbeam` (cage source) | optional first line **`OCBEAM 1`** (tolerated/ignored by both UE and Java loaders) | **text**, UTF/ASCII | comma-separated fields; optional single trailing `;` stripped; `#` = comment; sections `MATERIAL{} NODE{} BEAM{} PANEL{} PART{}` + optional `GLBXFORM`/`GLB{}` and `BONE{}/SKIN{}/ANIM{}` | `CLAUDE.md:267-274,295-308,518-529` |
| `.ocaero` (baked aero) | **`OCAERO 1`** header line | text | `HEADER{}` + `PART{}`; `#` comments, optional trailing `;` | `CLAUDE.md:364-372` |
| Embedded glTF body | `GLBXFORM yup/forward/mirror` + `GLB{ ENC 1; B <base64> }` | base64 of raw `.glb` bytes inside the `.ocbeam` | import-factory-only; runtime parser skips any line starting `GLB`/`MESH`/`TEXTURE`/`RENDERMAT` | `CLAUDE.md:276-293,625-628` |
| Net wire batch | `[u8 version=1]` then `[i32 count]`, then `([u8 type][i32 id][i32 len][bytes])*` | **UE binary** (`FMemoryWriter`), native little-endian x64 | same-engine-version peers only; `count`/`len` sanity-bounded; malformed frame dropped whole | `OCNetTypes.h:242,299-338` |
| Sim ABI | `oc_abi_version()` returns **1**; host asserts match at startup | C ABI | struct return uses **out-pointer** (`oc_vehicle_position`) to avoid Win-x64/SysV/ARM64 ABI mismatch | `oc_sim.h:45-66`; `oc_sim.cpp:33` |
| `.uproject` | `FileVersion: 3`, `EngineAssociation: 5.7` | JSON | — | `OpenConstructor27.uproject:2-3` |

**Compiled-asset "bytecode":** `UVehicleCageAsset` (`UDataAsset`) is the compiled product of a `.ocbeam` — analogy in the code is `.java → javac → .class` (`CLAUDE.md:114`). It stores resolved cage arrays + embedded per-part mesh (`PartMeshes[]`) + textures/materials as subobjects (`CLAUDE.md:50,282-283`).

**Record cross-references (must stay stable across the port):**
- `FOCVehicleId.Value` (int32, 0 = invalid) is **the** vehicle key — same id used by the sim (`oc_world_add_vehicle(id)`, `oc_sim.h:51`), the net layer (`OCNetTypes.h:69-83`), and the wire (`FWireMsg.Id`, `OCNetTypes.h:290`). Deliberately **not** UE's `FNetworkGUID` so an external server can address it (`OCNetTypes.h:69-71`).
- `.ocaero` `PART.Name` ↔ `.ocbeam` `PART` name ↔ aero `LinkedPartName` ↔ `FOCDetachEvent.PartIndex` (index into cage-asset `Parts`) — the chain that couples damage to aero (`CLAUDE.md:372,240-243`; `OCNetTypes.h:132`).
- `.ocbeam` NODE `<ID>` ↔ glTF bone `node_<ID>` (`CLAUDE.md:273`).

---

## 4. The cook / DDC model

**Loose text → compiled `.uasset`, resolved once at import.**

| Stage | What happens | Source |
|---|---|---|
| Source | `.ocbeam` (text) is the source of truth; `.ocaero` similarly | `CLAUDE.md:55,114` |
| Compile (editor only) | `UVehicleCageFactory` (a `UFactory`, `#if WITH_EDITOR`) parses + validates + reference-resolves via the **single shared** `UVehicleDamage::ParseOcbeam`, and bakes resolved cage arrays into `UVehicleCageAsset`; right-click **Reimport** re-reads edited source | `CLAUDE.md:114-124,622-624` |
| Runtime load | `UVehicleDamage::LoadCageFromAsset` **copies** the asset arrays (no parse) when `CageAsset` set; falls back to parsing the loose `.ocbeam` by path otherwise | `CLAUDE.md:118-119` |
| Cook/package | The asset cooks/packages automatically through the BP's `TSoftObjectPtr` — **loose `.ocbeam` files would not cook** | `CLAUDE.md:120-121` |
| Shareability | Asset stores **strength-neutral** beams; `StructuralStrength` applied per-vehicle in `InitSolver`, never at parse/import → one asset is shareable | `CLAUDE.md:121-123,622-624` |
| Editor-only fracture bake | `OC27FractureLibrary` mirrors the Fracture editor's Voronoi tool (Fracture not exposed to Python in UE 5.7) | `Build.cs:57-72` |

There is no custom DDC keying documented; caching is UE's standard asset/cook pipeline plus this one-time import-compile step. **Port note:** the new engine needs an offline compiler (`.ocbeam`/`.ocaero` → binary asset) and a runtime loader that only reads the compiled form.

---

## 5. How client and server share data — "author-once / consume-by-both"

This philosophy appears in **four** distinct places; all are load-bearing for the port.

**(a) Physics core shared by linking the same source (the strongest form).** `OCSimCore` is a "UE-free authoritative vehicle SIM core (C ABI)" — the C# server host P/Invokes it and **"the UE client links the SAME source so client + server run identical physics"** (`oc_sim.h:1-9`). It is dependency-free (stdint / `<cmath>` only) so it compiles into UE **and** the standalone server (`oc_sim.h:10`; `Vec3.h:2`; `CMakeLists.txt:1-4`). Determinism is explicit: `oc_world_step` is "Deterministic given inputs" (`oc_sim.h:59`).
- Current state is a **P0 stub** (trivial forward-motion) proving the C#↔C++ interop; the real port (suspension/tire/drivetrain/rigid-body/collision, extracted UE-free from `UOCVehicleMovementComponent`) is **P2** (`oc_sim.h:8-10`; `oc_sim.cpp:1-6,66-67`).
- Build outputs one name on every OS: `OCSimCore.dll` / `libOCSimCore.so` / `.dylib`, matching `DllImport("OCSimCore")` + `SimCoreLoader` (`CMakeLists.txt:2-4,22-25`). Only the C ABI is exported (visibility hidden) (`CMakeLists.txt:18-25`).

**(b) One net contract, two transports.** `OCNetTypes.h` structs are "the ONLY things that travel between the simulation and a server," plain data, no raw UObject pointers (`OCNetTypes.h:13-31`). Same structs serve: UE-native dedicated-server (RPC params / replicated `UPROPERTY`, using `FVector_NetQuantize*` for packing) **and** an external process (`OCNetWire::ToBytes/FromBytes` + framed `WriteBatch/ReadBatch`) (`OCNetTypes.h:23-30,240-353`).
- **Reconstruct-don't-send:** dents/audio/aero are rebuilt locally from *(config + impacts + replicated movement)*, never transmitted (`OCNetTypes.h:18-21`). The damage snapshot is an **end-state**, never an impact log (replaying would double-apply) (`OCNetTypes.h:153-156`).
- Authority model: `EOCNetRole {Standalone, Authority, Replica}` — a Replica **never** self-decides detach, only obeys `Cmd_*` (`OCNetTypes.h:33-43`). Crash seeds replay through each client's deterministic local cage so dents land identically (`OCNetTypes.h:108-112`).
- Roles/quantization: input quantized to bytes (throttle/brake 0..255, steer 128=centre) (`OCNetTypes.h:89-105`); `FVector_NetQuantize100/10/Normal` for spatial fields (`OCNetTypes.h:118-133`).

**(c) One mesh, both rendered and simulated.** Every part compiles to a **Vehicle Cage Asset** that *both renders the part and is its damage cage* (`ART_PIPELINE_SPEC.md:16-21,79-88`). Artists deliver plain meshes; one command (`glb_to_ocbeam.py`) or Studio compiles them; the file name becomes the part name (`ART_PIPELINE_SPEC.md:79-96`).

**(d) Author-once part kit → any layout.** Engine parts are authored **once** (one bank / one cylinder) and the code arrays/mirrors/tilts/**bore-scales** them into I3…V12/Flat; swapping a placeholder for a real model is "a data change, not code" (`ART_PIPELINE_SPEC.md:6-8,68-69`). Uniform bore-scale = `chosen_bore / 86mm`; stroke is displacement-only and never scales the mesh (`ART_PIPELINE_SPEC.md:63-66`). Aero is authored once as a cage, baked by the OCAERO wind tunnel to `.ocaero`, and consumed identically by the Studio tunnel and the in-game `UVehicleAerodynamics` (`CLAUDE.md:245-265,354-360`).

---

## 6. Current rendering stack

**Body deformation has two render paths (both must be ported or one chosen):**

| Path | Component | Mechanism | Source |
|---|---|---|---|
| CPU (recommended/working) | `UProceduralMeshComponent` / `UVehicleCageComponent` | nearest-K, inverse-distance cage-node skin; `UpdateMeshSection_LinearColor` (no-topology-change fast path); `vertex = rest + Σ(weight·node displacement)`, clamped by `MaxBoneDisplacement`, scaled by `DeformGain` | `CLAUDE.md:98-111,132-145` |
| GPU (in progress) | `UGpuVehicleCageComponent` | custom `FPrimitiveSceneProxy` + `FLocalVertexFactory` + compute shader `FVehicleDeformCS` writing the vertex buffer (UAV) | `Build.cs:31-40`; `VehicleDeform.usf:1-80` |

- **`OCShaders` plugin**: sole job is registering the `/OCShaders` **virtual shader directory** at `PostConfigInit` (before global-shader compilation) and hosting the deform compute shader (`OCShaders.uplugin:6,12-18`; `OCShaders.Build.cs:5-8`). A global shader **must** register from a PostConfigInit module or it asserts `!AreShaderTypesInitialized` (`Build.cs:35-37`). Must load in Editor, Game, and cook commandlet (`OCShaders.Build.cs:7-8`).
- **Compute shader parity contract**: `VehicleDeform.usf` must match `UVehicleCageComponent::DeformedVert` byte-for-byte — weights pre-normalized at bake (sum, don't re-divide); skip `ni<0 || ni>=NumNodes`; order = *blend → mirrorY → ×Gain → clamp-to-`MaxD` → rest+disp* (`VehicleDeform.usf:8-11,57-79`). This is the CPU/GPU "author-once" invariant.
- **Materials**: built with the **runtime** Engine material API (`GetExpressionCollection().AddExpression` + `FExpressionInput::Connect` + `PostEditChange`) — **never** `UMaterialEditingLibrary::RecompileMaterial`, which rebuilds editor UI and **crashes headless import** (`CLAUDE.md:33-35,633-637`). Source materials are glTF **metallic-roughness PBR**: BaseColor = baseColorTexture×factor, Metallic/Roughness from MR texture B/G × factors, normal, emissive, alpha mode (`CLAUDE.md:161-171`).
- **VFX**: `Niagara` for carbon-dust / glass-mist / spark bursts on shatter (`Build.cs:27`).
- **Cesium**: the `CesiumForUnreal-57-main` plugin **is present on disk** (`Plugins/`) but is **NOT enabled in `OpenConstructor27.uproject`, NOT in `Build.cs`, and NOT referenced anywhere under `Source/`** (verified by grep — 0 hits). `ATTRIBUTIONS.txt:77-79,104` lists Cesium/ion as "still to check" and unconfirmed whether ion data is used. **Assessment (flagged as ambiguous):** Cesium appears **staged/dormant** — the likely intended georeferenced-world/terrain-streaming layer (cf. the Monza track built from real OSM GPS, `CLAUDE.md:466-469`) but not currently wired into the build. Do not treat it as an active dependency; treat it as a *planned* world/terrain streaming capability the new engine may need.

---

## 7. UE subsystems currently relied upon (to provide natively or strip in the port)

From `Build.cs` (authoritative for what actually links) + `.uproject` plugins. "Replace" = the new engine must provide equivalent functionality; "Strip" = editor/tooling-only, not needed at runtime.

**Runtime `PublicDependencyModuleNames` (`Build.cs:23-49`):**
| UE module | Used for | Port action |
|---|---|---|
| `Core`, `CoreUObject`, `Engine`, `InputCore` | base types, actor/component model, reflection, input | Replace with engine's core/ECS + input |
| `GeometryCollectionEngine` | GC fracture / whole-part detachment APIs | Replace: custom fracture/convex-shatter |
| `ProceduralMeshComponent` | cage-driven deformable body skin (runtime mesh) | Replace: dynamic vertex-buffer mesh component |
| `Niagara` | debris/dust/spark/glass VFX on shatter | Replace: particle system |
| `DeveloperSettings` | `ULoadingScreenSettings` (Project Settings) | Replace: config system |
| `Sockets`, `Networking` | `UOCNetSocketClient` raw UDP `FSocket` to OCServer | Replace: UDP socket layer |
| `RenderCore` | `FLocalVertexFactory`, `FStaticMeshVertexBuffers`, `FComputeShaderUtils`, shader-dir mapping | Replace: renderer/RHI abstraction |
| `RHI` | `FRWBuffer`, buffer/transition APIs for GPU deform | Replace: GPU buffer API |
| `OCShaders` (own plugin) | virtual shader dir + `FVehicleDeformCS` compute shader | Port the shader; drop the UE global-shader registration scaffolding |
| `MetasoundEngine`, `MetasoundFrontend`, `MetasoundGraphCore` | **procedurally build engine sound at runtime** (`UMetaSoundBuilderSubsystem`/`SourceBuilder`) | Replace: procedural audio graph |
| `AudioMixer` | runtime audio + live parameter set on the playing source | Replace: audio engine |

Note: `"Renderer"` is deliberately **excluded** — private engine module, breaks packaged linking (`Build.cs:33-34`).

**Runtime `PrivateDependencyModuleNames` (`Build.cs:51-55`):** `MoviePlayer` (blocking-load loading screen), `Slate`, `SlateCore` (`SLoadingScreen` widget). → Replace with the engine's UI + loading-screen system.

**Editor-only `if (Target.bBuildEditor)` (`Build.cs:60-72`):** `PlanarCut`, `Voronoi`, `Chaos` (fracture geometry/GUIDs/damage enum), `AssetRegistry`, `UnrealEd` (`UFactory`/reimport/`VehicleCageFactory`), `AssetDefinition` (Content Browser registration), `ImageWrapper` (decode embedded PNG/JPEG), `Json` (parse embedded glTF chunk). → **Strip from runtime**; reimplement as an **offline asset-compiler toolchain** (glTF+JSON parse, image decode, Voronoi fracture bake). `ParseOcbeam` and `OcGltf::Parse` are already UE-light and portable; the `UFactory`/`UAssetDefinition` wrappers are pure UE tooling.

**Plugins enabled in `.uproject` (`OpenConstructor27.uproject:16-72`):**
| Plugin | Status / port note |
|---|---|
| `OCShaders` | own plugin — port the compute shader |
| `ChaosVehiclesPlugin`, `ChaosModularVehicle` | **enabled in uproject but `ChaosVehicles` was removed from `Build.cs` 2026-07-05** — vehicle is now 100% the custom cage pawn (`AOCRigidVehiclePawn` + `UOCVehicleMovementComponent`) (`Build.cs:43-44`). *Ambiguity:* uproject still enables these plugins; `CLAUDE.md:8-9` still describes the test car parented to Chaos `WheeledVehiclePawn`. Treat as **mid-migration** — the port's vehicle sim is the custom `UOCVehicleMovementComponent`/OCSimCore, not Chaos Vehicles. |
| `GeometryScripting`, `ModelingToolsEditorMode`, `PlanarCut` | editor mesh/fracture tooling — strip to offline tools |
| `Niagara` (via `ProceduralVegetationEditor` etc.) | VFX — replace |
| `RawInput` | direct input devices (wheels/pedals) — replace |
| `WorldBLD`, `GameplayStateTree` | world/AI — evaluate need |
| `ProceduralVegetationEditor` | foliage tooling — strip |
| `HttpBlueprint` | HTTP from BP — replace if used |
| `McpAutomationBridge` | dev-automation bridge — **strip** (not shipped) |
| `UnrealClaude` | **disabled** — ignore |
| `CesiumForUnreal` (on disk only) | **not enabled** — dormant georeferenced-world layer (see §6) |

**Also referenced in code / notes (not in Build.cs default set):**
- `FieldSystemEngine` — "add only if new Chaos/field link errors appear" (`CLAUDE.md:35`).
- Chaos rigid body for the chassis: "chassis is rigid Chaos; cage deform is cosmetic" (`CLAUDE.md:232`). The **cage solver itself is custom** (Verlet mass-spring + Gauss-Seidel, `CLAUDE.md:82-90`) — **not** Chaos Cloth/Flesh (explicitly rejected: elastic, surface-only, no plastic dents, `CLAUDE.md:20-24`). Port the custom solver; replace only the rigid-body chassis integrator.
- `FVector`/`FName`/`TArray`/`TSoftObjectPtr`/`UPROPERTY` reflection, `FMemoryWriter/Reader` + `FObjectAndNameAsStringProxyArchive`, `FVector_NetQuantize*` (`OCNetTypes.h:5-9,118-133,243-256`) — the net layer leans on UE serialization + reflection; the external-transport path notes a non-UE peer must **marshal field-by-field** per the documented layouts (`OCNetTypes.h:28-30,275`).

---

## 8. Ambiguities / things the code does NOT pin down (flagged, not invented)

1. **Chaos Vehicles migration is half-done in config**: plugins enabled in `.uproject` but delinked in `Build.cs`; `CLAUDE.md` intro still describes a Chaos-parented test car while later sections describe the custom cage pawn. The authoritative runtime path is the **custom** pawn/movement component + OCSimCore.
2. **Cesium**: present but unwired; whether the world uses ion data is explicitly "still to check" (`ATTRIBUTIONS.txt:77-79,104`). Its role is inferred (georeferenced terrain), not confirmed by code.
3. **OCSimCore is a P0 stub**: it defines the ABI, units, determinism contract, and data flow, but the real vehicle physics is **not yet in the shared core** (still in `UOCVehicleMovementComponent`, UE-side) — the "identical client/server physics" guarantee is a design intent pending the P2 extraction (`oc_sim.h:8-10`; `oc_sim.cpp:1-6`).
4. **glTF unit convention stated two ways**: "glTF is metres → UE ×100" (`ART_PIPELINE_SPEC.md:33`) vs `SCALE=0.01` at import (`CLAUDE.md:273`) — these are consistent (0.01 is the m→cm reciprocal applied on the source side), but the port should pick one canonical scale constant.
5. **Two body-render paths coexist** (CPU proc-mesh vs GPU compute); GPU path is "in progress" and CPU path is the working one. The parity contract (`VehicleDeform.usf:8-11`) is the spec if both are kept.

*End of conventions extract. Key source files (absolute paths): `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/CLAUDE.md`, `.../Docs/ART_PIPELINE_SPEC.md`, `.../Source/OpenConstructor27/OpenConstructor27.Build.cs`, `.../Source/OpenConstructor27/Net/OCNetTypes.h`, `.../Plugins/OCShaders/{OCShaders.uplugin, Source/OCShaders/OCShaders.Build.cs, Shaders/Private/VehicleDeform.usf}`, `C:/Users/User/Documents/OpenConstructorSupportAssets/OCServer/OCSimCore/{include/oc_sim.h, src/oc_sim.cpp, src/math/Vec3.h, CMakeLists.txt}`.*