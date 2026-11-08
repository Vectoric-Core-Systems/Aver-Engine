#include "aver/synapse/EqsAsset.hpp"

#include "aver/synapse/TextRecords.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace aver::synapse {
namespace {

const char* kBuiltinContexts[] = {"querier", "target", "seen", "point"};

std::string lowerStr(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool isIdent(std::string_view s) {
    if (s.empty()) return false;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && i > 0))) return false;
    }
    return true;
}

bool isCtxName(std::string_view s) {
    if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

bool parseMask(std::string_view s, u32& out) {
    if (s.empty()) return false;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s.remove_prefix(2); }
    u64 v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || v > 0xFFFFFFFFull) return false;
    out = static_cast<u32>(v);
    return true;
}

// ---- parser ---------------------------------------------------------------------------------------

struct Parser {
    std::string name;
    std::string err;
    std::vector<std::string> declared;   // CONTEXT names

    bool fail(u32 line, const std::string& msg) {
        err = name + ":" + std::to_string(line) + ": " + msg;
        return false;
    }

    bool ctxKnown(const std::string& c) const {
        for (const char* b : kBuiltinContexts)
            if (c == b) return true;
        return std::find(declared.begin(), declared.end(), c) != declared.end();
    }

    bool checkKeys(const TextRecord& r, std::initializer_list<const char*> allowed, const std::string& what) {
        for (const auto& p : r.kv) {
            bool ok = false;
            for (const char* a : allowed) ok = ok || p.first == a;
            if (!ok) return fail(r.line, "unknown key '" + p.first + "' on " + what);
        }
        return true;
    }

    bool getF(const TextRecord& r, const char* key, f32& v, f32 lo, f32 hi) {
        if (!r.has(key)) return true;
        f32 x = 0;
        if (!parseF32(r.get(key), x)) return fail(r.line, std::string("'") + key + "' is not a finite number");
        if (x < lo || x > hi) return fail(r.line, std::string("'") + key + "' is out of range");
        v = x;
        return true;
    }
    bool getU(const TextRecord& r, const char* key, u32& v, u32 lo, u32 hi) {
        if (!r.has(key)) return true;
        i32 x = 0;
        if (!parseI32(r.get(key), x)) return fail(r.line, std::string("'") + key + "' is not an integer");
        if (x < 0 || static_cast<u32>(x) < lo || static_cast<u32>(x) > hi) return fail(r.line, std::string("'") + key + "' is out of range");
        v = static_cast<u32>(x);
        return true;
    }
    bool getB(const TextRecord& r, const char* key, bool& v) {
        if (!r.has(key)) return true;
        const std::string s = r.get(key);
        if (s != "0" && s != "1") return fail(r.line, std::string("'") + key + "' must be 0 or 1");
        v = s == "1";
        return true;
    }
    bool getCtx(const TextRecord& r, const char* key, std::string& v) {
        if (!r.has(key)) return true;
        const std::string s = r.get(key);
        if (!ctxKnown(s)) return fail(r.line, "context '" + s + "' is not declared");
        v = s;
        return true;
    }

    bool parseGenerator(const TextRecord& r, EqsGenerator& g) {
        if (r.args.size() != 1) return fail(r.line, "GENERATOR needs exactly one kind");
        const std::string k = lowerStr(r.args[0]);
        EqsGen kind;
        if (k == "grid") kind = EqsGen::Grid;
        else if (k == "ring") kind = EqsGen::Ring;
        else if (k == "around") kind = EqsGen::Around;
        else if (k == "nav") kind = EqsGen::Nav;
        else if (k == "actors") kind = EqsGen::Actors;
        else if (k == "slots") kind = EqsGen::Slots;
        else if (k == "points") kind = EqsGen::Points;
        else return fail(r.line, "unknown generator '" + r.args[0] + "'");
        g = eqsDefaultGenerator(kind);
        const std::string what = "GENERATOR " + k;

        bool ok = false;
        switch (kind) {
            case EqsGen::Grid:   ok = checkKeys(r, {"centre", "project", "zoffset", "max", "halfx", "halfy", "step"}, what); break;
            case EqsGen::Ring:   ok = checkKeys(r, {"centre", "project", "zoffset", "max", "inner", "outer", "rings", "count", "arcstart", "arcend", "facing"}, what); break;
            case EqsGen::Around: ok = checkKeys(r, {"centre", "project", "zoffset", "max", "radius", "count", "arc", "facing"}, what); break;
            case EqsGen::Nav:    ok = checkKeys(r, {"centre", "project", "zoffset", "max", "radius", "stride", "region"}, what); break;
            case EqsGen::Actors: ok = checkKeys(r, {"centre", "project", "zoffset", "max", "class", "tagbits", "named", "radius", "derived", "excludequerier"}, what); break;
            case EqsGen::Slots:  ok = checkKeys(r, {"centre", "project", "zoffset", "max", "tags", "matchall", "radius", "free"}, what); break;
            case EqsGen::Points: ok = checkKeys(r, {"centre", "project", "zoffset", "max", "list"}, what); break;
        }
        if (!ok) return false;

        if (!getCtx(r, "centre", g.centre)) return false;
        if (r.has("project")) {
            const std::string p = lowerStr(r.get("project"));
            if (p == "none") g.project = EqsProject::None;
            else if (p == "nav") g.project = EqsProject::Nav;
            else if (p == "ground") g.project = EqsProject::Ground;
            else return fail(r.line, "project must be none, nav or ground");
        }
        if (!getF(r, "zoffset", g.zOffset, -1e7f, 1e7f)) return false;
        if (!getU(r, "max", g.maxItems, 1, kEqsMaxItems)) return false;

        switch (kind) {
            case EqsGen::Grid:
                if (!getF(r, "halfx", g.halfX, 0, 1e6f) || !getF(r, "halfy", g.halfY, 0, 1e6f) || !getF(r, "step", g.step, 10, 1e6f)) return false;
                {
                    const f64 nx = std::floor(2.0 * g.halfX / g.step + 1e-4) + 1.0;
                    const f64 ny = std::floor(2.0 * g.halfY / g.step + 1e-4) + 1.0;
                    if (nx * ny > static_cast<f64>(g.maxItems))
                        return fail(r.line, "grid has " + std::to_string(static_cast<u64>(nx * ny)) + " points, over max=" + std::to_string(g.maxItems));
                }
                break;
            case EqsGen::Ring:
                if (!getF(r, "inner", g.inner, 0, 1e6f) || !getF(r, "outer", g.outer, 0, 1e6f) || !getU(r, "rings", g.rings, 1, 32) ||
                    !getU(r, "count", g.count, 1, 256) || !getF(r, "arcstart", g.arcStart, -3600, 3600) || !getF(r, "arcend", g.arcEnd, -3600, 3600) ||
                    !getCtx(r, "facing", g.facing))
                    return false;
                if (g.outer < g.inner) return fail(r.line, "outer must not be below inner");
                if (g.arcEnd < g.arcStart) return fail(r.line, "arcend must not be below arcstart");
                break;
            case EqsGen::Around: {
                f32 arc = 360;
                if (!getF(r, "radius", g.radius, 0, 1e6f) || !getU(r, "count", g.count, 1, 256) || !getF(r, "arc", arc, 0, 360) ||
                    !getCtx(r, "facing", g.facing))
                    return false;
                if (g.radius <= 0) return fail(r.line, "'radius' must be above 0");
                if (arc <= 0) return fail(r.line, "'arc' must be above 0");
                g.arcStart = -arc * 0.5f;
                g.arcEnd = arc * 0.5f;
                break;
            }
            case EqsGen::Nav:
                if (!getF(r, "radius", g.radius, 0, 1e6f) || !getU(r, "stride", g.stride, 1, 16)) return false;
                if (g.radius <= 0) return fail(r.line, "'radius' must be above 0");
                if (r.has("region")) {
                    const std::string s = lowerStr(r.get("region"));
                    if (s == "same") g.sameRegion = true;
                    else if (s == "any") g.sameRegion = false;
                    else return fail(r.line, "region must be same or any");
                }
                break;
            case EqsGen::Actors: {
                const int sel = (r.has("class") ? 1 : 0) + (r.has("tagbits") ? 1 : 0) + (r.has("named") ? 1 : 0);
                if (sel != 1) return fail(r.line, "actors needs exactly one of class, tagbits, named");
                g.className = r.get("class");
                g.namePrefix = r.get("named");
                if (r.has("tagbits") && (!parseMask(r.get("tagbits"), g.tagBits) || g.tagBits == 0)) return fail(r.line, "'tagbits' is not a non-zero mask");
                if ((r.has("class") && g.className.empty()) || (r.has("named") && g.namePrefix.empty())) return fail(r.line, "empty actor filter");
                if (!getF(r, "radius", g.radius, 0, 1e7f) || !getB(r, "derived", g.derived) || !getB(r, "excludequerier", g.excludeQuerier)) return false;
                break;
            }
            case EqsGen::Slots:
                if (!getF(r, "radius", g.radius, 0, 1e7f) || !getB(r, "matchall", g.matchAll) || !getB(r, "free", g.freeOnly)) return false;
                if (g.radius <= 0) return fail(r.line, "'radius' must be above 0");
                if (r.has("tags")) {
                    g.slotTags = TagList::parse(r.get("tags"));
                    if (g.slotTags.v.empty()) return fail(r.line, "'tags' has no valid tag names");
                }
                break;
            case EqsGen::Points: {
                if (!r.has("list")) return fail(r.line, "points needs list=\"x,y,z;x,y,z\"");
                const std::string list = r.get("list");
                usize i = 0;
                while (i <= list.size()) {
                    usize j = list.find(';', i);
                    if (j == std::string::npos) j = list.size();
                    const std::string item = list.substr(i, j - i);
                    f32 v[3];
                    usize a = 0;
                    for (int c = 0; c < 3; ++c) {
                        usize b = item.find(',', a);
                        if (c == 2) b = item.size();
                        if (b == std::string::npos) return fail(r.line, "points list needs x,y,z triples");
                        if (!parseF32(item.substr(a, b - a), v[c])) return fail(r.line, "points list has a bad number");
                        a = b + 1;
                    }
                    if (item.find(',', 0) == std::string::npos) return fail(r.line, "points list needs x,y,z triples");
                    g.points.push_back(Vec3{v[0], v[1], v[2]});
                    if (g.points.size() > 256) return fail(r.line, "points list is over 256 entries");
                    i = j + 1;
                    if (j == list.size()) break;
                }
                break;
            }
        }
        return true;
    }

    bool parseTest(const TextRecord& r, EqsTest& t) {
        if (r.args.size() != 1) return fail(r.line, "TEST needs exactly one kind");
        const std::string k = lowerStr(r.args[0]);
        EqsTestKind kind;
        if (k == "distance") kind = EqsTestKind::Distance;
        else if (k == "dot") kind = EqsTestKind::Dot;
        else if (k == "trace") kind = EqsTestKind::Trace;
        else if (k == "path") kind = EqsTestKind::Path;
        else if (k == "tag") kind = EqsTestKind::Tag;
        else if (k == "random") kind = EqsTestKind::Random;
        else if (k == "custom") kind = EqsTestKind::Custom;
        else return fail(r.line, "unknown test '" + r.args[0] + "'");
        t = eqsDefaultTest(kind);
        const std::string what = "TEST " + k;

        bool ok = false;
        switch (kind) {
            case EqsTestKind::Distance: ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "to", "metric"}, what); break;
            case EqsTestKind::Dot:      ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "from", "ref", "metric"}, what); break;
            case EqsTestKind::Trace:    ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "from", "fromz", "toz", "radius", "gap"}, what); break;
            case EqsTestKind::Path:     ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "from", "metric", "maxexp", "partial"}, what); break;
            case EqsTestKind::Tag:      ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "names", "bits", "match"}, what); break;
            case EqsTestKind::Random:   ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "seed"}, what); break;
            case EqsTestKind::Custom:   ok = checkKeys(r, {"purpose", "weight", "curve", "lo", "hi", "min", "max", "want", "name", "from", "p0", "p1", "p2", "p3", "text"}, what); break;
        }
        if (!ok) return false;

        if (r.has("purpose")) {
            const std::string s = lowerStr(r.get("purpose"));
            if (s == "filter") t.purpose = EqsPurpose::Filter;
            else if (s == "score") t.purpose = EqsPurpose::Score;
            else if (s == "both") t.purpose = EqsPurpose::Both;
            else return fail(r.line, "purpose must be filter, score or both");
        }
        if (r.has("curve")) {
            const std::string s = lowerStr(r.get("curve"));
            if (s == "linear") t.curve = EqsCurve::Linear;
            else if (s == "inverse") t.curve = EqsCurve::Inverse;
            else if (s == "square") t.curve = EqsCurve::Square;
            else if (s == "invsquare") t.curve = EqsCurve::InvSquare;
            else if (s == "sqrt") t.curve = EqsCurve::Sqrt;
            else if (s == "step") t.curve = EqsCurve::Step;
            else return fail(r.line, "unknown curve '" + s + "'");
        }
        if (!getF(r, "weight", t.weight, -1e6f, 1e6f)) return false;
        if (r.has("lo")) { if (!getF(r, "lo", t.lo, -1e12f, 1e12f)) return false; t.hasLo = true; }
        if (r.has("hi")) { if (!getF(r, "hi", t.hi, -1e12f, 1e12f)) return false; t.hasHi = true; }
        if (t.hasLo && t.hasHi && t.hi < t.lo) return fail(r.line, "'hi' is below 'lo'");
        if (r.has("min")) { f32 v = 0; if (!getF(r, "min", v, -1e12f, 1e12f)) return false; t.min = v; }
        if (r.has("max")) { f32 v = 0; if (!getF(r, "max", v, -1e12f, 1e12f)) return false; t.max = v; }
        if (t.min > t.max) return fail(r.line, "'min' is above 'max'");
        if (!getB(r, "want", t.want)) return false;
        t.name = r.get("name");

        switch (kind) {
            case EqsTestKind::Distance:
                if (!getCtx(r, "to", t.to)) return false;
                if (r.has("metric")) {
                    t.metric = lowerStr(r.get("metric"));
                    if (t.metric != "3d" && t.metric != "2d" && t.metric != "z" && t.metric != "absz") return fail(r.line, "metric must be 3d, 2d, z or absz");
                }
                break;
            case EqsTestKind::Dot:
                if (!getCtx(r, "from", t.from)) return false;
                if (r.has("metric")) {
                    t.metric = lowerStr(r.get("metric"));
                    if (t.metric != "3d" && t.metric != "2d") return fail(r.line, "metric must be 3d or 2d");
                }
                if (r.has("ref")) {
                    const std::string s = r.get("ref");
                    const std::string l = lowerStr(s);
                    if (l == "forward" || l == "up") t.ref = l;
                    else if (l.rfind("toward:", 0) == 0) {
                        t.ref = "toward";
                        t.refCtx = s.substr(7);
                        if (!ctxKnown(t.refCtx)) return fail(r.line, "context '" + t.refCtx + "' is not declared");
                    } else if (l.rfind("dir:", 0) == 0) {
                        t.ref = "dir";
                        f32 v[3];
                        usize a = 4;
                        for (int c = 0; c < 3; ++c) {
                            usize b = s.find(',', a);
                            if (c == 2) b = s.size();
                            if (b == std::string::npos || !parseF32(s.substr(a, b - a), v[c])) return fail(r.line, "ref=dir: needs x,y,z");
                            a = b + 1;
                        }
                        t.refDir = Vec3{v[0], v[1], v[2]};
                    } else return fail(r.line, "ref must be forward, up, toward:<context> or dir:x,y,z");
                }
                break;
            case EqsTestKind::Trace:
                if (!getCtx(r, "from", t.from) || !getF(r, "fromz", t.fromZ, -1e7f, 1e7f) || !getF(r, "toz", t.toZ, -1e7f, 1e7f) ||
                    !getF(r, "radius", t.radius, 0, 1e6f) || !getF(r, "gap", t.gap, 0, 1e6f))
                    return false;
                break;
            case EqsTestKind::Path:
                if (!getCtx(r, "from", t.from) || !getU(r, "maxexp", t.maxExp, 1, 1000000)) return false;
                if (r.has("metric")) {
                    t.metric = lowerStr(r.get("metric"));
                    if (t.metric != "length" && t.metric != "reachable") return fail(r.line, "metric must be length or reachable");
                }
                if (r.has("partial")) {
                    const std::string s = lowerStr(r.get("partial"));
                    if (s == "ok") t.partialOk = true;
                    else if (s == "fail") t.partialOk = false;
                    else return fail(r.line, "partial must be fail or ok");
                }
                break;
            case EqsTestKind::Tag: {
                if (r.has("names") == r.has("bits")) return fail(r.line, "tag needs exactly one of names, bits");
                if (r.has("names")) {
                    t.tags = TagList::parse(r.get("names"));
                    if (t.tags.v.empty()) return fail(r.line, "'names' has no valid tag names");
                } else if (!parseMask(r.get("bits"), t.bits) || t.bits == 0) {
                    return fail(r.line, "'bits' is not a non-zero mask");
                }
                if (r.has("match")) {
                    const std::string s = lowerStr(r.get("match"));
                    if (s == "any") t.match = 0;
                    else if (s == "all") t.match = 1;
                    else if (s == "none") t.match = 2;
                    else return fail(r.line, "match must be any, all or none");
                }
                break;
            }
            case EqsTestKind::Random:
                if (!getU(r, "seed", t.seed, 0, 0x7FFFFFFFu)) return false;
                break;
            case EqsTestKind::Custom:
                if (t.name.empty()) return fail(r.line, "custom needs name=<managed test>");
                t.custom = t.name;
                if (!getCtx(r, "from", t.from)) return false;
                for (int i = 0; i < 4; ++i) {
                    const std::string key = "p" + std::to_string(i);
                    if (!getF(r, key.c_str(), t.p[i], -1e12f, 1e12f)) return false;
                }
                t.text = r.get("text");
                break;
        }
        if (r.has("want") && !eqsTestIsBoolean(t)) return fail(r.line, "'want' applies to boolean tests only");
        return true;
    }
};

// ---- writer helpers ---------------------------------------------------------------------------------

void kv(std::string& s, const char* key, const std::string& val) {
    s += ' ';
    s += key;
    s += '=';
    s += quoteToken(val);
}
void kvf(std::string& s, const char* key, f32 v) { kv(s, key, fmtF32(v)); }
void kvu(std::string& s, const char* key, u32 v) { kv(s, key, std::to_string(v)); }
void kvb(std::string& s, const char* key, bool v) { kv(s, key, v ? "1" : "0"); }

const char* curveName(EqsCurve c) {
    switch (c) {
        case EqsCurve::Linear: return "linear";
        case EqsCurve::Inverse: return "inverse";
        case EqsCurve::Square: return "square";
        case EqsCurve::InvSquare: return "invsquare";
        case EqsCurve::Sqrt: return "sqrt";
        case EqsCurve::Step: return "step";
    }
    return "linear";
}

std::string pointsText(const std::vector<Vec3>& pts) {
    std::string r;
    for (usize i = 0; i < pts.size(); ++i) {
        if (i) r += ';';
        r += fmtF32(pts[i].x) + "," + fmtF32(pts[i].y) + "," + fmtF32(pts[i].z);
    }
    return r;
}

std::string writeGenerator(const EqsGenerator& g) {
    const EqsGenerator d = eqsDefaultGenerator(g.kind);
    std::string s = std::string("GENERATOR ") + eqsGenName(g.kind);
    if (g.centre != d.centre) kv(s, "centre", g.centre);
    switch (g.kind) {
        case EqsGen::Grid:
            if (g.halfX != d.halfX) kvf(s, "halfx", g.halfX);
            if (g.halfY != d.halfY) kvf(s, "halfy", g.halfY);
            if (g.step != d.step) kvf(s, "step", g.step);
            break;
        case EqsGen::Ring:
            if (g.inner != d.inner) kvf(s, "inner", g.inner);
            if (g.outer != d.outer) kvf(s, "outer", g.outer);
            if (g.rings != d.rings) kvu(s, "rings", g.rings);
            if (g.count != d.count) kvu(s, "count", g.count);
            if (g.arcStart != d.arcStart) kvf(s, "arcstart", g.arcStart);
            if (g.arcEnd != d.arcEnd) kvf(s, "arcend", g.arcEnd);
            if (!g.facing.empty()) kv(s, "facing", g.facing);
            break;
        case EqsGen::Around:
            if (g.radius != d.radius) kvf(s, "radius", g.radius);
            if (g.count != d.count) kvu(s, "count", g.count);
            if (g.arcEnd - g.arcStart != d.arcEnd - d.arcStart) kvf(s, "arc", g.arcEnd - g.arcStart);
            if (!g.facing.empty()) kv(s, "facing", g.facing);
            break;
        case EqsGen::Nav:
            if (g.radius != d.radius) kvf(s, "radius", g.radius);
            if (g.stride != d.stride) kvu(s, "stride", g.stride);
            if (g.sameRegion != d.sameRegion) kv(s, "region", g.sameRegion ? "same" : "any");
            break;
        case EqsGen::Actors:
            if (!g.className.empty()) kv(s, "class", g.className);
            if (g.tagBits != 0) kvu(s, "tagbits", g.tagBits);
            if (!g.namePrefix.empty()) kv(s, "named", g.namePrefix);
            if (g.radius != d.radius) kvf(s, "radius", g.radius);
            if (g.derived != d.derived) kvb(s, "derived", g.derived);
            if (g.excludeQuerier != d.excludeQuerier) kvb(s, "excludequerier", g.excludeQuerier);
            break;
        case EqsGen::Slots:
            if (!g.slotTags.v.empty()) kv(s, "tags", g.slotTags.csv());
            if (g.matchAll != d.matchAll) kvb(s, "matchall", g.matchAll);
            if (g.radius != d.radius) kvf(s, "radius", g.radius);
            if (g.freeOnly != d.freeOnly) kvb(s, "free", g.freeOnly);
            break;
        case EqsGen::Points:
            kv(s, "list", pointsText(g.points));
            break;
    }
    if (g.project != EqsProject::None) kv(s, "project", g.project == EqsProject::Nav ? "nav" : "ground");
    if (g.zOffset != d.zOffset) kvf(s, "zoffset", g.zOffset);
    if (g.maxItems != d.maxItems) kvu(s, "max", g.maxItems);
    return s;
}

std::string writeTest(const EqsTest& t) {
    const EqsTest d = eqsDefaultTest(t.kind);
    std::string s = std::string("TEST ") + eqsTestName(t.kind);
    switch (t.kind) {
        case EqsTestKind::Distance:
            if (t.to != d.to) kv(s, "to", t.to);
            if (t.metric != d.metric) kv(s, "metric", t.metric);
            break;
        case EqsTestKind::Dot:
            if (t.from != d.from) kv(s, "from", t.from);
            if (t.ref == "toward") kv(s, "ref", "toward:" + t.refCtx);
            else if (t.ref == "dir") kv(s, "ref", "dir:" + fmtF32(t.refDir.x) + "," + fmtF32(t.refDir.y) + "," + fmtF32(t.refDir.z));
            else if (t.ref != d.ref) kv(s, "ref", t.ref);
            if (t.metric != d.metric) kv(s, "metric", t.metric);
            break;
        case EqsTestKind::Trace:
            if (t.from != d.from) kv(s, "from", t.from);
            if (t.fromZ != d.fromZ) kvf(s, "fromz", t.fromZ);
            if (t.toZ != d.toZ) kvf(s, "toz", t.toZ);
            if (t.radius != d.radius) kvf(s, "radius", t.radius);
            if (t.gap != d.gap) kvf(s, "gap", t.gap);
            break;
        case EqsTestKind::Path:
            if (t.from != d.from) kv(s, "from", t.from);
            if (t.metric != d.metric) kv(s, "metric", t.metric);
            if (t.maxExp != d.maxExp) kvu(s, "maxexp", t.maxExp);
            if (t.partialOk) kv(s, "partial", "ok");
            break;
        case EqsTestKind::Tag:
            if (!t.tags.v.empty()) kv(s, "names", t.tags.csv());
            else kvu(s, "bits", t.bits);
            if (t.match != 0) kv(s, "match", t.match == 1 ? "all" : "none");
            break;
        case EqsTestKind::Random:
            if (t.seed != d.seed) kvu(s, "seed", t.seed);
            break;
        case EqsTestKind::Custom:
            kv(s, "name", t.custom);
            if (t.from != d.from) kv(s, "from", t.from);
            for (int i = 0; i < 4; ++i)
                if (t.p[i] != 0) kvf(s, i == 0 ? "p0" : i == 1 ? "p1" : i == 2 ? "p2" : "p3", t.p[i]);
            if (!t.text.empty()) kv(s, "text", t.text);
            break;
    }
    if (t.purpose != d.purpose) kv(s, "purpose", t.purpose == EqsPurpose::Filter ? "filter" : "score");
    if (t.weight != d.weight) kvf(s, "weight", t.weight);
    if (t.curve != d.curve) kv(s, "curve", curveName(t.curve));
    if (t.hasLo) kvf(s, "lo", t.lo);
    if (t.hasHi) kvf(s, "hi", t.hi);
    if (std::isfinite(t.min)) kvf(s, "min", t.min);
    if (std::isfinite(t.max)) kvf(s, "max", t.max);
    if (t.want != d.want) kvb(s, "want", t.want);
    if (t.kind != EqsTestKind::Custom && !t.name.empty()) kv(s, "name", t.name);
    return s;
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------

const char* eqsGenName(EqsGen k) {
    switch (k) {
        case EqsGen::Grid: return "grid";
        case EqsGen::Ring: return "ring";
        case EqsGen::Around: return "around";
        case EqsGen::Nav: return "nav";
        case EqsGen::Actors: return "actors";
        case EqsGen::Slots: return "slots";
        case EqsGen::Points: return "points";
    }
    return "grid";
}

const char* eqsTestName(EqsTestKind k) {
    switch (k) {
        case EqsTestKind::Distance: return "distance";
        case EqsTestKind::Dot: return "dot";
        case EqsTestKind::Trace: return "trace";
        case EqsTestKind::Path: return "path";
        case EqsTestKind::Tag: return "tag";
        case EqsTestKind::Random: return "random";
        case EqsTestKind::Custom: return "custom";
    }
    return "distance";
}

bool eqsTestIsBoolean(const EqsTest& t) {
    return t.kind == EqsTestKind::Trace || t.kind == EqsTestKind::Tag || (t.kind == EqsTestKind::Path && t.metric == "reachable");
}

EqsGenerator eqsDefaultGenerator(EqsGen kind) {
    EqsGenerator g;
    g.kind = kind;
    switch (kind) {
        case EqsGen::Around: g.radius = 400; g.count = 8; g.arcStart = -180; g.arcEnd = 180; break;
        case EqsGen::Actors: g.radius = 0; break;
        case EqsGen::Slots: g.radius = 1500; break;
        default: break;
    }
    return g;
}

EqsTest eqsDefaultTest(EqsTestKind kind) {
    EqsTest t;
    t.kind = kind;
    switch (kind) {
        case EqsTestKind::Distance: t.metric = "3d"; break;
        case EqsTestKind::Dot: t.metric = "3d"; break;
        case EqsTestKind::Path: t.metric = "length"; break;
        default: break;
    }
    return t;
}

bool parseEqsQuery(std::string_view text, const std::string& name, EqsQuery& out, std::string* why) {
    out = EqsQuery{};
    std::vector<TextRecord> recs;
    std::string rwhy;
    if (!readTextRecords(text, "OCEQS", 1, recs, &rwhy)) {
        if (why) *why = name + ":" + rwhy.substr(5);   // "line N: msg" -> "name:N: msg"
        return false;
    }
    Parser p;
    p.name = name;
    EqsQuery q;
    q.name = name;
    bool sawDesc = false, sawOutput = false, sawResult = false, sawRun = false;
    u32 optionLine = 0;
    bool haveGen = false;

    const auto bad = [&]() {
        if (why) *why = p.err;
        out = EqsQuery{};
        return false;
    };
    const auto closeOption = [&]() {
        if (!q.options.empty() && !haveGen) return p.fail(optionLine, "OPTION has no GENERATOR");
        return true;
    };

    for (const TextRecord& r : recs) {
        const std::string& k = r.keyword;
        if (k == "OPTION") {
            if (!closeOption()) return bad();
            if (q.options.size() >= kEqsMaxOptions) { p.fail(r.line, "more than 8 OPTIONs"); return bad(); }
            if (!r.args.empty() || !r.kv.empty()) { p.fail(r.line, "OPTION takes no arguments"); return bad(); }
            q.options.emplace_back();
            optionLine = r.line;
            haveGen = false;
        } else if (k == "GENERATOR") {
            if (q.options.empty()) { p.fail(r.line, "GENERATOR outside an OPTION"); return bad(); }
            if (haveGen) { p.fail(r.line, "second GENERATOR in one OPTION"); return bad(); }
            if (!p.parseGenerator(r, q.options.back().gen)) return bad();
            haveGen = true;
        } else if (k == "TEST") {
            if (q.options.empty() || !haveGen) { p.fail(r.line, "TEST needs an OPTION with a GENERATOR before it"); return bad(); }
            if (q.options.back().tests.size() >= kEqsMaxTests) { p.fail(r.line, "more than 16 TESTs in one OPTION"); return bad(); }
            EqsTest t;
            if (!p.parseTest(r, t)) return bad();
            q.options.back().tests.push_back(std::move(t));
        } else if (!q.options.empty()) {
            p.fail(r.line, "'" + k + "' must come before the first OPTION");
            return bad();
        } else if (k == "DESC") {
            if (sawDesc) { p.fail(r.line, "duplicate DESC"); return bad(); }
            sawDesc = true;
            for (usize i = 0; i < r.args.size(); ++i) q.desc += (i ? " " : "") + r.args[i];
        } else if (k == "OUTPUT") {
            if (sawOutput) { p.fail(r.line, "duplicate OUTPUT"); return bad(); }
            sawOutput = true;
            if (!r.args.empty() || !p.checkKeys(r, {"pos", "entity", "score", "count"}, "OUTPUT")) { if (p.err.empty()) p.fail(r.line, "OUTPUT takes key=value pairs"); return bad(); }
            for (const auto& e : r.kv) {
                if (!isIdent(e.second)) { p.fail(r.line, "'" + e.first + "' needs a blackboard key name"); return bad(); }
                (e.first == "pos" ? q.out.pos : e.first == "entity" ? q.out.entity : e.first == "score" ? q.out.score : q.out.count) = e.second;
            }
        } else if (k == "RESULT") {
            if (sawResult) { p.fail(r.line, "duplicate RESULT"); return bad(); }
            sawResult = true;
            if (r.args.empty() || !r.kv.empty()) { p.fail(r.line, "RESULT needs all, best, top <n>, percent <p> or randompercent <p>"); return bad(); }
            const std::string m = lowerStr(r.args[0]);
            if (m == "all" || m == "best") {
                if (r.args.size() != 1) { p.fail(r.line, "unexpected argument"); return bad(); }
                q.mode = m == "all" ? EqsResultMode::All : EqsResultMode::Best;
            } else if (m == "top") {
                i32 n = 0;
                if (r.args.size() != 2 || !parseI32(r.args[1], n) || n < 1 || n > static_cast<i32>(kEqsMaxItems)) { p.fail(r.line, "top needs a count of 1..16384"); return bad(); }
                q.mode = EqsResultMode::Top;
                q.modeN = static_cast<u32>(n);
            } else if (m == "percent" || m == "randompercent") {
                f32 pc = 0;
                if (r.args.size() != 2 || !parseF32(r.args[1], pc) || pc <= 0 || pc > 100) { p.fail(r.line, m + " needs a percentage above 0 and at most 100"); return bad(); }
                q.mode = m == "percent" ? EqsResultMode::Percent : EqsResultMode::RandomPercent;
                q.modeP = pc;
            } else {
                p.fail(r.line, "unknown RESULT '" + r.args[0] + "'");
                return bad();
            }
        } else if (k == "RUN") {
            if (sawRun) { p.fail(r.line, "duplicate RUN"); return bad(); }
            sawRun = true;
            const std::string m = r.args.size() == 1 && r.kv.empty() ? lowerStr(r.args[0]) : std::string();
            if (m == "firstvalid") q.runAll = false;
            else if (m == "all") q.runAll = true;
            else { p.fail(r.line, "RUN must be firstvalid or all"); return bad(); }
        } else if (k == "CONTEXT") {
            if (r.args.size() != 1 || !r.has("key") || r.kv.size() != 1) { p.fail(r.line, "CONTEXT needs <name> key=<BbKey>"); return bad(); }
            const std::string& cn = r.args[0];
            if (!isCtxName(cn)) { p.fail(r.line, "context names are [a-z][a-z0-9]*"); return bad(); }
            if (cn == "querier" || cn == "seen" || cn == "point") { p.fail(r.line, "'" + cn + "' is a built-in context"); return bad(); }
            if (!isIdent(r.get("key"))) { p.fail(r.line, "CONTEXT key is not a blackboard key name"); return bad(); }
            if (std::find(p.declared.begin(), p.declared.end(), cn) != p.declared.end()) { p.fail(r.line, "duplicate CONTEXT '" + cn + "'"); return bad(); }
            if (p.declared.size() >= kEqsMaxContexts) { p.fail(r.line, "more than 8 CONTEXTs"); return bad(); }
            p.declared.push_back(cn);
            q.contexts.emplace_back(cn, r.get("key"));
        } else {
            p.fail(r.line, "unknown record '" + k + "'");
            return bad();
        }
    }
    if (!closeOption()) return bad();
    if (q.options.empty()) { p.fail(recs.empty() ? 1 : recs.back().line, "the query has no OPTION"); return bad(); }
    if (q.runAll && q.mode == EqsResultMode::Best && q.options.size() >= 2) {
        p.fail(optionLine, "RUN all with RESULT best across several OPTIONs: use top, percent or RUN firstvalid");
        return bad();
    }
    out = std::move(q);
    return true;
}

std::string writeEqsQuery(const EqsQuery& q) {
    std::string s = "OCEQS 1\n";
    if (!q.desc.empty()) s += "DESC " + quoteToken(q.desc) + "\n";
    if (!q.out.pos.empty() || !q.out.entity.empty() || !q.out.score.empty() || !q.out.count.empty()) {
        s += "OUTPUT";
        if (!q.out.pos.empty()) kv(s, "pos", q.out.pos);
        if (!q.out.entity.empty()) kv(s, "entity", q.out.entity);
        if (!q.out.score.empty()) kv(s, "score", q.out.score);
        if (!q.out.count.empty()) kv(s, "count", q.out.count);
        s += "\n";
    }
    switch (q.mode) {
        case EqsResultMode::All: break;
        case EqsResultMode::Best: s += "RESULT best\n"; break;
        case EqsResultMode::Top: s += "RESULT top " + std::to_string(q.modeN) + "\n"; break;
        case EqsResultMode::Percent: s += "RESULT percent " + fmtF32(q.modeP) + "\n"; break;
        case EqsResultMode::RandomPercent: s += "RESULT randompercent " + fmtF32(q.modeP) + "\n"; break;
    }
    if (q.runAll) s += "RUN all\n";
    for (const auto& c : q.contexts) s += "CONTEXT " + c.first + " key=" + quoteToken(c.second) + "\n";
    for (const EqsOption& o : q.options) {
        s += "OPTION\n  " + writeGenerator(o.gen) + "\n";
        for (const EqsTest& t : o.tests) s += "  " + writeTest(t) + "\n";
    }
    return s;
}

bool loadEqsQuery(const std::string& path, EqsQuery& out, std::string* why) {
    out = EqsQuery{};
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) {
        if (why) *why = path + ": cannot open";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseEqsQuery(ss.str(), path, out, why);
}

bool saveEqsQuery(const std::string& path, const EqsQuery& in, std::string* why) {
    std::ofstream f(aver::nativePath(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = path + ": cannot write";
        return false;
    }
    const std::string text = writeEqsQuery(in);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (why) *why = path + ": write failed";
        return false;
    }
    return true;
}

std::vector<std::string> validateEqsQuery(const EqsQuery& q) {
    std::vector<std::string> w;
    const auto uses = [&](const std::string& ctx) {
        for (const EqsOption& o : q.options) {
            if (o.gen.centre == ctx || o.gen.facing == ctx) return true;
            for (const EqsTest& t : o.tests)
                if (t.from == ctx || t.to == ctx || t.refCtx == ctx) return true;
        }
        return false;
    };
    for (const auto& c : q.contexts)
        if (!uses(c.first)) w.push_back("context '" + c.first + "' is declared but never used");
    for (usize i = 0; i < q.options.size(); ++i) {
        const EqsOption& o = q.options[i];
        const std::string where = "option " + std::to_string(i + 1);
        if (o.tests.empty()) w.push_back(where + " has no tests: every item scores 1");
        bool scoring = false;
        for (const EqsTest& t : o.tests) {
            if (t.purpose != EqsPurpose::Filter) scoring = true;
            if (t.purpose != EqsPurpose::Filter && t.weight == 0) w.push_back(where + ": a scoring test has weight 0");
            if (t.hasLo && t.hasHi && t.hi == t.lo) w.push_back(where + ": a test has lo equal to hi, so its score is constant");
            if (t.kind == EqsTestKind::Custom && t.custom.empty()) w.push_back(where + ": a custom test has no name");
        }
        if (!scoring && !o.tests.empty() && q.mode != EqsResultMode::All) w.push_back(where + " only filters: RESULT picks among equal scores by generation order");
        if (o.gen.kind == EqsGen::Nav && o.gen.project == EqsProject::Nav) w.push_back(where + ": project=nav on a nav generator is redundant");
    }
    return w;
}

EqsQuery eqsStarter() {
    EqsQuery q;
    q.name = "NewQuery";
    q.desc = "Points around the querier, nearest first";
    q.mode = EqsResultMode::Top;
    q.modeN = 5;
    EqsOption o;
    o.gen = eqsDefaultGenerator(EqsGen::Ring);
    o.gen.inner = 200;
    o.gen.outer = 800;
    o.gen.rings = 2;
    o.gen.count = 12;
    EqsTest t = eqsDefaultTest(EqsTestKind::Distance);
    t.purpose = EqsPurpose::Score;
    t.curve = EqsCurve::Inverse;
    o.tests.push_back(t);
    q.options.push_back(std::move(o));
    return q;
}

} // namespace aver::synapse
