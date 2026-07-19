aver_add_module(Aver.RHI.D3D12
  SOURCES
    src/D3D12Device.cpp
  DEPS
    Aver.RHI
)

# Phase 3 will add: target_link_libraries(Aver.RHI.D3D12 PUBLIC d3d12 dxgi dxguid)
# The Windows SDK headers (d3d12.h) are already present on this machine.
