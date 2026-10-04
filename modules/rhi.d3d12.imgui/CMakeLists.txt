# The concrete Dear ImGui implementation of Aver.RHI.D3D12's IUiBackend seam (see that module's own
# include/aver/rhi/d3d12/UiBackend.hpp). A SEPARATE module, and the entire point of it existing: only
# Sandbox links this one, so AverEngineRuntime.exe -- which links Aver.RHI.D3D12 through Aver.Runtime in every
# AVER_ENABLE_UI=ON tree -- never sees Dear ImGui, no matter how AVER_ENABLE_UI is set.
aver_add_module(Aver.RHI.D3D12.ImGui
  SOURCES
    src/ImGuiUiBackend.cpp
  DEPS
    Aver.RHI.D3D12
)

# PRIVATE, not PUBLIC -- this is the actual fix. Nothing that links this module inherits imgui's
# include dirs or symbols through it. Sandbox links `imgui` a second time, directly and for its own
# reason (sandbox/CMakeLists.txt): the ~90 call sites across sandbox/src/*.cpp that build editor
# widgets with raw ImGui:: calls, which is a different concern from this module's one job of hosting
# ImGui's own DX12/Win32 render backend.
target_link_libraries(Aver.RHI.D3D12.ImGui PRIVATE imgui)
