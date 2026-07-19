aver_add_module(Aver.RHI.D3D12
  SOURCES
    src/D3D12Device.cpp
  DEPS
    Aver.RHI
)

# Windows SDK libraries (headers already present on this machine). d3dcompiler for
# runtime HLSL->DXBC (SM5.1) compilation; DXC/SM6 is a later upgrade.
target_link_libraries(Aver.RHI.D3D12 PUBLIC d3d12 dxgi dxguid d3dcompiler)
