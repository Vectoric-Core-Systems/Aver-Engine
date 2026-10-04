// MakeRig — writes content/dev/Rig.gltf, the tree's only skinned asset source, and then checks it.
//
// THE TREE HAS NEVER CONTAINED A SKINNED MESH. No .gltf, no .glb, no .ocmesh with skin streams, so
// every piece of skinning built so far has been exercised by geometry assembled in memory by the
// thing testing it. That is fine for arithmetic and useless for the importer, the container, and
// anything a human opens.
//
// IT EMITS glTF RATHER THAN THE .oc* TRIPLE, and that is the point rather than a shortcut:
//   - a generator that wrote the engine's own container would be testing only itself, and would
//     become a second source of truth for the skin stream's interleave, for the R8G8B8A8_UNORM
//     weight quantisation, and for the chunk hashing -- all of which already live in Aver.Formats
//     and would drift the first time it changed;
//   - going through the importer exercises the metres-to-centimetres scale, the determinant -1
//     basis change, the winding reversal and the child-first joint reorder, which is what a real
//     asset actually hits;
//   - glTF is TEXT, so the source can be committed and diffed rather than landing as a blob.
//
// The cooked triple is produced from it by ConvertTool and is deliberately NOT committed.
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/anim/AnimSampler.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace aver;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) AVER_INFO("  ok    {}", what);
    else  { AVER_ERROR("  FAIL  {}", what); ++g_failures; }
}

// ---------------------------------------------------------------- little-endian byte pushing

void put(std::vector<u8>& b, const void* p, usize n) {
    const u8* s = static_cast<const u8*>(p);
    b.insert(b.end(), s, s + n);
}
void putF(std::vector<u8>& b, f32 v) { put(b, &v, 4); }
void putU16(std::vector<u8>& b, u16 v) { put(b, &v, 2); }

// Base64, so the buffer rides inside the .gltf as a data URI and the asset is ONE file.
std::string b64(const std::vector<u8>& in) {
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

std::string f(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

// ---------------------------------------------------------------- the rig

// glTF units are METRES and glTF up is +Y. The importer scales by 100 and rotates into the engine's
// +Z up, so a one-metre ribbon here is a 100 cm one in the editor.
constexpr u32 kSegments = 12;                    // rings - 1
constexpr u32 kRings    = kSegments + 1;
constexpr f32 kLength   = 1.0f;                  // metres, along glTF +Y
constexpr f32 kRadius   = 0.08f;                 // metres, half the square cross-section

// Where the second bone sits, and the span the weight ramp crosses. The ramp is what makes this a
// SKINNING asset rather than a rigid one parented to a bone: mid-ribbon vertices are genuinely
// blended between two matrices, which is the only thing that can catch a weight that was dropped,
// renormalised, or read at the wrong stride.
constexpr f32 kBendAt    = 0.5f;
constexpr f32 kRampStart = 0.25f;
constexpr f32 kRampEnd   = 0.75f;

f32 bendWeight(f32 y) {
    const f32 t = (y - kRampStart) / (kRampEnd - kRampStart);
    return t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
}

// The four corners of a ring, and their outward normals. A closed square tube rather than a flat
// ribbon so it is visible from every angle and cannot be lost to back-face culling.
const f32 kCorner[4][2] = {{ 1, 1}, {-1, 1}, {-1,-1}, { 1,-1}};   // (x, z) in glTF

std::string buildGltf() {
    std::vector<u8> bin;

    // --- positions ---
    const usize posOff = bin.size();
    for (u32 r = 0; r < kRings; ++r) {
        const f32 y = kLength * static_cast<f32>(r) / static_cast<f32>(kSegments);
        for (u32 c = 0; c < 4; ++c) {
            putF(bin, kCorner[c][0] * kRadius);
            putF(bin, y);
            putF(bin, kCorner[c][1] * kRadius);
        }
    }
    const usize posLen = bin.size() - posOff;

    // --- normals, radially outward so the tube shades as a tube ---
    const usize nrmOff = bin.size();
    const f32 inv = 0.70710678f;
    for (u32 r = 0; r < kRings; ++r)
        for (u32 c = 0; c < 4; ++c) {
            putF(bin, kCorner[c][0] * inv);
            putF(bin, 0.0f);
            putF(bin, kCorner[c][1] * inv);
        }
    const usize nrmLen = bin.size() - nrmOff;

    // --- joints. The skin lists its joints CHILD FIRST (see the JSON), so glTF joint 0 is `bend`
    //     and joint 1 is `root`. The importer must reorder both the skeleton AND these indices, or
    //     every vertex silently names the wrong bone. ---
    const usize jOff = bin.size();
    for (u32 r = 0; r < kRings; ++r)
        for (u32 c = 0; c < 4; ++c) { putU16(bin, 0); putU16(bin, 1); putU16(bin, 0); putU16(bin, 0); }
    const usize jLen = bin.size() - jOff;

    // --- weights, summing to exactly one before quantisation ---
    const usize wOff = bin.size();
    for (u32 r = 0; r < kRings; ++r) {
        const f32 y = kLength * static_cast<f32>(r) / static_cast<f32>(kSegments);
        const f32 wb = bendWeight(y);
        for (u32 c = 0; c < 4; ++c) { putF(bin, wb); putF(bin, 1.0f - wb); putF(bin, 0.0f); putF(bin, 0.0f); }
    }
    const usize wLen = bin.size() - wOff;

    // --- indices: four side quads per segment, plus a cap at each end ---
    const usize iOff = bin.size();
    u32 triCount = 0;
    for (u32 s = 0; s < kSegments; ++s) {
        const u16 a = static_cast<u16>(s * 4);
        const u16 b = static_cast<u16>((s + 1) * 4);
        for (u16 c = 0; c < 4; ++c) {
            const u16 c2 = static_cast<u16>((c + 1) % 4);
            putU16(bin, static_cast<u16>(a + c));  putU16(bin, static_cast<u16>(b + c));  putU16(bin, static_cast<u16>(b + c2));
            putU16(bin, static_cast<u16>(a + c));  putU16(bin, static_cast<u16>(b + c2)); putU16(bin, static_cast<u16>(a + c2));
            triCount += 2;
        }
    }
    const u16 last = static_cast<u16>(kSegments * 4);
    putU16(bin, 0); putU16(bin, 2); putU16(bin, 1);
    putU16(bin, 0); putU16(bin, 3); putU16(bin, 2);
    putU16(bin, last); putU16(bin, static_cast<u16>(last + 1)); putU16(bin, static_cast<u16>(last + 2));
    putU16(bin, last); putU16(bin, static_cast<u16>(last + 2)); putU16(bin, static_cast<u16>(last + 3));
    triCount += 4;
    const usize iLen = bin.size() - iOff;
    while (bin.size() % 4) bin.push_back(0);

    // --- clip keys. THREE keys, not two, so the clip RETURNS TO REST: a verification that asserts
    //     a pose is reversible needs a time at which the answer is the rest pose again. ---
    const usize tOff = bin.size();
    putF(bin, 0.0f); putF(bin, 0.5f); putF(bin, 1.0f);
    const usize tLen = bin.size() - tOff;

    const usize rOff = bin.size();
    const f32 s45 = 0.70710678f;
    putF(bin, 0); putF(bin, 0); putF(bin, 0); putF(bin, 1);            // identity
    putF(bin, s45); putF(bin, 0); putF(bin, 0); putF(bin, s45);        // 90 degrees about glTF +X
    putF(bin, 0); putF(bin, 0); putF(bin, 0); putF(bin, 1);            // and back
    const usize rLen = bin.size() - rOff;

    auto view = [](usize off, usize len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
               ",\"byteLength\":" + std::to_string(len) + "}";
    };
    const u32 verts = kRings * 4;

    // inverseBindMatrices is deliberately absent: the importer RE-DERIVES it from the rest pose,
    // because converting a matrix through a determinant -1 basis change is not a matter of
    // permuting its entries. Supplying one would be noted as unsupported and then ignored.
    std::string json = std::string("{")
      + "\"asset\":{\"version\":\"2.0\",\"generator\":\"Aver MakeRig\"},\"scene\":0,"
      + "\"scenes\":[{\"nodes\":[0,2]}],"
      + "\"nodes\":["
        + "{\"name\":\"root\",\"children\":[1]},"
        + "{\"name\":\"bend\",\"translation\":[0," + f(kBendAt) + ",0]},"
        + "{\"name\":\"Ribbon\",\"mesh\":0,\"skin\":0}"
      + "],"
      + "\"skins\":[{\"name\":\"Rig\",\"joints\":[1,0]}],"
      + "\"meshes\":[{\"name\":\"Ribbon\",\"primitives\":[{\"attributes\":"
        + "{\"POSITION\":0,\"NORMAL\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4}]}],"
      + "\"animations\":[{\"name\":\"Bend\",\"channels\":[{\"sampler\":0,"
        + "\"target\":{\"node\":1,\"path\":\"rotation\"}}],"
        + "\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"}]}],"
      + "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin)
        + "\",\"byteLength\":" + std::to_string(bin.size()) + "}],"
      + "\"bufferViews\":[" + view(posOff, posLen) + "," + view(nrmOff, nrmLen) + ","
        + view(jOff, jLen) + "," + view(wOff, wLen) + "," + view(iOff, iLen) + ","
        + view(tOff, tLen) + "," + view(rOff, rLen) + "],"
      + "\"accessors\":["
        + "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(verts) + ",\"type\":\"VEC3\"},"
        + "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(verts) + ",\"type\":\"VEC3\"},"
        + "{\"bufferView\":2,\"componentType\":5123,\"count\":" + std::to_string(verts) + ",\"type\":\"VEC4\"},"
        + "{\"bufferView\":3,\"componentType\":5126,\"count\":" + std::to_string(verts) + ",\"type\":\"VEC4\"},"
        + "{\"bufferView\":4,\"componentType\":5123,\"count\":" + std::to_string(triCount * 3) + ",\"type\":\"SCALAR\"},"
        + "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
        + "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
      + "]}";
    return json;
}

// ---------------------------------------------------------------- the self-check

// Imports what was just written and asserts every property the rest of the engine relies on. Written
// here rather than left to a test because a generator whose output nobody validates is how an asset
// comes to be subtly wrong for months.
void verify(const std::string& path) {
    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(path, res, {}, &why)) {
        AVER_ERROR("  FAIL  the file just written does not import: {}", why);
        ++g_failures;
        return;
    }
    for (const std::string& u : res.unsupported) AVER_INFO("  note  unsupported: {}", u);

    check(res.meshes.size() == 1, "one mesh");
    check(res.skeletons.size() == 1, "one skeleton");
    check(res.animations.size() == 1, "one clip");
    if (res.meshes.empty() || res.skeletons.empty() || res.animations.empty()) return;

    const fmt::OcMeshData& m = res.meshes[0];
    const fmt::OcSkeleton& sk = res.skeletons[0];
    const fmt::OcAnimation& an = res.animations[0];

    check(m.valid(), "the mesh is valid");
    check(m.hasSkin(), "and it CARRIES SKIN -- which no other asset in this tree does");
    check(sk.valid(), "the skeleton is valid: parents before children, no cycles");
    check(sk.bones.size() == 2, "two bones");
    check(sk.bones.size() == 2 && sk.bones[0].name == "root" && sk.bones[1].name == "bend",
          "the CHILD-FIRST joint list was reordered, not taken as authored");

    // The metre-to-centimetre scale and the basis change, in one assertion: a one-metre ribbon
    // along glTF +Y must be a 100 cm ribbon along engine +Z.
    const f32 height = m.boundsMax.z - m.boundsMin.z;
    check(std::fabs(height - 100.0f) < 1.0f,
          "a 1 m ribbon along glTF +Y became a " + f(height) + " cm one along engine +Z");

    // A vertex mid-ramp must name BOTH bones with a partial weight. This is what separates a
    // skinned asset from a rigid one parented to a bone, and it is the only property that can catch
    // a weight stream quietly collapsed to one influence.
    u32 blended = 0;
    for (u32 v = 0; v < m.vertexCount(); ++v) {
        const f32 w0 = m.weights[v * 4 + 0], w1 = m.weights[v * 4 + 1];
        if (w0 > 0.01f && w1 > 0.01f) ++blended;
    }
    check(blended > 0, std::to_string(blended) + " vertices are genuinely blended between two bones");

    // Weights must still sum to one AFTER the container round trip, because .ocmesh stores them as
    // R8G8B8A8_UNORM and a naive round would leave a mesh that quietly shrinks at the joints.
    const std::string tmp = path + ".roundtrip.ocmesh";
    fmt::OcMeshData back;
    if (fmt::saveOcMesh(tmp, m, &why) && fmt::loadOcMesh(tmp, back, &why)) {
        f32 worst = 0.0f;
        for (u32 v = 0; v < back.vertexCount(); ++v) {
            f32 s = 0.0f;
            for (u32 i = 0; i < 4; ++i) s += back.weights[v * 4 + i];
            worst = std::fmax(worst, std::fabs(s - 1.0f));
        }
        check(worst <= 1.0f / 255.0f,
              "weights still sum to one after the 8-bit round trip (worst error " + f(worst) + ")");
        check(back.hasSkin() && back.vertexCount() == m.vertexCount(),
              "and the skin survives the container");
        std::remove(tmp.c_str());
    } else {
        AVER_ERROR("  FAIL  the round trip through .ocmesh failed: {}", why);
        ++g_failures;
    }

    // The clip must actually MOVE the rig, and must come back. A verification that asserts a pose is
    // reversible is worthless if the clip never left rest in the first place.
    check(std::fabs(an.duration - 1.0f) < 1e-4f, "the clip is one second long");
    // Every sample SEEDS FROM REST, which is the sampler's contract: a bone no track animates keeps
    // its rest transform instead of collapsing to identity.
    anim::Pose rest, mid, end;
    anim::restPose(sk, rest);
    mid = rest; anim::sampleAnimation(an, 0.5f, mid);
    end = rest; anim::sampleAnimation(an, 1.0f, end);
    std::vector<Mat4> restSkin, midSkin;
    anim::poseToSkinning(sk, rest, restSkin);
    anim::poseToSkinning(sk, mid, midSkin);

    f32 moved = 0.0f;
    for (usize b = 0; b < restSkin.size(); ++b)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                moved = std::fmax(moved, std::fabs(restSkin[b].m[r][c] - midSkin[b].m[r][c]));
    check(moved > 0.1f, "at t=0.5 the rig is visibly bent away from rest (max element delta " + f(moved) + ")");

    f32 returned = 0.0f;
    for (usize b = 0; b < rest.local.size() && b < end.local.size(); ++b) {
        returned = std::fmax(returned, std::fabs(rest.local[b].rotation.x - end.local[b].rotation.x));
        returned = std::fmax(returned, std::fabs(rest.local[b].rotation.w - end.local[b].rotation.w));
    }
    check(returned < 1e-3f, "and at t=1.0 it is back at rest, so the pose is a function of time");
}

} // namespace

// Writes <dir>/Rig.gltf and verifies it. Returns 0 on success, 1 on a failed check, 2 on bad usage.
int main(int argc, char** argv) {
    if (argc < 2) { AVER_ERROR("usage: MakeRig <output-directory>"); return exitCode(ExitCode::Usage); }

    std::string dir = argv[1];
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    const std::string path = dir + "/Rig.gltf";

    const std::string json = buildGltf();
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), fp);
        std::fclose(fp);
    } else {
        AVER_ERROR("could not write {}", path);
        return exitCode(ExitCode::Failed);
    }
    AVER_INFO("wrote {} ({} bytes, {} vertices, 2 bones, 1 clip)",
              path, json.size(), kRings * 4);

    AVER_INFO("verifying what was written");
    verify(path);

    AVER_INFO(g_failures ? "MakeRig: {} FAILURES" : "MakeRig: all checks passed ({})", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
