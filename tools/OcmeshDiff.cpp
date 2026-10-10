// OcmeshDiff -- compares two .ocmesh files by what they mean, not how their bytes are laid out
// (tool #7 of docs/TOOLING_COMPARISON_DRIFT.md; idea from Drift Engine's drft-diff).
//
//     OcmeshDiff.exe <a.ocmesh> <b.ocmesh> [--tol <cm>]
//
// Same geometry under a different vertex order, index order or meshlet partition counts as equal: triangles are
// compared as a multiset of quantised (--tol, default 0.01 cm) corner positions, sorted per triangle. Reported
// besides: vertex/triangle counts, submeshes (name, slot, triangles), material slots, skinning, LOD ladder
// (triangles per level), meshlet counts, bounds, flags and builder version.
// Exit code (core/ErrorCodes.hpp): Ok same meaning, Failed different, Usage a file did not load.
#include "aver/core/ErrorCodes.hpp"
#include "aver/formats/OcMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fmt;

namespace {

struct Tri { i64 v[9]; bool operator<(const Tri& o) const { return std::lexicographical_compare(v, v + 9, o.v, o.v + 9); }
             bool operator==(const Tri& o) const { return std::equal(v, v + 9, o.v); } };

std::vector<Tri> triangles(const OcMeshData& m, const std::vector<u32>& idx, f64 tol) {
    std::vector<Tri> out;
    out.reserve(idx.size() / 3);
    const usize nv = m.positions.size() / 3;
    for (usize t = 0; t + 2 < idx.size(); t += 3) {
        i64 c[3][3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const u32 i = idx[t + k];
            if (i >= nv) { ok = false; break; }
            for (int a = 0; a < 3; ++a) c[k][a] = static_cast<i64>(std::llround(m.positions[i * 3 + a] / tol));
        }
        if (!ok) continue;
        // Rotate so the smallest corner leads: keeps winding, ignores which corner the index list starts on.
        int lead = 0;
        for (int k = 1; k < 3; ++k)
            if (std::lexicographical_compare(c[k], c[k] + 3, c[lead], c[lead] + 3)) lead = k;
        Tri tri{};
        for (int k = 0; k < 3; ++k) std::memcpy(&tri.v[k * 3], c[(lead + k) % 3], sizeof c[0]);
        out.push_back(tri);
    }
    std::sort(out.begin(), out.end());
    return out;
}

int differences = 0;
template <class T> void cmp(const char* what, const T& a, const T& b) {
    if (a == b) return;
    ++differences;
    std::printf("  %-28s %s | %s\n", what, std::to_string(a).c_str(), std::to_string(b).c_str());
}
void cmpStr(const char* what, const std::string& a, const std::string& b) {
    if (a == b) return;
    ++differences;
    std::printf("  %-28s '%s' | '%s'\n", what, a.c_str(), b.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: OcmeshDiff <a.ocmesh> <b.ocmesh> [--tol <cm>]\n");
        return exitCode(ExitCode::Usage);
    }
    f64 tol = 0.01;
    for (int i = 3; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--tol")) tol = std::max(1e-6, std::atof(argv[i + 1]));

    OcMeshData a, b;
    std::string why;
    if (!loadOcMesh(argv[1], a, &why)) { std::fprintf(stderr, "%s: %s\n", argv[1], why.c_str()); return exitCode(ExitCode::Usage); }
    if (!loadOcMesh(argv[2], b, &why)) { std::fprintf(stderr, "%s: %s\n", argv[2], why.c_str()); return exitCode(ExitCode::Usage); }

    std::printf("a: %s\nb: %s\n", argv[1], argv[2]);
    cmp("vertices", a.positions.size() / 3, b.positions.size() / 3);
    cmp("triangles (LOD 0)", a.indices.size() / 3, b.indices.size() / 3);
    cmp("normals present", !a.normals.empty(), !b.normals.empty());
    cmp("uvs present", !a.uvs.empty(), !b.uvs.empty());
    cmp("skinned", !a.joints.empty(), !b.joints.empty());
    cmp("material slots", a.materialSlots.size(), b.materialSlots.size());
    for (usize i = 0; i < std::min(a.materialSlots.size(), b.materialSlots.size()); ++i)
        cmpStr(("slot " + std::to_string(i)).c_str(), a.materialSlots[i], b.materialSlots[i]);
    cmp("submeshes", a.submeshes.size(), b.submeshes.size());
    for (usize i = 0; i < std::min(a.submeshes.size(), b.submeshes.size()); ++i) {
        const std::string s = "submesh " + std::to_string(i);
        cmpStr((s + " name").c_str(), a.submeshes[i].name, b.submeshes[i].name);
        cmp((s + " slot").c_str(), a.submeshes[i].materialSlot, b.submeshes[i].materialSlot);
        cmp((s + " triangles").c_str(), a.submeshes[i].indexCount / 3, b.submeshes[i].indexCount / 3);
    }
    cmp("coarser LODs", a.coarserLods.size(), b.coarserLods.size());
    for (usize i = 0; i < std::min(a.coarserLods.size(), b.coarserLods.size()); ++i)
        cmp(("LOD " + std::to_string(i + 1) + " triangles").c_str(), a.coarserLods[i].indices.size() / 3,
            b.coarserLods[i].indices.size() / 3);
    cmp("meshlets (LOD 0)", a.meshlets.size(), b.meshlets.size());
    cmp("flags", a.flags, b.flags);
    cmp("builder version", a.builderVersion, b.builderVersion);
    const f32 bt = static_cast<f32>(tol);
    const bool boundsSame = std::fabs(a.boundsMin.x - b.boundsMin.x) <= bt && std::fabs(a.boundsMin.y - b.boundsMin.y) <= bt &&
                            std::fabs(a.boundsMin.z - b.boundsMin.z) <= bt && std::fabs(a.boundsMax.x - b.boundsMax.x) <= bt &&
                            std::fabs(a.boundsMax.y - b.boundsMax.y) <= bt && std::fabs(a.boundsMax.z - b.boundsMax.z) <= bt;
    cmp("bounds within tol", true, boundsSame);

    const std::vector<Tri> ta = triangles(a, a.indices, tol), tb = triangles(b, b.indices, tol);
    if (ta != tb) {
        std::vector<Tri> onlyA, onlyB;
        std::set_difference(ta.begin(), ta.end(), tb.begin(), tb.end(), std::back_inserter(onlyA));
        std::set_difference(tb.begin(), tb.end(), ta.begin(), ta.end(), std::back_inserter(onlyB));
        ++differences;
        std::printf("  %-28s %zu only in a, %zu only in b (of %zu | %zu)\n", "LOD 0 geometry", onlyA.size(),
                    onlyB.size(), ta.size(), tb.size());
    }
    std::printf(differences ? "DIFFERENT (%d)\n" : "SAME MEANING\n", differences);
    return exitCode(differences ? ExitCode::Failed : ExitCode::Ok);
}
