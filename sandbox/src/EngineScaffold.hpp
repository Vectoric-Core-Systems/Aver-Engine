#pragma once
// Everything the editor WRITES into the ENGINE tree: the `modules/<name>/` skeleton behind
// Tools > New C++ Module, and the header/source pair behind Tools > New C++ Class.
//
// Deliberately a separate file from ProjectScaffold, because the two sides are not symmetric and
// the split is the point. C# lives in `<project>/Content/Scripts` and needs no engine rebuild;
// C++ lives in the engine's `modules/` and does, because `.ocproject` carries no build
// integration (docs/PROJECTS.md). Unreal puts game C++ in the project; Aver cannot yet, and a
// reader of one file should not have to infer that from the other.
#include <string>
#include <vector>

namespace aver::editor {

// What Tools > New C++ Class needs to know about a module before writing into it.
struct ModuleInfo {
    std::string dir;        // folder name under modules/, e.g. "render.voxi"
    std::string includeSub; // existing subfolder of include/aver/, e.g. "voxi"; empty if none yet
    std::string nameSpace;  // namespace an existing header in it uses, e.g. "aver::voxi"
    bool built = false;     // has a CMakeLists.txt (a skeleton module has only a README)
};

// The engine source tree — the folder holding `modules/` and `cmake/AvModule.cmake`. Found by
// walking up from the executable rather than assumed to be two above bin/, which only holds for
// the default build layout. Empty when the editor is running with no source tree beside it, in
// which case authoring engine C++ is impossible and the menu says so instead of writing nowhere.
std::string engineRoot();

// Every folder under `modules/`, sorted, with the conventions each one already follows.
std::vector<ModuleInfo> listModules();

// A module folder name: lowercase letters, digits and dots ("render.voxi"), which is what the
// existing folders use and what the `Aver.Render.Voxi` target name is derived from.
bool validateModuleName(const std::string& name, std::string* err);

// Scaffold `modules/<name>/` — CMakeLists.txt, README.md, include/aver/<leaf>/<Leaf>.hpp and
// src/<Leaf>.cpp. Refuses an existing folder rather than merging into it.
//
// It does NOT touch the top-level CMakeLists.txt: `aver_add_module` needs at least one source, so
// the skeleton ships with a compilable pair, but WIRING it into the build is a deliberate act and
// every existing skeleton under modules/ is deliberately unwired. `outFiles` receives what landed.
bool createCppModule(const std::string& name, const std::string& purpose,
                     std::vector<std::string>* outFiles, std::string* err);

// Write `<module>/include/aver/<sub>/<Name>.hpp` + `<module>/src/<Name>.cpp`, matching the
// include path and namespace that module already uses. Never overwrites either file.
//
// It does NOT touch the module's CMakeLists.txt: `aver_add_module` takes an explicit SOURCES list
// and never globs, so the new .cpp compiles into nothing until a human adds the line the caller
// gets back in `outSourceLine`.
bool createCppClass(const ModuleInfo& mod, const std::string& name,
                    std::vector<std::string>* outFiles, std::string* outSourceLine,
                    std::string* err);

// "render.voxi" -> "Aver.Render.Voxi" (the CMake target name), and -> "voxi" (the leaf that names
// the include folder and the namespace). Exposed so the modals can show what will be created.
std::string moduleTargetName(const std::string& dir);
std::string moduleLeaf(const std::string& dir);

} // namespace aver::editor
