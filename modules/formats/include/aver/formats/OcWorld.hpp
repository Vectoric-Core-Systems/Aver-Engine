#pragma once
// .ocworld — the native world format, and a strict superset of .ocmap (FORMAT_SPECS.md §11).
// Identity, SUN/FOG environment, PLACE/PLACEG placements, the LANDSCAPE sections a level's terrain is
// built from, the SCATTER palette a PCGVOLUME's density field is populated with, and the WATER
// surfaces (with their WAVE sets) a level authors instead of taking the host's CLI-supplied ocean.
// Unknown records parse and are skipped.
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

    // Which spawnable CLASS this placement instantiates, by NAME -- resolved through
    // aver_fw_class_find/aver_fw_spawn at load time. Mirrors OcWorldData::gameMode's own "by name,
    // empty means no override" contract (see that field's comment, same file) for the identical
    // reason: framework classes are declared at runtime (aver_fw_class_declare) and their handles are
    // process-local, so only a NAME survives a save/load round trip.
    //
    // EMPTY MEANS AN ORDINARY MESH PLACEMENT, exactly as every placement has always been -- a level
    // authored before this field existed has className empty on every placement and instantiates
    // byte-for-byte identically. A NON-EMPTY className is a CLASS INSTANCE instead:
    // aver::world::instantiate skips building the raw mesh/physics entity for it (see
    // LevelInstance.cpp), and the host's own post-instantiate pass spawns the named class at this
    // placement's transform instead (see GameLevel::load). `asset`/`material` are simply unused for a
    // class placement -- the class's own defaults (and whatever its graph's OnStart/OnTick/SetMesh do)
    // decide how it looks, not this record.
    std::string className;

    // WHICH PLACEMENT THIS ONE HANGS FROM: an index into OcWorldData::placements, or -1 for a root.
    //
    // NESTING IS A FILE-LEVEL SHAPE ONLY. In memory the placements stay a flat vector with this
    // index, so LevelInstance, GameLevel, frameCameraOn, LevelInspect and ScatterBudgetTool need no
    // change at all; parseOcworld and writeOcworld are the only two functions that know about the
    // tree. That is deliberate: a std::vector of children would have rewritten every consumer to buy
    // nothing, since all of them walk placements linearly.
    //
    // THE INDEX IS NOT STABLE ACROSS A SAVE, and cannot be. writeOcworld emits depth-first from the
    // roots, so a flat array whose children do not already follow their parents comes back
    // renumbered. Nothing may store one of these across a round trip -- parents are re-derived from
    // the file's BEGIN/END nesting, never from a number written down. This tree has a scar from
    // exactly that mistake (LevelClassSave.hpp: "Pairing by position would write one placement's
    // transform onto another's line"), which is the reason the file format nests instead of carrying
    // a parent index of its own.
    //
    // x/y/z, yaw/pitch/roll and the scale are PARENT-RELATIVE when parent >= 0, and world when it is
    // -1. That is the same convention scene::CLocal already uses, which is what lets the editor's
    // save write CLocal straight out.
    i32 parent = -1;

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

    // How far this field streams, as a Chebyshev radius in CHUNKS. ZERO MEANS UNSET -- the runtime
    // keeps world::StreamSettings' own default (3).
    //
    // THIS IS WHAT LETS A LEVEL HAVE A FOREGROUND AND A BACKGROUND. One field for the whole world
    // forces one radius for everything, and one radius cannot be right for two things at once: at
    // 3 chunks the world is populated for 48 m while the camera sees kilometres, so the scatter
    // stops at a visible ring and bare terrain runs to the horizon. Raising the single radius is not
    // the answer either -- the palette is dominated by ground cover, and radius 5 multiplies ALL of
    // it, triangles that are invisible at that distance included.
    //
    // Several volumes, each with its own radius and its own species, is the answer: a canopy field
    // at radius 10 sampled coarsely, ground cover at radius 3 sampled finely. Cost scales with what
    // is actually visible at each distance rather than with the largest radius any species needs.
    //
    // COST IS QUADRATIC IN THIS, same as samplesPerAxis: radius 6 is four times the chunks of
    // radius 3. Pair a large radius with a small samplesPerAxis and a short palette, never with the
    // ground-cover tier.
    i32 radiusChunks = 0;

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

    // Which PCGVOLUME's density field places this species, by name. EMPTY MEANS THE FIRST NON-"Sky"
    // VOLUME, which is exactly what the runtime did for every species before this field existed, so
    // a level that never mentions it behaves identically.
    //
    // BY NAME, not by index, for the reason OcPcgVolume itself is named: a level may declare several
    // and "the first one" is how the wrong field gets sampled with nothing saying so. The same
    // argument the sky path already settled -- aver::game::GameLevel::pcgField(name) has stored
    // every declared volume by name from the start; only the scatter side was stuck on first-match.
    std::string volume;

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

// One authored Gerstner wave, contributing to a WATER record's swell. Mirrors
// aver::fluids::GerstnerWave (modules/water/include/aver/fluids/GerstnerWave.hpp) field for field --
// direction, wavelength, amplitude, steepness -- but keeps f64 here rather than that struct's f32,
// the same "narrowing happens once, at the format -> runtime conversion, not here" rule
// OcScatterSpecies states above. Aver.Formats cannot include GerstnerWave.hpp regardless: it depends
// on Core/Platform/Assets only (modules/formats/CMakeLists.txt's DEPS), and Aver.Fluids is not among
// them, so this struct is a deliberate duplicate rather than a missed reuse.
//
// A SEPARATE RECORD, cross-referenced to its WATER by NAME, rather than embedded inline on the
// WATER line -- exactly the shape OcScatterSpecies already takes against OcPcgVolume (`volume
// <name>`, this struct's own `water` field below). Nothing in this file repeats one keyword
// several times on a single line to build a list, and a four-or-more-wave swell would make such a
// line unreadable; one WAVE per line keeps each wave on its own, editable line instead.
struct OcGerstnerWave {
    // Which WATER this wave belongs to, by name. EMPTY MEANS THE FIRST DECLARED WATER RECORD --
    // mirroring OcScatterSpecies::volume's own "empty means the first non-Sky volume" contract,
    // simplified because water has no equivalent of a reserved name to exclude. The overwhelmingly
    // common case is one WATER per level, and an author of that one water should not have to name
    // it just to give it waves.
    std::string water;

    f64 dirX = 1.0, dirZ = 0.0;    // need not be pre-normalised, matching GerstnerWave.hpp's own contract
    f64 wavelengthCm = 800.0;
    f64 amplitudeCm  = 25.0;
    f64 steepness    = 0.6;
};

// One WATER surface a level authors. Replaces what sandbox/src/SandboxApp.cpp hardcoded at render
// registration -- a --water CLI flag for the height, and four fixed-heading Gerstner waves for the
// swell -- with level data, exactly what modules/water/README.md's "What this slice does not do"
// section names as the follow-up owed to the LANDSCAPE record: "Making them level data is a separate
// slice and follows the LANDSCAPE record precedent exactly."
//
// NAMED, for the same reason OcPcgVolume and OcLandscapePlacement are: nothing stops a level
// declaring an ocean AND a pool, and "the water" stops meaning anything once there are two.
//
// MODELLED ON OcPcgVolume, not OcLandscapePlacement, despite the README's wording: a WATER record is
// self-contained authored data, with no external asset it merely positions the way LANDSCAPE
// positions an .ocland file, so the infinite/bounded duality below is lifted from OcPcgVolume rather
// than from LANDSCAPE's single `extent` hint. INFINITE IS THE DEFAULT for the identical reason it is
// there: an endless surface is what every level had before this record existed (the host's
// CLI-supplied water has no edges), so a WATER record that never mentions bounds must still read
// back as that same endless plane; `bounds` narrows it to a POOL.
//
// BOUNDS ARE HORIZONTAL ONLY (X, Y), unlike OcPcgVolume's three-axis box: a water surface already
// has one height, `levelCm`, and no vertical thickness to bound -- a Z pair here would be a number
// nothing ever reads, which is worse than not having one.
struct OcWaterPlacement {
    std::string name;              // level-local identifier; "unnamed" when empty, mirroring OcPcgVolume
    f64 levelCm = 0.0;             // surface height, cm, +Z up -- what --water <heightCm> used to set

    bool infinite = true;          // false means boundsMin/Max are meaningful
    f64 boundsMin[2] = {0, 0};     // X, Y cm
    f64 boundsMax[2] = {0, 0};     // X, Y cm

    // SIMULATED rather than analytic: this surface is a soft body the physics solver sloshes, not a
    // Gerstner sum evaluated in a vertex shader. Off by default because every water record written
    // before this one meant the analytic surface, and because a simulated volume costs a solver body
    // where the analytic one costs nothing.
    //
    // ONLY MEANINGFUL WITH bounds. A simulated volume is a closed shell with a size; an endless
    // ocean has no size to give it, and the consumer is what refuses that pairing rather than the
    // parser -- the format's job is to carry what was written, not to adjudicate it.
    bool simulate = false;

    // The four solver knobs a simulated record may name -- see fluids::FluidVolumeDesc's own fields
    // of the same names for what each one means physically. These are what actually reaches Jolt.
    //
    // ALL FOUR DEFAULT TO -1, meaning "not authored", the same sentinel fluids::kFluidPressureAuto
    // already gives `pressure` for the identical reason: 0 is a real, legal value for compliance,
    // damping and pressure (0 compliance is "inextensible", 0 damping and 0 pressure are ordinary
    // Jolt settings), and 0 iterations divides by zero inside Jolt's own solver step -- so none of
    // the four can use 0 as "the author wrote nothing" without colliding with a value someone might
    // actually write. -1 has no such collision: nothing here is ever legally negative.
    //
    // The consumer (SandboxApp::applyLevelWater), not this struct, decides what an unauthored field
    // becomes -- fluids::FluidVolumeDesc's own defaults, exactly what a level written before these
    // four tokens existed already got. This struct's only job is to carry what was written.
    f64 compliance = -1.0;
    f64 damping    = -1.0;
    i32 iterations = -1;
    f64 pressure   = -1.0;

    // THE MATERIAL LAYER, alongside the four raw knobs above -- see fluids::FluidPhysicsMaterial's own
    // comment (FluidVolume.hpp) for what density/viscosity mean physically (density real,
    // viscosity a CALIBRATED FIT onto `damping` above, not a fifth thing that reaches Jolt
    // directly -- correcting what this struct's own comment used to say, back when there was no
    // such fit yet). `preset` names one of FluidPhysicsMaterial's own presets (water/lightoil/honey/lava,
    // case-insensitive) and wins over density/viscosity when non-empty; density/viscosity alone
    // build a hand-typed material. EMPTY PRESET AND BOTH density/viscosity <= 0 MEANS NO MATERIAL
    // -- the consumer, not this struct, decides what that becomes, the identical division of
    // responsibility the four -1 knobs above already have. A record naming BOTH a material (any of
    // the three fields below) and a non-default `damping` above is carried through exactly as
    // written; the consumer is what refuses the conflict (fluids::fluidResolvePhysicsMaterial, called from
    // fluids::FluidScene::spawn), not this struct and not the parser.
    std::string preset;
    f64 density   = -1.0;
    f64 viscosity = -1.0;

    // THE SURFACE MATERIAL -- a THIRD thing this record calls "material", and the only one that
    // decides how the water LOOKS. Names an .ocmat under Content/Materials, exactly as a PLACEG
    // line's trailing material token does, and resolves through the same
    // aver_scene_material -> surfaceMaterials_ path every other surface in a level uses.
    //
    // DO NOT CONFUSE IT WITH `preset` ABOVE, and the collision is why this comment is long.
    // `preset`/`density`/`viscosity` are the SOLVER's material: two floats that become particle mass
    // and Jolt damping (fluids::FluidPhysicsMaterial, renamed from FluidPhysicsMaterial precisely so these
    // two ideas stop sharing a word). They carry no colour, no roughness, no IOR, no texture, and
    // naming the preset "honey" has never made anything look like honey -- the appearance was a pair
    // of compile-time C++ literals in the fluid shader that no level could reach.
    //
    // EMPTY MEANS NO AUTHORED MATERIAL, and the consumer decides what that becomes -- the identical
    // division of responsibility every other optional field here already has. A water surface with
    // no material draws with the engine's fallback look rather than refusing to draw.
    std::string material;
};

// A LEVEL'S WEATHER, split out of OcWorldData so it can be COPIED WHOLE.
//
// It is a base class rather than a member for one reason: every existing reader says `w.sunLux`,
// and a member would have made this a rename across 81 sites for no gain. Inheriting keeps all of
// them working while letting `env = w` slice off exactly these fields -- which is what runtime.game
// does, and what removes a whole class of bug. GameLevel used to mirror twelve of these into its
// own members one assignment at a time, so a field added to the format and not there was
// authorable in the editor and silently ignored by the game. There is nothing to forget now.
//
// Values are plain numbers rather than an rhi::SkyAtmosphere because this module depends on Core
// alone; the defaults below MIRROR that struct's, so a level that omits a token behaves exactly as
// one written before the token existed.
//
// TWO CONVENTIONS LIVE HERE, on purpose:
//
//   * A NEGATIVE SENTINEL, for the six air parameters and the two that already used it. Every one
//     is a strictly positive physical quantity, so -1 cannot collide with an authored value, and
//     the writer omits an unset one entirely.
//   * TOKEN PRESENCE, for everything else. parseOcworld assigns only the tokens it actually reads,
//     so a token that is absent leaves the default standing -- which means no sentinel is needed
//     and none is safe: fogHeight and the cloud layer bounds are world Z, fogStart is a distance,
//     cloudWind is a direction and cloudsEnabled is a bool. Zero is an ordinary authored value for
//     all of them, and a `>= 0` rule would have made each of them unsettable.
struct OcWorldEnv {
    bool hasSun = false;
    // Points TOWARD the light, matching rhi::SkyAtmosphere::sunDirection.
    f64 sunDir[3] = {-0.5481, 0.3838, 0.7431};
    f64 sunColor[3] = {1.0, 0.98, 0.92};
    f64 sunLux = 100000.0;
    // Kelvin, and 0 means "use sunColor as authored" -- rhi::SkyAtmosphere spells it the same way,
    // so this is that field's convention rather than a sentinel of this format's invention.
    f64 sunTemperatureK = 0.0;
    f64 sunAngularDeg = 0.545;             // the disk's angular diameter, and how fast a shadow softens

    bool hasFog = false;
    f64 fogDensity = 4e-6;                 // per cm
    f64 fogColor[3] = {1.0, 1.0, 1.0};     // a tint on the in-scattered sky; white is clear air
    f64 fogFalloff = 0.0;                  // how fast it thins going up; 0 is uniform distance fog
    f64 fogHeight = 0.0;                   // world Z at which density is exactly fogDensity
    f64 fogStart = 0.0;                    // distance in front of the camera before fog accumulates
    f64 fogMaxOpacity = 1.0;               // so distance never fully erases the world

    // SKY: which sky model, the authored dome, the ground it stands on, and the air above it.
    bool hasSky = false;
    bool skyPhysical = true;               // false selects the authored two-colour dome
    // The authored dome. Read only when the model is Authored, but written either way: a level that
    // switches back to Authored must find the colours it left there.
    f64 skyZenith[3] = {0.24, 0.45, 0.85};
    f64 skyHorizon[3] = {0.72, 0.83, 0.95};
    f64 skyDomeExponent = 0.65;            // exponent on the horizon-to-zenith blend
    // What the world below the horizon reflects back, and how much of it replaces the sky there.
    f64 skyGroundAlbedo[3] = {0.24, 0.23, 0.21};
    f64 skyGroundBlend = 1.0;
    f64 skyLight = 1.0;                    // multiplier on the sky-hemisphere irradiance
    // The air. Negative means "leave the engine's default alone" -- see the note above.
    f64 skyMieScatter = -1.0;              // per km; raise for haze, dust or a coastal day
    f64 skyMieExtinction = -1.0;
    f64 skyMiePhaseG = -1.0;               // forward-scattering anisotropy
    f64 skyRayleighKm = -1.0;              // Rayleigh scale height
    f64 skyMieKm = -1.0;                   // Mie scale height
    f64 skyPlanetKm = -1.0;
    f64 skyAirDepthKm = -1.0;
    f64 skyMultiScatter = -1.0;            // isotropic multiple-scattering gain
    i32 skyViewSteps = 0;                  // samples along a sky ray
    i32 skyAerialSteps = 0;                // samples along the air between camera and surface

    // CLOUDS: its own record, so a level with none writes no line and reads exactly as it did
    // before the record existed. hasClouds says the level SPOKE about clouds; cloudsEnabled says
    // what it said. Collapsing the two would make "clouds off" indistinguishable from "no opinion",
    // and an authored overcast level would come back clear the first time anyone saved it.
    bool hasClouds = false;
    bool cloudsEnabled = false;
    f64 cloudCoverage = 0.45;              // 0 clear, 1 overcast
    f64 cloudDensity = 1.0;
    f64 cloudBottom = 150000.0;            // world Z of the layer's base and top
    f64 cloudTop = 280000.0;
    f64 cloudFeatureSize = 50000.0;        // the width of one noise feature, in world units
    f64 cloudWind[2] = {900.0, 260.0};     // world units per second
};

struct OcWorldData : OcWorldEnv {
    int version = 1;
    u64 contentId = 0;                     // ID = FNV-1a-64(NAME)
    std::string name;

    // The GAMEMODE this level overrides the project's default with, by CLASS NAME. Empty means "no
    // override" -- the project's own default applies, which is what every level did before this
    // existed.
    //
    // BY NAME, not by a handle or an index: framework classes are declared at runtime by the managed
    // side (aver_fw_class_declare) and their handles are process-local, so a number written into a
    // level file would mean something different in the next process. The name is the only stable
    // identity a file can carry, and aver_fw_class_find turns it back into a handle on load.
    //
    // A LEVEL-SCOPED OVERRIDE, deliberately, matching what a World Settings window is for: one
    // project can hold a menu level, a gameplay level and a test level that need different rules,
    // and putting the choice in the project would force them to share one.
    std::string gameMode;
    u32 build = 0;
    u32 algo = 3;


    bool hasSpawn = false;
    f64 spawnX = 0, spawnY = 0, spawnZ = 0, spawnYaw = 0;

    // WHERE THE EDITOR CAMERA WAS WHEN THE LEVEL WAS LAST SAVED, so reopening a level puts you back
    // where you were working instead of at the world origin looking down the X axis.
    //
    // THIS IS NOT THE SPAWN ABOVE, and the two must not be conflated. SPAWN is where the GAME puts
    // the player; this is where the AUTHOR was standing. A level whose player start is at the top of
    // a tower can perfectly well have been last edited in a basement, and overloading one record for
    // both would move the player every time somebody saved from a different vantage point.
    //
    // hasCamera false is the ordinary case for every level written before this record existed, and
    // means "no opinion" -- the editor then frames the level the way it always did rather than
    // jumping to a stored origin, which is the one behaviour that would look like a bug.
    //
    // ANGLES IN DEGREES, positions in centimetres, matching SPAWN directly above. Degrees do not
    // round-trip a direction exactly the way the SUN record's vector does, and here that is fine:
    // this is a viewpoint a person will move again within seconds, not a lighting input whose drift
    // would show up in a render.
    bool hasCamera = false;
    f64 camX = 0, camY = 0, camZ = 0;
    f64 camYaw = 0, camPitch = 0;
    // The fly speed that was in effect, in cm/s -- part of "where I was working" in the same sense
    // as the position. 0 means unstated, so a level that never carried one leaves the preference
    // alone rather than resetting it to a stored zero.
    f64 camSpeed = 0;

    // The level's landscape sections. Order is the file's order, same reasoning as pcgVolumes below --
    // a level naming two the same keeps both rather than silently losing one.
    std::vector<OcLandscapePlacement> landscapes;

    // Declared density fields. Order is the file's order, so a level that declares two with the
    // same name keeps both rather than silently losing one -- the reader reports it instead.
    std::vector<OcPcgVolume> pcgVolumes;

    // The level's scatter palette -- what the density field above places, species by species. Order
    // is the file's order, same reasoning as pcgVolumes above.
    std::vector<OcScatterSpecies> scatterSpecies;

    // The level's authored water surfaces. Order is the file's order, same reasoning as landscapes
    // and pcgVolumes above -- a level naming two the same keeps both rather than silently losing
    // one. EMPTY IS THE ORDINARY CASE: no WATER record means no water, exactly as a level parsed
    // before this record existed.
    std::vector<OcWaterPlacement> waters;

    // The waves each water surface above is given, cross-referenced by name exactly as
    // scatterSpecies is cross-referenced to pcgVolumes above. Order is the file's order.
    std::vector<OcGerstnerWave> waves;

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

// True when `path` uses at least one record kind only the legacy .ocmap grammar has: ROOT, CLIENT,
// SURFACE, GROUND, KILLZ or DEFORM. A caller sees this as "does this file need loadOcmap (OcMap.hpp)
// instead of loadOcworld below" -- the two parsers agree on NAME/ID/BUILD/ALGO/SPAWN/PLACE, so
// nothing is lost by asking only about the six records where they diverge.
//
// THE HEADER LINE ALONE CANNOT ANSWER THIS, AND TESTING AGAINST THIS TREE'S OWN REAL PROJECTS IS
// WHAT PROVES IT. A first version of this function read only the `OCMAP 1` vs `OCWORLD 1` token on
// line one -- which sounds right (parseOcworld's own header check, `equalsCI(key, "OCWORLD") ||
// equalsCI(key, "OCMAP")`, treats the two spellings as interchangeable, so nothing downstream of a
// completed parse can say which one a file declared) until it is run against
// AverProjects/ElectricDreams/Content/Maps/Default.ocmap and AverProjects/FirstPerson/Content/Maps/
// Default.ocmap: both are hand-authored, both open and save correctly today, and both start with
// the literal line `OCMAP 1` -- while their actual content is pure OCWORLD grammar (SUN/SKY/FOG/
// LANDSCAPE/PCGVOLUME/SCATTER for ElectricDreams; SUN/SKY/FOG/PLACEG for FirstPerson), none of
// which the legacy parser has any notion of at all. A header-only version of this function would
// have misrouted both through loadOcmap and silently destroyed exactly the records this whole
// mechanism exists to protect -- trading the demoworld.ocmap bug for a new, broader one on two
// files that were never broken. Only OpenConstructor's demoworld.ocmap -- and any file like it --
// actually uses ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM, so those six are what this asks about,
// wherever in the file they appear, and the header token is not consulted at all.
//
// EXTENSION CANNOT ANSWER IT EITHER, for the identical reason isLevelFile accepts both spellings:
// .ocmap and .ocworld are both used for levels in this tree, and a project can hold either grammar
// under either extension.
//
// Scans every line rather than stopping at the first meaningful one: the six keywords this looks
// for are never on line one of a real file (they follow NAME/ID/BUILD/ALGO), and the cost is the
// same one loadOcworld/loadOcmap already pay to actually parse the file a moment later.
bool levelFileIsLegacyOcmap(const std::string& path);

} // namespace aver::fmt
