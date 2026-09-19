// SeparationTest -- the engine tree holds no game content, only one file knows where projects live,
// and the decision header both render hosts share is not copied back into either of them.
//
// docs/PROJECTS.md opens with the claim this suite exists to keep true: "Aver Engine is a standalone,
// reusable engine -- it never contains game content." That is an architectural promise, and until now
// it was enforced by nothing but habit. A promise checked only by habit is the shape this repo has
// been bitten by before: something is declared, everyone believes it, and years later it is quietly
// false. So it is checked here, by reading the tree.
//
// WHY A SUITE AND NOT A SCRIPT. There are already two source-scanning scripts in scripts/, and no CI
// to run either. A check nobody runs cannot fail, and a check that cannot fail is decoration. This is
// an executable named *Test, so it is picked up by the same sweep as the other suites and runs every
// time anyone asks for the tests.
//
// WHAT IT DOES NOT CHECK, deliberately. Roughly ninety comments across the tree name a project --
// "measured on ElectricDreams at 2750x1639", "the FirstPerson pool". Those are where a MEASUREMENT
// came from, and given this repo's history of performance claims that nothing backed, stripping the
// provenance would be a step backwards. Comments are lexed away and ignored. The dangerous kind of
// mention is a PATH or a code branch -- something the engine would act on -- and that lives in a
// string literal, which is exactly what survives the lexer.
//
// THE THIRD CLAIM is younger than the other two and is the same shape as both. The editor/runtime
// split exists to end up with ONE runtime, and for the draw walk that lands in
// aver/game/SceneSubmission.hpp: a single header stating, exactly once, that per entity ONE function
// produces the draw list and that a cull decides only WHO delivers it, never WHAT is in it. Both
// hosts are meant to include that header. For most of its life Runtime/src/GameRender.cpp instead
// carried a hand-kept COPY of it -- at :49, :57 and :66 of the version this check replaced: its own
// PlannedDraw, its own kMaxPlannedDraws whose comment could only say "Matches
// SandboxApp::kMaxPlannedDraws" and hope, its own planEntityDraws, and an inline re-derivation of
// the look ladder -- and it said so in its own words: "ported, not included". By then the two
// statements had already drifted, which is the whole point. The copy's ladder
// branched on `if (authored)` with no liveness check, so a dead material handle baked the
// bright-white-mirror identity in as a surface's FINAL look, which is exactly the fall-through
// resolveSurfaceLook exists to guarantee (docs/RUNTIME-DEDUP.md:172 records the same gap from the
// warnDeadMaterialHandle side). Two hosts disagreed about what a culled entity draws, both looked
// correct read on their own, and nothing in the tree could notice. The copy is gone now; the last
// check below is what stops it growing back, because "include it, do not copy it" is otherwise
// enforced by precisely the habit this suite was written to distrust.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- the tree

// Directories never descended into, matched by name at any depth. Two of these are not obvious and
// both were found by running this suite rather than by reasoning about it:
//
//   build/, build-release/   contain STAGED COPIES of templates/ and test-content/, so without the
//                            skip every content check reports the same asset three times
//   .claude/                 holds git WORKTREES -- entire additional checkouts of this repository.
//                            The first run of this suite reported 84 stray assets and every one of
//                            them was a worktree copy of a file that is perfectly legal where it
//                            really lives. A check that reports another checkout of itself is
//                            reporting on tooling state, not on the engine.
static const char* kSkipDirs[] = {
    ".git", ".vs", ".claude", "build", "build-release", "build-matrix", "out",
    "third_party", "node_modules", "bin", "obj",
};

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static bool endsWithLower(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char c = s[s.size() - suffix.size() + i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

// Every file under root, as a path relative to it with '/' separators.
static std::vector<std::string> walk(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            for (const char* skip : kSkipDirs) {
                if (name == skip) { it.disable_recursion_pending(); break; }
            }
            continue;
        }
        std::string rel = fs::relative(it->path(), root, ec).string();
        if (ec) { ec.clear(); continue; }
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- lexing out everything but literals

// True for the character after a backslash in a C/C++/C# escape sequence. Used to tell "C:\Users"
// (a path, one literal backslash, and a hit) from "public:\n" (a newline escape, and not one). Note
// the set is CASE SENSITIVE on purpose: \u is a unicode escape, \U is the start of "\Users".
static bool isEscapeChar(char c) {
    return c == 'n' || c == 't' || c == 'r' || c == '0' || c == '"' || c == '\'' || c == '\\' ||
           c == 'a' || c == 'b' || c == 'f' || c == 'v' || c == 'x' || c == 'u';
}

// Returns the string literals in src, with comments and code discarded. Escape sequences are kept
// RAW (a "\\" in the source stays two characters) because the absolute-path test below needs to tell
// an escaped backslash from an escape sequence.
//
// Handles the four literal forms that appear in this tree: ordinary "...", C++ raw strings
// R"delim(...)delim" -- which is how every HLSL shader in this engine is written, so their bodies are
// scanned like any other literal -- C# verbatim @"..." where a lone backslash is literal, and '.'
// character literals, which are skipped so that '"' does not open a phantom string.
static std::vector<std::string> stringLiterals(const std::string& src) {
    std::vector<std::string> out;
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        const char c = src[i];

        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = std::min(i + 2, n);
            continue;
        }

        // C++ raw string: R"delim( ... )delim". The delimiter is at most 16 characters by the
        // standard, which is what bounds the search for '(' and stops a stray R" in prose running on.
        if (c == 'R' && i + 1 < n && src[i + 1] == '"') {
            const size_t open = src.find('(', i + 2);
            if (open != std::string::npos && open - (i + 2) <= 16) {
                const std::string close = ")" + src.substr(i + 2, open - (i + 2)) + "\"";
                const size_t end = src.find(close, open);
                if (end != std::string::npos) {
                    out.push_back(src.substr(open + 1, end - open - 1));
                    i = end + close.size();
                    continue;
                }
            }
        }

        // C# verbatim string: @"...", where "" is the only escape and a backslash is literal.
        if (c == '@' && i + 1 < n && src[i + 1] == '"') {
            i += 2;
            std::string buf;
            while (i < n) {
                if (src[i] == '"') {
                    if (i + 1 < n && src[i + 1] == '"') { buf += '"'; i += 2; continue; }
                    ++i;
                    break;
                }
                buf += src[i++];
            }
            out.push_back(buf);
            continue;
        }

        if (c == '"') {
            ++i;
            std::string buf;
            while (i < n && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < n) { buf += src[i]; buf += src[i + 1]; i += 2; continue; }
                buf += src[i++];
            }
            i = std::min(i + 1, n);
            out.push_back(buf);
            continue;
        }

        if (c == '\'') {
            ++i;
            while (i < n && src[i] != '\'') i += (src[i] == '\\') ? 2 : 1;
            i = std::min(i + 1, n);
            continue;
        }

        ++i;
    }
    return out;
}

// True when the literal contains a drive-letter absolute path -- "C:/x", "C:\\x" (escaped, in an
// ordinary literal) or "C:\x" (unescaped, in a C# verbatim literal).
static bool hasAbsolutePath(const std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        const char c = s[i];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter || s[i + 1] != ':') continue;
        const char sep = s[i + 2];
        if (sep == '/') return true;
        if (sep != '\\') continue;
        const char after = (i + 3 < s.size()) ? s[i + 3] : '\0';
        if (after == '\\') return true;         // an escaped backslash: a real separator
        if (!isEscapeChar(after)) return true;  // a verbatim backslash: also a real separator
    }
    return false;
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- what the tree is allowed to hold

// A file the ENGINE would load as a game asset. If one of these sits outside a sanctioned root, the
// engine has started carrying content.
static bool isGameContent(const std::string& rel) {
    static const char* kExts[] = {
        ".ocmap", ".ocmesh", ".ocmat", ".ocskel", ".ocanim", ".ocgraph", ".ocparticle",
        ".ocsnd", ".ocbeam", ".octex", ".ocworld", ".oclevel", ".ocproject",
    };
    for (const char* e : kExts)
        if (endsWithLower(rel, e)) return true;
    return false;
}

// The four places content is allowed, each for a stated reason rather than because it happens to be
// where something ended up:
//   templates/     the engine's own starter templates -- shipped BY the engine, copied INTO a project
//   test-content/  fixtures the suites open; miniature projects on purpose, so the loaders see the
//                  real shape rather than a mock
//   content/       engine development fixtures (Rig.gltf is the tree's only skinned asset source)
//   scripting/csharp/Aver.Graph.Tests/   .ocgraph fixtures for the managed graph tests
static bool inSanctionedContentRoot(const std::string& rel) {
    return startsWith(rel, "templates/") || startsWith(rel, "test-content/") ||
           startsWith(rel, "content/") || startsWith(rel, "scripting/csharp/Aver.Graph.Tests/");
}

// Engine source: the code that ships as the engine and the editor. tests/ is deliberately NOT here --
// a test may legitimately hard-code an absolute path as INPUT (tests/audio reads C:/Windows/Media,
// tests/render.voxi feeds a fake C:\Projects\Demo to the cache), and forbidding that would be
// forbidding the wrong thing.
// SHADERS ARE COVERED BY `modules/` AND `sandbox/`, which is worth stating because the two roots that
// used to say so explicitly are gone. `interop/` and `shaders/` were top-level directories holding one
// README each and no source at all; both were deleted, and neither ever contributed a path here.
//
// The 25 engine-authored .hlsl/.hlsli files this suite scans live at modules/<mod>/shaders/ and
// sandbox/shaders/*.hlsl, so `startsWith(rel, "modules/")` and `startsWith(rel, "sandbox/")` already
// reach every one of them, and `.hlsl`/`.hlsli` are in kExts below. A literal "modules/*/shaders/"
// cannot be added in their place: kRoots is matched with startsWith, not a glob, so that string would
// be a prefix of nothing and would silently narrow the sweep rather than widen it.
static bool isEngineSource(const std::string& rel) {
    static const char* kRoots[] = { "modules/", "sandbox/", "tools/", "scripting/" };
    static const char* kExcept[] = { "third_party/", "modules/physics.jolt/" };
    for (const char* x : kExcept)
        if (startsWith(rel, x)) return false;
    bool inRoot = false;
    for (const char* r : kRoots)
        if (startsWith(rel, r)) { inRoot = true; break; }
    if (!inRoot) return false;
    static const char* kExts[] = { ".c", ".cc", ".cpp", ".h", ".hpp", ".cs", ".rs", ".fs", ".hlsl", ".hlsli" };
    for (const char* e : kExts)
        if (endsWithLower(rel, e)) return true;
    return false;
}

// THE ONE FILE that is allowed to know where projects live. ProjectBrowser is the screen that asks
// the user to pick one, so it carries the convention -- documentsDir() + "\Aver Projects" as the
// folder it opens in, and "C:\...\Name.ocproject" as the shape of the path it wants. Everything
// downstream of it receives a path; nothing else derives one.
static const char* kProjectPathOwner = "sandbox/src/ProjectBrowser.cpp";

// ---------------------------------------------------------------- the decision header, stated once

// THE ONE HEADER that owns the draw-submission rule. Both render hosts include it; neither may
// restate it. It reaches them through Aver.Runtime.Game.Core's PUBLIC include directory
// (cmake/AvModule.cmake:15), which is why it can sit outside both host directories -- and that is
// load-bearing for the sweep below, which therefore needs no exception carved out for the header
// itself. Under Runtime/src/ and sandbox/src/ there is no legal declaration of these names at all,
// which is a much easier rule to check than "exactly one, over there".
static const char* kSubmissionHeader = "Runtime/include/aver/game/SceneSubmission.hpp";

// The two directories that are only ever allowed to CALL what that header declares.
static bool inRenderHost(const std::string& rel) {
    return startsWith(rel, "Runtime/src/") || startsWith(rel, "sandbox/src/");
}

// A name the header owns, and whether it is a TYPE (introduced by a struct/class tag) or a
// FUNCTION-OR-CONSTANT (introduced by a preceding type name). Those two are recognised by different
// shapes below, so which is which is recorded here rather than guessed from the spelling.
struct OwnedName {
    const char* name;
    bool isType;
};

// TWO OF THE HEADER'S NAMES ARE DELIBERATELY MISSING from this table, on the principle that a check
// which fails for an innocent reason gets deleted rather than fixed:
//
//   SurfaceLook   is not this header's alone. GameContent.hpp:197 has a nested SurfaceLook of its
//                 own -- a genuinely different type, and the older of the two -- and
//                 SandboxApp.hpp:98-101 spends four lines on the fact that they coexist on purpose
//                 and must always be written qualified. Watching a name two headers legitimately
//                 share would be reporting on C++ name lookup, not on a copied rule.
//   deliver       is one common verb, and would trip on any unrelated deliver() either host ever
//                 grows. VoxiDelivery, the struct it exists to return, is watched in its place: a
//                 second deliver() worth catching has to restate that struct to have anything to
//                 hand back.
static const OwnedName kOwnedNames[] = {
    { "SurfaceInputs",          true  },
    { "PlannedDraw",            true  },
    { "RouteDecision",          true  },
    { "VoxiDelivery",           true  },
    { "kMaxPlannedDraws",       false },
    { "planEntityDraws",        false },
    { "resolveSurfaceLook",     false },
    { "chooseRoute",            false },
    { "occlusionTestShouldRun", false },
};

// src with every comment and every string/character literal blanked to spaces, the same length as
// the original and with every newline left exactly where it was, so a byte offset into the result
// still names a line in the real file.
//
// This is the exact INVERSE of stringLiterals() above, and it is inverted because it asks the
// opposite question. That check asks what the ENGINE would act on, so it keeps literals and throws
// the code away. This one asks what the COMPILER would act on, so it keeps the code and throws the
// prose away. Both host directories are now full of comments that legitimately name planEntityDraws
// and chooseRoute while explaining the rule -- de-duplicating the copy ADDED such comments, on both
// sides -- so a matcher that counted those would fire on its own documentation the day it shipped.
static std::string codeOnly(const std::string& src) {
    const size_t n = src.size();
    std::string out(n, ' ');
    // Newlines survive inside a discarded span as well as outside one, which is the whole reason the
    // line numbers this reports are the line numbers a reader will find in the file.
    auto blank = [&](size_t from, size_t to) {
        for (size_t k = from; k < to && k < n; ++k)
            if (src[k] == '\n') out[k] = '\n';
    };
    size_t i = 0;
    while (i < n) {
        const char c = src[i];

        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t e = i;
            while (e < n && src[e] != '\n') ++e;
            blank(i, e);
            i = e;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t e = i + 2;
            while (e + 1 < n && !(src[e] == '*' && src[e + 1] == '/')) ++e;
            e = std::min(e + 2, n);
            blank(i, e);
            i = e;
            continue;
        }

        // Raw strings, delimiter-bounded exactly as stringLiterals() bounds them. Only sandbox/ has
        // any (ClusterMaterialShader.hpp holds HLSL that way), but an unrecognised R"(...)" would
        // leak its whole body into the code stream, so it is handled rather than hoped about.
        if (c == 'R' && i + 1 < n && src[i + 1] == '"') {
            const size_t open = src.find('(', i + 2);
            if (open != std::string::npos && open - (i + 2) <= 16) {
                const std::string close = ")" + src.substr(i + 2, open - (i + 2)) + "\"";
                const size_t end = src.find(close, open);
                if (end != std::string::npos) {
                    const size_t e = end + close.size();
                    blank(i, e);
                    i = e;
                    continue;
                }
            }
        }

        if (c == '"' || c == '\'') {
            const char quote = c;
            size_t e = i + 1;
            while (e < n && src[e] != quote) e += (src[e] == '\\' && e + 1 < n) ? 2 : 1;
            e = std::min(e + 1, n);
            blank(i, e);
            i = e;
            continue;
        }

        out[i] = c;
        ++i;
    }
    return out;
}

static bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// '\r' is in here on purpose. This repository is checked out with core.autocrlf=true -- .gitattributes
// pins only the byte-compared format fixtures to LF, and says why -- so the character before a
// newline in every file this scans is a carriage return. A token scan that did not skip it would
// find no preceding token on any line-wrapped declaration and read every one of them as a call.
static bool isSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The identifier immediately before `pos` (with `at` set to where it starts), or "" when the nearest
// non-space character is punctuation. This is the entire discriminator between a declaration and a
// use: something being DECLARED is introduced by a preceding name -- its return type, its element
// type, `auto`, or a struct/class tag -- while something being CALLED or read sits after punctuation
// (`=`, `(`, `,`, `::`, `[`, `.`) or after a keyword that opens an expression.
//
// `at` is not bookkeeping: the caller needs it to notice when the token it found belongs to a
// PREPROCESSOR line rather than to the statement being read. GameRender.cpp:194 is the shape that
// makes this real -- an `#endif`, a comment block, then a statement -- and with the comment blanked
// the nearest preceding token is the word `endif`, which reads as a perfectly good return type.
static std::string identBefore(const std::string& code, size_t pos, size_t& at) {
    size_t i = pos;
    while (i > 0 && isSpaceChar(code[i - 1])) --i;
    if (i == 0 || !isIdentChar(code[i - 1])) { at = pos; return {}; }
    const size_t end = i;
    while (i > 0 && isIdentChar(code[i - 1])) --i;
    at = i;
    return code.substr(i, end - i);
}

static char charAt(const std::string& code, size_t pos) {
    return pos < code.size() ? code[pos] : '\0';
}

static size_t skipSpace(const std::string& code, size_t pos) {
    while (pos < code.size() && isSpaceChar(code[pos])) ++pos;
    return pos;
}

// Keywords that can stand immediately before a CALL, which is the one way a call can wear a
// declaration's shape. `return kMaxPlannedDraws;` is the case that makes this list necessary rather
// than decorative: a bare name, a bare preceding identifier, and a semicolon after it.
//
// TYPE keywords are deliberately absent. `bool occlusionTestShouldRun(...)` is precisely the shape
// being hunted -- three of the header's four functions return a type spelled as an identifier
// (u32, SurfaceLook, RouteDecision) but the fourth returns `bool`, and if `bool` were treated as an
// excuse that one function could be copied back in unnoticed.
static bool opensAnExpression(const std::string& tok) {
    static const char* kWords[] = {
        "return", "co_return", "co_await", "co_yield", "if", "else", "while", "do", "for",
        "switch", "case", "throw", "new", "delete", "sizeof", "alignof", "typeid", "noexcept",
        "and", "or", "not", "xor",
    };
    for (const char* w : kWords)
        if (tok == w) return true;
    return false;
}

// True when `pos` sits on a preprocessor line. Those need their own rule, because the ordinary one
// cannot tell `#define kMaxPlannedDraws 64` -- a second statement of the constant, which must fail --
// from `#define SOMETHING chooseRoute(a, b)`, a macro that merely calls it, which must not: in both
// the preceding token is a bare identifier.
static bool onDirectiveLine(const std::string& code, size_t pos) {
    size_t bol = code.rfind('\n', pos);
    bol = (bol == std::string::npos) ? 0 : bol + 1;
    while (bol < pos && (code[bol] == ' ' || code[bol] == '\t')) ++bol;
    return charAt(code, bol) == '#';
}

// Byte offsets in `code` -- which must already have been through codeOnly() -- where `name` is
// DECLARED rather than called, read, or merely named.
//
// NARROW ON BOTH SIDES, ON PURPOSE. Every shape below is one the deleted copy actually had, and
// several shapes that would also be a redeclaration are let through rather than risk a false alarm:
// the elaborated-type use `struct PlannedDraw pd;` (a use, not a declaration, and indistinguishable
// without parsing), a typedef of an anonymous struct, and a function returning by pointer or
// reference (`PlannedDraw* planEntityDraws(...)`, where the preceding token is `*` and not a name).
// A check that cries wolf gets deleted, and a deleted check guards nothing at all; a check that
// catches the copy the way the copy was actually written keeps working.
static std::vector<size_t> declarationsOf(const std::string& code, const std::string& name,
                                          bool isType) {
    std::vector<size_t> out;
    for (size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + 1)) {
        // Whole-word only. Without this, every kMaxPlannedDraws would also report a PlannedDraw,
        // since the constant spells the type inside its own name.
        if (pos > 0 && isIdentChar(code[pos - 1])) continue;
        const size_t after = pos + name.size();
        if (after < code.size() && isIdentChar(code[after])) continue;

        size_t prevAt = 0;
        const std::string prev = identBefore(code, pos, prevAt);
        const size_t nextAt = skipSpace(code, after);
        const char next = charAt(code, nextAt);
        const char next2 = charAt(code, nextAt + 1);

        if (onDirectiveLine(code, pos)) {
            if (prev == "define") out.push_back(pos);
            continue;
        }
        // The preceding token is only a type if it belongs to the same statement. A token left over
        // from a `#endif` or `#include` line above is not one, and saying so here rather than in
        // identBefore() keeps the `#define` rule immediately above working, where the name and the
        // word `define` are on the directive line together and that is exactly the point.
        if (!prev.empty() && onDirectiveLine(code, prevAt)) continue;

        if (isType) {
            // A tag plus a body, a base list, or a forward declaration. `const aver::game::PlannedDraw&`
            // and `aver::game::PlannedDraw pdraws[...]` -- which is how both hosts write every real
            // use today -- carry no tag and are never reached.
            if ((prev == "struct" || prev == "class" || prev == "union") &&
                (next == '{' || next == ':' || next == ';')) {
                out.push_back(pos);
            } else if (prev == "using" && next == '=') {
                out.push_back(pos);  // an alias is a restatement too, however thin
            }
            continue;
        }

        // A preceding name that is not an expression keyword is a type, so this occurrence is being
        // introduced rather than used. The follower narrows it to the four ways something can be
        // introduced: a parameter list, a braced initialiser, an initialiser, or a bare declarator.
        // `==` is excluded explicitly so that `kMaxPlannedDraws == n` cannot read as an assignment.
        if (prev.empty() || opensAnExpression(prev)) continue;
        if (next == '(' || next == '{' || next == ';' || (next == '=' && next2 != '='))
            out.push_back(pos);
    }
    return out;
}

static size_t lineOf(const std::string& code, size_t pos) {
    size_t line = 1;
    for (size_t i = 0; i < pos && i < code.size(); ++i)
        if (code[i] == '\n') ++line;
    return line;
}

// The ORIGINAL line containing `pos`, trimmed and clipped -- so a failure quotes what is actually
// written there rather than the blanked-out version the matcher saw. codeOnly() preserves length and
// newline positions exactly, which is what makes an offset taken from one usable in the other.
static std::string lineTextAt(const std::string& src, size_t pos) {
    size_t b = src.rfind('\n', pos);
    b = (b == std::string::npos) ? 0 : b + 1;
    size_t e = src.find('\n', pos);
    if (e == std::string::npos) e = src.size();
    while (b < e && isSpaceChar(src[b])) ++b;
    while (e > b && isSpaceChar(src[e - 1])) --e;
    return src.substr(b, std::min<size_t>(e - b, 100));
}

int main() {
#ifndef AVER_REPO_ROOT
    AVER_INFO("SeparationTest: SKIP -- built without AVER_REPO_ROOT");
    return 0;
#else
    const fs::path root = AVER_REPO_ROOT;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        AVER_INFO("SeparationTest: SKIP -- {} is not a directory (packaged build?)", root.string());
        return 0;
    }
    AVER_INFO("scanning {}", root.string());
    const std::vector<std::string> files = walk(root);
    AVER_INFO("  {} files considered", files.size());

    // ---- the engine tree holds no game content ----
    AVER_INFO("the engine tree holds no game content outside its sanctioned roots");
    {
        std::vector<std::string> stray;
        int sanctioned = 0;
        for (const std::string& rel : files) {
            if (!isGameContent(rel)) continue;
            if (inSanctionedContentRoot(rel)) { ++sanctioned; continue; }
            stray.push_back(rel);
        }
        for (const std::string& s : stray)
            AVER_ERROR("    game content outside templates/, test-content/, content/: {}", s);
        check(stray.empty(), "no game asset sits outside a sanctioned content root");
        check(sanctioned > 0, "the sanctioned roots were actually found and scanned "
                              "(a walk that reached nothing would pass the check above vacuously)");
    }

    // A project manifest is the strongest form of the same defect: it does not merely put content in
    // the tree, it puts a PROJECT there. Only test-content/ has them, and only because the format
    // suites need a real manifest to parse.
    AVER_INFO("no project manifest lives in the engine tree outside the test fixtures");
    {
        std::vector<std::string> stray;
        int fixtures = 0;
        for (const std::string& rel : files) {
            if (!endsWithLower(rel, ".ocproject")) continue;
            if (startsWith(rel, "test-content/")) { ++fixtures; continue; }
            stray.push_back(rel);
        }
        for (const std::string& s : stray)
            AVER_ERROR("    .ocproject inside the engine tree: {}", s);
        check(stray.empty(), "no .ocproject outside test-content/");
        check(fixtures > 0, "the test-content/ manifests were found (the walk reached them)");
    }

    // ---- only one file knows where projects live ----
    AVER_INFO("only {} knows where projects live", kProjectPathOwner);
    {
        int scanned = 0, ownerAbs = 0, ownerRoot = 0;
        std::vector<std::string> badAbs, badRoot;
        for (const std::string& rel : files) {
            if (!isEngineSource(rel)) continue;
            ++scanned;
            const std::string src = readFile(root / rel);
            if (src.empty()) continue;
            const bool owner = (rel == kProjectPathOwner);
            for (const std::string& lit : stringLiterals(src)) {
                if (hasAbsolutePath(lit)) {
                    if (owner) ++ownerAbs;
                    else badAbs.push_back(rel + "  ->  " + lit.substr(0, 80));
                }
                if (lit.find("Aver Projects") != std::string::npos ||
                    lit.find("AverProjects") != std::string::npos) {
                    if (owner) ++ownerRoot;
                    else badRoot.push_back(rel + "  ->  " + lit.substr(0, 80));
                }
            }
        }
        AVER_INFO("  {} engine source files scanned", scanned);
        for (const std::string& s : badAbs)
            AVER_ERROR("    absolute path baked into engine source: {}", s);
        for (const std::string& s : badRoot)
            AVER_ERROR("    the projects root named outside {}: {}", kProjectPathOwner, s);
        // A floor, not a target. 469 files at the time of writing; the number is here so that a walk
        // which silently reaches almost nothing -- a moved root, a skip rule that matched too much --
        // fails loudly instead of passing every check below it vacuously.
        check(scanned > 300, "the engine source walk reached a plausible number of files");
        check(badAbs.empty(), "no engine source file bakes in an absolute filesystem path");
        check(badRoot.empty(), "no engine source file outside the project browser names the "
                               "projects root folder");

        // The other half of the same claim, and the half that rots silently: if ProjectBrowser stops
        // carrying the convention -- because it was moved, renamed, or refactored into a helper --
        // then the two checks above start passing for the wrong reason. They would be guarding a
        // rule about a file that no longer does the thing.
        check(ownerRoot > 0, "the project browser still carries the projects-root convention "
                             "(if this fails the checks above have gone vacuous, not green)");
        check(ownerAbs > 0, "the project browser still carries the example project path");
    }

    // ---- the shared decision header is stated once, and only in the header ----
    AVER_INFO("neither render host restates what {} owns", kSubmissionHeader);
    {
        // THE HEADER FIRST, because everything after it is a proof by ABSENCE, and an absence proves
        // nothing unless the detector can still recognise a presence. If SceneSubmission.hpp is
        // moved, renamed, or rewritten in a shape declarationsOf() stops reading as a declaration,
        // the sweep below reports no copies for the wrong reason and goes quietly green while the
        // defect it guards walks back in. Same vacuity trap the projects-root block above carries
        // its own ownerRoot/ownerAbs guards for; same answer.
        const std::string headerSrc = readFile(root / kSubmissionHeader);
        const std::string headerCode = codeOnly(headerSrc);
        std::vector<std::string> unseen;
        for (const OwnedName& sym : kOwnedNames)
            if (declarationsOf(headerCode, sym.name, sym.isType).empty()) unseen.push_back(sym.name);
        for (const std::string& s : unseen)
            AVER_ERROR("    {} no longer declares {}", kSubmissionHeader, s);
        check(!headerSrc.empty(), "the shared submission header is still where both hosts include "
                                  "it from");
        check(unseen.empty(), "the shared header still declares every name this check watches (when "
                              "this fails the sweep below proves nothing -- it would report no "
                              "copies because it can no longer recognise one)");

        // Both host directories, HEADERS INCLUDED: the constant this whole check exists over was
        // spelled SandboxApp::kMaxPlannedDraws in sandbox/src/SandboxApp.hpp before the move, so
        // sweeping only .cpp would leave the exact place the editor's half of the duplication lived
        // unwatched. tests/ is not swept, for the reason isEngineSource() leaves it out above:
        // tests/editor/src/SceneSubmissionTest.cpp declares its own stand-in part type to feed the
        // header's template, and a test re-deriving a rule in order to check it is the opposite of
        // a host quietly restating one.
        int scanned = 0;
        std::vector<std::string> restated;
        for (const std::string& rel : files) {
            if (!inRenderHost(rel)) continue;
            if (!endsWithLower(rel, ".cpp") && !endsWithLower(rel, ".hpp")) continue;
            ++scanned;
            const std::string src = readFile(root / rel);
            if (src.empty()) continue;
            const std::string code = codeOnly(src);
            for (const OwnedName& sym : kOwnedNames) {
                for (size_t pos : declarationsOf(code, sym.name, sym.isType)) {
                    restated.push_back(rel + ":" + std::to_string(lineOf(code, pos)) + "  declares " +
                                       sym.name + "  ->  " + lineTextAt(src, pos));
                }
            }
        }
        AVER_INFO("  {} render-host source files scanned", scanned);
        for (const std::string& s : restated)
            AVER_ERROR("    a render host declares a name {} owns: {}", kSubmissionHeader, s);
        // A floor, like the engine-source walk's: 114 files at the time of writing. A sweep that
        // reached almost none of them would report no copies and mean nothing by it.
        check(scanned > 50, "the render-host walk reached a plausible number of files");
        check(restated.empty(),
              "neither Runtime/src/ nor sandbox/src/ declares a name the shared submission header "
              "owns -- a second declaration is not a style complaint, it is how the editor and the "
              "shipped game came to disagree about what a culled entity draws while each read "
              "correctly on its own; include the header, never restate it");
    }

    if (g_failures == 0) { AVER_INFO("SeparationTest: ALL PASS"); return 0; }
    AVER_ERROR("SeparationTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
#endif
}
