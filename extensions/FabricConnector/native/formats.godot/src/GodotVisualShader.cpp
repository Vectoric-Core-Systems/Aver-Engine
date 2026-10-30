// Godot VisualShader resources as shading-language text (GodotVisualShader.hpp).
#include "aver/formats/godot/GodotVisualShader.hpp"

#include "aver/formats/godot/GodotShader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>

namespace aver::fmt::godot {
namespace {

// Port types, in Godot's PortType order (the Expression node writes these numbers).
enum class T : int { Float = 0, Int = 1, UInt = 2, V2 = 3, V3 = 4, V4 = 5, Bool = 6, Mat = 7, Sampler = 8, None = 9 };

const char* typeName(T t) {
    switch (t) {
        case T::Float: return "float"; case T::Int: return "int"; case T::UInt: return "uint"; case T::V2: return "vec2"; case T::V3: return "vec3";
        case T::V4: return "vec4"; case T::Bool: return "bool"; case T::Mat: return "mat4"; case T::Sampler: return "sampler2D"; default: return "float";
    }
}

T tFromBase(GdBase b) {
    switch (b) {
        case GdBase::Float: return T::Float; case GdBase::Int: return T::Int; case GdBase::UInt: return T::UInt; case GdBase::Vec2: return T::V2;
        case GdBase::Vec3: return T::V3; case GdBase::Vec4: return T::V4; case GdBase::Bool: return T::Bool; case GdBase::Mat4: return T::Mat;
        case GdBase::Sampler2D: return T::Sampler; default: return T::None;
    }
}

std::string num(double v) {
    char buf[40];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) std::snprintf(buf, sizeof buf, "%.1f", v);
    else std::snprintf(buf, sizeof buf, "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string zero(T t) {
    switch (t) {
        case T::Float: return "0.0"; case T::Int: return "0"; case T::UInt: return "0u"; case T::V2: return "vec2(0.0)"; case T::V3: return "vec3(0.0)";
        case T::V4: return "vec4(0.0)"; case T::Bool: return "false"; case T::Mat: return "mat4(1.0)"; default: return "0.0";
    }
}

// A property value as a literal of type `t`.
std::string literal(const GodotVariant& v, T t) {
    switch (v.type) {
        case GodotVType::Bool: return t == T::Bool ? (v.b ? "true" : "false") : (t == T::Float ? (v.b ? "1.0" : "0.0") : zero(t));
        case GodotVType::Int: case GodotVType::Float: {
            const double d = v.asNumber();
            if (t == T::Int) return std::to_string(static_cast<long long>(d));
            if (t == T::UInt) return std::to_string(static_cast<long long>(d)) + "u";
            if (t == T::Bool) return d != 0.0 ? "true" : "false";
            if (t == T::V2 || t == T::V3 || t == T::V4) return std::string(typeName(t)) + "(" + num(d) + ")";
            return num(d);
        }
        case GodotVType::Vector2: return "vec2(" + num(v.num(0)) + ", " + num(v.num(1)) + ")";
        case GodotVType::Vector3: return "vec3(" + num(v.num(0)) + ", " + num(v.num(1)) + ", " + num(v.num(2)) + ")";
        case GodotVType::Vector4: case GodotVType::Quaternion: return "vec4(" + num(v.num(0)) + ", " + num(v.num(1)) + ", " + num(v.num(2)) + ", " + num(v.num(3)) + ")";
        case GodotVType::Color:
            if (t == T::V3) return "vec3(" + num(v.num(0)) + ", " + num(v.num(1)) + ", " + num(v.num(2)) + ")";
            return "vec4(" + num(v.num(0)) + ", " + num(v.num(1)) + ", " + num(v.num(2)) + ", " + num(v.num(3)) + ")";
        case GodotVType::Transform3D: {
            // basis (9, row-major in the literal) then origin: mat4 columns are the basis columns.
            const auto n = [&](usize i) { return num(v.num(i)); };
            return "mat4(vec4(" + n(0) + ", " + n(3) + ", " + n(6) + ", 0.0), vec4(" + n(1) + ", " + n(4) + ", " + n(7) + ", 0.0), vec4(" + n(2) + ", " + n(5) + ", " + n(8) +
                   ", 0.0), vec4(" + n(9) + ", " + n(10) + ", " + n(11) + ", 1.0))";
        }
        default: return zero(t);
    }
}

// Godot's automatic conversion between connected port types.
std::string conv(const std::string& e, T from, T to) {
    if (from == to || to == T::None || from == T::None) return e;
    const bool fromVec = from == T::V2 || from == T::V3 || from == T::V4, toVec = to == T::V2 || to == T::V3 || to == T::V4;
    const bool fromNum = from == T::Float || from == T::Int || from == T::UInt;
    if (to == T::Float) {
        if (from == T::Int || from == T::UInt) return "float(" + e + ")";
        if (from == T::Bool) return "(" + e + " ? 1.0 : 0.0)";
        if (from == T::V2) return "dot(" + e + ", vec2(0.5))";
        if (from == T::V3) return "dot(" + e + ", vec3(0.333333))";
        if (from == T::V4) return "dot(" + e + ", vec4(0.25))";
    }
    if (to == T::Int) { if (fromNum) return "int(" + e + ")"; if (from == T::Bool) return "(" + e + " ? 1 : 0)"; if (fromVec) return "int(" + conv(e, from, T::Float) + ")"; }
    if (to == T::UInt) { if (fromNum) return "uint(" + e + ")"; if (from == T::Bool) return "(" + e + " ? 1u : 0u)"; if (fromVec) return "uint(" + conv(e, from, T::Float) + ")"; }
    if (to == T::Bool) { if (fromNum) return "(" + e + " > 0.0)"; if (fromVec) return "(" + conv(e, from, T::Float) + " > 0.0)"; }
    if (toVec) {
        if (fromNum) return std::string(typeName(to)) + "(" + conv(e, from, T::Float) + ")";
        if (from == T::Bool) return std::string(typeName(to)) + "(" + e + " ? 1.0 : 0.0)";
        if (from == T::V2) return to == T::V3 ? "vec3(" + e + ", 0.0)" : "vec4(" + e + ", 0.0, 0.0)";
        if (from == T::V3) return to == T::V2 ? "(" + e + ").xy" : "vec4(" + e + ", 0.0)";
        if (from == T::V4) return to == T::V2 ? "(" + e + ").xy" : "(" + e + ").xyz";
    }
    return e;
}

std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return s;
}

// Output ports of the output node (id 0), in Godot's order: {port name, type, shader code}.
struct OutPort { const char* name; T type; const char* code; };
const std::vector<OutPort>* outputPorts(const std::string& mode, const std::string& fn) {
    static const std::vector<OutPort> spatialVertex = {{"Vertex", T::V3, "VERTEX"}, {"Normal", T::V3, "NORMAL"}, {"Tangent", T::V3, "TANGENT"}, {"Binormal", T::V3, "BINORMAL"},
        {"UV", T::V2, "UV"}, {"UV2", T::V2, "UV2"}, {"Color", T::V3, "COLOR.rgb"}, {"Alpha", T::Float, "COLOR.a"}, {"Roughness", T::Float, "ROUGHNESS"},
        {"Point Size", T::Float, "POINT_SIZE"}, {"Model View Matrix", T::Mat, "MODELVIEW_MATRIX"}, {"Projection Matrix", T::Mat, "PROJECTION_MATRIX"}};
    static const std::vector<OutPort> spatialFragment = {{"Albedo", T::V3, "ALBEDO"}, {"Alpha", T::Float, "ALPHA"}, {"Metallic", T::Float, "METALLIC"},
        {"Roughness", T::Float, "ROUGHNESS"}, {"Specular", T::Float, "SPECULAR"}, {"Emission", T::V3, "EMISSION"}, {"AO", T::Float, "AO"},
        {"AO Light Affect", T::Float, "AO_LIGHT_AFFECT"}, {"Normal", T::V3, "NORMAL"}, {"Normal Map", T::V3, "NORMAL_MAP"}, {"Normal Map Depth", T::Float, "NORMAL_MAP_DEPTH"},
        {"Rim", T::Float, "RIM"}, {"Rim Tint", T::Float, "RIM_TINT"}, {"Clearcoat", T::Float, "CLEARCOAT"}, {"Clearcoat Roughness", T::Float, "CLEARCOAT_GLOSS"},
        {"Anisotropy", T::Float, "ANISOTROPY"}, {"Anisotropy Flow", T::V2, "ANISOTROPY_FLOW"}, {"Subsurf Scatter", T::Float, "SSS_STRENGTH"}, {"Backlight", T::V3, "BACKLIGHT"},
        {"Alpha Scissor Threshold", T::Float, "ALPHA_SCISSOR_THRESHOLD"}, {"Alpha Hash Scale", T::Float, "ALPHA_HASH_SCALE"},
        {"Alpha AA Edge", T::Float, "ALPHA_ANTIALIASING_EDGE"}, {"Alpha UV", T::V2, "ALPHA_TEXTURE_COORDINATE"}, {"Depth", T::Float, "DEPTH"}};
    static const std::vector<OutPort> spatialLight = {{"Diffuse", T::V3, "DIFFUSE_LIGHT"}, {"Specular", T::V3, "SPECULAR_LIGHT"}, {"Alpha", T::Float, "ALPHA"}};
    static const std::vector<OutPort> sky = {{"Color", T::V3, "COLOR"}, {"Alpha", T::Float, "ALPHA"}, {"Fog", T::V3, "FOG.rgb"}, {"Fog Alpha", T::Float, "FOG.a"}};
    static const std::vector<OutPort> fogP = {{"Density", T::Float, "DENSITY"}, {"Albedo", T::V3, "ALBEDO"}, {"Emission", T::V3, "EMISSION"}};
    if (mode == "spatial") { if (fn == "vertex") return &spatialVertex; if (fn == "fragment") return &spatialFragment; if (fn == "light") return &spatialLight; }
    if (mode == "sky" && fn == "sky") return &sky;
    if (mode == "fog" && fn == "fog") return &fogP;
    return nullptr;
}

// One node's ports and code. Inputs are %0..%9, outputs $0..$9 in the template.
struct Spec {
    std::vector<T> in;
    std::vector<std::string> inDefault;   // empty = zero(type)
    std::vector<T> out;
    std::string code;                     // template, may hold several statements
    bool unsupported = false;
    std::string reason;
};

struct Ctx {
    std::string mode, fn;
    int id = 0;
    std::vector<std::string>* uniforms = nullptr;
    std::set<std::string>* uniformNames = nullptr;
    std::vector<std::string>* globals = nullptr;
    std::set<std::string>* globalKeys = nullptr;
    std::vector<GdVisualNodeNote>* approx = nullptr;
    const GodotResource* res = nullptr;
    GdShaderType shaderType = GdShaderType::Spatial;
};

const GodotVariant* P(const GodotSubResource& n, const char* k) { return n.prop(k); }
int Pi(const GodotSubResource& n, const char* k, int d = 0) { const GodotVariant* v = n.prop(k); return v ? static_cast<int>(v->asInt(d)) : d; }
std::string Ps(const GodotSubResource& n, const char* k, const std::string& d = {}) { const GodotVariant* v = n.prop(k); return v && v->isStringLike() ? v->s : d; }
double Pd(const GodotSubResource& n, const char* k, double d = 0.0) { const GodotVariant* v = n.prop(k); return v && v->isNumber() ? v->asNumber(d) : d; }
bool Pb(const GodotSubResource& n, const char* k, bool d = false) { const GodotVariant* v = n.prop(k); return v && v->type == GodotVType::Bool ? v->b : d; }

void addGlobal(Ctx& c, const std::string& key, const std::string& code) {
    if (c.globalKeys->insert(key).second) c.globals->push_back(code);
}

// A uniform declaration, once per parameter name.
void addUniform(Ctx& c, const std::string& name, const std::string& decl) {
    if (c.uniformNames->insert(name).second) c.uniforms->push_back(decl);
}

std::string identifier(const std::string& s) {
    std::string o;
    for (char ch : s) o += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') ? ch : '_';
    if (o.empty() || std::isdigit(static_cast<unsigned char>(o[0]))) o = "p_" + o;
    return o;
}

T vecOfOpType(int opType) { return opType == 0 ? T::V2 : opType == 1 ? T::V3 : T::V4; }

// Mix / Clamp / Step / SmoothStep op types: 0 scalar, 1 vec2, 2 vec3, 3 vec4, 4 vec2+scalar, 5 vec3+scalar, 6 vec4+scalar.
void typesOfMix(int ot, T& full, T& weight) {
    static const T full_[] = {T::Float, T::V2, T::V3, T::V4, T::V2, T::V3, T::V4};
    full = full_[std::clamp(ot, 0, 6)];
    weight = ot >= 4 ? T::Float : full;
}

bool buildSpec(const GodotSubResource& n, Ctx& c, Spec& s) {
    const std::string& ty = n.type;
    const auto in2 = [&](T t) { s.in = {t, t}; s.inDefault = {zero(t), zero(t)}; s.out = {t}; };

    if (ty == "VisualShaderNodeInput") {
        const std::string name = upper(Ps(n, "input_name"));
        int count = 0;
        const GdBuiltinVar* tab = gdBuiltinVars(c.shaderType, count);
        T t = T::None;
        for (int i = 0; i < count; ++i) if (name == tab[i].name) t = tFromBase(tab[i].type);
        if (t == T::None) { s.unsupported = true; s.reason = "input '" + Ps(n, "input_name") + "' is not a built-in of this shader type"; return true; }
        s.out = {t};
        s.code = "$0 = " + name + ";";
        return true;
    }
    if (ty == "VisualShaderNodeFloatConstant") { s.out = {T::Float}; s.code = "$0 = " + num(Pd(n, "constant")) + ";"; return true; }
    if (ty == "VisualShaderNodeIntConstant") { s.out = {T::Int}; s.code = "$0 = " + std::to_string(Pi(n, "constant")) + ";"; return true; }
    if (ty == "VisualShaderNodeUIntConstant") { s.out = {T::UInt}; s.code = "$0 = " + std::to_string(Pi(n, "constant")) + "u;"; return true; }
    if (ty == "VisualShaderNodeBooleanConstant") { s.out = {T::Bool}; s.code = std::string("$0 = ") + (Pb(n, "constant") ? "true" : "false") + ";"; return true; }
    if (ty == "VisualShaderNodeColorConstant") {
        const GodotVariant* v = P(n, "constant");
        s.out = {T::V3, T::Float};
        const double a = v ? v->num(3) : 1.0;
        s.code = "$0 = " + (v ? literal(*v, T::V3) : std::string("vec3(1.0)")) + ";\n$1 = " + num(v && v->nums.size() > 3 ? a : 1.0) + ";";
        return true;
    }
    if (ty == "VisualShaderNodeVec2Constant") { s.out = {T::V2}; const GodotVariant* v = P(n, "constant"); s.code = "$0 = " + (v ? literal(*v, T::V2) : zero(T::V2)) + ";"; return true; }
    if (ty == "VisualShaderNodeVec3Constant") { s.out = {T::V3}; const GodotVariant* v = P(n, "constant"); s.code = "$0 = " + (v ? literal(*v, T::V3) : zero(T::V3)) + ";"; return true; }
    if (ty == "VisualShaderNodeVec4Constant") { s.out = {T::V4}; const GodotVariant* v = P(n, "constant"); s.code = "$0 = " + (v ? literal(*v, T::V4) : zero(T::V4)) + ";"; return true; }
    if (ty == "VisualShaderNodeTransformConstant") { s.out = {T::Mat}; const GodotVariant* v = P(n, "constant"); s.code = "$0 = " + (v ? literal(*v, T::Mat) : zero(T::Mat)) + ";"; return true; }

    // ---- parameters (uniforms) ----
    const auto qualifier = [&]() { const int q = Pi(n, "qualifier"); return q == 1 ? "global " : q == 2 ? "instance " : ""; };
    const std::string pname = identifier(Ps(n, "parameter_name", "param_" + std::to_string(c.id)));
    const auto scalarParam = [&](T t, const char* hintKind) {
        std::string hint;
        if (t == T::Float || t == T::Int) {
            const int h = Pi(n, "hint");
            if (h >= 1) {
                hint = " : hint_range(" + (t == T::Float ? num(Pd(n, "min")) : std::to_string(Pi(n, "min"))) + ", " + (t == T::Float ? num(Pd(n, "max", 1.0)) : std::to_string(Pi(n, "max", 100))) +
                       (h >= 2 ? ", " + (t == T::Float ? num(Pd(n, "step", 0.1)) : std::to_string(Pi(n, "step", 1))) : std::string()) + ")";
            }
        }
        std::string def;
        if (Pb(n, "default_value_enabled")) { const GodotVariant* v = P(n, "default_value"); if (v) def = " = " + literal(*v, t); }
        const std::string q = qualifier();
        addUniform(c, pname, q + std::string(q.empty() ? "uniform " : "uniform ") + typeName(t) + " " + pname + hint + def + ";");
        (void)hintKind;
        s.out = {t};
        s.code = "$0 = " + pname + ";";
    };
    if (ty == "VisualShaderNodeFloatParameter") { scalarParam(T::Float, ""); return true; }
    if (ty == "VisualShaderNodeIntParameter") { scalarParam(T::Int, ""); return true; }
    if (ty == "VisualShaderNodeUIntParameter") { scalarParam(T::UInt, ""); return true; }
    if (ty == "VisualShaderNodeBooleanParameter") { scalarParam(T::Bool, ""); return true; }
    if (ty == "VisualShaderNodeVec2Parameter") { scalarParam(T::V2, ""); return true; }
    if (ty == "VisualShaderNodeVec3Parameter") { scalarParam(T::V3, ""); return true; }
    if (ty == "VisualShaderNodeVec4Parameter") { scalarParam(T::V4, ""); return true; }
    if (ty == "VisualShaderNodeColorParameter") {
        std::string def;
        if (Pb(n, "default_value_enabled")) { const GodotVariant* v = P(n, "default_value"); if (v) def = " = " + literal(*v, T::V4); }
        addUniform(c, pname, std::string(qualifier()) + "uniform vec4 " + pname + " : source_color" + def + ";");
        s.out = {T::V3, T::Float};
        s.code = "$0 = " + pname + ".rgb;\n$1 = " + pname + ".a;";
        return true;
    }
    if (ty == "VisualShaderNodeTransformParameter") {
        addUniform(c, pname, std::string(qualifier()) + "uniform mat4 " + pname + ";");
        s.out = {T::Mat};
        s.code = "$0 = " + pname + ";";
        return true;
    }
    const auto samplerHints = [&](const GodotSubResource& m) {
        static const char* kType[] = {"", "source_color", "hint_normal", "hint_anisotropy"};
        static const char* kDef[] = {"hint_default_white", "hint_default_black", "hint_default_transparent"};
        static const char* kFilter[] = {"", "filter_nearest", "filter_linear", "filter_nearest_mipmap", "filter_linear_mipmap", "filter_nearest_mipmap_anisotropic", "filter_linear_mipmap_anisotropic"};
        static const char* kRepeat[] = {"", "repeat_enable", "repeat_disable"};
        static const char* kSrc[] = {"", "hint_screen_texture", "hint_depth_texture", "hint_normal_roughness_texture"};
        std::vector<std::string> h;
        const int src = Pi(m, "texture_source");
        if (src > 0 && src < 4) h.push_back(kSrc[src]);
        else {
            const int tt = Pi(m, "texture_type");
            if (tt > 0 && tt < 4) h.push_back(kType[tt]);
            const int cd = Pi(m, "color_default");
            if (tt == 0 && cd > 0 && cd < 3) h.push_back(kDef[cd]);
        }
        const int f = Pi(m, "texture_filter"), r = Pi(m, "texture_repeat");
        if (f > 0 && f < 7) h.push_back(kFilter[f]);
        if (r > 0 && r < 3) h.push_back(kRepeat[r]);
        std::string o;
        for (usize i = 0; i < h.size(); ++i) o += (i ? ", " : " : ") + h[i];
        return o;
    };
    if (ty == "VisualShaderNodeTextureParameter" || ty == "VisualShaderNodeTexture2DParameter" || ty == "VisualShaderNodeTextureParameterTriplanar") {
        if (ty.find("Triplanar") != std::string::npos) { s.unsupported = true; s.reason = "triplanar texture parameters are not converted"; return true; }
        addUniform(c, pname, std::string(qualifier()) + "uniform sampler2D " + pname + samplerHints(n) + ";");
        s.out = {T::Sampler};
        s.code = "$0 = " + pname + ";";
        return true;
    }
    if (ty == "VisualShaderNodeTexture") {
        // source: 0 texture, 1 screen, 2 2D texture, 3 2D normal map, 4 depth, 5 port, 6 3D, 7 3D normal
        const int src = Pi(n, "source");
        if (src == 6 || src == 7) { s.unsupported = true; s.reason = "3D texture sources are not converted"; return true; }
        std::string samp;
        if (src == 5) { samp = "%2"; }
        else {
            const std::string uname = "tex_" + c.fn.substr(0, 3) + "_" + std::to_string(c.id);
            std::string h;
            if (src == 1) h = " : hint_screen_texture";
            else if (src == 4) h = " : hint_depth_texture";
            else { const int tt = Pi(n, "texture_type"); h = tt == 1 ? " : source_color" : tt == 2 ? " : hint_normal" : ""; if (src == 3) h = " : hint_normal"; }
            addUniform(c, uname, "uniform sampler2D " + uname + h + ";");
            samp = uname;
            if (src == 0 || src == 2 || src == 3)
                c.approx->push_back({c.fn, c.id, ty, "the node's embedded texture is not bound: uniform '" + uname + "' reads its default colour until set as a shader parameter"});
        }
        s.in = {T::V2, T::Float, T::Sampler};
        s.inDefault = {c.mode == "spatial" && c.fn != "vertex" ? "UV" : "vec2(0.0)", "0.0", ""};
        s.out = {T::V3, T::Float, T::Sampler};
        s.code = "vec4 $t = texture(" + samp + ", %0);\n$0 = $t.rgb;\n$1 = $t.a;" + std::string(src == 5 ? "\n$2 = %2;" : "\n$2 = " + samp + ";");
        if (Pi(n, "texture_type") == 2 && src != 5) s.code = "vec4 $t = texture(" + samp + ", %0);\n$t.rgb = $t.rgb * 2.0 - 1.0;\n$0 = $t.rgb;\n$1 = $t.a;\n$2 = " + samp + ";";
        return true;
    }

    // ---- operators ----
    if (ty == "VisualShaderNodeFloatOp") {
        static const char* k[] = {"%0 + %1", "%0 - %1", "%0 * %1", "%0 / %1", "mod(%0, %1)", "pow(%0, %1)", "max(%0, %1)", "min(%0, %1)", "atan(%0, %1)", "step(%0, %1)"};
        in2(T::Float);
        s.code = std::string("$0 = ") + k[std::clamp(Pi(n, "operator"), 0, 9)] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeIntOp" || ty == "VisualShaderNodeUIntOp") {
        const bool u = ty == "VisualShaderNodeUIntOp";
        static const char* k[] = {"%0 + %1", "%0 - %1", "%0 * %1", "%0 / %1", "%0 % %1", "max(%0, %1)", "min(%0, %1)", "%0 & %1", "%0 | %1", "%0 ^ %1", "%0 << %1", "%0 >> %1"};
        in2(u ? T::UInt : T::Int);
        s.code = std::string("$0 = ") + k[std::clamp(Pi(n, "operator"), 0, 11)] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeVectorOp") {
        static const char* k[] = {"%0 + %1", "%0 - %1", "%0 * %1", "%0 / %1", "mod(%0, %1)", "pow(%0, %1)", "max(%0, %1)", "min(%0, %1)", "cross(%0, %1)", "atan(%0, %1)", "reflect(%0, %1)", "step(%0, %1)"};
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        in2(t);
        const int op = std::clamp(Pi(n, "operator"), 0, 11);
        if (op == 8 && t != T::V3) { s.unsupported = true; s.reason = "cross() takes vec3 only"; return true; }
        s.code = std::string("$0 = ") + k[op] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeColorOp") {
        in2(T::V3);
        static const char* k[] = {"vec3(1.0) - (vec3(1.0) - %0) * (vec3(1.0) - %1)", "abs(%0 - %1)", "min(%0, %1)", "max(%0, %1)",
            "vec3(vs_overlay(%0.r, %1.r), vs_overlay(%0.g, %1.g), vs_overlay(%0.b, %1.b))", "%0 / (vec3(1.0) - %1)", "vec3(1.0) - (vec3(1.0) - %0) / %1",
            "vec3(vs_soft_light(%0.r, %1.r), vs_soft_light(%0.g, %1.g), vs_soft_light(%0.b, %1.b))",
            "vec3(vs_hard_light(%0.r, %1.r), vs_hard_light(%0.g, %1.g), vs_hard_light(%0.b, %1.b))"};
        const int op = std::clamp(Pi(n, "operator"), 0, 8);
        if (op == 4) addGlobal(c, "vs_overlay", "float vs_overlay(float c1, float c2) {\n\tif (c1 < 0.5) return 2.0 * c1 * c2;\n\treturn 1.0 - 2.0 * (1.0 - c1) * (1.0 - c2);\n}\n");
        if (op == 7) addGlobal(c, "vs_soft_light", "float vs_soft_light(float c1, float c2) {\n\tif (c2 < 0.5) return 2.0 * c1 * c2 + c1 * c1 * (1.0 - 2.0 * c2);\n\treturn 2.0 * c1 * (1.0 - c2) + sqrt(c1) * (2.0 * c2 - 1.0);\n}\n");
        if (op == 8) addGlobal(c, "vs_hard_light", "float vs_hard_light(float c1, float c2) {\n\tif (c2 < 0.5) return c1 * (2.0 * c2);\n\treturn 1.0 - (1.0 - c1) * (1.0 - 2.0 * (c2 - 0.5));\n}\n");
        s.code = std::string("$0 = ") + k[op] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeFloatFunc") {
        static const char* k[] = {"sin(%0)", "cos(%0)", "tan(%0)", "asin(%0)", "acos(%0)", "atan(%0)", "sinh(%0)", "cosh(%0)", "tanh(%0)", "log(%0)", "exp(%0)", "sqrt(%0)", "abs(%0)", "sign(%0)",
            "floor(%0)", "round(%0)", "ceil(%0)", "fract(%0)", "clamp(%0, 0.0, 1.0)", "-(%0)", "acosh(%0)", "asinh(%0)", "atanh(%0)", "degrees(%0)", "exp2(%0)", "inversesqrt(%0)", "log2(%0)",
            "radians(%0)", "1.0 / (%0)", "roundEven(%0)", "trunc(%0)", "1.0 - (%0)"};
        s.in = {T::Float}; s.inDefault = {"0.0"}; s.out = {T::Float};
        s.code = std::string("$0 = ") + k[std::clamp(Pi(n, "function"), 0, 31)] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeIntFunc" || ty == "VisualShaderNodeUIntFunc") {
        const bool u = ty == "VisualShaderNodeUIntFunc";
        const T t = u ? T::UInt : T::Int;
        s.in = {t}; s.inDefault = {zero(t)}; s.out = {t};
        static const char* ki[] = {"abs(%0)", "-(%0)", "sign(%0)", "~(%0)"};
        static const char* ku[] = {"~(%0)"};
        s.code = std::string("$0 = ") + (u ? ku[0] : ki[std::clamp(Pi(n, "function"), 0, 3)]) + ";";
        return true;
    }
    if (ty == "VisualShaderNodeVectorFunc") {
        static const char* k[] = {"normalize(%0)", "clamp(%0, vec(0.0), vec(1.0))", "-(%0)", "vec(1.0) / (%0)", "abs(%0)", "acos(%0)", "acosh(%0)", "asin(%0)", "asinh(%0)", "atan(%0)", "atanh(%0)",
            "ceil(%0)", "cos(%0)", "cosh(%0)", "degrees(%0)", "exp(%0)", "exp2(%0)", "floor(%0)", "fract(%0)", "inversesqrt(%0)", "log(%0)", "log2(%0)", "radians(%0)", "round(%0)",
            "roundEven(%0)", "sign(%0)", "sin(%0)", "sinh(%0)", "sqrt(%0)", "tan(%0)", "tanh(%0)", "trunc(%0)", "vec(1.0) - (%0)"};
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        s.in = {t}; s.inDefault = {zero(t)}; s.out = {t};
        std::string code = k[std::clamp(Pi(n, "function"), 0, 32)];
        for (usize at = code.find("vec("); at != std::string::npos; at = code.find("vec(", at)) code.replace(at, 3, typeName(t));
        s.code = "$0 = " + code + ";";
        return true;
    }
    if (ty == "VisualShaderNodeVectorCompose") {
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        const int d = t == T::V2 ? 2 : t == T::V3 ? 3 : 4;
        s.in.assign(static_cast<usize>(d), T::Float);
        s.inDefault.assign(static_cast<usize>(d), "0.0");
        s.out = {t};
        s.code = std::string("$0 = ") + typeName(t) + "(%0, %1" + (d > 2 ? ", %2" : "") + (d > 3 ? ", %3" : "") + ");";
        return true;
    }
    if (ty == "VisualShaderNodeVectorDecompose") {
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        const int d = t == T::V2 ? 2 : t == T::V3 ? 3 : 4;
        s.in = {t}; s.inDefault = {zero(t)};
        s.out.assign(static_cast<usize>(d), T::Float);
        s.code = "$0 = %0.x;\n$1 = %0.y;" + std::string(d > 2 ? "\n$2 = %0.z;" : "") + (d > 3 ? "\n$3 = %0.w;" : "");
        return true;
    }
    if (ty == "VisualShaderNodeVectorLen") {
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        s.in = {t}; s.inDefault = {zero(t)}; s.out = {T::Float}; s.code = "$0 = length(%0);";
        return true;
    }
    if (ty == "VisualShaderNodeVectorDistance") {
        const T t = vecOfOpType(Pi(n, "op_type", 1));
        s.in = {t, t}; s.inDefault = {zero(t), zero(t)}; s.out = {T::Float}; s.code = "$0 = distance(%0, %1);";
        return true;
    }
    if (ty == "VisualShaderNodeDotProduct") { s.in = {T::V3, T::V3}; s.inDefault = {zero(T::V3), zero(T::V3)}; s.out = {T::Float}; s.code = "$0 = dot(%0, %1);"; return true; }
    if (ty == "VisualShaderNodeColorFunc") {
        s.in = {T::V3}; s.inDefault = {zero(T::V3)}; s.out = {T::V3};
        const int f = Pi(n, "function");
        if (f == 0) s.code = "$0 = vec3(dot(%0, vec3(0.299, 0.587, 0.114)));";
        else if (f == 3) s.code = "$0 = vec3(dot(%0, vec3(0.393, 0.769, 0.189)), dot(%0, vec3(0.349, 0.686, 0.168)), dot(%0, vec3(0.272, 0.534, 0.131)));";
        else { s.unsupported = true; s.reason = "the HSV conversion functions are not converted"; }
        return true;
    }
    if (ty == "VisualShaderNodeMix" || ty == "VisualShaderNodeClamp" || ty == "VisualShaderNodeStep" || ty == "VisualShaderNodeSmoothStep") {
        int ot = Pi(n, "op_type");
        if (ty == "VisualShaderNodeClamp") {
            // 0 float, 1 int, 2 uint, 3 vec2, 4 vec3, 5 vec4
            static const T tt[] = {T::Float, T::Int, T::UInt, T::V2, T::V3, T::V4};
            const T t = tt[std::clamp(ot, 0, 5)];
            s.in = {t, t, t}; s.inDefault = {zero(t), zero(t), t == T::Float ? "1.0" : zero(t)}; s.out = {t};
            s.code = "$0 = clamp(%0, %1, %2);";
            return true;
        }
        T full, weight;
        typesOfMix(ot, full, weight);
        if (ty == "VisualShaderNodeMix") { s.in = {full, full, weight}; s.inDefault = {zero(full), zero(full), full == T::Float ? "0.5" : std::string(typeName(full)) + "(0.5)"}; s.out = {full}; s.code = "$0 = mix(%0, %1, %2);"; }
        else if (ty == "VisualShaderNodeStep") { s.in = {weight, full}; s.inDefault = {zero(weight), zero(full)}; s.out = {full}; s.code = "$0 = step(%0, %1);"; }
        else { s.in = {weight, weight, full}; s.inDefault = {zero(weight), zero(weight), zero(full)}; s.out = {full}; s.code = "$0 = smoothstep(%0, %1, %2);"; }
        return true;
    }
    if (ty == "VisualShaderNodeFresnel") {
        const bool frag = c.mode == "spatial" && c.fn == "fragment";
        s.in = {T::V3, T::V3, T::Bool, T::Float};
        s.inDefault = {frag ? "NORMAL" : "vec3(0.0)", frag ? "VIEW" : "vec3(0.0)", "false", "1.0"};
        s.out = {T::Float};
        s.code = "$0 = %2 ? pow(clamp(dot(normalize(%0), normalize(%1)), 0.0, 1.0), %3) : pow(1.0 - clamp(dot(normalize(%0), normalize(%1)), 0.0, 1.0), %3);";
        return true;
    }
    if (ty == "VisualShaderNodeCompare") {
        // type: 0 scalar, 1 int, 2 uint, 3 vec2, 4 vec3, 5 vec4, 6 bool, 7 transform; function: ==, !=, >, >=, <, <=; condition: all / any (vectors)
        static const T tt[] = {T::Float, T::Int, T::UInt, T::V2, T::V3, T::V4, T::Bool, T::Mat};
        const int tp = std::clamp(Pi(n, "type"), 0, 7), fn = std::clamp(Pi(n, "function"), 0, 5), cond = Pi(n, "condition");
        const T t = tt[tp];
        s.in = {t, t}; s.inDefault = {zero(t), zero(t)}; s.out = {T::Bool};
        static const char* ops[] = {"==", "!=", ">", ">=", "<", "<="};
        const bool vec = t == T::V2 || t == T::V3 || t == T::V4;
        if (vec && fn >= 2) {
            static const char* fnn[] = {"", "", "greaterThan", "greaterThanEqual", "lessThan", "lessThanEqual"};
            s.code = std::string("$0 = ") + (cond == 0 ? "all(" : "any(") + fnn[fn] + "(%0, %1));";
        } else if (vec) {
            s.code = std::string("$0 = ") + (fn == 0 ? (cond == 0 ? "all(equal(%0, %1))" : "any(equal(%0, %1))") : (cond == 0 ? "all(notEqual(%0, %1))" : "any(notEqual(%0, %1))")) + ";";
        } else {
            s.code = std::string("$0 = %0 ") + ops[fn] + " %1;";
        }
        return true;
    }
    if (ty == "VisualShaderNodeIf") {
        s.in = {T::Float, T::Float, T::Float, T::V3, T::V3, T::V3};
        s.inDefault = {"0.0", "0.0", "1e-5", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"};
        s.out = {T::V3};
        // Ports 3, 4, 5 are "a == b", "a > b", "a < b".
        s.code = "if (abs(%0 - %1) < %2) {\n\t$0 = %3;\n} else if (%0 < %1) {\n\t$0 = %5;\n} else {\n\t$0 = %4;\n}";
        return true;
    }
    if (ty == "VisualShaderNodeSwitch") {
        static const T tt[] = {T::Float, T::Int, T::UInt, T::V2, T::V3, T::V4, T::Bool, T::Mat};
        const T t = tt[std::clamp(Pi(n, "op_type"), 0, 7)];
        s.in = {T::Bool, t, t}; s.inDefault = {"false", t == T::Float ? "1.0" : zero(t), zero(t)}; s.out = {t};
        s.code = "$0 = %0 ? %1 : %2;";
        return true;
    }
    if (ty == "VisualShaderNodeRemap") {
        s.in = {T::Float, T::Float, T::Float, T::Float, T::Float};
        s.inDefault = {"0.5", "0.0", "1.0", "0.0", "1.0"};
        s.out = {T::Float};
        s.code = "$0 = %3 + (%0 - %1) * (%4 - %3) / (%2 - %1);";
        return true;
    }
    if (ty == "VisualShaderNodeUVFunc") {
        const bool frag = c.mode == "spatial" && c.fn != "vertex";
        s.in = {T::V2, T::V2, T::V2};
        const int f = Pi(n, "function");
        s.inDefault = {frag ? "UV" : "vec2(0.0)", f == 0 ? "vec2(1.0)" : "vec2(1.0)", f == 0 ? "vec2(0.0)" : "vec2(0.5)"};
        s.out = {T::V2};
        s.code = f == 0 ? "$0 = %2 * %1 + %0;" : "$0 = (%0 - %2) * %1 + %2;";
        return true;
    }
    if (ty == "VisualShaderNodeTransformVecMult") {
        s.in = {T::Mat, T::V3}; s.inDefault = {"mat4(1.0)", "vec3(0.0)"}; s.out = {T::V3};
        static const char* k[] = {"(%0 * vec4(%1, 1.0)).xyz", "(vec4(%1, 1.0) * %0).xyz", "(mat3(%0) * %1)", "(%1 * mat3(%0))"};
        s.code = std::string("$0 = ") + k[std::clamp(Pi(n, "operator"), 0, 3)] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeTransformFunc") {
        s.in = {T::Mat}; s.inDefault = {"mat4(1.0)"}; s.out = {T::Mat};
        s.code = Pi(n, "function") == 0 ? "$0 = inverse(%0);" : "$0 = transpose(%0);";
        return true;
    }
    if (ty == "VisualShaderNodeTransformOp") {
        // AxB, BxA, AxB_COMP, BxA_COMP, ADD, A_MINUS_B, B_MINUS_A, A_DIV_B, B_DIV_A (the _COMP and divide forms work per column).
        static const char* k[] = {"%0 * %1", "%1 * %0", "mat4(%0[0] * %1[0], %0[1] * %1[1], %0[2] * %1[2], %0[3] * %1[3])", "mat4(%1[0] * %0[0], %1[1] * %0[1], %1[2] * %0[2], %1[3] * %0[3])",
            "%0 + %1", "%0 - %1", "%1 - %0", "mat4(%0[0] / %1[0], %0[1] / %1[1], %0[2] / %1[2], %0[3] / %1[3])", "mat4(%1[0] / %0[0], %1[1] / %0[1], %1[2] / %0[2], %1[3] / %0[3])"};
        s.in = {T::Mat, T::Mat}; s.inDefault = {"mat4(1.0)", "mat4(1.0)"}; s.out = {T::Mat};
        s.code = std::string("$0 = ") + k[std::clamp(Pi(n, "operator"), 0, 8)] + ";";
        return true;
    }
    if (ty == "VisualShaderNodeTransformCompose") {
        s.in = {T::V3, T::V3, T::V3, T::V3}; s.inDefault.assign(4, "vec3(0.0)"); s.out = {T::Mat};
        s.code = "$0 = mat4(vec4(%0, 0.0), vec4(%1, 0.0), vec4(%2, 0.0), vec4(%3, 1.0));";
        return true;
    }
    if (ty == "VisualShaderNodeTransformDecompose") {
        s.in = {T::Mat}; s.inDefault = {"mat4(1.0)"}; s.out = {T::V3, T::V3, T::V3, T::V3};
        s.code = "$0 = %0[0].xyz;\n$1 = %0[1].xyz;\n$2 = %0[2].xyz;\n$3 = %0[3].xyz;";
        return true;
    }
    if (ty == "VisualShaderNodeReroute") {
        static const T tt[] = {T::Float, T::Int, T::UInt, T::V2, T::V3, T::V4, T::Bool, T::Mat, T::Sampler};   // Godot's PortType order
        const T t = tt[std::clamp(Pi(n, "port_type"), 0, 8)];
        s.in = {t}; s.inDefault = {zero(t)}; s.out = {t}; s.code = "$0 = %0;";
        return true;
    }
    if (ty == "VisualShaderNodeExpression") {
        // input_ports / output_ports: "index,type,name;index,type,name;"
        struct Pt { int idx; T type; std::string name; };
        const auto parse = [&](const std::string& text) {
            std::vector<Pt> v;
            usize b = 0;
            while (b < text.size()) {
                const usize e = std::min(text.find(';', b), text.size());
                const std::string item = text.substr(b, e - b);
                b = e + 1;
                const usize c1 = item.find(','), c2 = c1 == std::string::npos ? c1 : item.find(',', c1 + 1);
                if (c2 == std::string::npos) continue;
                v.push_back({std::atoi(item.substr(0, c1).c_str()), static_cast<T>(std::clamp(std::atoi(item.substr(c1 + 1, c2 - c1 - 1).c_str()), 0, 8)), item.substr(c2 + 1)});
            }
            return v;
        };
        const std::vector<Pt> ins = parse(Ps(n, "input_ports")), outs = parse(Ps(n, "output_ports"));
        std::string body = "{\n";
        for (usize i = 0; i < ins.size(); ++i) {
            s.in.push_back(ins[i].type);
            s.inDefault.push_back(zero(ins[i].type));
            body += std::string("\t") + typeName(ins[i].type) + " " + ins[i].name + " = %" + std::to_string(i) + ";\n";
        }
        for (const Pt& o : outs) { s.out.push_back(o.type); body += std::string("\t") + typeName(o.type) + " " + o.name + " = " + zero(o.type) + ";\n"; }
        body += "\t" + Ps(n, "expression") + "\n";
        for (usize i = 0; i < outs.size(); ++i) body += "\t$" + std::to_string(i) + " = " + outs[i].name + ";\n";
        body += "}";
        s.code = body;
        return true;
    }
    if (ty == "VisualShaderNodeGlobalExpression") {
        addGlobal(c, "gx" + std::to_string(c.id) + c.fn, Ps(n, "expression") + "\n");
        s.code = "";
        return true;
    }
    if (ty == "VisualShaderNodeFrame" || ty == "VisualShaderNodeComment") { s.code = ""; return true; }
    s.unsupported = true;
    s.reason = ty.rfind("VisualShaderNodeParticle", 0) == 0 ? "particle emission and collision nodes are not converted" :
               ty.find("Varying") != std::string::npos ? "varyings are not converted" :
               ty == "VisualShaderNodeCustom" || ty.rfind("VisualShaderNodeCustom", 0) == 0 ? "script-defined nodes are not converted" :
               "this node class is not converted";
    return true;
}

}  // namespace

GdVisualShaderResult gdVisualShaderToCode(const GodotResource& res) {
    GdVisualShaderResult out;
    if (res.header.type != "VisualShader") { out.error = "the resource is a " + res.header.type + ", not a VisualShader"; return out; }
    const GodotVariant* modeV = res.prop("mode");
    const int mode = modeV ? static_cast<int>(modeV->asInt()) : 0;
    static const char* kModes[] = {"spatial", "canvas_item", "particles", "sky", "fog"};
    out.shaderType = kModes[std::clamp(mode, 0, 4)];
    if (mode == 1) { out.error = "VisualShader mode canvas_item (2D) is not supported"; return out; }
    if (mode == 2) { out.error = "particles VisualShaders end in particle output nodes (VisualShaderNodeParticle*) that are not converted; write the particle shader as a .gdshader"; return out; }

    std::vector<std::string> fns;
    if (mode == 0) fns = {"vertex", "fragment", "light"};
    else if (mode == 3) fns = {"sky"};
    else fns = {"fog"};

    std::vector<std::string> uniforms, globals, bodies;
    std::set<std::string> uniformNames, globalKeys;
    const GdShaderType st = mode == 0 ? GdShaderType::Spatial : mode == 3 ? GdShaderType::Sky : GdShaderType::Fog;
    bool anyConnected = false;

    for (const std::string& fn : fns) {
        const std::vector<OutPort>* ports = outputPorts(out.shaderType, fn);
        const GodotVariant* cv = res.prop("nodes/" + fn + "/connections");
        if (!cv || cv->nums.size() < 4) continue;
        struct Conn { int from, fromPort, to, toPort; };
        std::vector<Conn> conns;
        for (usize i = 0; i + 3 < cv->nums.size(); i += 4)
            conns.push_back({static_cast<int>(cv->nums[i]), static_cast<int>(cv->nums[i + 1]), static_cast<int>(cv->nums[i + 2]), static_cast<int>(cv->nums[i + 3])});
        // The nodes of this function.
        std::map<int, const GodotSubResource*> nodes;
        const std::string pre = "nodes/" + fn + "/";
        for (const GodotProperty& p : res.props) {
            if (p.name.compare(0, pre.size(), pre) != 0) continue;
            const std::string rest = p.name.substr(pre.size());
            const usize slash = rest.find('/');
            if (slash == std::string::npos || rest.substr(slash + 1) != "node") continue;
            const int id = std::atoi(rest.substr(0, slash).c_str());
            if (const GodotSubResource* sub = res.findSub(p.value.asString())) nodes[id] = sub;
        }
        if (!ports) {
            out.unsupported.push_back({fn, 0, "output", "no output ports are defined for this function in this shader type"});
            continue;
        }

        struct Out { T type = T::None; std::string var; };
        std::map<std::pair<int, int>, Out> produced;   // (node, port)
        std::set<int> visiting, done;
        std::string code;
        std::vector<GdVisualNodeNote> approx;
        Ctx c;
        c.mode = out.shaderType;
        c.fn = fn;
        c.uniforms = &uniforms; c.uniformNames = &uniformNames; c.globals = &globals; c.globalKeys = &globalKeys; c.approx = &out.approximated; c.res = &res; c.shaderType = st;

        std::function<void(int)> gen = [&](int id) {
            if (done.count(id) || !visiting.insert(id).second) return;
            const auto it = nodes.find(id);
            if (it == nodes.end()) { done.insert(id); return; }
            c.id = id;
            Spec s;
            Ctx local = c;
            local.id = id;
            buildSpec(*it->second, local, s);
            if (s.unsupported) {
                out.unsupported.push_back({fn, id, it->second->type, s.reason});
                done.insert(id);
                return;
            }
            // inputs
            std::vector<std::string> inExpr(s.in.size());
            for (usize p = 0; p < s.in.size(); ++p) {
                std::string e;
                bool connected = false;
                for (const Conn& cn : conns) {
                    if (cn.to != id || cn.toPort != static_cast<int>(p)) continue;
                    gen(cn.from);
                    const auto pr = produced.find({cn.from, cn.fromPort});
                    if (pr != produced.end()) { e = conv(pr->second.var, pr->second.type, s.in[p]); connected = true; }
                    break;
                }
                if (!connected) {
                    // default_input_values: [port, value, port, value, ...]
                    const GodotVariant* dv = it->second->prop("default_input_values");
                    bool found = false;
                    if (dv) {
                        for (usize k = 0; k + 1 < dv->items.size(); k += 2) {
                            if (dv->items[k].asInt(-1) == static_cast<i64>(p)) { e = literal(dv->items[k + 1], s.in[p]); found = true; }
                        }
                    }
                    if (!found) e = p < s.inDefault.size() && !s.inDefault[p].empty() ? s.inDefault[p] : zero(s.in[p]);
                    if (s.in[p] == T::Sampler && e.empty()) e = "0";
                }
                inExpr[p] = e;
            }
            // outputs: locals
            std::string stmts = s.code;
            std::string decl;
            for (usize o = 0; o < s.out.size(); ++o) {
                const std::string var = "n_out" + std::to_string(id) + "p" + std::to_string(o);
                decl += std::string(typeName(s.out[o])) + " " + var + (s.out[o] == T::Sampler ? "" : " = " + zero(s.out[o])) + ";\n";
                produced[{id, static_cast<int>(o)}] = {s.out[o], var};
            }
            // substitute %N then $N (high indices first so %1 does not eat %10)
            const auto subst = [&](std::string& t, char mark, const std::vector<std::string>& vals) {
                for (int k = static_cast<int>(vals.size()) - 1; k >= 0; --k) {
                    const std::string key = std::string(1, mark) + std::to_string(k);
                    for (usize at = t.find(key); at != std::string::npos; at = t.find(key, at)) { t.replace(at, key.size(), vals[static_cast<usize>(k)]); at += vals[static_cast<usize>(k)].size(); }
                }
            };
            std::vector<std::string> outNames;
            for (usize o = 0; o < s.out.size(); ++o) outNames.push_back("n_out" + std::to_string(id) + "p" + std::to_string(o));
            if (s.in.size() > 0 && s.in[0] == T::Sampler) {} // samplers pass through as expressions
            for (usize at = stmts.find("$t"); at != std::string::npos; at = stmts.find("$t", at)) stmts.replace(at, 2, "n_tmp" + std::to_string(id));
            subst(stmts, '%', inExpr);
            subst(stmts, '$', outNames);
            // Sampler outputs are aliased by expression rather than copied (a local sampler2D is not valid).
            for (usize o = 0; o < s.out.size(); ++o) {
                if (s.out[o] != T::Sampler) continue;
                const std::string var = outNames[o];
                const usize at = stmts.find(var + " = ");
                if (at != std::string::npos) {
                    const usize e = stmts.find(';', at);
                    produced[{id, static_cast<int>(o)}].var = stmts.substr(at + var.size() + 3, e - at - var.size() - 3);
                    stmts.erase(at, e - at + 1);
                }
                const usize dpos = decl.find("sampler2D " + var);
                if (dpos != std::string::npos) decl.erase(dpos, decl.find('\n', dpos) - dpos + 1);
            }
            code += decl + stmts + (stmts.empty() ? "" : "\n");
            ++out.nodeCount;
            done.insert(id);
        };

        std::string tail;
        for (const Conn& cn : conns) {
            if (cn.to != 0 || cn.toPort < 0 || cn.toPort >= static_cast<int>(ports->size())) continue;
            gen(cn.from);
            const auto pr = produced.find({cn.from, cn.fromPort});
            if (pr == produced.end()) continue;
            const OutPort& op = (*ports)[static_cast<usize>(cn.toPort)];
            tail += std::string("\t") + op.code + " = " + conv(pr->second.var, pr->second.type, op.type) + ";\n";
            anyConnected = true;
        }
        // Godot emits every node's globals, wired or not: global expressions and parameters (uniforms) have nothing to be reached from.
        for (const auto& [id, sub] : nodes) {
            if (done.count(id) || (sub->type != "VisualShaderNodeGlobalExpression" && sub->type.find("Parameter") == std::string::npos)) continue;
            Ctx local = c;
            local.id = id;
            Spec unused;
            buildSpec(*sub, local, unused);
        }
        std::string indented;
        for (usize b = 0; b < code.size();) {
            const usize e = std::min(code.find('\n', b), code.size());
            const std::string line = code.substr(b, e - b);
            if (!line.empty()) indented += "\t" + line + "\n";
            b = e + 1;
        }
        bodies.push_back("void " + fn + "() {\n" + indented + (indented.empty() || tail.empty() ? "" : "\n") + tail + "}\n");
    }
    if (!anyConnected && out.unsupported.empty()) { out.error = "the VisualShader has no connection to its output node"; return out; }

    std::string code = std::string("shader_type ") + out.shaderType + ";\n";
    if (mode == 0) {
        static const char* kBlend[] = {"mix", "add", "sub", "mul", "premul_alpha"};
        static const char* kDepth[] = {"opaque", "always", "never"};
        static const char* kCull[] = {"back", "front", "disabled"};
        static const char* kDiffuse[] = {"lambert", "lambert_wrap", "burley", "toon"};
        static const char* kSpec[] = {"schlick_ggx", "toon", "disabled"};
        std::vector<std::string> modes;
        const auto en = [&](const char* key, const char* const* names, int count, const char* prefix) {
            if (const GodotVariant* v = res.prop(key)) { const int i = static_cast<int>(v->asInt()); if (i > 0 && i < count) modes.push_back(std::string(prefix) + names[i]); }
        };
        en("modes/blend", kBlend, 5, "blend_");
        en("modes/depth_draw", kDepth, 3, "depth_draw_");
        en("modes/cull", kCull, 3, "cull_");
        en("modes/diffuse", kDiffuse, 4, "diffuse_");
        en("modes/specular", kSpec, 3, "specular_");
        for (const GodotProperty& p : res.props)
            if (p.name.compare(0, 6, "flags/") == 0 && p.value.type == GodotVType::Bool && p.value.b) modes.push_back(p.name.substr(6));
        if (!modes.empty()) {
            code += "render_mode ";
            for (usize i = 0; i < modes.size(); ++i) code += (i ? ", " : "") + modes[i];
            code += ";\n";
        }
    }
    code += "\n";
    for (const std::string& u : uniforms) code += u + "\n";
    if (!uniforms.empty()) code += "\n";
    for (const std::string& g : globals) code += g + "\n";
    for (const std::string& b : bodies) code += b + "\n";
    out.code = code;
    out.ok = true;
    return out;
}

}  // namespace aver::fmt::godot
