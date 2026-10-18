// Blend shapes in the .ocmesh format (the MRPH chunk) and the helpers importers use to fill them:
// round trip, backward compatibility, every malformed-chunk case, MorphTargetBuilder, remap and append.
// Bytes in memory only; no GPU.
#include "aver/formats/Avr1.hpp"
#include "aver/formats/MorphTarget.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if AVER_MORPH_TEST_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

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

constexpr u32 kV = 70;   // spans more than two 32-bit words

// A strip of triangles over kV vertices, one submesh, one slot. No targets.
static fmt::OcMeshData makeStrip() {
    fmt::OcMeshData m;
    for (u32 i = 0; i < kV; ++i) {
        m.positions.insert(m.positions.end(), {f32(i), 0.0f, f32(i % 2)});
        m.normals.insert(m.normals.end(), {0.0f, 1.0f, 0.0f});
        m.uvs.insert(m.uvs.end(), {f32(i) / f32(kV), 0.5f});
    }
    for (u32 i = 0; i + 2 < kV; ++i) m.indices.insert(m.indices.end(), {i, i + 1, i + 2});
    m.submeshes.push_back(fmt::OcMeshSubmesh{"strip", 0, 0, static_cast<u32>(m.indices.size()), 0, kV});
    m.materialSlots = {"M_Strip"};
    return m;
}

// Three targets: normals + the first/last/middle vertices; no normals + a default weight; one at the last vertex.
static fmt::OcMeshData makeMorphed() {
    fmt::OcMeshData m = makeStrip();
    fmt::OcMorphTarget a;
    a.name = "mouth Smile.L";   // a name that needs the string table
    a.vertices = {0, 33, kV - 1};
    a.positionDeltas = {1, 2, 3,  -4, 5, -6,  0.5f, 0.25f, -0.125f};
    a.normalDeltas = {0, 0.5f, 0,  0.25f, 0, 0,  0, 0, -0.5f};
    fmt::OcMorphTarget b;
    b.name = "jawOpen";
    b.defaultWeight = 0.25f;
    b.vertices = {2, 3};
    b.positionDeltas = {0, 0, -10,  0, 0, -20};
    fmt::OcMorphTarget c;
    c.name = "blinkL";
    c.vertices = {kV - 1};
    c.positionDeltas = {0.1f, 0.2f, 0.3f};
    m.morphTargets = {a, b, c};
    for (fmt::OcMorphTarget& t : m.morphTargets) t.finalize();
    m.flags |= fmt::kOcMeshHasMorph;
    return m;
}

static std::vector<u8> bytesOf(const fmt::OcMeshData& m, std::string* why = nullptr) {
    std::vector<u8> out;
    if (!fmt::writeOcMesh(m, out, why)) out.clear();
    return out;
}

// Parses a container, lets `fn` edit it, writes it back. False when any step fails.
static bool rewrite(const std::vector<u8>& in, const std::function<void(fmt::Avr1File&)>& fn, std::vector<u8>& out) {
    fmt::Avr1File f;
    if (!fmt::parseAvr1(in.data(), in.size(), f, nullptr)) return false;
    fn(f);
    return fmt::writeAvr1(f, out, nullptr);
}

static u32 fourCC(const char* s) {
    return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

static fmt::AvrChunk* findChunk(fmt::Avr1File& f, const char* id) {
    for (fmt::AvrChunk& c : f.chunks) if (c.id == fourCC(id)) return &c;
    return nullptr;
}

static void patchU32(std::vector<u8>& d, usize off, u32 v) { std::memcpy(d.data() + off, &v, 4); }
static void patchU64(std::vector<u8>& d, usize off, u64 v) { std::memcpy(d.data() + off, &v, 8); }
static u32 readU32(const std::vector<u8>& d, usize off) { u32 v; std::memcpy(&v, d.data() + off, 4); return v; }
static u64 readU64(const std::vector<u8>& d, usize off) { u64 v; std::memcpy(&v, d.data() + off, 8); return v; }

// MRPH layout: 12-byte header, then 32-byte descriptors, entries after (8-aligned).
constexpr usize kDesc0 = 12;
constexpr usize kDescStride = 32;

// Loads `bytes`; true on success.
static bool loads(const std::vector<u8>& bytes, fmt::OcMeshData& out, std::string* why = nullptr) {
    return !bytes.empty() && fmt::parseOcMesh(bytes.data(), bytes.size(), out, why);
}

static void testRoundTrip() {
    AVER_INFO("=== MRPH round trip ===");
    const fmt::OcMeshData src = makeMorphed();
    check(src.valid(), "the fixture is valid");
    std::string why;
    const std::vector<u8> bytes = bytesOf(src, &why);
    check(!bytes.empty(), "writes: " + why);
    fmt::OcMeshData back;
    check(loads(bytes, back, &why), "reads: " + why);
    check(back.hasMorph() && back.morphTargets.size() == 3, "three targets survive");
    check((back.flags & fmt::kOcMeshHasMorph) != 0, "the flag is set on the loaded mesh");
    if (back.morphTargets.size() != 3) return;
    for (usize i = 0; i < 3; ++i) {
        const fmt::OcMorphTarget& s = src.morphTargets[i];
        const fmt::OcMorphTarget& d = back.morphTargets[i];
        check(d.name == s.name, "target " + std::to_string(i) + " keeps its name '" + s.name + "'");
        check(d.vertices == s.vertices, "target " + std::to_string(i) + " keeps its vertex list");
        check(d.positionDeltas == s.positionDeltas, "target " + std::to_string(i) + " keeps its position deltas exactly");
        check(d.normalDeltas.size() == s.normalDeltas.size(), "target " + std::to_string(i) + " keeps its normal-delta presence");
        for (usize k = 0; k < s.normalDeltas.size() && k < d.normalDeltas.size(); ++k)
            checkNear(d.normalDeltas[k], s.normalDeltas[k], 1e-3f, "half-precision normal delta " + std::to_string(k));
        checkNear(d.defaultWeight, s.defaultWeight, 0.0f, "default weight");
    }
    check(back.morphTargets[0].vertices.front() == 0 && back.morphTargets[0].vertices.back() == kV - 1,
          "the vertex-0 and last-vertex edges survive");
    check(back.morphTargets[1].normalDeltas.empty(), "the target without normals still has none");
    // finalize
    checkNear(back.morphTargets[0].maxAbs[0], 4.0f, 1e-6f, "maxAbs x is the largest |dx|");
    checkNear(back.morphTargets[0].maxAbs[2], 6.0f, 1e-6f, "maxAbs z is the largest |dz|");
    checkNear(back.morphTargets[1].maxLen, 20.0f, 1e-5f, "maxLen is the longest delta");
    // the base mesh is intact
    check(back.positions == src.positions && back.indices == src.indices, "the base mesh is untouched");
}

static void testCompat() {
    AVER_INFO("=== backward compatibility ===");
    const fmt::OcMeshData base = makeStrip();
    const std::vector<u8> plain = bytesOf(base);
    fmt::Avr1File pf;
    check(fmt::parseAvr1(plain.data(), plain.size(), pf, nullptr), "a mesh without targets parses as a container");
    check(pf.find(fmt::avrFourCC("MRPH")) == nullptr, "and writes no MRPH chunk");
    check(pf.contentVersion == 1, "contentVersion stays 1 without targets");
    // The plain mesh is written exactly as a mesh whose targets were emptied.
    fmt::OcMeshData morphed = makeMorphed();
    const std::vector<u8> withT = bytesOf(morphed);
    morphed.morphTargets.clear();
    morphed.flags &= ~fmt::kOcMeshHasMorph;
    morphed.positions = base.positions;   // the fixture is the strip, so this is a no-op, kept for clarity
    check(bytesOf(morphed) == plain, "clearing the targets writes the same bytes as a mesh that never had any");

    fmt::Avr1File mf;
    check(fmt::parseAvr1(withT.data(), withT.size(), mf, nullptr), "a morphed mesh parses as a container");
    check(mf.find(fmt::avrFourCC("MRPH")) != nullptr, "it carries an MRPH chunk");
    check(mf.contentVersion == 2, "contentVersion is 2 only with targets");
    const fmt::AvrChunk* mr = mf.find(fmt::avrFourCC("MRPH"));
    check(mr && (mr->flags & fmt::kAvrChunkRequired) == 0, "MRPH is optional (not Required)");
    for (const char* id : {"VTXS", "IDXS", "MADR"}) {
        const fmt::AvrChunk* a = pf.find(fourCC(id));
        const fmt::AvrChunk* b = mf.find(fourCC(id));
        check(a && b && a->data == b->data, std::string(id) + " is byte-identical with and without targets");
    }

    // An old reader never looks at MRPH: simulate it by removing the chunk.
    std::vector<u8> stripped;
    check(rewrite(withT, [](fmt::Avr1File& f) {
        for (usize i = 0; i < f.chunks.size(); ++i)
            if (f.chunks[i].id == fmt::avrFourCC("MRPH")) { f.chunks.erase(f.chunks.begin() + i); break; }
    }, stripped), "the MRPH chunk can be removed");
    fmt::OcMeshData oldView;
    std::string why;
    check(loads(stripped, oldView, &why), "a file without the chunk still loads: " + why);
    check(!oldView.hasMorph(), "and reports no targets");
    check(oldView.positions == base.positions && oldView.indices == base.indices, "with the base mesh intact");
}

static void testMalformed() {
    AVER_INFO("=== malformed MRPH chunks ===");
    const std::vector<u8> good = bytesOf(makeMorphed());
    const auto mutate = [&](const std::function<void(std::vector<u8>&)>& fn, std::vector<u8>& out) {
        return rewrite(good, [&](fmt::Avr1File& f) { if (fmt::AvrChunk* c = findChunk(f, "MRPH")) fn(c->data); }, out);
    };
    fmt::OcMeshData m; std::string why;

    {   // truncated chunk
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) { d.resize(20); }, b), "truncate the chunk");
        check(!loads(b, m, &why), "a truncated chunk fails the file");
    }
    {   // entry offset past the end
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) { patchU64(d, kDesc0 + 8, 0xFFFFFFFFFFFFFF00ull); }, b), "patch an entry offset");
        check(!loads(b, m, &why), "an offset past the end fails");
    }
    {   // vertices not ascending: swap the first two entries' vertex ids of target 1 (entries are 16 bytes)
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) {
            const usize off = usize(readU64(d, kDesc0 + kDescStride + 8));
            const u32 v0 = readU32(d, off), v1 = readU32(d, off + 16);
            patchU32(d, off, v1); patchU32(d, off + 16, v0);
        }, b), "patch two vertex ids");
        check(!loads(b, m, &why), "non-ascending vertices fail");
    }
    {   // vertex id >= count
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) {
            const usize off = usize(readU64(d, kDesc0 + 2 * kDescStride + 8));
            patchU32(d, off, kV + 5);
        }, b), "patch a vertex id past the count");
        check(!loads(b, m, &why), "a vertex id >= the vertex count fails");
    }
    {   // NaN position delta
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) {
            const usize off = usize(readU64(d, kDesc0 + kDescStride + 8));
            patchU32(d, off + 4, 0x7FC00000u);
        }, b), "patch a NaN delta");
        check(!loads(b, m, &why), "a non-finite delta fails");
    }
    {   // 513 targets
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) { patchU32(d, 0, 513); }, b), "patch TargetCount to 513");
        check(!loads(b, m, &why), "513 targets fail");
    }
    {   // VertexCount mismatch: dropped with a warning, base mesh loads
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) { patchU32(d, 4, kV + 1); }, b), "patch the chunk's VertexCount");
        check(loads(b, m, &why), "a VertexCount mismatch still loads the base mesh: " + why);
        check(!m.hasMorph() && m.vertexCount() == kV, "without targets");
    }
    {   // a newer chunk version is skipped
        std::vector<u8> b;
        check(rewrite(good, [](fmt::Avr1File& f) { if (fmt::AvrChunk* c = findChunk(f, "MRPH")) c->version = 2; }, b),
              "bump the chunk version");
        check(loads(b, m, &why), "a version-2 chunk is skipped, the file loads: " + why);
        check(!m.hasMorph(), "with no targets");
    }
    {   // duplicate names: point target 1's NameRef at target 0's
        std::vector<u8> b;
        check(mutate([](std::vector<u8>& d) { patchU32(d, kDesc0 + kDescStride, readU32(d, kDesc0)); }, b),
              "give two targets one name");
        check(loads(b, m, &why), "duplicate names load: " + why);
        if (m.morphTargets.size() == 3) {
            check(m.morphTargets[0].name == "mouth Smile.L" && m.morphTargets[1].name == "mouth Smile.L_2",
                  "the second is renamed name_2");
        } else {
            check(false, "all three targets survive the rename");
        }
    }
    {   // the writer refuses what the reader would
        fmt::OcMeshData bad = makeMorphed();
        bad.morphTargets[1].name = bad.morphTargets[0].name;
        std::vector<u8> out;
        check(!fmt::writeOcMesh(bad, out, &why), "the writer refuses duplicate names");
        bad = makeMorphed();
        std::swap(bad.morphTargets[0].vertices[0], bad.morphTargets[0].vertices[1]);
        check(!fmt::writeOcMesh(bad, out, &why), "and non-ascending vertices");
        bad = makeMorphed();
        bad.morphTargets[0].positionDeltas[1] = std::nanf("");
        check(!bad.valid() && !fmt::writeOcMesh(bad, out, &why), "and NaN deltas");
        bad = makeMorphed();
        bad.morphTargets[0].vertices.back() = kV;
        check(!bad.valid(), "valid() rejects a vertex past the mesh");
    }
}

static void testBuilder() {
    AVER_INFO("=== MorphTargetBuilder ===");
    fmt::OcMeshData m = makeStrip();
    fmt::MorphTargetBuilder b(kV);
    const u32 t0 = b.addTarget("smile", 0.5f);
    const u32 t1 = b.addTarget("smile");      // repeated name made unique
    const u32 t2 = b.addTarget("empty");
    const u32 t3 = b.addTarget("");           // empty name gets a generated one
    check(t0 == 0 && t1 == 1 && t2 == 2 && t3 == 3, "targets are indexed in order");
    check(b.targetName(t1) == "smile_2", "a repeated name becomes name_2");
    check(b.targetName(t3) == "Target_3", "an empty name becomes Target_<i>");

    b.add(t0, 10, Vec3{1, 0, 0});
    b.add(t0, 5, Vec3{0, 2, 0});
    b.add(t0, 10, Vec3{0.5f, 0, 0});          // accumulates
    b.add(t0, 20, Vec3{1e-7f, 0, 0});         // under epsilon
    b.add(t0, kV + 3, Vec3{9, 9, 9});         // out of range: ignored
    const Vec3 n{0, 0.1f, 0};
    b.add(t1, 7, Vec3{0, 0, 1}, &n);
    b.add(t1, 8, Vec3{0, 0, 1});              // no normal: zeros stand in
    b.add(t2, 1, Vec3{0, 0, 0});              // zero: the target ends up empty
    b.add(t3, 2, Vec3{3, 0, 0});
    const usize dropped = b.finish(m);
    check(dropped == 1, "one empty target dropped and counted");
    check(m.morphTargets.size() == 3, "three targets remain");
    check((m.flags & fmt::kOcMeshHasMorph) != 0, "the mesh flag is set");
    if (m.morphTargets.size() == 3) {
        const fmt::OcMorphTarget& s = m.morphTargets[0];
        check(s.vertices == std::vector<u32>({5, 10}), "entries are sorted and sparsified");
        checkNear(s.positionDeltas[3], 1.5f, 1e-6f, "duplicate adds accumulate");
        checkNear(s.defaultWeight, 0.5f, 0.0f, "the default weight is kept");
        const fmt::OcMorphTarget& s2 = m.morphTargets[1];
        check(s2.normalDeltas.size() == 6 && s2.normalDeltas[3] == 0.0f, "a target with any normal gets zeros for the rest");
        check(m.valid(), "the mesh with built targets is valid");
    }

    // source map: output vertices 0..5 come from control points 0,0,1,1,1,2
    fmt::OcMeshData m2 = makeStrip();
    fmt::MorphTargetBuilder b2(kV);
    const u32 t = b2.addTarget("fbx");
    b2.setSourceMap({0, 0, 1, 1, 1, 2});
    b2.addForSource(t, 1, Vec3{0, 0, 4});
    b2.addForSource(t, 99, Vec3{1, 1, 1});    // unknown source: ignored
    b2.finish(m2);
    check(m2.morphTargets.size() == 1 && m2.morphTargets[0].vertices == std::vector<u32>({2, 3, 4}),
          "a source vertex expands to every output vertex that came from it");
}

static void testRemapAppend() {
    AVER_INFO("=== remapMorphTargets / appendMorphTargets ===");
    fmt::OcMeshData m = makeMorphed();
    // drop vertex 33 and vertex 2; reverse the rest in a way that reorders entries.
    std::vector<u32> oldToNew(kV);
    for (u32 v = 0; v < kV; ++v) oldToNew[v] = kV - 1 - v;     // reversal
    oldToNew[33] = ~0u;
    oldToNew[2] = ~0u;
    fmt::remapMorphTargets(m, oldToNew, kV);
    check(m.morphTargets[0].vertices == std::vector<u32>({0, kV - 1}), "removed vertices drop out, the rest renumber and re-sort");
    checkNear(m.morphTargets[0].positionDeltas[0], 0.5f, 1e-6f, "the vertex that was last carries its delta to vertex 0");
    checkNear(m.morphTargets[0].positionDeltas[3], 1.0f, 1e-6f, "and the one that was first carries its delta to the end");
    check(m.morphTargets[0].normalDeltas.size() == 6, "normal deltas follow");
    check(m.morphTargets[1].vertices == std::vector<u32>({kV - 4}), "vertex 3 -> kV-4 survives, vertex 2 is gone");
    check(m.valid(), "the remapped mesh is valid");

    // append: dst has smile (vertices < kV), src has smile and an extra target
    fmt::OcMeshData dst = makeMorphed();
    fmt::OcMeshData src = makeMorphed();
    src.morphTargets.erase(src.morphTargets.begin() + 1);      // smile, blinkL
    fmt::OcMorphTarget extra;
    extra.name = "extra";
    extra.vertices = {1};
    extra.positionDeltas = {1, 1, 1};
    src.morphTargets.push_back(extra);
    // pretend src's vertices were appended after dst's
    const u32 base = dst.vertexCount();
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    fmt::appendMorphTargets(dst, src, base);
    check(dst.morphTargets.size() == 4, "same-name targets union, the new one is added");
    check(dst.morphTargets[0].vertices.size() == 6 && dst.morphTargets[0].vertices[3] == base,
          "the unioned target holds dst's entries then src's, shifted");
    check(dst.morphTargets[3].name == "extra" && dst.morphTargets[3].vertices[0] == base + 1, "the new target is shifted");
    check(dst.morphTargets[0].normalDeltas.size() == 18, "normals stay all-or-none");
    check(dst.morphTargets[1].vertices.size() == 2, "a dst-only target is untouched");
    check(dst.valid(), "the merged mesh is valid");
}

#if AVER_MORPH_TEST_TRIFACTOR
static void testSimplify() {
    AVER_INFO("=== simplifyMesh keeps blend shapes ===");
    // A flat grid with a skin and one target whose delta equals the vertex's x, so the delta identifies its vertex.
    fmt::OcMeshData m;
    const u32 N = 12;
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            m.positions.insert(m.positions.end(), {f32(x), f32(y), 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {f32(x) / N, f32(y) / N});
            m.joints.insert(m.joints.end(), {0, 0, 0, 0});
            m.weights.insert(m.weights.end(), {1, 0, 0, 0});
        }
    for (u32 y = 0; y + 1 < N; ++y)
        for (u32 x = 0; x + 1 < N; ++x) {
            const u32 a = y * N + x;
            m.indices.insert(m.indices.end(), {a, a + 1, a + N, a + 1, a + N + 1, a + N});
        }
    m.submeshes.push_back(fmt::OcMeshSubmesh{"grid", 0, 0, static_cast<u32>(m.indices.size()), 0, N * N});
    m.materialSlots = {"M"};
    fmt::OcMorphTarget t;
    t.name = "tag";
    for (u32 v = 0; v < N * N; ++v) {
        t.vertices.push_back(v);
        t.positionDeltas.insert(t.positionDeltas.end(), {m.positions[v * 3] + 1.0f, m.positions[v * 3 + 1] + 1.0f, 0.0f});
    }
    t.finalize();
    m.morphTargets.push_back(t);
    m.flags |= fmt::kOcMeshHasSkin;
    check(m.valid(), "the skinned morphing grid is valid");
    std::string why;
    if (!aver::trifactor::simplifyMesh(m, 0.5f, &why)) { check(false, "simplify: " + why); return; }
    check(m.valid() && m.hasMorph(), "after simplifying the mesh is valid and still has its target");
    bool ok = true;
    for (usize i = 0; i < m.morphTargets[0].vertices.size(); ++i) {
        const u32 v = m.morphTargets[0].vertices[i];
        ok = ok && std::fabs(m.morphTargets[0].positionDeltas[i * 3] - (m.positions[usize(v) * 3] + 1.0f)) < 1e-5f;
    }
    check(ok, "every surviving delta is still on its own vertex (compared by position)");
}
#endif

int main() {
    testRoundTrip();
    testCompat();
    testMalformed();
    testBuilder();
    testRemapAppend();
#if AVER_MORPH_TEST_TRIFACTOR
    testSimplify();
#endif
    if (g_failures == 0) AVER_INFO("=== all morph format tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
