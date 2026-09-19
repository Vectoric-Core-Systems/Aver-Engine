// The InputState -> aver_fw_* bridge.
#pragma once
#include "aver/core/Types.hpp"

#include <string>

#if AVER_MODULE_FRAMEWORK
// For AVER_FW_KEY_COUNT alone, which dimensions InputPublishPolicy::eaten below. The array is sized
// by the enum rather than by a second constant kept in step by hand -- a policy that could be one
// slot shorter than the loop that reads it is a silent out-of-bounds read, not a compile error.
#include "aver/framework/framework_abi.h"
#endif

namespace aver {
class InputState;
}

namespace aver::game {

#if AVER_MODULE_FRAMEWORK
// WHO OWNS WHICH DEVICE THIS FRAME -- one named policy rather than the five positional arguments
// publishInput used to take.
//
// THE DEFECT THIS WIDENING EXISTS TO CLOSE is not in this file. sandbox/src/SandboxPlay.cpp's
// SandboxApp::pushInput never called publishInput at all: it re-implemented the whole publish
// against ImGui, hand-enumerating 47 ImGui keys into the named AVER_FW_KEY_* slots while filling
// the raw-VK twin from its own InputState. So in the editor the named slot and its own raw-VK twin
// were read from TWO DIFFERENT SOURCES for the same physical key -- and the .ocinput scheme system
// (commit 02bf9fac) now reads that same raw contract back through EnhancedInput, which makes the
// disagreement a second consumer's problem too. The editor could not simply call this function
// because it needs per-device gates the old signature had no room for. These fields are that room.
struct InputPublishPolicy {
    // THE MASTER SWITCH, and the one field the others cannot overrule: false publishes an explicit
    // RELEASE into every slot -- named, raw VK, mouse, pad -- rather than skipping any of them. See
    // publishInput's own stuck-key comment for why skipping is the defect and not the shortcut.
    bool focused = false;
    // Whether the KEYS -- the named slots and their raw-VK twins alike -- belong to the game this
    // frame. The editor's own term for this is own_.keyboardToGame, which goes false the moment an
    // ImGui field wants the keyboard; the standalone runtime has no panels to lose the keyboard to,
    // so it passes the same value as `focused`.
    bool keyboardToGame = false;
    // Whether the mouse BUTTONS, the look delta and the wheel belong to the game. Not folded into
    // keyboardToGame because the editor's two answers genuinely diverge, and in both directions:
    // resolveInputOwnership (sandbox/src/InputOwnership.cpp:18-20) lets CAPTURE override the panel
    // flags for the mouse alone -- a hidden, confined cursor has no widget it could be interacting
    // with -- so a frame where a text field holds the keyboard can still be a frame where the
    // player is looking around; and a pointer resting on a panel takes the mouse while the keyboard
    // stays the game's.
    bool mouseToGame = false;
    // Whether pad 0 belongs to the game. ITS OWN FIELD RATHER THAN FOLLOWING keyboardToGame, and
    // SandboxPlay.cpp's existing comment on that gate says exactly why: "an ImGui text field steals
    // a keystroke, never a controller button". That host already gates its pad on the wider
    // `!suppressed` while the keyboard takes the narrower own_.keyboardToGame
    // (sandbox/src/SandboxPlay.cpp:390); folding the two together here would mute the controller
    // every time someone clicked into a text field. The runtime, again, passes `focused`.
    bool gamepadActive = false;
    // When true, capturedDx/capturedDy feed the framework's mouse delta INSTEAD OF `in`'s own
    // window-accumulated deltas -- see GameApp::pollCapturedMouse for where those come from and
    // why: `in.mouseDX()`/`mouseDY()` accumulate WM_MOUSEMOVE, which stops moving the instant the
    // OS cursor (invisible, but still real) reaches the clip rect's edge, so a player looking hard
    // in one direction would run out of travel after a few hundred pixels of window width. The
    // captured deltas instead come from re-centring the (hidden, confined) cursor every frame,
    // which has no edge to hit. CAPTURE REPLACES THE SOURCE OF THE DELTA, NOT THE GATE: it is read
    // only where the delta would have been published anyway, and a frame that publishes 0,0 because
    // the mouse is not the game's publishes 0,0 whatever these hold. The wheel is never captured --
    // confining and hiding the cursor has no opinion about it.
    bool captured = false;
    f32  capturedDx = 0.0f, capturedDy = 0.0f;
    // Named slots a HOST CHORD is claiming this frame, indexed by AVER_FW_KEY_*. The editor's
    // drawer toggle takes SPACE and LCTRL and its dismiss takes ESCAPE, and the key that opened a
    // panel must not also make the pawn jump.
    //
    // AN EATEN SLOT IS STILL PUBLISHED -- as an explicit release -- and is never skipped. That is
    // the entire difference between "this slot is eaten" and "the publish stopped": a skipped slot
    // stays latched at whatever it last held while aver_fw_tick keeps running, which is the exact
    // stuck-key defect publishInput's own comment and InputBridgeTest's unfocused case exist to
    // prevent. InputBridgeTest's eaten case asserts the other half too -- that a second key held at
    // the same time still reads HELD -- because that is what proves the publish did not stop.
    //
    // The raw-VK twin is deliberately NOT eaten: a chord is authored against a named slot, the
    // editor has never eaten a raw VK, and doing it here would need a reverse framework-key -> VK
    // table that InputKeys.hpp does not have and that would rot the moment either side moved.
    bool eaten[AVER_FW_KEY_COUNT] = {};
};

// Publishes this frame's accumulated input into the framework's input state, under `policy` --
// which decides, per device, whether the game is the one that gets it. Every named slot, every raw
// VK, the mouse and pad 0 are written every frame with a value whatever the policy says; nothing is
// ever left to stand at last frame's answer.
//
// `echo`, when non-null, receives a human-readable list of the keys held, so --input-echo can prove
// the path end to end without a debugger. It lists what was PUBLISHED rather than what is
// physically down, which is the useful reading of it: an eaten or ungated key is not input the
// game got.
void publishInput(const InputState& in, const InputPublishPolicy& policy,
                   std::string* echo = nullptr);

// Publishes pad 0's gamepad state into the framework -- shared by both hosts, which otherwise
// carried identical copies of this block. `active` gates publication exactly as the policy's own
// per-device gates do for publishInput's keyboard and mouse: everything reads released/zeroed when
// false. publishInput calls this with InputPublishPolicy::gamepadActive; the editor's Play session
// still calls it directly with its own `!suppressed` -- see the definition for why the pad is
// polled every frame regardless.
void publishGamepad(bool active);
#endif

} // namespace aver::game
