add_executable(FormatTest src/FormatTest.cpp)
target_link_libraries(FormatTest PRIVATE Aver.Formats Aver.Assets Aver.Core Aver.Platform)
set_target_properties(FormatTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LevelInspect -- not a test. A small CLI that loads a .ocworld/.ocmap through the LIVE parser
# (parseOcworld, not FormatTest's testMap which goes through the separate legacy OcMap.cpp) and
# reports its PCGVOLUME and SCATTER records, for the same reason ActorSweep exists: a fixture is
# written by whoever wrote the parser and agrees with it by construction, but a hand-authored level
# gets no editor and no compiler, so this is how a typo in a SCATTER line gets caught before it
# silently scatters nothing.
add_executable(LevelInspect src/LevelInspect.cpp)
target_link_libraries(LevelInspect PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(LevelInspect PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OcGraphTest — the .ocgraph visual scripting format. Text parsing, round-tripping, unknown record
# preservation, and deterministic output. See modules/formats/include/aver/formats/OcGraph.hpp.
add_executable(OcGraphTest src/OcGraphTest.cpp)
target_link_libraries(OcGraphTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
# The cross-implementation fixture the C# runtime is tested against. Passed in rather than guessed,
# so the test does not have to reason about where it is being run from -- and so the ONE definition
# of that path lives here, next to the target, rather than in two languages.
target_compile_definitions(OcGraphTest PRIVATE
  AVER_OCGRAPH_FIXTURE="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/cross_impl_test.ocgraph")
set_target_properties(OcGraphTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OcInputTest -- the .ocinput input-binding-scheme format, and the OcProject INPUT.SCHEME key that
# names one (see modules/formats/include/aver/formats/OcInput.hpp for the grammar and
# modules/formats/include/aver/formats/OcProject.hpp for the new key). The OcProject assertions live
# in this same executable rather than a separate one: the point of INPUT.SCHEME is the pairing of the
# two formats, and neither Aver.Formats target dependency changes by testing them together.
add_executable(OcInputTest src/OcInputTest.cpp)
target_link_libraries(OcInputTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(OcInputTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OcParticleTest -- the .ocparticle format (particles DECIDED 3). Round-tripping, unknown-record and
# comment preservation, strict malformed-input rejection, and a real on-disk malformed file. Guarded
# on the TARGET rather than on AVER_MODULE_PARTICLES for MaterialTest's exact reason below: an
# AVER_MODULE_PARTICLES=OFF (or scene-off, which cascades to it) tree simply has no
# Aver.Formats.Particles target to link, and the guard has to ask the question CMake can actually
# answer at this point rather than re-deriving the option's cascade by hand.
if(TARGET Aver.Formats.Particles)
  add_executable(OcParticleTest src/OcParticleTest.cpp)
  target_link_libraries(OcParticleTest PRIVATE
    Aver.Formats.Particles Aver.Formats Aver.Particles Aver.Core Aver.Platform)
  set_target_properties(OcParticleTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

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

# TrifactorTest -- the cluster builder, the LOD DAG, and MLET persistence (see the file's own header
# comment for why it is a separate executable from MeshTest). `if(TARGET Aver.Trifactor)`, not
# `if(AVER_MODULE_TRIFACTOR)`, for MaterialTest's exact reason above: an unconditional Aver.Trifactor
# is a hard configure error in the AVER_MODULE_TRIFACTOR=OFF tree this repo defaults to, and that
# default has to keep configuring even though this test cannot exist in it.
if(TARGET Aver.Trifactor)
  add_executable(TrifactorTest src/TrifactorTest.cpp)
  target_link_libraries(TrifactorTest PRIVATE Aver.Trifactor Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(TrifactorTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Guarded on the target for the same reason as Trifactor above: -DAVER_MODULE_UPGRADE=OFF is a
# configuration the matrix checks, and it has to keep configuring even though this test cannot exist.
if(TARGET Aver.Upgrade)
  add_executable(UpgradeTest src/UpgradeTest.cpp)
  target_link_libraries(UpgradeTest PRIVATE Aver.Upgrade Aver.Core Aver.Platform)
  set_target_properties(UpgradeTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

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

# MaterialGraphTest -- the .ocgraph -> HLSL material compiler, checked against a REAL SHADER
# COMPILER rather than against an expected string.
#
# IT LINKS NO BACKEND AND CREATES NO DEVICE. dxcompiler.dll is loaded by name at runtime (the test
# does its own LoadLibraryW; see the file's header for why it cannot share the engine's) and HLSL
# compilation is pure CPU, so this runs on a machine with no adapter at all -- and SKIPS, saying so,
# on one with no dxcompiler.dll. modules/rhi.d3d12's CMakeLists already copies that DLL into this
# same bin/ directory, so nothing here has to arrange for it.
#
# Aver.RHI is the GENERIC interface, for sharedShaderPrelude() alone. Never Aver.RHI.D3D12.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(MaterialGraphTest src/MaterialGraphTest.cpp)
  target_link_libraries(MaterialGraphTest PRIVATE
    Aver.Render.PBR.Materials Aver.Formats Aver.RHI Aver.Core Aver.Platform)
  set_target_properties(MaterialGraphTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# MaterialGraphRegistryTest -- pbr::MaterialGraphRegistry's bookkeeping: the ids it hands out, the
# revision it bumps, and that it never recycles an id. NOT dxc: this is about the registry's own
# state, not about whether the HLSL it assembles compiles, which MaterialGraphTest already covers --
# see that file's header for why a real shader compiler is load-bearing there and not here.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(MaterialGraphRegistryTest src/MaterialGraphRegistryTest.cpp)
  target_link_libraries(MaterialGraphRegistryTest PRIVATE
    Aver.Render.PBR.Materials Aver.Formats Aver.RHI Aver.Core Aver.Platform)
  set_target_properties(MaterialGraphRegistryTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ConvertTool -- drives the importer over a real file and writes the whole .oc* triple. Not a test:
# a small harness so the end-to-end path can be exercised without the editor's UI.
add_executable(ConvertTool src/ConvertTool.cpp)
target_link_libraries(ConvertTool PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(ConvertTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Clustering the imported mesh before saving is OPTIONAL, exactly like MaterialTest/ActorScriptTest
# above needing Aver.Formats.Material: `if(TARGET ...)`, not `if(AVER_MODULE_...)`, because an
# unconditional Aver.Trifactor is a hard configure error in the AVER_MODULE_TRIFACTOR=OFF tree this
# repo defaults to (modules/trifactor/CMakeLists.txt), and ConvertTool has to work in that tree.
if(TARGET Aver.Trifactor)
  target_link_libraries(ConvertTool PRIVATE Aver.Trifactor)
endif()
# Cooking the imported glTF's materials is optional for the same reason clustering is, and guarded
# the same way: `if(TARGET ...)`, not `if(AVER_MODULE_PBR)`, so ConvertTool still builds and still
# imports geometry in a tree where PBR is off. With the target absent, the tool says materials were
# skipped rather than leaving the user to wonder where they went.
if(TARGET Aver.Formats.Material)
  target_link_libraries(ConvertTool PRIVATE Aver.Formats.Material)
  target_compile_definitions(ConvertTool PRIVATE AVER_HAVE_MATERIAL_COOK=1)
endif()

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

# RelodTool -- reports the LOD ladder Trifactor would build today for an already-cooked .ocmesh,
# against the one the file carries. Sits here with the other generators for the same reason they do:
# it is a tool, not a test, and it links the same formats+trifactor pair the cook does.
#
#     RelodTool.exe "<project>\Content\Meshes"
if (AVER_MODULE_TRIFACTOR)
    add_executable(RelodTool ${CMAKE_SOURCE_DIR}/tools/RelodTool.cpp)
    target_link_libraries(RelodTool PRIVATE Aver.Formats Aver.Trifactor Aver.Core)
    set_target_properties(RelodTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_executable(MakeSamples ${CMAKE_SOURCE_DIR}/tools/MakeSamples.cpp)
target_link_libraries(MakeSamples PRIVATE Aver.Core)
set_target_properties(MakeSamples PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AverAssetC -- the standalone asset compiler the launcher's Aver Exchange feature shells out to.
# Sits here for ConvertTool's reason: this is where Aver.Formats is already linked, and it ports
# ConvertTool's own import/merge/verify logic rather than duplicating it (see the tool's own header
# comment). Registered as a SHIPPING tool -- unlike ConvertTool, ActorSweep and the test executables,
# it must also be added to scripts/payload.allowlist's [bin] section, or a downloaded engine has
# nothing for the launcher to invoke.
add_executable(AverAssetC ${CMAKE_SOURCE_DIR}/tools/AverAssetC.cpp)
target_link_libraries(AverAssetC PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AverAssetC PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Clustering/LOD decimation is OPTIONAL, exactly like ConvertTool above.
if(TARGET Aver.Trifactor)
  target_link_libraries(AverAssetC PRIVATE Aver.Trifactor)
endif()
# Audio import (.wav/.mp3/.m4a/.flac -> .ocaudio) is OPTIONAL for the same reason MaterialTest/
# ActorSweep guard on Aver.Formats.Material above: an AVER_MODULE_SCRIPTING-independent, always-
# available module in most trees, but a tree can still be configured without Aver.Audio (its own
# CMakeLists gates it), and this tool has to keep configuring in that tree too.
if(TARGET Aver.Formats.Audio)
  target_link_libraries(AverAssetC PRIVATE Aver.Formats.Audio)
  target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_AUDIO_IMPORT=1)
endif()
# Material generation (texture set -> .ocmat, with roughness+metal channel packing) is OPTIONAL for
# the same reason: Aver.Formats.Material only builds under AVER_MODULE_PBR (see its own CMakeLists.txt
# comment), and this tool has to keep configuring in a tree without it too.
if(TARGET Aver.Formats.Material)
  target_link_libraries(AverAssetC PRIVATE Aver.Formats.Material)
  target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_MATERIAL_COMPILE=1)
  # stb_image_write, for the packed metalRough PNG. A SECOND STB_IMAGE_WRITE_IMPLEMENTATION relative
  # to MakeFoliage is fine: they are separate executables, so there is no duplicate symbol to collide.
  target_include_directories(AverAssetC PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
  # Emitting the material's own .ocgraph needs the graph->HLSL compiler on top, so that the tool can
  # PROVE the graph it just wrote shades before pointing the .ocmat at it. Nested inside the material
  # guard because a graph is only ever written beside a .ocmat.
  #
  # LINKING THE COMPILER COSTS NO DEVICE AND NO BACKEND -- MaterialGraphTest above links this exact
  # set and creates neither (see its own comment); compileMaterialGraph is a pure function from an
  # OcGraphData to HLSL text. Aver.RHI comes along because Aver.Render.PBR.Materials declares it
  # PUBLIC, not because anything here calls it.
  if(TARGET Aver.Render.PBR.Materials)
    target_link_libraries(AverAssetC PRIVATE Aver.Render.PBR.Materials)
    target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_MATERIAL_GRAPH=1)
  endif()
endif()

# AverAssetCTest -- the JSON contract Aver Exchange in the LAUNCHER parses.
#
# THIS IS THE ONLY THING PROTECTING A CROSS-PROCESS, CROSS-REPOSITORY INTERFACE. Nothing in this
# repository calls AverAssetC -- correctly, since the launcher shells out to the executable -- so the
# usual protection of "change it, break the build, notice" does not exist. Renaming a JSON key here
# is a green build and a broken importer over there, found by a user.
#
# It RUNS the tool, so it depends on the target rather than merely linking a library.
add_executable(AverAssetCTest src/AverAssetCTest.cpp)
target_link_libraries(AverAssetCTest PRIVATE Aver.Core Aver.Platform)
add_dependencies(AverAssetCTest AverAssetC)
target_compile_definitions(AverAssetCTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(AverAssetCTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OcRigTest -- the .ocrig control-rig format. Text like .ocgraph and for the same reason (a rig is
# four lines a person writes), so the checks are the ones text formats need: parse, round-trip
# through both memory and disk, byte-stable rewrite, and a refusal with a reason for each malformed
# shape.
add_executable(OcRigTest src/OcRigTest.cpp)
target_link_libraries(OcRigTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(OcRigTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
