// Play-in-editor session: play mode, ejected/step flags, the pre-play snapshots and PIE travel state.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct PlayState {
#if AVER_MODULE_SCENE
    // Editor view saved before Play started (restored on stop).
    bool preplayViewValid_ = false;
    editor::LevelView preplayView_{};
    std::string pieSnapshotPath_;
    std::vector<scene::Entity> piePreEntities_;   // every live entity at Play start, sorted
    bool pieReloadPending_ = false;
    bool pieWasUnsaved_ = false;
    bool pieTravelled_ = false;
    bool pieTravelling_ = false;
    std::string pieEditedLevelPath_;
    // Snapshot of transforms/visibility/spawned entities at Play start; restored on Stop.
    bool defaultPawnPlay_ = false;  // True while engine's default GameMode is active.
    struct PlaySavedTransform { scene::Entity e; Transform xf; bool visible = true; };
    std::vector<PlaySavedTransform> playWorldSnapshot_;
    bool playWorldCaptured_ = false;
    // What moved during session (Voxi GI excludes it). Started in capturePlayWorld, ended in stopPlay.
    game::PlayMobility playMobility_;
#if AVER_MODULE_FRAMEWORK
    struct PlayClassSaved { usize slot; EditId id; Transform xf; };
    std::vector<PlayClassSaved> playClassSnapshot_;        // class roots at Play start, for the Stop respawn
#endif
#endif
    // Play options (persisted in editor.ini). Declared unguarded for toolbar compatibility.
    enum class PlayMode : u8 {
        SelectedViewport = 0,   // play in the Level viewport, possessing the player (Alt+P)
        Simulate         = 1,   // the game runs, the player is not possessed, the editor keeps the camera (Alt+S)
        Standalone       = 2,   // a separate AverEngineRuntime.exe on the saved level (launchInRuntime)
        NewWindow        = 3,   // the viewport session, shown in a window of its own; Esc returns
    };
    enum class PlaySpawnAt : u8 { PlayerStart = 0, CameraLocation = 1 };
    PlayMode    playMode_ = PlayMode::SelectedViewport;    // Last launched; main button repeats it.
    PlaySpawnAt playSpawnAt_ = PlaySpawnAt::PlayerStart;
    // Default pawn walks (capsule: gravity, stairs, jumps, runs) without GameMode. Off = flying drone.
    bool        defaultPawnWalk_ = false;
    int32_t     walkCapsule_ = 0;
    int32_t     flyCapsule_ = 0;   // the flying default pawn's zero-gravity collider (0 = no-clip fallback)
    bool        playGameGetsMouse_ = true;   // Capture mouse when viewport session starts.
    std::string playStandaloneArgs_;         // Extra command line for Standalone Game.
    // Ejected (F8): editor has camera/input/tools; session keeps running. Possess (F8) to snap back.
    bool        playEjected_ = false;
    bool        playFrameStepPending_ = false;
    bool        scrollPrefsToPlay_ = false;  // "Advanced Settings..." scrolls Preferences to Play, once.
    std::optional<Vec3> playFromHere_;
    rhi::IDevice* playWindowDevice_ = nullptr;
    u32 playWindowW_ = 0, playWindowH_ = 0;
};
