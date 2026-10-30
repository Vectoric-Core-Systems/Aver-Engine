// GodotMeshTest -- G4 R7: Godot 4's packed ArrayMesh surface (vertex / attribute / skin / index byte arrays, octahedral normals and tangents, compressed
// attributes, 16- and 32-bit indices, strips) decoded from HAND-BUILT byte arrays, the text round trip of such a surface, and GridMap's packed cells and its
// 24 orthogonal bases. Pure data, no engine. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/godot/GodotMesh.hpp"
#include "aver/formats/godot/GodotVariant.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::fmt::godot;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

// ---- builders ----
static GodotVariant vInt(i64 v) { GodotVariant x; x.type = GodotVType::Int; x.i = v; return x; }
static GodotVariant vBytes(const std::vector<u8>& b) { GodotVariant x; x.type = GodotVType::PackedByteArray; x.bytes = b; return x; }
static GodotVariant vAabb(double px, double py, double pz, double sx, double sy, double sz) {
    GodotVariant x; x.type = GodotVType::AABB; x.nums = {px, py, pz, sx, sy, sz}; return x;
}
static GodotVariant vVec4(double a, double b, double c, double d) { GodotVariant x; x.type = GodotVType::Vector4; x.nums = {a, b, c, d}; return x; }
static GodotVariant vStr(const std::string& s) { GodotVariant x; x.type = GodotVType::String; x.s = s; return x; }
static GodotVariant vDict(const std::vector<std::pair<std::string, GodotVariant>>& kv) {
    GodotVariant d; d.type = GodotVType::Dictionary;
    for (const auto& [k, v] : kv) { d.keys.push_back(vStr(k)); d.items.push_back(v); }
    return d;
}
static void put16(std::vector<u8>& b, u32 v) { b.push_back(u8(v & 0xFF)); b.push_back(u8((v >> 8) & 0xFF)); }
static void put32(std::vector<u8>& b, u32 v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
static void putF(std::vector<u8>& b, f32 f) { u32 u; std::memcpy(&u, &f, 4); put32(b, u); }
static u32 q16(double v) { return static_cast<u32>(std::lround(std::min(1.0, std::max(0.0, v)) * 65535.0)); }

// format bits: vertex 1, normal 2, tangent 4, colour 8, uv 16, uv2 32, bones 1024, weights 2048, index 4096, version 1 (<< 35)
static constexpr i64 kV = 1, kN = 2, kT = 4, kC = 8, kU = 16, kU2 = 32, kBones = 1024, kWeights = 2048, kIdx = 4096;
static constexpr i64 kVersion = 1ll << 35;
static constexpr i64 kCompress = 1ll << 29;

static GodotVariant triangleSurface(i64 format, i64 primitive = 3) {
    // Three vertices of a triangle in the XY plane, uncompressed: positions, then normal + tangent words, then UVs; indexed with u16.
    std::vector<u8> vd, ad, id;
    const f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (f32 f : pos) putF(vd, f);
    for (int i = 0; i < 3; ++i) {
        put16(vd, q16(0.5)); put16(vd, q16(0.5));                          // normal (0, 0, 1)
        put16(vd, q16(1.0)); put16(vd, q16(i == 1 ? 0.25 : 0.75));         // tangent (1, 0, 0), handedness + except vertex 1
    }
    const f32 uv[6] = {0, 0, 1, 0, 0, 1};
    for (f32 f : uv) putF(ad, f);
    put16(id, 0); put16(id, 1); put16(id, 2);
    return vDict({{"aabb", vAabb(0, 0, 0, 1, 1, 0)}, {"attribute_data", vBytes(ad)}, {"format", vInt(format)}, {"index_count", vInt(3)}, {"index_data", vBytes(id)},
                  {"primitive", vInt(primitive)}, {"uv_scale", vVec4(0, 0, 0, 0)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(vd)}});
}

static void testUncompressed() {
    AVER_INFO("=== uncompressed surface ===");
    const i64 fmtReal = 34359742487ll;   // what Godot 4.2+ writes for vertex + normal + tangent + uv + index
    check(fmtReal == (kV | kN | kT | kU | kIdx | kVersion), "the format number Godot writes for such a mesh is the flags added up (version 1 in bit 35)");
    GodotSurface s;
    std::string why;
    std::vector<std::string> notes;
    check(decodeArrayMeshSurface(triangleSurface(fmtReal), s, &why, &notes), "it decodes: " + why);
    check(s.vertexCount == 3 && s.indices.size() == 3 && s.indices[1] == 1 && s.indices[2] == 2, "3 vertices, one triangle");
    check(near(s.pos[3], 1, 1e-6) && near(s.pos[7], 1, 1e-6) && near(s.pos[8], 0, 1e-6), "positions are the floats written");
    check(s.nrm.size() == 9 && near(s.nrm[2], 1.0) && near(s.nrm[0], 0.0, 2e-3), "the octahedral normal decodes to +Z");
    check(s.tan.size() == 12 && near(s.tan[0], 1.0) && near(s.tan[3], 1.0) && near(s.tan[7], -1.0) && near(s.tan[11], 1.0), "the tangent decodes to +X and its handedness lives in the y half (vertex 1 is -1)");
    check(s.uv.size() == 6 && near(s.uv[2], 1, 1e-6) && near(s.uv[5], 1, 1e-6), "UVs are floats at an 8-byte stride");
    check(s.col.empty() && s.uv2.empty() && s.boneCount == 0 && !s.compressed, "streams the format does not name are absent");
    check(s.formatVersion == 1 && notes.empty(), "format version 1 and nothing to say");

    // the same through Godot's text form
    GodotVariant parsed;
    const std::string text = godotVariantToText(triangleSurface(fmtReal));
    check(parseGodotVariant(text, parsed), "the surface survives Godot's text form (base64 byte arrays)");
    GodotSurface s2;
    check(decodeArrayMeshSurface(parsed, s2, &why) && s2.pos == s.pos && s2.indices == s.indices && s2.uv == s.uv, "and decodes to the same thing");

    // colour and UV2 interleaved in the attribute buffer, in Godot's order: colour, uv, uv2
    std::vector<u8> vd, ad, id;
    const f32 pos[9] = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    for (f32 f : pos) putF(vd, f);
    for (int i = 0; i < 3; ++i) { put16(vd, q16(0.5)); put16(vd, q16(0.5)); }   // normal only
    for (int i = 0; i < 3; ++i) {
        ad.push_back(u8(i == 0 ? 255 : 0)); ad.push_back(u8(i == 1 ? 255 : 0)); ad.push_back(0); ad.push_back(255);   // colour
        putF(ad, float(i)); putF(ad, 0.5f);                                                                          // uv
        putF(ad, 0.25f); putF(ad, float(i) * 0.5f);                                                                  // uv2
    }
    put16(id, 2); put16(id, 1); put16(id, 0);
    GodotVariant d = vDict({{"attribute_data", vBytes(ad)}, {"format", vInt(kV | kN | kC | kU | kU2 | kIdx | kVersion)}, {"index_count", vInt(3)}, {"index_data", vBytes(id)},
                            {"primitive", vInt(3)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(vd)}});
    GodotSurface s3;
    check(decodeArrayMeshSurface(d, s3, &why), "colour + uv + uv2: " + why);
    check(s3.col.size() == 12 && near(s3.col[0], 1) && near(s3.col[1], 0) && near(s3.col[4 + 1], 1) && near(s3.col[3], 1), "colour is four bytes over 255");
    check(near(s3.uv[2], 1) && near(s3.uv[3], 0.5) && near(s3.uv2[0], 0.25) && near(s3.uv2[5], 1.0), "uv and uv2 follow the colour in the attribute stride");
    check(s3.indices[0] == 2 && s3.indices[2] == 0, "indices are kept in order");
    check(s3.tan.empty() && s3.nrm.size() == 9, "a normal with no tangent flag gives a normal only");

    // not indexed: the vertices in order
    GodotVariant noIdx = vDict({{"attribute_data", vBytes(ad)}, {"format", vInt(kV | kN | kC | kU | kU2 | kVersion)}, {"primitive", vInt(3)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(vd)}});
    GodotSurface s4;
    check(decodeArrayMeshSurface(noIdx, s4, &why) && s4.indices.size() == 3 && s4.indices[0] == 0 && s4.indices[2] == 2, "a surface with no index array uses 0..n-1");
}

static void testIndexWidth() {
    AVER_INFO("=== 16- and 32-bit indices ===");
    const u32 n = 65537;   // over 1 << 16: indices are four bytes
    std::vector<u8> vd, id;
    for (u32 i = 0; i < n; ++i) { putF(vd, f32(i)); putF(vd, 0); putF(vd, 0); }
    put32(id, 0); put32(id, 65536); put32(id, 1);
    GodotVariant d = vDict({{"format", vInt(kV | kIdx | kVersion)}, {"index_count", vInt(3)}, {"index_data", vBytes(id)}, {"primitive", vInt(3)}, {"vertex_count", vInt(i64(n))},
                            {"vertex_data", vBytes(vd)}});
    GodotSurface s;
    std::string why;
    check(decodeArrayMeshSurface(d, s, &why) && s.indices.size() == 3 && s.indices[1] == 65536 && near(s.pos[65536 * 3], 65536.0, 1e-3), "more than 65536 vertices: 32-bit indices and the last vertex");
    std::vector<u8> shortIds;
    put16(shortIds, 0); put16(shortIds, 1);
    GodotVariant bad = vDict({{"format", vInt(kV | kIdx | kVersion)}, {"index_count", vInt(3)}, {"index_data", vBytes(shortIds)}, {"primitive", vInt(3)}, {"vertex_count", vInt(i64(n))},
                              {"vertex_data", vBytes(vd)}});
    check(!decodeArrayMeshSurface(bad, s, &why) && why.find("index_data") != std::string::npos, "index data shorter than index_count says is refused");
}

static void testCompressed() {
    AVER_INFO("=== compressed attributes ===");
    // Positions as four u16 relative to the aabb, the normal word the rotation axis, UVs two u16; the fourth position word is the tangent-frame angle.
    const auto build = [&](u32 angleWord) {
        std::vector<u8> vd, ad, id;
        const double ps[3][3] = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}};   // aabb (-1,-1,-1) size (2,2,2): z = 0 is the middle
        for (int i = 0; i < 3; ++i) {
            put16(vd, q16((ps[i][0] + 1) / 2)); put16(vd, q16((ps[i][1] + 1) / 2)); put16(vd, q16((ps[i][2] + 1) / 2)); put16(vd, angleWord);
        }
        for (int i = 0; i < 3; ++i) { put16(vd, q16(0.5)); put16(vd, q16(0.5)); }   // axis (0, 0, 1)
        const double uvs[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int i = 0; i < 3; ++i) { put16(ad, q16(uvs[i][0])); put16(ad, q16(uvs[i][1])); }
        put16(id, 0); put16(id, 1); put16(id, 2);
        return vDict({{"aabb", vAabb(-1, -1, -1, 2, 2, 2)}, {"attribute_data", vBytes(ad)}, {"format", vInt(kV | kN | kT | kU | kIdx | kVersion | kCompress)}, {"index_count", vInt(3)},
                      {"index_data", vBytes(id)}, {"primitive", vInt(3)}, {"uv_scale", vVec4(0, 0, 0, 0)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(vd)}});
    };
    std::string why;
    GodotSurface s;
    check(decodeArrayMeshSurface(build(32768), s, &why) && s.compressed, "a compressed surface decodes: " + why);
    check(near(s.pos[0], -1, 1e-4) && near(s.pos[3], 1, 1e-4) && near(s.pos[7], 1, 1e-4) && near(s.pos[2], 0, 1e-4), "positions are the u16 fractions of the aabb");
    check(near(s.uv[2], 1, 1e-4) && near(s.uv[5], 1, 1e-4) && near(s.uv[0], 0, 1e-4), "UVs are u16 fractions when uv_scale is zero");
    check(near(s.nrm[2], 1.0) && near(s.tan[0], 1.0) && near(s.tan[3], 1.0), "angle just over one half: the identity frame, tangent +X, normal +Z, handedness +");
    GodotSurface q;
    check(decodeArrayMeshSurface(build(q16(0.75)), q, &why) && near(q.tan[0], 0.0, 2e-3) && near(q.tan[1], -1.0, 2e-3) && near(q.tan[3], 1.0) && near(q.nrm[2], 1.0),
          "angle 0.75 (a quarter turn about +Z): the tangent is row 0 of the rotation, (0, -1, 0), handedness +");
    GodotSurface r;
    check(decodeArrayMeshSurface(build(q16(0.25)), r, &why) && near(r.tan[1], -1.0, 2e-3) && near(r.tan[3], -1.0), "angle 0.25: the same quarter turn with handedness -");

    // uv_scale: stored fraction is (uv / scale + 0.5)
    std::vector<u8> ad;
    put16(ad, q16(0.5 + 3.0 / 8.0)); put16(ad, q16(0.5)); put16(ad, q16(0.5)); put16(ad, q16(0.5 - 1.0 / 8.0)); put16(ad, q16(0.5)); put16(ad, q16(0.5));
    GodotVariant d = build(32768);
    for (usize k = 0; k < d.keys.size(); ++k) {
        if (d.keys[k].s == "attribute_data") d.items[k] = vBytes(ad);
        if (d.keys[k].s == "uv_scale") d.items[k] = vVec4(8, 8, 0, 0);
    }
    GodotSurface u;
    check(decodeArrayMeshSurface(d, u, &why) && near(u.uv[0], 3.0, 2e-3) && near(u.uv[1], 0.0, 2e-3) && near(u.uv[3], -1.0, 2e-3), "a non-zero uv_scale rebases: (stored - 0.5) * scale");
}

static void testOtherPrimitivesAndSkin() {
    AVER_INFO("=== strips, skin, custom data, refusals ===");
    std::string why;
    GodotSurface s;
    // a strip of five vertices: three triangles with alternating winding
    std::vector<u8> vd, id;
    for (int i = 0; i < 5; ++i) { putF(vd, f32(i)); putF(vd, f32(i % 2)); putF(vd, 0); }
    for (int i = 0; i < 5; ++i) put16(id, u32(i));
    GodotVariant strip = vDict({{"format", vInt(kV | kIdx | kVersion)}, {"index_count", vInt(5)}, {"index_data", vBytes(id)}, {"primitive", vInt(4)}, {"vertex_count", vInt(5)}, {"vertex_data", vBytes(vd)}});
    std::vector<std::string> notes;
    check(decodeArrayMeshSurface(strip, s, &why, &notes) && s.indices.size() == 9, "a 5-vertex strip is three triangles");
    check(s.indices[0] == 0 && s.indices[1] == 1 && s.indices[2] == 2 && s.indices[3] == 2 && s.indices[4] == 1 && s.indices[5] == 3, "the second triangle's winding alternates");
    check(!notes.empty(), "and the surface says it expanded a strip");

    GodotVariant points = strip;
    for (usize k = 0; k < points.keys.size(); ++k) if (points.keys[k].s == "primitive") points.items[k] = vInt(0);
    check(!decodeArrayMeshSurface(points, s, &why) && why.find("triangle") != std::string::npos, "a point list is refused (only triangles are meshes here)");

    // skin: 4 bones per vertex (u16) then 4 weights (u16), interleaved per vertex in skin_data
    std::vector<u8> v2, sk, i2;
    for (int i = 0; i < 3; ++i) { putF(v2, f32(i)); putF(v2, 0); putF(v2, 0); }
    for (int i = 0; i < 3; ++i) {
        for (int b = 0; b < 4; ++b) put16(sk, u32(i + b));
        put16(sk, q16(0.5)); put16(sk, q16(0.25)); put16(sk, q16(0.25)); put16(sk, 0);
    }
    put16(i2, 0); put16(i2, 1); put16(i2, 2);
    GodotVariant skinned = vDict({{"format", vInt(kV | kBones | kWeights | kIdx | kVersion)}, {"index_count", vInt(3)}, {"index_data", vBytes(i2)}, {"primitive", vInt(3)},
                                  {"skin_data", vBytes(sk)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(v2)}});
    GodotSurface sk4;
    check(decodeArrayMeshSurface(skinned, sk4, &why) && sk4.boneCount == 4 && sk4.bones.size() == 12 && sk4.bones[4] == 1 && sk4.bones[7] == 4 && near(sk4.weights[4], 0.5) && near(sk4.weights[5], 0.25),
          "bones and weights: u16 each, four per vertex");
    GodotVariant eight = skinned;
    for (usize k = 0; k < eight.keys.size(); ++k) {
        if (eight.keys[k].s == "format") eight.items[k] = vInt(kV | kBones | kWeights | kIdx | kVersion | (1ll << 27));
        if (eight.keys[k].s == "skin_data") {
            std::vector<u8> sk8;
            for (int i = 0; i < 3; ++i) { for (int b = 0; b < 8; ++b) put16(sk8, u32(b)); for (int b = 0; b < 8; ++b) put16(sk8, b == 0 ? 65535 : 0); }
            eight.items[k] = vBytes(sk8);
        }
    }
    GodotSurface sk8;
    check(decodeArrayMeshSurface(eight, sk8, &why) && sk8.boneCount == 8 && sk8.bones.size() == 24 && near(sk8.weights[8], 1.0), "the 8-bone flag widens the skin stride to eight bones and eight weights");
    GodotVariant noSkin = skinned;
    for (usize k = 0; k < noSkin.keys.size(); ++k) if (noSkin.keys[k].s == "skin_data") noSkin.items[k] = vBytes({1, 2, 3});
    check(!decodeArrayMeshSurface(noSkin, s, &why) && why.find("skin_data") != std::string::npos, "skin data shorter than the format needs is refused");

    // custom data is walked past and reported
    std::vector<u8> ad;
    for (int i = 0; i < 3; ++i) { ad.insert(ad.end(), {255, 255, 255, 255}); for (int k = 0; k < 4; ++k) putF(ad, 1.0f); }   // colour, then CUSTOM0 as RGBA float (format 7)
    GodotVariant custom = vDict({{"attribute_data", vBytes(ad)}, {"format", vInt(kV | kC | (1ll << 6) | (7ll << 13) | kIdx | kVersion)}, {"index_count", vInt(3)}, {"index_data", vBytes(i2)},
                                 {"primitive", vInt(3)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes(v2)}});
    std::vector<std::string> cn;
    GodotSurface cs;
    check(decodeArrayMeshSurface(custom, cs, &why, &cn) && cs.customFormat[0] == 7 && !cn.empty() && cs.col.size() == 12, "CUSTOM0 is skipped by its stride and reported, colour still reads");

    // refusals
    check(!decodeArrayMeshSurface(vInt(3), s, &why), "a non-dictionary is refused");
    GodotVariant noData = vDict({{"format", vInt(kV | kVersion)}, {"vertex_count", vInt(3)}});
    check(!decodeArrayMeshSurface(noData, s, &why), "no vertex_data is refused");
    GodotVariant shortV = vDict({{"format", vInt(kV | kVersion)}, {"primitive", vInt(3)}, {"vertex_count", vInt(3)}, {"vertex_data", vBytes({0, 0, 0, 0})}});
    check(!decodeArrayMeshSurface(shortV, s, &why) && why.find("vertex_data") != std::string::npos, "vertex_data shorter than the format needs is refused");
    GodotVariant noAttr = triangleSurface(kV | kN | kT | kU | kIdx | kVersion);
    for (usize k = 0; k < noAttr.keys.size(); ++k) if (noAttr.keys[k].s == "attribute_data") noAttr.items[k] = vBytes({});
    check(!decodeArrayMeshSurface(noAttr, s, &why) && why.find("attribute_data") != std::string::npos, "a UV stream with no attribute_data is refused");
    std::vector<u8> badIdx;
    put16(badIdx, 0); put16(badIdx, 1); put16(badIdx, 9);
    GodotVariant oob = triangleSurface(kV | kN | kT | kU | kIdx | kVersion);
    for (usize k = 0; k < oob.keys.size(); ++k) if (oob.keys[k].s == "index_data") oob.items[k] = vBytes(badIdx);
    check(!decodeArrayMeshSurface(oob, s, &why) && why.find("past") != std::string::npos, "an index past the vertex count is refused");
    GodotVariant lods = triangleSurface(kV | kN | kT | kU | kIdx | kVersion);
    lods.keys.push_back(vStr("lods")); lods.items.push_back(vDict({}));
    std::vector<std::string> ln;
    check(decodeArrayMeshSurface(lods, s, &why, &ln) && s.hasLods && !ln.empty(), "LODs and blend shapes are noted, not decoded");
}

static void testOctahedral() {
    AVER_INFO("=== octahedral coding ===");
    const f32 dirs[][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0.577350f, 0.577350f, 0.577350f}, {-0.577350f, 0.577350f, -0.577350f}, {0.2f, -0.9f, 0.3f}};
    bool ok = true;
    for (const auto& d : dirs) {
        f32 n[3] = {d[0], d[1], d[2]};
        const double l = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
        for (f32& c : n) c = f32(c / l);
        f32 e[2], back[3];
        gdOctEncode(n, e);
        gdOctDecode(e, back);
        ok = ok && near(back[0], n[0], 1e-4) && near(back[1], n[1], 1e-4) && near(back[2], n[2], 1e-4) && e[0] >= 0 && e[0] <= 1 && e[1] >= 0 && e[1] <= 1;
    }
    check(ok, "encode then decode returns the direction for the six axes and the diagonals, with the code in 0..1");
    f32 up[3] = {0, 0, 1}, e[2];
    gdOctEncode(up, e);
    check(near(e[0], 0.5) && near(e[1], 0.5), "+Z encodes to the middle of the square");
}

static void testGrid() {
    AVER_INFO("=== GridMap cells and bases ===");
    std::vector<GodotGridCell> cells;
    std::string why;
    GodotVariant data = vDict({});
    check(decodeGridCells(data, cells, &why) && cells.empty(), "a GridMap with no cells is an empty grid");

    const GodotGridCell want[] = {{0, 0, 0, 0, 0, 0}, {1, -2, 3, 5, 10, 0}, {-32768, 32767, -1, 65534, 23, 7}, {4, 5, 6, 2, 16, 255}};
    GodotVariant arr;
    arr.type = GodotVType::PackedInt32Array;
    for (const GodotGridCell& c : want) {
        i32 w[3];
        encodeGridCell(c, w);
        for (i32 x : w) arr.nums.push_back(double(x));
    }
    GodotVariant cellsDict = vDict({{"cells", arr}});
    check(decodeGridCells(cellsDict, cells, &why) && cells.size() == 4, "four cells decode: " + why);
    bool same = true;
    for (usize i = 0; i < cells.size(); ++i)
        same = same && cells[i].x == want[i].x && cells[i].y == want[i].y && cells[i].z == want[i].z && cells[i].item == want[i].item && cells[i].orientation == want[i].orientation && cells[i].layer == want[i].layer;
    check(same, "x, y and z are signed 16-bit, the item 16 bits, the orientation 5 bits and the layer 8 bits");
    i32 w[3];
    encodeGridCell({1, 0, 0, 1, 0, 0}, w);
    check(w[0] == 1 && w[1] == 0 && w[2] == 1, "the key's low word is x | y << 16, then z, then the cell value");
    encodeGridCell({0, 1, 2, 3, 4, 0}, w);
    check(w[0] == (1 << 16) && w[1] == 2 && w[2] == (3 | (4 << 16)), "y sits in the high half of the first int, orientation above the item in the third");

    GodotVariant withEmpty;
    withEmpty.type = GodotVType::PackedInt32Array;
    i32 e3[3];
    encodeGridCell({0, 0, 0, 0xFFFF, 0, 0}, e3);
    for (i32 x : e3) withEmpty.nums.push_back(double(x));
    check(decodeGridCells(vDict({{"cells", withEmpty}}), cells, &why) && cells.empty(), "an INVALID_CELL_ITEM (0xFFFF) cell is skipped");
    GodotVariant ragged;
    ragged.type = GodotVType::PackedInt32Array;
    ragged.nums = {1, 2};
    check(!decodeGridCells(vDict({{"cells", ragged}}), cells, &why), "a cells array that is not a multiple of three is refused");

    // the 24 orthogonal bases: distinct, orthonormal, determinant +1, closed under the table's identity
    std::set<std::string> seen;
    bool good = true;
    for (int i = 0; i < 24; ++i) {
        double b[9];
        gdGridBasis(i, b);
        std::string key;
        for (double v : b) key += std::to_string(int(v)) + ",";
        seen.insert(key);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                double dot = 0;
                for (int k = 0; k < 3; ++k) dot += b[r * 3 + k] * b[c * 3 + k];
                good = good && near(dot, r == c ? 1.0 : 0.0, 1e-12);
            }
        const double det = b[0] * (b[4] * b[8] - b[5] * b[7]) - b[1] * (b[3] * b[8] - b[5] * b[6]) + b[2] * (b[3] * b[7] - b[4] * b[6]);
        good = good && near(det, 1.0, 1e-12);
    }
    check(seen.size() == 24, "all 24 bases are different");
    check(good, "each is orthonormal with determinant +1 (a proper rotation)");
    double b0[9], b10[9], b16[9], b99[9];
    gdGridBasis(0, b0);
    gdGridBasis(10, b10);
    gdGridBasis(16, b16);
    gdGridBasis(99, b99);
    check(b0[0] == 1 && b0[4] == 1 && b0[8] == 1 && b0[1] == 0, "index 0 is the identity");
    check(b10[0] == -1 && b10[4] == 1 && b10[8] == -1, "index 10 is a half turn about Y");
    check(b16[2] == 1 && b16[4] == 1 && b16[6] == -1, "index 16 is the quarter turn about Y the GridMap editor's rotate key makes");
    check(b99[0] == b0[0] && b99[4] == b0[4], "an index out of range answers the identity (Godot's answer)");
}

int main() {
    testUncompressed();
    testIndexWidth();
    testCompressed();
    testOtherPrimitivesAndSkin();
    testOctahedral();
    testGrid();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
