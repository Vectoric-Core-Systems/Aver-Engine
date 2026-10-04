#pragma once
// Everything the editor writes into the ENGINE tree: the `modules/<name>/` skeleton behind
// Tools > New C++ Module, and the header/source pair behind Tools > New C++ Class.
#include <string>
#include <vector>

namespace aver::editor {

// One module under modules/, and the conventions it already follows.
struct ModuleInfo {
    std::string dir;        // folder name under modules/, e.g. "render.voxi"
    std::string includeSub; // existing subfolder of include/aver/, e.g. "voxi"; empty if none yet
    std::string nameSpace;  // namespace an existing header in it uses, e.g. "aver::voxi"
    bool built = false;     // has a CMakeLists.txt (a skeleton module has only a README)
};

// The engine source tree, found by walking up from the executable. Empty when there is none beside
// the editor, in which case authoring engine C++ is impossible.
std::string engineRoot();

// Every folder under `modules/`, sorted.
std::vector<ModuleInfo> listModules();

// True when `name` is a legal module folder name: lowercase letters, digits and dots.
bool validateModuleName(const std::string& name, std::string* err);

// Scaffolds `modules/<name>/` — CMakeLists.txt, README.md, a header and a source. Refuses an
// existing folder, and does not wire the module into the top-level CMakeLists.txt.
bool createCppModule(const std::string& name, const std::string& purpose,
                     std::vector<std::string>* outFiles, std::string* err);

// Writes `<module>/include/aver/<sub>/<Name>.hpp` + `<module>/src/<Name>.cpp` matching the module's
// own include path and namespace. Never overwrites, and never edits the module's CMakeLists.txt —
// the line a human must add comes back in `outSourceLine`.
bool createCppClass(const ModuleInfo& mod, const std::string& name,
                    std::vector<std::string>* outFiles, std::string* outSourceLine,
                    std::string* err);

// "render.voxi" -> "Aver.Render.Voxi", the CMake target name.
std::string moduleTargetName(const std::string& dir);
// "render.voxi" -> "voxi", the leaf naming the include folder and the namespace.
std::string moduleLeaf(const std::string& dir);

} // namespace aver::editor
