// The Content Browser's .ocgraph classification: sandbox/src/GraphAssetPresentation.hpp, which
// decides whether a graph presents as an ordinary Gameplay graph, a Material asset, or neither.
//
// WHY THIS TEST EXISTS. The browser classified every .ocgraph purely by EXTENSION, never asking the
// file's own DOMAIN record (fmt::ocGraphDomainOf, OcGraph.hpp) whether it names a material graph.
// docs/MATERIALS.md's own example, Content/Materials/MG_Bands.ocgraph, would have shown up with a
// generic blue "Graph" icon sorted beside gameplay graphs it shares nothing with.
//
// A REAL FIXTURE, WRITTEN THROUGH fmt::saveOcgraph -- not a hand-typed "OCGRAPH 1\nDOMAIN material\n"
// string. writeOcgraph (behind saveOcgraph) owns the actual on-disk grammar; a hand-typed string
// tests today's grammar as I remember it, not as the writer produces it, and the two have disagreed
// before (see the DOMAIN merge-on-save note in OcGraph.cpp). Round-tripping through the real
// writer+reader is what MaterialGraphTest and OcGraphTest already do for domain="material"; this
// test adds the ABSENT and UNKNOWN cases neither of those covers, and checks the BROWSER'S mapping
// on top, which lives in sandbox/src, not modules/formats.
#include "GraphAssetPresentation.hpp"

#include "aver/formats/OcGraph.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Writes `domain` (verbatim, "" for an absent DOMAIN record) through the REAL writer, reads it back
// through the REAL reader, and returns what the browser would present it as.
static editor::GraphAssetPresentation presentationForDomain(const std::filesystem::path& path,
                                                              const std::string& domain) {
    fmt::OcGraphData g;
    g.name = "Fixture";
    g.domain = domain;
    std::string err;
    if (!fmt::saveOcgraph(path.string(), g, &err)) {
        AVER_ERROR("saveOcgraph failed for domain '{}': {}", domain, err);
        return editor::graphAssetPresentationFor(fmt::OcGraphDomain::Unknown);
    }

    fmt::OcGraphData loaded;
    if (!fmt::loadOcgraph(path.string(), loaded, &err)) {
        AVER_ERROR("loadOcgraph failed for domain '{}': {}", domain, err);
        return editor::graphAssetPresentationFor(fmt::OcGraphDomain::Unknown);
    }
    return editor::graphAssetPresentationFor(fmt::ocGraphDomainOf(loaded));
}

int main() {
    AVER_INFO("GraphAssetPresentationTest");

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aver_graphassetpresentation_test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    // ---- the enum mapping itself, direct -------------------------------------------------
    AVER_INFO("-- direct domain -> presentation mapping --");
    {
        const auto p = editor::graphAssetPresentationFor(fmt::OcGraphDomain::Gameplay);
        check(p.family == editor::GraphAssetFamily::Gameplay, "Gameplay domain -> Gameplay family");
        check(std::string(p.label) == "Graph", "Gameplay domain labels as plain 'Graph'");
    }
    {
        const auto p = editor::graphAssetPresentationFor(fmt::OcGraphDomain::Material);
        check(p.family == editor::GraphAssetFamily::Material, "Material domain -> Material family");
        check(std::string(p.label) == "Material Graph", "Material domain labels as 'Material Graph'");
    }
    {
        const auto p = editor::graphAssetPresentationFor(fmt::OcGraphDomain::Unknown);
        check(p.family == editor::GraphAssetFamily::Unknown, "Unknown domain -> Unknown family");
        check(p.family != editor::GraphAssetFamily::Gameplay && p.family != editor::GraphAssetFamily::Material,
              "Unknown is presented as NEITHER Gameplay NOR Material -- the format deliberately "
              "distinguishes all three (OcGraphDomain's own comment), and the UI must not erase that");
    }

    // ---- through a REAL fixture, written by the actual writer, read by the actual reader --
    AVER_INFO("-- round-tripped through fmt::saveOcgraph / fmt::loadOcgraph --");

    {
        const auto p = presentationForDomain(root / "absent.ocgraph", "");
        check(p.family == editor::GraphAssetFamily::Gameplay,
              "a graph with NO DOMAIN record at all reads as Gameplay -- every .ocgraph written "
              "before the record existed must keep presenting as an ordinary graph");
    }
    {
        const auto p = presentationForDomain(root / "gameplay.ocgraph", "gameplay");
        check(p.family == editor::GraphAssetFamily::Gameplay, "`DOMAIN gameplay` reads as Gameplay");
    }
    {
        const auto p = presentationForDomain(root / "material.ocgraph", "material");
        check(p.family == editor::GraphAssetFamily::Material,
              "`DOMAIN material` -- docs/MATERIALS.md's own MG_Bands.ocgraph shape -- reads as "
              "Material, through the SAME writer avermatc's tooling and the graph editor use");
    }
    {
        // A domain no build of this engine has ever defined. ocGraphDomainOf's own contract: an
        // unrecognised name is Unknown, not silently accepted as Gameplay.
        const auto p = presentationForDomain(root / "unknown.ocgraph", "vfx");
        check(p.family == editor::GraphAssetFamily::Unknown,
              "`DOMAIN vfx` -- a name this build has never heard of -- reads as Unknown, not Gameplay");
    }

    AVER_INFO("{}/{} checks passed", g_checks - g_failures, g_checks);
    if (g_failures) AVER_ERROR("FAILED  {} check(s) failed", g_failures);
    return g_failures ? 1 : 0;
}
