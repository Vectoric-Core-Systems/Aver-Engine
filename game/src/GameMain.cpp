// AverGame.exe -- the standalone game runtime. The second aver::Application in the tree, and the
// first one that is not an editor.
//
// Deliberately tiny. Everything is in Aver.Runtime.Game so that the editor can eventually CALL the
// same code rather than keep its own copy of it; an executable that accumulates logic is how
// Sandbox.exe became impossible to ship a game out of.
#include "aver/game/GameApp.hpp"
#include "aver/runtime/EntryPoint.hpp"

namespace aver {

Application* createApplication(int argc, char** argv) {
    return new game::GameApp(game::parseArgs(argc, argv));
}

} // namespace aver
