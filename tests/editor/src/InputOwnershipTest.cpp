// InputOwnershipTest -- the editor's input arbitration table.
//
// WHY THIS TEST CAN EXIST AT ALL, when almost nothing about the editor can be tested: resolveInputOwnership
// is a pure function of a plain bool struct. No ImGui context, no window, no device, no SandboxApp.
// That was the whole point of extracting it -- the decision was previously spread across ~11 call
// sites, each reading ImGuiIO inline, and therefore reachable only by running the editor and looking.
//
// WHAT IT IS DEFENDING. Two live bugs this session came from consumers disagreeing about who owns
// input: a play session that stopped publishing left every key latched down forever, and the editor
// fly camera fought a possessed first-person pawn. Both were one consumer's private answer to a
// question nobody owned. These assertions are that question's answers, written down.
#include "InputOwnership.hpp"

#include <cstdio>

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

// A frame with a project open, an ImGui frame up, nothing playing and nothing hovered. Every test
// below starts from this and changes only the field it is about, so a failure names one condition
// rather than a whole configuration.
InputConditions editing() {
    InputConditions c;
    c.uiActive = true;
    return c;
}

} // namespace

int main() {
    std::printf("[INFO ] === input ownership ===\n");

    // ---- the ordinary editing frame ----
    {
        InputConditions c = editing();
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        check(o.keyboardToTool, "editing: the tools get the keyboard");
        check(o.mouseToTool,    "editing: the pointer is over the viewport, so the tools get the mouse");
        // AND THE GAME STILL GETS IT TOO, which looks wrong and is not. pushInput publishes every
        // frame whether or not a session is running -- publishing nothing is what latched keys down
        // forever. With no session live there is no gameplay to receive it.
        check(o.keyboardToGame, "editing: the input bridge still publishes (a quiet frame is still a published frame)");
    }

    // ---- a headless / no-UI frame ----
    {
        InputConditions c = editing();
        c.uiActive = false;
        const InputOwnership o = resolveInputOwnership(c);
        check(!o.keyboardToGame, "no ImGui frame: nothing is published to gameplay");
        check(!o.mouseToGame,    "no ImGui frame: no mouse to gameplay");
        check(!o.keyboardToTool, "no ImGui frame: no tools either");
    }

    // ---- a live play session owns the devices ----
    {
        InputConditions c = editing();
        c.playing = true;
        c.mouseCaptured = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        check(o.keyboardToGame,  "playing: the game gets the keyboard");
        check(o.mouseToGame,     "playing: the game gets the mouse");
        check(!o.keyboardToTool, "playing: the editor tools stand down");
        check(!o.mouseToTool,    "playing: the editor tools stand down for the mouse too");
    }

    // ---- THE RELEASE CHORD, and the bug it caused ----
    {
        InputConditions c = editing();
        c.playing = true;
        c.releasedByUser = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        // The game must be told the keys are UP -- not left holding whatever it had. This is the
        // arbitration half of the stuck-key bug: the publish itself still happens (pushInput never
        // returns early any more), and this is what makes its published value a release.
        check(!o.keyboardToGame, "released: gameplay gets nothing, so the bridge publishes releases");
        check(!o.mouseToGame,    "released: gameplay gets no mouse either");
        // ...and the editor takes over, which is the entire point of the chord.
        check(o.keyboardToTool,  "released: the editor tools take the keyboard back");
        check(o.mouseToTool,     "released: the editor tools take the mouse back");
    }

    // ---- the no-GameMode spectator exception ----
    {
        InputConditions c = editing();
        c.playing = true;
        c.defaultPawnPlay = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        // A project with no GameMode has no gameplay code to drive the pawn -- the editor fly
        // controls ARE the spectator camera. Without this exception the block stood down the instant
        // Play began and the camera froze.
        check(o.keyboardToTool, "spectator play: the fly controls stay live because nothing else drives the pawn");
    }

    // ---- ImGui wants the devices ----
    {
        InputConditions c = editing();
        c.uiWantsKeyboard = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        check(!o.keyboardToGame, "a focused widget: gameplay gets no keyboard");
        check(!o.keyboardToTool, "a focused widget: the tools get no keyboard");
    }
    {
        InputConditions c = editing();
        c.uiWantsMouse = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        check(!o.mouseToTool, "the pointer is over a panel: the tools get no mouse");
    }

    // ---- CAPTURE OVERRIDES WantCaptureMouse, deliberately ----
    {
        InputConditions c = editing();
        c.playing = true;
        c.mouseCaptured = true;
        c.uiWantsMouse = true;   // stale: the OS cursor is hidden and confined, there is no widget under it
        const InputOwnership o = resolveInputOwnership(c);
        check(o.mouseToGame, "captured: gameplay still gets the mouse, because WantCaptureMouse is a stale answer");
    }

    // ---- A TEXT FIELD BEATS EVERYTHING ----
    {
        InputConditions c = editing();
        c.playing = true;
        c.mouseCaptured = true;
        c.textInput = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        // Typing WASD into a rename box must not also walk the character or fly the camera.
        check(!o.keyboardToGame, "typing: gameplay gets no keyboard");
        check(!o.keyboardToTool, "typing: the tools get no keyboard");
    }

    // ---- the project browser ----
    {
        InputConditions c = editing();
        c.browserActive = true;
        c.pointerInViewport = true;
        const InputOwnership o = resolveInputOwnership(c);
        check(!o.keyboardToTool, "the start screen: there is no level, so no tools");
        check(!o.mouseToTool,    "the start screen: no viewport tools either");
    }

    // ---- THE INVARIANT: one device, at most one owner ----
    // Exhaustive over every combination of the twelve conditions. 4096 cases is nothing to run and it
    // is the only honest way to claim the table has no contradictory corner -- a hand-picked set of
    // cases proves only that the cases somebody thought of are fine.
    {
        bool bothEver = false;
        for (unsigned bits = 0; bits < (1u << 12); ++bits) {
            InputConditions c;
            c.uiActive          = (bits >> 0)  & 1u;
            c.browserActive     = (bits >> 1)  & 1u;
            c.textInput         = (bits >> 2)  & 1u;
            c.uiWantsKeyboard   = (bits >> 3)  & 1u;
            c.uiWantsMouse      = (bits >> 4)  & 1u;
            c.playing           = (bits >> 5)  & 1u;
            c.releasedByUser    = (bits >> 6)  & 1u;
            c.defaultPawnPlay   = (bits >> 7)  & 1u;
            c.mouseCaptured     = (bits >> 8)  & 1u;
            c.pointerInViewport = (bits >> 9)  & 1u;
            c.drawerOpen        = (bits >> 10) & 1u;
            c.landscapeMode     = (bits >> 11) & 1u;
            const InputOwnership o = resolveInputOwnership(c);
            if (o.keyboardToGame && o.keyboardToTool) bothEver = true;
            if (o.mouseToGame && o.mouseToTool)       bothEver = true;
        }
        // NOT ASSERTED, AND THAT IS THE FINDING WORTH RECORDING. The tools' gate and the bridge's
        // gate are genuinely allowed to be true together during spectator play: the fly controls
        // drive the pawn AND the bridge publishes, on purpose. So the honest invariant is weaker than
        // "at most one owner", and writing the strong version would have meant weakening the code to
        // fit a test. What is asserted below is the narrower thing that must hold.
        (void)bothEver;
    }

    // ---- the narrower invariant that MUST hold: a real session with the mouse captured is exclusive ----
    {
        int violations = 0;
        for (unsigned bits = 0; bits < (1u << 12); ++bits) {
            InputConditions c;
            c.uiActive          = (bits >> 0)  & 1u;
            c.browserActive     = (bits >> 1)  & 1u;
            c.textInput         = (bits >> 2)  & 1u;
            c.uiWantsKeyboard   = (bits >> 3)  & 1u;
            c.uiWantsMouse      = (bits >> 4)  & 1u;
            c.playing           = true;           // ONLY meaningful while a session is live -- see below
            c.releasedByUser    = (bits >> 6)  & 1u;
            c.defaultPawnPlay   = false;          // a REAL GameMode session, not the spectator fallback
            c.mouseCaptured     = (bits >> 8)  & 1u;
            c.pointerInViewport = (bits >> 9)  & 1u;
            c.drawerOpen        = (bits >> 10) & 1u;
            c.landscapeMode     = (bits >> 11) & 1u;
            const InputOwnership o = resolveInputOwnership(c);
            if (o.keyboardToGame && o.keyboardToTool) ++violations;
            if (o.mouseToGame && o.mouseToTool)       ++violations;
        }
        // PINNED TO playing = true, AND THE SWEEP IS WHAT FORCED THAT. The first version of this left
        // `playing` free and failed, correctly: with no session running the bridge publishes AND the
        // tools are live, both true at once, which the very first assertion in this file calls the
        // right answer. The exclusivity claim is only meaningful WHILE A SESSION IS LIVE -- outside
        // one there is no gameplay to be exclusive with. Weakening the sweep to fit was the wrong fix
        // and narrowing what it claims was the right one.
        check(violations == 0,
              "exhaustive over 2048 states: while a real GameMode session is live, gameplay and the "
              "editor tools never both own a device");
    }

    // ---- the scope mask ----
    {
        InputConditions c = editing();
        const InputOwnership o = resolveInputOwnership(c);
        check((o.activeScopeMask & kScopeObjectMode) != 0,   "object mode is live by default");
        check((o.activeScopeMask & kScopeLandscapeMode) == 0, "landscape mode is not");
        check((o.activeScopeMask & kScopeGlobalUI) != 0,      "global UI commands are always live");
        check((o.activeScopeMask & kScopePlaySession) == 0,   "no session, no play-session commands");
    }
    {
        InputConditions c = editing();
        c.landscapeMode = true;
        c.playing = true;
        c.drawerOpen = true;
        const InputOwnership o = resolveInputOwnership(c);
        check((o.activeScopeMask & kScopeLandscapeMode) != 0, "landscape mode is live when selected");
        // EXCLUSIVE BY CONSTRUCTION, which is exactly what EditorKeybinds' Scope comment relies on to
        // say Tool.Select and Sculpt.Raise can share a chord without it being a real conflict.
        check((o.activeScopeMask & kScopeObjectMode) == 0,    "...and object mode is not, so the two never overlap");
        check((o.activeScopeMask & kScopePlaySession) != 0,   "a live session lights the play scope");
        check((o.activeScopeMask & kScopeDrawerOpen) != 0,    "an open drawer lights the drawer scope");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
