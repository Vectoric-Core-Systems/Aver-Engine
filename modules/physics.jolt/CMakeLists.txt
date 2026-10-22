# Aver.Physics.Jolt — the rigid-body backend behind Aver.Physics's plain-C ABI.
#
# It lives under modules/ and is named the way every other backend in this tree is named:
# rhi + rhi.d3d12, audio + audio.wasapi, formats + formats.roslyn, physics + physics.jolt. The
# generic module states the contract; the dotted one is the concrete thing that satisfies it. Jolt
# is to Aver.Physics what WASAPI is to the mixer, and the directory now says so.
#
# The sources are still VENDORED and still not ours to edit -- moving them did not make them ours.
# README.md here keeps the whole provenance apparatus (version, upstream, archive SHA-256, licence,
# what was left out, how to update), the LICENSE file beside it is what scripts/stage-payload.ps1
# concatenates into THIRD-PARTY-NOTICES.txt, and the /W0 at the bottom is unchanged. A local fix in
# Jolt/ is still invisible at the next update.
#
# This file exists because upstream's option declarations live in Jolt's own Build/CMakeLists.txt,
# which is NOT vendored -- only the Jolt/ library directory is. Jolt/Jolt.cmake is self-contained
# otherwise: it lists the sources, defines EMIT_X86_INSTRUCTION_SET_DEFINITIONS, creates the target
# and sets its include directories and JPH_* definitions. So the job here is to declare the handful
# of variables it reads, then include it.
#
# Worth knowing: upstream's Build/CMakeLists.txt also sets warnings-as-errors and disables C++
# exceptions and RTTI. Those are NOT in Jolt.cmake, so none of it reaches this build -- which is what
# lets the engine keep using try/catch around std::filesystem while Jolt is linked in.

set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(JPH_LIB_TYPE STATIC)

# --- Instruction set ------------------------------------------------------------------------------
# SSE4.2 baseline, no AVX and no FMADD. Two reasons, both deliberate:
#
#  * AVX/AVX2 would raise the minimum CPU to Haswell (2013) for the whole engine. Jolt defaults them
#    ON; a physics library is not the right place for that decision to get made by default.
#  * FMADD fuses a multiply and an add into one rounding step, which CHANGES RESULTS. This repo
#    verifies rendering against a pixel-exact baseline, so a flag that quietly alters float
#    arithmetic is a bad trade for a few percent. Upstream disables FMADD under
#    CROSS_PLATFORM_DETERMINISTIC for the same reason.
set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_AVX    OFF)
set(USE_AVX2   OFF)
set(USE_AVX512 OFF)
set(USE_LZCNT  ON)
set(USE_TZCNT  ON)
set(USE_F16C   ON)
set(USE_FMADD  OFF)

# --- Features -------------------------------------------------------------------------------------
# Everything not set here is left undefined, which CMake treats as false -- so single precision, no
# ObjectStream (the engine serialises through .ocmap, not Jolt's own format), no stats tracking, and
# no cross-platform determinism.
#
# CROSS_PLATFORM_DETERMINISTIC stays OFF by default, on purpose. Jolt's guarantee is that the same BINARY given
# the same call order reproduces exactly, and that is the only guarantee the gates oracle needs --
# it re-runs this machine's build against a baseline recorded from it. The cross-platform mode costs
# performance to buy a property nothing here checks. AVER_PHYSICS_CROSS_PLATFORM (below) turns it on for the
# determinism CI job and for anyone who wants to test the claim; the claim itself is not made (see MODULES.md).
set(DOUBLE_PRECISION              OFF)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)   # would arm FP exceptions in Debug/Release builds
set(ENABLE_OBJECT_STREAM          OFF)
set(USE_STD_VECTOR                OFF)

# --- WORLD_STATE_DEBUG options (docs/plans/wave11/WORLD_STATE_DEBUG.md, 2.1) ---------------------------------
# Each maps onto a variable Jolt.cmake already reads, and each ends up as a PUBLIC compile definition on the Jolt
# target, so Aver.Physics and the one test that links Jolt directly see the same class layouts (Jolt checks the
# configuration string at RegisterTypes: a mismatch is a loud failure, not a silent one). Both apply to Debug and
# Release (Jolt.cmake's *_IN_DEBUG_AND_RELEASE); aver_phys_build_flags() reports what a binary has.
#
#  AVER_PHYSICS_DEBUG_RENDERER  JPH_DEBUG_RENDERER: the Jolt debug renderer behind the editor's Show > Physics overlay.
#                               Binary size only; there is no run cost until a draw call. With it OFF the debug ABI
#                               answers aver_phys_debug_available() == 0 and everything else is unchanged.
#  AVER_PHYSICS_PROFILER        JPH_EXTERNAL_PROFILE plus AverProfileHook.cpp: every Jolt scope and the engine's fixed-step
#                               stages feed aver_phys_profile_*. Off at run time until enabled (a null sink is one relaxed
#                               load and a branch per scope). Mutually exclusive with Jolt's own HTML profiler, which is a
#                               diagnostics-preset job and not a default.
#  AVER_PHYSICS_CROSS_PLATFORM  CROSS_PLATFORM_DETERMINISTIC: Jolt's mode for bit-identical results across compilers and CPUs
#                               (no FMA, IEEE-compliant maths; slower). DEFAULT OFF AND NO CLAIM: the engine's own float code in
#                               the fixed step (the vehicle driver aids) must be bit-stable across the two compilers too, so the
#                               claim is made only when tests/physics PhysicsDeterminismTest's golden hash is green on Windows and
#                               macOS with this on (the CI job "physics-determinism"). Also adds -ffp-contract=off outside MSVC,
#                               PUBLIC, so Aver.Physics's own float arithmetic is not fused either.
#  AVER_PHYSICS_DIAGNOSTICS     The developer preset, never CI and never the default: JPH_ENABLE_DETERMINISM_LOG (detlog.txt; diff two
#                               runs for the first diverging VALUE), the three stats-tracking defines (aver_phys_diag_report prints
#                               them to the engine log), and Jolt's INTERNAL profiler (JPH_PROFILE_ENABLED, HTML dumps) in place of
#                               the external hook, which the two cannot share. aver_phys_build_flags() reports DETERMINISM_LOG and
#                               STATS_TRACKING; the profiler panel then answers "no hook".
option(AVER_PHYSICS_DEBUG_RENDERER "Compile Jolt's debug renderer (Debug and Release)" ON)
option(AVER_PHYSICS_PROFILER "Compile the external profiler hook (Debug and Release); off at run time until enabled" ON)
option(AVER_PHYSICS_CROSS_PLATFORM "Jolt's cross-platform deterministic mode (slower; makes no promise until the golden hash is green on two OSes)" OFF)
option(AVER_PHYSICS_DIAGNOSTICS "Developer preset: determinism log, stats tracking and Jolt's internal profiler (heavy; never CI)" OFF)
if (AVER_PHYSICS_DEBUG_RENDERER)
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ON)
endif()
if (AVER_PHYSICS_DIAGNOSTICS)
  # The internal profiler: Jolt.cmake defines JPH_PROFILE_ENABLED instead of JPH_EXTERNAL_PROFILE, and no hook is built.
  set(JPH_USE_EXTERNAL_PROFILE OFF)
  set(PROFILER_IN_DEBUG_AND_RELEASE ON)
  set(TRACK_BROADPHASE_STATS ON)
  set(TRACK_NARROWPHASE_STATS ON)
  set(JPH_TRACK_SIMULATION_STATS ON)
elseif (AVER_PHYSICS_PROFILER)
  set(JPH_USE_EXTERNAL_PROFILE ON)
  set(PROFILER_IN_DEBUG_AND_RELEASE ON)
endif()
if (AVER_PHYSICS_CROSS_PLATFORM)
  set(CROSS_PLATFORM_DETERMINISTIC ON)
else()
  set(CROSS_PLATFORM_DETERMINISTIC OFF)
endif()

# Jolt's Compute module can build DX12/Vulkan compute shaders, which would invoke DXC at configure
# time and want the Vulkan SDK. The engine has its own RHI and does not use Jolt on the GPU.
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK   OFF)
set(JPH_USE_MTL  OFF)

# Hair (wave 11, docs/plans/wave11/SOFTBODY_HAIR.md 2.10): Jolt's CPU compute backend is ON. It adds only Compute/CPU/* and Shaders/HairWrapper.cpp (no dxc, no
# Vulkan SDK) and runs Jolt Hair's fifteen kernels as C++ on the calling thread: the reference for tests/physics HairTest and the fallback when no GPU compute host is
# installed (aver_phys_hair_set_compute_host). The DX12 and Vulkan backends above stay OFF on purpose: Aver.Render.Hair runs the same kernels over the engine's RHI
# through that host table, one code path for D3D12 and Vulkan.
set(JPH_USE_CPU_COMPUTE ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/Jolt/Jolt.cmake)

# The profiler hook lives in the Jolt target, not in Aver.Physics: a statically linked Jolt leaves
# ExternalProfileMeasurement's constructor and destructor for the user to supply, and PhysicsTest links Jolt directly, so
# defined only in the DLL they would be unresolved symbols in that exe. The file compiles to the one sink variable and
# nothing else in a configuration without JPH_EXTERNAL_PROFILE.
if (AVER_PHYSICS_PROFILER AND NOT AVER_PHYSICS_DIAGNOSTICS)
  target_sources(Jolt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/AverProfileHook.cpp)
endif()

# The determinism log is a compile definition (DeterminismLog.h only has it as a commented-out #define). PUBLIC, like every
# other Jolt feature define, so Aver.Physics sees the same JPH_DET_LOG macros the Jolt sources do. The log is opened and closed by
# the diagnostics hooks in modules/physics/src/PhysicsDeterminism.cpp.
if (AVER_PHYSICS_DIAGNOSTICS)
  target_compile_definitions(Jolt PUBLIC JPH_ENABLE_DETERMINISM_LOG)
endif()

# Cross-platform determinism also wants plain IEEE arithmetic from the engine's own code that runs inside the fixed step. MSVC's
# default (/fp:precise) already does not contract; clang and gcc contract a*b+c into an FMA by default on targets that have one
# (Apple silicon), which the other platform would not do.
if (AVER_PHYSICS_CROSS_PLATFORM)
  if (MSVC)
    target_compile_options(Jolt PUBLIC /fp:precise)
  else()
    target_compile_options(Jolt PUBLIC -ffp-contract=off)
  endif()
endif()

# Asserts in Debug only. Upstream's USE_ASSERTS is config-independent, which would carry the cost
# into Release; a generator expression gets the Debug diagnostics without paying for them shipped.
target_compile_definitions(Jolt PUBLIC "$<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>")

# Jolt asks for C++17; the engine is C++20 and they share headers, so pin the target rather than let
# Jolt.cmake's directory-scope CMAKE_CXX_STANDARD decide for anything else.
set_target_properties(Jolt PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "modules")

# The target is created by the vendored Jolt.cmake and so is called `Jolt`, which is upstream's name
# and not this tree's. An ALIAS rather than a rename: renaming would mean editing a vendored file,
# and the next update would silently undo it. Consumers link Aver.Physics.Jolt, so a link line reads
# the same as every other module's and the vendored file stays untouched.
add_library(Aver.Physics.Jolt ALIAS Jolt)

# Vendored code is not ours to fix warnings in, and it is compiled far more often than it is read.
if (MSVC)
  target_compile_options(Jolt PRIVATE /W0)
else()
  target_compile_options(Jolt PRIVATE -w)
endif()
