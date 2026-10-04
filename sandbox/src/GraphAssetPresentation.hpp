#pragma once
// How the Content Browser should PRESENT a .ocgraph asset -- its label and which icon/colour
// FAMILY it belongs to -- as a function of the file's DOMAIN record.
//
// Separate from aver::fmt::OcGraphDomain (aver/formats/OcGraph.hpp), which answers "is this graph
// mine" for a COMPILER (GameApp's class sweep, HostBridge.DeclareGraphClasses). This answers "what
// does the person browsing Content see", which is SandboxApp's question alone -- and it is kept free
// of ImGui and every other editor dependency so a headless test can drive it straight off a real
// .ocgraph fixture without a window. See GraphAssetPresentationTest.cpp.
//
// GAMEPLAY AND UNKNOWN DO NOT COLLAPSE INTO ONE "NOT MATERIAL" BUCKET. OcGraphDomain already
// distinguishes an absent record (every graph written before DOMAIN existed, and this build's own
// default -- safe to treat as gameplay) from a name this build does not recognise (someone else's
// convention, or a typo -- unsafe to treat as anything). Presenting both as the same generic "Graph"
// would erase that distinction in the one place a person actually looks at the file. See
// OcGraphDomain's own comment in OcGraph.hpp for the fuller argument.
#include "aver/formats/OcGraph.hpp"

namespace aver::editor {

enum class GraphAssetFamily {
    Gameplay,   // absent DOMAIN, or `DOMAIN gameplay` -- the ordinary visual-scripting graph
    Material,   // `DOMAIN material` -- shades a surface; presents as a material asset, not a graph
    Unknown,    // a DOMAIN this build does not recognise -- deliberately neither of the above
};

struct GraphAssetPresentation {
    GraphAssetFamily family;
    const char* label;   // what the Content Browser calls this asset
};

// The one place a Content Browser row asks "what am I drawing". Total over OcGraphDomain's three
// values, so a domain this build adds later without a matching `case` here falls through the
// `default` as Gameplay -- which is at least the SAFE mistake (a graph readable as one, not one
// silently claimed as a material) rather than a compile error nobody sees before the browser ships.
inline GraphAssetPresentation graphAssetPresentationFor(aver::fmt::OcGraphDomain domain) {
    switch (domain) {
        case aver::fmt::OcGraphDomain::Material:
            return GraphAssetPresentation{GraphAssetFamily::Material, "Material Graph"};
        case aver::fmt::OcGraphDomain::Unknown:
            return GraphAssetPresentation{GraphAssetFamily::Unknown, "Unknown Graph"};
        case aver::fmt::OcGraphDomain::Gameplay:
        default:
            return GraphAssetPresentation{GraphAssetFamily::Gameplay, "Graph"};
    }
}

} // namespace aver::editor
