#pragma once
// Synthetic meshes shared by the trifactor tests and the build bench. Built in memory, never shipped.
#include "aver/formats/OcMesh.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace trifix {

using namespace aver;

// n x n vertex grid in the XY plane, one submesh.
inline fmt::OcMeshData makeGrid(u32 n, f32 spacing = 100.0f) {
    fmt::OcMeshData m;
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            m.positions.insert(m.positions.end(), {f32(x) * spacing, f32(y) * spacing, 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {f32(x) / f32(n - 1), f32(y) / f32(n - 1)});
        }
    for (u32 y = 0; y + 1 < n; ++y)
        for (u32 x = 0; x + 1 < n; ++x) {
            const u32 i00 = y * n + x, i10 = y * n + x + 1, i01 = (y + 1) * n + x, i11 = (y + 1) * n + x + 1;
            for (u32 i : {i00, i10, i11, i00, i11, i01}) m.indices.push_back(i);
        }
    m.submeshes.push_back({"grid", 0, 0, u32(m.indices.size()), 0, m.vertexCount()});
    m.materialSlots = {"M0"};
    return m;
}

// Closed UV sphere (segments x rings quads), one submesh.
inline fmt::OcMeshData makeSphere(u32 segments, u32 rings, f32 radius = 500.0f) {
    fmt::OcMeshData m;
    const f32 pi = 3.14159265358979f;
    for (u32 r = 0; r <= rings; ++r)
        for (u32 s = 0; s <= segments; ++s) {
            const f32 th = pi * f32(r) / f32(rings), ph = 2.0f * pi * f32(s % segments) / f32(segments);
            const f32 x = std::sin(th) * std::cos(ph), y = std::sin(th) * std::sin(ph), z = std::cos(th);
            m.positions.insert(m.positions.end(), {x * radius, y * radius, z * radius});
            m.normals.insert(m.normals.end(), {x, y, z});
            m.uvs.insert(m.uvs.end(), {f32(s) / f32(segments), f32(r) / f32(rings)});
        }
    const u32 stride = segments + 1;
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * stride + s, b = a + 1, c = a + stride, d = c + 1;
            if (r != 0) for (u32 i : {a, c, b}) m.indices.push_back(i);
            if (r + 1 != rings) for (u32 i : {b, c, d}) m.indices.push_back(i);
        }
    m.submeshes.push_back({"sphere", 0, 0, u32(m.indices.size()), 0, m.vertexCount()});
    m.materialSlots = {"M0"};
    return m;
}

// Regroups the triangles by `slotOf(triangleIndex)` into one submesh per slot (slots 0..slotCount-1,
// empty ones dropped from the table but kept as names), reordering `indices` to match.
inline void splitByMaterial(fmt::OcMeshData& m, u32 slotCount, const std::function<u32(u32)>& slotOf) {
    std::vector<std::vector<u32>> per(slotCount);
    for (u32 t = 0; t < m.indices.size() / 3; ++t) {
        auto& v = per[slotOf(t) % slotCount];
        v.insert(v.end(), {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]});
    }
    m.indices.clear();
    m.submeshes.clear();
    m.materialSlots.clear();
    for (u32 s = 0; s < slotCount; ++s) {
        m.materialSlots.push_back("M" + std::to_string(s));
        if (per[s].empty()) continue;
        fmt::OcMeshSubmesh sm;
        sm.name = "sub" + std::to_string(s);
        sm.materialSlot = s;
        sm.indexStart = u32(m.indices.size());
        sm.indexCount = u32(per[s].size());
        sm.vertexCount = m.vertexCount();
        m.indices.insert(m.indices.end(), per[s].begin(), per[s].end());
        m.submeshes.push_back(sm);
    }
}

// Disconnected quads far from a base mesh: small shells, as foliage cards are.
inline void addQuadFragments(fmt::OcMeshData& m, u32 count) {
    for (u32 f = 0; f < count; ++f) {
        const f32 ox = 100000.0f + f32(f % 40) * 1000.0f, oy = 100000.0f + f32(f / 40) * 1000.0f;
        const u32 base = m.vertexCount();
        m.positions.insert(m.positions.end(), {ox, oy, 0.0f, ox + 50, oy, 0.0f, ox + 50, oy + 50, 0.0f, ox, oy + 50, 0.0f});
        m.normals.insert(m.normals.end(), {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
        m.uvs.insert(m.uvs.end(), {0, 0, 1, 0, 1, 1, 0, 1});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

} // namespace trifix
