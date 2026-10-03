#include "aver/game/GameApp.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"

#include <filesystem>

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"
// Shared GPU-timing tree formatter -- see --stats in GameConfig, and the header's own note on
// why it moved out of the editor console.
#include "aver/rhi/GpuTimingFormat.hpp"

// --screenshot (captureScreenshotIfDue). A second STB_IMAGE_WRITE_IMPLEMENTATION is fine (precedent:
// tests/formats/CMakeLists.txt's MakeFoliage target): AverEngineRuntime.exe and Sandbox.exe are separate
// binaries, so there's no symbol collision, unlike Image.cpp's STB_IMAGE_IMPLEMENTATION which every
// module links into both and may define only once.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_WITH_UI_ABI
#  include "aver/ui/ui_abi.h"
// aver::ui::parseOcfont, for loadGameUiFont below (same .ocfont grammar SandboxApp.cpp parses).
#  include "aver/ui/UiFont.hpp"
// decodeImage/ImageData: the .ocfont names a PNG beside it that this host decodes and uploads itself.
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
// spawnParticleTestContent (below) needs the component types, aver_scene_material and fnv1a64 directly --
// everything else here reaches the scene only through GameContent/GameLevel/GameRender.
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/core/Hash.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
// Divergence census both hosts print, so "the game draws what the editor draws" is checked, not
// assumed. Header-only; see SceneCensus.hpp for why a census and not a pixel diff.
#include "aver/world/SceneCensus.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/Voxi.hpp"
// Shared, header-only two-phase manifest apply (Lane 2 of the settings-separation pass) -- see
// attachVoxi/applyProjectRenderSettings below. Also pulls in RenderSettingsResolver.hpp (voxi::resolve,
// used by pushFrame's G-buffer switch).
#  include "aver/voxi/ProjectRenderApply.hpp"
// resolveAverSrLevel/autoAverSrLevel/AverSrDecision/AverSrSource (3.3 C, contract C2-12), for onInit's
// AverSR block below. Also pulls in Voxi.hpp itself via RenderSettingsResolver.hpp -- already included
// above, so no new dependency, only new names.
#  include "aver/voxi/Scalability.hpp"
#endif
#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// DECIDED 4's GI seam glue (particleGiPrepare/particleGiBind below) is the only reason this TU needs
// Voxi's borrowed-HLSL header -- the narrow slice of Voxi's HLSL a foreign pipeline may borrow
// (VoxiGiShaders.hpp). Guarded identically to the two functions that use it, and nothing else here.
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
// installLevelHooks' afterInstantiate loads the level's instanced foliage once placements exist --
// see the header for the contract three packages implement parts of.
#  include "aver/game/GameFoliage.hpp"
#endif
#if AVER_MODULE_PBR
// setTextureCacheDir, called from openProject before any material/texture load -- see its own call
// site below.
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

// True when an .ocgraph file's text carries a top-level record with this key, and when it does, sets
// `*outValue` to the token after the key (empty when the record names nothing).
//
// A lightweight text scan, not a parse -- still right even though the real C++ reader has since
// learned one of these two records (DOMAIN): pulling Aver.Formats into this module's link line to read
// one token off a header would be a dependency bought for two string comparisons. CLASS/PARAM/VAR
// all ride through OcGraph.cpp's classifyLine as OwnedLineKind::Other by design, so the real reader
// will never learn CLASS either. Matches OcGraphParser's "'#' starts a comment only at line start"
// rule, close enough for the yes/no purposes both callers below need.
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

// True when an .ocgraph carries a DOMAIN record naming anything other than gameplay (see
// aver::fmt::OcGraphDomain). An ABSENT record means gameplay (every graph predates the record); an
// unrecognised one does not, since naming an unknown domain says out loud it isn't a gameplay graph.
bool ocgraphIsForeign(const std::string& text) {
    std::string domain;
    if (!ocgraphRecord(text, "DOMAIN", &domain) || domain.empty()) return false;
    return !equalsAsciiCI(domain, "gameplay");
}

// True for a path ending in ".ocproject", case-insensitively. Lifted from the editor's copy
// (sandbox/src/SandboxMain.cpp:26, via that file's hasExtension helper).
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

// True for a path ending in ".ocworld" or ".ocmap", case-insensitively (editor's isLevelFile). The
// extension only decides whether to try: GameLevel::load tells a legacy .ocmap from OCWORLD content
// by what the file contains.
bool isLevelFile(const char* p) {
    const std::string_view s(p);
    const auto endsWithCI = [&](std::string_view ext) {
        return s.size() > ext.size() && equalsAsciiCI(s.substr(s.size() - ext.size()), ext);
    };
    return endsWithCI(".ocworld") || endsWithCI(".ocmap");
}

// The .ocproject that owns `mapPath`, found by walking up at most eight directories, or empty
// (editor's ownerProjectOf). A level's placements name content relative to its project, so a level
// opened without one resolves none of them.
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
// [AverSR] log naming (3.3 C): supplies the {level} and {source} words for "[AverSR] {level} ({source}):
// scene {}x{} -> present {}x{}" below (C2-10's fixed shape). Typed out locally rather than borrowed from
// aver::sr::qualityName's identical table, since this file must not include AverSrQuality.hpp
// (Scalability.hpp's module boundary). Not a second source of truth for the numbering itself:
// `level` is the ladder's own kAverSrOff..kAverSrPerformance (QualityLadder.hpp, 3.2), cross-checked
// against aver::sr::Quality by RuntimeMain.cpp's static_asserts under AVER_MODULE_SR.
const char* averSrLevelName(u32 level) {
    switch (level) {
        case voxi::ladder::kAverSrOff:         return "Off";
        case voxi::ladder::kAverSrQuality:     return "Quality";
        case voxi::ladder::kAverSrBalanced:    return "Balanced";
        case voxi::ladder::kAverSrPerformance: return "Performance";
        default:                               return "Off";
    }
}

// Mirrors the editor's Project Settings source vocabulary (3.3 A) for the subset a packaged game can
// produce. onInit always passes userLevel=-1 (no Display page to prefer from -- U2's "the packaged
// game uses the same chain minus the user choice") and never forces a crash cookie, so User and
// ForcedOff are named here defensively but should never actually occur.
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
        // --scene-census: prints one canonical line describing what the level put in the world, exit-safe
        // either way. scripts/verify-game.ps1 asks both hosts for it and compares; Sandbox.exe prints the same format.
        else if (std::strcmp(a, "--scene-census") == 0) { c.sceneCensus = true; }
        // --stats [seconds]: interval is optional. Only consumed when it parses as a number, or
        // "--stats --headless" would silently eat the next flag and run headless for a reason nobody could see.
        else if (std::strcmp(a, "--stats") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            const f32 secs = v ? static_cast<f32>(std::atof(v)) : 0.0f;
            if (secs > 0.0f) { c.statsIntervalSec = secs; ++i; }
            else             { c.statsIntervalSec = 5.0f; }
        }
        else if (std::strcmp(a, "--pcg-volume-test") == 0) { c.pcgVolumeTest = true; }
        // --chunk-stream [N]: optional N is taken only when the next argument is not a flag, as in the editor's parser.
        else if (std::strcmp(a, "--chunk-stream") == 0) {
            c.chunkStreamAutoFrames = (i + 1 < argc && argv[i + 1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (std::strcmp(a, "--no-chunk-stream") == 0) { c.chunkStreamAutoFrames = 0; }
        else if (std::strcmp(a, "--no-particle-gi") == 0)  { c.noParticleGi = true; }
        else if (std::strcmp(a, "--particle-test") == 0)   { c.particleTest = true; }
        else if (std::strcmp(a, "--screenshot") == 0)      { c.screenshotPath = valueAfter(argc, argv, i, ""); ++i; }
        // --no-vsync (M7): measurement parity with SandboxApp.cpp's identical flag -- see GameConfig::vsyncOff.
        else if (std::strcmp(a, "--no-vsync") == 0) { c.vsyncOff = true; }
        // See GameConfig::noMouseCapture's own comment.
        else if (std::strcmp(a, "--no-mouse-capture") == 0) { c.noMouseCapture = true; }
        // --cam-wobble DEG PERIOD: same measurement-only yaw swing as SandboxApp.cpp's --cam-wobble
        // (GameConfig::camWobbleDeg/camWobblePeriod). Both values are consumed only when both are
        // present; a lone or half-given --cam-wobble leaves the config untouched rather than eating
        // whatever argument follows as if it were the period.
        else if (std::strcmp(a, "--cam-wobble") == 0 && i + 2 < argc) {
            c.camWobbleDeg = static_cast<f32>(std::atof(argv[i + 1]));
            const int period = std::atoi(argv[i + 2]);
            c.camWobblePeriod = period > 0 ? static_cast<u32>(period) : 0u;
            i += 2;
        }
        // --aversr off|quality|balanced|performance|auto: parsed locally into a raw int rather than
        // through aver::sr::parseQuality (see GameConfig::averSrArg). equalsAsciiCI is the same comparator
        // ocgraphRecord/ocgraphDeclaresClass use above; v is null-guarded like this function's other
        // valueAfter callers. Unrecognised value is ignored, matching this parser's "bad/foreign argument
        // is never fatal" convention.
        else if (std::strcmp(a, "--aversr") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if      (v && equalsAsciiCI(v, "off"))         { c.averSrArg = 0;  ++i; }
            else if (v && equalsAsciiCI(v, "quality"))     { c.averSrArg = 1;  ++i; }
            else if (v && equalsAsciiCI(v, "balanced"))    { c.averSrArg = 2;  ++i; }
            else if (v && equalsAsciiCI(v, "performance")) { c.averSrArg = 3;  ++i; }
            else if (v && equalsAsciiCI(v, "auto"))        { c.averSrArg = -1; ++i; }
        }
        // --frame-interp 0|1: frame interpolation on/off over the project's RENDER.FRAMEINTERP.
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
        // A bare .ocproject path is the project, so double-clicking or dropping it on the exe works. A
        // packaged game launched with no arguments at all finds its manifest in its own directory
        // instead -- see openProject.
        else if (isOcproject(a))                       { c.projectPath = a; }
        // A bare level path opens that level instead of the start map -- see GameConfig::levelPath.
        else if (isLevelFile(a))                       { c.levelPath = a; }
        // Anything else is deliberately ignored: see the header.
    }
    // A level named on its own brings its project with it, as in the editor's createApplication. An
    // explicit .ocproject argument still wins.
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

    // ---- WINDOW.* from the manifest, read HERE and not in onInit ------------------------------
    // The window is created from this BootConfig before onInit runs, so title/resolution/fullscreen
    // must be known now -- hence the manifest is read twice: here for WINDOW.TITLE/SIZE/FULLSCREEN
    // (+NAME as title fallback), and again in openProject for everything else. Reading a small text
    // file twice is cheaper than a window resized or re-created borderless after it's on screen. Same
    // manifest lookup order as openProject: explicit path, else Game.ocproject beside the executable
    // (what stage-game.ps1 writes for a packaged game launched with no arguments).
    //
    // Command line still wins (standing rule): --width/--height/--title override the manifest; only
    // an unstated value falls through to the project.
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
                    // WINDOW.TITLE, else NAME: a shipped game must not show the engine's default title.
                    windowTitleOwned_ = !d.windowTitle.empty() ? d.windowTitle : d.name;
                    if (!windowTitleOwned_.empty()) b.windowTitle = windowTitleOwned_.c_str();
                }
                if (cfg_.width  == GameConfig::kDefaultWidth  && d.windowWidth  > 0) b.windowWidth  = static_cast<u32>(d.windowWidth);
                if (cfg_.height == GameConfig::kDefaultHeight && d.windowHeight > 0) b.windowHeight = static_cast<u32>(d.windowHeight);
                // WINDOW.FULLSCREEN: fullscreen is a window-creation-time decision (WindowDesc::
                // fullscreen -> Win32Window::create), not something to redo once up. A
                // non-interactive capture run is unaffected: WindowDesc ignores fullscreen when
                // activate is false, so a measured run keeps the exact window its baselines were
                // recorded through. No `cfg_ == default` guard, unlike title/size: no --fullscreen
                // flag exists to outrank, and -1 means "unstated" (whoever adds that flag owes this
                // line the same command-line-wins shape as title/size). Before this, the key was
                // parsed and editable in Project Settings while no host read it -- a "declared but
                // unread" author could tick Fullscreen, save, ship, and the game started windowed
                // with nothing logged.
                if (d.windowFullscreen >= 0) b.fullscreen = d.windowFullscreen != 0;
                // WINDOW.RESIZABLE: used to be the one window key nothing read (BootConfig had no
                // member for it); this is the third line closing that gap. Same shape as FULLSCREEN
                // above (BootConfig::resizable, and `wd.resizable = cfg.resizable` in Engine.cpp). No
                // --resizable flag exists, so -1 means unstated; whoever adds that flag owes both keys
                // the same command-line-wins test.
                if (d.windowResizable >= 0) b.resizable = d.windowResizable != 0;
            }
        }
    }
    return b;
}

void GameApp::initPhysics() {
#if AVER_MODULE_PHYSICS
    // No implicit ground: a level supplies its own collision. GameLevel::load already adds one
    // static body per colliding PLACE (a 100 m shared z=0 floor used to be added here, hiding pits,
    // chasms and water below a level's own floor; docs/GAME-LIFT.md flagged it as worth dropping when
    // first lifted from the editor -- this is that).
    game::startPhysics("Game");
#endif
}

void GameApp::tickGameplay(f32 dt) {
#if AVER_MODULE_FRAMEWORK
    // Gated on PLAYING only, not PAUSED: playSessionActive() counts PAUSED as active, which is right
    // for "is a session open" but wrong for "should the world advance". (The editor widens this
    // gate with a --spawn-test term; that's a CLI harness with no place in a game.)
    if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;

#if AVER_MODULE_PHYSICS
#if AVER_MODULE_SCENE
    // The cars' drivers decide this frame's throttle, brake and steering immediately before the step
    // that uses them, and their entities are written from the bodies immediately after it -- both
    // inside the gate above, so a paused session freezes traffic with everything else. The focus is
    // the camera: the cars far from it think less often (VehicleSystem's own LOD).
    vehicles_.prePhysics(dt, camPos_);
#endif
    // += the step count, not ++ on a flag: tickGameplayGroups returns how many fixed steps
    // aver_phys_step actually ran (a frame can run several, via the up-to-8-step accumulator, or
    // none). Used to hardcode true, so this counted frames that ticked and logged that count as
    // "physics step(s)".
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

    // Same spec PcgMirrorTest uses, so the C++/F#/HLSL comparison is over one set of numbers, not
    // three that happen to look similar.
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

    // Every voxel, not a sample: 32768 comparisons cost nothing, and a sample could miss a shader
    // right on the diagonal and wrong off it.
    //
    // Tolerance: measured GPU/CPU agreement is within 1.19e-07 (exactly 2^-23, one ULP) on every
    // differing voxel, even with coverageBias=1.0 (rules out pow()) -- floating-point contraction, DXC
    // fusing the fBm multiply-add where MSVC's /fp:precise does not; the integer hash agrees
    // bit-for-bit. 1e-5 is four orders above that and four below any real logic error (a transposed
    // axis, wrong seed offset or dropped octave moves density by 0.1-1.0). Worst difference is
    // reported every run so the number can't quietly grow.
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
    // init compiles HLSL at runtime, so this can fail on a green build. Failure leaves
    // skinnedScene_ null (legal): skinned entities draw at rest pose rather than not at all -- a
    // character that fails to skin must still appear.
    if (!scene->init(*dev)) {
        AVER_WARN("[Game] skinning unavailable; skinned entities will draw at rest");
        return;
    }
    // Same tables the draw walk reads: a skin target built from a different upload than what's on
    // screen would be a rig skinning geometry nobody can see.
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
    // The denoiser needs the G-buffer, which only D3D12 has; derived from the device, not read off
    // caps, so no module guard needed here. rhi::DeviceCaps itself is untouched.
    di.denoiserSupported = dev->backend() == rhi::Backend::D3D12;
    voxi::Renderer::get().setDeviceInfo(di);

    // Seed the manifest's render settings BEFORE init() (N2): createVoxelVolume(settings_.voxelResolution)
    // is the only place the volume is ever sized, so without this RENDER.VOXELRES 512 shipped a
    // grid 64x too small (init() had already built the default 128^3 grid). Uses a LOCAL
    // fmt::ProjectDesc, not project_ (not populated yet); openProject's later
    // applyProjectRenderSettings call re-applies the same file through project_, idempotently.
    // Also fixes N10: layeredBsdf, read once before raster PSOs are built (Settings::layeredBsdf): the
    // unseeded path used to latch it Off regardless of RENDER.LAYEREDBSDF, warning about a latch it
    // could never resolve, on every launch.
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
    // Only when RENDER.MSAA is unstated (-1 sentinel): the seed above already applied an explicit
    // manifest MSAA, and this device-derived fallback must not un-pin it.
    if (manifestMsaa < 0) s.msaa = static_cast<voxi::Msaa>(dev->sampleCount());
    voxi::Renderer::get().setSettings(s);
    voxiRenderer_.setSettings(s);

    if (voxiRenderer_.init(*dev)) {
        dev->addRenderFeature(&voxiRenderer_);
        voxiAttached_ = true;
#if AVER_MODULE_PBR
        // These two lines stay adjacent (as in SandboxApp): the resolver is installed in the same
        // breath as the factory it depends on, so the !textureFactory_ guard in
        // resolveMaterialTexture stays unreachable rather than merely unlikely.
        content_.setTextureFactory(dev->resources());
        voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_);
#endif
        // Depth proxy resolver for LOD-based shadow/voxel optimization. Installed unconditionally
        // even without Trifactor linked: depthProxyMap is then empty and every lookup answers 0.
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
// The HUD's render feature: an overlay pass, so ordering against the scene features is free.
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

// The game UI's own font load path: SandboxApp.cpp's loadGameUiFont can't be reused here since it
// uploads through Sandbox.exe's own device, and this executable opens a separate one. Only the file
// format (.ocfont via aver::ui::parseOcfont) and staging convention (beside the exe) are shared.
//
// Non-fatal on every failure branch below, matching the editor's contract: a missing .ocfont, a
// parse error, or an atlas that won't decode all leave uiFont_ default or fontless (never with a
// garbage atlasTexture), and the game keeps running -- a HUD with no labels beats a worse boot.
void GameApp::loadGameUiFont(Engine& e) {
    rhi::IDevice* dev = e.device();
    rhi::IResourceFactory* res = dev ? dev->resources() : nullptr;
    if (!res) return;   // headless, or a device that never came up -- nothing to upload into

    const std::string fontPath = executableDir() + "\\Roboto-Regular.ocfont";
    std::string text;
    if (!readFileText(fontPath, text)) {
        // Not a warning: shipping no HUD font is a legal, common shape (same INFO level as the
        // editor's copy of this function).
        AVER_INFO("[Game] no game-UI font at {} -- the HUD draws without text", fontPath);
        return;
    }
    std::string why;
    if (!ui::parseOcfont(text, uiFont_, &why)) {
        AVER_WARN("[Game] '{}': {}", fontPath, why);
        return;
    }

    // The atlas sits beside the .ocfont; only the filename survives past the last slash --
    // uiFont_.atlasPath is content-relative, while the staged copy beside this exe is flat.
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
    // Raw TextureHandle, not a UI descriptor id: UiDrawCmd::texture is cast straight back to an
    // rhi::TextureHandle by UiRenderer.
    uiFont_.atlasTexture = static_cast<u64>(uiFontTexture_);
    AVER_INFO("[Game] game-UI font '{}' loaded: {} glyph(s), atlas {}x{}",
              uiFont_.name, uiFont_.glyphs.size(), img.width, img.height);
}
#endif

void GameApp::attachParticles(Engine& e) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // Process-global singletons, matching anim::animSystem() above: without this the system never
    // resolves CParticleEmitter::effect and every tick silently spawns nothing (ParticleSystem::tick's
    // early-out).
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*dev)) {
        particleRenderer_.setSystem(&particles::particleSystem());
        dev->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // DECIDED 4's seam, installed only once Voxi has actually attached this run (attachVoxi runs
        // before attachParticles -- see onInit's call order) and unless --no-particle-gi asked for the
        // A/B comparison. See particleGiPrepare/particleGiBind below for the contract; particleRenderer_
        // never learns Voxi's name.
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
    // Standalone by design, matching SandboxApp.cpp's --particle-test: no project required.
    // registerBuiltins is normally openProject's job; guarded on the lookup (not called
    // unconditionally) so a run that DOES have a project doesn't create a second orphaned GPU mesh.
    const u64 cubeId = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    if (!content_.meshFor(cubeId)) content_.registerBuiltins(device);

    scene::World& world = scene::World::instance();

    // Same occluder, dust cloud and ember burst as SandboxApp.cpp's --particle-test (identical
    // positions/effect data), rebuilt over this executable's ECS entity + CMeshRenderer path rather
    // than the editor's objects_ list. A screenshot from each executable is then evidence the SAME
    // scene renders correctly twice, not two scenes that happen to look similar.
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
            // A builtin surface token registerBuiltins already interned (GameContent.cpp) -- crate-
            // brown, metallic 0.02, roughness 0.78. Not invented for this test: reusing an existing
            // named surface keeps this from becoming its own little material feature.
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

    // DECIDED 4's proof of the opposite half of the seam: receivesGI = false, additive, well clear of
    // the dust cloud's footprint so the two never overlap in one screenshot; sits in open sky with
    // nothing opaque behind it (see SandboxApp.cpp's own comment on why).
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
    emberFx.receivesGI = false;   // an ember is its own light source (particles DECIDED 4)
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

    // Same camera as SandboxApp.cpp's --particle-test (`--cam 0 0 50 0 0`): camForward() composes
    // {cosP cosY, cosP sinY, sinP}, so yaw=pitch=0 looks down +X, directly at the occluder and both
    // emitters above.
    camPos_ = Vec3{0.0f, 0.0f, 50.0f};
    yaw_ = 0.0f;
    pitch_ = 0.0f;
    AVER_INFO("[Game] --particle-test: camera set to (0,0,50) looking down +X");
}
#endif

void GameApp::installLevelHooks(Engine& e) {
#if AVER_MODULE_SCENE
    GameLevel::LoadHooks hooks;
    // Water, then terrain, the order the editor's loadLevel applies them in -- and unloads them in.
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
#  if AVER_MODULE_FLUIDS
        water_.unload();
#  endif
#  if AVER_MODULE_LANDSCAPE
        // A level with no terrain must clear the last level's.
        landscape_.unload(e.device());
#  endif
#  if AVER_MODULE_VOXI
        // FOLIAGE LIVES OUTSIDE THE ENTITY LIST ENTIRELY (GameFoliage.hpp's own header comment), so
        // nothing else here touches it -- the next level's afterInstantiate replaces it, but a level
        // that fails to load anything at all must not leave the previous one's trees standing.
        if (voxiAttached_) voxiRenderer_.clearFoliage();
#  endif
        (void)e;
    };
    // STAGED PROGRESS (GameLevel::LoadHooks::progress) onto the engine's startup splash: its status
    // line and its bar.
    hooks.progress = [&e](const std::string& stage, f32 fraction) {
        e.setLoadingStatus(stage);
        e.setLoadingProgress(fraction);
    };
    hooks.afterInstantiate = [this](const GameLevel::LoadedLevel& loaded) {
#  if AVER_MODULE_VOXI
        // FOLIAGE, AFTER PLACEMENTS -- mirrors sandbox/src/SandboxLevelLoad.cpp's identical call so
        // both hosts load a level's instanced foliage the same way.
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
    // New terrain under a live stream restarts it, so scatter regenerates on the new surface --
    // SandboxApp::applyLandscapeToStreaming.
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
    // A packaged game launches with no arguments; stage-game.ps1 writes Game.ocproject beside
    // AverEngineRuntime.exe. Look beside the EXECUTABLE, never the working directory -- a shortcut,
    // store client or Explorer double-click can set cwd to anything (verify-game.ps1 runs the
    // package with cwd=C:\ for this reason).
    std::string path = cfg_.projectPath;
    if (path.empty()) {
        const std::string beside = executableDir() + "\\Game.ocproject";
        if (fileExists(beside)) { path = beside; }
    }
    if (path.empty()) {
        AVER_INFO("[Game] no project: pass one on the command line, or ship a Game.ocproject beside the executable");
#if AVER_MODULE_SCENE
        // A level named on the command line still opens with no project above it (mirrors
        // SandboxApp::onInit): placements won't resolve with no content index, but the level's
        // shape is better than an empty world.
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
    // BEFORE ANY MATERIAL OR TEXTURE LOAD -- content_.adopt/registerBuiltins/loadProjectMeshes and
    // level_.loadStartMap below are exactly that, so this has to run first or the cache dir changes
    // out from under textures this same open already decoded.
    assets::setTextureCacheDir(project_.dir + "\\Saved\\DerivedDataCache\\Textures");
#endif
    if (project_.startMap.empty()) {
        AVER_WARN("[Game] the manifest names no STARTMAP, so there is no level to open");
    }
#if AVER_MODULE_VOXI
    // RENDER.FRAMEBUDGETMS read early, decoupled from hasRenderSettings()/voxiAttached_ (mirrors
    // SandboxApp::applyProject in SandboxProject.cpp) so a project stating only this key still gets
    // the controller. <= 0 (manifest default) leaves frameBudget_ off (FrameBudgetState::budgetMs).
    frameBudget_.budgetMs = project_.frameBudgetMs;
#endif
    // Order is load-bearing, same as applyProject: the index must precede the meshes (mesh walk
    // resolves through it), and meshes must precede any level (CMeshRenderer resolves through the
    // mesh table).
    content_.adopt(project_);
#if AVER_MODULE_SCENE
    if (rhi::IDevice* dev = e.device()) {
        content_.registerBuiltins(*dev);
        content_.loadProjectMeshes(*dev);
    }
#if AVER_MODULE_PARTICLES
    // Same order rule as the mesh load above: a placed CParticleEmitter's effect id must resolve
    // before anything reads it. Needs no device -- an effect is CPU-only data (particles DECIDED 2)
    // -- so it runs regardless of whether e.device() succeeded above.
    content_.loadProjectParticleEffects();
#endif
    level_.loadStartMap(project_, content_, cfg_.levelPath);
#if AVER_MODULE_SYNAPSE_SCENE
    // Optional, silently: most levels have no baked navigation, loadOcNav's failure path leaves
    // gameNav_ empty (OcNavData::valid()==false), and AgentSystem::tick treats that as "no grid
    // yet" -- an agent waits rather than failing. Logged at INFO, not WARN, since most projects
    // have no navigation at all and never asked for the feature (an earlier draft of this comment
    // called it WARN-worthy; no branch below ever has been).
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
    // RENDER.GIVOLUME, when the manifest authors one, wins over the auto-fit below -- mirrors the
    // editor's precedence exactly (`project_.giExtent <= 0.0f` guards the fit call). Without this
    // guard, fitGiVolumeToLevel() ran unconditionally and silently discarded any hand-authored GI
    // volume on every launch -- see docs/RUNTIME-DEDUP.md's C4 slice.
    if (project_.hasGiVolume) {
        giCenter_ = Vec3{project_.giCenter[0], project_.giCenter[1], project_.giCenter[2]};
        giExtent_ = project_.giExtent;
        AVER_INFO("[Voxi] GI volume from the manifest: centre[{:.0f} {:.0f} {:.0f}] extent {:.0f}cm",
                  giCenter_.x, giCenter_.y, giCenter_.z, giExtent_);
    }
    if (project_.giExtent <= 0.0f) fitGiVolumeToLevel();
#endif
    applyProjectRenderSettings();
    // RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE/TONEMAP are per-device post state, not Voxi settings, so
    // applied unconditionally here rather than inside the call above -- same decoupling as
    // PHYSICS.*/AUDIO.* below. See applyProjectPostSettings' own comment.
    applyProjectPostSettings();
#if AVER_MODULE_PHYSICS
    // PHYSICS.GRAVITY/FIXEDSTEP from the manifest, applied unconditionally as the project opens.
    // Deliberately NOT behind applyProjectRenderSettings' AVER_MODULE_VOXI/hasRenderSettings()/
    // voxiAttached_ guards, unlike the editor used to -- a project stating no RENDER.* key must
    // still get these.
    game::applyProjectPhysics(project_);
#endif
#if AVER_WITH_AUDIO_ABI
    // AUDIO.* from the manifest -- the third such decoupled read, after FRAMEBUDGETMS and
    // PHYSICS.*: without this the master/bus volumes stay at defaults, so a project shipping a
    // quiet mix shipped a loud game.
    game::applyProjectAudioMix(project_);
#endif
}

#if AVER_MODULE_SCRIPTING
// The one thing about a project that survives packaging unchanged is its NAME -- a shipped game has
// no project directory to key a per-project settings path by (stage-game.ps1 lays the manifest and
// Binaries directly under the install root). Windows-reserved characters become '_' ('/' and '\\'
// matter on every platform); leading/trailing whitespace and dots are trimmed (Windows rejects a
// directory name ending in either). An empty result (unnamed, or all-reserved) falls back to
// "Project" rather than colliding with userDataDir()'s own default settings.ini.
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
    // Bootstraps the CLR host and drives it from the frame loop, using only what ScriptHost/
    // Aver.Graph expose publicly. ScriptHost::init/graphLoad/graphTick know nothing about what a
    // graph or node means, so this belongs here beside the other subsystems this class owns.
    // (Aver.Scripting.Host has been linked into this target since CMakeLists.txt said a game "needs
    // the CLR host as much as the editor does -- more, since it has no other way to run anything.")
    //
    // First time anything in AverGame.exe calls into the scripting host: only the editor did
    // before, which kept visual scripting, the C# actor framework and AverBehaviour scripts
    // confined to its play-test harness. LoadScripts (inside init(), see HostDesc's own doc)
    // declares actor classes and runs AverBehaviour.OnStart for Binaries\Scripts, same as the
    // editor, but does NOT drive them -- nothing here calls scripts_.update() or aver_fw_begin_play()
    // to bind/tick an actor class (a separate gap, not closed here -- see the phase-2 report).
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + "\\Scripting";
    // Same formula as ProjectScaffold.cpp's scriptsBinaryDir(project), reproduced rather than
    // shared (that helper lives in the sandbox target, which a game executable can't link).
    // stage-game.ps1's LAYOUT comment: "<out>\Binaries\ the project's compiled scripts and
    // materials" -- Game.ocproject is written at <out>, so binariesDir() IS <out>\Binaries.
    hd.scriptsDir = project_.valid() ? (project_.binariesDir() + "\\Scripts") : std::string();
    scriptsReady_ = scripts_.init(hd);
    if (scriptsReady_) {
        AVER_INFO("[Game] scripting host ready: bridge='{}' scripts='{}' ({} legacy behaviour(s) live)",
                  hd.bridgeDir, hd.scriptsDir, scripts_.behaviourCount());
        // GRAPH-AS-CLASS: declare one framework class per CLASS-bearing .ocgraph under the project's
        // content, BEFORE discoverProjectGraphs runs (see that function's own comment on why it must
        // skip a file this pass already claimed). Placed right after scripts_.init() rather than
        // folded into it, mirroring how HUD/graph hosting are each their own optional capability
        // rather than baked into init() itself -- a bridge with no graph-class support still boots and
        // this call is simply a documented no-op (declareGraphClasses returns 0).
        const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
        if (graphClasses > 0)
            AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());

        // Input scheme. Right after declareGraphClasses on purpose: an InputAction/RebindAction/
        // GetActionKey node resolves its handle (via GraphInterop.ActionHandleForGraph) the moment
        // its owning class is bound, so the scheme must already be a pushed context before anything
        // can spawn. See ScriptHost::configureInput and HostBridge.cs's ConfigureInput for the full
        // contract.
        {
            // INPUT.SCHEME is relative to the content root, like DRONE.GRAPH (SandboxLevelLoad.cpp's
            // droneRel: a project key, not an assumed filename -- see OcProject.hpp).
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

        // Animation notifies, installed here (not beside the asset resolver in GameContent) since
        // the sink needs the ScriptHost. Survives a content reload: AnimSystem::clear() drops
        // clips/playhead history but not the sink. Installed unconditionally: a C++ caller can ask
        // for a curve with no scripting host at all.
        //
        // The guard is the INSTALL's, not the relay's: aver_fw_set_* needs AVER_MODULE_FRAMEWORK;
        // saveWriteProvider/saveLoadProvider need SCENE && FRAMEWORK; animCurve needs only the
        // scene. Guarding this block on AVER_MODULE_SCRIPTING alone would reach three undeclared
        // functions in a scene-off (and therefore framework-off) tree.
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&GameApp::animCurve, this);

        // Save/load: the framework relays; this is what it relays to.
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
        // GetSynapseTarget (Aver Node) reaches CSynapseAgent's current waypoint through this --
        // SCENE doesn't imply FRAMEWORK (one-way FRAMEWORK-needs-SCENE forcing in the root
        // CMakeLists), so both module guards below are asked for separately.
        aver_fw_set_synapse_target_provider(&GameApp::synapseTarget, this);
        // GetSynapsePerception (Aver Node) reaches CSynapsePerception's sight state the same way.
        aver_fw_set_synapse_perception_provider(&GameApp::synapsePerception, this);
        // PerceptionSystem's own resolver seam (SynapsePerception.hpp), not a framework_abi.h
        // relay: Aver.Synapse.Scene must not link Aver.Framework, so only a composition root
        // (linking both) can answer "who is the target" -- it still needs the framework one level
        // in, since synapseTargetResolver's body asks who is possessed.
        synapse::perceptionSystem().setTargetResolver(&GameApp::synapseTargetResolver, this);
#endif
#endif

        if (scripts_.graphFireAvailable()) {
            // The two synapse sinks below need no separate scene term: AVER_MODULE_SYNAPSE_SCENE
            // cannot be on without AVER_MODULE_SCENE.
#if AVER_MODULE_SCENE
            anim::animSystem().setNotifySink(&GameApp::animNotify, this);
#endif
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink as animNotify above (its body is just scripts_.graphFire(entity, name),
            // nothing anim-specific): PerceptionSystem::NotifyFn is byte-for-byte AnimNotifyFn's
            // signature (SynapsePerception.hpp).
            synapse::perceptionSystem().setNotifySink(&GameApp::animNotify, this);
            // The built-in "FireEvent" BT action reaches a graph the same way (BtSystem::NotifyFn,
            // SynapseBt.hpp).
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

// The animation system's answer to the framework's relayed curve query (function pointer, not a
// link edge -- see framework_abi.h). Guarded on SCENE, not SCRIPTING: this definition stood
// unguarded by SCRIPTING while the declaration sat inside it, so a scripting-off build used to lose
// the declared member but keep this definition; it only reaches for anim::animSystem() and
// scene::Entity, both of which arrive with the scene.
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

// The animation-notify wire: a clip crossed a marker naming a graph event, raised on the entity
// playing the clip. This function is the only place anim/graph meet, which is why the anim module
// takes a function pointer instead of knowing what a graph is. Perception and the BT "FireEvent"
// action reach a graph through this same sink (byte-for-byte identical NotifyFn signatures).
//
// A missing handler is not an error here: graphFire returns false for an entity with no graph, a
// graph with no such event, or a too-old bridge, none worth a line per frame -- the managed router
// already logs each once per (entity, event) pair.
//
// Two guards, different questions: the OUTER (signature's) is scene::Entity existing at all -- the
// same guard GameApp.hpp now declares it under; the INNER (body's) is scripting availability. The
// #else below anticipated a scripting-off build reaching this function, not its declaration
// vanishing too.
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

    // A no-graph project must be a no-op (one of two behaviors this task must prove correct):
    // pathsWithExtension over a graph-free tree returns empty, projectGraphs_ stays empty, and
    // tickProjectGraphs below is a single empty-vector early-out every frame after -- same shape as
    // the PBR/VOXI/PHYSICS #if no-ops, decided by content instead.
    const std::vector<std::string> paths = content_.pathsWithExtension(".ocgraph");
    if (paths.empty()) {
        AVER_INFO("[Graph] 0 .ocgraph file(s) under this project's content -- nothing to run");
        return;
    }

    // Synthetic, strictly-negative entity ids, decreasing from -1000. ScriptHost::graphLoad/graphTick
    // is entity-scoped -- its only caller before this, SandboxApp's graph-driven drone, always binds
    // a real scene::Entity -- but a project-level graph is about no one entity, so it needs an id
    // that can never collide with a live one and is safe to hand to a position-write side effect if
    // the graph declares one anyway (see GraphHost.ApplyResult, C# side). Every real scene entity
    // handle is a non-negative packed index (World::valid, SceneAbi field accessors -- fieldAddr
    // returns null for an unknown entity, and every get/set is a documented no-op on null), so a
    // negative id is guaranteed unresolvable and an accidental write becomes a safe no-op. -1000
    // rather than -1 just leaves headroom below zero.
    i32 nextId = -1000;
    u32 loaded = 0;
    u32 skippedAsClasses = 0;
    u32 skippedForeign = 0;   // graphs belonging to another domain -- see ocgraphIsForeign
    for (const std::string& path : paths) {
        // Graph-as-class: a file carrying a CLASS record was already claimed by
        // scripts_.declareGraphClasses (in initScripting) and is now driven by per-instance
        // GraphHosts (ScriptHost::tickGraphClassInstances), not this synthetic-entity path. Loading
        // it again here would tick the file twice, harmlessly but noisily (see ocgraphDeclaresClass
        // for why this is a text scan, not a real parse).
        std::string text;
        const bool haveText = readFileText(path, text);
        // Not every .ocgraph is a gameplay graph, and this sweep reaches all of them. A material
        // graph handed to graphLoad below would fail with a confusing "unknown node type" instead
        // of being recognized as offered to the wrong compiler. See ocgraphIsForeign.
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
            // ScriptHost::graphLoad already logged the C# side's [GraphHost] reason (parse/compile
            // error, or an unsupplyable PARAM shape); this line adds the one thing it can't know:
            // WHICH file, by full path. The game continues -- a graph that fails to load must log
            // clearly and must not take the game down, as visual-scripting phase 2 required.
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
    // Ungated on aver_fw_play_state(), deliberately: Aver.Graph has no reference to Aver.Framework
    // at all, so gating on PLAYING would leave every graph inert in the configuration where it's
    // most likely the ONLY gameplay a project has (framework off, or on with no GameMode declared
    // in any language -- this clause used to read "nothing here calls aver_fw_begin_play from a
    // packaged game", false since beginPlayIfGameModeDeclared landed below). "OnTick fires every
    // frame" is literal, from the first successful load for the life of the process. A discovered
    // project-level graph is not a class instance (unlike onUpdate's gated tickGraphClassInstances
    // call): nothing places these in a level, so ticking them can't mutate a level's contents.
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
    if (!scriptsReady_) return;   // no CLR host up -> no classes were ever declared, nothing to find

    // aver_fw_find_class_with_flags skips abstract rows (FrameworkAbi.cpp), so the base "GameMode"
    // row DeclareBaseClasses seals at bootstrap is invisible here -- only a project's own concrete
    // [AverGameMode] subclass returns non-zero -- a real registry question, not an assumption.
    const i32 modeClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
    // Latched for the rest of the process: onUpdate's graph-class tick needs to tell "no session
    // begun yet" from "nothing here can ever begin one", and this line is the only place that asks.
    // Set before the early return, not after
    // begin_play, so the "declined" branch below still counts as a project that HAS a GameMode --
    // failed, not undeclared -- and a graph actor doesn't start ticking on a failure's strength.
    gameModeDeclared_ = modeClass != 0;
    if (modeClass == 0) {
        AVER_INFO("[Game] no GameMode class declared -- play session not started (the framework stays "
                  "exactly as inert as it was before this existed; see discoverProjectGraphs for the "
                  "same shape decided by content instead of by class declarations)");
        return;
    }
    // 0 is a legal, common answer too: aver_fw_begin_play already treats "no GameInstance class" as
    // "skip that spawn" -- a GameMode with no GameInstance simply had nothing worth putting there.
    const i32 instanceClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);
    // (PlayMobility already began in onInit, before this call: it does not depend on a GameMode.)
    if (aver_fw_begin_play(instanceClass, modeClass)) {
        AVER_INFO("[Game] play session begun automatically (GameMode class {}) -- a shipped game has no "
                  "editor Play button, so booting it IS beginning play", modeClass);
        // After begin_play, not before (same order as SandboxPlay.cpp's startPlay() /
        // placePawnAtPlayerStart): the pawn doesn't exist until the GameMode has spawned and
        // possessed it. No fallback here, deliberately: a project's pawn spawns where its GameMode
        // chooses, and relocating it would override that decision every boot.
        placePawnAtSpawn();
    } else {
        // Only reachable if something upstream already began play (aver_fw_begin_play refuses a
        // second session), or the class failed validClass() despite being found (hard to hit
        // honestly) -- logged, not asserted, since
        // "boots with the framework inert" is a survivable outcome, like a graph that fails to compile.
        AVER_WARN("[Game] aver_fw_begin_play declined for GameMode class {} -- the framework stays in "
                  "EDITOR state; the world still renders, nothing in it plays", modeClass);
    }
#endif
}

// Player start / spawn placement, through game::placePossessedPawn (GamePawn.hpp) -- minus the
// PlayerStart marker lookup half of the editor's playerStartTransform() call, which has no runtime
// equivalent (GameLevel::spawn()'s own comment: a shipped game has no selectable marker, only the
// raw SPAWN record). Called from
// beginPlayIfGameModeDeclared() after aver_fw_begin_play succeeds.
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
// particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam (mirrors
// SandboxApp.cpp's pair of the same name). modules/particles never includes this file or
// voxi/VoxiRenderer.hpp; this pair, installed from attachParticles below, is the entire boundary.
bool GameApp::particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                std::string* outPrelude, std::string* outDefines, void* user) {
    (void)user;   // giShaderPrelude()/giShaderDefines() are pure functions of the register numbers
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

    // One shared mapping (aver::voxi::applyLevelEnv), so the game and editor cannot drift: it reads
    // every field this used to read only five of, which is how clouds, height fog, authored dome
    // and sun temperature reach a running game at all.
    assets::applyLevelEnv(w, sky_);

    // Reseeded from the atmosphere, not the level: the frame loop copies sunColor_ back over
    // sky_.sunColor every frame, so skipping this would show the level's sun for zero frames.
    for (int i = 0; i < 3; ++i) sunColor_[i] = sky_.sunColor[i];
    if (w.hasFog) fogDensity_ = sky_.fogDensity;

    AVER_INFO("[Level] applied environment: sun={} sky={} fog={} clouds={}, intensity {:.2f}, "
              "model {}", w.hasSun, w.hasSky, w.hasFog, w.hasClouds, sky_.sunIntensity,
              w.skyPhysical ? "physical" : "authored");
#endif
}

// Fits the GI volume to the level, the way the editor's frameCameraOn does when it opens one.
//
// The extent is the point, not the centre: Voxi voxelises into a fixed 128^3 grid, so extent alone
// sets voxel size (~9cm at 12m, ~15.6m at 2000m -- an entire tree in one cell, indirect light as
// uniform mush). The editor's slider stops at 100000cm for that reason. Fitting the level rather
// than a constant matches the shipped image to what the author saw.
void GameApp::fitGiVolumeToLevel() {
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
    Vec3 lo{}, hi{};
    f32 radius = 0.0f;
    if (!level_.placementBounds(lo, hi, radius)) return;   // no placements: keep the editor default
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

    // Both roots or neither: a render setting the editor applies and the shipped game ignores is
    // this repo's most-repeated defect, which is why RENDER.* lives in OcProject at all.
    //
    // Two-phase, not one (R2): tiers commit first, so derivation for an unstated knob
    // (voxelResolution, giCones, ...) runs against the NEW tier before the manifest's own values
    // land on top -- see ProjectRenderApply.hpp for the regression a single merged call
    // reintroduces.
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

    // A manifest contradiction -- RENDER.GIMODE/DENOISER/RTRENDERMODE/REFRACTIONMODE asking for
    // something that didn't survive into the effective settings (RenderSettingsResolver.hpp's
    // prerequisite table) -- logs one WARN per project open. Skips any report whose reason already
    // has a Feature that refuse() logged once for this device (Renderer::refusalLogged, reset on
    // setDeviceInfo), so the four hardware reasons don't say the same thing twice.
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

// RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE/TONEMAP, from the manifest into the post settings the shipped
// game actually renders through (previously parsed, round-tripped, and read by nobody). Writes into
// post_, not the device: pushFrame unconditionally pushes this member via setPostProcess(post_)
// once per frame, so post_ is the game's post state -- a direct call here would survive only until
// that first push overwrote it. Outside applyProjectRenderSettings on purpose, same decoupling as
// PHYSICS.*/AUDIO.* in openProject: a Voxi-less build must still ship the author's exposure. A key
// at its sentinel leaves the compiled default alone (OcProject.hpp:182-189's negative-float
// convention): zero is a legal authored bloom (RHI.hpp:151-152: no bloom pass at all) or exposure,
// so this tests "the manifest said something", never truthiness.
void GameApp::applyProjectPostSettings() {
    if (!project_.valid()) return;

    bool stated = false;
    if (project_.postExposure >= 0.0f)  { post_.exposure       = project_.postExposure;          stated = true; }
    if (project_.postBloom    >= 0.0f)  { post_.bloomIntensity = project_.postBloom;             stated = true; }
    if (project_.postAutoExposure >= 0) { post_.autoExposure   = project_.postAutoExposure != 0; stated = true; }
    if (project_.postTonemap >= 0) {
        // Clamped here because nothing downstream does: setPostProcess is `post_ = p;` in both
        // backends (D3D12Device.cpp, VulkanCommon.hpp). The image would survive an unclamped 7 on
        // its own (averTonemap only tests `mode > 1.5`, color.hlsli:144-147), but IDevice::
        // postProcess() would then report a mode the frame wasn't drawn with. Same [0,2] the
        // console clamps its own `set` to.
        post_.tonemap = static_cast<u32>(project_.postTonemap > 2 ? 2 : project_.postTonemap);
        stated = true;
    }
    if (!stated) return;

    AVER_INFO("[Project] applied post settings: exposure={:.3f} bloom={:.3f} autoExposure={} tonemap={}",
              post_.exposure, post_.bloomIntensity, post_.autoExposure ? 1 : 0, post_.tonemap);

    // A manifest contradiction, reported like applyProjectRenderSettings' Voxi ones: with eye
    // adaptation on, the composite pass multiplies by the adapted value and never reads
    // PostSettings::exposure (post.hlsl:216-220), so a stated RENDER.EXPOSURE silently does
    // nothing -- easy to hit by accident since autoExposure defaults to true (RHI.hpp:158).
    // Deliberately not "fixed" by forcing adaptation off: saying nothing about AUTOEXPOSURE isn't
    // the same as saying zero.
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
    // From the swapchain, not a viewport rect: SandboxApp::viewAspect divides vpW_/vpH_, the
    // dockspace's central node, because the editor's 3D view is one panel among many and is latched
    // by buildUI a frame earlier; a game's scene IS the backbuffer, so copying that formula would
    // need ImGui-only members and use last frame's size.
    if (const Window* w = e.window()) {
        const u32 h = w->height();
        if (h > 0) return static_cast<f32>(w->width()) / static_cast<f32>(h);
    }
    return 16.0f / 9.0f;   // headless: no window to ask
}

void GameApp::pushFrame(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // No setViewportRect: a game renders to the whole backbuffer (the editor confines to its
    // dockspace's central node), so leaving the rect alone is correct, not an omission.
    //
    // --cam-wobble DEG PERIOD (M7): same formula as SandboxApp::onUpdate's -- a sine returning to
    // zero at every whole multiple of the period, driven off frames_ (incremented once at the top
    // of onUpdate, before pushFrame runs this frame) rather than the clock, so the path is identical
    // every run. Applied as a TEMPORARY offset to yaw_ for this camForward() call only and restored
    // immediately: yaw_ is the free camera's real orientation and drivePlayCamera rewrites it from
    // the possessed pawn every playing frame, so there's nowhere safe to accumulate drift.
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

    // Logged once, and it is this commit's oracle: a game asked for 800x600 must report 1.333,
    // 1600x900 must report 1.778, and the editor's dockspace formula can't produce either. Cheap
    // enough to leave in.
    if (frames_ <= 1) {
        AVER_INFO("[Game] camera: aspect={:.3f} fov=60deg near={} far={} (from the swapchain, not a viewport rect)",
                  aspect, game::kCameraNearCm, game::kCameraFarCm);
    }

    // One member for one value: applyLevelSky seeds fogDensity_ from the level, so there's no
    // second member for this to lose a race with.
    const f32 fog = fogDensity_;
    // Frozen: sunDirection stays unnormalised here -- the shaders normalise it.
    sky_.enabled = true;
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
#if AVER_MODULE_SCENE
    // A PCGVOLUME named "Sky" drives the cloud layer: its seed picks the sky, coverage floor
    // becomes cloud cover. By NAME, not "the first field" -- a level may declare other fields (cave
    // mask, moisture) too, and sampling one of those would look like a rendering bug, not a lookup
    // one. The floor is INVERTED into coverage: a high density floor means less
    // material survives, i.e. less cloud, so passing it through unchanged would clear the sky
    // exactly when the author asked for overcast.
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
    // A script's sky wins over the level's, only when there is one: aver_fw_sky_clouds returns 0
    // until something publishes, so a sky-script-free project keeps its .ocworld sky unchanged.
    // Read every frame, not latched at load, so hot-reloading the F# moves the sky without restarting.
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
    // Cloud clock, advanced by real time so wind moves. Owned here per the RHI's own comment that
    // the app owns it; a clock that never advances gives a procedural sky that reads as a painted
    // backdrop.
    sky_.cloudTime         = cloudTime_;

#if AVER_MODULE_VOXI
    if (voxiAttached_) {
        voxi::Renderer& vx = voxi::Renderer::get();
        // N4: without this, DENOISER 1 in a manifest ran with no G-buffer allocated (the
        // denoiser never even got a G-buffer to read) and RENDER.MSAA changes mid-session never reached the device. Mirrors SandboxApp.cpp's
        // onUpdate pattern (:3865-3882 there): the G-buffer switch reads resolve()'s
        // denoiserGBufferWanted (see RenderSettingsResolver.hpp), and the MSAA push is the one-shot
        // consumeMsaaDirty() flag.
        // Frame interpolation: --frame-interp over RENDER.FRAMEINTERP. It reads motion and depth from the
        // G-buffer, so wanting it turns that on too; the device pauses it (and says why once) when
        // 1x anti-aliasing is missing. Without vsync it presents on the display's refresh clock.
        bool wantFrameInterp = cfg_.frameInterpArg >= 0 ? cfg_.frameInterpArg == 1 : project_.frameInterp == 1;
        if (wantFrameInterp && !frameInterpolator_) {
            if (dev->resources()) {
                auto fg = std::make_unique<neurafi::NeuraFI>(*dev);
                // The trajectory: --frame-interp-trajectory; Neural reads the per-machine file the editor
                // trains into, else the weights shipped in bin/data.
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
        if (vx.consumeMsaaDirty()) dev->setSampleCount(static_cast<u32>(vx.settings().msaa));

        // The volume the editor would be showing, pushed every frame exactly as the editor does
        // (SandboxApp.cpp:1641). Centre and extent are the LEVEL's, fitted once at load by
        // fitGiVolumeToLevel -- not the camera's: a camera-following volume would re-voxelise on
        // every move and diverge from the editor preview.
        voxiRenderer_.setVolume(&giCenter_.x, giExtent_);
        // Reads the same cache the editor wrote; never writes a miss back -- a game's install
        // directory isn't somewhere to grow derived data at play time. A level shipped without a
        // baked volume simply voxelises as it always did.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif

#if AVER_MODULE_FLUIDS
    // Underwater fog on a copy, so sky_ stays the authored sky.
    dev->setSkyAtmosphere(water_.applyUnderwaterFog(sky_, camPos_.z));
#else
    dev->setSkyAtmosphere(sky_);
#endif
    // Not the editor's 0.055 chrome grey: a game has no outside, so anything the sky doesn't cover
    // should read as black (a bug), not a colour that looks deliberate.
    dev->setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev->setPostProcess(post_);
}

void GameApp::onInit(Engine& e) {
    // First, before anything reads a file: later would miss the project manifest and content walk,
    // the two likeliest places a package reaches outside itself.
    if (cfg_.traceOpens) {
        setFileTrace(&onFileOpen, nullptr);
        AVER_INFO("[Game] --trace-opens: every engine file read is logged with an [open] prefix");
    }

    // The platform-side InputState: a game reads the window's own event stream, with no ImGui
    // anywhere. SandboxApp cannot do this -- its input path is inside `#if AVER_WITH_IMGUI` and
    // reads ImGui::IsKeyDown -- which is why a game executable was not merely unwritten but
    // unbuildable.
    // Borrowed for the rest of this object's life (mouse_.set/poll need the HWND) rather than
    // re-asking e.window() from onUpdate, mirroring SandboxApp's window_ member.
    window_ = e.window();
    if (Window* w = e.window()) {
        w->setEventCallback(&onWindowEvent, &input_);
        AVER_INFO("[Game] input bound to the window event stream ({}x{})", w->width(), w->height());
    } else {
        AVER_INFO("[Game] headless: no window, no input");
    }
    // Oracle for the link interface, worth a log line every run: if it's wrong every #if below is
    // false, the lifted code compiles to nothing, and the only symptom is an empty world that looks
    // like a broken renderer.
    AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={} PARTICLES={}",
              AVER_MODULE_PBR, AVER_MODULE_SCENE, AVER_MODULE_VOXI,
              AVER_MODULE_PHYSICS, AVER_MODULE_FRAMEWORK, AVER_MODULE_SCRIPTING, AVER_MODULE_PARTICLES);
    // Applied here, the first point in onInit a device is guaranteed to exist (unlike parseArgs).
    // Mirrors SandboxApp.cpp's vsyncOffRequested_ handling. One-shot, not a per-frame reassert:
    // nothing in a shipped game flips vsync back on mid-run.
    if (cfg_.vsyncOff) {
        if (rhi::IDevice* dev = e.device()) {
            if (dev->vsyncCanDisable()) { dev->setVSync(false); AVER_INFO("[Game] vsync OFF (--no-vsync)"); }
            else AVER_WARN("[Game] --no-vsync ignored: this display path cannot tear");
        } else {
            AVER_WARN("[Game] --no-vsync ignored: no device");
        }
    }
    // First of the render features: its prePass stages this frame's bone matrices, which the scene
    // pass then asks drawHandle() for.
    attachSkinning(e);
#if AVER_MODULE_FLUIDS
    // Before Voxi, as in the editor: its acceleration-structure build reads the vertex buffer the
    // fluid scene's prePass writes; registering after would leave ray-traced effects a frame stale.
    if (rhi::IDevice* dev = e.device()) water_.init(*dev);
#endif
    attachVoxi(e);
#if AVER_WITH_UI_ABI
    attachGameUi(e);
    // After attachGameUi: loadGameUiFont only needs a device, not the HUD render feature itself,
    // but this keeps every UI-ABI setup call grouped together.
    loadGameUiFont(e);
#endif
    attachParticles(e);
    if (cfg_.pcgVolumeTest) attachPcgTest(e);
    initPhysics();      // Before openProject: level load builds a static body per colliding placement
#if AVER_WITH_AUDIO_ABI
    // Opens the audio device, which a packaged game never did before: every audio_abi.h entry gates
    // on the started flag this sets, so PlaySound returned a handle but nothing ever made a sound
    // in a shipped build (the editor calls this). Before openProject, matching initPhysics() above:
    // applying AUDIO.* mix settings needs a started device. 0 is not an error here (means "no
    // output device"), unlike physics, since no sound card is a machine fact, not a broken engine.
    game::startAudio("Game");
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    // Before openProject, like initPhysics() above -- registration needs no level and no physics:
    // a class placement carrying CSynapseAgent (a later slice's concern; none does yet) must
    // find the component already registered.
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    // After registerComponents: the seven built-in behaviors read CSynapseAgent/CSynapsePerception
    // directly through agentSystem()/perceptionSystem(), so both types should already exist -- not
    // strictly required by this call, but no reason to race the ordering.
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif
#if AVER_MODULE_SCENE
    // The control rig, which a shipped game never had: registered/installed in the editor's onInit
    // but never here -- this host registered every other runtime-registered component (the three
    // Synapse ones above) and never this one -- so `Set Control Rig` returned false and every
    // IK/aim rig was inert in a packaged build while working in Play. Not a missing dependency --
    // Aver.Anim.Scene is already linked and ticked -- just two calls that were never written.
    // Before openProject, same reason as initPhysics()/Synapse above: a class placement carrying
    // CControlRig must find the type already there.
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
#endif
    openProject(e);
#if AVER_MODULE_VOXI
    // AverSR (3.3 C, contract C2-12) resolved and installed right after openProject:
    // applyProjectRenderSettings has already committed the project's GI/RT tiers into
    // voxi::Renderer::get().settings(), which is what lets autoAverSrLevel's Custom ->
    // max(GI tier, RT tier) fallback (Scalability.hpp) read the PROJECT's tiers, not the
    // compiled-in default. CLI (--aversr) beats RENDER.AVERSR beats Auto, same
    // resolveAverSrLevel chain as the editor, with userLevel pinned to -1 always: a packaged game
    // has no Display page to prefer from (U2's own "the packaged game uses the same chain minus
    // the user choice").
    if (rhi::IDevice* dev = e.device()) {
        const voxi::Renderer& vx = voxi::Renderer::get();
        const voxi::AverSrDecision dec = voxi::resolveAverSrLevel(
            cfg_.averSrArg, -1, project_.averSr, voxi::autoAverSrLevel(vx.settings(), vx.deviceInfo()));

        // Null when the SR module isn't linked (the legal default; averSrInstaller_ is set only
        // from Runtime/host/RuntimeMain.cpp under AVER_MODULE_SR -- the sole translation unit in
        // this executable allowed to name sr::anything). A resolved level with no installer stays
        // at native scale: the game must still build and run with the SR module absent, not merely
        // compile with it unreachable.
        f32 scale = 1.0f;
        if (dec.level != voxi::ladder::kAverSrOff && averSrInstaller_ &&
            averSrInstaller_(*dev, dec.level, averSrUpscaler_, scale)) {
            dev->setRenderScale(scale);
            dev->setUpscaler(averSrUpscaler_.get());
        }

        // Mirrors setRenderScale's documented formula ("the scene renders at round(present * scale)",
        // RHI.hpp); no public scene-size accessor exists to read back (a backend's own
        // sceneWidth_/sceneHeight_ are private), so this recomputes the same round(), from the
        // present size and whatever scale is in effect above (native 1.0 unless the installer ran).
        const u32 presentW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 presentH = e.window() ? e.window()->height() : cfg_.height;
        const u32 sceneW = static_cast<u32>(std::lround(static_cast<f64>(presentW) * scale));
        const u32 sceneH = static_cast<u32>(std::lround(static_cast<f64>(presentH) * scale));
        AVER_INFO("[AverSR] {} ({}): scene {}x{} -> present {}x{}",
                  averSrLevelName(dec.level), averSrSourceName(dec.source), sceneW, sceneH, presentW, presentH);
    }
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // After openProject: needs registerBuiltins' unit cube (or spawns it itself if no project
    // reached that call) and overrides the camera openProject/applyLevelSky may have touched.
    if (cfg_.particleTest) {
        if (rhi::IDevice* dev = e.device()) spawnParticleTestContent(*dev);
        else AVER_WARN("[Game] --particle-test: no device, nothing spawned");
    }
#endif
    // After openProject: both the scripts directory and graph discovery below read state openProject
    // just populated. Scan before bootstrapping, so a project that needs neither pays for neither --
    // measured cost of bootstrapping unconditionally on a graph-free project: 40-60ms extra startup
    // (938-965ms against 890-908ms), plus a new failure surface (missing bridge dir / nethost.dll).
    // pathsWithExtension only reads the content index, so asking first is nearly free. A legacy
    // Scripts assembly still forces the bootstrap: projects predating graphs rely on it.
#if AVER_MODULE_SCRIPTING
    const bool haveGraphs = !content_.pathsWithExtension(".ocgraph").empty();
    // "\\Scripts", not "\Scripts": MSVC drops the unescaped backslash (warning C4129), leaving this
    // testing for "...BinariesScripts", a path with no separator that never exists, and a project
    // whose gameplay is compiled C# with no .ocgraph beside it silently skipped bootstrapping the
    // script host. Line 686, four hundred lines up, builds the same path correctly; only this test
    // was wrong.
    const bool haveScriptAssembly = std::filesystem::exists(project_.binariesDir() + "\\Scripts");
    if (haveGraphs || haveScriptAssembly) {
        initScripting();
        discoverProjectGraphs();
    } else {
        AVER_INFO("[Game] no .ocgraph content and no Scripts assembly -- scripting host not started");
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // After initScripting/declareGraphClasses, deliberately not folded into openProject's own level
    // load (which ran above this point): a class placement's class isn't declared until this runs.
    // See GameLevel.hpp's classPlacements_ comment for why this two-step split exists at all.
    level_.spawnClassPlacements();
#endif
    // After scripting is up (so any project GameMode is declared) and after the graph/behaviour
    // report just above, and before beginPlay -- that placement is the point: taken after it, this
    // host reported entities=24 meshRenderers=20 against the editor's 20/19 -- a real but misleading
    // difference, since the game had spawned/possessed a pawn while the editor was still editing.
    // The census is for the LEVEL's content (placements, meshes, materials each host resolved) --
    // what b262c73 meant by "a different subset of the scene" -- so it's taken once both hosts have
    // finished loading the level and spawning class placements, before either plays.
#if AVER_MODULE_SCENE
    if (cfg_.sceneCensus) {
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(world::takeSceneCensus(scene::World::instance())));
    }
#endif
#if AVER_MODULE_SCENE
    // A shipped game has no editing state to keep animated objects out of, so booting the level is
    // starting them (the editor turns this on at Play instead) -- and it is the moment the mobility
    // session begins too, GameMode or not: without one, object-animated props were never marked
    // movable and rebuilt the whole Voxi GI bake every frame. After the level and its class
    // placements (that is what "level content" means to PlayMobility), BEFORE begin-play spawns the
    // pawn so that and everything after it starts provisional. Nothing ticks inside onInit, so
    // nothing moves before the world is complete.
    playMobility_.begin(scene::World::instance());
    anim::animSystem().setObjectAnimationLive(true);
#endif
    beginPlayIfGameModeDeclared();
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS && AVER_MODULE_FRAMEWORK
    // THE LEVEL'S CARS BECOME REAL HERE, once there is a play session to step them: a physics vehicle per
    // `vehicle` placement, on the lanes beside the level file. Physics only steps while a session is
    // PLAYING (tickGameplay), so a project that declares no GameMode never starts one -- and building
    // the cars anyway would only mark them movable, taking every car out of the GI bake for a motion that
    // never comes. They are movable from the first frame (a car that drives would otherwise cost the bake
    // a rebuild on its first move); seedMovable needs playMobility_'s session, begun above, and does not
    // mind that the pawn has been spawned since.
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING && level_.beginVehicles(vehicles_, content_) > 0)
        playMobility_.seedMovable(vehicles_.entities());
#endif
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    ++frames_;
    cloudTime_ += t.dt;

#if AVER_WITH_AUDIO_ABI
    // The other half of opening the device: a finished voice's slot is reclaimed here or not at
    // all. aver_audio_collect had exactly one caller in the tree, the editor's, so without this a
    // graph-started voice in a packaged game leaks its slot until the mixer runs out. Every frame,
    // ungated: voices outlive whatever started them.
    aver_audio_collect();
#endif

    // The profiler a shipped game never had. See GameConfig::statsIntervalSec for why this is a
    // log dump, not an overlay.
    //
    // First dump at the full interval, not immediately: gpuTiming() averages since boot, so asking
    // in the first second gets nothing collected, or a number dominated by unrepresentative frames.
    if (cfg_.statsIntervalSec > 0.0f && e.device()) {
        statsTimer_ += t.dt;
        if (statsTimer_ >= cfg_.statsIntervalSec) {
            statsTimer_ = 0.0f;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            // Same rough GPU-bound/CPU-bound signal the editor console prints: different sources,
            // different moments, deliberately not a matched pair. A GPU total well under the CPU
            // frame says CPU-bound; close to or above it says GPU-bound.
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
            logStatsVideoMemory(*e.device());
        }
    }
    // M7: the one-shot end-of-run dump, distinct from the periodic one above: a bounded (--frames N)
    // run can end before statsIntervalSec ever elapses once -- a 60-frame run at 60 FPS is one
    // second long, and the default interval alone is 5 -- so a script pricing a short scenario would
    // see no dump without this. Fires at the same frame gpuTimingCheck/rayProbeCheck use in
    // SandboxApp.cpp (maxFrames-2, or -1 for a very short run), so numbers from both hosts read the
    // same instant. Gated on --stats having been asked for; latched by statsFinalDumped_ so it can
    // never fire twice.
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
    // Quit, polled once a frame: a REQUEST, not an immediate exit(), since the setter can be
    // reached deep inside a script's tick with device/audio/physics all still live -- tearing down
    // from there would unwind through the managed/native boundary mid-frame. requestExit() just
    // sets a flag Engine::run checks between frames, letting this frame's onRender/onShutdown run
    // through the normal shutdown order.
    if (aver_fw_quit_requested()) e.requestExit();
    // Foreground, asked once here since two different questions this frame need the same answer:
    // confining the cursor, and whether the player is driving the pawn. Without it, publishInput
    // answered the focus question with `e.window() != nullptr` -- true even alt-tabbed away -- so a
    // backgrounded game kept feeding its pawn live input.
    //
    // Falls back to "a window exists" off Win32: no portable foreground query exists in this tree,
    // and a silent behaviour change on a platform nobody measured would be worse than the known gap.
    bool foreground = e.window() != nullptr;
#if defined(_WIN32)
    if (window_) {
        HWND hwnd = static_cast<HWND>(window_->nativeHandle());
        foreground = hwnd && ::GetForegroundWindow() == hwnd;
    }
#endif
    // Mouse capture: decided and polled BEFORE publishInput reads it (SandboxPlay.cpp order).
    // Engages only while playing AND foreground -- a background window shouldn't confine the
    // cursor. Never on a bounded (--frames N) run or with --no-mouse-capture: an automated
    // capture/gate run must not have its cursor hijacked.
    {
        bool wantCapture = false;
#if defined(_WIN32)
        wantCapture = foreground && window_ && cfg_.maxFrames == 0 && !cfg_.noMouseCapture &&
                      aver_fw_play_state() == AVER_FW_PLAY_PLAYING;
#endif
        // The new ABI folded in, as an additional reason to release only: framework_abi.h's own
        // comment on this triple spells the resolution this line implements -- "free cursor when
        // aver_fw_cursor_requested()==1 regardless of play state, else fall back to today's
        // play-state-only rule". A plain assignment to false, not folded into the expression above,
        // because the cursor rule above is load-bearing for the test harness: wantCapture already
        // reads false for a bounded/--no-mouse-capture run, so this line can only ever turn a true
        // into a false, never the reverse -- it can never cause a capture the guards above did not
        // already allow, at worst leaving the cursor visible during ordinary play.
        if (aver_fw_cursor_requested()) wantCapture = false;
        setMouseCaptured(wantCapture);
        pollCapturedMouse();
    }
    // Before the gameplay tick, so a PrePhysics actor reads THIS frame's input, not last frame's --
    // publishing after would give every input one frame of latency.
    //
    // A bounded (--frames N) run keeps the old answer, required not cautious: such a window opens
    // UNACTIVATED so a measurement never steals desktop focus (docs/headless-vs-editor.md's own
    // "The window opens unactivated" entry), so ::GetForegroundWindow() would read "not foreground"
    // every frame and every gate/screenshot would silently publish nothing but releases.
    //
    // Losing focus cannot latch a key down: publishInput writes an explicit 0 to every named slot
    // and every raw VK when `focused` is false, rather than returning early (GameInput.cpp;
    // asserted by InputBridgeTest).
    const bool inputFocused = cfg_.maxFrames > 0 ? (e.window() != nullptr) : foreground;
    InputPublishPolicy inputPolicy;
    inputPolicy.focused = inputFocused;
    // All three device gates take the same answer: the editor has panels to lose a device to
    // (ImGui field/hovered panel), a shipped game does not, so "is this window in front" answers
    // all three at once. Each written out because the fields default to FALSE -- omitting any would
    // mute that device for the game's whole life.
    inputPolicy.keyboardToGame = inputFocused;
    inputPolicy.mouseToGame    = inputFocused;
    inputPolicy.gamepadActive  = inputFocused;
    inputPolicy.captured   = mouse_.captured();
    inputPolicy.capturedDx = mouse_.dx();
    inputPolicy.capturedDy = mouse_.dy();
    // inputPolicy.eaten stays all-false: eating a slot is a chord's claim on a key, and the only
    // chords in the tree belong to the editor's drawer.
    publishInput(input_, inputPolicy, cfg_.inputEcho ? &echoHeld_ : nullptr);
    if (cfg_.inputEcho && echoHeld_ != echoLast_) {
        AVER_INFO("[Game] input: {}", echoHeld_);
        echoLast_ = echoHeld_;
    }
#endif

#if AVER_WITH_UI_ABI
    // The UI frame opens before gameplay ticks (ticking is when a game draws its HUD). Rect is the
    // whole window: a game has no dockspace. Opened even with no gameUi_, so a script always has a
    // list to draw into.
    {
        const u32 uiW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 uiH = e.window() ? e.window()->height() : cfg_.height;
        aver_ui_begin_frame(0.0f, 0.0f, static_cast<f32>(uiW), static_cast<f32>(uiH));
    }
    // The font, lent per frame (mirrors SandboxApp.cpp). loadGameUiFont (onInit) is non-fatal and
    // can leave uiFont_ invalid; aver_ui_has_font()/addText already treat that as "draw nothing".
    aver_ui_set_font(&uiFont_);
    // The pointer. Unlike the editor's conversion, no offset is needed: aver_ui_begin_frame was
    // just given the whole window, and input_.mouseX()/mouseY() are already window-client-relative
    // (Win32Window.cpp's WM_MOUSEMOVE reads client-area coordinates directly). While OS mouse
    // capture (above) is engaged the cursor sits re-centred near the window's middle every frame, a
    // meaningless but harmless HUD pointer position since a captured, playing session has no pause
    // menu to hit-test against.
    {
        u32 buttons = 0;
        if (input_.mouseHeld(0)) buttons |= 1u;   // left
        if (input_.mouseHeld(1)) buttons |= 2u;   // right
        if (input_.mouseHeld(2)) buttons |= 4u;   // middle
        aver_ui_set_pointer(static_cast<f32>(input_.mouseX()), static_cast<f32>(input_.mouseY()), buttons);
    }
#if AVER_MODULE_SCRIPTING
    // The HUD draw call: "Draw(dt) into whatever rect aver_ui_begin_frame last established"
    // (ScriptHost::hudDraw), hence after begin_frame/set_font/set_pointer and before tickGameplay.
    //
    // Index argument: the editor's only caller (SandboxApp.cpp:2567) passes hudPreviewIndex_, a
    // preview selection with no equivalent in a shipped game (no HUD-picker tab). Index 0 is the
    // closest answer instead -- the first-declared [AverHud] class, since nothing between here and
    // HostBridge.cs's DiscoverHuds exposes which index carries the `Default` flag; hudCount/
    // hudName/hudDraw (ScriptHost.hpp) are the whole native surface this host can reach. A project
    // with exactly one HUD always lands here regardless; a project with more than one gets
    // whichever loaded first, not necessarily Default -- a named limitation, not a silent one.
    if (scriptsReady_ && scripts_.hudCount() > 0) scripts_.hudDraw(0, t.dt);
#endif
#endif
#if AVER_MODULE_SCENE
    // Play PAUSED holds object animation and kinematic driving with the rest of the world. Sampled
    // BEFORE the gameplay tick, the same point tickGameplay gates on, and tested for PAUSED alone --
    // never "not PLAYING": a project with no GameMode sits in EDITOR for the whole run and its props
    // must still move. A Frame Skip is the pause lifted for one tick, so it animates that step only.
    bool objectsHeld = false;
#if AVER_MODULE_FRAMEWORK
    objectsHeld = aver_fw_play_state() == AVER_FW_PLAY_PAUSED;
#endif
#endif
    tickGameplay(t.dt);
#if AVER_MODULE_FLUIDS
    // Straight after the physics step, as in the editor: spawns what a level or script asked for,
    // reads the solver's new particle positions, and pushes the player into a pool. Not gated on Play.
    if (rhi::IDevice* dev = e.device()) water_.update(*dev, t.dt);
#endif
    // Beside tickGameplay(), same per-frame call site, just not gated on PLAYING (see
    // tickProjectGraphs' own comment for why).
    tickProjectGraphs(t.dt);
#if AVER_MODULE_SCRIPTING
    // Graph-as-class instances -- gated on PLAY, same condition the editor spells at
    // sandbox/src/SandboxApp.cpp:2494. A since-corrected comment here claimed a graph-only project
    // never calls aver_fw_begin_play (no C# GameMode to find); that premise was false. A graph class
    // declares into the same native class registry a C# one does, and sealClass's
    // inheritedKindFlags hands a graph class parented to "GameMode" the GAME_MODE bit, so
    // aver_fw_find_class_with_flags finds it and a graph-only project DOES reach
    // aver_fw_begin_play via beginPlayIfGameModeDeclared. Measured ungated cost in the editor on
    // PTTest over 1000 idle frames: a graph's `elapsed` VAR climbed from 3.6e-05 to 12.31s across
    // 4003 tick lines just from browsing a level.
    //
    // Second term (`!gameModeDeclared_`), the one case the false premise was really protecting: a
    // project that declares no GameMode in any language never reaches aver_fw_begin_play, so
    // play_state stays EDITOR forever and gating on PLAYING alone would freeze every class-placed
    // graph -- exactly the configuration graph-as-class exists to serve. gameModeDeclared_ is
    // beginPlayIfGameModeDeclared's own latched answer, not re-queried here: two callers asking the
    // same question is two answers to keep in step.
    //
    // Left ungated with AVER_MODULE_FRAMEWORK absent, same reason as the AI tick below: no play
    // state to ask about, and no way to declare a GameMode either.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING || !gameModeDeclared_) {
#endif
    scripts_.tickGraphClassInstances(t.dt);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#endif

#if AVER_MODULE_SCENE
    // Animation clock ticks UNCONDITIONALLY, not from the gameplay groups above (gated on PLAYING):
    // hanging animation off them would freeze everything animated whenever no session is running.
    // Deliberate asymmetry, copied from the editor.
    anim::animSystem().setObjectAnimationPaused(objectsHeld);
    anim::animSystem().tick(scene::World::instance(), t.dt);
    // Animated placements' kinematic bodies follow the poses the tick just wrote, so the next
    // physics step carries a character standing on one. Nothing was written while held (and the
    // physics step is not running), so there is nothing to follow.
    if (!objectsHeld)
        world::driveKinematicBodies(scene::World::instance(), level_.animatedBodies(), t.dt);
#if AVER_MODULE_PARTICLES
    // Same unconditional reasoning as the animation clock above.
    particles::particleSystem().tick(scene::World::instance(), t.dt);
#endif
    // After the animation tick, before anything draws: creates per-entity skin targets the draw
    // pass will ask for, and copies this frame's matrices out of AnimSystem (whose skinning() is
    // valid only until the next tick).
    //
    // Before World::flush, as this code has always done, though SkinnedScene.hpp's own comment
    // says "after". Copying the code, not the comment, and noting the disagreement rather than
    // silently picking a side -- if the header is right, both hosts share the bug.
    if (skinnedScene_) skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());

#if AVER_MODULE_SCENE
    // Chunk streaming, where the editor ticks it: switched on a few frames in so the level's
    // camera placement settles first, stepped every frame (Play or not), evictions retired by the
    // flush below before AI paths against the world.
    if (chunkStreamFramesLeft_ > 0 && --chunkStreamFramesLeft_ == 0) enableChunkStreaming();
    if (streaming_.enabled()) streaming_.tick(camPos_, t.dt);
#endif
    // Retires deferred destroys, rebuilds topological order, recomposes stale world matrices.
    // Without it, World::worldMatrix reads uncomposed matrices and the draw walk places everything
    // at the origin -- looks like a broken transform pipeline, is really a missing flush.
    scene::World::instance().flush();
#if AVER_MODULE_SYNAPSE_SCENE
    // Gated on PLAYING, mirroring the editor's condition minus its --spawn-test harness term (a CLI
    // self-test flag has no place in a shipped game). An AI agent has nothing to do while nothing
    // else in the level is playing. Left ungated when AVER_MODULE_FRAMEWORK is absent (no
    // aver_fw_play_state to gate on) -- a superset of the editor's reach, which never had to decide
    // this case since its block doesn't compile without both modules.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#endif
    game::tickAi(t.dt, &gameNav_);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#endif
#endif

    // AFTER flush, BEFORE the view matrix is built in onRender. Reading pawn transforms before the
    // flush would give last frame's, so the camera would trail the player by a frame -- which reads
    // as input lag rather than as an ordering bug.
    drivePlayCamera();

    // Polled after the feature has had its prePass. The build takes three frames by design; a
    // --pcg-volume-test run should be given at least that many.
    if (cfg_.pcgVolumeTest) checkPcgVolume();

    // A crossing test, not `physSteps_ % 600 == 0`: this counter advances by the real step count,
    // so a frame catching up over two steps can jump straight from 599 to 601 and skip an
    // exact-multiple test. `lastReportedSteps_ == 0` keeps the first-step-ever-announced rule.
    if (physSteps_ != lastReportedSteps_ &&
        (lastReportedSteps_ == 0 || physSteps_ / 600 != lastReportedSteps_ / 600)) {
        AVER_INFO("[Game] physics: {} step(s) taken", physSteps_);
        lastReportedSteps_ = physSteps_;
    }

#if AVER_MODULE_VOXI
    // Frame budget: FrameBudget.hpp's shared controller, seeded from project_.frameBudgetMs in
    // openProject, called right before pushFrame's Voxi push so voxiRenderer_ holds this frame's
    // rung by the time pushFrame reads it. Mirrors SandboxApp.cpp's onUpdate call site
    // (SandboxProject.cpp's frameBudgetTick, also gated on voxiAttached_ there). `vs` is a COPY of
    // the singleton: the controller must never write back into voxi::Renderer::get() itself, or a
    // throttled frame would become the new authored baseline and quality could only ratchet down.
    //
    // Never on a bounded (--frames N) run: mirrors frameBudgetTick's own maxFrames_ early-out, minus
    // its --frame-budget force flag which this runtime has no CLI equivalent for. The early-out is
    // on the CALLER's side (FrameBudget.hpp knows nothing about bounded runs) -- an automated
    // capture/gate run must not have its quality retuned mid-run.
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

    // Must be the last thing onUpdate does. Engine::frameStep runs
    // onUpdate -> beginFrame -> onRender -> endFrame, and beginFrame takes the ONE snapshot of
    // PerFrameCB into the GPU-visible buffer (D3D12Device::beginFrame is the sole write to
    // frameCBPtr_); setCamera/setLight/setSkyAtmosphere only touch the CPU-side shadow copy.
    // Calling this from onRender (after beginFrame) rasterised frame N with frame N-1's
    // camera/sky/fog/cloud constants -- worse than a uniform lag, since viewProj_ is also what
    // drawWorld culls against, so culling used THIS frame's matrix while the GPU drew the previous
    // one, a one-frame divergence. The editor never had this bug -- SandboxApp sets its camera in
    // onUpdate (SandboxApp.cpp:1115) -- which is why it never showed up in the gates.
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
    // First pixels: onUpdate's pushFrame set the camera before beginFrame uploaded the frame
    // constants, so viewProj_ is this frame's and the GPU has the matching matrix. The frustum is
    // derived from it inside drawWorld rather than cached, so it never culls what's on screen.
    if (rhi::IDevice* dev = e.device()) {
        pbr::MaterialSystem* ms = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#endif
#if AVER_MODULE_FLUIDS
        // Simulated fluid volumes, drawn first as in the editor's onRender.
        water_.draw(*dev, content_, ms);
#endif
#if AVER_MODULE_LANDSCAPE
        // Terrain before entities, as in the editor. LOD scale uses window height: a game's view
        // is the whole backbuffer, as in viewAspect.
        if (landscape_.loaded()) {
            landscape_.updateRingTiles(dev, eye_.x, eye_.y, &content_, ms);
            const u32 viewH = e.window() ? e.window()->height() : cfg_.height;
            landscape_.draw(*dev, eye_, viewProj_, static_cast<f32>(viewH), &content_, ms);
        }
#endif
        // Owner-hide: hides the possessed first-person pawn's own body mesh from the raster pass
        // (mirrors SandboxRender.cpp's owner-hide check: kMeshRendererHiddenFromOwner, ancestor walk
        // against firstPersonPawn_). firstPersonPawn_ is set every frame by drivePlayCamera() and is
        // kInvalidEntity when no session is first-person, which drawWorld treats as off -- the same
        // no-op DrawWorldOptions{} already was before this field existed.
        DrawWorldOptions opts;
        opts.ownerHideRoot = firstPersonPawn_;
        // The possessed pawn's whole tree is movable from its first frame (the viewmodel hangs off
        // its camera); everything else is judged by PlayMobility's own rule.
        {
            scene::Entity movableRoot = scene::kInvalidEntity;
#if AVER_MODULE_FRAMEWORK
            const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
            if (pn > 0) movableRoot = static_cast<scene::Entity>(static_cast<u32>(pn));
#endif
            playMobility_.beginFrame(movableRoot);
            opts.mobility = &playMobility_;
        }
#if AVER_MODULE_VOXI
        // Culled and owner-hidden entities still reach Voxi through this, so an off-screen caster
        // keeps its shadow and GI -- the editor's direct route (see drawWorld). Null without Voxi:
        // nothing to feed.
        if (voxiAttached_) opts.voxiRenderer = &voxiRenderer_;
#endif
        drawWorld(*dev, viewProj_, content_, drawStats_, ms, skinnedScene_.get(), opts);
    }
#endif
#if AVER_WITH_UI_ABI
    // Before the screenshot request, so a --screenshot capture includes the HUD.
    submitGameUi(e);
#endif
    captureScreenshotIfDue(e);

    // Rolling the input edges is the last thing the frame does, deliberately: Engine::run pumps the
    // window at the top of the loop (pumpEvents -> frameStep{onUpdate -> beginFrame -> onRender ->
    // endFrame}), so a key pressed this frame is already in InputState by the time onUpdate runs.
    // Calling newFrame() at the start of onUpdate instead would throw away edges that just arrived,
    // and the game would ignore every tap while handling held keys fine. Clearing here, after the
    // frame's last reader, leaves the accumulator empty for the next pumpEvents to fill.
    input_.newFrame();
}

void GameApp::captureScreenshotIfDue(Engine& e) {
    if (cfg_.screenshotPath.empty() || screenshotDone_ || cfg_.maxFrames == 0) return;
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // requestCapture/getFrameImage are a request/poll pair (RHI.hpp's own comment: getFrameImage
    // only has data after a requestCapture completes), so asking and reading cannot happen on the
    // same frame -- SandboxApp.cpp's captureCheck asks a few frames before the run ends for the
    // identical reason. The x,y passed to requestCapture is irrelevant here (wants the full frame
    // image, not a pixel probe).
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
    // Release the cursor first, before anything else can fail or early-out (mirrors SandboxApp.cpp's
    // own onShutdown call to setMouseCaptured(false)): an OS cursor left hidden/clipped because the
    // game exited mid-capture is a machine-wide annoyance that outlives this process.
    setMouseCaptured(false);
    // Exact reverse registration order: the device holds bare pointers to every render feature, so
    // one removed out of order can be torn down while another still references it. voxiRenderer_ is
    // a MEMBER held by value for this reason -- it outlives the device by construction, but only if
    // unregistered first.
    rhi::IDevice* dev = e.device();
    // Detach before destroy, the first statement after `dev` exists: the device holds a raw,
    // non-owning pointer into averSrUpscaler_, so resetting the unique_ptr first would leave it
    // dangling. Unconditional and harmless when AverSR was never installed: both calls are no-ops
    // on an already-null slot/pointer. Mirrors SandboxApp.cpp's clearAverSrUpscaler/
    // applyAverSrQuality guard for the identical crash class (search: "--aversr-cycle").
    if (dev) dev->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    // The frame generator: same detach-then-destroy order, same reason.
    if (dev) { dev->setFrameInterpolation(false); dev->setFrameInterpolator(nullptr); }
    frameInterpolator_.reset();
    if (dev && pcgAttached_) { dev->removeRenderFeature(&pcgVolume_); pcgAttached_ = false; }
    pcgVolume_.shutdown();
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered last of onInit's render features (after skinning and Voxi), so removed first here.
    if (dev && particlesAttached_) {
        dev->removeRenderFeature(&particleRenderer_);
        particlesAttached_ = false;
    }
    particleRenderer_.shutdown();
#endif
#if AVER_WITH_UI_ABI
    // Font comes out first: ui_abi.h's own comment on aver_ui_set_font is explicit that a font
    // outliving its atlas texture would draw glyphs sampling a dead descriptor (uiFont_.atlasTexture
    // is a raw handle into the texture destroyed two lines down). Unconditional and safe regardless
    // of whether a font or device exists -- it touches no RHI object, only the Aver.UI module's own
    // held pointer.
    aver_ui_set_font(nullptr);
    if (dev && uiFontTexture_) {
        // waitIdle mirrors SandboxApp.cpp's own icon-texture teardown at shutdown (search
        // "destroyTexture" there): destroying a texture a still-in-flight command list references is
        // a GPU-side use-after-free the debug layer catches immediately and a release build does
        // not, and this atlas was bound for sampling as recently as this frame's HUD draw.
        if (rhi::IResourceFactory* res = dev->resources()) {
            res->waitIdle();
            res->destroyTexture(uiFontTexture_);
        }
        uiFontTexture_ = 0;
    }
    // Registered after Voxi and before particles, so removed after particles and before Voxi.
    if (dev && gameUi_) {
        dev->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
#endif
#if AVER_MODULE_SCENE
    // Reverse registration order: skinning went in FIRST, so it comes out LAST of the two.
    // Removed before the device goes, because the device holds a bare pointer to it.
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
    // After the Voxi teardown: the material system lives inside VoxiRenderer, and dropping
    // materials while it's still registered would leave the render feature pointing at freed textures.
    content_.releaseProjectMaterials();
    content_.setTextureFactory(nullptr);
#endif
#if AVER_MODULE_SCENE
    skinnedScene_.reset();
    // Streamed chunks own entities and static bodies too, released here while physics still exists
    // (the editor instead leaves chunk worlds to their destructors, after physics is gone).
    streaming_.disable();
    // Before physics: unloading destroys entities and removes static bodies, and removing one from
    // a shut-down physics world is the wrong order. Object animation stops first so nothing moves
    // as the level comes down, and the mobility session ends with the level it was tracking.
    anim::animSystem().setObjectAnimationLive(false);
    playMobility_.end();
#if AVER_MODULE_PHYSICS
    // The cars' bodies and suspension constraints go before the level that holds the road under them.
    vehicles_.end();
#endif
    level_.unload();
#endif
#if AVER_MODULE_FLUIDS
    // After the level's volumes were despawned above, and before physics goes: retiring a live
    // volume calls into the solver.
    water_.shutdown(dev);
#endif
#if AVER_MODULE_PHYSICS
    aver_phys_shutdown();
#endif
#if AVER_WITH_AUDIO_ABI
    // Stops the mixer, releases the device, forgets every sound. Idempotent; a no-op if never
    // opened, so a machine with no output device is unaffected.
    aver_audio_shutdown();
#endif
#if AVER_MODULE_SCRIPTING
    // Drains every loaded graph/behaviour and closes the CLR host. Ordering against the
    // physics/scene shutdown above isn't load-bearing like render-feature registration order is --
    // nothing above depends on the scripting host being alive during its own teardown, and
    // ScriptHost never became a bare pointer anything else in this class retained.
    if (scriptsReady_) { scripts_.shutdown(); scriptsReady_ = false; }
#endif

    // Reported unconditionally, even when zero: a silent zero is indistinguishable from a broken
    // counter, and "did the world simulate at all" is the first question asked when gameplay
    // doesn't move.
    setFileTrace(nullptr, nullptr);
    AVER_INFO("[Game] shutdown after {} frame(s), {} physics step(s)", frames_, physSteps_);
    (void)dev;
}

} // namespace aver::game
