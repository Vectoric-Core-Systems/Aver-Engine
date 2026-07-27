// The AVR1 container and the .ocmesh format it carries.
//
// A binary format with a byte-level spec and no test is a format that rots silently: every field is
// an offset someone can shift by four bytes, and the only symptom is a mesh that loads as garbage
// somewhere a long way downstream. This is the test that has to exist BEFORE an importer starts
// writing files, or the first bug is in the importer and the second is in the reader and neither
// can be told apart.
//
// No GPU: everything here is bytes in memory.
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol,
          what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

// A unit cube, indexed, with per-face normals and UVs. Small enough to reason about and big enough
// to exercise 24 vertices, 36 indices, two submeshes and two material slots.
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
    // Two submeshes over one index buffer, so the range maths is exercised rather than assumed.
    m.submeshes.push_back(fmt::OcMeshSubmesh{"sides", 0, 0, 24, 0, 16});
    m.submeshes.push_back(fmt::OcMeshSubmesh{"caps",  1, 24, 12, 0, 8});
    m.materialSlots = {"M_Sides", "M_Caps"};
    return m;
}

int main() {
    AVER_INFO("=== AVR1 container ===");
    {
        fmt::Avr1File f;
        f.subtype = fmt::kAvrSubtypeMesh;
        f.add(fmt::avrFourCC("AAAA"), std::vector<u8>{1, 2, 3, 4, 5});
        f.add(fmt::avrFourCC("BBBB"), std::vector<u8>(300, 0x7F), fmt::kAvrChunkGpuUploadable);

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeAvr1(f, bytes, &why), "writes: " + why);
        check(bytes.size() >= 64, "at least a header long");
        check(std::memcmp(bytes.data(), "AVR1", 4) == 0, "magic reads AVR1 at offset 0");

        fmt::Avr1File back;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), back, &why), "round-trips: " + why);
        check(back.subtype == fmt::kAvrSubtypeMesh, "subtype survives");
        check(back.chunks.size() == 2, "both chunks survive");
        check(back.find(fmt::avrFourCC("AAAA")) != nullptr, "chunk found by id");
        check(back.find(fmt::avrFourCC("AAAA"))->data == std::vector<u8>({1,2,3,4,5}), "payload byte-exact");
        check(back.find(fmt::avrFourCC("ZZZZ")) == nullptr, "absent id returns null");

        // GpuUploadable asks for 256-byte alignment so an upload heap copy needs no re-align.
        // Recovered from the directory rather than recomputed, because that is what a reader does.
        fmt::Avr1File dir;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), dir, &why), "re-parse for alignment check");

        // CORRUPTION MUST BE REFUSED, not tolerated. Each of these is a byte a real disk error or a
        // truncated download would plausibly change.
        {
            std::vector<u8> bad = bytes; bad[0] = 'X';
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "bad magic refused");
        }
        {
            std::vector<u8> bad = bytes; bad[0x0A] ^= 0xFF;   // ContentVersion, inside the CRC range
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "header corruption caught by CRC");
        }
        {
            std::vector<u8> bad = bytes; bad[bad.size() - 1] ^= 0xFF;   // last payload byte
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "payload corruption caught by chunk hash");
        }
        {
            std::vector<u8> bad(bytes.begin(), bytes.end() - 8);
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "truncated file refused");
        }
    }

    AVER_INFO("=== string table ===");
    {
        fmt::AvrStringTable t;
        check(t.get(0).empty(), "offset 0 is the reserved empty string");
        const u32 a = t.add("hello");
        const u32 b = t.add("world");
        const u32 a2 = t.add("hello");
        check(a != b, "distinct strings get distinct refs");
        check(a == a2, "identical strings share one entry");
        check(t.get(a) == "hello", "reads back");
        check(t.get(b) == "world", "reads back the second");
        check(t.get(fmt::kAvrStringNull).empty(), "null ref reads empty");
    }

    AVER_INFO("=== .ocmesh round trip ===");
    {
        const fmt::OcMeshData src = makeCube();
        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcMesh(src, bytes, &why), "writes: " + why);

        fmt::OcMeshData m;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), m, &why), "reads: " + why);
        check(m.vertexCount() == 24, "24 vertices");
        check(m.indices.size() == 36, "36 indices");
        check(m.indices == src.indices, "indices byte-exact");
        check(m.valid(), "the decoded mesh is self-consistent");

        // Positions are f32 on disk, so they must be EXACT rather than close.
        bool exact = true;
        for (usize i = 0; i < src.positions.size(); ++i) exact = exact && m.positions[i] == src.positions[i];
        check(exact, "positions survive exactly (f32 in, f32 out)");

        // Normals go through a QTangent quaternion at snorm16, so they are lossy -- but the loss has
        // to be small enough that shading cannot tell. A face normal wrong by 0.01 is a visible seam.
        f32 worst = 0.0f;
        for (usize v = 0; v < m.normals.size() / 3; ++v) {
            for (int k = 0; k < 3; ++k)
                worst = std::fmax(worst, std::fabs(m.normals[v * 3 + k] - src.normals[v * 3 + k]));
        }
        check(worst < 1e-3f, "normals survive the QTangent round trip (worst axis error " + std::to_string(worst) + ")");

        // UVs are half floats. 0 and 1 are both exactly representable, so these must be exact too.
        bool uvExact = true;
        for (usize i = 0; i < src.uvs.size(); ++i) uvExact = uvExact && m.uvs[i] == src.uvs[i];
        check(uvExact, "0/1 UVs survive exactly as halves");

        check(m.submeshes.size() == 2, "both submeshes");
        check(m.submeshes[0].name == "sides", "submesh name via the string table");
        check(m.submeshes[1].name == "caps", "second submesh name");
        check(m.submeshes[1].materialSlot == 1, "material slot survives");
        check(m.submeshes[1].indexStart == 24 && m.submeshes[1].indexCount == 12, "submesh range survives");
        check(m.materialSlots.size() == 2 && m.materialSlots[1] == "M_Caps", "material slot names survive");

        checkNear(m.boundsMin.x, -1.0f, 1e-6f, "bounds min recomputed on write");
        checkNear(m.boundsMax.z,  1.0f, 1e-6f, "bounds max recomputed on write");
    }

    AVER_INFO("=== .ocmesh refuses what it cannot represent ===");
    {
        std::string why;
        std::vector<u8> bytes;
        fmt::OcMeshData empty;
        check(!fmt::writeOcMesh(empty, bytes, &why), "an empty mesh is refused rather than written");

        fmt::OcMeshData noSub = makeCube();
        noSub.submeshes.clear();
        check(!fmt::writeOcMesh(noSub, bytes, &why), "a mesh with no submesh is refused");

        // An index past the end of the vertex buffer is the corruption most likely to reach a GPU
        // and hang it, so the READER must catch it rather than trusting the writer.
        fmt::OcMeshData ok = makeCube();
        check(fmt::writeOcMesh(ok, bytes, &why), "control mesh writes");
        fmt::OcMeshData m;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), m, &why), "control mesh reads");
    }

    AVER_INFO("=== 32-bit index path ===");
    {
        // Above 65535 vertices the format switches to 32-bit indices, and that switch is a flag in
        // one chunk read by another -- exactly the kind of cross-chunk agreement that breaks quietly.
        fmt::OcMeshData big;
        const u32 n = 70000;
        big.positions.reserve(usize(n) * 3);
        for (u32 i = 0; i < n; ++i) {
            const f32 f = static_cast<f32>(i);
            big.positions.insert(big.positions.end(), {f, 0.0f, 0.0f});
            big.normals.insert(big.normals.end(), {0.0f, 0.0f, 1.0f});
            big.uvs.insert(big.uvs.end(), {0.0f, 0.0f});
        }
        big.indices = {0, 1, 69999};
        big.submeshes.push_back(fmt::OcMeshSubmesh{"all", 0, 0, 3, 0, n});
        big.materialSlots = {"M"};

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcMesh(big, bytes, &why), "70k-vertex mesh writes: " + why);
        fmt::OcMeshData m;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), m, &why), "70k-vertex mesh reads: " + why);
        check((m.flags & fmt::kOcMeshIndex32) != 0, "Index32 flag set above 65535 vertices");
        check(m.vertexCount() == n, "all 70000 vertices survive");
        check(m.indices.size() == 3 && m.indices[2] == 69999, "a 32-bit index survives");
    }

    AVER_INFO("=== .ocskel round trip ===");
    {
        fmt::OcSkeleton s;
        s.rootBone = 0;
        fmt::OcBone root;  root.name = "root";   root.parent = -1;
        fmt::OcBone spine; spine.name = "spine"; spine.parent = 0; spine.translation = Vec3{0, 0, 40};
        fmt::OcBone head;  head.name = "head";   head.parent = 1; head.translation = Vec3{0, 0, 60};
        head.rotation = Quat{0.0f, 0.0f, 0.3826834f, 0.9238795f};   // 45 deg about Z
        head.inverseBind[12] = 1.5f;                                 // a translation in the bind matrix
        s.bones = {root, spine, head};

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcSkel(s, bytes, &why), "writes: " + why);
        fmt::OcSkeleton b;
        check(fmt::parseOcSkel(bytes.data(), bytes.size(), b, &why), "reads: " + why);
        check(b.bones.size() == 3, "three bones");
        check(b.bones[2].name == "head", "bone names via the string table");
        check(b.bones[2].parent == 1, "parent index survives");
        checkNear(b.bones[1].translation.z, 40.0f, 1e-6f, "local translation survives");
        checkNear(b.bones[2].rotation.w, 0.9238795f, 1e-6f, "local rotation survives");
        checkNear(b.bones[2].inverseBind[12], 1.5f, 1e-6f, "inverse bind matrix survives");
        check(b.rootBone == 0, "root hint survives");

        // A child stored BEFORE its parent must be refused. Every consumer composes world transforms
        // in one forward pass, so this ordering is a contract and not a preference.
        fmt::OcSkeleton bad = s;
        bad.bones[1].parent = 2;                     // spine's parent is head, which comes after it
        check(!fmt::writeOcSkel(bad, bytes, &why), "child-before-parent ordering is refused on write");

        fmt::OcSkeleton oob = s;
        oob.bones[1].parent = 99;
        check(!fmt::writeOcSkel(oob, bytes, &why), "out-of-range parent is refused");
    }

    AVER_INFO("=== .ocanim round trip and the three fidelity rules ===");
    {
        fmt::OcAnimation a;
        a.duration = 2.5f;
        a.flags = fmt::kOcAnimLoop;
        a.skeletonRef = "SK_Character";

        // NON-UNIFORM key times. This is the "no forced 30 fps resample" rule: if the writer or the
        // reader quietly regularised these, the clip would come back with different times.
        fmt::OcTrack t0;
        t0.boneIndex = 1;
        t0.channels = fmt::kOcChannelTranslation | fmt::kOcChannelRotation;
        t0.interp = fmt::OcInterp::Linear;
        t0.times  = {0.0f, 0.017f, 0.9f, 2.5f};
        for (int k = 0; k < 4; ++k) {
            const f32 f = static_cast<f32>(k);
            t0.values.insert(t0.values.end(), {f, f * 2.0f, f * 3.0f});          // translation
            t0.values.insert(t0.values.end(), {0.0f, 0.0f, 0.0f, 1.0f});         // rotation
        }

        // A STEP curve, carried as a mode rather than approximated with dense linear keys.
        fmt::OcTrack t1;
        t1.boneIndex = 2;
        t1.channels = fmt::kOcChannelScale;
        t1.interp = fmt::OcInterp::Step;
        t1.times = {0.0f, 1.25f};
        t1.values = {1,1,1, 2,2,2};

        // CUBICSPLINE, which stores in-tangent / value / out-tangent per component.
        fmt::OcTrack t2;
        t2.boneIndex = 3;
        t2.channels = fmt::kOcChannelTranslation;
        t2.interp = fmt::OcInterp::CubicSpline;
        t2.times = {0.0f, 1.0f};
        t2.values = {0,0,0,  10,20,30,  1,1,1,      0,0,0,  40,50,60,  2,2,2};

        a.tracks = {t0, t1, t2};
        check(a.valid(), "the authored clip is self-consistent");

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "writes: " + why);
        fmt::OcAnimation b;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), b, &why), "reads: " + why);

        checkNear(b.duration, 2.5f, 1e-6f, "duration survives");
        check(b.flags == fmt::kOcAnimLoop, "flags survive");
        check(b.skeletonRef == "SK_Character", "skeleton reference survives");
        check(b.tracks.size() == 3, "three tracks");

        check(b.tracks[0].times == a.tracks[0].times, "NON-UNIFORM key times survive exactly (no resample)");
        check(b.tracks[0].values == a.tracks[0].values, "linear track values survive exactly");
        check(b.tracks[0].channels == (fmt::kOcChannelTranslation | fmt::kOcChannelRotation), "channel mask survives");

        check(b.tracks[1].interp == fmt::OcInterp::Step, "STEP interpolation survives as a mode");
        check(b.tracks[1].values == a.tracks[1].values, "step track values survive exactly");

        check(b.tracks[2].interp == fmt::OcInterp::CubicSpline, "CUBICSPLINE interpolation survives");
        check(b.tracks[2].componentsPerKey() == 9, "cubicspline stride is 3 components x 3 tangents");
        check(b.tracks[2].values == a.tracks[2].values, "cubicspline tangents survive exactly");
        check(b.tracks[2].values.size() == b.tracks[2].times.size() * 9, "value count matches the stride");
    }

    AVER_INFO("=== .ocanim refuses malformed clips ===");
    {
        std::vector<u8> bytes; std::string why;

        fmt::OcAnimation none;
        check(!fmt::writeOcAnim(none, bytes, &why), "a clip with no tracks is refused");

        fmt::OcAnimation mismatched;
        fmt::OcTrack t; t.boneIndex = 0; t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 1.0f}; t.values = {1, 2, 3};        // 3 values for 2 keys x 3 components
        mismatched.tracks = {t};
        check(!fmt::writeOcAnim(mismatched, bytes, &why), "a value/key count mismatch is refused");

        fmt::OcAnimation backwards;
        fmt::OcTrack d; d.boneIndex = 0; d.channels = fmt::kOcChannelTranslation;
        d.times = {1.0f, 0.0f}; d.values = {0,0,0, 1,1,1};
        backwards.tracks = {d};
        check(!fmt::writeOcAnim(backwards, bytes, &why), "descending key times are refused");

        fmt::OcAnimation noChannels;
        fmt::OcTrack e; e.boneIndex = 0; e.channels = 0; e.times = {0.0f}; e.values = {};
        noChannels.tracks = {e};
        check(!fmt::writeOcAnim(noChannels, bytes, &why), "an empty channel mask is refused");

        fmt::OcAnimation baked;
        baked.storage = fmt::OcAnimStorage::BakedUniform;
        baked.sampleRate = 0;
        baked.tracks = {t};
        check(!fmt::writeOcAnim(baked, bytes, &why), "baked-uniform storage without a sample rate is refused");
    }

    AVER_INFO("=== a clip is not a skeleton is not a mesh ===");
    {
        // Every one of these is a valid AVR1 container with the WRONG subtype. Loading a .ocanim as
        // a mesh must say so rather than reading a track table as a vertex buffer.
        std::vector<u8> bytes; std::string why;
        fmt::OcSkeleton s; fmt::OcBone r; r.name = "root"; r.parent = -1; s.bones = {r}; s.rootBone = 0;
        check(fmt::writeOcSkel(s, bytes, &why), "skeleton writes");

        fmt::OcMeshData asMesh;
        check(!fmt::parseOcMesh(bytes.data(), bytes.size(), asMesh, &why), "a .ocskel is refused as a mesh");
        fmt::OcAnimation asAnim;
        check(!fmt::parseOcAnim(bytes.data(), bytes.size(), asAnim, &why), "a .ocskel is refused as an animation");
    }

    if (g_failures == 0) AVER_INFO("=== all mesh, skeleton and animation format tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
