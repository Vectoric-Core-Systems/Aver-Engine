# Engine ⟂ Projects

Aver Engine is a **standalone, reusable engine** — it never contains game content. A
**game is a separate project** that lives in its own folder and *references* the engine.
This is the deliberate opposite of bundling a game's content inside the engine tree.

```
C:\Users\User\Documents\
├── Aver Engine\                 ← THE ENGINE (this repo). No game content lives here.
│   ├── modules\  abi\  tools\  editor\  shaders\
│   ├── sandbox\                 ← an engine SAMPLE, not a game project
│   └── tests\                   ← format-conformance vectors only
│
└── Aver Projects\               ← projects root (sibling; never inside the engine)
    └── OpenConstructor\         ← a GAME PROJECT
        ├── OpenConstructor.ocproject   ← manifest (references the engine)
        └── Content\
            ├── Maps\            *.ocmap
            ├── Vehicles\        *.ocbeam / *.ocaero
            └── Meshes\          *.ocmesh / *.octex / *.ocmat
```

The engine binary/tools are pointed at a project folder at runtime; they load `.oc*`
assets from the project's `Content/`. Nothing about a project is compiled into the
engine, and the engine has zero knowledge of any specific game.

## Opening a project — implemented

A project is **additive, never required**: the editor runs exactly as well with none loaded.

- **Start screen.** Launching the editor interactively with no project shows an in-window
  project browser (recent projects, New Project…, Open Project…). It is the same window and
  the same ImGui host as the editor — not a launcher process.
- **Command line.** `Sandbox.exe <path>.ocproject` loads it directly and skips the browser.
  A positional argument is treated as a project when it ends in `.ocproject`, and as an
  `.ocbeam` otherwise.
- **Never in automation.** The browser is suppressed when `--frames` is present, when a project
  was named on the command line, or when headless. The verification harness drives the editor
  with `--frames` and reads one probe pixel out of the viewport; a full-screen chooser in front
  of it would take out all 13 oracle gates at once.
- **Recent projects** persist in `%LOCALAPPDATA%\AverEngine\recent.txt` — one path per line,
  most-recent first, capped at 10, and entries whose file has gone are dropped on read.
- **New Project** scaffolds `<location>/<Name>/` with the manifest and the `Content/{Maps,
  Meshes,Scripts}` tree below, defaulting the location to `Documents\Aver Projects` so projects
  land as siblings of the engine. It refuses to write into an existing folder.

Once loaded, the project's name appears in the window title and the status bar, the Content
Browser reports where the mount points, and Edit ▸ Project Settings ▸ Description shows the
manifest. The editor **reads** `.ocproject`; it does not write one back yet.

## `.ocproject` manifest — implemented

Text, OC-dialect (`#` comments, `KEY value`), read by `Aver.Formats` (`loadOcproject`) with the
same `detail/TextScan.hpp` scanner as `.ocbeam`/`.ocmap`. Minimal v1:

```
OCPROJECT 1
NAME OpenConstructor
ENGINE Aver 0.1.0            # engine name + minimum version this project needs
CONTENT Content             # content root, relative to this file
STARTMAP Maps/demoworld.ocmap
AUTHOR OpenConstructor Team
```

- `CONTENT` is the single mount root; asset references in `.ocmap`/`.ocprefab` resolve
  relative to it (by name or path), and every asset also has a stable Object ID.
- `ENGINE <name> <minVersion>` lets the engine refuse a project that needs a newer build.
  **Enforced**: a different engine name, or a minimum version above this build's, fails the load
  with a message naming both versions rather than loading something the engine cannot honour.
  The build's version comes from `project(... VERSION ...)` via `aver/core/Version.hpp`, so
  there is no second copy of the number to drift.
- Additional keys (dependencies, plugins/opt-modules the project needs, cook targets)
  are added as the engine grows — unknown keys are ignored (forward-compatible). This is a
  guarantee the loader keeps, not an aspiration: an unrecognised key is skipped silently.
- A leading UTF-8 BOM is stripped. Manifests get hand-edited, and Notepad writes one.

## Why this separation (anti-UE-bloat)

- The engine ships without any game's multi-GB content or project-specific code.
- Multiple projects share one engine build; upgrading the engine doesn't touch content.
- Tools (Rust cookers, C# editor) operate on a *project path*, not the engine tree.
- The `sandbox/` app stays in the engine only because it's an engine-development sample
  with no game content — the moment real content exists, it belongs in a project.
