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
# own. Every configuration this engine currently builds keeps AVER_RHI_D3D12 ON alongside
# AVER_RHI_VULKAN (scripts/module-matrix.ps1's vulkan-on row layers Vulkan ON TOP of the default
# set rather than replacing it), so that copy step is always present when this module is. Flagged
# here so it is not a silent gap the day a Vulkan-only, D3D12-off configuration is asked for.
