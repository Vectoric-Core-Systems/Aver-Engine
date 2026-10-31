# Aver.Extensions

The extension loader. An extension is a directory, `<engine>/Extensions/<Name>/`, with an `extension.json` manifest and a native library
under `native/<platform>/`. The guide is `docs/EXTENSIONS.md`.

- `include/aver/extensions/aver_ext.h` is the native contract, plain C: `AverExtHost` (what the engine provides: log, progress, `getService`),
  `AverExtApi` (what the extension fills in: `runCommand`, `shutdown`, `getInterface`) and the one export `aver_extension_entry`. An extension
  builds against the headers alone. `aver_ext_services.h` holds the services a host offers by name and version (`aver.asset.import-model`),
  and `aver_ext_protocol.h` the progress and result lines a command prints on stdout.
- `include/aver/extensions/Extensions.hpp` is the loader. `Registry::scan` lists the extensions and marks each Ready or Refused with a
  reason (name differs from the directory, ABI not supported, engine outside the declared range, a listed file missing, a duplicate name).
  `Registry::run` loads the library the first time one of its commands runs; `Registry::interfaceOf` asks a loaded extension for an interface.
  `ServiceRegistry` is where a host (AverAssetC, the editor, a test) adds the service tables an extension reaches through `getService`.
  `parseCommandMessage` / `formatCommandMessage` read and write the protocol lines.
- `include/aver/extensions/Avext.hpp` is installing: `.avext` packages (a zip of an extension's directory). A zip reader that refuses hostile
  archives (`ZipReader`; raw deflate is `aver::inflateRaw` over the engine's stb_image), SHA-256 and CRC-32, `inspectPackage` (the manifest
  and digest of a package before anything is copied), `installPackage` (Replace through a trash callback, unpack into `.staging`, the loader's
  own checks, move into place) and `uninstallExtension`. No UI and no third-party code.

Who reads the list: `AverAssetC` for subcommands it does not know (and for `--help`), the editor's import dialog (`sandbox/src/ExtensionImport.*`,
which reads each `ImporterInfo`: the menu text, the input, the options, and renders what the command prints through the protocol), the editor's
Extensions window (`sandbox/src/ExtensionsWindow.*`: the list, Install from file..., Uninstall), `ScriptHost` for the managed assemblies, and
`scripts/stage-game.ps1` (which reads the JSON itself).

`missingProjectExtensions` / `warnMissingProjectExtensions` check a project's `EXTENSION` lines against the registry and log one warning per extension that is not installed or was refused.

Tests: `tests/extensions` (`ExtensionsTest`, with two tiny extension libraries written in C against the headers: services, interfaces and the protocol;
`ManagedExtensionTest` and `ManagedExtensionEntryTest`, which run `ScriptHost` against fake extensions that carry real managed assemblies;
`ExtensionImportTest`, the editor's import dialog logic against fake manifests and a fake command; `AvextTest`, packages: digests, the zip reader
on valid and hostile archives, inspect, install, Replace and uninstall).
