// WHO OWNS THE KEYBOARD AND MOUSE THIS FRAME.
//
// THE PROBLEM THIS REPLACES. Roughly eleven separate consumers in SandboxApp.cpp each answered that
// question for themselves, by hand, from ImGui flags: the fly camera, the gizmo, landscape sculpt,
// foliage paint, the console, the content browser's shortcuts, the keybind registry, the mouse-capture
// logic, pushInput, the MCP synthetic-input bridge, and the test harnesses. Each grew its own spelling
// of "is the UI using this" -- some checking WantCaptureKeyboard, some WantCaptureMouse, some both,
// some also levelHovered_/inViewport(), some also playSessionActive() -- and nothing kept them in
// agreement. There was no arbitration concept at all, only a convention that everybody re-derived.
//
// That is not a hypothetical complaint. This session has already paid for it twice: a play session
// that stopped publishing input left every key latched down forever (pushInput's own gate), and a
// first-person camera fought the editor fly camera because the two gates disagreed about who was
// driving. Both were one consumer's private answer to a question nobody owned.
//
// A PURE FUNCTION, DELIBERATELY. No ImGui types, no SandboxApp state, no globals -- plain bools in,
// plain bools out. That is what makes the arbitration table a genuine headless unit test
// (tests/editor/src/InputOwnershipTest.cpp) with no window, no device and no ImGui context, in a
// codebase where almost nothing about the editor can be tested at all. Every ImGui query stays at the
// call site, where it belongs; only the DECISION moves here.
//
// IT DOES NOT DECIDE VALUES, ONLY PERMISSION. What the keys actually are is InputState's job (see
// SandboxApp's input_ member). This answers "may this consumer look?", nothing more.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::editor {

// Which editor context(s) a command's chord is live in. Two commands sharing a chord is only a REAL
// conflict -- the kind that leaves "which command does this key mean" genuinely ambiguous -- when
// their scopes overlap. Tool.Select (1) and Sculpt.Raise (1) never can, because the editor is only
// ever in Object mode or Landscape mode, never both; Play.Stop (Escape) and Drawer.Dismiss (Escape)
// never can, because a play session and an open drawer are different UI states. See conflictWith().
//
// IT LIVES HERE, NOT IN EditorKeybinds.hpp WHERE IT WAS WRITTEN, for one concrete reason: that header
// is whole-file `#if AVER_WITH_IMGUI` (a Chord names an ImGuiKey, so it has nothing to say in a build
// with no ImGui), and a scope is not an ImGui idea. Leaving it there would have made this header --
// whose entire purpose is to be testable with no ImGui context -- depend on ImGui to name a bit.
// EditorKeybinds.hpp includes this one for the enum, so there is still exactly one definition.
enum Scope : u32 {
    kScopeObjectMode    = 1u << 0,
    kScopeLandscapeMode = 1u << 1,
    kScopePlaySession   = 1u << 2,
    kScopeDrawerOpen    = 1u << 3,
    kScopeGlobalUI      = 1u << 4,
    // THE ASSET-EDITOR TABS, which each had their own hardcoded keys and no way to rebind them. They
    // are separate scopes rather than one because a chord may legitimately mean different things in
    // a node canvas and in a skeletal preview -- the graph editor's bare `C` is "comment box around
    // the selection", which would be a bizarre binding to force on the actor editor.
    //
    // Their WHOLE POINT is that they do not overlap kScopeObjectMode: a tab is either the level
    // viewport or one of these, never both, so Ctrl+C can mean copy-entity in one and copy-node in
    // the other without conflictWith() calling that ambiguous.
    kScopeGraphEditor   = 1u << 5,
    kScopeActorEditor   = 1u << 6,
    kScopeAssetEditor   = 1u << 7,   // any asset tab: the shared Ctrl+S lives here
};

// Everything the decision depends on, gathered once per frame at the call site.
//
// One flat struct rather than a dozen positional arguments because the arguments are all bools: a
// caller that transposes two of them in a long parameter list gets no compiler error and a subtly
// wrong editor. Named initialisation makes that mistake visible on the page.
struct InputConditions {
    bool uiActive          = false;  // an ImGui frame is open (false in a headless/no-UI build)
    bool browserActive     = false;  // the project-picker start screen owns the window
    bool textInput         = false;  // io.WantTextInput -- a text field has the keyboard
    bool uiWantsKeyboard   = false;  // io.WantCaptureKeyboard
    bool uiWantsMouse      = false;  // io.WantCaptureMouse
    bool playing           = false;  // playSessionActive()
    bool releasedByUser    = false;  // the player pressed the release-mouse chord mid-session
    bool defaultPawnPlay   = false;  // no GameMode: Play is standing in the engine's spectator pawn
    bool mouseCaptured     = false;  // the OS cursor is hidden and confined to the window
    bool pointerInViewport = false;  // levelHovered_ && inViewport(mouse)
    bool drawerOpen        = false;  // a content/console drawer is open
    bool landscapeMode     = false;  // the editor is in Landscape mode rather than Object mode
};

// The answer. Four permissions plus the scope mask the keybind registry needs.
struct InputOwnership {
    // GAME means the framework input bridge may publish this device to gameplay.
    bool keyboardToGame = false;
    bool mouseToGame    = false;
    // TOOL means an editor tool -- fly camera, gizmo, sculpt, foliage -- may act on this device.
    //
    // BOTH HALVES CAN BE TRUE AT ONCE, and the exhaustive sweep in InputOwnershipTest is what
    // established it rather than an argument: during no-GameMode spectator play the fly controls
    // drive the pawn AND the bridge publishes, deliberately. So the invariant this type guarantees
    // is the narrower one the test actually asserts -- WHILE A REAL GameMode SESSION IS LIVE, gameplay
    // and the editor tools never both own a device -- and not the tidier "at most one owner" assumed
    // here first. Outside a session both are true at once and that is correct: the bridge publishes
    // every frame (publishing nothing is what latched keys down) while the tools are obviously live.
    // Writing the strong version down would have meant weakening the code to fit the comment.
    // Both can also be false at once, whenever an ImGui text field has the keyboard.
    bool keyboardToTool = false;
    bool mouseToTool    = false;

    // Which editor::Scope bits are live right now.
    //
    // WHY THIS IS HERE AT ALL. KeybindRegistry::pressed() (EditorKeybinds.cpp) does NOT consult the
    // Scope bitmask its own KeybindDef table carries -- scope is used only at rebind time, by
    // conflictWith(), to decide whether two chords genuinely clash. At runtime every call site is
    // expected to remember to AND in its own extra condition, and a site that forgets fires a command
    // in a context its own table says it does not belong to. Handing the live mask out here gives
    // pressed() something to check against, so that stops depending on each caller's memory.
    u32 activeScopeMask = 0;
};

// Resolves the whole table. Reproduces the existing per-consumer gates exactly -- see the
// implementation for which call site each clause came from -- rather than imposing a new policy;
// changing behaviour and centralising it in one commit would make any resulting bug impossible to
// attribute to either half.
InputOwnership resolveInputOwnership(const InputConditions& c);

} // namespace aver::editor
