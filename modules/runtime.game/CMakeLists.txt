# Aver.Runtime.Game -- the game half of what currently lives inside SandboxApp.
#
# Sandbox.exe IS the editor: it is the only aver::Application in the tree, and roughly two thousand
# lines of what a GAME needs are interleaved with dockspaces and inspector panels inside it. This
# library is where that half moves to, one subsystem per commit, so the editor keeps working while
# the game executable grows.
#
# It links NO UI. That is the whole point of the module existing, and it is currently true by
# construction rather than by enforcement: ImGui is linked PUBLIC from Aver.RHI.D3D12, so an
# AVER_ENABLE_UI=ON tree propagates it here regardless. The staging script is what will refuse to
# ship a binary built that way, until the ImGui/RHI split makes it structural.
aver_add_module(Aver.Runtime.Game
  SOURCES
    src/GameApp.cpp
  DEPS
    Aver.Runtime
    Aver.Platform
    Aver.Core
)
