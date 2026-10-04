#pragma once
// Aver.Upgrade -- migrating a project from the engine version it was made in to this one.
//
// A CHAIN, NOT A FIX. The naive shape is one "upgrade a project" function that knows what today's
// projects need; it works exactly once, and the next release either rewrites it or grows an
// if-ladder nobody can audit. This module instead holds an ordered list of STEPS, each declaring
// the series it upgrades FROM and TO, and plans a path through them. A project made in 0.1 reaching
// an engine at 0.4 runs 0.1->0.2, 0.2->0.3, 0.3->0.4 in order, and each step only ever has to know
// about the one boundary it was written for. Steps are never edited once shipped: a project out
// there in the world will run them years from now, exactly as they were.
//
// SERIES, NOT VERSIONS. Only major.minor gate an upgrade. 0.1.0 -> 0.1.7 is a patch: no format or
// API break, so it opens with no prompt at all. 0.1.x -> 0.2.x is a series change and runs the
// chain. Patch numbers are recorded but never compared, which is what keeps a bugfix release from
// asking every author on earth to migrate.
//
// WHAT A STEP MAY ASSUME. That every earlier step has already run, and nothing else. A step must
// not read the CURRENT engine version, or "is this the last step" -- that is how a chain rots into
// a special case for whatever was newest when it was written.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::upgrade {

// A parsed engine version. `patch` is carried so a project can record exactly what made it, and is
// deliberately absent from every comparison below.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// Parses "0.2.0", "0.2", or "0.2.0-anything" (the suffix is ignored). False on anything else,
// INCLUDING an empty string -- "this project records no version" is a real state the caller has to
// handle, not a zero.
bool parseVersion(std::string_view text, Version& out);

// "<major>.<minor>.<patch>".
std::string formatVersion(const Version& v);

// The engine this build is, from kEngineVersion.
Version engineVersion();

// Same major.minor. Patch is ignored, which is the whole compatibility rule.
inline bool sameSeries(const Version& a, const Version& b) {
    return a.major == b.major && a.minor == b.minor;
}

// Is `a` an older SERIES than `b`? False for two patches of one series.
inline bool olderSeries(const Version& a, const Version& b) {
    return a.major != b.major ? a.major < b.major : a.minor < b.minor;
}

// Everything a step is allowed to touch, resolved once by the caller so no step has to re-derive a
// path and get it subtly different from its neighbour.
struct Context {
    std::string manifestPath;   // <root>\<name>.ocproject
    std::string projectRoot;    // the folder holding the manifest
    std::string contentDir;     // <root>\Content
    std::string scriptsDir;     // <root>\Content\Scripts
    std::string name;           // the project's name, for messages
};

// ENGINE-OWNED FILES A STEP MAY REWRITE. A step that needs to replace a file the engine authored --
// Sky.fs, a starter script, a generated project -- asks for its CURRENT text by logical name rather
// than carrying a copy. Two reasons: this module would otherwise have to duplicate content the
// scaffold owns and drift from it, and a step frozen at its shipping form would keep rewriting the
// 0.2-era version of a file forever. Returns false when the host has nothing under that name.
using ResourceFn = bool (*)(const char* logicalName, const Context& ctx, std::string& out, void* user);
void setResourceProvider(ResourceFn fn, void* user);
bool engineResource(const char* logicalName, const Context& ctx, std::string& out);

// One migration between two adjacent series.
struct Step {
    Version from;
    Version to;
    const char* summary;   // one line, shown in the prompt
    bool (*apply)(const Context& ctx, std::string* err);
};

// Every step this engine knows, oldest first.
const std::vector<Step>& steps();

// Fills `out` with the steps that carry `from` to this engine's series, in order.
//
// Returns true and an EMPTY plan when the project is already current (same series) -- "nothing to
// do" is success, not failure. Returns false when no chain exists, which is the honest answer for a
// project made in a NEWER engine than this one: there is no downgrade path and pretending otherwise
// would corrupt somebody's work.
bool planUpgrade(const Version& from, std::vector<const Step*>& out, std::string* err);

// Runs a plan in order, stopping at the first failure. A step that fails leaves the project as that
// step found it; there is no rollback, which is exactly why the editor offers to work on a copy.
bool runUpgrade(const Context& ctx, const std::vector<const Step*>& plan, std::string* err);

} // namespace aver::upgrade
