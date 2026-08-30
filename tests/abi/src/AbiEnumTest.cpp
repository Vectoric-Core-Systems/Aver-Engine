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

// -------------------------------------------------------------- signatures

// THE OTHER HALF OF THIS BOUNDARY. Everything above compares NUMBERS. The rest of what crosses into
// C# is FUNCTION SIGNATURES -- 53 of them across the physics and audio ABIs -- and P/Invoke checks
// none of it. Marshalling is purely positional: the CLR takes the managed signature at face value,
// pushes arguments in that order, and jumps. There is no export-table comparison that could
// disagree, no mangling to mismatch, nothing at load time that reads the C declaration at all.
//
// So inserting a parameter, widening one, or swapping two of the same type compiles cleanly on both
// sides and produces a running program that moves the wrong numbers: a script reading velocity where
// mass belongs, a raycast normal landing in the point slot. The audit that prompted this found the
// two lists in exact agreement today -- this exists so they stay that way.
//
// A SOURCE-LEVEL CHECK, AND ITS LIMIT STATED PLAINLY. The stronger test is a managed round-trip
// against the real DLL, which would catch things text cannot. It also needs a dotnet toolchain, the
// built native DLL and a runtime, and this repo's one managed test project is wired into no
// CMakeLists, no script and no MCP tool. Reading both declarations catches arity, type and
// return-type drift with none of that, in the suite that actually runs. What it cannot catch is a
// rename applied to both sides at once -- which is a refactor, not a drift.
struct Signature {
    std::string              ret;
    std::vector<std::string> types;
    std::vector<std::string> names;
};

// A POINTER IS A POINTER, however each language spells it. C's `float*`, C#'s `float[]` and C#'s
// `out int` are the same thing at the call site -- an address -- so all three normalise to `T*`.
// Getting this wrong in the obvious direction (treating `out int` as a value `int`) reports three
// false mismatches on a file that is correct, which is exactly what the first draft of this did.
static std::string canonType(std::string t) {
    // `out`/`ref` ARE the pointer -- they do not merely decorate the type. Stripping them like a
    // qualifier turns `out int` into a value `int`, which is what made the first run of this suite
    // report four false mismatches against a header that was correct.
    bool ptr = (t.find("out ") != std::string::npos) || (t.find("ref ") != std::string::npos);
    for (const char* q : {"const ", "out ", "ref ", "in ", "unsafe "}) {
        const std::string s = q;
        for (size_t p = t.find(s); p != std::string::npos; p = t.find(s)) t.erase(p, s.size());
    }
    for (size_t p = t.find('*'); p != std::string::npos; p = t.find('*')) { t.erase(p, 1); ptr = true; }
    for (size_t p = t.find("[]"); p != std::string::npos; p = t.find("[]")) { t.erase(p, 2); ptr = true; }
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.pop_back();

    if (t == "int32_t")  t = "int";
    if (t == "uint32_t") t = "uint";
    if (t == "int64_t")  t = "long";
    if (t == "uint64_t") t = "ulong";
    if (t == "uint8_t")  t = "byte";
    if (t == "string") { t = "char"; ptr = true; }   // C# string <-> const char*
    return ptr ? t + "*" : t;
}

// Splits a parameter list into (type, name) pairs. `void` and an empty list are both no parameters.
static void splitParams(std::string list, Signature& sig) {
    // C# puts marshalling attributes INSIDE the parameter list --
    // `[MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path` -- and they can contain their own
    // commas, so they come out before anything is split. `[]` is left alone: that is an array, and
    // erasing it would silently turn `float[]` into a by-value float.
    for (size_t p = list.find('['); p != std::string::npos; p = list.find('[', p)) {
        if (p + 1 < list.size() && list[p + 1] == ']') { p += 2; continue; }
        const size_t e = list.find(']', p);
        if (e == std::string::npos) break;
        list.erase(p, e - p + 1);
    }
    if (list.empty() || list == "void") return;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string one = list.substr(start, comma - start);
        const bool last = (comma == list.size());
        start = comma + 1;

        while (!one.empty() && (one.back()  == ' ' || one.back()  == '\t')) one.pop_back();
        while (!one.empty() && (one.front() == ' ' || one.front() == '\t')) one.erase(one.begin());
        if (!one.empty()) {
            // the final whitespace-or-star separated token is the name, everything before it the type
            const size_t sp = one.find_last_of(" \t*");
            std::string type = (sp == std::string::npos) ? one : one.substr(0, sp + 1);
            std::string name = (sp == std::string::npos) ? std::string() : one.substr(sp + 1);
            sig.types.push_back(canonType(type));
            sig.names.push_back(normalise(name));
        }
        if (last) break;
    }
}

// Collapses runs of whitespace, so a declaration wrapped across three lines reads like a one-liner.
static std::string flatten(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// Every `<apiMacro> <ret> <prefix>name(params);` in a C header. Comments come out first: these
// declarations carry doc comments that would otherwise land inside a parameter list.
static std::map<std::string, Signature> cSignatures(std::string text, const std::string& apiMacro,
                                                    const std::string& prefix) {
    for (size_t p = text.find("/*"); p != std::string::npos; p = text.find("/*")) {
        const size_t e = text.find("*/", p);
        if (e == std::string::npos) { text.erase(p); break; }
        text.erase(p, e - p + 2);
    }
    for (size_t p = text.find("//"); p != std::string::npos; p = text.find("//")) {
        const size_t e = text.find('\n', p);
        text.erase(p, (e == std::string::npos ? text.size() : e) - p);
    }

    std::map<std::string, Signature> out;
    for (size_t p = text.find(apiMacro); p != std::string::npos; p = text.find(apiMacro, p + 1)) {
        const size_t semi = text.find(';', p);
        const size_t open = text.find('(', p);
        if (semi == std::string::npos || open == std::string::npos || open > semi) continue;
        const size_t close = text.rfind(')', semi);
        if (close == std::string::npos || close < open) continue;

        const std::string head = flatten(text.substr(p + apiMacro.size(), open - p - apiMacro.size()));
        const size_t sp = head.find_last_of(" \t*");
        if (sp == std::string::npos) continue;
        const std::string name = head.substr(sp + 1);
        if (name.rfind(prefix, 0) != 0) continue;

        Signature sig;
        sig.ret = canonType(head.substr(0, sp + 1));
        splitParams(flatten(text.substr(open + 1, close - open - 1)), sig);
        out[name] = sig;
    }
    return out;
}

// Every `static extern <ret> <prefix>name(params)` in a .cs file.
static std::map<std::string, Signature> csSignatures(const std::string& text, const std::string& prefix) {
    const std::string kw = "static extern";
    std::map<std::string, Signature> out;
    for (size_t p = text.find(kw); p != std::string::npos; p = text.find(kw, p + 1)) {
        const size_t open = text.find('(', p);
        if (open == std::string::npos) continue;
        // THE MATCHING PAREN, NOT THE FIRST ONE. A marshalling attribute brings its own parentheses
        // into the parameter list -- `([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path)` -- so
        // the first `)` closes MarshalAs, not the function, and the list gets truncated mid-attribute.
        size_t close = std::string::npos;
        for (size_t i = open, depth = 0; i < text.size(); ++i) {
            if (text[i] == '(') ++depth;
            else if (text[i] == ')') { if (--depth == 0) { close = i; break; } }
            else if (text[i] == ';' && depth == 0) break;
        }
        if (close == std::string::npos) continue;

        const std::string head = flatten(text.substr(p + kw.size(), open - p - kw.size()));
        const size_t sp = head.find_last_of(" \t*");
        if (sp == std::string::npos) continue;
        const std::string name = head.substr(sp + 1);
        if (name.rfind(prefix, 0) != 0) continue;

        Signature sig;
        sig.ret = canonType(head.substr(0, sp + 1));
        splitParams(flatten(text.substr(open + 1, close - open - 1)), sig);
        out[name] = sig;
    }
    return out;
}

struct Abi {
    const char* label;
    const char* header;
    const char* apiMacro;
    const char* csFile;
    const char* prefix;
};

static const Abi kAbis[] = {
    {"physics ABI", "modules/physics/include/aver/physics/physics_abi.h", "AVER_PHYS_API",
     "scripting/csharp/Aver.Framework/Physics.cs", "aver_phys_"},
    {"audio ABI",   "modules/audio.abi/include/aver/audio/audio_abi.h",   "AVER_AUDIO_API",
     "scripting/csharp/Aver.Framework/Audio.cs",   "aver_audio_"},
};

// A DOCUMENTED, DELIBERATE DIVERGENCE. Parameter NAMES are compared because types alone cannot see
// the failure this check most wants to catch: swapping two same-typed arguments, which is nearly
// every argument in these functions -- aver_phys_body_set_velocity takes three bare floats. One pair
// genuinely differs and is correct, the C header spelling contact_get's out-params outBodyA/outBodyB
// where the C# says outA/outB. Waiving that one costs two lines and keeps the name check strict
// everywhere else, which is worth far more than dropping it because of a single rename.
struct NameWaiver { const char* fn; const char* cName; const char* csName; };
static const NameWaiver kNameWaivers[] = {
    {"aver_phys_contact_get", "outbodya", "outa"},
    {"aver_phys_contact_get", "outbodyb", "outb"},
};

static bool waivedName(const std::string& fn, const std::string& a, const std::string& b) {
    for (const NameWaiver& w : kNameWaivers)
        if (fn == w.fn && a == w.cName && b == w.csName) return true;
    return false;
}

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
    AVER_INFO("=== the C ABI's constants, signatures and their C# mirrors ===");

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


    // ------------------------------------------------ function signatures
    for (const Abi& a : kAbis) {
        std::string hdr, cs;
        if (!readText(root + a.header, hdr)) { check(false, std::string(a.label) + ": cannot read " + a.header); continue; }
        if (!readText(root + a.csFile, cs))  { check(false, std::string(a.label) + ": cannot read " + a.csFile); continue; }

        const std::map<std::string, Signature> c = cSignatures(hdr, a.apiMacro, a.prefix);
        const std::map<std::string, Signature> m = csSignatures(cs, a.prefix);

        // Same rule as the constant groups above: a parse that finds nothing must FAIL. Rename the
        // export macro or move the .cs file and every comparison below succeeds vacuously.
        check(!c.empty(), std::string(a.label) + ": found C exports behind " + a.apiMacro);
        check(!m.empty(), std::string(a.label) + ": found C# imports of " + a.prefix + "*");
        if (c.empty() || m.empty()) continue;

        size_t mirrored = 0;
        for (const auto& [name, sm] : m) {
            const auto it = c.find(name);
            // A C# import with no C export is not drift, it is a guaranteed
            // EntryPointNotFoundException the first time that function is called.
            if (it == c.end()) {
                check(false, std::string(a.label) + ": C# imports '" + name + "', which the header does not export");
                continue;
            }
            ++mirrored;
            const Signature& sc = it->second;

            check(sc.ret == sm.ret,
                  std::string(a.label) + ": " + name + " returns " + sc.ret + " / " + sm.ret);

            if (sc.types.size() != sm.types.size()) {
                check(false, std::string(a.label) + ": " + name + " takes " +
                             std::to_string(sc.types.size()) + " parameter(s) in C and " +
                             std::to_string(sm.types.size()) + " in C#");
                continue;   // positions are meaningless once the counts differ
            }

            bool sameTypes = true, sameNames = true;
            for (size_t i = 0; i < sc.types.size(); ++i) {
                if (sc.types[i] != sm.types[i]) {
                    sameTypes = false;
                    check(false, std::string(a.label) + ": " + name + " parameter " + std::to_string(i) +
                                 " is " + sc.types[i] + " in C and " + sm.types[i] + " in C#");
                }
                if (sc.names[i] != sm.names[i] && !waivedName(name, sc.names[i], sm.names[i])) {
                    sameNames = false;
                    check(false, std::string(a.label) + ": " + name + " parameter " + std::to_string(i) +
                                 " is named " + sc.names[i] + " in C and " + sm.names[i] + " in C#" +
                                 " -- if this is a deliberate rename, waive it; if the arguments moved, this is the bug");
                }
            }
            if (sameTypes && sameNames)
                check(true, std::string(a.label) + ": " + name + " (" + std::to_string(sc.types.size()) + " params)");
        }

        // Not every export is bound -- C# reaches the subset scripts need -- so an unbound export is
        // reported, not failed. The count moving is still worth seeing in the log.
        check(mirrored > 0, std::string(a.label) + ": " + std::to_string(mirrored) + " of " +
                            std::to_string(c.size()) + " C export(s) are mirrored in C#");
    }

    if (g_failures == 0) AVER_INFO("=== every ABI constant and signature matches its C# mirror ===");
    else                 AVER_ERROR("=== {} ABI mirror assertion(s) failed ===", g_failures);
    return g_failures == 0 ? 0 : 1;
#endif
}
