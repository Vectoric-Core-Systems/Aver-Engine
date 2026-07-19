aver_add_module(Aver.RHI.Vulkan
  SOURCES
    src/VulkanDevice.cpp
  DEPS
    Aver.RHI
)

# Only added to the build when AVER_RHI_VULKAN=ON (needs the LunarG Vulkan SDK).
# Phase 3+: find_package(Vulkan REQUIRED); target_link_libraries(... Vulkan::Vulkan).
