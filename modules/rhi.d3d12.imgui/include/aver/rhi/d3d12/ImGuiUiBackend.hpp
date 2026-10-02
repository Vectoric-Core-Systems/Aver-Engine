// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The ONE public entry point of Aver.RHI.D3D12.ImGui: a factory for the concrete Dear ImGui
// implementation of aver::rhi::d3d12::IUiBackend (see that header, in Aver.RHI.D3D12, for the
// contract). Deliberately a separate header from UiBackend.hpp -- that one is what the RHI module
// itself knows about and must stay free of anything ImGui-specific; this one is what Sandbox, the
// sole caller, includes to construct the real thing. Neither header ever includes imgui.h: the
// concrete class and everything ImGui-shaped about it stay inside ImGuiUiBackend.cpp, the one file in
// this module that needs Dear ImGui's own headers.
#pragma once
#include "aver/rhi/d3d12/UiBackend.hpp"

namespace aver::rhi::d3d12::imgui_backend {

// Constructs a new Dear ImGui backend. The caller owns it (Sandbox holds it in a std::unique_ptr --
// see SandboxApp.cpp's uiBackend_ member) and must not destroy it before the device's uiShutdown()
// has run.
IUiBackend* create();

} // namespace aver::rhi::d3d12::imgui_backend
