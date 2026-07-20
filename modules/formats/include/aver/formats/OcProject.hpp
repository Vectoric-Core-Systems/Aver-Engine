#pragma once
// .ocproject — the manifest that makes a folder a project. Text, OC-dialect
// (`#` comments, `KEY value`), same scanner as .ocbeam/.ocmap. See docs/PROJECTS.md.
//
// The engine never contains project content, so everything a project needs to be found lives
// here: the content mount root and the start map are relative to the manifest, and `dir` is the
// absolute folder they resolve against.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

struct ProjectDesc {
    int version = 1;                    // OCPROJECT <n>
    std::string name;                   // NAME
    std::string engineName;             // ENGINE <name> ...
    std::string engineMinVersion;       // ENGINE ... <minVersion>
    std::string contentRoot = "Content"; // CONTENT, relative to the manifest
    std::string startMap;               // STARTMAP, relative to the content root
    std::string author;                 // AUTHOR (free text, rest of line)

    std::string dir;                    // absolute directory the manifest lives in
    std::string manifestPath;           // absolute path to the .ocproject itself

    bool valid() const { return !name.empty() && !dir.empty(); }
    // Absolute content mount root. Empty when the project was parsed from memory with no `dir`.
    std::string contentDir() const { return dir.empty() ? std::string() : dir + "\\" + contentRoot; }
    std::string scriptsDir() const { return dir.empty() ? std::string() : contentDir() + "\\Scripts"; }
};

// Parse from memory. `dir`/`manifestPath` are left for the caller to fill.
// Unknown keys are IGNORED, not an error: docs/PROJECTS.md declares the format
// forward-compatible, so a manifest written by a newer build must still load here.
bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err = nullptr);

// Load from disk and enforce the ENGINE line against this build. Fails — rather than loading
// something the engine cannot honour — when the manifest names a different engine or asks for a
// version newer than ours. That refusal is the entire reason the key exists.
bool loadOcproject(const std::string& path, ProjectDesc& out, std::string* err = nullptr);

} // namespace aver::fmt
