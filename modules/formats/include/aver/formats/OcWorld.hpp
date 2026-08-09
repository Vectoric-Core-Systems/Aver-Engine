#pragma once
// .ocworld — the native world format, and a strict superset of .ocmap (FORMAT_SPECS.md §11).
// Identity, SUN/FOG environment, PLACE/PLACEG placements, the LANDSCAPE sections a level's terrain is
// built from, and the SCATTER palette a PCGVOLUME's density field is populated with. Unknown records
// parse and are skipped.
// Engine space: centimetres, +X forward, +Y right, +Z up, left-handed. Positions are f64.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One placed thing. Covers both record forms: PLACE (uniform scale) and PLACEG (non-uniform).
struct OcWorldPlacement {
    std::string asset;                     // asset reference (mesh path or name)
    ObjectId objectId = kInvalidObjectId;  // fnv1a64(asset) unless given explicitly
    std::string material;                  // material name; empty = the asset's own
    f64 x = 0, y = 0, z = 0;               // cm
    f64 yaw = 0, pitch = 0, roll = 0;      // degrees
    f64 sx = 1, sy = 1, sz = 1;            // non-uniform scale (a PLACE sets all three equal)
    bool collide = true;                   // whether the world builds a static body for it

    // `snap`: sit on the ground, with `z` read as an offset ABOVE it rather than an absolute
    // height. A bare token, matching PLACE's own `nocollide` and PCGVOLUME's `infinite` -- it is a
    // statement about what the placement IS, not a value it carries.
    //
    // WHO ANSWERS IT is the host, through InstantiateOptions::groundHeightAt. This module has no
    // idea whether a level has terrain under it, and adding one would drag the landscape into the
    // dependency graph of a text parser.
    bool snapToGround = false;

    bool uniform() const { return sx == sy && sy == sz; }
};

// A whole world: identity, optional environment, and the placements.
// A procedural density field the level declares. Authored, not generated: this is the RECIPE, and
// what fills it is the GPU (a bounded one) or a sampling call (an infinite one).
//
// NAMED, because a level may declare several -- a sky, a cave mask, a moisture field -- and a script
// asking for "the PCG volume" when there are three is how the wrong one gets used silently.
//
// INFINITE IS THE DEFAULT, and that is deliberate rather than lazy. Hash noise has a value at every
// lattice cell in every direction; bounds are an extra restriction, not a prerequisite. An infinite
// field also generates identically per chunk, in any order, which is the property a streamed world
// needs and a bounded one cannot offer.
struct OcPcgVolume {
    std::string name;
    i32 seed = 0;
    // World centimetres spanned by one lattice cell. Defaults to 1600 -- one 16 m chunk per cell,
    // the chunk size docs/CHUNKS.md specifies.
    f64 cellSizeCm = 1600.0;
    i32 octaves = 4;
    f64 coverageFloor = 0.0;
    f64 coverageBias  = 1.0;
    // Candidate placement positions per horizontal chunk axis: the field is sampled on an n x n grid
    // and each sample that clears the coverage floor may become one scattered entity. ZERO MEANS
    // UNSET -- the runtime keeps aver::world::GeneratorSettings' own default rather than being told
    // 0 samples and generating an empty world.
    //
    // THIS IS THE KNOB THAT DECIDES HOW DENSE A WORLD LOOKS, and it had no way into a level at all:
    // the generator's default of 4 puts one candidate every 400 cm on a 1600 cm chunk, so even a
    // palette of twenty species rendered as a handful of objects scattered metres apart. Nothing was
    // wrong with the palette; a forest floor simply needs to be sampled far more finely than a
    // scattering of boulders, and only the level knows which it is.
    //
    // COST IS QUADRATIC. n=16 is 16x the candidates of n=4, and every accepted one is an entity with
    // its own draw. Raise it with the palette's triangle budget in view, not on its own.
    i32 samplesPerAxis = 0;
    // False means boundsMin/Max are meaningful. True means the field is everywhere.
    bool infinite = true;
    f64 boundsMin[3] = {0, 0, 0};
    f64 boundsMax[3] = {0, 0, 0};
};

// One species in a level's scatter palette: what a PCGVOLUME's density field places, and how.
//
// THE PALETTE IS LEVEL DATA, sitting beside the PCGVOLUME it scatters onto, for the reason
// OcWorld.hpp already states for that record: the engine must not know what is in a project. Every
// field here mirrors aver::world::ScatterSpecies (modules/world/include/aver/world/
// ChunkGenerator.hpp) one for one, but keeps its own f64 fields rather than that struct's f32 ones --
// every other numeric field in this file is f64, and the narrowing to f32 happens exactly once, at
// the format -> runtime conversion in Aver.World (aver::world::buildScatterPalette), not here.
struct OcScatterSpecies {
    std::string meshPath;                  // required in practice; empty fails validation, same as a
                                            // mesh path that does not resolve to a real asset
    // EMPTY, not "M_Foliage". A text format has no business naming a material that only one
    // project ever had: empty means "use the mesh's own cooked material", which is what a
    // SCATTER record that states no opinion actually wants. Every record in the demo level
    // names its material explicitly, so nothing depended on the old default.
    std::string material;
    f64 weight = 1.0;
    f64 scaleMin = 0.75, scaleMax = 1.25;
    bool randomizeYaw = true;
    // Unbounded by default -- eligible at every density sampleInfinite can return, matching
    // ScatterSpecies's own default so a level that names one species with no `density` clause scatters
    // it everywhere the field accepts a candidate at all.
    f64 densityMin = -std::numeric_limits<f64>::max();
    f64 densityMax =  std::numeric_limits<f64>::max();
    f64 collisionRadiusCm = 0.0;
};

// One landscape SECTION a level places in world space: heightfield terrain from an .ocland asset
// (modules/formats/include/aver/formats/OcLand.hpp -- "one landscape SECTION: a square heightfield
// grid, in the AVR1 container"). The level carries no heights itself, only where a section sits and,
// optionally, how large the level declares it to be.
//
// NAMED, for the same reason OcPcgVolume is: a level tiling several sections into one landscape names
// each one, so a script or a scout asking for "the landscape" when there are several knows which it
// means.
//
// PLACEMENT IS SEPARATE FROM THE ASSET, exactly as OcWorldPlacement's x/y/z sit apart from the mesh it
// names: an .ocland file already carries its own originCm, but a level author positioning (or
// re-using) the same section at a different spot in the world should not have to re-author the asset
// to do it -- `at` OVERRIDES the section's own origin, the same way PLACE overrides wherever a mesh's
// pivot happens to be.
struct OcLandscapePlacement {
    std::string name;      // level-local identifier; "unnamed" when empty, mirroring OcPcgVolume
    std::string section;   // asset reference to the .ocland file; required in practice
    // Material name, resolved by the host exactly as PLACE's and SCATTER's are -- empty means the
    // renderer's own flat default colour.
    //
    // THE LEVEL IS THE ONLY PLACE THAT CAN SAY THIS. An .ocland file carries heights and nothing
    // else (OcLand.hpp: two chunk ids, LHDR and HGHT), so the same heightfield is a forest floor in
    // one level and a sand dune in another; putting the surface in the asset would make that
    // impossible, and putting it in the engine would make it the same in every project.
    //
    // Its absence is why terrain rendered as one flat olive: LandscapeRenderer::setSurface had a
    // hardcoded {0.42, 0.45, 0.36} and no call site anywhere in the tree, and a level had no token
    // with which to override it.
    std::string material;
    f64 x = 0, y = 0, z = 0;   // world placement (cm) of the section's own origin sample, overriding
                                // whatever the .ocland file's own originCm says

    // Declared footprint in cm. ZERO MEANS UNSET -- the section file's own sampleCount/spacingCm
    // remain the authority; this is a level-authored HINT for a reader that would rather not open the
    // asset just to learn how much space it occupies (an editor bounds preview, a streaming budget),
    // never a second source of truth the runtime has to reconcile against the file.
    f64 extentCm = 0.0;
};

struct OcWorldData {
    int version = 1;
    u64 contentId = 0;                     // ID = FNV-1a-64(NAME)
    std::string name;
    u32 build = 0;
    u32 algo = 3;

    bool hasSun = false;
    // Points TOWARD the light, matching rhi::SkyAtmosphere::sunDirection.
    f64 sunDir[3] = {-0.5481, 0.3838, 0.7431};
    f64 sunColor[3] = {1.0, 0.98, 0.92};
    f64 sunLux = 100000.0;

    bool hasFog = false;
    f64 fogDensity = 4e-6;                 // per cm
    f64 fogColor[3] = {1.0, 1.0, 1.0};     // a tint on the in-scattered sky; white is clear air

    // SKY: which sky model, and the few air parameters worth authoring per level. Kept as plain
    // numbers rather than an rhi::AtmosphereProfile because this module depends on Core alone.
    // A negative or zero override means "leave the engine's default alone".
    bool hasSky = false;
    bool skyPhysical = true;               // false selects the authored two-colour dome
    f64 skyMieScatter = -1.0;              // per km; raise for haze, dust or a coastal day
    f64 skyMultiScatter = -1.0;            // isotropic multiple-scattering gain
    i32 skyViewSteps = 0;                  // samples along a sky ray
    i32 skyAerialSteps = 0;                // samples along the air between camera and surface

    bool hasSpawn = false;
    f64 spawnX = 0, spawnY = 0, spawnZ = 0, spawnYaw = 0;

    // The level's landscape sections. Order is the file's order, same reasoning as pcgVolumes below --
    // a level naming two the same keeps both rather than silently losing one.
    std::vector<OcLandscapePlacement> landscapes;

    // Declared density fields. Order is the file's order, so a level that declares two with the
    // same name keeps both rather than silently losing one -- the reader reports it instead.
    std::vector<OcPcgVolume> pcgVolumes;

    // The level's scatter palette -- what the density field above places, species by species. Order
    // is the file's order, same reasoning as pcgVolumes above.
    std::vector<OcScatterSpecies> scatterSpecies;

    std::vector<OcWorldPlacement> placements;
};

// Parses a world from memory. Unknown records are skipped, not failed.
bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err = nullptr);

// Loads a world from disk.
bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err = nullptr);

// Serialises a world to the text form. Round-trips through parseOcworld.
std::string writeOcworld(const OcWorldData& w);

// Writes a world to disk, creating parent directories.
bool saveOcworld(const std::string& path, const OcWorldData& w, std::string* err = nullptr);

} // namespace aver::fmt
