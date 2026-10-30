# Fabric Connector as an engine extension

Fabric moves out of the engine repo into its own public repo, `Vectoric-Core-Systems/Fabric-Connector` (LGPL-2.1),
and ships as one `.avext` file. The engine gains an extension loader and neutral hook points; nothing in engine core
names Godot or Fabric when the move is done. This is the design. **Phase 1 is done** (section 5 has its gate results):
the neutral hooks, the general-feature renames, the loader (3.1 to 3.4), the editor's import dialog (3.5, the dialog
only), the managed half (3.6), the packaging that reads `EXTENSION` lines (3.7) and the in-tree relocation (section 4)
are built. Still to do: the Extensions panel and `.avext` install (phase 3), and the new repo (phase 2). The
engine-facing description of what exists is [../../EXTENSIONS.md](../../EXTENSIONS.md).

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

Phase 1 (hooks) did these renames. The table does not list the names that go with them, and it does not say how old files load:
- **HLSL library and contexts.** `gd_*`/`gdr_*` became `emb_*`/`embr_*`, `Gd*Sampler*` became `Emb*Sampler*`, `gdSlot*`/`gdrSlot*`
  became `embSlot*`/`embrSlot*`, `gGd`/`GdCtx` became `gEmb`/`EmbCtx`, `GDN`/`GD_NS_CUR` became `EMBN`/`EMB_NS_CUR`, `VSMainGd`/`AVER_MATX_GDVS`
  became `VSMainEmb`/`AVER_MATX_EMBVS`, `fxkey::kGdVertex` became `kEmbVertex`, `averRtGd*` became `averRtEmb*`. The private copy the
  sky, fog and particles programs use is renamed `cenv_*`/`CEnv*`/`cenvSlot*` (`customEnvRename`). The engine and the extension share
  these names through `material_embedded_lib.hlsl`; the extension's own test compares it with the library its translator generates.
- **Containers.** `DOMAIN godotenv` became `DOMAIN customenv`. The `.ocmat` FX keys `gdvs`/`gddiscard` became `embvs`/`embdiscard`, and
  `GDREGS` became `EMBREGS`. The `.ocparticle` record `GDSHADER` became `CUSTOMSHADER`, and `ParticleEffect::godotShader` became
  `customShader`. `aver::fmt::gdfx` became `hlslfx`, `MaterialGraphEntry::gd`/`isGodot` became `emb`/`isEmbedded`, and
  `MaterialGraphRegistry::hasGodotShaders`/`hasGodotVaryings` became `hasEmbeddedShaders`/`hasEmbeddedVaryings`.
- **Old files.** The readers keep the old node types, domain, FX keys, `GDREGS` and `GDSHADER` as read aliases. An old `GodotShader` or
  `GodotEnvShader` node also carries HLSL that uses the old identifiers, so the reader maps `AVER_GD_*`, `GDN`, `gd_*` and `Gd[IU]Sampler*`
  in those sources to the new ones as it unpacks them (`OcGraphHlsl.cpp`, `OcGraphHlslTest`). Files are always written with the new names.
- **Left as they were.** The translator's own emitted names (`gdsm_`, `gdt_`, `gdf_`, `gdv_`, `gdc_`, `gdctor_`, `GdS_`) and its C++ `Gd*`
  types are Fabric's and move with it. The general-feature renames (physics, tonemap, light model, audio, UI, fog volumes) are the
  "general" package.
- **VoxiSpirvCompileTest** no longer includes `tests/formats.godot`: `tests/render.pbr/src/EmbeddedHlslBridge.hpp` registers two embedded
  shaders written by hand in the shape the translator emits, so the engine's tests need no translator.

Phase 1 (general) did the Godot-named general features. Behaviour is unchanged; the new names are:
- **Physics.** `PhysicsGodotFilter.*` is `PhysicsLayerFilter.*` (`GodotFilterState` is `LayerFilterState`, `godotLayerBitsOf` is `layerBitsOfBody`, `godotJointExclusion`
  is `layerJointExclusion`, `World::godot` is `World::layerFilter`); `PhysicsBodyGodot.*` is `PhysicsRigidExtras.*` (`BodyGodotState` is `RigidExtrasState`);
  `AVER_PHYS_COMBINE_GODOT` is `AVER_PHYS_COMBINE_MATERIAL` (C# `CombineMode.Godot` is `CombineMode.Material`); `BodyGodot.cs` is `RigidExtras.cs`;
  `KinematicCharacterOptions.Godot` is `KinematicCharacterOptions.Default`. `docs/physics/GODOT_{FILTERING,QUERIES,KINEMATIC,AREAS,VEHICLE}.md` stay in the engine
  (they document engine ABIs) as `LAYER_FILTERING`, `SPACE_QUERIES`, `KINEMATIC`, `AREAS` and `RAYCAST_VEHICLE`. Tests: `GodotFilterTest` is `LayerFilterTest`,
  `BodyGodotTest` is `RigidExtrasTest`.
- **Tonemap.** `kTonemapGodot*` is `kTonemapExt*`, `tonemapIsGodot` is `tonemapIsExt`, `tonemapModeFromGodot` is `tonemapModeFromExtIndex`, `tmap::godot`/`acesGodot` are
  `tmap::ext`/`acesExt`; in HLSL `averTonemap*Godot` is `averTonemap*Ext`. Project keys are numbers, so no alias is needed; the editor labels read "Extended ...".
- **Light model.** The Godot light parameter block is the "parametric" model: `kLightGodot` is `kLightParametric`, `WorldLight::godot`/`SceneLight::godot` are `parametric`,
  `kFlagGodot` is `kFlagParametric`, `windowGodot` is `windowLinear`, `AVER_RL_F_GODOT` is `AVER_RL_F_PARAMETRIC`, the shadow bias constants lose "Godot". The flag bit
  values are unchanged, so saved levels load as before.
- **Audio.** `godotFilterCoeffs`/`godotFilterMagnitude` are `filterCoeffs`/`filterMagnitude`, `Effect::godotClassName` is `Effect::className`, `PlayDesc::godotSpatial` is
  `spatialModel`, `godotPanGains` is `spatialPanGains`, `Voice::godot3d` is `spatial3d`, and the ABI parameter `godotClass` is `effectClass`. The effect class strings
  ("AudioEffectReverb" ...) are the `.ocbus` format's own effect names and stay: a bus file stores them and the engine reads them, so the lookup table stays in the engine.
- **UI.** `uiGodotDefaultTheme[Text]` is `uiBuiltinDefaultTheme[Text]`; the built-in theme's name string is `builtin_default` (was `godot_default`).
- **File keywords.** `PHYSICS.COMBINE godot` is read as `material`; files are written with `material`. The converter's report line says `PHYSICS.COMBINE material`.
- **Comments** that said "Godot parity" are neutral. `docs/plans/fabric/G*.md` still describe the mapping; they move with Fabric.

Fabric code that sits in core but has no other user: it moves with Fabric. Done in the general phase:
- `GodotEnvironment.hpp` and `GodotLight.hpp` are in `modules/formats.godot/include/aver/formats/godot/`. `RasterLightsTest` carries its own reference copy of the two
  falloff formulas and the energy constants; `TonemapTest` and `PanoramaSkyTest` lost their checks of the Godot mapping, which now run in `FabricEnvTest`;
- `Aver.Physics/GodotJoints.cs` is `Aver.Fabric/Nodes/GodotJoints.cs` (namespace `Aver.Fabric`), and the `RaycastWheelSettings.*FromGodot` helpers are
  `Aver.Fabric.RaycastWheelGodot` (`Nodes/RaycastWheelGodot.cs`);
- the `Aver.Fabric` line in `scripts/csharp-api-index.py` stays until Fabric's API page moves out (phase 2).

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
  "engine": ">=0.7.0 <0.9.0",
  "license": "LGPL-2.1",
  "source": "https://github.com/Vectoric-Core-Systems/Fabric-Connector",
  "native": "Aver.Fabric.Native",
  "managed": { "runtime": ["Aver.Fabric.dll"], "actorBase": "Aver.Fabric.Node" },
  "commands": [ { "name": "fabric", "summary": "convert a Godot 4 project", "usage": "<godot-project-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]" } ],
  "importers": [ { "menu": "Import Godot Project...", "command": "fabric", "input": "folder",
                   "detect": "project.godot", "opensResult": true } ],
  "runtimeFiles": ["native/{platform}/Aver.Fabric.Native.*", "managed/Aver.Fabric.dll", "data/extension_api.json"]
}
```

What the built loader does differently from the sketch above:
- The engine range reads `>=0.7.0 <0.9.0` while the engine is 0.7.0-1 (a `-1` suffix is ignored by the comparison); it becomes
  `>=0.8.0 <0.9.0` with the first engine release that ships the loader.
- A command has an optional `usage` (the arguments `AverAssetC --help` prints after the name).
- The library has no `lib` prefix on any platform (`Aver.Fabric.Native.so`, `.dylib`), so the loader builds the name from `native`.
- `runtimeFiles` are checked like the other listed files: a `{platform}` is replaced by this platform's name and a `*` in the last
  segment matches any file there. An extension that lists a file it does not have is Refused.

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
  - `executableDir` and `extensionDir`, as strings (the same host struct serves one extension, so they are fields, not functions);
  - `importModel(const AverExtModelArgs*, AverExtModelResult*)`, today's `fabric::Host::importModel` (parts and
    notes are returned as arrays the host allocates and frees with `freeModelResult`);
  - `progress(fraction, text)`.
- `AverExtApi`, filled in by the extension:
  - `runCommand(name, argc, argv)`, which returns an exit code (`argv[0]` is the command's name, so the arguments are
    `argv[1..]`);
  - `shutdown()`.
- Every struct starts with its own size, so a field appended later is known by the side that has it.

Runtime P/Invokes, such as today's `aver_gdext_*`, stay ordinary exports of the same DLL. The engine never calls them.

The extension links its own copy of the engine's static libraries (Core, Formats, ...). Two rules follow from that:
- Only plain data crosses the boundary.
- At entry the extension points its copy of the Core log sink at `host->log`, so messages still reach the Output Log.
  Core gained `setLogConsole(bool)` (`aver/core/LogConsole.hpp`) for this: the extension turns its own stdout/stderr
  line off, because the host writes the forwarded line again, and a line must appear once.

### 3.3 Loader (`modules/extensions`, new, Core-only)

The loader works in stages:
- At start it scans `<exeDir>/Extensions/*/extension.json` and validates each entry:
  - the name matches the directory;
  - the `abi` is one this engine supports;
  - the engine version is in the declared range;
  - the files the manifest lists are present;
  - the name is not one an earlier scan already listed.
- An entry that fails stays in the list, marked with the reason, and is never loaded. So does one whose library loads
  but whose entry refuses the host's ABI.
- The loader's dependency is Core, Platform and Aver.Formats, for its JSON reader. Nothing about rendering or scenes.
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

**Status: the import dialog is built; the Extensions panel is phase 3.** What was built:
- `sandbox/src/FabricImport.*` is gone. `sandbox/src/ExtensionImport.*` reads the `importers` of every Ready extension through the
  loader (`ExtensionImporter::scan`) and draws one File menu item and one dialog per entry. The editor's own file mentions no
  extension. `sandbox/src/ProcessStream.*` is the child-process runner (argv, no shell, a callback per output line); the editor no
  longer compiles any Fabric source.
- Everything that was Fabric's in the old dialog is data in `extension.json`, with a default for each when the manifest is silent.
  It reads word for word as before: the menu text and title "Import Godot Project", the "Godot project" label, the `_Aver` output
  suffix, the "Replace a project already converted there" and "Report only" checkboxes (flags `--overwrite` and `--dry-run`, in that
  order on the command line), the Godot 3 refusal (`detectVersion`: `config_version=` of at least 5), the "Open converted project"
  button, the notification titles and the summary line. Fields: see [../../EXTENSIONS.md](../../EXTENSIONS.md), "The editor".
- The summary the dialog reads is the command's closing `{"schemaVersion":1,"summary":true,"kind":"<command>",...}` line, parsed with
  the engine's JSON reader instead of the old string search; `output`, `project`, `report` and `dryRun` are the keys it uses by name,
  the rest feed the manifest's wording (`{converted}` ...).
- Not word for word: the UI registry id of the menu item is `file.import.fabric` (was `file.importGodotProject`; nothing reads it),
  and the log prefix and the open-failure line are the manifest's `logTag` (`Fabric`) and `[Editor]`.
- Tests: `ExtensionImportTest` (engine, 43 checks: fake manifests through the loader, the input checks, the command line, the
  summary, the importer on its worker thread against a fake tool) and `FabricImportTest` (the extension's, 36 checks: its manifest
  through the loader, then the real `AverAssetC` converting `ConvertGame`). The dialog (the ImGui half) was never run (engine-run ban).

Extensions panel (Window > Extensions), phase 3, not built:
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

What is built (the managed package):
- **`Aver.Framework.Hosting.ExtensionHost`** (`scripting/csharp/Aver.Framework/Hosting.cs`) is the public surface. Its members do
  exactly what the framework's own code did at the same point:
  - `SpawnActor` (offer the pending instance, `aver_fw_spawn`, clear the offer), `BeginSpawnBatch` / `EndSpawnBatch`;
  - `ReadProperties` (the `aver_fw_props_*` calls, as a list), `SetViewEntity`, `RegisteredActionNames`, `EntityFromHandle`;
  - `SetSelf` (the `Actor.Self` setter), `MarkEditablesApplied`, `MarkHandled` (the `InputEvent.Handled` setter);
  - `BeginPass`, `EndFrame`, `FlushDeferred`, `ResetDeferred` (the game thread).

  `InternalsVisibleTo` for `Aver.Fabric` and `Aver.Fabric.Tests` is gone; `Aver.Fabric` and its tests compile against the public
  surface only (found by removing the grants and building: those were all the compile errors).
- **The bridge** has no reference to Fabric. It exports `LoadExtensions(spec)`, bound optionally like the other late exports. `ScriptHost::init`
  scans the extensions (`HostDesc::extensionsDir`, default `<exe dir>/Extensions`), writes one line per Ready extension that has a
  managed half (TAB-separated: name, actor base, each assembly's full path), calls it after `Bootstrap` and before `LoadScripts`, and logs one
  warning for each Refused extension. The bridge loads each assembly into the host context, resolves `actorBase` in them and calls
  `DeclareActors`. A missing assembly, a load failure or an `actorBase` found in none of them logs one line and skips that extension.
  `Aver.Scripting.Host` therefore depends on `Aver.Extensions`.
- **Build.** `extensions/FabricConnector/managed/CMakeLists.txt` builds `Aver.Fabric.dll` into `bin/Extensions/FabricConnector/managed/`
  (through a private output folder, so the engine's assemblies are not copied beside it) and `Aver.Fabric.Convert` into `.../tools/`. It
  runs after the engine's own `dotnet build` commands (which share the project folders' `obj/`). `modules/scripting` no longer builds or
  lists either.
- **Converter.** `FabricScripts.cpp` finds the tool in `<extension>/tools`, passes `--refs <extension>/managed;<exe dir>/Scripting` (a
  list of folders, first wins) and the extension folder comes from `AverExtHost::extensionDir`. `Aver.Fabric.Convert` writes the
  `Scripts.csproj` reference as `<Reference Include="Aver.Fabric"><HintPath>` to the installed assembly, relative to the Scripts folder like
  the engine references, with `Private` false: the host loads that one assembly, and a copy in `Binaries/Scripts` would be a second `Aver.Fabric`.
  The engine's other four references stay `ProjectReference`s. `ProjectScaffold` no longer writes or repoints an `Aver.Fabric` reference.
- **`.ocproject`.** `EXTENSION <Name> [<version>]` is parsed and written (`ProjectDesc::extensions`; old files without it load as before). The
  converter writes `EXTENSION FabricConnector <version>` from `extension.json`. When a project opens (`GameApp`, `ProjectBrowser::open`),
  `aver::ext::warnMissingProjectExtensions` logs one warning per named extension that is not installed or was refused.
- **`NativeResolver`** already probes `<assembly dir>/../native/<platform>` first (loader package); it was checked from a bare process that
  loaded `Aver.Fabric.dll` from the extension folder, where only that probe can succeed.

### 3.7 Packaging

`stage-game.ps1` reads the project's `EXTENSION` lines and copies each extension's `runtimeFiles`, never `tools/`, into
`<game>/Extensions/<Name>/`. The Fabric entries in `game.allowlist` and `payload.allowlist` are removed.

**Status: built.** `stage-game.ps1` does this right after it copies the engine files:
- for each distinct `EXTENSION <Name>` line it takes `<build>/bin/Extensions/<Name>/extension.json` (a name the build does not have is a
  `Fail`), and copies `extension.json` plus the `runtimeFiles` to `<out>/Extensions/<Name>/` at the same relative paths (`{platform}` is
  `win-x64`, or `win-arm64` on an ARM host; a `*` in the last segment matches any file there, minus the linker's `.pdb`, `.ilk`,
  `.exp` and `.lib`);
- a `runtimeFiles` entry that leaves the folder, or points into `tools/`, is a `Fail`. The manifest itself is always copied, since the
  loader needs it;
- the copied native library goes through the script's existing import-closure check and VC++ runtime staging.
Neither allowlist names an extension any more (their comments describe the anim/nav/net seams without naming one). `stage-payload.ps1`
carries no extension by default; `-WithExtension <Name>` stages a whole extension folder, tools included, into `<out>/bin/Extensions/`.
`docs/PACKAGING.md` has an "Extensions" subsection. No packaged game was built or run for this (the repo has no staging test, and the
script refuses a Debug tree without `-AllowDebugCrt`): see the gate in section 5 for the dry run that was done.

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

In the engine repo during phase 1 the same layout is `extensions/FabricConnector/`, so the later move is a copy of one
directory:

| Today | In the tree |
|---|---|
| `modules/formats.godot`, `modules/fabric.gdext` | `extensions/FabricConnector/native/formats.godot`, `native/gdext` |
| `tools/fabric` | `extensions/FabricConnector/native/tools/fabric` |
| `scripting/csharp/Aver.Fabric`, `.Convert`, `.Tests` | `extensions/FabricConnector/managed/...` |
| `tests/fabric`, `tests/formats.godot` | `extensions/FabricConnector/tests/...` |
| `third_party/godot-gdextension` | `extensions/FabricConnector/third_party/godot-gdextension` |

- The root CMake has one option, `AVER_WITH_FABRIC` (default ON), that adds `extensions/FabricConnector`; nothing else in the
  engine build names its targets. The engine also configures and builds with it OFF.
- `Aver.Fabric.Native` is one SHARED library: `native/formats.godot` and `native/gdext` (the `aver_gdext_*` exports under the
  names the C# runtime already P/Invokes; the old `Aver.Fabric.GDExt.Abi` target is gone), the converter, and
  `aver_extension_entry` (`native/src/FabricExtension.cpp`), whose `runCommand "fabric"` runs `fabric::run` with a `Host` built from
  `AverExtHost`. It builds into `build/bin/Extensions/FabricConnector/native/<platform>/` with `extension.json` and
  `data/extension_api.json` beside it. The GDExtension host finds the API dump through the library's own location
  (`<library dir>/../../data`), so a game that carries the extension folder needs nothing beside its executable.
- `AverAssetC` has no Fabric code. The model importer a converter needs is `AverExtHost::importModel`, which `AverAssetC`
  implements with plain structs; every other subcommand it does not know goes to the loader.
- The `managed/Aver.Fabric.dll` the manifest lists is built by `extensions/FabricConnector/managed` (section 3.6). `Aver.Fabric` resolves its native library through `NativeResolver`
  (`scripting/csharp/Shared`), which now probes `<assembly dir>/../native/<platform>` first, then the executable folder, then
  each `Extensions/*/native/<platform>`.
- `tests/formats` keeps its own copy of the one fixture it read from Fabric's data (`data/quad.gltf`, used by `MeshExtraTest`).
- The packaging allowlists name no extension. For a short while (the relocation package) they listed the extension's runtime files
  (`Extensions/FabricConnector/...`, conditional on `AVER_WITH_FABRIC`); `stage-game.ps1` now reads the project's `EXTENSION` lines
  instead (3.7).

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

   **Status: met (10-30), headless.** Results:
   - `build/` (Debug, `AVER_WITH_FABRIC` ON): full build clean; ctest with the GPU suites excluded 523/523; `Aver.Framework.Tests` and
     `Aver.Graph.Tests` green, `Aver.Fabric.Tests` green through `FabricManagedTests`; the physics checkpoints are the ones recorded
     (100 7396bb86c731dc22 ... 600 6a21dda931eca85a); the module-catalog, node-catalog and C# API index checks are ok.
   - `C:/Users/User/AverBuilds/nofabric` (`AVER_WITH_FABRIC` OFF): full build clean; ctest 493/493, with no Fabric test registered
     (`FabricManagedTests` and the 28 other Fabric rows are absent). `AbiEnumTest` there runs the C side of the anim, nav, net, UI-style
     and audio rows and logs one `skipped: no Fabric` line for their C# mirrors.
   - Naming grep over `modules/`, `Runtime/`, `sandbox/`, `tools/`, `cmake/`, `scripts/`, `scripting/csharp/` and the root
     `CMakeLists.txt` (case-insensitive `godot`/`fabric`): what is left is (1) the read aliases (`NODE gd GodotShader`, `DOMAIN godotenv`,
     `PHYSICS.COMBINE godot` in the readers and their comments), (2) the `AVER_WITH_FABRIC` option and its comments, (3) pointers to the
     extension's documents and files (`docs/plans/fabric/G*.md`, `docs/FABRIC.md`, `extensions/FabricConnector/managed/...` as the C#
     mirror of a C seam, the "Mirror Aver.Fabric.UiSbKind" lines of `ui_style_abi.h`), (4) the `Aver.Fabric` API page entry of
     `scripts/csharp-api-index.py`, which goes with phase 2, and (5) text that is not the name: "fabric" the cloth, "fabricate", and a
     URL in vendored Jolt. The pointers in (3) dangle once those documents move out; phase 2 repoints them.
   - `AverAssetC fabric` over all 24 fixture projects against the pre-phase baseline: exit 0 everywhere, the same summary counts, the
     same set of 317 files, and differences only in `.ocproject` (the `EXTENSION FabricConnector 0.1.0` line; `PHYSICS.COMBINE material`
     for `godot`), `Scripts.csproj` (the `HintPath` reference instead of the project reference), `FabricReport.jsonl` (the same
     `PHYSICS.COMBINE` text), and, in the shader fixtures, the renamed keywords and identifiers of the hooks phase (`.ocgraph` node
     type and domain, `AVER_GD_*` to `AVER_CUSTOM_*`, `Gd*Sampler*`/`GDN`/`gd_*` to `Emb*`/`EMBN`/`emb_*`, `.ocmat` `GDREGS`/`gdvs`/
     `gddiscard` to `EMBREGS`/`embvs`/`embdiscard`, `.ocparticle` `GDSHADER` to `CUSTOMSHADER`).
   - Not run: the editor, the import dialog, a packaged game, anything on a GPU. `stage-game.ps1` was run on a converted project from
     the Debug tree (`-AllowDebugCrt`): it copied the four extension files (`extension.json`, `data/extension_api.json`,
     `managed/Aver.Fabric.dll`, `native/win-x64/Aver.Fabric.Native.dll`) and no `tools/`; it still fails on the Debug CRT, as it must
     for a Debug tree.
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
