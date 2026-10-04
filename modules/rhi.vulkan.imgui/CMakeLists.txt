# The concrete Dear ImGui implementation of Aver.RHI.Vulkan's IUiBackend seam (see that module's own
# include/aver/rhi/vulkan/UiBackend.hpp). A SEPARATE module for exactly the reason its D3D12 twin is
# one: only Sandbox links this, so nothing that merely links Aver.RHI.Vulkan gets Dear ImGui
# compiled into it.
aver_add_module(Aver.RHI.Vulkan.ImGui
  SOURCES
    src/ImGuiVulkanUiBackend.cpp
  DEPS
    Aver.RHI.Vulkan
)

# PRIVATE, not PUBLIC -- nothing linking this module inherits imgui's include dirs or symbols
# through it. Sandbox links `imgui` a second time, directly, for its own ~90 raw ImGui:: call sites.
target_link_libraries(Aver.RHI.Vulkan.ImGui PRIVATE imgui)

# The vendored Vulkan headers, PRIVATE, and the same VK_NO_PROTOTYPES the RHI and the ImGui backend
# are both built with: there is no Vulkan SDK on this machine and none is required. Every entry
# point is resolved at runtime through ImGui_ImplVulkan_LoadFunctions -- see this module's source.
target_include_directories(Aver.RHI.Vulkan.ImGui PRIVATE ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
target_compile_definitions(Aver.RHI.Vulkan.ImGui PRIVATE VK_NO_PROTOTYPES)
