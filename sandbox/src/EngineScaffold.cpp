// Engine-side scaffolding: finds the engine source tree, lists its modules, and writes new C++
// module and class skeletons into modules/.

#include "EngineScaffold.hpp"
#include "ProjectScaffold.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace aver::editor {
namespace {

// Upper-cases the first character only. "voxi" -> "Voxi".
std::string capitalise(const std::string& s) {
    std::string out = s;
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

// Reads the namespace the files in `dir` open, e.g. "aver::voxi". Empty if none found.
std::string namespaceIn(const std::filesystem::path& dir, const char* ext) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ext) continue;
        std::string text;
        if (!readFileText(e.path().string(), text)) continue;
        usize pos = 0;
        while (pos < text.size()) {
            usize nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.rfind("namespace aver", 0) != 0) continue;
            const usize brace = line.find('{');
            std::string ns = line.substr(10, (brace == std::string::npos ? line.size() : brace) - 10);
            while (!ns.empty() && (ns.back() == ' ' || ns.back() == '\r' || ns.back() == '\t')) ns.pop_back();
            if (ns.find("detail") != std::string::npos) continue;
            return ns;
        }
    }
    return {};
}

// The one subfolder of include/aver/ a module owns, e.g. "voxi" for modules/render.voxi.
std::string includeSubOf(const std::filesystem::path& moduleDir) {
    std::error_code ec;
    const std::filesystem::path base = moduleDir / "include" / "aver";
    if (!std::filesystem::is_directory(base, ec)) return {};
    for (const auto& e : std::filesystem::directory_iterator(base, ec))
        if (e.is_directory(ec)) return e.path().filename().string();
    return {};
}

// The namespace a module opens, taken from its headers first and its sources second.
std::string namespaceOf(const std::filesystem::path& moduleDir, const std::string& includeSub) {
    std::string ns = namespaceIn(moduleDir / "include" / "aver" / includeSub, ".hpp");
    if (ns.empty()) ns = namespaceIn(moduleDir / "src", ".cpp");
    return ns;
}

// Builds the CMakeLists.txt text for a new module.
std::string cmakeText(const std::string& target, const std::string& leafClass) {
    std::string s;
    s += "aver_add_module(" + target + "\n";
    s += "  SOURCES\n";
    s += "    src/" + leafClass + ".cpp\n";
    s += "  DEPS\n";
    s += "    Aver.Core\n";
    s += ")\n";
    return s;
}

// Builds the README.md text for a new module.
std::string readmeText(const std::string& dir, const std::string& target,
                       const std::string& purpose) {
    std::string s;
    s += "# " + target + "  (`modules/" + dir + "`)\n\n";
    s += "- **Language:** C++\n";
    s += "- **Depends on:** Core\n";
    s += "- **Planned phase:** unscheduled\n\n";
    s += (purpose.empty() ? std::string("One-paragraph purpose goes here.") : purpose) + "\n\n";
    s += "> Skeleton only \xE2\x80\x94 not yet wired into the top-level `CMakeLists.txt`. Add\n";
    s += "> `add_subdirectory(modules/" + dir + ")` there when it is ready to build; wiring a module\n";
    s += "> into the build is a deliberate act, which is why every skeleton here is unwired.\n";
    s += "> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.\n";
    return s;
}

// Builds the .hpp text for a new engine class.
std::string headerText(const std::string& cls, const std::string& ns, const std::string& target,
                       const char* origin) {
    std::string s;
    s += "#pragma once\n";
    s += "// " + cls + " \xE2\x80\x94 part of " + target + ".\n";
    s += "//\n";
    s += "// ENGINE code, written by the editor (Tools > " + std::string(origin) + "). It compiles into\n";
    s += "// " + target + ", not into any project, so the engine has to be rebuilt before anything\n";
    s += "// here runs. C# in a project needs no rebuild; this is the other side of that asymmetry.\n";
    s += "#include \"aver/core/Types.hpp\"\n";
    s += "\n";
    s += "namespace " + ns + " {\n";
    s += "\n";
    s += "class " + cls + " {\n";
    s += "public:\n";
    s += "    bool init();\n";
    s += "    void shutdown();\n";
    s += "    bool ready() const { return ready_; }\n";
    s += "\n";
    s += "private:\n";
    s += "    bool ready_ = false;\n";
    s += "};\n";
    s += "\n";
    s += "} // namespace " + ns + "\n";
    return s;
}

// Builds the .cpp text for a new engine class.
std::string sourceText(const std::string& cls, const std::string& ns, const std::string& includeSub) {
    std::string s;
    s += "#include \"aver/" + includeSub + "/" + cls + ".hpp\"\n";
    s += "#include \"aver/core/Log.hpp\"\n";
    s += "\n";
    s += "namespace " + ns + " {\n";
    s += "\n";
    s += "bool " + cls + "::init() {\n";
    s += "    AVER_INFO(\"[" + cls + "] init\");\n";
    s += "    ready_ = true;\n";
    s += "    return ready_;\n";
    s += "}\n";
    s += "\n";
    s += "void " + cls + "::shutdown() {\n";
    s += "    ready_ = false;\n";
    s += "}\n";
    s += "\n";
    s += "} // namespace " + ns + "\n";
    return s;
}

} // namespace

// The last dot-segment of a module folder name. "render.voxi" -> "voxi".
std::string moduleLeaf(const std::string& dir) {
    const usize dot = dir.find_last_of('.');
    return dot == std::string::npos ? dir : dir.substr(dot + 1);
}

// The CMake target name for a module folder. "render.voxi" -> "Aver.Render.Voxi".
std::string moduleTargetName(const std::string& dir) {
    std::string out = "Aver";
    usize pos = 0;
    while (pos <= dir.size()) {
        usize dot = dir.find('.', pos);
        if (dot == std::string::npos) dot = dir.size();
        out += "." + capitalise(dir.substr(pos, dot - pos));
        pos = dot + 1;
    }
    return out;
}

// The engine source tree found by walking up from the executable. Empty if not found. Cached.
std::string engineRoot() {
    static const std::string cached = [] {
        std::error_code ec;
        std::filesystem::path probe = std::filesystem::path(executableDir());
        for (int up = 0; up < 8 && !probe.empty(); ++up) {
            if (std::filesystem::exists(probe / "cmake" / "AvModule.cmake", ec) &&
                std::filesystem::is_directory(probe / "modules", ec))
                return probe.string();
            if (!probe.has_parent_path() || probe.parent_path() == probe) break;
            probe = probe.parent_path();
        }
        return std::string();
    }();
    return cached;
}

// Every folder under modules/, with its namespace and include subfolder, sorted by folder name.
std::vector<ModuleInfo> listModules() {
    std::vector<ModuleInfo> out;
    const std::string root = engineRoot();
    if (root.empty()) return out;

    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(root) / "modules", ec)) {
        if (!e.is_directory(ec)) continue;
        ModuleInfo m;
        m.dir = e.path().filename().string();
        m.built = std::filesystem::exists(e.path() / "CMakeLists.txt", ec);
        m.includeSub = includeSubOf(e.path());
        if (m.includeSub.empty()) m.includeSub = moduleLeaf(m.dir);
        m.nameSpace = namespaceOf(e.path(), m.includeSub);
        if (m.nameSpace.empty()) m.nameSpace = "aver::" + m.includeSub;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const ModuleInfo& a, const ModuleInfo& b) { return a.dir < b.dir; });
    return out;
}

// Checks a module folder name. Returns false and fills `err` with the reason.
bool validateModuleName(const std::string& name, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (name.empty()) return fail("Enter a module name.");
    if (name.size() > 40) return fail("Name is too long (40 characters max).");
    if (!(name.front() >= 'a' && name.front() <= 'z'))
        return fail("A module folder name starts with a lowercase letter.");
    if (name.back() == '.') return fail("A module folder name cannot end with a dot.");
    char prev = 0;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (!ok) return fail("Use lowercase letters, digits and dots only - e.g. render.terrain");
        if (c == '.' && prev == '.') return fail("Two dots in a row is not a module name.");
        prev = c;
    }
    return true;
}

// Writes a new modules/<name>/ skeleton: CMakeLists, README and one class pair. False on failure.
bool createCppModule(const std::string& name, const std::string& purpose,
                     std::vector<std::string>* outFiles, std::string* err) {
    if (!validateModuleName(name, err)) return false;

    const std::string root = engineRoot();
    if (root.empty()) { if (err) *err = "The engine source tree could not be found from the editor's path."; return false; }

    const std::string dir = root + "\\modules\\" + name;
    if (directoryExists(dir) || fileExists(dir)) {
        if (err) *err = "modules\\" + name + " already exists.";
        return false;
    }

    const std::string leaf = moduleLeaf(name);
    const std::string cls = capitalise(leaf);
    const std::string target = moduleTargetName(name);
    const std::string ns = "aver::" + leaf;
    const std::string includeDir = dir + "\\include\\aver\\" + leaf;
    const std::string srcDir = dir + "\\src";

    for (const std::string& d : {includeDir, srcDir}) {
        if (!createDirectories(d)) { if (err) *err = "Could not create " + d; return false; }
    }

    const struct { std::string path, text; } files[] = {
        { dir + "\\CMakeLists.txt", cmakeText(target, cls) },
        { dir + "\\README.md",      readmeText(name, target, purpose) },
        { includeDir + "\\" + cls + ".hpp", headerText(cls, ns, target, "New C++ Module") },
        { srcDir + "\\" + cls + ".cpp",     sourceText(cls, ns, leaf) },
    };
    for (const auto& f : files) {
        if (!writeNewFile(f.path, f.text, err)) return false;
        if (outFiles) outFiles->push_back(f.path);
    }

    AVER_INFO("[Editor] new C++ module: {} ({})", dir, target);
    return true;
}

// Writes a .hpp/.cpp pair into an existing module and reports the CMake SOURCES line to add.
bool createCppClass(const ModuleInfo& mod, const std::string& name,
                    std::vector<std::string>* outFiles, std::string* outSourceLine,
                    std::string* err) {
    if (!validateTypeName(name, err)) return false;
    if (mod.dir.empty()) { if (err) *err = "Pick a module first."; return false; }

    const std::string root = engineRoot();
    if (root.empty()) { if (err) *err = "The engine source tree could not be found from the editor's path."; return false; }

    const std::string moduleDir = root + "\\modules\\" + mod.dir;
    if (!directoryExists(moduleDir)) { if (err) *err = "modules\\" + mod.dir + " is not there any more."; return false; }

    const std::string includeDir = moduleDir + "\\include\\aver\\" + mod.includeSub;
    const std::string srcDir = moduleDir + "\\src";
    for (const std::string& d : {includeDir, srcDir}) {
        if (!createDirectories(d)) { if (err) *err = "Could not create " + d; return false; }
    }

    const std::string hpp = includeDir + "\\" + name + ".hpp";
    const std::string cpp = srcDir + "\\" + name + ".cpp";
    if (fileExists(hpp)) { if (err) *err = name + ".hpp already exists in " + mod.dir + "."; return false; }
    if (fileExists(cpp)) { if (err) *err = name + ".cpp already exists in " + mod.dir + "."; return false; }

    const std::string target = moduleTargetName(mod.dir);
    if (!writeNewFile(hpp, headerText(name, mod.nameSpace, target, "New C++ Class"), err)) return false;
    if (outFiles) outFiles->push_back(hpp);
    if (!writeNewFile(cpp, sourceText(name, mod.nameSpace, mod.includeSub), err)) return false;
    if (outFiles) outFiles->push_back(cpp);

    if (outSourceLine) *outSourceLine = "    src/" + name + ".cpp";
    AVER_INFO("[Editor] new C++ class: {} (+ {})", hpp, cpp);
    return true;
}

} // namespace aver::editor
