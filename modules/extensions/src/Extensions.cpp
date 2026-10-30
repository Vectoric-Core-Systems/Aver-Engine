// The extension loader: scan <exeDir>/Extensions/*/extension.json, validate, and load a native library on first use.
#include "aver/extensions/Extensions.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/Json.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>

namespace aver::ext {

namespace fsys = std::filesystem;

namespace {

constexpr i32 kSupportedAbi = AVER_EXT_ABI;

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string str(const fmt::JsonValue& v) { return std::string(v.asString()); }

fsys::path toPath(const std::string& utf8) { return fsys::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())); }
std::string leafName(const fsys::path& p) {
    const std::u8string u = p.filename().u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// '*' matches any run of characters; nothing else is special.
bool wildcardMatch(std::string_view pattern, std::string_view text) {
    usize p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (p < pattern.size() && pattern[p] == text[t]) { ++p; ++t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    for (usize at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

// True when `relPattern` (slash-separated, '*' only in the last segment) names a file under `dir`.
bool listedFileExists(const std::string& dir, const std::string& relPattern) {
    const usize slash = relPattern.find_last_of('/');
    const std::string folder = slash == std::string::npos ? std::string() : relPattern.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? relPattern : relPattern.substr(slash + 1);
    const std::string where = folder.empty() ? dir : dir + "/" + folder;
    if (leaf.find('*') == std::string::npos) return fileExists(where + "/" + leaf);
    std::error_code ec;
    for (fsys::directory_iterator it(toPath(where), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && wildcardMatch(leaf, leafName(it->path()))) return true;
    }
    return false;
}

void readStringList(const fmt::JsonValue& v, std::vector<std::string>& out) {
    if (!v.isArray()) return;
    for (const fmt::JsonValue& e : v.elements()) if (e.isString()) out.push_back(str(e));
}

// The manifest's own fields. Returns false with `why` when it is not an object we can read.
bool parseManifest(const std::string& text, Extension& e, std::string& why) {
    std::string_view body = text;
    if (body.size() >= 3 && body.substr(0, 3) == "\xEF\xBB\xBF") body.remove_prefix(3);
    fmt::JsonValue root;
    std::string err;
    if (!fmt::parseJson(body, root, &err)) { why = "extension.json is not valid JSON: " + err; return false; }
    if (!root.isObject()) { why = "extension.json is not a JSON object"; return false; }
    e.name = str(root["name"]);
    e.displayName = str(root["displayName"]);
    e.version = str(root["version"]);
    e.license = str(root["license"]);
    e.source = str(root["source"]);
    e.abi = root["abi"].isNumber() ? i32(root["abi"].asInt()) : 0;
    e.engineRange = str(root["engine"]);
    e.nativeName = str(root["native"]);
    const fmt::JsonValue& managed = root["managed"];
    readStringList(managed["runtime"], e.managedRuntime);
    e.actorBase = str(managed["actorBase"]);
    for (const fmt::JsonValue& c : root["commands"].elements())
        if (c.isObject() && !str(c["name"]).empty()) e.commands.push_back({str(c["name"]), str(c["summary"]), str(c["usage"])});
    for (const fmt::JsonValue& i : root["importers"].elements()) {
        if (!i.isObject()) continue;
        ImporterInfo m;
        m.menu = str(i["menu"]);
        m.command = str(i["command"]);
        m.input = str(i["input"]);
        m.detect = str(i["detect"]);
        m.opensResult = i["opensResult"].asBool();
        const fmt::JsonValue& dv = i["detectVersion"];
        m.detectKey = str(dv["key"]);
        m.detectMin = i32(dv["min"].asInt());
        m.detectMessage = str(dv["message"]);
        m.title = str(i["title"]);
        m.tooltip = str(i["tooltip"]);
        m.description = str(i["description"]);
        m.inputLabel = str(i["inputLabel"]);
        m.outputLabel = str(i["outputLabel"]);
        m.outputSuffix = str(i["outputSuffix"]);
        m.openLabel = str(i["openLabel"]);
        m.busyText = str(i["busyText"]);
        m.logTag = str(i["logTag"]);
        const fmt::JsonValue& nt = i["notify"];
        m.notifyRunning = str(nt["running"]);
        m.notifyDone = str(nt["done"]);
        m.notifyChecked = str(nt["checked"]);
        m.notifyFailed = str(nt["failed"]);
        for (const fmt::JsonValue& o : i["options"].elements())
            if (o.isObject() && !str(o["flag"]).empty()) m.options.push_back({str(o["flag"]), str(o["label"]), o["default"].asBool()});
        const fmt::JsonValue& sm = i["summary"];
        m.summaryText = str(sm["text"]);
        m.summaryBrief = str(sm["brief"]);
        m.summaryWarnKey = str(sm["warnIfNonZero"]);
        for (const fmt::JsonValue& n : sm["notes"].elements())
            if (n.isObject()) m.notes.push_back({str(n["text"]), str(n["when"]), str(n["unless"])});
        e.importers.push_back(std::move(m));
    }
    readStringList(root["runtimeFiles"], e.runtimeFiles);
    if (e.displayName.empty()) e.displayName = e.name;
    return true;
}

// Why this manifest cannot be used on this engine and platform, or empty.
std::string refusalReason(Extension& e, std::string_view engineVersion) {
    if (e.name.empty()) return "extension.json has no name";
    if (e.name != e.dirName) return "the name \"" + e.name + "\" does not match the directory \"" + e.dirName + "\"";
    if (e.abi != kSupportedAbi)
        return "it was built for extension ABI " + std::to_string(e.abi) + "; this engine supports ABI " + std::to_string(kSupportedAbi);
    if (e.engineRange.empty()) return "extension.json names no engine range";
    std::string why;
    if (!versionInRange(engineVersion, e.engineRange, &why)) return why;

    if (!e.nativeName.empty()) {
        const std::string rel = std::string("native/") + platformDirName() + "/" + nativeFileName(e.nativeName);
        e.nativePath = e.dir + "/" + rel;
        if (!fileExists(e.nativePath)) return "missing file " + rel;
    } else if (!e.commands.empty()) {
        return "it lists commands but names no native library";
    }
    for (const std::string& m : e.managedRuntime)
        if (!fileExists(e.dir + "/managed/" + m)) return "missing file managed/" + m;
    for (const std::string& f : e.runtimeFiles) {
        const std::string rel = replaceAll(f, "{platform}", platformDirName());
        if (!listedFileExists(e.dir, rel)) return "missing file " + rel;
    }
    return {};
}

void defaultLog(i32 level, const char* msg) {
    const i32 clamped = std::clamp<i32>(level, AVER_EXT_LOG_TRACE, AVER_EXT_LOG_CRITICAL);   // a fatal from outside never ends the host
    logMsg(static_cast<LogLevel>(clamped), "{}", msg ? msg : "");
}

i32 refuseImport(const AverExtModelArgs*, AverExtModelResult* out) {
    if (out) { out->ok = 0; out->error = "the host offers no model importer"; out->parts = nullptr; out->partCount = 0; out->notes = nullptr; out->noteCount = 0; }
    return 1;
}
void freeNothing(AverExtModelResult*) {}
void dropProgress(double, const char*) {}

}  // namespace

const char* platformDirName() {
#if defined(_WIN32)
#  if defined(_M_ARM64)
    return "win-arm64";
#  else
    return "win-x64";
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "osx-arm64";
#  else
    return "osx-x64";
#  endif
#else
#  if defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-x64";
#  endif
#endif
}

std::string nativeFileName(std::string_view name) {
#if defined(_WIN32)
    return std::string(name) + ".dll";
#elif defined(__APPLE__)
    return std::string(name) + ".dylib";
#else
    return std::string(name) + ".so";
#endif
}

bool versionInRange(std::string_view version, std::string_view range, std::string* why) {
    bool any = false, bad = false, outside = false;
    usize i = 0;
    auto isSep = [](char c) { return c == ' ' || c == '\t' || c == ','; };
    while (i < range.size()) {
        while (i < range.size() && isSep(range[i])) ++i;
        if (i >= range.size()) break;
        usize j = i;
        while (j < range.size() && !isSep(range[j])) ++j;
        std::string_view tok = range.substr(i, j - i);
        i = j;
        enum { Eq, Gt, Ge, Lt, Le } op = Eq;
        usize k = 0;
        if (tok.substr(0, 2) == ">=") { op = Ge; k = 2; }
        else if (tok.substr(0, 2) == "<=") { op = Le; k = 2; }
        else if (tok[0] == '>') { op = Gt; k = 1; }
        else if (tok[0] == '<') { op = Lt; k = 1; }
        else if (tok[0] == '=') { op = Eq; k = 1; }
        const std::string_view ver = tok.substr(k);
        if (ver.empty() || ver[0] < '0' || ver[0] > '9') { bad = true; continue; }
        any = true;
        const int c = compareVersions(version, ver);
        const bool ok = op == Eq ? c == 0 : op == Gt ? c > 0 : op == Ge ? c >= 0 : op == Lt ? c < 0 : c <= 0;
        if (!ok) outside = true;
    }
    if (bad || !any) {
        if (why) *why = "the engine range \"" + std::string(range) + "\" is not understood";
        return false;
    }
    if (outside) {
        if (why) *why = "it needs engine " + std::string(range) + "; this is " + std::string(version);
        return false;
    }
    return true;
}

struct Registry::Impl {
    struct Loaded {
        void* lib = nullptr;
        AverExtApi api{};
        AverExtHost host{};
        std::string engineVersion, exeDir, extDir;
    };
    mutable std::mutex mu;
    std::vector<Extension> list;
    std::map<std::string, std::unique_ptr<Loaded>> loaded;
    HostServices services;
    std::string engineVersion{kEngineVersion};
    std::string exeDir;
    bool exeDirSet = false;

    std::string executable() { return exeDirSet ? exeDir : executableDir(); }

    void unloadAllLocked() {
        for (auto& [name, l] : loaded) {
            if (l->api.shutdown) l->api.shutdown();
            if (l->lib) freeLibrary(l->lib);
        }
        loaded.clear();
    }

    Extension* findLocked(std::string_view name) {
        for (Extension& e : list) if (e.name == name) return &e;
        return nullptr;
    }

    bool loadLocked(std::string_view name, std::string* why) {
        auto fail = [&](Extension& e, std::string msg) {
            e.status = Status::Refused;
            e.reason = msg;
            if (why) *why = std::move(msg);
            return false;
        };
        Extension* e = findLocked(name);
        if (!e) { if (why) *why = "no extension named \"" + std::string(name) + "\""; return false; }
        if (!e->ready()) { if (why) *why = e->reason; return false; }
        if (loaded.count(e->name)) return true;
        if (e->nativePath.empty()) return fail(*e, "it has no native library to load");

        void* lib = loadLibrary(e->nativePath.c_str());
        if (!lib) return fail(*e, "the library did not load: " + libraryError());
        auto entry = reinterpret_cast<AverExtensionEntryFn>(librarySymbol(lib, AVER_EXT_ENTRY_NAME));
        if (!entry) { freeLibrary(lib); return fail(*e, "the library has no " AVER_EXT_ENTRY_NAME); }

        auto l = std::make_unique<Loaded>();
        l->lib = lib;
        l->engineVersion = engineVersion;
        l->exeDir = executable();
        l->extDir = e->dir;
        AverExtHost& h = l->host;
        h.size = sizeof(AverExtHost);
        h.abi = AVER_EXT_ABI;
        h.engineVersion = l->engineVersion.c_str();
        h.executableDir = l->exeDir.c_str();
        h.extensionDir = l->extDir.c_str();
        h.log = services.log ? services.log : defaultLog;
        h.importModel = services.importModel ? services.importModel : refuseImport;
        h.freeModelResult = services.freeModelResult ? services.freeModelResult : freeNothing;
        h.progress = services.progress ? services.progress : dropProgress;
        l->api.size = sizeof(AverExtApi);
        const i32 rc = entry(&h, &l->api);
        if (rc != 0) {
            if (l->api.shutdown) l->api.shutdown();
            freeLibrary(lib);
            return fail(*e, "the extension does not support extension ABI " + std::to_string(AVER_EXT_ABI) + " (entry returned " + std::to_string(rc) + ")");
        }
        if (!l->api.runCommand) { freeLibrary(lib); return fail(*e, "the extension offers no runCommand"); }
        loaded[e->name] = std::move(l);
        return true;
    }
};

Registry::Registry() : impl_(std::make_unique<Impl>()) {}

Registry::~Registry() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->unloadAllLocked();
}

void Registry::setHostServices(const HostServices& services) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->services = services;
}

void Registry::setEngineVersion(std::string_view version) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->engineVersion = std::string(version);
}

void Registry::setExecutableDir(std::string_view dir) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->exeDir = std::string(dir);
    impl_->exeDirSet = true;
}

usize Registry::scan(const std::string& root) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<std::string> dirs;
    std::error_code ec;
    for (fsys::directory_iterator it(toPath(root), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code dec;
        if (!it->is_directory(dec)) continue;
        const std::string name = leafName(it->path());
        if (name.empty() || name[0] == '.') continue;
        dirs.push_back(name);
    }
    std::sort(dirs.begin(), dirs.end());

    usize added = 0;
    for (const std::string& dirName : dirs) {
        Extension e;
        e.dirName = dirName;
        e.dir = root + "/" + dirName;
        e.name = dirName;
        std::string text, why;
        if (!readFileText(e.dir + "/extension.json", text)) why = "no extension.json";
        else if (Extension parsed; parseManifest(text, parsed, why)) {
            parsed.dirName = e.dirName;
            parsed.dir = e.dir;
            e = std::move(parsed);
            why = refusalReason(e, impl_->engineVersion);
            if (why.empty())
                for (const Extension& other : impl_->list)
                    if (other.ready() && lower(other.name) == lower(e.name)) { why = "an extension named " + other.name + " is already listed (" + other.dir + ")"; break; }
        }
        e.status = why.empty() ? Status::Ready : Status::Refused;
        e.reason = why;
        impl_->list.push_back(std::move(e));
        ++added;
    }
    return added;
}

usize Registry::scanDefault() { return scan(impl_->executable() + "/Extensions"); }

void Registry::clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->unloadAllLocked();
    impl_->list.clear();
}

const std::vector<Extension>& Registry::extensions() const { return impl_->list; }

const Extension* Registry::find(std::string_view name) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    for (const Extension& e : impl_->list) if (e.name == name) return &e;
    return nullptr;
}

const Extension* Registry::findCommand(std::string_view command) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    for (const Extension& e : impl_->list) {
        if (!e.ready()) continue;
        for (const CommandInfo& c : e.commands) if (c.name == command) return &e;
    }
    return nullptr;
}

bool Registry::load(std::string_view name, std::string* why) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->loadLocked(name, why);
}

bool Registry::isLoaded(std::string_view name) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->loaded.count(std::string(name)) != 0;
}

bool Registry::run(std::string_view command, int argc, const char* const* argv, int& exitCode, std::string* why) {
    const Extension* e = findCommand(command);
    if (!e) {
        if (why) *why = "no installed extension offers the command \"" + std::string(command) + "\"";
        return false;
    }
    AverExtApi api;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->loadLocked(e->name, why)) return false;
        api = impl_->loaded[e->name]->api;
    }
    exitCode = api.runCommand(std::string(command).c_str(), argc, argv);
    return true;
}

std::vector<std::string> missingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry) {
    std::vector<std::string> missing;
    for (const fmt::ProjectExtension& want : project.extensions) {
        const Extension* have = registry.find(want.name);
        const std::string needs = "project \"" + project.name + "\" needs the extension " + want.name + (want.version.empty() ? "" : " " + want.version);
        if (!have) missing.push_back(needs + ", which is not installed (install it under Extensions/" + want.name + ")");
        else if (!have->ready()) missing.push_back(needs + ", which is installed but refused: " + have->reason);
    }
    return missing;
}

usize warnMissingProjectExtensions(const fmt::ProjectDesc& project, const Registry& registry) {
    const std::vector<std::string> missing = missingProjectExtensions(project, registry);
    for (const std::string& line : missing) AVER_WARN("[Extensions] {}", line);
    return missing.size();
}

usize warnMissingProjectExtensions(const fmt::ProjectDesc& project) {
    if (project.extensions.empty()) return 0;
    Registry registry;
    registry.scanDefault();
    return warnMissingProjectExtensions(project, registry);
}

}  // namespace aver::ext
