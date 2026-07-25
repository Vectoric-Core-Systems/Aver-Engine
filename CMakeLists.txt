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
# Jolt Physics (MIT). Optional so the engine still configures without it -- and so the cost of
# compiling 153 translation units is opt-out for anyone who does not need physics.
option(AVER_MODULE_PHYSICS "Build the physics module (Jolt Physics)" ON)
if(AVER_MODULE_PHYSICS)
  add_subdirectory(third_party/JoltPhysics)
endif()

# ---- Tier 0-6 implemented modules ----
add_subdirectory(modules/core)
add_subdirectory(modules/platform)
add_subdirectory(modules/assets)   # Phase 2
add_subdirectory(modules/formats)  # Phase 2 (.ocbeam / .ocmap loaders)
if(AVER_MODULE_PHYSICS)
  add_subdirectory(modules/physics)   # Jolt behind the engine's plain-C ABI
endif()
add_subdirectory(modules/rhi)
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
add_subdirectory(modules/runtime)

if(AVER_BUILD_SANDBOX)
  add_subdirectory(sandbox)
endif()

if(AVER_BUILD_TESTS)
  add_subdirectory(tests/formats)
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
