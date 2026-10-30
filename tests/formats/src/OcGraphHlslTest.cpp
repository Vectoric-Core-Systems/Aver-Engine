// OcGraphHlslTest -- the EmbeddedHlsl / EmbeddedHlslEnv nodes of an .ocgraph (OcGraphHlsl.hpp): the writer's spellings, the round trip, and the older files.
//
// Files written before the rename carry `NODE gd GodotShader` / `NODE gd GodotEnvShader` and `DOMAIN godotenv`, and their embedded HLSL uses the old
// spellings (AVER_GD_*, Gd*Sampler*, gd_*, GDN). Those files must keep loading: the reader takes the old node types and maps the old identifiers in
// the sources to today's, and the material graph compiler accepts the result. Pure logic: no device, no shader compiler.
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/pbr/MaterialGraphHlsl.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// The old file text for a shader node: the sources percent-escaped into one chunk each, as the old writer did.
static std::string oldMaterialFile(const std::string& fragment, const std::string& vertex) {
    std::string t = "OCGRAPH 1\nDOMAIN material\nNAME legacy\n\nNODE gd GodotShader name=legacy ns=gs12345678_ nu=1 ni=0 ng=0 fx=0 hasfs=1 hasvs=1";
    t += " vary=smooth:1:" + fmt::hlslGraphEscape("float3 gs12345678_gdv_p");
    t += " fs0=" + fmt::hlslGraphEscape(fragment);
    t += " vs0=" + fmt::hlslGraphEscape(vertex) + "\n";
    return t;
}

int main() {
    AVER_INFO("=== OcGraphHlslTest ===");

    // ---- the writer uses the new spellings and round-trips ----
    {
        fmt::EmbeddedHlslPart p;
        p.name = "fresh";
        p.ns = "ge0000abcd_";
        p.regsU = 3;
        p.hasFragment = true;
        p.fragmentSource = "void EMBN(main_fragment)() { AVER_CUSTOM_ALBEDO = float3(1.0, 0.0, 0.0); }\n";
        fmt::OcGraphData g;
        fmt::packEmbeddedHlsl(p, g);
        const std::string text = fmt::writeOcgraph(g);
        check(has(text, "NODE hlsl EmbeddedHlsl ") && has(text, "DOMAIN material") && !has(text, "GodotShader"), "a packed graph is written with NODE hlsl EmbeddedHlsl");
        fmt::OcGraphData back;
        std::string err;
        fmt::EmbeddedHlslPart q;
        check(fmt::parseOcgraph(text, back, &err) && fmt::ocGraphHasEmbeddedHlsl(back) && fmt::unpackEmbeddedHlsl(back, q, &err), "and reads back (" + err + ")");
        check(q.fragmentSource == p.fragmentSource && q.regsU == 3 && q.ns == p.ns, "unchanged: a new file's source is not rewritten");
        check(pbr::compileMaterialGraph(back).isEmbedded, "the material graph compiler takes it");

        fmt::EmbeddedHlslEnvPart e;
        e.kind = "sky";
        e.name = "dusk";
        e.ns = "ge0000abcd_";
        e.regs = 1;
        e.defaults = {1.0f, 2.0f, 3.0f, 4.0f};
        e.source = "void ge0000abcd_main_sky() { AVER_CUSTOM_COLOR = float3(0.1, 0.2, 0.3); }\n";
        fmt::OcGraphData ge;
        fmt::packEmbeddedHlslEnv(e, ge);
        const std::string etext = fmt::writeOcgraph(ge);
        check(has(etext, "DOMAIN customenv") && has(etext, "NODE hlsl EmbeddedHlslEnv ") && !has(etext, "godotenv") && !has(etext, "GodotEnvShader"), "an environment graph is written with DOMAIN customenv");
        fmt::OcGraphData eback;
        fmt::EmbeddedHlslEnvPart eq;
        check(fmt::parseOcgraph(etext, eback, &err) && fmt::ocGraphHasEmbeddedHlslEnv(eback) && fmt::unpackEmbeddedHlslEnv(eback, eq, &err) && eq.source == e.source && eq.defaults == e.defaults,
              "and reads back (" + err + ")");
    }

    // ---- an older material file still loads, and its sources are brought up to today's HLSL names ----
    {
        const std::string frag =
            "GdSampler2D GDN(gdsm_noise)() { GdSampler2D r; r.slot = 1u; r.m = 1u; return r; }\n"
            "void GDN(main_fragment)()\n{\n"
            "    float4 l_n = gd_texture(GDN(gdsm_noise)(), AVER_GD_UV);\n"
            "    AVER_GD_ALBEDO = gd_mix(AVER_GD_UBUF[0].xyz, l_n.xyz, 0.5) + AVER_GD_VARYINGS.gs12345678_gdv_p;\n"
            "}\n";
        const std::string vert =
            "void GDN(main_vertex)()\n{\n    AVER_GD_VARYINGS.gs12345678_gdv_p = AVER_GD_VERTEX;\n}\n";
        const std::string text = oldMaterialFile(frag, vert);
        fmt::OcGraphData g;
        std::string err;
        check(fmt::parseOcgraph(text, g, &err), "an old GodotShader graph parses (" + err + ")");
        check(fmt::ocGraphHasEmbeddedHlsl(g), "and is recognised as an embedded HLSL graph");
        fmt::EmbeddedHlslPart p;
        check(fmt::unpackEmbeddedHlsl(g, p, &err), "its node unpacks (" + err + ")");
        check(p.hasFragment && p.hasVertex && p.regsU == 1 && p.ns == "gs12345678_" && p.varyings.size() == 1 && p.varyings[0].decl == "float3 gs12345678_gdv_p", "with its attributes");
        check(has(p.fragmentSource, "EmbSampler2D EMBN(gdsm_noise)() { EmbSampler2D r;") && has(p.fragmentSource, "emb_texture(EMBN(gdsm_noise)(), AVER_CUSTOM_UV)") &&
                  has(p.fragmentSource, "AVER_CUSTOM_ALBEDO = emb_mix(AVER_CUSTOM_UBUF[0].xyz") && has(p.fragmentSource, "AVER_CUSTOM_VARYINGS.gs12345678_gdv_p"),
              "the old HLSL names in the fragment source read as the new ones");
        check(!has(p.fragmentSource, "AVER_GD_") && !has(p.fragmentSource, "GDN") && !has(p.fragmentSource, "gd_") && !has(p.fragmentSource, "GdSampler"), "and none of the old ones is left");
        check(has(p.vertexSource, "EMBN(main_vertex)") && has(p.vertexSource, "AVER_CUSTOM_VERTEX") && !has(p.vertexSource, "AVER_GD_"), "likewise the vertex source");
        check(has(p.fragmentSource, "gdsm_noise") && has(p.fragmentSource, "gs12345678_gdv_p"), "the translator's own private names are left as they were");
        const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(g);
        check(body.ok && body.isEmbedded && body.emb.fragmentSource == p.fragmentSource, "the material graph compiler takes the old file (" + body.error + ")");

        // Saved again, the graph is written with the new spellings.
        fmt::EmbeddedHlslPart again = p;
        fmt::packEmbeddedHlsl(again, g);
        const std::string rewritten = fmt::writeOcgraph(g);
        check(has(rewritten, "EmbeddedHlsl ") && !has(rewritten, "GodotShader"), "an old graph packed again carries the new node type");
    }

    // ---- an older environment graph (DOMAIN godotenv, GodotEnvShader) ----
    {
        const std::string src = "float4 ge1_f() { return gd_mix(float4(0, 0, 0, 0), float4(1, 1, 1, 1), 0.5); }\nvoid ge1_main_sky() { AVER_GD_COLOR = ge1_f().xyz; }\n";
        const std::string text = "OCGRAPH 1\nDOMAIN godotenv\nNAME legacySky\n\nNODE gd GodotEnvShader kind=sky name=legacySky ns=ge1_ regs=1 gregs=0 start=0 process=0 reg=3F800000,00000000,00000000,3F800000 src0=" +
                                 fmt::hlslGraphEscape(src) + "\n";
        fmt::OcGraphData g;
        std::string err;
        fmt::EmbeddedHlslEnvPart e;
        check(fmt::parseOcgraph(text, g, &err) && fmt::ocGraphHasEmbeddedHlslEnv(g) && fmt::unpackEmbeddedHlslEnv(g, e, &err), "an old GodotEnvShader graph unpacks (" + err + ")");
        check(e.kind == "sky" && e.regs == 1 && e.defaults.size() == 4 && e.defaults[0] == 1.0f, "with its registers");
        check(has(e.source, "emb_mix(") && has(e.source, "AVER_CUSTOM_COLOR") && !has(e.source, "gd_mix") && !has(e.source, "AVER_GD_") && has(e.source, "ge1_f()"), "and its source brought up to today's names");
    }

    if (g_failures) {
        AVER_ERROR("OcGraphHlslTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("OcGraphHlslTest: ok");
    return 0;
}
