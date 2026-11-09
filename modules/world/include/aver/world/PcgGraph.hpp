// The PCG graph evaluator (docs/PCG.md): a `DOMAIN pcg` .ocgraph run as typed point-set data flow.
//
// ONE EVALUATOR for the editor preview, the chunk streamer and tests. Every node is a pure function of
// (its params, its inputs, the node seed, the evaluation bounds), so results are deterministic and cacheable.
// Samplers are anchored to WORLD lattices and seed each point from its lattice index, never from a running count,
// which is what makes a cell-by-cell run equal a whole-volume run (PcgVolumeRunner.hpp).
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/world/PcgPoints.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::world {

// Landscape access, an interface so this module never links Aver.Landscape and tests use a fake heightfield.
class ILandscapeQuery {
public:
    virtual ~ILandscapeQuery() = default;
    // World cm in, surface Z out. False = no surface known at (x, y).
    virtual bool heightAt(f64 x, f64 y, f32& outZ) const = 0;
    // Unit surface normal (+Z up). The default differences heightAt over 50 cm.
    virtual bool normalAt(f64 x, f64 y, Vec3& outN) const;
    // Painted layer weight in [0, 1]; 0 for an unknown layer.
    virtual f32 layerWeight(std::string_view layer, f64 x, f64 y) const { (void)layer; (void)x; (void)y; return 0.0f; }
};

struct PcgBox {
    Vec3 min{0, 0, 0};
    Vec3 max{0, 0, 0};
    // Half-open in X and Y (a point on a shared edge belongs to exactly one cell), closed in Z.
    bool containsXY(const Vec3& p) const { return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }
    bool contains(const Vec3& p) const { return containsXY(p) && p.z >= min.z && p.z <= max.z; }
};

struct PcgDiag {
    std::string node;      // node id; empty = the graph as a whole
    std::string message;
    bool error = false;    // false = warning
};

struct PcgInstance {
    Transform xf;
    i32 seed = 0;
};
struct PcgInstanceGroup {
    std::string mesh;
    std::string material;
    std::vector<PcgInstance> items;
};
struct PcgActorSpawn {
    std::string actorClass;
    Transform xf;
    i32 seed = 0;
};

// What one node produced, and what the evaluator caches.
struct PcgNodeOut {
    std::vector<PcgPoints> pins;                // one per declared output pin, in declaration order
    std::vector<PcgInstanceGroup> groups;       // spawner nodes only
    std::vector<PcgActorSpawn> actors;
    std::vector<PcgDiag> diags;
};

struct PcgCache {
    std::unordered_map<u64, std::shared_ptr<const PcgNodeOut>> entries;
    u64 hits = 0, misses = 0;
    void clear() { entries.clear(); hits = misses = 0; }
};

struct PcgContext {
    i32 seed = 0;
    PcgBox bounds;                                           // evaluation region
    const ILandscapeQuery* landscape = nullptr;
    std::map<std::string, std::vector<Vec3>> splines;        // name -> polyline
    std::map<std::string, std::vector<Vec3>> actors;         // tag -> positions
    std::map<std::string, PcgBox> volumes;                   // name -> box
    // Resolves a Subgraph/Loop `graph=` name. Null result = unknown graph.
    std::function<const fmt::OcGraphData*(const std::string&)> graphLoader;
    // Folded into every cache key. The host bumps it when landscape/splines/actors change under a fixed graph.
    u64 environmentStamp = 0;

    const std::atomic<bool>* cancel = nullptr;
    f64 budgetMs = 0.0;          // wall-clock budget for one evaluate(); 0 = unlimited
    u32 maxPoints = 4'000'000;   // per node output; larger is truncated with an error
    bool partitioned = false;    // set by the cell runner; count-based nodes warn
    PcgCache* cache = nullptr;
};

struct PcgResult {
    bool ok = true;              // false on any error diagnostic, cancellation or timeout
    bool cancelled = false;
    bool timedOut = false;
    std::vector<PcgInstanceGroup> groups;   // merged by (mesh, material), in first-seen order
    std::vector<PcgActorSpawn> actors;
    std::vector<PcgDiag> diags;
    std::map<std::string, u32> nodePointCount;   // node id -> points on its first output pin (editor inspect)
    u32 nodesRun = 0, nodesCached = 0;
    PcgPoints output;            // points reaching an `Output` node (subgraph evaluation)

    usize instanceCount() const;
};

// ---- node registry ------------------------------------------------------------------------------------------------------

class PcgParams {
public:
    PcgParams() = default;
    // key=value tokens (OcGraphNode::extraTokens); tokens without '=' are ignored.
    explicit PcgParams(const std::vector<std::string>& tokens);
    bool has(std::string_view k) const;
    std::string str(std::string_view k, std::string_view def = {}) const;
    f64 num(std::string_view k, f64 def) const;
    i32 integer(std::string_view k, i32 def) const;
    bool flag(std::string_view k, bool def) const;
    Vec3 vec3(std::string_view k, const Vec3& def) const;     // "x,y,z"
    std::vector<Vec3> polyline(std::string_view k) const;     // "x,y,z|x,y,z"
    const std::vector<std::pair<std::string, std::string>>& all() const { return kv_; }
private:
    const std::string* find(std::string_view k) const;
    std::vector<std::pair<std::string, std::string>> kv_;
};

struct PcgNodeCtx {
    const PcgParams& p;
    i32 seed;                  // per-node: hash of the graph seed and the node id
    const PcgContext& ctx;
    std::string id;
    std::vector<PcgDiag>& diags;
    void warn(std::string m) const { diags.push_back({id, std::move(m), false}); }
    void error(std::string m) const { diags.push_back({id, std::move(m), true}); }
};

using PcgNodeFn = void (*)(const PcgNodeCtx&, const std::vector<const PcgPoints*>& in, PcgNodeOut& out);

struct PcgNodeDef {
    const char* type;
    std::vector<const char*> inputs;     // all pins carry `points`
    std::vector<const char*> outputs;
    PcgNodeFn fn;                        // null for the evaluator's own nodes (Input, Output, Subgraph, Loop)
    bool sink;                           // a root of evaluation (spawners)
    // How far outside the evaluation region the node's result for a point can depend on other points, in cm.
    f32 (*margin)(const PcgParams&);
    const char* doc;
};

const std::vector<PcgNodeDef>& pcgNodeDefs();
const PcgNodeDef* pcgFindNode(std::string_view type);

// An OcGraphNode of `type` with its standard pins declared and `params` as key=value tokens. For tests and the editor.
fmt::OcGraphNode pcgMakeNode(std::string_view id, std::string_view type, const std::vector<std::string>& params = {});
// `DOMAIN pcg` is the profile marker.
inline constexpr const char* kPcgDomain = "pcg";
bool pcgIsGraph(const fmt::OcGraphData& g);

// Checks node types, pin names, links, cycles and sink presence without evaluating. Empty = valid.
std::vector<PcgDiag> pcgValidate(const fmt::OcGraphData& g);

// How far from a cell the evaluator must look so a point near the cell edge sees every point that can affect it.
f32 pcgGraphMargin(const fmt::OcGraphData& g, const PcgContext* ctx = nullptr);

// Content hash of a graph's nodes, params and links (not positions, names or comments): the "graph changed" signal.
u64 pcgGraphHash(const fmt::OcGraphData& g);

PcgResult pcgEvaluate(const fmt::OcGraphData& g, const PcgContext& ctx);

// ---- reference port of Aver.Pcg.Pcg.scatter -----------------------------------------------------------------------------
struct PcgScatterSpec {
    PcgBox region;
    i32 count = 0;
    i32 seed = 0;
    i32 assetCount = 0;      // >0 fills the int attribute "asset" with the F# choice
    bool randomYaw = true;
    f32 scaleMin = 1.0f, scaleMax = 1.0f;
    f32 minSpacing = 0.0f;
};
PcgPoints pcgScatterReference(const PcgScatterSpec& s);

} // namespace aver::world
