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

    if (g_failures == 0) AVER_INFO("=== all glTF import tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
