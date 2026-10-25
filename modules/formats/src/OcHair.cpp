// Reader and writer for .ochair. See OcHair.hpp for the format. The HEAD chunk is hand-editable text; the strands and the embedded scalp are
// little-endian binary chunks. Writing a parsed groom again gives the same bytes: the text is rebuilt from the fields, every number in its
// shortest form that reads back as the same f32, and a comment in a hand-edited file does not survive (the Groom tab is the editor).
#include "aver/formats/OcHair.hpp"

#include "aver/core/CharConv.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool fail(std::string* why, std::string m) {
    if (why) *why = std::move(m);
    return false;
}

// ---- numbers ----------------------------------------------------------------------------------------------------------------------

// The whole token or nothing: "0.6abc" is not 0.6.
bool parseWhole(std::string_view s, f32& out) {
    if (s.empty()) return false;
    const char* const end = s.data() + s.size();
    const auto r = aver::fromChars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parseWholeU32(std::string_view s, u32& out) {
    if (s.empty()) return false;
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parseWholeI32(std::string_view s, i32& out) {
    if (s.empty()) return false;
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

// The shortest text that reads back as the SAME f32.
void appendNum(std::string& s, f32 v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    s.append(buf, r.ptr);
}

bool finite(f32 v) { return std::isfinite(v); }
bool finite3(const f32* v) { return finite(v[0]) && finite(v[1]) && finite(v[2]); }

// ---- the binary writers and readers ---------------------------------------------------------------------------------------

struct W {
    std::vector<u8>& b;
    void u8v(u8 v) { b.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

struct R {
    const u8* p;
    const u8* e;
    bool ok = true;
    bool need(u64 n) { if (static_cast<u64>(e - p) < n) { ok = false; return false; } return true; }
    u8 u8v() { if (!need(1)) return 0; return *p++; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    f32 f32v() { const u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
    usize left() const { return static_cast<usize>(e - p); }
};

// ---- HEAD: tokens and key runs -----------------------------------------------------------------------------------------------

// One key and what followed it: numbers, or one word for the keys that take a word.
struct KeyRun {
    std::string_view key;
    std::vector<f32> v;
    std::string_view word;
    bool hasWord = false;
};

// Splits tokens [from, end) into key runs. A key in `wordKeys` takes the next token whole; any other key takes every following token that is a
// number (none is a legal run: `collision` alone is rejected by the record's own count check).
bool keyRuns(const std::vector<std::string_view>& t, usize from, std::initializer_list<const char*> wordKeys, std::vector<KeyRun>& out, std::string& err) {
    for (usize i = from; i < t.size();) {
        KeyRun run;
        run.key = t[i++];
        f32 probe = 0.0f;
        if (parseWhole(run.key, probe)) { err = "'" + std::string(run.key) + "' is a number where a key was expected"; return false; }
        bool isWord = false;
        for (const char* k : wordKeys)
            if (equalsCI(run.key, k)) isWord = true;
        if (isWord) {
            if (i >= t.size()) { err = "'" + std::string(run.key) + "' has no value"; return false; }
            run.word = t[i++];
            run.hasWord = true;
        } else {
            while (i < t.size()) {
                f32 v = 0.0f;
                if (!parseWhole(t[i], v)) break;
                run.v.push_back(v);
                ++i;
            }
        }
        out.push_back(std::move(run));
    }
    return true;
}

struct Parser {
    OcHairData& d;
    std::string err;
    bool warned = false;
    std::unordered_set<u32> materialSeen, lookSeen;

    void unknown(const std::string& what) {
        if (warned) return;
        warned = true;
        AVER_WARN("[ochair] {} skipped (a newer groom?)", what);
    }

    bool count(const KeyRun& r, usize n) {
        if (r.v.size() == n) return true;
        err = "'" + std::string(r.key) + "' takes " + std::to_string(n) + " number(s), found " + std::to_string(r.v.size());
        return false;
    }
    bool countRange(const KeyRun& r, usize lo, usize hi) {
        if (r.v.size() >= lo && r.v.size() <= hi) return true;
        err = "'" + std::string(r.key) + "' takes " + std::to_string(lo) + " to " + std::to_string(hi) + " numbers, found " + std::to_string(r.v.size());
        return false;
    }
    bool vec3(const KeyRun& r, f32* dst) {
        if (!count(r, 3)) return false;
        for (int k = 0; k < 3; ++k) dst[k] = r.v[static_cast<usize>(k)];
        return true;
    }
    bool gradient(const KeyRun& r, OcHairGradient& g) {
        if (!countRange(r, 2, 4)) return false;
        g.min = r.v[0];
        g.max = r.v[1];
        if (r.v.size() >= 3) g.minFraction = r.v[2];
        if (r.v.size() >= 4) g.maxFraction = r.v[3];
        else if (r.v.size() == 3) { err = "'" + std::string(r.key) + "' takes 2 or 4 numbers, found 3"; return false; }
        return true;
    }
    bool toU32(f32 v, u32& out, const KeyRun& r) {
        if (!(v >= 0.0f) || v > 4294967040.0f || v != std::floor(v)) { err = "'" + std::string(r.key) + "' needs a whole number"; return false; }
        out = static_cast<u32>(v);
        return true;
    }

    bool sim(const std::vector<KeyRun>& runs) {
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "iterationsPerSecond")) { if (!count(r, 1)) return false; d.sim.iterationsPerSecond = r.v[0]; }
            else if (equalsCI(r.key, "maxDeltaTime")) { if (!count(r, 1)) return false; d.sim.maxDeltaTime = r.v[0]; }
            else if (equalsCI(r.key, "grid")) { if (!count(r, 3)) return false; for (int k = 0; k < 3; ++k) d.sim.grid[k] = r.v[static_cast<usize>(k)]; }
            else if (equalsCI(r.key, "boundsPaddingCm")) { if (!count(r, 1)) return false; d.sim.boundsPaddingCm = r.v[0]; }
            else if (equalsCI(r.key, "simFraction")) { if (!count(r, 1)) return false; d.sim.simFraction = r.v[0]; }
            else if (equalsCI(r.key, "resample")) { if (!count(r, 1) || !toU32(r.v[0], d.sim.resample, r)) return false; }
            else if (equalsCI(r.key, "initialGravity")) { if (!count(r, 3)) return false; for (int k = 0; k < 3; ++k) d.sim.initialGravity[k] = r.v[static_cast<usize>(k)]; }
            else unknown("SIM key '" + std::string(r.key) + "'");
        }
        return true;
    }

    bool material(OcHairMaterial& m, const std::vector<KeyRun>& runs) {
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "collision")) { if (!count(r, 1)) return false; m.collision = r.v[0] != 0.0f; }
            else if (equalsCI(r.key, "lra")) { if (!count(r, 1)) return false; m.lra = r.v[0] != 0.0f; }
            else if (equalsCI(r.key, "linearDamping")) { if (!count(r, 1)) return false; m.linearDamping = r.v[0]; }
            else if (equalsCI(r.key, "angularDamping")) { if (!count(r, 1)) return false; m.angularDamping = r.v[0]; }
            else if (equalsCI(r.key, "maxLinearCmS")) { if (!count(r, 1)) return false; m.maxLinearCmS = r.v[0]; }
            else if (equalsCI(r.key, "maxAngularRadS")) { if (!count(r, 1)) return false; m.maxAngularRadS = r.v[0]; }
            else if (equalsCI(r.key, "gravity")) { if (!gradient(r, m.gravity)) return false; }
            else if (equalsCI(r.key, "friction")) { if (!count(r, 1)) return false; m.friction = r.v[0]; }
            else if (equalsCI(r.key, "bendCompliance")) { if (!count(r, 1)) return false; m.bendCompliance = r.v[0]; }
            else if (equalsCI(r.key, "bendMul")) { if (!count(r, 4)) return false; for (int k = 0; k < 4; ++k) m.bendMul[k] = r.v[static_cast<usize>(k)]; }
            else if (equalsCI(r.key, "stretchCompliance")) { if (!count(r, 1)) return false; m.stretchCompliance = r.v[0]; }
            else if (equalsCI(r.key, "inertiaMul")) { if (!count(r, 1)) return false; m.inertiaMul = r.v[0]; }
            else if (equalsCI(r.key, "radiusCm")) { if (!gradient(r, m.radiusCm)) return false; }
            else if (equalsCI(r.key, "worldInfluence")) { if (!gradient(r, m.worldInfluence)) return false; }
            else if (equalsCI(r.key, "gridVelocity")) { if (!gradient(r, m.gridVelocity)) return false; }
            else if (equalsCI(r.key, "gridDensityForce")) { if (!count(r, 1)) return false; m.gridDensityForce = r.v[0]; }
            else if (equalsCI(r.key, "globalPose")) { if (!gradient(r, m.globalPose)) return false; }
            else if (equalsCI(r.key, "skinGlobalPose")) { if (!gradient(r, m.skinGlobalPose)) return false; }
            else if (equalsCI(r.key, "gravityPreload")) { if (!count(r, 1)) return false; m.gravityPreload = r.v[0]; }
            else if (equalsCI(r.key, "simFraction")) { if (!count(r, 1)) return false; m.simFraction = r.v[0]; }
            else unknown("MATERIAL key '" + std::string(r.key) + "'");
        }
        return true;
    }

    bool look(OcHairLook& l, const std::vector<KeyRun>& runs) {
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "baseColor")) { if (!vec3(r, l.baseColor)) return false; }
            else if (equalsCI(r.key, "tipColor")) { if (!vec3(r, l.tipColor)) return false; }
            else if (equalsCI(r.key, "colorVariation")) { if (!count(r, 1)) return false; l.colorVariation = r.v[0]; }
            else if (equalsCI(r.key, "widthRootCm")) { if (!count(r, 1)) return false; l.widthRootCm = r.v[0]; }
            else if (equalsCI(r.key, "widthTipCm")) { if (!count(r, 1)) return false; l.widthTipCm = r.v[0]; }
            else if (equalsCI(r.key, "roughness")) { if (!count(r, 1)) return false; l.roughness = r.v[0]; }
            else if (equalsCI(r.key, "shift")) { if (!count(r, 1)) return false; l.shift = r.v[0]; }
            else if (equalsCI(r.key, "tt")) { if (!count(r, 1)) return false; l.tt = r.v[0]; }
            else if (equalsCI(r.key, "trt")) { if (!count(r, 1)) return false; l.trt = r.v[0]; }
            else if (equalsCI(r.key, "azimuthRough")) { if (!count(r, 1)) return false; l.azimuthRough = r.v[0]; }
            else if (equalsCI(r.key, "tint")) { if (!vec3(r, l.tint)) return false; }
            else if (equalsCI(r.key, "scatter")) { if (!count(r, 1)) return false; l.scatter = r.v[0]; }
            else unknown("LOOK key '" + std::string(r.key) + "'");
        }
        return true;
    }

    bool generate(const std::vector<KeyRun>& runs) {
        OcHairGenerate& g = d.generate;
        g.present = true;
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "seed")) { if (!count(r, 1) || !toU32(r.v[0], g.seed, r)) return false; }
            else if (equalsCI(r.key, "strands")) { if (!count(r, 1) || !toU32(r.v[0], g.strands, r)) return false; }
            else if (equalsCI(r.key, "lengthCm")) { if (!count(r, 2)) return false; g.lengthCm[0] = r.v[0]; g.lengthCm[1] = r.v[1]; }
            else if (equalsCI(r.key, "segments")) { if (!count(r, 1) || !toU32(r.v[0], g.segments, r)) return false; }
            else if (equalsCI(r.key, "curlCm")) { if (!count(r, 1)) return false; g.curlCm = r.v[0]; }
            else if (equalsCI(r.key, "curlFrequency")) { if (!count(r, 1)) return false; g.curlFrequency = r.v[0]; }
            else if (equalsCI(r.key, "clumps")) { if (!count(r, 1) || !toU32(r.v[0], g.clumps, r)) return false; }
            else if (equalsCI(r.key, "clumpRadiusCm")) { if (!count(r, 1)) return false; g.clumpRadiusCm = r.v[0]; }
            else if (equalsCI(r.key, "clumpTightness")) { if (!count(r, 1)) return false; g.clumpTightness = r.v[0]; }
            else if (equalsCI(r.key, "partDirection")) { if (!vec3(r, g.partDirection)) return false; }
            else if (equalsCI(r.key, "partWidthCm")) { if (!count(r, 1)) return false; g.partWidthCm = r.v[0]; }
            else if (equalsCI(r.key, "style")) { if (!vec3(r, g.style)) return false; }
            else if (equalsCI(r.key, "styleWeight")) { if (!count(r, 1)) return false; g.styleWeight = r.v[0]; }
            else if (equalsCI(r.key, "mask")) { g.mask = std::string(r.word); }
            else if (equalsCI(r.key, "material")) { if (!count(r, 1) || !toU32(r.v[0], g.material, r)) return false; }
            else unknown("GENERATE key '" + std::string(r.key) + "'");
        }
        return true;
    }

    bool lod(const std::vector<KeyRun>& runs) {
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "distances")) { if (!count(r, 3)) return false; for (int k = 0; k < 3; ++k) d.lod.distances[k] = r.v[static_cast<usize>(k)]; }
            else if (equalsCI(r.key, "strandFractions")) { if (!count(r, 4)) return false; for (int k = 0; k < 4; ++k) d.lod.strandFractions[k] = r.v[static_cast<usize>(k)]; }
            else unknown("LOD key '" + std::string(r.key) + "'");
        }
        return true;
    }

    bool proxy(OcHairProxy& p, const std::vector<KeyRun>& runs) {
        for (const KeyRun& r : runs) {
            if (equalsCI(r.key, "bone")) p.bone = std::string(r.word);
            else if (equalsCI(r.key, "shape")) {
                if (equalsCI(r.word, "sphere")) p.shape = OcHairProxyShape::Sphere;
                else if (equalsCI(r.word, "capsule")) p.shape = OcHairProxyShape::Capsule;
                else if (equalsCI(r.word, "box")) p.shape = OcHairProxyShape::Box;
                else { err = "PROXY shape must be sphere, capsule or box, found '" + std::string(r.word) + "'"; return false; }
            }
            else if (equalsCI(r.key, "size")) {
                if (!countRange(r, 1, 3)) return false;
                for (usize k = 0; k < 3; ++k) p.sizeCm[k] = k < r.v.size() ? r.v[k] : 0.0f;
            }
            else if (equalsCI(r.key, "offset")) { if (!count(r, 3)) return false; for (int k = 0; k < 3; ++k) p.offsetCm[k] = r.v[static_cast<usize>(k)]; }
            else unknown("PROXY key '" + std::string(r.key) + "'");
        }
        return true;
    }
};

// Lines of the HEAD text with continuation lines (a leading white space) joined to the record before them; comments and blank lines dropped.
struct Record {
    std::string text;
    int line = 0;
};

std::vector<Record> splitRecords(std::string_view text) {
    std::vector<Record> out;
    int lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view raw = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        raw = truncateHash(raw);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        if (trim(raw).empty()) continue;
        const bool continuation = isSpace(raw.front());
        if (continuation && !out.empty()) {
            out.back().text += ' ';
            out.back().text += std::string(trim(raw));
        } else {
            out.push_back({std::string(trim(raw)), lineNo});
        }
    }
    return out;
}

} // namespace

// ---- valid() ------------------------------------------------------------------------------------------------------------------------

const OcHairMaterial* OcHairData::findMaterial(u32 index) const {
    for (const OcHairMaterial& m : materials)
        if (m.index == index) return &m;
    return nullptr;
}

const OcHairLook* OcHairData::findLook(u32 material) const {
    for (const OcHairLook& l : looks)
        if (l.material == material) return &l;
    return nullptr;
}

namespace {

bool gradientOk(const OcHairGradient& g) {
    return finite(g.min) && finite(g.max) && finite(g.minFraction) && finite(g.maxFraction) && g.minFraction != g.maxFraction;
}

bool endsWithCI(const std::string& s, const char* suffix) {
    const usize n = std::strlen(suffix);
    return s.size() >= n && equalsCI(std::string_view(s).substr(s.size() - n), suffix);
}

} // namespace

bool OcHairData::valid(std::string* why) const {
    auto bad = [&](const std::string& m) { return fail(why, ".ochair: " + m); };

    // SIM
    if (!(sim.iterationsPerSecond >= 1.0f && sim.iterationsPerSecond <= 4096.0f)) return bad("SIM iterationsPerSecond must be 1 to 4096");
    if (!(sim.maxDeltaTime >= 1.0e-4f && sim.maxDeltaTime <= 1.0f)) return bad("SIM maxDeltaTime must be 0.0001 to 1 seconds");
    for (int k = 0; k < 3; ++k)
        if (!(sim.grid[k] >= 4.0f && sim.grid[k] <= 128.0f)) return bad("SIM grid cells must be 4 to 128 on each axis");
    if (!(sim.boundsPaddingCm >= 0.0f && finite(sim.boundsPaddingCm))) return bad("SIM boundsPaddingCm must be zero or more");
    if (!(sim.simFraction > 0.0f && sim.simFraction <= 1.0f)) return bad("SIM simFraction must be in (0, 1]");
    if (sim.resample != 0 && (sim.resample < kOcHairMinStrandVertices || sim.resample > kOcHairMaxStrandVertices))
        return bad("SIM resample must be 0 or 3 to 255");
    if (!finite3(sim.initialGravity)) return bad("SIM initialGravity is not finite");

    // MATERIAL
    {
        std::unordered_set<u32> seen;
        for (const OcHairMaterial& m : materials) {
            if (m.index > kOcHairMaxMaterials) return bad("MATERIAL index above " + std::to_string(kOcHairMaxMaterials));
            if (!seen.insert(m.index).second) return bad("MATERIAL " + std::to_string(m.index) + " is declared twice");
            const f32 nums[] = {m.linearDamping, m.angularDamping, m.maxLinearCmS, m.maxAngularRadS, m.friction, m.bendCompliance,
                                m.bendMul[0], m.bendMul[1], m.bendMul[2], m.bendMul[3], m.stretchCompliance, m.inertiaMul, m.gridDensityForce,
                                m.gravityPreload, m.simFraction};
            for (f32 v : nums)
                if (!finite(v)) return bad("MATERIAL " + std::to_string(m.index) + " has a non-finite number");
            if (m.linearDamping < 0.0f || m.angularDamping < 0.0f || !(m.maxLinearCmS > 0.0f) || !(m.maxAngularRadS > 0.0f) || m.friction < 0.0f ||
                m.bendCompliance < 0.0f || m.stretchCompliance < 0.0f || !(m.inertiaMul > 0.0f) || m.gridDensityForce < 0.0f || m.gravityPreload < 0.0f)
                return bad("MATERIAL " + std::to_string(m.index) + " has a value out of range (damping, friction, compliance and preload cannot be negative; "
                           "speed caps and inertiaMul must be positive)");
            for (f32 v : m.bendMul)
                if (v < 0.0f) return bad("MATERIAL " + std::to_string(m.index) + " bendMul cannot be negative");
            if (m.simFraction >= 0.0f && !(m.simFraction > 0.0f && m.simFraction <= 1.0f))
                return bad("MATERIAL " + std::to_string(m.index) + " simFraction must be in (0, 1]");
            for (const OcHairGradient* g : {&m.gravity, &m.radiusCm, &m.worldInfluence, &m.gridVelocity, &m.globalPose, &m.skinGlobalPose})
                if (!gradientOk(*g)) return bad("MATERIAL " + std::to_string(m.index) + " has a gradient that is not finite or whose two fractions coincide");
            if (m.radiusCm.min < 0.0f || m.radiusCm.max < 0.0f) return bad("MATERIAL " + std::to_string(m.index) + " radiusCm cannot be negative");
        }
    }

    // LOOK
    {
        std::unordered_set<u32> seen;
        for (const OcHairLook& l : looks) {
            if (l.material > kOcHairMaxMaterials) return bad("LOOK material above " + std::to_string(kOcHairMaxMaterials));
            if (!seen.insert(l.material).second) return bad("LOOK " + std::to_string(l.material) + " is declared twice");
            if (!finite3(l.baseColor) || !finite3(l.tipColor) || !finite3(l.tint) || !finite(l.colorVariation) || !finite(l.widthRootCm) ||
                !finite(l.widthTipCm) || !finite(l.roughness) || !finite(l.shift) || !finite(l.tt) || !finite(l.trt) || !finite(l.azimuthRough) ||
                !finite(l.scatter))
                return bad("LOOK " + std::to_string(l.material) + " has a non-finite number");
            if (!(l.widthRootCm > 0.0f) || l.widthTipCm < 0.0f) return bad("LOOK " + std::to_string(l.material) + " widths must be positive at the root and not negative at the tip");
            if (l.baseColor[0] < 0 || l.baseColor[1] < 0 || l.baseColor[2] < 0 || l.tipColor[0] < 0 || l.tipColor[1] < 0 || l.tipColor[2] < 0)
                return bad("LOOK " + std::to_string(l.material) + " colours cannot be negative");
        }
    }

    // GENERATE
    if (generate.present) {
        const OcHairGenerate& g = generate;
        if (g.strands == 0 || g.strands > maxRenderStrands) return bad("GENERATE strands must be 1 to MAXSTRANDS (" + std::to_string(maxRenderStrands) + ")");
        if (!finite(g.lengthCm[0]) || !finite(g.lengthCm[1]) || !(g.lengthCm[0] > 0.0f) || g.lengthCm[1] < g.lengthCm[0]) return bad("GENERATE lengthCm must be 0 < min <= max");
        if (g.segments < 2 || g.segments + 1 > kOcHairMaxStrandVertices) return bad("GENERATE segments must be 2 to 254");
        if (!finite(g.curlCm) || g.curlCm < 0.0f || !finite(g.curlFrequency) || g.curlFrequency < 0.0f) return bad("GENERATE curl values cannot be negative");
        if (!finite(g.clumpRadiusCm) || g.clumpRadiusCm < 0.0f || !finite(g.clumpTightness) || g.clumpTightness < 0.0f || g.clumpTightness > 1.0f)
            return bad("GENERATE clumpRadiusCm must be zero or more and clumpTightness 0 to 1");
        if (!finite3(g.partDirection) || !finite(g.partWidthCm) || g.partWidthCm < 0.0f) return bad("GENERATE part values are not usable");
        if (!finite3(g.style) || !finite(g.styleWeight) || g.styleWeight < 0.0f || g.styleWeight > 1.0f) return bad("GENERATE style values are not usable (styleWeight 0 to 1)");
        if (g.styleWeight > 0.0f && g.style[0] == 0.0f && g.style[1] == 0.0f && g.style[2] == 0.0f) return bad("GENERATE style cannot be the zero vector while styleWeight is above zero");
        if (g.material > kOcHairMaxMaterials) return bad("GENERATE material above " + std::to_string(kOcHairMaxMaterials));
    }

    // LOD
    {
        f32 last = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (!finite(lod.distances[k]) || !(lod.distances[k] > last)) return bad("LOD distances must be positive and increasing");
            last = lod.distances[k];
        }
        f32 prev = 1.0f;
        for (int k = 0; k < 4; ++k) {
            if (!finite(lod.strandFractions[k]) || !(lod.strandFractions[k] > 0.0f) || lod.strandFractions[k] > prev) return bad("LOD strandFractions must be in (0, 1] and not increase");
            prev = lod.strandFractions[k];
        }
    }

    // PROXY
    for (const OcHairProxy& p : proxies) {
        if (p.name.empty()) return bad("a PROXY needs a name");
        if (!finite3(p.sizeCm) || !finite3(p.offsetCm)) return bad("PROXY '" + p.name + "' has a non-finite number");
        if (!(p.sizeCm[0] > 0.0f)) return bad("PROXY '" + p.name + "' needs a positive size");
        if (p.shape == OcHairProxyShape::Capsule && !(p.sizeCm[1] > 0.0f)) return bad("PROXY '" + p.name + "' is a capsule and needs a half height");
        if (p.shape == OcHairProxyShape::Box && !(p.sizeCm[1] > 0.0f && p.sizeCm[2] > 0.0f)) return bad("PROXY '" + p.name + "' is a box and needs three half extents");
    }

    // scalp
    if (scalpJoints.size() > kOcHairMaxScalpJoints) return bad("SCALPJOINTS names more than " + std::to_string(kOcHairMaxScalpJoints) + " joints");
    for (const std::string& j : scalpJoints)
        if (j.empty()) return bad("SCALPJOINTS has an empty joint name");
    if (!scalpMesh.empty() && !endsWithCI(scalpMesh, ".ocmesh")) return bad("SCALP '" + scalpMesh + "' is not an .ocmesh");
    if (scalpSubmesh < -1) return bad("SCALP SUBMESH cannot be negative");
    if (maxRenderStrands == 0) return bad("MAXSTRANDS must be at least 1");

    // STRN
    if (!strands.empty() || !strands.start.empty() || !strands.points.empty()) {
        const OcHairStrands& s = strands;
        if (s.start.size() < 2 || s.start.front() != 0) return bad("the strand table must start at 0 and name at least one strand");
        const usize n = s.strandCount();
        if (n > maxRenderStrands) return bad("more strands (" + std::to_string(n) + ") than MAXSTRANDS (" + std::to_string(maxRenderStrands) + ")");
        if (s.points.size() % 3 != 0 || s.start.back() != s.pointCount()) return bad("the strand table does not cover the points");
        if (s.material.size() != n) return bad("the strand material table has the wrong length");
        if (!s.width.empty() && s.width.size() != n) return bad("the strand width table has the wrong length");
        for (usize i = 0; i < n; ++i) {
            if (s.start[i + 1] < s.start[i]) return bad("the strand table is not increasing");
            const u32 count = s.start[i + 1] - s.start[i];
            if (count < kOcHairMinStrandVertices) return bad("strand " + std::to_string(i) + " has fewer than " + std::to_string(kOcHairMinStrandVertices) + " points");
            if (count > kOcHairMaxStrandVertices) return bad("strand " + std::to_string(i) + " has more than " + std::to_string(kOcHairMaxStrandVertices) + " points");
        }
        for (f32 v : s.points)
            if (!finite(v)) return bad("a strand point is not finite");
        for (f32 w : s.width)
            if (!finite(w) || w < 0.0f) return bad("a strand width is not usable");
    } else if (!generate.present) {
        return bad("the groom has neither strands (STRN) nor a GENERATE record");
    }

    // SCLP
    if (hasScalp) {
        const OcHairScalp& sc = scalp;
        const usize vc = sc.vertexCount();
        if (sc.positions.size() % 3 != 0 || vc < 3 || sc.triangles.size() % 3 != 0 || sc.triangleCount() < 1) return bad("the embedded scalp is empty or malformed");
        for (f32 v : sc.positions)
            if (!finite(v)) return bad("an embedded scalp position is not finite");
        for (u32 i : sc.triangles)
            if (i >= vc) return bad("an embedded scalp triangle names a missing vertex");
        if (sc.joints.size() != vc * 4 || sc.weights.size() != vc * 4) return bad("the embedded scalp skin tables have the wrong length");
        const usize jointLimit = std::max<usize>(scalpJoints.size(), 1);
        for (u8 j : sc.joints)
            if (j >= jointLimit) return bad("an embedded scalp vertex names a joint past SCALPJOINTS");
        for (f32 w : sc.weights)
            if (!finite(w) || w < 0.0f) return bad("an embedded scalp weight is not usable");
    }
    return true;
}

// ---- HEAD text writer ---------------------------------------------------------------------------------------------------------

namespace {

constexpr usize kWrapColumn = 118;

// One record under construction: groups are added whole and a new line starts (indented, so it reads as a continuation) when one would not fit.
struct RecordWriter {
    std::string& out;
    usize lineStart;
    explicit RecordWriter(std::string& o, const std::string& head) : out(o), lineStart(o.size()) { out += head; }
    void group(const std::string& g) {
        if (out.size() - lineStart + 1 + g.size() > kWrapColumn) {
            out += "\n        ";
            lineStart = out.size() - 8;
            out += g;
        } else {
            out += ' ';
            out += g;
        }
    }
    void end() { out += '\n'; }
};

std::string num(f32 v) { std::string s; appendNum(s, v); return s; }

std::string key1(const char* k, f32 v) { return std::string(k) + ' ' + num(v); }

std::string key3(const char* k, const f32* v) { return std::string(k) + ' ' + num(v[0]) + ' ' + num(v[1]) + ' ' + num(v[2]); }

std::string keyN(const char* k, const f32* v, int n) {
    std::string s = k;
    for (int i = 0; i < n; ++i) { s += ' '; s += num(v[i]); }
    return s;
}

std::string keyGradient(const char* k, const OcHairGradient& g) {
    const f32 v[4] = {g.min, g.max, g.minFraction, g.maxFraction};
    return keyN(k, v, 4);
}

} // namespace

std::string ocHairHeadText(const OcHairData& d) {
    std::string out;
    out += "OCHAIR " + std::to_string(kOcHairVersion) + "\n";

    if (!d.scalpMesh.empty()) {
        RecordWriter r(out, "SCALP " + d.scalpMesh);
        if (d.scalpSubmesh >= 0) r.group("SUBMESH " + std::to_string(d.scalpSubmesh));
        r.end();
    }
    if (!d.scalpJoints.empty()) {
        RecordWriter r(out, "SCALPJOINTS");
        for (const std::string& j : d.scalpJoints) r.group(j);
        r.end();
    }
    {
        RecordWriter r(out, "SIM");
        r.group(key1("iterationsPerSecond", d.sim.iterationsPerSecond));
        r.group(key1("maxDeltaTime", d.sim.maxDeltaTime));
        r.group(keyN("grid", d.sim.grid, 3));
        r.group(key1("boundsPaddingCm", d.sim.boundsPaddingCm));
        r.group(key1("simFraction", d.sim.simFraction));
        r.group("resample " + std::to_string(d.sim.resample));
        r.group(key3("initialGravity", d.sim.initialGravity));
        r.end();
    }
    for (const OcHairMaterial& m : d.materials) {
        RecordWriter r(out, "MATERIAL " + std::to_string(m.index));
        r.group(std::string("collision ") + (m.collision ? "1" : "0"));
        r.group(std::string("lra ") + (m.lra ? "1" : "0"));
        r.group(key1("linearDamping", m.linearDamping));
        r.group(key1("angularDamping", m.angularDamping));
        r.group(key1("maxLinearCmS", m.maxLinearCmS));
        r.group(key1("maxAngularRadS", m.maxAngularRadS));
        r.group(keyGradient("gravity", m.gravity));
        r.group(key1("friction", m.friction));
        r.group(key1("bendCompliance", m.bendCompliance));
        r.group(keyN("bendMul", m.bendMul, 4));
        r.group(key1("stretchCompliance", m.stretchCompliance));
        r.group(key1("inertiaMul", m.inertiaMul));
        r.group(keyGradient("radiusCm", m.radiusCm));
        r.group(keyGradient("worldInfluence", m.worldInfluence));
        r.group(keyGradient("gridVelocity", m.gridVelocity));
        r.group(key1("gridDensityForce", m.gridDensityForce));
        r.group(keyGradient("globalPose", m.globalPose));
        r.group(keyGradient("skinGlobalPose", m.skinGlobalPose));
        r.group(key1("gravityPreload", m.gravityPreload));
        if (m.simFraction >= 0.0f) r.group(key1("simFraction", m.simFraction));
        r.end();
    }
    for (const OcHairLook& l : d.looks) {
        RecordWriter r(out, "LOOK " + std::to_string(l.material));
        r.group(key3("baseColor", l.baseColor));
        r.group(key3("tipColor", l.tipColor));
        r.group(key1("colorVariation", l.colorVariation));
        r.group(key1("widthRootCm", l.widthRootCm));
        r.group(key1("widthTipCm", l.widthTipCm));
        r.group(key1("roughness", l.roughness));
        r.group(key1("shift", l.shift));
        r.group(key1("tt", l.tt));
        r.group(key1("trt", l.trt));
        r.group(key1("azimuthRough", l.azimuthRough));
        r.group(key3("tint", l.tint));
        r.group(key1("scatter", l.scatter));
        r.end();
    }
    if (d.generate.present) {
        const OcHairGenerate& g = d.generate;
        RecordWriter r(out, "GENERATE");
        r.group("seed " + std::to_string(g.seed));
        r.group("strands " + std::to_string(g.strands));
        r.group(keyN("lengthCm", g.lengthCm, 2));
        r.group("segments " + std::to_string(g.segments));
        r.group(key1("curlCm", g.curlCm));
        r.group(key1("curlFrequency", g.curlFrequency));
        r.group("clumps " + std::to_string(g.clumps));
        r.group(key1("clumpRadiusCm", g.clumpRadiusCm));
        r.group(key1("clumpTightness", g.clumpTightness));
        r.group(key3("partDirection", g.partDirection));
        r.group(key1("partWidthCm", g.partWidthCm));
        r.group(key3("style", g.style));
        r.group(key1("styleWeight", g.styleWeight));
        if (!g.mask.empty()) r.group("mask " + g.mask);
        r.group("material " + std::to_string(g.material));
        r.end();
    }
    {
        RecordWriter r(out, "LOD");
        r.group(keyN("distances", d.lod.distances, 3));
        r.group(keyN("strandFractions", d.lod.strandFractions, 4));
        r.end();
    }
    for (const OcHairProxy& p : d.proxies) {
        RecordWriter r(out, "PROXY " + p.name);
        if (!p.bone.empty()) r.group("bone " + p.bone);
        const char* shape = p.shape == OcHairProxyShape::Sphere ? "sphere" : (p.shape == OcHairProxyShape::Capsule ? "capsule" : "box");
        r.group(std::string("shape ") + shape);
        r.group(keyN("size", p.sizeCm, p.shape == OcHairProxyShape::Sphere ? 1 : (p.shape == OcHairProxyShape::Capsule ? 2 : 3)));
        r.group(key3("offset", p.offsetCm));
        r.end();
    }
    if (d.maxRenderStrands != kOcHairDefaultMaxRenderStrands) out += "MAXSTRANDS " + std::to_string(d.maxRenderStrands) + "\n";
    return out;
}

// ---- HEAD text reader ---------------------------------------------------------------------------------------------------------

bool parseOcHairHead(std::string_view text, OcHairData& out, std::string* why) {
    // Parse into a copy of the strands-free part, so a bad record leaves `out` as it was.
    OcHairData d;
    d.strands = out.strands;
    d.hasScalp = out.hasScalp;
    d.scalp = out.scalp;
    Parser p{d, {}, false, {}, {}};

    const std::vector<Record> records = splitRecords(text);
    bool sawHeader = false;
    for (const Record& rec : records) {
        const std::vector<std::string_view> t = splitWhitespace(rec.text);
        if (t.empty()) continue;
        const std::string at = " (line " + std::to_string(rec.line) + ")";

        if (equalsCI(t[0], "OCHAIR")) {
            if (sawHeader) return fail(why, ".ochair: a second OCHAIR header" + at);
            u32 version = 0;
            if (t.size() < 2 || !parseWholeU32(t[1], version)) return fail(why, ".ochair: OCHAIR needs a version number" + at);
            if (version > kOcHairVersion) return fail(why, ".ochair: this groom is version " + std::to_string(version) + ", newer than this engine reads (" + std::to_string(kOcHairVersion) + ")" + at);
            if (version < 1) return fail(why, ".ochair: unsupported version " + std::to_string(version) + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(why, ".ochair: the first record must be OCHAIR 1" + at);

        std::vector<KeyRun> runs;
        std::string err;
        bool ok = true;
        if (equalsCI(t[0], "SCALP")) {
            if (t.size() < 2) return fail(why, ".ochair: SCALP needs a mesh path" + at);
            d.scalpMesh = std::string(t[1]);
            d.scalpSubmesh = -1;
            for (usize i = 2; i < t.size(); ++i) {
                if (equalsCI(t[i], "SUBMESH") && i + 1 < t.size()) {
                    i32 sm = -1;
                    if (!parseWholeI32(t[i + 1], sm) || sm < 0) return fail(why, ".ochair: SUBMESH needs a whole number" + at);
                    d.scalpSubmesh = sm;
                    ++i;
                } else p.unknown("SCALP token '" + std::string(t[i]) + "'");
            }
        } else if (equalsCI(t[0], "SCALPJOINTS")) {
            d.scalpJoints.clear();
            for (usize i = 1; i < t.size(); ++i) d.scalpJoints.emplace_back(t[i]);
        } else if (equalsCI(t[0], "SIM")) {
            ok = keyRuns(t, 1, {}, runs, err) && p.sim(runs);
        } else if (equalsCI(t[0], "MATERIAL") || equalsCI(t[0], "LOOK")) {
            const bool isMaterial = equalsCI(t[0], "MATERIAL");
            u32 index = 0;
            if (t.size() < 2 || !parseWholeU32(t[1], index)) return fail(why, ".ochair: " + std::string(t[0]) + " needs a material index" + at);
            if (index > kOcHairMaxMaterials) return fail(why, ".ochair: material index " + std::to_string(index) + " is above " + std::to_string(kOcHairMaxMaterials) + at);
            if (!(isMaterial ? p.materialSeen : p.lookSeen).insert(index).second) return fail(why, ".ochair: " + std::string(t[0]) + " " + std::to_string(index) + " is declared twice" + at);
            ok = keyRuns(t, 2, {}, runs, err);
            if (ok && isMaterial) {
                OcHairMaterial m;
                m.index = index;
                ok = p.material(m, runs);
                if (ok) d.materials.push_back(m);
            } else if (ok) {
                OcHairLook l;
                l.material = index;
                ok = p.look(l, runs);
                if (ok) d.looks.push_back(l);
            }
        } else if (equalsCI(t[0], "GENERATE")) {
            ok = keyRuns(t, 1, {"mask"}, runs, err) && p.generate(runs);
        } else if (equalsCI(t[0], "LOD")) {
            ok = keyRuns(t, 1, {}, runs, err) && p.lod(runs);
        } else if (equalsCI(t[0], "PROXY")) {
            if (t.size() < 2) return fail(why, ".ochair: PROXY needs a name" + at);
            OcHairProxy proxy;
            proxy.name = std::string(t[1]);
            ok = keyRuns(t, 2, {"bone", "shape"}, runs, err) && p.proxy(proxy, runs);
            if (ok) d.proxies.push_back(std::move(proxy));
        } else if (equalsCI(t[0], "MAXSTRANDS")) {
            if (t.size() < 2 || !parseWholeU32(t[1], d.maxRenderStrands)) return fail(why, ".ochair: MAXSTRANDS needs a whole number" + at);
        } else {
            p.unknown("record '" + std::string(t[0]) + "'");
        }
        if (!ok) return fail(why, ".ochair: " + (err.empty() ? p.err : err) + at);
    }
    if (!sawHeader) return fail(why, ".ochair: no OCHAIR header");
    out = std::move(d);
    return true;
}

// ---- the chunks ----------------------------------------------------------------------------------------------------------------

bool writeOcHair(const OcHairData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid(why)) return false;

    Avr1File f;
    f.subtype = kAvrSubtypeHair;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;

    const std::string head = ocHairHeadText(in);
    f.add(kOcHairChunkHead, std::vector<u8>(head.begin(), head.end()), kAvrChunkRequired | kAvrChunkText);

    if (!in.strands.empty()) {
        const OcHairStrands& s = in.strands;
        std::vector<u8> strn;
        strn.reserve(8 + s.points.size() * 4 + s.start.size() * 4 + s.material.size() + s.width.size() * 4);
        W w{strn};
        w.u32v(static_cast<u32>(s.strandCount()));
        w.u32v(static_cast<u32>(s.pointCount()));
        for (f32 v : s.points) w.f32v(v);
        for (u32 v : s.start) w.u32v(v);
        for (u8 m : s.material) w.u8v(m);
        for (f32 v : s.width) w.f32v(v);
        f.add(kOcHairChunkStrands, std::move(strn));
    }
    if (in.hasScalp) {
        const OcHairScalp& sc = in.scalp;
        std::vector<u8> sclp;
        W w{sclp};
        w.u32v(static_cast<u32>(sc.vertexCount()));
        w.u32v(static_cast<u32>(sc.triangleCount()));
        for (f32 v : sc.positions) w.f32v(v);
        for (u32 v : sc.triangles) w.u32v(v);
        for (u8 v : sc.joints) w.u8v(v);
        for (f32 v : sc.weights) w.f32v(v);
        f.add(kOcHairChunkScalp, std::move(sclp));
    }
    return writeAvr1(f, out, why);
}

bool parseOcHair(const u8* bytes, usize size, OcHairData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeHair) return fail(why, ".ochair: container subtype is not HAIR");

    const AvrChunk* head = f.find(kOcHairChunkHead);
    if (!head) return fail(why, ".ochair: no HEAD chunk");

    OcHairData d;
    if (!parseOcHairHead(std::string_view(reinterpret_cast<const char*>(head->data.data()), head->data.size()), d, why)) return false;

    if (const AvrChunk* c = f.find(kOcHairChunkStrands)) {
        R r{c->data.data(), c->data.data() + c->data.size()};
        const u32 strandCount = r.u32v();
        const u32 pointCount = r.u32v();
        if (!r.ok) return fail(why, ".ochair: STRN ended inside its header");
        const u64 base = static_cast<u64>(pointCount) * 12 + (static_cast<u64>(strandCount) + 1) * 4 + strandCount;
        const u64 withWidth = base + static_cast<u64>(strandCount) * 4;
        if (strandCount == 0 || (static_cast<u64>(r.left()) != base && static_cast<u64>(r.left()) != withWidth))
            return fail(why, ".ochair: STRN is " + std::to_string(r.left()) + " bytes after its header where " + std::to_string(base) + " or " + std::to_string(withWidth) + " were expected");
        OcHairStrands& s = d.strands;
        s.points.resize(static_cast<usize>(pointCount) * 3);
        for (f32& v : s.points) v = r.f32v();
        s.start.resize(static_cast<usize>(strandCount) + 1);
        for (u32& v : s.start) v = r.u32v();
        s.material.resize(strandCount);
        for (u8& v : s.material) v = r.u8v();
        if (static_cast<u64>(c->data.size()) - 8 == withWidth) {
            s.width.resize(strandCount);
            for (f32& v : s.width) v = r.f32v();
        }
    }
    if (const AvrChunk* c = f.find(kOcHairChunkScalp)) {
        R r{c->data.data(), c->data.data() + c->data.size()};
        const u32 vc = r.u32v();
        const u32 tc = r.u32v();
        if (!r.ok) return fail(why, ".ochair: SCLP ended inside its header");
        const u64 need = static_cast<u64>(vc) * 12 + static_cast<u64>(tc) * 12 + static_cast<u64>(vc) * 4 + static_cast<u64>(vc) * 16;
        if (static_cast<u64>(r.left()) != need) return fail(why, ".ochair: SCLP is " + std::to_string(r.left()) + " bytes after its header where " + std::to_string(need) + " were expected");
        OcHairScalp& sc = d.scalp;
        d.hasScalp = true;
        sc.positions.resize(static_cast<usize>(vc) * 3);
        for (f32& v : sc.positions) v = r.f32v();
        sc.triangles.resize(static_cast<usize>(tc) * 3);
        for (u32& v : sc.triangles) v = r.u32v();
        sc.joints.resize(static_cast<usize>(vc) * 4);
        for (u8& v : sc.joints) v = r.u8v();
        sc.weights.resize(static_cast<usize>(vc) * 4);
        for (f32& v : sc.weights) v = r.f32v();
    }

    if (!d.valid(why)) return false;
    out = std::move(d);
    return true;
}

bool loadOcHair(const std::string& path, OcHairData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return fail(why, ".ochair: cannot open " + path);
    const std::streamoff n = in.tellg();
    if (n <= 0) return fail(why, ".ochair: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!in) return fail(why, ".ochair: short read on " + path);
    return parseOcHair(bytes.data(), bytes.size(), out, why);
}

bool saveOcHair(const std::string& path, const OcHairData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcHair(in, bytes, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (!writeFileBytes(path, bytes.data(), bytes.size())) return fail(why, ".ochair: cannot write " + path);
    return true;
}

} // namespace aver::fmt
