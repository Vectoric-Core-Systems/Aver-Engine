#pragma once
// .ocproject — the manifest that makes a folder a project. Text, OC dialect (`#` comments,
// `KEY value`), same scanner as .ocbeam/.ocmap. See docs/PROJECTS.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// A parsed manifest: what the project is called, where its content lives, and its render intent.
struct ProjectDesc {
    int version = 1;                    // OCPROJECT <n>
    std::string name;                   // NAME
    std::string engineName;             // ENGINE <name> ...
    std::string engineMinVersion;       // ENGINE ... <minVersion>

    // CREATEDWITH: the engine version this project was last opened and stamped by. NOT the same
    // thing as engineMinVersion, which is a FLOOR the manifest may state ("needs at least 0.2");
    // this records what actually touched it, so the editor can tell an 0.1-era project from a
    // current one and run the migration chain between.
    //
    // EMPTY MEANS CURRENT, DELIBERATELY. Every project that existed before this field did has no
    // value here, and treating empty as "very old" would prompt every author on earth to migrate
    // something that is probably fine. Empty is adopted: stamped with today's version on open, no
    // prompt. Only a project stamped with an OLDER SERIES is ever asked to upgrade.
    std::string createdWith;            // CREATEDWITH <version>
    std::string contentRoot = "Content"; // CONTENT, relative to the manifest
    std::string startMap;               // STARTMAP, relative to the content root
    std::string author;                 // AUTHOR (free text, rest of line)

    // RENDER.*, all -1 when the manifest did not state them.
    int giQuality       = -1;   // RENDER.GI            0=Off 1=Low 2=Medium 3=High 4=Epic
    int rayTracing      = -1;   // RENDER.RAYTRACING    same scale
    int pathTracing     = -1;   // RENDER.PATHTRACING   same scale
    int voxelResolution = -1;   // RENDER.VOXELRES      64 / 128 / 256
    f32 giIntensity     = -1.0f;// RENDER.GIINTENSITY   indirect bounce multiplier
    f32 giMaxDistance   = -1.0f;// RENDER.GIDISTANCE    cone trace range, centimetres
    int rtShadowRays       = -1; // RENDER.RTSHADOWRAYS     sun occlusion rays/pixel, [1,32]
    int rtPixelsPerRayTile = -1; // RENDER.RTPIXELSPERRAY   shadow amortisation tile edge, [1,16]
    int rtShadowDenoise    = -1; // RENDER.RTSHADOWDENOISE  SPATIAL filter radius in pixels, [0,3]

    // True when the manifest stated at least one RENDER.* key.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0;
    }

    std::string dir;                    // absolute directory the manifest lives in
    std::string manifestPath;           // absolute path to the .ocproject itself

    bool valid() const { return !name.empty() && !dir.empty(); }
    // Absolute content mount root. Empty when the project was parsed from memory with no `dir`.
    std::string contentDir() const { return dir.empty() ? std::string() : dir + "\\" + contentRoot; }
    std::string scriptsDir() const { return dir.empty() ? std::string() : contentDir() + "\\Scripts"; }
    // Absolute build output directory, beside Content rather than inside it.
    std::string binariesDir() const { return dir.empty() ? std::string() : dir + "\\Binaries"; }
};

// Parses a manifest from memory. Unknown keys are ignored; `dir`/`manifestPath` are the caller's.
bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err = nullptr);

// Serialises a manifest, copying every line of `existing` that is not an owned key through untouched.
// Pass an empty string to write a fresh manifest. Never touches the disk.
std::string writeOcproject(const ProjectDesc& d, std::string_view existing);

// Loads a manifest from disk and rejects one whose ENGINE line names another engine or a version
// newer than this build.
bool loadOcproject(const std::string& path, ProjectDesc& out, std::string* err = nullptr);

} // namespace aver::fmt
