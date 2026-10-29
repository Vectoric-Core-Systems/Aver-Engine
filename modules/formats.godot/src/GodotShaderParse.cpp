// Godot shading language: lexer, parser and type checker in one pass. Godot requires every name to be declared before use (functions included),
// so the parser resolves identifiers and types while it builds the AST and the result is fully typed. The first error stops the parse; the
// diagnostic carries file:line:col of the offending token. Written from the documented language, no Godot source copied.
#include "GodotShaderInternal.hpp"

#include "aver/core/CharConv.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace aver::fmt::godot {
namespace {

using B = GdBase;

// ------------------------------------------------------------------------------------------------ lexer

enum class TK : u8 { Ident, Int, UInt, Float, String, Punct, End };

struct Token {
    TK k = TK::End;
    std::string text;
    double num = 0;
    int line = 0, col = 0;
    u16 file = 0;
};

bool isIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

struct LexError { std::string msg; int line, col; u16 file; };

bool lexShader(const GdPreprocessed& pp, std::vector<Token>& out, LexError& err) {
    static const char* puncts[] = {"<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
                                   "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&", "|", "^", "?", ":", ";", ",", ".", "(", ")", "[", "]", "{", "}"};
    usize lineStart = 0;
    for (usize li = 0; li < pp.lines.size(); ++li) {
        usize e = pp.text.find('\n', lineStart);
        if (e == std::string::npos) e = pp.text.size();
        const std::string_view s(pp.text.data() + lineStart, e - lineStart);
        lineStart = e + 1;
        const GdLineOrigin& org = pp.lines[li];
        for (usize i = 0; i < s.size();) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
            Token t;
            t.line = org.line;
            t.col = static_cast<int>(i) + 1;
            t.file = org.file;
            if (isIdStart(c)) {
                usize j = i;
                while (j < s.size() && isIdChar(s[j])) ++j;
                t.k = TK::Ident;
                t.text = std::string(s.substr(i, j - i));
                i = j;
            } else if (isDigit(c) || (c == '.' && i + 1 < s.size() && isDigit(s[i + 1]))) {
                usize j = i;
                if (c == '0' && j + 1 < s.size() && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                    j += 2;
                    const usize hs = j;
                    while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
                    if (j == hs) { err = {"invalid hexadecimal literal", t.line, t.col, t.file}; return false; }
                    unsigned long long hv = 0;
                    if (fromChars(s.data() + hs, s.data() + j, hv, 16).ec != std::errc()) { err = {"integer literal is too large", t.line, t.col, t.file}; return false; }
                    t.num = static_cast<double>(hv);
                    t.k = TK::Int;
                    if (j < s.size() && (s[j] == 'u' || s[j] == 'U')) { t.k = TK::UInt; ++j; }
                } else {
                    bool isFloat = false;
                    while (j < s.size() && isDigit(s[j])) ++j;
                    if (j < s.size() && s[j] == '.') { isFloat = true; ++j; while (j < s.size() && isDigit(s[j])) ++j; }
                    if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                        usize k = j + 1;
                        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                        if (k < s.size() && isDigit(s[k])) {
                            isFloat = true;
                            while (k < s.size() && isDigit(s[k])) ++k;
                            j = k;
                        }
                    }
                    const std::string txt(s.substr(i, j - i));
                    if (isFloat) {
                        f64 v = 0;
                        const auto r = fromChars(txt.data(), txt.data() + txt.size(), v);
                        if (r.ec != std::errc()) { err = {"invalid float literal '" + txt + "'", t.line, t.col, t.file}; return false; }
                        t.num = v;
                        t.k = TK::Float;
                        if (j < s.size() && (s[j] == 'f' || s[j] == 'F')) ++j;
                    } else {
                        unsigned long long iv = 0;
                        if (fromChars(txt.data(), txt.data() + txt.size(), iv).ec != std::errc()) { err = {"integer literal is too large", t.line, t.col, t.file}; return false; }
                        t.num = static_cast<double>(iv);
                        t.k = TK::Int;
                        if (j < s.size() && (s[j] == 'u' || s[j] == 'U')) { t.k = TK::UInt; ++j; }
                        else if (j < s.size() && (s[j] == 'f' || s[j] == 'F')) { t.k = TK::Float; ++j; }
                    }
                }
                if (j < s.size() && isIdChar(s[j])) { err = {"invalid suffix on a numeric literal", t.line, static_cast<int>(j) + 1, t.file}; return false; }
                t.text = std::string(s.substr(i, j - i));
                i = j;
            } else if (c == '"') {
                usize j = i + 1;
                while (j < s.size() && s[j] != '"') ++j;
                if (j >= s.size()) { err = {"unterminated string literal", t.line, t.col, t.file}; return false; }
                t.k = TK::String;
                t.text = std::string(s.substr(i + 1, j - i - 1));
                i = j + 1;
            } else {
                bool found = false;
                for (const char* p : puncts) {
                    const usize n = std::char_traits<char>::length(p);
                    if (s.compare(i, n, p) == 0) { t.k = TK::Punct; t.text = p; i += n; found = true; break; }
                }
                if (!found) { err = {std::string("unexpected character '") + c + "'", t.line, t.col, t.file}; return false; }
            }
            out.push_back(std::move(t));
        }
    }
    Token end;
    end.k = TK::End;
    if (!out.empty()) { end.line = out.back().line; end.col = out.back().col; end.file = out.back().file; }
    out.push_back(end);
    return true;
}

// ------------------------------------------------------------------------------------------------ parser

struct Abort {};

struct LocalVar {
    GdType type;
    bool isConst = false, isParam = false;
    GdParamQual qual = GdParamQual::In;
    std::shared_ptr<const GdConstValue> cv;
};

bool isPrecision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }

bool isKeyword(const std::string& s) {
    static const std::set<std::string> kw = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "discard", "const", "struct",
        "uniform", "varying", "in", "out", "inout", "true", "false", "shader_type", "render_mode", "lowp", "mediump", "highp", "flat", "smooth",
        "global", "instance", "group_uniforms"};
    return kw.count(s) != 0;
}

class Parser {
public:
    Parser(GdShaderModule& m, const GdCompileOptions& opt, std::vector<Token> toks) : m_(m), opt_(opt), t_(std::move(toks)) {}

    void parseAll() {
        try {
            globalScope();
        } catch (const Abort&) {
        }
    }

private:
    GdShaderModule& m_;
    const GdCompileOptions& opt_;
    std::vector<Token> t_;
    usize p_ = 0;

    std::vector<std::map<std::string, LocalVar>> scopes_;
    GdFunction* fn_ = nullptr;
    GdProcessor proc_ = GdProcessor::None;
    int loopDepth_ = 0, switchDepth_ = 0;
    std::string group_, subgroup_;
    int instanceCount_ = 0;
    std::set<std::string> warnedVarying_;
    int bvCount_ = 0;
    const GdBuiltinVar* bv_ = nullptr;

    // ---- diagnostics
    void addDiag(GodotSeverity sev, const Token& at, const std::string& msg, const char* code) {
        GdShaderDiag d;
        d.severity = sev;
        d.file = at.file < m_.files.size() ? m_.files[at.file] : std::string();
        d.line = at.line;
        d.column = at.col;
        d.code = code;
        d.message = msg;
        m_.diags.push_back(std::move(d));
    }
    [[noreturn]] void fail(const Token& at, const std::string& msg, const char* code = "gdshader.syntax") {
        addDiag(GodotSeverity::Error, at, msg, code);
        throw Abort{};
    }
    [[noreturn]] void typeFail(const Token& at, const std::string& msg) { fail(at, msg, "gdshader.type"); }
    void warn(const Token& at, const std::string& msg, const char* code = "gdshader.warn") { addDiag(GodotSeverity::Warning, at, msg, code); }

    // ---- token access
    const Token& peek(usize o = 0) const { return t_[std::min(p_ + o, t_.size() - 1)]; }
    const Token& next() { const Token& t = t_[p_]; if (p_ + 1 < t_.size()) ++p_; return t; }
    bool isP(const char* s, usize o = 0) const { const Token& t = peek(o); return t.k == TK::Punct && t.text == s; }
    bool isId(const char* s, usize o = 0) const { const Token& t = peek(o); return t.k == TK::Ident && t.text == s; }
    bool acceptP(const char* s) { if (isP(s)) { next(); return true; } return false; }
    bool acceptId(const char* s) { if (isId(s)) { next(); return true; } return false; }
    void expectP(const char* s) {
        if (!isP(s)) fail(peek(), std::string("expected '") + s + "' but found " + describe(peek()));
        next();
    }
    std::string describe(const Token& t) const {
        switch (t.k) {
        case TK::End: return "end of shader";
        case TK::Ident: return "'" + t.text + "'";
        case TK::String: return "string \"" + t.text + "\"";
        default: return "'" + t.text + "'";
        }
    }
    std::string expectIdent(const char* what) {
        const Token& t = peek();
        if (t.k != TK::Ident || isKeyword(t.text)) fail(t, std::string("expected ") + what + " but found " + describe(t));
        return next().text;
    }

    // ---- scopes and lookup
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    LocalVar* findLocal(const std::string& n) {
        for (usize i = scopes_.size(); i-- > 0;) {
            auto it = scopes_[i].find(n);
            if (it != scopes_[i].end()) return &it->second;
        }
        return nullptr;
    }
    void declareLocal(const Token& at, const std::string& n, LocalVar v) {
        if (scopes_.back().count(n)) fail(at, "redefinition of '" + n + "' in this scope", "gdshader.type");
        scopes_.back()[n] = std::move(v);
    }

    bool isTypeName(const std::string& s) const { return gdBaseFromName(s) != B::Error || m_.findStruct(s) >= 0; }
    GdType typeFromName(const std::string& s) const {
        GdType t;
        const GdBase b = gdBaseFromName(s);
        if (b != B::Error) { t.base = b; return t; }
        t.base = B::Struct;
        t.structIdx = static_cast<i16>(m_.findStruct(s));
        return t;
    }
    std::string tn(const GdType& t) const { return gdTypeName(t, &m_); }

    GdExprP mk(GdExprKind k, const Token& at) {
        auto e = std::make_unique<GdExpr>();
        e->kind = k;
        e->line = at.line;
        e->col = at.col;
        return e;
    }

    // ---- array sizes
    int arraySizeFromExpr(const Token& at, const GdExpr& e) {
        GdConstValue v;
        if (!detail::foldConst(e, m_, v) || (gdKind(v.type.base) != GdKind::Int && gdKind(v.type.base) != GdKind::UInt) || gdDim(v.type.base) != 1)
            typeFail(at, "array size must be a constant integer expression");
        if (v.v[0] < 1 || v.v[0] > 65536) typeFail(at, "array size must be between 1 and 65536");
        return static_cast<int>(v.v[0]);
    }
    // After a type or a name: optional [N] / [] . Returns 0 = none, -1 = unsized, else N.
    int parseArraySuffix() {
        if (!isP("[")) return 0;
        const Token at = next();
        if (acceptP("]")) return -1;
        GdExprP e = parseTernary();
        expectP("]");
        return arraySizeFromExpr(at, *e);
    }

    // [const] [precision] type: the type name only (array suffix handled by the caller)
    GdType parseTypeName(bool allowVoid) {
        while (peek().k == TK::Ident && isPrecision(peek().text)) next();
        const Token& t = peek();
        if (t.k != TK::Ident) fail(t, "expected a type but found " + describe(t));
        if (t.text == "void") {
            if (!allowVoid) fail(t, "'void' is not allowed here");
            next();
            return GdType{};
        }
        if (!isTypeName(t.text)) fail(t, "unknown type '" + t.text + "'");
        next();
        return typeFromName(t.text);
    }

    // ================================================================= global scope
    void globalScope() {
        scopes_.clear();
        // shader_type must come first
        if (!isId("shader_type")) fail(peek(), "the shader must begin with 'shader_type'");
        next();
        const Token tt = peek();
        const std::string st = expectIdent("a shader type");
        if (st == "spatial") m_.shaderType = GdShaderType::Spatial;
        else if (st == "canvas_item") { m_.shaderType = GdShaderType::CanvasItem; m_.is2D = true; }
        else if (st == "particles") m_.shaderType = GdShaderType::Particles;
        else if (st == "sky") m_.shaderType = GdShaderType::Sky;
        else if (st == "fog") m_.shaderType = GdShaderType::Fog;
        else fail(tt, "unknown shader type '" + st + "' (spatial, canvas_item, particles, sky, fog)");
        m_.hasShaderType = true;
        expectP(";");
        bv_ = gdBuiltinVars(m_.shaderType, bvCount_);
        if (m_.is2D) warn(tt, "canvas_item (2D) shaders are not supported by the converter: parsed and checked only", "gdshader.unsupported_2d");

        while (peek().k != TK::End) globalDecl();
    }

    void globalDecl() {
        const Token& t = peek();
        if (t.k != TK::Ident) fail(t, "unexpected " + describe(t) + " at global scope");
        const std::string& s = t.text;
        if (s == "shader_type") fail(t, "'shader_type' may appear only once, at the start");
        if (s == "render_mode") return renderMode();
        if (s == "group_uniforms") return groupUniforms();
        if (s == "uniform" || s == "global" || s == "instance") return uniformDecl();
        if (s == "varying" || s == "flat" || s == "smooth") return varyingDecl();
        if (s == "struct") return structDecl();
        if (s == "const") return globalConst();
        return functionDef();
    }

    void renderMode() {
        next();
        for (;;) {
            const Token at = peek();
            const std::string mode = expectIdent("a render mode");
            if (!gdRenderModeValid(m_.shaderType, mode)) fail(at, "unknown render mode '" + mode + "' for " + gdShaderTypeName(m_.shaderType) + " shaders");
            if (std::find(m_.renderModes.begin(), m_.renderModes.end(), mode) != m_.renderModes.end()) warn(at, "render mode '" + mode + "' repeated");
            else m_.renderModes.push_back(mode);
            if (!acceptP(",")) break;
        }
        expectP(";");
    }

    void groupUniforms() {
        next();
        group_.clear();
        subgroup_.clear();
        if (!isP(";")) {
            group_ = expectIdent("a group name");
            if (acceptP(".")) subgroup_ = expectIdent("a subgroup name");
        }
        expectP(";");
    }

    void checkGlobalName(const Token& at, const std::string& name) {
        if (isTypeName(name) || isKeyword(name) || gdIsBuiltinFunction(name))
            fail(at, "'" + name + "' cannot be used as a name here", "gdshader.type");
        if (m_.findUniform(name) >= 0 || m_.findVarying(name) >= 0) fail(at, "redefinition of '" + name + "'", "gdshader.type");
        for (const GdConstDecl& c : m_.consts) if (c.name == name) fail(at, "redefinition of '" + name + "'", "gdshader.type");
        for (const GdFunction& f : m_.functions) if (f.name == name) fail(at, "'" + name + "' is already a function", "gdshader.type");
    }

    // ---- struct
    void structDecl() {
        const Token at = next();
        const Token nameTok = peek();
        const std::string name = expectIdent("a struct name");
        if (isTypeName(name)) fail(nameTok, "redefinition of type '" + name + "'", "gdshader.type");
        expectP("{");
        GdStruct st;
        st.name = name;
        st.line = at.line;
        while (!isP("}")) {
            if (peek().k == TK::End) fail(peek(), "unterminated struct");
            GdType ty = parseTypeName(false);
            if (gdIsSampler(ty.base)) fail(peek(), "a struct member cannot be a sampler");
            const int tsz = parseArraySuffix();
            for (;;) {
                const Token mt = peek();
                const std::string mn = expectIdent("a member name");
                for (const GdStructMember& o : st.members) if (o.name == mn) fail(mt, "duplicate member '" + mn + "'", "gdshader.type");
                GdType mty = ty;
                int sz = parseArraySuffix();
                if (sz == 0) sz = tsz;
                if (sz < 0) fail(mt, "a struct member array needs an explicit size");
                mty.arraySize = sz > 0 ? sz : 0;
                st.members.push_back({mn, mty});
                if (!acceptP(",")) break;
            }
            expectP(";");
        }
        next();
        expectP(";");
        if (st.members.empty()) fail(at, "a struct needs at least one member", "gdshader.type");
        m_.structs.push_back(std::move(st));
    }

    // ---- varying
    void varyingDecl() {
        GdInterp interp = GdInterp::Smooth;
        bool seenVarying = false;
        const Token at = peek();
        for (;;) {
            if (acceptId("flat")) interp = GdInterp::Flat;
            else if (acceptId("smooth")) interp = GdInterp::Smooth;
            else if (!seenVarying && acceptId("varying")) seenVarying = true;
            else break;
        }
        if (!seenVarying) fail(at, "expected 'varying'");
        GdType ty = parseTypeName(false);
        if (gdIsSampler(ty.base) || ty.base == B::Struct) fail(at, "a varying cannot be a sampler or a struct", "gdshader.type");
        const int tsz = parseArraySuffix();
        for (;;) {
            const Token nt = peek();
            const std::string name = expectIdent("a varying name");
            checkGlobalName(nt, name);
            int sz = parseArraySuffix();
            if (sz == 0) sz = tsz;
            if (sz < 0) fail(nt, "a varying array needs an explicit size");
            GdVarying v;
            v.name = name;
            v.type = ty;
            v.type.arraySize = sz > 0 ? sz : 0;
            v.interp = interp;
            v.line = nt.line;
            if (isP("=")) fail(peek(), "a varying cannot have an initialiser");
            if (m_.shaderType == GdShaderType::Fog || m_.shaderType == GdShaderType::Sky || m_.shaderType == GdShaderType::Particles)
                warn(nt, "varyings are not used by " + std::string(gdShaderTypeName(m_.shaderType)) + " shaders");
            m_.varyings.push_back(std::move(v));
            if (!acceptP(",")) break;
        }
        expectP(";");
    }

    // ---- const
    void globalConst() {
        next();
        GdType ty = parseTypeName(false);
        if (gdIsSampler(ty.base)) fail(peek(), "a constant cannot be a sampler", "gdshader.type");
        const int tsz = parseArraySuffix();
        for (;;) {
            const Token nt = peek();
            const std::string name = expectIdent("a constant name");
            checkGlobalName(nt, name);
            int sz = parseArraySuffix();
            if (sz == 0) sz = tsz;
            GdConstDecl c;
            c.name = name;
            c.line = nt.line;
            expectP("=");
            const Token it = peek();
            c.init = parseAssign();
            GdType dt = ty;
            if (sz > 0) dt.arraySize = sz;
            else if (sz < 0) {
                if (!c.init->type.isArray()) typeFail(it, "cannot infer the array size from this initialiser");
                dt.arraySize = c.init->type.arraySize;
            }
            if (c.init->type != dt) typeFail(it, "cannot initialise '" + tn(dt) + "' with '" + tn(c.init->type) + "'");
            c.type = dt;
            detail::foldConst(*c.init, m_, c.value);
            m_.consts.push_back(std::move(c));
            if (!acceptP(",")) break;
        }
        expectP(";");
    }

    // ---- uniform
    void uniformDecl() {
        GdUniformScope scope = GdUniformScope::Normal;
        const Token at = peek();
        if (acceptId("global")) scope = GdUniformScope::Global;
        else if (acceptId("instance")) scope = GdUniformScope::Instance;
        if (!acceptId("uniform")) fail(peek(), "expected 'uniform'");
        GdType ty = parseTypeName(false);
        if (ty.base == B::Struct) fail(at, "a uniform cannot be a struct", "gdshader.type");
        const int tsz = parseArraySuffix();
        const Token nt = peek();
        const std::string name = expectIdent("a uniform name");
        checkGlobalName(nt, name);
        int sz = parseArraySuffix();
        if (sz == 0) sz = tsz;
        if (sz < 0) fail(nt, "a uniform array needs an explicit size");
        ty.arraySize = sz > 0 ? sz : 0;
        GdUniform u;
        u.name = name;
        u.type = ty;
        u.scope = scope;
        u.group = group_;
        u.subgroup = subgroup_;
        u.line = nt.line;
        u.col = nt.col;
        if (scope == GdUniformScope::Instance) {
            if (gdIsSampler(ty.base) || ty.isArray()) fail(at, "an instance uniform must be a scalar or vector (no samplers or arrays)", "gdshader.type");
            if (++instanceCount_ > 16) fail(at, "at most 16 instance uniforms are allowed", "gdshader.type");
        }
        if (acceptP(":")) {
            for (;;) {
                uniformHint(u);
                if (!acceptP(",")) break;
            }
        }
        if (acceptP("=")) {
            const Token dt = peek();
            if (gdIsSampler(ty.base)) fail(dt, "a texture uniform cannot have an initialiser (use hint_default_*)", "gdshader.type");
            if (ty.isArray()) fail(dt, "an array uniform cannot have an initialiser", "gdshader.type");
            if (scope == GdUniformScope::Global) warn(dt, "the default of a global uniform is ignored (the project's shader globals decide)");
            GdExprP e = parseAssign();
            if (e->type != ty) typeFail(dt, "cannot initialise '" + tn(ty) + "' with '" + tn(e->type) + "'");
            u.hasDefault = true;
            detail::foldConst(*e, m_, u.def);
            if (!u.def.valid) warn(dt, "the uniform default is not a constant expression; it is ignored", "gdshader.default_not_const");
        }
        expectP(";");
        if (scope == GdUniformScope::Global && !opt_.globalUniformNames.empty() &&
            std::find(opt_.globalUniformNames.begin(), opt_.globalUniformNames.end(), name) == opt_.globalUniformNames.end())
            typeFail(nt, "global uniform '" + name + "' is not defined in the project's shader globals");
        m_.uniforms.push_back(std::move(u));
    }

    void uniformHint(GdUniform& u) {
        const Token ht = peek();
        const std::string h = expectIdent("a hint");
        u.hints.push_back(h);
        const GdKind k = gdKind(u.type.base);
        const bool tex2d = u.type.base == B::Sampler2D || u.type.base == B::ISampler2D || u.type.base == B::USampler2D;
        auto needTex = [&] { if (!tex2d && !gdIsSampler(u.type.base)) typeFail(ht, "hint '" + h + "' applies to texture uniforms"); };
        auto needSampler2D = [&] { if (u.type.base != B::Sampler2D) typeFail(ht, "hint '" + h + "' applies to sampler2D uniforms"); };

        std::vector<GdConstValue> numArgs;
        std::vector<std::string> strArgs;
        if (acceptP("(")) {
            if (!isP(")")) {
                for (;;) {
                    if (peek().k == TK::String) strArgs.push_back(next().text);
                    else {
                        const Token at = peek();
                        GdExprP e = parseTernary();
                        GdConstValue v;
                        if (!detail::foldConst(*e, m_, v)) typeFail(at, "hint arguments must be constants");
                        numArgs.push_back(v);
                    }
                    if (!acceptP(",")) break;
                }
            }
            expectP(")");
        }
        auto noArgs = [&] { if (!numArgs.empty() || !strArgs.empty()) typeFail(ht, "hint '" + h + "' takes no arguments"); };

        if (h == "source_color") {
            noArgs();
            const bool ok = u.type.base == B::Vec3 || u.type.base == B::Vec4 || u.type.base == B::Sampler2D || u.type.base == B::Sampler2DArray ||
                            u.type.base == B::Sampler3D || u.type.base == B::SamplerCube;
            if (!ok) typeFail(ht, "source_color applies to vec3, vec4 and float textures");
            u.sourceColor = true;
        } else if (h == "hint_range") {
            if (k != GdKind::Int && k != GdKind::Float && k != GdKind::UInt) typeFail(ht, "hint_range applies to int and float uniforms");
            if (numArgs.size() < 2 || numArgs.size() > 3) typeFail(ht, "hint_range takes (min, max[, step])");
            u.hasRange = true;
            u.rangeMin = numArgs[0].v[0];
            u.rangeMax = numArgs[1].v[0];
            u.rangeStep = numArgs.size() > 2 ? numArgs[2].v[0] : 0;
        } else if (h == "hint_enum") {
            if (u.type.base != B::Int) typeFail(ht, "hint_enum applies to int uniforms");
            if (strArgs.empty()) typeFail(ht, "hint_enum needs at least one string");
            u.enumLabels = strArgs;
        } else if (h == "instance_index") {
            if (u.scope != GdUniformScope::Instance) typeFail(ht, "instance_index applies to instance uniforms");
            if (numArgs.size() != 1 || numArgs[0].v[0] < 0 || numArgs[0].v[0] > 15) typeFail(ht, "instance_index takes one value from 0 to 15");
            u.instanceIndex = static_cast<int>(numArgs[0].v[0]);
        } else if (h == "hint_normal") { noArgs(); needSampler2D(); u.texHint = GdTexHint::Normal; }
        else if (h == "hint_default_white") { noArgs(); needSampler2D(); u.texHint = GdTexHint::DefaultWhite; }
        else if (h == "hint_default_black") { noArgs(); needSampler2D(); u.texHint = GdTexHint::DefaultBlack; }
        else if (h == "hint_default_transparent") { noArgs(); needSampler2D(); u.texHint = GdTexHint::DefaultTransparent; }
        else if (h == "hint_anisotropy") { noArgs(); needSampler2D(); u.texHint = GdTexHint::Anisotropy; }
        else if (h == "hint_roughness_r") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessR; }
        else if (h == "hint_roughness_g") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessG; }
        else if (h == "hint_roughness_b") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessB; }
        else if (h == "hint_roughness_a") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessA; }
        else if (h == "hint_roughness_normal") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessNormal; }
        else if (h == "hint_roughness_gray") { noArgs(); needSampler2D(); u.texHint = GdTexHint::RoughnessGray; }
        else if (h == "hint_screen_texture") { noArgs(); needSampler2D(); u.texHint = GdTexHint::ScreenTexture; }
        else if (h == "hint_depth_texture") { noArgs(); needSampler2D(); u.texHint = GdTexHint::DepthTexture; }
        else if (h == "hint_normal_roughness_texture") { noArgs(); needSampler2D(); u.texHint = GdTexHint::NormalRoughnessTexture; }
        else if (h == "filter_nearest") { noArgs(); needTex(); u.filter = GdFilter::Nearest; }
        else if (h == "filter_linear") { noArgs(); needTex(); u.filter = GdFilter::Linear; }
        else if (h == "filter_nearest_mipmap") { noArgs(); needTex(); u.filter = GdFilter::NearestMipmap; }
        else if (h == "filter_linear_mipmap") { noArgs(); needTex(); u.filter = GdFilter::LinearMipmap; }
        else if (h == "filter_nearest_mipmap_anisotropic") { noArgs(); needTex(); u.filter = GdFilter::NearestMipmapAniso; }
        else if (h == "filter_linear_mipmap_anisotropic") { noArgs(); needTex(); u.filter = GdFilter::LinearMipmapAniso; }
        else if (h == "repeat_enable") { noArgs(); needTex(); u.repeat = GdRepeat::Enable; }
        else if (h == "repeat_disable") { noArgs(); needTex(); u.repeat = GdRepeat::Disable; }
        else fail(ht, "unknown uniform hint '" + h + "'");
    }

    // ================================================================= functions
    bool paramTypesEqual(const GdFunction& f, const std::vector<GdParam>& ps) const {
        if (f.params.size() != ps.size()) return false;
        for (usize i = 0; i < ps.size(); ++i) if (f.params[i].type != ps[i].type) return false;
        return true;
    }

    static bool processorsOf(GdShaderType t, const std::string& n, GdProcessor& out) {
        struct R { GdShaderType t; const char* n; GdProcessor p; };
        static const R rows[] = {
            {GdShaderType::Spatial, "vertex", GdProcessor::Vertex}, {GdShaderType::Spatial, "fragment", GdProcessor::Fragment}, {GdShaderType::Spatial, "light", GdProcessor::Light},
            {GdShaderType::CanvasItem, "vertex", GdProcessor::Vertex}, {GdShaderType::CanvasItem, "fragment", GdProcessor::Fragment}, {GdShaderType::CanvasItem, "light", GdProcessor::Light},
            {GdShaderType::Particles, "start", GdProcessor::Start}, {GdShaderType::Particles, "process", GdProcessor::Process},
            {GdShaderType::Sky, "sky", GdProcessor::Sky}, {GdShaderType::Fog, "fog", GdProcessor::Fog},
        };
        for (const R& r : rows) if (r.t == t && n == r.n) { out = r.p; return true; }
        return false;
    }

    void functionDef() {
        const Token at = peek();
        GdType ret = parseTypeName(true);
        const int rsz = parseArraySuffix();
        if (rsz < 0) fail(at, "a function return array needs an explicit size");
        if (rsz > 0) ret.arraySize = rsz;
        if (gdIsSampler(ret.base)) fail(at, "a function cannot return a sampler", "gdshader.type");
        const Token nt = peek();
        const std::string name = expectIdent("a function name");
        if (!isP("(")) fail(peek(), "expected '(' after the function name '" + name + "'");
        next();

        GdFunction fn;
        fn.name = name;
        fn.ret = ret;
        fn.line = nt.line;
        fn.col = nt.col;
        GdProcessor pr = GdProcessor::None;
        const bool isProc = processorsOf(m_.shaderType, name, pr);
        if (!isP(")")) {
            for (;;) {
                GdParam p;
                bool qualSeen = false;
                for (;;) {
                    if (acceptId("const")) p.isConst = true;
                    else if (!qualSeen && acceptId("in")) { p.qual = GdParamQual::In; qualSeen = true; }
                    else if (!qualSeen && acceptId("out")) { p.qual = GdParamQual::Out; qualSeen = true; }
                    else if (!qualSeen && acceptId("inout")) { p.qual = GdParamQual::InOut; qualSeen = true; }
                    else break;
                }
                p.type = parseTypeName(false);
                const int tsz = parseArraySuffix();
                const Token pt = peek();
                p.name = expectIdent("a parameter name");
                int sz = parseArraySuffix();
                if (sz == 0) sz = tsz;
                if (sz < 0) fail(pt, "an array parameter needs an explicit size");
                p.type.arraySize = sz > 0 ? sz : 0;
                if (p.isConst && p.qual != GdParamQual::In) fail(pt, "a const parameter cannot be out or inout", "gdshader.type");
                if (gdIsSampler(p.type.base) && p.qual != GdParamQual::In) fail(pt, "a sampler parameter must be 'in'", "gdshader.type");
                for (const GdParam& o : fn.params) if (o.name == p.name) fail(pt, "duplicate parameter '" + p.name + "'", "gdshader.type");
                fn.params.push_back(std::move(p));
                if (!acceptP(",")) break;
            }
        }
        expectP(")");

        if (isProc) {
            if (ret.base != B::Void || ret.isArray()) fail(at, "'" + name + "()' must return void", "gdshader.type");
            if (!fn.params.empty()) fail(nt, "'" + name + "()' takes no parameters", "gdshader.type");
            for (const GdFunction& f : m_.functions) if (f.processor == pr) fail(nt, "'" + name + "()' is defined twice", "gdshader.type");
            fn.processor = pr;
        } else {
            if (isTypeName(name) || isKeyword(name)) fail(nt, "'" + name + "' cannot be used as a function name", "gdshader.type");
            if (gdIsBuiltinFunction(name)) fail(nt, "'" + name + "' redefines a built-in function", "gdshader.type");
            if (m_.findUniform(name) >= 0 || m_.findVarying(name) >= 0) fail(nt, "'" + name + "' is already a variable", "gdshader.type");
            for (const GdConstDecl& c : m_.consts) if (c.name == name) fail(nt, "'" + name + "' is already a constant", "gdshader.type");
            for (const GdFunction& f : m_.functions)
                if (f.name == name && paramTypesEqual(f, fn.params)) fail(nt, "function '" + name + "' is already defined with these parameters", "gdshader.type");
        }

        if (!isP("{")) fail(peek(), "expected a function body '{'");
        fn_ = &fn;
        proc_ = pr;
        pushScope();
        for (const GdParam& p : fn.params) {
            LocalVar v;
            v.type = p.type;
            v.isConst = p.isConst;
            v.isParam = true;
            v.qual = p.qual;
            declareLocal(nt, p.name, v);
        }
        loopDepth_ = switchDepth_ = 0;
        fn.body = parseBlock(false);
        popScope();
        if (ret.base != B::Void && !alwaysReturns(*fn.body)) fail(nt, "function '" + name + "' must return a value on every path", "gdshader.type");
        fn_ = nullptr;
        proc_ = GdProcessor::None;
        m_.functions.push_back(std::move(fn));
    }

    static bool alwaysReturns(const GdStmt& s) {
        switch (s.kind) {
        case GdStmtKind::Return: return true;
        case GdStmtKind::Block: return !s.body.empty() && std::any_of(s.body.begin(), s.body.end(), [](const GdStmtP& c) { return alwaysReturns(*c); });
        case GdStmtKind::If: return s.elseS && alwaysReturns(*s.thenS) && alwaysReturns(*s.elseS);
        case GdStmtKind::Switch: return true;   // lenient: the cases are checked by the author's tests, not by control-flow analysis
        case GdStmtKind::While: case GdStmtKind::For: case GdStmtKind::DoWhile: return false;
        default: return false;
        }
    }

    // ================================================================= statements
    GdStmtP mkStmt(GdStmtKind k, const Token& at) {
        auto s = std::make_unique<GdStmt>();
        s->kind = k;
        s->line = at.line;
        s->col = at.col;
        return s;
    }

    GdStmtP parseBlock(bool pushNew) {
        const Token at = peek();
        expectP("{");
        if (pushNew) pushScope();
        auto b = mkStmt(GdStmtKind::Block, at);
        while (!isP("}")) {
            if (peek().k == TK::End) fail(peek(), "unterminated block (missing '}')");
            b->body.push_back(parseStatement());
        }
        next();
        if (pushNew) popScope();
        return b;
    }

    bool startsDeclaration() const {
        const Token& t = peek();
        if (t.k != TK::Ident) return false;
        if (t.text == "const" || isPrecision(t.text)) return true;
        if (!isTypeName(t.text)) return false;
        if (peek(1).k == TK::Ident) return true;
        if (isP("[", 1)) {
            usize o = 2;
            int depth = 1;
            while (peek(o).k != TK::End && depth > 0) {
                if (isP("[", o)) ++depth;
                else if (isP("]", o)) --depth;
                ++o;
            }
            return peek(o).k == TK::Ident;
        }
        return false;
    }

    GdExprP condition(const char* what) {
        expectP("(");
        const Token at = peek();
        GdExprP c = parseComma();
        if (c->type.base != B::Bool || c->type.isArray()) typeFail(at, std::string(what) + " condition must be bool, not '" + tn(c->type) + "'");
        expectP(")");
        return c;
    }

    GdStmtP parseStatement() {
        DepthGuard guard(*this);
        const Token& t = peek();
        if (t.k == TK::Punct) {
            if (t.text == "{") return parseBlock(true);
            if (t.text == ";") { auto s = mkStmt(GdStmtKind::Block, t); next(); return s; }
        }
        if (t.k == TK::Ident) {
            const std::string& w = t.text;
            if (w == "if") {
                auto s = mkStmt(GdStmtKind::If, next());
                s->expr = condition("if");
                s->thenS = parseScopedBody();
                if (acceptId("else")) s->elseS = parseScopedBody();
                return s;
            }
            if (w == "while") {
                auto s = mkStmt(GdStmtKind::While, next());
                s->expr = condition("while");
                ++loopDepth_;
                s->thenS = parseScopedBody();
                --loopDepth_;
                return s;
            }
            if (w == "do") {
                auto s = mkStmt(GdStmtKind::DoWhile, next());
                ++loopDepth_;
                s->thenS = parseScopedBody();
                --loopDepth_;
                if (!acceptId("while")) fail(peek(), "expected 'while' after the do body");
                s->expr = condition("do-while");
                expectP(";");
                return s;
            }
            if (w == "for") return parseFor();
            if (w == "switch") return parseSwitch();
            if (w == "break") {
                const Token at = next();
                if (loopDepth_ == 0 && switchDepth_ == 0) fail(at, "'break' outside a loop or switch", "gdshader.type");
                expectP(";");
                return mkStmt(GdStmtKind::Break, at);
            }
            if (w == "continue") {
                const Token at = next();
                if (loopDepth_ == 0) fail(at, "'continue' outside a loop", "gdshader.type");
                expectP(";");
                return mkStmt(GdStmtKind::Continue, at);
            }
            if (w == "return") return parseReturn();
            if (w == "discard") {
                const Token at = next();
                if (proc_ == GdProcessor::Vertex || proc_ == GdProcessor::Start || proc_ == GdProcessor::Process || proc_ == GdProcessor::Sky ||
                    proc_ == GdProcessor::Fog)
                    fail(at, std::string("'discard' is not allowed in ") + gdProcessorName(proc_) + "()", "gdshader.type");
                if (fn_) fn_->usesDiscard = true;
                expectP(";");
                return mkStmt(GdStmtKind::Discard, at);
            }
            if (w == "else") fail(t, "'else' without a matching 'if'");
            if (w == "case" || w == "default") fail(t, "'" + w + "' outside a switch", "gdshader.type");
        }
        if (startsDeclaration()) return parseLocalDecl(true);
        auto s = mkStmt(GdStmtKind::Expr, t);
        s->expr = parseComma();
        expectP(";");
        return s;
    }

    // A statement used as the body of if / loop: its own scope.
    GdStmtP parseScopedBody() {
        if (isP("{")) return parseBlock(true);
        pushScope();
        GdStmtP s = parseStatement();
        popScope();
        return s;
    }

    GdStmtP parseFor() {
        auto s = mkStmt(GdStmtKind::For, next());
        expectP("(");
        pushScope();
        if (!isP(";")) {
            if (startsDeclaration()) s->init = parseLocalDecl(true);
            else {
                auto e = mkStmt(GdStmtKind::Expr, peek());
                e->expr = parseComma();
                expectP(";");
                s->init = std::move(e);
            }
        } else next();
        if (!isP(";")) {
            const Token at = peek();
            s->expr = parseComma();
            if (s->expr->type.base != B::Bool || s->expr->type.isArray()) typeFail(at, "for condition must be bool");
        }
        expectP(";");
        if (!isP(")")) s->step = parseComma();
        expectP(")");
        ++loopDepth_;
        s->thenS = parseScopedBody();
        --loopDepth_;
        popScope();
        return s;
    }

    GdStmtP parseSwitch() {
        auto s = mkStmt(GdStmtKind::Switch, next());
        expectP("(");
        const Token vt = peek();
        s->expr = parseComma();
        if (s->expr->type.base != B::Int || s->expr->type.isArray()) typeFail(vt, "the switch value must be a signed int, not '" + tn(s->expr->type) + "'");
        expectP(")");
        expectP("{");
        pushScope();
        ++switchDepth_;
        std::set<long long> seen;
        bool hasDefault = false;
        while (!isP("}")) {
            if (peek().k == TK::End) fail(peek(), "unterminated switch");
            if (isId("case")) {
                auto c = mkStmt(GdStmtKind::Case, next());
                const Token et = peek();
                c->expr = parseTernary();
                GdConstValue v;
                if (c->expr->type.base != B::Int || !detail::foldConst(*c->expr, m_, v)) typeFail(et, "a case label must be a constant int");
                if (!seen.insert(static_cast<long long>(v.v[0])).second) typeFail(et, "duplicate case label " + std::to_string(static_cast<long long>(v.v[0])));
                expectP(":");
                s->body.push_back(std::move(c));
            } else if (isId("default")) {
                const Token dt = next();
                if (hasDefault) fail(dt, "duplicate 'default' label", "gdshader.type");
                hasDefault = true;
                expectP(":");
                s->body.push_back(mkStmt(GdStmtKind::Default, dt));
            } else s->body.push_back(parseStatement());
        }
        next();
        --switchDepth_;
        popScope();
        return s;
    }

    GdStmtP parseReturn() {
        const Token at = next();
        auto s = mkStmt(GdStmtKind::Return, at);
        if (!fn_) fail(at, "'return' outside a function");
        if (isP(";")) {
            if (fn_->ret.base != B::Void) typeFail(at, "function '" + fn_->name + "' must return '" + tn(fn_->ret) + "'");
        } else {
            const Token et = peek();
            s->expr = parseComma();
            if (fn_->ret.base == B::Void) typeFail(et, "function '" + fn_->name + "' returns void");
            if (s->expr->type != fn_->ret) typeFail(et, "cannot return '" + tn(s->expr->type) + "' from a function returning '" + tn(fn_->ret) + "'");
        }
        expectP(";");
        return s;
    }

    GdStmtP parseLocalDecl(bool wantSemi) {
        const Token at = peek();
        auto s = mkStmt(GdStmtKind::Decl, at);
        if (acceptId("const")) s->isConst = true;
        GdType ty = parseTypeName(false);
        if (gdIsSampler(ty.base)) fail(at, "a local variable cannot be a sampler", "gdshader.type");
        const int tsz = parseArraySuffix();
        for (;;) {
            const Token nt = peek();
            GdDeclarator d;
            d.name = expectIdent("a variable name");
            d.line = nt.line;
            d.col = nt.col;
            int sz = parseArraySuffix();
            if (sz == 0) sz = tsz;
            GdType dt = ty;
            if (sz > 0) dt.arraySize = sz;
            std::shared_ptr<const GdConstValue> cv;
            if (acceptP("=")) {
                const Token it = peek();
                d.init = parseAssign();
                if (sz < 0) {
                    if (!d.init->type.isArray()) typeFail(it, "cannot infer the array size from this initialiser");
                    dt.arraySize = d.init->type.arraySize;
                }
                if (d.init->type != dt) typeFail(it, "cannot initialise '" + tn(dt) + "' with '" + tn(d.init->type) + "'");
                if (s->isConst) {
                    GdConstValue v;
                    if (detail::foldConst(*d.init, m_, v)) cv = std::make_shared<GdConstValue>(std::move(v));
                }
            } else {
                if (s->isConst) typeFail(nt, "a constant needs an initialiser");
                if (sz < 0) typeFail(nt, "an unsized array needs an initialiser");
            }
            d.type = dt;
            LocalVar lv;
            lv.type = dt;
            lv.isConst = s->isConst;
            lv.cv = cv;
            declareLocal(nt, d.name, lv);
            s->decls.push_back(std::move(d));
            if (!acceptP(",")) break;
        }
        if (wantSemi) expectP(";");
        return s;
    }

    // ================================================================= expressions
    GdExprP parseComma() {
        GdExprP e = parseAssign();
        for (int chain = 0; isP(","); ++chain) {
            if (chain > 300) fail(peek(), "expression has too many comma operators (limit 300)");
            const Token op = next();
            GdExprP r = parseAssign();
            auto c = mk(GdExprKind::Comma, op);
            c->type = r->type;
            c->args.push_back(std::move(e));
            c->args.push_back(std::move(r));
            e = std::move(c);
        }
        return e;
    }

    static bool isAssignOp(const std::string& s) {
        return s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=" || s == "%=" || s == "<<=" || s == ">>=" || s == "&=" || s == "|=" || s == "^=";
    }

    // Nesting guard: a hostile or broken shader must produce an error, not a stack overflow.
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& pp) : p(pp) {
            if (++p.depth_ > 96) { --p.depth_; p.fail(p.peek(), "expression or statement nested too deeply"); }
        }
        ~DepthGuard() { --p.depth_; }
    };
    int depth_ = 0;

    GdExprP parseAssign() {
        DepthGuard guard(*this);
        GdExprP lhs = parseTernary();
        if (peek().k == TK::Punct && isAssignOp(peek().text)) {
            const Token op = next();
            requireAssignable(*lhs, op);
            GdExprP rhs = parseAssign();
            auto e = mk(GdExprKind::Assign, op);
            e->op = op.text;
            if (op.text == "=") {
                if (rhs->type != lhs->type) typeFail(op, "cannot assign '" + tn(rhs->type) + "' to '" + tn(lhs->type) + "'");
            } else {
                const std::string bop = op.text.substr(0, op.text.size() - 1);
                const GdType r = detail::binaryResultType(bop, lhs->type, rhs->type);
                if (r.base == B::Error) typeFail(op, "invalid operands to '" + op.text + "': '" + tn(lhs->type) + "' and '" + tn(rhs->type) + "'");
                if (r != lhs->type) typeFail(op, "'" + op.text + "' would produce '" + tn(r) + "', which cannot be stored in '" + tn(lhs->type) + "'");
            }
            e->type = lhs->type;
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            return e;
        }
        return lhs;
    }

    GdExprP parseTernary() {
        GdExprP c = parseBinary(0);
        if (isP("?")) {
            const Token op = next();
            if (c->type.base != B::Bool || c->type.isArray()) typeFail(op, "the condition of '?:' must be bool");
            GdExprP a = parseAssign();
            expectP(":");
            GdExprP b = parseAssign();
            if (a->type != b->type) typeFail(op, "the branches of '?:' have different types ('" + tn(a->type) + "' and '" + tn(b->type) + "')");
            auto e = mk(GdExprKind::Ternary, op);
            e->type = a->type;
            e->args.push_back(std::move(c));
            e->args.push_back(std::move(a));
            e->args.push_back(std::move(b));
            return e;
        }
        return c;
    }

    // Precedence of a binary operator token (higher binds tighter); 0 = not a binary operator.
    int binaryPrec() const {
        const Token& t = peek();
        if (t.k != TK::Punct) return 0;
        const std::string& s = t.text;
        if (s == "||") return 1;
        if (s == "&&") return 2;
        if (s == "|") return 3;
        if (s == "^") return 4;
        if (s == "&") return 5;
        if (s == "==" || s == "!=") return 6;
        if (s == "<" || s == ">" || s == "<=" || s == ">=") return 7;
        if (s == "<<" || s == ">>") return 8;
        if (s == "+" || s == "-") return 9;
        if (s == "*" || s == "/" || s == "%") return 10;
        return 0;
    }

    [[noreturn]] void badOperands(const Token& op, const GdType& a, const GdType& b) {
        typeFail(op, "invalid operands to '" + op.text + "': '" + tn(a) + "' and '" + tn(b) + "'");
    }

    // Precedence climbing (one frame per nesting level, which keeps deeply nested input within the stack of a debug build).
    GdExprP parseBinary(int minPrec) {
        GdExprP l = parseUnary();
        for (int chain = 0;; ++chain) {
            const int prec = binaryPrec();
            if (prec == 0 || prec < minPrec) return l;
            if (chain > 300) fail(peek(), "expression has too many chained operators (limit 300)");
            const Token op = next();
            GdExprP r = parseBinary(prec + 1);
            const GdType rt = detail::binaryResultType(op.text, l->type, r->type);
            if (rt.base == B::Error) badOperands(op, l->type, r->type);
            auto e = mk(GdExprKind::Binary, op);
            e->op = op.text;
            e->type = rt;
            e->args.push_back(std::move(l));
            e->args.push_back(std::move(r));
            l = std::move(e);
        }
    }

    GdExprP parseUnary() {
        DepthGuard guard(*this);
        const Token& t = peek();
        if (t.k == TK::Punct && (t.text == "+" || t.text == "-" || t.text == "!" || t.text == "~")) {
            const Token op = next();
            GdExprP a = parseUnary();
            const GdType rt = detail::unaryResultType(op.text, a->type);
            if (rt.base == B::Error) typeFail(op, "invalid operand to unary '" + op.text + "': '" + tn(a->type) + "'");
            auto e = mk(GdExprKind::Unary, op);
            e->op = op.text;
            e->type = rt;
            e->args.push_back(std::move(a));
            return e;
        }
        if (t.k == TK::Punct && (t.text == "++" || t.text == "--")) {
            const Token op = next();
            GdExprP a = parseUnary();
            requireAssignable(*a, op);
            const GdType rt = detail::unaryResultType(op.text, a->type);
            if (rt.base == B::Error) typeFail(op, "invalid operand to '" + op.text + "': '" + tn(a->type) + "'");
            auto e = mk(GdExprKind::IncDec, op);
            e->op = op.text;
            e->postfix = false;
            e->type = rt;
            e->args.push_back(std::move(a));
            return e;
        }
        return parsePostfix();
    }

    GdExprP parsePostfix() {
        GdExprP e = parsePrimary();
        for (int chain = 0;; ++chain) {
            if (chain > 300 && (isP(".") || isP("[") || isP("++") || isP("--"))) fail(peek(), "expression has too many chained accesses (limit 300)");
            if (isP(".")) {
                const Token dot = next();
                const Token nameTok = peek();
                if (nameTok.k != TK::Ident) fail(nameTok, "expected a member name after '.'");
                next();
                const std::string& n = nameTok.text;
                if (n == "length" && isP("(")) {
                    next();
                    expectP(")");
                    if (!e->type.isArray()) typeFail(nameTok, "length() applies to arrays");
                    auto l = mk(GdExprKind::ArrayLength, nameTok);
                    l->type.base = B::Int;
                    l->num = e->type.arraySize;
                    l->args.push_back(std::move(e));
                    e = std::move(l);
                    continue;
                }
                if (e->type.isArray()) typeFail(nameTok, "an array has no member '" + n + "'");
                if (e->type.base == B::Struct) {
                    const GdStruct& st = m_.structs[static_cast<usize>(e->type.structIdx)];
                    int idx = -1;
                    for (usize i = 0; i < st.members.size(); ++i) if (st.members[i].name == n) idx = static_cast<int>(i);
                    if (idx < 0) typeFail(nameTok, "struct '" + st.name + "' has no member '" + n + "'");
                    auto m = mk(GdExprKind::Member, nameTok);
                    m->name = n;
                    m->memberIdx = idx;
                    m->type = st.members[static_cast<usize>(idx)].type;
                    m->args.push_back(std::move(e));
                    e = std::move(m);
                    continue;
                }
                const GdKind k = gdKind(e->type.base);
                const int dim = gdDim(e->type.base);
                if ((k == GdKind::Bool || k == GdKind::Int || k == GdKind::UInt || k == GdKind::Float) && dim >= 2) {
                    const std::string sw = detail::normaliseSwizzle(n, dim);
                    if (sw.empty()) typeFail(nameTok, "invalid swizzle '." + n + "' on '" + tn(e->type) + "'");
                    auto s = mk(GdExprKind::Swizzle, nameTok);
                    s->swizzle = sw;
                    s->name = n;
                    s->type.base = gdMakeVec(gdScalarOf(e->type.base), static_cast<int>(sw.size()));
                    s->args.push_back(std::move(e));
                    e = std::move(s);
                    continue;
                }
                (void)dot;
                typeFail(nameTok, "'" + tn(e->type) + "' has no member '" + n + "'");
            } else if (isP("[")) {
                const Token br = next();
                GdExprP idx = parseComma();
                expectP("]");
                if (idx->type.isArray() || (idx->type.base != B::Int && idx->type.base != B::UInt)) typeFail(br, "an index must be an int or uint");
                GdType et;
                if (e->type.isArray()) et = e->type.element();
                else if (gdKind(e->type.base) == GdKind::Matrix) et.base = gdMakeVec(B::Float, gdDim(e->type.base));
                else if (gdDim(e->type.base) >= 2 && gdKind(e->type.base) != GdKind::Sampler && gdKind(e->type.base) != GdKind::Struct) et.base = gdScalarOf(e->type.base);
                else typeFail(br, "'" + tn(e->type) + "' cannot be indexed");
                int bound = e->type.isArray() ? e->type.arraySize : gdDim(e->type.base);
                GdConstValue iv;
                if (detail::foldConst(*idx, m_, iv) && (iv.v[0] < 0 || iv.v[0] >= bound))
                    typeFail(br, "index " + std::to_string(static_cast<long long>(iv.v[0])) + " is out of range (size " + std::to_string(bound) + ")");
                auto x = mk(GdExprKind::Index, br);
                x->type = et;
                x->args.push_back(std::move(e));
                x->args.push_back(std::move(idx));
                e = std::move(x);
            } else if (isP("++") || isP("--")) {
                const Token op = next();
                requireAssignable(*e, op);
                const GdType rt = detail::unaryResultType(op.text, e->type);
                if (rt.base == B::Error) typeFail(op, "invalid operand to '" + op.text + "': '" + tn(e->type) + "'");
                auto x = mk(GdExprKind::IncDec, op);
                x->op = op.text;
                x->postfix = true;
                x->type = rt;
                x->args.push_back(std::move(e));
                e = std::move(x);
            } else return e;
        }
    }

    GdExprP parsePrimary() {
        const Token t = peek();
        switch (t.k) {
        case TK::Int: { next(); auto e = mk(GdExprKind::IntLit, t); e->type.base = B::Int; e->num = t.num; return e; }
        case TK::UInt: { next(); auto e = mk(GdExprKind::UIntLit, t); e->type.base = B::UInt; e->num = t.num; return e; }
        case TK::Float: { next(); auto e = mk(GdExprKind::FloatLit, t); e->type.base = B::Float; e->num = t.num; return e; }
        case TK::Punct:
            if (t.text == "(") {
                next();
                GdExprP e = parseComma();
                expectP(")");
                return e;
            }
            fail(t, "unexpected " + describe(t) + " in an expression");
        case TK::Ident: break;
        default: fail(t, "unexpected " + describe(t) + " in an expression");
        }

        if (t.text == "true" || t.text == "false") {
            next();
            auto e = mk(GdExprKind::BoolLit, t);
            e->type.base = B::Bool;
            e->num = t.text == "true" ? 1 : 0;
            return e;
        }
        if (isPrecision(t.text)) { next(); return parsePrimary(); }
        if (isTypeName(t.text) || t.text == "void") {
            if (t.text == "void") fail(t, "'void' is not an expression");
            return parseConstructor();
        }
        if (isKeyword(t.text)) fail(t, "unexpected keyword '" + t.text + "' in an expression");
        if (isP("(", 1)) return parseCall();
        next();
        return resolveVariable(t);
    }

    // ---- variable references
    const GdBuiltinVar* findBuiltin(const std::string& name, int& idx, bool& existsElsewhere) const {
        existsElsewhere = false;
        const u16 bit = proc_ == GdProcessor::None ? u16{0} : gdStageBit(proc_);
        for (int i = 0; i < bvCount_; ++i) {
            if (name != bv_[i].name) continue;
            existsElsewhere = true;
            if (bv_[i].global || (bit && (bv_[i].stages & bit))) { idx = i; return &bv_[i]; }
        }
        return nullptr;
    }

    GdExprP resolveVariable(const Token& t) {
        const std::string& n = t.text;
        auto e = mk(GdExprKind::Var, t);
        e->name = n;
        if (LocalVar* lv = findLocal(n)) {
            e->ref = lv->isParam ? GdRefKind::Param : GdRefKind::Local;
            e->type = lv->type;
            if (lv->isConst && lv->cv) e->cv = lv->cv;
            return e;
        }
        for (usize i = 0; i < m_.consts.size(); ++i) {
            if (m_.consts[i].name == n) {
                e->ref = GdRefKind::GlobalConst;
                e->refIdx = static_cast<int>(i);
                e->type = m_.consts[i].type;
                if (m_.consts[i].value.valid) e->cv = std::make_shared<GdConstValue>(m_.consts[i].value);
                return e;
            }
        }
        if (const int ui = m_.findUniform(n); ui >= 0) {
            e->ref = GdRefKind::Uniform;
            e->refIdx = ui;
            e->type = m_.uniforms[static_cast<usize>(ui)].type;
            return e;
        }
        if (const int vi = m_.findVarying(n); vi >= 0) {
            e->ref = GdRefKind::Varying;
            e->refIdx = vi;
            e->type = m_.varyings[static_cast<usize>(vi)].type;
            GdVarying& v = m_.varyings[static_cast<usize>(vi)];
            if ((proc_ == GdProcessor::Fragment || proc_ == GdProcessor::Light) && !v.assignedInVertex && warnedVarying_.insert(n).second)
                warn(t, "varying '" + n + "' is read before any assignment in vertex()", "gdshader.varying_unassigned");
            return e;
        }
        int bi = -1;
        bool elsewhere = false;
        if (const GdBuiltinVar* bv = findBuiltin(n, bi, elsewhere)) {
            e->ref = GdRefKind::Builtin;
            e->refIdx = bi;
            e->type.base = bv->type;
            return e;
        }
        if (elsewhere) {
            if (proc_ == GdProcessor::None)
                typeFail(t, "built-in '" + n + "' is not accessible in a custom function (pass it as an argument)");
            typeFail(t, "built-in '" + n + "' is not available in " + gdProcessorName(proc_) + "()");
        }
        typeFail(t, "unknown identifier '" + n + "'");
    }

    // The lvalue rule for assignment, ++ / -- and out / inout arguments.
    void requireAssignable(const GdExpr& e, const Token& at) {
        switch (e.kind) {
        case GdExprKind::Var:
            switch (e.ref) {
            case GdRefKind::Local: case GdRefKind::Param: {
                LocalVar* lv = findLocal(e.name);
                if (lv && lv->isConst) typeFail(at, "cannot modify the constant '" + e.name + "'");
                if (lv && gdIsSampler(lv->type.base)) typeFail(at, "cannot modify a sampler");
                return;
            }
            case GdRefKind::GlobalConst: typeFail(at, "cannot modify the constant '" + e.name + "'");
            case GdRefKind::Uniform: typeFail(at, "cannot modify the uniform '" + e.name + "' (uniforms are read-only)");
            case GdRefKind::Varying: {
                if (proc_ != GdProcessor::Vertex && proc_ != GdProcessor::Fragment)
                    typeFail(at, "varying '" + e.name + "' can be assigned only in vertex() or fragment()");
                if (proc_ == GdProcessor::Vertex) m_.varyings[static_cast<usize>(e.refIdx)].assignedInVertex = true;
                return;
            }
            case GdRefKind::Builtin:
                if (bv_[e.refIdx].access == GdAccess::In || gdIsSampler(bv_[e.refIdx].type))
                    typeFail(at, "built-in '" + e.name + "' is read-only here");
                return;
            default: typeFail(at, "not assignable");
            }
        case GdExprKind::Member: case GdExprKind::Index:
            requireAssignable(*e.args[0], at);
            return;
        case GdExprKind::Swizzle: {
            requireAssignable(*e.args[0], at);
            std::set<char> seen;
            for (char c : e.swizzle) if (!seen.insert(c).second) typeFail(at, "a swizzle with repeated components cannot be assigned");
            return;
        }
        default:
            typeFail(at, "the left side is not assignable");
        }
    }

    // ---- constructors
    std::vector<GdExprP> parseArgs() {
        std::vector<GdExprP> args;
        expectP("(");
        if (!isP(")")) {
            for (;;) {
                args.push_back(parseAssign());
                if (!acceptP(",")) break;
            }
        }
        expectP(")");
        return args;
    }

    GdExprP parseConstructor() {
        const Token t = next();
        GdType ty = typeFromName(t.text);
        const int sz = parseArraySuffix();
        if (!isP("(")) fail(peek(), "expected '(' after the type '" + t.text + "'");
        const Token open = peek();
        std::vector<GdExprP> args = parseArgs();

        if (sz != 0) {   // array constructor
            if (gdIsSampler(ty.base)) typeFail(t, "arrays of samplers cannot be constructed");
            const int n = sz > 0 ? sz : static_cast<int>(args.size());
            if (args.empty() || static_cast<int>(args.size()) != n) typeFail(open, "array constructor needs " + std::to_string(n) + " element(s), got " + std::to_string(args.size()));
            for (const GdExprP& a : args)
                if (a->type != ty) typeFail(open, "array element of type '" + tn(a->type) + "' where '" + tn(ty) + "' is expected");
            auto e = mk(GdExprKind::ArrayConstruct, t);
            e->name = t.text;
            e->type = ty;
            e->type.arraySize = n;
            e->args = std::move(args);
            return e;
        }

        auto e = mk(GdExprKind::Construct, t);
        e->name = t.text;
        e->type = ty;
        checkConstruct(t, ty, args);
        e->args = std::move(args);
        return e;
    }

    void checkConstruct(const Token& at, const GdType& ty, const std::vector<GdExprP>& args) {
        if (args.empty()) typeFail(at, "constructor '" + tn(ty) + "' needs arguments");
        if (ty.base != B::Struct)
            for (const GdExprP& a : args) if (a->type.isArray()) typeFail(at, "an array cannot be a constructor argument");
        if (ty.base == B::Struct) {
            const GdStruct& st = m_.structs[static_cast<usize>(ty.structIdx)];
            if (args.size() != st.members.size()) typeFail(at, "struct '" + st.name + "' has " + std::to_string(st.members.size()) + " members, " + std::to_string(args.size()) + " given");
            for (usize i = 0; i < args.size(); ++i)
                if (args[i]->type != st.members[i].type)
                    typeFail(at, "member '" + st.members[i].name + "' is '" + tn(st.members[i].type) + "', got '" + tn(args[i]->type) + "'");
            return;
        }
        if (gdIsSampler(ty.base)) typeFail(at, "a sampler cannot be constructed");
        const GdKind tk = gdKind(ty.base);
        int comps = 0;
        bool anyMat = false;
        for (const GdExprP& a : args) {
            const GdKind ak = gdKind(a->type.base);
            if (ak == GdKind::Struct || ak == GdKind::Sampler || ak == GdKind::Void) typeFail(at, "'" + tn(a->type) + "' cannot be a constructor argument");
            if (ak == GdKind::Matrix) { anyMat = true; comps += gdDim(a->type.base) * gdDim(a->type.base); }
            else comps += gdDim(a->type.base);
        }
        if (tk == GdKind::Matrix) {
            const int n = gdDim(ty.base);
            if (anyMat) {
                if (args.size() != 1) typeFail(at, "a matrix can be constructed from one matrix");
                return;
            }
            if (args.size() == 1 && gdDim(args[0]->type.base) == 1) return;      // diagonal
            if (comps == n * n) return;
            typeFail(at, "'" + tn(ty) + "' needs one scalar, " + std::to_string(n * n) + " scalars or " + std::to_string(n) + " column vectors");
        }
        // scalar / vector
        if (anyMat) typeFail(at, "a matrix cannot be converted to '" + tn(ty) + "'");
        const int n = gdDim(ty.base);
        if (args.size() == 1) {
            const int ad = gdDim(args[0]->type.base);
            if (ad == 1 || ad >= n) return;   // splat / convert, or truncate a larger vector
            typeFail(at, "cannot construct '" + tn(ty) + "' from '" + tn(args[0]->type) + "'");
        }
        if (comps < n) typeFail(at, "constructor '" + tn(ty) + "' needs " + std::to_string(n) + " component(s), got " + std::to_string(comps));
        int used = 0;
        for (usize i = 0; i < args.size(); ++i) {
            if (used >= n) typeFail(at, "too many arguments for the constructor '" + tn(ty) + "'");
            used += gdDim(args[i]->type.base);
        }
    }

    // ---- calls
    GdExprP parseCall() {
        const Token t = next();
        const Token open = peek();
        std::vector<GdExprP> args = parseArgs();
        auto e = mk(GdExprKind::Call, t);
        e->name = t.text;

        // user functions (exact parameter types; overloads allowed)
        bool anyUser = false;
        for (usize i = 0; i < m_.functions.size(); ++i) {
            const GdFunction& f = m_.functions[i];
            if (f.name != t.text) continue;
            anyUser = true;
            if (f.params.size() != args.size()) continue;
            bool match = true;
            for (usize k = 0; k < args.size() && match; ++k) match = f.params[k].type == args[k]->type;
            if (!match) continue;
            for (usize k = 0; k < args.size(); ++k)
                if (f.params[k].qual != GdParamQual::In) requireAssignable(*args[k], open);
            e->functionIdx = static_cast<int>(i);
            e->type = f.ret;
            e->args = std::move(args);
            return e;
        }
        if (!anyUser) {
            const std::vector<int>& ids = detail::builtinOverloadIds(t.text);
            const auto& all = detail::builtinOverloads();
            for (int id : ids) {
                const detail::BuiltinOverload& o = all[static_cast<usize>(id)];
                if (o.params.size() != args.size()) continue;
                bool match = true;
                for (usize k = 0; k < args.size() && match; ++k) match = !args[k]->type.isArray() && o.params[k] == args[k]->type.base;
                if (!match) continue;
                for (usize k = 0; k < args.size(); ++k) if (o.outParam[k]) requireAssignable(*args[k], open);
                e->builtinOverload = id;
                e->type = o.ret;
                e->args = std::move(args);
                return e;
            }
            if (ids.empty()) typeFail(t, "unknown function '" + t.text + "'");
        }
        std::string sig;
        for (usize k = 0; k < args.size(); ++k) sig += (k ? ", " : "") + tn(args[k]->type);
        typeFail(t, "no overload of '" + t.text + "' takes (" + sig + ")");
    }
};

}  // namespace

bool gdShaderCompile(std::string_view source, std::string_view fileName, const GdCompileOptions& opt, GdShaderModule& out) {
    out = GdShaderModule{};
    GdPreprocessed pp;
    gdShaderPreprocess(source, fileName, opt.pre, pp, out.diags);
    out.files = pp.files;
    out.pragmas = pp.pragmas;
    if (out.errorCount() != 0) { out.ok = false; return false; }

    std::vector<Token> toks;
    LexError le;
    if (!lexShader(pp, toks, le)) {
        GdShaderDiag d;
        d.file = le.file < out.files.size() ? out.files[le.file] : std::string();
        d.line = le.line;
        d.column = le.col;
        d.code = "gdshader.syntax";
        d.message = le.msg;
        out.diags.push_back(std::move(d));
        out.ok = false;
        return false;
    }
    Parser p(out, opt, std::move(toks));
    p.parseAll();
    out.ok = out.errorCount() == 0;
    return out.ok;
}

}  // namespace aver::fmt::godot
