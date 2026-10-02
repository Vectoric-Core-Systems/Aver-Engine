// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// The one symbol this module exports: a Dear ImGui implementation of Aver.RHI.Vulkan's IUiBackend
// seam. Exact counterpart of modules/rhi.d3d12.imgui's own header, and separate from the RHI for
// the same reason: only Sandbox links this, so nothing that merely links Aver.RHI.Vulkan gets Dear
// ImGui compiled into it.
//
// NO ImGui TYPE APPEARS HERE. The caller receives an IUiBackend* and never needs to know what
// implements it.
#pragma once
#include "aver/rhi/vulkan/UiBackend.hpp"

namespace aver::rhi::vkb::imgui_backend {

// A new Dear ImGui backend, caller-owned. Hand it to vkb::installUiBackend and keep it alive until
// after the device's uiShutdown() -- see IUiBackend's own note on ownership.
IUiBackend* create();

} // namespace aver::rhi::vkb::imgui_backend
