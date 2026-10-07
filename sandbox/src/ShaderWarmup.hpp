#pragma once
// Runs a second copy of the editor in --warm-shaders mode (ShaderWarmRun.cpp) after a project opens, so the
// shader caches hold every renderer variant before the user switches mode. Silent (it logs its end): the editor's
// own pipelines build off the main thread with their own notification. Windows only; elsewhere a no-op.
#include <memory>
#include <string>

namespace aver::editor {

class ShaderWarmup {
public:
    ShaderWarmup();
    ~ShaderWarmup();   // terminates a run still going
    ShaderWarmup(const ShaderWarmup&) = delete;
    ShaderWarmup& operator=(const ShaderWarmup&) = delete;

    // Starts a run for this project manifest (empty = default settings). No-op while one runs.
    void start(const std::string& projectManifest);
    // Main thread, once a frame: turns the tool's output into the notification.
    void poll();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aver::editor
