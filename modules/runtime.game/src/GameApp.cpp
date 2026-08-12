#include "aver/game/GameApp.hpp"

#include <filesystem>

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#endif
#if AVER_MODULE_VOXI
#  include "aver/voxi/Voxi.hpp"
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

namespace aver::game {
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
        else if (std::strcmp(a, "--pcg-volume-test") == 0) { c.pcgVolumeTest = true; }
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
        AVER_INFO("[Game] Voxi attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}",
                  caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);
    } else {
        AVER_WARN("[Game] Voxi failed to initialise; the world draws untextured");
    }
#else
    (void)e;
#endif
}

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
    level_.loadStartMap(project_, content_);
#endif
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
    } else {
        AVER_INFO("[Game] scripting host unavailable ({}) -- graphs and any other C# gameplay will not run",
                  scripts_.declineReason());
    }
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
    for (const std::string& path : paths) {
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
    AVER_INFO("[Graph] {} of {} project graph(s) loaded", loaded, projectGraphs_.size());
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

    f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
    if (level_.hasFog()) fog = level_.fogDensity();
#endif
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
    AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={}",
              AVER_MODULE_PBR, AVER_MODULE_SCENE, AVER_MODULE_VOXI,
              AVER_MODULE_PHYSICS, AVER_MODULE_FRAMEWORK, AVER_MODULE_SCRIPTING);
    // FIRST of the render features. Its prePass stages this frame's bone matrices, and the scene
    // pass then asks drawHandle() for a posed handle that must already exist.
    attachSkinning(e);
    attachVoxi(e);
    if (cfg_.pcgVolumeTest) attachPcgTest(e);
    initPhysics();      // BEFORE openProject: level load builds a static body per colliding placement
    openProject(e);
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
    const bool haveScriptAssembly = std::filesystem::exists(project_.binariesDir() + "\Scripts");
    if (haveGraphs || haveScriptAssembly) {
        initScripting();
        discoverProjectGraphs();
    } else {
        AVER_INFO("[Game] no .ocgraph content and no Scripts assembly -- scripting host not started");
    }
#endif
    // AFTER scripting is up (so any project GameMode is declared) and AFTER the graph/behaviour report
    // just above (so a reader sees what loaded before seeing whether it started playing). See this
    // function's own comment for why "declares a GameMode" is the generic, content-driven switch this
    // is gated on, matching the shape haveGraphs/haveScriptAssembly already uses just above.
    beginPlayIfGameModeDeclared();
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    ++frames_;
    cloudTime_ += t.dt;
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

#if AVER_MODULE_SCENE
    // The animation clock ticks UNCONDITIONALLY, not from the gameplay groups above. Those are
    // gated on PLAYING, and hanging animation off them would freeze every animated thing the moment
    // a session was not running. Deliberate asymmetry, copied from the editor.
    anim::animSystem().tick(scene::World::instance(), t.dt);
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
    // Nothing drawn yet: Engine::frameStep() already does beginFrame/endFrame around this, so the
    // swapchain is cleared and presented. The world draw walk lands here in a later slice.

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

void GameApp::onShutdown(Engine& e) {
    // EXACT REVERSE REGISTRATION ORDER. The device holds bare pointers to every render feature, so
    // a feature that outlives its removal is a dangling call and one removed out of order can be
    // torn down while another still references it. voxiRenderer_ is a MEMBER held by value for
    // exactly this reason -- it must outlive the device, which it does by construction, but only if
    // it is unregistered before the device goes.
    rhi::IDevice* dev = e.device();
    if (dev && pcgAttached_) { dev->removeRenderFeature(&pcgVolume_); pcgAttached_ = false; }
    pcgVolume_.shutdown();
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
