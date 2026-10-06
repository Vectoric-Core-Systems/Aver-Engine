#include "aver/game/GameApp.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/GameUiInput.hpp"
#include "aver/game/LevelDecals.hpp"

#include <filesystem>

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"
// GPU-timing tree formatter
#include "aver/rhi/GpuTimingFormat.hpp"

// OK to define twice: separate binaries don't collide on symbols (unlike Image.cpp's STB_IMAGE_IMPLEMENTATION).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_WITH_UI_ABI
#  include "aver/ui/ui_abi.h"
// aver::ui::parseOcfont, for loadGameUiFont below (same .ocfont grammar SandboxApp.cpp parses).
#  include "aver/ui/UiFont.hpp"
// Decode font atlas PNG
#  include "aver/platform/Image.hpp"
#endif
#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/anim/ControlRig.hpp"
#  include "aver/save/SaveWorld.hpp"
#  include "aver/formats/OcSave.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseAgent.hpp"
#  include "aver/synapse/SynapsePerception.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
#  include "aver/particles/ParticleSystem.hpp"
// Needed for spawnParticleTestContent
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/core/Hash.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
// Compare editor and game render output (header-only)
#include "aver/world/SceneCensus.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/Voxi.hpp"
// Two-phase render settings apply from manifest
#  include "aver/voxi/ProjectRenderApply.hpp"
// AverSR scalability settings and resolution
#  include "aver/voxi/Scalability.hpp"
#endif
#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// Borrowed HLSL for particleGiPrepare/particleGiBind
#  include "aver/voxi/VoxiGiShaders.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"
#  include "aver/framework/framework_hooks.h"
#endif
// GamePawn.hpp (game::placePossessedPawn): placePawnAtSpawn's pawn lookup and placement.
#include "aver/game/GamePawn.hpp"
#if AVER_MODULE_VOXI
// For loading instanced foliage
#  include "aver/game/GameFoliage.hpp"
#endif
#if AVER_MODULE_PBR
// For texture cache setup
#  include "aver/assets/TextureUpload.hpp"
#endif

// Gated on _WIN32 alone, not nested in a module guard (unlike SandboxApp.hpp's copy of this comment):
// onUpdate's capture-decision check below needs it regardless of which modules are linked.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>

// These module macros are PUBLIC compile defs reaching this TU only via Aver.Runtime.Game's link
// interface. Defaulting to 0 lets a missing link interface print as "off" instead of failing to
// compile or silently compiling to nothing.
#ifndef AVER_MODULE_PBR
#  define AVER_MODULE_PBR 0
#endif
#ifndef AVER_MODULE_SCENE
#  define AVER_MODULE_SCENE 0
#endif
#ifndef AVER_MODULE_VOXI
#  define AVER_MODULE_VOXI 0
#endif
#ifndef AVER_MODULE_PHYSICS
#  define AVER_MODULE_PHYSICS 0
#endif
#ifndef AVER_MODULE_FRAMEWORK
#  define AVER_MODULE_FRAMEWORK 0
#endif
#ifndef AVER_MODULE_SCRIPTING
#  define AVER_MODULE_SCRIPTING 0
#endif
#ifndef AVER_MODULE_TRIFACTOR
#  define AVER_MODULE_TRIFACTOR 0
#endif
#ifndef AVER_MODULE_PARTICLES
#  define AVER_MODULE_PARTICLES 0
#endif

namespace aver::game {

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
namespace {

// ---- Save seams a composition root installs -----------------------------------------------
// Aver.Save takes function pointers rather than linking Aver.Framework; this is where they meet --
// same shape as the animation notify sink and the animation-curve provider above.

// Spawns without BeginPlay -- the whole reason aver_fw_dispatch_begin_play exists: aver_fw_spawn runs
// bind -> build_models -> beginPlay inline, so an actor spawned then patched would begin play
// against class defaults. Preview, patch, then begin.
aver::scene::Entity saveSpawnClass(const char* className, void*) {
    const i32 c = aver_fw_class_find(className);
    if (c == 0) return aver::scene::kInvalidEntity;
    const i32 e = aver_fw_spawn_preview(c, className, nullptr, nullptr, nullptr);
    return e == 0 ? aver::scene::kInvalidEntity : static_cast<aver::scene::Entity>(e);
}

const char* saveClassOf(aver::scene::Entity e, void*) {
    const i32 c = aver_fw_class_of(static_cast<i32>(e));
    return c == 0 ? nullptr : aver_fw_class_name(c);
}

void saveBeginPlay(aver::scene::Entity e, void*) {
    aver_fw_dispatch_begin_play(static_cast<i32>(e), AVER_FW_BEGIN_SPAWN);
}

// Through the framework, so OnEndPlay runs and the managed instance is released. World::destroy
// would take the entity out from under a live C# object.
void saveDestroyActor(aver::scene::Entity e, void*) { aver_fw_destroy(static_cast<i32>(e)); }

// Graph-local variables: thin forwards to the aver_fw_graph_var_* relay (framework_abi.h), field
// for field with SandboxApp.cpp's own saveHost(), since both build a save::Host from the same
// framework ABI.
i32 saveGraphVarCount(aver::scene::Entity e, void*) {
    return aver_fw_graph_var_count(static_cast<i32>(e));
}
i32 saveGraphVarAt(aver::scene::Entity e, i32 index, char* nameBuf, i32 nameBufLen,
                   u32* outKind, f32* outF, i32* outI, void*) {
    return aver_fw_graph_var_at(static_cast<i32>(e), index, nameBuf, nameBufLen,
                                reinterpret_cast<int32_t*>(outKind), outF, outI);
}
i32 saveGraphVarSet(aver::scene::Entity e, const char* name, u32 kind, f32 f, i32 i, void*) {
    return aver_fw_graph_var_set(static_cast<i32>(e), name, static_cast<int32_t>(kind), f, i);
}

aver::save::Host saveHost() {
    aver::save::Host h;
    h.spawnClass    = &saveSpawnClass;
    h.classOf       = &saveClassOf;
    h.beginPlay     = &saveBeginPlay;
    h.destroyActor  = &saveDestroyActor;
    h.graphVarCount = &saveGraphVarCount;
    h.graphVarAt    = &saveGraphVarAt;
    h.graphVarSet   = &saveGraphVarSet;
    return h;
}

i32 saveWriteProvider(const char* path, void*) {
    aver::save::CaptureOptions co;
    co.host = saveHost();
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::save::capture(aver::scene::World::instance(), snap, co, &why)) {
        AVER_ERROR("[Save] capture failed: {}", why);
        return 0;
    }
    if (!aver::fmt::saveOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] write failed: {}", why);
        return 0;
    }
    AVER_INFO("[Save] wrote {} ({} entities)", path, snap.entities.size());
    return 1;
}

i32 saveLoadProvider(const char* path, void*) {
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::fmt::loadOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] load failed: {}", why);
        return 0;
    }
    aver::save::RestoreOptions ro;
    ro.host = saveHost();
    if (!aver::save::restore(snap, aver::scene::World::instance(), ro, &why)) {
        AVER_ERROR("[Save] restore failed: {}", why);
        return 0;
    }
    return 1;
}

} // namespace
#endif


namespace {

// Reads the value that follows a flag, or returns the fallback. Bounds-checked so a trailing flag
// with no value is ignored rather than reading past argv.
const char* valueAfter(int argc, char** argv, int i, const char* fallback) {
    return (i + 1 < argc) ? argv[i + 1] : fallback;
}

u32 parseU32(const char* s, u32 fallback) {
    if (!s || !*s) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || v == 0) return fallback;
    return static_cast<u32>(v);
}

// M7 (C-7): one shared [Stats] video-memory line for both the periodic and one-shot end-of-run dumps
// in onUpdate, so they can't drift apart. Mirrors SandboxApp.cpp's gpuTimingCheck [GPU] line: same MB
// rounding (bytes/1048576) as the RHI backends' own init-time lines. `supported` false (D3D11, no
// VK_EXT_memory_budget, mocks) prints a distinct sentence rather than a row of zeros that could read
// as "nothing in use".
void logStatsVideoMemory(rhi::IDevice& dev) {
    const rhi::VideoMemoryInfo vm = dev.videoMemory();
    if (vm.supported) {
        AVER_INFO("[Stats] video memory: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                  vm.localUsageBytes / 1048576, vm.localBudgetBytes / 1048576,
                  vm.nonLocalUsageBytes / 1048576, vm.nonLocalBudgetBytes / 1048576);
    } else {
        AVER_INFO("[Stats] video memory: not reported by this backend");
    }
}

#if AVER_MODULE_SYNAPSE_SCENE
// A level's .ocnav sits beside it with the same stem (Arena.ocworld -> Arena.ocnav). Deliberately a
// separate copy of NavBakeCommand.cpp's navPathForLevel: that file is editor-only, and this
// composition root must not depend on it for a five-line string derivation.
std::string navPathForLevel(const std::string& levelPath) {
    if (levelPath.empty()) return {};
    const usize slash = levelPath.find_last_of("/\\");
    const usize dot = levelPath.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExt ? levelPath.substr(0, dot) : levelPath) + ".ocnav";
}
#endif

// Logs one line per file the engine opens. Installed only under --trace-opens. The "[open]" prefix
// is machine-readable: verify-game.ps1 greps it and asserts every path is under the package root --
// a packaged game falling back to a dev-tree asset would run fine on the build machine and fail
// everywhere else otherwise silently.
void onFileOpen(const char* path, void*) {
    AVER_INFO("[open] {}", path ? path : "(null)");
}

// The one event sink the game installs. Everything the window produces lands in InputState.
void onWindowEvent(void* user, const Event& e) {
    static_cast<InputState*>(user)->onEvent(e);
}

// ASCII case-insensitive equality, for the record scanner below and the one token it reads.
bool equalsAsciiCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Lightweight scan for top-level records in .ocgraph text. Sets outValue to the token after the key.
bool ocgraphRecord(const std::string& text, std::string_view wanted, std::string* outValue) {
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;

        const usize start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) continue;
        line.remove_prefix(start);
        if (line.empty() || line[0] == '#') continue;

        const usize end = line.find_first_of(" \t\r");
        if (!equalsAsciiCI(end == std::string_view::npos ? line : line.substr(0, end), wanted)) continue;

        if (outValue) {
            outValue->clear();
            std::string_view rest = end == std::string_view::npos ? std::string_view() : line.substr(end);
            const usize vs = rest.find_first_not_of(" \t\r");
            if (vs != std::string_view::npos) {
                rest.remove_prefix(vs);
                const usize ve = rest.find_first_of(" \t\r");
                *outValue = std::string(ve == std::string_view::npos ? rest : rest.substr(0, ve));
            }
        }
        return true;
    }
    return false;
}

// True when an .ocgraph declares itself a spawnable class via a top-level CLASS record (Graph.
// ClassName). Lets discoverProjectGraphs skip a file already driven by the class-registration/spawn
// path, so it is never also ticked against a synthetic entity alongside its real spawned instances.
bool ocgraphDeclaresClass(const std::string& text) { return ocgraphRecord(text, "CLASS", nullptr); }

// Check if graph's DOMAIN is not gameplay
bool ocgraphIsForeign(const std::string& text) {
    std::string domain;
    if (!ocgraphRecord(text, "DOMAIN", &domain) || domain.empty()) return false;
    return !equalsAsciiCI(domain, "gameplay");
}

// Check for .ocproject file extension (case-insensitive)
bool isOcproject(const char* p) {
    const usize n = std::strlen(p);
    if (n < 11) return false;
    const char* ext = p + n - 10;
    static const char* kExt = ".ocproject";
    for (int i = 0; i < 10; ++i) {
        char a = ext[i], b = kExt[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Check for .ocworld or .ocmap file extension (case-insensitive)
bool isLevelFile(const char* p) {
    const std::string_view s(p);
    const auto endsWithCI = [&](std::string_view ext) {
        return s.size() > ext.size() && equalsAsciiCI(s.substr(s.size() - ext.size()), ext);
    };
    return endsWithCI(".ocworld") || endsWithCI(".ocmap");
}

// Find owning .ocproject by walking up max 8 directories
std::string ownerProjectOf(const std::string& mapPath) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(mapPath).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up) {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (!it->is_directory(ec) && isOcproject(it->path().string().c_str()))
                return it->path().string();
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

#if AVER_MODULE_VOXI
// AverSR level names for logging
const char* averSrLevelName(u32 level) {
    switch (level) {
        case voxi::ladder::kAverSrOff:         return "Off";
        case voxi::ladder::kAverSrQuality:     return "Quality";
        case voxi::ladder::kAverSrBalanced:    return "Balanced";
        case voxi::ladder::kAverSrPerformance: return "Performance";
        default:                               return "Off";
    }
}

// AverSR source names for logging
const char* averSrSourceName(voxi::AverSrSource source) {
    switch (source) {
        case voxi::AverSrSource::Auto:      return "Auto";
        case voxi::AverSrSource::Manifest:  return "project default";
        case voxi::AverSrSource::User:      return "your Display preference";
        case voxi::AverSrSource::Cli:       return "--aversr";
        case voxi::AverSrSource::ForcedOff: return "forced Off after a failed launch";
    }
    return "Auto";
}
#endif

} // namespace

GameConfig parseArgs(int argc, char** argv) {
    GameConfig c;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--frames") == 0)      { c.maxFrames = parseU32(valueAfter(argc, argv, i, nullptr), 0); ++i; }
        else if (std::strcmp(a, "--width") == 0)       { c.width  = parseU32(valueAfter(argc, argv, i, nullptr), c.width);  ++i; }
        else if (std::strcmp(a, "--height") == 0)      { c.height = parseU32(valueAfter(argc, argv, i, nullptr), c.height); ++i; }
        else if (std::strcmp(a, "--backend") == 0)     { c.backend = valueAfter(argc, argv, i, ""); ++i; }
        else if (std::strcmp(a, "--title") == 0)       { c.title   = valueAfter(argc, argv, i, c.title.c_str()); ++i; }
        else if (std::strcmp(a, "--headless") == 0)    { c.headless = true; }
        else if (std::strcmp(a, "--warp") == 0)        { c.useWarp = true; }
        else if (std::strcmp(a, "--debug-layer") == 0) { c.debugLayer = true; }
        else if (std::strcmp(a, "--project") == 0)     { c.projectPath = valueAfter(argc, argv, i, ""); ++i; }
        else if (std::strcmp(a, "--input-echo") == 0)  { c.inputEcho = true; }
        else if (std::strcmp(a, "--trace-opens") == 0) { c.traceOpens = true; }
        // Prints scene contents for verification (both hosts match)
        else if (std::strcmp(a, "--scene-census") == 0) { c.sceneCensus = true; }
        // Stats interval, optional. Consumed only if it parses as a number.
        else if (std::strcmp(a, "--stats") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            const f32 secs = v ? static_cast<f32>(std::atof(v)) : 0.0f;
            if (secs > 0.0f) { c.statsIntervalSec = secs; ++i; }
            else             { c.statsIntervalSec = 5.0f; }
        }
        else if (std::strcmp(a, "--pcg-volume-test") == 0) { c.pcgVolumeTest = true; }
        // Chunk streaming with optional frame count
        else if (std::strcmp(a, "--chunk-stream") == 0) {
            c.chunkStreamAutoFrames = (i + 1 < argc && argv[i + 1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (std::strcmp(a, "--no-chunk-stream") == 0) { c.chunkStreamAutoFrames = 0; }
        else if (std::strcmp(a, "--no-particle-gi") == 0)  { c.noParticleGi = true; }
        else if (std::strcmp(a, "--particle-test") == 0)   { c.particleTest = true; }
        else if (std::strcmp(a, "--screenshot") == 0)      { c.screenshotPath = valueAfter(argc, argv, i, ""); ++i; }
        // Disable VSync for measurement parity
        else if (std::strcmp(a, "--no-vsync") == 0) { c.vsyncOff = true; }
        // See GameConfig::noMouseCapture's own comment.
        else if (std::strcmp(a, "--no-mouse-capture") == 0) { c.noMouseCapture = true; }
        // Camera wobble: DEG and PERIOD both required
        else if (std::strcmp(a, "--cam-wobble") == 0 && i + 2 < argc) {
            c.camWobbleDeg = static_cast<f32>(std::atof(argv[i + 1]));
            const int period = std::atoi(argv[i + 2]);
            c.camWobblePeriod = period > 0 ? static_cast<u32>(period) : 0u;
            i += 2;
        }
        // AverSR quality level selection
        else if (std::strcmp(a, "--aversr") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if      (v && equalsAsciiCI(v, "off"))         { c.averSrArg = 0;  ++i; }
            else if (v && equalsAsciiCI(v, "quality"))     { c.averSrArg = 1;  ++i; }
            else if (v && equalsAsciiCI(v, "balanced"))    { c.averSrArg = 2;  ++i; }
            else if (v && equalsAsciiCI(v, "performance")) { c.averSrArg = 3;  ++i; }
            else if (v && equalsAsciiCI(v, "auto"))        { c.averSrArg = -1; ++i; }
        }
        // Override frame interpolation setting
        else if (std::strcmp(a, "--frame-interp") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if (v) { c.frameInterpArg = std::atoi(v) != 0 ? 1 : 0; ++i; }
        }
        else if (std::strcmp(a, "--frame-interp-trajectory") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if (v) {
                c.frameInterpTrajectory = equalsAsciiCI(v, "quadratic") ? 1 : equalsAsciiCI(v, "neural") ? 2 : 0;
                ++i;
            }
        }
        // Bare .ocproject or .ocworld path is recognized
        else if (isOcproject(a))                       { c.projectPath = a; }
        // Opens level instead of start map
        else if (isLevelFile(a))                       { c.levelPath = a; }
        // Anything else is deliberately ignored: see the header.
    }
    // Level path implies its project (explicit --project wins)
    if (!c.levelPath.empty() && c.projectPath.empty()) {
        c.projectPath = ownerProjectOf(c.levelPath);
        if (c.projectPath.empty())
            AVER_WARN("[Game] '{}' is not inside a project (no .ocproject above it); its "
                      "placements will not resolve", c.levelPath);
    }
    return c;
}

GameApp::GameApp(GameConfig cfg) : cfg_(std::move(cfg)) {
#if AVER_MODULE_SCENE
    chunkStreamFramesLeft_ = cfg_.chunkStreamAutoFrames;
#endif
}

BootConfig GameApp::config() const {
    BootConfig b;
    b.windowTitle      = cfg_.title.c_str();
    b.windowWidth      = cfg_.width;
    b.windowHeight     = cfg_.height;
    b.maxFrames        = cfg_.maxFrames;
    b.headless         = cfg_.headless;
    b.useWarp          = cfg_.useWarp;
    b.enableDebugLayer = cfg_.debugLayer;
    b.backend          = cfg_.backend.empty() ? nullptr : cfg_.backend.c_str();

    // Read WINDOW.* from manifest (must be done before window is created).
    // Command line wins: --width/--height/--title override manifest.
    {
        std::string manifest = cfg_.projectPath;
        if (manifest.empty()) {
            const std::string beside = executableDir() + "\\Game.ocproject";
            std::error_code ec;
            if (std::filesystem::exists(beside, ec)) manifest = beside;
        }
        if (!manifest.empty()) {
            fmt::ProjectDesc d;
            std::string why;
            if (fmt::loadOcproject(manifest, d, &why)) {
                if (cfg_.title.empty() || cfg_.title == GameConfig::kDefaultTitle) {
                    // Use WINDOW.TITLE, else NAME
                    windowTitleOwned_ = !d.windowTitle.empty() ? d.windowTitle : d.name;
                    if (!windowTitleOwned_.empty()) b.windowTitle = windowTitleOwned_.c_str();
                }
                if (cfg_.width  == GameConfig::kDefaultWidth  && d.windowWidth  > 0) b.windowWidth  = static_cast<u32>(d.windowWidth);
                if (cfg_.height == GameConfig::kDefaultHeight && d.windowHeight > 0) b.windowHeight = static_cast<u32>(d.windowHeight);
                // Fullscreen is a window-creation-time decision. No command-line override exists.
                if (d.windowFullscreen >= 0) b.fullscreen = d.windowFullscreen != 0;
                // Resizable window setting from manifest
                if (d.windowResizable >= 0) b.resizable = d.windowResizable != 0;
            }
        }
    }
    return b;
}

void GameApp::initPhysics() {
#if AVER_MODULE_PHYSICS
    // No implicit ground; level supplies its own collision
    game::startPhysics("Game");
#endif
}

void GameApp::tickGameplay(f32 dt) {
#if AVER_MODULE_FRAMEWORK
    // Only tick when PLAYING, not PAUSED
    if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;

#if AVER_MODULE_PHYSICS
#if AVER_MODULE_SCENE
    // Vehicle logic: input -> step -> output. LOD based on camera distance.
    vehicles_.prePhysics(dt, camPos_);
#endif
    // Sum step count; a frame can run 0-8 fixed steps
    physSteps_ += static_cast<u64>(game::tickGameplayGroups(dt));
#if AVER_MODULE_SCENE
    vehicles_.postPhysics(scene::World::instance());
#endif
#else
    game::tickGameplayGroups(dt);
#endif
#else
    (void)dt;
#endif
}

#if AVER_MODULE_SCENE
bool GameApp::sequenceEmissiveScale(scene::Entity e, f32 outRgb[3], void* user) {
    return static_cast<const GameApp*>(user)->sequencePlayer_.emissiveScale(e, outRgb);
}
#endif

void GameApp::drivePlayCamera() {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    firstPersonPawn_ = game::drivePlayCamera(camPos_, yaw_, pitch_);
#endif
}

// Gives the mouse to the game or hands it back, through game::MouseCapture (MouseCapture.hpp).
void GameApp::setMouseCaptured(bool on) {
    mouse_.set(on, window_, "Game");
}

// Measures one frame of captured mouse movement and re-centres for the next, through
// game::MouseCapture.
void GameApp::pollCapturedMouse() {
    mouse_.poll(window_);
}

void GameApp::attachPcgTest(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;
    if (!pcgVolume_.init(*dev)) {
        AVER_ERROR("[PCG] the volume shader would not compile - the HLSL and the C++ mirror cannot "
                   "be compared, so this run proves nothing");
        return;
    }
    dev->addRenderFeature(&pcgVolume_);
    pcgAttached_ = true;

    // Match PcgMirrorTest spec for C++/F#/HLSL parity
    pcgSpec_.resX = pcgSpec_.resY = pcgSpec_.resZ = 32;
    pcgSpec_.seed = 20260802;
    pcgSpec_.layerCount = 2;
    pcgSpec_.layers[0] = { 4.0f,  1.0f, 4, 2.0f, 0.5f, 7919 };
    pcgSpec_.layers[1] = { 11.0f, 0.5f, 3, 2.0f, 0.5f, 104729 };
    pcgSpec_.coverageFloor = 0.35f;
    pcgSpec_.coverageBias  = 1.7f;
    if (!pcgVolume_.request(pcgSpec_)) AVER_ERROR("[PCG] could not queue the volume build");
}

bool GameApp::checkPcgVolume() {
    if (!pcgAttached_ || pcgChecked_ || !pcgVolume_.done()) return false;
    pcgChecked_ = true;

    const usize n = usize(pcgSpec_.resX) * pcgSpec_.resY * pcgSpec_.resZ;
    std::vector<f32> gpu(n, 0.0f);
    if (!pcgVolume_.read(gpu.data(), n)) { AVER_ERROR("[PCG] could not read the volume back"); return true; }

    // Compare all voxels against CPU reference. Tolerance 1e-5 (above contraction, below logic errors).
    constexpr f32 kTolerance = 1e-5f;
    usize differing = 0;
    usize exact = 0;
    f32 worst = 0.0f;
    usize worstAt = 0;
    for (u32 z = 0; z < pcgSpec_.resZ; ++z)
        for (u32 y = 0; y < pcgSpec_.resY; ++y)
            for (u32 x = 0; x < pcgSpec_.resX; ++x) {
                const usize i = usize(z) * pcgSpec_.resY * pcgSpec_.resX + usize(y) * pcgSpec_.resX + x;
                const f32 cpu = pcg::sampleDensity(pcgSpec_, x, y, z);
                const f32 d = std::fabs(cpu - gpu[i]);
                if (d == 0.0f) ++exact;
                if (d > kTolerance) { ++differing; }
                if (d > worst) { worst = d; worstAt = i; }
            }

    if (differing == 0) {
        AVER_INFO("[PCG] volume mirror PASS: {} of {} voxels agree with the C++ reference to within "
                  "{}, of which {} are bit-identical; worst difference {} at index {}",
                  n, n, kTolerance, exact, worst, worstAt);
    } else {
        AVER_ERROR("[PCG] volume mirror FAIL: {} of {} voxels differ by more than {}, worst {} at "
                   "index {} - that is far above float contraction and means the HLSL and the C++ "
                   "reference have genuinely diverged", differing, n, kTolerance, worst, worstAt);
    }
    return true;
}

void GameApp::attachSkinning(Engine& e) {
#if AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    auto scene = std::make_unique<render::SkinnedScene>();
    // HLSL compiled at runtime; null on fail (entities draw at rest pose)
    if (!scene->init(*dev)) {
        AVER_WARN("[Game] skinning unavailable; skinned entities will draw at rest");
        return;
    }
    // Use same resolvers as the draw walk
    scene->setResolvers(&GameContent::resolveAnimAsset, &GameContent::resolveSceneMesh, &content_);
    dev->addRenderFeature(scene.get());
    skinnedScene_ = std::move(scene);
    AVER_INFO("[Game] skinning attached");
#else
    (void)e;
#endif
}

void GameApp::attachVoxi(Engine& e) {
#if AVER_MODULE_VOXI
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // Hand the GPU's real capabilities to Voxi so its settings reflect this hardware.
    const rhi::DeviceCaps caps = dev->caps();
    voxi::DeviceInfo di;
    di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
    di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
    di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
    di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
    di.dxcAvailable = caps.dxcAvailable;
    // Denoiser needs G-buffer (D3D12 only)
    di.denoiserSupported = dev->backend() == rhi::Backend::D3D12;
    voxi::Renderer::get().setDeviceInfo(di);

    // Seed render settings before init() to set voxel volume size and layered BSDF
    int manifestMsaa = -1;
    {
        std::string manifestPath = cfg_.projectPath;
        if (manifestPath.empty()) {
            const std::string beside = executableDir() + "\\Game.ocproject";
            if (fileExists(beside)) manifestPath = beside;
        }
        if (!manifestPath.empty()) {
            fmt::ProjectDesc seed;
            std::string seedErr;
            if (fmt::loadOcproject(manifestPath, seed, &seedErr) && seed.hasRenderSettings()) {
                manifestMsaa = seed.msaa;
                voxi::Renderer& vx = voxi::Renderer::get();
                voxi::Settings seeded = vx.settings();
                voxi::applyManifestTwoPhase(seed, seeded, [&]() {
                    vx.setSettings(seeded);
                    seeded = vx.settings();
                });
            }
        }
    }

    voxi::Settings s = voxi::Renderer::get().settings();
    // Device MSAA fallback only when manifest unstated
    if (manifestMsaa < 0) s.msaa = static_cast<voxi::Msaa>(dev->sampleCount());
    voxi::Renderer::get().setSettings(s);
    voxiRenderer_.setSettings(s);
    voxiRenderer_.setNrd2WeightsPaths(
        aver::userDataDir().empty() ? std::string() : aver::userDataDir() + "\\" + render::denoise::kNrd2WeightsFileName,
        aver::executableDir() + "\\data\\" + render::denoise::kNrd2WeightsFileName);

    if (voxiRenderer_.init(*dev)) {
        dev->addRenderFeature(&voxiRenderer_);
        voxiAttached_ = true;
#if AVER_MODULE_PBR
        // Keep adjacent: factory and resolver installed together
        content_.setTextureFactory(dev->resources());
        voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_);
#endif
        // Depth proxy for LOD-based shadow/voxel optimization
        voxiRenderer_.setDepthProxy(&GameApp::depthProxyLookup, this);

        AVER_INFO("[Game] Voxi attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}",
                  caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);
    } else {
        AVER_WARN("[Game] Voxi failed to initialise; the world draws untextured");
    }
#else
    (void)e;
#endif
}

#if AVER_WITH_UI_ABI
// HUD render feature (overlay pass)
void GameApp::attachGameUi(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    gameUi_ = render::ui::UiRenderer::create(*dev);
    if (gameUi_) {
        dev->addRenderFeature(gameUi_);
        AVER_INFO("[Game] game UI attached");
    } else {
        AVER_WARN("[Game] game UI render feature failed to create -- the HUD will not draw");
    }
}

// Load font for game UI (non-fatal on all failures)
void GameApp::loadGameUiFont(Engine& e) {
    rhi::IDevice* dev = e.device();
    rhi::IResourceFactory* res = dev ? dev->resources() : nullptr;
    if (!res) return;   // headless, or a device that never came up -- nothing to upload into

    const std::string fontPath = executableDir() + "\\Roboto-Regular.ocfont";
    std::string text;
    if (!readFileText(fontPath, text)) {
        // Legal to ship without HUD font
        AVER_INFO("[Game] no game-UI font at {} -- the HUD draws without text", fontPath);
        return;
    }
    std::string why;
    if (!ui::parseOcfont(text, uiFont_, &why)) {
        AVER_WARN("[Game] '{}': {}", fontPath, why);
        return;
    }

    // Extract atlas filename from content-relative path
    std::string atlas = uiFont_.atlasPath;
    const usize slash = atlas.find_last_of("/\\");
    if (slash != std::string::npos) atlas = atlas.substr(slash + 1);
    const std::string atlasPath = executableDir() + "\\" + atlas;

    ImageData img;
    if (!decodeImage(atlasPath, img, &why)) {
        AVER_WARN("[Game] the game-UI font atlas '{}' could not be read ({})", atlasPath, why);
        uiFont_ = ui::UiFont{};   // glyphs with no atlas would sample nothing; worse than no font
        return;
    }
    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "GameUiFontAtlas";
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    uiFontTexture_ = res->createTexture(td);
    if (!uiFontTexture_) { AVER_WARN("[Game] the game-UI font atlas could not be uploaded"); return; }
    // UiDrawCmd::texture is cast back to rhi::TextureHandle
    uiFont_.atlasTexture = static_cast<u64>(uiFontTexture_);
    AVER_INFO("[Game] game-UI font '{}' loaded: {} glyph(s), atlas {}x{}",
              uiFont_.name, uiFont_.glyphs.size(), img.width, img.height);
}
#endif

void GameApp::attachParticles(Engine& e) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*dev)) {
        particleRenderer_.setSystem(&particles::particleSystem());
        dev->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // Install GI seam if Voxi attached and not disabled
        if (voxiAttached_ && !cfg_.noParticleGi) {
            particles::ParticleRenderer::GiSeam seam;
            seam.prepare = &GameApp::particleGiPrepare;
            seam.bind = &GameApp::particleGiBind;
            seam.user = this;
            particleRenderer_.setGiSeam(seam);
        }
#endif
        AVER_INFO("[Game] particles attached");
    } else {
        AVER_WARN("[Game] particle renderer unavailable on this device; CParticleEmitter placements draw nothing");
    }
#else
    (void)e;
#endif
}

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
void GameApp::spawnParticleTestContent(rhi::IDevice& device) {
    // Standalone particle test (no project required)
    const u64 cubeId = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    if (!content_.meshFor(cubeId)) content_.registerBuiltins(device);

    scene::World& world = scene::World::instance();

    // Match SandboxApp.cpp --particle-test for parity
    const Vec3 cubePos{600.0f, 0.0f, 50.0f};
    {
        Transform xf;
        xf.position = cubePos;
        xf.scale = Vec3{80.0f, 80.0f, 80.0f};
        const scene::Entity occ = world.create("ParticleTest.Occluder", scene::kInvalidEntity, xf);
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(occ, scene::kComponentMeshRenderer))) {
            mr->mesh = cubeId;
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
            // Crate-brown material
            mr->material = aver_scene_material(0, "M_Crate");
        }
    }

    particles::ParticleEffect fx;
    fx.shape = particles::EmitterShape::Box;
    fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
    fx.emissionRate = 150.0f;
    fx.burstCount = 0;
    fx.maxParticles = 400;
    fx.lifetimeMin = 3.0f;
    fx.lifetimeMax = 5.0f;
    fx.direction = Vec3{0.0f, 0.0f, 1.0f};
    fx.spreadDeg = 60.0f;
    fx.speedMin = 5.0f;
    fx.speedMax = 15.0f;
    fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
    fx.damping = 0.3f;
    fx.sizeStart = 25.0f;
    fx.sizeEnd = 45.0f;
    fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
    fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
    fx.blend = rhi::BlendMode::PremultipliedAlpha;
    constexpr u64 kDustCloudEffectId = 0x50415254'44550001ull;   // arbitrary, non-zero
    particles::particleEffects().set(kDustCloudEffectId, fx);

    {
        Transform xf;
        xf.position = cubePos;
        const scene::Entity emitter = world.create("ParticleTest.DustCloud", scene::kInvalidEntity, xf);
        if (auto* c = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emitter, scene::kComponentParticleEmitter))) {
            c->effect = kDustCloudEffectId;
        }
        AVER_INFO("[Game] --particle-test: dust cloud entity {} around effect 0x{:016X}",
                  emitter, kDustCloudEffectId);
    }

    // Additive embers, no GI, separate from dust cloud for visual clarity
    particles::ParticleEffect emberFx;
    emberFx.shape = particles::EmitterShape::Sphere;
    emberFx.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
    emberFx.emissionRate = 80.0f;
    emberFx.burstCount = 0;
    emberFx.maxParticles = 200;
    emberFx.lifetimeMin = 1.0f;
    emberFx.lifetimeMax = 1.6f;
    emberFx.direction = Vec3{0.0f, 0.0f, 1.0f};
    emberFx.spreadDeg = 35.0f;
    emberFx.speedMin = 40.0f;
    emberFx.speedMax = 80.0f;
    emberFx.gravity = Vec3{0.0f, 0.0f, -25.0f};
    emberFx.damping = 0.1f;
    emberFx.sizeStart = 22.0f;
    emberFx.sizeEnd = 6.0f;
    emberFx.colorStart[0] = 1.0f; emberFx.colorStart[1] = 0.55f; emberFx.colorStart[2] = 0.12f; emberFx.colorStart[3] = 1.0f;
    emberFx.colorEnd[0]   = 1.0f; emberFx.colorEnd[1]   = 0.15f; emberFx.colorEnd[2]   = 0.02f; emberFx.colorEnd[3] = 0.0f;
    emberFx.blend = rhi::BlendMode::Additive;
    emberFx.receivesGI = false;   // ember is its own light source
    constexpr u64 kEmberEffectId = 0x50415254'45420001ull;   // arbitrary, non-zero
    particles::particleEffects().set(kEmberEffectId, emberFx);

    {
        Transform xf;
        xf.position = cubePos + Vec3{0.0f, 0.0f, 220.0f};
        const scene::Entity emberEmitter = world.create("ParticleTest.Embers", scene::kInvalidEntity, xf);
        if (auto* c = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emberEmitter, scene::kComponentParticleEmitter))) {
            c->effect = kEmberEffectId;
        }
        AVER_INFO("[Game] --particle-test: ember entity {} around effect 0x{:016X} (receivesGI=false)",
                  emberEmitter, kEmberEffectId);
    }

    // Match SandboxApp camera position for parity
    camPos_ = Vec3{0.0f, 0.0f, 50.0f};
    yaw_ = 0.0f;
    pitch_ = 0.0f;
    AVER_INFO("[Game] --particle-test: camera set to (0,0,50) looking down +X");
}
#endif

void GameApp::installLevelHooks(Engine& e) {
#if AVER_MODULE_SCENE
    GameLevel::LoadHooks hooks;
    // Water, then terrain (matches editor order)
    hooks.beforePlacements = [this, &e](const std::string& path, const fmt::OcWorldData& w) {
#  if AVER_MODULE_FLUIDS
        if (rhi::IDevice* dev = e.device()) water_.applyLevel(*dev, w);
#  endif
#  if AVER_MODULE_LANDSCAPE
        pbr::MaterialSystem* ms = nullptr;
#    if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#    endif
        landscape_.loadForLevel(e.device(), project_.contentDir(), path, w, &content_, ms);
#  endif
        (void)path; (void)w;
    };
#  if AVER_MODULE_LANDSCAPE
    hooks.groundHeightAt = [this](f64 x, f64 y, f64& outZ) { return landscape_.groundHeightAt(x, y, outZ); };
#  endif
    hooks.afterUnload = [this, &e] {
        sequencePlayer_ = game::SequencePlayer{};
#  if AVER_MODULE_FLUIDS
        water_.unload();
#  endif
#  if AVER_MODULE_LANDSCAPE
        // Clear last level's terrain
        landscape_.unload(e.device());
#  endif
#  if AVER_MODULE_VOXI
        // Clear foliage (outside entity list)
        if (voxiAttached_) voxiRenderer_.clearFoliage();
#  endif
        (void)e;
    };
    // Display loading progress on startup splash
    hooks.progress = [&e](const std::string& stage, f32 fraction) {
        e.setLoadingStatus(stage);
        e.setLoadingProgress(fraction);
    };
    hooks.afterInstantiate = [this](const GameLevel::LoadedLevel& loaded) {
        levelGameMode_    = loaded.world.gameMode;      // World Settings overrides, read at begin play
        levelDefaultPawn_ = loaded.world.defaultPawn;
        // Records the placement loop does not know: bare CDecal entities, and prefab instances rebuilt from
        // their assets with their overrides on top.
        game::spawnLevelDecals(scene::World::instance(), loaded.world.decals);
        if (prefabSys_) prefabSys_->instantiateLevelInstances(loaded.world.prefabInstances);
        // The level's first sequence, bound to its placements' entities; it starts with the session.
        sequencePlayer_ = game::SequencePlayer{};
        if (!loaded.world.sequences.empty()) {
            std::vector<scene::Entity> byPlacement(loaded.world.placements.size(), scene::kInvalidEntity);
            const auto& inst = loaded.instance;
            for (usize k = 0; k < inst.entities.size() && k < inst.placementIndex.size(); ++k)
                if (inst.placementIndex[k] < byPlacement.size()) byPlacement[inst.placementIndex[k]] = inst.entities[k];
            const fmt::OcSequence& seq = loaded.world.sequences.front();
            sequencePlayer_.setSequence(seq);
            sequencePlayer_.bind(byPlacement);
            sequencePlayer_.setTime(0);
            if (seq.autoplay) sequencePlayer_.play();
            AVER_INFO("[Game] level sequence '{}': {} track(s), {:.1f}s{}", seq.name, seq.tracks.size(), seq.length,
                      seq.autoplay ? ", autoplay" : "");
        }
#  if AVER_MODULE_VOXI
        // Load foliage after placements (matches editor)
        if (voxiAttached_) {
            const FoliageLoadResult fr =
                loadLevelFoliage(loaded.world, content_, &voxiRenderer_, project_.contentDir());
            if (!fr.error.empty()) AVER_WARN("[Foliage] {}", fr.error);
        }
#  else
        (void)loaded;
#  endif
    };
    level_.setLoadHooks(std::move(hooks));
#  if AVER_MODULE_LANDSCAPE
    // Restart streaming on terrain change
    landscape_.setTerrainChangedHook([this] {
        if (!streaming_.enabled()) return;
        AVER_INFO("[ChunkWorld] terrain changed under a live stream -- restarting it so scatter "
                  "re-generates against the new surface");
        streaming_.disable();
        enableChunkStreaming();
    });
#  endif
#else
    (void)e;
#endif
}

void GameApp::enableChunkStreaming() {
#if AVER_MODULE_SCENE
    GameStreaming::HeightQueryFn height;
#  if AVER_MODULE_LANDSCAPE
    if (landscape_.loaded())
        height = [this](f32 x, f32 y, f32& outZ) { return landscape_.scatterHeightAt(x, y, outZ); };
#  endif
    streaming_.enable(project_, level_.pcgVolumes(), level_.scatterSpecies(), &content_, std::move(height));
    streaming_.warnIfCameraOutsideGeneratedBand(camPos_);
#endif
}

void GameApp::openProject(Engine& e) {
    installLevelHooks(e);
    // Look beside executable for Game.ocproject (not working directory)
    std::string path = cfg_.projectPath;
    if (path.empty()) {
        const std::string beside = executableDir() + "\\Game.ocproject";
        if (fileExists(beside)) { path = beside; }
    }
    if (path.empty()) {
        AVER_INFO("[Game] no project: pass one on the command line, or ship a Game.ocproject beside the executable");
#if AVER_MODULE_SCENE
        // Load level without project if named on command line
        if (!cfg_.levelPath.empty()) level_.loadStartMap(project_, content_, cfg_.levelPath);
#endif
        return;
    }

    std::string err;
    if (!fmt::loadOcproject(path, project_, &err)) {
        AVER_ERROR("[Game] could not open '{}': {}", path, err);
        return;   // deliberately not fatal; see the header
    }
    AVER_INFO("[Game] project '{}'  content={}  startMap='{}'",
              project_.name, project_.contentDir(),
              project_.startMap.empty() ? "<none>" : project_.startMap);
#if AVER_MODULE_PBR
    // Set texture cache dir before any load
    assets::setTextureCacheDir(project_.dir + "\\Saved\\DerivedDataCache\\Textures");
#endif
    if (project_.startMap.empty()) {
        AVER_WARN("[Game] the manifest names no STARTMAP, so there is no level to open");
    }
#if AVER_MODULE_VOXI
    // Read frame budget from manifest (<=0 disables it)
    frameBudget_.budgetMs = project_.frameBudgetMs;
#endif
    // Load order is critical: index -> meshes -> levels
    content_.adopt(project_);
#if AVER_MODULE_SCENE
    if (rhi::IDevice* dev = e.device()) {
        content_.registerBuiltins(*dev);
        content_.loadProjectMeshes(*dev);
    }
#if AVER_MODULE_PARTICLES
    // Load particle effects before levels (CPU-only)
    content_.loadProjectParticleEffects();
#endif
    level_.loadStartMap(project_, content_, cfg_.levelPath);
#if AVER_MODULE_SYNAPSE_SCENE
    // Load navigation (optional; most levels have none)
    {
        std::string navErr;
        const std::string navPath = navPathForLevel(level_.path());
        if (!navPath.empty() && fmt::loadOcNav(navPath, gameNav_, &navErr)) {
            AVER_INFO("[Game] navigation: '{}' ({}x{} cells)", navPath, gameNav_.widthCells, gameNav_.heightCells);
        } else if (!navPath.empty()) {
            AVER_INFO("[Game] navigation: no baked '{}' ({}) -- Synapse agents will wait for a goal grid",
                      navPath, navErr);
        }
    }
#endif
    applyLevelSky();
    // Manifest GI volume wins over auto-fit (mirrors editor precedence)
    if (project_.hasGiVolume) {
        giCenter_ = Vec3{project_.giCenter[0], project_.giCenter[1], project_.giCenter[2]};
        giExtent_ = project_.giExtent;
        AVER_INFO("[Voxi] GI volume from the manifest: centre[{:.0f} {:.0f} {:.0f}] extent {:.0f}cm",
                  giCenter_.x, giCenter_.y, giCenter_.z, giExtent_);
    }
    if (project_.giExtent <= 0.0f) fitGiVolumeToLevel();
#endif
    applyProjectRenderSettings();
    // RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE/TONEMAP are per-device post state (like PHYSICS/AUDIO)
    applyProjectPostSettings();
#if AVER_MODULE_PHYSICS
    // Apply physics settings from manifest
    game::applyProjectPhysics(project_);
#endif
#if AVER_WITH_AUDIO_ABI
    // Apply audio mix from manifest
    game::applyProjectAudioMix(project_);
#endif
}

#if AVER_MODULE_SCRIPTING
// Convert project name to filesystem-safe directory name
static std::string filesystemSafeProjectName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool reserved = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                               c == '\\' || c == '|' || c == '?' || c == '*' ||
                               static_cast<unsigned char>(c) < 0x20;
        out += reserved ? '_' : c;
    }
    const std::size_t b = out.find_first_not_of(" \t.");
    const std::size_t e = out.find_last_not_of(" \t.");
    out = (b == std::string::npos) ? std::string() : out.substr(b, e - b + 1);
    return out.empty() ? "Project" : out;
}
#endif

void GameApp::initScripting() {
#if AVER_MODULE_SCRIPTING
    // Initialize CLR host and scripting system
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + "\\Scripting";
    // Scripts dir: <out>\Binaries\Scripts (see ProjectScaffold.cpp/stage-game.ps1)
    hd.scriptsDir = project_.valid() ? (project_.binariesDir() + "\\Scripts") : std::string();
    scriptsReady_ = scripts_.init(hd);
    if (scriptsReady_) {
        AVER_INFO("[Game] scripting host ready: bridge='{}' scripts='{}' ({} legacy behaviour(s) live)",
                  hd.bridgeDir, hd.scriptsDir, scripts_.behaviourCount());
        // Graph-as-class: declare framework classes before discoverProjectGraphs (which skips claimed files)
        const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
        if (graphClasses > 0)
            AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());

        // Input scheme
        {
            // INPUT.SCHEME is relative to content root, like DRONE.GRAPH
            std::string schemePath = project_.inputScheme.empty()
                ? std::string() : project_.contentDir() + "\\" + project_.inputScheme;
            if (!schemePath.empty() && !fileExists(schemePath)) {
                AVER_WARN("[Scripting] INPUT.SCHEME '{}' does not exist -- no input scheme will be loaded",
                          schemePath);
                schemePath.clear();
            }

            const std::string settingsPath =
                userDataDir() + "\\" + filesystemSafeProjectName(project_.name) + "\\Settings.ini";

            const i32 actions = scripts_.configureInput(schemePath, settingsPath);
            if (actions > 0)
                AVER_INFO("[Scripting] input scheme ready: {} action(s) from '{}', bindings persist to '{}'",
                          actions, schemePath, settingsPath);
            else if (actions == 0)
                AVER_INFO("[Scripting] no input scheme loaded (project has no INPUT.SCHEME, or its file "
                          "was missing) -- settings store opened at '{}'", settingsPath);
            else if (actions == -2)
                AVER_WARN("[Scripting] this build's scripting bridge predates ConfigureInput -- "
                          "rebindable input is unavailable");
            else
                AVER_WARN("[Scripting] input scheme '{}' failed to load (see the [Scripting] error line "
                          "just above)", schemePath);
        }

        // Animation notifies wired to the scripting host
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&GameApp::animCurve, this);
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
        // GetSynapseTarget reaches CSynapseAgent waypoint (requires both modules)
        aver_fw_set_synapse_target_provider(&GameApp::synapseTarget, this);
        // GetSynapsePerception reaches sight state
        aver_fw_set_synapse_perception_provider(&GameApp::synapsePerception, this);
        // PerceptionSystem target resolver (not a framework relay; Aver.Synapse.Scene cannot link Aver.Framework)
        synapse::perceptionSystem().setTargetResolver(&GameApp::synapseTargetResolver, this);
#endif
#endif

        if (scripts_.graphFireAvailable()) {
#if AVER_MODULE_SCENE
            anim::animSystem().setNotifySink(&GameApp::animNotify, this);
#endif
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink as animNotify (byte-for-byte identical signatures)
            synapse::perceptionSystem().setNotifySink(&GameApp::animNotify, this);
            // "FireEvent" BT action also fires through animNotify
            synapse::btSystem().setNotifySink(&GameApp::animNotify, this);
#endif
            AVER_INFO("[Anim] animation notifies will be raised as graph events");
        } else {
            AVER_WARN("[Anim] this build's scripting bridge cannot be fired at; animation notifies "
                      "will be tracked but not delivered (see ScriptHost::graphFireAvailable)");
        }
    } else {
        AVER_INFO("[Game] scripting host unavailable ({}) -- graphs and any other C# gameplay will not run",
                  scripts_.declineReason());
    }
#endif
}

// Framework's curve provider (guarded on SCENE, not SCRIPTING: definition stayed unguarded while declaration was guarded)
#if AVER_MODULE_SCENE
i32 GameApp::animCurve(i32 entity, i64 nameHash, f32* outValue, void*) {
    f32 v = 0.0f;
    if (!anim::animSystem().curveValue(static_cast<scene::Entity>(entity),
                                       static_cast<u64>(nameHash), v)) return 0;
    *outValue = v;
    return 1;
}
#endif // AVER_MODULE_SCENE (animCurve)

#if AVER_MODULE_SYNAPSE_SCENE
i32 GameApp::synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void*) {
    scene::World& w = scene::World::instance();
    const auto* a = w.component<synapse::CSynapseAgent>(static_cast<scene::Entity>(entity),
                                                         synapse::agentSystem().componentType());
    if (!a || a->status != static_cast<i32>(synapse::AgentStatus::Pathing)) return 0;
    *outX = a->targetXCm;
    *outY = a->targetYCm;
    *outZ = a->targetZCm;
    return 1;
}

#if AVER_MODULE_FRAMEWORK
scene::Entity GameApp::synapseTargetResolver(void*) {
    return static_cast<scene::Entity>(aver_fw_controlled_pawn(aver_fw_player_controller(0)));
}
#endif

i32 GameApp::synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget, f32* outTimeSinceSeen, void*) {
    scene::World& w = scene::World::instance();
    const auto* p = w.component<synapse::CSynapsePerception>(
        static_cast<scene::Entity>(entity), synapse::perceptionSystem().componentType());
    if (!p) return 0;
    *outCanSee = p->canSeeTarget;
    *outLastTarget = p->lastKnownTargetEntity;
    *outTimeSinceSeen = p->timeSinceSeenSec;
    return 1;
}
#endif

// Animation-notify wire: clip marker names graph event, raised on the entity playing the clip
// graphFire returns false for entity with no graph or graph with no event (not an error)
#if AVER_MODULE_SCENE
void GameApp::animNotify(scene::Entity e, const char* name, void* user) {
#if AVER_MODULE_SCRIPTING
    auto* self = static_cast<GameApp*>(user);
    if (!self || !name) return;
    self->scripts_.graphFire(static_cast<i32>(e), name);
#else
    (void)e; (void)name; (void)user;
#endif
}
#endif // AVER_MODULE_SCENE (animNotify)


void GameApp::discoverProjectGraphs() {
#if AVER_MODULE_SCRIPTING
    if (!scriptsReady_) return;
    if (!scripts_.graphAvailable()) {
        AVER_INFO("[Graph] this build's scripting bridge has no graph hosting -- project .ocgraph "
                  "files, if any, will not run (see ScriptHost::graphAvailable's own doc)");
        return;
    }

    // No-graph project is a no-op: empty projectGraphs_, empty early-out loop
    const std::vector<std::string> paths = content_.pathsWithExtension(".ocgraph");
    if (paths.empty()) {
        AVER_INFO("[Graph] 0 .ocgraph file(s) under this project's content -- nothing to run");
        return;
    }

    // Synthetic negative entity ids (-1000, decreasing) for project-level graphs (not scoped to any entity)
    i32 nextId = -1000;
    u32 loaded = 0;
    u32 skippedAsClasses = 0;
    u32 skippedForeign = 0;
    for (const std::string& path : paths) {
        // Graph-as-class: file with CLASS record already claimed by declareGraphClasses (in initScripting)
        std::string text;
        const bool haveText = readFileText(path, text);
        // Not every .ocgraph is a gameplay graph; material graphs fail with "unknown node type"
        if (haveText && ocgraphIsForeign(text)) {
            ++skippedForeign;
            continue;
        }
        if (haveText && ocgraphDeclaresClass(text)) {
            ++skippedAsClasses;
            continue;
        }

        const i32 id = nextId--;
        const bool ok = scripts_.graphLoad(id, path);
        projectGraphs_.push_back(ProjectGraph{path, id, ok});
        if (ok) {
            ++loaded;
            AVER_INFO("[Graph] loaded '{}' (synthetic id {})", path, id);
        } else {
            AVER_WARN("[Graph] '{}' failed to load -- see the [GraphHost] reason above; continuing without it", path);
        }
    }
    AVER_INFO("[Graph] {} of {} project graph(s) loaded ({} skipped -- declared as classes instead, "
              "{} not gameplay graphs)",
              loaded, projectGraphs_.size(), skippedAsClasses, skippedForeign);
#endif
}

void GameApp::tickProjectGraphs(f32 dt) {
#if AVER_MODULE_SCRIPTING
    if (!scriptsReady_ || projectGraphs_.empty()) return;
    // Ungated on aver_fw_play_state(): Aver.Graph has no reference to Aver.Framework
    for (const ProjectGraph& g : projectGraphs_) {
        if (!g.loaded) continue;
        scripts_.graphTick(g.syntheticEntity, dt);
    }
#else
    (void)dt;
#endif
}

void GameApp::beginPlayIfGameModeDeclared() {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCRIPTING
    if (!scriptsReady_) return;

    // The level's World Settings GAMEMODE, else the project's GAME.MODE, else (a shipped game has no
    // editor drone) the first concrete GameMode declared, as before those keys existed.
    auto named = [](const std::string& n, i32 flag) -> i32 {
        const i32 c = n.empty() ? 0 : aver_fw_class_find(n.c_str());
        return (c && (aver_fw_class_get_flags(c) & flag)) ? c : 0;
    };
    i32 modeClass = named(levelGameMode_, AVER_FW_CLASS_GAME_MODE);
    if (!modeClass) modeClass = named(project_.gameMode, AVER_FW_CLASS_GAME_MODE);
    if (!modeClass) modeClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
    const i32 pawnClass = named(levelDefaultPawn_, AVER_FW_CLASS_PAWN);
    // Latch for the rest of process (onUpdate's graph-class tick needs to distinguish "not begun" from "undeclared")
    gameModeDeclared_ = modeClass != 0;
    if (modeClass == 0) {
        AVER_INFO("[Game] no GameMode class declared -- play session not started (the framework stays "
                  "exactly as inert as it was before this existed; see discoverProjectGraphs for the "
                  "same shape decided by content instead of by class declarations)");
        return;
    }
    const i32 instanceClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);
    // (PlayMobility already began in onInit, before this call)
    if (aver_fw_begin_play_with_pawn(instanceClass, modeClass, pawnClass)) {
        AVER_INFO("[Game] play session begun automatically (GameMode class {}) -- a shipped game has no "
                  "editor Play button, so booting it IS beginning play", modeClass);
        // Pawn doesn't exist until GameMode spawned and possessed it
        placePawnAtSpawn();
    } else {
        AVER_WARN("[Game] aver_fw_begin_play declined for GameMode class {} -- the framework stays in "
                  "EDITOR state; the world still renders, nothing in it plays", modeClass);
    }
#endif
}

// Place pawn at level spawn (from beginPlayIfGameModeDeclared after aver_fw_begin_play succeeds)
bool GameApp::placePawnAtSpawn() {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    const GameLevel::SpawnPoint& sp = level_.spawn();
    if (!sp.valid) return false;
    if (!game::placePossessedPawn(sp.position, sp.yawDeg)) return false;
    AVER_INFO("[Game] pawn placed at the level's Player Start ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
              sp.position.x, sp.position.y, sp.position.z, sp.yawDeg);
    return true;
#else
    return false;
#endif
}

rhi::MeshHandle GameApp::depthProxyLookup(rhi::MeshHandle mesh, void* user) {
#if AVER_MODULE_SCENE
    if (!user) return 0;
    const GameApp* app = static_cast<const GameApp*>(user);
    const auto& proxyMap = app->content_.depthProxyMap();
    const auto it = proxyMap.find(mesh);
    return it == proxyMap.end() ? 0 : it->second;
#else
    (void)mesh; (void)user;
    return 0;
#endif
}

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// ParticleRenderer GI seam (particles DECIDED 4; modules/particles does not include voxi headers)
bool GameApp::particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                std::string* outPrelude, std::string* outDefines, void* user) {
    (void)user;
    if (!outPrelude || !outDefines) return false;
    *outPrelude = voxi::giShaderPrelude();
    *outDefines = voxi::giShaderDefines(srvBase, samplerBase, cbRegister);
    return true;
}

void GameApp::particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                             const void** outCbData, u32* outCbBytes, void* user) {
    auto* self = static_cast<GameApp*>(user);
    if (!self || !outCbData || !outCbBytes) return;
    self->voxiRenderer_.bindGiResources(res, set, srvBase);
    *outCbData = self->voxiRenderer_.giFrameConstants();
    *outCbBytes = self->voxiRenderer_.giFrameConstantBytes();
}
#endif

void GameApp::applyLevelSky() {
#if AVER_MODULE_SCENE
    const fmt::OcWorldEnv& w = level_.env();
    if (!w.hasSun && !w.hasSky && !w.hasFog && !w.hasClouds) return;

    // Shared mapping (aver::voxi::applyLevelEnv) ensures game and editor don't drift
    assets::applyLevelEnv(w, sky_);

    // Reseed from atmosphere (frame loop resets sunColor_ every frame)
    for (int i = 0; i < 3; ++i) sunColor_[i] = sky_.sunColor[i];
    if (w.hasFog) fogDensity_ = sky_.fogDensity;

    AVER_INFO("[Level] applied environment: sun={} sky={} fog={} clouds={}, intensity {:.2f}, "
              "model {}", w.hasSun, w.hasSky, w.hasFog, w.hasClouds, sky_.sunIntensity,
              w.skyPhysical ? "physical" : "authored");
#endif
}

// Fit GI volume to level (extent sets voxel size in 128^3 grid: ~9cm at 12m, ~15.6m at 2000m)
void GameApp::fitGiVolumeToLevel() {
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
    Vec3 lo{}, hi{};
    f32 radius = 0.0f;
    if (!level_.placementBounds(lo, hi, radius)) return;
    giCenter_ = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    giExtent_ = radius;
    AVER_INFO("[Voxi] GI volume fitted to the level: centre[{:.0f} {:.0f} {:.0f}] extent {:.0f}cm",
              giCenter_.x, giCenter_.y, giCenter_.z, giExtent_);
#endif
}

void GameApp::applyProjectRenderSettings() {
#if AVER_MODULE_VOXI
    if (!project_.valid() || !project_.hasRenderSettings()) return;
    if (!voxiAttached_) return;

    // Two-phase: tiers commit first, so derivation runs against new tier before manifest lands on top
    voxi::Renderer& vx = voxi::Renderer::get();
    voxi::Settings s = vx.settings();
    voxi::applyManifestTwoPhase(project_, s, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    voxiRenderer_.setSettings(vx.settings());
    AVER_INFO("[Project] applied render settings: gi={}, rt={}, pt={}, voxelRes={}, giIntensity={}, giDist={}",
              static_cast<int>(s.globalIllumination), static_cast<int>(s.rayTracing),
              static_cast<int>(s.pathTracing), s.voxelResolution, s.giIntensity, s.giMaxDistance);

    // Report manifest contradictions: requests that didn't survive RenderSettingsResolver prerequisites
    voxi::ManifestAsks asks;
    asks.giMode         = project_.giMode;
    asks.denoiser       = project_.denoiser;
    asks.rtRenderMode   = project_.rtRenderMode;
    asks.refractionMode = project_.refractionMode;
    voxi::FieldReport reports[4];
    const u32 reportCount = voxi::manifestContradictions(vx.settings(), vx.deviceInfo(), asks, reports);
    for (u32 i = 0; i < reportCount; ++i) {
        voxi::Feature alreadyReportedBy;
        if (voxi::refusalFeatureFor(reports[i].reason, alreadyReportedBy) &&
            vx.refusalLogged(alreadyReportedBy)) {
            continue;
        }
        AVER_WARN("[Project] RENDER.{} asked for {} but it is not in effect: {}",
                  reports[i].manifestKey, reports[i].requested,
                  voxi::disableReasonText(reports[i].reason));
    }
#endif
}

// Apply post-processing settings from manifest (decoupled from applyProjectRenderSettings like PHYSICS/AUDIO)
void GameApp::applyProjectPostSettings() {
    if (!project_.valid()) return;

    bool stated = false;
    if (project_.postExposure >= 0.0f)  { post_.exposure       = project_.postExposure;          stated = true; }
    if (project_.postBloom    >= 0.0f)  { post_.bloomIntensity = project_.postBloom;             stated = true; }
    if (project_.postAutoExposure >= 0) { post_.autoExposure   = project_.postAutoExposure != 0; stated = true; }
    if (project_.postTonemap >= 0) {
        // Clamp mode [0,2] (matches console and frame reporting)
        post_.tonemap = static_cast<u32>(project_.postTonemap > 2 ? 2 : project_.postTonemap);
        stated = true;
    }
    if (!stated) return;

    AVER_INFO("[Project] applied post settings: exposure={:.3f} bloom={:.3f} autoExposure={} tonemap={}",
              post_.exposure, post_.bloomIntensity, post_.autoExposure ? 1 : 0, post_.tonemap);

    // With auto-exposure on, composite pass ignores RENDER.EXPOSURE (post.hlsl:216-220)
    if (project_.postExposure >= 0.0f && post_.autoExposure) {
        AVER_WARN("[Project] RENDER.EXPOSURE {:.3f} is not in effect: auto-exposure is on{} and "
                  "replaces the exposure every frame -- state RENDER.AUTOEXPOSURE 0 to use it",
                  project_.postExposure,
                  project_.postAutoExposure >= 0 ? " (RENDER.AUTOEXPOSURE 1)" : " by default");
    }
}

Vec3 GameApp::camForward() const {
    return game::cameraForward(yaw_, pitch_);
}

f32 GameApp::viewAspect(const Engine& e) const {
    // From swapchain (game scene is the backbuffer, unlike editor's dockspace panel)
    if (const Window* w = e.window()) {
        const u32 h = w->height();
        if (h > 0) return static_cast<f32>(w->width()) / static_cast<f32>(h);
    }
    return 16.0f / 9.0f;
}

void GameApp::pushFrame(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // No setViewportRect: games render to the whole backbuffer, unlike the editor's dockspace.
    //
    // --cam-wobble DEG PERIOD: sinusoidal offset, applied temporarily to yaw_ for this call only.
    f32 wobble = 0.0f;
    if (cfg_.camWobbleDeg != 0.0f && cfg_.camWobblePeriod != 0) {
        wobble = cfg_.camWobbleDeg * 0.01745329252f *
                 std::sin(6.2831853f * static_cast<f32>(frames_ - 1) / static_cast<f32>(cfg_.camWobblePeriod));
    }
    const f32 yawBeforeWobble = yaw_;
    yaw_ += wobble;
    const Vec3 fwd = camForward();
    yaw_ = yawBeforeWobble;
    const f32  aspect = viewAspect(e);
    const game::CameraMatrices cam = game::pushCamera(*dev, camPos_, fwd, aspect);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = camPos_;

    // Log once: verifies aspect ratio calculation.
    if (frames_ <= 1) {
        AVER_INFO("[Game] camera: aspect={:.3f} fov=60deg near={} far={} (from the swapchain, not a viewport rect)",
                  aspect, game::kCameraNearCm, game::kCameraFarCm);
    }

    // One member for one value: applyLevelSky seeds fogDensity_.
    const f32 fog = fogDensity_;
    // sunDirection stays unnormalised; shaders normalise it.
    sky_.enabled = true;
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
#if AVER_MODULE_SCENE
    // PCGVOLUME "Sky" drives clouds: seed picks the sky, coverage floor becomes cloud cover (inverted).
    if (const GameLevel::PcgField* skyField = level_.pcgField("Sky")) {
        sky_.cloudsEnabled = true;
        sky_.cloudSeed     = skyField->infinite ? skyField->infiniteSpec.seed
                                                : skyField->boundedSpec.seed;
        const f32 floorV = skyField->infinite ? skyField->infiniteSpec.coverageFloor
                                              : skyField->boundedSpec.coverageFloor;
        sky_.cloudCoverage = 1.0f - (floorV < 0.0f ? 0.0f : (floorV > 1.0f ? 1.0f : floorV));
        if (frames_ <= 1)
            AVER_INFO("[PCG] sky field '{}' parameterises the cloud layer: seed {}, coverage {:.2f}",
                      skyField->name, sky_.cloudSeed, sky_.cloudCoverage);
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // Script sky wins over level sky when published; read every frame for hot-reload.
    {
        i32 seed = 0;
        f32 coverage = 0.0f, density = 0.0f, bottom = 0.0f, top = 0.0f, scale = 0.0f;
        f32 windX = 0.0f, windY = 0.0f;
        if (aver_fw_sky_clouds(&seed, &coverage, &density, &bottom, &top, &scale, &windX, &windY)) {
            sky_.cloudsEnabled = true;
            sky_.cloudSeed     = seed;
            sky_.cloudCoverage = coverage;
            sky_.cloudDensity  = density;
            sky_.cloudBottom   = bottom;
            sky_.cloudTop      = top;
            sky_.cloudScale    = scale;
            sky_.cloudWind[0]  = windX;
            sky_.cloudWind[1]  = windY;
            if (!scriptSkyReported_) {
                scriptSkyReported_ = true;
                AVER_INFO("[Sky] a script owns the cloud layer: seed {}, coverage {:.2f}, "
                          "{:.0f}..{:.0f} cm", seed, coverage, bottom, top);
            }
        } else if (scriptSkyReported_) {
            scriptSkyReported_ = false;
            AVER_INFO("[Sky] the script released the cloud layer; the level's sky is back");
        }
    }
#endif
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity        = fog;
    // Cloud clock: advanced by real time so wind moves. App owns it; a clock that never advances gives a painted backdrop.
    sky_.cloudTime         = cloudTime_;

#if AVER_MODULE_VOXI
    if (voxiAttached_) {
        voxi::Renderer& vx = voxi::Renderer::get();
        // G-buffer switch reads denoiserGBufferWanted; MSAA push consumes consumeMsaaDirty().
        // Frame interpolation: wants motion and depth from G-buffer. Reads --frame-interp over RENDER.FRAMEINTERP.
        bool wantFrameInterp = cfg_.frameInterpArg >= 0 ? cfg_.frameInterpArg == 1 : project_.frameInterp == 1;
        if (wantFrameInterp && !frameInterpolator_) {
            if (dev->resources()) {
                auto fg = std::make_unique<neurafi::NeuraFI>(*dev);
                // --frame-interp-trajectory; neural reads per-machine file or shipped weights.
                fg->setTrajectory(static_cast<neurafi::Trajectory>(cfg_.frameInterpTrajectory));
                fg->setWeightsPath(userDataDir() + "\\" + neurafi::NeuraFI::kWeightsFileName);
                fg->setShippedWeightsPath(executableDir() + "\\data\\" +
                                          neurafi::NeuraFI::kWeightsFileName);
                frameInterpolator_ = std::move(fg);
                dev->setFrameInterpolator(frameInterpolator_.get());
            } else {
                wantFrameInterp = false;
            }
        }
        dev->setFrameInterpolation(wantFrameInterp);
        dev->setGBufferEnabled(voxi::resolve(vx.settings(), vx.deviceInfo()).denoiserGBufferWanted || wantFrameInterp);
        // Ray-driven frames run at 1x whatever MSAA says (voxi::Resolution::sampleCount).
        {
            const u32 samples = voxi::resolve(vx.settings(), vx.deviceInfo()).sampleCount;
            if (vx.consumeMsaaDirty() || samples != msaaPushed_) {
                dev->setSampleCount(samples);
                msaaPushed_ = samples;
            }
        }

        // LEVEL's GI volume, fitted once at load; not camera-following (would re-voxelise every move).
        voxiRenderer_.setVolume(&giCenter_.x, giExtent_);
        // Reads editor's cache; never writes a miss back. A level without a baked volume voxelises as usual.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif

#if AVER_MODULE_FLUIDS
    // Apply underwater fog (sky stays authored).
    dev->setSkyAtmosphere(water_.applyUnderwaterFog(sky_, camPos_.z));
#else
    dev->setSkyAtmosphere(sky_);
#endif
    // Black: a game has no outside; anything the sky doesn't cover should read as a bug, not a colour.
    dev->setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev->setPostProcess(post_);
}

void GameApp::onInit(Engine& e) {
    // First, before any file is read: catches package boundaries.
    if (cfg_.traceOpens) {
        setFileTrace(&onFileOpen, nullptr);
        AVER_INFO("[Game] --trace-opens: every engine file read is logged with an [open] prefix");
    }

#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    // GPU crowd backend: selectable through the Synapse ABI once installed; CPU stays the default.
    if (rhi::IDevice* gdev = e.device()) crowdGpuAttached_ = game::installCrowdGpu(*gdev, crowdGpu_);
#endif
    // Platform-side InputState: games read the window's event stream, not ImGui.
    window_ = e.window();
    if (Window* w = e.window()) {
        w->setEventCallback(&onWindowEvent, &input_);
        AVER_INFO("[Game] input bound to the window event stream ({}x{})", w->width(), w->height());
    } else {
        AVER_INFO("[Game] headless: no window, no input");
    }
    // Verify link interface: if wrong, lifted code compiles to nothing and the world is empty.
    AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={} PARTICLES={}",
              AVER_MODULE_PBR, AVER_MODULE_SCENE, AVER_MODULE_VOXI,
              AVER_MODULE_PHYSICS, AVER_MODULE_FRAMEWORK, AVER_MODULE_SCRIPTING, AVER_MODULE_PARTICLES);
    // Applied once at start: mirrors SandboxApp's vsyncOffRequested_ handling.
    if (cfg_.vsyncOff) {
        if (rhi::IDevice* dev = e.device()) {
            if (dev->vsyncCanDisable()) { dev->setVSync(false); AVER_INFO("[Game] vsync OFF (--no-vsync)"); }
            else AVER_WARN("[Game] --no-vsync ignored: this display path cannot tear");
        } else {
            AVER_WARN("[Game] --no-vsync ignored: no device");
        }
    }
    // Render features: skinning prePass stages bone matrices; scene pass then draws.
    attachSkinning(e);
#if AVER_MODULE_FLUIDS
    // Before Voxi: its acceleration-structure build reads the fluid's prePass vertex buffer.
    if (rhi::IDevice* dev = e.device()) water_.init(*dev);
#endif
    attachVoxi(e);
#if AVER_WITH_UI_ABI
    attachGameUi(e);
    // loadGameUiFont only needs a device; grouped with UI-ABI setup.
    loadGameUiFont(e);
#endif
    attachParticles(e);
    if (cfg_.pcgVolumeTest) attachPcgTest(e);
    initPhysics();      // Before openProject: level load builds static bodies.
#if AVER_WITH_AUDIO_ABI
    // Opens audio device; gates on started flag. Before openProject: mix settings need a started device.
    // 0 is not an error (no output device); unlike physics, which requires success.
    game::startAudio("Game");
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    // Before openProject: registration needs no level/physics. Class placements must find components already registered.
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    // After registerComponents: built-in behaviors read these types directly.
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif
#if AVER_MODULE_SCENE
    // Control rig: registered in editor onInit but never before in shipped game. Class placements must find the type.
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
#endif
    // Animation state machines, reverb zones, blackboard relay, crowd/hearing/cover behaviours.
    game::registerGameSystems();
#if AVER_MODULE_SCENE
    // Prefab assets come out of the project's content index (the same id space every other asset uses).
    prefabLib_.setLoader([this](const std::string& path, fmt::OcPrefabData& out, std::string* why) {
        const std::string full = content_.pathFor(fnv1a64(std::string_view(path)));
        if (full.empty()) {
            if (why) *why = "'" + path + "' is not in the project content";
            return false;
        }
        return fmt::loadOcPrefab(full, out, why);
    });
    prefabSys_ = std::make_unique<prefab::PrefabSystem>(scene::World::instance(), prefabLib_);
    prefabHost_ = prefab::makeAbiHost(*prefabSys_);
    aver_prefab_set_host(&prefabHost_);
#endif
    openProject(e);
#if AVER_MODULE_VOXI
    // AverSR resolved after openProject. applyProjectRenderSettings committed project's GI/RT tiers.
    // CLI (--aversr) beats RENDER.AVERSR beats Auto. Shipped game: userLevel pinned to -1.
    if (rhi::IDevice* dev = e.device()) {
        const voxi::Renderer& vx = voxi::Renderer::get();
        const voxi::AverSrDecision dec = voxi::resolveAverSrLevel(
            cfg_.averSrArg, -1, project_.averSr, voxi::autoAverSrLevel(vx.settings(), vx.deviceInfo()));

        // averSrInstaller_ is null when SR module is absent. Resolved level with no installer stays native 1.0.
        f32 scale = 1.0f;
        if (dec.level != voxi::ladder::kAverSrOff && averSrInstaller_ &&
            averSrInstaller_(*dev, dec.level, averSrUpscaler_, scale)) {
            dev->setRenderScale(scale);
            dev->setUpscaler(averSrUpscaler_.get());
        }

        // Mirrors setRenderScale formula: scene renders at round(present * scale).
        const u32 presentW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 presentH = e.window() ? e.window()->height() : cfg_.height;
        const u32 sceneW = static_cast<u32>(std::lround(static_cast<f64>(presentW) * scale));
        const u32 sceneH = static_cast<u32>(std::lround(static_cast<f64>(presentH) * scale));
        AVER_INFO("[AverSR] {} ({}): scene {}x{} -> present {}x{}",
                  averSrLevelName(dec.level), averSrSourceName(dec.source), sceneW, sceneH, presentW, presentH);
    }
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // After openProject: needs registerBuiltins' unit cube and overrides camera state.
    if (cfg_.particleTest) {
        if (rhi::IDevice* dev = e.device()) spawnParticleTestContent(*dev);
        else AVER_WARN("[Game] --particle-test: no device, nothing spawned");
    }
#endif
    // After openProject: scripts directory and graph discovery need state openProject populated.
    // Scan before bootstrapping to avoid cost on graph-free projects (measured: 40-60ms extra).
#if AVER_MODULE_SCRIPTING
    const bool haveGraphs = !content_.pathsWithExtension(".ocgraph").empty();
    // "\\" not "\": MSVC drops unescaped backslash (C4129), otherwise tests "...BinariesScripts" (no separator).
    const bool haveScriptAssembly = std::filesystem::exists(project_.binariesDir() + "\\Scripts");
    if (haveGraphs || haveScriptAssembly) {
        initScripting();
        discoverProjectGraphs();
    } else {
        AVER_INFO("[Game] no .ocgraph content and no Scripts assembly -- scripting host not started");
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // Deliberately after initScripting: class placement's class isn't declared until this runs.
    // See GameLevel.hpp's classPlacements_ comment.
    level_.spawnClassPlacements();
#endif
    // Census taken after level load and class spawns, before beginPlay.
#if AVER_MODULE_SCENE
    if (cfg_.sceneCensus) {
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(world::takeSceneCensus(scene::World::instance())));
    }
#endif
#if AVER_MODULE_SCENE
    // Shipped game: booting the level starts objects (editor turns this on at Play). Marks objects movable.
    playMobility_.begin(scene::World::instance());
    // The sequence's actors are going to move: movable from the first frame, like the physics cars.
    if (!sequencePlayer_.empty()) playMobility_.seedMovable(sequencePlayer_.boundEntities());
    anim::animSystem().setObjectAnimationLive(true);
#endif
    beginPlayIfGameModeDeclared();
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS && AVER_MODULE_FRAMEWORK
    // LEVEL'S CARS BECOME REAL HERE, once there is a play session: physics vehicle per `vehicle` placement.
    // Physics only steps while PLAYING, so a project with no GameMode never starts one.
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING && level_.beginVehicles(vehicles_, content_) > 0)
        playMobility_.seedMovable(vehicles_.entities());
#endif
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    ++frames_;
    cloudTime_ += t.dt;

#if AVER_WITH_AUDIO_ABI
    // Reclaim finished voice slots. aver_audio_collect had one caller (editor); needed here to avoid leaks in shipped game.
    aver_audio_collect();
#endif

    // Profiler dump: see GameConfig::statsIntervalSec. First dump at full interval, not immediately.
    //
    if (cfg_.statsIntervalSec > 0.0f && e.device()) {
        statsTimer_ += t.dt;
        if (statsTimer_ >= cfg_.statsIntervalSec) {
            statsTimer_ = 0.0f;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            // Rough GPU-bound/CPU-bound signal: different sources, not a matched pair.
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
            logStatsVideoMemory(*e.device());
        }
    }
    // End-of-run dump: distinct from periodic one. Bounded run (--frames N) may end before statsIntervalSec elapses.
    if (cfg_.statsIntervalSec > 0.0f && cfg_.maxFrames > 0 && !statsFinalDumped_ && e.device()) {
        const u64 want = cfg_.maxFrames > 8 ? cfg_.maxFrames - 2 : cfg_.maxFrames - 1;
        if (frames_ >= want) {
            statsFinalDumped_ = true;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
            logStatsVideoMemory(*e.device());
        }
    }
    // Input is READ here, never rolled here. See onRender for why.
#if AVER_MODULE_FRAMEWORK
    // Quit is a REQUEST, not immediate exit: allows device/audio/physics to stay live. requestExit() sets flag Engine::run checks.
    if (aver_fw_quit_requested()) e.requestExit();
    // Foreground: asked once for cursor confinement and player drive input. Falls back to "a window exists" (no portable Win32 query).
    bool foreground = e.window() != nullptr;
#if defined(_WIN32)
    if (window_) {
        HWND hwnd = static_cast<HWND>(window_->nativeHandle());
        foreground = hwnd && ::GetForegroundWindow() == hwnd;
    }
#endif
    // Mouse capture: BEFORE publishInput (SandboxPlay order). Only while playing AND foreground. Never on bounded/--no-mouse-capture runs.
    {
        bool wantCapture = false;
#if defined(_WIN32)
        wantCapture = foreground && window_ && cfg_.maxFrames == 0 && !cfg_.noMouseCapture &&
                      aver_fw_play_state() == AVER_FW_PLAY_PLAYING;
#endif
        // Framework ABI: free cursor when aver_fw_cursor_requested()==1 regardless of play state.
        if (aver_fw_cursor_requested()) wantCapture = false;
        // An open menu frees the cursor so the player can reach it (retained widgets, docs/GAME_UI.md).
        if (game::uiWantsCursor()) wantCapture = false;
        setMouseCaptured(wantCapture);
        pollCapturedMouse();
    }
    // Before gameplay tick: PrePhysics actors read THIS frame's input. Bounded run keeps old answer (window opens unactivated).
    //
    const bool inputFocused = cfg_.maxFrames > 0 ? (e.window() != nullptr) : foreground;
    InputPublishPolicy inputPolicy;
    inputPolicy.focused = inputFocused;
    // All three device gates: shipped game has no panels, so "window in front" answers all at once.
    inputPolicy.keyboardToGame = inputFocused;
    inputPolicy.mouseToGame    = inputFocused;
    inputPolicy.gamepadActive  = inputFocused;
    inputPolicy.captured   = mouse_.captured();
    inputPolicy.capturedDx = mouse_.dx();
    inputPolicy.capturedDy = mouse_.dy();
    // inputPolicy.eaten stays all-false: only editor's drawer uses chords.
    // An open menu takes the keyboard, mouse and pad away from gameplay until it closes.
    game::uiGateInput(inputPolicy);
    publishInput(input_, inputPolicy, cfg_.inputEcho ? &echoHeld_ : nullptr);
    if (cfg_.inputEcho && echoHeld_ != echoLast_) {
        AVER_INFO("[Game] input: {}", echoHeld_);
        echoLast_ = echoHeld_;
    }
#endif

#if AVER_WITH_UI_ABI
    // UI frame opens before gameplay ticks. Rect is the whole window. Opened even with no gameUi_.
    {
        const u32 uiW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 uiH = e.window() ? e.window()->height() : cfg_.height;
        aver_ui_begin_frame(0.0f, 0.0f, static_cast<f32>(uiW), static_cast<f32>(uiH));
    }
    // Font lent per frame (mirrors SandboxApp). loadGameUiFont is non-fatal.
    aver_ui_set_font(&uiFont_);
    // Pointer: no offset needed. input_.mouseX()/mouseY() are already window-relative. Captured session has no pause menu.
    {
        u32 buttons = 0;
        if (input_.mouseHeld(0)) buttons |= 1u;   // left
        if (input_.mouseHeld(1)) buttons |= 2u;   // right
        if (input_.mouseHeld(2)) buttons |= 4u;   // middle
        aver_ui_set_pointer(static_cast<f32>(input_.mouseX()), static_cast<f32>(input_.mouseY()), buttons);
    }
#if AVER_MODULE_SCRIPTING
    // HUD draw call: "Draw(dt) into whatever rect aver_ui_begin_frame last established". Index 0 is first-declared [AverHud].
    if (scriptsReady_ && scripts_.hudCount() > 0) scripts_.hudDraw(0, t.dt);
#endif
    // Retained widgets: this frame's keyboard/pad/wheel in, then lay out, update, raise events, draw.
    game::uiFeedInput(input_, t.dt, !mouse_.captured());
    aver_ui_widgets_frame(t.dt);
#endif
#if AVER_MODULE_SCENE
    // Play PAUSED holds object animation. Sampled BEFORE gameplay tick, tested for PAUSED (not "not PLAYING").
    bool objectsHeld = false;
#if AVER_MODULE_FRAMEWORK
    objectsHeld = aver_fw_play_state() == AVER_FW_PLAY_PAUSED;
#endif
#endif
    tickGameplay(t.dt);
#if AVER_MODULE_FLUIDS
    // After physics step: spawns, reads solver particles, pushes player into pool. Not gated on Play.
    if (rhi::IDevice* dev = e.device()) water_.update(*dev, t.dt);
#endif
    // Beside tickGameplay, not gated on PLAYING.
    tickProjectGraphs(t.dt);
#if AVER_MODULE_SCRIPTING
    // Graph-as-class instances: gated on PLAY. Graph class declares like C# one and finds GAME_MODE bit.
    // Project with no GameMode never calls aver_fw_begin_play; gating on PLAYING alone would freeze class-placed graphs.
    // gameModeDeclared_ is latched by beginPlayIfGameModeDeclared.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING || !gameModeDeclared_) {
#endif
    scripts_.tickGraphClassInstances(t.dt);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#endif

#if AVER_MODULE_SCENE
    // Animation clock ticks UNCONDITIONALLY: hanging off gameplay gates would freeze it when no session is running.
    anim::animSystem().setObjectAnimationPaused(objectsHeld);
    game::tickAnimGraphs(t.dt);   // state machines write the pose the clip sampler would
    anim::animSystem().tick(scene::World::instance(), t.dt);
    // Level sequence: advances with the session (not while paused), writes transforms before the flush below.
    if (!sequencePlayer_.empty()) {
        if (!objectsHeld) sequencePlayer_.advance(t.dt);
        sequencePlayer_.evaluate(scene::World::instance());
    }
    // Animated placements' kinematic bodies follow poses the tick just wrote.
    if (!objectsHeld)
        world::driveKinematicBodies(scene::World::instance(), level_.animatedBodies(), t.dt);
#if AVER_MODULE_PARTICLES
    // Particle system: same unconditional reasoning.
    particles::particleSystem().tick(scene::World::instance(), t.dt);
#endif
    // After animation tick, before draw pass: creates per-entity skin targets and copies matrices from AnimSystem.
    // Before World::flush per this code's history; SkinnedScene.hpp says "after" -- disagreement noted.
    if (skinnedScene_) skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());

#if AVER_MODULE_SCENE
    // Chunk streaming: switched on after level camera settles, stepped every frame, evictions retired by flush.
    if (chunkStreamFramesLeft_ > 0 && --chunkStreamFramesLeft_ == 0) enableChunkStreaming();
    if (streaming_.enabled()) streaming_.tick(camPos_, t.dt);
#endif
    // Retires deferred destroys, rebuilds topological order, recomposes stale world matrices.
    scene::World::instance().flush();
#if AVER_MODULE_SYNAPSE_SCENE
    // AI: gated on PLAYING, like the editor's condition. Ungated when AVER_MODULE_FRAMEWORK absent.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#endif
    game::tickAi(t.dt, &gameNav_);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#endif
#endif

    // AFTER flush, BEFORE view matrix build. Reading pawn transforms before flush gives last frame's.
    drivePlayCamera();
#if AVER_MODULE_SCENE
    // A sequence's camera track takes the view while it plays (the pawn's body shows, as in a cutscene).
    if (sequencePlayer_.sequence().camera) {
        game::SeqCameraPose seqCam;
        if (sequencePlayer_.camera(seqCam)) {
            camPos_ = seqCam.position;
            yaw_ = seqCam.yaw;
            pitch_ = seqCam.pitch;
            firstPersonPawn_ = scene::kInvalidEntity;
        }
    }
#endif

    // Polled after prePass. Build takes three frames by design.
    if (cfg_.pcgVolumeTest) checkPcgVolume();

    // Physics step counter: advancing by real step count, so skip test detects jumps.
    if (physSteps_ != lastReportedSteps_ &&
        (lastReportedSteps_ == 0 || physSteps_ / 600 != lastReportedSteps_ / 600)) {
        AVER_INFO("[Game] physics: {} step(s) taken", physSteps_);
        lastReportedSteps_ = physSteps_;
    }

#if AVER_MODULE_VOXI
    // Frame budget: shared controller seeded from project_.frameBudgetMs in openProject.
    // Called right before pushFrame so voxiRenderer_ holds this frame's rung.
    // Never on bounded (--frames N) run: automated capture/gate must not retune quality.
    if (voxiAttached_) {
        voxi::Settings vs = voxi::Renderer::get().settings();
        if (cfg_.maxFrames == 0 && voxi::frameBudgetTick(frameBudget_, t.dt, vs)) {
            AVER_INFO("[Game] frame budget {:.1f}ms: {:.1f}ms average -> rung {} "
                      "(GI every {} frame(s), {} cone(s))",
                      frameBudget_.budgetMs, frameBudget_.avgMs, frameBudget_.rung,
                      vs.giUpdateInterval, vs.giCones);
        }
        voxiRenderer_.setSettings(vs);
    }
#endif

    // MUST BE LAST: Engine::frameStep does onUpdate -> beginFrame -> onRender -> endFrame.
    // beginFrame takes ONE snapshot of PerFrameCB. setCamera/setLight/setSkyAtmosphere touch CPU shadow copy.
    // Calling from onRender would render frame N with frame N-1 constants and culling divergence.
    pushFrame(e);
}

#if AVER_WITH_UI_ABI
void GameApp::submitGameUi(Engine&) {
    if (!gameUi_) return;

    const auto* dl = static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list());
    if (!dl) return;
    gameUi_->submit(*dl);
}
#endif

void GameApp::onRender(Engine& e) {
#if AVER_MODULE_SCENE
    // First pixels: pushFrame set camera before beginFrame uploaded frame constants.
    if (rhi::IDevice* dev = e.device()) {
        pbr::MaterialSystem* ms = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#endif
#if AVER_MODULE_VOXI
        // CDecal entities -> Voxi's projected-decal list. A decal's image id resolves through the project's
        // content index, like every other asset.
        if (voxiAttached_)
            sceneDecalFeed_.update(scene::World::instance(), voxiRenderer_,
                                   [this](u64 id, voxi::DecalImageKind, ImageData& out) {
                                       const std::string full = content_.pathFor(id);
                                       std::string err;
                                       return !full.empty() && decodeImage(full, out, &err) && out.valid();
                                   });
#endif
#if AVER_MODULE_FLUIDS
        // Simulated fluid volumes, drawn first (editor's pattern).
        water_.draw(*dev, content_, ms);
#endif
#if AVER_MODULE_LANDSCAPE
        // Terrain before entities. LOD scale uses window height (whole backbuffer).
        if (landscape_.loaded()) {
            landscape_.updateRingTiles(dev, eye_.x, eye_.y, &content_, ms);
            const u32 viewH = e.window() ? e.window()->height() : cfg_.height;
            landscape_.draw(*dev, eye_, viewProj_, static_cast<f32>(viewH), &content_, ms);
        }
#endif
        // Owner-hide: hides possessed first-person pawn's body from raster pass. firstPersonPawn_ set every frame by drivePlayCamera().
        DrawWorldOptions opts;
        opts.ownerHideRoot = firstPersonPawn_;
        // Possessed pawn's tree is movable from first frame; everything else by PlayMobility.
        {
            scene::Entity movableRoot = scene::kInvalidEntity;
#if AVER_MODULE_FRAMEWORK
            const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
            if (pn > 0) movableRoot = static_cast<scene::Entity>(static_cast<u32>(pn));
#endif
            playMobility_.beginFrame(movableRoot);
            opts.mobility = &playMobility_;
        }
        if (!sequencePlayer_.empty()) {
            opts.emissiveScale = &GameApp::sequenceEmissiveScale;
            opts.user = this;
        }
#if AVER_MODULE_VOXI
        // Culled and owner-hidden entities reach Voxi too, so off-screen casters keep shadow and GI.
        if (voxiAttached_) opts.voxiRenderer = &voxiRenderer_;
#endif
        drawWorld(*dev, viewProj_, content_, drawStats_, ms, skinnedScene_.get(), opts);
    }
#endif
#if AVER_WITH_UI_ABI
    // Before screenshot request, so capture includes the HUD.
    submitGameUi(e);
#endif
    captureScreenshotIfDue(e);

    // Rolling input edges: last thing onUpdate does. Engine::run pumps window at loop top, so a key pressed this frame is in InputState by onUpdate.
    // Calling at onUpdate start would throw away edges; calling here after last reader leaves accumulator empty for next pumpEvents.
    input_.newFrame();
}

void GameApp::captureScreenshotIfDue(Engine& e) {
    if (cfg_.screenshotPath.empty() || screenshotDone_ || cfg_.maxFrames == 0) return;
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // requestCapture/getFrameImage are request/poll pair; both cannot happen same frame.
    const u64 f = e.time().frame;
    const u64 sf = cfg_.maxFrames > 8 ? cfg_.maxFrames - 3 : (cfg_.maxFrames > 1 ? cfg_.maxFrames - 1 : 0);
    if (f == sf) { dev->requestCapture(0, 0); return; }
    if (f <= sf) return;

    std::vector<u8> img; u32 iw = 0, ih = 0;
    if (dev->getFrameImage(img, iw, ih) && iw && ih &&
        stbi_write_png(cfg_.screenshotPath.c_str(), static_cast<int>(iw), static_cast<int>(ih), 4,
                       img.data(), static_cast<int>(iw) * 4)) {
        AVER_INFO("[Game] screenshot: {} ({}x{})", cfg_.screenshotPath, iw, ih);
        screenshotDone_ = true;
    }
}

void GameApp::onShutdown(Engine& e) {
    // Release cursor first: an OS cursor left hidden/clipped outlives this process and annoys the user.
    setMouseCaptured(false);
    // Exact reverse registration order: device holds bare pointers to render features.
    // voxiRenderer_ is a MEMBER by value, so it outlives the device if unregistered first.
    rhi::IDevice* dev = e.device();
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    if (crowdGpuAttached_) { game::removeCrowdGpu(dev, crowdGpu_); crowdGpuAttached_ = false; }
#endif
    // Detach before destroy: device holds raw pointer into averSrUpscaler_. Both calls no-op when never installed.
    if (dev) dev->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    // Frame generator: same detach-then-destroy order.
    if (dev) { dev->setFrameInterpolation(false); dev->setFrameInterpolator(nullptr); }
    frameInterpolator_.reset();
    if (dev && pcgAttached_) { dev->removeRenderFeature(&pcgVolume_); pcgAttached_ = false; }
    pcgVolume_.shutdown();
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered last, so removed first.
    if (dev && particlesAttached_) {
        dev->removeRenderFeature(&particleRenderer_);
        particlesAttached_ = false;
    }
    particleRenderer_.shutdown();
#endif
#if AVER_WITH_UI_ABI
    // Font out first: ui_abi.h comment on aver_ui_set_font: a font outliving its atlas textures draws glyphs sampling dead descriptor.
    aver_ui_set_font(nullptr);
    if (dev && uiFontTexture_) {
        // waitIdle: destroying a texture a still-in-flight command list references is GPU use-after-free.
        if (rhi::IResourceFactory* res = dev->resources()) {
            res->waitIdle();
            res->destroyTexture(uiFontTexture_);
        }
        uiFontTexture_ = 0;
    }
    // Registered after Voxi, before particles; removed after particles, before Voxi.
    if (dev && gameUi_) {
        dev->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
#endif
#if AVER_MODULE_SCENE
    // Reverse order: skinning first, last to come out. Removed before device.
    if (dev && skinnedScene_) dev->removeRenderFeature(skinnedScene_.get());
#endif
#if AVER_MODULE_VOXI
    if (dev && voxiAttached_) {
        dev->removeRenderFeature(&voxiRenderer_);
        voxiAttached_ = false;
    }
    voxiRenderer_.shutdown();
#endif
#if AVER_MODULE_PBR
    // After Voxi teardown: material system lives inside VoxiRenderer. Dropping materials while registered would leave freed textures.
    content_.releaseProjectMaterials();
    content_.setTextureFactory(nullptr);
#endif
#if AVER_MODULE_SCENE
    skinnedScene_.reset();
    // Streamed chunks own entities and static bodies. Released here while physics exists (unlike editor, which waits for destructors).
    streaming_.disable();
    // Before physics: unloading destroys entities and removes static bodies. Object animation stops first so nothing moves as level comes down.
    anim::animSystem().setObjectAnimationLive(false);
    playMobility_.end();
#if AVER_MODULE_PHYSICS
    // Cars' bodies and constraints go before the level.
    vehicles_.end();
#endif
    level_.unload();
#endif
#if AVER_MODULE_FLUIDS
    // After level's volumes were despawned, before physics: retiring live volume calls into solver.
    water_.shutdown(dev);
#endif
#if AVER_MODULE_PHYSICS
    aver_phys_shutdown();
#endif
#if AVER_WITH_AUDIO_ABI
    // Stops mixer, releases device, forgets every sound. Idempotent. 0 is not an error (no output device).
    aver_audio_shutdown();
#endif
#if AVER_MODULE_SCRIPTING
    // Drains graphs/behaviors and closes CLR host. Ordering against physics/scene shutdown not load-bearing.
    if (scriptsReady_) { scripts_.shutdown(); scriptsReady_ = false; }
#endif

    // Reported unconditionally, even when zero: silent zero is indistinguishable from broken counter.
    setFileTrace(nullptr, nullptr);
    AVER_INFO("[Game] shutdown after {} frame(s), {} physics step(s)", frames_, physSteps_);
    (void)dev;
}

} // namespace aver::game
