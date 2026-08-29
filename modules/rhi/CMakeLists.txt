aver_add_module(Aver.RHI
  SOURCES
    src/RHI.cpp
    src/RHIShaders.cpp
    src/ShaderFiles.cpp
    src/Atmosphere.cpp
    src/null/NullDevice.cpp
  DEPS
    Aver.Core
    Aver.Platform
)

# HLSL beside the executable, refreshed on every build.
#
# POST_BUILD, not a configure-time file(COPY), and the difference matters: a configure-time copy
# means editing a shader and rebuilding gets you the OLD shader, silently, until someone thinks to
# re-run cmake. The DXC redistributables above can be copied at configure time because they change
# when the SDK does, i.e. almost never; shader source changes every time anyone works on it.
#
# bin/shaders is the ONLY place a shipped build looks (see ShaderFiles.hpp for why that default is
# not negotiable -- aver::platform's file trace exists so a package can prove it never read the tree
# that built it). Iterating without a rebuild is a separate, opt-in path: --shader-source <dir>.
add_custom_command(TARGET Aver.RHI POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_BINARY_DIR}/bin/shaders"
  COMMENT "Aver.RHI: shaders -> bin/shaders")

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
