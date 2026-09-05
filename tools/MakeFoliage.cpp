// Synthesises procedural foliage — a pine, an oak and a bush — as .ocmesh, the same geometry as
// .obj, and the PNG texture set they share.
//
// WHY THIS EXISTS. A forest needs tree meshes and this engine has no mesh authoring, so the routes
// are "import someone else's model" or "generate one". Marketplace foliage (Fab, Quixel, Megascans)
// is out: its licence is not permissive and it cannot live in this tree. Generating leaves no
// licence attached to the result, and the output is deterministic, so a forest built from it is
// byte-identical on every machine.
//
// ONE MESH PER TREE, NOT TWO. The renderer draws a whole mesh with a single material — RHI.hpp:348
// is drawMesh(mesh, world, baseColor, metallic, roughness), and nothing in the draw path iterates
// submeshes. So bark and leaves cannot be two material slots on one entity. Splitting each tree into
// a trunk mesh and a canopy mesh would work and would double the draw calls, which matters because
// there is no instancing either. Instead both surfaces live in ONE texture and the UVs are banded:
// trunks map into the left half, foliage into the right. One draw, correct colours.
//
// THE .OBJ IS NOT A CONVENIENCE. It gives the OBJ importer a real multi-hundred-triangle input with
// UVs and normals to be checked against, and emit() round-trips every mesh through it rather than
// assuming the basis change is its own inverse.
//
//   MakeFoliage.exe <mesh output directory> [forest level .ocworld] [texture output directory]
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

// The two UV bands. Inset from the halves so bilinear filtering and the lowest mip cannot drag bark
// across into the leaves — at a 1x1 mip the whole texture is one colour anyway, but at 4x4 an
// un-inset band bleeds visibly along every trunk.
constexpr f32 kBarkU0 = 0.02f, kBarkU1 = 0.48f;
constexpr f32 kLeafU0 = 0.52f, kLeafU1 = 0.98f;

// splitmix32 — the same hash the PCG module, the HLSL and the F# side all mirror.
u32 hash32(u32 x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x21F0AAADu;
    x = (x ^ (x >> 15)) * 0x735A2D97u;
    return x ^ (x >> 15);
}
// The TOP 24 bits over 2^24, matching pcg::float01 rather than approximating it.
f32 float01(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

struct Builder {
    fmt::OcMeshData m;
    u32 slot = 0;
    u32 runStart = 0;

    u32 addVertex(f32 px, f32 py, f32 pz, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
        const u32 i = static_cast<u32>(m.positions.size() / 3);
        m.positions.insert(m.positions.end(), {px, py, pz});
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-9f) { nx /= len; ny /= len; nz /= len; } else { nz = 1.0f; }
        m.normals.insert(m.normals.end(), {nx, ny, nz});
        m.uvs.insert(m.uvs.end(), {u, v});
        return i;
    }

    // WINDING IS DECIDED BY MEASUREMENT, NOT BY REASONING. The engine's rule is that
    // cross(b-a, c-a) points along the outward normal for a front-facing triangle — the invariant
    // every importer's generateNormals() relies on. Rather than derive the right vertex order for
    // each primitive and get one of them backwards, each triangle is emitted in whichever order
    // satisfies that invariant against a known outward reference.
    void addTri(u32 a, u32 b, u32 c, f32 rx, f32 ry, f32 rz) {
        const f32* pa = &m.positions[a * 3];
        const f32* pb = &m.positions[b * 3];
        const f32* pc = &m.positions[c * 3];
        const f32 e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const f32 e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                          e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
        if (n[0] * rx + n[1] * ry + n[2] * rz >= 0.0f) m.indices.insert(m.indices.end(), {a, b, c});
        else                                          m.indices.insert(m.indices.end(), {a, c, b});
    }

    void beginSlot(const std::string& name) {
        endSlot();
        slot = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.push_back(name);
        runStart = static_cast<u32>(m.indices.size());
    }
    void endSlot() {
        const u32 end = static_cast<u32>(m.indices.size());
        if (end == runStart || m.materialSlots.empty()) return;
        fmt::OcMeshSubmesh sm;
        sm.name         = m.materialSlots[slot];
        sm.materialSlot = slot;
        sm.indexStart   = runStart;
        sm.indexCount   = end - runStart;
        sm.baseVertex   = 0;
        sm.vertexCount  = m.vertexCount();
        m.submeshes.push_back(std::move(sm));
        runStart = end;
    }
};

// A tapered cylinder about +Z, from z0 radius r0 to z1 radius r1. Sides only: a trunk's ends are
// never seen and the triangles would be pure cost.
void addTrunk(Builder& b, f32 cx, f32 cy, f32 z0, f32 z1, f32 r0, f32 r1, u32 sides, f32 vTile) {
    std::vector<u32> lo(sides + 1), hi(sides + 1);
    // The seam duplicates its vertex so U can reach the band's far edge instead of wrapping back.
    for (u32 i = 0; i <= sides; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a = t * 2.0f * kPi;
        const f32 ca = std::cos(a), sa = std::sin(a);
        // The outward normal of a CONE is not radial: it tilts by the slope of the side.
        const f32 slope = (r0 - r1) / (z1 - z0 > 1e-6f ? z1 - z0 : 1.0f);
        const f32 u = lerp(kBarkU0, kBarkU1, t);
        lo[i] = b.addVertex(cx + ca * r0, cy + sa * r0, z0, ca, sa, slope, u, 0.0f);
        hi[i] = b.addVertex(cx + ca * r1, cy + sa * r1, z1, ca, sa, slope, u, vTile);
    }
    for (u32 i = 0; i < sides; ++i) {
        const f32 a = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(sides) * 2.0f * kPi;
        const f32 rx = std::cos(a), ry = std::sin(a);
        b.addTri(lo[i], lo[i + 1], hi[i],     rx, ry, 0.0f);
        b.addTri(hi[i], lo[i + 1], hi[i + 1], rx, ry, 0.0f);
    }
}

// A cone skirt: a ring at z0 radius r, apex at z0+h. One pine tier.
void addCone(Builder& b, f32 cx, f32 cy, f32 z0, f32 h, f32 r, u32 sides) {
    const f32 uMid = (kLeafU0 + kLeafU1) * 0.5f;
    const u32 apex = b.addVertex(cx, cy, z0 + h, 0.0f, 0.0f, 1.0f, uMid, 1.0f);
    std::vector<u32> ring(sides + 1);
    const f32 slope = r / (h > 1e-6f ? h : 1.0f);
    for (u32 i = 0; i <= sides; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a = t * 2.0f * kPi;
        ring[i] = b.addVertex(cx + std::cos(a) * r, cy + std::sin(a) * r, z0,
                              std::cos(a), std::sin(a), slope, lerp(kLeafU0, kLeafU1, t), 0.0f);
    }
    for (u32 i = 0; i < sides; ++i) {
        const f32 a = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(sides) * 2.0f * kPi;
        b.addTri(ring[i], ring[i + 1], apex, std::cos(a), std::sin(a), 0.30f);
    }
    // The underside, so a tier is not see-through from below.
    const u32 hub = b.addVertex(cx, cy, z0, 0.0f, 0.0f, -1.0f, uMid, 0.5f);
    for (u32 i = 0; i < sides; ++i) b.addTri(hub, ring[i], ring[i + 1], 0.0f, 0.0f, -1.0f);
}

// An ellipsoid, for a broadleaf canopy or a bush. Deliberately low-poly: a placeholder that should
// read as a tree at 20 m, not a hero asset.
void addBlob(Builder& b, f32 cx, f32 cy, f32 cz, f32 rx, f32 ry, f32 rz, u32 rings, u32 sectors,
             u32 seed) {
    std::vector<std::vector<u32>> grid(rings + 1);
    for (u32 i = 0; i <= rings; ++i) {
        const f32 v = static_cast<f32>(i) / static_cast<f32>(rings);
        const f32 phi = v * kPi;
        grid[i].resize(sectors + 1);
        for (u32 j = 0; j <= sectors; ++j) {
            const f32 t = static_cast<f32>(j) / static_cast<f32>(sectors);
            const f32 th = t * 2.0f * kPi;
            // Per-vertex jitter so three blobs do not read as three billiard balls. Hashed from the
            // lattice position, so it is stable and the seam vertices agree with their twins.
            const f32 jit = 0.88f + 0.24f * float01(hash32(seed ^ (i * 73856093u) ^
                                                           ((j % sectors) * 19349663u)));
            const f32 sx = std::sin(phi) * std::cos(th);
            const f32 sy = std::sin(phi) * std::sin(th);
            const f32 sz = std::cos(phi);
            grid[i][j] = b.addVertex(cx + sx * rx * jit, cy + sy * ry * jit, cz + sz * rz * jit,
                                     sx / rx, sy / ry, sz / rz, lerp(kLeafU0, kLeafU1, t), v);
        }
    }
    for (u32 i = 0; i < rings; ++i)
        for (u32 j = 0; j < sectors; ++j) {
            const f32 v = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(rings) * kPi;
            const f32 th = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(sectors) * 2.0f * kPi;
            const f32 nx = std::sin(v) * std::cos(th), ny = std::sin(v) * std::sin(th), nz = std::cos(v);
            b.addTri(grid[i][j], grid[i][j + 1], grid[i + 1][j],         nx, ny, nz);
            b.addTri(grid[i + 1][j], grid[i][j + 1], grid[i + 1][j + 1], nx, ny, nz);
        }
}

void finish(Builder& b) {
    b.endSlot();
    if (b.m.positions.size() < 3) return;
    Vec3 lo{b.m.positions[0], b.m.positions[1], b.m.positions[2]}, hi = lo;
    for (usize i = 3; i + 2 < b.m.positions.size(); i += 3) {
        lo.x = std::fmin(lo.x, b.m.positions[i]);     hi.x = std::fmax(hi.x, b.m.positions[i]);
        lo.y = std::fmin(lo.y, b.m.positions[i + 1]); hi.y = std::fmax(hi.y, b.m.positions[i + 1]);
        lo.z = std::fmin(lo.z, b.m.positions[i + 2]); hi.z = std::fmax(hi.z, b.m.positions[i + 2]);
    }
    b.m.boundsMin = lo;
    b.m.boundsMax = hi;
}

// ---- the three species -------------------------------------------------------------------------
//
// One material slot each, named M_Tree, because one entity gets one material. The bark/leaf
// distinction is carried by the UV band, not by the slot.

fmt::OcMeshData makePine(u32 seed) {
    Builder b;
    b.beginSlot("M_Tree");
    addTrunk(b, 0, 0, 0.0f, 460.0f, 14.0f, 6.0f, 8, 6.0f);
    // Four tiers, each smaller and shorter than the one below. The overlap is deliberate: a gap
    // between tiers shows the trunk through the canopy and reads as a broken model.
    const f32 z[4]   = { 90.0f, 190.0f, 280.0f, 355.0f};
    const f32 rad[4] = {185.0f, 150.0f, 110.0f,  68.0f};
    const f32 hh[4]  = {190.0f, 165.0f, 135.0f, 105.0f};
    for (int i = 0; i < 4; ++i) {
        const f32 j = 0.94f + 0.12f * float01(hash32(seed + static_cast<u32>(i)));
        addCone(b, 0, 0, z[i], hh[i] * j, rad[i] * j, 10);
    }
    finish(b);
    return b.m;
}

fmt::OcMeshData makeOak(u32 seed) {
    Builder b;
    b.beginSlot("M_Tree");
    addTrunk(b, 0, 0, 0.0f, 300.0f, 22.0f, 14.0f, 8, 4.0f);
    // Two limbs, so the silhouette is not a lollipop.
    addTrunk(b,  40.0f,  20.0f, 210.0f, 330.0f, 9.0f, 5.0f, 6, 2.0f);
    addTrunk(b, -35.0f, -25.0f, 230.0f, 340.0f, 8.0f, 4.0f, 6, 2.0f);
    addBlob(b,    0.0f,   0.0f, 400.0f, 210.0f, 200.0f, 150.0f, 7, 12, seed);
    addBlob(b,  110.0f,  60.0f, 340.0f, 130.0f, 125.0f,  95.0f, 6, 10, seed + 7919u);
    addBlob(b,  -95.0f, -70.0f, 355.0f, 120.0f, 130.0f, 100.0f, 6, 10, seed + 15838u);
    finish(b);
    return b.m;
}

fmt::OcMeshData makeBush(u32 seed) {
    Builder b;
    b.beginSlot("M_Tree");
    addBlob(b,   0.0f,  0.0f, 46.0f, 74.0f, 68.0f, 46.0f, 6, 10, seed);
    addBlob(b,  38.0f, 22.0f, 34.0f, 46.0f, 42.0f, 32.0f, 5,  8, seed + 7919u);
    finish(b);
    return b.m;
}

// ---- the shared texture set ---------------------------------------------------------------------
//
// Three maps, and the set is deliberately complete rather than base-colour-only: it is what makes
// this content exercise the colour-space split. Base colour is sRGB; metal-rough and normal are
// LINEAR. A pipeline that got that wrong would show up here as foliage that is subtly too bright.

constexpr int kTexW = 256, kTexH = 256;

// Value noise from the same hash, so the texture is reproducible with everything else.
f32 vnoise(i32 x, i32 y, u32 seed) {
    return float01(hash32(seed ^ (static_cast<u32>(x) * 73856093u) ^
                                 (static_cast<u32>(y) * 19349663u)));
}
f32 fbm(f32 x, f32 y, u32 seed, int octaves) {
    f32 sum = 0.0f, amp = 0.5f, f = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        const f32 fx = x * f, fy = y * f;
        const i32 ix = static_cast<i32>(std::floor(fx)), iy = static_cast<i32>(std::floor(fy));
        const f32 tx = fx - static_cast<f32>(ix), ty = fy - static_cast<f32>(iy);
        // Smoothstep, so the octaves do not show their lattice.
        const f32 sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const f32 n00 = vnoise(ix, iy, seed + static_cast<u32>(o));
        const f32 n10 = vnoise(ix + 1, iy, seed + static_cast<u32>(o));
        const f32 n01 = vnoise(ix, iy + 1, seed + static_cast<u32>(o));
        const f32 n11 = vnoise(ix + 1, iy + 1, seed + static_cast<u32>(o));
        sum += amp * lerp(lerp(n00, n10, sx), lerp(n01, n11, sx), sy);
        amp *= 0.5f;
        f *= 2.0f;
    }
    return sum;
}

u8 quant(f32 v) {
    const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u8>(c * 255.0f + 0.5f);
}

bool writeTextures(const std::filesystem::path& dir) {
    std::vector<u8> bc(kTexW * kTexH * 3), mr(kTexW * kTexH * 3), nm(kTexW * kTexH * 3);

    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            const usize i = (static_cast<usize>(y) * kTexW + x) * 3;
            const f32 u = static_cast<f32>(x) / static_cast<f32>(kTexW);
            const bool bark = u < 0.5f;

            f32 r, g, b, rough, height;
            if (bark) {
                // Vertical streaks: high frequency across, low frequency down, which is what makes
                // it read as bark rather than as noise.
                const f32 n = fbm(static_cast<f32>(x) * 0.16f, static_cast<f32>(y) * 0.03f, 11u, 4);
                const f32 t = 0.55f + 0.9f * n;
                r = 0.20f * t; g = 0.135f * t; b = 0.085f * t;
                rough = 0.88f + 0.10f * n;
                height = n;
            } else {
                // Mottled leaf mass, with a second lower-frequency term so there are lit and shaded
                // clumps rather than uniform static.
                const f32 n = fbm(static_cast<f32>(x) * 0.09f, static_cast<f32>(y) * 0.09f, 23u, 4);
                const f32 c = fbm(static_cast<f32>(x) * 0.025f, static_cast<f32>(y) * 0.025f, 37u, 2);
                const f32 t = 0.5f + 1.05f * n * (0.7f + 0.6f * c);
                r = 0.115f * t; g = 0.285f * t; b = 0.095f * t;
                rough = 0.70f + 0.18f * n;
                height = n * 0.6f + c * 0.4f;
            }

            bc[i + 0] = quant(r); bc[i + 1] = quant(g); bc[i + 2] = quant(b);
            // glTF packing: R occlusion, G roughness, B metallic. Nothing here is metal.
            mr[i + 0] = 255; mr[i + 1] = quant(rough); mr[i + 2] = 0;

            // A cheap normal from the height field's finite differences. Tangent space, +Z out.
            const f32 hx = (bark ? fbm(static_cast<f32>(x + 1) * 0.16f, static_cast<f32>(y) * 0.03f, 11u, 4)
                                 : fbm(static_cast<f32>(x + 1) * 0.09f, static_cast<f32>(y) * 0.09f, 23u, 4));
            const f32 hy = (bark ? fbm(static_cast<f32>(x) * 0.16f, static_cast<f32>(y + 1) * 0.03f, 11u, 4)
                                 : fbm(static_cast<f32>(x) * 0.09f, static_cast<f32>(y + 1) * 0.09f, 23u, 4));
            const f32 dx = (hx - height) * 4.0f, dy = (hy - height) * 4.0f;
            f32 nx = -dx, ny = -dy, nz = 1.0f;
            const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            nm[i + 0] = quant(nx * 0.5f + 0.5f);
            nm[i + 1] = quant(ny * 0.5f + 0.5f);
            nm[i + 2] = quant(nz * 0.5f + 0.5f);
        }
    }

    const std::string bcPath = (dir / "T_Tree_BC.png").string();
    const std::string mrPath = (dir / "T_Tree_MR.png").string();
    const std::string nmPath = (dir / "T_Tree_N.png").string();
    if (!stbi_write_png(bcPath.c_str(), kTexW, kTexH, 3, bc.data(), kTexW * 3)) return false;
    if (!stbi_write_png(mrPath.c_str(), kTexW, kTexH, 3, mr.data(), kTexW * 3)) return false;
    if (!stbi_write_png(nmPath.c_str(), kTexW, kTexH, 3, nm.data(), kTexW * 3)) return false;
    AVER_INFO("[MakeFoliage] textures: T_Tree_BC / _MR / _N  {}x{} -> {}", kTexW, kTexH, dir.string());
    return true;
}

// ---- .obj output --------------------------------------------------------------------------------
//
// The INVERSE of what ObjImport does, so a round trip lands where it started: engine (x, y, z) is
// obj (y, z, -x), lengths go back to metres, V unflips, and the winding reverses again.
bool writeObj(const std::string& path, const std::string& name, const fmt::OcMeshData& m) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "# %s - generated by MakeFoliage, engine units reversed to OBJ convention\n",
                 name.c_str());
    std::fprintf(f, "mtllib foliage.mtl\n");
    std::fprintf(f, "o %s\n", name.c_str());
    const usize vn = m.positions.size() / 3;
    for (usize i = 0; i < vn; ++i)
        std::fprintf(f, "v %.6f %.6f %.6f\n", m.positions[i * 3 + 1] * 0.01f,
                     m.positions[i * 3 + 2] * 0.01f, -m.positions[i * 3] * 0.01f);
    for (usize i = 0; i < vn; ++i)
        std::fprintf(f, "vt %.6f %.6f\n", m.uvs[i * 2], 1.0f - m.uvs[i * 2 + 1]);
    for (usize i = 0; i < vn; ++i)
        std::fprintf(f, "vn %.6f %.6f %.6f\n", m.normals[i * 3 + 1],
                     m.normals[i * 3 + 2], -m.normals[i * 3]);
    for (const fmt::OcMeshSubmesh& sm : m.submeshes) {
        std::fprintf(f, "usemtl %s\n", sm.name.c_str());
        for (u32 i = sm.indexStart; i + 2 < sm.indexStart + sm.indexCount; i += 3) {
            const u32 a = m.indices[i] + 1, b = m.indices[i + 1] + 1, c = m.indices[i + 2] + 1;
            std::fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, c, c, c, b, b, b);
        }
    }
    std::fclose(f);
    return true;
}

bool writeMtl(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f,
        "# The material the generated .obj files reference. One material, because the meshes are one\n"
        "# material each: bark and leaves are told apart by the UV band, not by the slot.\n"
        "newmtl M_Tree\n"
        "Kd 1 1 1\n"
        "Pr 0.8\n"
        "Pm 0.0\n"
        "map_Kd T_Tree_BC.png\n"
        "map_Pr T_Tree_MR.png\n"
        "norm T_Tree_N.png\n");
    std::fclose(f);
    return true;
}

// ---- the forest level ---------------------------------------------------------------------------
//
// Deterministic scatter, written as PLACE records into an .ocworld. AUTHORED DATA, not a runtime
// system: the level file is the artifact, so the forest exists in the editor, survives packaging,
// and does not depend on a script running at BeginPlay. The engine's F# scatter (Aver.Pcg +
// PcgApply) is the other route and is session-scoped by design; this one is not.

struct Placed { f32 x, y, r; };

// Rejection sampling with a minimum spacing, against accepted points only. O(n^2), which for a few
// hundred trees is trivial and, unlike a grid, does not bias the result toward cell centres.
bool tooClose(const std::vector<Placed>& acc, f32 x, f32 y, f32 r) {
    for (const Placed& p : acc) {
        const f32 dx = p.x - x, dy = p.y - y, need = p.r + r;
        if (dx * dx + dy * dy < need * need) return true;
    }
    return false;
}

void scatter(std::vector<Placed>& acc, std::vector<std::string>& lines, const char* mesh,
             const char* material, u32 seed, u32 want, f32 half, f32 spacing,
             f32 scaleMin, f32 scaleMax) {
    u32 placed = 0;
    // A hard attempt ceiling, so a spacing that cannot be satisfied ends rather than spins.
    for (u32 i = 0; i < want * 40 && placed < want; ++i) {
        const u32 h0 = hash32(seed ^ (i * 2654435761u));
        const u32 h1 = hash32(h0);
        const u32 h2 = hash32(h1);
        const u32 h3 = hash32(h2);
        const f32 x = (float01(h0) - 0.5f) * 2.0f * half;
        const f32 y = (float01(h1) - 0.5f) * 2.0f * half;
        const f32 sc = scaleMin + (scaleMax - scaleMin) * float01(h2);
        if (tooClose(acc, x, y, spacing * sc)) continue;
        acc.push_back(Placed{x, y, spacing * sc});
        const f32 yaw = float01(h3) * 360.0f;
        char buf[256];
        // nocollide: a per-tree collider would be an axis-aligned box the size of the whole canopy,
        // blocking the player metres from a trunk they can see they are not touching. Trees are
        // scenery until the engine has a capsule or a convex hull.
        std::snprintf(buf, sizeof(buf),
                      "PLACE  Meshes/%s.ocmesh %.1f %.1f 0 0 0 %.1f %.3f %s nocollide",
                      mesh, x, y, yaw, sc, material);
        lines.emplace_back(buf);
        ++placed;
    }
    AVER_INFO("[MakeFoliage] {}: {} of {} requested placed at >= {:.0f} cm spacing",
              mesh, placed, want, spacing * scaleMin);
}

bool writeForestLevel(const std::string& path, u32 seed) {
    const f32 half = 4000.0f;                 // an 80 m x 80 m clearing
    std::vector<Placed> acc;
    std::vector<std::string> lines;

    // THE SPACING IS TRUNK CLEARANCE, NOT CANOPY RADIUS. Real stands have interlocking canopies;
    // spacing them so the crowns never touch produces an orchard, not a forest.
    //
    // It is also a yield decision. Each species is rejected against everything placed before it, so
    // a radius that is generous for the first starves the ones after — an earlier pass at 260/320 cm
    // placed only 21 of 70 oaks and read as a pine monoculture.
    scatter(acc, lines, "Tree_Pine",  "M_Tree", seed,           140, half, 150.0f, 0.85f, 1.35f);
    scatter(acc, lines, "Tree_Oak",   "M_Tree", seed + 7919u,    90, half, 190.0f, 0.80f, 1.25f);
    scatter(acc, lines, "Bush_Small", "M_Tree", seed + 15838u,  260, half,  55.0f, 0.70f, 1.40f);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    // A RAW STRING LITERAL, so the level text reads as the level text. The alternative is a hundred
    // quoted lines each ending in an escape, which is unreadable and is exactly where a mangled
    // escape hides.
    std::fprintf(f, R"OCW(OCWORLD 1
# SkyForge - the forest clearing.
#
# GENERATED by tools/MakeFoliage.cpp with seed %u. Re-running that tool with the same seed
# reproduces this file exactly, so it can be regenerated rather than hand-maintained. Once written
# it is ordinary authored data, and editing it by hand is fine.
#
# Centimetres, +X forward, +Y right, +Z up, left-handed. The trees are real-scale: a pine stands
# about 4.6 m to its top tier, so the clearing is 80 m across rather than the arena's 16 m.
NAME SkyForge_Forest
BUILD 1
ALGO 3

SPAWN 0 0 20 0

# A LOW SUN, about 34 degrees. Trees are the one subject where a high sun is actively wrong:
# overhead light puts the canopy in its own shadow and the trunks in nothing, and the whole stand
# reads as green soup. A low sun rakes between the trunks and separates them.
SUN elev 34 azim 118 color 1 0.94 0.84 lux 95000

# More Mie than the arena, for the haze that makes distant trunks recede. Without that depth cue a
# forest reads as a flat wall of trees at every distance.
SKY model physical mie 7 multiscatter 1.8
FOG exp density 0.000012 color 0.66 0.72 0.78

# The sky as a seeded PCG field. A DIFFERENT SEED FROM THE ARENA on purpose: the two levels are
# meant to look like different places, and the seed is what makes one set of sky settings produce
# a different cloud field.
PCGVOLUME name Sky seed 7 cell 1600 octaves 4 floor 0.35 bias 1.5 infinite

# The ground. COLLIDING: the engine has no implicit floor at z=0, so this box is what holds the
# character up. Step off the clearing's edge and you fall.
PLACEG Meshes/cube.ocmesh 0 0 -10 0 0 0 4200 4200 10 M_Ground

# --- the forest --------------------------------------------------------------------------------
# %zu placements, scattered by rejection sampling with a per-species minimum spacing. Each is one
# draw call: there is no instancing in the RHI, so this count is the renderer's load.
)OCW",
        seed, lines.size());
    for (const std::string& l : lines) { std::fputs(l.c_str(), f); std::fputc('\n', f); }
    std::fclose(f);
    AVER_INFO("[MakeFoliage] forest level: {} placements -> {}", lines.size(), path);
    return true;
}

bool emit(const std::filesystem::path& dir, const std::string& name, const fmt::OcMeshData& m) {
    // THE .OBJ GOES IN Source/, NOT BESIDE THE .ocmesh. It is editable source, not a shipped asset,
    // and docs/PACKAGING.md drops every `**/Source/` directory when staging a game. Written next to
    // the .ocmesh it would be copied into the package: a few hundred kilobytes of text the runtime
    // cannot even load.
    std::error_code sec;
    const std::filesystem::path srcDir = dir / "Source";
    std::filesystem::create_directories(srcDir, sec);

    const std::string meshPath = (dir / (name + ".ocmesh")).string();
    const std::string objPath  = (srcDir / (name + ".obj")).string();
    std::string why;
    if (!fmt::saveOcMesh(meshPath, m, &why)) {
        AVER_ERROR("[MakeFoliage] {} could not be written: {}", meshPath, why);
        return false;
    }
    if (!writeObj(objPath, name, m)) {
        AVER_ERROR("[MakeFoliage] {} could not be written", objPath);
        return false;
    }

    // ROUND TRIP, CHECKED HERE RATHER THAN CLAIMED. writeObj undoes the importer's basis change by
    // hand, so if either side of that pair is wrong the geometry comes back moved, mirrored or wound
    // inside out. Comparing the re-import against the source is the only way to know the two
    // conversions are actually inverses — and it exercises the OBJ importer on real geometry.
    fmt::ObjImportResult back;
    std::string objWhy;
    if (!fmt::importObj(objPath, back, {}, &objWhy)) {
        AVER_ERROR("[MakeFoliage] {} did not re-import: {}", objPath, objWhy);
        return false;
    }
    if (back.meshes.size() != 1) {
        AVER_ERROR("[MakeFoliage] {} re-imported as {} meshes, expected 1", name, back.meshes.size());
        return false;
    }
    const fmt::OcMeshData& r = back.meshes[0];
    if (m.indices.size() / 3 != r.indices.size() / 3) {
        AVER_ERROR("[MakeFoliage] {} round trip changed the triangle count: {} -> {}",
                   name, m.indices.size() / 3, r.indices.size() / 3);
        return false;
    }
    // 1e-2 cm. The .obj carries six decimals of METRES, so a centimetre value survives to about
    // 1e-4 cm; a hundredth of a centimetre is well inside that and well outside noise.
    const f32 worst = std::fmax(
        std::fmax(std::fmax(std::fabs(r.boundsMin.x - m.boundsMin.x), std::fabs(r.boundsMin.y - m.boundsMin.y)),
                  std::fabs(r.boundsMin.z - m.boundsMin.z)),
        std::fmax(std::fmax(std::fabs(r.boundsMax.x - m.boundsMax.x), std::fabs(r.boundsMax.y - m.boundsMax.y)),
                  std::fabs(r.boundsMax.z - m.boundsMax.z)));
    if (worst > 1e-2f) {
        AVER_ERROR("[MakeFoliage] {} round trip moved the bounds by {:.4f} cm", name, worst);
        return false;
    }

    AVER_INFO("[MakeFoliage] {}  {} verts, {} tris  -> .ocmesh + .obj  "
              "(obj round trip OK, bounds within {:.4f} cm)",
              name, m.vertexCount(), m.indices.size() / 3, worst);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_ERROR("usage: MakeFoliage <mesh output directory> [forest level .ocworld] [texture directory]");
        AVER_INFO("  writes Tree_Pine, Tree_Oak and Bush_Small as .ocmesh and .obj, plus foliage.mtl");
        return exitCode(ExitCode::Usage);
    }
    const std::filesystem::path dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir)) {
        AVER_ERROR("[MakeFoliage] not a directory and could not be created: {}", dir.string());
        return exitCode(ExitCode::Failed);
    }

    // Fixed seeds. A generator whose output moved between runs would make every forest built from it
    // un-reproducible, which is the opposite of what the PCG side of this engine is for.
    int bad = 0;
    if (!emit(dir, "Tree_Pine",  makePine(1u)))  ++bad;
    if (!emit(dir, "Tree_Oak",   makeOak(2u)))   ++bad;
    if (!emit(dir, "Bush_Small", makeBush(3u)))  ++bad;
    // Beside the .obj files it describes, for the same reason they are in Source/.
    if (!writeMtl((dir / "Source" / "foliage.mtl").string())) {
        AVER_ERROR("[MakeFoliage] foliage.mtl failed");
        ++bad;
    }

    if (argc >= 3 && !writeForestLevel(argv[2], 20260803u)) {
        AVER_ERROR("[MakeFoliage] the forest level could not be written to {}", argv[2]);
        ++bad;
    }
    if (argc >= 4) {
        const std::filesystem::path tex = argv[3];
        std::filesystem::create_directories(tex, ec);
        if (!writeTextures(tex)) { AVER_ERROR("[MakeFoliage] the textures could not be written"); ++bad; }
    }

    if (bad) { AVER_ERROR("[MakeFoliage] {} output(s) failed", bad); return exitCode(ExitCode::Failed); }
    AVER_INFO("[MakeFoliage] done");
    return exitCode(ExitCode::Ok);
}
