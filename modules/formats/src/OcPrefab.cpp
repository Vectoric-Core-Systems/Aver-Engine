// .ocprefab reader/writer and the prefab-instance records of .ocworld. See OcPrefab.hpp.
#include "aver/formats/OcPrefab.hpp"
#include "aver/formats/detail/TextEscape.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

constexpr const char* kKindNames[] = {"f32", "vec3", "quat", "i32", "bool", "i64", "entity", "string", "mat4"};
constexpr u32 kKindCount = 9;

bool kindFromName(std::string_view n, u32& out) {
    for (u32 k = 0; k < kKindCount; ++k)
        if (equalsCI(n, kKindNames[k])) { out = k; return true; }
    return false;
}

// The shortest of %.9g and %.17g that reads back as exactly `v`.
std::string numAuto(f64 v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    if (std::strtod(buf, nullptr) == v) return buf;
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

// strtof rather than from_chars: no dependence on the float overloads being available; every host here runs in the "C" locale.
f32 parseF32(std::string_view s, f32 dflt = 0.0f) {
    if (s.empty()) return dflt;
    const std::string tmp(s);
    char* end = nullptr;
    const f32 v = std::strtof(tmp.c_str(), &end);   // direct to f32: no double rounding on the way
    return end == tmp.c_str() ? dflt : v;
}

i64 parseI64(std::string_view s, i64 dflt = 0) {
    if (s.empty()) return dflt;
    const std::string tmp(s);
    char* end = nullptr;
    const long long v = std::strtoll(tmp.c_str(), &end, 10);
    return end == tmp.c_str() ? dflt : static_cast<i64>(v);
}

// "value of a field" as tokens, appended to `s` with a leading space.
void appendValue(std::string& s, const OcSaveField& f) {
    s += ' '; s += f.kind < kKindCount ? kKindNames[f.kind] : "i32";
    switch (f.kind) {
        case kOcPrefabKindF32: case kOcPrefabKindVec3: case kOcPrefabKindQuat: case kOcPrefabKindMat4: {
            const u32 n = ocSaveFloatCount(f.kind);
            for (u32 i = 0; i < n; ++i) { s += ' '; s += numF32(i < f.f.size() ? f.f[i] : 0.0f); }
            break;
        }
        case kOcPrefabKindEntity:
            s += ' '; s += std::to_string(f.i); s += ' '; s += strToken(f.s);
            break;
        case kOcPrefabKindString:
            s += ' '; s += strToken(f.s);
            break;
        default:   // I32, BOOL, I64
            s += ' '; s += std::to_string(f.i);
            break;
    }
}

// Reads the payload for `kind` starting at tokens[i]; advances i past it. False when short.
bool readPayload(const std::vector<std::string_view>& t, usize& i, u32 kind, OcSaveField& f) {
    f.kind = kind;
    switch (kind) {
        case kOcPrefabKindF32: case kOcPrefabKindVec3: case kOcPrefabKindQuat: case kOcPrefabKindMat4: {
            const u32 n = ocSaveFloatCount(kind);
            if (i + n > t.size()) return false;
            f.f.resize(n);
            for (u32 k = 0; k < n; ++k) f.f[k] = parseF32(t[i + k]);
            i += n;
            return true;
        }
        case kOcPrefabKindEntity:
            if (i + 2 > t.size()) return false;
            f.i = parseI64(t[i]);
            f.s = strFromToken(t[i + 1]);
            i += 2;
            return true;
        case kOcPrefabKindString:
            if (i + 1 > t.size()) return false;
            f.s = strFromToken(t[i]);
            i += 1;
            return true;
        default:
            if (i + 1 > t.size()) return false;
            f.i = parseI64(t[i]);
            i += 1;
            return true;
    }
}

std::string pathToken(const std::string& p) { return p.empty() ? std::string("-") : pctEncode(p); }
std::string pathFromToken(std::string_view t) { return t == "-" ? std::string() : pctDecode(t); }

// `<set|addcomp|rmcomp> <path|-> <component> [<field> <kind> <payload...>]` starting at t[from].
bool parseOverride(const std::vector<std::string_view>& t, usize from, OcPrefabOverride& ov) {
    if (from + 3 > t.size()) return false;
    ov = OcPrefabOverride{};
    ov.path = pathFromToken(t[from + 1]);
    ov.component = pctDecode(t[from + 2]);
    if (equalsCI(t[from], "set")) {
        ov.op = OcOverrideOp::Set;
        usize i = from + 3;
        if (i + 2 > t.size()) return false;
        ov.value.name = pctDecode(t[i]);
        u32 kind = 0;
        if (!kindFromName(t[i + 1], kind)) return false;
        i += 2;
        return readPayload(t, i, kind, ov.value);
    }
    if (equalsCI(t[from], "addcomp")) { ov.op = OcOverrideOp::AddComponent; return true; }
    if (equalsCI(t[from], "rmcomp"))  { ov.op = OcOverrideOp::RemoveComponent; return true; }
    return false;
}

void appendOverrideBody(std::string& s, const OcPrefabOverride& ov) {
    switch (ov.op) {
        case OcOverrideOp::Set:
            s += "set "; s += pathToken(ov.path); s += ' '; s += pctEncode(ov.component);
            s += ' '; s += pctEncode(ov.value.name);
            appendValue(s, ov.value);
            break;
        case OcOverrideOp::AddComponent:
            s += "addcomp "; s += pathToken(ov.path); s += ' '; s += pctEncode(ov.component);
            break;
        case OcOverrideOp::RemoveComponent:
            s += "rmcomp "; s += pathToken(ov.path); s += ' '; s += pctEncode(ov.component);
            break;
    }
}

} // namespace

// ---- helpers --------------------------------------------------------------------------------------

std::string ocPrefabJoinPath(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string r(a);
    r += '/';
    r += b;
    return r;
}

void ocPrefabSplitPath(std::string_view path, std::string& head, std::string& rest) {
    const usize slash = path.find('/');
    if (slash == std::string_view::npos) { head = std::string(path); rest.clear(); return; }
    head = std::string(path.substr(0, slash));
    rest = std::string(path.substr(slash + 1));
}

bool ocPrefabFieldEqual(const OcSaveField& a, const OcSaveField& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case kOcPrefabKindF32: case kOcPrefabKindVec3: case kOcPrefabKindQuat: case kOcPrefabKindMat4:
            return a.f.size() == b.f.size() &&
                   (a.f.empty() || std::memcmp(a.f.data(), b.f.data(), a.f.size() * sizeof(f32)) == 0);
        case kOcPrefabKindString:
            return a.s == b.s;
        case kOcPrefabKindEntity:
            // -1 is "none" and carries no path; anything else is the path.
            return a.i == b.i && (a.i < 0 || a.s == b.s);
        default:
            return a.i == b.i;
    }
}

bool ocPrefabSameTarget(const OcPrefabOverride& a, const OcPrefabOverride& b) {
    if (a.op != b.op || a.path != b.path || a.component != b.component) return false;
    return a.op != OcOverrideOp::Set || a.value.name == b.value.name;
}

// ---- OcPrefabData -----------------------------------------------------------------------------------

const OcPrefabNode* OcPrefabData::find(u32 uid) const {
    for (const OcPrefabNode& n : nodes) if (n.uid == uid) return &n;
    return nullptr;
}

OcPrefabNode* OcPrefabData::find(u32 uid) {
    for (OcPrefabNode& n : nodes) if (n.uid == uid) return &n;
    return nullptr;
}

u32 OcPrefabData::rootUid() const {
    for (const OcPrefabNode& n : nodes) if (n.parent == 0) return n.uid;
    return 0;
}

bool OcPrefabData::valid(std::string* why) const {
    const auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (nodes.empty()) return fail("prefab has no nodes");
    std::unordered_set<u32> seen;
    u32 roots = 0, maxUid = 0;
    for (const OcPrefabNode& n : nodes) {
        if (n.uid == 0) return fail("a node has uid 0");
        if (seen.count(n.uid)) return fail("duplicate node uid");
        if (n.parent == 0) ++roots;
        else if (!seen.count(n.parent)) return fail("a node's parent is not defined before it");
        seen.insert(n.uid);
        maxUid = std::max(maxUid, n.uid);
    }
    if (roots != 1) return fail("a prefab needs exactly one root node");
    if (nextUid <= maxUid) return fail("nextUid is not past every node uid");
    return true;
}

// ---- the asset --------------------------------------------------------------------------------------

bool parseOcPrefab(std::string_view text, OcPrefabData& out, std::string* err) {
    out = OcPrefabData{};
    bool sawHeader = false;
    OcPrefabNode* node = nullptr;
    OcSaveComponent* comp = nullptr;
    // Nodes are appended to a vector, so a pointer into it must be re-derived after a push_back.
    const auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;
        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];

        if (equalsCI(key, "OCPREFAB")) {
            out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            out.name = t.size() > 1 ? strFromToken(t[1]) : std::string();
        } else if (equalsCI(key, "NEXTUID")) {
            out.nextUid = t.size() > 1 ? static_cast<u32>(parseU64(t[1], 1)) : 1;
        } else if (equalsCI(key, "NODE")) {
            if (t.size() < 2) return fail("NODE without a uid");
            OcPrefabNode n;
            n.uid = static_cast<u32>(parseU64(t[1]));
            for (usize i = 2; i + 1 < t.size(); i += 2) {
                if      (equalsCI(t[i], "parent")) n.parent = static_cast<u32>(parseU64(t[i + 1]));
                else if (equalsCI(t[i], "name"))   n.name = strFromToken(t[i + 1]);
                else if (equalsCI(t[i], "class"))  n.className = strFromToken(t[i + 1]);
                else if (equalsCI(t[i], "prefab")) n.prefab = strFromToken(t[i + 1]);
            }
            out.nodes.push_back(std::move(n));
            node = &out.nodes.back();
            comp = nullptr;
        } else if (equalsCI(key, "COMP")) {
            if (!node || t.size() < 2) continue;
            node->components.emplace_back();
            comp = &node->components.back();
            comp->type = pctDecode(t[1]);
        } else if (equalsCI(key, "F")) {
            if (!comp || t.size() < 4) continue;
            OcSaveField f;
            f.name = pctDecode(t[1]);
            u32 kind = 0;
            if (!kindFromName(t[2], kind)) continue;
            usize i = 3;
            if (!readPayload(t, i, kind, f)) continue;
            comp->fields.push_back(std::move(f));
        } else if (equalsCI(key, "OVERRIDE")) {
            if (!node) continue;
            OcPrefabOverride ov;
            if (parseOverride(t, 1, ov)) node->overrides.push_back(std::move(ov));
        }
        // Unknown records are skipped, like every other text format here.
    }

    if (!sawHeader) return fail("not an .ocprefab file (no OCPREFAB header line)");
    u32 maxUid = 0;
    for (const OcPrefabNode& n : out.nodes) maxUid = std::max(maxUid, n.uid);
    if (out.nextUid <= maxUid) out.nextUid = maxUid + 1;
    std::string why;
    if (!out.valid(&why)) return fail("invalid .ocprefab: " + why);
    return true;
}

bool loadOcPrefab(const std::string& path, OcPrefabData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcPrefab(text, out, err);
}

std::string writeOcPrefab(const OcPrefabData& p) {
    std::string s;
    s.reserve(256 + p.nodes.size() * 160);
    s += "OCPREFAB 1\n";
    s += "# Written by the Aver Engine editor. Components and fields by name; paths are node uids.\n";
    s += "NAME "; s += strToken(p.name); s += '\n';
    s += "NEXTUID "; s += std::to_string(p.nextUid); s += '\n';
    for (const OcPrefabNode& n : p.nodes) {
        s += "NODE "; s += std::to_string(n.uid);
        s += " parent "; s += std::to_string(n.parent);
        s += " name "; s += strToken(n.name);
        if (!n.className.empty()) { s += " class "; s += strToken(n.className); }
        if (!n.prefab.empty())    { s += " prefab "; s += strToken(n.prefab); }
        s += '\n';
        for (const OcSaveComponent& c : n.components) {
            s += "  COMP "; s += pctEncode(c.type); s += '\n';
            for (const OcSaveField& f : c.fields) {
                s += "    F "; s += pctEncode(f.name);
                appendValue(s, f);
                s += '\n';
            }
        }
        for (const OcPrefabOverride& ov : n.overrides) {
            s += "  OVERRIDE ";
            appendOverrideBody(s, ov);
            s += '\n';
        }
    }
    return s;
}

bool saveOcPrefab(const std::string& path, const OcPrefabData& p, std::string* err) {
    std::error_code ec;
    const std::filesystem::path fp(path);
    if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    if (!writeFileTextAtomic(path, writeOcPrefab(p))) {
        if (err) *err = "could not write " + path;
        return false;
    }
    return true;
}

// ---- the level records --------------------------------------------------------------------------------

bool parseOcPrefabInstanceLine(const std::vector<std::string_view>& t,
                               std::vector<OcPrefabInstance>& out, i32& open) {
    if (t.empty()) return false;
    if (equalsCI(t[0], "PREFABINST")) {
        OcPrefabInstance inst;
        if (t.size() >= 2) inst.prefab = pctDecode(t[1]);
        for (usize i = 2; i < t.size(); ++i) {
            if (equalsCI(t[i], "pos") && i + 3 < t.size()) {
                inst.x = parseF64(t[i + 1]); inst.y = parseF64(t[i + 2]); inst.z = parseF64(t[i + 3]); i += 3;
            } else if (equalsCI(t[i], "rot") && i + 3 < t.size()) {
                inst.yaw = parseF64(t[i + 1]); inst.pitch = parseF64(t[i + 2]); inst.roll = parseF64(t[i + 3]); i += 3;
            } else if (equalsCI(t[i], "scale") && i + 3 < t.size()) {
                inst.sx = parseF64(t[i + 1], 1.0); inst.sy = parseF64(t[i + 2], 1.0); inst.sz = parseF64(t[i + 3], 1.0); i += 3;
            } else if (equalsCI(t[i], "name") && i + 1 < t.size()) {
                inst.name = strFromToken(t[++i]);
            }
        }
        open = static_cast<i32>(out.size());
        out.push_back(std::move(inst));
        return true;
    }
    if (equalsCI(t[0], "POVERRIDE")) {
        if (open >= 0 && static_cast<usize>(open) < out.size()) {
            OcPrefabOverride ov;
            if (parseOverride(t, 1, ov)) out[static_cast<usize>(open)].overrides.push_back(std::move(ov));
        }
        return true;
    }
    if (equalsCI(t[0], "ENDPREFABINST")) {
        open = -1;
        return true;
    }
    return false;
}

void appendOcPrefabInstances(std::string& out, const std::vector<OcPrefabInstance>& instances) {
    if (instances.empty()) return;
    out += '\n';
    for (const OcPrefabInstance& in : instances) {
        out += "PREFABINST "; out += pctEncode(in.prefab);
        out += " pos " + numAuto(in.x) + " " + numAuto(in.y) + " " + numAuto(in.z);
        out += " rot " + numAuto(in.yaw) + " " + numAuto(in.pitch) + " " + numAuto(in.roll);
        out += " scale " + numAuto(in.sx) + " " + numAuto(in.sy) + " " + numAuto(in.sz);
        if (!in.name.empty()) { out += " name "; out += strToken(in.name); }
        out += '\n';
        for (const OcPrefabOverride& ov : in.overrides) {
            out += "  POVERRIDE ";
            appendOverrideBody(out, ov);
            out += '\n';
        }
        out += "ENDPREFABINST\n";
    }
}

} // namespace aver::fmt
