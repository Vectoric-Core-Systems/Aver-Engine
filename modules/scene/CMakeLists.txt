# Aver.Scene — the entity/component world (see README.md).
#
# Built SHARED for the same reason Aver.Render.PBR is: the C# scripting layer P/Invokes this DLL,
# and keeping the world in ONE binary means the editor and the bindings address the same entities.
# Two copies of the world would each look correct in isolation, which is the worst shape a bug of
# this kind can take.
#
# Depends only on Aver.Core and Aver.Assets, so nothing of the RHI leaks into the DLL — the moment
# `aver/rhi/*` appears behind this target, the scripting boundary is broken.
#
# aver_add_module() is STATIC-only, so a plain add_library is used here.
add_library(Aver.Scene SHARED
  src/World.cpp
  src/ComponentPool.cpp
  src/Fields.cpp
  src/Builtins.cpp
  src/SceneAbi.cpp
  src/DecalAbi.cpp
  src/Scene2D.cpp     # 2D components and the 2D transform pass (docs/plans/RENDER_2D.md)
  src/Canvas2D.cpp    # the 2D canvas: sprites and external items sorted into the UI draw list
  src/Scene2DAbi.cpp) # scene2d_abi.h

target_include_directories(Aver.Scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Scene PUBLIC cxx_std_20)
target_link_libraries(Aver.Scene PUBLIC Aver.Core Aver.Assets)
# Canvas2D emits into the UI draw list. Aver.UI is Core-only (no RHI), so nothing of the renderer enters this DLL; PRIVATE keeps the
# UI headers off every Scene consumer (a consumer of Canvas2D.hpp links Aver.UI itself).
target_link_libraries(Aver.Scene PRIVATE Aver.UI)

# AVER_SCENE_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Scene PRIVATE AVER_SCENE_BUILD)
# Consumers get AVER_MODULE_SCENE so they can #if around the whole feature.
target_compile_definitions(Aver.Scene PUBLIC AVER_MODULE_SCENE=1)

set_target_properties(Aver.Scene PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe
