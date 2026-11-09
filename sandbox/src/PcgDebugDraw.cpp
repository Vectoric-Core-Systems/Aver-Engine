// The PCG Graph tab's viewport debug points: a cross at every point the selected node outputs, through the line-mesh route the other overlays use.
#include "PcgGraphEditor.hpp"

#include "aver/runtime/Engine.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::editor {

void drawPcgDebugPoints(Engine& e, bool hideChrome) {
    static rhi::LineHandle mesh = 0;
    if (mesh) { e.device()->destroyLineMesh(mesh); mesh = 0; }
    const std::vector<Vec3>& segs = pcgDebugSegments();
    if (hideChrome || segs.empty()) return;
    std::vector<rhi::LineVertex> v;
    v.reserve(segs.size());
    for (const Vec3& p : segs) v.push_back(rhi::LineVertex{p.x, p.y, p.z, 1.0f, 0.62f, 0.15f});
    mesh = e.device()->createLineMesh(v.data(), static_cast<u32>(v.size()));
    const Mat4 id = Mat4::identity();
    if (mesh) e.device()->drawLines(mesh, &id.m[0][0]);
}

} // namespace aver::editor
