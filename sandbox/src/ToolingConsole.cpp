// The `validate` and `trace` console commands. Its own translation unit for the reason WorldConsole.cpp
// is: EditorConsole.hpp only declares the registration function.
#include "EditorConsole.hpp"
#include "AssetValidateRun.hpp"

#include "aver/core/TraceCapture.hpp"

namespace aver::editor {

void registerToolingCommands(std::vector<ConsoleCommandDesc>& t) {
    t.push_back({"validate",
        "validate [path] -- check the project's assets (missing references, bad graphs, mesh slots, texture sizes); issues go to the Message Log",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            const std::string& dir = toolingContentDirSlot();
            if (dir.empty()) { print(LogLevel::Error, "No project is open."); return false; }
            print(LogLevel::Info, runAssetValidation(dir, args.empty() ? std::string() : args[0]));
            return true;
        }});
    t.push_back({"trace",
        "trace <frames> [file.json] | trace stop -- record CPU scopes (and averaged GPU passes) for N frames to a Chrome trace file "
        "(open it in chrome://tracing or Perfetto)",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            if (!args.empty() && args[0] == "stop") {
                if (!trace::stop()) { print(LogLevel::Error, "No capture is running."); return false; }
                print(LogLevel::Info, "Capture written to " + trace::lastWrittenPath());
                return true;
            }
            const int frames = args.empty() ? 0 : std::atoi(args[0].c_str());
            if (frames < 1) { print(LogLevel::Error, "Usage: trace <frames> [file.json] | trace stop"); return false; }
            const std::string path = args.size() > 1 ? args[1] : std::string("aver_trace.json");
            if (!trace::begin(static_cast<u32>(frames), path)) {
                print(LogLevel::Error, "A capture is already running; 'trace stop' ends it.");
                return false;
            }
            print(LogLevel::Info, "Capturing " + std::to_string(frames) + " frames to " + path + ".");
            return true;
        }});
}

}  // namespace aver::editor
