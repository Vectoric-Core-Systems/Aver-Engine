# Dear ImGui (MIT) — vendored (docking branch). Third-party: warnings silenced.
add_library(imgui STATIC
  imgui.cpp
  imgui_draw.cpp
  imgui_tables.cpp
  imgui_widgets.cpp
  backends/imgui_impl_win32.cpp
  backends/imgui_impl_dx12.cpp
)
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
