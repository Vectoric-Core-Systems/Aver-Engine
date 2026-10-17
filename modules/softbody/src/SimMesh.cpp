// Welding render vertices into sim particles, building the cage over them, and painting pins and reach.
#include "aver/softbody/SimMesh.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace aver::softbody {
namespace {

constexpr u32 kMaxSimParticles = 65536;   // Jolt body limit in the physics ABI; also keeps triangle keys in 48 bits

struct Weld {
    std::vector<u32> particleOf;   // render vertex -> particle
    std::vector<u32> rep;          // particle -> lowest render vertex
};

u64 cellHash(i64 x, i64 y, i64 z) {
    return (static_cast<u64>(x) * 73856093ull) ^ (static_cast<u64>(y) * 19349663ull) ^ (static_cast<u64>(z) * 83492791ull);
}

// Deterministic: vertices are visited in ascending order, the lowest matching particle wins.
bool weldVertices(const f32* pos, u32 n, f32 weldCm, Weld& out) {
    out.particleOf.assign(n, 0);
    out.rep.clear();
    const f32 tol = std::max(weldCm, 0.0f);
    const f32 tolSq = tol * tol;
    const f32 cell = std::max(tol, 1e-3f);
    std::unordered_map<u64, std::vector<u32>> grid;
    grid.reserve(n);
    for (u32 v = 0; v < n; ++v) {
        const f32 x = pos[v * 3 + 0], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        const i64 cx = static_cast<i64>(std::floor(x / cell));
        const i64 cy = static_cast<i64>(std::floor(y / cell));
        const i64 cz = static_cast<i64>(std::floor(z / cell));
        u32 best = kNone;
        for (i64 dx = -1; dx <= 1; ++dx)
            for (i64 dy = -1; dy <= 1; ++dy)
                for (i64 dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find(cellHash(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (const u32 p : it->second) {
                        if (p >= best) continue;
                        const u32 r = out.rep[p];
                        const f32 ex = pos[r * 3 + 0] - x, ey = pos[r * 3 + 1] - y, ez = pos[r * 3 + 2] - z;
                        if (ex * ex + ey * ey + ez * ez <= tolSq) best = p;
                    }
                }
        if (best == kNone) {
            best = static_cast<u32>(out.rep.size());
            out.rep.push_back(v);
            grid[cellHash(cx, cy, cz)].push_back(best);
        }
        out.particleOf[v] = best;
    }
    return true;
}

Vec3 transformPoint(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

u64 fnvStep(u64 h, const void* data, usize len) {
    const u8* b = static_cast<const u8*>(data);
    for (usize i = 0; i < len; ++i) { h ^= b[i]; h *= kFnv1a64Prime; }
    return h;
}

bool inputOk(const SimMeshInput& in, std::string& why) {
    if (!in.positions || in.vertexCount == 0) { why = "mesh has no positions"; return false; }
    if (!in.indices || in.indexCount < 3 || in.indexCount % 3 != 0) { why = "mesh has no triangles"; return false; }
    for (u32 i = 0; i < in.indexCount; ++i)
        if (in.indices[i] >= in.vertexCount) { why = "mesh index out of range"; return false; }
    return true;
}

} // namespace

u64 simMeshKey(const SimMeshInput& in, f32 weldCm) {
    Weld w;
    if (!in.positions || in.vertexCount == 0 || !weldVertices(in.positions, in.vertexCount, weldCm, w)) return 0;
    u64 h = kFnv1a64OffsetBasis;
    const u32 counts[2] = {in.vertexCount, static_cast<u32>(w.rep.size())};
    h = fnvStep(h, counts, sizeof counts);
    for (const u32 r : w.rep) {
        const i32 q[3] = {static_cast<i32>(std::lround(in.positions[r * 3 + 0] * 1000.0f)),
                          static_cast<i32>(std::lround(in.positions[r * 3 + 1] * 1000.0f)),
                          static_cast<i32>(std::lround(in.positions[r * 3 + 2] * 1000.0f))};
        h = fnvStep(h, q, sizeof q);
    }
    return h;
}

u32 applyPaint(Cage& cage, const PaintData& paint, const PaintEnv& env) {
    const u32 n = static_cast<u32>(cage.particles.size());
    if (!env.mesh || !env.binding || n == 0) return 0;
    const SimMeshInput& mesh = *env.mesh;

    // First render vertex of each particle (ascending scan).
    std::vector<u32> rep(n, kNone);
    for (u32 v = 0; v < env.binding->vertexParticle.size(); ++v) {
        const u32 p = env.binding->vertexParticle[v];
        if (p < n && rep[p] == kNone) rep[p] = v;
    }

    std::vector<u8> pin(n, 0);
    std::vector<f32> reach(n, -1.0f);
    const bool painted = paint.hasPin() && paint.pin.size() == n && !paint.stale(env.meshKey);
    if (painted) {
        for (u32 i = 0; i < n; ++i) pin[i] = paint.pin[i] == 255 ? 1 : 0;
        if (paint.hasReach() && paint.reach.size() == n)
            for (u32 i = 0; i < n; ++i) reach[i] = reachFromPaint(paint.reach[i], env.defaultReachCm) * env.uniformScale;
    } else {
        for (const PaintRule& r : paint.rules) {
            for (u32 i = 0; i < n; ++i) {
                const u32 v = rep[i];
                if (v == kNone || v >= mesh.vertexCount) continue;
                const f32* p = mesh.positions + static_cast<usize>(v) * 3;
                switch (r.type) {
                    case PaintRule::Type::Above:
                        if (p[r.axis < 3 ? r.axis : 2] >= r.valueCm) pin[i] = 1;
                        break;
                    case PaintRule::Type::Bone:
                        if (env.boneWeight && env.boneWeight(r.bone, v) >= r.minWeight) pin[i] = 1;
                        break;
                    case PaintRule::Type::Sphere: {
                        const f32 dx = p[0] - r.centreCm[0], dy = p[1] - r.centreCm[1], dz = p[2] - r.centreCm[2];
                        if (dx * dx + dy * dy + dz * dz <= r.radiusCm * r.radiusCm) pin[i] = 1;
                        break;
                    }
                    case PaintRule::Type::Mask: {
                        f32 mr = 1.0f, mg = 1.0f;
                        if (!env.mask || !mesh.uvs || !env.mask(r.texture, mesh.uvs[v * 2 + 0], mesh.uvs[v * 2 + 1], mr, mg)) break;
                        if (mr < r.pinBelow) pin[i] = 1;
                        else reach[i] = mg >= 0.999f ? -1.0f : mg * 2.0f * env.defaultReachCm * env.uniformScale;
                        break;
                    }
                }
            }
        }
        if (paint.rules.empty() && !paint.hasPin() && env.defaultTopPin) {
            f32 lo = 1e30f, hi = -1e30f;
            for (u32 i = 0; i < n; ++i) {
                if (rep[i] == kNone) continue;
                const f32 z = mesh.positions[static_cast<usize>(rep[i]) * 3 + 2];
                lo = std::min(lo, z); hi = std::max(hi, z);
            }
            const f32 cut = hi - 0.02f * (hi - lo) - 1e-4f;
            for (u32 i = 0; i < n; ++i)
                if (rep[i] != kNone && mesh.positions[static_cast<usize>(rep[i]) * 3 + 2] >= cut) pin[i] = 1;
        }
    }

    u32 pinned = 0;
    for (u32 i = 0; i < n; ++i) {
        Particle& p = cage.particles[i];
        p.pinned = pin[i] != 0;
        p.reachCm = p.pinned ? 0.0f : reach[i];
        p.invMass = p.pinned ? 0.0f : 1.0f / std::max(p.mass, 1e-4f);
        pinned += p.pinned ? 1u : 0u;
    }
    return pinned;
}

bool buildSimMesh(const SimMeshInput& in, const SimBuildSpec& spec, SimMesh& out) {
    out = SimMesh{};
    if (!inputOk(in, out.error)) return false;
    Weld w;
    if (!weldVertices(in.positions, in.vertexCount, spec.weldCm, w)) { out.error = "mesh has a non-finite position"; return false; }
    const u32 n = static_cast<u32>(w.rep.size());
    if (n > std::min(spec.maxParticles, kMaxSimParticles)) {
        out.error = "mesh welds to " + std::to_string(n) + " particles, over the limit of " +
                    std::to_string(std::min(spec.maxParticles, kMaxSimParticles));
        return false;
    }

    Cage& cage = out.cage;
    const u32 mat = addMaterial(cage, spec.material);
    out.bindLocal.resize(n);
    out.repVertex = w.rep;
    for (u32 p = 0; p < n; ++p) {
        const f32* s = in.positions + static_cast<usize>(w.rep[p]) * 3;
        out.bindLocal[p] = Vec3{s[0], s[1], s[2]};
        addParticle(cage, transformPoint(out.bindLocal[p], spec.entityWorld), false);
    }

    // Triangles: degenerate (a collapsed edge) and duplicate (same three particles) ones are not
    // simulated; a duplicate's render triangle follows the sim triangle it repeats.
    RenderBinding& rb = out.binding;
    rb.vertexParticle = w.particleOf;
    rb.indices.assign(in.indices, in.indices + in.indexCount);
    const u32 renderTris = in.indexCount / 3;
    rb.triSim.assign(renderTris, kNone);
    std::unordered_map<u64, u32> seen;
    seen.reserve(renderTris);
    for (u32 t = 0; t < renderTris; ++t) {
        const u32 a = w.particleOf[in.indices[t * 3 + 0]];
        const u32 b = w.particleOf[in.indices[t * 3 + 1]];
        const u32 c = w.particleOf[in.indices[t * 3 + 2]];
        if (a == b || b == c || a == c) continue;
        u32 s[3] = {a, b, c};
        std::sort(s, s + 3);
        const u64 key = (static_cast<u64>(s[0]) << 32) | (static_cast<u64>(s[1]) << 16) | s[2];
        const auto it = seen.find(key);
        if (it != seen.end()) { rb.triSim[t] = it->second; continue; }
        const u32 sim = addTriangle(cage, a, b, c, mat);
        if (sim == kNone) continue;
        seen.emplace(key, sim);
        rb.triSim[t] = sim;
    }
    if (cage.triangles.empty()) { out.error = "mesh has no usable triangles after welding"; return false; }
    rb.finalize(static_cast<u32>(cage.triangles.size()));

    build(cage);

    if (in.joints && in.weights) {
        out.joint4.resize(static_cast<usize>(n) * 4);
        out.weight4.resize(static_cast<usize>(n) * 4);
        for (u32 p = 0; p < n; ++p)
            for (u32 k = 0; k < 4; ++k) {
                out.joint4[p * 4 + k] = in.joints[static_cast<usize>(w.rep[p]) * 4 + k];
                out.weight4[p * 4 + k] = in.weights[static_cast<usize>(w.rep[p]) * 4 + k];
            }
    }

    out.meshKey = simMeshKey(in, spec.weldCm);
    out.paintStale = spec.paint.stale(out.meshKey);
    PaintEnv env;
    env.mesh = &in;
    env.binding = &rb;
    env.mask = spec.mask;
    env.boneWeight = spec.boneWeight;
    env.meshKey = out.meshKey;
    env.defaultReachCm = spec.defaultReachCm;
    env.uniformScale = spec.uniformScale;
    env.defaultTopPin = !spec.destructible;
    applyPaint(cage, spec.paint, env);
    recomputeMasses(cage);
    return true;
}

} // namespace aver::softbody
