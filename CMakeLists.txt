cmake_minimum_required(VERSION 3.24)
project(AverEngine VERSION 0.2.0 LANGUAGES C CXX RC)

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
option(AVER_BUILD_GAME "Build the standalone game runtime (AverGame.exe)" ON)
option(AVER_BUILD_TESTS "Build test executables" ON)
option(AVER_ENABLE_UI "In-window editor UI (Dear ImGui)" ON)

# --- Editions: a named module set the launcher can offer and install ---
#
# WHAT THIS IS FOR. The launcher already installs per-EDITION -- scripts/publish-release.ps1 takes
# -Edition (default 'standard'), AverDist packs it, AverLauncherCli installs to
# <root>\<edition>\<version>\bin -- and that path is exactly where a shipped engine reports itself
# from. The plumbing has existed and has only ever cut one edition. This is the missing half: a
# name on THIS side that says which modules an edition contains.
#
# WHY NOT RUNTIME MODULE SWAPPING. AVER_MODULE_* is compile-time. Five modules are already SHARED
# (Voxi, PBR, Scene, Framework, Physics), but a Sandbox.exe built with one calls into it through
# an import table, so deleting the DLL does not disable a feature -- it stops the process starting.
# Toggling modules inside ONE install needs runtime dispatch through every cross-module call, which
# is a different engine from the one the 18-row matrix in scripts/module-matrix.ps1 verifies. An
# edition is a build, and every edition is just another row that matrix can check.
#
# STANDARD IS TODAY'S DEFAULTS, EXACTLY. Not approximately: the value beside every module below is
# the value that was already there, so a tree that never names an edition configures identically to
# one from before this block existed. That is what makes this safe to land ahead of anyone using it.
set(AVER_EDITION "standard" CACHE STRING "Module preset an edition ships: standard | slim | full")
set_property(CACHE AVER_EDITION PROPERTY STRINGS standard slim full)

# ON for every module the edition includes; the option() calls below take these as their DEFAULT,
# so an explicit -DAVER_MODULE_X=OFF on the command line still wins over the edition.
# One command per line: CMake has no statement separator, so two set()s on one line is a parse
# error, not a style question.
macro(_aver_edition_defaults voxi pbr scripting scene framework physics landscape deform trifactor sr mcp upgrade)
  set(_ed_VOXI      ${voxi})
  set(_ed_PBR       ${pbr})
  set(_ed_SCRIPTING ${scripting})
  set(_ed_SCENE     ${scene})
  set(_ed_FRAMEWORK ${framework})
  set(_ed_PHYSICS   ${physics})
  set(_ed_LANDSCAPE ${landscape})
  set(_ed_DEFORM    ${deform})
  set(_ed_TRIFACTOR ${trifactor})
  set(_ed_SR        ${sr})
  set(_ed_MCP       ${mcp})
  set(_ed_UPGRADE   ${upgrade})
endmacro()

if(AVER_EDITION STREQUAL "slim")
  # The smallest thing that still runs a game: scene, gameplay, materials, physics, terrain.
  # No voxel GI, no geometry cooking, no sub-bone deformation, no upscaler.
  _aver_edition_defaults(OFF ON ON ON ON ON ON OFF OFF OFF OFF ON)
elseif(AVER_EDITION STREQUAL "full")
  # Everything, including the editor control channel that standard deliberately leaves out.
  _aver_edition_defaults(ON ON ON ON ON ON ON ON ON ON ON ON)
elseif(AVER_EDITION STREQUAL "standard")
  _aver_edition_defaults(ON ON ON ON ON ON ON ON ON ON OFF ON)
else()
  message(FATAL_ERROR "AVER_EDITION '${AVER_EDITION}' is not a known edition (standard, slim, full)")
endif()

# AN EDITION CHANGE HAS TO RE-DERIVE, and this is the trap that makes it worth the code. option()
# never overwrites a cache entry that already exists, so reconfiguring an existing tree with a
# different -DAVER_EDITION would leave every module exactly as the previous edition set it and the
# build would silently be the old edition under the new name. Only a CHANGE forces the update, so a
# deliberate -DAVER_MODULE_X=OFF on a later configure of the SAME edition is still respected.
if(DEFINED AVER_EDITION_APPLIED AND NOT AVER_EDITION_APPLIED STREQUAL AVER_EDITION)
  message(STATUS "Aver: edition ${AVER_EDITION_APPLIED} -> ${AVER_EDITION}, re-deriving module set")
  foreach(m VOXI PBR SCRIPTING SCENE FRAMEWORK PHYSICS LANDSCAPE DEFORM TRIFACTOR SR MCP UPGRADE)
    set(AVER_MODULE_${m} ${_ed_${m}} CACHE BOOL "" FORCE)
  endforeach()
endif()
set(AVER_EDITION_APPLIED "${AVER_EDITION}" CACHE INTERNAL "the edition the cached module set came from")

# --- Optional feature modules (the engine builds and runs with these OFF) ---
option(AVER_MODULE_VOXI "Voxi render module: AA / GI / ray tracing / path tracing settings" ${_ed_VOXI})
option(AVER_MODULE_PBR  "PBR material system: material library, C ABI and the surface BRDF" ${_ed_PBR})
option(AVER_MODULE_SCRIPTING "In-process .NET scripting host (hostfxr/CoreCLR)" ${_ed_SCRIPTING})
option(AVER_MODULE_SCENE "Entity/component world: entities, transforms, hierarchy and the C ABI" ${_ed_SCENE})
option(AVER_MODULE_FRAMEWORK "Gameplay framework: game instance, game mode, actors, pawns" ${_ed_FRAMEWORK})
# Physics is declared HERE, beside its peers, rather than down in the third-party section where it
# used to sit. Jolt is the engine's rigid-body backend, not a dependency the build happens to pull
# in, and the switch that turns physics off is the same kind of switch as the five above it. Still
# optional for the same two reasons as before: the engine configures and runs without it, and 153
# translation units are worth being able to opt out of.
option(AVER_MODULE_PHYSICS "Physics module: rigid bodies, characters and raycasts (Jolt backend)" ${_ed_PHYSICS})
# Landscape: the chunked heightfield terrain. CPU-only at this stage -- the quadtree, the LOD metric,
# culling and chunk geometry -- so it depends on nothing optional and costs nothing to leave on.
option(AVER_MODULE_LANDSCAPE "Landscape module: chunked heightfield terrain, LOD and chunk geometry" ${_ed_LANDSCAPE})
option(AVER_MODULE_DEFORM "Sub-bone vertex deformation: a cage finer than the skeleton" ${_ed_DEFORM})
# Trifactor: the virtualized-geometry cluster builder + LOD DAG (docs/VIRTUALIZED_GEOMETRY.md).
# ON, like its siblings above. It was briefly OFF because meshoptimizer was not vendored and every
# default build would have failed at configure; third_party/meshoptimizer (v1.2, MIT) is now here,
# so that reason is gone. Left ON deliberately rather than merely permitted: it is CPU-only, so it
# costs nothing at runtime for a game that never cooks, and being in the default build is what makes
# TrifactorTest part of the default suite instead of something that silently rots.
option(AVER_MODULE_TRIFACTOR "Virtualized-geometry cluster builder and LOD DAG (meshlets, LOD DAG)" ${_ed_TRIFACTOR})
# AverSR: the upscaler seam's implementations (docs/AVERSR.md). ON like its render-module siblings
# above -- it links only Aver.Core and Aver.RHI, so it costs nothing to leave in a build that never
# registers an IUpscaler, and -DAVER_MODULE_SR=OFF must still build, link and render at native
# resolution, pixel-identical to a tree where this module was never written (see the doc for why
# that is the actual test, not the option existing).
option(AVER_MODULE_SR "AverSR upscaler implementations: the built-in spatial resample, and the seam FSR vendors into" ${_ed_SR})
# The editor control channel, so a tool can press the editor's own buttons and then look at the result.
# OFF BY DEFAULT: it is a debugging aid, the editor must work without it, and nobody should ship an
# editor that can be driven over a socket unless they asked for it. Even built, it listens only when the
# app is told to with --mcp.
option(AVER_MODULE_MCP "Editor control channel over loopback (debugging aid, not needed to run)" ${_ed_MCP})
# Upgrade: the project migration chain. ON in every edition, including slim -- an engine that
# cannot open a project made by an older build of itself is not a smaller engine, it is a broken
# one. Optional only so the matrix can prove the editor still builds and runs without it.
option(AVER_MODULE_UPGRADE "Project migration chain: upgrade a project from the version it was made in" ${_ed_UPGRADE})
# Occlusion: hierarchical-Z occlusion culling (modules/occlusion). NOT threaded through the edition
# macro above like its siblings: it is a generic RHI utility with no gameplay, content or C# ABI
# dependency of its own -- closer to AverSR (links only Core and RHI) than to a curated feature like
# Voxi or Physics -- so there is no per-edition judgement call to make about it, and ON is simply the
# right default for every edition at once. The runtime behaviour stays opt-in regardless of this
# switch: the module builds by default, but --occlusion-cull (sandbox) defaults OFF, exactly like
# --depth-prepass and --edge-aa (see modules/occlusion/README.md).
option(AVER_MODULE_OCCLUSION "Occlusion module: hierarchical-Z pyramid + two-pass box culling" ON)
# Particles: CPU-simulated billboard particles, depth-tested against the scene through
# IRenderFeature::transparentPass (see modules/particles/README.md). Optional for the same reason
# Occlusion is: the engine builds, links and renders with it off. It DOES depend on the scene
# (scene::CParticleEmitter drives it), unlike Occlusion/AverSR, which is why the force-off guard
# beside AVER_MODULE_FRAMEWORK's exists just below -- see it for what happens with the scene off.
option(AVER_MODULE_PARTICLES "Particle system: CPU-simulated billboard particles, depth-tested against the scene" ON)

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
# meshoptimizer (MIT), the meshlet/simplify backbone RENDERING.md:42 and FORMAT_SPECS.md:242 already
# named. Added only when something wants it -- it is a static library nobody else links, and a build
# with Trifactor off should not pay to compile twenty translation units it will never call.
if(AVER_MODULE_TRIFACTOR)
  add_subdirectory(third_party/meshoptimizer)
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
# Animation sampling, above formats because it reads .ocskel/.ocanim through it and below anything
# that draws. Posing a skeleton is arithmetic, so this needs no RHI and never will.
add_subdirectory(modules/anim)
# Sub-bone vertex deformation, above anim because a cage is generated against a skeleton's rest
# pose. Optional: a game that only needs bone skinning does not link it.
if(AVER_MODULE_DEFORM)
  add_subdirectory(modules/deform)
endif()
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
# AverSR, beside Voxi for the same reason Deform sits beside Trifactor: it is optional and needs
# only Aver.Core and Aver.RHI, so where it is added relative to Voxi/PBR does not matter -- unlike
# Voxi it takes no dependency on PBR (see docs/AVERSR.md: an upscaler resamples a colour target and
# has no legitimate reason to know what a material is).
if(AVER_MODULE_SR)
  add_subdirectory(modules/render.sr)     # optional: AverSR upscaler implementations
endif()
# PCG's GPU half. Needs only the RHI, so it is unconditional like the UI: there is no combination of
# switches in which the RHI exists and a compute shader that writes a 3D texture does not.
add_subdirectory(modules/render.pcg)
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
# scene::CParticleEmitter is declared IN modules/scene (Builtins.cpp), but the module that TICKS and
# DRAWS it links Aver.Scene, not the other way round -- with the scene off there is no component for
# it to read, exactly the shape AVER_MODULE_FRAMEWORK's guard above already has.
if(AVER_MODULE_PARTICLES AND NOT AVER_MODULE_SCENE)
  message(STATUS "Aver: particles are driven by a scene component and cannot be built without the scene - forcing AVER_MODULE_PARTICLES=OFF")
  set(AVER_MODULE_PARTICLES OFF)
endif()
if(AVER_MODULE_SCENE)
  add_subdirectory(modules/scene)         # optional: the entity/component world + C ABI for C#
  # The scene/animation join. Guarded on BOTH, because it is the edge between them and neither owns
  # it; a build with either switched off simply does not have the join.
  add_subdirectory(modules/anim.scene)
endif()
if(AVER_MODULE_FRAMEWORK)
  add_subdirectory(modules/framework)     # optional: gameplay vocabulary over the world
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
# The path tracer and its white-furnace oracle. Unconditional and RHI-only, like the two above: it
# owns its own geometry and scenes, so there is no world, material system or render feature it could
# depend on, and it declines at init() on a device without DXR 1.1.
add_subdirectory(modules/render.pt)
# The audio seam. Added down HERE rather than beside the mixer because it links the WAV reader,
# which is a target inside modules/formats and so does not exist until that has been added.
add_subdirectory(modules/audio.abi)
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
# After modules/runtime: it links Aver.Runtime, and this is a plain subdirectory order dependency.
if(AVER_BUILD_GAME)
  add_subdirectory(modules/runtime.game)
endif()

if(AVER_BUILD_SANDBOX)
  add_subdirectory(sandbox)
endif()

# The standalone game runtime. Separate from the sandbox switch because they are opposites: one is
# the editor, one is what a shipped game runs, and a tree may reasonably want either alone.
#
# NOTE FOR PACKAGING, UPDATED BY THE IMGUI/RHI SPLIT: an AverGame.exe built from a tree with
# AVER_ENABLE_UI=ON no longer has ImGui linked into it. Aver.RHI.D3D12 never links imgui (see its own
# CMakeLists.txt and include/aver/rhi/d3d12/UiBackend.hpp); the concrete Dear ImGui implementation
# lives in the separate Aver.RHI.D3D12.ImGui module, which only Sandbox links regardless of this
# switch. scripts/stage-game.ps1 now proves this against the actual staged binary rather than
# refusing the whole configuration on principle -- see its own comment.
if(AVER_BUILD_GAME)
  add_subdirectory(game)
endif()

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

if(AVER_BUILD_TESTS)
  # Needs Aver.Runtime.Game, so it only exists when the game runtime is built.
  if(AVER_BUILD_GAME AND TARGET Aver.Framework)
    add_subdirectory(tests/game)
  endif()
  add_subdirectory(tests/pcg)
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
  endif()
  if(AVER_MODULE_SCENE)
    add_subdirectory(tests/scene)
    add_subdirectory(tests/anim.scene)
    # The placement -> entity loop the editor and the game runtime now share. Guarded on the scene
    # for the same reason the module's own source is: without it there is nothing to instantiate into.
    add_subdirectory(tests/world)
  endif()
  if(AVER_MODULE_FRAMEWORK)
    add_subdirectory(tests/framework)
  endif()
  if(AVER_MODULE_PARTICLES)
    # The CPU simulation only -- spawn rate, lifetime, gravity/damping integration. Guarded by the
    # module because it links Aver.Particles/Aver.Scene, CPU-only because what it decides (a particle
    # count and a position after N seconds) is arithmetic no screenshot needs to settle first, the
    # same reasoning tests/occlusion and tests/trifactor give for their own CPU-only suites.
    add_subdirectory(tests/particles)
  endif()
endif()
