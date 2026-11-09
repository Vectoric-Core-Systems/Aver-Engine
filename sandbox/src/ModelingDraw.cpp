// Modeling mode's wireframe overlay, through the line-mesh route the other overlays use (world-space vertices, identity matrix).
#include "ModelingMode.hpp"

#include "aver/runtime/Engine.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::editor {

void drawModelingLines(Engine& e, const ModelingMode& mode, bool active, bool hideChrome) {
    static rhi::LineHandle mesh = 0;
    if (mesh) { e.device()->destroyLineMesh(mesh); mesh = 0; }
    if (!active || hideChrome) return;
    std::vector<ModelingLine> lines;
    mode.collectLines(lines);
    if (lines.empty()) return;
    std::vector<rhi::LineVertex> v;
    v.reserve(lines.size() * 2);
    for (const ModelingLine& l : lines) {
        v.push_back(rhi::LineVertex{l.a.x, l.a.y, l.a.z, l.r, l.g, l.bl});
        v.push_back(rhi::LineVertex{l.b.x, l.b.y, l.b.z, l.r, l.g, l.bl});
    }
    mesh = e.device()->createLineMesh(v.data(), static_cast<u32>(v.size()));
    const Mat4 id = Mat4::identity();
    if (mesh) e.device()->drawLines(mesh, &id.m[0][0]);
}

} // namespace aver::editor
