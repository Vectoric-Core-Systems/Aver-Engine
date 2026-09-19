# Packaging an Aver project into a shippable game

> **RENAMED 2026-09-16.** The runtime moved to `Runtime/`: the library that was `modules/runtime.game`
> is `Runtime/` (target still `Aver.Runtime.Game`), and the executable that was `game/AverGame.exe` is
> `Runtime/host` -> **`AverEngineRuntime.exe`**, shown as "Aver Engine Runtime". The design record below
> keeps the names it was written with; the commands and the notices table use the current ones.
>
> **STATUS: LIVE AGAIN, 2026-09-05, WITH THE CHECK THAT WAS MISSING.** Removed 2026-08-17
> (`b262c73`), restored today. `AverEngineRuntime.exe` (`Runtime/host/`), `scripts/stage-game.ps1`,
> `scripts/verify-game.ps1`, `scripts/game.allowlist` and the editor's **Package Project** item
> (`sandbox/src/SandboxShell.cpp:1038` → `ToolsMenu.cpp:1083`) all exist and all work; everything
> below can be run.
>
> **RE-READ AGAINST THE SCRIPTS 2026-09-20.** Sections (a), (b), (e), (f), (g) and (h) were checked
> against `scripts/stage-game.ps1` and `scripts/verify-game.ps1` as they stand today, and
> the corrections are inline and marked. The plan below was written before any of it was built, so
> its refusal lists and verification checks are *proposals*; where one did not land, that is now
> said in the same place rather than left for a reader to discover by running the script. Anything
> still carrying a bare `file:line` was **not** re-resolved — treat an unmarked citation as
> unverified and check it before leaning on it.
>
> **WHY IT WAS REMOVED, AND WHAT IS DIFFERENT NOW.** The stated reason was real: a second host
> "rendered a different subset of the scene than the editor", and nothing in the tree could notice
> — no CI, no packaging test, and `verify-payload.ps1` compares `Sandbox.exe` against
> `Sandbox.exe`. Restoring the executable without answering that would have restored the problem.
>
> `verify-game.ps1` now runs a **divergence gate**: both hosts open the same project with
> `--scene-census` and their censuses must match — entity counts, distinct meshes and materials,
> and an order-independent hash of every (mesh, material) pair. It is a census and not a frame
> comparison on purpose; see `modules/world/include/aver/world/SceneCensus.hpp` for why (different
> viewports, different aspect ratios, different cameras by design).
>
> **It found a real defect on its first run.** A staged package reported
> `VSMain (vs_6_0): error: missing entry point definition`, fell back to `backend=Null` and refused
> every mesh. The cause was not a shader bug: `game.allowlist` predated the migration of HLSL out of
> C++ literals into `modules/*/shaders/`, so the package shipped the HLSL compiler and no HLSL —
> the same hole `payload.allowlist` had already grown an entry to close. A missing shader file is
> deliberately non-fatal, which is exactly why this surfaced as something that looks like a shader
> bug rather than a packaging one.
>
> The runtime never left: `modules/runtime.game` kept building and growing for the nineteen days it
> had no executable, and `GameApp::onInit` is a superset of the editor's `applyProject`. What was
> deleted was ~30 lines of glue and three scripts.
>
> **Was BUILT before removal, further than this header used to admit.** A project did package, ran
> from a scratch directory with the working directory outside the tree, and failed correctly when a
> staged DLL was deleted (`0xC0000135`). The game half of `SandboxApp` was lifted in eleven commits,
> C1–C11, the same day (2026-08-02) this document's own Slice 1 was first declared built — see
> [GAME-LIFT.md](GAME-LIFT.md), also HISTORY. **"Slices 2 onward never left the plan stage" was true
> that day and stayed in this header for two more weeks of exactly the work it says never happened.**
> That lift *is* this document's Slice 2 in substance: `Aver.Runtime.Game` gained its own content
> index, level load, world render and play loop, each re-guarded per module rather than moved
> verbatim (2026-08-02). Slice 3 (input reaching gameplay with no ImGui) landed the same day, as
> Lift C10. Slice 4 — splitting ImGui hosting out of `Aver.RHI.D3D12` into the separate
> `Aver.RHI.D3D12.ImGui` module cited two paragraphs up — landed 2026-08-15, two days before removal.
> Graphs running in the game, and the shipped game connecting the seams the editor connects, landed
> 2026-08-13 and 2026-08-15. **What genuinely never happened:** Slice 5 (fullscreen/presentation),
> Slice 6 (`.ocpak`), Slice 7 (retrofitting the notices assertion to `stage-payload.ps1`), and Check 3
> (comparing the editor's probe against the game's, section g — its own text already says why, and
> that reason never stopped being true). And the library kept growing *after* the executable was
> deleted: save/load (2026-08-21, see (h) below) and several Synapse AI slices landed in
> `modules/runtime.game` afterward, with no executable left to run any of it end-to-end — only
> `tests/game` and the editor's own composition root exercise the library now.
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

**REGENERATED FROM `scripts/game.allowlist` 2026-09-20.** The sketch that was here predated four
entries that file has since grown, each added because its absence was a real defect: the crash
reporter, the audio seam, the settings seam and the HLSL. A picture of a package that omits them is
worse than no picture, because every one of those omissions shipped once.

```
<out>\
  AverEngineRuntime.exe              ← the entry point game.json names; NOT Sandbox.exe
  AverCrashReporter.exe              ← spawned by the runtime's own crash handler; see below
  Aver.Scene.dll  Aver.Framework.dll  Aver.Physics.dll
  Aver.Render.PBR.dll  Aver.Render.Voxi.dll  Aver.UI.Abi.dll
  Aver.Audio.Abi.dll                 ← P/Invoked by Audio.cs; was excluded on a stale "zero callers"
  Aver.Settings.dll                  ← P/Invoked by Native.cs; see (e) and (h)
  dxcompiler.dll  dxil.dll  nethost.dll
  shaders\**                         ← the HLSL itself: it is read from disk, not compiled in
  MSVCP140.dll  VCRUNTIME140.dll  VCRUNTIME140_1.dll     ← staged app-local; see (e)
  Scripting\                         ← managed bridge + contract assemblies
  Game.ocproject                     ← rewritten manifest, every source KEY carried through
  Content\                           ← the project's Content, filtered
  Binaries\Materials\  Binaries\Scripts\
  THIRD-PARTY-NOTICES.txt
  game.json
```

The module DLLs are conditional on their `AVER_MODULE_*` option; the UI, audio and settings seams
are not, because the top-level `CMakeLists.txt` builds all three in every edition. `shaders\**` is a
recursive glob rather than a list of names on purpose — every module that calls `aver_deploy_shaders`
copies into that one flat directory, so naming files would silently drop the next module's shader.
The three CRT DLLs are not in the build tree at all; `stage-game.ps1` resolves them from the
installed redist and stages them beside the exe, and refuses to produce a package if it cannot.

**Why this shape and not an archive.** The engine cannot read an archive. `kAvrSubtypePak` exists at `modules/formats/include/aver/formats/Avr1.hpp:30` and has no other occurrence in the tree; the `TOC ` chunk in `docs/formats/FORMAT_SPECS.md` §13 is unimplemented. Worse, `parseAvr1` is hostile to naive embedding: `modules/formats/src/Avr1.cpp:242` requires the header's `FileSize` to equal the passed length *exactly*, `:262` refuses any compressed chunk outright ("no decompressor is built in"), `:267` copies every chunk into a fresh `std::vector<u8>`, and `:268` xxHash64s every chunk on every load. The parenthetical here used to read "`third_party/` contains only `fonts`, `imgui`, `stb`", which was a headcount of a directory that now holds fourteen entries; the claim that survives the recount is the one the argument actually needs — **none of them is a general-purpose stream codec.** `meshoptimizer` compresses vertex and index buffers and nothing else, and there is still no zstd or lz4 anywhere in the tree. A pak is a real project, not a packaging detail. Shipping it in the same slice as the first game executable means a black screen has two candidate causes.

**Why this *exact* directory shape.** It is chosen so that no path-derivation code has to change. `ProjectDesc::binariesDir()` is `dir + "\\Binaries"` and `scriptsDir()` is `contentDir() + "\\Scripts"` — **re-resolved 2026-09-20 to `modules/formats/include/aver/formats/OcProject.hpp:320-323`**, from the `:41-44` written here, which the manifest's own growth pushed 279 lines down. Put `Game.ocproject` at the package root with `CONTENT Content` and:

**The rewritten manifest carries every KEY value line of the source project through**, in its original order — `stage-game.ps1` used to write only `OCPROJECT`/`NAME`/`CONTENT`/`STARTMAP`, which silently dropped every `RENDER.*`, `PHYSICS.*`, `AUDIO.*`, `WINDOW.*`, `IMPORT.*`, `STREAM.*`, `DRONE.GRAPH` and `INPUT.SCHEME` key the source project stated, so a packaged game opened with none of the render/physics/audio tuning — and no default input scheme — its own project asked for. Only `CONTENT` is rewritten, to the staged folder name; `NAME` and `STARTMAP` are re-emitted from the values the script already parses and validates rather than copied as raw text.

**RE-RESOLVED 2026-09-20, and two of the three moved hosts, not just lines.** All three were cited into `SandboxApp.cpp` when the editor owned this code; the editor/runtime split moved the first and third into the shared library, which makes the argument *stronger* — it is now one implementation answering for both hosts rather than two that happen to agree.

- `GameContent::materialForSurface()` (`Runtime/src/GameContent.cpp:712`) finds `<out>\Binaries\Materials\M_Wall.ocmat` on its first candidate. Shared: the editor reaches it through the same `GameContent` (`sandbox/src/SandboxAssets.cpp:551`) that the runtime does.
- The scripts directory is derived, not searched. The runtime does it in one line — `hd.scriptsDir = project_.binariesDir() + "\\Scripts"` (`Runtime/src/GameApp.cpp:1285`) — so it lands on `<out>\Binaries\Scripts` for a package exactly as it does for a dev project. The editor's own `resolveScriptsDir()` (`sandbox/src/SandboxAutosave.cpp:361`, the former `SandboxApp.cpp:1839`) is editor-only and is not what a package uses.
- `GameContent::adopt()` (`Runtime/src/GameContent.cpp:42`) hashes exactly the same forward-slash relative paths it hashes in the dev project, so **every ObjectId in every `.ocworld` and `.ocmat` is unchanged by packaging.** That property is the whole reason loose-file packaging is nearly free here, and the function's own comment at `:62-65` now says so in those words — the invariant this document relies on is asserted at the site that would break it. (`rebuildContentIndex`, the name used here and in section (b) below, no longer exists in the tree.)

`Content\` is filtered, not copied: drop `Content/Scripts/**` (source, `bin/`, `obj/`, `Scripts.csproj`), `Content/Materials/*.cs`, any `**/Source/` directory (SkyForge has `Content/Audio/Source`), and `*.pdb`. Dropping files changes ids of nothing, because ids hash paths and no shipped asset references a `.cs`.

`game.json` carries what `.ocproject` cannot. Be blunt: **`.ocproject` cannot describe a shipped game.** It has no entry point, no build id and no icon, and the editor never writes one back. `stage-game.ps1` puts `entryPoint`, `window{width,height}`, `dotnetRequired`, `enableUi`, `config`, `sourceCommit`/`sourceDirty` and the staged file inventory into `game.json`, mirroring `payload.json`'s schema and its BOM-less-UTF-8 requirement (`System.Text.Json` rejects a BOM, and `verify-game.ps1` checks the first three bytes for one).

**Two corrections, 2026-09-20.**

**The key set is not twelve lines, and "no window/resolution defaults" stopped being true.** `OcProject.hpp:22-33` — cited here as "the entire key set" — is the `CREATEDWITH` comment block and nothing else. `ProjectDesc` runs to `OcProject.hpp:318` and `loadOcproject` parses about seventy keys at `OcProject.cpp:54-235`, among them four `WINDOW.*` and forty `RENDER.*` (the newest being `RENDER.EXPOSURE`, `RENDER.BLOOM`, `RENDER.AUTOEXPOSURE` and `RENDER.TONEMAP`). The manifest won that argument on purpose: `OcProject.hpp:235-236` states the decision outright — the project is the right home for anything the editor can author, "and `stage-game.ps1` can generate `game.json` FROM them instead of the other way round".

**Nothing in the engine reads `game.json`.** `fmt::parseJson` exists, but no host calls it on this file: `grep game.json` over `Runtime/` returns one comment. The only reader is `verify-game.ps1`, which uses it for the package's name, entry point, start map, file count and — load-bearing for the divergence gate in (g) — the `config` the package was staged from. That is a perfectly good reason for the file to exist; it is just not the reason written here. **A reader who assumes the runtime honours `game.json`'s `window{width,height}` is wrong** — `stage-game.ps1` writes a hardcoded 1280×720 there, while the size the runtime actually opens comes from the manifest's `WINDOW.TITLE`/`WINDOW.SIZE` through `GameApp::config()` (`Runtime/src/GameApp.cpp:587-591`, applied only when no `--width`/`--height` overrode them). `game.json`'s copy is an unread record.

---

## b) The runtime host

**Decision: yes, a game-only executable must exist. It is `AverGame.exe`, and it is genuinely small — but only because ~2,000 lines move out of the editor to make it so.** *(It is `AverEngineRuntime.exe` since 2026-09-16; the design record below keeps the name it was written with, per the banner. The "~2,000 lines move out of the editor" half was an understatement — see "The shape".)*

Today there is exactly one `aver::Application` subclass in the tree. `grep` for `createApplication` returns four hits: the declaration (`modules/runtime/include/aver/runtime/Application.hpp:38`), the call (`EntryPoint.hpp:8`), and `sandbox/src/SandboxApp.cpp:4` + `:5574`. `Aver.Runtime` has exactly one consumer, `sandbox/CMakeLists.txt:17`. `README.md:14` says it outright: `Sandbox.exe` **is** the editor.

The seam is real and clean — `EntryPoint.hpp` is 14 lines, `Application` is four virtuals plus `BootConfig`. A second `add_executable` links and runs *today*. What it gets is a window, a device, a swapchain and an empty loop, because everything a game needs lives in `SandboxApp`: content indexing (`:1188`), mesh registration (`:1260`), level load (`:5113`), start-map open (`:5236`), the world draw walk (`:1384-1520`), the tick-group ordering (`:1047-1055`), `startPlay` (`:2070`), and the play camera (`:2151`).

### The shape

> **WHAT WAS ACTUALLY BUILT, 2026-09-20.** The two-directory plan below is right in outline and wrong
> in every filename, so a reader following it will look for sources that do not exist. `Runtime/`
> holds `GameApp.cpp`, `GameContent.cpp`, `GameLevel.cpp`, `GameRender.cpp`, `GameInput.cpp`,
> `GameLandscape.cpp`, `GameStreaming.cpp`, `GameWater.cpp` and `MouseCapture.cpp`; `Runtime/host/`
> holds `RuntimeMain.cpp` and `AverEngineRuntime.rc`.
>
> **And it is two libraries, not one, which the plan did not anticipate.** `Aver.Runtime.Game.Core`
> holds everything both hosts share; `Aver.Runtime.Game` holds `GameApp` alone. The editor links Core
> and never the second, so `GameApp.cpp` cannot be pulled into `Sandbox.exe` — the exclusion is a
> link edge rather than a rule someone has to remember, which is the same argument this document
> makes for an allowlist over a denylist. `Runtime/host/CMakeLists.txt:28` builds the executable over
> them.

Two new directories:

**`modules/runtime.game/`** → static lib **`Aver.Runtime.Game`**, via `aver_add_module`:
- `src/GameApp.cpp` — the `aver::Application` subclass: `config()`, `onInit`, `onUpdate`, `onRender`, `onShutdown`.
- `src/ContentIndex.cpp` — `rebuildContentIndex`, `resolveAssetPath`, `resolveAnimAsset`, `resolveSceneMesh`, `materialForSurface`, `loadProjectMeshes`, `loadProjectMaterials`. All of that currently sits inside `#if AVER_MODULE_PBR`, so a `PBR=OFF, SCENE=ON` build resolves no assets by id and hands the anim system no resolver.

  > **CORRECTED 2026-09-13.** `loadProjectMaterials` no longer exists in the runtime: it had no caller
  > and was removed. Per-surface material resolution goes through `materialForSurface`.

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
- **`Aver.Formats.Roslyn`** and everything under `bin\Tools\` — `avermatc`, `averdesign`, `Microsoft.CodeAnalysis.dll` (3.0 MB), `Microsoft.CodeAnalysis.CSharp.dll` (6.6 MB) and 26 localization satellites. A shipped game has no reason to carry a C# compiler. Note that `payload.allowlist` gates `Tools/**` on `?AVER_MODULE_PBR` (`:124` today, `:104` when this was written), so a game payload that reused that allowlist would ship ~10 MB of Roslyn for nothing. `game.allowlist` records the same exclusion in its own "DELIBERATELY ABSENT" block.
- **`Aver.Render.ActorPreview`**, **`Aver.Render.PathTracer`** (`--pt-furnace` only), **`Aver.Mcp`**, **`Aver.Formats.Audio`** (Media Foundation import, editor-only).
- **`Aver.Audio.Abi.dll`** — do not ship it (for slice 1's purposes; see the (e) table and (h) correction below for what changed since). It is built and staged by `payload.allowlist` (`:46` today). **Superseded: a game ships it now**, and `game.allowlist`'s own entry records why the exclusion outlived its reason. **Corrected:** this bullet originally said it had zero callers anywhere, managed or native. That stopped being true at `dbfd6a6` ("The audio stack had no callers at all; now a game can make a noise"): `scripting/csharp/Aver.Framework/Audio.cs` now wraps the whole ABI, six Aver Node nodes call it, and `SandboxApp.cpp:1559` now calls `aver_audio_init()` on entering Play — so the editor itself, which is how a project runs, is no longer silent. Slice 1 as scoped still ships no such wiring in `AverGame`/`GameApp`, which is now moot: `AverGame.exe` was deleted before slice 1 shipped (see the header).

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
2. **`.ocmat` texture refs.** The editor's material texture slot is a free-text `ImGui::InputText` (`SandboxApp.cpp:4156-4165`) with no relativity check, `resolveAssetPath` deliberately honours an absolute path (`:1170-1171`), and `avermatc` bakes whatever was typed into the `.ocmat` as `{path:C:\...}` (`modules/formats/src/OcMat.cpp:295`). SkyForge is clean today — I scanned `Content\` and `Binaries\` for `C:\Users` and found nothing — but nothing enforces it. **Refusal: `stage-game.ps1` scans every staged file, text and binary, for `[A-Za-z]:\`, a leading `\\`, or `\\?\`, and fails naming the file and the offending string.** *(Built, but in `verify-game.ps1` rather than here, and narrowed to text extensions — deliberately, because scanning `.dll`s reported managed assemblies' build paths as failures. See the note at the end of (f).)*
3. **Anything the world references outside `Content\`.** **Refusal: for every `objectId` in every staged `.ocworld`, assert it is in the index the package's own `Content\` produces.** This is a check the loose-file model *can* make, and it is the one that catches "the level references a mesh that lives in the dev tree". *(Still unbuilt as of 2026-09-20. `verify-game.ps1`'s `--trace-opens` assertion catches the same class of fault — a dev-tree asset shows up as an opened path outside the package — but only at run time, only for content the run actually touches, and only on a machine that still has the dev tree to fall back to.)*

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
| `AverEngineRuntime.exe` | ours | `LICENSE.md` | yes |
| `Aver.Scene/Framework/Physics/Render.PBR/Render.Voxi/UI.Abi .dll` | ours | ours | yes — load-time imports of the exe |
| *statically linked:* Jolt Physics | `modules/physics.jolt/LICENSE` | **MIT** | yes; **notice required** |
| *statically linked:* stb | `third_party/stb/LICENSE.txt` | **MIT / public domain** | yes; **notice required** |
| *statically linked:* Dear ImGui | `third_party/imgui/LICENSE.txt` | MIT | **must not be present** — `AVER_ENABLE_UI=OFF` |
| Roboto `.ttf` | `third_party/fonts/LICENSE` | Apache-2.0 | **excluded** — only `SandboxApp.cpp:390-401` loads them |
| `dxcompiler.dll` (14.3 MB), `dxil.dll` (1.5 MB) | Windows SDK, found by `find_file` from `${CMAKE_RC_COMPILER}`'s directory (`modules/rhi.d3d12/CMakeLists.txt:22-24`) | Windows SDK redistributable terms (DXC's own source is NCSA) | yes; notice already written at `stage-payload.ps1:310-316` |
| `nethost.dll` (122 KB) | `%ProgramFiles%\dotnet\packs\Microsoft.NETCore.App.Host.win-x64\*\runtimes\win-x64\native\` (`modules/scripting/CMakeLists.txt:133`) | MIT / .NET Library terms | yes; notice at `stage-payload.ps1:318-321` |
| `Scripting\Aver.{Framework,Scene,Scripting,UI}.dll` + `Aver.Scripting.Bridge.*` | ours | ours | yes |
| `Binaries\Scripts\*.dll` | the project's | the project's | yes |
| `FSharp.Core.dll` | .NET SDK `library-packs` | **MIT** | yes **if** F# is used — and **still without a notice.** See the correction under the table. |
| `Microsoft.CodeAnalysis(.CSharp).dll` + 26 satellites | `bin\Tools\` | **MIT** | **NO.** No game needs a compiler. Note in passing: these ship in the *engine* payload today and `THIRD-PARTY-NOTICES.txt` never mentioned Roslyn — MIT requires the copyright notice. **Fixed:** `stage-payload.ps1` now emits a Roslyn section whenever it finds `Microsoft.CodeAnalysis*.dll` in the staged tree, concatenating `third_party/nuget/LICENSE.roslyn.txt`. The licence file had to be written rather than vendored: the packages declare MIT by SPDX expression and carry no licence text of their own. |
| `Aver.Audio.Abi.dll` | ours | ours | **yes, since 2026-09.** This row said NO on a "zero callers" reading that went stale; `game.allowlist` carries the entry unconditionally now and its comment records the whole episode. |
| `Aver.Settings.dll` | ours | ours | **yes, since 2026-09-20 — and this is the row the table was missing.** See below. |
| `AverCrashReporter.exe` | ours | `LICENSE.md` | yes. Unconditional: it links nothing from the engine, and without it the runtime's crash handler writes a full report folder and then spawns an executable that is not there. |
| `shaders\**` | ours | ours | yes. Not a redistribution question — a correctness one. The HLSL is read from `<exeDir>\shaders` at pipeline-build time, so a package without it ships the compiler and nothing to compile. |

### The two seams no import scan can see

**`Aver.Settings.dll` was absent from both allowlists and from this table, and the reason is worth more than the fix.** `scripting/csharp/Aver.Framework/Native.cs:243` declares `Lib = "Aver.Settings"` and `:248-276` import fourteen `aver_settings_*` entry points through it. Nothing *native* links that DLL — search the tree's `CMakeLists.txt` files and the only consumers are the module itself and `tests/settings` — so its name appears in **no staged binary's import directory at all.** `stage-game.ps1`'s refusal 8b, the import-closure walk that is this pipeline's one general safety net, therefore cannot see it; the script says so in its own log line at `:592`, "managed assemblies have none". A managed `DllImport` is a string the CLR resolves at first call, not a PE import entry.

The failure it would have produced is the kind that sends you to the wrong file. `Runtime/src/GameApp.cpp:1314` builds a settings path unconditionally and hands it to `configureInput` at `:1317`, so `HostBridge.cs:440` calls `Settings.Open` on **every launch of a scripted game**. The `DllNotFoundException` is caught by `ConfigureInput`'s own handler (`HostBridge.cs:461-464`), which returns `-1`, and `GameApp.cpp:1328` reports that as *"input scheme '…' failed to load"*. Rebindable input dies at startup and the diagnostic blames a `.ocinput` file that is present and correct. The editor takes the identical route at Play start (`sandbox/src/SandboxPlay.cpp:168-169`), which is why `payload.allowlist` grew the entry in the same change.

Both entries are **unconditional**, and that is forced rather than chosen: `CMakeLists.txt:242` adds `modules/settings` beside core and platform with no switch over it, so there is no `AVER_MODULE_SETTINGS` in `CMakeCache.txt` to gate on — and `stage-game.ps1:320` refuses any condition the cache does not mention. Gating on `AVER_MODULE_SCRIPTING`, the option that decides whether the managed caller ships, would be worse: the DLL is built in every edition regardless, so a scripting-off tree would trip the *OFF* half of the both-ways check and refuse a package that was fine.

**The generalisation, for whoever adds the next seam.** This is the second time exactly this has happened — `Aver.Audio.Abi.dll` was excluded on a "zero callers, no DllImport names it, it sits in no import table" note that all three stopped being true — and the two cases share a mechanism: *the only check that would have caught it reads import tables, and a P/Invoke is not in one.* A new `Aver.X.dll` that only C# calls must be added to both allowlists by hand, because nothing will tell you.

### Still open: F# ships without its notice

**Found 2026-09-20 while checking the table above.** `stage-game.ps1` emits `FSharp.Core`'s MIT paragraph only if it finds `<out>\Scripting\FSharp.Core.dll` — the *engine's* bridge directory. A project's F# assemblies do not land there. `ProjectScaffold.cpp:992` puts a project's compiled scripts in `<project>\Binaries\Scripts`, `stage-game.ps1` copies that tree verbatim to `<out>\Binaries\Scripts`, and `dotnet build` of an `.fsproj` copies `FSharp.Core.dll` beside its own output — so the file arrives under `Binaries\Scripts` and the `Test-Path` looks in `Scripting`. (The engine's own F# sample builds to `bin/FSharpScripts` — `modules/scripting/CMakeLists.txt:398` — which is not `bin/Scripting` either, so the check has no directory in which it can succeed.) **A game that uses F# therefore redistributes `FSharp.Core.dll` with no MIT notice**, which is the exact failure the Roslyn row above records. This is a `stage-game.ps1` fix, not an allowlist one, and it is not made here.

### The one that was missing and was a hard blocker — CLOSED

**Both scripts stage it now, and neither does it from a hardcoded list.** `stage-game.ps1` section 5 walks the import tables of everything it has already staged, collects whatever matches `$AverVcRuntimePattern` (`scripts/PeImports.ps1:112`), resolves the redist with `Find-VcRedistDir` and copies exactly those DLLs app-local — so a toolset bump that adds, say, `msvcp140_atomic_wait.dll` is picked up instead of silently missed. If the redist cannot be found, or it has no copy of a DLL the staged binaries import, that is a `Fail` and no package is produced. `stage-payload.ps1:296-308` does the same for the engine payload, and `game.allowlist`'s `[crt]` section lists the three names so the file stays a complete inventory of a package without anyone having to read PowerShell.

The account below is kept because it is the measurement that motivated the decision, and because `verify-game.ps1`'s own header records the trap that follows from it: **deleting `vcruntime140.dll` from a package does not fail the isolation run**, since a developer machine has the redistributable installed system-wide and the loader finds it in `System32`. The app-local copy exists for machines that do not, which by definition are not the one running the check. The static import-closure refusal in `stage-game.ps1` is the thing that catches this, and it is proven to bite.

#### The original finding

I walked the import directory of `build-release\bin\Aver.Scene.dll`:

```
MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll,
api-ms-win-crt-{math,heap,locale,string,stdio,runtime}-l1-1-0.dll, KERNEL32.dll
```

The `api-ms-win-crt-*` are the UCRT and are part of Windows 10+. **`MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll` are not.** They are the Visual C++ 2015-2022 runtime, they are **not in `payload.allowlist`**, and they are not shipped by anything in this repo. A staged payload — engine or game — **does not start on a machine without the VC++ Redistributable installed.** `stage-payload.ps1` correctly refuses the *debug* CRT (`MSVCP140D.dll` / `VCRUNTIME140D.dll` / `VCRUNTIME140_1D.dll` / `ucrtbased.dll`, non-redistributable, enforced by a real PE import walk at `:331`) but says nothing about the release CRT being absent.

**Decision: `stage-game.ps1` stages the three release CRT DLLs app-local**, located via `$env:VCToolsRedistDir\x64\Microsoft.VC143.CRT\`, with a notice paragraph naming the Visual Studio redistributable terms. App-local deployment of these is explicitly sanctioned, and a package that requires the user to run an installer first is not "a usable game". If they cannot be located, **fail** — do not produce a package that will not start. **Landed, and generalised: the set is derived from import tables rather than hardcoded, as described above.**

### The notices file must derive, not be hand-maintained

`stage-payload.ps1:293-298` is a hand-written `$components` array of four entries plus two hand-typed paragraphs. Roslyn shipping unlisted for however long is the proof that this does not hold.

**Decision, in slice 1, for `stage-game.ps1`:** build a provenance table keyed by staged filename, and after the copy phase, assert that **every staged `.dll`/`.exe` is either named `Aver.*` or has a provenance entry with a licence file or paragraph**. An unaccounted binary is a hard `Fail`. That is the difference between a notices file and a claim about a notices file. Retrofit the same assertion to `stage-payload.ps1` in a later slice.

> **HALF LANDED, 2026-09-20 — and the half that did not is the half that was the point.** `stage-game.ps1` section 7 *derives* the notices file from what was actually staged: Jolt is emitted only when `AVER_MODULE_PHYSICS` is on, the `nethost` paragraph only when scripting is, `FSharp.Core` only when the file is found, and the CRT paragraph names the exact DLLs section 5 copied. A missing licence file on disk is a `Fail`. But there is **no provenance table and no closing assertion**: nothing walks the staged binaries afterwards and demands that each one be accounted for. The Roslyn failure this decision was written to make impossible is therefore still possible — it is merely unlikely, because `game.allowlist` excludes `Tools/**` by construction. The F# finding above is the same gap in its live form: a binary shipped, no notice emitted, no check complained. Slice 7's retrofit to `stage-payload.ps1` has not happened either.

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

> **WHICH OF THESE EXIST, checked against the script 2026-09-20.** That list is a *proposal*, and reading it as an inventory would leave you trusting three checks nobody wrote.
>
> **Landed in `stage-game.ps1`:** authored `.cs`/`.fs` under `Content\Scripts` with nothing compiled in `Binaries\Scripts`, and `Binaries\Scripts` with no `.dll` in it at all (`:263`, `:268`); sources, build intermediates and symbols reaching the package — refusal 8a, which catches `Content\` and everywhere else (`:565-566`); the debug CRT in any staged binary's import table (`:581`); the release CRT not locatable, or the redist missing a DLL the binaries import (`:417`, `:421`); `Debug` without `-AllowDebugCrt` (`:119`); a missing third-party licence file (`:531`).
>
> **Landed but not on this list, and each is load-bearing:** the full **import closure** — refusal 8b fails on *any* imported DLL that is neither OS-provided nor staged, which is the check that catches a dependency nobody thought of rather than three named ones; `AverEngineRuntime.exe` absent from the package (8c); **`AverEngineRuntime.exe` containing Dear ImGui**, proven by scanning the staged binary's own bytes for four linker-visible markers rather than by inferring it from `AVER_ENABLE_UI` (8d — a config flag stopped implying this after the ImGui/RHI split, so the inference was replaced with evidence); a `CMakeCache.txt` that does not mention one of the eight options the script reads, because *absent is not OFF* and an unknowable edition must not be staged (`:98`); `AVER_BUILD_GAME=OFF` (`:116`); a manifest with no `STARTMAP` or a missing `CONTENT` root (`:139`, `:147`); and an allowlist entry whose `?` condition the cache does not mention (`:320`) or which matches no file while its option is ON, or *any* file while its option is OFF (`:348-350`).
>
> **Not written, and still worth writing:** the `Aver.Scripting` contract-version check on each script DLL; the F#-without-`FSharp.Core` refusal; the per-`objectId` assertion against the packaged `Content\` index — the one check the loose-file model uniquely *can* make, and the one that would catch "the level references a mesh that lives in the dev tree" at stage time instead of at verify time; and the per-binary notices assertion (see (e)).
>
> **Moved rather than dropped:** the absolute-path scan lives in `verify-game.ps1`, not here, and it scans a text-extension allowlist rather than every staged file. Its own comment explains the narrowing: the first version used `-Include`, which PowerShell silently ignores on a wildcard-free `-LiteralPath`, so it scanned every `.dll` too and reported eight "failures" for managed assemblies that legitimately carry their build paths in the debug directory. That is an information leak worth its own conversation, not a reason a package will not run.

**Editor button — `File ▸ Package Project…`** in `sandbox/src/ToolsMenu.cpp`, beside `Compile Scripts` (`:827`). That file already owns the background-thread shell-out (`startCompile`, `:715`), the progress modal (`:549`), the exit-code reap (`:695`), and the staleness check — the packaging item reuses all four and runs `powershell -File scripts/stage-game.ps1`. Disabled with a specific tooltip when: no project loaded / no `Binaries\Scripts` / scripts stale / no staged engine payload to draw binaries from. **The menu item must never be the only path** — a check that can only be run by clicking is not a check.

**One honest wrinkle.** A user with a *downloaded* engine has no `scripts/` directory: `payload.allowlist` ships none. So `[engine]` must gain exactly two lines — `scripts/stage-game.ps1` and `scripts/game.allowlist` — and **not** `scripts/**`, which would also ship `gates.ps1` and the recorded baselines.

> **STILL OPEN, and "exactly two" is wrong, 2026-09-20.** `payload.allowlist`'s `[engine]` section still has no `scripts/` line of any kind, so a downloaded engine cannot package a game — and the editor's own Package Project code already assumes otherwise: `sandbox/src/ToolsMenu.cpp:1102` comments that "a payload ships `stage-game.ps1` but not the whole repo". It ships neither.
>
> Two lines would not be enough in any case. `stage-game.ps1:77` dot-sources `scripts/PeImports.ps1`, which is where `Get-PeImports`, `Find-VcRedistDir` and the three CRT patterns live, so a payload carrying the first two files would fail on its first run with a missing-function error. **Three lines, not two** — and a fourth, `scripts/verify-game.ps1`, if a downloaded engine is meant to be able to *check* the package it just made, which is the whole argument of section (g). Deciding that is a payload question, not a documentation one, which is why it is recorded here rather than quietly fixed.

---

## g) Verification

The question is *"how does anyone know a package works, and that it is not secretly reading the dev tree?"* Three checks. Two ship in slice 1. The third is honestly deferred, with the reason stated.

### Prerequisite: the game exe grows the capture contract

`AverGame.exe --frames N --probe-rel U V --screenshot <png> --width W --height H --trace-opens`.

> **WHAT IT ACTUALLY GREW, 2026-09-20.** `AverEngineRuntime.exe` takes `--frames`, `--width`, `--height`, `--screenshot`, `--headless`, `--trace-opens`, `--no-vsync`, `--scene-census`, `--play-test` and a good deal more. **`--probe-rel` and `--no-world` were never added to it** — `--probe-rel` exists on `Sandbox.exe` only, and `--no-world` exists nowhere in the tree. Everything below that assumes those two flags describes a check that was not built; what replaced it is the divergence gate, and the substitution is a better answer than the original. See Check 2 and Check 3.

Cheap, because the capture primitives are **engine-side, not editor-side**: `requestCapture(x,y)`, `getCapture(f32[4])` and `getFrameImage(bytes,w,h)` are virtuals on `rhi::IDevice` (`modules/rhi/include/aver/rhi/RHI.hpp:371-375`). `Engine::run` already treats `maxFrames>0` as a capture run — no splash, window opened unactivated (`modules/runtime/src/Engine.cpp:19-42`), which honours the standing "capture runs must not steal focus" rule. `GameApp::captureCheck` is `SandboxApp::captureCheck` (`:4877`) with `vpX_/vpY_/vpW_/vpH_` replaced by the whole backbuffer — which is *simpler*, because a game has no dockspace and no editor chrome to accidentally probe.

### Check 1 — the isolation check (the one the question is really about)

The claim to disprove: *the package is silently reading the dev tree.*

**Mechanism A — the engine says what it opened.** Add `--trace-opens` to `AverGame`: a log sink over `platform::readFileBytes` / `readFileText` (`modules/platform/include/aver/platform/FileSystem.hpp` is the whole surface — there is no other file API in the engine) plus the `stbi_load` path in `assets::uploadTexture`. `verify-game.ps1` asserts **every logged path is under the package root**. This catches a dev-tree dependency *by name*, with the offending path printed, instead of by a symptom. It ships with the mechanism it checks, which is the house rule.

> **BUILT — and "`FileSystem.hpp` is the whole surface" was wrong, which nearly made the whole check worthless.** `verify-game.ps1`'s own header records the measurement: the TEXT loaders (`OcProject`, `OcWorld`, `OcMap`, `OcMat`, `OcBeam`) go through `readFileBytes`/`readFileText`, but the BINARY ones (`Avr1`, `OcMesh`, `OcAnim`) open their own `ifstream`. A packaged game reported exactly **two** traced opens — the manifest and the level — and would have passed while loading every mesh from the dev tree. Those three call `aver::traceFileOpen` explicitly now, and the habit spread — `Avr1`, `OcMesh`, `OcAnim`, `OcBt`, `OcFoliage`, `OcNav`, `OcSave` and `OcSound` all do. That is the whole point of this check in one example: a package that falls back to a dev-tree asset runs perfectly on the machine that built it, and only the list of what it opened tells the two apart.

State its limit rather than overselling it: it covers `aver::platform` and the format loaders. It does **not** cover `LoadLibraryW` (`dxcompiler.dll` at `D3D12Device.cpp:70`, `nethost.dll` at `ScriptHost.cpp:130`, then hostfxr), `dotnet`'s own probing, or anything inside the CLR.

**Mechanism B — run it where the dev tree is not an ancestor.** Copy the package to `%TEMP%\aver-game-verify-<guid>\` and run it *there*, with the process working directory set to `C:\` rather than the package directory. Two properties fall out for free:

- Nothing up that chain contains `cmake/AvModule.cmake` **and** `modules/`, the two markers `EngineScaffold::engineRoot()` requires, so the whole `ProjectScaffold` walk-up class of bug is dead by construction.
- A working directory that is not the package root exposes any relative-path fallback that happens to work only when you `cd` first.

If a second drive root exists, prefer it — `std::filesystem::relative` returning an empty path across roots is the specific behaviour this repo already suspects (`ProjectScaffold.cpp:26` has an `rel.empty()` guard that exists for *some* reason), and **that inference is not yet verified; it deserves a five-line test before anyone leans on it.**

**Mechanism C — the environment is scrubbed.** `Start-Process` with `PATH` reduced to `%SystemRoot%\system32;%SystemRoot%`, `DOTNET_ROOT` and `AVER_*` unset. If the package needs the dev machine's environment, it fails here.

I deliberately do *not* propose Process Monitor. It would be stronger for `LoadLibraryW`, but it is an external, non-redistributable tool, and a check nobody can run is not a check.

### Check 2 — it reached a frame and drew something

> **WHAT `verify-game.ps1` ASSERTS TODAY, 2026-09-20.** Assertion 1 landed in a stronger form than
> proposed; assertions 2 and 3 did not land at all.
>
> **Landed:** exit code 0 from the isolation run; the log must contain `[RHI] device created`, because
> a process that failed to initialise and gave up quietly would otherwise read as a pass; the log must
> contain no `ImGui` at all. Two details are measured rather than assumed and are worth carrying
> forward — the script concatenates **both** `stdout` and `stderr` before matching, since the engine
> splits one log across two handles and reading `run.out` alone made a healthy package look like one
> that never made a device; and **a run that logs no `[open]` lines is a FAIL**, not a pass, because
> it means `--trace-opens` is not wired and the script proved nothing.
>
> **Not landed: the `--no-world` probe relation (assertion 2).** Neither flag it needs exists, and no
> screenshot is taken or compared anywhere in `verify-game.ps1`. The question it was meant to answer —
> *did geometry actually reach the frame* — is answered instead by the divergence gate's census, which
> counts the entities, meshes and materials the host loaded. That is a weaker claim about pixels and a
> much stronger one about content, and it is the claim the package's original deletion was about.
>
> **Not landed: the `OnBeginPlay` assertion (assertion 3).** Nothing greps the log for a game mode's
> begin-play line, so the four failures listed below — a missing .NET runtime, a version-mismatched
> bridge, a missing `FSharp.Core.dll`, an empty `Binaries\Scripts` — are still invisible to
> verification. It is the cheapest unbuilt check in this document: one regex over a log the script
> already has in a variable.

Three assertions, none of which needs a recorded baseline.

1. **It reached a frame.** Exit code 0, and the log contains `Aver Engine stopped after 60 frame(s)` — that string already exists at `Engine.cpp:107`. A probe line must be present at all; if every probe is `NO-PROBE` that is a **FAIL**, copying `verify-payload.ps1`'s explicit refusal of the degenerate pass ("agrees with itself while proving nothing").

2. **It drew something — as a relation, not a number.** Run the *same* package twice at the *same* width and height:
   ```
   AverEngineRuntime.exe --frames 60 --width 1600 --height 900 --probe-rel 0.5 0.5 --screenshot a.png
   AverEngineRuntime.exe --frames 60 --width 1600 --height 900 --probe-rel 0.5 0.5 --no-world --screenshot b.png
   ```
   and assert the two probe codes **differ**. A probe equal to the clear colour proves nothing; a probe that *changes when the world is removed* proves geometry reached that pixel. No baseline, no recorded value, nothing to go stale. `--no-world` is a new flag on `GameApp` that skips `loadStartMap` and the world draw.

   Two constraints from the standing notes are non-negotiable and must be enforced by the script, not assumed:
   - **identical aspect ratio, not merely identical size** — `--probe-rel` survives a resize but *not* a change of aspect. Both runs pass explicit `--width`/`--height`, and the script asserts the two screenshots came out the same dimensions.
   - **screen the probe for a flat 7×7 neighbourhood** in `a.png` before trusting it, so a one-pixel feature is not being read as signal. `gates.ps1` documents this trap at length (`:136-171`); it has already caught this repo once.

3. **Gameplay actually ran.** If the package contains `Binaries\Scripts\*.dll` and `AVER_MODULE_SCRIPTING` is on, the log must contain the GameMode's `OnBeginPlay`. This single line catches a missing .NET runtime, a version-mismatched bridge, a missing `FSharp.Core.dll`, and a `Binaries\Scripts` that was staged empty — the four failures that a rendering probe cannot see.

### Check 3 — package vs. dev tree, the direct analogue of `verify-payload.ps1`

Run the editor over the *source* project and the packaged game over its *copy*, same probe, same window size, and compare the two probe codes **to each other**. This is exactly `verify-payload.ps1`'s argument — comparing two binaries removes the baseline from the question, so a disagreement means one thing ("packaging lost something") instead of two.

**Be honest: this does not work today, and slice 1 must not claim it.** The editor probes a viewport rect inside a dockspace and draws its own `objects_` scene; the game probes the whole backbuffer and draws only the world. The two images are not comparable until `GameApp` and `SandboxApp` share the *same* extracted world-draw code and the editor is driven with a level loaded and the viewport maximised. **Check 3 is Slice 2.**

> **BUILT, 2026-09-20 — as a census, not as a probe, and the change of instrument is the point.**
> `verify-game.ps1`'s divergence gate runs both hosts over the *same* `Game.ocproject` inside the
> package, with `--scene-census`, and compares the single `[Census]` line each prints: entity count,
> distinct mesh and material counts, and an order-independent hash of every (mesh, material) pair.
> A mismatch fails and prints both lines.
>
> **Why not the probe comparison this section proposed.** The obstacle described above is real and
> did not go away: the editor's 3D viewport is a sub-rect of its window, so the two hosts have
> different aspect ratios and therefore different projections, and their cameras differ by design. A
> pixel comparison would be dominated by framing and would fail for reasons that have nothing to do
> with divergence. What the deletion commit actually complained about was the **subset of the scene**
> each host rendered, so that is what is compared. See `modules/world/include/aver/world/SceneCensus.hpp`.
>
> **Three things the gate learned the hard way, all of them in its own comments:**
>
> - **The editor is run with `--open-legacy`, and without it the gate could never pass** on a package
>   made from a project older than the current series. The editor refuses such a project when the run
>   has a frame limit, because the upgrade prompt is modal and nothing can answer it — it says so and
>   carries on with no project, so the editor's census came back `entities=0` against the game's real
>   scene and the gate called that a divergence. Opening as-is is also the only honest comparison: the
>   packaged runtime has no upgrade prompt and no way to rewrite the manifest it ships with.
> - **The editor defaults to the tree matching the package's own `config`**, read out of `game.json`.
>   It used to be a hardcoded `build/bin/Sandbox.exe` while `stage-game.ps1` defaults to `-Config
>   Release`, so the default pairing compared a *Debug* editor against a *Release* runtime and neither
>   script said so. The census is configuration-independent in principle, so that usually still
>   passed — which is worse than failing, because the gate was quietly answering a question nobody
>   asked.
> - **A host that prints no `[Census]` line is a failure, not a skip.** Only a *missing editor* skips,
>   and that is deliberate: this script's subject is a package, and a package legitimately ships with
>   no `Sandbox.exe` beside it, so a missing editor means the question cannot be asked rather than
>   that the answer is bad. `-SkipDivergence` exists for exactly that machine.
>
> **This is the check whose absence deleted the executable.** The removal commit's reason was that a
> second host "rendered a different subset of the scene than the editor" and that nothing in the tree
> could notice. Something can now.

### Static checks, which cost nothing and run every time

- Walk every staged file for `[A-Za-z]:\`, leading `\\`, `\\?\`.
- Walk every staged PE's import directory (reuse `Get-PeImports` from `stage-payload.ps1:331`): no debug CRT, and every import is either a Windows DLL, an `Aver.*` sitting beside it, or one of the named redistributables.
- Every staged binary has a `THIRD-PARTY-NOTICES.txt` section.
- `game.json` `fileCount`/`totalBytes` match the tree on disk.

> **WHERE THESE ENDED UP, 2026-09-20.** They are split across the two scripts rather than gathered in
> one, which is right — a check that needs the build tree belongs to staging and a check that needs
> only the package belongs to verification — but it means neither script matches this list.
>
> - **The absolute-path walk is in `verify-game.ps1`**, narrowed to text extensions (`.json`,
>   `.ocproject`, `.ocmap`, `.ocworld`, `.ocmat`, `.txt`, `.ini`, `.cfg`) and to a pattern that looks
>   for `Users`, `Program Files` or `Windows` after a drive letter. See the note in (f) for why it is
>   not every file: scanning `.dll`s reported managed assemblies' debug-directory build paths as
>   failures.
> - **The import walk is in `stage-game.ps1`** as refusal 8b, and it is stronger than described: it
>   needs no list of "named redistributables", because anything staged counts as resolved and the
>   `api-ms-win-*` and OS-provided sets are shared through `PeImports.ps1`. It also prints how many
>   binaries it checked out of how many it found, which is the line that reveals its blind spot —
>   managed assemblies have no import table, which is how `Aver.Settings.dll` went missing (see (e)).
> - **`verify-game.ps1` also refuses `.cs`, `.fs`, `.csproj`, `.fsproj`, `.pdb`, `.ilk`, `.exp` and
>   `.lib` anywhere in the package**, and refuses a `game.json` that begins with a UTF-8 BOM. The BOM
>   check is not tidiness: `System.Text.Json`'s `JsonDocument.Parse` over bytes rejects one outright.
> - **Not built:** the per-binary notices assertion (see (e)), and the `fileCount`/`totalBytes`
>   reconciliation — `stage-game.ps1` writes both fields and nothing ever compares them to the tree.

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

  > **WRONG ON BOTH HALVES, 2026-09-20.** `WindowDesc` is `{title, width, height, resizable, startHidden, activate, fullscreen}` (`modules/platform/include/aver/platform/Window.hpp:11-38`). Borderless fullscreen has a field, a ten-line comment explaining why it is `WS_POPUP` and never DXGI exclusive mode, and a Win32 backend that honours it — and a documented interaction that matters more here than the feature does: **it is ignored when `activate` is false**, because a capture run's window is a measuring instrument and a window that sized itself to whatever monitor it ran on would move every recorded gate probe. `startHidden` is there for the splash. What is genuinely missing is the *wiring*: `WINDOW.FULLSCREEN` and `WINDOW.RESIZABLE` are parsed, serialised and editable in Project Settings, and no host reads either — the manifest's `WINDOW.TITLE` and `WINDOW.SIZE` beside them *are* applied at `Runtime/src/GameApp.cpp:587-591`. And the size does not come from `game.json`; see the correction in (a).
- **Save games: no longer true — a working system landed 2026-08-21, after this document was already HISTORY.** `grep SaveGame` no longer returns nothing; it returns a real system, four layers deep:

  - **On disk, wherever `path=` says, with no save directory of its own.** `Game.SaveGame(path)`/`LoadGame(path)` (`scripting/csharp/Aver.Framework/Game.cs:68,81`) forward straight to `aver_fw_save_write`/`aver_fw_save_load` (`FrameworkAbi.cpp:760,765`), which hand the path unchanged to `fmt::saveOcSave`/`loadOcSave` (`modules/formats/src/OcSave.cpp`). There is no `%LOCALAPPDATA%` resolution, no save-slot convention, no default filename — a relative path is relative to the process's current working directory, exactly like every other file path in this engine. Parent directories are created if missing. The write is genuinely atomic: bytes go to `path + ".tmp"`, then `renameFile` (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`) swaps it into place, and a failed swap deletes the temp file and leaves the previous save untouched (`OcSave.cpp:259-287`).
  - **Reachable two ways: a C# call, or an Aver Node graph with no entity pin at all.** `SaveGame`/`LoadGame` are exec nodes in the `Game` category (`sandbox/src/GraphNodeDefs.hpp:200-203`) carrying one node-line attribute, `path=` — the same "value is data, not a pin" mechanism `socket=`/`curve=`/`mesh=` already use, because `PinType` has no string member. Both act on the *whole* world, which is why neither takes an entity input.
  - **What a save captures.** `aver::save::capture()` (`modules/save/src/SaveWorld.cpp`) walks every live entity through the engine's reflection API — every registered component, every non-`readOnly` field, by name — with no hardcoded list of "known" components. Three kinds of state are excluded, each named, not silently dropped: `CWorld` (recomposed from `CLocal` every frame — writing it would restore a matrix the next flush overwrites anyway), `CHierarchy` (parent/child travel as an array index instead, since an entity handle means nothing in a file), and `CName`'s raw `offset`/`len` (a slice into this process's own name blob). Graph-local `VAR`s are a fourth, narrower case: they live entirely in managed memory with no native component behind them, so they ride along as a synthetic `$GraphVars` component — captured only for entities whose class is `GameInstance` or a subclass (`HostBridge.cs:1580-1591`, `FindPersistedGraphHost`), not for every graph-hosted actor. A plain level-dressing graph's VARs still reset to their declared defaults on load; that is a stated design choice ("persistent state belongs to the class meant to carry it across a level transition"), not a bug.
  - **What restore does that a naive "spawn then patch" would get wrong.** `aver_fw_spawn` (`FrameworkAbi.cpp:620-624`) is a three-line wrapper that calls the static `spawnActor` helper with `dispatchBeginPlay=true`, and `spawnActor` (`FrameworkAbi.cpp:535-580`) is what actually runs `bind → build_models → beginPlay` inline — restoring an actor and then writing its saved fields over it would already have run `OnBeginPlay` against the class defaults — a door reading `isOpen` sees the *wrong* answer. `aver_fw_dispatch_begin_play` (added `66843a6`) splits that: `aver_fw_spawn_preview` creates the actor without dispatching begin-play, fields are restored, and only then is `OnBeginPlay` dispatched for every restored actor at once (so one actor's begin-play sees every *other* restored actor already in the world, not just the ones created before it). Teardown before a load destroys the current world through the framework's own destroy path, so `OnEndPlay` runs and no managed instance is torn out from under live C# code. A field whose type changed or no longer exists is dropped and counted, not crashed on; a component the build no longer registers logs a warning and the entity restores without it; a failed restore leaves the world **empty**, not half-populated.
  - **Where it's actually installed, and what that implies.** Both composition roots install the same provider pair: the editor's in `sandbox/src/SandboxApp.cpp`, and the runtime's at **`Runtime/src/GameApp.cpp:1344`** (`aver_fw_set_save_provider`, over the `saveHost()` built at `:186`) — the `modules/runtime.game/src/GameApp.cpp:875` recorded here is a path that no longer exists, and the `SandboxApp.cpp:2185` beside it has drifted twice before (from `2352`, itself from `1816`) and is not re-resolved here. That is the honest state of this feature: real, tested at the C++ level (`tests/save/src/SaveWorldTest.cpp`, `SaveActorTest.cpp`, a real-CLR test) and at the graph level (`Aver.Graph.Tests/SaveLoadGameNodeTests.cs`).

    > **THE CLOSING CLAUSE IS NOW FALSE, 2026-09-20.** It read "never exercised by a standalone game process, because there is no standalone game process", and the editor's own comment quoted here said the editor "is the only place [save/load] can be tested at all". `Runtime/host/` builds `AverEngineRuntime.exe`; this document's own banner announces exactly that. The process exists, it installs the providers, and nothing in `verify-game.ps1` exercises save/load through it — which is a **gap in verification**, not an absence of a host, and those are worth telling apart because only one of them is cheap to close.
  - **What is NOT there.** No skip predicate is wired up by either composition root, even though `CaptureOptions`/`RestoreOptions` both carry one specifically for a host that streams world chunks (`SaveWorld.hpp`'s own comment: to avoid double-persisting content `world::RegionFile` already owns). A project using chunk streaming would have its streamed content captured into the save file a second time today. There is no save-slot UI, no thumbnail/metadata, no versioning beyond a dropped-field warning, and no encryption. Physics body state (velocity, wake state) round-trips only to the extent it is an ordinary, non-`readOnly` component field — it is not treated specially.

  **Settings are a separate, smaller story — corrected.** `modules/settings` (`aver_settings_open/get_*/set_*`, added `103e4df`) is a durable key/value store, deliberately not part of a save. This paragraph originally said it had zero callers *and* no C# binding, so nothing reachable from a script or a graph could read or write a setting. The second half stopped being true at `160bcdd`: `scripting/csharp/Aver.Framework/Settings.cs` now wraps the whole ABI (`Open`/`OpenDefault`/`Flush`/`GetFloat`/`GetInt`/`GetBool`/`GetString`/`SetFloat`/`SetInt`/`SetBool`/`SetString`/`Has`/`Remove`/`Count`) — its own header comment calls itself "the FIRST C# binding to it," written with per-player input rebinds as the intended first caller. `aver_settings_default_path()` resolves to `%LOCALAPPDATA%\AverEngine\settings.ini` (verified in `FileSystem.cpp`'s `userDataDir()` + `Settings.cpp`'s `aver_settings_default_path`). `%LOCALAPPDATA%\AverEngine` remains the *editor's* separate prefs store regardless (`EditorPrefs.cpp:145`: "Nothing about a PROJECT is stored here") — settings and editor prefs are two different files.

  > **"NOTHING CALLS IT YET" IS FALSE, 2026-09-20, AND ITS BEING FALSE WAS A PACKAGING DEFECT.** The
  > sentence removed here read: *no script or graph anywhere in the tree references
  > `Aver.Framework.Settings`, and no composition root calls `aver_settings_open` natively either*.
  > The intended first caller arrived and closed both halves. `EnhancedInput.cs:307-338` saves and
  > loads every rebind through `Settings.SetString`/`GetString`/`Has`, and both composition roots open
  > the store before any of that can run: the runtime at `Runtime/src/GameApp.cpp:1314-1317` and the
  > editor at every Play start (`sandbox/src/SandboxPlay.cpp:168-169`), each handing
  > `ScriptHost::configureInput` a settings path that `HostBridge.cs:440` passes straight to
  > `Settings.Open`. The runtime's path is built unconditionally, so this runs on **every launch**.
  >
  > The consequence landed squarely in this document's subject. Because the call is a managed
  > `DllImport` and nothing native links `Aver.Settings`, the DLL appeared in no import table, the
  > import-closure refusal could not see it, and **`Aver.Settings.dll` was staged by neither
  > allowlist and named nowhere in this file.** Fixed 2026-09-20: it is in both, unconditional, with
  > the argument written at the entry. See (e) for the whole account, including why no condition can
  > correctly be attached and why the same shape will recur for the next managed-only seam.
  >
  > The paragraph's practical conclusion was right for the wrong reason and is now wrong outright: a
  > per-project settings value written by gameplay code **does** have somewhere to land, and it is
  > neither a save file nor `%LOCALAPPDATA%\AverEngine\settings.ini`. The runtime writes to
  > `userDataDir()\<project name>\Settings.ini` — per project, deliberately, so two packaged games
  > from the same engine do not share one file. `aver_settings_default_path()` is what a caller gets
  > when it opts out of that, not what a shipped game uses.
- **Audio — corrected.** This used to say no managed contract assembly existed and the game was silent. Both halves are stale: `Aver.Framework/Audio.cs` now wraps the full ABI and the editor opens the audio device on entering Play (`SandboxApp.cpp:1559`, landed `dbfd6a6`), so the editor — which is how a project runs, per the header — is not silent. There is still no packaged, standalone game exe to be silent or not; that half of the sentence was overtaken by the exe's deletion, not fixed.
- **Gamepad — corrected twice, and now the other way.** This first said no path existed at all. It was then softened to "shape only, no polling, zero consumers", quoting `framework_abi.h`'s own header. **Both are stale as of 2026-09-20: a real XInput poller exists and a shipped game reads a controller.** `modules/platform/src/win32/Gamepad.cpp` polls XInput with hotplug-safe re-probe throttling and XInput's own documented radial dead zones, `Runtime/src/GameInput.cpp:163` calls `pollGamepads` inside `publishGamepad`, and both hosts reach it through `publishInput` — the editor's direct call was deleted so the device is not polled twice a frame and the second answer published over the first. What remains true is the narrower thing: the pad is polled every frame and only the published *value* is gated on whether the game owns the device.
- **Dedicated server / true `--headless` game mode.** `Engine::run` knows "capture run" and "no window"; neither is a server.
- **Splitting ImGui out of `Aver.RHI.D3D12`.** Worked around with a second build tree.
- **`Scripts.csproj` `<Reference>`+`<HintPath>` conversion.** Real bug, wrong slice — packaging excludes that file entirely.

### Slice order

> **WHERE THE SLICES STAND, 2026-09-20.** Read the list below as the plan it was; this is what
> happened to it.
>
> | Slice | State |
> |---|---|
> | 0 — capture contract | **Mostly.** `--frames`, `--width/--height`, `--screenshot`, `--trace-opens`, `--headless` and `--scene-census` all exist on `AverEngineRuntime.exe`. `--probe-rel` and `--no-world` do not, and the checks that needed them were replaced rather than built. |
> | 1 — the package | **Done**, with the naming from the 2026-09-16 rename: `Runtime/` + `Runtime/host/` + `AverEngineRuntime.exe`, all three scripts, the CRT staging, and `File ▸ Package Project…`. The derived-notices *assertion* is the one piece that did not land — see (e). |
> | 2 — extraction, editor calls the library | **Done, and past what this section asked for.** The editor's content index, level load, water, landscape, input publish and world-draw walk are all the runtime library's now; `GameContent::adopt`, `game::publishInput` and `game::drawWorld` each have one implementation that both hosts call. The depth prepass was the last duplicate and it is deleted. |
> | 3 — input off ImGui | **Done.** Both hosts publish through `game::publishInput` with a named `InputPublishPolicy`; the editor's separate publisher is gone, and `resolveInputOwnership` is what keeps `WantCapture*` a filter rather than the source. |
> | 4 — the ImGui/RHI split | **Done** (`c71e3fc`). The check below needs one correction: ImGui is *statically* linked, so there is no import for an import-table walk to find absent. Refusal 8d scans the staged executable's own bytes for four ImGui markers instead — real evidence for a static dependency, where an import walk would have proved nothing. |
> | 5 — presentation | **Not done, and closer than it reads.** `WindowDesc` grew `fullscreen` (borderless, never DXGI exclusive) and the Win32 backend honours it; `WINDOW.FULLSCREEN`/`WINDOW.RESIZABLE` are parsed and editable but reach no host. See the correction under "Fullscreen, borderless…" above. |
> | 6 — `.ocpak` | **Not done.** Still no reader and still no vendored codec. |
> | 7 — notices retrofit to `stage-payload.ps1` | **Not done.** Nor is the assertion it was to be a retrofit *of*. |

**Slice 0 (small, enabling).** `AverGame`'s capture contract: `--frames`, `--width/--height`, `--probe-rel`, `--screenshot`, `--no-world`, `--trace-opens`. Nothing to verify yet; it is what makes everything after it verifiable.

**Slice 1 — the package.** `modules/runtime.game/` + `game/` + `AverGame.exe`, `scripts/stage-game.ps1`, `scripts/game.allowlist`, `scripts/verify-game.ps1`, the release-CRT staging, the derived notices assertion, and `File ▸ Package Project…`. **Done when:** SkyForge packages to `%TEMP%`, runs 60 frames from a scratch copy with a scrubbed environment and `cwd = C:\`, opens no file outside the package, prints `OnBeginPlay` for `FpsGameMode`, and its centre probe changes when `--no-world` is passed.

> **Of that "done when", three of five clauses are checked today.** The package is copied to `%TEMP%`, run from there with `cwd = C:\`, and every traced open is asserted inside the copy — with a run that logs *no* opens treated as a failure. The `OnBeginPlay` assertion and the `--no-world` probe relation were never built (see Check 2); the divergence gate answers the second one's underlying question and nothing answers the first. The environment is also **not** scrubbed: `verify-game.ps1` inherits the caller's `PATH` and `DOTNET_ROOT` rather than reducing them, so Mechanism C in (g) remains a proposal.

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
- `scripts/payload.allowlist` names 17 test executables in its "NOT SHIPPED" note (now at `:186-190`, moved by the `Aver.Settings.dll` entry added 2026-09-20); `build/bin` holds **140** `*Test.exe` today, in 157 executables total. Comment drift only — the allowlist excludes them by construction, which is the whole argument for an allowlist, and the point of the note is the *reason* rather than the roster. Left as prose rather than re-enumerated: a list of 140 names would rot faster than the one it replaced.