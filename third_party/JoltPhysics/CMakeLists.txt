# Jolt Physics, vendored. See README.md here for provenance and what was left out.
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
# CROSS_PLATFORM_DETERMINISTIC stays OFF on purpose. Jolt's guarantee is that the same BINARY given
# the same call order reproduces exactly, and that is the only guarantee the gates oracle needs --
# it re-runs this machine's build against a baseline recorded from it. The cross-platform mode costs
# performance to buy a property nothing here checks.
set(DOUBLE_PRECISION              OFF)
set(CROSS_PLATFORM_DETERMINISTIC  OFF)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)   # would arm FP exceptions in Debug/Release builds
set(ENABLE_OBJECT_STREAM          OFF)
set(USE_STD_VECTOR                OFF)

# Jolt's Compute module can build DX12/Vulkan compute shaders, which would invoke DXC at configure
# time and want the Vulkan SDK. The engine has its own RHI and does not use Jolt on the GPU.
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK   OFF)
set(JPH_USE_MTL  OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/Jolt/Jolt.cmake)

# Asserts in Debug only. Upstream's USE_ASSERTS is config-independent, which would carry the cost
# into Release; a generator expression gets the Debug diagnostics without paying for them shipped.
target_compile_definitions(Jolt PUBLIC "$<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>")

# Jolt asks for C++17; the engine is C++20 and they share headers, so pin the target rather than let
# Jolt.cmake's directory-scope CMAKE_CXX_STANDARD decide for anything else.
set_target_properties(Jolt PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "third_party")

# Vendored code is not ours to fix warnings in, and it is compiled far more often than it is read.
if (MSVC)
  target_compile_options(Jolt PRIVATE /W0)
else()
  target_compile_options(Jolt PRIVATE -w)
endif()
