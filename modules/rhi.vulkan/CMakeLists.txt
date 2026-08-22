# src/VulkanShaderCompiler.cpp was once NAMED HERE WITHOUT EXISTING, so this module could not
# CONFIGURE at all -- CMake fails on a missing source before any compiler runs. Nothing caught it
# because scripts/module-matrix.ps1's vulkan-on row was the only configuration that would have. That
# TU is written now (see its own banner).
#
# src/VulkanPipeline.cpp IS DELIBERATELY NOT LISTED, and this is the more interesting half.
#
# It was added to this list at the same time, on the reasonable assumption that a file on disk in a
# module's src/ belongs in its build. It does not, because THE FILE SPLIT THAT CREATED IT WAS NEVER
# FINISHED: VulkanPipeline.cpp holds pipeline/descriptor-layout code moved out of
# VulkanResourceFactory.cpp, and the originals were never deleted. Every externally visible thing it
# defines -- tableSetLayout, getOrCreateSampler, descriptorLayout, createGraphicsPipeline,
# createComputePipeline and the free pushConstantLayout -- is DUPLICATED there, and all six
# DIFFER between the two copies.
#
# WHICH ONE WAS WINNING IS NOT A GUESS: with the module compiled, the linker says so itself --
#   "VulkanPipeline.cpp.obj : warning LNK4006: ... already defined in
#    VulkanResourceFactory.cpp.obj; second definition ignored"
# -- six times. VulkanResourceFactory.cpp's definitions link; VulkanPipeline.cpp's are discarded
# wholesale, so that translation unit contributed NOTHING to the binary while costing six warnings
# and offering a 686-line file in which someone could "fix" a function that never runs. Its
# anonymous-namespace helpers are internal and reachable from nowhere else.
#
# DROPPING IT FROM THE LIST THEREFORE CHANGES NO BEHAVIOUR WHATEVER -- proven by the linker's own
# message, not asserted -- and removes both the warnings and the trap. It is NOT the finished job:
# whoever completes the split should delete the six originals from VulkanResourceFactory.cpp and
# list this file again, which SWAPS six divergent implementations and must be verified on a real
# Vulkan GPU rather than by a build going green. That was not attempted here, because with the
# camera post chain still faulting the driver there is no way to tell a good swap from a bad one.
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

# No `vulkan-1.lib` exists anywhere on this machine (confirmed: that import library ships only with
# the SDK, which the README above deliberately keeps uninstalled -- there is no VULKAN_SDK
# environment variable and no .lib to be found). VulkanCommon.hpp therefore builds every TU in this
# module with VK_NO_PROTOTYPES: no Vulkan function is an extern symbol this target ever asks the
# linker to resolve. Every entry point -- starting from vkGetInstanceProcAddr itself -- is resolved
# at RUNTIME from vulkan-1.dll via LoadLibraryW + GetProcAddress (VulkanCommon.hpp's VulkanApi
# table, loaded by VulkanDevice::init()). So "link it by name" means naming the DLL to
# LoadLibraryW, not naming an import library to the linker -- there is nothing Vulkan-related to
# add to target_link_libraries below; Aver.RHI (above) is this module's only link dependency.
target_compile_definitions(Aver.RHI.Vulkan PRIVATE
  VK_NO_PROTOTYPES=1
  VK_USE_PLATFORM_WIN32_KHR=1
)

# In-window UI: NOT wired up in this pass. modules/rhi.d3d12 hosts Dear ImGui through
# imgui_impl_dx12 (see its own CMakeLists.txt, gated on AVER_ENABLE_UI). Vulkan's equivalent is the
# SEPARATE imgui_impl_vulkan backend -- a real, not-yet-vendored dependency (its own license check,
# its own render-target/command-buffer contract) rather than a recompile of the same code against a
# different header. Until it is vendored, VulkanDevice simply does not override IDevice's ui*
# methods, which default to false/no-op (RHI.hpp) -- an honest "no UI on this backend yet". Revisit
# alongside AVER_ENABLE_UI once imgui_impl_vulkan clears the same permissive-license bar as
# everything else under third_party/.

# DXC redistributables (dxcompiler.dll / dxil.dll) are copied beside the executable by
# modules/rhi.d3d12/CMakeLists.txt whenever that module is part of the build. VulkanShaderCompiler
# (src/VulkanShaderCompiler.cpp) loads that SAME dxcompiler.dll by name at runtime and asks it for
# SPIR-V instead of DXIL via the `-spirv` argument, so this module adds no new copy step of its
# own.
#
# ############################################################################################
# THAT LAST SENTENCE IS WRONG, AND IT IS THE CURRENT BLOCKER. Measured, not reasoned: running
# `Sandbox.exe --backend vulkan` gets all the way to a live VkDevice with caps queried (MSAA 8x,
# RT tier 11, SM 65, mesh-shader tier 1) and then dies on the first shader with
#
#     [RHI.Vulkan] VSMain (vs_6_0): SPIR-V CodeGen not available.
#                                    Please recompile with -DENABLE_SPIRV_CODEGEN=ON.
#
# The dxcompiler.dll being loaded comes from the WINDOWS SDK -- see modules/rhi.d3d12/
# CMakeLists.txt's `find_file(AVER_DXCOMPILER dxcompiler.dll HINTS "${_wsdk_bin}" ...)` -- and
# Microsoft builds their redistributable WITHOUT the SPIR-V backend compiled in. `-spirv` is
# accepted as an argument and then refused at codegen. Only the Khronos DirectXShaderCompiler
# GitHub releases and the LunarG Vulkan SDK ship a dxcompiler.dll built with
# ENABLE_SPIRV_CODEGEN=ON.
#
# So the premise this whole backend was designed on -- "the engine already has DXC, which emits
# SPIR-V with -spirv, so no glslang/shaderc either" -- does not hold for the DLL the engine
# actually has. Closing it needs ONE of:
#   (a) vendor a SPIR-V-capable dxcompiler.dll (Khronos DXC release; check its licence against
#       docs/ASSET_IMPORT.md's accepted list) and load THAT by an explicit path here rather than
#       whichever one the SDK put beside the exe;
#   (b) vendor glslang/shaderc after all, which this module's README deliberately ruled out for
#       being a second source of truth for the same HLSL;
#   (c) accept the LunarG SDK as a build dependency, which third_party/vulkan-headers/README.md
#       deliberately ruled out.
# (a) is the only one that keeps both earlier decisions intact. It is an external dependency
# fetch, so it is the user's call, not something to do silently.
# ############################################################################################ Every configuration this engine currently builds keeps AVER_RHI_D3D12 ON alongside
# AVER_RHI_VULKAN (scripts/module-matrix.ps1's vulkan-on row layers Vulkan ON TOP of the default
# set rather than replacing it), so that copy step is always present when this module is. Flagged
# here so it is not a silent gap the day a Vulkan-only, D3D12-off configuration is asked for.
