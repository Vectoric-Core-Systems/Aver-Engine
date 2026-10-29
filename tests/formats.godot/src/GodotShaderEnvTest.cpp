// GodotShaderEnvTest -- Fabric G6 SH3: Godot sky, fog and particles shaders, checked headless.
//
// Part 1 is plain logic: the front-end's preparation (uniform registers, unbound samplers, the report), the GodotEnvShader graph node round trip, the library
// rename, the sky splice marker, and the fog-volume shape word.
// Part 2 compiles the engine's real translation units with DXC, to DXIL and SPIR-V: Voxi (shared prelude + the sky spliced in + material prelude + voxi.hlsl) for
// the background pass, the scene pixel shader, ray-driven mode and the compute stages; the froxel fog compute passes with fog shaders; and the GPU particle
// simulation's spawn and update kernels with a particles shader. Nothing is run on a device. The sky, fog and particles shaders without a converted shader are
// compiled too, as the default twins.
#include "GdGraphBridge.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/FogVolume.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphGodot.hpp"
#include "aver/formats/godot/GodotScene.hpp"
#include "aver/formats/godot/GodotShaderEnv.hpp"
#include "aver/formats/godot/GodotVisualShader.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/DxcApi.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/GodotEnvPrograms.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

static std::string stageShaders() {
    const fs::path root = fs::path(AVER_REPO_ROOT) / "modules";
    const fs::path dst = fs::temp_directory_path() / "AverGodotShaderEnvTestShaders";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const char* mod : {"rhi", "render.pbr", "render.voxi", "particles"}) {
        for (const auto& e : fs::directory_iterator(root / mod / "shaders", ec)) {
            const std::string ext = e.path().extension().string();
            if (e.is_regular_file() && (ext == ".hlsl" || ext == ".hlsli")) fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst.string();
}

// ------------------------------------------------------------------------------------------------------------------------------ the shaders

static const char* kSky = R"(shader_type sky;
uniform vec3 top_color : source_color = vec3(0.1, 0.3, 0.8);
uniform vec3 horizon_color : source_color = vec3(0.7, 0.8, 0.9);
uniform float sun_size = 0.02;
uniform int star_density = 3;
uniform bool show_stars = true;
uniform sampler2D noise_tex : hint_default_black;
global uniform float wind_strength;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void sky() {
    float h = clamp(EYEDIR.y, 0.0, 1.0);
    vec3 col = mix(horizon_color, top_color, pow(h, 0.5));
    float sun = smoothstep(1.0 - sun_size, 1.0, dot(EYEDIR, LIGHT0_DIRECTION));
    col += LIGHT0_COLOR * LIGHT0_ENERGY * sun;
    if (show_stars && !AT_CUBEMAP_PASS) {
        vec2 g = floor(EYEDIR.xz / max(EYEDIR.y, 0.05) * float(star_density) * 20.0);
        col += vec3(step(0.995, hash(g))) * (1.0 - h);
    }
    col += texture(noise_tex, SKY_COORDS).rgb * 0.01 + vec3(sin(TIME * 0.1) * 0.01) + vec3(POSITION.y * 0.0) + vec3(wind_strength * 0.0);
    COLOR = col;
    FOG = vec4(horizon_color, 0.05 * (1.0 - h));
}
)";

static const char* kFog = R"(shader_type fog;
uniform float density = 1.5;
uniform vec3 fog_albedo : source_color = vec3(0.8, 0.9, 1.0);
uniform vec4 glow = vec4(1.0, 0.5, 0.2, 2.0);
uniform sampler2D noise_tex : hint_default_white;
void fog() {
    float n = texture(noise_tex, WORLD_POSITION.xz * 0.1).r;
    float h = clamp(UVW.y, 0.0, 1.0);
    DENSITY = density * (1.0 - h) * (0.5 + 0.5 * sin(WORLD_POSITION.x * 0.5 + TIME)) * n;
    ALBEDO = fog_albedo;
    EMISSION = glow.rgb * glow.a * smoothstep(0.8, 1.0, UVW.x) + OBJECT_POSITION * 0.0 + SIZE * 0.0 + vec3(SDF * 0.0);
}
)";

static const char* kParticles = R"(shader_type particles;
uniform vec4 gravity_and_drag = vec4(0.0, -9.8, 0.0, 0.5);
uniform vec3 emit_dir = vec3(0.0, 1.0, 0.0);
uniform float spin = 2.0;
uniform sampler2D flow : hint_default_black;
float rand(uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint w = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967295.0;
}
void start() {
    float r = rand(RANDOM_SEED);
    VELOCITY = (EMISSION_TRANSFORM * vec4(emit_dir * (1.0 + r), 0.0)).xyz;
    COLOR = vec4(1.0, 0.5 + 0.5 * r, 0.2, 1.0);
    CUSTOM = vec4(r, 0.0, 0.0, 0.0);
    TRANSFORM[3].xyz += vec3(r * 0.1, 0.0, 0.0);
    if (NUMBER % 7u == 0u) { ACTIVE = false; }
}
void process() {
    VELOCITY += gravity_and_drag.xyz * DELTA;
    VELOCITY *= max(0.0, 1.0 - gravity_and_drag.w * DELTA);
    TRANSFORM[3].xyz += VELOCITY * DELTA;
    float a = spin * DELTA;
    mat3 rot = mat3(vec3(cos(a), 0.0, sin(a)), vec3(0.0, 1.0, 0.0), vec3(-sin(a), 0.0, cos(a)));
    TRANSFORM = mat4(vec4(rot * TRANSFORM[0].xyz, 0.0), vec4(rot * TRANSFORM[1].xyz, 0.0), vec4(rot * TRANSFORM[2].xyz, 0.0), TRANSFORM[3]);
    COLOR.a = 1.0 - CUSTOM.x * 0.0 - float(INDEX) * 0.0;
    if (RESTART) { CUSTOM.y = TIME; }
    if (TRANSFORM[3].y < -5.0 || LIFETIME < 0.0) { ACTIVE = false; }
}
)";

static GdEnvShader prepare(const char* src, const char* name) {
    GdEnvShader s;
    GdEnvOptions o;
    o.name = name;
    gdShaderForEnv(src, std::string(name) + ".gdshader", o, s);
    return s;
}

static rhi::GodotEnvProgram programOf(const GdEnvShader& s) {
    rhi::GodotEnvProgram p;
    p.name = s.name;
    p.ns = s.ns;
    p.source = s.source;
    p.regs = static_cast<u32>(s.registerCount);
    p.globalRegs = static_cast<u32>(s.globalRegisterCount);
    p.hasStart = s.hasStart;
    p.hasProcess = s.hasProcess;
    p.values = s.defaultRegisters;
    return p;
}

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

bool compileHlsl(const DxcLib& d, const std::string& source, const char* entry, char stage, u32 sm, bool spirv, const std::string& defines, std::string& err) {
    const wchar_t* prefix = stage == 'v' ? L"vs" : stage == 'p' ? L"ps" : L"cs";
    const std::wstring target = std::wstring(prefix) + L"_" + std::to_wstring(sm / 10) + L"_" + std::to_wstring(sm % 10);
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

struct Row {
    std::string name;
    std::string source;
    const char* entry;
    char stage;
    u32 sm;
    std::string defines;
};

// The Voxi translation unit as VoxiRenderer's voxiShaderPrelude composes it, then voxi.hlsl. skyProgram empty = no Godot sky.
std::string voxiTu(const std::string& skyProgram) {
    auto& reg = pbr::materialGraphs();
    std::string s = skyProgram.empty() ? std::string() : std::string("#define AVER_GD_SKY 1\n");
    if (reg.hasGodotVaryings()) s += reg.vsOutMembersDefine();
    s += skyProgram.empty() ? std::string(rhi::sharedShaderPrelude()) : rhi::spliceGodotSky(rhi::sharedShaderPrelude(), skyProgram);
    if (!reg.hlsl().empty()) s += "\n#define AVER_MATERIAL_GRAPH 1\n";
    if (reg.hasGodotShaders()) s += "#define AVER_GD_FX_STOCK 1\n";
    s += pbr::materialShaderPrelude();
    s += reg.hlsl();
    s += rhi::shaderFile("voxi.hlsl");
    return s;
}
}  // namespace

int main() {
    AVER_INFO("=== GodotShaderEnvTest ===");
    rhi::setShaderSourceDir(stageShaders());
    pbr::materialGraphs().clear();

    // ---------------------------------------------------------------------------------------------------------------- part 1: logic
    const GdEnvShader sky = prepare(kSky, "TestSky");
    const GdEnvShader fog = prepare(kFog, "TestFog");
    const GdEnvShader par = prepare(kParticles, "TestParticles");
    check(sky.ok && sky.kind == GdEnvKind::Sky, "the sky shader prepares (" + sky.error + ")");
    check(fog.ok && fog.kind == GdEnvKind::Fog, "the fog shader prepares (" + fog.error + ")");
    check(par.ok && par.kind == GdEnvKind::Particles && par.hasStart && par.hasProcess, "the particles shader prepares and has start() and process() (" + par.error + ")");
    check(sky.ns.rfind("ge", 0) == 0 && sky.source.find(sky.ns + "main_sky") != std::string::npos, "the sky entry is <ns>main_sky");
    check(fog.source.find(fog.ns + "main_fog") != std::string::npos, "the fog entry is <ns>main_fog");
    check(par.source.find(par.ns + "main_start") != std::string::npos && par.source.find(par.ns + "main_process") != std::string::npos, "the particles entries are <ns>main_start and <ns>main_process");
    check(sky.ns != fog.ns && fog.ns != par.ns, "every shader gets its own name prefix");

    // registers: top_color (vec3 at 0), horizon_color (vec3 at 1), sun_size (float, with horizon in its 4th component? vec3 may be followed by a scalar), star_density (int), show_stars (bool)
    check(sky.registerCount >= 2 && sky.registerCount <= 4, "the sky's normal block is a few registers (" + std::to_string(sky.registerCount) + ")");
    check(sky.globalRegisterCount == 1, "the sky's one global uniform has a register in the global block");
    check(sky.defaultRegisters.size() == static_cast<usize>(sky.registerCount + sky.globalRegisterCount) * 4, "the default table is registers * 4 floats");
    {
        // source_color decodes piecewise sRGB: 0.1 -> 0.0100, 0.3 -> 0.0732, 0.8 -> 0.6038
        const float r = sky.defaultRegisters[0], g = sky.defaultRegisters[1], b = sky.defaultRegisters[2];
        check(std::fabs(r - 0.01002f) < 1e-3f && std::fabs(g - 0.07324f) < 1e-3f && std::fabs(b - 0.6038f) < 1e-3f, "source_color defaults are stored linear (sRGB decoded)");
        bool intFound = false, boolFound = false;
        for (const GdEngineUniform& u : sky.params) {
            const usize at = static_cast<usize>(u.slot.reg) * 4 + static_cast<usize>(u.slot.comp);
            u32 bits = 0;
            std::memcpy(&bits, &sky.defaultRegisters[at], 4);
            if (u.name == "star_density") intFound = bits == 3u;
            if (u.name == "show_stars") boolFound = bits == 1u;
        }
        check(intFound && boolFound, "int and bool uniforms ride the register as raw words (3 and 1)");
    }
    check(sky.samplers.size() == 1 && !sky.samplers[0].bindable, "a sampler uniform is kept and reported unbound");
    {
        bool unbound = false, fogNote = false;
        for (const GdEngineNote& n : sky.notes) if (n.subject == "uniform noise_tex" && n.severity == "approximated") unbound = true;
        for (const GdEngineNote& n : sky.notes) if (n.subject == "FOG") fogNote = true;
        check(unbound, "the report says the texture is approximated");
        check(fogNote, "the report says FOG is blended over the colour");
        bool sdf = false;
        for (const GdEngineNote& n : fog.notes) if (n.subject == "SDF") sdf = true;
        check(sdf, "the fog report says SDF reads a large distance");
        bool ud = false, tr = false;
        for (const GdEngineNote& n : par.notes) { if (n.subject == "EMISSION_TRANSFORM") ud = true; if (n.subject == "TRANSFORM") tr = true; }
        check(ud && tr, "the particles report names EMISSION_TRANSFORM and TRANSFORM");
    }

    // refusals
    {
        GdEnvShader s = prepare("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(1.0); }\n", "Spatial");
        check(!s.ok && s.error.find("spatial") != std::string::npos, "a spatial shader is refused here (SH2 takes it): " + s.error);
        s = prepare("shader_type sky;\nvoid sky() { COLOR = texture(RADIANCE, EYEDIR).rgb; }\n", "Radiance");
        check(!s.ok && s.error.find("RADIANCE") != std::string::npos, "sampling RADIANCE is refused with a reason: " + s.error);
        s = prepare("shader_type sky;\nuniform samplerCube c;\nvoid sky() { COLOR = texture(c, EYEDIR).rgb; }\n", "Cube");
        check(!s.ok && s.error.find("sampler2D") != std::string::npos, "a cube sampler is refused: " + s.error);
        s = prepare("shader_type fog;\nuniform vec4 a; uniform vec4 b; uniform vec4 c; uniform vec4 d; uniform vec4 e; uniform vec4 f; uniform vec4 g; uniform vec4 h;\n"
                    "uniform vec4 i; uniform vec4 j; uniform vec4 k; uniform vec4 l; uniform vec4 m; uniform vec4 n; uniform vec4 o; uniform vec4 p; uniform vec4 q;\n"
                    "void fog() { DENSITY = a.x + q.x; }\n", "TooMany");
        check(!s.ok && s.error.find("17 registers") != std::string::npos, "17 registers are refused: " + s.error);
        s = prepare("shader_type sky;\nvoid sky() { COLOR = vec3(1.0)\n}\n", "Broken");
        check(!s.ok && s.error.find("Broken.gdshader") != std::string::npos, "a shader error is reported with its file: " + s.error);
        s = prepare("shader_type particles;\nvoid helper() {}\n", "NoEntry");
        check(!s.ok, "a particles shader with neither start() nor process() is refused");
        s = prepare("shader_type particles;\nvoid process() { ACTIVE = false; }\n", "OnlyProcess");
        check(s.ok && !s.hasStart && s.hasProcess, "process() alone is a particles shader");
    }

    // the graph node
    {
        fmt::GodotEnvPart p;
        p.kind = "particles";
        p.name = "Rain #1 100%";
        p.ns = par.ns;
        p.regs = static_cast<u32>(par.registerCount);
        p.globalRegs = static_cast<u32>(par.globalRegisterCount);
        p.hasStart = true;
        p.hasProcess = true;
        p.defaults = par.defaultRegisters;
        u32 odd = 0x00000003u;
        std::memcpy(&p.defaults[0], &odd, 4);
        p.source = par.source + "\n// tail # with % and \t tab and CR\r\n";
        for (int i = 0; i < 300; ++i) p.source += "float4 x" + std::to_string(i) + " = float4(1.0, 2.0, 3.0, 4.0); // a # comment, 100%\n";
        fmt::OcGraphData g;
        fmt::packGodotEnvShader(p, g);
        const std::string text = fmt::writeOcgraph(g);
        check(text.find("DOMAIN godotenv") != std::string::npos && text.find("GodotEnvShader") != std::string::npos, "the packed graph names its domain and node");
        check(fmt::ocGraphDomainOf(g) == fmt::OcGraphDomain::Unknown, "the godotenv domain is one the gameplay and material sweeps skip");
        fmt::OcGraphData back;
        std::string err;
        const bool parsed = fmt::parseOcgraph(text, back, &err);
        check(parsed, "the packed graph parses (" + err + ")");
        fmt::GodotEnvPart q;
        const bool un = parsed && fmt::unpackGodotEnvShader(back, q, &err);
        check(un, "the node unpacks (" + err + ")");
        check(un && q.kind == "particles" && q.name == p.name && q.ns == p.ns && q.regs == p.regs && q.hasStart && q.hasProcess, "kind, name, ns, registers and entries survive");
        check(un && q.source == p.source, "a source with spaces, '#', '%', tabs and CRs survives byte for byte");
        u32 bits = 0;
        if (un && !q.defaults.empty()) std::memcpy(&bits, &q.defaults[0], 4);
        check(un && q.defaults.size() == p.defaults.size() && bits == 3u, "the default table survives as raw 32-bit words (a denormal int stays 3)");
        fmt::OcGraphData g2 = back;
        g2.nodes[0].extraTokens.push_back("kind=bogus");
        fmt::OcGraphData bad = g;
        for (std::string& t : bad.nodes[0].extraTokens) if (t.rfind("kind=", 0) == 0) t = "kind=bogus";
        check(!fmt::unpackGodotEnvShader(bad, q, &err), "an unknown kind is refused");
        fmt::OcGraphData mat;
        check(!fmt::ocGraphHasGodotEnvShader(mat) && fmt::ocGraphHasGodotEnvShader(g), "ocGraphHasGodotEnvShader tells the node from none");
    }

    // the rename
    {
        const std::string r = rhi::godotEnvRename("float4 gd_texture(GdSampler2D s) { return gdSlotSample(s.slot, s.m, uv, 0.0) + gdr_x + gdrSlotSize(1, 2) + ge1_gdf_mix + AVER_GD_TIME + GD_SMP(a,b,c) + GdS_Foo; }\n#ifndef AVER_GD_LIBRARY\n");
        check(r.find("gde_texture") != std::string::npos && r.find("GdESampler2D") != std::string::npos && r.find("gdeSlotSample") != std::string::npos &&
              r.find("gder_x") != std::string::npos && r.find("gderSlotSize") != std::string::npos && r.find("GDE_SMP") != std::string::npos && r.find("GdES_Foo") != std::string::npos &&
              r.find("AVER_GDE_LIBRARY") != std::string::npos, "the library names are renamed (gd_ gdr_ Gd gdSlot gdrSlot GD_SMP, the guard)");
        check(r.find("ge1_gdf_mix") != std::string::npos && r.find("AVER_GD_TIME") != std::string::npos, "prefixed user names and the hook macros are left alone");
    }

    // the sky splice
    {
        const std::string prelude = rhi::sharedShaderPrelude();
        usize count = 0;
        for (usize at = prelude.find(rhi::kGodotSkyMarker); at != std::string::npos; at = prelude.find(rhi::kGodotSkyMarker, at + 1)) ++count;
        check(count == 1, "the shared prelude carries the sky splice marker exactly once");
        check(rhi::spliceGodotSky(prelude, std::string()) == prelude, "splicing an empty program leaves the prelude untouched");
        const std::string prog = rhi::godotSkyProgram(programOf(sky));
        check(!prog.empty() && prog.find("averGdSky") != std::string::npos && prog.find("gde_mix") != std::string::npos && prog.find(sky.ns + "main_sky") != std::string::npos,
              "the sky program is composed (hooks, renamed library, the converted source, the entry)");
        const std::string spliced = rhi::spliceGodotSky(prelude, prog);
        check(spliced.find(rhi::kGodotSkyMarker) == std::string::npos && spliced.size() > prelude.size() + prog.size() / 2, "the splice replaces the marker line with the program");
        check(prog.find("#undef AVER_GD_EYEDIR") != std::string::npos && prog.find("#undef ddx") != std::string::npos, "the program lifts its macros again");
    }

    // the fog-volume shape word
    {
        FogVolumeDesc v;
        v.shape = FogVolumeShape::Cylinder;
        v.world[9] = 100.0f; v.world[10] = 200.0f; v.world[11] = 300.0f;
        v.halfExtentCm[0] = 400.0f; v.halfExtentCm[1] = 500.0f; v.halfExtentCm[2] = 600.0f;
        v.albedo[0] = 0.25f;
        f32 plain[kFogVolumeVecs][4];
        check(fogv::pack(v, plain) && plain[3][3] == 2.0f && fogVolumeShapeOf(plain[3][3]) == 2 && fogVolumeShaderOf(plain[3][3]) == 0u, "a volume without a shader packs the shape word as before");
        check(plain[4][0] == 0.25f, "and keeps its albedo in the albedo slot");
        v.shader = 3;
        f32 sh[kFogVolumeVecs][4];
        check(fogv::pack(v, sh) && fogVolumeShapeOf(sh[3][3]) == 2 && fogVolumeShaderOf(sh[3][3]) == 3u, "a shader volume packs the shape in the low bits and its shader index above");
        check(sh[4][0] == 100.0f && sh[4][1] == 200.0f && sh[4][2] == 300.0f && sh[5][0] == 400.0f && sh[5][1] == 500.0f && sh[5][2] == 600.0f,
              "its albedo and emission slots carry the centre and the local half extents");
        const f32 p[3] = {100.0f, 200.0f, 300.0f};
        check(fogv::density(sh, p) == fogv::density(plain, p), "the CPU twin of the density decodes the shape and ignores the shader bits");
    }

    // ---------------------------------------------------------------------------------------------------------------- part 1b: VisualShader
    {
        const fs::path dir = fs::path(AVER_REPO_ROOT) / "tests" / "fabric" / "data" / "EnvShaderGame" / "visual";
        const auto load = [&](const char* name, GdVisualShaderResult& out) {
            GodotResource res;
            GodotDiagnostics diag;
            const bool parsed = loadGodotResource((dir / name).string(), res, diag, name);
            check(parsed, std::string(name) + " parses");
            out = gdVisualShaderToCode(res);
        };
        GdVisualShaderResult rim, skyV, fogV;
        load("rim.tres", rim);
        check(rim.ok && rim.shaderType == "spatial", "the spatial VisualShader converts (" + rim.error + ")");
        check(rim.code.find("render_mode cull_disabled, unshaded;") != std::string::npos, "modes/cull and flags/unshaded become render modes");
        check(rim.code.find("uniform float rim_power : hint_range(0.5, 8.0, 0.1) = 3.0;") != std::string::npos || rim.code.find("uniform float rim_power : hint_range(0.5, 8.0)") != std::string::npos,
              "a FloatParameter becomes a uniform with its range and default");
        check(rim.code.find("pow(1.0 - clamp(dot(normalize(NORMAL), normalize(VIEW)), 0.0, 1.0)") != std::string::npos, "Fresnel uses NORMAL and VIEW as its unconnected inputs");
        check(rim.code.find("mix(") != std::string::npos && rim.code.find("vec3(1.0, 0.8, 0.2)") != std::string::npos, "Mix takes its default_input_values for the unconnected port");
        check(rim.code.find("ALBEDO = ") != std::string::npos && rim.code.find("ALPHA = ") != std::string::npos, "the connected output ports are assigned");
        check(rim.code.find("sin(TIME)") != std::string::npos || (rim.code.find("= sin(") != std::string::npos && rim.code.find("= TIME;") != std::string::npos), "an Input node reads the built-in and FloatFunc sin applies");
        check(rim.code.find("o = a * 0.5 + 0.5;") != std::string::npos, "an Expression node keeps its expression");
        bool curve = false, tex = false;
        for (const GdVisualNodeNote& n : rim.unsupported) if (n.type == "VisualShaderNodeCurveTexture") curve = true;
        for (const GdVisualNodeNote& n : rim.approximated) if (n.type == "VisualShaderNodeTexture") tex = true;
        check(curve, "an unknown node is reported unsupported per node");
        check(tex, "an embedded-texture Texture node is reported approximated");
        {
            GdEngineShader gs;
            GdEngineOptions go;
            go.name = "rim";
            const bool ok = gdShaderForEngine(rim.code, "rim.gdshader", go, gs);
            check(ok, "the generated spatial text goes through the SH2 path (" + gs.error + ")");
        }
        check(skyV.ok == false, "(sky result starts empty)");
        load("gradient_sky.tres", skyV);
        check(skyV.ok && skyV.shaderType == "sky" && skyV.code.find("COLOR = ") != std::string::npos && skyV.code.find("EYEDIR") != std::string::npos, "the sky VisualShader converts (" + skyV.error + ")");
        {
            const GdEnvShader e = prepare(skyV.code.c_str(), "gradient_sky");
            check(e.ok && e.kind == GdEnvKind::Sky, "the generated sky text goes through gdShaderForEnv (" + e.error + ")");
        }
        load("haze_fog.tres", fogV);
        check(fogV.ok && fogV.shaderType == "fog" && fogV.code.find("DENSITY = ") != std::string::npos && fogV.code.find("UVW") != std::string::npos, "the fog VisualShader converts (" + fogV.error + ")");
        {
            const GdEnvShader e = prepare(fogV.code.c_str(), "haze_fog");
            check(e.ok && e.kind == GdEnvKind::Fog, "the generated fog text goes through gdShaderForEnv (" + e.error + ")");
        }
        GodotResource notVs;
        GodotDiagnostics d2;
        parseGodotResource("[gd_resource type=\"Environment\" format=3]\n\n[resource]\n", "env.tres", notVs, d2);
        check(!gdVisualShaderToCode(notVs).ok, "another resource type is refused");
        GodotResource particles;
        parseGodotResource("[gd_resource type=\"VisualShader\" format=3]\n\n[resource]\nmode = 2\n", "p.tres", particles, d2);
        check(!gdVisualShaderToCode(particles).ok, "a particles VisualShader is refused with a reason");

        // Enum orders and reachability, from inline resources.
        const auto inlineVs = [&](const char* text) {
            GodotResource r;
            GodotDiagnostics d;
            const bool parsed = parseGodotResource(text, "inline.tres", r, d);
            check(parsed, "an inline VisualShader parses");
            return gdVisualShaderToCode(r);
        };
        const auto has = [](const GdVisualShaderResult& r, const char* s) { return r.code.find(s) != std::string::npos; };
        {
            const GdVisualShaderResult r = inlineVs(
                "[gd_resource type=\"VisualShader\" format=3]\n"
                "[sub_resource type=\"VisualShaderNodeTexture\" id=\"a\"]\ntexture_type = 1\n"
                "[sub_resource type=\"VisualShaderNodeTexture\" id=\"b\"]\ntexture_type = 2\n"
                "[resource]\nmode = 0\n"
                "nodes/fragment/2/node = SubResource(\"a\")\nnodes/fragment/3/node = SubResource(\"b\")\n"
                "nodes/fragment/connections = PackedInt32Array(2, 0, 0, 0, 3, 0, 0, 9)\n");
            check(r.ok, "a Texture colour and normal-map pair converts (" + r.error + ")");
            check(has(r, "uniform sampler2D tex_fra_2 : source_color;"), "Texture type Color (1) is declared source_color");
            check(has(r, "uniform sampler2D tex_fra_3 : hint_normal;"), "Texture type Normal Map (2) is declared hint_normal");
        }
        {
            const GdVisualShaderResult r = inlineVs(
                "[gd_resource type=\"VisualShader\" format=3]\n"
                "[sub_resource type=\"VisualShaderNodeVec3Constant\" id=\"a\"]\nconstant = Vector3(1, 2, 3)\n"
                "[sub_resource type=\"VisualShaderNodeReroute\" id=\"b\"]\nport_type = 4\n"
                "[resource]\nmode = 0\n"
                "nodes/fragment/2/node = SubResource(\"a\")\nnodes/fragment/3/node = SubResource(\"b\")\n"
                "nodes/fragment/connections = PackedInt32Array(2, 0, 3, 0, 3, 0, 0, 0)\n");
            check(r.ok && has(r, "vec3 n_out3p0") && !has(r, "mat4"), "a vec3 Reroute (port_type 4) is a vec3 local");
            GdEngineShader gs;
            GdEngineOptions go;
            go.name = "reroute";
            check(gdShaderForEngine(r.code, "reroute.gdshader", go, gs), "the Reroute shader compiles through the SH2 path (" + gs.error + ")");
        }
        {
            // TransformOp operators: AxB 0, BxA 1, AxB_COMP 2, BxA_COMP 3, ADD 4, A_MINUS_B 5, B_MINUS_A 6, A_DIV_B 7, B_DIV_A 8.
            const auto top = [&](int op) {
                return inlineVs((std::string("[gd_resource type=\"VisualShader\" format=3]\n"
                    "[sub_resource type=\"VisualShaderNodeTransformConstant\" id=\"a\"]\n"
                    "[sub_resource type=\"VisualShaderNodeTransformConstant\" id=\"b\"]\n"
                    "[sub_resource type=\"VisualShaderNodeTransformOp\" id=\"c\"]\noperator = ") + std::to_string(op) + "\n"
                    "[resource]\nmode = 0\n"
                    "nodes/vertex/2/node = SubResource(\"a\")\nnodes/vertex/3/node = SubResource(\"b\")\nnodes/vertex/4/node = SubResource(\"c\")\n"
                    "nodes/vertex/connections = PackedInt32Array(2, 0, 4, 0, 3, 0, 4, 1, 4, 0, 0, 10)\n").c_str());
            };
            const GdVisualShaderResult mulComp = top(2), add = top(4), sub = top(5), subBA = top(6), div = top(7), divBA = top(8), axb = top(0);
            check(has(axb, "n_out4p0 = n_out2p0 * n_out3p0;"), "TransformOp AxB multiplies");
            check(has(mulComp, "n_out2p0[0] * n_out3p0[0]") && !has(mulComp, "n_out4p0 = n_out2p0 + "), "TransformOp AxB_COMP multiplies component-wise");
            check(has(add, "n_out4p0 = n_out2p0 + n_out3p0;"), "TransformOp ADD adds");
            check(has(sub, "n_out4p0 = n_out2p0 - n_out3p0;"), "TransformOp A_MINUS_B subtracts");
            check(has(subBA, "n_out4p0 = n_out3p0 - n_out2p0;"), "TransformOp B_MINUS_A subtracts the other way");
            check(has(div, "n_out2p0[0] / n_out3p0[0]"), "TransformOp A_DIV_B divides component-wise");
            check(has(divBA, "n_out3p0[0] / n_out2p0[0]"), "TransformOp B_DIV_A divides the other way");
        }
        {
            const GdVisualShaderResult r = inlineVs(
                "[gd_resource type=\"VisualShader\" format=3]\n"
                "[sub_resource type=\"VisualShaderNodeIf\" id=\"a\"]\n"
                "default_input_values = [0, 2.0, 1, 1.0, 3, Vector3(1, 0, 0), 4, Vector3(0, 1, 0), 5, Vector3(0, 0, 1)]\n"
                "[resource]\nmode = 0\n"
                "nodes/fragment/2/node = SubResource(\"a\")\n"
                "nodes/fragment/connections = PackedInt32Array(2, 0, 0, 0)\n");
            // Ports: a == b is 3, a > b is 4, a < b is 5; a > b is the else branch.
            check(r.ok && has(r, "n_out2p0 = vec3(1.0, 0.0, 0.0);\n\t} else if") && has(r, "n_out2p0 = vec3(0.0, 0.0, 1.0);\n\t} else {") && has(r, "n_out2p0 = vec3(0.0, 1.0, 0.0);\n\t}\n"),
                  "If maps a == b, a < b and a > b to their own ports");
        }
        {
            const GdVisualShaderResult r = inlineVs(
                "[gd_resource type=\"VisualShader\" format=3]\n"
                "[sub_resource type=\"VisualShaderNodeGlobalExpression\" id=\"g\"]\nexpression = \"float gx_f(float x) { return x * 2.0; }\"\n"
                "[sub_resource type=\"VisualShaderNodeFloatParameter\" id=\"p\"]\nparameter_name = \"unwired\"\n"
                "[sub_resource type=\"VisualShaderNodeExpression\" id=\"e\"]\noutput_ports = \"0,0,o;\"\nexpression = \"o = gx_f(1.0);\"\n"
                "[resource]\nmode = 0\n"
                "nodes/fragment/2/node = SubResource(\"g\")\nnodes/fragment/3/node = SubResource(\"p\")\nnodes/fragment/4/node = SubResource(\"e\")\n"
                "nodes/fragment/connections = PackedInt32Array(4, 0, 0, 1)\n");
            check(r.ok && has(r, "float gx_f(float x)"), "an unconnected GlobalExpression is still emitted");
            check(has(r, "uniform float unwired;"), "an unconnected parameter node still declares its uniform");
            GdEngineShader gs;
            GdEngineOptions go;
            go.name = "gx";
            check(gdShaderForEngine(r.code, "gx.gdshader", go, gs), "the shader calling the global function compiles through the SH2 path (" + gs.error + ")");
        }
    }
    // The register-count bound must not wrap.
    {
        fmt::GodotEnvPart p;
        p.kind = "sky";
        p.name = "wrap";
        p.ns = "ge0_";
        p.source = "void ge0_main() {}\n";
        fmt::OcGraphData g;
        fmt::packGodotEnvShader(p, g);
        for (std::string& t : g.nodes[0].extraTokens) {
            if (t.rfind("regs=", 0) == 0) t = "regs=4294967295";
            if (t.rfind("gregs=", 0) == 0) t = "gregs=1";
        }
        fmt::GodotEnvPart q;
        std::string err;
        check(!fmt::unpackGodotEnvShader(g, q, &err), "a register count that wraps the u32 sum is refused");
        for (std::string& t : g.nodes[0].extraTokens) {
            if (t.rfind("regs=", 0) == 0) t = "regs=-1";
            if (t.rfind("gregs=", 0) == 0) t = "gregs=0";
        }
        check(!fmt::unpackGodotEnvShader(g, q, &err), "a negative register count is refused");
        for (std::string& t : g.nodes[0].extraTokens) {
            if (t.rfind("regs=", 0) == 0) t = "regs=10";
            if (t.rfind("gregs=", 0) == 0) t = "gregs=6";
        }
        check(fmt::unpackGodotEnvShader(g, q, &err) && q.defaults.size() == 64, "sixteen registers in all still unpack");
    }

    // ---------------------------------------------------------------------------------------------------------------- part 2: DXC
#if defined(_WIN32)
    const fs::path dxilPath = fs::path(executableDir()) / "dxcompiler.dll";
    const fs::path spirvPath = fs::path(AVER_REPO_ROOT) / "third_party" / "dxc-spirv" / "dxcompiler.dll";
#elif defined(__APPLE__)
    const fs::path dxilPath = fs::path(executableDir()) / "libdxcompiler.dylib";
    const fs::path spirvPath = dxilPath;
#else
    const fs::path dxilPath = fs::path(executableDir()) / "libdxcompiler.so";
    const fs::path spirvPath = dxilPath;
#endif
    DxcLib dxil, spirv;
    loadDxc(dxilPath, dxil);
    loadDxc(spirvPath, spirv);
    if (!dxil.ok() && !spirv.ok()) {
        AVER_WARN("  SKIP  no DXC compiler found next to the test or in third_party/dxc-spirv: the translation-unit compiles are skipped");
    } else {
        const std::string skyProg = rhi::godotSkyProgram(programOf(sky));
        const rhi::GodotEnvProgram fogProg = programOf(fog), parProg = programOf(par);
        const std::string fogText = rhi::godotFogProgram({&fogProg});
        const std::string parText = rhi::godotParticlesProgram(parProg);
        check(!fogText.empty() && !parText.empty() && !skyProg.empty(), "the three programs compose");

        constexpr u32 kVoxiSrvCount = 25, kMaterialSamplerSlot = 2, kRtTextureCapacity = 4096;
        const std::string mat = pbr::materialShaderDefines(kVoxiSrvCount, kMaterialSamplerSlot, false);
        const std::string bindless = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" + std::to_string(kRtTextureCapacity);
        const std::string singlePass = ";AVER_RD_SINGLE_PASS=1";
        const auto vx = [&](const std::string& extra) { return extra.empty() ? mat : mat + ";" + extra; };

        std::vector<Row> rows;
        const std::string voxiSky = voxiTu(skyProg);
        rows.push_back({"Voxi PSGodotSky (the background pass)", voxiSky, "PSGodotSky", 'p', 65, vx("")});
        rows.push_back({"Voxi PSMainVoxi raster, sky ambient + reflections", voxiSky, "PSMainVoxi", 'p', 65, vx("AVER_HAIR_SHADING=1")});
        rows.push_back({"Voxi PSMainVoxi RT, sky", voxiSky, "PSMainVoxi", 'p', 65, vx("AVER_RT=1")});
        rows.push_back({"Voxi PSRayDriven single pass, sky", voxiSky, "PSRayDriven", 'p', 65, vx(bindless + singlePass)});
        rows.push_back({"Voxi PSRayDriven split, sky", voxiSky, "PSRayDriven", 'p', 65, vx(bindless + ";AVER_RD_SPLIT=1")});
        rows.push_back({"Voxi CSRdGi, sky", voxiSky, "CSRdGi", 'c', 66, vx(bindless)});
        rows.push_back({"Voxi CSRdManyLights, sky", voxiSky, "CSRdManyLights", 'c', 65, vx(bindless)});
        rows.push_back({"Voxi PSVoxelDebug, sky", voxiSky, "PSVoxelDebug", 'p', 65, vx("")});
        rows.push_back({"Voxi VSky, sky", voxiSky, "VSky", 'v', 65, vx("")});
        // The default twins: no sky shader, the same entries.
        const std::string voxiPlain = voxiTu(std::string());
        rows.push_back({"Voxi PSMainVoxi raster (default twin)", voxiPlain, "PSMainVoxi", 'p', 65, vx("AVER_HAIR_SHADING=1")});
        rows.push_back({"Voxi PSRayDriven single pass (default twin)", voxiPlain, "PSRayDriven", 'p', 65, vx(bindless + singlePass)});

        // A spatial Godot material shader beside the sky: both carry a copy of the Godot library under different names.
        static const char* kSpatial = R"(shader_type spatial;
uniform vec4 tint : source_color = vec4(0.8, 0.4, 0.2, 1.0);
uniform sampler2D albedo_tex : source_color, repeat_enable;
void fragment() { vec4 a = texture(albedo_tex, UV); ALBEDO = a.rgb * tint.rgb; ALPHA = a.a; ROUGHNESS = mod(TIME, 1.0); }
)";
        {
            GdEngineShader gs;
            GdEngineOptions go;
            go.name = "Beside";
            const bool prepared = gdShaderForEngine(kSpatial, "Beside.gdshader", go, gs);
            check(prepared && gdtest::registerShader(gs, "Beside.ocgraph") != 0, "a spatial Godot shader registers beside the sky");
            const std::string both = voxiTu(skyProg);
            rows.push_back({"Voxi PSMainVoxi raster, sky + spatial Godot shader", both, "PSMainVoxi", 'p', 65, vx("AVER_HAIR_SHADING=1")});
            rows.push_back({"Voxi PSRayDriven single pass, sky + spatial Godot shader", both, "PSRayDriven", 'p', 65, vx(bindless + singlePass)});
            pbr::materialGraphs().clear();
        }

        // Fog: the froxel TU is the shared prelude, the program, then voxi_froxel.hlsl.
        const std::string froxel = std::string(rhi::sharedShaderPrelude()) + fogText + rhi::shaderFile("voxi_froxel.hlsl");
        rows.push_back({"Froxel CSFroxelScatter fog shaders", froxel, "CSFroxelScatter", 'c', 60, "AVER_RT=0;AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1"});
        rows.push_back({"Froxel CSFroxelScatter RT fog shaders", froxel, "CSFroxelScatter", 'c', 65, "AVER_RT=1;AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1"});
        rows.push_back({"Froxel CSFroxelIntegrate fog shaders", froxel, "CSFroxelIntegrate", 'c', 60, "AVER_RT=0;AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1"});
        const std::string froxelPlain = std::string(rhi::sharedShaderPrelude()) + rhi::shaderFile("voxi_froxel.hlsl");
        rows.push_back({"Froxel CSFroxelScatter fog volumes (default twin)", froxelPlain, "CSFroxelScatter", 'c', 60, "AVER_RT=0;AVER_FOG_VOLUMES=1"});
        // Two shaders in one table.
        {
            const GdEnvShader fog2 = prepare("shader_type fog;\nuniform float k = 3.0;\nvoid fog() { DENSITY = k * UVW.z; ALBEDO = vec3(1.0, 0.0, 0.0); }\n", "SecondFog");
            const rhi::GodotEnvProgram fp2 = programOf(fog2);
            const std::string two = std::string(rhi::sharedShaderPrelude()) + rhi::godotFogProgram({&fogProg, &fp2}) + rhi::shaderFile("voxi_froxel.hlsl");
            check(fog2.ok && !two.empty(), "two fog shaders compose into one table");
            rows.push_back({"Froxel CSFroxelScatter two fog shaders", two, "CSFroxelScatter", 'c', 60, "AVER_RT=0;AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1"});
        }

        // Particles: the program is the prelude of the simulation file.
        const std::string sim = rhi::shaderFile("particles_sim.hlsl");
        check(!sim.empty(), "particles_sim.hlsl is staged");
        const std::string simGd = parText + sim;
        rows.push_back({"Particles CSSpawn godot", simGd, "CSSpawn", 'c', 60, "AVER_GD_PARTICLES=1"});
        rows.push_back({"Particles CSUpdate godot", simGd, "CSUpdate", 'c', 60, "AVER_GD_PARTICLES=1"});
        rows.push_back({"Particles CSSpawn (default twin)", sim, "CSSpawn", 'c', 60, ""});
        rows.push_back({"Particles CSUpdate (default twin)", sim, "CSUpdate", 'c', 60, ""});
        rows.push_back({"Particles CSBegin (default twin)", sim, "CSBegin", 'c', 60, ""});
        {
            const GdEnvShader startOnly = prepare("shader_type particles;\nvoid start() { VELOCITY = vec3(0.0, 1.0, 0.0); COLOR = vec4(1.0); }\n", "StartOnly");
            const GdEnvShader procOnly = prepare("shader_type particles;\nuniform float s = 1.0;\nvoid process() { TRANSFORM[3].y += s * DELTA; }\n", "ProcOnly");
            check(startOnly.ok && procOnly.ok, "start()-only and process()-only particles shaders prepare");
            rows.push_back({"Particles CSSpawn godot, start() only", rhi::godotParticlesProgram(programOf(startOnly)) + sim, "CSSpawn", 'c', 60, "AVER_GD_PARTICLES=1"});
            rows.push_back({"Particles CSUpdate godot, start() only", rhi::godotParticlesProgram(programOf(startOnly)) + sim, "CSUpdate", 'c', 60, "AVER_GD_PARTICLES=1"});
            rows.push_back({"Particles CSSpawn godot, process() only", rhi::godotParticlesProgram(programOf(procOnly)) + sim, "CSSpawn", 'c', 60, "AVER_GD_PARTICLES=1"});
            rows.push_back({"Particles CSUpdate godot, process() only", rhi::godotParticlesProgram(programOf(procOnly)) + sim, "CSUpdate", 'c', 60, "AVER_GD_PARTICLES=1"});
        }

        int compiled = 0;
        for (const Row& r : rows) {
            for (const bool useSpirv : {false, true}) {
                const DxcLib& lib = useSpirv ? spirv : dxil;
                if (!lib.ok()) continue;
                std::string err;
                const bool ok = compileHlsl(lib, r.source, r.entry, r.stage, r.sm, useSpirv, r.defines, err);
                if (ok) ++compiled;
                check(ok, r.name + (useSpirv ? " -> SPIR-V" : " -> DXIL") + (ok ? std::string() : ": " + err.substr(0, 1500)));
            }
        }
        AVER_INFO("   {} translation-unit compiles", compiled);
    }

    AVER_INFO("=== GodotShaderEnvTest: {} checks, {} failures ===", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
