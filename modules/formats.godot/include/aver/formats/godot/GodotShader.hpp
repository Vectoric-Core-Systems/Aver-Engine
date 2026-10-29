#pragma once
// The Godot 4 shading-language front-end (Fabric G6 SH1): preprocessor, lexer, parser + type checker, and an HLSL emitter. Pure text in, plain
// structs out -- no engine type, no device. A shader goes through three calls:
//   gdShaderPreprocess  -> text with directives resolved, comments gone, one output line per origin line
//   gdShaderCompile     -> GdShaderModule: the typed AST plus everything a consumer needs (uniforms, varyings, render modes, ...)
//   gdShaderEmitHlsl    -> HLSL text, the uniform layout and the texture / sampler slots
// Written from the documented language (docs.godotengine.org shading language, spatial / particles / sky / fog shader pages), read for behaviour;
// no Godot code is copied. Contract and hook table: docs/plans/fabric/G6_SHADERS.md.
#include "aver/core/Types.hpp"
#include "aver/formats/godot/GodotDiagnostics.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::fmt::godot {

// ------------------------------------------------------------------------------------------------ diagnostics

struct GdShaderDiag {
    GodotSeverity severity = GodotSeverity::Error;
    std::string file;      // origin file (an #include shows its own name)
    int line = 0;          // 1-based; 0 = whole shader
    int column = 0;        // 1-based column in the preprocessed line; 0 = unknown
    std::string code;      // "gdshader.syntax", "gdshader.type", "gdshader.preprocess", ...
    std::string message;
    std::string str() const;   // "file:line:col: error: message"
};

// Folds shader diagnostics into the converter's GodotDiagnostics (the column goes into the message prefix).
void gdShaderDiagsToGodot(const std::vector<GdShaderDiag>& in, GodotDiagnostics& out);

// ------------------------------------------------------------------------------------------------ preprocessor

// `#include "path"`: fill `text` and `resolvedName` (the name later diagnostics and nested includes use). False = not found.
using GdIncludeResolver = std::function<bool(const std::string& includePath, const std::string& fromFile, std::string& text, std::string& resolvedName)>;

struct GdPreprocessOptions {
    GdIncludeResolver resolver;                                  // empty = every #include is an error
    std::vector<std::pair<std::string, std::string>> defines;    // predefined object-like macros (name, replacement)
    int maxIncludeDepth = 25;
};

struct GdLineOrigin { u16 file = 0; int line = 0; };

struct GdPreprocessed {
    std::string text;                      // output lines joined by '\n'; disabled lines and directives are empty lines
    std::vector<std::string> files;        // [0] = the main file
    std::vector<GdLineOrigin> lines;       // lines[i] = origin of output line i+1
    std::vector<std::string> pragmas;      // every #pragma's text (the ones the front-end acts on are also applied)
};

// Function-like macros whose argument list spans lines are joined; the joined text lands on the first line and the rest are empty.
bool gdShaderPreprocess(std::string_view source, std::string_view fileName, const GdPreprocessOptions& opt, GdPreprocessed& out,
                        std::vector<GdShaderDiag>& diags);

// ------------------------------------------------------------------------------------------------ types

// Group order matters: scalar = group start, vecN = start + N - 1 (gdMakeVec / gdScalarOf / gdDim rely on it).
enum class GdBase : u8 {
    Void = 0,
    Bool = 1, BVec2, BVec3, BVec4,
    Int = 5, IVec2, IVec3, IVec4,
    UInt = 9, UVec2, UVec3, UVec4,
    Float = 13, Vec2, Vec3, Vec4,
    Mat2 = 17, Mat3, Mat4,
    Sampler2D = 20, ISampler2D, USampler2D, Sampler2DArray, ISampler2DArray, USampler2DArray,
    Sampler3D, ISampler3D, USampler3D, SamplerCube, SamplerCubeArray, SamplerExternalOES,
    Struct = 40,
    Error = 41,
};

enum class GdKind : u8 { Void, Bool, Int, UInt, Float, Matrix, Sampler, Struct, Error };

struct GdType {
    GdBase base = GdBase::Void;
    i16 structIdx = -1;     // GdShaderModule::structs index when base == Struct
    i32 arraySize = 0;      // 0 = not an array; N > 0 = array of N
    bool operator==(const GdType& o) const { return base == o.base && structIdx == o.structIdx && arraySize == o.arraySize; }
    bool operator!=(const GdType& o) const { return !(*this == o); }
    bool isArray() const { return arraySize > 0; }
    GdType element() const { GdType t = *this; t.arraySize = 0; return t; }
};

GdKind gdKind(GdBase b);
int gdDim(GdBase b);                   // components of a scalar (1) / vector; the side of a matrix; 0 for others
GdBase gdScalarOf(GdBase b);           // vec3 -> float; scalar -> itself; matrix -> Float
GdBase gdMakeVec(GdBase scalar, int n);   // n = 1 gives the scalar
GdBase gdMakeMat(int n);
bool gdIsSampler(GdBase b);
bool gdIsNumeric(GdBase b);            // int / uint / float scalar, vector or matrix
const char* gdBaseName(GdBase b);      // "vec3", "sampler2D", ...
GdBase gdBaseFromName(std::string_view name);   // Error when it is not a type keyword
std::string gdTypeName(const GdType& t, const struct GdShaderModule* m = nullptr);   // "float[3]", struct names need the module

// A folded constant: components in order (matrix column-major), bool as 0 / 1.
struct GdConstValue {
    GdType type;
    std::vector<double> v;
    bool valid = false;
};

// ------------------------------------------------------------------------------------------------ AST

enum class GdShaderType : u8 { Spatial, CanvasItem, Particles, Sky, Fog };
const char* gdShaderTypeName(GdShaderType t);

enum class GdAccess : u8 { In, Out, InOut };

// What a Var expression refers to.
enum class GdRefKind : u8 { None, Local, Param, GlobalConst, Uniform, Varying, Builtin };

enum class GdExprKind : u8 {
    BoolLit, IntLit, UIntLit, FloatLit,
    Var,            // name + ref
    Unary,          // op = "+" "-" "!" "~"          args[0]
    Binary,         // op = "+" ... "||"             args[0], args[1]
    Assign,         // op = "=" "+=" ...             args[0] = lvalue, args[1]
    Ternary,        // args = cond, a, b
    Call,           // user function (functionIdx) or built-in (builtinName / builtinOverload)
    Construct,      // type constructor: vec3(...), mat4(...), StructName(...)
    ArrayConstruct, // float[3](...), float[](...)
    Member,         // struct field: args[0] . name (memberIdx)
    Swizzle,        // vector swizzle: args[0] . swizzle (xyzw-normalised)
    Index,          // args[0][args[1]]
    IncDec,         // op = "++" / "--", postfix flag
    ArrayLength,    // args[0].length()
    Comma,          // args[0], args[1]
};

struct GdExpr {
    GdExprKind kind = GdExprKind::IntLit;
    GdType type;
    int line = 0, col = 0;
    std::string name;       // Var / Call / Member / Construct (type name)
    std::string op;
    std::string swizzle;    // normalised to xyzw
    double num = 0;         // literals (bool 0/1, int, uint, float)
    bool postfix = false;
    GdRefKind ref = GdRefKind::None;
    int refIdx = -1;        // Uniform / Varying / GlobalConst index; Builtin: index into the built-in variable table
    int functionIdx = -1;   // Call -> user function (GdShaderModule::functions)
    int builtinOverload = -1;   // Call -> built-in overload id (valid with name when functionIdx < 0)
    int memberIdx = -1;
    std::shared_ptr<const GdConstValue> cv;   // Var that names a constant (local or global): its folded value
    std::vector<std::unique_ptr<GdExpr>> args;

    GdExpr() = default;
    GdExpr(const GdExpr&) = delete;
    GdExpr& operator=(const GdExpr&) = delete;
    ~GdExpr();   // iterative, so a long left-deep chain (a + b + c + ...) cannot overflow the stack
};
using GdExprP = std::unique_ptr<GdExpr>;

struct GdDeclarator {
    std::string name;
    GdType type;
    GdExprP init;
    int line = 0, col = 0;
};

enum class GdStmtKind : u8 { Block, Decl, Expr, If, For, While, DoWhile, Switch, Case, Default, Break, Continue, Return, Discard };

struct GdStmt {
    GdStmtKind kind = GdStmtKind::Block;
    int line = 0, col = 0;
    bool isConst = false;                    // Decl
    std::vector<GdDeclarator> decls;         // Decl
    std::vector<std::unique_ptr<GdStmt>> body;   // Block statements; Switch body (Case / Default are flat markers in it)
    GdExprP expr;                            // Expr, If / While / DoWhile / For condition, Switch value, Case value, Return value
    GdExprP step;                            // For step
    std::unique_ptr<GdStmt> init;            // For init (Decl or Expr or null)
    std::unique_ptr<GdStmt> thenS, elseS;    // If; While / DoWhile / For use thenS as the body
};
using GdStmtP = std::unique_ptr<GdStmt>;

struct GdStructMember { std::string name; GdType type; };
struct GdStruct { std::string name; std::vector<GdStructMember> members; int line = 0; };

struct GdConstDecl {
    std::string name;
    GdType type;
    GdExprP init;
    GdConstValue value;    // folded when the initialiser is constant (always for scalars / vectors / matrices of literals)
    int line = 0;
};

enum class GdInterp : u8 { Smooth, Flat };
struct GdVarying {
    std::string name;
    GdType type;
    GdInterp interp = GdInterp::Smooth;
    bool assignedInVertex = false;
    int line = 0;
};

enum class GdUniformScope : u8 { Normal, Instance, Global };
enum class GdFilter : u8 { Default, Nearest, Linear, NearestMipmap, LinearMipmap, NearestMipmapAniso, LinearMipmapAniso };
enum class GdRepeat : u8 { Default, Enable, Disable };
// A texture uniform's special source / default (hint_*). Screen, depth and normal-roughness are bound to the renderer's own targets.
enum class GdTexHint : u8 {
    None, DefaultWhite, DefaultBlack, DefaultTransparent, Normal, Anisotropy,
    RoughnessR, RoughnessG, RoughnessB, RoughnessA, RoughnessNormal, RoughnessGray,
    ScreenTexture, DepthTexture, NormalRoughnessTexture,
};

struct GdUniform {
    std::string name;
    GdType type;
    GdUniformScope scope = GdUniformScope::Normal;
    std::string group, subgroup;           // group_uniforms
    bool sourceColor = false;
    bool hasRange = false;
    double rangeMin = 0, rangeMax = 0, rangeStep = 0;
    std::vector<std::string> enumLabels;   // hint_enum
    GdTexHint texHint = GdTexHint::None;
    GdFilter filter = GdFilter::Default;
    GdRepeat repeat = GdRepeat::Default;
    int instanceIndex = -1;                // instance_index(n)
    bool hasDefault = false;
    GdConstValue def;                      // def.valid false when the initialiser was not a constant expression
    std::vector<std::string> hints;        // every hint as written, e.g. "hint_range", "source_color"
    int line = 0, col = 0;
};

enum class GdParamQual : u8 { In, Out, InOut };
struct GdParam { std::string name; GdType type; GdParamQual qual = GdParamQual::In; bool isConst = false; };

// The processor functions of each shader type.
enum class GdProcessor : u8 { None, Vertex, Fragment, Light, Start, Process, Sky, Fog };
const char* gdProcessorName(GdProcessor p);

struct GdFunction {
    std::string name;
    GdType ret;
    std::vector<GdParam> params;
    GdStmtP body;
    GdProcessor processor = GdProcessor::None;
    bool usesDiscard = false;
    int line = 0, col = 0;
};

// A built-in variable (TIME, VERTEX, ALBEDO, ...) in one shader type's table.
struct GdBuiltinVar {
    const char* name;
    GdBase type;
    GdAccess access;
    u16 stages;        // bitmask of 1 << GdProcessor
    bool global;       // readable from custom functions too (TIME, PI, TAU, E, ...)
};

constexpr u16 gdStageBit(GdProcessor p) { return static_cast<u16>(1u << static_cast<unsigned>(p)); }

// Per shader type: the table, its length. Index = GdExpr::refIdx of a Builtin Var.
const GdBuiltinVar* gdBuiltinVars(GdShaderType t, int& count);
// render_mode names the type accepts.
bool gdRenderModeValid(GdShaderType t, std::string_view mode);

struct GdShaderModule {
    GdShaderType shaderType = GdShaderType::Spatial;
    bool hasShaderType = false;
    bool is2D = false;                     // canvas_item: parsed, flagged unsupported
    std::vector<std::string> renderModes;
    std::vector<std::string> files;        // origin files (index 0 = main)
    std::vector<GdStruct> structs;
    std::vector<GdConstDecl> consts;       // global constants in order
    std::vector<GdUniform> uniforms;
    std::vector<GdVarying> varyings;
    std::vector<GdFunction> functions;     // in definition order (a function precedes its callers)
    std::vector<GdShaderDiag> diags;
    std::vector<std::string> pragmas;
    bool ok = false;                       // no error diagnostic

    int findStruct(std::string_view name) const;
    int findUniform(std::string_view name) const;
    int findVarying(std::string_view name) const;
    int findFunction(std::string_view name) const;   // first match
    const GdFunction* processor(GdProcessor p) const;
    bool hasRenderMode(std::string_view mode) const;
    usize errorCount() const;
};

struct GdCompileOptions {
    GdPreprocessOptions pre;
    std::vector<std::string> globalUniformNames;   // names known to exist as project shader globals (empty = accept any `global uniform`)
};

// Preprocess + parse + check. The module is always filled as far as it got; `ok` says whether it is usable. Diagnostics carry file:line:col.
bool gdShaderCompile(std::string_view source, std::string_view fileName, const GdCompileOptions& opt, GdShaderModule& out);

// ------------------------------------------------------------------------------------------------ built-in functions

// Overload table lookups (exposed for tests and for SH2's diagnostics).
struct GdBuiltinSig {
    const char* name;
    GdType ret;
    std::vector<GdBase> params;
    std::vector<bool> outParam;
    const char* hlsl;       // how the HLSL emitter spells it: "" = same name, "gd_x" = library helper, "@x" = inline rewrite, "!" = unsupported
};
// All overloads of `name` (empty when it is not a built-in function).
std::vector<GdBuiltinSig> gdBuiltinOverloads(std::string_view name);
bool gdIsBuiltinFunction(std::string_view name);

// ------------------------------------------------------------------------------------------------ HLSL emission

struct GdHlslOptions {
    std::string entryPrefix = "AverGd";    // processor function `fragment()` is emitted as <prefix>_fragment
    int textureRegisterBase = 0;           // t# of the first texture
    int samplerRegisterBase = 0;           // s# of the first sampler state
    int varyingSemanticBase = 0;           // TEXCOORD# of the first varying in the IO declaration
    GdFilter defaultFilter = GdFilter::LinearMipmap;   // project default for textures without filter_*
    GdRepeat defaultRepeat = GdRepeat::Disable;        // project default for textures without repeat_*
    // Which processor functions to emit (gdStageBit); custom functions are emitted only when one of them reaches them. A vertex-stage
    // compile should pass gdStageBit(Vertex) so the fragment-only hooks are never referenced.
    u16 processorMask = 0xFFFF;
    // SH2. Prepended to every global name the emitter writes (functions, structs, constants, sampler constructors, varying members), so several
    // shaders can share one translation unit. Empty = the SH1 names.
    std::string namePrefix;
    // SH2. When set, every global name (but not a varying member, which keeps `namePrefix`) is written NAMEMACRO(base) instead, e.g. `GDN(gdf_foo)`, and the
    // entry functions `GDN(<entryPrefix>_fragment)`: the consumer defines GDN to paste a name space on, then includes the one source under several (raster
    // and ray flavour of the engine's material shader).
    std::string nameMacro;
    // SH2. Sampler uniforms become engine texture slots: `gdsm_<name>()` returns a slot-sampler value (`GdSampler2D { slot, m }` under
    // AVER_GD_SLOT_SAMPLERS in the library) and no Texture / SamplerState global is declared. Only sampler2D is allowed. `samplerSlots` is one engine
    // slot per sampler uniform in declaration order. The mode word m: bit 0 nearest, bit 1 repeat, bit 2 mipmaps, bits 8-9 the colour
    // of an unbound texture (0 white, 1 black, 2 transparent, 3 flat normal: the hint_default_* / hint_normal hints).
    bool slotSamplers = false;
    std::vector<int> samplerSlots;
};

// One uniform in a float4 register array. Every register is 16 bytes; scalars and vectors pack as std140 does (a vec3 may be followed by a
// scalar in its fourth component, a vec2 aligns to two components), each array element and each matrix column starts a new register.
// int / uint / bool are the 32-bit pattern in the float slot (CPU writes the integer bits; bool is 0 / 1). A matrix is its columns in
// consecutive registers (mat3: three registers, three used components each).
struct GdUniformSlot {
    std::string name;
    GdType type;
    int reg = 0;            // first register
    int comp = 0;           // first component inside it (0..3), scalars / vectors only
    int registers = 0;      // registers occupied (arrays: count * per-element)
    int elementStride = 0;  // registers per array element (arrays only)
    GdUniformScope scope = GdUniformScope::Normal;
    int uniformIndex = -1;  // GdShaderModule::uniforms
};

struct GdUniformBlock {
    std::vector<GdUniformSlot> slots;
    int registerCount = 0;
};

struct GdSamplerStateDesc {
    GdFilter filter = GdFilter::LinearMipmap;
    GdRepeat repeat = GdRepeat::Disable;
    int reg = 0;            // s# register
    std::string hlslName;   // gds_<filter>_<repeat>
};

struct GdTextureSlot {
    std::string name;
    GdBase type = GdBase::Sampler2D;
    int reg = 0;                    // t# register
    int sampler = 0;                // index into GdHlslResult::samplers
    GdTexHint hint = GdTexHint::None;
    bool sourceColor = false;
    bool isGlobal = false;
    int uniformIndex = -1;
    std::string hlslName;           // the Texture2D variable
};

struct GdHookUse { std::string macro; bool read = false; bool written = false; GdBase type = GdBase::Void; };

// One varying member of the stage IO struct, without a semantic (SH2 assigns semantics when several shaders share one VS output).
struct GdVaryingSlot { std::string decl; bool flat = false; int rows = 1; };   // decl: "float3 <prefix>gdv_a" or "float3 <prefix>gdv_b[2]"

struct GdHlslResult {
    bool ok = false;
    std::string error;
    std::string source;                         // structs, uniform accessors, textures, functions, entry functions -- needs gdShaderHlslLibrary() first
    std::string varyingIo;                      // `float3 gdv_x : TEXCOORD0; ...` members for the stage IO struct (empty = no varyings)
    std::vector<GdVaryingSlot> varyingSlots;    // the same members as data
    GdUniformBlock uniforms, instanceUniforms, globalUniforms;
    std::vector<GdTextureSlot> textures;
    std::vector<GdSamplerStateDesc> samplers;
    std::vector<GdHookUse> hooks;               // every AVER_GD_* macro the source uses, deduplicated
    std::vector<std::string> entries;           // emitted processor function names ("AverGd_fragment", ...)
    std::vector<GdShaderDiag> diags;
};

// Emits HLSL for a checked module. Fails (ok = false, error set) for canvas_item, for a module with errors and for constructs HLSL has no
// spelling for (array return values, array constructors outside an initialiser, sampler arrays, matrix-array uniforms).
GdHlslResult gdShaderEmitHlsl(const GdShaderModule& m, const GdHlslOptions& opt = {});

// The fixed HLSL helper library the emitted source is written against (sampler wrapper structs, texture() family, mod / mix / sign / ...,
// matrix helpers, pack / unpack). Include it before the hook prelude and the emitted source.
const char* gdShaderHlslLibrary();

// The uniform layout alone (same rules as the emitter), for tools that fill a constant block without emitting code.
void gdShaderLayoutUniforms(const GdShaderModule& m, GdUniformBlock& normal, GdUniformBlock& instance, GdUniformBlock& global);

}  // namespace aver::fmt::godot
