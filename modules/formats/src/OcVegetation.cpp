// Reader and writer for .ocveg (text). See OcVegetation.hpp.
#include "aver/formats/OcVegetation.hpp"

#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

std::string fnum(f32 v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}
bool toF(const std::string& s, f32& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}
template <class T> bool toInt(const std::string& s, T& out, int base = 10) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

} // namespace

bool OcVegetationRecord::valid(std::string* why) const {
    if (!(areaMin[0] < areaMax[0]) || !(areaMin[1] < areaMax[1])) return fail(why, "AREA is empty");
    if (!(zMin < zMax)) return fail(why, "AREA z range is empty");
    if (cellCm < 100) return fail(why, "CELL must be at least 100 cm");
    for (const OcVegAdded& a : added) if (a.asset.empty()) return fail(why, "an ADD names no asset");
    for (const OcVegErased& e : erased) if (!(e.radiusCm >= 0.0f)) return fail(why, "an ERASE has a negative radius");
    return true;
}

bool parseOcVegetation(const std::string& text, OcVegetationRecord& out, std::string* why) {
    OcVegetationRecord r;
    bool header = false;
    std::istringstream in(text);
    std::string line;
    usize n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::vector<std::string> t;
        for (std::string w; ls >> w;) t.push_back(w);
        if (t.empty() || t[0][0] == '#') continue;
        const std::string at = ".ocveg line " + std::to_string(n) + ": ";
        const std::string& k = t[0];
        if (k == "OCVEG") {
            i32 v = 0;
            if (t.size() < 2 || !toInt(t[1], v) || v != 1) return fail(why, at + "unsupported version");
            header = true;
        } else if (!header) {
            return fail(why, at + "the first record must be OCVEG <version>");
        } else if (k == "BIOME" && t.size() >= 2) {
            r.biomes.push_back(t[1]);
        } else if (k == "SEED" && t.size() >= 2) {
            if (!toInt(t[1], r.seed)) return fail(why, at + "bad SEED");
        } else if (k == "AREA") {
            f32 f[6];
            bool ok = t.size() >= 7;
            for (int i = 0; ok && i < 6; ++i) ok = toF(t[1 + i], f[i]);
            if (!ok) return fail(why, at + "AREA needs minx miny maxx maxy zmin zmax");
            r.areaMin[0] = f[0]; r.areaMin[1] = f[1]; r.areaMax[0] = f[2]; r.areaMax[1] = f[3]; r.zMin = f[4]; r.zMax = f[5];
        } else if (k == "CELL") {
            if (t.size() < 2 || !toInt(t[1], r.cellCm)) return fail(why, at + "bad CELL");
        } else if (k == "OUTPUT" && t.size() >= 2) {
            r.output = t[1];
        } else if (k == "HASH") {
            if (t.size() < 2 || !toInt(t[1], r.inputHash, 16)) return fail(why, at + "bad HASH");
        } else if (k == "COUNT") {
            if (t.size() < 2 || !toInt(t[1], r.count)) return fail(why, at + "bad COUNT");
        } else if (k == "ADD") {
            OcVegAdded a;
            bool ok = t.size() >= 12;
            if (ok) a.asset = t[1];
            for (int i = 0; ok && i < 3; ++i) ok = toF(t[2 + i], a.pos[i]);
            for (int i = 0; ok && i < 4; ++i) ok = toF(t[5 + i], a.rot[i]);
            for (int i = 0; ok && i < 3; ++i) ok = toF(t[9 + i], a.scale[i]);
            if (!ok) return fail(why, at + "ADD needs asset, 3 position, 4 rotation, 3 scale numbers");
            r.added.push_back(std::move(a));
        } else if (k == "ERASE") {
            OcVegErased e;
            bool ok = t.size() >= 5;
            for (int i = 0; ok && i < 3; ++i) ok = toF(t[1 + i], e.center[i]);
            if (ok) ok = toF(t[4], e.radiusCm);
            if (!ok) return fail(why, at + "ERASE needs x y z radius");
            r.erased.push_back(e);
        }
        // unknown records are skipped
    }
    if (!header) return fail(why, ".ocveg: no OCVEG header");
    std::string v;
    if (!r.valid(&v)) return fail(why, ".ocveg: " + v);
    out = std::move(r);
    return true;
}

bool writeOcVegetation(const OcVegetationRecord& in, std::string& out, std::string* why) {
    std::string v;
    if (!in.valid(&v)) return fail(why, ".ocveg: refusing to write: " + v);
    std::ostringstream o;
    o << "OCVEG 1\n";
    for (const std::string& b : in.biomes) o << "BIOME " << b << "\n";
    o << "SEED " << in.seed << "\n";
    o << "AREA " << fnum(in.areaMin[0]) << " " << fnum(in.areaMin[1]) << " " << fnum(in.areaMax[0]) << " " << fnum(in.areaMax[1]) << " "
      << fnum(in.zMin) << " " << fnum(in.zMax) << "\n";
    o << "CELL " << in.cellCm << "\n";
    if (!in.output.empty()) o << "OUTPUT " << in.output << "\n";
    char hx[24];
    std::snprintf(hx, sizeof hx, "%016llx", static_cast<unsigned long long>(in.inputHash));
    o << "HASH " << hx << "\n";
    o << "COUNT " << in.count << "\n";
    for (const OcVegAdded& a : in.added) {
        o << "ADD " << a.asset;
        for (f32 f : a.pos) o << " " << fnum(f);
        for (f32 f : a.rot) o << " " << fnum(f);
        for (f32 f : a.scale) o << " " << fnum(f);
        o << "\n";
    }
    for (const OcVegErased& e : in.erased)
        o << "ERASE " << fnum(e.center[0]) << " " << fnum(e.center[1]) << " " << fnum(e.center[2]) << " " << fnum(e.radiusCm) << "\n";
    out = o.str();
    return true;
}

bool loadOcVegetation(const std::string& path, OcVegetationRecord& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) return fail(why, ".ocveg: cannot read " + path);
    return parseOcVegetation(text, out, why);
}

bool saveOcVegetation(const std::string& path, const OcVegetationRecord& in, std::string* why) {
    std::string text;
    if (!writeOcVegetation(in, text, why)) return false;
    createParentDirectories(path);
    if (!writeFileTextAtomic(path, text)) return fail(why, ".ocveg: cannot write " + path);
    return true;
}

} // namespace aver::fmt
