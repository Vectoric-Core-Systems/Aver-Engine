// See ChunkMesh.hpp.
#include "aver/landscape/ChunkMesh.hpp"

#include <cmath>

namespace aver::landscape {

ChunkCounts chunkCounts(u32 q) {
    ChunkCounts c;
    const u32 side = q + 1;
    c.surfaceVertices = side * side;
    c.surfaceIndices  = q * q * 2 * 3;
    c.vertices = c.surfaceVertices + 4 * side;
    c.indices  = c.surfaceIndices + 4 * q * 2 * 3;
    return c;
}

bool buildChunkMesh(const fmt::OcLandData& d, const LandscapeTree& tree, u32 nodeIndex,
                    ChunkMesh& out, f32 uvTilingCm) {
    if (nodeIndex >= tree.nodes().size()) return false;
    if (!(uvTilingCm > 0.0f)) return false;
    const LandscapeNode& n = tree.nodes()[nodeIndex];
    const u32 q = tree.nodeQuads();
    const u32 side = q + 1;
    const u32 stride = n.stride;

    const ChunkCounts counts = chunkCounts(q);
    out.vertices.clear();
    out.indices.clear();
    out.vertices.reserve(counts.vertices);
    out.indices.reserve(counts.indices);

    // Sample at the node's own stride. `heightAt` clamps out-of-range to 0, but a well-formed tree
    // never asks past the edge: the last vertex of the last node lands exactly on the final sample,
    // which is the whole reason a section is (k*q)+1 samples rather than k*q.
    auto sampleXY = [&](u32 vx, u32 vy) {
        return std::pair<u32,u32>{n.sampleX + vx * stride, n.sampleY + vy * stride};
    };

    for (u32 vy = 0; vy < side; ++vy) {
        for (u32 vx = 0; vx < side; ++vx) {
            const auto [sx, sy] = sampleXY(vx, vy);
            f32 w[3];
            d.worldAt(sx, sy, w);

            // NORMAL from central differences at the NODE's stride, not the source spacing. A coarse
            // node whose normals were computed from the fine grid would be lit as though it had detail
            // it does not draw, and the shading would visibly change at every LOD switch -- which is
            // exactly what the skirt cannot hide.
            const f32 step = d.spacingCm * static_cast<f32>(stride);
            const f32 hl = d.heightAt(sx >= stride ? sx - stride : 0, sy);
            const f32 hr = d.heightAt(sx + stride, sy);
            const f32 hd = d.heightAt(sx, sy >= stride ? sy - stride : 0);
            const f32 hu = d.heightAt(sx, sy + stride);
            // +Z up, so the gradient goes in x and y and the up component is the step. Normalised
            // because a renderer will not do it for us and an unnormalised normal reads as a material
            // that is too bright at glancing angles.
            f32 nx = -(hr - hl), ny = -(hu - hd), nz = 2.0f * step;
            const f32 len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }

            LandVertex v;
            v.px = w[0]; v.py = w[1]; v.pz = w[2];
            v.nx = nx;   v.ny = ny;   v.nz = nz;
            // WORLD-ALIGNED uv, so the parameterisation is continuous across a level change.
            v.u = w[0] / uvTilingCm;
            v.v = w[1] / uvTilingCm;
            out.vertices.push_back(v);
        }
    }

    // Surface triangles. Winding chosen to match the engine's left-handed, +Z-up convention: seen from
    // above, (v0, v2, v1) walks clockwise, which is front-facing here. A flipped winding on terrain is
    // invisible until something casts a shadow onto it, so it is worth stating rather than discovering.
    for (u32 y = 0; y < q; ++y) {
        for (u32 x = 0; x < q; ++x) {
            const u32 i0 = y * side + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + side;
            const u32 i3 = i2 + 1;
            out.indices.push_back(i0); out.indices.push_back(i2); out.indices.push_back(i1);
            out.indices.push_back(i1); out.indices.push_back(i2); out.indices.push_back(i3);
        }
    }
    out.surfaceIndexCount = static_cast<u32>(out.indices.size());
    out.skirtVertexStart = static_cast<u32>(out.vertices.size());

    // ---- the skirt ----
    //
    // A ring of duplicated rim vertices pushed DOWN by the node's skirt depth, joined to the rim by two
    // triangles per boundary quad. It is not a cosmetic band: where this node meets a coarser one, the
    // neighbour's chord can sit below this rim, and the skirt fills the wedge so no sky shows through.
    //
    // Its normals point along the surface's own normal rather than outward. The skirt is meant to be
    // unnoticeable, and a vertical wall lit as a vertical wall is a dark stripe at every LOD boundary.
    auto addSkirtEdge = [&](u32 fromX, u32 fromY, u32 dx, u32 dy) {
        const u32 base = static_cast<u32>(out.vertices.size());
        for (u32 s = 0; s < side; ++s) {
            const u32 vx = fromX + dx * s, vy = fromY + dy * s;
            LandVertex v = out.vertices[vy * side + vx];
            v.pz -= n.skirtCm;
            out.vertices.push_back(v);
        }
        for (u32 s = 0; s < q; ++s) {
            const u32 r0 = (fromY + dy * s) * side + (fromX + dx * s);
            const u32 r1 = (fromY + dy * (s+1)) * side + (fromX + dx * (s+1));
            const u32 s0 = base + s, s1 = base + s + 1;
            out.indices.push_back(r0); out.indices.push_back(s0); out.indices.push_back(r1);
            out.indices.push_back(r1); out.indices.push_back(s0); out.indices.push_back(s1);
        }
    };
    addSkirtEdge(0,        0,        1, 0);   // south rim, running +X
    addSkirtEdge(0,        q,        1, 0);   // north
    addSkirtEdge(0,        0,        0, 1);   // west,  running +Y
    addSkirtEdge(q,        0,        0, 1);   // east

    return out.vertices.size() == counts.vertices && out.indices.size() == counts.indices;
}

} // namespace aver::landscape
