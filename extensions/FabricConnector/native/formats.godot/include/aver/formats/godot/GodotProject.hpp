#pragma once
// Godot's ConfigFile-style files: project.godot, *.import, *.gdextension (and plugin.cfg, which is
// the same grammar). Contract: docs/plans/fabric/G3_FORMATS.md.
#include "aver/formats/godot/GodotSections.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::fmt::godot {

// A parsed ConfigFile: sections of `key = value`. Top-level keys (before any section) live in the
// section named "".
struct GodotConfigSection {
    std::string name;
    std::vector<GodotProperty> entries;
    int line = 0;
};

struct GodotConfig {
    std::string file;
    std::vector<GodotConfigSection> sections;

    const GodotConfigSection* section(std::string_view name) const;
    const GodotVariant* get(std::string_view section, std::string_view key) const;
    std::string getString(std::string_view section, std::string_view key, std::string_view def = {}) const;
};

bool parseGodotConfig(std::string_view text, std::string_view file, GodotConfig& out, GodotDiagnostics& diag);
bool loadGodotConfig(const std::string& path, GodotConfig& out, GodotDiagnostics& diag, std::string_view displayName = {});

// ---- project.godot ---------------------------------------------------------------------------------

struct GodotAutoload {
    std::string name;     // the singleton's name
    std::string path;     // "res://..." without the leading '*'
    bool singleton = false;  // the value began with '*': the node is added to the root under `name`
    int order = 0;        // position in the file (autoloads initialise in this order)
    int line = 0;
};

// One `[input]` action: `ui_accept={ "deadzone": 0.5, "events": [Object(InputEventKey, ...), ...] }`.
struct GodotInputAction {
    std::string name;
    f64 deadzone = 0.5;
    std::vector<GodotVariant> events;  // Object variants (InputEventKey, InputEventMouseButton, ...)
    int line = 0;
};

struct GodotProject {
    GodotConfig config;       // the raw sections
    std::string dir;          // the project folder (set by loadGodotProject); empty from parse
    int configVersion = 0;    // 5 = Godot 4.x
    std::string name;         // application/config/name
    std::string description;  // application/config/description
    std::string mainScene;    // application/run/main_scene (a res:// or uid:// string)
    std::string icon;         // application/config/icon
    std::vector<std::string> features;  // application/config/features: ["4.3", "Forward Plus", "C#"]
    std::string engineVersion;          // the "4.3"-like entry of `features`, or empty
    bool usesCSharp = false;            // features contains "C#"
    std::string renderingMethod;        // "forward_plus", "mobile" or "gl_compatibility"
    std::vector<GodotAutoload> autoloads;
    std::vector<GodotInputAction> inputActions;
    std::vector<std::string> enabledPlugins;  // editor_plugins/enabled (plugin.cfg res:// paths)
    // [layer_names]: "3d_physics/layer_1" -> "Player" etc, in file order.
    std::vector<std::pair<std::string, std::string>> layerNames;

    // A setting by its full path "section/key/subkey": the first component is the file section, the
    // rest the key ("application/config/name"). nullptr if absent.
    const GodotVariant* setting(std::string_view path) const;
    bool hasFeature(std::string_view f) const;
};

// Godot 4 (config_version 5) reads; an older config_version is an error ("Godot 3 project"); an
// engine version below 4.1 in `features` warns.
bool parseGodotProject(std::string_view text, std::string_view file, GodotProject& out, GodotDiagnostics& diag);
// `path` is a project.godot file or the folder that holds one.
bool loadGodotProject(const std::string& path, GodotProject& out, GodotDiagnostics& diag);

// ---- *.import --------------------------------------------------------------------------------------

struct GodotImport {
    std::string file;
    std::string importer;    // [remap] importer: "texture", "scene", "wavefront_obj", "dynamic_font", ...
    std::string type;        // [remap] type: "CompressedTexture2D", "PackedScene", ...
    std::string uid;         // [remap] uid
    bool valid = true;       // [remap] valid=false: the editor flagged the import as broken
    std::string path;        // [remap] path (the primary imported file under .godot/imported)
    // [remap] path.<tag>: per-platform-format outputs ("s3tc_bptc" -> res://...)
    std::vector<std::pair<std::string, std::string>> pathsByTag;
    std::string sourceFile;  // [deps] source_file
    std::vector<std::string> destFiles;  // [deps] dest_files
    std::vector<GodotProperty> params;   // [params]: the importer settings, names as written
    GodotVariant metadata;               // [remap] metadata (a Dictionary), Nil if absent
    GodotConfig config;

    const GodotVariant* param(std::string_view name) const;
};

bool parseGodotImport(std::string_view text, std::string_view file, GodotImport& out, GodotDiagnostics& diag);
bool loadGodotImport(const std::string& path, GodotImport& out, GodotDiagnostics& diag, std::string_view displayName = {});

// ---- *.gdextension ---------------------------------------------------------------------------------

struct GodotGdLibrary {
    std::string key;                    // "windows.debug.x86_64"
    std::vector<std::string> tags;      // {"windows", "debug", "x86_64"}
    std::string path;                   // res://bin/libfoo.windows.debug.x86_64.dll
    int line = 0;
};

struct GodotGdExtension {
    std::string file;
    std::string entrySymbol;            // [configuration] entry_symbol
    std::string compatibilityMinimum;   // "4.1"
    std::string compatibilityMaximum;   // "4.2", empty if absent
    bool reloadable = false;
    std::vector<GodotGdLibrary> libraries;                         // [libraries], file order
    std::vector<std::pair<std::string, std::string>> icons;        // [icons]: class -> res:// path
    // [dependencies]: feature key -> list of (source res:// path, destination folder)
    struct Dependency {
        std::string key;
        std::vector<std::pair<std::string, std::string>> files;
    };
    std::vector<Dependency> dependencies;
    GodotConfig config;

    // Godot's selection rule: the first [libraries] entry (file order) all of whose tags are in
    // `features` (e.g. {"windows","debug","x86_64","double"...}). nullptr if none matches.
    const GodotGdLibrary* selectLibrary(const std::vector<std::string>& features) const;
};

bool parseGodotGdExtension(std::string_view text, std::string_view file, GodotGdExtension& out, GodotDiagnostics& diag);
bool loadGodotGdExtension(const std::string& path, GodotGdExtension& out, GodotDiagnostics& diag,
                          std::string_view displayName = {});

}  // namespace aver::fmt::godot
