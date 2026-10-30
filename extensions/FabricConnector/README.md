# Fabric Connector

Converts a Godot 4 project into an Aver project: scenes, assets, GDScript and GodotSharp, and compiled GDExtension plugins. User guide: `docs/FABRIC.md`. How an extension plugs into the engine: `docs/EXTENSIONS.md`. Where this directory is going (its own repository, `Vectoric-Core-Systems/Fabric-Connector`): `docs/plans/fabric/EXTENSION.md`.

```
extension.json     the manifest the engine's loader reads
CMakeLists.txt     added by the engine's root CMake when AVER_WITH_FABRIC is ON (the default)
native/            Aver.Fabric.Native, one SHARED library
  formats.godot/     the Godot text and binary readers, the shading-language front end
  gdext/             the GDExtension host (the aver_gdext_* exports the C# runtime P/Invokes)
  tools/fabric/      the converter
  src/               aver_extension_entry (the extension ABI) and the stb_image_write implementation
managed/           Aver.Fabric (the runtime the host loads and converted scripts compile against), Aver.Fabric.Convert (the script converter), Aver.Fabric.Tests;
                   CMakeLists.txt builds the first two into the extension's folder. Aver.Fabric uses only the public Aver.Framework.Hosting surface of the engine.
tests/             fabric (the converter end to end, the GDExtension host) and formats.godot, with their fixtures
third_party/       godot-gdextension: Godot's published GDExtension interface header and API dump (MIT)
```

The build puts the extension in `build/bin/Extensions/FabricConnector/`: `extension.json`, `native/<platform>/Aver.Fabric.Native.*`, `managed/Aver.Fabric.dll`, `tools/Aver.Fabric.Convert.dll` (with Roslyn; never shipped in a game) and `data/extension_api.json`. Convert a project with `AverAssetC fabric <godot-project-dir> <out-dir>`.

License: LGPL-2.1.
