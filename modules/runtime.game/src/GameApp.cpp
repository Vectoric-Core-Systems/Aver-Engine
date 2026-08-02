#include "aver/game/GameApp.hpp"

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
    if (aver_phys_init()) {
        groundBody_ = aver_phys_add_static_box(0.0f, 0.0f, -kGroundHalfThickCm,
                                               kGroundHalfExtentCm, kGroundHalfExtentCm,
                                               kGroundHalfThickCm);
        AVER_INFO("[Game] physics started, ground body={} (fixed step {:.4f}s)",
                  groundBody_, aver_phys_fixed_step());
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
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity        = fog;
    sky_.cloudTime         = cloudTime_;
    dev->setSkyAtmosphere(sky_);
    // NOT the editor's 0.055 chrome grey. Nothing outside a game's viewport is chrome, because a
    // game has no outside -- anything the sky does not cover is a bug the player should see as
    // black, not as a colour that looks deliberate.
    dev->setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev->setPostProcess(post_);
}

void GameApp::onInit(Engine& e) {
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
    attachVoxi(e);
    initPhysics();      // BEFORE openProject: level load builds a static body per colliding placement
    openProject(e);
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    ++frames_;
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

#if AVER_MODULE_SCENE
    // The animation clock ticks UNCONDITIONALLY, not from the gameplay groups above. Those are
    // gated on PLAYING, and hanging animation off them would freeze every animated thing the moment
    // a session was not running. Deliberate asymmetry, copied from the editor.
    anim::animSystem().tick(scene::World::instance(), t.dt);
    // Retires deferred destroys, rebuilds the topological order and recomposes stale world
    // matrices. Without it World::worldMatrix reads uncomposed matrices and the draw walk in C7
    // would place everything at the origin -- which looks like a broken transform pipeline and is
    // really a missing flush.
    scene::World::instance().flush();
#endif

    // AFTER flush, BEFORE the view matrix is built in onRender. Reading pawn transforms before the
    // flush would give last frame's, so the camera would trail the player by a frame -- which reads
    // as input lag rather than as an ordering bug.
    drivePlayCamera();

    if (physSteps_ != lastReportedSteps_ && (physSteps_ <= 1 || physSteps_ % 600 == 0)) {
        AVER_INFO("[Game] physics: {} step(s) taken", physSteps_);
        lastReportedSteps_ = physSteps_;
    }
}

void GameApp::onRender(Engine& e) {
    pushFrame(e);

#if AVER_MODULE_SCENE
    // FIRST PIXELS. pushFrame set the camera, so viewProj_ is this frame's; the frustum is derived
    // from it inside drawWorld rather than cached, because a stale frustum culls things that are on
    // screen.
    if (rhi::IDevice* dev = e.device()) {
        pbr::MaterialSystem* ms = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#endif
        drawWorld(*dev, viewProj_, content_, drawStats_, ms);
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

void GameApp::onShutdown(Engine&) {
    // Reported unconditionally, including when it is zero. A silent zero is indistinguishable from
    // a broken counter, and "did the world simulate at all" is the first question asked when
    // gameplay does not move.
    AVER_INFO("[Game] shutdown after {} frame(s), {} physics step(s)", frames_, physSteps_);
}

} // namespace aver::game
