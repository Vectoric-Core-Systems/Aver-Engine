#pragma once
// Runs a second copy of the editor in --warm-shaders mode (ShaderWarmRun.cpp) after a project opens, so the
// shader caches hold every renderer variant before the user switches mode. Its progress shows as a notification,
// only when something actually compiles; a warm cache stays silent. Windows only; elsewhere a no-op.
#include <memory>
#include <string>

namespace aver::editor {

class ShaderWarmup {
public:
    ShaderWarmup();
    ~ShaderWarmup();   // terminates a run still going
    ShaderWarmup(const ShaderWarmup&) = delete;
    ShaderWarmup& operator=(const ShaderWarmup&) = delete;

    // Starts a run for this project manifest (empty = default settings). No-op while one runs. `graphList`: a file of
    // "<id> <path>" lines, the material graphs a level brought (ShaderWarmRun.cpp --graphs); the run then says so in its notification.
    void start(const std::string& projectManifest, const std::string& graphList = std::string());
    // Main thread, once a frame: turns the tool's output into the notification.
    void poll();
    bool running() const;
    // A run was started and has ended (done, failed or cancelled) since the last start().
    bool finished() const;
    // The fingerprint of the graphs the run registered ("warm: graphs"), once it has said it.
    bool graphFingerprint(unsigned long long& out) const;
    // Ends a run still going and drops its notification.
    void cancel();
    // The last progress line ("412 of 1032 shaders") and its fraction (-1 = unknown).
    float progress(std::string& note) const;
    // Someone is waiting on this run: raise it from idle to normal priority.
    void boost();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aver::editor
