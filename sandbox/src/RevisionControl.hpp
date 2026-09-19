// WHAT GIT SAYS ABOUT THE OPEN PROJECT, and nothing about how it was asked.
//
// A PURE HEADER, DELIBERATELY, following sandbox/src/InputOwnership.hpp and
// Runtime/include/aver/game/SceneSubmission.hpp line for line -- both say so in their own top
// comments. No ImGui types, no SandboxApp state, no globals, no <windows.h>, no process anywhere
// near it: plain text in, plain values out, over aver/core/Types.hpp and the standard library. That
// is what makes the whole of this file a headless unit test (tests/editor/src/RevisionControlTest.
// cpp, which links Aver.Core alone) in a codebase where almost nothing about the editor can be
// tested at all. Every CreateProcessW, every command line and every log line lives in
// RevisionControl.cpp; only the DECISIONS -- what git's bytes mean -- live here.
//
// WHY THE PARSE IS THE PART WORTH PINNING. A status parser is the one piece of a revision-control
// panel that is both easy to get subtly wrong and impossible to notice being wrong: a mis-split
// rename shows the user the WRONG FILENAME next to a change, and a path with a space silently
// truncated shows them a file that does not exist. Neither throws, neither logs, and neither is
// visible from a screenshot. So the goldens below are the contract, and the spawning half is
// deliberately the thin part.
//
// WHY PORCELAIN v2 WITH -z, and not the v1 short format everybody's first Git panel parses:
//   * v2 IS DOCUMENTED AS STABLE FOR MACHINE READING. `--porcelain=v1`'s short format is stable
//     too, but v2 is the one git-status(1) offers to tools that want more than two letters, and it
//     is versioned -- a future v3 is a new number, not a changed meaning of these records.
//   * IT CARRIES RENAME INFORMATION. v1 gives `R  new -> old` only with rename detection on and
//     with the arrow embedded in the path text, which is ambiguous the moment a filename contains
//     " -> ". v2 puts the old path in its own field with a similarity score beside it.
//   * -z MEANS core.quotepath CAN NEVER MANGLE A PATH. Without it git re-encodes any path that is
//     not plain ASCII -- spaces get the whole path wrapped in double quotes, non-ASCII bytes become
//     \303\251 octal escapes -- and a parser then has to reverse a quoting scheme it does not own,
//     for exactly the paths (accented, spaced, CJK) where being wrong is worst. With -z the paths
//     arrive as raw bytes between NULs and there is nothing to un-escape.
//   * IT ALSO SETTLES THE LINE-ENDING QUESTION. NUL-separated records cannot be confused by a CRLF
//     from a Windows git, which a line-oriented reader here would have had to strip by hand.
//
// SAFETY, WHICH IS A PROPERTY OF THIS FILE'S VOCABULARY AND NOT OF A COMMENT: nothing in this
// header or in RevisionControl.cpp can discard work. There is no commit, no checkout, no reset, no
// clean, no stash, no revert and no push -- the entire surface is status, log, diff and rev-parse,
// and isReadOnlyGitSubcommand() below is the tripwire that keeps it that way at the one place a
// process is actually spawned. When staging and committing arrive, they arrive as their OWN entry
// points with their own confirmation, and they do NOT get added to that list; see its comment.
#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::editor {

// What happened to one path, decoded from one half of porcelain v2's XY pair.
//
// TWO CODES GIT HAS THAT THIS DOES NOT, both folded deliberately rather than dropped: 'T'
// (typechange -- a file became a symlink, or the reverse) reads as Modified, because every caller
// that cares asks "is this different from HEAD", and 'C' (copied) reads as Renamed with
// FileEntry::copied set, because a copy and a rename present identically in a panel -- both have an
// origin path to show -- and differ only in whether the origin survived.
enum class FileStatus : u8 {
    Unmodified,
    Modified,
    Added,
    Deleted,
    Renamed,
    Untracked,
    Conflicted,
    Ignored,
};

// Human-readable, for a log line or a tooltip. Not for round-tripping: parse from git's bytes, not
// from these.
inline const char* statusName(FileStatus s) {
    switch (s) {
        case FileStatus::Unmodified: return "unmodified";
        case FileStatus::Modified:   return "modified";
        case FileStatus::Added:      return "added";
        case FileStatus::Deleted:    return "deleted";
        case FileStatus::Renamed:    return "renamed";
        case FileStatus::Untracked:  return "untracked";
        case FileStatus::Conflicted: return "conflicted";
        case FileStatus::Ignored:    return "ignored";
    }
    return "unknown";
}

// One path git had something to say about. Porcelain emits nothing at all for a path that is
// identical in HEAD, the index and the worktree, so every entry here is a change of some kind.
struct FileEntry {
    // REPO-RELATIVE AND SLASH-SEPARATED, as git prints it, on Windows too. It is NOT a path the
    // filesystem will accept unmodified -- the caller joins it onto the repository root, which is
    // also why nothing here touches std::filesystem.
    std::string path;

    // Where a rename or a copy came from. Empty for everything else. This is the field v1's
    // `old -> new` text cannot give safely, and the reason for -z.
    std::string oldPath;

    // THE DECODED ANSWER, one per side of the pair: `staged` is HEAD vs the index, `unstaged` is
    // the index vs the worktree. Both matter and neither subsumes the other -- a file can be staged
    // as Added and then modified again, which is one entry with two different answers, and a panel
    // that keeps only one of them cannot draw git's two lists.
    //
    // FOR AN UNTRACKED OR IGNORED PATH ONLY `unstaged` IS MEANINGFUL, and `staged` stays Unmodified
    // rather than being made to echo it: the index holds nothing whatsoever for such a path, so a
    // caller building "changes to be committed" by filtering `staged != Unmodified` gets the right
    // list by construction instead of having to know to exclude these.
    FileStatus staged = FileStatus::Unmodified;
    FileStatus unstaged = FileStatus::Unmodified;

    // GIT'S OWN TWO-LETTER SPELLING, kept verbatim ('.' for unmodified, and '?'/'!' for the
    // untracked and ignored records, which is how the v1 short format writes them). It is here
    // because FileStatus::Conflicted cannot distinguish "both modified" (UU) from "deleted by us"
    // (DU), and those need different words in front of a user about to lose an edit.
    char x = '.';
    char y = '.';

    // Rename/copy similarity, 0-100, straight out of the R<score>/C<score> field. 0 when this is
    // neither.
    u8 similarity = 0;
    // True when git said C rather than R: the origin is still there.
    bool copied = false;

    bool conflicted() const { return staged == FileStatus::Conflicted; }
    bool untracked() const { return unstaged == FileStatus::Untracked; }
    bool ignored() const { return unstaged == FileStatus::Ignored; }
};

// Everything one `git status --porcelain=v2 --branch -z` said.
struct RepoStatus {
    // FALSE IS A NORMAL ANSWER, NOT A FAILURE. A project living outside any git repository is an
    // ordinary way to use this editor, and the whole panel's job in that state is to say "not under
    // revision control" quietly. It is set from the presence of a `# branch.` header rather than
    // from an exit code, because that is the one signal that means "git understood, and this is a
    // repository": git writes nothing at all to stdout when the directory is not one.
    bool isRepo = false;

    // Empty when `detached` -- git says `(detached)` where the branch name goes, and inventing a
    // name for that state is how a UI ends up offering to push to a branch that does not exist.
    std::string branch;
    bool detached = false;

    // No commits yet: git says `(initial)` for branch.oid. The branch NAME still exists (it is
    // whatever HEAD points at), but there is nothing to diff against, which is worth knowing before
    // asking for a log.
    bool initialCommit = false;
    std::string headOid;

    // The tracking branch, and how far apart the two are. `hasUpstream` is false for a branch that
    // tracks nothing, which is different from one that tracks something and is level with it --
    // git emits no branch.ab line at all in the first case, and `+0 -0` in the second.
    std::string upstream;
    bool hasUpstream = false;
    // BOTH ARE POSITIVE COUNTS. git writes behind as `-3`; it is stored as 3, because the field is
    // a number of commits and a UI that renders a negative count beside a down-arrow draws
    // nonsense.
    i32 ahead = 0;
    i32 behind = 0;

    std::vector<FileEntry> files;

    // Nothing to commit and nothing untracked -- git's own "working tree clean". Ignored paths do
    // not count, which is what ignoring them means.
    bool clean() const {
        for (const FileEntry& f : files)
            if (!f.ignored()) return false;
        return true;
    }
    bool hasConflicts() const {
        for (const FileEntry& f : files)
            if (f.conflicted()) return true;
        return false;
    }
};

// ---- the parse ---------------------------------------------------------------------------------

// The remainder of `rec` after `n` space-separated fields, or empty when the record is short.
// Porcelain v2 puts the path LAST for exactly this reason: a path may contain spaces, so it is
// never tokenised, only taken whole as what is left.
inline std::string_view afterFields(std::string_view rec, int n) {
    usize i = 0;
    for (int f = 0; f < n; ++f) {
        const usize sp = rec.find(' ', i);
        if (sp == std::string_view::npos) return {};
        i = sp + 1;
    }
    return rec.substr(i);
}

// The `k`th space-separated field (0-based), or empty when the record is short.
inline std::string_view nthField(std::string_view rec, int k) {
    usize i = 0;
    for (int f = 0; f < k; ++f) {
        const usize sp = rec.find(' ', i);
        if (sp == std::string_view::npos) return {};
        i = sp + 1;
    }
    const usize sp = rec.find(' ', i);
    return sp == std::string_view::npos ? rec.substr(i) : rec.substr(i, sp - i);
}

// Leading decimal digits as a count. Deliberately not std::stoi: this runs on whatever a child
// process wrote, and a parser for untrusted bytes that can throw is a parser that takes the editor
// down when git prints something unexpected. A malformed field reads as 0.
inline i32 parseCount(std::string_view s) {
    i32 v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

inline FileStatus fileStatusFromCode(char c) {
    switch (c) {
        case 'M': return FileStatus::Modified;
        case 'T': return FileStatus::Modified;   // typechange; see FileStatus' own comment
        case 'A': return FileStatus::Added;
        case 'D': return FileStatus::Deleted;
        case 'R': return FileStatus::Renamed;
        case 'C': return FileStatus::Renamed;    // copy; FileEntry::copied carries the difference
        default:  return FileStatus::Unmodified; // '.', and anything a future git invents
    }
}

// Parses `git status --porcelain=v2 --branch -z`.
//
// TAKES A string_view AND NOT A const char*, because the payload is full of NUL bytes -- that is
// the entire point of -z -- and any interface that finds its own end would stop at the first path.
//
// EVERY MALFORMED RECORD IS SKIPPED RATHER THAN REJECTED. Half a status is a usable panel; a thrown
// exception or an empty result because one record was short is not. There is no git error text to
// find in here either: stdout is the only stream this ever sees (RevisionControl.cpp keeps stderr
// for the caller's `why`), so an unrecognised record means a git that learned a new record type,
// not a failure to report.
inline RepoStatus parsePorcelainV2(std::string_view text) {
    RepoStatus st;

    // Split on NUL first, because a type-2 record's origin path is the NEXT record, not part of
    // this one -- the only place this format needs to look ahead.
    std::vector<std::string_view> rec;
    for (usize start = 0; start <= text.size();) {
        const usize nul = text.find('\0', start);
        if (nul == std::string_view::npos) {
            if (start < text.size()) rec.push_back(text.substr(start));
            break;
        }
        rec.push_back(text.substr(start, nul - start));
        start = nul + 1;
    }

    for (usize k = 0; k < rec.size(); ++k) {
        const std::string_view r = rec[k];
        if (r.empty()) continue;

        if (r[0] == '#') {
            if (r.starts_with("# branch.oid ")) {
                const std::string_view v = r.substr(13);
                st.isRepo = true;
                if (v == "(initial)") st.initialCommit = true;
                else                  st.headOid = std::string(v);
            } else if (r.starts_with("# branch.head ")) {
                const std::string_view v = r.substr(14);
                st.isRepo = true;
                // `(detached)` is not a branch name and must not be shown as one. A real branch
                // literally called "(detached)" is impossible: git refuses parentheses in ref names.
                if (v == "(detached)") st.detached = true;
                else                   st.branch = std::string(v);
            } else if (r.starts_with("# branch.upstream ")) {
                st.upstream = std::string(r.substr(18));
                st.hasUpstream = true;
            } else if (r.starts_with("# branch.ab ")) {
                const std::string_view v = r.substr(12);
                const usize plus = v.find('+');
                const usize minus = v.find('-');
                if (plus != std::string_view::npos)  st.ahead  = parseCount(v.substr(plus + 1));
                if (minus != std::string_view::npos) st.behind = parseCount(v.substr(minus + 1));
            }
            continue;
        }

        FileEntry e;
        const char type = r[0];
        if (type == '1' || type == '2' || type == 'u') {
            if (r.size() < 4) continue;              // no XY pair: not a record this understands
            e.x = r[2];
            e.y = r[3];
            if (type == 'u') {
                // An unmerged path is BOTH sides at once and neither cleanly: the index holds three
                // stages of it. Decoding XY here with fileStatusFromCode would read UU as
                // "unmodified, unmodified", which is the most dangerous wrong answer available.
                e.staged = FileStatus::Conflicted;
                e.unstaged = FileStatus::Conflicted;
                e.path = std::string(afterFields(r, 10));
            } else if (type == '1') {
                e.staged = fileStatusFromCode(e.x);
                e.unstaged = fileStatusFromCode(e.y);
                e.path = std::string(afterFields(r, 8));
            } else {
                e.staged = fileStatusFromCode(e.x);
                e.unstaged = fileStatusFromCode(e.y);
                const std::string_view score = nthField(r, 8);   // "R100", "C75"
                if (!score.empty()) {
                    e.copied = score[0] == 'C';
                    e.similarity = static_cast<u8>(parseCount(score.substr(1)));
                }
                e.path = std::string(afterFields(r, 9));
                // The origin path is its own NUL-terminated record. Consuming it here (rather than
                // letting the loop see it) is what stops a rename's old path being reported as an
                // unrecognised record -- or, worse, as a file of its own.
                if (k + 1 < rec.size()) e.oldPath = std::string(rec[++k]);
            }
        } else if (type == '?') {
            e.x = '?';
            e.y = '?';
            e.unstaged = FileStatus::Untracked;
            e.path = std::string(afterFields(r, 1));
        } else if (type == '!') {
            e.x = '!';
            e.y = '!';
            e.unstaged = FileStatus::Ignored;
            e.path = std::string(afterFields(r, 1));
        } else {
            continue;
        }

        if (!e.path.empty()) st.files.push_back(std::move(e));
    }

    return st;
}

// ---- the log -----------------------------------------------------------------------------------

// One commit, as the history list needs it.
struct LogEntry {
    std::string oid;        // full 40-hex, for anything that has to name this commit to git again
    std::string shortOid;   // what a UI shows
    std::string author;
    std::string email;
    std::string date;       // strict ISO-8601 with offset, straight from %aI -- NOT reformatted here
    std::string subject;    // the first line of the message, and only that
};

// THE FORMAT THE PARSER BELOW EXPECTS, defined next to it so the two cannot drift -- the failure
// mode otherwise is silent and total, since a re-ordered format string still parses, just into the
// wrong fields.
//
// \x1f (unit separator) BETWEEN FIELDS AND \x1e (record separator) BETWEEN COMMITS, rather than any
// printable delimiter: an author name may contain anything, and a subject line certainly may --
// tabs, pipes, quotes and the arrow that makes v1's rename format ambiguous are all ordinary text
// in a commit message. These two bytes are the ones git itself offers for the purpose, and a commit
// message containing them is pathological in a way a filename containing a space is not.
inline constexpr const char* kLogFormat = "%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s%x1e";

// Parses the output of `git log --pretty=tformat:kLogFormat`. Pure, for parsePorcelainV2's reason.
//
// Records are trimmed of surrounding whitespace because `tformat` puts a newline after each
// terminator; a short record is skipped rather than half-filled, so a truncated stream cannot
// produce a commit with somebody else's author on it.
inline std::vector<LogEntry> parseGitLog(std::string_view text) {
    std::vector<LogEntry> out;
    usize start = 0;
    while (start < text.size()) {
        const usize rs = text.find('\x1e', start);
        std::string_view r = text.substr(start, rs == std::string_view::npos ? std::string_view::npos
                                                                             : rs - start);
        start = (rs == std::string_view::npos) ? text.size() : rs + 1;

        while (!r.empty() && (r.front() == '\n' || r.front() == '\r')) r.remove_prefix(1);
        while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.remove_suffix(1);
        if (r.empty()) continue;

        std::string_view field[6];
        int n = 0;
        usize i = 0;
        for (; n < 6; ++n) {
            const usize us = r.find('\x1f', i);
            if (us == std::string_view::npos) { field[n] = r.substr(i); ++n; break; }
            field[n] = r.substr(i, us - i);
            i = us + 1;
        }
        if (n < 6) continue;

        LogEntry e;
        e.oid = std::string(field[0]);
        e.shortOid = std::string(field[1]);
        e.author = std::string(field[2]);
        e.email = std::string(field[3]);
        e.date = std::string(field[4]);
        e.subject = std::string(field[5]);
        out.push_back(std::move(e));
    }
    return out;
}

// ---- what may be run at all ---------------------------------------------------------------------

// THE TRIPWIRE. RevisionControl.cpp spawns git in exactly one place, and that place asks this first.
//
// IT IS NOT A SECURITY BOUNDARY -- nothing stops a future call site writing its own CreateProcessW,
// and this header could not stop it if it tried. It is a statement of scope that a compiler can be
// pointed at: this commit reads a repository and cannot alter one, and a change that breaks that
// has to edit THIS LIST, in a diff where that edit is the most visible line.
//
// WHEN STAGING AND COMMITTING ARRIVE, THEY DO NOT GO IN HERE. `add`, `commit`, `checkout`,
// `restore`, `reset`, `clean`, `stash`, `revert`, `rm`, `push` and `pull` belong behind their own
// entry point with its own confirmation -- the user must have said yes to the specific thing that
// discards work before the process starts. Widening this list instead would make every one of them
// reachable from every existing call site at once, which is precisely the mistake the list exists
// to make hard.
//
// AUTHENTICATION IS NOT THIS FILE'S BUSINESS EITHER, and the list is what keeps that true: nothing
// on it talks to a remote, so nothing here can ever be handed a credential to hold. When a remote
// operation does arrive it runs as git's own child with git's own credential helper, and what the
// user sees when that fails is git's error text, verbatim.
inline bool isReadOnlyGitSubcommand(std::string_view sub) {
    // `status` does refresh the index on disk; that is a cache write, not a change to any tracked
    // content, and excluding it would leave no way to ask the question this file exists to ask.
    return sub == "status" || sub == "log" || sub == "diff" || sub == "rev-parse" ||
           sub == "show" || sub == "ls-files" || sub == "cat-file" || sub == "blame" ||
           sub == "version";
}

// ---- the I/O half, defined in RevisionControl.cpp ------------------------------------------------
//
// DECLARED HERE AND NOWHERE ELSE because they are the same concept, and DEFINED THERE because the
// definitions need <windows.h> and a child process -- which is exactly what RevisionControlTest
// must not link to run. The test includes this header and compiles none of that.

// Which of a path's two diffs to fetch.
enum class DiffSide : u8 {
    Worktree,   // the index vs the file on disk -- `git diff`
    Index,      // HEAD vs the index -- `git diff --cached`
};

// The repository containing `projectDirectory`, via `git rev-parse --show-toplevel`.
//
// THE PROJECT'S REPOSITORY, NEVER THE ENGINE'S. The editor's own tree is irrelevant here and
// assuming it is the answer is how a panel ends up offering to commit the user's game content into
// Aver Engine's history. A project nested anywhere -- inside another repository, on another drive,
// in a submodule -- gets whatever git says from where the project actually lives.
//
// EMPTY IS THE NORMAL "not under revision control" ANSWER, and leaves `*why` untouched. `*why`
// (when non-null) is set only when git could not be asked at all: not installed, not on PATH.
std::string gitRepositoryRoot(const std::string& projectDirectory, std::string* why);

// `git status --porcelain=v2 --branch -z` in `repositoryRoot`, parsed.
//
// A result with isRepo == false means the question could not be answered -- either git did not run
// (with `*why` set) or the directory is not a repository (with `*why` empty).
RepoStatus gitStatus(const std::string& repositoryRoot, bool includeIgnored, std::string* why);

// The last `maxCount` commits, newest first. `path` restricts the history to one file when it is
// non-empty; it is repo-relative, in git's spelling, exactly as FileEntry::path gives it.
std::vector<LogEntry> gitLog(const std::string& repositoryRoot, int maxCount,
                             const std::string& path, std::string* why);

// One path's unified diff, as text. NOT parsed into hunks: a diff is what the user reads, and
// inventing a hunk model before anything draws one would be guessing at what the viewer needs.
std::string gitDiff(const std::string& repositoryRoot, const std::string& path, DiffSide side,
                    std::string* why);

// Whether a git that can be run exists at all, for the panel's empty state. Cached after the first
// call -- the answer cannot change while the editor is open in any way worth a process per frame.
bool gitAvailable();

} // namespace aver::editor
