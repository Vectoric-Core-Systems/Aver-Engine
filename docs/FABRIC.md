# Fabric Connector: bring a Godot 4 project into Aver

The Fabric Connector converts a Godot 4.1+ project into a standalone Aver project, one way (Godot to Aver): scenes, assets, GDScript and GodotSharp scripts, input, translations and GDExtension plugins, with a report that says item by item what came across exactly, approximately or not at all. Converted scripts run on `Aver.Fabric`, a runtime with Godot's node API, signals and `Variant` on top of the engine. Scope is 3D and UI.

It is an [extension](EXTENSIONS.md), and it lives in its own repository, not in this one: **[Vectoric-Core-Systems/Fabric-Connector](https://github.com/Vectoric-Core-Systems/Fabric-Connector)** (LGPL-2.1). The engine knows nothing about it. It finds the extension at run time, in a `FabricConnector` folder under `Extensions/` beside its executables, through the extension loader (`modules/extensions`). Without that folder `AverAssetC fabric` is an unknown command and the editor has no Import Godot Project item.

## Build and install

The extension is built against an engine checkout (this one, or the commit the Fabric repository pins), and then has to sit next to the engine's executables:

```
git clone https://github.com/Vectoric-Core-Systems/Fabric-Connector
cd Fabric-Connector
cmake -S . -B build -G Ninja -DAVER_ENGINE_DIR=<this engine checkout> -DFABRIC_DEPLOY_TO=<this engine checkout>/build/bin
cmake --build build
```

`AVER_ENGINE_DIR` is this checkout. `FABRIC_DEPLOY_TO` is optional (empty by default): after each build it copies the extension folder (`extension.json`, `native/<platform>/`, `managed/`, `tools/`, `data/`) into `<FABRIC_DEPLOY_TO>/Extensions/`, which here is `build/bin/Extensions/`, beside `AverAssetC` and the editor. Without it the extension is left in the Fabric build tree (`build/engine/bin/Extensions/`) and you copy that folder yourself. Close the editor and `AverAssetC` before building again: on Windows they hold the extension's library open. Building the extension needs the .NET SDK. A release will be one `.avext` file per platform set.

## Convert a project

```
AverAssetC fabric <godot-project-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]
```

or **File > Import Godot Project...** in the editor, which runs the same command as a child process. The converter never changes the Godot project. It writes the Aver project, and `FabricReport.md` / `FabricReport.jsonl` next to it. Read the report first.

## Where its documents are

All in the Fabric repository:

- [docs/FABRIC.md](https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/FABRIC.md): the user guide (options, what converts, units and axes, scripts, GDExtension plugins, reading the report, known limits).
- [docs/plans](https://github.com/Vectoric-Core-Systems/Fabric-Connector/tree/main/docs/plans): the design of the extension and of the converter and runtime (EXTENSION.md, FABRIC_CONNECTOR.md, G3 to G6).
- [docs/api/Aver.Fabric.md](https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/api/Aver.Fabric.md): the runtime's API list.

In this repository: [EXTENSIONS.md](EXTENSIONS.md) is the mechanism (manifest, loader, native ABI, managed half, packaging), [plans/GODOT_PARITY.md](plans/GODOT_PARITY.md) is the engine's Godot parity plan, and the engine features the extension uses are documented with their systems: [physics/](physics/) (layer filtering, space queries, kinematic characters, areas, the raycast vehicle), [AUDIO.md](AUDIO.md), [SYNAPSE.md](SYNAPSE.md), [ASSET_IMPORT.md](ASSET_IMPORT.md) (the axis map) and [ABI.md](ABI.md) (the C seams the extension calls).
