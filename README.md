# Aver Engine

A custom, **modular** 3D game engine built exclusively on **permissively-licensed** libraries (MIT / BSD / zlib / Apache-2.0 / public-domain). It began as the successor runtime for the **OpenConstructor** soft-body destructible racing sim — carrying its `.oc*` storage formats forward while replacing Unreal Engine — and has since been taken in a more general-purpose direction. The engine holds no game content: a game is a sibling folder with its own `.ocproject` manifest.

- **Polyglot:** C++ (core, RHI, renderer, physics), C (one seam per module, not one seam for everything), C# on .NET 10 (scripting, gameplay, materials, HUD). **There is no Rust in this tree.** `tools/README.md` still advertises a Rust asset pipeline that was never written, and `abi/README.md` records why the single flat `Aver.ABI` those files describe is not coming either.
- **Render backends:** DirectX 12 is the one you should use. `modules/rhi.d3d11` is a stub that returns a null device. **`modules/rhi.vulkan` is no longer a stub** — it creates a real device and swapchain, compiles the shared HLSL to SPIR-V through a vendored SPIR-V-capable DXC, and brings the engine up to the point of drawing; it does not yet render a frame, and `AVER_RHI_VULKAN` stays **OFF by default** until it does. Its own source names what is left. All three sit behind one RHI abstraction.
- **Anti-bloat:** strict dependency DAG, no `UObject`, pay-for-what-you-use modules behind `AVER_MODULE_*` switches — the engine builds and runs with every optional one off, which is what makes the headless tests meaningful.
- **Coordinate contract:** centimetres, +Z up, +X forward, +Y right, left-handed, row-major with row vectors (`v * M`) — carried from OpenConstructor.

See **[docs/ABI.md](docs/ABI.md)** for every C entry point and which seam to reach for, **[docs/STATUS.md](docs/STATUS.md)** for where the work actually stands, **[docs/formats/FORMAT_SPECS.md](docs/formats/FORMAT_SPECS.md)** for the storage formats, **[docs/ASSET_IMPORT.md](docs/ASSET_IMPORT.md)** for getting third-party geometry and textures in (glTF, OBJ, USDA, PNG — and what each importer refuses), and **[docs/recon/](docs/recon/)** for the extraction of the existing OpenConstructor formats & solver. **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** holds the original module design and is worth reading for the principles, but it predates most of the tree and its Tier 7 (a C# editor, Rust tools, one flat ABI) describes none of what was built.

## Status

**`build/bin/Sandbox.exe` *is* the editor** — a Dear ImGui shell composited over the D3D12 viewport, with a project browser, asset and actor editor tabs, a Details panel and a Tools menu that compiles a project's C#. The separate C# editor process that `editor/` and `interop/` describe was never started; both directories hold one README and no code.

Two of its panels write back to C# source rather than to a built artefact, which is the shape the editor is converging on. The actor editor (`sandbox/src/ActorEditor.*`) opens a `.cs` that carries a generated designer region — or, read-only, one whose class declares a mesh, camera or light — draws it through `Aver.Render.ActorPreview`, and rewrites the placements in that file when you drag the gizmo. An external edit reloads into the view unless the tab is dirty, in which case the reload is refused and says so. The Details panel's **Save to C#** writes the material's `.cs`, never the `.ocmat` — a save into a build artefact appears to work and then vanishes on the next compile.

Implemented and exercised by headless tests: the D3D12 renderer, the AVR1 container and the `.oc*` readers on it, the entity/component world and the gameplay layer over it, Jolt physics behind the engine's own C seam, the in-process CoreCLR host with hot reload, the retained game UI and its renderer, and the audio mixer.

Not implemented — each is a README under `modules/` describing a responsibility nothing has taken yet: `render`, `render.gi`, `softbody`, `aero`, `gpudeform`, `fracture`, `vehicle`, `net`, `netvehicle`, `match`, `world`. `modules/abi` and `abi/` are empty *by decision* rather than by omission; `abi/README.md` says why.

Audio is the newest and the least connected: the mixer, the WASAPI device and the C seam all exist and are tested, but nothing in the editor plays a sound, no managed `Aver.Audio` assembly exists yet, and `sandbox` does not link any of it.

## Build

Requires Visual Studio 18 (C++ workload), which supplies CMake + Ninja + the Windows SDK; `scripts/build.bat` hard-codes its install path. `dotnet` is optional — CMake reports at configure time when it is missing, skips the managed half, and the scripting host then declines at run time rather than the editor failing.

```powershell
# from the repo root
./scripts/build.ps1                     # configure + build (Debug)
./scripts/build.ps1 -Release            # ...or Release, into a separate tree
./scripts/run.ps1                       # build then launch the editor
./scripts/run.ps1 --headless --frames 5 # run without opening a window
./scripts/run.ps1 --ui-demo             # draw a HUD through the C seam, to see the game UI working
./scripts/gates.ps1 -Config baseline    # probe rendered frames against a recorded baseline
```

Output goes to `build/bin/` (`build-release/bin/` for `-Release`). The two trees coexist on purpose: each has its own gates baseline, and optimisation changes floating-point codegen, so comparing one build against the other's numbers is a mistake the pairing exists to prevent. `scripts/run.ps1` always builds and launches the Debug tree. Vulkan stays compiled out (`-DAVER_RHI_VULKAN=ON` to enable once the SDK is installed).

The test executables land beside `Sandbox.exe` and each is a standalone `.exe` you run directly — `UiTest`, `UiRenderTest`, `ActorPreviewTest`, `AudioTest`, `OcAudioTest`, `FormatTest`, `JsonTest`, `GltfTest`, `MeshTest`, `MaterialTest`, `ActorScriptTest`, `SceneTest`, `FrameworkTest`, `PhysicsTest`. None needs a GPU: the two renderer tests stand up their own mock `IDevice` and assert on what was recorded, rather than needing a backend. Which ones exist depends on the `AVER_MODULE_*` switches, since a test is added with its module.

## Layout

```
modules/          C++ engine modules (strict DAG: core -> platform -> rhi/assets -> ... -> runtime)
sandbox/          Sandbox.exe — the editor
scripting/csharp/ the C# side: what a project references, the CLR bridge, avermatc
tests/            headless test executables, one directory per area
tools/            ActorSweep.cpp — reports what the actor editor makes of a real project's scripts
docs/             the C seams, format specs, editor and project specs, recon of the existing engine
cmake/            AvModule.cmake (aver_add_module)
third_party/      imgui (docking), stb, fonts (Roboto). Jolt is vendored under modules/, at
                  physics.jolt/ — it is the rigid-body backend, not an incidental dependency
branding/         master lockup (human-authored) -> splash, icons, logo
abi/ interop/ editor/ shaders/ content/   README only — nothing is built from these
```

`shaders/` is one of those placeholders: HLSL is embedded in C++ next to the code that compiles it (`modules/rhi/src/RHIShaders.cpp` for the shared prelude, `modules/render.pbr/include/aver/pbr/PbrShaders.hpp`, `modules/render.ui/src/UiShaders.hpp`) and is compiled at run time by DXC. Game content lives outside the engine entirely — see [docs/PROJECTS.md](docs/PROJECTS.md).

## Modules

| Directory | Target(s) | What it is |
|---|---|---|
| `core` | `Aver.Core` | Maths, log, time, hashing, types. Depends on nothing engine-specific. |
| `platform` | `Aver.Platform` | Win32 window and input, splash, filesystem, image decode, a debounced directory watcher. |
| `assets` | `Aver.Assets`, `Aver.Assets.Gpu` | Asset ids; the decode-to-GPU texture step. |
| `formats` | `Aver.Formats` (+ `.Material`, `.Audio`) | The AVR1 container and the `.oc*` readers/writers, glTF import, JSON, and the C# source rewriters (`MaterialScript`, `ActorScript`). |
| `rhi` | `Aver.RHI` | Device/swapchain interface, the generic render-feature surface, the shared shader prelude, a Null backend. |
| `rhi.d3d12` | `Aver.RHI.D3D12` | The backend: PBR, procedural sky, lines, wireframe, MSAA as a runtime setting, a mesh-shader geometry path, the ImGui host, capture. |
| `rhi.d3d11` | `Aver.RHI.D3D11` | A stub. Returns a null device. |
| `rhi.vulkan` | `Aver.RHI.Vulkan` | Vulkan 1.3, opt-in via `AVER_RHI_VULKAN` (default OFF). Real device, swapchain, descriptor sets, mesh shaders and ray query; the shared HLSL is compiled to SPIR-V by the vendored DXC in `third_party/dxc-spirv`. Brings the engine up and **presents a frame** — grid, cube, shadow, sky — at 14 validation errors, none fatal. The editor UI does not draw on it for a reason outside this backend: the editor is ImGui and the only ImGui backend here is `rhi.d3d12.imgui`, with no Vulkan equivalent yet. Its own README names the rest. Run it with `--backend vulkan --debug-layer`; with the LunarG SDK installed that enables `VK_LAYER_KHRONOS_validation`, which names each one exactly. |
| `render.pbr` | `Aver.Render.PBR`, `.Materials` | The material system and its C seam; the surface BRDF and the GPU binding half. |
| `render.softbody` | `Aver.Render.SoftBody` | Draws a mesh whose vertices come from a simulated soft body: reads the particles back from Jolt through `Aver.Physics`, packs them into the renderer's interleaved vertex format (recomputing normals, converting world space back to mesh-local) and substitutes the `MeshHandle` at the draw, the same seam GPU skinning uses. Needs both the scene and physics; force-disabled without either. |
| `render.voxi` | `Aver.Render.Voxi`, `.Renderer` | Render-feature settings + C seam (Core-only) and the GI / shadow / RayQuery feature that drives the RHI. |
| `render.ui` | `Aver.Render.UI` | Turns a `UiDrawList` into draw calls in the overlay pass, after the camera post chain, so a HUD is not tonemapped with the world. |
| `render.actorpreview` | `Aver.Render.ActorPreview` | The actor editor's 3D preview: its own colour+depth target, pipeline, mesh registry, and a camera published at `b4` so it never collides with the shared prelude's blocks. |
| `scene` | `Aver.Scene` | Entities, packed component pools, hierarchy, fields addressed by name, and a C seam readable end to end without meeting the word *actor*. |
| `framework` | `Aver.Framework` | The gameplay vocabulary over that world — class registry, defaults, spawn, possess, begin/end play — as data rather than an inheritance tree. |
| `physics` | `Aver.Physics` | Jolt behind the engine's own plain-C seam. |
| `synapse` | `Aver.Synapse` | AI with no world in it: the baked navigation grid, A\* across it, and the behaviour-tree evaluator. Core and Formats only, which is what lets a path be checked against a map drawn in ASCII. See [docs/SYNAPSE.md](docs/SYNAPSE.md). |
| `synapse.scene` | `Aver.Synapse.Scene` | The join — agent, perception and behaviour components, and the ticks that drive them. Synapse advises and never moves anything itself; steering reaches a character through nodes the author can see. |
| `scripting` | `Aver.Scripting.Host` | In-process CoreCLR via nethost/hostfxr; collectible load context, reflection discovery, hot reload. Needs Core and Platform, never the RHI. |
| `ui` | `Aver.UI` | The retained game UI draw list — layers, batching, clip intersection, premultiplied alpha. Core-only, which is what makes it testable with no device. Not the editor's ImGui. |
| `ui.abi` | `Aver.UI.Abi` | The C seam a game's HUD calls. |
| `sound` | `Aver.Sound` | Aver Sound — a procedural sound authored as a node graph (`.ocsnd`) and rendered to PCM. Pure: a graph and arithmetic go in, a buffer comes out, so the whole synthesiser is checkable on a machine with no sound card. See [docs/SOUND.md](docs/SOUND.md). |
| `audio` | `Aver.Audio` | The mixer — voices, buses, 3D pan and attenuation. Core-only; it fills a buffer the caller supplies and never touches a device. |
| `audio.wasapi` | `Aver.Audio.Wasapi` | The device. Windows only, so it is guarded the way the D3D12 backend is. |
| `audio.abi` | `Aver.Audio.Abi` | The C seam for audio. Links the device, because "play a sound" only means something once something drives a sound card. |
| `runtime` | `Aver.Runtime` | The engine loop, `Application`, and the entry point that wires the compiled-in modules. |

Each seam versions on its own, and most are exported by their own DLL. `docs/ABI.md` catalogues them entry point by entry point; note that it was written before `audio.abi` landed, so it counts one seam fewer than the tree now has.

## The C# side

`scripting/csharp/` holds what a project references and what the host loads. The user-facing assemblies are `Aver.Scripting` (`AverBehaviour`, `Log`, the Voxi and PBR bindings — its assembly version is a contract the host enforces), `Aver.Framework` and `Aver.Scene` (actors, pawns, `ActorBuilder`, `ClassBuilder`, entities and transforms), `Aver.UI` (`Hud`, `Layer`, `Colour`, `Rect`, over `Aver.UI.Abi`), and `Aver.Materials` (`[AverMaterial]`, `MaterialBuilder`). `Aver.Scripting.Bridge` is the managed end of the host and is never referenced by a script.

A material is authored as a C# class: `avermatc` (`Aver.MaterialCompiler`, staged to `bin/Tools/`) reflects the built assembly and emits `.ocmat`. So `Content/Materials/*.cs` is the source and `Binaries/Materials/*.ocmat` is the build output, and the editor looks in `Binaries` **first** — a project that has not adopted C# materials falls through to a hand-authored `.ocmat` and still works. **Tools ▸ Compile Scripts** builds both halves; **Tools ▸ Reload Scripts** swaps the result into the running editor without a restart. Hot reload does not carry state: a behaviour's fields start again from their initialisers.

A new project is scaffolded with a `Scripts.csproj` referencing four of those — `Aver.Scripting`, `Aver.Framework` (which carries `Aver.Scene` behind it), `Aver.UI` and `Aver.Materials` — plus `Content/{Maps,Meshes,Materials,Textures,Sounds,Scripts}` and a starter `M_Default` material. Opening an older project offers an upgrade, and that upgrade **merges** the `.csproj` rather than regenerating it, so hand-added references and settings survive. Both are reachable without the UI: `Sandbox.exe --new-project <location> <name>` and `--upgrade-project <path.ocproject>`.
