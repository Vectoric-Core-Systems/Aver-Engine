# Engine ⟂ Projects

Aver Engine is a **standalone, reusable engine** — it never contains game content. A
**game is a separate project** that lives in its own folder and *references* the engine.
This is the deliberate opposite of bundling a game's content inside the engine tree.

```
C:\Users\User\Documents\
├── Aver Engine\                 ← THE ENGINE (this repo). No game content lives here.
│   ├── modules\  abi\  tools\   ← editor\ and shaders\ existed here once; both were deleted
│   │                              (the editor is sandbox\ -> Sandbox.exe; HLSL lives in
│   │                              modules/*/shaders/ and sandbox/shaders/ files instead)
│   ├── sandbox\                 ← an engine SAMPLE, not a game project — and also THE EDITOR
│   ├── templates\               ← starter templates the engine COPIES into a new project
│   ├── test-content\            ← fixtures the suites open; miniature projects on purpose
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
  of it would take out all 20 oracle gates at once (`scripts/gates.ps1`'s `$Gates` array; 13 when
  this line was first written, since grown).
- **Older-series projects need `--open-legacy` in automation**, and the bullet above is exactly
  why this is easy to miss: the BROWSER is suppressed, but the *upgrade prompt* is a different
  modal and is not. A project whose `CREATEDWITH` predates the current series is offered an
  upgrade before it opens — deliberately, since choosing a project by argv rather than by mouse
  is no reason to migrate it silently — and with a frame limit set there is nobody to answer.
  The project then never opens and the run renders an **empty editor**, reporting timings, probe
  pixels and screenshots of nothing. Those numbers look completely ordinary; the only tell is
  `over 0 entities` in a scene-walk line. A frame-limited run in this state now logs an error
  saying so, and `--open-legacy` is the fix: it opens the project **as-is, upgrading nothing**,
  so it is safe to point at content that must be measured exactly as it exists on disk.
- **Recent projects** persist in `%LOCALAPPDATA%\AverEngine\recent.txt` — one path per line,
  most-recent first, capped at 10, and entries whose file has gone are dropped on read.
- **New Project** scaffolds `<location>/<Name>/` with the manifest and the `Content/{Maps,
  Meshes,Scripts}` tree below, defaulting the location to `Documents\Aver Projects` so projects
  land as siblings of the engine. It refuses to write into an existing folder.

Once loaded, the project's name appears in the window title and the status bar, the Content
Browser reports where the mount points, and Edit ▸ Project Settings ▸ Description shows the
manifest. The editor reads `.ocproject` and, since Project Settings became editable, writes one back:
Name, Author and Start map are fields, and Save rewrites the manifest through
`fmt::writeOcproject`. Engine and Content stay read-only, with the reason on screen — changing
`CONTENT` moves where every asset resolves and nothing re-mounts.

## Where code goes — C# is project-side, C++ is engine-side

This is the one place Aver deliberately differs from Unreal, and the editor's **Tools** menu is
built around making it visible rather than something a user infers from a missing file.

| | Lives in | Authored by | Engine rebuild |
|---|---|---|---|
| **C# scripts and classes** | `<project>/Content/Scripts/` | Tools ▸ New C# Script / New C# Class | no |
| **C++ modules and classes** | the ENGINE's `modules/` | Tools ▸ New C++ Module / New C++ Class | **yes** |

`.ocproject` declares content, not a build: there is no compiler invocation, no source list and no
target in the manifest, so a project has nowhere to put C++. Until that changes, new C++ authors
the engine — the Tools items are labelled `(engine)` and their modals say it outright.

C# is the reverse: `Content/Scripts/` is plain content, `Scripts.csproj` is generated beside it,
and **Tools ▸ Compile Scripts** runs `dotnet build` on it without the engine being touched.
Compiled scripts now *run*: `spawnActor` dispatches `OnBeginPlay`
(`modules/framework/src/FrameworkAbi.cpp:535`) and the Play lifecycle spawns
GameInstance/GameMode/Controller/Pawn. One deliberate exception — the Actor Editor's **Live**
preview spawns the class, runs `BuildModels` and destroys it the same frame, so no `OnBeginPlay`
and no ticking there; its tooltip says so.

Neither CMakeLists is edited automatically. Wiring a module into the top-level build is a
deliberate act — every skeleton under `modules/` is deliberately unwired — and `aver_add_module`
takes an explicit `SOURCES` list rather than globbing, so a new `.cpp` needs a human to add the
line. Both modals show exactly which line, in which file.

## `.ocproject` manifest — implemented

Text, OC-dialect (`#` comments, `KEY value`), read by `Aver.Formats` (`loadOcproject`) with the
same `detail/TextScan.hpp` scanner as `.ocbeam`/`.ocmap`. Minimal v1:

```
OCPROJECT 1
NAME OpenConstructor
ENGINE Aver 0.1.0            # engine name + minimum version this project needs
CONTENT Content             # content root, relative to this file
STARTMAP Maps/Default.ocmap
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

## How the separation is kept — `SeparationTest`

Everything above is a claim about the tree, and a claim about a tree drifts. `tests/repo/`
builds **`SeparationTest`**, which runs with the ordinary suite sweep and decides it by
reading the source rather than trusting it:

- **No game asset outside a sanctioned root.** Any `.ocmap`/`.ocmesh`/`.ocmat`/`.ocgraph`/…
  must be under `templates/`, `test-content/`, `content/` or the managed graph fixtures.
- **No `.ocproject` outside `test-content/`.** A manifest in the tree is the strongest form
  of the same defect: it puts a whole *project* in the engine.
- **Only `sandbox/src/ProjectBrowser.cpp` may know where projects live.** It is the screen
  that asks the user to pick one, so it carries `documentsDir() + "\\Aver Projects"` and the
  example path. No other engine source may bake in an absolute path or name that folder.

Comments are lexed away first, on purpose. Around ninety comments in the tree name a
project — "measured on ElectricDreams at 2750×1639" — and that is where a *measurement*
came from, which is worth keeping. Only string literals are searched, because a literal is
something the engine can act on.

All three rules were confirmed by planting a violation and watching each one fail, and its
own vacuity guards fail if `ProjectBrowser.cpp` ever stops carrying the convention — so the
checks cannot go green by having nothing left to look at.
