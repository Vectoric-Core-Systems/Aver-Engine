#pragma once
// Hair viewport overlay (Show > Hair): the groom's guide strands at rest (a sample, amber) and the collision proxies at their live pose (cyan for
// the groom's, magenta for the ones a script added). The full-strand debug draw of Jolt's Hair::Draw is behind JPH_DEBUG_RENDERER, which stays off;
// the guides are the sample HairSystem keeps from the build (hair space, at rest, so they follow the character's transform but not the simulation
// or the pose), which is what is wanted for judging where the roots and the proxies are.
//
// HEADER-ONLY AND SELF-CONTAINED so the editor needs only two lines: the viewport pass calls hairDebugDraw(device, noEditorChrome)
// once per frame beside the other overlays, and the Show menu has one checkbox row for hairDebugOptions().guides and .proxies. The options are a
// function-local static (hairDebugOptions()), so no SandboxApp member is needed; the line mesh is owned here and replaced each frame, the way the
// other overlays do it.
//
// Pure feeds -> rhi::LineVertex in world space, drawn with an identity matrix.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_HAIR_SCENE
#  include "aver/hair/HairSystem.hpp"
#  include "aver/scene/World.hpp"
#endif

#include <cmath>
#include <vector>

namespace aver::editor {

struct HairDebugOptions {
    bool guides = false;          // the sampled guide strands at rest
    bool proxies = false;         // the collision proxies at their live pose
    bool selectedOnly = true;     // only the selected character's hair (everything when nothing is selected or it has none)
    u32 selected = 0;             // the selected entity, set by the host each frame
    bool any() const { return guides || proxies; }
};

inline HairDebugOptions& hairDebugOptions() {
    static HairDebugOptions o;
    return o;
}

struct HairDebugFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    void clear() { lines.clear(); }
};

namespace hairdbg {

// Row-vector convention (Mat4 in this engine): a point goes through the matrix as x * row0 + y * row1 + z * row2 + row3.
inline Vec3 toWorld(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

inline void line(HairDebugFrame& f, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    f.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    f.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

// A circle of `n` segments in the plane spanned by unit axes u and v, centred at c, in the hull's local space.
inline void circle(HairDebugFrame& f, const Mat4& m, const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, int n, f32 r, f32 g, f32 b) {
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= n; ++i) {
        const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(n);
        const Vec3 cur = c + u * (radius * std::cos(a)) + v * (radius * std::sin(a));
        line(f, toWorld(prev, m), toWorld(cur, m), r, g, b);
        prev = cur;
    }
}

} // namespace hairdbg

// The guide strands of a groom: `segments` are 6 floats each (two points, hair space, cm) and `world` is the owner's world matrix. Pure.
inline void hairDebugFeedGuides(const std::vector<f32>& segments, const Mat4& world, f32 r, f32 g, f32 b, HairDebugFrame& out) {
    for (usize i = 0; i + 6 <= segments.size(); i += 6)
        hairdbg::line(out, hairdbg::toWorld({segments[i], segments[i + 1], segments[i + 2]}, world),
                      hairdbg::toWorld({segments[i + 3], segments[i + 4], segments[i + 5]}, world), r, g, b);
}

// One collision proxy as a wire hull at `world` (the body's pose; the long axis of a capsule is its local +Z, as proxyHullPoints builds it): a sphere
// as three circles, a capsule as two end circles, two mid circles and four rails, a box as its twelve edges. Pure; a test pins the line counts.
inline void hairDebugFeedProxy(fmt::OcHairProxyShape shape, const f32 size[3], const Mat4& world, f32 r, f32 g, f32 b, HairDebugFrame& out) {
    const Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1}, O{0, 0, 0};
    switch (shape) {
        case fmt::OcHairProxyShape::Sphere:
            hairdbg::circle(out, world, O, X, Y, size[0], 16, r, g, b);
            hairdbg::circle(out, world, O, X, Z, size[0], 16, r, g, b);
            hairdbg::circle(out, world, O, Y, Z, size[0], 16, r, g, b);
            break;
        case fmt::OcHairProxyShape::Capsule: {
            const f32 rad = size[0], h = size[1];
            hairdbg::circle(out, world, Z * h, X, Y, rad, 16, r, g, b);
            hairdbg::circle(out, world, Z * -h, X, Y, rad, 16, r, g, b);
            hairdbg::circle(out, world, Z * (h + rad), X, Y, rad * 0.35f, 8, r, g, b);
            hairdbg::circle(out, world, Z * (-h - rad), X, Y, rad * 0.35f, 8, r, g, b);
            hairdbg::line(out, hairdbg::toWorld(X * rad + Z * h, world), hairdbg::toWorld(X * rad + Z * -h, world), r, g, b);
            hairdbg::line(out, hairdbg::toWorld(X * -rad + Z * h, world), hairdbg::toWorld(X * -rad + Z * -h, world), r, g, b);
            hairdbg::line(out, hairdbg::toWorld(Y * rad + Z * h, world), hairdbg::toWorld(Y * rad + Z * -h, world), r, g, b);
            hairdbg::line(out, hairdbg::toWorld(Y * -rad + Z * h, world), hairdbg::toWorld(Y * -rad + Z * -h, world), r, g, b);
            break;
        }
        default: {
            const f32 x = size[0], y = size[1], z = size[2];
            Vec3 c[8];
            for (int i = 0; i < 8; ++i) c[i] = {(i & 1) ? x : -x, (i & 2) ? y : -y, (i & 4) ? z : -z};
            static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) hairdbg::line(out, hairdbg::toWorld(c[e[0]], world), hairdbg::toWorld(c[e[1]], world), r, g, b);
            break;
        }
    }
}

#if AVER_MODULE_HAIR_SCENE
// Clears `out`, then feeds the hair of the selected character (everything when none is selected or the selection has no hair).
inline void hairDebugCollect(const HairDebugOptions& opts, hair::HairSystem& sys, scene::World& world, HairDebugFrame& out) {
    out.clear();
    if (!opts.any()) return;
    u32 count = 0;
    const hair::HairView* views = sys.views(count);
    bool anySelected = false;
    if (opts.selectedOnly && opts.selected != 0)
        for (u32 i = 0; i < count; ++i)
            if (static_cast<u32>(views[i].entity) == opts.selected) { anySelected = true; break; }
    std::vector<hair::HairSystem::DebugProxy> proxies;
    for (u32 i = 0; i < count; ++i) {
        const hair::HairView& v = views[i];
        if (opts.selectedOnly && anySelected && static_cast<u32>(v.entity) != opts.selected) continue;
        if (!world.valid(v.entity)) continue;
        if (opts.guides)
            if (const std::vector<f32>* guides = sys.guideSegments(v.entity)) hairDebugFeedGuides(*guides, v.world, 0.95f, 0.7f, 0.3f, out);
        if (opts.proxies) {
            proxies.clear();
            sys.debugProxies(v.entity, proxies);
            for (const hair::HairSystem::DebugProxy& p : proxies)
                hairDebugFeedProxy(p.shape, p.sizeCm, p.world, p.script ? 1.0f : 0.2f, p.script ? 0.3f : 0.9f, 1.0f, out);
        }
    }
}

// The one call the viewport pass makes per frame: collects, replaces the overlay's line mesh and draws it. `noEditorChrome` skips the draw (a
// screenshot without chrome). The mesh handle lives here, so the host keeps nothing.
inline void hairDebugDraw(rhi::IDevice& dev, bool noEditorChrome, u32 selectedEntity) {
    static rhi::LineHandle mesh = 0;
    static HairDebugFrame frame;
    if (mesh) { dev.destroyLineMesh(mesh); mesh = 0; }
    HairDebugOptions& opts = hairDebugOptions();
    opts.selected = selectedEntity;
    hairDebugCollect(opts, hair::hairSystem(), scene::World::instance(), frame);
    if (noEditorChrome || frame.lines.empty()) return;
    mesh = dev.createLineMesh(frame.lines.data(), static_cast<u32>(frame.lines.size()));
    const Mat4 id = Mat4::identity();
    if (mesh) dev.drawLines(mesh, &id.m[0][0]);
}
#endif

} // namespace aver::editor
