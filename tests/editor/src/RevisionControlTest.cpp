// RevisionControlTest -- the git porcelain-v2 parser, with no git anywhere near it.
//
// WHY THIS TEST CAN EXIST AT ALL, when almost nothing about the editor can be tested:
// parsePorcelainV2 is a pure function of a byte string. No process, no repository on disk, no
// ImGui, no window, no SandboxApp -- that is the entire reason the parse was separated from the
// spawning in the first place (see RevisionControl.hpp's own top comment, which follows
// InputOwnership.hpp's). RevisionControl.cpp is deliberately NOT compiled into this target: it is
// the half that needs <windows.h> and a child process, and it decides nothing.
//
// WHAT IT IS DEFENDING. A status parser is the one piece of a revision-control panel that is both
// easy to get subtly wrong and impossible to notice being wrong. A mis-split rename record shows
// the user the WRONG FILENAME beside a change; a path truncated at its first space shows them a
// file that does not exist; an unmerged record decoded as though its XY pair were an ordinary
// staged/unstaged pair reads "UU" as "nothing happened", which is the most dangerous wrong answer
// available in a program that will later offer to discard work. None of those throws, logs, or is
// visible in a screenshot. So they are written down here instead.
//
// THE GOLDENS ARE REAL RECORDS. Field counts, mode strings and object names are shaped exactly as
// git prints them, because the parser finds the path by counting spaces -- a golden with one field
// missing would pass against a parser that counts wrong.
#include "RevisionControl.hpp"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace aver;
using namespace aver::editor;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

// Porcelain v2 with -z is a stream of NUL-TERMINATED records, so a golden cannot be an ordinary
// string literal -- the first embedded NUL would end it, and every test below would silently be
// asserting about one header line. Each argument here is one record and the terminator is appended
// for it, which also lets the goldens read the way git's own output reads.
std::string z(std::initializer_list<const char*> records) {
    std::string s;
    for (const char* r : records) {
        s += r;
        s.push_back('\0');
    }
    return s;
}

// git's empty-blob object name, used wherever a golden needs one: the parser never looks at these,
// but a record with a short one would not have git's field widths.
constexpr const char* kOid = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

// The four headers a clean `--branch` status opens with, on a branch level with its upstream.
std::string cleanHeaders() {
    return z({"# branch.oid e69de29bb2d1d6434b8b29ae775ad8c2e48c5391",
              "# branch.head main",
              "# branch.upstream origin/main",
              "# branch.ab +0 -0"});
}

// One commit in the shape kLogFormat asks git for: \x1f between fields, \x1e terminating the
// record, and the newline `tformat` adds after it.
std::string commit(const char* oid, const char* shortOid, const char* author, const char* email,
                   const char* date, const char* subject) {
    std::string s;
    s += oid;      s.push_back('\x1f');
    s += shortOid; s.push_back('\x1f');
    s += author;   s.push_back('\x1f');
    s += email;    s.push_back('\x1f');
    s += date;     s.push_back('\x1f');
    s += subject;  s.push_back('\x1e');
    s.push_back('\n');
    return s;
}

} // namespace

int main() {
    std::printf("[INFO ] === revision control ===\n");

    // ---- a clean tree ---------------------------------------------------------------------------
    {
        const std::string in = cleanHeaders();
        const RepoStatus st = parsePorcelainV2(in);
        check(st.isRepo,            "clean: a `# branch.` header is what says this is a repository");
        check(st.branch == "main",  "clean: the branch name");
        check(!st.detached,         "clean: a named branch is not detached");
        check(!st.initialCommit,    "clean: branch.oid is an object name, so there are commits");
        check(st.headOid == kOid,   "clean: HEAD's object name");
        check(st.hasUpstream && st.upstream == "origin/main", "clean: the tracking branch");
        check(st.ahead == 0 && st.behind == 0, "clean: level with its upstream");
        check(st.files.empty(),     "clean: porcelain emits no record for an unchanged path");
        check(st.clean(),           "clean: nothing to commit");
        check(!st.hasConflicts(),   "clean: no conflicts");
    }

    // ---- modified, added, deleted, untracked ----------------------------------------------------
    {
        std::string in = cleanHeaders();
        in += z({"1 .M N... 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 sandbox/src/SandboxApp.cpp",
                 "1 A. N... 000000 100644 100644 0000000000000000000000000000000000000000 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 sandbox/src/RevisionControl.hpp",
                 "1 .D N... 100644 100644 000000 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 docs/OLD.md",
                 "? Binaries/scratch.txt"});
        const RepoStatus st = parsePorcelainV2(in);

        check(st.files.size() == 4, "four records, four entries");
        if (st.files.size() == 4) {
            // `.M` -- CLEAN INDEX, DIRTY WORKTREE. The two halves are not interchangeable: a panel
            // that read the staged half here would put this file in "changes to be committed",
            // which is the opposite of the truth.
            check(st.files[0].path == "sandbox/src/SandboxApp.cpp", "modified: the path");
            check(st.files[0].staged == FileStatus::Unmodified,     "modified: the index matches HEAD");
            check(st.files[0].unstaged == FileStatus::Modified,     "modified: the worktree does not");
            check(st.files[0].x == '.' && st.files[0].y == 'M',     "modified: git's own XY pair is kept verbatim");

            // `A.` -- STAGED AND NOT TOUCHED SINCE, the mirror image of the one above.
            check(st.files[1].staged == FileStatus::Added,          "added: staged for the next commit");
            check(st.files[1].unstaged == FileStatus::Unmodified,   "added: and not modified since");

            check(st.files[2].unstaged == FileStatus::Deleted,      "deleted: gone from the worktree");
            check(st.files[2].path == "docs/OLD.md",                "deleted: the path survives the record it came from");

            check(st.files[3].untracked(),                          "untracked: git has never seen this path");
            check(st.files[3].path == "Binaries/scratch.txt",       "untracked: the path");
            // THE INDEX HOLDS NOTHING FOR AN UNTRACKED PATH, so the staged half stays Unmodified
            // rather than echoing the worktree half -- that is what makes "changes to be committed"
            // a filter on `staged != Unmodified` and not a filter with an exception in it.
            check(st.files[3].staged == FileStatus::Unmodified,     "untracked: nothing is staged, so nothing reads as staged");
            check(!st.clean(),                                      "an untracked file is not a clean tree");
        }
    }

    // ---- a rename, with its old path ------------------------------------------------------------
    {
        std::string in = cleanHeaders();
        in += z({"2 R. N... 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 R100 sandbox/src/RevisionControl.hpp",
                 "sandbox/src/GitPanel.hpp",
                 "1 .M N... 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 sandbox/src/ToolsMenu.cpp"});
        const RepoStatus st = parsePorcelainV2(in);

        // THE OLD PATH IS ITS OWN RECORD, not part of the rename's. Getting this wrong does not
        // fail loudly -- it produces one entry with the wrong name and one phantom file called
        // `sandbox/src/GitPanel.hpp` that the next record's real entry then has to fight with. The
        // trailing `1 ...` record is here precisely so the count catches that.
        check(st.files.size() == 2, "rename: the origin path is consumed, not counted as a file of its own");
        if (st.files.size() == 2) {
            check(st.files[0].staged == FileStatus::Renamed,               "rename: staged as a rename");
            check(st.files[0].path == "sandbox/src/RevisionControl.hpp",   "rename: the new path");
            check(st.files[0].oldPath == "sandbox/src/GitPanel.hpp",       "rename: the old path");
            check(st.files[0].similarity == 100,                           "rename: the R100 similarity score");
            check(!st.files[0].copied,                                     "rename: R, not C -- the original is gone");
            check(st.files[1].path == "sandbox/src/ToolsMenu.cpp",         "rename: the record after it still parses");
        }
    }

    // A COPY IS THE SAME RECORD SHAPE WITH A DIFFERENT LETTER, and the difference matters to what a
    // panel says: the origin of a copy is still on disk.
    {
        std::string in = cleanHeaders();
        in += z({"2 C. N... 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 C75 Content/Levels/Copy.oclevel",
                 "Content/Levels/Main.oclevel"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.files.size() == 1 && st.files[0].copied,          "copy: C is flagged as a copy");
        check(st.files.size() == 1 && st.files[0].similarity == 75, "copy: the C75 similarity score");
        check(st.files.size() == 1 && st.files[0].staged == FileStatus::Renamed,
              "copy: still reads as a rename, because it presents as one");
    }

    // ---- a conflict -----------------------------------------------------------------------------
    {
        std::string in = cleanHeaders();
        in += z({"u UU N... 100644 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 docs/ARCHITECTURE.md"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.files.size() == 1, "conflict: one unmerged entry");
        if (st.files.size() == 1) {
            check(st.files[0].conflicted(),                       "conflict: both sides read as conflicted");
            check(st.files[0].path == "docs/ARCHITECTURE.md",     "conflict: the path, after ten fields rather than eight");
            // WITHOUT THE `u` SPECIAL CASE this record's XY pair decodes as two unknown letters,
            // which fileStatusFromCode reads as Unmodified -- a file in the middle of a merge
            // conflict reported as having nothing wrong with it.
            check(st.files[0].staged != FileStatus::Unmodified,   "conflict: never reads as unmodified");
            check(st.files[0].x == 'U' && st.files[0].y == 'U',   "conflict: UU is kept, because DU and UD need different words");
            check(st.hasConflicts(),                              "conflict: the repository reports it");
            check(!st.clean(),                                    "conflict: not a clean tree");
        }
    }

    // ---- a detached HEAD ------------------------------------------------------------------------
    {
        const std::string in = z({"# branch.oid e69de29bb2d1d6434b8b29ae775ad8c2e48c5391",
                                  "# branch.head (detached)"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.isRepo,        "detached: still a repository");
        check(st.detached,      "detached: git's `(detached)` is recognised");
        // NOT A BRANCH NAME. Showing "(detached)" where a branch goes is how a UI ends up offering
        // to push to a branch that does not exist; git refuses parentheses in ref names, so this
        // string can never be a real one.
        check(st.branch.empty(), "detached: no branch name is invented for it");
        check(!st.hasUpstream,   "detached: no upstream, because there is no branch to track one");
    }

    // ---- ahead and behind -----------------------------------------------------------------------
    {
        const std::string in = z({"# branch.oid e69de29bb2d1d6434b8b29ae775ad8c2e48c5391",
                                  "# branch.head 0.6-main",
                                  "# branch.upstream origin/0.6-main",
                                  "# branch.ab +2 -13"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.branch == "0.6-main",  "ahead/behind: a branch name containing digits and a dash");
        check(st.ahead == 2,            "ahead/behind: two commits to push");
        // STORED AS A POSITIVE COUNT although git writes it `-13`: the field is a number of
        // commits, and a UI rendering -13 next to a down arrow draws nonsense.
        check(st.behind == 13,          "ahead/behind: thirteen to pull, as a count and not as -13");
    }
    {
        // A BRANCH THAT TRACKS NOTHING emits no branch.ab line at all, which is a different state
        // from tracking something and being level with it -- one offers a push, the other does not.
        const std::string in = z({"# branch.oid e69de29bb2d1d6434b8b29ae775ad8c2e48c5391",
                                  "# branch.head local-only"});
        const RepoStatus st = parsePorcelainV2(in);
        check(!st.hasUpstream,                 "no upstream: hasUpstream is false");
        check(st.ahead == 0 && st.behind == 0, "no upstream: and the counts stay zero");
    }

    // ---- a path with a space --------------------------------------------------------------------
    //
    // THE WHOLE REASON FOR -z. Without it git applies core.quotepath and this path arrives as
    // `"Content/Levels/Main Menu.oclevel"` -- quoted, and with any non-ASCII byte turned into an
    // octal escape -- and the parser would have to reverse a quoting scheme it does not own. With
    // -z the path is raw bytes and the only rule is "everything after the eighth space".
    {
        std::string in = cleanHeaders();
        in += z({"1 .M N... 100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 Content/Levels/Main Menu.oclevel",
                 "? Content/Textures/rock albedo.png"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.files.size() == 2, "spaces: two entries");
        if (st.files.size() == 2) {
            check(st.files[0].path == "Content/Levels/Main Menu.oclevel",
                  "spaces: a tracked path keeps everything after the eighth field");
            check(st.files[1].path == "Content/Textures/rock albedo.png",
                  "spaces: and an untracked one keeps everything after the first");
        }
    }

    // ---- ignored paths, which only appear when they were asked for ------------------------------
    {
        std::string in = cleanHeaders();
        in += z({"! build/", "! Binaries/Scripts.dll"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.files.size() == 2 && st.files[0].ignored(), "ignored: `!` records are ignored paths");
        check(st.files.size() == 2 && st.files[0].path == "build/",
              "ignored: an ignored directory keeps its trailing slash, which is how git spells one");
        // IGNORING SOMETHING IS WHAT MAKES IT NOT COUNT. A tree whose only records are ignored
        // paths is clean, or --ignored would turn every build directory into a dirty repository.
        check(st.clean(), "ignored: an ignored path does not make the tree dirty");
    }

    // ---- an initial commit ----------------------------------------------------------------------
    {
        const std::string in = z({"# branch.oid (initial)",
                                  "# branch.head main",
                                  "? README.md"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.isRepo,             "initial: `git init` with nothing committed is still a repository");
        check(st.initialCommit,      "initial: `(initial)` is recognised");
        check(st.headOid.empty(),    "initial: and is not stored as if it were an object name");
        check(st.branch == "main",   "initial: the branch exists even with no commit on it");
    }

    // ---- empty input ----------------------------------------------------------------------------
    //
    // WHAT A PROJECT OUTSIDE ANY REPOSITORY LOOKS LIKE. git writes nothing to stdout for a
    // directory that is not one, and that must read as "not under revision control" rather than as
    // a failure -- it is an ordinary way to use this editor. Nothing about it is an error, so
    // nothing here throws, and the result is simply empty.
    {
        const RepoStatus st = parsePorcelainV2(std::string_view{});
        check(!st.isRepo,       "empty: no branch header, so not a repository");
        check(st.files.empty(), "empty: no entries");
        check(st.clean(),       "empty: and nothing claims to be dirty");
        check(st.branch.empty() && !st.detached, "empty: no branch state is invented");
    }
    {
        // A SHORT OR UNRECOGNISED RECORD IS SKIPPED, NOT FATAL. Half a status is a usable panel; an
        // exception thrown at a child process's bytes is an editor that closes when git says
        // something new.
        std::string in = cleanHeaders();
        in += z({"1 .M", "x something git has not invented yet", "", "? real.txt"});
        const RepoStatus st = parsePorcelainV2(in);
        check(st.isRepo, "malformed: the headers still parsed");
        check(st.files.size() == 1 && st.files[0].path == "real.txt",
              "malformed: the good record after the bad ones is still found");
    }

    // ---- the names a panel will print -----------------------------------------------------------
    //
    // Pinned because statusName() has no other reader until the UI lands, and a switch that has
    // silently stopped covering an enumerator is the sort of thing this tree has been bitten by
    // before -- it reads as a status printed "unknown" rather than as anything that fails.
    {
        check(std::string(statusName(FileStatus::Unmodified)) == "unmodified", "name: unmodified");
        check(std::string(statusName(FileStatus::Renamed)) == "renamed",       "name: renamed");
        check(std::string(statusName(FileStatus::Conflicted)) == "conflicted", "name: conflicted");
        check(std::string(statusName(FileStatus::Ignored)) == "ignored",       "name: ignored");
    }

    // ---- the commit log -------------------------------------------------------------------------
    {
        std::string in = commit("aa21c8ce0000000000000000000000000000cafe", "aa21c8ce",
                                "Hollander-Lawn", "someone@example.com",
                                "2026-09-19T21:04:11+01:00",
                                "Things the tree declared and never read");
        // A SUBJECT CONTAINING A TAB AND A PIPE, which is why the format uses \x1f and \x1e rather
        // than any printable delimiter: both of those are ordinary text in a commit message.
        in += commit("28323d680000000000000000000000000000beef", "28323d68",
                     "Hollander-Lawn", "someone@example.com",
                     "2026-09-19T18:22:05+01:00",
                     "CLI overrides\tare one struct | both hosts read");
        const std::vector<LogEntry> log = parseGitLog(in);

        check(log.size() == 2, "log: two commits");
        if (log.size() == 2) {
            check(log[0].shortOid == "aa21c8ce",     "log: the abbreviated object name");
            check(log[0].author == "Hollander-Lawn", "log: the author");
            check(log[0].date == "2026-09-19T21:04:11+01:00",
                  "log: %aI's offset is kept verbatim, because reformatting a date is the caller's business");
            check(log[1].subject == "CLI overrides\tare one struct | both hosts read",
                  "log: a subject containing a tab and a pipe survives intact");
        }
        check(parseGitLog(std::string_view{}).empty(), "log: empty input is an empty history, not an error");
    }
    {
        // A TRUNCATED RECORD IS DROPPED, NOT HALF-FILLED. A commit shown with the previous one's
        // author is worse than a commit not shown.
        // SPLIT LITERALS, NOT ONE STRING. `"...\x1fdeadbee..."` would not compile as intended: a hex
        // escape eats every hex digit that follows it, so \x1f followed by `d` is one character
        // numbered 0x1fd. Splitting the literal ends the escape at the quote.
        std::string in = "deadbeef" "\x1f" "deadbee" "\x1f" "Someone";
        in.push_back('\x1e');
        check(parseGitLog(in).empty(), "log: a record with too few fields is dropped whole");
    }

    // ---- the read-only tripwire -------------------------------------------------------------------
    //
    // THIS IS THE COMMIT'S SCOPE, WRITTEN AS AN ASSERTION. RevisionControl.cpp spawns git in exactly
    // one place and asks this first, so a future change that lets this module discard work has to
    // edit the list these lines are about -- and break these lines doing it.
    {
        check(isReadOnlyGitSubcommand("status"),    "read-only: status");
        check(isReadOnlyGitSubcommand("log"),       "read-only: log");
        check(isReadOnlyGitSubcommand("diff"),      "read-only: diff");
        check(isReadOnlyGitSubcommand("rev-parse"), "read-only: rev-parse");

        check(!isReadOnlyGitSubcommand("checkout"), "refused: checkout overwrites the worktree");
        check(!isReadOnlyGitSubcommand("reset"),    "refused: reset can discard staged work");
        check(!isReadOnlyGitSubcommand("clean"),    "refused: clean deletes untracked files outright");
        check(!isReadOnlyGitSubcommand("restore"),  "refused: restore is checkout by another name");
        check(!isReadOnlyGitSubcommand("stash"),    "refused: stash moves work somewhere the user must find again");
        check(!isReadOnlyGitSubcommand("rm"),       "refused: rm deletes");
        check(!isReadOnlyGitSubcommand("commit"),   "refused: commit writes history");
        check(!isReadOnlyGitSubcommand("add"),      "refused: staging belongs to the commit that adds a confirmation");
        check(!isReadOnlyGitSubcommand("push"),     "refused: push contacts a remote, which is where credentials would enter");
        check(!isReadOnlyGitSubcommand("pull"),     "refused: pull is a fetch and a merge, and a merge can conflict");
        check(!isReadOnlyGitSubcommand("fetch"),    "refused: fetch contacts a remote");
        check(!isReadOnlyGitSubcommand("config"),   "refused: config rewrites the user's settings");
        check(!isReadOnlyGitSubcommand("branch"),   "refused: branch -d deletes one");
        check(!isReadOnlyGitSubcommand(""),         "refused: an empty subcommand is not on the list either");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
