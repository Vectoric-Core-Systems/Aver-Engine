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

# ImportTest — the OBJ and USDA importers. Alongside GltfTest rather than inside it because the
# three formats share nothing but the destination struct: a glTF failure and an OBJ failure have no
# common cause, and one binary that reported both would make the shared basis-change code look like
# the suspect every time either moved.
add_executable(ImportTest src/ImportTest.cpp)
target_link_libraries(ImportTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(ImportTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(MeshTest src/MeshTest.cpp)
target_link_libraries(MeshTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(MeshTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LandscapeTest — `.ocland`, the landscape heightfield. Its own executable for MeshTest's reason: the
# fixtures are synthesised in memory, so it has nothing to say when handed a file path.
#
# It links nothing but Aver.Formats, and that is the point of landscape stage 1 rather than an
# accident of it: the whole data model is checkable with no device, no .NET and no GPU, so the terrain
# arithmetic is settled before anything tries to draw it.
add_executable(LandscapeTest src/LandscapeTest.cpp)
target_link_libraries(LandscapeTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(LandscapeTest PROPERTIES
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

# ConvertTool -- drives the importer over a real file and writes the whole .oc* triple. Not a test:
# a small harness so the end-to-end path can be exercised without the editor's UI.
add_executable(ConvertTool src/ConvertTool.cpp)
target_link_libraries(ConvertTool PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(ConvertTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# MakeRig -- writes content/dev/Rig.gltf, the tree's ONLY skinned asset source. Registered here for
# ConvertTool's reason rather than because it is a test: this is where Aver.Formats is already linked.
#
# It emits glTF and NOT the .oc* triple, on purpose: a generator that wrote the engine's own
# container would be testing only itself, and would become a second source of truth for the skin
# stream's interleave and for the weight quantisation. Going through the importer instead exercises
# the metre-to-centimetre scale, the determinant -1 basis change and the child-first joint reorder,
# which is what a real asset hits. It also links Aver.Anim, because the one thing a static check
# cannot tell you about a rig is whether its clip actually moves it.
add_executable(MakeRig ${CMAKE_SOURCE_DIR}/tools/MakeRig.cpp)
target_link_libraries(MakeRig PRIVATE Aver.Formats Aver.Anim Aver.Core Aver.Platform)
set_target_properties(MakeRig PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
# MakeFoliage -- a GENERATOR, like MakeRig and MakeSamples, not a test. It synthesises the tree and
# bush meshes a forest needs, because this engine has no mesh authoring and marketplace foliage
# cannot be committed here on licence grounds. It also writes each mesh as .obj, which gives
# ImportTest's format a real multi-thousand-triangle input rather than only hand-written ones.
#
#     MakeFoliage.exe "<project>\Content\Meshes"
add_executable(MakeFoliage ${CMAKE_SOURCE_DIR}/tools/MakeFoliage.cpp)
target_link_libraries(MakeFoliage PRIVATE Aver.Formats Aver.Core Aver.Platform)
# stb_image_write, for the PNG texture set. A SECOND STB_IMAGE_WRITE_IMPLEMENTATION relative to
# Sandbox is fine: they are separate executables, so there is no duplicate symbol to collide.
target_include_directories(MakeFoliage PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
set_target_properties(MakeFoliage PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(MakeSamples ${CMAKE_SOURCE_DIR}/tools/MakeSamples.cpp)
target_link_libraries(MakeSamples PRIVATE Aver.Core)
set_target_properties(MakeSamples PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
