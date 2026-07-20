aver_add_module(Aver.RHI
  SOURCES
    src/RHI.cpp
    src/RHIShaders.cpp
    src/null/NullDevice.cpp
  DEPS
    Aver.Core
    Aver.Platform
)

# Tell the factory which backend modules are compiled in (their symbols get linked
# by Aver.Runtime). Vulkan stays off until the SDK is installed.
if(AVER_RHI_D3D12)
  target_compile_definitions(Aver.RHI PUBLIC AVER_HAS_D3D12=1)
endif()
if(AVER_RHI_D3D11)
  target_compile_definitions(Aver.RHI PUBLIC AVER_HAS_D3D11=1)
endif()
if(AVER_RHI_VULKAN)
  target_compile_definitions(Aver.RHI PUBLIC AVER_HAS_VULKAN=1)
endif()
