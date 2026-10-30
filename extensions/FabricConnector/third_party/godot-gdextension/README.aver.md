# Godot GDExtension interface -- vendored for Aver

**Upstream:** https://github.com/godotengine/godot-cpp (the `gdextension/` folder, which mirrors Godot's own
`core/extension/gdextension_interface.h` and the API dump Godot writes with `--dump-extension-api`).
**Version:** tag `godot-4.5-stable`, commit `e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77`. The 4.5 interface is a
superset of 4.1's (functions are looked up by name), and the API dump keeps `hash_compatibility` for older
method hashes, so extensions built for Godot 4.1 to 4.5 can be hosted.
**Licence:** MIT, (c) 2017-present Godot Engine contributors. `LICENSE.md` is kept beside the files.
**Approved:** by the owner for the Fabric Connector (2026-10-27).

## Why it is here

`modules/fabric.gdext` hosts GDExtension plugins of converted Godot projects: it implements the interface
functions an extension asks for and checks method hashes against the API dump. See
`docs/plans/fabric/FABRIC_CONNECTOR.md`.

## What was taken

The three files below, unmodified. Nothing else from godot-cpp (no bindings, generator or build files).

| file | sha256 |
|---|---|
| `gdextension_interface.h` | `a40ac4fca0f526910bd0e6afc6da6c169f50801c84d4e29c4ce2891cadc7b550` |
| `extension_api.json` | `23d807e3f914f7a91b152a8a7b03638d4853f8e642b79f10ce85a43b44340bfd` |
| `LICENSE.md` | `26a5b210d90760156ce886267ce9df235787dcc6cebfd7ecd95ef5b6fdcc95bf` |
