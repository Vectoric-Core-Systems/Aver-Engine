// The world.* console commands, registered with the editor console (the logic is WorldConsole.hpp). Its own translation unit, like
// PhysicsConsoleVars.hpp's phys.* commands: EditorConsole.hpp reaches TUs at the Debug COFF section cap and only declares the function.
#include "EditorConsole.hpp"
#include "WorldConsole.hpp"

#if AVER_MODULE_SCENE

namespace aver::editor {

void registerWorldCommands(std::vector<ConsoleCommandDesc>& t) {
    struct Row { const char* name; const char* help; };
    static const Row rows[] = {
        {"world.origin",  "Print the floating origin: region, origin and camera in km, generation, shifts, deferrals and the last shift's cost"},
        {"world.rebase",  "world.rebase <dxRegions> <dyRegions> -- move the origin by whole regions (16.384 km) at the start of the next frame"},
        {"world.margin",  "world.margin [cm] -- how far past a region's half-width the camera may go before the origin follows (0 to 229376)"},
        {"world.enable",  "world.enable [0|1] -- turn the floating origin off or on (off: the origin stays where it is)"},
        {"world.policy",  "world.policy [follow|fixed|manual] -- follow the camera, never move, or move only on world.rebase"},
        {"world.tp",      "world.tp <xKm> <yKm> <zKm> -- put the camera at an absolute position and move the origin to it, in one step"},
    };
    for (const Row& r : rows) {
        const std::string name = r.name;
        t.push_back({name, r.help,
            [name](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
                WorldConsoleHost* h = worldConsoleHostSlot();
                if (!h) { print(LogLevel::Error, "the floating origin is not available here"); return false; }
                std::vector<std::string> lines;
                const bool ok = runWorldCommand(*h, name, args, lines);
                for (const std::string& l : lines) print(ok ? LogLevel::Info : LogLevel::Error, l);
                return ok;
            }});
    }
}

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
