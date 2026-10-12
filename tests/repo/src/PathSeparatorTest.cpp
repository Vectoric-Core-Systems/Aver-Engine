// PathSeparatorTest -- no code that builds on Linux or macOS joins a path with a backslash.
//
// docs/PLATFORMS.md ("Writing code that stays portable") states the rule: a path is joined with
// AVER_PATH_SEP (aver/core/Types.hpp), which is "\\" on Windows and "/" everywhere else, so a Windows
// build keeps exactly the strings it always had. A "\\" typed by hand compiles everywhere and is only
// wrong off Windows, silently: executableDir() + "\\splash.png" on Linux names a file called
// "bin\splash.png" in the build directory, and nothing fails until someone wonders where the splash
// went. That exact line arrived in sandbox/src/SandboxPlay.cpp in the same week as the port, which is
// why this is a suite and not a paragraph.
//
// WHAT COUNTS. A narrow string literal holding an escaped backslash next to a path character, that is
// an operand of a binary `+` (directly, through adjacent literals and AVER_PATH_SEP, or through
// std::string(...)). That is the shape of every path join and of every message that quotes one.
//
// WHAT DOES NOT, deliberately:
//   * Windows-only code: every module whose module.json says "platforms": ["windows"], the files in
//     kWindowsOnlyFiles below, and anything inside #if defined(_WIN32) or the #else of
//     #if !defined(_WIN32).
//   * `+=`. The project scaffolder appends .csproj XML a line at a time, and a backslash is MSBuild's
//     own convention there, which MSBuild reads on every platform.
//   * Escape sequences a writer emits ("\\u%04x" handed to snprintf): never an operand of `+`.
//   * Wide and raw literals. Wide strings are Win32 API arguments, and raw strings are HLSL.
//   * tests/, whose fixtures spell Windows paths on purpose.
//   * Comments.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;
static constexpr int kSkip = 77;  // ctest SKIP_RETURN_CODE (root CMakeLists.txt); not a pass

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- the lexer

struct Hit { int line; std::string text; };

struct Scan {
    std::vector<Hit> hits;   // backslash joins outside Windows-only code
    int pathSepJoins = 0;    // AVER_PATH_SEP tokens seen outside Windows-only code
};

static bool isIdent(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool isPathChar(char c) { return isIdent(c) || c == '<' || c == '>' || c == '%' || c == '*' || c == '.'; }
static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Does this literal body (escapes still raw) use an escaped backslash as a path separator?
static bool hasBackslashSeparator(const std::string& body) {
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\') continue;
        if (i + 1 >= body.size()) break;
        if (body[i + 1] != '\\') { ++i; continue; }   // \n, \t, \" ... : one escape, skip it
        const bool after = i + 2 < body.size() && isPathChar(body[i + 2]);
        const bool trailing = i + 2 == body.size() && i > 0 && isPathChar(body[i - 1]);
        if (after || trailing) return true;
        ++i;
    }
    return false;
}

// One #if frame: whether the branch being read is Windows-only, and whether its #else would be.
struct Frame { bool windows; bool elseWindows; };

// "_WIN32" alone, defined(_WIN32) or defined _WIN32, with nothing else in the condition.
static bool namesOnlyWin32(std::string cond) {
    std::string t;
    for (char c : cond) if (!isSpace(c) && c != '(' && c != ')') t += c;
    return t == "_WIN32" || t == "defined_WIN32";
}

static bool windowsOnly(const std::vector<Frame>& stack) {
    for (const Frame& f : stack) if (f.windows) return true;
    return false;
}

// The directive on the line starting at `at` (`at` is the '#'), with continuations joined. `end` is
// the newline, or a comment that starts on the line, which the caller then lexes as a comment.
static std::string directiveText(const std::string& src, size_t at, size_t& end) {
    std::string out;
    size_t i = at + 1;
    for (; i < src.size() && src[i] != '\n'; ++i) {
        if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') { ++i; continue; }
        if (src[i] == '/' && i + 1 < src.size() && (src[i + 1] == '/' || src[i + 1] == '*')) break;
        out += src[i];
    }
    end = i;
    return out;
}

static void applyDirective(const std::string& d, std::vector<Frame>& stack) {
    size_t p = 0;
    while (p < d.size() && isSpace(d[p])) ++p;
    size_t q = p;
    while (q < d.size() && isIdent(d[q])) ++q;
    const std::string kw = d.substr(p, q - p);
    const std::string rest = d.substr(q);
    if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
        Frame f{false, false};
        if (kw == "ifdef") f.windows = namesOnlyWin32(rest);
        else if (kw == "ifndef") f.elseWindows = namesOnlyWin32(rest);
        else {
            size_t b = 0;
            while (b < rest.size() && isSpace(rest[b])) ++b;
            if (b < rest.size() && rest[b] == '!') f.elseWindows = namesOnlyWin32(rest.substr(b + 1));
            else f.windows = namesOnlyWin32(rest);
        }
        stack.push_back(f);
    } else if (kw == "elif" || kw == "elifdef" || kw == "elifndef") {
        if (!stack.empty()) stack.back() = {false, false};   // conservatively scanned
    } else if (kw == "else") {
        if (!stack.empty()) stack.back() = {stack.back().elseWindows, false};
    } else if (kw == "endif") {
        if (!stack.empty()) stack.pop_back();
    }
}

// Past the whitespace around `i` in `code` (comments already blanked): skipBack returns where the
// token before `i` ends, skipFwd where the one after it starts.
static size_t skipBack(const std::string& code, size_t i) {
    while (i > 0 && isSpace(code[i - 1])) --i;
    return i;
}
static size_t skipFwd(const std::string& code, size_t i) {
    while (i < code.size() && isSpace(code[i])) ++i;
    return i;
}

static bool identEndsAt(const std::string& code, size_t end, const char* name) {
    const size_t n = std::char_traits<char>::length(name);
    if (end < n || code.compare(end - n, n, name) != 0) return false;
    return end == n || !isIdent(code[end - n - 1]);
}
static bool identStartsAt(const std::string& code, size_t at, const char* name) {
    const size_t n = std::char_traits<char>::length(name);
    if (code.compare(at, n, name) != 0) return false;
    return at + n >= code.size() || !isIdent(code[at + n]);
}

struct Lit { size_t begin, end; int line; bool candidate; bool windowsOnly; std::string body; };

static Scan scanSource(const std::string& src) {
    Scan scan;
    std::string code = src;          // src with comments blanked, positions kept
    std::vector<Lit> lits;
    std::vector<Frame> stack;
    int line = 1;
    bool lineStart = true;
    const size_t n = src.size();
    for (size_t i = 0; i < n;) {
        const char c = src[i];
        if (c == '\n') { ++line; lineStart = true; ++i; continue; }
        if (lineStart && isSpace(c)) { ++i; continue; }
        if (lineStart && c == '#') {
            size_t end = i;
            const std::string d = directiveText(src, i, end);
            for (size_t k = i; k < end; ++k) if (src[k] == '\n') ++line;
            applyDirective(d, stack);
            i = end;
            continue;
        }
        lineStart = false;
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') code[i++] = ' ';
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            code[i] = code[i + 1] = ' ';
            i += 2;
            while (i < n && !(src[i] == '*' && i + 1 < n && src[i + 1] == '/')) {
                if (src[i] == '\n') ++line; else code[i] = ' ';
                ++i;
            }
            if (i < n) { code[i] = code[i + 1] = ' '; i += 2; }
            continue;
        }
        if (c == '\'') {
            // A digit separator (1'000) follows a digit or letter that is not a character-literal prefix.
            const bool prefix = i > 0 && (src[i - 1] == 'L' || src[i - 1] == 'u' || src[i - 1] == 'U' || src[i - 1] == '8') &&
                                (i < 2 || !isIdent(src[i - 2]) || (src[i - 1] == '8' && src[i - 2] == 'u'));
            if (i > 0 && isIdent(src[i - 1]) && !prefix) { ++i; continue; }
            ++i;
            while (i < n && src[i] != '\'' && src[i] != '\n') i += (src[i] == '\\') ? 2 : 1;
            ++i;
            continue;
        }
        if (c == '"') {
            // The encoding prefix glued to the quote, if the identifier there is one.
            size_t p = i;
            while (p > 0 && isIdent(src[p - 1])) --p;
            std::string prefix = src.substr(p, i - p);
            static const char* kPrefixes[] = {"L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R"};
            bool known = false;
            for (const char* k : kPrefixes) known = known || prefix == k;
            if (!known) { prefix.clear(); p = i; }
            const bool raw = !prefix.empty() && prefix.back() == 'R';
            const bool wide = prefix == "L" || prefix == "u" || prefix == "U" || prefix == "LR" || prefix == "uR" || prefix == "UR";
            const int startLine = line;
            if (raw) {
                size_t d = i + 1;
                while (d < n && src[d] != '(') ++d;
                const std::string close = ")" + src.substr(i + 1, d - i - 1) + "\"";
                const size_t e = src.find(close, d);
                const size_t stop = e == std::string::npos ? n : e + close.size();
                for (size_t k = i; k < stop; ++k) if (src[k] == '\n') ++line;
                lits.push_back({p, stop, startLine, false, windowsOnly(stack), {}});
                i = stop;
                continue;
            }
            size_t j = i + 1;
            while (j < n && src[j] != '"' && src[j] != '\n') j += (src[j] == '\\') ? 2 : 1;
            const size_t stop = j < n ? j + 1 : n;
            lits.push_back({p, stop, startLine, !wide, windowsOnly(stack), src.substr(i + 1, j - i - 1)});
            i = stop;
            continue;
        }
        if (isIdent(c) && (i == 0 || !isIdent(src[i - 1]))) {
            if (identStartsAt(src, i, "AVER_PATH_SEP") && !windowsOnly(stack)) ++scan.pathSepJoins;
            while (i < n && isIdent(src[i])) ++i;
            continue;
        }
        ++i;
    }

    // A unit is a run of adjacent literals and AVER_PATH_SEP tokens: one string to the compiler.
    auto unitStart = [&](size_t li) {
        size_t b = lits[li].begin;
        for (;;) {
            const size_t e = skipBack(code, b);
            if (li > 0 && lits[li - 1].end == e) { --li; b = lits[li].begin; continue; }
            if (identEndsAt(code, e, "AVER_PATH_SEP")) { b = e - std::char_traits<char>::length("AVER_PATH_SEP"); continue; }
            return b;
        }
    };
    auto unitEnd = [&](size_t li) {
        size_t e = lits[li].end;
        for (;;) {
            const size_t s = skipFwd(code, e);
            if (li + 1 < lits.size() && lits[li + 1].begin == s) { ++li; e = lits[li].end; continue; }
            if (identStartsAt(code, s, "AVER_PATH_SEP")) { e = s + std::char_traits<char>::length("AVER_PATH_SEP"); continue; }
            return e;
        }
    };
    auto plusBefore = [&](size_t b) {
        const size_t e = skipBack(code, b);
        return e > 0 && code[e - 1] == '+' && (e < 2 || code[e - 2] != '+');
    };
    auto plusAfter = [&](size_t e) {
        const size_t s = skipFwd(code, e);
        return s < code.size() && code[s] == '+' && (s + 1 >= code.size() || (code[s + 1] != '+' && code[s + 1] != '='));
    };

    for (size_t li = 0; li < lits.size(); ++li) {
        const Lit& l = lits[li];
        if (!l.candidate || l.windowsOnly || !hasBackslashSeparator(l.body)) continue;
        size_t b = unitStart(li), e = unitEnd(li);
        // std::string("...\\") + x: look through the constructor call.
        const size_t open = skipBack(code, b), close = skipFwd(code, e);
        if (open > 0 && code[open - 1] == '(' && close < code.size() && code[close] == ')') {
            const size_t name = skipBack(code, open - 1);
            if (identEndsAt(code, name, "string")) {
                size_t s = name - 6;
                if (s >= 5 && code.compare(s - 5, 5, "std::") == 0) s -= 5;
                b = s; e = close + 1;
            }
        }
        if (!plusBefore(b) && !plusAfter(e)) continue;
        const size_t ls = src.rfind('\n', l.begin), le = src.find('\n', l.begin);
        std::string text = src.substr(ls == std::string::npos ? 0 : ls + 1,
                                      (le == std::string::npos ? n : le) - (ls == std::string::npos ? 0 : ls + 1));
        const size_t t = text.find_first_not_of(" \t");
        scan.hits.push_back({l.line, t == std::string::npos ? text : text.substr(t)});
    }
    return scan;
}

// ---------------------------------------------------------------- the tree

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A module.json whose "platforms" list names neither linux nor macos.
static bool windowsOnlyModule(const fs::path& moduleDir) {
    const std::string json = readFile(moduleDir / "module.json");
    const size_t key = json.find("\"platforms\"");
    if (key == std::string::npos) return false;
    const size_t open = json.find('[', key), close = json.find(']', key);
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    const std::string list = json.substr(open, close - open);
    return list.find("\"linux\"") == std::string::npos && list.find("\"macos\"") == std::string::npos;
}

// Built on Windows only, by a CMake condition and not by a manifest, so the walk cannot tell.
static const char* kWindowsOnlyFiles[] = {
    "tools/AverCrashReporter.cpp",   // root CMakeLists.txt: if(WIN32) ... add_executable(AverCrashReporter
};

static bool isCppSource(const fs::path& p) {
    const std::string e = p.extension().string();
    return e == ".cpp" || e == ".hpp" || e == ".h" || e == ".cc" || e == ".inl";
}

int main() {
    // ---- the detector recognises what it is meant to, first: everything below is a proof by absence
    AVER_INFO("the detector finds a backslash join, and only where it counts");
    {
        struct Case { const char* src; size_t hits; const char* what; };
        const Case cases[] = {
            {"auto p = dir + \"\\\\Saved\";", 1, "a join with a leading separator"},
            {"auto p = \"Content\\\\\" + d;", 1, "a join with a trailing separator"},
            {"auto p = std::string(\"Create Content\\\\\") + d;", 1, "a join through std::string(...)"},
            {"auto p = dir + AVER_PATH_SEP \"a\\\\b\";", 1, "a join next to AVER_PATH_SEP"},
            {"auto p = dir + AVER_PATH_SEP \"Saved\";", 0, "AVER_PATH_SEP itself"},
            {"std::snprintf(b, sizeof b, \"\\\\u%04x\", c);", 0, "an escape a writer emits"},
            {"s += \"<Compile Include=\\\"..\\\\Materials\\\\*.cs\\\" />\\n\";", 0, "+= appending MSBuild XML"},
            {"auto m = \"say \\\"hi\\\"\\n\" + x;", 0, "a message whose escapes are a quote and a newline"},
            {"// p = dir + \"\\\\Saved\";\n/* q = dir + \"\\\\Saved\"; */", 0, "a join inside a comment"},
            {"#if defined(_WIN32)\nauto p = dir + \"\\\\Saved\";\n#endif", 0, "an #if defined(_WIN32) branch"},
            {"#ifdef _WIN32\n#else\nauto p = dir + \"\\\\Saved\";\n#endif", 1, "the #else of #ifdef _WIN32"},
            {"#if !defined(_WIN32)\n#else\nauto p = dir + \"\\\\Saved\";\n#endif", 0, "the #else of #if !defined(_WIN32)"},
            {"#if defined(_WIN32) || defined(__APPLE__)\nauto p = dir + \"\\\\Saved\";\n#endif", 1, "a condition that is not Windows alone"},
            {"auto w = L\"\\\\x\" + v;", 0, "a wide literal"},
            {"auto s = R\"(a\\\\b)\" + v;", 0, "a raw literal"},
            {"int k = 1'000; auto p = dir + \"\\\\Saved\";", 1, "a join after a digit separator"},
        };
        for (const Case& c : cases) {
            const Scan s = scanSource(c.src);
            check(s.hits.size() == c.hits, std::string(c.what) + (c.hits ? " is found" : " is not counted"));
        }
    }

#ifndef AVER_REPO_ROOT
    AVER_INFO("PathSeparatorTest: SKIP -- built without AVER_REPO_ROOT");
    return kSkip;
#else
    const fs::path root = AVER_REPO_ROOT;
    std::error_code ec;
    if (!fs::is_directory(root / "modules", ec)) {
        AVER_INFO("PathSeparatorTest: SKIP -- {} is not a source tree (packaged build?)", root.string());
        return kSkip;
    }

    AVER_INFO("no code that builds off Windows joins a path with a backslash");
    int scanned = 0, joins = 0, windowsModules = 0;
    std::vector<std::string> found;
    for (const char* f : kWindowsOnlyFiles)
        check(fs::is_regular_file(root / f, ec), std::string(f) + " is still there to be skipped");
    for (const char* top : {"modules", "sandbox", "Runtime", "tools"}) {
        fs::recursive_directory_iterator it(root / top, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                const std::string name = p.filename().string();
                const bool vendored = name == "third_party" || (name == "Jolt" && p.parent_path().filename() == "physics.jolt");
                const bool winModule = p.parent_path() == root / "modules" && windowsOnlyModule(p);
                if (winModule) ++windowsModules;
                if (vendored || winModule) it.disable_recursion_pending();
                continue;
            }
            if (!isCppSource(p)) continue;
            const std::string rel = fs::relative(p, root, ec).generic_string();
            bool skip = false;
            for (const char* f : kWindowsOnlyFiles) skip = skip || rel == f;
            if (skip) continue;
            ++scanned;
            const Scan s = scanSource(readFile(p));
            joins += s.pathSepJoins;
            for (const Hit& h : s.hits) found.push_back(rel + ":" + std::to_string(h.line) + ": " + h.text);
        }
    }
    AVER_INFO("  {} source files scanned, {} Windows-only modules skipped, {} AVER_PATH_SEP joins seen",
              scanned, windowsModules, joins);
    for (const std::string& f : found) AVER_ERROR("    {}", f);
    // Floors, not targets: a walk that silently reached almost nothing would pass the check below.
    check(scanned > 500, "the walk reached a plausible number of source files");
    check(windowsModules >= 3, "the Windows-only modules were recognised from their module.json");
    check(joins > 100, "the AVER_PATH_SEP joins the tree is known to have were seen");
    check(found.empty(), "no backslash join outside Windows-only code (use AVER_PATH_SEP, aver/core/Types.hpp)");

    if (g_failures == 0) { AVER_INFO("PathSeparatorTest: ALL PASS"); return 0; }
    AVER_ERROR("PathSeparatorTest: {} failure(s)", g_failures);
    return exitCode(ExitCode::Failed);
#endif
}
