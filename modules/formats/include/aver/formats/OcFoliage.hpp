#pragma once
// `.ocfoliage` — one FOLIAGE TYPE: what mesh a foliage brush scatters, and how it randomises each
// placed instance. The AVR1 container, following OcLand.hpp/OcBt.hpp's shape (a required header
// chunk plus an AvrStringTable for the two strings), not OcParticle.hpp's hand-edited OC-dialect
// text: a foliage type has no author-facing grammar to hand-edit or preserve comments in, it is
// authored entirely from the editor tab (FoliageTypeEditor, sandbox/src/FoliageTypeEditor.hpp).
//
// THE SPLIT THIS EXISTS TO MAKE REAL. Before this format, "what to scatter and how" lived as ad hoc
// global fields on SandboxApp (FoliageSpecies plus foliageScaleMin_/Max_/foliageSpacingCm_/
// foliageAlignToNormal_) shared by EVERY palette entry at once -- there was no way to say "this tree,
// at this density, with this scale range" and reuse it. This format is that authored, persisted,
// per-species record: the paint tool (a level mode) stays in SandboxApp, and what it paints becomes
// an ASSET, the same split Unreal draws between its Foliage brush and its Foliage Type.
//
// FIELD SHAPE MIRRORS aver::world::ScatterSpecies (modules/world/include/aver/world/
// ChunkGenerator.hpp) ONE FOR ONE for every concept both this format and the procedural scatter
// system can actually use -- meshPath, material, scaleMin/scaleMax, weight, randomizeYaw and
// collisionRadiusCm are the exact names ScatterSpecies (and its own text-format mirror,
// fmt::OcScatterSpecies in OcWorld.hpp) already use for these ideas. Two names for one concept is
// its own kind of bug, so this format does not invent "spacing" beside "collisionRadiusCm" or
// "density" beside "densityMin/densityMax" -- it reuses the established word.
//
// WHAT IS DELIBERATELY NOT HERE, ON PURPOSE, NOT BY OVERSIGHT:
//   * densityMin/densityMax -- ScatterSpecies' density-BAND eligibility test (which noise value a
//     species is allowed to appear at) has no meaning for the interactive brush this format feeds:
//     a held mouse button has no density field sampled under the cursor to test a candidate against,
//     unlike GeneratedChunkSource::generate()'s per-candidate pcg::sampleInfinite() call. Adding the
//     field with no reader for it would be exactly the "format with no consumer" defect this whole
//     feature exists to avoid one layer up.
//   * a per-species seed -- the brush already advances ONE shared, running RNG stream per paint
//     session (SandboxApp's foliageSeed_), reseeded per placement attempt rather than per type; a
//     fixed seed stored on a TYPE has nothing honest to drive in that model, unlike
//     aver::world::GeneratorSettings::worldSeed, which seeds a pure function of (seed, chunk coord).
// A future consumer that can actually use either (say, this type feeding a SCATTER record) gets to
// add them then, against a real reader -- not speculatively here.
//
// alignToNormal HAS NO ScatterSpecies COUNTERPART (the procedural generator has no slope query at
// all) but mirrors the brush's OWN already-established concept one for one instead -- see
// sandbox/src/FoliageAlign.hpp's foliagePlacementRotation, which is exactly what this field now
// drives per type rather than as one global checkbox for the whole palette.
//
// THE ABSENT-VS-ZERO RULE (FORMAT_SPECS.md §3.2: additive fields grow the struct; a reader reads
// min(known_size, on_disk_size) and DEFAULTS the rest, the same way OcMesh.cpp's MeshletBounds v1/v2
// readers default OwnError/ParentError rather than zeroing them) matters here from day one, even
// though this is a brand new format with no v1 file in the wild yet: randomizeYaw's real default is
// TRUE and weight's is 1.0, neither of which is the bit pattern a blind zero-fill would produce. A
// chunk shorter than this reader expects -- whether a future additive field, or the synthetic
// truncation OcFoliageTest exercises directly -- leaves every field parseOcFoliage did not reach at
// OcFoliageData's own compiled-in default, never at binary zero. See OcFoliage.cpp's parseOcFoliage
// for where that is enforced.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// AVR1 subtype and chunk ids for the format.
inline constexpr u32 kAvrSubtypeFoliage = avrFourCC("FOLI");
inline constexpr u32 kOcFoliageChunkHeader  = avrFourCC("FHDR");
inline constexpr u32 kOcFoliageChunkStrings = avrFourCC("STRT");

// One foliage type: what a foliage brush places, and how it randomises each instance.
struct OcFoliageData {
    // Required in practice; empty fails validation, same rule OcScatterSpecies' own meshPath comment
    // states for the identical reason -- a species with nothing to place is not a species. Defaults
    // to ScatterSpecies' OWN default mesh (ChunkGenerator.hpp), not an arbitrary placeholder, so a
    // freshly-constructed OcFoliageData -- the Content Browser's "New Foliage Type" starter included
    // -- is already valid() rather than needing the author's first edit just to become saveable.
    std::string meshPath = "Meshes/cube.ocmesh";

    // Empty means "use the mesh's own cooked material" -- mirrors ScatterSpecies::material exactly.
    std::string material;

    f32 scaleMin = 0.75f;
    f32 scaleMax = 1.25f;

    // Relative selection weight among the palette entries a brush is currently painting with.
    // <= 0 means this type can never be picked -- mirrors ScatterSpecies::weight exactly.
    f32 weight = 1.0f;

    bool randomizeYaw = true;

    // Minimum-spacing radius in world centimetres: two instances whose radii would overlap reject
    // the later placement. 0 disables the check -- mirrors ScatterSpecies::collisionRadiusCm exactly,
    // including its own "grass and other overlap-tolerant fill has no business paying for this"
    // rationale.
    f32 collisionRadiusCm = 0.0f;

    // Tilts a placed instance to the sampled terrain slope instead of standing upright -- mirrors the
    // brush's own foliageAlignToNormal_ concept (FoliageAlign.hpp), now authored per type rather than
    // as one checkbox for the whole palette.
    bool alignToNormal = false;

    // meshPath is non-empty; scaleMin/scaleMax/weight/collisionRadiusCm are all finite, scaleMin is
    // positive, scaleMax is not below scaleMin, and collisionRadiusCm is not negative.
    bool valid() const;
};

// Reads a foliage type. `why` receives the reason on failure.
bool parseOcFoliage(const u8* bytes, usize size, OcFoliageData& out, std::string* why = nullptr);
bool loadOcFoliage(const std::string& path, OcFoliageData& out, std::string* why = nullptr);

// Writes a foliage type. Refuses (returns false) an OcFoliageData that fails valid() rather than
// writing a file its own loader would go on to reject -- OcBt.cpp's writeOcBt precedent.
bool writeOcFoliage(const OcFoliageData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcFoliage(const std::string& path, const OcFoliageData& in, std::string* why = nullptr);

} // namespace aver::fmt
