# Aver.Fabric.GDExt  (`modules/fabric.gdext`)

- **Language:** C++ (a plain-C surface)
- **Depends on:** Core, Platform, Formats, Formats.Godot
- **Built:** always. `Aver.Fabric.GDExt` is the static module (the headless test links it); `Aver.Fabric.GDExt.Abi` is the SHARED library the `Aver.Fabric` C# runtime P/Invokes.

The GDExtension host of the Fabric Connector: loads a converted Godot project's native plugins (`.gdextension` + a per-platform library), serves the interface functions godot-cpp based extensions call (Variant, String, ClassDB registration, method binds with hash checks against the vendored `extension_api.json`), and keeps a minimal engine class registry (`Object`, `RefCounted`, `Resource`, `Node`, `Node3D`) behind them. The `aver_gdext_*` C ABI (`include/aver/fabric/gdext/fabric_gdext_abi.h`) lets C# load a plugin, list and instantiate its classes, call methods, get and set properties, hear signals and drive the virtuals.

What is implemented, what is not, the ABI, the wire form of a Variant and the C# side: **`docs/plans/fabric/G3_GDEXT.md`**. Test: `tests/fabric/src/GdextHostTest.cpp` over `tests/fabric/gdext_test/` (a plugin written in C against `gdextension_interface.h` only). Godot's `gdextension_interface.h` and `extension_api.json` (MIT) are vendored in `third_party/godot-gdextension/`.
