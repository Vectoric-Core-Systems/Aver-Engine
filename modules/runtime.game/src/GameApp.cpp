#include "aver/game/GameApp.hpp"

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"

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

void GameApp::openProject(Engine&) {
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
    openProject(e);
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine&, const Timestep&) {
    ++frames_;
    // Input is READ here, never rolled here. See onRender for why.
}

void GameApp::onRender(Engine&) {
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
    AVER_INFO("[Game] shutdown after {} frame(s)", frames_);
}

} // namespace aver::game
