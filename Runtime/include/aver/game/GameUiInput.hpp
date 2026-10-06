// GameUiInput: the host half of the retained game-UI widgets (modules/ui, docs/GAME_UI.md). The
// widget tree is driven through aver_ui_*; what only a HOST can do is feed it the frame's keyboard,
// mouse buttons and pad, and let an open menu take input away from gameplay. Shared by the editor's
// Play session and the packaged game for the reason GameTick.hpp is: two copies drift.
//
// HEADER-ONLY, evaluated per host with its own AVER_WITH_UI_ABI.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/game/GameInput.hpp"

#if AVER_WITH_UI_ABI
#  include "aver/framework/InputKeys.hpp"
#  include "aver/framework/framework_abi.h"
#  include "aver/platform/Gamepad.hpp"
#  include "aver/platform/InputState.hpp"
#  include "aver/ui/ui_widget_abi.h"
#endif

namespace aver::game {

// What the widget tree asked for as of last frame (AVER_UI_WANTS_*); 0 without the widget ABI.
inline u32 uiWants() {
#if AVER_WITH_UI_ABI
    return aver_ui_wants_input();
#else
    return 0;
#endif
}

// True while a menu or text field wants the OS cursor free (mouse capture must be off).
inline bool uiWantsCursor() { return (uiWants() & 8u) != 0; }   // AVER_UI_WANTS_CURSOR

// An open menu pauses gameplay input: every device reads released this frame (publishInput writes the
// explicit release, so nothing sticks), and the cursor is freed so the player can reach the menu.
// Call on the policy before publishInput. Returns true when the cursor must be free.
inline bool uiGateInput(InputPublishPolicy& policy) {
    const u32 wants = uiWants();
#if AVER_WITH_UI_ABI
    if (wants & AVER_UI_WANTS_PAUSE) {
        policy.keyboardToGame = false;
        policy.mouseToGame = false;
        policy.gamepadActive = false;
    }
    return (wants & AVER_UI_WANTS_CURSOR) != 0;
#else
    (void)policy;
    return false;
#endif
}

#if AVER_WITH_UI_ABI
// Feeds this frame's raw input to the widget tree: navigation, text editing, typed characters, the
// wheel, and the key a rebinding widget is waiting for. `pointerValid` is false while the game owns
// the mouse (mouse-look), so a hidden cursor cannot click a button. Call before aver_ui_widgets_frame.
inline void uiFeedInput(const InputState& in, f32 dt, bool pointerValid) {
    uint8_t pressed[256] = {}, held[256] = {};
    int16_t slot[256];
    for (i32 vk = 0; vk < 256; ++vk) {
        pressed[vk] = in.keyPressed(vk) ? 1 : 0;
        held[vk] = in.keyHeld(vk) ? 1 : 0;
        slot[vk] = static_cast<int16_t>(fw::frameworkKeyFromVk(vk));
    }
    // Mouse buttons ride the same arrays at VK 1, 2 and 4 so a mouse button can be rebound.
    const i32 mouseVk[3] = {0x01, 0x02, 0x04};
    const i32 mouseSlot[3] = {AVER_FW_KEY_MOUSE_LEFT, AVER_FW_KEY_MOUSE_RIGHT, AVER_FW_KEY_MOUSE_MIDDLE};
    for (i32 b = 0; b < 3; ++b) {
        pressed[mouseVk[b]] = in.mousePressed(b) ? 1 : 0;
        held[mouseVk[b]] = in.mouseHeld(b) ? 1 : 0;
        slot[mouseVk[b]] = static_cast<int16_t>(mouseSlot[b]);
    }
    GamepadState pad;
    pollGamepads(&pad, 1);
    uint8_t buttons[GamepadState::kButtonCount] = {};
    for (usize i = 0; i < GamepadState::kButtonCount; ++i) buttons[i] = pad.buttons[i] ? 1 : 0;
    aver_ui_input_host_frame(dt, pressed, held, slot, pad.connected ? 1 : 0, buttons, pad.axes);
    aver_ui_input_wheel(in.wheel());
    aver_ui_input_pointer_valid(pointerValid ? 1 : 0);
}
#endif

} // namespace aver::game
