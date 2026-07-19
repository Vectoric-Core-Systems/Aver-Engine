aver_add_module(Aver.RHI.D3D11
  SOURCES
    src/D3D11Device.cpp
  DEPS
    Aver.RHI
)

# Phase 3 will add: target_link_libraries(Aver.RHI.D3D11 PUBLIC d3d11 dxgi)
