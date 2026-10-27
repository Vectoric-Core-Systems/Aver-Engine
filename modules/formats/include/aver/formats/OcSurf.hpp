#pragma once
// .ocsurf: a project's physical-surface table (Content/Physics/Surfaces.ocsurf), loaded before any level builds a shape.
// Text, so it can be edited and diffed. Surfaces are stored by name; the 32-bit id comes from aver::surfaceIdFromName,
// which Aver.Framework.Surface.IdOf must match.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kOcSurfMaxSurfaces = 4096;

// An unset friction or restitution defers to the body: the surface still names something for queries and effects
// without changing how anything slides.
struct OcSurfRecord {
    std::string name;
    f32 friction = -1.0f;      // < 0: unset
    f32 restitution = -1.0f;   // < 0: unset
    i32 tag = 0;               // footstep group or effect id chosen by the game
};

struct OcSurfData {
    std::vector<OcSurfRecord> surfaces;   // file order
};

// [A-Za-z0-9_]{1,63}, case-sensitive.
bool ocSurfNameValid(std::string_view name);

// Tolerates CRLF, '#' comments and blank lines. On failure `out` is left empty.
// A header-only file is legal; a file without the OCSURF header is not.
bool parseOcSurf(std::string_view text, OcSurfData& out, std::string* err = nullptr);

// Canonical text; numbers are the shortest that read back as the same f32.
std::string writeOcSurf(const OcSurfData& d);

bool loadOcSurf(const std::string& path, OcSurfData& out, std::string* err = nullptr);
// Creates the folder; a crash mid-save keeps the old table.
bool saveOcSurf(const std::string& path, const OcSurfData& in, std::string* err = nullptr);

} // namespace aver::fmt
