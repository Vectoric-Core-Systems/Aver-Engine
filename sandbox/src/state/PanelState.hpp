// Editor panel/dialog visibility flags, prompt requests and dock layout flags.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct PanelState {
    bool showPlaceActors_ = false, showMessageLog_ = false, showUndoHistory_ = false;
    char placeQuery_[64] = {};
    int  msgLogLevel_ = 0;              // the Output Log's filter indices: All, Info+, Warn+, Error+, Critical+
    std::string msgLogCategory_;
    char msgLogQuery_[96] = {};
    bool dockBuilt_=false;   // false = build the default layout on the next frame
    // Set by View > Reset Layout so the builder overrides a layout restored from the ini.
    bool dockResetRequested_=false;
    // Warn ONCE about a missing icon font (not once per DPI change).
    bool iconWarned_=false;
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showWorldSettings_=false;   // Window > World Settings (per-LEVEL settings)
    // Window > World Outliner / Details. Default ON: these are the editor's two primary panels.
    bool showOutliner_=true;
    bool showDetails_=true;
    bool showAbout_=false;             // Help > About
    bool wantSaveLevelAs_=false;
    char saveLevelAsName_[128]={};
    std::string saveLevelAsError_;
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    bool scrollPrefsToKeybinds_=false;   // --scroll-prefs-to-keybinds, one-shot
    int  settingsPage_=1;            // 0 Description, 1 Rendering>General, 2 >GI, 3 >Ray Tracing, 4 >Path Tracing
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    // ONE PENDING OPEN, whatever asked for it: File > Open Level's picker, a Content Browser double-click, or a path forwarded from a second launch.
    // Funnelled through one request so the unsaved-changes guard and the class-placement spawn happen once.
    std::string pendingOpenPath_;
    std::string pendingOpenWhy_;      // how it was asked for, for the modal's own sentence
    // The modal shown when a pending open would discard unsaved work. Separate from exitPrompt_.
    bool        pendingOpenPrompt_ = false;
    bool        pendingNewLevel_ = false;   // File > New Level, waiting on the unsaved-changes prompt
    // File > Launch in Aver Engine Runtime, waiting on drawLaunchRuntimePrompt's unsaved-changes ask.
    bool        launchRuntimePrompt_ = false;
    bool showUiDemo_ = false;
    bool openLegacy_ = false;   // --open-legacy
    bool        upgradeAsked_ = false;
    bool        exitPrompt_ = false;      // the unsaved-changes modal is up
    std::string exitPromptError_;         // why a "Save all" attempt failed
    f32  uiDemoHealth_ = 0.72f, uiDemoStamina_ = 0.44f, uiDemoScroll_ = 0.0f, uiDemoClock_ = 0.0f;
#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
    bool showRenderQueue_ = false;
#endif
#endif
    bool                showDataLayers_ = false;
};
