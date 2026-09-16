// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Polled keyboard and mouse for gameplay, plus the key code enum.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Keyboard and mouse codes. Matches the framework's AVER_FW_KEY_* enum.</summary>
public enum Key
{
    A = 0, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D0 = 26, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Space = 36, LeftShift, LeftCtrl, LeftAlt, Enter, Escape, Tab,
    Left, Right, Up, Down, MouseLeft, MouseRight, MouseMiddle,
}

/// <summary>Gamepad buttons. Matches the framework's AVER_FW_GAMEPAD_* enum (framework_abi.h), itself
/// modelled on XInput's XINPUT_GAMEPAD_* bitmask so a future real provider is a mechanical
/// bit-to-index unpack, not a redesign.</summary>
public enum GamepadButton
{
    DPadUp = 0, DPadDown, DPadLeft, DPadRight, Start, Back,
    LeftThumb, RightThumb, LeftShoulder, RightShoulder, A, B, X, Y,
}

/// <summary>Gamepad axes. Matches the framework's AVER_FW_GAMEPAD_AXIS_* enum (framework_abi.h),
/// itself modelled on XInput's XINPUT_STATE thumbstick/trigger fields.</summary>
public enum GamepadAxis
{
    LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger,
}

/// <summary>Polled keyboard and mouse. State is per-frame; read it from OnTick.</summary>
public static class Input
{
    [ThreadStatic] private static float[]? s_mouse;
    private static float[] MouseBuf => s_mouse ??= new float[3];

    /// <summary>True while <paramref name="k"/> is held.</summary>
    public static bool GetKey(Key k) => Fw.aver_fw_input_key((int)k) != 0;

    /// <summary>True on the frame <paramref name="k"/> went down.</summary>
    public static bool GetKeyDown(Key k) => Fw.aver_fw_input_key_pressed((int)k) != 0;

    /// <summary>True on the frame <paramref name="k"/> went up.</summary>
    public static bool GetKeyUp(Key k) => Fw.aver_fw_input_key_released((int)k) != 0;

    /// <summary>This frame's horizontal mouse movement, in pixels.</summary>
    public static float MouseDeltaX { get { Fw.aver_fw_input_mouse(MouseBuf); return MouseBuf[0]; } }

    /// <summary>This frame's vertical mouse movement, in pixels.</summary>
    public static float MouseDeltaY { get { Fw.aver_fw_input_mouse(MouseBuf); return MouseBuf[1]; } }

    /// <summary>This frame's mouse wheel notches.</summary>
    public static float MouseWheel { get { Fw.aver_fw_input_mouse(MouseBuf); return MouseBuf[2]; } }

    /// <summary>WASD / arrows as a movement vector: X = forward, Y = right, Z = 0. Not normalised.</summary>
    public static Vec3 MoveAxis
    {
        get
        {
            float f = ((GetKey(Key.W) || GetKey(Key.Up)) ? 1f : 0f) - ((GetKey(Key.S) || GetKey(Key.Down)) ? 1f : 0f);
            float r = ((GetKey(Key.D) || GetKey(Key.Right)) ? 1f : 0f) - ((GetKey(Key.A) || GetKey(Key.Left)) ? 1f : 0f);
            return new Vec3(f, r, 0f);
        }
    }

    // ---- RAW WIN32 VK, an additive twin to Key above -- see framework_abi.h's own RAW WIN32 VK
    // section for why this exists: `Key` has 46 slots and a saved .ocgraph's InputKey node stores one
    // of them as a literal int, so the enum can never be renumbered to grow and cover the F-keys,
    // numpad and OEM range Win32 actually has (InputKeys.hpp's own comment). A caller that wants F5
    // asks for raw vk 0x74 by value instead, through this parallel array.
    /// <summary>True while the raw Win32 VK code <paramref name="vk"/> is held.</summary>
    public static bool GetVk(int vk) => Fw.aver_fw_input_vk(vk) != 0;

    /// <summary>True on the frame the raw Win32 VK code <paramref name="vk"/> went down.</summary>
    public static bool GetVkDown(int vk) => Fw.aver_fw_input_vk_pressed(vk) != 0;

    /// <summary>True on the frame the raw Win32 VK code <paramref name="vk"/> went up.</summary>
    public static bool GetVkUp(int vk) => Fw.aver_fw_input_vk_released(vk) != 0;

    // ---- GAMEPAD -- see framework_abi.h's own GAMEPAD section: these read back whatever
    // modules/platform's poller last wrote through aver_fw_input_set_gamepad_button/axis (via
    // Runtime/src/GameInput.cpp and sandbox/src/SandboxPlay.cpp), or the all-zero/false default when
    // no pad is connected. Also now bindable onto a named action -- EnhancedInput.cs's own
    // BindGamepadButton/BindGamepadAxis -- rather than only reachable by polling here every OnTick.
    // `pad` defaults to 0 because the ABI itself rejects any other value today (only player 0 exists
    // until split-screen does, the same precedent aver_fw_player_controller already sets) -- kept as a
    // parameter rather than dropped so this signature does not have to change the day a second pad
    // becomes real.
    /// <summary>True while <paramref name="button"/> is held on gamepad <paramref name="pad"/>.</summary>
    public static bool GetGamepadButton(GamepadButton button, int pad = 0) =>
        Fw.aver_fw_input_gamepad_button(pad, (int)button) != 0;

    /// <summary>The last-set value of <paramref name="axis"/> on gamepad <paramref name="pad"/>.
    /// Unclamped, with no dead zone applied -- the ABI's own comment says why: the poller's raw
    /// stick/trigger reading is meant to cross exactly as read.</summary>
    public static float GetGamepadAxis(GamepadAxis axis, int pad = 0) =>
        Fw.aver_fw_input_gamepad_axis(pad, (int)axis);
}
