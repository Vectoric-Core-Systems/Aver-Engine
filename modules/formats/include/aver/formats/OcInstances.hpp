#pragma once
// `.ocinst` — one level's baked INSTANCED FOLIAGE: a flat table of world transforms, grouped by the
// prototype asset each range instances. The AVR1 container, following OcLand.hpp/OcFoliage.hpp's
// shape (a header chunk, a string table, and this format's own bulk data chunk) rather than
// OcWorld.hpp's hand-edited text: an instance table is baked by an importer or a paint tool, never
// typed by a person, and at up to millions of rows it could not be a text format anyway.
//
// WHAT THIS IS FOR. A level's instanced foliage lives OUTSIDE the entity/draw system entirely: a
// FOLIAGE record in the level (OcWorldData::foliageFiles, OcWorld.hpp) names one of these files.
// Foliage is static, ray-traced only (one TLAS instance per row, never VoxiRenderer::draws_), has no
// collision, and is not individually selectable -- there is no scene::Entity per blade of grass to
// select. That is the whole reason this format exists rather than the level simply writing one PLACE
// line per instance: a scattered USD stage can declare millions of instances (Jungle Ruins: 8.7
// million), and PLACE was never meant to scale past the low thousands a hand-authored level places.
//
// THE TRANSFORM CONVENTION, uniform everywhere a transform crosses this boundary (this file, the
// runtime loader, VoxiRenderer's own instance buffer): 12 floats are the engine's own ROW-VECTOR
// world matrix (v*M, translation in row 3 -- world[9..11] here, world[12..14] in the flattened Mat4
// a placement's Transform::toMatrix() produces, since that matrix's 4th COLUMN is always (0,0,0,1)
// and is what gets dropped) with that 4th column dropped: t[r*3 + c] = M.m[r][c] for r in 0..3, c in
// 0..2. Row 0..2 are the scaled rotation basis, row 3 the translation, centimetres, engine world
// space -- precisely what game::drawWorld already passes as `&wm.m[0][0]` and what
// voxi::TlasInstance::world already holds, just without the 4 columns of trailing (0,0,0,1) neither
// of those ever reads either. Nothing here invents a second convention for "the same idea, but for
// foliage" -- see tools/AverAssetC.cpp's importer for why THIS is the transform that must come out
// (it calls the identical world::instantiate math a PLACE line's own entity would have used, not a
// second hand-rolled TRS-to-matrix).
//
// WHY A STRING TABLE FOR SOMETHING THIS FLAT. A group's `asset` is the same content-relative path a
// PLACE line's own asset column carries (Meshes/Tree_Pine.ocmesh, say), and a stage with dozens of
// prototypes repeats a handful of distinct paths across every group an importer emits per source
// folder -- interning them once, the way every other AVR1 format's STRT chunk already does, costs
// nothing a fixed-size group record could not otherwise pay for twice.
//
// FAST PATH FOR LARGE FILES, STATED HERE BECAUSE IT SHAPES THE LAYOUT. At kMaxFoliageInstances
// (VoxiRenderer.hpp; 8,000,000) this file's one bulk transform chunk is 384 MB. loadOcInstances reads
// the whole file into ONE buffer with a single ifstream read (OcFoliage.cpp's loadOcFoliage shape, not
// OcLand.cpp's loadOcLand -- see loadOcInstances's own comment for why that second shape's extra
// parse-then-reserialise round trip is not affordable here), and parseOcInstances copies the transform
// chunk into OcInstanceData::transforms with ONE memcpy of the whole span rather than a per-instance
// or per-float loop -- see IXFM's own comment below for why that is the chunk's entire reason to be a
// flat float array rather than an array of per-instance records.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// AVR1 subtype and chunk ids for the format.
inline constexpr u32 kAvrSubtypeInst        = avrFourCC("INST");
inline constexpr u32 kOcInstChunkHeader     = avrFourCC("IHDR");
inline constexpr u32 kOcInstChunkStrings    = avrFourCC("ISTR");
inline constexpr u32 kOcInstChunkGroups     = avrFourCC("IGRP");
// The bulk transform data -- see this header's own top comment for why it is one flat chunk of
// 12 f32 per instance rather than an array of per-instance records with anything else attached.
// GPU-uploadable: this is the array a TLAS-instance build reads straight out of, unmodified.
inline constexpr u32 kOcInstChunkTransforms = avrFourCC("IXFM");

// A reserved bit for a per-group flag word. Cast-shadow is not actually optional today -- foliage
// casts shadows unconditionally, so every group this tree writes carries `flags = kOcInstanceFlagCastShadow`
// -- but the bit is named and reserved now rather than left as a bare `1` a future no-shadow toggle
// would have to go hunting for.
inline constexpr u32 kOcInstanceFlagCastShadow = 1u;

// One contiguous run of `transforms` that all share a prototype asset. `asset` is the SAME string a
// PLACE line's own asset column carries, so a loader derives the identical objectId a mesh placement
// would have (fnv1a64(asset), as OcWorld.cpp already computes it) rather than needing a second identity
// scheme for foliage.
struct OcInstanceGroup {
    std::string asset;
    u32 flags = 0;
    u32 first = 0;
    u32 count = 0;
};

// A whole instance table: every group's range, and the flat transform buffer they index into.
struct OcInstanceData {
    std::vector<OcInstanceGroup> groups;
    // 12 f32 per instance -- see this header's own top comment for the exact row-vector convention.
    std::vector<f32> transforms;

    // True when transforms holds a whole number of 12-float rows, every group names an asset and its
    // [first, first+count) range lies inside that instance count with no overflow, and every float is
    // finite. Checked by both writeOcInstances (which refuses to write data its own reader would go on
    // to reject -- OcFoliage.cpp's writeOcFoliage precedent) and parseOcInstances.
    bool valid() const;
};

// Reads an instance table. `why` receives the reason on failure. Validates counts (the header's
// declared instance/group counts against the chunks that actually carry them), ranges (each group's
// [first, first+count) inside the instance count, checked by subtraction rather than addition so a
// crafted `first` near u32's own maximum cannot wrap the comparison -- Avr1.cpp's own chunk-bounds
// checks use the identical rearrangement, for the identical reason) and that every transform float is
// finite, so a truncated or hand-corrupted file is refused here rather than handed to a TLAS build as
// silent garbage.
bool parseOcInstances(const u8* bytes, usize size, OcInstanceData& out, std::string* why = nullptr);
bool loadOcInstances(const std::string& path, OcInstanceData& out, std::string* why = nullptr);

// Writes an instance table. Refuses (returns false) an OcInstanceData that fails valid().
bool writeOcInstances(const OcInstanceData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcInstances(const std::string& path, const OcInstanceData& in, std::string* why = nullptr);

} // namespace aver::fmt
