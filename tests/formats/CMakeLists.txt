add_executable(FormatTest src/FormatTest.cpp)
target_link_libraries(FormatTest PRIVATE Aver.Formats Aver.Assets Aver.Core Aver.Platform)
set_target_properties(FormatTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# MeshTest — the AVR1 container and .ocmesh. A THIRD executable rather than more cases in FormatTest
# because FormatTest takes file paths on the command line and reports usage with none, so a binary
# format that must be exercised from memory would never run in a plain `MeshTest.exe` invocation.
# JsonTest — the reader the glTF importer stands on. Its own executable because everything
# downstream trusts it silently: an importer does not verify that a number was understood, so a
# parser bug surfaces as wrong vertices several modules away, and a failure should name this layer.
add_executable(JsonTest src/JsonTest.cpp)
target_link_libraries(JsonTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(JsonTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GltfTest — the importer. Builds its glTF documents by hand rather than shipping a binary fixture,
# so what each case exercises is legible and a failure names the field that broke.
add_executable(GltfTest src/GltfTest.cpp)
target_link_libraries(GltfTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(GltfTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(MeshTest src/MeshTest.cpp)
target_link_libraries(MeshTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(MeshTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# MaterialTest — the .ocmat reader/writer and the mip filter. A SECOND executable rather than more
# cases in FormatTest, because it is the only one that needs the material DLL: a machine where
# Aver.Render.PBR.dll fails to load would otherwise report the .ocbeam parser as broken.
if(TARGET Aver.Formats.Material)
  add_executable(MaterialTest src/MaterialTest.cpp)
  # Aver.Render.PBR.Materials is here for packMaterial alone. It carries Aver.RHI as a link
  # dependency, but nothing in this test creates a device: the GPU block is pure CPU-side packing,
  # and it is precisely the kind of hand-maintained cross-module layout that needs a test.
  target_link_libraries(MaterialTest PRIVATE
    Aver.Formats.Material Aver.Formats Aver.Render.PBR.Materials Aver.Core Aver.Platform)
  set_target_properties(MaterialTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ConvertTool -- drives the importer over a real file and writes a real .ocmesh. Not a test: a small
# harness so the end-to-end path can be exercised without the editor's UI.
add_executable(ConvertTool src/ConvertTool.cpp)
target_link_libraries(ConvertTool PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(ConvertTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ActorScriptTest — reading and rewriting the actor designer's generated region.
#
# A THIRD executable rather than more assertions in MaterialTest: it exercises a different format
# with a different grammar, and the point of splitting is that a failure names which one broke.
if(TARGET Aver.Formats.Material)
  add_executable(ActorScriptTest src/ActorScriptTest.cpp)
  target_link_libraries(ActorScriptTest PRIVATE Aver.Formats.Material Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(ActorScriptTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ActorSweep — not a test. A TOOL that reports what the actor editor would make of every .cs under a
# directory, run against a REAL project rather than a fixture.
#
# It exists because a fixture is written by the same person who wrote the parser, so it agrees with
# that parser by construction. Pointed at SkyForge, this found two bugs on its first run that no
# fixture had: attribute kinds were searched in turn rather than in file order, so a file naming an
# [AverGameMode] above an [AverClass] came back named after the wrong one; and a file was assumed to
# declare ONE actor, where real ones declare several.
#
#     ActorSweep.exe "<project>\Content\Scripts"
if(TARGET Aver.Formats.Material)
  add_executable(ActorSweep ${CMAKE_SOURCE_DIR}/tools/ActorSweep.cpp)
  target_link_libraries(ActorSweep PRIVATE Aver.Formats.Material Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(ActorSweep PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# MakeSamples -- not a test either. A GENERATOR that synthesises a few short .wav files, so a
# template project has sounds to play and the editor's audio import has real input to decode.
#
# SYNTHESISED rather than sourced: a downloaded sample carries somebody's licence into whatever
# repository it lands in, and this tree already decided against that -- OcAudioTest ships no media
# and reports a SKIP rather than a pass, on exactly that reasoning. Generated audio has no licence,
# is byte-identical everywhere, and is retuned by editing a number.
#
# It emits .wav, NOT .ocaudio, on purpose: the point is to exercise the import path end to end, and a
# generator that wrote the engine's own container would be testing only itself.
#
#     MakeSamples.exe "<project>\Content\Audio\Source"
add_executable(MakeSamples ${CMAKE_SOURCE_DIR}/tools/MakeSamples.cpp)
target_link_libraries(MakeSamples PRIVATE Aver.Core)
set_target_properties(MakeSamples PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
