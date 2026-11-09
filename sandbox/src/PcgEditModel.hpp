#pragma once
// The PCG graph editor's rules (WAVE2_UE W3 step 6), without a window: the node palette with search and categories, the parameter schema of each
// node (so the inspector can show keys the node reads even before they are set), adding and removing nodes and links with the evaluator's own
// pin vocabulary, validation, a preview run that gives every node a point count, and the points one node outputs (for the viewport debug draw).
// Header-only over aver::world's evaluator, GraphEditorGeometry's link rules and OcGraph; no ImGui, no scene, no GPU.
//
// The schema is the list of keys each node function in modules/world/src/PcgNodes.cpp reads, with their defaults. A key the schema does not
// know is still shown and edited as a leftover token (GraphEditorGeometry's computeAttributeRows), so a hand-written graph loses nothing.
#include "GraphEditorGeometry.hpp"

#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/world/PcgGraph.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::editor::pcged {

enum class ParamKind : u8 { Num, Int, Flag, Text, Vec3, Polyline };

struct ParamSpec {
    const char* key;
    ParamKind kind;
    const char* def;     // what the evaluator uses when the key is absent, as text ("" = none / unset)
    const char* label;
};

struct NodeSpec {
    const char* type;
    const char* category;
    std::vector<ParamSpec> params;
};

inline const std::vector<NodeSpec>& specs() {
    using K = ParamKind;
    static const std::vector<NodeSpec> s = {
        {"CreatePointsGrid", "Samplers", {{"spacing", K::Num, "200", "Spacing (cm)"}, {"z", K::Num, "0", "Z (cm)"}}},
        {"CreatePointsRandom", "Samplers", {{"count", K::Int, "100", "Count"}, {"randomYaw", K::Flag, "1", "Random yaw"}, {"scaleMin", K::Num, "1", "Scale min"},
                                            {"scaleMax", K::Num, "1", "Scale max"}, {"minSpacing", K::Num, "0", "Min spacing (cm)"}}},
        {"SurfaceSampler", "Samplers", {{"spacing", K::Num, "200", "Spacing (cm)"}, {"align", K::Flag, "0", "Align to normal"}, {"zOffset", K::Num, "0", "Z offset (cm)"}}},
        {"SplineSampler", "Samplers", {{"name", K::Text, "", "Spline name"}, {"pts", K::Polyline, "", "Inline points"}, {"spacing", K::Num, "200", "Spacing (cm)"},
                                       {"jitter", K::Num, "0", "Jitter (cm)"}}},
        {"VolumeSampler", "Samplers", {{"volume", K::Text, "", "Volume name"}, {"min", K::Vec3, "", "Box min"}, {"max", K::Vec3, "", "Box max"}, {"spacing", K::Num, "200", "Spacing (cm)"}}},
        {"BoundsFilter", "Filters", {{"volume", K::Text, "", "Volume name"}, {"min", K::Vec3, "", "Box min"}, {"max", K::Vec3, "", "Box max"}}},
        {"DensityNoise", "Density", {{"cell", K::Num, "", "Cell (cm)"}, {"octaves", K::Int, "3", "Octaves"}, {"seed", K::Int, "0", "Seed"}, {"mode", K::Text, "set", "Mode (set, mul, add, max)"},
                                     {"scale", K::Num, "1", "Scale"}, {"bias", K::Num, "0", "Bias"}, {"use3d", K::Flag, "0", "3D noise"}}},
        {"DensityFilter", "Density", {{"min", K::Num, "0.5", "Min"}, {"max", K::Num, "", "Max"}}},
        {"DensityRemap", "Density", {{"inMin", K::Num, "0", "In min"}, {"inMax", K::Num, "1", "In max"}, {"outMin", K::Num, "0", "Out min"}, {"outMax", K::Num, "1", "Out max"},
                                     {"clamp", K::Flag, "1", "Clamp"}}},
        {"AttributeFilter", "Attributes", {{"attr", K::Text, "density", "Attribute"}, {"op", K::Text, ">=", "Compare"}, {"value", K::Num, "0.5", "Value"}}},
        {"AttributeMath", "Attributes", {{"dst", K::Text, "density", "Destination"}, {"a", K::Text, "density", "A"}, {"b", K::Text, "", "B (or use Value)"},
                                         {"op", K::Text, "mul", "Operation"}, {"value", K::Num, "1", "Value"}}},
        {"AttributeSet", "Attributes", {{"name", K::Text, "", "Attribute"}, {"value", K::Num, "0", "Value"}, {"min", K::Num, "", "Random min"}, {"max", K::Num, "", "Random max"}}},
        {"TransformPoints", "Transform", {{"posMin", K::Vec3, "", "Position min"}, {"posMax", K::Vec3, "", "Position max"}, {"rotMin", K::Vec3, "", "Rotation min (deg)"},
                                          {"rotMax", K::Vec3, "", "Rotation max (deg)"}, {"scaleMin", K::Vec3, "1,1,1", "Scale min"}, {"scaleMax", K::Vec3, "1,1,1", "Scale max"},
                                          {"uniformScale", K::Flag, "1", "Uniform scale"}}},
        {"SelfPrune", "Transform", {{"radius", K::Num, "0", "Radius (cm, 0 = per-point)"}, {"priority", K::Text, "density", "Priority attribute"}, {"mode", K::Text, "greedy", "Mode (greedy, local)"},
                                    {"dims3", K::Flag, "0", "3D distance"}}},
        {"Difference", "Set operations", {{"radius", K::Num, "1", "Radius (cm)"}, {"dims3", K::Flag, "0", "3D distance"}}},
        {"Union", "Set operations", {{"dedup", K::Num, "0", "Drop duplicates within (cm)"}}},
        {"Intersect", "Set operations", {{"radius", K::Num, "1", "Radius (cm)"}, {"dims3", K::Flag, "0", "3D distance"}}},
        {"ProjectToSurface", "Landscape", {{"align", K::Flag, "0", "Align to normal"}, {"drop", K::Flag, "1", "Drop points off the surface"}, {"zOffset", K::Num, "0", "Z offset (cm)"},
                                           {"alignMax", K::Num, "90", "Max align angle (deg)"}}},
        {"SlopeFilter", "Landscape", {{"min", K::Num, "0", "Min (deg)"}, {"max", K::Num, "30", "Max (deg)"}, {"source", K::Text, "point", "Source (point, landscape)"}}},
        {"HeightFilter", "Landscape", {{"min", K::Num, "", "Min Z (cm)"}, {"max", K::Num, "", "Max Z (cm)"}}},
        {"LayerWeightFilter", "Landscape", {{"layer", K::Text, "", "Paint layer"}, {"min", K::Num, "0.5", "Min"}, {"max", K::Num, "1", "Max"}}},
        {"DistanceToSplineFilter", "Filters", {{"name", K::Text, "", "Spline name"}, {"min", K::Num, "0", "Min (cm)"}, {"max", K::Num, "", "Max (cm)"}}},
        {"DistanceToActorFilter", "Filters", {{"tag", K::Text, "", "Actor tag"}, {"min", K::Num, "0", "Min (cm)"}, {"max", K::Num, "", "Max (cm)"}}},
        {"NoiseMask", "Biomes", {{"cell", K::Num, "", "Cell (cm)"}, {"octaves", K::Int, "3", "Octaves"}, {"seed", K::Int, "0", "Seed"}, {"min", K::Num, "0.5", "Min"}, {"max", K::Num, "", "Max"}}},
        {"EdgeFalloff", "Biomes", {{"source", K::Text, "layer", "Source (layer, spline, volume)"}, {"name", K::Text, "", "Name"}, {"edge", K::Num, "0", "Edge width"},
                                   {"min", K::Num, "0.5", "Min"}, {"half", K::Num, "500", "Half width (cm)"}, {"invert", K::Flag, "0", "Invert"}}},
        {"Clump", "Biomes", {{"radius", K::Num, "0", "Radius (cm)"}, {"min", K::Int, "3", "Min children"}, {"max", K::Int, "6", "Max children"}, {"chance", K::Num, "1", "Chance"},
                             {"spacing", K::Num, "0", "Sibling spacing (cm)"}, {"keepParent", K::Flag, "0", "Keep parent"}}},
        {"Compete", "Biomes", {{"radiusAttr", K::Text, "radius", "Radius attribute"}, {"priorityAttr", K::Text, "priority", "Priority attribute"}, {"overlap", K::Num, "1", "Overlap"},
                               {"maxRadius", K::Num, "0", "Max radius (cm)"}, {"mode", K::Text, "greedy", "Mode (greedy, local)"}, {"dims3", K::Flag, "0", "3D distance"}}},
        {"WeightedMeshSpawner", "Spawners", {{"meshes", K::Text, "", "Meshes (path:weight,...)"}, {"material", K::Text, "", "Material"}, {"cullStart", K::Num, "0", "Cull start (cm)"},
                                             {"cullEnd", K::Num, "0", "Cull end (cm)"}, {"lodBias", K::Num, "1", "LOD bias"}, {"collision", K::Int, "0", "Collision flags"}}},
        {"ActorSpawner", "Spawners", {{"class", K::Text, "", "Actor class"}, {"radius", K::Num, "0", "Prune radius (cm)"}, {"mode", K::Text, "greedy", "Prune mode"}}},
        {"Input", "Flow", {}},
        {"Output", "Flow", {}},
        {"Subgraph", "Flow", {{"graph", K::Text, "", "Graph"}}},
        {"Loop", "Flow", {{"graph", K::Text, "", "Graph"}, {"max", K::Int, "", "Max iterations"}}},
    };
    return s;
}

inline const NodeSpec* specOf(std::string_view type) {
    for (const NodeSpec& s : specs()) if (type == s.type) return &s;
    return nullptr;
}

// The (key, label) pairs computeAttributeRows takes.
inline std::vector<std::pair<std::string, std::string>> declaredKeys(std::string_view type) {
    std::vector<std::pair<std::string, std::string>> out;
    if (const NodeSpec* s = specOf(type)) for (const ParamSpec& p : s->params) out.emplace_back(p.key, p.label);
    return out;
}

inline const ParamSpec* paramSpec(std::string_view type, std::string_view key) {
    if (const NodeSpec* s = specOf(type)) for (const ParamSpec& p : s->params) if (key == p.key) return &p;
    return nullptr;
}

// ---- palette ----
struct PaletteEntry {
    std::string type, category, doc;
    int inputs = 0, outputs = 0;
    bool sink = false;
};

inline std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

// Every evaluator node, ordered by category (the order the schema lists them) then name, filtered by a case-insensitive substring of the type,
// category or description. An empty search lists them all.
inline std::vector<PaletteEntry> palette(std::string_view search = {}) {
    const std::string needle = lower(search);
    std::vector<PaletteEntry> out;
    for (const world::PcgNodeDef& d : world::pcgNodeDefs()) {
        PaletteEntry e;
        e.type = d.type;
        const NodeSpec* s = specOf(d.type);
        e.category = s ? s->category : "Other";
        e.doc = d.doc ? d.doc : "";
        e.inputs = static_cast<int>(d.inputs.size());
        e.outputs = static_cast<int>(d.outputs.size());
        e.sink = d.sink;
        if (!needle.empty() && lower(e.type).find(needle) == std::string::npos && lower(e.category).find(needle) == std::string::npos &&
            lower(e.doc).find(needle) == std::string::npos)
            continue;
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) { return a.category < b.category; });
    return out;
}

// ---- editing ----
inline bool hasNode(const fmt::OcGraphData& g, std::string_view id) {
    for (const fmt::OcGraphNode& n : g.nodes) if (n.id == id) return true;
    return false;
}

// "grid", "grid2", ... from the type name: lower-cased first letters, unique in the graph.
inline std::string freshNodeId(const fmt::OcGraphData& g, std::string_view type) {
    std::string base = lower(type);
    for (int i = 1;; ++i) {
        std::string id = i == 1 ? base : base + std::to_string(i);
        if (!hasNode(g, id)) return id;
    }
}

// Adds a node of an evaluator type with its standard pins at (x, y). False for a type the evaluator does not know. A graph with no DOMAIN record
// is given `DOMAIN pcg` (the profile marker).
inline bool addNode(fmt::OcGraphData& g, std::string_view type, f64 x, f64 y, std::string* outId = nullptr) {
    if (!world::pcgFindNode(type)) return false;
    if (g.domain.empty()) g.domain = world::kPcgDomain;
    const std::string id = freshNodeId(g, type);
    fmt::OcGraphNode n = world::pcgMakeNode(id, type);
    n.x = x; n.y = y;
    n.hasPosition = true;
    g.nodes.push_back(std::move(n));
    if (outId) *outId = id;
    return true;
}

// Removes the node, every link touching it, and any OUT / ENTRY records naming it.
inline bool removeNode(fmt::OcGraphData& g, std::string_view id) {
    const auto it = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const fmt::OcGraphNode& n) { return n.id == id; });
    if (it == g.nodes.end()) return false;
    g.nodes.erase(it);
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const fmt::OcGraphLink& l) { return l.sourceNode == id || l.destNode == id; }), g.links.end());
    g.outputs.erase(std::remove_if(g.outputs.begin(), g.outputs.end(), [&](const auto& o) { return o.first == id; }), g.outputs.end());
    g.entryPoints.erase(std::remove_if(g.entryPoints.begin(), g.entryPoints.end(), [&](const auto& o) { return o.first == id; }), g.entryPoints.end());
    return true;
}

inline bool removeLink(fmt::OcGraphData& g, usize index) {
    if (index >= g.links.size()) return false;
    g.links.erase(g.links.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// ---- parameters ----
// Writes `key=value`; an empty value, or one equal to the schema default, removes the token (the file then says only what the author changed).
inline void setParam(fmt::OcGraphNode& n, const std::string& key, const std::string& value) {
    const ParamSpec* spec = paramSpec(n.type, key);
    if (value.empty() || (spec && value == spec->def && spec->def[0])) removeNodeAttribute(n, key);
    else setNodeAttribute(n, key, value);
}

// ---- pins ----
// Packed 0xAABBGGRR (ImGui's IM_COL32 layout) so the editor can use it directly: points are one type today; an unknown type falls back to grey.
inline u32 pinColor(std::string_view type) {
    const auto col = [](u32 r, u32 g, u32 b) { return 0xFF000000u | (b << 16) | (g << 8) | r; };
    if (type == "points") return col(110, 200, 255);
    return col(170, 170, 170);
}

// ---- problems ----
struct Problem {
    std::string node;    // empty = the graph
    std::string text;
    bool error = true;
};
inline std::vector<Problem> problems(const fmt::OcGraphData& g) {
    std::vector<Problem> out;
    for (const world::PcgDiag& d : world::pcgValidate(g)) out.push_back({d.node, d.message, d.error});
    return out;
}

// ---- preview ----
struct PreviewSetup {
    i32 seed = 0;
    world::PcgBox bounds{Vec3{-5000, -5000, -100000}, Vec3{5000, 5000, 100000}};
    const world::ILandscapeQuery* landscape = nullptr;
    std::map<std::string, std::vector<Vec3>> splines;
    std::map<std::string, world::PcgBox> volumes;
    std::function<const fmt::OcGraphData*(const std::string&)> graphLoader;
};

struct PreviewResult {
    bool ran = false;
    bool ok = false;
    std::map<std::string, u32> pointCount;      // node id -> points on its first output pin
    std::vector<Problem> diags;
    u32 instances = 0;
    u32 nodesRun = 0;
    world::PcgResult full;                      // for the sinks (Generate)
};

inline world::PcgContext contextOf(const PreviewSetup& s) {
    world::PcgContext c;
    c.seed = s.seed;
    c.bounds = s.bounds;
    c.landscape = s.landscape;
    c.splines = s.splines;
    c.volumes = s.volumes;
    c.graphLoader = s.graphLoader;
    c.budgetMs = 4000.0;
    return c;
}

inline PreviewResult runPreview(const fmt::OcGraphData& g, const PreviewSetup& s) {
    PreviewResult out;
    for (const Problem& p : problems(g)) if (p.error) { out.diags.push_back(p); }
    if (!out.diags.empty()) return out;   // an invalid graph is not run
    out.full = world::pcgEvaluate(g, contextOf(s));
    out.ran = true;
    out.ok = out.full.ok;
    out.pointCount = out.full.nodePointCount;
    out.instances = static_cast<u32>(out.full.instanceCount());
    out.nodesRun = out.full.nodesRun;
    for (const world::PcgDiag& d : out.full.diags) out.diags.push_back({d.node, d.message, d.error});
    return out;
}

// The points one node's output pin gives, for the viewport debug draw. The top-level evaluator runs from its sink nodes only, so the graph is
// copied, every real sink is dropped (nothing downstream of the tap needs to run) and a throwaway `ActorSpawner` is wired to the pin: it emits one
// spawn record per point, whose transform is the point. Empty for an unknown node or pin, or a graph that fails to run.
inline std::vector<Transform> debugPoints(const fmt::OcGraphData& g, const std::string& nodeId, const std::string& pin, const PreviewSetup& s,
                                          usize cap = 20000) {
    const fmt::OcGraphNode* node = nullptr;
    for (const fmt::OcGraphNode& n : g.nodes) if (n.id == nodeId) node = &n;
    if (!node) return {};
    bool pinOk = false;
    for (const fmt::OcGraphPin& p : node->pins) if (p.isOutput && p.name == pin) pinOk = true;
    if (!pinOk) return {};
    fmt::OcGraphData copy = g;
    std::vector<std::string> sinks;
    for (const fmt::OcGraphNode& n : copy.nodes) {
        const world::PcgNodeDef* d = world::pcgFindNode(n.type);
        if (d && d->sink && n.id != nodeId) sinks.push_back(n.id);
    }
    for (const std::string& id : sinks) removeNode(copy, id);
    std::string tap;
    if (!addNode(copy, "ActorSpawner", 0, 0, &tap)) return {};
    copy.links.push_back({nodeId, pin, tap, "in"});
    const world::PcgResult r = world::pcgEvaluate(copy, contextOf(s));
    std::vector<Transform> out;
    for (const world::PcgActorSpawn& a : r.actors) {
        if (out.size() >= cap) break;
        out.push_back(a.xf);
    }
    return out;
}

// A cross (three axis segments, two vertices each) of half-length `halfCm` at every point; the viewport debug draw's input.
inline void debugCrossSegments(const std::vector<Transform>& points, f32 halfCm, std::vector<Vec3>& out) {
    for (const Transform& t : points) {
        const Vec3 p = t.position;
        out.push_back(Vec3{p.x - halfCm, p.y, p.z}); out.push_back(Vec3{p.x + halfCm, p.y, p.z});
        out.push_back(Vec3{p.x, p.y - halfCm, p.z}); out.push_back(Vec3{p.x, p.y + halfCm, p.z});
        out.push_back(Vec3{p.x, p.y, p.z - halfCm}); out.push_back(Vec3{p.x, p.y, p.z + halfCm});
    }
}

}  // namespace aver::editor::pcged
