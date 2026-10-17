// Soft-body viewport overlay. See the header for the wiring.
#include "SoftBodyDebugOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if AVER_MODULE_SOFTBODY_SCENE
#  include "aver/scene/World.hpp"
#endif
#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

namespace {

void line(SoftBodyDebugFrame& f, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    f.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    f.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

}  // namespace

void softBodyStrainColour(f32 strain, f32 breakStrain, f32 plastic01, bool broken, f32& r, f32& g, f32& b) {
    if (broken) { r = 1.0f; g = 0.1f; b = 1.0f; return; }
    const f32 limit = breakStrain > 0.0f ? breakStrain : 0.3f;
    const f32 t = std::clamp(strain / limit, -1.0f, 1.0f);
    if (t >= 0.0f) { r = t; g = 1.0f - 0.5f * t; b = 0.2f * (1.0f - t); }       // green -> red
    else           { r = 0.0f; g = 1.0f + 0.5f * t; b = -t; }                    // green -> blue
    const f32 w = std::clamp(plastic01, 0.0f, 1.0f);
    r += (1.0f - r) * w; g += (1.0f - g) * w; b += (1.0f - b) * w;               // the set tints toward white
}

#if AVER_MODULE_SOFTBODY_SCENE
void softBodyDebugFeedCage(const softbody::Cage& cage, const SoftBodyDebugOptions& opts, SoftBodyDebugFrame& out) {
    if (opts.strain) {
        for (const softbody::Beam& b : cage.beams) {
            f32 r, g, bl;
            const Vec3& pa = cage.particles[b.a].pos;
            const Vec3& pb = cage.particles[b.b].pos;
            if (b.broken) {
                softBodyStrainColour(0.0f, 0.0f, 0.0f, true, r, g, bl);
                line(out, pa, pb, r, g, bl);
                continue;
            }
            const f32 strain = b.rest0 > 0.0f ? (dist(pa, pb) - b.rest0) / b.rest0 : 0.0f;
            const softbody::Material& m = cage.materials[b.material];
            const f32 plastic01 = b.rest0 > 0.0f ? std::min(1.0f, b.plastic / (0.25f * b.rest0)) : 0.0f;
            softBodyStrainColour(strain, m.breakStrain, plastic01, false, r, g, bl);
            line(out, pa, pb, r, g, bl);
        }
    }
    if (opts.pins) {
        for (const softbody::Particle& p : cage.particles) {
            if (!p.pinned) continue;
            const f32 s = 1.5f;
            line(out, p.pos - Vec3{s, 0, 0}, p.pos + Vec3{s, 0, 0}, 1.0f, 0.9f, 0.1f);
            line(out, p.pos - Vec3{0, s, 0}, p.pos + Vec3{0, s, 0}, 1.0f, 0.9f, 0.1f);
            line(out, p.pos - Vec3{0, 0, s}, p.pos + Vec3{0, 0, s}, 1.0f, 0.9f, 0.1f);
        }
    }
}

void softBodyDebugCollect(const SoftBodyDebugOptions& opts, softbody::SoftBodySystem& sys, SoftBodyDebugFrame& out) {
    out.clear();
    if (!opts.any()) return;
    scene::World& w = scene::World::instance();
    if (opts.strain || opts.pins) {
        scene::ComponentPool* pool = w.pool(scene::kComponentSoftBody);
        for (usize i = 0; pool && i < pool->size(); ++i) {
            const scene::Entity e = pool->entityAt(i);
            if (const softbody::Cage* c = w.valid(e) ? sys.cageOf(e) : nullptr) softBodyDebugFeedCage(*c, opts, out);
        }
    }
    u32 count = 0;
    const softbody::PieceView* views = sys.pieces(count);
    if (opts.pieces) {
        for (u32 i = 0; i < count; ++i) {
            // The view carries the world positions of its vertices; the box is theirs.
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (u32 v = 0; v < views[i].vertexCount; ++v) {
                const Vec3& p = views[i].world[v];
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            if (views[i].vertexCount == 0) continue;
            const f32 r = views[i].useEntityWorld ? 1.0f : 0.2f, g = views[i].useEntityWorld ? 1.0f : 0.9f, b = views[i].useEntityWorld ? 1.0f : 0.9f;
            const Vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                               {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
            static const int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : kEdges) line(out, c[e[0]], c[e[1]], r, g, b);
        }
    }
    if (opts.wind) {
        const Vec3 wind = sys.windVelocity();
        const f32 speed = wind.size();
        if (speed > 0.0f) {
            for (u32 i = 0; i < count; ++i) {
                if (!views[i].useEntityWorld) continue;
                const Vec3 from = views[i].centroid + Vec3{0, 0, 60};
                const Vec3 to = from + wind.getSafeNormal() * std::min(150.0f, speed * 0.25f);
                line(out, from, to, 0.4f, 0.8f, 1.0f);
                const Vec3 side = cross(wind.getSafeNormal(), Vec3{0, 0, 1}).getSafeNormal() * 8.0f;
                line(out, to, to - wind.getSafeNormal() * 12.0f + side, 0.4f, 0.8f, 1.0f);
                line(out, to, to - wind.getSafeNormal() * 12.0f - side, 0.4f, 0.8f, 1.0f);
            }
        }
    }
    if (opts.budget) {
        const softbody::Stats s = sys.stats();
        char buf[200];
        std::snprintf(buf, sizeof buf, "soft bodies  entries %u   pieces %u (debris %u)   particles %u   awake %u   frozen %u   rebuilds %u   deposits %u",
                      s.entries, s.pieces, s.debris, s.activeParticles, s.awakePieces, s.frozenPieces, s.rebuildsLastTick, s.depositsLastTick);
        out.budgetText = buf;
    }
}
#endif

#if AVER_WITH_IMGUI
void softBodyDebugDrawBudget(const SoftBodyDebugFrame& frame, f32 vpX, f32 vpY) {
    if (frame.budgetText.empty()) return;
    ImGui::GetForegroundDrawList()->AddText(ImVec2(vpX + 8.0f, vpY + 8.0f), IM_COL32(230, 230, 200, 255), frame.budgetText.c_str());
}
#endif

}  // namespace aver::editor
