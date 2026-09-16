// Win32 backend for Gamepad: XInput polling, hotplug-safe, with XInput's own documented dead zones.
// See Gamepad.hpp for why this exists and what it deliberately does not depend on.

#include "aver/platform/Gamepad.hpp"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>

namespace aver {
namespace {

// Radial dead zone for one stick, per Microsoft's own XInput programming guide: treat the stick as a
// circle, not two independent sliders. A per-axis (square) cutoff leaves diagonal input less shaped
// than cardinal input -- push both axes to 60% of the flat cutoff and a square dead zone lets the
// diagonal through at a magnitude the guide itself never intended, which reads as "the stick feels
// looser on the diagonals" and is exactly the inconsistency the radial shape avoids.
// rawX/rawY are XInput's signed 16-bit stick values (-32768..32767); outX/outY land in [-1,1].
// `deadzone` takes i32, not i16, purely so the XINPUT_GAMEPAD_*_THUMB_DEADZONE macros (plain `int`
// literals) pass in without a narrowing conversion at every call site.
void applyStickDeadzone(i16 rawX, i16 rawY, i32 deadzone, f32& outX, f32& outY) {
    const f32 x = static_cast<f32>(rawX);
    const f32 y = static_cast<f32>(rawY);
    const f32 dz = static_cast<f32>(deadzone);
    f32 magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dz) { outX = 0.0f; outY = 0.0f; return; }

    // Direction first, off the UNCLIPPED magnitude, then clip and rescale the length -- clipping
    // magnitude before dividing would shrink a slightly-out-of-range diagonal's direction too, not
    // just its length.
    const f32 nx = x / magnitude, ny = y / magnitude;
    magnitude = std::min(magnitude, 32767.0f);
    const f32 shaped = (magnitude - dz) / (32767.0f - dz);
    outX = nx * shaped;
    outY = ny * shaped;
}

// Flat threshold for one trigger, per the same guide's XINPUT_GAMEPAD_TRIGGER_THRESHOLD. raw is
// XInput's unsigned 8-bit trigger value (0..255); the result lands in [0,1]. `threshold` takes i32
// for the same narrowing-conversion reason as applyStickDeadzone's `deadzone` above.
f32 applyTriggerDeadzone(u8 raw, i32 threshold) {
    if (static_cast<i32>(raw) <= threshold) return 0.0f;
    return static_cast<f32>(raw - threshold) / static_cast<f32>(255 - threshold);
}

// Per-slot re-probe throttle state -- see pollGamepads' own header comment for why this exists.
// wasConnected[i] is whether slot i answered ERROR_SUCCESS on the MOST RECENT poll that actually ran
// (not merely the most recent call to this function, which may have skipped it); lastEmptyProbeMs[i]
// is when a since-confirmed-empty slot was last actually asked, 0 meaning "never probed yet".
bool g_wasConnected[kMaxGamepads] = {};
u64  g_lastEmptyProbeMs[kMaxGamepads] = {};
constexpr u64 kReprobeIntervalMs = 1000;   // about once a second, per this module's own header comment

} // namespace

void pollGamepads(GamepadState* out, u32 count) {
    const u64 now = GetTickCount64();
    for (u32 i = 0; i < count; ++i) {
        GamepadState& s = out[i];
        s = GamepadState{};   // disconnected/all-zero is the answer for every path below that does
                               // not go on to set connected = true from a genuine ERROR_SUCCESS

        if (i >= kMaxGamepads) continue;   // past XInput's own range -- nothing more to poll

        // THROTTLE: a slot already known empty, and not asked within the last second, is skipped
        // entirely rather than handed to XInputGetState -- `s` already reads disconnected/zeroed
        // above. A slot that answered connected on its last actual poll always goes through, so an
        // unplug is caught on the very next call, never delayed by this throttle.
        if (!g_wasConnected[i] && g_lastEmptyProbeMs[i] != 0 &&
            now - g_lastEmptyProbeMs[i] < kReprobeIntervalMs) {
            continue;
        }

        XINPUT_STATE state{};
        const DWORD result = XInputGetState(static_cast<DWORD>(i), &state);
        if (result != ERROR_SUCCESS) {
            g_wasConnected[i] = false;
            g_lastEmptyProbeMs[i] = now;
            continue;   // `s` stays disconnected/zeroed -- never read a field of `state` here
        }
        g_wasConnected[i] = true;
        g_lastEmptyProbeMs[i] = 0;

        s.connected = true;
        const XINPUT_GAMEPAD& pad = state.Gamepad;

        // ---- buttons, in Gamepad.hpp's own documented order (mirrors AVER_FW_GAMEPAD_*) ----
        s.buttons[0]  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP)       != 0;
        s.buttons[1]  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)     != 0;
        s.buttons[2]  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)     != 0;
        s.buttons[3]  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)    != 0;
        s.buttons[4]  = (pad.wButtons & XINPUT_GAMEPAD_START)         != 0;
        s.buttons[5]  = (pad.wButtons & XINPUT_GAMEPAD_BACK)          != 0;
        s.buttons[6]  = (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)    != 0;
        s.buttons[7]  = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)   != 0;
        s.buttons[8]  = (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        s.buttons[9]  = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        s.buttons[10] = (pad.wButtons & XINPUT_GAMEPAD_A)             != 0;
        s.buttons[11] = (pad.wButtons & XINPUT_GAMEPAD_B)             != 0;
        s.buttons[12] = (pad.wButtons & XINPUT_GAMEPAD_X)             != 0;
        s.buttons[13] = (pad.wButtons & XINPUT_GAMEPAD_Y)             != 0;

        // ---- axes, in Gamepad.hpp's own documented order (mirrors AVER_FW_GAMEPAD_AXIS_*) ----
        applyStickDeadzone(pad.sThumbLX, pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
                           s.axes[0], s.axes[1]);
        applyStickDeadzone(pad.sThumbRX, pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,
                           s.axes[2], s.axes[3]);
        s.axes[4] = applyTriggerDeadzone(pad.bLeftTrigger,  XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
        s.axes[5] = applyTriggerDeadzone(pad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    }
}

} // namespace aver
