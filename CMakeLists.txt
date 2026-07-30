cmake_minimum_required(VERSION 3.24)
project(AverEngine VERSION 0.1.0 LANGUAGES C CXX RC)

# ---------------------------------------------------------------------------
# Aver Engine — permissively-licensed, modular 3D engine (see docs/ARCHITECTURE.md).
# Strict-DAG modules; pay-for-what-you-use. This top-level file only wires the
# modules that are IMPLEMENTED. Skeleton-only modules under modules/ carry a
# README describing their eventual responsibility and are added as they land.
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(AvModule)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --- RHI backend options (Vulkan OFF until the LunarG SDK is installed) ---
option(AVER_RHI_D3D12  "DirectX 12 backend (primary)" ON)
option(AVER_RHI_D3D11  "DirectX 11 backend"           ON)
option(AVER_RHI_VULKAN "Vulkan backend (needs Vulkan SDK)" OFF)
option(AVER_BUILD_SANDBOX "Build the sandbox sample app" ON)
option(AVER_BUILD_TESTS "Build test executables" ON)
option(AVER_ENABLE_UI "In-window editor UI (Dear ImGui)" ON)

# --- Optional feature modules (the engine builds and runs with these OFF) ---
option(AVER_MODULE_VOXI "Voxi render module: AA / GI / ray tracing / path tracing settings" ON)
option(AVER_MODULE_PBR  "PBR material system: material library, C ABI and the surface BRDF" ON)
option(AVER_MODULE_SCRIPTING "In-process .NET scripting host (hostfxr/CoreCLR)" ON)
option(AVER_MODULE_SCENE "Entity/component world: entities, transforms, hierarchy and the C ABI" ON)
option(AVER_MODULE_FRAMEWORK "Gameplay framework: game instance, game mode, actors, pawns" ON)
# Physics is declared HERE, beside its peers, rather than down in the third-party section where it
# used to sit. Jolt is the engine's rigid-body backend, not a dependency the build happens to pull
# in, and the switch that turns physics off is the same kind of switch as the five above it. Still
# optional for the same two reasons as before: the engine configures and runs without it, and 153
# translation units are worth being able to opt out of.
option(AVER_MODULE_PHYSICS "Physics module: rigid bodies, characters and raycasts (Jolt backend)" ON)
# Landscape: the chunked heightfield terrain. CPU-only at this stage -- the quadtree, the LOD metric,
# culling and chunk geometry -- so it depends on nothing optional and costs nothing to leave on.
option(AVER_MODULE_LANDSCAPE "Landscape module: chunked heightfield terrain, LOD and chunk geometry" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /MP /utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(
    AVER_PLATFORM_WINDOWS=1
    NOMINMAX WIN32_LEAN_AND_MEAN
    UNICODE _UNICODE
    _CRT_SECURE_NO_WARNINGS)
endif()

message(STATUS "Aver Engine ${PROJECT_VERSION} | D3D12=${AVER_RHI_D3D12} D3D11=${AVER_RHI_D3D11} Vulkan=${AVER_RHI_VULKAN}")

# ---- Third-party (permissively-licensed) ----
if(AVER_ENABLE_UI)
  add_subdirectory(third_party/imgui)
endif()

# ---- Tier 0-6 implemented modules ----
add_subdirectory(modules/core)
add_subdirectory(modules/platform)
add_subdirectory(modules/assets)   # Phase 2
# Audio. Core-only and unconditional for the same reason the UI is: the mixer fills a buffer the
# caller supplies and touches no device, so there is no backend it could depend on and no build
# configuration in which it does not exist. See docs/AUDIO.md.
add_subdirectory(modules/audio)
# ...and the device under it. Windows-only, so it is guarded the way the D3D12 backend is rather
# than being unconditional like the mixer: a build with no backend still has a working mixer.
if(WIN32)
  add_subdirectory(modules/audio.wasapi)
endif()
add_subdirectory(modules/formats)  # Phase 2 (.ocbeam / .ocmap loaders)
# The Roslyn backend for actor scripts. Above Aver.Formats rather than inside it, so the base
# module keeps its no-dependency, no-process guarantee. See its CMakeLists for the full argument --
# including why it is guarded on Aver.Formats.Material rather than on a switch of its own.
add_subdirectory(modules/formats.roslyn)
# Landscape, above formats because it reads .ocland through it and below anything that draws. The
# renderer half is a separate target and is not built yet, which is why this needs no RHI.
# Physics: the backend first, then the module that fronts it. Two directories rather than one for
# the reason rhi/ and rhi.d3d12/ are two -- the ABI above must be expressible without the backend,
# and a link line is the only place that can be enforced.
if(AVER_MODULE_PHYSICS)
  add_subdirectory(modules/physics.jolt)   # vendored Jolt, built as Aver.Physics.Jolt
  add_subdirectory(modules/physics)        # the engine's plain-C ABI over it
endif()
add_subdirectory(modules/rhi)
# Landscape, AFTER the RHI rather than beside formats: the CPU half needs neither, but the renderer
# half is a second target in the same directory and links Aver.RHI, and a target cannot link something
# CMake has not defined yet. The CPU half still depends on nothing but Core and Formats -- that is
# enforced on its own link line, not by where this appears.
if(AVER_MODULE_LANDSCAPE)
  add_subdirectory(modules/landscape)
endif()
if(AVER_RHI_D3D12)
  add_subdirectory(modules/rhi.d3d12)
endif()
if(AVER_RHI_D3D11)
  add_subdirectory(modules/rhi.d3d11)
endif()
if(AVER_RHI_VULKAN)
  add_subdirectory(modules/rhi.vulkan)
endif()
if(AVER_MODULE_PBR)
  add_subdirectory(modules/render.pbr)    # optional: material system + C ABI for C#
endif()
# "PBR is a material system; Voxi is the thing that renders it" is a dependency, not a slogan:
# Aver.Render.Voxi.Renderer links Aver.Render.PBR.Materials and its header includes MaterialSystem.
# Said here rather than left to happen, because the failure without it is a C1083 on a header four
# includes deep in an unrelated translation unit, which reads as a broken include path rather than
# as a module combination that was never valid.
if(AVER_MODULE_VOXI AND NOT AVER_MODULE_PBR)
  message(STATUS "Aver: Voxi renders materials and cannot be built without them - forcing AVER_MODULE_VOXI=OFF")
  set(AVER_MODULE_VOXI OFF)
endif()
if(AVER_MODULE_VOXI)
  add_subdirectory(modules/render.voxi)   # optional: render feature settings + C ABI for C#
endif()
# Scripting sits beside the render modules rather than under them: it needs Core and Platform and
# nothing else, and in particular not the RHI. Added after them only because that is reading order.
if(AVER_MODULE_SCRIPTING)
  add_subdirectory(modules/scripting)     # optional: in-process CLR host + the managed bridge
endif()
# The framework stands on the scene the way Voxi stands on PBR above, and is forced off for the same
# reason: without this line the failure is an unresolved external for every scene entry point, which
# reads as a broken build rather than as a module combination that was never valid.
if(AVER_MODULE_FRAMEWORK AND NOT AVER_MODULE_SCENE)
  message(STATUS "Aver: the framework stands on the scene and cannot be built without it - forcing AVER_MODULE_FRAMEWORK=OFF")
  set(AVER_MODULE_FRAMEWORK OFF)
endif()
if(AVER_MODULE_SCENE)
  add_subdirectory(modules/scene)         # optional: the entity/component world + C ABI for C#
endif()
if(AVER_MODULE_FRAMEWORK)
  add_subdirectory(modules/framework)     # optional: gameplay vocabulary over the world
endif()
# Unconditional and Core-only: it produces a draw list and renders nothing itself, so there is no
# device, no backend and no render feature it could depend on. A build with every optional module off
# still gets a working UI system, which is what makes it testable.
add_subdirectory(modules/ui)              # retained game UI (NOT the editor's ImGui)
# The GPU half of the above, and a SEPARATE target for the reason Aver.Render.Voxi.Renderer is one:
# it needs the RHI, and giving Aver.UI that dependency would end the property that makes the widget
# tree testable with no device. Unconditional too — the RHI always exists, and a backend without GPU
# support declines at create() rather than failing to link.
add_subdirectory(modules/render.ui)
# And the plain-C seam onto it, so a game's HUD is authored in the game's language rather than in
# the engine's. A DLL because the .NET host P/Invokes it; Core-only, like everything under ui/.
add_subdirectory(modules/ui.abi)
# The actor editor's preview. Talks to the generic RHI and never to a backend, so it exists in
# every configuration and declines at create() where there is no GPU.
add_subdirectory(modules/render.actorpreview)
# The audio seam. Added down HERE rather than beside the mixer because it links the WAV reader,
# which is a target inside modules/formats and so does not exist until that has been added.
add_subdirectory(modules/audio.abi)
add_subdirectory(modules/runtime)

if(AVER_BUILD_SANDBOX)
  add_subdirectory(sandbox)
endif()

if(AVER_BUILD_TESTS)
  add_subdirectory(tests/formats)
  # Unconditional, like the module: Aver.UI depends on nothing optional, so there is no combination
  # of switches in which it exists and its test does not.
  add_subdirectory(tests/ui)
  # The renderer's half, and unconditional for the same reason: it talks to the generic RHI, which
  # always exists, and the test supplies its own recording device rather than needing a backend.
  add_subdirectory(tests/render.ui)
  # The actor preview, for the same reason and by the same means.
  add_subdirectory(tests/render.actorpreview)
  # Unconditional, like the module.
  add_subdirectory(tests/audio)
  # The directory watcher. Unconditional because Aver.Platform always exists, and needed because the
  # editor's live sync now rests on it -- it shipped with no consumer and no test, and the first
  # thing that depended on it should not also be the first thing that exercised it.
  add_subdirectory(tests/platform)
  # The editor's preferences store. Unconditional: it depends only on Core and Platform, and its
  # failure mode -- a setting that silently does not persist -- is one nobody ever reports.
  add_subdirectory(tests/editor)
  # The Roslyn backend, checked against the scanner it falls back from. Unconditional like its module;
  # the test itself skips when averdesign is not staged.
  add_subdirectory(tests/formats.roslyn)
  if(AVER_MODULE_LANDSCAPE)
    add_subdirectory(tests/landscape)
  endif()
  if(AVER_MODULE_PHYSICS)
    add_subdirectory(tests/physics)
  endif()
  if(AVER_MODULE_SCENE)
    add_subdirectory(tests/scene)
  endif()
  if(AVER_MODULE_FRAMEWORK)
    add_subdirectory(tests/framework)
  endif()
endif()
