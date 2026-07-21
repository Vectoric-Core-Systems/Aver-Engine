# Aver Engine

A custom, **modular** 3D game engine built exclusively on **permissively-licensed** libraries (MIT / BSD / zlib / Apache-2.0 / public-domain). It is the successor runtime for the **OpenConstructor** soft-body destructible racing sim — carrying its `.oc*` storage formats forward while replacing Unreal Engine.

- **Polyglot:** C++ (core, RHI, renderer, physics), C (stable ABI), C# (.NET 10 editor + scripting), Rust (asset pipeline).
- **Render backends:** DirectX 12 (primary), DirectX 11, Vulkan — behind one RHI abstraction.
- **Anti-bloat:** strict dependency DAG, no `UObject`, pay-for-what-you-use modules, offline content-addressed cook.
- **Coordinate contract:** centimeters, +Z up, +X forward, +Y right, left-handed (carried from OpenConstructor).

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for the full module design, **[docs/formats/FORMAT_SPECS.md](docs/formats/FORMAT_SPECS.md)** for the storage formats, and **[docs/recon/](docs/recon/)** for the authoritative extraction of the existing OpenConstructor formats & solver.

## Status

**Phase 1 — Foundation (current).** Buildable slice: `Aver.Core` + `Aver.Platform` (Win32 window) + `Aver.RHI` (Null backend; D3D12/D3D11 stubs) + `Aver.Runtime` + a `Sandbox` app that opens a window and runs the engine loop.

Roadmap: Phase 2 formats (`.ocbeam`/`.ocmap` loaders, golden-tested against real files) → Phase 3 D3D12 renderer → Phase 4 soft-body + GPU deform → Phase 5 aero/vehicle/fracture → Phase 6 net/match → Phase 7 Rust pipeline + C# editor.

## Build

Requires Visual Studio 18 (C++ workload), which supplies CMake + Ninja + the Windows SDK.

```powershell
# from the repo root
./scripts/build.ps1                     # configure + build (Debug)
./scripts/build.ps1 -Release            # ...or Release, into a separate tree
./scripts/run.ps1 --frames 5            # build then run the sandbox for 5 frames
./scripts/run.ps1 --headless --frames 5 # run without opening a window
```

Output goes to `build/bin/` (`build-release/bin/` for `-Release`; the two coexist). Vulkan stays compiled out (`-DAVER_RHI_VULKAN=ON` to enable once the SDK is installed).

## Layout

```
modules/          C++ engine modules (strict DAG: core -> platform -> rhi/assets -> ... -> runtime)
  core/ platform/ rhi/ rhi.d3d12/ rhi.d3d11/ rhi.vulkan/ runtime/   (implemented)
  assets/ formats/ render/ scene/ physics/ softbody/ aero/ ...        (skeleton — see each README)
abi/              stable extern "C" seam (Aver.ABI) — C#/Rust bind here
tools/            Rust asset pipeline (aver-assetc, aver-ocbeamc, ...)
editor/ scripting/  C# / .NET 10
sandbox/          sample app
shaders/          HLSL sources
docs/             architecture, format specs, recon of the existing engine
cmake/            AvModule.cmake helper
```
