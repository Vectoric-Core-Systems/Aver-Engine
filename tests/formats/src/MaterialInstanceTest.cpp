// Material instances (.ocmati), graph parameters and material functions. CPU only, no device, no files:
// every "file" is an in-memory string.
#include "aver/formats/MaterialInstance.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/pbr/MaterialGraphHlsl.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <map>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

using Files = std::map<std::string, std::string>;

static fmt::MaterialTextReader textReader(const Files& f) {
    return [&f](const std::string& p, std::string& out) {
        const auto it = f.find(p);
        if (it == f.end()) return false;
        out = it->second;
        return true;
    };
}

static fmt::MaterialGraphReader graphReader(const Files& f) {
    return [&f](const std::string& p, fmt::OcGraphData& out, std::string* err) {
        const auto it = f.find(p);
        if (it == f.end()) { if (err) *err = "no such file " + p; return false; }
        return fmt::parseOcgraph(it->second, out, err);
    };
}

static fmt::OcGraphData parse(const std::string& text) {
    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(text, g, &err)) AVER_ERROR("fixture does not parse: {}", err);
    return g;
}

static const char* kBase =
    "OCMAT 1\nNAME Base\nPARAM roughnessFactor 0.9\nPARAM metallicFactor 0.1\nGRAPHREF Materials/G.ocgraph\n";

// ------------------------------------------------------------------------------------ .ocmati

static void testFormatRoundTrip() {
    AVER_INFO("=== .ocmati round trip ===");
    fmt::OcMatInstance m;
    std::string err;
    const bool ok = fmt::parseOcmati(
        "OCMATI 1\n# note\nNAME Rust\nPARENT Materials/Base.ocmat\nSCALAR Wear 0.75\nVECTOR Tint 1 0.5 0.25 1\n"
        "PARAM roughnessFactor 0.3\n", m, &err);
    check(ok, "parses: " + err);
    check(m.name == "Rust" && m.parent == "Materials/Base.ocmat", "name and parent");
    check(m.params.size() == 2 && m.params[0].second == "0.75" && m.params[1].second == "1,0.5,0.25,1", "parameters");
    check(m.records.size() == 1 && m.records[0] == "PARAM roughnessFactor 0.3", "record kept verbatim");
    fmt::OcMatInstance m2;
    check(fmt::parseOcmati(fmt::writeOcmati(m), m2, &err), "written text parses");
    check(fmt::writeOcmati(m2) == fmt::writeOcmati(m), "write(parse(write(x))) is stable");
    check(m2.params == m.params && m2.records == m.records && m2.parent == m.parent, "fields survive");
    check(!fmt::parseOcmati("OCMATI 1\nSCALAR X nope\n", m2, &err), "a non-number is refused: " + err);
    check(!fmt::parseOcmati("OCMATI 2\n", m2, &err), "a newer version is refused");
    check(!fmt::parseOcmati("PARENT a\n", m2, &err), "no header is refused");
    check(fmt::isMaterialInstancePath("A/b.OCMATI") && !fmt::isMaterialInstancePath("a.ocmat"), "extension test");
}

static void testChainResolution() {
    AVER_INFO("=== parameter and record resolution through a chain ===");
    Files f;
    f["Base.ocmat"] = kBase;
    f["Mid.ocmati"] = "OCMATI 1\nPARENT Base.ocmat\nSCALAR Wear 0.5\nSCALAR Keep 2\nPARAM roughnessFactor 0.5\n";
    f["Leaf.ocmati"] = "OCMATI 1\nPARENT Mid.ocmati\nSCALAR Wear 0.9\nVECTOR Tint 1 0 0 1\nPARAM metallicFactor 0.8\n";
    fmt::ResolvedMaterialInstance r;
    std::string err;
    check(fmt::resolveMaterialInstance("Leaf.ocmati", textReader(f), r, &err), "resolves: " + err);
    check(r.basePath == "Base.ocmat", "root is the .ocmat");
    check(r.chain.size() == 3 && r.chain[0] == "Leaf.ocmati" && r.chain[2] == "Base.ocmat", "chain leaf first");
    std::map<std::string, std::string> p(r.params.begin(), r.params.end());
    check(p["Wear"] == "0.9", "child parameter wins over parent");
    check(p["Keep"] == "2" && p["Tint"] == "1,0,0,1", "inherited and new parameters are present");

    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    check(fmt::parseOcmat(fmt::composeInstanceOcmat(r), d, &ex, &err), "composed text parses: " + err);
    check(std::fabs(d.roughnessFactor - 0.5f) < 1e-6f, "mid record overrides the base roughness");
    check(std::fabs(d.metallicFactor - 0.8f) < 1e-6f, "leaf record overrides the base metallic");
    check(ex.graphRef == "Materials/G.ocgraph", "untouched base records (GRAPHREF) survive");
}

static void testChainErrors() {
    AVER_INFO("=== instance chain errors ===");
    Files f;
    f["A.ocmati"] = "OCMATI 1\nPARENT B.ocmati\n";
    f["B.ocmati"] = "OCMATI 1\nPARENT C.ocmati\n";
    f["C.ocmati"] = "OCMATI 1\nPARENT a.ocmati\n";   // different case: still a cycle
    fmt::ResolvedMaterialInstance r;
    std::string err;
    check(!fmt::resolveMaterialInstance("A.ocmati", textReader(f), r, &err), "a three-instance cycle is refused");
    check(contains(err, "cycle") && contains(err, "A.ocmati") && contains(err, "C.ocmati"), "the error names the chain: " + err);

    f["Self.ocmati"] = "OCMATI 1\nPARENT Self.ocmati\n";
    check(!fmt::resolveMaterialInstance("Self.ocmati", textReader(f), r, &err) && contains(err, "cycle"), "self-parent is a cycle");

    f["NoParent.ocmati"] = "OCMATI 1\nSCALAR X 1\n";
    check(!fmt::resolveMaterialInstance("NoParent.ocmati", textReader(f), r, &err) && contains(err, "PARENT"), "missing PARENT is refused");
    f["Dangling.ocmati"] = "OCMATI 1\nPARENT Gone.ocmat\n";
    check(!fmt::resolveMaterialInstance("Dangling.ocmati", textReader(f), r, &err) && contains(err, "Gone.ocmat"), "missing parent file is named");

    for (int i = 0; i < 40; ++i)
        f["L" + std::to_string(i) + ".ocmati"] = "OCMATI 1\nPARENT L" + std::to_string(i + 1) + ".ocmati\n";
    check(!fmt::resolveMaterialInstance("L0.ocmati", textReader(f), r, &err) && contains(err, "longer"), "a runaway chain is capped");
}

// ------------------------------------------------------------------------------------ parameters

static const char* kParamGraph =
    "OCGRAPH 1\nDOMAIN material\nNAME G\n"
    "NODE w ScalarParameter 0 0 name=Wear\nPIN w value out float 0.25\n"
    "NODE t VectorParameter 0 0 name=Tint\nPIN t value out float4 1,1,1,1\n"
    "NODE m Multiply 0 0\nPIN m a in float3\nPIN m b in float\nPIN m result out float3\n"
    "NODE o MaterialOutput 0 0\nPIN o BaseColor in float3\nPIN o Roughness in float\n"
    "LINK t.value m.a\nLINK w.value m.b\nLINK m.result o.BaseColor\nLINK w.value o.Roughness\n";

static void testParameters() {
    AVER_INFO("=== graph parameters ===");
    fmt::OcGraphData g = parse(kParamGraph);
    const auto list = fmt::listMaterialGraphParameters(g);
    check(list.size() == 2 && list[0].name == "Wear" && !list[0].isVector && list[0].defaultValue == "0.25", "scalar listed with its default");
    check(list[1].name == "Tint" && list[1].isVector, "vector listed");

    const pbr::MaterialGraphBody base = pbr::compileMaterialGraph(g);
    check(base.ok, "the parent graph compiles: " + base.error);
    check(contains(base.hlsl, "0.25"), "the default is in the emitted HLSL");

    std::vector<std::string> unknown;
    std::string err;
    const std::vector<std::pair<std::string, std::string>> ov = {{"Wear", "0.75"}, {"Tint", "1,0.5,0.125,1"}, {"Ghost", "3"}};
    check(fmt::applyMaterialGraphParameters(g, ov, &unknown, &err), "overrides apply: " + err);
    check(unknown.size() == 1 && unknown[0] == "Ghost", "a name no node carries is reported, not fatal");
    const pbr::MaterialGraphBody inst = pbr::compileMaterialGraph(g);
    check(inst.ok && contains(inst.hlsl, "0.75") && contains(inst.hlsl, "0.5") && !contains(inst.hlsl, "0.25"),
          "the override replaces the default in the HLSL");
    check(inst.hlsl != base.hlsl, "instance and parent compile to different arms");

    fmt::OcGraphData g2 = parse(kParamGraph);
    check(!fmt::applyMaterialGraphParameters(g2, {{"Wear", "1,2"}}, nullptr, &err), "a vector for a scalar is refused: " + err);
    check(!fmt::applyMaterialGraphParameters(g2, {{"Tint", "1,2,3,4,5"}}, nullptr, &err), "five components are refused");
    check(fmt::applyMaterialGraphParameters(g2, {{"Tint", "0.5"}}, nullptr, &err), "a single number splats into a vector");

    check(fmt::materialParameterKey({{"b", "1"}, {"a", "2"}}) == fmt::materialParameterKey({{"a", "2"}, {"b", "1"}}),
          "the registry key does not depend on order");
    check(fmt::materialParameterKey({{"a", "2"}}) != fmt::materialParameterKey({{"a", "3"}}), "different values, different keys");

    std::string written = fmt::writeOcgraph(parse(kParamGraph));
    check(contains(written, "name=Wear") && contains(written, "name=Tint"), "the name= attribute survives a graph save");
}

// ------------------------------------------------------------------------------------ functions

// y = x * k, with k defaulting to 2.
static const char* kScaleFn =
    "OCGRAPH 1\nDOMAIN materialfunction\nNAME Scale\n"
    "NODE x FunctionInput 0 0 name=X\nPIN x value out float 0\n"
    "NODE k FunctionInput 0 0 name=K\nPIN k value out float 2\n"
    "NODE m Multiply 0 0\nPIN m a in float\nPIN m b in float\nPIN m result out float\n"
    "NODE y FunctionOutput 0 0 name=Y\nPIN y value in float\n"
    "LINK x.value m.a\nLINK k.value m.b\nLINK m.result y.value\n";

static const char* kCaller =
    "OCGRAPH 1\nDOMAIN material\nNAME G\n"
    "NODE c ConstFloat 0 0\nPIN c value out float 0.125\n"
    "NODE f CallFunction 0 0 function=Fn/Scale.ocgraph\nPIN f X in float\nPIN f K in float\nPIN f Y out float\n"
    "NODE o MaterialOutput 0 0\nPIN o Roughness in float\n"
    "LINK c.value f.X\nLINK f.Y o.Roughness\n";

static bool hasType(const fmt::OcGraphData& g, const char* t) {
    for (const auto& n : g.nodes) if (n.type == t) return true;
    return false;
}

static void testFunctionInlining() {
    AVER_INFO("=== material function inlining ===");
    Files f;
    f["Fn/Scale.ocgraph"] = kScaleFn;
    fmt::OcGraphData g = parse(kCaller);
    std::string err;
    check(fmt::inlineMaterialFunctions(g, graphReader(f), &err), "inlines: " + err);
    check(!hasType(g, "CallFunction") && !hasType(g, "FunctionInput") && !hasType(g, "FunctionOutput"),
          "no call or boundary nodes remain");
    const pbr::MaterialGraphBody b = pbr::compileMaterialGraph(g);
    check(b.ok, "the inlined graph compiles: " + b.error);
    check(contains(b.hlsl, "0.125") && contains(b.hlsl, "2.0"), "caller's wire and the default K=2 both reach the body");

    // The call's own typed literal beats the function's default.
    fmt::OcGraphData g2 = parse(kCaller);
    for (auto& n : g2.nodes) if (n.id == "f") for (auto& p : n.pins) if (p.name == "K") p.defaultValue = "8";
    check(fmt::inlineMaterialFunctions(g2, graphReader(f), &err), "inlines with a typed literal");
    const pbr::MaterialGraphBody b2 = pbr::compileMaterialGraph(g2);
    check(b2.ok && contains(b2.hlsl, "8.0"), "the call's literal replaces the function default");

    // Two calls of one function do not share nodes.
    fmt::OcGraphData g3 = parse(kCaller);
    fmt::OcGraphNode second = g3.nodes[1];
    second.id = "f2";
    g3.nodes.push_back(second);
    g3.links.push_back({"c", "value", "f2", "X"});
    check(fmt::inlineMaterialFunctions(g3, graphReader(f), &err), "a second call inlines");
    usize mults = 0;
    for (const auto& n : g3.nodes) if (n.type == "Multiply") ++mults;
    check(mults == 2, "each call gets its own copy of the body");

    // Nesting: Outer calls Scale.
    f["Fn/Outer.ocgraph"] =
        "OCGRAPH 1\nDOMAIN materialfunction\nNAME Outer\n"
        "NODE a FunctionInput 0 0 name=A\nPIN a value out float 1\n"
        "NODE s CallFunction 0 0 function=Fn/Scale.ocgraph\nPIN s X in float\nPIN s Y out float\n"
        "NODE r FunctionOutput 0 0 name=R\nPIN r value in float\n"
        "LINK a.value s.X\nLINK s.Y r.value\n";
    fmt::OcGraphData g4 = parse(kCaller);
    for (auto& n : g4.nodes) if (n.id == "f") { n.extraTokens = {"function=Fn/Outer.ocgraph"}; n.pins = {{"A", "float", false, ""}, {"R", "float", true, ""}}; }
    g4.links = {{"c", "value", "f", "A"}, {"f", "R", "o", "Roughness"}};
    check(fmt::inlineMaterialFunctions(g4, graphReader(f), &err), "a nested call inlines: " + err);
    check(!hasType(g4, "CallFunction") && pbr::compileMaterialGraph(g4).ok, "nested result compiles");
}

static void testFunctionErrors() {
    AVER_INFO("=== material function errors ===");
    Files f;
    f["Fn/Scale.ocgraph"] = kScaleFn;
    std::string err;

    f["Fn/A.ocgraph"] =
        "OCGRAPH 1\nDOMAIN materialfunction\nNODE c CallFunction 0 0 function=Fn/B.ocgraph\nPIN c Y out float\n"
        "NODE r FunctionOutput 0 0 name=Y\nPIN r value in float\nLINK c.Y r.value\n";
    f["Fn/B.ocgraph"] =
        "OCGRAPH 1\nDOMAIN materialfunction\nNODE c CallFunction 0 0 function=Fn/A.ocgraph\nPIN c Y out float\n"
        "NODE r FunctionOutput 0 0 name=Y\nPIN r value in float\nLINK c.Y r.value\n";
    fmt::OcGraphData g = parse(kCaller);
    for (auto& n : g.nodes) if (n.id == "f") n.extraTokens = {"function=Fn/A.ocgraph"};
    check(!fmt::inlineMaterialFunctions(g, graphReader(f), &err), "mutual recursion is refused");
    check(contains(err, "cycle") && contains(err, "fn/a.ocgraph") && contains(err, "fn/b.ocgraph"), "the error names the chain: " + err);

    f["Fn/Self.ocgraph"] =
        "OCGRAPH 1\nDOMAIN materialfunction\nNODE c CallFunction 0 0 function=Fn/Self.ocgraph\nPIN c Y out float\n";
    fmt::OcGraphData g2 = parse(kCaller);
    for (auto& n : g2.nodes) if (n.id == "f") n.extraTokens = {"function=Fn/Self.ocgraph"};
    check(!fmt::inlineMaterialFunctions(g2, graphReader(f), &err) && contains(err, "cycle"), "direct self-call is refused");

    fmt::OcGraphData g3 = parse(kCaller);
    g3.links[1].sourcePin = "Nope";
    check(!fmt::inlineMaterialFunctions(g3, graphReader(f), &err) && contains(err, "Nope"), "a missing output is named: " + err);

    fmt::OcGraphData g4 = parse(kCaller);
    for (auto& n : g4.nodes) if (n.id == "f") n.extraTokens = {"function=Fn/Missing.ocgraph"};
    check(!fmt::inlineMaterialFunctions(g4, graphReader(f), &err) && contains(err, "Missing"), "a missing file is named");

    f["Fn/NotFn.ocgraph"] = "OCGRAPH 1\nDOMAIN material\nNAME N\n";
    fmt::OcGraphData g5 = parse(kCaller);
    for (auto& n : g5.nodes) if (n.id == "f") n.extraTokens = {"function=Fn/NotFn.ocgraph"};
    check(!fmt::inlineMaterialFunctions(g5, graphReader(f), &err) && contains(err, "materialfunction"), "a non-function graph is refused");

    fmt::OcGraphData g6 = parse(kCaller);
    check(!pbr::compileMaterialGraph(g6).ok, "an un-inlined CallFunction does not compile");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testFormatRoundTrip();
    testChainResolution();
    testChainErrors();
    testParameters();
    testFunctionInlining();
    testFunctionErrors();
    if (g_failures == 0) AVER_INFO("=== all material instance tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
