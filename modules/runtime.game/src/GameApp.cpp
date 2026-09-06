#include "aver/game/GameApp.hpp"

#include <filesystem>

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"
// The shared GPU-timing tree formatter -- see --stats in GameConfig, and the header's own note on
// why it moved out of the editor console.
#include "aver/rhi/GpuTimingFormat.hpp"

// --screenshot (captureScreenshotIfDue). A SECOND STB_IMAGE_WRITE_IMPLEMENTATION relative to
// sandbox/src/SandboxApp.cpp's own is fine -- tests/formats/CMakeLists.txt's MakeFoliage target
// already establishes the precedent: AverGame.exe and Sandbox.exe are separate binaries, so there is
// no duplicate symbol to collide, unlike modules/platform/src/Image.cpp's STB_IMAGE_IMPLEMENTATION,
// which every module ultimately links into BOTH executables and therefore may only be defined once.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
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
// --particle-test's own content (spawnParticleTestContent, below) needs the component types,
// aver_scene_material and fnv1a64 directly -- everything else in this file reaches the scene only
// through GameContent/GameLevel/GameRender, none of which needed any of the three until now.
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/core/Hash.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
// The divergence census both hosts print, so "the game draws what the editor draws" is a check
// rather than a claim. Header-only; see SceneCensus.hpp for why a census and not a pixel diff.
#include "aver/world/SceneCensus.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/Voxi.hpp"
#endif
#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// particles DECIDED 4's GI seam glue (particleGiPrepare/particleGiBind, below) is the ONLY reason
// this translation unit needs Voxi's borrowed-HLSL header -- see VoxiGiShaders.hpp's own comment on
// what it is: "the NARROW slice of Voxi's HLSL a foreign pipeline is allowed to borrow". Guarded
// identically to the two functions that use it, and to nothing else in this file.
#  include "aver/voxi/VoxiGiShaders.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"
#  include "aver/framework/framework_hooks.h"
#endif

#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>

// The module macros are PUBLIC compile definitions on the module targets (AVER_MODULE_PBR=1 on
// Aver.Render.PBR, SCENE=1 on Aver.Scene, and so on), so they reach this translation unit ONLY
// through Aver.Runtime.Game's link interface. Defaulting them to 0 here is not a convenience: it is
// what makes the report below able to say "off" rather than fail to compile, so a missing link
// interface shows up as a printed 0 instead of as lifted code silently compiling to nothing.
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

// ---- THE SAVE SEAMS a composition root installs ----------------------------------------------
//
// Aver.Save takes function pointers rather than linking Aver.Framework, so this is where the two
// meet. Same shape as the animation notify sink and the animation-curve provider above.

// SPAWNS WITHOUT BeginPlay, and that is the whole reason aver_fw_dispatch_begin_play exists.
// aver_fw_spawn runs bind -> build_models -> beginPlay inline, so an actor spawned and THEN patched
// has already begun play against its class defaults. Preview, patch, then begin.
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

// GRAPH-LOCAL VARIABLES: thin forwards to the aver_fw_graph_var_* relay (framework_abi.h) --
// mirrors SandboxApp.cpp's own saveHost() field for field, since both build a save::Host from the
// same framework ABI.
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

#if AVER_MODULE_SYNAPSE_SCENE
// A level's own .ocnav sits beside it with the same stem -- e.g. Content/Maps/Arena.ocworld ->
// Content/Maps/Arena.ocnav. Deliberately a SEPARATE copy of sandbox/src/NavBakeCommand.cpp's own
// navPathForLevel, not a shared call: that file is editor-only (sandbox/), and this composition
// root must not depend on it for a five-line string derivation.
std::string navPathForLevel(const std::string& levelPath) {
    if (levelPath.empty()) return {};
    const usize slash = levelPath.find_last_of("/\\");
    const usize dot = levelPath.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExt ? levelPath.substr(0, dot) : levelPath) + ".ocnav";
}
#endif

// Logs one line per file the engine opens. Installed only under --trace-opens.
//
// The prefix is machine-readable on purpose: verify-game.ps1 greps for it and asserts every path is
// under the package root. A packaged game that falls back to a dev-tree asset runs perfectly on the
// machine that built it and fails everywhere else, and nothing but this says so.
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
// A LIGHTWEIGHT TEXT SCAN, not a parse, and that is still the right shape even though the C++ reader
// has since learned one of the two records asked about here. CLASS it will never learn -- PARAM/VAR/
// CLASS all ride through OcGraph.cpp's classifyLine as OwnedLineKind::Other by design -- and pulling
// Aver.Formats into this module's link line to read one token off a header would be a dependency
// bought for two string comparisons. Matches OcGraphParser's own "a '#' starts a comment only at the
// START of a line" rule, which is close enough for the yes/no purposes both callers below have.
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

// True when an .ocgraph declares itself a spawnable class via a top-level CLASS record (see
// Aver.Graph's Graph.ClassName). discoverProjectGraphs needs only a yes/no: is THIS file already
// driven by the class-registration/spawn path (skip it, so it is never ALSO ticked against a
// synthetic entity in parallel with its real spawned instances), or is it an ordinary
// project-utility graph (drive it against a synthetic entity, exactly as always).
bool ocgraphDeclaresClass(const std::string& text) { return ocgraphRecord(text, "CLASS", nullptr); }

// True when an .ocgraph belongs to a domain this loader has no business compiling -- i.e. it carries
// a DOMAIN record naming anything other than gameplay. See aver::fmt::OcGraphDomain for the full
// account; the asymmetry that matters here is that an ABSENT record means gameplay (every graph in
// every project predates the record) while an UNRECOGNISED one does not, because a file naming a
// domain this build has never heard of has said out loud that it is not a gameplay graph.
bool ocgraphIsForeign(const std::string& text) {
    std::string domain;
    if (!ocgraphRecord(text, "DOMAIN", &domain) || domain.empty()) return false;
    return !equalsAsciiCI(domain, "gameplay");
}

// True for a path ending in ".ocproject", case-insensitively. Lifted from SandboxApp.cpp:5562.
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
        // --scene-census: print one canonical line describing what the level actually put in
        // the world, and exit-safe either way. scripts/verify-game.ps1 asks BOTH hosts for it
        // and compares -- the divergence check whose absence is why this executable was
        // deleted. Sandbox.exe accepts the identical flag and prints the identical format.
        else if (std::strcmp(a, "--scene-census") == 0) { c.sceneCensus = true; }
        // --stats [seconds]: the interval is OPTIONAL, so a bare --stats works. valueAfter is only
        // consumed when it parses as a number, or "--stats --headless" would silently eat the next
        // flag and run with no window for a reason nobody could see.
        else if (std::strcmp(a, "--stats") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            const f32 secs = v ? static_cast<f32>(std::atof(v)) : 0.0f;
            if (secs > 0.0f) { c.statsIntervalSec = secs; ++i; }
            else             { c.statsIntervalSec = 5.0f; }
        }
        else if (std::strcmp(a, "--pcg-volume-test") == 0) { c.pcgVolumeTest = true; }
        else if (std::strcmp(a, "--no-particle-gi") == 0)  { c.noParticleGi = true; }
        else if (std::strcmp(a, "--particle-test") == 0)   { c.particleTest = true; }
        else if (std::strcmp(a, "--screenshot") == 0)      { c.screenshotPath = valueAfter(argc, argv, i, ""); ++i; }
        // A bare path ending .ocproject is the project, so double-clicking one or dropping it on the
        // exe works. A packaged game is launched with no arguments at all and finds its manifest in
        // its own directory instead -- see openProject.
        else if (isOcproject(a))                       { c.projectPath = a; }
        // Anything else is deliberately ignored: see the header.
    }
    return c;
}

GameApp::GameApp(GameConfig cfg) : cfg_(std::move(cfg)) {}

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

    // ---- WINDOW.* FROM THE MANIFEST, read HERE and not in onInit ------------------------------
    //
    // This is the only moment the answer is usable: the window is created from this BootConfig,
    // before onInit runs, so a title or resolution the project states has to be known now. That is
    // why the manifest is read twice -- once here for four keys, once in openProject for everything
    // -- and reading a small text file twice is a much smaller price than a window that has to be
    // resized after it is already on screen.
    //
    // THE SAME TWO PLACES openProject looks, in the same order: an explicit path, else
    // Game.ocproject beside the executable, which is what stage-game.ps1 writes for a packaged game
    // launched with no arguments at all.
    //
    // THE COMMAND LINE STILL WINS, and that is the standing rule in this repo rather than a
    // preference here: a flag exists so a human at the keyboard can override recorded state, so
    // --width/--height/--title are applied over the manifest, not under it. Only a value the caller
    // did NOT state falls through to the project.
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
                    // The project's own WINDOW.TITLE, else its NAME -- a shipped game showing the
                    // engine's default title is the sort of thing nobody notices until a player does.
                    windowTitleOwned_ = !d.windowTitle.empty() ? d.windowTitle : d.name;
                    if (!windowTitleOwned_.empty()) b.windowTitle = windowTitleOwned_.c_str();
                }
                if (cfg_.width  == GameConfig::kDefaultWidth  && d.windowWidth  > 0) b.windowWidth  = static_cast<u32>(d.windowWidth);
                if (cfg_.height == GameConfig::kDefaultHeight && d.windowHeight > 0) b.windowHeight = static_cast<u32>(d.windowHeight);
            }
        }
    }
    return b;
}

void GameApp::initPhysics() {
#if AVER_MODULE_PHYSICS
    // NO IMPLICIT GROUND. This used to create a 100 m box whose top face sat exactly on z = 0, so
    // EVERY level had an invisible floor there whether it authored one or not -- a level with a
    // pit, a chasm, or water below its own floor fell through to the same z = 0 plane as a level
    // with nothing below it at all. A level supplies its own collision now: GameLevel::load already
    // adds one static body per colliding PLACE. docs/GAME-LIFT.md flagged this as worth dropping
    // when it was first lifted from the editor; this is that.
    if (aver_phys_init()) {
        AVER_INFO("[Game] physics started (fixed step {:.4f}s)", aver_phys_fixed_step());
    } else {
        AVER_WARN("[Game] physics failed to start - gameplay will not collide");
    }
#endif
}

void GameApp::tickGameplay(f32 dt) {
#if AVER_MODULE_FRAMEWORK
    // GATED ON PLAYING, and only PLAYING. playSessionActive() counts PAUSED as active, which is the
    // right answer for "is a session open" and the wrong one for "should the world advance".
    //
    // The editor widens this gate with a --spawn-test term; that is a CLI harness and has no place
    // in a game.
    if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;

    // ORDER COPIED FROM THE CODE, NOT FROM THE COMMENT ABOVE IT. SandboxApp.cpp:1049 says the tick
    // groups "bracket the physics step: PrePhysics -> Physics -> PostPhysics", which describes the
    // GROUPS and not where the step lands. The step actually sits between PRE_PHYSICS and PHYSICS,
    // so PHYSICS-group ticks observe the results of this frame's simulation. Reordering to match
    // the sentence would make every PHYSICS-group actor read last frame's transforms.
    aver_fw_tick(AVER_FW_TICK_PRE_PHYSICS, dt);
#if AVER_MODULE_PHYSICS
    aver_phys_step(dt);
    ++physSteps_;
#endif
    aver_fw_tick(AVER_FW_TICK_PHYSICS, dt);
    aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, dt);
#else
    (void)dt;
#endif
}

void GameApp::drivePlayCamera() {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;
    const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (pawn == 0) return;
    const scene::Entity ent = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
    scene::World& w = scene::World::instance();
    if (!w.valid(ent)) return;

    int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f, boom = 450.0f;
    aver_fw_view(&mode, &eye, &boom);

    // Prefer the view node; fall back to the pawn if it has not published one.
    const int32_t viewId = aver_fw_view_entity();
    const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(viewId));
    const bool haveView = viewId != 0 && w.valid(ve);
    const Mat4& vm = haveView ? w.worldMatrix(ve) : w.worldMatrix(ent);
    const Mat4& pm = w.worldMatrix(ent);

    const Vec3 headPos{vm.m[3][0], vm.m[3][1], vm.m[3][2]};
    const Vec3 headFwd = Vec3{vm.m[0][0], vm.m[0][1], vm.m[0][2]}.getSafeNormal();
    const Vec3 pawnPos{pm.m[3][0], pm.m[3][1], pm.m[3][2]};
    const Vec3 pawnFwd = Vec3{pm.m[0][0], pm.m[0][1], pm.m[0][2]}.getSafeNormal();
    const Vec3 up{0, 0, 1};

    Vec3 look;
    if (mode == AVER_FW_VIEW_FIRST_PERSON) {
        camPos_ = haveView ? headPos : pawnPos + up * eye;
        look    = headFwd;
    } else {
        const Vec3 pivot  = haveView ? headPos : pawnPos + up * eye;
        const Vec3 armDir = haveView ? headFwd : pawnFwd;
        camPos_ = pivot - armDir * boom;
        look    = (pivot - camPos_).getSafeNormal();
    }
    // camForward() composes {cosP cosY, cosP sinY, sinP}; invert the look direction to yaw/pitch.
    yaw_   = std::atan2(look.y, look.x);
    pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
#endif
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

    // The SAME spec PcgMirrorTest uses, so the C++/F#/HLSL comparison is over one set of numbers
    // rather than three sets that happen to look similar.
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

    // EVERY voxel, not a sample. 32768 comparisons cost nothing and a sampled check would miss a
    // shader that is right on the diagonal and wrong off it.
    //
    // THE TOLERANCE, and why it is not a cop-out. Measured: the GPU and the CPU agree to within
    // 1.19e-07 -- exactly 2^-23, one ULP -- on every voxel that differs at all, and they still
    // differ with coverageBias set to 1.0, which rules out pow() and leaves floating-point
    // CONTRACTION: DXC fuses multiply-add in the fBm accumulation where MSVC under /fp:precise does
    // not. The integer hash agrees bit-for-bit; only the float tail moves.
    //
    // 1e-5 is four orders of magnitude above that and four below any logic error worth the name: a
    // transposed axis, a wrong seed offset or a dropped octave moves a density by 0.1 to 1.0, not by
    // 0.0000001. So this catches everything it is meant to and tolerates only the thing it cannot
    // fix. The worst observed difference is reported every run so the number cannot quietly grow.
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
    // init compiles HLSL AT RUNTIME, so this can fail on a machine where the build was perfectly
    // green. A failure leaves skinnedScene_ null, which is a legal state the draw walk handles:
    // skinned entities draw at their REST POSE rather than not at all. A character that fails to
    // skin must still appear.
    if (!scene->init(*dev)) {
        AVER_WARN("[Game] skinning unavailable; skinned entities will draw at rest");
        return;
    }
    // The SAME tables the draw walk reads, deliberately. A skin target built from a different
    // upload than the one on screen would be a rig skinning geometry nobody can see.
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
    voxi::Renderer::get().setDeviceInfo(di);

    voxi::Settings s = voxi::Renderer::get().settings();
    s.msaa = static_cast<voxi::Msaa>(dev->sampleCount());
    voxi::Renderer::get().setSettings(s);
    voxiRenderer_.setSettings(s);

    if (voxiRenderer_.init(*dev)) {
        dev->addRenderFeature(&voxiRenderer_);
        voxiAttached_ = true;
#if AVER_MODULE_PBR
        // THESE TWO LINES STAY ADJACENT, exactly as SandboxApp has them. The resolver is installed
        // in the same breath as the factory it depends on, which is what makes the !textureFactory_
        // guard inside resolveMaterialTexture unreachable rather than merely unlikely. Separating
        // them would open a window in which a material resolves to a silent zero.
        content_.setTextureFactory(dev->resources());
        voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_);
#endif
        // Install the depth proxy resolver for LOD-based shadow/voxel optimization.
        // Installed unconditionally even if Trifactor is not linked: depthProxyMap is then empty,
        // every lookup answers 0, and every pass draws what it drew before.
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

void GameApp::attachParticles(Engine& e) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // Both process-global singletons, matching anim::animSystem() above: the system resolves
    // CParticleEmitter::effect through the library, and without this line it never resolves
    // anything -- effects_ defaults to null and every tick sees "no effect", silently spawning
    // nothing (see ParticleSystem::tick's own early-out).
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*dev)) {
        particleRenderer_.setSystem(&particles::particleSystem());
        dev->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // DECIDED 4's seam, installed only once Voxi has actually attached this run (attachVoxi runs
        // before attachParticles -- see onInit's call order) and only unless --no-particle-gi asked
        // for the A/B comparison this decision's own proof needs. See particleGiPrepare/particleGiBind
        // above for the whole contract; particleRenderer_ never learns Voxi's name.
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
    // STANDALONE BY DESIGN, matching sandbox/src/SandboxApp.cpp's own --particle-test: no project and
    // no Game.ocproject are required. registerBuiltins is normally openProject's job, run only once a
    // project's content dir resolves (see openProject's own AVER_MODULE_SCENE block) -- a bare
    // `AverGame.exe --particle-test` never opens one, so this guarantees the builtin unit cube this
    // test draws through exists regardless. Guarded on the lookup rather than called unconditionally,
    // so a run THAT does have a project (e.g. `--project <path> --particle-test`) does not create a
    // second, orphaned GPU mesh behind the one openProject already made.
    const u64 cubeId = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    if (!content_.meshFor(cubeId)) content_.registerBuiltins(device);

    scene::World& world = scene::World::instance();

    // THE SAME OCCLUDER, THE SAME DUST CLOUD, THE SAME EMBER BURST sandbox/src/SandboxApp.cpp's own
    // --particle-test spawns (SandboxApp.cpp, the particleTest_ block: identical positions, identical
    // effect data) -- rebuilt over this executable's own ECS entity + CMeshRenderer path rather than
    // the editor's objects_ list, which GameApp has no equivalent of (GameRender.cpp draws only
    // CMeshRenderer entities; see its own header comment on what a game deliberately does not carry
    // over from the editor). A screenshot from each executable is then evidence about the SAME scene
    // rendering correctly twice, not about two different scenes that both merely happen to show
    // something.
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

    // DECIDED 4's own proof of the opposite half of the seam: receivesGI = false, additive, well
    // clear of the dust cloud's footprint so the two never overlap in one screenshot -- see
    // SandboxApp.cpp's own comment on why this sits in open sky with nothing opaque behind it.
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
    emberFx.receivesGI = false;   // DECIDED 4: an ember is its own light source
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

    // THE SAME CAMERA sandbox/src/SandboxApp.cpp's own --particle-test proof used
    // (`--cam 0 0 50 0 0`): camForward() composes {cosP cosY, cosP sinY, sinP}, so yaw=pitch=0 looks
    // down +X, directly at the occluder and both emitters above.
    camPos_ = Vec3{0.0f, 0.0f, 50.0f};
    yaw_ = 0.0f;
    pitch_ = 0.0f;
    AVER_INFO("[Game] --particle-test: camera set to (0,0,50) looking down +X");
}
#endif

void GameApp::openProject(Engine& e) {
    // A PACKAGED GAME IS LAUNCHED WITH NO ARGUMENTS. stage-game.ps1 writes Game.ocproject beside
    // AverGame.exe, so when nothing was named on the command line, look there -- and look beside the
    // EXECUTABLE, never in the working directory. A player's shortcut, a store client and a
    // double-click from Explorer all set cwd to somewhere unrelated, and verify-game.ps1
    // deliberately runs the package with cwd = C:\ for exactly that reason.
    std::string path = cfg_.projectPath;
    if (path.empty()) {
        const std::string beside = executableDir() + "\\Game.ocproject";
        if (fileExists(beside)) { path = beside; }
    }
    if (path.empty()) {
        AVER_INFO("[Game] no project: pass one on the command line, or ship a Game.ocproject beside the executable");
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
    if (project_.startMap.empty()) {
        AVER_WARN("[Game] the manifest names no STARTMAP, so there is no level to open");
    }
    // ORDER IS LOAD-BEARING, and it is the same order applyProject uses: the index must precede
    // the meshes because the mesh walk resolves through it, and the meshes must precede any level
    // because a CMeshRenderer's mesh id is resolved through the mesh table.
    content_.adopt(project_);
#if AVER_MODULE_SCENE
    if (rhi::IDevice* dev = e.device()) {
        content_.registerBuiltins(*dev);
        content_.loadProjectMeshes(*dev);
    }
#if AVER_MODULE_PARTICLES
    // Same order rule as the mesh load two lines up: a placed CParticleEmitter's effect id must be
    // able to resolve before anything might read it. Unlike meshes this needs no device -- an effect
    // is CPU-only data (particles DECIDED 2) -- so it runs whether or not e.device() succeeded above.
    content_.loadProjectParticleEffects();
#endif
    level_.loadStartMap(project_, content_);
#if AVER_MODULE_SYNAPSE_SCENE
    // OPTIONAL, and silently so: most levels have no baked navigation, loadOcNav's own failure path
    // leaves gameNav_ default-constructed (empty, OcNavData::valid() == false), and
    // AgentSystem::tick already treats that identically to "no grid yet" -- an agent with a goal
    // simply waits rather than failing. Logged at INFO, not WARN: a project with no navigation at all
    // -- which every graph-driven game without agents is -- would otherwise open with a warning about
    // a feature it never asked for. (An earlier version of this comment said "only worth a WARN",
    // which no branch below has ever done.)
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
    // Apply the level's sun and sky settings
    applyLevelSky();
    fitGiVolumeToLevel();
#endif
    // Apply the project's render settings to Voxi
    applyProjectRenderSettings();
}

void GameApp::initScripting() {
#if AVER_MODULE_SCRIPTING
    // WHERE THIS BELONGS, decided and justified here rather than assumed: the C# side of visual
    // scripting (OcGraphParser, GraphCompiler, GraphHost) lives in Aver.Graph and Aver.Scripting.Bridge
    // already hosts GraphHost per entity for the editor's graph-driven drone -- but this task's own
    // scope deliberately excludes editing Aver.Scripting.Bridge or modules/scripting (another agent's
    // and an already-shared surface), so the choice was never "which of the three owns graph
    // semantics", it was "who bootstraps the CLR host and drives it from a shipped game's frame loop,
    // using ONLY what those two already expose publicly". That is a native-runtime question, not a
    // graph-semantics one -- ScriptHost::init/graphLoad/graphTick are plain C++ calls with no
    // knowledge of what a graph or a node MEANS, exactly like aver_fw_tick just below has no idea what
    // an Actor subclass does -- so it belongs here, in Aver.Runtime.Game, beside the other subsystems
    // this class already owns the lifetime of.
    //
    // THIS IS ALSO THE FIRST TIME ANYTHING IN AverGame.exe CALLS INTO THE SCRIPTING HOST AT ALL.
    // Aver.Scripting.Host has been linked into Aver.Runtime.Game since the CMakeLists.txt comment two
    // lines above target_link_libraries(... Aver.Scripting.Host) was written ("a game runs the
    // project's C# gameplay, so it needs the CLR host as much as the editor does -- more, since it has
    // no other way to run anything"), but nothing ever constructed a ScriptHost or called init() on
    // one from this executable -- only sandbox/src/SandboxApp.cpp (the editor) did. That gap is
    // exactly what kept visual scripting -- and, incidentally, the C# actor framework and AverBehaviour
    // scripts -- confined to the editor's --play-test/--spawn-test harness. Closing it for graphs
    // necessarily reopens the door for those too: LoadScripts (called inside init(), see HostDesc's own
    // doc) declares actor classes and runs AverBehaviour.OnStart for anything in Binaries\Scripts, the
    // same as it always has for the editor. What it does NOT do is drive them: nothing here calls
    // scripts_.update() (AverBehaviour.OnUpdate) or aver_fw_begin_play() (which is what would let an
    // actor class ever get bound to an entity and ticked) -- both are a separate, larger gap this task
    // does not close, and are named rather than silently left implied. See the phase-2 report.
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + "\\Scripting";
    // The exact formula sandbox/src/ProjectScaffold.cpp's scriptsBinaryDir(project) uses for the
    // editor's dev-tree case, reproduced here rather than shared: that helper lives in the sandbox
    // target, which a game executable cannot link (see this module's own CMakeLists.txt header on why
    // Aver.Runtime.Game exists at all). scripts/stage-game.ps1's own LAYOUT comment is the other half
    // of why this is right for a PACKAGED game specifically: "<out>\Binaries\ the project's compiled
    // scripts and materials", and Game.ocproject is written at <out>, so project_.binariesDir() IS
    // <out>\Binaries once a game's manifest has been opened.
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

        // ANIMATION NOTIFIES, installed here rather than beside the asset resolver in GameContent
        // for one reason: the sink needs the ScriptHost, and that lives on this class. It survives
        // a content reload without being reinstalled -- AnimSystem::clear() drops the loaded clips
        // and the playhead history, deliberately NOT the sink, because which host owns the wire
        // does not change when a project reloads its content.
        // Installed unconditionally: a C++ caller can ask for a curve with no scripting host at all.
        aver_fw_set_anim_curve_provider(&GameApp::animCurve, this);

        // SAVE/LOAD. The framework relays; this is what it relays to.
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);

#if AVER_MODULE_SYNAPSE_SCENE
        // GetSynapseTarget (Aver Node) reaches CSynapseAgent's current waypoint through this --
        // same reason and same placement as the anim-curve provider immediately above.
        aver_fw_set_synapse_target_provider(&GameApp::synapseTarget, this);
        // GetSynapsePerception (Aver Node) reaches CSynapsePerception's current sight state the
        // same way.
        aver_fw_set_synapse_perception_provider(&GameApp::synapsePerception, this);
#if AVER_MODULE_FRAMEWORK
        // PerceptionSystem's own resolver seam (SynapsePerception.hpp), NOT a framework_abi.h relay
        // -- see that header's own comment for why Aver.Synapse.Scene must not link Aver.Framework
        // at all, so only a composition root (linking both) can answer "who is the target".
        synapse::perceptionSystem().setTargetResolver(&GameApp::synapseTargetResolver, this);
#endif
#endif

        if (scripts_.graphFireAvailable()) {
            anim::animSystem().setNotifySink(&GameApp::animNotify, this);
#if AVER_MODULE_SYNAPSE_SCENE
            // The SAME sink as animNotify immediately above -- its body is just
            // scripts_.graphFire(entity, name), nothing anim-specific, and PerceptionSystem's
            // NotifyFn is byte-for-byte AnimNotifyFn's own signature (see SynapsePerception.hpp).
            synapse::perceptionSystem().setNotifySink(&GameApp::animNotify, this);
            // The built-in "FireEvent" BT action reaches a graph the SAME way -- BtSystem::NotifyFn
            // is the identical signature too (SynapseBt.hpp).
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

// The animation system's answer to the framework's relayed curve query. See framework_abi.h for
// why this is a function pointer rather than a link edge.
i32 GameApp::animCurve(i32 entity, i64 nameHash, f32* outValue, void*) {
    f32 v = 0.0f;
    if (!anim::animSystem().curveValue(static_cast<scene::Entity>(entity),
                                       static_cast<u64>(nameHash), v)) return 0;
    *outValue = v;
    return 1;
}

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

// THE ANIMATION-NOTIFY WIRE. A clip crossed a marker; that marker names a graph event; the entity
// playing the clip is the one to raise it on. Every part of that sentence belongs to a different
// module, and this function is the only place they meet -- which is exactly why the anim module takes
// a function pointer instead of knowing what a graph is. Perception and the BT "FireEvent" action
// reach a graph through this same sink; their NotifyFn signatures are byte-for-byte identical.
//
// A MISSING HANDLER IS NOT AN ERROR HERE. graphFire returns false for an entity with no graph, a
// graph with no such event, and a bridge too old to be fired at, and none of those is worth a line
// per frame from an animation tick -- the managed router already logs each once per (entity, event)
// pair with a message saying which it was.
//
// (This comment had drifted ~50 lines up the file, where it sat after a closing brace and above
// animCurve, which has a doc comment of its own. Re-homed.)
void GameApp::animNotify(scene::Entity e, const char* name, void* user) {
#if AVER_MODULE_SCRIPTING
    auto* self = static_cast<GameApp*>(user);
    if (!self || !name) return;
    self->scripts_.graphFire(static_cast<i32>(e), name);
#else
    (void)e; (void)name; (void)user;
#endif
}


void GameApp::discoverProjectGraphs() {
#if AVER_MODULE_SCRIPTING
    if (!scriptsReady_) return;
    if (!scripts_.graphAvailable()) {
        AVER_INFO("[Graph] this build's scripting bridge has no graph hosting -- project .ocgraph "
                  "files, if any, will not run (see ScriptHost::graphAvailable's own doc)");
        return;
    }

    // A NO-GRAPH PROJECT IS ONE OF THE TWO THINGS THIS TASK HAS TO PROVE BEHAVES CORRECTLY. This is
    // where that is decided: pathsWithExtension over an empty (or graph-free) content tree returns an
    // empty vector, projectGraphs_ stays empty, and tickProjectGraphs below is a single empty-vector
    // early-out every frame thereafter -- the same shape of no-op the PBR/VOXI/PHYSICS #if blocks
    // already are for a tree missing THOSE modules, just decided by content rather than by a build flag.
    const std::vector<std::string> paths = content_.pathsWithExtension(".ocgraph");
    if (paths.empty()) {
        AVER_INFO("[Graph] 0 .ocgraph file(s) under this project's content -- nothing to run");
        return;
    }

    // SYNTHETIC, STRICTLY-NEGATIVE entity ids, decreasing from -1000. ScriptHost::graphLoad/graphTick
    // is an ENTITY-scoped API -- its only caller before this (SandboxApp's graph-driven drone) always
    // binds a REAL scene::Entity, because a dataflow graph's PARAM entity and its 2-3-OUT position
    // write are both meant to land on one. A project-level graph is not about any one entity, so it
    // needs an id that can never collide with a live one AND is safe to hand to that position-write
    // side effect if the graph declares one anyway (see GraphHost.ApplyResult, C# side). Every native
    // scene entity handle is a non-negative packed index (World::valid rejects anything else, and
    // SceneAbi's field accessors all resolve through that same validity check before touching memory
    // -- SceneAbi.cpp's fieldAddr returns null for an unknown entity, and every get/set is a documented
    // no-op on null) -- so a negative id is guaranteed unresolvable, and an accidental write from a
    // project graph is a safe no-op rather than a stray write into whatever real entity shares the
    // number. -1000 rather than -1 leaves headroom below zero in case something else ever wants small
    // negative sentinels; the exact value carries no other meaning.
    i32 nextId = -1000;
    u32 loaded = 0;
    u32 skippedAsClasses = 0;
    u32 skippedForeign = 0;   // graphs belonging to another domain -- see ocgraphIsForeign
    for (const std::string& path : paths) {
        // GRAPH-AS-CLASS: a file carrying a CLASS record was already claimed by
        // scripts_.declareGraphClasses (called from initScripting, before this function runs) -- it
        // is now driven by aver_fw_spawn'd, per-instance GraphHosts (see ScriptHost::
        // tickGraphClassInstances), not by this synthetic-entity path. Loading it AGAIN here would tick
        // the same file twice: once correctly, against every real spawned instance, and once uselessly,
        // against an unresolvable synthetic id that writes nowhere (see the comment above this loop) --
        // harmless, but noisy, and it would double the [GraphHost] log lines this task's own evidence
        // rule leans on for a clean per-entity trajectory. See ocgraphDeclaresClass's own comment for
        // why this is a lightweight text scan rather than a real parse.
        std::string text;
        const bool haveText = readFileText(path, text);
        // NOT EVERY .ocgraph IS A GAMEPLAY GRAPH, and this sweep reaches every one under the
        // project. A material graph handed to graphLoad below would get as far as GraphCompiler and
        // fail with "unknown node type" -- a message that reads like a broken graph rather than a
        // graph offered to the wrong compiler. See ocgraphIsForeign.
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
            // error, or a PARAM shape GraphHost cannot supply) via HostBridge.GraphLoad's own Emit
            // call, which reaches this process's log the same way every other managed log line does.
            // This line adds the one thing that log line cannot know on its own: WHICH FILE, by its
            // full path, so a project with several graphs does not leave the reader guessing which one
            // is broken. The game continues -- a graph that fails to load is exactly the "must log
            // clearly and must NOT take the game down" case visual-scripting phase 2 asked for.
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
    // UNGATED ON aver_fw_play_state(), and deliberately so -- unlike tickGameplay() just above this
    // call's site in onUpdate(). Graphs are a scripting-layer feature: Aver.Graph has no reference to
    // Aver.Framework at all (check the .csproj), so gating a graph's OnTick on the framework's PLAYING
    // state would make every graph silently inert in exactly the configuration where it is most likely
    // to be the ONLY gameplay a project has -- AVER_MODULE_FRAMEWORK off, or on but this project
    // declares no GameMode (nothing in this task ever calls aver_fw_begin_play from a packaged game;
    // see initScripting's own comment on that separate, larger, un-closed gap). "OnTick fires every
    // frame" is read literally: from the first frame this graph loaded successfully, for as long as
    // the process runs, independent of whether anything else in the game is "playing".
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

    // aver_fw_find_class_with_flags already skips abstract rows (FrameworkAbi.cpp), so the base
    // "GameMode" row DeclareBaseClasses seals at bootstrap is invisible to this query on its own --
    // only a project's OWN concrete [AverGameMode] subclass makes this return non-zero. That is what
    // makes "declared" the right word in this function's name: it is asking the registry a project
    // question, not assuming one.
    const i32 modeClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
    if (modeClass == 0) {
        AVER_INFO("[Game] no GameMode class declared -- play session not started (the framework stays "
                  "exactly as inert as it was before this existed; see discoverProjectGraphs for the "
                  "same shape decided by content instead of by class declarations)");
        return;
    }
    // 0 is a legal, common answer here too -- aver_fw_begin_play already treats "no GameInstance class"
    // as "skip that spawn" (FrameworkAbi.cpp), so a project with a GameMode but no GameInstance is not
    // a degraded case, just a project that had nothing worth putting there.
    const i32 instanceClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);
    if (aver_fw_begin_play(instanceClass, modeClass)) {
        AVER_INFO("[Game] play session begun automatically (GameMode class {}) -- a shipped game has no "
                  "editor Play button, so booting it IS beginning play", modeClass);
    } else {
        // Only reachable if something upstream already called aver_fw_begin_play (it refuses a second
        // session) or the class failed validClass() despite being found, which aver_fw_find_class_with_
        // flags's own linear scan makes very hard to hit honestly -- logged rather than asserted because
        // "the game boots with the framework inert" is still a survivable outcome, same as a graph that
        // fails to compile.
        AVER_WARN("[Game] aver_fw_begin_play declined for GameMode class {} -- the framework stays in "
                  "EDITOR state; the world still renders, nothing in it plays", modeClass);
    }
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
// particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam. IDENTICAL in shape and
// reasoning to sandbox/src/SandboxApp.cpp's own pair of the same name -- see that file's comment for
// the full contract. modules/particles never includes this file, this class, or voxi/VoxiRenderer.hpp;
// this pair of functions, installed from attachParticles below, is the entire boundary.
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

    // ONE SHARED MAPPING, so the game and the editor cannot drift. This function used to read five
    // of the format's fields; aver::voxi::applyLevelEnv reads all of them, which is how a level's
    // clouds, height fog, authored dome and sun temperature reach a running game at all -- until
    // now they were authorable in the editor and dropped on the floor here.
    assets::applyLevelEnv(w, sky_);

    // RESEEDED FROM THE ATMOSPHERE, not from the level, because the frame loop copies sunColor_
    // back over sky_.sunColor every frame -- so applying the level and not doing this would show
    // the level's sun for exactly zero frames. See LevelSky.hpp's note on host-owned mirrors.
    for (int i = 0; i < 3; ++i) sunColor_[i] = sky_.sunColor[i];
    if (w.hasFog) fogDensity_ = sky_.fogDensity;

    AVER_INFO("[Level] applied environment: sun={} sky={} fog={} clouds={}, intensity {:.2f}, "
              "model {}", w.hasSun, w.hasSky, w.hasFog, w.hasClouds, sky_.sunIntensity,
              w.skyPhysical ? "physical" : "authored");
#endif
}

// Fits the GI volume to the level, the way the editor's frameCameraOn does when it opens one.
//
// THE EXTENT IS THE POINT, not the centre. Voxi voxelises this volume into a fixed grid, so extent
// alone decides how many centimetres one voxel spans: at the shipped 128^3, a 12m volume gives ~9cm
// voxels and a 2000m one gives ~15.6m voxels -- a grid in which an entire tree fits inside a single
// cell and indirect light is uniform mush. The editor's own slider stops at 100000cm for that
// reason. Fitting the level rather than picking a constant is what makes the shipped image match
// what the author was looking at when they saved.
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

    voxi::Renderer& vx = voxi::Renderer::get();
    voxi::Settings s = vx.settings();
    if (project_.giQuality >= 0) s.globalIllumination = static_cast<voxi::Quality>(project_.giQuality);
    if (project_.rayTracing >= 0) s.rayTracing = static_cast<voxi::Quality>(project_.rayTracing);
    if (project_.pathTracing >= 0) s.pathTracing = static_cast<voxi::Quality>(project_.pathTracing);
    if (project_.voxelResolution > 0) s.voxelResolution = static_cast<u32>(project_.voxelResolution);
    if (project_.giIntensity >= 0.0f) s.giIntensity = project_.giIntensity;
    if (project_.giMaxDistance >= 0.0f) s.giMaxDistance = project_.giMaxDistance;
    if (project_.rtShadowRays >= 0) s.rtShadowRays = static_cast<u32>(project_.rtShadowRays);
    if (project_.rtPixelsPerRayTile >= 0) s.rtPixelsPerRayTile = static_cast<u32>(project_.rtPixelsPerRayTile);
    if (project_.rtShadowDenoise >= 0) s.rtShadowDenoise = static_cast<u32>(project_.rtShadowDenoise);
    // BOTH ROOTS OR NEITHER. A render setting the editor applies and the shipped game ignores is
    // this repo's most-repeated defect: the project looks right while it is being made and ships
    // looking different. These two lines are the whole reason the key exists in OcProject rather
    // than in the editor's own preferences.
    if (project_.rtRenderMode >= 0) s.rtRenderMode = static_cast<u32>(project_.rtRenderMode);
    if (project_.ptBounces >= 0) s.ptBounces = static_cast<u32>(project_.ptBounces);
    vx.setSettings(s);
    voxiRenderer_.setSettings(vx.settings());
    AVER_INFO("[Project] applied render settings: gi={}, rt={}, pt={}, voxelRes={}, giIntensity={}, giDist={}",
              static_cast<int>(s.globalIllumination), static_cast<int>(s.rayTracing),
              static_cast<int>(s.pathTracing), s.voxelResolution, s.giIntensity, s.giMaxDistance);
#endif
}

Vec3 GameApp::camForward() const {
    return Vec3{ std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_) };
}

f32 GameApp::viewAspect(const Engine& e) const {
    // FROM THE SWAPCHAIN, not from a viewport rect. SandboxApp::viewAspect divides vpW_/vpH_, the
    // dockspace's central node, because the editor's 3D view is one panel among many and is latched
    // by buildUI a frame earlier. A game's scene IS the backbuffer. Copying the editor's formula
    // would have required members that exist only because ImGui does, and would have produced a
    // game whose projection silently used last frame's panel size.
    if (const Window* w = e.window()) {
        const u32 h = w->height();
        if (h > 0) return static_cast<f32>(w->width()) / static_cast<f32>(h);
    }
    return 16.0f / 9.0f;   // headless: no window to ask
}

void GameApp::pushFrame(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // NO setViewportRect. The editor confines the scene to the dockspace's central node; a game
    // renders to the whole backbuffer, so leaving the rect alone is the correct behaviour and not
    // an omission.
    const Vec3 fwd    = camForward();
    const f32  aspect = viewAspect(e);
    const Mat4 view   = Mat4::lookAtLH(camPos_, camPos_ + fwd, Vec3{0, 0, 1});
    const f32  zNear = 2.0f, zFar = 200000.0f;   // centimetres
    const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, zNear, zFar);
    const Mat4 viewProj = view * proj;           // row-vector: v * M, so view then proj
    const Mat4 invVP = viewProj.inverse();
    dev->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &camPos_.x);
    invVP_ = invVP; viewProj_ = viewProj; eye_ = camPos_;

    // Logged once, and it is this commit's oracle. A game asked for 800x600 must report 1.333 and
    // one asked for 1600x900 must report 1.778; the editor's dockspace formula cannot produce
    // either, because it divides a panel that does not exist here. Cheap enough to leave in.
    if (frames_ <= 1) {
        AVER_INFO("[Game] camera: aspect={:.3f} fov=60deg near={} far={} (from the swapchain, not a viewport rect)",
                  aspect, zNear, zFar);
    }

    // ONE MEMBER FOR ONE VALUE, matching the editor's own fix: applyLevelSky seeds fogDensity_ from
    // the level, so there is no second member for this to lose a race with.
    const f32 fog = fogDensity_;
    // FROZEN: sunDirection stays unnormalised here -- the shaders normalise it.
    sky_.enabled = true;
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
#if AVER_MODULE_SCENE
    // THE LEVEL'S DECLARED SKY FIELD, reaching the renderer. A PCGVOLUME named "Sky" drives the
    // cloud layer: its seed picks which sky this is, and its coverage floor becomes cloud cover.
    //
    // BY NAME, not "the first field": a level may declare a cave mask and a moisture field too, and
    // sampling one of those as the sky would look like a rendering bug rather than a lookup one.
    //
    // The floor is INVERTED into coverage on purpose. A density floor is the threshold below which
    // the field is empty, so a HIGH floor means less material survives -- which is less cloud, not
    // more. Passing it through unchanged would make the sky clear exactly when the author asked for
    // overcast.
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
    // A SCRIPT'S SKY WINS OVER THE LEVEL'S, and only when there is one. aver_fw_sky_clouds returns
    // 0 until something has published, so a project with no sky script keeps exactly the sky its
    // .ocworld authored -- which is what makes this additive rather than a behaviour change.
    //
    // Read every frame rather than latched at load, so editing the F# and hot-reloading moves the
    // sky without restarting. That is the point of putting the sky in a script at all.
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
    // The cloud clock, advanced by real time so wind moves. Owned here because the RHI's comment
    // says the app owns it, and a clock that never advances gives a sky that is procedural and
    // completely static, which reads as a painted backdrop.
    sky_.cloudTime         = cloudTime_;

#if AVER_MODULE_VOXI
    if (voxiAttached_) {
        // The volume the editor would be showing, pushed every frame exactly as the editor pushes it
        // (SandboxApp.cpp:1641). Centre and extent are the LEVEL's, fitted once at load by
        // fitGiVolumeToLevel -- deliberately not the camera's. A camera-following volume is a
        // different feature, for worlds bigger than one volume, and it would re-voxelise on every
        // move and give the shipped game a different image from the editor preview, which is the one
        // thing this whole change exists to stop.
        voxiRenderer_.setVolume(&giCenter_.x, giExtent_);
        // The shipped runtime reads the same cache the editor wrote. It never WRITES a miss back:
        // a game's install directory is not somewhere to grow derived data at play time, and a
        // level shipped without a baked volume simply voxelises as it always did.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif

    dev->setSkyAtmosphere(sky_);
    // NOT the editor's 0.055 chrome grey. Nothing outside a game's viewport is chrome, because a
    // game has no outside -- anything the sky does not cover is a bug the player should see as
    // black, not as a colour that looks deliberate.
    dev->setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev->setPostProcess(post_);
}

void GameApp::onInit(Engine& e) {
    // FIRST, before anything reads a file. Installing it later would miss the project manifest and
    // the content walk, which are the two most likely places a package reaches outside itself.
    if (cfg_.traceOpens) {
        setFileTrace(&onFileOpen, nullptr);
        AVER_INFO("[Game] --trace-opens: every engine file read is logged with an [open] prefix");
    }

    // The whole point of the platform-side InputState: a game reads the window's own event stream,
    // with no ImGui anywhere. SandboxApp cannot do this -- its input path is inside
    // `#if AVER_WITH_IMGUI` and reads ImGui::IsKeyDown -- which is why a game executable was not
    // merely unwritten but unbuildable.
    if (Window* w = e.window()) {
        w->setEventCallback(&onWindowEvent, &input_);
        AVER_INFO("[Game] input bound to the window event stream ({}x{})", w->width(), w->height());
    } else {
        AVER_INFO("[Game] headless: no window, no input");
    }
    // This line is the oracle for the link interface, and it is worth a log line every run. Every
    // subsystem lifted out of SandboxApp is wrapped in one of these #ifs; if the link interface is
    // wrong they are all false, the lifted code compiles to nothing, and the only symptom is a game
    // that draws an empty world -- which looks exactly like a broken renderer.
    AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={} PARTICLES={}",
              AVER_MODULE_PBR, AVER_MODULE_SCENE, AVER_MODULE_VOXI,
              AVER_MODULE_PHYSICS, AVER_MODULE_FRAMEWORK, AVER_MODULE_SCRIPTING, AVER_MODULE_PARTICLES);
    // FIRST of the render features. Its prePass stages this frame's bone matrices, and the scene
    // pass then asks drawHandle() for a posed handle that must already exist.
    attachSkinning(e);
    attachVoxi(e);
    attachParticles(e);
    if (cfg_.pcgVolumeTest) attachPcgTest(e);
    initPhysics();      // BEFORE openProject: level load builds a static body per colliding placement
#if AVER_MODULE_SYNAPSE_SCENE
    // BEFORE openProject, matching initPhysics() immediately above -- registration needs no level
    // and no physics, and a class placement spawned by openProject that carries CSynapseAgent (a
    // later slice's concern; none does yet) must find the component already registered.
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    // AFTER registerComponents: the seven built-ins read CSynapseAgent/CSynapsePerception through
    // agentSystem()/perceptionSystem() directly (see registerBuiltinBehaviors' own comment), so
    // both must already have a registered component type before this call would be meaningful --
    // it does not itself require one, but there is no reason to race the ordering.
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif
    openProject(e);
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // AFTER openProject: needs registerBuiltins' unit cube (or spawns it itself when no project ever
    // reached that call -- see spawnParticleTestContent's own comment) and overrides the camera
    // openProject/applyLevelSky may already have touched.
    if (cfg_.particleTest) {
        if (rhi::IDevice* dev = e.device()) spawnParticleTestContent(*dev);
        else AVER_WARN("[Game] --particle-test: no device, nothing spawned");
    }
#endif
    // AFTER openProject: the scripts directory this resolves (project_.binariesDir() + "\Scripts")
    // and the graph discovery below (content_.pathsWithExtension) both read state openProject just
    // populated. See initScripting's own comment for why bootstrapping the CLR host lives here at
    // all, and discoverProjectGraphs' for the no-graph-project behaviour this order guarantees.
    // SCAN BEFORE BOOTSTRAPPING, so a project that needs neither never pays for either. This used to
    // read `initScripting(); discoverProjectGraphs();` unconditionally, and the graph-count check that
    // makes a no-graph project a no-op lived one function too late -- it gated the GRAPH side effects
    // while the CLR host had already been started above it. A reviewer measured the cost on a
    // genuinely graph-free project: 40-60ms of extra startup (938-965ms against 890-908ms), plus a new
    // failure surface -- a missing bridge directory or nethost.dll -- that AverGame.exe simply did not
    // have before, because nothing outside the editor had ever called ScriptHost::init.
    //
    // pathsWithExtension only reads the content index openProject already populated, so asking first
    // is nearly free and needs no live host. A legacy Scripts assembly still forces the bootstrap:
    // projects predating graphs rely on it and must not silently lose their scripts.
#if AVER_MODULE_SCRIPTING
    const bool haveGraphs = !content_.pathsWithExtension(".ocgraph").empty();
    // "\\Scripts", NOT "\Scripts". MSVC does not recognise \S as an escape, drops the backslash with
    // warning C4129, and leaves this testing for "...BinariesScripts" -- a path with no separator in
    // it, which never exists. So haveScriptAssembly was effectively always false, and a project whose
    // gameplay is compiled C# with no .ocgraph beside it silently skipped bootstrapping the script
    // host entirely. Line 686 four hundred lines up builds the SAME path correctly; only this test
    // was wrong, which is why nothing looked broken until someone shipped a graph-free project.
    //
    // The compiler said so on every single build. C4129 has been in this tree's warning output the
    // whole time, unread, next to the C4127s and C4324s nobody triages -- which is the actual lesson
    // here and worth more than the one character.
    const bool haveScriptAssembly = std::filesystem::exists(project_.binariesDir() + "\\Scripts");
    if (haveGraphs || haveScriptAssembly) {
        initScripting();
        discoverProjectGraphs();
    } else {
        AVER_INFO("[Game] no .ocgraph content and no Scripts assembly -- scripting host not started");
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // AFTER initScripting/declareGraphClasses (whichever branch above ran), and DELIBERATELY not
    // folded into openProject's own level load, which already ran ABOVE this point in this same
    // function -- see GameLevel.hpp's classPlacements_ comment for the ordering reason this two-step
    // split exists at all: a class placement's class is not declared until this line has run.
    level_.spawnClassPlacements();
#endif
    // AFTER scripting is up (so any project GameMode is declared) and AFTER the graph/behaviour report
    // just above (so a reader sees what loaded before seeing whether it started playing). See this
    // function's own comment for why "declares a GameMode" is the generic, content-driven switch this
    // is gated on, matching the shape haveGraphs/haveScriptAssembly already uses just above.
    // BEFORE beginPlay, and that placement is the whole point.
    //
    // Taken after it instead, this host reported entities=24 meshRenderers=20 against the editor's
    // 20/19 -- a difference that is REAL and entirely legitimate: the game had spawned and possessed
    // a pawn, and the editor was still editing. Comparing a playing host against an editing one
    // measures the lifecycle, not the content, and would have made this gate cry wolf on every
    // project that declares a GameMode.
    //
    // What the census is for is the LEVEL's content -- the placements, the meshes and the materials
    // each host resolved -- which is what b262c73 meant by "a different subset of the scene". So it
    // is taken at the point both hosts have finished loading the level and spawning its class
    // placements, and neither has started playing.
#if AVER_MODULE_SCENE
    if (cfg_.sceneCensus) {
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(world::takeSceneCensus(scene::World::instance())));
    }
#endif
    beginPlayIfGameModeDeclared();
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    ++frames_;
    cloudTime_ += t.dt;

    // THE PROFILER A SHIPPED GAME NEVER HAD. See GameConfig::statsIntervalSec for why this is a log
    // dump and not an overlay, and why the numbers were already being collected.
    //
    // FIRST DUMP AT THE FULL INTERVAL, not immediately: gpuTiming() reports an average over frames
    // since boot, so asking in the first second gets either "nothing collected yet" or a number
    // dominated by the first frames, which are the least representative ones a game ever renders.
    if (cfg_.statsIntervalSec > 0.0f && e.device()) {
        statsTimer_ += t.dt;
        if (statsTimer_ >= cfg_.statsIntervalSec) {
            statsTimer_ = 0.0f;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            // The same rough GPU-bound/CPU-bound signal the editor console prints, and the same
            // caveat: different sources, different moments, deliberately not a matched pair. A GPU
            // total well under the CPU frame says CPU-bound; close to or above it says GPU-bound.
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
        }
    }
    // Input is READ here, never rolled here. See onRender for why.
#if AVER_MODULE_FRAMEWORK
    // BEFORE the gameplay tick, so a PrePhysics actor reads THIS frame's input rather than last
    // frame's. Publishing after the tick would give every input one frame of latency, which is the
    // kind of thing that gets blamed on the display.
    publishInput(input_, e.window() != nullptr, cfg_.inputEcho ? &echoHeld_ : nullptr);
    if (cfg_.inputEcho && echoHeld_ != echoLast_) {
        AVER_INFO("[Game] input: {}", echoHeld_);
        echoLast_ = echoHeld_;
    }
#endif

    tickGameplay(t.dt);
    // BESIDE tickGameplay(), not a parallel loop of its own: this is the same per-frame call site,
    // just not gated on the same PLAYING check -- see tickProjectGraphs' own comment for why.
    tickProjectGraphs(t.dt);
#if AVER_MODULE_SCRIPTING
    // GRAPH-AS-CLASS instances, same "beside tickGameplay(), not gated on PLAYING" placement and
    // reasoning as tickProjectGraphs immediately above -- see ScriptHost::tickGraphClassInstances'
    // own comment (and DeclareGraphClasses' `ticks` comment on the C# side) for exactly why: a
    // graph-only project never calls aver_fw_begin_play (no C# GameMode to find), so gating a
    // spawned class instance's OnTick on aver_fw_play_state() would make graph-as-class silently
    // inert in precisely the "no C# at all" configuration it exists to serve.
    scripts_.tickGraphClassInstances(t.dt);
#endif

#if AVER_MODULE_SCENE
    // The animation clock ticks UNCONDITIONALLY, not from the gameplay groups above. Those are
    // gated on PLAYING, and hanging animation off them would freeze every animated thing the moment
    // a session was not running. Deliberate asymmetry, copied from the editor.
    anim::animSystem().tick(scene::World::instance(), t.dt);
#if AVER_MODULE_PARTICLES
    // Same "unconditionally, not from the gameplay tick" reasoning as the animation clock above.
    particles::particleSystem().tick(scene::World::instance(), t.dt);
#endif
    // Retires deferred destroys, rebuilds the topological order and recomposes stale world
    // matrices. Without it World::worldMatrix reads uncomposed matrices and the draw walk in C7
    // would place everything at the origin -- which looks like a broken transform pipeline and is
    // really a missing flush.
    // AFTER the animation tick and BEFORE anything draws: update() creates the per-entity skin
    // targets the draw pass is about to ask for, and COPIES this frame's matrices out of the
    // AnimSystem, whose skinning() is valid only until the next tick.
    //
    // BEFORE World::flush, which is what the editor does. SkinnedScene.hpp's own comment says
    // "AFTER AnimSystem::tick and World::flush"; the code has always called it before. Copying the
    // code rather than the comment, and noting the disagreement rather than silently picking a
    // side -- if the header is right, both hosts have the same bug and it should be fixed in one
    // place.
    if (skinnedScene_) skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());

    scene::World::instance().flush();
#if AVER_MODULE_SYNAPSE_SCENE
    // AFTER flush: an agent must path from where physics/anim actually left it this frame, not from
    // last frame's stale transform. gameNav_ may be empty (no baked navigation for this level, or
    // none loaded yet) -- AgentSystem::tick treats that as "wait", not an error; see its own comment.
    synapse::agentSystem().tick(scene::World::instance(), &gameNav_);
    // Same "after flush" reasoning -- a perceiver's sight check reads this frame's actual position,
    // not last frame's. Needs dt (unlike AgentSystem::tick) for its own think-interval throttle.
    synapse::perceptionSystem().tick(scene::World::instance(), t.dt);
    // AFTER perceptionSystem: a behaviour's own "CanSeeTarget"/"HasTarget" conditions read THIS
    // frame's sight state, not last frame's.
    synapse::btSystem().tick(scene::World::instance(), t.dt);
#endif
#endif

    // AFTER flush, BEFORE the view matrix is built in onRender. Reading pawn transforms before the
    // flush would give last frame's, so the camera would trail the player by a frame -- which reads
    // as input lag rather than as an ordering bug.
    drivePlayCamera();

    // Polled after the feature has had its prePass. The build takes three frames by design; a
    // --pcg-volume-test run should be given at least that many.
    if (cfg_.pcgVolumeTest) checkPcgVolume();

    if (physSteps_ != lastReportedSteps_ && (physSteps_ <= 1 || physSteps_ % 600 == 0)) {
        AVER_INFO("[Game] physics: {} step(s) taken", physSteps_);
        lastReportedSteps_ = physSteps_;
    }

    // THE LAST THING onUpdate DOES, AND IT HAS TO BE IN onUpdate. Engine::frameStep runs
    //
    //     onUpdate -> beginFrame -> onRender -> endFrame
    //
    // and beginFrame takes the ONE snapshot of PerFrameCB into the GPU-visible buffer
    // (D3D12Device.cpp:2022 is the sole write to frameCBPtr_). setCamera, setLight and
    // setSkyAtmosphere only touch the CPU-side shadow copy.
    //
    // This used to be the first line of onRender, which is AFTER beginFrame -- so every pixel of
    // frame N was rasterised with frame N-1's gViewProj, camera position, sky, fog and cloud
    // constants. Worse than a uniform one-frame lag: viewProj_ is also what drawWorld culls
    // against, so culling used THIS frame's matrix while the GPU drew with the previous one, and
    // the two disagreed by exactly one frame of camera motion.
    //
    // The editor never had this bug -- SandboxApp sets its camera in onUpdate (SandboxApp.cpp:1115)
    // -- which is why it never showed up in the gates.
    pushFrame(e);
}

void GameApp::onRender(Engine& e) {
#if AVER_MODULE_SCENE
    // FIRST PIXELS. onUpdate's pushFrame set the camera before beginFrame uploaded the frame
    // constants, so viewProj_ is this frame's AND the GPU has the matching matrix. The frustum is
    // derived from it inside drawWorld rather than cached, because a stale frustum culls things
    // that are on screen.
    if (rhi::IDevice* dev = e.device()) {
        pbr::MaterialSystem* ms = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#endif
        drawWorld(*dev, viewProj_, content_, drawStats_, ms, skinnedScene_.get());
    }
#endif
    captureScreenshotIfDue(e);

    // ROLLING THE INPUT EDGES IS THE LAST THING THE FRAME DOES, and the ordering is not arbitrary.
    // Engine::run pumps the window at the TOP of the loop:
    //
    //     pumpEvents -> frameStep{ onUpdate -> beginFrame -> onRender -> endFrame }
    //
    // so a key pressed this frame is already in InputState by the time onUpdate runs. Calling
    // newFrame() at the start of onUpdate -- which is where it looks like it belongs -- would throw
    // away the edges that had just arrived, and the game would ignore every single tap while
    // handling held keys perfectly. Clearing here, after the frame's last reader, leaves the
    // accumulator empty for the next pumpEvents to fill.
    input_.newFrame();
}

void GameApp::captureScreenshotIfDue(Engine& e) {
    if (cfg_.screenshotPath.empty() || screenshotDone_ || cfg_.maxFrames == 0) return;
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // requestCapture/getFrameImage are a REQUEST/POLL pair (see RHI.hpp's own comment: getFrameImage
    // only has data "after a requestCapture completes"), so asking and reading cannot happen on the
    // SAME frame -- sandbox/src/SandboxApp.cpp's captureCheck asks a few frames before the run ends
    // for the identical reason. The x,y passed to requestCapture is irrelevant here (this only wants
    // the full frame image, not the single-pixel probe Sandbox also reads).
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
    // EXACT REVERSE REGISTRATION ORDER. The device holds bare pointers to every render feature, so
    // a feature that outlives its removal is a dangling call and one removed out of order can be
    // torn down while another still references it. voxiRenderer_ is a MEMBER held by value for
    // exactly this reason -- it must outlive the device, which it does by construction, but only if
    // it is unregistered before the device goes.
    rhi::IDevice* dev = e.device();
    if (dev && pcgAttached_) { dev->removeRenderFeature(&pcgVolume_); pcgAttached_ = false; }
    pcgVolume_.shutdown();
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered LAST of onInit's render features (after skinning and Voxi), so removed first among
    // them here.
    if (dev && particlesAttached_) {
        dev->removeRenderFeature(&particleRenderer_);
        particlesAttached_ = false;
    }
    particleRenderer_.shutdown();
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
    // AFTER the Voxi teardown: the material system lives inside VoxiRenderer, and dropping the
    // materials it holds handles to while it is still registered would leave the render feature
    // pointing at freed textures for however many frames remain.
    content_.releaseProjectMaterials();
    content_.setTextureFactory(nullptr);
#endif
#if AVER_MODULE_SCENE
    skinnedScene_.reset();
    // Before physics: unloading destroys entities AND removes their static bodies, and removing a
    // body from a shut-down physics world is the wrong order.
    level_.unload();
#endif
#if AVER_MODULE_PHYSICS
    aver_phys_shutdown();
#endif
#if AVER_MODULE_SCRIPTING
    // Drains every loaded graph/behaviour and closes the CLR host, if one ever came up. Nothing above
    // this line depends on the scripting host being alive during its own teardown, so exact ordering
    // against the physics/scene shutdown just above is not load-bearing the way registration order is
    // for a render feature -- unlike voxiRenderer_/skinnedScene_, ScriptHost never became a bare
    // pointer anything else in this class retained.
    if (scriptsReady_) { scripts_.shutdown(); scriptsReady_ = false; }
#endif

    // Reported unconditionally, including when it is zero. A silent zero is indistinguishable from
    // a broken counter, and "did the world simulate at all" is the first question asked when
    // gameplay does not move.
    setFileTrace(nullptr, nullptr);
    AVER_INFO("[Game] shutdown after {} frame(s), {} physics step(s)", frames_, physSteps_);
    (void)dev;
}

} // namespace aver::game
