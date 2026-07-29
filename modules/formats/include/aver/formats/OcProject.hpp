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

    // ---- RENDER.*, the project's authoring intent about how the game should look ----------------
    //
    // IN THE MANIFEST because they are a decision about the GAME, not about the machine: a colleague
    // opening this project should get the lighting the author chose. Contrast the editor's own
    // preferences (window widths, which IDE, vsync), which are this machine's taste and live in
    // editor.ini -- writing those into a project a team shares would be writing one person's setup
    // into everybody's checkout.
    //
    // DELIBERATELY NOT HERE: the GI volume's centre and extent, which are FIT TO THE LEVEL's bounds
    // and so belong to a world rather than to the project; MSAA and mesh shaders, which are device
    // capabilities the editor adopts from the swapchain at startup; and the GI debug view, which is
    // a way of looking at a frame rather than a property of the game.
    //
    // -1 MEANS "NOT STATED", for every one of them. A project written before these keys existed --
    // every project on disk today -- must load and keep whatever the engine's own defaults are,
    // rather than being told its GI is off because a number was absent. The caller applies a stated
    // value and leaves an unstated one alone.
    int giQuality       = -1;   // RENDER.GI            0=Off 1=Low 2=Medium 3=High 4=Epic
    int rayTracing      = -1;   // RENDER.RAYTRACING    same scale
    int pathTracing     = -1;   // RENDER.PATHTRACING   same scale
    int voxelResolution = -1;   // RENDER.VOXELRES      64 / 128 / 256
    f32 giIntensity     = -1.0f;// RENDER.GIINTENSITY   indirect bounce multiplier
    f32 giMaxDistance   = -1.0f;// RENDER.GIDISTANCE    cone trace range, centimetres

    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f;
    }

    std::string dir;                    // absolute directory the manifest lives in
    std::string manifestPath;           // absolute path to the .ocproject itself

    bool valid() const { return !name.empty() && !dir.empty(); }
    // Absolute content mount root. Empty when the project was parsed from memory with no `dir`.
    std::string contentDir() const { return dir.empty() ? std::string() : dir + "\\" + contentRoot; }
    std::string scriptsDir() const { return dir.empty() ? std::string() : contentDir() + "\\Scripts"; }
    // Build OUTPUT, beside Content rather than inside it: what a compiler wrote, never what a person
    // authored. Scripts.dll is staged here already; the material compiler writes Materials\*.ocmat
    // here from the .cs sources under Content\Materials. Not in the manifest, because it is not a
    // choice -- a project that could relocate its build output would be a project whose .gitignore,
    // packaging step and clean command each had to be told separately.
    std::string binariesDir() const { return dir.empty() ? std::string() : dir + "\\Binaries"; }
};

// Parse from memory. `dir`/`manifestPath` are left for the caller to fill.
// Unknown keys are IGNORED, not an error: docs/PROJECTS.md declares the format
// forward-compatible, so a manifest written by a newer build must still load here.
bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err = nullptr);

// Serialise a manifest back to text.
//
// PRESERVES THE FILE IT IS GIVEN. `existing` is the current contents; every line that is not a key
// this function owns is copied through untouched, in place, and only the owned keys are rewritten or
// appended. That is not politeness -- SkyForge's manifest carries a hand-written comment block and a
// commented-out AUTHOR line, and a serialiser that rebuilt the file from ProjectDesc would silently
// delete both. Pass an empty string to write a fresh manifest.
//
// Returns the new text. Never touches the disk: the caller decides when a project is written, which
// is the rule the whole editor follows about other people's files.
std::string writeOcproject(const ProjectDesc& d, std::string_view existing);

// Load from disk and enforce the ENGINE line against this build. Fails — rather than loading
// something the engine cannot honour — when the manifest names a different engine or asks for a
// version newer than ours. That refusal is the entire reason the key exists.
bool loadOcproject(const std::string& path, ProjectDesc& out, std::string* err = nullptr);

} // namespace aver::fmt
