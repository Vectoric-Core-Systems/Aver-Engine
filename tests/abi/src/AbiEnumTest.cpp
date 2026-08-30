// AbiEnumTest -- the C ABI's integer constants and the C# enums that restate them agree.
//
// A cross-language audit of this tree found that every group below is mirrored BY HAND in C#, with
// nothing checking the values, while the STRUCT sitting in the same headers gets a full runtime
// handshake: HostBridge.cs refuses a table whose ContractVersion or Marshal.SizeOf<HostApi>() does
// not match, and aver_fw_install_managed_dispatch does the mirror-image check. The engine already
// knows how to guard this boundary; it just applied the guard to the structs and not to the plain
// integers beside them.
//
// WHAT DRIFT COSTS HERE. Insert a bit in the middle of the AVER_FW_CLASS_* list without editing
// Enums.cs and ClassFlags.Pawn silently means whatever bit is really Controller now -- which inverts
// the type test aver_fw_possess is built on. Renumber a tick group and actors tick in the wrong
// phase relative to physics. None of it fails a build, because the two sides are compiled by
// different toolchains from different files and never see each other.
//
// WHY A NATIVE SUITE AND NOT A C# TEST, and this is the load-bearing choice. The one managed test
// project in this tree (Aver.Graph.Tests) is wired into no CMakeLists, no script and no MCP tool --
// it must be run by hand, so it is available rather than enforced. tools/mcp/aver_mcp.py discovers
// suites by exactly one rule: any *Test.exe in <build>/bin. There is no CTest anywhere. So a native
// executable that READS the .cs files is the only venue in this repo where a check actually runs,
// which is the same reasoning tests/repo/src/SeparationTest.cpp states for itself and the same shape
// this file borrows.
//
// WHY THE PAIRING IS DERIVED, NOT TABULATED. A hand-written list of
// (AVER_FW_TICK_PRE_PHYSICS, TickGroup.PrePhysics) pairs would be a THIRD copy of the thing whose
// duplication is the bug -- and one that must be edited every time either side gains a member,
// which is precisely the discipline that failed. Instead each GROUP is named once (a C prefix and
// the C# type that mirrors it) and members are matched by normalising both spellings:
// AVER_FW_TICK_PRE_PHYSICS -> "prephysics" <- PrePhysics. Adding a member to BOTH sides needs no
// edit here. Adding it to one side fails, which is the entire point.
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ------------------------------------------------------------------ reading

static bool readText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Lower-cased with every underscore removed, which is the whole of the naming convention that
// separates the two languages: SCREAMING_SNAKE on the C side, PascalCase on the C# side, and nothing
// else. TICK_IN_EDITOR and TickInEditor both become "tickineditor".
static std::string normalise(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '_') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Parses `#define <PREFIX><NAME> <int>` -- decimal or 0x hex, which the flag groups use. Returns
// normalised name -> value. Anything that is not a plain integer literal (a macro referring to
// another macro, a cast, an expression) is skipped rather than guessed at.
static std::map<std::string, long long> cDefines(const std::string& text, const std::string& prefix) {
    std::map<std::string, long long> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // TRAILING COMMENTS COME OFF FIRST. Every constant in framework_hooks.h carries an aligned
        // /* ... */ describing it, and the "more tokens means this is not a simple constant" rule
        // below -- which is there to refuse function-like macros and expressions -- threw all seven
        // of them away. The first run of this suite reported both reason groups as EMPTY, which the
        // "found C constants" check turned into a failure rather than a silent pass. That check
        // earned its place immediately.
        if (const auto c = line.find("/*"); c != std::string::npos) line.erase(c);
        if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
        const auto d = line.find("#define ");
        if (d == std::string::npos) continue;
        std::istringstream ls(line.substr(d + 8));
        std::string name, value;
        if (!(ls >> name >> value)) continue;
        if (name.rfind(prefix, 0) != 0) continue;
        // Trailing tokens mean this is not a simple constant (a function-like macro, an expression).
        std::string extra;
        if (ls >> extra) continue;
        try {
            const long long v = std::stoll(value, nullptr, 0);   // base 0: handles 0x
            out[normalise(name.substr(prefix.size()))] = v;
        } catch (...) { /* not an integer literal; not ours to interpret */ }
    }
    return out;
}

// Parses the members of one C# `enum X { ... }` or `static class X { ... }` block. Handles both
// forms because the C side's flag groups are mirrored as a class of consts (ClassFlags) while the
// rest are real enums -- `Name = 0,` and `internal const int Name = 0x0001;` alike.
static std::map<std::string, long long> csMembers(const std::string& text, const std::string& typeName) {
    std::map<std::string, long long> out;
    // Find "enum <typeName>" or "class <typeName>", then take the braced block that follows.
    size_t at = std::string::npos;
    for (const char* kw : {"enum ", "class "}) {
        const size_t p = text.find(std::string(kw) + typeName);
        if (p != std::string::npos && (at == std::string::npos || p < at)) at = p;
    }
    if (at == std::string::npos) return out;
    const size_t open = text.find('{', at);
    if (open == std::string::npos) return out;
    const size_t close = text.find('}', open);
    if (close == std::string::npos) return out;
    const std::string body = text.substr(open + 1, close - open - 1);

    // Every `<name> = <int>` in the block. The declaration keywords before a const member
    // (internal/const/int) are skipped by taking the token immediately before '='.
    // SPLIT ON ',' AND ';' BOTH. A real enum separates members with commas; a static class of
    // consts (ClassFlags, which is how the C flag group is mirrored) separates them with semicolons.
    // Splitting on commas alone read the whole ClassFlags body as ONE member and reported 8 C
    // constants against 1 C# member.
    std::string flat = body;
    std::replace(flat.begin(), flat.end(), ';', ',');
    std::istringstream in(flat);
    std::string chunk;
    while (std::getline(in, chunk, ',')) {
        const size_t eq = chunk.find('=');
        if (eq == std::string::npos) continue;
        std::istringstream ns(chunk.substr(0, eq));
        std::string tok, last;
        while (ns >> tok) last = tok;            // the identifier is the token before '='
        std::istringstream vs(chunk.substr(eq + 1));
        std::string value;
        if (!(vs >> value) || last.empty()) continue;
        while (!value.empty() && (value.back() == ';' || value.back() == '}')) value.pop_back();
        try {
            out[normalise(last)] = std::stoll(value, nullptr, 0);
        } catch (...) { /* not an integer literal */ }
    }
    return out;
}

// ------------------------------------------------------------------ the groups

struct Group {
    const char* label;       // what to call it in the log
    const char* header;      // repo-relative C header
    const char* prefix;      // the #define prefix that selects this group
    const char* csFile;      // repo-relative .cs
    const char* csType;      // the enum or static class that mirrors it
    const char* skip;        // one normalised C name to ignore, or "" -- see below
};

// SENTINELS ARE SKIPPED BY NAME, one per group at most. AVER_FW_TICK_COUNT and the two _COUNT
// members below are "how many are there", not members of the set, and the C# side rightly has no
// mirror for them. Naming the exception here rather than pattern-matching "*count" keeps it honest:
// a group that grows a second unmirrored constant fails until someone decides that is intended.
static const Group kGroups[] = {
    {"framework class flags", "modules/framework/include/aver/framework/framework_abi.h",   "AVER_FW_CLASS_",
     "scripting/csharp/Aver.Framework/Enums.cs", "ClassFlags",  ""},
    {"framework tick groups", "modules/framework/include/aver/framework/framework_abi.h",   "AVER_FW_TICK_",
     "scripting/csharp/Aver.Framework/Enums.cs", "TickGroup",   "count"},
    {"framework play state",  "modules/framework/include/aver/framework/framework_abi.h",   "AVER_FW_PLAY_",
     "scripting/csharp/Aver.Framework/Enums.cs", "PlayState",   ""},
    {"actor begin reasons",   "modules/framework/include/aver/framework/framework_hooks.h", "AVER_FW_BEGIN_",
     "scripting/csharp/Aver.Framework/Enums.cs", "BeginReason", ""},
    {"actor end reasons",     "modules/framework/include/aver/framework/framework_hooks.h", "AVER_FW_END_",
     "scripting/csharp/Aver.Framework/Enums.cs", "EndReason",   ""},
    {"UI layers",             "modules/ui.abi/include/aver/ui/ui_abi.h",                    "AVER_UI_LAYER_",
     "scripting/csharp/Aver.UI/Hud.cs",          "Layer",       ""},
    {"PBR features",          "modules/render.pbr/include/aver/pbr/pbr_abi.h",              "AVER_PBR_FEATURE_",
     "scripting/csharp/Aver.Scripting/Pbr.cs",   "PbrFeature",  "count"},
    {"PBR status",            "modules/render.pbr/include/aver/pbr/pbr_abi.h",              "AVER_PBR_STATUS_",
     "scripting/csharp/Aver.Scripting/Pbr.cs",   "PbrStatus",   ""},
    {"Voxi features",         "modules/render.voxi/include/aver/voxi/voxi_abi.h",           "AVER_VOXI_FEATURE_",
     "scripting/csharp/Aver.Scripting/Voxi.cs",  "VoxiFeature", "count"},
    {"Voxi status",           "modules/render.voxi/include/aver/voxi/voxi_abi.h",           "AVER_VOXI_STATUS_",
     "scripting/csharp/Aver.Scripting/Voxi.cs",  "VoxiStatus",  ""},
    {"Voxi quality",          "modules/render.voxi/include/aver/voxi/voxi_abi.h",           "AVER_VOXI_QUALITY_",
     "scripting/csharp/Aver.Scripting/Voxi.cs",  "VoxiQuality", ""},
};

int main() {
    AVER_INFO("=== the C ABI's constants and their C# mirrors ===");

#ifndef AVER_REPO_ROOT
    AVER_WARN("=== SKIPPED: built without AVER_REPO_ROOT ===");
    return 0;
#else
    const std::string root = std::string(AVER_REPO_ROOT) + "/";

    for (const Group& g : kGroups) {
        std::string hdr, cs;
        if (!readText(root + g.header, hdr)) { check(false, std::string(g.label) + ": cannot read " + g.header); continue; }
        if (!readText(root + g.csFile, cs))  { check(false, std::string(g.label) + ": cannot read " + g.csFile); continue; }

        std::map<std::string, long long> c = cDefines(hdr, g.prefix);
        const std::map<std::string, long long> m = csMembers(cs, g.csType);
        if (*g.skip) c.erase(g.skip);

        // A GROUP THAT MATCHES NOTHING IS A FAILURE, not a silent pass. If a header is reorganised or
        // an enum renamed, the parse quietly returns nothing and every value check below vacuously
        // succeeds -- a check that cannot fail is the exact thing this suite exists to avoid.
        check(!c.empty(), std::string(g.label) + ": found C constants with prefix " + g.prefix);
        check(!m.empty(), std::string(g.label) + ": found C# members of " + g.csType);
        if (c.empty() || m.empty()) continue;

        check(c.size() == m.size(),
              std::string(g.label) + ": " + std::to_string(c.size()) + " C constant(s) and " +
              std::to_string(m.size()) + " C# member(s)");

        for (const auto& [name, value] : c) {
            const auto it = m.find(name);
            if (it == m.end()) {
                check(false, std::string(g.label) + ": '" + name + "' has no C# member");
                continue;
            }
            check(it->second == value,
                  std::string(g.label) + ": " + name + " = " + std::to_string(value));
        }
        for (const auto& [name, value] : m) {
            (void)value;
            if (!c.count(name)) check(false, std::string(g.label) + ": C# '" + name + "' has no C constant");
        }
    }

    if (g_failures == 0) AVER_INFO("=== every ABI constant matches its C# mirror ===");
    else                 AVER_ERROR("=== {} ABI mirror assertion(s) failed ===", g_failures);
    return g_failures == 0 ? 0 : 1;
#endif
}
