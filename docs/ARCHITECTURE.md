# Aver Engine — Modular Architecture Specification

**Target:** `C:/Users/User/Documents/Aver Engine` (greenfield). **Mandate:** replace Unreal for the OpenConstructor soft-body destructible racing sim using only MIT/BSD/zlib/Apache-2.0/public-domain code, with UE5+Source2-class rendering, and a strict *pay-for-what-you-use* module DAG.

Symbol/naming convention adopted: C++ types `Av*` (e.g. `AvVec3`), C ABI functions `aver_*`, C ABI structs `Av*` POD, build targets `Aver.*`, native format extensions `.oc*` (carried) / new `.oc*` (added). Copyright header: `Aver Engine`.

---

## 1. Design principles (the anti-bloat contract)

| # | Principle | How it is enforced structurally |
|---|---|---|
| P1 | **Core knows nothing engine-specific.** | `Aver.Core` depends only on the C++ standard library + a math header. It has zero knowledge of RHI, render, physics, scene, ECS, or assets. Every arrow in the DAG points *toward* core, never away. |
| P2 | **No universal base object / no runtime reflection tax.** | There is no `UObject`. Entities are 32-bit handles; components are POD in packed arrays (data-oriented). Reflection is *offline codegen* used by the serializer/editor bridge only — it costs nothing at runtime and nothing for objects that don't opt in. |
| P3 | **Editor is a separate process that drives a headless runtime through the C ABI.** | The shipping runtime links **no** editor code. The editor (C#) and tools (Rust) are downstream consumers of the C ABI; the arrow points editor→runtime, never runtime→editor. |
| P4 | **Every subsystem is an opt-in module.** | The engine boots with an empty module registry. Physics, render, net, audio, softbody, etc. each self-register only if compiled in *and* enabled. No mandatory subsystems. |
| P5 | **Build only what you ship.** | Each module is an independent CMake target behind a `AVER_MODULE_*` option. A "cage-viewer" tool and a "full multiplayer client" are two different link sets from the same tree. |
| P6 | **The cook is a plain content-addressed file cache, not a service.** | Offline Rust compilers turn source → versioned binary asset; the cache key is `hash(source ⊕ compilerVersion ⊕ options)`. No monolithic DDC daemon, no network DDC. |
| P7 | **One RHI, swappable backends.** | Renderer and all GPU code target `Aver.RHI` only. DX12/DX11/Vulkan are backend plugins selected at runtime from the compiled-in set. |

---

## 2. Layered dependency DAG (ASCII)

Arrows point to dependencies (A ──▶ B means "A depends on B"). Nothing below ever depends on anything above it. `[opt]` = optional pay-for-use module; everything else is core-tier (but only `Core`/`Platform` are *mandatory* to boot).

```
 LANGUAGE LEGEND:  (C++)  (C ABI)  [C#]  {Rust}

 ── TIER 7: OTHER-LANGUAGE CONSUMERS (out of the runtime binary) ─────────────────────
       [Aver.Editor (C#/.NET10)]      {aver-assetc / aver-ocbeamc / aver-mapc (Rust)}
       [Aver.Scripting.NET (C#)]      {aver-aerobake (Rust)}
                 │  P/Invoke                        │  FFI
                 ▼                                   ▼
 ═════════════════════ THE STABLE C ABI SEAM ════════════════════════════════════════
                          (Aver.ABI  — extern "C")
                 ▲   thin C wrappers over the C++ public module APIs
 ── TIER 6: COMPOSITION ─────────────────────────────────────────────────────────────
        (Aver.Runtime)  ── aggregator: enables + wires the compiled-in modules
              │        │         │            │            │           │
   ┌──────────┘        │         │            │            │           └─────────┐
   ▼                   ▼         ▼            ▼            ▼                      ▼
 ── TIER 5: FEATURE / BRIDGE MODULES ────────────────────────────────────────────────
 (Aver.World)   (Aver.GpuDeform)[opt] (Aver.NetVehicle)[opt] (Aver.Render.GI)[opt]
   │  │  │            │  │  │              │   │   │
   │  │  │            │  │  │              │   │   │
 ── TIER 4: SUBSYSTEMS ──────────────────────────────────────────────────────────────
 (Aver.Render) (Aver.Scene) (Aver.Physics) (Aver.SoftBody)[opt] (Aver.Aero)[opt]
 (Aver.Fracture)[opt] (Aver.Vehicle)[opt] (Aver.Net)[opt] (Aver.Match)[opt] (Aver.Audio)[opt]
      │     │        │            │             │
 ── TIER 3: BACKENDS & FORMAT RUNTIME ───────────────────────────────────────────────
 (Aver.RHI.D3D12)[opt,ON] (Aver.RHI.D3D11)[opt] (Aver.RHI.Vulkan)[opt,OFF]
 (Aver.Formats)  ── runtime loaders: native binary + legacy .oc* text fallback
      │
 ── TIER 2: HARDWARE / IO ABSTRACTIONS ──────────────────────────────────────────────
 (Aver.RHI)          (Aver.Assets)
      │                   │
 ── TIER 1: OS ──────────────────────────────────────────────────────────────────────
 (Aver.Platform)  ── window, input, files, dynlib, UDP sockets, clock, threads
      │
 ── TIER 0: FOUNDATION (depends on NOTHING engine-specific) ──────────────────────────
 (Aver.Core)  ── math, memory/arenas, containers policy, IDs/handles, jobs,
                 logging, time, hashing (fnv1a64/xxhash/blake3), strings/interning
```

Resolved edge list (every edge, to prove acyclicity):

```
Platform      -> Core
RHI           -> Core, Platform
Assets        -> Core, Platform
RHI.D3D12     -> RHI
RHI.D3D11     -> RHI
RHI.Vulkan    -> RHI
Formats       -> Assets, Core
Render        -> RHI, Assets, Core
Scene         -> Core, Assets
Physics       -> Core
SoftBody      -> Core
Aero          -> Core
Fracture      -> Core
Vehicle       -> Core, Physics
Net           -> Core, Platform
Match         -> Core
Audio         -> Core, Platform
GpuDeform     -> RHI, Render, SoftBody
World         -> Scene, Render, Physics, Assets, Formats   (+ softly: Aero/SoftBody/Vehicle when enabled)
NetVehicle    -> Net, Vehicle, SoftBody
Render.GI     -> Render
Runtime       -> (all enabled modules)
ABI           -> Runtime (wraps public APIs)
```

No target appears in its own transitive closure → the graph is a DAG. `Core` is a sink.

---

## 3. Full module table

Core-tier = compiled by default in a game client; only **Core** and **Platform** are strictly mandatory to instantiate an engine. `[opt]` modules default OFF unless noted.

| Module (target) | Lang | Depends on | Optional? | Purpose | Carries recon subsystem |
|---|---|---|---|---|---|
| **Aver.Core** | C++ | — | Core (mandatory) | `AvVec2/3/4`, `AvMat3/4`, `AvQuat`, `AvTransform`, `AvAABB`, `AvPlane` (float32, LH Z-up, cm — mirrors `oc::Vec3`); arena/pool/stack allocators; handle/slot-map; job system + fibers; `AvId` (u32, 0=invalid); interned string ids; logging; `fnv1a64`, xxHash, Blake3, SHA-256; time. | `oc::Vec3`/`Vec3.h` upgrade (adds `Dist/DistSquared/GetSafeNormal/Size`) |
| **Aver.Platform** | C++ | Core | Core (mandatory) | Window, raw input (incl. wheels/pedals via native HID), virtual filesystem + mount points, dynamic-library loader (for plugin DLLs), non-blocking UDP sockets, `steady_clock`, thread creation + priority. | UE `Sockets/Networking`, `RawInput`, `FPlatformProcess` replacements |
| **Aver.RHI** | C++ | Core, Platform | Core | Abstract render hardware interface: device, queues, command lists, PSO, root signature/descriptor model, typed & structured buffers, textures, resource-state/barrier model, `Buffer<float>`/`RWBuffer` semantics, feature-level query. Backend-agnostic. | The "ONE RHI abstraction"; GPU-deform buffer contract (typed R32) |
| **Aver.RHI.D3D12** | C++ | RHI | `[opt, default ON]` | DirectX 12 backend (primary). DXIL PSOs, D3D12 barriers, UAV↔vertex-buffer aliasing. | Primary backend |
| **Aver.RHI.D3D11** | C++ | RHI | `[opt, default ON]` | DirectX 11 backend (fallback for older HW). | Secondary backend |
| **Aver.RHI.Vulkan** | C++ | RHI | `[opt, default OFF]` | Vulkan backend, scaffolded, compiled out until Vulkan SDK present (SPIR-V, VK barriers). | Vulkan (SDK not installed) |
| **Aver.Assets** | C++ | Core, Platform | Core | Asset registry, typed handles, ref-counting, async streaming, content-id (`fnv1a64`), the compiled binary-asset container (chunked, versioned, little-endian), and the offline-cache reader (P6). No format-specific knowledge. | Asset I/O runtime backbone; `UVehicleCageAsset` "bytecode" model |
| **Aver.Formats** | C++ | Assets, Core | Core | Runtime *loaders*: fast binary readers for native `.oc*c` compiled assets **and** tolerant text parsers for legacy `.ocbeam/.ocaero/.ocmap/.scene/.octrack` (dev/loose-file fallback). Exact lexing rules from recon (comment `#`, trailing `;`, comma fields, section modes). | Asset I/O (all carried-over text formats) |
| **Aver.Render** | C++ | RHI, Assets, Core | Core | Render graph/frame graph, render-scene (renderables/lights/views/decals), clustered deferred + forward+ hybrid, PBR metallic-roughness material system (runtime graph, no editor recompile), virtualized shadows, TAA/temporal upscale (FSR), HDR/tonemap/post. Owns `RestPositions`/vertex-stream contracts. | Rendering stack; material/texture pipeline; `VehicleDeform` parity host |
| **Aver.Render.GI** | C++ | Render | `[opt]` | Global illumination plugin: surfel/probe GI + screen-space GI, optional DXR ray-traced reflections/GI — the "UE5 Lumen / Source 2" tier, behind a feature gate. | Rendering quality (UE5/Source2 parity) |
| **Aver.Scene** | C++ | Core, Assets | Core | Minimal data-oriented entity/component world (EnTT-style): transforms, hierarchy, component storage, system scheduler. Render/physics-agnostic (P2). | world/scene (entity layer) |
| **Aver.Physics** | C++ | Core | Core | Rigid-body dynamics + collision (Jolt Physics, MIT) wrapped behind an `AvPhysics` facade: bodies, shapes, broadphase, raycasts (ground probe for aero ride height), contact callbacks feeding impact seams. | Rigid chassis integrator; collision layer for `OnHit`/`ReportImpact` |
| **Aver.SoftBody** | C++ | Core | `[opt]` | The OpenConstructor cage solver, extracted UE-free: `AvVehicleMaterial/Node/Beam/Panel/Part`, `AvSolveConfig/Result`, `InitSolver`, `SolveStep` (Verlet + Gauss-Seidel PBD, plasticity, break/tear), impact injection (`OCCrushCurve`, tear radius), part detach/repair, active-set + async worker (`std::thread`/`condition_variable`/`atomic`). Exposes C ABI. | **soft-body solver** (whole `VehicleDamage` core) |
| **Aver.Aero** | C++ | Core | `[opt]` | Aerodynamics: baked-table trilinear sampler (`SampleBaked`, `Bracket`), attitude derivation (`flowDir` inverse, `UnwindDegrees`), force application at CoP, ground-effect model, `Surfaces[]` fallback. Optionally the wind-tunnel re-bake (`AeroSim`) for in-engine baking. | **aerodynamics** (`VehicleAerodynamics` + `AeroSim`) |
| **Aver.GpuDeform** | C++ | RHI, Render, SoftBody | `[opt]` | Compute cage-skin: per-vertex K-nearest node blend → mirrorY → gain → clamp → rest+disp, dispatched per section, UAV aliased as position vertex stream, tear/hide in-place updates. `VehicleDeform` compute shader ported to HLSL/DXC (DXIL/SPIR-V). CPU parity fallback lives in Render. | **GPU deform** (the whole compute pipeline) |
| **Aver.Fracture** | C++ | Core | `[opt]` | Runtime debris: farthest-point cluster split, mass-from-area, launch impulse, secondary-shatter generations, live-debris cap. (Author-time Voronoi bake is a Rust tool, not here.) | **fracture/debris** (`OCDebrisChunk` runtime parts) |
| **Aver.Vehicle** | C++ | Core, Physics | `[opt]` | Authoritative vehicle sim: powertrain (engine map, gearbox, driveline), tire model (slip/load/friction), suspension, chassis rigid-body coupling. The `oc_sim`/`UOCVehicleMovementComponent` successor. Deterministic; exposes the C ABI (`aver_world_step`, `aver_vehicle_set_input`). | **powertrain**, **tire model** (`OCSimCore` P2 realization) |
| **Aver.Net** | C++ | Core, Platform | `[opt]` | Modular replication framework: `AvNetPayload`/channel/module/coordinator triad; raw little-endian UDP **protocol (A)** codec (exact MsgType byte layouts), pose paging, damage fragmentation, join-parity gate. Interop-exact with the existing C# OCServer. Exposes C ABI for the headless host. | **networking/replication** (protocol A + channel framework) |
| **Aver.NetVehicle** | C++ | Net, Vehicle, SoftBody | `[opt]` | Vehicle net modules (Drive/Damage/Config): quantized input, crash-seed replay through the deterministic cage, durable damage end-state snapshots, detach commands. | networking (vehicle-specific channels) |
| **Aver.Match** | C++ | Core | `[opt]` | Matchmaking core: coordinator **protocol (§6)** codec (CoordMsg byte layouts), deterministic greedy match forming (`oc_match` successor). Links into the client for queue and the C# coordinator host via the same source. | **coordinator/matchmaking** (`OCMatchCore`) |
| **Aver.Audio** | C++ | Core, Platform | `[opt]` | Procedural audio graph (Metasound successor) on miniaudio (MIT/public-domain): runtime node graph, live parameter set on playing sources (engine RPM sound). | Audio (Metasound/AudioMixer replacement) |
| **Aver.World** | C++ | Scene, Render, Physics, Assets, Formats | Core | The level/world runtime: `.ocworld`/`.ocmap`/`.scene` loading, object-reference placement (asset name + transform), surface/ground/killz env, spawn, streaming; wires Scene↔Render↔Physics and (softly) Vehicle/Aero/SoftBody when present. | **world/scene** (`.ocmap` object-reference model, `.octrack`) |
| **Aver.Runtime** | C++ | (all enabled modules) | Core | Composition root: engine bootstrap, module registry, tick scheduler, feature wiring. Contains no gameplay logic. | — (aggregator) |
| **Aver.ABI** | C | Runtime (+ public module headers) | Core | The flat `extern "C"` surface: opaque handles, out-pointer struct returns, `aver_abi_version()`. The single seam C#/Rust bind against. | The C interop backbone |
| **Aver.Scripting.NET** | C# | ABI (P/Invoke) | `[opt]` | .NET 10 gameplay scripting host: managed wrappers, component authoring, hot-reload of gameplay assemblies. | **scripting** |
| **Aver.Editor** | C# | ABI (P/Invoke) | tool (never shipped) | Editor application hosting a headless runtime through the ABI: viewport, inspectors, asset browser, live tuning. Zero runtime→editor coupling (P3). | **editor** |
| **aver-assetc** | Rust | ABI/FFI + standalone | tool | Asset pipeline: glTF/OBJ import (cgltf/gltf-rs), image decode (image crate), mesh optimize (meshopt), tangent gen; emits native `.ocmesh/.ocmat/.octex/.ocskm/.ocskel/.ocanim/.ocprefab`. | **asset I/O** (offline), `MeshImporter`/`GltfRig` successor |
| **aver-ocbeamc** | Rust | standalone (+ FFI to SoftBody validators) | tool | `.ocbeam` (+embedded glb) → compiled `.ocbeamc` cage asset: parse, resolve refs (phase-2 material push-down), bounds-fit glb, fold rig/collision/aero. The `Main.java`/`VehicleCageFactory` successor. | asset I/O (cage compile) |
| **aver-aerobake** | Rust | standalone | tool | Wind-tunnel bake (`AeroSim` port): particle-momentum solver + ground-effect pass → `.ocaero`/compiled `.ocaeroc`. | aerodynamics (offline bake) |
| **aver-mapc** | Rust | standalone | tool | `.scene` → `.ocmap` compiler (`OCMapConverter` successor): FNV-1a-64 id, Merkle ROOT (upgraded from name-only stub), invariant-locale LF output. | world/scene (map compile) |
| **aver-shaderc** | Rust/C++ | standalone (wraps DXC) | tool | Shader compile: HLSL → DXIL (+ SPIR-V when Vulkan on); replaces UE virtual-shader-dir registry with an include resolver + cache. | Rendering (shader cook) |

---

## 4. Every carried-over subsystem → module (traceability matrix)

| Recon subsystem | Owning module(s) | Notes on the port |
|---|---|---|
| Soft-body solver (`VehicleDamage`) | **Aver.SoftBody** | Keep exact phase order (Verlet → damage/plasticity → Gauss-Seidel), formulas, float eval order for MP determinism. Async worker via `std::thread`. |
| Aerodynamics (`VehicleAerodynamics`) | **Aver.Aero** | Trilinear sampler + attitude inverse must match writer `flowDir`. SI units (drop `NEWTONS_TO_UE`, CoP cm→m). Ride probe via **Aver.Physics** raycast. |
| GPU deform (`VehicleDeform.usf`) | **Aver.GpuDeform** (+ CPU parity in **Aver.Render**) | Byte-for-byte CPU/GPU parity contract preserved; UAV-as-vertex-stream via **Aver.RHI**. |
| Fracture/debris | **Aver.Fracture** (runtime) + **aver-assetc** (author-time Voronoi pre-dice) | Runtime = clustering split/launch; author-time = pre-fractured shards (no runtime Voronoi). |
| Powertrain | **Aver.Vehicle** (Powertrain subsystem) | Extracted UE-free from `UOCVehicleMovementComponent` (recon P2). |
| Tire model | **Aver.Vehicle** (Tire subsystem) | Same sim core; shared client/server via C ABI. |
| Networking / replication | **Aver.Net** + **Aver.NetVehicle** | Protocol (A) LE UDP kept for OCServer interop; UE `OCNetWire`(B) and RPC(C) dropped. Channel/module/coordinator triad reimplemented in C++. |
| Coordinator / matchmaking | **Aver.Match** | CoordMsg codec + `oc_match` deterministic forming. C# coordinator host stays external, speaks the same wire. |
| World / scene | **Aver.Scene** (ECS) + **Aver.World** (levels, `.ocmap`/`.scene`/`.octrack`) | Object-reference world model (asset name + transform), surface/ground/killz env. |
| Asset I/O | **Aver.Assets** + **Aver.Formats** (runtime) + **aver-assetc / aver-ocbeamc / aver-aerobake / aver-mapc** (offline) | Loose text = dev fallback; compiled binary = ship path. |
| Editor | **Aver.Editor** (C#) | Separate app over C ABI (P3). |
| Scripting | **Aver.Scripting.NET** (C#) | .NET 10 P/Invoke over C ABI. |

---

## 5. Formats — ownership & fidelity

**Carried over (full fidelity, existing OpenConstructor27 content loads).** Runtime loaders in **Aver.Formats**; offline compilers in Rust.

| Ext | Kind | Runtime reader | Offline compiler | Fidelity notes |
|---|---|---|---|---|
| `.ocbeam` | soft-body cage source (text) | Aver.Formats (tolerant parser) | aver-ocbeamc → `.ocbeamc` | Resolve the recon discrepancies: accept 10–13 material fields (default 10–12), `DETACH=` position-independent, 3 behaviors + `GLASS` alias, panel 4th-field override, discard PART leading id. |
| `.ocaero` | baked aero table (text) | Aver.Formats | aver-aerobake → `.ocaeroc` | Validate `OCAERO 1` magic; ascending-axis check; 9- and 10-field rows; density unused at runtime. |
| `.ocmap` / `.scene` | object-reference world (text) | Aver.Formats | aver-mapc | Invariant-locale, LF-normalized writer (fix Java locale + mixed-CRLF bugs). Upgrade ROOT to a real Merkle over placements+asset hashes. |
| `.octrack` | track variant (text) | Aver.Formats | aver-mapc | Treated as an `.ocmap`/`.scene` sibling profile. |

**New native formats (added for unaddressed asset types).** Binary, versioned, little-endian, magic-tagged, produced by **aver-assetc**, read by **Aver.Formats/Aver.Assets**. These close the recon gap where render data survived only as opaque embedded glb.

| Ext | Payload | Fills recon gap |
|---|---|---|
| `.ocmesh` | static mesh: positions, normals, tangents, UV sets, vertex colors, index buffer, per-submesh material binding, LODs, bounds | `MeshData` kept only position+index — everything else was discarded |
| `.ocmat` | PBR material: base color, metallic, roughness, normal, emissive, alpha mode, texture refs, per-primitive assignment | no render-material model existed at all |
| `.octex` | texture: pixel data (BCn), sampler/wrap/filter, sRGB flag, mips | never parsed; only inside embedded glb |
| `.ocskm` | skeletal render mesh: bind pose, 4×(joint,weight), render attrs, explicit vertex-order (kills the implicit "canonical-order contract") | `PartSkin` had no render attrs + implicit external contract |
| `.ocskel` | skeleton: bone hierarchy, local bind TRS, inverse-bind matrices, multi-skin | only text `BONE{}` rows; single-skin limit |
| `.ocanim` | animation: TRS tracks, per-channel mask, native fps + keyframe timing, curve interp | fixed 30-fps resample + CUBICSPLINE/STEP loss — fixed here |
| `.ocprefab` | prefab: component graph + overrides for reusable actor templates | no prefab concept existed |
| `.ocworld` | native scene/world: entity graph, environment, streaming cells (supersedes `.ocmap` for new content while `.ocmap` stays importable) | native world format |

---

## 6. Plugin / module system

### 6.1 Linkage model — static-first, dynamic-optional

- **Default (shipping game client):** all enabled modules are **static** libraries link-collapsed into one runtime executable. This gives LTO/whole-program-opt, no plugin discovery cost, and one binary — the *opposite* of UE's DLL sprawl, while still being modular at the source/target level.
- **Dynamic (editor & tools):** any `[opt]` module may additionally build as a **shared** plugin (`AvModule_*.dll`) so the editor can hot-swap gameplay/tooling modules without relinking the whole runtime. Loaded via `Aver.Platform`'s dynlib loader.
- The choice is per-build, per-module: `AVER_LINK=STATIC|SHARED` global default, overridable as `AVER_MODULE_<NAME>_SHARED=ON`.

### 6.2 Registration & lifecycle

Every module (static or DLL) implements one tiny C-linkage descriptor — no base class, no reflection:

```c
/* Aver.ABI: the only thing a module must expose */
typedef struct AvModuleDesc {
    const char*  name;              /* "Aver.SoftBody"        */
    uint32_t     abi_version;       /* must == aver_abi_version() */
    const char** depends;           /* null-terminated dep names */
    void       (*on_register)(AvEngine*);   /* declare systems/channels/asset types */
    void       (*on_startup) (AvEngine*);   /* acquire resources                    */
    void       (*on_tick)    (AvEngine*, float dt); /* optional; may be null        */
    void       (*on_shutdown)(AvEngine*);
} AvModuleDesc;

/* Static build: each module exports one of these; Aver.Runtime collects them.
   DLL build: the DLL exports `const AvModuleDesc* aver_module_entry(void);` */
```

- **Static collection:** CMake generates `AvModuleManifest.cpp` listing the `AvModuleDesc*` of every compiled-in module (no runtime scanning). `Aver.Runtime` topologically sorts by `depends`, then calls `on_register → on_startup` in DAG order and `on_shutdown` in reverse.
- **Systems, asset types, and net channels are pull-registered** inside `on_register` (e.g. `aver_scene_add_system(...)`, `aver_assets_register_loader(".ocmesh", ...)`, `aver_net_register_channel(...)`). The engine core has no compiled-in list of subsystems (P4). Boot with zero modules → an empty, valid engine.
- ABI-version mismatch on any descriptor aborts load (guards the DLL path).

### 6.3 CMake targets + feature options (compile in/out)

```cmake
# Top-level: one option per optional module; core needs no flag.
option(AVER_RHI_D3D12   "DirectX 12 backend (primary)" ON)
option(AVER_RHI_D3D11   "DirectX 11 backend"           ON)
option(AVER_RHI_VULKAN  "Vulkan backend"               OFF)  # no SDK yet
option(AVER_SOFTBODY    "Soft-body cage solver"        ON)
option(AVER_AERO        "Aerodynamics"                 ON)
option(AVER_GPUDEFORM   "GPU cage deform"              ON)
option(AVER_FRACTURE    "Runtime fracture/debris"      ON)
option(AVER_VEHICLE     "Powertrain + tire sim"        ON)
option(AVER_NET         "Networking"                   ON)
option(AVER_MATCH       "Matchmaking"                  ON)
option(AVER_AUDIO       "Procedural audio"             ON)
option(AVER_RENDER_GI   "Global illumination"          ON)
option(AVER_SCRIPTING   ".NET scripting host"          ON)

add_subdirectory(modules/core)          # always
add_subdirectory(modules/platform)      # always
add_subdirectory(modules/rhi)
if(AVER_SOFTBODY)  add_subdirectory(modules/softbody)  endif()
# ...one guarded add_subdirectory per option...

# Each module target declares ONLY its own deps -> CMake enforces the DAG:
#   modules/softbody/CMakeLists.txt
add_library(Aver.SoftBody ${SRC})
target_link_libraries(Aver.SoftBody PUBLIC Aver.Core)   # nothing else
# A dependency violation (e.g. linking Aver.Render) is a build error by policy-lint.
```

A CI "DAG lint" step parses each `target_link_libraries` and fails if any edge points up a tier or introduces a cycle — the DAG is machine-enforced, not just documented.

### 6.4 How this kills each specific UE bloat source

| UE bloat source | Aver structural fix |
|---|---|
| **Monolithic build** (change one thing, rebuild the world) | Per-module CMake targets + feature options; a tool build (e.g. cage-viewer) links only `Core+Platform+RHI+Render+Formats+SoftBody`. No engine-wide rebuild for a module change. |
| **Everything-is-a-UObject** (vtable + reflection + GC tax on every object) | No base object; handles + POD component arrays; offline codegen reflection used only by serializer/editor (P2). Zero per-object runtime cost. |
| **Editor coupled to runtime** (UnrealEd bleeds into game modules) | Editor is a C# app over the C ABI; runtime links no editor code (P3). Editor-only work (import, Voronoi bake) lives in Rust tools. |
| **Giant DDC** (multi-GB shared cache, network DDC service) | Plain content-addressed file cache keyed by `hash(source⊕compilerVer⊕opts)` (P6). No daemon, no network. |
| **Mandatory subsystems** (engine forces a fixed subsystem set) | Engine boots empty; every subsystem is an opt-in self-registering module (P4). Pay-for-what-you-use is the default. |
| **Plugin DLL sprawl / discovery cost** | Static-first collapse to one binary for ship; DLLs only where hot-swap earns its keep (editor/tools). |

---

## 7. The C ABI boundary — where C#/Rust plug in

### 7.1 Shape of the seam (`Aver.ABI`)

The ABI is a **flat, versioned, opaque-handle** C surface, deliberately narrow — it is the interop backbone all non-C++ code binds against, and the only thing that must stay stable.

Rules (locked in from `oc_sim.h` conventions):
- **Opaque handles** (`typedef struct AvEngine AvEngine;`) — no C++ types cross the line.
- **Struct returns via out-pointer** (`aver_vehicle_position(id, AvVec3* out)`) to avoid Win-x64/SysV/ARM64 return-ABI mismatch.
- **POD-only structs** (`AvVec3{float x,y,z;}`, `AvInput{...}`) — LH Z-up, cm, mirroring the sim.
- **Version guard:** `uint32_t aver_abi_version(void)`; hosts assert equality at startup.
- **No ownership ambiguity:** create/destroy pairs; the ABI never frees caller memory and vice-versa.

```c
uint32_t     aver_abi_version(void);
AvEngine*    aver_engine_create(const AvBootDesc*);
void         aver_engine_destroy(AvEngine*);
void         aver_engine_tick(AvEngine*, float dt);

/* sim (Aver.Vehicle / Aver.SoftBody) — shared client+server source */
AvId         aver_world_add_vehicle(AvEngine*, AvId id, const AvCageDesc*);
void         aver_vehicle_set_input(AvEngine*, AvId, AvInput);
void         aver_vehicle_report_impact(AvEngine*, AvId, AvVec3 localPt, AvVec3 n, float kmh);
void         aver_vehicle_position(AvEngine*, AvId, AvVec3* out);

/* net (Aver.Net) — headless host loop */
int          aver_net_apply_inbound (AvEngine*, const uint8_t* buf, int len);
int          aver_net_collect_outbound(AvEngine*, uint8_t* buf, int cap);

/* assets (Aver.Assets) */
AvHandle     aver_assets_load(AvEngine*, const char* path);
```

### 7.2 Where each language plugs in

```
                 ┌──────────────────────────────────────────────┐
   C# (.NET 10)  │  Aver.Editor        Aver.Scripting.NET        │
                 │   viewport, tools    gameplay logic, hot-reload│
                 └───────────────┬───────────────┬───────────────┘
                                 │  P/Invoke      │  P/Invoke
                 ════════════════▼════════════════▼═══════════════   STABLE C ABI
                                 (Aver.ABI, extern "C", versioned)
                 ════════════════▲════════════════▲═══════════════
                                 │  FFI (shared)  │  FFI (validators only)
   Rust          ┌───────────────┴───────────────┴───────────────┐
                 │ aver-assetc  aver-ocbeamc  aver-aerobake       │
                 │ aver-mapc    aver-shaderc  (mostly standalone) │
                 └────────────────────────────────────────────────┘
                                 │  produce binary .oc* files
                                 ▼
                 C++ runtime reads compiled assets via Aver.Assets/Formats
```

- **C# — editor + gameplay scripting (.NET 10).** Consumes the ABI by P/Invoke. The editor process *is* a headless-runtime host: it creates an `AvEngine`, drives ticks, and renders into an OS child window handed to `Aver.Platform`. Gameplay scripts register components/systems through ABI calls. C# never sees a C++ type — this is what makes the editor swappable and the runtime editor-free (P3).
- **Rust — tooling / asset pipeline.** Primarily **standalone executables** producing the binary `.oc*` formats the runtime consumes (the clean cook boundary — tools and runtime share *files*, not linkage). Rust FFIs into the C ABI only for **shared validators** (e.g. `aver-ocbeamc` calling `Aver.SoftBody`'s parse/resolve to guarantee the compiled cage solves identically to the runtime). This mirrors the recon's "author-once, consume-by-both" law and the OCServer interop model.
- **C — the ABI itself + the physics interop core.** `Aver.SoftBody`/`Aver.Vehicle`/`Aver.Net`/`Aver.Match` export their C ABI so the *existing* C# OCServer/OCCoordinator link the same source and run identical deterministic physics — preserving OCServer/OCCoordinator interoperability with zero changes on the server side.

---

## 8. Rendering-quality posture (UE5 + Source 2 class, royalty-free)

All behind `Aver.Render` / `Aver.Render.GI`, RHI-abstracted, permissive-licensed only:

- **Pipeline:** clustered deferred + forward+ hybrid; visibility/GPU-driven culling; virtualized shadow atlas.
- **GI (the UE5-Lumen / Source2 tier):** surfel + irradiance-probe GI and screen-space GI as the baseline (no SDK needed); optional DXR ray-traced reflections/GI when the D3D12 device supports it. Shipped as the `Aver.Render.GI` opt module so a low-end build pays nothing.
- **Upscaling/AA:** TAA + **AMD FidelityFX FSR** (MIT).
- **Materials:** glTF metallic-roughness PBR built with a *runtime* material-graph API (never an editor recompile — the recon's headless-crash lesson).
- **Third-party (all permissive):** Jolt Physics (MIT), meshoptimizer (MIT), DirectXTex/DirectXMath (MIT), Microsoft DXC (open), FSR (MIT), EnTT (MIT), miniaudio (MIT/public-domain), cgltf/stb (public-domain/MIT), xxHash (BSD), Blake3 (Apache/CC0), Recast/Detour (zlib), Dear ImGui (MIT, editor-debug only). No GPL runtime, no Unreal, no royalty-bearing tech.

---

## 9. Suggested repo layout

```
Aver Engine/
  CMakeLists.txt                 # top-level options + DAG-lint hook
  cmake/AvModule.cmake           # add_av_module() helper (enforces PUBLIC dep policy)
  modules/
    core/  platform/  rhi/  rhi.d3d12/  rhi.d3d11/  rhi.vulkan/
    assets/ formats/ render/ render.gi/ scene/ physics/
    softbody/ aero/ gpudeform/ fracture/ vehicle/
    net/ netvehicle/ match/ audio/ world/ runtime/
  abi/                           # Aver.ABI: extern "C" headers + wrapper TUs
  shaders/                       # HLSL sources -> aver-shaderc
  tools/                         # Rust: aver-assetc, aver-ocbeamc, aver-aerobake, aver-mapc, aver-shaderc
  editor/                        # C#/.NET 10: Aver.Editor
  scripting/                     # C#/.NET 10: Aver.Scripting.NET
  interop/                       # generated P/Invoke + Rust bindgen from abi/
  content/legacy/                # sample .ocbeam/.ocaero/.ocmap/.scene fixtures for golden tests
```

---

## 10. Key risks / decisions to pin before build-out

1. **Sim determinism is a linkage guarantee, not a hope.** `Aver.SoftBody`/`Aver.Vehicle` must compile bit-identically for client, C# server (P/Invoke), and the Rust cage-compiler validator. Fix float eval order and phase order per recon; add a golden cross-platform determinism test in CI.
2. **`.ocbeam` material field-count discrepancy is load-bearing** (runtime wanted exactly 10, writer emitted 13). Canonicalize in `Aver.Formats` + `aver-ocbeamc` (accept 10–13, default 10–12) or existing content silently loses all materials.
3. **Map ROOT/locale/CRLF bugs** in the legacy Java writer must be *fixed* in `aver-mapc` (invariant locale, LF, real Merkle root) while `Aver.Formats` stays tolerant of the old mixed output for import.
4. **Scene↔Render dependency direction.** Kept one-way (World pushes into Render's render-scene; Scene stays render-agnostic) to preserve the DAG — do not let Render reach back into Scene.
5. **Vulkan stays compiled OFF** (`AVER_RHI_VULKAN=OFF`) until the SDK is installed; the backend is scaffolded so enabling it is a flag flip, not a refactor.

---

Deliverable complete. This defines a strict-DAG, pay-for-what-you-use module graph (Core sinks to nothing engine-specific), maps all twelve carried-over subsystems to owning modules, specifies static-first/dynamic-optional linkage with a reflection-free registration lifecycle and CMake feature gating, enumerates the anti-bloat fixes against each named UE failure mode, and fixes the C ABI as the single seam where C# (editor + scripting via P/Invoke) and Rust (asset pipeline via standalone tools + FFI validators) plug in — while keeping the existing C# OCServer/OCCoordinator interoperable by sharing the same sim/net/match C ABI source.