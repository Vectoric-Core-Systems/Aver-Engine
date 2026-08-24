#include "aver/upgrade/Upgrade.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/Version.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aver::upgrade {
namespace {

ResourceFn gResourceFn = nullptr;
void*      gResourceUser = nullptr;

bool readText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeText(const std::string& path, const std::string& text, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not write " + path; return false; }
    f << text;
    if (!f) { if (err) *err = "could not finish writing " + path; return false; }
    return true;
}

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

// ---------------------------------------------------------------------------
// 0.1 -> 0.2
//
// Two things broke for anyone whose project was made by a 0.1 install, and both come from the same
// missing payload entry: scripts/payload.allowlist shipped scripting/csharp/** and never
// scripting/fsharp/**, while ProjectScaffold wrote a Scripts.FSharp.fsproj referencing Aver.Pcg and
// a Sky.fs that opens it. On a shipped engine the reference resolved to nothing, was correctly
// omitted -- and Sky.fs was written anyway. Every project a 0.1 Launcher install created failed on
// its first build with FS0039, naming a namespace its author had never typed.
//
// SKY.FS IS OVERWRITTEN, NOT MERGED, because it is an engine resource rather than the author's
// work: the scaffold writes it, the engine's own PCG types define what it can say, and a project
// that has never been opened has never edited it. Asking the host for the CURRENT text rather than
// carrying a copy is what stops this step from rewriting the 0.2-era file forever.
//
// AND IF THERE IS NO F# AT ALL, this step does nothing and succeeds. A project whose author deleted
// the pair, or one scaffolded by a fixed engine that declined to write F# it could not reference,
// is already correct.
bool step_0_1_to_0_2(const Context& ctx, std::string* err) {
    const std::string fsproj = ctx.scriptsDir + "\\Scripts.FSharp.fsproj";
    const std::string sky    = ctx.scriptsDir + "\\Sky.fs";
    if (!exists(fsproj) && !exists(sky)) return true;

    std::string text;
    if (!engineResource("Sky.fs", ctx, text)) {
        // No provider, or the host no longer has such a resource. Removing the pair is the correct
        // outcome and not a fallback: what is on disk cannot compile, and a project with no F# is a
        // supported, working project. Leaving a broken file behind to preserve "content" would
        // preserve nothing an author wrote.
        std::error_code ec;
        std::filesystem::remove(fsproj, ec);
        std::filesystem::remove(sky, ec);
        AVER_INFO("[Upgrade] '{}': removed the F# starter (no Aver.Pcg to reference)", ctx.name);
        return true;
    }

    if (!writeText(sky, text, err)) return false;
    AVER_INFO("[Upgrade] '{}': rewrote Sky.fs from the engine's current starter", ctx.name);

    // The .fsproj's ProjectReference is repointed by the editor's existing project inspection
    // (RepointReference), which already handles "a reference whose target no longer exists" for
    // every engine project. This step deliberately does not duplicate that logic.
    return true;
}

// A 0.2 PROJECT NEEDS NOTHING DONE TO IT, and this function exists to say so out loud.
//
// Every project-visible change in 0.3 is additive or engine-side. New projects are scaffolded with
// RENDER.RAYTRACING 2, but a manifest that omits the key parses to -1 -- "not stated" -- and follows
// the engine default, which is that same Medium tier, so a 0.2 manifest already renders the way a
// fresh 0.3 one does. The graph reader's ENTRY/OUT forward references, the missing-pin diagnostic
// and the per-record writer all changed how the engine READS .ocgraph files, not what those files
// have to contain. Nothing on disk is stale.
//
// SO WHY IS IT HERE. planUpgrade walks by SERIES and reports a gap rather than skipping it, on the
// grounds that a missing link silently strands a project halfway and then tells it it is current.
// That rule only works if "nothing to repair" and "we forgot" are written differently, and an empty
// step IS the difference: the chain 0.1 -> 0.2 -> 0.3 stays contiguous, the plan a 0.1 project gets
// still ends at this engine's series, and the log line names this summary so an author can see that
// the step ran and chose to do nothing. Deleting it would not simplify anything -- it would make
// every 0.1-era project fail to open.
bool step_0_2_to_0_3(const Context& ctx, std::string* err) {
    (void)ctx;
    (void)err;
    return true;
}

// A 0.3 PROJECT ALSO NEEDS NOTHING DONE TO IT, for the same reason and by the same rule as above.
//
// Every format 0.4 touched was extended in a way that makes ABSENCE the old behaviour, which is what
// makes this step empty rather than merely unwritten:
//   - `.ocgraph` gained a DOMAIN record naming which language the graph is written in. A file
//     without one is a GAMEPLAY graph, which is what every 0.3 graph is. That default was chosen so
//     no existing graph would need rewriting, and this is where that decision gets paid back.
//   - `.ocmat` gained GRAPHREF. A material without one takes the same stock shading path it took in
//     0.3, through code the node-graph work never modified.
//   - `COMP ... Fluid`, `hidden=owner`, `WATER ... SIMULATED`, sockets and animation notifies are
//     all new things a file may now say. A 0.3 file does not say them, and nothing requires it to.
//   - `.ocbt` and `.ocsnd` are new formats. A 0.3 project contains none.
// The legacy `.ocmap` dispatch fixed this release changes how the engine READS a level, not what a
// level has to contain -- and it exists precisely so those files stop being rewritten wrongly.
//
// ONE THING IS DELIBERATELY NOT DONE HERE, and it is the interesting half of this comment.
//
// 0.4 fixed the FirstPerson template rendering the inside of its own character, by adding
// `hidden=owner` to the body component in the TEMPLATE. A project someone scaffolded from the 0.3
// template has its own copy of that graph, and this step does not go and edit it. Two reasons, and
// the second is the real one:
//
//   1. It cannot be done reliably. Finding "the COMP line that is this character's body" in a graph
//      the author has since renamed, restructured or replaced means guessing at content by
//      convention, and a wrong guess silently hides a mesh the author wanted drawn.
//   2. An upgrade step edits files the author owns. `step_0_1_to_0_2` did exactly that and was
//      right to -- it repaired a reference the 0.1 PAYLOAD had shipped broken, which was our defect
//      sitting in their directory. This is not that. The author's graph is not broken; it is
//      missing an attribute that did not exist when they wrote it, and the fix is one word they can
//      read and understand.
//
// So it is a documented manual step instead: add ` hidden=owner` to the body's COMP line. That is
// stated in the 0.4.0 release notes rather than performed silently here.
bool step_0_3_to_0_4(const Context& ctx, std::string* err) {
    (void)ctx;
    (void)err;
    return true;
}

// EVERY STEP EVER SHIPPED, OLDEST FIRST, AND NONE OF THEM EDITED AFTER THE FACT. A project made in
// 0.1 will still be opened years from now, and it will run exactly this function.
const std::vector<Step> kSteps = {
    {{0, 1, 0}, {0, 2, 0},
     "Repair the F# starter the 0.1 payload could not reference",
     &step_0_1_to_0_2},
    {{0, 2, 0}, {0, 3, 0},
     "Nothing to repair: 0.3's project-visible changes are all additive",
     &step_0_2_to_0_3},
    {{0, 3, 0}, {0, 4, 0},
     "Nothing to repair: 0.4's format changes all read an absent record as the 0.3 behaviour",
     &step_0_3_to_0_4},
};

} // namespace

bool parseVersion(std::string_view text, Version& out) {
    if (text.empty()) return false;
    Version v;
    int part = 0;
    int value = 0;
    bool any = false;
    for (usize i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '.';
        if (c >= '0' && c <= '9') { value = value * 10 + (c - '0'); any = true; continue; }
        if (c != '.' && c != '-' && c != '+') return false;   // junk where a separator belongs
        if (!any) return false;
        if (part == 0) v.major = value;
        else if (part == 1) v.minor = value;
        else if (part == 2) v.patch = value;
        ++part;
        value = 0;
        any = false;
        if (c == '-' || c == '+') break;   // "0.2.0-rc1": the suffix is not ours to interpret
        if (part > 2) break;
    }
    if (part == 0) return false;
    out = v;
    return true;
}

std::string formatVersion(const Version& v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d.%d.%d", v.major, v.minor, v.patch);
    return buf;
}

Version engineVersion() {
    Version v;
    // kEngineVersion comes from the build; a malformed one would be a build error, not a runtime
    // condition, so the zero here is unreachable rather than a fallback anybody should rely on.
    parseVersion(kEngineVersion, v);
    return v;
}

void setResourceProvider(ResourceFn fn, void* user) {
    gResourceFn = fn;
    gResourceUser = user;
}

bool engineResource(const char* logicalName, const Context& ctx, std::string& out) {
    if (!gResourceFn || !logicalName) return false;
    return gResourceFn(logicalName, ctx, out, gResourceUser);
}

const std::vector<Step>& steps() { return kSteps; }

bool planUpgrade(const Version& from, std::vector<const Step*>& out, std::string* err) {
    out.clear();
    const Version now = engineVersion();
    if (sameSeries(from, now)) return true;   // a patch apart at most: nothing to do

    if (olderSeries(now, from)) {
        if (err) *err = "this project was made in " + formatVersion(from) + ", which is newer than "
                        "this engine (" + formatVersion(now) + "); there is no downgrade path";
        return false;
    }

    // Walk the chain by SERIES rather than by list order, so a gap in the table is reported instead
    // of silently skipped -- a missing link means some project out there upgrades halfway and is
    // then told it is current.
    Version at = from;
    while (!sameSeries(at, now)) {
        const Step* next = nullptr;
        for (const Step& s : kSteps) {
            if (sameSeries(s.from, at)) { next = &s; break; }
        }
        if (!next) {
            if (err) *err = "no upgrade step from " + formatVersion(at) + " to " + formatVersion(now);
            out.clear();
            return false;
        }
        out.push_back(next);
        at = next->to;
    }
    return true;
}

bool runUpgrade(const Context& ctx, const std::vector<const Step*>& plan, std::string* err) {
    for (const Step* s : plan) {
        AVER_INFO("[Upgrade] '{}': {} -> {}: {}", ctx.name, formatVersion(s->from),
                  formatVersion(s->to), s->summary);
        if (!s->apply(ctx, err)) {
            if (err) *err = "step " + formatVersion(s->from) + " -> " + formatVersion(s->to) +
                            " failed: " + *err;
            return false;
        }
    }
    return true;
}

} // namespace aver::upgrade
