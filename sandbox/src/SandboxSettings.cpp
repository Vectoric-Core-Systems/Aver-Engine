// Editor: editor preferences and every settings page.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

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
    showColliders_      = prefBool ("viewport.showColliders",        showColliders_);
    wireframe_          = prefBool ("viewport.wireframe",            wireframe_);
    // Toggled from the Window menu rather than the Preferences panel, so these ride
    // onShutdown's sync rather than buildEditorPrefs' own save-on-close.
    showOutliner_       = prefBool ("panels.worldOutliner",          showOutliner_);
    showDetails_        = prefBool ("panels.details",                showDetails_);
    flySpeed_           = prefFloat("viewport.flySpeed",             flySpeed_);
    lookSpeed_          = prefFloat("viewport.lookSensitivity",      lookSpeed_);
    // The three view flags and the snap grid: state the editor already had and already showed,
    // and the only reason it was lost was that nobody had written these lines.
    //
    // THE VIEW FLAGS ARE INTERACTIVE-ONLY, both directions, for the reason the post-process
    // block above gives at length: they change the IMAGE, --unlit and the Show menu can set
    // them, and a stored one reaching a --frames run would move all twenty gate probes on one
    // machine and not another. Snap is not gated -- nothing on the command line touches it and
    // it cannot alter a pixel.
    if (maxFrames_ == 0) {
        unlit_            = prefBool("viewport.unlit",              unlit_);
        showStaticMeshes_ = prefBool("viewport.showStaticMeshes",   showStaticMeshes_);
        showAtmosphere_   = prefBool("viewport.showAtmosphere",     showAtmosphere_);
    }
    snapMove_           = prefBool ("snap.move",                      snapMove_);
    snapRot_            = prefBool ("snap.rotate",                    snapRot_);
    snapScale_          = prefBool ("snap.scale",                     snapScale_);
    moveSnap_           = prefFloat("snap.moveStep",                  moveSnap_);
    rotSnap_            = prefFloat("snap.rotateStep",                rotSnap_);
    scaleSnap_          = prefFloat("snap.scaleStep",                 scaleSnap_);

    // POST PROCESS IS A PROPERTY OF THE VIEW, NOT OF THE WORLD, which is why these nine live
    // here and not in the level file: docs/EDITOR.md notes exposure is one histogram over the
    // whole target, and two people opening the same level should not inherit each other's eyes.
    // Every one of them was lost on exit before this.
    //
    // NOT ON A CAPTURE RUN. maxFrames_ != 0 is this file's established test for "not
    // interactive" and is the same one applyCaptureExposureRule uses, so the two agree by
    // construction rather than by discipline. docs/STATUS.md puts the reason plainly -- "A post
    // chain whose default state changed the image would invalidate the whole oracle" -- and a
    // stored exposure reaching a --frames run would do exactly that: on one machine and not on
    // another, which is the worst shape a gate failure can take.
    //
    // AND NOT WHEN THE PROJECT STATES THE KEY, which is the second guard on the three fields that
    // now have one. A project named on the COMMAND LINE is applied in onInit (SandboxApp.cpp:763),
    // before the first buildUI and therefore before this function has ever run -- so without this
    // test a stored post.exposure would silently overwrite RENDER.EXPOSURE one frame after
    // applyProject wrote it, and the key would look inert on every machine whose editor.ini has a
    // post block in it. The browser path already resolves the other way round (prefs load at the
    // top of buildUI, the Open click applies the project later in that same frame), so without
    // this the two ways of opening the same project would disagree about the same file; with it,
    // both read command line > manifest > my own stored preference.
    if (maxFrames_ == 0) {
        if (!postExposureFromCli_ && project_.postExposure < 0.0f)
            post_.exposure       = prefFloat("post.exposure",       post_.exposure);
        if (!postAutoExpFromCli_ && project_.postAutoExposure < 0)
            post_.autoExposure   = prefBool ("post.autoExposure",   post_.autoExposure);
        if (!postBloomFromCli_ && project_.postBloom < 0.0f)
            post_.bloomIntensity = prefFloat("post.bloomIntensity", post_.bloomIntensity);
        post_.exposureKey    = prefFloat("post.exposureKey",    post_.exposureKey);
        post_.exposureSpeed  = prefFloat("post.exposureSpeed",  post_.exposureSpeed);
        post_.exposureMin    = prefFloat("post.exposureMin",    post_.exposureMin);
        post_.exposureMax    = prefFloat("post.exposureMax",    post_.exposureMax);
        // A STORED 8 IS THE OLD DEFAULT, NOT A CHOICE: every session before 2026-09-24 saved the
        // compiled-in 8 back on exit, so every existing editor.ini holds it and would pin the camera
        // at the ceiling the physical sky outgrew (RHI.hpp's own comment on exposureMax). Exactly 8
        // is read as "never chosen" and takes the new default; any other stored value is kept.
        if (post_.exposureMax == 8.0f) post_.exposureMax = rhi::PostSettings{}.exposureMax;
        post_.bloomThreshold = prefFloat("post.bloomThreshold", post_.bloomThreshold);
        post_.bloomKnee      = prefFloat("post.bloomKnee",      post_.bloomKnee);
        // THE TWO FIELDS OF PostSettings THAT WERE STORED NOWHERE. Every other member of the
        // struct rides in this block; these two were simply missed, so the auto-exposure
        // histogram window reset to its compiled-in default on every launch while the nine
        // knobs around it persisted.
        post_.histogramLowPercent  = prefFloat("post.histogramLow",  post_.histogramLowPercent);
        post_.histogramHighPercent = prefFloat("post.histogramHigh", post_.histogramHighPercent);
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
    // A STORED RENDER SCALE IS APPLIED BEHIND A CRASH COOKIE: applying one below 1 can lose the
    // GPU device, and a persisted setting that kills the device at startup is a trap with NO WAY
    // OUT FROM INSIDE THE EDITOR -- one evening was lost to exactly that after the AverSR combo
    // persisted a 0.67 that then bricked every launch.
    // WHY A COOKIE AND NOT A HANDLER: Engine::frameStep returns as soon as deviceLost() is true,
    // BEFORE onUpdate(), so there is no "undo it" hook to hang this on -- only what was already
    // written to disk survives. The flag goes down BEFORE the risky call and clears thirty frames
    // later once presenting has demonstrably worked.
    // The scale resets to 1 rather than merely skipped, so the user sees and can change it.
    // THE UNDERLYING BUG IS FIXED (setRenderScale now parks and rebuilds at the next beginFrame),
    // but this stays: it costs one bool, and it's the only rescue for an editor.ini written by a
    // build that HAD the bug.
    // optimisation-wave-2, 3.3 A: THE CHOICE (AverSrChoice.hpp), migrated once off the pre-Auto
    // display.aversr/display.renderScale pair below if display.aversrChoice has never been
    // written -- see AverSrChoice.hpp's own top comment for why the migration itself is a pure
    // function tested in isolation (AverSrChoiceTest.cpp) rather than inlined here.
    //
    // --render-scale ON THE COMMAND LINE WINS OUTRIGHT (this whole block is skipped, the same
    // guard the pre-Auto render-scale restore used); --aversr LEVEL or --aversr auto wins too
    // (averSrFromCli_) -- loadEditorPreferences leaves BOTH the render scale and the AverSR choice
    // alone in either case, matching "the command line wins" everywhere else in this file.
    // THE WHOLE RESTORE IS AverSR's, so the whole restore is guarded rather than the four members
    // the compiler happened to name first. display.aversrChoice, display.aversr and
    // display.renderScalePending are the three keys the AverSR combo owns (saveEditorPreferences
    // calls them "the three AverSR keys" in as many words), and every branch below either resolves
    // an aver::sr::Quality or applies one through applyAverSrQuality, which is itself declared
    // `#if AVER_MODULE_SR`. With the module out there is no level to restore, and the load skips
    // the same keys the save skips -- symmetric, rather than reading back a preference nothing in
    // the build can act on.
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
            // A STORED RENDER SCALE IS APPLIED BEHIND A CRASH COOKIE: applying one below 1 can
            // lose the GPU device, and a persisted setting that kills the device at startup is a
            // trap with NO WAY OUT FROM INSIDE THE EDITOR -- one evening was lost to exactly that
            // after the AverSR combo persisted a 0.67 that then bricked every launch.
            // WHY A COOKIE AND NOT A HANDLER: Engine::frameStep returns as soon as deviceLost() is
            // true, BEFORE onUpdate(), so there is no "undo it" hook to hang this on -- only what
            // was already written to disk survives. The flag goes down BEFORE the risky call and
            // clears thirty frames later once presenting has demonstrably worked.
            // The scale resets to 1 rather than merely skipped, so the user sees and can change it.
            // THE UNDERLYING BUG IS FIXED (setRenderScale now parks and rebuilds at the next
            // beginFrame), but this stays: it costs one bool, and it's the only rescue for an
            // editor.ini written by a build that HAD the bug. UNCHANGED from before Auto existed
            // (3.3 A's own "Manual: the existing crash-cookie dance... unchanged" rule) -- this is
            // the ONLY choice that still restores a raw display.renderScale directly; every named
            // level below re-derives its own canonical scale through applyAverSrQuality instead.
            const f32 stored = prefFloat("display.renderScale", prefsDevice_->renderScale());
            if (stored == 1.0f) {
                prefsDevice_->setRenderScale(stored);         // early-outs; costs nothing
            } else if (pending && !renderScaleCookieArmed_) {
                // `&& !renderScaleCookieArmed_`: a cookie THIS process armed is not evidence that the
                // PREVIOUS launch crashed -- see updateAverSrAuto's prefsLoaded_ gate for the frame-1
                // ordering that once made exactly that misreading happen on every launch.
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
            // (`&& !renderScaleCookieArmed_`: same reason as the Manual branch above -- a cookie this
            // process armed itself says nothing about the previous launch.)
            // A NAMED LEVEL (or a level Auto resolved to in a PRIOR session) DID NOT SURVIVE ITS
            // OWN LAUNCH -- the same crash this cookie already protects Manual's raw scale from,
            // for a level applied through applyAverSrQuality instead of a raw setRenderScale.
            // Force Off for THIS launch (never re-attempt the level that just crashed) rather than
            // merely resetting a number, since there is no single "the scale" to reset back to 1
            // the way Manual's own branch does -- and latch it so the Project Settings/Display
            // surfaces can say why AverSR reads Off when the stored choice says otherwise.
            averSrCookieTripped_ = true;
            AVER_CRITICAL("[AverSR] the last launch did not survive applying {} -- forcing Off "
                          "for this launch. Choose a level again if that was not the cause.",
                          editor::averSrChoiceName(averSrChoice_));
            setPrefBool("display.renderScalePending", false);
            flushEditorPrefs();
            applyAverSrQuality(prefsDevice_, aver::sr::Quality::Off);
        } else if (averSrChoice_ == editor::AverSrChoice::Auto) {
            // NOTHING HERE: updateAverSrAuto (onUpdate) applies Auto once a project's settings
            // exist to derive a level from -- this load runs before any project is open, so there
            // is nothing yet for autoAverSrLevel to read.
        } else {
            // Off/Quality/Balanced/Performance: applyAverSrQuality re-derives the level's OWN
            // canonical scale (aver::sr::renderScaleFor) rather than trusting a stored
            // display.renderScale that may have drifted from it -- restoring the quality is what
            // should own the scale, the same reasoning the pre-Auto code gave for ordering this
            // block after the render-scale one.
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
        // THE SAME PERSISTED VARIABLE as the Output Log drawer's own combo, so it has to offer
        // the same entries. This copy stopped at Warn+ when Error+ and Critical+ were appended
        // to the other one: choosing Error+ in the drawer left this preview blank, and this
        // control could then only ever set the filter back to one of the first three.
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

#if AVER_MODULE_SR
        // optimisation-wave-2, U2/3.3 A: AverSR now has a per-rung default (every Overall rung
        // has a level, QualityLadder.hpp's ladder::averSrLevel), so this combo's job changed from
        // "pick a level" to "pick a SOURCE for the level": Auto follows the SAME CLI > Display
        // choice > project manifest > ladder chain updateAverSrAuto resolves every frame (and can
        // move under it -- a scalability button moving the Overall rung, say), the four named
        // items pin one level outright the way this combo always could, and "Manual scale" is
        // what dragging the Render Scale slider below sets on its own.
        //
        // The Auto item's OWN label is computed fresh every time this window draws (cheap; this
        // window is not open every frame), not read off averSrQuality_/averSrSource_ directly --
        // those two reflect whatever averSrChoice_ CURRENTLY is, which might not be Auto, and the
        // item still has to preview what picking Auto would resolve to right now.
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
                    // AN EXPLICIT PICK LIFTS THE CRASH-COOKIE LATCH. The CRITICAL line that sets it
                    // tells the user to "choose a level again if that was not the cause" -- which
                    // did nothing while updateAverSrAuto kept forcing Off for the whole session.
                    averSrCookieTripped_ = false;
                    averSrChoice_ = static_cast<editor::AverSrChoice>(i);
                    // Auto and Manual apply NOTHING here: Auto is picked up by updateAverSrAuto
                    // next frame (it needs vx.settings()/deviceInfo(), not available mid-UI-draw
                    // the same way applyAverSrQuality below is), and Manual keeps whatever scale
                    // the slider below is already at, or is about to be dragged to.
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
                              "was recorded pre-ReSTIR/pre-NRD; aver-aversr-measured.md records\n"
                              "this figure as unverified.");
        if (averSrMigrationNoteArmed_)
            ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                "AverSR now defaults to Auto (%s). Choose Off for native resolution.",
                aver::sr::qualityName(averSrQuality_));
#endif
        // Render scale: the 3D scene's own resolution as a fraction of the window's. 1.0 (right
        // edge) is the pre-existing behaviour -- the scene renders 1:1 -- and everything below
        // trades scene sharpness for pixel-bound pass cost. The editor UI itself never moves.
        float rs = prefsDevice_ ? prefsDevice_->renderScale() : 1.0f;
        if (ImGui::SliderFloat("Render Scale", &rs, 0.25f, 1.0f, "%.2f") && prefsDevice_) {
            prefsDevice_->setRenderScale(rs);
#if AVER_MODULE_SR
            // 3.3 A: dragging this slider is what "Manual scale" means -- a number no named level
            // produced. Cleared the migration note too: the slider is as much a deliberate choice
            // as picking a combo item is.
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
        // THE OTHER THREE VIEW FLAGS, which sat beside the two above in the Show menu and were
        // the only ones not remembered -- turn off Atmosphere, close the editor, and it came
        // back on. Same two-line pattern as Show grid and Wireframe; nothing new was needed.
        ImGui::Checkbox("Unlit", &unlit_);
        uiReg_.track("prefs.viewport.unlit");
        ImGui::Checkbox("Show static meshes", &showStaticMeshes_);
        uiReg_.track("prefs.viewport.showStaticMeshes");
        ImGui::Checkbox("Show atmosphere", &showAtmosphere_);
        uiReg_.track("prefs.viewport.showAtmosphere");
        // THE SAME DIAL AS THE VIEWPORT CHIP, not the raw rate. Two places showing one setting
        // in two different units is how a person ends up believing they are two settings -- and
        // this one is the more misleading of the pair, since it is the page you go to when the
        // chip's number is not what you expected.
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

        // SNAP, WHICH THE TOOLBAR COULD SET AND NOTHING COULD KEEP. The increments live in
        // moveSnap_/rotSnap_/scaleSnap_ and the toolbar dropdowns write them, but neither the
        // toggles nor the values were in loadEditorPreferences or saveEditorPreferences -- so a
        // grid somebody set up for a level was gone the next time they opened the editor.
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
    // --scroll-prefs-to-keybinds: a one-shot verification aid: Preferences has grown to six
    // DefaultOpen sections, more than fit one screen, and there is no human here to scroll. Fires
    // once (consumes its own flag) so it never fights a person who scrolls the window themselves.
    if (ImGui::CollapsingHeader("Derived Data Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Baked global illumination, cached beside the project under");
        ImGui::TextDisabled("DerivedDataCache\\GI. Derived data: deleting it costs one rebuild.");
#if AVER_MODULE_VOXI
        // IN MB, because a budget in bytes is a number nobody can read at a glance. The floor
        // is one volume's worth: below that every bake would flush immediately and the buffer
        // would do nothing at all except add a copy.
        if (ImGui::SliderFloat("Memory budget", &ddcRamBudgetMb_, 32.0f, 4096.0f, "%.0f MB",
                               ImGuiSliderFlags_Logarithmic))
            voxiRenderer_.setGiCacheRamBudget(static_cast<u64>(ddcRamBudgetMb_) * 1024ull * 1024ull);
        uiReg_.track("prefs.ddc.ramBudget");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bakes are held in memory and written in batches, rather than one\n"
                              "~18 MB file per bake. They go to disk when this is exceeded, and\n"
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

    // The scroll lands HERE, on the section it names. It used to fire immediately above the
    // Derived Data Cache header, so --scroll-prefs-to-keybinds put the DDC section at the top of
    // the window and left Keybinds below the fold -- which is exactly what the flag exists to
    // avoid, and it went unnoticed because a screenshot of the wrong section still looks like a
    // screenshot of a section.
    if (scrollPrefsToKeybinds_) { ImGui::SetScrollHereY(0.0f); scrollPrefsToKeybinds_ = false; }
    // CALLED WHETHER OR NOT IT IS OPEN, passing which. Collapsing the header mid-capture used to
    // strand `listening_`: the section stopped being drawn, so the capture loop that clears it
    // never ran again, and the row still said "Press a chord..." when the header was reopened.
    // The state belongs to the registry, so the registry is what clears it.
    const bool keybindsOpen = ImGui::CollapsingHeader("Keybinds", ImGuiTreeNodeFlags_DefaultOpen);
    keybinds_.drawPreferencesSection(dpi_, keybindsOpen, &uiReg_);
    ImGui::Separator();
    ImGui::TextDisabled("Preferences apply immediately and are saved for next time.");
    ImGui::TextDisabled("%s", editor::editorPrefsPath().c_str());
    ImGui::End();

    saveEditorPreferences();
}

// Draws the Project Settings window: a category sidebar beside the selected settings page.
// Window > World Settings: per-LEVEL settings versus Project Settings' per-project ones -- one
// project routinely holds a menu level, a gameplay level and a test level needing different rules.
// EVERY VALUE HERE EDITS SOMETHING THE LEVEL FILE ALREADY CARRIES, no parallel setting: a
// window-only copy would be the two-sources-of-truth trap the Player Start marker avoids.
void SandboxApp::buildWorldSettings() {
    if (!showWorldSettings_) return;
#if !AVER_MODULE_SCENE
    // WITHOUT THE SCENE MODULE THERE IS NO LEVEL to have settings for -- levelPath_/levelName_
    // are themselves scene-guarded. Saying so beats hiding the menu entry: a person who opens it
    // deserves the reason it is empty rather than a menu item that silently does nothing.
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
        // BY NAME, because that is what the file stores and what survives a restart: framework
        // class handles come from aver_fw_class_declare at runtime and are process-local, so a
        // number written into a level would mean something else next launch.
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
        // ONE ROW PER FIELD, because the radius is per field: a single level-wide "streaming
        // radius" is exactly the setting this engine deliberately does not have -- one radius
        // cannot serve a dense ground cover and a sparse canopy at once.
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
    // SIBLINGS OF RENDERING, not children of it: neither is drawn by the renderer, and nesting
    // them under it would say they were.
    if (ImGui::Selectable("Physics",                settingsPage_==5)) settingsPage_=5;
    if (ImGui::Selectable("Audio",                  settingsPage_==6)) settingsPage_=6;
    // THREE CATEGORIES A PROJECT COULD NOT STATE AT ALL until now. Each corresponds to a real
    // options struct the engine already had and no file could reach: platform::WindowDesc,
    // the four importers' options, and world::StreamSettings.
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
            // EDITABLE AT LAST. This page was five ImGui::Text lines, so a project's author,
            // start map and name could not be changed from the editor at all -- while
            // saveProjectManifest has always written the whole desc through writeOcproject,
            // which already owns NAME, AUTHOR, CONTENT and STARTMAP. Only the widgets were
            // missing; nothing in the format or the save path needed touching.
            ImGui::PushItemWidth(-160.0f * dpi_);
            if (editField("Name", project_.name, 96)) projectDirty_ = true;
            uiReg_.track("project.name");
            // The rename is IN THE MANIFEST ONLY, and the consequences are worth stating rather
            // than leaving to be discovered: the folder and the .ocproject filename keep the old
            // name, and project_.name also feeds csharpNamespaceFor(), which decides the
            // namespace of NEWLY created materials and scripts -- so existing files stay in the
            // old namespace and new ones land in another.
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Renames the project in the manifest only.\nThe folder and file keep their names, and new scripts get a namespace from this.");
            if (editField("Author", project_.author, 96)) projectDirty_ = true;
            uiReg_.track("project.author");
            if (editField("Start map", project_.startMap, 256)) projectDirty_ = true;
            uiReg_.track("project.startMap");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Relative to the content root, e.g. Maps/Default.ocmap");

            // A COMBO, not free text like Start map above: INPUT.SCHEME names a scheme by
            // CONTENT-RELATIVE PATH (OcProject.hpp's own comment on the field), and a typo here
            // fails SILENTLY at runtime -- no context is ever pushed -- rather than refusing to
            // open the way a bad Start Map at least tries to load and errors. Scanned fresh each
            // time the combo opens, refreshFoliagePalette's own recursive_directory_iterator
            // pattern (SandboxViewport.cpp), sized for the same kind of project: a handful of
            // .ocinput files, never thousands.
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

            // READ-ONLY ON PURPOSE, both of them. ENGINE is what the project needs at least, not
            // a preference; and changing CONTENT mid-session would move where every asset
            // resolves from with nothing re-mounting behind it.
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

    // ---- THE SAVE FOOTER, ON EVERY PAGE ---------------------------------------------------
    //
    // It used to live inside the Description page and nowhere else. Every other page sets
    // projectDirty_ correctly -- gravity, the audio mix, every render knob -- so editing any of
    // them armed a save whose button was two clicks away on a different page, with nothing on
    // screen saying so. The most common outcome is the obvious one: the edit is made, the window
    // is closed, and the change is gone.
    //
    // THIS IS NO LONGER THE ONLY WAY TO SAVE, and the comment that used to sit here argued it
    // should be: a .ocproject is source-controlled, so writing it on every slider drag makes
    // noise nobody asked for. True, and it still lost -- a settings page that needs a separate
    // click to mean anything gets edited and closed, and the edit is gone. See
    // maybeAutosaveProject, which now writes half a second after the last change. The button
    // remains for anyone who wants to save deliberately, and the dirty mark below still shows
    // the brief window before the debounce fires.
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

        // REVERT IS RE-READ, not an undo stack. saveProjectManifest's inverse is simply loading
        // the file again, and a manifest is small enough that re-reading it is exact where a
        // remembered snapshot would drift the moment a field is added.
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

// ---- THE FIVE PAGES THAT ARE NOT THE RENDERING PAGE ------------------------------------------
// Window, Import, Streaming, Physics and Audio were inside the `#if AVER_MODULE_VOXI` above purely
// because buildRenderingSettings is, and they were written next to it. None of them mentions a
// voxel: they edit WINDOW.*/IMPORT.*/STREAM.*/PHYSICS.*/AUDIO.* manifest keys, and buildSettings()
// dispatches to all five from an else-if chain gated on ImGui alone -- so -DAVER_MODULE_VOXI=OFF
// (which -DAVER_MODULE_PBR=OFF forces too) deleted five definitions while their five calls stayed.
// The Rendering page reopens the guard below, where it belongs, and buildSettings' own `#else`
// already says what that page shows without the module.
//
// A small helper for the three pages below: an int field that is UNSTATED at -1 rather than
// zero, which is the convention every RENDER.*/STREAM.* key uses. Without the checkbox there is
// no way to author "this project does not state a value", and a project that states everything
// pins defaults it never meant to pin.
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

// ---- Window: how a shipped game presents itself ---------------------------------------------
//
// docs/PACKAGING.md named this gap in as many words: ".ocproject cannot describe a shipped game.
// It has no entry point, no window/resolution defaults, no build id, no icon." The old design
// put them in a side-car game.json, for which no reader or writer ever existed.
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
    // These used to read "RECORDED, NOT YET CONSUMED". They are consumed now -- GameStreaming::enable
    // seeds every field's ChunkWorldSettings::stream from the manifest -- and the two things a user
    // still needs told are the ones that would otherwise look like the dial being ignored: the
    // settings are read when streaming is enabled rather than live, and a PCGVOLUME that states its
    // own radius beats the project-wide radii.
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
    // NOT READY IS A REAL STATE, not an error: a project can be open before anything has called
    // aver_phys_init. Every setter below would silently no-op, so the page says so instead of
    // offering controls that do nothing.
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

    // SECONDS ON THE WIRE, HERTZ ON SCREEN. The ABI takes a step in seconds; nobody reasons in
    // 0.0167. The conversion happens here rather than in the format so the file stays the ABI's
    // own unit.
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

    // ONE PRESENCE FLAG FOR THE WHOLE MIX -- see OcProjectDesc::hasAudioMix. Touching any
    // slider makes the project state a mix, because a muted bus and an unstated one have to
    // stay different things.
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
    // Every prerequisite this page's controls grey against, resolved ONCE per frame from the
    // settings this page itself is about to edit -- see RenderSettingsResolver.hpp's own comment
    // for why the same question answered four different ways (a setSettings clamp, an ad hoc
    // BeginDisabled check, a console var's validate(), and nothing at all for a load-time warning)
    // is the defect this header exists to remove.
    const Resolution er = resolve(s, vx.deviceInfo());
    bool changed = false;
    // Set by an Overall Quality button or one of the three per-group rows below (section 3): which
    // groups this edit just moved to a rung, so captureRenderSettingsFromUi's call to
    // captureVoxiSettings (Lane 2) knows to write "follow the tier" (-1) for those groups' knobs
    // rather than nine explicit pins.
    u32 overallFollowMask = 0;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

    if (page == 1) {
        // ---- OVERALL QUALITY (D2): UE-STYLE ENGINE SCALABILITY PRESET --------------------------
        // One button moves every group this project has a LADDER for -- Global Illumination, Ray
        // Tracing, Path Tracing -- to the SAME rung at once (Scalability.hpp). MSAA, AverSR,
        // shadows and post are deliberately not among them; see that header's own "Groups excluded,
        // and why" for the full list this comment does not repeat. Placed at the very top of this
        // page, before even the MSAA radios, the way UE's own Scalability panel leads its settings.
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
            // AMBER "CUSTOM" rather than leaving all four rungs unhighlighted with no explanation:
            // overallFromSettings reads Custom the moment any available group's knobs have drifted
            // off the ladder, the groups disagree on a rung, or Path Tracing is above Off.
            if (cur == OverallQuality::Custom) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1), "Custom");
            }
            // T-7: EVERY rung above turns Path Tracing OFF (scalabilityRung's own comment, a locked
            // decision) -- a PT tier above Off always reads Custom and would silently drop to Off
            // the moment any button above is clicked. Said here, in amber, rather than discovered
            // by the path-traced view vanishing with no visible cause.
            if (s.pathTracing != Quality::Off)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                                   "Picking a preset turns Path Tracing off (currently %s).",
                                   Renderer::qualityName(s.pathTracing));
        }

#if AVER_MODULE_SR
        // ---- AverSR's OWN default (U2, 3.3 A) -- NOT one of the three groups above -------------
        // AverSR sits outside Overall/Custom detection on purpose (module boundary,
        // Scalability.hpp's own header comment): an Overall preset moves GI/RT/PT together and
        // never touches this. This is the one place a pinned AverSR level that has drifted from
        // the rung's own default is visible at all.
        {
            const char* rungName = averSrAutoRungName(s, vx.deviceInfo());
            std::string sourceText = averSrSource_ == voxi::AverSrSource::Auto
                ? (std::string("Auto from ") + rungName)
                : (averSrSource_ == voxi::AverSrSource::ForcedOff
                       ? (edgeAaEnabled_ ? "--edge-aa" : "forced Off after a failed launch")
                       : averSrSourceText(averSrSource_));
            ImGui::Text("Upscaling: AverSR %s (%s)", aver::sr::qualityName(averSrQuality_),
                        sourceText.c_str());
            // "(differs from the <rung> preset's default, <level>)" -- only meaningful once the
            // resolved level did NOT come from Auto (2.3/10.2's Custom-detection gap: Custom can
            // hide a pinned-off AverSR, and this is the line that shows it).
            if (averSrSource_ != voxi::AverSrSource::Auto) {
                const u32 rungLevel = voxi::autoAverSrLevel(s, vx.deviceInfo());
                if (rungLevel != static_cast<u32>(averSrQuality_))
                    ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                        "(differs from the %s preset's default, %s)", rungName,
                        aver::sr::qualityName(static_cast<aver::sr::Quality>(rungLevel)));
            }
            // ONE-TIME NOTE (3.3 A): the same "until the user picks any item" flag the Display
            // combo shows and clears -- shown here too so a person who never opens the Display
            // page still sees why AverSR turned on.
            if (averSrMigrationNoteArmed_)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                    "AverSR now defaults to Auto (%s). Choose Off for native resolution.",
                    aver::sr::qualityName(averSrQuality_));
            // project_.averSr's own live edit state -- captured beside project_.occlusionCull in
            // captureRenderSettingsFromUi (3.3 A). -1 ("Follow Overall preset") is itself the
            // explicit default, not "unstated": index 0 in this combo, not a blank/no-selection.
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

        // The three groups an Overall preset moves together, each as its own Off..Epic row: this
        // is where a project that reads Custom above actually lives, and "(modified)" flags a
        // group that is nominally AT the right tier but has a hand-edited knob (groupFollowsLadder,
        // Scalability.hpp) -- nominally-Epic-but-modified and actually-Custom look identical from
        // the Overall row alone, and this is the page that tells them apart.
        {
            static const ScalabilityGroup kGroups[] = {ScalabilityGroup::GlobalIllumination,
                                                       ScalabilityGroup::RayTracing,
                                                       ScalabilityGroup::PathTracing};
            static const Feature kGroupFeature[] = {Feature::GlobalIllumination, Feature::RayTracing,
                                                    Feature::PathTracing};
            static const char* kGroupLabel[] = {"Global Illumination", "Ray Tracing", "Path Tracing"};
            static const char* kTierNames[] = {"Off", "Low", "Medium", "High", "Epic"};
            // Writes ONE group's tier and its own derived knobs to a rung -- the per-group
            // equivalent of applyOverall (Scalability.hpp), which always moves all three together
            // and so cannot serve a single row here. Mirrors applyOverall's own field list exactly,
            // group for group.
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
        // er.rtRenderMode.effective, not the raw request: what matters here is whether the ray
        // pass is ACTUALLY the thing finding primary visibility this frame -- it is one fullscreen
        // triangle with no per-triangle coverage, so it always runs at a single sample regardless
        // of what is picked above (Settings::rtRenderMode's own "WHAT DEFAULTING TO IT TRADES
        // AWAY" list). RT Low rasterises (D3), so this note does not apply there even though the
        // RT tier itself is not Off.
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
        // THE LAYERED BSDF LIVES ON THIS PAGE, not its own: it changes what EVERY material-shaded
        // draw computes, like MSAA or mesh shaders.
        // IT DOES NOT TAKE EFFECT UNTIL THE PROJECT IS RELOADED, stated on the control: Off
        // compiles the coat lobe out entirely, and VoxiRenderer builds and latches one shader set
        // per setting -- a live toggle would double a 20+ PSO matrix at startup for every project.
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
        // Compared against what the RENDERER latched, not against the last value this combo
        // wrote, so it is still shown after the settings window is closed and reopened.
        if ((s.layeredBsdf != Quality::Off) != voxiRenderer_.layeredBsdfActive())
            ImGui::TextWrapped("Takes effect when the project is reloaded; the shaders compiled "
                               "for this session are unchanged.");

        // ---- refraction, and three culling knobs that were CLI-only ------------------------
        //
        // All four were live per-frame state with no control anywhere: a flag on the command
        // line could set them and nothing could record the choice. Each is one manifest key.
        ImGui::Separator();
        ImGui::TextUnformatted("Refraction");
        {
            // BeginCombo/Selectable rather than a plain Combo: only the RAY-TRACED entry has a
            // prerequisite (RT hardware, RT tier not Off -- er.refractionMode.reason), and a plain
            // Combo has no way to grey out one entry while leaving Off and Screen-space selectable.
            // A request already at or below Screen-space never needed a ray tracer and is never
            // touched by this reason at all (RenderSettingsResolver.hpp's resolve(), refractionMode).
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

        // ---- THE POST CHAIN, THE ONE PART OF THE IMAGE THE FILE COULD NOT HOLD ---------------
        // Four keys that existed only as command-line flags and as live sliders in the Post
        // Process panel: set an exposure there, package the game, and the game rendered
        // rhi::PostSettings' compiled defaults instead -- docs/RUNTIME-DEDUP.md carried that as
        // "A project cannot author exposure/bloom/tonemap". The stated/unstated idiom is
        // buildImportSettings' and settingInt's (see settingInt's own comment for why -1 rather
        // than 0 means unstated, and why a project that states everything pins defaults it never
        // meant to pin).
        //
        // ON GENERAL, not beside the ReSTIR sliders on the Global Illumination page whose layout
        // these copy: filing exposure under Global Illumination would repeat precisely the mistake
        // the "Indirect diffuse" combo's own comment on that page describes -- a control filed
        // under its implementation instead of its job, reported as "I cannot find that project
        // setting". Exposure, bloom and the tone curve belong to the camera, like MSAA and the
        // shading model above them.
        //
        // EVERY CONTROL WRITES THE KEY AND THE LIVE post_ TOGETHER, so the viewport answers the
        // question the control is actually asking -- what will the shipped game look like -- while
        // it is being dragged. A LIVE EDIT OUTRANKS A FLAG, which is deliberately the opposite of
        // load time: applyProject lets --exposure win over the manifest because a flag is the
        // newest thing the human said, and here the drag is.
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
                // TICKING ADOPTS WHAT THE VIEWPORT IS ALREADY SHOWING, not a hardcoded number:
                // the reason anyone ticks this box is to record the look they just spent an
                // afternoon on in the Post Process panel.
                f32 v = stated ? *key : *live;
                if (ImGui::Checkbox("##stated", &stated)) {
                    // UNTICKING LEAVES THE VIEWPORT ALONE. "Unstated" means the compiled default
                    // applies on the next open, not that this session's eyes snap back mid-edit --
                    // which is also how applyProjectRenderSettings reads every other absent key.
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
                // TRACKED BEFORE THE TOOLTIP IS ASKED FOR, the same ordering rule the ReSTIR
                // history slider above states for IsItemHovered: both read the LAST item
                // submitted, and a tooltip submits items of its own.
                uiReg_.track(trackName);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tip);
                ImGui::PopID();
            };
            postFloat("exposure", "Exposure", &project_.postExposure, &post_.exposure,
                      0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic,
                      "project.post.exposure",
                      "Linear multiplier on scene radiance, applied before the tone\n"
                      "curve. Auto exposure below overrides it every frame while it\n"
                      "is on.\n\nRound-trips as RENDER.EXPOSURE.");
            postFloat("bloom", "Bloom", &project_.postBloom, &post_.bloomIntensity,
                      0.0f, 1.0f, "%.3f", 0, "project.post.bloom",
                      "ZERO IS A REAL ANSWER HERE, which is why the key's sentinel is\n"
                      "-1 and not 0: zero intensity builds no pyramid and records no\n"
                      "pass at all (RHI.hpp:151), so it is a cost choice as well as a\n"
                      "look. The sandbox's own --bloom default is 0 rather than\n"
                      "PostSettings' 0.06, so every recorded gate image was taken\n"
                      "with no bloom.\n\nRound-trips as RENDER.BLOOM.");
            {
                // A CHECKBOX RATHER THAN settingInt's 0..1 SLIDER, unlike WINDOW.RESIZABLE and the
                // IMPORT.* pair that use that helper: this is the same on/off the Post Process
                // panel already spells as a checkbox (SandboxPanels.cpp:1917), and hand-rolling it
                // is also what lets the registry track the control before its tooltip runs.
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
            // AMBER, NOT GREYING THE EXPOSURE SLIDER as the Post Process panel does: there the
            // slider is the live value the adaptation is about to overwrite, here it is a RECORDED
            // one that is perfectly meaningful to author in either order -- a project that ships
            // with adaptation off next month still wants its exposure in the file today.
            if (post_.autoExposure && project_.postExposure >= 0.0f)
                ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                    "Auto exposure is on, so the adaptation overwrites Exposure every frame. The "
                    "recorded value still applies wherever auto exposure is off.");
            {
                ImGui::PushID("tonemap");
                bool stated = project_.postTonemap >= 0;
                // CLAMPED FROM BOTH SOURCES, because neither one is bounded: the parser stores
                // whatever integer the file holds (OcProject.cpp's RENDER.TONEMAP) and setTonemap
                // takes --tonemap's atoi as it comes, so a 5 from either would leave this combo
                // indexing its item list off the end and showing a blank.
                int mode = stated ? project_.postTonemap : static_cast<int>(post_.tonemap);
                if (mode > 2) mode = 2;
                if (ImGui::Checkbox("##stated", &stated)) {
                    project_.postTonemap = stated ? mode : -1;
                    if (stated) post_.tonemap = static_cast<u32>(mode);
                    projectDirty_ = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unticked: this project does not state it,\n"
                                      "and the engine's own default (ACES luminance) applies.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!stated);
                if (ImGui::Combo("Tone curve", &mode,
                                 "Per-channel ACES\0ACES fitted\0ACES luminance (default)\0")) {
                    project_.postTonemap = mode;
                    post_.tonemap = static_cast<u32>(mode);
                    projectDirty_ = true;
                }
                ImGui::EndDisabled();
                uiReg_.track("project.post.tonemap");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(
                        "Which curve resolves HDR radiance to the display:\n"
                        "  Per-channel     the original Narkowicz/Hill approximation, and\n"
                        "                  the DEFAULT: the gentlest toe, so dim bounce\n"
                        "                  light stays visible instead of crushing to\n"
                        "                  black. Every recorded gate baseline in\n"
                        "                  scripts/ was measured through it\n"
                        "  ACES fitted     the same curve between the ACES matrices\n"
                        "  ACES luminance  tonemaps LUMINANCE and puts the original\n"
                        "                  chromaticity back, so hue survives any exposure\n\n"
                        "Measured on PTTest Sponza at exposure 8: fitted holds chroma 1.41\n"
                        "where luminance holds 3.09 at the same brightness, and above 5x\n"
                        "fitted lets BLUE overtake GREEN -- warm stone rendering cold\n"
                        "(RHI.hpp:196-216).\n\nRound-trips as RENDER.TONEMAP.");
                ImGui::PopID();
            }
            // WHERE THE PRECEDENCE IS ACTUALLY VISIBLE. A flag applies once at startup and the
            // manifest is applied later, when a project opens, so applyProject deliberately
            // declines to let these keys overwrite one -- which from this page looks like a
            // control that saved fine and then did nothing on the next launch. Said here instead.
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
            // THE RENDERER THIS PROJECT ASKS FOR. Unlike everything else on this page, changing
            // it does nothing until the editor is restarted -- the device is created before a
            // project is open, so the manifest is peeked for this one key before startup (see
            // main). Saying so on the control is the difference between a setting that looks
            // broken and one that is understood.
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
                // WHAT IS ACTUALLY RUNNING, beside what was asked for. A backend has to be
                // compiled in to be selectable at all, and the default CMake configuration has
                // AVER_RHI_VULKAN OFF -- so a project can name Vulkan on a build that cannot
                // give it one. The RHI already warns in the log when that happens; this puts the
                // same fact where the choice is made.
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

        // ---- WHICH DIFFUSE GI ESTIMATOR, and it lives on THIS page deliberately ----
        // It was on the Ray Tracing page, because ReSTIR needs RayQuery and that looked like
        // the dependency that mattered. It is not. Someone looking for the diffuse GI
        // algorithm looks under Global Illumination, finds Quality / Voxel grid / Diffuse
        // cones -- every one of which describes the CONE gather specifically -- and reasonably
        // concludes cones are the only option there is. Reported as "I cannot find that
        // project setting", which is the correct bug report for a control filed under its
        // IMPLEMENTATION instead of its JOB. It sits above the cone knobs because it decides
        // whether they apply at all.
        //
        // giMode round-trips through the .ocproject as RENDER.GIMODE, so a project remembers
        // the choice; absent from an older manifest it stays -1 and the engine default (cones)
        // applies, which is what every project written before this key existed meant.
        //
        // SHOWS er.giMode.EFFECTIVE, NOT THE RAW STORED REQUEST (F-d, Lane 1): giMode is no
        // longer clamped inside Settings itself, so a project asking for ReSTIR on a device or RT/
        // GI tier that cannot run it right now still remembers the ask (s.giMode is untouched
        // below) rather than being silently rewritten to 0 -- see RenderSettingsResolver.hpp's
        // resolve() for exactly which three things have to agree first. Showing the raw value here
        // while the renderer quietly ran cones instead would read as a control that does nothing;
        // showing the effective one means the combo reads Voxel cones until it actually can switch,
        // and switches back on its own the moment the reason clears, since the stored request was
        // never touched.
        int giAlgo = static_cast<int>(er.giMode.effective);
        if (ImGui::Combo("Indirect diffuse", &giAlgo, "Voxel cones\0ReSTIR (experimental)\0")) {
            s.giMode = static_cast<u32>(giAlgo); changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Voxel cones is what this engine has always shipped: a clipmap\n"
                              "marched with cones. It is the default and is unchanged. The\n"
                              "Voxel grid and Diffuse cones settings below apply ONLY to it.\n\n"
                              "ReSTIR resamples ray-traced indirect samples over time AND\n"
                              "across neighbouring pixels (NVIDIA RTXDI, third_party/rtxdi).\n"
                              "It has no voxel volume, so the boundary artefacts the clipmap\n"
                              "produces -- surfaces near the edge reading as unoccluded, worst\n"
                              "while the camera moves -- cannot occur.\n\n"
                              "EXPERIMENTAL: spatio-temporal resampling. Grainier than the\n"
                              "cone gather unless Denoiser below is on -- which is what the\n"
                              "NRD pass is for, and it needs MSAA 1 to run at all.");
        // ReSTIR is REQUESTED (the combo above just picked it, or a manifest already had it) but
        // not EFFECTIVE: named here, in this page's own red-reason idiom, rather than left for the
        // combo silently reading "Voxel cones" with no explanation of why the choice did not take.
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
            "No ray (pre-fix, over-bright)\0Reconstructed (no ray)\0Half resolution\0Full\0")) {
            s.giRestirVisibility = static_cast<u32>(vis); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.gi.restirVisibility");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Extra rays per shaded fragment beyond the candidate/sun/sky/\n"
                              "reflection rays this estimator already traces (cost UNMEASURED):\n"
                              "  Full           up to 2 (F2 is the expensive one)\n"
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
        } else if (vis == 0) {
            // AMBER, NOT RED: this is a legal, live choice (unlike the giMode reason line above,
            // which names a prerequisite the user cannot fix from this combo) -- it is simply the
            // one choice that reopens a fix this project already shipped. No magnitude is quoted:
            // R2/R3's pixel size is UNMEASURED (2.10 G).
            ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
                "No ray restores the pre-fix candidate-hit sky and reuse visibility (legacy bits "
                "4 and 8): shadowed and enclosed areas read over-bright again -- the washed-out "
                "look cb4b48df fixed.");
        } else {
            // The legacy console switches force NoRay for their OWN ray regardless of this combo
            // (2.8's one precedence rule) -- named here so a switch left on from a by-hand A/B
            // does not read as this combo silently doing nothing.
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

        // ---- INDIRECT LIGHT HISTORY: RTXDI's stparams.maxHistoryLength, RIGHT UNDER THE VISIBILITY
        // COMBO ABOVE because it is the OTHER dial over the same estimator's own artefact -- how much
        // weight a previous-frame reservoir may carry into the ReSTIR GI combine. This is what 674ed667
        // fixed: the camera-motion brightness fade was this value, not the visibility rays above it.
        //
        // GATED THE SAME WAY AS ReSTIR VISIBILITY RAYS, DELIBERATELY: er.giRestirMaxHistory mirrors
        // er.giRestirVisibility's own RequiresRestirGi reason chain (RenderSettingsResolver.hpp) rather
        // than computing a fresh one, because both controls are inert for the identical reason -- ReSTIR
        // GI itself is not running. SHOWS er.giRestirMaxHistory.REQUESTED, not the raw s.giRestirMaxHistory,
        // for giRestirVisibility's own reason directly above: effective always equals requested for this
        // field, so the two reads are the same value, but requested is the one the resolver actually
        // computed and is what stays correct if that ever stops being true.
        const bool histGreyed = greysControl(er.giRestirMaxHistory.reason);
        ImGui::BeginDisabled(histGreyed);
        int hist = static_cast<int>(er.giRestirMaxHistory.requested);
        if (ImGui::SliderInt("Indirect light history (frames)", &hist, 0, 8)) {
            s.giRestirMaxHistory = static_cast<u32>(hist); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.gi.restirHistory");
        // THE TOOLTIP BINDS TO THE SLIDER, so it is asked for BEFORE the greyed-reason label below:
        // IsItemHovered reads the LAST item submitted, and with the label submitted first a disabled
        // control's tooltip would hang off the label instead of the control it explains.
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

        // THE DENOISER SITS HERE, UNDER THE ESTIMATOR IT FILTERS, because it is only reachable from
        // this page's own choices: it denoises the ReSTIR radiance above and the sky occlusion the
        // ray-tracing page turns on, and it is the ONLY thing in the engine that asks for the
        // G-buffer. Before this checkbox existed the whole pass was reachable only by passing
        // --gbuffer on a command line, which meant the editor could never see it at all.
        //
        // GREYED FOR A HARD REASON (er.denoiser.reason, greysControl -- RT hardware/tier, NRD
        // support, or nothing to denoise); LEFT CLICKABLE FOR THE SOFT ONE (RequiresMsaaOne),
        // because the same page's own MSAA radios can undo that in one click and the amber line
        // below already says so -- see the prerequisite table's own "soft reasons only warn
        // inline" rule (RenderSettingsResolver.hpp). The displayed check state is the EFFECTIVE
        // value while hard-greyed (s.denoiser is kept, untouched, so a request survives a
        // temporary hardware loss) and the RAW request otherwise, so the soft MSAA case still
        // shows what was actually asked for.
        const bool denoiserHardGreyed = greysControl(er.denoiser.reason);
        ImGui::BeginDisabled(denoiserHardGreyed);
        bool den = s.denoiser && !denoiserHardGreyed;
        if (ImGui::Checkbox("Denoiser (NVIDIA NRD)", &den)) { s.denoiser = den; changed = true; }
        ImGui::EndDisabled();
        if (denoiserHardGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.denoiser.reason));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Filters the ReSTIR indirect diffuse above and the ray-traced sky\n"
                                    "occlusion, with NVIDIA NRD's REBLUR (third_party/nrd).\n\n"
                                    "COSTS THE G-BUFFER: velocity, view-space depth and packed\n"
                                    "normal/roughness -- three render targets NOTHING ELSE in\n"
                                    "this engine needs, about 54 MB at 1080p. That is why it is\n"
                                    "off by default rather than something enabled for you.\n\n"
                                    "REQUIRES MSAA 1 and D3D12. Above 1x the G-buffer is cleared\n"
                                    "but never written, so the pass refuses to run rather than\n"
                                    "filter blanks into a confidently wrong image.\n\n"
                                    "Round-trips as RENDER.DENOISER.");
        // s.msaa, NOT the device's live sample count: this is the value being edited on this very
        // page, so the warning appears the moment the two settings disagree rather than only after
        // Apply -- and it goes away as soon as Anti-aliasing is set to 1, before anything commits.
        // Kept exactly as the soft reason's own dedicated line: RequiresMsaaOne never greys, so it
        // needs this rather than the red reason tag above, which fires only for the hard reasons.
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
        // THE GRID IS ONLY EVER BUILT AT INIT (VoxiRenderer::createVoxelVolume) -- nothing resizes
        // it afterward, so a Quality change or a hand-picked value above is recorded immediately
        // but does not reach the GPU volume until the project reloads. voxelResolutionBuilt()
        // (Lane 3) is the edge that IS actually running; comparing against it, rather than against
        // whatever this session started with, is what lets this note clear itself the moment a
        // reload catches up.
        if (s.voxelResolution != voxiRenderer_.voxelResolutionBuilt())
            ImGui::TextWrapped("Takes effect when the project is reloaded; the volume built for "
                               "this session is still %u^3.", voxiRenderer_.voxelResolutionBuilt());
        if (ImGui::SliderFloat("GI intensity", &s.giIntensity, 0.0f, 4.0f)) changed = true;
        if (ImGui::SliderFloat("GI distance", &s.giMaxDistance, 10.0f, 20000.0f, "%.0f")) changed = true;
        ImGui::DragFloat3("Volume centre", &giCenter_.x, 0.5f);
        ImGui::DragFloat("Volume extent", &giExtent_, 0.5f, 1.0f, 100000.0f);
        // THE CONE COUNT, which this renderer's own comment calls the GI setting that actually
        // costs anything. Tier-derived with an open knob, like every other rung on this ladder:
        // Low 3, Medium 6, High 9, Epic 13, and picking a Quality above re-derives it.
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
        ImGui::Checkbox("Debug: show voxel radiance", &giDebugView_);

        // LIVE, CONSOLE-VAR-ONLY DEBUG PAINTS, beside the voxel-radiance one above: neither is a
        // Settings field (consoleGiPoisonViewSlot()'s own comment, EditorConsole.hpp) -- their live
        // values are raw bool slots reasserted onto the renderer every frame, the same slots `set
        // voxi.giPoisonView`/`set voxi.giVisPathView` already reach from the console. Reading and
        // writing the slots directly here means the checkbox and the console command are the same
        // switch, not two that can disagree.
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
                                  "  blue     NRD-readback guard\n"
                                  "  red      raw estimate hit voxi.giRadianceCeiling, still finite\n"
                                  "  green    NRD-denoised readback hit the same ceiling\n"
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

        // INNER GREY, BENEATH THE OUTER ONE ABOVE (st != Status::Ready, hardware-only): everything
        // from here to this page's own EndDisabled below is inert while the RT TIER itself is Off,
        // which the outer grey does not cover -- a device WITH ray-tracing hardware and Quality set
        // to Off has every row below still fully interactive today, sliding rays and filter radii
        // that do nothing. er.rtSubControls carries exactly that second reason (rtGate,
        // RenderSettingsResolver.hpp), so this second BeginDisabled greys the sub-rows without
        // touching the Quality combo above, which must stay choosable regardless -- it is the only
        // control on this page that can turn the tier back on.
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
        // Tile edge, not raw pixels-per-ray: VoxiRenderer::setPixelsPerRayTile only ever rounds
        // to a power of two, so offering anything else would just be relabelled after the fact.
        // Options are powers of two (1,2,4,8,16) so the SQUARE one ray covers is always one too (1..256).
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

        // A SECOND, INDEPENDENT DENOISER, and the separate heading is the point: these two fail
        // in opposite directions. The one above reuses THIS pixel across TIME -- converges
        // beautifully while still, collapses a penumbra the moment the camera moves. The one below
        // averages NEIGHBOURS within a frame with no history, so motion cannot poison it. They compose; neither substitutes for the other.
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

        // ---- A THIRD, SEPARATE DENOISER FROM THE TWO ABOVE (Settings::reblurMaxAccumulatedFrameNum,
        // Voxi.hpp) -- those two filter the ray-traced SUN SHADOW; this one tunes REBLUR_DIFFUSE,
        // which filters the ReSTIR indirect-diffuse GI estimate behind the "Denoiser (NVIDIA NRD)"
        // checkbox on the Global Illumination page. It sits here, with this page's other
        // "Denoiser: ..." rows, rather than there, and reaches NRD through the same s/changed/
        // vx.setSettings(s) idiom as every other dial on this page -- VoxiRenderer re-issues
        // nrd::SetDenoiserSettings every time setSettings() runs, so the new value takes effect
        // next frame with no rebuild (Voxi.hpp's own comment on the field).
        ImGui::Spacing();
        ImGui::TextUnformatted("Denoiser: NRD/REBLUR history depth (indirect diffuse GI)");
        ImGui::Separator();
        {
            int accum = static_cast<int>(s.reblurMaxAccumulatedFrameNum);
            if (ImGui::SliderInt("History depth (frames)", &accum, 0, 63)) {
                s.reblurMaxAccumulatedFrameNum = static_cast<u32>(accum);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("REBLUR_DIFFUSE's main history depth, in frames -- a latency/\n"
                                  "noise trade, not a dispatch toggle: higher converges quieter\n"
                                  "but lags longer behind a moving light or camera. Filters the\n"
                                  "indirect-diffuse GI estimate (the \"Denoiser (NVIDIA NRD)\"\n"
                                  "checkbox, Global Illumination page), not the sun shadow above.\n"
                                  "[0,63] is NRD's own REBLUR_MAX_HISTORY_FRAME_NUM.\n"
                                  "NOT captured to the project manifest -- like giSkyOcclusionRays/\n"
                                  "Tile, it has no manifest key (ProjectRenderApply.hpp's own\n"
                                  "capture rule), so it resets to the compiled default, 30, on the\n"
                                  "next project reload rather than round-tripping through .ocproject.");
        }

        // ---- HISTORY RESETS (debug): one button per reset*history console command
        // (EditorConsole.hpp) -- see each command's own help string there for the bisection order
        // (GI first, then RT, then NRD) and why AO has no button of its own: requestAoHistoryReset()
        // shares RT's own validity flag today (no independent one exists), so its button would be a
        // second way to do exactly what Reset RT history already does. Reset All mirrors
        // resetallhistory exactly -- GI + RT + NRD, not a redundant fourth AO call.
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
        if (ImGui::Button("Reset NRD history")) vx.requestNrdHistoryReset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forces NVIDIA NRD/REBLUR to throw away its own internal temporal\n"
                              "history on the next frame. Try this if neither GI nor RT history\n"
                              "reset cleared the artifact.");
        ImGui::SameLine();
        if (ImGui::Button("Reset All")) {
            vx.requestGiHistoryReset();
            vx.requestRtHistoryReset();
            vx.requestNrdHistoryReset();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs all three resets above at once. Not the first move during a\n"
                              "bisection -- it clears everything and says nothing about which\n"
                              "buffer was actually poisoned; try one at a time first.");

        // ---- ray-driven rendering (experimental) ----
        // WHICH THING FINDS THE FIRST SURFACE: everything downstream already runs in one
        // pixel-shader invocation, so this swaps the one stage still fixed-function.
        // THE MEASURED BASELINE IS IN THE TOOLTIP ON PURPOSE: this trades hardware early-Z (which
        // a ray has no equivalent of) for whatever a primary ray costs, and an author deciding
        // that deserves the number, not a shrug.
        ImGui::Spacing();
        // NO LONGER "(experimental)" -- it is the shipped default from Medium up (D3, this
        // retune). Low is the one deliberate exception, kept on the rasteriser by explicit product
        // decision rather than a hardware gap; see ladder::rtRenderMode (QualityLadder.hpp) for
        // the full reasoning and its own honest accounting of what the evidence for Low does and
        // does not show.
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

        // ---- MILESTONE 1: STAGED RAY-DRIVEN PASSES, AN A/B SWITCH OVER THE COMBO ABOVE'S OWN
        // INTERNAL SHAPE, RIGHT UNDER IT for the same "read together" reason Indirect light history
        // sits directly under ReSTIR visibility rays below. GATED THE SAME WAY, DELIBERATELY:
        // er.rayDrivenStages mirrors er.rtRenderMode's own reason chain (RenderSettingsResolver.hpp)
        // rather than computing a fresh one, because both controls are inert for the identical
        // reason -- primary rays are not the active render mode. SHOWS er.rayDrivenStages.REQUESTED,
        // not the raw s.rayDrivenStages, for giRestirMaxHistory's own reason: effective always equals
        // requested for this field, but requested is what the resolver actually computed.
        //
        // MILESTONE 4 adds the combo's third entry, value 2. Unlike Staged it deliberately trades
        // GI quality for speed rather than staying a same-image comparison against Single pass --
        // see Voxi.hpp's own comment on Settings::rayDrivenStages for the full reasoning. The combo
        // index equals the settings value directly (0/1/2), so no separate index<->value mapping is
        // needed here beyond the string having three entries.
        const bool stagesGreyed = greysControl(er.rayDrivenStages.reason);
        ImGui::BeginDisabled(stagesGreyed);
        int stages = static_cast<int>(er.rayDrivenStages.requested);
        if (ImGui::Combo("Ray-driven passes", &stages,
                          "Single pass (default)\0Staged (experimental)\0"
                          "Staged + half-rate GI (experimental)\0")) {
            s.rayDrivenStages = static_cast<u32>(stages); changed = true;
        }
        ImGui::EndDisabled();
        uiReg_.track("project.rt.rayDrivenStages");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("For comparing the split against the single pass above, nothing more:\n"
                              "Staged runs the same primary ray, sun-shadow ray and shading in three\n"
                              "GPU passes (a visibility pass, a shadow pass, then the existing shade)\n"
                              "instead of one. Falls back to Single pass, logged once, wherever the\n"
                              "staged path is unsupported (non-D3D12, or a pipeline/resource it needs\n"
                              "is missing).\n\n"
                              "Staged + half-rate GI is the same split, but the ReSTIR GI stage traces\n"
                              "only half the GI rays each frame -- NRD's own checkerboard pattern -- and\n"
                              "REBLUR reconstructs the other half from the traced half and history.\n"
                              "Unlike Staged this is NOT a same-image comparison: it deliberately trades\n"
                              "GI quality and latency for speed, and can smear or shimmer under motion.\n"
                              "Needs ReSTIR GI and the NRD denoiser on; without them it behaves as Staged.\n"
                              "Compare it against Staged, not against Single pass, to see what the trade\n"
                              "actually costs.\n\n"
                              "Round-trips as RENDER.RDSTAGES.");
        if (stagesGreyed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[%s]", disableReasonText(er.rayDrivenStages.reason));
        }

        // BOUNCES ARE NOT ON THIS PAGE ANY MORE: they are a PATH TRACING quantity living with
        // that setting; leaving the slider here, disabled on ray-tracing mode, made the two look
        // like one feature. See Settings::ptBounces.
        ImGui::EndDisabled();   // the inner grey opened beside the Quality combo above
        ImGui::EndDisabled();
    }

    if (page == 4) {
        // THIS COMBO IS NOW THE REAL CONTROL: voxi::Settings::pathTracing, round-tripped through
        // the manifest and exposed to C# scripting. It used to have NO relationship to
        // modules/render.pt -- Voxi.cpp hard-coded NotImplemented regardless of hardware, so this
        // clamped to Off on every device, forever. status() now mirrors PathTracer::init()'s
        // DXR-1.1/SM-6.5/DXC/compute gate field for field, unlocking exactly when the reference
        // view can run.
        // THE TIER DRIVES THE ACCUMULATOR RESOLUTION: it used to drive nothing, leaving the view
        // permanently at 480x270 magnified ~5.7x -- that blow-up, not the sample count, is what
        // reads as a shimmering mess while the camera moves, since motion restarts accumulation
        // every frame.
        const Status st = vx.status(Feature::PathTracing);
        ImGui::TextUnformatted(Renderer::featureName(Feature::PathTracing));
        featureStatusBadge(vx, Feature::PathTracing);
        // ptSceneViewUnavailable_ is a RUNTIME signal PathTracer::init() itself raised (a DXC
        // compile failure, say) that the static device caps above did not predict. Disabling on it
        // too keeps this combo from claiming a quality that isn't running; it is NOT reset to Off
        // automatically, so a stale non-Off selection can sit here disabled until the user picks Off or reopens the project.
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

        // BOUNCES, WHICH THE MANIFEST HAS ALWAYS CARRIED AND NOTHING EVER SHOWED.
        // OcProject's RENDER.PTBOUNCES round-trips through save and load, applyProjectRender
        // pushes it into Settings, and --pt-bounces overrides it -- so the value was
        // authorable by hand-editing a .ocproject or by a command line, and by no other means.
        //
        // TIER-DERIVED WITH AN OPEN KNOB, which is this renderer's established shape (see
        // Settings::ptBounces and ptBouncesForQuality): picking a Quality above re-derives
        // this, so a hand-set value survives until the tier next changes. Said out loud in the
        // tooltip rather than left for somebody to discover by losing a setting.
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

        // A1: the priority between these three used to be two `if`s (unavailable, else active)
        // with no way to say "suppressed" at all -- the Quality combo would just silently do
        // nothing while ray-driven painted the scene. choosePtViewTag() is the pure decision
        // (see PtRenderConflict.hpp for the full priority reasoning); every ImGui call stays here.
        switch (aver::editor::choosePtViewTag(ptSceneViewUnavailable_,
                                               ptSceneViewSuppressedByRayDriven_,
                                               ptSceneView_ != nullptr)) {
        case aver::editor::PtViewTag::Unavailable:
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[unavailable on this device]");
            break;
        case aver::editor::PtViewTag::SuppressedByRayDriven:
            // SAME COLOUR AS [unavailable on this device] ABOVE, deliberately: both are "this
            // combo is not doing anything right now", and the existing red is this page's own
            // idiom for that -- inventing a second colour here would say the two situations
            // matter differently when, from this control's point of view, they don't.
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
        // The requested settings reach the manifest BEFORE setSettings can clamp them -- "live"
        // inside captureRenderSettingsFromUi's own capture rule means "before this edit", which
        // only holds while this call runs first. overallFollowMask carries whatever an Overall
        // Quality button or a per-group row above wrote this same edit (0 otherwise), so the
        // groups it just moved are captured as "follow the tier" rather than nine explicit pins.
        captureRenderSettingsFromUi(s, overallFollowMask);
        vx.setSettings(s);
    }
}

#endif
#endif

// OUTSIDE `#if AVER_WITH_IMGUI`, matching the declaration, and it has to be BOTH or neither:
// moving only the declaration left `no-ui` linking against a definition that was never compiled,
// an LNK2019 raised from maybeAutosavePrefs -- which calls this every quarter second in every
// configuration. The body makes no ImGui call at all; it only pushes members through
// setPrefBool/setPrefFloat/setPrefInt.
// Writes every editor preference back and flushes. Each setter compares before it stores.
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
    setPrefBool ("viewport.showColliders",       showColliders_);
    setPrefBool ("viewport.wireframe",           wireframe_);
    setPrefBool ("panels.worldOutliner",         showOutliner_);
    setPrefBool ("panels.details",               showDetails_);
    setPrefFloat("viewport.flySpeed",            flySpeed_);
    setPrefFloat("viewport.lookSensitivity",     lookSpeed_);
    setPrefFloat("ddc.ramBudgetMb",              ddcRamBudgetMb_);
    // Guarded exactly as the load is: a --frames run that wrote its --unlit back would leave
    // the next interactive session unlit, and the run after that with moved gates.
    if (maxFrames_ == 0) {
        setPrefBool("viewport.unlit",             unlit_);
        setPrefBool("viewport.showStaticMeshes",  showStaticMeshes_);
        setPrefBool("viewport.showAtmosphere",    showAtmosphere_);
    }
    setPrefBool ("snap.move",                    snapMove_);
    setPrefBool ("snap.rotate",                  snapRot_);
    setPrefBool ("snap.scale",                   snapScale_);
    setPrefFloat("snap.moveStep",                moveSnap_);
    setPrefFloat("snap.rotateStep",              rotSnap_);
    setPrefFloat("snap.scaleStep",               scaleSnap_);

    // Guarded exactly as the load is, and for the same reason: a --frames run that wrote its
    // CLI exposure back would leave the next interactive session looking at the capture's eyes.
    //
    // THE THREE WITH A MANIFEST KEY ARE GUARDED A SECOND TIME, symmetrically with the load: while
    // the project states RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE, the preference channel for that field
    // is inert in BOTH directions. Writing it anyway would quietly adopt the project's authored
    // look as this user's personal default for every project afterwards, and it could not even buy
    // a persisted Post-panel tweak in exchange -- the load above would decline to restore it, and
    // applyProject would overwrite it from the manifest on the next open regardless.
    if (maxFrames_ == 0) {
        if (project_.postExposure < 0.0f)     setPrefFloat("post.exposure",     post_.exposure);
        if (project_.postAutoExposure < 0)    setPrefBool ("post.autoExposure", post_.autoExposure);
        setPrefFloat("post.exposureKey",    post_.exposureKey);
        setPrefFloat("post.exposureSpeed",  post_.exposureSpeed);
        setPrefFloat("post.exposureMin",    post_.exposureMin);
        setPrefFloat("post.exposureMax",    post_.exposureMax);
        if (project_.postBloom < 0.0f)      setPrefFloat("post.bloomIntensity", post_.bloomIntensity);
        setPrefFloat("post.bloomThreshold", post_.bloomThreshold);
        setPrefFloat("post.bloomKnee",      post_.bloomKnee);
        setPrefFloat("post.histogramLow",   post_.histogramLowPercent);
        setPrefFloat("post.histogramHigh",  post_.histogramHighPercent);
    }

    const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
    if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size()))
        setPrefString("contentBrowser.ide", ides[static_cast<usize>(cbIdeChoice_)].name);
    else
        setPrefString("contentBrowser.ide", "");   // Automatic

    // GUARDED ON maxFrames_ == 0 LIKE EVERY OTHER CAPTURE-SENSITIVE PREF, and these two were the
    // ones that were not. The viewport flags above and the post-processing block above them each
    // carry this guard with a comment explaining it -- "a --frames run that wrote its CLI
    // exposure back would leave the next interactive session looking at the capture's eyes" --
    // and display.vsync/display.renderScale sat just below, unguarded.
    //
    // WHY THAT IS WORSE THAN IT SOUNDS: maybeAutosavePrefs() DOES return early on maxFrames_,
    // but onShutdown() calls saveEditorPreferences() unconditionally, and onShutdown() runs at
    // the end of every capture, benchmark and gate invocation. So every `--frames N
    // --render-scale F` or `--no-vsync` run wrote its measurement settings into the SAME
    // %LOCALAPPDATA%/AverEngine/editor.ini an interactive session reads back -- and the load at
    // the top of this file then faithfully restored the capture's settings as if the user had
    // chosen them. Measured against this session's own history: --no-vsync is passed by every
    // capture harness in scripts/, so the user's vsync preference has been decided by whichever
    // measurement ran last.
    //
    // The guard belongs here rather than in onShutdown() so that a future third caller cannot
    // reintroduce it, and so the rule reads identically to its two neighbours.
    if (maxFrames_ == 0) {
        if (prefsDevice_ && prefsDevice_->vsyncCanDisable())
            setPrefBool("display.vsync", prefsDevice_->vsync());
        // optimisation-wave-2, 3.3 A: THE THREE AverSR KEYS BELOW ARE SKIPPED OUTRIGHT, not
        // merely written a neutral value, when the command line drove this session's render scale
        // or AverSR level. TODAY'S OWN GUARD ENDS AT maxFrames_ == 0, so an interactive `--aversr
        // quality` run (maxFrames_ IS 0 for an interactive session -- this is not a --frames
        // capture) used to write itself into editor.ini with no CLI check at all, and the NEXT
        // ordinary launch inherited a choice nobody made from the Display page. Skipping the write
        // means a session driven by --render-scale/--aversr never touches any of the three keys,
        // so whatever a PRIOR interactive session actually chose there survives untouched.
        // AND SKIPPED ENTIRELY WITH THE MODULE OFF, the mirror of loadEditorPreferences' own
        // `#if AVER_MODULE_SR`: all three keys are AverSR's, two of them are written FROM
        // AverSR-only state (averSrChoice_, averSrQuality_), and a build that cannot read them
        // back has no business overwriting what a build that can wrote there.
#if AVER_MODULE_SR
        if (!averSrFromCli_ && renderScaleOverride_ == 1.0f) {
            setPrefString("display.aversrChoice", editor::averSrChoiceName(averSrChoice_));
            if (prefsDevice_)
                // THROUGH renderScaleToPersist, not the live device scale directly -- persisting
                // the live scale for anything but Manual is the exact bug this wave's own
                // 10.1/3.3-A corrections describe: Auto would persist whatever fraction the rung
                // it happened to land on THIS session resolved to, and the crash-cookie
                // render-scale block at load would apply that stale fraction before Auto ever got
                // a chance to re-derive it.
                setPrefFloat("display.renderScale",
                            editor::renderScaleToPersist(averSrChoice_, prefsDevice_->renderScale()));
            // THE UPSCALER'S QUALITY MIRROR -- WHICH, BEFORE display.aversrChoice EXISTED, WAS
            // NEVER PERSISTED AT ALL (the Display page's AverSR combo wrote averSrQuality_ and
            // applied it to the device; nothing wrote it to a pref and nothing read one back, so
            // it reset to Off on every launch, and desynced from display.renderScale above once it
            // was). display.aversrChoice is now the source of truth this mirror only backs up --
            // kept for at-a-glance reading of a raw editor.ini, and so a build that predates
            // display.aversrChoice reading the same file back still sees a level, not Off.
            // setPrefFloat rather than an int helper because there is no int helper -- the prefs
            // layer is float/bool/string, and every other numeric setting here goes through the
            // float pair. The enum is four values; a float carries them exactly.
            setPrefFloat("display.aversr", static_cast<f32>(static_cast<int>(averSrQuality_)));
        }
#endif  // AVER_MODULE_SR
    }

    // THE ONE LINE IN THIS FUNCTION THAT NEEDS THE EDITOR UI. keybinds_ is the chord registry the
    // ImGui layer owns and is compiled out with it; everything else here is a plain setPref* call,
    // which is why the function as a whole sits outside the guard.
#if AVER_WITH_IMGUI
    keybinds_.saveToPrefs();
#endif

    flushEditorPrefs();
}

} // namespace aver
