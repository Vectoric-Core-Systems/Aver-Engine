// Reader and writer for .ocmat, the engine's line-based text material format.

#include "aver/formats/OcMat.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>

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
        case TextureSlot::Layer1BaseColor: return "sRGB";
        case TextureSlot::Emissive:   return "sRGB";
        case TextureSlot::Normal:
        case TextureSlot::Layer1Normal: return "normal";
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
        // GRAPH may glue its opening brace onto the keyword when the block opens on the same line
        // ("GRAPH{", the only form the editor and every fixture in this tree write), so the token
        // this loop sees is as often "GRAPH{" as bare "GRAPH". `graphKey` strips one trailing '{'
        // before the GRAPH comparison further down, so that comparison can test the keyword exactly
        // rather than by prefix -- see that branch for why the prefix test had to go.
        const std::string_view graphKey =
            (!key.empty() && key.back() == '{') ? key.substr(0, key.size() - 1) : key;

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
        } else if (equalsCI(key, "GRAPHREF")) {
            // Rest-of-line, trimmed -- NOT t[1] -- the same choice NAME makes a few branches up and
            // DESCRIPTION makes in OcGraph.cpp, because a content path may contain spaces and a
            // single token would cut it at the first one and point at a file that does not exist.
            if (t.size() > 1) ex.graphRef = std::string(trim(after(key)));
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
            // Index of refraction. 1.0 (vacuum) is the physical floor -- glass is ~1.5, water ~1.33,
            // diamond ~2.42 -- so a value below it is repaired UP to 1.0 rather than kept as authored
            // or silently dropped to the struct default (1.5f, MaterialDesc::ior): the same "clamp,
            // don't reject" idiom slopeBlend's swapped lo/hi already uses a few branches up, chosen so
            // a typo reads back as the nearest *true* statement ("does not refract") instead of vanishing.
            else if (equalsCI(p, "ior")) {
                const f32 v = tokF(t, 2, out.ior);
                out.ior = v < 1.0f ? 1.0f : v;
            }
            // Dielectric transmission weight: 0 is the opaque default, 1 is fully transmissive (clear
            // glass). Belongs in [0,1] by definition, so it is clamped into range rather than passed
            // through -- an out-of-range PARAM is an authoring mistake, and storing e.g. 1.4 would let
            // that mistake reach the renderer as a value nothing else in this format ever produces.
            else if (equalsCI(p, "transmission")) {
                const f32 v = tokF(t, 2, out.transmission);
                out.transmission = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            // Subsurface wrap weight: 0 is the feature-off default, 1 wraps light all the way past
            // the terminator. Belongs in [0,1] by definition -- same reasoning as transmission just
            // above -- so it is clamped rather than passed through; an authored 1.4 would reach the
            // renderer as a magnitude this format never otherwise produces.
            else if (equalsCI(p, "subsurfaceWeight")) {
                const f32 v = tokF(t, 2, out.subsurfaceWeight);
                out.subsurfaceWeight = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            // Thickness proxy for the same back-scatter lobe; also [0,1], clamped for the same reason.
            else if (equalsCI(p, "subsurfaceRadius")) {
                const f32 v = tokF(t, 2, out.subsurfaceRadius);
                out.subsurfaceRadius = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            // ---- the coat: a clear film over the base material ----
            // Clamped into [0,1] like the two above, and for the same reason -- a weight outside that
            // range is an authoring slip, and letting it through gives a lobe that adds energy.
            else if (equalsCI(p, "coatWeight")) {
                const f32 v = tokF(t, 2, out.coatWeight);
                out.coatWeight = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            else if (equalsCI(p, "coatRoughness")) {
                const f32 v = tokF(t, 2, out.coatRoughness);
                out.coatRoughness = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            // Normal-incidence reflectance of the coat film. Clamped to [0,1] as a reflectance must
            // be; 0.04 (IOR 1.5) is ordinary lacquer and is the default when the key is absent.
            else if (equalsCI(p, "coatF0")) {
                const f32 v = tokF(t, 2, out.coatF0);
                out.coatF0 = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
            else if (equalsCI(p, "alphaCutoff"))          out.alphaCutoff       = tokF(t, 2, out.alphaCutoff);
            // World centimetres per tile; a non-positive value is dropped.
            // slopeBlend <lo> <hi> [layer1UvScale] -- turns the second layer on and says across
            // which slope band it fades in. lo/hi are world-normal Z: 1 flat, 0 vertical, so LO is
            // the steeper end and layer 1 wins there.
            else if (equalsCI(p, "slopeBlend")) {
                out.slopeBlend   = true;
                out.slopeBlendLo = tokF(t, 2, out.slopeBlendLo);
                out.slopeBlendHi = tokF(t, 3, out.slopeBlendHi);
                const f32 sc = tokF(t, 4, out.layer1UvScale);
                if (sc > 0.0f) out.layer1UvScale = sc;
                // Swapped bounds would make smoothstep return garbage rather than fail; order them.
                if (out.slopeBlendLo > out.slopeBlendHi) {
                    const f32 tmp = out.slopeBlendLo;
                    out.slopeBlendLo = out.slopeBlendHi;
                    out.slopeBlendHi = tmp;
                }
            }
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
        } else if (equalsCI(graphKey, "GRAPH")) {
            ex.hasGraph = true;
            // NARROWED FROM A PREFIX TEST. This used to be startsWithCI(key, "GRAPH"), which also
            // matches GRAPHREF -- a real record now, parsed above -- and would silently steal its
            // line into this brace-skipping block, discarding the referenced path with no warning at
            // all: exactly the trap this record's own history warns about. Comparing `graphKey`
            // rather than `key` is what lets the test stay an exact equalsCI rather than widening
            // back to a prefix: `graphKey` has already dropped the brace that glues onto the keyword
            // when the block opens on the same line ("GRAPH{"), which is the only form this tree's
            // editor and fixtures write. The `{` this block skips may be on this line or the next.
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
    // Unconditional, like reflectance/f90 just above and unlike slopeBlend below: these are always-
    // present scalars with meaningful defaults (ior 1.5, transmission 0 -- see MaterialDesc), not a
    // mode a material opts into, so there is no "unstated" case to protect by omitting them.
    s += "PARAM ior "               + num(d.ior)               + "\n";
    s += "PARAM transmission "      + num(d.transmission)      + "\n";
    s += "PARAM uvTiling "          + num(d.uvTiling)          + "\n";
    // OMITTED WHEN OFF, unlike every PARAM above it, and deliberately: the others are always-present
    // scalars with meaningful defaults, while this one is a MODE. Writing `slopeBlend 0.55 0.8` into
    // a material that has no second layer would claim a feature it does not have, and re-reading it
    // would set MaterialDesc::slopeBlend on a material that never asked for it.
    if (d.slopeBlend)
        s += "PARAM slopeBlend " + num(d.slopeBlendLo) + " " + num(d.slopeBlendHi) + " "
           + num(d.layer1UvScale) + "\n";
    // OMITTED WHEN OFF, same reasoning as slopeBlend just above and NOT the same reasoning as
    // ior/transmission further up: subsurfaceWeight 0 is not merely a number but the feature's own
    // off switch (MaterialGpu.cpp's packMaterial sets MaterialFlag_Subsurface exactly when this is
    // > 0), so a material that never asked for the wrap term has nothing meaningful to round-trip.
    // Writing "PARAM subsurfaceWeight 0" into every pre-existing .ocmat would still parse back to the
    // same off state, but it would turn every material in this tree's test fixtures into a diff the
    // instant this field was added, for a line that carries no information beyond "not in use" --
    // the same byte-stability argument that keeps slopeBlend opt-in. subsurfaceRadius rides along
    // unconditionally on this one line because it is meaningless without the weight that gates it.
    if (d.subsurfaceWeight > 0.0f)
        s += "PARAM subsurfaceWeight " + num(d.subsurfaceWeight) + "\n"
             "PARAM subsurfaceRadius " + num(d.subsurfaceRadius) + "\n";

    // Gated on coatWeight for exactly the reason stated above, and the other two ride along with it:
    // a roughness or an F0 with no weight describes a coat that is not there.
    if (d.coatWeight > 0.0f)
        s += "PARAM coatWeight " + num(d.coatWeight)    + "\n"
             "PARAM coatRoughness " + num(d.coatRoughness) + "\n"
             "PARAM coatF0 " + num(d.coatF0)        + "\n";

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
    if (!ex.graphRef.empty()) {
        // Recorded verbatim, exactly as GRAPHREF was parsed -- see OcMatExtras::graphRef. Nothing in
        // this format layer reads the referenced .ocgraph or resolves the path; a caller that wants
        // the compiled graph goes through pbr::MaterialGraphRegistry instead.
        s += "\nGRAPHREF " + ex.graphRef + "\n";
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
    const std::string text = writeOcmat(d, extras);

    // WRITE TO A TEMPORARY AND SWAP, via writeFileTextAtomic (aver/platform/FileSystem.hpp) -- the
    // same pattern aver::fmt::saveOcSave (OcSave.cpp) and aver_settings_flush (Settings.cpp)
    // already ship with, lifted to the shared platform layer. This function used to open `path`
    // directly with ios::trunc, which zeroes the file the instant it opens -- before writeOcmat's
    // result has landed a single byte -- so a crash, a kill, or a full disk between the open and
    // the write destroyed the material being saved rather than merely failing to update it.
    if (!writeFileTextAtomic(path, text)) {
        if (err) *err = "could not write " + path;
        return false;
    }
    return true;
}

} // namespace aver::fmt
