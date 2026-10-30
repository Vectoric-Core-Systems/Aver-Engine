#pragma once
// The extension loader (docs/plans/fabric/EXTENSION.md section 3.3): finds the extensions installed beside the executable, checks each
// manifest, and loads an extension's native library the first time one of its commands runs. The native contract is aver_ext.h.
#include "aver/core/Types.hpp"
#include "aver/extensions/aver_ext.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt { struct ProjectDesc; }

namespace aver::ext {

struct CommandInfo {
    std::string name;
    std::string summary;   // one line for --help
    std::string usage;     // the arguments after the name, as --help prints them; may be empty
};

// A File menu entry an extension adds (read by the editor's import dialog). Everything after `opensResult` is optional wording and options for
// that dialog; an empty string means the dialog's own default.
struct ImporterInfo {
    std::string menu;
    std::string command;
    std::string input;     // "folder" or "file"
    std::string detect;    // a file whose presence marks a valid input; empty for none
    bool opensResult = false;

    // A key=value line in `detect` that must reach a minimum: "detectVersion": {"key", "min", "message"} ({value} in the message is what the file holds).
    std::string detectKey;
    i32 detectMin = 0;
    std::string detectMessage;

    std::string title;        // the dialog's title (default: `menu` without its trailing dots)
    std::string tooltip;      // the menu item's hover text
    std::string description;  // the paragraph at the top of the dialog
    std::string inputLabel;   // what the input is called ("Source project")
    std::string outputLabel;  // the output folder's caption
    std::string outputSuffix; // the default output folder is "<input folder><suffix>" beside the input
    std::string openLabel;    // the button that opens the result
    std::string busyText;     // shown while the command runs
    std::string logTag;       // prefix of the lines the dialog logs (default: the extension's display name)
    std::string notifyRunning, notifyDone, notifyChecked, notifyFailed;   // the notification's titles

    struct Option {
        std::string flag;     // appended to the command line when ticked
        std::string label;
        bool onByDefault = false;
    };
    std::vector<Option> options;

    // The command's closing line `{"schemaVersion":1,"summary":true,"kind":"<command>", ...}`: {key} in these is that line's value.
    struct Note {
        std::string text;
        std::string when;     // the note shows when this key is true or non-zero (empty: always)
        std::string unless;   // ... and this key is not
    };
    std::string summaryText;      // the line under the log
    std::string summaryBrief;     // the notification's body, before ". Report: <path>"
    std::string summaryWarnKey;   // a non-zero value makes the notification a warning
    std::vector<Note> notes;
};

enum class Status {
    Ready,     // the manifest checks out; the library loads on first use
    Refused,   // never loaded; `reason` says why
};

// One <Extensions>/<Name>/ directory, as scanned.
struct Extension {
    std::string name, displayName, version, license, source;
    std::string dirName;                    // the directory's own name
    std::string dir;                        // its path
    i32 abi = 0;
    std::string engineRange;                // ">=0.7.0 <0.9.0"
    std::string nativeName;                 // the manifest's `native`: the library's name without directory or suffix
    std::string nativePath;                 // that library for this platform; empty when the manifest names none
    std::vector<std::string> managedRuntime;   // managed/<file> assemblies a game loads
    std::string actorBase;                  // the base class of the actor types they declare
    std::vector<CommandInfo> commands;
    std::vector<ImporterInfo> importers;
    std::vector<std::string> runtimeFiles;  // what a packaged game copies
    Status status = Status::Refused;
    std::string reason;                     // why it is refused; empty when Ready

    bool ready() const { return status == Status::Ready; }
};

// What the running application offers an extension through AverExtHost. A null function falls back to the default noted.
struct HostServices {
    void (*log)(i32 level, const char* msg) = nullptr;                                   // the engine log
    i32 (*importModel)(const AverExtModelArgs* args, AverExtModelResult* out) = nullptr;   // refuses: ok = 0
    void (*freeModelResult)(AverExtModelResult* result) = nullptr;
    void (*progress)(double fraction, const char* text) = nullptr;                       // dropped
};

// The directory name an extension's native library sits under: "win-x64", "linux-x64", "osx-arm64" ...
const char* platformDirName();
// "<name>.dll", "<name>.so" or "<name>.dylib".
std::string nativeFileName(std::string_view name);
// True when `version` satisfies `range`: space-separated comparators (>=, >, <=, <, =, or a bare version for =), all of which must hold.
// `why` says what is wrong with a range that does not parse or a version it excludes.
bool versionInRange(std::string_view version, std::string_view range, std::string* why = nullptr);

class Registry {
public:
    Registry();
    ~Registry();                      // shuts every loaded extension down and unloads it
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void setHostServices(const HostServices& services);
    void setEngineVersion(std::string_view version);   // default: the engine's own; tests set it
    void setExecutableDir(std::string_view dir);       // default: the running executable's directory

    // Adds every <root>/<Name>/extension.json found. An entry that fails its checks stays in the list, Refused with the reason. A name already
    // listed from an earlier scan is refused as a duplicate. Returns the number of entries added.
    usize scan(const std::string& root);
    // scan(<executable directory>/Extensions).
    usize scanDefault();
    // Forgets every entry (and shuts down any that was loaded).
    void clear();

    const std::vector<Extension>& extensions() const;
    const Extension* find(std::string_view name) const;
    // The Ready extension that lists `command`, or null.
    const Extension* findCommand(std::string_view command) const;

    // Loads the extension's library and calls its entry, once. False with `why` when it cannot: the entry is then Refused for good.
    bool load(std::string_view name, std::string* why = nullptr);
    bool isLoaded(std::string_view name) const;

    // Runs `command` (loading its extension first). argv[0] is the command's name, as in AverExtApi::runCommand. True when it ran, with the
    // extension's exit code in `exitCode`; false with `why` when no Ready extension lists it or the library would not load.
    bool run(std::string_view command, int argc, const char* const* argv, int& exitCode, std::string* why = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The EXTENSION lines of `project` that `registry` cannot satisfy, in the project's order: no entry of that name, or one that is Refused.
// Each result says which, in a sentence a person can act on ("project \"X\" needs the extension Y 1.0, which is not installed (...)").
std::vector<std::string> missingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry);
// Logs one warning line per missing extension, naming it. Called when a project opens; returns how many were missing.
usize warnMissingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry);
// The same against a registry that scans <executable dir>/Extensions.
usize warnMissingProjectExtensions(const fmt::ProjectDesc& project);

}  // namespace aver::ext
