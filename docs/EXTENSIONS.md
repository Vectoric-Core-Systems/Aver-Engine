# Extensions

An extension adds a feature to the engine without being part of it: a converter, an importer, a tool, a set of gameplay classes. The engine knows no extension by name. It finds the ones installed beside the executable, checks their manifests, and offers each a small, versioned contract: a plain-C native interface, a command protocol for tools that run its commands, and a managed entry point. The Fabric Connector ([FABRIC.md](FABRIC.md)), which converts Godot 4 projects and lives in [its own repository](https://github.com/Vectoric-Core-Systems/Fabric-Connector), is the first extension and appears below only as an example. Engine code reaches an extension through the loader (`modules/extensions`, `Aver.Extensions`) and the contract in `aver_ext.h`; it never links an extension's target.

## On disk

An installed extension is a directory beside the executable: `<exe dir>/Extensions/<Name>/`. A build tree has it under `build/bin/Extensions/<Name>/`. A typical layout:

```
Example/
  extension.json
  native/win-x64/Example.Native.dll      (linux-x64/*.so, osx-arm64/*.dylib)
  managed/Example.dll                    the managed half the host loads
  tools/...                              conversion-time files, never shipped in a game
  data/...
```

`extension.json`:

| Field | Meaning |
|---|---|
| `name` | Must equal the directory name. |
| `displayName`, `version`, `license`, `source` | Shown in the editor. |
| `abi` | The extension ABI the library was built for. This engine supports `2` (`AVER_EXT_ABI`); an extension built for `1` is refused with a reason that says to rebuild it. |
| `engine` | Engine versions it runs on: space-separated comparators (`>=0.7.0 <0.9.0`; `>=`, `>`, `<=`, `<`, `=`, or a bare version). A suffix such as `-1` is ignored. |
| `native` | The library's name without folder or suffix. It is `native/<platform>/<name>.dll` (`.so`, `.dylib`); the platform is `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `osx-x64` or `osx-arm64`. There is no `lib` prefix. |
| `managed` | `runtime`: assemblies under `managed/` a game loads. `entry`: the full name of a type implementing `Aver.Framework.Hosting.IAverExtension`. `actorBase`: shorthand for "declare every actor type that derives from this one". See "The managed half". |
| `commands` | `name`, `summary` and `usage` of each command `AverAssetC <name> ...` runs. |
| `importers` | File menu entries for the editor, see "The editor". |
| `runtimeFiles` | What a packaged game copies. `{platform}` and a `*` in the last segment are understood. |

## Installing

An extension is shipped as an **`.avext`** file: a zip of the extension's directory. In the editor, **Window > Extensions > Install from file...** picks one. Copying the directory into `<exe dir>/Extensions/` by hand (or building with the extension's `FABRIC_DEPLOY_TO`-style option, see "Building") installs it just as well; the window and the package are conveniences, not a second kind of install.

The package holds, at the top level of the zip (never inside a folder named after the extension):

```
extension.json
native/<platform>/<native>.dll|.so|.dylib    one folder per platform the package was built for
managed/*  tools/*  data/*
LICENSE  README.md
```

It carries runtime files only (no `.pdb`, `.ilk`, `.lib`, `.exp`). A package may hold several platforms' `native/` folders, and installs on any of them; one that holds none for this platform is refused before anything is copied, naming the platforms it has. Entries are stored or deflated, use `/`, and may carry Unix modes (a `tools/` file is made executable on Linux and macOS).

Installing does this, in order:

1. **Reads the package without copying it**: the central directory, then `extension.json`. The window shows the name, version, licence, source, ABI, engine range, the platforms inside, the size and the file's **SHA-256** (compare it with the release page) and asks before going on. The ABI and engine range are checked here; an incompatible package says why and cannot be installed.
2. **Refuses a name that is installed** (compared without regard to case, and whether or not that folder loads) unless the person chooses **Replace**. Replace moves the installed folder to the Recycle Bin (Windows) or the trash (macOS and Linux); it is never deleted. If the trash refuses (a file in it is in use, or a Linux machine has no trash), nothing is changed.
3. **Unpacks into `<exe dir>/Extensions/.staging/<Name>/`** (the loader skips folders that start with a dot). Every entry is checked first when the package is opened and again as it is written: an absolute path, a drive, a `..` or empty segment, a backslash, a colon, a character Windows cannot name, a Windows device name (`CON`, `NUL`, `COM1`...), a link (a Unix symlink, a Windows reparse point), an encrypted entry, a method but stored and deflate, Zip64, a name given twice (case aside), an entry below a file, more than 4096 entries, one entry over 512 MiB, 2 GiB in all, a compression ratio no deflate stream reaches, a local header that disagrees with the central directory, and a size or CRC-32 that does not match all refuse the package. Nothing outside the staging folder is written.
4. **Runs the loader's own checks on the unpacked folder** (`Registry::scan` on the staging root): every file the manifest lists is there, this platform's library included. A package that fails leaves nothing behind.
5. **Moves the folder into place** (`<exe dir>/Extensions/<Name>/`), then the window rescans and the File menu's import entries are re-read. An extension's managed half (`managed/`) is loaded when the editor starts, so a changed one needs a restart.

**Uninstall** moves `<exe dir>/Extensions/<Name>/` to the Recycle Bin or trash. On Windows a loaded library or assembly is held open and the move fails with a message and no change; close the editor and uninstall by hand.

An extension is code that runs with the person's permissions: install packages from sources you trust.

The logic (zip reading, the digests, `inspectPackage`, `installPackage`, `uninstallExtension`) is `modules/extensions/include/aver/extensions/Avext.hpp`. Raw deflate comes from the `stb_image` the engine already carries (`aver::inflateRaw`, `Aver.Platform`); there is no zip or zlib library in the tree. An extension's repository builds its package with its own script (the Fabric Connector's is `scripts/pack-avext.ps1`).

## What the loader checks

`Registry::scan(root)` reads every `<root>/<Name>/extension.json`. An entry that fails stays in the list, marked Refused with the reason, and is never loaded:

- the manifest is not valid JSON, or the directory has none;
- `name` differs from the directory;
- `abi` is not the supported one (an older one is told to rebuild);
- `engine` is missing, does not parse, or excludes this engine;
- `native` names a path rather than a library, or a `managed.runtime` or `runtimeFiles` entry could leave the extension's directory (absolute, a drive, a backslash, a `..` segment);
- a file the manifest lists is missing (the native library for this platform, each `managed.runtime` assembly, each `runtimeFiles` entry);
- it lists commands but no native library;
- `managed.entry` names a type but `managed.runtime` lists no assembly;
- an importer lacks its `menu` or `command`, or its `input.kind` is neither `folder` nor `file`;
- an earlier scan already listed that name.

The library loads lazily, through `aver::loadLibrary`, the first time `Registry::run` is asked for one of its commands. If its entry returns non-zero (it does not support the host's ABI), the entry becomes Refused as well.

## The native contract

`modules/extensions/include/aver/extensions/aver_ext.h` is plain C. The extension exports one function:

```c
int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out);   /* 0 ok, 1 unsupported ABI */
```

- `AverExtHost` (the engine provides it): `abi`, `engineVersion`, `executableDir`, `extensionDir`, and three functions: `log(level, msg)`, `progress(fraction, text)` and `getService(name, version)`.
- `AverExtApi` (the extension fills it in): `runCommand(name, argc, argv)`, which returns the exit code (`argv[0]` is the command's name), `shutdown()` (may be null) and `getInterface(name, version)` (may be null).
- Every struct starts with its size, and everything that crosses is a struct, a UTF-8 `const char*` or a function pointer. No C++ type, allocation or ownership crosses the boundary: the side that allocates a result gives it back to the same side to free.

An extension links its own copy of the engine's static libraries. At entry it can point its copy of Core's log at the host: `aver::setLogSink` forwards each line to `host->log`, and `aver::setLogConsole(false)` (`aver/core/LogConsole.hpp`) stops it writing the line a second time itself.

### Services and interfaces

What the host offers beyond logging is a **service**: a table of functions the extension asks for with `host->getService(name, version)`. What an extension offers beyond its commands is an **interface**: a table the host (or another extension, through the host) asks for with `api->getInterface(name, version)`. Both work the same way:

- A table is a struct that starts with its own `uint32_t size`. The owner keeps it alive until the extension's `shutdown` returns. The asker reads only the fields inside `size`, so a field appended to a table later is invisible to code built before it.
- The answer is null when the owner does not offer that name at that version. An extension that needs a service says so (a command's result message) instead of failing obscurely; one that can do without it carries on.
- **Names** are reverse-DNS-style: two or more segments joined by `.`, each segment lower-case letters, digits and `-` (not first), at most 128 characters in all: `aver.asset.import-model`, `com.example.report.writer`. The engine's own names start with `aver.`; an extension prefixes its own with a domain it controls. `isValidInterfaceName` checks the rule, and a host cannot add a service whose name breaks it.
- **Versions** are positive integers. The version changes only when a table's existing fields change meaning or go away; adding a field at the end keeps it. An owner that must break a table offers the new version beside the old one for as long as it can, so older extensions keep working. A different version is a different table: asking for version 2 never returns version 1.

A host fills the services it offers into `ext::ServiceRegistry` (`Extensions.hpp`) before an extension loads; `Registry::interfaceOf(extension, name, version)` is how a host asks an extension for an interface.

The services the engine's tools offer today (`aver_ext_services.h`):

| Name | Version | Offered by | What it is |
|---|---|---|---|
| `aver.asset.import-model` | 1 | `AverAssetC` | The engine's own model import. `importModel(args, result)` writes one model file (glTF, GLB, FBX, OBJ) as meshes, materials, textures, skeletons and clips under `<contentDir>/<outSubdir>`, and reports each mesh's content path, its source node's name, its position in the model (engine centimetres) and `sourceIndex` (the source file's node index, -1 when the format has none), plus anything it dropped. `freeResult(result)` releases what `importModel` allocated. |

## The command protocol

A command runs under a tool: `AverAssetC <command> ...` from a shell, or the same as the editor's child process. The tool should not have to know the command to show it, so a command reports in a standard form on **stdout** (`aver_ext_protocol.h`). One JSON object per line, UTF-8, ending in a newline, with nothing else on the line. Any other line the command prints is for people (the tool shows it in a log). A line or key this version does not know is ignored.

```
{"aver":"progress","fraction":0.4,"text":"scenes"}
{"aver":"result","ok":true,"open":"C:/out/Game/Game.ocproject","report":"C:/out/Report.md","warnings":3,"errors":0,"message":"71 converted"}
```

| Line | Keys |
|---|---|
| `progress` | `fraction`: 0 to 1, or -1 (or absent) when unknown; `text`: what is being done, optional. Any number of them. |
| `result` | Printed once, last. `ok`: the command did what was asked. `open`: a file the tool may offer to open (`""`: none). `report`: a file with the details (`""`: none). `warnings`, `errors`: counts the tool shows. `message`: one line for a person, and the reason when `ok` is false. |

A command has succeeded only if it exits 0, prints a `result` line and that line's `ok` is true. A command that checks its input itself (a project from an unsupported version of the other tool, say) prints `ok: false` with the reason in `message`; the tool shows the message. The protocol is version `AVER_EXT_PROTOCOL` (1); it grows by new keys and new line kinds, which older readers ignore. `ext::parseCommandMessage` and `ext::formatCommandMessage` read and write the lines for C++ code; a C or managed extension just prints them.

## The managed half

An extension with `managed.runtime` assemblies brings C# into the game. The engine reaches it through the manifest only; the scripting bridge has no reference to any extension.

- **Loading.** `ScriptHost::init` asks the loader for the Ready extensions (`HostDesc::extensionsDir`, default `<exe dir>/Extensions`), writes them into one list and hands it to the bridge's `LoadExtensions` after the bridge boots and before any project script loads. The list is versioned: a first line `#aver-extensions 2`, then a line per extension with a managed half (tab-separated: name, version, folder, `managed.entry`, `managed.actorBase`, then each assembly's full path); the bridge refuses a version it does not know with one log line. The bridge loads each extension's assemblies into the **host** load context (the script context resolves host-first, so a project's scripts see the extension's types with one identity), runs the entry, and declares the actor types that were asked for. A Refused extension logs its reason once and is skipped; so does one whose assembly will not load, whose entry cannot start or whose `actorBase` is in none of its assemblies. Nothing else is affected.
- **The entry.** `managed.entry` names a type implementing `Aver.Framework.Hosting.IAverExtension` (public, with a public parameterless constructor). The bridge creates one instance and calls `Load(ExtensionContext)` once; the engine keeps no reference to it. The `ExtensionContext` gives the extension's `Name`, `Version`, `Directory` and loaded `Assemblies`, a `Log(message)` that writes to the engine log with the extension's name, and the declarations: `DeclareActors(Type baseType)`, `DeclareActors(Func<Type, bool> predicate)` and `DeclareAllActors()`. Each declares the non-abstract actor types in the extension's assemblies that match; a type is declared once however many requests match it. Throwing from `Load` skips the extension with one log line.
- **`actorBase`** is the shorthand for the common case: it is `DeclareActors(baseType)` for that type, applied after the entry. With neither an entry nor an `actorBase`, every actor type in the assemblies is declared.
- **What the extension may use.** `Aver.Framework.Hosting.ExtensionHost` (`scripting/csharp/Aver.Framework/Hosting.cs`, `[EditorBrowsable(Never)]`, listed in [api/Aver.Framework.md](api/Aver.Framework.md)) is the hosting API, in five groups: spawning (an actor around an instance the extension made, a spawn batch), entity data (an entity's level-authored properties), view and input (the view entity, the registered input action names, marking an input event handled), binding (binding an actor to an existing entity, marking `[Editable]` values applied) and frame driving (a tick pass, the deferred queue, without the engine). `Aver.Framework` grants `InternalsVisibleTo` to the engine's own assemblies and test projects only.
- **Native libraries.** An extension assembly's P/Invokes resolve through `NativeResolver` (`scripting/csharp/Shared`): `<assembly dir>/../native/<platform>` first, then the executable folder, then each `Extensions/*/native/<platform>`.
- **Scripts that use it.** A project's `Scripts.csproj` references an extension assembly by `HintPath` (relative to the Scripts folder, `Private` false) rather than as an engine project; the extension's converter or template writes it.

## Projects

`.ocproject` records the extensions a project needs: `EXTENSION <Name> [<version>]` (see [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md)). A tool that generates a project writes its own line (`EXTENSION FabricConnector 0.1.0-alpha`). When a project opens in the game or the editor, `aver::ext::warnMissingProjectExtensions` logs one warning for each named extension that is not installed, or is installed but Refused, naming it. The project still loads.

The editor's project upgrade (`inspectProject` / `applyProjectUpgrade` in `sandbox/src/ProjectScaffold.cpp`, test `ProjectUpgradeTest`) knows every Ready extension's `managed.runtime` assemblies and fixes script references to them:
- A dead `ProjectReference` to `<Assembly>.csproj` becomes a `HintPath` reference (`Private` false) to the installed `<Assembly>.dll`. This covers projects made before the extension existed, whose scripts referenced the assembly's C# project inside the engine tree.
- A `HintPath` to one of those assemblies that no longer resolves, because the engine or its `Extensions` folder moved, is repointed at the installed copy.
- A project whose scripts use an installed extension but whose manifest has no `EXTENSION` line for it gets one, with the installed version. Its other lines are kept.

A reference that no installed extension explains is reported as before ("cannot resolve; edit by hand"). `ProjectUpgradeTest --project <manifest>` runs the upgrade on one real project against the extensions installed beside it and prints each fix.

## Command line

`AverAssetC <cmd> ...` for a command it does not know asks the loader. If an installed, Ready extension lists `cmd`, AverAssetC offers it its services, loads it and exits with the code `runCommand` returns. `AverAssetC --help` ends with each extension command, its usage and its summary. With nothing installed an unknown command is a usage error (exit 2).

## The editor

Each `importers` entry of a Ready extension adds a **File** menu item and a dialog (`sandbox/src/ExtensionImport.*`). No extension code runs in the editor's process: the dialog runs `AverAssetC <command> <input> <output> [flags]` as a child process on a worker thread and renders the command protocol, so it knows no command.

```json
"importers": [ {
  "id": "convert-thing",
  "menu": "Import Thing...",
  "tooltip": "Hover text of the menu item.",
  "description": "The paragraph at the top of the dialog.",
  "command": "thing",
  "input": { "kind": "folder", "patterns": [], "detect": "thing.project" },
  "outputDefault": "{input}_out",
  "options": [ { "flag": "--overwrite", "label": "Replace a result already there", "default": false } ]
} ]
```

| Field | Meaning |
|---|---|
| `id` | A stable name for the entry (the default is `command`); the editor's usage tracking is `file.import.<id>`. |
| `menu`, `tooltip`, `description` | The menu text (its trailing dots dropped is the dialog's title), the hover text (default: the description) and the paragraph at the top of the dialog. |
| `command` | The extension command to run; list it in `commands`, which is how `AverAssetC` finds the extension. |
| `input` | `kind`: `folder` (the default) or `file`. `patterns`: file name patterns (`*.gltf`) the Browse button offers, and for a `file` input the names that are accepted. `detect`: for a `folder` input, a file whose presence marks a valid folder (Browse picks that file and the dialog uses its folder); empty for any folder. |
| `outputDefault` | The default output, beside the input: `{input}` is the input's folder name, or a file's name without its extension (default `{input}_out`). |
| `options` | `[{"flag", "label", "default"}]`: a checkbox for each; the ticked flags follow the paths on the command line. |

The dialog shows the input and output fields with Browse buttons, the options, and Import. While the command runs it shows a progress bar (moved by `progress` lines; indeterminate until the first) and the command's other lines as a log. When the `result` line arrives it shows the `message` (green, or the failure in red), the warning and error counts, **Open result** when `open` names a project file (`.ocproject`; the editor opens it, after a prompt to save if there are unsaved edits) and **Copy report path** when `report` is set. The finished import also ends the editor notification it raised. Checks that belong to the input, such as refusing a project from an unsupported version of another tool, belong in the command: it prints `ok: false` and the message, and the dialog shows it. The item is greyed out when `AverAssetC` is not beside the editor. The dialog's logic is covered headless.

**Window > Extensions** (`sandbox/src/ExtensionsWindow.*`) lists every extension the loader found under `<exe dir>/Extensions`, Ready or Refused: name, version, status (a Refused entry shows its reason), the source as a link (an `http://` or `https://` address only), licence and folder. **Install from file...** picks an `.avext`, shows its manifest and SHA-256 and asks before copying; **Replace** appears when the name is installed. **Uninstall** and a replaced folder go to the Recycle Bin or trash. What it does is "Installing" above; the window only lists, asks and shows. The window itself has not been run (engine-run ban); the logic behind it is covered headless by `AvextTest`.

## Packaging

`stage-game.ps1` reads the project's `EXTENSION` lines and copies each named extension's `extension.json` and `runtimeFiles` (never `tools/`) to `<game>/Extensions/<Name>/`; the allowlists name no extension. See [PACKAGING.md](PACKAGING.md), "Extensions".

## Building

No extension is in this tree: the engine configures and builds without one (and a command an extension would add is then an unknown command). An extension in its own repository is built into the engine's build with `-DAVER_EXTERNAL_EXTENSIONS=<its source directory>` (a `;`-separated list; empty by default): the root CMake adds each after every module, binary directory `extensions/<directory name>`, and the extension's `CMakeLists.txt` sees the engine's `CMAKE_SOURCE_DIR` and `CMAKE_BINARY_DIR`. `AverAssetC`, which every extension's commands run through, is built whether or not `AVER_BUILD_TESTS` is on (`tools/CMakeLists.txt`).

The Fabric Connector's repository configures the engine as a project of its own, with the editor, the runtime host and the engine's tests off, and leaves the extension in the `FabricConnector` folder under `Extensions/` in its own engine build. To use it with your own engine build, give its configure `-DFABRIC_DEPLOY_TO=<engine build>/bin`: after each build it copies the extension folder into `<engine build>/bin/Extensions/`, beside the editor and `AverAssetC` ([FABRIC.md](FABRIC.md), "Build and install").

## What is stable

Stable (changing it raises `AVER_EXT_ABI`, the protocol version or a service version, and the older value keeps working or is refused with a reason): the layout and field order of the structs in `aver_ext.h`, `aver_ext_services.h` and the meaning of their fields (tables grow at the end, readers go by `size`); the names and versions of the services in the table above; the manifest fields above; the protocol lines and keys; the `IAverExtension` / `ExtensionContext` surface and the public members of `ExtensionHost`, which follow the rules of the public C# API (`docs/api/Aver.Framework.md` is checked against the source).

Not part of the contract: the C++ API of `Aver.Extensions` (`Registry`, `ServiceRegistry`), which is for the engine's own hosts; the list `ScriptHost` hands the bridge (it is versioned, but ships with the bridge); the editor dialog's layout and wording.

## Tests

`ExtensionsTest` (`tests/extensions`) runs the loader against fake extension directories and two tiny extension libraries written in C against the headers alone. The first covers a valid extension, each refusal with its reason (an older and a newer ABI included), lazy loading, command dispatch and exit codes, log forwarding, the host's directories and progress, services by name and version (the model-import round trip, with its allocation freed by the host; a host that offers none), an extension whose entry refuses the host, and shutdown. The second uses a host service, offers an interface and reports through the protocol: its stdout is captured and parsed. The service naming rule and the protocol lines are checked directly.

`AvextTest` covers `.avext` packages headless: SHA-256 and CRC-32 against known answers; the zip reader on valid archives (stored, fixed- and dynamic-Huffman deflate, an empty entry, a directory entry, an end comment) and on hostile ones (`../` and nested `..`, absolute, drive, stream, backslash and device names, control characters, a Unix symlink, a device file, a Windows reparse point, encryption, an unknown method, Zip64, a stored entry with two sizes, a size over the cap, a ratio no deflate stream reaches, a directory with data, names that differ only in case, an entry below a file, an offset or count that lies, a truncated or garbage file, the entry, size and name caps, and on read a wrong CRC-32, a deflate stream shorter or longer than its size, a junk stream, a local header naming another file, and data running past the archive); the manifest read from a package before anything is copied (a wrong or old ABI, an engine out of range, no build for this platform, a name that is a path, no manifest); install into a scratch `Extensions` folder (Replace, a trash that fails, no trash, a folder that differs only in case, a file in the way, a stale staging folder, a package missing a listed file, a zip-slip, a bad checksum and a symlink each leaving nothing behind) and uninstall (hostile names, no trash, a trash that fails). `AvextTest --avext <file> <exe dir>` installs a real package and prints what the loader then says; the Fabric Connector's repository uses it on the package `scripts/pack-avext.ps1` makes.

`ExtensionImportTest` covers the editor dialog's logic against fake manifests and a fake command (the test executable run as `fakecmd`): the importers read through the loader, the input checks, the command line, the progress and result lines (success, `ok: false`, a missing result, a command that refuses its input), and the importer on its worker thread reporting through the notification queue. `ManagedExtensionTest` runs `ScriptHost::init` with the real CLR against fake extensions that carry sample assemblies: the actor types an entry and an `actorBase` ask for are declared and the others are not, the entry's context names the extension, and a refused extension and an `actorBase` that no assembly holds each produce one log line. `ManagedExtensionEntryTest` does the same for an entry that throws and one that does not exist. `AbiEnumTest` pins the extension log levels and `AVER_EXT_ABI` against the header and [ABI.md](ABI.md). An extension's own repository tests its manifest through the loader and the real `AverAssetC`.
