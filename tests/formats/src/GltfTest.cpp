// The glTF importer: .gltf with a base64 data URI, .glb with a binary chunk, the right-handed to
// left-handed coordinate change, and what the importer refuses. Documents are built by hand here.
#include "aver/formats/GltfImport.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

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

// Base64-encodes a byte blob, so a .gltf case can embed its buffer as a data URI.
static std::string b64(const std::vector<u8>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < in.size(); i += 3) {
        const u32 a = in[i], b = i+1 < in.size() ? in[i+1] : 0, c = i+2 < in.size() ? in[i+2] : 0;
        const u32 v = (a << 16) | (b << 8) | c;
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(i+1 < in.size() ? T[(v >> 6) & 63] : '=');
        out.push_back(i+2 < in.size() ? T[v & 63] : '=');
    }
    return out;
}

// Appends n raw bytes.
static void put(std::vector<u8>& b, const void* p, usize n) {
    const u8* s = static_cast<const u8*>(p);
    b.insert(b.end(), s, s + n);
}
// Appends a 32-bit float.
static void putF(std::vector<u8>& b, f32 f) { put(b, &f, 4); }
// Appends a 16-bit unsigned integer.
static void putU16(std::vector<u8>& b, u16 v) { put(b, &v, 2); }
// Appends a 32-bit unsigned integer.
static void putU32(std::vector<u8>& b, u32 v) { put(b, &v, 4); }

// A glTF buffer holding one triangle, with the view ranges for its positions and indices.
struct Tri { std::vector<u8> bin; usize posOff, posLen, idxOff, idxLen; };

// Builds one triangle in glTF space, wound so its face points along glTF up (+Y).
static Tri makeTriangleBuffer() {
    Tri t;
    t.posOff = 0;
    putF(t.bin, 0); putF(t.bin, 0); putF(t.bin, 0);
    putF(t.bin, 1); putF(t.bin, 0); putF(t.bin, 0);
    putF(t.bin, 0); putF(t.bin, 0); putF(t.bin, -1);
    t.posLen = t.bin.size();
    while (t.bin.size() % 4) t.bin.push_back(0);
    t.idxOff = t.bin.size();
    putU16(t.bin, 0); putU16(t.bin, 1); putU16(t.bin, 2);
    t.idxLen = t.bin.size() - t.idxOff;
    while (t.bin.size() % 4) t.bin.push_back(0);
    return t;
}

// Builds the glTF JSON for one triangle, with the buffer embedded or left external.
static std::string triangleJson(usize posOff, usize posLen, usize idxOff, usize idxLen, bool embedded,
                                const std::string& b64data) {
    std::string uri = embedded ? ("\"uri\":\"data:application/octet-stream;base64," + b64data + "\",") : "";
    return std::string("{")
      + "\"asset\":{\"version\":\"2.0\"},"
      + "\"scene\":0,"
      + "\"scenes\":[{\"nodes\":[0]}],"
      + "\"nodes\":[{\"mesh\":0}],"
      + "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
      + "\"buffers\":[{" + uri + "\"byteLength\":" + std::to_string(posLen + idxLen + 8) + "}],"
      + "\"bufferViews\":["
        + "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(posLen) + "},"
        + "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxLen) + "}"
      + "],"
      + "\"accessors\":["
        + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
      + "]}";
}

// Wraps JSON and binary into a GLB container.
static std::vector<u8> makeGlb(const std::string& json, const std::vector<u8>& bin) {
    std::string j = json;
    while (j.size() % 4) j.push_back(' ');           // JSON chunk pads with spaces
    std::vector<u8> binPad = bin;
    while (binPad.size() % 4) binPad.push_back(0);   // BIN chunk pads with zeros

    std::vector<u8> g;
    put(g, "glTF", 4);
    putU32(g, 2);
    putU32(g, static_cast<u32>(12 + 8 + j.size() + 8 + binPad.size()));
    putU32(g, static_cast<u32>(j.size()));
    putU32(g, 0x4E4F534A);                            // 'JSON'
    put(g, j.data(), j.size());
    putU32(g, static_cast<u32>(binPad.size()));
    putU32(g, 0x004E4942);                            // 'BIN\0'
    put(g, binPad.data(), binPad.size());
    return g;
}

// The normalised face normal of the first imported triangle, from its winding.
static void faceNormal(const fmt::OcMeshData& m, f32 out[3]) {
    const u32 ia = m.indices[0], ib = m.indices[1], ic = m.indices[2];
    const f32* A = &m.positions[usize(ia)*3];
    const f32* B = &m.positions[usize(ib)*3];
    const f32* C = &m.positions[usize(ic)*3];
    const f32 e1[3] = {B[0]-A[0], B[1]-A[1], B[2]-A[2]};
    const f32 e2[3] = {C[0]-A[0], C[1]-A[1], C[2]-A[2]};
    out[0] = e1[1]*e2[2]-e1[2]*e2[1];
    out[1] = e1[2]*e2[0]-e1[0]*e2[2];
    out[2] = e1[0]*e2[1]-e1[1]*e2[0];
    const f32 len = std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
    if (len > 1e-12f) { out[0]/=len; out[1]/=len; out[2]/=len; }
}

// Runs every glTF import test. Returns 0 when they all pass.
int main() {
    const Tri tri = makeTriangleBuffer();

    AVER_INFO("=== .gltf with an embedded base64 buffer ===");
    {
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        fmt::GltfImportResult res;
        std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports: " + why);
        check(res.meshes.size() == 1, "one mesh");
        check(res.meshNames.size() == 1 && res.meshNames[0] == "Tri", "mesh name survives");
        check(res.meshes[0].vertexCount() == 3, "three vertices");
        check(res.meshes[0].indices.size() == 3, "three indices");
        check(res.meshes[0].submeshes.size() == 1, "one submesh per primitive");
    }

    AVER_INFO("=== .glb with a real binary chunk ===");
    {
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, false, "");
        const std::vector<u8> glb = makeGlb(json, tri.bin);
        fmt::GltfImportResult res;
        std::string why;
        check(fmt::importGltfFromMemory(glb.data(), glb.size(), "", res, {}, &why), "imports: " + why);
        check(res.meshes.size() == 1 && res.meshes[0].vertexCount() == 3, "same mesh out of the GLB path");
    }

    AVER_INFO("=== the coordinate change ===");
    {
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        fmt::GltfImportResult res;
        std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports for the axis check");
        const fmt::OcMeshData& m = res.meshes[0];

        checkNear(m.positions[3], 0.0f,   1e-4f, "glTF +X does not land on engine X");
        checkNear(m.positions[4], 100.0f, 1e-3f, "glTF +X becomes engine +Y, scaled to centimetres");
        checkNear(m.positions[5], 0.0f,   1e-4f, "glTF +X does not land on engine Z");

        checkNear(m.positions[6], 100.0f, 1e-3f, "glTF -Z (forward) becomes engine +X");
        checkNear(m.positions[7], 0.0f,   1e-4f, "and nothing on Y");

        f32 n[3]; faceNormal(m, n);
        checkNear(n[2], 1.0f, 1e-3f, "the face still points UP after the handedness change (winding was reversed)");
    }

    AVER_INFO("=== convertAxes off leaves the data alone ===");
    {
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        fmt::GltfImportOptions opt; opt.convertAxes = false; opt.scale = 1.0f;
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, opt, &why),
              "imports unconverted");
        checkNear(res.meshes[0].positions[3], 1.0f, 1e-6f, "raw glTF +X stays on X at scale 1");
        check(res.meshes[0].indices[1] == 1 && res.meshes[0].indices[2] == 2, "winding untouched when not converting");
    }

    AVER_INFO("=== node transforms are applied ===");
    {
        std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        const std::string from = "\"nodes\":[{\"mesh\":0}]";
        const std::string to   = "\"nodes\":[{\"mesh\":0,\"translation\":[2,0,0]}]";
        const usize at = json.find(from);
        check(at != std::string::npos, "test fixture patched");
        json.replace(at, from.size(), to);

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports with a node translation: " + why);
        checkNear(res.meshes[0].positions[1], 200.0f, 1e-2f, "node translation lands on engine +Y in centimetres");
    }

    AVER_INFO("=== generated normals face outward ===");
    {
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports without a NORMAL attribute");
        checkNear(res.meshes[0].normals[2], 1.0f, 1e-3f, "generated normal points up, agreeing with the winding");
    }

    AVER_INFO("=== malformed input is refused ===");
    {
        fmt::GltfImportResult res; std::string why;
        const std::string notJson = "this is not json";
        check(!fmt::importGltfFromMemory(reinterpret_cast<const u8*>(notJson.data()), notJson.size(), "", res, {}, &why),
              "refuses a non-JSON file");

        const std::string v1 = "{\"asset\":{\"version\":\"1.0\"},\"meshes\":[]}";
        check(!fmt::importGltfFromMemory(reinterpret_cast<const u8*>(v1.data()), v1.size(), "", res, {}, &why),
              "refuses glTF 1.0");

        const std::string noMesh = "{\"asset\":{\"version\":\"2.0\"}}";
        check(!fmt::importGltfFromMemory(reinterpret_cast<const u8*>(noMesh.data()), noMesh.size(), "", res, {}, &why),
              "refuses a file with no meshes");

        const std::string external = "{\"asset\":{\"version\":\"2.0\"},"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
            "\"buffers\":[{\"uri\":\"missing.bin\",\"byteLength\":4}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}]}";
        check(!fmt::importGltfFromMemory(reinterpret_cast<const u8*>(external.data()), external.size(), "", res, {}, &why),
              "refuses an external buffer with no directory context");

        std::vector<u8> shortGlb = makeGlb(triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, false, ""), tri.bin);
        shortGlb.resize(shortGlb.size() / 2);
        check(!fmt::importGltfFromMemory(shortGlb.data(), shortGlb.size(), "", res, {}, &why),
              "refuses a truncated GLB");
    }

    AVER_INFO("=== what it could not carry is NAMED ===");
    {
        std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        const std::string from = "\"meshes\":[";
        const usize at = json.find(from);
        json.insert(at, "\"animations\":[{\"channels\":[],\"samplers\":[]}],\"skins\":[{\"joints\":[0]}],");

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "still imports the geometry: " + why);
        bool sawSkins = false, sawAnims = false;
        for (const std::string& s : res.unsupported) {
            if (s.find("skins") != std::string::npos) sawSkins = true;
            if (s.find("animations") != std::string::npos) sawAnims = true;
        }
        check(sawSkins, "skins reported as unsupported by name");
        check(sawAnims, "animations reported as unsupported by name");
    }

    if (g_failures == 0) AVER_INFO("=== all glTF import tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
