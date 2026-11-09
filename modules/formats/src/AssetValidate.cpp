#include "aver/formats/AssetValidate.hpp"

#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace aver::fmt {

namespace fs = std::filesystem;

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string extOf(const std::string& rel) { return lowerCopy(fs::path(rel).extension().string()); }

std::string joinPath(const std::string& root, const std::string& rel) {
    return (fs::path(root) / fs::path(rel)).string();
}

void add(ValidateReport& r, ValidateSeverity sev, const char* rule, const std::string& asset, std::string msg) {
    r.issues.push_back({sev, rule, asset, std::move(msg)});
}

// Extensions a reference token can end in. A token is a reference only when it ends in one of these.
const std::set<std::string>& referenceExts() {
    static const std::set<std::string> s = {
        ".ocmesh", ".ocmat", ".ocmati", ".ocgraph", ".ocworld", ".ocmap", ".ocskel", ".ocanim", ".ocsound",
        ".octex", ".ocprefab", ".ocvehicle", ".ocparticle", ".ocland", ".ocsurf", ".ocbeam", ".ocrig",
        ".png", ".jpg", ".jpeg", ".tga", ".hdr", ".exr", ".wav", ".ogg", ".flac"};
    return s;
}

bool isDelim(char c) {
    switch (c) {
        case ' ': case '\t': case '\r': case '\n': case '"': case '\'': case '=': case '{': case '}':
        case ',': case '(': case ')': case ';': case ':': case '[': case ']': case '<': case '>': case '|':
            return true;
        default: return false;
    }
}

// Every reference-looking token of a text asset, with its 1-based line. Whole-line comments are skipped.
struct RefToken { std::string path; u32 line = 0; };
std::vector<RefToken> extractRefs(const std::string& text) {
    std::vector<RefToken> out;
    u32 line = 1;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        size_t k = i;
        while (k < e && (text[k] == ' ' || text[k] == '\t')) ++k;
        if (k < e && text[k] != '#') {
            size_t t = k;
            while (t < e) {
                while (t < e && isDelim(text[t])) ++t;
                size_t u = t;
                while (u < e && !isDelim(text[u])) ++u;
                if (u > t) {
                    std::string tok = text.substr(t, u - t);
                    while (!tok.empty() && tok.back() == '.') tok.pop_back();
                    const bool absolute = tok[0] == '/' || tok[0] == '\\' || (tok.size() > 1 && tok[1] == ':');
                    if (!tok.empty() && !absolute && tok.find('*') == std::string::npos &&
                        tok.find('$') == std::string::npos && referenceExts().count(extOf(tok)))
                        out.push_back({tok, line});
                }
                t = u;
            }
        }
        i = e + 1;
        ++line;
    }
    return out;
}

bool readText(const ValidateContext& ctx, const std::string& rel, std::string& out) {
    return aver::readFileText(joinPath(ctx.contentDir, rel), out);
}

// ---- rules ---------------------------------------------------------------------------------------

void ruleMissingReference(const ValidateContext& ctx, const std::string& rel, ValidateReport& r) {
    std::string text;
    if (!readText(ctx, rel, text)) return;
    const std::string dir = fs::path(rel).parent_path().generic_string();
    std::set<std::string> seen;
    for (const RefToken& t : extractRefs(text)) {
        std::string norm = t.path;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        if (ctx.exists(norm) || (!dir.empty() && ctx.exists(dir + "/" + norm))) continue;
        if (!seen.insert(norm).second) continue;
        add(r, ValidateSeverity::Error, "missing-reference", rel,
            "line " + std::to_string(t.line) + ": '" + t.path + "' does not exist");
    }
}

void ruleOcmat(const ValidateContext& ctx, const std::string& rel, ValidateReport& r) {
    std::string text;
    if (!readText(ctx, rel, text)) return;
    size_t h = 0;
    while (h < text.size()) {
        size_t e = text.find('\n', h);
        if (e == std::string::npos) e = text.size();
        const size_t k = text.find_first_not_of(" \t\r", h);
        if (k != std::string::npos && k < e && text[k] != '#') {
            if (text.compare(k, 5, "OCMAT") != 0)
                add(r, ValidateSeverity::Error, "ocmat-parse", rel, "does not start with an OCMAT header");
            break;
        }
        h = e + 1;
    }
    // A material's graph must be a material-domain graph; the reference itself is the missing-reference rule's.
    size_t at = 0;
    while ((at = text.find("GRAPHREF", at)) != std::string::npos) {
        const bool lineStart = at == 0 || text[at - 1] == '\n';
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        if (lineStart) {
            std::string path = text.substr(at + 8, e - at - 8);
            const size_t a = path.find_first_not_of(" \t"), b = path.find_last_not_of(" \t\r");
            if (a != std::string::npos) {
                path = path.substr(a, b - a + 1);
                OcGraphData g;
                if (ctx.exists(path) && loadOcgraph(joinPath(ctx.contentDir, path), g) && lowerCopy(g.domain) != "material")
                    add(r, ValidateSeverity::Error, "ocmat-parse", rel,
                        "GRAPHREF '" + path + "' is not a material-domain graph (DOMAIN is '" + g.domain + "')");
            }
        }
        at = e;
    }
}

void ruleOcgraph(const ValidateContext& ctx, const std::string& rel, ValidateReport& r) {
    OcGraphData g;
    std::string err;
    if (!loadOcgraph(joinPath(ctx.contentDir, rel), g, &err)) {
        add(r, ValidateSeverity::Error, "ocgraph-parse", rel, "does not parse: " + err);
        return;
    }
    std::set<std::string> ids;
    for (const OcGraphNode& n : g.nodes)
        if (!ids.insert(n.id).second)
            add(r, ValidateSeverity::Error, "ocgraph-parse", rel, "node id '" + n.id + "' is used twice");
    for (const OcGraphLink& l : g.links) {
        if (!ids.count(l.sourceNode))
            add(r, ValidateSeverity::Error, "ocgraph-parse", rel,
                "link from unknown node '" + l.sourceNode + "' (pin '" + l.sourcePin + "')");
        if (!ids.count(l.destNode))
            add(r, ValidateSeverity::Error, "ocgraph-parse", rel,
                "link to unknown node '" + l.destNode + "' (pin '" + l.destPin + "')");
    }
}

void ruleMeshSlots(const ValidateContext& ctx, const std::string& rel, ValidateReport& r) {
    OcMeshData m;
    std::string why;
    if (!loadOcMesh(joinPath(ctx.contentDir, rel), m, &why)) {
        add(r, ValidateSeverity::Error, "mesh-slots", rel, "does not load: " + why);
        return;
    }
    const u32 slots = static_cast<u32>(m.materialSlots.size());
    const u32 verts = static_cast<u32>(m.positions.size() / 3);
    const u64 idx = m.indices.size();
    for (size_t i = 0; i < m.submeshes.size(); ++i) {
        const OcMeshSubmesh& s = m.submeshes[i];
        const std::string who = "submesh " + std::to_string(i) + (s.name.empty() ? "" : " '" + s.name + "'");
        if (slots > 0 ? s.materialSlot >= slots : s.materialSlot != 0)
            add(r, ValidateSeverity::Error, "mesh-slots", rel,
                who + " uses material slot " + std::to_string(s.materialSlot) + " but the mesh has " + std::to_string(slots));
        if (static_cast<u64>(s.indexStart) + s.indexCount > idx)
            add(r, ValidateSeverity::Error, "mesh-slots", rel,
                who + " index range " + std::to_string(s.indexStart) + "+" + std::to_string(s.indexCount) +
                " runs past the " + std::to_string(idx) + " indices");
    }
    if (m.submeshes.empty() && idx > 0)
        add(r, ValidateSeverity::Warning, "mesh-slots", rel, "has indices but no submesh to draw them");
    for (u32 v : m.indices)
        if (v >= verts) {
            add(r, ValidateSeverity::Error, "mesh-slots", rel,
                "an index (" + std::to_string(v) + ") is past the " + std::to_string(verts) + " vertices");
            break;
        }
}

bool imageSize(const std::string& path, const std::string& ext, u32& w, u32& h) {
    std::vector<u8> b;
    if (!aver::readFileBytes(path, b)) return false;
    if (ext == ".png") {
        if (b.size() < 24 || std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
        w = (u32(b[16]) << 24) | (u32(b[17]) << 16) | (u32(b[18]) << 8) | b[19];
        h = (u32(b[20]) << 24) | (u32(b[21]) << 16) | (u32(b[22]) << 8) | b[23];
        return true;
    }
    if (ext == ".tga") {
        if (b.size() < 18) return false;
        w = b[12] | (u32(b[13]) << 8);
        h = b[14] | (u32(b[15]) << 8);
        return true;
    }
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;   // JPEG: walk to a SOF marker
    size_t p = 2;
    while (p + 9 < b.size()) {
        if (b[p] != 0xFF) { ++p; continue; }
        const u8 mk = b[p + 1];
        if (mk == 0xFF) { ++p; continue; }
        if (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) { p += 2; continue; }
        const size_t len = (size_t(b[p + 2]) << 8) | b[p + 3];
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            h = (u32(b[p + 5]) << 8) | b[p + 6];
            w = (u32(b[p + 7]) << 8) | b[p + 8];
            return true;
        }
        p += 2 + len;
    }
    return false;
}

void ruleTextureDims(const ValidateContext& ctx, const std::string& rel, ValidateReport& r) {
    u32 w = 0, h = 0;
    if (!imageSize(joinPath(ctx.contentDir, rel), extOf(rel), w, h)) {
        add(r, ValidateSeverity::Warning, "texture-dimensions", rel, "image header could not be read");
        return;
    }
    if (w == 0 || h == 0) { add(r, ValidateSeverity::Error, "texture-dimensions", rel, "image has a zero dimension"); return; }
    const std::string size = std::to_string(w) + "x" + std::to_string(h);
    if (w % 4 != 0 || h % 4 != 0)
        add(r, ValidateSeverity::Warning, "texture-dimensions", rel,
            size + " is not a multiple of 4, so it cannot be block-compressed");
    else if ((w & (w - 1)) != 0 || (h & (h - 1)) != 0)
        add(r, ValidateSeverity::Info, "texture-dimensions", rel, size + " is not a power of two");
}

}  // namespace

u32 ValidateReport::count(ValidateSeverity s) const {
    u32 n = 0;
    for (const ValidateIssue& i : issues) if (i.severity == s) ++n;
    return n;
}

bool ValidateContext::exists(const std::string& relPath) const {
    // The engine's generated primitives are names, not files (rhi/PrimitiveMeshes.hpp, PreviewMeshCache).
    static const char* const kBuiltins[] = {"meshes/cube.ocmesh", "meshes/sphere.ocmesh", "meshes/plane.ocmesh",
                                            "meshes/cylinder.ocmesh", "meshes/cone.ocmesh", "meshes/capsule.ocmesh"};
    std::string key = lowerCopy(relPath);
    std::replace(key.begin(), key.end(), '\\', '/');
    for (const char* b : kBuiltins) if (key == b) return true;
    std::error_code ec;
    if (fs::exists(fs::path(contentDir) / relPath, ec)) return true;
    for (const std::string& root : extraRoots)
        if (fs::exists(fs::path(root) / relPath, ec)) return true;
    return false;
}

std::vector<ValidateRule> defaultRules() {
    std::vector<ValidateRule> v;
    v.push_back({"missing-reference", {".ocworld", ".ocmap", ".ocmat", ".ocmati", ".ocgraph", ".ocproject", ".ocprefab"},
                 ruleMissingReference});
    v.push_back({"ocmat-parse", {".ocmat"}, ruleOcmat});
    v.push_back({"ocgraph-parse", {".ocgraph"}, ruleOcgraph});
    v.push_back({"mesh-slots", {".ocmesh"}, ruleMeshSlots});
    v.push_back({"texture-dimensions", {".png", ".jpg", ".jpeg", ".tga"}, ruleTextureDims});
    return v;
}

AssetValidator::AssetValidator() : rules_(defaultRules()) {}

void AssetValidator::validateFile(const ValidateContext& ctx, const std::string& relPath, ValidateReport& out) const {
    const std::string ext = extOf(relPath);
    bool any = false;
    for (const ValidateRule& rule : rules_) {
        if (!rule.extensions.empty() &&
            std::find(rule.extensions.begin(), rule.extensions.end(), ext) == rule.extensions.end()) continue;
        any = true;
        rule.run(ctx, relPath, out);
    }
    if (any) ++out.filesChecked;
}

ValidateReport AssetValidator::validateAll(const ValidateContext& ctx) const {
    ValidateReport rep;
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(ctx.contentDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) files.push_back(fs::relative(it->path(), ctx.contentDir, e2).generic_string());
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) validateFile(ctx, f, rep);
    return rep;
}

const char* validateSeverityName(ValidateSeverity s) {
    return s == ValidateSeverity::Error ? "error" : s == ValidateSeverity::Warning ? "warning" : "info";
}

std::string formatValidateIssue(const ValidateIssue& i) {
    return std::string("[") + validateSeverityName(i.severity) + "] " + i.asset + ": " + i.message + " (" + i.rule + ")";
}

}  // namespace aver::fmt
