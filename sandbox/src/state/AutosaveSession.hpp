// Level/project/prefs autosave timers, crash-recovery state and the postpone/retry handshake.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct AutosaveSession {
    bool projectDirty_ = false;
    // Auto-saved on edit with 0.5s debounce.
    static constexpr f32 kProjectAutosaveSec = 0.5f;
    f32 projectAutosaveAccum_ = 0.0f;
    bool projectRenderPending_ = false;   // manifest read before the device attached
    // ---- FOUR GROUPS THAT SAT INSIDE `#if AVER_MODULE_SCENE` AND ARE READ FROM OUTSIDE IT ----
    // Plain arithmetic/flags, no scene:: type, each read unconditionally.
    //
    // Preference autosave timer: maybeAutosavePrefs (SandboxAutosave.cpp) runs unguarded. 0.25s debounce.
    // Prefs are cheap to check/write, so the interval is short; dragged slider collapses 60 frames to 4.
    static constexpr f32 kPrefsAutosaveSec = 0.25f;
    f32 prefsAutosaveAccum_ = 0.0f;
    // Deferred autosave answers: drawNotifications writes them, maybeAutosave consumes next tick.
    bool autosavePostponeRequested_ = false;
    bool autosaveRetryRequested_ = false;
#if AVER_MODULE_SCENE
    // The Outliner row currently being renamed in place, and its edit buffer.
    // Create-a-landscape controls; the shape knobs are landscape_.noiseParams(), shared with the
    // ring generator so a created section and the tiles around it come from one set of numbers.
    // Autosave interval, TEN MINUTES. The old 30s period made the countdown visible; silence
    // would be expected, but now saveLevel doesn't call markLevelSaved, so the timer restarts immediately
    // and a 10s warning on 30s put a countdown visible a third of every minute.
    static constexpr f32 kAutosaveDefaultSec = 600.0f;
    f32 autosaveIntervalSec_ = kAutosaveDefaultSec;
    f32 autosaveAccum_ = 0.0f;
    // Why three states, not a bool: the save is synchronous (onUpdate runs BEFORE onRender), so a "Saving..."
    // notification raised and saved in the same tick would be replaced by "Saved" before buildUI draws it.
    // Pending spends one presented frame saying so, then writes on the tick after.
    enum class AutosaveState : u8 { Idle, Counting, Pending };
    AutosaveState autosaveState_ = AutosaveState::Idle;
    u64 autosaveNotify_ = 0;      // the countdown's notification, updated in place
    int autosaveShownSec_ = -1;   // last whole second rendered, so updates are 1/s not 1/frame
    u8  autosavePostpones_ = 0;
    // Ten seconds is long enough to finish a drag and press Postpone, short enough that the warning
    // is about the save that is coming rather than a background fact.
    static constexpr f32 kAutosaveWarnSec = 10.0f;
    // A Postpone that works forever is a way to switch the safety net off without ever deciding to.
    static constexpr u8  kAutosaveMaxPostpones = 3;
    bool autosaveWritten_ = false;
    u64 autosavedEditMark_ = ~0ull;   // the edit the sidecar holds: the countdown waits for a newer one
    bool autosaveFailedWarned_ = false;
    std::string recoveryPath_;      // a sidecar newer than its level, waiting to be offered
#endif
};
