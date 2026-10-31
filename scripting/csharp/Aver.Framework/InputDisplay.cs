// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Gamepads (connection, names, rumble), touch, the pointer and cursor, and the window: what a script reaches on the machine's
// input and display beyond the polled keys. The pointer, cursor and window are REQUESTS the shipped game applies once a frame; the editor
// owns its own and ignores them (see framework_abi.h, POINTER AND CURSOR / WINDOW AND DISPLAY).

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Local gamepads: up to <see cref="MaxPads"/>, numbered from 0.</summary>
public static class Gamepads
{
    /// <summary>The most pads the engine reads. Matches AVER_FW_GAMEPAD_MAX.</summary>
    public const int MaxPads = 4;

    /// <summary>Whether pad <paramref name="pad"/> is plugged in.</summary>
    public static bool IsConnected(int pad) => Fw.aver_fw_input_gamepad_connected(pad) != 0;

    /// <summary>The pad's name, "" when it is not connected.</summary>
    public static string Name(int pad) => Fw.Str(Fw.aver_fw_input_gamepad_name(pad)) is var s && s != "?" ? s : "";

    /// <summary>Rumbles a pad: the weak (high-frequency) and strong (low-frequency) motors in 0..1, for <paramref name="seconds"/>
    /// (0 or less: until stopped). The pad's owner applies it next frame; a pad that is not connected ignores it.</summary>
    public static void Rumble(int pad, float weak, float strong, float seconds = 0f) => Fw.aver_fw_gamepad_rumble(pad, weak, strong, seconds);

    /// <summary>Stops a pad's rumble.</summary>
    public static void StopRumble(int pad) => Fw.aver_fw_gamepad_stop_rumble(pad);
}

/// <summary>Fingers on a touch screen, by index. On a machine whose window layer reports no touch, <see cref="Emulation"/> makes the left
/// mouse button finger 0.</summary>
public static class Touch
{
    /// <summary>The most fingers tracked. Matches AVER_FW_TOUCH_MAX.</summary>
    public const int MaxFingers = 10;

    private static readonly float[] s_buf = new float[3];

    /// <summary>True while finger <paramref name="index"/> is down; its position in window pixels and pressure (0 to 1).</summary>
    public static bool IsDown(int index, out float x, out float y, out float pressure)
    {
        bool down = Fw.aver_fw_input_touch(index, s_buf) != 0;
        x = s_buf[0]; y = s_buf[1]; pressure = s_buf[2];
        return down;
    }

    /// <summary>True on the frame finger <paramref name="index"/> went down.</summary>
    public static bool Pressed(int index) => Fw.aver_fw_input_touch_pressed(index) != 0;

    /// <summary>True on the frame finger <paramref name="index"/> went up.</summary>
    public static bool Released(int index) => Fw.aver_fw_input_touch_released(index) != 0;

    /// <summary>Whether the left mouse button publishes as finger 0 (when no real finger has been seen). Off by default.</summary>
    public static bool Emulation
    {
        get => Fw.aver_fw_touch_emulation() != 0;
        set => Fw.aver_fw_set_touch_emulation(value ? 1 : 0);
    }

    /// <summary>Whether the machine reported a real touch screen (a finger has arrived).</summary>
    public static bool Available => Fw.aver_fw_touch_available() != 0;
}

/// <summary>How the pointer behaves over the game window. Matches AVER_FW_POINTER_*.</summary>
public enum PointerMode
{
    /// <summary>The engine's own rule: captured while a session plays, free when a menu or <see cref="Game.FreeCursor"/> asks.</summary>
    Auto = 0,
    /// <summary>Shown, free to leave the window.</summary>
    Visible = 1,
    /// <summary>Hidden while over the window.</summary>
    Hidden = 2,
    /// <summary>Hidden and held, reporting relative motion.</summary>
    Captured = 3,
    /// <summary>Shown, kept inside the window.</summary>
    Confined = 4,
    /// <summary>Hidden and kept inside the window.</summary>
    ConfinedHidden = 5,
}

/// <summary>The cursor shapes a window offers. Matches AVER_FW_CURSOR_*.</summary>
public enum CursorShape
{
    Arrow = 0, IBeam, PointingHand, Cross, Wait, Busy, Drag, CanDrop, Forbidden, VSize, HSize, BDiagSize, FDiagSize, Move, VSplit, HSplit, Help,
}

/// <summary>The pointer over the game window: its mode, the cursor's shape and images, and moving it.</summary>
public static class Pointer
{
    /// <summary>The pointer mode. <see cref="PointerMode.Auto"/> by default.</summary>
    public static PointerMode Mode
    {
        get => (PointerMode)Fw.aver_fw_pointer_mode();
        set => Fw.aver_fw_set_pointer_mode((int)value);
    }

    /// <summary>The shape shown while the pointer is over the window.</summary>
    public static CursorShape Shape
    {
        get => (CursorShape)Fw.aver_fw_cursor_shape();
        set => Fw.aver_fw_set_cursor_shape((int)value);
    }

    /// <summary>Replaces a shape with an image: <paramref name="rgba"/> is width x height x 4 straight-alpha bytes, top row first, each side 1 to 256, the hotspot
    /// inside it. A null array puts the system shape back. False when the image was refused.</summary>
    public static bool SetCursorImage(CursorShape shape, byte[]? rgba, int width, int height, int hotX = 0, int hotY = 0)
    {
        if (rgba != null && (width < 1 || height < 1 || rgba.Length < (long)width * height * 4)) return false;
        return Fw.aver_fw_set_cursor_image((int)shape, rgba, rgba == null ? 0 : width, rgba == null ? 0 : height, hotX, hotY) != 0;
    }

    /// <summary>Moves the pointer to the window pixel (x, y) at the next frame.</summary>
    public static void Warp(float x, float y) => Fw.aver_fw_warp_pointer(x, y);
}

/// <summary>A window's mode. Matches AVER_FW_WINDOW_*. There is no exclusive fullscreen: <see cref="Fullscreen"/> is a borderless window the size
/// of the monitor.</summary>
public enum WindowMode { Windowed = 0, Minimized = 1, Maximized = 2, Fullscreen = 3 }

/// <summary>Window flags. Matches AVER_FW_WINDOW_FLAG_*.</summary>
[Flags]
public enum WindowFlags { None = 0, Resizable = 1, Borderless = 2, AlwaysOnTop = 4 }

/// <summary>The game window: what it is now (as the host last published it) and what a script asks it to become. Requests take effect at the next
/// frame in the shipped game; the editor's window is not the game's to change.</summary>
public static class DisplayWindow
{
    private const int Mode_ = 0, Width_ = 1, Height_ = 2, X_ = 3, Y_ = 4, Flags_ = 5, MonitorW_ = 6, MonitorH_ = 7, Dpi_ = 8, Focused_ = 9;
    private static readonly int[] s_state = new int[10];

    private static int[] State { get { Fw.aver_fw_window_state(s_state); return s_state; } }

    /// <summary>The mode now. <see cref="WindowMode.Windowed"/> before the host has published anything.</summary>
    public static WindowMode Mode
    {
        get => (WindowMode)State[Mode_];
        set => Fw.aver_fw_window_set_mode((int)value);
    }

    /// <summary>The client area in physical pixels (0 by 0 before the host has published).</summary>
    public static (int Width, int Height) Size => (State[Width_], State[Height_]);

    /// <summary>Asks for a client area of this many pixels (a windowed window takes it).</summary>
    public static void SetSize(int width, int height) => Fw.aver_fw_window_set_size(width, height);

    /// <summary>The outer window's top-left in screen pixels.</summary>
    public static (int X, int Y) Position => (State[X_], State[Y_]);

    public static void SetPosition(int x, int y) => Fw.aver_fw_window_set_position(x, y);

    /// <summary>The flags that are on now.</summary>
    public static WindowFlags Flags => (WindowFlags)State[Flags_];

    public static void SetFlag(WindowFlags flag, bool on) => Fw.aver_fw_window_set_flag((int)flag, on ? 1 : 0);

    public static void SetTitle(string title) => Fw.aver_fw_window_set_title(title);

    /// <summary>The monitor the window is on, in pixels (0 by 0 when it cannot be told).</summary>
    public static (int Width, int Height) MonitorSize => (State[MonitorW_], State[MonitorH_]);

    /// <summary>The display scale: 1 is 96 DPI.</summary>
    public static float DpiScale => State[Dpi_] / 100f;

    /// <summary>Whether the window has the keyboard focus.</summary>
    public static bool Focused => State[Focused_] != 0;
}
