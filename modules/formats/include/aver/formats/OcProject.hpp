#pragma once
// .ocproject — manifest format (OC dialect: `#` comments, `KEY value`). See docs/PROJECTS.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// Maximum OCPROJECT version this build can read; files claiming more are rejected.
inline constexpr int kOcProjectVersion = 1;

struct ProjectDesc {
    int version = 1;                    // OCPROJECT <n>
    std::string name;                   // NAME
    std::string engineName;             // ENGINE <name> ...
    std::string engineMinVersion;       // ENGINE ... <minVersion>

    // CREATEDWITH: the engine version that last opened this project. Empty = current (adopted on open).
    // Differs from engineMinVersion, which is a floor; this records what touched it for migration.
    std::string createdWith;            // CREATEDWITH <version>
    std::string contentRoot = "Content"; // CONTENT, relative to the manifest
    std::string startMap;               // STARTMAP, relative to the content root
    // DRONE.GRAPH: project's drone graph (.ocgraph), relative to content root; may be empty.
    // A project key, not an assumed filename, so the engine does not require a particular layout.
    std::string droneGraph;             // DRONE.GRAPH, relative to the content root; may be empty

    // INPUT.SCHEME: project's default input bindings (.ocinput), relative to content root; may be empty.
    // A project key so the engine does not assume a particular path or layout.
    std::string inputScheme;            // INPUT.SCHEME, relative to the content root; may be empty
    // GAME.MODE: the project's default GameMode class name, used by every level without its own
    // GAMEMODE override. Empty: the engine's default (the flying drone pawn). Declaring a GameMode
    // class does NOT make it the default; this key does.
    std::string gameMode;
    std::string author;                 // AUTHOR (free text, rest of line)

    // RENDER.*, all -1 when the manifest did not state them.
    int giQuality       = -1;   // RENDER.GI            0=Off 1=Low 2=Medium 3=High 4=Epic
    int rayTracing      = -1;   // RENDER.RAYTRACING    same scale
    int pathTracing     = -1;   // RENDER.PATHTRACING   same scale
    // Clamped to [32, 512] by Voxi.cpp's setSettings; Epic derives 512.
    int voxelResolution = -1;   // RENDER.VOXELRES      grid edge, 32..512 (Epic derives 512)
    f32 giIntensity     = -1.0f;// RENDER.GIINTENSITY   indirect bounce multiplier
    f32 giMaxDistance   = -1.0f;// RENDER.GIDISTANCE    cone trace range, centimetres
    int rtShadowRays       = -1; // RENDER.RTSHADOWRAYS     sun occlusion rays/pixel, [1,32]
    int rtPixelsPerRayTile = -1; // RENDER.RTPIXELSPERRAY   shadow amortisation tile edge, [1,16]
    int rtShadowDenoise    = -1; // RENDER.RTSHADOWDENOISE  SPATIAL filter radius in pixels, [0,3]
    int rtRenderMode       = -1; // RENDER.RTRENDERMODE     0 = raster primary, 1 = ray-driven
    // RENDER.RDSTAGES: ray-driven primary's staged shape (see voxi::Settings::rayDrivenStages).
    // 0 = single pass, 1 = staged visibility -> shadow -> shade (D3D12 only), 2 = staged + half-rate GI.
    int rdStages           = -1; // RENDER.RDSTAGES         0 = single pass, 1 = staged,
                                  //                         2 = staged + half-rate GI (default)
    // RENDER.FOGOCCLUSION: fog in-scatter scaled by sky visibility, so enclosed air stops glowing.
    int fogOcclusion       = -1; // RENDER.FOGOCCLUSION     0/1 (engine default 1)
    int ptBounces          = -1; // RENDER.PTBOUNCES        PATH tracing: bounces after the first hit, [1,8]
    int ptMode             = -1; // RENDER.PTMODE           0 = ReSTIR path tracing, 1 = reference (converging)
    int layeredBsdf        = -1; // RENDER.LAYEREDBSDF      0=Off 1=Low 2=Medium 3=High 4=Epic
    int giCones            = -1; // RENDER.GICONES          diffuse gather cones, [1,16]
    // RENDER.GIMODE: 0 = voxel cone gather (default), 1 = ReSTIR GI. -1 keeps engine default.
    int giMode             = -1; // RENDER.GIMODE           diffuse GI algorithm, [0,1]
    // RENDER.DENOISER: 1 AMD FidelityFX over ReSTIR GI radiance, 2 NRD2 (docs/rendering/NRD2.md).
    int denoiser           = -1; // RENDER.DENOISER         0/1/2
    // RENDER.RESTIRVISIBILITY: ReSTIR GI visibility rays (see voxi::Settings::giRestirVisibility).
    int restirVisibility   = -1; // RENDER.RESTIRVISIBILITY  0=no ray 1=reconstructed 2=half 3=full 4=cached
    // RENDER.RESTIRHISTORY: ReSTIR GI reuse maxHistory weight. Measured: history 0 no overshoot,
    // 1 +8%, 8 +104% (Sponza, at rest and moving vs settled).
    int restirHistory      = -1; // RENDER.RESTIRHISTORY    ReSTIR GI temporal history weight, frames
    int refractionMode     = -1; // RENDER.REFRACTIONMODE   0=Off 1=Screen-space 2=Ray-traced
    f32 refractionStrength = -1.0f; // RENDER.REFRACTIONSTRENGTH  bend scale
    f32 refractionEdgeFade = -1.0f; // RENDER.REFRACTIONEDGEFADE  hides the screen-space miss at edges
    int lodSelect          = -1; // RENDER.LODSELECT        0/1; off draws every instance at LOD 0
    f32 lodThresholdPx     = -1.0f; // RENDER.LODTHRESHOLD  screen-space error, pixels
    int occlusionCull      = -1; // RENDER.OCCLUSIONCULL    0/1
    int depthPrepass       = -1; // RENDER.DEPTHPREPASS     0/1

    // RENDER.BACKEND: project's preferred RHI backend. Empty = engine default.
    // A preference, not a guarantee; the backend must be compiled in and create a device.
    // Peeked early in SandboxApp's main(), before setBackend (search RENDER.BACKEND there).
    std::string backend;        // RENDER.BACKEND  "d3d12" | "vulkan" | "d3d11"; empty = engine default

    // RENDER.FRAMEBUDGETMS: target frame time in milliseconds. <= 0 (default) leaves quality as authored.
    f32 frameBudgetMs = -1.0f;  // RENDER.FRAMEBUDGETMS  e.g. 16.7 for 60 Hz; <= 0 = off

    // RENDER.AVERSR: project-level AverSR default. -1 follows the Overall rung's default.
    // Not applied through ProjectRenderApply (module boundary: render.voxi must not include render.sr).
    int averSr = -1; // RENDER.AVERSR  -1 follow preset, 0 off, 1 quality, 2 balanced, 3 performance

    // RENDER.FRAMEINTERP: frame interpolation for Play / PIE and packaged runtime.
    // -1 / absent = off; CLI --frame-interp outranks it.
    int frameInterp = -1; // RENDER.FRAMEINTERP  0 off, 1 on (needs vsync and 1x anti-aliasing)

    // RENDER.TAA: temporal anti-aliasing (AverSR TAAU). -1 / absent = engine default (on);
    // CLI --taa / --no-taa outranks it.
    int taa = -1;         // RENDER.TAA  0 off, 1 on (needs MSAA 1; otherwise FSR 1 is used)
    // RENDER.NEURAA: NeuRAA edge anti-aliasing on ray-driven frames (docs/rendering/NEURAA_NRD.md).
    // -1 / absent = off; CLI --neuraa / --no-neuraa outranks it.
    int neuraa = -1;      // RENDER.NEURAA  0 off, 1 on

    // ---- UI SETTINGS THAT THE FILE COULD NOT HOLD (now fixed) ---------------------------------
    // These were live controls in Project Settings that applied immediately, then vanished on reopen
    // because captureRenderSettingsFromUi never read them and there was no key to write.
    int msaa            = -1;   // RENDER.MSAA           1/2/4/8 samples
    int meshShaders     = -1;   // RENDER.MESHSHADERS    0/1
    // giUpdateInterval was flag-only (--gi-update-interval) with no manifest key, causing asymmetry.
    int giUpdateInterval = -1;  // RENDER.GIUPDATEINTERVAL  frames between GI volume refreshes

    // The GI volume's placement. Two sliders in Project Settings > Global Illumination, now persisted.
    // hasGiVolume is a presence flag: volume is a cube (Vec3 center + scalar extent).
    bool hasGiVolume = false;   // RENDER.GIVOLUME <cx> <cy> <cz> <extent>
    f32  giCenter[3] = {0, 0, 0};
    f32  giExtent    = 0.0f;    // cm, half-edge of the cube

    // ---- POST-PROCESSING: exposure, bloom, tonemap (now persisted) ----------------------------
    // These were settable from CLI and the editor's Post panel but not persisted in the file,
    // so packaged games always used compiled-in defaults.
    // Floats use -1 as unstated (0 is valid: bloom 0 means no bloom pass).
    f32 postExposure     = -1.0f; // RENDER.EXPOSURE      linear pre-tonemap multiplier; < 0 = unstated
    f32 postBloom        = -1.0f; // RENDER.BLOOM         bloom intensity; 0 = no bloom pass at all
    int postAutoExposure = -1;    // RENDER.AUTOEXPOSURE  0/1; when on, EXPOSURE is compensation on it
    // RENDER.TONEMAP: curve selector (Narkowicz/Hill=0, Narkowicz+ACES=1, acesLuma=2).
    int postTonemap      = -1;    // RENDER.TONEMAP  0/1/2; -1 keeps PostSettings' own default (0)

    // True when the manifest stated at least one RENDER.* key.
    // Bug N9: giMode/denoiser were parsed but not applied because this check did not include them.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0 ||
               rtRenderMode >= 0 || ptBounces >= 0 || ptMode >= 0 || layeredBsdf >= 0 ||
               giCones >= 0 || giMode >= 0 || denoiser >= 0 || restirVisibility >= 0 ||
               restirHistory >= 0 || rdStages >= 0 || fogOcclusion >= 0 ||
               refractionMode >= 0 || refractionStrength >= 0.0f ||
               refractionEdgeFade >= 0.0f || lodSelect >= 0 || lodThresholdPx >= 0.0f ||
               occlusionCull >= 0 || depthPrepass >= 0 ||
               msaa >= 0 || meshShaders >= 0 || giUpdateInterval >= 0 || hasGiVolume ||
               postExposure >= 0.0f || postBloom >= 0.0f ||
               postAutoExposure >= 0 || postTonemap >= 0 ||
               !backend.empty() || frameBudgetMs > 0.0f || averSr >= 0 || taa >= 0 || neuraa >= 0;
    }

    // ---- WINDOW.* -- shipped game window presentation (now persisted) -------------------------
    // These were hardcoded (1280x720) with no flag, key, or UI, so shipped games could not be configured.
    std::string windowTitle;        // WINDOW.TITLE <prose>   empty = use NAME
    int windowWidth     = -1;       // WINDOW.SIZE <w> <h>
    int windowHeight    = -1;
    int windowResizable = -1;       // WINDOW.RESIZABLE 0/1
    int windowFullscreen = -1;      // WINDOW.FULLSCREEN 0/1  borderless-fullscreen intent
    bool hasWindowSettings() const {
        return !windowTitle.empty() || windowWidth > 0 || windowHeight > 0 ||
               windowResizable >= 0 || windowFullscreen >= 0;
    }

    // ---- IMPORT.* -- importer defaults (not actively used yet) --------------------------------
    // Keys parse and serialize, but no importer option struct is constructed from ProjectDesc.
    // Command-line options are still the way importers are configured.
    f32 importScale       = -1.0f;  // IMPORT.SCALE <f>       source unit -> cm (100 = metres)
    int importConvertAxes = -1;     // IMPORT.CONVERTAXES 0/1 Y-up right-handed -> Z-up left-handed
    int importGenNormals  = -1;     // IMPORT.GENNORMALS 0/1  synthesise missing normals
    int importGenMips     = -1;     // IMPORT.GENMIPS 0/1
    int importMaxTexture  = -1;     // IMPORT.MAXTEXTURE <px> downscale ceiling, 0 = no cap
    bool hasImportSettings() const {
        return importScale >= 0.0f || importConvertAxes >= 0 || importGenNormals >= 0 ||
               importGenMips >= 0 || importMaxTexture >= 0;
    }

    // ---- STREAM.* -- world streaming budgets (applied in GameStreaming::enable) ---------------
    // These fields were parsed and editable in Project Settings but never read by anything.
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

    // ---- PHYSICS.* -- world-wide simulation defaults ------------------------------------------
    // hasGravity is a presence flag: gravity points down, so components are legitimately negative.
    bool hasGravity = false;
    f32  gravity[3] = {0.0f, 0.0f, -980.0f};   // cm/s^2, engine axes; one g down +Z-up
    f32  fixedStep  = -1.0f;                   // PHYSICS.FIXEDSTEP  seconds, strictly positive
    // Jolt world ceilings from PhysicsWorld.cpp's system.Init; were hardcoded before.
    int physMaxBodies         = -1;  // PHYSICS.MAXBODIES
    int physMaxBodyPairs      = -1;  // PHYSICS.MAXBODYPAIRS
    int physMaxContacts       = -1;  // PHYSICS.MAXCONTACTS
    int physTempAllocatorMb   = -1;  // PHYSICS.TEMPALLOCMB   per-frame contact scratch, MiB

    bool hasPhysicsSettings() const {
        return hasGravity || fixedStep > 0.0f ||
               physMaxBodies > 0 || physMaxBodyPairs > 0 || physMaxContacts > 0 ||
               physTempAllocatorMb > 0;
    }

    // ---- AUDIO.* -- the mix a project starts at. Bus order matches audio_abi.h. ---------------
    // hasAudioMix is a presence flag: zero is valid (mute), so -1 cannot mean unstated.
    bool hasAudioMix = false;
    f32  masterVolume = 1.0f;                  // AUDIO.MASTER
    f32  busVolume[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // AUDIO.BUS <sfx> <music> <voice> <ui>

    std::string dir;                    // absolute directory the manifest lives in
    std::string manifestPath;           // absolute path to the .ocproject itself

    bool valid() const { return !name.empty() && !dir.empty(); }
    // Absolute content mount root. Empty when parsed from memory with no `dir`.
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
