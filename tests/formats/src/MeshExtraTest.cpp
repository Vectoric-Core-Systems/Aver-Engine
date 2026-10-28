// MeshExtraTest -- G4 R1, the second vertex stream (vertex colour, authored tangent, UV2) below the renderer: no GPU, no window.
//
//   - rhi::MeshVertexExtra: the 16-byte slot-1 packing the shaders decode (colour UNORM8, tangent xyz*0.5+0.5 with w in {255, 0, ~128}, UV2).
//   - .ocmesh: a mesh without the streams writes the chunks it always wrote (no VXEX, content version 1) and loads unchanged; with them it
//     round-trips colour (to 1/255), tangent and UV1, sets the MeshFlags bits, and refuses a damaged VXEX; a file whose VXEX was cut off is a
//     plain old file to the reader.
//   - the glTF importer on tests/fabric/data/ExtraGame/models/quad.gltf (COLOR_0, TANGENT, TEXCOORD_1 in an embedded base64 buffer): the
//     values through the axis map, the handedness re-derived for the engine's UVs, and a primitive without them leaving the streams empty.
//   - the OBJ importer's `v x y z r g b`, and generateTangents / packTangent on their own contract.
//   - the cluster simplifier keeping the streams in step with the vertices it compacts.
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/MeshVertexExtra.hpp"
#if AVER_MESHEXTRA_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

// A unit cube: 24 vertices, 36 indices, one submesh.
static fmt::OcMeshData makeCube() {
    fmt::OcMeshData m;
    const f32 n[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    const f32 p[6][4][3] = {
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},
        {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{ 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}},
        {{-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}},
        {{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}},
        {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
    };
    const f32 uv[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (int f = 0; f < 6; ++f) {
        const u32 base = static_cast<u32>(m.positions.size() / 3);
        for (int v = 0; v < 4; ++v) {
            m.positions.insert(m.positions.end(), {p[f][v][0], p[f][v][1], p[f][v][2]});
            m.normals.insert(m.normals.end(), {n[f][0], n[f][1], n[f][2]});
            m.uvs.insert(m.uvs.end(), {uv[v][0], uv[v][1]});
        }
        for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    m.submeshes.push_back(fmt::OcMeshSubmesh{"all", 0, 0, 36, 0, 24});
    m.materialSlots = {"M_Cube"};
    return m;
}

// The cube with all three extra streams: colour in 1/255 steps, a tangent per vertex except every sixth (the "none" vertex), UV1 = UV0 * 0.5.
static fmt::OcMeshData makeCubeWithExtras() {
    fmt::OcMeshData m = makeCube();
    const u32 vc = m.vertexCount();
    for (u32 i = 0; i < vc; ++i) {
        m.colors.insert(m.colors.end(), {f32(i * 10 % 256) / 255.0f, f32(255 - i * 10 % 256) / 255.0f, f32(i) / 255.0f, (i % 3 == 0) ? 1.0f : 128.0f / 255.0f});
        m.uvs1.insert(m.uvs1.end(), {m.uvs[i * 2] * 0.5f, m.uvs[i * 2 + 1] * 0.5f + 0.125f});
    }
    fmt::generateTangents(m);
    for (u32 i = 0; i < vc; i += 6) m.tangents[i * 4] = m.tangents[i * 4 + 1] = m.tangents[i * 4 + 2] = m.tangents[i * 4 + 3] = 0.0f;
    return m;
}

int main() {
    AVER_INFO("=== rhi::MeshVertexExtra packing ===");
    {
        static_assert(sizeof(rhi::MeshVertexExtra) == 16, "the slot-1 stride");
        const f32 col[4] = {1.0f, 0.5f, 0.0f, 0.25f};
        const f32 tan[4] = {0.0f, 1.0f, 0.0f, -1.0f};
        const f32 uv[2] = {0.125f, 0.875f};
        const rhi::MeshVertexExtra e = rhi::packMeshVertexExtra(col, tan, uv);
        check(e.color[0] == 255 && e.color[1] == 128 && e.color[2] == 0 && e.color[3] == 64, "colour is RGBA8 UNORM, rounded to nearest");
        check(e.tangent[3] == 0, "handedness -1 is 0 in the UNORM alpha");
        f32 back[4];
        check(rhi::unpackExtraTangent(e, back) && near(back[0], 0, 0.01) && near(back[1], 1, 0.01) && near(back[2], 0, 0.01) && back[3] == -1.0f,
              "the tangent decodes the way the vertex shader decodes it");
        check(e.u2 == 0.125f && e.v2 == 0.875f, "UV2 stays 32-bit float");
        const f32 tanPlus[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        check(rhi::packMeshVertexExtra(nullptr, tanPlus, nullptr).tangent[3] == 255, "handedness +1 is 255");
        const rhi::MeshVertexExtra none = rhi::packMeshVertexExtra(nullptr, nullptr, nullptr);
        const f32 zero4[4] = {0, 0, 0, 0};
        const rhi::MeshVertexExtra noneZero = rhi::packMeshVertexExtra(nullptr, zero4, nullptr);
        f32 dec[4];
        check(none.color[0] == 255 && none.color[3] == 255, "a missing colour is white");
        check(!rhi::unpackExtraTangent(none, dec) && !rhi::unpackExtraTangent(noneZero, dec), "a missing or zero tangent decodes as 'none' (the shader derives the frame)");
        check(none.tangent[3] > 100 && none.tangent[3] < 160, "'none' sits mid-range in the UNORM alpha, away from both handedness values");
        const f32 over[4] = {2.0f, -1.0f, std::nanf(""), 1.0f};
        const rhi::MeshVertexExtra clamped = rhi::packMeshVertexExtra(over, nullptr, nullptr);
        check(clamped.color[0] == 255 && clamped.color[1] == 0 && clamped.color[2] == 0, "out-of-range and NaN colour clamps rather than wrapping");
    }

    AVER_INFO("=== packTangent / generateTangents: the handedness contract ===");
    {
        // N = +Z, T along +X; increasing v along +Y -> cross(N, T) = +Y -> w = +1; along -Y -> w = -1.
        const f32 n[3] = {0, 0, 1}, t[3] = {1, 0, 0}, up[3] = {0, 1, 0}, down[3] = {0, -1, 0};
        f32 out[4];
        fmt::packTangent(n, t, up, out);
        check(near(out[0], 1) && near(out[3], 1), "v increasing along cross(N, T): w = +1");
        fmt::packTangent(n, t, down, out);
        check(near(out[0], 1) && near(out[3], -1), "v increasing against cross(N, T): w = -1");
        const f32 tSkew[3] = {1, 0, 0.5f};
        fmt::packTangent(n, tSkew, up, out);
        check(near(out[2], 0) && near(std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]), 1), "the tangent is made perpendicular to the normal and unit length");
        const f32 zero[3] = {0, 0, 0};
        fmt::packTangent(n, zero, up, out);
        check(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0, "a zero tangent is the 'none' vertex (0,0,0,0)");

        // A quad in the XY plane, u along +X, v along +Y: T = +X and w = +1 for a +Z normal; flipping v makes w = -1.
        fmt::OcMeshData q;
        q.positions = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
        q.normals   = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
        q.uvs       = {0,0, 1,0, 1,1, 0,1};
        q.indices   = {0,1,2, 0,2,3};
        check(fmt::generateTangents(q) && q.hasTangent(), "generateTangents fills a tangent per vertex");
        check(near(q.tangents[0], 1) && near(q.tangents[1], 0) && near(q.tangents[3], 1), "u along +X gives T = +X, w = +1");
        q.uvs = {0,1, 1,1, 1,0, 0,0};
        fmt::generateTangents(q);
        check(near(q.tangents[0], 1) && near(q.tangents[3], -1), "v flipped (rising toward -Y) gives w = -1");
        fmt::OcMeshData noUv = q;
        noUv.uvs.clear();
        check(!fmt::generateTangents(noUv), "a mesh with no UVs gets no tangents");
        // Degenerate UVs: every vertex shares one coordinate -> no tangent rather than NaN.
        fmt::OcMeshData flat = q;
        flat.uvs = {0.5f,0.5f, 0.5f,0.5f, 0.5f,0.5f, 0.5f,0.5f};
        fmt::generateTangents(flat);
        bool allNone = flat.hasTangent();
        for (usize i = 0; i < flat.tangents.size(); ++i) allNone = allNone && flat.tangents[i] == 0.0f;
        check(allNone, "degenerate UVs give the 'none' vertex everywhere");
    }

    AVER_INFO("=== .ocmesh without the streams is the old file ===");
    std::vector<u8> plainBytes;
    {
        const fmt::OcMeshData cube = makeCube();
        std::string why;
        check(fmt::writeOcMesh(cube, plainBytes, &why), "writes: " + why);
        fmt::Avr1File f;
        check(fmt::parseAvr1(plainBytes.data(), plainBytes.size(), f, &why), "parses as a container: " + why);
        check(f.find(fmt::avrFourCC("VXEX")) == nullptr, "no VXEX chunk");
        check(f.contentVersion == 1, "content version 1, as before");
        check(f.find(fmt::avrFourCC("MHDR")) && f.find(fmt::avrFourCC("VTXS")) && f.find(fmt::avrFourCC("IDXS")) && f.find(fmt::avrFourCC("MADR")), "the same chunks as before");
        fmt::OcMeshData back;
        check(fmt::parseOcMesh(plainBytes.data(), plainBytes.size(), back, &why), "loads: " + why);
        check(!back.hasExtra() && back.colors.empty() && back.tangents.empty() && back.uvs1.empty(), "no extra stream appears from nowhere");
        check((back.flags & (fmt::kOcMeshHasColor | fmt::kOcMeshHasUV1 | fmt::kOcMeshHasTangent)) == 0, "and the flags say none");
        std::vector<u8> again;
        check(fmt::writeOcMesh(back, again, &why) && again == plainBytes, "rewriting it is byte-identical");
    }

    AVER_INFO("=== .ocmesh with colour, tangent and UV1 ===");
    std::vector<u8> extraBytes;
    {
        const fmt::OcMeshData cube = makeCubeWithExtras();
        std::string why;
        check(cube.valid() && cube.hasColor() && cube.hasTangent() && cube.hasUv1(), "the source mesh is valid with all three streams");
        check(fmt::writeOcMesh(cube, extraBytes, &why), "writes: " + why);
        fmt::Avr1File f;
        check(fmt::parseAvr1(extraBytes.data(), extraBytes.size(), f, &why), "parses as a container: " + why);
        const fmt::AvrChunk* vx = f.find(fmt::avrFourCC("VXEX"));
        check(vx != nullptr && f.contentVersion == 3, "a VXEX chunk, content version 3");
        check(vx != nullptr && (vx->flags & fmt::kAvrChunkRequired) == 0, "never marked required: an older reader skips it");
        check(vx != nullptr && vx->data.size() % 16 == 0, "its payload is padded to 16 bytes like the others");
        fmt::OcMeshData back;
        check(fmt::parseOcMesh(extraBytes.data(), extraBytes.size(), back, &why), "loads: " + why);
        check(back.hasColor() && back.hasTangent() && back.hasUv1() && back.valid(), "all three streams come back, valid");
        check((back.flags & fmt::kOcMeshHasColor) && (back.flags & fmt::kOcMeshHasUV1) && (back.flags & fmt::kOcMeshHasTangent), "the MeshFlags bits are set");
        bool colourExact = back.colors.size() == cube.colors.size(), uvExact = back.uvs1 == cube.uvs1, tanOk = back.tangents.size() == cube.tangents.size();
        for (usize i = 0; colourExact && i < cube.colors.size(); ++i) colourExact = near(back.colors[i], cube.colors[i], 0.5 / 255.0 + 1e-6);
        for (usize i = 0; tanOk && i < cube.tangents.size(); ++i) tanOk = near(back.tangents[i], cube.tangents[i], 1e-3);
        check(colourExact, "colour round-trips to within half a step of 1/255");
        check(uvExact, "UV1 round-trips exactly (32-bit floats)");
        check(tanOk, "tangents round-trip to snorm16, handedness exactly +-1 and the 'none' vertices still (0,0,0,0)");
        check(back.positions == cube.positions && back.indices == cube.indices, "the base streams are untouched");
        check(back.vertexCount() == 24 && back.colors.size() == 96 && back.tangents.size() == 96 && back.uvs1.size() == 48, "one entry per vertex");

        // A reader that has never heard of VXEX: the same file with the chunk removed.
        fmt::Avr1File stripped;
        stripped.subtype = f.subtype;
        stripped.contentVersion = f.contentVersion;
        stripped.flags = f.flags;
        for (const fmt::AvrChunk& c : f.chunks)
            if (c.id != fmt::avrFourCC("VXEX")) stripped.add(c.id, c.data, c.flags, c.version);
        std::vector<u8> strippedBytes;
        check(fmt::writeAvr1(stripped, strippedBytes, &why), "re-container without VXEX: " + why);
        fmt::OcMeshData old;
        check(fmt::parseOcMesh(strippedBytes.data(), strippedBytes.size(), old, &why), "a file whose VXEX is missing still loads: " + why);
        check(!old.hasExtra() && old.positions == cube.positions && old.uvs == cube.uvs, "with the base mesh intact and the streams empty");

        // Damage: a VXEX that describes another vertex count, and one cut short.
        fmt::Avr1File bad = f;
        for (fmt::AvrChunk& c : bad.chunks)
            if (c.id == fmt::avrFourCC("VXEX")) c.data[4] = 25;   // vertexCount 24 -> 25
        std::vector<u8> badBytes;
        fmt::writeAvr1(bad, badBytes, &why);
        fmt::OcMeshData rejected;
        check(!fmt::parseOcMesh(badBytes.data(), badBytes.size(), rejected, &why), "a VXEX vertex count that disagrees with the mesh is refused");
        fmt::Avr1File shortF = f;
        for (fmt::AvrChunk& c : shortF.chunks)
            if (c.id == fmt::avrFourCC("VXEX")) c.data.resize(40);
        std::vector<u8> shortBytes;
        fmt::writeAvr1(shortF, shortBytes, &why);
        check(!fmt::parseOcMesh(shortBytes.data(), shortBytes.size(), rejected, &why), "a truncated VXEX is refused: " + why);

        // A half-sized stream never reaches a file.
        fmt::OcMeshData half = cube;
        half.colors.resize(40);
        std::vector<u8> halfBytes;
        check(!half.valid() && !fmt::writeOcMesh(half, halfBytes, &why), "a colour stream not sized for the vertices is refused at write");

        // Only some streams.
        fmt::OcMeshData onlyColour = makeCube();
        onlyColour.colors.assign(24 * 4, 1.0f);
        std::vector<u8> ocBytes;
        fmt::OcMeshData ocBack;
        check(fmt::writeOcMesh(onlyColour, ocBytes, &why) && fmt::parseOcMesh(ocBytes.data(), ocBytes.size(), ocBack, &why), "colour alone round-trips: " + why);
        check(ocBack.hasColor() && !ocBack.hasTangent() && !ocBack.hasUv1() && !(ocBack.flags & fmt::kOcMeshHasTangent), "and only colour appears");
    }

    AVER_INFO("=== the glTF importer ===");
    {
        const std::string path = std::string(AVER_FABRIC_DATA) + "/ExtraGame/models/quad.gltf";
        fmt::GltfImportResult res;
        std::string why;
        check(fmt::importGltf(path, res, {}, &why), "imports the hand-written quad: " + why);
        check(res.meshes.size() == 1, "one mesh");
        if (res.meshes.size() == 1) {
            const fmt::OcMeshData& q = res.meshes[0];
            check(q.vertexCount() == 4 && q.hasColor() && q.hasTangent() && q.hasUv1() && q.valid(), "four vertices with colour, tangent and UV1");
            check(near(q.colors[0], 1) && near(q.colors[1], 0) && near(q.colors[4], 0) && near(q.colors[5], 1) && near(q.colors[10], 1) && near(q.colors[15], 0.5),
                  "COLOR_0 passes through (red, green, blue, white at half alpha)");
            check(near(q.uvs1[2], 0.75) && near(q.uvs1[3], 0.25), "TEXCOORD_1 passes through unchanged");
            check(near(q.uvs[0], 0) && near(q.uvs[1], 1), "TEXCOORD_0 is still the first set");
            // glTF tangent +X -> engine +Y; glTF normal +Z -> engine -X; w re-derived against the engine's increasing-v direction (-Z).
            check(near(q.tangents[0], 0) && near(q.tangents[1], 1) && near(q.tangents[2], 0) && near(q.tangents[3], 1), "TANGENT goes through the axis map, w = +1");
            check(near(q.normals[0], -1), "the normal took the same map");
            const f32 crossZ = q.normals[0] * q.tangents[1] - q.normals[1] * q.tangents[0];
            check(crossZ * q.tangents[3] < 0, "cross(N, T) * w points along increasing v");
            bool droppedNote = false;
            for (const std::string& u : res.unsupported) droppedNote = droppedNote || u.find("vertex colour") != std::string::npos;
            check(!droppedNote, "vertex colour is no longer reported as dropped");
        }

        // The same file with the axis change off: the glTF frame is kept, so cross(N, T) already is +Y, and v increases along -Y, so w flips.
        fmt::GltfImportResult raw;
        fmt::GltfImportOptions off;
        off.convertAxes = false;
        check(fmt::importGltf(path, raw, off, &why) && raw.meshes.size() == 1 && raw.meshes[0].hasTangent(), "imports with convertAxes off: " + why);
        if (raw.meshes.size() == 1 && raw.meshes[0].hasTangent()) {
            const fmt::OcMeshData& q = raw.meshes[0];
            check(near(q.tangents[0], 1) && near(q.tangents[3], -1), "unconverted, the handedness is -1 (glTF's bitangent points at decreasing v; ours at increasing)");
        }

        // A primitive without the attributes leaves the streams empty.
        const std::string noExtras =
            R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"scene":0,"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
            R"("buffers":[{"byteLength":44,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
        fmt::GltfImportResult plain;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(noExtras.data()), noExtras.size(), "", plain, {}, &why) && plain.meshes.size() == 1,
              "a primitive without the attributes imports: " + why);
        check(plain.meshes.size() == 1 && !plain.meshes[0].hasExtra() && plain.meshes[0].colors.empty(), "and carries no extra stream");
    }

    AVER_INFO("=== the OBJ importer: v x y z r g b ===");
    {
        const std::string obj =
            "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n"
            "f 1/1/1 2/2/1 3/3/1\n";
        fmt::ObjImportResult res;
        std::string why;
        check(fmt::importObjFromMemory(obj.data(), obj.size(), "", res, {}, &why) && res.meshes.size() == 1, "imports: " + why);
        if (res.meshes.size() == 1) {
            const fmt::OcMeshData& m = res.meshes[0];
            check(m.hasColor() && m.vertexCount() == 3, "the colour stream is kept");
            check(m.colors.size() == 12 && near(m.colors[0], 1) && near(m.colors[5], 1) && near(m.colors[10], 1) && near(m.colors[3], 1), "per-vertex rgb, alpha 1");
        }
        const std::string plainObj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        fmt::ObjImportResult res2;
        check(fmt::importObjFromMemory(plainObj.data(), plainObj.size(), "", res2, {}, &why) && res2.meshes.size() == 1 && !res2.meshes[0].hasColor(),
              "a file without colours has no colour stream");
    }

#if AVER_MESHEXTRA_TRIFACTOR
    AVER_INFO("=== simplifyMesh compacts the extra streams with the vertices ===");
    {
        // A 9 x 9 vertex grid, so the simplifier has room to remove triangles.
        fmt::OcMeshData g;
        const int N = 9;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                g.positions.insert(g.positions.end(), {f32(x), f32(y), 0.0f});
                g.normals.insert(g.normals.end(), {0, 0, 1});
                g.uvs.insert(g.uvs.end(), {f32(x) / (N - 1), f32(y) / (N - 1)});
                g.colors.insert(g.colors.end(), {f32(x) / (N - 1), f32(y) / (N - 1), 0.0f, 1.0f});
                g.uvs1.insert(g.uvs1.end(), {f32(x) / (N - 1), f32(y) / (N - 1)});
            }
        for (int y = 0; y + 1 < N; ++y)
            for (int x = 0; x + 1 < N; ++x) {
                const u32 a = u32(y * N + x), b = a + 1, c = a + N, d = c + 1;
                for (u32 i : {a, b, d, a, d, c}) g.indices.push_back(i);
            }
        fmt::generateTangents(g);
        g.submeshes.push_back(fmt::OcMeshSubmesh{"grid", 0, 0, u32(g.indices.size()), 0, g.vertexCount()});
        g.materialSlots = {"M_Grid"};
        std::string why;
        const bool ok = trifactor::simplifyMesh(g, 0.4f, &why);
        check(ok, "simplifies: " + why);
        if (ok) {
            check(g.vertexCount() < u32(N * N), "vertices were compacted");
            check(g.hasColor() && g.hasTangent() && g.hasUv1() && g.valid(), "the colour, tangent and UV1 streams were compacted with them");
            // Each surviving vertex still has the colour its position implies (colour = position / 8 on this grid).
            bool inStep = true;
            for (u32 i = 0; i < g.vertexCount(); ++i) {
                inStep = inStep && near(g.colors[i * 4], g.positions[i * 3] / (N - 1), 1e-5) && near(g.colors[i * 4 + 1], g.positions[i * 3 + 1] / (N - 1), 1e-5);
                inStep = inStep && near(g.uvs1[i * 2], g.positions[i * 3] / (N - 1), 1e-5);
            }
            check(inStep, "and each vertex kept its own values");
        }
    }
#endif

    AVER_INFO("=== {} ===", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
