// Editor: preference and level autosave, crash recovery, the content watcher, script auto-compile and reload.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_LANDSCAPE
// ---- editor preferences, written while the editor is still running --------------------------
//
// THEY ONLY REACHED DISK ON A CLEAN EXIT. saveEditorPreferences() had exactly two callers: the
// tail of buildEditorPrefs(), which runs only while the Preferences WINDOW is open, and
// onShutdown(). Everything adjusted through a convenient control instead of that window --
// fly speed on the mouse wheel, the wireframe toggle, Content Browser tile size, the drawer
// grip, snap steps -- survived only if the process was closed politely. A crash, a kill, or a
// device loss the user then closed lost the lot.
//
// CHEAP BY CONSTRUCTION, so a timer is all this needs: saveEditorPreferences() pushes every
// member through setPrefString, which compares against the stored value and does not dirty on
// a match, and flushEditorPrefs() early-outs when nothing is dirty. A tick that changed
// nothing costs a few dozen map lookups and no I/O at all -- which is precisely what
// onShutdown's own comment already asserted about calling it unconditionally.
//
// PERSIST ONLY, NEVER APPLY. This runs from onUpdate(), and it must stay that way: the
// preference LOAD path pokes the device (setVSync/setRenderScale) and runs from buildUI(),
// between beginFrame() and endFrame(), which removed the device at Present once already
// (D3D12Device.cpp's "DEFERRED TO A FRAME BOUNDARY, ALWAYS"). Writing a file is safe anywhere;
// re-applying is not, and nothing here re-applies.
void SandboxApp::maybeAutosavePrefs(f32 dt) {
    // NOT DURING A CAPTURE RUN. A --frames run sets view flags and the post chain from the
    // command line, and saveEditorPreferences() writes those back under `maxFrames_ == 0`
    // guards -- but the honest rule is that a bounded run must leave no trace in the user's
    // profile at all, or one machine's gate results start depending on what was captured last.
    if (maxFrames_ != 0) return;
    prefsAutosaveAccum_ += dt;
    if (prefsAutosaveAccum_ < kPrefsAutosaveSec) return;
    prefsAutosaveAccum_ = 0.0f;
    saveEditorPreferences();
}

// ---- autosave, and the recovery it exists for -----------------------------------------------
//
// There was none. Not a timer, not a sidecar, not a marker -- a repo-wide search for autosave,
// backup or recovery finds nothing, so a crash or a power cut cost the whole session, and the
// crash reporter that already exists had nothing to offer the person reading it.
//
// WRITTEN BESIDE THE LEVEL, as "<name>.ocworld.autosave", not into a temp directory: it has to
// be findable by a person who does not know this function exists, and it has to travel with the
// project if they copy the folder to ask someone else about it. The extension chain also keeps
// it out of listLevels(), which matches on the last extension, so a recovery file never appears
// in the Open Level picker as if it were a level of its own.
//
// NEVER OVER THE LEVEL. An autosave that overwrites the file is not a safety net, it is an
// unrequested save -- it would defeat the whole point of "close without saving", and it would
// write half-finished work over the last good version. The sidecar is offered on the next open
// and deleted the moment the real file is saved.
std::string SandboxApp::autosavePathFor(const std::string& levelPath) const {
    if (!levelPath.empty()) return levelPath + ".autosave";
    // A LEVEL THAT HAS NEVER BEEN SAVED STILL GETS AUTOSAVED, and it is the session that most
    // needs it: everything built since launch exists only in memory, and the old rule -- "no
    // path, so nowhere to put the sidecar" -- meant a crash or a mis-click threw away
    // strictly more work than any other case the feature covers.
    //
    // BESIDE THE PROJECT, NOT BESIDE A LEVEL THAT DOES NOT EXIST. `Saved/` is the convention the
    // crash reporter already uses for exactly this kind of thing (recoverable state that is not
    // authored content), and keeping it out of Content/ matters: a stray .ocworld under Content
    // would show up in the Open Level list and in the Content Browser as if someone had made it.
    //
    // ONE FILE, NOT ONE PER UNTITLED LEVEL. Two unsaved levels cannot exist at once -- opening
    // another closes this one, which already prompts -- so a name is not needed and a growing
    // pile of Untitled-3.autosave files is avoided.
    const std::string dir = project_.dir;
    return dir.empty() ? std::string() : dir + "\\Saved\\Untitled.ocworld.autosave";
}

void SandboxApp::maybeAutosave(f32 dt) {
#if AVER_MODULE_SCENE
    // NOT DURING A CAPTURE RUN, for maybeAutosavePrefs' reason and a sharper one: that guard
    // says a bounded run must leave no trace in the user's PROFILE, and this is their PROJECT.
    //
    // MEASURED, by doing it. A --frames run that opened a start map and then switched to another
    // level with --open-level tripped this timer DURING the switch and wrote a sidecar holding a
    // SUN record and no placements at all. That sidecar is newer than the level, so the next
    // interactive open offers to "recover unsaved changes" from it -- and accepting would have
    // replaced a working level with an empty one. A capture must not be able to do that.
    // --autosave-test LIFTS THIS DELIBERATELY, and the guard is otherwise absolute. The reason
    // for the guard is written above and it is a real incident, not a precaution: a bounded run
    // once wrote a sidecar holding a SUN record and no placements, which the next interactive
    // open then offered to "recover" over a working level. The diagnostic flag exists because
    // the countdown is unreachable from a bounded run without it -- it needs thirty seconds of
    // unsaved edits, which no capture has -- and it announces itself so a sidecar appearing
    // beside somebody's level is never a surprise.
    if (maxFrames_ != 0 && !autosaveTestLift_) { autosaveAccum_ = 0.0f; return; }
    if (maxFrames_ != 0 && autosaveTestLift_ && !autosaveTestWarned_) {
        autosaveTestWarned_ = true;
        AVER_WARN("[Autosave] --autosave-test is lifting the capture guard: this bounded run "
                  "WILL write a recovery sidecar. Point it at a throwaway project.");
    }

    // A Retry pressed on the failure notification. Handled before the disabled-check below,
    // because that check is exactly what Retry exists to undo.
    if (autosaveRetryRequested_) {
        autosaveRetryRequested_ = false;
        autosaveIntervalSec_ = kAutosaveDefaultSec;
        autosaveFailedWarned_ = false;
        autosaveAccum_ = 0.0f;
        autosaveCancelNotice();
    }

    if (autosaveIntervalSec_ <= 0.0f) return;
    // NOTHING TO SAVE is the common case and must cost nothing: no level, never saved (so there
    // is nowhere to put the sidecar), or nothing edited since it was opened.
    // NOTHING TO SAVE is the common case and must cost nothing: nothing edited since the level
    // was opened, or nowhere at all to write (no level AND no project, i.e. the placeholder
    // scene, which is not the user's work). An UNTITLED level with a project open is now saved
    // -- see autosavePathFor.
    // A COUNTDOWN IN FLIGHT IS CANCELLED, NOT LEFT RUNNING, when the reason for it goes away --
    // the commonest case being that the user saved manually while the warning was on screen.
    // Counting down to a save that is no longer needed is worse than not warning at all.
    if (!levelHasUnsavedEdits()) { autosaveAccum_ = 0.0f; autosaveCancelNotice(); return; }
    if (levelPath_.empty() && project_.dir.empty()) { autosaveAccum_ = 0.0f; autosaveCancelNotice(); return; }

    // PENDING: the countdown reached zero LAST frame and the toast said "Saving..." then. Only
    // now, one presented frame later, is the blocking write actually done -- see the state's
    // comment at autosaveState_.
    if (autosaveState_ == AutosaveState::Pending) {
        autosaveState_ = AutosaveState::Idle;
        autosaveAccum_ = 0.0f;
        autosaveRunSave();
        return;
    }

    if (autosavePostponeRequested_) {
        autosavePostponeRequested_ = false;
        if (autosavePostpones_ < kAutosaveMaxPostpones) ++autosavePostpones_;
        autosaveAccum_ = 0.0f;
        autosaveCancelNotice();
        editor::Notification p;
        p.severity = editor::NotifySeverity::Info;
        p.title = "Autosave postponed";
        p.body  = "Next attempt in " + std::to_string(static_cast<int>(autosaveIntervalSec_)) + "s.";
        p.ttlSec = 3.0;
        editor::notifications().push(std::move(p));
        return;
    }

    autosaveAccum_ += dt;

    // THE WARNING WINDOW. Announced before it happens rather than after, because the save is a
    // synchronous world walk on the main thread -- the user feels it, and a hitch you were
    // warned about is a different experience from one you were not.
    const f32 remaining = autosaveIntervalSec_ - autosaveAccum_;
    if (remaining <= kAutosaveWarnSec) {
        const int secs = remaining > 0.0f ? static_cast<int>(remaining) + 1 : 0;
        if (autosaveState_ != AutosaveState::Counting) {
            autosaveState_ = AutosaveState::Counting;
            autosaveShownSec_ = -1;
            editor::Notification n;
            n.severity = editor::NotifySeverity::Info;
            n.title = "Autosaving soon";
            n.hasProgress = true;   // implies no expiry, so the countdown cannot fade mid-count
            autosaveNotify_ = editor::notifications().push(std::move(n));
        }
        // ONE UPDATE PER WHOLE SECOND, not per frame: the text only changes once a second, and
        // pushing 144 identical updates a second through a mutex to redraw the same string is
        // work nobody sees.
        if (secs != autosaveShownSec_) {
            autosaveShownSec_ = secs;
            const bool exhausted = autosavePostpones_ >= kAutosaveMaxPostpones;
            editor::Notification u;
            u.severity = editor::NotifySeverity::Info;
            u.title = "Autosaving in " + std::to_string(secs) + "s";
            u.body  = exhausted ? "Postponed " + std::to_string(kAutosaveMaxPostpones) +
                                      " times already - saving this time."
                                : "The editor will pause briefly to write a recovery file.";
            editor::notifications().update(autosaveNotify_, u.severity, u.title, u.body);
            editor::notifications().setProgress(
                autosaveNotify_, 1.0f - (remaining / kAutosaveWarnSec), "");
            if (!exhausted)
                editor::notifications().setActions(autosaveNotify_,
                                                   editor::NotifyAction::PostponeAutosave, "Postpone",
                                                   editor::NotifyAction::None, "");
            else
                editor::notifications().setActions(autosaveNotify_, editor::NotifyAction::None, "",
                                                   editor::NotifyAction::None, "");
        }
    }

    if (autosaveAccum_ < autosaveIntervalSec_) return;

    // ZERO. Say "Saving..." and STOP -- the write happens next frame, after this one has been
    // presented. Doing both here would set the state to "Saved" before buildUI ever ran, so the
    // one message describing the stall the user is about to feel would never be drawn.
    autosaveState_ = AutosaveState::Pending;
    editor::notifications().update(autosaveNotify_, editor::NotifySeverity::Info,
                                   "Autosaving...", "Writing a recovery file.");
    editor::notifications().setProgress(autosaveNotify_, -1.0f, "");
    // The buttons go NOW, not when the save finishes: activations are drained in onRender, which
    // runs after onUpdate, so a Postpone clicked on this frame would arrive after the save had
    // already happened and would silently do nothing.
    editor::notifications().setActions(autosaveNotify_, editor::NotifyAction::None, "",
                                       editor::NotifyAction::None, "");
#else
    (void)dt;
#endif
}

// Clears any countdown notification and returns to Idle. Safe to call when there is none.
void SandboxApp::autosaveCancelNotice() {
    if (autosaveNotify_) editor::notifications().close(autosaveNotify_);
    autosaveNotify_ = 0;
    autosaveState_ = AutosaveState::Idle;
    autosaveShownSec_ = -1;
}

// The write itself, unchanged in substance from what maybeAutosave used to do inline. Guarded on
// the same module as its caller's body was: saveLevel and autosavePathFor are scene-side.
void SandboxApp::autosaveRunSave() {
#if AVER_MODULE_SCENE
    const std::string dst = autosavePathFor(levelPath_);
    if (dst.empty()) { autosaveAccum_ = 0.0f; autosaveCancelNotice(); return; }
    // The untitled path lives under <project>/Saved, which need not exist yet. Harmless for the
    // beside-the-level case, where the parent is the level's own folder.
    {
        std::error_code mkec;
        const std::filesystem::path parent = std::filesystem::path(dst).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, mkec);
    }
    const std::string name = std::filesystem::path(dst).filename().string();
    if (saveLevel(dst)) {
        autosaveWritten_ = true;
        autosavePostpones_ = 0;   // a successful save earns the postpones back
        AVER_INFO("[Autosave] wrote {}", dst);
        editor::notifications().finish(autosaveNotify_, editor::NotifySeverity::Success,
                                       "Autosaved", name, 4.0);
    } else {
        // ONCE, not every interval: a directory that cannot be written will not start working,
        // and a log line every thirty seconds would bury everything else.
        if (!autosaveFailedWarned_) {
            autosaveFailedWarned_ = true;
            AVER_WARN("[Autosave] could not write {} -- autosave is off for this session", dst);
        }
        autosaveIntervalSec_ = 0.0f;
        // STICKY, WITH A WAY BACK. This used to be a single log line and then silence: the
        // user's safety net was gone for the rest of the session and nothing on screen ever said
        // so. Every real cause of it -- a locked file, a full disk, a share that blinked -- is
        // transient, so latching it off for the session with no way to undo was always the wrong
        // answer to a temporary problem.
        editor::notifications().finish(autosaveNotify_, editor::NotifySeverity::Error,
                                       "Autosave failed - it is now OFF for this session",
                                       "Could not write " + dst, 0.0);
        editor::notifications().setSticky(autosaveNotify_, true);
        editor::notifications().setActions(autosaveNotify_,
                                           editor::NotifyAction::RetryAutosave, "Turn back on",
                                           editor::NotifyAction::ShowOutputLog, "Show in Output Log");
    }
    autosaveNotify_ = 0;
#endif
}

// Drops the sidecar. Called when the level is saved for real, and when its recovery is declined.
void SandboxApp::clearAutosave() {
    const std::string p = autosavePathFor(levelPath_);
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::remove(p, ec);
    autosaveWritten_ = false;
    autosaveAccum_ = 0.0f;
}

// Offers a newer sidecar after a level opens. Answering is the point -- an autosave nobody is
// told about is a file, not a recovery.
void SandboxApp::checkForRecovery() {
#if AVER_MODULE_SCENE
    recoveryPath_.clear();
    const std::string p = autosavePathFor(levelPath_);
    if (p.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return;
    // AN UNTITLED SIDECAR HAS NOTHING TO BE NEWER THAN. The comparison below exists to reject a
    // sidecar older than the level it shadows; with no level there is no such file, so the test
    // cannot be made and must not be faked -- offer it and let the author decide.
    if (levelPath_.empty()) {
        recoveryPath_ = p;
        AVER_WARN("[Autosave] an unsaved level from a previous session is recoverable: {}", p);
        return;
    }
    // NEWER THAN THE LEVEL, or there is nothing to recover: a sidecar older than the file it
    // shadows is the leftover of a session that ended by saving properly, and offering it would
    // invite someone to overwrite good work with stale work.
    const auto sideT = std::filesystem::last_write_time(p, ec);
    if (ec) return;
    const auto liveT = std::filesystem::last_write_time(levelPath_, ec);
    if (ec || sideT <= liveT) { std::filesystem::remove(p, ec); return; }
    recoveryPath_ = p;
    AVER_WARN("[Autosave] '{}' has unsaved changes from a previous session", levelPath_);
#endif
}

void SandboxApp::drawRecoveryPrompt(Engine& e) {
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    if (recoveryPath_.empty()) return;
    constexpr const char* kTitle = "Recover unsaved changes?";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("'%s' was left with unsaved changes -- the editor closed before they were "
                       "saved.", std::filesystem::path(levelPath_).filename().string().c_str());
    ImGui::TextDisabled("Recovering opens the autosave. The level on disk is not touched until "
                        "you save.");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Recover", ImVec2(120.0f * dpi_, 0.0f))) {
        const std::string p = recoveryPath_;
        recoveryPath_.clear();
        ImGui::CloseCurrentPopup();
        // Loaded like any other level, then RE-POINTED at the real file: recovering must not
        // leave levelPath_ aimed at the sidecar, or the next Ctrl+S would save into it and the
        // level would never actually be written.
        const std::string real = levelPath_;
        openLevelDirect(e, p);
        levelPath_ = real;
        levelName_ = std::filesystem::path(real).stem().string();
        // AND IT REALLY IS UNSAVED. loadLevel marks the document clean, which is right for
        // every ordinary open and wrong for exactly this one: the content came from the
        // autosave sidecar, so the file at levelPath_ does NOT contain it. Saying otherwise
        // would let the recovered work be closed without a prompt -- losing it twice.
        markLevelUnsaved();
        setUpgradeStatus("Recovered - not yet saved", editor::NotifySeverity::Warning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(120.0f * dpi_, 0.0f))) {
        std::error_code ec;
        std::filesystem::remove(recoveryPath_, ec);
        recoveryPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

#endif

// Tells the formats layer where averdesign.exe is installed. Not scripting-specific: it points at
// the Roslyn build tool and is called unguarded from onInit(). This, and everything down to
// reloadScripts(), used to sit inside one AVER_MODULE_SCRIPTING block, but only
// resolveScriptsDir()/reloadScripts() actually touch scripts_/ScriptHost -- the rest is called
// from unguarded sites. The guard is narrowed to just the two functions that need it.
void SandboxApp::locateAverDesign() const {
#if AVER_HAVE_ROSLYN
    fmt::setAverDesignPath(executableDir() + "/Tools/averdesign.exe");
#endif
}

#if AVER_MODULE_SCRIPTING
std::string SandboxApp::resolveScriptsDir() const {
    if (scriptsDir_.empty())
        return project_.valid() ? editor::scriptsBinaryDir(project_) : executableDir() + "\\Scripts";
    const std::string& sd = scriptsDir_;
    const bool absolute = sd.size() > 1 && (sd[1] == ':' || sd[0] == '\\' || sd[0] == '/');
    return absolute ? sd : executableDir() + "\\" + sd;
}

#endif

void SandboxApp::maybeAutosaveProject(f32 dt) {
    // NOT DURING A CAPTURE RUN, the same rule maybeAutosavePrefs follows and for a sharper
    // reason: --frames sets render settings from the command line, and applyProjectRenderSettings
    // marks the project dirty when it does, so a bounded run would write those flags into the
    // user's manifest as if they had chosen them.
    if (maxFrames_ != 0) { projectAutosaveAccum_ = 0.0f; return; }
    if (!projectDirty_ || !project_.valid()) { projectAutosaveAccum_ = 0.0f; return; }
    projectAutosaveAccum_ += dt;
    if (projectAutosaveAccum_ < kProjectAutosaveSec) return;
    projectAutosaveAccum_ = 0.0f;
    std::string why;
    if (saveProjectManifest(&why)) {
        projectSaveStatus_ = "Saved.";
    } else {
        projectSaveStatus_ = "Save failed: " + why;
        // THE FAILURE ARM ONLY. A successful project autosave fires on a 0.5s debounce while a
        // slider is being dragged -- a toast per success would be a toast twice a second. A
        // FAILURE means the manifest could not be written at all, and today that is invisible
        // the moment the Project Settings panel is closed.
        notifyOutcome(editor::NotifySeverity::Error, "Could not save project settings", why, true);
    }
}

// Starts watching the project's content root, recursively, for changes made outside the editor.
void SandboxApp::startContentWatch() {
    contentWatch_.stop();
    if (!project_.valid()) return;
    const std::string root = project_.contentDir();
    if (root.empty()) return;
    if (contentWatch_.start(root, /*recursive=*/true))
        AVER_INFO("[Editor] watching '{}' for changes made outside this editor", root);
}

// Drains the watcher once a frame and tells the asset editors what changed.
void SandboxApp::pumpContentWatch() {
    if (!contentWatch_.watching()) return;
    watchEvents_.clear();
    if (contentWatch_.poll(watchEvents_)) {
        AVER_WARN("[Editor] the watcher lost records; every open editor is being told to re-read");
        assetEditors_.notifyWatchLost();
// BOTH modules, not just PBR: materials() is the PBR system, but it is reached THROUGH
// voxiRenderer_, declared under AVER_MODULE_VOXI. Guarding on PBR alone compiles in a VOXI=OFF +
// PBR=ON build where the member doesn't exist -- a configuration failing to build at head, unnoticed, since module-matrix.ps1 could not run (Get-Cached).
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // Records were lost, so a texture may have arrived unseen. Forgetting the failed
        // resolves is cheap and the alternative is a material stuck on its fallback forever.
        voxiRenderer_.materials().forgetFailedResolves();
#endif
        if (autoCompile_) scheduleAutoCompile("the watcher lost records");
        return;
    }
    bool sawImage = false;
    for (const FileEvent& ev : watchEvents_) {
        if (ev.kind == FileChange::Deleted) continue;
        const std::string full = (std::filesystem::path(contentWatch_.root()) / ev.path).string();
        if (assetEditors_.notifyFileChanged(full))
            AVER_TRACE("[Editor] '{}' changed on disk; its tab was told", ev.path);
        if (autoCompile_ && isScriptSource(ev.path)) scheduleAutoCompile(ev.path);
        if (isTextureSource(ev.path)) sawImage = true;
    }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
    // AN IMAGE APPEARED OR CHANGED UNDER THE CONTENT ROOT, the one moment a texture that failed
    // to resolve might now succeed. The material system remembers failures so a material naming a
    // missing file doesn't re-hit the filesystem every drain; without this call a dropped-in PNG would do nothing until restart.
    if (sawImage) voxiRenderer_.materials().forgetFailedResolves();
#else
    (void)sawImage;
#endif
    serviceAutoCompile();
}

// True for an image the texture loader can actually decode. Kept in step with
// modules/platform/src/Image.cpp, which is stb_image: a format listed here that stb cannot read
// costs one wasted retry, and one it CAN read that is missing here never triggers a retry.
 bool SandboxApp::isTextureSource(const std::string& rel) {
    const usize dot = rel.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = rel.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp" ||
           ext == "psd" || ext == "gif" || ext == "hdr" || ext == "pic" || ext == "ppm" ||
           ext == "pgm" || ext == "octex";
}

// True for a hand-written .cs under the content root. Excludes bin/ and obj/ path segments.
 bool SandboxApp::isScriptSource(const std::string& rel) {
    if (rel.size() < 4 || rel.compare(rel.size() - 3, 3, ".cs") != 0) return false;
    for (usize seg = 0; seg < rel.size(); ) {
        const usize slash = rel.find('/', seg);
        const usize len = (slash == std::string::npos ? rel.size() : slash) - seg;
        const std::string_view part(rel.data() + seg, len);
        if (part == "bin" || part == "obj") return false;
        if (slash == std::string::npos) break;
        seg = slash + 1;
    }
    return true;
}

// Pushes the auto-compile deadline out, so a burst of changes produces one build.
void SandboxApp::scheduleAutoCompile(const std::string& why) {
    autoCompileDue_ = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(kAutoCompileQuietMs);
    if (autoCompileReason_.empty()) autoCompileReason_ = why;
    ++autoCompilePending_;
}

// Starts the queued auto-compile once its deadline passes and no build is running.
void SandboxApp::serviceAutoCompile() {
    if (autoCompilePending_ == 0) return;
    if (std::chrono::steady_clock::now() < autoCompileDue_) return;
    if (tools_.compiling()) {
        autoCompileDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        return;
    }
    if (!project_.valid()) { autoCompilePending_ = 0; autoCompileReason_.clear(); return; }
    AVER_INFO("[Editor] auto-compile: {} script change(s) settled (first was '{}')",
              autoCompilePending_, autoCompileReason_);
    autoCompilePending_ = 0;
    autoCompileReason_.clear();
    tools_.triggerToolbarCompile(project_);
}

#if AVER_MODULE_SCRIPTING
bool SandboxApp::reloadScripts(const std::string& binDir, std::string* status) {
    if (!scripts_.ready()) {
        if (status) *status = "The scripting host is not running: " + scripts_.declineReason();
        return false;
    }
    const bool collected = scripts_.unloadScripts();
    const i32 n = scripts_.loadScripts(binDir);
    if (n < 0) {
        if (status) *status = "The scripting host refused the load.";
        return false;
    }
    if (status) {
        *status = std::to_string(n) + " behaviour(s) live from " + binDir +
                  (collected ? "" : " (the previous load context is still finalising)");
    }
    editor::notifyActorEditorsScriptsReloaded();
    return true;
}

#endif

} // namespace aver
