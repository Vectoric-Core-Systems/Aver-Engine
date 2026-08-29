// Emits and rewrites the C# MaterialBuilder chain in an [AverMaterial] class.

#include "aver/formats/MaterialScript.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace aver::fmt {
namespace {

// Defaults mirroring pbr::MaterialDesc and the C# MaterialBuilder. A value equal to one is not written.
constexpr f32 kDefMetallic = 1.0f, kDefRoughness = 1.0f, kDefNormalScale = 1.0f;
constexpr f32 kDefOcclusion = 1.0f, kDefReflectance = 0.04f, kDefF90 = 1.0f, kDefTiling = 100.0f;
constexpr f32 kDefIor = 1.5f;

// True when two floats agree to 1e-6.
bool same(f32 a, f32 b) { return std::fabs(a - b) <= 1.0e-6f; }

// Formats a float as a C# literal at the shortest precision that parses back to the same value.
std::string num(f32 v) {
    char buf[40];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, static_cast<double>(v));
        if (static_cast<f32>(std::strtod(buf, nullptr)) == v) break;
    }
    return std::string(buf) + "f";
}

// The C# `Slot.` enumerator name for a texture slot.
const char* slotName(pbr::TextureSlot s) {
    switch (s) {
        case pbr::TextureSlot::BaseColor:  return "BaseColor";
        case pbr::TextureSlot::MetalRough: return "MetalRough";
        case pbr::TextureSlot::Normal:     return "Normal";
        case pbr::TextureSlot::Occlusion:  return "Occlusion";
        case pbr::TextureSlot::Emissive:   return "Emissive";
        default: return "BaseColor";
    }
}

// Wraps a path in a C# string literal, escaping backslashes and quotes.
std::string quoted(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') r += '\\';
        r += c;
    }
    r += '"';
    return r;
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Skips a C# string literal opening at `i`. Returns the index just past the closing quote.
usize skipString(std::string_view t, usize i) {
    ++i;   // the opening quote
    while (i < t.size()) {
        if (t[i] == '\\' && i + 1 < t.size()) { i += 2; continue; }
        if (t[i] == '"') return i + 1;
        ++i;
    }
    return i;
}

// Skips whitespace, line comments and block comments.
usize skipTrivia(std::string_view t, usize i) {
    for (;;) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
        if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) ++i;
            i = i + 2 < t.size() ? i + 2 : t.size();
            continue;
        }
        return i;
    }
}

// The .Comment("...") arguments already in a builder chain, in order, unescaped.
std::vector<std::string> existingComments(std::string_view body) {
    std::vector<std::string> out;
    const std::string_view needle = ".Comment(";
    usize i = 0;
    while ((i = body.find(needle, i)) != std::string_view::npos) {
        usize p = skipTrivia(body, i + needle.size());
        if (p >= body.size() || body[p] != '"') { i += needle.size(); continue; }
        const usize end = skipString(body, p);
        std::string lit(body.substr(p + 1, end - p - 2));
        std::string un;
        for (usize k = 0; k < lit.size(); ++k) {
            if (lit[k] == '\\' && k + 1 < lit.size() && (lit[k + 1] == '"' || lit[k + 1] == '\\')) ++k;
            un += lit[k];
        }
        out.push_back(un);
        i = end;
    }
    return out;
}

} // namespace

// Builds the `b.…` MaterialBuilder chain for a material. Only non-default values are emitted.
std::string materialConfigureChain(const pbr::MaterialDesc& d, const OcMatExtras* extras,
                                   const std::vector<std::string>& comments,
                                   const std::string& indent) {
    const OcMatExtras defaults{};
    const OcMatExtras& ex = extras ? *extras : defaults;

    std::string s = "b";
    auto line = [&](const std::string& call) { s += "\n" + indent + call; };

    for (const std::string& c : comments) line(".Comment(" + quoted(c) + ")");

    if (d.twoSided)                     line(".Culling(Cull.None)");
    else if (ex.cull == "front")        line(".Culling(Cull.Front)");
    if (!d.castShadow)                  line(".CastShadow(false)");

    if (ex.additive)                              line(".Blending(Blend.Additive)");
    else if (d.alphaMode == pbr::AlphaMode::Mask) line(".Blending(Blend.Masked, " + num(d.alphaCutoff) + ")");
    else if (d.alphaMode == pbr::AlphaMode::Blend)line(".Blending(Blend.Translucent)");

    if (d.uvMode == pbr::UvMode::WorldAligned) {
        std::string call = ".WorldUv(true)";
        if (!same(d.uvTiling, kDefTiling)) call += ".Tiling(" + num(d.uvTiling) + ")";
        line(call);
    } else if (!same(d.uvTiling, kDefTiling)) {
        line(".Tiling(" + num(d.uvTiling) + ")");
    }

    if (!same(d.baseColorFactor[0], 1.0f) || !same(d.baseColorFactor[1], 1.0f) ||
        !same(d.baseColorFactor[2], 1.0f) || !same(d.baseColorFactor[3], 1.0f))
        line(".BaseColor(" + num(d.baseColorFactor[0]) + ", " + num(d.baseColorFactor[1]) + ", " +
             num(d.baseColorFactor[2]) + ", " + num(d.baseColorFactor[3]) + ")");

    if (!same(d.metallicFactor, kDefMetallic))   line(".Metallic(" + num(d.metallicFactor) + ")");
    if (!same(d.roughnessFactor, kDefRoughness)) line(".Roughness(" + num(d.roughnessFactor) + ")");

    if (!same(d.emissiveFactor[0], 0.0f) || !same(d.emissiveFactor[1], 0.0f) || !same(d.emissiveFactor[2], 0.0f))
        line(".Emissive(" + num(d.emissiveFactor[0]) + ", " + num(d.emissiveFactor[1]) + ", " +
             num(d.emissiveFactor[2]) + ")");

    if (!same(d.normalScale, kDefNormalScale))       line(".NormalScale(" + num(d.normalScale) + ")");
    if (!same(d.occlusionStrength, kDefOcclusion))   line(".OcclusionStrength(" + num(d.occlusionStrength) + ")");
    if (!same(d.reflectance, kDefReflectance))       line(".Reflectance(" + num(d.reflectance) + ")");
    if (!same(d.f90, kDefF90))                       line(".F90(" + num(d.f90) + ")");
    // WITHOUT THESE TWO LINES, Save-to-C# SILENTLY DESTROYS AN AUTHORED GLASS. The chain this
    // function generates is what the .ocmat is regenerated from, so a field the chain omits is a
    // field that survives in the text right up until someone presses the button. ior and transmission
    // reached the parser, the writer, the GPU and the shaders without ever reaching here.
    if (!same(d.ior, kDefIor))                       line(".Ior(" + num(d.ior) + ")");
    if (d.transmission > 0.0f)                       line(".Transmission(" + num(d.transmission) + ")");

    // OMITTED WHEN OFF, same reasoning as OcMat.cpp's writer (see its comment): subsurfaceWeight 0 is
    // the feature's own off switch, not a default worth stating, so a material that never asked for
    // the wrap term gets no .Subsurface... calls at all. subsurfaceRadius is emitted unconditionally
    // once weight is set -- it is meaningless without the weight that gates it -- so a Save-to-C#
    // round trip never silently drops an authored radius by treating it as "still default".
    if (d.subsurfaceWeight > 0.0f) {
        line(".SubsurfaceWeight(" + num(d.subsurfaceWeight) + ")");
        line(".SubsurfaceRadius(" + num(d.subsurfaceRadius) + ")");
    }

    for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
        const pbr::TextureRef& r = d.textures[i];
        if (r.path.empty()) continue;
        line(".Texture(Slot." + std::string(slotName(static_cast<pbr::TextureSlot>(i))) + ", " +
             quoted(r.path) + ")");
    }

    return s;
}

// Replaces the builder chain in the [AverMaterial(boundName)] class. Returns false with `err` set.
bool rewriteMaterialScript(std::string_view csText, const std::string& boundName,
                           const pbr::MaterialDesc& d, const OcMatExtras* extras,
                           std::string& out, std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };

    const std::string marker = "[AverMaterial(\"" + boundName + "\")]";
    const usize attr = csText.find(marker);
    if (attr == std::string_view::npos) return fail("no [AverMaterial] class with that name in this file");

    usize cfg = csText.find("Configure", attr);
    if (cfg == std::string_view::npos) return fail("the class has no Configure method");

    // Past the parameter list.
    usize p = csText.find('(', cfg);
    if (p == std::string_view::npos) return fail("Configure has no parameter list");
    int depth = 0;
    for (; p < csText.size(); ++p) {
        if (csText[p] == '"') { p = skipString(csText, p) - 1; continue; }
        if (csText[p] == '(') ++depth;
        else if (csText[p] == ')' && --depth == 0) { ++p; break; }
    }
    if (depth != 0) return fail("Configure's parameter list is unterminated");

    const usize bodyStart = skipTrivia(csText, p);
    if (bodyStart >= csText.size()) return fail("Configure has no body");

    usize chainStart = 0, chainEnd = 0;
    bool expressionBodied = false;

    if (csText.compare(bodyStart, 2, "=>") == 0) {
        expressionBodied = true;
        chainStart = skipTrivia(csText, bodyStart + 2);
        // To the terminating semicolon at paren depth zero, skipping strings.
        usize i = chainStart;
        int par = 0;
        for (; i < csText.size(); ++i) {
            if (csText[i] == '"') { i = skipString(csText, i) - 1; continue; }
            if (csText[i] == '(') ++par;
            else if (csText[i] == ')') --par;
            else if (csText[i] == ';' && par == 0) break;
        }
        if (i >= csText.size()) return fail("the expression body is unterminated");
        chainEnd = i;
    } else if (csText[bodyStart] == '{') {
        usize i = bodyStart + 1;
        int braces = 1;
        for (; i < csText.size(); ++i) {
            if (csText[i] == '"') { i = skipString(csText, i) - 1; continue; }
            if (csText[i] == '{') ++braces;
            else if (csText[i] == '}' && --braces == 0) break;
        }
        if (braces != 0) return fail("the method body is unterminated");
        chainStart = skipTrivia(csText, bodyStart + 1);
        chainEnd = i;
        // Must be exactly `b<chain>;` and nothing else.
        std::string_view inner = csText.substr(chainStart, chainEnd - chainStart);
        usize semi = inner.find_last_of(';');
        if (semi == std::string_view::npos) return fail("the block body has no statement to replace");
        if (inner.find(';') != semi) return fail("Configure has more than one statement; edit it by hand");
        if (inner.empty() || (inner[0] != 'b' && csText.compare(chainStart, 7, "return ") != 0))
            return fail("Configure's body is not a builder chain; edit it by hand");
        chainEnd = chainStart + semi;
    } else {
        return fail("Configure's body is neither an expression nor a block");
    }

    const std::string_view oldChain = csText.substr(chainStart, chainEnd - chainStart);

    // The indentation of the first continuation line, so a save does not reformat the file.
    std::string indent = "        ";
    if (const usize nl = oldChain.find('\n'); nl != std::string_view::npos) {
        usize k = nl + 1;
        std::string got;
        while (k < oldChain.size() && (oldChain[k] == ' ' || oldChain[k] == '\t')) got += oldChain[k++];
        if (!got.empty()) indent = got;
    }

    const std::string chain =
        materialConfigureChain(d, extras, existingComments(oldChain), indent);

    out.assign(csText.substr(0, chainStart));
    if (!expressionBodied && csText.compare(chainStart, 7, "return ") == 0) out += "return ";
    out += chain;
    out.append(csText.substr(chainEnd));
    return true;
}

// Writes a whole new C# material source file. The class name is the bound name minus any `M_`.
std::string newMaterialScript(const std::string& boundName, const std::string& csharpNamespace,
                              const pbr::MaterialDesc& d, const OcMatExtras* extras) {
    std::string cls = boundName;
    if (cls.rfind("M_", 0) == 0) cls = cls.substr(2);
    if (cls.empty() || !isIdentChar(cls[0]) || (cls[0] >= '0' && cls[0] <= '9')) cls = "Surface" + cls;
    for (char& c : cls) if (!isIdentChar(c)) c = '_';

    std::string s;
    s += "using Aver.Materials;\n\n";
    if (!csharpNamespace.empty()) s += "namespace " + csharpNamespace + ";\n\n";
    s += "/// <summary>" + boundName + ".</summary>\n";
    s += "[AverMaterial(\"" + boundName + "\")]\n";
    s += "public sealed class " + cls + " : Material\n";
    s += "{\n";
    s += "    public static void Configure(MaterialBuilder b) => ";
    s += materialConfigureChain(d, extras, {}, "        ");
    s += ";\n}\n";
    return s;
}

} // namespace aver::fmt
