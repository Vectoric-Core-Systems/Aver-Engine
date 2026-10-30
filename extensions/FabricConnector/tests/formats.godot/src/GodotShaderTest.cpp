// GodotShaderTest -- G6 SH1: the Godot shading-language front-end (modules/formats.godot, GodotShader.hpp).
//
//   * the preprocessor (#define with arguments, ##, #undef, #if / #elif / #else / #ifdef / defined(), #include through a resolver, #pragma, #error,
//     CRLF input, the line map);
//   * lexer + parser + type checker: errors with line and column, operator precedence (folded and compared with C++), every statement kind,
//     overload resolution, built-in variable access rules, uniform hints, render modes;
//   * the uniform layout / packing;
//   * HLSL emission: representative real-world-shaped shaders (water, toon, dissolve, triplanar, vertex wind, fresnel rim, a language torture
//     shader, particles, sky, fog) compiled by the engine's DXC to DXIL (build/bin/dxcompiler.dll) and to SPIR-V (third_party/dxc-spirv) with a
//     stub hook prelude. No device, no window. The DXC halves skip (and say so) when a compiler DLL is absent.
#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/godot/GodotShader.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/DxcApi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fmt::godot;
using rhi::ComPtr;
namespace fs = std::filesystem;

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ------------------------------------------------------------------------------------------------ helpers

static std::string diagText(const std::vector<GdShaderDiag>& d) {
    std::string s;
    for (const GdShaderDiag& x : d) s += x.str() + "\n";
    return s;
}

static GdShaderModule compile(const std::string& src, const GdCompileOptions& opt = {}) {
    GdShaderModule m;
    gdShaderCompile(src, "test.gdshader", opt, m);
    return m;
}

static bool compilesClean(const std::string& src, const std::string& what, GdShaderModule* out = nullptr, const GdCompileOptions& opt = {}) {
    GdShaderModule m = compile(src, opt);
    const bool ok = m.ok;
    if (!ok) AVER_ERROR("{}", diagText(m.diags));
    check(ok, what);
    if (out) *out = std::move(m);
    return ok;
}

// 1-based line / column of the first occurrence of `marker` in `src`.
static void locate(const std::string& src, const std::string& marker, int& line, int& col) {
    const usize p = src.find(marker);
    line = 1;
    col = 1;
    for (usize i = 0; i < p && i < src.size(); ++i) {
        if (src[i] == '\n') { ++line; col = 1; }
        else ++col;
    }
}

// The shader must fail with an error at `marker` whose message contains `what`.
static void expectErrorAt(const std::string& src, const std::string& marker, const std::string& what, const GdCompileOptions& opt = {}) {
    const GdShaderModule m = compile(src, opt);
    int line = 0, col = 0;
    locate(src, marker, line, col);
    const GdShaderDiag* e = nullptr;
    for (const GdShaderDiag& d : m.diags) if (d.severity == GodotSeverity::Error) { e = &d; break; }
    bool ok = e != nullptr && !m.ok;
    if (ok) ok = e->line == line && e->column == col && e->message.find(what) != std::string::npos;
    if (!ok) AVER_ERROR("expected error '{}' at {}:{}; got: {}", what, line, col, diagText(m.diags));
    check(ok, "error '" + what + "' at " + std::to_string(line) + ":" + std::to_string(col));
}

static const GdConstDecl* findConst(const GdShaderModule& m, const std::string& n) {
    for (const GdConstDecl& c : m.consts) if (c.name == n) return &c;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ preprocessor

static std::string pre(const std::string& src, const GdPreprocessOptions& opt, std::vector<GdShaderDiag>* diags = nullptr, GdPreprocessed* keep = nullptr) {
    GdPreprocessed out;
    std::vector<GdShaderDiag> d;
    gdShaderPreprocess(src, "main.gdshader", opt, out, d);
    if (diags) *diags = d;
    if (keep) *keep = out;
    return out.text;
}

static std::string squeeze(const std::string& s) {
    std::string o;
    for (char c : s) if (c != '\n' && c != ' ') o += c;
    return o;
}

static void testPreprocessor() {
    AVER_INFO("=== preprocessor ===");
    GdPreprocessOptions opt;

    check(squeeze(pre("#define A 3\nfloat x = A;", opt)) == "floatx=3;", "object-like #define");
    check(squeeze(pre("#define SQ(a) ((a)*(a))\nfloat x = SQ(2.0 + 1.0);", opt)) == "floatx=((2.0+1.0)*(2.0+1.0));", "function-like #define with an argument");
    check(squeeze(pre("#define ADD(a, b) a + b\nint x = ADD(1, 2) * ADD(3, 4);", opt)) == "intx=1+2*3+4;", "two arguments, no implicit parentheses");
    check(squeeze(pre("#define MIX(a,b) mix(a,b,0.5)\nvec3 v = MIX(vec3(0.0), MIX(vec3(1.0), vec3(2.0)));", opt)) ==
              "vec3v=mix(vec3(0.0),mix(vec3(1.0),vec3(2.0),0.5),0.5);", "nested macro calls");
    check(squeeze(pre("#define CAT(a,b) a##b\nint CAT(my,Var) = 1;", opt)) == "intmyVar=1;", "## token pasting");
    check(squeeze(pre("#define A B\n#define B 5\nint x = A;", opt)) == "intx=5;", "a macro expands macros in its body");
    check(squeeze(pre("#define LOOP LOOP + 1\nint x = LOOP;", opt)) == "intx=LOOP+1;", "a macro is not expanded inside itself");
    check(squeeze(pre("#define A 1\n#undef A\nint x = A;", opt)) == "intx=A;", "#undef");
    check(squeeze(pre("#define LONG 1 + \\\n 2\nint x = LONG;", opt)) == "intx=1+2;", "backslash continuation in #define");
    {
        const std::string s = pre("#define F(a,b) (a+b)\nint x = F(1,\n 2);\nint y;", opt);
        GdPreprocessed keep;
        pre("#define F(a,b) (a+b)\nint x = F(1,\n 2);\nint y;", opt, nullptr, &keep);
        check(squeeze(s) == "intx=(1+2);inty;" && keep.lines.size() == 4, "a macro call spanning lines is joined and the line count is kept");
    }

    // conditionals
    check(squeeze(pre("#define X\n#ifdef X\nint a;\n#else\nint b;\n#endif", opt)) == "inta;", "#ifdef taken");
    check(squeeze(pre("#ifndef X\nint a;\n#else\nint b;\n#endif", opt)) == "inta;", "#ifndef taken");
    check(squeeze(pre("#define V 2\n#if V == 1\nint a;\n#elif V == 2\nint b;\n#else\nint c;\n#endif", opt)) == "intb;", "#elif");
    check(squeeze(pre("#if defined(Q) || 1\nint a;\n#endif", opt)) == "inta;", "defined() and ||");
    check(squeeze(pre("#if !defined Q && (3 * 2 > 5)\nint a;\n#endif", opt)) == "inta;", "defined without parentheses, arithmetic, comparison");
    check(squeeze(pre("#if 0\n#if 1\nint a;\n#endif\nint b;\n#else\nint c;\n#endif", opt)) == "intc;", "nested #if inside a skipped region");
    check(squeeze(pre("#define D 4\n#if D > 3 ? 1 : 0\nint a;\n#endif", opt)) == "inta;", "ternary in #if");
    {
        GdPreprocessOptions o2;
        o2.defines = {{"RENDERER_FORWARD_PLUS", "1"}};
        check(squeeze(pre("#ifdef RENDERER_FORWARD_PLUS\nint a;\n#endif", o2)) == "inta;", "predefined macros");
    }

    // comments and line endings
    check(squeeze(pre("int a; // tail\n/* multi\nline */ int b;", opt)) == "inta;intb;", "line and block comments are removed");
    check(squeeze(pre("int a;\r\nint b;\r\n", opt)) == "inta;intb;", "CRLF input");
    {
        GdPreprocessed keep;
        pre("/* two\nlines */\nint a;\n", opt, nullptr, &keep);
        check(keep.lines.size() >= 3 && keep.lines[2].line == 3 && keep.lines[2].file == 0, "a block comment keeps the line numbers");
    }
    check(squeeze(pre("uniform int e : hint_enum(\"a // b\", \"c\");", opt)) == "uniformInte:hint_enum(\"a//b\",\"c\");" ||
              pre("uniform int e : hint_enum(\"a // b\", \"c\");", opt).find("a // b") != std::string::npos,
          "a // inside a string is not a comment");

    // includes
    {
        GdPreprocessOptions o2;
        o2.resolver = [](const std::string& path, const std::string&, std::string& text, std::string& resolved) {
            if (path == "res://lib.gdshaderinc") { text = "#define LIBVAL 7\nfloat helper() { return 1.0; }\n"; resolved = "lib.gdshaderinc"; return true; }
            if (path == "res://a.gdshaderinc") { text = "#include \"res://b.gdshaderinc\"\nint fromA;\n"; resolved = "a.gdshaderinc"; return true; }
            if (path == "res://b.gdshaderinc") { text = "int fromB;\n"; resolved = "b.gdshaderinc"; return true; }
            if (path == "res://loop.gdshaderinc") { text = "#include \"res://loop.gdshaderinc\"\n"; resolved = "loop.gdshaderinc"; return true; }
            return false;
        };
        GdPreprocessed keep;
        std::vector<GdShaderDiag> d;
        const std::string out = pre("#include \"res://lib.gdshaderinc\"\nfloat x = LIBVAL;", o2, &d, &keep);
        check(d.empty() && squeeze(out) == "floathelper(){return1.0;}floatx=7;", "#include inlines a file and its macros reach the includer");
        bool mapped = false;
        for (usize i = 0; i < keep.lines.size(); ++i)
            if (keep.files[keep.lines[i].file] == "lib.gdshaderinc" && keep.lines[i].line == 2) mapped = true;
        check(mapped && keep.files.size() == 2, "the line map names the included file and its line");
        check(squeeze(pre("#include \"res://a.gdshaderinc\"", o2)) == "intfromB;intfromA;", "nested #include");
        pre("#include \"res://missing.gdshaderinc\"", o2, &d);
        check(!d.empty() && d[0].line == 1 && d[0].message.find("cannot open") != std::string::npos, "a missing include is an error at its line");
        pre("#include \"res://loop.gdshaderinc\"", o2, &d);
        check(!d.empty() && d.back().message.find("nested too deeply") != std::string::npos, "a self-including file stops at the depth limit");
        pre("#include \"x\"", opt, &d);
        check(!d.empty(), "#include without a resolver is an error");
    }

    // pragma and errors
    {
        GdPreprocessed keep;
        const std::string out = pre("#pragma disable_preprocessor\n#define A 1\nint x = A;", opt, nullptr, &keep);
        check(keep.pragmas.size() == 1 && keep.pragmas[0] == "disable_preprocessor", "#pragma is recorded");
        check(out.find("#define A 1") != std::string::npos && out.find("= A;") != std::string::npos, "disable_preprocessor stops directives and macros for the rest of the file");
    }
    {
        std::vector<GdShaderDiag> d;
        pre("#error stop here", opt, &d);
        check(d.size() == 1 && d[0].line == 1 && d[0].message.find("stop here") != std::string::npos, "#error");
        pre("#if 1\nint a;", opt, &d);
        check(!d.empty() && d[0].message.find("unterminated") != std::string::npos, "unterminated #if");
        pre("#endif", opt, &d);
        check(!d.empty(), "#endif without #if");
        pre("#bogus", opt, &d);
        check(!d.empty() && d[0].message.find("unknown preprocessor directive") != std::string::npos, "unknown directive");
        pre("#define F(a) a\nint x = F(1, 2);", opt, &d);
        check(!d.empty() && d[0].line == 2, "macro argument count mismatch");
        pre("#if 1 +\nint a;\n#endif", opt, &d);
        check(!d.empty(), "a malformed #if expression is an error");
    }
}

// ------------------------------------------------------------------------------------------------ parser and checker

static void testPrecedence() {
    AVER_INFO("=== operator precedence ===");
    const std::string src = R"(shader_type spatial;
const int A = 2 + 3 * 4;
const int B = (2 + 3) * 4;
const int C = 1 << 2 + 1;
const int D = 7 & 3 | 8 ^ 1;
const int E = 10 - 4 - 3;
const int F = 100 / 10 / 5;
const int G = 17 % 5 * 2;
const bool H = 1 < 2 == true;
const bool I = 1 == 1 && 2 > 3 || true;
const int J = true ? 1 : false ? 2 : 3;
const int K = false ? 1 : false ? 2 : 3;
const int L = -2 * -3;
const int M = ~0 & 0xFF;
const float O = 1.0 + 2.0 * 3.0 / 4.0;
const int P = 5 - -3;
const int Q = 2 * (3 + 4) % 5;
const int R = 1 | 2 & 3 ^ 4;
const int S = 8 >> 1 << 2;
const bool T = !true || !false && false;
const int U = 6 / 4;
const int V = 1 + 2 << 1;
const vec3 W = vec3(1.0, 2.0, 3.0) * 2.0 + 1.0;
const float X = vec2(3.0, 4.0).y - 1.0;
const mat2 Y = mat2(2.0) * mat2(3.0);
void fragment() {}
)";
    GdShaderModule m;
    if (!compilesClean(src, "the precedence shader compiles", &m)) return;
    struct E { const char* n; double v; };
    const E expect[] = {
        {"A", 2 + 3 * 4}, {"B", (2 + 3) * 4}, {"C", 1 << (2 + 1)}, {"D", (7 & 3) | (8 ^ 1)}, {"E", 10 - 4 - 3}, {"F", 100 / 10 / 5}, {"G", 17 % 5 * 2},
        {"H", 1}, {"I", 1}, {"J", 1}, {"K", 3}, {"L", 6}, {"M", 255}, {"O", 1.0 + 2.0 * 3.0 / 4.0}, {"P", 8}, {"Q", 2 * (3 + 4) % 5},
        {"R", 1 | ((2 & 3) ^ 4)}, {"S", (8 >> 1) << 2}, {"T", 0 || (!false && false)}, {"U", 6 / 4}, {"V", (1 + 2) << 1},
    };
    for (const E& e : expect) {
        const GdConstDecl* c = findConst(m, e.n);
        check(c && c->value.valid && std::fabs(c->value.v[0] - e.v) < 1e-9, std::string("const ") + e.n + " folds to " + std::to_string(e.v));
    }
    const GdConstDecl* w = findConst(m, "W");
    check(w && w->value.valid && w->value.v.size() == 3 && w->value.v[0] == 3 && w->value.v[1] == 5 && w->value.v[2] == 7, "vec3 constant arithmetic folds");
    const GdConstDecl* x = findConst(m, "X");
    check(x && x->value.valid && x->value.v[0] == 3.0, "swizzle of a constructor folds");
    const GdConstDecl* y = findConst(m, "Y");
    check(y && y->value.valid && y->value.v.size() == 4 && y->value.v[0] == 6 && y->value.v[3] == 6 && y->value.v[1] == 0, "mat2 * mat2 folds");
}

static int countKinds(const GdStmt& s, std::set<GdStmtKind>& seen) {
    seen.insert(s.kind);
    int n = 1;
    if (s.init) n += countKinds(*s.init, seen);
    if (s.thenS) n += countKinds(*s.thenS, seen);
    if (s.elseS) n += countKinds(*s.elseS, seen);
    for (const GdStmtP& b : s.body) n += countKinds(*b, seen);
    return n;
}

static void testStatements() {
    AVER_INFO("=== statements ===");
    const std::string src = R"(shader_type spatial;
float f(int n) {
    float acc = 0.0;
    for (int i = 0, k = 1; i < n; i++, k++) {
        if (i == 2) continue;
        if (i > 6) break;
        else acc += float(k);
    }
    int j = 0;
    while (j < 3) { acc += 1.0; j++; }
    do { acc -= 0.5; j--; } while (j > 0);
    switch (n) {
        case 0: acc += 1.0; break;
        case 1:
        case 2: acc += 2.0; break;
        default: acc = 0.0;
    }
    {
        float inner = acc;
        acc = inner * 2.0;
    }
    ;
    return acc;
}
void fragment() {
    if (ALPHA > 2.0) discard;
    ALBEDO = vec3(f(3));
}
)";
    GdShaderModule m;
    if (!compilesClean(src, "a shader using every statement kind compiles", &m)) return;
    std::set<GdStmtKind> seen;
    for (const GdFunction& fn : m.functions) if (fn.body) countKinds(*fn.body, seen);
    const GdStmtKind all[] = {GdStmtKind::Block, GdStmtKind::Decl, GdStmtKind::Expr, GdStmtKind::If, GdStmtKind::For, GdStmtKind::While, GdStmtKind::DoWhile,
                              GdStmtKind::Switch, GdStmtKind::Case, GdStmtKind::Default, GdStmtKind::Break, GdStmtKind::Continue, GdStmtKind::Return,
                              GdStmtKind::Discard};
    bool allSeen = true;
    for (GdStmtKind k : all) allSeen = allSeen && seen.count(k);
    check(allSeen, "the AST holds a node of every statement kind");
    check(m.functions.size() == 2 && m.functions[1].processor == GdProcessor::Fragment && m.functions[1].usesDiscard, "fragment() is a processor and notes discard");
    const GdStmt* forS = nullptr;
    for (const GdStmtP& s : m.functions[0].body->body) if (s->kind == GdStmtKind::For) forS = s.get();
    check(forS && forS->init && forS->init->decls.size() == 2 && forS->step && forS->step->kind == GdExprKind::Comma, "for: two declarators and a comma step");
}

static void testOverloads() {
    AVER_INFO("=== overload resolution ===");
    const std::string src = R"(shader_type spatial;
float f(float x) { return 1.0; }
float f(int x) { return 2.0; }
float f(vec2 x) { return 3.0; }
float f(float x, float y) { return 4.0; }
void fragment() {
    ALBEDO = vec3(f(1.0) + f(1) + f(vec2(1.0)) + f(1.0, 2.0));
}
)";
    GdShaderModule m;
    if (!compilesClean(src, "user overloads compile", &m)) return;
    std::set<int> picked;
    std::function<void(const GdExpr&)> walk = [&](const GdExpr& e) {
        if (e.kind == GdExprKind::Call && e.functionIdx >= 0) picked.insert(e.functionIdx);
        for (const GdExprP& a : e.args) walk(*a);
    };
    std::function<void(const GdStmt&)> walkS = [&](const GdStmt& s) {
        if (s.expr) walk(*s.expr);
        for (const GdStmtP& b : s.body) walkS(*b);
    };
    walkS(*m.functions[4].body);
    check(picked == std::set<int>({0, 1, 2, 3}), "each call picks its own overload by exact argument types");
    expectErrorAt("shader_type spatial;\nfloat f(float x) { return x; }\nvoid fragment() { ALBEDO = vec3(f(1u)); }\n", "f(1u)", "no overload of 'f'");
    expectErrorAt("shader_type spatial;\nfloat f(float x) { return x; }\nfloat f(float y) { return y; }\nvoid fragment() {}\n", "f(float y)", "already defined");

    check(gdBuiltinOverloads("mix").size() == 11, "mix has 11 distinct overloads");
    check(gdBuiltinOverloads("texture").size() == 22, "texture: two forms for each of the 11 sampler types");
    check(gdBuiltinOverloads("radians").size() == 4 && gdIsBuiltinFunction("fract") && !gdIsBuiltinFunction("lerp"), "built-in lookup");
    {
        bool found = false;
        for (const GdBuiltinSig& s : gdBuiltinOverloads("modf"))
            if (s.params.size() == 2 && s.outParam[1] && s.params[0] == GdBase::Vec3) found = true;
        check(found, "modf(vec3, out vec3) is an out parameter overload");
    }
    GdShaderModule m2;
    compilesClean(R"(shader_type spatial;
void fragment() {
    float a = max(1.0, 2.0);
    vec3 b = max(vec3(1.0), 2.0);
    ivec2 c = clamp(ivec2(5), 0, 3);
    uint d = min(3u, 4u);
    vec2 fr;
    vec2 ip = modf(vec2(1.5), fr);
    bvec2 lt = lessThan(vec2(0.0), vec2(1.0));
    float e = atan(1.0, 2.0) + atan(0.5);
    ALBEDO = b + vec3(a, float(c.x), float(d)) + vec3(ip + fr, e) + vec3(float(any(lt)));
})", "scalar/vector mixes of min, max, clamp, modf, atan and relational built-ins", &m2);
}

static void testErrors() {
    AVER_INFO("=== errors carry line and column ===");
    expectErrorAt("void fragment() {}\n", "void", "must begin with 'shader_type'");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    ALBEDO = vec3(foo);\n}\n", "foo", "unknown identifier 'foo'");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float f = 1;\n}\n", "1;", "cannot initialise 'float' with 'int'");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    vec3 v = vec3(1.0) * vec2(1.0);\n}\n", "* vec2", "invalid operands to '*'");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float f = 1 + 1.0;\n}\n", "+ 1.0", "invalid operands to '+'");
    expectErrorAt("shader_type spatial;\nuniform float u;\nvoid fragment() {\n    u = 2.0;\n}\n", "= 2.0", "read-only");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    VIEW = vec3(0.0);\n}\n", "= vec3(0.0)", "read-only");
    expectErrorAt("shader_type spatial;\nfloat g() {\n    return VERTEX.x;\n}\nvoid fragment() {}\n", "VERTEX", "not accessible in a custom function");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    VERTEX_ID;\n}\n", "VERTEX_ID", "not available in fragment()");
    expectErrorAt("shader_type spatial;\nfloat g(float x) {\n    if (x > 0.0) return 1.0;\n}\nvoid fragment() {}\n", "g(float x)", "must return a value");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    ALBEDO = vec3(max(1.0));\n}\n", "max(1.0)", "no overload of 'max'");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float a = 1.0;\n    float a = 2.0;\n}\n", "a = 2.0", "redefinition of 'a'");
    expectErrorAt("shader_type spatial;\nvoid vertex() {\n    discard;\n}\n", "discard", "not allowed in vertex()");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    break;\n}\n", "break", "outside a loop");
    expectErrorAt("shader_type spatial;\nrender_mode blend_mix, nonsense;\nvoid fragment() {}\n", "nonsense", "unknown render mode");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float f = 1.0\n}\n", "}", "expected ';'");
    expectErrorAt("shader_type spatial;\nvarying vec3 v;\nvoid helper() {\n    v = vec3(0.0);\n}\nvoid fragment() {}\n", "=", "varying 'v' can be assigned only in vertex()");
    expectErrorAt("shader_type spatial;\n#include \"nope\"\nvoid fragment() {}\n", "#include", "cannot open include");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    switch (1) { case 1: break; case 1: break; }\n}\n", "1: break; }", "duplicate case label");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float v = 1.0;\n    switch (v) { default: break; }\n}\n", "v)", "must be a signed int");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    vec3 v = vec3(1.0);\n    float a = v[3];\n}\n", "[3]", "out of range");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    vec3 v = vec3(1.0);\n    float a = v.xr;\n}\n", "xr", "invalid swizzle");
    expectErrorAt("shader_type spatial;\nfloat sin(float x) { return x; }\nvoid fragment() {}\n", "sin", "redefines a built-in");
    expectErrorAt("shader_type spatial;\nstruct S { float a; };\nvoid fragment() {\n    S s = S(1.0);\n    float b = s.nope;\n}\n", "nope", "has no member");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    ALBEDO = vec3(later());\n}\nfloat later() { return 1.0; }\n", "later", "unknown function");
    expectErrorAt("shader_type spatial;\ninstance uniform sampler2D t;\nvoid fragment() {}\n", "instance uniform", "must be a scalar or vector");
    expectErrorAt("shader_type spatial;\nuniform float f : hint_normal;\nvoid fragment() {}\n", "hint_normal", "sampler2D");
    expectErrorAt("shader_type spatial;\nuniform float f : hint_bogus;\nvoid fragment() {}\n", "hint_bogus", "unknown uniform hint");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    int i = 0;\n    float f = float[2](1.0, 2.0)[i] + 1;\n}\n", "+ 1;", "invalid operands");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    if (1) {}\n}\n", "1)", "condition must be bool");
    expectErrorAt("shader_type spatial;\nconst float c = 1.0;\nvoid fragment() {\n    c = 2.0;\n}\n", "= 2.0", "cannot modify the constant");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    mat3 m = mat3(vec3(1.0), vec3(1.0));\n}\n", "mat3(vec3(1.0), vec3(1.0))", "needs one scalar");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    ALBEDO = vec3(1.0) @ 2.0;\n}\n", "@", "unexpected character");
    expectErrorAt("shader_type spatial;\nuniform float u = vec3(1.0);\nvoid fragment() {}\n", "vec3(1.0);", "cannot initialise");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    float x = TIME;\n    x.y = 1.0;\n}\n", "y = 1.0", "has no member");
    expectErrorAt("shader_type spatial;\nvoid fragment() {\n    vec2 a; vec2 b;\n    (a, b).x = 1.0;\n}\n", "=", "not assignable");

    // an error inside an include names the include
    {
        GdCompileOptions opt;
        opt.pre.resolver = [](const std::string&, const std::string&, std::string& text, std::string& resolved) {
            text = "float broken() {\n    return 1;\n}\n";
            resolved = "lib.gdshaderinc";
            return true;
        };
        const GdShaderModule m = compile("shader_type spatial;\n#include \"res://lib.gdshaderinc\"\nvoid fragment() {}\n", opt);
        check(!m.ok && !m.diags.empty() && m.diags.back().file == "lib.gdshaderinc" && m.diags.back().line == 2, "a type error inside an include reports that file and line");
    }
}

static void testLanguage() {
    AVER_INFO("=== language features ===");
    // shader types and canvas_item flag
    for (const char* t : {"spatial", "particles", "sky", "fog"}) {
        GdShaderModule m = compile(std::string("shader_type ") + t + ";\n");
        check(m.ok && !m.is2D, std::string("shader_type ") + t + " parses");
    }
    {
        GdShaderModule m = compile("shader_type canvas_item;\nvoid fragment() { COLOR = texture(TEXTURE, UV); }\n");
        bool flagged = false;
        for (const GdShaderDiag& d : m.diags) if (d.code == "gdshader.unsupported_2d") flagged = true;
        check(m.ok && m.is2D && flagged, "canvas_item parses and is flagged 2D-unsupported");
        const GdHlslResult r = gdShaderEmitHlsl(m);
        check(!r.ok && r.error.find("not supported") != std::string::npos, "the HLSL emitter refuses canvas_item");
    }
    // render modes and processors
    {
        GdShaderModule m;
        compilesClean("shader_type spatial;\nrender_mode unshaded, cull_disabled, blend_add;\nrender_mode depth_draw_always;\nvoid vertex() {}\nvoid fragment() {}\nvoid light() {}\n", "render_mode lists", &m);
        check(m.renderModes.size() == 4 && m.hasRenderMode("cull_disabled") && m.processor(GdProcessor::Light) && m.processor(GdProcessor::Vertex), "render modes and processors are recorded");
    }
    {
        // fragment() is only a processor function in spatial / canvas_item shaders; elsewhere it is an ordinary name
        GdShaderModule m;
        compilesClean("shader_type particles;\nvoid fragment() {}\nvoid start() {}\n", "fragment() in a particles shader is an ordinary function", &m);
        check(m.functions.size() == 2 && m.functions[0].processor == GdProcessor::None && m.functions[1].processor == GdProcessor::Start, "only start() is a processor there");
    }
    expectErrorAt("shader_type spatial;\nvoid fragment() {}\nvoid fragment() { }\n", "fragment() { }", "defined twice");
}

static void testUniformsAndHints() {
    AVER_INFO("=== uniforms and hints ===");
    const std::string src = R"(shader_type spatial;
group_uniforms look;
uniform vec4 tint : source_color = vec4(1.0, 0.5, 0.25, 1.0);
uniform float amount : hint_range(0.0, 2.0, 0.05) = 1.0;
uniform int choice : hint_enum("Low", "Mid", "High") = 1;
group_uniforms look.detail;
uniform sampler2D nrm : hint_normal, filter_nearest, repeat_enable;
uniform sampler2D white_tex : hint_default_white;
uniform sampler2D rough : hint_roughness_g, filter_linear_mipmap_anisotropic;
group_uniforms;
uniform sampler2D screen : hint_screen_texture;
uniform sampler2D depth : hint_depth_texture;
uniform sampler2D nr : hint_normal_roughness_texture;
uniform sampler2D flow : hint_anisotropy;
uniform float arr[3];
uniform vec2 grid = vec2(2.0, 3.0) * 2.0;
instance uniform vec3 inst_a : source_color;
instance uniform float inst_b : instance_index(3) = 0.5;
global uniform float wind_strength;
uniform samplerCube env;
uniform sampler3D vol : repeat_disable;
uniform samplerCube sky_cube : filter_linear, source_color, hint_default_black;
void fragment() {}
)";
    GdShaderModule m;
    if (!compilesClean(src, "uniform declarations with every hint kind", &m)) return;
    auto u = [&](const char* n) -> const GdUniform& { return m.uniforms[static_cast<usize>(m.findUniform(n))]; };
    check(u("tint").sourceColor && u("tint").hasDefault && u("tint").def.valid && u("tint").def.v.size() == 4 && u("tint").def.v[1] == 0.5, "source_color and a vec4 default");
    check(u("amount").hasRange && u("amount").rangeMin == 0.0 && u("amount").rangeMax == 2.0 && std::fabs(u("amount").rangeStep - 0.05) < 1e-9, "hint_range(min, max, step)");
    check(u("choice").enumLabels.size() == 3 && u("choice").enumLabels[2] == "High", "hint_enum labels");
    check(u("nrm").texHint == GdTexHint::Normal && u("nrm").filter == GdFilter::Nearest && u("nrm").repeat == GdRepeat::Enable, "hint_normal, filter_nearest, repeat_enable");
    check(u("rough").texHint == GdTexHint::RoughnessG && u("rough").filter == GdFilter::LinearMipmapAniso, "hint_roughness_g and an anisotropic filter");
    check(u("white_tex").texHint == GdTexHint::DefaultWhite, "hint_default_white");
    check(u("screen").texHint == GdTexHint::ScreenTexture && u("depth").texHint == GdTexHint::DepthTexture && u("nr").texHint == GdTexHint::NormalRoughnessTexture, "screen, depth and normal-roughness textures");
    check(u("flow").texHint == GdTexHint::Anisotropy, "hint_anisotropy");
    check(u("arr").type.arraySize == 3 && u("grid").def.valid && u("grid").def.v[0] == 4.0 && u("grid").def.v[1] == 6.0, "an array uniform and a constant-folded default");
    check(u("inst_a").scope == GdUniformScope::Instance && u("inst_b").instanceIndex == 3 && u("wind_strength").scope == GdUniformScope::Global, "instance and global uniforms");
    check(u("tint").group == "look" && u("tint").subgroup.empty() && u("nrm").group == "look" && u("nrm").subgroup == "detail" && u("screen").group.empty(), "group_uniforms groups and subgroups");
    check(u("vol").repeat == GdRepeat::Disable && u("env").type.base == GdBase::SamplerCube, "sampler3D and samplerCube uniforms");
    check(u("sky_cube").texHint == GdTexHint::DefaultBlack && u("sky_cube").sourceColor, "hint_default_black on a samplerCube (the platformer demo's sky does)");
    expectErrorAt("shader_type spatial;\nuniform samplerCube c : hint_normal;\nvoid fragment() {}\n", "hint_normal", "applies to sampler2D uniforms");

    GdCompileOptions opt;
    opt.globalUniformNames = {"other"};
    expectErrorAt("shader_type spatial;\nglobal uniform float missing;\nvoid fragment() {}\n", "missing", "not defined in the project's shader globals", opt);
}

static void testBuiltinVariables() {
    AVER_INFO("=== built-in variables per processor function ===");
    compilesClean(R"(shader_type spatial;
varying vec3 wp;
void vertex() {
    wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    VERTEX += NORMAL * 0.1 * sin(TIME);
    UV = UV * 2.0;
    COLOR.a = 0.5;
    POINT_SIZE = 4.0;
    ROUGHNESS = 0.1;
    MODELVIEW_MATRIX = VIEW_MATRIX * MODEL_MATRIX;
}
void fragment() {
    ALBEDO = wp + vec3(UV, 0.0) + COLOR.rgb;
    ALPHA = FRONT_FACING ? 1.0 : 0.5;
    NORMAL = normalize(NORMAL);
    EMISSION += vec3(SCREEN_UV, FRAGCOORD.z);
    DEPTH = FRAGCOORD.z;
    RIM = 0.1; CLEARCOAT = 0.2; ANISOTROPY = 0.3; AO = 1.0; SSS_STRENGTH = 0.0;
    BACKLIGHT = vec3(0.1);
}
void light() {
    DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * dot(NORMAL, LIGHT) * ATTENUATION;
    SPECULAR_LIGHT += vec3(SPECULAR_AMOUNT);
    ALPHA = LIGHT_IS_DIRECTIONAL ? 1.0 : 0.5;
})", "spatial built-ins in vertex(), fragment() and light()");
    compilesClean(R"(shader_type particles;
void start() { TRANSFORM = EMISSION_TRANSFORM; VELOCITY = vec3(RANDOM_SEED % 7u == 0u ? 1.0 : 0.0); CUSTOM = vec4(0.0); ACTIVE = true; }
void process() { VELOCITY.y -= 9.8 * DELTA; TRANSFORM[3].xyz += VELOCITY * DELTA; if (RESTART) { COLOR = vec4(1.0); } if (COLLIDED) { ACTIVE = false; } }
)", "particles built-ins in start() and process()");
    compilesClean("shader_type sky;\nvoid sky() { COLOR = EYEDIR * 0.5 + 0.5; ALPHA = 1.0; FOG = vec4(0.0); if (AT_CUBEMAP_PASS) { COLOR = LIGHT0_COLOR; } }\n", "sky built-ins");
    compilesClean("shader_type fog;\nvoid fog() { DENSITY = length(WORLD_POSITION - OBJECT_POSITION) / SIZE.x; ALBEDO = UVW; EMISSION = vec3(SDF); }\n", "fog built-ins");
    expectErrorAt("shader_type spatial;\nvoid vertex() {\n    ALBEDO = vec3(1.0);\n}\n", "ALBEDO", "not available in vertex()");
    expectErrorAt("shader_type spatial;\nvoid light() {\n    ALBEDO = vec3(1.0);\n}\n", "=", "read-only");
    expectErrorAt("shader_type particles;\nvoid start() {\n    float x = DELTA;\n    DELTA = 1.0;\n}\nvoid process() {}\n", "= 1.0", "read-only");
    expectErrorAt("shader_type particles;\nvoid start() {\n    bool b = COLLIDED;\n}\n", "COLLIDED", "not available in start()");
    // a custom function sees only TIME, PI, TAU, E
    compilesClean("shader_type spatial;\nfloat helper(float x) { return x * PI + TAU * E + TIME; }\nvoid fragment() { ALBEDO = vec3(helper(1.0)); }\n", "custom functions read TIME, PI, TAU and E");
}

static void testLayout() {
    AVER_INFO("=== uniform layout ===");
    GdShaderModule m;
    if (!compilesClean(R"(shader_type spatial;
uniform float a;
uniform vec3 b;
uniform float c;
uniform vec2 d;
uniform float e;
uniform vec4 f;
uniform mat3 g;
uniform float h[3];
uniform vec2 i;
uniform vec3 j;
uniform int k;
uniform ivec2 l;
uniform bool m;
uniform mat4 n;
uniform sampler2D tex;
instance uniform float inst0;
instance uniform vec2 inst1;
global uniform vec4 glob;
void fragment() {}
)", "the layout shader compiles", &m)) return;
    GdUniformBlock nb, ib, gb;
    gdShaderLayoutUniforms(m, nb, ib, gb);
    auto slot = [&](const char* n) -> const GdUniformSlot* {
        for (const GdUniformSlot& s : nb.slots) if (s.name == n) return &s;
        return nullptr;
    };
    struct Ex { const char* n; int reg, comp, regs; };
    const Ex ex[] = {{"a", 0, 0, 1}, {"b", 1, 0, 1}, {"c", 1, 3, 1}, {"d", 2, 0, 1}, {"e", 2, 2, 1}, {"f", 3, 0, 1}, {"g", 4, 0, 3}, {"h", 7, 0, 3},
                     {"i", 10, 0, 1}, {"j", 11, 0, 1}, {"k", 11, 3, 1}, {"l", 12, 0, 1}, {"m", 12, 2, 1}, {"n", 13, 0, 4}};
    for (const Ex& e : ex) {
        const GdUniformSlot* s = slot(e.n);
        check(s && s->reg == e.reg && s->comp == e.comp && s->registers == e.regs,
              std::string("uniform ") + e.n + " at register " + std::to_string(e.reg) + "." + std::to_string(e.comp));
    }
    check(nb.registerCount == 17 && nb.slots.size() == 14, "the block is 17 registers and holds no texture");
    check(ib.slots.size() == 2 && ib.slots[0].reg == 0 && ib.slots[1].reg == 0 && ib.slots[1].comp == 2 && ib.registerCount == 1, "instance uniforms get their own block");
    check(gb.slots.size() == 1 && gb.slots[0].reg == 0 && gb.registerCount == 1, "global uniforms get their own block");
    const GdHlslResult r = gdShaderEmitHlsl(m);
    check(r.ok && r.textures.size() == 1 && r.textures[0].reg == 0 && r.uniforms.registerCount == 17, "the emitter reports the same layout and one texture slot");
}

// ------------------------------------------------------------------------------------------------ DXC

class Dxc {
public:
    bool load(const fs::path& dll, bool spirv) {
        spirv_ = spirv;
        if (!fs::exists(dll)) return false;
        dll_ = loadLibrary(reinterpret_cast<const char*>(dll.u8string().c_str()));
        if (!dll_) return false;
        auto create = reinterpret_cast<DxcCreateInstanceProc>(librarySymbol(dll_, "DxcCreateInstance"));
        if (!create) return false;
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) return false;
        if (spirv) {
            // the redistributable DXC has no SPIR-V back end; a probe tells the two apart
            std::string err;
            if (!compile("float4 main() : SV_Target { return 0; }", "main", "ps_6_5", "", err) && err.find("SPIR-V") != std::string::npos) return false;
        }
        return true;
    }
    bool ok() const { return compiler_ != nullptr; }

    bool compile(const std::string& source, const char* entry, const char* target, const std::string& defines, std::string& err) {
        err.clear();
        std::vector<std::wstring> args;
        auto w = [](const char* s) { return std::wstring(s, s + std::strlen(s)); };
        args.push_back(L"-E"); args.push_back(w(entry));
        args.push_back(L"-T"); args.push_back(w(target));
        args.push_back(L"-Zpr");
        args.push_back(L"-HV"); args.push_back(L"2021");
        if (spirv_) {
            args.push_back(L"-spirv");
            args.push_back(L"-fspv-target-env=vulkan1.3");
            for (const wchar_t* a : {L"-fvk-t-shift", L"16", L"0", L"-fvk-s-shift", L"48", L"0", L"-fvk-b-shift", L"0", L"0", L"-fvk-u-shift", L"64", L"0"}) args.push_back(a);
            if (target[0] == 'v') args.push_back(L"-fvk-invert-y");
        } else {
            args.push_back(L"-Vd");
        }
        for (usize b = 0; b <= defines.size();) {
            const usize e = std::min(defines.find(';', b), defines.size());
            if (e > b) { args.push_back(L"-D"); const std::string d = defines.substr(b, e - b); args.emplace_back(d.begin(), d.end()); }
            b = e + 1;
        }
        std::vector<LPCWSTR> argv;
        for (const std::wstring& a : args) argv.push_back(a.c_str());
        DxcBuffer buf{source.c_str(), source.size(), DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        HRESULT hr = compiler_->Compile(&buf, argv.data(), static_cast<UINT32>(argv.size()), nullptr, IID_PPV_ARGS(&result));
        if (SUCCEEDED(hr) && result) result->GetStatus(&hr);
        ComPtr<IDxcBlobUtf8> errs;
        if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength())
            err.assign(errs->GetStringPointer(), errs->GetStringLength());
        if (FAILED(hr)) return false;
        ComPtr<IDxcBlob> obj;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj || obj->GetBufferSize() == 0) { err += "no object produced"; return false; }
        return true;
    }

private:
    bool spirv_ = false;
    void* dll_ = nullptr;
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
};

static const char* hlslType(GdBase b) {
    switch (b) {
    case GdBase::Bool: return "bool"; case GdBase::BVec2: return "bool2"; case GdBase::BVec3: return "bool3"; case GdBase::BVec4: return "bool4";
    case GdBase::Int: return "int"; case GdBase::IVec2: return "int2"; case GdBase::IVec3: return "int3"; case GdBase::IVec4: return "int4";
    case GdBase::UInt: return "uint"; case GdBase::UVec2: return "uint2"; case GdBase::UVec3: return "uint3"; case GdBase::UVec4: return "uint4";
    case GdBase::Float: return "float"; case GdBase::Vec2: return "float2"; case GdBase::Vec3: return "float3"; case GdBase::Vec4: return "float4";
    case GdBase::Mat2: return "float2x2"; case GdBase::Mat3: return "float3x3"; case GdBase::Mat4: return "float4x4";
    default: return "float";
    }
}

enum class Stage { Vertex, Pixel, Compute };

// The stub hook prelude: constant buffers for the three uniform blocks, a static IO struct for the varyings, and a static variable per hook.
static std::string hookPrelude(const GdHlslResult& r) {
    std::string s;
    s += "cbuffer GdU : register(b0) { float4 gdUbuf[" + std::to_string(std::max(1, r.uniforms.registerCount)) + "]; }\n#define AVER_CUSTOM_UBUF gdUbuf\n";
    s += "cbuffer GdI : register(b1) { float4 gdIbuf[" + std::to_string(std::max(1, r.instanceUniforms.registerCount)) + "]; }\n#define AVER_CUSTOM_IBUF gdIbuf\n";
    s += "cbuffer GdG : register(b2) { float4 gdGbuf[" + std::to_string(std::max(1, r.globalUniforms.registerCount)) + "]; }\n#define AVER_CUSTOM_GBUF gdGbuf\n";
    s += "struct VsOut {\n    float4 pos : SV_Position;\n" + r.varyingIo + "};\nstatic VsOut gdVary;\n#define AVER_CUSTOM_VARYINGS gdVary\n";
    for (const GdHookUse& h : r.hooks) {
        if (h.type == GdBase::SamplerCube) {
            s += "TextureCube<float4> gdhtex_" + h.macro + " : register(t40);\nSamplerState gdhsmp_" + h.macro + " : register(s14);\n";
            s += "EmbSamplerCube gdhsm_" + h.macro + "() { EmbSamplerCube r; r.t = gdhtex_" + h.macro + "; r.s = gdhsmp_" + h.macro + "; return r; }\n";
            s += "#define " + h.macro + " gdhsm_" + h.macro + "()\n";
        } else {
            s += std::string("static ") + hlslType(h.type) + " gdh_" + h.macro + ";\n#define " + h.macro + " gdh_" + h.macro + "\n";
        }
    }
    return s;
}

static bool hasHook(const GdHlslResult& r, const char* macro) {
    for (const GdHookUse& h : r.hooks) if (h.macro == macro) return true;
    return false;
}

static std::string entrySource(const GdShaderModule& m, const GdHlslResult& r, Stage st) {
    std::string s;
    auto has = [&](const char* e) { return std::find(r.entries.begin(), r.entries.end(), std::string("AverGd_") + e) != r.entries.end(); };
    if (st == Stage::Vertex) {
        s += "VsOut VSMain() {\n    AverGd_vertex();\n    gdVary.pos = float4(0.0, 0.0, 0.0, 1.0);\n    return gdVary;\n}\n";
    } else if (st == Stage::Pixel) {
        s += "float4 PSMain(VsOut i) : SV_Target {\n    gdVary = i;\n";
        if (has("fragment")) s += "    AverGd_fragment();\n";
        if (has("light")) s += "    AverGd_light();\n";
        if (has("sky")) s += "    AverGd_sky();\n";
        s += "    return float4(0.0, 0.0, 0.0, 1.0)";
        if (hasHook(r, "AVER_CUSTOM_ALBEDO")) s += " + float4(AVER_CUSTOM_ALBEDO, 0.0)";
        if (hasHook(r, "AVER_CUSTOM_COLOR") && m.shaderType == GdShaderType::Sky) s += " + float4(AVER_CUSTOM_COLOR, 0.0)";
        s += ";\n}\n";
    } else {
        s += "[numthreads(1, 1, 1)] void CSMain() {\n";
        for (const char* e : {"start", "process", "fog"}) if (has(e)) s += std::string("    AverGd_") + e + "();\n";
        s += "}\n";
    }
    return s;
}

struct StageBuild { GdHlslResult hlsl; std::string source; const char* entry; const char* target; std::string defines; bool valid = false; };

static std::vector<StageBuild> buildStages(const GdShaderModule& m, std::string& why) {
    std::vector<StageBuild> out;
    auto one = [&](u16 mask, Stage st, const char* entry, const char* target, bool noDeriv) {
        GdHlslOptions opt;
        opt.processorMask = mask;
        StageBuild b;
        b.hlsl = gdShaderEmitHlsl(m, opt);
        if (!b.hlsl.ok) { why += "emit failed: " + b.hlsl.error + "\n" + diagText(b.hlsl.diags); return; }
        b.source = std::string(noDeriv ? "#define AVER_CUSTOM_NO_DERIVATIVES\n" : "") + gdShaderHlslLibrary() + hookPrelude(b.hlsl) + b.hlsl.source + entrySource(m, b.hlsl, st);
        b.entry = entry;
        b.target = target;
        b.valid = true;
        out.push_back(std::move(b));
    };
    switch (m.shaderType) {
    case GdShaderType::Spatial:
        if (m.processor(GdProcessor::Vertex)) one(gdStageBit(GdProcessor::Vertex), Stage::Vertex, "VSMain", "vs_6_5", true);
        one(gdStageBit(GdProcessor::Fragment) | gdStageBit(GdProcessor::Light), Stage::Pixel, "PSMain", "ps_6_5", false);
        break;
    case GdShaderType::Sky: one(0xFFFF, Stage::Pixel, "PSMain", "ps_6_5", false); break;
    default: one(0xFFFF, Stage::Compute, "CSMain", "cs_6_5", true); break;
    }
    return out;
}

static std::string writeDump(const std::string& name, const std::string& text) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "AverGodotShaderTest";
    fs::create_directories(dir, ec);
    const fs::path p = dir / (name + ".hlsl");
    if (FILE* f = std::fopen(p.string().c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    return p.string();
}

struct Shader { const char* name; const char* src; };

// ------------------------------------------------------------------------------------------------ the representative shaders

static const Shader kShaders[] = {
{"water", R"(shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

group_uniforms colors;
uniform vec4 shallow_color : source_color = vec4(0.1, 0.5, 0.6, 0.6);
uniform vec4 deep_color : source_color = vec4(0.0, 0.1, 0.3, 0.9);
group_uniforms waves;
uniform sampler2D normal_a : hint_normal, repeat_enable, filter_linear_mipmap;
uniform sampler2D normal_b : hint_normal, repeat_enable;
uniform vec2 flow_a = vec2(0.03, 0.02);
uniform vec2 flow_b = vec2(-0.02, 0.04);
uniform float wave_height : hint_range(0.0, 2.0, 0.01) = 0.3;
uniform float wave_speed = 1.0;
uniform float depth_fade_distance : hint_range(0.1, 10.0) = 2.0;
group_uniforms;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
uniform sampler2D depth_tex : hint_depth_texture;

varying vec3 world_pos;
varying float wave;

float gerstner(vec2 p, float t) {
    return sin(p.x * 1.7 + t) * 0.5 + cos(p.y * 2.3 - t * 1.3) * 0.5;
}

void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    wave = gerstner(world_pos.xz, TIME * wave_speed);
    VERTEX.y += wave * wave_height;
}

void fragment() {
    vec2 uv_a = world_pos.xz * 0.1 + flow_a * TIME;
    vec2 uv_b = world_pos.xz * 0.13 + flow_b * TIME;
    vec3 n = mix(texture(normal_a, uv_a).rgb, texture(normal_b, uv_b).rgb, 0.5);
    NORMAL_MAP = n;
    float depth = texture(depth_tex, SCREEN_UV).x;
    vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, depth);
    vec4 view = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
    view.xyz /= view.w;
    float depth_diff = -view.z + VERTEX.z;
    float fade = clamp(depth_diff / depth_fade_distance, 0.0, 1.0);
    vec3 refracted = texture(screen_tex, SCREEN_UV + n.xy * 0.02).rgb;
    float fresnel = pow(1.0 - clamp(dot(NORMAL, VIEW), 0.0, 1.0), 4.0);
    vec4 col = mix(shallow_color, deep_color, fade);
    ALBEDO = mix(refracted, col.rgb, col.a * fade) + fresnel * 0.2;
    ROUGHNESS = 0.05;
    SPECULAR = 0.8;
    ALPHA = 1.0;
}
)"},
{"toon", R"(shader_type spatial;
render_mode diffuse_toon, specular_toon, cull_back;
uniform vec4 albedo : source_color = vec4(1.0);
uniform sampler2D albedo_tex : source_color, filter_nearest, repeat_enable;
uniform sampler2D ramp : filter_nearest;
uniform float steps : hint_range(1, 8) = 3.0;
uniform vec4 shadow_tint : source_color = vec4(0.2, 0.2, 0.4, 1.0);

void fragment() {
    ALBEDO = albedo.rgb * texture(albedo_tex, UV).rgb;
    ROUGHNESS = 1.0;
}

void light() {
    float ndl = dot(NORMAL, LIGHT) * 0.5 + 0.5;
    float band = floor(ndl * steps) / steps;
    vec3 lit = texture(ramp, vec2(band, 0.0)).rgb;
    DIFFUSE_LIGHT += mix(shadow_tint.rgb, vec3(1.0), lit) * LIGHT_COLOR * ATTENUATION * ALBEDO / PI;
    float spec = step(0.9, pow(max(dot(reflect(-LIGHT, NORMAL), VIEW), 0.0), 16.0));
    SPECULAR_LIGHT += spec * LIGHT_COLOR * ATTENUATION * SPECULAR_AMOUNT;
}
)"},
{"dissolve", R"(shader_type spatial;
render_mode cull_disabled;
uniform sampler2D albedo_tex : source_color;
uniform sampler2D noise_tex;
uniform float dissolve : hint_range(0.0, 1.0) = 0.0;
uniform float edge_width = 0.05;
uniform vec4 edge_color : source_color = vec4(1.0, 0.4, 0.0, 1.0);
uniform float edge_energy = 4.0;

void fragment() {
    float n = texture(noise_tex, UV * 2.0).r;
    if (n < dissolve) {
        discard;
    }
    float edge = 1.0 - smoothstep(0.0, edge_width, n - dissolve);
    ALBEDO = texture(albedo_tex, UV).rgb;
    EMISSION = edge_color.rgb * edge * edge_energy;
    ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)"},
{"triplanar", R"(shader_type spatial;
uniform sampler2D tex_top : source_color, repeat_enable;
uniform sampler2D tex_side : source_color, repeat_enable;
uniform float tex_scale = 0.25;
uniform float sharpness : hint_range(1.0, 16.0) = 4.0;
varying vec3 tri_pos;
varying vec3 tri_normal;

vec4 triplanar(sampler2D top, sampler2D side, vec3 p, vec3 n) {
    vec3 w = pow(abs(n), vec3(sharpness));
    w /= (w.x + w.y + w.z);
    vec4 x = texture(side, p.zy);
    vec4 y = texture(top, p.xz);
    vec4 z = texture(side, p.xy);
    return x * w.x + y * w.y + z * w.z;
}

void vertex() {
    tri_pos = VERTEX * tex_scale;
    tri_normal = NORMAL;
}

void fragment() {
    ALBEDO = triplanar(tex_top, tex_side, tri_pos, tri_normal).rgb;
}
)"},
{"wind", R"(shader_type spatial;
render_mode cull_disabled;
instance uniform float phase : hint_range(0.0, 6.28) = 0.0;
uniform float wind_strength = 0.2;
uniform vec3 wind_dir = vec3(1.0, 0.0, 0.3);
uniform float frequency = 2.0;
uniform sampler2D albedo_tex : source_color;
uniform sampler2D noise : repeat_enable;

void vertex() {
    float mask = COLOR.r;
    vec3 wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    float gust = texture(noise, wp.xz * 0.05 + vec2(TIME * 0.1)).r;
    float sway = sin(TIME * frequency + phase + wp.x * 0.5) * wind_strength * (0.5 + gust);
    VERTEX += normalize(wind_dir) * sway * mask * mask;
}

void fragment() {
    vec4 c = texture(albedo_tex, UV);
    ALBEDO = c.rgb;
    ALPHA = c.a;
    ALPHA_SCISSOR_THRESHOLD = 0.5;
    BACKLIGHT = vec3(0.2);
}
)"},
{"rim", R"(shader_type spatial;
uniform vec4 albedo : source_color = vec4(0.8);
uniform vec4 rim_color : source_color = vec4(0.3, 0.7, 1.0, 1.0);
uniform float rim_power : hint_range(0.5, 8.0) = 3.0;
uniform float rim_strength : hint_range(0.0, 4.0) = 1.5;
uniform sampler2D normal_map : hint_normal;

void fragment() {
    ALBEDO = albedo.rgb;
    NORMAL_MAP = texture(normal_map, UV).rgb;
    float f = pow(1.0 - clamp(dot(normalize(NORMAL), normalize(VIEW)), 0.0, 1.0), rim_power);
    EMISSION = rim_color.rgb * f * rim_strength;
    METALLIC = 0.0;
    ROUGHNESS = 0.4;
}
)"},
{"torture", R"(shader_type spatial;
render_mode unshaded;

struct Light { vec3 color; float power; float taps[2]; };
struct Pair { Light a; ivec2 idx; };

const float K = 2.5;
const int N = 4;
const vec3 PTS[3] = vec3[3](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
const mat2 R = mat2(vec2(0.0, 1.0), vec2(-1.0, 0.0));
const float COEF[3] = float[3](0.2, 0.3, 0.5);

uniform vec4 tint : source_color = vec4(1.0);
uniform float weights[4];
uniform ivec2 grid = ivec2(4, 4);
uniform mat3 xform;
uniform int mode : hint_enum("A", "B", "C") = 0;
uniform bool flip = false;
uniform uint seed = 7u;
uniform sampler3D vol;
uniform samplerCube env;
uniform sampler2DArray layers;
uniform usampler2D ids;
varying flat int vid;
varying smooth vec2 vuv;

float sq(float x) { return x * x; }
int sq(int x) { return x * x; }
vec3 sq(vec3 x) { return x * x; }

void split(in vec3 v, out float len, inout vec3 dir) {
    len = length(v);
    dir = normalize(v + dir);
}

float sum3(float a[3]) {
    return a[0] + a[1] + a[2];
}

float apply(Light l, int i) {
    return l.color.x * l.power + l.taps[i % 2];
}

vec3 mat_ops(vec3 v) {
    mat3 m = xform * mat3(1.0);
    mat3 t = transpose(m) * inverse(m);
    vec3 a = m * v;
    vec3 b = v * m;
    mat4 big = mat4(m);
    big[3] = vec4(v, 1.0);
    return a + b + (big * vec4(v, 1.0)).xyz + t[1];
}

int bits(int a, uint b) {
    int r = (a << 2) | (a >> 1) & 3 ^ 5;
    r += int(b & 0xFu) + bitCount(b);
    return r;
}

float loops(int n) {
    float acc = 0.0;
    for (int i = 0; i < n; i++) {
        if (i == 2) continue;
        if (i > 6) break;
        acc += weights[i % 4];
    }
    for (int a = 0, b = 10; a < b; a++, b--) {
        acc += 0.0;
    }
    int j = 0;
    while (j < 3) { acc += 1.0; j++; }
    do { acc -= 0.5; j--; } while (j > 0);
    switch (mode) {
        case 0: acc += 1.0; break;
        case 1:
        case 2: acc += 2.0; break;
        default: acc = 0.0;
    }
    return acc;
}

void vertex() {
    vid = INSTANCE_ID;
    vuv = UV;
}

void fragment() {
    float tp[2] = float[2](0.1, 0.2);
    Light l = Light(vec3(1.0, 0.5, 0.25), 2.0, tp);
    Pair p = Pair(l, ivec2(1, 2));
    float tp3[3] = float[3](1.0, 2.0, 3.0);
    float un[] = float[](4.0, 5.0);
    float s = sum3(tp3) + COEF[int(un[1]) % 3] + sq(K) + float(sq(N)) + sq(vec3(1.0)).x;
    float len;
    vec3 dir = vec3(0.0, 1.0, 0.0);
    split(vec3(1.0, 2.0, 3.0), len, dir);
    vec3 col = mat_ops(dir) * (flip ? 1.0 : 0.5) + PTS[N % 3];
    col.rgb *= tint.rgb;
    col.xy = R * col.xy;
    float v1 = texture(vol, vec3(UV, 0.5)).r;
    vec3 v2 = textureLod(env, NORMAL, 2.0).rgb;
    vec4 v3 = texture(layers, vec3(UV, 1.0));
    uvec4 v4 = texelFetch(ids, ivec2(UV * vec2(grid)), 0);
    ivec3 sz = textureSize(vol, 0);
    s += loops(bits(p.idx.x, seed)) + apply(l, 1) + v1 + float(v4.x) + float(sz.z) + len;
    bvec3 lt = lessThan(col, vec3(0.5));
    if (any(lt) && !all(lt) || false) {
        col = mix(col, vec3(1.0), lt);
    }
    if (col == vec3(0.0) || xform != mat3(1.0)) { col += 1.0; }
    ALBEDO = col * s + v2 + v3.rgb + float(vid) * 0.0 + vec3(vuv, 0.0);
}
)"},
{"particles", R"(shader_type particles;
uniform float spread : hint_range(0.0, 1.0) = 0.3;
uniform vec3 gravity = vec3(0.0, -9.8, 0.0);
uniform float life_scale = 1.0;

float rand(uint seed) {
    uint x = seed * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return float((x >> 22u) ^ x) / 4294967295.0;
}

void start() {
    uint seed = RANDOM_SEED + INDEX;
    float a = rand(seed);
    VELOCITY = vec3(cos(a * TAU), 1.0, sin(a * TAU)) * spread * 5.0;
    TRANSFORM[3].xyz = EMISSION_TRANSFORM[3].xyz;
    COLOR = vec4(a, 0.5, 1.0 - a, 1.0);
    CUSTOM.x = 0.0;
}

void process() {
    VELOCITY += gravity * DELTA;
    CUSTOM.x += DELTA / (LIFETIME * life_scale);
    TRANSFORM[3].xyz += VELOCITY * DELTA;
    if (CUSTOM.x > 1.0) { ACTIVE = false; }
    if (COLLIDED) { VELOCITY = reflect(VELOCITY, COLLISION_NORMAL) * 0.5; }
}
)"},
{"sky", R"(shader_type sky;
uniform vec3 top_color : source_color = vec3(0.1, 0.3, 0.8);
uniform vec3 horizon_color : source_color = vec3(0.7, 0.8, 0.9);
uniform sampler2D clouds : repeat_enable;

void sky() {
    float h = clamp(EYEDIR.y, 0.0, 1.0);
    vec3 c = mix(horizon_color, top_color, pow(h, 0.5));
    if (LIGHT0_ENABLED) {
        float d = max(dot(EYEDIR, LIGHT0_DIRECTION), 0.0);
        c += LIGHT0_COLOR * LIGHT0_ENERGY * pow(d, 256.0);
    }
    c += texture(clouds, EYEDIR.xz / max(EYEDIR.y, 0.05) * 0.1 + TIME * 0.01).rgb * 0.2;
    COLOR = c;
    vec3 r = textureLod(RADIANCE, EYEDIR, 3.0).rgb * 0.0;
    COLOR += r;
}
)"},
{"fog", R"(shader_type fog;
uniform vec3 fog_color : source_color = vec3(0.6, 0.7, 0.8);
uniform float density = 0.1;
uniform sampler3D noise : repeat_enable;

void fog() {
    float n = texture(noise, WORLD_POSITION * 0.05 + vec3(TIME * 0.02)).r;
    DENSITY = density * n * (1.0 - clamp(length(OBJECT_POSITION) / SIZE.x, 0.0, 1.0));
    ALBEDO = fog_color;
    EMISSION = vec3(0.0);
}
)"},
{"varyings", R"(shader_type spatial;
varying vec3 vbuf[2];
varying mat3 vm;
varying flat uint vflag;
varying vec4 vcol;

void kill(float a) {
    if (a < 0.0) {
        discard;
    }
}

void vertex() {
    vbuf[0] = VERTEX;
    vbuf[1] = NORMAL;
    vm = mat3(MODEL_NORMAL_MATRIX);
    vflag = uint(VERTEX_ID);
    vcol = COLOR;
}

void fragment() {
    kill(vcol.a - 0.1);
    ALBEDO = vbuf[0] * vm * vbuf[1] + vcol.rgb * float(vflag);
}
)"},
{"builtins", R"(shader_type spatial;
uniform sampler2D s2;
uniform isampler2D is2;
uniform usampler2D us2;
uniform sampler2DArray s2a;
uniform isampler2DArray is2a;
uniform sampler3D s3;
uniform samplerCube sc;
uniform samplerCubeArray sca;

void fragment() {
    float f = 0.5;
    vec2 v2 = vec2(0.3, 0.6);
    vec3 v3 = vec3(0.1, 0.2, 0.3);
    vec4 v4 = vec4(0.1, 0.2, 0.3, 0.4);
    ivec2 i2 = ivec2(1, 2);
    ivec3 i3 = ivec3(1, 2, 3);
    int i = 3;
    uint u = 5u;
    uvec2 u2 = uvec2(1u, 2u);

    vec3 a = radians(v3) + degrees(v3) + sin(v3) + cos(v3) + tan(v3) + asin(v3) + acos(v3) + atan(v3) + atan(v3, v3 + 1.0) + sinh(v3) + cosh(v3) + tanh(v3) + asinh(v3) + acosh(v3 + 1.0) + atanh(v3);
    a += pow(v3, v3) + exp(v3) + exp2(v3) + log(v3) + log2(v3) + sqrt(v3) + inversesqrt(v3);
    a += abs(v3) + sign(v3) + floor(v3) + trunc(v3) + round(v3) + roundEven(v3) + ceil(v3) + fract(v3) + mod(v3, v3) + mod(v3, 0.3);
    vec3 ip;
    a += modf(v3, ip) + ip;
    a += min(v3, v3) + max(v3, 0.1) + clamp(v3, 0.0, 1.0) + clamp(v3, v3, v3) + mix(v3, v3, 0.3) + mix(v3, v3, v3) + fma(v3, v3, v3);
    a += step(0.5, v3) + step(v3, v3) + smoothstep(0.0, 1.0, v3) + smoothstep(v3, v3 + 1.0, v3);
    int ai = abs(i) + sign(i) + min(i, 2) + max(i, 2) + clamp(i, 0, 5);
    ivec3 ai3 = abs(i3) + sign(i3) + min(i3, i3) + clamp(i3, ivec3(0), ivec3(9));
    uint au = min(u, 3u) + max(u, 3u) + clamp(u, 1u, 9u);
    bool b1 = isinf(f) || isnan(f);
    bvec3 b3 = isinf(v3);
    ivec3 bi = floatBitsToInt(v3);
    uvec3 bu = floatBitsToUint(v3);
    vec3 bf = intBitsToFloat(bi) + uintBitsToFloat(bu);
    float len = length(v3) + distance(v3, v3) + dot(v3, v3) + length(f);
    vec3 g = cross(v3, v3) + normalize(v3) + reflect(v3, v3) + refract(v3, v3, 0.7) + faceforward(v3, v3, v3);

    mat2 m2 = mat2(1.0);
    mat3 m3 = mat3(1.0);
    mat4 m4 = mat4(1.0);
    m2 = matrixCompMult(m2, m2) * inverse(m2) * transpose(m2);
    m3 = outerProduct(v3, v3) + transpose(m3) * inverse(m3);
    m4 = inverse(m4) * transpose(m4) + outerProduct(v4, v4);
    float det = determinant(m2) + determinant(m3) + determinant(m4);
    mat3 m3b = mat3(m2);
    mat4 m4b = mat4(m2) * mat4(m3);
    mat2 m2b = mat2(m3);

    bool cmp = any(lessThan(v3, v3)) || all(greaterThan(i3, i3)) || any(lessThanEqual(u2, u2)) || any(greaterThanEqual(v2, v2)) || any(equal(v3, v3)) || any(notEqual(i3, i3)) || any(not(b3)) || all(equal(b3, b3));
    vec3 d = dFdx(v3) + dFdy(v3) + fwidth(v3) + dFdxCoarse(v3) + dFdyCoarse(v3) + dFdxFine(v3) + dFdyFine(v3) + fwidthCoarse(v3) + fwidthFine(v3);

    uint p1 = packHalf2x16(v2) + packUnorm2x16(v2) + packSnorm2x16(v2) + packUnorm4x8(v4) + packSnorm4x8(v4);
    vec2 up = unpackHalf2x16(p1) + unpackUnorm2x16(p1) + unpackSnorm2x16(p1);
    vec4 up4 = unpackUnorm4x8(p1) + unpackSnorm4x8(p1);
    int bb = bitfieldExtract(i, 2, 3) + bitCount(i) + bitCount(u) + findLSB(i) + findLSB(u) + findMSB(i) + findMSB(u) + bitfieldReverse(i);
    uint bb2 = bitfieldInsert(u, u, 1, 2) + bitfieldExtract(u, 1, 2) + bitfieldReverse(u);

    vec4 t = texture(s2, v2) + texture(s2, v2, 1.0) + textureLod(s2, v2, 2.0) + textureGrad(s2, v2, v2, v2) + textureProj(s2, v3) + textureProj(s2, v4);
    t += textureProjLod(s2, v3, 1.0) + textureGather(s2, v2) + textureGather(s2, v2, 1) + texelFetch(s2, i2, 0);
    t += texture(s2a, v3) + textureLod(s2a, v3, 1.0) + texelFetch(s2a, i3, 0) + textureGather(s2a, v3) + textureGrad(s2a, v3, v2, v2);
    t += texture(s3, v3) + textureLod(s3, v3, 1.0) + textureProj(s3, v4) + texelFetch(s3, i3, 0) + textureGrad(s3, v3, v3, v3);
    t += texture(sc, v3) + textureLod(sc, v3, 1.0) + textureGather(sc, v3) + textureGrad(sc, v3, v3, v3);
    t += texture(sca, v4) + textureLod(sca, v4, 1.0);
    ivec4 ti = texture(is2, v2) + texelFetch(is2, i2, 0) + textureLod(is2, v2, 0.0) + textureGather(is2, v2) + texelFetch(is2a, i3, 0) + texture(is2a, v3);
    uvec4 tu = texelFetch(us2, i2, 0) + texture(us2, v2);
    ivec2 ts = textureSize(s2, 0) + textureSize(sc, 0);
    ivec3 ts3 = textureSize(s3, 0) + textureSize(s2a, 0) + textureSize(sca, 0);
    int lv = textureQueryLevels(s2) + textureQueryLevels(s3) + textureQueryLevels(sc);
    vec2 ql = textureQueryLod(s2, v2) + textureQueryLod(s2a, v2) + textureQueryLod(s3, v3) + textureQueryLod(sc, v3) + textureQueryLod(sca, v3);

    float sum = len + det + float(ai) + float(ai3.x) + float(au) + float(bb) + float(bb2) + float(cmp) + float(b1) + float(lv) + m3b[0][0] + m4b[1][1] + m2b[0][0] + float(ts.x + ts3.z);
    ALBEDO = a + bf + g + d + vec3(sum) + t.rgb + vec3(ti.rgb) + vec3(tu.rgb) + vec3(up, up4.x) + vec3(ql, 0.0);
}
)"},
};

static void testHlsl() {
    AVER_INFO("=== HLSL emission and DXC (DXIL + SPIR-V) ===");
    Dxc dxil, spirv;
    const bool haveDxil = dxil.load(fs::path(executableDir()) / "dxcompiler.dll", false);
#if defined(_WIN32)
    const fs::path spirvDll = fs::path(AVER_REPO_ROOT) / "third_party" / "dxc-spirv" / "dxcompiler.dll";
#elif defined(__APPLE__)
    const fs::path spirvDll = fs::path(executableDir()) / "libdxcompiler.dylib";
#else
    const fs::path spirvDll = fs::path(executableDir()) / "libdxcompiler.so";
#endif
    const bool haveSpirv = spirv.load(spirvDll, true);
    if (!haveDxil) AVER_WARN("  SKIP  no DXIL-capable dxcompiler beside the test: the DXIL half is NOT verified");
    if (!haveSpirv) AVER_WARN("  SKIP  no SPIR-V-capable dxcompiler (third_party/dxc-spirv): the SPIR-V half is NOT verified");

    for (const Shader& sh : kShaders) {
        GdShaderModule m = compile(sh.src);
        if (!m.ok) AVER_ERROR("{}", diagText(m.diags));
        check(m.ok, std::string(sh.name) + ": the Godot shader compiles");
        if (!m.ok) continue;
        std::string why;
        const std::vector<StageBuild> stages = buildStages(m, why);
        check(!stages.empty() && why.empty(), std::string(sh.name) + ": HLSL emitted" + (why.empty() ? "" : " -- " + why));
        for (const StageBuild& b : stages) {
            std::string err;
            if (std::getenv("AVER_CUSTOM_DUMP")) writeDump(std::string(sh.name) + "_" + b.target, b.source);   // inspect what was compiled
            if (haveDxil) {
                const bool ok = dxil.compile(b.source, b.entry, b.target, "", err);
                if (!ok) AVER_ERROR("{}\n--- dump: {}", err, writeDump(std::string(sh.name) + "_" + b.target + "_dxil", b.source));
                check(ok, std::string(sh.name) + ": DXC compiles " + b.target + " to DXIL");
            }
            if (haveSpirv) {
                const bool ok = spirv.compile(b.source, b.entry, b.target, "", err);
                if (!ok) AVER_ERROR("{}\n--- dump: {}", err, writeDump(std::string(sh.name) + "_" + b.target + "_spv", b.source));
                check(ok, std::string(sh.name) + ": DXC compiles " + b.target + " to SPIR-V");
            }
        }
    }
}

// Broken and hostile input must produce diagnostics, never a crash, a hang or a stack overflow.
static void testRobustness() {
    AVER_INFO("=== robustness ===");
    int ran = 0, okCount = 0, silentFails = 0;
    auto run = [&](const std::string& src) {
        if (std::getenv("AVER_CUSTOM_TRACE")) { std::fprintf(stderr, "TRACE run %d len %zu head '%.60s'\n", ran, src.size(), src.c_str()); std::fflush(stderr); }
        GdShaderModule m = compile(src);
        ++ran;
        if (m.ok) {
            ++okCount;
            (void)gdShaderEmitHlsl(m);
        } else if (m.errorCount() == 0) {
            ++silentFails;
        }
        return m.ok;
    };
    for (const Shader& sh : kShaders) {
        const std::string src = sh.src;
        const usize step = std::max<usize>(1, src.size() / 60);
        for (usize cut = 0; cut < src.size(); cut += step) run(src.substr(0, cut));               // truncations
        for (usize at = 0; at < src.size(); at += step * 2) run(src.substr(0, at) + src.substr(at + 1));   // one character deleted
        for (usize at = 3; at < src.size(); at += step * 3) { std::string s = src; s[at] = '('; run(s); s[at] = '}'; run(s); s[at] = '"'; run(s); }
    }
    check(ran > 1000, "mutated shaders ran: " + std::to_string(ran));

    const std::string head = "shader_type spatial;\n";
    run("");
    run("\n\n   \t\n");
    run(std::string("\xEF\xBB\xBF") + head + "void fragment() {}\n");
    check(compile(std::string("\xEF\xBB\xBF") + head + "void fragment() {}\n").ok, "a UTF-8 BOM at the start is skipped");
    {
        std::string nul = head + "void fragment() { float f = 1.0; ";
        nul.push_back('\0');
        run(nul + " }");
    }
    run(head + "void fragment() { /* never closed\n");
    run(head + "uniform int e : hint_enum(\"never closed);\n");
    run(head + "const int a = 99999999999999999999999999999;\n");
    run(head + "const int a = 0xFFFFFFFFFFFFFFFFFFFF;\n");
    run(head + "const float a = 1e999;\n");
    run(head + "const int a = " + std::string(100000, '(') + "1" + std::string(100000, ')') + ";\n");
    run(head + "const int a = " + std::string(100000, '-') + "1;\n");
    {
        std::string chain = head + "const int a = 1";
        for (int i = 0; i < 100000; ++i) chain += "+1";
        run(chain + ";\n");
    }
    {
        std::string post = head + "void fragment() { vec4 v; float f = v";
        for (int i = 0; i < 100000; ++i) post += ".xyzw.x";
        run(post + "; }\n");
        std::string comma = head + "void fragment() { float f = 1.0; f = 1.0";
        for (int i = 0; i < 100000; ++i) comma += ", 1.0";
        run(comma + "; }\n");
        std::string idx = head + "void fragment() { mat4 m; float f = m";
        for (int i = 0; i < 100000; ++i) idx += "[0]";
        run(idx + "; }\n");
    }
    {
        std::string blocks = head + "void fragment() ";
        for (int i = 0; i < 50000; ++i) blocks += "{";
        run(blocks);
    }
    {
        std::string ternary = head + "const int a = ";
        for (int i = 0; i < 50000; ++i) ternary += "1 ? 1 : ";
        run(ternary + "1;\n");
    }
    run("#define A B B\n#define B C C\n#define C D D\n#define D E E\n#define E F F\n#define F G G\n#define G H H\n#define H I I\n#define I J J\n#define J K K\n#define K L L\n#define L M M\n#define M N N\n#define N O O\n#define O P P\n#define P Q Q\n#define Q R R\n#define R S S\n#define S T T\n#define T U U\n#define U 1\nA\n");
    run("#if " + std::string(100000, '(') + "1" + std::string(100000, ')') + "\n#endif\n");
    run("#if " + std::string(50000, '!') + "1\n#endif\n");
    run("#define F(a) F(a) a\nF(F(F(1)))\n");
    run("#include <>\n#include\n#define\n#define 1\n#undef\n#if\n#elif\n#else\n#endif\n#pragma\n#error\n");
    check(silentFails == 0, "every failed compile carried an error diagnostic (" + std::to_string(okCount) + " of " + std::to_string(ran) + " compiled)");
}

// Hostile-input budgets, macro/number lexing and library formulas that earlier reviews found wrong.
static void testHostileAndMacros() {
    AVER_INFO("=== hostile input budgets and macro lexing ===");
    GdPreprocessOptions plain;

    // a numeric literal's suffix is not an identifier
    {
        const std::string out = pre("#define PACK(x, u) ((x) * 255u + (u))\nuint r = PACK(a, b);", plain);
        check(squeeze(out) == "uintr=((a)*255u+(b));", "a macro parameter named like a literal suffix does not eat 255u: " + out);
        const std::string f = pre("#define SC(x, f) ((x) * 2.0f + (f))\nfloat r = SC(a, b);", plain);
        check(squeeze(f) == "floatr=((a)*2.0f+(b));", "a parameter named f does not eat 2.0f: " + f);
        const std::string e = pre("#define K(x, e) ((x) * 1e5 + (e))\nfloat r = K(a, b);", plain);
        check(squeeze(e) == "floatr=((a)*1e5+(b));", "a parameter named e does not eat 1e5: " + e);
    }

    // exponential #include expansion stops at the file budget
    {
        GdPreprocessOptions o;
        o.maxIncludeDepth = 17;
        o.resolver = [](const std::string&, const std::string&, std::string& text, std::string& resolved) {
            text = "#include \"a\"\n#include \"a\"\nint x;\n";
            resolved = "a";
            return true;
        };
        std::vector<GdShaderDiag> d;
        GdPreprocessed out;
        const bool ok = gdShaderPreprocess("#include \"a\"\n", "main.gdshader", o, out, d);
        check(!ok && !d.empty() && out.files.size() <= 65535 && d.size() <= 1100, "an include bomb (depth 17) fails with a diagnostic and bounded files/diagnostics");
        bool tooMany = false;
        for (const GdShaderDiag& x : d) tooMany = tooMany || x.message.find("too many") != std::string::npos;
        check(tooMany, "the include budget names itself");
    }

    // macro output is bounded in total, not only per line
    {
        std::string src;
        src += "#define L0 x\n";
        for (int i = 1; i <= 18; ++i) src += "#define L" + std::to_string(i) + " L" + std::to_string(i - 1) + " L" + std::to_string(i - 1) + "\n";
        for (int i = 0; i < 400; ++i) src += "L18\n";
        GdPreprocessed out;
        std::vector<GdShaderDiag> d;
        const bool ok = gdShaderPreprocess(src, "main.gdshader", plain, out, d);
        check(!ok && out.text.size() <= (usize{1} << 25) + (usize{1} << 21), "a macro bomb repeated on many lines stops at the output budget (" + std::to_string(out.text.size()) + " bytes)");
    }

    // nested left-deep chains: each parenthesis level restarts the chain counter, the tree height is bounded instead
    {
        auto chainOf = [](const std::string& inner, int terms, const char* lit) {
            std::string s = "(" + inner + ")";
            for (int i = 0; i < terms; ++i) s += std::string("+") + lit;
            return s;
        };
        std::string e = "1";
        for (int lvl = 0; lvl < 47; ++lvl) e = chainOf(e, 300, "1");
        GdShaderModule m = compile("shader_type spatial;\nconst int a = " + e + ";\n");
        check(!m.ok && m.errorCount() > 0, "a 47x300 nested chain in a constant is rejected with a diagnostic");
        std::string f = "1.0";
        for (int lvl = 0; lvl < 47; ++lvl) f = chainOf(f, 300, "1.0");
        GdShaderModule m2 = compile("shader_type spatial;\nvoid fragment() { float f = " + f + "; }\n");
        check(!m2.ok && m2.errorCount() > 0, "a 47x300 nested chain in a function body is rejected before the emitter sees it");
        bool named = false;
        for (const GdShaderDiag& x : m2.diags) named = named || x.message.find("too deep") != std::string::npos;
        check(named, "the height limit is named");

        std::string g = "1.0";
        g = chainOf(g, 70, "1.0");
        g = chainOf(g, 50, "1.0");
        GdShaderModule m3 = compile("shader_type spatial;\nvoid fragment() { float f = " + g + "; ALBEDO = vec3(f); }\n");
        check(m3.ok, "a 120-deep chain still compiles: " + diagText(m3.diags));
        if (m3.ok) check(gdShaderEmitHlsl(m3).ok, "and still emits");
        std::string h = "1.0";
        h = chainOf(h, 70, "1.0");
        h = chainOf(h, 70, "1.0");
        GdShaderModule m4 = compile("shader_type spatial;\nvoid fragment() { float f = " + h + "; ALBEDO = vec3(f); }\n");
        check(!m4.ok && m4.errorCount() > 0, "a 140-deep chain is over the height limit");
    }

    // asinh must stay finite and odd for large negative arguments
    {
        const std::string lib = gdShaderHlslLibrary();
        const usize at = lib.find("float emb_asinh(float x)");
        const std::string body = at == std::string::npos ? std::string() : lib.substr(at, lib.find('}', at) - at);
        check(body.find("abs(x)") != std::string::npos && body.find("sign(x)") != std::string::npos, "emb_asinh works on |x| and restores the sign: " + body);
    }

    // the same include bomb at the default depth (2^25 expansions without a budget); last because it is the slow one when the budget is missing
    {
        GdPreprocessOptions o;
        o.resolver = [](const std::string&, const std::string&, std::string& text, std::string& resolved) {
            text = "#include \"a\"\n#include \"a\"\nint x;\n";
            resolved = "a";
            return true;
        };
        std::vector<GdShaderDiag> d;
        GdPreprocessed out;
        const bool ok = gdShaderPreprocess("#include \"a\"\n", "main.gdshader", o, out, d);
        check(!ok && out.files.size() <= 4096 && out.lines.size() < 100000, "an include bomb at the default depth ends quickly with bounded output");
    }
}

// What the emitter writes, spot-checked on small shaders (no DXC).
static void testEmittedText() {
    AVER_INFO("=== emitted HLSL spot checks ===");
    auto emit = [](const std::string& body, std::string pre = "") {
        GdShaderModule m = compile("shader_type spatial;\n" + pre + "void fragment() {\n" + body + "\n}\n");
        if (!m.ok) AVER_ERROR("{}", diagText(m.diags));
        return gdShaderEmitHlsl(m);
    };
    auto has = [](const GdHlslResult& r, const std::string& s) { return r.source.find(s) != std::string::npos; };

    {
        const GdHlslResult r = emit("mat3 a = mat3(1.0); mat3 b = a * a; vec3 v = a * vec3(1.0); vec3 w = vec3(1.0) * a; float s = (a * 2.0)[0][0];");
        check(r.ok && has(r, "mul(l_a, l_a)"), "mat * mat is mul(rhs, lhs)");
        check(has(r, "mul(((float3)(1.0)), l_a)"), "mat * vec is mul(vec, mat)");
        check(has(r, "mul(l_a, ((float3)(1.0)))"), "vec * mat is mul(mat, vec)");
        check(has(r, "emb_mat3_diag("), "mat3(float) is the diagonal helper");
    }
    {
        const GdHlslResult r = emit("vec3 a = vec3(1.0); bool e = a == vec3(0.0); bool n = a != vec3(0.0); float f = 1.0; bool z = f == 1.0;");
        check(has(r, "all(l_a == ") && has(r, "any(l_a != "), "vector == and != reduce with all() / any()");
        check(has(r, "(l_f == 1.0)"), "scalar == stays a plain comparison");
    }
    {
        const GdHlslResult r = emit("ALBEDO = vec3(1.0); float t = TIME + PI;");
        bool albedoWritten = false, timeRead = false;
        for (const GdHookUse& h : r.hooks) {
            if (h.macro == "AVER_CUSTOM_ALBEDO" && h.written && !h.read) albedoWritten = true;
            if (h.macro == "AVER_CUSTOM_TIME" && h.read && !h.written) timeRead = true;
        }
        check(albedoWritten && timeRead, "hooks are listed with read / write use");
        check(!hasHook(r, "AVER_CUSTOM_PI") && has(r, "3.14159265358979"), "PI is a literal, not a hook");
    }
    {
        const GdHlslResult r = emit("float x = fract(inversesqrt(mod(1.5, 1.0))) + sign(0.3) + mix(0.0, 1.0, 0.5) + float(atan(1.0, 2.0) > 0.0);");
        check(has(r, "frac(rsqrt(emb_mod(") && has(r, "emb_sign(") && has(r, "emb_mix(") && has(r, "atan2("), "GLSL names map to HLSL intrinsics and helpers");
    }
    {
        const GdHlslResult r = emit("float f = 0.0; int i = 2; uint u = 3u; bool b = true; f = max(f, 1.0); vec2 v = max(vec2(1.0), 0.5);");
        check(has(r, "max(l_f, 1.0)"), "max(float, float) is emitted directly");
        check(has(r, "((float2)(0.5))"), "a scalar argument of max(vec2, float) is splatted");
    }
    {
        // identifiers cannot collide with HLSL names: a variable called "frac", "float2" or "sample" is legal in Godot
        const GdHlslResult r = emit("float frac = 1.0; float float2 = 2.0; float sample = frac + float2;");
        check(r.ok && has(r, "l_frac") && has(r, "l_float2") && has(r, "l_sample"), "user identifiers are prefixed so HLSL names cannot collide");
    }
    {
        const GdHlslResult r = emit("float a[2] = float[2](1.0, 2.0); float b = a[1] + float(a.length());");
        if (!r.ok || !has(r, "float l_a[2] = { 1.0, 2.0 };")) AVER_ERROR("{}\n{}", r.error, r.source);
        check(r.ok && has(r, "float l_a[2] = { 1.0, 2.0 };") && has(r, "l_a[1]"), "an array declaration with a constructor becomes an initialiser list");
    }
    {
        // unsupported constructs are reported, not emitted wrongly
        GdShaderModule m = compile("shader_type spatial;\nfloat[2] two() { return float[2](1.0, 2.0); }\nvoid fragment() { float f = two()[0]; }\n");
        const GdHlslResult r = gdShaderEmitHlsl(m);
        check(m.ok && !r.ok && r.error.find("array") != std::string::npos, "an array-returning function is reported unsupported by the HLSL emitter");
        GdShaderModule m2 = compile("shader_type spatial;\nvoid fragment() { float f = 0.0; f = float[2](1.0, 2.0)[1]; }\n");
        const GdHlslResult r2 = gdShaderEmitHlsl(m2);
        check(m2.ok && !r2.ok, "an array constructor outside an initialiser is reported unsupported");
    }
    {
        GdShaderModule m = compile("shader_type spatial;\nuniform sampler2D t : repeat_enable;\nuniform sampler2D u : filter_nearest;\nuniform sampler2D w : repeat_enable;\nvarying flat int a;\nvarying vec3 b[2];\nvoid vertex() { a = 1; b[0] = vec3(1.0); b[1] = b[0]; }\nvoid fragment() { ALBEDO = texture(t, UV).rgb + texture(u, UV).rgb + texture(w, UV).rgb; }\n");
        GdHlslOptions o;
        o.varyingSemanticBase = 3;
        o.textureRegisterBase = 5;
        o.samplerRegisterBase = 2;
        const GdHlslResult r = gdShaderEmitHlsl(m, o);
        check(r.ok && r.textures.size() == 3 && r.textures[0].reg == 5 && r.textures[2].reg == 7, "texture registers start at the base");
        check(r.samplers.size() == 2 && r.textures[0].sampler == r.textures[2].sampler && r.samplers[0].reg == 2 && r.samplers[1].reg == 3, "equal filter / repeat pairs share a sampler state");
        check(r.varyingIo.find("nointerpolation int gdv_a : TEXCOORD3;") != std::string::npos && r.varyingIo.find("float3 gdv_b[2] : TEXCOORD4;") != std::string::npos,
              "varying IO members: flat -> nointerpolation, semantics advance by array size");
        check(r.entries.size() == 2 && r.entries[0] == "AverGd_vertex" && r.entries[1] == "AverGd_fragment", "entry function names");
        GdHlslOptions vOnly;
        vOnly.processorMask = gdStageBit(GdProcessor::Vertex);
        const GdHlslResult rv = gdShaderEmitHlsl(m, vOnly);
        bool anyAlbedo = false;
        for (const GdHookUse& h : rv.hooks) anyAlbedo = anyAlbedo || h.macro == "AVER_CUSTOM_ALBEDO";
        check(rv.ok && rv.entries.size() == 1 && !anyAlbedo, "a vertex-only emit has no fragment hooks");
    }
    check(std::string(gdShaderHlslLibrary()).find("emb_inverse") != std::string::npos, "the helper library is available");
}

// `GodotShaderTest <file.gdshader>` compiles one file, prints its diagnostics and the HLSL (a debugging aid; the test itself takes no arguments).
static int compileOne(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::string src;
    char buf[4096];
    for (usize n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) src.append(buf, n);
    std::fclose(f);
    GdShaderModule m = compile(src);
    std::printf("%s", diagText(m.diags).c_str());
    if (m.ok) {
        const GdHlslResult r = gdShaderEmitHlsl(m);
        std::printf("%s\n", r.ok ? r.source.c_str() : ("emit failed: " + r.error).c_str());
    }
    return m.ok ? 0 : 1;
}

// `GodotShaderTest --hooks` prints the built-in variable table as Markdown rows (the source of the table in G6_SHADERS.md).
static int printHooks() {
    const GdShaderType types[] = {GdShaderType::Spatial, GdShaderType::Particles, GdShaderType::Sky, GdShaderType::Fog, GdShaderType::CanvasItem};
    for (GdShaderType t : types) {
        int n = 0;
        const GdBuiltinVar* v = gdBuiltinVars(t, n);
        std::printf("\n#### %s\n\n| Built-in | Type | Access | Where | Hook macro |\n|---|---|---|---|---|\n", gdShaderTypeName(t));
        for (int i = 0; i < n; ++i) {
            std::string where;
            if (v[i].global) where = "everywhere";
            else {
                for (GdProcessor p : {GdProcessor::Vertex, GdProcessor::Fragment, GdProcessor::Light, GdProcessor::Start, GdProcessor::Process, GdProcessor::Sky, GdProcessor::Fog})
                    if (v[i].stages & gdStageBit(p)) where += (where.empty() ? "" : ", ") + std::string(gdProcessorName(p));
            }
            const std::string name = v[i].name;
            const bool lit = name == "PI" || name == "TAU" || name == "E";
            std::printf("| `%s` | %s | %s | %s | %s |\n", v[i].name, gdBaseName(v[i].type), v[i].access == GdAccess::In ? "in" : v[i].access == GdAccess::Out ? "out" : "inout",
                        where.c_str(), lit ? "(literal)" : ("`AVER_CUSTOM_" + name + "`").c_str());
        }
    }
    return 0;
}

// `GodotShaderTest --functions` prints the built-in function table (name, overload count, HLSL spelling).
static int printFunctions() {
    const char* names[] = {"radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "pow", "exp", "exp2",
                           "log", "log2", "sqrt", "inversesqrt", "abs", "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract", "mod", "modf", "min", "max",
                           "clamp", "mix", "fma", "isinf", "isnan", "step", "smoothstep", "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat",
                           "length", "distance", "dot", "cross", "normalize", "reflect", "refract", "faceforward", "matrixCompMult", "outerProduct", "transpose",
                           "determinant", "inverse", "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual", "any", "all", "not",
                           "dFdx", "dFdy", "fwidth", "dFdxCoarse", "dFdyCoarse", "dFdxFine", "dFdyFine", "fwidthCoarse", "fwidthFine", "packHalf2x16",
                           "unpackHalf2x16", "packUnorm2x16", "unpackUnorm2x16", "packSnorm2x16", "unpackSnorm2x16", "packUnorm4x8", "unpackUnorm4x8",
                           "packSnorm4x8", "unpackSnorm4x8", "bitfieldExtract", "bitfieldInsert", "bitfieldReverse", "bitCount", "findLSB", "findMSB", "frexp",
                           "ldexp", "umulExtended", "imulExtended", "uaddCarry", "usubBorrow", "texture", "textureProj", "textureLod", "textureProjLod",
                           "textureGrad", "textureGather", "textureSize", "texelFetch", "textureQueryLod", "textureQueryLevels"};
    std::printf("| Built-in | Overloads | HLSL |\n|---|---|---|\n");
    for (const char* n : names) {
        const std::vector<GdBuiltinSig> o = gdBuiltinOverloads(n);
        std::string h = o.empty() ? "?" : o[0].hlsl;
        std::printf("| `%s` | %zu | %s |\n", n, o.size(), h.empty() ? "same name" : h == "!" ? "unsupported" : ("`" + h + "`").c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--hooks") return printHooks();
    if (argc > 1 && std::string(argv[1]) == "--functions") return printFunctions();
    // SH2: the engine ships the helper library as modules/render.pbr/shaders/material_embedded_lib.hlsl (GodotShaderMaterialTest compares the two).
    if (argc > 1 && std::string(argv[1]) == "--library") { std::fputs(gdShaderHlslLibrary(), stdout); return 0; }
    if (argc > 1) return compileOne(argv[1]);
    testPreprocessor();
    testPrecedence();
    testStatements();
    testOverloads();
    testErrors();
    testLanguage();
    testUniformsAndHints();
    testBuiltinVariables();
    testLayout();
    testEmittedText();
    testRobustness();
    testHostileAndMacros();
    testHlsl();
    AVER_INFO("GodotShaderTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
