// .ocsoft read/write, the preset resolution, validation and the paint run-length codec.
#include "aver/softbody/SoftAsset.hpp"

#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>

namespace aver::softbody {
namespace {

using fmt::JsonValue;

const char* behaviorKey(Behavior b) {
    switch (b) {
        case Behavior::Fracture: return "fracture";
        case Behavior::Shatter:  return "shatter";
        default:                 return "deform";
    }
}

bool behaviorFromKey(std::string_view k, Behavior& out) {
    if (k == "deform")   { out = Behavior::Deform;   return true; }
    if (k == "fracture") { out = Behavior::Fracture; return true; }
    if (k == "shatter")  { out = Behavior::Shatter;  return true; }
    return false;
}

const char* axisKey(u8 a) { return a == 0 ? "x" : (a == 1 ? "y" : "z"); }

void readF(const JsonValue& o, const char* key, f32& dst) {
    const JsonValue& v = o[key];
    if (v.isNumber()) dst = v.asFloat();
}
void readU(const JsonValue& o, const char* key, u32& dst) {
    const JsonValue& v = o[key];
    if (v.isNumber() && v.asDouble() >= 0.0) dst = static_cast<u32>(v.asDouble());
}
void readB(const JsonValue& o, const char* key, bool& dst) {
    const JsonValue& v = o[key];
    if (v.isBool()) dst = v.asBool();
}

bool finite(f32 v) { return std::isfinite(v); }

// ---- writer ----------------------------------------------------------------------------------------

std::string num(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    return buf;
}
std::string unum(u32 v) { return std::to_string(v); }

std::string esc(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
    return out;
}

std::string hex64(u64 v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

// ---- paint codec -----------------------------------------------------------------------------------

std::string encodePaintRle(const std::vector<u8>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    usize i = 0;
    while (i < bytes.size()) {
        const u8 v = bytes[i];
        usize run = 1;
        while (i + run < bytes.size() && bytes[i + run] == v && run < 255) ++run;
        out += kHex[v >> 4]; out += kHex[v & 15];
        out += kHex[(run >> 4) & 15]; out += kHex[run & 15];
        i += run;
    }
    return out;
}

bool decodePaintRle(std::string_view hex, std::vector<u8>& out) {
    out.clear();
    if (hex.size() % 4 != 0) return false;
    auto nib = [](char c, int& v) {
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        return true;
    };
    for (usize i = 0; i < hex.size(); i += 4) {
        int n[4];
        for (int k = 0; k < 4; ++k)
            if (!nib(hex[i + static_cast<usize>(k)], n[k])) { out.clear(); return false; }
        const u8 v = static_cast<u8>(n[0] * 16 + n[1]);
        const u32 run = static_cast<u32>(n[2] * 16 + n[3]);
        if (run == 0) { out.clear(); return false; }
        out.insert(out.end(), run, v);
    }
    return true;
}

f32 reachFromPaint(u8 byte, f32 defaultReachCm) {
    if (byte == 255) return -1.0f;
    return static_cast<f32>(byte) / 254.0f * 2.0f * defaultReachCm;
}

// ---- asset -----------------------------------------------------------------------------------------

SoftAsset makeSoftAsset(SoftKind kind, SoftPreset preset) {
    SoftAsset a;
    a.kind = kind;
    a.preset = preset;
    const PresetInfo& info = presetInfo(preset);
    a.material = info.material;
    a.sim.iterations = info.iterations;
    a.sim.damping = info.damping;
    return a;
}

bool SoftAsset::valid(std::string* why) const {
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (sim.iterations < 1 || sim.iterations > 64) return fail("sim.iterations must be 1..64");
    if (!finite(sim.damping) || sim.damping < 0.0f) return fail("sim.damping must be >= 0");
    if (!finite(sim.vertexRadiusCm) || sim.vertexRadiusCm < 0.0f) return fail("sim.vertexRadiusCm must be >= 0");
    if (!finite(sim.maxSpeedCmS) || sim.maxSpeedCmS <= 0.0f) return fail("sim.maxSpeedCmS must be > 0");
    if (!finite(sim.gravityScale)) return fail("sim.gravityScale must be finite");
    if (!finite(sim.weldCm) || sim.weldCm < 0.0f || sim.weldCm > 10.0f) return fail("sim.weldCm must be 0..10");
    if (sim.maxParticles < 3 || sim.maxParticles > 65536) return fail("sim.maxParticles must be 3..65536");
    const Material& m = material;
    if (!finite(m.compliance) || m.compliance < 0.0f) return fail("material.compliance must be >= 0");
    if (!finite(m.bendCompliance) || m.bendCompliance < 0.0f) return fail("material.bendCompliance must be >= 0");
    if (!finite(m.areaDensityKgM2) || m.areaDensityKgM2 <= 0.0f) return fail("material.areaDensityKgM2 must be > 0");
    if (!finite(m.friction) || m.friction < 0.0f) return fail("material.friction must be >= 0");
    if (!finite(m.restitution) || m.restitution < 0.0f || m.restitution > 1.0f) return fail("material.restitution must be 0..1");
    if (!finite(m.yieldStrain) || m.yieldStrain < 0.0f) return fail("material.yieldStrain must be >= 0");
    if (!finite(m.hardening) || m.hardening < 0.0f) return fail("material.hardening must be >= 0");
    if (!finite(m.maxSetFrac) || m.maxSetFrac < 0.0f) return fail("material.maxSetFrac must be >= 0");
    if (!finite(m.breakStrain) || m.breakStrain < 0.0f) return fail("material.breakStrain must be >= 0");
    if (!finite(m.toughnessJ) || m.toughnessJ < 0.0f) return fail("material.toughnessJ must be >= 0");
    if (kind == SoftKind::Cloth) {
        if (!finite(cloth.dragCoeff) || cloth.dragCoeff < 0.0f || cloth.dragCoeff > 2.0f) return fail("cloth.dragCoeff must be 0..2");
        if (!finite(cloth.liftCoeff) || cloth.liftCoeff < 0.0f || cloth.liftCoeff > 2.0f) return fail("cloth.liftCoeff must be 0..2");
        if (!finite(cloth.defaultReachCm) || cloth.defaultReachCm < 0.0f) return fail("cloth.skin.defaultReachCm must be >= 0");
        if (!finite(cloth.backstopCm)) return fail("cloth.skin.backstopCm must be finite");
    } else {
        if (!finite(destruct.minImpactCmS) || destruct.minImpactCmS < 0.0f) return fail("destruct.minImpactCmS must be >= 0");
        if (!finite(destruct.impulseEfficiency) || destruct.impulseEfficiency <= 0.0f || destruct.impulseEfficiency > 1.0f)
            return fail("destruct.impulseEfficiency must be in (0, 1]");
        if (!finite(destruct.maxKickCmS) || destruct.maxKickCmS < 0.0f) return fail("destruct.maxKickCmS must be >= 0");
        if (destruct.minPieceTriangles < 1) return fail("destruct.minPieceTriangles must be >= 1");
        if (!finite(destruct.debrisLifetimeS) || destruct.debrisLifetimeS < 0.0f) return fail("destruct.debrisLifetimeS must be >= 0");
        if (destruct.maxPieces < 1) return fail("destruct.maxPieces must be >= 1");
    }
    if (paint.hasPin() && paint.hasReach() && paint.pin.size() != paint.reach.size())
        return fail("paint.pin and paint.reach differ in length");
    for (const PaintRule& r : paint.rules) {
        if (r.axis > 2) return fail("paint rule axis must be x, y or z");
        switch (r.type) {
            case PaintRule::Type::Above:  if (!finite(r.valueCm)) return fail("paint rule 'above' needs a finite valueCm"); break;
            case PaintRule::Type::Bone:   if (r.bone.empty()) return fail("paint rule 'bone' needs a bone name");
                                          if (!finite(r.minWeight) || r.minWeight < 0.0f || r.minWeight > 1.0f) return fail("paint rule minWeight must be 0..1");
                                          break;
            case PaintRule::Type::Sphere: if (!finite(r.radiusCm) || r.radiusCm <= 0.0f) return fail("paint rule 'sphere' needs radiusCm > 0"); break;
            case PaintRule::Type::Mask:   if (r.texture.empty()) return fail("paint rule 'mask' needs a texture");
                                          if (!finite(r.pinBelow)) return fail("paint rule pinBelow must be finite"); break;
        }
    }
    return true;
}

bool parseSoftAsset(std::string_view text, SoftAsset& out, std::string* why) {
    auto fail = [&](const std::string& msg) {
        if (why) *why = msg;
        return false;
    };
    JsonValue root;
    std::string jerr;
    if (!fmt::parseJson(text, root, &jerr)) return fail(jerr);
    if (!root.isObject()) return fail("not a JSON object");
    if (root["format"].asString() != "ocsoft") return fail("format is not \"ocsoft\"");
    const i64 version = root["version"].asInt(0);
    if (version < 1) return fail("missing or invalid version");
    if (version > static_cast<i64>(kSoftAssetVersion)) return fail("newer .ocsoft version than this build reads");

    SoftKind kind = SoftKind::Cloth;
    const std::string_view kindKey = root["kind"].asString("cloth");
    if (kindKey == "cloth") kind = SoftKind::Cloth;
    else if (kindKey == "destructible" || kindKey == "destruct") kind = SoftKind::Destructible;
    else return fail("kind must be \"cloth\" or \"destructible\"");

    SoftPreset preset = kind == SoftKind::Cloth ? SoftPreset::ClothLight : SoftPreset::Metal;
    if (root.has("preset") && !presetFromKey(root["preset"].asString(), preset)) return fail("unknown preset");

    SoftAsset a = makeSoftAsset(kind, preset);

    const JsonValue& mat = root["material"];
    if (mat.isObject()) {
        Material& m = a.material;
        readF(mat, "areaDensityKgM2", m.areaDensityKgM2);
        readF(mat, "compliance", m.compliance);
        readF(mat, "bendCompliance", m.bendCompliance);
        readF(mat, "yieldStrain", m.yieldStrain);
        readF(mat, "hardening", m.hardening);
        readF(mat, "maxSetFrac", m.maxSetFrac);
        readF(mat, "breakStrain", m.breakStrain);
        readF(mat, "toughnessJ", m.toughnessJ);
        readF(mat, "friction", m.friction);
        readF(mat, "restitution", m.restitution);
        if (mat.has("maxSetCm")) {
            const f32 cm = mat["maxSetCm"].asFloat();
            m.maxBend = cm > 0.0f ? cm : 1e9f;
        }
        if (mat.has("behavior") && !behaviorFromKey(mat["behavior"].asString(), m.behavior)) return fail("unknown material.behavior");
    }
    const JsonValue& sim = root["sim"];
    if (sim.isObject()) {
        readU(sim, "iterations", a.sim.iterations);
        readF(sim, "damping", a.sim.damping);
        readF(sim, "vertexRadiusCm", a.sim.vertexRadiusCm);
        readF(sim, "maxSpeedCmS", a.sim.maxSpeedCmS);
        readF(sim, "gravityScale", a.sim.gravityScale);
        readF(sim, "weldCm", a.sim.weldCm);
        readU(sim, "maxParticles", a.sim.maxParticles);
        readB(sim, "allowSleep", a.sim.allowSleep);
    }
    const JsonValue& cl = root["cloth"];
    if (cl.isObject()) {
        readF(cl, "dragCoeff", a.cloth.dragCoeff);
        readF(cl, "liftCoeff", a.cloth.liftCoeff);
        readB(cl, "tearable", a.cloth.tearable);
        const JsonValue& skin = cl["skin"];
        if (skin.isObject()) {
            readF(skin, "defaultReachCm", a.cloth.defaultReachCm);
            readF(skin, "backstopCm", a.cloth.backstopCm);
        }
    }
    const JsonValue& de = root["destruct"];
    if (de.isObject()) {
        readF(de, "minImpactCmS", a.destruct.minImpactCmS);
        readF(de, "impulseEfficiency", a.destruct.impulseEfficiency);
        readF(de, "maxKickCmS", a.destruct.maxKickCmS);
        readU(de, "minPieceTriangles", a.destruct.minPieceTriangles);
        readF(de, "debrisLifetimeS", a.destruct.debrisLifetimeS);
        readU(de, "maxPieces", a.destruct.maxPieces);
    }
    const JsonValue& pt = root["paint"];
    if (pt.isObject()) {
        const std::string_view key = pt["meshKey"].asString();
        if (!key.empty()) {
            u64 k = 0;
            for (const char c : key) {
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return fail("paint.meshKey is not hex");
                k = (k << 4) | static_cast<u64>(d);
            }
            a.paint.meshKey = k;
        }
        if (pt.has("pin") && !decodePaintRle(pt["pin"].asString(), a.paint.pin)) return fail("paint.pin is not valid run-length hex");
        if (pt.has("reach") && !decodePaintRle(pt["reach"].asString(), a.paint.reach)) return fail("paint.reach is not valid run-length hex");
        const JsonValue& rules = pt["rules"];
        for (usize i = 0; i < rules.size(); ++i) {
            const JsonValue& rj = rules[i];
            if (!rj.isObject()) return fail("paint.rules entries must be objects");
            PaintRule r;
            const std::string_view t = rj["type"].asString();
            if (t == "above") {
                r.type = PaintRule::Type::Above;
                const std::string_view ax = rj["axis"].asString("z");
                r.axis = ax == "x" ? 0 : (ax == "y" ? 1 : (ax == "z" ? 2 : 3));
                readF(rj, "valueCm", r.valueCm);
            } else if (t == "bone") {
                r.type = PaintRule::Type::Bone;
                r.bone = std::string(rj["bone"].asString());
                readF(rj, "minWeight", r.minWeight);
            } else if (t == "sphere") {
                r.type = PaintRule::Type::Sphere;
                const JsonValue& c = rj["centreCm"];
                for (usize k = 0; k < 3; ++k) r.centreCm[k] = c[k].asFloat();
                readF(rj, "radiusCm", r.radiusCm);
            } else if (t == "mask") {
                r.type = PaintRule::Type::Mask;
                r.texture = std::string(rj["texture"].asString());
                readF(rj, "pinBelow", r.pinBelow);
            } else return fail("unknown paint rule type");
            a.paint.rules.push_back(std::move(r));
        }
    }
    std::string why2;
    if (!a.valid(&why2)) return fail(why2);
    out = std::move(a);
    return true;
}

bool loadSoftAsset(const std::string& path, SoftAsset& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) {
        if (why) *why = "cannot read " + path;
        return false;
    }
    return parseSoftAsset(text, out, why);
}

std::string writeSoftAsset(const SoftAsset& a) {
    const Material& m = a.material;
    std::string s;
    s += "{\n";
    s += "  \"format\": \"ocsoft\",\n";
    s += "  \"version\": " + unum(kSoftAssetVersion) + ",\n";
    s += std::string("  \"kind\": \"") + (a.kind == SoftKind::Cloth ? "cloth" : "destructible") + "\",\n";
    s += std::string("  \"preset\": \"") + presetKey(a.preset) + "\",\n";
    s += "  \"material\": {\n";
    s += "    \"areaDensityKgM2\": " + num(m.areaDensityKgM2) + ",\n";
    s += "    \"compliance\": " + num(m.compliance) + ",\n";
    s += "    \"bendCompliance\": " + num(m.bendCompliance) + ",\n";
    s += "    \"yieldStrain\": " + num(m.yieldStrain) + ",\n";
    s += "    \"hardening\": " + num(m.hardening) + ",\n";
    s += "    \"maxSetCm\": " + num(m.maxBend >= 1e8f ? 0.0f : m.maxBend) + ",\n";
    s += "    \"maxSetFrac\": " + num(m.maxSetFrac) + ",\n";
    s += "    \"breakStrain\": " + num(m.breakStrain) + ",\n";
    s += "    \"toughnessJ\": " + num(m.toughnessJ) + ",\n";
    s += std::string("    \"behavior\": \"") + behaviorKey(m.behavior) + "\",\n";
    s += "    \"friction\": " + num(m.friction) + ",\n";
    s += "    \"restitution\": " + num(m.restitution) + "\n";
    s += "  },\n";
    s += "  \"sim\": {\n";
    s += "    \"iterations\": " + unum(a.sim.iterations) + ",\n";
    s += "    \"damping\": " + num(a.sim.damping) + ",\n";
    s += "    \"vertexRadiusCm\": " + num(a.sim.vertexRadiusCm) + ",\n";
    s += "    \"maxSpeedCmS\": " + num(a.sim.maxSpeedCmS) + ",\n";
    s += "    \"gravityScale\": " + num(a.sim.gravityScale) + ",\n";
    s += "    \"weldCm\": " + num(a.sim.weldCm) + ",\n";
    s += "    \"maxParticles\": " + unum(a.sim.maxParticles) + ",\n";
    s += std::string("    \"allowSleep\": ") + (a.sim.allowSleep ? "true" : "false") + "\n";
    s += "  },\n";
    s += "  \"cloth\": {\n";
    s += "    \"dragCoeff\": " + num(a.cloth.dragCoeff) + ",\n";
    s += "    \"liftCoeff\": " + num(a.cloth.liftCoeff) + ",\n";
    s += std::string("    \"tearable\": ") + (a.cloth.tearable ? "true" : "false") + ",\n";
    s += "    \"skin\": { \"defaultReachCm\": " + num(a.cloth.defaultReachCm) + ", \"backstopCm\": " + num(a.cloth.backstopCm) + " }\n";
    s += "  },\n";
    s += "  \"destruct\": {\n";
    s += "    \"minImpactCmS\": " + num(a.destruct.minImpactCmS) + ",\n";
    s += "    \"impulseEfficiency\": " + num(a.destruct.impulseEfficiency) + ",\n";
    s += "    \"maxKickCmS\": " + num(a.destruct.maxKickCmS) + ",\n";
    s += "    \"minPieceTriangles\": " + unum(a.destruct.minPieceTriangles) + ",\n";
    s += "    \"debrisLifetimeS\": " + num(a.destruct.debrisLifetimeS) + ",\n";
    s += "    \"maxPieces\": " + unum(a.destruct.maxPieces) + "\n";
    s += "  },\n";
    s += "  \"paint\": {\n";
    s += "    \"meshKey\": \"" + hex64(a.paint.meshKey) + "\",\n";
    s += "    \"pin\": \"" + encodePaintRle(a.paint.pin) + "\",\n";
    s += "    \"reach\": \"" + encodePaintRle(a.paint.reach) + "\",\n";
    s += "    \"rules\": [";
    for (usize i = 0; i < a.paint.rules.size(); ++i) {
        const PaintRule& r = a.paint.rules[i];
        s += i == 0 ? "\n      " : ",\n      ";
        switch (r.type) {
            case PaintRule::Type::Above:
                s += std::string("{ \"type\": \"above\", \"axis\": \"") + axisKey(r.axis) + "\", \"valueCm\": " + num(r.valueCm) + " }";
                break;
            case PaintRule::Type::Bone:
                s += "{ \"type\": \"bone\", \"bone\": " + esc(r.bone) + ", \"minWeight\": " + num(r.minWeight) + " }";
                break;
            case PaintRule::Type::Sphere:
                s += "{ \"type\": \"sphere\", \"centreCm\": [" + num(r.centreCm[0]) + ", " + num(r.centreCm[1]) + ", " +
                     num(r.centreCm[2]) + "], \"radiusCm\": " + num(r.radiusCm) + " }";
                break;
            case PaintRule::Type::Mask:
                s += "{ \"type\": \"mask\", \"texture\": " + esc(r.texture) + ", \"pinBelow\": " + num(r.pinBelow) + " }";
                break;
        }
    }
    s += a.paint.rules.empty() ? "]\n" : "\n    ]\n";
    s += "  }\n";
    s += "}\n";
    return s;
}

bool saveSoftAsset(const std::string& path, const SoftAsset& a, std::string* why) {
    if (!writeFileTextAtomic(path, writeSoftAsset(a))) {
        if (why) *why = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace aver::softbody
