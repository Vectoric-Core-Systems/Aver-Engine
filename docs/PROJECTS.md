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

## `.ocproject` manifest

Text, OC-dialect (`#` comments, `KEY value`). Minimal v1:

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
- Additional keys (dependencies, plugins/opt-modules the project needs, cook targets)
  are added as the engine grows — unknown keys are ignored (forward-compatible).

## Why this separation (anti-UE-bloat)

- The engine ships without any game's multi-GB content or project-specific code.
- Multiple projects share one engine build; upgrading the engine doesn't touch content.
- Tools (Rust cookers, C# editor) operate on a *project path*, not the engine tree.
- The `sandbox/` app stays in the engine only because it's an engine-development sample
  with no game content — the moment real content exists, it belongs in a project.
