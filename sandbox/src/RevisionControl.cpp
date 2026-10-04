// THE I/O HALF of revision control: build a git command line, run it, hand the bytes to the parser
// in RevisionControl.hpp. Every process spawn, every <windows.h> type and every log line in this
// feature is in this file and nowhere else -- that division is the whole point, and the header's own
// top comment says why. Nothing here decides what git's output MEANS.
//
// READ-ONLY, AND STRUCTURALLY SO. There is exactly one function below that starts a process
// (runGit), it asks isReadOnlyGitSubcommand() before it does, and the list that answers lives in the
// header where a reviewer reads it before any of this. This commit cannot stage, commit, checkout,
// reset, clean, stash, revert or push, and the next one must not make it able to by widening that
// list -- see the list's own comment for where those belong instead.
//
// NO CREDENTIALS, EVER. Nothing on that list contacts a remote, so nothing here is ever in a
// position to be handed a password or a token, to store one, or to log one. When fetching does
// arrive it runs as git's own child with git's own credential helper, and what the user sees when
// it fails is git's error text as git wrote it.
#include "RevisionControl.hpp"

#include "ProcessRun.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>

namespace aver::editor {
namespace {

// Trailing and leading whitespace off a captured stream. git terminates `rev-parse` output with a
// newline, and a newline inside a path is what turns a repository root into a directory that does
// not exist.
std::string trimmed(const std::string& s) {
    usize b = 0, e = s.size();
    const auto space = [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; };
    while (b < e && space(s[b])) ++b;
    while (e > b && space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// One argument, quoted for CreateProcessW's single command-line string.
//
// THE TRAILING-BACKSLASH RULE IS NOT DECORATION. A run of backslashes immediately before the
// closing quote must be doubled, or the last one escapes the quote and the rest of the command line
// becomes part of the argument -- which for a pathspec means git diffing something nobody named. A
// double quote inside the argument is dropped rather than escaped: Windows forbids it in a filename,
// so a path containing one did not come from a file, and refusing to pass it on is the safe reading.
std::wstring quoteArg(const std::wstring& a) {
    std::wstring q = L"\"";
    for (const wchar_t c : a) {
        if (c == L'"') continue;
        q.push_back(c);
    }
    usize backslashes = 0;
    while (backslashes < q.size() - 1 && q[q.size() - 1 - backslashes] == L'\\') ++backslashes;
    q.append(backslashes, L'\\');
    q.push_back(L'"');
    return q;
}

// THE ONE PLACE THIS FEATURE STARTS A PROCESS.
//
// `subcommand` is a separate argument rather than the head of `tail` so the allow-list check reads
// the real thing instead of re-tokenising a command line -- a check that parses its own input is a
// check with a bug in it waiting.
//
// TRUE MEANS GIT RAN, not that it liked the question: `exitCode` carries that, and the caller
// decides. The two are different answers -- "git is not installed" and "this is not a repository"
// must not reach the user as the same sentence -- which is the distinction runCaptured's own return
// value exists to preserve.
//
// STDOUT AND STDERR SHARE ONE PIPE, which is runCaptured's contract (see ProcessRun.hpp) and is
// what makes git's own error text available to put in `*why` verbatim. The cost is that a warning
// git writes on its way out lands glued to an adjacent record in the porcelain stream: -z means
// that noise carries no NUL of its own, so it corrupts at most the ONE record it touches and the
// parse walks on. gitStatus() below covers the one case where losing a single record would
// otherwise matter.
bool runGit(const std::string& workingDir, const std::string& subcommand,
            const std::wstring& leading, const std::wstring& tail,
            std::string& out, int& exitCode, std::string* why) {
    if (!isReadOnlyGitSubcommand(subcommand)) {
        // A PROGRAMMING ERROR, NOT A USER CONDITION. Reaching here means a call site asked this
        // module to do something it is documented not to do, and the loud log is the point.
        AVER_ERROR("[RevisionControl] refusing to run `git {}` -- this module reads a repository, "
                   "it does not change one", subcommand);
        if (why) *why = "internal error: `git " + subcommand + "` is not one of this module's read-only commands";
        return false;
    }

    out.clear();
    exitCode = -1;

    std::wstring cmd = L"git ";
    if (!leading.empty()) { cmd += leading; cmd += L' '; }
    cmd += widen(subcommand);
    if (!tail.empty()) { cmd += L' '; cmd += tail; }

    if (!runCaptured(cmd, widen(workingDir), out, exitCode)) {
        if (why) *why = "could not start git -- is it installed and on PATH?";
        return false;
    }
    return true;
}

} // namespace

std::string gitRepositoryRoot(const std::string& projectDirectory, std::string* why) {
    if (why) why->clear();
    if (projectDirectory.empty()) return {};

    // ASKED BEFORE SPAWNING because a missing directory would make CreateProcessW fail, and this
    // function would then report "could not start git" about a git that is installed and fine.
    std::error_code ec;
    if (!std::filesystem::is_directory(projectDirectory, ec)) return {};

    // FROM THE PROJECT'S DIRECTORY, so git walks up from where the user's content actually lives.
    // The engine's own repository is not the answer and must never be assumed to be: a project kept
    // inside a checkout of Aver Engine, or on another drive, or in a submodule, each gets its own
    // correct answer from this one question, and none of them gets the editor's.
    std::string out;
    int code = -1;
    if (!runGit(projectDirectory, "rev-parse", {}, L"--show-toplevel", out, code, why)) return {};

    // NOT A REPOSITORY IS A NORMAL STATE. git exits 128 with "fatal: not a git repository" and that
    // is not a failure to report anywhere -- a project simply may not be under revision control.
    // `*why` is deliberately left empty here so a caller cannot show it as an error.
    if (code != 0) return {};

    return trimmed(out);
}

RepoStatus gitStatus(const std::string& repositoryRoot, bool includeIgnored, std::string* why) {
    if (why) why->clear();
    RepoStatus st;
    if (repositoryRoot.empty()) return st;   // already known not to be a repository; isRepo stays false

    // --no-optional-locks: do NOT take the index lock just to refresh it. The editor asks this
    // question on a timer, and without this an ordinary `git commit` typed in a terminal at the
    // wrong moment fails because a background status held the lock. The panel's convenience must
    // not be able to break the user's actual git.
    //
    // --branch for the four `# branch.*` headers (the name, the upstream, and ahead/behind); the
    // porcelain v2 -z rationale is in RevisionControl.hpp and is the reason this string is not
    // --porcelain alone.
    //
    // UNTRACKED DIRECTORIES STAY COLLAPSED (git's default `--untracked-files=normal`): a new
    // folder of a thousand imported assets is one row saying the folder is new, not a thousand
    // rows. --untracked-files=all is what a stage-individual-files UI would need, and it can ask
    // for it when something is drawing that.
    std::wstring tail = L"--porcelain=v2 --branch -z";
    if (includeIgnored) tail += L" --ignored=matching";

    std::string out;
    int code = -1;
    if (!runGit(repositoryRoot, "status", L"--no-optional-locks", tail, out, code, why)) return st;
    if (code != 0) {
        if (why) *why = trimmed(out);
        return st;
    }

    st = parsePorcelainV2(out);
    // rev-parse ALREADY ESTABLISHED THAT THIS IS A REPOSITORY, and that is the stronger signal.
    // The parser derives isRepo from the `# branch.` headers because that is all it can see, but
    // those headers are the records nearest the front of the stream and therefore the ones a stray
    // warning on the shared stderr pipe can glue itself to. An open repository reading as "not
    // under revision control" because git mentioned a line ending is not a trade worth making.
    st.isRepo = true;
    return st;
}

std::vector<LogEntry> gitLog(const std::string& repositoryRoot, int maxCount,
                             const std::string& path, std::string* why) {
    if (why) why->clear();
    if (repositoryRoot.empty() || maxCount <= 0) return {};

    // tformat: (not format:) TERMINATES each record rather than separating them, so the last commit
    // ends the same way every other one does and the parser needs no special case for it.
    //
    // --no-color because a git configured with color.ui=always would otherwise wrap every field in
    // escape sequences, and the parser would hand a UI an author name with ANSI codes in it.
    std::wstring tail = L"--max-count=" + std::to_wstring(maxCount) +
                        L" --no-color --pretty=tformat:" + quoteArg(widen(std::string(kLogFormat)));
    // `--` BEFORE THE PATHSPEC, ALWAYS. Without it a file named like an option -- or, worse, like a
    // branch -- is read as one, and `git log` then answers a question nobody asked.
    if (!path.empty()) tail += L" -- " + quoteArg(widen(path));

    std::string out;
    int code = -1;
    if (!runGit(repositoryRoot, "log", {}, tail, out, code, why)) return {};
    if (code != 0) {
        // A REPOSITORY WITH NO COMMITS LANDS HERE, and it is not a defect: git exits non-zero with
        // "does not have any commits yet". The caller that wants to tell those apart has
        // RepoStatus::initialCommit and should check it before asking for a history at all.
        if (why) *why = trimmed(out);
        return {};
    }
    return parseGitLog(out);
}

std::string gitDiff(const std::string& repositoryRoot, const std::string& path, DiffSide side,
                    std::string* why) {
    if (why) why->clear();
    if (repositoryRoot.empty() || path.empty()) return {};

    // --no-ext-diff AND --no-textconv, BOTH DELIBERATE AND BOTH ABOUT NOT RUNNING SOMEBODY ELSE'S
    // PROGRAM. A repository's config can name an external diff command (diff.external) or a
    // per-path textconv filter, and git will happily launch either. The editor asking "what changed
    // in this file" must not be a way for a cloned repository to start a process of its choosing
    // behind the user, and a GUI diff tool popping up over the viewport would be the polite version
    // of that.
    std::wstring tail = L"--no-color --no-ext-diff --no-textconv";
    if (side == DiffSide::Index) tail += L" --cached";
    // `--` before the pathspec for gitLog's reason above, and here it is not optional: every path
    // this is called with came out of a status the user is looking at, and one of them being read
    // as a revision instead would diff something else entirely.
    tail += L" -- " + quoteArg(widen(path));

    std::string out;
    int code = -1;
    if (!runGit(repositoryRoot, "diff", {}, tail, out, code, why)) return {};
    // EXIT 0 WITH NO OUTPUT IS THE ORDINARY "nothing differs on this side" ANSWER -- a staged file
    // has no worktree diff -- so an empty string here is not an error and does not set `*why`.
    if (code != 0) {
        if (why) *why = trimmed(out);
        return {};
    }
    return out;
}

bool gitAvailable() {
    // A MAGIC STATIC: probed once, on the first caller's thread, and never again. Whether git is
    // installed cannot usefully change while the editor is open, and asking per frame would mean a
    // process per frame for a question with a constant answer.
    static const bool present = [] {
        std::string out;
        int code = -1;
        // No working directory: this asks about the machine, not about any repository.
        if (!runGit({}, "version", {}, {}, out, code, nullptr)) return false;
        return code == 0;
    }();
    return present;
}

} // namespace aver::editor
