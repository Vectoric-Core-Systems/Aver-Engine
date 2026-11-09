#include "aver/formats/OcMeshMerge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace aver::fmt {

namespace {

struct Box { f64 lo[3], hi[3]; };

struct WeldKey {
    i64 p[3];
    i32 n[3];
    i32 uv[2];
    bool operator==(const WeldKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
struct WeldHash {
    usize operator()(const WeldKey& k) const {
        u64 h = 1469598103934665603ull;
        const auto* b = reinterpret_cast<const unsigned char*>(&k);
        for (usize i = 0; i < sizeof k; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return static_cast<usize>(h);
    }
};

bool validMesh(const OcMeshData& m) {
    const usize v = m.positions.size() / 3;
    if (v == 0 || m.positions.size() % 3 != 0 || m.indices.size() < 3 || m.indices.size() % 3 != 0) return false;
    if (!m.normals.empty() && m.normals.size() != v * 3) return false;
    if (!m.uvs.empty() && m.uvs.size() != v * 2) return false;
    for (const u32 i : m.indices) if (i >= v) return false;
    return true;
}

Box worldBox(const OcMeshData& mesh, const f64* m) {
    Box b;
    for (int a = 0; a < 3; ++a) { b.lo[a] = 1e300; b.hi[a] = -1e300; }
    for (usize i = 0; i + 2 < mesh.positions.size(); i += 3) {
        const f64 x = mesh.positions[i], y = mesh.positions[i + 1], z = mesh.positions[i + 2];
        for (int a = 0; a < 3; ++a) {
            const f64 w = m[a * 4] * x + m[a * 4 + 1] * y + m[a * 4 + 2] * z + m[a * 4 + 3];
            b.lo[a] = std::min(b.lo[a], w); b.hi[a] = std::max(b.hi[a], w);
        }
    }
    return b;
}

f64 diagonal(const Box& b) {
    f64 s = 0;
    for (int a = 0; a < 3; ++a) s += (b.hi[a] - b.lo[a]) * (b.hi[a] - b.lo[a]);
    return std::sqrt(s);
}

bool inside(const Box& in, const Box& outer) {
    for (int a = 0; a < 3; ++a) if (in.lo[a] < outer.lo[a] || in.hi[a] > outer.hi[a]) return false;
    return true;
}

} // namespace

bool mergeStaticMeshes(const std::vector<OcMergeItem>& items, const OcMergeOptions& opt, OcMeshData& out, OcMergeReport* report,
                       std::string* why) {
    OcMergeReport rep;
    out = OcMeshData{};

    std::vector<Box> boxes(items.size());
    std::vector<u8> keep(items.size(), 0);
    for (usize i = 0; i < items.size(); ++i) {
        if (!items[i].mesh || !validMesh(*items[i].mesh)) { ++rep.skipped; continue; }
        boxes[i] = worldBox(*items[i].mesh, items[i].m);
        if (opt.dropSmallerCm > 0.0f && diagonal(boxes[i]) < static_cast<f64>(opt.dropSmallerCm)) { ++rep.droppedSmall; continue; }
        keep[i] = 1;
    }
    if (opt.dropEnclosed && items.size() <= 4096) {
        // An item is enclosed when its box sits in another kept item's box and is strictly smaller (ties keep the earlier one).
        std::vector<u8> drop(items.size(), 0);
        for (usize i = 0; i < items.size(); ++i) {
            if (!keep[i]) continue;
            for (usize j = 0; j < items.size() && !drop[i]; ++j) {
                if (j == i || !keep[j] || drop[j] || !inside(boxes[i], boxes[j])) continue;
                const f64 di = diagonal(boxes[i]), dj = diagonal(boxes[j]);
                if (di < dj || (di == dj && j < i)) drop[i] = 1;
            }
        }
        for (usize i = 0; i < items.size(); ++i) if (drop[i]) { keep[i] = 0; ++rep.droppedEnclosed; }
    }

    std::vector<std::string> names;
    std::vector<std::vector<u32>> perMaterial;
    std::unordered_map<std::string, usize> materialAt;
    std::unordered_map<WeldKey, u32, WeldHash> weld;
    const f64 cell = opt.weldCm > 0.0f ? static_cast<f64>(opt.weldCm) : 1e-6;
    const auto materialIndex = [&](const std::string& n) {
        const auto it = materialAt.find(n);
        if (it != materialAt.end()) return it->second;
        materialAt.emplace(n, names.size());
        names.push_back(n);
        perMaterial.emplace_back();
        return names.size() - 1;
    };

    for (usize it = 0; it < items.size(); ++it) {
        if (!keep[it]) continue;
        const OcMeshData& mesh = *items[it].mesh;
        const f64* m = items[it].m;
        // Normal matrix: the cofactors of the 3x3, times the sign of the determinant so a mirror keeps normals outward.
        const f64 c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
        const f64 c10 = m[2] * m[9] - m[1] * m[10], c11 = m[0] * m[10] - m[2] * m[8], c12 = m[1] * m[8] - m[0] * m[9];
        const f64 c20 = m[1] * m[6] - m[2] * m[5], c21 = m[2] * m[4] - m[0] * m[6], c22 = m[0] * m[5] - m[1] * m[4];
        const f64 det = m[0] * c00 + m[1] * c01 + m[2] * c02;
        const f64 sgn = det < 0 ? -1.0 : 1.0;
        const bool flip = det < 0;
        const usize vcount = mesh.positions.size() / 3;
        std::vector<u32> remap(vcount, ~0u);
        const auto emit = [&](u32 v) -> u32 {
            if (remap[v] != ~0u) return remap[v];
            const f64 x = mesh.positions[v * 3], y = mesh.positions[v * 3 + 1], z = mesh.positions[v * 3 + 2];
            f64 w[3];
            for (int a = 0; a < 3; ++a) w[a] = m[a * 4] * x + m[a * 4 + 1] * y + m[a * 4 + 2] * z + m[a * 4 + 3] - opt.origin[a];
            f64 n[3] = {0, 0, 1};
            if (!mesh.normals.empty()) {
                const f64 nx = mesh.normals[v * 3], ny = mesh.normals[v * 3 + 1], nz = mesh.normals[v * 3 + 2];
                n[0] = sgn * (c00 * nx + c01 * ny + c02 * nz);
                n[1] = sgn * (c10 * nx + c11 * ny + c12 * nz);
                n[2] = sgn * (c20 * nx + c21 * ny + c22 * nz);
                const f64 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-12) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[0] = 0; n[1] = 0; n[2] = 1; }
            }
            const f32 u = mesh.uvs.empty() ? 0.0f : mesh.uvs[v * 2], t = mesh.uvs.empty() ? 0.0f : mesh.uvs[v * 2 + 1];
            WeldKey k{};
            for (int a = 0; a < 3; ++a) { k.p[a] = static_cast<i64>(std::llround(w[a] / cell)); k.n[a] = static_cast<i32>(std::lround(n[a] * 64.0)); }
            k.uv[0] = static_cast<i32>(std::lround(u * 1024.0)); k.uv[1] = static_cast<i32>(std::lround(t * 1024.0));
            ++rep.verticesBeforeWeld;
            const auto f = weld.find(k);
            if (f != weld.end()) return remap[v] = f->second;
            const u32 id = static_cast<u32>(out.positions.size() / 3);
            for (int a = 0; a < 3; ++a) { out.positions.push_back(static_cast<f32>(w[a])); }
            for (int a = 0; a < 3; ++a) out.normals.push_back(static_cast<f32>(n[a]));
            out.uvs.push_back(u); out.uvs.push_back(t);
            weld.emplace(k, id);
            return remap[v] = id;
        };
        const auto slotName = [&](u32 slot) -> std::string {
            if (!items[it].material.empty()) return items[it].material;
            return slot < mesh.materialSlots.size() ? mesh.materialSlots[slot] : std::string();
        };
        const auto addRange = [&](usize start, usize count, const std::string& name) {
            std::vector<u32>& dst = perMaterial[materialIndex(name)];
            for (usize t = start; t + 3 <= start + count; t += 3) {
                const u32 a = emit(mesh.indices[t]), b = emit(mesh.indices[t + (flip ? 2 : 1)]), c = emit(mesh.indices[t + (flip ? 1 : 2)]);
                ++rep.trianglesIn;
                if (a == b || b == c || a == c) continue;
                dst.push_back(a); dst.push_back(b); dst.push_back(c);
            }
        };
        if (mesh.submeshes.empty()) {
            addRange(0, mesh.indices.size(), slotName(0));
        } else {
            for (const OcMeshSubmesh& s : mesh.submeshes) {
                if (usize(s.indexStart) + s.indexCount > mesh.indices.size()) continue;
                addRange(s.indexStart, s.indexCount, slotName(s.materialSlot));
            }
        }
        ++rep.merged;
    }

    for (usize k = 0; k < names.size(); ++k) {
        if (perMaterial[k].empty()) continue;
        OcMeshSubmesh sm;
        sm.name = names[k];
        sm.materialSlot = static_cast<u32>(out.materialSlots.size());
        sm.indexStart = static_cast<u32>(out.indices.size());
        sm.indexCount = static_cast<u32>(perMaterial[k].size());
        sm.baseVertex = 0;
        sm.vertexCount = static_cast<u32>(out.positions.size() / 3);
        out.materialSlots.push_back(names[k]);
        out.submeshes.push_back(sm);
        out.indices.insert(out.indices.end(), perMaterial[k].begin(), perMaterial[k].end());
    }
    rep.trianglesOut = static_cast<u32>(out.indices.size() / 3);
    rep.verticesOut = static_cast<u32>(out.positions.size() / 3);
    out.computeBounds();
    if (report) *report = rep;
    if (out.indices.empty()) {
        if (why) *why = "nothing to merge";
        return false;
    }
    return true;
}

} // namespace aver::fmt
