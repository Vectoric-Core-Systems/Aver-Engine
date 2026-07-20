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
add_subdirectory(modules/formats)  # Phase 2 (.ocbeam / .ocmap loaders)
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
if(AVER_MODULE_VOXI)
  add_subdirectory(modules/render.voxi)   # optional: render feature settings + C ABI for C#
endif()
add_subdirectory(modules/runtime)

if(AVER_BUILD_SANDBOX)
  add_subdirectory(sandbox)
endif()

if(AVER_BUILD_TESTS)
  add_subdirectory(tests/formats)
endif()
