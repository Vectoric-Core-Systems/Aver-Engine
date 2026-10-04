#include "InputOwnership.hpp"

namespace aver::editor {

InputOwnership resolveInputOwnership(const InputConditions& c) {
    InputOwnership o;

    // ---- THE GAME'S HALF, lifted verbatim from pushInput's own gate --------------------------------
    //
    // `suppressed` there is (!uiActive || (releasedByUser && playing)). Note what it is NOT: it is not
    // "the game is not playing". A no-play frame still publishes -- every key as UP -- and that is
    // load-bearing rather than incidental. Publishing nothing is what left keys latched down forever,
    // because aver_fw_input_new_frame() rolls cur into prev without clearing cur. The gate decides the
    // VALUE published, never whether to publish at all.
    const bool suppressed = !c.uiActive || (c.releasedByUser && c.playing);
    // THE KEYBOARD TAKES THE SAME CAPTURE OVERRIDE THE MOUSE DOES, and for the identical reason
    // spelled out below it: a captured cursor is hidden and confined to this window, so there is no
    // ImGui widget the user could be interacting with, and WantCaptureKeyboard is then a stale
    // answer to a question that no longer applies.
    //
    // WITHOUT IT, A CAPTURED PLAY SESSION GOT NO KEYS AT ALL while the mouse worked perfectly --
    // which is precisely as strange as it sounds, and is why --pie-camera-test has always reported
    // "THE VIEW IGNORES W". Measured on SkyForge, frame 60 of that test, mid-session:
    //     kbToGame=false mouseToGame=true captured=true uiWantsKb=true textInput=false
    //     playState=PLAYING fwKeyW=0 rawVkW=0 inputHeldW=true
    // W held in InputState, the pawn possessed, the mouse captured -- and W published as 0, because
    // the viewport is itself an ImGui window and ImGui therefore wants the keyboard. The mouse
    // escaped that only because it already had this clause.
    //
    // A TEXT FIELD IS STILL SAFE, and is not what this weakens: WantTextInput is handled on its own
    // below and forces both keyboard consumers off regardless of capture. That is ImGui's actual
    // answer to "is the user typing", and it is the one that should win.
    o.keyboardToGame = !suppressed && (c.mouseCaptured || !c.uiWantsKeyboard);
    // The mouse asks for both flags, and captures override both. Captured means the OS cursor is
    // hidden and confined to this window, so there is no ImGui widget it could be interacting with --
    // WantCaptureMouse is then a stale answer to a question that no longer applies.
    o.mouseToGame = !suppressed && (c.mouseCaptured || (!c.uiWantsMouse && !c.uiWantsKeyboard));

    // ---- THE TOOLS' HALF, lifted from the fly-camera block's gate ----------------------------------
    //
    // (!gameHasInput() || defaultPawnPlay), where gameHasInput() is playing && !releasedByUser.
    //
    // THE defaultPawnPlay EXCEPTION IS NOT A BUG. When a project declares no GameMode, Play possesses
    // the engine's own spectator pawn, and the editor fly controls are what drive it -- there is no
    // gameplay code to take over. Without this clause the whole block stood down the instant Play
    // began and the spectator camera was frozen. See the fly block's own comment in SandboxApp.cpp.
    const bool gameHasInput = c.playing && !c.releasedByUser;
    const bool toolsLive = c.uiActive && !c.browserActive && (!gameHasInput || c.defaultPawnPlay);
    o.keyboardToTool = toolsLive && !c.uiWantsKeyboard;
    // Tools additionally want the pointer over the 3D view: a click on a panel is that panel's, and
    // the viewport tools all recompute this same `overScene` idea by hand today.
    o.mouseToTool = toolsLive && !c.uiWantsMouse && c.pointerInViewport;

    // A TEXT FIELD BEATS EVERYTHING, and it is stated once here rather than trusted to the two
    // WantCapture flags. They usually imply it, but not always: WantTextInput is ImGui's own answer to
    // "is the user typing", and typing WASD into a rename box must not also fly the camera or walk the
    // character. This is the single clause in this function that is not a transcription of an existing
    // call site -- it is the one place they were all quietly relying on ImGui getting it right.
    if (c.textInput) {
        o.keyboardToGame = false;
        o.keyboardToTool = false;
    }

    // ---- THE LIVE SCOPE MASK ------------------------------------------------------------------------
    // Mirrors EditorKeybinds' own Scope comment: the editor is only ever in Object mode or Landscape
    // mode, never both, so those two are exclusive by construction rather than by policy.
    o.activeScopeMask = c.landscapeMode ? kScopeLandscapeMode : kScopeObjectMode;
    if (c.playing)    o.activeScopeMask |= kScopePlaySession;
    if (c.drawerOpen) o.activeScopeMask |= kScopeDrawerOpen;
    // Always live: the global-UI commands are the ones that must work from anywhere, which is exactly
    // what makes them the scope with no condition attached.
    o.activeScopeMask |= kScopeGlobalUI;

    return o;
}

} // namespace aver::editor
