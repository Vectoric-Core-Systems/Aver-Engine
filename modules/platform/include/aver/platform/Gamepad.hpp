#pragma once
#include "aver/core/Types.hpp"

namespace aver {

// One controller slot, after dead-zone shaping and range normalisation -- the poller framework_abi.h's
// own gamepad section says does not exist yet ("GAMEPAD, SHAPE ONLY -- NO POLLING": no XInput open, no
// hotplug detection, no dead zone). This is that poller.
//
// INDEPENDENT OF THE FRAMEWORK, deliberately, the same way InputState is: no aver_fw_* call and no
// framework_abi.h include anywhere in this header or Gamepad.cpp. A caller wires this into the ABI
// at its own call site, and there is exactly ONE such site in the tree: Runtime/src/GameInput.cpp's
// publishGamepad, which does the equivalent translation for keyboard/mouse in the same file. The
// editor reaches the pad only through that -- it calls game::publishInput, which calls
// publishGamepad from the policy's gamepadActive -- and includes this header nowhere.
//
// buttons[]/axes[] ARE LAID OUT IN THE ABI'S OWN DOCUMENTED ORDER -- AVER_FW_GAMEPAD_* and
// AVER_FW_GAMEPAD_AXIS_* in framework_abi.h -- so a caller publishes them with one indexed loop
// (`aver_fw_input_set_gamepad_button(pad, i, buttons[i])`) rather than a name-by-name switch. That
// order, spelled out once here so this file stays honest about the contract it mirrors without
// including the header that defines it:
//   buttons: DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, START, BACK, LEFT_THUMB, RIGHT_THUMB,
//            LEFT_SHOULDER, RIGHT_SHOULDER, A, B, X, Y
//   axes:    LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER
struct GamepadState {
    static constexpr usize kButtonCount = 14;  // mirrors AVER_FW_GAMEPAD_BUTTON_COUNT
    static constexpr usize kAxisCount   = 6;   // mirrors AVER_FW_GAMEPAD_AXIS_COUNT

    bool connected = false;
    bool buttons[kButtonCount] = {};
    // Sticks in [-1,1], triggers in [0,1] -- see Gamepad.cpp for the exact dead-zone shaping. Sign
    // matches XInput's own raw fields untouched (Y already reads "up positive"), which is the
    // convention framework_abi.h's gamepad section says its axes mirror ("modelled on XInput's own
    // ... XINPUT_STATE thumbstick/trigger fields"), so no flip belongs here or at any caller.
    f32 axes[kAxisCount] = {};
};

// Local controller slots this module will poll -- XInput's own XUSER_MAX_COUNT, not a limit this
// module invents. framework_abi.h's gamepad ABI accepts only pad 0 today ("pad is fixed at 0 for
// every call ... the same only-player-0 precedent aver_fw_player_controller documents"); polling the
// full XInput range anyway means the day the ABI grows a second local player, this module is not the
// thing standing in the way.
constexpr u32 kMaxGamepads = 4;

// Polls every slot in [0, min(count, kMaxGamepads)) into out[0..count), in slot order, and writes a
// disconnected/all-zero GamepadState{} to any slot beyond kMaxGamepads (a caller passing a larger
// count still gets an explicit answer for every element, never an untouched one).
//
// A DISCONNECTED SLOT REPORTS GamepadState{} -- connected=false, every button false, every axis 0 --
// rather than leaving out[i] as it found it. That is what lets a caller publish every slot's answer
// unconditionally, every frame, the same "always write an explicit release" discipline InputState and
// GameInput.cpp already use for keyboard: aver_fw_input_new_frame() rolls current state into previous
// without clearing it, so a slot skipped instead of zeroed reads as "still held" forever downstream.
// It is also this function's answer to framework_abi.h's own warning that a vanished controller can
// leave XInputGetState returning stale data rather than a trustworthy error for that slot: the whole
// state is zeroed BEFORE the poll, so the only way a slot reports connected is a genuine
// ERROR_SUCCESS this same call just received.
//
// RE-PROBING A DISCONNECTED SLOT IS THROTTLED to about once a second. XInputGetState on an empty slot
// walks the underlying HID bus looking for a device that is not there and is measurably slower than
// the connected-slot case -- on a typical desk with one pad plugged in, the other three slots are
// empty every frame, and polling all four without this would spend most of this function's own time
// re-proving slots 1..3 are still nothing there. The throttle applies only to a slot already known
// empty; a slot that WAS connected last call is always re-polled immediately, so an unplug is seen on
// the very next frame rather than up to a second late.
void pollGamepads(GamepadState* out, u32 count);

} // namespace aver
