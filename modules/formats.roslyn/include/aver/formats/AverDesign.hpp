#pragma once
// The Roslyn backend for reading an actor script: `averdesign`, run as a process.
//
// WHY THIS IS ITS OWN MODULE. Aver.Formats has no dependencies and reads bytes; that is what lets a
// test link it alone and what lets the built-in scanner run on a machine with no .NET at all. Adding
// process spawning to it to serve one optional fallback would put a dependency on every consumer of
// every format in the tree. So the escalation path lives here, above Aver.Formats, and a build
// without this module simply has no Roslyn backend -- which is the honest structural statement of
// "optional" and needs no flag to express.
//
// WHEN IT RUNS. Only when the built-in scanner returns `ActorParseStatus::Malformed`. That status
// means the text has left the locked grammar of docs/DESIGNER_REWRITE.md -- somebody hand edited
// inside a region whose header told them not to -- rather than that the text is wrong. Roslyn parses
// real C#, so it survives a named argument moved, an argument omitted, a coordinate written as an
// expression, or a `#if` around a placement.
//
// WHAT IT IS NOT. It is not the parser and must never become it. The scanner needs no .NET, no
// NuGet and no process; it answers in microseconds and it is correct by construction for the files
// the editor itself writes. Every result this produces is compared, where both can read a file,
// against what the scanner produced -- see RoslynTest -- because a fallback that quietly disagreed
// would be worse than no fallback at all.
#include "aver/formats/ActorScript.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Where the tool is. Set once at startup by whoever knows the layout of the install; until then the
// PATH is searched, which is what makes a developer's `dotnet run` build usable without configuring
// anything.
//
// Pushed rather than discovered here because a formats module has no business knowing where an
// editor stages its binaries, and hard-coding a relative path would work in exactly one layout.
void setAverDesignPath(std::string exePath);
const std::string& averDesignPath();

// Can it run AT ALL? Answered by invoking `--probe`, which does no parsing: the caller is asking
// whether a runtime and a binary exist, and answering that by parsing something would report a bad
// file as a missing tool.
//
// The result is CACHED for the process. Spawning a process to answer "is the tool there" on every
// file that fails to scan would put a process launch on a path that runs while somebody is typing.
// Pass `recheck` after staging the tool, which is the one case the cache is wrong.
bool averDesignAvailable(bool recheck = false);

// Everything a single run of the tool yields. One struct because one launch produces both, and
// launching twice to fill two would double the cost of the slow part.
struct RoslynParse {
    ActorScript script;
    std::vector<ActorClassInfo> classes;
};

// Parse `csPath` through Roslyn.
//
// Takes a PATH rather than the text, deliberately: the tool reads the file itself, so the bytes it
// computes spans against are the bytes on disk rather than a copy that may have been normalised on
// the way through this process. A span that indexed different bytes from the ones it will be applied
// to is the one failure a rewriter cannot detect.
//
// Returns false with `err` set when the tool could not be run, exited non-zero, or produced output
// this could not read. On success `out.script.backend` is `ActorParserBackend::Roslyn`, which is how
// a panel or a log line can say which implementation answered.
bool parseActorFileRoslyn(const std::string& csPath, RoslynParse& out, std::string* err = nullptr);

} // namespace aver::fmt
