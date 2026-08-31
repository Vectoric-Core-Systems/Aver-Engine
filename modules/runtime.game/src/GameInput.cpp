// InputState -> aver_fw_* : the bridge that makes a game playable without ImGui.
//
// THE DEFECT THIS CLOSES. Until today the only thing in the tree that turned OS input into gameplay
// input was SandboxApp::pushInput, whose entire body sits inside `#if AVER_WITH_IMGUI` and reads
// ImGui::IsKeyDown / io.MouseDelta. Every other aver_fw_input_set_key call site is the hardcoded
// --play-test harness that holds W. So ImGui was a FUNCTIONAL dependency of being able to play, and
// an AVER_ENABLE_UI=OFF build -- which is exactly what a shipped game is -- had no input at all.
//
// The MECHANISM is lifted; the POLICY is not. The editor decides whether to publish input by asking
// ImGui whether it wants the keyboard (io.WantCaptureKeyboard) and whether the pointer is inside the
// dockspace's central node. A game has no panels to lose focus to: if the window has focus, the
// player is playing.
#include "aver/game/GameInput.hpp"

#if AVER_MODULE_FRAMEWORK

#include "aver/core/Log.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/framework/InputKeys.hpp"
#include "aver/platform/InputState.hpp"

#include <string>

namespace aver::game {
namespace {

// The VK mapping and the key names moved to aver/framework/InputKeys.hpp so the editor can use the
// same table -- see that header for why a mapping that disagrees between two hosts is worse than
// most. Pulled into this namespace by name so every call site below reads exactly as it did.
using aver::fw::frameworkKeyFromVk;
using aver::fw::frameworkKeyName;

} // namespace

void publishInput(const InputState& in, bool focused, std::string* echo) {
    aver_fw_input_new_frame();

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
    if (focused) {
        for (i32 vk = 0; vk < static_cast<i32>(InputState::kKeyCount); ++vk) {
            const i32 fw = frameworkKeyFromVk(vk);
            if (fw >= 0 && in.keyHeld(vk)) held[fw] = true;
        }
        held[AVER_FW_KEY_MOUSE_LEFT]   = in.mouseHeld(0);
        held[AVER_FW_KEY_MOUSE_RIGHT]  = in.mouseHeld(1);
        held[AVER_FW_KEY_MOUSE_MIDDLE] = in.mouseHeld(2);
    }

    std::string names;
    for (i32 k = 0; k < AVER_FW_KEY_COUNT; ++k) {
        aver_fw_input_set_key(k, held[k] ? 1 : 0);
        if (echo && held[k]) {
            if (!names.empty()) names += '+';
            names += frameworkKeyName(k);
        }
    }

    // Mouse DELTA, not position: the framework's look input is relative. InputState accumulates
    // every WM_MOUSEMOVE in the frame rather than keeping the last, which matters at high poll
    // rates where a frame holds a dozen of them. Zeroed when unfocused for the same reason as the
    // keys -- a stale delta would spin the camera on the frame focus returns.
    if (focused) {
        aver_fw_input_set_mouse(static_cast<f32>(in.mouseDX()), static_cast<f32>(in.mouseDY()), in.wheel());
    } else {
        aver_fw_input_set_mouse(0.0f, 0.0f, 0.0f);
    }

    if (echo) *echo = focused ? (names.empty() ? "(none)" : names) : std::string();
}

} // namespace aver::game

#endif // AVER_MODULE_FRAMEWORK
