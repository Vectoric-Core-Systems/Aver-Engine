#pragma once
// Runs a child program for `AverAssetC fabric` (the script converter is a .NET tool): no shell, stdout and stderr merged,
// stdin closed. Windows uses CreateProcessW, everything else posix_spawnp.
#include <functional>
#include <string>
#include <vector>

namespace aver::fabric {

struct ProcessOutput {
    bool started = false;   // false: the program could not be started at all (not found, not executable)
    int exitCode = -1;
    std::string text;       // everything the program wrote, in order
};

// `onLine` sees each complete line as it arrives (without the line end); the last partial line is delivered at the end.
ProcessOutput runProcess(const std::string& program, const std::vector<std::string>& args,
                         const std::function<void(const std::string& line)>& onLine = {});

// The `dotnet` host: DOTNET_HOST_PATH, then PATH, then DOTNET_ROOT and the usual install folders. Empty when there is none.
std::string findDotnet();

}  // namespace aver::fabric
