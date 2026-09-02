// SeparationTest -- the engine tree holds no game content, and only one file knows where projects live.
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

    if (g_failures == 0) { AVER_INFO("SeparationTest: ALL PASS"); return 0; }
    AVER_ERROR("SeparationTest: {} failure(s)", g_failures);
    return g_failures;
#endif
}
