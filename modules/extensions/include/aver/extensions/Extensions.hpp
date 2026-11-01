#pragma once
// The extension loader (docs/EXTENSIONS.md): finds the extensions installed beside the executable, checks each
// manifest, and loads an extension's native library the first time one of its commands runs. The native contract is aver_ext.h; what a
// command prints for the tool that runs it is aver_ext_protocol.h (CommandMessage below).
#include "aver/core/Types.hpp"
#include "aver/extensions/aver_ext.h"
#include "aver/extensions/aver_ext_protocol.h"
#include "aver/extensions/aver_ext_services.h"

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

// A File menu entry an extension adds (read by the editor's import dialog). The command it runs speaks the protocol of aver_ext_protocol.h
// (parseCommandMessage); nothing else about the command is known to the dialog.
struct ImporterInfo {
    std::string id;           // stable name of the entry (default: the command)
    std::string menu;         // "Import Thing..."
    std::string startButton;  // a button on the start screen, under "Open Project...", that opens the same dialog; empty: no button
    std::string tooltip;      // the menu item's hover text (default: `description`)
    std::string description;  // the paragraph at the top of the dialog
    std::string command;      // the extension command AverAssetC runs as: <command> <input> <output> [flags]

    struct Input {
        std::string kind = "folder";          // "folder" or "file"
        std::vector<std::string> patterns;    // file name patterns ("*.gltf"): what the picker offers, and for a "file" input what is accepted
        std::string detect;                   // folder input: a file whose presence marks a valid folder; empty for any folder
    } input;
    std::string outputDefault;                // the default output beside the input: "{input}" is the input's name ("{input}_out"); empty: "{input}_out"

    struct Option {
        std::string flag;     // appended to the command line when ticked
        std::string label;
        bool onByDefault = false;
    };
    std::vector<Option> options;
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
    std::string managedEntry;               // the managed.entry type (Aver.Framework.Hosting.IAverExtension); empty for none
    std::string actorBase;                  // the base class of the actor types they declare
    std::vector<CommandInfo> commands;
    std::vector<ImporterInfo> importers;
    std::vector<std::string> runtimeFiles;  // what a packaged game copies
    Status status = Status::Refused;
    std::string reason;                     // why it is refused; empty when Ready

    bool ready() const { return status == Status::Ready; }
};

// What the running application offers an extension through AverExtHost directly. A null function falls back to the default noted.
struct HostServices {
    void (*log)(i32 level, const char* msg) = nullptr;                // the engine log
    void (*progress)(double fraction, const char* text) = nullptr;    // dropped
};

// True when `name` follows the naming rule for services and interfaces (aver_ext.h): two or more '.'-joined segments of [a-z0-9-], none empty
// or starting with '-', at most 128 characters in all.
bool isValidInterfaceName(std::string_view name);

// The services the running application offers, by name and version (AverExtHost::getService). The host fills it before an extension loads; an
// extension reaches it through the one function pointer, which carries no context, so there is one per process. A table must outlive its entry.
class ServiceRegistry {
public:
    static ServiceRegistry& instance();

    // False when `name` breaks the naming rule, `version` is 0, `table` is null or (name, version) is already added.
    bool add(std::string_view name, u32 version, const void* table);
    bool remove(std::string_view name, u32 version);
    // The table added for exactly (name, version), or null.
    const void* find(std::string_view name, u32 version) const;
    void clear();

private:
    ServiceRegistry();
    ~ServiceRegistry();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The directory name an extension's native library sits under: "win-x64", "linux-x64", "osx-arm64" ...
const char* platformDirName();
// "<name>.dll", "<name>.so" or "<name>.dylib".
std::string nativeFileName(std::string_view name);
// True when `version` satisfies `range`: space-separated comparators (>=, >, <=, <, =, or a bare version for =), all of which must hold.
// `why` says what is wrong with a range that does not parse or a version it excludes.
bool versionInRange(std::string_view version, std::string_view range, std::string* why = nullptr);

// The manifest's own fields from the text of an extension.json (name, version, ABI, engine range, commands, importers, listed files); status, directory
// and native path stay unset. False with `why` when the text is not a JSON object. Nothing is checked against a directory: see Registry::scan.
bool parseExtensionManifest(const std::string& text, Extension& out, std::string& why);
// Why a parsed manifest cannot run on an engine of `engineVersion` (its ABI, its engine range), or empty. The files it lists are not looked at.
std::string manifestIncompatibility(const Extension& manifest, std::string_view engineVersion);

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

    // The interface `name` at `version` the extension offers (AverExtApi::getInterface), loading the extension first. Null with `why` when it
    // cannot load, and null without when it offers no such interface. The table belongs to the extension and lives until it is unloaded.
    const void* interfaceOf(std::string_view extension, std::string_view name, u32 version, std::string* why = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- the command protocol (aver_ext_protocol.h) ----

enum class MessageKind { None, Progress, Result };

// One protocol line a command printed.
struct CommandMessage {
    MessageKind kind = MessageKind::None;
    double fraction = -1.0;   // Progress: [0, 1], or -1 when unknown
    std::string text;         // Progress: what is being done; may be empty
    bool ok = false;          // Result: the command did what was asked
    std::string open;         // Result: a file the tool may offer to open; empty for none
    std::string report;       // Result: a file with the details; empty for none
    i32 warnings = 0;         // Result
    i32 errors = 0;           // Result
    std::string message;      // Result: one line for a person, the reason when !ok
};

// kind None for a line that is not a protocol line (anything but a JSON object whose "aver" is a kind this version knows).
CommandMessage parseCommandMessage(std::string_view line);
// The line for a message (without the newline); empty for kind None.
std::string formatCommandMessage(const CommandMessage& message);

// The EXTENSION lines of `project` that `registry` cannot satisfy, in the project's order: no entry of that name, or one that is Refused.
// Each result says which, in a sentence a person can act on ("project \"X\" needs the extension Y 1.0, which is not installed (...)").
std::vector<std::string> missingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry);
// Logs one warning line per missing extension, naming it. Called when a project opens; returns how many were missing.
usize warnMissingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry);
// The same against a registry that scans <executable dir>/Extensions.
usize warnMissingProjectExtensions(const fmt::ProjectDesc& project);

}  // namespace aver::ext
