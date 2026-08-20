// The AVR1 container and the formats it carries: .ocmesh, .ocskel and .ocanim. Bytes in memory
// only; no GPU.
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Asserts two floats agree within tol, reporting both values.
static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol,
          what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

// Builds a unit cube: 24 vertices, 36 indices, two submeshes and two material slots.
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
    m.submeshes.push_back(fmt::OcMeshSubmesh{"sides", 0, 0, 24, 0, 16});
    m.submeshes.push_back(fmt::OcMeshSubmesh{"caps",  1, 24, 12, 0, 8});
    m.materialSlots = {"M_Sides", "M_Caps"};
    return m;
}

// Runs every container, mesh, skeleton and animation test. Returns 0 when they all pass.
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

        fmt::Avr1File dir;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), dir, &why), "re-parse for alignment check");

        {
            std::vector<u8> bad = bytes; bad[0] = 'X';
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "bad magic refused");
        }
        {
            std::vector<u8> bad = bytes; bad[0x0A] ^= 0xFF;
            fmt::Avr1File junk;
            check(!fmt::parseAvr1(bad.data(), bad.size(), junk, &why), "header corruption caught by CRC");
        }
        {
            std::vector<u8> bad = bytes; bad[bad.size() - 1] ^= 0xFF;
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

        bool exact = true;
        for (usize i = 0; i < src.positions.size(); ++i) exact = exact && m.positions[i] == src.positions[i];
        check(exact, "positions survive exactly (f32 in, f32 out)");

        f32 worst = 0.0f;
        for (usize v = 0; v < m.normals.size() / 3; ++v) {
            for (int k = 0; k < 3; ++k)
                worst = std::fmax(worst, std::fabs(m.normals[v * 3 + k] - src.normals[v * 3 + k]));
        }
        check(worst < 1e-3f, "normals survive the QTangent round trip (worst axis error " + std::to_string(worst) + ")");

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

    AVER_INFO("=== .ocmesh MHDR BuilderVersion (formerly Reserved) ===");
    {
        // THIS FILE, NOT TrifactorTest, IS WHERE builderVersion'S BASIC CONTRACT LIVES -- reading and
        // writing a plain u32 field needs no LodDag and no Aver.Trifactor at all (AVER_MODULE_TRIFACTOR
        // is OFF by default in this tree; see TrifactorTest.cpp's own header), so this file is the one
        // that actually runs by default and proves the on-disk contract OcMeshData::builderVersion's
        // own comment describes. aver::trifactor::packLodDag stamping the CURRENT builder's version
        // in is covered separately, in TrifactorTest.cpp, where kBuilderVersion is actually visible.
        //
        // makeCube() never touches builderVersion, so it stays at OcMeshData's own default -- this is
        // deliberately the same shape as every mesh cooked before this field existed: neither this
        // fixture nor a caller that predates packLodDag has to know the field exists to write the
        // spec's "Reserved fields are zero" byte here.
        const fmt::OcMeshData plain = makeCube();
        check(plain.builderVersion == 0, "a mesh nobody stamped defaults to builderVersion 0 (unknown/stale)");

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcMesh(plain, bytes, &why), "writes with the default (unstamped) builderVersion: " + why);
        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads it back: " + why);
        check(back.builderVersion == 0,
              "an unstamped mesh reads back as builderVersion 0 -- the same value FORMAT_SPECS.md has "
              "always required of this offset's Reserved bytes, so this feature existing changes "
              "nothing about a caller that does not know about it");

        // A caller that DID cook a ladder (packLodDag, in the real pipeline) stamps a nonzero version.
        // Simulated here without Trifactor, against an arbitrary nonzero value -- MeshTest is not the
        // place to assert what aver::trifactor::kBuilderVersion's CURRENT value is (that would make
        // this file need updating every time that constant is bumped for an unrelated reason); it only
        // needs to prove the field is a plain u32 that survives the round trip bit-exact, whatever
        // value it holds.
        fmt::OcMeshData stamped = makeCube();
        stamped.builderVersion = 7;
        std::vector<u8> stampedBytes;
        check(fmt::writeOcMesh(stamped, stampedBytes, &why), "writes with a stamped builderVersion: " + why);
        fmt::OcMeshData stampedBack;
        check(fmt::parseOcMesh(stampedBytes.data(), stampedBytes.size(), stampedBack, &why),
              "reads the stamped mesh back: " + why);
        check(stampedBack.builderVersion == 7, "a nonzero builderVersion survives the round trip bit-exact (got " +
                                                    std::to_string(stampedBack.builderVersion) + ")");
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

        fmt::OcMeshData ok = makeCube();
        check(fmt::writeOcMesh(ok, bytes, &why), "control mesh writes");
        fmt::OcMeshData m;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), m, &why), "control mesh reads");
    }

    AVER_INFO("=== 32-bit index path ===");
    {
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

    AVER_INFO("=== .ocmesh skin streams ===");
    {
        // JOINTS/WEIGHTS had a MeshFlags bit and nowhere to put the data. These are the first
        // vertices in the engine's history with a bone influence on them.
        fmt::OcMeshData src = makeCube();
        const u32 n = src.vertexCount();
        for (u32 v = 0; v < n; ++v) {
            src.joints.insert(src.joints.end(),
                              {static_cast<u16>(v % 4), static_cast<u16>((v + 1) % 4), 300, 0});
            // Deliberately NOT normalised, and deliberately including a zero: the writer has to
            // renormalise, and a zero influence has to survive as a zero rather than as noise.
            src.weights.insert(src.weights.end(), {0.5f, 0.25f, 0.25f, 0.0f});
        }
        check(src.hasSkin(), "the fixture reports itself skinned");
        check(src.valid(), "and is still a valid mesh");

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcMesh(src, bytes, &why), "a skinned mesh writes: " + why);
        fmt::OcMeshData m;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), m, &why), "and reads back: " + why);
        check((m.flags & fmt::kOcMeshHasSkin) != 0, "the HasSkin flag is set by the writer, not the caller");
        check(m.hasSkin(), "the streams come back the right size");
        check(m.vertexCount() == n, "with every vertex");

        bool joints = true, sums = true, zeroKept = true;
        f32 worst = 0.0f;
        for (u32 v = 0; v < n; ++v) {
            for (u32 k = 0; k < fmt::kOcMeshInfluences; ++k) {
                const usize i = usize(v) * fmt::kOcMeshInfluences + k;
                if (m.joints[i] != src.joints[i]) joints = false;
                worst = std::fmax(worst, std::fabs(m.weights[i] - src.weights[i]));
            }
            if (m.weights[usize(v) * fmt::kOcMeshInfluences + 3] != 0.0f) zeroKept = false;
            f32 s = 0.0f;
            for (u32 k = 0; k < fmt::kOcMeshInfluences; ++k)
                s += m.weights[usize(v) * fmt::kOcMeshInfluences + k];
            if (std::fabs(s - 1.0f) > 1.0e-6f) sums = false;
        }
        check(joints, "every joint index survives exactly, including one above 255");
        check(zeroKept, "a zero influence stays exactly zero");
        checkNear(worst, 0.0f, 1.0f / 255.0f + 1e-6f,
                  "weights round-trip to within one 255th, which is what R8G8B8A8_UNORM buys");
        check(sums, "and still sum to EXACTLY one -- the writer spends the rounding remainder");

        // An unskinned mesh must not grow a stream, or every static mesh in the project pays for it.
        fmt::OcMeshData plain = makeCube();
        std::vector<u8> plainBytes;
        check(fmt::writeOcMesh(plain, plainBytes, &why), "an unskinned mesh still writes");
        fmt::OcMeshData pm;
        check(fmt::parseOcMesh(plainBytes.data(), plainBytes.size(), pm, &why), "and reads");
        check(!pm.hasSkin() && pm.joints.empty(), "with no skin streams at all");
        check((pm.flags & fmt::kOcMeshHasSkin) == 0, "and no HasSkin flag");
        check(plainBytes.size() < bytes.size(), "and it is genuinely smaller on disk");

        // Half a skin is worse than none.
        fmt::OcMeshData half = makeCube();
        half.joints.assign(usize(half.vertexCount()) * fmt::kOcMeshInfluences, 0);
        check(!half.valid(), "joints without weights is refused rather than half-written");
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

        fmt::OcSkeleton bad = s;
        bad.bones[1].parent = 2;
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

        fmt::OcTrack t1;
        t1.boneIndex = 2;
        t1.channels = fmt::kOcChannelScale;
        t1.interp = fmt::OcInterp::Step;
        t1.times = {0.0f, 1.25f};
        t1.values = {1,1,1, 2,2,2};

        fmt::OcTrack t2;
        t2.boneIndex = 3;
        t2.channels = fmt::kOcChannelTranslation;
        t2.interp = fmt::OcInterp::CubicSpline;
        t2.times = {0.0f, 1.0f};
        t2.values = {0,0,0,  10,20,30,  1,1,1,      0,0,0,  40,50,60,  2,2,2};   // in-tangent / value / out-tangent

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

    AVER_INFO("=== .ocskel sockets: a named place on the rig ===");
    {
        fmt::OcSkeleton k;
        fmt::OcBone root;  root.name = "root";
        fmt::OcBone hand;  hand.name = "hand_r"; hand.parent = 0; hand.translation = Vec3{0, 0, 90};
        k.bones = {root, hand};
        k.rootBone = 0;
        k.sockets.push_back({"WeaponGrip", 1, Vec3{2.5f, -1.0f, 0.5f}, Quat{0, 0, 0.7071f, 0.7071f}, Vec3{1, 1, 1}});
        k.sockets.push_back({"Muzzle",     1, Vec3{40.0f, 0, 0},       Quat{0, 0, 0, 1},             Vec3{2, 2, 2}});

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcSkel(k, bytes, &why), "a rig with sockets writes: " + why);
        fmt::OcSkeleton b;
        check(fmt::parseOcSkel(bytes.data(), bytes.size(), b, &why), "and reads back: " + why);
        check(b.sockets.size() == 2, "both sockets survive");
        if (b.sockets.size() == 2) {
            check(b.sockets[0].name == "WeaponGrip" && b.sockets[0].bone == 1,
                  "the first keeps its name and its bone");
            checkNear(b.sockets[0].translation.x, 2.5f, 1e-6f, "and its offset");
            checkNear(b.sockets[0].rotation.z, 0.7071f, 1e-6f, "and its rotation");
            checkNear(b.sockets[1].scale.y, 2.0f, 1e-6f, "the second keeps a non-unit scale");
        }
        check(b.socket("Muzzle") != nullptr && b.socket("Muzzle")->bone == 1, "lookup by name works");
        check(b.socket("NoSuchThing") == nullptr, "and an unknown name is null, not the first socket");

        // THE FAILURE THIS EXISTS TO CATCH: a rig re-exported with fewer bones leaves every socket
        // past the new end pointing at nothing. Refused at write AND at load, so the bad file is
        // rejected with a message rather than indexing off the end the first time something attaches.
        fmt::OcSkeleton bad = k;
        bad.sockets[0].bone = 7;
        std::vector<u8> junk;
        check(!fmt::writeOcSkel(bad, junk, &why), "a socket naming a bone that does not exist is refused");

        // And a rig with none adds no chunk at all -- the backward-compatibility claim, by bytes.
        fmt::OcSkeleton plain = k;
        plain.sockets.clear();
        std::vector<u8> plainBytes;
        check(fmt::writeOcSkel(plain, plainBytes, &why), "the socket-free rig writes");
        check(plainBytes.size() < bytes.size(), "and is SMALLER -- no empty chunk is emitted");
        fmt::OcSkeleton backPlain;
        check(fmt::parseOcSkel(plainBytes.data(), plainBytes.size(), backPlain, &why), "and reads");
        check(backPlain.sockets.empty(), "carrying no sockets rather than one unnamed one");

        // Idempotence, for the reason the clip has it: an editor that adds a socket rewrites the
        // whole rig, so anything the parser drops is something the editor deletes.
        std::vector<u8> again;
        check(fmt::writeOcSkel(b, again, &why), "the parsed rig writes again");
        check(again == bytes, "BYTE-IDENTICAL -- a parse and a rewrite cannot quietly drop a field");
    }

    AVER_INFO("=== .ocanim notifies: a named event at a time in a clip ===");
    {
        fmt::OcAnimation a;
        a.duration = 2.0f;
        a.skeletonRef = "SK_Character";
        fmt::OcTrack t;
        t.boneIndex = 0;
        t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 2.0f};
        t.values = {0,0,0,  1,2,3};
        a.tracks = {t};
        // Deliberately NOT in time order, and with a name carrying a space and a dot: the format
        // interns names into the same string table skeletonRef uses, which has no such limit, and a
        // reader that quietly required sorted input would be a rule nothing enforces on write.
        a.notifies.push_back({1.25f, "OnFootstep.Right"});
        a.notifies.push_back({0.5f,  "OnFootstep Left"});

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "a clip with notifies writes: " + why);
        fmt::OcAnimation b;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), b, &why), "and reads back: " + why);
        check(b.notifies.size() == 2, "both notifies survive");
        if (b.notifies.size() == 2) {
            checkNear(b.notifies[0].time, 1.25f, 1e-6f, "the first notify keeps its time");
            check(b.notifies[0].name == "OnFootstep.Right", "and its name, dot included");
            checkNear(b.notifies[1].time, 0.5f, 1e-6f, "the second keeps its time");
            check(b.notifies[1].name == "OnFootstep Left", "and its name, space included");
            check(b.notifies[0].time > b.notifies[1].time,
                  "FILE ORDER IS PRESERVED, not silently sorted -- see OcAnimation::notifies");
        }
        check(b.tracks.size() == 1 && b.tracks[0].values == a.tracks[0].values,
              "and the clip itself is unaffected by carrying them");
    }

    AVER_INFO("=== .ocanim without notifies is byte-identical to before they existed ===");
    {
        // THE BACKWARD-COMPATIBILITY CLAIM, checked rather than asserted: a clip with no notifies
        // must add NO chunk, so an engine that predates NOTF sees a file it would have written
        // itself. Compared by BYTES, because "it still parses" would pass even if an empty chunk
        // were being emitted.
        fmt::OcAnimation a;
        a.duration = 1.0f;
        fmt::OcTrack t;
        t.boneIndex = 0;
        t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 1.0f};
        t.values = {0,0,0,  1,1,1};
        a.tracks = {t};

        std::vector<u8> plain; std::string why;
        check(fmt::writeOcAnim(a, plain, &why), "the notify-free clip writes: " + why);

        fmt::OcAnimation withOne = a;
        withOne.notifies.push_back({0.5f, "OnBeat"});
        std::vector<u8> marked;
        check(fmt::writeOcAnim(withOne, marked, &why), "the same clip with one notify writes");
        check(marked.size() > plain.size(), "adding a notify adds bytes (the chunk is really there)");

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(plain.data(), plain.size(), back, &why), "the notify-free clip reads");
        check(back.notifies.empty(), "and carries no notifies rather than an empty-named one");
    }

    AVER_INFO("=== .ocanim survives the round trip the ANIMATION EDITOR puts it through ===");
    {
        // WHAT THIS IS REALLY ABOUT. AnimEditor::save writes the WHOLE clip back from the struct it
        // parsed, because adding a notify is an edit to the file and there is no partial write. So
        // every field the parser does not capture is a field the editor DELETES the first time
        // somebody drops a footstep marker on a clip -- silently, on an asset with no source art.
        // Idempotence is the check that catches it: write, parse, write again, compare BYTES. If the
        // second write differs, the parse lost something the first write emitted.
        fmt::OcAnimation a;
        a.duration = 3.5f;
        a.storage = fmt::OcAnimStorage::Keyframed;
        a.flags = fmt::kOcAnimLoop;
        a.sampleRate = 0;
        a.skeletonRef = "SK_Mannequin";

        // Three tracks that differ in every way the format allows one to: which bone, which
        // channels, which interpolation, how many keys.
        fmt::OcTrack t0;
        t0.boneIndex = 0;
        t0.channels = fmt::kOcChannelTranslation | fmt::kOcChannelRotation;
        t0.interp = fmt::OcInterp::Linear;
        t0.times = {0.0f, 1.75f, 3.5f};
        t0.values = {0,0,0,      0,0,0,1,
                     5,0,0,      0,0,0,1,
                     10,0,0,     0,0,0,1};
        fmt::OcTrack t1;
        t1.boneIndex = 7;
        t1.channels = fmt::kOcChannelScale;
        t1.interp = fmt::OcInterp::Step;
        t1.times = {0.0f, 3.5f};
        t1.values = {1,1,1,  2,2,2};
        a.tracks = {t0, t1};
        a.notifies = {{0.25f, "OnFootstep.L"}, {2.0f, "OnFootstep.R"}, {3.5f, "OnLand"}};

        std::vector<u8> first; std::string why;
        check(fmt::writeOcAnim(a, first, &why), "the fully-populated clip writes: " + why);

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(first.data(), first.size(), back, &why), "and parses: " + why);

        std::vector<u8> second;
        check(fmt::writeOcAnim(back, second, &why), "and writes again: " + why);
        check(first == second,
              "BYTE-IDENTICAL after a parse and a rewrite -- so opening a clip in the editor and "
              "saving it back cannot quietly drop a field");

        // Named individually as well, because "the bytes match" tells you nothing about WHICH field
        // went missing on the day it stops matching.
        checkNear(back.duration, a.duration, 1e-6f, "duration survives");
        check(back.flags == a.flags, "flags survive");
        check(back.storage == a.storage, "storage survives");
        check(back.skeletonRef == a.skeletonRef, "the skeleton reference survives");
        check(back.tracks.size() == 2, "both tracks survive");
        check(back.notifies.size() == 3, "all three notifies survive");
        if (back.tracks.size() == 2) {
            check(back.tracks[1].boneIndex == 7 && back.tracks[1].interp == fmt::OcInterp::Step,
                  "including the second track's bone index and its STEP interpolation");
        }
    }

    AVER_INFO("=== .ocanim refuses malformed clips ===");
    {
        std::vector<u8> bytes; std::string why;

        fmt::OcAnimation none;
        check(!fmt::writeOcAnim(none, bytes, &why), "a clip with no tracks is refused");

        fmt::OcAnimation mismatched;
        fmt::OcTrack t; t.boneIndex = 0; t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 1.0f}; t.values = {1, 2, 3};
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
