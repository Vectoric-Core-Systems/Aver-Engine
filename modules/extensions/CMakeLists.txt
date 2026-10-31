# Aver.Extensions -- the extension loader and the native extension ABI (docs/EXTENSIONS.md).
#
# Scans <exe dir>/Extensions/*/extension.json, validates each manifest and loads an extension's native library on first use through
# aver::loadLibrary. Core, Platform and the JSON reader of Aver.Formats: it knows nothing of scenes, rendering or any particular extension.
# aver_ext.h is the plain-C contract an extension builds against; an extension needs that header and nothing else from the engine.
aver_add_module(Aver.Extensions
  SOURCES
    src/Extensions.cpp
  DEPS
    Aver.Formats
    Aver.Platform
    Aver.Core
)
