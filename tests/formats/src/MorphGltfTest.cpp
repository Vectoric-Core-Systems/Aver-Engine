// glTF blend shapes: targets (dense and sparse accessors), the delta transform, weight animation into
// "morph.<target>" curves, curve-only clips, and what the importer still reports. Documents are built by
// hand, as GltfTest does.
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/MorphTarget.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

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
static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol,
          what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

static std::string b64(const std::vector<u8>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < in.size(); i += 3) {
        const u32 a = in[i], b = i + 1 < in.size() ? in[i + 1] : 0, c = i + 2 < in.size() ? in[i + 2] : 0;
        const u32 v = (a << 16) | (b << 8) | c;
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? T[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < in.size() ? T[v & 63] : '=');
    }
    return out;
}

static std::vector<u8> floats(const std::vector<f32>& v) {
    std::vector<u8> b(v.size() * 4);
    if (!v.empty()) std::memcpy(b.data(), v.data(), b.size());
    return b;
}
static std::vector<u8> u16s(const std::vector<u16>& v) {
    std::vector<u8> b(v.size() * 2);
    if (!v.empty()) std::memcpy(b.data(), v.data(), b.size());
    return b;
}
static std::vector<u8> i16s(const std::vector<i16>& v) {
    std::vector<u8> b(v.size() * 2);
    if (!v.empty()) std::memcpy(b.data(), v.data(), b.size());
    return b;
}
static std::vector<u8> u32s(const std::vector<u32>& v) {
    std::vector<u8> b(v.size() * 4);
    if (!v.empty()) std::memcpy(b.data(), v.data(), b.size());
    return b;
}

// Accumulates a buffer with views and accessors; json() wraps them with the caller's scene members.
struct Doc {
    std::vector<u8> bin;
    std::string views, accessors;
    int nViews = 0, nAcc = 0;

    int view(const std::vector<u8>& data) {
        while (bin.size() % 4) bin.push_back(0);
        const usize off = bin.size();
        bin.insert(bin.end(), data.begin(), data.end());
        if (nViews++) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(data.size()) + "}";
        return nViews - 1;
    }
    // viewIdx < 0 omits the bufferView (an accessor of zeros, or a sparse base).
    int acc(int viewIdx, int componentType, u32 count, const char* type, const std::string& extra = "") {
        if (nAcc++) accessors += ",";
        accessors += "{";
        if (viewIdx >= 0) accessors += "\"bufferView\":" + std::to_string(viewIdx) + ",";
        accessors += "\"componentType\":" + std::to_string(componentType) + ",\"count\":" + std::to_string(count) +
                     ",\"type\":\"" + type + "\"" + extra + "}";
        return nAcc - 1;
    }
    int vec3(const std::vector<f32>& v) { return acc(view(floats(v)), 5126, u32(v.size() / 3), "VEC3"); }
    int scalar(const std::vector<f32>& v) { return acc(view(floats(v)), 5126, u32(v.size()), "SCALAR"); }

    std::string json(const std::string& members) const {
        return "{\"asset\":{\"version\":\"2.0\"}," + members +
               ",\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin) +
               "\",\"byteLength\":" + std::to_string(bin.size()) + "}]" +
               ",\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
    }
};

// Accessors 0..2 of every document: a triangle facing glTF +Y, its +Y normals, its indices.
struct Tri { int pos, nrm, idx; };
static Tri addTriangle(Doc& d) {
    Tri t;
    t.pos = d.vec3({0, 0, 0,  1, 0, 0,  0, 0, -1});
    t.nrm = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});
    t.idx = d.acc(d.view(u16s({0, 1, 2})), 5123, 3, "SCALAR");
    return t;
}

static const char* kScene = "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}]";

static bool importDoc(const std::string& json, fmt::GltfImportResult& res, std::string* why) {
    return fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, why);
}
static bool noted(const fmt::GltfImportResult& r, const std::string& needle) {
    for (const std::string& s : r.unsupported) if (s.find(needle) != std::string::npos) return true;
    return false;
}
static const fmt::OcMorphTarget* targetNamed(const fmt::OcMeshData& m, const std::string& n) {
    for (const fmt::OcMorphTarget& t : m.morphTargets) if (t.name == n) return &t;
    return nullptr;
}
static const fmt::OcCurve* curveNamed(const fmt::OcAnimation& a, const std::string& n) {
    for (const fmt::OcCurve& c : a.curves) if (c.name == n) return &c;
    return nullptr;
}

static std::string primJson(const Tri& t, const std::string& targets) {
    return "{\"attributes\":{\"POSITION\":" + std::to_string(t.pos) + ",\"NORMAL\":" + std::to_string(t.nrm) +
           "},\"indices\":" + std::to_string(t.idx) + (targets.empty() ? "" : ",\"targets\":[" + targets + "]") + "}";
}

static void testTargets() {
    AVER_INFO("=== targets: names, default weights, delta transform ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int dpSmile = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});      // glTF +Y: engine +Z, x100
    const int dnSmile = d.vec3({0.5f, 0, 0,  0.5f, 0, 0,  0.5f, 0, 0});
    const int dpBlink = d.vec3({1, 0, 0,  0, 0, 0,  0, 0, 0});      // only vertex 0 moves: glTF +X is engine +Y
    const std::string tg = "{\"POSITION\":" + std::to_string(dpSmile) + ",\"NORMAL\":" + std::to_string(dnSmile) + "},"
                           "{\"POSITION\":" + std::to_string(dpBlink) + "}";
    const std::string json = d.json(std::string(kScene) +
        ",\"meshes\":[{\"name\":\"Face\",\"extras\":{\"targetNames\":[\"smile\",\"blink\"]},\"weights\":[0.25,0],"
        "\"primitives\":[" + primJson(t, tg) + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    check(!noted(res, "morph targets"), "'morph targets' is no longer reported as unsupported");
    if (res.meshes.empty()) return;
    const fmt::OcMeshData& m = res.meshes[0];
    check(m.hasMorph() && m.morphTargets.size() == 2, "two targets");
    check(m.valid(), "the mesh is valid");
    const fmt::OcMorphTarget* smile = targetNamed(m, "smile");
    const fmt::OcMorphTarget* blink = targetNamed(m, "blink");
    check(smile && blink, "names come from extras.targetNames");
    if (!smile || !blink) return;
    checkNear(smile->defaultWeight, 0.25f, 0.0f, "mesh.weights gives the default weight");
    check(smile->vertices == std::vector<u32>({0, 1, 2}), "smile moves all three vertices");
    checkNear(smile->positionDeltas[0], 0.0f, 1e-4f, "glTF +Y delta: engine x");
    checkNear(smile->positionDeltas[1], 0.0f, 1e-4f, "glTF +Y delta: engine y");
    checkNear(smile->positionDeltas[2], 100.0f, 1e-3f, "glTF +Y delta: engine +Z, scaled by 100");
    check(blink->vertices == std::vector<u32>({0}), "zero deltas are not stored");
    checkNear(blink->positionDeltas[1], 100.0f, 1e-3f, "glTF +X delta is engine +Y, x100");
    check(blink->normalDeltas.empty(), "no NORMAL on the target: no normal deltas");
    check(smile->normalDeltas.size() == 9, "NORMAL deltas carried");
    // base normal (0,1,0) -> engine (0,0,1); delta (0.5,0,0) -> engine (0,0.5,0)
    checkNear(smile->normalDeltas[1], 0.5f, 1e-5f, "normal delta lands on the engine axis");
    checkNear(m.normals[2], 1.0f, 1e-5f, "base normal is engine +Z");
}

static void testNormalDeltaScale() {
    AVER_INFO("=== normal deltas through a non-uniform scale ===");
    Doc d;
    const Tri t = addTriangle(d);
    const f32 r = 0.70710678f;
    // a slanted base normal makes the node's inverse-transpose matter
    const int nrm = d.vec3({r, r, 0,  r, r, 0,  r, r, 0});
    const int dp = d.vec3({0.1f, 0, 0,  0.1f, 0, 0,  0.1f, 0, 0});
    const int dn = d.vec3({0.1f, 0, 0,  0.1f, 0, 0,  0.1f, 0, 0});
    const std::string prim = "{\"attributes\":{\"POSITION\":" + std::to_string(t.pos) + ",\"NORMAL\":" + std::to_string(nrm) +
        "},\"indices\":" + std::to_string(t.idx) + ",\"targets\":[{\"POSITION\":" + std::to_string(dp) +
        ",\"NORMAL\":" + std::to_string(dn) + "}]}";
    const std::string json = d.json(
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"scale\":[2,1,1]}],"
        "\"meshes\":[{\"primitives\":[" + prim + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    if (res.meshes.empty() || res.meshes[0].morphTargets.size() != 1) { check(false, "one target"); return; }
    const fmt::OcMeshData& m = res.meshes[0];
    const fmt::OcMorphTarget& tg = m.morphTargets[0];
    check(tg.name == "Target_0", "no names in the file: Target_<i>");
    // the node scale (2,1,1) stretches the position delta along glTF x
    checkNear(tg.positionDeltas[1], 20.0f, 1e-3f, "the position delta takes the node scale (0.1 * 2 * 100)");
    // expected: normalize(nrmXf * (n + dn)) with nrmXf = diag(0.5, 1, 1) up to scale, mapped to the engine axes
    const f32 mx = 0.5f * (r + 0.1f), my = r, mz = 0.0f;
    const f32 el = std::sqrt(mx * mx + my * my + mz * mz);
    const f32 want[3] = {-mz / el, mx / el, my / el};
    f32 got[3];
    for (int c = 0; c < 3; ++c) got[c] = m.normals[c] + tg.normalDeltas[c];
    const f32 gl = std::sqrt(got[0] * got[0] + got[1] * got[1] + got[2] * got[2]);
    for (int c = 0; c < 3; ++c) checkNear(got[c] / gl, want[c], 1e-4f, "normalize(base + delta) matches the analytic normal, axis " + std::to_string(c));
}

static void testMirrorAndCopy() {
    AVER_INFO("=== mirrored node and a second basis ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int dp = d.vec3({1, 0, 0,  0, 0, 0,  0, 0, 0});
    const std::string prim = primJson(t, "{\"POSITION\":" + std::to_string(dp) + "}");
    {
        const std::string json = d.json(
            "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"scale\":[-1,1,1]}],"
            "\"meshes\":[{\"primitives\":[" + prim + "]}]");
        fmt::GltfImportResult res; std::string why;
        check(importDoc(json, res, &why), "a mirrored node imports: " + why);
        if (!res.meshes.empty() && res.meshes[0].morphTargets.size() == 1) {
            // the vertex moves from x to x+1 in glTF; mirrored, the engine delta is -(+1) along the mapped axis
            checkNear(res.meshes[0].morphTargets[0].positionDeltas[1], -100.0f, 1e-3f, "the delta mirrors with the positions");
        } else {
            check(false, "the mirrored mesh has its target");
        }
    }
    {
        const f32 q = 0.70710678f;
        const std::string json = d.json(
            "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],\"nodes\":[{\"mesh\":0},{\"mesh\":0,\"rotation\":[0," +
            std::to_string(q) + ",0," + std::to_string(q) + "]}],"
            "\"meshes\":[{\"name\":\"M\",\"primitives\":[" + prim + "]}]");
        fmt::GltfImportResult res; std::string why;
        check(importDoc(json, res, &why), "two nodes, two bases: " + why);
        check(res.meshes.size() == 2, "a second basis gets its own mesh copy");
        if (res.meshes.size() == 2 && res.meshes[0].hasMorph() && res.meshes[1].hasMorph()) {
            const f32* a = res.meshes[0].morphTargets[0].positionDeltas.data();
            const f32* b = res.meshes[1].morphTargets[0].positionDeltas.data();
            const bool aY = std::fabs(a[1] - 100.0f) < 1e-2f, bY = std::fabs(b[1] - 100.0f) < 1e-2f;
            const bool aX = std::fabs(a[0] - 100.0f) < 1e-2f, bX = std::fabs(b[0] - 100.0f) < 1e-2f;
            check((aY && bX) || (aX && bY), "one copy keeps +Y, the quarter-turned copy's delta turns to +X");
        } else {
            check(false, "both copies carry the target");
        }
    }
}

static void testSparse() {
    AVER_INFO("=== sparse accessors ===");
    Doc d;
    const Tri t = addTriangle(d);
    // 1: u16 indices {0,2}, float values
    const int i16v = d.view(u16s({0, 2}));
    const int v1 = d.view(floats({1, 0, 0,  0, 2, 0}));
    const int sp1 = d.acc(-1, 5126, 3, "VEC3", ",\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":" + std::to_string(i16v) +
                          ",\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(v1) + "}}");
    // 2: u32 index {1}, normalized shorts (1, 0, -1)
    const int i32v = d.view(u32s({1}));
    const int v2 = d.view(i16s({32767, 0, -32767}));
    const int sp2 = d.acc(-1, 5122, 3, "VEC3", ",\"normalized\":true,\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" +
                          std::to_string(i32v) + ",\"componentType\":5125},\"values\":{\"bufferView\":" + std::to_string(v2) + "}}");
    const std::string json = d.json(std::string(kScene) +
        ",\"meshes\":[{\"extras\":{\"targetNames\":[\"u16\",\"u32n\"]},\"primitives\":[" +
        primJson(t, "{\"POSITION\":" + std::to_string(sp1) + "},{\"POSITION\":" + std::to_string(sp2) + "}") + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "a document with sparse morph accessors imports: " + why);
    if (res.meshes.empty()) return;
    const fmt::OcMorphTarget* a = targetNamed(res.meshes[0], "u16");
    const fmt::OcMorphTarget* b = targetNamed(res.meshes[0], "u32n");
    check(a && b, "both targets present");
    if (!a || !b) return;
    check(a->vertices == std::vector<u32>({0, 2}), "u16 sparse indices select vertices 0 and 2");
    checkNear(a->positionDeltas[1], 100.0f, 1e-3f, "vertex 0: glTF +X -> engine +Y");
    checkNear(a->positionDeltas[5], 200.0f, 1e-3f, "vertex 2: glTF +Y*2 -> engine +Z*200");
    check(b->vertices == std::vector<u32>({1}), "u32 sparse index selects vertex 1");
    checkNear(b->positionDeltas[0], 100.0f, 1e-2f, "normalized short -1 on z -> engine +X 100");
    checkNear(b->positionDeltas[1], 100.0f, 1e-2f, "normalized short +1 on x -> engine +Y 100");

    // failures
    {
        Doc e;
        const Tri te = addTriangle(e);
        const int iv = e.view(u16s({2, 1}));
        const int vv = e.view(floats({1, 0, 0,  0, 2, 0}));
        const int sp = e.acc(-1, 5126, 3, "VEC3", ",\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":" + std::to_string(iv) +
                             ",\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(vv) + "}}");
        const std::string bad = e.json(std::string(kScene) + ",\"meshes\":[{\"primitives\":[" +
                                       primJson(te, "{\"POSITION\":" + std::to_string(sp) + "}") + "]}]");
        fmt::GltfImportResult r2; std::string w2;
        check(!importDoc(bad, r2, &w2) && w2.find("sparse") != std::string::npos, "descending sparse indices fail with a why: " + w2);
    }
    {
        Doc e;
        const Tri te = addTriangle(e);
        const int iv = e.view(u16s({0, 9}));
        const int vv = e.view(floats({1, 0, 0,  0, 2, 0}));
        const int sp = e.acc(-1, 5126, 3, "VEC3", ",\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":" + std::to_string(iv) +
                             ",\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(vv) + "}}");
        const std::string bad = e.json(std::string(kScene) + ",\"meshes\":[{\"primitives\":[" +
                                       primJson(te, "{\"POSITION\":" + std::to_string(sp) + "}") + "]}]");
        fmt::GltfImportResult r2; std::string w2;
        check(!importDoc(bad, r2, &w2) && w2.find("sparse") != std::string::npos, "an out-of-range sparse index fails with a why: " + w2);
    }
}

static void testTwoPrimitives() {
    AVER_INFO("=== primitives with different target counts ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int dpA = d.vec3({1, 0, 0,  1, 0, 0,  1, 0, 0});
    const int dpB = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});
    const std::string p0 = primJson(t, "{\"POSITION\":" + std::to_string(dpA) + "},{\"POSITION\":" + std::to_string(dpB) + "}");
    const std::string p1 = primJson(t, "{\"POSITION\":" + std::to_string(dpB) + "}");
    const std::string json = d.json(std::string(kScene) +
        ",\"meshes\":[{\"extras\":{\"targetNames\":[\"a\",\"b\"]},\"primitives\":[" + p0 + "," + p1 + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    if (res.meshes.empty()) return;
    const fmt::OcMeshData& m = res.meshes[0];
    check(m.vertexCount() == 6, "six vertices from two primitives");
    const fmt::OcMorphTarget* a = targetNamed(m, "a");
    const fmt::OcMorphTarget* b = targetNamed(m, "b");
    check(a && b, "the target count is the maximum over primitives");
    if (!a || !b) return;
    check(a->vertices == std::vector<u32>({0, 1, 2, 3, 4, 5}), "target a covers both primitives, the second at baseVertex 3");
    check(b->vertices == std::vector<u32>({0, 1, 2}), "target b exists only on the first primitive");
}

static void testSkinnedMorph() {
    AVER_INFO("=== skinned + morph ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int joints = d.acc(d.view({0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0}), 5121, 3, "VEC4");
    const int w4 = d.acc(d.view(floats({1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0})), 5126, 3, "VEC4");
    const int dp = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});
    const std::string prim = "{\"attributes\":{\"POSITION\":" + std::to_string(t.pos) + ",\"NORMAL\":" + std::to_string(t.nrm) +
        ",\"JOINTS_0\":" + std::to_string(joints) + ",\"WEIGHTS_0\":" + std::to_string(w4) + "},\"indices\":" + std::to_string(t.idx) +
        ",\"targets\":[{\"POSITION\":" + std::to_string(dp) + "}]}";
    const std::string json = d.json(
        "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],\"nodes\":[{\"mesh\":0,\"skin\":0},{\"name\":\"jaw\"}],"
        "\"skins\":[{\"joints\":[1]}],\"meshes\":[{\"primitives\":[" + prim + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    if (res.meshes.empty()) return;
    check(res.meshes[0].hasSkin(), "the skin survives");
    check(res.meshes[0].hasMorph(), "and so do the blend shapes");
    check(res.meshes[0].valid(), "the mesh is valid");
}

static void testReports() {
    AVER_INFO("=== what is still reported ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int dp = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});
    const int tan = d.vec3({0, 1, 0,  0, 1, 0,  0, 1, 0});
    const std::string json = d.json(
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"weights\":[0.5]}],"
        "\"meshes\":[{\"primitives\":[" + primJson(t, "{\"POSITION\":" + std::to_string(dp) + ",\"TANGENT\":" + std::to_string(tan) + "}") + "]}]");
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    check(noted(res, "morph tangents"), "tangent deltas are reported as dropped");
    check(noted(res, "node-level morph weights"), "node-level weights are reported");
    check(!noted(res, "morph targets"), "morph targets themselves are not");
}

// Output layouts: LINEAR [k0t0,k0t1,k1t0,k1t1]; CUBIC per key: in(T), value(T), out(T).
static void testWeightAnimation() {
    AVER_INFO("=== weights animation -> morph.<target> curves ===");
    Doc d;
    const Tri t = addTriangle(d);
    const int dpA = d.vec3({1, 0, 0,  0, 0, 0,  0, 0, 0});
    const int dpB = d.vec3({0, 1, 0,  0, 0, 0,  0, 0, 0});
    const int times = d.scalar({0.0f, 1.0f});
    const int lin = d.scalar({0.0f, 0.5f,  1.0f, 0.25f});
    const int cub = d.scalar({0, 0,  0, 0.5f,  2, 4,      6, 8,  1, 0.25f,  0, 0});
    const std::string mesh = "\"meshes\":[{\"extras\":{\"targetNames\":[\"smile\",\"blink\"]},\"primitives\":[" +
        primJson(t, "{\"POSITION\":" + std::to_string(dpA) + "},{\"POSITION\":" + std::to_string(dpB) + "}") + "]}]";
    const auto chan = [](int sampler, int node) {
        return "{\"sampler\":" + std::to_string(sampler) + ",\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"weights\"}}";
    };
    const std::string anims = "\"animations\":["
        "{\"name\":\"lin\",\"channels\":[" + chan(0, 0) + "],\"samplers\":[{\"input\":" + std::to_string(times) + ",\"output\":" + std::to_string(lin) + ",\"interpolation\":\"LINEAR\"}]},"
        "{\"name\":\"step\",\"channels\":[" + chan(0, 0) + "],\"samplers\":[{\"input\":" + std::to_string(times) + ",\"output\":" + std::to_string(lin) + ",\"interpolation\":\"STEP\"}]},"
        "{\"name\":\"cub\",\"channels\":[" + chan(0, 0) + "],\"samplers\":[{\"input\":" + std::to_string(times) + ",\"output\":" + std::to_string(cub) + ",\"interpolation\":\"CUBICSPLINE\"}]},"
        "{\"name\":\"twice\",\"channels\":[" + chan(0, 0) + "," + chan(0, 1) + "],\"samplers\":[{\"input\":" + std::to_string(times) + ",\"output\":" + std::to_string(lin) + "}]}"
        "]";
    const std::string json = d.json("\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],\"nodes\":[{\"mesh\":0},{\"mesh\":0,\"translation\":[5,0,0]}]," + mesh + "," + anims);
    fmt::GltfImportResult res; std::string why;
    check(importDoc(json, res, &why), "imports: " + why);
    check(!noted(res, "morph-target animation"), "'morph-target animation' is no longer reported");
    check(res.animations.size() == 4 && res.animationNames.size() == 4, "four curve-only clips, names parallel");
    if (res.animations.size() != 4) return;
    for (usize i = 0; i < 4; ++i)
        check(res.animations[i].tracks.empty() && res.animations[i].valid() && res.animations[i].skeletonRef.empty(),
              "clip " + std::to_string(i) + " has no tracks, no skeleton and is valid");
    check(res.animationNames[0] == "lin" && res.animationNames[2] == "cub", "clip names");

    const fmt::OcAnimation& lin0 = res.animations[0];
    const fmt::OcCurve* ls = curveNamed(lin0, "morph.smile");
    const fmt::OcCurve* lb = curveNamed(lin0, "morph.blink");
    check(ls && lb && lin0.curves.size() == 2, "one curve per target");
    if (ls && lb) {
        check(ls->interp == fmt::OcInterp::Linear && ls->times == std::vector<f32>({0, 1}), "linear times");
        check(ls->values == std::vector<f32>({0.0f, 1.0f}) && lb->values == std::vector<f32>({0.5f, 0.25f}),
              "linear values are de-interleaved per target");
        check(ls->inTangents.empty(), "no tangents on a linear curve");
    }
    checkNear(lin0.duration, 1.0f, 1e-6f, "the clip lasts to its last key");
    const fmt::OcCurve* ss = curveNamed(res.animations[1], "morph.smile");
    check(ss && ss->interp == fmt::OcInterp::Step, "STEP maps to a step curve");
    const fmt::OcCurve* cs = curveNamed(res.animations[2], "morph.smile");
    const fmt::OcCurve* cb = curveNamed(res.animations[2], "morph.blink");
    check(cs && cb && cs->interp == fmt::OcInterp::CubicSpline, "CUBICSPLINE maps to a cubic curve");
    if (cs && cb) {
        check(cs->values == std::vector<f32>({0.0f, 1.0f}) && cb->values == std::vector<f32>({0.5f, 0.25f}), "cubic values");
        check(cs->inTangents == std::vector<f32>({0.0f, 6.0f}) && cs->outTangents == std::vector<f32>({2.0f, 0.0f}), "cubic tangents, smile");
        check(cb->inTangents == std::vector<f32>({0.0f, 8.0f}) && cb->outTangents == std::vector<f32>({4.0f, 0.0f}), "cubic tangents, blink");
    }
    check(res.animations[3].curves.size() == 2, "two nodes animating the same targets keep one curve each");

    // the curve-only clip round-trips through the .ocanim container
    std::vector<u8> bytes; std::string w2;
    check(fmt::writeOcAnim(res.animations[2], bytes, &w2), "a curve-only clip writes: " + w2);
    fmt::OcAnimation back;
    check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &w2), "and reads back: " + w2);
    check(back.tracks.empty() && back.valid() && back.curves.size() == 2, "with its curves and no tracks");
    const fmt::OcCurve* bs = curveNamed(back, "morph.smile");
    check(bs && bs->poseId == fnv1a64(std::string_view("smile")), "parse fills the pose id (hash of the bare name)");
    check(bs && bs->inTangents == cs->inTangents && bs->outTangents == cs->outTangents, "tangents survive the container");
}

int main() {
    testTargets();
    testNormalDeltaScale();
    testMirrorAndCopy();
    testSparse();
    testTwoPrimitives();
    testSkinnedMorph();
    testReports();
    testWeightAnimation();
    if (g_failures == 0) AVER_INFO("=== all morph glTF tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
