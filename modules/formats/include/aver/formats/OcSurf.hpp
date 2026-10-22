#pragma once
// .ocsurf -- a project's physical-surface table: the names of the surfaces its materials, landscape layers and scripts
// refer to, and the friction, restitution and tag each one carries. One file per project, Content/Physics/Surfaces.ocsurf;
// hosts load it when a project opens, before any level builds a shape, and register every record with the physics module.
//
// TEXT, like .oclanes, so the table is a thing a person edits and diffs. A surface is a NAME hashed to a 32-bit id (the id
// aver::surfaceIdFromName and Aver.Framework.Surface.IdOf already agree on), so the file stores the name and nothing else
// that identifies it.
//
// ---------------------------------------------------------------------------------------------
// OCSURF 1
// # everything after a # is a comment
// SURFACE Grass  friction 0.6  restitution 0.05  tag 1
// SURFACE Ice    friction 0.03
// SURFACE Metal  friction 0.35 restitution 0.2   tag 4
// ---------------------------------------------------------------------------------------------
//
//   SURFACE <name> [friction <f>] [restitution <r>] [tag <n>]     one per line
//
// A name is [A-Za-z0-9_]{1,63}, case-sensitive, unique in the file. friction and restitution are unset unless stated (an unset
// surface DEFERS to the body it is on: it names a surface for queries and effects without changing how anything slides); a
// stated value is clamped to 0..10, a negative one means unset. tag is an integer a game uses to pick a footstep group or an
// effect (default 0). At most 4096 surfaces.
//
// FORWARD COMPATIBILITY. An unknown key, and a record other than SURFACE, is skipped with ONE warning per file: a table
// written by a newer engine still loads in an older one. A newer VERSION is refused.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kOcSurfMaxSurfaces = 4096;

struct OcSurfRecord {
    std::string name;
    f32 friction = -1.0f;      // < 0: unset, defer to the body
    f32 restitution = -1.0f;   // < 0: unset
    i32 tag = 0;
};

struct OcSurfData {
    std::vector<OcSurfRecord> surfaces;   // file order
};

// True for a name the format accepts: [A-Za-z0-9_]{1,63}.
bool ocSurfNameValid(std::string_view name);

// Parses `text`. TOLERANT OF CRLF, '#' comments and blank lines; strict about everything else. False on a bad record, with
// `err` set to what and which line, and `out` left EMPTY -- never the partial state of a file that failed halfway.
//
// Refused: a first record that is not `OCSURF <n>`, a version above 1, a SURFACE with no name or an invalid one, a repeated
// name, a key with no value or a non-numeric or non-finite value, and more than kOcSurfMaxSurfaces records. A file with only
// the header is legal (a project that names no surface); one with no header, an empty one included, is not.
bool parseOcSurf(std::string_view text, OcSurfData& out, std::string* err = nullptr);

// Serialises to the canonical form: the header, then one SURFACE line per record in order with only the keys that are set
// (friction and restitution when >= 0, tag when non-zero). Numbers are the shortest text that reads back as the same f32,
// so parse(write(d)) is d and a second write is byte-identical to the first.
std::string writeOcSurf(const OcSurfData& d);

// Reads `path`. False if it cannot be opened or does not parse.
bool loadOcSurf(const std::string& path, OcSurfData& out, std::string* err = nullptr);
// Writes `path` (creating its folder) without the truncate-on-open hazard: a crash mid-save keeps the old table.
bool saveOcSurf(const std::string& path, const OcSurfData& in, std::string* err = nullptr);

} // namespace aver::fmt
