# Aver.Render.UI — the GPU half of the retained UI.
#
# The split from Aver.UI is the same one Aver.Render.Voxi draws between its settings DLL and its
# renderer: the widget tree produces a draw list and knows nothing about a device; this turns one
# into draw calls and knows nothing about widgets. Aver.UI is what a game links to build a HUD;
# this is what an application links to put one on screen.
#
# Aver.RHI is the GENERIC interface. Never Aver.RHI.D3D12 — no backend type may cross this
# boundary, and a link line is the only place that rule can actually be enforced.
aver_add_module(Aver.Render.UI
  SOURCES
    src/UiRenderer.cpp
  DEPS
    Aver.Core
    Aver.UI
    Aver.RHI
)

# HLSL beside the executable, redeployed when a shader changes -- the same stamp mechanism
# modules/rhi, render.pbr and render.voxi already use. See cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Render.UI "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
