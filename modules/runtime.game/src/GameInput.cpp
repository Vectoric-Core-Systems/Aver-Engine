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
#include "aver/platform/InputState.hpp"

#include <string>

namespace aver::game {
namespace {

// Win32 virtual key -> framework key. Returns -1 for keys the framework has no slot for.
//
// VK_SHIFT/VK_CONTROL/VK_MENU and NOT VK_LSHIFT/VK_LCONTROL/VK_LMENU: WM_KEYDOWN delivers the
// UNSIDED code unless the receiver does the extended-key dance, so mapping the sided ones would
// produce a Shift key that never registers. The framework's slot is named LSHIFT but means "shift".
i32 frameworkKey(i32 vk) {
    if (vk >= 'A' && vk <= 'Z') return AVER_FW_KEY_A + (vk - 'A');
    if (vk >= '0' && vk <= '9') return AVER_FW_KEY_0 + (vk - '0');
    switch (vk) {
        case 0x20: return AVER_FW_KEY_SPACE;    // VK_SPACE
        case 0x10: return AVER_FW_KEY_LSHIFT;   // VK_SHIFT
        case 0x11: return AVER_FW_KEY_LCTRL;    // VK_CONTROL
        case 0x12: return AVER_FW_KEY_LALT;     // VK_MENU
        case 0x0D: return AVER_FW_KEY_ENTER;    // VK_RETURN
        case 0x1B: return AVER_FW_KEY_ESCAPE;   // VK_ESCAPE
        case 0x09: return AVER_FW_KEY_TAB;      // VK_TAB
        case 0x25: return AVER_FW_KEY_LEFT;
        case 0x26: return AVER_FW_KEY_UP;
        case 0x27: return AVER_FW_KEY_RIGHT;
        case 0x28: return AVER_FW_KEY_DOWN;
        default:   return -1;
    }
}

const char* frameworkKeyName(i32 k) {
    static const char* kNames[] = {
        "A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
        "0","1","2","3","4","5","6","7","8","9",
        "SPACE","LSHIFT","LCTRL","LALT","ENTER","ESCAPE","TAB",
        "LEFT","RIGHT","UP","DOWN","MOUSE_LEFT","MOUSE_RIGHT","MOUSE_MIDDLE",
    };
    if (k < 0 || k >= AVER_FW_KEY_COUNT) return "?";
    return kNames[k];
}

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
            const i32 fw = frameworkKey(vk);
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
