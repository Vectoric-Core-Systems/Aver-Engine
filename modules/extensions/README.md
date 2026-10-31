# Aver.Extensions

The extension loader. An extension is a directory, `<engine>/Extensions/<Name>/`, with an `extension.json` manifest and a native library
under `native/<platform>/`. The guide is `docs/EXTENSIONS.md`.

- `include/aver/extensions/aver_ext.h` is the native contract, plain C: `AverExtHost` (what the engine provides), `AverExtApi` (what the
  extension fills in) and the one export `aver_extension_entry`. An extension builds against this header alone.
- `include/aver/extensions/Extensions.hpp` is the loader. `Registry::scan` lists the extensions and marks each Ready or Refused with a
  reason (name differs from the directory, ABI not supported, engine outside the declared range, a listed file missing, a duplicate name).
  `Registry::run` loads the library the first time one of its commands runs.

Who reads the list: `AverAssetC` for subcommands it does not know (and for `--help`), the editor's import dialog (`sandbox/src/ExtensionImport.*`,
which reads each `ImporterInfo`: the menu text, the options and the wording come from the manifest) and, later, its Extensions panel,
`ScriptHost` for the managed assemblies, and `scripts/stage-game.ps1` (which reads the JSON itself).

`missingProjectExtensions` / `warnMissingProjectExtensions` check a project's `EXTENSION` lines against the registry and log one warning per extension that is not installed or was refused.

Tests: `tests/extensions` (`ExtensionsTest`, with a tiny extension library written in C against `aver_ext.h`; `ManagedExtensionTest`, which runs `ScriptHost` against fake extensions that carry real managed assemblies; `ExtensionImportTest`, the editor's import dialog logic against fake manifests).
