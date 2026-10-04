aver_add_module(Aver.RHI
  SOURCES
    src/RHI.cpp
    src/RHIShaders.cpp
    src/ShaderFiles.cpp
    src/Atmosphere.cpp
    src/EditorLines.cpp
    src/null/NullDevice.cpp
  DEPS
    Aver.Core
    Aver.Platform
)

# HLSL beside the executable, redeployed whenever a shader actually changes.
#
# NOT a configure-time file(COPY), and not POST_BUILD either. The comment that stood here argued for
# POST_BUILD over file(COPY) because a configure-time copy "means editing a shader and rebuilding
# gets you the OLD shader, silently" -- the right worry, aimed one step short of the target.
# POST_BUILD has the SAME failure: it runs when Aver.RHI RELINKS, and a .hlsl is not a source of
# Aver.RHI, so a shader-only edit left every target up to date and the copy never ran. Measured, not
# reasoned: material_prelude.hlsl in the source tree was 59,153 bytes while the deployed copy was
# 59,072 from six minutes earlier, and a deliberately broken shader term rendered a correct picture.
#
# aver_deploy_shaders() makes the .hlsl files real DEPENDS of a stamp, which is what gives the build
# graph a reason to rerun the copy. See its own comment in cmake/AvModule.cmake.
#
# bin/shaders is the ONLY place a shipped build looks (see ShaderFiles.hpp for why that default is
# not negotiable -- aver::platform's file trace exists so a package can prove it never read the tree
# that built it). Iterating without a rebuild is a separate, opt-in path: --shader-source <dir>.
aver_deploy_shaders(Aver.RHI "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

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
