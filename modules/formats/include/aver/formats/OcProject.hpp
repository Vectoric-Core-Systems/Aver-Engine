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
    // DRONE.GRAPH -- the .ocgraph that flies this project's drone, relative to the content root.
    //
    // EXISTS BECAUSE THE DRONE COULD ONLY EVER BE FLOWN FROM A COMMAND LINE. The graph that drives
    // it was settable by --drone-graph and by nothing else: no UI, no project key. So starting the
    // drone from the editor -- from Play, or from the Drone window -- spawned it with no graph and
    // it sat perfectly still, which is what "the drone is glitched" looks like from the outside. Its
    // own warning said so ("It will sit still"), into a log nobody reading the viewport was watching.
    //
    // A PROJECT KEY RATHER THAN A CONVENTIONAL FILENAME, deliberately. setDroneGraph's comment is
    // right that the engine must not assume a project contains a file with any particular name --
    // so the project says which file, and a project that says nothing still gets a drone that sits
    // still, but now because it was never told where to fly rather than because there was no way to
    // tell it.
    std::string droneGraph;             // DRONE.GRAPH, relative to the content root; may be empty

    // INPUT.SCHEME -- the .ocinput that supplies this project's default input bindings, relative to
    // the content root. Empty means none, for DRONE.GRAPH's exact reason directly above: no context
    // is pushed for the project automatically, rather than a loader guessing at a path.
    //
    // A PROJECT KEY, NOT AN ASSUMED FILENAME, for the identical reason DRONE.GRAPH is one (see that
    // field's comment above -- this key repeats its argument rather than restating it): the engine
    // must not require a project to keep its default bindings at one particular path under Content,
    // so the manifest says which file rather than a loader guessing e.g. "Input/Default.ocinput" and
    // silently finding nothing in a project laid out differently. A project stating no INPUT.SCHEME
    // still runs -- gameplay code can still call EnhancedInput.AddContext directly with a hand-built
    // context -- but a project whose bindings are DATA (see aver/formats/OcInput.hpp) rather than a
    // compiled-in InputMappingContext subclass now has one place to say where that data lives.
    std::string inputScheme;            // INPUT.SCHEME, relative to the content root; may be empty
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
    int rtRenderMode       = -1; // RENDER.RTRENDERMODE     0 = raster primary, 1 = ray-driven
    int ptBounces          = -1; // RENDER.PTBOUNCES        PATH tracing: bounces after the first hit, [1,8]
    int layeredBsdf        = -1; // RENDER.LAYEREDBSDF      0=Off 1=Low 2=Medium 3=High 4=Epic

    // True when the manifest stated at least one RENDER.* key.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0 ||
               rtRenderMode >= 0 || ptBounces >= 0 || layeredBsdf >= 0;
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
