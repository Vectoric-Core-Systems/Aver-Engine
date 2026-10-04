#pragma once
// Which code editors this machine has, how to make one jump to a file:line:col, and how to turn
// MSBuild output back into the positions worth jumping to.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// The IDEs this knows how to drive. ShellDefault is the always-available fallback.
enum class IdeKind { VisualStudio, VsCode, Rider, ShellDefault };

// One detected IDE.
struct IdeInfo {
    IdeKind kind = IdeKind::ShellDefault;
    std::string name;     // shown on the menu item
    std::string exePath;  // empty for ShellDefault
    bool canGoto = false; // whether it can be told a line, not merely a file
};

// Every IDE found, in preference order, with ShellDefault always last. The first call starts the
// scan on a worker thread and returns the shell entry alone. Main thread only.
const std::vector<IdeInfo>& detectedIdes();

// False while the detection scan is still running.
bool ideDetectionFinished();

// The head of the list: the real IDE if there is one, the shell otherwise. Never null.
const IdeInfo& preferredIde();

// Where a click on a diagnostic should go. Never null.
const IdeInfo& preferredGotoIde();

// Opens `file` at a 1-based `line`/`col`; `line <= 0` just opens it. False only when the process
// could not be started at all.
bool openInIde(const IdeInfo& ide, const std::string& file, int line = 0, int col = 0);

// Opens a `.csproj` the way each IDE expects it: the project file, or the folder for VS Code.
bool openProjectInIde(const IdeInfo& ide, const std::string& csprojPath);

// One line of build output, parsed as far as it could be. `raw` is always the line verbatim.
struct BuildLine {
    std::string raw;
    std::string file;      // absolute where it could be resolved; empty when there is no position
    int line = 0;          // 1-based; 0 = none
    int col = 0;           // 1-based; 0 = none
    bool isError = false;
    bool isWarning = false;
    std::string code;      // "CS1061", "MSB3021"; empty when the diagnostic carried none
    std::string message;   // the text after the code, with the trailing "[project]" removed
    std::string label;     // prebuilt row text: file leaf, position, message; empty with no position

    bool hasPosition() const { return !file.empty() && line > 0; }
};

// Parses a whole `dotnet build` transcript. Every line comes back, in order, understood or not.
// `baseDir` resolves MSBuild's relative paths; repeated diagnostics are dropped, other output is not.
std::vector<BuildLine> parseBuildOutput(const std::string& output, const std::string& baseDir);

} // namespace aver::editor
