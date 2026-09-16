#pragma once
// Starts the standalone runtime (Runtime/host/AverEngineRuntime.exe) on a level from the editor,
// the same way a packaged game would run it -- a fresh process reading the project and level from
// disk, not the editor's in-memory state. No ImGui here; SandboxShell.cpp owns the menu item and
// the unsaved-changes prompt that decide when this gets called.
#include <string>

namespace aver::editor {

// AverEngineRuntime.exe beside the editor, or empty if it is not there -- Runtime/CMakeLists.txt
// builds it into the same bin directory as Sandbox.exe, but a Sandbox-only build never links it.
std::string runtimeExecutablePath();

// Launches AverEngineRuntime.exe on `levelPath` within `projectManifestPath`'s project, detached:
// the editor neither waits for the process nor holds a handle to it afterwards. False on failure,
// with `*why` (when non-null) set to what went wrong.
bool launchRuntime(const std::string& projectManifestPath, const std::string& levelPath, std::string* why);

} // namespace aver::editor
