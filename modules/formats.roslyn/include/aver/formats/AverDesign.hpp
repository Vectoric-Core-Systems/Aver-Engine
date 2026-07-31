#pragma once
// The Roslyn backend for reading an actor script: the `averdesign` tool, run as a process. Its own
// module because Aver.Formats has no dependencies and must not gain process spawning. Used only
// when the built-in scanner returns ActorParseStatus::Malformed.
#include "aver/formats/ActorScript.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Sets where the tool is. Empty means search the PATH.
void setAverDesignPath(std::string exePath);
// Where the tool is, as last set.
const std::string& averDesignPath();

// True when the tool can run at all, answered by invoking `--probe`. Cached for the process; pass
// `recheck` after staging the tool.
bool averDesignAvailable(bool recheck = false);

// Everything a single run of the tool yields.
struct RoslynParse {
    ActorScript script;
    std::vector<ActorClassInfo> classes;
};

// Parses `csPath` through Roslyn. Takes a path rather than the text, so the tool computes its spans
// against the bytes on disk. Returns false with `err` set when the tool could not be run, exited
// non-zero, or produced output this could not read.
bool parseActorFileRoslyn(const std::string& csPath, RoslynParse& out, std::string* err = nullptr);

} // namespace aver::fmt
