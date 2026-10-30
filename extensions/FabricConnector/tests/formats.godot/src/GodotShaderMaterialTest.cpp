// GodotShaderMaterialTest -- Fabric G6 SH2: a Godot spatial shader on an Aver material, checked headless.
//
// Part 1 is plain logic: the engine-side copy of the helper library equals the front-end's, the hlslfx bits agree, the EmbeddedHlsl graph node round-trips
// through the .ocgraph text format, the uniform defaults land in the right registers, samplers take the right texture slots, render modes become the
// right material state, and a registry without a converted shader produces the generated code it always did.
// Part 2 compiles the engine's real translation unit (shared prelude + material prelude + the registry's generated code + a test entry) with DXC, to DXIL
// and to SPIR-V, for the raster pixel stage, the vertex stage (VSMainEmb's hook) and the mesh-extra twins. No device, no GPU, nothing is run.
#include "GdGraphBridge.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/pbr/MaterialFx.hpp"
#include "aver/pbr/MaterialGraphHlsl.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/DxcApi.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

using namespace aver;
using namespace aver::fmt::godot;
using rhi::ComPtr;
namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// shaderFile() is a flat namespace; stage every module's shaders into one temp directory so the test does not depend on a stale bin/shaders deploy.
static std::string stageShaders() {
    const fs::path root = fs::path(AVER_REPO_ROOT) / "modules";
    const fs::path dst = fs::temp_directory_path() / "AverGodotShaderMaterialTestShaders";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const char* mod : {"rhi", "render.pbr", "render.voxi"}) {
        for (const auto& e : fs::directory_iterator(root / mod / "shaders", ec)) {
            const std::string ext = e.path().extension().string();
            if (e.is_regular_file() && (ext == ".hlsl" || ext == ".hlsli")) fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst.string();
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string noCr(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// ------------------------------------------------------------------------------------------------------------------------------ the shaders

// Every fragment output the engine maps, samplers of each state, uniforms of each type, an instance and a global uniform, a screen texture (not bindable),
// a varying read and discard.
static const char* kOutputs = R"(shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, specular_schlick_ggx;
uniform vec4 tint : source_color = vec4(0.8, 0.4, 0.2, 1.0);
uniform float amount : hint_range(0.0, 1.0) = 0.5;
uniform int steps = 3;
uniform bool use_rim = true;
uniform vec3 emit_col : source_color = vec3(0.1, 0.2, 0.3);
uniform mat3 xf = mat3(1.0);
uniform vec2 scroll = vec2(0.1, 0.2);
uniform float weights[3];
uniform sampler2D albedo_tex : source_color, filter_nearest, repeat_enable;
uniform sampler2D mask_tex : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D normal_tex : hint_normal;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
instance uniform float inst_gain : hint_range(0.0, 2.0) = 1.0;
global uniform float wind_strength;
varying vec3 v_pos;
void vertex() {
    v_pos = VERTEX;
}
void fragment() {
    vec2 uv = UV + scroll * TIME;
    vec4 a = texture(albedo_tex, uv);
    float m = texture(mask_tex, uv * 2.0).r;
    ALBEDO = a.rgb * tint.rgb * amount;
    ALPHA = a.a * tint.a * m;
    ALPHA_SCISSOR_THRESHOLD = 0.3;
    METALLIC = float(steps) * 0.1;
    ROUGHNESS = 0.4 + weights[1] + wind_strength * 0.0;
    SPECULAR = 0.6;
    EMISSION = emit_col * inst_gain * (use_rim ? 1.0 : 0.0) * (xf * vec3(1.0)).x;
    NORMAL_MAP = texture(normal_tex, uv).rgb;
    NORMAL_MAP_DEPTH = 0.8;
    AO = m;
    RIM = 0.5;
    RIM_TINT = 0.2;
    CLEARCOAT = 0.4;
    CLEARCOAT_GLOSS = 0.8;
    ANISOTROPY = 0.3;
    SSS_STRENGTH = 0.2;
    SSS_TRANSMITTANCE_COLOR = vec4(1.0, 0.5, 0.5, 1.0);
    SSS_TRANSMITTANCE_DEPTH = 1.0;
    BACKLIGHT = vec3(0.1);
    vec3 sc = texture(screen_tex, SCREEN_UV).rgb;
    FOG = vec4(sc, 0.0);
    float h = v_pos.y;
    ALBEDO += vec3(h * 0.0);
    if (a.a < 0.01) {
        discard;
    }
}
)";

static const char* kNormalWrite = R"(shader_type spatial;
void fragment() {
    NORMAL = normalize(NORMAL + vec3(0.0, 0.2, 0.0));
    ROUGHNESS = 0.3;
}
)";

static const char* kVertexModel = R"(shader_type spatial;
uniform float wave = 0.2;
uniform sampler2D height_tex;
varying vec2 v_uv;
varying flat int v_id;
varying vec3 v_world[2];
void vertex() {
    vec4 h = textureLod(height_tex, UV, 0.0);
    VERTEX.y += h.r * wave + sin(TIME + VERTEX.x) * wave;
    NORMAL = normalize(NORMAL + vec3(0.0, wave, 0.0));
    UV = UV * 2.0;
    UV2 = UV2 + vec2(0.1);
    COLOR = vec4(1.0, 0.5, 0.2, 1.0);
    TANGENT = normalize(TANGENT);
    v_uv = UV;
    v_id = INSTANCE_ID;
    v_world[0] = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    mat4 mv = VIEW_MATRIX * MODEL_MATRIX;
    v_world[1] = NODE_POSITION_WORLD + CAMERA_POSITION_WORLD + (INV_VIEW_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz + (mv * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
}
void fragment() {
    ALBEDO = vec3(v_uv, float(v_id)) + v_world[0] * 0.0 + v_world[1] * 0.0;
}
)";

static const char* kVertexWorld = R"(shader_type spatial;
render_mode world_vertex_coords;
uniform float lift = 1.0;
void vertex() {
    VERTEX.y += lift * sin(VERTEX.x + TIME);
}
void fragment() {
    ALBEDO = vec3(0.2, 0.6, 0.9);
}
)";

static const char* kVertexSkip = R"(shader_type spatial;
render_mode skip_vertex_transform;
void vertex() {
    VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
    NORMAL = normalize(MODELVIEW_NORMAL_MATRIX * NORMAL);
}
void fragment() {
    ALBEDO = NORMAL * 0.5 + 0.5;
}
)";

// Reads every built-in the engine supplies, in both stages.
static const char* kReadsEverything = R"(shader_type spatial;
void vertex() {
    vec4 q = INSTANCE_CUSTOM + CUSTOM0 + CUSTOM1 + CUSTOM2 + CUSTOM3 + BONE_WEIGHTS;
    uvec4 bi = BONE_INDICES;
    float f = float(VERTEX_ID + VIEW_INDEX + VIEW_MONO_LEFT + VIEW_RIGHT + INSTANCE_ID) + float(CAMERA_VISIBLE_LAYERS) + float(bi.x);
    vec3 e = EYE_OFFSET + NODE_POSITION_VIEW + CAMERA_DIRECTION_WORLD;
    mat4 m = MAIN_CAM_INV_VIEW_MATRIX * INV_PROJECTION_MATRIX * PROJECTION_MATRIX * MODELVIEW_MATRIX;
    mat3 n = MODEL_NORMAL_MATRIX * MODELVIEW_NORMAL_MATRIX;
    vec2 vs = VIEWPORT_SIZE;
    bool s = OUTPUT_IS_SRGB;
    VERTEX += vec3(q.x * 0.0 + f * 0.0 + e.x * 0.0 + m[0][0] * 0.0 + n[0][0] * 0.0 + vs.x * 0.0 + (s ? 0.0 : 0.0));
    ROUGHNESS = 0.5;
    POINT_SIZE = 1.0;
}
void fragment() {
    vec4 fc = FRAGCOORD;
    bool ff = FRONT_FACING;
    vec3 vw = VIEW;
    vec2 pc = POINT_COORD;
    vec2 su = SCREEN_UV;
    vec3 lv = LIGHT_VERTEX;
    vec4 col = COLOR;
    vec2 u2 = UV2;
    vec3 bn = BINORMAL;
    vec3 tg = TANGENT;
    vec3 fv = VERTEX;
    mat4 mm = MODEL_MATRIX * VIEW_MATRIX * INV_VIEW_MATRIX * INV_PROJECTION_MATRIX * PROJECTION_MATRIX;
    vec3 np = NODE_POSITION_WORLD + NODE_POSITION_VIEW + CAMERA_POSITION_WORLD + CAMERA_DIRECTION_WORLD + EYE_OFFSET;
    ALBEDO = col.rgb * fc.xyz * 0.0 + vw * (ff ? 1.0 : 0.5) + vec3(pc, su.x) * 0.0 + bn * 0.0 + tg * 0.0 + fv * 0.0 + np * 0.0 + vec3(mm[0][0] * 0.0);
    ALPHA_HASH_SCALE = 1.0;
    ALPHA_ANTIALIASING_EDGE = 0.1;
    ALPHA_TEXTURE_COORDINATE = u2;
    PREMUL_ALPHA_FACTOR = 0.0;
    AO_LIGHT_AFFECT = 0.3;
    ANISOTROPY_FLOW = vec2(1.0, 0.0);
    SSS_TRANSMITTANCE_BOOST = 1.0;
    BENT_NORMAL_MAP = vec3(0.5, 0.5, 1.0);
    LIGHT_VERTEX = lv;
    DEPTH = 0.5;
    RADIANCE = vec4(0.0);
    IRRADIANCE = vec4(0.0);
}
)";

// Two shaders that share every global name (a function, a constant, a struct, a varying): the registry must keep them apart.
static const char* kClashA = R"(shader_type spatial;
uniform float k = 1.0;
const float C = 2.0;
struct S { float a; vec3 b; };
float f(float x) { return x * C; }
varying vec3 vv;
void vertex() { vv = VERTEX; }
void fragment() { S s = S(f(k), vv); ALBEDO = s.b * s.a; }
)";
static const char* kClashB = R"(shader_type spatial;
uniform float k = 3.0;
const float C = 5.0;
struct S { float a; vec3 b; };
float f(float x) { return x + C; }
varying vec3 vv;
void vertex() { vv = NORMAL; }
void fragment() { S s = S(f(k), vv); ALBEDO = s.b + s.a; ALPHA = 0.5; }
)";

// ------------------------------------------------------------------------------------------------------------------------------ DXC

namespace {
struct DxcLib {
    void* dll = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    bool ok() const { return compiler != nullptr; }
};

// Fills l in place: off Windows ComPtr can be neither copied nor moved.
void loadDxc(const fs::path& path, DxcLib& l) {
    if (!fs::exists(path)) return;
    l.dll = loadLibrary(reinterpret_cast<const char*>(path.u8string().c_str()));
    if (!l.dll) return;
    auto create = reinterpret_cast<DxcCreateInstanceProc>(librarySymbol(l.dll, "DxcCreateInstance"));
    if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&l.utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&l.compiler)))) {
        l.utils.Reset();
        l.compiler.Reset();
    }
}

std::vector<std::wstring> splitDefines(const std::string& all) {
    std::vector<std::wstring> out;
    for (usize b = 0; b <= all.size();) {
        const usize e = all.find(';', b) == std::string::npos ? all.size() : all.find(';', b);
        if (e > b) out.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(b), all.begin() + static_cast<std::ptrdiff_t>(e));
        b = e + 1;
    }
    return out;
}

bool compileHlsl(const DxcLib& d, const std::string& source, const char* entry, char stage, bool spirv, const std::string& defines, std::string& err) {
    const wchar_t* prefix = stage == 'v' ? L"vs" : L"ps";
    const std::wstring target = std::wstring(prefix) + L"_6_5";
    const std::wstring wentry(entry, entry + std::strlen(entry));
    const std::vector<std::wstring> defs = splitDefines(defines);
    std::vector<LPCWSTR> args = {L"-E", wentry.c_str(), L"-T", target.c_str(), L"-Zpr", L"-HV", L"2021", L"-I", L"."};
    const std::wstring shift = L"28";
    if (spirv) {
        args.push_back(L"-spirv"); args.push_back(L"-fspv-target-env=vulkan1.3");
        args.push_back(L"-fvk-u-shift"); args.push_back(shift.c_str()); args.push_back(L"0");
        if (stage == 'v') args.push_back(L"-fvk-invert-y");
    } else {
        args.push_back(L"-Vd");
    }
    for (const std::wstring& w : defs) { args.push_back(L"-D"); args.push_back(w.c_str()); }
    DxcBuffer buf{source.c_str(), source.size(), DXC_CP_UTF8};
    rhi::DxcShaderInclude includes(d.utils.Get());
    ComPtr<IDxcResult> result;
    HRESULT hr = d.compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), &includes, IID_PPV_ARGS(&result));
    if (SUCCEEDED(hr) && result) result->GetStatus(&hr);
    ComPtr<IDxcBlobUtf8> errs;
    err.clear();
    if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength()) err.assign(errs->GetStringPointer(), errs->GetStringLength());
    if (FAILED(hr)) { if (err.empty()) err = "DXC failed with HRESULT " + std::to_string(static_cast<long>(hr)); return false; }
    ComPtr<IDxcBlob> obj;
    const bool got = SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj && obj->GetBufferSize() > 0;
    if (!got && err.empty()) err = "DXC produced no object";
    return got;
}

const char* kEntries = R"(
float4 PSGdTest(VSOut i) : SV_Target {
#ifdef AVER_CUSTOM_VARYINGS_ON
    averEmbLoadVaryings(i);
#endif
    AverVertex vtx = averVertexOf(i);
    AverLight sun;
    sun.direction  = normalize(float3(0.3, 0.4, 0.9));
    sun.radiance   = float3(3.0, 3.0, 3.0);
    sun.visibility = 1.0;
    AverSurface s = averEvalMaterial(vtx, sun);
    float3 lit = averShadeDirect(float3(0, 0, 0), s, sun);
    return float4(lit, averOpacity(s));
}
#if AVER_MATX_EMBVS
VSOut VSGdTest(VSIn i) {
    VSOut o = VSMain(i);
    averEmbVertex(i, o);
    return o;
}
#endif
)";

GdEngineShader prepare(const char* src, const char* name) {
    GdEngineShader s;
    GdEngineOptions o;
    o.name = name;
    gdShaderForEngine(src, std::string(name) + ".gdshader", o, s);
    return s;
}
}  // namespace

int main() {
    AVER_INFO("=== GodotShaderMaterialTest ===");
    rhi::setShaderSourceDir(stageShaders());
    pbr::materialGraphs().clear();

    // ---------------------------------------------------------------------------------------------------------------- part 1: logic
    {
        const std::string engineLib = noCr(readFile(fs::path(AVER_REPO_ROOT) / "modules" / "render.pbr" / "shaders" / "material_embedded_lib.hlsl"));
        check(!engineLib.empty() && engineLib == noCr(gdShaderHlslLibrary()), "material_embedded_lib.hlsl is exactly gdShaderHlslLibrary() (regenerate with GodotShaderTest --library)");
        check(fmt::hlslfx::kUnshaded == 1u && fmt::hlslfx::kAmbientDisabled == 2u && fmt::hlslfx::kSpecularDisabled == 4u && fmt::hlslfx::kDiffuseWrap == 8u &&
              fmt::hlslfx::kWorldVertexCoords == 16u && fmt::hlslfx::kSkipVertexTransform == 32u && fmt::hlslfx::kUsesNormalWrite == 64u,
              "the hlslfx bits are the ones the hook prelude reads (1 unshaded, 2 ambient, 4 specular, 8 wrap, 16 world vertex, 32 skip transform, 64 normal write)");
    }
    {
        // The node text survives the line format: spaces, '#', '%', newlines, a source longer than one chunk.
        fmt::EmbeddedHlslPart p;
        p.name = "Wavy Water #1 100%";
        p.ns = "gs12345678_";
        p.regsU = 5; p.regsI = 2; p.regsG = 1; p.fx = 0x2Bu;
        p.hasFragment = true; p.hasVertex = true;
        std::string big;
        for (int i = 0; i < 400; ++i) big += "float4 x" + std::to_string(i) + " = float4(1.0, 2.0, 3.0, 4.0); // a # comment, 100% \t tabs\r\n";
        p.fragmentSource = big;
        p.vertexSource = "void v() { }\n";
        p.varyings.push_back(fmt::EmbeddedHlslVarying{"float3 gs12345678_gdv_a", false, 1});
        p.varyings.push_back(fmt::EmbeddedHlslVarying{"int gs12345678_gdv_b", true, 1});
        p.varyings.push_back(fmt::EmbeddedHlslVarying{"float3 gs12345678_gdv_c[2]", false, 2});
        fmt::OcGraphData g;
        fmt::packEmbeddedHlsl(p, g);
        const std::string text = fmt::writeOcgraph(g);
        check(text.find("DOMAIN material") != std::string::npos && text.find("EmbeddedHlsl") != std::string::npos && text.find("GodotShader") == std::string::npos, "the packed graph names its domain and node");
        fmt::OcGraphData back;
        std::string err;
        check(fmt::parseOcgraph(text, back, &err), "the packed graph parses back (" + err + ")");
        fmt::EmbeddedHlslPart q;
        check(fmt::unpackEmbeddedHlsl(back, q, &err), "the EmbeddedHlsl node unpacks (" + err + ")");
        check(q.name == p.name && q.ns == p.ns && q.regsU == 5 && q.regsI == 2 && q.regsG == 1 && q.fx == 0x2Bu && q.hasFragment && q.hasVertex, "the node's scalar attributes round-trip");
        check(q.fragmentSource == big && q.vertexSource == p.vertexSource, "a 20 KB source with spaces, '#', '%', tabs and CRLF round-trips through the text format");
        check(q.varyings.size() == 3 && q.varyings[1].flat && q.varyings[2].rows == 2 && q.varyings[2].decl == "float3 gs12345678_gdv_c[2]", "the varyings round-trip");
        std::string ignored;
        check(!fmt::hlslGraphUnescape("%4", ignored) && !fmt::hlslGraphUnescape("%zz", ignored), "a malformed escape is refused");
    }
    {
        // Defaults land in the right registers; every uniform type is packed the way the emitter reads it.
        const GdEngineShader s = prepare(kOutputs, "Outputs");
        check(s.ok, "the outputs shader prepares for the engine (" + s.error + ")");
        check(s.hasFragment && s.hasVertex && !s.hasLight, "it has a vertex() and a fragment() and no light()");
        // layout: tint (reg 0), amount (reg 1.x), steps (1.y), use_rim (1.z), emit_col (reg 2), xf (regs 3-5), scroll (reg 6.xy), weights (regs 7-9)
        check(s.uniforms.registerCount == 10, "the normal block uses 10 registers (got " + std::to_string(s.uniforms.registerCount) + ")");
        check(s.instanceUniforms.registerCount == 1 && s.globalUniforms.registerCount == 1, "one instance and one global register");
        check(s.defaultRegisters.size() == 12u * 4u, "the table is 12 registers (normal 10 + instance 1 + global 1)");
        const auto f = [&](usize reg, usize comp) { return s.defaultRegisters[reg * 4 + comp]; };
        const auto bitsInt = [&](usize reg, usize comp) { int v; std::memcpy(&v, &s.defaultRegisters[reg * 4 + comp], sizeof v); return v; };
        const auto lin = [](double c) { return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); };
        check(std::abs(f(0, 0) - lin(0.8)) < 1e-6f && std::abs(f(0, 1) - lin(0.4)) < 1e-6f && std::abs(f(0, 2) - lin(0.2)) < 1e-6f && f(0, 3) == 1.0f,
              "tint (source_color vec4) default is decoded to linear in register 0, alpha as written");
        check(f(1, 0) == 0.5f, "amount (float) default in register 1.x");
        check(bitsInt(1, 1) == 3, "steps (int) is the 32-bit pattern 3 in register 1.y");
        check(bitsInt(1, 2) == 1, "use_rim (bool) is 1 in register 1.z");
        check(std::abs(f(2, 0) - lin(0.1)) < 1e-6f && std::abs(f(2, 1) - lin(0.2)) < 1e-6f && std::abs(f(2, 2) - lin(0.3)) < 1e-6f, "emit_col (source_color vec3) default, decoded, in register 2");
        check(f(3, 0) == 1.0f && f(4, 1) == 1.0f && f(5, 2) == 1.0f && f(3, 1) == 0.0f, "xf (mat3) default is the identity, one column per register");
        check(f(6, 0) == 0.1f && f(6, 1) == 0.2f, "scroll (vec2) default in register 6.xy");
        check(f(7, 0) == 0.0f && f(8, 0) == 0.0f && f(9, 0) == 0.0f, "weights (float[3], no initialiser) takes one register per element, zero");
        {   // gdPackUniform writes an array element into its own register
            std::vector<float> regs(12 * 4, 0.0f);
            GdUniform wu;
            wu.type.base = GdBase::Float;
            wu.type.arraySize = 3;
            GdUniformSlot ws;
            ws.reg = 7; ws.comp = 0; ws.registers = 3; ws.elementStride = 1;
            gdPackUniform(ws, wu, {0.2, 0.3, 0.5}, regs);
            check(regs[7 * 4] == 0.2f && regs[8 * 4] == 0.3f && regs[9 * 4] == 0.5f && regs[7 * 4 + 1] == 0.0f, "gdPackUniform puts float[3] elements in registers 7, 8 and 9 (x only)");
        }
        check(f(10, 0) == 1.0f && f(11, 0) == 0.0f, "inst_gain (instance) default in register 10, the global's zero default in register 11");
        // sampler -> slot assignment
        check(s.samplers.size() == 4, "four sampler uniforms");
        const auto slotOf = [&](const char* n) { for (const auto& x : s.samplers) if (x.name == n) return x.slot; return -9; };
        check(slotOf("albedo_tex") == 0, "the first source_color texture takes the base colour slot (0)");
        check(slotOf("mask_tex") == 1 && slotOf("normal_tex") == 3, "linear textures take 1 then 3");
        check(slotOf("screen_tex") >= 0 && !s.samplers[3].bindable, "hint_screen_texture is not bindable and takes a spare slot that stays unbound");
        check(slotOf("albedo_tex") != 7 && slotOf("mask_tex") != 7 && slotOf("normal_tex") != 7 && slotOf("screen_tex") != 7, "no sampler takes slot 7, the uniform table's");
        bool screenNote = false, instanceNote = false, rimNote = false, fogNote = false;
        for (const auto& n : s.notes) {
            if (n.subject == "uniform screen_tex" && n.severity == "approximated") screenNote = true;
            if (n.subject == "instance uniforms") instanceNote = true;
            if (n.subject == "RIM") rimNote = true;
            if (n.subject == "FOG" && n.severity == "unsupported") fogNote = true;
        }
        check(screenNote && instanceNote && rimNote && fogNote, "the report names the screen texture, the instance uniform, RIM (approximated) and FOG (unsupported)");
        check(s.writesAlpha && s.writesAlphaScissor && s.usesDiscard, "it writes ALPHA and ALPHA_SCISSOR_THRESHOLD and discards");
        check(s.cull == 3 && s.depthDraw == 1 && s.blend == 0, "render_mode cull_disabled, depth_draw_always, blend_mix");
        check(!s.varyings.empty() && s.varyings[0].decl.find("gdv_v_pos") != std::string::npos, "the varying is a prefixed member");
        // the slot sampler constructors carry filter, repeat, mips and the unbound colour
        check(s.fragmentSource.find("EMBN(gdsm_albedo_tex)") != std::string::npos && s.fragmentSource.find("r.slot = 0u; r.m = 3u;") != std::string::npos,
              "albedo_tex is slot 0 with mode 3 (nearest, repeat; no mipmaps)");
        check(s.fragmentSource.find("r.slot = 1u; r.m = 256u;") != std::string::npos, "mask_tex (filter_linear, no mipmaps) is mode 256: unbound reads black");
        check(s.fragmentSource.find("r.m = 772u;") != std::string::npos, "normal_tex is mode 4 | 768 (unbound reads the flat normal)");
    }
    {
        // Render modes -> material state.
        const GdEngineShader a = prepare("shader_type spatial;\nrender_mode unshaded, blend_add, depth_test_disabled, shadows_disabled, specular_disabled, ambient_light_disabled, cull_front;\nvoid fragment() { ALBEDO = vec3(1.0); }\n", "Modes");
        check(a.ok && a.blend == 1 && a.cull == 2 && a.depthTestDisabled && a.shadowsDisabled, "blend_add, cull_front, depth_test_disabled, shadows_disabled");
        check((a.fxBits & fmt::hlslfx::kUnshaded) && (a.fxBits & fmt::hlslfx::kSpecularDisabled) && (a.fxBits & fmt::hlslfx::kAmbientDisabled), "unshaded, specular_disabled and ambient_light_disabled become hlslfx bits");
        const GdEngineShader b = prepare("shader_type spatial;\nrender_mode world_vertex_coords, vertex_lighting, fog_disabled, diffuse_toon, specular_toon, depth_prepass_alpha;\nvoid vertex() { VERTEX.x += 1.0; }\n", "Modes2");
        bool vl = false, fd = false, dt = false;
        for (const auto& n : b.notes) { if (n.subject == "render_mode vertex_lighting") vl = true; if (n.subject == "render_mode fog_disabled" && n.severity == "unsupported") fd = true; if (n.subject == "render_mode diffuse_toon") dt = true; }
        check(b.ok && b.hasVertex && !b.hasFragment && (b.fxBits & fmt::hlslfx::kWorldVertexCoords) && vl && fd && dt, "a vertex-only shader prepares; vertex_lighting / diffuse_toon approximated, fog_disabled unsupported");
        const GdEngineShader c = prepare("shader_type spatial;\nuniform sampler2D a : source_color;\nuniform sampler2D b : source_color;\nuniform sampler2D c : source_color;\nuniform sampler2D d : source_color;\nvoid fragment() { ALBEDO = texture(a, UV).rgb; }\n", "TooMany");
        check(!c.ok && c.error.find("sRGB") != std::string::npos, "four source_color textures do not fit the three sRGB slots (" + c.error + ")");
        const GdEngineShader d = prepare("shader_type particles;\nvoid process() { }\n", "Particles");
        check(!d.ok && d.error.find("particles") != std::string::npos, "a particles shader is refused with a reason");
        const GdEngineShader e = prepare("shader_type spatial;\nuniform samplerCube c;\nvoid fragment() { ALBEDO = texture(c, vec3(0.0, 0.0, 1.0)).rgb; }\n", "Cube");
        check(!e.ok && e.error.find("sampler2D") != std::string::npos, "a samplerCube uniform is refused with a reason");
        const GdEngineShader f2 = prepare("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(undefined_name); }\n", "Broken");
        check(!f2.ok && f2.error.find("undefined_name") != std::string::npos, "a shader error is reported with its message (" + f2.error + ")");
        const GdEngineShader g = prepare("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(1.0); }\nvoid light() { DIFFUSE_LIGHT = vec3(1.0); }\n", "HasLight");
        bool lightNote = false;
        for (const auto& n : g.notes) if (n.subject == "light()" && n.severity == "unsupported") lightNote = true;
        check(g.ok && g.hasLight && lightNote, "light() is reported unsupported and does not stop the shader");
    }
    {
        // A graph without a converted shader generates what it always did; the Godot machinery appears only with one.
        pbr::MaterialGraphRegistry plain;
        fmt::OcGraphData g;
        g.name = "M_Flat";
        g.domain = "material";
        fmt::OcGraphNode c;
        c.id = "colour"; c.type = "ConstFloat3";
        fmt::OcGraphPin cp; cp.name = "value"; cp.type = "float3"; cp.isOutput = true; cp.defaultValue = "0.85,0.16,0.10";
        c.pins.push_back(cp);
        g.nodes.push_back(c);
        fmt::OcGraphNode out;
        out.id = "out"; out.type = "MaterialOutput";
        fmt::OcGraphPin ip; ip.name = "BaseColor"; ip.type = "float3"; ip.isOutput = false;
        out.pins.push_back(ip);
        g.nodes.push_back(out);
        fmt::OcGraphLink l; l.sourceNode = "colour"; l.sourcePin = "value"; l.destNode = "out"; l.destPin = "BaseColor";
        g.links.push_back(l);
        const u32 id = plain.add("flat.ocgraph", "M_Flat", g);
        check(id == 1 && !plain.hlsl().empty(), "a stock graph registers and generates code");
        check(plain.hlsl().find("AVER_CUSTOM") == std::string::npos && plain.hlsl().find("embFragment") == std::string::npos && plain.hlsl().find("EmbeddedHlsl") == std::string::npos,
              "no Godot text appears in the generated code of a registry without a converted shader");
        check(!plain.hasEmbeddedShaders() && !plain.hasEmbeddedVaryings() && plain.vsOutMembersDefine().empty(), "such a registry reports no embedded shader and no varyings");
        check(plain.hlsl().rfind("// ---- generated from .ocgraph material graphs; do not edit ----\nAverSurface averEvalMaterial(AverVertex v, AverLight l) {\n", 0) == 0,
              "its generated code still opens with the stock averEvalMaterial");
        const std::string once = plain.hlsl();
        check(plain.add("flat.ocgraph", "M_Flat", g) == 1 && plain.hlsl() == once, "re-registering the same graph keeps its id and its code");
    }
    {
        // Variant keys: the new bits stay out of the pixel mask and mark the vertex stage and late Z.
        pbr::MaterialFx fx;
        check(!fx.active() && pbr::fxVariantKey(fx) == 0, "an untouched fx still has key 0");
        fx.embVertex = true;
        check(fx.active() && (pbr::fxVariantKey(fx) & pbr::fxkey::kEmbVertex) && !(pbr::fxVariantKey(fx) & pbr::fxkey::kShaderMask), "embVertex sets kEmbVertex only");
        check((pbr::kFxNoPrepassKeys & pbr::fxkey::kEmbVertex) != 0, "a vertex-shaded draw never joins the depth prepass");
        pbr::MaterialFx dx;
        dx.embDiscard = true;
        check(dx.active() && (pbr::fxVariantKey(dx) & pbr::fxkey::kLateZ), "a discarding shader compiles without forced early depth");
    }

    // ---------------------------------------------------------------------------------------------------------------- part 2: compile
    const fs::path dxilPath = fs::path(executableDir()) / "dxcompiler.dll";
#if defined(_WIN32)
    const fs::path spirvPath = fs::path(AVER_REPO_ROOT) / "third_party" / "dxc-spirv" / "dxcompiler.dll";
#elif defined(__APPLE__)
    const fs::path spirvPath = fs::path(executableDir()) / "libdxcompiler.dylib";
#else
    const fs::path spirvPath = fs::path(executableDir()) / "libdxcompiler.so";
#endif
    DxcLib dxil, spirv;
    loadDxc(dxilPath, dxil);
    loadDxc(spirvPath, spirv);
    if (!dxil.ok()) AVER_WARN("  SKIP  the DXIL compiles: no dxcompiler.dll beside the test");
    if (!spirv.ok()) AVER_WARN("  SKIP  the SPIR-V compiles: no SPIR-V-capable dxcompiler.dll");

    const std::string mat = pbr::materialShaderDefines(25, 2, /*layeredBsdf=*/false);
    const std::string matLayered = pbr::materialShaderDefines(25, 2, /*layeredBsdf=*/true);

    // Registers a set of shaders and compiles the composed translation unit for the raster pixel stage, the vertex stage and their twins.
    const auto compileSet = [&](const std::string& label, const std::vector<std::pair<const char*, const char*>>& shaders) {
        pbr::materialGraphs().clear();
        std::vector<GdEngineShader> prepared;
        for (const auto& sh : shaders) {
            GdEngineShader s = prepare(sh.first, sh.second);
            check(s.ok, label + ": " + sh.second + " prepares (" + s.error + ")");
            if (!s.ok) return;
            prepared.push_back(std::move(s));
        }
        for (const GdEngineShader& s : prepared) {
            const u32 id = gdtest::registerShader(s, std::string(s.name) + ".ocgraph");
            check(id != 0, label + ": " + s.name + " registers (id " + std::to_string(id) + ")");
            if (!id) return;
        }
        check(pbr::materialGraphs().hasEmbeddedShaders(), label + ": the registry reports a converted shader");
        const std::string unit = gdtest::compose(kEntries);
        struct Row { const char* name; const char* entry; char stage; std::string defines; };
        const bool vary = pbr::materialGraphs().hasEmbeddedVaryings();
        const std::string va = vary ? ";AVER_CUSTOM_VARYINGS_ON=1" : "";
        const std::vector<Row> rows = {
            {"raster pixel", "PSGdTest", 'p', mat},
            {"raster pixel, layered BSDF", "PSGdTest", 'p', matLayered},
            {"raster pixel, fx variant", "PSGdTest", 'p', mat + ";AVER_MATX=1;AVER_MATX_LATEZ=1" + va},
            {"raster pixel, mesh extra", "PSGdTest", 'p', mat + ";AVER_MESH_EXTRA=1" + va},
            {"vertex stage (VSMainEmb's hook)", "VSGdTest", 'v', mat + ";AVER_MATX=1;AVER_MATX_EMBVS=1;AVER_CUSTOM_NO_DERIVATIVES=1" + va},
            {"vertex stage, mesh extra", "VSGdTest", 'v', mat + ";AVER_MATX=1;AVER_MATX_EMBVS=1;AVER_CUSTOM_NO_DERIVATIVES=1;AVER_MESH_EXTRA=1" + va},
        };
        {   // ActorPreview and the editor's cluster path compose the same text without the fx fallback define and without the varyings: it must still compile
            std::string alt = rhi::sharedShaderPrelude();
            alt += "\n#define AVER_MATERIAL_GRAPH 1\n";
            alt += pbr::materialShaderPrelude();
            alt += pbr::materialGraphs().hlsl();
            alt += kEntries;
            if (dxil.ok()) { std::string err; const bool ok = compileHlsl(dxil, alt, "PSGdTest", 'p', false, mat, err); check(ok, label + ": the ActorPreview-style composition compiles to DXIL" + (ok ? "" : " -- " + err.substr(0, 1500))); }
            if (spirv.ok()) { std::string err; const bool ok = compileHlsl(spirv, alt, "PSGdTest", 'p', true, mat, err); check(ok, label + ": the ActorPreview-style composition compiles to SPIR-V" + (ok ? "" : " -- " + err.substr(0, 1500))); }
        }
        if (std::getenv("AVER_EMB_DUMP")) {   // the composed unit, for reading a DXC error against
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "AverGodotShaderMaterialTest";
            fs::create_directories(dir, ec);
            std::string safe = label;
            for (char& c : safe) if (c == ' ') c = '_';
            std::ofstream(dir / (safe + ".hlsl"), std::ios::binary) << unit;
        }
        for (const Row& r : rows) {
            if (dxil.ok()) {
                std::string err;
                const bool ok = compileHlsl(dxil, unit, r.entry, r.stage, false, r.defines, err);
                check(ok, label + ": " + r.name + " compiles to DXIL" + (ok || err.empty() ? "" : " -- " + err.substr(0, 1500)));
            }
            if (spirv.ok()) {
                std::string err;
                const bool ok = compileHlsl(spirv, unit, r.entry, r.stage, true, r.defines, err);
                check(ok, label + ": " + r.name + " compiles to SPIR-V" + (ok || err.empty() ? "" : " -- " + err.substr(0, 1500)));
            }
        }
    };

    compileSet("outputs", {{kOutputs, "Outputs"}});
    compileSet("normal write", {{kNormalWrite, "NormalWrite"}});
    compileSet("vertex model space", {{kVertexModel, "VertexModel"}});
    compileSet("vertex world space", {{kVertexWorld, "VertexWorld"}});
    compileSet("vertex skip transform", {{kVertexSkip, "VertexSkip"}});
    compileSet("reads everything", {{kReadsEverything, "ReadsEverything"}});
    compileSet("name clash", {{kClashA, "ClashA"}, {kClashB, "ClashB"}});
    compileSet("all together", {{kOutputs, "Outputs"}, {kVertexModel, "VertexModel"}, {kVertexSkip, "VertexSkip"}, {kClashA, "ClashA"}});

    {
        // The uniform table is read in the vertex stage and the fragment stage from slot 7; its texel layout is one register per texel.
        pbr::materialGraphs().clear();
        GdEngineShader s = prepare(kOutputs, "Outputs");
        gdtest::registerShader(s, "Outputs.ocgraph");
        const std::string& h = pbr::materialGraphs().hlsl();
        check(h.find("embLoadReg(k)") != std::string::npos && h.find("embrLoadReg(k)") != std::string::npos, "the arms load the table in both flavours");
        check(h.find("embFragmentEnd(a, v,") != std::string::npos && h.find("embPost(s, v, l, gEmbFx)") != std::string::npos, "the arms map the context onto the AverAuthored and run the post step");
        check(h.find("#define EMB_NS_CUR " + s.ns + "R_") != std::string::npos && h.find("#define EMB_NS_CUR " + s.ns + "T_") != std::string::npos, "the converted source is included once per flavour under its own name space");
        const std::string var = pbr::materialGraphs().vsOutMembersDefine();
        check(var.find("TEXCOORD4") != std::string::npos, "the varyings start at TEXCOORD4, after the engine's own interpolants");
    }
    pbr::materialGraphs().clear();

    if (g_failures) {
        AVER_ERROR("=== GodotShaderMaterialTest FAILED === ({} of {} checks)", g_failures, g_checks);
        return 1;
    }
    AVER_INFO("=== GodotShaderMaterialTest PASSED === ({} checks)", g_checks);
    return 0;
}
