// pollGamepads over SDL 3's gamepad API, everywhere but Windows (src/win32/Gamepad.cpp is XInput).
//
// THE SAME PAD ON EVERY PLATFORM. SDL's gamepad mapping database turns any controller it knows --
// Xbox, PlayStation, Switch Pro, the long tail of generic pads -- into one Xbox-shaped layout, and that
// is the layout GamepadState already describes, so the slots below are XInput's slot for slot. Stick
// and trigger shaping is XInput's too, with the dead zones its programming guide documents, so a game
// tuned on Windows feels the same here. The two things SDL does differently are translated:
//
//   * Y axes point DOWN in SDL and UP in XInput; both sticks are negated.
//   * Triggers run 0..32767 in SDL and 0..255 in XInput; the threshold is rescaled to match.
//
// Slots are first-come and stable while a pad stays connected, like XInput's user indices.
#include "aver/platform/Gamepad.hpp"
#include "aver/core/Log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace aver {
namespace {

// XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, _RIGHT_THUMB_DEADZONE and _TRIGGER_THRESHOLD, in XInput units.
constexpr f32 kLeftStickDeadzone = 7849.0f;
constexpr f32 kRightStickDeadzone = 8689.0f;
constexpr f32 kTriggerThreshold = 30.0f * 32767.0f / 255.0f;
constexpr u64 kReprobeIntervalMs = 1000;   // Gamepad.cpp's re-probe rate for an empty slot

// Radial dead zone -- Gamepad.cpp's applyStickDeadzone, in floats.
void shapeStick(f32 x, f32 y, f32 dz, f32& outX, f32& outY) {
    f32 magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dz) { outX = 0.0f; outY = 0.0f; return; }
    const f32 nx = x / magnitude, ny = y / magnitude;
    magnitude = std::min(magnitude, 32767.0f);
    const f32 shaped = (magnitude - dz) / (32767.0f - dz);
    outX = nx * shaped;
    outY = ny * shaped;
}

f32 shapeTrigger(f32 raw) {
    if (raw <= kTriggerThreshold) return 0.0f;
    return std::min(1.0f, (raw - kTriggerThreshold) / (32767.0f - kTriggerThreshold));
}

SDL_Gamepad* g_slots[kMaxGamepads] = {};
u64 g_lastProbeMs = 0;

bool gamepadsUp() {
    static const bool up = [] {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            AVER_WARN("[Platform] SDL gamepad subsystem unavailable ({})", SDL_GetError());
            return false;
        }
        return true;
    }();
    return up;
}

// Drops slots whose pad went away and opens newly connected ones into free slots.
void refreshSlots(u64 now) {
    for (SDL_Gamepad*& g : g_slots)
        if (g && !SDL_GamepadConnected(g)) { SDL_CloseGamepad(g); g = nullptr; }
    const bool anyFree = std::find(std::begin(g_slots), std::end(g_slots), nullptr) != std::end(g_slots);
    if (!anyFree || (g_lastProbeMs != 0 && now - g_lastProbeMs < kReprobeIntervalMs)) return;
    g_lastProbeMs = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        bool open = false;
        for (SDL_Gamepad* g : g_slots) if (g && SDL_GetGamepadID(g) == ids[i]) open = true;
        if (open) continue;
        for (SDL_Gamepad*& g : g_slots) {
            if (g) continue;
            g = SDL_OpenGamepad(ids[i]);
            if (g) AVER_INFO("[Platform] gamepad connected: {}", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?");
            break;
        }
    }
    SDL_free(ids);
}

} // namespace

void pollGamepads(GamepadState* out, u32 count) {
    for (u32 i = 0; i < count; ++i) out[i] = GamepadState{};
    if (!gamepadsUp()) return;
    // State normally advances with the event pump; a host with no window has none, so ask directly.
    SDL_UpdateGamepads();
    refreshSlots(SDL_GetTicks());

    for (u32 i = 0; i < count && i < kMaxGamepads; ++i) {
        SDL_Gamepad* g = g_slots[i];
        if (!g) continue;
        GamepadState& s = out[i];
        s.connected = true;
        auto btn = [g](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
        s.buttons[0]  = btn(SDL_GAMEPAD_BUTTON_DPAD_UP);
        s.buttons[1]  = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        s.buttons[2]  = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        s.buttons[3]  = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        s.buttons[4]  = btn(SDL_GAMEPAD_BUTTON_START);
        s.buttons[5]  = btn(SDL_GAMEPAD_BUTTON_BACK);
        s.buttons[6]  = btn(SDL_GAMEPAD_BUTTON_LEFT_STICK);
        s.buttons[7]  = btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        s.buttons[8]  = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        s.buttons[9]  = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        s.buttons[10] = btn(SDL_GAMEPAD_BUTTON_SOUTH);   // A
        s.buttons[11] = btn(SDL_GAMEPAD_BUTTON_EAST);    // B
        s.buttons[12] = btn(SDL_GAMEPAD_BUTTON_WEST);    // X
        s.buttons[13] = btn(SDL_GAMEPAD_BUTTON_NORTH);   // Y
        auto axis = [g](SDL_GamepadAxis a) { return static_cast<f32>(SDL_GetGamepadAxis(g, a)); };
        shapeStick(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY), kLeftStickDeadzone, s.axes[0], s.axes[1]);
        shapeStick(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY), kRightStickDeadzone, s.axes[2], s.axes[3]);
        s.axes[4] = shapeTrigger(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        s.axes[5] = shapeTrigger(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }
}

} // namespace aver
