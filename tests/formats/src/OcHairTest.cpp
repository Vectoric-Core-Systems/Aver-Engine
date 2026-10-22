// `.ochair`, one strand groom: the byte-identical round trip of the HEAD text and the STRN and SCLP chunks, the refusals valid() promises, forward
// compatibility (a newer OCHAIR number refused, unknown records and keys skipped), the optional strand width table, hand-edited text (comments and
// continuation lines), and the glTF LINES / LINE_STRIP import into strands. Exit code = failure count.
//
// The deterministic generator is checked where it lives, in tests/hair (HairGenerateTest), because tests/formats links Aver.Formats and nothing above it.
// NOTHING IN HERE HAS BEEN RUN: thresholds and fixtures are starting values for the first pass.
#include "aver/formats/Avr1.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcHair.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// ---- a groom with every section ------------------------------------------------------------------------------------------------

static fmt::OcHairData makeGroom() {
    fmt::OcHairData d;
    d.scalpMesh = "Meshes/Head.ocmesh";
    d.scalpSubmesh = 1;
    d.scalpJoints = {"head", "neck_01"};
    d.sim.iterationsPerSecond = 240.0f;
    d.sim.maxDeltaTime = 0.04f;
    d.sim.grid[0] = 24.0f; d.sim.grid[1] = 24.0f; d.sim.grid[2] = 16.0f;
    d.sim.boundsPaddingCm = 12.5f;
    d.sim.simFraction = 0.25f;
    d.sim.resample = 12;
    d.sim.initialGravity[2] = -980.0f;

    fmt::OcHairMaterial m0;
    m0.index = 0;
    m0.linearDamping = 2.5f;
    m0.bendCompliance = 3.0e-7f;
    m0.gravity = {0.1f, 1.0f, 0.2f, 0.8f};
    m0.simFraction = 0.2f;
    fmt::OcHairMaterial m1;
    m1.index = 3;
    m1.collision = false;
    m1.lra = false;
    m1.bendMul[1] = 50.0f;
    m1.globalPose = {0.02f, 0.0f, 0.0f, 0.5f};
    d.materials = {m0, m1};

    fmt::OcHairLook l0;
    l0.material = 0;
    l0.baseColor[0] = 0.1f; l0.baseColor[1] = 0.2f; l0.baseColor[2] = 0.3f;
    l0.widthRootCm = 0.15f;
    l0.tint[1] = 0.55f;
    d.looks = {l0};

    d.generate.present = true;
    d.generate.seed = 12345;
    d.generate.strands = 2000;
    d.generate.lengthCm[0] = 20.0f; d.generate.lengthCm[1] = 31.5f;
    d.generate.segments = 10;
    d.generate.curlCm = 1.2f;
    d.generate.clumps = 40;
    d.generate.partWidthCm = 0.8f;
    d.generate.mask = "Textures/HairMask.octex";
    d.generate.material = 0;

    d.lod.distances[0] = 250.0f; d.lod.distances[1] = 650.0f; d.lod.distances[2] = 1400.0f;
    d.lod.strandFractions[3] = 0.1f;

    fmt::OcHairProxy head;
    head.name = "head";
    head.bone = "head";
    head.shape = fmt::OcHairProxyShape::Sphere;
    head.sizeCm[0] = 11.0f;
    head.offsetCm[2] = 4.0f;
    fmt::OcHairProxy neck;
    neck.name = "neck";
    neck.bone = "neck_01";
    neck.shape = fmt::OcHairProxyShape::Capsule;
    neck.sizeCm[0] = 5.0f; neck.sizeCm[1] = 3.0f;
    fmt::OcHairProxy chest;
    chest.name = "chest";
    chest.shape = fmt::OcHairProxyShape::Box;
    chest.sizeCm[0] = 14.0f; chest.sizeCm[1] = 8.0f; chest.sizeCm[2] = 12.0f;
    d.proxies = {head, neck, chest};
    d.maxRenderStrands = 25000;

    // Three explicit strands of 4, 5 and 3 points.
    fmt::OcHairStrands& s = d.strands;
    s.start = {0, 4, 9, 12};
    for (u32 i = 0; i < 12; ++i) {
        s.points.push_back(static_cast<f32>(i) * 0.5f);
        s.points.push_back(static_cast<f32>(i % 4));
        s.points.push_back(-static_cast<f32>(i) * 0.25f);
    }
    s.material = {0, 0, 3};

    d.hasScalp = true;
    fmt::OcHairScalp& sc = d.scalp;
    sc.positions = {-5, -5, 0, 5, -5, 0, 5, 5, 0, -5, 5, 0};
    sc.triangles = {0, 1, 2, 0, 2, 3};
    sc.joints = {0, 1, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0,  0, 1, 0, 0};
    sc.weights = {0.5f, 0.5f, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  0.25f, 0.75f, 0, 0};
    return d;
}

static bool sameBytes(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

static std::vector<u8> bytesOf(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

// A groom the validity tests mutate: valid by construction.
static fmt::OcHairData validGroom() {
    fmt::OcHairData d = makeGroom();
    return d;
}

static bool refused(const fmt::OcHairData& d, const std::string& needle) {
    std::string why;
    if (d.valid(&why)) return false;
    return needle.empty() || why.find(needle) != std::string::npos;
}

// ---- a tiny base64 encoder for the glTF fixture -------------------------------------------------------------------------------

static std::string base64(const std::vector<u8>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const u32 a = in[i], b = i + 1 < in.size() ? in[i + 1] : 0, c = i + 2 < in.size() ? in[i + 2] : 0;
        const u32 v = (a << 16) | (b << 8) | c;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

int main() {
    AVER_INFO("=== .ochair round trip ===");
    {
        const fmt::OcHairData src = makeGroom();
        std::string why;
        check(src.valid(&why), "the fixture is valid (" + why + ")");

        std::vector<u8> bytes;
        check(fmt::writeOcHair(src, bytes, &why), "writes (" + why + ")");
        check(bytes.size() > 64, "produced a container of " + std::to_string(bytes.size()) + " bytes");

        fmt::Avr1File f;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), f, &why), "the container parses (" + why + ")");
        check(f.subtype == fmt::kAvrSubtypeHair, "subtype HAIR");
        check(f.find(fmt::kOcHairChunkHead) && f.find(fmt::kOcHairChunkStrands) && f.find(fmt::kOcHairChunkScalp), "HEAD, STRN and SCLP chunks are present");

        fmt::OcHairData back;
        check(fmt::parseOcHair(bytes.data(), bytes.size(), back, &why), "parses back (" + why + ")");
        check(back.scalpMesh == src.scalpMesh && back.scalpSubmesh == src.scalpSubmesh, "SCALP and SUBMESH survive");
        check(back.scalpJoints == src.scalpJoints, "SCALPJOINTS survive in order");
        check(back.sim.iterationsPerSecond == 240.0f && back.sim.grid[2] == 16.0f && back.sim.resample == 12 && back.sim.boundsPaddingCm == 12.5f, "SIM survives");
        check(back.materials.size() == 2 && back.materials[1].index == 3 && !back.materials[1].collision && !back.materials[1].lra, "MATERIAL records survive");
        check(back.materials[0].bendCompliance == 3.0e-7f && back.materials[0].simFraction == 0.2f && back.materials[1].simFraction < 0.0f,
              "a material's own simFraction survives, and an absent one stays 'use the SIM one'");
        check(back.materials[1].globalPose == src.materials[1].globalPose && back.materials[0].gravity == src.materials[0].gravity, "gradients survive");
        check(back.looks.size() == 1 && back.looks[0].widthRootCm == 0.15f && back.looks[0].tint[1] == 0.55f, "LOOK survives");
        check(back.generate.present && back.generate.seed == 12345 && back.generate.strands == 2000 && back.generate.mask == "Textures/HairMask.octex" &&
                  back.generate.lengthCm[1] == 31.5f && back.generate.segments == 10,
              "GENERATE survives");
        check(back.lod.distances[2] == 1400.0f && back.lod.strandFractions[3] == 0.1f, "LOD survives");
        check(back.proxies.size() == 3 && back.proxies[1].shape == fmt::OcHairProxyShape::Capsule && back.proxies[1].sizeCm[1] == 3.0f &&
                  back.proxies[2].shape == fmt::OcHairProxyShape::Box && back.proxies[2].bone.empty() && back.proxies[0].offsetCm[2] == 4.0f,
              "PROXY records survive");
        check(back.maxRenderStrands == 25000, "MAXSTRANDS survives");
        check(back.strands.start == src.strands.start && back.strands.points == src.strands.points && back.strands.material == src.strands.material,
              "the strands survive bit for bit");
        check(back.strands.width.empty(), "no width table was written, none is read");
        check(back.hasScalp && back.scalp.positions == src.scalp.positions && back.scalp.triangles == src.scalp.triangles &&
                  back.scalp.joints == src.scalp.joints && back.scalp.weights == src.scalp.weights,
              "the embedded scalp survives bit for bit");

        std::vector<u8> again;
        check(fmt::writeOcHair(back, again, &why) && sameBytes(bytes, again), "write, parse, write again is byte identical");
        check(fmt::ocHairHeadText(back) == fmt::ocHairHeadText(src), "the HEAD text is identical after the round trip");

        // The text on its own: parse into an empty groom, the strands untouched.
        fmt::OcHairData fromText;
        check(fmt::parseOcHairHead(fmt::ocHairHeadText(src), fromText, &why), "the HEAD text parses on its own (" + why + ")");
        check(fmt::ocHairHeadText(fromText) == fmt::ocHairHeadText(src), "the text reads back to the same text");
        check(fromText.strands.empty(), "parsing text alone brings no strands");
    }

    AVER_INFO("=== the optional width table, and a generate-only groom ===");
    {
        fmt::OcHairData d = makeGroom();
        d.strands.width = {0.1f, 0.12f, 0.08f};
        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcHair(d, bytes, &why), "writes with widths (" + why + ")");
        fmt::OcHairData back;
        check(fmt::parseOcHair(bytes.data(), bytes.size(), back, &why) && back.strands.width == d.strands.width, "the width table round trips");
        std::vector<u8> again;
        check(fmt::writeOcHair(back, again, &why) && sameBytes(bytes, again), "and is byte stable");

        fmt::OcHairData g;
        g.generate.present = true;
        g.generate.strands = 500;
        g.scalpMesh = "Meshes/Scalp.ocmesh";
        check(g.valid(&why), "a groom with GENERATE and no strands is valid (" + why + ")");
        std::vector<u8> gb;
        check(fmt::writeOcHair(g, gb, &why), "and writes");
        fmt::OcHairData gback;
        check(fmt::parseOcHair(gb.data(), gb.size(), gback, &why) && gback.strands.empty() && gback.generate.present && gback.generate.strands == 500,
              "and reads back with no strands");
    }

    AVER_INFO("=== forward compatibility and hand-edited text ===");
    {
        fmt::OcHairData d;
        std::string why;
        check(!fmt::parseOcHairHead("OCHAIR 2\nSIM grid 32 32 32\n", d, &why) && why.find("newer") != std::string::npos, "a newer OCHAIR number is refused, and says why");
        check(!fmt::parseOcHairHead("SIM grid 32 32 32\n", d, &why), "the first record must be OCHAIR");
        check(!fmt::parseOcHairHead("OCHAIR 1\nOCHAIR 1\n", d, &why), "a second OCHAIR header is refused");

        const std::string text =
            "# a hand edited groom\n"
            "OCHAIR 1\n"
            "SCALP Meshes/H.ocmesh SUBMESH 2   # trailing comment\n"
            "SCALPJOINTS head neck_01\n"
            "FUTURERECORD 1 2 3\n"
            "SIM iterationsPerSecond 180 futureKey 7 8 grid 16 16 16\n"
            "MATERIAL 1 linearDamping 3\n"
            "       friction 0.4 gravity 0.2 0.9\n"
            "LOOK 1 baseColor 0.2 0.1 0.05 widthRootCm 0.2\r\n"
            "GENERATE seed 7 strands 100 mask Textures/M.octex lengthCm 10 12\n"
            "PROXY ear bone head shape box size 1 2 3 offset 0 0 1\n";
        fmt::OcHairData t;
        check(fmt::parseOcHairHead(text, t, &why), "comments, CRLF, continuation lines, an unknown record and an unknown key are accepted (" + why + ")");
        check(t.scalpMesh == "Meshes/H.ocmesh" && t.scalpSubmesh == 2 && t.scalpJoints.size() == 2, "SCALP with SUBMESH and SCALPJOINTS read");
        check(t.sim.iterationsPerSecond == 180.0f && t.sim.grid[0] == 16.0f, "SIM read around the unknown key");
        check(t.materials.size() == 1 && t.materials[0].index == 1 && t.materials[0].linearDamping == 3.0f && t.materials[0].friction == 0.4f,
              "a continuation line joins its record");
        check(t.materials[0].gravity.min == 0.2f && t.materials[0].gravity.max == 0.9f && t.materials[0].gravity.minFraction == 0.2f,
              "a two-number gradient keeps the default fractions");
        check(t.looks.size() == 1 && t.looks[0].material == 1 && t.looks[0].widthRootCm == 0.2f, "LOOK read");
        check(t.generate.present && t.generate.seed == 7 && t.generate.mask == "Textures/M.octex" && t.generate.lengthCm[1] == 12.0f, "GENERATE read, mask as a word");
        check(t.proxies.size() == 1 && t.proxies[0].shape == fmt::OcHairProxyShape::Box && t.proxies[0].sizeCm[2] == 3.0f, "PROXY read");

        check(!fmt::parseOcHairHead("OCHAIR 1\nSIM grid 32 32\n", d, &why), "a key with the wrong number of values is refused");
        check(!fmt::parseOcHairHead("OCHAIR 1\nMATERIAL 0\nMATERIAL 0\n", d, &why), "a repeated MATERIAL index is refused");
        check(!fmt::parseOcHairHead("OCHAIR 1\nMATERIAL 300\n", d, &why), "a material index above 255 is refused at read");
        check(!fmt::parseOcHairHead("OCHAIR 1\nPROXY a shape cone size 1\n", d, &why), "an unknown proxy shape is refused");
        check(!fmt::parseOcHairHead("OCHAIR 1\nMATERIAL 0 gravity 0.1 1 0.2\n", d, &why), "a gradient of three numbers is refused");
    }

    AVER_INFO("=== valid() refusals ===");
    {
        fmt::OcHairData d = validGroom();
        d.strands.start = {0, 2, 6};
        d.strands.points.resize(18);
        d.strands.material = {0, 0};
        check(refused(d, "fewer than 3"), "a strand of 2 points is refused");

        d = validGroom();
        d.strands.start = {0, 256};
        d.strands.points.assign(256 * 3, 0.0f);
        d.strands.material = {0};
        check(refused(d, "more than 255"), "a strand of 256 points is refused");

        d = validGroom();
        d.materials[0].index = 256;
        check(refused(d, "above 255"), "a material index of 256 is refused");

        d = validGroom();
        d.strands.material[2] = 200;
        d.maxRenderStrands = 2;
        check(refused(d, "MAXSTRANDS"), "more strands than MAXSTRANDS is refused");

        d = validGroom();
        d.strands.points[4] = std::nanf("");
        check(refused(d, "not finite"), "a non-finite strand point is refused");

        d = validGroom();
        d.sim.initialGravity[1] = INFINITY;
        check(refused(d, "initialGravity"), "a non-finite SIM number is refused");

        d = validGroom();
        d.scalpJoints.assign(17, "j");
        check(refused(d, "more than 16"), "more than 16 SCALPJOINTS are refused");

        d = validGroom();
        d.scalpMesh = "Meshes/Head.obj";
        check(refused(d, "not an .ocmesh"), "a SCALP that is not an .ocmesh is refused");

        d = validGroom();
        d.materials[0].gravity.maxFraction = d.materials[0].gravity.minFraction;
        check(refused(d, "gradient"), "a gradient whose fractions coincide is refused");

        d = validGroom();
        d.strands = fmt::OcHairStrands{};
        d.generate.present = false;
        check(refused(d, "neither"), "no strands and no GENERATE is refused");

        d = validGroom();
        d.strands.width = {0.1f};
        check(refused(d, "width"), "a width table of the wrong length is refused");

        d = validGroom();
        d.scalp.joints[0] = 5;
        check(refused(d, "joint"), "an embedded scalp vertex naming a missing joint is refused");

        d = validGroom();
        d.lod.distances[1] = 100.0f;
        check(refused(d, "LOD"), "LOD distances that do not increase are refused");

        d = validGroom();
        d.sim.resample = 2;
        check(refused(d, "resample"), "SIM resample of 2 is refused");

        std::vector<u8> bytes;
        std::string why;
        d = validGroom();
        d.materials[0].index = 300;
        check(!fmt::writeOcHair(d, bytes, &why) && bytes.empty(), "writeOcHair refuses an invalid groom and writes nothing");
    }

    AVER_INFO("=== damaged chunks ===");
    {
        const fmt::OcHairData src = makeGroom();
        std::vector<u8> bytes;
        std::string why;
        fmt::writeOcHair(src, bytes, &why);
        fmt::Avr1File f;
        fmt::parseAvr1(bytes.data(), bytes.size(), f, &why);

        // A STRN one byte short.
        {
            fmt::Avr1File g = f;
            for (fmt::AvrChunk& c : g.chunks)
                if (c.id == fmt::kOcHairChunkStrands) c.data.pop_back();
            std::vector<u8> out;
            fmt::writeAvr1(g, out, &why);
            fmt::OcHairData d;
            check(!fmt::parseOcHair(out.data(), out.size(), d, &why) && why.find("STRN") != std::string::npos, "a STRN one byte short is refused by name");
        }
        // No HEAD.
        {
            fmt::Avr1File g;
            g.subtype = fmt::kAvrSubtypeHair;
            g.add(fmt::kOcHairChunkStrands, std::vector<u8>{1, 2, 3, 4});
            std::vector<u8> out;
            fmt::writeAvr1(g, out, &why);
            fmt::OcHairData d;
            check(!fmt::parseOcHair(out.data(), out.size(), d, &why) && why.find("HEAD") != std::string::npos, "a container with no HEAD is refused");
        }
        // The wrong subtype.
        {
            fmt::Avr1File g = f;
            g.subtype = fmt::kAvrSubtypeMesh;
            std::vector<u8> out;
            fmt::writeAvr1(g, out, &why);
            fmt::OcHairData d;
            check(!fmt::parseOcHair(out.data(), out.size(), d, &why), "a container of another subtype is refused");
        }
        // An SCLP that names a vertex it does not have.
        {
            fmt::Avr1File g = f;
            for (fmt::AvrChunk& c : g.chunks)
                if (c.id == fmt::kOcHairChunkScalp) {
                    // triangle table starts after 8 + 4 * 12 bytes of positions
                    const size_t tri0 = 8 + 4 * 12;
                    c.data[tri0] = 200;
                }
            std::vector<u8> out;
            fmt::writeAvr1(g, out, &why);
            fmt::OcHairData d;
            check(!fmt::parseOcHair(out.data(), out.size(), d, &why), "an SCLP triangle past its vertices is refused");
        }
    }

    AVER_INFO("=== glTF line strands ===");
    {
        // Six points, then the indices of a LINES primitive.
        const f32 pts[18] = {1, 2, 3,  1, 2, 4,  1, 2, 5,  9, 9, 9,  4, 0, 0,  4, 1, 0};
        const u16 idx[6] = {0, 1, 1, 2, 4, 5};
        std::vector<u8> buf(sizeof pts + sizeof idx);
        std::memcpy(buf.data(), pts, sizeof pts);
        std::memcpy(buf.data() + sizeof pts, idx, sizeof idx);
        const std::string json =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,0,0]}],"
            "\"meshes\":[{\"primitives\":["
            "{\"attributes\":{\"POSITION\":0},\"mode\":3,\"material\":2},"
            "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":1}]}],"
            "\"buffers\":[{\"byteLength\":" + std::to_string(buf.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(buf) + "\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72},{\"buffer\":0,\"byteOffset\":72,\"byteLength\":12}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}";
        fmt::GltfImportResult r;
        std::string why;
        const bool ok = fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", r, {}, &why);
        check(ok, "a glTF with only line primitives imports (" + why + ")");
        const fmt::ImportedHairCurves& hc = r.hairCurves;
        // Strip: 6 points in one strand. Lines: (0,1),(1,2) chain into 3 points, (4,5) is its own 2-point strand.
        check(hc.strandCount() == 3, "three strands were found, not " + std::to_string(hc.strandCount()));
        if (hc.strandCount() == 3) {
            check(hc.strandStart[1] == 6 && hc.strandStart[2] == 9 && hc.strandStart[3] == 11, "strand starts are 0, 6, 9, 11");
            check(hc.material[0] == 2 && hc.material[1] == 0 && hc.material[2] == 0, "the strip carries its glTF material, the lines primitive has none (0)");
            // glTF (1,2,3) metres + node translation (10,0,0) -> engine (-z, x, y) * 100
            check(std::fabs(hc.points[0] - (-300.0f)) < 0.01f && std::fabs(hc.points[1] - 1100.0f) < 0.01f && std::fabs(hc.points[2] - 200.0f) < 0.01f,
                  "points are in engine space, centimetres, with the node's translation applied");
            check(hc.pointWidthCm.empty(), "no _WIDTH attribute, no width table");
        }
        check(r.meshes.size() == 1 && r.meshes[0].indices.empty(), "the mesh carries no triangles");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
