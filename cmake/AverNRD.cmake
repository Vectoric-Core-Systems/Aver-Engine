# ---------------------------------------------------------------------------
# NVIDIA Real-Time Denoisers (NRD) — build wiring
#
# WHAT THIS FILE IS FOR
#
# third_party/nrd is upstream NRD, unmodified. Its own CMakeLists.txt is written for a self-build:
# it FetchContent-downloads ShaderMake and MathLib from GitHub, downloads a DXC release from GitHub
# on top of that, and writes its generated shader headers back into its own source tree. None of
# that is acceptable here -- this repository configures offline, vendors what it builds, and does
# not write into third_party/. This file makes NRD build under those rules WITHOUT patching it:
#
#   * ShaderMake and MathLib are vendored (third_party/shadermake, third_party/mathlib, both MIT)
#     and added BEFORE NRD. Upstream guards both fetches with `if(NOT TARGET ...)`, so defining the
#     targets first is the supported way to say "already have it" -- no download is attempted.
#   * SHADERMAKE_FIND_COMPILERS is OFF, so ShaderMake does not download DXC either. We locate a
#     compiler on this machine instead, and PROVE it works by compiling a shader with it at
#     configure time rather than assuming a found executable is the right one (the Windows SDK's
#     dxc.exe accepts -spirv and then fails at codegen -- see third_party/dxc-spirv/README.md).
#   * NRD_SHADERS_PATH points into the build tree, so the ~1000 generated *.cs.dxil.h headers land
#     in build/, not in the vendored source.
#
# WHAT IT DECIDES, AND WHY IT CAN DECIDE IT SILENTLY
#
# NRD needs an OFFLINE shader compiler; the engine's own HLSL is compiled at runtime through
# dxcompiler.dll and never needed one. A machine with no dxc.exe therefore cannot build NRD, and
# that must not be a build failure on a machine that was building fine yesterday. So AVER_WITH_NRD
# degrades: found and proven -> ON, absent or broken -> OFF with a STATUS line saying which. The
# engine builds and renders either way; the denoiser seam falls back to the hand-written filter.
# Set -DAVER_WITH_NRD=OFF to opt out deliberately, or -DAVER_NRD_REQUIRED=ON to turn the degrade
# into a hard error (what CI should do once NRD is on the critical path).
#
# LICENCE
#
# NRD is NOT permissively licensed. It ships under the NVIDIA RTX SDKs License, and this repository
# otherwise accepts MIT/BSD/Apache-2.0/zlib/CC0/CC-BY only. That exception was accepted by the owner
# on 2026-09-09 with the flow-through consequence stated; the reasoning is recorded in
# third_party/nrd/AVER_README.md and must be read before anyone relies on this in a shipped
# product. NRD_STATIC_LIBRARY is ON partly for that reason: the grant is to redistribute the SDK
# "as incorporated in object code format into a software application", and a static link is the
# shape that describes.
# ---------------------------------------------------------------------------

option(AVER_WITH_NRD "NVIDIA Real-Time Denoisers (third_party/nrd). NOT permissively licensed -- see third_party/nrd/AVER_README.md" ON)
option(AVER_NRD_REQUIRED "Fail configuration instead of degrading when NRD's prerequisites are missing" OFF)

if(NOT AVER_WITH_NRD)
  message(STATUS "Aver: NRD disabled (-DAVER_WITH_NRD=OFF)")
  return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nrd/CMakeLists.txt")
  message(STATUS "Aver: NRD disabled - third_party/nrd is not present")
  set(AVER_WITH_NRD OFF CACHE BOOL "" FORCE)
  return()
endif()

# --- Locate offline shader compilers -----------------------------------------------------------
#
# Two distinct compilers, deliberately. The Windows SDK's dxc.exe is built WITHOUT the SPIR-V
# backend, and the Vulkan SDK's is built without DXIL signing (dxil.dll sits beside the SDK one).
# Asking one binary to do both is how you get a DXIL blob the driver rejects, or a -spirv run that
# dies at codegen. So: SDK dxc for DXIL, Vulkan SDK dxc for SPIR-V, each probed for the thing it is
# actually being asked to emit.

get_filename_component(_aver_wsdk_bin "${CMAKE_RC_COMPILER}" DIRECTORY)

find_program(AVER_NRD_DXC_DXIL
  NAMES dxc
  HINTS "${_aver_wsdk_bin}" "${_aver_wsdk_bin}/../../x64" "$ENV{VULKAN_SDK}/Bin"
  DOC "dxc.exe used to compile NRD's shaders to DXIL")

find_program(AVER_NRD_DXC_SPIRV
  NAMES dxc
  HINTS "$ENV{VULKAN_SDK}/Bin" "${_aver_wsdk_bin}"
  DOC "dxc.exe used to compile NRD's shaders to SPIR-V (needs the SPIR-V backend)")

# Probe, do not assume. Costs ~100ms at configure and is the difference between "we found a file
# called dxc" and "that file emits the bytecode NRD is about to embed".
set(_aver_nrd_probe "${CMAKE_BINARY_DIR}/nrd/probe.cs.hlsl")
file(WRITE "${_aver_nrd_probe}"
  "RWBuffer<float> o : register(u0);\n"
  "[numthreads(1,1,1)] void main(uint3 t : SV_DispatchThreadID) { o[t.x] = 1.0; }\n")

function(aver_probe_dxc OUT_VAR COMPILER)
  set(${OUT_VAR} OFF PARENT_SCOPE)
  if(NOT COMPILER)
    return()
  endif()
  execute_process(
    COMMAND "${COMPILER}" -T cs_6_0 -E main ${ARGN} -Fo "${CMAKE_BINARY_DIR}/nrd/probe.bin" "${_aver_nrd_probe}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if(_rc EQUAL 0 AND EXISTS "${CMAKE_BINARY_DIR}/nrd/probe.bin")
    set(${OUT_VAR} ON PARENT_SCOPE)
  else()
    string(REPLACE "\n" " " _err "${_err}")
    message(STATUS "Aver: NRD shader probe failed for '${COMPILER}' ${ARGN}: ${_err}")
  endif()
  file(REMOVE "${CMAKE_BINARY_DIR}/nrd/probe.bin")
endfunction()

aver_probe_dxc(_aver_nrd_dxil_ok  "${AVER_NRD_DXC_DXIL}")
aver_probe_dxc(_aver_nrd_spirv_ok "${AVER_NRD_DXC_SPIRV}" -spirv)

# SPIR-V is only worth the (considerable) shader build time if a Vulkan backend exists to consume
# it. DXBC is never worth it: Shaders.cfg asks for shader model 6.0 throughout, which FXC cannot
# reach, and NRD is not being wired into the D3D11 backend.
if(NOT AVER_RHI_VULKAN)
  set(_aver_nrd_spirv_ok OFF)
endif()

if(NOT _aver_nrd_dxil_ok AND NOT _aver_nrd_spirv_ok)
  set(_msg "Aver: NRD disabled - no working offline shader compiler found. Install the Windows SDK (dxc.exe beside rc.exe) or the Vulkan SDK, or set -DAVER_NRD_DXC_DXIL=<path to dxc.exe>.")
  if(AVER_NRD_REQUIRED)
    message(FATAL_ERROR "${_msg}")
  endif()
  message(STATUS "${_msg}")
  set(AVER_WITH_NRD OFF CACHE BOOL "" FORCE)
  return()
endif()

# --- Feed the vendored dependencies to upstream's guards ---------------------------------------

set(SHADERMAKE_FIND_COMPILERS OFF CACHE BOOL "" FORCE)  # no GitHub downloads at configure time
set(SHADERMAKE_FIND_DXC       OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_DXC_VK    OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_SLANG     OFF CACHE BOOL "" FORCE)
# OFF => ShaderMake is a normal target in this build rather than a nested ExternalProject configure.
# One CMake configure, one Ninja graph, and NRD's `DEPENDS ShaderMake` still resolves.
set(SHADERMAKE_TOOL           OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_DXC_PATH    "${AVER_NRD_DXC_DXIL}"  CACHE INTERNAL "")
set(SHADERMAKE_DXC_VK_PATH "${AVER_NRD_DXC_SPIRV}" CACHE INTERNAL "")

set(NRD_STATIC_LIBRARY       ON  CACHE BOOL "" FORCE)
set(NRD_NRI                  OFF CACHE BOOL "" FORCE)  # we dispatch through Aver's own RHI
set(NRD_EMBEDS_DXIL_SHADERS  ${_aver_nrd_dxil_ok}  CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS ${_aver_nrd_spirv_ok} CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS  OFF CACHE BOOL "" FORCE)
# Generated headers out of the vendored tree. (Shaders/NRDConfig.hlsli is still written into
# third_party/nrd/Shaders by upstream's file(WRITE) -- it has to be, the compile runs with that as
# its source dir -- so it is gitignored.)
set(NRD_SHADERS_PATH "${CMAKE_BINARY_DIR}/nrd/_Shaders" CACHE STRING "" FORCE)

# NRD does not set a C++ standard of its own, so without this it would be compiled as C++20 along
# with the rest of the tree. Upstream targets C++17; pin it rather than discover the difference in
# a warning-as-error deep inside a vendored header. Restored immediately afterwards -- this is an
# included file, so its directory scope IS the root's, and leaving 17 set here would silently
# downgrade every module added below the include().
set(_aver_saved_cxx_standard ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)

add_subdirectory(third_party/mathlib    EXCLUDE_FROM_ALL)
add_subdirectory(third_party/shadermake EXCLUDE_FROM_ALL)
add_subdirectory(third_party/nrd)

set(CMAKE_CXX_STANDARD ${_aver_saved_cxx_standard})

# Vendored code does not get to fail this repository's build over a warning its own maintainers
# have not seen yet. Both trees compile themselves with warnings-as-errors under a compiler they
# pinned; ours is whatever the developer has. The flag is REMOVED rather than countermanded with a
# trailing /WX- -- appending the opposite works, but MSVC then prints D9025 for every translation
# unit in both libraries, which is a lot of noise to buy nothing. Warning LEVEL is left alone.
foreach(_t ShaderMake ShaderMakeBlob NRD)
  if(TARGET ${_t})
    get_target_property(_opts ${_t} COMPILE_OPTIONS)
    if(_opts)
      list(REMOVE_ITEM _opts /WX -Werror)
      set_target_properties(${_t} PROPERTIES COMPILE_OPTIONS "${_opts}")
    endif()
  endif()
endforeach()

foreach(_t MathLib ShaderMake ShaderMakeBlob NRD NRDIntegration NRDShaders)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES FOLDER "third_party/NRD")
  endif()
endforeach()

message(STATUS "Aver: NRD ON (DXIL=${_aver_nrd_dxil_ok} SPIRV=${_aver_nrd_spirv_ok}) - NVIDIA RTX SDKs License, see third_party/nrd/AVER_README.md")
