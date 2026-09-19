// InputState -> aver_fw_* : the bridge that makes a game playable without ImGui.
//
// THE DEFECT THIS CLOSES. Until today the only thing in the tree that turned OS input into gameplay
// input was SandboxApp::pushInput, whose entire body sits inside `#if AVER_WITH_IMGUI` and reads
// ImGui::IsKeyDown / io.MouseDelta. Every other aver_fw_input_set_key call site is the hardcoded
// --play-test harness that holds W. So ImGui was a FUNCTIONAL dependency of being able to play, and
// an AVER_ENABLE_UI=OFF build -- which is exactly what a shipped game is -- had no input at all.
//
// THE MECHANISM IS LIFTED; THE POLICY IS NOW AN ARGUMENT. The editor decides whether to publish
// input by asking ImGui whether it wants the keyboard (io.WantCaptureKeyboard) and whether the
// pointer is inside the dockspace's central node. A game has no panels to lose focus to: if the
// window has focus, the player is playing. Those two answers used to justify two separate
// implementations of the publish, and that is exactly what went wrong -- see InputPublishPolicy's
// own comment in GameInput.hpp for what the second one cost. They now differ only in what a caller
// fills into the policy it hands this function.
#include "aver/game/GameInput.hpp"

#if AVER_MODULE_FRAMEWORK

#include "aver/core/Log.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/framework/InputKeys.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/platform/Gamepad.hpp"

#include <string>

namespace aver::game {
namespace {

// The VK mapping and the key names moved to aver/framework/InputKeys.hpp so the editor can use the
// same table -- see that header for why a mapping that disagrees between two hosts is worse than
// most. Pulled into this namespace by name so every call site below reads exactly as it did.
using aver::fw::frameworkKeyFromVk;
using aver::fw::frameworkKeyName;

// GamepadState's arrays are laid out in the ABI's own documented order (Gamepad.hpp's own comment
// spells the order out without including this header) precisely so the loops below can walk it
// index-for-index instead of a name-by-name switch. If either side's count ever drifts the mismatch
// would silently under- or over-publish rather than fail to compile, which is worse -- so pin it here.
static_assert(GamepadState::kButtonCount == static_cast<usize>(AVER_FW_GAMEPAD_BUTTON_COUNT),
              "GamepadState::buttons and AVER_FW_GAMEPAD_* have drifted apart");
static_assert(GamepadState::kAxisCount == static_cast<usize>(AVER_FW_GAMEPAD_AXIS_COUNT),
              "GamepadState::axes and AVER_FW_GAMEPAD_AXIS_* have drifted apart");

} // namespace

void publishInput(const InputState& in, const InputPublishPolicy& policy, std::string* echo) {
    aver_fw_input_new_frame();

    // THE THREE GATES, EACH ALREADY ANDED WITH THE MASTER SWITCH, resolved once here so no line
    // below has to remember to repeat `policy.focused &&` -- forgetting it on one device is how a
    // backgrounded window keeps feeding its pawn, which is the defect GameApp.cpp's own foreground
    // query (and its comment) was added to close. Only the per-device field differs between them:
    // the editor resolves three separate ownership questions (own_.keyboardToGame, own_.mouseToGame
    // and !suppressed, see sandbox/src/SandboxPlay.cpp:338-390), while this host's caller passes the
    // same foreground answer to all three, so the standalone runtime behaves exactly as it did when
    // this function took one `focused` bool.
    const bool kb  = policy.focused && policy.keyboardToGame;
    const bool m   = policy.focused && policy.mouseToGame;
    const bool pad = policy.focused && policy.gamepadActive;

    // Collected into a dense array indexed by FRAMEWORK key, not published as the VK loop walks.
    // Two reasons, and the second was a bug:
    //
    //  1. The echo string then reads in framework order (A..Z, 0..9, SPACE, LSHIFT, ...) rather
    //     than Win32 order, where VK_SHIFT is 0x10 and sorts before 'W' at 0x57. "W+LSHIFT" is
    //     what a person expects to see; "LSHIFT+W" is an implementation detail leaking out.
    //  2. It makes the unfocused case below able to publish an explicit RELEASE for every key.
    bool held[AVER_FW_KEY_COUNT] = {};

    // AN UNFOCUSED WINDOW PUBLISHES EVERY KEY AS UP, and does not simply return.
    //
    // The first version of this returned early after aver_fw_input_new_frame(), on the assumption
    // that new_frame clears the key state. IT DOES NOT -- it rolls the pressed/released edges and
    // leaves `held` intact. So a key held when the player alt-tabbed stayed held in the framework
    // and kept driving the pawn, in a build where InputState itself had correctly cleared. The
    // character walks into a wall while the player is in another window, and nothing logs.
    // InputBridgeTest asserts this directly.
    //
    // THE KEYBOARD AND THE THREE MOUSE BUTTONS ARE GATED SEPARATELY even though both land in this
    // one array, because the editor's answer to "does the game own this device" genuinely differs
    // per device: a name field takes the keyboard while the cursor still hovers the viewport.
    if (kb) {
        for (i32 vk = 0; vk < static_cast<i32>(InputState::kKeyCount); ++vk) {
            const i32 fw = frameworkKeyFromVk(vk);
            if (fw >= 0 && in.keyHeld(vk)) held[fw] = true;
        }
    }
    if (m) {
        held[AVER_FW_KEY_MOUSE_LEFT]   = in.mouseHeld(0);
        held[AVER_FW_KEY_MOUSE_RIGHT]  = in.mouseHeld(1);
        held[AVER_FW_KEY_MOUSE_MIDDLE] = in.mouseHeld(2);
    }

    // The raw Win32 VK twin (framework_abi.h's own RAW WIN32 VK section) gets the SAME "unfocused
    // means every key publishes as up" discipline as the named slots just above, and for the exact
    // same reason this function's own header comment spells out: an early return, or any path that
    // skips a slot instead of writing an explicit 0 to it, leaves that slot frozen at whatever it
    // last was while aver_fw_tick keeps running. That bug already bit the named enum once
    // (InputBridgeTest asserts against it there); reintroducing it for the 200-odd VKs the named
    // enum cannot reach would be the same defect wearing a new array. `kb &&` is ANDed into the
    // value itself, not a guard around the loop, so every one of the AVER_FW_VK_COUNT calls below
    // still runs and publishes an explicit answer every frame, whatever the policy said. The twin
    // takes the KEYBOARD gate, the same one the named slots above took, so the two halves of one
    // physical key can never disagree -- which is precisely the disagreement the editor's private
    // re-implementation of this publish created, and the reason .ocinput's scheme system (which
    // reads this raw contract back through EnhancedInput) now depends on them agreeing.
    for (i32 vk = 0; vk < static_cast<i32>(InputState::kKeyCount); ++vk)
        aver_fw_input_set_vk(vk, (kb && in.keyHeld(vk)) ? 1 : 0);

    // EATING A SLOT SUBTRACTS FROM THE VALUE, NOT FROM THE LOOP. A host chord that claims SPACE
    // this frame still gets an explicit release written into SPACE; the one thing it must never do
    // is leave the slot out, because aver_fw_input_new_frame does not clear cur[] and the slot
    // would stand at whatever it last held for as long as the chord is down -- the same stuck-key
    // defect this function's unfocused branch above exists to prevent, wearing a chord's clothes.
    std::string names;
    for (i32 k = 0; k < AVER_FW_KEY_COUNT; ++k) {
        const bool down = held[k] && !policy.eaten[k];
        aver_fw_input_set_key(k, down ? 1 : 0);
        if (echo && down) {
            if (!names.empty()) names += '+';
            names += frameworkKeyName(k);
        }
    }

    // Mouse DELTA, not position: the framework's look input is relative. InputState accumulates
    // every WM_MOUSEMOVE in the frame rather than keeping the last, which matters at high poll
    // rates where a frame holds a dozen of them. Zeroed when the mouse is not the game's for the
    // same reason as the keys -- a stale delta would spin the camera on the frame it comes back.
    //
    // CAPTURED REPLACES THE SOURCE, NOT THE GATE: `policy.captured` only ever matters inside the
    // branch that was going to publish a delta anyway (see InputPublishPolicy::captured for why) --
    // the wheel still comes from `in` either way, since capture confines and hides the cursor but
    // has no opinion about the wheel. The wheel rides the same `m` gate as the delta and the
    // buttons, which is what the editor's own publish already does for it.
    if (m) {
        aver_fw_input_set_mouse(policy.captured ? policy.capturedDx : static_cast<f32>(in.mouseDX()),
                                 policy.captured ? policy.capturedDy : static_cast<f32>(in.mouseDY()),
                                 in.wheel());
    } else {
        aver_fw_input_set_mouse(0.0f, 0.0f, 0.0f);
    }

    publishGamepad(pad);

    // Still keyed on the MASTER switch and not on `kb`: this string is what --input-echo prints,
    // and "the window is not ours" is a different statement from "we are focused and holding
    // nothing", which is what "(none)" says. InputBridgeTest asserts both readings.
    if (echo) *echo = policy.focused ? (names.empty() ? "(none)" : names) : std::string();
}

// ---- GAMEPAD, published pad 0 only -- the ABI itself accepts nothing else (framework_abi.h:
// "pad is fixed at 0 for every call"). Polled unconditionally, the same as publishInput's own
// keyboard loop reads `in` unconditionally: only the PUBLISHED value is gated on `active`, so
// Aver.Platform's own hotplug/re-probe throttle (Gamepad.hpp) keeps ticking across an inactive
// stretch instead of resetting cold the moment it ends. Shared by both hosts -- publishInput hands
// it the policy's gamepadActive, which the standalone runtime fills with the same foreground answer
// it gives the keyboard, while the editor's Play session still calls here directly with
// `!suppressed` -- each passing its own rule for when the game does not own the device.
void publishGamepad(bool active) {
    GamepadState pad{};
    pollGamepads(&pad, 1);
    for (i32 b = 0; b < AVER_FW_GAMEPAD_BUTTON_COUNT; ++b)
        aver_fw_input_set_gamepad_button(0, b, (active && pad.buttons[b]) ? 1 : 0);
    for (i32 a = 0; a < AVER_FW_GAMEPAD_AXIS_COUNT; ++a)
        aver_fw_input_set_gamepad_axis(0, a, active ? pad.axes[a] : 0.0f);
}

} // namespace aver::game

#endif // AVER_MODULE_FRAMEWORK
