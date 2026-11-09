// Data layers at runtime (docs/LEVEL_STREAMING.md): named layers with a state, a parent chain and a runtime flag, bound to the
// PlacementStreamer. Pure logic -- no scene, GPU or physics. The level's DATALAYER records (OcWorld.hpp) build it.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/PlacementStreamer.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::world {

class DataLayerSet {
public:
    enum class Result : u8 { Ok, Unknown, NotRuntime };

    void build(const std::vector<fmt::OcDataLayer>& defs);
    usize count() const { return layers_.size(); }
    // -1 for a name that is not a layer.
    i32 idOf(std::string_view name) const;
    const std::string& name(u32 id) const { return layers_[id].name; }
    bool runtime(u32 id) const { return layers_[id].runtime; }
    // The state asked for, and the state in force: never above any ancestor's.
    LayerState state(u32 id) const { return layers_[id].state; }
    LayerState effective(u32 id) const;

    // The editor sets any layer (fromGame = false); the running game only the `runtime=1` ones. Setting what it already is queues nothing.
    Result setState(std::string_view name, LayerState st, bool fromGame);
    // Ids for the names a placement lists; names that are not layers are dropped.
    std::vector<u32> idsOf(const std::vector<std::string>& names) const;

    // Hands the streamer every layer's effective state, and the item's membership. Call apply() after any setState.
    void apply(PlacementStreamer& st) const;
    // Records the item as a member (for settled()) and gives the streamer its layers.
    void bind(PlacementStreamer& st, u32 item, const std::vector<std::string>& names);

    // A state change reaches "complete" when the streamer has acted on it: an unloaded layer has no resident member, and a loaded or
    // activated one has every member within load range of a viewer resident. Call once per frame after the streamer's update has
    // been applied; returns the layers that completed since the last call (in the order they were set).
    struct Completed { u32 layer; LayerState state; };
    std::vector<Completed> settle(const PlacementStreamer& st, const std::vector<Vec3d>& viewers);
    usize pending() const { return pending_.size(); }

private:
    struct Layer {
        std::string name;
        LayerState state = LayerState::Activated;
        bool runtime = false;
        i32 parent = -1;
        std::vector<u32> members;
    };
    std::vector<Layer> layers_;
    std::vector<u32> pending_;   // layer ids with an unsettled change
};

LayerState toLayerState(fmt::OcDataLayerState s);

} // namespace aver::world
