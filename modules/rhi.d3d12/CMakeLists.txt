aver_add_module(Aver.RHI.D3D12
  SOURCES
    src/D3D12Device.cpp
  DEPS
    Aver.RHI
)

# Windows SDK libraries (headers already present on this machine). d3dcompiler for
# runtime HLSL->DXBC (SM5.1) compilation; DXC/SM6 is a later upgrade.
target_link_libraries(Aver.RHI.D3D12 PUBLIC d3d12 dxgi dxguid d3dcompiler)

# In-window UI: this backend hosts Dear ImGui. PUBLIC so the include dirs + the
# AVER_WITH_IMGUI define propagate to the sandbox (which builds the editor widgets).
if(AVER_ENABLE_UI)
  target_link_libraries(Aver.RHI.D3D12 PUBLIC imgui)
  target_compile_definitions(Aver.RHI.D3D12 PUBLIC AVER_WITH_IMGUI=1)
endif()

# DXC redistributables. dxcompiler.dll (compiler) and dxil.dll (signing - drivers reject unsigned
# DXIL) ship with the Windows SDK but NOT with Windows, so copy them beside the executable.
# Missing them is not fatal: D3D12Device falls back to FXC/SM5.1.
get_filename_component(_wsdk_bin "${CMAKE_RC_COMPILER}" DIRECTORY)
find_file(AVER_DXCOMPILER dxcompiler.dll HINTS "${_wsdk_bin}" "${_wsdk_bin}/../../x64")
find_file(AVER_DXIL       dxil.dll       HINTS "${_wsdk_bin}" "${_wsdk_bin}/../../x64")
if(AVER_DXCOMPILER AND AVER_DXIL)
  file(COPY ${AVER_DXCOMPILER} ${AVER_DXIL} DESTINATION ${CMAKE_BINARY_DIR}/bin)
  message(STATUS "Aver: DXC redistributables copied to bin/")
else()
  message(WARNING "Aver: dxcompiler.dll/dxil.dll not found - shaders fall back to FXC (SM 5.1)")
endif()
