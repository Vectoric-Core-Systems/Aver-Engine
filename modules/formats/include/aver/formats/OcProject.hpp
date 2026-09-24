#pragma once
// .ocproject — the manifest that makes a folder a project. Text, OC dialect (`#` comments,
// `KEY value`), same scanner as .ocbeam/.ocmap. See docs/PROJECTS.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// A parsed manifest: what the project is called, where its content lives, and its render intent.
// The highest OCPROJECT version this build can read. A file claiming more is REFUSED rather than
// read as version 1 -- see parseOcproject. Bump this in the same change that teaches the parser
// whatever the new version means, never ahead of it.
inline constexpr int kOcProjectVersion = 1;

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
    // RENDER.RDSTAGES: milestone 1's A/B switch over the ray-driven primary's own internal shape --
    // see voxi::Settings::rayDrivenStages, which owns the full explanation. 0 = single pass (today's
    // one drawFullscreen, the default and the comparison baseline), 1 = staged visibility -> shadow
    // -> shade, D3D12 only, falling back to single pass wherever the staged path is unavailable.
    // Milestone 4 adds 2 = staged + half-rate GI: the same staged path, but the ReSTIR GI stage
    // checkerboards its rays (NRD's pattern) and REBLUR reconstructs the rest -- unlike 1, this
    // deliberately changes the image rather than staying a same-image comparison.
    //
    // NOT TIER-DERIVED, THE SAME SHAPE AS RESTIRHISTORY BELOW, NOT RTRENDERMODE JUST ABOVE: this
    // field has no ladder rung to fall back to (Voxi.hpp's own comment on the field says so
    // outright), so an absent key simply leaves the engine's default (0) alone regardless of any RT
    // tier change -- ProjectRenderApply.hpp applies it the way it applies giMode/denoiser/
    // restirHistory, not the way it applies rtRenderMode.
    int rdStages           = -1; // RENDER.RDSTAGES         0 = single pass (default), 1 = staged,
                                  //                         2 = staged + half-rate GI
    int ptBounces          = -1; // RENDER.PTBOUNCES        PATH tracing: bounces after the first hit, [1,8]
    int layeredBsdf        = -1; // RENDER.LAYEREDBSDF      0=Off 1=Low 2=Medium 3=High 4=Epic
    int giCones            = -1; // RENDER.GICONES          diffuse gather cones, [1,16]
    // RENDER.GIMODE: 0 = voxel cone gather (the default and what every project written before
    // this key existed means by its absence), 1 = ReSTIR GI. -1 keeps the engine's own default, so
    // an older .ocproject is unchanged in meaning as well as in bytes.
    int giMode             = -1; // RENDER.GIMODE           diffuse GI algorithm, [0,1]
    // RENDER.DENOISER: 0/1, NVIDIA NRD over the sky occlusion and the ReSTIR GI radiance. Needs the
    // G-buffer and MSAA 1 -- see Voxi::Settings::denoiser, which owns the explanation.
    int denoiser           = -1; // RENDER.DENOISER         0/1
    // RENDER.RESTIRVISIBILITY: the ReSTIR GI contrast fix's two per-pixel rays (F2 candidate-hit sky,
    // F3 reuse) -- see voxi::Settings::giRestirVisibility, which owns the explanation. -1 is one of the
    // TEN tier-derived GI/RT/PT knobs ProjectRenderApply.hpp's N6 rule covers: absent follows whatever
    // the GI tier's own ladder rung says, not whatever value happened to already be live.
    int restirVisibility   = -1; // RENDER.RESTIRVISIBILITY  0=no ray 1=reconstructed 2=half 3=full
    // RENDER.RESTIRHISTORY: RTXDI's stparams.maxHistoryLength -- how much weight a previous-frame
    // reservoir may carry into the ReSTIR GI combine; see voxi::Settings::giRestirMaxHistory, which
    // owns the full explanation and the Sponza numbers behind it (674ed667: history 0 measured no
    // overshoot at rest or moving, 1 -- the old default -- +8%, 8 +104%, all versus settled).
    //
    // NOT ONE OF THE TEN TIER-DERIVED KNOBS just above (restirVisibility included): the field has no
    // ladder rung to fall back to (Voxi.hpp's own comment on giRestirMaxHistory says so outright), so
    // absent here leaves the engine's default (0) alone regardless of the GI tier -- ProjectRenderApply
    // .hpp applies it the same way it applies giMode/denoiser, not the way it applies restirVisibility.
    int restirHistory      = -1; // RENDER.RESTIRHISTORY    ReSTIR GI temporal history weight, frames
    int refractionMode     = -1; // RENDER.REFRACTIONMODE   0=Off 1=Screen-space 2=Ray-traced
    f32 refractionStrength = -1.0f; // RENDER.REFRACTIONSTRENGTH  bend scale
    f32 refractionEdgeFade = -1.0f; // RENDER.REFRACTIONEDGEFADE  hides the screen-space miss at edges
    int lodSelect          = -1; // RENDER.LODSELECT        0/1; off draws every instance at LOD 0
    f32 lodThresholdPx     = -1.0f; // RENDER.LODTHRESHOLD  screen-space error, pixels
    int occlusionCull      = -1; // RENDER.OCCLUSIONCULL    0/1
    int depthPrepass       = -1; // RENDER.DEPTHPREPASS     0/1

    // WHICH RHI BACKEND THIS PROJECT WANTS. Empty = whatever the engine picks on its own, which is
    // the behaviour every project had before this key existed, so an old manifest is unaffected.
    //
    // A PREFERENCE, NOT A GUARANTEE, and it cannot be otherwise: a backend has to be COMPILED IN to
    // be selectable (AVER_RHI_VULKAN is OFF in the default CMake configuration), and even a compiled
    // one can fail to create a device on a given machine. Engine::run already falls back in that
    // case; the only thing this key changes is which backend is ASKED FOR first.
    //
    // READ EARLIER THAN EVERY OTHER KEY IN THIS STRUCT. The device is created before the editor
    // opens a project, so applying this the way the rest of RenderSettings is applied -- per frame,
    // after load -- would be far too late to choose a device. The peek is inline in the sandbox's
    // own main(), just before setBackend: search there for RENDER.BACKEND. (This comment named a
    // `peekBackend()` helper that was never written.)
    std::string backend;        // RENDER.BACKEND  "d3d12" | "vulkan" | "d3d11"; empty = engine default

    // A FRAME TIME TO AIM AT, in milliseconds. <= 0 (the default) leaves quality exactly as
    // authored, which is what every existing project gets and what every MEASUREMENT needs -- a
    // renderer that quietly retunes itself cannot be A/B'd against anything.
    f32 frameBudgetMs = -1.0f;  // RENDER.FRAMEBUDGETMS  e.g. 16.7 for 60 Hz; <= 0 = off

    // RENDER.AVERSR: the project-level default for AverSR upscaling. Deliberately NOT a voxi::Settings
    // field and NOT applied through ProjectRenderApply.hpp -- render.voxi must never include render.sr
    // (the module boundary docs/AVERSR.md states), so both hosts read project.averSr directly rather
    // than through the shared Voxi apply path every other RENDER.* key above goes through. -1 follows
    // the Overall rung's own AverSR default (ladder::averSrLevel); the editor's Display > AverSR choice
    // and CLI --aversr both outrank this when they are stated.
    int averSr = -1; // RENDER.AVERSR  -1 follow preset, 0 off, 1 quality, 2 balanced, 3 performance

    // ---- FOUR THINGS THE UI COULD SET AND THE FILE COULD NOT HOLD ------------------------------
    //
    // Each of these was a live control in Project Settings that applied immediately and then
    // vanished on the next open, because captureRenderSettingsFromUi never read it and there was no
    // key to write it to. Set MSAA to 8x, save, reopen: 4x. That is worse than the setting not
    // existing, because the editor showed it working.
    int msaa            = -1;   // RENDER.MSAA           1/2/4/8 samples
    int meshShaders     = -1;   // RENDER.MESHSHADERS    0/1
    // Also closes a flag-vs-manifest asymmetry: --gi-update-interval existed with no key at all,
    // while giUpdateInterval is TIER-DERIVED inside voxi::Renderer::setSettings -- so opening a
    // project mid-session could silently re-derive over the flag with nothing logged. A key here
    // plus a take() entry in applyProjectRenderSettings makes the two channels symmetric.
    int giUpdateInterval = -1;  // RENDER.GIUPDATEINTERVAL  frames between GI volume refreshes

    // The GI volume's placement. Two live sliders in Project Settings > Global Illumination that
    // set neither projectDirty_ nor any manifest key, so a project could never state where its
    // indirect light is gathered -- on a level whose interesting geometry is not at the origin,
    // that is the difference between GI working and GI being somewhere else.
    //
    // A PRESENCE FLAG rather than a sentinel, for PHYSICS.GRAVITY's reason: every component of a
    // centre is legitimately negative.
    //
    // THE EXTENT IS ONE NUMBER, not three, because the volume is a CUBE -- the renderer holds it as
    // `Vec3 giCenter_` plus a scalar `f32 giExtent_` and setVolume takes exactly that pair. A
    // three-component extent here would be a format promising a shape the engine cannot make.
    bool hasGiVolume = false;   // RENDER.GIVOLUME <cx> <cy> <cz> <extent>
    f32  giCenter[3] = {0, 0, 0};
    f32  giExtent    = 0.0f;    // cm, half-edge of the cube

    // ---- THE CAMERA'S POST CHAIN, WHICH A PROJECT COULD NOT AUTHOR AT ALL ----------------------
    //
    // docs/RUNTIME-DEDUP.md records the gap as an owner decision in one line: "A project cannot
    // author exposure/bloom/tonemap, so a shipped game uses compiled defaults." Every one of these
    // was settable from a command line (--bloom, --exposure, --auto-exposure at
    // sandbox/src/SandboxMain.cpp:986-988, --tonemap at :419) and from the editor's own Post panel,
    // and by nothing the FILE could hold -- so a packaged game rendered with rhi::PostSettings'
    // compiled defaults (exposure 1, bloom 0.06, auto-exposure on, tonemap 2; RHI.hpp:146-217)
    // whatever the author had spent the afternoon looking at in the editor. Same shape as the "four
    // things the UI could set and the file could not hold" above, one step worse: there the setting
    // came back wrong on the next open, here it came back wrong in the SHIPPED GAME.
    //
    // A NEGATIVE SENTINEL IS MANDATORY FOR THE TWO FLOATS, not merely conventional. Zero is a legal
    // authored value for both -- bloom 0 is precisely how an author says "no bloom", and it is a
    // meaningful thing to say because zero intensity builds no pyramid and records no pass at all
    // (RHI.hpp:151-152), so it is a performance choice as well as a look. The usual "0 means the
    // manifest never said" shortcut would make the one interesting value unwritable, for the same
    // reason AUDIO.* below carries a presence flag rather than a sentinel: a project that ships with
    // its music muted has to be able to say so. -1 means the manifest was silent and the compiled
    // default stands; 0 means the author said zero, and it must come back a zero.
    f32 postExposure     = -1.0f; // RENDER.EXPOSURE      linear pre-tonemap multiplier; < 0 = unstated
    f32 postBloom        = -1.0f; // RENDER.BLOOM         bloom intensity; 0 = no bloom pass at all
    int postAutoExposure = -1;    // RENDER.AUTOEXPOSURE  0/1; when on it overrides EXPOSURE per frame
    // RENDER.TONEMAP: a genuine SELECTOR rather than a fourth exposure knob, which is the only
    // reason it earns a key of its own -- rhi::PostSettings::tonemap (RHI.hpp:217) chooses between
    // the per-channel Narkowicz/Hill approximation (0), the same curve applied between the ACES
    // matrices (1), and acesLumaTonemap (2, the default). The choice is visible rather than
    // academic: measured on PTTest Sponza at exposure 8, mode 1 holds chroma 1.41 where mode 2 holds
    // 3.09 at the same brightness (the numbers and the "colours are washed out" report they came
    // from are recorded at RHI.hpp:196-216). 0 is also the mode every recorded gate baseline in
    // scripts/ was measured through, so a project that exists to reproduce one has to be able to say
    // so in the file rather than on a command line nobody will remember to pass.
    int postTonemap      = -1;    // RENDER.TONEMAP  0/1/2; -1 keeps PostSettings' own default (2)

    // True when the manifest stated at least one RENDER.* key.
    //
    // GIMODE AND DENOISER WERE MISSING (N9), and a manifest stating only those two keys was invisible
    // to both loaders: hasRenderSettings() gated the whole apply block in each of them (SandboxApp.cpp's
    // applyProjectRenderSettings, GameApp::applyProjectRenderSettings), so RENDER.GIMODE 1 or
    // RENDER.DENOISER 1 with nothing else stated in the file was parsed correctly into giMode/denoiser
    // above and then never read by either apply function, silently. No format change and no new key --
    // the two fields already existed; this OR-chain was simply never taught about them.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0 ||
               rtRenderMode >= 0 || ptBounces >= 0 || layeredBsdf >= 0 ||
               giCones >= 0 || giMode >= 0 || denoiser >= 0 || restirVisibility >= 0 ||
               restirHistory >= 0 || rdStages >= 0 ||
               refractionMode >= 0 || refractionStrength >= 0.0f ||
               refractionEdgeFade >= 0.0f || lodSelect >= 0 || lodThresholdPx >= 0.0f ||
               occlusionCull >= 0 || depthPrepass >= 0 ||
               msaa >= 0 || meshShaders >= 0 || giUpdateInterval >= 0 || hasGiVolume ||
               postExposure >= 0.0f || postBloom >= 0.0f ||
               postAutoExposure >= 0 || postTonemap >= 0 ||
               !backend.empty() || frameBudgetMs > 0.0f || averSr >= 0;
    }

    // ---- WINDOW.* -- how a shipped game presents itself -----------------------------------------
    //
    // docs/PACKAGING.md named this gap outright: ".ocproject cannot describe a shipped game. It has
    // no entry point, no window/resolution defaults, no build id, no icon." The old design put those
    // in a side-car game.json, which no reader or writer for has ever existed -- so a packaged game
    // took platform::WindowDesc's compiled-in 1280x720 and the project could say nothing about it.
    //
    // THE PROJECT IS THE RIGHT HOME, not game.json: the editor can author these, they belong to the
    // title rather than to the build, and stage-game.ps1 can generate game.json FROM them instead of
    // being a second place the same facts are written.
    std::string windowTitle;        // WINDOW.TITLE <prose>   empty = use NAME
    int windowWidth     = -1;       // WINDOW.SIZE <w> <h>
    int windowHeight    = -1;
    int windowResizable = -1;       // WINDOW.RESIZABLE 0/1
    int windowFullscreen = -1;      // WINDOW.FULLSCREEN 0/1  borderless-fullscreen intent
    bool hasWindowSettings() const {
        return !windowTitle.empty() || windowWidth > 0 || windowHeight > 0 ||
               windowResizable >= 0 || windowFullscreen >= 0;
    }

    // ---- IMPORT.* -- what the asset compiler assumes when a file does not say -------------------
    //
    // The single most conventional Project Settings category this engine lacked. Every importer has
    // an options struct (GltfImportOptions, ObjImportOptions, UsdImportOptions, TextureLoadOptions,
    // MaterialCookOptions) and NONE of them was reachable from a project -- so "this project's
    // source art is in metres" was a fact that could only be re-stated on every command line.
    //
    // STILL TRUE, AND ADDING THESE KEYS DID NOT CLOSE IT. Nothing constructs any of those five
    // option structs from a ProjectDesc: the keys parse, serialise and have Project Settings
    // widgets, and AverAssetC takes its options from its own command line without opening the
    // owning .ocproject. Said here rather than only in the UI's disclosure text because the
    // paragraph above otherwise reads as a gap that was closed, and the next reader would have to
    // grep the importers to find out it was not.
    f32 importScale       = -1.0f;  // IMPORT.SCALE <f>       source unit -> cm (100 = metres)
    int importConvertAxes = -1;     // IMPORT.CONVERTAXES 0/1 Y-up right-handed -> Z-up left-handed
    int importGenNormals  = -1;     // IMPORT.GENNORMALS 0/1  synthesise missing normals
    int importGenMips     = -1;     // IMPORT.GENMIPS 0/1
    int importMaxTexture  = -1;     // IMPORT.MAXTEXTURE <px> downscale ceiling, 0 = no cap
    bool hasImportSettings() const {
        return importScale >= 0.0f || importConvertAxes >= 0 || importGenNormals >= 0 ||
               importGenMips >= 0 || importMaxTexture >= 0;
    }

    // ---- STREAM.* -- world streaming budgets ----------------------------------------------------
    //
    // world::StreamSettings is exactly the set a shipping title tunes per-platform, and it lived in
    // compiled-in defaults with no file and no UI. loadBudget in particular is documented as "the
    // only thing bounding the frame hitch, since the load is synchronous".
    //
    // APPLIED IN GameStreaming::enable, which seeds every field's ChunkWorldSettings::stream from
    // these before a PCGVOLUME's own radiusChunks overrides the two radii. They spent a while
    // parsed, written back out and editable in Project Settings while reaching nothing at all, so
    // the read side is named here: a key whose only consumer is the file it came from is the shape
    // this struct has been caught in more than once.
    int streamLoadRadius     = -1;  // STREAM.LOADRADIUS
    int streamEvictRadius    = -1;  // STREAM.EVICTRADIUS
    int streamLoadBudget     = -1;  // STREAM.LOADBUDGET     chunks loaded per frame
    int streamEvictBudget    = -1;  // STREAM.EVICTBUDGET
    int streamVerticalRadius = -1;  // STREAM.VERTICALRADIUS
    f32 streamLeadSeconds    = -1.0f; // STREAM.LEADSECONDS  look-ahead along the camera's velocity
    bool hasStreamSettings() const {
        return streamLoadRadius >= 0 || streamEvictRadius >= 0 || streamLoadBudget >= 0 ||
               streamEvictBudget >= 0 || streamVerticalRadius >= 0 || streamLeadSeconds >= 0.0f;
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

    // THE JOLT WORLD'S OWN CEILINGS, which were hardcoded at PhysicsWorld.cpp's `system.Init(4096,
    // 0, 8192, 2048, ...)` and reachable from nothing. A project with more than 4096 bodies simply
    // could not be configured -- there was no flag, no key and no UI, only a recompile.
    int physMaxBodies         = -1;  // PHYSICS.MAXBODIES
    int physMaxBodyPairs      = -1;  // PHYSICS.MAXBODYPAIRS
    int physMaxContacts       = -1;  // PHYSICS.MAXCONTACTS
    int physTempAllocatorMb   = -1;  // PHYSICS.TEMPALLOCMB   per-frame contact scratch, MiB

    bool hasPhysicsSettings() const {
        return hasGravity || fixedStep > 0.0f ||
               physMaxBodies > 0 || physMaxBodyPairs > 0 || physMaxContacts > 0 ||
               physTempAllocatorMb > 0;
    }

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
