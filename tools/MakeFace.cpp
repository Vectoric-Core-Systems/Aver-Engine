// MakeFace -- writes content/dev/Face.gltf, FaceRigged.gltf and Face.ocface, the tree's blend-shape test assets, and then
// checks them. The sibling of MakeRig (read its header for why the output is glTF and not the .oc* triple: going through the
// importer exercises the scale, the basis change and the sparse-accessor and weights-animation paths a real asset hits, and
// glTF is text, so the source can be committed and diffed).
//
//   Face.gltf        a 24 x 24 quad patch ("head", 625 vertices, 20 x 25 cm) with 16 named blend shapes, 15 of them dense
//                    accessors (jawOpen also carries NORMAL deltas) and one (noseSneerLeft) a SPARSE accessor; default weights
//                    on two of them; two animations of weights, one LINEAR ("Talk") and one CUBICSPLINE ("Emote").
//   FaceRigged.gltf  the same mesh skinned to a two-bone jaw rig (head, jaw), so skinned + morph is exercised; "Talk" there
//                    also bends the jaw, so the clip carries bone tracks AND morph.* curves.
//   Face.ocface      a face rig for it: smile / surprise / blink poses and a "talkOpen" pose that moves the jaw bone too.
//
// Everything is computed in double and rounded to f32 once, so a regeneration reproduces the committed bytes except possibly a
// last-bit difference in libm's exp.
#include "aver/anim/FaceRig.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

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

// ---------------------------------------------------------------- byte pushing

void put(std::vector<u8>& b, const void* p, usize n) {
    const u8* s = static_cast<const u8*>(p);
    b.insert(b.end(), s, s + n);
}
void putF(std::vector<u8>& b, double v) { const f32 f = static_cast<f32>(v); put(b, &f, 4); }
void putU16(std::vector<u8>& b, u16 v) { put(b, &v, 2); }
void align4(std::vector<u8>& b) { while (b.size() % 4) b.push_back(0); }

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

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

// ---------------------------------------------------------------- the patch

constexpr u32 kN = 24;                 // quads per side
constexpr u32 kSide = kN + 1;          // vertices per side
constexpr u32 kVerts = kSide * kSide;  // 625
constexpr double kWidth = 0.20, kHeight = 0.25;   // metres (glTF)

struct Shape {
    const char* name;
    double cx, cy, sigma;      // a Gaussian bump on the patch, metres
    double dx, dy, dz;         // the displacement at its peak, metres
    bool normals;              // also emit NORMAL deltas
    bool sparse;               // emit as a sparse accessor
    double defaultWeight;
};

const Shape kShapes[16] = {
    {"jawOpen",          0.000, 0.040, 0.040,  0.000, -0.030, 0.000, true,  false, 0.0},
    {"mouthSmileLeft",   0.040, 0.070, 0.030,  0.010,  0.012, 0.004, false, false, 0.0},
    {"mouthSmileRight", -0.040, 0.070, 0.030, -0.010,  0.012, 0.004, false, false, 0.0},
    {"eyeBlinkLeft",     0.040, 0.170, 0.025,  0.000, -0.015, 0.000, false, false, 0.0},
    {"eyeBlinkRight",   -0.040, 0.170, 0.025,  0.000, -0.015, 0.000, false, false, 0.0},
    {"browInnerUp",      0.000, 0.205, 0.040,  0.000,  0.012, 0.002, false, false, 0.15},
    {"cheekPuff",        0.000, 0.100, 0.070,  0.000,  0.000, 0.015, false, false, 0.0},
    {"mouthFunnel",      0.000, 0.070, 0.030,  0.000,  0.000, 0.012, false, false, 0.0},
    {"mouthPucker",      0.000, 0.070, 0.025,  0.000,  0.000, 0.010, false, false, 0.0},
    {"mouthClose",       0.000, 0.065, 0.035,  0.000,  0.006, 0.000, false, false, 0.1},
    {"eyeWideLeft",      0.040, 0.170, 0.025,  0.000,  0.010, 0.003, false, false, 0.0},
    {"eyeWideRight",    -0.040, 0.170, 0.025,  0.000,  0.010, 0.003, false, false, 0.0},
    {"tongueOut",        0.000, 0.055, 0.020,  0.000,  0.000, 0.020, false, false, 0.0},
    {"viseme_aa",        0.000, 0.050, 0.040,  0.000, -0.020, 0.006, false, false, 0.0},
    {"viseme_oh",        0.000, 0.070, 0.030,  0.000, -0.005, 0.012, false, false, 0.0},
    {"noseSneerLeft",    0.030, 0.120, 0.015,  0.004,  0.006, 0.002, false, true,  0.0},
};

double vx(u32 i) { return (static_cast<double>(i) / kN - 0.5) * kWidth; }
double vy(u32 j) { return static_cast<double>(j) / kN * kHeight; }

double bump(const Shape& s, u32 i, u32 j) {
    const double dx = vx(i) - s.cx, dy = vy(j) - s.cy;
    return std::exp(-(dx * dx + dy * dy) / (2.0 * s.sigma * s.sigma));
}

// The jaw bone sits at y = 0.075 m; vertices below it follow it, with a ramp across 5 cm.
constexpr double kJawAt = 0.075, kJawRamp = 0.05;
double jawWeight(u32 j) {
    const double w = (kJawAt - vy(j)) / kJawRamp;
    return w <= 0.0 ? 0.0 : (w >= 1.0 ? 1.0 : w);
}

// Animation keys, shared by both animations and by the jaw rotation.
constexpr u32 kTalkKeys = 5;
const double kTalkT[kTalkKeys] = {0.0, 0.25, 0.5, 0.75, 1.0};
// Per key: jawOpen, smile (both sides), blink (both sides), viseme_aa.
const double kJaw[kTalkKeys]   = {0.0, 0.6, 0.1, 0.8, 0.0};
const double kSmile[kTalkKeys] = {0.0, 0.2, 0.5, 0.2, 0.0};
const double kBlink[kTalkKeys] = {0.0, 0.0, 1.0, 0.0, 0.0};
const double kAa[kTalkKeys]    = {0.0, 0.5, 0.0, 0.7, 0.0};
const double kJawDeg[kTalkKeys] = {0.0, 20.0, 5.0, 25.0, 0.0};

double talkValue(u32 shape, u32 key) {
    switch (shape) {
        case 0: return kJaw[key];
        case 1: case 2: return kSmile[key];
        case 3: case 4: return kBlink[key];
        case 13: return kAa[key];
        default: return 0.0;
    }
}

constexpr u32 kEmoteKeys = 3;
const double kEmoteT[kEmoteKeys] = {0.0, 0.5, 1.0};
double emoteValue(u32 shape, u32 key) {
    // A smile that builds and settles, with the brows lifting at the middle.
    if (shape == 1 || shape == 2) return key == 1 ? 0.9 : 0.0;
    if (shape == 5) return key == 1 ? 0.8 : 0.15;
    return 0.0;
}

std::string view(usize off, usize len) {
    return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
}

std::string buildGltf(bool rigged) {
    std::vector<u8> bin;
    std::vector<std::string> views, accessors;
    const auto addView = [&](usize off, usize len) { views.push_back(view(off, len)); return static_cast<int>(views.size()) - 1; };
    const auto addAcc = [&](int viewIdx, int ct, usize count, const char* type, const std::string& extra = "") {
        accessors.push_back(std::string("{") + (viewIdx >= 0 ? "\"bufferView\":" + std::to_string(viewIdx) + "," : "") +
                            "\"componentType\":" + std::to_string(ct) + ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + extra + "}");
        return static_cast<int>(accessors.size()) - 1;
    };

    // --- positions, normals, uvs ---
    usize off = bin.size();
    for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i) { putF(bin, vx(i)); putF(bin, vy(j)); putF(bin, 0.0); }
    const int accPos = addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC3",
                              ",\"min\":[" + num(-kWidth / 2) + ",0,0],\"max\":[" + num(kWidth / 2) + "," + num(kHeight) + ",0]");
    off = bin.size();
    for (u32 n = 0; n < kVerts; ++n) { putF(bin, 0.0); putF(bin, 0.0); putF(bin, 1.0); }
    const int accNrm = addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC3");
    off = bin.size();
    for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i) { putF(bin, static_cast<double>(i) / kN); putF(bin, 1.0 - static_cast<double>(j) / kN); }
    const int accUv = addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC2");

    // --- skin ---
    int accJoints = -1, accWeights = -1;
    if (rigged) {
        off = bin.size();
        for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i) { putU16(bin, 0); putU16(bin, 1); putU16(bin, 0); putU16(bin, 0); }
        accJoints = addAcc(addView(off, bin.size() - off), 5123, kVerts, "VEC4");
        off = bin.size();
        for (u32 j = 0; j < kSide; ++j) {
            const double wj = jawWeight(j);
            for (u32 i = 0; i < kSide; ++i) { putF(bin, 1.0 - wj); putF(bin, wj); putF(bin, 0.0); putF(bin, 0.0); }
        }
        accWeights = addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC4");
    }

    // --- indices ---
    off = bin.size();
    u32 indexCount = 0;
    for (u32 j = 0; j < kN; ++j)
        for (u32 i = 0; i < kN; ++i) {
            const u16 a = static_cast<u16>(j * kSide + i), b = static_cast<u16>(a + 1), c = static_cast<u16>(a + kSide), d = static_cast<u16>(c + 1);
            putU16(bin, a); putU16(bin, b); putU16(bin, d);
            putU16(bin, a); putU16(bin, d); putU16(bin, c);
            indexCount += 6;
        }
    align4(bin);
    const int accIdx = addAcc(addView(off, indexCount * 2), 5123, indexCount, "SCALAR");

    // --- morph targets ---
    std::string targets;
    for (u32 s = 0; s < 16; ++s) {
        const Shape& sh = kShapes[s];
        std::string t = "{";
        if (sh.sparse) {
            std::vector<u16> idx;
            std::vector<u32> where;
            for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i)
                if (bump(sh, i, j) > 0.05) { idx.push_back(static_cast<u16>(j * kSide + i)); where.push_back(j * kSide + i); }
            usize o1 = bin.size();
            for (const u16 v : idx) putU16(bin, v);
            const usize l1 = bin.size() - o1;
            align4(bin);
            const int vIdx = addView(o1, l1);
            usize o2 = bin.size();
            for (const u32 w : where) {
                const u32 i = w % kSide, j = w / kSide;
                const double g = bump(sh, i, j);
                putF(bin, sh.dx * g); putF(bin, sh.dy * g); putF(bin, sh.dz * g);
            }
            const int vVal = addView(o2, bin.size() - o2);
            const int a = addAcc(-1, 5126, kVerts, "VEC3",
                                 ",\"sparse\":{\"count\":" + std::to_string(idx.size()) + ",\"indices\":{\"bufferView\":" + std::to_string(vIdx) +
                                 ",\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(vVal) + "}}");
            t += "\"POSITION\":" + std::to_string(a);
        } else {
            off = bin.size();
            for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i) {
                const double g = bump(sh, i, j);
                putF(bin, sh.dx * g); putF(bin, sh.dy * g); putF(bin, sh.dz * g);
            }
            const int a = addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC3");
            t += "\"POSITION\":" + std::to_string(a);
            if (sh.normals) {
                off = bin.size();
                for (u32 j = 0; j < kSide; ++j) for (u32 i = 0; i < kSide; ++i) { const double g = bump(sh, i, j); putF(bin, 0.0); putF(bin, -0.5 * g); putF(bin, 0.0); }
                t += ",\"NORMAL\":" + std::to_string(addAcc(addView(off, bin.size() - off), 5126, kVerts, "VEC3"));
            }
        }
        t += "}";
        targets += (s ? "," : "") + t;
    }

    // --- animations ---
    off = bin.size();
    for (u32 k = 0; k < kTalkKeys; ++k) putF(bin, kTalkT[k]);
    const int accTalkT = addAcc(addView(off, bin.size() - off), 5126, kTalkKeys, "SCALAR");
    off = bin.size();
    for (u32 k = 0; k < kTalkKeys; ++k) for (u32 s = 0; s < 16; ++s) putF(bin, talkValue(s, k));
    const int accTalkW = addAcc(addView(off, bin.size() - off), 5126, kTalkKeys * 16, "SCALAR");
    off = bin.size();
    for (u32 k = 0; k < kEmoteKeys; ++k) putF(bin, kEmoteT[k]);
    const int accEmoteT = addAcc(addView(off, bin.size() - off), 5126, kEmoteKeys, "SCALAR");
    // CUBICSPLINE: per key an in-tangent, the value and an out-tangent for every target; flat tangents ease in and out.
    off = bin.size();
    for (u32 k = 0; k < kEmoteKeys; ++k) {
        for (u32 s = 0; s < 16; ++s) putF(bin, 0.0);
        for (u32 s = 0; s < 16; ++s) putF(bin, emoteValue(s, k));
        for (u32 s = 0; s < 16; ++s) putF(bin, 0.0);
    }
    const int accEmoteW = addAcc(addView(off, bin.size() - off), 5126, kEmoteKeys * 3 * 16, "SCALAR");
    int accJawRot = -1;
    if (rigged) {
        off = bin.size();
        for (u32 k = 0; k < kTalkKeys; ++k) {
            const double h = kJawDeg[k] * 3.14159265358979323846 / 180.0 * 0.5;   // about glTF +X
            putF(bin, std::sin(h)); putF(bin, 0.0); putF(bin, 0.0); putF(bin, std::cos(h));
        }
        accJawRot = addAcc(addView(off, bin.size() - off), 5126, kTalkKeys, "VEC4");
    }

    // --- the document ---
    std::string names = "[";
    std::string weights = "[";
    for (u32 s = 0; s < 16; ++s) {
        names += std::string(s ? "," : "") + "\"" + kShapes[s].name + "\"";
        weights += (s ? "," : "") + num(kShapes[s].defaultWeight);
    }
    names += "]";
    weights += "]";

    std::string prim = "{\"attributes\":{\"POSITION\":" + std::to_string(accPos) + ",\"NORMAL\":" + std::to_string(accNrm) +
                       ",\"TEXCOORD_0\":" + std::to_string(accUv);
    if (rigged) prim += ",\"JOINTS_0\":" + std::to_string(accJoints) + ",\"WEIGHTS_0\":" + std::to_string(accWeights);
    prim += "},\"indices\":" + std::to_string(accIdx) + ",\"targets\":[" + targets + "]}";

    std::string talkChannels = "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}";
    std::string talkSamplers = "{\"input\":" + std::to_string(accTalkT) + ",\"output\":" + std::to_string(accTalkW) + ",\"interpolation\":\"LINEAR\"}";
    if (rigged) {
        talkChannels += ",{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}}";
        talkSamplers += ",{\"input\":" + std::to_string(accTalkT) + ",\"output\":" + std::to_string(accJawRot) + ",\"interpolation\":\"LINEAR\"}";
    }
    const std::string animations =
        "[{\"name\":\"Talk\",\"channels\":[" + talkChannels + "],\"samplers\":[" + talkSamplers + "]},"
        "{\"name\":\"Emote\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}],"
        "\"samplers\":[{\"input\":" + std::to_string(accEmoteT) + ",\"output\":" + std::to_string(accEmoteW) + ",\"interpolation\":\"CUBICSPLINE\"}]}]";

    std::string nodes, scene;
    if (rigged) {
        nodes = "[{\"name\":\"Face\",\"mesh\":0,\"skin\":0},{\"name\":\"head\",\"children\":[2]},"
                "{\"name\":\"jaw\",\"translation\":[0," + num(kJawAt) + ",0]}]";
        scene = "[0,1]";
    } else {
        nodes = "[{\"name\":\"Face\",\"mesh\":0}]";
        scene = "[0]";
    }

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Aver MakeFace\"},\"scene\":0,\"scenes\":[{\"nodes\":" + scene + "}],\"nodes\":" + nodes;
    if (rigged) json += ",\"skins\":[{\"name\":\"FaceRig\",\"joints\":[1,2]}]";
    json += ",\"meshes\":[{\"name\":\"Face\",\"extras\":{\"targetNames\":" + names + "},\"weights\":" + weights + ",\"primitives\":[" + prim + "]}]";
    json += ",\"animations\":" + animations;
    json += ",\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + b64(bin) + "\",\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":[";
    for (usize i = 0; i < views.size(); ++i) json += (i ? "," : "") + views[i];
    json += "],\"accessors\":[";
    for (usize i = 0; i < accessors.size(); ++i) json += (i ? "," : "") + accessors[i];
    json += "]}";
    return json;
}

// ---------------------------------------------------------------- the rig

anim::FaceRigAsset buildRig() {
    anim::FaceRigAsset r;
    r.convention = "arkit52";
    r.mesh = "FaceRigged.ocmesh";
    r.skeleton = "FaceRigged.ocskel";
    const auto pose = [&](const char* name, std::vector<anim::FaceTargetRef> t) {
        anim::FacePose p;
        p.name = name;
        p.targets = std::move(t);
        r.poses.push_back(std::move(p));
    };
    pose("smile", {{"mouthSmileLeft", 0.9f}, {"mouthSmileRight", 0.9f}, {"cheekPuff", 0.2f}});
    pose("surprise", {{"browInnerUp", 1.0f}, {"eyeWideLeft", 0.8f}, {"eyeWideRight", 0.8f}, {"jawOpen", 0.4f}});
    pose("blink", {{"eyeBlinkLeft", 1.0f}, {"eyeBlinkRight", 1.0f}});
    anim::FacePose open;
    open.name = "talkOpen";
    open.targets = {{"jawOpen", 0.5f}};
    anim::FaceJoint jaw;
    jaw.bone = "jaw";
    jaw.rotation = Quat::fromAxisAngle(Vec3{1, 0, 0}, 12.0f * kDegToRad);
    open.joints = {jaw};
    r.poses.push_back(std::move(open));
    r.phonemes = {{"AA", "viseme_aa"}};
    return r;
}

// ---------------------------------------------------------------- the self-check

void verifyFile(const std::string& path, bool rigged) {
    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(path, res, {}, &why)) {
        AVER_ERROR("  FAIL  {} does not import: {}", path, why);
        ++g_failures;
        return;
    }
    for (const std::string& u : res.unsupported) AVER_INFO("  note  unsupported: {}", u);
    check(res.meshes.size() == 1, "one mesh");
    if (res.meshes.empty()) return;
    const fmt::OcMeshData& m = res.meshes[0];
    check(m.valid() && m.vertexCount() == kVerts, "a valid 625-vertex mesh");
    check(m.morphTargets.size() == 16, "16 morph targets (got " + std::to_string(m.morphTargets.size()) + ")");
    check(m.hasSkin() == rigged, rigged ? "and it carries the jaw skin" : "and no skin");
    for (const std::string& u : res.unsupported) check(u.find("morph targets") == std::string::npos, "morph targets are not reported as unsupported");

    for (u32 s = 0; s < 16 && s < m.morphTargets.size(); ++s)
        if (m.morphTargets[s].name != kShapes[s].name) { check(false, "target " + std::to_string(s) + " is named " + kShapes[s].name); break; }

    // jawOpen: a 3 cm drop (glTF -Y is engine -Z), with normal deltas.
    const fmt::OcMorphTarget* jaw = nullptr;
    const fmt::OcMorphTarget* nose = nullptr;
    const fmt::OcMorphTarget* brow = nullptr;
    for (const fmt::OcMorphTarget& t : m.morphTargets) {
        if (t.name == "jawOpen") jaw = &t;
        if (t.name == "noseSneerLeft") nose = &t;
        if (t.name == "browInnerUp") brow = &t;
    }
    check(jaw && jaw->maxLen > 2.9f && jaw->maxLen <= 3.0001f, "jawOpen moves at most 3 cm (" + std::to_string(jaw ? jaw->maxLen : 0.0f) + ")");
    check(jaw && jaw->maxAbs[2] > 2.9f && jaw->maxAbs[0] < 1e-3f, "and it is along engine Z");
    check(jaw && !jaw->normalDeltas.empty(), "jawOpen carries normal deltas");
    check(nose && !nose->vertices.empty() && nose->vertices.size() < 100, "the sparse target has only its nearby vertices (" +
          std::to_string(nose ? nose->vertices.size() : 0) + ")");
    check(brow && std::fabs(brow->defaultWeight - 0.15f) < 1e-6f, "browInnerUp has its default weight");

    check(res.animations.size() == 2, "two weight animations");
    if (res.animations.size() == 2) {
        const fmt::OcAnimation& talk = res.animations[0];
        const fmt::OcAnimation& emote = res.animations[1];
        check(talk.curves.size() == 16 && emote.curves.size() == 16, "16 morph curves each");
        check(talk.valid() && emote.valid(), "both clips are valid");
        check(talk.tracks.empty() == !rigged, rigged ? "Talk also carries the jaw's bone track" : "Talk is a curve-only clip");
        check(std::fabs(talk.duration - 1.0f) < 1e-5f, "Talk lasts a second");
        const fmt::OcCurve* c = nullptr;
        for (const fmt::OcCurve& x : talk.curves) if (x.name == "morph.jawOpen") c = &x;
        check(c && c->values.size() == kTalkKeys && std::fabs(c->values[3] - 0.8f) < 1e-6f, "morph.jawOpen reads 0.8 at 0.75 s");
        const fmt::OcCurve* e = nullptr;
        for (const fmt::OcCurve& x : emote.curves) if (x.name == "morph.mouthSmileLeft") e = &x;
        check(e && e->interp == fmt::OcInterp::CubicSpline && e->inTangents.size() == kEmoteKeys && std::fabs(e->values[1] - 0.9f) < 1e-6f,
              "Emote is a cubic spline with tangents");
    }
    if (rigged) check(res.skeletons.size() == 1 && res.skeletons[0].bones.size() == 2, "a two-bone skeleton");
}

bool writeText(const std::string& path, const std::string& text) {
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), fp);
        std::fclose(fp);
        return true;
    }
    return false;
}

} // namespace

// Writes <dir>/Face.gltf, FaceRigged.gltf and Face.ocface and verifies them. 0 on success, 1 on a failed check, 2 on bad usage.
int main(int argc, char** argv) {
    if (argc < 2) { AVER_ERROR("usage: MakeFace <output-directory>"); return exitCode(ExitCode::Usage); }
    std::string dir = argv[1];
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();

    const std::string facePath = dir + "/Face.gltf", riggedPath = dir + "/FaceRigged.gltf", rigPath = dir + "/Face.ocface";
    const std::string face = buildGltf(false), rigged = buildGltf(true);
    if (!writeText(facePath, face) || !writeText(riggedPath, rigged)) {
        AVER_ERROR("could not write into {}", dir);
        return exitCode(ExitCode::Failed);
    }
    std::string why;
    if (!anim::saveFaceRig(rigPath, buildRig(), &why)) {
        AVER_ERROR("could not write {}: {}", rigPath, why);
        return exitCode(ExitCode::Failed);
    }
    AVER_INFO("wrote {} ({} bytes), {} ({} bytes), {}", facePath, face.size(), riggedPath, rigged.size(), rigPath);

    AVER_INFO("verifying Face.gltf");
    verifyFile(facePath, false);
    AVER_INFO("verifying FaceRigged.gltf");
    verifyFile(riggedPath, true);
    AVER_INFO("verifying Face.ocface");
    {
        anim::FaceRigAsset back;
        check(anim::loadFaceRig(rigPath, back, &why) && back.valid(&why), "the rig reloads and is valid: " + why);
        check(back.poses.size() == 4 && back.find("talkOpen") && back.find("talkOpen")->joints.size() == 1, "four poses, one with a bone delta");
        // every blend shape a pose names is on the mesh
        bool ok = true;
        for (const anim::FacePose& p : back.poses)
            for (const anim::FaceTargetRef& t : p.targets) {
                bool found = false;
                for (const Shape& s : kShapes) if (t.name == s.name) found = true;
                ok = ok && found;
            }
        check(ok, "every target the poses use exists on the mesh");
    }

    if (g_failures) AVER_ERROR("MakeFace: {} FAILURES", g_failures);
    else AVER_INFO("MakeFace: all checks passed");
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
