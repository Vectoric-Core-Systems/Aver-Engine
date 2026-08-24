// The .ocgraph -> HLSL material compiler, checked against a REAL SHADER COMPILER.
//
// WHY THIS TEST IS DIFFERENT FROM EVERY OTHER TEST IN THIS TREE. Asserting that a code generator
// emits the string you expected is a test of your expectations. A material graph's only real
// contract is "dxc accepts this and it means what the author drew", and the first half of that is
// mechanically checkable here: dxcompiler.dll sits beside these binaries (modules/rhi.d3d12's
// CMakeLists copies it into the shared bin/), and HLSL compilation is pure CPU -- no device, no
// adapter, no window. So this test composes exactly what the engine composes, hands it to the same
// compiler the engine uses, and fails when the text does not compile.
//
// That matters more here than anywhere else in the tree because a shader in this engine is a C++
// string assembled at runtime and type-checked only by createShader, inside a live device, at
// startup. Without this, a generated function with one wrong symbol is a warning on somebody else's
// machine. tools/DumpClusterPs.cpp exists for the same reason and says so at greater length.
//
// THE DXC BOOTSTRAP IS DUPLICATED, not shared, and that is not laziness: the engine's own
// ShaderCompiler lives in an anonymous namespace inside D3D12Device.cpp, so it has internal linkage
// and cannot be linked from here. Exporting it would put a backend type on a boundary
// modules/render.pbr/CMakeLists.txt explicitly forbids ("NEVER Aver.RHI.D3D12: no backend type may
// cross this boundary"). Thirty lines of LoadLibraryW glue is the cheaper of the two prices.
#include "aver/pbr/MaterialGraphHlsl.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/core/Log.hpp"

// NOT WIN32_LEAN_AND_MEAN: dxcapi.h is a COM header and needs IUnknown, IStream and BSTR, all of
// which lean-and-mean excludes. NOMINMAX because std::min is used below and windows.h defines a
// macro of that name.
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace aver;
using Microsoft::WRL::ComPtr;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// --------------------------------------------------------------------------------- the compiler

// DXC, loaded once. `available()` is false on a machine without dxcompiler.dll, and the caller
// SKIPS rather than fails there -- a missing redistributable is not a defect in the generator, and
// a test that cannot tell the two apart is worse than one that says which it saw.
class Dxc {
public:
    bool available() {
        if (tried_) return compiler_ != nullptr;
        tried_ = true;
        dll_ = LoadLibraryW(L"dxcompiler.dll");
        if (!dll_) return false;
        auto create = reinterpret_cast<DxcCreateInstanceProc>(
            reinterpret_cast<void*>(GetProcAddress(dll_, "DxcCreateInstance")));
        if (!create) return false;
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
            utils_.Reset();
            compiler_.Reset();
            return false;
        }
        return true;
    }

    // Compiles one pixel shader. Returns true on success; `outErr` carries dxc's own diagnostics,
    // which name the line and are the only useful thing to print when a generator emits bad text.
    bool compile(const std::string& source, const std::string& defines, std::string& outErr) {
        outErr.clear();
        if (!available()) return false;

        std::vector<std::wstring> args;
        args.push_back(L"-T");
        args.push_back(L"ps_6_5");
        args.push_back(L"-E");
        args.push_back(L"PSMaterialGraphTest");
        args.push_back(L"-Zpr");     // row-major, matching the engine's own D3DCOMPILE_PACK_MATRIX_ROW_MAJOR
        args.push_back(L"-Vd");      // no validation: dxil.dll signing is irrelevant to "does it compile"
        for (usize b = 0; b <= defines.size();) {
            const usize e = std::min(defines.find(';', b), defines.size());
            if (e > b) {
                const std::string d = defines.substr(b, e - b);
                args.push_back(L"-D");
                args.emplace_back(d.begin(), d.end());
            }
            b = e + 1;
        }
        std::vector<const wchar_t*> argv;
        argv.reserve(args.size());
        for (const std::wstring& a : args) argv.push_back(a.c_str());

        DxcBuffer buf{};
        buf.Ptr = source.data();
        buf.Size = source.size();
        buf.Encoding = DXC_CP_UTF8;

        ComPtr<IDxcResult> result;
        if (FAILED(compiler_->Compile(&buf, argv.data(), static_cast<UINT32>(argv.size()),
                                      nullptr, IID_PPV_ARGS(&result)))) {
            outErr = "IDxcCompiler3::Compile itself failed";
            return false;
        }
        ComPtr<IDxcBlobUtf8> errors;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
            errors && errors->GetStringLength() > 0) {
            outErr = errors->GetStringPointer();
        }
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        return SUCCEEDED(status);
    }

private:
    bool tried_ = false;
    HMODULE dll_ = nullptr;
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
};

// --------------------------------------------------------------------------------- composition

// A pixel shader that does nothing but exercise the material contract end to end: build a vertex,
// evaluate the material, shade it. DELIBERATELY NOT the sandbox's own PSClusterMain -- that would
// drag in Voxi's prelude and the cluster register map, and a failure would then be ambiguous
// between "the generated material is wrong" and "this test got the register numbers wrong".
static const char* kTestPs = R"(
float4 PSMaterialGraphTest(VSOut i) : SV_Target {
    AverVertex vtx = averVertexOf(i);
    AverLight sun;
    sun.direction  = normalize(float3(0.3, 0.4, 0.9));
    sun.radiance   = float3(3.0, 3.0, 3.0);
    sun.visibility = 1.0;
    AverSurface s = averEvalMaterial(vtx, sun);
    float3 lit = averShadeDirect(float3(0, 0, 0), s, sun);
    return float4(lit, averOpacity(s));
}
)";

// The engine's own composition order, from PbrShaders.hpp: shared prelude, material prelude, then
// the renderer's source -- with the generated function slotted in exactly where a renderer would
// append it, which is between the material prelude and anything that calls averEvalMaterial.
static std::string compose(const std::string& generated) {
    std::string s = rhi::sharedShaderPrelude();
    s += pbr::materialShaderPrelude();
    s += generated;
    s += kTestPs;
    return s;
}

// t0.. for the eight material maps and s0 for their sampler: any consistent map does, since nothing
// here builds a root signature to disagree with.
static std::string defines(bool graph) {
    std::string d = pbr::materialShaderDefines(0, 0);
    if (graph) d += ";AVER_MATERIAL_GRAPH=1";
    return d;
}

// --------------------------------------------------------------------------------- graph builders

static fmt::OcGraphNode node(const char* id, const char* type) {
    fmt::OcGraphNode n;
    n.id = id;
    n.type = type;
    return n;
}

static void addPin(fmt::OcGraphNode& n, const char* name, const char* type, bool out,
                   const char* def = "") {
    fmt::OcGraphPin p;
    p.name = name;
    p.type = type;
    p.isOutput = out;
    p.defaultValue = def;
    n.pins.push_back(p);
}

static void link(fmt::OcGraphData& g, const char* sn, const char* sp, const char* dn, const char* dp) {
    fmt::OcGraphLink l;
    l.sourceNode = sn;
    l.sourcePin = sp;
    l.destNode = dn;
    l.destPin = dp;
    g.links.push_back(l);
}

// A MaterialOutput node with the eight surface inputs and NO defaults on any of them -- which is
// what makes "this pin carries a literal" mean "a person typed one".
static fmt::OcGraphNode materialOutput(const char* id) {
    fmt::OcGraphNode n = node(id, "MaterialOutput");
    addPin(n, "BaseColor",   "float3", false);
    addPin(n, "Metallic",    "float",  false);
    addPin(n, "Roughness",   "float",  false);
    addPin(n, "Normal",      "float3", false);
    addPin(n, "Emissive",    "float3", false);
    addPin(n, "Occlusion",   "float",  false);
    addPin(n, "Opacity",     "float",  false);
    addPin(n, "AlphaCutoff", "float",  false);
    return n;
}

// The two-node graph the whole feature is proved on: a constant colour into BaseColor.
static fmt::OcGraphData flatColorGraph() {
    fmt::OcGraphData g;
    g.name = "M_Flat";
    g.domain = "material";
    fmt::OcGraphNode c = node("colour", "ConstFloat3");
    addPin(c, "value", "float3", true, "0.85,0.16,0.10");
    g.nodes.push_back(c);
    g.nodes.push_back(materialOutput("out"));
    link(g, "colour", "value", "out", "BaseColor");
    return g;
}

// A ConstFloat / ConstFloat3 source node, for feeding the node under test something real.
static fmt::OcGraphNode constFloat(const char* id, const char* value) {
    fmt::OcGraphNode n = node(id, "ConstFloat");
    addPin(n, "value", "float", true, value);
    return n;
}
static fmt::OcGraphNode constFloat3(const char* id, const char* value) {
    fmt::OcGraphNode n = node(id, "ConstFloat3");
    addPin(n, "value", "float3", true, value);
    return n;
}

// A `key=value` NODE attribute -- the same extraTokens slot Swizzle's mask= and SampleTexture's
// slot= actually read (see MaterialGraphHlsl.cpp's nodeAttr()).
static void attr(fmt::OcGraphNode& n, const char* keyValue) {
    n.extraTokens.emplace_back(keyValue);
}

// --------------------------------------------------------------------- new-vocabulary coverage
//
// TABLE-DRIVEN, ONE SMALL GRAPH PER NODE TYPE, over one enormous graph exercising all of them at
// once. The whole point of handing generated HLSL to a real compiler is to find out when a node's
// emitter is wrong; a single sprawling graph would still find that out, but dxc's diagnostic names
// a LINE in a wall of generated code, and it is then this file's job to guess which of two dozen
// nodes produced it. A small graph per node type instead means a failure comes back already
// labelled with the node type that caused it (runNodeCase below puts the label in front of every
// check it makes), which is the difference between "line 214: invalid operands" and "Reflect: DXC
// compiles it" failing. The cost is more boilerplate per node; the ORIGINAL 25-node vocabulary
// still gets its combined-graph exercise too (testItActuallyCompiles's M_Everything, above,
// untouched), so this is additive rather than a rewrite of how that test works.
//
// Each builder below wires the node under test so its OUTPUT reaches a MaterialOutput input --
// never a node built and left floating, which emission would simply never visit (see
// testDeadNodesAreNotEmitted). Where a node's own result is a float2 (TilingOffset, Rotator) it is
// routed through Length first: MaterialOutput has no float2 input and a float2 cannot widen to a
// float3 (testWideningRules covers why), but every field accepts a float, so a magnitude is the
// smallest already-proven node that turns "some vector" into "something any field will take".

// A unary float3 -> float3 node (Sqrt/Ceil/Sign/Exp/Log/Tan all have this shape), fed a literal
// vector and driving BaseColor.
static fmt::OcGraphData unaryMathGraph(const char* type, const char* value) {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = std::string("M_") + type;
    g.nodes.push_back(constFloat3("v", value));
    fmt::OcGraphNode n = node("n", type);
    addPin(n, "x", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "v", "value", "n", "x");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData cameraPositionGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_CameraPosition";
    fmt::OcGraphNode n = node("n", "CameraPosition");
    addPin(n, "xyz", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "xyz", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData objectPositionGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_ObjectPosition";
    fmt::OcGraphNode n = node("n", "ObjectPosition");
    addPin(n, "xyz", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "xyz", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData modGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Modulo";
    g.nodes.push_back(constFloat3("a", "0.9,0.7,0.5"));
    g.nodes.push_back(constFloat3("b", "0.3,0.2,0.4"));
    fmt::OcGraphNode n = node("n", "Modulo");
    addPin(n, "a", "float3", false);
    addPin(n, "b", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "value", "n", "a");
    link(g, "b", "value", "n", "b");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData stepGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Step";
    g.nodes.push_back(constFloat("x", "0.62"));
    fmt::OcGraphNode n = node("n", "Step");
    addPin(n, "edge", "float", false, "0.5");
    addPin(n, "x", "float", false);
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "x", "value", "n", "x");
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData smoothstepGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Smoothstep";
    g.nodes.push_back(constFloat("x", "0.4"));
    fmt::OcGraphNode n = node("n", "Smoothstep");
    addPin(n, "edge0", "float", false, "0");
    addPin(n, "edge1", "float", false, "1");
    addPin(n, "x", "float", false);
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "x", "value", "n", "x");
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData remapGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Remap";
    g.nodes.push_back(constFloat("x", "0.75"));
    fmt::OcGraphNode n = node("n", "Remap");
    addPin(n, "x", "float", false);
    addPin(n, "inMin", "float", false, "0");
    addPin(n, "inMax", "float", false, "1");
    addPin(n, "outMin", "float", false, "0");
    addPin(n, "outMax", "float", false, "10");
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "x", "value", "n", "x");
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData crossGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Cross";
    g.nodes.push_back(constFloat3("a", "1,0,0"));
    g.nodes.push_back(constFloat3("b", "0,1,0"));
    fmt::OcGraphNode n = node("n", "Cross");
    addPin(n, "a", "float3", false);
    addPin(n, "b", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "value", "n", "a");
    link(g, "b", "value", "n", "b");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData reflectGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Reflect";
    g.nodes.push_back(constFloat3("i", "0.2,-0.8,0.3"));
    g.nodes.push_back(constFloat3("nn", "0,1,0"));
    fmt::OcGraphNode n = node("n", "Reflect");
    addPin(n, "i", "float3", false);
    addPin(n, "n", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "i", "value", "n", "i");
    link(g, "nn", "value", "n", "n");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData distanceGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Distance";
    g.nodes.push_back(constFloat3("a", "0.1,0.2,0.3"));
    g.nodes.push_back(constFloat3("b", "0.4,0.1,0.9"));
    fmt::OcGraphNode n = node("n", "Distance");
    addPin(n, "a", "float3", false);
    addPin(n, "b", "float3", false);
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "value", "n", "a");
    link(g, "b", "value", "n", "b");
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData blendNormalsGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_BlendNormals";
    g.nodes.push_back(constFloat3("a", "0,0,1"));
    g.nodes.push_back(constFloat3("b", "0.1,0.1,0.98"));
    fmt::OcGraphNode n = node("n", "BlendNormals");
    addPin(n, "a", "float3", false);
    addPin(n, "b", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "value", "n", "a");
    link(g, "b", "value", "n", "b");
    link(g, "n", "result", "out", "Normal");
    return g;
}

// mask=xyz on a float3: the ordinary case, and the one the table-driven pass compiles through dxc.
static fmt::OcGraphData swizzleXyzGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Swizzle_xyz";
    g.nodes.push_back(constFloat3("v", "0.2,0.4,0.6"));
    fmt::OcGraphNode n = node("n", "Swizzle");
    addPin(n, "x", "float3", false);
    addPin(n, "result", "float3", true);
    attr(n, "mask=xyz");
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "v", "value", "n", "x");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

// mask=r on a float3: a ONE-component mask, which is a float, not a float1-of-something.
static fmt::OcGraphData swizzleRGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Swizzle_r";
    g.nodes.push_back(constFloat3("v", "0.2,0.4,0.6"));
    fmt::OcGraphNode n = node("n", "Swizzle");
    addPin(n, "x", "float3", false);
    addPin(n, "result", "float", true);
    attr(n, "mask=r");
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "v", "value", "n", "x");
    link(g, "n", "result", "out", "Roughness");
    return g;
}

// TilingOffset's own result is a float2, which cannot drive any MaterialOutput field directly (no
// field is float2, and a float2 does not widen to a float3 -- see testWideningRules). Length is the
// bridge: it is already proven by testItActuallyCompiles's M_Everything, is generic over its
// input's arity, and always returns a float, which every field accepts by splatting.
static fmt::OcGraphData tilingOffsetGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_TilingOffset";
    fmt::OcGraphNode uv = node("uv", "UV");
    addPin(uv, "uv", "float2", true);
    g.nodes.push_back(uv);
    fmt::OcGraphNode n = node("n", "TilingOffset");
    addPin(n, "uv", "float2", false);
    addPin(n, "tiling", "float2", false, "2,2");
    addPin(n, "offset", "float2", false, "0.1,0.1");
    addPin(n, "result", "float2", true);
    g.nodes.push_back(n);
    fmt::OcGraphNode len = node("len", "Length");
    addPin(len, "x", "float2", false);
    addPin(len, "result", "float", true);
    g.nodes.push_back(len);
    g.nodes.push_back(materialOutput("out"));
    link(g, "uv", "uv", "n", "uv");
    link(g, "n", "result", "len", "x");
    link(g, "len", "result", "out", "Roughness");
    return g;
}

// Same float2-result situation as TilingOffset, same Length bridge.
static fmt::OcGraphData rotatorGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Rotator";
    fmt::OcGraphNode uv = node("uv", "UV");
    addPin(uv, "uv", "float2", true);
    g.nodes.push_back(uv);
    fmt::OcGraphNode n = node("n", "Rotator");
    addPin(n, "uv", "float2", false);
    addPin(n, "centre", "float2", false, "0.5,0.5");
    addPin(n, "angle", "float", false, "0.7");
    addPin(n, "result", "float2", true);
    g.nodes.push_back(n);
    fmt::OcGraphNode len = node("len", "Length");
    addPin(len, "x", "float2", false);
    addPin(len, "result", "float", true);
    g.nodes.push_back(len);
    g.nodes.push_back(materialOutput("out"));
    link(g, "uv", "uv", "n", "uv");
    link(g, "n", "result", "len", "x");
    link(g, "len", "result", "out", "Roughness");
    return g;
}

// uv is deliberately left unlinked -- proving the surface-uv fallback compiles is exactly as
// important for Noise as it is for SampleTexture, and costs nothing extra here.
static fmt::OcGraphData noiseGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Noise";
    fmt::OcGraphNode n = node("n", "Noise");
    addPin(n, "uv", "float2", false);
    addPin(n, "scale", "float", false, "6");
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData checkerGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Checker";
    fmt::OcGraphNode n = node("n", "Checker");
    addPin(n, "uv", "float2", false);
    addPin(n, "scale", "float", false, "6");
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "result", "out", "Roughness");
    return g;
}

// slot=basecolor, uv unlinked -- the happy path this node type is proved on in the table below. The
// bad-slot and unlinked-uv BEHAVIOUR (not just "it compiles") gets its own dedicated tests further
// down, because those assert things about the emitted TEXT that a pass/fail table cannot express.
static fmt::OcGraphData sampleTextureGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_SampleTexture";
    fmt::OcGraphNode n = node("n", "SampleTexture");
    addPin(n, "uv", "float2", false);
    addPin(n, "rgb", "float3", true);
    addPin(n, "a", "float", true);
    addPin(n, "rgba", "float4", true);
    attr(n, "slot=basecolor");
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "rgb", "out", "BaseColor");
    return g;
}

static fmt::OcGraphData fresnelGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_Fresnel";
    fmt::OcGraphNode n = node("n", "Fresnel");
    addPin(n, "power", "float", false, "3");
    addPin(n, "result", "float", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "n", "result", "out", "Roughness");
    return g;
}

static fmt::OcGraphData ifGraph() {
    fmt::OcGraphData g;
    g.domain = "material";
    g.name = "M_If";
    g.nodes.push_back(constFloat("a", "0.3"));
    g.nodes.push_back(constFloat("b", "0.6"));
    g.nodes.push_back(constFloat3("yes", "1,0,0"));
    g.nodes.push_back(constFloat3("no", "0,0,1"));
    fmt::OcGraphNode n = node("n", "If");
    addPin(n, "a", "float", false);
    addPin(n, "b", "float", false);
    addPin(n, "ifTrue", "float3", false);
    addPin(n, "ifFalse", "float3", false);
    addPin(n, "result", "float3", true);
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "value", "n", "a");
    link(g, "b", "value", "n", "b");
    link(g, "yes", "value", "n", "ifTrue");
    link(g, "no", "value", "n", "ifFalse");
    link(g, "n", "result", "out", "BaseColor");
    return g;
}

// Compiles ONE node case: to compileMaterialGraph, then (dxc permitting) to dxc, with `label` --
// the node TYPE, not the graph's node id -- in front of every check this makes, so a failure names
// what it was that failed to compile without the reader having to open the graph literal above to
// find out.
static void runNodeCase(Dxc& dxc, const char* label, const fmt::OcGraphData& g) {
    const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(g);
    check(body.ok, std::string(label) + " compiles to HLSL: " + body.error);
    if (!body.ok || !dxc.available()) return;
    pbr::MaterialGraphEntry e;
    e.id = 1;
    e.name = label;
    e.hlsl = body.hlsl;
    std::string err;
    const bool ok = dxc.compile(compose(pbr::materialGraphHlsl({e})), defines(true), err);
    check(ok, std::string(label) + ": DXC compiles it: " + err);
}

using GraphBuilder = fmt::OcGraphData (*)();
struct NodeCase { const char* label; GraphBuilder build; };

// Every node type added since the original 25 -- see the big comment above for why this is a table
// of small graphs rather than one more addition to M_Everything.
static const NodeCase kNewNodeCases[] = {
    {"CameraPosition", []() -> fmt::OcGraphData { return cameraPositionGraph(); }},
    {"ObjectPosition",  []() -> fmt::OcGraphData { return objectPositionGraph(); }},
    {"Sqrt",  []() -> fmt::OcGraphData { return unaryMathGraph("Sqrt", "0.2,0.4,0.6"); }},
    {"Ceil",  []() -> fmt::OcGraphData { return unaryMathGraph("Ceil", "0.2,0.4,0.6"); }},
    {"Sign",  []() -> fmt::OcGraphData { return unaryMathGraph("Sign", "0.2,0.4,0.6"); }},
    {"Exp",   []() -> fmt::OcGraphData { return unaryMathGraph("Exp",  "0.2,0.4,0.6"); }},
    {"Log",   []() -> fmt::OcGraphData { return unaryMathGraph("Log",  "0.2,0.4,0.6"); }},
    {"Tan",   []() -> fmt::OcGraphData { return unaryMathGraph("Tan",  "0.2,0.4,0.6"); }},
    {"Modulo", []() -> fmt::OcGraphData { return modGraph(); }},
    {"Step", []() -> fmt::OcGraphData { return stepGraph(); }},
    {"Smoothstep", []() -> fmt::OcGraphData { return smoothstepGraph(); }},
    {"Remap", []() -> fmt::OcGraphData { return remapGraph(); }},
    {"Cross", []() -> fmt::OcGraphData { return crossGraph(); }},
    {"Reflect", []() -> fmt::OcGraphData { return reflectGraph(); }},
    {"Distance", []() -> fmt::OcGraphData { return distanceGraph(); }},
    {"BlendNormals", []() -> fmt::OcGraphData { return blendNormalsGraph(); }},
    {"Swizzle", []() -> fmt::OcGraphData { return swizzleXyzGraph(); }},
    {"TilingOffset", []() -> fmt::OcGraphData { return tilingOffsetGraph(); }},
    {"Rotator", []() -> fmt::OcGraphData { return rotatorGraph(); }},
    {"Noise", []() -> fmt::OcGraphData { return noiseGraph(); }},
    {"Checker", []() -> fmt::OcGraphData { return checkerGraph(); }},
    {"SampleTexture", []() -> fmt::OcGraphData { return sampleTextureGraph(); }},
    {"Fresnel", []() -> fmt::OcGraphData { return fresnelGraph(); }},
    {"If", []() -> fmt::OcGraphData { return ifGraph(); }},
};

static void testEveryNewNodeTypeCompiles(Dxc& dxc) {
    AVER_INFO("=== every node type added since the original 25, each in its own graph ===");
    for (const NodeCase& c : kNewNodeCases) runNodeCase(dxc, c.label, c.build());
}

// Counts NON-OVERLAPPING occurrences of `needle` in `hay` -- used below to prove a memoisation
// property (one texture fetch, not two) that no pass/fail compile result can show by itself.
static int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (usize pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

static void testSwizzleRules() {
    AVER_INFO("=== Swizzle: mask width, r/g/b/a spelling, and what a bad mask says ===");
    {
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(swizzleXyzGraph());
        check(r.ok, "mask=xyz on a float3 compiles: " + r.error);
        check(r.hlsl.find(".xyz") != std::string::npos,
              "and emits .xyz, a float3 result: " + r.hlsl);
    }
    {
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(swizzleRGraph());
        check(r.ok, "mask=r on a float3 compiles: " + r.error);
        check(r.hlsl.find(".x") != std::string::npos,
              "and emits .x -- r/g/b/a is spelled x/y/z/w in the generated HLSL, and one component "
              "is a float, not a one-wide vector: " + r.hlsl);
    }
    {
        // A float3 has no .w. The mask is well-formed (a legal letter) but the INPUT is too narrow
        // for it, which is a different failure from an illegal letter and is checked separately.
        fmt::OcGraphData g;
        g.domain = "material";
        g.nodes.push_back(constFloat3("v", "0.2,0.4,0.6"));
        fmt::OcGraphNode n = node("badmask", "Swizzle");
        addPin(n, "x", "float3", false);
        addPin(n, "result", "float", true);
        attr(n, "mask=w");
        g.nodes.push_back(n);
        g.nodes.push_back(materialOutput("out"));
        link(g, "v", "value", "badmask", "x");
        link(g, "badmask", "result", "out", "Roughness");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok, "a mask reading a component the input does not have is refused, not clamped "
                     "or wrapped");
        check(r.error.find("badmask") != std::string::npos,
              "and the message names the node: " + r.error);
    }
    {
        // No mask= attribute at all.
        fmt::OcGraphData g;
        g.domain = "material";
        g.nodes.push_back(constFloat3("v", "0.2,0.4,0.6"));
        fmt::OcGraphNode n = node("nomask", "Swizzle");
        addPin(n, "x", "float3", false);
        addPin(n, "result", "float3", true);
        g.nodes.push_back(n);
        g.nodes.push_back(materialOutput("out"));
        link(g, "v", "value", "nomask", "x");
        link(g, "nomask", "result", "out", "BaseColor");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok, "an absent mask= is refused rather than defaulting to something");
        check(r.error.find("nomask") != std::string::npos, "naming the node: " + r.error);
        check(r.error.find("mask=") != std::string::npos,
              "and the message says what a mask looks like: " + r.error);
    }
}

static void testSampleTextureBadSlotFails() {
    AVER_INFO("=== SampleTexture: an unrecognised slot= ===");
    fmt::OcGraphData g;
    g.domain = "material";
    fmt::OcGraphNode n = node("tex", "SampleTexture");
    addPin(n, "uv", "float2", false);
    addPin(n, "rgb", "float3", true);
    attr(n, "slot=diffuse");   // not one of the material's own eight slot names
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "tex", "rgb", "out", "BaseColor");
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(!r.ok, "an unrecognised slot= FAILS the compile rather than silently sampling slot 0");
    check(r.error.find("basecolor") != std::string::npos &&
          r.error.find("layer1normal") != std::string::npos,
          "and the message lists every legal slot name, first to last: " + r.error);
    check(r.error.find("diffuse") != std::string::npos,
          "and names the value that was actually given: " + r.error);
}

static void testSampleTextureUnlinkedUvUsesSurfaceUv() {
    AVER_INFO("=== SampleTexture: no uv link samples the surface's own uv ===");
    fmt::OcGraphData g;
    g.domain = "material";
    fmt::OcGraphNode n = node("tex", "SampleTexture");
    addPin(n, "uv", "float2", false);   // deliberately not linked
    addPin(n, "rgb", "float3", true);
    attr(n, "slot=basecolor");
    g.nodes.push_back(n);
    g.nodes.push_back(materialOutput("out"));
    link(g, "tex", "rgb", "out", "BaseColor");
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(r.ok, "compiles with an unlinked uv pin: " + r.error);
    check(r.hlsl.find("averSampleSlot(0u, uv)") != std::string::npos,
          "and samples the BARE identifier `uv`, not a float2(...) literal -- the one place this "
          "emitter treats an unlinked pin as anything other than zero: " + r.hlsl);
}

// SampleTexture's own comment (MaterialGraphHlsl.cpp) says: "the memo in resolve() means a graph
// reading both .rgb and .a from the same node samples once as well." resolve()'s memo is keyed by
// (nodeId, pin) -- see its `key = nodeId + '\0' + pin` -- so that claim is true for the SAME pin
// read twice and false for two DIFFERENT pins of the same node: SampleTexture's emitNode recomputes
// its local `sample = averSampleSlot(...)` fresh on every call, and a second call happens whenever
// the (node, pin) key differs, pin name included. Both halves are asserted here, against what DXC
// actually receives, rather than trusting the comment -- which is the whole reason this test file
// compiles through a real compiler instead of asserting on the generator's own account of itself.
static void testSampleTextureSampleMemoisation() {
    AVER_INFO("=== SampleTexture: one fetch per node, however many of its pins are read ===");
    {
        // The SAME pin (.rgb) driving two different fields: one (nodeId, pin) key, resolved once.
        fmt::OcGraphData g;
        g.domain = "material";
        fmt::OcGraphNode n = node("tex", "SampleTexture");
        addPin(n, "uv", "float2", false);
        addPin(n, "rgb", "float3", true);
        attr(n, "slot=basecolor");
        g.nodes.push_back(n);
        g.nodes.push_back(materialOutput("out"));
        link(g, "tex", "rgb", "out", "BaseColor");
        link(g, "tex", "rgb", "out", "Emissive");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(r.ok, "the same .rgb pin driving two fields compiles: " + r.error);
        const int samples = countOccurrences(r.hlsl, "averSampleSlot");
        check(samples == 1, "and IS memoised -- reading .rgb twice samples once, not twice -- found " +
                             std::to_string(samples) + " call(s): " + r.hlsl);
    }
    {
        // TWO DIFFERENT pins (.rgb and .a) of the same node: two (nodeId, pin) keys, resolved twice.
        fmt::OcGraphData g;
        g.domain = "material";
        fmt::OcGraphNode n = node("tex", "SampleTexture");
        addPin(n, "uv", "float2", false);
        addPin(n, "rgb", "float3", true);
        addPin(n, "a", "float", true);
        attr(n, "slot=basecolor");
        g.nodes.push_back(n);
        g.nodes.push_back(materialOutput("out"));
        link(g, "tex", "rgb", "out", "BaseColor");
        link(g, "tex", "a", "out", "Opacity");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(r.ok, ".rgb and .a from one node both driving the surface compiles: " + r.error);
        const int samples = countOccurrences(r.hlsl, "averSampleSlot");
        // THIS ASSERTION USED TO READ `samples == 2`, AND THAT IS THE POINT OF IT.
        //
        // resolve()'s memo keys on (node, PIN), which is right everywhere else -- two pins of a
        // Split really are two values -- but .rgb and .a are two VIEWS OF ONE FETCH, so keying per
        // pin emitted averSampleSlot twice: two real texture reads at runtime for one texel, in the
        // commonest material there is (albedo colour plus albedo alpha). The emitter's own comment
        // claimed the opposite and was simply wrong. A test that reads the EMITTED TEXT is what
        // caught it; nothing about the rendered picture would have.
        check(samples == 1, "and is memoised ACROSS PINS -- one fetch, swizzled twice, not two "
                            "texture reads for one texel -- found " + std::to_string(samples) +
                            " call(s): " + r.hlsl);
    }
}

// --------------------------------------------------------------------------------- the tests

static void testDomainIsRequired() {
    AVER_INFO("=== a material graph must say it is one ===");
    fmt::OcGraphData g = flatColorGraph();
    g.domain.clear();   // i.e. a gameplay graph
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(!r.ok, "a graph with no DOMAIN is REFUSED rather than compiled as a material");
    check(r.error.find("gameplay") != std::string::npos,
          "and the message says what it actually is: " + r.error);
}

static void testOutputNodeIsRequired() {
    AVER_INFO("=== the output node ===");
    {
        fmt::OcGraphData g;
        g.domain = "material";
        g.nodes.push_back(node("c", "ConstFloat3"));
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok && r.error.find("MaterialOutput") != std::string::npos,
              "a graph with no MaterialOutput is refused, naming what is missing");
    }
    {
        fmt::OcGraphData g = flatColorGraph();
        g.nodes.push_back(materialOutput("out2"));
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok && r.error.find("two MaterialOutput") != std::string::npos,
              "TWO outputs are refused -- a graph describes one surface, and nothing would say "
              "which of two won");
    }
    {
        fmt::OcGraphData g;
        g.domain = "material";
        g.nodes.push_back(materialOutput("out"));
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok && r.error.find("nothing connected") != std::string::npos,
              "an output with nothing wired into it is refused rather than compiling to a copy of "
              "the stock material");
    }
}

static void testOnlyDrivenFieldsAreWritten() {
    AVER_INFO("=== a partial graph stays partial ===");
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(flatColorGraph());
    check(r.ok, "the two-node graph compiles: " + r.error);
    check(r.hlsl.find("a.baseColor") != std::string::npos, "base colour is written");
    check(r.hlsl.find("a.roughness") == std::string::npos,
          "and ROUGHNESS IS NOT -- an undriven field keeps what the material's own maps and factors "
          "put there, which is the whole reason a graph may describe part of a surface");
    check(r.hlsl.find("a.metallic") == std::string::npos, "nor is metallic");
    check(r.hlsl.find("0.85") != std::string::npos && r.hlsl.find("0.16") != std::string::npos,
          "the literal reaches the emitted text");
}

static void testUnknownNodeFails() {
    AVER_INFO("=== a node this build cannot shade ===");
    fmt::OcGraphData g = flatColorGraph();
    g.nodes[0].type = "CharacterMove";   // a real GAMEPLAY node, which is the realistic mistake
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(!r.ok, "an unknown node type FAILS the compile rather than emitting nothing for it");
    check(r.error.find("colour") != std::string::npos,
          "and the message names the node by its id, so it can be found without the editor: " + r.error);
    check(r.error.find("CharacterMove") != std::string::npos, "and names the type it did not know");
}

static void testCycleIsCaught() {
    AVER_INFO("=== a data cycle ===");
    fmt::OcGraphData g;
    g.domain = "material";
    fmt::OcGraphNode a = node("a", "Add");
    addPin(a, "a", "float3", false);
    addPin(a, "b", "float3", false);
    addPin(a, "result", "float3", true);
    fmt::OcGraphNode b = node("b", "Add");
    addPin(b, "a", "float3", false);
    addPin(b, "b", "float3", false);
    addPin(b, "result", "float3", true);
    g.nodes.push_back(a);
    g.nodes.push_back(b);
    g.nodes.push_back(materialOutput("out"));
    link(g, "a", "result", "b", "a");
    link(g, "b", "result", "a", "a");
    link(g, "a", "result", "out", "BaseColor");
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(!r.ok && r.error.find("cycle") != std::string::npos,
          "a data cycle is reported, not recursed into: " + r.error);
}

static void testWideningRules() {
    AVER_INFO("=== promotion ===");
    {
        // A SCALAR SPLATS. Grey authored as one number is the commonest thing in a material.
        fmt::OcGraphData g;
        g.domain = "material";
        fmt::OcGraphNode c = node("grey", "ConstFloat");
        addPin(c, "value", "float", true, "0.5");
        g.nodes.push_back(c);
        g.nodes.push_back(materialOutput("out"));
        link(g, "grey", "value", "out", "BaseColor");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(r.ok, "a float drives a float3 input by splatting: " + r.error);
        check(r.hlsl.find("(float3)") != std::string::npos, "and the emitted text says so");
    }
    {
        // A float2 does NOT become a float3. Guessing the third component is how a UV wired into a
        // colour silently loses its blue instead of being refused.
        fmt::OcGraphData g;
        g.domain = "material";
        g.nodes.push_back(node("uv", "UV"));
        g.nodes.back().pins.push_back({"uv", "float2", true, ""});
        g.nodes.push_back(materialOutput("out"));
        link(g, "uv", "uv", "out", "BaseColor");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(!r.ok && r.error.find("widen") != std::string::npos,
              "a float2 into a float3 is REFUSED, not zero-filled: " + r.error);
        check(r.error.find("MakeFloat3") != std::string::npos,
              "and the message says what to do about it");
    }
    {
        // Wider truncates, which is ordinary HLSL and ordinary intent.
        fmt::OcGraphData g;
        g.domain = "material";
        fmt::OcGraphNode c = node("rgba", "ConstFloat4");
        addPin(c, "value", "float4", true, "0.2,0.4,0.6,0.8");
        g.nodes.push_back(c);
        g.nodes.push_back(materialOutput("out"));
        link(g, "rgba", "value", "out", "BaseColor");
        const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
        check(r.ok && r.hlsl.find(".xyz") != std::string::npos,
              "a float4 into a float3 truncates: " + r.error);
    }
}

static void testDeadNodesAreNotEmitted() {
    AVER_INFO("=== unreachable nodes ===");
    fmt::OcGraphData g = flatColorGraph();
    fmt::OcGraphNode orphan = node("scratch", "ConstFloat3");
    addPin(orphan, "value", "float3", true, "9,9,9");
    g.nodes.push_back(orphan);
    const pbr::MaterialGraphBody r = pbr::compileMaterialGraph(g);
    check(r.ok, "a graph with an unconnected node still compiles -- an author's half-finished "
                "experiment on the canvas is not an error: " + r.error);
    check(r.hlsl.find("9.0") == std::string::npos,
          "and the unreachable node emits NOTHING, because emission walks backwards from the output");
}

static void testDeterminism() {
    AVER_INFO("=== determinism ===");
    const std::string a = pbr::compileMaterialGraph(flatColorGraph()).hlsl;
    const std::string b = pbr::compileMaterialGraph(flatColorGraph()).hlsl;
    check(a == b, "the same graph emits BYTE-IDENTICAL text -- without which nothing downstream can "
                  "be keyed on a hash of it");
}

static void testDispatchShape() {
    AVER_INFO("=== the dispatch function ===");
    {
        const std::string s = pbr::materialGraphHlsl({});
        check(s.find("averEvalMaterial") != std::string::npos,
              "an EMPTY graph list still emits a valid averEvalMaterial");
        check(s.find("default: break") != std::string::npos,
              "whose only arm is the stock one");
    }
    {
        pbr::MaterialGraphEntry e;
        e.id = 3;
        e.name = "M_Flat";
        e.hlsl = pbr::compileMaterialGraph(flatColorGraph()).hlsl;
        const std::string s = pbr::materialGraphHlsl({e});
        check(s.find("case 3:") != std::string::npos, "a graph gets its own case arm");
        check(s.find("M_Flat") != std::string::npos, "named in a comment, so a shader dump says where it came from");
        check(s.find("gMaterialGraphId") != std::string::npos,
              "switched on the per-draw constant the material block already carries");
    }
}

// The one that matters: does dxc accept it.
static void testItActuallyCompiles(Dxc& dxc) {
    AVER_INFO("=== DXC compiles what the generator emitted ===");
    if (!dxc.available()) {
        AVER_WARN("   SKIP  dxcompiler.dll not loadable -- the generated HLSL was NOT verified");
        return;
    }

    std::string err;

    // The control. If the STOCK path stops compiling, every other result here is meaningless.
    const bool stock = dxc.compile(compose(""), defines(false), err);
    check(stock, "the STOCK material path compiles -- the control, without which nothing below "
                 "means anything: " + err);

    // The empty dispatch: AVER_MATERIAL_GRAPH removes the stock averEvalMaterial and this puts one
    // back. Proves the #ifndef seam works in isolation, before any graph is involved.
    const bool empty = dxc.compile(compose(pbr::materialGraphHlsl({})), defines(true), err);
    check(empty, "the generated dispatch with NO graphs compiles, so the AVER_MATERIAL_GRAPH seam "
                 "itself is sound: " + err);

    // And with a real graph in it.
    const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(flatColorGraph());
    check(body.ok, "the flat-colour graph compiles to HLSL: " + body.error);
    pbr::MaterialGraphEntry e;
    e.id = 1;
    e.name = "M_Flat";
    e.hlsl = body.hlsl;
    const bool withGraph = dxc.compile(compose(pbr::materialGraphHlsl({e})), defines(true), err);
    check(withGraph, "and DXC COMPILES IT: " + err);

    // A graph exercising every emitter, so the whole vocabulary is checked by a real compiler
    // rather than by the shape of the emitted string.
    {
        fmt::OcGraphData g;
        g.domain = "material";
        g.name = "M_Everything";

        const auto push = [&g](fmt::OcGraphNode n) { g.nodes.push_back(std::move(n)); };

        fmt::OcGraphNode uv = node("uv", "UV");
        addPin(uv, "uv", "float2", true);
        push(uv);
        fmt::OcGraphNode wp = node("wp", "WorldPosition");
        addPin(wp, "xyz", "float3", true);
        push(wp);
        fmt::OcGraphNode wn = node("wn", "WorldNormal");
        addPin(wn, "xyz", "float3", true);
        push(wn);
        fmt::OcGraphNode vd = node("vd", "ViewDirection");
        addPin(vd, "xyz", "float3", true);
        push(vd);

        fmt::OcGraphNode k = node("k", "ConstFloat");
        addPin(k, "value", "float", true, "0.35");
        push(k);
        fmt::OcGraphNode tint = node("tint", "ConstFloat3");
        addPin(tint, "value", "float3", true, "0.9,0.5,0.2");
        push(tint);

        const auto binaryNode = [&](const char* id, const char* type, const char* outType) {
            fmt::OcGraphNode n = node(id, type);
            addPin(n, "a", outType, false);
            addPin(n, "b", outType, false);
            addPin(n, "result", outType, true);
            push(n);
        };
        binaryNode("add", "Add", "float3");
        binaryNode("sub", "Subtract", "float3");
        binaryNode("mul", "Multiply", "float3");
        binaryNode("div", "Divide", "float3");
        binaryNode("mn", "Min", "float3");
        binaryNode("mx", "Max", "float3");
        binaryNode("pw", "Power", "float3");

        fmt::OcGraphNode lerpN = node("lp", "Lerp");
        addPin(lerpN, "a", "float3", false);
        addPin(lerpN, "b", "float3", false);
        addPin(lerpN, "t", "float", false, "0.5");
        addPin(lerpN, "result", "float3", true);
        push(lerpN);

        fmt::OcGraphNode clampN = node("cl", "Clamp");
        addPin(clampN, "x", "float3", false);
        addPin(clampN, "lo", "float", false, "0");
        addPin(clampN, "hi", "float", false, "1");
        addPin(clampN, "result", "float3", true);
        push(clampN);

        const auto unary = [&](const char* id, const char* type, const char* t) {
            fmt::OcGraphNode n = node(id, type);
            addPin(n, "x", t, false);
            addPin(n, "result", t, true);
            push(n);
        };
        unary("sat", "Saturate", "float3");
        unary("ab", "Abs", "float3");
        unary("fr", "Frac", "float3");
        unary("fl", "Floor", "float3");
        unary("si", "Sin", "float3");
        unary("co", "Cos", "float3");
        unary("nz", "Normalize", "float3");
        unary("om", "OneMinus", "float3");

        fmt::OcGraphNode dotN = node("dt", "Dot");
        addPin(dotN, "a", "float3", false);
        addPin(dotN, "b", "float3", false);
        addPin(dotN, "result", "float", true);
        push(dotN);
        fmt::OcGraphNode lenN = node("ln", "Length");
        addPin(lenN, "x", "float3", false);
        addPin(lenN, "result", "float", true);
        push(lenN);

        fmt::OcGraphNode sp = node("sp", "Split");
        addPin(sp, "x", "float3", false);
        addPin(sp, "x", "float", true);
        addPin(sp, "y", "float", true);
        addPin(sp, "z", "float", true);
        push(sp);

        fmt::OcGraphNode mk3 = node("mk3", "MakeFloat3");
        addPin(mk3, "x", "float", false);
        addPin(mk3, "y", "float", false);
        addPin(mk3, "z", "float", false);
        addPin(mk3, "result", "float3", true);
        push(mk3);
        fmt::OcGraphNode mk2 = node("mk2", "MakeFloat2");
        addPin(mk2, "x", "float", false);
        addPin(mk2, "y", "float", false);
        addPin(mk2, "result", "float2", true);
        push(mk2);
        fmt::OcGraphNode mk4 = node("mk4", "MakeFloat4");
        addPin(mk4, "x", "float", false);
        addPin(mk4, "y", "float", false);
        addPin(mk4, "z", "float", false);
        addPin(mk4, "w", "float", false);
        addPin(mk4, "result", "float4", true);
        push(mk4);

        push(materialOutput("out"));

        // Wire a chain that reaches every one of them.
        link(g, "wp", "xyz", "fr", "x");
        link(g, "fr", "result", "si", "x");
        link(g, "si", "result", "ab", "x");
        link(g, "ab", "result", "co", "x");
        link(g, "co", "result", "fl", "x");
        link(g, "tint", "value", "add", "a");
        link(g, "fl", "result", "add", "b");
        link(g, "add", "result", "sub", "a");
        link(g, "wn", "xyz", "sub", "b");
        link(g, "sub", "result", "mul", "a");
        link(g, "vd", "xyz", "mul", "b");
        link(g, "mul", "result", "div", "a");
        link(g, "div", "result", "mn", "a");
        link(g, "mn", "result", "mx", "a");
        link(g, "mx", "result", "pw", "a");
        link(g, "pw", "result", "cl", "x");
        link(g, "cl", "result", "sat", "x");
        link(g, "sat", "result", "nz", "x");
        link(g, "nz", "result", "om", "x");
        link(g, "om", "result", "lp", "a");
        link(g, "tint", "value", "lp", "b");
        link(g, "lp", "result", "out", "BaseColor");

        link(g, "wn", "xyz", "dt", "a");
        link(g, "vd", "xyz", "dt", "b");
        link(g, "dt", "result", "out", "Metallic");
        link(g, "tint", "value", "ln", "x");
        link(g, "ln", "result", "out", "Roughness");

        link(g, "tint", "value", "sp", "x");
        link(g, "sp", "x", "mk3", "x");
        link(g, "sp", "y", "mk3", "y");
        link(g, "sp", "z", "mk3", "z");
        link(g, "mk3", "result", "out", "Normal");

        link(g, "uv", "uv", "mk2", "x");   // float2 into a float scalar pin: truncates to .x
        link(g, "k", "value", "mk2", "y");
        link(g, "mk2", "result", "out", "Emissive");   // float2 -> float3 must FAIL, see below

        const pbr::MaterialGraphBody wide = pbr::compileMaterialGraph(g);
        check(!wide.ok, "the float2 -> float3 link is still refused even inside a large graph");

        // Repair it and require the whole thing to compile.
        g.links.pop_back();
        link(g, "mk4", "result", "out", "Emissive");   // float4 -> float3 truncates, which is legal
        link(g, "k", "value", "mk4", "x");
        link(g, "dt", "result", "mk4", "y");
        link(g, "ln", "result", "mk4", "z");
        link(g, "k", "value", "mk4", "w");
        link(g, "k", "value", "out", "Opacity");

        const pbr::MaterialGraphBody all = pbr::compileMaterialGraph(g);
        check(all.ok, "a graph using EVERY emitter compiles to HLSL: " + all.error);
        if (all.ok) {
            pbr::MaterialGraphEntry e2;
            e2.id = 2;
            e2.name = "M_Everything";
            e2.hlsl = all.hlsl;
            const bool ok = dxc.compile(compose(pbr::materialGraphHlsl({e, e2})), defines(true), err);
            check(ok, "and DXC COMPILES IT, with two graphs sharing one dispatch: " + err);
        }
    }
}

// A graph that arrived as TEXT, which is how every real one will.
static void testFromParsedText(Dxc& dxc) {
    AVER_INFO("=== from a .ocgraph file's own text ===");
    const std::string text =
        "OCGRAPH 1\n"
        "DOMAIN material\n"
        "NAME M_Parsed\n"
        "NODE colour ConstFloat3 -200 0\n"
        "PIN colour value out float3 0.05,0.35,0.45\n"
        "NODE rough ConstFloat -200 120\n"
        "PIN rough value out float 0.15\n"
        "NODE out MaterialOutput 40 0\n"
        "PIN out BaseColor in float3\n"
        "PIN out Roughness in float\n"
        "LINK colour.value out.BaseColor\n"
        "LINK rough.value out.Roughness\n";

    fmt::OcGraphData g;
    std::string err;
    check(fmt::parseOcgraph(text, g, &err), "the text parses: " + err);
    check(fmt::ocGraphDomainOf(g) == fmt::OcGraphDomain::Material, "and reads as a material graph");

    const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(g);
    check(body.ok, "a graph read from text compiles: " + body.error);
    check(body.hlsl.find("a.baseColor") != std::string::npos &&
          body.hlsl.find("a.roughness") != std::string::npos,
          "driving both of the fields it wired");

    if (!dxc.available()) return;
    pbr::MaterialGraphEntry e;
    e.id = 7;
    e.name = g.name;
    e.hlsl = body.hlsl;
    std::string cerr;
    check(dxc.compile(compose(pbr::materialGraphHlsl({e})), defines(true), cerr),
          "and DXC compiles it: " + cerr);
}

int main() {
    // UNBUFFERED, so the last line printed is the last line REACHED. Redirected to a file, the CRT
    // full-buffers stdout, and a run that stops mid-way then reports its progress 4 KB behind where
    // it actually got to -- which sends the reader looking at the wrong test.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Dxc dxc;
    AVER_INFO("==================================================");
    AVER_INFO("material graph -> HLSL");
    AVER_INFO("  DXC: {}", dxc.available() ? "loaded" : "NOT AVAILABLE -- compile checks will SKIP");

    testDomainIsRequired();
    testOutputNodeIsRequired();
    testOnlyDrivenFieldsAreWritten();
    testUnknownNodeFails();
    testCycleIsCaught();
    testWideningRules();
    testDeadNodesAreNotEmitted();
    testDeterminism();
    testDispatchShape();
    testItActuallyCompiles(dxc);
    testFromParsedText(dxc);
    testEveryNewNodeTypeCompiles(dxc);
    testSwizzleRules();
    testSampleTextureBadSlotFails();
    testSampleTextureUnlinkedUvUsesSurfaceUv();
    testSampleTextureSampleMemoisation();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== all material graph tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
