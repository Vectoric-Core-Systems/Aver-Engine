#pragma once
// Runs a child program for the import dialog: argv without a shell, stdout and stderr merged, stdin closed, each output line handed over as it
// arrives (ProcessRun.hpp's runCaptured goes through a command line and returns only at the end). Windows uses CreateProcessW, everything
// else posix_spawnp.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aver::editor {

struct ProcessOutput {
    bool started = false;   // false: the program could not be started at all (not found, not executable)
    bool cancelled = false; // ProcessControl::cancel ended it
    int exitCode = -1;
    std::string text;       // everything the program wrote, in order
};

// Stops a running child from another thread, with everything it started (an importer's own dotnet build holds the same output pipe, so ending only
// the child would leave the reader waiting): a job object on Windows, which also ends them if the editor exits; a process group elsewhere.
// cancel() before the child starts stops it as it starts; after it ends it does nothing.
class ProcessControl {
public:
    void cancel();
    bool cancelled() const { return cancelled_.load(); }

    // runProcess's side.
    void attach(void* handle, int group);
    void detach();

private:
    std::mutex lock_;
    std::atomic<bool> cancelled_{false};
    void* handle_ = nullptr;   // Windows: the job object
    int group_ = 0;            // POSIX: the child's process group
};

// `onLine` sees each complete line as it arrives (without the line end); the last partial line is delivered at the end.
ProcessOutput runProcess(const std::string& program, const std::vector<std::string>& args,
                         const std::function<void(const std::string& line)>& onLine = {}, ProcessControl* control = nullptr);

}  // namespace aver::editor
