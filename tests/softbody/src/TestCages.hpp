// Cages shared by the soft-body tests: a planar truss bar, a cloth strip with a weak seam.
#pragma once

#include "aver/softbody/Cage.hpp"

#include <cmath>
#include <vector>

namespace sbtest {

using namespace aver;
namespace sb = aver::softbody;

// A material that never yields and never breaks: the elastic control.
inline sb::Material elasticMaterial() {
    sb::Material m;
    m.bendForceN = 1e9f;
    m.breakForceN = 1e9f;
    m.maxBend = 1e9f;
    return m;
}

// The beam of `c` joining particles a and b as authored, or kNone.
inline u32 findBeam(const sb::Cage& c, u32 a, u32 b) {
    for (u32 i = 0; i < c.beams.size(); ++i) {
        const sb::Beam& bm = c.beams[i];
        if ((bm.a0 == a && bm.b0 == b) || (bm.a0 == b && bm.b0 == a)) return i;
    }
    return sb::kNone;
}

// A cantilever truss along +X: `bays` bays of `bay` cm, `height` cm tall, the root column pinned.
struct Truss {
    sb::Cage cage;
    std::vector<u32> top, bot;
    f32 topZ0 = 0.0f, botZ0 = 0.0f;
    u32 tipTop() const { return top.back(); }
    u32 tipBot() const { return bot.back(); }
};

inline Truss makeTruss(const sb::Material& mat, u32 bays = 5, f32 bay = 20.0f, f32 height = 20.0f) {
    Truss t;
    const u32 m = sb::addMaterial(t.cage, mat);
    for (u32 i = 0; i <= bays; ++i) {
        t.bot.push_back(sb::addParticle(t.cage, Vec3{bay * static_cast<f32>(i), 0, 0}, i == 0));
        t.top.push_back(sb::addParticle(t.cage, Vec3{bay * static_cast<f32>(i), 0, height}, i == 0));
    }
    for (u32 i = 0; i <= bays; ++i) {
        sb::addBeam(t.cage, t.bot[i], t.top[i], m);
        if (i == bays) break;
        sb::addBeam(t.cage, t.bot[i], t.bot[i + 1], m);
        sb::addBeam(t.cage, t.top[i], t.top[i + 1], m);
        sb::addBeam(t.cage, t.bot[i], t.top[i + 1], m);
        sb::addBeam(t.cage, t.top[i], t.bot[i + 1], m);
    }
    sb::build(t.cage);
    t.topZ0 = t.cage.particles[t.tipTop()].pos.z;
    t.botZ0 = t.cage.particles[t.tipBot()].pos.z;
    return t;
}

// Holds the tip down by `depth` cm (kinematic), then lets go and lets the bar settle.
inline void bendTip(Truss& t, f32 depth, const sb::StepConfig& cfg) {
    const u32 tips[2] = {t.tipTop(), t.tipBot()};
    const f32 z0[2] = {t.topZ0, t.botZ0};
    for (const u32 n : tips) t.cage.particles[n].pinned = true;
    const int drive = 30, hold = 60, settle = 400;
    for (int s = 0; s < drive; ++s) {
        for (int k = 0; k < 2; ++k)
            t.cage.particles[tips[k]].pos.z = z0[k] - depth * static_cast<f32>(s + 1) / static_cast<f32>(drive);
        sb::step(t.cage, cfg);
    }
    for (int s = 0; s < hold; ++s) sb::step(t.cage, cfg);
    for (const u32 n : tips) t.cage.particles[n].pinned = false;
    for (int s = 0; s < settle; ++s) sb::step(t.cage, cfg);
}

// A cols x rows strip in the XZ plane, spacing 10 cm, the two end columns pinned. Beams crossing
// between column `seamCol` and `seamCol + 1` use a weaker break strain: a perforation.
struct Strip {
    sb::Cage cage;
    std::vector<u32> ids;          // row-major
    u32 cols = 0, rows = 0;
    u32 seamBeams = 0;
};

inline Strip makeStrip(u32 cols, u32 rows, u32 seamCol, f32 seamStrain, f32 strain) {
    Strip s;
    s.cols = cols;
    s.rows = rows;
    sb::Material cloth = elasticMaterial();
    cloth.breakStrain = strain;
    sb::Material seam = cloth;
    seam.breakStrain = seamStrain;
    const u32 clothM = sb::addMaterial(s.cage, cloth);
    const u32 seamM = sb::addMaterial(s.cage, seam);
    sb::GridSpec g;
    g.cols = cols;
    g.rows = rows;
    g.material = clothM;
    s.ids = sb::addGrid(s.cage, g);
    for (sb::Beam& b : s.cage.beams) {
        const u32 ca = b.a0 % cols, cb = b.b0 % cols;
        if ((ca == seamCol && cb == seamCol + 1) || (cb == seamCol && ca == seamCol + 1)) {
            b.material = seamM;
            ++s.seamBeams;
        }
    }
    for (u32 y = 0; y < rows; ++y) {
        s.cage.particles[s.ids[y * cols]].pinned = true;
        s.cage.particles[s.ids[y * cols + cols - 1]].pinned = true;
    }
    sb::build(s.cage);
    return s;
}

// Drags the right-hand column `dx` cm per step for `steps` steps.
inline void pullStrip(Strip& s, f32 dx, int steps, const sb::StepConfig& cfg) {
    for (int i = 0; i < steps; ++i) {
        for (u32 y = 0; y < s.rows; ++y)
            s.cage.particles[s.ids[y * s.cols + s.cols - 1]].pos.x += dx;
        sb::step(s.cage, cfg);
    }
}

// A flat cols x rows sheet in the XY plane with its x == 0 column pinned: a flag that swings down.
inline sb::Cage makeFlag(const sb::Material& mat, u32 cols = 8, u32 rows = 8) {
    sb::Cage c;
    const u32 m = sb::addMaterial(c, mat);
    sb::GridSpec g;
    g.cols = cols;
    g.rows = rows;
    g.dv = Vec3{0, 10, 0};
    g.material = m;
    const std::vector<u32> ids = sb::addGrid(c, g);
    for (u32 y = 0; y < rows; ++y) c.particles[ids[y * cols]].pinned = true;
    sb::build(c);
    return c;
}

inline bool bitwiseEqual(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

inline std::vector<Vec3> positionsOf(const sb::Cage& c) {
    std::vector<Vec3> out;
    out.reserve(c.particles.size());
    for (const sb::Particle& p : c.particles) out.push_back(p.pos);
    return out;
}

inline bool allFinite(const sb::Cage& c) {
    for (const sb::Particle& p : c.particles)
        if (!std::isfinite(p.pos.x) || !std::isfinite(p.pos.y) || !std::isfinite(p.pos.z)) return false;
    return true;
}

} // namespace sbtest
