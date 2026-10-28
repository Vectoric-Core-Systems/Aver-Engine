#pragma once
// .ocproject — manifest format (OC dialect: `#` comments, `KEY value`). See docs/PROJECTS.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::fmt {

// Maximum OCPROJECT version this build can read; files claiming more are rejected.
inline constexpr int kOcProjectVersion = 1;

// The names of the live physics solver settings a manifest may state as PHYSICS.<NAME UPPER-CASED>, in the
// physics ABI's table order (aver_phys_setting_name). Count is physicsSettingCount().
int physicsSettingCount();
const char* physicsSettingName(int index);   // nullptr when out of range

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
    // LOCALE.DEFAULT: the culture every missing translation falls back to (BCP-47-like tag). Empty = unstated = "en".
    std::string localeDefault;          // LOCALE.DEFAULT
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
    // RENDER.MESHEXTRA: meshes that carry the second vertex stream (vertex colour, authored tangent, UV2) draw through the pipelines that read it.
    // 0 keeps every mesh on the pipelines meshes without the stream use (the stream is bound and ignored). Default 1; no effect on a mesh without it.
    int meshExtra          = -1; // RENDER.MESHEXTRA        0/1 (engine default 1)
    // RENDER.MATERIALFX: materials with BaseMaterial3D features (the .ocmat FX record: UV transforms, cull / blend / depth modes, billboard, rim, ...) draw
    // through their own pipeline variants. 0 draws every material through the ordinary pipelines (the features are ignored). Default 1; no effect on
    // a material without an FX record.
    int materialFx         = -1; // RENDER.MATERIALFX       0/1 (engine default 1)
    // RENDER.LOCALRASTER: omni/spot lights from the raster path (no ray tracing, Vulkan). 0 auto (where the ray-traced lights cannot serve),
    // 1 on (also in raster-primary ray-traced frames), 2 off. RENDER.LOCALSHADOWS: shadow maps for them, 0/1 (default 1).
    int localRaster        = -1; // RENDER.LOCALRASTER      0/1/2 (engine default 0)
    int localShadows       = -1; // RENDER.LOCALSHADOWS     0/1 (engine default 1)
    // G4 R8: RENDER.VOLFOG switches Voxi's volumetric fog (light shafts; the home of Godot FogVolumes) on or off; RENDER.VOLFOGDENSITY is its uniform
    // medium in extinction per CENTIMETRE (engine default 0.00002). Off by default; the Fabric converter writes both for a Godot project with volumetric fog.
    int volFog             = -1;    // RENDER.VOLFOG         0/1 (engine default 0)
    f32 volFogDensity      = -1.0f; // RENDER.VOLFOGDENSITY  per cm, [0, 0.01]
    // G4 R5, sub-viewports (render-to-texture views): the cost guard. RENDER.SUBVIEWPORTS = how many views may be live at once (0 turns the feature
    // off, default 4, at most 16); RENDER.SUBVIEWRENDERS = how many are drawn per frame (default 2); RENDER.SUBVIEWPIXELS = the summed pixel cap in
    // megapixels (default 8). A project with no view in any level pays nothing whatever these say.
    int subViewports       = -1; // RENDER.SUBVIEWPORTS     [0,16] (engine default 4)
    int subViewRenders     = -1; // RENDER.SUBVIEWRENDERS   [1,16] (engine default 2)
    f32 subViewPixels      = -1.0f; // RENDER.SUBVIEWPIXELS megapixels (engine default 8)
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
    // RENDER.MANYLIGHTS: stochastic many-light direct lighting from a light tree (docs/rendering/MANY_LIGHTS.md).
    // 0 = off (the grid's capped tail), 1 = auto (engages above voxi.manyLightsMin lights), 2 = on. -1 / absent = auto.
    // The console's 3 (exhaustive ground truth) is never stored. Needs NRD2.
    int manyLights = -1;  // RENDER.MANYLIGHTS  0/1/2
    // RENDER.PARTICLEGPU: where particle emitters may simulate (docs/PARTICLES.md). 0 = everything on the CPU, 1 = auto (emitters of
    // 2048+ particles on the GPU when the device supports compute and indirect draw), 2 = on (every emitter that can). -1 / absent
    // = auto. CLI --particles-cpu / --particles-gpu outrank it.
    int particleGpu = -1; // RENDER.PARTICLEGPU  0/1/2

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
    // RENDER.TONEMAP: curve selector (Narkowicz/Hill=0, Narkowicz+ACES=1, acesLuma=2; 3..7 Godot's linear, Reinhard,
    // filmic, ACES, AgX: rhi/Tonemap.hpp).
    int postTonemap      = -1;    // RENDER.TONEMAP  0..7; -1 keeps PostSettings' own default
    // Godot-style tonemap white point and display-space adjustments (all identity when unstated; < 0 = unstated).
    f32 postTonemapWhite    = -1.0f;   // RENDER.TONEMAPWHITE
    f32 postAdjustBrightness = -1.0f;  // RENDER.ADJUSTBRIGHTNESS
    f32 postAdjustContrast   = -1.0f;  // RENDER.ADJUSTCONTRAST
    f32 postAdjustSaturation = -1.0f;  // RENDER.ADJUSTSATURATION

    // True when the manifest stated at least one RENDER.* key.
    // Bug N9: giMode/denoiser were parsed but not applied because this check did not include them.
    bool hasRenderSettings() const {
        return giQuality >= 0 || rayTracing >= 0 || pathTracing >= 0 ||
               voxelResolution > 0 || giIntensity >= 0.0f || giMaxDistance >= 0.0f ||
               rtShadowRays >= 0 || rtPixelsPerRayTile >= 0 || rtShadowDenoise >= 0 ||
               rtRenderMode >= 0 || ptBounces >= 0 || ptMode >= 0 || layeredBsdf >= 0 ||
               giCones >= 0 || giMode >= 0 || denoiser >= 0 || restirVisibility >= 0 ||
               restirHistory >= 0 || rdStages >= 0 || fogOcclusion >= 0 || meshExtra >= 0 || materialFx >= 0 || localRaster >= 0 || localShadows >= 0 ||
               volFog >= 0 || volFogDensity >= 0.0f ||
               subViewports >= 0 || subViewRenders >= 0 || subViewPixels >= 0.0f ||
               refractionMode >= 0 || refractionStrength >= 0.0f ||
               refractionEdgeFade >= 0.0f || lodSelect >= 0 || lodThresholdPx >= 0.0f ||
               occlusionCull >= 0 || depthPrepass >= 0 ||
               msaa >= 0 || meshShaders >= 0 || giUpdateInterval >= 0 || hasGiVolume ||
               postExposure >= 0.0f || postBloom >= 0.0f ||
               postAutoExposure >= 0 || postTonemap >= 0 ||
               postTonemapWhite >= 0.0f || postAdjustBrightness >= 0.0f || postAdjustContrast >= 0.0f || postAdjustSaturation >= 0.0f ||
               !backend.empty() || frameBudgetMs > 0.0f || averSr >= 0 || taa >= 0 || neuraa >= 0 || manyLights >= 0 || particleGpu >= 0;
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

    // ---- WORLD.* -- the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.2, 2.7) ----------------
    // -1 = unstated; the host then uses the engine default (on, 102400 cm, no script shift requests).
    int worldFloatingOrigin    = -1;  // WORLD.FLOATINGORIGIN     0/1; 0 keeps the origin at (0,0): the old +-10 km single frame
    int worldRebaseMarginCm    = -1;  // WORLD.REBASEMARGIN       cm past the region half-width before a shift; the service clamps to [0, 229376]
    int worldAllowShiftRequest = -1;  // WORLD.ALLOWSHIFTREQUEST  0/1; lets aver_fw_origin_request_shift (console, tests) move the origin
    bool hasWorldSettings() const {
        return worldFloatingOrigin >= 0 || worldRebaseMarginCm >= 0 || worldAllowShiftRequest >= 0;
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
    // PHYSICS.THREADS: physics worker threads. -1 = automatic (cores - 2), 0 = single-threaded, N = N workers;
    // -2 = unstated (-1 is a legal value, so it cannot be the sentinel).
    int physThreads           = -2;
    // PHYSICS.SHAPECACHE: 1 = keep mesh and heightfield collision shapes' BVHs on disk (Saved/DerivedDataCache/Physics),
    // 0 = do not; -1 = unstated (the default, which is off until the cache has been measured on the real levels).
    int physShapeCache        = -1;
    // PHYSICS.INTERPOLATION: 1 = draw simulated bodies between the last two fixed steps (Godot's physics
    // interpolation), 0 = at the last step; -1 = unstated (off).
    int physInterpolation     = -1;
    // PHYSICS.COMBINE: how two materials combine in a contact. 0 = jolt (friction sqrt(f1 f2), restitution max), 1 = godot (friction
    // min or max when rough, restitution a clamped sum); -1 = unstated (jolt).
    int physCombine           = -1;
    // PHYSICS.<NAME> for the live solver settings of physics_world_abi.h (PHYSICS.VELOCITYSTEPS,
    // PHYSICS.SLEEPTIME ...): only the ones the file states, in file order, the name in the setting table's own
    // camelCase spelling ("velocitySteps") and the value as written (the physics ABI clamps it). Formats may not
    // depend on Physics, so the list of names is mirrored by physicsSettingName(); a test pins the mirror.
    std::vector<std::pair<std::string, f32>> physicsSettings;

    bool hasPhysicsSettings() const {
        return hasGravity || fixedStep > 0.0f ||
               physMaxBodies > 0 || physMaxBodyPairs > 0 || physMaxContacts > 0 ||
               physTempAllocatorMb > 0 || physThreads > -2 || physShapeCache >= 0 || physCombine >= 0 || !physicsSettings.empty();
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
    std::string contentDir() const { return dir.empty() ? std::string() : dir + AVER_PATH_SEP + contentRoot; }
    std::string scriptsDir() const { return dir.empty() ? std::string() : contentDir() + AVER_PATH_SEP "Scripts"; }
    // Absolute build output directory, beside Content rather than inside it.
    std::string binariesDir() const { return dir.empty() ? std::string() : dir + AVER_PATH_SEP "Binaries"; }
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
