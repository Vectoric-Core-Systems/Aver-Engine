# Fabric Connector as an engine extension

Fabric moves out of the engine repo into its own public repo, `Vectoric-Core-Systems/Fabric-Connector` (LGPL-2.1),
and ships as one `.avext` file. The engine gains an extension loader and neutral hook points; nothing in engine core
names Godot or Fabric when the move is done. This is the design; nothing here is built yet.

## 1. Where Fabric is today

| Moves wholesale | Size |
|---|---|
| `modules/formats.godot/` (Godot text/binary readers, Zstd, shader-language front end) | 34 files, ~10.8k lines |
| `modules/fabric.gdext/` (GDExtension host, `Aver.Fabric.GDExt.Abi`, vendored `third_party/godot-gdextension`) | 16 files, ~6.4k lines |
| `tools/fabric/` (the `AverAssetC fabric` converter) | 26 files, ~9.4k lines |
| `scripting/csharp/Aver.Fabric/`, `Aver.Fabric.Convert/`, `Aver.Fabric.Tests/` | 186 files, ~74k lines |
| `tests/fabric/`, `tests/formats.godot/`, `sandbox/src/FabricImport.*` | ~190 files, ~18.7k lines |
| `docs/FABRIC.md`, `docs/plans/fabric/`, `docs/api/Aver.Fabric.md`, `docs/physics/GODOT_*.md` | |

What breaks in the engine if those directories vanish today:
- `Aver.Scripting.Bridge` references `Aver.Fabric` and calls `DeclareActors(typeof(Aver.Fabric.Node).Assembly, ...)`
  (`HostBridge.cs:1384`).
- `modules/scripting/CMakeLists.txt` builds `Aver.Fabric.Convert` and lists `Aver.Fabric.dll` as a bridge output.
- `AverAssetC` compiles `tools/fabric/*.cpp` and dispatches `fabric` (`tools/AverAssetC.cpp:47, 2069`).
- The editor builds `FabricImport.cpp`, which adds File > Import Godot Project (`SandboxShell.cpp:10, 1987, 2641`).
- Some tests reach into Fabric:
  - `VoxiSpirvCompileTest` includes `tests/formats.godot/src/GdGraphBridge.hpp`.
  - `MeshExtraTest` reads `tests/fabric/data/.../quad.gltf`.
  - `AbiEnumTest` parses Fabric's `*Native.cs` files. It is the only cross-check for the anim, nav, net and UI-style ABIs.
- The game and payload allowlists name `Aver.Fabric.GDExt.Abi.dll` and `extension_api.json` without a condition.
- `ProjectScaffold.cpp` gives every `Scripts.csproj` a reference to `Aver.Fabric.csproj`.
- `Aver.Framework` grants `InternalsVisibleTo` to Fabric. Fabric uses several framework internals:
  - the `Fw.*` P/Invokes;
  - `Actor.Self`, `AverActor.OfferPending` and `EditablesApplied`;
  - `GameThread.BeginPass`/`EndFrame`.

## 2. What stays in the engine, renamed

Three kinds of code stay in the engine.

General features that already work for any game: these keep their behaviour and lose the Godot name.
- the physics layer/mask filter;
- Godot-unit body and joint mapping;
- the Godot tonemap curves;
- the Godot light-falloff model;
- the audio filter, pan and 3D model;
- the UI default theme, style box and stretch;
- prop animation, nav, net and the subview/rtmesh/loc ABIs.

File keywords that projects already contain (`PHYSICS.COMBINE godot`, the tonemap names, `GDSHADER` in particle files)
are still read as aliases, and new files are written with the new names.

Hook points: the extension's shader translator emits HLSL, and today's engine-side receivers carry Godot names. They
become a general "embedded HLSL program" feature that any tool can target.

| Today | Becomes |
|---|---|
| `OcGraphGodot` (`NODE gd GodotShader` / `GodotEnvShader`) | `OcGraphHlsl` (`NODE hlsl EmbeddedHlsl` / `EmbeddedHlslEnv`) |
| `material_godot*.hlsl`, `GdShaderParams`, `kGdTableSlot`, `gdVertex`/`gdDiscard`, `aver_pbr_*_gd_register*` | `material_embedded*.hlsl`, `EmbeddedParams`, `kEmbeddedTableSlot`, `embVertex`/`embDiscard`, `aver_pbr_*_embedded_register*` |
| `framework_godotenv_abi.h` (`aver_fw_genv_*`), `GodotEnv.cs`, `GodotEnvFeed.hpp`, `GodotEnvPrograms` | `framework_customenv_abi.h` (`aver_fw_cenv_*`), `CustomEnv.cs`, `CustomEnvFeed.hpp`, `CustomEnvPrograms` |
| `godot_{env_common,sky,fog,particles}.hlsl`, `AVER_GD_*` | `custom_{env_common,sky,fog,particles}.hlsl`, `AVER_CUSTOM_*` |
| `setGodotSky`, `PSGodotSky`, `setGodotParticlePrograms`, `kGpuFlagGodot` | `setCustomSky`, `PSCustomSky`, `setCustomParticlePrograms`, `kGpuFlagCustom` |

Fabric code that sits in core but has no other user: it moves with Fabric.
- `GodotEnvironment.hpp`;
- `GodotLight.hpp`, after its test checks move to the renamed light model;
- `Aver.Physics/GodotJoints.cs` and the `RaycastVehicle.*FromGodot` helpers;
- the `Aver.Fabric` line in `scripts/csharp-api-index.py`.

`Csg` and `PanoramaSky` stay in the engine: they have engine tests and they are general.

`anim.abi`, `nav.abi`, `net`/`net.abi` and the UI style ABI stay, as decided on 10-30. After the move nothing in the
engine calls them from C#. `AbiEnumTest` keeps its C-side signature rows. The check that the C# side matches moves to
the extension repo's CI, which runs it against the pinned engine headers.

## 3. The extension contract

### 3.1 Files on disk

An installed extension is a directory: `<engine>/Extensions/<Name>/`. The `.avext` file is that directory zipped.

```
FabricConnector/
  extension.json
  native/win-x64/Aver.Fabric.Native.dll      (linux-x64/*.so, osx-arm64/*.dylib)
  managed/Aver.Fabric.dll                     runtime; converted scripts compile against it
  tools/Aver.Fabric.Convert.dll (+ Roslyn)    conversion time only, never shipped in a game
  data/extension_api.json                     GDExtension API description
  LICENSE, README.md
```

What was asked for as "one massive DLL" works out like this:
- Everything native goes into one DLL per platform: the readers, the converter, the GDExtension host and the shader
  translator.
- The managed assemblies cannot go inside that DLL, because converted projects compile against `Aver.Fabric.dll`.
- The single download is the `.avext`.

`extension.json`:

```json
{
  "name": "FabricConnector",
  "displayName": "Fabric Connector",
  "version": "0.1.0",
  "abi": 1,
  "engine": ">=0.8.0 <0.9.0",
  "license": "LGPL-2.1",
  "source": "https://github.com/Vectoric-Core-Systems/Fabric-Connector",
  "native": "Aver.Fabric.Native",
  "managed": { "runtime": ["Aver.Fabric.dll"], "actorBase": "Aver.Fabric.Node" },
  "commands": [ { "name": "fabric", "summary": "convert a Godot 4 project" } ],
  "importers": [ { "menu": "Import Godot Project...", "command": "fabric", "input": "folder",
                   "detect": "project.godot", "opensResult": true } ],
  "runtimeFiles": ["native/{platform}/Aver.Fabric.Native.*", "managed/Aver.Fabric.dll", "data/extension_api.json"]
}
```

### 3.2 Native ABI (`modules/extensions/include/aver/extensions/aver_ext.h`, plain C)

The extension exports one function:

```c
int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out);
```

It returns 0 on success, and 1 when `host->abi` is one it does not support. There is no C++ across the boundary: only
plain structs, `const char*` in UTF-8, and function pointers.

- `AverExtHost`, provided by the engine:
  - the ABI version and the engine version;
  - `log(level, msg)`;
  - `executableDir()` and `extensionDir()`;
  - `importModel(const AverExtModelArgs*, AverExtModelResult*)`, today's `fabric::Host::importModel` (stems and
    placements are returned as arrays the host frees with `freeModelResult`);
  - `progress(fraction, text)`.
- `AverExtApi`, filled in by the extension:
  - `runCommand(name, argc, argv)`, which returns an exit code;
  - `shutdown()`.

Runtime P/Invokes, such as today's `aver_gdext_*`, stay ordinary exports of the same DLL. The engine never calls them.

The extension links its own copy of the engine's static libraries (Core, Formats, ...). Two rules follow from that:
- Only plain data crosses the boundary.
- At entry the extension points its copy of the Core log sink at `host->log`, so messages still reach the Output Log.

### 3.3 Loader (`modules/extensions`, new, Core-only)

The loader works in stages:
- At start it scans `<exeDir>/Extensions/*/extension.json` and validates each entry:
  - the name matches the directory;
  - the `abi` is one this engine supports;
  - the engine version is in the declared range;
  - the files the manifest lists are present.
- An entry that fails stays in the list, marked with the reason, and is never loaded.
- A native library loads lazily, the first time one of its commands runs, through the existing
  `aver::loadLibrary` (`modules/core/.../DynamicLibrary.hpp`).
- Anyone else can read the list:
  - the editor's Extensions panel;
  - `AverAssetC`, for unknown subcommands;
  - `ScriptHost`, for managed paths;
  - `stage-game.ps1`, which reads the JSON directly.

### 3.4 Command line

When `AverAssetC <cmd>` gets a command it does not know, it asks the loader. If an extension lists `cmd`, AverAssetC
loads that extension and returns `runCommand`'s exit code. `AverAssetC fabric ...` keeps working word for word, and
`AverAssetC --help` lists the extension commands with their summaries.

### 3.5 Editor

`FabricImport.cpp` is replaced by a generic import dialog. Each entry in `importers` adds a File menu item, here
"Import Godot Project...". The dialog:
- picks a folder;
- warns when the `detect` file is missing;
- runs `AverAssetC <command> <in> <out>` on a worker thread, as today, showing its progress lines;
- opens the result when `opensResult` is set.

No ImGui crosses the DLL boundary, so the editor never runs extension code in its own process for imports.

Extensions panel (Window > Extensions):
- It lists each extension with its name, version, source, licence and status (with the reason when it is refused).
- **Install from file...** reads an `.avext` and shows its manifest and sha256 before anything is copied. It unpacks
  into `Extensions/<Name>/` and refuses an existing name unless the user asks to replace it. The old copy goes to the
  Recycle Bin.
- **Uninstall** sends the extension to the Recycle Bin.
- Downloading from GitHub is left for later, and would need its own confirmation step.

### 3.6 Managed

- `ScriptHost` passes the installed extensions' `managed.runtime` paths to the bridge.
- The bridge loads them into the **host** context before project scripts, so type identity holds, because
  `ScriptLoadContext` resolves host-first.
- It then calls `DeclareActors(asm, name, t => actorBase.IsAssignableFrom(t))`, with `actorBase` taken from the manifest.
- The bridge's project reference to `Aver.Fabric` is removed.
- The `Fw.*` calls and other internals Fabric uses become a public `Aver.Framework.Hosting` surface, marked
  `[EditorBrowsable(Never)]`, documented as "for extensions" and covered by the API index.
- `InternalsVisibleTo Aver.Fabric*` is removed.
- `NativeResolver` gains a probe of the calling assembly's own extension directory (`../native/<platform>`) before the
  executable directory.
- `ProjectScaffold` stops adding `Aver.Fabric.csproj`. A converted project's `Scripts.csproj` gets a `HintPath` to
  `Extensions/FabricConnector/managed/Aver.Fabric.dll`, written by the converter.
- Converted projects record `EXTENSION FabricConnector 0.1.0` in `.ocproject`. If the extension is missing, loading
  reports that by name, instead of failing on a missing type.

### 3.7 Packaging

`stage-game.ps1` reads the project's `EXTENSION` lines and copies each extension's `runtimeFiles`, never `tools/`, into
`<game>/Extensions/<Name>/`. The Fabric entries in `game.allowlist` and `payload.allowlist` are removed.

## 4. The Fabric-Connector repo

```
Fabric-Connector/
  CMakeLists.txt        FetchContent of Aver-Engine at a pinned tag (AVER_ENGINE_DIR overrides for local work)
  native/               formats.godot, gdext host, converter, shader translator -> Aver.Fabric.Native
  managed/              Aver.Fabric, Aver.Fabric.Convert, Aver.Fabric.Tests
  tests/                today's tests/fabric + tests/formats.godot, fixtures, FabricTestExt
  third_party/godot-gdextension/
  docs/                 FABRIC.md, plans/, api/
  scripts/pack-avext.ps1
  .github/workflows/ci.yml   Windows, Ubuntu, macOS: build engine at the pin + extension, tests, ABI C#-vs-header check
```

History is moved, not copied:
- The Fabric paths are extracted with their history: `git filter-repo --path ...`, which is a tool install and needs a
  yes, or the built-in `git filter-branch --index-filter`, which is slower and needs nothing new.
- They are rewritten to the new layout and pushed as the first commits.
- The engine repo keeps its history and, in phase 4, removes the paths in one commit that names the new repo.

Releases are `Fabric Connector x.y.z` on GitHub with one `.avext` per platform set, and each one needs the owner's
approval. The first one needs the engine release that ships the loader (0.8.0).

## 5. Phases and gates

1. **Engine: loader and neutral hooks.** Fabric stays in-tree but is reached only through the extension contract:
   - its outputs are built into `build/bin/Extensions/FabricConnector/`;
   - the bridge, AverAssetC and the editor find it through the loader;
   - the section 2 renames are done, with Fabric updated in the same commits.

   **Gate:** a full build, and ctest with the GPU suites excluded, both green. A second build directory configured
   without Fabric must build and pass the same tests. No file under `modules/`, `Runtime/`, `sandbox/` or
   `scripting/csharp/Aver.Framework` names Godot or Fabric, apart from the read aliases.
2. **New repo.** Create `Vectoric-Core-Systems/Fabric-Connector`, public (the owner approved creating it), and move the
   code with history.

   **Gate:** its CI is green on all three platforms against the engine pin. The corpus conversions of Squash and
   platformer give the same reports as today.
3. **`.avext` and the Extensions panel.** Add `pack-avext.ps1`, install from file, and the panel.

   **Gate:** an `.avext` built by CI installs into a clean engine build, and `AverAssetC fabric` converts Squash.
   Opening the panel is a headed check, so it waits for the owner.
4. **Remove Fabric from the engine.** Delete the moved paths, point `docs/FABRIC.md` at the new repo, regenerate the
   catalogs and push.

   **Gate:** engine CI green with Fabric absent.

The conversion checks requested on 10-30 (read-back verification and the Godot `--headless` trace diff) come after
phase 2, in the new repo.

## 6. Open points

- `git filter-repo`: install it (a Python tool) or use `git filter-branch`. The default is `filter-branch`, which
  needs nothing new.
- Engine 0.8.0: the extension needs a released engine to pin against. Until then the pin is a commit hash on
  `08-gdint`/`main`, and cutting 0.8.0 needs the owner's approval.
- Godot-named general features (section 2, first list): renamed in phase 1, or left for a later cleanup. The plan
  renames everything in phase 1, because the owner's rule is no Godot-named code in core, and the read aliases keep
  old files loading.
