# Aver.Formats.Godot -- the reader for Godot 4's text formats (Fabric Connector, docs/plans/fabric/G3_FORMATS.md).
#
# Core and Platform only: it parses text into plain structs and resolves res:// and uid:// against a
# project folder. It knows nothing of scenes, meshes or materials, so the converter (and any test)
# can link it without an engine behind it. Always built, like Aver.Formats.
aver_add_module(Aver.Formats.Godot
  SOURCES
    src/GodotDiagnostics.cpp
    src/GodotVariant.cpp
    src/GodotSections.cpp
    src/GodotScene.cpp
    src/GodotProject.cpp
    src/GodotProjectFs.cpp
  DEPS
    Aver.Core
    Aver.Platform
)
