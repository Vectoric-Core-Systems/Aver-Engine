// Biome -> PCG graph compiler and the generation guardrails. See PcgBiome.hpp.
#include "aver/world/PcgBiome.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace aver::world {
namespace {

std::string num(f64 v) {
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<f32>(v));
    return std::string(buf, r.ptr);
}
std::string key(const char* k, f64 v) { return std::string(k) + "=" + num(v); }
std::string key(const char* k, const std::string& v) { return std::string(k) + "=" + v; }
std::string vec(f64 x, f64 y, f64 z) { return num(x) + "," + num(y) + "," + num(z); }

constexpr f32 kCm2PerArea = 1.0e6f;   // density is per 100 m^2 = 1e6 cm^2

struct Chain {
    fmt::OcGraphData& g;
    std::string tail;       // node id whose `tailPin` feeds the next link
    std::string tailPin = "out";
    void add(const std::string& id, const char* type, std::vector<std::string> params, const char* nextOut = "out") {
        g.nodes.push_back(pcgMakeNode(id, type, std::move(params)));
        if (!tail.empty()) g.links.push_back({tail, tailPin, id, "in"});
        tail = id;
        tailPin = nextOut;
    }
};

f32 effectivePruneRadius(const fmt::OcBiomeEntry& e, const fmt::OcFoliageData& t) {
    return e.pruneRadiusCm >= 0.0f ? e.pruneRadiusCm : t.collisionRadiusCm;
}

} // namespace

bool pcgBuildBiomeGraph(const std::vector<PcgBiomeSource>& sources, fmt::OcGraphData& out, std::string* why) {
    out = fmt::OcGraphData{};
    out.domain = kPcgDomain;
    out.name = "biome";
    const auto fail = [&](const std::string& m) { if (why) *why = m; out = fmt::OcGraphData{}; return false; };

    f32 maxRadius = 0.0f;
    for (const PcgBiomeSource& s : sources) {
        if (!s.biome) return fail("a biome source has no biome");
        std::string v;
        if (!s.biome->valid(&v)) return fail("biome '" + s.biome->name + "': " + v);
        if (s.types.size() != s.biome->entries.size()) return fail("biome '" + s.biome->name + "': types do not match entries");
        for (usize k = 0; k < s.types.size(); ++k) {
            if (!s.types[k].valid()) return fail("biome '" + s.biome->name + "': foliage type " + std::to_string(k) + " is invalid");
            maxRadius = std::max(maxRadius, effectivePruneRadius(s.biome->entries[k], s.types[k]) * s.types[k].scaleMax);
        }
    }

    std::vector<std::string> tails;      // per entry, the tagged point stream
    struct Spawn { std::string id; const fmt::OcFoliageData* type; };
    std::vector<Spawn> spawns;
    u32 typeIndex = 0;
    for (usize bi = 0; bi < sources.size(); ++bi) {
        const fmt::OcBiomeData& b = *sources[bi].biome;
        for (usize ei = 0; ei < b.entries.size(); ++ei, ++typeIndex) {
            const fmt::OcBiomeEntry& e = b.entries[ei];
            const fmt::OcFoliageData& t = sources[bi].types[ei];
            const std::string P = "b" + std::to_string(bi) + "e" + std::to_string(ei) + "_";
            Chain c{out};

            const bool clump = e.clumpRadiusCm > 0.0f;
            const f32 avgChildren = clump ? 0.5f * static_cast<f32>(e.clumpMin + e.clumpMax) : 1.0f;
            // One candidate per cell of area 1/density; a clump entry plants parents sparser by the mean clump size.
            const f32 spacing = std::max(10.0f, std::sqrt(kCm2PerArea / e.densityPer100m2) * std::sqrt(std::max(avgChildren, 1.0f)));
            c.add(P + "grid", "CreatePointsGrid", {key("spacing", spacing), "z=0"});
            const f32 jitter = 0.5f * spacing;
            c.add(P + "jitter", "TransformPoints", {"posMin=" + vec(-jitter, -jitter, 0), "posMax=" + vec(jitter, jitter, 0)});
            if (clump)
                c.add(P + "clump", "Clump", {key("radius", e.clumpRadiusCm), key("min", static_cast<f64>(e.clumpMin)),
                                             key("max", static_cast<f64>(e.clumpMax)),
                                             key("spacing", std::max(0.0f, effectivePruneRadius(e, t)))});
            std::vector<std::string> proj = {"align=" + std::string(t.alignToNormal ? "1" : "0"), key("alignMax", t.alignMaxAngleDeg),
                                             key("zOffset", t.groundOffsetCm), "drop=1"};
            c.add(P + "project", "ProjectToSurface", proj);

            const fmt::OcBiomeMask& m = b.mask;
            if (m.kind != fmt::OcBiomeMask::Kind::None) {
                std::vector<std::string> mp = {key("edge", m.edge), std::string("invert=") + (m.invert ? "1" : "0")};
                switch (m.kind) {
                    case fmt::OcBiomeMask::Kind::Layer:
                        mp.insert(mp.end(), {"source=layer", key("name", m.name), key("min", m.min)});
                        break;
                    case fmt::OcBiomeMask::Kind::Noise:
                        mp.insert(mp.end(), {"source=noise", key("cell", m.noiseScaleCm), key("octaves", static_cast<f64>(m.noiseOctaves)),
                                             key("seed", static_cast<f64>(m.noiseSeed)), key("min", m.min)});
                        break;
                    case fmt::OcBiomeMask::Kind::Spline:
                        mp.insert(mp.end(), {"source=spline", key("name", m.name), key("half", m.halfWidthCm)});
                        break;
                    default:
                        mp.insert(mp.end(), {"source=volume", key("volume", m.name)});
                        break;
                }
                c.add(P + "mask", "EdgeFalloff", mp, "inside");
            }
            if (e.slopeMinDeg > 0.0f || e.slopeMaxDeg < 90.0f)
                c.add(P + "slope", "SlopeFilter", {key("min", e.slopeMinDeg), key("max", e.slopeMaxDeg)}, "inside");
            if (e.heightMinCm > -1.0e8f || e.heightMaxCm < 1.0e8f)
                c.add(P + "height", "HeightFilter", {key("min", e.heightMinCm), key("max", e.heightMaxCm)}, "inside");
            if (!e.layer.empty())
                c.add(P + "layer", "LayerWeightFilter", {key("layer", e.layer), key("min", e.layerMin), "max=1"}, "inside");
            if (e.noiseScaleCm > 0.0f)
                c.add(P + "noise", "NoiseMask", {key("cell", e.noiseScaleCm), key("seed", static_cast<f64>(e.noiseSeed)),
                                                 key("min", e.noiseMin), key("max", e.noiseMax)}, "inside");

            std::vector<std::string> xf = {"scaleMin=" + vec(t.scaleMin, t.scaleMin, t.scaleMin), "scaleMax=" + vec(t.scaleMax, t.scaleMax, t.scaleMax),
                  "uniformScale=1"};
            if (t.randomizeYaw) { xf.push_back("rotMin=0,0,0"); xf.push_back("rotMax=0,0,360"); }
            c.add(P + "xform", "TransformPoints", xf);
            c.add(P + "tagType", "AttributeSet", {"name=type", key("value", static_cast<f64>(typeIndex))});
            c.add(P + "tagRadius", "AttributeSet", {"name=radius", key("value", effectivePruneRadius(e, t))});
            c.add(P + "tagPriority", "AttributeSet",
                  {"name=priority", key("value", static_cast<f64>(b.priority) * 1000.0 + static_cast<f64>(e.priority))});
            tails.push_back(c.tail);
            spawns.push_back({P + "spawn", &t});
        }
    }
    if (tails.empty()) return fail("no entries");

    std::string merged = tails[0];
    for (usize i = 1; i < tails.size(); ++i) {
        const std::string id = "union" + std::to_string(i);
        out.nodes.push_back(pcgMakeNode(id, "Union"));
        out.links.push_back({merged, "out", id, "a"});
        out.links.push_back({tails[i], "out", id, "b"});
        merged = id;
    }
    out.nodes.push_back(pcgMakeNode("compete", "Compete", {key("maxRadius", maxRadius), "mode=greedy"}));
    out.links.push_back({merged, "out", "compete", "in"});

    for (usize i = 0; i < spawns.size(); ++i) {
        const fmt::OcFoliageData& t = *spawns[i].type;
        const std::string fid = "pick" + std::to_string(i);
        out.nodes.push_back(pcgMakeNode(fid, "AttributeFilter", {"attr=type", "op===", key("value", static_cast<f64>(i))}));
        out.links.push_back({"compete", "out", fid, "in"});
        std::vector<std::string> sp = {"meshes=" + t.meshPath + ":1", key("cullStart", t.cullStartCm), key("cullEnd", t.cullEndCm),
                                       key("lodBias", t.lodScaleBias), key("collision", static_cast<f64>(static_cast<int>(t.collision)))};
        if (!t.material.empty()) sp.push_back(key("material", t.material));
        out.nodes.push_back(pcgMakeNode(spawns[i].id, "WeightedMeshSpawner", sp));
        out.links.push_back({fid, "inside", spawns[i].id, "in"});
    }
    return true;
}

PcgBiomeEstimate pcgEstimateBiome(const std::vector<PcgBiomeSource>& sources, f32 cellCm, const PcgGuardrails& caps,
                                  bool (*meshHasLods)(const std::string&, void*), void* user,
                                  const std::vector<std::string>& knownLayers) {
    PcgBiomeEstimate est;
    const f64 areaCm2 = static_cast<f64>(cellCm) * static_cast<f64>(cellCm);
    for (const PcgBiomeSource& s : sources) {
        if (!s.biome || s.types.size() != s.biome->entries.size()) continue;
        for (usize k = 0; k < s.types.size(); ++k) {
            const fmt::OcBiomeEntry& e = s.biome->entries[k];
            const fmt::OcFoliageData& t = s.types[k];
            const std::string who = "'" + e.foliage + "' in biome '" + s.biome->name + "'";
            const f64 n = areaCm2 / kCm2PerArea * static_cast<f64>(e.densityPer100m2);   // children included: density means the total
            est.candidatesPerCell += n;
            if (!(t.cullEndCm > 0.0f))
                est.warnings.push_back(who + " has no cullEndCm: every instance stays resident while its cell is loaded");
            if (meshHasLods && !meshHasLods(t.meshPath, user))
                est.warnings.push_back(who + ": mesh '" + t.meshPath + "' has no LODs");
            if (n > caps.maxInstancesPerCell)
                est.warnings.push_back(who + " alone asks for about " + std::to_string(static_cast<u64>(n)) + " candidates per cell, over the cap of " +
                                       std::to_string(caps.maxInstancesPerCell));
            if (!e.layer.empty() && !knownLayers.empty() && std::find(knownLayers.begin(), knownLayers.end(), e.layer) == knownLayers.end())
                est.warnings.push_back(who + " gates on paint layer '" + e.layer + "', which the landscape does not have");
        }
        const fmt::OcBiomeMask& m = s.biome->mask;
        if (m.kind == fmt::OcBiomeMask::Kind::Layer && !knownLayers.empty() &&
            std::find(knownLayers.begin(), knownLayers.end(), m.name) == knownLayers.end())
            est.warnings.push_back("biome '" + s.biome->name + "' masks on paint layer '" + m.name + "', which the landscape does not have");
    }
    est.instancesPerCellMax = est.candidatesPerCell;
    est.bytesPerCellMax = est.instancesPerCellMax * 96.0;
    est.overCap = est.instancesPerCellMax > caps.maxInstancesPerCell || est.bytesPerCellMax > static_cast<f64>(caps.maxCellBytes);
    if (est.overCap)
        est.warnings.push_back("a " + std::to_string(static_cast<u64>(cellCm)) + " cm cell could hold up to " +
                               std::to_string(static_cast<u64>(est.instancesPerCellMax)) + " instances (about " +
                               std::to_string(static_cast<u64>(est.bytesPerCellMax / (1024.0 * 1024.0))) + " MB), over the guardrail");
    return est;
}

} // namespace aver::world
