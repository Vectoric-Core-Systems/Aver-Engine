#include "aver/world/DataLayers.hpp"

#include "aver/world/ChunkPayloadExtras.hpp"

#include <algorithm>

namespace aver::world {

LayerState toLayerState(fmt::OcDataLayerState s) {
    return s == fmt::OcDataLayerState::Unloaded ? LayerState::Unloaded
         : s == fmt::OcDataLayerState::Loaded   ? LayerState::Loaded : LayerState::Activated;
}

void DataLayerSet::build(const std::vector<fmt::OcDataLayer>& defs) {
    layers_.clear();
    pending_.clear();
    for (const fmt::OcDataLayer& d : defs) {
        if (idOf(d.name) >= 0) continue;   // the first definition of a name wins
        Layer l;
        l.name = d.name;
        l.state = toLayerState(d.initial);
        l.runtime = d.runtime;
        layers_.push_back(std::move(l));
    }
    for (const fmt::OcDataLayer& d : defs)
        if (!d.parent.empty()) {
            const i32 self = idOf(d.name), par = idOf(d.parent);
            if (self >= 0 && par >= 0 && par != self && layers_[static_cast<usize>(self)].parent < 0) layers_[static_cast<usize>(self)].parent = par;
        }
}

i32 DataLayerSet::idOf(std::string_view name) const {
    for (usize i = 0; i < layers_.size(); ++i)
        if (layers_[i].name == name) return static_cast<i32>(i);
    return -1;
}

LayerState DataLayerSet::effective(u32 id) const {
    LayerState out = layers_[id].state;
    i32 p = layers_[id].parent;
    for (usize hops = 0; p >= 0 && hops <= layers_.size(); ++hops) {   // the hop cap ends a parent cycle
        out = std::min(out, layers_[static_cast<usize>(p)].state);
        p = layers_[static_cast<usize>(p)].parent;
    }
    return out;
}

LayerState DataLayerSet::effectiveByName(std::string_view name) const {
    const i32 id = idOf(name);
    return id < 0 ? LayerState::Activated : effective(static_cast<u32>(id));
}

DataLayerSet::Result DataLayerSet::setState(std::string_view name, LayerState st, bool fromGame) {
    const i32 id = idOf(name);
    if (id < 0) return Result::Unknown;
    Layer& l = layers_[static_cast<usize>(id)];
    if (fromGame && !l.runtime) return Result::NotRuntime;
    if (l.state == st) return Result::Ok;
    l.state = st;
    if (std::find(pending_.begin(), pending_.end(), static_cast<u32>(id)) == pending_.end()) pending_.push_back(static_cast<u32>(id));
    return Result::Ok;
}

std::vector<u32> DataLayerSet::idsOf(const std::vector<std::string>& names) const {
    std::vector<u32> out;
    for (const std::string& n : names)
        if (const i32 id = idOf(n); id >= 0 && std::find(out.begin(), out.end(), static_cast<u32>(id)) == out.end()) out.push_back(static_cast<u32>(id));
    return out;
}

void DataLayerSet::apply(PlacementStreamer& st) const {
    for (u32 i = 0; i < layers_.size(); ++i) st.setLayerState(i, effective(i));
}

void DataLayerSet::bind(PlacementStreamer& st, u32 item, const std::vector<std::string>& names) {
    const std::vector<u32> ids = idsOf(names);
    st.setItemLayers(item, ids);
    for (const u32 id : ids) layers_[id].members.push_back(item);
}

std::vector<DataLayerSet::Completed> DataLayerSet::settle(const PlacementStreamer& st, const std::vector<Vec3d>& viewers) {
    std::vector<Completed> done;
    for (usize k = 0; k < pending_.size();) {
        const u32 id = pending_[k];
        const Layer& l = layers_[id];
        const LayerState eff = effective(id);
        bool ok = true;
        for (const u32 item : l.members) {
            if (eff == LayerState::Unloaded) { if (st.resident(item)) { ok = false; break; } }
            else if (!st.resident(item) && st.eligible(item) && st.distance(item, viewers) <= static_cast<f64>(st.settings().loadCm)) { ok = false; break; }
        }
        if (ok) { done.push_back({id, l.state}); pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(k)); }
        else ++k;
    }
    return done;
}

PayloadLayerSplit splitPayloadByLayers(const ChunkPayload& p, const DataLayerSet& layers) {
    PayloadLayerSplit out;
    std::vector<LayerState> fate(p.entities.size(), LayerState::Activated);
    for (usize i = 0; i < p.entities.size(); ++i) {
        const PayloadEntity& e = p.entities[i];
        LayerState st = LayerState::Activated;
        PlacementExtras x;
        if (getPlacementExtras(e, x))
            for (const std::string& n : x.layers) st = std::min(st, layers.effectiveByName(n));
        if (e.parent >= 0 && static_cast<usize>(e.parent) < i) st = std::min(st, fate[static_cast<usize>(e.parent)]);   // parents precede children
        fate[i] = st;
        (st == LayerState::Unloaded ? out.held : st == LayerState::Loaded ? out.hidden : out.active).push_back(static_cast<u32>(i));
    }
    return out;
}

} // namespace aver::world
