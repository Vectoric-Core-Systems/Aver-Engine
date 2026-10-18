// .ocface: the face rig asset, the built-in ARKit/viseme rig, and per-entity pose resolution.
// See FaceRig.hpp and docs/plans/wave9/MORPH_TARGETS.md 2.6.
#include "aver/anim/FaceRig.hpp"

#include "AnimJson.hpp"
#include "aver/anim/Morph.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace aver::anim {

namespace {

using ajson::esc;
using ajson::num;
using ajson::str;

bool finiteIn(f32 v, f32 lo, f32 hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool fail(std::string* why, std::string m) {
    if (why) *why = std::move(m);
    return false;
}

std::string lowerOf(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
std::string upperOf(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

// ---- the built-in rig's tables (starting art; the structure is frozen, the numbers are tunable) ----

struct TW { const char* target; f32 weight; };
struct VisemeDef { const char* name; const TW* tw; u32 count; };

const TW kPP[] = {{"mouthClose", .8f}, {"mouthPressLeft", .5f}, {"mouthPressRight", .5f}};
const TW kFF[] = {{"mouthRollLower", .6f}, {"mouthUpperUpLeft", .3f}, {"mouthUpperUpRight", .3f}, {"jawOpen", .1f}};
const TW kTH[] = {{"tongueOut", .5f}, {"jawOpen", .15f}};
const TW kDD[] = {{"jawOpen", .2f}, {"mouthUpperUpLeft", .15f}, {"mouthUpperUpRight", .15f}};
const TW kKK[] = {{"jawOpen", .25f}, {"mouthFunnel", .1f}};
const TW kCH[] = {{"mouthFunnel", .5f}, {"mouthPucker", .3f}, {"jawOpen", .15f}};
const TW kSS[] = {{"mouthSmileLeft", .3f}, {"mouthSmileRight", .3f}, {"jawOpen", .05f}};
const TW kNN[] = {{"jawOpen", .15f}, {"mouthClose", .1f}};
const TW kRR[] = {{"mouthPucker", .4f}, {"mouthFunnel", .25f}, {"jawOpen", .15f}};
const TW kAA[] = {{"jawOpen", .7f}, {"mouthLowerDownLeft", .3f}, {"mouthLowerDownRight", .3f}};
const TW kE[]  = {{"jawOpen", .3f}, {"mouthSmileLeft", .4f}, {"mouthSmileRight", .4f}, {"mouthStretchLeft", .2f}, {"mouthStretchRight", .2f}};
const TW kIH[] = {{"jawOpen", .25f}, {"mouthSmileLeft", .3f}, {"mouthSmileRight", .3f}};
const TW kOH[] = {{"jawOpen", .4f}, {"mouthFunnel", .6f}, {"mouthPucker", .2f}};
const TW kOU[] = {{"jawOpen", .2f}, {"mouthPucker", .7f}, {"mouthFunnel", .4f}};

#define AVER_FACE_N(a) static_cast<u32>(sizeof(a) / sizeof(a[0]))
const VisemeDef kVisemeDefs[kVisemeCount] = {
    {"viseme_sil", nullptr, 0},
    {"viseme_PP", kPP, AVER_FACE_N(kPP)}, {"viseme_FF", kFF, AVER_FACE_N(kFF)}, {"viseme_TH", kTH, AVER_FACE_N(kTH)},
    {"viseme_DD", kDD, AVER_FACE_N(kDD)}, {"viseme_kk", kKK, AVER_FACE_N(kKK)}, {"viseme_CH", kCH, AVER_FACE_N(kCH)},
    {"viseme_SS", kSS, AVER_FACE_N(kSS)}, {"viseme_nn", kNN, AVER_FACE_N(kNN)}, {"viseme_RR", kRR, AVER_FACE_N(kRR)},
    {"viseme_aa", kAA, AVER_FACE_N(kAA)}, {"viseme_E", kE, AVER_FACE_N(kE)}, {"viseme_ih", kIH, AVER_FACE_N(kIH)},
    {"viseme_oh", kOH, AVER_FACE_N(kOH)}, {"viseme_ou", kOU, AVER_FACE_N(kOU)},
};

struct ArpaRow { const char* symbol; const char* viseme; };
const ArpaRow kArpa[kArpabetCount] = {
    {"AA", "viseme_aa"}, {"AE", "viseme_aa"}, {"AH", "viseme_aa"}, {"AO", "viseme_oh"}, {"AW", "viseme_ou"},
    {"AY", "viseme_aa"}, {"B", "viseme_PP"},  {"CH", "viseme_CH"}, {"D", "viseme_DD"},  {"DH", "viseme_TH"},
    {"EH", "viseme_E"},  {"ER", "viseme_RR"}, {"EY", "viseme_E"},  {"F", "viseme_FF"},  {"G", "viseme_kk"},
    {"HH", "viseme_ih"}, {"IH", "viseme_ih"}, {"IY", "viseme_ih"}, {"JH", "viseme_CH"}, {"K", "viseme_kk"},
    {"L", "viseme_DD"},  {"M", "viseme_PP"},  {"N", "viseme_nn"},  {"NG", "viseme_kk"}, {"OW", "viseme_oh"},
    {"OY", "viseme_oh"}, {"P", "viseme_PP"},  {"R", "viseme_RR"},  {"S", "viseme_SS"},  {"SH", "viseme_CH"},
    {"T", "viseme_DD"},  {"TH", "viseme_TH"}, {"UH", "viseme_ou"}, {"UW", "viseme_ou"}, {"V", "viseme_FF"},
    {"W", "viseme_ou"},  {"Y", "viseme_ih"},  {"Z", "viseme_SS"},  {"ZH", "viseme_CH"}};

void readVec3(const fmt::JsonValue& a, Vec3& v) {
    if (a.size() >= 3) v = Vec3{a[usize(0)].asFloat(v.x), a[usize(1)].asFloat(v.y), a[usize(2)].asFloat(v.z)};
}
void readQuat(const fmt::JsonValue& a, Quat& q) {
    if (a.size() >= 4) q = Quat{a[usize(0)].asFloat(), a[usize(1)].asFloat(), a[usize(2)].asFloat(), a[usize(3)].asFloat(1.0f)};
}

std::string vec3Json(const Vec3& v) { return "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "]"; }

} // namespace

// ---- asset ----

const FacePose* FaceRigAsset::find(std::string_view name) const {
    for (const FacePose& p : poses) if (p.name == name) return &p;
    return nullptr;
}

bool FaceRigAsset::valid(std::string* why) const {
    if (poses.size() > kFaceRigMaxPoses) return fail(why, "more than 256 poses");
    std::set<std::string> seen;
    for (const FacePose& p : poses) {
        if (p.name.empty()) return fail(why, "a pose has no name");
        if (p.name.size() > kFaceNameMax) return fail(why, "pose '" + p.name.substr(0, 32) + "...' has a name over 128 bytes");
        if (!seen.insert(p.name).second) return fail(why, "two poses are named '" + p.name + "'");
        if (p.targets.size() > kFaceRigMaxTargets) return fail(why, "pose '" + p.name + "' has more than 32 targets");
        if (p.joints.size() > kFaceRigMaxJoints) return fail(why, "pose '" + p.name + "' has more than 16 joints");
        for (const FaceTargetRef& t : p.targets) {
            if (t.name.empty() || t.name.size() > kFaceNameMax) return fail(why, "pose '" + p.name + "' has a target with an empty or over-long name");
            if (!finiteIn(t.weight, kMorphWeightMin, kMorphWeightMax))
                return fail(why, "pose '" + p.name + "' target '" + t.name + "' has a weight outside [-4, 4]");
        }
        for (const FaceJoint& j : p.joints) {
            if (j.bone.empty() || j.bone.size() > kFaceNameMax) return fail(why, "pose '" + p.name + "' has a joint with an empty or over-long bone name");
            const f32 v[] = {j.position.x, j.position.y, j.position.z, j.rotation.x, j.rotation.y, j.rotation.z,
                             j.rotation.w, j.scale.x, j.scale.y, j.scale.z};
            for (const f32 f : v) if (!std::isfinite(f)) return fail(why, "pose '" + p.name + "' joint '" + j.bone + "' has a non-finite value");
        }
    }
    for (const auto& ph : phonemes)
        if (ph.first.empty() || ph.second.empty() || ph.first.size() > kFaceNameMax || ph.second.size() > kFaceNameMax)
            return fail(why, "a phoneme mapping has an empty or over-long name");
    const f32 lp[] = {lipsync.lead, lipsync.attack, lipsync.release, lipsync.minHold, lipsync.maxTotal};
    for (const f32 f : lp) if (!std::isfinite(f) || f < 0.0f) return fail(why, "a lip-sync parameter is negative or not finite");
    return true;
}

std::string writeFaceRig(const FaceRigAsset& a) {
    std::string s = "{\n  \"format\": \"ocface\",\n  \"version\": " + std::to_string(kFaceRigVersion) + ",\n";
    s += "  \"convention\": " + esc(a.convention) + ",\n";
    s += "  \"mesh\": " + esc(a.mesh) + ",\n  \"skeleton\": " + esc(a.skeleton) + ",\n";
    s += "  \"poses\": [";
    for (usize i = 0; i < a.poses.size(); ++i) {
        const FacePose& p = a.poses[i];
        s += i ? ",\n    " : "\n    ";
        s += "{\"name\": " + esc(p.name) + ", \"targets\": [";
        for (usize k = 0; k < p.targets.size(); ++k) {
            if (k) s += ", ";
            s += "{\"name\": " + esc(p.targets[k].name) + ", \"weight\": " + num(p.targets[k].weight) + "}";
        }
        s += "], \"joints\": [";
        for (usize k = 0; k < p.joints.size(); ++k) {
            const FaceJoint& j = p.joints[k];
            if (k) s += ", ";
            s += "{\"bone\": " + esc(j.bone) + ", \"position\": " + vec3Json(j.position) + ", \"rotation\": [" +
                 num(j.rotation.x) + ", " + num(j.rotation.y) + ", " + num(j.rotation.z) + ", " + num(j.rotation.w) +
                 "], \"scale\": " + vec3Json(j.scale) + "}";
        }
        s += "]}";
    }
    s += a.poses.empty() ? "],\n" : "\n  ],\n";
    s += "  \"phonemes\": {";
    for (usize i = 0; i < a.phonemes.size(); ++i) {
        s += i ? ", " : "";
        s += esc(a.phonemes[i].first) + ": " + esc(a.phonemes[i].second);
    }
    s += "},\n";
    s += "  \"lipsync\": {\"lead\": " + num(a.lipsync.lead) + ", \"attack\": " + num(a.lipsync.attack) +
         ", \"release\": " + num(a.lipsync.release) + ", \"minHold\": " + num(a.lipsync.minHold) +
         ", \"maxTotal\": " + num(a.lipsync.maxTotal) + "}\n}\n";
    return s;
}

bool parseFaceRig(std::string_view json, FaceRigAsset& out, std::string* why) {
    fmt::JsonValue root;
    if (!fmt::parseJson(json, root, why)) return false;
    if (!ajson::header(root, "ocface", kFaceRigVersion, why)) return false;
    FaceRigAsset a;
    a.convention = str(root, "convention");
    if (a.convention.empty()) a.convention = "arkit52";
    a.mesh = str(root, "mesh");
    a.skeleton = str(root, "skeleton");
    const fmt::JsonValue& ps = root["poses"];
    for (usize i = 0; i < ps.size(); ++i) {
        const fmt::JsonValue& pv = ps[i];
        FacePose p;
        p.name = str(pv, "name");
        const fmt::JsonValue& ts = pv["targets"];
        for (usize k = 0; k < ts.size(); ++k) {
            FaceTargetRef t;
            t.name = str(ts[k], "name");
            t.weight = ts[k]["weight"].asFloat(1.0f);
            p.targets.push_back(std::move(t));
        }
        const fmt::JsonValue& js = pv["joints"];
        for (usize k = 0; k < js.size(); ++k) {
            FaceJoint j;
            j.bone = str(js[k], "bone");
            readVec3(js[k]["position"], j.position);
            readQuat(js[k]["rotation"], j.rotation);
            readVec3(js[k]["scale"], j.scale);
            p.joints.push_back(std::move(j));
        }
        a.poses.push_back(std::move(p));
    }
    const fmt::JsonValue& ph = root["phonemes"];
    if (ph.isObject())
        for (const fmt::JsonMember& m : ph.members()) a.phonemes.push_back({m.key, std::string(m.value.asString())});
    const fmt::JsonValue& ls = root["lipsync"];
    a.lipsync.lead = ls["lead"].asFloat(a.lipsync.lead);
    a.lipsync.attack = ls["attack"].asFloat(a.lipsync.attack);
    a.lipsync.release = ls["release"].asFloat(a.lipsync.release);
    a.lipsync.minHold = ls["minHold"].asFloat(a.lipsync.minHold);
    a.lipsync.maxTotal = ls["maxTotal"].asFloat(a.lipsync.maxTotal);
    out = std::move(a);
    return true;
}

bool saveFaceRig(const std::string& path, const FaceRigAsset& a, std::string* why) {
    const std::string tmp = path + ".tmp";
    if (!ajson::writeFile(tmp, writeFaceRig(a), why)) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // rename onto an existing file can fail on some platforms: remove and retry once.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (ec) return fail(why, "cannot replace " + path);
    }
    return true;
}

bool loadFaceRig(const std::string& path, FaceRigAsset& out, std::string* why) {
    std::string text;
    return ajson::readFile(path, text, why) && parseFaceRig(text, out, why);
}

const FaceRigAsset& defaultFaceRig() {
    static const FaceRigAsset rig = [] {
        FaceRigAsset a;
        a.convention = "arkit52";
        for (u32 i = 0; i < kArkitShapeCount; ++i) {
            FacePose p;
            p.name = kArkitShapes[i];
            p.targets.push_back({kArkitShapes[i], 1.0f});
            a.poses.push_back(std::move(p));
        }
        for (const VisemeDef& d : kVisemeDefs) {
            FacePose p;
            p.name = d.name;
            for (u32 k = 0; k < d.count; ++k) p.targets.push_back({d.tw[k].target, d.tw[k].weight});
            a.poses.push_back(std::move(p));
        }
        return a;
    }();
    return rig;
}

const char* arpabetViseme(std::string_view symbol) {
    for (const ArpaRow& r : kArpa) if (symbol == r.symbol) return r.viseme;
    if (symbol == "sil" || symbol == "sp" || symbol == "pau") return "viseme_sil";
    return nullptr;
}

const char* arpabetSymbol(u32 index) { return index < kArpabetCount ? kArpa[index].symbol : nullptr; }

std::string resolveVisemeName(const FaceRigAsset* rig, std::string_view symbol, bool visemeOnly) {
    if (symbol.empty()) return {};
    if (!visemeOnly) {
        if (rig)
            for (const auto& ph : rig->phonemes) if (ph.first == symbol) return ph.second;
        if (const char* v = arpabetViseme(symbol)) return v;
        const std::string up = upperOf(symbol);
        if (const char* v = arpabetViseme(up)) return v;
    }
    // A viseme name as is: "viseme_aa", "aa", or any case of either.
    const std::string low = lowerOf(symbol);
    for (u32 i = 0; i < kVisemeCount; ++i) {
        const std::string full = kVisemeNames[i];
        if (symbol == full || lowerOf(full) == low) return full;
        const std::string shortName = full.substr(7);
        if (symbol == shortName || lowerOf(shortName) == low) return full;
    }
    return {};
}

// ---- runtime ----

void FaceRigRuntime::compile(const FaceRigAsset& a, std::unordered_map<u64, CompiledPose>& out) {
    out.clear();
    for (const FacePose& p : a.poses) {
        CompiledPose c;
        c.src = &p;
        for (const FaceTargetRef& t : p.targets) c.targets.push_back({fnv1a64(std::string_view(t.name)), t.weight});
        out[fnv1a64(std::string_view(p.name))] = std::move(c);
    }
}

void FaceRigRuntime::bind(const FaceRigAsset* rig) {
    rig_ = rig;
    mine_.clear();
    if (rig) compile(*rig, mine_);
    boundSkel_ = nullptr;
    boundBones_ = 0;
    boneIndex_.clear();
    warned_.clear();
}

void FaceRigRuntime::setDeclared(const std::vector<u64>& ids, bool known) {
    declared_.clear();
    declared_.insert(ids.begin(), ids.end());
    declaredKnown_ = known;
}

namespace {
const std::unordered_map<u64, const FacePose*>& defaultPoses() {
    static const std::unordered_map<u64, const FacePose*> m = [] {
        std::unordered_map<u64, const FacePose*> t;
        for (const FacePose& p : defaultFaceRig().poses) t[fnv1a64(std::string_view(p.name))] = &p;
        return t;
    }();
    return m;
}
}

const FaceRigRuntime::CompiledPose* FaceRigRuntime::lookup(u64 poseId) const {
    const auto it = mine_.find(poseId);
    return it == mine_.end() ? nullptr : &it->second;
}

bool FaceRigRuntime::expand(u64 poseId, f32 w, std::vector<FaceTargetOut>& out) const {
    if (const CompiledPose* c = lookup(poseId)) {                      // 1: the entity's own pose
        for (const auto& t : c->targets) out.push_back({t.first, t.second * w});
        return true;
    }
    if (declaredKnown_ && declared_.count(poseId)) {                          // 2: a mesh target of that name
        out.push_back({poseId, w});
        return true;
    }
    const auto& dp = defaultPoses();                                          // 3: the built-in rig
    const auto it = dp.find(poseId);
    if (it == dp.end()) return false;
    for (const FaceTargetRef& t : it->second->targets) out.push_back({fnv1a64(std::string_view(t.name)), t.weight * w});
    return true;
}

bool FaceRigRuntime::hasJoints(u64 poseId) const {
    if (const CompiledPose* c = lookup(poseId)) return c->src && !c->src->joints.empty();
    const auto& dp = defaultPoses();
    const auto it = dp.find(poseId);
    return it != dp.end() && !it->second->joints.empty();
}

void FaceRigRuntime::applyJoints(u64 poseId, f32 w, const fmt::OcSkeleton& skel, Pose& pose) {
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    if (w <= 0.0f || pose.local.size() != skel.bones.size()) return;
    const FacePose* fp = nullptr;
    if (const CompiledPose* c = lookup(poseId)) fp = c->src;
    else { const auto& dp = defaultPoses(); const auto it = dp.find(poseId); if (it != dp.end()) fp = it->second; }
    if (!fp || fp->joints.empty()) return;

    if (boundSkel_ != &skel || boundBones_ != skel.bones.size()) {
        boundSkel_ = &skel;
        boundBones_ = skel.bones.size();
        boneIndex_.clear();
        for (usize i = 0; i < skel.bones.size(); ++i) boneIndex_.emplace(skel.bones[i].name, static_cast<i32>(i));
    }
    for (const FaceJoint& j : fp->joints) {
        const auto bi = boneIndex_.find(j.bone);
        if (bi == boneIndex_.end()) {
            if (warned_.insert(j.bone).second)
                AVER_WARN("[Face] pose '{}' moves bone '{}', which the skeleton does not have; skipped", fp->name, j.bone);
            continue;
        }
        Transform& t = pose.local[static_cast<usize>(bi->second)];
        t.position = t.position + j.position * w;
        t.rotation = (t.rotation * Quat::slerp(Quat::identity(), j.rotation.normalized(), w)).normalized();
        t.scale = Vec3{t.scale.x * (1.0f + (j.scale.x - 1.0f) * w), t.scale.y * (1.0f + (j.scale.y - 1.0f) * w),
                       t.scale.z * (1.0f + (j.scale.z - 1.0f) * w)};
    }
}

} // namespace aver::anim
