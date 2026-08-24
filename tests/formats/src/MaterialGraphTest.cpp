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

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== all material graph tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
