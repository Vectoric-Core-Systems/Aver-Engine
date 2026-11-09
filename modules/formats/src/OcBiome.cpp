// Reader and writer for .ocbiome (text). See OcBiome.hpp for the grammar.
#include "aver/formats/OcBiome.hpp"

#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

std::string fnum(f32 v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}
std::string inum(i32 v) { return std::to_string(v); }

bool toF(std::string_view s, f32& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}
bool toI(std::string_view s, i32& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> v;
    usize b = 0;
    for (;;) {
        const usize e = s.find(sep, b);
        if (e == std::string_view::npos) { v.push_back(s.substr(b)); break; }
        v.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return v;
}

// "a,b,c" -> floats; false unless exactly n.
bool floats(std::string_view s, usize n, f32* out) {
    const auto parts = split(s, ',');
    if (parts.size() != n) return false;
    for (usize i = 0; i < n; ++i) if (!toF(parts[i], out[i])) return false;
    return true;
}

const char* kindName(OcBiomeMask::Kind k) {
    switch (k) {
        case OcBiomeMask::Kind::Layer: return "layer";
        case OcBiomeMask::Kind::Noise: return "noise";
        case OcBiomeMask::Kind::Spline: return "spline";
        case OcBiomeMask::Kind::Volume: return "volume";
        default: return "none";
    }
}

} // namespace

bool OcBiomeData::valid(std::string* why) const {
    const auto fin = [](f32 v) { return std::isfinite(v); };
    const OcBiomeMask& m = mask;
    if (!fin(m.min) || !fin(m.edge) || m.edge < 0.0f || !fin(m.halfWidthCm) || m.halfWidthCm < 0.0f || !fin(m.noiseScaleCm))
        return fail(why, "mask: min/edge/half/scale must be finite and edge, half >= 0");
    if ((m.kind == OcBiomeMask::Kind::Layer || m.kind == OcBiomeMask::Kind::Spline || m.kind == OcBiomeMask::Kind::Volume) && m.name.empty())
        return fail(why, "mask: a layer, spline or volume mask needs name=");
    if (m.kind == OcBiomeMask::Kind::Noise && (m.noiseScaleCm <= 0.0f || m.noiseOctaves < 1 || m.noiseOctaves > 8))
        return fail(why, "mask: a noise mask needs scale > 0 and 1..8 octaves");
    for (usize i = 0; i < entries.size(); ++i) {
        const OcBiomeEntry& e = entries[i];
        const std::string at = "entry " + std::to_string(i) + ": ";
        if (e.foliage.empty()) return fail(why, at + "no foliage type");
        if (!fin(e.densityPer100m2) || !(e.densityPer100m2 > 0.0f)) return fail(why, at + "density must be > 0");
        if (!fin(e.slopeMinDeg) || !fin(e.slopeMaxDeg) || e.slopeMinDeg > e.slopeMaxDeg) return fail(why, at + "slope range is not ordered");
        if (!fin(e.heightMinCm) || !fin(e.heightMaxCm) || e.heightMinCm > e.heightMaxCm) return fail(why, at + "height range is not ordered");
        if (!fin(e.layerMin) || !fin(e.noiseScaleCm) || e.noiseScaleCm < 0.0f || !fin(e.noiseMin) || !fin(e.noiseMax) || e.noiseMin > e.noiseMax)
            return fail(why, at + "layer/noise gate is not ordered");
        if (!fin(e.clumpRadiusCm) || e.clumpRadiusCm < 0.0f) return fail(why, at + "clump radius must be >= 0");
        if (e.clumpRadiusCm > 0.0f && (e.clumpMin < 1 || e.clumpMax < e.clumpMin || e.clumpMax > 64))
            return fail(why, at + "clump counts need 1 <= min <= max <= 64");
        if (!fin(e.pruneRadiusCm)) return fail(why, at + "prune radius must be finite");
    }
    return true;
}

bool parseOcBiome(const std::string& text, OcBiomeData& out, std::string* why) {
    OcBiomeData d;
    bool sawHeader = false;
    std::istringstream in(text);
    std::string line;
    usize lineNo = 0;
    const auto bad = [&](const std::string& m) { return fail(why, ".ocbiome line " + std::to_string(lineNo) + ": " + m); };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const usize hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::vector<std::string> tok;
        {
            std::istringstream ls(line);
            std::string t;
            while (ls >> t) tok.push_back(t);
        }
        if (tok.empty()) continue;
        const std::string& kw = tok[0];
        if (kw == "OCBIOME") {
            i32 ver = 0;
            if (tok.size() < 2 || !toI(tok[1], ver)) return bad("OCBIOME needs a version");
            if (ver != 1) return bad("unsupported OCBIOME version " + tok[1]);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return bad("the first record must be OCBIOME <version>");
        if (kw == "NAME") {
            const usize p = line.find("NAME");
            d.name = line.substr(p + 4);
            const usize b = d.name.find_first_not_of(" \t");
            d.name = b == std::string::npos ? std::string() : d.name.substr(b);
            while (!d.name.empty() && (d.name.back() == ' ' || d.name.back() == '\t')) d.name.pop_back();
            continue;
        }
        if (kw == "PRIORITY") {
            if (tok.size() < 2 || !toI(tok[1], d.priority)) return bad("PRIORITY needs an integer");
            continue;
        }
        if (kw == "MASK") {
            if (tok.size() < 2) return bad("MASK needs a kind");
            OcBiomeMask m;
            if (tok[1] == "layer") m.kind = OcBiomeMask::Kind::Layer;
            else if (tok[1] == "noise") m.kind = OcBiomeMask::Kind::Noise;
            else if (tok[1] == "spline") m.kind = OcBiomeMask::Kind::Spline;
            else if (tok[1] == "volume") m.kind = OcBiomeMask::Kind::Volume;
            else if (tok[1] != "none") return bad("unknown mask kind '" + tok[1] + "'");
            for (usize i = 2; i < tok.size(); ++i) {
                const usize eq = tok[i].find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok[i].substr(0, eq), v = tok[i].substr(eq + 1);
                bool ok = true;
                if (k == "name") m.name = v;
                else if (k == "min") ok = toF(v, m.min);
                else if (k == "edge") ok = toF(v, m.edge);
                else if (k == "half") ok = toF(v, m.halfWidthCm);
                else if (k == "scale") ok = toF(v, m.noiseScaleCm);
                else if (k == "octaves") ok = toI(v, m.noiseOctaves);
                else if (k == "seed") ok = toI(v, m.noiseSeed);
                else if (k == "invert") m.invert = v == "1";
                if (!ok) return bad("bad value for " + k);
            }
            d.mask = m;
            continue;
        }
        if (kw == "ENTRY") {
            OcBiomeEntry e;
            for (usize i = 1; i < tok.size(); ++i) {
                const usize eq = tok[i].find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok[i].substr(0, eq), v = tok[i].substr(eq + 1);
                bool ok = true;
                f32 f[4];
                if (k == "foliage") e.foliage = v;
                else if (k == "density") ok = toF(v, e.densityPer100m2);
                else if (k == "slope") { ok = floats(v, 2, f); if (ok) { e.slopeMinDeg = f[0]; e.slopeMaxDeg = f[1]; } }
                else if (k == "height") { ok = floats(v, 2, f); if (ok) { e.heightMinCm = f[0]; e.heightMaxCm = f[1]; } }
                else if (k == "layer") e.layer = v;
                else if (k == "layerMin") ok = toF(v, e.layerMin);
                else if (k == "noise") {
                    ok = floats(v, 4, f);
                    if (ok) { e.noiseScaleCm = f[0]; e.noiseMin = f[1]; e.noiseMax = f[2]; e.noiseSeed = static_cast<i32>(f[3]); }
                } else if (k == "clump") {
                    ok = floats(v, 3, f);
                    if (ok) { e.clumpRadiusCm = f[0]; e.clumpMin = static_cast<i32>(f[1]); e.clumpMax = static_cast<i32>(f[2]); }
                } else if (k == "priority") ok = toI(v, e.priority);
                else if (k == "prune") ok = toF(v, e.pruneRadiusCm);
                if (!ok) return bad("bad value for " + k);
            }
            d.entries.push_back(std::move(e));
            continue;
        }
        // An unknown record is skipped: a newer file still opens in an older build.
    }
    if (!sawHeader) return fail(why, ".ocbiome: no OCBIOME header");
    std::string v;
    if (!d.valid(&v)) return fail(why, ".ocbiome: " + v);
    out = std::move(d);
    return true;
}

bool writeOcBiome(const OcBiomeData& in, std::string& out, std::string* why) {
    std::string v;
    if (!in.valid(&v)) return fail(why, ".ocbiome: refusing to write an invalid biome: " + v);
    const OcBiomeMask dm;
    const OcBiomeEntry de;
    std::ostringstream o;
    o << "OCBIOME 1\n";
    o << "NAME " << in.name << "\n";
    if (in.priority != 0) o << "PRIORITY " << in.priority << "\n";
    const OcBiomeMask& m = in.mask;
    if (m.kind != OcBiomeMask::Kind::None) {
        o << "MASK " << kindName(m.kind);
        if (!m.name.empty()) o << " name=" << m.name;
        if (m.kind == OcBiomeMask::Kind::Layer || m.kind == OcBiomeMask::Kind::Noise) o << " min=" << fnum(m.min);
        if (m.kind == OcBiomeMask::Kind::Noise) {
            o << " scale=" << fnum(m.noiseScaleCm) << " octaves=" << inum(m.noiseOctaves);
            if (m.noiseSeed != dm.noiseSeed) o << " seed=" << inum(m.noiseSeed);
        }
        if (m.kind == OcBiomeMask::Kind::Spline) o << " half=" << fnum(m.halfWidthCm);
        o << " edge=" << fnum(m.edge);
        if (m.invert) o << " invert=1";
        o << "\n";
    }
    for (const OcBiomeEntry& e : in.entries) {
        o << "ENTRY foliage=" << e.foliage << " density=" << fnum(e.densityPer100m2);
        if (e.slopeMinDeg != de.slopeMinDeg || e.slopeMaxDeg != de.slopeMaxDeg) o << " slope=" << fnum(e.slopeMinDeg) << "," << fnum(e.slopeMaxDeg);
        if (e.heightMinCm != de.heightMinCm || e.heightMaxCm != de.heightMaxCm) o << " height=" << fnum(e.heightMinCm) << "," << fnum(e.heightMaxCm);
        if (!e.layer.empty()) o << " layer=" << e.layer << " layerMin=" << fnum(e.layerMin);
        if (e.noiseScaleCm > 0.0f) o << " noise=" << fnum(e.noiseScaleCm) << "," << fnum(e.noiseMin) << "," << fnum(e.noiseMax) << "," << inum(e.noiseSeed);
        if (e.clumpRadiusCm > 0.0f) o << " clump=" << fnum(e.clumpRadiusCm) << "," << inum(e.clumpMin) << "," << inum(e.clumpMax);
        if (e.priority != 0) o << " priority=" << inum(e.priority);
        if (e.pruneRadiusCm >= 0.0f) o << " prune=" << fnum(e.pruneRadiusCm);
        o << "\n";
    }
    out = o.str();
    return true;
}

bool loadOcBiome(const std::string& path, OcBiomeData& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) return fail(why, ".ocbiome: cannot read " + path);
    return parseOcBiome(text, out, why);
}

bool saveOcBiome(const std::string& path, const OcBiomeData& in, std::string* why) {
    std::string text;
    if (!writeOcBiome(in, text, why)) return false;
    createParentDirectories(path);
    if (!writeFileTextAtomic(path, text)) return fail(why, ".ocbiome: cannot write " + path);
    return true;
}

} // namespace aver::fmt
