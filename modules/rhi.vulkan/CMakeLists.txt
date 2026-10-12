# src/VulkanShaderCompiler.cpp was once NAMED HERE WITHOUT EXISTING, so this module could not
# CONFIGURE at all -- CMake fails on a missing source before any compiler runs. Nothing caught it
# because scripts/module-matrix.ps1's vulkan-on row was the only configuration that would have. That
# TU is written now (see its own banner).
#
# src/VulkanPipeline.cpp was DELETED: an unfinished split that was never in the build. It duplicated
# six functions of VulkanResourceFactory.cpp (tableSetLayout, getOrCreateSampler, descriptorLayout,
# createGraphicsPipeline, createComputePipeline, pushConstantLayout) and the linker discarded its copies
# (LNK4006), so removing it changed no behaviour. VulkanResourceFactory.cpp holds the live definitions.
aver_add_module(Aver.RHI.Vulkan
  SOURCES
    src/VulkanDevice.cpp
    src/VulkanShaderCompiler.cpp
    src/VulkanResourceFactory.cpp
    src/VulkanRenderContext.cpp
  DEPS
    Aver.RHI
)

# Vulkan headers only (Apache-2.0, vendored at third_party/vulkan-headers/include, v1.3.296) -- see
# third_party/vulkan-headers/README.md for why the LunarG SDK is deliberately NOT a dependency of
# this backend: headers are vendored here; the LOADER is already on every machine as
# C:\Windows\System32\vulkan-1.dll (it ships with the GPU driver, not the SDK); and the engine
# already has DXC, which emits SPIR-V with `-spirv`, so no glslang/shaderc either. This is a
# PRIVATE include -- nothing outside modules/rhi.vulkan/src/*.cpp (via VulkanCommon.hpp) may see a
# Vulkan type, matching modules/rhi.d3d12's own d3d12.h isolation (house rule 3).
target_include_directories(Aver.RHI.Vulkan PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
)
# libdxcompiler beside the executables. On Windows modules/rhi.d3d12 copies the vendored DLL; there
# is no D3D12 module anywhere else, so this one does it (cmake/AvDxc.cmake). The library is loaded at
# run time, so a machine without one still builds -- and reports "no shader compiler" when run.
if(NOT WIN32)
  include(AvDxc)
  aver_stage_dxc(Aver.RHI.Vulkan)
endif()

# No `vulkan-1.lib` exists anywhere on this machine (confirmed: that import library ships only with
# the SDK, which the README above deliberately keeps uninstalled -- there is no VULKAN_SDK
# environment variable and no .lib to be found). VulkanCommon.hpp therefore builds every TU in this
# module with VK_NO_PROTOTYPES: no Vulkan function is an extern symbol this target ever asks the
# linker to resolve. Every entry point -- starting from vkGetInstanceProcAddr itself -- is resolved
# at RUNTIME from vulkan-1.dll via LoadLibraryW + GetProcAddress (VulkanCommon.hpp's VulkanApi
# table, loaded by VulkanDevice::init()). So "link it by name" means naming the DLL to
# LoadLibraryW, not naming an import library to the linker -- there is nothing Vulkan-related to
# add to target_link_libraries below; Aver.RHI (above) is this module's only link dependency.
target_compile_definitions(Aver.RHI.Vulkan PRIVATE VK_NO_PROTOTYPES=1)
# The Win32 surface types are the only platform ones this module names. Off Windows the surface comes
# from Aver.Platform (createVulkanSurface in Window.hpp), so no X11, Wayland or Metal header is needed.
if(WIN32)
  target_compile_definitions(Aver.RHI.Vulkan PRIVATE VK_USE_PLATFORM_WIN32_KHR=1)
endif()

# In-window UI: DONE, and this comment said otherwise long after it stopped being true. It claimed
# imgui_impl_vulkan was "a real, not-yet-vendored dependency" and that "VulkanDevice simply does not
# override IDevice's ui* methods". Both are stale: modules/rhi.vulkan.imgui exists and is added by
# the root CMakeLists under AVER_RHI_VULKAN AND AVER_ENABLE_UI, and a `--backend vulkan` run logs
# "[RHI.Vulkan.ImGui] ImGui UI initialised (docking, dynamic rendering)" and draws the whole editor.
# Left as a correction rather than deleted: this file's other stale claim (SPIR-V codegen) cost the
# backend months of being dead, and the pattern is worth being able to see twice.

# DXC redistributables (dxcompiler.dll / dxil.dll) are copied beside the executable by
# modules/rhi.d3d12/CMakeLists.txt whenever that module is part of the build. VulkanShaderCompiler
# (src/VulkanShaderCompiler.cpp) loads that SAME dxcompiler.dll by name at runtime and asks it for
# SPIR-V instead of DXIL via the `-spirv` argument, so this module adds no new copy step of its
# own.
#
# THAT WAS WRONG FOR A LONG TIME, AND IS NOW FIXED -- the history is kept because the failure was
# invisible from the build and cost this backend its entire life. `Sandbox.exe --backend vulkan`
# reached a live VkDevice with caps queried (MSAA 8x, RT tier 11, SM 65, mesh-shader tier 1) and then
# died on the first shader with
#
#     [RHI.Vulkan] VSMain (vs_6_0): SPIR-V CodeGen not available.
#                                    Please recompile with -DENABLE_SPIRV_CODEGEN=ON.
#
# because the dxcompiler.dll beside the exe came from the WINDOWS SDK, and Microsoft build that
# redistributable WITHOUT the SPIR-V backend: `-spirv` is accepted as an argument and refused at
# codegen. The premise this backend was designed on -- "the engine already has DXC, which emits
# SPIR-V with -spirv" -- was false for the DLL the engine actually had.
#
# RESOLVED by option (a) of the three that were on the table: a SPIR-V-capable DXC is vendored at
# third_party/dxc-spirv (official Microsoft DXC v1.9.2607, MIT + LLVM licences, both checked in), and
# modules/rhi.d3d12/CMakeLists.txt's find_file HINTS it FIRST, ahead of the Windows SDK, so the DLL
# copied beside the executable emits both DXIL and SPIR-V. The two alternatives stay ruled out for
# their original reasons: glslang/shaderc would be a second source of truth for the same HLSL, and
# the LunarG SDK as a build dependency is what third_party/vulkan-headers/README.md exists to avoid.
#
# WHAT IS STILL NOT TRUE is that this backend is at parity. The cascade shadow map is not written at
# all (pushRenderScope discards the caller's render targets), and RT shadows mask it. Compiling and
# running are not the same as correct; see docs and the module README before trusting a picture.
#
# Every configuration this engine currently builds keeps AVER_RHI_D3D12 ON alongside
# AVER_RHI_VULKAN (scripts/module-matrix.ps1's vulkan-on row layers Vulkan ON TOP of the default
# set rather than replacing it), so that copy step is always present when this module is. Flagged
# here so it is not a silent gap the day a Vulkan-only, D3D12-off configuration is asked for.

# ARCHIVE ORDER, for the linkers that care about it. Aver.RHI's factory (RHI.cpp, tryBackend) calls
# createVulkanDevice, which lives HERE, while this module links Aver.RHI -- so a single pass over the
# archives meets this one first, keeps nothing from it, and leaves the factory's call unresolved.
# MSVC's linker resolves across every library on the line in any order and never noticed. GNU ld and
# lld do not, so the cycle is stated: CMake then repeats the pair on the link line. LINK_ONLY keeps it
# out of every consumer's include paths and definitions.
if(NOT MSVC)
  target_link_libraries(Aver.RHI INTERFACE $<LINK_ONLY:Aver.RHI.Vulkan>)
endif()
