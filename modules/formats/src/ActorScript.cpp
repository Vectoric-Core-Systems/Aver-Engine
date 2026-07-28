#include "aver/formats/ActorScript.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace aver::fmt {
namespace {

// The marker lines, matched anchored and with leading whitespace ignored, exactly as
// docs/DESIGNER_REWRITE.md specifies. Matched as TEXT and not as a `#region`, because that is what
// the format uses: a line comment carries no nesting and no matching, so the open/close pairing is
// this function's job rather than a parser's.
constexpr const char* kOpenA  = "<aver-generated region=\"models\" schema=\"";
constexpr const char* kClose  = "</aver-generated>";

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdent(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '_';
}

usize skipSpace(std::string_view t, usize i) {
    while (i < t.size() && isSpace(t[i])) ++i;
    return i;
}

// The start of the line containing `i`, so a marker's own line can be excluded from the region.
usize lineBegin(std::string_view t, usize i) {
    while (i > 0 && t[i - 1] != '\n') --i;
    return i;
}
usize lineEnd(std::string_view t, usize i) {
    while (i < t.size() && t[i] != '\n') ++i;
    return i < t.size() ? i + 1 : i;
}

// Is the text from `lineStart` a comment line carrying `needle`? Anchored: leading whitespace, then
// `//`, then optional space, then the needle. A `needle` appearing anywhere else on a line -- inside
// a string, say -- must not be mistaken for a marker.
bool markerLineAt(std::string_view t, usize lineStart, const char* needle, usize& needleAt) {
    usize i = skipSpace(t, lineStart);
    if (i + 1 >= t.size() || t[i] != '/' || t[i + 1] != '/') return false;
    i = skipSpace(t, i + 2);
    const usize n = std::strlen(needle);
    if (t.compare(i, n, needle) != 0) return false;
    needleAt = i;
    return true;
}

// A C# float literal per the locked grammar: -?[0-9]+(\.[0-9]+)?f
//
// The `f` is REQUIRED, and refusing a literal without one is deliberate rather than pedantic: `1` in
// that position is an int, which would not bind to the float parameter, so a file containing one is
// a file that does not compile and reading it as 1.0f would be inventing a value the compiler never
// accepted. An exponent is likewise refused -- the grammar has none, and the material rewriter's
// formatter can emit one, which is exactly the drift this check exists to catch.
bool parseFloatLiteral(std::string_view t, usize& i, f32& out) {
    const usize start = i;
    if (i < t.size() && t[i] == '-') ++i;
    const usize digits = i;
    while (i < t.size() && isDigit(t[i])) ++i;
    if (i == digits) { i = start; return false; }
    if (i < t.size() && t[i] == '.') {
        ++i;
        const usize frac = i;
        while (i < t.size() && isDigit(t[i])) ++i;
        if (i == frac) { i = start; return false; }
    }
    if (i >= t.size() || (t[i] != 'f' && t[i] != 'F')) { i = start; return false; }
    const std::string lit(t.substr(start, i - start));
    ++i;   // the suffix
    out = static_cast<f32>(std::strtod(lit.c_str(), nullptr));
    return true;
}

// `(a, b, c)` of three float literals.
bool parseTuple(std::string_view t, usize& i, f32 out[3]) {
    i = skipSpace(t, i);
    if (i >= t.size() || t[i] != '(') return false;
    ++i;
    for (int k = 0; k < 3; ++k) {
        i = skipSpace(t, i);
        if (!parseFloatLiteral(t, i, out[k])) return false;
        i = skipSpace(t, i);
        if (k < 2) { if (i >= t.size() || t[i] != ',') return false; ++i; }
    }
    if (i >= t.size() || t[i] != ')') return false;
    ++i;
    return true;
}

// A `"..."` literal, with the two escapes the writer produces unescaped.
bool parseString(std::string_view t, usize& i, std::string& out) {
    i = skipSpace(t, i);
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out.clear();
    while (i < t.size()) {
        if (t[i] == '\\' && i + 1 < t.size()) { out += t[i + 1]; i += 2; continue; }
        if (t[i] == '"') { ++i; return true; }
        out += t[i++];
    }
    return false;
}

// A named argument: `name: `. Required by the grammar and checked rather than skipped, because a
// file with the arguments reordered is one the SCANNER must decline -- that is precisely the case
// the Roslyn backend exists to pick up, and silently accepting it here would take the wrong values.
bool expectNamed(std::string_view t, usize& i, const char* name) {
    i = skipSpace(t, i);
    const usize n = std::strlen(name);
    if (t.compare(i, n, name) != 0) return false;
    i += n;
    i = skipSpace(t, i);
    if (i >= t.size() || t[i] != ':') return false;
    ++i;
    return true;
}

} // namespace

std::string canonicalMeshPath(std::string_view path) {
    std::string s(path);
    for (char& c : s) if (c == '\\') c = '/';
    // Leading "./" and a leading "Content/" both name the same asset the registry keys without them.
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    if (s.size() > 8) {
        const bool prefixed =
            (s[0] == 'C' || s[0] == 'c') && s.compare(1, 7, "ontent/") == 0;
        if (prefixed) s.erase(0, 8);
    }
    return s;
}

namespace {

// Where each [AverClass("...")] / [AverGameMode("...")] / [AverActor("...")] sits, in FILE ORDER.
//
// File order, not attribute-kind order, and that is the whole fix: scanning for one attribute kind
// and then the next finds whichever kind comes first in the SEARCH rather than in the FILE, which is
// how FpsGameMode.cs came back named after the controller declared below its game mode.
struct AttrHit { usize at; usize nameAt; };

std::vector<AttrHit> actorAttributes(std::string_view t) {
    static const char* kAttrs[] = {"[AverClass(\"", "[AverGameMode(\"", "[AverActor(\""};
    std::vector<AttrHit> hits;
    for (const char* a : kAttrs) {
        const usize n = std::strlen(a);
        usize i = 0;
        while ((i = t.find(a, i)) != std::string_view::npos) {
            hits.push_back({i, i + n});
            i += n;
        }
    }
    for (usize x = 0; x + 1 < hits.size(); ++x)
        for (usize y = x + 1; y < hits.size(); ++y)
            if (hits[y].at < hits[x].at) std::swap(hits[x], hits[y]);
    return hits;
}

// Numbers following a call, e.g. `.Camera(70f, 5f, 100000f)`. Returns true when the CALL is present
// at all -- a value that is not a plain literal still means the feature is declared, and the number
// is what could not be read rather than the fact.
bool numbersAfter(std::string_view t, const char* call, f32* out, int count) {
    const usize at = t.find(call);
    if (at == std::string_view::npos) return false;
    usize p = at + std::strlen(call);
    for (int k = 0; k < count; ++k) {
        p = skipSpace(t, p);
        if (!parseFloatLiteral(t, p, out[k])) return true;
        p = skipSpace(t, p);
        if (k + 1 < count) { if (p >= t.size() || t[p] != ',') return true; ++p; }
    }
    return true;
}

} // namespace

const char* actorKindName(ActorKind k) {
    switch (k) {
        case ActorKind::Actor:            return "Actor";
        case ActorKind::Pawn:             return "Pawn";
        case ActorKind::Character:        return "Character";
        case ActorKind::PlayerController: return "Player Controller";
        case ActorKind::GameMode:         return "Game Mode";
        case ActorKind::GameInstance:     return "Game Instance";
        default:                          return "Class";
    }
}

bool actorKindHasViewport(ActorKind k) {
    // Unknown counts as spatial. Guessing the other way HIDES something: a class deriving a base
    // this build has not heard of is far more likely to be an actor than a rules object, and the
    // cost of being wrong is an empty viewport rather than a missing one.
    switch (k) {
        case ActorKind::PlayerController:
        case ActorKind::GameMode:
        case ActorKind::GameInstance: return false;
        default:                      return true;
    }
}

namespace {

ActorKind kindOfBase(std::string_view base) {
    // Matched on a SUFFIX rather than equality, so a project's own intermediate base -- and
    // MyGameCharacter is the first thing anybody writes -- still classifies. A class deriving
    // something entirely its own falls to Unknown and keeps its viewport, per the note above.
    auto ends = [&](const char* x) {
        const usize n = std::strlen(x);
        return base.size() >= n && base.compare(base.size() - n, n, x) == 0;
    };
    if (ends("GameInstance"))     return ActorKind::GameInstance;
    if (ends("GameMode"))         return ActorKind::GameMode;
    if (ends("PlayerController")) return ActorKind::PlayerController;
    if (ends("Controller"))       return ActorKind::PlayerController;
    if (ends("Character"))        return ActorKind::Character;
    if (ends("Pawn"))             return ActorKind::Pawn;
    if (ends("Actor"))            return ActorKind::Actor;
    return ActorKind::Unknown;
}

// `Field = 123f;` or `Field = 123;` anywhere in the class. Scanned rather than parsed to a method
// because a character sets these in OnBeginPlay, not in Configure -- which is where the framework's
// own template puts them and where every real character puts them too.
bool assignedNumber(std::string_view t, const char* field, f32& out) {
    const usize n = std::strlen(field);
    usize i = 0;
    while ((i = t.find(field, i)) != std::string_view::npos) {
        // A whole identifier, so `Height` does not match `EyeHeight`.
        const bool leftOk = i == 0 || !isIdent(t[i - 1]);
        usize p = i + n;
        if (leftOk && p < t.size()) {
            p = skipSpace(t, p);
            if (p < t.size() && t[p] == '=' && (p + 1 >= t.size() || t[p + 1] != '=')) {
                ++p;
                p = skipSpace(t, p);
                if (parseFloatLiteral(t, p, out)) return true;
                // An int literal is legal here (`Health = 3;`) even though the coordinate grammar
                // forbids one; this is ordinary C#, not the locked region.
                const usize d = p;
                while (p < t.size() && isDigit(t[p])) ++p;
                if (p > d) {
                    out = static_cast<f32>(std::strtod(std::string(t.substr(d, p - d)).c_str(), nullptr));
                    return true;
                }
            }
        }
        i += n;
    }
    return false;
}

} // namespace

std::vector<ActorClassInfo> parseActorClasses(std::string_view t) {
    std::vector<ActorClassInfo> out;
    const std::vector<AttrHit> hits = actorAttributes(t);

    for (usize h = 0; h < hits.size(); ++h) {
        ActorClassInfo info;

        usize p = hits[h].nameAt;
        while (p < t.size() && t[p] != '"') info.className += t[p++];

        // Only the text belonging to THIS class: from its attribute up to the next one. Without the
        // bound, two actors in one file borrow each other's mesh -- and the picture would be of a
        // class the panel is not naming.
        const usize sliceEnd = (h + 1 < hits.size()) ? hits[h + 1].at : t.size();
        const std::string_view slice = t.substr(hits[h].at, sliceEnd - hits[h].at);

        // The C# type the attribute is on, for the panel: `public sealed class Gun : AverActor`.
        if (const usize c = slice.find("class "); c != std::string_view::npos) {
            usize q = skipSpace(slice, c + 6);
            while (q < slice.size() && isIdent(slice[q])) info.typeName += slice[q++];
            // The base, after the colon. It is what decides whether a viewport means anything: a
            // GameMode has no transform, and a 3D view of one shows nothing while looking broken.
            q = skipSpace(slice, q);
            if (q < slice.size() && slice[q] == ':') {
                q = skipSpace(slice, q + 1);
                while (q < slice.size() && isIdent(slice[q])) info.baseType += slice[q++];
            }
        }
        info.kind = kindOfBase(info.baseType);

        if (info.kind == ActorKind::Character) {
            assignedNumber(slice, "Height", info.capsuleHeight);
            assignedNumber(slice, "Radius", info.capsuleRadius);
            assignedNumber(slice, "EyeHeight", info.eyeHeight);
        }

        usize m = slice.find(".Mesh(");
        if (m != std::string_view::npos) {
            usize q = m + 6;
            if (parseString(slice, q, info.meshPath)) {
                info.hasMesh = true;
                q = skipSpace(slice, q);
                if (q < slice.size() && slice[q] == ',') { ++q; parseString(slice, q, info.material); }
            }
        }

        f32 cam[3] = {};
        if (numbersAfter(slice, ".Camera(", cam, 3)) {
            info.hasCamera = true;
            info.cameraFovDeg = cam[0]; info.cameraNearCm = cam[1]; info.cameraFarCm = cam[2];
        }
        f32 lit[2] = {};
        if (numbersAfter(slice, ".PointLight(", lit, 2)) {
            info.hasPointLight = true;
            info.lightIntensityLux = lit[0]; info.lightRangeCm = lit[1];
        }
        out.push_back(std::move(info));
    }
    return out;
}

ActorClassInfo parseActorClass(std::string_view t) {
    const std::vector<ActorClassInfo> all = parseActorClasses(t);
    return all.empty() ? ActorClassInfo{} : all.front();
}

ActorScript parseActorScript(std::string_view t) {
    ActorScript out;
    out.backend = ActorParserBackend::Builtin;

    // --- the region ---
    usize openAt = std::string_view::npos, openNeedle = 0;
    for (usize ls = 0; ls < t.size(); ls = lineEnd(t, ls)) {
        usize at = 0;
        if (markerLineAt(t, ls, kOpenA, at)) { openAt = ls; openNeedle = at; break; }
        if (lineEnd(t, ls) == ls) break;
    }
    if (openAt == std::string_view::npos) {
        out.status = ActorParseStatus::NoRegion;
        return out;
    }

    // The schema, read rather than assumed. An unknown one is left alone and surfaced: a newer
    // editor's grammar read by an older one is how a file gets silently truncated.
    usize sv = openNeedle + std::strlen(kOpenA);
    std::string schema;
    while (sv < t.size() && t[sv] != '"') schema += t[sv++];
    if (schema != "1") {
        out.status = ActorParseStatus::UnknownSchema;
        out.error = "the region is schema \"" + schema + "\"; this build knows schema \"1\"";
        return out;
    }

    usize closeAt = std::string_view::npos;
    for (usize ls = lineEnd(t, openAt); ls < t.size(); ls = lineEnd(t, ls)) {
        usize at = 0;
        if (markerLineAt(t, ls, kClose, at)) { closeAt = ls; break; }
        if (lineEnd(t, ls) == ls) break;
    }
    if (closeAt == std::string_view::npos) {
        out.status = ActorParseStatus::Malformed;
        out.error = "the region has an open marker but no </aver-generated>";
        return out;
    }

    out.regionBegin = lineEnd(t, openAt);
    out.regionEnd = closeAt;

    // --- the placements ---
    const std::string_view region = t.substr(out.regionBegin, out.regionEnd - out.regionBegin);
    const std::string_view needle = "b.Place(";
    usize i = 0;
    while ((i = region.find(needle, i)) != std::string_view::npos) {
        ActorModel m;

        // Backwards to the property name and the '='. Done by scanning rather than by matching a
        // whole statement, because the assignment target is the one part of the row whose spacing
        // the grammar does not pin.
        usize back = i;
        while (back > 0 && isSpace(region[back - 1])) --back;
        if (back == 0 || region[back - 1] != '=') { i += needle.size(); continue; }
        --back;
        while (back > 0 && isSpace(region[back - 1])) --back;
        const usize nameEnd = back;
        while (back > 0 && isIdent(region[back - 1])) --back;
        if (back == nameEnd) { i += needle.size(); continue; }
        m.property.assign(region.substr(back, nameEnd - back));
        m.begin = out.regionBegin + back;

        usize p = i + needle.size();

        // ObjectId: 0x, 1..16 hex digits, UL.
        p = skipSpace(region, p);
        if (region.compare(p, 2, "0x") != 0 && region.compare(p, 2, "0X") != 0) {
            out.status = ActorParseStatus::Malformed;
            out.error = "placement for '" + m.property + "' has no 0x... ObjectId";
            return out;
        }
        p += 2;
        const usize hexStart = p;
        u64 id = 0;
        while (p < region.size()) {
            const char c = region[p];
            u32 v;
            if (isDigit(c)) v = static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = static_cast<u32>(c - 'A' + 10);
            else break;
            id = (id << 4) | v;
            ++p;
        }
        if (p == hexStart || p - hexStart > 16) {
            out.status = ActorParseStatus::Malformed;
            out.error = "placement for '" + m.property + "' has a malformed ObjectId";
            return out;
        }
        if (region.compare(p, 2, "UL") != 0 && region.compare(p, 2, "ul") != 0) {
            out.status = ActorParseStatus::Malformed;
            out.error = "the ObjectId for '" + m.property + "' is missing its UL suffix";
            return out;
        }
        p += 2;
        m.objectId = id;

        auto comma = [&]() -> bool {
            p = skipSpace(region, p);
            if (p >= region.size() || region[p] != ',') return false;
            ++p;
            return true;
        };
        auto fail = [&](const char* what) {
            out.status = ActorParseStatus::Malformed;
            out.error = std::string(what) + " for '" + m.property + "'";
        };

        if (!comma() || !parseString(region, p, m.meshPath)) { fail("expected a mesh path"); return out; }
        if (!comma() || !expectNamed(region, p, "material") || !parseString(region, p, m.material)) {
            fail("expected material: \"...\""); return out;
        }
        if (!comma() || !expectNamed(region, p, "pos") || !parseTuple(region, p, m.pos)) {
            fail("expected pos: (x, y, z)"); return out;
        }
        if (!comma() || !expectNamed(region, p, "rot") || !parseTuple(region, p, m.rot)) {
            fail("expected rot: (x, y, z)"); return out;
        }
        if (!comma() || !expectNamed(region, p, "scale") || !parseTuple(region, p, m.scale)) {
            fail("expected scale: (x, y, z)"); return out;
        }
        p = skipSpace(region, p);
        if (p >= region.size() || region[p] != ')') { fail("the placement is unterminated"); return out; }
        ++p;
        p = skipSpace(region, p);
        if (p >= region.size() || region[p] != ';') { fail("the placement has no semicolon"); return out; }
        ++p;

        m.end = out.regionBegin + p;
        out.models.push_back(std::move(m));
        i = p;
    }

    // Duplicate ids are refused. The id IS the match key, so two rows sharing one makes every
    // rewrite ambiguous -- and the failure would be a drag that moves the wrong model.
    for (usize a = 0; a < out.models.size(); ++a)
        for (usize b = a + 1; b < out.models.size(); ++b)
            if (out.models[a].objectId == out.models[b].objectId) {
                out.status = ActorParseStatus::Malformed;
                out.error = "'" + out.models[a].property + "' and '" + out.models[b].property +
                            "' share one ObjectId; it is the key a rewrite matches on";
                return out;
            }

    out.status = ActorParseStatus::Ok;
    return out;
}

namespace {

// The locked numeric grammar: -?[0-9]+(\.[0-9]+)?f, and NO exponent form.
//
// %g would produce one for a large or tiny value, and the material rewriter's formatter does exactly
// that -- which is why this is written here rather than shared. A literal this file cannot read back
// is a literal it must not write.
std::string coord(f32 v) {
    char buf[64];
    // Enough places to round-trip a float, then trailing zeros trimmed so an authored 0 stays "0f"
    // rather than becoming "0.000000f" on the first save.
    std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(v));
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.empty() || s == "-") s = "0";
    return s + "f";
}

std::string tuple(const f32 v[3]) {
    return "(" + coord(v[0]) + ", " + coord(v[1]) + ", " + coord(v[2]) + ")";
}

} // namespace

bool rewriteActorScript(std::string_view t, const std::vector<ActorModel>& edits,
                        std::string& out, std::string* err) {
    const ActorScript parsed = parseActorScript(t);
    if (parsed.status != ActorParseStatus::Ok) {
        if (err) *err = parsed.error.empty() ? "no generated region in this file" : parsed.error;
        return false;
    }

    // Rebuilt back to front, so every recorded byte range stays valid as the text changes under it.
    out.assign(t);
    for (usize k = parsed.models.size(); k-- > 0;) {
        const ActorModel& have = parsed.models[k];
        const ActorModel* want = nullptr;
        for (const ActorModel& e : edits) if (e.objectId == have.objectId) { want = &e; break; }
        if (!want) continue;   // not edited; left exactly as written

        // Only the three tuples are rewritten. The id, the mesh and the material are read-only to a
        // coordinate save by the format's own rule -- changing a mesh is a different operation with
        // different consequences, and doing it from a gizmo drag would be surprising.
        const std::string statement(t.substr(have.begin, have.end - have.begin));
        std::string rebuilt = statement;

        struct Field { const char* name; const f32* v; };
        const Field fields[3] = {{"pos:", want->pos}, {"rot:", want->rot}, {"scale:", want->scale}};
        bool ok = true;
        for (const Field& f : fields) {
            const usize at = rebuilt.find(f.name);
            if (at == std::string::npos) { ok = false; break; }
            usize a = at + std::strlen(f.name);
            while (a < rebuilt.size() && isSpace(rebuilt[a])) ++a;
            if (a >= rebuilt.size() || rebuilt[a] != '(') { ok = false; break; }
            usize b = a;
            int depth = 0;
            for (; b < rebuilt.size(); ++b) {
                if (rebuilt[b] == '(') ++depth;
                else if (rebuilt[b] == ')' && --depth == 0) { ++b; break; }
            }
            if (depth != 0) { ok = false; break; }
            rebuilt.replace(a, b - a, tuple(f.v));
        }
        if (!ok) {
            if (err) *err = "could not locate pos/rot/scale in the placement for '" + have.property + "'";
            return false;
        }
        out.replace(have.begin, have.end - have.begin, rebuilt);
    }
    return true;
}

} // namespace aver::fmt
