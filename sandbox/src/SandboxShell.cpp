// Editor: the menu bar and toolbar (buildUI), modal prompts, the output log and console, the drawer and notifications, reference/profiler/nav panels.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_SYNAPSE
// ---- The GPU profiler, as a panel rather than as scrolling text -----------------------------
//
// D3D12Device has kept a full hierarchical timestamp profiler for a long time and its ONLY
// consumer was the interactive `frametime` console command -- so a per-pass tree existed and
// could only be read as a wall of text that scrolled away. GpuTimingReport is already
// structural (label, inclusive ms, parent index), so this is genuinely a view over data that
// was there: no new instrumentation, no new cost.
//
// EXCLUSIVE TIME IS DERIVED HERE, not reported: the nodes carry INCLUSIVE ms and a parent
// index, and "how much of this pass is not its children" is the number that actually points at
// what to optimise. Deriving it in the view keeps the ABI carrying one number per node.
// The References panel: which files name the asset you asked about.
//
// WHY IT IS A PANEL AND NOT ONLY A MODAL LINE. The scan has existed for a while, and could be
// reached from exactly two places: the delete confirm and the rename confirm. So the one moment
// an author could ask "what uses this?" was the moment they had already decided to remove or
// rename it -- which is the wrong end of the question. This asks it on demand.
//
// A SCAN, NOT AN INDEX, and it says so. The result is a snapshot taken when you pressed the menu
// item, over the text formats only, and it can be wrong in one direction: a reference built at
// runtime in C#, or held only as a hashed ObjectId with the path nowhere on disk, cannot be seen
// from here. Reporting "found N" rather than "there are exactly N" is the honest framing and the
// panel repeats it, because a reader who takes this for a complete answer will delete something.
void SandboxApp::buildReferencesPanel() {
#if AVER_WITH_IMGUI
    if (!showReferences_) return;
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 320.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("References", &showReferences_)) { ImGui::End(); return; }

    if (!refPanelScanned_) {
        ImGui::TextWrapped("Right-click an asset in the Content Browser and choose "
                           "\"Find References\" to see what names it.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", cbRelativeToContent(refPanelAsset_).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) refPanelResults_ = cbFindReferencesTo(refPanelAsset_);
    uiReg_.track("references.rescan");
    ImGui::Separator();

    if (refPanelResults_.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Nothing found that names this asset.");
    } else {
        ImGui::Text("%zu file(s) name it:", refPanelResults_.size());
        ImGui::Separator();
        for (const std::string& rel : refPanelResults_) {
            // DOUBLE-CLICK OPENS IT, which is what makes this a navigation surface rather than a
            // list of strings you then have to go and find by hand.
            if (ImGui::Selectable(rel.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const std::string abs = project_.contentDir() + "\\" + rel;
                cbSelectedFile_ = abs;
                assetEditors_.open(abs);
            }
        }
    }

    ImGui::Separator();
    // THE LIMIT, said every time rather than once in a comment nobody reads. See
    // cbFindReferencesTo for what it can and cannot see.
    ImGui::TextDisabled("Scanned .ocworld/.ocmap/.ocmat/.ocgraph/.ocproject for this path.");
    ImGui::TextDisabled("Cannot see: a path built in C# at runtime, or a reference stored only as");
    ImGui::TextDisabled("a hashed id. This is \"found N\", not \"there are exactly N\".");
    ImGui::End();
#endif
}

void SandboxApp::buildProfilerPanel(Engine& e) {
#if AVER_WITH_IMGUI
    if (!showProfiler_) return;
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 420.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Profiler", &showProfiler_)) { ImGui::End(); return; }

    const rhi::GpuTimingReport r = e.device()->gpuTiming();
    if (!r.supported) {
        // THE TWO "NO DATA" AXES ARE DIFFERENT QUESTIONS and the panel says which one it is
        // rather than showing an empty tree. Vulkan has no timestamp machinery at all today.
        ImGui::TextWrapped("This backend does not report GPU timings. D3D12 does; the Vulkan "
                           "backend has no timestamp machinery yet.");
        ImGui::End();
        return;
    }
    if (r.framesAccumulated == 0 || r.nodes.empty()) {
        ImGui::TextWrapped("No timings collected yet. Pass --gpu-timing, or run a few frames.");
        ImGui::End();
        return;
    }

    // Exclusive = inclusive minus the inclusive time of the direct children.
    std::vector<f64> childSum(r.nodes.size(), 0.0);
    for (usize i = 0; i < r.nodes.size(); ++i) {
        const u32 p = r.nodes[i].parent;
        if (p != rhi::GpuTimingNode::kNoParent && p < childSum.size()) childSum[p] += r.nodes[i].ms;
    }
    f64 total = 0.0;
    for (usize i = 0; i < r.nodes.size(); ++i)
        if (r.nodes[i].parent == rhi::GpuTimingNode::kNoParent) total += r.nodes[i].ms;

    ImGui::Text("%.2f ms over %u frames", total, r.framesAccumulated);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An AVERAGE since boot, not one sampled frame -- see the device's own\n"
                          "comment on why. A number here lags a change by a few frames.");
    ImGui::SameLine();
    ImGui::TextDisabled("(marked passes only)");
    ImGui::Separator();

    if (ImGui::BeginTable("##passes", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("incl ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
        ImGui::TableSetupColumn("excl ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
        ImGui::TableSetupColumn("% GPU",   ImGuiTableColumnFlags_WidthFixed, 60.0f * dpi_);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // ONE PASS, PARENT-BEFORE-CHILD, using the depth the parent chain implies rather than a
        // recursive walk: the report is emitted in that order already, and indenting by computed
        // depth keeps this immune to a node whose parent index points forward.
        for (usize i = 0; i < r.nodes.size(); ++i) {
            const rhi::GpuTimingNode& n = r.nodes[i];
            int depth = 0;
            for (u32 p = n.parent; p != rhi::GpuTimingNode::kNoParent && depth < 8; ++depth) {
                if (p >= r.nodes.size()) break;
                p = r.nodes[p].parent;
            }
            const f64 excl = n.ms - childSum[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(depth * 14.0f * dpi_);
            ImGui::TextUnformatted(n.label.c_str());
            ImGui::Unindent(depth * 14.0f * dpi_);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", n.ms);
            ImGui::TableSetColumnIndex(2);
            // THE COLUMN WORTH READING, so it is the one that gets colour: a pass whose own
            // time dominates is where the work is, and a parent that is nearly all children is
            // just a label.
            const f64 frac = total > 0.0 ? (excl / total) : 0.0;
            if (frac > 0.20)      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%.2f", excl);
            else if (frac > 0.08) ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.40f, 1.0f), "%.2f", excl);
            else                  ImGui::Text("%.2f", excl);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", frac * 100.0);
        }
        ImGui::EndTable();
    }
    ImGui::End();
#else
    (void)e;
#endif
}

// Bakes, writes the result beside the level, and rebuilds the overlay. Returns false with a
// reason rather than throwing one away, because every way this fails is something the author
// has to act on: an empty level, an entity dropped far from the rest, physics not running.
bool SandboxApp::bakeNavigationNow(Engine& e, std::string* why) {
    editor::NavBakeSettings s;
    s.cellSizeCm = navBakeCell_;
    std::string reason;
    synapse::BakeStats st;
    if (!editor::bakeNavigation(scene::World::instance(), s, nav_, &st, &reason)) {
        AVER_WARN("[Editor] navigation bake failed: {}", reason);
        notifyOutcome(editor::NotifySeverity::Error, "Navigation bake failed", reason, true);
        if (why) *why = reason;
        nav_ = fmt::OcNavData{};
        rebuildNavOverlay(e);
        return false;
    }
    rebuildNavOverlay(e);
    showNav_ = true;   // baking something invisible is how a bake gets run twice

    // NOT WRITTEN FOR AN UNSAVED LEVEL. A .ocnav is named after its level, so a level with no
    // path has nowhere for one to go; baking it into the session and saying so beats
    // inventing a filename the author never asked for.
    if (levelPath_.empty()) {
        AVER_WARN("[Editor] navigation baked but NOT saved -- this level has no path yet; "
                  "save the level and bake again to write its .ocnav");
        notifyOutcome(editor::NotifySeverity::Warning, "Navigation baked, but not saved",
                     "This level has no path yet. Save it and bake again to write its .ocnav.");
        return true;
    }
    const std::string path = editor::navPathForLevel(levelPath_);
    std::string wwhy;
    if (!fmt::saveOcNav(path, nav_, &wwhy)) {
        AVER_WARN("[Editor] navigation bake could not be written to {}: {}", path, wwhy);
        if (why) *why = wwhy;
        notifyOutcome(editor::NotifySeverity::Error, "Could not write the navmesh", wwhy, true);
        return false;
    }
    AVER_INFO("[Editor] navigation written to {}", path);
    notifyOutcome(editor::NotifySeverity::Success, "Navigation baked",
                 std::filesystem::path(path).filename().string());
    return true;
}

#endif

// ---- Revision control: the editor-facing half ------------------------------------------------
//
// THE DIVISION, WHICH IS THE POINT AND NOT A STYLE CHOICE. RevisionControl.hpp decides what git's
// bytes MEAN and carries a headless unit test for it; RevisionControl.cpp owns every process spawn.
// This file owns every ImGui call, every log line, and the one thing neither of those can have an
// opinion about: WHEN to ask. Nothing below parses a byte of git's output, and nothing below starts
// a process on the frame thread.
//
// NOTHING HERE CAN LOSE WORK, AND THAT IS ENFORCED A LAYER DOWN rather than promised here: the only
// git this editor can run is isReadOnlyGitSubcommand()'s list (RevisionControl.hpp), which has no
// commit, checkout, restore, reset, clean, stash, revert, push or pull on it. So there is no
// Discard button, no Revert, no Sync -- not because they were left for later tidiness, but because
// a button whose verb loses work needs the user to have said yes to THAT SPECIFIC THING first.
// WHEN THOSE ARRIVE THEY HANG OFF A CONFIRMATION MODAL, and this file already has the shape to copy:
// drawLaunchRuntimePrompt below. Note what it does -- it names what is at stake ("this level has
// unsaved changes"), it offers the non-destructive way out first, it STAYS OPEN when the safe path
// fails rather than proceeding anyway, and the action is reached only through a button the user
// pressed inside it. A revert or a discard belongs behind exactly that, with its own entry point in
// RevisionControl.cpp, and NOT by widening the read-only list (see the list's own comment).
#if AVER_WITH_IMGUI
namespace {

// Which paths the diff viewer has something to show for.
//
// IT LIVES HERE, NOT IN RevisionControl.hpp, on purpose. That header's entire vocabulary is what
// git said; ".ocworld" is a fact about THIS editor's asset formats, and teaching it to the parser
// would make RevisionControlTest -- whose claim is that it links Aver.Core and nothing else -- the
// owner of the editor's format table.
//
// EVERYTHING ELSE A PROJECT HOLDS IS A BINARY CONTAINER OR AN IMAGE. .ocmesh, .ocbeam, .ocbt,
// .ocaudio and the rest are AVR1 files; a unified diff of those is a wall of escaped bytes that
// tells a reader nothing, which is why the panel says so in words instead of showing an empty pane.
bool isDiffableAsset(std::string_view path) {
    const usize dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(path.substr(dot));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ocworld" || ext == ".ocmat" || ext == ".ocgraph" || ext == ".cs";
}

// A path with its separators and case flattened, for comparing an editor path against git's.
// Windows hands this editor "C:\Users\...\Content" and git prints "C:/Users/.../Content" for the
// same directory, and either may differ in case from the other without naming a different file.
std::string flattenedPath(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

// How far back the per-file history goes. A panel, not a history browser: fifty touches is more
// than anybody scrolls in a docked pane, and `git log` over a whole repository's history is the
// one read here whose cost grows with the project's age rather than with its size.
constexpr int kRcLogCount = 50;

// How loudly a status should speak for a FOLDER that contains it. Only the ordering matters.
int folderRank(editor::FileStatus s) {
    switch (s) {
        case editor::FileStatus::Conflicted: return 3;
        case editor::FileStatus::Untracked:  return 1;
        case editor::FileStatus::Ignored:    return 0;
        default:                             return 2;   // any tracked change
    }
}

} // namespace

// Reaps whatever a worker finished, and decides whether to ask again. Once a frame, from buildUI.
void SandboxApp::revisionControlTick() {
    // THE PROJECT CHANGED UNDER THE LATCH. Everything held here describes a repository that is no
    // longer open, and badges drawn from it would mark the new project's files with the old
    // project's changes -- wrong in the one direction that matters, since a mark says "this differs
    // from what is committed".
    if (rcProjectDir_ != project_.dir) {
        rcProjectDir_ = project_.dir;
        rcRoot_.clear();
        rcRootKey_.clear();
        rcStatus_ = editor::RepoStatus{};
        rcWhy_.clear();
        rcMarks_.clear();
        rcAnswered_ = false;
        rcGitPresent_ = false;
        rcRefreshedAt_ = -1.0;
        rcSelected_.clear();
        rcSelectedDiffable_ = false;
        rcLog_.clear();
        rcDiff_.clear();
        rcLogWhy_.clear();
        rcDiffWhy_.clear();
        // The in-flight jobs are DROPPED, not cancelled: a detached worker cannot be recalled, and
        // its shared_ptr keeps its own result alive until it finishes writing and exits. Releasing
        // the handle here is what stops the answer for the old project being latched for the new
        // one -- the reaps below would otherwise take it, since the worker knows nothing about a
        // project having been closed.
        rcStatusJob_.reset();
        rcFileJob_.reset();
    }

    if (rcStatusJob_ && rcStatusJob_->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RcStatusQuery> job = rcStatusJob_;
        rcStatusJob_.reset();
        // Stale by a project switch that happened while it ran. Dropped without a word: the user
        // did nothing wrong and there is nothing to report.
        if (job->dir == project_.dir) {
            rcGitPresent_ = job->gitPresent;
            rcRoot_ = job->root;
            rcRootKey_ = flattenedPath(job->root);
            rcStatus_ = job->status;
            rcWhy_ = job->why;
            rcAnswered_ = true;
            rcRefreshedAt_ = ImGui::GetTime();

            // THE BADGE TABLE, built once per answer rather than per card per frame.
            //
            // ONE STATUS PER PATH, out of the two git reports. The worktree side wins because it is
            // what is on disk in front of the user; a path whose only difference is staged (edited,
            // added, then left alone) shows the staged answer instead, which is the only one it
            // has. A conflict outranks both -- FileEntry::conflicted() is the state where being
            // told the wrong thing costs an edit.
            rcMarks_.clear();
            rcMarks_.reserve(rcStatus_.files.size());
            for (const editor::FileEntry& f : rcStatus_.files) {
                if (f.ignored()) continue;   // not asked for, and not a change if it arrived anyway
                editor::FileStatus s = f.conflicted()  ? editor::FileStatus::Conflicted
                                     : f.unstaged != editor::FileStatus::Unmodified ? f.unstaged
                                                                                    : f.staged;
                if (s == editor::FileStatus::Unmodified) continue;
                rcMarks_.emplace_back(f.path, s);
            }
            std::sort(rcMarks_.begin(), rcMarks_.end(),
                      [](const std::pair<std::string, editor::FileStatus>& a,
                         const std::pair<std::string, editor::FileStatus>& b) {
                          return a.first < b.first;
                      });
        }
    }

    if (rcFileJob_ && rcFileJob_->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RcFileQuery> job = rcFileJob_;
        rcFileJob_.reset();
        // Latched only when it is still the row on screen: clicking a second file before the first
        // answer arrives must not put one file's history above another file's diff.
        if (job->root == rcRoot_ && job->path == rcSelected_) {
            rcLog_ = std::move(job->log);
            rcLogWhy_ = std::move(job->logWhy);
            rcDiff_ = std::move(job->diff);
            rcDiffWhy_ = std::move(job->diffWhy);
        }
    }

    // ASKED ONLY WHILE SOMETHING IS SHOWING THE ANSWER. A timer that runs whenever the editor is
    // open would spawn a git process every few seconds for the whole session to keep a panel nobody
    // has opened up to date.
    if (!showRevisionControl_ && drawer_ != Drawer::Content) return;
    // TWO ANSWERS THE TIMER MUST NOT RETRY, both settled facts rather than slow ones: git is not
    // installed, and this project is not in a repository. Neither changes while the editor watches,
    // and re-asking every few seconds would be a process spawn per tick forever. The Refresh button
    // asks again, because the user is the one who knows they just installed git or ran `git init`.
    if (rcAnswered_ && (!rcGitPresent_ || rcRoot_.empty())) return;

    constexpr f64 kRcAutoRefreshSec = 4.0;
    const f64 now = ImGui::GetTime();
    // MEASURED FROM THE LAST LATCH, not from the last start: on a repository where status takes
    // longer than the interval, starting from the launch time would queue a new refresh the moment
    // the previous one landed and leave a git running permanently.
    if (rcRefreshedAt_ < 0.0 || now - rcRefreshedAt_ >= kRcAutoRefreshSec) revisionControlRefresh(false);
}

// Hands `git status` to a worker. Returns immediately, always.
void SandboxApp::revisionControlRefresh(bool force) {
    if (rcStatusJob_) return;            // one in flight answers everyone waiting
    if (!project_.valid() || project_.dir.empty()) return;
    if (!force && rcAnswered_ && !rcGitPresent_) return;

    auto job = std::make_shared<RcStatusQuery>();
    job->dir = project_.dir;
    // THE ROOT IS RESOLVED ONCE AND THEN REUSED. rev-parse is a second process for an answer that
    // cannot change while a project stays open, so only the first refresh for a project pays for
    // it -- and Refresh re-resolves, because that is the button someone presses after `git init`.
    const std::string knownRoot = (force || rcProjectDir_ != job->dir) ? std::string() : rcRoot_;
    rcProjectDir_ = job->dir;
    rcStatusJob_ = job;

    // CAPTURES THE JOB AND ONE STRING BY VALUE, never `this` and never a reference into SandboxApp.
    // That is what makes detaching safe: nothing this thread touches can be destroyed out from
    // under it, whatever the editor does next.
    //
    // AND IT MUST NOT LOG. AVER_* on a worker reaches logSink, which writes into SandboxApp's own
    // logLines_ -- a reach back into the editor that the capture rule above exists to forbid. The
    // calls below reach exactly one AVER_* in RevisionControl.cpp, runGit's refusal, and that one
    // fires only when a call site asks for a subcommand off the read-only list: a programming
    // error, not a runtime state, and unreachable from here. Everything else comes back in the
    // job's `why` and is reported from the frame thread. An AVER_* added to this lambda would
    // quietly undo the argument.
    std::thread([job, knownRoot] {
        job->gitPresent = editor::gitAvailable();
        if (!job->gitPresent) {
            job->why = "git was not found -- is it installed and on PATH?";
        } else {
            job->root = knownRoot.empty() ? editor::gitRepositoryRoot(job->dir, &job->why) : knownRoot;
            // An EMPTY root with an empty `why` is the ordinary "this project is not under revision
            // control" answer, which is not a failure and must not be shown as one.
            if (!job->root.empty()) job->status = editor::gitStatus(job->root, false, &job->why);
        }
        job->done.store(true, std::memory_order_release);
    }).detach();
}

// Hands one path's history and diff to a worker. Same rules as the refresh above.
void SandboxApp::revisionControlSelect(const std::string& repoRelativePath) {
    rcSelected_ = repoRelativePath;
    rcLog_.clear();
    rcDiff_.clear();
    rcLogWhy_.clear();
    rcDiffWhy_.clear();
    rcSelectedDiffable_ = isDiffableAsset(repoRelativePath);
    if (rcRoot_.empty() || repoRelativePath.empty()) return;
    // A second click while the first query runs: the old job is released rather than waited on, and
    // its answer is discarded on reap because the path no longer matches.
    auto job = std::make_shared<RcFileQuery>();
    job->root = rcRoot_;
    job->path = repoRelativePath;
    job->wantDiff = rcSelectedDiffable_;
    const editor::DiffSide side = rcDiffSide_;
    // NO HISTORY TO ASK FOR before the first commit -- git exits non-zero with "does not have any
    // commits yet", which RevisionControl.cpp would hand back as an error string. RepoStatus says
    // so outright, so the panel checks instead of reporting a defect that is a normal new repo.
    const bool haveCommits = !rcStatus_.initialCommit;
    rcFileJob_ = job;

    std::thread([job, side, haveCommits] {
        if (haveCommits) job->log = editor::gitLog(job->root, kRcLogCount, job->path, &job->logWhy);
        if (!job->wantDiff) { job->done.store(true, std::memory_order_release); return; }
        std::string text = editor::gitDiff(job->root, job->path, side, &job->diffWhy);
        // Split here, on the worker, for the reason RcFileQuery::diff's own comment gives.
        usize start = 0;
        while (start <= text.size()) {
            usize nl = text.find('\n', start);
            if (nl == std::string::npos) {
                if (start < text.size()) job->diff.emplace_back(text.substr(start));
                break;
            }
            usize end = nl;
            if (end > start && text[end - 1] == '\r') --end;   // a git that writes CRLF
            job->diff.emplace_back(text.substr(start, end - start));
            start = nl + 1;
        }
        job->done.store(true, std::memory_order_release);
    }).detach();
}

// An editor path as git would name it. Empty means "git has nothing to say about this", which
// covers both "outside the repository" and "no repository known yet".
std::string SandboxApp::rcKeyFor(const std::string& absolute) const {
    if (rcRootKey_.empty() || absolute.size() <= rcRootKey_.size()) return {};
    const std::string flat = flattenedPath(absolute);
    if (flat.size() <= rcRootKey_.size()) return {};
    if (flat.compare(0, rcRootKey_.size(), rcRootKey_) != 0) return {};
    // THE SEPARATOR CHECK IS NOT PEDANTRY: without it a sibling directory whose name merely starts
    // with the root's -- "MyGame" and "MyGameOld" beside each other -- would be read as living
    // inside it, and every file under the second would be marked with the first's statuses.
    if (flat[rcRootKey_.size()] != '/') return {};

    // The ORIGINAL case is kept: git records the case the filesystem gave it, and rcMarks_ holds
    // git's spelling. Only the comparison above is case-insensitive.
    std::string key = absolute.substr(rcRootKey_.size() + 1);
    for (char& c : key) if (c == '\\') c = '/';
    return key;
}

// What to mark a Content Browser entry with.
bool SandboxApp::rcMarkFor(const std::string& absolute, bool isDir, editor::FileStatus& out) const {
    if (rcMarks_.empty()) return false;
    const std::string key = rcKeyFor(absolute);
    if (key.empty()) return false;

    const auto byPath = [](const std::pair<std::string, editor::FileStatus>& e,
                           const std::string& k) { return e.first < k; };
    if (!isDir) {
        const auto it = std::lower_bound(rcMarks_.begin(), rcMarks_.end(), key, byPath);
        if (it == rcMarks_.end() || it->first != key) return false;
        out = it->second;
        return true;
    }

    // A FOLDER'S MARK IS A SUMMARY, and the panel is where the detail lives. Saying "Modified"
    // for a folder holding one added and one deleted file is the honest reading of a single
    // corner dot; picking one of the two children's statuses to show instead would be a specific
    // claim about a specific file that the folder is not making.
    //
    // UNTRACKED ONLY WHEN NOTHING TRACKED CHANGED, so a brand-new folder of imported assets reads
    // as new rather than as edited. git collapses such a folder to ONE record ending in '/' (the
    // default --untracked-files=normal, which gitStatus asks for deliberately), and that record is
    // inside this same prefix range, so it needs no special case.
    const std::string prefix = key + "/";
    int best = 0;
    for (auto it = std::lower_bound(rcMarks_.begin(), rcMarks_.end(), prefix, byPath);
         it != rcMarks_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        const int rank = folderRank(it->second);
        if (rank > best) best = rank;
        if (best == 3) break;   // a conflict is the loudest thing a folder can say; stop looking
    }
    if (best == 0) return false;
    out = best == 3 ? editor::FileStatus::Conflicted
        : best == 1 ? editor::FileStatus::Untracked
                    : editor::FileStatus::Modified;
    return true;
}

// ONE TABLE FOR EVERY VIEW. The gallery, the list and the panel all read this, so a status cannot
// be amber in one place and green in another for the same file.
//
// COLOUR IS NEVER THE ONLY CARRIER. Every badge has a tooltip carrying statusName(), and the panel
// prints git's own two letters beside each row -- a reader who cannot separate the amber from the
// green still gets the answer in words.
ImU32 SandboxApp::rcStatusColour(editor::FileStatus s) {
    switch (s) {
        case editor::FileStatus::Modified:   return IM_COL32(230, 170,  60, 255);
        case editor::FileStatus::Added:      return IM_COL32( 98, 200, 110, 255);
        case editor::FileStatus::Deleted:    return IM_COL32(226,  92,  80, 255);
        case editor::FileStatus::Renamed:    return IM_COL32(120, 170, 240, 255);
        case editor::FileStatus::Untracked:  return IM_COL32(150, 156, 168, 255);
        case editor::FileStatus::Conflicted: return IM_COL32(240,  70, 150, 255);
        case editor::FileStatus::Ignored:    return IM_COL32(110, 114, 122, 255);
        case editor::FileStatus::Unmodified: break;
    }
    return IM_COL32(150, 156, 168, 255);
}

// Window > Revision Control: what git says about the open project, and nothing that can change it.
void SandboxApp::buildRevisionControlPanel() {
    if (!showRevisionControl_) return;
    ImGui::SetNextWindowSize(ImVec2(680.0f * dpi_, 480.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Revision Control", &showRevisionControl_)) { ImGui::End(); return; }

    if (!project_.valid()) {
        ImGui::TextWrapped("Open a project first. This panel reports on the repository the "
                           "PROJECT lives in, which is not the same tree as the editor's own.");
        ImGui::End();
        return;
    }

    const bool busy = rcStatusJob_ != nullptr;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_REFRESH " Refresh")) revisionControlRefresh(true);
    ImGui::EndDisabled();
    uiReg_.track("revisionControl.refresh");
    ImGui::SameLine();
    if (busy) {
        ImGui::TextDisabled("asking git...");
    } else if (rcRefreshedAt_ >= 0.0) {
        ImGui::TextDisabled("as of %.0fs ago", ImGui::GetTime() - rcRefreshedAt_);
    } else {
        ImGui::TextDisabled("not asked yet");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("git runs on a worker thread, never in a frame.\n"
                          "A slow or missing git costs the editor nothing.");
    ImGui::Separator();

    // THE THREE "nothing to show" STATES ARE THREE DIFFERENT SENTENCES. Collapsing them into one
    // empty list is how a panel tells somebody their work is untracked when git simply is not
    // installed.
    if (!rcAnswered_) {
        ImGui::TextWrapped("Waiting for the first answer from git.");
        ImGui::End();
        return;
    }
    if (!rcGitPresent_) {
        ImGui::TextWrapped("git was not found. Install it, or put it on PATH, then press Refresh.");
        if (!rcWhy_.empty()) ImGui::TextDisabled("%s", rcWhy_.c_str());
        ImGui::End();
        return;
    }
    if (rcRoot_.empty()) {
        ImGui::TextWrapped("'%s' is not inside a git repository.", project_.name.c_str());
        ImGui::TextDisabled("That is a normal way to use the editor. Run `git init` in the project "
                            "folder and press Refresh if you want history for it.");
        // A REAL FAILURE, when there is one. gitRepositoryRoot leaves this empty for "not a
        // repository" precisely so the two cannot be shown as the same thing.
        if (!rcWhy_.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcWhy_.c_str());
        ImGui::End();
        return;
    }

    // ---- the branch line ----
    if (rcStatus_.detached) {
        // NOT GIVEN AN INVENTED NAME. git says `(detached)` where the branch goes, and a UI that
        // fills that in tells somebody they are on a branch they are not on.
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), ICON_WARNING " detached HEAD");
        if (!rcStatus_.headOid.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("at %.10s", rcStatus_.headOid.c_str());
        }
    } else {
        ImGui::Text("%s", rcStatus_.branch.empty() ? "(no branch)" : rcStatus_.branch.c_str());
    }
    if (rcStatus_.initialCommit) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no commits yet)");
    }
    if (rcStatus_.hasUpstream) {
        ImGui::SameLine();
        // BOTH COUNTS ARE POSITIVE (RepoStatus says so outright: git writes behind as `-3` and the
        // parser stores 3), so the words carry the direction and nothing here ever renders the
        // nonsense "-3 behind". Level with the upstream is its own line rather than "0 ahead, 0
        // behind", which reads like a problem.
        if (rcStatus_.ahead == 0 && rcStatus_.behind == 0)
            ImGui::TextDisabled("= %s", rcStatus_.upstream.c_str());
        else
            ImGui::TextDisabled("%d ahead, %d behind %s", rcStatus_.ahead, rcStatus_.behind,
                                rcStatus_.upstream.c_str());
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(tracks nothing)");
    }
    ImGui::TextDisabled("%s", rcRoot_.c_str());
    if (rcStatus_.hasConflicts())
        ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.59f, 1.0f),
                           ICON_WARNING " This working tree has unresolved conflicts.");
    ImGui::Separator();

    // ---- the changed files ----
    const f32 listH = ImGui::GetContentRegionAvail().y * 0.42f;
    if (rcStatus_.clean()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Nothing changed. The working tree is clean.");
    } else if (ImGui::BeginTable("##rcFiles", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                                 ImVec2(0.0f, listH))) {
        ImGui::TableSetupColumn("XY",     ImGuiTableColumnFlags_WidthFixed, 34.0f * dpi_);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 96.0f * dpi_);
        ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const editor::FileEntry& f : rcStatus_.files) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // GIT'S OWN TWO LETTERS, VERBATIM, and this is the column that earns FileEntry keeping
            // them: FileStatus::Conflicted cannot tell "both modified" (UU) from "deleted by us"
            // (DU), and those are different sentences to put in front of somebody.
            const char xy[3] = {f.x, f.y, '\0'};
            ImGui::TextUnformatted(xy);

            ImGui::TableSetColumnIndex(1);
            const editor::FileStatus shown = f.conflicted() ? editor::FileStatus::Conflicted
                                           : f.unstaged != editor::FileStatus::Unmodified ? f.unstaged
                                                                                          : f.staged;
            ImGui::PushStyleColor(ImGuiCol_Text, rcStatusColour(shown));
            ImGui::TextUnformatted(editor::statusName(shown));
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(f.path.c_str());
            if (ImGui::Selectable(f.path.c_str(), rcSelected_ == f.path,
                                  ImGuiSelectableFlags_SpanAllColumns))
                revisionControlSelect(f.path);
            ImGui::PopID();
            // WHERE IT CAME FROM, for a rename or a copy. This is the field porcelain v1 cannot
            // give safely and the whole reason RevisionControl.hpp asks for v2 with -z.
            if (!f.oldPath.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(f.copied ? "(copied from %s, %d%%)" : "(was %s, %d%%)",
                                    f.oldPath.c_str(), static_cast<int>(f.similarity));
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (rcSelected_.empty()) {
        ImGui::TextDisabled("Pick a file above for its history and its diff.");
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(rcSelected_.c_str());

    if (ImGui::BeginTabBar("##rcDetail")) {
        if (ImGui::BeginTabItem("History")) {
            if (rcStatus_.initialCommit) {
                ImGui::TextDisabled("This repository has no commits yet, so nothing has a history.");
            } else if (rcFileJob_) {
                ImGui::TextDisabled("asking git...");
            } else if (!rcLogWhy_.empty()) {
                ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcLogWhy_.c_str());
            } else if (rcLog_.empty()) {
                // A FILE GIT HAS NEVER SEEN, which is the ordinary answer for anything untracked --
                // not a failure, and not the same thing as a log that could not be read.
                ImGui::TextDisabled("No commits touch this path yet.");
            } else if (ImGui::BeginTable("##rcLog", 3,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, 78.0f * dpi_);
                ImGui::TableSetupColumn("When",   ImGuiTableColumnFlags_WidthFixed, 96.0f * dpi_);
                ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const editor::LogEntry& c : rcLog_) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(c.shortOid.c_str());
                    ImGui::TableSetColumnIndex(1);
                    // THE DATE IS PRINTED AS GIT WROTE IT, clipped to the day. %aI is strict
                    // ISO-8601 and its first ten characters are the calendar date in every
                    // timezone git can emit -- reformatting it here would mean parsing a timestamp
                    // to display it, which is a second place for the format to be got wrong.
                    ImGui::Text("%.10s", c.date.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(c.subject.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s <%s>\n%s", c.oid.c_str(), c.author.c_str(),
                                          c.email.c_str(), c.date.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Diff")) {
            if (!rcSelectedDiffable_) {
                // SAID IN WORDS RATHER THAN SHOWN AS AN EMPTY PANE. An empty diff view and "this
                // file has no text to diff" look identical and mean opposite things.
                ImGui::TextWrapped("No diff for this file. The viewer covers the text formats "
                                   "(.ocworld, .ocmat, .ocgraph, .cs); everything else a project "
                                   "holds is a binary container or an image, and a unified diff of "
                                   "those bytes would say nothing.");
                ImGui::TextDisabled("Its status and its history above still apply.");
            } else {
                // BOTH SIDES ARE OFFERED because a path can genuinely have two different diffs at
                // once -- staged as added and then edited again -- and a viewer showing only one
                // of them silently hides half of what changed.
                //
                // THE TWO RadioButton CALLS ARE BOTH MADE, then their results combined. Writing
                // this as `if (a() || b())` would let a click on the first one short-circuit the
                // second out of the frame entirely -- an ImGui widget that is not called is not
                // drawn, so the Staged button would vanish on the frame Worktree was picked.
                int side = rcDiffSide_ == editor::DiffSide::Worktree ? 0 : 1;
                bool sideChanged = ImGui::RadioButton("Worktree", &side, 0);
                ImGui::SameLine();
                sideChanged = ImGui::RadioButton("Staged", &side, 1) || sideChanged;
                if (sideChanged) {
                    rcDiffSide_ = side == 0 ? editor::DiffSide::Worktree : editor::DiffSide::Index;
                    revisionControlSelect(rcSelected_);
                }
                ImGui::SameLine();
                ImGui::TextDisabled(side == 0 ? "(the index vs the file on disk)"
                                              : "(HEAD vs the index)");
                if (rcFileJob_) {
                    ImGui::TextDisabled("asking git...");
                } else if (!rcDiffWhy_.empty()) {
                    ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcDiffWhy_.c_str());
                } else if (rcDiff_.empty()) {
                    ImGui::TextDisabled("Nothing differs on this side.");
                } else {
                    // BeginChild's return value is NOT a gate for EndChild. It says whether the
                    // child's contents are worth submitting, not whether the child was opened, and
                    // EndChild must be called either way -- the same rule as the Begin/End above,
                    // which is why that one calls End on the failing branch too. Written as an
                    // unconditional pair so an `else if` chain cannot grow a path that skips it.
                    ImGui::BeginChild("##rcDiffText", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    // CLIPPED, not drawn whole: a diff is as long as the change is, and a
                    // thousand-line one would otherwise cost a thousand AddText calls a frame for
                    // the thirty lines on screen.
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(rcDiff_.size()));
                    while (clip.Step()) {
                        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                            const std::string& line = rcDiff_[static_cast<usize>(i)];
                            // The first character is what unified diff means by the line, with one
                            // trap: "+++"/"---" are the FILE HEADERS, not an added and a removed
                            // line, and colouring them as such paints the header green and red.
                            ImVec4 col(0.86f, 0.87f, 0.89f, 1.0f);
                            if (line.starts_with("+++") || line.starts_with("---"))
                                col = ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
                            else if (line.starts_with("@@"))
                                col = ImVec4(0.47f, 0.67f, 0.94f, 1.0f);
                            else if (!line.empty() && line[0] == '+')
                                col = ImVec4(0.38f, 0.78f, 0.43f, 1.0f);
                            else if (!line.empty() && line[0] == '-')
                                col = ImVec4(0.89f, 0.36f, 0.31f, 1.0f);
                            else if (line.starts_with("diff ") || line.starts_with("index "))
                                col = ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
                            ImGui::TextColored(col, "%s", line.c_str());
                        }
                    }
                    clip.End();
                    ImGui::EndChild();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
#endif  // AVER_WITH_IMGUI -- the whole revision-control UI half

// True when the HUD preview may draw: a tab published a rect and no session is playing.
bool SandboxApp::hudPreviewActive() const {
#if AVER_MODULE_SCRIPTING && AVER_MODULE_FRAMEWORK
    return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f &&
           aver_fw_play_state() != AVER_FW_PLAY_PLAYING;
#elif AVER_MODULE_SCRIPTING
    return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f;
#else
    return false;
#endif
}

// Published by the HUD tab each frame it draws; cleared when it does not.
void SandboxApp::setHudPreview(int index, f32 x, f32 y, f32 w, f32 h) {
    hudPreviewIndex_ = index; hudRectX_ = x; hudRectY_ = y; hudRectW_ = w; hudRectH_ = h;
}

#if AVER_WITH_IMGUI
// Draws a button with a drop-down triangle. Returns true when clicked.
bool SandboxApp::dropButton(const char* label) {
    const f32 extra = 16.0f*dpi_;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const bool clicked = ImGui::Button(label, ImVec2(ts.x + ImGui::GetStyle().FramePadding.x*2 + extra, 0));
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const f32 cx = mx.x - extra*0.5f - 2.0f*dpi_, cy = (mn.y+mx.y)*0.5f, s = 3.0f*dpi_;
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(cx-s,cy-s*0.55f), ImVec2(cx+s,cy-s*0.55f), ImVec2(cx,cy+s*0.8f), ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

#endif

// Records the outcome of an upgrade decision and restarts its time on screen.
// THROUGH A SETTER so the timer cannot be forgotten at one of the three call sites: the string
// used to be assigned and never displayed, so whether the upgrade worked was reported only to the log.
// NOW A NOTIFICATION, and the name is kept only because fourteen call sites say it.
//
// This was the editor's one transient-status channel: a string in the status bar with a
// twelve-second timer, whose severity was recovered by matching the prose prefix
// `rfind("Upgrade failed", 0) == 0`. Exactly one of its call sites began with that literal, so
// "Could not save <name>", "Could not write <file>", "Wrote <file> but could not load it back.",
// "Could not synthesise a landscape at that size." and "Open a project first." all rendered in
// the green that means success. A failure reported in the colour of a success is worse than no
// message at all: it is read, and believed.
//
// It is also misnamed for what it does -- its sites include "Saved", "Added water" and
// "Created" -- which is why folding it into the notification queue rather than keeping a second
// parallel channel is the right end for it.
void SandboxApp::setUpgradeStatus(std::string msg,
                      editor::NotifySeverity sev) {
    notifyOutcome(sev, std::move(msg), "", sev == editor::NotifySeverity::Error);
}

// May the window close? Called from inside WM_CLOSE, on the message thread -- so it decides and
// returns, and the prompt it arms is drawn by the ordinary frame loop that keeps running because
// this said no.
 bool SandboxApp::onCloseGuardThunk(void* user) {
    return static_cast<SandboxApp*>(user)->onCloseGuard();
}

bool SandboxApp::onCloseGuard() {
#if AVER_WITH_IMGUI
    // ALREADY ASKING is not a reason to ask again: a second X click while the prompt is up must
    // not stack a second prompt, and must still not close.
    if (exitPrompt_) return false;
    if (assetEditors_.anyDirty() || levelHasUnsavedEdits()) { exitPrompt_ = true; return false; }
#endif
    return true;
}

// Does the LEVEL differ from its file?
//
// TRUE WHEN THE LEVEL DIFFERS FROM WHAT IS ON DISK, which is not the same question as "has
// anything ever been edited" -- and it used to be answered with canUndo(), i.e. "is the undo
// stack non-empty".
//
// That could never become false by SAVING, because saving does not touch the undo stack. So the
// editor asked "you have unsaved changes" on close no matter how recently the level had been
// written, and the only way to be told otherwise was to undo every edit of the session. A prompt
// that cries wolf on every single exit is worse than no prompt: it trains the reflex that
// dismisses it, and one day it will have been telling the truth.
//
// NOT THE STACK'S SIZE EITHER, which is the tempting fix and is wrong here: pushEdit erases from
// the FRONT once kUndoDepth is reached, so the same depth can mean two different documents.
// A per-edit serial is stable under that trimming.
bool SandboxApp::levelHasUnsavedEdits() const { return currentEditMark() != savedEditMark_; }

// Called wherever the document and the file agree: after a successful save, and after a load or
// a New Level, both of which start from a file nothing has edited yet.
void SandboxApp::markLevelSaved() { savedEditMark_ = currentEditMark(); }

// Forces the dirty state on, for content that came from somewhere other than levelPath_.
// ~0 is a serial pushEdit can never produce, so no edit can accidentally match it.
void SandboxApp::markLevelUnsaved() { savedEditMark_ = ~0ull; }

// Exit, unless something is unsaved -- in which case ASK first.
// Every exit used to call requestExit() straight through. AssetEditorHost::anyDirty() existed for
// exactly this check and had no callers, so closing with an unsaved material silently discarded it.
//
// THE LEVEL ITSELF WAS NOT IN THAT CHECK, which is the bigger hole and the one that bites while
// building one: the level being edited in the viewport is never registered as an AssetEditor, so
// an hour of placing things and then File > Exit warned about exactly nothing.
void SandboxApp::requestExitChecked(Engine& e) {
#if AVER_WITH_IMGUI
    if (assetEditors_.anyDirty() || levelHasUnsavedEdits()) { exitPrompt_ = true; return; }
#endif
    e.requestExit();
}

// File > Save All, Ctrl+Shift+S. THE LEVEL, THE SAME WAY SAVE LEVEL DOES: saveLevelInteractive()
// already opens Save Level As for a level with no path rather than inventing one to write to, and
// already reports its own outcome -- calling it here keeps Save All and Save Level agreeing about
// what saving a level means, instead of a second copy of that rule slowly drifting from the first.
//
// EVERY DIRTY ASSET TAB is the other half, via AssetEditorHost::saveAllDirty -- until now that
// only ran from the quit prompt's "Save all and exit" button. It carries on past a failure rather
// than stopping at the first, so nine tabs that CAN be saved still get written when a tenth can't.
//
// A FAILURE IS A NOTIFICATION; A SUCCESS IS QUIET. The quit prompt can afford to leave its failure
// in the modal's own error line, because the modal is still open. Save All has no modal to write
// into, so the notification queue -- the same one every other save outcome in this file reports
// through -- is where a failure has to surface, or it is not surfaced at all.
void SandboxApp::saveAll() {
#if AVER_WITH_IMGUI
    saveLevelInteractive();

    std::string why;
    const usize failed = assetEditors_.saveAllDirty(&why);
    if (failed != 0) {
        notifyOutcome(editor::NotifySeverity::Error,
                      failed == 1 ? "1 asset could not be saved"
                                  : std::to_string(failed) + " assets could not be saved",
                      why, true);
        AVER_ERROR("[Editor] Save All: {} editor(s) could not be saved: {}", failed, why);
    }
#endif
}

// The unsaved-changes modal. Names the files, because "you have unsaved changes" is not
// something a user can act on.
// Help > About. Deliberately short: what this build is, what it is drawing with, and where the
// preferences it writes live -- the three things somebody filing a bug is asked for.
void SandboxApp::drawAboutPrompt(Engine& e) {
    if (!showAbout_) return;
    constexpr const char* kTitle = "About Aver Engine";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (logoUiId_) {
        const f32 h = 56.0f * dpi_;
        ImGui::Image(static_cast<ImTextureID>(logoUiId_), ImVec2(h * logoAspect_, h));
        ImGui::SameLine(0.0f, 14.0f * dpi_);
    }
    ImGui::BeginGroup();
    if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
    // kEngineName/kEngineVersion are string_views, not char* -- print by length.
    ImGui::Text("%.*s", static_cast<int>(kEngineName.size()), kEngineName.data());
    if (fontMedium_) ImGui::PopFont();
    ImGui::TextDisabled("Version %.*s", static_cast<int>(kEngineVersion.size()), kEngineVersion.data());
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (rhi::IDevice* dev = e.device()) {
        ImGui::Text("Renderer   %s", rhi::backendName(dev->backend()));
        const char* adapter = dev->adapterName();
        if (adapter && *adapter) ImGui::Text("Adapter    %s", adapter);
    }
    ImGui::Text("Interface  %.0f%% scale", static_cast<double>(dpi_) * 100.0);
    ImGui::TextDisabled("Settings   %s", editor::editorPrefsPath().c_str());

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi_, 0.0f))) {
        showAbout_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Names a new file for the current level and saves it there.
//
// A NAME, NOT A FILE DIALOG, because the platform layer has openFileDialog and no save
// counterpart -- and a level belongs in the project's Content\Maps regardless, the same way
// the Content Browser's own create items work. saveLevel already accepts an arbitrary path.
void SandboxApp::drawSaveLevelAsPrompt() {
    if (!wantSaveLevelAs_) return;
    constexpr const char* kTitle = "Save Level As";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const std::string maps = project_.valid()
        ? (std::filesystem::path(project_.contentDir()) / "Maps").string()
        : std::string();

    ImGui::TextUnformatted("Name");
    ImGui::PushItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool submit = ImGui::InputTextWithHint("##savelevelas", "Arena", saveLevelAsName_,
                                                 sizeof saveLevelAsName_,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    const std::string stem = saveLevelAsName_;
    const bool named = !stem.empty();
    const std::filesystem::path target = named && !maps.empty()
        ? std::filesystem::path(maps) / (stem + ".ocworld")
        : std::filesystem::path();
    if (!target.empty()) ImGui::TextDisabled("Saves to: %s", target.string().c_str());
    else                 ImGui::TextDisabled("Saves to: <project>\\Content\\Maps\\<Name>.ocworld");

    std::error_code ec;
    const bool exists = !target.empty() && std::filesystem::exists(target, ec);
    if (exists) ImGui::TextDisabled("A level of that name is already there and will be replaced.");
    if (!saveLevelAsError_.empty())
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", saveLevelAsError_.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    const bool valid = named && !maps.empty() && stem.find_first_of("\\/:*?\"<>|") == std::string::npos;
    ImGui::BeginDisabled(!valid);
    const bool go = ImGui::Button("Save", ImVec2(110.0f * dpi_, 0.0f));
    ImGui::EndDisabled();
    if (!valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(maps.empty() ? "Open or create a project first."
                                       : "Enter a name with no path separators.");
    if ((go || (submit && valid)) && valid) {
        std::filesystem::create_directories(maps, ec);
        if (saveLevel(target.string())) {
            levelPath_ = target.string();
            levelName_ = stem;
            cbInvalidate(maps);
            setUpgradeStatus("Saved " + target.filename().string());
            wantSaveLevelAs_ = false;
            ImGui::CloseCurrentPopup();
        } else {
            saveLevelAsError_ = "Could not write " + target.filename().string() + ".";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        wantSaveLevelAs_ = false;
        saveLevelAsError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SandboxApp::drawExitPrompt(Engine& e) {
#if AVER_WITH_IMGUI
    if (!exitPrompt_) return;
    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::vector<std::string> dirty = assetEditors_.dirtyTitles();
    // The LEVEL first, because it is the thing most likely to represent an afternoon's work and
    // the thing this prompt used not to mention at all.
    const bool levelDirty = levelHasUnsavedEdits();
    if (levelDirty)
        dirty.insert(dirty.begin(),
                     "Level: " + (levelName_.empty() ? std::string("untitled") : levelName_));
    ImGui::TextWrapped("%zu item%s ha%s unsaved changes:",
                       dirty.size(), dirty.size() == 1 ? "" : "s", dirty.size() == 1 ? "s" : "ve");
    ImGui::Spacing();
    for (const std::string& t : dirty) ImGui::BulletText("%s", t.c_str());
    if (levelDirty && levelPath_.empty())
        ImGui::TextDisabled("This level has never been saved -- \"Save all\" cannot name a file "
                            "for it. Cancel, then File > Save Level As.");
    if (!exitPromptError_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", exitPromptError_.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save all and exit", ImVec2(150.0f * dpi_, 0.0f))) {
        std::string why;
        usize failed = assetEditors_.saveAllDirty(&why);
        // The level too, and only when it already HAS a path: a never-saved level has no name to
        // write to, and inventing one here would put a file somewhere the user did not choose.
        // The line above the buttons says so, and Cancel -> Save Level As is the way out.
        if (levelDirty && !levelPath_.empty() && !saveLevel(levelPath_)) {
            ++failed;
            why += (why.empty() ? "" : "; ") + std::string("could not save the level to ") + levelPath_;
        }
        if (failed == 0) {
            ImGui::CloseCurrentPopup();
            exitPrompt_ = false;
            e.requestExit();
        } else {
            // STAY OPEN on a failed save. Exiting anyway would discard exactly the work the
            // user just asked to keep.
            exitPromptError_ = why;
            AVER_ERROR("[Editor] {} editor(s) could not be saved; the exit was cancelled: {}",
                       failed, why);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard and exit", ImVec2(150.0f * dpi_, 0.0f))) {
        AVER_WARN("[Editor] exiting with {} unsaved item(s); the changes are gone", dirty.size());
        ImGui::CloseCurrentPopup();
        exitPrompt_ = false;
        e.requestExit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        exitPrompt_ = false;
        exitPromptError_.clear();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

// Opens the picker, rebuilding its list. Shared by the menu item and --open-level-picker, which
// exists so a bounded --frames run can capture this modal -- the same reason --tools-menu and
// --project-settings-page exist, and the only way a screenshot can prove a menu-driven panel
// renders without a human clicking anything.
void SandboxApp::openLevelPickerNow() {
    openLevelList_ = editor::listLevels(project_.contentDir());
    openLevelSelected_ = -1;
    openLevelError_.clear();
    // Pre-select what is already open, so the list appears showing where you are.
    for (int i = 0; i < static_cast<int>(openLevelList_.size()); ++i) {
        std::error_code lec;
        if (!levelPath_.empty() &&
            std::filesystem::equivalent(openLevelList_[static_cast<usize>(i)].absPath,
                                        levelPath_, lec))
            openLevelSelected_ = i;
    }
    openLevelPicker_ = true;
}

// File > Open Level's picker.
//
// A LIST OF THE PROJECT'S OWN LEVELS, NOT AN OS FILE DIALOG, for the same two reasons Save Level
// As gives: the platform layer has openFileDialog and no counterpart worth threading through
// here, and a level belongs to a project anyway -- offering the whole filesystem would mostly
// offer levels this project cannot resolve the assets of. listLevels walks the content root; see
// LevelList.hpp for what it refuses to walk into and why.
//
// BUILT WHEN THE MODAL OPENS, not per frame. A content root is a few thousand directory entries
// in a project that has streamed a scatter, and re-walking it every frame to draw a list that
// cannot have changed is work nobody asked for.
void SandboxApp::drawOpenLevelPrompt(Engine& e) {
    (void)e;   // required by the caller's uniform signature (buildPanels/menu dispatch); unused here
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    // ARMED BY --open-level-picker, consumed once. Done here rather than at startup because the
    // list needs project_ to be applied, which happens after the flags are read.
    if (armOpenLevelPicker_) { armOpenLevelPicker_ = false; openLevelPickerNow(); }
    // --open-level <name>: matched against both the content-relative path and the bare stem, so
    // either "Maps/Scratch.ocworld" or "Scratch" finds it. Through requestOpenLevel like every
    // other caller, so a name that matches nothing REPORTS that instead of emptying the world.
    if (!openLevelByName_.empty()) {
        const std::string want = openLevelByName_;
        openLevelByName_.clear();
        bool found = false;
        for (const editor::LevelEntry& l : editor::listLevels(project_.contentDir())) {
            if (l.relPath != want && l.name != want) continue;
            found = true;
            if (!requestOpenLevel(l.absPath, "opened by --open-level"))
                AVER_ERROR("[Level] --open-level {}: {}", want, openLevelError_);
            break;
        }
        if (!found) AVER_ERROR("[Level] --open-level {}: no level of that name in this project", want);
    }
    if (!openLevelPicker_) return;
    constexpr const char* kTitle = "Open Level";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (openLevelList_.empty()) {
        ImGui::TextDisabled("This project has no levels yet.");
        ImGui::TextDisabled("File > New Level, then Save Level As, writes one to Content\\Maps.");
    } else {
        ImGui::TextUnformatted("Levels in this project");
        ImGui::BeginChild("##levels", ImVec2(0.0f, 240.0f * dpi_), true);
        for (int i = 0; i < static_cast<int>(openLevelList_.size()); ++i) {
            const editor::LevelEntry& l = openLevelList_[static_cast<usize>(i)];
            // The one already open is marked rather than hidden: re-opening it is a legitimate
            // way to discard edits and go back to what is on disk.
            const bool current = !levelPath_.empty() &&
                std::filesystem::path(l.absPath) == std::filesystem::path(levelPath_);
            ImGui::PushID(i);
            if (ImGui::Selectable(l.name.c_str(), openLevelSelected_ == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                openLevelSelected_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (requestOpenLevel(l.absPath, "opened from File > Open Level")) {
                        openLevelPicker_ = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::SameLine();
            if (current) ImGui::TextDisabled("%s  (open)", l.relPath.c_str());
            else         ImGui::TextDisabled("%s", l.relPath.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (!openLevelError_.empty())
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", openLevelError_.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    const bool picked = openLevelSelected_ >= 0 &&
                        openLevelSelected_ < static_cast<int>(openLevelList_.size());
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Open", ImVec2(110.0f * dpi_, 0.0f)) && picked) {
        const editor::LevelEntry& l = openLevelList_[static_cast<usize>(openLevelSelected_)];
        if (requestOpenLevel(l.absPath, "opened from File > Open Level")) {
            openLevelPicker_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        openLevelPicker_ = false;
        openLevelError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#endif
}

// The unsaved-changes modal for ANY pending open. Same Save/Discard/Cancel shape as
// drawExitPrompt, relabeled: this asks about the CURRENT level's undo history and opens a
// different level rather than exiting, so reusing exitPrompt_ risked a "Discard and exit" button
// that actually opened a file.
//
// ONE MODAL FOR THREE CALLERS now (the picker, the Content Browser, a forwarded launch), which is
// why the sentence below is built from pendingOpenWhy_ rather than saying "forwarded" outright --
// it used to serve only the forwarded case because that was the only case that existed.
void SandboxApp::drawPendingOpenPrompt(Engine& e) {
#if AVER_WITH_IMGUI
    if (!pendingOpenPrompt_ && !pendingNewLevel_) return;
    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (pendingNewLevel_)
        ImGui::TextWrapped("The current level has unsaved changes. Starting a new level "
                           "discards them.");
    else
        ImGui::TextWrapped("'%s' is about to be %s. The current level has unsaved changes.",
                           std::filesystem::path(pendingOpenPath_).filename().string().c_str(),
                           pendingOpenWhy_.empty() ? "opened" : pendingOpenWhy_.c_str());
    if (levelPath_.empty())
        ImGui::TextDisabled("This level has never been saved, so there is no file to save it to. "
                            "Cancel, then File > Save Level As.");
    ImGui::Spacing();
    ImGui::Separator();

    // DISABLED WITH NO PATH TO SAVE TO. It used to treat "never saved" as success and open
    // straight through -- the one case where "Save and open" was a button that discarded.
    ImGui::BeginDisabled(levelPath_.empty());
    if (ImGui::Button(pendingNewLevel_ ? "Save and continue" : "Save and open",
                      ImVec2(160.0f * dpi_, 0.0f))) {
        if (saveLevel(levelPath_)) {
            const std::string toOpen = pendingOpenPath_;
            const bool wasNew = pendingNewLevel_;
            ImGui::CloseCurrentPopup();
            pendingOpenPrompt_ = false;
            pendingNewLevel_ = false;
            pendingOpenPath_.clear();
            if (wasNew) startNewLevel(e);
            else        openLevelDirect(e, toOpen);
        } else {
            // STAY OPEN on a failed save -- same reasoning as drawExitPrompt's identical guard:
            // proceeding anyway would discard exactly the work this prompt exists to protect.
            AVER_ERROR("[Sandbox] could not save '{}'; the forwarded open was cancelled", levelPath_);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(pendingNewLevel_ ? "Discard and continue" : "Discard and open",
                      ImVec2(170.0f * dpi_, 0.0f))) {
        const std::string toOpen = pendingOpenPath_;
        const bool wasNew = pendingNewLevel_;
        AVER_WARN("[Sandbox] the current level's unsaved changes are gone");
        ImGui::CloseCurrentPopup();
        pendingOpenPrompt_ = false;
        pendingNewLevel_ = false;
        pendingOpenPath_.clear();
        if (wasNew) startNewLevel(e);
        else        openLevelDirect(e, toOpen);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        pendingOpenPrompt_ = false;
        pendingNewLevel_ = false;
        pendingOpenPath_.clear();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

// File > Launch in Aver Engine Runtime. Starts a SEPARATE process that reads the level from disk,
// the same inputs a packaged game gets -- not anything held only in the editor's memory -- so an
// unsaved edit would silently not be there. `skipDirtyCheck` is how drawLaunchRuntimePrompt's own
// buttons launch after already answering that question, without re-triggering the same prompt.
void SandboxApp::launchInRuntime(Engine& e, bool skipDirtyCheck) {
    (void)e;   // no engine access needed; kept for the same uniform signature other menu actions use
#if AVER_MODULE_SCENE
    if (!project_.valid() || levelPath_.empty()) return;
    // Anything the runtime would read from disk and find stale: the level, an asset tab, the terrain.
    bool anythingUnsaved = levelHasUnsavedEdits() || assetEditors_.anyDirty();
#if AVER_MODULE_LANDSCAPE
    anythingUnsaved = anythingUnsaved || landscape_.dirty();
#endif
    if (!skipDirtyCheck && anythingUnsaved) { launchRuntimePrompt_ = true; return; }

    const std::string levelFile = std::filesystem::path(levelPath_).filename().string();
    std::string why;
    if (editor::launchRuntime(project_.manifestPath, levelPath_, &why)) {
        setUpgradeStatus("Launched " + levelFile + " in Aver Engine Runtime");
        AVER_INFO("[Editor] launched AverEngineRuntime.exe on {}", levelPath_);
    } else {
        setUpgradeStatus("Could not launch Aver Engine Runtime: " + why, editor::NotifySeverity::Error);
        AVER_ERROR("[Editor] could not launch Aver Engine Runtime: {}", why);
    }
#else
    (void)skipDirtyCheck;
#endif
}

// The unsaved-changes modal for File > Launch in Aver Engine Runtime. Same Save/Discard/Cancel
// shape as drawPendingOpenPrompt: the runtime is a second process reading the level from disk, so
// proceeding with unsaved edits silently ships a stale level, same as opening over them would.
void SandboxApp::drawLaunchRuntimePrompt(Engine& e) {
    (void)e;
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    if (!launchRuntimePrompt_) return;
    constexpr const char* kTitle = "Launch in Aver Engine Runtime";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const bool levelDirty = levelHasUnsavedEdits();
    bool otherDirty = assetEditors_.anyDirty();
#if AVER_MODULE_LANDSCAPE
    otherDirty = otherDirty || landscape_.dirty();
#endif
    ImGui::TextWrapped("The runtime reads everything from disk.");
    if (levelDirty)
        ImGui::TextWrapped("This level has unsaved changes.");
    if (otherDirty)
        ImGui::TextDisabled("Unsaved asset or terrain edits are not included -- save them first.");
    ImGui::Spacing();
    ImGui::Separator();

    // Saves the LEVEL only; offered only when the level is what is unsaved.
    ImGui::BeginDisabled(!levelDirty);
    const bool saveAndLaunch = ImGui::Button("Save and launch", ImVec2(160.0f * dpi_, 0.0f));
    ImGui::EndDisabled();
    if (saveAndLaunch) {
        if (saveLevel(levelPath_)) {
            ImGui::CloseCurrentPopup();
            launchRuntimePrompt_ = false;
            launchInRuntime(e, true);
        } else {
            // STAY OPEN on a failed save -- same reasoning as drawPendingOpenPrompt's identical
            // guard: proceeding anyway would launch a level missing the very edits this is for.
            AVER_ERROR("[Editor] could not save '{}'; the runtime was not launched", levelPath_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Launch saved version", ImVec2(170.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        launchRuntimePrompt_ = false;
        launchInRuntime(e, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        launchRuntimePrompt_ = false;
    }
    ImGui::EndPopup();
#endif
}

// Draws the modal offering to add the project files this project is missing, listing each fix.
void SandboxApp::drawUpgradePrompt() {
#if AVER_WITH_IMGUI
    if (pendingUpgrade_.empty() || upgradeAsked_) return;
    constexpr const char* kTitle = "Upgrade project?";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);

    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("'%s' was created by an earlier version of the editor and is missing some "
                       "of the files a project now needs.", project_.name.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Nothing you wrote is replaced. The .csproj is edited by appending, not "
                        "regenerated, so anything you added by hand stays.");
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::BeginChild("##upgradeList", ImVec2(0.0f, 200.0f * dpi_), ImGuiChildFlags_Borders)) {
        for (const editor::ProjectFix& f : pendingUpgrade_.fixes) {
            ImGui::BulletText("%s", f.summary.c_str());
            if (!f.detail.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("%s", f.detail.c_str());
                ImGui::Unindent();
            }
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    if (ImGui::Button("Upgrade", ImVec2(120.0f * dpi_, 0.0f))) {
        std::string err;
        if (editor::applyProjectUpgrade(project_, pendingUpgrade_, &err)) {
            setUpgradeStatus("Project upgraded - Compile C# to rebuild.");
            AVER_INFO("[Editor] '{}' upgraded", project_.name);
        } else {
            setUpgradeStatus("Upgrade failed: " + err, editor::NotifySeverity::Error);
            AVER_ERROR("[Editor] upgrade of '{}' failed: {}", project_.name, err);
        }
        pendingUpgrade_ = {};
        upgradeAsked_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(120.0f * dpi_, 0.0f))) {
        upgradeAsked_ = true;
        setUpgradeStatus("Project left as it is.", editor::NotifySeverity::Info);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Not now leaves every file untouched.");
    ImGui::EndPopup();
#endif
}

// Builds the whole editor UI for one frame: menu bar, toolbars, panels, drawers and dialogs.
void SandboxApp::buildUI(Engine& e) {
#if AVER_WITH_IMGUI
    uiReg_.beginFrame();
#endif
    prefsDevice_ = e.device();
#if AVER_WITH_IMGUI
    // Editor preferences are read/written by the preferences PANEL; a UI-less editor never opens
    // one, so the defaults compiled into the members stand.
    if (!prefsLoaded_) { prefsLoaded_ = true; loadEditorPreferences(); }
#endif
#if AVER_MODULE_SCENE
    if (wantMeshReload_) {
        wantMeshReload_ = false;
        releaseProjectMeshes(e);
        loadProjectMeshes(e);
    }
#endif
    if (vsyncOffRequested_) {
        vsyncOffRequested_ = false;
        if (prefsDevice_->vsyncCanDisable()) { prefsDevice_->setVSync(false); AVER_INFO("[Sandbox] vsync OFF (--no-vsync)"); }
        else AVER_WARN("[Sandbox] --no-vsync ignored: this display path cannot tear");
    }
#if AVER_WITH_IMGUI
    if (!e.device()->uiActive()) return;
    ++frameNo_;   // the Content Browser's directory-cache freshness clock

    if (browserActive_) {
        switch (browser_.draw(dpi_, fontMedium_, logoUiId_, logoAspect_)) {
            case editor::BrowserAction::Open: applyProject(e); browserActive_ = false; break;
            case editor::BrowserAction::Skip: browserActive_ = false; break;
            case editor::BrowserAction::Quit: requestExitChecked(e); break;
            case editor::BrowserAction::Stay: break;
        }
        return;
    }

    // BEFORE ANYTHING DRAWS, so the Content Browser's badges and the panel read the same latch on
    // the same frame rather than one of them trailing the other by one. It reaps finished workers
    // and may start one; it never waits on git, and never spawns a process itself.
    revisionControlTick();

    // Drawer shortcuts: Ctrl+Space toggles the Content Browser, ` toggles the Console, Escape
    // closes an open drawer. All three gate on the SAME !WantTextInput guard, so ` cannot OPEN the
    // console while another text field has focus, and once the console's own input line has
    // focus, WantTextInput swallows further ` presses too -- the same existing Ctrl+Space behaviour.
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !io.WantCaptureKeyboard) {
            if (keybinds_.pressed(editor::CommandId::DrawerToggleContent, io)) toggleDrawer(Drawer::Content);
            if (keybinds_.pressed(editor::CommandId::DrawerToggleConsole, io)) toggleDrawer(Drawer::Console);
            if (drawer_ != Drawer::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                keybinds_.pressed(editor::CommandId::DrawerDismiss, io))
                drawer_ = Drawer::None;
        }
    }

    // ---------------- menu bar ----------------
    if (ImGui::BeginMainMenuBar()) {
        if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
        ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE");
                    const bool open_file = ImGui::BeginMenu("File");
        uiReg_.track("menu.file");
        if (open_file){
#if AVER_MODULE_SCENE
            const bool haveProject = project_.valid();
            ImGui::BeginDisabled(!haveProject);
            // ASKS FIRST NOW. This called unloadLevel() straight through, which clears the undo
            // stack and every entity with no prompt -- so one wrong click on a level somebody had
            // been building for an hour threw all of it away, silently, with Cancel nowhere.
            if (ImGui::MenuItem("New Level")) {
                if (levelHasUnsavedEdits()) pendingNewLevel_ = true;
                else                        startNewLevel(e);
            }
            uiReg_.track("file.newLevel");
            // OPENS A PICKER NOW. It used to call loadStartMap(), which reopens the project's ONE
            // start map -- so a project with three levels had two the editor could not reach, and
            // clicking this on a start map whose assets had gone reloaded the same empty world and
            // looked like the menu item itself was broken.
            if (ImGui::MenuItem("Open Level...")) openLevelPickerNow();
            uiReg_.track("file.openLevel");
            // RELOAD, which is what the old "Open Level" actually did. Keeping it as its own item
            // rather than deleting the behaviour: reopening the start map to throw away edits is
            // a real thing to want, it just is not what "Open Level" means.
            ImGui::BeginDisabled(project_.startMap.empty());
            if (ImGui::MenuItem("Reload Start Level")) {
                const std::string start = project_.contentDir() + "\\" + project_.startMap;
                if (!requestOpenLevel(start, "reloaded from the project's start level"))
                    AVER_WARN("[Level] reload: {}", openLevelError_);
            }
            ImGui::EndDisabled();
            uiReg_.track("file.reloadStartLevel");
            if (ImGui::MenuItem("Save Level", "Ctrl+S")) saveLevelInteractive();
            uiReg_.track("file.saveLevel");
            if (ImGui::MenuItem("Save All", editor::chordToString(keybinds_.chordFor(editor::CommandId::SaveAll)).c_str()))
                saveAll();
            uiReg_.track("file.saveAll");
            // SAVE LEVEL AS, which the toolbar's own Save tooltip has been telling people to use
            // for as long as it has existed -- "File > New Level, then Save Level As" -- while
            // the menu had New / Open / Save and nothing else. saveLevel already takes an
            // arbitrary path; only the way to name one was missing.
            if (ImGui::MenuItem("Save Level As...")) {
                saveLevelAsName_[0] = '\0';
                const std::string stem = levelPath_.empty()
                    ? levelName_
                    : std::filesystem::path(levelPath_).stem().string();
                std::snprintf(saveLevelAsName_, sizeof saveLevelAsName_, "%s",
                              stem.empty() ? "untitled" : stem.c_str());
                saveLevelAsError_.clear();
                wantSaveLevelAs_ = true;
            }
            uiReg_.track("file.saveLevelAs");
            ImGui::EndDisabled();
            if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Open or create a project first - a level belongs to one.");
#else
            ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level");
#endif
#if AVER_MODULE_LANDSCAPE
            // Independent of the Save Level item above: a sculpt changes the resident section
            // (landscape_.data()) in memory only (handleSculpt) -- the SAME split saveLevel/Save
            // Level already has between "the editor's state" and "what is actually on disk".
            {
                const bool canSave = landscape_.loaded() && !landscape_.path().empty();
                ImGui::BeginDisabled(!canSave);
                if (ImGui::MenuItem(landscape_.dirty() ? "Save Landscape *" : "Save Landscape")) saveLandscape();
                ImGui::EndDisabled();
                uiReg_.track("file.saveLandscape");
                if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(!landscape_.loaded()
                        ? "No landscape section loaded (--landscape, or <levelname>.ocland beside the level)."
                        : "This section has no file path to save back to.");
            }
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Take Screenshot", editor::chordToString(keybinds_.chordFor(editor::CommandId::Screenshot)).c_str()))
                requestViewportScreenshot();
            uiReg_.track("file.takeScreenshot");
#if AVER_MODULE_SCENE
            // Starts a SEPARATE process (AverEngineRuntime.exe) on this level -- the disk-only
            // inputs a packaged game gets, unlike Play, which runs in-process against whatever is
            // in memory. See RuntimeLaunch.hpp for what gets launched and why it runs detached.
            {
                const bool haveProj = project_.valid();
                const bool haveLevel = !levelPath_.empty();
                const std::string runtimeExe = editor::runtimeExecutablePath();
                ImGui::BeginDisabled(!haveProj || !haveLevel || runtimeExe.empty());
                if (ImGui::MenuItem("Launch in Aver Engine Runtime")) launchInRuntime(e);
                ImGui::EndDisabled();
                uiReg_.track("file.launchRuntime");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (!haveProj)       ImGui::SetTooltip("Open a project first.");
                    else if (!haveLevel) ImGui::SetTooltip("Save the level first - the runtime reads it from disk.");
                    else if (runtimeExe.empty())
                        ImGui::SetTooltip("AverEngineRuntime.exe is not beside the editor - build the runtime (AVER_BUILD_GAME).");
                    else ImGui::SetTooltip("Starts AverEngineRuntime.exe on this level, as a packaged game would run it.");
                }
            }
#endif
            ImGui::Separator();
            // Packaging. Disabled with a SPECIFIC reason rather than a generic one: "greyed
            // out" with no explanation is the single most common way an editor wastes somebody's
            // afternoon. The item is a shell over scripts/stage-game.ps1 and adds nothing of its
            // own -- a packaging path that exists only behind a button cannot run in CI.
            {
                const bool haveProj = project_.valid();
                const bool haveScripts = haveProj &&
                    std::filesystem::exists(editor::scriptsBinaryDir(project_));
                ImGui::BeginDisabled(!haveProj || tools_.compiling());
                if (ImGui::MenuItem("Package Project...")) tools_.openPackageProject();
                ImGui::EndDisabled();
                uiReg_.track("file.packageProject");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (!haveProj)              ImGui::SetTooltip("Open a project first.");
                    else if (tools_.compiling()) ImGui::SetTooltip("A script build is running; packaging would stage a half-written assembly.");
                    else if (!haveScripts)       ImGui::SetTooltip("No compiled scripts yet - run Compile .NET first.\nThe packager refuses a project whose Binaries\\Scripts is empty.");
                    else                         ImGui::SetTooltip("Stage this project into a standalone, runnable game directory.\nRuns scripts/stage-game.ps1, which you can also run from a shell.");
                }
            }
            ImGui::Separator(); if(ImGui::MenuItem("Exit")) requestExitChecked(e); ImGui::EndMenu(); }
                    const bool open_edit = ImGui::BeginMenu("Edit");
        uiReg_.track("menu.edit");
        if (open_edit){
            if (ImGui::MenuItem("Undo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditUndo)).c_str(), false, canUndo())) undo();
            uiReg_.track("edit.undo");
            if (ImGui::MenuItem("Redo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditRedo)).c_str(), false, canRedo())) redo();
            uiReg_.track("edit.redo");
            ImGui::Separator();
            // Shortcut hints are read from the SAME registry the keypress dispatch in
            // handleManip() reads from, so a rebind can never leave the menu and the keyboard
            // disagreeing about what a chord does.
            if (ImGui::MenuItem("Copy", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditCopy)).c_str(), false, anySelected())) copySelection();
            uiReg_.track("edit.copy");
            if (ImGui::MenuItem("Paste", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditPaste)).c_str(), false, !clipboard_.entities.empty() || clipboard_.hasObject)) pasteClipboard();
            uiReg_.track("edit.paste");
            if (ImGui::MenuItem("Duplicate", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDuplicate)).c_str(), false, anySelected())) duplicateSelection();
            uiReg_.track("edit.duplicate");
            if (ImGui::MenuItem("Delete", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDelete)).c_str(), false, anySelected())) deleteSelection();
            uiReg_.track("edit.delete");
            ImGui::Separator();
            if (ImGui::MenuItem("Editor Preferences...")) showEditorPrefs_ = true;
            uiReg_.track("edit.editorPreferences");
            if (ImGui::MenuItem("Project Settings...")) showProjectSettings_ = true;
            uiReg_.track("edit.projectSettings");
            ImGui::EndMenu();
        }
                    const bool open_window = ImGui::BeginMenu("Window");
        uiReg_.track("menu.window");
        if (open_window){
            // Both of these used to discard their return value against panels that had no
            // p_open, so the items consumed a click and could never do anything: docking let
            // you close the tab, and nothing could reopen it.
            ImGui::MenuItem("World Outliner", nullptr, &showOutliner_);
            uiReg_.track("window.worldOutliner");
            ImGui::MenuItem("Details", nullptr, &showDetails_);
            uiReg_.track("window.details");
            if (ImGui::MenuItem("Content Browser", "Ctrl+Space", drawer_ == Drawer::Content)) toggleDrawer(Drawer::Content);
            uiReg_.track("window.contentBrowser");
            if (ImGui::MenuItem("Output Log", nullptr, drawer_ == Drawer::Log)) toggleDrawer(Drawer::Log);
            // track() names THE LAST ITEM SUBMITTED, so it belongs immediately after its own
            // widget. A trailing `track("window.outputLog")` used to sit after World Settings
            // below, which registered the WORLD SETTINGS rect under the Output Log's name --
            // and this is a live automation target (setWidgetResolver -> centreOf), so anything
            // clicking "window.outputLog" toggled World Settings instead. The real item was
            // left under the tell-tale name "window.outputLogDup".
            uiReg_.track("window.outputLog");
            if (ImGui::MenuItem("Console", "`", drawer_ == Drawer::Console)) toggleDrawer(Drawer::Console);
            uiReg_.track("window.console");
            if (ImGui::MenuItem("World Settings", nullptr, showWorldSettings_))
                showWorldSettings_ = !showWorldSettings_;
            uiReg_.track("window.worldSettings");
            ImGui::Separator();
#if AVER_MODULE_SCENE
            {
                const bool streamOn = streaming_.enabled();
                ImGui::BeginDisabled(!project_.valid());
                if (ImGui::MenuItem("Chunk Streaming", nullptr, streamOn)) setChunkStreamingEnabled(!streamOn);
                ImGui::EndDisabled();
                uiReg_.track("window.chunkStreaming");
                // The per-pass GPU tree, which existed for a long time and could only be read
                // as scrolling console text from the `frametime` command.
                ImGui::MenuItem("GPU Profiler", nullptr, &showProfiler_);
                uiReg_.track("window.gpuProfiler");
                // Which files name an asset. Reachable from here as well as from the Content
                // Browser's own context menu, so it can be left open while working.
                ImGui::MenuItem("References", nullptr, &showReferences_);
                uiReg_.track("window.references");
                if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - a streamed world belongs to one.");
                else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(streamOn
                        ? "Streams chunks generated + saved under <project>\\Chunks around the editor camera.\nTurning this off releases every streamed entity."
                        : "Opt-in: streams chunks generated + saved under <project>\\Chunks around the editor camera.\nDoes nothing to your level until switched on.");
            }
            {
                const bool droneOn = droneEntity_ != scene::kInvalidEntity;
                ImGui::BeginDisabled(!project_.valid());
                if (ImGui::MenuItem("Drone", nullptr, droneOn)) setDroneEnabled(!droneOn);
                ImGui::EndDisabled();
                uiReg_.track("window.drone");
                if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - the drone reads its graph from Content\\Scripts.");
                else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(droneOn
                        ? "A cube driven by Content\\Scripts\\Drone.ocgraph through GraphHost.\nTurning this off releases it."
                        : "Opt-in: spawns a cube and drives it from Content\\Scripts\\Drone.ocgraph through GraphHost.");
            }
            ImGui::Separator();
#endif
            // OUTSIDE the AVER_MODULE_SCENE block above, unlike the GPU Profiler and References
            // items: what git says about the project's files has nothing to do with whether this
            // build has an ECS compiled in.
            ImGui::MenuItem("Revision Control", nullptr, &showRevisionControl_);
            uiReg_.track("window.revisionControl");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("What git says about this project: the branch, what changed, and\n"
                                  "each file's history and diff. It READS the repository and cannot\n"
                                  "alter one -- there is no commit, revert or push here.");
            ImGui::Separator();
            ImGui::BeginDisabled(gameUi_ == nullptr);
            if (ImGui::MenuItem("Game UI Demo", nullptr, showUiDemo_)) showUiDemo_ = !showUiDemo_;
            uiReg_.track("window.gameUiDemo");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(gameUi_ ? "A hand-written Aver.UI draw list, until there is a widget tree to produce one."
                                          : "The UI render feature is unavailable on this backend.");
            ImGui::Separator();
            // Reset Layout is scoped to whichever tab is in front.
            //
            // ROUTED THROUGH THE FOCUSED EDITOR, NOT HARDCODED TO THE ACTOR EDITOR. This used to
            // call editor::resetActorEditorLayout() unconditionally whenever ANY asset tab was
            // open, so resetting a Sound or Graph tab's layout silently reset the Actor editor's
            // instead -- assetTabActive said only "some asset tab is open", never which one.
            // AssetEditorHost::resetFocusedLayout() tracks which tab's ImGui window was actually
            // focused (see its own comment) and calls that editor's own resetLayout(), which is
            // exactly what the previous behaviour needed and never had.
            const bool assetTabActive = !levelVisible_ && assetEditors_.anyOpen();
            if (ImGui::MenuItem(assetTabActive ? "Reset Tab Layout" : "Reset Layout")) {
                if (assetTabActive) assetEditors_.resetFocusedLayout();
                else              { dockBuilt_ = false; dockResetRequested_ = true; }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(assetTabActive
                    ? "Restores this tab's column widths.\nThe editor's panel layout is left alone."
                    : "Restores every panel to its default slot.\nOpen an asset tab to reset that tab instead.");
            ImGui::EndMenu();
        }
        tools_.drawMenu(project_);
        if (ImGui::BeginMenu("Build")){
            // Build Geometry is GONE rather than disabled: there is no CSG or brush system in
            // this engine and no plan for one, so the item was an Unreal-shaped label with
            // nothing behind it. Build Lighting stays, disabled and explained -- a lightmap
            // baker IS planned (docs/rendering/RENDERING.md), so that one is genuinely "not
            // yet" rather than "not a thing here".
            ImGui::BeginDisabled(true);
            ImGui::MenuItem("Build Lighting");
            ImGui::EndDisabled();
            uiReg_.track("build.buildLighting");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("No lightmap baker yet.\nLighting is real-time: the sun, and voxel cone-traced GI.");
#if AVER_MODULE_SYNAPSE
            ImGui::Separator();
            if (ImGui::MenuItem("Bake Navigation")) bakeNavigationNow(e);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Samples the live physics scene into a walkable grid and "
                                  "writes it beside this level as .ocnav.\n"
                                  "Colours in the overlay are REGIONS: two patches of floor "
                                  "in different colours have no path between them.");
            ImGui::MenuItem("Show Navigation", nullptr, &showNav_);
#if AVER_MODULE_PHYSICS
            ImGui::MenuItem("Show Colliders", nullptr, &showColliders_);
            uiReg_.track("view.showColliders");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every physics body's WORLD-SPACE BOUNDING BOX.\n"
                                  "Green = static, amber = it moves.\n"
                                  "A box collider is drawn exactly; a sphere, capsule or mesh\n"
                                  "is drawn as its bound, not its outline.");
#endif
            if (ImGui::MenuItem("Region Colours", nullptr, &navRegionColours_))
                rebuildNavOverlay(e);
#endif
            ImGui::EndMenu();
        }
        uiReg_.track("menu.build");
        if (ImGui::BeginMenu("Select")){
            // Select All works. It is greyed only when the Outliner has no rows to select, which
            // is the one case where it would genuinely do nothing.
            //
            // (The paragraph that used to lead this comment said Select All "is disabled and says
            // so" because selection was two scalars with no set behind it. That was true before
            // multi-selection shipped and contradicted the code below it afterwards -- the same
            // failure as the tooltip it went on to describe, one paragraph away from describing
            // it. Removed rather than left to catch the next reader.)
            //
            // WAS DISABLED WITH A TOOLTIP THAT OUTLIVED ITS OWN TRUTH: "The editor selects one
            // object at a time. Multi-selection is not built yet." Multi-selection shipped, and
            // this kept telling people it had not -- worse than a missing feature, because it
            // talks someone out of trying the one that exists.
            //
            // OVER THE OUTLINER'S DRAWN ORDER, not over World's entity list, and the difference
            // matters: outlinerOrder_ is what the filter and the expanded folders left on screen,
            // which is what "all" means to someone looking at it. It is also exactly what
            // multiRange walks, so shift-click and this agree by construction.
            ImGui::BeginDisabled(outlinerOrder_.empty());
            // The hint comes from the registry, like every Edit-menu row. It was a hardcoded
            // "Ctrl+A" string with no command behind it, so the menu named a key that was never
            // dispatched -- the precise drift the Edit menu's comment says this pattern prevents.
            if (ImGui::MenuItem("Select All",
                                editor::chordToString(
                                    keybinds_.chordFor(editor::CommandId::EditSelectAll)).c_str()))
                selectAllInOutliner();
            ImGui::EndDisabled();
            uiReg_.track("select.all");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && outlinerOrder_.empty())
                ImGui::SetTooltip("Nothing is listed in the World Outliner to select.");
            // BOTH, like every other deselect in this file. Clearing sel_ alone left selEntity_
            // live; it happens to read correctly today only because every consumer tests sel_
            // first, which is one refactor away from resurrecting a dead selection.
            if(ImGui::MenuItem("Select None")) { sel_=-1; selEntity_=kInvalidId; }
            uiReg_.track("select.none");
            ImGui::EndMenu();
        }
        uiReg_.track("menu.select");
        if (ImGui::BeginMenu("Help")){
            // Discarded its return value and had no dialog behind it. Every string it needs was
            // already one line away -- the version is printed in Project Settings, the backend
            // and adapter in the status bar.
            if (ImGui::MenuItem("About Aver Engine")) showAbout_ = true;
            uiReg_.track("help.about");
            ImGui::EndMenu();
        }
        uiReg_.track("menu.help");
        if (fontMedium_) ImGui::PopFont();
        ImGui::EndMainMenuBar();
    }

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize; // already excludes the menu bar
    const f32 toolbarH = 42.0f*dpi_, statusH = 26.0f*dpi_;

    // ---------------- main toolbar ----------------
    ImGui::SetNextWindowPos(wpos);
    ImGui::SetNextWindowSize(ImVec2(wsize.x, toolbarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##maintoolbar", nullptr, kChromeFlags);
    ImGui::SetCursorPosY((toolbarH - ImGui::GetFrameHeight()) * 0.5f);
    // levelPath_ and saveLevel() are declared only under AVER_MODULE_SCENE (there is no level to
    // path or save without a world), matching the File-menu Save Level item's own guard above --
    // this toolbar button was the same feature, unguarded.
#if AVER_MODULE_SCENE
    // NOT DISABLED WHEN THE LEVEL HAS NO PATH ANY MORE. A level that has never been written is
    // exactly when a person most wants this button, and greying it out to explain that in a
    // tooltip is a worse answer than opening Save As, which is what it does now.
    if (ImGui::Button(ICON_SAVE " Save")) saveLevelInteractive();
    uiReg_.track("toolbar.save");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(levelPath_.empty() ? "Save Level As..."
                                             : "Save Level (Ctrl+S)");
#else
    ImGui::BeginDisabled(true);
    ImGui::Button("Save");
    uiReg_.track("toolbar.save");
    ImGui::EndDisabled();
#endif
    ImGui::SameLine();
    if (dropButton("Add")) ImGui::OpenPopup("addActor");
    uiReg_.track("toolbar.add");
    if (ImGui::BeginPopup("addActor")) {
        ImGui::TextDisabled("Place Actor"); ImGui::Separator();
        if (ImGui::Selectable("Cube"))     spawnCube(e);
        if (ImGui::Selectable("Player Start")) addPlayerStart(e);
        uiReg_.track("toolbar.add.playerStart");
        if (ImGui::Selectable("Sphere")) spawnPrimitive(e, "Meshes/sphere.ocmesh", "Sphere");
        uiReg_.track("toolbar.add.sphere");
        ImGui::Selectable("Plane",   false, ImGuiSelectableFlags_Disabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No plane primitive yet.\nA flattened cube is the convention: add a Cube and scale Z down.");
        ImGui::Selectable("Point Light", false, ImGuiSelectableFlags_Disabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("CLight exists as a component, but no renderer reads it yet.\nLighting is the sun plus voxel cone-traced GI.");
        ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
    uiReg_.track("toolbar.compileCs");
    ImGui::SameLine();
    // Play controls, centred.
    {
        const f32 grpW = 200.0f*dpi_;
        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsize.x - grpW)*0.5f));
#if AVER_MODULE_FRAMEWORK
        const int32_t ps = aver_fw_play_state();
        const bool playing = ps != AVER_FW_PLAY_EDITOR;
        // A drone stand-in is not a play session -- begin_play never ran -- so aver_fw_play_state
        // reports EDITOR throughout. Without folding it in here, Play would stay lit while the
        // drone flew and Stop would sit greyed out, leaving no way to stop it from the toolbar.
        const bool anyPlay = playing || dronePlayActive() || spectatorPlayActive();
        ImGui::BeginDisabled(anyPlay);
        if (ImGui::Button(ICON_PLAY " Play")) startPlay();
        uiReg_.track("toolbar.play");
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Pause stays tied to a REAL session: there is no framework state to pause for a drone,
        // and a Pause button that visibly does nothing is worse than one that is clearly off.
        ImGui::BeginDisabled(!playing);
        if (ImGui::Button(ps == AVER_FW_PLAY_PAUSED ? ICON_PLAY " Resume" : ICON_PAUSE " Pause"))
            aver_fw_set_paused(ps != AVER_FW_PLAY_PAUSED ? 1 : 0);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!anyPlay);
        if (ImGui::Button(ICON_STOP " Stop")) stopPlay();
        uiReg_.track("toolbar.stop");
        ImGui::EndDisabled();
#else
        ImGui::BeginDisabled(true);
        ImGui::Button(ICON_PLAY " Play"); ImGui::SameLine();
        ImGui::Button(ICON_PAUSE " Pause"); ImGui::SameLine();
        ImGui::Button(ICON_STOP " Stop");
        ImGui::EndDisabled();
#endif
    }
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 130.0f*dpi_));
    if (dropButton(ICON_SETTINGS " Settings")) ImGui::OpenPopup("settingsMenu");
    if (ImGui::BeginPopup("settingsMenu")) {
        ImGui::Checkbox("Show Grid", &showGrid_);
        ImGui::Checkbox("Wireframe", &wireframe_);
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // ---------------- dockspace host ----------------
    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, wsize.y - toolbarH - statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("##dockhost", nullptr, kChromeFlags | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("AverDockspace");
    const ImVec2 dockSize(wsize.x, wsize.y - toolbarH - statusH);
    ImGui::DockSpace(dockId, ImVec2(0,0), ImGuiDockNodeFlags_PassthruCentralNode);

    // DEFER TO A RESTORED LAYOUT. Now that io.IniFilename points at a real file (see the UI
    // backend), ImGui rebuilds the dock tree from it before the first frame -- and this pass
    // would then DockBuilderRemoveNode it straight back to the default, so the layout would be
    // saved faithfully every exit and discarded every launch, which is worse than not saving it
    // at all: the file would exist and appear to work.
    //
    // A node with children is a layout that came from somewhere. Reset Layout still forces this
    // by clearing dockBuilt_, and that path deliberately ignores the check below -- it is the
    // one case where overwriting a restored layout is exactly what was asked for.
    // A node with children is a layout that came from somewhere -- the ini, restored before the
    // first frame. Adopting it is the whole point of persisting; rebuilding over it would mean
    // the file is written faithfully every exit and ignored every launch, which is worse than
    // not saving at all because it would look like it worked.
    const ImGuiDockNode* restored = ImGui::DockBuilderGetNode(dockId);
    const bool adoptRestored = !dockBuilt_ && !dockResetRequested_ &&
                               restored && restored->IsSplitNode();
    if (adoptRestored) {
        dockBuilt_ = true;
        // SAID ONCE, because "the layout persisted" and "the layout was silently rebuilt from
        // the default" look identical on a first launch, when the default IS what you'd see
        // either way. Without this line a regression here is invisible until someone notices,
        // months later, that their panels moved back.
        AVER_INFO("[Editor] dock layout restored from editor-layout.ini");
    }

    if (!dockBuilt_ && dockSize.x > 1.0f && dockSize.y > 1.0f) {
        // Reset Layout clears dockBuilt_ AND sets this, so it overrides the adopt above -- the
        // one case where overwriting a restored layout is exactly what was asked for.
        dockResetRequested_ = false;
        dockBuilt_ = true;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace); // private flag, required here
        ImGui::DockBuilderSetNodeSize(dockId, dockSize); // must precede the splits
        ImGuiID centre = dockId, right = 0, rightTop = 0, rightBottom = 0, left = 0;
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, &right,  &centre);
        ImGui::DockBuilderSplitNode(right,  ImGuiDir_Down,  0.60f, &rightBottom, &rightTop);
        // THE MODE PANEL GOES LEFT, where Unreal puts it and the opposite side from selection
        // panels: the right column answers "what is this object", the left "what am I doing".
        // Brush settings under Details would put terrain tools in the panel that describes the
        // selected entity, a different question with a different lifetime.
        // Split off `centre` AFTER the right-hand splits so the viewport keeps the middle: split
        // order decides which node is the central (passthrough) node, and the viewport has to be it or the scene stops rendering.
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, &left, &centre);
        ImGui::DockBuilderDockWindow("World Outliner",  rightTop);
        ImGui::DockBuilderDockWindow("Details",         rightBottom);
        ImGui::DockBuilderDockWindow("Mode",            left);
        ImGui::DockBuilderDockWindow("Level",           centre);
        ImGui::DockBuilderFinish(dockId);
    }
    if (const ImGuiDockNode* cn = ImGui::DockBuilderGetCentralNode(dockId)) centralDock_ = cn->ID;
    ImGui::End(); // ##dockhost

    // ---------------- the level, as a tab drawing the scene texture ----------------
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (focusLevelAt_ > 0 && ImGui::GetFrameCount() == focusLevelAt_) {
            ImGui::SetWindowFocus("Level");
            AVER_INFO("[Editor] --focus-level-at: bringing the Level tab forward");
        }
        levelVisible_ = ImGui::Begin("Level", nullptr,
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                     ImGuiWindowFlags_NoCollapse);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 w = avail.x > 8.0f ? avail.x : 8.0f;
        const f32 h = avail.y > 8.0f ? avail.y : 8.0f;

        vpX_ = at.x; vpY_ = at.y; vpW_ = w; vpH_ = h;
        drawGraphPrintOverlay(at, ImVec2(at.x + w, at.y + h));
        levelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        levelHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        if (levelVisible_ && e.device()) {
            e.device()->setViewportToTexture(true);
            if (const u64 tex = e.device()->viewportTextureId()) {
                // The texture is the whole backbuffer; the scene is this sub-rect of it.
                const f32 bw = ImGui::GetIO().DisplaySize.x;
                const f32 bh = ImGui::GetIO().DisplaySize.y;
                if (bw > 1.0f && bh > 1.0f) {
                    const ImVec2 uv0(at.x / bw, at.y / bh);
                    const ImVec2 uv1((at.x + w) / bw, (at.y + h) / bh);
                    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h), uv0, uv1);
                    // Drop target for content-browser assets. The scene symbols the actual placement
                    // needs live in spawnFromAssetDrop, guarded on their own -- this call site stays
                    // compilable with the module off either way.
                    //
                    // ONE PAYLOAD PER DRAG, AND IT IS THE BROWSER'S MOVE PAYLOAD. ImGui keeps exactly
                    // one payload per drag: SetDragDropPayload with cond 0 means ImGuiCond_Always and
                    // overwrites the type (imgui.cpp SetDragDropPayload). The browser used to set
                    // kAssetDragDropType and then kCbMoveDragDropType on the SAME drag, so only the
                    // move payload was ever delivered -- and this target, accepting only the asset
                    // type, silently ignored every drop. The browser now sends its whole selection
                    // under kCbMoveDragDropType and THIS target keeps what it can place.
                    // kAssetDragDropType is still accepted for any source that sends one asset.
                    if (ImGui::BeginDragDropTarget()) {
                        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kCbMoveDragDropType);
                        if (!payload) payload = ImGui::AcceptDragDropPayload(kAssetDragDropType);
                        if (payload && payload->Data) {
                            const std::string droppedPath(
                                static_cast<const char*>(payload->Data),
                                payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
                            const ImVec2 mp = ImGui::GetMousePos();
#if AVER_MODULE_SCENE
                            // ONE PATH PER LINE. A multi-selection drag sends every selected entry;
                            // a single drag sends one path with no newline in it, which this loop
                            // yields unchanged. Folders and non-placeable files are skipped HERE,
                            // since the payload is the browser's unfiltered selection.
                            //
                            // ALL AT THE SAME POINT, not fanned out: the cursor names one place,
                            // and each lands on whatever is under it -- so a stack of assets
                            // dropped together piles up rather than scattering to positions
                            // nobody chose. Moving them apart afterwards is a drag each; guessing
                            // a layout for them is not undoable in one gesture.
                            const std::vector<std::string> dragged = editor::splitDropPayload(droppedPath);
                            usize placed = 0;
                            for (const std::string& one : dragged) {
                                std::error_code dirEc;
                                if (std::filesystem::is_directory(std::filesystem::path(one), dirEc)) continue;
                                if (!isPlaceableAssetExt(lowerExt(std::filesystem::path(one)))) continue;
                                spawnFromAssetDrop(e, one, mp.x, mp.y);
                                ++placed;
                            }
                            if (placed == 0) {
                                cbStatus_ = "Nothing in that drag can be placed in the level (only .ocmesh/.ocparticle assets can)";
                                AVER_INFO("[Editor] drop: none of the {} dragged item(s) is a placeable asset",
                                          dragged.size());
                            }
#else
                            cbStatus_ = "Placing objects needs the scene module";
                            AVER_WARN("[Editor] drop: scene module not compiled in, ignoring '{}'", droppedPath);
#endif
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // NO MODE PANEL IN SELECT MODE, and the dock node collapses with it so the viewport gets the
    // width back: Select's whole tool set is already in the viewport toolbar, so a panel repeating
    // them bought nothing and cost 20% of the viewport permanently. The panel exists for modes
    // with genuinely more settings than a toolbar row can hold -- Landscape's brush, Foliage's palette.
    if (levelVisible_ || !assetEditors_.anyOpen()) {
        if (mode_ != EditorMode::Select) buildModePanel(e);
        buildPanels(e);
    }
    if (levelVisible_) buildViewportOverlay();
    drawDrawer(e);
    // AFTER drawDrawer, deliberately: it publishes drawerPixelH_, so the stack dodges the
    // drawer using this frame's height rather than last frame's.
    drawNotifications();
    buildEditorPrefs();
    buildProjectSettings();
    buildWorldSettings();
    buildProfilerPanel(e);
    buildReferencesPanel();
    buildRevisionControlPanel();
#if AVER_MODULE_SCENE
#if AVER_WITH_IMGUI
    buildChunkStreamingPanel();
#endif
#endif
#if AVER_MODULE_SCENE
    // Cleared after it fires, exactly like openAsset_ below: a selection the user then changes
    // must not be yanked back every frame.
    if (!selectEntity_.empty() && frameNo_ > 5) {
        const std::string want = selectEntity_;
        selectEntity_.clear();
        // THE OUTLINER'S PSEUDO-ENTRIES FIRST. Sun, Sky and Post Process are rows in the same
        // list and open the same Details panel, but they are not entities, so a flag that
        // enumerates the world could never reach them -- and this flag exists, by its own
        // comment, to make the Details panel capturable. The level's WATER controls live under
        // Sky + Atmosphere, and were the part that made the gap obvious.
        {
            struct Pseudo { int sel; const char* shown; };
            static const Pseudo kPseudo[] = {
                {-2, "Directional Light (Sun)"}, {-3, "Sky + Atmosphere"}, {-4, "Post Process"},
            };
            for (const Pseudo& ps : kPseudo) {
                if (std::string(ps.shown).find(want) == std::string::npos) continue;
                sel_ = ps.sel; selEntity_ = kInvalidId;
                AVER_INFO("[Editor] --select matched '{}' for '{}'", ps.shown, want);
                return;
            }
        }
        scene::World& sw = scene::World::instance();
        scene::Entity found = kInvalidId;
        // ENUMERATED THE WAY THE OUTLINER ENUMERATES, via count()/at(), matched against the SAME
        // string it displays. The first version searched entityLabels_/levelEntities_ instead and
        // matched nothing: those are editor-side bookkeeping an entity may simply not be in, while
        // the world is the authority. If this stops finding what the outliner shows, THIS is the copy that's wrong.
        const u32 n = sw.count();
        for (u32 i = 0; i < n && found == kInvalidId; ++i) {
            const scene::Entity ent = sw.at(i);
            if (!sw.valid(ent) || sw.destroyPending(ent)) continue;
            const std::string nm = sw.name(ent);
            const auto lit = entityLabels_.find(static_cast<u32>(ent));
            const std::string shown = lit != entityLabels_.end() ? lit->second : nm;
            if (shown.find(want) != std::string::npos || nm.find(want) != std::string::npos)
                found = ent;
        }
        if (found != kInvalidId) {
            sel_ = kSelScene; selEntity_ = found;
            AVER_INFO("[Editor] --select matched entity {} ('{}') for '{}'",
                      (u32)found, sw.name(found), want);
        } else {
            AVER_WARN("[Editor] --select found no entity matching '{}' among {} in the world",
                      want, n);
        }
    }
#endif
    if (!openAsset_.empty() && frameNo_ > 5) {
        const std::string want = openAsset_;
        openAsset_.clear();
        lastOpenAssetPath_ = want; // --graph-select (below) needs this after openAsset_ is cleared
        if (assetEditors_.open(want)) AVER_INFO("[Editor] --open-asset opened {}", want);
        else AVER_ERROR("[Editor] --open-asset: no registered editor accepts {}", want);
    }
    // --graph-select <nodeId>: one frame after the block above could have opened something, so the
    // editor this looks for is guaranteed to exist by the time this runs. See
    // setGraphSelectNode's own comment for why this exists at all.
    if (!graphSelectNode_.empty() && frameNo_ > 6) {
        const std::string nodeId = graphSelectNode_;
        graphSelectNode_.clear();
        if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
            if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                // The message follows the RESULT, not the call. It used to say "selected node"
                // unconditionally, which is how a capture run against a node id that does not
                // exist reported success and proved nothing.
                if (ge->selectNode(nodeId))
                    AVER_INFO("[Editor] --graph-select selected '{}'", nodeId);
                else
                    AVER_ERROR("[Editor] --graph-select: '{}' names no node or component in {}",
                               nodeId, lastOpenAssetPath_);
            } else {
                AVER_ERROR("[Editor] --graph-select: '{}' is not a graph editor", lastOpenAssetPath_);
            }
        } else {
            AVER_ERROR("[Editor] --graph-select: no open editor for '{}'", lastOpenAssetPath_);
        }
    }
    // --graph-tab <name>: same one-frame-after-open timing as --graph-select above, and the
    // same dynamic_cast, because the same thing is true -- only a graph editor has inner tabs.
    if (!graphTab_.empty() && frameNo_ > 6) {
        const std::string tab = graphTab_;
        graphTab_.clear();
        if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
            if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                if (tab == "viewport") { ge->showViewportTab(); AVER_INFO("[Editor] --graph-tab viewport"); }
                else AVER_WARN("[Editor] --graph-tab: only 'viewport' is selectable, got '{}'", tab);
            } else {
                AVER_ERROR("[Editor] --graph-tab: '{}' is not a graph editor", lastOpenAssetPath_);
            }
        } else {
            AVER_ERROR("[Editor] --graph-tab: no open editor for '{}'", lastOpenAssetPath_);
        }
    }
    pumpContentWatch();
    assetEditors_.draw(e, centralDock_, dpi_);
    tools_.drawModals(project_, dpi_);
    drawUpgradePrompt();
    drawAboutPrompt(e);
    drawSaveLevelAsPrompt();
    drawOpenLevelPrompt(e);
    drawRecoveryPrompt(e);
    drawExitPrompt(e);
    drawPendingOpenPrompt(e);
    drawLaunchRuntimePrompt(e);
    // AFTER the modals, so a request made this frame is guarded by the prompt this frame rather
    // than being loaded out from under a modal that is about to ask about it.
    applyPendingOpen(e);

    // ---------------- status bar ----------------
    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##statusbar", nullptr, kChromeFlags);
    const f32 dt = e.time().dt;
    ImGui::SetCursorPosY((statusH - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::Text("%s  |  %s  |  %s  |  DPI %.0f%%  |  %.0f FPS (%.2f ms)  |  %zu actors  |  %s",
                project_.valid() ? project_.name.c_str() : "No project",
                rhi::backendName(e.device()->backend()), e.device()->adapterName(), dpi_*100.f,
                dt>1e-6f?1.f/dt:0.f, dt*1000.f, objects_.size(),
                selectionLabel().c_str());

    // The upgrade outcome, for a while: twelve seconds is long enough to read after clicking a
    // button and short enough not to become permanent furniture. A FAILURE draws in the error
    // colour -- "Upgrade failed: ..." sliding past in frame-rate grey reads as it having worked.
    auto drawerButton = [&](const char* label, Drawer d, const char* tip) {
        const bool on = drawer_ == d;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(label)) toggleDrawer(d);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    // 250 -> 340: widened for the third (Console) button below; the other two keep their spot.
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 340.0f*dpi_));
    drawerButton("Content Browser", Drawer::Content, "Show the Content Browser  (Ctrl+Space)");
    ImGui::SameLine();
    drawerButton("Output Log", Drawer::Log, "Show the Output Log");
    ImGui::SameLine();
    drawerButton("Console", Drawer::Console, "Show the Console  (`)");
    ImGui::End();
    ImGui::PopStyleVar(2);
#else
    (void)e;
#endif
}

#if AVER_WITH_IMGUI
// THE ON-SCREEN GRAPH PRINT FEED, drawn over the 3D viewport's bottom-left corner.
//
// Visual scripting had no debugging surface at all. A Print node wrote one line into the engine
// log, among the render, asset and physics lines, so watching a graph run meant reading the
// Output Log for "[Graph]" -- and a print fired once during a jump was already scrolled away by
// the time you found it. Nothing showed which node ran, when, or in what order.
//
// NOT A SECOND LOG. The Output Log still holds every one of these permanently; this is a
// transient feed of the last few, so it answers "what just happened" without becoming something
// to scroll. Oldest at the top, so a burst reads in the order it fired.
void SandboxApp::drawGraphPrintOverlay(ImVec2 vpMin, ImVec2 vpMax) {
    if (!graphPrintOverlay_) return;

    const f64 now = ImGui::GetTime();
    std::vector<std::pair<std::string, f32>> visible;   // text, alpha
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        for (auto& gp : graphPrints_) {
            // STAMPED ON FIRST SIGHT, not when logged: logSink runs under the core log mutex on
            // whichever thread logged, where it is contracted to be quick, and ImGui::GetTime is
            // this thread's clock anyway. A line logged while the editor was minimised therefore
            // gets its full few seconds when the editor comes back, rather than having expired
            // unseen -- which is the behaviour you want from something you are watching for.
            if (gp.at < 0.0) gp.at = now;
            const f64 age = now - gp.at;
            if (age > kGraphPrintHoldSec) continue;
            const f64 fadeStart = kGraphPrintHoldSec - kGraphPrintFadeSec;
            const f32 a = age <= fadeStart
                ? 1.0f
                : static_cast<f32>(1.0 - (age - fadeStart) / kGraphPrintFadeSec);
            visible.emplace_back(gp.count > 1 ? gp.text + "  (x" + std::to_string(gp.count) + ")"
                                              : gp.text,
                                 a);
        }
        // Dropped here rather than in the sink, so the sink stays a push and a pop: an expired
        // line is only expired once something has had a chance to look at it.
        while (!graphPrints_.empty() && graphPrints_.front().at >= 0.0 &&
               now - graphPrints_.front().at > kGraphPrintHoldSec)
            graphPrints_.pop_front();
    }
    if (visible.empty()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 pad = 8.0f * dpi_;
    const f32 lineH = ImGui::GetTextLineHeight() + 2.0f * dpi_;
    f32 y = vpMax.y - pad - lineH * static_cast<f32>(visible.size());
    for (const auto& [text, alpha] : visible) {
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        // A backing plate, because the 3D viewport behind this is arbitrary: white text on a
        // snow bank or a sunlit wall is unreadable, and this exists to be read at a glance.
        dl->AddRectFilled(ImVec2(vpMin.x + pad - 4.0f * dpi_, y - 1.0f * dpi_),
                          ImVec2(vpMin.x + pad + sz.x + 4.0f * dpi_, y + lineH - 1.0f * dpi_),
                          IM_COL32(0, 0, 0, static_cast<int>(150.0f * alpha)), 3.0f * dpi_);
        dl->AddText(ImVec2(vpMin.x + pad, y),
                    IM_COL32(150, 230, 255, static_cast<int>(255.0f * alpha)), text.c_str());
        y += lineH;
    }
}

// Draws the Output Log: clear / level filter / auto-scroll over the captured log, under logMutex_.
// ONE severity palette, shared by the Output Log and the Console: four flat colours weren't worth
// factoring out, but six levels with two carrying a row background made the two copies' agreement
// load-bearing -- disagreeing means the same line reads as a different severity per drawer.
// THE PALETTE:
//   Warn      bright orange, NOT kAverOrange (the product accent) -- a warning that looks like a
//             selected control is worse than a yellow one.
//   Error     red.
//   Critical  DARK red on a dark red row: hard to scroll past unnoticed.
//   Fatal     BLACK on red -- black alone would be invisible on this panel's dark background; the
//             filled row is what makes it readable.
 void SandboxApp::logLineStyle(LogLevel l, ImVec4& text, ImVec4& row, bool& filled) {
    filled = false;
    row    = ImVec4(0, 0, 0, 0);
    switch (l) {
        case LogLevel::Fatal:
            text   = ImVec4(0.04f, 0.04f, 0.05f, 1.0f);
            row    = ImVec4(0.86f, 0.16f, 0.13f, 1.0f);
            filled = true;
            break;
        case LogLevel::Critical:
            text   = ImVec4(0.86f, 0.20f, 0.17f, 1.0f);
            row    = ImVec4(0.34f, 0.05f, 0.05f, 0.55f);
            filled = true;
            break;
        case LogLevel::Error: text = ImVec4(0.95f, 0.30f, 0.28f, 1.0f); break;
        case LogLevel::Warn:  text = ImVec4(1.00f, 0.62f, 0.15f, 1.0f); break;
        case LogLevel::Trace: text = ImVec4(0.55f, 0.57f, 0.62f, 1.0f); break;
        default:              text = ImVec4(0.82f, 0.84f, 0.88f, 1.0f); break;
    }
}

// Draws one line in its severity's style, painting the row behind it first for the two levels that
// have one. The rect draws at the cursor before the text so it sits underneath, spanning the
// content region so a short Fatal message still reads as a full banner, not a small red smear.
 void SandboxApp::drawLogLine(LogLevel level, const char* text) {
    ImVec4 fg, bg;
    bool filled = false;
    logLineStyle(level, fg, bg, filled);
    if (filled) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        const float  h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                                                  ImGui::ColorConvertFloat4ToU32(bg));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void SandboxApp::drawOutputLog() {
    if (ImGui::SmallButton("Clear")) { std::lock_guard<std::mutex> lk(logMutex_); logLines_.clear(); }
    ImGui::SameLine();
    // Same reasoning as the console's own Copy button: this text is not selectable either, and
    // the log is the FIRST thing anyone is asked for when something goes wrong.
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            for (const LogLine& ln : logLines_) { all += ln.text; all += char(10); }
        }
        ImGui::SetClipboardText(all.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the whole log to the clipboard");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * dpi_);
    // APPENDED AFTER "Warn+", never inserted before it. This index is persisted verbatim as
    // outputLog.levelFilter in editor.ini, so inserting an entry earlier would silently change
    // what every existing user's stored filter means.
    ImGui::Combo("##loglevel", &logLevelFilter_, "All\0Info+\0Warn+\0Error+\0Critical+\0");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::Separator();

    ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        const int minLevel = logLevelFilter_ == 4 ? (int)LogLevel::Critical
                          : logLevelFilter_ == 3 ? (int)LogLevel::Error
                          : logLevelFilter_ == 2 ? (int)LogLevel::Warn
                          : logLevelFilter_ == 1 ? (int)LogLevel::Info : (int)LogLevel::Trace;
        for (const LogLine& ln : logLines_) {
            if ((int)ln.level < minLevel) continue;
            drawLogLine(ln.level, ln.text.c_str());
        }
    }
    if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// Truncates to at most `maxLen` bytes, appending "..." when it had to cut -- used for the ONE-LINE
// rows in the suggestion popup and the variable browser, both of which show the FULL text in a
// hover tooltip anyway (see editor::varTooltipText), so nothing here is ever the only copy.
 std::string SandboxApp::elide(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen > 3 ? maxLen - 3 : 0) + "...";
}

// Computes up to a handful of live suggestions for whatever is currently in `consoleInput_` --
// WHAT TO BUILD item 2, "discoverability while typing". Reads the buffer as it stood at the START
// of this frame (i.e. after the input widget's own edits last frame), which is what will be drawn
// in the box this frame too -- the normal one-widget-reads-what-the-other-just-wrote ordering
// every ImGui filter-as-you-type panel uses. The word being completed is the LAST whitespace run
// in the buffer; a trailing space means that word is already finished and there is nothing partial
// left to suggest. Matches SUBSTRING (editor::commandMatchesQuery/varMatchesQuery), not prefix, so
// "cone" finds voxi.giCones from the middle of the name and "firefly" would find anything whose
// DESCRIPTION mentions it even with no matching name at all.
std::vector<SandboxApp::ConsoleSuggestion> SandboxApp::computeConsoleSuggestions() const {
    std::vector<ConsoleSuggestion> out;
    const std::string_view sv(consoleInput_);
    if (sv.empty() || std::isspace(static_cast<unsigned char>(sv.back()))) return out;

    std::vector<std::string_view> words;
    for (std::size_t i = 0; i < sv.size();) {
        while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
        const std::size_t start = i;
        while (i < sv.size() && !std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
        if (i > start) words.push_back(sv.substr(start, i - start));
    }
    if (words.empty()) return out;
    const std::string_view partial = words.back();

    if (words.size() == 1) {
        for (const editor::ConsoleCommandDesc& c : editor::consoleCatalog()) {
            if (!editor::commandMatchesQuery(c, partial)) continue;
            out.push_back({c.name + "  -- " + elide(c.help, 70), c.name, c.help});
            if (out.size() >= 6) break;
        }
    }
    // Variable names fill the rest of the list (or all of it, past the first word) -- covers
    // `get <partial>`, `set <partial>`, and simply typing a name with no command at all, which is
    // the common case for someone who does not yet think of this as a command language.
    if (out.size() < 8) {
        // ---- SHORTEST MATCH FIRST, NOT TABLE ORDER ----
        //
        // MEASURED, by looking at the panel: typing "voxi.gi" listed giCones,
        // giSkyOcclusionRays, giSkyOcclusionTile, giIntensity, giMaxDistance and
        // giUpdateInterval -- and pushed voxi.giMode off the end. That is the single variable a
        // person typing "voxi.gi" is most likely to be after (it chooses the GI estimator at all),
        // and in table order it simply lost to whatever happened to be declared earlier. A
        // completion list that hides the most general name under six specialisations of it is
        // worse than no completion list, because it reads as "that variable does not exist".
        //
        // Shortest-first is the heuristic, and it is chosen because it needs no relevance
        // scoring to maintain: within a family sharing a prefix, the shortest name IS the most
        // general one -- voxi.giMode over voxi.giSkyOcclusionRays, post.tonemap over
        // post.tonemapWhitePoint. Ties break on the name so the order is stable frame to frame
        // rather than dependent on the table, which someone editing the table would otherwise
        // perturb without ever touching this code.
        std::vector<const editor::ConsoleVar*> hits;
        for (const editor::ConsoleVar& v : editor::allVars())
            if (editor::varMatchesQuery(v, partial)) hits.push_back(&v);
        std::sort(hits.begin(), hits.end(), [](const editor::ConsoleVar* a, const editor::ConsoleVar* b) {
            if (a->name.size() != b->name.size()) return a->name.size() < b->name.size();
            return a->name < b->name;
        });
        for (const editor::ConsoleVar* v : hits) {
            std::string disp = v->name + "  =  " + editor::formatValue(v->read());
            if (v->readOnly) disp += " (ro)";
            disp += "  -- " + elide(v->help, 60);
            out.push_back({std::move(disp), v->name, editor::varTooltipText(*v)});
            if (out.size() >= 8) break;
        }
    }
    return out;
}

// Overwrites consoleInput_ wholesale and arms the same consoleFocusPending_ latch
// toggleDrawer(Console) uses -- so seeding a line (a click on a Browse Variables row) both fills
// the box and puts the caret back in it, whichever tab of the drawer happens to be showing right
// now (see drawConsole's own comment on ImGuiTabItemFlags_SetSelected for why that latch also has
// to steer the tab bar).
void SandboxApp::seedConsoleInput(const std::string& line) {
    std::strncpy(consoleInput_, line.c_str(), sizeof(consoleInput_) - 1);
    consoleInput_[sizeof(consoleInput_) - 1] = '\0';
    consoleFocusPending_ = true;
}

// Replaces only the LAST whitespace-delimited word of consoleInput_ with `insertText` plus a
// trailing space -- the live suggestion popup's accept action, distinct from seedConsoleInput
// above: a popup completes the word being TYPED, it does not discard whatever precedes it (a
// partly-typed `set voxi.msaa 4 post.t` picking "post.tonemap" from the popup must keep "set
// voxi.msaa 4 ", not erase it).
void SandboxApp::acceptConsoleSuggestion(const std::string& insertText) {
    const std::string cur(consoleInput_);
    const std::size_t lastSpace = cur.find_last_of(" \t");
    const std::string prefix = lastSpace == std::string::npos ? std::string() : cur.substr(0, lastSpace + 1);
    seedConsoleInput(prefix + insertText + " ");
}

// Draws one transcript line, splitting it into whitespace runs so a token that IS a known
// variable name carries its OWN hover tooltip -- the transcript half of WHAT TO BUILD item 1
// ("hovering a name anywhere it appears"). Runs that match nothing are left as one TextUnformatted
// call each, same as before this existed -- only a line containing at least one dot (every name in
// the table is dotted; see EditorConsole.hpp's own naming convention) pays the per-token cost, and
// even then only tokens that also contain a dot ever reach findVar. Wrapped in a Begin/EndGroup so
// the CALLER's one IsItemHovered()-after-the-call (the existing right-click-to-copy-this-line
// feature) keeps meaning "hovering anywhere on this line", not just its last token.
void SandboxApp::drawConsoleTranscriptLine(LogLevel level, const std::string& text) {
    ImGui::BeginGroup();
    if (text.find('.') == std::string::npos) {
        drawLogLine(level, text.c_str());
    } else {
        ImVec4 fg, bg; bool filled = false;
        logLineStyle(level, fg, bg, filled);
        if (filled) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                                                      ImGui::ColorConvertFloat4ToU32(bg));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, fg);
        bool any = false;
        auto emit = [&](const std::string& s) {
            if (s.empty()) return;
            if (any) ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(s.c_str());
            any = true;
        };
        std::size_t lastEnd = 0, i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            const std::size_t wordStart = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (wordStart == i) break;
            const std::string word = text.substr(wordStart, i - wordStart);
            if (word.find('.') != std::string::npos) {
                std::string trimmed = word;
                while (!trimmed.empty() &&
                       (trimmed.back() == ':' || trimmed.back() == ',' || trimmed.back() == '='))
                    trimmed.pop_back();
                if (const editor::ConsoleVar* v = editor::findVar(trimmed)) {
                    emit(text.substr(lastEnd, wordStart - lastEnd));
                    emit(word);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", editor::varTooltipText(*v).c_str());
                    lastEnd = i;
                }
            }
        }
        emit(text.substr(lastEnd));
        if (!any) ImGui::TextUnformatted("");   // an all-punctuation line; never seen in practice
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}

// One row of the Browse Variables tab: name, live value, read-only/[live]/[reload] tags, and a
// trimmed description -- WHAT TO BUILD item 3 ("descriptions, current values, and whether each is
// read-only") -- plus the full editor::varTooltipText on hover (item 1 again: this is the
// "listing" surface). Clicking seeds a ready-to-run `get`/`set` line into the console input, which
// is what actually gets someone from "found it while browsing" to "used it" without retyping the
// name they just spent effort finding.
void SandboxApp::drawConsoleVarRow(const editor::ConsoleVar& v) {
    ImGui::PushID(v.name.c_str());
    std::string row = v.name + "  =  " + editor::formatValue(v.read());
    if (v.readOnly) row += "   (read-only)";
    const char* tag = editor::varHonestyTag(editor::varHonesty(v));
    if (*tag) { row += "   "; row += tag; }
    if (!v.help.empty()) { row += "  -- "; row += elide(v.help, 100); }
    if (ImGui::Selectable(row.c_str()))
        seedConsoleInput((v.readOnly ? std::string("get ") : std::string("set ")) + v.name + " ");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", editor::varTooltipText(v).c_str());
    ImGui::PopID();
}

// The REPL: transcript, live suggestions, and the input line. Was the whole of drawConsole()
// before the console rework split it from drawConsoleBrowserTab() below; unchanged in shape.
void SandboxApp::drawConsoleTranscriptTab(Engine& e) {
    if (ImGui::SmallButton("Clear")) consoleLines_.clear();
    ImGui::SameLine();
    // COPY, BECAUSE THE TEXT CANNOT BE SELECTED. drawLogLine emits ImGui text, which draws a
    // colour per severity and offers no drag-selection -- so the one thing a console exists for,
    // getting output back OUT to paste somewhere, was impossible. Rendering the buffer into a
    // read-only InputTextMultiline would make it selectable and would throw the severity colours
    // away; a button keeps both. `frametime`'s tree is exactly the output somebody needs to hand
    // to someone else.
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (const LogLine& ln : consoleLines_) { all += ln.text; all += char(10); }
        ImGui::SetClipboardText(all.c_str());
        consoleLines_.push_back({LogLevel::Info,
                                 "[console] copied " + std::to_string(consoleLines_.size()) +
                                 " line(s) to the clipboard"});
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy every line here to the clipboard");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &consoleAutoScroll_);
    ImGui::Separator();

    // An empty transcript teaches nothing -- WHAT TO BUILD item 4's other half ("an empty console
    // worth reading"). Re-seeded any time it goes empty, Clear included: `help` below covers the
    // full grammar, this is only the orientation someone needs before they have typed a first
    // thing here.
    if (consoleLines_.empty())
        for (const std::string& line : editor::consoleWelcomeLines())
            consoleLines_.push_back({LogLevel::Info, line});

    // --drawer console:<seed> (verification-only; see drawerStartSub_'s own comment): pre-fills
    // the input box ONCE so a screenshot script can capture the live suggestion popup without a
    // mouse to type with. Consumed before computeConsoleSuggestions() below so the very first
    // frame already shows suggestions for it, not just the seeded text with an empty popup.
    if (!drawerStartSub_.empty()) {
        std::strncpy(consoleInput_, drawerStartSub_.c_str(), sizeof(consoleInput_) - 1);
        consoleInput_[sizeof(consoleInput_) - 1] = '\0';
        drawerStartSub_.clear();
    }

    // Live suggestions (WHAT TO BUILD item 2) are computed from THIS frame's starting buffer, so
    // how tall the panel needs to be is known before laying out the transcript above it.
    const std::vector<ConsoleSuggestion> suggestions = computeConsoleSuggestions();
    const f32 inputLineH = ImGui::GetFrameHeightWithSpacing();
    const f32 suggestRowH = ImGui::GetTextLineHeightWithSpacing();
    const f32 suggestH = suggestions.empty() ? 0.0f
        : std::min<f32>(static_cast<f32>(suggestions.size()), 6.0f) * suggestRowH + 8.0f * dpi_;

    ImGui::BeginChild("##consolescroll", ImVec2(0, -(inputLineH + suggestH)), false,
                       ImGuiWindowFlags_HorizontalScrollbar);
    {
        for (const LogLine& ln : consoleLines_) {
            drawConsoleTranscriptLine(ln.level, ln.text);
            // ONE line, for when the whole buffer is not what is wanted. Right-click is where a
            // person looks for this, and it costs nothing when unused.
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::SetClipboardText(ln.text.c_str());
        }
    }
    if (consoleAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (!suggestions.empty()) {
        ImGui::BeginChild("##consolesuggest", ImVec2(0, suggestH), true);
        for (std::size_t i = 0; i < suggestions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool clicked = ImGui::Selectable(suggestions[i].display.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", suggestions[i].tooltip.c_str());
            if (clicked) acceptConsoleSuggestion(suggestions[i].insert);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (consoleFocusPending_) { ImGui::SetKeyboardFocusHere(); consoleFocusPending_ = false; }
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputText("##consoleinput", consoleInput_, sizeof(consoleInput_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion, &SandboxApp::consoleInputCallback, this);
    if (submitted && consoleInput_[0] != '\0') {
        runConsoleLine(e, consoleInput_);
        consoleHistory_.emplace_back(consoleInput_);
        consoleHistoryPos_ = -1;
        consoleInput_[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);   // keep focus in the input box after Enter
    }
}

// The Browse Variables tab: a filter box over EVERY entry in editor::allVars(), grouped by prefix
// (editor::varGroupKey) -- WHAT TO BUILD item 3 in full. `get`/`set`/`vars` still work exactly as
// they always did (see EditorConsole.hpp's own handleVarsList comment); this is a second way to
// reach the same table for someone who does not yet know a name to type.
void SandboxApp::drawConsoleBrowserTab() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##consolebrowsefilter", "Filter by name or description (e.g. \"gi\", \"exposure\", \"read-only\")...",
                              consoleBrowseFilter_, sizeof(consoleBrowseFilter_));
    ImGui::Separator();

    ImGui::BeginChild("##consolevarlist", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()));
    std::string lastGroup;
    std::size_t shown = 0, total = 0;
    for (const editor::ConsoleVar& v : editor::allVars()) {
        ++total;
        if (!editor::varMatchesQuery(v, consoleBrowseFilter_)) continue;
        const std::string group = editor::varGroupKey(v.name);
        if (group != lastGroup) {
            if (!lastGroup.empty()) ImGui::Spacing();
            ImGui::TextDisabled("%s", editor::varGroupLabel(group).c_str());
            lastGroup = group;
        }
        drawConsoleVarRow(v);
        ++shown;
    }
    if (shown == 0) ImGui::TextDisabled("No variable's name or description matches '%s'.", consoleBrowseFilter_);
    ImGui::EndChild();

    // The two omissions EditorConsole.hpp's own header comment names (occlusion culling,
    // virtualized-geometry LOD selection) stay visible here too, not just in `help` -- someone
    // browsing rather than reading the transcript should not have to find that gap by its absence.
    ImGui::TextDisabled("%zu of %zu variables shown. Click a row to copy a ready-to-run line to the console input.", shown, total);
}

// The bottom Console drawer: a tab bar over the REPL transcript (unchanged behaviour) and the new
// Browse Variables listing. The Transcript tab is FORCED to the front (ImGuiTabItemFlags_SetSelected)
// exactly on the frame consoleFocusPending_ is set -- toggleDrawer(Console) sets it when the drawer
// is freshly opened (backtick, the Window menu, or the toolbar button), and drawConsoleVarRow /
// acceptConsoleSuggestion set it too, so either path lands the caret in the input box on the tab
// that actually has one, instead of leaving the latch to fire uselessly against a hidden widget.
void SandboxApp::drawConsole(Engine& e) {
    // Hands this frame's device to EditorConsole.hpp's post.* variable table, which cannot reach
    // Engine& itself (ConsoleVar::read takes no arguments -- see its own file comment for why).
    editor::setConsoleDevice(e.device());

    if (ImGui::BeginTabBar("##consoleTabs")) {
        const ImGuiTabItemFlags replFlags =
            consoleFocusPending_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Transcript", nullptr, replFlags)) {
            drawConsoleTranscriptTab(e);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Browse Variables")) {
            drawConsoleBrowserTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Splits on whitespace, echoes the line into consoleLines_ as a REPL transcript would, looks the
// first token up in editor::consoleCatalog() case-insensitively, and dispatches. UNKNOWN COMMAND
// is the one error the console itself synthesizes -- by definition no handler exists to say it more specifically.
// NO QUOTING SUPPORT: an argument containing a space cannot be expressed. A known limit, not
// solved: neither frametime nor get/set/vars ever needs one.
void SandboxApp::runConsoleLine(Engine& e, const std::string& line) {
    consoleLines_.push_back({LogLevel::Info, "> " + line});
    if (consoleLines_.size() > kMaxConsoleLines) consoleLines_.pop_front();

    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : line) {
            if (std::isspace(static_cast<unsigned char>(c))) { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty()) return;

    const editor::ConsoleCommandDesc* cmd = editor::findCommand(tokens[0]);
    auto print = [this](LogLevel lvl, std::string text) {
        consoleLines_.push_back({lvl, std::move(text)});
        if (consoleLines_.size() > kMaxConsoleLines) consoleLines_.pop_front();
    };
    if (!cmd) {
        print(LogLevel::Error, "Unknown command '" + tokens[0] + "'. Type 'help' for a list.");
        return;
    }
    const std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    cmd->handler(*this, e, args, print);
}

// ImGui InputText callback: Up/Down walks consoleHistory_, Tab completes a command name against
// editor::consoleCatalog(). Same pattern imgui_demo.cpp's own Console example uses.
 int SandboxApp::consoleInputCallback(ImGuiInputTextCallbackData* data) {
    SandboxApp* self = static_cast<SandboxApp*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        // consoleHistory_ is most-recent-LAST, so `last` is the newest entry and index 0 is the
        // oldest. Up walks toward older entries (decreasing index), Down toward newer ones
        // (increasing index) and back out to the blank line once it walks past the newest.
        if (self->consoleHistory_.empty()) return 0;
        const int last = static_cast<int>(self->consoleHistory_.size()) - 1;
        int pos = self->consoleHistoryPos_;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (pos < 0) pos = last;        // first Up press: jump to the newest entry
            else if (pos > 0) --pos;
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (pos < 0) return 0;          // already at the blank line; nothing newer to recall
            ++pos;
            if (pos > last) {               // walked past the newest: back to the blank line
                self->consoleHistoryPos_ = -1;
                data->DeleteChars(0, data->BufTextLen);
                return 0;
            }
        } else {
            return 0;
        }
        self->consoleHistoryPos_ = pos;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, self->consoleHistory_[static_cast<size_t>(pos)].c_str());
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Complete the FIRST token only (a command name); args are never completed in v1.
        const char* bufStart = data->Buf;
        const char* wordEnd = data->Buf + data->CursorPos;
        const char* wordStart = wordEnd;
        while (wordStart > bufStart && !std::isspace(static_cast<unsigned char>(wordStart[-1]))) --wordStart;
        if (wordStart != bufStart) return 0;   // only completes the command name, not an argument
        const std::string_view prefix(wordStart, static_cast<size_t>(wordEnd - wordStart));
        std::vector<const editor::ConsoleCommandDesc*> matches;
        for (const editor::ConsoleCommandDesc& c : editor::consoleCatalog())
            if (c.name.size() >= prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), c.name.begin(),
                           [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
                matches.push_back(&c);
        if (matches.size() == 1) {
            data->DeleteChars(static_cast<int>(wordStart - bufStart), static_cast<int>(wordEnd - wordStart));
            data->InsertChars(data->CursorPos, matches[0]->name.c_str());
            data->InsertChars(data->CursorPos, " ");
        } else if (matches.size() > 1) {
            std::string list = "Candidates:";
            for (const editor::ConsoleCommandDesc* m : matches) list += " " + m->name;
            self->consoleLines_.push_back({LogLevel::Info, list});
            if (self->consoleLines_.size() > kMaxConsoleLines) self->consoleLines_.pop_front();
        }
    }
    return 0;
}

// Draws the bottom drawer, sliding the Content Browser or the Output Log up over the viewport.
void SandboxApp::drawDrawer(Engine& e) {
    const f32 dt = std::fmin(e.time().dt, 0.05f);
    const f32 target = drawer_ == Drawer::None ? 0.0f : 1.0f;
    drawerAnim_ += (target - drawerAnim_) * (1.0f - std::exp(-drawerRate_ * dt));
    if (drawer_ != Drawer::None) drawerShown_ = drawer_;
    if (drawer_ == Drawer::None && drawerAnim_ < 0.004f) { drawerAnim_ = 0.0f; drawerPixelH_ = 0.0f; return; }

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize;
    const f32 statusH = 26.0f * dpi_;
    const f32 fullH = (wsize.y - statusH) * drawerFrac_;
    const f32 h = fullH * drawerAnim_;
    // PUBLISHED FOR THE VIEWPORT HINT, which is anchored to the viewport's own bottom edge and
    // would otherwise draw straight through an open drawer -- see drawerPixelH_'s declaration.
    drawerPixelH_ = h;

    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH - h));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, h));
    if (drawerRaise_) { ImGui::SetNextWindowFocus(); drawerRaise_ = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##drawer", nullptr, kDrawerFlags);

    // The top edge is a resize grip, claimed before anything else is submitted.
    const f32 gripH = 5.0f * dpi_;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##drawergrip", ImVec2(std::fmax(wsize.x, 1.0f), gripH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && wsize.y > statusH + 1.0f) {
        drawerFrac_ -= ImGui::GetIO().MouseDelta.y / (wsize.y - statusH);
        drawerFrac_ = std::fmin(0.88f, std::fmax(0.14f, drawerFrac_));
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, gripH + ImGui::GetStyle().WindowPadding.y));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, ImGui::GetWindowPos().y),
                                        ImVec2(wpos.x + wsize.x, ImGui::GetWindowPos().y),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

    if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
    ImGui::TextUnformatted(drawerShown_ == Drawer::Log ? "Output Log" :
                            drawerShown_ == Drawer::Console ? "Console" : "Content Browser");
    if (fontMedium_) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled(drawerShown_ == Drawer::Content ? "(Ctrl+Space or Esc to dismiss)" :
                         drawerShown_ == Drawer::Console ? "(` or Esc to dismiss)" : "(Esc to dismiss)");
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 34.0f * dpi_));
    if (ImGui::Button("X")) drawer_ = Drawer::None;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the drawer");
    ImGui::Separator();

    if (ImGui::GetContentRegionAvail().y > ImGui::GetFrameHeight()) {
        if (drawerShown_ == Drawer::Log)          drawOutputLog();
        else if (drawerShown_ == Drawer::Console) drawConsole(e);
        else                                       drawContentBrowser();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Opens a drawer, or closes it if it is already the one showing.
void SandboxApp::toggleDrawer(Drawer d) {
    drawer_ = (drawer_ == d) ? Drawer::None : d;
    if (drawer_ != Drawer::None) drawerRaise_ = true;
    // Arms the input line's SetKeyboardFocusHere() for the NEXT drawConsole call. Gated on this
    // flag rather than ImGui::IsWindowAppearing(), since the ##drawer window doesn't newly "appear"
    // switching FROM Content/Log TO Console -- the flag targets "just chose Console specifically".
    if (drawer_ == Drawer::Console) consoleFocusPending_ = true;
}

// Maps a notification's severity onto the palette the Output Log and Console already share.
// REUSED RATHER THAN RE-CHOSEN: logLineStyle argues that two copies of these colours drifting
// apart is itself the bug, and an error that is one red in the log and another in a toast is
// exactly that drift.
 ImVec4 SandboxApp::notifyColour(editor::NotifySeverity s) {
    switch (s) {
        case editor::NotifySeverity::Success:  return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
        case editor::NotifySeverity::Warning:  return ImVec4(1.00f, 0.62f, 0.15f, 1.0f);
        case editor::NotifySeverity::Error:    return ImVec4(0.95f, 0.30f, 0.28f, 1.0f);
        case editor::NotifySeverity::Critical: return ImVec4(0.86f, 0.20f, 0.17f, 1.0f);
        default:                               return ImVec4(0.82f, 0.84f, 0.88f, 1.0f);
    }
}

// Draws the notification stack, bottom-right, newest nearest the corner and growing upward.
void SandboxApp::drawNotifications() {
    // NOTHING DURING A CAPTURE RUN. --frames drives the render gates and the screenshot tooling,
    // which compare exact pixel values; a toast in shot changes what was measured. Every
    // autosave sibling guards on the same condition for the same class of reason.
    if (maxFrames_ != 0 && !notifyTestLift_) return;

    editor::NotificationQueue& q = editor::notifications();
    static std::vector<editor::Notification> shown;
    usize hidden = 0;
    q.tick(ImGui::GetTime(), shown, hidden);
    if (shown.empty() && hidden == 0) return;

    // OWN WINDOWS, NOT THE LEVEL'S DRAW LIST. drawGraphPrintOverlay writes into the Level
    // window's list before ImGui::Image appends the viewport texture to that same list, so it is
    // painted and then covered. A separate Begin() cannot be reached by another window's
    // submission order. GetForegroundDrawList() would also sit on top but has no hit-testing,
    // and these carry real buttons.
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const f32 pad     = 8.0f * dpi_;
    const f32 statusH = 26.0f * dpi_;
    const f32 width   = 340.0f * dpi_;
    const f32 anchorX = mv->WorkPos.x + mv->WorkSize.x - pad;
    // Above the status bar AND above the drawer. drawerPixelH_ is this frame's value because
    // this runs after drawDrawer -- ##vphint reads the same member one call earlier and is
    // therefore a frame stale while the drawer slides.
    f32 y = mv->WorkPos.y + mv->WorkSize.y - statusH - drawerPixelH_ - pad;

    const ImGuiWindowFlags base =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;

    // NEWEST FIRST, so the toast being read is placed exactly: with a bottom-right pivot its
    // position needs no knowledge of its own height. Only the older ones above it need the
    // newer heights, which are remembered from last frame.
    static std::unordered_map<u64, f32> heights;
    for (usize i = shown.size(); i-- > 0;) {
        const editor::Notification& n = shown[i];
        const bool interactive = n.actions[0] != editor::NotifyAction::None;

        ImGui::SetNextWindowPos(ImVec2(anchorX, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        // OPAQUE, not merely mostly-opaque. The viewport bars sit at 0.62 over a scene, where
        // translucency reads as depth; these land on the Details panel, and 0.92 was measured to
        // let its labels bleed through the plate -- "Transform", "Rotation" and "Base Color"
        // legible straight through an error message. A notification competing with the text
        // underneath it is one you misread.
        ImGui::SetNextWindowBgAlpha(1.0f);

        char id[32];
        std::snprintf(id, sizeof id, "##notify_%llu", static_cast<unsigned long long>(n.id));
        if (ImGui::Begin(id, nullptr, interactive ? base : (base | ImGuiWindowFlags_NoInputs))) {
            if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
            ImGui::TextColored(notifyColour(n.severity), "%s", n.title.c_str());
            if (fontMedium_) ImGui::PopFont();
            if (n.count > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("x%u", n.count);
            }
            if (!n.body.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(n.body.c_str());
                ImGui::PopTextWrapPos();
            }
            if (n.hasProgress) {
                // A negative progress is the indeterminate sentinel; ImGui draws an animated bar
                // for it when the fraction is negative, which is exactly the "working, no idea
                // how long" case a bake's first step is in.
                // EMPTY OVERLAY IN BOTH CASES. ImGui's default draws "60%" centred in the bar,
                // which at this height is taller than the bar and renders clipped through it --
                // and the percentage is redundant anyway, since the title already says "in 4s".
                ImGui::ProgressBar(n.progress, ImVec2(-FLT_MIN, 6.0f * dpi_), "");
                if (!n.progressNote.empty()) ImGui::TextDisabled("%s", n.progressNote.c_str());
            }
            for (int a = 0; a < 2; ++a) {
                if (n.actions[a] == editor::NotifyAction::None) continue;
                if (a) ImGui::SameLine();
                ImGui::PushID(a);
                if (ImGui::SmallButton(n.actionLabels[a].c_str())) q.activate(n.id, n.actions[a]);
                ImGui::PopID();
            }
            heights[n.id] = ImGui::GetWindowSize().y;
        }
        ImGui::End();

        const auto it = heights.find(n.id);
        y -= (it == heights.end() ? 48.0f * dpi_ : it->second) + pad * 0.5f;
    }

    if (hidden > 0) {
        ImGui::SetNextWindowPos(ImVec2(anchorX, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##notify_more", nullptr, base | ImGuiWindowFlags_NoInputs))
            ImGui::TextDisabled("+%zu more", hidden);
        ImGui::End();
    }

    // ACTED ON HERE, not inside the button, so an action that closes or re-orders the stack
    // cannot do it while the stack is mid-iteration.
    u64 actId = 0;
    editor::NotifyAction act = editor::NotifyAction::None;
    while (q.drainActivation(actId, act)) {
        switch (act) {
            case editor::NotifyAction::ShowOutputLog:
                // OPENED, not toggled: the user asked to see the log, and toggleDrawer would
                // close it if the Output Log already happened to be the open drawer.
                drawer_ = Drawer::Log;
                drawerRaise_ = true;
                break;
            case editor::NotifyAction::Dismiss:          q.dismiss(actId); break;
            case editor::NotifyAction::PostponeAutosave: autosavePostponeRequested_ = true; break;
            case editor::NotifyAction::RetryAutosave:    autosaveRetryRequested_ = true; break;
            default: break;
        }
    }
    // Forget heights for notifications that are gone, so the map cannot grow for the life of the
    // session on an editor that raises thousands of them.
    if (heights.size() > editor::NotificationQueue::kMaxLive * 4) heights.clear();
}

#endif

} // namespace aver
