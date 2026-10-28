#include "aver/formats/godot/GodotSections.hpp"

#include "aver/platform/FileSystem.hpp"

#include <cstring>

namespace aver::fmt::godot {

const GodotVariant* GodotTag::attr(std::string_view key) const {
    for (const GodotProperty& a : attrs)
        if (a.name == key) return &a.value;
    return nullptr;
}

std::string GodotTag::attrString(std::string_view key, std::string_view def) const {
    const GodotVariant* v = attr(key);
    if (v && v->isStringLike()) return v->s;
    return std::string(def);
}

i64 GodotTag::attrInt(std::string_view key, i64 def) const {
    const GodotVariant* v = attr(key);
    return v ? v->asInt(def) : def;
}

const GodotVariant* GodotSection::prop(std::string_view key) const {
    for (const GodotProperty& p : props)
        if (p.name == key) return &p.value;
    return nullptr;
}

namespace {

bool isAttrChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' ||
           c == '.' || c == '-' || c == ':';
}

std::string trimmed(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

bool parseTag(GodotVariantReader& r, GodotTag& tag) {
    tag.line = r.line();
    r.advance();  // '['
    const usize nameStart = r.pos();
    while (!r.atEnd() && r.peek() != ']' && r.peek() != ' ' && r.peek() != '\t' && r.peek() != '\n' && r.peek() != '\r')
        r.advance();
    tag.name = std::string(r.slice(nameStart, r.pos()));
    if (tag.name.empty()) {
        r.error("empty section name");
        return false;
    }
    while (true) {
        r.skipSpace(true);
        if (r.consume(']')) return true;
        if (r.atEnd()) {
            r.error("unterminated [" + tag.name + " ...] header");
            return false;
        }
        GodotProperty a;
        a.line = r.line();
        const usize ks = r.pos();
        while (isAttrChar(r.peek())) r.advance();
        a.name = std::string(r.slice(ks, r.pos()));
        if (a.name.empty()) {
            r.error(std::string("unexpected '") + r.peek() + "' in the [" + tag.name + "] header");
            return false;
        }
        r.skipSpace(false);
        if (!r.expect('=', "after the attribute name")) return false;
        if (!r.parseValue(a.value)) return false;
        tag.attrs.push_back(std::move(a));
    }
}

// True when the line starting `at` characters ahead of the cursor looks like `key = value`.
bool keyLikeLine(const GodotVariantReader& r, usize at) {
    usize p = at;
    while (r.peek(p) == ' ' || r.peek(p) == '\t') ++p;
    if (r.peek(p) == '"') {
        ++p;
        while (r.peek(p) != '"' && r.peek(p) != '\n' && r.peek(p) != '\0') ++p;
        if (r.peek(p) != '"') return false;
        ++p;
    } else {
        const usize s = p;
        while (isAttrChar(r.peek(p))) ++p;
        if (p == s) return false;
    }
    while (r.peek(p) == ' ' || r.peek(p) == '\t') ++p;
    return r.peek(p) == '=';
}

// Error recovery after a bad property: finish the line, then stop at the next tag or `key = value` line,
// so one bad value does not swallow the properties after it.
void skipToNextEntry(GodotVariantReader& r) {
    r.skipLine();
    while (!r.atEnd()) {
        usize p = 0;
        while (r.peek(p) == ' ' || r.peek(p) == '\t' || r.peek(p) == '\r') ++p;
        if (r.peek(p) == '[' || keyLikeLine(r, 0)) return;
        r.skipLine();
    }
}

}  // namespace

bool parseGodotSections(std::string_view text, std::string_view file, GodotSectionFile& out, GodotDiagnostics& diag) {
    const usize errorsBefore = diag.errorCount();
    out = GodotSectionFile{};
    out.file = std::string(file);
    GodotVariantReader r(text, file, &diag);

    while (true) {
        r.skipSpace(true);
        if (r.atEnd()) break;

        if (r.peek() == '[') {
            GodotSection sec;
            if (!parseTag(r, sec.tag)) {
                r.clearFailed();
                r.skipToNextTag();
                continue;
            }
            out.sections.push_back(std::move(sec));
            continue;
        }

        // key = value
        GodotProperty p;
        p.line = r.line();
        if (r.peek() == '"') {
            if (!r.parseString(p.name)) {
                r.clearFailed();
                skipToNextEntry(r);
                continue;
            }
            r.skipSpace(false);
        } else {
            const usize ks = r.pos();
            while (!r.atEnd() && r.peek() != '=' && r.peek() != '\n') r.advance();
            p.name = trimmed(r.slice(ks, r.pos()));
        }
        if (p.name.empty() || r.peek() != '=') {
            r.errorAt(p.line, "expected 'key = value'");
            r.clearFailed();
            skipToNextEntry(r);
            continue;
        }
        r.advance();  // '='
        if (!r.parseValue(p.value)) {
            r.clearFailed();
            skipToNextEntry(r);
            continue;
        }
        r.skipSpace(false);
        if (!r.atEnd() && r.peek() != '\n') {
            r.error("unexpected text after the value");
            r.clearFailed();
            skipToNextEntry(r);
            continue;
        }
        if (out.sections.empty()) out.sections.emplace_back();
        out.sections.back().props.push_back(std::move(p));
    }
    return diag.errorCount() == errorsBefore;
}

GodotFileKind detectGodotFileKind(const u8* data, usize size) {
    if (size >= 4 && std::memcmp(data, "RSRC", 4) == 0) return GodotFileKind::BinaryResource;
    if (size >= 4 && std::memcmp(data, "RSCC", 4) == 0) return GodotFileKind::BinaryResource;
    if (size >= 4 && std::memcmp(data, "GDSC", 4) == 0) return GodotFileKind::BinaryScript;
    if (size == 0) return GodotFileKind::Unknown;
    // NUL bytes in the head mean this is not text.
    const usize n = size < 512 ? size : 512;
    for (usize i = 0; i < n; ++i)
        if (data[i] == 0) return GodotFileKind::Unknown;
    return GodotFileKind::Text;
}

bool readGodotTextFile(const std::string& path, std::string& text, GodotDiagnostics& diag, std::string_view displayName) {
    const std::string_view name = displayName.empty() ? std::string_view(path) : displayName;
    std::vector<u8> bytes;
    if (!aver::readFileBytes(path, bytes)) {
        diag.error(name, 0, "cannot read the file", "godot.unreadable");
        return false;
    }
    const GodotFileKind kind = detectGodotFileKind(bytes.data(), bytes.size());
    if (kind == GodotFileKind::BinaryResource) {
        diag.error(name, 0,
                   "binary Godot resource; resave as text (open it in the Godot editor and save as .tscn/.tres, "
                   "or set the project's text-format resave option)",
                   "godot.binary_resource");
        return false;
    }
    if (kind == GodotFileKind::BinaryScript) {
        diag.error(name, 0, "compiled GDScript (.gdc); the converter needs the .gd source", "godot.binary_script");
        return false;
    }
    if (kind == GodotFileKind::Unknown && !bytes.empty()) {
        diag.error(name, 0, "not a text file; if this is a binary .scn/.res, resave as text", "godot.binary_resource");
        return false;
    }
    text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

}  // namespace aver::fmt::godot
