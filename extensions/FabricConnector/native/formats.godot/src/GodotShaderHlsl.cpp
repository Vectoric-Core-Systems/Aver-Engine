// Godot shading language -> HLSL. Emits structs, uniform accessors over a float4 register array, texture and sampler-state declarations, global
// constants, the custom functions and the processor functions (vertex / fragment / light / start / process / sky / fog) as `<prefix>_<name>`.
// Built-in variables become `AVER_CUSTOM_<NAME>` macros the engine side defines (SH2 / SH3); math built-ins become HLSL intrinsics or helpers from
// gdShaderHlslLibrary(). Matrices keep GLSL's memory order (HLSL rows hold GLSL columns), so m[i] is column i and `A * B` is mul(B, A).
#include "GodotShaderInternal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace aver::fmt::godot {
namespace {

using B = GdBase;

// ------------------------------------------------------------------------------------------------ layout

int registersPerElement(const GdType& t) {
    const GdKind k = gdKind(t.base);
    if (k == GdKind::Matrix) return gdDim(t.base);
    return 1;
}

void layoutBlock(const GdShaderModule& m, GdUniformScope scope, GdUniformBlock& blk) {
    blk = GdUniformBlock{};
    int reg = 0, comp = 0;
    auto nextReg = [&] { if (comp != 0) { ++reg; comp = 0; } };
    for (usize i = 0; i < m.uniforms.size(); ++i) {
        const GdUniform& u = m.uniforms[i];
        if (u.scope != scope || gdIsSampler(u.type.base)) continue;
        GdUniformSlot s;
        s.name = u.name;
        s.type = u.type;
        s.scope = scope;
        s.uniformIndex = static_cast<int>(i);
        const GdKind k = gdKind(u.type.base);
        if (u.type.isArray() || k == GdKind::Matrix) {
            nextReg();
            s.reg = reg;
            s.comp = 0;
            s.elementStride = registersPerElement(u.type);
            s.registers = s.elementStride * (u.type.isArray() ? u.type.arraySize : 1);
            reg += s.registers;
        } else {
            const int n = gdDim(u.type.base);
            const int align = n == 1 ? 1 : n == 2 ? 2 : 4;   // vec3 / vec4 start a register (std140), vec2 on an even component
            if (comp % align != 0) comp = (comp + align - 1) / align * align;
            if (comp + n > 4) { comp = 4; nextReg(); }
            s.reg = reg;
            s.comp = comp;
            s.registers = 1;
            s.elementStride = 1;
            comp += n;
            if (comp >= 4) { ++reg; comp = 0; }
        }
        blk.slots.push_back(std::move(s));
    }
    blk.registerCount = reg + (comp > 0 ? 1 : 0);
}

const char* hlslTypeOf(GdBase b) {
    switch (b) {
    case B::Void: return "void";
    case B::Bool: return "bool"; case B::BVec2: return "bool2"; case B::BVec3: return "bool3"; case B::BVec4: return "bool4";
    case B::Int: return "int"; case B::IVec2: return "int2"; case B::IVec3: return "int3"; case B::IVec4: return "int4";
    case B::UInt: return "uint"; case B::UVec2: return "uint2"; case B::UVec3: return "uint3"; case B::UVec4: return "uint4";
    case B::Float: return "float"; case B::Vec2: return "float2"; case B::Vec3: return "float3"; case B::Vec4: return "float4";
    case B::Mat2: return "float2x2"; case B::Mat3: return "float3x3"; case B::Mat4: return "float4x4";
    case B::Sampler2D: return "EmbSampler2D"; case B::ISampler2D: return "EmbISampler2D"; case B::USampler2D: return "EmbUSampler2D";
    case B::Sampler2DArray: return "EmbSampler2DArray"; case B::ISampler2DArray: return "EmbISampler2DArray"; case B::USampler2DArray: return "EmbUSampler2DArray";
    case B::Sampler3D: return "EmbSampler3D"; case B::ISampler3D: return "EmbISampler3D"; case B::USampler3D: return "EmbUSampler3D";
    case B::SamplerCube: return "EmbSamplerCube"; case B::SamplerCubeArray: return "EmbSamplerCubeArray";
    default: return "float";
    }
}

const char* textureTypeOf(GdBase b) {
    switch (b) {
    case B::Sampler2D: return "Texture2D<float4>"; case B::ISampler2D: return "Texture2D<int4>"; case B::USampler2D: return "Texture2D<uint4>";
    case B::Sampler2DArray: return "Texture2DArray<float4>"; case B::ISampler2DArray: return "Texture2DArray<int4>"; case B::USampler2DArray: return "Texture2DArray<uint4>";
    case B::Sampler3D: return "Texture3D<float4>"; case B::ISampler3D: return "Texture3D<int4>"; case B::USampler3D: return "Texture3D<uint4>";
    case B::SamplerCube: return "TextureCube<float4>"; case B::SamplerCubeArray: return "TextureCubeArray<float4>";
    default: return nullptr;
    }
}

const char* filterName(GdFilter f) {
    switch (f) {
    case GdFilter::Nearest: return "nearest";
    case GdFilter::Linear: return "linear";
    case GdFilter::NearestMipmap: return "nearest_mipmap";
    case GdFilter::NearestMipmapAniso: return "nearest_mipmap_aniso";
    case GdFilter::LinearMipmapAniso: return "linear_mipmap_aniso";
    default: return "linear_mipmap";
    }
}

std::string fmtFloat(double v) {
    if (std::isinf(v)) v = v > 0 ? 3.4e38 : -3.4e38;
    if (std::isnan(v)) v = 0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

// ------------------------------------------------------------------------------------------------ emitter

class Emitter {
public:
    Emitter(const GdShaderModule& m, const GdHlslOptions& opt, GdHlslResult& r) : m_(m), opt_(opt), r_(r) {
        bv_ = gdBuiltinVars(m.shaderType, bvCount_);
    }

    void run() {
        if (m_.errorCount() != 0) { fatal("the shader has errors"); return; }
        if (m_.is2D) { fatal("canvas_item (2D) shaders are not supported"); return; }
        gdShaderLayoutUniforms(m_, r_.uniforms, r_.instanceUniforms, r_.globalUniforms);
        buildSlots();
        markReachable();

        o_ += "// Generated from a Godot ";
        o_ += gdShaderTypeName(m_.shaderType);
        o_ += " shader by gdShaderEmitHlsl. Needs gdShaderHlslLibrary() first and the AVER_CUSTOM_* hook macros.\n";
        emitStructs();
        emitTextures();
        emitVaryingIo();
        emitConsts();
        for (usize i = 0; i < m_.functions.size(); ++i) {
            const GdFunction& f = m_.functions[i];
            if (!reach_[i]) continue;
            emitFunction(f);
        }
        r_.source = o_;
        r_.ok = r_.diags.empty() || std::none_of(r_.diags.begin(), r_.diags.end(), [](const GdShaderDiag& d) { return d.severity == GodotSeverity::Error; });
        if (!r_.ok && r_.error.empty()) {
            for (const GdShaderDiag& d : r_.diags) if (d.severity == GodotSeverity::Error) { r_.error = d.message; break; }
        }
    }

private:
    const GdShaderModule& m_;
    const GdHlslOptions& opt_;
    GdHlslResult& r_;
    std::string o_;
    int ind_ = 0;
    const GdBuiltinVar* bv_ = nullptr;
    int bvCount_ = 0;
    std::vector<int> slotOfUniform_;     // uniform index -> index into the scope's block, -1 for samplers
    std::vector<int> texOfUniform_;      // uniform index -> index into r_.textures, -1 otherwise
    std::vector<bool> reach_;
    std::vector<std::string> slotLines_;   // slotSamplers: the sampler constructors, emitted after the loop
    std::map<std::string, usize> hookIdx_;
    GdProcessor curProc_ = GdProcessor::None;
    const GdExpr* noRead_ = nullptr;

    void fatal(const std::string& msg) {
        r_.ok = false;
        r_.error = msg;
        GdShaderDiag d;
        d.severity = GodotSeverity::Error;
        d.code = "gdshader.hlsl";
        d.message = msg;
        r_.diags.push_back(d);
    }
    void unsupported(const GdExpr* e, const std::string& msg) {
        GdShaderDiag d;
        d.severity = GodotSeverity::Error;
        d.code = "gdshader.hlsl_unsupported";
        d.line = e ? e->line : 0;
        d.column = e ? e->col : 0;
        d.file = m_.files.empty() ? std::string() : m_.files[0];
        d.message = msg;
        r_.diags.push_back(std::move(d));
    }

    void line(const std::string& s) {
        o_.append(static_cast<usize>(ind_) * 4, ' ');
        o_ += s;
        o_ += '\n';
    }

    // ---- names
    static std::string lname(const std::string& n) { return "l_" + n; }
    // A global name: the prefix and the base, or (nameMacro) NAMEMACRO(base) so one source can be included under several name spaces.
    std::string gn(const std::string& base) const { return opt_.nameMacro.empty() ? opt_.namePrefix + base : opt_.nameMacro + "(" + base + ")"; }
    std::string sname(const GdType& t) const { return gn("GdS_" + m_.structs[static_cast<usize>(t.structIdx)].name); }
    const std::string& px() const { return opt_.namePrefix; }
    std::string typeName(const GdType& t) const { return t.base == B::Struct ? sname(t) : hlslTypeOf(t.base); }
    std::string declare(const GdType& t, const std::string& name) const {
        std::string s = typeName(t) + " " + name;
        if (t.isArray()) s += "[" + std::to_string(t.arraySize) + "]";
        return s;
    }

    // ---- uniform / texture slots
    void buildSlots() {
        slotOfUniform_.assign(m_.uniforms.size(), -1);
        texOfUniform_.assign(m_.uniforms.size(), -1);
        for (const GdUniformBlock* b : {&r_.uniforms, &r_.instanceUniforms, &r_.globalUniforms})
            for (usize i = 0; i < b->slots.size(); ++i) slotOfUniform_[static_cast<usize>(b->slots[i].uniformIndex)] = static_cast<int>(i);
    }

    const GdUniformSlot& slotOf(const GdUniform& u, int ui) const {
        const GdUniformBlock& b = u.scope == GdUniformScope::Instance ? r_.instanceUniforms : u.scope == GdUniformScope::Global ? r_.globalUniforms : r_.uniforms;
        return b.slots[static_cast<usize>(slotOfUniform_[static_cast<usize>(ui)])];
    }
    static const char* bufName(GdUniformScope s) { return s == GdUniformScope::Instance ? "AVER_CUSTOM_IBUF" : s == GdUniformScope::Global ? "AVER_CUSTOM_GBUF" : "AVER_CUSTOM_UBUF"; }

    void emitTextures() {
        int texIndex = 0;
        for (usize i = 0; i < m_.uniforms.size(); ++i) {
            const GdUniform& u = m_.uniforms[i];
            if (!gdIsSampler(u.type.base)) continue;
            if (u.type.isArray()) { unsupported(nullptr, "sampler array uniform '" + u.name + "' has no HLSL spelling"); continue; }
            if (!textureTypeOf(u.type.base)) { unsupported(nullptr, "samplerExternalOES uniform '" + u.name + "' is not supported"); continue; }
            GdTextureSlot t;
            t.name = u.name;
            t.type = u.type.base;
            t.reg = opt_.textureRegisterBase + texIndex++;
            t.hint = u.texHint;
            t.sourceColor = u.sourceColor;
            t.isGlobal = u.scope == GdUniformScope::Global;
            t.uniformIndex = static_cast<int>(i);
            t.hlslName = px() + "gdt_" + u.name;
            const GdFilter f = u.filter == GdFilter::Default ? opt_.defaultFilter : u.filter;
            const GdRepeat rp = u.repeat == GdRepeat::Default ? opt_.defaultRepeat : u.repeat;
            if (opt_.slotSamplers) {
                // The engine binds the material's own texture slots; one slot-sampler value per uniform, no deduplicated sampler states.
                if (u.type.base != B::Sampler2D) { unsupported(nullptr, "sampler uniform '" + u.name + "' is a " + gdBaseName(u.type.base) + "; only sampler2D reaches an engine material slot"); continue; }
                if (texIndex > static_cast<int>(opt_.samplerSlots.size())) { unsupported(nullptr, "no engine texture slot is left for sampler uniform '" + u.name + "'"); continue; }
                t.reg = opt_.samplerSlots[static_cast<usize>(texIndex - 1)];
                t.sampler = 0;
                const int unbound = u.texHint == GdTexHint::DefaultBlack ? 1 : u.texHint == GdTexHint::DefaultTransparent ? 2 : u.texHint == GdTexHint::Normal ? 3 : 0;
                const int mode = (f == GdFilter::Nearest || f == GdFilter::NearestMipmap || f == GdFilter::NearestMipmapAniso ? 1 : 0) | (rp == GdRepeat::Enable ? 2 : 0) |
                                 (f == GdFilter::Nearest || f == GdFilter::Linear ? 0 : 4) | (unbound << 8);
                texOfUniform_[i] = static_cast<int>(r_.textures.size());
                r_.textures.push_back(t);
                slotLines_.push_back(std::string("EmbSampler2D ") + gn("gdsm_" + t.name) + "() { EmbSampler2D r; r.slot = " + std::to_string(t.reg) + "u; r.m = " + std::to_string(mode) + "u; return r; }");
                continue;
            }
            usize si = 0;
            for (; si < r_.samplers.size(); ++si) if (r_.samplers[si].filter == f && r_.samplers[si].repeat == rp) break;
            if (si == r_.samplers.size()) {
                GdSamplerStateDesc sd;
                sd.filter = f;
                sd.repeat = rp;
                sd.reg = opt_.samplerRegisterBase + static_cast<int>(si);
                sd.hlslName = px() + "gds_" + filterName(f) + (rp == GdRepeat::Enable ? "_repeat" : "_clamp");
                r_.samplers.push_back(sd);
            }
            t.sampler = static_cast<int>(si);
            texOfUniform_[i] = static_cast<int>(r_.textures.size());
            r_.textures.push_back(std::move(t));
        }
        if (r_.textures.empty()) return;
        if (opt_.slotSamplers) { for (const std::string& l : slotLines_) line(l); return; }
        for (const GdSamplerStateDesc& s : r_.samplers) line("SamplerState " + s.hlslName + " : register(s" + std::to_string(s.reg) + ");");
        for (const GdTextureSlot& t : r_.textures) {
            line(std::string(textureTypeOf(t.type)) + " " + t.hlslName + " : register(t" + std::to_string(t.reg) + ");");
            line(std::string(hlslTypeOf(t.type)) + " " + gn("gdsm_" + t.name) + "() { " + hlslTypeOf(t.type) + " r; r.t = " + t.hlslName + "; r.s = " +
                 r_.samplers[static_cast<usize>(t.sampler)].hlslName + "; return r; }");
        }
    }

    void emitVaryingIo() {
        int sem = opt_.varyingSemanticBase;
        for (const GdVarying& v : m_.varyings) {
            std::string s;
            if (v.interp == GdInterp::Flat) s += "nointerpolation ";
            s += declare(v.type, px() + "gdv_" + v.name);
            const int rows = (gdKind(v.type.base) == GdKind::Matrix ? gdDim(v.type.base) : 1) * (v.type.isArray() ? v.type.arraySize : 1);
            r_.varyingSlots.push_back(GdVaryingSlot{declare(v.type, px() + "gdv_" + v.name), v.interp == GdInterp::Flat, rows});
            s += " : TEXCOORD" + std::to_string(sem) + ";";
            sem += rows;
            r_.varyingIo += s + "\n";
        }
    }

    // ---- structs
    void emitStructs() {
        for (usize si = 0; si < m_.structs.size(); ++si) {
            const GdStruct& st = m_.structs[si];
            line("struct " + gn("GdS_" + st.name) + " {");
            ++ind_;
            for (const GdStructMember& mem : st.members) line(declare(mem.type, "m_" + mem.name) + ";");
            --ind_;
            line("};");
            std::string sig = gn("GdS_" + st.name) + " " + gn("gdctor_" + st.name) + "(";
            for (usize i = 0; i < st.members.size(); ++i) sig += (i ? ", " : "") + declare(st.members[i].type, "a" + std::to_string(i));
            sig += ") { " + gn("GdS_" + st.name) + " s;";
            for (usize i = 0; i < st.members.size(); ++i) {
                if (st.members[i].type.isArray())
                    sig += " [unroll] for (int k = 0; k < " + std::to_string(st.members[i].type.arraySize) + "; ++k) s.m_" + st.members[i].name + "[k] = a" + std::to_string(i) + "[k];";
                else sig += " s.m_" + st.members[i].name + " = a" + std::to_string(i) + ";";
            }
            sig += " return s; }";
            line(sig);
        }
    }

    void emitConsts() {
        for (const GdConstDecl& c : m_.consts) line("static const " + declare(c.type, gn("gdc_" + c.name)) + " = " + initializer(*c.init) + ";");
    }

    // ---- reachability
    void markReachable() {
        reach_.assign(m_.functions.size(), false);
        std::function<void(const GdExpr&)> walkE;
        std::function<void(const GdStmt&)> walkS;
        std::function<void(usize)> visit = [&](usize fi) {
            if (reach_[fi]) return;
            reach_[fi] = true;
            if (m_.functions[fi].body) walkS(*m_.functions[fi].body);
        };
        walkE = [&](const GdExpr& e) {
            if (e.kind == GdExprKind::Call && e.functionIdx >= 0) visit(static_cast<usize>(e.functionIdx));
            for (const GdExprP& a : e.args) if (a) walkE(*a);
        };
        walkS = [&](const GdStmt& s) {
            for (const GdDeclarator& d : s.decls) if (d.init) walkE(*d.init);
            if (s.expr) walkE(*s.expr);
            if (s.step) walkE(*s.step);
            if (s.init) walkS(*s.init);
            if (s.thenS) walkS(*s.thenS);
            if (s.elseS) walkS(*s.elseS);
            for (const GdStmtP& b : s.body) walkS(*b);
        };
        for (usize i = 0; i < m_.functions.size(); ++i)
            if (m_.functions[i].processor != GdProcessor::None && (opt_.processorMask & gdStageBit(m_.functions[i].processor))) visit(i);
    }

    // ---- hooks
    void noteHook(const std::string& macro, GdBase type, bool write) {
        auto it = hookIdx_.find(macro);
        if (it == hookIdx_.end()) {
            hookIdx_[macro] = r_.hooks.size();
            GdHookUse h;
            h.macro = macro;
            h.type = type;
            r_.hooks.push_back(h);
            it = hookIdx_.find(macro);
        }
        if (write) r_.hooks[it->second].written = true;
        else r_.hooks[it->second].read = true;
    }
    void markWritten(const GdExpr& e) {
        if (e.kind == GdExprKind::Var && e.ref == GdRefKind::Builtin) noteHook(std::string("AVER_CUSTOM_") + bv_[e.refIdx].name, bv_[e.refIdx].type, true);
        else if (e.kind == GdExprKind::Member || e.kind == GdExprKind::Index || e.kind == GdExprKind::Swizzle) markWritten(*e.args[0]);
    }

    // ---- expressions
    std::string uniformValue(const GdUniform& u, const GdUniformSlot& s, const std::string& regExpr) {
        const std::string buf = bufName(u.scope);
        auto sw = [&](int n) { return std::string(".") + std::string("xyzw").substr(static_cast<usize>(s.comp), static_cast<usize>(n)); };
        const GdKind k = gdKind(u.type.base);
        const int n = gdDim(u.type.base);
        auto reg = [&](int add) { return buf + "[" + (add ? regExpr + " + " + std::to_string(add) : regExpr) + "]"; };
        if (k == GdKind::Matrix) {
            std::string s2 = std::string(hlslTypeOf(u.type.base)) + "(";
            for (int c = 0; c < n; ++c) s2 += (c ? ", " : "") + reg(c) + "." + std::string("xyzw").substr(0, static_cast<usize>(n));
            return s2 + ")";
        }
        const std::string raw = reg(0) + sw(n);
        switch (k) {
        case GdKind::Int: return "asint(" + raw + ")";
        case GdKind::UInt: return "asuint(" + raw + ")";
        case GdKind::Bool: return "(asuint(" + raw + ") != 0)";
        default: return raw;
        }
    }

    std::string varRef(const GdExpr& e) {
        switch (e.ref) {
        case GdRefKind::Local: case GdRefKind::Param: return lname(e.name);
        case GdRefKind::GlobalConst: return gn("gdc_" + e.name);
        case GdRefKind::Varying: return "AVER_CUSTOM_VARYINGS." + px() + "gdv_" + e.name;
        case GdRefKind::Uniform: {
            const GdUniform& u = m_.uniforms[static_cast<usize>(e.refIdx)];
            if (gdIsSampler(u.type.base)) return gn("gdsm_" + u.name) + "()";
            if (u.type.isArray()) { unsupported(&e, "uniform array '" + u.name + "' can be used only by indexing or .length()"); return "0"; }
            const GdUniformSlot& s = slotOf(u, e.refIdx);
            return uniformValue(u, s, std::to_string(s.reg));
        }
        case GdRefKind::Builtin: {
            const GdBuiltinVar& b = bv_[e.refIdx];
            const std::string n = b.name;
            if (n == "PI") return "3.14159265358979323846";
            if (n == "TAU") return "6.28318530717958647692";
            if (n == "E") return "2.71828182845904523536";
            if (&e != noRead_) noteHook("AVER_CUSTOM_" + n, b.type, false);
            return "AVER_CUSTOM_" + n;
        }
        default: return "0";
        }
    }

    std::string args(const GdExpr& e, usize from = 0) {
        std::string s;
        for (usize i = from; i < e.args.size(); ++i) s += (i > from ? ", " : "") + expr(*e.args[i]);
        return s;
    }

    static bool isMat(const GdType& t) { return !t.isArray() && gdKind(t.base) == GdKind::Matrix; }
    static bool isScalarT(const GdType& t) { return !t.isArray() && gdDim(t.base) == 1 && gdKind(t.base) != GdKind::Matrix; }

    std::string expr(const GdExpr& e) {
        switch (e.kind) {
        case GdExprKind::BoolLit: return e.num != 0 ? "true" : "false";
        case GdExprKind::IntLit: return std::to_string(static_cast<long long>(e.num));
        case GdExprKind::UIntLit: return std::to_string(static_cast<unsigned long long>(e.num)) + "u";
        case GdExprKind::FloatLit: return fmtFloat(e.num);
        case GdExprKind::Var: return varRef(e);
        case GdExprKind::Unary: return "(" + e.op + expr(*e.args[0]) + ")";
        case GdExprKind::Binary: return binary(e);
        case GdExprKind::Assign: return assign(e);
        case GdExprKind::Ternary: return "(" + expr(*e.args[0]) + " ? " + expr(*e.args[1]) + " : " + expr(*e.args[2]) + ")";
        case GdExprKind::Call: return call(e);
        case GdExprKind::Construct: return construct(e);
        case GdExprKind::ArrayConstruct:
            unsupported(&e, "an array constructor is supported only as a declaration or constant initialiser");
            return "0";
        case GdExprKind::Member: return operand(*e.args[0]) + ".m_" + e.name;
        case GdExprKind::Swizzle: return operand(*e.args[0]) + "." + e.swizzle;
        case GdExprKind::Index: return index(e);
        case GdExprKind::IncDec:
            markWritten(*e.args[0]);
            return e.postfix ? "(" + expr(*e.args[0]) + e.op + ")" : "(" + e.op + expr(*e.args[0]) + ")";
        case GdExprKind::ArrayLength: return std::to_string(static_cast<int>(e.num));
        case GdExprKind::Comma: return "(" + expr(*e.args[0]) + ", " + expr(*e.args[1]) + ")";
        }
        return "0";
    }

    std::string index(const GdExpr& e) {
        const GdExpr& b = *e.args[0];
        if (b.kind == GdExprKind::Var && b.ref == GdRefKind::Uniform && b.type.isArray()) {
            const GdUniform& u = m_.uniforms[static_cast<usize>(b.refIdx)];
            if (gdKind(u.type.base) == GdKind::Matrix) { unsupported(&e, "matrix array uniform '" + u.name + "' has no HLSL spelling"); return "0"; }
            const GdUniformSlot& s = slotOf(u, b.refIdx);
            GdUniform elem = u;
            elem.type.arraySize = 0;
            return uniformValue(elem, s, "(" + std::to_string(s.reg) + " + (int)(" + expr(*e.args[1]) + "))");
        }
        return operand(b) + "[" + expr(*e.args[1]) + "]";
    }

    // An expression used as the base of [] or . : plain when it is already a postfix-safe form.
    std::string operand(const GdExpr& b) {
        std::string s = expr(b);
        const bool safe = (b.kind == GdExprKind::Var && b.ref != GdRefKind::Builtin && b.ref != GdRefKind::Uniform) || b.kind == GdExprKind::Index || b.kind == GdExprKind::Member ||
                          b.kind == GdExprKind::Swizzle || b.kind == GdExprKind::Call;
        return safe ? s : "(" + s + ")";
    }

    // Drops one pair of parentheses that wraps the whole text (statements and conditions read better without them).
    static std::string unwrap(const std::string& s) {
        if (s.size() < 2 || s.front() != '(' || s.back() != ')') return s;
        int depth = 0;
        for (usize i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')' && --depth == 0 && i + 1 < s.size()) return s;
        }
        return s.substr(1, s.size() - 2);
    }

    std::string binary(const GdExpr& e) {
        const GdExpr& a = *e.args[0];
        const GdExpr& b = *e.args[1];
        const std::string sa = expr(a), sb = expr(b);
        if (e.op == "*" && (isMat(a.type) || isMat(b.type))) {
            const bool aMat = isMat(a.type), bMat = isMat(b.type);
            if ((aMat && bMat) || (aMat && !isScalarT(b.type)) || (bMat && !isScalarT(a.type))) return "mul(" + sb + ", " + sa + ")";
        }
        if ((e.op == "==" || e.op == "!=") && !isScalarT(a.type)) {
            if (a.type.base == B::Struct || a.type.isArray()) { unsupported(&e, "comparing structs or arrays has no HLSL spelling"); return "false"; }
            if (isMat(a.type)) return std::string(e.op == "==" ? "" : "!") + "emb_eq(" + sa + ", " + sb + ")";   // DXC's SPIR-V back end has no matrix ==

            return std::string(e.op == "==" ? "all" : "any") + "(" + sa + " " + e.op + " " + sb + ")";
        }
        return "(" + sa + " " + e.op + " " + sb + ")";
    }

    std::string assign(const GdExpr& e) {
        const GdExpr& l = *e.args[0];
        const GdExpr& r = *e.args[1];
        markWritten(l);
        if (e.op == "=") {   // a plain store does not read the target built-in
            const GdExpr* root = &l;
            while (root->kind == GdExprKind::Member || root->kind == GdExprKind::Index || root->kind == GdExprKind::Swizzle) root = root->args[0].get();
            noRead_ = root;
        }
        const std::string sl = expr(l);
        noRead_ = nullptr;
        const std::string sr = expr(r);
        if (e.op == "*=" && (isMat(r.type) || isMat(l.type)) && !isScalarT(r.type)) return "(" + sl + " = mul(" + sr + ", " + sl + "))";
        return "(" + sl + " " + e.op + " " + sr + ")";
    }

    // ---- constructors
    static const char* castType(GdBase b) { return hlslTypeOf(b); }

    std::string construct(const GdExpr& e) {
        const GdType& t = e.type;
        if (t.base == B::Struct) return gn("gdctor_" + m_.structs[static_cast<usize>(t.structIdx)].name) + "(" + args(e) + ")";
        const GdKind tk = gdKind(t.base);
        const int n = gdDim(t.base);
        if (tk == GdKind::Matrix) return constructMat(e, n);
        const std::string T = castType(t.base);
        if (e.args.size() == 1) {
            const GdExpr& a = *e.args[0];
            const int ad = gdDim(a.type.base);
            if (ad == 1) return "((" + T + ")(" + expr(a) + "))";
            if (ad == n) return "((" + T + ")(" + expr(a) + "))";
            return "((" + T + ")((" + expr(a) + ")." + std::string("xyzw").substr(0, static_cast<usize>(n)) + "))";
        }
        // several arguments: each keeps its own width but takes the target scalar kind
        const GdBase sc = gdScalarOf(t.base);
        std::string s = T + "(";
        int used = 0;
        for (usize i = 0; i < e.args.size(); ++i) {
            const GdExpr& a = *e.args[i];
            const int ad = gdDim(a.type.base);
            std::string v = expr(a);
            const int take = std::min(ad, n - used);
            if (take < ad) v = "(" + v + ")." + std::string("xyzw").substr(0, static_cast<usize>(take));
            if (gdScalarOf(a.type.base) != sc) v = "((" + std::string(castType(gdMakeVec(sc, take))) + ")(" + v + "))";
            s += (i ? ", " : "") + v;
            used += take;
            if (used >= n) break;
        }
        return s + ")";
    }

    std::string constructMat(const GdExpr& e, int n) {
        const std::string T = hlslTypeOf(e.type.base);
        const GdExpr& a0 = *e.args[0];
        if (e.args.size() == 1 && isMat(a0.type)) {
            const int d = gdDim(a0.type.base);
            if (d == n) return expr(a0);
            if (d > n) return "((" + T + ")(" + expr(a0) + "))";
            return "emb_mat" + std::to_string(n) + "_from" + std::to_string(d) + "(" + expr(a0) + ")";
        }
        if (e.args.size() == 1 && isScalarT(a0.type)) return "emb_mat" + std::to_string(n) + "_diag((float)(" + expr(a0) + "))";
        std::string s = T + "(";
        for (usize i = 0; i < e.args.size(); ++i) {
            const GdExpr& a = *e.args[i];
            std::string v = expr(a);
            if (gdScalarOf(a.type.base) != B::Float) v = "((" + std::string(castType(gdMakeVec(B::Float, gdDim(a.type.base)))) + ")(" + v + "))";
            s += (i ? ", " : "") + v;
        }
        return s + ")";
    }

    // ---- calls
    std::string call(const GdExpr& e) {
        if (e.functionIdx >= 0) {
            const GdFunction& f = m_.functions[static_cast<usize>(e.functionIdx)];
            for (usize i = 0; i < f.params.size(); ++i) if (f.params[i].qual != GdParamQual::In) markWritten(*e.args[i]);
            return gn("gdf_" + e.name) + "(" + args(e) + ")";
        }
        const detail::BuiltinOverload& o = detail::builtinOverloads()[static_cast<usize>(e.builtinOverload)];
        for (usize i = 0; i < o.params.size(); ++i) if (o.outParam[i]) markWritten(*e.args[i]);
        const std::string& h = o.hlsl;
        if (h == "!") { unsupported(&e, "built-in '" + o.name + "' has no HLSL spelling"); return "0"; }
        if (h == "@atan") return (e.args.size() == 1 ? "atan(" : "atan2(") + args(e) + ")";
        if (h == "@compmult") return "(" + expr(*e.args[0]) + " * " + expr(*e.args[1]) + ")";
        if (h == "@not") return "(!" + expr(*e.args[0]) + ")";
        if (h.rfind("@cmp", 0) == 0) return "(" + expr(*e.args[0]) + " " + h.substr(4) + " " + expr(*e.args[1]) + ")";
        if (h == "@splat") {
            const int n = gdDim(e.type.base);
            std::string s = o.name + "(";
            for (usize i = 0; i < e.args.size(); ++i) {
                std::string v = expr(*e.args[i]);
                if (n > 1 && isScalarT(e.args[i]->type)) v = "((" + std::string(hlslTypeOf(e.type.base)) + ")(" + v + "))";
                s += (i ? ", " : "") + v;
            }
            return s + ")";
        }
        return (h.empty() ? o.name : h) + "(" + args(e) + ")";
    }

    // ---- initialisers and statements
    std::string initializer(const GdExpr& e) {
        if (e.kind == GdExprKind::ArrayConstruct) {
            std::string s = "{ ";
            for (usize i = 0; i < e.args.size(); ++i) s += (i ? ", " : "") + expr(*e.args[i]);
            return s + " }";
        }
        if (e.type.isArray()) {
            // an array-typed value (const array, another array): HLSL copies array to array on initialisation
            return expr(e);
        }
        return expr(e);
    }

    std::string declStmt(const GdStmt& s, bool inlineFor) {
        std::string out;
        for (usize i = 0; i < s.decls.size(); ++i) {
            const GdDeclarator& d = s.decls[i];
            std::string one = (s.isConst ? "const " : "") + declare(d.type, lname(d.name));
            if (d.init) one += " = " + initializer(*d.init);
            if (inlineFor) {
                if (i == 0) out = one;
                else out += ", " + lname(d.name) + (d.init ? " = " + initializer(*d.init) : "");
            } else out += (i ? "\n" : "") + one + ";";
        }
        return out;
    }

    void stmtLines(const GdStmt& s) {
        switch (s.kind) {
        case GdStmtKind::Block:
            line("{");
            ++ind_;
            for (const GdStmtP& c : s.body) stmtLines(*c);
            --ind_;
            line("}");
            break;
        case GdStmtKind::Decl:
            for (const GdDeclarator& d : s.decls) {
                std::string one = (s.isConst ? "const " : "") + declare(d.type, lname(d.name));
                if (d.init) one += " = " + initializer(*d.init);
                line(one + ";");
            }
            break;
        case GdStmtKind::Expr: line(unwrap(expr(*s.expr)) + ";"); break;
        case GdStmtKind::If:
            line("if (" + unwrap(expr(*s.expr)) + ")");
            body(*s.thenS);
            if (s.elseS) { line("else"); body(*s.elseS); }
            break;
        case GdStmtKind::For: {
            std::string init;
            if (s.init) init = s.init->kind == GdStmtKind::Decl ? declStmt(*s.init, true) : expr(*s.init->expr);
            line("for (" + init + "; " + (s.expr ? unwrap(expr(*s.expr)) : std::string()) + "; " + (s.step ? unwrap(expr(*s.step)) : std::string()) + ")");
            body(*s.thenS);
            break;
        }
        case GdStmtKind::While: line("while (" + unwrap(expr(*s.expr)) + ")"); body(*s.thenS); break;
        case GdStmtKind::DoWhile: line("do"); body(*s.thenS); line("while (" + unwrap(expr(*s.expr)) + ");"); break;
        case GdStmtKind::Switch: {
            line("switch (" + unwrap(expr(*s.expr)) + ") {");
            ++ind_;
            for (const GdStmtP& c : s.body) {
                if (c->kind == GdStmtKind::Case) {
                    GdConstValue v;
                    detail::foldConst(*c->expr, m_, v);
                    line("case " + std::to_string(static_cast<long long>(v.v.empty() ? 0 : v.v[0])) + ":");
                } else if (c->kind == GdStmtKind::Default) line("default:");
                else stmtLines(*c);
            }
            --ind_;
            line("}");
            break;
        }
        case GdStmtKind::Case: case GdStmtKind::Default: break;
        case GdStmtKind::Break: line("break;"); break;
        case GdStmtKind::Continue: line("continue;"); break;
        case GdStmtKind::Return: line(s.expr ? "return " + expr(*s.expr) + ";" : std::string("return;")); break;
        case GdStmtKind::Discard: line("discard;"); break;
        }
    }
    void body(const GdStmt& s) {
        if (s.kind == GdStmtKind::Block) { stmtLines(s); return; }
        line("{");
        ++ind_;
        stmtLines(s);
        --ind_;
        line("}");
    }

    void emitFunction(const GdFunction& f) {
        curProc_ = f.processor;
        std::string sig;
        if (f.processor != GdProcessor::None) {
            const std::string entry = opt_.nameMacro.empty() ? opt_.entryPrefix + "_" + gdProcessorName(f.processor) : gn(opt_.entryPrefix + "_" + gdProcessorName(f.processor));
            r_.entries.push_back(entry);
            sig = "void " + entry + "()";
        } else {
            if (f.ret.isArray()) { unsupported(nullptr, "function '" + f.name + "' returns an array, which HLSL cannot return by value"); return; }
            sig = typeName(f.ret) + " " + gn("gdf_" + f.name) + "(";
            for (usize i = 0; i < f.params.size(); ++i) {
                const GdParam& p = f.params[i];
                const char* q = p.qual == GdParamQual::Out ? "out " : p.qual == GdParamQual::InOut ? "inout " : "in ";
                sig += (i ? ", " : "") + std::string(q) + declare(p.type, lname(p.name));
            }
            sig += ")";
        }
        line(sig);
        stmtLines(*f.body);
        curProc_ = GdProcessor::None;
    }
};

}  // namespace

void gdShaderLayoutUniforms(const GdShaderModule& m, GdUniformBlock& normal, GdUniformBlock& instance, GdUniformBlock& global) {
    layoutBlock(m, GdUniformScope::Normal, normal);
    layoutBlock(m, GdUniformScope::Instance, instance);
    layoutBlock(m, GdUniformScope::Global, global);
}

GdHlslResult gdShaderEmitHlsl(const GdShaderModule& m, const GdHlslOptions& opt) {
    GdHlslResult r;
    Emitter e(m, opt, r);
    e.run();
    return r;
}

}  // namespace aver::fmt::godot
