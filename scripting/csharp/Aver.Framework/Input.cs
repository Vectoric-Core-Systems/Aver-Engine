using Aver.Scene;   // Vec3

namespace Aver.Framework;

/// <summary>Keyboard/mouse codes, matching the framework's AVER_FW_KEY_* enum. A..Z = 0..25, D0..D9 =
/// 26..35, then the named keys and mouse buttons.</summary>
public enum Key
{
    A = 0, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D0 = 26, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Space = 36, LeftShift, LeftCtrl, LeftAlt, Enter, Escape, Tab,
    Left, Right, Up, Down, MouseLeft, MouseRight, MouseMiddle,
}

/// <summary>
/// Polled keyboard and mouse, in the Unity idiom. The app pushes the frame's raw device state into the
/// framework each tick (only when the game — not an editor text field — has focus); gameplay reads it here.
/// State is per-frame: <see cref="GetKeyDown"/>/<see cref="GetKeyUp"/> are edges (this frame only), and the
/// mouse deltas are this frame's movement. Read it from OnTick.
/// </summary>
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

    /// <summary>WASD / arrow keys as a movement vector: X = forward (W/Up minus S/Down), Y = right
    /// (D/Right minus A/Left), Z = 0. Not normalised — a diagonal is longer, so normalise if that matters.</summary>
    public static Vec3 MoveAxis
    {
        get
        {
            float f = ((GetKey(Key.W) || GetKey(Key.Up)) ? 1f : 0f) - ((GetKey(Key.S) || GetKey(Key.Down)) ? 1f : 0f);
            float r = ((GetKey(Key.D) || GetKey(Key.Right)) ? 1f : 0f) - ((GetKey(Key.A) || GetKey(Key.Left)) ? 1f : 0f);
            return new Vec3(f, r, 0f);
        }
    }
}
