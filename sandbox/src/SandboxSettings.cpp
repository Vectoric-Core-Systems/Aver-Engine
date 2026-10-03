// Editor: editor preferences and every settings page.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

#include <algorithm>

namespace aver {
#if AVER_WITH_IMGUI
// Reads every editor preference into the members that back the widgets.
void SandboxApp::loadEditorPreferences() {
    using namespace editor;
    cbGallery_          = prefBool ("contentBrowser.gallery",        cbGallery_);
    cbTileSize_         = prefFloat("contentBrowser.tileSize",       cbTileSize_);
    cbDoubleClickEnter_ = prefBool ("contentBrowser.dblClickEnter",  cbDoubleClickEnter_);
    drawerFrac_         = prefFloat("drawers.heightFraction",        drawerFrac_);
    drawerRate_         = prefFloat("drawers.slideRate",             drawerRate_);
    logAutoScroll_      = prefBool ("outputLog.autoScroll",          logAutoScroll_);
    logLevelFilter_     = prefInt  ("outputLog.levelFilter",         logLevelFilter_);
    consoleAutoScroll_  = prefBool ("console.autoScroll",            consoleAutoScroll_);
    // --auto-compile ON THE COMMAND LINE WINS FOR THIS RUN (autoCompileFromCli_, set by
    // setAutoCompile at construction), the same precedence averSrFromCli_ gives AverSR further down.
    if (!autoCompileFromCli_) autoCompile_ = prefBool("scripting.autoCompile", autoCompile_);
    showGrid_           = prefBool ("viewport.showGrid",             showGrid_);
    // viewport.showColliders is NOT restored on purpose: it's a debug view, and persisting it left
    // every level opening covered in collider bounds (glowing dots, with bloom). Off each launch
    // (View > Show Colliders); toggled from the Window menu, so it rides onShutdown's sync, not
    // buildEditorPrefs' save-on-close.
    showOutliner_       = prefBool ("panels.worldOutliner",          showOutliner_);
    showDetails_        = prefBool ("panels.details",                showDetails_);
    flySpeed_           = prefFloat("viewport.flySpeed",             flySpeed_);
    lookSpeed_          = prefFloat("viewport.lookSensitivity",      lookSpeed_);
    // View flags (wireframe/unlit/showStaticMeshes/showAtmosphere) and the snap grid restore prior
    // state. View flags are gated to interactive runs only: they change the rendered image, so a
    // stored value reaching a --frames run would shift all twenty gate probes machine to machine
    // (--view-mode/--unlit on the command line still win, via viewModeFromCli_; wireframe used to
    // load ungated here until --view-mode wireframe exposed that bug). Snap is ungated: it can't
    // alter a pixel.
    if (maxFrames_ == 0) {
        if (!viewModeFromCli_) {
            wireframe_    = prefBool("viewport.wireframe",          wireframe_);
            unlit_        = prefBool("viewport.unlit",              unlit_);
        }
        showStaticMeshes_ = prefBool("viewport.showStaticMeshes",   showStaticMeshes_);
        showAtmosphere_   = prefBool("viewport.showAtmosphere",     showAtmosphere_);
    }
    snapMove_           = prefBool ("snap.move",                      snapMove_);
    snapRot_            = prefBool ("snap.rotate",                    snapRot_);
    snapScale_          = prefBool ("snap.scale",                     snapScale_);
    moveSnap_           = prefFloat("snap.moveStep",                  moveSnap_);
    rotSnap_            = prefFloat("snap.rotateStep",                rotSnap_);
    scaleSnap_          = prefFloat("snap.scaleStep",                 scaleSnap_);

    // Post process is a property of the VIEW, not the world (docs/EDITOR.md: exposure is one
    // histogram over the whole target; two people opening the same level shouldn't inherit each
    // other's eyes), so these nine live here, not in the level file; none of them persisted
    // before this.
    //
    // Gated off on capture runs (maxFrames_ != 0, same test applyCaptureExposureRule uses): a
    // stored exposure reaching a --frames run would change the image machine to machine --
    // docs/STATUS.md: "A post chain whose default state changed the image would invalidate the
    // whole oracle," the worst shape a gate failure can take.
    //
    // Also gated when the project states the key -- the second guard, on the three fields that
    // have one: a project named on the command line applies in onInit (SandboxApp.cpp:763)
    // before this function first runs, so without this guard a stored post.exposure would
    // silently overwrite it one frame later. Precedence is command line > manifest > stored
    // preference, matching the browser-open path.
    if (maxFrames_ == 0) {
        // Only the four fields the panel still shows persist; the rest of PostSettings (exposureKey,
        // the speeds, Min/Max clamps, histogram cuts, local exposure, adaptationRealism,
        // meteringCenterWeight, bloom threshold/knee) is tuned in RHI.hpp now (a stored copy let a
        // dragged-once post.exposure of 4.05, +2 stops, outlive every relaunch and override that
        // tuning). A console post.* var can still set any of them for the session (EditorConsole.hpp)
        // -- it's just not written back to disk.
        if (!postExposureFromCli_ && project_.postExposure < 0.0f)
            post_.exposure       = prefFloat("post.exposure",       post_.exposure);
        if (!postAutoExpFromCli_ && project_.postAutoExposure < 0)
            post_.autoExposure   = prefBool ("post.autoExposure",   post_.autoExposure);
        if (!postBloomFromCli_ && project_.postBloom < 0.0f)
            post_.bloomIntensity = prefFloat("post.bloomIntensity", post_.bloomIntensity);
        post_.nightVision = prefFloat("post.nightVision", post_.nightVision);
        // One-time reset: post.exposure's meaning changed from a raw multiplier to compensation on
        // Eye Adaptation, so a stored 4.05 is the old bug preserved, not a real choice.
        // settingsVersion < 2 means this .ini predates the change -- take the compiled-in value once
        // and let the save below bump the version. Same CLI/manifest guard as the load above.
        if (!postExposureFromCli_ && project_.postExposure < 0.0f && prefInt("post.settingsVersion", 0) < 2)
            post_.exposure = 1.0f;
        // Version 3 (2026-09-28): the compiled-in exposureKey was halved, one stop (owner: "make the
        // current eye exposure -1.0 the default"). A stored compensation doubles once so the picture
        // stays as it was: the owner's -1.0 reads +0.0 now. Capped at the panel's +3 EV.
        if (!postExposureFromCli_ && project_.postExposure < 0.0f && prefInt("post.settingsVersion", 0) < 3)
            post_.exposure = std::min(post_.exposure * 2.0f, 8.0f);
    }

    // The derived-data cache's write-behind budget, in MEGABYTES on the wire because that is
    // the unit the control shows; the renderer takes bytes.
    ddcRamBudgetMb_ = prefFloat("ddc.ramBudgetMb", ddcRamBudgetMb_);
#if AVER_MODULE_VOXI
    voxiRenderer_.setGiCacheRamBudget(static_cast<u64>(ddcRamBudgetMb_) * 1024ull * 1024ull);
#endif

    prefIdeName_ = prefString("contentBrowser.ide", "");

    if (prefsDevice_ && prefsDevice_->vsyncCanDisable())
        prefsDevice_->setVSync(prefBool("display.vsync", prefsDevice_->vsync()));
    frameGenWhileEditing_ = prefBool("display.frameGenWhileEditing", false);
    frameGenTrajectory_ = prefInt("display.frameGenTrajectory", 0);
    frameGenTrain_ = prefBool("display.frameGenTrain", false);
    // A stored render scale applies behind a crash cookie: applying one below 1 can lose the GPU
    // device -- a persisted setting that kills the device at startup is a trap with no way out
    // from inside the editor; one evening was lost after the AverSR combo persisted a 0.67 that
    // bricked every launch. Engine::frameStep returns as soon as deviceLost() is true, before
    // onUpdate(), so there's no "undo it" hook -- only what's already on disk survives. The flag
    // goes down before the risky call and clears 30 frames after presenting has worked; the scale
    // resets to 1 rather than being silently skipped, so the user sees and can change it. The
    // underlying bug is fixed (setRenderScale now parks and rebuilds at the next beginFrame), but
    // this stays as the only rescue for an editor.ini written by a build that had it.
    //
    // AverSrChoice (AverSrChoice.hpp) migrates once off the pre-Auto display.aversr/renderScale pair
    // if display.aversrChoice was never written (migration is a pure function, unit-tested in
    // AverSrChoiceTest.cpp, not inlined here).
    // --render-scale or --aversr on the command line skips this whole block (averSrFromCli_),
    // leaving both the render scale and the AverSR choice alone. display.aversrChoice, .aversr and
    // .renderScalePending are the three keys the AverSR combo owns (see saveEditorPreferences); the
    // restore is guarded as a whole under #if AVER_MODULE_SR, symmetric with the save.
#if AVER_MODULE_SR
    if (prefsDevice_ && renderScaleOverride_ == 1.0f && !averSrFromCli_) {
        const std::string storedChoice = prefString("display.aversrChoice", "");
        averSrChoice_ = editor::migrateAverSrChoice(!storedChoice.empty(), storedChoice,
                                                    prefFloat("display.aversr", 0.0f),
                                                    prefFloat("display.renderScale", 1.0f));
        // ONE-TIME NOTE (3.3 A): only when THIS load is the migration itself, and it landed on
        // Auto -- a session that already has a display.aversrChoice key on disk (Auto included)
        // was an explicit choice, not a silent default, and needs no note.
        averSrMigrationNoteArmed_ =
            storedChoice.empty() && averSrChoice_ == editor::AverSrChoice::Auto;

        const bool pending = prefBool("display.renderScalePending", false);
        if (averSrChoice_ == editor::AverSrChoice::Manual) {
            // Crash-cookie mechanism as above (unchanged from before Auto existed). Manual is the
            // ONLY choice that still restores a raw display.renderScale directly -- every named
            // level below re-derives its own canonical scale through applyAverSrQuality instead.
            const f32 stored = prefFloat("display.renderScale", prefsDevice_->renderScale());
            if (stored == 1.0f) {
                prefsDevice_->setRenderScale(stored);         // early-outs; costs nothing
            } else if (pending && !renderScaleCookieArmed_) {
                // !renderScaleCookieArmed_: a cookie THIS process armed isn't evidence the previous
                // launch crashed (see updateAverSrAuto's prefsLoaded_ gate for the frame-1 ordering bug).
                AVER_CRITICAL("[Sandbox] the last launch did not survive a stored render scale of "
                              "{:.2f} -- resetting display.renderScale to 1. Set it again if that "
                              "was not the cause; the scale itself is the thing that needs fixing.",
                              stored);
                setPrefFloat("display.renderScale", 1.0f);
                setPrefBool("display.renderScalePending", false);
                flushEditorPrefs();
            } else {
                setPrefBool("display.renderScalePending", true);
                flushEditorPrefs();   // ON DISK BEFORE THE DEVICE IS RISKED -- the whole point
                renderScaleCookieArmed_ = true;
                prefsDevice_->setRenderScale(stored);
            }
        } else if (pending && !renderScaleCookieArmed_) {
            // Same !renderScaleCookieArmed_ reasoning as the Manual branch above.
            // A named level (or one Auto resolved to previously) didn't survive its own launch --
            // same crash class as Manual's raw scale, but via applyAverSrQuality. Force Off for
            // this launch (never re-attempt the level that just crashed) rather than reset a
            // number (there's no single "the scale" to reset here), and latch it so Project
            // Settings/Display can say why AverSR reads Off.
            averSrCookieTripped_ = true;
            AVER_CRITICAL("[AverSR] the last launch did not survive applying {} -- forcing Off "
                          "for this launch. Choose a level again if that was not the cause.",
                          editor::averSrChoiceName(averSrChoice_));
            setPrefBool("display.renderScalePending", false);
            flushEditorPrefs();
            applyAverSrQuality(prefsDevice_, aver::sr::Quality::Off);
        } else if (averSrChoice_ == editor::AverSrChoice::Auto) {
            // Nothing here: updateAverSrAuto (onUpdate) applies Auto once a project exists to derive
            // a level from; this load runs before any project is open.
        } else {
            // Off/Quality/Balanced/Performance: applyAverSrQuality re-derives the level's own
            // canonical scale (aver::sr::renderScaleFor) rather than trusting a possibly-drifted
            // display.renderScale -- the quality owns the scale.
            const aver::sr::Quality q =
                static_cast<aver::sr::Quality>(editor::userLevelFor(averSrChoice_));
            if (q != aver::sr::Quality::Off) {
                // ARMED BEFORE THE FIRST NON-OFF APPLICATION (3.3 A): a named level can lose the
                // device exactly the way a raw Manual scale can -- same cookie, same reason.
                setPrefBool("display.renderScalePending", true);
                flushEditorPrefs();
                renderScaleCookieArmed_ = true;
            }
            if (q != averSrQuality_) applyAverSrQuality(prefsDevice_, q);
        }
    }
#endif  // AVER_MODULE_SR

    // Play toolbar/preferences. An out-of-range stored enum (an older build's editor.ini, or hand
    // edited) clamps to the compiled-in default rather than reading past the enum's own values.
    {
        const i32 storedMode = prefInt("play.mode", static_cast<i32>(playMode_));
        playMode_ = (storedMode >= 0 && storedMode <= static_cast<i32>(PlayMode::Standalone))
                  ? static_cast<PlayMode>(storedMode) : PlayMode::SelectedViewport;
        const i32 storedSpawn = prefInt("play.spawnAt", static_cast<i32>(playSpawnAt_));
        playSpawnAt_ = (storedSpawn >= 0 && storedSpawn <= static_cast<i32>(PlaySpawnAt::CameraLocation))
                     ? static_cast<PlaySpawnAt>(storedSpawn) : PlaySpawnAt::PlayerStart;
    }
    playGameGetsMouse_  = prefBool  ("play.gameGetsMouse",  playGameGetsMouse_);
    defaultPawnWalk_    = prefBool  ("play.defaultPawnWalk", defaultPawnWalk_);
    playStandaloneArgs_ = prefString("play.standaloneArgs", "");

    keybinds_.loadFromPrefs();
}

// Resolves the stored IDE name to an index, once the async scan has produced the list.
void SandboxApp::resolvePreferredIdeFromPrefs() {
    if (prefIdeName_.empty() || !editor::ideDetectionFinished()) return;
    const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
    for (usize i = 0; i < ides.size(); ++i)
        if (ides[i].name == prefIdeName_) { cbIdeChoice_ = static_cast<int>(i); break; }
    prefIdeName_.clear();
}


// Editor Preferences > Play: the toolbar dropdown's own settings (default mode, spawn location,
// mouse control, Standalone's extra command line), so "Advanced Settings..." lands somewhere that
// already has them.
void SandboxApp::buildPlayPrefsSection() {
    // Same one-shot pattern as scrollPrefsToKeybinds_: lands here, on the header, then clears.
    if (scrollPrefsToPlay_) { ImGui::SetScrollHereY(0.0f); scrollPrefsToPlay_ = false; }
    if (!ImGui::CollapsingHeader("Play", ImGuiTreeNodeFlags_DefaultOpen)) return;

    static const char* kModeNames[] = {"Selected Viewport", "Simulate", "Standalone Game"};
    const int modeIdx = static_cast<int>(playMode_);
    if (ImGui::BeginCombo("Default mode", kModeNames[modeIdx])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(kModeNames[i], modeIdx == i)) playMode_ = static_cast<PlayMode>(i);
        ImGui::EndCombo();
    }
    uiReg_.track("prefs.play.mode");

    static const char* kSpawnNames[] = {"Default Player Start", "Current Camera Location"};
    const int spawnIdx = static_cast<int>(playSpawnAt_);
    if (ImGui::BeginCombo("Spawn player at", kSpawnNames[spawnIdx])) {
        for (int i = 0; i < 2; ++i)
            if (ImGui::Selectable(kSpawnNames[i], spawnIdx == i)) playSpawnAt_ = static_cast<PlaySpawnAt>(i);
        ImGui::EndCombo();
    }
    uiReg_.track("prefs.play.spawnAt");

    ImGui::Checkbox("Game gets mouse control", &playGameGetsMouse_);
    uiReg_.track("prefs.play.gameGetsMouse");

    ImGui::Checkbox("Default pawn walks (no GameMode)", &defaultPawnWalk_);
    uiReg_.track("prefs.play.defaultPawnWalk");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With no GameMode, Play possesses the engine's default pawn. On: it walks -- a capsule "
                          "with gravity, 40 cm stair steps, Space to jump, Shift to run -- so stairs, decks and "
                          "bridges can be tested by hand. Off: it flies (WASD/QE).");

    editField("Standalone launch arguments", playStandaloneArgs_, 256);
    uiReg_.track("prefs.play.standaloneArgs");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Appended to AverEngineRuntime.exe's command line, after --project and "
                          "the level path.");

    // The Pause command's own chord IS the word "Pause" (its bound key's display name), so unlike
    // its neighbours here it needs no verb after it.
    ImGui::TextDisabled("%s play, %s simulate, %s eject/possess, %s pawn to camera (ejected), %s, %s stop, "
                        "%s release mouse",
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayStart)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlaySimulate)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayEject)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayPawnToCamera)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayPause)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayStop)).c_str(),
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayReleaseMouse)).c_str());
}

// Draws the Editor Preferences window: how this machine's editor behaves.
void SandboxApp::buildEditorPrefs() {
    resolvePreferredIdeFromPrefs();
    if (!showEditorPrefs_) return;
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    // 460 -> 640: six sections (the Keybinds table added a sixth) no longer fit the old height
    // on a typical monitor without immediately scrolling; still just a FirstUseEver default, so
    // anyone who has already resized this window keeps their own size.
    ImGui::SetNextWindowSize(ImVec2(560.0f*dpi_, 640.0f*dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
    if (!ImGui::Begin("Editor Preferences", &showEditorPrefs_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

    if (ImGui::CollapsingHeader("Content Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Open in gallery (tiles) view", &cbGallery_);
        ImGui::SliderFloat("Tile size", &cbTileSize_, 56.0f, 168.0f, "%.0f dp");
        ImGui::Checkbox("Double-click a folder to enter it", &cbDoubleClickEnter_);
        ImGui::TextDisabled("Single-click always selects; the tree navigates either way.");
        const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
        std::string label = cbIdeChoice_ < 0 || cbIdeChoice_ >= static_cast<int>(ides.size())
                          ? "Automatic (" + editor::preferredIde().name + ")"
                          : ides[static_cast<usize>(cbIdeChoice_)].name;
        if (ImGui::BeginCombo("Open source in", label.c_str())) {
            if (ImGui::Selectable("Automatic", cbIdeChoice_ < 0)) cbIdeChoice_ = -1;
            for (usize i = 0; i < ides.size(); ++i)
                if (ImGui::Selectable(ides[i].name.c_str(), cbIdeChoice_ == static_cast<int>(i)))
                    cbIdeChoice_ = static_cast<int>(i);
            ImGui::EndCombo();
        }
        if (!editor::ideDetectionFinished())
            ImGui::TextDisabled("Still scanning for installed IDEs...");
    }
    if (ImGui::CollapsingHeader("Drawers", ImGuiTreeNodeFlags_DefaultOpen)) {
        f32 pct = drawerFrac_ * 100.0f;
        if (ImGui::SliderFloat("Height", &pct, 14.0f, 88.0f, "%.0f%% of the window"))
            drawerFrac_ = pct / 100.0f;
        ImGui::SliderFloat("Slide speed", &drawerRate_, 4.0f, 40.0f, "%.0f");
        ImGui::TextDisabled("Ctrl+Space opens the Content Browser; Esc dismisses a drawer.");
    }
    if (ImGui::CollapsingHeader("Output Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Auto-scroll to the newest line", &logAutoScroll_);
        // Same persisted variable as the Output Log drawer's own combo, so it must offer the same
        // entries -- this copy once lagged behind when Error+/Critical+ were added to the other one:
        // picking Error+ there left this preview blank, and this combo could then only set the
        // filter back to one of the first three.
        ImGui::Combo("Level filter", &logLevelFilter_, "All\0Info+\0Warn+\0Error+\0Critical+\0");
    }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool canDisable = prefsDevice_ && prefsDevice_->vsyncCanDisable();
        bool vs = prefsDevice_ ? prefsDevice_->vsync() : true;
        ImGui::BeginDisabled(!canDisable);
        if (ImGui::Checkbox("V-Sync", &vs) && prefsDevice_) prefsDevice_->setVSync(vs);
        ImGui::EndDisabled();
        if (!canDisable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("This display path cannot tear, so vsync cannot be turned off.\n"
                              "Needs DXGI tearing support (DXGI 1.5+).");
        ImGui::SameLine();
        ImGui::TextDisabled(vs ? "(capped to the refresh rate)" : "(uncapped, may tear)");

        // Frame generation while editing (docs/rendering/FRAME_INTERPOLATION.md, decision 8). Play
        // follows the project's Frame generation setting instead; --frame-gen outranks both.
        ImGui::Checkbox("Frame generation in viewport while editing", &frameGenWhileEditing_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shows a generated frame between every two rendered ones: twice the frames\n"
                              "on screen for the same rendering work, at about half a frame of extra delay.\n"
                              "Needs V-Sync on and 1x anti-aliasing. Play uses the project's setting instead.");
        if (frameGenWhileEditing_ && prefsDevice_) {
            ImGui::SameLine();
            ImGui::TextDisabled(!prefsDevice_->vsync()       ? "(paused: needs V-Sync)"
                                : prefsDevice_->sampleCount() > 1 ? "(paused: needs 1x anti-aliasing)"
                                : prefsDevice_->frameGenerated() ? "(running)"
                                                                 : "(starting)");
        }

        // The path the in-between frame's pixels are gathered along (FRAME_INTERPOLATION.md §3.5).
        static const char* kPaths[] = {"Straight lines", "Quadratic (procedural)", "Learned (neural)"};
        int path = frameGenTrajectory_ < 0 ? 0 : (frameGenTrajectory_ > 2 ? 2 : frameGenTrajectory_);
        if (ImGui::Combo("Frame generation path", &path, kPaths, 3)) frameGenTrajectory_ = path;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How a pixel is followed between two rendered frames. Straight lines: constant\n"
                              "motion. Quadratic: bends with the last two frames' motion (exact for steady\n"
                              "acceleration). Learned: a small network predicts the bend; it uses the\n"
                              "quadratic until it has trained. Applies while editing and in Play.");
        ImGui::Checkbox("Train the learned path while frame generation runs", &frameGenTrain_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Learns from the frames you are already rendering: about 0.5 ms a frame while on.\n"
                              "Progress is saved every 500 steps beside the editor's settings and kept\n"
                              "between sessions; learning slows down as it accumulates, so leave it on as\n"
                              "long as you like.");
        if (frameGenerator_ && (frameGenTrain_ || frameGenTrajectoryCli_ == 2 || frameGenTrajectory_ == 2)) {
            const framegen::ProceduralFrameGenerator::TrainingStatus st = frameGenerator_->trainingStatus();
            ImGui::TextDisabled("Learned path: %llu steps (learning rate %.1e), %s",
                                static_cast<unsigned long long>(st.lifetimeSteps), st.learningRate,
                                st.networkInUse ? "in use"
                                : st.evaluated && !st.networkBeatsQuadratic ? "quadratic stands in (it is still better)"
                                                                            : "quadratic stands in until trained");
            if (st.evaluated)
                ImGui::TextDisabled("Measured in-between position error (smoothed): straight %.3f px, "
                                    "quadratic %.3f px, learned %.3f px", st.errLinear, st.errAnalytic,
                                    st.errNetwork);
        }

#if AVER_MODULE_SR
        // Every Overall rung has an AverSR default (QualityLadder.hpp's ladder::averSrLevel), so
        // this combo picks a SOURCE for the level, not the level itself: Auto follows the CLI >
        // Display choice > project manifest > ladder chain that updateAverSrAuto resolves every
        // frame (and can move under it, e.g. a scalability button moving the Overall rung); the
        // four named items pin one level outright; "Manual scale" is what the slider below sets.
        //
        // Auto's label is recomputed fresh each draw (cheap; not an every-frame window) rather than
        // read off averSrQuality_/averSrSource_, which reflect the CURRENT choice, not a preview of
        // what picking Auto would resolve to.
        std::string autoLabel = "Auto";
#if AVER_MODULE_VOXI
        if (voxiAttached_) {
            voxi::Renderer& vxr = voxi::Renderer::get();
            const int cliLevel = (averSrFromCli_ && !averSrCliAuto_) ? averSrCliLevel_ : -1;
            const voxi::AverSrDecision preview = voxi::resolveAverSrLevel(
                cliLevel, -1, averSrProjectDefault_,
                voxi::autoAverSrLevel(vxr.settings(), vxr.deviceInfo()));
            autoLabel = std::string("Auto (") +
                       aver::sr::qualityName(static_cast<aver::sr::Quality>(preview.level)) +
                       " from " + averSrSourceText(preview.source) + ")";
        }
#endif
        static const char* kAverSrItems[] = {"", "Off", "Quality", "Balanced", "Performance", "Manual scale"};
        const int curIdx = static_cast<int>(averSrChoice_);
        const char* curLabel = curIdx == 0 ? autoLabel.c_str() : kAverSrItems[curIdx];
        if (ImGui::BeginCombo("AverSR", curLabel)) {
            for (int i = 0; i < 6; ++i) {
                const bool sel = curIdx == i;
                const char* itemLabel = i == 0 ? autoLabel.c_str() : kAverSrItems[i];
                if (ImGui::Selectable(itemLabel, sel)) {
                    averSrMigrationNoteArmed_ = false;   // 3.3 A: cleared the moment ANY item is picked
                    // An explicit pick lifts the crash-cookie latch -- the CRITICAL message tells the
                    // user to "choose a level again", which did nothing while it stayed forced Off.
                    averSrCookieTripped_ = false;
                    averSrChoice_ = static_cast<editor::AverSrChoice>(i);
                    // Auto and Manual apply nothing here: Auto is picked up by updateAverSrAuto next
                    // frame (needs vx.settings()/deviceInfo(), unavailable mid-UI-draw); Manual keeps
                    // whatever scale the slider below already holds.
                    const int lvl = editor::userLevelFor(averSrChoice_);
                    if (lvl >= 0 && prefsDevice_)
                        applyAverSrQuality(prefsDevice_, static_cast<aver::sr::Quality>(lvl));
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Aver Super Resolution: renders the scene smaller and resamples it\n"
                              "back up with a bicubic Catmull-Rom filter. Off is bit-identical to\n"
                              "no AverSR at all. See docs/AVERSR.md.\n"
                              "\n"
                              "Auto follows the Overall preset's own AverSR default and moves with\n"
                              "it; the named levels and Manual scale pin one choice regardless of\n"
                              "the preset.\n"
                              "\n"
                              "The 5.0ms-at-Off / 2.3-1.7-1.3ms figure this tooltip used to quote\n"
                              "was recorded pre-ReSTIR/pre-denoiser; aver-aversr-measured.md records\n"
                              "this figure as unverified.");
        if (averSrMigrationNoteArmed_)
            ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                "AverSR now defaults to Auto (%s). Choose Off for native resolution.",
                aver::sr::qualityName(averSrQuality_));
#endif
        // Render scale: the 3D scene's resolution as a fraction of the window's. 1.0 (right edge)
        // is 1:1; below that trades scene sharpness for pixel-bound pass cost. The UI never moves.
        float rs = prefsDevice_ ? prefsDevice_->renderScale() : 1.0f;
        if (ImGui::SliderFloat("Render Scale", &rs, 0.25f, 1.0f, "%.2f") && prefsDevice_) {
            prefsDevice_->setRenderScale(rs);
#if AVER_MODULE_SR
            // Dragging this slider is what "Manual scale" means -- a number no named level produced.
            // Clears the migration note too: it's as deliberate a choice as picking a combo item.
            averSrMigrationNoteArmed_ = false;
            averSrChoice_ = editor::AverSrChoice::Manual;
#endif
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Renders the 3D scene at a fraction of the window's resolution, then\n"
                              "upscales it back for display. The editor UI stays crisp either way.");
    }
    if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show grid", &showGrid_);
        uiReg_.track("prefs.viewport.showGrid");
        ImGui::Checkbox("Wireframe", &wireframe_);
        uiReg_.track("prefs.viewport.wireframe");
        // The other three view flags sat beside the two above in the Show menu but weren't
        // remembered (e.g. turning off Atmosphere didn't survive a restart). Same pattern as above.
        ImGui::Checkbox("Unlit", &unlit_);
        uiReg_.track("prefs.viewport.unlit");
        ImGui::Checkbox("Show static meshes", &showStaticMeshes_);
        uiReg_.track("prefs.viewport.showStaticMeshes");
        ImGui::Checkbox("Show atmosphere", &showAtmosphere_);
        uiReg_.track("prefs.viewport.showAtmosphere");
        // Same dial as the viewport chip, not the raw rate -- one setting shown in two units reads
        // as two settings otherwise, and this is the page you check when the chip's number surprises you.
        {
            f32 dial = flySpeed_ / kCamSpeedUnit;
            if (ImGui::SliderFloat("Fly speed", &dial, 20.0f / kCamSpeedUnit,
                                   20000.0f / kCamSpeedUnit, "%.2f", ImGuiSliderFlags_Logarithmic))
                flySpeed_ = dial * kCamSpeedUnit;
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f cm/s)", flySpeed_);
        }
        uiReg_.track("prefs.viewport.flySpeed");
        ImGui::SliderFloat("Look sensitivity", &lookSpeed_, 0.001f, 0.02f, "%.4f");
        uiReg_.track("prefs.viewport.lookSensitivity");

        // Snap: the toolbar could set it but nothing kept it -- moveSnap_/rotSnap_/scaleSnap_ and
        // their toggles weren't in load/saveEditorPreferences, so a grid setup didn't survive a restart.
        ImGui::Separator();
        ImGui::TextDisabled("Snap");
        ImGui::Checkbox("Move", &snapMove_);   uiReg_.track("prefs.snap.move");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * dpi_);
        ImGui::DragFloat("##moveSnap", &moveSnap_, 0.1f, 0.01f, 10000.0f, "%.2f units");
        uiReg_.track("prefs.snap.moveStep");
        ImGui::Checkbox("Rotate", &snapRot_);  uiReg_.track("prefs.snap.rotate");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * dpi_);
        ImGui::DragFloat("##rotSnap", &rotSnap_, 0.5f, 0.1f, 180.0f, "%.1f deg");
        uiReg_.track("prefs.snap.rotateStep");
        ImGui::Checkbox("Scale", &snapScale_); uiReg_.track("prefs.snap.scale");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * dpi_);
        ImGui::DragFloat("##scaleSnap", &scaleSnap_, 0.01f, 0.01f, 10.0f, "%.2f");
        uiReg_.track("prefs.snap.scaleStep");
    }
    // --scroll-prefs-to-keybinds: one-shot verification aid for scripted runs (six DefaultOpen
    // sections don't fit one screen and there's no human to scroll). Consumes its own flag once,
    // so it never fights a person who scrolls the window themselves.
    if (ImGui::CollapsingHeader("Derived Data Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Baked global illumination, cached beside the project under");
        ImGui::TextDisabled("DerivedDataCache\\GI. Derived data: deleting it costs one rebuild.");
#if AVER_MODULE_VOXI
        // In MB, since bytes aren't glanceable. Floor is one volume's worth -- below that every
        // bake flushes immediately and the buffer adds nothing but an extra copy.
        if (ImGui::SliderFloat("Memory budget", &ddcRamBudgetMb_, 32.0f, 4096.0f, "%.0f MB",
                               ImGuiSliderFlags_Logarithmic))
            voxiRenderer_.setGiCacheRamBudget(static_cast<u64>(ddcRamBudgetMb_) * 1024ull * 1024ull);
        uiReg_.track("prefs.ddc.ramBudget");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only settled bakes are kept: dragging the sun or a material slider\n"
                              "adds one volume when you let go, not one per frame. They are held\n"
                              "in memory and go to disk in batches -- when this is exceeded, and\n"
                              "when the editor closes. Lowering it below what is already held\n"
                              "writes immediately.");

        const u64 pend = voxiRenderer_.giCachePendingBytes();
        ImGui::Text("Buffered: %llu MB in %u entr(ies)",
                    static_cast<unsigned long long>(pend / (1024 * 1024)),
                    voxiRenderer_.giCachePendingCount());
        ImGui::BeginDisabled(pend == 0);
        if (ImGui::Button("Write to disk now")) {
            const u32 wrote = voxiRenderer_.giCacheFlush();
            // A count, not "done": flushing nothing and flushing 400 volumes look identical
            // from the button, and the difference is the whole reason to press it.
            notifyOutcome(wrote ? editor::NotifySeverity::Success : editor::NotifySeverity::Info,
                         wrote ? "GI cache written" : "Nothing to write",
                         wrote ? std::to_string(wrote) + " volume(s) flushed to disk"
                               : "Every bake is already on disk.");
        }
        ImGui::EndDisabled();
        uiReg_.track("prefs.ddc.flushNow");
        if (pend == 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Nothing is buffered -- every bake is already on disk.");
#else
        ImGui::TextDisabled("The Voxi render module is not in this build, so nothing bakes.");
#endif
    }

    // Before Keybinds so its own shortcuts line names commands the section right below already
    // shows in full.
    buildPlayPrefsSection();

    // Scroll lands HERE, on the section it names. It used to fire above the DDC header instead, so
    // --scroll-prefs-to-keybinds put DDC at the top and left Keybinds below the fold -- unnoticed,
    // since a screenshot of the wrong section still looks like a screenshot of a section.
    if (scrollPrefsToKeybinds_) { ImGui::SetScrollHereY(0.0f); scrollPrefsToKeybinds_ = false; }
    // Called whether or not it is open, passing which. Collapsing the header mid-capture used to
    // strand `listening_` (the row stayed "Press a chord..." on reopen); the registry now owns clearing it.
    const bool keybindsOpen = ImGui::CollapsingHeader("Keybinds", ImGuiTreeNodeFlags_DefaultOpen);
    keybinds_.drawPreferencesSection(dpi_, keybindsOpen, &uiReg_);
    ImGui::Separator();
    ImGui::TextDisabled("Preferences apply immediately and are saved for next time.");
    ImGui::TextDisabled("%s", editor::editorPrefsPath().c_str());
    ImGui::End();

    saveEditorPreferences();
}

// Draws the Project Settings window: a category sidebar beside the selected settings page.
// World Settings are per-LEVEL vs Project Settings' per-project (one project often holds a menu,
// gameplay and test level needing different rules). Every value here edits the level file
// directly -- no parallel setting, avoiding the two-sources-of-truth trap the Player Start marker avoids.
void SandboxApp::buildWorldSettings() {
    if (!showWorldSettings_) return;
#if !AVER_MODULE_SCENE
    // Without the scene module there's no level to configure (levelPath_/levelName_ are themselves
    // scene-guarded); saying so beats silently hiding the menu entry.
    if (ImGui::Begin("World Settings", &showWorldSettings_, ImGuiWindowFlags_NoDocking))
        ImGui::TextDisabled("This build has no scene module, so there is no level to configure.");
    ImGui::End();
    return;
#else

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 480.0f * dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("World Settings", &showWorldSettings_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    if (levelPath_.empty()) {
        ImGui::TextDisabled("No level is open.");
        ImGui::TextWrapped("World Settings edit the level file. Open or create a level first.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Level");
    ImGui::Separator();
    ImGui::Text("%s", levelName_.empty() ? "(untitled)" : levelName_.c_str());
    ImGui::TextDisabled("%s", levelPath_.c_str());
    ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

    // ---- GameMode override ----
    ImGui::TextDisabled("Game Mode");
    ImGui::Separator();
    {
        // By name: that's what the file stores and what survives a restart. Framework class handles
        // from aver_fw_class_declare are process-local, so a number here would mean something else next launch.
        char buf[128];
        const std::string& gm = levelHeader_.gameMode;
        std::snprintf(buf, sizeof buf, "%s", gm.c_str());
        ImGui::SetNextItemWidth(320.0f * dpi_);
        if (ImGui::InputText("GameMode Override", buf, sizeof buf)) {
            levelHeader_.gameMode = buf;
        }
        uiReg_.track("worldSettings.gameMode");

        // Says whether the name resolves, rather than leaving a typo to be discovered on Play.
        // A blank field is not an error -- it means "use the project default".
#if AVER_MODULE_FRAMEWORK
        if (levelHeader_.gameMode.empty()) {
            ImGui::TextDisabled("Empty -- the project's default GameMode applies.");
        } else {
            const int32_t c = aver_fw_class_find(levelHeader_.gameMode.c_str());
            if (c == 0) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                   "No class named '%s' is declared.", levelHeader_.gameMode.c_str());
                ImGui::TextDisabled("Compile .NET first, or check the spelling.");
            } else if ((aver_fw_class_get_flags(c) & AVER_FW_CLASS_GAME_MODE) == 0) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                   "'%s' exists but is not a GameMode.", levelHeader_.gameMode.c_str());
            } else {
                ImGui::TextDisabled("Resolved.");
            }
        }
#else
        ImGui::TextDisabled("This build has no framework module, so the name cannot be checked.");
#endif
    }
    ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

    // ---- Player Start ----
    ImGui::TextDisabled("Player Start");
    ImGui::Separator();
    {
        Vec3 sp{}; f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            ImGui::Text("(%.0f, %.0f, %.0f)  yaw %.0f", sp.x, sp.y, sp.z, sy);
#if AVER_MODULE_SCENE
            if (playerStart_ != scene::kInvalidEntity) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Select")) { sel_ = kSelScene; selEntity_ = playerStart_; }
                uiReg_.track("worldSettings.selectPlayerStart");
            }
#endif
            ImGui::TextDisabled("Where the player spawns in. Move the marker to change it.");
        } else {
            ImGui::TextDisabled("None. Add one with Add > Player Start.");
        }
    }
    ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

    // ---- Streaming, per density field ----
    ImGui::TextDisabled("World Streaming");
    ImGui::Separator();
    if (levelPcgVolumes_.empty()) {
        ImGui::TextDisabled("This level declares no PCGVOLUME, so nothing streams.");
    } else {
        // One row per field: radius is per field on purpose -- a single level-wide radius can't
        // serve a dense ground cover and a sparse canopy at once.
        ImGui::TextDisabled("Radius is per density field, in 16m chunks. Cost is quadratic in it.");
        for (usize i = 0; i < levelPcgVolumes_.size(); ++i) {
            fmt::OcPcgVolume& v = levelPcgVolumes_[i];
            if (v.name == "Sky") continue;   // the cloud field; it streams nothing
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", v.name.empty() ? "(unnamed)" : v.name.c_str());

            int radius = v.radiusChunks > 0 ? v.radiusChunks : 3;   // 0 = "runtime default (3)"
            ImGui::SetNextItemWidth(160.0f * dpi_);
            if (ImGui::SliderInt("Radius (chunks)", &radius, 1, 24)) {
                v.radiusChunks = radius;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%.0fm", static_cast<f64>(radius) * 16.0);

            int samples = v.samplesPerAxis > 0 ? v.samplesPerAxis : 4;
            ImGui::SetNextItemWidth(160.0f * dpi_);
            if (ImGui::SliderInt("Samples / axis", &samples, 1, 32)) {
                v.samplesPerAxis = samples;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d candidates/chunk", samples * samples);
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4.0f * dpi_));
        }
        ImGui::TextDisabled("Takes effect when streaming is next enabled (Window > Chunk Streaming).");
    }

    ImGui::End();
#endif
}

void SandboxApp::buildProjectSettings() {
    if (focusVoxi_ > 0) { showProjectSettings_ = true; --focusVoxi_; } // --project-settings
    if (!showProjectSettings_) return;

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(880.0f*dpi_, 560.0f*dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
    if (!ImGui::Begin("Project Settings", &showProjectSettings_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

    // settingsPage_: 0 Description, 1 Rendering>General, 2 >Global Illumination,
    // 3 >Ray Tracing (denoiser lives here, next to the rays it thins out), 4 >Path Tracing.
    ImGui::BeginChild("##categories", ImVec2(220.0f*dpi_, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Project");
    ImGui::Indent();
    if (ImGui::Selectable("Description", settingsPage_==0)) settingsPage_=0;
    ImGui::Unindent();
    ImGui::TextDisabled("Engine");
    ImGui::Indent();
    ImGui::TextDisabled("Rendering");
    ImGui::Indent();
    if (ImGui::Selectable("General",               settingsPage_==1)) settingsPage_=1;
    if (ImGui::Selectable("Global Illumination",    settingsPage_==2)) settingsPage_=2;
    if (ImGui::Selectable("Ray Tracing",            settingsPage_==3)) settingsPage_=3;
    if (ImGui::Selectable("Path Tracing",           settingsPage_==4)) settingsPage_=4;
    ImGui::Unindent();
    // Siblings of Rendering, not children: neither is drawn by the renderer.
    if (ImGui::Selectable("Physics",                settingsPage_==5)) settingsPage_=5;
    if (ImGui::Selectable("Audio",                  settingsPage_==6)) settingsPage_=6;
    // Three categories a project couldn't state until now, each a real options struct the engine
    // already had but no file could reach: platform::WindowDesc, the importers' options, world::StreamSettings.
    if (ImGui::Selectable("Window",                 settingsPage_==7)) settingsPage_=7;
    if (ImGui::Selectable("Import Defaults",        settingsPage_==8)) settingsPage_=8;
    if (ImGui::Selectable("World Streaming",        settingsPage_==9)) settingsPage_=9;
    ImGui::Unindent();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (settingsPage_ == 0) {
        ImGui::TextUnformatted("Description");
        ImGui::Separator();
        if (project_.valid()) {
            // Editable at last: this page was five ImGui::Text lines, so name/author/start map
            // couldn't be changed from the editor -- saveProjectManifest already wrote them via
            // writeOcproject, which owns NAME/AUTHOR/CONTENT/STARTMAP. Only the widgets were missing.
            ImGui::PushItemWidth(-160.0f * dpi_);
            if (editField("Name", project_.name, 96)) projectDirty_ = true;
            uiReg_.track("project.name");
            // Rename is manifest-only: folder/filename keep the old name, and project_.name feeds
            // csharpNamespaceFor(), so new scripts/materials get a namespace existing ones don't.
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Renames the project in the manifest only.\nThe folder and file keep their names, and new scripts get a namespace from this.");
            if (editField("Author", project_.author, 96)) projectDirty_ = true;
            uiReg_.track("project.author");
            if (editField("Start map", project_.startMap, 256)) projectDirty_ = true;
            uiReg_.track("project.startMap");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Relative to the content root, e.g. Maps/Default.ocmap");

            // A combo, not free text like Start map: INPUT.SCHEME names a scheme by content-relative
            // path, and a typo here fails SILENTLY at runtime (no context pushed), unlike a bad Start
            // Map which at least errors. Rescanned each time it opens (same pattern as
            // refreshFoliagePalette in SandboxViewport.cpp); fine for a handful of .ocinput files.
            {
                const std::string curLabel = project_.inputScheme.empty() ? "None" : project_.inputScheme;
                if (ImGui::BeginCombo("Input Scheme", curLabel.c_str())) {
                    if (ImGui::Selectable("None", project_.inputScheme.empty())) {
                        project_.inputScheme.clear();
                        projectDirty_ = true;
                    }
                    const std::string dir = project_.contentDir();
                    std::error_code ec;
                    if (!dir.empty() && std::filesystem::exists(dir, ec)) {
                        std::vector<std::string> choices;
                        for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
                             it != end; it.increment(ec)) {
                            if (ec) break;
                            if (!it->is_regular_file(ec)) continue;
                            std::string ext = it->path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (ext != ".ocinput") continue;
                            std::error_code relEc;
                            std::string rel = std::filesystem::relative(it->path(), dir, relEc).string();
                            if (relEc || rel.empty()) continue;
                            for (char& c : rel) if (c == '\\') c = '/';
                            choices.push_back(std::move(rel));
                        }
                        std::sort(choices.begin(), choices.end());
                        for (const std::string& c : choices)
                            if (ImGui::Selectable(c.c_str(), c == project_.inputScheme)) {
                                project_.inputScheme = c;
                                projectDirty_ = true;
                            }
                    }
                    ImGui::EndCombo();
                }
                uiReg_.track("project.inputScheme");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The .ocinput scheme EnhancedInput pushes as this "
                                       "project's default bindings.");
            }
            ImGui::PopItemWidth();

            // Read-only on purpose: ENGINE is a requirement, not a preference, and changing CONTENT
            // mid-session would move where every asset resolves from with nothing re-mounting.
            ImGui::Text("Engine      %s %s (this build: %.*s)",
                        project_.engineName.empty() ? "(unset)" : project_.engineName.c_str(),
                        project_.engineMinVersion.empty() ? "" : project_.engineMinVersion.c_str(),
                        (int)kEngineVersion.size(), kEngineVersion.data());
            ImGui::Text("Content     %s", project_.contentRoot.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Read-only: every asset path resolves from here, and nothing re-mounts on a change.");
            ImGui::Separator();
            ImGui::TextDisabled("%s", project_.manifestPath.c_str());
            ImGui::Spacing();
            if (project_.hasRenderSettings())
                ImGui::TextDisabled("This project states render settings; they were applied on open.");
            else
                ImGui::TextDisabled("This project states no render settings yet.");
            ImGui::Spacing();

            // The Save/Revert pair moved to a FOOTER shared by every page -- see the end of
            // buildProjectSettings for why it could not stay here.
            ImGui::TextDisabled("Rendering settings are the game's, so they go in the project.");
            ImGui::TextDisabled("The editor's own preferences are per-machine and do not.");
        } else {
            ImGui::TextDisabled("No project loaded. The editor runs fine without one; open or");
            ImGui::TextDisabled("create a project from the start screen to populate this page.");
        }
    } else if (settingsPage_ == 5) {
        buildPhysicsSettings();
    } else if (settingsPage_ == 6) {
        buildAudioSettings();
    } else if (settingsPage_ == 7) {
        buildWindowSettings();
    } else if (settingsPage_ == 8) {
        buildImportSettings();
    } else if (settingsPage_ == 9) {
        buildStreamSettings();
    } else {
#if AVER_MODULE_VOXI
        buildRenderingSettings(settingsPage_);
#else
        ImGui::TextUnformatted("Rendering");
        ImGui::Separator();
        ImGui::TextDisabled("Built without the Voxi render module (-DAVER_MODULE_VOXI=OFF).");
#endif
    }
    ImGui::EndChild();

    // ---- SAVE FOOTER, ON EVERY PAGE ----
    // Shared because every page sets projectDirty_ (gravity, audio mix, every render knob), and a
    // save button on only one page meant edits elsewhere were lost on window close. Writing on
    // every drag was rejected as noise for a source-controlled .ocproject, but losing edits lost
    // worse -- see maybeAutosaveProject, which now debounces a write half a second after the last
    // change. The button stays for a deliberate save; the dirty mark shows the window before it fires.
    if (project_.valid()) {
        ImGui::Separator();
        ImGui::BeginDisabled(!projectDirty_);
        if (ImGui::Button("Save Project Settings", ImVec2(220.0f * dpi_, 0.0f))) {
            std::string why;
            if (saveProjectManifest(&why)) {
                projectSaveStatus_ = "Saved.";
            } else {
                projectSaveStatus_ = "Save failed: " + why;
                notifyOutcome(editor::NotifySeverity::Error, "Could not save project settings",
                              why, true);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(projectDirty_
                ? "Writes EVERY page into the .ocproject.\nComments and any keys this build does not know are preserved."
                : "Nothing has changed since the manifest was last read.");

        // Revert re-reads the file rather than keeping an undo stack -- a manifest is small enough
        // that this stays exact where a remembered snapshot would drift as fields are added.
        ImGui::SameLine();
        ImGui::BeginDisabled(!projectDirty_);
        if (ImGui::Button("Revert", ImVec2(110.0f * dpi_, 0.0f))) {
            fmt::ProjectDesc fresh;
            std::string why;
            if (fmt::loadOcproject(project_.manifestPath, fresh, &why)) {
                project_ = fresh;
                projectDirty_ = false;
                projectSaveStatus_ = "Reverted to the file on disk.";
            } else {
                projectSaveStatus_ = "Revert failed: " + why;
                notifyOutcome(editor::NotifySeverity::Error, "Could not revert project settings",
                              why, true);
            }
        }
        ImGui::EndDisabled();

        // THE DIRTY MARK ITSELF. Without it the only signal was a button on another page going
        // from grey to enabled, which nobody watches.
        if (projectDirty_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "unsaved changes");
        }
        if (!projectSaveStatus_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", projectSaveStatus_.c_str());
        }
    }
    ImGui::End();
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_VOXI
// A feature's status badge: green ready, amber not implemented, red unsupported.
 void SandboxApp::featureStatusBadge(aver::voxi::Renderer& vx, aver::voxi::Feature f) {
    using namespace aver::voxi;
    const Status st = vx.status(f);
    const ImVec4 col = st==Status::Ready ? ImVec4(0.45f,0.85f,0.45f,1)
                     : st==Status::NotImplemented ? ImVec4(0.95f,0.72f,0.25f,1)
                                                  : ImVec4(0.75f,0.35f,0.35f,1);
    ImGui::SameLine(); ImGui::TextColored(col, "[%s]", vx.statusText(f));
}
#endif  // AVER_MODULE_VOXI

// ---- THE FIVE PAGES THAT AREN'T THE RENDERING PAGE ----
// Window/Import/Streaming/Physics/Audio don't mention a voxel (they edit WINDOW.*/IMPORT.*/
// STREAM.*/PHYSICS.*/AUDIO.* keys) but used to sit inside `#if AVER_MODULE_VOXI` just because
// buildRenderingSettings did -- so -DAVER_MODULE_VOXI=OFF (which -DAVER_MODULE_PBR=OFF forces too)
// deleted their five definitions while buildSettings()'s else-if chain (gated on ImGui alone)
// still called them. The guard now wraps only the Rendering page, where it belongs.
//
// Helper for the pages below: an int field UNSTATED at -1, not 0 (the RENDER.*/STREAM.* convention).
// Without the checkbox a project can't author "no value stated" and pins defaults it never meant to.
bool SandboxApp::settingInt(const char* label, int* v, int lo, int hi, int whenEnabled, const char* tip) {
#if AVER_WITH_IMGUI
    ImGui::PushID(label);
    bool stated = (*v >= 0);
    bool changed = false;
    if (ImGui::Checkbox("##stated", &stated)) { *v = stated ? whenEnabled : -1; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unticked: this project does not state it,\nand the engine's own default applies.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!stated);
    ImGui::SetNextItemWidth(180.0f * dpi_);
    if (ImGui::SliderInt(label, v, lo, hi)) changed = true;
    ImGui::EndDisabled();
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    if (changed) projectDirty_ = true;
    return changed;
#else
    (void)label; (void)v; (void)lo; (void)hi; (void)whenEnabled; (void)tip; return false;
#endif
}

// ---- Window: how a shipped game presents itself ----
// docs/PACKAGING.md named the gap: ".ocproject cannot describe a shipped game... no entry point,
// no window/resolution defaults, no build id, no icon." The old design's side-car game.json never had a reader or writer.
void SandboxApp::buildWindowSettings() {
#if AVER_WITH_IMGUI
    ImGui::TextUnformatted("Window");
    ImGui::Separator();
    if (!project_.valid()) { ImGui::TextDisabled("No project loaded."); return; }

    ImGui::TextDisabled("What a PACKAGED GAME opens as. The editor ignores all of it -- its own");
    ImGui::TextDisabled("window is an editor concern and lives in editor.ini.");
    ImGui::Spacing();

    {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", project_.windowTitle.c_str());
        ImGui::SetNextItemWidth(320.0f * dpi_);
        if (ImGui::InputTextWithHint("Title", project_.name.c_str(), buf, sizeof buf)) {
            project_.windowTitle = buf;
            projectDirty_ = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty uses the project's NAME.");
    }
    {
        int wh[2] = {project_.windowWidth > 0 ? project_.windowWidth : 1280,
                     project_.windowHeight > 0 ? project_.windowHeight : 720};
        bool stated = project_.windowWidth > 0 && project_.windowHeight > 0;
        ImGui::PushID("size");
        if (ImGui::Checkbox("##stated", &stated)) {
            project_.windowWidth  = stated ? wh[0] : -1;
            project_.windowHeight = stated ? wh[1] : -1;
            projectDirty_ = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!stated);
        ImGui::SetNextItemWidth(220.0f * dpi_);
        if (ImGui::InputInt2("Size", wh)) {
            project_.windowWidth  = wh[0];
            project_.windowHeight = wh[1];
            projectDirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    settingInt("Resizable", &project_.windowResizable, 0, 1, 1, "0 or 1.");
    settingInt("Fullscreen", &project_.windowFullscreen, 0, 1, 0,
               "Borderless-fullscreen INTENT. Recorded now; WindowDesc has no fullscreen\n"
               "field yet, so nothing consumes it -- see the note below.");
    ImGui::Spacing();
    ImGui::TextDisabled("Title, size and resizable reach platform::WindowDesc through the game's");
    ImGui::TextDisabled("BootConfig. FULLSCREEN IS RECORDED BUT NOT YET APPLIED: WindowDesc is");
    ImGui::TextDisabled("{title,width,height,resizable,activate} and has no fullscreen field.");
#endif
}

// ---- Import defaults: what the asset compiler assumes when a file does not say ---------------
void SandboxApp::buildImportSettings() {
#if AVER_WITH_IMGUI
    ImGui::TextUnformatted("Import Defaults");
    ImGui::Separator();
    if (!project_.valid()) { ImGui::TextDisabled("No project loaded."); return; }

    ImGui::TextDisabled("What an import assumes when the source file does not say. Every importer");
    ImGui::TextDisabled("already had these as an options struct; none of them was reachable from");
    ImGui::TextDisabled("a project, so \"this project's art is in metres\" had to be re-typed on");
    ImGui::TextDisabled("every command line.");
    ImGui::Spacing();

    {
        f32 v = project_.importScale >= 0.0f ? project_.importScale : 100.0f;
        bool stated = project_.importScale >= 0.0f;
        ImGui::PushID("scale");
        if (ImGui::Checkbox("##stated", &stated)) { project_.importScale = stated ? v : -1.0f; projectDirty_ = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!stated);
        ImGui::SetNextItemWidth(180.0f * dpi_);
        if (ImGui::DragFloat("Unit scale", &v, 0.5f, 0.001f, 10000.0f, "%.3f")) {
            project_.importScale = v;
            projectDirty_ = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Source unit -> centimetres. 100 = the source is in metres,\n"
                              "which is what glTF, USD and most DCCs export.");
        ImGui::PopID();
    }
    settingInt("Convert axes", &project_.importConvertAxes, 0, 1, 1,
               "Y-up right-handed -> this engine's Z-up left-handed.");
    settingInt("Generate normals", &project_.importGenNormals, 0, 1, 1,
               "Synthesise normals a source file omitted.");
    settingInt("Generate mips", &project_.importGenMips, 0, 1, 1, nullptr);
    settingInt("Max texture (px)", &project_.importMaxTexture, 0, 8192, 2048,
               "Downscale ceiling. 0 means no cap.\n"
               "A 4K import is tens of MB per texture once mipped.");
    ImGui::Spacing();
    ImGui::TextDisabled("RECORDED, NOT YET CONSUMED: AverAssetC takes these on its command line");
    ImGui::TextDisabled("and does not read the manifest. The keys exist so a project can state");
    ImGui::TextDisabled("them and they survive a save; wiring the compiler to them is separate.");
#endif
}

// ---- World streaming budgets ----------------------------------------------------------------
void SandboxApp::buildStreamSettings() {
#if AVER_WITH_IMGUI
    ImGui::TextUnformatted("World Streaming");
    ImGui::Separator();
    if (!project_.valid()) { ImGui::TextDisabled("No project loaded."); return; }

    ImGui::TextDisabled("world::StreamSettings -- the knobs a shipping title tunes per platform.");
    ImGui::Spacing();
    settingInt("Load radius",     &project_.streamLoadRadius,     1, 16, 3, "Chunks loaded around the camera.");
    settingInt("Evict radius",    &project_.streamEvictRadius,    1, 32, 5,
               "Must exceed the load radius, or a chunk is evicted the frame after it loads.");
    settingInt("Load budget",     &project_.streamLoadBudget,     1, 32, 2,
               "Chunks loaded per frame. THE ONLY THING BOUNDING THE FRAME HITCH:\n"
               "the load is synchronous, so this is a frame-time dial, not a memory one.");
    settingInt("Evict budget",    &project_.streamEvictBudget,    1, 32, 4, nullptr);
    settingInt("Vertical radius", &project_.streamVerticalRadius, 0, 8,  1, nullptr);
    {
        f32 v = project_.streamLeadSeconds >= 0.0f ? project_.streamLeadSeconds : 1.5f;
        bool stated = project_.streamLeadSeconds >= 0.0f;
        ImGui::PushID("lead");
        if (ImGui::Checkbox("##stated", &stated)) { project_.streamLeadSeconds = stated ? v : -1.0f; projectDirty_ = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!stated);
        ImGui::SetNextItemWidth(180.0f * dpi_);
        if (ImGui::SliderFloat("Lead seconds", &v, 0.0f, 10.0f, "%.2f")) {
            project_.streamLeadSeconds = v;
            projectDirty_ = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How far ahead of the camera's velocity to pre-load.");
        ImGui::PopID();
    }
    ImGui::Spacing();
    // Consumed now: GameStreaming::enable seeds every field's ChunkWorldSettings::stream from the
    // manifest. Read when streaming is enabled, not live; a PCGVOLUME's own radius beats these.
    ImGui::TextDisabled("Read when streaming starts, not live: a running world keeps the budgets");
    ImGui::TextDisabled("it opened with. A PCGVOLUME stating its own radius overrides the two");
    ImGui::TextDisabled("radii above for that field; the budgets and lead time always apply.");
#endif
}

// Project Settings > Physics. World-wide defaults, written to PHYSICS.* in the manifest.
void SandboxApp::buildPhysicsSettings() {
    ImGui::TextUnformatted("Physics");
    ImGui::SameLine(); ImGui::TextDisabled("(Jolt, behind the plain-C seam)");
    ImGui::Separator();
#if AVER_MODULE_PHYSICS
    // Not ready is a real state, not an error: a project can be open before aver_phys_init runs,
    // and every setter below would silently no-op -- so the page says so instead of offering dead controls.
    const bool ready = aver_phys_ready() != 0;
    if (!ready)
        ImGui::TextDisabled("No physics world yet -- these apply when one is created.");

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::TextUnformatted("Gravity (cm/s^2)");
    if (ImGui::DragFloat3("##gravity", project_.gravity, 1.0f, -10000.0f, 10000.0f, "%.1f")) {
        project_.hasGravity = true;
        projectDirty_ = true;
        if (ready) aver_phys_set_gravity(project_.gravity[0], project_.gravity[1], project_.gravity[2]);
    }
    uiReg_.track("project.physics.gravity");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Engine axes, +Z up. One g down is (0, 0, -980).");

    // Seconds on the wire, Hz on screen: the ABI takes a step in seconds (nobody reasons in 0.0167).
    // Converted here, not in the format, so the file stays in the ABI's own unit.
    f32 hz = project_.fixedStep > 0.0f ? 1.0f / project_.fixedStep : 60.0f;
    if (ImGui::SliderFloat("Tick rate (Hz)", &hz, 20.0f, 240.0f, "%.0f")) {
        project_.fixedStep = 1.0f / hz;
        projectDirty_ = true;
        // CHECKED: the setter refuses anything outside (0, 0.5] and reports it by returning 0.
        if (ready && !aver_phys_set_fixed_step(project_.fixedStep))
            AVER_WARN("[Project] physics tick rate {} Hz refused by the solver", hz);
    }
    uiReg_.track("project.physics.tickRate");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The fixed step the solver advances in. Its own header advises setting\n"
                          "this before any bodies exist; changing it mid-session changes how a\n"
                          "running simulation behaves.");
    ImGui::PopItemWidth();

    if (ready) ImGui::TextDisabled("%d body(ies) in the world.", aver_phys_body_count());
#else
    ImGui::TextDisabled("This build has no physics module.");
#endif
}

// Project Settings > Audio. The mix a project starts at, written to AUDIO.* in the manifest.
void SandboxApp::buildAudioSettings() {
    ImGui::TextUnformatted("Audio");
    ImGui::SameLine(); ImGui::TextDisabled("(Aver.Audio mixer)");
    ImGui::Separator();
#if AVER_WITH_AUDIO_ABI
    ImGui::TextDisabled("Applied when the project opens. The device may be closed until a");
    ImGui::TextDisabled("Play session or the Sound Editor opens it; the values still stick.");
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

    // One presence flag for the whole mix (OcProjectDesc::hasAudioMix): touching any slider states
    // it, because a muted bus and an unstated one must stay different things.
    bool mixed = false;
    if (ImGui::SliderFloat("Master", &project_.masterVolume, 0.0f, 1.0f, "%.2f")) mixed = true;
    uiReg_.track("project.audio.master");
    ImGui::Separator();
    static const char* kBusNames[4] = {"Sfx", "Music", "Voice", "UI"};
    for (int b = 0; b < 4; ++b) {
        if (ImGui::SliderFloat(kBusNames[b], &project_.busVolume[b], 0.0f, 1.0f, "%.2f")) mixed = true;
        uiReg_.track((std::string("project.audio.bus.") + kBusNames[b]).c_str());
    }
    ImGui::PopItemWidth();

    if (mixed) {
        project_.hasAudioMix = true;
        projectDirty_ = true;
        aver_audio_set_master_volume(project_.masterVolume);
        for (int b = 0; b < 4; ++b) aver_audio_set_bus_volume(b, project_.busVolume[b]);
    }
    if (!project_.hasAudioMix)
        ImGui::TextDisabled("This project states no mix; these are the engine's defaults.");
#else
    ImGui::TextDisabled("This build does not link the audio seam.");
#endif
}

#if AVER_MODULE_VOXI
// Draws one of the Rendering page's sub-pages (General / Global Illumination / Ray Tracing /
// Path Tracing). Each feature reports its real status and is disabled when the renderer or GPU
// can't do it. Reads and writes the WHOLE Settings struct regardless of which sub-page is showing.
void SandboxApp::buildRenderingSettings(int page) {
    using namespace aver::voxi;
    Renderer& vx = Renderer::get();
    static const char* kPageTitle[] = {"", "General", "Global Illumination", "Ray Tracing", "Path Tracing"};
    ImGui::TextUnformatted(kPageTitle[page]);
    ImGui::SameLine(); ImGui::TextDisabled("(Voxi render module)");
    ImGui::Separator();

    Settings s = vx.settings();
    // Every prerequisite this page greys against, resolved ONCE per frame from these settings --
    // see RenderSettingsResolver.hpp: the same question used to get answered four different ways
    // (a clamp, an ad hoc disabled check, a console var, nothing at load time); this removes that.
    const Resolution er = resolve(s, vx.deviceInfo());
    bool changed = false;
    // Set by an Overall button or a per-group row below: which groups this edit moved to a rung, so
    // captureRenderSettingsFromUi/captureVoxiSettings (Lane 2) write "follow the tier" (-1) for them.
    u32 overallFollowMask = 0;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

    if (page == 1) {
        // ---- OVERALL QUALITY: UE-style scalability preset ----
        // One button moves GI/Ray Tracing/Path Tracing to the same rung at once (Scalability.hpp).
        // MSAA, AverSR, shadows and post are deliberately excluded -- see that header's "Groups
        // excluded, and why". Placed at the top of the page, like UE's own Scalability panel.
        ImGui::TextUnformatted("Overall Quality");
        {
            const auto qualityButton = [&](const char* label, bool active) {
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                const bool hit = ImGui::Button(label);
                if (active) ImGui::PopStyleColor();
                return hit;
            };
            const OverallQuality cur = overallFromSettings(s, vx.deviceInfo());
            static const OverallQuality kRungs[] = {OverallQuality::Low, OverallQuality::Medium,
                                                    OverallQuality::High, OverallQuality::Epic};
            static const char* kRungNames[] = {"Low", "Medium", "High", "Epic"};
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (qualityButton(kRungNames[i], cur == kRungs[i])) {
                    overallFollowMask |= applyOverall(s, kRungs[i], vx.deviceInfo());
                    changed = true;
                }
                if (kRungs[i] == OverallQuality::Epic && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Global Illumination's voxel grid goes to 512^3, about\n"
                                      "3.2 GB -- by far the steepest rung on that ladder. The grid\n"
                                      "is only ever built at init, so this takes effect on the\n"
                                      "next project reload, not immediately.");
            }
            // Amber "Custom" rather than leaving all four rungs unhighlighted: overallFromSettings
            // reads Custom once any group drifts off the ladder, groups disagree, or PT is above Off.
            if (cur == OverallQuality::Custom) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1), "Custom");
            }
            // Every rung above turns Path Tracing OFF (scalabilityRung, a locked decision) -- a PT
            // tier above Off would silently drop to Off on the next click. Said here in amber rather
            // than discovered by the path-traced view vanishing.
            if (s.pathTracing != Quality::Off)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                                   "Picking a preset turns Path Tracing off (currently %s).",
                                   Renderer::qualityName(s.pathTracing));
        }

#if AVER_MODULE_SR
        // ---- AverSR's own default -- NOT one of the three groups above ----
        // AverSR sits outside Overall/Custom detection on purpose (module boundary, Scalability.hpp):
        // an Overall preset moves GI/RT/PT together but never touches this. The one place a pinned
        // AverSR level that's drifted from the rung's default is visible.
        {
            const char* rungName = averSrAutoRungName(s, vx.deviceInfo());
            std::string sourceText = averSrSource_ == voxi::AverSrSource::Auto
                ? (std::string("Auto from ") + rungName)
                : (averSrSource_ == voxi::AverSrSource::ForcedOff
                       ? (edgeAaEnabled_ ? "--edge-aa" : "forced Off after a failed launch")
                       : averSrSourceText(averSrSource_));
            ImGui::Text("Upscaling: AverSR %s (%s)", aver::sr::qualityName(averSrQuality_),
                        sourceText.c_str());
            // Only meaningful once the resolved level did NOT come from Auto -- Custom-detection can
            // hide a pinned AverSR level, and this line surfaces it.
            if (averSrSource_ != voxi::AverSrSource::Auto) {
                const u32 rungLevel = voxi::autoAverSrLevel(s, vx.deviceInfo());
                if (rungLevel != static_cast<u32>(averSrQuality_))
                    ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                        "(differs from the %s preset's default, %s)", rungName,
                        aver::sr::qualityName(static_cast<aver::sr::Quality>(rungLevel)));
            }
            // Same one-time "until any item is picked" flag the Display combo shows -- repeated
            // here so a person who never opens Display still sees why AverSR turned on.
            if (averSrMigrationNoteArmed_)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                    "AverSR now defaults to Auto (%s). Choose Off for native resolution.",
                    aver::sr::qualityName(averSrQuality_));
            // Captured beside project_.occlusionCull in captureRenderSettingsFromUi. -1 ("Follow
            // Overall preset") is an explicit default, not "unstated" -- index 0, not a blank selection.
            static const char* kProjDefaultItems[] = {"Follow Overall preset", "Off", "Quality",
                                                       "Balanced", "Performance"};
            int projIdx = averSrProjectDefault_ < 0 ? 0 : averSrProjectDefault_ + 1;
            if (ImGui::Combo("Upscaling default (AverSR)", &projIdx, kProjDefaultItems, 5)) {
                averSrProjectDefault_ = projIdx == 0 ? -1 : projIdx - 1;
                averSrMigrationNoteArmed_ = false;
                averSrCookieTripped_ = false;   // an explicit pick lifts the crash-cookie latch, as the Display combo's does
                projectDirty_ = true;
            }
            uiReg_.track("project.averSr");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The project's OWN AverSR default, round-tripped as "
                                  "RENDER.AVERSR -- outranked by --aversr and by your own Display "
                                  "preference, and itself outranks the Overall rung's ladder "
                                  "default. \"Follow Overall preset\" (-1) pins nothing.");
        }
#endif

        // RENDER.FRAMEGEN: frame generation during Play and in the packaged game. Edited on project_
        // directly (like AVERSR it is device-level, outside the Voxi settings this page mostly mirrors);
        // editing uses the Editor Preference instead, and --frame-gen outranks both.
        {
            bool fg = project_.frameGen == 1;
            if (ImGui::Checkbox("Frame generation (Play and packaged game)", &fg)) {
                project_.frameGen = fg ? 1 : 0;
                projectDirty_ = true;
            }
            uiReg_.track("project.frameGen");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Generates a frame between every two rendered ones (procedural frame "
                                  "interpolation). Round-trips as RENDER.FRAMEGEN. Needs V-Sync and 1x "
                                  "anti-aliasing; the viewport while editing has its own Editor Preference.");
        }

        // The three groups an Overall preset moves together, each its own Off..Epic row: where a
        // Custom project actually lives. "(modified)" flags a group at the right tier with a
        // hand-edited knob (groupFollowsLadder) -- indistinguishable from Custom in the Overall row alone.
        {
            static const ScalabilityGroup kGroups[] = {ScalabilityGroup::GlobalIllumination,
                                                       ScalabilityGroup::RayTracing,
                                                       ScalabilityGroup::PathTracing};
            static const Feature kGroupFeature[] = {Feature::GlobalIllumination, Feature::RayTracing,
                                                    Feature::PathTracing};
            static const char* kGroupLabel[] = {"Global Illumination", "Ray Tracing", "Path Tracing"};
            static const char* kTierNames[] = {"Off", "Low", "Medium", "High", "Epic"};
            // Per-group equivalent of applyOverall (Scalability.hpp), which always moves all three
            // together and so can't serve a single row; mirrors its field list exactly, group for group.
            const auto setGroupTier = [](Settings& set, ScalabilityGroup g, Quality t) {
                switch (g) {
                    case ScalabilityGroup::GlobalIllumination:
                        set.globalIllumination = t;
                        set.voxelResolution    = ladder::voxelResolution(t);
                        set.giCones            = ladder::giCones(t);
                        set.giUpdateInterval   = ladder::giUpdateInterval(t);
                        set.giRestirVisibility = ladder::giRestirVisibility(t);
                        break;
                    case ScalabilityGroup::RayTracing:
                        set.rayTracing         = t;
                        set.rtShadowRays       = ladder::rtShadowRays(t);
                        set.rtPixelsPerRayTile = ladder::rtPixelsPerRayTile(t);
                        set.rtShadowDenoise    = ladder::rtShadowDenoise(t);
                        set.rtRenderMode       = ladder::rtRenderMode(t);
                        set.refractionMode     = ladder::refraction(t);
                        set.giSkyOcclusionRays = ladder::giSkyOcclusionRays(t);
                        set.giSkyOcclusionTile = ladder::giSkyOcclusionTile(t);
                        break;
                    case ScalabilityGroup::PathTracing:
                        set.pathTracing = t;
                        set.ptBounces   = ladder::ptBounces(t);
                        break;
                    default: break;
                }
            };
            for (int gi = 0; gi < 3; ++gi) {
                const ScalabilityGroup group = kGroups[gi];
                ImGui::PushID(gi);
                ImGui::TextUnformatted(kGroupLabel[gi]);
                featureStatusBadge(vx, kGroupFeature[gi]);
                if (!groupFollowsLadder(s, group)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1), "(modified)");
                }
                const bool avail = groupAvailable(group, vx.deviceInfo());
                ImGui::BeginDisabled(!avail);
                const Quality gt = groupTier(s, group);
                const auto qualityButton = [&](const char* label, bool active) {
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    const bool hit = ImGui::Button(label);
                    if (active) ImGui::PopStyleColor();
                    return hit;
                };
                for (int i = 0; i < 5; ++i) {
                    if (i) ImGui::SameLine();
                    if (qualityButton(kTierNames[i], static_cast<u32>(gt) == static_cast<u32>(i))) {
                        setGroupTier(s, group, static_cast<Quality>(i));
                        overallFollowMask |= 1u << static_cast<u32>(group);
                        changed = true;
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::Separator();

        ImGui::TextUnformatted(Renderer::featureName(Feature::Msaa));
        const u32 mask = vx.deviceInfo().msaaMask;
        const u32 counts[4] = {1,2,4,8};
        const char* labels[4] = {"Off","2x","4x","8x"};
        for (int i=0;i<4;++i) {
            const bool ok = (mask & counts[i]) != 0;
            if (i) ImGui::SameLine();
            ImGui::BeginDisabled(!ok);
            if (ImGui::RadioButton(labels[i], static_cast<u32>(s.msaa)==counts[i])) { s.msaa=static_cast<Msaa>(counts[i]); changed=true; }
            ImGui::EndDisabled();
        }
        // er.rtRenderMode.effective, not the raw request: whether the ray pass is ACTUALLY finding
        // primary visibility this frame -- one fullscreen triangle, no per-triangle coverage, always
        // single-sample regardless of the pick above (Settings::rtRenderMode's trade-off list).
        // RT Low rasterises (D3), so this note doesn't apply there even with RT not Off.
        if (er.rtRenderMode.effective == 1)
            ImGui::TextDisabled("Primary visibility is ray-driven -- the ray pass runs at a single "
                                "sample regardless of the setting above.");

        ImGui::Separator();
        const Status st = vx.status(Feature::MeshShaders);
        ImGui::TextUnformatted(Renderer::featureName(Feature::MeshShaders));
        featureStatusBadge(vx, Feature::MeshShaders);
        ImGui::BeginDisabled(st != Status::Ready);
        if (ImGui::Checkbox("Use mesh shaders", &s.meshShaders)) changed = true;
        ImGui::EndDisabled();

        ImGui::Separator();
        // Layered BSDF lives on this page, not its own: it changes what every material-shaded draw
        // computes, like MSAA or mesh shaders. Doesn't take effect until reload (stated on the
        // control): Off compiles the coat lobe out entirely, and VoxiRenderer builds and latches one
        // shader set per setting, so a live toggle would double a 20+ PSO matrix at startup.
        const Status lst = vx.status(Feature::LayeredBsdf);
        ImGui::TextUnformatted(Renderer::featureName(Feature::LayeredBsdf));
        featureStatusBadge(vx, Feature::LayeredBsdf);
        ImGui::BeginDisabled(lst != Status::Ready);
        int lq = static_cast<int>(s.layeredBsdf);
        const char* lqs[] = {"Off","Low","Medium","High","Epic"};
        if (ImGui::Combo("Shading model", &lq, lqs, 5)) {
            s.layeredBsdf = static_cast<Quality>(lq);
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off is the standard BRDF. Any other setting adds a clear-coat lobe "
                              "over the base for materials that author one -- a material with "
                              "coat weight 0 renders identically either way.");
        // Compared against what the RENDERER latched, not the combo's last value, so this still
        // shows after the settings window is closed and reopened.
        if ((s.layeredBsdf != Quality::Off) != voxiRenderer_.layeredBsdfActive())
            ImGui::TextWrapped("Takes effect when the project is reloaded; the shaders compiled "
                               "for this session are unchanged.");

        // ---- Refraction, and three culling knobs that were CLI-only ----
        // All four were live per-frame state with no control anywhere: a CLI flag could set them
        // but nothing recorded the choice. Each is now one manifest key.
        ImGui::Separator();
        ImGui::TextUnformatted("Refraction");
        {
            // BeginCombo/Selectable, not a plain Combo: only the RAY-TRACED entry has a prerequisite
            // (RT hardware, RT tier not Off -- er.refractionMode.reason), and a plain Combo can't
            // grey out one entry while leaving Off/Screen-space selectable. A request at or below
            // Screen-space never touches this reason (RenderSettingsResolver.hpp resolve(), refractionMode).
            static const char* rms[] = {"Off", "Screen-space", "Ray-traced"};
            if (ImGui::BeginCombo("Mode", rms[s.refractionMode])) {
                for (int i = 0; i < 3; ++i) {
                    const bool rayTracedGreyed = (i == 2) && greysControl(er.refractionMode.reason);
                    ImGui::BeginDisabled(rayTracedGreyed);
                    if (ImGui::Selectable(rms[i], s.refractionMode == static_cast<u32>(i))) {
                        s.refractionMode = static_cast<u32>(i);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if (rayTracedGreyed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", disableReasonText(er.refractionMode.reason));
                }
                ImGui::EndCombo();
            }
            uiReg_.track("project.refraction.mode");
            ImGui::BeginDisabled(s.refractionMode == 0);
            if (ImGui::SliderFloat("Strength", &s.refractionStrength, 0.0f, 2.0f)) changed = true;
            uiReg_.track("project.refraction.strength");
            if (ImGui::SliderFloat("Edge fade", &s.refractionEdgeFade, 0.0f, 1.0f)) changed = true;
            uiReg_.track("project.refraction.edgeFade");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Screen-space refraction cannot see what the camera never saw.\n"
                                  "This fades the effect out near the screen edge, where the miss\n"
                                  "would otherwise be visible.");
            ImGui::EndDisabled();
        }

        // ---- THE POST CHAIN: the one part of the image the file couldn't hold ----
        // Exposure/bloom/tonemap/auto-exposure existed only as CLI flags and live Post Process
        // sliders: set one there, package the game, and it shipped with rhi::PostSettings'
        // compiled defaults instead (docs/RUNTIME-DEDUP.md: "a project cannot author
        // exposure/bloom/tonemap"). Same stated/unstated idiom as buildImportSettings/settingInt
        // (-1, not 0, means unstated -- 0 would pin a default a fully-stated project never meant to pin).
        //
        // On General, not beside the GI page's ReSTIR sliders: filing exposure under Global
        // Illumination repeats the "Indirect diffuse" combo's mistake -- a control filed under its
        // implementation instead of its job, reported as "I cannot find that project setting".
        // These belong to the camera, like MSAA above them.
        //
        // Every control writes the key AND the live post_ together, so the viewport shows what's
        // being dragged. A live edit outranks a flag -- the opposite of load time, where
        // applyProject lets --exposure win because the flag is the newest thing said -- here, the
        // drag is.
        ImGui::Separator();
        ImGui::TextUnformatted("Post processing");
        ImGui::TextDisabled("What a PACKAGED GAME renders with. An unticked key is not stated, and");
        ImGui::TextDisabled("the engine's compiled default applies to it.");
        {
            // A lambda rather than settingInt: these two need a float widget and the live mirror,
            // neither of which that int helper can give.
            const auto postFloat = [&](const char* id, const char* label, f32* key, f32* live,
                                       f32 lo, f32 hi, const char* form, ImGuiSliderFlags flags,
                                       const char* trackName, const char* tip) {
                ImGui::PushID(id);
                bool stated = (*key >= 0.0f);
                // Ticking adopts what the viewport already shows, not a hardcoded number -- the
                // point is to record the look already dialed in via the Post Process panel.
                f32 v = stated ? *key : *live;
                if (ImGui::Checkbox("##stated", &stated)) {
                    // Unticking leaves the viewport alone: "unstated" means the compiled default
                    // applies next open, not that this session's eyes snap back mid-edit -- same as
                    // how applyProjectRenderSettings reads every other absent key.
                    *key = stated ? v : -1.0f;
                    if (stated) *live = v;
                    projectDirty_ = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unticked: this project does not state it,\n"
                                      "and the engine's own default applies.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!stated);
                if (ImGui::SliderFloat(label, &v, lo, hi, form, flags)) {
                    *key = v;
                    *live = v;
                    projectDirty_ = true;
                }
                ImGui::EndDisabled();
                // Tracked before the tooltip: IsItemHovered reads the LAST submitted item, and a
                // tooltip submits its own (same ordering rule as the ReSTIR history slider above).
                uiReg_.track(trackName);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tip);
                ImGui::PopID();
            };
            postFloat("exposure", "Exposure", &project_.postExposure, &post_.exposure,
                      0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic,
                      "project.post.exposure",
                      "Linear multiplier on scene radiance, applied before the tone\n"
                      "curve. With auto exposure below on, it is compensation on the\n"
                      "adapted value: 1 = as metered, 2 = one stop brighter.\n\n"
                      "Round-trips as RENDER.EXPOSURE.");
            postFloat("bloom", "Bloom", &project_.postBloom, &post_.bloomIntensity,
                      0.0f, 1.0f, "%.3f", 0, "project.post.bloom",
                      "ZERO IS A REAL ANSWER HERE, which is why the key's sentinel is\n"
                      "-1 and not 0: zero intensity builds no pyramid and records no\n"
                      "pass at all (RHI.hpp:151), so it is a cost choice as well as a\n"
                      "look. The sandbox's own --bloom default is 0 rather than\n"
                      "PostSettings' 0.06, so every recorded gate image was taken\n"
                      "with no bloom.\n\nRound-trips as RENDER.BLOOM.");
            {
                // A checkbox, not settingInt's 0..1 slider (unlike WINDOW.RESIZABLE and the IMPORT.*
                // pair that use that helper): the Post Process panel already spells this on/off as a
                // checkbox (SandboxPanels.cpp:1917), and hand-rolling it also lets the registry track
                // the control before its tooltip runs.
                ImGui::PushID("autoExposure");
                bool stated = project_.postAutoExposure >= 0;
                bool on = stated ? project_.postAutoExposure != 0 : post_.autoExposure;
                if (ImGui::Checkbox("##stated", &stated)) {
                    project_.postAutoExposure = stated ? (on ? 1 : 0) : -1;
                    if (stated) post_.autoExposure = on;
                    projectDirty_ = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unticked: this project does not state it,\n"
                                      "and the engine's own default (on) applies.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!stated);
                if (ImGui::Checkbox("Auto exposure", &on)) {
                    project_.postAutoExposure = on ? 1 : 0;
                    post_.autoExposure = on;
                    projectDirty_ = true;
                }
                ImGui::EndDisabled();
                uiReg_.track("project.post.autoExposure");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Eye adaptation from a luminance histogram of the frame.\n"
                                      "A run with a frame limit forces it OFF unless\n"
                                      "--auto-exposure asked for it by name, so a capture\n"
                                      "measures one exposure instead of the histogram's warm-up.\n\n"
                                      "Round-trips as RENDER.AUTOEXPOSURE.");
                ImGui::PopID();
            }
            // Amber, not greying the slider like the Post Process panel does: there it's the live
            // value adaptation is about to overwrite; here it's a RECORDED one, meaningful either
            // order -- a project shipping with adaptation off next month still wants it recorded today.
            if (post_.autoExposure && project_.postExposure >= 0.0f)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                    "Auto exposure is on, so the adaptation overwrites Exposure every frame. The "
                    "recorded value still applies wherever auto exposure is off.");
            {
                ImGui::PushID("tonemap");
                bool stated = project_.postTonemap >= 0;
                // Clamped from both sources, since neither is bounded: the parser stores whatever
                // integer RENDER.TONEMAP holds (OcProject.cpp) and setTonemap takes --tonemap's atoi
                // as-is, so a 5 from either would index this combo off the end.
                int mode = stated ? project_.postTonemap : static_cast<int>(post_.tonemap);
                if (mode > 2) mode = 2;
                if (ImGui::Checkbox("##stated", &stated)) {
                    project_.postTonemap = stated ? mode : -1;
                    if (stated) post_.tonemap = static_cast<u32>(mode);
                    projectDirty_ = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unticked: this project does not state it,\n"
                                      "and the engine's own default (ACES fitted) applies.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!stated);
                if (ImGui::Combo("Tone curve", &mode,
                                 "Per-channel ACES\0ACES fitted (default)\0ACES luminance\0")) {
                    project_.postTonemap = mode;
                    post_.tonemap = static_cast<u32>(mode);
                    projectDirty_ = true;
                }
                ImGui::EndDisabled();
                uiReg_.track("project.post.tonemap");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(
                        "Which curve resolves HDR radiance to the display:\n"
                        "  Per-channel     the original Narkowicz/Hill approximation: the\n"
                        "                  gentlest toe, so dim bounce light stays visible\n"
                        "                  instead of crushing to black\n"
                        "  ACES fitted     the DEFAULT: the same curve between the ACES\n"
                        "                  matrices, the space Unreal's filmic curve runs\n"
                        "                  in; matched a UE5 Lumen Sponza's contrast\n"
                        "  ACES luminance  tonemaps LUMINANCE and puts the original\n"
                        "                  chromaticity back, so hue survives any exposure\n\n"
                        "Measured on PTTest Sponza at exposure 8: fitted holds chroma 1.41\n"
                        "where luminance holds 3.09 at the same brightness, and above 5x\n"
                        "fitted lets BLUE overtake GREEN -- warm stone rendering cold\n"
                        "(RHI.hpp:196-216).\n\nRound-trips as RENDER.TONEMAP.");
                ImGui::PopID();
            }
            // Where the precedence is actually visible: a flag applies at startup, the manifest
            // later, and applyProject won't let these keys overwrite one -- which otherwise looks
            // like a control that saved fine and did nothing next launch.
            if (postExposureFromCli_ || postBloomFromCli_ || postAutoExpFromCli_)
                ImGui::TextDisabled("A post-process flag was given on this launch; it outranks "
                                    "these keys when a project opens.");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Culling and level of detail");
        {
            bool lod = lodSelectEnabled_;
            if (ImGui::Checkbox("LOD select", &lod)) {
                setLodSelect(lod, lodErrorThresholdPx_);
                projectDirty_ = true;
            }
            uiReg_.track("project.lod.select");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off draws every instance at LOD 0. Measured on Electric Dreams:\n"
                                  "102.7 ms median off, 76.7 ms on.");
            ImGui::BeginDisabled(!lodSelectEnabled_);
            f32 px = lodErrorThresholdPx_;
            if (ImGui::SliderFloat("Screen error (px)", &px, 0.25f, 8.0f, "%.2f")) {
                setLodSelect(lodSelectEnabled_, px);
                projectDirty_ = true;
            }
            uiReg_.track("project.lod.threshold");
            ImGui::EndDisabled();

            if (ImGui::Checkbox("Occlusion culling", &occlusionCullEnabled_)) projectDirty_ = true;
            uiReg_.track("project.occlusionCull");
            if (ImGui::Checkbox("Depth pre-pass", &depthPrepassOverride_)) projectDirty_ = true;
            uiReg_.track("project.depthPrepass");

            ImGui::Separator();
            // The renderer this project asks for. Unlike everything else here, it does nothing
            // until restart -- the device is created before a project opens, so main peeks the
            // manifest for this one key before startup. Said on the control so it reads as
            // understood, not broken.
            {
                static const char* kNames[] = {"Engine default", "D3D12", "Vulkan", "D3D11"};
                static const char* kKeys[]  = {"", "d3d12", "vulkan", "d3d11"};
                int cur = 0;
                for (int i = 1; i < 4; ++i) if (projectBackend_ == kKeys[i]) { cur = i; break; }
                if (ImGui::Combo("Renderer", &cur, kNames, 4)) {
                    projectBackend_ = kKeys[cur];
                    projectDirty_ = true;
                }
                uiReg_.track("project.backend");
                // What is actually running, beside what was asked for: a backend has to be compiled
                // in to be selectable at all, and the default CMake config has AVER_RHI_VULKAN OFF,
                // so a project can name Vulkan on a build that can't give it one. The RHI already
                // warns in the log when that happens; this puts the same fact where the choice is made.
                ImGui::SameLine();
                ImGui::TextDisabled("(now: %s)", runningBackend_.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Takes effect on the next launch.\n"
                                      "A backend must be compiled into the build to be usable:\n"
                                      "configure with -DAVER_RHI_VULKAN=ON for Vulkan.");
            }
        }
    }

    if (page == 2) {
        const Status st = vx.status(Feature::GlobalIllumination);
        ImGui::TextUnformatted(Renderer::featureName(Feature::GlobalIllumination));
        featureStatusBadge(vx, Feature::GlobalIllumination);
        ImGui::BeginDisabled(st != Status::Ready);
        int q = static_cast<int>(s.globalIllumination);
        const char* qs[] = {"Off","Low","Medium","High","Epic"};
        if (ImGui::Combo("Quality", &q, qs, 5)) { s.globalIllumination = static_cast<Quality>(q); changed = true; }

        // ---- Which diffuse GI estimator -- lives on THIS page deliberately ----
        // Used to sit on Ray Tracing -- ReSTIR needs RayQuery, which looked like the dependency
        // that mattered. It is not. Someone looking for the diffuse GI algorithm looks under Global
        // Illumination and finds only cone-gather knobs -- a control filed under its implementation
        // instead of its job, reported as "I cannot find that project setting". Sits above the cone
        // knobs since it decides whether they apply at all.
        //
        // giMode round-trips as RENDER.GIMODE; absent from an older manifest it stays -1 and the
        // engine default (cones) applies -- what every pre-existing project already meant.
        //
        // Shows er.giMode.EFFECTIVE, not the raw stored request (F-d, Lane 1): giMode isn't clamped
        // inside Settings itself, so a request ReSTIR can't run right now is remembered (s.giMode
        // untouched below) rather than rewritten to 0 -- see RenderSettingsResolver.hpp's resolve()
        // for which three things have to agree first. Showing the raw value while cones ran instead
        // would read as dead; the effective one switches back on its own once the reason clears.
        int giAlgo = static_cast<int>(er.giMode.effective);
        if (ImGui::Combo("Indirect diffuse", &giAlgo, "Voxel cones\0ReSTIR (experimental)\0")) {
            s.giMode = static_cast<u32>(giAlgo); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Voxel cones is what this engine has always shipped: a clipmap\n"
                              "marched with cones. It is the default and is unchanged. The\n"
                              "Voxel grid and Diffuse cones settings below apply ONLY to it.\n\n"
                              "ReSTIR resamples ray-traced indirect samples over time AND\n"
                              "across neighbouring pixels (the engine's own implementation).\n"
                              "It has no voxel volume, so the boundary artefacts the clipmap\n"
                              "produces -- surfaces near the edge reading as unoccluded, worst\n"
                              "while the camera moves -- cannot occur.\n\n"
                              "EXPERIMENTAL: spatio-temporal resampling. Grainier than the\n"
                              "cone gather unless Denoiser below is on -- which is what the\n"
                              "denoiser pass is for, and it needs MSAA 1 to run at all.");
        // ReSTIR is requested but not effective: named here in the page's red-reason idiom, rather
        // than left for the combo to silently read "Voxel cones" with no explanation.
        if (s.giMode != 0 && er.giMode.reason != DisableReason::None)
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1),
                               "   ReSTIR is selected and resumes when %s",
                               disableReasonText(er.giMode.reason));

        // optimisation-wave-2's U1: how much of F2 (candidate-hit sky) and F3 (reuse visibility)
        // -- the contrast fix's two per-pixel rays, cb4b48df -- this ReSTIR estimator pays for.
        // SHOWS er.giRestirVisibility.REQUESTED, deliberately, unlike the giMode combo just above:
        // RenderSettingsResolver.hpp's own comment on this field explains why effective always
        // equals requested for it (clamping to 0 on a failed prerequisite would read "No ray
        // (over-bright)" while no ReSTIR runs at all) -- inertness is shown by greying the control
        // and naming the reason below, never by the combo silently jumping to a different choice.
        const bool visGreyed = greysControl(er.giRestirVisibility.reason);
        ImGui::BeginDisabled(visGreyed);
        int vis = static_cast<int>(er.giRestirVisibility.requested);
        if (ImGui::Combo("ReSTIR visibility rays", &vis,
            "No ray (pre-fix, over-bright)\0Reconstructed (no ray)\0Half resolution\0Full\0Cached (radiance cache)\0")) {
            s.giRestirVisibility = static_cast<u32>(vis); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.gi.restirVisibility");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Extra rays per shaded fragment beyond the candidate/sun/sky/\n"
                              "reflection rays this estimator already traces (cost UNMEASURED):\n"
                              "  Full           up to 2 (F2 is the expensive one)\n"
                              "  Cached         the Half ray budget, but those rays also train a\n"
                              "                 world-space radiance cache that the untraced pixels\n"
                              "                 read (staged ray-driven on D3D12 only; elsewhere\n"
                              "                 it runs as Half; UNVERIFIED)\n"
                              "  Half           up to 0.5 at rest, plus Full on pixels with no\n"
                              "                 valid reconstruction this frame\n"
                              "  Reconstructed  0 rays -- one voxel-cone march instead\n"
                              "  No ray         0 -- the pre-fix behaviour\n\n"
                              "Known error of each approximation:\n"
                              "  Full           none -- this is the traced ground truth\n"
                              "  Half           lags about 5 frames and blurs over a 2x2\n"
                              "                 block where it reconstructs instead of tracing\n"
                              "  Reconstructed  leaks light through thin/near occluders and past\n"
                              "                 the voxel volume's own edge\n"
                              "  No ray         restores cb4b48df's over-brightness outright\n\n"
                              "Round-trips as RENDER.RESTIRVISIBILITY.");
        if (visGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]",
                               disableReasonText(er.giRestirVisibility.reason));
        } else if (er.giRestirVisibility.reason == DisableReason::RequiresStagedRayDriven) {
            // SOFT reason (greysControl is false for it): the choice stays live and runs as Half
            // resolution, so this is amber, not the red a greyed control's reason gets above.
            ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1), "[%s]",
                               disableReasonText(er.giRestirVisibility.reason));
        } else if (vis == 0) {
            // Amber, not red: a legal, live choice (unlike the giMode reason above, an unfixable
            // prerequisite) -- just the one choice that reopens a fix already shipped. No magnitude
            // quoted: R2/R3's pixel size is UNMEASURED (2.10 G).
            ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                "No ray restores the pre-fix candidate-hit sky and reuse visibility (legacy bits "
                "4 and 8): shadowed and enclosed areas read over-bright again -- the washed-out "
                "look cb4b48df fixed.");
        } else {
            // Legacy console switches force NoRay for their own ray regardless of this combo
            // (2.8's precedence rule) -- named here so a switch left on from a by-hand A/B doesn't
            // read as silent inaction.
            const u32 legacy = editor::consoleLightingLegacySlot();
            if (legacy & 12u) {
                std::string which = (legacy & 4u) ? "voxi.legacyRestirHitSky" : "";
                if (legacy & 8u) which += which.empty() ? "voxi.legacyRestirReuseVisibility"
                                                        : " / voxi.legacyRestirReuseVisibility";
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                    "Console switch %s is ON and forces No ray for that ray; this setting is "
                    "ignored for it until the switch is off.", which.c_str());
            }
        }

        // ---- Indirect light history: ReSTIR GI reuse.maxHistory ----
        // Right under Visibility because it's the other dial on the same estimator's artefact -- how
        // much a previous-frame reservoir weighs into the ReSTIR GI combine. This is what 674ed667
        // fixed: the camera-motion brightness fade was this value, not the visibility rays above.
        //
        // Gated the same way as ReSTIR visibility rays (mirrors er.giRestirVisibility's
        // RequiresRestirGi chain -- both are inert for the same reason, ReSTIR GI not running).
        // Shows er.giRestirMaxHistory.REQUESTED, not the raw field: effective equals requested
        // today, but requested is what stays correct if that ever stops being true.
        const bool histGreyed = greysControl(er.giRestirMaxHistory.reason);
        ImGui::BeginDisabled(histGreyed);
        int hist = static_cast<int>(er.giRestirMaxHistory.requested);
        if (ImGui::SliderInt("Indirect light history (frames)", &hist, 0, 8)) {
            s.giRestirMaxHistory = static_cast<u32>(hist); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.gi.restirHistory");
        // Tooltip binds to the slider, asked for BEFORE the greyed-reason label below: IsItemHovered
        // reads the LAST submitted item, so asking after the label would hang the tooltip off it.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("0 (default): each frame's indirect light stands on its own -- no\n"
                              "bright flash when the camera stops. Higher values let a pixel\n"
                              "lean on previous frames, which is exactly what caused that flash:\n"
                              "1 measured about 8%% too bright for roughly 25 frames after the\n"
                              "camera stopped, 8 about +104%% (Sponza, viewport mean luminance,\n"
                              "674ed667). The denoiser below already smooths this, so 0 measured\n"
                              "no noisier than 1 either at rest or in motion.\n\n"
                              "Round-trips as RENDER.RESTIRHISTORY.");
        if (histGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.giRestirMaxHistory.reason));
        }

        // The denoiser sits here, under the estimator it filters: it denoises the ReSTIR radiance
        // above and the ray-tracing page's sky occlusion, and it's the only thing in the engine that
        // needs the G-buffer. Before this checkbox it was only reachable via --gbuffer on the command line.
        //
        // Greyed for a hard reason (er.denoiser.reason -- RT hardware/tier, D3D12 backend, nothing to
        // denoise); left clickable for the soft one (RequiresMsaaOne), since the MSAA radios below
        // fix that in one click (RenderSettingsResolver.hpp's "soft reasons only warn inline").
        // Shown state is EFFECTIVE while hard-greyed (s.denoiser untouched, so a request survives a
        // temporary hardware loss) and the RAW request otherwise.
        const bool denoiserHardGreyed = greysControl(er.denoiser.reason);
        ImGui::BeginDisabled(denoiserHardGreyed);
        bool den = s.denoiser && !denoiserHardGreyed;
        if (ImGui::Checkbox("Denoiser (AMD FidelityFX)", &den)) { s.denoiser = den; changed = true; }
        ImGui::EndDisabled();
        if (denoiserHardGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.denoiser.reason));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Filters the ReSTIR indirect diffuse above and the ray-traced sky\n"
                                    "occlusion, with the AMD FidelityFX Denoiser (MIT,\n"
                                    "third_party/fidelityfx-denoiser).\n\n"
                                    "COSTS THE G-BUFFER: velocity, view-space depth and packed\n"
                                    "normal/roughness -- three render targets NOTHING ELSE in\n"
                                    "this engine needs, about 54 MB at 1080p. That is why it is\n"
                                    "off by default rather than something enabled for you.\n\n"
                                    "REQUIRES the G-buffer, MSAA 1 and D3D12. Above 1x the G-buffer is cleared\n"
                                    "but never written, so the pass refuses to run rather than\n"
                                    "filter blanks into a confidently wrong image.\n\n"
                                    "Round-trips as RENDER.DENOISER.");
        // s.msaa, not the device's live sample count: it's the value being edited on this page, so
        // the warning appears the moment the two disagree, before Apply, and clears the moment
        // Anti-aliasing is set to 1, before anything commits. RequiresMsaaOne never greys, so it
        // needs this dedicated line rather than the red reason tag above (hard reasons only).
        if (den && static_cast<u32>(s.msaa) != 1u)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                               "   Anti-aliasing is %ux -- set it to 1 or the denoiser stays off.",
                               static_cast<u32>(s.msaa));

        int res = static_cast<int>(s.voxelResolution);
        const char* resLabels[] = {"64", "128", "256", "512"};
        const int resValues[] = {64, 128, 256, 512};
        int resIdx = res>=512 ? 3 : (res>=256 ? 2 : (res>=128 ? 1 : 0));
        if (ImGui::Combo("Voxel grid", &resIdx, resLabels, 4)) { s.voxelResolution = (u32)resValues[resIdx]; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cubic edge of the GI volume -- memory and per-voxel GPU cost are\n"
                               "both this NUMBER CUBED, so each step up is an 8x jump:\n"
                               "  64  ~6 MB    128  ~50 MB    256  ~400 MB    512  ~3.2 GB\n"
                               "Changing Quality above moves this to match its rung (Off/Low=64,\n"
                               "Medium=128, High=256, Epic=512) unless you pick a value here\n"
                               "yourself, which then overrides the tier's default.");
        // The grid is only ever built at init (VoxiRenderer::createVoxelVolume) -- nothing resizes
        // it live, so a change here is recorded immediately but reaches the GPU volume only on
        // reload. Compared against voxelResolutionBuilt() (Lane 3, what's actually running), so this
        // note clears itself once a reload catches up.
        if (s.voxelResolution != voxiRenderer_.voxelResolutionBuilt())
            ImGui::TextWrapped("Takes effect when the project is reloaded; the volume built for "
                               "this session is still %u^3.", voxiRenderer_.voxelResolutionBuilt());
        if (ImGui::SliderFloat("GI intensity", &s.giIntensity, 0.0f, 4.0f)) changed = true;
        if (ImGui::SliderFloat("GI distance", &s.giMaxDistance, 10.0f, 20000.0f, "%.0f")) changed = true;
        ImGui::DragFloat3("Volume centre", &giCenter_.x, 0.5f);
        ImGui::DragFloat("Volume extent", &giExtent_, 0.5f, 1.0f, 100000.0f);
        // Cone count: the GI setting that actually costs anything. Tier-derived (Low 3, Medium 6,
        // High 9, Epic 13); picking a Quality above re-derives it.
        {
            int cones = static_cast<int>(s.giCones);
            if (ImGui::SliderInt("Diffuse cones", &cones, 1, 16)) {
                s.giCones = static_cast<u32>(cones);
                changed = true;
            }
            uiReg_.track("project.gi.cones");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cones in the diffuse gather, including the axial one.\n"
                                  "Changing Quality above re-derives this from the tier.");
        }
        // Occlusion-aware fog (Settings::fogOcclusion): lives here since it's built from the GI
        // voxel volume, and greys with the same prerequisite (er.fogOcclusion.reason).
        {
            const bool fogOccGreyed = greysControl(er.fogOcclusion.reason);
            ImGui::BeginDisabled(fogOccGreyed);
            bool fogOcc = s.fogOcclusion;
            if (ImGui::Checkbox("Fog respects occlusion", &fogOcc)) { s.fogOcclusion = fogOcc; changed = true; }
            ImGui::EndDisabled();
            uiReg_.track("project.gi.fogOcclusion");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Fog and haze only glow where the air can actually see the sky.\n"
                                  "Without it, the fog inside a covered arcade or a room is lit as\n"
                                  "if it were outdoors: a blue veil brighter than the walls behind it.\n\n"
                                  "Built from the GI voxel volume (a small sky-visibility grid,\n"
                                  "refreshed a slice per frame, ~0.2 ms), so it needs Global\n"
                                  "Illumination on. Outdoor fog is unchanged.\n\n"
                                  "Round-trips as RENDER.FOGOCCLUSION.");
            if (fogOccGreyed) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.fogOcclusion.reason));
            }
        }
        ImGui::Checkbox("Debug: show voxel radiance", &giDebugView_);

        // Live, console-var-only debug paints: neither is a Settings field (EditorConsole.hpp) --
        // both are raw bool slots reasserted onto the renderer every frame, the same ones `set
        // voxi.giPoisonView`/`giVisPathView` reach. Reading/writing them directly here means the
        // checkbox and the console command are the same switch, not two that can disagree.
        {
            bool poisonView = editor::consoleGiPoisonViewSlot();
            if (ImGui::Checkbox("Debug: GI poison view", &poisonView))
                editor::consoleGiPoisonViewSlot() = poisonView;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Paints an unmistakable colour over any pixel where one of "
                                  "voxi_restir.hlsli's guards fired THIS frame (giMode 1 / ReSTIR "
                                  "only, except violet):\n"
                                  "  magenta  store-time reservoir guard (the one that matters most)\n"
                                  "  cyan     candidate-radiance clamp\n"
                                  "  yellow   target-pdf guard\n"
                                  "  orange   pre-existing final-estimate guard\n"
                                  "  blue     denoiser-readback guard\n"
                                  "  red      raw estimate hit voxi.giRadianceCeiling, still finite\n"
                                  "  green    denoised readback hit the same ceiling\n"
                                  "  violet   ray-traced specular indirect ceiling hit (either "
                                  "diffuse estimator)\n"
                                  "All colours zero means no guard is firing.");

            bool visPathView = editor::consoleGiVisPathViewSlot();
            if (ImGui::Checkbox("Debug: GI visibility path view", &visPathView))
                editor::consoleGiVisPathViewSlot() = visPathView;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Paints F2's resolved visibility path over indirect diffuse:\n"
                                  "  yellow  no ray (mode 0 or a legacy bit)\n"
                                  "  green   reconstructed (voxel cone)\n"
                                  "  blue    half-resolution reconstruction\n"
                                  "  red     half-resolution fallback (traced -- no valid "
                                  "reconstruction available this pixel)\n"
                                  "  white   full trace\n"
                                  "Suppressed while GI poison view above is also on, which paints "
                                  "first.");
        }
        ImGui::EndDisabled();
    }

    if (page == 3) {
        const Status st = vx.status(Feature::RayTracing);
        ImGui::TextUnformatted(Renderer::featureName(Feature::RayTracing));
        featureStatusBadge(vx, Feature::RayTracing);
        ImGui::BeginDisabled(st != Status::Ready);
        int q = static_cast<int>(s.rayTracing);
        const char* qs[] = {"Off","Low","Medium","High","Epic"};
        if (ImGui::Combo("Quality", &q, qs, 5)) { s.rayTracing = static_cast<Quality>(q); changed = true; }

        // Inner grey beneath the outer one above (st != Ready, hardware-only): everything below is
        // inert while the RT tier is Off, which the outer grey doesn't cover -- a device with RT
        // hardware but Quality Off would otherwise leave every row interactive but dead.
        // er.rtSubControls carries that reason (rtGate, RenderSettingsResolver.hpp); the Quality
        // combo above stays clickable regardless, since it's the only control here that can turn
        // the tier back on.
        ImGui::BeginDisabled(greysControl(er.rtSubControls));

        ImGui::Spacing();
        ImGui::TextUnformatted("Sun shadow");
        ImGui::Separator();
        int rays = static_cast<int>(s.rtShadowRays);
        if (ImGui::SliderInt("Occlusion rays / pixel", &rays, 1, 32)) {
            s.rtShadowRays = static_cast<u32>(rays); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How many rays a pixel that traces THIS frame casts toward the\n"
                               "sun's disc. Linear in cost -- this is the knob to turn down\n"
                               "first if ray tracing starts costing frames.");

        ImGui::Spacing();
        ImGui::TextUnformatted("Denoiser: temporal amortisation");
        ImGui::Separator();
        // Tile edge, not raw pixels-per-ray: setPixelsPerRayTile only rounds to a power of two, so
        // offering anything else would be relabelled after the fact. Options: 1,2,4,8,16 (1..256 px/ray).
        const int tiles[] = {1, 2, 4, 8, 16};
        const char* tileLabels[] = {"Off (1x1 -- every pixel, every frame)",
                                    "2x2 (4 pixels/ray)", "4x4 (16 pixels/ray)",
                                    "8x8 (64 pixels/ray)", "16x16 (256 pixels/ray)"};
        int tileIdx = 0;
        for (int i = 0; i < 5; ++i) if (tiles[i] == (int)s.rtPixelsPerRayTile) tileIdx = i;
        if (ImGui::Combo("Shadow amortisation", &tileIdx, tileLabels, 5)) {
            s.rtPixelsPerRayTile = static_cast<u32>(tiles[tileIdx]); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("At N, one pixel in each NxN tile traces a fresh ray each frame;\n"
                               "every other pixel reuses a reprojected history sample instead.\n"
                               "Every pixel gets its own turn every N*N frames. Off is bit-for-\n"
                               "bit identical to having no denoiser; larger tiles trade a real\n"
                               "cut in rays traced for more frames of lag on fast-moving shadows.");

        // A second, independent denoiser -- the separate heading is the point: these two fail in
        // opposite directions. Above reuses THIS pixel across TIME (converges while still, collapses
        // under motion); below averages NEIGHBOURS with no history (motion can't poison it). They compose.
        ImGui::Spacing();
        ImGui::TextUnformatted("Denoiser: spatial filter");
        ImGui::Separator();
        int denoise = static_cast<int>(s.rtShadowDenoise);
        if (ImGui::SliderInt("Filter radius (px)", &denoise, 0, 3, denoise == 0 ? "Off" : "%d")) {
            s.rtShadowDenoise = static_cast<u32>(denoise); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Averages a (2R+1)^2 neighbourhood of the shadow, weighting each\n"
                               "neighbour by how well its depth agrees with this pixel's surface.\n"
                               "At one ray per pixel the shadow term is a hard 0 or 1, so a\n"
                               "penumbra comes out dithered rather than soft; this is what\n"
                               "resolves it without paying for more rays. Keeps no history, so\n"
                               "unlike amortisation above it costs nothing in lag under motion.");

        // ---- A third, separate denoiser (Settings::denoiserMaxSamples) ----
        // The two above filter the ray-traced sun shadow; this tunes the AMD FidelityFX Denoiser,
        // filtering the ReSTIR indirect-diffuse GI estimate behind GI page's "Denoiser (AMD
        // FidelityFX)" checkbox. Sits here with this page's other "Denoiser: ..." rows. VoxiRenderer
        // re-issues the denoiser tuning every setSettings() call, so it takes effect next frame, no
        // rebuild. Greyed with the checkbox's hard reason, like the rest of the denoiser.
        ImGui::Spacing();
        ImGui::TextUnformatted("Denoiser: AMD FidelityFX history (indirect diffuse GI)");
        ImGui::Separator();
        {
            const bool tuneGreyed = greysControl(er.denoiser.reason);
            ImGui::BeginDisabled(tuneGreyed);
            int maxSamples = static_cast<int>(s.denoiserMaxSamples);
            if (ImGui::SliderInt("History depth (frames)", &maxSamples, 1, 255)) {
                s.denoiserMaxSamples = static_cast<u32>(maxSamples);
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The denoiser's history length, in frames -- a latency/noise\n"
                                  "trade, not a dispatch toggle: higher converges quieter but lags\n"
                                  "longer behind a moving light or camera. Filters the indirect-\n"
                                  "diffuse GI estimate (the \"Denoiser (AMD FidelityFX)\"\n"
                                  "checkbox, Global Illumination page), not the sun shadow above.\n"
                                  "[1,255]; default 32.\n"
                                  "NOT captured to the project manifest -- like giSkyOcclusionRays/\n"
                                  "Tile, it has no manifest key (ProjectRenderApply.hpp's own\n"
                                  "capture rule), so it resets to the compiled default on the\n"
                                  "next project reload rather than round-tripping through .ocproject.");
            float clip = s.denoiserHistoryClipWeight;
            if (ImGui::SliderFloat("History clip width", &clip, 0.01f, 4.0f, "%.2f")) {
                s.denoiserHistoryClipWeight = clip;
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Width of the neighbourhood box the history is clipped to.\n"
                                  "Smaller rejects stale history harder (less ghosting, more\n"
                                  "grain); larger trusts it longer. [0.01,4]; default 0.5.\n"
                                  "Not captured to the project manifest (see above).");
            int sunSamples = static_cast<int>(s.denoiserSunMovingSamples);
            if (ImGui::SliderInt("History while sun moves (frames)", &sunSamples, 1, 255)) {
                s.denoiserSunMovingSamples = static_cast<u32>(sunSamples);
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("History cap while the sun moves (and one frame after), so a\n"
                                  "drag's bounce light does not lag the sun. At or above the\n"
                                  "history depth it has no effect. [1,255]; default 4.\n"
                                  "Not captured to the project manifest (see above).");
            ImGui::EndDisabled();
        }

        // ---- History resets (debug): one button per reset*history console command ----
        // Bisection order is GI, then RT, then the denoiser (EditorConsole.hpp). AO has no button of its own:
        // requestAoHistoryReset() shares RT's validity flag, so it'd duplicate Reset RT history.
        // Reset All mirrors resetallhistory: GI + RT + denoiser, not a redundant fourth AO call.
        ImGui::Spacing();
        ImGui::TextUnformatted("History resets (debug)");
        ImGui::Separator();
        if (ImGui::Button("Reset GI history")) vx.requestGiHistoryReset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Invalidates the ReSTIR-GI reservoir + surface history (giMode 1\n"
                              "only) on the next frame -- the same thing a viewport resize does to\n"
                              "this one resource. Try this first if a GI blotch/burn-in appears.");
        ImGui::SameLine();
        if (ImGui::Button("Reset RT history")) vx.requestRtHistoryReset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Invalidates RT shadow/reflection/sky-occlusion history on the next\n"
                              "frame (all three, plus AO, share one validity flag today). Try\n"
                              "this if resetting GI history alone did not clear the artifact.");
        ImGui::SameLine();
        if (ImGui::Button("Reset denoiser history")) vx.requestDenoiserHistoryReset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forces the AMD FidelityFX denoiser to throw away its own internal\n"
                              "temporal history on the next frame. Try this if neither GI nor RT history\n"
                              "reset cleared the artifact.");
        ImGui::SameLine();
        if (ImGui::Button("Reset All")) {
            vx.requestGiHistoryReset();
            vx.requestRtHistoryReset();
            vx.requestDenoiserHistoryReset();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs all three resets above at once. Not the first move during a\n"
                              "bisection -- it clears everything and says nothing about which\n"
                              "buffer was actually poisoned; try one at a time first.");

        // ---- Ray-driven rendering (experimental) ----
        // Which thing finds the first surface: everything downstream already runs in one
        // pixel-shader invocation, so this swaps the one stage still fixed-function. Measured
        // baseline is in the tooltip on purpose: trades hardware early-Z (a ray has no equivalent
        // of it) for a primary ray's cost -- an author deciding that deserves the number, not a shrug.
        ImGui::Spacing();
        // No longer "(experimental)": shipped default from Medium up (D3, this retune). Low is a
        // deliberate exception, kept on the rasteriser by product decision, not a hardware gap; see
        // ladder::rtRenderMode (QualityLadder.hpp) for the full reasoning, including what the
        // evidence for Low does and does not show.
        ImGui::TextUnformatted("Primary visibility");
        ImGui::SameLine();
        ImGui::TextDisabled("(default at Medium and above; Low rasterises)");
        ImGui::Separator();
        int mode = static_cast<int>(s.rtRenderMode);
        if (ImGui::Combo("Finds the first surface", &mode,
                          "Rasteriser\0Primary rays\0")) {
            s.rtRenderMode = static_cast<u32>(mode); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rasteriser is what every version of this engine has shipped, and\n"
                               "is Ray Tracing Low's own tier default (D3): the user's explicit\n"
                               "decision to keep Low on it, not a hardware limit. Primary rays\n"
                               "trace one ray per pixel to find the first surface instead, then\n"
                               "shade it exactly as the raster path does -- the default from\n"
                               "Medium up.\n\n"
                               "Measured baseline to beat: raster primary visibility plus\n"
                               "material shading is 9.2ms on ElectricDreams at 4x MSAA,\n"
                               "2750x1639; one extra shadow ray costs 1.6ms at the same size.");

        // ---- Milestone 1: staged ray-driven passes, an A/B over the combo above ----
        // Right under it for the "read together" reason Indirect light history sits under ReSTIR
        // visibility rays. Gated the same way, deliberately: er.rayDrivenStages mirrors
        // er.rtRenderMode's reason chain -- both are inert for the same reason, primary rays not active.
        // Shows REQUESTED, not the raw field, same reasoning as giRestirMaxHistory above.
        //
        // Value 2 (staged + half-rate GI) is the engine default since 2026-09-27; the measurements
        // are on Voxi.hpp's Settings::rayDrivenStages. Combo index equals the settings value (0/1/2).
        const bool stagesGreyed = greysControl(er.rayDrivenStages.reason);
        ImGui::BeginDisabled(stagesGreyed);
        int stages = static_cast<int>(er.rayDrivenStages.requested);
        if (ImGui::Combo("Ray-driven passes", &stages,
                          "Single pass\0Staged\0"
                          "Staged + half-rate GI (default)\0")) {
            s.rayDrivenStages = static_cast<u32>(stages); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.rt.rayDrivenStages");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How the ray-driven frame is split into GPU passes.\n\n"
                              "Staged runs the primary ray, lighting and shading as separate GPU passes\n"
                              "instead of one giant shader: the same image, measured 40-45%% faster.\n"
                              "Staged + half-rate GI (the default) also traces only half the GI rays each\n"
                              "frame (a checkerboard) and the denoiser rebuilds the other half: a further\n"
                              "~1.4 ms faster, within 0.4%% of Staged when still, slightly noisier in motion.\n"
                              "It needs ReSTIR GI and the denoiser on; without them it behaves as Staged.\n\n"
                              "Single pass is the one-shader baseline, kept as a fallback. Staged modes\n"
                              "are D3D12-only; other backends, or a pipeline/resource that is missing,\n"
                              "fall back to Single pass (logged once).\n\n"
                              "Round-trips as RENDER.RDSTAGES.");
        if (stagesGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.rayDrivenStages.reason));
        }

        // Bounces aren't on this page any more: they're a Path Tracing quantity that lives there
        // now, leaving them here disabled made the two look like one feature. See Settings::ptBounces.
        ImGui::EndDisabled();   // the inner grey opened beside the Quality combo above
        ImGui::EndDisabled();
    }

    if (page == 4) {
        // This combo is the real control now: voxi::Settings::pathTracing, round-tripped through
        // the manifest and exposed to C#. Used to have no relationship to modules/render.pt --
        // Voxi.cpp hard-coded NotImplemented regardless of hardware, clamping this to Off on every
        // device, forever. status() now mirrors PathTracer::init()'s DXR-1.1/SM-6.5/
        // DXC/compute gate field for field, unlocking exactly when the reference view can run.
        // The tier now drives the accumulator resolution too -- previously fixed at 480x270
        // magnified ~5.7x, which is what read as a shimmering mess under motion (motion restarts
        // accumulation every frame), not the sample count.
        const Status st = vx.status(Feature::PathTracing);
        ImGui::TextUnformatted(Renderer::featureName(Feature::PathTracing));
        featureStatusBadge(vx, Feature::PathTracing);
        // ptSceneViewUnavailable_ is a RUNTIME signal from PathTracer::init() (e.g. a DXC compile
        // failure) the static device caps above didn't predict; disabling on it too keeps this
        // combo from claiming a quality that isn't running. Not auto-reset to Off, so a stale
        // selection can sit here disabled until the user picks Off or reopens the project.
        ImGui::BeginDisabled(st != Status::Ready || ptSceneViewUnavailable_);
        int q = static_cast<int>(s.pathTracing);
        const char* qs[] = {"Off","Low","Medium","High","Epic"};
        if (ImGui::Combo("Quality", &q, qs, 5)) {
            s.pathTracing = static_cast<Quality>(q);
            changed = true;
            // THE SEAM: this is the one place a UI event turns into a request for PtSceneView.
            // syncPtSceneView() performs the actual RHI registration next onUpdate(), never here.
            ptSceneViewWantEnabled_ = (s.pathTracing != Quality::Off);
            // Quality::Low is 1, so the rung is one less; Off never reaches this branch.
            if (ptSceneView_ && s.pathTracing != Quality::Off)
                ptSceneView_->setQuality(static_cast<u32>(s.pathTracing) - 1);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turns on Aver.PathTracer's reference view: a still-camera, brute-\n"
                               "force render of the real scene through modules/render.pt.\n"
                               "SUPPRESSES the raster view entirely while on. Sky and sun light\n"
                               "only -- no CLight (point/spot/area) and no emissive term --\n"
                               "static geometry only, flat albedo only, no denoiser -- see\n"
                               "PtSceneView.hpp for the full list of what it deliberately does\n"
                               "not do.\n\n"
                               "HAS TIERS NOW: Low..Epic drive the reference view's accumulator\n"
                               "resolution (480x270 up to 1280x720, see Bounces below for the\n"
                               "bounce budget each rung buys) -- both this combo and the Overall\n"
                               "Quality preset on the General page set it, though Overall always\n"
                               "sets it to Off (a locked decision: a PT tier above Off takes over\n"
                               "the entire view, too large a side effect for one preset button).\n"
                               "The console's voxi.pathTracing/voxi.scalability reach it too.");
        ImGui::EndDisabled();

        // Bounces: the manifest always carried RENDER.PTBOUNCES (round-tripped by
        // OcProject/applyProjectRender, overridden by --pt-bounces), but nothing ever showed it --
        // only hand-editing the file or the CLI could set it.
        //
        // Tier-derived with an open knob (Settings::ptBounces/ptBouncesForQuality): picking a
        // Quality above re-derives this, so a hand-set value survives until the tier next changes.
        ImGui::BeginDisabled(s.pathTracing == Quality::Off);
        int bounces = static_cast<int>(s.ptBounces);
        if (ImGui::SliderInt("Bounces", &bounces, 1, 8)) {
            s.ptBounces = static_cast<u32>(bounces);
            changed = true;
        }
        uiReg_.track("project.pt.bounces");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Light paths after the first hit. Changing Quality above re-derives\n"
                              "this from the tier, so set it after picking one.");

        // A1: priority between these three used to be two `if`s (unavailable, else active) with no
        // way to say "suppressed" -- the combo would silently do nothing while ray-driven painted
        // the scene. choosePtViewTag() is the pure decision (PtRenderConflict.hpp); every ImGui
        // call stays here.
        switch (aver::editor::choosePtViewTag(ptSceneViewUnavailable_,
                                               ptSceneViewSuppressedByRayDriven_,
                                               ptSceneView_ != nullptr)) {
        case aver::editor::PtViewTag::Unavailable:
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[unavailable on this device]");
            break;
        case aver::editor::PtViewTag::SuppressedByRayDriven:
            // Same colour as [unavailable] above, deliberately: both mean "this combo isn't doing
            // anything right now", and a second colour would imply the two situations differ.
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1),
                                "[suppressed: ray-driven rendering is drawing]");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ray-driven primary visibility is painting the scene, and it "
                                   "always wins the\nelection over this view when both want the "
                                   "frame -- see syncPtSceneView().\nSet Ray Tracing > \"Finds "
                                   "the first surface\" to Rasteriser, above, to see this\nview "
                                   "instead.");
            break;
        case aver::editor::PtViewTag::Active:
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.45f,0.85f,0.45f,1), "[active]");
            ImGui::Text("%s, %u sample(s) accumulated",
                        ptSceneView_->sceneReady() ? "tracing" : "no static geometry captured yet",
                        ptSceneView_->samplesAccumulated());
            break;
        case aver::editor::PtViewTag::None:
            break;
        }
    }

    ImGui::PopItemWidth();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("GPU: MSAA %ux, RT tier %u, shader model %u, mesh-shader tier %u, DXC %s",
                        vx.deviceInfo().maxMsaaSamples, vx.deviceInfo().rayTracingTier,
                        vx.deviceInfo().shaderModel, vx.deviceInfo().meshShaderTier,
                        vx.deviceInfo().dxcAvailable ? "yes" : "no");
    ImGui::TextDisabled("Scriptable from C# via aver_voxi_* (Aver.Scripting)");
    ImGui::PopTextWrapPos();

    if (changed) {
        // Requested settings reach the manifest BEFORE setSettings can clamp them: "live" inside
        // captureRenderSettingsFromUi's own capture rule means "before this edit", which only holds
        // while this call runs first. overallFollowMask carries whatever an Overall button or
        // per-group row wrote this edit (0 otherwise), so those groups are captured as "follow the
        // tier", not nine pins.
        captureRenderSettingsFromUi(s, overallFollowMask);
        vx.setSettings(s);
    }
}

#endif
#endif

// Outside `#if AVER_WITH_IMGUI`, matching the declaration -- has to be both or neither: moving
// only the declaration left `no-ui` linking against an uncompiled definition (LNK2019 from
// maybeAutosavePrefs, called every quarter second in every configuration). No ImGui call here,
// only setPref* pushes. Writes every editor preference back and flushes; each setter compares
// before it stores.
void SandboxApp::saveEditorPreferences() {
    using namespace editor;
    setPrefBool ("contentBrowser.gallery",       cbGallery_);
    setPrefFloat("contentBrowser.tileSize",      cbTileSize_);
    setPrefBool ("contentBrowser.dblClickEnter", cbDoubleClickEnter_);
    setPrefFloat("drawers.heightFraction",       drawerFrac_);
    setPrefFloat("drawers.slideRate",            drawerRate_);
    setPrefBool ("outputLog.autoScroll",         logAutoScroll_);
    setPrefInt  ("outputLog.levelFilter",        logLevelFilter_);
    setPrefBool ("console.autoScroll",           consoleAutoScroll_);
    // Not written back from a --auto-compile run: that launch must not overwrite what the user chose
    // from the Tools menu before this session started.
    if (!autoCompileFromCli_) setPrefBool("scripting.autoCompile", autoCompile_);
    setPrefBool ("viewport.showGrid",            showGrid_);
    setPrefBool ("panels.worldOutliner",         showOutliner_);
    setPrefBool ("panels.details",               showDetails_);
    setPrefFloat("viewport.flySpeed",            flySpeed_);
    setPrefFloat("viewport.lookSensitivity",     lookSpeed_);
    setPrefFloat("ddc.ramBudgetMb",              ddcRamBudgetMb_);
    // Guarded exactly as the load is: a --frames run that wrote its --unlit back would leave
    // the next interactive session unlit, and the run after that with moved gates.
    if (maxFrames_ == 0) {
        if (!viewModeFromCli_) {
            setPrefBool("viewport.wireframe",     wireframe_);
            setPrefBool("viewport.unlit",         unlit_);
        }
        setPrefBool("viewport.showStaticMeshes",  showStaticMeshes_);
        setPrefBool("viewport.showAtmosphere",    showAtmosphere_);
    }
    setPrefBool ("snap.move",                    snapMove_);
    setPrefBool ("snap.rotate",                  snapRot_);
    setPrefBool ("snap.scale",                   snapScale_);
    setPrefFloat("snap.moveStep",                moveSnap_);
    setPrefFloat("snap.rotateStep",              rotSnap_);
    setPrefFloat("snap.scaleStep",               scaleSnap_);

    // Guarded like the load, same reason: a --frames run writing its CLI exposure back would leave
    // the next interactive session looking at the capture's eyes.
    //
    // The three with a manifest key are guarded a second time, symmetrically with the load: while
    // the project states RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE, this channel is inert both directions --
    // writing anyway would adopt the project's look as this user's default everywhere, and buy
    // nothing, since the load declines to restore it and applyProject overwrites it regardless.
    if (maxFrames_ == 0) {
        if (project_.postExposure < 0.0f)     setPrefFloat("post.exposure",     post_.exposure);
        if (project_.postAutoExposure < 0)    setPrefBool ("post.autoExposure", post_.autoExposure);
        if (project_.postBloom < 0.0f)      setPrefFloat("post.bloomIntensity", post_.bloomIntensity);
        setPrefFloat("post.nightVision", post_.nightVision);
        // Marks the one-time exposure migrations above as done, so a session that never touches
        // Brightness still leaves editor.ini past the version that would force them again.
        setPrefInt("post.settingsVersion", 3);
    }

    const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
    if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size()))
        setPrefString("contentBrowser.ide", ides[static_cast<usize>(cbIdeChoice_)].name);
    else
        setPrefString("contentBrowser.ide", "");   // Automatic

    // Guarded on maxFrames_ == 0 like every other capture-sensitive pref -- these two were not.
    // maybeAutosavePrefs() returns early on maxFrames_, but onShutdown() calls
    // saveEditorPreferences() unconditionally at the end of every capture/benchmark/gate run, so
    // `--frames N --render-scale F` or `--no-vsync` wrote its measurement settings into the same
    // %LOCALAPPDATA%/AverEngine/editor.ini an interactive session reads back -- every capture
    // harness in scripts/ passes --no-vsync, so vsync had been decided by whichever measurement
    // ran last.
    //
    // Guarded here rather than in onShutdown() so a future third caller can't reintroduce it, and
    // so the rule reads the same as its two neighbours.
    if (maxFrames_ == 0) {
        if (prefsDevice_ && prefsDevice_->vsyncCanDisable())
            setPrefBool("display.vsync", prefsDevice_->vsync());
        setPrefBool("display.frameGenWhileEditing", frameGenWhileEditing_);
        setPrefInt("display.frameGenTrajectory", frameGenTrajectory_);
        setPrefBool("display.frameGenTrain", frameGenTrain_);
        // The three AverSR keys below are skipped outright, not written a neutral value, when the
        // command line drove this session's render scale or AverSR level: an interactive `--aversr
        // quality` run has maxFrames_ == 0 too (it is not a --frames capture), so the outer guard
        // alone didn't stop it, and it used to write itself with no CLI check, so the NEXT ordinary
        // launch inherited a choice nobody made from the Display page. Skipping means a prior
        // session's real choice survives untouched.
        // Skipped entirely with the module off too, mirroring loadEditorPreferences' own
        // `#if AVER_MODULE_SR`: two of the three are written from AverSR-only state, and a build
        // that can't read them back shouldn't overwrite what one that can wrote there.
#if AVER_MODULE_SR
        if (!averSrFromCli_ && renderScaleOverride_ == 1.0f) {
            setPrefString("display.aversrChoice", editor::averSrChoiceName(averSrChoice_));
            if (prefsDevice_)
                // Through renderScaleToPersist, not the live device scale directly: persisting the
                // live scale for anything but Manual was the exact bug the plan's 10.1/3.3-A
                // corrections describe -- Auto would persist whatever fraction this session's rung
                // resolved to, and the crash-cookie render-scale block at load would apply that
                // stale fraction before Auto got to re-derive it.
                setPrefFloat("display.renderScale",
                            editor::renderScaleToPersist(averSrChoice_, prefsDevice_->renderScale()));
            // The upscaler's quality mirror: before display.aversrChoice existed this was never
            // persisted at all (reset to Off every launch, desynced from display.renderScale above
            // once it wasn't). display.aversrChoice is now the source of truth; this mirror just
            // backs it up, for at-a-glance reading of a raw editor.ini and so an older build reading
            // the same file still sees a level, not Off.
            // setPrefFloat, not an int helper, since none exists -- the prefs layer is float/bool/
            // string (every other numeric setting here goes through the same float pair), and the
            // enum's four values fit a float exactly.
            setPrefFloat("display.aversr", static_cast<f32>(static_cast<int>(averSrQuality_)));
        }
#endif  // AVER_MODULE_SR
    }

    setPrefInt   ("play.mode",           static_cast<i32>(playMode_));
    setPrefInt   ("play.spawnAt",        static_cast<i32>(playSpawnAt_));
    setPrefBool  ("play.gameGetsMouse",  playGameGetsMouse_);
    setPrefBool  ("play.defaultPawnWalk", defaultPawnWalk_);
    setPrefString("play.standaloneArgs", playStandaloneArgs_);

    // The one line in this function that needs the editor UI: keybinds_ is the chord registry the
    // ImGui layer owns. Everything else is a plain setPref* call, so the guard sits only here.
#if AVER_WITH_IMGUI
    keybinds_.saveToPrefs();
#endif

    flushEditorPrefs();
}

} // namespace aver
