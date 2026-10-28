#pragma once
// The line grammar every Godot text file shares (.tscn, .tres, project.godot, .import, .gdextension,
// plugin.cfg): `[tag key=value ...]` headers, each followed by `key = value` lines. Values are
// Variant literals and may span lines; `;` starts a comment. This layer knows no tag names; the
// scene/resource and config readers interpret the sections.
#include "aver/formats/godot/GodotDiagnostics.hpp"
#include "aver/formats/godot/GodotVariant.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot {

struct GodotProperty {
    std::string name;  // may hold '/' and '.' (e.g. "surface_material_override/0", "windows.debug.x86_64")
    GodotVariant value;
    int line = 0;
};

// `[name attr=value ...]`.
struct GodotTag {
    std::string name;
    std::vector<GodotProperty> attrs;
    int line = 0;

    const GodotVariant* attr(std::string_view key) const;
    // The attribute's text when it is a string-like; `def` when absent or another type.
    std::string attrString(std::string_view key, std::string_view def = {}) const;
    i64 attrInt(std::string_view key, i64 def = 0) const;
};

// One tag and the `key = value` lines under it. Lines before the first tag form a section whose tag
// name is empty (project.godot's `config_version=5`).
struct GodotSection {
    GodotTag tag;
    std::vector<GodotProperty> props;

    const GodotVariant* prop(std::string_view key) const;
};

struct GodotSectionFile {
    std::string file;
    std::vector<GodotSection> sections;
};

// Parses every section. A bad value reports file:line and parsing resumes at the next tag, so one
// run lists every error. True when this call added no errors.
bool parseGodotSections(std::string_view text, std::string_view file, GodotSectionFile& out, GodotDiagnostics& diag);

// ---- binary files and loading ---------------------------------------------------------------------

enum class GodotFileKind : u8 {
    Text,            // starts like a text resource/config
    BinaryResource,  // "RSRC" / "RSCC": .scn/.res
    BinaryScript,    // "GDSC": compiled GDScript (.gdc)
    Unknown,
};

GodotFileKind detectGodotFileKind(const u8* data, usize size);

// Reads `path` as text. A binary Godot resource is reported as an error with code
// "godot.binary_resource" ("resave as text") and returns false, as does an unreadable file.
// `displayName` (default: path) names the file in diagnostics, e.g. its res:// path.
bool readGodotTextFile(const std::string& path, std::string& text, GodotDiagnostics& diag,
                       std::string_view displayName = {});

}  // namespace aver::fmt::godot
