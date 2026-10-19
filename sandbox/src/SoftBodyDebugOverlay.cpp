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

Vec3 toWorld(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

Vec3 dirToWorld(const Vec3& v, const Mat4& m) {
    return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0], v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
            v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2]};
}

// A circle of `radius` in the local plane spanned by (u, v), centred at `centre`, carried through `m`.
void circle(SoftBodyDebugFrame& f, const Mat4& m, const Vec3& centre, const Vec3& u, const Vec3& v, f32 radius, f32 r, f32 g, f32 b) {
    constexpr int kSeg = 32;
    Vec3 prev = toWorld(centre + u * radius, m);
    for (int i = 1; i <= kSeg; ++i) {
        const f32 a = kTwoPi * static_cast<f32>(i) / static_cast<f32>(kSeg);
        const Vec3 p = toWorld(centre + u * (std::cos(a) * radius) + v * (std::sin(a) * radius), m);
        line(f, prev, p, r, g, b);
        prev = p;
    }
}

void arrow(SoftBodyDebugFrame& f, const Vec3& from, const Vec3& dir, f32 length, f32 r, f32 g, f32 b) {
    const Vec3 d = dir.getSafeNormal();
    if (d.sizeSquared() < 0.5f) return;
    const Vec3 to = from + d * length;
    Vec3 side = cross(d, Vec3{0, 0, 1});
    if (side.sizeSquared() < 1e-4f) side = cross(d, Vec3{1, 0, 0});
    side = side.getSafeNormal() * (length * 0.12f);
    line(f, from, to, r, g, b);
    line(f, to, to - d * (length * 0.2f) + side, r, g, b);
    line(f, to, to - d * (length * 0.2f) - side, r, g, b);
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
void softFieldDebugFeed(const softbody::FieldSpec& spec, f32 r, f32 g, f32 b, SoftBodyDebugFrame& out) {
    const Mat4& m = spec.world;
    const Vec3 centre = toWorld(Vec3{0, 0, 0}, m);
    const Vec3 ex = spec.extents;
    switch (spec.shape) {
        case softbody::FieldShape::Sphere:
            circle(out, m, Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, ex.x, r, g, b);
            circle(out, m, Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}, ex.x, r, g, b);
            circle(out, m, Vec3{0, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, ex.x, r, g, b);
            break;
        case softbody::FieldShape::Box: {
            Vec3 c[8];
            for (int i = 0; i < 8; ++i) c[i] = toWorld(Vec3{(i & 1) ? ex.x : -ex.x, (i & 2) ? ex.y : -ex.y, (i & 4) ? ex.z : -ex.z}, m);
            static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : kEdges) line(out, c[e[0]], c[e[1]], r, g, b);
            break;
        }
        case softbody::FieldShape::Capsule: {
            circle(out, m, Vec3{0, 0, -ex.z}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, ex.x, r, g, b);
            circle(out, m, Vec3{0, 0, ex.z}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, ex.x, r, g, b);
            const Vec3 rails[4] = {{ex.x, 0, 0}, {-ex.x, 0, 0}, {0, ex.x, 0}, {0, -ex.x, 0}};
            for (const Vec3& rl : rails) line(out, toWorld(rl + Vec3{0, 0, -ex.z}, m), toWorld(rl + Vec3{0, 0, ex.z}, m), r, g, b);
            line(out, toWorld(Vec3{0, 0, -ex.z - ex.x}, m), toWorld(Vec3{0, 0, ex.z + ex.x}, m), r, g, b);
            break;
        }
        default: {
            const f32 s = 40.0f;
            line(out, centre - Vec3{s, 0, 0}, centre + Vec3{s, 0, 0}, r, g, b);
            line(out, centre - Vec3{0, s, 0}, centre + Vec3{0, s, 0}, r, g, b);
            line(out, centre - Vec3{0, 0, s}, centre + Vec3{0, 0, s}, r, g, b);
            break;
        }
    }
    const f32 reach = spec.shape == softbody::FieldShape::Everywhere ? 100.0f : std::max({ex.x, spec.shape == softbody::FieldShape::Sphere ? ex.x : ex.y, 20.0f});
    if (spec.kind == softbody::FieldKind::DirectionalForce || (spec.kind == softbody::FieldKind::Damage && spec.dir.sizeSquared() > 1e-8f)) {
        const Vec3 axis = spec.dir.sizeSquared() > 1e-8f ? dirToWorld(spec.dir, m) : dirToWorld(Vec3{0, 0, 1}, m);
        arrow(out, centre, axis, reach, r, g, b);
    } else if (spec.kind == softbody::FieldKind::RadialForce) {
        const f32 sign = spec.strength < 0.0f ? -1.0f : 1.0f;
        static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const Vec3& a : kAxes) {
            const Vec3 d = dirToWorld(a, m).getSafeNormal() * sign;
            // A push points out of the centre; a pull points in at the same place.
            arrow(out, sign > 0.0f ? centre : centre + d * (-0.4f * reach), d, 0.4f * reach, r, g, b);
        }
    }
}

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
    if (opts.fields) {
        // Component fields (each entity's frame is its transform), transient fields, and the pieces in each field's reach.
        std::vector<softbody::FieldSpec> specs;
        const u32 fieldType = softbody::SoftBodySystem::softFieldType();
        if (fieldType) {
            scene::ComponentPool* fp = w.pool(fieldType);
            for (usize i = 0; fp && i < fp->size(); ++i) {
                const scene::Entity e = fp->entityAt(i);
                softbody::FieldSpec spec;
                if (!w.valid(e) || !softbody::SoftBodySystem::specOfComponent(w, e, spec)) continue;
                const auto* c = w.component<softbody::CSoftField>(e, fieldType);
                if (c && (c->flags & softbody::kFieldDisabled)) { f32 gr, gg, gb; fieldmodel::kindColour(spec.kind, gr, gg, gb); softFieldDebugFeed(spec, gr * 0.4f, gg * 0.4f, gb * 0.4f, out); continue; }
                specs.push_back(spec);
            }
        }
        for (const auto& tf : sys.transientFields()) specs.push_back(tf.spec);
        for (const softbody::FieldSpec& spec : specs) {
            f32 fr, fg, fb;
            fieldmodel::kindColour(spec.kind, fr, fg, fb);
            softFieldDebugFeed(spec, fr, fg, fb, out);
            for (u32 i = 0; i < count; ++i) {
                if (views[i].vertexCount == 0 || views[i].layer != 0) continue;
                Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
                for (u32 v = 0; v < views[i].vertexCount; ++v) {
                    const Vec3& p = views[i].world[v];
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                if (!softbody::fieldOverlapsBounds(spec, lo, hi)) continue;
                // Highlight: the piece's box in the field's colour, a little brighter than the field itself.
                const Vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                                   {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
                static const int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& e : kEdges) line(out, c[e[0]], c[e[1]], std::min(1.0f, fr + 0.3f), std::min(1.0f, fg + 0.3f), std::min(1.0f, fb + 0.3f));
            }
        }
    }
    if (opts.tuning && opts.selected != 0) {
        const scene::Entity sel = static_cast<scene::Entity>(opts.selected);
        const softbody::HitInfo h = sys.lastHit(sel);
        fieldmodel::TuningHit th;
        th.valid = h.valid; th.kind = h.kind; th.closingCmS = h.closingCmS; th.energyJ = h.energyJ; th.radiusCm = h.radiusCm;
        th.beamsDamaged = h.beamsDamaged; th.broken = h.broken; th.maxStrain = h.maxStrain; th.maxSetCm = h.maxSetCm;
        th.tick = h.tick; th.pieces = h.pieces; th.baked = h.baked;
        out.tuningLines.push_back(fieldmodel::tuningHitText(th));
        const softbody::Stats st = sys.stats();
        fieldmodel::TuningFrame tf;
        tf.pieces = st.pieces; tf.activeParticles = st.activeParticles; tf.rebuilds = st.rebuildsLastTick; tf.drawBuilds = opts.drawBuilds;
        tf.fxSent = st.fxSent; tf.fxDropped = st.fxDropped;
        out.tuningLines.push_back(fieldmodel::tuningFrameText(tf));
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
void softBodyDebugDrawTuning(const SoftBodyDebugFrame& frame, f32 vpX, f32 vpY);

void softBodyDebugDrawBudget(const SoftBodyDebugFrame& frame, f32 vpX, f32 vpY) {
    softBodyDebugDrawTuning(frame, vpX, vpY);
    if (frame.budgetText.empty()) return;
    ImGui::GetForegroundDrawList()->AddText(ImVec2(vpX + 8.0f, vpY + 8.0f), IM_COL32(230, 230, 200, 255), frame.budgetText.c_str());
}

void softBodyDebugDrawTuning(const SoftBodyDebugFrame& frame, f32 vpX, f32 vpY) {
    f32 y = vpY + (frame.budgetText.empty() ? 8.0f : 26.0f);
    for (const std::string& line : frame.tuningLines) {
        ImGui::GetForegroundDrawList()->AddText(ImVec2(vpX + 8.0f, y), IM_COL32(255, 220, 140, 255), line.c_str());
        y += 16.0f;
    }
}
#endif

}  // namespace aver::editor
