#include "aver/formats/MeshBuild.hpp"

#include <algorithm>
#include <cmath>

namespace aver::fmt {

void computeSmoothNormals(const std::vector<f32>& pos, const std::vector<u32>& idx, std::vector<f32>& nrm) {
    const usize vc = pos.size() / 3;
    std::vector<double> acc(vc * 3, 0.0);
    for (usize t = 0; t + 2 < idx.size(); t += 3) {
        const u32 a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= vc || b >= vc || c >= vc) continue;
        const double e1[3] = {double(pos[usize(b) * 3]) - pos[usize(a) * 3], double(pos[usize(b) * 3 + 1]) - pos[usize(a) * 3 + 1], double(pos[usize(b) * 3 + 2]) - pos[usize(a) * 3 + 2]};
        const double e2[3] = {double(pos[usize(c) * 3]) - pos[usize(a) * 3], double(pos[usize(c) * 3 + 1]) - pos[usize(a) * 3 + 1], double(pos[usize(c) * 3 + 2]) - pos[usize(a) * 3 + 2]};
        const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};   // length = twice the area
        for (const u32 v : {a, b, c})
            for (int k = 0; k < 3; ++k) acc[usize(v) * 3 + usize(k)] += n[k];
    }
    nrm.assign(vc * 3, 0.0f);
    for (usize v = 0; v < vc; ++v) {
        const double l = std::sqrt(acc[v * 3] * acc[v * 3] + acc[v * 3 + 1] * acc[v * 3 + 1] + acc[v * 3 + 2] * acc[v * 3 + 2]);
        if (l > 1e-20) for (int k = 0; k < 3; ++k) nrm[v * 3 + usize(k)] = f32(acc[v * 3 + usize(k)] / l);
        else nrm[v * 3 + 2] = 1.0f;
    }
}

bool assembleSurfaces(const std::vector<RuntimeSurface>& surfaces, OcMeshData& out, std::string* why) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    out = OcMeshData{};
    bool anyCol = false, anyUv1 = false, anyTan = false;
    for (usize s = 0; s < surfaces.size(); ++s) {
        const RuntimeSurface& r = surfaces[s];
        const usize vc = r.pos.size() / 3;
        const std::string tag = "surface " + std::to_string(s) + ": ";
        if (r.pos.size() % 3 != 0 || vc == 0) return fail(tag + "no vertices");
        if (!r.nrm.empty() && r.nrm.size() != vc * 3) return fail(tag + "the normal array does not match the vertex count");
        if (!r.uv.empty() && r.uv.size() != vc * 2) return fail(tag + "the UV array does not match the vertex count");
        if (!r.col.empty() && r.col.size() != vc * 4) return fail(tag + "the colour array does not match the vertex count");
        if (!r.uv1.empty() && r.uv1.size() != vc * 2) return fail(tag + "the UV2 array does not match the vertex count");
        if (!r.tan.empty() && r.tan.size() != vc * 4) return fail(tag + "the tangent array does not match the vertex count");
        if (r.idx.size() % 3 != 0) return fail(tag + "the index count is not a multiple of three");
        for (const u32 i : r.idx) if (i >= vc) return fail(tag + "an index is past the vertex count");
        anyCol |= !r.col.empty(); anyUv1 |= !r.uv1.empty(); anyTan |= !r.tan.empty();
    }
    for (const RuntimeSurface& r : surfaces) {
        const u32 base = out.vertexCount();
        const usize vc = r.pos.size() / 3;
        std::vector<u32> idx = r.idx;
        if (idx.empty()) { idx.resize(vc / 3 * 3); for (usize i = 0; i < idx.size(); ++i) idx[i] = u32(i); }
        if (idx.empty()) continue;
        std::vector<f32> nrm = r.nrm;
        if (nrm.empty()) computeSmoothNormals(r.pos, idx, nrm);
        out.positions.insert(out.positions.end(), r.pos.begin(), r.pos.end());
        out.normals.insert(out.normals.end(), nrm.begin(), nrm.end());
        if (r.uv.empty()) out.uvs.insert(out.uvs.end(), vc * 2, 0.0f); else out.uvs.insert(out.uvs.end(), r.uv.begin(), r.uv.end());
        if (anyCol) { if (r.col.empty()) out.colors.insert(out.colors.end(), vc * 4, 1.0f); else out.colors.insert(out.colors.end(), r.col.begin(), r.col.end()); }
        if (anyUv1) { if (r.uv1.empty()) out.uvs1.insert(out.uvs1.end(), vc * 2, 0.0f); else out.uvs1.insert(out.uvs1.end(), r.uv1.begin(), r.uv1.end()); }
        if (anyTan) { if (r.tan.empty()) out.tangents.insert(out.tangents.end(), vc * 4, 0.0f); else out.tangents.insert(out.tangents.end(), r.tan.begin(), r.tan.end()); }
        OcMeshSubmesh sm;
        sm.materialSlot = static_cast<u32>(out.materialSlots.size());
        sm.name = r.material.empty() ? "M_Default" : r.material;
        sm.indexStart = static_cast<u32>(out.indices.size());
        sm.indexCount = static_cast<u32>(idx.size());
        sm.baseVertex = base;
        sm.vertexCount = static_cast<u32>(vc);
        out.materialSlots.push_back(sm.name);
        for (const u32 i : idx) out.indices.push_back(base + i);
        out.submeshes.push_back(std::move(sm));
    }
    if (out.indices.empty()) return fail("the mesh has no triangles");
    out.computeBounds();
    return out.valid();
}

bool bakeInstances(const OcMeshData& src, const MeshInstanceSet& set, u32 visible, u32 maxVertices, OcMeshData& out, u32* used, std::string* why) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    out = OcMeshData{};
    if (used) *used = 0;
    const u32 vc = src.vertexCount();
    if (vc == 0 || src.indices.empty()) return fail("the source mesh is empty");
    visible = std::min(visible, set.count());
    if (visible == 0) return fail("no instance is visible");
    if (maxVertices > 0 && usize(visible) * vc > maxVertices) {
        visible = std::max<u32>(1, maxVertices / vc);
        if (usize(visible) * vc > maxVertices) return fail("one instance of the mesh is already over the vertex budget");
    }
    const bool useCol = src.hasColor() || !set.colors.empty();
    const bool useTan = src.hasTangent();
    const bool useUv1 = src.hasUv1() || !set.custom.empty();
    const bool haveColors = set.colors.size() >= usize(visible) * 4, haveCustom = set.custom.size() >= usize(visible) * 4;

    // The source's submesh ranges; a mesh with none is one range over all of it.
    std::vector<OcMeshSubmesh> subs = src.submeshes;
    if (subs.empty()) {
        OcMeshSubmesh whole;
        whole.indexStart = 0;
        whole.indexCount = static_cast<u32>(src.indices.size());
        whole.vertexCount = vc;
        subs.push_back(whole);
    }
    out.materialSlots = src.materialSlots;
    if (out.materialSlots.empty()) out.materialSlots.push_back("M_Default");

    out.positions.resize(usize(visible) * vc * 3);
    out.normals.resize(usize(visible) * vc * 3);
    out.uvs.resize(usize(visible) * vc * 2);
    if (useCol) out.colors.resize(usize(visible) * vc * 4);
    if (useTan) out.tangents.resize(usize(visible) * vc * 4);
    if (useUv1) out.uvs1.resize(usize(visible) * vc * 2);
    std::vector<char> mirrored(visible, 0);
    for (u32 inst = 0; inst < visible; ++inst) {
        const f32* t = &set.transforms[usize(inst) * 12];
        // p' = p.x * row0 + p.y * row1 + p.z * row2 + row3
        const double det = double(t[0]) * (double(t[4]) * t[8] - double(t[5]) * t[7]) - double(t[1]) * (double(t[3]) * t[8] - double(t[5]) * t[6]) +
                           double(t[2]) * (double(t[3]) * t[7] - double(t[4]) * t[6]);
        mirrored[inst] = det < 0;
        const double invDet = std::fabs(det) > 1e-30 ? 1.0 / det : 1.0;
        // cof[r*3+k] = cofactor(M, r, k) / det: n' = n * cof is the inverse-transpose rule for the row-vector form.
        const double cof[9] = {
            (double(t[4]) * t[8] - double(t[5]) * t[7]) * invDet, -(double(t[3]) * t[8] - double(t[5]) * t[6]) * invDet, (double(t[3]) * t[7] - double(t[4]) * t[6]) * invDet,
            -(double(t[1]) * t[8] - double(t[2]) * t[7]) * invDet, (double(t[0]) * t[8] - double(t[2]) * t[6]) * invDet, -(double(t[0]) * t[7] - double(t[1]) * t[6]) * invDet,
            (double(t[1]) * t[5] - double(t[2]) * t[4]) * invDet, -(double(t[0]) * t[5] - double(t[2]) * t[3]) * invDet, (double(t[0]) * t[4] - double(t[1]) * t[3]) * invDet};
        for (u32 v = 0; v < vc; ++v) {
            const usize o = (usize(inst) * vc + v);
            const double x = src.positions[usize(v) * 3], y = src.positions[usize(v) * 3 + 1], z = src.positions[usize(v) * 3 + 2];
            for (int k = 0; k < 3; ++k) out.positions[o * 3 + usize(k)] = f32(x * t[k] + y * t[3 + k] + z * t[6 + k] + t[9 + k]);
            const double nx = src.normals[usize(v) * 3], ny = src.normals[usize(v) * 3 + 1], nz = src.normals[usize(v) * 3 + 2];
            double n2[3];
            for (int k = 0; k < 3; ++k) n2[k] = nx * cof[k] + ny * cof[3 + k] + nz * cof[6 + k];
            const double l = std::sqrt(n2[0] * n2[0] + n2[1] * n2[1] + n2[2] * n2[2]);
            for (int k = 0; k < 3; ++k) out.normals[o * 3 + usize(k)] = l > 1e-20 ? f32(n2[k] / l) : f32(k == 2);
            out.uvs[o * 2] = src.uvs[usize(v) * 2]; out.uvs[o * 2 + 1] = src.uvs[usize(v) * 2 + 1];
            if (useCol) {
                for (int k = 0; k < 4; ++k) {
                    const f32 a = src.hasColor() ? src.colors[usize(v) * 4 + usize(k)] : 1.0f;
                    const f32 b = haveColors ? set.colors[usize(inst) * 4 + usize(k)] : 1.0f;
                    out.colors[o * 4 + usize(k)] = a * b;
                }
            }
            if (useTan) {
                const double tx = src.tangents[usize(v) * 4], ty = src.tangents[usize(v) * 4 + 1], tz = src.tangents[usize(v) * 4 + 2];
                double t2[3];
                for (int k = 0; k < 3; ++k) t2[k] = tx * t[k] + ty * t[3 + k] + tz * t[6 + k];
                const double tl = std::sqrt(t2[0] * t2[0] + t2[1] * t2[1] + t2[2] * t2[2]);
                const bool none = src.tangents[usize(v) * 4] == 0 && src.tangents[usize(v) * 4 + 1] == 0 && src.tangents[usize(v) * 4 + 2] == 0 && src.tangents[usize(v) * 4 + 3] == 0;
                for (int k = 0; k < 3; ++k) out.tangents[o * 4 + usize(k)] = (none || tl < 1e-20) ? 0.0f : f32(t2[k] / tl);
                out.tangents[o * 4 + 3] = none ? 0.0f : src.tangents[usize(v) * 4 + 3] * (mirrored[inst] ? -1.0f : 1.0f);
            }
            if (useUv1) {
                if (haveCustom) { out.uvs1[o * 2] = set.custom[usize(inst) * 4]; out.uvs1[o * 2 + 1] = set.custom[usize(inst) * 4 + 1]; }
                else if (src.hasUv1()) { out.uvs1[o * 2] = src.uvs1[usize(v) * 2]; out.uvs1[o * 2 + 1] = src.uvs1[usize(v) * 2 + 1]; }
                else { out.uvs1[o * 2] = 0; out.uvs1[o * 2 + 1] = 0; }
            }
        }
    }
    // Triangles grouped by the source's submeshes: submesh k of every instance, then submesh k + 1.
    for (const OcMeshSubmesh& sm : subs) {
        if (usize(sm.indexStart) + sm.indexCount > src.indices.size() || sm.indexCount == 0) continue;
        OcMeshSubmesh o;
        o.name = sm.name;
        o.materialSlot = sm.materialSlot < out.materialSlots.size() ? sm.materialSlot : 0;
        o.indexStart = static_cast<u32>(out.indices.size());
        for (u32 inst = 0; inst < visible; ++inst) {
            const u32 base = inst * vc;
            for (u32 i = sm.indexStart; i + 2 < sm.indexStart + sm.indexCount; i += 3) {
                const u32 a = src.indices[i] + base, b = src.indices[i + 1] + base, c = src.indices[i + 2] + base;
                out.indices.push_back(a);
                if (mirrored[inst]) { out.indices.push_back(c); out.indices.push_back(b); }
                else { out.indices.push_back(b); out.indices.push_back(c); }
            }
        }
        o.indexCount = static_cast<u32>(out.indices.size()) - o.indexStart;
        o.baseVertex = 0;
        o.vertexCount = visible * vc;
        out.submeshes.push_back(std::move(o));
    }
    if (out.indices.empty()) return fail("the source mesh has no drawable submesh");
    out.computeBounds();
    if (used) *used = visible;
    return out.valid();
}

}  // namespace aver::fmt
