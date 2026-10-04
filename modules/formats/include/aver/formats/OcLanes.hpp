#pragma once
// .oclanes -- a level's traffic lanes: directed polylines a driven vehicle follows, and which lanes
// each one flows into. The sidecar of a level, `<level>.oclanes` beside `<level>.ocworld`, same folder
// and same stem; world::VehicleSystem is what reads it.
//
// TEXT, NOT AN AVR1 CONTAINER, for .ocrig's reasons plus one of its own: the writer is the level
// generator's Blender pipeline (a Python script), and a text file is the one thing that writes
// without a binary container library. It is also small -- a couple of thousand lanes -- so the
// decision a binary format would buy (mmap, bulk upload) has nothing to pay for here, and a person
// can open the file and see why a car took the wrong turn.
//
// ---------------------------------------------------------------------------------------------
// OCLANES 1
// # everything after a # is a comment
// LANE 1 1300 350 0 3  0 0 0  1000 0 0  2000 0 0
// LANE 2 2200 350 1 2  2000 0 0  9000 0 0
// NEXT 1 2
// ---------------------------------------------------------------------------------------------
//
//   LANE <id> <speedLimit> <width> <flags> <n> x y z x y z ...     n >= 2 points, engine cm
//   NEXT <id> <successor> <successor> ...                          zero or more successors
//
// A LANE IS DIRECTED: traffic runs from its first point to its last. A successor's first point is
// (near) this lane's last point -- the file does not enforce that, since "near" is the consumer's
// tolerance to choose, but the generator writes it that way and the vehicle AI assumes it when it
// steps from one lane onto the next. Left-hand or right-hand traffic is not a property of the format:
// the generator offsets each lane from its road's centre line, and this only carries the result.
//
// NEXT MAY COME BEFORE OR AFTER THE LANE IT NAMES, and every id it mentions must be declared by some
// LANE in the file. Resolving them is deferred until the whole file is read, because the generator
// writes a lane's successors wherever is convenient to it and a forward reference is the ordinary
// case, not a corner. A NEXT naming a lane nothing declares is refused rather than dropped: a
// vehicle that reaches the end of its lane and finds a successor id no table holds is the kind of
// defect that shows up as one car stopping at one junction, a long way from its cause.
//
// Units are the engine's: centimetres for points and width, cm/s for the speed limit, +X forward,
// +Y right, +Z up, left-handed -- the same space as .ocworld, so a lane point and a placement
// position compare directly.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// OcLane::flags. A bit set is a statement about the road the lane runs on, there for a consumer to
// weigh; none of them changes how the lane is followed.
inline constexpr u32 kOcLaneHighway = 1u << 0;
inline constexpr u32 kOcLaneBridge  = 1u << 1;
inline constexpr u32 kOcLaneRamp    = 1u << 2;

struct OcLane {
    // Unique within a file; any int the generator chose. NEXT refers to lanes by this, never by
    // position, so a lane may be reordered or dropped without rewriting its neighbours.
    i32 id = 0;
    f32 speedLimit = 1300.0f;     // cm/s: 13 m/s, about 47 km/h
    f32 width = 350.0f;           // cm, the lane's drivable width
    u32 flags = 0;                // kOcLane* bits
    std::vector<Vec3> pts;        // engine cm, first = where traffic enters
    std::vector<i32> next;    // ids of the lanes this one flows into; empty = a dead end
};

struct OcLanesData {
    std::vector<OcLane> lanes;
};

// Parses `text`. TOLERANT OF CRLF and of '#' comments (a whole line or the tail of one), and of
// blank lines; strict about everything else. False on a bad record, with `err` set to what and which
// line, and `out` left EMPTY -- never the partial state of a file that failed halfway.
//
// Refused: a first record that is not `OCLANES 1`, an unknown record, a LANE whose point count is
// below 2 or does not match the coordinates that follow, a non-numeric or non-finite number, a
// speed limit or width that is not positive, a repeated lane id, and a NEXT that names an undeclared
// lane, lists an undeclared successor, or repeats for the same lane. A file with the header and no
// lanes is legal (a level with no traffic); a file with no header at all, an empty one included, is not.
bool parseOcLanes(std::string_view text, OcLanesData& out, std::string* err = nullptr);

// Serialises to the text form above: one LANE line per lane in order, each followed by its NEXT line
// when it has successors. Coordinates are written with the shortest text that reads back to the SAME
// f32, so parse(write(d)) is d exactly and a second write is byte-identical to the first.
// Writes what it is given: a lane with fewer than two points goes out as a line parseOcLanes refuses.
std::string writeOcLanes(const OcLanesData& d);

// Reads `path`. False if it cannot be opened or does not parse.
bool loadOcLanes(const std::string& path, OcLanesData& out, std::string* err = nullptr);

} // namespace aver::fmt
