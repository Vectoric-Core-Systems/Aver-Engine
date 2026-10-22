// PrimitiveMeshesTest: every built-in primitive (aver/rhi/PrimitiveMeshes.hpp) is unit-sized, indexes
// only its own vertices, has unit normals, winds outward, and closed shapes enclose positive volume.
#include "aver/rhi/PrimitiveMeshes.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace aver;
using namespace aver::rhi::primitives;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
}
} // namespace

int main() {
    std::set<std::string> paths;
    for (const BuiltinPrimitive& p : kBuiltinPrimitives) {
        const std::string name = p.label;
        check(paths.insert(p.path).second, name + ": path is unique");
        check(findBuiltinPrimitive(p.path) == &p, name + ": findBuiltinPrimitive");
        std::vector<rhi::MeshVertex> v;
        std::vector<u32> idx;
        p.build(v, idx);
        check(!v.empty() && idx.size() >= 3 && idx.size() % 3 == 0, name + ": has triangles");
        bool inRange = true, unitNormals = true, inside = true;
        for (u32 i : idx) inRange &= i < v.size();
        for (const rhi::MeshVertex& m : v) {
            unitNormals &= std::fabs(std::sqrt(m.nx * m.nx + m.ny * m.ny + m.nz * m.nz) - 1.0f) < 1e-3f;
            inside &= std::fabs(m.px) <= 1.0f + 1e-5f && std::fabs(m.py) <= 1.0f + 1e-5f && std::fabs(m.pz) <= 1.0f + 1e-5f;
        }
        check(inRange, name + ": indices in range");
        check(unitNormals, name + ": unit normals");
        check(inside, name + ": fits the unit cube");
        if (!inRange) continue;

        // Winding agrees with the normals; the divergence theorem gives the enclosed volume.
        u32 against = 0;
        f64 volume = 0.0;
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            const rhi::MeshVertex& a = v[idx[t]];
            const rhi::MeshVertex& b = v[idx[t + 1]];
            const rhi::MeshVertex& c = v[idx[t + 2]];
            const f32 ex = b.px - a.px, ey = b.py - a.py, ez = b.pz - a.pz;
            const f32 fx = c.px - a.px, fy = c.py - a.py, fz = c.pz - a.pz;
            const f32 nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
            if (nx * (a.nx + b.nx + c.nx) + ny * (a.ny + b.ny + c.ny) + nz * (a.nz + b.nz + c.nz) < 0.0f) ++against;
            volume += (a.px * (b.py * c.pz - b.pz * c.py) - a.py * (b.px * c.pz - b.pz * c.px) +
                       a.pz * (b.px * c.py - b.py * c.px)) / 6.0;
        }
        check(against == 0, name + ": " + std::to_string(against) + " triangle(s) wound against their normals");
        if (name != "Plane") check(volume > 0.0, name + ": encloses positive volume (" + std::to_string(volume) + ")");

        f32 mn[3], mx[3];
        primitiveBounds(v, mn, mx);
        bool thick = true;
        for (int k = 0; k < 3; ++k) thick &= mx[k] - mn[k] >= 0.02f - 1e-6f;
        check(thick, name + ": bounds are never zero-thick");
    }

    // Known volumes (unit-sized shapes): a tessellated solid is a little under the exact figure.
    const auto volumeOf = [](const char* path) {
        std::vector<rhi::MeshVertex> v; std::vector<u32> idx;
        findBuiltinPrimitive(path)->build(v, idx);
        f64 vol = 0.0;
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            const auto& a = v[idx[t]]; const auto& b = v[idx[t + 1]]; const auto& c = v[idx[t + 2]];
            vol += (a.px * (b.py * c.pz - b.pz * c.py) - a.py * (b.px * c.pz - b.pz * c.px) + a.pz * (b.px * c.py - b.py * c.px)) / 6.0;
        }
        return vol;
    };
    const f64 pi = 3.14159265358979;
    const auto near = [](f64 got, f64 want, f64 tol) { return std::fabs(got - want) <= tol * want; };
    check(near(volumeOf("Meshes/cylinder.ocmesh"), 2.0 * pi, 0.02), "cylinder volume");
    check(near(volumeOf("Meshes/cone.ocmesh"), 2.0 * pi / 3.0, 0.02), "cone volume");
    check(near(volumeOf("Meshes/pyramid.ocmesh"), 8.0 / 3.0, 1e-4), "pyramid volume");
    check(near(volumeOf("Meshes/wedge.ocmesh"), 4.0, 1e-4), "wedge volume");
    check(near(volumeOf("Meshes/tube.ocmesh"), 2.0 * pi * (1.0 - 0.5625), 0.02), "tube volume");
    check(near(volumeOf("Meshes/torus.ocmesh"), 2.0 * pi * pi * 0.7 * 0.09, 0.03), "torus volume");
    check(near(volumeOf("Meshes/capsule.ocmesh"), pi * 0.25 * 1.0 + 4.0 / 3.0 * pi * 0.125, 0.03), "capsule volume");
    // A banana is about 18 cm long and 4 cm thick at the editor's real-size scale of 10 cm per unit.
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> idx;
        const BuiltinPrimitive* b = findBuiltinPrimitive("Meshes/banana.ocmesh");
        b->build(v, idx);
        f32 mn[3], mx[3];
        primitiveBounds(v, mn, mx);
        const f32 len = (mx[0] - mn[0]) * b->spawnScale, thick = (mx[1] - mn[1]) * b->spawnScale;
        check(len > 16.0f && len < 21.0f, "banana length " + std::to_string(len) + " cm");
        check(thick > 3.0f && thick < 5.0f, "banana thickness " + std::to_string(thick) + " cm");
        check(mx[2] - mn[2] > thick / b->spawnScale, "banana is curved");
        check(b->look && std::string(b->look) == "M_Banana", "banana is yellow");
    }

    std::printf(g_fail ? "PrimitiveMeshesTest: %d failure(s)\n" : "PrimitiveMeshesTest: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
