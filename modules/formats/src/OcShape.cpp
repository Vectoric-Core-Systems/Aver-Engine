// .ocshape text reader and writer. See OcShape.hpp for what the format is.
#include "aver/formats/OcShape.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/core/Log.hpp"
#include "aver/formats/OcSurf.hpp"  // ocSurfNameValid: a shape names surfaces by the same rule
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_map>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// WHOLE TOKEN OR NOTHING (TextScan's parsers accept a numeric prefix, which turns a typo into a number nobody wrote).
bool parseWholeF32(std::string_view s, f32& out) {
    const char* const end = s.data() + s.size();
    const auto r = aver::fromChars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end && std::isfinite(out);
}

bool parseWholeInt(std::string_view s, i32& out) {
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

// The shortest text that reads back as the SAME f32, in fixed notation.
void appendNum(std::string& s, f32 v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    s.append(buf, r.ptr);
}

void appendTriple(std::string& s, const f32* v) {
    for (int i = 0; i < 3; ++i) { s += ' '; appendNum(s, v[i]); }
}

bool isIdentityRot(const f32* q) { return q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f && q[3] == 1.0f; }
bool isZero3(const f32* v) { return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f; }

struct KindWord { const char* word; ShapeNodeKind kind; };
constexpr KindWord kKindWords[] = {
    {"box", ShapeNodeKind::Box},           {"sphere", ShapeNodeKind::Sphere},       {"capsule", ShapeNodeKind::Capsule},
    {"cylinder", ShapeNodeKind::Cylinder}, {"tcapsule", ShapeNodeKind::TaperedCapsule}, {"tcylinder", ShapeNodeKind::TaperedCylinder},
    {"triangle", ShapeNodeKind::Triangle}, {"plane", ShapeNodeKind::Plane},         {"empty", ShapeNodeKind::Empty},
    {"hullpoints", ShapeNodeKind::HullPoints}, {"hull", ShapeNodeKind::HullMesh},   {"mesh", ShapeNodeKind::Mesh},
    {"rt", ShapeNodeKind::RotatedTranslated}, {"com", ShapeNodeKind::OffsetCom},    {"scale", ShapeNodeKind::Scaled},
    {"compound", ShapeNodeKind::Compound},
};

bool kindFromWord(std::string_view w, ShapeNodeKind& out) {
    for (const KindWord& k : kKindWords)
        if (equalsCI(w, k.word)) { out = k.kind; return true; }
    return false;
}

struct ParseState {
    bool warned = false;
};

void warnOnce(ParseState& st, const std::string& what) {
    if (st.warned) return;
    st.warned = true;
    AVER_WARN("[ocshape] {} (a newer file?)", what);
}

// `at x y z`, `rot qx qy qz qw` and `user <n>` words, in any order, from t[i] on. A null pointer makes that word an error
// (a decorator takes `at` and `rot` only; CHILD takes all three).
bool parsePoseWords(const std::vector<std::string_view>& t, usize i, f32* pos, f32* rot, i32* user, std::string& why) {
    while (i < t.size()) {
        const std::string_view w = t[i];
        if (equalsCI(w, "at") && pos) {
            if (i + 3 >= t.size()) { why = "'at' needs three numbers"; return false; }
            for (int c = 0; c < 3; ++c)
                if (!parseWholeF32(t[i + 1 + static_cast<usize>(c)], pos[c])) { why = "bad number '" + std::string(t[i + 1 + static_cast<usize>(c)]) + "' after 'at'"; return false; }
            i += 4;
        } else if (equalsCI(w, "rot") && rot) {
            if (i + 4 >= t.size()) { why = "'rot' needs four numbers"; return false; }
            for (int c = 0; c < 4; ++c)
                if (!parseWholeF32(t[i + 1 + static_cast<usize>(c)], rot[c])) { why = "bad number '" + std::string(t[i + 1 + static_cast<usize>(c)]) + "' after 'rot'"; return false; }
            i += 5;
        } else if (equalsCI(w, "user") && user) {
            if (i + 1 >= t.size() || !parseWholeInt(t[i + 1], *user)) { why = "'user' needs an integer"; return false; }
            i += 2;
        } else {
            why = "unexpected '" + std::string(w) + "'";
            return false;
        }
    }
    return true;
}

// Trailing `key value` options of a leaf, from t[i] on.
bool parseLeafOptions(const std::vector<std::string_view>& t, usize i, ShapeNode& n, ParseState& st, std::string& why) {
    while (i < t.size()) {
        const std::string_view key = t[i];
        const bool known = equalsCI(key, "radius") || equalsCI(key, "density") || equalsCI(key, "tolerance") || equalsCI(key, "edge") ||
                           equalsCI(key, "quality") || equalsCI(key, "surface");
        if (!known) {
            warnOnce(st, "unknown option '" + std::string(key) + "' skipped");
            i += 2;
            continue;
        }
        if (i + 1 >= t.size()) { why = "'" + std::string(key) + "' has no value"; return false; }
        const std::string_view value = t[i + 1];
        if (equalsCI(key, "surface")) {
            n.surface = std::string(value);
        } else if (equalsCI(key, "quality")) {
            if (equalsCI(value, "runtime")) n.quality = 0;
            else if (equalsCI(value, "build")) n.quality = 1;
            else { why = "quality is 'runtime' or 'build', not '" + std::string(value) + "'"; return false; }
        } else {
            f32 v = 0.0f;
            if (!parseWholeF32(value, v)) { why = "bad " + std::string(key) + " '" + std::string(value) + "'"; return false; }
            if (equalsCI(key, "radius")) n.radius = v;
            else if (equalsCI(key, "density")) n.density = v;
            else if (equalsCI(key, "tolerance")) n.tolerance = v;
            else { n.hasEdge = true; n.edge = v; }
        }
        i += 2;
    }
    return true;
}

bool parseNodeRecord(const std::vector<std::string_view>& t, ShapeNode& n, ParseState& st, std::string& why) {
    if (t.size() < 3) { why = "NODE needs an id and a kind"; return false; }
    n.id = std::string(t[1]);
    if (!ocShapeIdValid(n.id)) { why = "bad node id '" + n.id + "' (letters, digits and underscore, 1 to 63)"; return false; }
    if (!kindFromWord(t[2], n.kind)) { why = "unknown node kind '" + std::string(t[2]) + "'"; return false; }

    usize i = 3;
    if (shapeKindIsLeaf(n.kind)) {
        if (n.kind == ShapeNodeKind::HullMesh) {
            if (t.size() < 5 || !equalsCI(t[3], "mesh")) { why = "'hull' is followed by 'mesh <path>'"; return false; }
            n.path = std::string(t[4]);
            i = 5;
        } else if (n.kind == ShapeNodeKind::Mesh) {
            if (t.size() < 4) { why = "'mesh' needs a content path"; return false; }
            n.path = std::string(t[3]);
            i = 4;
        } else if (n.kind == ShapeNodeKind::HullPoints) {
            f32 v = 0.0f;
            while (i < t.size() && parseWholeF32(t[i], v)) {
                if (n.nums.size() >= 3u * kOcShapeMaxChildren) { why = "too many hull points"; return false; }
                n.nums.push_back(v);
                ++i;
            }
        } else {
            const int need = shapeKindNumberCount(n.kind);
            if (t.size() < 3 + static_cast<usize>(need)) { why = std::string(shapeKindWord(n.kind)) + " needs " + std::to_string(need) + " number(s)"; return false; }
            n.nums.resize(static_cast<usize>(need));
            for (int c = 0; c < need; ++c)
                if (!parseWholeF32(t[3 + static_cast<usize>(c)], n.nums[static_cast<usize>(c)])) {
                    why = "bad number '" + std::string(t[3 + static_cast<usize>(c)]) + "' for " + std::string(shapeKindWord(n.kind));
                    return false;
                }
            i = 3 + static_cast<usize>(need);
        }
        return parseLeafOptions(t, i, n, st, why);
    }

    if (n.kind == ShapeNodeKind::Compound) {
        if (t.size() != 4) { why = "'compound' is followed by 'static' or 'mutable'"; return false; }
        if (equalsCI(t[3], "static")) n.mutableCompound = false;
        else if (equalsCI(t[3], "mutable")) n.mutableCompound = true;
        else { why = "a compound is 'static' or 'mutable', not '" + std::string(t[3]) + "'"; return false; }
        return true;
    }

    // Decorators: a child id first.
    if (t.size() < 4) { why = std::string(shapeKindWord(n.kind)) + " needs a child node"; return false; }
    n.child = std::string(t[3]);
    if (n.kind == ShapeNodeKind::RotatedTranslated) return parsePoseWords(t, 4, n.pos, n.rot, nullptr, why);
    if (n.kind == ShapeNodeKind::OffsetCom) {
        if (t.size() != 8 || !equalsCI(t[4], "at")) { why = "'com' is followed by a child and 'at x y z'"; return false; }
        for (int c = 0; c < 3; ++c)
            if (!parseWholeF32(t[5 + static_cast<usize>(c)], n.pos[c])) { why = "bad number '" + std::string(t[5 + static_cast<usize>(c)]) + "'"; return false; }
        return true;
    }
    if (t.size() != 7) { why = "'scale' is followed by a child and three numbers"; return false; }
    for (int c = 0; c < 3; ++c)
        if (!parseWholeF32(t[4 + static_cast<usize>(c)], n.scale[c])) { why = "bad number '" + std::string(t[4 + static_cast<usize>(c)]) + "'"; return false; }
    return true;
}

bool parseChildRecord(const std::vector<std::string_view>& t, ShapeChildRec& c, std::string& why) {
    if (t.size() < 3) { why = "CHILD needs a compound and a node"; return false; }
    c.compound = std::string(t[1]);
    c.node = std::string(t[2]);
    return parsePoseWords(t, 3, c.pos, c.rot, &c.user, why);
}

void leafOptionsOut(std::string& s, const ShapeNode& n) {
    if (n.radius >= 0.0f)    { s += " radius ";    appendNum(s, n.radius); }
    if (n.density >= 0.0f)   { s += " density ";   appendNum(s, n.density); }
    if (n.tolerance >= 0.0f) { s += " tolerance "; appendNum(s, n.tolerance); }
    if (n.hasEdge)           { s += " edge ";      appendNum(s, n.edge); }
    if (n.quality == 0)      s += " quality runtime";
    else if (n.quality == 1) s += " quality build";
    if (!n.surface.empty())  s += " surface " + n.surface;
}

} // namespace

const char* shapeKindWord(ShapeNodeKind k) {
    for (const KindWord& w : kKindWords)
        if (w.kind == k) return w.word;
    return "box";
}

int shapeKindNumberCount(ShapeNodeKind k) {
    switch (k) {
        case ShapeNodeKind::Box: return 3;
        case ShapeNodeKind::Sphere: return 1;
        case ShapeNodeKind::Capsule: return 2;
        case ShapeNodeKind::Cylinder: return 2;
        case ShapeNodeKind::TaperedCapsule: return 3;
        case ShapeNodeKind::TaperedCylinder: return 3;
        case ShapeNodeKind::Triangle: return 9;
        case ShapeNodeKind::Plane: return 5;
        case ShapeNodeKind::Empty: return 3;
        default: return -1;
    }
}

const ShapeNode* OcShapeData::find(std::string_view id) const {
    for (const ShapeNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

ShapeNode* OcShapeData::find(std::string_view id) {
    for (ShapeNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

bool ocShapeIdValid(std::string_view id) {
    if (id.empty() || id.size() > 63) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

namespace {

bool finite3(const f32* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The ABI's range for one leaf, as a sentence; empty when it is fine. The same limits the builders enforce.
std::string leafProblem(const ShapeNode& n) {
    const int need = shapeKindNumberCount(n.kind);
    if (need >= 0 && n.nums.size() != static_cast<usize>(need))
        return std::string(shapeKindWord(n.kind)) + " takes " + std::to_string(need) + " number(s), not " + std::to_string(n.nums.size());
    for (const f32 v : n.nums)
        if (!std::isfinite(v)) return "a number is not finite";
    const std::vector<f32>& a = n.nums;
    switch (n.kind) {
        case ShapeNodeKind::Box:
            if (!(a[0] > 0.0f && a[1] > 0.0f && a[2] > 0.0f)) return "a box needs positive half extents";
            break;
        case ShapeNodeKind::Sphere:
            if (!(a[0] > 0.0f)) return "a sphere needs a positive radius";
            break;
        case ShapeNodeKind::Capsule:
            if (!(a[0] > 0.0f) || !(a[1] > 2.0f * a[0])) return "a capsule needs a positive radius and a total height above twice the radius";
            break;
        case ShapeNodeKind::Cylinder:
            if (!(a[0] > 0.0f) || !(a[1] > 0.0f)) return "a cylinder needs a positive radius and height";
            break;
        case ShapeNodeKind::TaperedCapsule:
            if (!(a[0] > 0.0f) || !(a[1] > 0.0f) || !(a[2] > a[0] + a[1])) return "a tapered capsule needs positive radii and a total height above their sum";
            break;
        case ShapeNodeKind::TaperedCylinder:
            if (a[0] < 0.0f || a[1] < 0.0f || (a[0] <= 0.0f && a[1] <= 0.0f) || !(a[2] > 0.0f)) return "a tapered cylinder needs non-negative radii (not both zero) and a positive height";
            break;
        case ShapeNodeKind::Triangle: {
            const f32 ux = a[3] - a[0], uy = a[4] - a[1], uz = a[5] - a[2];
            const f32 vx = a[6] - a[0], vy = a[7] - a[1], vz = a[8] - a[2];
            const f32 cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            if (cx * cx + cy * cy + cz * cz < 1.0e-4f) return "the three triangle vertices are collinear";
            break;
        }
        case ShapeNodeKind::Plane:
            if (std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) < 1.0e-6f) return "a plane needs a non-zero normal";
            if (!(a[4] >= 1.0f && a[4] <= 1.0e6f)) return "a plane's half extent is 1 to 1e6 cm";
            break;
        case ShapeNodeKind::Empty:
            break;
        case ShapeNodeKind::HullPoints:
            if (n.nums.size() % 3 != 0) return "hullpoints takes three numbers per point";
            if (n.nums.size() < 12) return "a convex hull needs at least four points";
            break;
        case ShapeNodeKind::HullMesh:
        case ShapeNodeKind::Mesh:
            if (n.path.empty()) return "a mesh leaf needs a content path";
            break;
        default: break;
    }
    if (n.density == 0.0f) return "density must be positive";
    if (!std::isfinite(n.radius) || !std::isfinite(n.density) || !std::isfinite(n.tolerance) || !std::isfinite(n.edge)) return "an option is not finite";
    if (!n.surface.empty() && !ocSurfNameValid(n.surface)) return "bad surface name '" + n.surface + "'";
    return {};
}

} // namespace

std::vector<std::string> validateOcShape(const OcShapeData& d) {
    std::vector<std::string> out;
    const auto add = [&](std::string s) { if (out.size() < 64) out.push_back(std::move(s)); };

    if (!std::isfinite(d.massKg) || d.massKg < 0.0f) add("MASS must be zero or positive");
    if (!d.surface.empty() && !ocSurfNameValid(d.surface)) add("bad SURFACE name '" + d.surface + "'");
    if (d.nodes.empty()) add("the shape has no nodes");
    if (d.nodes.size() > kOcShapeMaxNodes) add("more than " + std::to_string(kOcShapeMaxNodes) + " nodes");
    if (d.children.size() > kOcShapeMaxChildren) add("more than " + std::to_string(kOcShapeMaxChildren) + " CHILD records");

    std::unordered_map<std::string, u32> index;
    for (u32 i = 0; i < d.nodes.size(); ++i) {
        const ShapeNode& n = d.nodes[i];
        if (!ocShapeIdValid(n.id)) add("bad node id '" + n.id + "' (letters, digits and underscore, 1 to 63)");
        else if (!index.emplace(n.id, i).second) add("node '" + n.id + "' is declared twice");
    }

    // Per node: its own numbers and its references.
    std::vector<u32> refs(d.nodes.size(), 0);
    std::vector<u32> childCount(d.nodes.size(), 0);
    const auto lookup = [&](const std::string& id) -> i64 {
        const auto it = index.find(id);
        return it == index.end() ? -1 : static_cast<i64>(it->second);
    };
    for (const ShapeNode& n : d.nodes) {
        if (shapeKindIsLeaf(n.kind)) {
            if (const std::string p = leafProblem(n); !p.empty()) add("node '" + n.id + "': " + p);
            continue;
        }
        if (n.kind == ShapeNodeKind::Compound) continue;
        const i64 c = lookup(n.child);
        if (c < 0) add("node '" + n.id + "' names an undefined child '" + n.child + "'");
        else ++refs[static_cast<usize>(c)];
        if (!finite3(n.pos)) add("node '" + n.id + "': a position is not finite");
        if (n.kind == ShapeNodeKind::RotatedTranslated) {
            const f32 len2 = n.rot[0] * n.rot[0] + n.rot[1] * n.rot[1] + n.rot[2] * n.rot[2] + n.rot[3] * n.rot[3];
            if (!std::isfinite(len2) || len2 < 1.0e-12f) add("node '" + n.id + "': a zero or non-finite rotation");
        }
        if (n.kind == ShapeNodeKind::Scaled) {
            for (int c2 = 0; c2 < 3; ++c2)
                if (!std::isfinite(n.scale[c2]) || std::fabs(n.scale[c2]) < 1.0e-4f) { add("node '" + n.id + "': a scale component is zero or not finite"); break; }
        }
    }
    for (const ShapeChildRec& c : d.children) {
        const i64 p = lookup(c.compound);
        const i64 k = lookup(c.node);
        if (p < 0) { add("CHILD names an undefined compound '" + c.compound + "'"); continue; }
        if (d.nodes[static_cast<usize>(p)].kind != ShapeNodeKind::Compound) { add("CHILD '" + c.compound + "' is not a compound"); continue; }
        ++childCount[static_cast<usize>(p)];
        if (k < 0) { add("CHILD of '" + c.compound + "' names an undefined node '" + c.node + "'"); continue; }
        ++refs[static_cast<usize>(k)];
        if (!finite3(c.pos)) add("CHILD '" + c.node + "' of '" + c.compound + "': a position is not finite");
        const f32 len2 = c.rot[0] * c.rot[0] + c.rot[1] * c.rot[1] + c.rot[2] * c.rot[2] + c.rot[3] * c.rot[3];
        if (!std::isfinite(len2) || len2 < 1.0e-12f) add("CHILD '" + c.node + "' of '" + c.compound + "': a zero or non-finite rotation");
    }
    for (u32 i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].kind == ShapeNodeKind::Compound && (childCount[i] < 1 || childCount[i] > kOcShapeMaxChildren))
            add("compound '" + d.nodes[i].id + "' needs 1 to " + std::to_string(kOcShapeMaxChildren) + " children (it has " + std::to_string(childCount[i]) + ")");

    if (d.root.empty()) add("there is no ROOT");
    else if (const i64 r = lookup(d.root); r < 0) add("ROOT names an undefined node '" + d.root + "'");
    else ++refs[static_cast<usize>(r)];

    for (u32 i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].kind == ShapeNodeKind::Compound && d.nodes[i].mutableCompound && refs[i] > 1)
            add("mutable compound '" + d.nodes[i].id + "' is used " + std::to_string(refs[i]) + " times; it belongs to one body");

    // Cycles: a depth-first walk over decorator and CHILD edges.
    std::vector<std::vector<u32>> edges(d.nodes.size());
    for (u32 i = 0; i < d.nodes.size(); ++i)
        if (shapeKindIsDecorator(d.nodes[i].kind))
            if (const i64 c = lookup(d.nodes[i].child); c >= 0) edges[i].push_back(static_cast<u32>(c));
    for (const ShapeChildRec& c : d.children) {
        const i64 p = lookup(c.compound), k = lookup(c.node);
        if (p >= 0 && k >= 0) edges[static_cast<usize>(p)].push_back(static_cast<u32>(k));
    }
    std::vector<u8> colour(d.nodes.size(), 0);   // 0 new, 1 on the path, 2 done
    bool cycle = false;
    for (u32 s = 0; s < d.nodes.size() && !cycle; ++s) {
        if (colour[s]) continue;
        std::vector<std::pair<u32, usize>> stack{{s, 0}};
        colour[s] = 1;
        while (!stack.empty() && !cycle) {
            auto& [node, next] = stack.back();
            if (next < edges[node].size()) {
                const u32 to = edges[node][next++];
                if (colour[to] == 1) { add("node '" + d.nodes[to].id + "' is part of a cycle"); cycle = true; }
                else if (colour[to] == 0) { colour[to] = 1; stack.emplace_back(to, 0); }
            } else {
                colour[node] = 2;
                stack.pop_back();
            }
        }
    }
    return out;
}

std::vector<u32> ocShapeBuildOrder(const OcShapeData& d) {
    std::vector<u32> order;
    std::unordered_map<std::string, u32> index;
    for (u32 i = 0; i < d.nodes.size(); ++i) index.emplace(d.nodes[i].id, i);
    const auto rootIt = index.find(d.root);
    if (rootIt == index.end()) return order;

    std::unordered_map<std::string, std::vector<u32>> kids;   // compound id -> children in file order
    for (const ShapeChildRec& c : d.children)
        if (const auto it = index.find(c.node); it != index.end()) kids[c.compound].push_back(it->second);

    std::vector<u8> colour(d.nodes.size(), 0);
    std::vector<std::pair<u32, usize>> stack{{rootIt->second, 0}};
    colour[rootIt->second] = 1;
    const auto edgesOf = [&](u32 n, std::vector<u32>& tmp) -> const std::vector<u32>& {
        const ShapeNode& node = d.nodes[n];
        tmp.clear();
        if (shapeKindIsDecorator(node.kind)) {
            if (const auto it = index.find(node.child); it != index.end()) tmp.push_back(it->second);
        } else if (node.kind == ShapeNodeKind::Compound) {
            if (const auto it = kids.find(node.id); it != kids.end()) return it->second;
        }
        return tmp;
    };
    std::vector<u32> scratch;
    while (!stack.empty()) {
        const u32 node = stack.back().first;
        const std::vector<u32>& e = edgesOf(node, scratch);
        const usize at = stack.back().second;
        if (at < e.size()) {
            const u32 to = e[at];
            ++stack.back().second;
            if (colour[to] == 0) { colour[to] = 1; stack.emplace_back(to, 0); }
        } else {
            colour[node] = 2;
            order.push_back(node);
            stack.pop_back();
        }
    }
    return order;
}

bool parseOcShape(std::string_view text, OcShapeData& out, std::string* err) {
    out = OcShapeData{};

    OcShapeData d;
    ParseState st;
    bool sawHeader = false;

    int lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        const std::string_view line = trim(truncateHash(rawLine));   // trim drops the CR a CRLF file leaves
        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string at = " (line " + std::to_string(lineNo) + ")";

        if (equalsCI(t[0], "OCSHAPE")) {
            if (sawHeader) return fail(err, "a second OCSHAPE header" + at);
            if (t.size() < 2) return fail(err, "OCSHAPE needs a version" + at);
            i32 version = 0;
            if (!parseWholeInt(t[1], version)) return fail(err, "bad .ocshape version '" + std::string(t[1]) + "'" + at);
            if (version > 1) return fail(err, "this .ocshape is version " + std::to_string(version) + ", newer than this engine reads (1)" + at);
            if (version < 1) return fail(err, "unsupported .ocshape version '" + std::string(t[1]) + "'" + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(err, "the first record must be OCSHAPE 1" + at);

        std::string why;
        if (equalsCI(t[0], "NAME")) {
            d.name = std::string(trim(line.substr(t[0].size())));
        } else if (equalsCI(t[0], "MASS")) {
            if (t.size() != 2 || !parseWholeF32(t[1], d.massKg)) return fail(err, "MASS needs one number" + at);
        } else if (equalsCI(t[0], "SURFACE")) {
            if (t.size() != 2) return fail(err, "SURFACE needs one name" + at);
            d.surface = std::string(t[1]);
        } else if (equalsCI(t[0], "ROOT")) {
            if (t.size() != 2) return fail(err, "ROOT needs one node id" + at);
            if (!d.root.empty()) return fail(err, "a second ROOT" + at);
            d.root = std::string(t[1]);
        } else if (equalsCI(t[0], "NODE")) {
            if (d.nodes.size() >= kOcShapeMaxNodes) return fail(err, "more than " + std::to_string(kOcShapeMaxNodes) + " nodes" + at);
            ShapeNode n;
            if (!parseNodeRecord(t, n, st, why)) return fail(err, why + at);
            d.nodes.push_back(std::move(n));
        } else if (equalsCI(t[0], "CHILD")) {
            if (d.children.size() >= kOcShapeMaxChildren) return fail(err, "more than " + std::to_string(kOcShapeMaxChildren) + " CHILD records" + at);
            ShapeChildRec c;
            if (!parseChildRecord(t, c, why)) return fail(err, why + at);
            d.children.push_back(std::move(c));
        } else {
            warnOnce(st, "unknown record '" + std::string(t[0]) + "' skipped" + at);
        }
    }

    if (!sawHeader) return fail(err, "empty or missing OCSHAPE header");
    const std::vector<std::string> problems = validateOcShape(d);
    if (!problems.empty()) return fail(err, problems.front());
    out = std::move(d);
    return true;
}

std::string writeOcShape(const OcShapeData& d) {
    std::string s = "OCSHAPE 1\n";
    if (!d.name.empty()) s += "NAME " + d.name + "\n";
    if (d.massKg > 0.0f) { s += "MASS "; appendNum(s, d.massKg); s += '\n'; }
    if (!d.surface.empty()) s += "SURFACE " + d.surface + "\n";
    for (const ShapeNode& n : d.nodes) {
        s += "NODE " + n.id + ' ';
        if (shapeKindIsLeaf(n.kind)) {
            s += shapeKindWord(n.kind);
            if (n.kind == ShapeNodeKind::HullMesh) s += " mesh " + n.path;
            else if (n.kind == ShapeNodeKind::Mesh) s += ' ' + n.path;
            else for (const f32 v : n.nums) { s += ' '; appendNum(s, v); }
            leafOptionsOut(s, n);
        } else if (n.kind == ShapeNodeKind::Compound) {
            s += n.mutableCompound ? "compound mutable" : "compound static";
        } else {
            s += shapeKindWord(n.kind);
            s += ' ' + n.child;
            if (n.kind == ShapeNodeKind::RotatedTranslated) {
                if (!isZero3(n.pos)) { s += " at"; appendTriple(s, n.pos); }
                if (!isIdentityRot(n.rot)) { s += " rot"; appendTriple(s, n.rot); s += ' '; appendNum(s, n.rot[3]); }
            } else if (n.kind == ShapeNodeKind::OffsetCom) {
                s += " at"; appendTriple(s, n.pos);
            } else {
                appendTriple(s, n.scale);
            }
        }
        s += '\n';
    }
    for (const ShapeChildRec& c : d.children) {
        s += "CHILD " + c.compound + ' ' + c.node;
        if (!isZero3(c.pos)) { s += " at"; appendTriple(s, c.pos); }
        if (!isIdentityRot(c.rot)) { s += " rot"; appendTriple(s, c.rot); s += ' '; appendNum(s, c.rot[3]); }
        if (c.user != 0) s += " user " + std::to_string(c.user);
        s += '\n';
    }
    if (!d.root.empty()) s += "ROOT " + d.root + "\n";
    return s;
}

bool loadOcShape(const std::string& path, OcShapeData& out, std::string* err) {
    // readFileText reports the open to the file trace, which is how a packaged game proves it read this file from its package.
    std::string text;
    if (!readFileText(path, text)) return fail(err, "cannot open " + path);
    return parseOcShape(text, out, err);
}

bool saveOcShape(const std::string& path, const OcShapeData& in, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (!writeFileTextAtomic(path, writeOcShape(in))) return fail(err, "could not write " + path);
    return true;
}

} // namespace aver::fmt
