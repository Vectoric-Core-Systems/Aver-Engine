// Avoidance for NavServer: Godot's agents and obstacles on the ORCA core the crowd already uses (CrowdOrca.hpp).
// An agent that had its velocity set since the last step gets a safe velocity; nothing moves by itself, as in Godot
// (the agent node reads the answer and moves its body).
#include "aver/synapse/CrowdOrca.hpp"
#include "aver/synapse/NavMesh.hpp"

#include <algorithm>
#include <cmath>

namespace aver::synapse {
namespace {

V2 xy(const Vec3& v) { return {v.x, v.y}; }

V2 closestOnSeg2(V2 p, V2 a, V2 b) {
    const V2 ab = b - a;
    const f32 l2 = lenSq2(ab);
    if (l2 < 1e-9f) return a;
    return a + ab * std::clamp(dot2(p - a, ab) / l2, 0.0f, 1.0f);
}

// Whether the vertical ranges [aLo, aHi] and [bLo, bHi] overlap (a height of 0 or less means "unbounded upwards").
bool heightsOverlap(f32 aZ, f32 aH, f32 bZ, f32 bH) {
    const f32 aHi = aH > 0 ? aZ + aH : 1e30f, bHi = bH > 0 ? bZ + bH : 1e30f;
    return aZ <= bHi && bZ <= aHi;
}

}  // namespace

void NavServer::stepAvoidance(u32 map, f32 dt) {
    std::vector<NavAgentData*> agents;
    std::vector<NavObstacleData*> obstacles;
    for (auto& [id, a] : agents_) if (a.map == map && a.avoidance && !a.paused) agents.push_back(&a);
    for (auto& [id, o] : obstacles_) if (o.map == map && o.avoidance && !o.paused) obstacles.push_back(&o);

    OrcaScratch scratch;
    std::vector<OrcaLine> lines;
    std::vector<std::pair<f32, usize>> near;
    for (NavAgentData* a : agents) {
        if (!a->hasVelocity) continue;
        a->hasVelocity = false;
        const V2 pos = xy(a->position);
        const V2 pref = xy(a->velocity);
        lines.clear();

        // Walls first (never relaxed).
        const f32 wallHorizon = std::max(a->timeHorizonObstacles, 0.1f);
        const f32 reach = a->radius + a->maxSpeed * wallHorizon;
        for (NavObstacleData* o : obstacles) {
            if (o->verts.size() < 3 || (o->layers & a->avoidanceMask) == 0) continue;   // a wall needs a polygon, as in Godot
            f32 zMin = o->verts[0].z;
            for (const Vec3& v : o->verts) zMin = std::min(zMin, v.z);
            if (!heightsOverlap(a->position.z, a->height, zMin, o->height)) continue;
            const usize n = o->verts.size();
            // Winding picks the side agents may be on: counter-clockwise keeps them inside, clockwise keeps them out.
            {
                bool inside = false;
                for (usize i = 0, j = n - 1; i < n; j = i++) {
                    const V2 pa = xy(o->verts[i]), pb = xy(o->verts[j]);
                    if (((pa.y > pos.y) != (pb.y > pos.y)) && (pos.x < (pb.x - pa.x) * (pos.y - pa.y) / (pb.y - pa.y) + pa.x)) inside = !inside;
                }
                if (!inside && navSignedAreaXY(o->verts) > 0.0f) continue;   // kept inside, and already out: nothing to hold it
                // clockwise: edges whose left side holds the agent push it out; an agent already inside has none to lean on
            }
            for (usize i = 0; i < n; ++i) {
                const V2 va = xy(o->verts[i]), vb = xy(o->verts[(i + 1) % n]);
                const V2 d = vb - va;
                if (len2(d) < 1e-4f) continue;
                if (det2(d, pos - va) < 0.0f) continue;   // agents live on the left of an edge; this one is on the other side
                const V2 c = closestOnSeg2(pos, va, vb);
                if (dist2(pos, c) > reach) continue;
                lines.push_back(orcaWallLine(pos, a->radius, c, perp2(norm2(d)), wallHorizon));
            }
        }
        const u32 hard = static_cast<u32>(lines.size());

        // Round obstacles behave like agents that never give way.
        for (NavObstacleData* o : obstacles) {
            if (!o->verts.empty() || o->radius <= 0.0f || (o->layers & a->avoidanceMask) == 0) continue;
            if (!heightsOverlap(a->position.z, a->height, o->position.z, o->height)) continue;
            if (dist2(pos, xy(o->position)) > a->neighborDistance + o->radius) continue;
            lines.push_back(orcaAgentLine(pos, pref, a->radius, xy(o->position), xy(o->velocity), o->radius, 1.0f,
                                          std::max(a->timeHorizonAgents, 0.1f), dt));
        }

        // Other agents, nearest first.
        near.clear();
        for (usize i = 0; i < agents.size(); ++i) {
            const NavAgentData* b = agents[i];
            if (b == a) continue;
            if ((a->avoidanceMask & b->avoidanceLayers) == 0) continue;
            if (!heightsOverlap(a->position.z, a->height, b->position.z, b->height)) continue;
            const f32 d = dist2(pos, xy(b->position));
            if (d > a->neighborDistance + b->radius) continue;
            near.emplace_back(d, i);
        }
        std::sort(near.begin(), near.end(), [&](const auto& l, const auto& r) { return l.first < r.first || (l.first == r.first && agents[l.second]->id < agents[r.second]->id); });
        if (near.size() > a->maxNeighbors) near.resize(a->maxNeighbors);
        for (const auto& [d, i] : near) {
            const NavAgentData* b = agents[i];
            // The one with the lower priority gives way more.
            const f32 share = std::clamp(0.5f + 0.5f * (b->priority - a->priority), 0.0f, 1.0f);
            lines.push_back(orcaAgentLine(pos, pref, a->radius, xy(b->position), xy(b->velocity), b->radius, share,
                                          std::max(a->timeHorizonAgents, 0.1f), dt));
        }

        const V2 v = orcaSolve(lines, hard, pref, a->maxSpeed, scratch);
        a->safeVelocity = {v.x, v.y, a->velocity.z};
        a->hasSafe = true;
    }
}

}  // namespace aver::synapse
