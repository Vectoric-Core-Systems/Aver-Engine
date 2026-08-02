// GameApp: the aver::Application a shipped game runs, as opposed to the editor.
#pragma once
#include "aver/runtime/Application.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/game/GameContent.hpp"

#include <string>

namespace aver::game {

// Everything GameApp needs that comes off the command line or out of game.json.
struct GameConfig {
    std::string title = "Aver Game";
    u32 width = 1280;
    u32 height = 720;
    u64 maxFrames = 0;      // 0 = run until the window closes
    bool headless = false;
    bool useWarp = false;
    bool debugLayer = false;
    std::string backend;    // empty = the compiled-in default order
    // The .ocproject to open. A packaged game passes Game.ocproject; empty means no world.
    std::string projectPath;
};

// Parses the arguments a game executable accepts. Unknown arguments are ignored rather than fatal:
// a launcher or a store client can append its own, and refusing to start because of one would be a
// bad trade for a shipped product.
GameConfig parseArgs(int argc, char** argv);

// The game-side Application.
//
// WHY THIS EXISTS SEPARATELY FROM SandboxApp. Sandbox.exe IS the editor -- it is the only
// aver::Application in the tree, and roughly two thousand lines of what a game needs (content
// indexing, level load, the world draw walk, the tick-group ordering, the play camera) live inside
// it, interleaved with dockspaces and inspector panels. A game cannot link that, and this class is
// where the game half moves to. Right now it is the window and the frame loop; the lifts land in
// later slices, one subsystem per commit, so that a black screen never has two candidate causes.
class GameApp final : public Application {
public:
    explicit GameApp(GameConfig cfg);

    BootConfig config() const override;
    void onInit(Engine&) override;
    void onUpdate(Engine&, const Timestep&) override;
    void onRender(Engine&) override;
    void onShutdown(Engine&) override;

    // The accumulated keyboard and mouse state for this frame.
    const InputState& input() const { return input_; }

    // The open project. Invalid until openProject succeeds.
    const fmt::ProjectDesc& project() const { return project_; }

    // The project's asset index.
    const GameContent& content() const { return content_; }

private:
    // Loads cfg_.projectPath. Logs and leaves project_ invalid on failure rather than aborting: a
    // game with no world is a diagnosable state, and a process that dies before its first frame
    // tells the player nothing.
    void openProject(Engine&);

    GameConfig cfg_;
    InputState input_;
    fmt::ProjectDesc project_;
    GameContent content_;
    u64 frames_ = 0;
};

} // namespace aver::game
