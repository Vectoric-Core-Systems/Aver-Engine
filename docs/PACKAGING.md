# Packaging an Aver project into a shippable game

> **STATUS: REMOVED, 2026-08-17. THIS DOCUMENT IS HISTORY, NOT A DESCRIPTION OF THE ENGINE.**
> `AverGame.exe` (`game/`), `scripts/stage-game.ps1`, `scripts/verify-game.ps1`,
> `scripts/game.allowlist` and the editor's **Package Project** item have all been deleted. Nothing
> below can be run today, and the file paths it cites no longer exist. It is kept because the design
> reasoning — the allowlist split, the `--trace-opens` verification, the ImGui/RHI separation it
> forced — outlived the feature and is cited from live code comments.
>
> **What replaced it: nothing, deliberately.** The editor is how a project is run. A second host that
> rendered a different subset of the scene was a standing source of confusion about what "the game"
> actually shows, and it was the reason a graph-only project appeared broken when the real gap was
> elsewhere. `modules/runtime.game` survives as a LIBRARY, driven by `tests/game`, which is what the
> `AVER_BUILD_GAME` option now switches.
>
> **Was BUILT before removal**, and the record is worth keeping accurate: a project did package, ran
> from a scratch directory with the working directory outside the tree, and failed correctly when a
> staged DLL was deleted (`0xC0000135`). The game half of `SandboxApp` was lifted in eleven commits,
> C1–C11 — see [GAME-LIFT.md](GAME-LIFT.md). Slices 2 onward never left the plan stage.
>
> Produced by a planning workflow reading the tree at **`ae47a2f`**, not the `5714ba3` this header
> originally claimed — which is why its line numbers ran two low against HEAD. Load-bearing claims
> carry a `file:line` that was read rather than assumed; re-check before relying on one. The two
> structural findings (ImGui linked PUBLIC from the D3D12 backend, and asset resolution living
> inside `#if AVER_MODULE_PBR`) are engine facts that outlive this document — and the second was
> fixed by **re-guarding each symbol at the destination**, not by relocating the code, because
> moving code does not change which `#if` it sits under.

---

# THE PACKAGING PLAN — Aver Engine, first shippable game

Verified against the tree at `5714ba3`. No engine code changed. Every load-bearing claim carries a file:line.

---

## a) What a packaged game IS

**Decision: a package is a project directory that happens to contain the engine runtime. Loose content files, no archive, one directory, one manifest.**

```
<out>\
  AverGame.exe                       ← new target, see (b)
  Aver.Scene.dll  Aver.Framework.dll  Aver.Physics.dll
  Aver.Render.PBR.dll  Aver.Render.Voxi.dll  Aver.UI.Abi.dll
  dxcompiler.dll  dxil.dll  nethost.dll
  MSVCP140.dll  VCRUNTIME140.dll  VCRUNTIME140_1.dll     ← see (e); missing today
  Scripting\                         ← managed bridge + contract assemblies
  Game.ocproject                     ← rewritten manifest
  Content\                           ← the project's Content, filtered
  Binaries\Materials\  Binaries\Scripts\
  THIRD-PARTY-NOTICES.txt
  game.json
```

**Why this shape and not an archive.** The engine cannot read an archive. `kAvrSubtypePak` exists at `modules/formats/include/aver/formats/Avr1.hpp:30` and has no other occurrence in the tree; the `TOC ` chunk in `docs/formats/FORMAT_SPECS.md` §13 is unimplemented. Worse, `parseAvr1` is hostile to naive embedding: `modules/formats/src/Avr1.cpp:242` requires the header's `FileSize` to equal the passed length *exactly*, `:262` refuses any compressed chunk outright ("no decompressor is built in", and `third_party/` contains only `fonts`, `imgui`, `stb`), `:267` copies every chunk into a fresh `std::vector<u8>`, and `:268` xxHash64s every chunk on every load. A pak is a real project, not a packaging detail. Shipping it in the same slice as the first game executable means a black screen has two candidate causes.

**Why this *exact* directory shape.** It is chosen so that no path-derivation code has to change. `ProjectDesc::binariesDir()` is `dir + "\\Binaries"` and `scriptsDir()` is `contentDir() + "\\Scripts"` (`modules/formats/include/aver/formats/OcProject.hpp:41-44`). Put `Game.ocproject` at the package root with `CONTENT Content` and:

- `materialForSurface()` (`sandbox/src/SandboxApp.cpp:1230`) finds `<out>\Binaries\Materials\M_Wall.ocmat` on its first candidate;
- `resolveScriptsDir()` (`SandboxApp.cpp:1839`) returns `<out>\Binaries\Scripts`;
- `rebuildContentIndex()` (`:1188`) hashes exactly the same forward-slash relative paths it hashes in the dev project, so **every ObjectId in every `.ocworld` and `.ocmat` is unchanged by packaging.** That property is the whole reason loose-file packaging is nearly free here.

`Content\` is filtered, not copied: drop `Content/Scripts/**` (source, `bin/`, `obj/`, `Scripts.csproj`), `Content/Materials/*.cs`, any `**/Source/` directory (SkyForge has `Content/Audio/Source`), and `*.pdb`. Dropping files changes ids of nothing, because ids hash paths and no shipped asset references a `.cs`.

`game.json` carries what `.ocproject` cannot. Be blunt: **`.ocproject` cannot describe a shipped game.** It has no entry point, no window/resolution defaults, no build id, no icon (`OcProject.hpp:22-33` is the entire key set), and the editor never writes one back. Slice 1 does not pretend otherwise — it puts `entryPoint`, `window{width,height}`, `dotnetRequired`, `sourceCommit`/`sourceDirty` and the staged file inventory into `game.json`, mirroring `payload.json`'s schema and its BOM-less-UTF-8 requirement (`System.Text.Json` rejects a BOM). `AverGame.exe` can read it: `fmt::parseJson` exists at `modules/formats/include/aver/formats/Json.hpp:67`.

---

## b) The runtime host

**Decision: yes, a game-only executable must exist. It is `AverGame.exe`, and it is genuinely small — but only because ~2,000 lines move out of the editor to make it so.**

Today there is exactly one `aver::Application` subclass in the tree. `grep` for `createApplication` returns four hits: the declaration (`modules/runtime/include/aver/runtime/Application.hpp:38`), the call (`EntryPoint.hpp:8`), and `sandbox/src/SandboxApp.cpp:4` + `:5574`. `Aver.Runtime` has exactly one consumer, `sandbox/CMakeLists.txt:17`. `README.md:14` says it outright: `Sandbox.exe` **is** the editor.

The seam is real and clean — `EntryPoint.hpp` is 14 lines, `Application` is four virtuals plus `BootConfig`. A second `add_executable` links and runs *today*. What it gets is a window, a device, a swapchain and an empty loop, because everything a game needs lives in `SandboxApp`: content indexing (`:1188`), mesh registration (`:1260`), level load (`:5113`), start-map open (`:5236`), the world draw walk (`:1384-1520`), the tick-group ordering (`:1047-1055`), `startPlay` (`:2070`), and the play camera (`:2151`).

### The shape

Two new directories:

**`modules/runtime.game/`** → static lib **`Aver.Runtime.Game`**, via `aver_add_module`:
- `src/GameApp.cpp` — the `aver::Application` subclass: `config()`, `onInit`, `onUpdate`, `onRender`, `onShutdown`.
- `src/ContentIndex.cpp` — `rebuildContentIndex`, `resolveAssetPath`, `resolveAnimAsset`, `resolveSceneMesh`, `materialForSurface`, `loadProjectMeshes`, `loadProjectMaterials`. All of that currently sits inside `#if AVER_MODULE_PBR`, so a `PBR=OFF, SCENE=ON` build resolves no assets by id and hands the anim system no resolver.

  > **CORRECTED 2026-08-02, twice.** The block opens at `SandboxApp.cpp:1132` and closes at `:1347`,
  > not the 1130/1345 written here — this section was authored against `ae47a2f` despite the header
  > claiming `5714ba3`. And the claim that relocating the code "fixes a bug" is **wrong**: a verbatim
  > lift inherits the defect. Only re-guarding each symbol at the destination fixes it, which is what
  > [GAME-LIFT.md](GAME-LIFT.md) specifies per-symbol. Moving code does not change which `#if` it is
  > written under.
- `src/LevelLoad.cpp` — `loadLevel` / `loadStartMap` / `applyLevelSky`, lifted.
- `src/WorldRender.cpp` — the `AVER_MODULE_SCENE` block of `onRender`: six frustum planes, `CMeshRenderer` walk, cull, skin-handle substitution, `drawMesh`.
- `src/PlayLoop.cpp` — `startPlay`, the `PrePhysics → aver_phys_step → Physics → PostPhysics` ordering, `anim::animSystem().tick`, `skinnedScene_->update`, `World::flush()`, `drivePlayCamera`.
- `src/WindowInput.cpp` — **new code, not lifted**, see (b) *"what does not exist"* below.

**`game/`** → `add_executable(AverGame src/GameMain.cpp AverGame.rc)`, guarded by a new `option(AVER_BUILD_GAME "Build the standalone game runtime" ON)` beside `AVER_BUILD_SANDBOX` in the root `CMakeLists.txt:32`. `GameMain.cpp` is ~50 lines: parse argv, `#include "aver/runtime/EntryPoint.hpp"`, return `new game::GameApp(...)`.

### Exactly what it links

`Aver.Runtime` (→ `Aver.Core`, `Aver.Platform`, `Aver.RHI`, `Aver.RHI.D3D12`), `Aver.Runtime.Game`, `Aver.Formats`, `Aver.Scene`, `Aver.Anim.Scene`, `Aver.Framework`, `Aver.Physics`, `Aver.Render.PBR` + `Aver.Render.PBR.Materials`, `Aver.Render.Voxi` + `Aver.Render.Voxi.Renderer`, `Aver.Formats.Material`, `Aver.Assets.Gpu`, `Aver.Render.Skin`, `Aver.UI` + `Aver.Render.UI` + `Aver.UI.Abi`, `Aver.Scripting.Host`.

### Exactly what it must NOT link

- **`imgui`** — and this is not a matter of discipline, it is a structural problem. `modules/rhi.d3d12/CMakeLists.txt:14-16` links `imgui` **PUBLIC** on `Aver.RHI.D3D12` and propagates `AVER_WITH_IMGUI=1` **PUBLIC**. `Aver.Runtime` links `Aver.RHI.D3D12` PUBLIC. **You cannot build an ImGui-free `AverGame` out of an `AVER_ENABLE_UI=ON` tree.** Decision for slice 1: **a second build tree**, `build-game`, configured `-DAVER_ENABLE_UI=OFF -DAVER_BUILD_SANDBOX=OFF -DAVER_BUILD_TESTS=OFF`. Splitting ImGui hosting out of the D3D12 backend is the right fix and it is Slice 4, not Slice 1. Say so in the commit message rather than implying the exclusion is enforced by the link line, because it is not.
- **`Aver.Formats.Roslyn`** and everything under `bin\Tools\` — `avermatc`, `averdesign`, `Microsoft.CodeAnalysis.dll` (3.0 MB), `Microsoft.CodeAnalysis.CSharp.dll` (6.6 MB) and 26 localization satellites. A shipped game has no reason to carry a C# compiler. Note that `payload.allowlist:104` gates `Tools/**` on `?AVER_MODULE_PBR`, so a game payload that reused that allowlist would ship ~10 MB of Roslyn for nothing.
- **`Aver.Render.ActorPreview`**, **`Aver.Render.PathTracer`** (`--pt-furnace` only), **`Aver.Mcp`**, **`Aver.Formats.Audio`** (Media Foundation import, editor-only).
- **`Aver.Audio.Abi.dll`** — do not ship it. It is built and staged by `payload.allowlist:44` and has **zero callers**: the managed `Aver.Audio` its own `modules/audio.abi/CMakeLists.txt:7-10` describes does not exist in `scripting/csharp/`, no `DllImport` names it, and it is not in any import table. Slice 1 ships a **silent game** and says so out loud.

### What does not exist and blocks the exe from being a game

1. **Input.** `aver_fw_input_set_key` / `set_mouse` is called from exactly one place — `pushInput()` at `SandboxApp.cpp:2116-2147` — whose entire body is inside `#if AVER_WITH_IMGUI` and reads `ImGui::IsKeyDown` / `io.MouseDelta` / `io.MouseWheel`. There is **no path from `aver::Window`'s message stream into framework input state.** An `AVER_ENABLE_UI=OFF` build has zero game input. `src/WindowInput.cpp` is therefore new code: `aver::Window` grows a keyboard/mouse state surface, and `GameApp` publishes from it.
2. **Camera.** `drivePlayCamera()` writes `camPos_`/`yaw_`/`pitch_`, which are *editor* members, and `viewProj_` is computed in `SandboxApp::onUpdate:1102-1108` from `viewAspect()`, which reads the ImGui dockspace rect `vpW_/vpH_`. `GameApp` computes aspect from the swapchain.
3. **`BootConfig`** has no project path, no start map, no fullscreen, no vsync. Slice 1 adds `--width/--height` CLI flags and reads window size from `game.json`; presentation modes are Slice 5.

---

## c) Assets

**Decision: loose files. Nothing rewrites paths, because nothing absolute survives the filter — but the packager must prove that, not assume it.**

The identity scheme is already package-neutral. `contentIndex_` keys on `fnv1a64` of the *content-relative, forward-slash* path (`SandboxApp.cpp:1199`, marked FROZEN because it must agree with C# `Assets.ObjectIdOf` in `scripting/csharp/Aver.Scene/Native.cs:77` and with `modules/scene/src/World.cpp:394`). `.ocworld` stores `asset` as a relative string plus `objectId = fnv1a64(asset)` (`OcWorld.hpp:16-17`), and `saveLevel` writes `"Meshes/cube.ocmesh"` (`SandboxApp.cpp:2501`, FROZEN). Copy `Content\` and every id resolves.

Three absolute-path leaks exist. Two are excluded by construction, one needs a refusal:

1. **`Content\Scripts\Scripts.csproj`** — `engineProjectReference()` (`sandbox/src/ProjectScaffold.cpp:20-35`) writes `std::filesystem::relative(candidate, scriptsDir)` and falls back to `candidate.string()` — an **absolute engine install path** — when `relative()` fails or returns empty. SkyForge's is currently `..\..\..\..\Aver Engine\scripting\csharp\...`, which is correct only because this machine's layout makes it so. **Packaging does not care: `Content/Scripts/**` is excluded.** This is a project-portability bug and it should not be dragged into the packaging slice.
2. **`.ocmat` texture refs.** The editor's material texture slot is a free-text `ImGui::InputText` (`SandboxApp.cpp:4156-4165`) with no relativity check, `resolveAssetPath` deliberately honours an absolute path (`:1170-1171`), and `avermatc` bakes whatever was typed into the `.ocmat` as `{path:C:\...}` (`modules/formats/src/OcMat.cpp:295`). SkyForge is clean today — I scanned `Content\` and `Binaries\` for `C:\Users` and found nothing — but nothing enforces it. **Refusal: `stage-game.ps1` scans every staged file, text and binary, for `[A-Za-z]:\`, a leading `\\`, or `\\?\`, and fails naming the file and the offending string.**
3. **Anything the world references outside `Content\`.** **Refusal: for every `objectId` in every staged `.ocworld`, assert it is in the index the package's own `Content\` produces.** This is a check the loose-file model *can* make, and it is the one that catches "the level references a mesh that lives in the dev tree".

The seam for the future archive is `IContentSource` — `has(id) / read(id,out) / view(id) / debugName(id) / enumerate()` — with `LooseContentSource` being `rebuildContentIndex()` moved out of `SandboxApp` verbatim. It is not needed for slice 1 and should not be built for slice 1. The single most important fact for whenever it *is* built: **every format reader in the tree already has a from-memory entry point** — `parseAvr1`, `parseOcMesh`, `parseOcSkel/Anim`, `parseOcAudio`, `parseOcLand`, `parseOcWorld/Map/Beam/Mat/Project`. The only two functions that cannot take a byte span are `fmt::loadTexture(path)` (`Texture.hpp:30`) and `assets::uploadTexture(res, path, …)` (`TextureUpload.hpp:27`), and both are thin wrappers over `platform::decodeImage(bytes,size)` (`Image.hpp:26`), which already exists. A pak needs no parser changes.

---

## d) Scripts

**Decision: ship built assemblies. Never ship source, never compile on first run.**

This is barely a choice — it is the only thing the host can do. `HostBridge.LoadScripts` (`scripting/csharp/Aver.Scripting.Bridge/HostBridge.cs:129`) does `Directory.GetFiles(dir, "*.dll").OrderBy(Ordinal)` into a collectible `ScriptLoadContext`. There is no source path in `ScriptHost` at all. Compilation is an editor-time `dotnet build` shell-out on a background thread (`sandbox/src/ToolsMenu.cpp:715, :737`). Compiling on first run would mean shipping the .NET **SDK** or Roslyn plus a reference-assembly set, and failing on any machine without `dotnet` on PATH.

**The tradeoff, honestly.** A shipped assembly is pinned to this engine's `Aver.Scripting` contract version; the bridge already rejects a mismatch by name at load. A one-line script fix requires a re-package. That is correct: a game is a build artifact, not a source tree.

**What it means for the CoreCLR host: nothing changes.** `HostDesc.bridgeDir = executableDir() + "\\Scripting"` (`SandboxApp.cpp:831`) and `HostDesc.scriptsDir = <package>\Binaries\Scripts` — which is exactly what `resolveScriptsDir()` already returns for a valid project. Zero host modification.

**One thing must change, and it is a refusal.** `ScriptHost::init` **declines cleanly** when `nethost.dll` finds no installed .NET (`modules/scripting/src/ScriptHost.cpp:130-146`). Every `runtimeconfig.json` in `bin\Scripting\` names `Microsoft.NETCore.App 10.0.0` with `rollForward: LatestMinor`, so the deployment is **framework-dependent** and the player must have .NET 10. In the editor, a declining host is a degraded but usable tool. In a game, a declining host means **all gameplay silently does not run** and the player gets a static world. `GameApp::onInit` must therefore log `AVER_ERROR` and exit non-zero when scripting is compiled in and the host declines. `game.json` records `dotnetRequired: "10.0"`.

**F#.** Shipping F# scripts is a pure add of `FSharp.Core.dll` (2.4 MB) beside the other assemblies — `HostBridge.LoadScripts` calls `AddProbeDirectory(dir)` *before* enumerating (`:144`) precisely so it resolves. Two refusals: **F# assembly present but `FSharp.Core.dll` absent → fail** (the CMake rule at `modules/scripting/CMakeLists.txt:244-251` names it as an explicit OUTPUT because a library project does not copy package assets by default and a green build can silently omit it), and **`FSharp.Core.dll` staged with no notice → fail** (see (e)). Separately worth naming, because it is a real gap: the editor has **no path to compiling F# in a project**. `dotnet build` picks the language from the extension so `startCompile` would accept an `.fsproj`, but `scriptsCsprojPath()` hard-codes `kCsprojName = "Scripts.csproj"` (`ProjectScaffold.cpp:16, :473`), so nothing ever hands it one. Packaging does not fix that; it just carries whatever `Binaries\Scripts` contains.

Self-contained publish and NativeAOT are **out** of slice 1. They are a distinct licence question (the .NET runtime's own redistribution terms) and must not be smuggled in under "packaging".

---

## e) Third-party redistribution

Every binary that must ship, with what actually pulls it in and whether it may be redistributed.

| Binary | Origin | Licence | Ships? |
|---|---|---|---|
| `AverGame.exe` | ours | `LICENSE.md` | yes |
| `Aver.Scene/Framework/Physics/Render.PBR/Render.Voxi/UI.Abi .dll` | ours | ours | yes — load-time imports of the exe |
| *statically linked:* Jolt Physics | `modules/physics.jolt/LICENSE` | **MIT** | yes; **notice required** |
| *statically linked:* stb | `third_party/stb/LICENSE.txt` | **MIT / public domain** | yes; **notice required** |
| *statically linked:* Dear ImGui | `third_party/imgui/LICENSE.txt` | MIT | **must not be present** — `AVER_ENABLE_UI=OFF` |
| Roboto `.ttf` | `third_party/fonts/LICENSE` | Apache-2.0 | **excluded** — only `SandboxApp.cpp:390-401` loads them |
| `dxcompiler.dll` (14.3 MB), `dxil.dll` (1.5 MB) | Windows SDK, found by `find_file` from `${CMAKE_RC_COMPILER}`'s directory (`modules/rhi.d3d12/CMakeLists.txt:22-24`) | Windows SDK redistributable terms (DXC's own source is NCSA) | yes; notice already written at `stage-payload.ps1:310-316` |
| `nethost.dll` (122 KB) | `%ProgramFiles%\dotnet\packs\Microsoft.NETCore.App.Host.win-x64\*\runtimes\win-x64\native\` (`modules/scripting/CMakeLists.txt:133`) | MIT / .NET Library terms | yes; notice at `stage-payload.ps1:318-321` |
| `Scripting\Aver.{Framework,Scene,Scripting,UI}.dll` + `Aver.Scripting.Bridge.*` | ours | ours | yes |
| `Binaries\Scripts\*.dll` | the project's | the project's | yes |
| `FSharp.Core.dll` | .NET SDK `library-packs` | **MIT** | yes **if** F# is used — **currently in NO allowlist section, so it neither ships nor gets a notice.** Newly *redistributed* the moment a game ships. |
| `Microsoft.CodeAnalysis(.CSharp).dll` + 26 satellites | `bin\Tools\` | **MIT** | **NO.** No game needs a compiler. Note in passing: these ship in the *engine* payload today and `THIRD-PARTY-NOTICES.txt` never mentions Roslyn — MIT requires the copyright notice. That is a live compliance miss in `stage-payload.ps1`. |
| `Aver.Audio.Abi.dll` | ours | ours | **NO** — zero callers (see (b)) |

### The one that is missing and is a hard blocker

I walked the import directory of `build-release\bin\Aver.Scene.dll`:

```
MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll,
api-ms-win-crt-{math,heap,locale,string,stdio,runtime}-l1-1-0.dll, KERNEL32.dll
```

The `api-ms-win-crt-*` are the UCRT and are part of Windows 10+. **`MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll` are not.** They are the Visual C++ 2015-2022 runtime, they are **not in `payload.allowlist`**, and they are not shipped by anything in this repo. A staged payload — engine or game — **does not start on a machine without the VC++ Redistributable installed.** `stage-payload.ps1` correctly refuses the *debug* CRT (`MSVCP140D.dll` / `VCRUNTIME140D.dll` / `VCRUNTIME140_1D.dll` / `ucrtbased.dll`, non-redistributable, enforced by a real PE import walk at `:331`) but says nothing about the release CRT being absent.

**Decision: `stage-game.ps1` stages the three release CRT DLLs app-local**, located via `$env:VCToolsRedistDir\x64\Microsoft.VC143.CRT\`, with a notice paragraph naming the Visual Studio redistributable terms. App-local deployment of these is explicitly sanctioned, and a package that requires the user to run an installer first is not "a usable game". If they cannot be located, **fail** — do not produce a package that will not start.

### The notices file must derive, not be hand-maintained

`stage-payload.ps1:293-298` is a hand-written `$components` array of four entries plus two hand-typed paragraphs. Roslyn shipping unlisted for however long is the proof that this does not hold.

**Decision, in slice 1, for `stage-game.ps1`:** build a provenance table keyed by staged filename, and after the copy phase, assert that **every staged `.dll`/`.exe` is either named `Aver.*` or has a provenance entry with a licence file or paragraph**. An unaccounted binary is a hard `Fail`. That is the difference between a notices file and a claim about a notices file. Retrofit the same assertion to `stage-payload.ps1` in a later slice.

---

## f) The UX

**Decision: the script is the primitive, the menu item is a thin shell-out over it, and there is no CLI mode on the game exe.**

**Primitive — `scripts/stage-game.ps1`**, sibling of `stage-payload.ps1`, same `Fail`/`Note` idiom, same `Get-PeImports` helper, driven by a new **`scripts/game.allowlist`** with `[bin]`, `[scripting]`, `[crt]` sections and the same both-ways conditional check (`?AVER_MODULE_X` ON must match ≥1 file, OFF must match zero — the half that catches a reconfigured Ninja tree still holding the previous configure's DLL).

```
./scripts/stage-game.ps1 -Project "C:\Users\User\Documents\Aver Projects\SkyForge\SkyForge.ocproject" `
                         -Out ..\stage\SkyForge-0.1.0 -Config Release [-Force]
```

Its refusals, all of which are silent failures today:

- `Binaries\Scripts` empty, or older than the newest `.cs`/`.fs` under `Content\Scripts` — reuse `newestDllTime` (`sandbox/src/ToolsMenu.cpp:650`), which already computes exactly this staleness for the Tools menu badge.
- a script DLL whose `Aver.Scripting` reference version ≠ the staged engine's.
- an F# assembly with no `FSharp.Core.dll` beside it.
- any `objectId` in a staged `.ocworld` that is not in the packaged `Content\` index.
- any absolute path in any staged file.
- any `\bin\`, `\obj\`, `.cs`, `.fs`, `.csproj`, `.pdb` reaching `Content\`.
- any staged binary importing the debug CRT (PE walk, not a grep — the CRT names appear as plain ASCII in a PE for unrelated reasons).
- any staged binary with no notices entry.
- the release CRT not locatable.
- Debug config without `-AllowDebugCrt`, same as `stage-payload.ps1`.

**Editor button — `File ▸ Package Project…`** in `sandbox/src/ToolsMenu.cpp`, beside `Compile Scripts` (`:827`). That file already owns the background-thread shell-out (`startCompile`, `:715`), the progress modal (`:549`), the exit-code reap (`:695`), and the staleness check — the packaging item reuses all four and runs `powershell -File scripts/stage-game.ps1`. Disabled with a specific tooltip when: no project loaded / no `Binaries\Scripts` / scripts stale / no staged engine payload to draw binaries from. **The menu item must never be the only path** — a check that can only be run by clicking is not a check.

**One honest wrinkle.** A user with a *downloaded* engine has no `scripts/` directory: `payload.allowlist` ships none. So `[engine]` must gain exactly two lines — `scripts/stage-game.ps1` and `scripts/game.allowlist` — and **not** `scripts/**`, which would also ship `gates.ps1` and the recorded baselines.

---

## g) Verification

The question is *"how does anyone know a package works, and that it is not secretly reading the dev tree?"* Three checks. Two ship in slice 1. The third is honestly deferred, with the reason stated.

### Prerequisite: the game exe grows the capture contract

`AverGame.exe --frames N --probe-rel U V --screenshot <png> --width W --height H --trace-opens`.

Cheap, because the capture primitives are **engine-side, not editor-side**: `requestCapture(x,y)`, `getCapture(f32[4])` and `getFrameImage(bytes,w,h)` are virtuals on `rhi::IDevice` (`modules/rhi/include/aver/rhi/RHI.hpp:371-375`). `Engine::run` already treats `maxFrames>0` as a capture run — no splash, window opened unactivated (`modules/runtime/src/Engine.cpp:19-42`), which honours the standing "capture runs must not steal focus" rule. `GameApp::captureCheck` is `SandboxApp::captureCheck` (`:4877`) with `vpX_/vpY_/vpW_/vpH_` replaced by the whole backbuffer — which is *simpler*, because a game has no dockspace and no editor chrome to accidentally probe.

### Check 1 — the isolation check (the one the question is really about)

The claim to disprove: *the package is silently reading the dev tree.*

**Mechanism A — the engine says what it opened.** Add `--trace-opens` to `AverGame`: a log sink over `platform::readFileBytes` / `readFileText` (`modules/platform/include/aver/platform/FileSystem.hpp` is the whole surface — there is no other file API in the engine) plus the `stbi_load` path in `assets::uploadTexture`. `verify-game.ps1` asserts **every logged path is under the package root**. This catches a dev-tree dependency *by name*, with the offending path printed, instead of by a symptom. It ships with the mechanism it checks, which is the house rule.

State its limit rather than overselling it: it covers `aver::platform` and the format loaders. It does **not** cover `LoadLibraryW` (`dxcompiler.dll` at `D3D12Device.cpp:70`, `nethost.dll` at `ScriptHost.cpp:130`, then hostfxr), `dotnet`'s own probing, or anything inside the CLR.

**Mechanism B — run it where the dev tree is not an ancestor.** Copy the package to `%TEMP%\aver-game-verify-<guid>\` and run it *there*, with the process working directory set to `C:\` rather than the package directory. Two properties fall out for free:

- Nothing up that chain contains `cmake/AvModule.cmake` **and** `modules/`, the two markers `EngineScaffold::engineRoot()` requires, so the whole `ProjectScaffold` walk-up class of bug is dead by construction.
- A working directory that is not the package root exposes any relative-path fallback that happens to work only when you `cd` first.

If a second drive root exists, prefer it — `std::filesystem::relative` returning an empty path across roots is the specific behaviour this repo already suspects (`ProjectScaffold.cpp:26` has an `rel.empty()` guard that exists for *some* reason), and **that inference is not yet verified; it deserves a five-line test before anyone leans on it.**

**Mechanism C — the environment is scrubbed.** `Start-Process` with `PATH` reduced to `%SystemRoot%\system32;%SystemRoot%`, `DOTNET_ROOT` and `AVER_*` unset. If the package needs the dev machine's environment, it fails here.

I deliberately do *not* propose Process Monitor. It would be stronger for `LoadLibraryW`, but it is an external, non-redistributable tool, and a check nobody can run is not a check.

### Check 2 — it reached a frame and drew something

Three assertions, none of which needs a recorded baseline.

1. **It reached a frame.** Exit code 0, and the log contains `Aver Engine stopped after 60 frame(s)` — that string already exists at `Engine.cpp:107`. A probe line must be present at all; if every probe is `NO-PROBE` that is a **FAIL**, copying `verify-payload.ps1`'s explicit refusal of the degenerate pass ("agrees with itself while proving nothing").

2. **It drew something — as a relation, not a number.** Run the *same* package twice at the *same* width and height:
   ```
   AverGame.exe --frames 60 --width 1600 --height 900 --probe-rel 0.5 0.5 --screenshot a.png
   AverGame.exe --frames 60 --width 1600 --height 900 --probe-rel 0.5 0.5 --no-world --screenshot b.png
   ```
   and assert the two probe codes **differ**. A probe equal to the clear colour proves nothing; a probe that *changes when the world is removed* proves geometry reached that pixel. No baseline, no recorded value, nothing to go stale. `--no-world` is a new flag on `GameApp` that skips `loadStartMap` and the world draw.

   Two constraints from the standing notes are non-negotiable and must be enforced by the script, not assumed:
   - **identical aspect ratio, not merely identical size** — `--probe-rel` survives a resize but *not* a change of aspect. Both runs pass explicit `--width`/`--height`, and the script asserts the two screenshots came out the same dimensions.
   - **screen the probe for a flat 7×7 neighbourhood** in `a.png` before trusting it, so a one-pixel feature is not being read as signal. `gates.ps1` documents this trap at length (`:136-171`); it has already caught this repo once.

3. **Gameplay actually ran.** If the package contains `Binaries\Scripts\*.dll` and `AVER_MODULE_SCRIPTING` is on, the log must contain the GameMode's `OnBeginPlay`. This single line catches a missing .NET runtime, a version-mismatched bridge, a missing `FSharp.Core.dll`, and a `Binaries\Scripts` that was staged empty — the four failures that a rendering probe cannot see.

### Check 3 — package vs. dev tree, the direct analogue of `verify-payload.ps1`

Run the editor over the *source* project and the packaged game over its *copy*, same probe, same window size, and compare the two probe codes **to each other**. This is exactly `verify-payload.ps1`'s argument — comparing two binaries removes the baseline from the question, so a disagreement means one thing ("packaging lost something") instead of two.

**Be honest: this does not work today, and slice 1 must not claim it.** The editor probes a viewport rect inside a dockspace and draws its own `objects_` scene; the game probes the whole backbuffer and draws only the world. The two images are not comparable until `GameApp` and `SandboxApp` share the *same* extracted world-draw code and the editor is driven with a level loaded and the viewport maximised. **Check 3 is Slice 2.**

### Static checks, which cost nothing and run every time

- Walk every staged file for `[A-Za-z]:\`, leading `\\`, `\\?\`.
- Walk every staged PE's import directory (reuse `Get-PeImports` from `stage-payload.ps1:331`): no debug CRT, and every import is either a Windows DLL, an `Aver.*` sitting beside it, or one of the named redistributables.
- Every staged binary has a `THIRD-PARTY-NOTICES.txt` section.
- `game.json` `fileCount`/`totalBytes` match the tree on disk.

### The free oracle for Slice 2

The extraction in Slice 2 is behaviour-preserving by construction, so **the editor's 153 existing gate probes must be bit-identical before and after it.** Any probe that moves is a bug in the extraction. This is the one place in the whole plan where an existing oracle answers the question for free — and it is a further argument for doing the extraction *after* the game exe exists, so its correctness is decided by a mechanism that already works.

---

## h) Out of scope for slice 1, and the order after it

### Explicitly OUT of slice 1

- **`.ocpak`, compression, memory-mapping, zero-copy chunks.** No reader; `parseAvr1` copies and hashes every chunk and refuses compression; no codec is vendored (zstd BSD-3 or lz4 BSD-2 would each need a `third_party/` tree with the `modules/physics.jolt/README.md` provenance pattern and a notices entry). A package that merely *contains* the assets is worth having before one that transforms them.
- **Cook / DDC / `.ocmeta` / `.octex` / BC7.** `FORMAT_SPECS.md` §13 specifies all of it; none of it exists. `docs/STATUS.md:2993` is explicit that PNG/JPEG decode straight to RGBA8 with CPU mips.
- **GUID asset identity.** `fnv1a64(relative path)` is FROZEN in three languages. Changing identity and adding packaging in one step gives a failure two candidate causes.
- **Self-contained / NativeAOT .NET.** Separate licence question.
- **Fullscreen, borderless, resolution selection, monitor choice, cursor clip, Alt+Enter.** `WindowDesc` is `{title,width,height,resizable,activate}` and has none of it. Slice 1 ships a fixed-size window from `game.json`.
- **Save games, settings, per-game user data.** `%LOCALAPPDATA%\AverEngine` is the *editor's* prefs store (`EditorPrefs.cpp:145` asserts it: "Nothing about a PROJECT is stored here"). `grep SaveGame` returns nothing.
- **Audio.** No managed contract assembly exists; the game is silent.
- **Gamepad.** No path exists at all.
- **Dedicated server / true `--headless` game mode.** `Engine::run` knows "capture run" and "no window"; neither is a server.
- **Splitting ImGui out of `Aver.RHI.D3D12`.** Worked around with a second build tree.
- **`Scripts.csproj` `<Reference>`+`<HintPath>` conversion.** Real bug, wrong slice — packaging excludes that file entirely.

### Slice order

**Slice 0 (small, enabling).** `AverGame`'s capture contract: `--frames`, `--width/--height`, `--probe-rel`, `--screenshot`, `--no-world`, `--trace-opens`. Nothing to verify yet; it is what makes everything after it verifiable.

**Slice 1 — the package.** `modules/runtime.game/` + `game/` + `AverGame.exe`, `scripts/stage-game.ps1`, `scripts/game.allowlist`, `scripts/verify-game.ps1`, the release-CRT staging, the derived notices assertion, and `File ▸ Package Project…`. **Done when:** SkyForge packages to `%TEMP%`, runs 60 frames from a scratch copy with a scrubbed environment and `cwd = C:\`, opens no file outside the package, prints `OnBeginPlay` for `FpsGameMode`, and its centre probe changes when `--no-world` is passed.

**Slice 2 — extraction and the editor/game relation.** Move the lifted code out of `SandboxApp` into `Aver.Runtime.Game` and have the editor *call* it. Proved by the 153 existing gate probes being bit-identical. Check 3 becomes real at the end of this slice.

**Slice 3 — input off ImGui.** `aver::Window` grows a keyboard/mouse surface; both hosts publish from it; the editor keeps `WantCapture*` as a *filter* on top rather than as the source. Check: drive the same key sequence through both paths, assert the pawn's final world matrix is identical. Relation, no baseline.

**Slice 4 — the ImGui/RHI split.** Move ImGui hosting out of `Aver.RHI.D3D12` so `AVER_ENABLE_UI` stops being a whole-tree property and `AverGame` can be built from the same tree as the editor. Check: `AverGame.exe`'s import table and static-link map contain no ImGui symbol, in a tree configured `AVER_ENABLE_UI=ON`.

**Slice 5 — presentation.** Fullscreen / borderless / resolution / vsync / cursor clip in `WindowDesc` and `game.json`. Check: 100 enter/leave cycles with no swapchain leak; client size matches the requested mode.

**Slice 6 — `.ocpak` behind `IContentSource`.** Subtype `PAK ` (already reserved), `TOC ` sorted by id, `STRT` via the existing `AvrStringTable` (`Avr1.cpp:115-142`), one `BLOB` with every embedded container at a 256-byte boundary. Check: open the same content loose and packed and assert identical id sets, byte-identical `read()` for every id, and field-equal parsed structs for a sample. Plus determinism: pack twice, `memcmp`.

**Slice 7 — retrofit the derived-notices assertion to `stage-payload.ps1`**, which will immediately fail on Roslyn and on `FSharp.Core.dll`. That failure is the point.

### Doc drift found in passing (cheap, correct it with slice 1)

- `docs/PROJECTS.md:67` — *"nothing executes those scripts yet"*. False since `c43ad23`/`d65dfaa`.
- `docs/PROJECTS.md:78` — the example manifest said `STARTMAP Maps/demoworld.ocmap`. **FIXED 2026-08-02.**
  Note the original finding here was itself wrong: it claimed SkyForge's real manifest says
  `Maps/Default.ocworld`. There is no `.ocproject` file anywhere in this repo to compare against —
  that string is a fixture in `tests/formats/src/FormatTest.cpp:105`. The authority is what the
  editor actually writes, `STARTMAP Maps/Default.ocmap` (`sandbox/src/ProjectScaffold.cpp:66`),
  and that is what the doc now says.
- `sandbox/src/ProjectScaffold.cpp` actor templates emit *"IT COMPILES, BUT IT DOES NOT TICK YET… its hooks are not called"* into **every generated `.cs` file in every new user project**. Same staleness, and this one ships.
- `scripts/payload.allowlist:113-115` names 17 test executables; `build/bin` holds 29. Comment drift only — the allowlist excludes them by construction.