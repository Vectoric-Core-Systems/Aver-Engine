#pragma once
// Runs a child program for the import dialog: argv without a shell, stdout and stderr merged, stdin closed, each output line handed over as it
// arrives (ProcessRun.hpp's runCaptured goes through a command line and returns only at the end). Windows uses CreateProcessW, everything
// else posix_spawnp.
#include <functional>
#include <string>
#include <vector>

namespace aver::editor {

struct ProcessOutput {
    bool started = false;   // false: the program could not be started at all (not found, not executable)
    int exitCode = -1;
    std::string text;       // everything the program wrote, in order
};

// `onLine` sees each complete line as it arrives (without the line end); the last partial line is delivered at the end.
ProcessOutput runProcess(const std::string& program, const std::vector<std::string>& args,
                         const std::function<void(const std::string& line)>& onLine = {});

}  // namespace aver::editor
