// Reader and writer for .ocmat, the engine's line-based text material format.

#include "aver/formats/OcMat.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
using namespace aver::fmt::detail;
using pbr::TextureSlot;

namespace {

// Reads token `i` as a float, or returns `dflt` when it is absent.
f32 tokF(const std::vector<std::string_view>& t, usize i, f32 dflt = 0.0f) {
    return i < t.size() ? static_cast<f32>(parseF64(t[i], dflt)) : dflt;
}

// Formats a float short and round-trippable.
std::string num(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<f64>(v));
    return buf;
}

// Looks up a texture slot by its name, case-insensitively.
bool slotFromName(std::string_view name, TextureSlot& out) {
    for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
        const auto s = static_cast<TextureSlot>(i);
        if (equalsCI(name, pbr::MaterialLibrary::textureSlotName(s))) { out = s; return true; }
    }
    return false;
}

// One parsed texture reference plus whatever followed it on the line.
struct TexRefParse {
    pbr::TextureRef ref;
    std::string_view trailing;   // what followed the reference: uvN, colour space
    bool ok = false;
};

// Parses a texture reference: `{guid:0x…}`, `{path:…}`, `{…}`, or a bare unbraced path token.
TexRefParse parseTexRef(std::string_view afterSlot) {
    TexRefParse r;
    const usize open = afterSlot.find('{');
    if (open != std::string_view::npos) {
        const usize close = afterSlot.find('}', open);
        if (close == std::string_view::npos) return r;   // unterminated: not a reference at all
        const std::string_view inner = trim(afterSlot.substr(open + 1, close - open - 1));
        r.trailing = afterSlot.substr(close + 1);
        if (startsWithCI(inner, "guid:")) {
            r.ref.id = parseU64(trim(inner.substr(5)));
        } else if (startsWithCI(inner, "path:")) {
            r.ref.path = std::string(trim(inner.substr(5)));
        } else {
            r.ref.path = std::string(inner);
        }
        r.ok = !r.ref.empty();
        return r;
    }

    const std::string_view s = trim(afterSlot);
    if (s.empty()) return r;
    usize end = 0;
    while (end < s.size() && !isSpace(s[end])) ++end;
    r.ref.path = std::string(s.substr(0, end));
    r.trailing = s.substr(end);
    r.ok = true;
    return r;
}

} // namespace

// The colour space a texture slot is always read in.
const char* ocmatColorSpace(TextureSlot s) {
    switch (s) {
        case TextureSlot::BaseColor:
        case TextureSlot::Emissive:   return "sRGB";
        case TextureSlot::Normal:     return "normal";
        default:                      return "linear";
    }
}

// Parses .ocmat text into a MaterialDesc. Returns false with `err` set. Unknown records are skipped.
bool parseOcmat(std::string_view text, pbr::MaterialDesc& out, OcMatExtras* extras, std::string* err) {
    out = pbr::MaterialDesc{};
    OcMatExtras localExtras;
    OcMatExtras& ex = extras ? *extras : localExtras;
    ex = OcMatExtras{};

    bool sawHeader = false;
    int graphDepth = 0;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        if (graphDepth > 0) {
            for (const char c : line) {
                if (c == '{') ++graphDepth;
                else if (c == '}') --graphDepth;
            }
            if (graphDepth < 0) graphDepth = 0;
            continue;
        }

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];
        const auto after = [&line](std::string_view tok) {
            return line.substr(static_cast<usize>(tok.data() - line.data()) + tok.size());
        };

        if (equalsCI(key, "OCMAT")) {
            const i32 version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            if (version != 1) {
                if (err) *err = "unsupported OCMAT version " + std::to_string(version);
                return false;
            }
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            if (t.size() > 1) out.name = std::string(trim(after(key)));
        } else if (equalsCI(key, "SHADER")) {
            if (t.size() > 1) ex.shader = std::string(t[1]);
        } else if (equalsCI(key, "CULL")) {
            if (t.size() > 1) ex.cull = std::string(t[1]);
        } else if (equalsCI(key, "BLEND")) {
            if (t.size() > 1) {
                if (equalsCI(t[1], "opaque")) {
                    out.alphaMode = pbr::AlphaMode::Opaque;
                } else if (equalsCI(t[1], "masked")) {
                    out.alphaMode = pbr::AlphaMode::Mask;
                    if (t.size() > 2) out.alphaCutoff = tokF(t, 2, out.alphaCutoff);
                } else if (equalsCI(t[1], "translucent")) {
                    out.alphaMode = pbr::AlphaMode::Blend;
                } else if (equalsCI(t[1], "additive")) {
                    ex.additive = true;
                    out.alphaMode = pbr::AlphaMode::Blend;
                }
            }
        } else if (equalsCI(key, "FLAGS")) {
            for (usize i = 1; i < t.size(); ++i) {
                const std::vector<std::string_view> kv = splitChar(t[i], '=');
                if (kv.size() != 2) continue;
                const bool on = parseI32(kv[1], 0) != 0;
                if (equalsCI(kv[0], "twosided"))       out.twoSided = on;
                else if (equalsCI(kv[0], "castshadow")) out.castShadow = on;
                else if (equalsCI(kv[0], "worlduv"))    out.uvMode = on ? pbr::UvMode::WorldAligned
                                                                        : pbr::UvMode::Mesh;
            }
        } else if (equalsCI(key, "PARENT")) {
            const TexRefParse p = parseTexRef(after(key));
            if (p.ok) { ex.parentId = p.ref.id; ex.parentPath = p.ref.path; }
        } else if (equalsCI(key, "PARAM")) {
            if (t.size() < 3) continue;
            const std::string_view p = t[1];
            if (equalsCI(p, "baseColorFactor")) {
                for (u32 i = 0; i < 4; ++i) out.baseColorFactor[i] = tokF(t, 2 + i, out.baseColorFactor[i]);
            } else if (equalsCI(p, "emissiveFactor")) {
                for (u32 i = 0; i < 3; ++i) out.emissiveFactor[i] = tokF(t, 2 + i, out.emissiveFactor[i]);
            } else if (equalsCI(p, "metallicFactor"))     out.metallicFactor    = tokF(t, 2, out.metallicFactor);
            else if (equalsCI(p, "roughnessFactor"))      out.roughnessFactor   = tokF(t, 2, out.roughnessFactor);
            else if (equalsCI(p, "normalScale"))          out.normalScale       = tokF(t, 2, out.normalScale);
            else if (equalsCI(p, "occlusionStrength"))    out.occlusionStrength = tokF(t, 2, out.occlusionStrength);
            else if (equalsCI(p, "reflectance"))          out.reflectance       = tokF(t, 2, out.reflectance);
            else if (equalsCI(p, "f90"))                  out.f90               = tokF(t, 2, out.f90);
            else if (equalsCI(p, "alphaCutoff"))          out.alphaCutoff       = tokF(t, 2, out.alphaCutoff);
            // World centimetres per tile; a non-positive value is dropped.
            else if (equalsCI(p, "uvTiling")) {
                const f32 v = tokF(t, 2, out.uvTiling);
                if (v > 0.0f) out.uvTiling = v;
            }
        } else if (equalsCI(key, "TEX")) {
            if (t.size() < 3) continue;
            TextureSlot slot{};
            if (!slotFromName(t[1], slot)) {
                AVER_WARN("[ocmat] unknown texture slot '{}' — ignored", std::string(t[1]));
                continue;
            }
            const TexRefParse p = parseTexRef(after(t[1]));
            if (!p.ok) continue;
            out.textures[static_cast<u32>(slot)] = p.ref;

            for (const std::string_view tok : splitWhitespace(p.trailing)) {
                if (startsWithCI(tok, "uv") && tok.size() > 2) {
                    ex.uvSet[static_cast<u32>(slot)] = static_cast<u32>(parseI32(tok.substr(2), 0));
                } else if (!equalsCI(tok, ocmatColorSpace(slot))) {
                    AVER_WARN("[ocmat] {} declares '{}' but that slot is always {} — using {}",
                              pbr::MaterialLibrary::textureSlotName(slot), std::string(tok),
                              ocmatColorSpace(slot), ocmatColorSpace(slot));
                }
            }
        } else if (startsWithCI(key, "GRAPH")) {
            ex.hasGraph = true;
            // The `{` may be on this line (`GRAPH{`) or the next.
            int depth = 0;
            for (const char c : line) {
                if (c == '{') ++depth;
                else if (c == '}') --depth;
            }
            graphDepth = depth > 0 ? depth : 1;
        }
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocmat file: no OCMAT version line";
        return false;
    }
    if (ex.hasGraph) {
        AVER_WARN("[ocmat] '{}' carries a GRAPH block; no material compiler consumes it yet, so the "
                  "PARAM factors and TEX bindings alone are in effect", out.name);
    }
    return true;
}

// Reads an .ocmat file. The filename stem supplies the name when the file carries none.
bool loadOcmat(const std::string& path, pbr::MaterialDesc& out, OcMatExtras* extras, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    if (!parseOcmat(text, out, extras, err)) return false;
    if (out.name.empty()) out.name = std::filesystem::path(path).stem().string();
    return true;
}

// Renders a material as .ocmat text.
std::string writeOcmat(const pbr::MaterialDesc& d, const OcMatExtras* extras) {
    const OcMatExtras defaults{};
    const OcMatExtras& ex = extras ? *extras : defaults;

    std::string s;
    s.reserve(768);
    s += "OCMAT 1\n";
    s += "# Written by the Aver Engine editor.\n";
    if (!d.name.empty()) { s += "NAME "; s += d.name; s += "\n"; }
    s += "SHADER "; s += ex.shader.empty() ? "standard" : ex.shader; s += "\n";

    s += "BLEND ";
    if (ex.additive) {
        s += "additive";
    } else {
        switch (d.alphaMode) {
            case pbr::AlphaMode::Mask:  s += "masked " + num(d.alphaCutoff); break;
            case pbr::AlphaMode::Blend: s += "translucent"; break;
            default:                    s += "opaque"; break;
        }
    }
    s += "\n";

    // Derived from twoSided, which is authoritative, rather than echoed from `extras`.
    s += "CULL "; s += d.twoSided ? "none" : (ex.cull == "front" ? "front" : "back"); s += "\n";
    s += "FLAGS twosided="; s += d.twoSided ? "1" : "0";
    s += " castshadow="; s += d.castShadow ? "1" : "0";
    s += " worlduv="; s += d.uvMode == pbr::UvMode::WorldAligned ? "1" : "0"; s += "\n\n";

    s += "PARAM baseColorFactor " + num(d.baseColorFactor[0]) + " " + num(d.baseColorFactor[1]) + " "
       + num(d.baseColorFactor[2]) + " " + num(d.baseColorFactor[3]) + "\n";
    s += "PARAM metallicFactor "    + num(d.metallicFactor)    + "\n";
    s += "PARAM roughnessFactor "   + num(d.roughnessFactor)   + "\n";
    s += "PARAM emissiveFactor "    + num(d.emissiveFactor[0]) + " " + num(d.emissiveFactor[1]) + " "
       + num(d.emissiveFactor[2]) + "\n";
    s += "PARAM normalScale "       + num(d.normalScale)       + "\n";
    s += "PARAM occlusionStrength " + num(d.occlusionStrength) + "\n";
    s += "PARAM reflectance "       + num(d.reflectance)       + "\n";
    s += "PARAM f90 "               + num(d.f90)               + "\n";
    s += "PARAM uvTiling "          + num(d.uvTiling)          + "\n";

    bool anyTex = false;
    for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
        const pbr::TextureRef& r = d.textures[i];
        if (r.empty()) continue;
        if (!anyTex) { s += "\n"; anyTex = true; }
        const auto slot = static_cast<TextureSlot>(i);
        s += "TEX ";
        s += pbr::MaterialLibrary::textureSlotName(slot);
        s += " ";
        // The id wins where set, matching MaterialSystem's cache key.
        if (r.id) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "{guid:0x%016llX}", static_cast<unsigned long long>(r.id));
            s += buf;
        } else {
            s += "{path:" + r.path + "}";
        }
        s += " uv" + std::to_string(ex.uvSet[i]);
        s += " "; s += ocmatColorSpace(slot);
        s += "\n";
    }

    if (ex.parentId || !ex.parentPath.empty()) {
        s += "\n# PARENT is recorded but not yet applied — this material's own PARAMs are in effect.\n";
        s += "PARENT ";
        if (ex.parentId) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "{guid:0x%016llX}", static_cast<unsigned long long>(ex.parentId));
            s += buf;
        } else {
            s += "{path:" + ex.parentPath + "}";
        }
        s += "\n";
    }
    if (ex.hasGraph) {
        s += "\n# The GRAPH block of the source material was dropped: nothing compiles one yet, so\n"
             "# re-saving it would claim a fidelity this writer cannot deliver.\n";
    }
    return s;
}

// Writes a material to an .ocmat file, creating parent directories. Returns false with `err` set.
bool saveOcmat(const std::string& path, const pbr::MaterialDesc& d, const OcMatExtras* extras,
               std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcmat(d, extras);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
