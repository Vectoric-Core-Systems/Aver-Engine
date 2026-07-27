aver_add_module(Aver.Formats
  SOURCES
    src/Avr1.cpp
    src/GltfImport.cpp
    src/Json.cpp
    src/OcAnim.cpp
    src/OcBeam.cpp
    src/OcMap.cpp
    src/OcMesh.cpp
    src/OcWorld.cpp
    src/OcProject.cpp
    src/Texture.cpp
  DEPS
    Aver.Core
    Aver.Platform
    Aver.Assets
)

# Aver.Formats.Material — the .ocmat reader/writer, deliberately a THIRD target.
#
# It is the only format that needs a type from outside Aver.Formats: it parses straight into
# pbr::MaterialDesc rather than into a duplicate struct. Aver.Render.PBR is a Core-only shared DLL
# and cannot link this, and making Aver.Formats link the PBR DLL would put a renderer-family binary
# behind every headless tool that only wanted to read a map. See OcMat.hpp.
#
# Guarded on the module switch rather than defined unconditionally: with PBR off there is no
# MaterialDesc to parse into, and a target linking a library that was never added is a configure
# error a long way from the option that caused it.
if(AVER_MODULE_PBR)
  aver_add_module(Aver.Formats.Material
    SOURCES
      src/OcMat.cpp
    DEPS
      Aver.Formats
      Aver.Render.PBR
  )
endif()

# Aver.Formats.Audio — the WAV reader, deliberately a FOURTH target, for the reason Material is a
# third: it is the only format that parses into a type from outside Aver.Formats. It fills an
# audio::SoundData directly rather than a duplicate struct that something would then have to copy
# across, and making Aver.Formats itself depend on Aver.Audio would put an audio dependency on every
# consumer of a mesh loader.
if(TARGET Aver.Audio)
  aver_add_module(Aver.Formats.Audio
    SOURCES
      src/Wav.cpp
    DEPS
      Aver.Formats
      Aver.Audio
  )
endif()
