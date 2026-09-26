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

// The same triangle, plus whatever materials/images/textures the caller wants spliced in. The mesh's
// primitive is given material 0 so the slot naming is exercised too.
//
// THE IMAGE BYTES NEED NOT BE A REAL PNG. The importer copies an image's bytes verbatim and never
// decodes them, so a recognisable seven-byte pattern is a BETTER fixture than a real file: any
// corruption is visible in the assertion rather than hidden inside a compressor.
static std::string materialJson(usize posOff, usize posLen, usize idxOff, usize idxLen,
                                const std::string& b64data, const std::string& extraTopLevel) {
    return std::string("{")
      + "\"asset\":{\"version\":\"2.0\"},"
      + "\"scene\":0,"
      + "\"scenes\":[{\"nodes\":[0]}],"
      + "\"nodes\":[{\"mesh\":0}],"
      + "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"material\":0}]}],"
      + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64data + "\","
        "\"byteLength\":" + std::to_string(posLen + idxLen + 8) + "}],"
      + "\"bufferViews\":["
        + "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(posLen) + "},"
        + "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxLen) + "}"
      + "],"
      + "\"accessors\":["
        + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
      + "]," + extraTopLevel + "}";
}

// True when any unsupported note contains `needle` -- the notes are prose, so this asks whether the
// importer SAID the thing, not whether it said it in one exact form.
static bool noted(const fmt::GltfImportResult& r, const std::string& needle) {
    for (const std::string& s : r.unsupported) if (s.find(needle) != std::string::npos) return true;
    return false;
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
        check(res.meshSkinIndex.size() == 1 && res.meshSkinIndex[0] == -1,
              "an unskinned mesh's meshSkinIndex is -1");
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

    AVER_INFO("=== a node's TRANSLATION becomes a placement, not geometry ===");
    {
        // THIS TEST USED TO ASSERT THE OPPOSITE, and asserting it is what kept the bug alive: it
        // checked that a node translated by 2 m produced a VERTEX at 200 cm, i.e. that the
        // translation had been welded into the geometry. That is exactly what put every imported
        // mesh's pivot metres away from itself -- measured on Intel Sponza as 115 of 115 meshes
        // displaced, median 10.7 m, and an arch piece reporting a 14.2 m bounding radius for
        // geometry a few metres across. The gizmo, the bounds, culling and F-focus all read that
        // gap as real.
        //
        // The contract now: rotation and scale stay baked (they are what the mesh looks like), the
        // translation comes out as a GltfPlacement. Both halves are asserted, because either alone
        // can pass while the feature is broken -- geometry at the origin with no placement has
        // simply lost the scene, and a placement whose geometry is still displaced double-counts.
        std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        const std::string from = "\"nodes\":[{\"mesh\":0}]";
        const std::string to   = "\"nodes\":[{\"mesh\":0,\"translation\":[2,0,0]}]";
        const usize at = json.find(from);
        check(at != std::string::npos, "test fixture patched");
        json.replace(at, from.size(), to);

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports with a node translation: " + why);

        // The vertex is where it would have been with no node transform at all: glTF +X (the
        // triangle's own second vertex) still maps to engine +Y at 100 cm, NOT 300 cm.
        checkNear(res.meshes[0].positions[4], 100.0f, 1e-2f,
                  "the node translation is NOT welded into the vertex");
        checkNear(res.meshes[0].positions[1], 0.0f, 1e-2f,
                  "and the first vertex stays at the mesh's own origin");

        // ...and the 2 m the geometry no longer carries is in the placement instead, through the
        // same axis/unit conversion a position gets: glTF +X -> engine +Y, metres -> centimetres.
        check(res.placements.size() == 1, "one placement, for the one node that instances a mesh");
        if (res.placements.size() == 1) {
            check(res.placements[0].meshIndex == 0, "the placement names mesh 0");
            checkNear(res.placements[0].position.y, 200.0f, 1e-2f,
                      "glTF +X translation of 2 m becomes engine +Y at 200 cm in the placement");
            checkNear(res.placements[0].position.x, 0.0f, 1e-2f, "nothing on engine X");
            checkNear(res.placements[0].position.z, 0.0f, 1e-2f, "nothing on engine Z");
        }
    }

    AVER_INFO("=== a node at the origin produces a placement at the origin ===");
    {
        // The ordinary single-object export. A placement still exists -- one per node that
        // instances a mesh -- it is simply at zero, so a caller can treat placements uniformly
        // rather than special-casing "no transform".
        const std::string json = triangleJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, true, b64(tri.bin));
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "imports the untransformed fixture: " + why);
        check(res.placements.size() == 1, "an untransformed node still yields one placement");
        if (res.placements.size() == 1)
            checkNear(res.placements[0].position.x + res.placements[0].position.y +
                      res.placements[0].position.z, 0.0f, 1e-4f, "and it sits at the origin");
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
        check(!sawSkins, "skins are no longer listed as unsupported -- they are imported");
        check(res.skeletons.size() == 1 && res.skeletons[0].bones.size() == 1,
              "a one-joint skin imports as a one-bone skeleton");
        check(sawAnims || res.animations.empty(), "an animation with no channels produces no clip");
    }

    AVER_INFO("=== skins and animations ===");
    {
        // A two-bone rig, a skinned triangle and one translation clip. The skin lists its joints
        // CHILD FIRST on purpose: .ocskel requires parents before children and glTF promises no
        // such thing, so this is the case that has to be reordered -- and the mesh's joint indices
        // reordered with it, or every vertex silently names the wrong bone.
        std::vector<u8> bin;
        const usize posOff = bin.size();
        putF(bin, 0); putF(bin, 0); putF(bin, 0);
        putF(bin, 1); putF(bin, 0); putF(bin, 0);
        putF(bin, 0); putF(bin, 0); putF(bin, -1);
        const usize posLen = bin.size() - posOff;

        const usize jOff = bin.size();
        for (int v = 0; v < 3; ++v) { putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); }
        const usize jLen = bin.size() - jOff;

        const usize wOff = bin.size();
        for (int v = 0; v < 3; ++v) { putF(bin, 1); putF(bin, 0); putF(bin, 0); putF(bin, 0); }
        const usize wLen = bin.size() - wOff;

        const usize iOff = bin.size();
        putU16(bin, 0); putU16(bin, 1); putU16(bin, 2);
        const usize iLen = bin.size() - iOff;
        while (bin.size() % 4) bin.push_back(0);

        const usize tOff = bin.size();
        putF(bin, 0.0f); putF(bin, 0.5f);
        const usize tLen = bin.size() - tOff;

        const usize vOff = bin.size();
        putF(bin, 0); putF(bin, 0); putF(bin, 0);
        putF(bin, 0); putF(bin, 2); putF(bin, 0);       // 2 metres along glTF up
        const usize vLen = bin.size() - vOff;

        auto view = [](usize off, usize len) {
            return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                   ",\"byteLength\":" + std::to_string(len) + "}";
        };
        const std::string s45 = "0.70710678";
        const std::string json = std::string("{")
          + "\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
          + "\"scenes\":[{\"nodes\":[0,2]}],"
          + "\"nodes\":["
            + "{\"name\":\"root\",\"children\":[1]},"
            + "{\"name\":\"child\",\"translation\":[0,1,0],\"rotation\":[0," + s45 + ",0," + s45 + "]},"
            + "{\"name\":\"body\",\"mesh\":0,\"skin\":0}"
          + "],"
          + "\"skins\":[{\"name\":\"rig\",\"joints\":[1,0]}],"
          + "\"meshes\":[{\"name\":\"Body\",\"primitives\":[{\"attributes\":"
            + "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]}],"
          + "\"animations\":[{\"name\":\"wave\",\"channels\":[{\"sampler\":0,"
            + "\"target\":{\"node\":1,\"path\":\"translation\"}}],"
            + "\"samplers\":[{\"input\":4,\"output\":5,\"interpolation\":\"LINEAR\"}]}],"
          + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin)
            + "\",\"byteLength\":" + std::to_string(bin.size()) + "}],"
          + "\"bufferViews\":[" + view(posOff,posLen) + "," + view(jOff,jLen) + "," + view(wOff,wLen)
            + "," + view(iOff,iLen) + "," + view(tOff,tLen) + "," + view(vOff,vLen) + "],"
          + "\"accessors\":["
            + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
            + "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
            + "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
          + "]}";

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a skinned, animated glTF imports: " + why);

        check(res.skeletons.size() == 1, "one skeleton");
        check(res.skeletonNames.size() == 1 && res.skeletonNames[0] == "rig", "carrying the skin's name");
        if (res.skeletons.size() == 1) {
            const fmt::OcSkeleton& sk = res.skeletons[0];
            check(sk.bones.size() == 2, "with both joints");
            check(sk.valid(), "and it is a VALID skeleton -- parents before children");
            check(sk.bones[0].name == "root" && sk.bones[1].name == "child",
                  "the child-first joint list was REORDERED, not taken as authored");
            check(sk.bones[0].parent == -1, "the root has no parent");
            check(sk.bones[1].parent == 0, "and the child points at it by its NEW index");

            // 1 metre along glTF up is 100 cm along engine +Z.
            checkNear(sk.bones[1].translation.z, 100.0f, 1e-3f, "a joint's offset is metres to centimetres");
            checkNear(sk.bones[1].translation.x, 0.0f, 1e-3f, "on the right axis");

            // THE CONVERSION THAT IS EASIEST TO GET WRONG. The basis change has determinant -1, so
            // a rotation does not simply permute its components -- the sense of the turn flips too.
            // Checked by ROTATING A VECTOR rather than by comparing quaternion components, so it
            // cannot agree with a wrong formula that happens to match the implementation.
            //
            // +90 degrees about glTF +Y takes glTF +X to glTF -Z. Those are engine +Y and engine +X,
            // so the imported rotation must take engine +Y to engine +X.
            const Vec3 got = sk.bones[1].rotation.rotate(Vec3{0, 1, 0});
            checkNear(got.x, 1.0f, 1e-3f, "the imported rotation takes engine +Y to engine +X");
            checkNear(got.y, 0.0f, 1e-3f, "with nothing left on +Y");
            checkNear(got.z, 0.0f, 1e-3f, "and nothing on +Z");
        }

        check(res.meshes.size() == 1 && res.meshes[0].hasSkin(), "the mesh comes back skinned");
        if (!res.meshes.empty() && res.meshes[0].hasSkin()) {
            const fmt::OcMeshData& m = res.meshes[0];
            bool remapped = true, weighted = true;
            for (u32 v = 0; v < m.vertexCount(); ++v) {
                if (m.joints[usize(v) * fmt::kOcMeshInfluences] != 1) remapped = false;
                if (std::fabs(m.weights[usize(v) * fmt::kOcMeshInfluences] - 1.0f) > 1e-6f) weighted = false;
            }
            check(remapped, "every vertex's joint index was REMAPPED through the reorder (0 -> 1)");
            check(weighted, "and its weight survived");
        }

        check(res.animations.size() == 1, "one clip");
        check(res.animationNames.size() == 1 && res.animationNames[0] == "wave", "carrying its name");
        if (res.animations.size() == 1) {
            const fmt::OcAnimation& a = res.animations[0];
            check(a.valid(), "the clip is valid");
            checkNear(a.duration, 0.5f, 1e-5f, "its duration is the last key's time");
            check(a.tracks.size() == 1, "one track");
            if (a.tracks.size() == 1) {
                const fmt::OcTrack& t = a.tracks[0];
                check(t.boneIndex == 1, "targeting the reordered bone, not the file's index");
                check(t.channels == fmt::kOcChannelTranslation, "on the translation channel");
                check(t.interp == fmt::OcInterp::Linear, "with the sampler's interpolation");
                check(t.times.size() == 2 && t.values.size() == 6, "two keys of three floats");
                checkNear(t.values[5], 200.0f, 1e-2f,
                          "and 2 metres along glTF up became 200 cm along engine +Z");
            }
        }
    }

    AVER_INFO("=== sockets: an Empty parented to a bone ===");
    {
        // Nothing in glTF calls anything a socket, so this is a RULE the importer applies rather
        // than a field it reads. The rig below carries one node of every shape the rule has to
        // decide about, and each is asserted individually -- a single "two sockets were found"
        // check would pass while the importer accepted the wrong two.
        //
        //   0 root     joint
        //   1 child    joint, under root
        //   2 body     MESH under child                     -> rejected: geometry, not a socket
        //   3 Grip     empty under child                    -> ACCEPTED
        //   4 Muzzle   empty under root                     -> ACCEPTED
        //   5 spacer   empty under root, WITH A JOINT UNDER -> rejected: a spacer between bones
        //   6 loose    empty at the scene root              -> rejected: no bone to hang from
        //   7 tip      joint, under spacer
        //
        // THE JSON IS A RAW STRING LITERAL. Every other fixture in this file escapes each quote by
        // hand, which makes a nine-node hierarchy unreadable and was how the first attempt at this
        // test was silently mangled. Only the base64 buffer is spliced in.
        std::vector<u8> bin;
        const usize posOff = bin.size();
        putF(bin, 0); putF(bin, 0); putF(bin, 0);
        putF(bin, 1); putF(bin, 0); putF(bin, 0);
        putF(bin, 0); putF(bin, 0); putF(bin, -1);
        const usize posLen = bin.size() - posOff;
        const usize jOff = bin.size();
        for (int v = 0; v < 3; ++v) { putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); }
        const usize jLen = bin.size() - jOff;
        const usize wOff = bin.size();
        for (int v = 0; v < 3; ++v) { putF(bin, 1); putF(bin, 0); putF(bin, 0); putF(bin, 0); }
        const usize wLen = bin.size() - wOff;
        const usize iOff = bin.size();
        putU16(bin, 0); putU16(bin, 1); putU16(bin, 2);
        const usize iLen = bin.size() - iOff;

        auto view = [](usize off, usize len) {
            return R"({"buffer":0,"byteOffset":)" + std::to_string(off) +
                   R"(,"byteLength":)" + std::to_string(len) + "}";
        };

        const std::string json = std::string(R"JSON({
          "asset":{"version":"2.0"}, "scene":0,
          "scenes":[{"nodes":[0,6]}],
          "nodes":[
            {"name":"root",   "children":[1,4,5]},
            {"name":"child",  "translation":[0,1,0], "children":[2,3]},
            {"name":"body",   "mesh":0, "skin":0},
            {"name":"Grip",   "translation":[0,0,2]},
            {"name":"Muzzle"},
            {"name":"spacer", "children":[7]},
            {"name":"loose"},
            {"name":"tip"}
          ],
          "skins":[{"name":"rig","joints":[0,1,7]}],
          "meshes":[{"primitives":[{"attributes":
            {"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3}]}],
          "buffers":[{"uri":"data:application/octet-stream;base64,)JSON") + b64(bin)
          + R"JSON(","byteLength":)JSON" + std::to_string(bin.size()) + "}],"
          + R"JSON("bufferViews":[)JSON" + view(posOff,posLen) + "," + view(jOff,jLen) + ","
          + view(wOff,wLen) + "," + view(iOff,iLen) + "],"
          + R"JSON("accessors":[
            {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},
            {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
            {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}
          ]})JSON";

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a rig with socket empties imports: " + why);
        check(res.skeletons.size() == 1, "one skeleton");
        if (res.skeletons.size() == 1) {
            const fmt::OcSkeleton& sk = res.skeletons[0];
            check(sk.sockets.size() == 2, "EXACTLY TWO sockets -- the two empties on bones, and no more");
            check(sk.socket("Grip") != nullptr, "the empty under a bone is a socket");
            check(sk.socket("Muzzle") != nullptr, "and so is the one under the root bone");
            check(sk.socket("body") == nullptr,
                  "A MESH PARENTED TO A BONE IS NOT A SOCKET -- every skinned character export has one");
            check(sk.socket("spacer") == nullptr,
                  "NOR IS A SPACER WITH A JOINT UNDER IT -- glTF allows non-joint nodes between bones");
            check(sk.socket("loose") == nullptr, "nor an empty that is not on a bone at all");
            check(sk.socket("tip") == nullptr, "and a joint is a bone, not a socket");

            if (const fmt::OcSocket* g = sk.socket("Grip")) {
                check(g->bone < sk.bones.size() && sk.bones[g->bone].name == "child",
                      "Grip hangs from the bone it was parented to, by its POST-SORT index");
                // 2 metres along glTF +Z. Whatever the engine basis does with that, it must be the
                // SAME thing it did to the bone rest transforms -- a socket offset is a bone-local
                // transform exactly as a joint's is.
                const f32 mag = std::fabs(g->translation.x) + std::fabs(g->translation.y)
                              + std::fabs(g->translation.z);
                check(mag > 1.0f, "and carries a real converted offset, not zero");
            }
            if (const fmt::OcSocket* m = sk.socket("Muzzle"))
                check(m->bone < sk.bones.size() && sk.bones[m->bone].name == "root",
                      "Muzzle hangs from the root bone");
            check(sk.valid(), "and the rig is still valid with sockets on it");
        }
    }

    AVER_INFO("=== a joint stored as a baked matrix, rather than as TRS ===");
    {
        // THE SAME RIG, SPELLED BOTH WAYS, MUST IMPORT THE SAME. glTF lets a node give its
        // transform either as translation/rotation/scale or as one baked 16-float `matrix`, and
        // which one a file uses is the exporter's choice, not the artist's -- assimp writes
        // `matrix` for every node, so anything that has been through an FBX conversion arrives in
        // that form. Written as an EQUIVALENCE rather than against hand-computed expected values
        // because that is the property that actually matters, and because numbers copied out of a
        // working run prove only that the run did not change.
        //
        // WHAT IT CAUGHT. The matrix branch used to note the case and fall through, leaving
        // OcBone's defaults: an identity rest pose on every joint. That is invisible on a static
        // render -- the inverse bind is re-derived from the rest pose, so identity cancels identity
        // and the bind pose is pixel-correct -- and only surfaces once a clip is played against the
        // rig. Measured on a real 45-joint character, 45 of 45 bones came in at translation
        // (0,0,0), with the largest bone offset in the whole skeleton reading 0.000 cm.
        //
        // THE MESH IS NOT INCIDENTAL. Gltf::run refuses a file with no meshes outright, so a rig
        // on its own cannot be imported at all and a joints-only document would test nothing --
        // hence the triangle, which is otherwise irrelevant to what is being checked here.
        auto rig = [&](const std::string& childTransform) {
            return std::string("{")
              + "\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
              + "\"scenes\":[{\"nodes\":[0,1]}],"
              + "\"nodes\":["
                + "{\"mesh\":0},"
                + "{\"name\":\"root\",\"children\":[2]},"
                + "{\"name\":\"child\"," + childTransform + "}"
              + "],"
              + "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
              + "\"skins\":[{\"name\":\"rig\",\"joints\":[1,2]}],"
              + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(tri.bin)
                + "\",\"byteLength\":" + std::to_string(tri.bin.size()) + "}],"
              + "\"bufferViews\":["
                + "{\"buffer\":0,\"byteOffset\":" + std::to_string(tri.posOff) + ",\"byteLength\":" + std::to_string(tri.posLen) + "},"
                + "{\"buffer\":0,\"byteOffset\":" + std::to_string(tri.idxOff) + ",\"byteLength\":" + std::to_string(tri.idxLen) + "}"
              + "],"
              + "\"accessors\":["
                + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
              + "]}";
        };
        const std::string s45 = "0.70710678";

        // The matrix here is the row-major, row-vector form of exactly the TRS beside it: a quarter
        // turn about glTF +Y (which sends +X to -Z and +Z to +X, hence the two off-diagonal ones)
        // with the same one-metre offset up. glTF stores `matrix` column-major, which is the
        // transpose of the row-vector layout and therefore the same sixteen floats in the same
        // order -- the equality asserted below is what confirms that, rather than assuming it.
        const std::string trsJson = rig("\"translation\":[0,1,0],\"rotation\":[0," + s45 + ",0," + s45 + "]");
        const std::string matJson = rig("\"matrix\":[0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,1,0,1]");

        fmt::GltfImportResult trs, mat;
        std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(trsJson.data()), trsJson.size(),
                                        "", trs, {}, &why), "the TRS spelling imports: " + why);
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(matJson.data()), matJson.size(),
                                        "", mat, {}, &why), "and so does the baked-matrix spelling: " + why);

        check(trs.skeletons.size() == 1 && mat.skeletons.size() == 1, "both produce one skeleton");
        if (trs.skeletons.size() == 1 && mat.skeletons.size() == 1 &&
            trs.skeletons[0].bones.size() == 2 && mat.skeletons[0].bones.size() == 2) {
            const fmt::OcBone& a = trs.skeletons[0].bones[1];
            const fmt::OcBone& b = mat.skeletons[0].bones[1];
            check(a.name == "child" && b.name == "child", "and the compared bone is the child in both");

            // Not merely "non-zero" -- the point is that it is the RIGHT pose, and the TRS branch
            // is covered by the test above, so it serves as the oracle.
            checkNear(b.translation.x, a.translation.x, 1e-3f, "a baked matrix yields the same rest translation X");
            checkNear(b.translation.y, a.translation.y, 1e-3f, "...Y");
            checkNear(b.translation.z, a.translation.z, 1e-3f, "...Z");

            // A quaternion and its negation are the same rotation, so the comparison is on |dot|,
            // which is 1 exactly when the two describe the same turn whichever sign each carries.
            const f32 dot = std::fabs(a.rotation.x*b.rotation.x + a.rotation.y*b.rotation.y +
                                      a.rotation.z*b.rotation.z + a.rotation.w*b.rotation.w);
            checkNear(dot, 1.0f, 1e-3f, "and the same rest rotation, up to quaternion sign");

            checkNear(b.scale.x, a.scale.x, 1e-3f, "and the same scale X");
            checkNear(b.scale.y, a.scale.y, 1e-3f, "...Y");
            checkNear(b.scale.z, a.scale.z, 1e-3f, "...Z");

            // The regression guard proper: identity is exactly what the old code left behind, on
            // every joint of every rig an FBX converter had touched.
            const f32 len = std::sqrt(b.translation.x*b.translation.x +
                                      b.translation.y*b.translation.y +
                                      b.translation.z*b.translation.z);
            check(len > 1.0f, "the baked-matrix rest pose is not the identity the old code left behind");
        }

        // A pure scale of (2,3,4) about the glTF axes. Scale is dimensionless and only permutes
        // through the basis change, so engine (x,y,z) reads the glTF (z,x,y) -- see toEngineScale,
        // and note this is why scale is checked apart from translation, which also picks up the
        // metres-to-centimetres factor.
        const std::string scaledJson = rig("\"matrix\":[2,0,0,0, 0,3,0,0, 0,0,4,0, 0,0,0,1]");
        fmt::GltfImportResult sc; std::string scy;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(scaledJson.data()), scaledJson.size(),
                                        "", sc, {}, &scy), "a scaled baked matrix imports: " + scy);
        if (sc.skeletons.size() == 1 && sc.skeletons[0].bones.size() == 2) {
            const fmt::OcBone& b = sc.skeletons[0].bones[1];
            checkNear(b.scale.x, 4.0f, 1e-3f, "glTF Z scale becomes engine X scale");
            checkNear(b.scale.y, 2.0f, 1e-3f, "glTF X scale becomes engine Y scale");
            checkNear(b.scale.z, 3.0f, 1e-3f, "glTF Y scale becomes engine Z scale");
        }

        // SHEAR IS THE ONE THING A TRS TRIPLE CANNOT CARRY, so it must be reported rather than
        // quietly approximated. This tilts the second basis row into the first, which no
        // rotation-times-scale can reproduce.
        const std::string shearJson = rig("\"matrix\":[1,0,0,0, 0.5,1,0,0, 0,0,1,0, 0,0,0,1]");
        fmt::GltfImportResult sh; std::string shy;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(shearJson.data()), shearJson.size(),
                                        "", sh, {}, &shy), "a sheared baked matrix still imports: " + shy);
        bool reported = false;
        for (const std::string& u : sh.unsupported)
            if (u.find("shear") != std::string::npos) reported = true;
        check(reported, "and the shear it could not carry is reported, not silently dropped");
    }

    AVER_INFO("=== the transform ABOVE the topmost joint is part of the rest pose ===");
    {
        // A SKELETON MUST END UP IN THE SAME SPACE AS THE MESH IT SKINS. This importer bakes a
        // mesh's whole node chain into its vertices, so the skeleton has to be baked the same way
        // -- and it used to read joint LOCALS only, silently dropping every node above the topmost
        // joint. That is not a corner case: assimp writes the FBX-to-glTF axis and unit conversion
        // as exactly such a node, so the armature root routinely carries a rotation and a scale of
        // 100 that the mesh received and the skeleton did not.
        //
        // Checked as an INVARIANT rather than against expected numbers: whatever transform sits
        // above the rig, the mesh and the joints must move together under it. The armature node
        // below carries a scale of 2 and a quarter turn about glTF +Y, and the same node transform
        // is put on the mesh -- so a correct importer keeps the bone exactly where the vertex it
        // is bound to went.
        auto rig = [&](const std::string& armatureTransform) {
            return std::string("{")
              + "\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
              + "\"scenes\":[{\"nodes\":[0]}],"
              + "\"nodes\":["
                + "{\"name\":\"armature\"," + armatureTransform + ",\"children\":[1,2]},"
                + "{\"mesh\":0,\"skin\":0},"
                + "{\"name\":\"root\",\"translation\":[0,1,0]}"
              + "],"
              + "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
              + "\"skins\":[{\"name\":\"rig\",\"joints\":[2]}],"
              + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(tri.bin)
                + "\",\"byteLength\":" + std::to_string(tri.bin.size()) + "}],"
              + "\"bufferViews\":["
                + "{\"buffer\":0,\"byteOffset\":" + std::to_string(tri.posOff) + ",\"byteLength\":" + std::to_string(tri.posLen) + "},"
                + "{\"buffer\":0,\"byteOffset\":" + std::to_string(tri.idxOff) + ",\"byteLength\":" + std::to_string(tri.idxLen) + "}"
              + "],"
              + "\"accessors\":["
                + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
              + "]}";
        };

        const std::string plainJson = rig("\"scale\":[1,1,1]");
        const std::string xformJson = rig("\"scale\":[2,2,2],\"rotation\":[0,0.70710678,0,0.70710678]");

        fmt::GltfImportResult plain, xform;
        std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(plainJson.data()), plainJson.size(),
                                        "", plain, {}, &why), "the untransformed armature imports: " + why);
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(xformJson.data()), xformJson.size(),
                                        "", xform, {}, &why), "and the transformed one imports: " + why);

        if (plain.skeletons.size() == 1 && xform.skeletons.size() == 1 &&
            !plain.meshes.empty() && !xform.meshes.empty()) {
            const fmt::OcBone& pb = plain.skeletons[0].bones[0];
            const fmt::OcBone& xb = xform.skeletons[0].bones[0];

            // The armature scales by 2, so the joint one metre up must end up twice as far out.
            const f32 pl = std::sqrt(pb.translation.x*pb.translation.x + pb.translation.y*pb.translation.y +
                                     pb.translation.z*pb.translation.z);
            const f32 xl = std::sqrt(xb.translation.x*xb.translation.x + xb.translation.y*xb.translation.y +
                                     xb.translation.z*xb.translation.z);
            checkNear(xl, 2.0f * pl, 1e-2f, "a scale above the rig reaches the joint's rest offset");
            check(pl > 1.0f, "and the untransformed case really did have an offset to scale");

            // THE INVARIANT THAT MATTERS: bone and vertex move together. Comparing the ratio of the
            // two rather than either alone is what makes this a test of CONSISTENCY -- the property
            // skinning actually needs -- rather than of one hand-computed number.
            auto meshSpan = [](const fmt::OcMeshData& m) {
                f32 lo[3] = {1e9f,1e9f,1e9f}, hi[3] = {-1e9f,-1e9f,-1e9f};
                for (usize v = 0; v * 3 + 2 < m.positions.size(); ++v)
                    for (int a = 0; a < 3; ++a) {
                        const f32 c = m.positions[v*3 + usize(a)];
                        lo[a] = c < lo[a] ? c : lo[a];
                        hi[a] = c > hi[a] ? c : hi[a];
                    }
                f32 best = 0.0f;
                for (int a = 0; a < 3; ++a) best = (hi[a]-lo[a]) > best ? (hi[a]-lo[a]) : best;
                return best;
            };
            const f32 pm = meshSpan(plain.meshes[0]), xm = meshSpan(xform.meshes[0]);
            check(pm > 1.0f, "the mesh has a measurable span to compare");
            checkNear(xm / pm, xl / pl, 1e-2f,
                      "the mesh and the skeleton scaled by the SAME factor -- they stay in one space");
        }
    }

    AVER_INFO("=== two glTF skin OBJECTS naming the same joints collapse into ONE skeleton ===");
    {
        // Mirrors the shape a Kenney/Blender export produces when one armature is exported as a
        // separate skin per mesh (see GltfImport.cpp importSkins()'s own comment): two skin
        // objects, each independently listing the SAME joints in the SAME order. Both meshes reuse
        // the SAME position/joints/weights/index accessors -- only which skin their node names
        // (skin 0 vs skin 1) differs, exactly like body-mesh/skin-0 and head-mesh/skin-1 do.
        std::vector<u8> bin;
        const usize posOff = bin.size();
        putF(bin, 0); putF(bin, 0); putF(bin, 0);
        putF(bin, 1); putF(bin, 0); putF(bin, 0);
        putF(bin, 0); putF(bin, 0); putF(bin, -1);
        const usize posLen = bin.size() - posOff;

        const usize jOff = bin.size();
        // Raw skin-local index 0. In BOTH skins, joints[0] names node 1 ("child"), so a correct
        // per-skin remap sends every vertex to sorted index 1 regardless of which skin's own
        // newIndexOf does the remapping.
        for (int v = 0; v < 3; ++v) { putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); }
        const usize jLen = bin.size() - jOff;

        const usize wOff = bin.size();
        for (int v = 0; v < 3; ++v) { putF(bin, 1); putF(bin, 0); putF(bin, 0); putF(bin, 0); }
        const usize wLen = bin.size() - wOff;

        const usize iOff = bin.size();
        putU16(bin, 0); putU16(bin, 1); putU16(bin, 2);
        const usize iLen = bin.size() - iOff;
        while (bin.size() % 4) bin.push_back(0);

        auto view = [](usize off, usize len) {
            return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                   ",\"byteLength\":" + std::to_string(len) + "}";
        };
        const std::string json = std::string("{")
          + "\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
          + "\"scenes\":[{\"nodes\":[0,2,3]}],"
          + "\"nodes\":["
            + "{\"name\":\"root\",\"children\":[1]},"
            + "{\"name\":\"child\",\"translation\":[0,1,0]},"
            + "{\"name\":\"bodyNode\",\"mesh\":0,\"skin\":0},"
            + "{\"name\":\"headNode\",\"mesh\":1,\"skin\":1}"
          + "],"
          + "\"skins\":[{\"joints\":[1,0]},{\"joints\":[1,0]}],"
          + "\"meshes\":["
            + "{\"name\":\"Mesh0\",\"primitives\":[{\"attributes\":"
              + "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]},"
            + "{\"name\":\"Mesh1\",\"primitives\":[{\"attributes\":"
              + "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]}"
          + "],"
          + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin)
            + "\",\"byteLength\":" + std::to_string(bin.size()) + "}],"
          + "\"bufferViews\":[" + view(posOff,posLen) + "," + view(jOff,jLen) + "," + view(wOff,wLen)
            + "," + view(iOff,iLen) + "],"
          + "\"accessors\":["
            + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
          + "]}";

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "two identical-shape skins import: " + why);

        check(res.meshes.size() == 2, "two meshes");
        check(res.skeletons.size() == 1,
              "the two skin OBJECTS collapsed into ONE skeleton, not two -- they name the same joints");
        check(res.meshSkinIndex.size() == 2, "meshSkinIndex is parallel to meshes");
        if (res.meshSkinIndex.size() == 2) {
            check(res.meshSkinIndex[0] >= 0 && res.meshSkinIndex[0] == res.meshSkinIndex[1],
                  "both meshes resolve to the SAME skeletons[] entry, even though they named "
                  "DIFFERENT (0 vs 1) raw glTF skin objects");
        }

        // THE LOOP-SCOPING REGRESSION CASE: mesh1 is bound to skin INDEX 1 (s != 0), so the old
        // `if (s == 0)` gate never remapped its joints at all, leaving raw index 0 (= skin-local
        // "child") unremapped. A correct per-skin remap sends it to sorted index 1 instead, exactly
        // like mesh0's own (skin-0) case.
        for (usize mi = 0; mi < res.meshes.size(); ++mi) {
            const fmt::OcMeshData& m = res.meshes[mi];
            check(m.hasSkin(), "mesh " + std::to_string(mi) + " comes back skinned");
            bool remapped = true;
            for (u32 v = 0; v < m.vertexCount(); ++v)
                if (m.joints[usize(v) * fmt::kOcMeshInfluences] != 1) remapped = false;
            check(remapped, "mesh " + std::to_string(mi) +
                  "'s joint index was remapped through ITS OWN skin's reorder (0 -> 1), not left raw");
        }
    }

    AVER_INFO("=== two glTF skins naming DIFFERENT joints do NOT collapse ===");
    {
        // The negative control for the test above: skin 1 is genuinely a different (smaller)
        // skeleton, so it must stay a separate skeletons[] entry, and the two meshes must disagree
        // on meshSkinIndex -- or ConvertTool's same-skeleton merge check would wrongly treat them
        // as mergeable and silently bind head-mesh-shaped vertices to body-mesh-shaped bones.
        std::vector<u8> bin;
        const usize posOff = bin.size();
        putF(bin, 0); putF(bin, 0); putF(bin, 0);
        putF(bin, 1); putF(bin, 0); putF(bin, 0);
        putF(bin, 0); putF(bin, 0); putF(bin, -1);
        const usize posLen = bin.size() - posOff;

        const usize jOff = bin.size();
        for (int v = 0; v < 3; ++v) { putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); putU16(bin, 0); }
        const usize jLen = bin.size() - jOff;

        const usize wOff = bin.size();
        for (int v = 0; v < 3; ++v) { putF(bin, 1); putF(bin, 0); putF(bin, 0); putF(bin, 0); }
        const usize wLen = bin.size() - wOff;

        const usize iOff = bin.size();
        putU16(bin, 0); putU16(bin, 1); putU16(bin, 2);
        const usize iLen = bin.size() - iOff;
        while (bin.size() % 4) bin.push_back(0);

        auto view = [](usize off, usize len) {
            return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                   ",\"byteLength\":" + std::to_string(len) + "}";
        };
        const std::string json = std::string("{")
          + "\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
          + "\"scenes\":[{\"nodes\":[0,2,3]}],"
          + "\"nodes\":["
            + "{\"name\":\"root\",\"children\":[1]},"
            + "{\"name\":\"child\",\"translation\":[0,1,0]},"
            + "{\"name\":\"bodyNode\",\"mesh\":0,\"skin\":0},"
            + "{\"name\":\"headNode\",\"mesh\":1,\"skin\":1}"
          + "],"
          // skin 1 has only ONE joint (just "root") -- a genuinely different, smaller skeleton
          // than skin 0's two bones, not a duplicate of it.
          + "\"skins\":[{\"joints\":[1,0]},{\"joints\":[0]}],"
          + "\"meshes\":["
            + "{\"name\":\"Mesh0\",\"primitives\":[{\"attributes\":"
              + "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]},"
            + "{\"name\":\"Mesh1\",\"primitives\":[{\"attributes\":"
              + "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]}"
          + "],"
          + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin)
            + "\",\"byteLength\":" + std::to_string(bin.size()) + "}],"
          + "\"bufferViews\":[" + view(posOff,posLen) + "," + view(jOff,jLen) + "," + view(wOff,wLen)
            + "," + view(iOff,iLen) + "],"
          + "\"accessors\":["
            + "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            + "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            + "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
          + "]}";

        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "two differently-shaped skins import: " + why);

        check(res.skeletons.size() == 2, "genuinely different skins stay TWO skeletons, not collapsed");
        check(res.meshSkinIndex.size() == 2 && res.meshSkinIndex[0] != res.meshSkinIndex[1],
              "the two meshes disagree on meshSkinIndex -- ConvertTool must refuse to merge their joints");
        if (res.skeletons.size() == 2) {
            check(res.skeletons[0].bones.size() == 2 && res.skeletons[1].bones.size() == 1,
                  "each mesh kept its own skeleton's own, different, bone count");
        }
    }

    // ---- MATERIALS AND IMAGES ----------------------------------------------------------------
    // Everything below reads what a glTF says about a SURFACE. Before this existed the importer
    // answered the whole subject with two notes ("material definitions", "textures") and dropped it.
    {
        const Tri tri = makeTriangleBuffer();

        // THE DEFAULTS, WHICH ARE THE EASIEST THING TO GET WRONG. A material that states none of
        // these is, by the glTF spec, a fully rough METAL: metallicFactor and roughnessFactor are
        // both 1.0. JsonValue::asFloat's own fallback is 0.0f, so taking it yields a plausible
        // dielectric that is wrong on every asset relying on the spec -- and wrong in a way that
        // reads as a lighting bug rather than an import bug.
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Bare\"}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a material-only glTF imports: " + why);
        check(res.materials.size() == 1, "one material came across");
        if (res.materials.size() == 1) {
            const fmt::GltfMaterial& m = res.materials[0];
            check(m.name == "Bare", "the material's name survives");
            checkNear(m.metallicFactor,  1.0f, 1e-6f, "an ABSENT metallicFactor is 1.0, the spec default, NOT zero");
            checkNear(m.roughnessFactor, 1.0f, 1e-6f, "an ABSENT roughnessFactor is 1.0, not zero");
            checkNear(m.baseColorFactor[0], 1.0f, 1e-6f, "an absent baseColorFactor is white, not black");
            checkNear(m.baseColorFactor[3], 1.0f, 1e-6f, "and opaque, not transparent");
            checkNear(m.emissiveFactor[0], 0.0f, 1e-6f, "an absent emissiveFactor is black");
            checkNear(m.normalScale, 1.0f, 1e-6f, "an absent normalScale is 1.0");
            checkNear(m.occlusionStrength, 1.0f, 1e-6f, "an absent occlusionStrength is 1.0");
            checkNear(m.alphaCutoff, 0.5f, 1e-6f, "an absent alphaCutoff is 0.5");
            check(m.alphaMode == "OPAQUE", "an absent alphaMode is OPAQUE");
            check(!m.doubleSided, "an absent doubleSided is false");
        }
    }
    {
        // THE SAME DEFAULTS, BUT WITH pbrMetallicRoughness PRESENT AND THE FACTORS MISSING FROM IT.
        // This is the shape that actually occurs -- both Kenney materials in this tree state
        // metallicFactor and omit roughnessFactor -- and it is a DIFFERENT code path from the block
        // above, which has no pbrMetallicRoughness at all and so never reads a factor. Written after
        // a falsification proved the earlier case could not catch a wrong fallback: breaking the
        // default left that test green because the line never ran.
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"HalfStated\",\"pbrMetallicRoughness\":{\"metallicFactor\":0.0}}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a partly stated pbrMetallicRoughness imports: " + why);
        if (res.materials.size() == 1) {
            checkNear(res.materials[0].metallicFactor, 0.0f, 1e-6f,
                      "a STATED metallicFactor of 0 is kept -- not confused with absence");
            checkNear(res.materials[0].roughnessFactor, 1.0f, 1e-6f,
                      "an absent roughnessFactor BESIDE a stated sibling is still 1.0, the spec default");
            checkNear(res.materials[0].baseColorFactor[0], 1.0f, 1e-6f,
                      "and an absent baseColorFactor inside a present pbr block is still white");
        }
    }
    {
        // STATED VALUES SURVIVE, including the ones that are also the defaults' neighbours.
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Stated\",\"pbrMetallicRoughness\":{"
              "\"baseColorFactor\":[0.8,0.6,0.4,0.5],\"metallicFactor\":0.25,\"roughnessFactor\":0.7},"
              "\"emissiveFactor\":[0.1,0,0.2],\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25,"
              "\"doubleSided\":true}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a fully stated material imports: " + why);
        if (res.materials.size() == 1) {
            const fmt::GltfMaterial& m = res.materials[0];
            checkNear(m.baseColorFactor[0], 0.8f, 1e-6f, "baseColorFactor r");
            checkNear(m.baseColorFactor[3], 0.5f, 1e-6f, "baseColorFactor a -- the alpha channel is not dropped");
            checkNear(m.metallicFactor,  0.25f, 1e-6f, "metallicFactor survives");
            checkNear(m.roughnessFactor, 0.70f, 1e-6f, "roughnessFactor survives");
            checkNear(m.emissiveFactor[2], 0.2f, 1e-6f, "emissiveFactor b");
            checkNear(m.alphaCutoff, 0.25f, 1e-6f, "a stated alphaCutoff survives");
            check(m.alphaMode == "MASK", "alphaMode survives verbatim");
            check(m.doubleSided, "doubleSided survives");
        }
    }
    {
        // KHR_materials_emissive_strength LIFTS emissiveFactor PAST glTF core's [0,1] cap. Bright
        // emitters -- a lamp bulb, a neon tube -- cannot be authored with emissiveFactor alone, since
        // the core spec caps it at 1; authoring tools bolt this multiplier on instead of a bigger
        // factor, and GltfImport.cpp must fold it into emissiveFactor rather than drop it as "a
        // material extension this importer does not carry".
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Bright\",\"emissiveFactor\":[1.0,0.5,0.25],"
              "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":8}}}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a material with KHR_materials_emissive_strength imports: " + why);
        if (res.materials.size() == 1) {
            const fmt::GltfMaterial& m = res.materials[0];
            checkNear(m.emissiveFactor[0], 8.0f, 1e-5f, "emissiveFactor r scaled by emissiveStrength (1.0 * 8)");
            checkNear(m.emissiveFactor[1], 4.0f, 1e-5f, "emissiveFactor g scaled by emissiveStrength (0.5 * 8)");
            checkNear(m.emissiveFactor[2], 2.0f, 1e-5f, "emissiveFactor b scaled by emissiveStrength (0.25 * 8)");
        }
        check(!noted(res, "KHR_materials_emissive_strength"),
              "the extension that was READ AND APPLIED is not also reported as one this importer dropped");
    }
    {
        // WITHOUT the extension, a stated emissiveFactor carries verbatim -- there is no strength to
        // fold in, and the no-extension path must not scale by anything other than the implicit 1.0.
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"NoStrength\",\"emissiveFactor\":[1.0,0.5,0.25]}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a material without the extension imports: " + why);
        if (res.materials.size() == 1) {
            const fmt::GltfMaterial& m = res.materials[0];
            checkNear(m.emissiveFactor[0], 1.00f, 1e-6f, "emissiveFactor r unscaled without the extension");
            checkNear(m.emissiveFactor[1], 0.50f, 1e-6f, "emissiveFactor g unscaled without the extension");
            checkNear(m.emissiveFactor[2], 0.25f, 1e-6f, "emissiveFactor b unscaled without the extension");
        }
    }
    {
        // AN IMAGE THROUGH A data: URI, and its bytes must arrive unchanged. Nothing decodes or
        // re-encodes an image: a round trip through a compressor would change bytes the source is
        // entitled to get back, and would cost quality on a JPEG for nothing.
        const std::vector<u8> want = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Tex\",\"pbrMetallicRoughness\":{"
              "\"baseColorTexture\":{\"index\":0}}}],"
            "\"textures\":[{\"source\":0}],"
            "\"images\":[{\"name\":\"pat\",\"mimeType\":\"image/png\",\"uri\":\"data:image/png;base64," + b64(want) + "\"}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a data: URI image imports: " + why);
        check(res.images.size() == 1 && res.images[0].ok, "the image was read");
        if (res.images.size() == 1) {
            check(res.images[0].bytes == want, "its bytes are BYTE-IDENTICAL to the source -- nothing re-encoded");
            check(res.images[0].ext == ".png", "the mimeType decided the extension");
        }
        check(res.materials.size() == 1 && res.materials[0].baseColorTex.imageIndex == 0,
              "the material -> textureInfo -> textures[] -> images[] chain resolved to image 0");
    }
    {
        // THE CASE BOTH REAL ASSETS IN THIS TREE HIT: a file naming a texture it does not ship.
        // The slot must be left UNBOUND and the path must be NAMED -- a material carrying a texture
        // reference to a file that is not there looks like a renderer fault, not an import one.
        const Tri tri = makeTriangleBuffer();
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Dangling\",\"pbrMetallicRoughness\":{"
              "\"baseColorTexture\":{\"index\":0}}}],"
            "\"textures\":[{\"source\":0}],"
            "\"images\":[{\"uri\":\"Textures/nosuch.png\"}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a dangling image reference does not fail the whole import: " + why);
        check(res.images.size() == 1 && !res.images[0].ok, "the image is marked unread");
        check(res.materials.size() == 1 && res.materials[0].baseColorTex.imageIndex < 0,
              "the texture slot is left UNBOUND rather than pointed at a file that is not there");
        check(noted(res, "nosuch.png"), "and the missing path is named in the notes, not merely counted");
        if (res.materials.size() == 1)
            checkNear(res.materials[0].metallicFactor, 1.0f, 1e-6f,
                      "the rest of the material still imported -- one bad image is not a lost material");
    }
    {
        // A SECOND UV SET HAS NOWHERE TO GO (OcMeshData carries one), and an unknown extension is a
        // missing feature, not a detail. Both are said by name rather than dropped.
        const Tri tri = makeTriangleBuffer();
        const std::vector<u8> px = {0x11, 0x22};
        const std::string json = materialJson(tri.posOff, tri.posLen, tri.idxOff, tri.idxLen, b64(tri.bin),
            "\"materials\":[{\"name\":\"Odd\",\"pbrMetallicRoughness\":{"
              "\"baseColorTexture\":{\"index\":0,\"texCoord\":1,"
                "\"extensions\":{\"KHR_texture_transform\":{}}}},"
              "\"extensions\":{\"KHR_materials_transmission\":{}}}],"
            "\"textures\":[{\"source\":0}],"
            "\"images\":[{\"name\":\"p\",\"mimeType\":\"image/png\",\"uri\":\"data:image/png;base64," + b64(px) + "\"}]");
        fmt::GltfImportResult res; std::string why;
        check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
              "a material with an unknown extension still imports: " + why);
        check(noted(res, "KHR_materials_transmission"), "the MATERIAL's extension is named");
        check(noted(res, "KHR_texture_transform"),
              "and so is the one on the textureInfo -- where both real Kenney assets actually put it");
        check(noted(res, "texCoord 1"), "a second UV set is reported rather than silently sampled as uv0");
        check(res.materials.size() == 1 && res.materials[0].baseColorTex.texCoord == 0,
              "and the slot falls back to uv0, the only set that exists");
        check(res.materials.size() == 1 && res.materials[0].baseColorTex.imageIndex == 0,
              "the texture still binds -- an unsupported extension does not cost the texture");
    }

    if (g_failures == 0) AVER_INFO("=== all glTF import tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
