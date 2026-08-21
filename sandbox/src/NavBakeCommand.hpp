#pragma once
// The editor's Bake Navigation command, and the overlay that draws the result.
//
// Lives here rather than in modules/synapse because it is the one part of navigation that HAS to
// know about physics, a scene and a device -- and Aver.Synapse's whole value is that it knows about
// none of the three. bakeNav takes two function pointers; this file is what installs the ones that
// call aver_phys_raycast.
//
// WHY THE EDITOR AND NOT THE RUNTIME: terrain collision in this engine is editor-only
// (SandboxApp::rebuildLandscapeCollision has no counterpart in modules/runtime.game), so the one
// process where the landscape body exists is the one process that can see the ground. The bake ships
// its result as an asset; the runtime only ever loads one.
#include "aver/formats/OcNav.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/synapse/NavBake.hpp"

#include <string>
#include <vector>

namespace aver::scene { class World; }

namespace aver::editor {

struct NavBakeSettings {
    f32 cellSizeCm = 50.0f;
    // How far past the outermost entity to bake. An agent standing at the edge of the level still
    // needs cells around it, and a level's entities are its contents, not its floor plan.
    f32 marginCm = 500.0f;
    f32 agentRadiusCm = 34.0f;
    f32 agentHeightCm = 180.0f;
    f32 maxSlopeDeg = 50.0f;
    f32 maxStepCm = 40.0f;
    // Refuses rather than bakes past this many cells, so a level with one placement at the far end
    // of the float range produces a message instead of a hang. 250k cells at 50 cm is a 250 m square.
    u32 maxCells = 250000;
};

// The bake area, derived from where the world's entities actually are.
struct NavExtent {
    f32 originXCm = 0.0f, originYCm = 0.0f;
    u32 widthCells = 0, heightCells = 0;
    f32 topZCm = 0.0f;      // above every entity, so the downward probe starts in clear air
    f32 depthCm = 0.0f;     // far enough below the lowest to reach any floor
    bool valid() const { return widthCells > 0 && heightCells > 0; }
};

// Measures the world. Returns an invalid extent for a world with nothing in it -- an empty level is
// not an error, it just has nothing to bake.
NavExtent measureWorld(scene::World& world, const NavBakeSettings& s);

// Bakes navigation for the world currently loaded, using the live physics scene as the geometry.
// The caller must have physics initialised and the level's bodies built; a bake against an empty
// physics scene silently produces a grid with no floor anywhere, so the result's own stats are the
// thing to look at, not the return value.
bool bakeNavigation(scene::World& world, const NavBakeSettings& s, fmt::OcNavData& out,
                    synapse::BakeStats* stats, std::string* why);

// The path a level's navigation is saved to: the level path with its extension replaced by .ocnav.
// A convention rather than a record in the level file, so a level that has never been baked and a
// level whose bake was deleted look the same -- absent.
std::string navPathForLevel(const std::string& levelPath);

// Builds the overlay line list: one quad per walkable cell at its own floor height, coloured by
// region. PURE -- takes a grid and returns vertices, so what the overlay shows can be asserted
// without a device.
//
// `regionColours` off draws every walkable cell the same. On, two adjacent cells that LOOK connected
// but are in different regions come out different colours, which is the single most useful thing
// this overlay does: an agent refusing to path across an apparently-open floor is invisible until
// the floor is two colours.
std::vector<rhi::LineVertex> buildNavOverlay(const fmt::OcNavData& nav, bool regionColours = true);

} // namespace aver::editor
