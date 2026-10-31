# AverAssetC -- the standalone asset compiler the launcher's Aver Exchange feature shells out to, and the process extension commands run through
# (docs/EXTENSIONS.md). It is a shipping tool, not a test, so it is built whether or not AVER_BUILD_TESTS is on; the suites that run it
# (AverAssetCTest and the others in tests/formats) depend on this target. Added by the root CMakeLists.txt after every module, so the optional
# ones tested below are already known.
#
# Sits next to ConvertTool's reason for existing: it ports ConvertTool's own import/merge/verify logic rather than duplicating it (see the tool's
# own header comment). Registered as a SHIPPING tool -- unlike ConvertTool, ActorSweep and the test executables, it must also be added to
# scripts/payload.allowlist's [bin] section, or a downloaded engine has nothing for the launcher to invoke.
add_executable(AverAssetC ${CMAKE_SOURCE_DIR}/tools/AverAssetC.cpp)
target_link_libraries(AverAssetC PRIVATE Aver.Formats Aver.Core Aver.Platform Aver.Anim)   # Aver.Anim: the `retarget` subcommand
set_target_properties(AverAssetC PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# The .ocworld Euler encoding (aver/world/LevelTransform.hpp) for the rotated placements a USD
# PointInstancer produces. Header-only and Core-only, so an include directory and no link -- the
# arrangement tests/editor already uses for it.
target_include_directories(AverAssetC PRIVATE ${CMAKE_SOURCE_DIR}/modules/world/include)
# Subcommands this file does not know go to the extension loader, which finds them in <exe dir>/Extensions (docs/EXTENSIONS.md). The model importer
# an extension may call back into (AverExtHost::importModel) is in this file.
target_link_libraries(AverAssetC PRIVATE Aver.Extensions)
# `AverAssetC cook-chunks` (LARGE_WORLD 2.4.5). The cook is the pure half of Aver.World (Core + Formats + Platform). It is compiled straight into the tool, like
# HairCurves.cpp below, rather than by linking Aver.World: that library's PUBLIC link interface carries Aver.Scene, Aver.Physics (Jolt) and Aver.Vehicle, and a shipping
# asset compiler must not depend on them. None of these files may see AVER_MODULE_SCENE (the tool must not link anything that defines it); none does.
set(AVER_WORLD_COOK_SRC ${CMAKE_SOURCE_DIR}/modules/world/src)
target_sources(AverAssetC PRIVATE
  ${AVER_WORLD_COOK_SRC}/ChunkCook.cpp
  ${AVER_WORLD_COOK_SRC}/LevelFlatten.cpp
  ${AVER_WORLD_COOK_SRC}/LevelAssetLibrary.cpp
  ${AVER_WORLD_COOK_SRC}/ChunkCodec.cpp
  ${AVER_WORLD_COOK_SRC}/ChunkPayloadExtras.cpp
  ${AVER_WORLD_COOK_SRC}/RegionFile.cpp
  ${AVER_WORLD_COOK_SRC}/RegionIndex.cpp
  ${AVER_WORLD_COOK_SRC}/ChunkSource.cpp
  ${AVER_WORLD_COOK_SRC}/AuthoredChunkSource.cpp)
target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_COOK=1)
# `AverAssetC hair` (glTF line strands -> .ochair, docs/HAIR.md). HairCurves.cpp is compiled straight into the tool rather than linking Aver.Hair: the library
# also holds the settings builder, which names Aver.Physics, and a shipping asset compiler must not depend on the physics DLL. HairCurves.cpp is pure (Core and
# Formats only).
if(TARGET Aver.Hair)
  target_sources(AverAssetC PRIVATE ${CMAKE_SOURCE_DIR}/modules/hair/src/HairCurves.cpp)
  target_include_directories(AverAssetC PRIVATE ${CMAKE_SOURCE_DIR}/modules/hair/include)
  target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_HAIR=1)
endif()
# Clustering/LOD decimation is OPTIONAL, exactly like ConvertTool in tests/formats.
if(TARGET Aver.Trifactor)
  target_link_libraries(AverAssetC PRIVATE Aver.Trifactor)
endif()
# FBX import (ufbx, docs/FBX_IMPORT.md). Aver.Formats.Fbx exists even without the vendored source, in which case
# `convert x.fbx` fails with "built without ufbx" instead of "unsupported input format".
if(TARGET Aver.Formats.Fbx)
  target_link_libraries(AverAssetC PRIVATE Aver.Formats.Fbx)
  target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_FBX=1)
endif()
# Audio import (.wav/.mp3/.m4a/.flac -> .ocaudio) is OPTIONAL for the same reason MaterialTest/
# ActorSweep guard on Aver.Formats.Material: an AVER_MODULE_SCRIPTING-independent, always-
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
  # LINKING THE COMPILER COSTS NO DEVICE AND NO BACKEND -- MaterialGraphTest in tests/formats links this exact
  # set and creates neither; compileMaterialGraph is a pure function from an
  # OcGraphData to HLSL text. Aver.RHI comes along because Aver.Render.PBR.Materials declares it
  # PUBLIC, not because anything here calls it.
  if(TARGET Aver.Render.PBR.Materials)
    target_link_libraries(AverAssetC PRIVATE Aver.Render.PBR.Materials)
    target_compile_definitions(AverAssetC PRIVATE AVER_HAVE_MATERIAL_GRAPH=1)
  endif()
endif()
