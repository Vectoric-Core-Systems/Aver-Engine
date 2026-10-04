aver_add_module(Aver.Runtime
  SOURCES
    src/Engine.cpp
  DEPS
    Aver.Core
    Aver.Platform
    Aver.RHI
)

# Runtime is the composition root: it links whichever backends are compiled in so
# the RHI factory's symbols resolve at final link.
if(AVER_RHI_D3D12)
  target_link_libraries(Aver.Runtime PUBLIC Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(Aver.Runtime PUBLIC Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(Aver.Runtime PUBLIC Aver.RHI.Vulkan)
endif()
