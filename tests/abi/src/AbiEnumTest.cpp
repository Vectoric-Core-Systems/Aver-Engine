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
#include "aver/core/CrashReport.hpp"
#include "aver/core/ErrorCodes.hpp"
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

// Whitespace off both ends. Beside normalise() because it does the same kind of job: turn a raw
// spelling into one that can be compared.
static std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
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

// The MEMBER NAMES of one C++ `enum class X { ... }`, in declaration order. Order is what this
// returns because that is what the caller needs: an enum that assigns no values gives each member
// its number by POSITION, so position is the contract and a reorder is a silent renumbering.
static std::vector<std::string> enumOrder(const std::string& text, const std::string& typeName) {
    std::vector<std::string> out;
    const auto e = text.find("enum class " + typeName);
    if (e == std::string::npos) return out;
    const auto open = text.find('{', e);
    if (open == std::string::npos) return out;
    const auto close = text.find('}', open);
    if (close == std::string::npos) return out;

    std::string cur;
    for (auto i = open + 1; i < close; ++i) {
        const char ch = text[i];
        if (ch == ',') { out.push_back(trimmed(cur)); cur.clear(); }
        else           { cur.push_back(ch); }
    }
    if (!trimmed(cur).empty()) out.push_back(trimmed(cur));

    // An explicit `= N` would make the name-to-value mapping something other than the position, and
    // this helper would be lying about what it returns. Refuse rather than guess.
    for (std::string& n : out) {
        if (n.find('=') != std::string::npos) { out.clear(); return out; }
    }
    return out;
}

// `<name> = <int>` out of one C++ `enum class X { ... }`, as normalised name -> value. The sibling
// of enumOrder above, for the enums whose contract is the WRITTEN value rather than the position.
//
// LINE COMMENTS COME OFF FIRST, and that is not caution -- ErrorCodes.hpp documents several members
// with a trailing // that contains commas ("a negative extent, an empty name, a NaN"), and a splitter
// that did not strip them would read three members where there is one.
static std::map<std::string, long long> cppEnumValues(const std::string& text, const std::string& typeName) {
    std::map<std::string, long long> out;
    const auto e = text.find("enum class " + typeName);
    if (e == std::string::npos) return out;
    const auto open = text.find('{', e);
    const auto close = open == std::string::npos ? std::string::npos : text.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return out;

    std::istringstream in(text.substr(open + 1, close - open - 1));
    std::string line, body;
    while (std::getline(in, line)) {
        if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
        body += line;
        body += '\n';
    }

    std::istringstream chunks(body);
    std::string chunk;
    while (std::getline(chunks, chunk, ',')) {
        const auto eq = chunk.find('=');
        if (eq == std::string::npos) continue;
        std::istringstream ns(chunk.substr(0, eq));
        std::string tok, last;
        while (ns >> tok) last = tok;
        std::istringstream vs(chunk.substr(eq + 1));
        std::string value;
        if (!(vs >> value) || last.empty()) continue;
        try { out[normalise(last)] = std::stoll(value, nullptr, 0); }
        catch (...) { /* not an integer literal */ }
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
    //
    // AND SO IS `[3]`, WHICH IS THE SAME THING SPELLED IN C. A C header may declare a parameter as a
    // fixed-size array -- `const float pointCm[3]` -- which is a POINTER, exactly like `float*`, and
    // is how physics_joints_abi.h spells nearly every anchor and axis it takes. Erasing that bracket
    // along with the attributes read the type as a by-value `float`, while the C# binding's `float[]`
    // read as `float*`, and the two disagreed on thirteen functions that were both correct. The rule
    // that tells them apart is what is INSIDE the brackets: a declarator holds a number or nothing, an
    // attribute holds a name. Collapsing `[N]` to `[]` puts the C spelling on the same footing as the
    // C# one and leaves canonType, which already understands `[]`, to mark both as pointers.
    for (size_t p = list.find('['); p != std::string::npos; p = list.find('[', p)) {
        if (p + 1 < list.size() && list[p + 1] == ']') { p += 2; continue; }
        const size_t e = list.find(']', p);
        if (e == std::string::npos) break;
        bool allDigits = e > p + 1;
        for (size_t i = p + 1; i < e && allDigits; ++i)
            if (list[i] < '0' || list[i] > '9') allDigits = false;
        if (allDigits) { list.erase(p + 1, e - p - 1); p += 2; continue; }   // `[3]` -> `[]`
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

        // A C++ DEFAULT ARGUMENT IS NOT PART OF THE ABI. physics_abi.h's aver_phys_softbody_create
        // ends `float damping = 0.1f, int32_t iterations = 5`, and without this that parameter's type
        // reads as "float damping =" and its NAME as "0.1f". Harmless until the day the function is
        // bound in C#, which is exactly when a parity test is supposed to be useful.
        if (const size_t eq = one.find('='); eq != std::string::npos) one.erase(eq);
        // C#'s nullable-reference annotation says something about null, not about the marshalled
        // type: `float[]?` and `float[]` are the same pointer as far as the CLR is concerned.
        for (size_t q = one.find('?'); q != std::string::npos; q = one.find('?')) one.erase(q, 1);

        while (!one.empty() && (one.back()  == ' ' || one.back()  == '\t')) one.pop_back();
        while (!one.empty() && (one.front() == ' ' || one.front() == '\t')) one.erase(one.begin());
        if (!one.empty()) {
            // the final whitespace-or-star separated token is the name, everything before it the type
            const size_t sp = one.find_last_of(" \t*");
            std::string type = (sp == std::string::npos) ? one : one.substr(0, sp + 1);
            std::string name = (sp == std::string::npos) ? std::string() : one.substr(sp + 1);
            // AN ARRAY DECLARATOR SITS ON THE NAME IN C AND ON THE TYPE IN C#. `const float p[3]`
            // and `float[] p` describe the same pointer, but the brackets land on opposite sides of
            // the space -- so the name gives them back to the type before either is compared.
            // Without this the C side reads as a by-value float called "p[]" and the C# side as a
            // "float*" called "p", disagreeing on both counts over one spelling difference.
            if (name.size() >= 2 && name.compare(name.size() - 2, 2, "[]") == 0) {
                name.erase(name.size() - 2);
                type += "[]";
            }
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
// Removes both kinds of comment IN ONE PASS, which is the only way to get it right.
//
// THIS USED TO BE TWO SEQUENTIAL SEARCH-AND-ERASE LOOPS, block comments first and then line comments,
// and that ordering is wrong in a way that silently ate whole headers. A line comment is allowed to
// contain the two characters that open a block comment -- physics_joints_abi.h's own opening
// paragraph names a build-artefact path with a wildcard in it -- and the block pass, running first
// and knowing nothing about line comments, treated that as the start of a block, found no terminator
// anywhere after it, and erased THE REST OF THE FILE. Both the joints and shapes headers parsed to
// zero exports because of one path in one sentence, and it presented as 29 believable "C# imports a
// function the header does not export" failures rather than as a parse error.
//
// Swapping the two passes only moves the bug: a block comment containing a line comment would then
// lose its own terminator to the line pass. A single left-to-right walk that knows which comment it
// is inside has neither problem, and is shorter than the two loops it replaces.
static std::string stripComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != 0x0A) ++i;
            if (i < in.size()) out.push_back(static_cast<char>(0x0A));
            continue;
        }
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            const size_t e = in.find("*/", i + 2);
            if (e == std::string::npos) break;   // unterminated: the rest genuinely is a comment
            i = e + 1;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}


static std::map<std::string, Signature> cSignatures(std::string text, const std::string& apiMacro,
                                                    const std::string& prefix) {
    text = stripComments(text);

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

// ONE C# FILE MAY MIRROR SEVERAL C HEADERS, which is why `headers` is a list.
//
// The physics ABI grew from 49 functions to 125 and split into five headers on the C side, while the
// C# side stayed one Native.cs -- a P/Invoke declaration has no natural home other than the assembly
// it lives in. Pairing them one-to-one would have made the base row see all 125 C# imports (its
// prefix, aver_phys_, matches every one of them) against only its own header's 74 exports, and report
// the other 51 as imports of functions that do not exist. They do exist; they are just declared next
// door. So a row names every header that together makes up the C side of one C# file.
struct Abi {
    const char* label;
    const char* headers[6];   // NULL-terminated; most rows name exactly one
    const char* apiMacro;
    const char* csFile;
    const char* prefix;
};

static const Abi kAbis[] = {
    {"physics ABI",
     {"modules/physics/include/aver/physics/physics_abi.h",
      "modules/physics/include/aver/physics/physics_joints_abi.h",
      "modules/physics/include/aver/physics/physics_shapes_abi.h",
      "modules/physics/include/aver/physics/physics_layers_abi.h",
      "modules/physics/include/aver/physics/physics_character_abi.h",
      nullptr},
     "AVER_PHYS_API", "scripting/csharp/Aver.Physics/Native.cs", "aver_phys_"},
    {"audio ABI",
     {"modules/audio.abi/include/aver/audio/audio_abi.h", nullptr},
     "AVER_AUDIO_API", "scripting/csharp/Aver.Framework/Audio.cs", "aver_audio_"},
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
    // THE LOG LEVELS, which are mirrored THREE times and were checked zero. Log.hpp says the order
    // "is load-bearing", names both mirrors by path, and warns that a value inserted in the middle
    // "would silently renumber every one of those without a single compile error" -- and then
    // nothing enforced it. This row covers the C-to-C# half; the C++ enum is checked below, because
    // it is an `enum class` and not a #define group.
    {"log levels",            "modules/scripting/include/aver/scripting/scripting_abi.h",   "AVER_SCRIPT_LOG_",
     "scripting/csharp/Aver.Scripting/Log.cs",   "Level",       ""},
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
        std::string cs;
        if (!readText(root + a.csFile, cs))  { check(false, std::string(a.label) + ": cannot read " + a.csFile); continue; }

        // Every header in the row, merged: together they are the C side of this one C# file.
        std::map<std::string, Signature> c;
        bool readAll = true;
        for (const char* h : a.headers) {
            if (!h) break;
            std::string hdr;
            if (!readText(root + h, hdr)) {
                check(false, std::string(a.label) + ": cannot read " + h);
                readAll = false;
                break;
            }
            for (auto& [n, sig] : cSignatures(hdr, a.apiMacro, a.prefix)) c.emplace(n, sig);
        }
        if (!readAll) continue;
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

    // ---- THE THIRD LOG MIRROR, and the two frozen numeric vocabularies ------------------------
    //
    // The group table above compares a C header against a .cs file. aver::LogLevel is neither: it is
    // a C++ `enum class` in Log.hpp whose ORDER is what gives every value its number. Log.hpp warns
    // in as many words that inserting a level "would silently renumber every one of those without a
    // single compile error" -- so the order is read here and compared against the C constants.
    AVER_INFO("=== the C++ side of the mirrors, and the frozen codes ===");
    {
        std::string log, abi;
        const bool haveLog = readText(root + "modules/core/include/aver/core/Log.hpp", log);
        const bool haveAbi = readText(root + "modules/scripting/include/aver/scripting/scripting_abi.h", abi);
        check(haveLog, "log levels: can read Log.hpp");
        check(haveAbi, "log levels: can read scripting_abi.h");
        if (haveLog && haveAbi) {
            const std::map<std::string, long long> c = cDefines(abi, "AVER_SCRIPT_LOG_");
            const std::vector<std::string> names = enumOrder(log, "LogLevel");
            check(!names.empty(), "log levels: LogLevel's body parses");
            check(names.size() == c.size(),
                  "log levels: " + std::to_string(names.size()) + " C++ level(s) and " +
                  std::to_string(c.size()) + " C constant(s)");
            // The POSITION of a name in the C++ enum is its value, because LogLevel assigns none.
            // So the check is that position against the number the C header hands out under the same
            // name -- derived on both sides, exactly like the group table above, so adding a level to
            // both needs no edit here and adding it to one fails.
            for (usize i = 0; i < names.size(); ++i) {
                const std::string key = normalise(names[i]);
                const auto it = c.find(key);
                if (it == c.end()) {
                    check(false, "log levels: LogLevel::" + names[i] + " has no AVER_SCRIPT_LOG_ constant");
                    continue;
                }
                check(it->second == static_cast<long long>(i),
                      "log levels: LogLevel::" + names[i] + " is " + std::to_string(i) +
                      " and AVER_SCRIPT_LOG_ says " + std::to_string(it->second) +
                      " -- the C++ value is its POSITION, so a reorder is an ABI break with no compile error");
            }
        }
    }

    // AbiError is mirrored in C# ONCE PER MODULE that exposes the channel, because each module's
    // slot is its own (Aver.Core is a static library linked into each ABI DLL). Three copies of one
    // numbering is exactly the shape this whole suite exists for, so they are compared here rather
    // than trusted. Derived on both sides: adding a code to all three needs no edit in this file.
    {
        std::string hpp, phys, scene;
        const bool ok = readText(root + "modules/core/include/aver/core/ErrorCodes.hpp", hpp) &
                        readText(root + "scripting/csharp/Aver.Physics/Enums.cs", phys) &
                        readText(root + "scripting/csharp/Aver.Scene/Native.cs", scene);
        check(ok, "abi errors: can read ErrorCodes.hpp and both C# mirrors");
        if (ok) {
            const std::map<std::string, long long> c = cppEnumValues(hpp, "AbiError");
            check(!c.empty(), "abi errors: AbiError parses");
            const struct { const char* label; const std::string* text; const char* type; } kMirrors[] = {
                {"Aver.Physics.PhysicsError", &phys,  "PhysicsError"},
                {"Aver.Scene.SceneError",     &scene, "SceneError"},
            };
            for (const auto& mi : kMirrors) {
                const std::map<std::string, long long> m = csMembers(*mi.text, mi.type);
                check(!m.empty(), std::string("abi errors: found C# members of ") + mi.type);
                check(c.size() == m.size(),
                      std::string("abi errors: ") + std::to_string(c.size()) + " C++ code(s) and " +
                      std::to_string(m.size()) + " in " + mi.label);
                for (const auto& [name, v] : c) {
                    const auto it = m.find(name);
                    if (it == m.end()) {
                        check(false, std::string("abi errors: ") + mi.label + " has no member for AbiError::" + name);
                        continue;
                    }
                    check(it->second == v,
                          std::string("abi errors: ") + name + " is " + std::to_string(v) + " in C++ and " +
                          std::to_string(it->second) + " in " + mi.label);
                }
            }
        }
    }

    // The two vocabularies in core/ErrorCodes.hpp. Frozen because both cross a boundary as bare
    // integers: an exit code is read by a shell and by CI, and an AbiError is read by a binding that
    // may have been built against an older header.
    {
        check(static_cast<int>(ExitCode::Ok) == 0 && static_cast<int>(ExitCode::Failed) == 1 &&
              static_cast<int>(ExitCode::Usage) == 2 && static_cast<int>(ExitCode::Environment) == 3 &&
              static_cast<int>(ExitCode::Interrupted) == 4,
              "the exit codes are 0..4 and frozen -- a shell and a CI job read these");
        check(std::string(exitCodeName(ExitCode::Usage)) == "usage", "and each one names itself");

        check(static_cast<i32>(AbiError::Ok) == 0, "AbiError::Ok is 0, so `if (last_error())` reads as 'something failed'");
        check(static_cast<i32>(AbiError::BadHandle) == -1 && static_cast<i32>(AbiError::NullPointer) == -2 &&
              static_cast<i32>(AbiError::NotInitialised) == -3 && static_cast<i32>(AbiError::OutOfRange) == -4 &&
              static_cast<i32>(AbiError::Unsupported) == -5 && static_cast<i32>(AbiError::InvalidArgument) == -6 &&
              static_cast<i32>(AbiError::AllocationFailed) == -7,
              "and the AbiError codes are -1..-7 and frozen -- append, never insert");
        // EVERY code is NEGATIVE except Ok, which is what lets a caller test `code < 0` for "failed"
        // without knowing the whole list -- including codes added after their binding was built.
        check(static_cast<i32>(AbiError::AllocationFailed) < 0 && static_cast<i32>(AbiError::BadHandle) < 0,
              "every error is negative, so `code < 0` means 'failed' even for codes this build knows nothing about");

        check(std::string(abiErrorNameOf(-3)) == "not initialised", "a raw code names itself");
        check(std::string(abiErrorNameOf(-999)) == "unknown error code",
              "and an UNKNOWN code is named as unknown rather than switched over as if it were valid");
        check(std::string(abiErrorNameOf(1)) == "not an error (positive)",
              "a positive value is called out -- that is a success return handed to the wrong function");
    }

    // crash::Kind is written into every crash report as a number, so it outlives the build.
    {
        check(crash::kindCode(crash::Kind::Crash) == 0 && crash::kindCode(crash::Kind::Assert) == 1 &&
              crash::kindCode(crash::Kind::Fatal) == 2 && crash::kindCode(crash::Kind::GpuCrash) == 3 &&
              crash::kindCode(crash::Kind::Terminate) == 4,
              "the five original crash kinds keep codes 0..4 -- reports already on disk carry them");
        check(crash::kindCode(crash::Kind::OutOfMemory) == 5, "and OutOfMemory was APPENDED at 5");
        // The name a report reader gets must match the name the writer used, for every kind.
        for (int c = 0; c <= 5; ++c) {
            check(std::string(crash::kindNameOf(c)) == std::string(crash::kindName(static_cast<crash::Kind>(c))),
                  std::string("crash kind ") + std::to_string(c) + " reads back as it was written (" +
                  crash::kindNameOf(c) + ")");
        }
        check(std::string(crash::kindNameOf(99)) == "Unknown",
              "and a kind from a NEWER build reads as Unknown rather than falling off the switch");
    }

    if (g_failures == 0) AVER_INFO("=== every ABI constant and signature matches its C# mirror ===");
    else                 AVER_ERROR("=== {} ABI mirror assertion(s) failed ===", g_failures);
    return g_failures == 0 ? 0 : 1;
#endif
}
