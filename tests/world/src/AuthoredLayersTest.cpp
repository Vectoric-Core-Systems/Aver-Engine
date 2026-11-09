// Data layers through the chunk cook: membership rides in the LAYR payload extra (round trip, old payloads without it), a layer change
// makes the cooked set stale, and splitPayloadByLayers divides a cooked chunk by layer state without losing an entity.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/ChunkCook.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/DataLayers.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static fmt::OcWorldPlacement place(f64 x, f64 y, u64 id, std::vector<std::string> layers = {}) {
    fmt::OcWorldPlacement p;
    p.asset = "Meshes/Box.ocmesh";
    p.x = x; p.y = y; p.z = 0;
    p.placementId = id;
    p.layers = std::move(layers);
    return p;
}

static CookInput inputFor() {
    CookInput in;
    in.levelAsset = "Maps/L.ocworld";
    in.levelStem = "L";
    in.meshBounds = [](const std::string&, Vec3& lo, Vec3& hi) { lo = Vec3{-50, -50, -50}; hi = Vec3{50, 50, 50}; return true; };
    return in;
}

int main() {
    AVER_INFO("=== AuthoredLayersTest ===");
    {   // the extra itself
        PayloadEntity e;
        PlacementExtras x;
        x.layers = {"Day", "Boss Room, east"};
        std::string why;
        check(setPlacementExtras(e, x, &why), "layers are written: " + why);
        PlacementExtras back;
        check(getPlacementExtras(e, back, &why) && back.layers == x.layers, "and read back, names with spaces and commas intact");
        PayloadEntity plain;
        PlacementExtras none;
        setPlacementExtras(plain, none);
        check(findExtra(plain, kExtraLayers) == nullptr, "no layers = no LAYR extra, so an unlayered level's payload bytes are unchanged");
        PayloadEntity bad;
        bad.extras.push_back(PayloadExtra{kExtraLayers, {5, 0, 1}});
        check(!getPlacementExtras(bad, back, &why), "a truncated LAYR is refused with a reason");
        PayloadEntity newer = e;
        newer.extras.push_back(PayloadExtra{extraTag('Z', 'Z', 'Z', 'Z'), {1}});
        setPlacementExtras(newer, x);
        check(findExtra(newer, extraTag('Z', 'Z', 'Z', 'Z')) != nullptr, "an unknown tag still survives a set");
    }

    fmt::OcWorldData level;
    level.name = "L";
    level.dataLayers = {{"Day", fmt::OcDataLayerState::Activated, false, ""}, {"Night", fmt::OcDataLayerState::Unloaded, true, ""},
                        {"Boss", fmt::OcDataLayerState::Loaded, true, "Night"}};
    level.placements = {place(100, 100, 1), place(300, 100, 2, {"Day"}), place(500, 100, 3, {"Night"}), place(700, 100, 4, {"Boss"}),
                        place(900, 100, 5, {"Day", "Night"})};
    {   // child follows its parent
        fmt::OcWorldPlacement child = place(0, 0, 6);
        child.parent = 2;   // child of the Night placement
        level.placements.push_back(child);
    }

    const CookInput in = inputFor();
    const CookOptions opt;
    CookPlan plan;
    CookReport rep;
    std::string why;
    check(planAuthoredCook(level, in, opt, plan, rep, &why), "the level plans: " + why);
    std::vector<ChunkPayload> all;
    usize entities = 0;
    for (usize i = 0; i < plan.chunks.size(); ++i) { all.push_back(plan.payload(i)); entities += all.back().entities.size(); }
    check(entities == level.placements.size() && rep.levelGlobal == 0, "every placement is chunk-owned");
    check(plan.slim.dataLayers == level.dataLayers, "the slim level keeps the DATALAYER records");

    usize withLayers = 0;
    for (const ChunkPayload& p : all)
        for (const PayloadEntity& e : p.entities) {
            PlacementExtras x;
            getPlacementExtras(e, x);
            if (!x.layers.empty()) ++withLayers;
        }
    check(withLayers == 4, "the four layered placements carry their names (the child and the unlayered one carry none)");

    DataLayerSet set;
    set.build(level.dataLayers);   // Day activated, Night unloaded, Boss loaded-but-under-unloaded-Night = unloaded
    usize active = 0, hidden = 0, held = 0;
    for (const ChunkPayload& p : all) {
        const PayloadLayerSplit sp = splitPayloadByLayers(p, set);
        check(sp.active.size() + sp.hidden.size() + sp.held.size() == p.entities.size(), "a chunk's split loses no entity");
        active += sp.active.size(); hidden += sp.hidden.size(); held += sp.held.size();
    }
    check(active == 2 && hidden == 0 && held == 4, "Night unloaded: the Night, Boss, Day+Night placements and the child of a Night placement are held");

    set.setState("Night", LayerState::Loaded, true);
    set.setState("Boss", LayerState::Activated, true);
    active = hidden = held = 0;
    for (const ChunkPayload& p : all) {
        const PayloadLayerSplit sp = splitPayloadByLayers(p, set);
        active += sp.active.size(); hidden += sp.hidden.size(); held += sp.held.size();
    }
    check(held == 0 && hidden == 4 && active == 2, "Night loaded, Boss activated but clamped by its parent: the Night-side placements are resident and hidden");
    set.setState("Night", LayerState::Activated, true);
    active = hidden = held = 0;
    for (const ChunkPayload& p : all) {
        const PayloadLayerSplit sp = splitPayloadByLayers(p, set);
        active += sp.active.size(); hidden += sp.hidden.size(); held += sp.held.size();
    }
    check(held == 0 && hidden == 0 && active == 6, "everything activated: all six are active");

    // staleness
    const u64 h0 = cookSourceHash(level, in, opt);
    fmt::OcWorldData moved = level;
    moved.placements[1].layers = {"Night"};
    check(cookSourceHash(moved, in, opt) != h0, "changing a placement's layers makes the cooked set stale");
    fmt::OcWorldData redefined = level;
    redefined.dataLayers[1].initial = fmt::OcDataLayerState::Loaded;
    check(cookSourceHash(redefined, in, opt) != h0, "so does changing a layer's definition");

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
