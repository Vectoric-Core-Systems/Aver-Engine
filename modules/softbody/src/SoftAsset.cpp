// .ocsoft read/write, the preset resolution, validation and the paint run-length codec.
#include "aver/softbody/SoftAsset.hpp"

#include "aver/core/Hash.hpp"
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

const char* capModeKey(CapMode m) { return m == CapMode::Off ? "off" : (m == CapMode::On ? "on" : "auto"); }
bool capModeFromKey(std::string_view k, CapMode& out) {
    if (k == "auto") { out = CapMode::Auto; return true; }
    if (k == "off")  { out = CapMode::Off;  return true; }
    if (k == "on")   { out = CapMode::On;   return true; }
    return false;
}
const char* backSkinKey(BackSkin b) {
    switch (b) {
        case BackSkin::Off: return "off";
        case BackSkin::Debris: return "debris";
        case BackSkin::All: return "all";
        default: return "auto";
    }
}
bool backSkinFromKey(std::string_view k, BackSkin& out) {
    if (k == "auto")   { out = BackSkin::Auto;   return true; }
    if (k == "off")    { out = BackSkin::Off;    return true; }
    if (k == "debris") { out = BackSkin::Debris; return true; }
    if (k == "all")    { out = BackSkin::All;    return true; }
    return false;
}

const char* solverKey(ClothSolver s) { return s == ClothSolver::Fast ? "fast" : "cage"; }
const char* clothBendKey(ClothBend b) { return b == ClothBend::None ? "none" : (b == ClothBend::Dihedral ? "dihedral" : "distance"); }
const char* clothLraKey(ClothLra l) { return l == ClothLra::Euclidean ? "euclidean" : (l == ClothLra::Geodesic ? "geodesic" : "off"); }

const char* kindKey(SoftKind k) {
    switch (k) {
        case SoftKind::Destructible: return "destructible";
        case SoftKind::Jelly:        return "jelly";
        case SoftKind::Rope:         return "rope";
        default:                     return "cloth";
    }
}

std::string jellyJson(const JellyParams& j) {
    std::string s = "  \"jelly\": { \"cellCm\": " + num(j.cellCm) + ", \"snapCm\": " + num(j.snapCm) + ", \"volumeCompliance\": " +
                    num(j.volumeCompliance) + ", \"edgeCompliance\": " + num(j.edgeCompliance);
    if (j.vertexRadiusCm >= 0.0f) s += ", \"vertexRadiusCm\": " + num(j.vertexRadiusCm);
    s += ", \"densityKgM3\": " + num(j.densityKgM3) + ", \"maxNodes\": " + unum(j.maxNodes) + " },\n";
    return s;
}

std::string ropeJson(const RopeParams& r) {
    std::string s = "  \"rope\": { \"radiusCm\": " + num(r.radiusCm) + ", \"segmentCm\": " + num(r.segmentCm) + ", \"massPerMeterKg\": " +
                    num(r.massPerMeterKg) + ", \"slack\": " + num(r.slack) + ", \"sides\": " + unum(r.sides) + ", \"uvCmPerTile\": " +
                    num(r.uvCmPerTile) + ", \"stretchCompliance\": " + num(r.stretchCompliance) + ", \"bendTwistCompliance\": " +
                    num(r.bendTwistCompliance);
    if (!r.material.empty()) s += ", \"material\": " + esc(r.material);
    s += " },\n";
    return s;
}

bool endsWithOcMesh(const std::string& path) {
    static const char ext[] = ".ocmesh";
    const usize n = sizeof ext - 1;
    if (path.size() < n) return false;
    for (usize i = 0; i < n; ++i) {
        char c = path[path.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != ext[i]) return false;
    }
    return true;
}

bool slotAtDefault(const FxSlotParams& s) {
    const FxSlotParams d;
    return s.particle.empty() && s.sounds.empty() && s.minSpeedCmS == d.minSpeedCmS && s.count == d.count &&
           s.countPerMs == d.countPerMs && s.countPerParticle == d.countPerParticle && s.volume == d.volume &&
           s.pitchJitter == d.pitchJitter && s.cooldownS == d.cooldownS && s.innerCm == d.innerCm && s.outerCm == d.outerCm;
}

bool anySlotWritten(const EffectParams& e) {
    for (const FxSlotParams& f : e.slot)
        if (!slotAtDefault(f)) return true;
    return false;
}

std::string joinKv(const std::vector<std::string>& kv) {
    std::string out = "{";
    for (usize i = 0; i < kv.size(); ++i) out += (i ? ", " : " ") + kv[i];
    return out + (kv.empty() ? "}" : " }");
}

std::string slotJson(const FxSlotParams& s) {
    const FxSlotParams d;
    std::vector<std::string> kv;
    if (!s.particle.empty()) kv.push_back("\"particle\": " + esc(s.particle));
    if (!s.sounds.empty()) {
        std::string a = "\"sounds\": [";
        for (usize i = 0; i < s.sounds.size(); ++i) a += std::string(i ? ", " : "") + esc(s.sounds[i]);
        kv.push_back(a + "]");
    }
    const auto f = [&](const char* key, f32 v, f32 def) {
        if (v != def) kv.push_back(std::string("\"") + key + "\": " + num(v));
    };
    f("minSpeedCmS", s.minSpeedCmS, d.minSpeedCmS);
    f("count", s.count, d.count);
    f("countPerMs", s.countPerMs, d.countPerMs);
    f("countPerParticle", s.countPerParticle, d.countPerParticle);
    f("volume", s.volume, d.volume);
    f("pitchJitter", s.pitchJitter, d.pitchJitter);
    f("cooldownS", s.cooldownS, d.cooldownS);
    f("innerCm", s.innerCm, d.innerCm);
    f("outerCm", s.outerCm, d.outerCm);
    return joinKv(kv);
}

bool capsAtDefault(const CapParams& c) {
    const CapParams d;
    return c.mode == d.mode && c.thicknessCm == d.thicknessCm && c.backSkin == d.backSkin && c.uvCmPerTile == d.uvCmPerTile &&
           c.lodCm == d.lodCm && c.interiorMaterial.empty();
}

std::string capsJson(const CapParams& c) {
    const CapParams d;
    std::vector<std::string> kv;
    if (c.mode != d.mode) kv.push_back(std::string("\"mode\": \"") + capModeKey(c.mode) + "\"");
    if (c.thicknessCm != d.thicknessCm) kv.push_back("\"thicknessCm\": " + num(c.thicknessCm));
    if (c.backSkin != d.backSkin) kv.push_back(std::string("\"backSkin\": \"") + backSkinKey(c.backSkin) + "\"");
    if (c.uvCmPerTile != d.uvCmPerTile) kv.push_back("\"uvCmPerTile\": " + num(c.uvCmPerTile));
    if (c.lodCm != d.lodCm) kv.push_back("\"lodCm\": " + num(c.lodCm));
    if (!c.interiorMaterial.empty()) kv.push_back("\"interiorMaterial\": " + esc(c.interiorMaterial));
    return joinKv(kv);
}

void readSlot(const JsonValue& o, FxSlotParams& s) {
    if (o["particle"].isString()) s.particle = std::string(o["particle"].asString());
    const JsonValue& snd = o["sounds"];
    if (snd.isArray())
        for (usize i = 0; i < snd.size() && i < 64; ++i)
            if (snd[i].isString()) s.sounds.emplace_back(snd[i].asString());
    readF(o, "minSpeedCmS", s.minSpeedCmS);
    readF(o, "count", s.count);
    readF(o, "countPerMs", s.countPerMs);
    readF(o, "countPerParticle", s.countPerParticle);
    readF(o, "volume", s.volume);
    readF(o, "pitchJitter", s.pitchJitter);
    readF(o, "cooldownS", s.cooldownS);
    readF(o, "innerCm", s.innerCm);
    readF(o, "outerCm", s.outerCm);
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

const char* fxSlotKey(FxSlot slot) {
    switch (slot) {
        case FxSlot::Impact:  return "impact";
        case FxSlot::Tear:    return "tear";
        case FxSlot::Detach:  return "detach";
        case FxSlot::Dust:    return "dust";
        default:              return "release";
    }
}

bool capsEnabled(const SoftAsset& a) {
    if (a.caps.mode == CapMode::On) return true;
    if (a.caps.mode == CapMode::Off) return false;
    return a.kind == SoftKind::Destructible;
}

f32 capThicknessCm(const SoftAsset& a) {
    return a.caps.thicknessCm >= 0.0f ? a.caps.thicknessCm : presetInfo(a.preset).capThicknessCm;
}

u64 softAssetKey(const SoftAsset& a) {
    SoftAsset k = a;
    k.caps = CapParams{};
    k.effects = EffectParams{};
    k.destruct.debrisLifetimeS = DestructParams{}.debrisLifetimeS;
    k.destruct.impactRadiusScale = 1.0f;
    k.destruct.bake = true;
    return fnv1a64(writeSoftAsset(k));
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
        if (!finite(cloth.lraSlack) || cloth.lraSlack < 1.0f) return fail("cloth.lraSlack must be >= 1");
        if (cloth.solver == ClothSolver::Fast && cloth.tearable) return fail("cloth.solver \"fast\" cannot be tearable");
        if (!finite(inflate.pressureKPa) || inflate.pressureKPa < 0.0f) return fail("inflate.pressureKPa must be >= 0");
        if (!finite(inflate.leakKPaPerS) || inflate.leakKPaPerS < 0.0f) return fail("inflate.leakKPaPerS must be >= 0");
    } else if (kind == SoftKind::Jelly) {
        if (!finite(jelly.cellCm) || jelly.cellCm <= 0.0f) return fail("jelly.cellCm must be > 0");
        if (!finite(jelly.snapCm) || jelly.snapCm < 0.0f) return fail("jelly.snapCm must be >= 0");
        if (!finite(jelly.volumeCompliance) || jelly.volumeCompliance < 0.0f) return fail("jelly.volumeCompliance must be >= 0");
        if (!finite(jelly.edgeCompliance) || jelly.edgeCompliance < 0.0f) return fail("jelly.edgeCompliance must be >= 0");
        if (!finite(jelly.vertexRadiusCm)) return fail("jelly.vertexRadiusCm must be finite");
        if (!finite(jelly.densityKgM3) || jelly.densityKgM3 <= 0.0f) return fail("jelly.densityKgM3 must be > 0");
        if (jelly.maxNodes < 4 || jelly.maxNodes > 65536) return fail("jelly.maxNodes must be 4..65536");
    } else if (kind == SoftKind::Rope) {
        if (!finite(rope.radiusCm) || rope.radiusCm <= 0.0f) return fail("rope.radiusCm must be > 0");
        if (!finite(rope.segmentCm) || rope.segmentCm <= 0.0f) return fail("rope.segmentCm must be > 0");
        if (!finite(rope.massPerMeterKg) || rope.massPerMeterKg <= 0.0f) return fail("rope.massPerMeterKg must be > 0");
        if (!finite(rope.slack) || rope.slack < 1.0f) return fail("rope.slack must be >= 1");
        if (rope.sides < 3 || rope.sides > 32) return fail("rope.sides must be 3..32");
        if (!finite(rope.uvCmPerTile) || rope.uvCmPerTile <= 0.0f) return fail("rope.uvCmPerTile must be > 0");
        if (!finite(rope.stretchCompliance) || rope.stretchCompliance < 0.0f) return fail("rope.stretchCompliance must be >= 0");
        if (!finite(rope.bendTwistCompliance) || rope.bendTwistCompliance < 0.0f) return fail("rope.bendTwistCompliance must be >= 0");
    } else {
        if (!finite(destruct.minImpactCmS) || destruct.minImpactCmS < 0.0f) return fail("destruct.minImpactCmS must be >= 0");
        if (!finite(destruct.impulseEfficiency) || destruct.impulseEfficiency <= 0.0f || destruct.impulseEfficiency > 1.0f)
            return fail("destruct.impulseEfficiency must be in (0, 1]");
        if (!finite(destruct.maxKickCmS) || destruct.maxKickCmS < 0.0f) return fail("destruct.maxKickCmS must be >= 0");
        if (destruct.minPieceTriangles < 1) return fail("destruct.minPieceTriangles must be >= 1");
        if (!finite(destruct.debrisLifetimeS) || destruct.debrisLifetimeS < 0.0f) return fail("destruct.debrisLifetimeS must be >= 0");
        if (destruct.maxPieces < 1) return fail("destruct.maxPieces must be >= 1");
        if (!finite(destruct.impactRadiusScale) || destruct.impactRadiusScale <= 0.0f) return fail("destruct.impactRadiusScale must be > 0");
    }
    if (!finite(caps.thicknessCm) || (caps.thicknessCm < 0.0f && caps.thicknessCm != -1.0f)) return fail("caps.thicknessCm must be >= 0");
    if (!finite(caps.uvCmPerTile) || caps.uvCmPerTile <= 0.0f) return fail("caps.uvCmPerTile must be > 0");
    if (!finite(caps.lodCm) || caps.lodCm < 0.0f) return fail("caps.lodCm must be >= 0");
    for (const FxSlotParams& f : effects.slot) {
        if (f.sounds.size() > kMaxFxSounds) return fail("an effects slot takes at most 8 sounds");
        if (!finite(f.minSpeedCmS) || f.minSpeedCmS < 0.0f) return fail("effects minSpeedCmS must be >= 0");
        if (!finite(f.count) || f.count < 0.0f || !finite(f.countPerMs) || f.countPerMs < 0.0f ||
            !finite(f.countPerParticle) || f.countPerParticle < 0.0f) return fail("effects counts must be >= 0");
        if (!finite(f.volume) || f.volume < 0.0f) return fail("effects volume must be >= 0");
        if (!finite(f.pitchJitter) || f.pitchJitter < 0.0f || f.pitchJitter > 1.0f) return fail("effects pitchJitter must be 0..1");
        if (!finite(f.cooldownS) || f.cooldownS < 0.0f) return fail("effects cooldownS must be >= 0");
        if (!finite(f.innerCm) || f.innerCm < 0.0f || !finite(f.outerCm) || f.outerCm < f.innerCm) return fail("effects innerCm and outerCm must satisfy 0 <= inner <= outer");
    }
    if (!proxy.mesh.empty() && !endsWithOcMesh(proxy.mesh)) return fail("proxy.mesh must be an .ocmesh");
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
    else if (kindKey == "jelly") kind = SoftKind::Jelly;
    else if (kindKey == "rope") kind = SoftKind::Rope;
    else return fail("kind must be \"cloth\", \"destructible\", \"jelly\" or \"rope\"");

    SoftPreset preset = kind == SoftKind::Cloth ? SoftPreset::ClothLight : SoftPreset::Metal;
    if (kind == SoftKind::Jelly) preset = SoftPreset::Jelly;
    else if (kind == SoftKind::Rope) preset = SoftPreset::Rope;
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
        if (cl.has("solver")) {
            const std::string_view v = cl["solver"].asString();
            if (v == "cage") a.cloth.solver = ClothSolver::Cage;
            else if (v == "fast") a.cloth.solver = ClothSolver::Fast;
            else return fail("cloth.solver must be cage or fast");
        }
        if (cl.has("bend")) {
            const std::string_view v = cl["bend"].asString();
            if (v == "none") a.cloth.bend = ClothBend::None;
            else if (v == "distance") a.cloth.bend = ClothBend::Distance;
            else if (v == "dihedral") a.cloth.bend = ClothBend::Dihedral;
            else return fail("cloth.bend must be none, distance or dihedral");
        }
        if (cl.has("lra")) {
            const std::string_view v = cl["lra"].asString();
            if (v == "off") a.cloth.lra = ClothLra::Off;
            else if (v == "euclidean") a.cloth.lra = ClothLra::Euclidean;
            else if (v == "geodesic") a.cloth.lra = ClothLra::Geodesic;
            else return fail("cloth.lra must be off, euclidean or geodesic");
        }
        readF(cl, "lraSlack", a.cloth.lraSlack);
    }
    const JsonValue& inf = root["inflate"];
    if (inf.isObject()) {
        readF(inf, "pressureKPa", a.inflate.pressureKPa);
        readF(inf, "leakKPaPerS", a.inflate.leakKPaPerS);
    }
    const JsonValue& grp = root["group"];
    if (grp.isObject() && grp.has("ragdollOf")) {
        if (grp["ragdollOf"].asString() != "owner") return fail("group.ragdollOf must be \"owner\"");
        a.group.ragdollOwner = true;
    }
    const JsonValue& jl = root["jelly"];
    if (jl.isObject()) {
        readF(jl, "cellCm", a.jelly.cellCm);
        readF(jl, "snapCm", a.jelly.snapCm);
        readF(jl, "volumeCompliance", a.jelly.volumeCompliance);
        readF(jl, "edgeCompliance", a.jelly.edgeCompliance);
        readF(jl, "vertexRadiusCm", a.jelly.vertexRadiusCm);
        readF(jl, "densityKgM3", a.jelly.densityKgM3);
        readU(jl, "maxNodes", a.jelly.maxNodes);
    }
    const JsonValue& rp = root["rope"];
    if (rp.isObject()) {
        readF(rp, "radiusCm", a.rope.radiusCm);
        readF(rp, "segmentCm", a.rope.segmentCm);
        readF(rp, "massPerMeterKg", a.rope.massPerMeterKg);
        readF(rp, "slack", a.rope.slack);
        readU(rp, "sides", a.rope.sides);
        readF(rp, "uvCmPerTile", a.rope.uvCmPerTile);
        readF(rp, "stretchCompliance", a.rope.stretchCompliance);
        readF(rp, "bendTwistCompliance", a.rope.bendTwistCompliance);
        if (rp["material"].isString()) a.rope.material = std::string(rp["material"].asString());
    }
    const JsonValue& de = root["destruct"];
    if (de.isObject()) {
        readF(de, "minImpactCmS", a.destruct.minImpactCmS);
        readF(de, "impulseEfficiency", a.destruct.impulseEfficiency);
        readF(de, "maxKickCmS", a.destruct.maxKickCmS);
        readU(de, "minPieceTriangles", a.destruct.minPieceTriangles);
        readF(de, "debrisLifetimeS", a.destruct.debrisLifetimeS);
        readU(de, "maxPieces", a.destruct.maxPieces);
        readF(de, "impactRadiusScale", a.destruct.impactRadiusScale);
        readB(de, "bake", a.destruct.bake);
    }
    const JsonValue& cp = root["caps"];
    if (cp.isObject()) {
        if (cp.has("mode") && !capModeFromKey(cp["mode"].asString(), a.caps.mode)) return fail("caps.mode must be auto, off or on");
        if (cp.has("backSkin") && !backSkinFromKey(cp["backSkin"].asString(), a.caps.backSkin)) return fail("caps.backSkin must be auto, off, debris or all");
        if (cp["thicknessCm"].isNumber()) {
            if (cp["thicknessCm"].asFloat() < 0.0f) return fail("caps.thicknessCm must be >= 0");
            a.caps.thicknessCm = cp["thicknessCm"].asFloat();
        }
        readF(cp, "uvCmPerTile", a.caps.uvCmPerTile);
        readF(cp, "lodCm", a.caps.lodCm);
        if (cp["interiorMaterial"].isString()) a.caps.interiorMaterial = std::string(cp["interiorMaterial"].asString());
    }
    const JsonValue& fx = root["effects"];
    if (fx.isObject())
        for (u32 i = 0; i < kFxSlotCount; ++i) {
            const JsonValue& slot = fx[fxSlotKey(static_cast<FxSlot>(i))];
            if (slot.isObject()) readSlot(slot, a.effects.slot[i]);
        }
    const JsonValue& px = root["proxy"];
    if (px.isObject()) {
        if (px["mesh"].isString()) a.proxy.mesh = std::string(px["mesh"].asString());
        readU(px, "maxParticles", a.proxy.maxParticles);
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
    s += std::string("  \"kind\": \"") + kindKey(a.kind) + "\",\n";
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
    s += "    \"skin\": { \"defaultReachCm\": " + num(a.cloth.defaultReachCm) + ", \"backstopCm\": " + num(a.cloth.backstopCm) + " }";
    {
        const ClothParams d;   // wave 11 keys, written only when they differ so an older asset writes (and hashes) as it always did
        if (a.cloth.solver != d.solver) s += std::string(",\n    \"solver\": \"") + solverKey(a.cloth.solver) + "\"";
        if (a.cloth.bend != d.bend) s += std::string(",\n    \"bend\": \"") + clothBendKey(a.cloth.bend) + "\"";
        if (a.cloth.lra != d.lra) s += std::string(",\n    \"lra\": \"") + clothLraKey(a.cloth.lra) + "\"";
        if (a.cloth.lraSlack != d.lraSlack) s += ",\n    \"lraSlack\": " + num(a.cloth.lraSlack);
    }
    s += "\n  },\n";
    s += "  \"destruct\": {\n";
    s += "    \"minImpactCmS\": " + num(a.destruct.minImpactCmS) + ",\n";
    s += "    \"impulseEfficiency\": " + num(a.destruct.impulseEfficiency) + ",\n";
    s += "    \"maxKickCmS\": " + num(a.destruct.maxKickCmS) + ",\n";
    s += "    \"minPieceTriangles\": " + unum(a.destruct.minPieceTriangles) + ",\n";
    s += "    \"debrisLifetimeS\": " + num(a.destruct.debrisLifetimeS) + ",\n";
    s += "    \"maxPieces\": " + unum(a.destruct.maxPieces);
    if (a.destruct.impactRadiusScale != DestructParams{}.impactRadiusScale) s += ",\n    \"impactRadiusScale\": " + num(a.destruct.impactRadiusScale);
    if (!a.destruct.bake) s += ",\n    \"bake\": false";
    s += "\n  },\n";
    if (!capsAtDefault(a.caps)) s += "  \"caps\": " + capsJson(a.caps) + ",\n";
    if (anySlotWritten(a.effects)) {
        s += "  \"effects\": {";
        bool first = true;
        for (u32 i = 0; i < kFxSlotCount; ++i) {
            if (slotAtDefault(a.effects.slot[i])) continue;
            s += std::string(first ? "\n    \"" : ",\n    \"") + fxSlotKey(static_cast<FxSlot>(i)) + "\": " + slotJson(a.effects.slot[i]);
            first = false;
        }
        s += "\n  },\n";
    }
    if (!a.proxy.mesh.empty() || a.proxy.maxParticles != 0) {
        s += "  \"proxy\": { \"mesh\": " + esc(a.proxy.mesh);
        if (a.proxy.maxParticles != 0) s += ", \"maxParticles\": " + unum(a.proxy.maxParticles);
        s += " },\n";
    }
    if (a.inflate.pressureKPa != 0.0f || a.inflate.leakKPaPerS != 0.0f)
        s += "  \"inflate\": { \"pressureKPa\": " + num(a.inflate.pressureKPa) + ", \"leakKPaPerS\": " + num(a.inflate.leakKPaPerS) + " },\n";
    if (a.group.ragdollOwner) s += "  \"group\": { \"ragdollOf\": \"owner\" },\n";
    if (a.kind == SoftKind::Jelly) s += jellyJson(a.jelly);
    if (a.kind == SoftKind::Rope) s += ropeJson(a.rope);
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
