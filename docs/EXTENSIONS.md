# Extensions

An extension adds a feature to the engine without being part of it. The first is the Fabric Connector (`extensions/FabricConnector/`, [FABRIC.md](FABRIC.md)), which converts a Godot 4 project. Engine code reaches an extension only through the loader (`modules/extensions`, `Aver.Extensions`) and the plain-C contract in `aver_ext.h`; it never links an extension's target. The design, the phases and what is still to come are in [plans/fabric/EXTENSION.md](plans/fabric/EXTENSION.md).

## On disk

An installed extension is a directory beside the executable: `<exe dir>/Extensions/<Name>/`. A build tree has it under `build/bin/Extensions/<Name>/`.

```
FabricConnector/
  extension.json
  native/win-x64/Aver.Fabric.Native.dll      (linux-x64/*.so, osx-arm64/*.dylib)
  managed/Aver.Fabric.dll                     the runtime the host loads; converted scripts compile against it
  tools/Aver.Fabric.Convert.dll (+ Roslyn)    conversion time only, never shipped in a game
  data/extension_api.json
```

`extension.json`:

| Field | Meaning |
|---|---|
| `name` | Must equal the directory name. |
| `displayName`, `version`, `license`, `source` | Shown in the editor's Extensions panel. |
| `abi` | The extension ABI the library was built for. This engine supports `1` (`AVER_EXT_ABI`). |
| `engine` | Engine versions it runs on: space-separated comparators (`>=0.7.0 <0.9.0`; `>=`, `>`, `<=`, `<`, `=`, or a bare version). A suffix such as `-1` is ignored. |
| `native` | The library's name without folder or suffix. It is `native/<platform>/<name>.dll` (`.so`, `.dylib`); the platform is `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `osx-x64` or `osx-arm64`. There is no `lib` prefix. |
| `managed` | `runtime`: assemblies under `managed/` a game loads; `actorBase`: the base class of the actor types they declare. |
| `commands` | `name`, `summary` and `usage` of each command `AverAssetC <name> ...` runs. |
| `importers` | File menu entries for the editor: `menu`, `command`, `input` (`folder` or `file`), `detect` (a file that marks a valid input) and `opensResult`, plus the optional wording and options listed under "The editor". |
| `runtimeFiles` | What a packaged game copies. `{platform}` and a `*` in the last segment are understood. |

## What the loader checks

`Registry::scan(root)` reads every `<root>/<Name>/extension.json`. An entry that fails stays in the list, marked Refused with the reason, and is never loaded:

- the manifest is not valid JSON, or the directory has none;
- `name` differs from the directory;
- `abi` is not supported;
- `engine` is missing, does not parse, or excludes this engine;
- a file the manifest lists is missing (the native library for this platform, each `managed.runtime` assembly, each `runtimeFiles` entry);
- it lists commands but no native library;
- an earlier scan already listed that name.

The library loads lazily, through `aver::loadLibrary`, the first time `Registry::run` is asked for one of its commands. If its entry returns non-zero (it does not support the host's ABI), the entry becomes Refused as well.

## The native ABI

`modules/extensions/include/aver/extensions/aver_ext.h` is plain C. The extension exports one function:

```c
int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out);   /* 0 ok, 1 unsupported ABI */
```

- `AverExtHost` (the engine provides it): `abi`, `engineVersion`, `executableDir`, `extensionDir`, and four functions: `log(level, msg)`, `importModel(args, result)` with `freeModelResult(result)`, and `progress(fraction, text)`. The model importer is the engine's own glTF/FBX/OBJ import; `AverAssetC` implements it.
- `AverExtApi` (the extension fills it in): `runCommand(name, argc, argv)`, which returns the exit code (`argv[0]` is the command's name), and `shutdown()`.
- Every struct starts with its size, and everything that crosses is a struct, a UTF-8 `const char*` or a function pointer. No C++ type, allocation or ownership crosses the boundary: the host allocates what it returns and takes it back through `freeModelResult`.

An extension links its own copy of the engine's static libraries. At entry it points its copy of Core's log at the host: `aver::setLogSink` forwards each line to `host->log`, and `aver::setLogConsole(false)` (`aver/core/LogConsole.hpp`) stops it writing the line a second time itself. The Fabric Connector's entry is `extensions/FabricConnector/native/src/FabricExtension.cpp`.

## The managed half

An extension with `managed.runtime` assemblies brings C# into the game: `Aver.Fabric.dll` declares the actor classes a converted scene places. The engine reaches it through the manifest only; the scripting bridge has no reference to any extension.

- **Loading.** `ScriptHost::init` asks the loader for the Ready extensions (`HostDesc::extensionsDir`, default `<exe dir>/Extensions`), writes the name, `managed.actorBase` and the full path of each `managed.runtime` assembly into one string, and hands it to the bridge's `LoadExtensions` after the bridge boots and before any project script loads. The bridge loads each assembly into the **host** load context (the script context resolves host-first, so a project's scripts see the extension's types with one identity), finds `actorBase` in them, and declares every non-abstract actor type that derives from it. An empty `actorBase` declares all actor types. A Refused extension logs its reason once and is skipped; so does one whose assembly will not load or whose `actorBase` is in none of its assemblies. Nothing else is affected.
- **What the extension may use.** `Aver.Framework.Hosting.ExtensionHost` (`scripting/csharp/Aver.Framework/Hosting.cs`, `[EditorBrowsable(Never)]`, listed in [api/Aver.Framework.md](api/Aver.Framework.md)): spawning an actor around an instance the extension made, a spawn batch, an entity's level-authored properties, the view entity, the registered input action names, binding an actor to an existing entity, marking `[Editable]` values applied or an input event handled, and driving a tick pass or the deferred queue without the engine. `Aver.Framework` grants `InternalsVisibleTo` to the engine's own assemblies and test projects only.
- **Native libraries.** An extension assembly's P/Invokes (`Aver.Fabric.Native`) resolve through `NativeResolver` (`scripting/csharp/Shared`): `<assembly dir>/../native/<platform>` first, then the executable folder, then each `Extensions/*/native/<platform>`.
- **Scripts that use it.** A converted project's `Scripts.csproj` references `Aver.Fabric.dll` by `HintPath` (relative to the Scripts folder, `Private` false) rather than as an engine project; the converter writes it.

## Projects

`.ocproject` records the extensions a project needs: `EXTENSION <Name> [<version>]` (see [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md)). A converter writes its own line (`EXTENSION FabricConnector 0.1.0`). When a project opens in the game or the editor, `aver::ext::warnMissingProjectExtensions` logs one warning for each named extension that is not installed, or is installed but Refused, naming it. The project still loads.

The editor's project upgrade (`inspectProject` / `applyProjectUpgrade` in `sandbox/src/ProjectScaffold.cpp`, test `ProjectUpgradeTest`) knows every Ready extension's `managed.runtime` assemblies and fixes script references to them:
- A dead `ProjectReference` to `<Assembly>.csproj` becomes a `HintPath` reference (`Private` false) to the installed `<Assembly>.dll`. This covers projects converted before the extension existed, whose scripts referenced the assembly's C# project inside the engine tree.
- A `HintPath` to one of those assemblies that no longer resolves, because the engine or its `Extensions` folder moved, is repointed at the installed copy.
- A project whose scripts use an installed extension but whose manifest has no `EXTENSION` line for it gets one, with the installed version. Its other lines are kept.

A reference that no installed extension explains is reported as before ("cannot resolve; edit by hand"). `ProjectUpgradeTest --project <manifest>` runs the upgrade on one real project against the extensions installed beside it and prints each fix.

## Command line

`AverAssetC <cmd> ...` for a command it does not know asks the loader. If an installed, Ready extension lists `cmd`, AverAssetC loads it and exits with the code `runCommand` returns. `AverAssetC --help` ends with each extension command, its usage and its summary. With nothing installed an unknown command is a usage error (exit 2).

## The editor

Each `importers` entry of a Ready extension adds a **File** menu item (`menu`) and a dialog (`sandbox/src/ExtensionImport.*`). The dialog asks for the input, warns when the `detect` file is missing (or, with `detectVersion`, too old), shows the output folder (`<input folder><outputSuffix>` beside it by default), offers the `options`, and runs `AverAssetC <command> <in> <out> [flags]` as a child process on a worker thread. It shows the lines the command prints, ends with the summary, and, when `opensResult` is set, offers to open the converted project. No extension code runs in the editor's process. The item is greyed out when `AverAssetC` is not beside the editor. There is no Extensions panel yet (install, uninstall, status): that is phase 3 of the plan.

The command's last line is `{"schemaVersion":1,"summary":true,"kind":"<command>", ...}`. The dialog reads `output` (the result's folder), `project` (its name; the file to open is `<output>/<project>.ocproject`), `report` and `dryRun`, and keeps every other key for the wording. Everything below is optional in the manifest (the default is in brackets) and sits in the importer entry:

| Field | Meaning |
|---|---|
| `detectVersion` | `{"key", "min", "message"}`: the detect file must hold `key` followed by a number of at least `min`, else the dialog refuses the folder with `message` (`{value}` is the number found). |
| `title` | The dialog's title (`menu` without its trailing dots). |
| `tooltip`, `description` | The menu item's hover text, and the paragraph at the top of the dialog (the tooltip defaults to the description). |
| `inputLabel`, `outputLabel` | What the input is called ("Input") and the output folder's caption ("Output folder"). |
| `outputSuffix` | The default output folder is the input folder's name plus this ("_Imported"). |
| `options` | `[{"flag", "label", "default"}]`: a checkbox for each; the ticked flags follow the paths on the command line. |
| `openLabel`, `busyText`, `logTag` | The open button ("Open result"), the text while it runs ("Working..."), and the prefix of the lines it logs (the extension's display name). |
| `notify` | `{"running", "done", "checked", "failed"}`: the notification's titles (`checked` is for a `dryRun` result). |
| `summary` | `text`: the line under the log, `brief`: the notification's body, `warnIfNonZero`: a summary key whose non-zero value makes the notification a warning, `notes`: `[{"text", "when", "unless"}]`, extra lines that show when the key `when` is true or non-zero and `unless` is not. `{key}` in any text is that key's value from the summary line. |

`ExtensionImportTest` (`tests/extensions`) covers this against fake manifests and a fake tool (the test executable run as `fakecmd`); `FabricImportTest` covers the Fabric Connector's manifest through the loader and the real `AverAssetC`. The dialog itself has not been run (engine-run ban).

## Packaging

`stage-game.ps1` reads the project's `EXTENSION` lines and copies each named extension's `extension.json` and `runtimeFiles` (never `tools/`) to `<game>/Extensions/<Name>/`; the allowlists name no extension. See [PACKAGING.md](PACKAGING.md), "Extensions".

## Building

The root CMake option `AVER_WITH_FABRIC` (default ON) adds `extensions/FabricConnector`. Its library is built to `build/bin/Extensions/FabricConnector/native/<platform>/`, with `extension.json` and `data/extension_api.json` beside it, and its managed half to `managed/Aver.Fabric.dll` and `tools/` (`extensions/FabricConnector/managed`; needs the .NET SDK, without which the manifest lists a file that is not there and the loader refuses the extension). With the option OFF the engine configures and builds without the extension (and `AverAssetC fabric` is then an unknown command). Nothing outside `extensions/FabricConnector` references its targets.

## Tests

`ExtensionImportTest` and `FabricImportTest` are described under "The editor". `AbiEnumTest` pins the C side of the anim, nav, net, UI-style and audio seams; the check that their C# mirrors in the extension's managed half agree runs only with `AVER_WITH_FABRIC` on (otherwise it logs one `skipped: no Fabric` line).

`ManagedExtensionTest` (`tests/extensions`) runs `ScriptHost::init` with the real CLR against three fake extensions that carry sample assemblies: the actor types under `actorBase` are declared and the others are not, and a refused extension and an `actorBase` that no assembly holds each produce one log line. `ExtensionsTest` (`tests/extensions`) runs the loader against fake extension directories and a tiny extension library written in C against `aver_ext.h` alone: a valid extension, each refusal with its reason, lazy loading, command dispatch and exit codes, log forwarding, the host's directories and progress, the model-import round trip, an extension whose entry refuses the host, and shutdown.
