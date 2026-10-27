#pragma once
// ScriptDebugLines: draws the lines scripts asked for (DebugDraw in C#, aver_fw_debug_line) on top of the frame,
// then ages them by the frame's real time. Header-only: the editor and the shipped game call it from their render.
#include "aver/core/Math.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/rhi/RHI.hpp"

namespace aver::game {

static_assert(sizeof(rhi::LineVertex) == 6 * sizeof(f32), "aver_fw_debug_line_data is LineVertex pairs");

// `mesh` is the caller's, replaced each frame. Draws nothing without a device; ages the lines either way.
inline void drawScriptDebugLines(rhi::IDevice* dev, rhi::LineHandle& mesh, f32 realDt) {
    if (mesh && dev) { dev->destroyLineMesh(mesh); mesh = 0; }
    const int32_t n = aver_fw_debug_line_count();
    if (dev && n > 0) {
        const auto* v = reinterpret_cast<const rhi::LineVertex*>(aver_fw_debug_line_data());
        mesh = dev->createLineMesh(v, static_cast<u32>(n) * 2u);
        const Mat4 id = Mat4::identity();
        if (mesh) dev->drawLines(mesh, &id.m[0][0]);
    }
    aver_fw_debug_lines_advance(realDt);
}

}  // namespace aver::game
