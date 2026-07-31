// Reads and rewrites the generated region and actor declarations in a C# actor script.

#include "aver/formats/ActorScript.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace aver::fmt {
namespace {

// The generated region's open and close marker text.
constexpr const char* kOpenA  = "<aver-generated region=\"models\" schema=\"";
constexpr const char* kClose  = "</aver-generated>";

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdent(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '_';
}

// Returns the first index at or after `i` that is not whitespace.
usize skipSpace(std::string_view t, usize i) {
    while (i < t.size() && isSpace(t[i])) ++i;
    return i;
}

// Returns the start of the line containing `i`.
usize lineBegin(std::string_view t, usize i) {
    while (i > 0 && t[i - 1] != '\n') --i;
    return i;
}
// Returns the start of the line after the one containing `i`.
usize lineEnd(std::string_view t, usize i) {
    while (i < t.size() && t[i] != '\n') ++i;
    return i < t.size() ? i + 1 : i;
}

// True when the line at `lineStart` is a `//` comment carrying `needle`; reports where it sits.
bool markerLineAt(std::string_view t, usize lineStart, const char* needle, usize& needleAt) {
    usize i = skipSpace(t, lineStart);
    if (i + 1 >= t.size() || t[i] != '/' || t[i + 1] != '/') return false;
    i = skipSpace(t, i + 2);
    const usize n = std::strlen(needle);
    if (t.compare(i, n, needle) != 0) return false;
    needleAt = i;
    return true;
}

// Parses a C# float literal, -?[0-9]+(\.[0-9]+)?f. No exponent form; the `f` suffix is required.
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

// Parses `(a, b, c)` of three float literals.
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

// Parses a `"..."` literal, unescaping the two sequences the writer produces.
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

// Consumes the named argument `name: `. Returns false if it is not there.
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

// Normalises a mesh path to the registry's key: forward slashes, no "./" or "Content/" prefix.
std::string canonicalMeshPath(std::string_view path) {
    std::string s(path);
    for (char& c : s) if (c == '\\') c = '/';
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    if (s.size() > 8) {
        const bool prefixed =
            (s[0] == 'C' || s[0] == 'c') && s.compare(1, 7, "ontent/") == 0;
        if (prefixed) s.erase(0, 8);
    }
    return s;
}

namespace {

// Where one actor attribute sits and where its quoted name starts.
struct AttrHit { usize at; usize nameAt; };

// Every [AverClass] / [AverGameMode] / [AverActor] attribute in the text, sorted into file order.
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

// Reads the numbers of a call such as `.Camera(70f, 5f, 100000f)`. Returns true when the call exists.
bool numbersAfter(std::string_view t, const char* call, f32* out, int count, ActorValueSpan* spans) {
    const usize at = t.find(call);
    if (at == std::string_view::npos) return false;
    usize p = at + std::strlen(call);
    for (int k = 0; k < count; ++k) {
        p = skipSpace(t, p);
        const usize valueAt = p;
        if (parseFloatLiteral(t, p, out[k])) {
            if (spans) { spans[k].begin = valueAt; spans[k].end = p; }
        } else {
            int depth = 0;
            while (p < t.size()) {
                const char ch = t[p];
                if (ch == '(') ++depth;
                else if (ch == ')') { if (depth == 0) break; --depth; }
                else if (ch == ',' && depth == 0) break;
                ++p;
            }
        }
        p = skipSpace(t, p);
        if (k + 1 < count) { if (p >= t.size() || t[p] != ',') return true; ++p; }
    }
    return true;
}

// Shifts a slice-relative span onto the whole file. An unset span is left alone.
void rebase(ActorValueSpan& sp, usize base) {
    if (!sp.valid()) return;
    sp.begin += base;
    sp.end += base;
}

} // namespace

// Returns a display name for an actor kind.
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

// True when a class of this kind is spatial and worth showing a 3D viewport for. Unknown counts as spatial.
bool actorKindHasViewport(ActorKind k) {
    switch (k) {
        case ActorKind::PlayerController:
        case ActorKind::GameMode:
        case ActorKind::GameInstance: return false;
        default:                      return true;
    }
}

namespace {

// Classifies a base type name by its suffix. Anything unrecognised is Unknown.
ActorKind kindOfBase(std::string_view base) {
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

// Finds `Field = 123f;` or `Field = 123;` anywhere in the class text and reads the number.
bool assignedNumber(std::string_view t, const char* field, f32& out, ActorValueSpan* span = nullptr) {
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
                const usize valueAt = p;
                if (parseFloatLiteral(t, p, out)) {
                    if (span) { span->begin = valueAt; span->end = p; }
                    return true;
                }
                const usize d = p;
                while (p < t.size() && isDigit(t[p])) ++p;
                if (p > d) {
                    out = static_cast<f32>(std::strtod(std::string(t.substr(d, p - d)).c_str(), nullptr));
                    if (span) { span->begin = d; span->end = p; }
                    return true;
                }
            }
        }
        i += n;
    }
    return false;
}

} // namespace

// One `class <Ident> : <Base>` declaration and where its keyword sits.
struct ClassDecl { usize at; std::string name; std::string base; };

// Every class declaration in the text, in file order.
std::vector<ClassDecl> classDeclarations(std::string_view t) {
    std::vector<ClassDecl> out;
    usize i = 0;
    while ((i = t.find("class ", i)) != std::string_view::npos) {
        // `class` has to be a word of its own: "subclassing" and "MyClass " both contain it.
        const bool boundedLeft = (i == 0) || !isIdent(t[i - 1]);
        if (!boundedLeft) { i += 6; continue; }

        ClassDecl d;
        d.at = i;
        usize q = skipSpace(t, i + 6);
        while (q < t.size() && isIdent(t[q])) d.name += t[q++];
        q = skipSpace(t, q);
        if (q < t.size() && t[q] == ':') {
            q = skipSpace(t, q + 1);
            while (q < t.size() && isIdent(t[q])) d.base += t[q++];
        }
        if (!d.name.empty()) out.push_back(std::move(d));
        i += 6;
    }
    return out;
}

// Every actor class in the text: one carrying an attribute, or one whose base has a known suffix.
std::vector<ActorClassInfo> parseActorClasses(std::string_view t) {
    std::vector<ActorClassInfo> out;
    const std::vector<AttrHit> attrs = actorAttributes(t);

    struct Start { usize at; usize nameAt; };   // nameAt == npos for an unattributed class
    std::vector<Start> starts;
    starts.reserve(attrs.size() + 4);
    for (const AttrHit& a : attrs) starts.push_back({a.at, a.nameAt});

    for (const ClassDecl& d : classDeclarations(t)) {
        if (kindOfBase(d.base) == ActorKind::Unknown) continue;   // not an actor by its base
        bool attributed = false;
        for (const AttrHit& a : attrs) {
            if (a.at >= d.at) continue;
            bool intervening = false;
            for (const ClassDecl& o : classDeclarations(t))
                if (o.at > a.at && o.at < d.at) { intervening = true; break; }
            if (!intervening) { attributed = true; break; }
        }
        if (!attributed) starts.push_back({d.at, std::string_view::npos});
    }

    std::sort(starts.begin(), starts.end(), [](const Start& a, const Start& b) { return a.at < b.at; });

    for (usize h = 0; h < starts.size(); ++h) {
        ActorClassInfo info;

        if (starts[h].nameAt != std::string_view::npos) {
            usize p = starts[h].nameAt;
            while (p < t.size() && t[p] != '"') info.className += t[p++];
        }

        const usize sliceEnd = (h + 1 < starts.size()) ? starts[h + 1].at : t.size();
        const std::string_view slice = t.substr(starts[h].at, sliceEnd - starts[h].at);

        if (const usize c = slice.find("class "); c != std::string_view::npos) {
            usize q = skipSpace(slice, c + 6);
            while (q < slice.size() && isIdent(slice[q])) info.typeName += slice[q++];
            q = skipSpace(slice, q);
            if (q < slice.size() && slice[q] == ':') {
                q = skipSpace(slice, q + 1);
                while (q < slice.size() && isIdent(slice[q])) info.baseType += slice[q++];
            }
        }
        info.kind = kindOfBase(info.baseType);

        if (info.kind == ActorKind::Character) {
            assignedNumber(slice, "Height", info.capsuleHeight, &info.capsuleHeightSpan);
            assignedNumber(slice, "Radius", info.capsuleRadius, &info.capsuleRadiusSpan);
            assignedNumber(slice, "EyeHeight", info.eyeHeight, &info.eyeHeightSpan);
        }

        usize m = slice.find(".Mesh(");
        if (m != std::string_view::npos) {
            usize q = skipSpace(slice, m + 6);
            const usize meshAt = q;
            if (parseString(slice, q, info.meshPath)) {
                info.hasMesh = true;
                info.meshPathSpan = {meshAt + 1, q - 1};   // literal contents, quotes excluded
                q = skipSpace(slice, q);
                if (q < slice.size() && slice[q] == ',') {
                    ++q;
                    const usize matAt = skipSpace(slice, q);
                    usize r = matAt;
                    if (parseString(slice, r, info.material)) info.materialSpan = {matAt + 1, r - 1};
                }
            }
        }

        f32 cam[3] = {};
        if (numbersAfter(slice, ".Camera(", cam, 3, info.cameraSpan)) {
            info.hasCamera = true;
            info.cameraFovDeg = cam[0]; info.cameraNearCm = cam[1]; info.cameraFarCm = cam[2];
        }
        f32 lit[2] = {};
        if (numbersAfter(slice, ".PointLight(", lit, 2, info.lightSpan)) {
            info.hasPointLight = true;
            info.lightIntensityLux = lit[0]; info.lightRangeCm = lit[1];
        }
        const usize base = starts[h].at;
        rebase(info.meshPathSpan, base);
        rebase(info.materialSpan, base);
        for (ActorValueSpan& sp : info.cameraSpan) rebase(sp, base);
        for (ActorValueSpan& sp : info.lightSpan) rebase(sp, base);
        rebase(info.capsuleHeightSpan, base);
        rebase(info.capsuleRadiusSpan, base);
        rebase(info.eyeHeightSpan, base);
        out.push_back(std::move(info));
    }
    return out;
}

// The first actor class in the text, or a default-constructed one when there is none.
ActorClassInfo parseActorClass(std::string_view t) {
    const std::vector<ActorClassInfo> all = parseActorClasses(t);
    return all.empty() ? ActorClassInfo{} : all.front();
}

// Writes `edited`'s values back into the class text. Returns false with `err` set on a bad parse.
bool rewriteActorClass(std::string_view t, const ActorClassInfo& edited,
                       std::string& out, std::string* err) {
    struct Edit { usize begin, end; std::string text; };
    std::vector<Edit> edits;

    auto number = [](f32 v) {
        // Fixed notation, never exponent: the parser's grammar has no exponent form.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(v));
        std::string t(buf);
        if (t.find('.') != std::string::npos) {
            while (!t.empty() && t.back() == '0') t.pop_back();
            if (!t.empty() && t.back() == '.') t.pop_back();
        }
        if (t.empty() || t == "-") t = "0";
        return t + "f";
    };
    auto put = [&](const ActorValueSpan& sp, std::string text) {
        if (!sp.valid() || sp.end > t.size()) return;
        edits.push_back({sp.begin, sp.end, std::move(text)});
    };

    put(edited.meshPathSpan, edited.meshPath);
    put(edited.materialSpan, edited.material);
    put(edited.capsuleHeightSpan, number(edited.capsuleHeight));
    put(edited.capsuleRadiusSpan, number(edited.capsuleRadius));
    put(edited.eyeHeightSpan, number(edited.eyeHeight));
    const f32 cam[3] = {edited.cameraFovDeg, edited.cameraNearCm, edited.cameraFarCm};
    for (int i = 0; i < 3; ++i) put(edited.cameraSpan[i], number(cam[i]));
    const f32 lit[2] = {edited.lightIntensityLux, edited.lightRangeCm};
    for (int i = 0; i < 2; ++i) put(edited.lightSpan[i], number(lit[i]));

    if (edits.empty()) { out.assign(t); return true; }

    for (usize a = 0; a + 1 < edits.size(); ++a)
        for (usize c = a + 1; c < edits.size(); ++c)
            if (edits[a].begin < edits[c].end && edits[c].begin < edits[a].end) {
                if (err) *err = "the class's value spans overlap; re-read the file before saving";
                return false;
            }

    for (usize a = 0; a + 1 < edits.size(); ++a)
        for (usize c = a + 1; c < edits.size(); ++c)
            if (edits[c].begin > edits[a].begin) std::swap(edits[a], edits[c]);

    out.assign(t);
    for (const Edit& e : edits) out.replace(e.begin, e.end - e.begin, e.text);
    return true;
}

// Parses the generated region and its b.Place placements out of an actor script.
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

        // Backwards to the property name and the '='.
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

// Formats one coordinate in the locked grammar: -?[0-9]+(\.[0-9]+)?f, no exponent form.
std::string coord(f32 v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(v));
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.empty() || s == "-") s = "0";
    return s + "f";
}

// Formats three coordinates as `(x, y, z)`.
std::string tuple(const f32 v[3]) {
    return "(" + coord(v[0]) + ", " + coord(v[1]) + ", " + coord(v[2]) + ")";
}

} // namespace

// Writes new pos/rot/scale into the placements matching `edits` by ObjectId. Only those three change.
bool rewriteActorScript(std::string_view t, const std::vector<ActorModel>& edits,
                        std::string& out, std::string* err) {
    const ActorScript parsed = parseActorScript(t);
    if (parsed.status != ActorParseStatus::Ok) {
        if (err) *err = parsed.error.empty() ? "no generated region in this file" : parsed.error;
        return false;
    }

    out.assign(t);
    for (usize k = parsed.models.size(); k-- > 0;) {
        const ActorModel& have = parsed.models[k];
        const ActorModel* want = nullptr;
        for (const ActorModel& e : edits) if (e.objectId == have.objectId) { want = &e; break; }
        if (!want) continue;   // not edited; left exactly as written

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
