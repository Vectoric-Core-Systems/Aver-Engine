// Cage building, repair, tear resolution and queries. The step itself is in Solver.cpp.
#include "aver/softbody/Cage.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aver::softbody {
namespace {

u64 edgeKey(u32 a, u32 b) {
    const u32 lo = std::min(a, b), hi = std::max(a, b);
    return (static_cast<u64>(lo) << 32) | hi;
}

// Union-find whose root is always the smallest member, so group order follows triangle order.
struct Dsu {
    std::vector<u32> parent;
    explicit Dsu(u32 n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    u32 find(u32 x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    void unite(u32 a, u32 b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

bool edgeHas(const Triangle& t, u32 k, u32 p) { return t.v[k] == p || t.v[(k + 1) % 3] == p; }

} // namespace

u32 addMaterial(Cage& c, const Material& m) {
    c.materials.push_back(m);
    return static_cast<u32>(c.materials.size() - 1);
}

u32 addParticle(Cage& c, const Vec3& p, bool pinned) {
    Particle q;
    q.pos = q.prev = q.rest = p;
    q.pinned = pinned;
    q.origin = static_cast<u32>(c.particles.size());
    c.particles.push_back(q);
    return q.origin;
}

u32 addBeam(Cage& c, u32 a, u32 b, u32 material) {
    const u32 n = static_cast<u32>(c.particles.size());
    if (c.built || a >= n || b >= n || a == b) return kNone;
    if (material >= c.materials.size()) material = 0;
    const u64 key = edgeKey(a, b);
    const auto it = c.edgeIndex.find(key);
    if (it != c.edgeIndex.end()) return it->second;
    Beam bm;
    bm.a = bm.a0 = a;
    bm.b = bm.b0 = b;
    bm.material = material;
    bm.rest = bm.rest0 = dist(c.particles[a].pos, c.particles[b].pos);
    const u32 idx = static_cast<u32>(c.beams.size());
    c.beams.push_back(bm);
    c.edgeIndex.emplace(key, idx);
    return idx;
}

u32 addTriangle(Cage& c, u32 a, u32 b, u32 d, u32 material) {
    const u32 n = static_cast<u32>(c.particles.size());
    if (c.built || a >= n || b >= n || d >= n || a == b || b == d || a == d) return kNone;
    Triangle t;
    t.v[0] = a; t.v[1] = b; t.v[2] = d;
    for (u32 k = 0; k < 3; ++k) t.edge[k] = addBeam(c, t.v[k], t.v[(k + 1) % 3], material);
    c.triangles.push_back(t);
    return static_cast<u32>(c.triangles.size() - 1);
}

std::vector<u32> addGrid(Cage& c, const GridSpec& g) {
    std::vector<u32> idx;
    if (g.cols < 2 || g.rows < 2) return idx;
    idx.reserve(static_cast<usize>(g.cols) * g.rows);
    for (u32 y = 0; y < g.rows; ++y)
        for (u32 x = 0; x < g.cols; ++x)
            idx.push_back(addParticle(c, g.origin + g.du * static_cast<f32>(x) + g.dv * static_cast<f32>(y)));
    auto at = [&](u32 x, u32 y) { return idx[static_cast<usize>(y) * g.cols + x]; };
    for (u32 y = 0; y + 1 < g.rows; ++y)
        for (u32 x = 0; x + 1 < g.cols; ++x) {
            const u32 A = at(x, y), B = at(x + 1, y), C = at(x + 1, y + 1), D = at(x, y + 1);
            if (((x + y) & 1u) == 0) {
                addTriangle(c, A, B, C, g.material);
                addTriangle(c, A, C, D, g.material);
            } else {
                addTriangle(c, A, B, D, g.material);
                addTriangle(c, B, C, D, g.material);
            }
        }
    return idx;
}

void build(Cage& c) {
    if (c.built) return;
    std::vector<i32> votes(c.particles.size(), 0);
    for (u32 i = 0; i < c.particles.size(); ++i) {
        Particle& p = c.particles[i];
        p.prev = p.rest = p.pos;
        p.origin = i;
        p.freed = false;
        p.intact = 0;
        p.hadBeams = false;
    }
    for (Beam& b : c.beams) {
        b.a = b.a0; b.b = b.b0;
        b.rest = b.rest0 = dist(c.particles[b.a].pos, c.particles[b.b].pos);
        b.plastic = b.peak = 0.0f;
        b.broken = false;
        const i32 vote = isBrittle(c.materials[b.material].behavior) ? 1 : -1;
        for (const u32 n : {b.a, b.b}) {
            Particle& p = c.particles[n];
            ++p.intact;
            p.hadBeams = true;
            votes[n] += vote;
        }
    }
    for (u32 i = 0; i < c.particles.size(); ++i) c.particles[i].brittle = votes[i] > 0;
    for (Triangle& t : c.triangles)
        for (u32 k = 0; k < 3; ++k) t.v0[k] = t.v[k];
    c.triDead.assign(c.triangles.size(), 0);
    c.splitLog.clear();
    c.baseParticleCount = static_cast<u32>(c.particles.size());
    c.topologyDirty = false;
    c.built = true;
    std::unordered_map<u64, u32>().swap(c.edgeIndex);
}

void repair(Cage& c) {
    if (!c.built) return;
    c.particles.resize(c.baseParticleCount);
    for (Particle& p : c.particles) {
        p.pos = p.prev = p.rest;
        p.freed = false;
        p.intact = 0;
    }
    for (Beam& b : c.beams) {
        b.a = b.a0; b.b = b.b0;
        b.rest = b.rest0;
        b.plastic = b.peak = 0.0f;
        b.broken = false;
        ++c.particles[b.a].intact;
        ++c.particles[b.b].intact;
    }
    for (Triangle& t : c.triangles)
        for (u32 k = 0; k < 3; ++k) t.v[k] = t.v0[k];
    std::fill(c.triDead.begin(), c.triDead.end(), static_cast<u8>(0));
    c.splitLog.clear();
    c.topologyDirty = false;
}

void breakBeam(Cage& c, u32 beam) {
    if (beam >= c.beams.size() || c.beams[beam].broken) return;
    c.beams[beam].broken = true;
    c.topologyDirty = true;
}

u32 resolveTears(Cage& c) {
    c.topologyDirty = false;
    if (!c.built) return 0;
    const u32 n0 = static_cast<u32>(c.particles.size());
    const u32 triCount = static_cast<u32>(c.triangles.size());
    u32 created = 0;

    if (triCount > 0) {
        // Vertex -> triangles, ascending triangle order.
        std::vector<u32> start(static_cast<usize>(n0) + 1, 0);
        for (const Triangle& t : c.triangles)
            for (u32 k = 0; k < 3; ++k) ++start[t.v[k] + 1];
        for (u32 i = 0; i < n0; ++i) start[i + 1] += start[i];
        std::vector<u32> list(start[n0]);
        std::vector<u32> cursor(start.begin(), start.end() - 1);
        for (u32 ti = 0; ti < triCount; ++ti)
            for (u32 k = 0; k < 3; ++k) list[cursor[c.triangles[ti].v[k]]++] = ti;

        // A triangle with two or more broken edges spans a gap: it is dead (see below) and must not
        // keep a group alive, or the particle it touches is split into a beamless debris duplicate.
        std::vector<u8> spans(triCount, 0);
        for (u32 ti = 0; ti < triCount; ++ti) {
            u32 brokenEdges = 0;
            for (u32 k = 0; k < 3; ++k)
                if (c.beams[c.triangles[ti].edge[k]].broken) ++brokenEdges;
            spans[ti] = brokenEdges >= 2 ? 1 : 0;
        }
        std::vector<u32> live;

        for (u32 p = 0; p < n0; ++p) {
            live.clear();
            for (u32 i = start[p]; i < start[p + 1]; ++i)
                if (!spans[list[i]]) live.push_back(list[i]);
            const u32 count = static_cast<u32>(live.size());
            if (count < 2) continue;
            const u32* tris = live.data();

            // Only a particle touching a broken edge can be torn open.
            bool candidate = false;
            for (u32 i = 0; i < count && !candidate; ++i) {
                const Triangle& t = c.triangles[tris[i]];
                for (u32 k = 0; k < 3; ++k)
                    if (edgeHas(t, k, p) && c.beams[t.edge[k]].broken) { candidate = true; break; }
            }
            if (!candidate) continue;

            // Triangles joined across an unbroken shared edge hold together around p.
            Dsu dsu(count);
            for (u32 i = 0; i < count; ++i) {
                const Triangle& ti = c.triangles[tris[i]];
                for (u32 j = i + 1; j < count; ++j) {
                    const Triangle& tj = c.triangles[tris[j]];
                    for (u32 ki = 0; ki < 3; ++ki) {
                        if (!edgeHas(ti, ki, p) || c.beams[ti.edge[ki]].broken) continue;
                        for (u32 kj = 0; kj < 3; ++kj)
                            if (ti.edge[ki] == tj.edge[kj]) dsu.unite(i, j);
                    }
                }
            }
            std::vector<u32> roots;
            std::vector<u32> groupOf(count);
            for (u32 i = 0; i < count; ++i) {
                const u32 r = dsu.find(i);
                u32 g = 0;
                while (g < roots.size() && roots[g] != r) ++g;
                if (g == roots.size()) roots.push_back(r);
                groupOf[i] = g;
            }
            if (roots.size() < 2) continue;

            // Group 0 keeps the particle; every other group gets a duplicate.
            std::vector<u32> dup(roots.size());
            dup[0] = p;
            const usize logBase = c.splitLog.size();
            for (u32 g = 1; g < roots.size(); ++g) {
                Particle copy = c.particles[p];
                copy.pinned = false;
                copy.freed = false;
                copy.intact = 0;
                dup[g] = static_cast<u32>(c.particles.size());
                c.particles.push_back(copy);
                SplitEvent ev;
                ev.oldParticle = p;
                ev.newParticle = dup[g];
                c.splitLog.push_back(std::move(ev));
            }
            for (u32 i = 0; i < count; ++i) {
                const u32 g = groupOf[i];
                if (g == 0) continue;
                const u32 ti = tris[i];
                Triangle& t = c.triangles[ti];
                for (u32 k = 0; k < 3; ++k) {
                    if (!edgeHas(t, k, p)) continue;
                    Beam& bm = c.beams[t.edge[k]];
                    if (bm.broken) continue;
                    if (bm.a == p) bm.a = dup[g];
                    else if (bm.b == p) bm.b = dup[g];
                }
                for (u32 k = 0; k < 3; ++k)
                    if (t.v[k] == p) t.v[k] = dup[g];
                c.splitLog[logBase + g - 1].triangles.push_back(ti);
            }
            created += static_cast<u32>(roots.size() - 1);
        }
    }

    for (Particle& p : c.particles) p.intact = 0;
    for (const Beam& b : c.beams) {
        if (b.broken) continue;
        ++c.particles[b.a].intact;
        ++c.particles[b.b].intact;
    }
    for (Particle& p : c.particles) p.freed = !p.pinned && p.intact == 0 && p.hadBeams;

    // A triangle with two broken edges no longer holds its corners together: it spans a gap.
    for (u32 ti = 0; ti < triCount; ++ti) {
        u32 brokenEdges = 0;
        for (u32 k = 0; k < 3; ++k)
            if (c.beams[c.triangles[ti].edge[k]].broken) ++brokenEdges;
        if (brokenEdges >= 2) c.triDead[ti] = 1;
    }
    return created;
}

u32 countPieces(const Cage& c, std::vector<u32>* ids) {
    const u32 n = static_cast<u32>(c.particles.size());
    Dsu dsu(n);
    for (const Beam& b : c.beams)
        if (!b.broken) dsu.unite(b.a, b.b);
    std::vector<u32> label(n, kNone);
    u32 count = 0;
    if (ids) ids->assign(n, 0);
    for (u32 i = 0; i < n; ++i) {
        const u32 r = dsu.find(i);
        if (label[r] == kNone) label[r] = count++;
        if (ids) (*ids)[i] = label[r];
    }
    return count;
}

u32 brokenBeamCount(const Cage& c) {
    u32 n = 0;
    for (const Beam& b : c.beams) n += b.broken ? 1u : 0u;
    return n;
}

f32 maxStrain(const Cage& c) {
    f32 worst = 0.0f;
    for (const Beam& b : c.beams) {
        if (b.broken || b.rest0 <= 0.0f) continue;
        const f32 len = dist(c.particles[b.a].pos, c.particles[b.b].pos);
        worst = std::max(worst, std::fabs(len - b.rest0) / b.rest0);
    }
    return worst;
}

f32 kineticProxy(const Cage& c) {
    f32 sum = 0.0f;
    for (const Particle& p : c.particles)
        if (!p.pinned) sum += distSquared(p.pos, p.prev);
    return sum;
}

} // namespace aver::softbody
