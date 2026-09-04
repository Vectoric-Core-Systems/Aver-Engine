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
    // Clamped to [32, 512] by Voxi.cpp's setSettings, and Epic's own derived rung IS 512 -- the
    // comment here used to say "64 / 128 / 256", which named neither the real floor nor the real
    // ceiling and made a legal 512 look like a typo. 512^3 RGBA16F is ~1.2 GiB with its mip chain,
    // so the ceiling is a real choice rather than a formality.
    int voxelResolution = -1;   // RENDER.VOXELRES      grid edge, 32..512 (Epic derives 512)
    f32 giIntensity     = -1.0f;// RENDER.GIINTENSITY   indirect bounce multiplier
    f32 giMaxDistance   = -1.0f;// RENDER.GIDISTANCE    cone trace range, centimetres
    int rtShadowRays       = -1; // RENDER.RTSHADOWRAYS     sun occlusion rays/pixel, [1,32]
    int rtPixelsPerRayTile = -1; // RENDER.RTPIXELSPERRAY   shadow amortisation tile edge, [1,16]
    int rtShadowDenoise    = -1; // RENDER.RTSHADOWDENOISE  SPATIAL filter radius in pixels, [0,3]
    int rtRenderMode       = -1; // RENDER.RTRENDERMODE     0 = raster primary, 1 = ray-driven
    int ptBounces          = -1; // RENDER.PTBOUNCES        PATH tracing: bounces after the first hit, [1,8]
    int layeredBsdf        = -1; // RENDER.LAYEREDBSDF      0=Off 1=Low 2=Medium 3=High 4=Epic
    int giCones            = -1; // RENDER.GICONES          diffuse gather cones, [1,16]
    int refractionMode     = -1; // RENDER.REFRACTIONMODE   0=Off 1=Screen-space 2=Ray-traced
    f32 refractionStrength = -1.0f; // RENDER.REFRACTIONSTRENGTH  bend scale
    f32 refractionEdgeFade = -1.0f; // RENDER.REFRACTIONEDGEFADE  hides the screen-space miss at edges
    int lodSelect          = -1; // RENDER.LODSELECT        0/1; off draws every instance at LOD 0
    f32 lodThresholdPx     = -1.0f; // RENDER.LODTHRESHOLD  screen-space error, pixels
    int occlusionCull      = -1; // RENDER.OCCLUSIONCULL    0/1
    int depthPrepass       = -1; // RENDER.DEPTHPREPASS     0/1

    // True when the manifest stated at least one RENDER.* key.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0 ||
               rtRenderMode >= 0 || ptBounces >= 0 || layeredBsdf >= 0 ||
               giCones >= 0 || refractionMode >= 0 || refractionStrength >= 0.0f ||
               refractionEdgeFade >= 0.0f || lodSelect >= 0 || lodThresholdPx >= 0.0f ||
               occlusionCull >= 0 || depthPrepass >= 0;
    }

    // PHYSICS.* -- the world-wide defaults a project starts its simulation with.
    //
    // GRAVITY NEEDS A PRESENCE FLAG, NOT A SENTINEL, and that is the whole reason this section is
    // shaped differently from RENDER.*: gravity points DOWN, so every component a project would
    // realistically state is negative, and the `< 0 means unstated` rule the RENDER keys use would
    // make the only interesting value unwritable.
    bool hasGravity = false;
    f32  gravity[3] = {0.0f, 0.0f, -980.0f};   // cm/s^2, engine axes; one g down +Z-up
    // Seconds. Strictly positive, so -1 can mean unstated here where it cannot for gravity.
    f32  fixedStep  = -1.0f;                   // PHYSICS.FIXEDSTEP

    bool hasPhysicsSettings() const { return hasGravity || fixedStep > 0.0f; }

    // AUDIO.* -- the mix a project starts at. Bus order matches audio_abi.h's
    // AVER_AUDIO_BUS_SFX / MUSIC / VOICE / UI.
    //
    // ONE PRESENCE FLAG FOR THE WHOLE MIX, because zero is the most meaningful value any of these
    // takes: a project that ships with music muted must be able to say so, and a `< 0 means
    // unstated` rule would silently turn that into "leave it at 1".
    bool hasAudioMix = false;
    f32  masterVolume = 1.0f;                  // AUDIO.MASTER
    f32  busVolume[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // AUDIO.BUS <sfx> <music> <voice> <ui>

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
