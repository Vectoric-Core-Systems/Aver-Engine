# Dear ImGui (MIT) — vendored (docking branch). Third-party: warnings silenced.
#
# THE RENDERER BACKENDS ARE OPT-IN PER RHI, and imgui_impl_vulkan.cpp is compiled only when
# AVER_RHI_VULKAN is on. It is not free to build it unconditionally: it includes <vulkan/vulkan.h>,
# which lives in third_party/vulkan-headers and would otherwise become an include-path dependency of
# every tree, including ones with no Vulkan backend at all. The Win32 PLATFORM backend has no such
# condition -- both renderers need it, and it is the only platform backend vendored.
add_library(imgui STATIC
  imgui.cpp
  imgui_draw.cpp
  imgui_tables.cpp
  imgui_widgets.cpp
  backends/imgui_impl_win32.cpp
  backends/imgui_impl_dx12.cpp
)
if(AVER_RHI_VULKAN)
  target_sources(imgui PRIVATE backends/imgui_impl_vulkan.cpp)
  # The SAME vendored headers modules/rhi.vulkan builds against, and for the same reason it does:
  # there is no Vulkan SDK on this machine and none is required. PRIVATE, not PUBLIC -- a consumer
  # of `imgui` gets ImGui's own headers, never vulkan.h.
  target_include_directories(imgui PRIVATE ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
  # imgui_impl_vulkan resolves every entry point through a loader function rather than linking
  # vulkan-1.lib, which is what lets this build with no SDK present -- the same
  # LoadLibraryW/GetProcAddress arrangement VulkanCommon.hpp documents. The backend must be told to
  # use it, and ImGui_ImplVulkan_LoadFunctions must be called before Init.
  target_compile_definitions(imgui PRIVATE VK_NO_PROTOTYPES)
endif()
target_include_directories(imgui PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/backends
)
if(MSVC)
  target_compile_options(imgui PRIVATE /w)
endif()
if(WIN32)
  # d3d12/dxgi for the DX12 backend; dwmapi + imm32 for the Win32 backend.
  target_link_libraries(imgui PUBLIC d3d12 dxgi dxguid dwmapi imm32)
endif()
set_target_properties(imgui PROPERTIES FOLDER "third_party")
