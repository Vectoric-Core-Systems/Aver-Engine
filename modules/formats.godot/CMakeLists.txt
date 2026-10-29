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
    # ArrayMesh surfaces and GridMap cells as plain arrays (G4 R7).
    src/GodotMesh.cpp
    # The Godot shading-language front-end (G6 SH1): preprocessor, parser + type checker, HLSL emitter.
    src/GodotShaderPre.cpp
    src/GodotShaderTypes.cpp
    src/GodotShaderParse.cpp
    src/GodotShaderHlsl.cpp
    src/GodotShaderHlslLib.cpp
    # SH2: a spatial shader prepared for the engine's material path (slots, uniform table, render modes, the report).
    src/GodotShaderEngine.cpp
    # SH3: sky, fog and particles shaders prepared for the engine.
    src/GodotShaderEnv.cpp
    # SH3: VisualShader resources as shading-language text.
    src/GodotVisualShader.cpp
  DEPS
    Aver.Core
    Aver.Platform
)
