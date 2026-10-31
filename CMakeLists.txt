cmake_minimum_required(VERSION 3.24)
# macOS 14 or later, set before project() because that is where CMake reads it. The C++20 library the
# engine uses (std::format, floating std::to_chars underneath it) is only available in libc++ from a
# 13.3 target on, and 14 is the oldest release docs/PLATFORMS.md supports.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
project(AverEngine VERSION 0.7.0 LANGUAGES C CXX)
# RC ONLY WHERE THERE IS A RESOURCE COMPILER. Naming it in project() above made configure fail on
# line 2 everywhere but Windows ("No CMAKE_RC_COMPILER could be found"), before a single option was
# read. The .rc files are icons and version blocks for Windows executables; nothing else has them.
if(WIN32)
  enable_language(RC)
endif()

# The node palette families (sandbox/src/GraphNodeDefs<Family>.hpp), found here so a new family edits no shared
# file: writes sandbox/src/GraphNodeFamilies.gen.inc, which GraphNodeDefs.hpp includes.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GraphNodeFamilies.cmake")

# THE FULL ENGINE VERSION IS 0.7.0-1, and the "-1" cannot live in project() above: CMake accepts only
# numeric components there. So the suffix is kept here and appended wherever the version reaches the
# binaries (modules/core/CMakeLists.txt). The split is deliberate: upgrade::parseVersion stops at '-' by
# design ("the suffix is not ours to interpret"), so series checks and upgrade planning see 0.7.0, while
# every CREATEDWITH/ENGINE stamp and the UI record the full 0.7.0-1.
# scripts/stage-payload.ps1 and scripts/publish-release.ps1 read only the NUMERIC part of project(); teach
# them this suffix before cutting a release from this series, or the payload and tag will say 0.7.0.
set(AVER_VERSION_SUFFIX "-1")

# ---------------------------------------------------------------------------
# Aver Engine — permissively-licensed, modular 3D engine (see docs/ARCHITECTURE.md).
# Strict-DAG modules; pay-for-what-you-use. This top-level file only wires the
# modules that are IMPLEMENTED. Skeleton-only modules under modules/ carry a
# README describing their eventual responsibility and are added as they land.
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(AvModule)
include(AvModuleManifest)

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
# ONE bin/ ON EVERY PLATFORM. On Windows a SHARED module is a RUNTIME artifact, so its .dll already
# lands in bin/ beside the executables, and everything that finds one at run time -- the managed
# NativeResolver, the scripting bridge, a staged game -- assumes exactly that. A .so or .dylib is a
# LIBRARY artifact instead and would land in lib/, so the same layout is forced here, and the RPATH
# below makes bin/ load its own libraries without LD_LIBRARY_PATH or DYLD_LIBRARY_PATH.
if(NOT WIN32)
  # Every static module is linked into at least one of the SHARED ones (Aver.Core into all of them),
  # and an ELF or Mach-O shared object cannot carry non-PIC code. MSVC has no such distinction.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  if(APPLE)
    set(CMAKE_BUILD_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH "@loader_path")
  else()
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
  endif()
endif()

# --- RHI backend options ---
#
# VULKAN IS ON, AND THE COMMENT THAT USED TO SIT HERE SAID IT WAS "OFF UNTIL THE LUNARG SDK IS
# INSTALLED". That premise was already false in two directions when it was written, and both are
# worth stating because they are why this can be ON without adding a build dependency:
#
#   - The SDK is NOT needed to COMPILE. Vulkan headers are vendored (third_party/vulkan-headers,
#     Apache-2.0), and no import library is linked at all: modules/rhi.vulkan builds every TU with
#     VK_NO_PROTOTYPES and resolves every entry point at runtime from vulkan-1.dll, which ships with
#     the GPU driver rather than the SDK. See that module's CMakeLists for the full argument.
#   - The SDK is NOT needed for SPIR-V either, which was the real blocker for far longer. The
#     Windows SDK's dxcompiler.dll is built WITHOUT the SPIR-V backend, so `-spirv` was accepted and
#     then refused at codegen, and the backend died on its first shader. A SPIR-V-capable DXC is now
#     vendored at third_party/dxc-spirv (official Microsoft DXC v1.9.2607, MIT/LLVM), and
#     modules/rhi.d3d12/CMakeLists.txt already prefers it when copying dxcompiler.dll beside the exe.
#
# So the cost of ON is build time, not a dependency. What it does NOT promise is a correct picture:
# the Vulkan cascade shadow map is still known-broken (pushRenderScope discards the caller's render
# targets, so the cascade atlas is never written; RT shadows mask it). ON here means SELECTABLE and
# COMPILED, which is what RENDER.BACKEND in a .ocproject needs to mean anything -- not "at parity".
# The three backend switches (AVER_RHI_D3D12 / _D3D11 / _VULKAN) are declared with the other module
# options by aver_declare_module_options() below, from modules/rhi.*/module.json.
option(AVER_BUILD_SANDBOX "Build the sandbox sample app" ON)
# The runtime in Runtime/: the Aver.Runtime.Game library, the standalone AverEngineRuntime.exe that
# packaged games run ("Aver Engine Runtime"), and the tests that exercise the library. The option keeps
# its old name on purpose -- renaming it would silently invalidate every existing CMake cache that set it.
option(AVER_BUILD_GAME "Build the runtime (Aver.Runtime.Game + AverEngineRuntime.exe)" ON)
option(AVER_BUILD_TESTS "Build test executables" ON)
# The Fabric Connector extension (extensions/FabricConnector): the converter for Godot 4 projects, built into bin/Extensions/FabricConnector. The
# engine reaches it only through the extension loader, so OFF configures and builds the engine without it.
option(AVER_WITH_FABRIC "Build the Fabric Connector extension (extensions/FabricConnector)" ON)
# CTest, so the suites are runnable by ONE command from a terminal or a build server.
#
# They were not, until now. The only two things that ran them were tools/mcp/aver_mcp.py's
# `aver_tests` -- reachable from an AI agent session and nowhere else -- and running each .exe by
# hand, which README.md documents by listing 14 of the 100+ by name. A check nobody can run in one
# step is a check that quietly stops being run, and this repo already has the commit to prove it
# ("Two skin tests that exited 0 however they went").
#
# Registration is a single recursive walk at the bottom of this file rather than an add_test() in
# each of the 33 tests/*/CMakeLists.txt: those files are long, heavily reasoned, and full of
# `if(TARGET ...)` guards, so 100+ hand-written registrations would be 100+ chances to add a suite
# and forget its line -- exactly the failure the walk cannot have.
enable_testing()
option(AVER_ENABLE_UI "In-window editor UI (Dear ImGui)" ON)

# --- Modules: manifests, editions, options ---
#
# modules/<dir>/module.json says what each module is, its option, its per-edition default and what it
# links; cmake/AvModuleManifest.cmake turns that into option() calls and the dependency rule, and
# cmake/modules.catalog.json (scripts/module-catalog.py) is the same data for the launcher. See
# docs/MODULES.md. To add or change an optional module, edit its manifest, not this file.
#
# AN EDITION is a named module set the launcher can offer and install (scripts/publish-release.ps1
# -Edition; AverLauncherCli installs to <root>/<edition>/<version>/bin). It is a build, not a runtime
# toggle: AVER_MODULE_* is compile-time, and a Sandbox.exe built with a module calls into it through an
# import table. STANDARD IS TODAY'S DEFAULTS, EXACTLY; an explicit -DAVER_MODULE_X=OFF beats the edition.
# Which modules an edition changes is each manifest's "editions" entry.
set(AVER_EDITION "standard" CACHE STRING "Module preset an edition ships: standard | slim | full")
set_property(CACHE AVER_EDITION PROPERTY STRINGS standard slim full)
if(NOT AVER_EDITION MATCHES "^(standard|slim|full)$")
  message(FATAL_ERROR "AVER_EDITION '${AVER_EDITION}' is not a known edition (standard, slim, full)")
endif()

# Declares every optional module's option() (default = the edition's value) and, when the edition or
# AVER_EDITION_REVISION changed since this tree was configured, re-derives the edition-controlled ones.
aver_declare_module_options()
# An enabled module whose dependency is off is an error naming both (or, with
# -DAVER_MODULE_AUTO_DEPS=ON, the dependency is switched on). This replaces the old per-pair
# "forcing X=OFF" guards: silently dropping a module the caller asked for hid the real cause.
aver_enforce_module_deps()

if(MSVC)
  add_compile_options(/W4 /permissive- /MP /utf-8 /Zc:__cplusplus /EHsc)
  # Release ships a PDB next to each binary so crash reports and minidumps resolve to functions. /Zi adds
  # debug info without changing codegen; /OPT:REF /OPT:ICF restate what /DEBUG would otherwise turn off.
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  add_compile_definitions(
    AVER_PLATFORM_WINDOWS=1
    NOMINMAX WIN32_LEAN_AND_MEAN
    UNICODE _UNICODE
    _CRT_SECURE_NO_WARNINGS)
else()
  # GCC and Clang: -Wall -Wextra is the nearest thing they have to /W4.
  add_compile_options(-Wall -Wextra)
endif()

message(STATUS "Aver Engine ${PROJECT_VERSION}${AVER_VERSION_SUFFIX} | D3D12=${AVER_RHI_D3D12} D3D11=${AVER_RHI_D3D11} Vulkan=${AVER_RHI_VULKAN}")

# ---- Third-party (permissively-licensed) ----
# SDL 3, the window/input/audio layer everywhere but Windows (defines AVER_SDL3_TARGET; see the file).
include(AvSdl)
if(AVER_ENABLE_UI)
  add_subdirectory(third_party/imgui)
endif()
# meshoptimizer (MIT), the meshlet/simplify backbone RENDERING.md:42 and FORMAT_SPECS.md:242 already
# named. Added only when something wants it -- it is a static library nobody else links, and a build
# with Trifactor off should not pay to compile twenty translation units it will never call.
if(AVER_MODULE_TRIFACTOR)
  add_subdirectory(third_party/meshoptimizer)
endif()
# ufbx (MIT), the FBX reader behind Aver.Formats.Fbx (docs/FBX_IMPORT.md). Must come before modules/formats; the guard keeps the
# tree configuring when the vendored source is absent (Aver.Formats.Fbx then reports "built without ufbx").
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/ufbx/ufbx.c" AND NOT TARGET ufbx)
  add_subdirectory(third_party/ufbx)
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
else()
  add_subdirectory(modules/audio.sdl)   # the same AudioDevice over SDL 3
endif()
# Durable key/value for a shipped game. Nothing above Core/Platform, so it sits here beside them
# rather than behind a module switch: a game with no scene still has settings.
add_subdirectory(modules/settings)
add_subdirectory(modules/formats)  # Phase 2 (.ocbeam / .ocmap loaders)
# The extension loader and the plain-C extension ABI (aver_ext.h): extensions are directories under <exe dir>/Extensions.
add_subdirectory(modules/extensions)
# The Roslyn backend for actor scripts. Above Aver.Formats rather than inside it, so the base
# module keeps its no-dependency, no-process guarantee. See its CMakeLists for the full argument --
# including why it is guarded on Aver.Formats.Material rather than on a switch of its own.
add_subdirectory(modules/formats.roslyn)
# Animation sampling, above formats because it reads .ocskel/.ocanim through it and below anything
# that draws. Posing a skeleton is arithmetic, so this needs no RHI and never will.
add_subdirectory(modules/anim)
# The plain-C seam onto the property-track player (animation-player model, driven by managed layers). Anim + Formats + Core only.
add_subdirectory(modules/anim.abi)
# Synapse: navigation and pathfinding. Core+Formats only, so it builds and tests with no scene,
# no physics and no device -- see the module's own CMakeLists for why that split is the point.
add_subdirectory(modules/synapse)
# The plain-C seam onto polygon navigation, the navmesh baker and the A* graphs (navigation-server model, driven by managed layers). Synapse + Core only.
add_subdirectory(modules/nav.abi)
# OS sockets and the reliable-UDP host/peer protocol (G6 NET1); opt-in, linked by nothing but its own C seam (Aver.Net.Abi) and tests.
if(AVER_MODULE_NET)
  add_subdirectory(modules/net)
  add_subdirectory(modules/net.abi)
endif()
# Aver Sound: a .ocsnd node graph rendered to PCM, or compiled for the mixer's real-time path. Core, Formats and the (Core-only) mixer
# library, beside Synapse and for the identical reason -- it builds and tests with no device and no scene.
add_subdirectory(modules/sound)
# Sub-bone vertex deformation, above anim because a cage is generated against a skeleton's rest
# pose. Optional: a game that only needs bone skinning does not link it.
if(AVER_MODULE_DEFORM)
  add_subdirectory(modules/deform)
endif()
# Plastic soft bodies (dents that stay, tearing). Core only, a leaf module like Synapse.
add_subdirectory(modules/softbody)
# Trifactor, beside Deform for the same reason: it needs only Core + Formats and is optional.
if(AVER_MODULE_TRIFACTOR)
  add_subdirectory(modules/trifactor)
endif()
# Upgrade: the project migration chain, from the engine version a project was made in to this one.
# Needs only Core -- a step edits files on a path it is handed and never sees the manifest struct --
# so it sits here with the other leaf modules.
if(AVER_MODULE_UPGRADE)
  add_subdirectory(modules/upgrade)
endif()
# Media: video file output for the render queue. Core only, so it sits with the other leaf modules.
if(AVER_MODULE_MEDIA)
  add_subdirectory(modules/media)
endif()
# Landscape, above formats because it reads .ocland through it and below anything that draws. The
# renderer half is a separate target and is not built yet, which is why this needs no RHI.
# Physics: the backend first, then the module that fronts it. Two directories rather than one for
# the reason rhi/ and rhi.d3d12/ are two -- the ABI above must be expressible without the backend,
# and a link line is the only place that can be enforced.
if(AVER_MODULE_PHYSICS)
  add_subdirectory(modules/physics.jolt)   # vendored Jolt, built as Aver.Physics.Jolt
  add_subdirectory(modules/physics)        # the engine's plain-C ABI over it

  # The vehicle as data (.ocvehicle, the spec, the builder): Core + Formats + the physics C ABI, so it sits right behind physics (docs/VEHICLES.md).
  if(AVER_MODULE_VEHICLE)
    add_subdirectory(modules/vehicle)
  endif()
endif()
# Strand hair: the .ochair groom's generator, glTF curve import, collision proxy hulls and the settings builder over the physics hair ABI (docs/HAIR.md). Core,
# Formats and Aver.Physics's C ABI only; the scene and render halves are later phases. The option comes from modules/hair/module.json.
if(AVER_MODULE_PHYSICS AND AVER_MODULE_HAIR)
  add_subdirectory(modules/hair)
endif()
add_subdirectory(modules/rhi)
# Landscape, AFTER the RHI rather than beside formats: the CPU half needs neither, but the renderer
# half is a second target in the same directory and links Aver.RHI, and a target cannot link something
# CMake has not defined yet. The CPU half still depends on nothing but Core and Formats -- that is
# enforced on its own link line, not by where this appears.
if(AVER_MODULE_LANDSCAPE)
  add_subdirectory(modules/landscape)
endif()
# Occlusion, beside Landscape for the identical reason: it needs Aver.RHI (the generic interface,
# never a backend -- see modules/occlusion/CMakeLists.txt) and nothing else, so it belongs after
# modules/rhi and does not care what comes after IT.
if(AVER_MODULE_OCCLUSION)
  add_subdirectory(modules/occlusion)
endif()
if(AVER_RHI_D3D12)
  add_subdirectory(modules/rhi.d3d12)
  # The concrete Dear ImGui implementation of that module's IUiBackend seam -- see
  # modules/rhi.d3d12.imgui/CMakeLists.txt. Nested here (it DEPS on Aver.RHI.D3D12) and additionally
  # gated on AVER_ENABLE_UI, since there is nothing for it to back when UI is off. Only Sandbox links
  # it -- see sandbox/CMakeLists.txt's matching if(TARGET Aver.RHI.D3D12.ImGui), which is also what
  # decides whether Sandbox gets AVER_WITH_IMGUI defined at all.
  if(AVER_ENABLE_UI)
    add_subdirectory(modules/rhi.d3d12.imgui)
  endif()
endif()
if(AVER_RHI_D3D11)
  add_subdirectory(modules/rhi.d3d11)
endif()
if(AVER_RHI_VULKAN)
  add_subdirectory(modules/rhi.vulkan)
  # The concrete Dear ImGui implementation of that module's IUiBackend seam, gated exactly as the
  # D3D12 one is: nested (it DEPS on Aver.RHI.Vulkan) and additionally on AVER_ENABLE_UI, since
  # there is nothing for it to back when UI is off. Only Sandbox links it.
  if(AVER_ENABLE_UI)
    add_subdirectory(modules/rhi.vulkan.imgui)
  endif()
endif()
if(AVER_MODULE_PBR)
  add_subdirectory(modules/render.pbr)    # optional: material system + C ABI for C#
endif()
if(AVER_MODULE_VOXI)
  add_subdirectory(modules/render.voxi)   # optional: render feature settings + C ABI for C#
endif()
# AverSR, beside Voxi for the same reason Deform sits beside Trifactor: it is optional and needs
# only Aver.Core and Aver.RHI, so where it is added relative to Voxi/PBR does not matter -- unlike
# Voxi it takes no dependency on PBR (see docs/AVERSR.md: an upscaler resamples a colour target and
# has no legitimate reason to know what a material is).
if(AVER_MODULE_SR)
  add_subdirectory(modules/render.sr)     # optional: AverSR upscaler implementations
endif()
# Frame interpolation (docs/rendering/NEURAFI.md). Unconditional for the same reason as PCG
# below: it needs only the generic RHI, and the device keeps it switched off until a host enables it.
add_subdirectory(modules/render.neurafi)
# PCG's GPU half. Needs only the RHI, so it is unconditional like the UI: there is no combination of
# switches in which the RHI exists and a compute shader that writes a 3D texture does not.
add_subdirectory(modules/render.pcg)
# Scripting sits beside the render modules rather than under them: it needs Core and Platform and
# nothing else, and in particular not the RHI. Added after them only because that is reading order.
if(AVER_MODULE_SCRIPTING)
  add_subdirectory(modules/scripting)     # optional: in-process CLR host + the managed bridge
endif()
if(AVER_MODULE_SCENE)
  add_subdirectory(modules/scene)         # optional: the entity/component world + C ABI for C#
  # The scene/animation join. Guarded on BOTH, because it is the edge between them and neither owns
  # it; a build with either switched off simply does not have the join.
  add_subdirectory(modules/anim.scene)
  # Ragdolls on skinned entities: the join between the animation seam and the physics ABI, so it
  # needs all of scene, anim.scene and physics; with any of them off it is simply not there.
  if(AVER_MODULE_RAGDOLL AND AVER_MODULE_PHYSICS)
    add_subdirectory(modules/ragdoll)
  endif()
  # Cloth and destruction on scene entities over Jolt soft bodies (docs/SOFTBODY.md). Needs scene, anim.scene and physics;
  # the manifest enforces that, so with any of them off the module is simply not there.
  if(AVER_MODULE_SOFTBODY_SCENE)
    add_subdirectory(modules/softbody.scene)
  endif()

  # Drivable vehicles on scene entities over Jolt's vehicle constraint (docs/VEHICLES.md). Needs vehicle, scene and physics; the manifest enforces that.
  if(AVER_MODULE_VEHICLE_SCENE)
    add_subdirectory(modules/vehicle.scene)
  endif()
  # Strand hair on scene entities (CHair, HairSystem; docs/HAIR.md). Needs hair, physics, scene and anim.scene; the manifest enforces that.
  if(AVER_MODULE_HAIR_SCENE AND TARGET Aver.Hair)
    add_subdirectory(modules/hair.scene)
  endif()

  # Gameplay cameras (docs/CAMERAS.md): spring-arm rigs, view-target blends and the camera modifier stack on scene entities. Needs scene and
  # physics; the manifest enforces that, so with either off the module is simply not there.
  if(AVER_MODULE_CAMERA)
    add_subdirectory(modules/camera)
  endif()
  # Save/load of a whole world. Guarded on SCENE for the same reason anim.scene is: it is the edge
  # between the world and the container format, and a build with no scene has no world to save.
  add_subdirectory(modules/save)
  # Prefab instances in a live world (spawn, overrides, propagation); see docs/PREFABS.md.
  add_subdirectory(modules/prefab)
  # [wave3/level-instances begin] The plain-C seam onto level instances (docs/LEVEL_INSTANCES.md).
  add_subdirectory(modules/levelinstance.abi)
  # [wave3/level-instances end]
  # The scene/navigation join, same shape as anim.scene immediately above and for the same reason:
  # Aver.Synapse stays pure grid-and-arithmetic (added unconditionally, above), and this is the
  # "somebody" that turns a goal into a scene::Entity's actual path.
  add_subdirectory(modules/synapse.scene)
  # The plain-C seam onto steering/crowds, hearing, cover and squads (docs/AI_CROWDS_HEARING_COVER.md).
  add_subdirectory(modules/synapse.abi)
endif()
if(AVER_MODULE_FRAMEWORK)
  add_subdirectory(modules/framework)     # optional: gameplay vocabulary over the world
endif()
# Strand hair drawing (phase G/H): Jolt's hair kernels on the RHI, the ribbon renderer, AVER_MODEL_HAIR's host side.
if(AVER_MODULE_RENDER_HAIR)
  add_subdirectory(modules/render.hair)
endif()
# Soft-body drawing (needs the scene and physics -- enforced from its manifest).
if(AVER_MODULE_RENDER_SOFTBODY)
  add_subdirectory(modules/render.softbody)
endif()

# The SIMULATED half of Aver.Fluids is not a switch of its own any more. It used to be
# AVER_MODULE_RENDER_FLUID, a separate module and a separate option that had to be force-disabled
# here whenever water or physics was missing; now it is one module that decides internally, adding
# FluidScene.cpp and defining AVER_FLUIDS_SIMULATED only when AVER_MODULE_PHYSICS is on (see
# modules/fluids/CMakeLists.txt for why the physics dependency is scoped rather than inherited).
# Deriving it there rather than declaring an option here is what makes the two impossible to
# disagree: a user cannot ask for simulated fluids in a tree with no solver to simulate them.
if(AVER_MODULE_FLUIDS)
  # NOT force-disabled by AVER_MODULE_SCENE the way particles is, and deliberately: a particle
  # emitter is a scene component so particles cannot exist without a world, whereas water in this
  # slice is one global plane the host owns. It builds in a scene-less tree, which is also what
  # keeps its wave math testable with no world at all.
  add_subdirectory(modules/fluids)        # optional: analytic surface, underwater fog, fluid volumes
endif()
if(AVER_MODULE_PARTICLES)
  add_subdirectory(modules/particles)     # optional: CPU particle simulation + the billboard renderer
endif()
# The .ocparticle reader/writer (DECIDED 3), unconditionally added right after Particles for
# modules/formats.roslyn's exact reason one dependency hop over: it depends on a target this file
# only just (conditionally) defined, and its own CMakeLists.txt guards itself with
# `if(TARGET Aver.Particles)` rather than re-deriving AVER_MODULE_PARTICLES's cascade here.
add_subdirectory(modules/formats.particles)
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
# GPU skinning. Down here rather than beside modules/anim because it needs the RHI, and generic
# enough to exist in every configuration for the same reason the preview does.
add_subdirectory(modules/render.skin)
# The path tracer, its white-furnace oracle, and PtSceneView (the reference view over a REAL scene).
# Unconditional and RHI-only, like the two above -- PathTracer owns its own geometry and PtSceneView
# takes a real level's authored colour through a host-installed callback rather than a material type,
# so there is no world, material system or render feature this must depend on, and it declines at
# init() on a device without DXR 1.1. Its position in this file therefore carries no ordering
# requirement; it sits here only because that is where it was added.
add_subdirectory(modules/render.pt)
# The denoiser (AMD FidelityFX Denoiser, MIT, plus this engine's host code). Unconditional and
# RHI-only for the same reason the three above are; whether it runs is decided at runtime.
add_subdirectory(modules/render.denoise)
# Small neural networks that run inside the frame (MLP inference + training). Unconditional and RHI-only, like the denoiser.
add_subdirectory(modules/render.neural)
# The GPU crowd-avoidance backend. RHI-only, like the modules above; the pure solver stays device-free.
add_subdirectory(modules/synapse.gpu)
# The audio seam. Added down HERE rather than beside the mixer because it links the WAV reader,
# which is a target inside modules/formats and so does not exist until that has been added.
add_subdirectory(modules/audio.abi)
# The scene join for audio (reverb zones, occlusion probes). After audio.abi, which it optionally links.
if(AVER_MODULE_SCENE)
  add_subdirectory(modules/audio.scene)
endif()
# The control channel. Core-only, so it could sit anywhere; here because it is the editor that uses it.
if(AVER_MODULE_MCP AND WIN32)
  add_subdirectory(modules/mcp)
endif()
# The level/world runtime: .ocworld placements into entities, and the region streaming this module
# has always been scoped to. UNCONDITIONAL and added HERE -- after modules/scene and modules/formats,
# which it links, and before both hosts, which link it. It is what stops "how a placement becomes an
# entity" from existing once in the editor and once in the game runtime.
add_subdirectory(modules/world)
add_subdirectory(modules/runtime)
# THE RUNTIME: Runtime/ holds the Aver.Runtime.Game library and, under Runtime/host, the standalone
# AverEngineRuntime.exe (was modules/runtime.game and game/AverGame.exe until 2026-09-16). See
# Runtime/host/CMakeLists.txt for the executable's history and the divergence gate it ships with.
#
# ORDER: after modules/runtime, which the library links, and after modules/render.sr, which the host's
# if(TARGET Aver.Render.Sr) must already be able to see. BEFORE sandbox, because the editor is to link
# this library as its private copy of the runtime is retired.
#
# Added for EITHER host: Aver.Runtime.Game.Core is the editor's too. Runtime/CMakeLists.txt builds
# GameApp and the executable only under AVER_BUILD_GAME.
if(AVER_BUILD_GAME OR AVER_BUILD_SANDBOX)
  add_subdirectory(Runtime)
endif()

if(AVER_BUILD_SANDBOX)
  add_subdirectory(sandbox)
endif()

# AverAssetC, the asset compiler (and the process extension commands run through). A tool, not a test: built with the tests off too, so an extension
# built out of tree (docs/EXTENSIONS.md) gets it. After every module, whose optional ones it tests for.
add_subdirectory(tools)

# EXTENSIONS built from this tree. Each is a directory the engine finds at run time under bin/Extensions (modules/extensions); nothing above
# reaches one except through that loader. Fabric Connector converts Godot 4 projects (docs/FABRIC.md); it is meant to move to its own repository
# (docs/plans/fabric/EXTENSION.md), and with this option OFF the engine configures and builds without it. After every module and the editor, so
# the optional modules its targets link are already known.
if(AVER_WITH_FABRIC)
  add_subdirectory(extensions/FabricConnector)
endif()
# Extensions that live in their own repositories (docs/EXTENSIONS.md): AVER_EXTERNAL_EXTENSIONS lists their source directories, each added the same way,
# after every module and tool. Empty by default, so nothing changes for a build that has none.
set(AVER_EXTERNAL_EXTENSIONS "" CACHE STRING "Source directories of extensions built from outside this tree (a ;-separated list)")
foreach(_aver_ext_dir IN LISTS AVER_EXTERNAL_EXTENSIONS)
  cmake_path(GET _aver_ext_dir FILENAME _aver_ext_name)
  add_subdirectory("${_aver_ext_dir}" "${CMAKE_BINARY_DIR}/extensions/${_aver_ext_name}")
endforeach()

# THE MODULE DAG CHECK, here rather than inside aver_add_module.
#
# This is the earliest point at which the question is answerable: CMake permits forward references,
# and this tree uses them (Aver.Assets.Gpu names Aver.Formats and Aver.RHI, both added later), so a
# check at the point of declaration cannot tell "declared later" from "optional and absent" and fails
# the default build. Here, every target exists.
#
# It catches exactly one bug class -- an optional module named without an if(TARGET ...) guard -- and
# that class is worth catching because it does not fail where it is written. It fails as LNK1104 in
# some unrelated target, which is how AVER_MODULE_PBR=OFF was broken. Everything else about
# modularity is a C++ preprocessor property of one configuration, and only scripts/module-matrix.ps1,
# which actually builds them, can decide it.
aver_check_module_dag()
# ...and the same moment answers whether the module manifests describe what was just configured.
aver_check_module_manifests()

# AverCrashReporter -- the separate process that displays a crash report.
#
# DECLARED HERE, UNCONDITIONALLY, and not with the other tools/ executables, which is a deliberate
# break from that pattern rather than an oversight. Every other tool (AverAssetC, MakeFoliage,
# MakeSamples...) is wired up inside tests/formats/CMakeLists.txt, which only runs under
# if(AVER_BUILD_TESTS) below. That is fine for an asset compiler. It is wrong for a crash reporter:
# configuring with -DAVER_BUILD_TESTS=OFF would produce a Sandbox.exe that installs crash handlers,
# writes crash folders, and then tries to launch a reporter that was never built -- the failure mode
# being silence at exactly the moment the user most needs to be told something.
#
# LINKS NOTHING FROM THE ENGINE. Not Aver.Core, not Aver.Platform, nothing. It has to still work when
# the engine does not, and the only way to be certain of that is to share no code with it. Win32 and
# the CRT only; see the file's own header comment.
#
# WINDOWS ONLY. The reporter is a Win32 dialog, and so is the report it shows: off Windows
# modules/core/src/CrashReport.cpp is a stated no-op and a crash is the system's own.
if(WIN32)
add_executable(AverCrashReporter tools/AverCrashReporter.cpp)
set_target_properties(AverCrashReporter PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
if(MSVC)
  # A GUI subsystem app: it must not flash a console window when the engine spawns it, and it is
  # spawned DETACHED_PROCESS precisely so it outlives the crash. main() is still the entry point --
  # /ENTRY:mainCRTStartup keeps the standard signature rather than forcing a WinMain.
  target_link_options(AverCrashReporter PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

if(AVER_BUILD_TESTS)
  # Needs Aver.Runtime.Game.Core, which exists whenever either host is built.
  if(TARGET Aver.Runtime.Game.Core AND TARGET Aver.Framework)
    add_subdirectory(tests/game)
  endif()
  add_subdirectory(tests/pcg)
  add_subdirectory(tests/formats)
  add_subdirectory(tests/extensions)   # the extension loader against fake extensions and a tiny test library
  # The render queue's video encoder; skips (exit 77) on a machine without an H.264 encoder.
  if(TARGET Aver.Media)
    add_subdirectory(tests/media)
  endif()
  # Unconditional, like the module: Aver.UI depends on nothing optional, so there is no combination
  # of switches in which it exists and its test does not.
  add_subdirectory(tests/ui)
  # The renderer's half, and unconditional for the same reason: it talks to the generic RHI, which
  # always exists, and the test supplies its own recording device rather than needing a backend.
  add_subdirectory(tests/render.ui)
  # The neural module's CPU reference: the maths its shaders must reproduce, checked without a GPU.
  add_subdirectory(tests/render.neural)
  # NRD2's CPU twin (pyramid, resolve forward/backward, oracle fit) and its pose files.
  add_subdirectory(tests/render.denoise)
  # The actor preview, for the same reason and by the same means.
  add_subdirectory(tests/render.actorpreview)
  # The layered BSDF's coat lobe. Unconditional, like the PBR material system it belongs to, and
  # needed because "does this coat add energy" is a question about arithmetic that a rendered image
  # cannot answer -- a plate 3% too bright still looks like a plate.
  add_subdirectory(tests/render.pbr)
  # Unconditional, like the module.
  add_subdirectory(tests/audio)
  # The atmosphere model. Unconditional because Aver.RHI always exists, and needed because the sky
  # is now derived rather than authored -- an airmass that is quietly 20% low still renders a blue
  # sky, so the only thing that can catch it is a numeric integral of the same physics.
  add_subdirectory(tests/rhi)
  # The animation sampler. Unconditional, like its module: a slerp that takes the long way round or
  # a cubic tangent scaled by the wrong interval is invisible until a character is on screen and
  # slightly wrong, and is decidable here by arithmetic.
  add_subdirectory(tests/anim)
  if(AVER_MODULE_DEFORM)
    add_subdirectory(tests/deform)
  endif()
  if(TARGET Aver.SoftBody)
    add_subdirectory(tests/softbody)
  endif()
  # Trifactor's runtime selection maths -- screen-space error, frustum and cone culling. A separate
  # directory from tests/formats (which holds TrifactorTest, the COOK side) because the two halves
  # fail for unrelated reasons: one is a file format, the other is arithmetic against a camera, and a
  # failure should name which. Without this line tests/trifactor/CMakeLists.txt exists and is inert.
  if(AVER_MODULE_TRIFACTOR)
    add_subdirectory(tests/trifactor)
  endif()
  # The directory watcher. Unconditional because Aver.Platform always exists, and needed because the
  # editor's live sync now rests on it -- it shipped with no consumer and no test, and the first
  # thing that depended on it should not also be the first thing that exercised it.
  add_subdirectory(tests/platform)
  # The editor's preferences store. Unconditional: it depends only on Core and Platform, and its
  # failure mode -- a setting that silently does not persist -- is one nobody ever reports.
  add_subdirectory(tests/editor)
  # The graph editor's model code (Problems, Find, clipboard, debugger model, Collapse to Function): header-only
  # and ImGui-free in sandbox/src, so it runs headless like tests/editor.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/graphtools/CMakeLists.txt")
    add_subdirectory(tests/graphtools)
  endif()
  # The engine/project boundary. Unconditional, and links only Aver.Core, because what it checks is a
  # property of the SOURCE TREE rather than of any module: that no game content sits in the engine and
  # that one file knows where projects live. docs/PROJECTS.md asserts both in prose; this decides them.
  add_subdirectory(tests/repo)
  # The C ABI's integer constants against the C# enums that restate them. Unconditional and
  # Aver.Core-only for the same reason tests/repo is: it reads the headers and the .cs files as
  # SOURCE rather than exercising either side, so it needs no device and no .NET SDK, and runs on a
  # machine without one.
  add_subdirectory(tests/abi)
  # The Roslyn backend, checked against the scanner it falls back from. Unconditional like its module;
  # the test itself skips when averdesign is not staged.
  add_subdirectory(tests/formats.roslyn)
  if(AVER_MODULE_LANDSCAPE)
    add_subdirectory(tests/landscape)
  endif()
  if(AVER_MODULE_OCCLUSION)
    # The HZB conservative-culling math -- projection, mip selection, the four-corner test. Guarded
    # by the module because it links Aver.Occlusion; CPU-only, like tests/trifactor, because what it
    # decides (a box straddling an occluder boundary must never read as hidden) is arithmetic no
    # screenshot needs to settle first.
    add_subdirectory(tests/occlusion)
  endif()
  if(AVER_MODULE_VOXI)
    # Voxi's ray-traced sun-disc sampling. Guarded by the module because it reads the module's own
    # shader source, and CPU-only because the property it decides -- that raising the ray count adds
    # samples instead of moving them -- is arithmetic no screenshot can settle.
    add_subdirectory(tests/render.voxi)
  endif()
  if(AVER_MODULE_MCP AND WIN32)
    add_subdirectory(tests/mcp)
  endif()
  if(AVER_MODULE_PHYSICS)
    add_subdirectory(tests/physics)

    if(TARGET Aver.Hair)
      add_subdirectory(tests/hair)
    endif()
  endif()

  if(TARGET Aver.Vehicle)
    add_subdirectory(tests/vehicle)
  endif()
  if(AVER_MODULE_SCENE)
    add_subdirectory(tests/scene)
    add_subdirectory(tests/anim.scene)
    if(TARGET Aver.Ragdoll)
      add_subdirectory(tests/ragdoll)
    endif()
    if(TARGET Aver.SoftBody.Scene)
      add_subdirectory(tests/softbody.scene)
    endif()

    if(TARGET Aver.Vehicle.Scene)
      add_subdirectory(tests/vehicle.scene)
    endif()

    if(TARGET Aver.Camera)
      add_subdirectory(tests/camera)
    endif()
    add_subdirectory(tests/save)
    add_subdirectory(tests/prefab)
    # [wave3/level-instances begin]
    add_subdirectory(tests/levelinstance)
    # [wave3/level-instances end]
    if(TARGET Aver.Formats)
      add_subdirectory(tests/decals)
    endif()
    # The placement -> entity loop the editor and the game runtime now share. Guarded on the scene
    # for the same reason the module's own source is: without it there is nothing to instantiate into.
    add_subdirectory(tests/world)
  endif()
  if(TARGET Aver.Settings)
    add_subdirectory(tests/settings)
  endif()

  if(TARGET Aver.Net)
    add_subdirectory(tests/net)   # G6 NET1: the protocol on a simulated network, sockets over loopback only
  endif()

  if(TARGET Aver.Synapse)
    add_subdirectory(tests/synapse)
    add_subdirectory(tests/synapse_bt)
    add_subdirectory(tests/synapse_query)   # AI: wave 3 #21
    add_subdirectory(tests/sound)
  endif()
  if(AVER_MODULE_FRAMEWORK)
    add_subdirectory(tests/framework)
    add_subdirectory(tests/framework_timers)
    add_subdirectory(tests/framework_anim)
    add_subdirectory(tests/framework_import)   # wave9 FBX_IMPORT: the model import ABI relay
    add_subdirectory(tests/framework_ik)   # wave5 #24: the IK / rig-input ABI relay
    add_subdirectory(tests/framework_worldfx)   # wave9 VISUAL_SCRIPTING_CSHARP_SYNC: sun / fog / sky light and post layer overrides
    add_subdirectory(tests/framework_subview)   # G4 R5: sub-viewport registry rules and the C ABI over them
    add_subdirectory(tests/framework_rtmesh)   # G4 R7: runtime meshes, MultiMesh baking, CSG and the C ABI over them
    add_subdirectory(tests/framework_envfx)   # G4 R8: fog volume and reflection probe maths and the C ABI over them
    add_subdirectory(tests/framework_origin)   # wave11 LARGE_WORLD: the floating-origin ABI relay and the WorldPos conversions

    add_subdirectory(tests/framework_morph)   # wave9 MORPH_TARGETS: the blend-shape / lip-sync ABI relay
add_subdirectory(tests/framework_softfield)

    add_subdirectory(tests/framework_softshape)   # wave11 SOFTBODY_HAIR: the jelly / rope / grab / buoyancy ABI relay and the Jelly and Rope Details model

    add_subdirectory(tests/framework_camera)   # wave10 GAMEPLAY_CAMERAS: the camera rig / view-target / modifier ABI relay
    add_subdirectory(tests/framework_sequence)   # wave5 #27: the sequencer ABI relay
    add_subdirectory(tests/framework_access)   # wave9 accessibility: options, captions, speech, camera feel, input assist

    add_subdirectory(tests/framework_particles)   # wave10 GPU_PARTICLES: the particle ABI relay (fake provider, no particle module)
    add_subdirectory(tests/framework_content)

    add_subdirectory(tests/framework_vehicle)   # wave10 VEHICLES: the vehicle ABI relay
    add_subdirectory(tests/framework_hair)   # wave11 SOFTBODY_HAIR: the hair ABI relay (fake provider) and hair::hairProvider on an empty HairSystem
    add_subdirectory(tests/gas)
    add_subdirectory(tests/managed)   # Aver.Graph.Tests and Aver.DataLoc.Tests, wired into nothing before
  endif()
  if(AVER_MODULE_PARTICLES)
    # The CPU simulation only -- spawn rate, lifetime, gravity/damping integration. Guarded by the
    # module because it links Aver.Particles/Aver.Scene, CPU-only because what it decides (a particle
    # count and a position after N seconds) is arithmetic no screenshot needs to settle first, the
    # same reasoning tests/occlusion and tests/trifactor give for their own CPU-only suites.
    add_subdirectory(tests/particles)
  endif()
  if(AVER_MODULE_FLUIDS)
    add_subdirectory(tests/fluids)
  endif()
  if(AVER_MODULE_RENDER_SOFTBODY)
    add_subdirectory(tests/render.softbody)
  endif()

  if(TARGET Aver.Render.Hair)
    add_subdirectory(tests/render.hair)   # HairRegsTest, RibbonPackTest, HairMaterialTest, HairSelfTest (headed, exit 77 without a device)
  endif()
  if(TARGET Aver.Render.Skin)
    add_subdirectory(tests/render.skin)   # wave9 MORPH_TARGETS: the GPU blend-shape layout
  endif()
endif()

# ---- CTest registration -------------------------------------------------------------------------
#
# Walks every directory added above and registers each EXECUTABLE whose name ends in `Test`. That
# suffix is the existing convention -- aver_mcp.py's `aver_tests` already globs `bin/*Test.exe` --
# so this registers exactly the set that was already being run, and keeps doing so when a suite is
# added or a module option removes one. Tools that live beside the suites (ConvertTool, ActorSweep,
# AudioProbe, DumpClusterPs) do not match and are not registered.
#
# PASS IS NOT JUST EXIT 0. Several suites have historically exited 0 however they went -- there is a
# commit named for it, and docs/STALE_CODE.md records another instance where an `ok_` flag was set
# and never read. FAIL_REGULAR_EXPRESSION is the second, independent signal.
#
# THE PATTERN IS EXACT ON PURPOSE, and matching the bare word does not work. Suites describe the
# negative cases they cover in prose -- "an OBJ with no faces FAILS rather than returning an empty
# mesh", "AND IT STAYS FAILED", "a bad input FAILS detectably" -- so a naive /FAIL/ marks six
# perfectly green suites red, which is what the first attempt at this line did. What only ever
# appears on a real failure is the shared check() helper's own formatting, `FAIL` followed by two
# spaces, and the summary line's `FAILED ===`. Verified against the passing output of all six of
# those suites: zero matches.
#
# VULKAN ROWS -- OPT-IN, NOT BLANKET. A few of these executables accept a `vulkan` argument and, when
# given it, run the identical assertions against the Vulkan backend instead of D3D12; each one says
# so in its own header comment (tests/rhi/src/LineMeshTest.cpp:43-49,
# tests/rhi/src/ShaderIncludeResolveTest.cpp:71-74). Before AVER_CTEST_VULKAN_TARGETS existed that
# argument was reachable only by a human typing it at a shell: add_test below never passed it, so
# LineMeshTest's Vulkan path -- written on purpose, with a comment about exactly this failure mode --
# had never once executed in a gate run or in CI. The list is the fix, and it is a small, explicit
# allowlist kept right beside the function that reads it, rather than a property each target would
# set on itself in its own CMakeLists.txt, because the root CMakeLists.txt is the one file this
# change is scoped to touch; a target earns a place on the list only once its own source is confirmed
# (by reading it, not by guessing from its name) to branch on argv the way the two above do. The
# blanket alternative -- give every *Test target a second row and pass `vulkan` unconditionally -- is
# wrong on purpose: a target that ignores argv would run its D3D12 path twice and report it as two
# passing rows, which is MORE false confidence than the single untested row it would replace, and
# false confidence in this exact spot is the whole reason this round of work exists.
#
# ON A MACHINE WITH NO VULKAN DRIVER, a `.vulkan` row PASSES, not fails, and does not show ctest's own
# separate "Not Run" state. Both suites above ask createDevice for Vulkan with Null as the only
# fallback (no silent retry of the backend already covered by the plain row), and when neither is
# available they log a line containing the word SKIPPED and `return 0` -- they were written to be
# safe to run by hand on any machine, long before this row existed, and neither calls
# set_tests_properties(... SKIP_RETURN_CODE ...) to ask ctest for a dedicated skipped state. Exit 0
# plus a log that contains neither `FAIL  ` nor `FAILED ===` reads, under the regex below, as an
# ordinary ctest Passed. That is the correct failure mode for THIS row to have: a `.vulkan` row must
# never turn red on a driver-less runner, because a red row someone did not expect is a row someone
# deletes, which is the exact regression -- a Vulkan check that was written and then silently stopped
# running -- this whole change exists to undo. The cost is that a green `.vulkan` row is not proof
# Vulkan itself ran; that has to come from reading the row's own output ("running against backend
# Vulkan" vs a SKIPPED line), the same way a WARP fallback already has to be told apart from a real
# GPU run today.
set(AVER_CTEST_VULKAN_TARGETS
  LineMeshTest
  ShaderIncludeResolveTest
  NeuralGpuParityTest
  ColourVisionShaderTest
  IndirectRhiTest
  ParticleGpuParityTest
)
function(aver_register_ctest_in dir)
  get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t IN LISTS _targets)
    if(_t MATCHES "Test$")
      get_target_property(_type ${_t} TYPE)
      if(_type STREQUAL "EXECUTABLE")
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES
          # Matches aver_tests' own per-suite budget, so a hang fails rather than wedging a build.
          TIMEOUT 600
          # bin/, because several suites resolve fixtures relative to the executable the same way a
          # shipped editor resolves shaders and splash.png.
          WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
          FAIL_REGULAR_EXPRESSION "FAIL  |FAILED ==="
          # Its own user data dir (editor.ini, weights, caches): never the developer's, never another suite's.
          ENVIRONMENT "AVER_USER_DATA_DIR=${CMAKE_BINARY_DIR}/Testing/userdata/${_t}"
          # ON EVERY ROW, NOT ONLY THE .vulkan ONES, AND THAT IS NOT BELT-AND-BRACES. The suites
          # that learned to exit 77 reach their skip paths from the PLAIN row too -- a machine with
          # no D3D12 runtime, or a backend with no createLineMesh, skips exactly the same way. Set
          # on the Vulkan row alone, this change would have turned those previously-green default
          # rows red, which is a regression introduced by a change whose entire purpose was to stop
          # tests lying about what they covered. Harmless on every other target: a suite that never
          # exits 77 never notices this property, and one that learns to later gets the right
          # behaviour without anyone having to remember this line.
          SKIP_RETURN_CODE 77)
        # A second, opt-in row for the targets listed in AVER_CTEST_VULKAN_TARGETS above (see that
        # comment for why this is a named allowlist and not "every Test target gets one"). Same
        # executable, same WORKING_DIRECTORY, same FAIL_REGULAR_EXPRESSION -- the regex matches text
        # the process PRINTS, not the ctest test name, so a `.vulkan`-suffixed name needs no regex of
        # its own -- with `vulkan` appended to argv so the target's own argv-parsing (grep the source
        # before adding a name here) takes the Vulkan branch, and a distinct name so `ctest -R`,
        # dashboards and a human reading `ctest -N` can tell the two rows apart at a glance.
        if(_t IN_LIST AVER_CTEST_VULKAN_TARGETS)
          add_test(NAME ${_t}.vulkan COMMAND ${_t} vulkan)
          set_tests_properties(${_t}.vulkan PROPERTIES
            TIMEOUT 600
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            FAIL_REGULAR_EXPRESSION "FAIL  |FAILED ==="
            ENVIRONMENT "AVER_USER_DATA_DIR=${CMAKE_BINARY_DIR}/Testing/userdata/${_t}.vulkan"
            # SKIPPED IS ITS OWN STATE, and it has to be, or this row is decorative. Both listed
            # suites exit 77 when the backend they were asked for is not on the machine. Without
            # this property that exit code reads as a FAILURE; with the `return 0` these suites
            # used before it, it read as a PASS. Neither is true, and the pass version is the
            # dangerous one -- a green `.vulkan` row on a driverless runner is a row that tested
            # nothing while claiming it tested Vulkan, which is the same false confidence that let
            # LineMeshTest's Vulkan path go unexecuted for its whole existence and is the reason
            # this entire block exists. ctest prints these as "Skipped" and does not count them
            # against the run, so a developer without a Vulkan driver is told the truth and has no
            # reason to delete the row.
            SKIP_RETURN_CODE 77)
        endif()
      endif()
    endif()
  endforeach()
  get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_s IN LISTS _subs)
    aver_register_ctest_in("${_s}")
  endforeach()
endfunction()

if(AVER_BUILD_TESTS)
  aver_register_ctest_in("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# The module catalog the launcher reads (cmake/modules.catalog.json) must match the manifests it is
# generated from. Fix a failure with: python scripts/module-catalog.py --write
if(AVER_BUILD_TESTS)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ModuleCatalogCheck
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/module-catalog.py" --check)
    set_tests_properties(ModuleCatalogCheck PROPERTIES TIMEOUT 60)
  else()
    message(STATUS "Aver: no Python 3 found - ModuleCatalogCheck is not registered")
  endif()
endif()

# The node reference and the C# API index are generated, and the node palette is a C++ mirror of the C# tables.
# NodeCatalogCheck fails on any disagreement between a table row and its palette entry, a duplicate node key, a
# family with no test file, or a stale docs/nodes page; CsharpApiIndexCheck fails on a stale docs/api page.
# Fix a failure with: python scripts/node-catalog.py --write   /   python scripts/csharp-api-index.py --write
if(AVER_BUILD_TESTS)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME NodeCatalogCheck
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/node-catalog.py" --check)
    add_test(NAME CsharpApiIndexCheck
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/csharp-api-index.py" --check)
    set_tests_properties(NodeCatalogCheck CsharpApiIndexCheck PROPERTIES TIMEOUT 120)
  else()
    message(STATUS "Aver: no Python 3 found - NodeCatalogCheck and CsharpApiIndexCheck are not registered")
  endif()
endif()
