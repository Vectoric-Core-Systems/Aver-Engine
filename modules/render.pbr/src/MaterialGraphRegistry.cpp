// The process's material graphs. See MaterialGraphRegistry.hpp for why there is only one.
#include "aver/pbr/MaterialGraphRegistry.hpp"

#include "aver/pbr/MaterialGraphHlsl.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/core/Log.hpp"

namespace aver::pbr {

u32 MaterialGraphRegistry::idOf(const std::string& key) const {
    for (const Entry& e : entries_)
        if (e.key == key) return e.id;
    return 0;
}

u32 MaterialGraphRegistry::add(const std::string& key, const std::string& displayName,
                               const fmt::OcGraphData& g) {
    const MaterialGraphBody body = compileMaterialGraph(g);
    if (!body.ok) {
        // NAMED BY FILE AND BY NODE. compileMaterialGraph's message already carries the node id; the
        // path is the half it cannot know, and without it a project with a dozen graphs leaves the
        // reader guessing which one is broken -- the same gap [Graph] closes for gameplay graphs.
        AVER_ERROR("[MaterialGraph] '{}' will not compile, so it shades as a stock material: {}",
                   key, body.error);
        return 0;
    }

    // G6 SH2: the converted shaders share one VS output, so their varyings share a budget. A graph that would exceed it does not compile (and shades stock).
    if (body.isGodot) {
        u32 rows = 0;
        for (const Entry& o : entries_)
            if (o.isGodot && o.key != key) for (const fmt::GodotShaderVarying& v : o.gd.varyings) rows += v.rows;
        for (const fmt::GodotShaderVarying& v : body.gd.varyings) rows += v.rows;
        if (rows > kGdVaryingMaxRows) {
            AVER_ERROR("[MaterialGraph] '{}' will not compile, so it shades as a stock material: its varyings bring the converted shaders to {} vertex-output rows (the "
                       "limit is {})", key, rows, kGdVaryingMaxRows);
            return 0;
        }
    }

    for (Entry& e : entries_) {
        if (e.key != key) continue;
        // SAME ID, NEW BODY. The id is already inside constant blocks that may be uploaded, so a
        // reload changes what the graph DOES without changing what names it.
        const bool sameGd = e.isGodot == body.isGodot && (!body.isGodot || (e.gd.ns == body.gd.ns && e.gd.fx == body.gd.fx && e.gd.regsU == body.gd.regsU &&
                            e.gd.regsI == body.gd.regsI && e.gd.regsG == body.gd.regsG && e.gd.fragmentSource == body.gd.fragmentSource &&
                            e.gd.vertexSource == body.gd.vertexSource && e.gd.hasFragment == body.gd.hasFragment && e.gd.hasVertex == body.gd.hasVertex &&
                            e.gd.varyings.size() == body.gd.varyings.size()));
        if (e.body == body.hlsl && e.name == displayName && sameGd) return e.id;   // nothing changed at all
        e.body = body.hlsl;
        e.name = displayName;
        e.isGodot = body.isGodot;
        e.gd = body.gd;
        rebuild();
        AVER_INFO("[MaterialGraph] '{}' recompiled (id {})", key, e.id);
        return e.id;
    }

    Entry e;
    e.key = key;
    e.name = displayName.empty() ? key : displayName;
    e.body = body.hlsl;
    e.isGodot = body.isGodot;
    e.gd = body.gd;
    e.id = nextId_++;
    entries_.push_back(std::move(e));
    rebuild();
    AVER_INFO("[MaterialGraph] '{}' compiled as id {} ({} graph(s) now)", key, entries_.back().id,
              entries_.size());
    return entries_.back().id;
}

void MaterialGraphRegistry::clear() {
    if (entries_.empty() && hlsl_.empty()) return;
    entries_.clear();
    hlsl_.clear();
    vsOutMembers_.clear();
    godot_ = false;
    // nextId_ is NOT reset. An id that has been inside a constant block must never be handed to a
    // different graph later in the same process, and a cleared registry cannot know which blocks
    // still hold one.
    ++revision_;
}

void MaterialGraphRegistry::rebuild() {
    std::vector<MaterialGraphEntry> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(MaterialGraphEntry{e.id, e.name, e.body, e.isGodot, e.gd});
    godot_ = anyGodotShader(out);
    vsOutMembers_ = godotVsOutMembersDefine(out);
    std::string next = materialGraphHlsl(out);
    if (next == hlsl_) return;
    hlsl_ = std::move(next);
    ++revision_;
}

MaterialGraphRegistry& materialGraphs() {
    static MaterialGraphRegistry r;
    return r;
}

} // namespace aver::pbr
