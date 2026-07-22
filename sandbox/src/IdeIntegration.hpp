#pragma once
// Which code editors this machine actually has, how to make one of them jump to a file:line:col,
// and how to turn MSBuild's output back into the positions worth jumping to.
//
// A separate pair from ToolsMenu.hpp because none of it is UI. Detection is a process launch
// (`vswhere.exe`), parsing is string work with no ImGui in it, and both are wanted by more than
// the Tools dropdown — a compile error is the first caller, a Content Browser or a log line is the
// obvious next. Keeping it here means the menu asks a question rather than implementing one.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// The shell entry is not an IDE and is always last: it is what happens today — hand the path to
// whatever is registered for the extension — and it must survive a machine where nothing else is
// found, which is the only configuration guaranteed to work everywhere.
enum class IdeKind { VisualStudio, VsCode, Rider, ShellDefault };

struct IdeInfo {
    IdeKind kind = IdeKind::ShellDefault;
    std::string name;     // shown on the menu item, so it names what will actually open
    std::string exePath;  // empty for ShellDefault, which has no executable of its own
    // Whether this entry can be told a LINE, as opposed to merely a file. False means a click on a
    // diagnostic still opens the file — it just lands at the top, and the UI says so rather than
    // pretending the jump happened.
    bool canGoto = false;
};

// Every IDE found, in preference order, with ShellDefault always present at the end.
//
// The first call starts detection on a worker thread and returns immediately with the shell entry
// alone; later calls return the full list once the scan has finished. Callers are frame code, and
// `vswhere` is a process launch — so this never waits, and a menu drawn during the first few
// frames after startup simply offers the fallback. Call from the main thread only.
const std::vector<IdeInfo>& detectedIdes();

// False while the scan is still running. Only worth asking to phrase a tooltip honestly; the list
// above is always safe to use.
bool ideDetectionFinished();

// The head of the list: the real IDE if there is one, the shell otherwise. Never null.
//
// This is the "open my project" answer, and it prefers Visual Studio. Where to send a DIAGNOSTIC is
// a different question with a different answer — see preferredGotoIde.
const IdeInfo& preferredIde();

// Where a click on a diagnostic should go. Split from preferredIde because opening a `.csproj` and
// landing a caret on a line are not the same capability: every entry with `canGoto` can be handed a
// line, but only some of them reliably act on it, and the head of the preference list is not one of
// them (see the Visual Studio case in openInIde). Never null.
const IdeInfo& preferredGotoIde();

// Open `file` at a 1-based `line`/`col`. `line <= 0` means "just open it", which is also what
// happens when the IDE has no goto form. Returns false only when the process could not be started
// at all — an IDE that opened the wrong place still returns true, so do not read this as "the
// cursor is where you asked".
bool openInIde(const IdeInfo& ide, const std::string& file, int line = 0, int col = 0);

// Open a `.csproj` the way each IDE expects to receive a project: Visual Studio and Rider want the
// project file, VS Code wants the FOLDER containing it — handing Code a `.csproj` opens an XML
// document, which is not what "open my scripts" means.
bool openProjectInIde(const IdeInfo& ide, const std::string& csprojPath);

// One line of build output, parsed as far as it could be. `raw` is always the line verbatim: a
// line this could not understand is still a line the user has to be able to read, and swallowing
// compiler output because a regex missed is a worse failure than showing it plainly.
struct BuildLine {
    std::string raw;
    std::string file;      // absolute where it could be resolved; empty when there is no position
    int line = 0;          // 1-based; 0 = none
    int col = 0;           // 1-based; 0 = none
    bool isError = false;
    bool isWarning = false;
    std::string code;      // "CS1061", "MSB3021"; empty when the diagnostic carried none
    std::string message;   // the text after the code, with the trailing "[project]" removed
    // What a clickable row shows: the file's LEAF and the position, then the message. Built here
    // because a modal is redrawn every frame it is open and a label rebuilt per line per frame is
    // per-frame string work for a string that cannot have changed. Empty when there is no position,
    // because such a line is shown as `raw` and has no row of its own to label.
    std::string label;

    // Clickable exactly when there is somewhere to jump to.
    bool hasPosition() const { return !file.empty() && line > 0; }
};

// Parse a whole `dotnet build` transcript. EVERY line comes back, in order, whether or not it was
// understood. `baseDir` resolves relative paths — MSBuild emits them relative to the directory the
// build ran in, and a relative path is useless to an IDE launched from elsewhere.
//
// Duplicates are dropped: MSBuild prints each diagnostic once where it happened and again in the
// "Build FAILED." summary, and two copies of one error is a list — and a header count — that lies
// about how many things are wrong. This covers the position-less origins (`CSC :`, `MSBUILD :`)
// too, which MSBuild repeats just the same; those are matched on their text, since they have no
// position to match on. Only repeats of a DIAGNOSTIC are dropped, never ordinary output.
std::vector<BuildLine> parseBuildOutput(const std::string& output, const std::string& baseDir);

} // namespace aver::editor
